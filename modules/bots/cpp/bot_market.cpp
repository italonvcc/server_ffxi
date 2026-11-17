/************************************************************************
 * Bot market
 *
 * A simulated auction house economy driven by a price snapshot from ffxiah.com
 * (bot_market_prices, loaded by modules/bots/market/load_snapshot.py):
 *
 * - Restock: keeps clamp(stock, kMinListings, kMaxListings) listings of each snapshot
 *   item on the auction house, at the snapshot price, sold by kMarketName (seller = 0).
 *   Players buy them through the normal AH code; seller 0 means nobody is paid.
 * - History: kHistoryRows sold rows per item at the snapshot price (seller and buyer 0),
 *   so the AH price history shows the price. Spaced by the item's sales rate; real
 *   sales are newer and push them out of the 10 the client is sent.
 * - Buyout: a player listing priced at or below the snapshot price is bought by
 *   kMarketName shortly after it is put up. Marking the row sold fires the
 *   auction_house_buy trigger, which sends the gil to the seller's delivery box,
 *   exactly like a sale to another player (and custom.AUCTION_GIL_TO_INVENTORY then
 *   moves it into their gil).
 *
 * Items missing from the snapshot (or with no price) are never bought or stocked.
 * See modules/bots/README.md, "Market".
 ************************************************************************/

#include "map/utils/moduleutils.h"

#include "common/database.h"
#include "common/earth_time.h"
#include "common/logging.h"
#include "common/settings.h"

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

// Stocked listings per item and stack size: the snapshot's stock, clamped to this range.
// Every priced item has sold at some point, so even out-of-stock ones get kMinListings.
constexpr uint32 kMinListings = 5;
constexpr uint32 kMaxListings = 10;

// Fake sales per item and stack size. The client's history shows the last 10.
constexpr uint32 kHistoryRows = 10;

// Spacing of fake sales: one per (1 / sold-per-day) days, within these bounds (seconds).
constexpr uint32 kHistoryMinGap = 3600;
constexpr uint32 kHistoryMaxGap = 7 * 86400;

// Stocked listings are dated here (2096) so the search server's EXPIRE_DAYS never sends
// them "back" to seller 0 (a delivery box row for charid 0). `date` is only used for that.
constexpr uint32 kNeverExpires = 4000000000;

// Upkeep walks the item ids in windows of this size, one window per time server tick,
// so every query only touches a few hundred items (5-15 ms) and a full cycle over all
// 65536 ids takes ~5 min. Whole-table queries took 140-480 ms on the main thread.
constexpr uint32 kWindow   = 512;
constexpr uint32 kMaxItems = 65536;

// Most rows one step writes per tick. A window that hits it is redone on the next tick
// (a fresh snapshot writes ~200k rows).
constexpr uint32 kChunk = 2000;

// A player listing must have been up this long before the market buys it, so the
// sale shows up as a sale rather than looking like the listing failed.
constexpr uint32 kBuyoutDelaySeconds = 10;

// Time server ticks are 2.4 s.
constexpr uint32 kBuyoutEveryNTicks = 2; // ~5 s

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

        ShowInfoFmt("bot_market: module loaded ({})", settings::get<bool>("custom.BOT_MARKET") ? "enabled" : "disabled by custom.BOT_MARKET");
    }

    void OnTimeServerTick() override
    {
        // settings/default/custom.lua; false = vanilla AH (nothing stocked, nothing bought)
        if (!settings::get<bool>("custom.BOT_MARKET"))
        {
            return;
        }

        ++tick_;

        // Advance to the next window unless this one hit kChunk and has more to write.
        if (!restock(cursor_, cursor_ + kWindow))
        {
            cursor_ = (cursor_ + kWindow) % kMaxItems;
        }

        if (tick_ % kBuyoutEveryNTicks == 0)
        {
            buyout();
        }
    }

