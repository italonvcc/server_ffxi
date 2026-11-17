/************************************************************************
 * AH gil to inventory (custom.AUCTION_GIL_TO_INVENTORY)
 *
 * When an auction house listing sells, the auction_house_buy trigger puts the gil in the
 * seller's delivery box (sender "AH-Jeuno"). With this mod on, every time server tick
 * those unreceived rows are taken out of the delivery box and the gil is added to the
 * seller's gil instead:
 * - online seller: through an item transaction, so the client sees the new total, plus a
 *   chat line saying what sold;
 * - offline seller: straight into char_inventory (slot 0 is gil).
 * A row whose gil would go past the gil cap is left in the delivery box.
 ************************************************************************/

#include "map/utils/moduleutils.h"

#include "common/database.h"
#include "common/logging.h"
#include "common/settings.h"

#include "map/entities/char_entity.h"
#include "map/enums/chat_message_type.h"
#include "map/items/item.h"
#include "map/items/transactions/item_claim.h"
#include "map/packets/s2c/0x017_chat_std.h"
#include "map/utils/itemutils.h"
#include "map/utils/zoneutils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{

constexpr uint32 kGilCap = 999999999;

struct Payout
{
    uint32 charid;
    uint16 slot;
    uint16 itemid;
    uint32 gil;
    uint8  quantity; // items sold (1, or the stack size)
};

} // namespace

class AhGilToInventoryModule : public CPPModule
{
    void OnInit() override
    {
        ShowInfoFmt("ah_gil_to_inventory: module loaded ({})", settings::get<bool>("custom.AUCTION_GIL_TO_INVENTORY") ? "enabled" : "disabled by custom.AUCTION_GIL_TO_INVENTORY");
    }

    void OnTimeServerTick() override
    {
        if (!settings::get<bool>("custom.AUCTION_GIL_TO_INVENTORY"))
        {
            return;
        }

        std::vector<Payout> payouts;

        // extra's first byte is the number of items sold (see the auction_house_buy trigger)
        const auto rset = db::preparedStmt("SELECT charid, slot, itemsubid, quantity, IFNULL(ORD(extra), 1) AS sold FROM delivery_box "
                                           "WHERE box = 1 AND itemid = 0xFFFF AND sender = 'AH-Jeuno' AND received = 0 "
                                           "ORDER BY charid, slot LIMIT 50");
        FOR_DB_MULTIPLE_RESULTS(rset)
        {
            payouts.push_back({
                rset->get<uint32>("charid"),
                rset->get<uint16>("slot"),
                rset->get<uint16>("itemsubid"),
                rset->get<uint32>("quantity"),
                static_cast<uint8>(rset->get<uint32>("sold")),
            });
        }

        for (const auto& payout : payouts)
        {
            if (CCharEntity* PChar = zoneutils::GetChar(payout.charid))
            {
                payOnline(PChar, payout);
            }
            else if (!isLoggedIn(payout.charid)) // logged in but not found = zoning; next tick
            {
                payOffline(payout);
            }
        }
    }

private:
    static auto isLoggedIn(const uint32 charid) -> bool
    {
        const auto rset = db::preparedStmt("SELECT 1 FROM accounts_sessions WHERE charid = ? LIMIT 1", charid);
        return !rset || rset->rowsCount() > 0; // on a DB error, act as if online and retry later
    }

    static auto takeRow(const Payout& payout) -> bool
    {
        const auto rset = db::preparedStmt("DELETE FROM delivery_box WHERE charid = ? AND box = 1 AND slot = ? "
                                           "AND itemid = 0xFFFF AND sender = 'AH-Jeuno' AND received = 0",
                                           payout.charid,
                                           payout.slot);
        return rset && rset->rowsAffected() == 1;
    }

    static void payOnline(CCharEntity* PChar, const Payout& payout)
    {
        auto transaction = ItemClaimTransaction::start(PChar);
        if (!transaction)
        {
            return; // busy (trade, other transaction); next tick
        }

        bool taken = false;

        const auto success = db::transaction(
            [&]()
            {
                if (!takeRow(payout))
                {
                    return;
                }

                taken = true;

                if (!transaction->earn(payout.gil))
                {
                    throw std::runtime_error(fmt::format("ah_gil_to_inventory: {} gil would put {} over the gil cap", payout.gil, PChar->getName()));
                }
            });

        if (!success || !taken || !transaction->commit())
        {
            return; // rolled back: the gil stays in the delivery box
        }

        const CItem* PItem = xi::items::lookup(payout.itemid);
        auto         name  = PItem ? PItem->getName() : fmt::format("item {}", payout.itemid);
        std::replace(name.begin(), name.end(), '_', ' '); // item_basic names are "fire_crystal"

        PChar->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PChar,
                                                    MESSAGE_SYSTEM_1,
                                                    fmt::format("Your {}{} sold on the auction house. {} gil added.",
                                                                payout.quantity > 1 ? fmt::format("{} ", payout.quantity) : "",
                                                                name,
                                                                payout.gil));
    }

    static void payOffline(const Payout& payout)
    {
        db::transaction(
            [&]()
            {
                if (!takeRow(payout))
                {
                    return;
                }

                const auto rset = db::preparedStmt("UPDATE char_inventory SET quantity = quantity + ? "
                                                   "WHERE charid = ? AND location = 0 AND slot = 0 AND itemId = 0xFFFF AND quantity + ? <= ?",
                                                   payout.gil,
                                                   payout.charid,
                                                   payout.gil,
                                                   kGilCap);
                if (!rset || rset->rowsAffected() != 1)
                {
                    throw std::runtime_error(fmt::format("ah_gil_to_inventory: could not add {} gil to offline charid {}", payout.gil, payout.charid));
                }
            });
    }
};

REGISTER_CPP_MODULE(AhGilToInventoryModule);
