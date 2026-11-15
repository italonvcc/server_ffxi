/************************************************************************
 * Bot market
 *
 * A simulated auction house economy driven by a price snapshot from ffxiah.com
 * (bot_market_prices, loaded by modules/bots/market/load_snapshot.py):
 *
 * - Restock: keeps up to min(stock, kMaxListings) listings of each snapshot item on
 *   the auction house, at the snapshot price, sold by kMarketName (seller = 0).
 *   Players buy them through the normal AH code; seller 0 means nobody is paid.
 * - Buyout: a player listing priced at or below the snapshot price is bought by
 *   kMarketName shortly after it is put up. Marking the row sold fires the
 *   auction_house_buy trigger, which sends the gil to the seller's delivery box,
 *   exactly like a sale to another player.
 *
 * Items missing from the snapshot (or with no price) are never bought or stocked.
 * See modules/bots/README.md, "Market".
 ************************************************************************/

#include "map/utils/moduleutils.h"

#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"

#include "map/entities/char_entity.h"
#include "map/enums/chat_message_type.h"
#include "map/items/item.h"
#include "map/packets/s2c/0x017_chat_std.h"
#include "map/utils/itemutils.h"
#include "map/utils/zoneutils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{

// Shown as the seller of stocked listings and the buyer of bought ones (AH history).
constexpr auto kMarketName = "Market";

// Cap on stocked listings per item and stack size (the snapshot's stock can be large).
constexpr uint32 kMaxListings = 10;

// A player listing must have been up this long before the market buys it, so the
// sale shows up as a sale rather than looking like the listing failed.
constexpr uint32 kBuyoutDelaySeconds = 10;

// Time server ticks are 2.4 s.
constexpr uint32 kBuyoutEveryNTicks  = 2;   // ~5 s
constexpr uint32 kRestockEveryNTicks = 125; // ~5 min
constexpr uint32 kRefreshEveryNTicks = 750; // ~30 min

struct Sold
{
    uint32      id;
    uint32      seller;
    uint16      itemid;
    bool        stack;
    uint32      price;
};

} // namespace