private:
    uint32 tick_{ 0 };
    uint32 cursor_{ 0 }; // first item id of the next upkeep window

    // Upkeep for items lo <= itemid < hi, each step capped at kChunk rows. Returns true if
    // a step hit the cap, i.e. the window has more to do. The itemid range lets every
    // step use idx_ah_active (buyer_name, itemid, ...) or idx_ah_history (itemid, ...).
    auto restock(const uint32 lo, const uint32 hi) -> bool
    {
        const auto now  = earth_time::timestamp();
        bool       more = false;

        // db::preparedStmt checks its query at compile time, so each step calls it directly.
        const auto done = [&](const char* what, const auto& rset) -> uint64
        {
            const auto affected = rset ? rset->rowsAffected() : 0;
            more                = more || affected >= kChunk;
            if (affected > 0)
            {
                ShowInfoFmt("bot_market: {} {} rows", what, affected);
            }
            return affected;
        };

        // Stocked listings whose price no longer matches the snapshot (or whose item left it).
        {
            done("removed stale listings:",
                db::preparedStmt("DELETE FROM auction_house WHERE id IN (SELECT id FROM ("
                                 "  SELECT ah.id FROM auction_house ah "
                                 "  LEFT JOIN bot_market_prices p ON p.itemid = ah.itemid AND p.stack = ah.stack "
                                 "  WHERE ah.buyer_name IS NULL AND ah.itemid >= ? AND ah.itemid < ? AND ah.seller = 0 "
                                 "    AND (p.itemid IS NULL OR p.price = 0 OR ah.price != p.price) "
                                 "  LIMIT ?) t)",
                                 lo,
                                 hi,
                                 kChunk));

            // Fake sales (seller and buyer both 0) at an old price. Real sales are never touched:
            // a player buying stock has buyer != 0, a buyout has seller != 0.
            done("removed stale history:",
                db::preparedStmt("DELETE FROM auction_house WHERE id IN (SELECT id FROM ("
                                 "  SELECT ah.id FROM auction_house ah "
                                 "  LEFT JOIN bot_market_prices p ON p.itemid = ah.itemid AND p.stack = ah.stack "
                                 "  WHERE ah.itemid >= ? AND ah.itemid < ? AND ah.seller = 0 AND ah.buyer = 0 AND ah.buyer_name IS NOT NULL "
                                 "    AND (p.itemid IS NULL OR p.price = 0 OR ah.sale != p.price) "
                                 "  LIMIT ?) t)",
                                 lo,
                                 hi,
                                 kChunk));

            // Listings stocked before kNeverExpires existed.
            done("un-expired listings:",
                db::preparedStmt("UPDATE auction_house SET date = ? "
                                 "WHERE buyer_name IS NULL AND itemid >= ? AND itemid < ? AND seller = 0 AND date < ? LIMIT ?",
                                 kNeverExpires,
                                 lo,
                                 hi,
                                 kNeverExpires,
                                 kChunk));
        }

        // Top every item up to clamp(stock, kMinListings, kMaxListings). seq_0_to_255 is
        // MariaDB's sequence engine: one row per listing still missing.
        done("stocked listings:",
            db::preparedStmt("INSERT INTO auction_house (itemid, stack, seller, seller_name, date, price) "
                             "SELECT p.itemid, p.stack, 0, ?, ?, p.price "
                             "FROM bot_market_prices p "
                             "JOIN item_basic ib ON ib.itemid = p.itemid AND ib.aH > 0 AND (p.stack = 0 OR ib.stackSize > 1) "
                             "LEFT JOIN (SELECT itemid, stack, COUNT(*) AS n FROM auction_house "
                             "           WHERE buyer_name IS NULL AND itemid >= ? AND itemid < ? AND seller = 0 GROUP BY itemid, stack) a "
                             "       ON a.itemid = p.itemid AND a.stack = p.stack "
                             "JOIN seq_0_to_255 s ON s.seq >= COALESCE(a.n, 0) AND s.seq < GREATEST(?, LEAST(p.stock, ?)) "
                             "WHERE p.itemid >= ? AND p.itemid < ? AND p.price > 0 "
                             "LIMIT ?",
                             kMarketName,
                             kNeverExpires,
                             lo,
                             hi,
                             kMinListings,
                             kMaxListings,
                             lo,
                             hi,
                             kChunk));

        // Top every item up to kHistoryRows fake sales at the snapshot price, the n-th one
        // n gaps before now. A refilled row continues from the oldest existing one.
        {
            done("added history:",
                db::preparedStmt("INSERT INTO auction_house (itemid, stack, seller, seller_name, date, price, buyer, buyer_name, sale, sell_date) "
                                 "SELECT g.itemid, g.stack, 0, ?, g.t, g.price, 0, ?, g.price, g.t FROM ("
                                 "  SELECT p.itemid, p.stack, p.price, "
                                 "         CAST(? - (s.seq + 1) * LEAST(GREATEST(FLOOR(86400 / GREATEST(p.rate, 0.01)), ?), ?) AS UNSIGNED) AS t "
                                 "FROM bot_market_prices p "
                                 "JOIN item_basic ib ON ib.itemid = p.itemid AND ib.aH > 0 AND (p.stack = 0 OR ib.stackSize > 1) "
                                 "LEFT JOIN (SELECT itemid, stack, COUNT(*) AS n FROM auction_house "
                                 "           WHERE itemid >= ? AND itemid < ? AND seller = 0 AND buyer = 0 AND buyer_name IS NOT NULL GROUP BY itemid, stack) h "
                                 "       ON h.itemid = p.itemid AND h.stack = p.stack "
                                 "  JOIN seq_0_to_255 s ON s.seq >= COALESCE(h.n, 0) AND s.seq < ? "
                                 "  WHERE p.itemid >= ? AND p.itemid < ? AND p.price > 0 "
                                 "  LIMIT ?) g",
                                 kMarketName,
                                 kMarketName,
                                 now,
                                 kHistoryMinGap,
                                 kHistoryMaxGap,
                                 lo,
                                 hi,
                                 kHistoryRows,
                                 lo,
                                 hi,
                                 kChunk));
        }

        return more;
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

            // With custom.AUCTION_GIL_TO_INVENTORY, ah_gil_to_inventory pays and tells the seller.
            if (CCharEntity* PChar = zoneutils::GetChar(sold.seller); PChar && !settings::get<bool>("custom.AUCTION_GIL_TO_INVENTORY"))
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