class BotMarketModule : public CPPModule
{
public:
    void OnInit() override
    {
        db::preparedStmt("CREATE TABLE IF NOT EXISTS `bot_market_prices` ("
                         "`itemid` SMALLINT(5) UNSIGNED NOT NULL,"
                         "`stack` TINYINT(1) UNSIGNED NOT NULL,"
                         "`price` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
                         "`lowest` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
                         "`median` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
                         "`stock` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,"
                         "`rate` FLOAT NOT NULL DEFAULT 0,"
                         "`sales` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,"
                         "`source` VARCHAR(16) NOT NULL DEFAULT '',"
                         "`updated` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
                         "PRIMARY KEY (`itemid`, `stack`)"
                         ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

        ShowInfo("bot_market: module loaded");
    }

    void OnTimeServerTick() override
    {
        ++tick_;

        if (tick_ == 1 || tick_ % kRestockEveryNTicks == 0)
        {
            restock(tick_ == 1 || tick_ % kRefreshEveryNTicks == 0);
        }

        if (tick_ % kBuyoutEveryNTicks == 0)
        {
            buyout();
        }
    }

private:
    uint32 tick_{ 0 };

    void restock(const bool refreshDates)
    {
        const auto now = earth_time::timestamp();

        // Stocked listings whose price no longer matches the snapshot (or whose item left it).
        db::preparedStmt("DELETE ah FROM auction_house ah "
                         "LEFT JOIN bot_market_prices p ON p.itemid = ah.itemid AND p.stack = ah.stack "
                         "WHERE ah.seller = 0 AND ah.buyer_name IS NULL AND (p.itemid IS NULL OR p.price = 0 OR ah.price != p.price)");

        // The search server sends listings older than EXPIRE_DAYS back to their seller; for
        // seller 0 that would be a delivery box row for charid 0. Keep them fresh instead.
        if (refreshDates)
        {
            db::preparedStmt("UPDATE auction_house SET date = ? WHERE seller = 0 AND buyer_name IS NULL", now);
        }

        // Top every item up to min(stock, kMaxListings). seq_0_to_255 is MariaDB's sequence engine.
        const auto rset = db::preparedStmt("INSERT INTO auction_house (itemid, stack, seller, seller_name, date, price) "
                                           "SELECT p.itemid, p.stack, 0, ?, ?, p.price "
                                           "FROM bot_market_prices p "
                                           "JOIN item_basic ib ON ib.itemid = p.itemid AND ib.aH > 0 AND (p.stack = 0 OR ib.stackSize > 1) "
                                           "LEFT JOIN (SELECT itemid, stack, COUNT(*) AS n FROM auction_house "
                                           "           WHERE seller = 0 AND buyer_name IS NULL GROUP BY itemid, stack) a "
                                           "       ON a.itemid = p.itemid AND a.stack = p.stack "
                                           "JOIN seq_0_to_255 s ON s.seq >= COALESCE(a.n, 0) AND s.seq < LEAST(p.stock, ?) "
                                           "WHERE p.price > 0",
                                           kMarketName,
                                           now,
                                           kMaxListings);

        if (rset && rset->rowsAffected() > 0)
        {
            ShowInfoFmt("bot_market: stocked {} auction house listings", rset->rowsAffected());
        }
    }

    void buyout()
    {
        const auto now = earth_time::timestamp();

        std::vector<Sold> candidates;

        const auto rset = db::preparedStmt("SELECT ah.id, ah.seller, ah.itemid, ah.stack, ah.price "
                                           "FROM auction_house ah "
                                           "JOIN bot_market_prices p ON p.itemid = ah.itemid AND p.stack = ah.stack "
                                           "WHERE ah.buyer_name IS NULL AND ah.seller != 0 AND ah.date <= ? "
                                           "AND p.price > 0 AND ah.price <= p.price "
                                           "LIMIT 50",
                                           now - kBuyoutDelaySeconds);

        FOR_DB_MULTIPLE_RESULTS(rset)
        {
            candidates.push_back({
                rset->get<uint32>("id"),
                rset->get<uint32>("seller"),
                rset->get<uint16>("itemid"),
                rset->get<uint8>("stack") != 0,
                rset->get<uint32>("price"),
            });
        }

        for (const auto& sold : candidates)
        {
            // buyer_name IS NULL guards against the seller cancelling or another player
            // buying it between the SELECT and here. The trigger pays the seller.
            const auto upd = db::preparedStmt("UPDATE auction_house SET buyer = 0, buyer_name = ?, sale = price, sell_date = ? "
                                              "WHERE id = ? AND buyer_name IS NULL",
                                              kMarketName,
                                              now,
                                              sold.id);

            if (!upd || upd->rowsAffected() == 0)
            {
                continue;
            }

            const CItem* PItem = xi::items::lookup(sold.itemid);
            auto         name  = PItem ? PItem->getName() : fmt::format("item {}", sold.itemid);
            std::replace(name.begin(), name.end(), '_', ' '); // item_basic names are "fire_crystal"

            ShowInfoFmt("bot_market: bought {}{} from charid {} for {} gil", name, sold.stack ? " (stack)" : "", sold.seller, sold.price);

            if (CCharEntity* PChar = zoneutils::GetChar(sold.seller))
            {
                PChar->pushPacket<GP_SERV_COMMAND_CHAT_STD>(PChar,
                                                            MESSAGE_SYSTEM_1,
                                                            fmt::format("Your {}{} sold on the auction house for {} gil. The gil is in your delivery box.",
                                                                        name,
                                                                        sold.stack ? " (stack)" : "",
                                                                        sold.price));
            }
        }
    }
};

REGISTER_CPP_MODULE(BotMarketModule);
