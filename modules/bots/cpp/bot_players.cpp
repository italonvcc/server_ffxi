//
// Bot Players
//
//   Server-controlled characters that the client sees as real players (they are sent with
//   the 0x00D char packet, like any other PC).
//
//   Each bot is a real character row in the database, owned by its own bot account
//   (login "bot<charid>", which cannot log in), and registered in `bot_players`. While a bot
//   is spawned it has an `accounts_sessions` row, so /sea lists it like a logged-in player.
//   A bot is loaded with the normal charutils::LoadChar() path and inserted into a zone's
//   character list, but it has no MapSession (PSession == nullptr). Anything queued for it
//   to "send to its client" is discarded every zone tick.
//
//   Control:
//     - `bot_commands` is a queue. Anything (the web dashboard in modules/bots/dashboard,
//       or a DB client) inserts a row, and this module runs it on the next time server tick
//       (every 2.4s) and writes back status/result.
//     - Bots with `bot_players.autospawn = 1` are spawned at their saved position on the
//       first time server tick after boot.
//     - Live state (online, zone, position, HP/MP) is written back to `bot_players`, and
//       a heartbeat to `server_variables.BotsHeartbeat`.
//
//   Lua API (bound in OnInit, used by the !bot command):
//     xi.bots.spawn(name)               -> string
//     xi.bots.despawn(name)             -> string
//     xi.bots.summon(name, player)      -> string   Moves the bot next to `player`
//     xi.bots.list()                    -> string
//
//   AI (runs in OnZoneTick, once per 400ms logic tick, only for the bots in that zone):
//     - A pending party invite is accepted after a short delay.
//     - In a party led by a real player, the bot follows the leader (navmesh pathing, the
//       same CPathFind mobs and trusts use), including into other zones.
//     - With no party (never invited, kicked, disbanded, or the leader left or logged off)
//       the bot walks back to its home: the place it was last spawned or summoned to.
//     - It fights what its leader fights, cures party members that are low on HP if it
//       knows a Cure spell, and sits down to rest when nothing is going on.
//     - A trade request from a player is accepted, and whatever the player offers is taken.
//     - It passes on every item in the treasure pool, and learns the scrolls it carries.
//     - Knocked out, it takes a Raise, or gets up by itself after kSelfRaiseDelay.
//
//   Performance rules for anything added to the tick:
//     - No database access. Only invite/leave/zone-change events touch the DB.
//     - An idle bot costs one packet-queue clear and a few pointer checks.
//     - Navmesh queries are throttled per bot (kRepathInterval) and only made when the
//       goal has actually moved.
//     - Inventory scans (scrolls, gear) only run when flagged by an event: spawn, a
//       finished trade, a level up.
//
//   NOTE: This is not retail behaviour, and a lot of core code assumes every CCharEntity
//   has a session. Sending a bot to another zone the normal way (Warp, Teleport, the
//   homepoint after a KO) needs one, so CZoneEntities::charTick skips session-less chars
//   and think() cancels the request. Sending a bot a /tell is untested.
//

#include "common/database.h"
#include "common/settings.h"
#include "common/timer.h"
#include "common/utils.h"
#include "common/xirand.h"

#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/ai/states/item_state.h"
#include "map/ai/states/magic_state.h"
#include "map/entities/mob_entity.h"
#include "map/entities/char_entity.h"
#include "map/enums/msg_std.h"
#include "map/enums/party_kind.h"
#include "map/item_container.h"
#include "map/items/item_equipment.h"
#include "map/items/item_usable.h"
#include "map/items/transactions/player_trade.h"
#include "map/lua/lua_base_entity.h"
#include "map/lua/luautils.h"
#include "map/navmesh/navmesh.h"
#include "map/packets/s2c/0x009_message.h"
#include "map/packets/s2c/0x022_item_trade_res.h"
#include "map/party.h"
#include "map/spell.h"
#include "map/status_effect_container.h"
#include "map/treasure_pool.h"
#include "map/utils/charutils.h"
#include "map/utils/moduleutils.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{


// xi.title.NEW_ADVENTURER: xi.player.charCreate grants it, and the core uses it to decide
// whether a character still needs its first-login setup (see luautils::OnGameIn).
constexpr uint16 kTitleNewAdventurer = 206;

constexpr uint8 kMaxLevel = 99;

// Write live status back to bot_players every Nth time server tick (2.4s * 2 = 4.8s).
constexpr uint32 kStatusEveryNTicks = 2;

//
// AI tuning
//

// A bot waits this long before accepting a party invite, like a player reading the prompt.
constexpr auto kInviteDelay = std::chrono::milliseconds(1200);

// The leader's entity is destroyed and rebuilt when it changes zone, so for a moment the
// party has no leader. Past this, the leader is gone for good and the bot leaves.
constexpr auto kLeaderGrace = std::chrono::seconds(120);

// Following: the first bot stops this close to the leader, each further bot in the party
// a bit further out, and a standing bot only starts moving again once the leader is
// kFollowSlack beyond that (so small leader movements don't make bots shuffle).
constexpr float kFollowStop  = 2.0f;
constexpr float kSlotSpacing = 0.8f;
constexpr float kFollowSlack = 2.0f;

// Further than this from the leader (teleport, fall, stuck): warp instead of walking.
constexpr float kWarpDistance = 45.0f;

// At most one navmesh query per bot per interval, and only once the leader has moved this
// far from where the current path ends.
constexpr auto  kRepathInterval = std::chrono::milliseconds(800);
constexpr float kRepathDrift    = 1.5f;

// Failed path queries in a row before giving up and warping.
constexpr uint8 kMaxPathFails = 3;

// Walking home: retry interval when no path is found, how close counts as home, how close
// a finished path may end to still snap onto the exact spot, and when to just teleport.
constexpr auto  kReturnRepathInterval = std::chrono::seconds(2);
constexpr float kHomeRadius           = 0.5f;
constexpr float kHomeSnapRadius       = 3.0f;
constexpr auto  kReturnTimeout        = std::chrono::seconds(180);

// CPathFind moves an entity (speed / 17) yalms per second, which for a PC's speed of 50 is
// 2.9 yalms/s: slower than a player runs (mobs and trusts get a 2.5x "run" multiplier that
// PCs don't). Two steps per tick is 5.9 yalms/s, the same as a trust, enough to keep up.
constexpr int kStepsPerTick = 2;

// A bot answers a trade request after this long, and confirms its side a moment later.
constexpr auto kTradeDelay   = std::chrono::milliseconds(1200);
constexpr auto kTradeConfirm = std::chrono::milliseconds(1500);

// How often a bot looks at its treasure pool.
constexpr auto kLootInterval = std::chrono::seconds(1);

// Inventory scans (scrolls to learn, gear to put on) are at least this far apart.
constexpr auto kItemCheckInterval = std::chrono::seconds(4);

// Fighting: a bot helps with the leader's target when it is this close, walks up to
// kMeleeInside yalms inside its melee range and only moves again once it is out of range.
constexpr float kAssistRange = 25.0f;
constexpr float kMeleeInside = 1.2f;
constexpr float kMeleeMargin = 0.3f;
constexpr float kTargetDrift = 1.0f;
constexpr auto  kEngageRetry = std::chrono::seconds(1);

// Healing: party members under this HP% within kCureRange get a Cure. Looked at every
// kCureCheckInterval; after a cast (or a refused one) the bot waits kCastRetry.
constexpr uint8 kCureBelowHpp      = 65;
constexpr float kCureRange         = 18.0f;
constexpr auto  kCureCheckInterval = std::chrono::milliseconds(800);
constexpr auto  kCastRetry         = std::chrono::seconds(3);

// What each Cure tier (spell ids 1-6) roughly restores, to not spend a Cure IV on a scratch.
constexpr std::array<int32, 6> kCurePotency{ 30, 90, 190, 400, 700, 1000 };

// Standing next to the leader this long with HP or MP missing: sit down and rest.
constexpr auto kSettleDelay = std::chrono::seconds(2);
constexpr auto kRestDelay   = std::chrono::seconds(4);

// Knocked out: a Raise is accepted after kRaiseAcceptDelay; with none, the bot gets up by
// itself (as if it had Raise cast on it) after kSelfRaiseDelay.
constexpr auto kRaiseAcceptDelay = std::chrono::seconds(4);
constexpr auto kSelfRaiseDelay   = std::chrono::seconds(60);
constexpr auto kRaiseAnimation   = std::chrono::seconds(4);

// Where to place a bot.
struct Placement
{
    xi::ZoneId zone;
    position_t pos;
};

enum class AiState : uint8
{
    Idle,   // No party, at home. Costs nothing.
    Follow, // In a party, following the leader
    Wait,   // In a party, but the leader is changing zone or somewhere bots don't go
    Return, // No party, on the way home
    Fight,  // Engaged with the leader's target
    Rest,   // Sitting next to the leader to recover HP/MP
    Dead,   // Knocked out, waiting for a Raise
};

auto aiStateName(const AiState state) -> const char*
{
    switch (state)
    {
        case AiState::Follow:
            return "follow";
        case AiState::Wait:
            return "wait";
        case AiState::Return:
            return "return";
        case AiState::Fight:
            return "fight";
        case AiState::Rest:
            return "rest";
        case AiState::Dead:
            return "dead";
        default:
            return "idle";
    }
}

// What was last written to bot_players, to skip writes when nothing changed.
struct StatusSnapshot
{
    uint16  zone{};
    float   x{}, y{}, z{};
    int32   hp{}, maxHp{}, mp{}, maxMp{};
    uint8   level{};
    uint16  exp{};
    AiState state{};
    uint32  leaderId{};
    bool    online{};

    auto operator==(const StatusSnapshot&) const -> bool = default;
};

// A spawned bot
struct Bot
{
    std::unique_ptr<CCharEntity> PChar;

    // Where the bot stands when it has nothing to do
    Placement home{};

    AiState state = AiState::Idle;

    bool              inviteSeen = false;
    timer::time_point inviteSeenAt{};

    bool              leaderLost = false;
    timer::time_point leaderLostAt{};

    timer::time_point nextRepath{};
    position_t        pathGoal{};
    uint8             pathFails = 0;

    timer::time_point returnDeadline{};

    // Trade
    bool              tradeSeen = false;
    timer::time_point tradeSeenAt{};
    bool              trading        = false;
    bool              tradeConfirmed = false;
    timer::time_point tradeStartedAt{};

    timer::time_point nextLootCheck{};

    // Set by events that can change what is in the inventory or what the bot can use
    bool              checkItems = true;
    timer::time_point nextItemCheck{};
    uint8             level = 0;

    // Combat
    timer::time_point nextEngage{};
    timer::time_point nextCureCheck{};
    timer::time_point nextCast{};

    // Standing next to the leader with nothing to do
    bool              still = false;
    timer::time_point stillSince{};

    bool              dead = false;
    timer::time_point deadAt{};

    // think() does nothing until then (getting up after a Raise)
    timer::time_point busyUntil{};

    // Testing aid (`puppet` command): no AI, and other bots treat it as a player, so it can
    // stand in for a party leader when no client is connected.
    bool puppet = false;

    std::optional<StatusSnapshot> lastStatus;
};

// Home nation per race (char_look.race 1-8), as in scripts/globals/player.lua.
// 0 San d'Oria, 1 Bastok, 2 Windurst.
auto homeNationForRace(const uint8 race) -> uint8
{
    switch (race)
    {
        case 3: // Elvaan M
        case 4: // Elvaan F
            return 0;
        case 5: // Taru M
        case 6: // Taru F
        case 7: // Mithra
            return 2;
        default: // Hume M/F, Galka
            return 1;
    }
}

// Same rules the client enforces: 3-15 letters, first upper case, rest lower case.
auto isValidName(const std::string& name) -> bool
{
    if (name.size() < 3 || name.size() > 15)
    {
        return false;
    }

    if (!std::isupper(static_cast<unsigned char>(name[0])))
    {
        return false;
    }

    for (size_t i = 1; i < name.size(); ++i)
    {
        if (!std::islower(static_cast<unsigned char>(name[i])))
        {
            return false;
        }
    }

    return true;
}

void ensureTables()
{
    // Keep in sync with modules/bots/sql/bot_players.sql
    db::preparedStmt("CREATE TABLE IF NOT EXISTS `bot_players` ("
                     "`charid` INT(10) UNSIGNED NOT NULL,"
                     "`autospawn` TINYINT(1) UNSIGNED NOT NULL DEFAULT 1,"
                     "`online` TINYINT(1) UNSIGNED NOT NULL DEFAULT 0,"
                     "`zone` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,"
                     "`pos_x` FLOAT NOT NULL DEFAULT 0,"
                     "`pos_y` FLOAT NOT NULL DEFAULT 0,"
                     "`pos_z` FLOAT NOT NULL DEFAULT 0,"
                     "`hp` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
                     "`max_hp` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
                     "`mp` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
                     "`max_mp` INT(10) UNSIGNED NOT NULL DEFAULT 0,"
                     "`status_updated` DATETIME NULL DEFAULT NULL,"
                     "`created` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                     "PRIMARY KEY (`charid`)"
                     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // Added with the AI. home_zone = 0 means "not set yet" (bots created before this).
    db::preparedStmt("ALTER TABLE `bot_players` "
                     "ADD COLUMN IF NOT EXISTS `home_zone` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,"
                     "ADD COLUMN IF NOT EXISTS `home_x` FLOAT NOT NULL DEFAULT 0,"
                     "ADD COLUMN IF NOT EXISTS `home_y` FLOAT NOT NULL DEFAULT 0,"
                     "ADD COLUMN IF NOT EXISTS `home_z` FLOAT NOT NULL DEFAULT 0,"
                     "ADD COLUMN IF NOT EXISTS `home_rot` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0,"
                     "ADD COLUMN IF NOT EXISTS `ai_state` VARCHAR(8) NOT NULL DEFAULT 'idle',"
                     "ADD COLUMN IF NOT EXISTS `leader` VARCHAR(15) NOT NULL DEFAULT ''");

    // Added with combat: main job level, its exp and the exp the next level needs (live columns)
    db::preparedStmt("ALTER TABLE `bot_players` "
                     "ADD COLUMN IF NOT EXISTS `level` TINYINT(3) UNSIGNED NOT NULL DEFAULT 0,"
                     "ADD COLUMN IF NOT EXISTS `exp` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0,"
                     "ADD COLUMN IF NOT EXISTS `exp_next` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0");

    db::preparedStmt("CREATE TABLE IF NOT EXISTS `bot_commands` ("
                     "`id` INT(10) UNSIGNED NOT NULL AUTO_INCREMENT,"
                     "`command` VARCHAR(16) NOT NULL,"
                     "`charid` INT(10) UNSIGNED NULL DEFAULT NULL,"
                     "`name` VARCHAR(15) NULL DEFAULT NULL,"
                     "`race` TINYINT(3) UNSIGNED NULL DEFAULT NULL,"
                     "`face` TINYINT(3) UNSIGNED NULL DEFAULT NULL,"
                     "`size` TINYINT(3) UNSIGNED NULL DEFAULT NULL,"
                     "`job` TINYINT(3) UNSIGNED NULL DEFAULT NULL,"
                     "`level` TINYINT(3) UNSIGNED NULL DEFAULT NULL,"
                     "`nation` TINYINT(3) UNSIGNED NULL DEFAULT NULL,"
                     "`autospawn` TINYINT(1) UNSIGNED NULL DEFAULT NULL,"
                     "`anchor_charid` INT(10) UNSIGNED NULL DEFAULT NULL,"
                     "`zone` SMALLINT(5) UNSIGNED NULL DEFAULT NULL,"
                     "`pos_x` FLOAT NULL DEFAULT NULL,"
                     "`pos_y` FLOAT NULL DEFAULT NULL,"
                     "`pos_z` FLOAT NULL DEFAULT NULL,"
                     "`status` VARCHAR(8) NOT NULL DEFAULT 'pending',"
                     "`result` VARCHAR(255) NULL DEFAULT NULL,"
                     "`created` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
                     "`processed` DATETIME NULL DEFAULT NULL,"
                     "PRIMARY KEY (`id`),"
                     "KEY `status` (`status`)"
                     ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
}

// Each bot gets its own account: accounts_sessions allows one online character per account,
// and bots need a session row to show up in /sea.
auto getOrCreateBotAccount(const std::string& login) -> uint32
{
    {
        const auto rset = db::preparedStmt("SELECT id FROM accounts WHERE login = ? LIMIT 1", login);
        if (rset && rset->rowsCount() && rset->next())
        {
            return rset->get<uint32>("id");
        }
    }

    uint32 accid = 1000;
    {
        const auto rset = db::preparedStmt("SELECT COALESCE(MAX(id), 999) + 1 AS next_id FROM accounts");
        if (rset && rset->rowsCount() && rset->next())
        {
            accid = rset->get<uint32>("next_id");
        }
    }

    // The password is not a valid hash, so nobody can log in to this account.
    if (!db::preparedStmt("INSERT INTO accounts(id, login, password, current_email, registration_email, timecreate, status, priv) "
                          "VALUES(?, ?, '!', '', '', NOW(), 1, 1)",
                          accid,
                          login))
    {
        ShowError("bot_players: failed to create bot account");
        return 0;
    }

    ShowInfoFmt("bot_players: created bot account '{}' ({})", login, accid);
    return accid;
}

auto findCharIdByName(const std::string& name) -> std::optional<uint32>
{
    const auto rset = db::preparedStmt("SELECT charid FROM chars WHERE charname = ? LIMIT 1", name);
    if (rset && rset->rowsCount() && rset->next())
    {
        return rset->get<uint32>("charid");
    }

    return std::nullopt;
}

auto isBot(const uint32 charid) -> bool
{
    const auto rset = db::preparedStmt("SELECT charid FROM bot_players WHERE charid = ? LIMIT 1", charid);
    return rset && rset->rowsCount();
}

// Places a bot about 2 yalms east of `PAnchor`, facing them.
auto placementNear(const CCharEntity* PAnchor) -> Placement
{
    Placement placement{ PAnchor->loc.zone->GetID(), PAnchor->loc.p };
    placement.pos.x += 2.0f;
    placement.pos.moving   = 0;
    placement.pos.rotation = worldAngle(placement.pos, PAnchor->loc.p);
    return placement;
}

// Equips anything in the inventory that fits an empty slot (starting weapon, nation ring, ...).
// EquipItem() does the level/job/race checks and quietly refuses what doesn't fit.
void autoEquipEmptySlots(CCharEntity* PBot)
{
    auto* PInventory = PBot->getStorage(LOC_INVENTORY);

    for (uint8 invSlot = 1; invSlot <= PInventory->GetSize(); ++invSlot)
    {
        auto* PItem = dynamic_cast<CItemEquipment*>(PInventory->GetItem(invSlot));
        if (PItem == nullptr || PItem->getReqLvl() > PBot->GetMLevel())
        {
            continue;
        }

        bool alreadyEquipped = false;
        for (uint8 slot = 0; slot < MAX_SLOTTYPE; ++slot)
        {
            if (PBot->getEquip(static_cast<SLOTTYPE>(slot)) == PItem)
            {
                alreadyEquipped = true;
                break;
            }
        }

        if (alreadyEquipped)
        {
            continue;
        }

        const auto slotMask = PItem->getEquipSlotId();
        for (uint8 slot = 0; slot < MAX_SLOTTYPE; ++slot)
        {
            if ((slotMask & (1 << slot)) && PBot->getEquip(static_cast<SLOTTYPE>(slot)) == nullptr)
            {
                charutils::EquipItem(PBot, invSlot, slot, LOC_INVENTORY);
                break;
            }
        }
    }
}

struct Command
{
    uint32                   id{};
    std::string              command;
    uint32                   charid{};
    std::string              name;
    uint8                    race{}, face{}, size{}, job{}, level{}, nation{}, autospawn{};
    uint32                   anchorCharId{};
    std::optional<Placement> placement;
};

void finishCommand(const uint32 id, const bool ok, const std::string& result)
{
    db::preparedStmt("UPDATE bot_commands SET status = ?, result = ?, processed = NOW() WHERE id = ?",
                     ok ? "done" : "failed",
                     result.substr(0, 255),
                     id);
}

// The party leader, if the bot is in a party and the leader is on this map server right now.
auto partyLeaderOf(const CCharEntity* PChar) -> CCharEntity*
{
    if (PChar->PParty == nullptr)
    {
        return nullptr;
    }

    auto* PLeader = PChar->PParty->GetLeader();
    return PLeader != nullptr && PLeader->objtype == TYPE_PC ? static_cast<CCharEntity*>(PLeader) : nullptr;
}

// Writes the live columns of bot_players, unless nothing changed since the last write.
void writeStatusFor(Bot& bot, const bool online)
{
    const auto& entity  = *bot.PChar;
    const auto* PLeader = online ? partyLeaderOf(&entity) : nullptr;

    const StatusSnapshot snapshot{
        .zone     = static_cast<uint16>(entity.loc.zone ? entity.loc.zone->GetID() : entity.loc.destination),
        .x        = entity.loc.p.x,
        .y        = entity.loc.p.y,
        .z        = entity.loc.p.z,
        .hp       = entity.health.hp,
        .maxHp    = entity.GetMaxHP(),
        .mp       = entity.health.mp,
        .maxMp    = entity.GetMaxMP(),
        .level    = entity.GetMLevel(),
        .exp      = entity.jobs.exp[static_cast<uint8>(entity.GetMJob())],
        .state    = online ? bot.state : AiState::Idle,
        .leaderId = PLeader ? PLeader->id : 0,
        .online   = online,
    };

    if (bot.lastStatus && *bot.lastStatus == snapshot)
    {
        return;
    }

    bot.lastStatus = snapshot;

    db::preparedStmt("UPDATE bot_players SET online = ?, zone = ?, pos_x = ?, pos_y = ?, pos_z = ?, "
                     "hp = ?, max_hp = ?, mp = ?, max_mp = ?, level = ?, exp = ?, exp_next = ?, "
                     "ai_state = ?, leader = ?, status_updated = NOW() WHERE charid = ?",
                     online ? 1 : 0,
                     snapshot.zone,
                     snapshot.x,
                     snapshot.y,
                     snapshot.z,
                     snapshot.hp,
                     snapshot.maxHp,
                     snapshot.mp,
                     snapshot.maxMp,
                     snapshot.level,
                     snapshot.exp,
                     charutils::GetExpNEXTLevel(snapshot.level),
                     aiStateName(snapshot.state),
                     PLeader ? PLeader->getName() : std::string(),
                     entity.id);
}

auto loadHome(const uint32 charid) -> std::optional<Placement>
{
    const auto rset = db::preparedStmt("SELECT home_zone, home_x, home_y, home_z, home_rot FROM bot_players WHERE charid = ? AND home_zone <> 0 LIMIT 1", charid);
    if (rset && rset->rowsCount() && rset->next())
    {
        position_t pos{};
        pos.x        = rset->get<float>("home_x");
        pos.y        = rset->get<float>("home_y");
        pos.z        = rset->get<float>("home_z");
        pos.rotation = rset->get<uint8>("home_rot");
        return Placement{ static_cast<xi::ZoneId>(rset->get<uint16>("home_zone")), pos };
    }

    return std::nullopt;
}

void saveHome(const uint32 charid, const Placement& home)
{
    db::preparedStmt("UPDATE bot_players SET home_zone = ?, home_x = ?, home_y = ?, home_z = ?, home_rot = ? WHERE charid = ?",
                     static_cast<uint16>(home.zone),
                     home.pos.x,
                     home.pos.y,
                     home.pos.z,
                     home.pos.rotation,
                     charid);
}

// Where a bot arrives when it follows `PLeader` into another zone: just behind them, or on
// top of them when that spot is not walkable (zone lines tend to have a wall behind).
auto placementBehind(const CCharEntity* PLeader) -> Placement
{
    Placement placement{ PLeader->loc.zone->GetID(), nearPosition(PLeader->loc.p, 1.5f, static_cast<float>(M_PI)) };
    if (!PLeader->loc.zone->navMesh()->validPosition(placement.pos))
    {
        placement.pos = PLeader->loc.p;
    }

    placement.pos.moving   = 0;
    placement.pos.rotation = PLeader->loc.p.rotation;
    return placement;
}

} // namespace

class BotPlayersModule : public CPPModule
{
public:
    void OnInit() override
    {
        ensureTables();

        // Nothing is spawned yet, whatever the table says from the last run
        db::preparedStmt("UPDATE bot_players SET online = 0");

        // NOTE: Use the global ::lua, not CPPModule::lua. The member is bound during static
        //     : initialization, possibly before the global state exists, and crashes on use.
        sol::table xi = ::lua["xi"].get_or_create<sol::table>();

        sol::table bots = ::lua.create_table();

        bots["spawn"] = [this](const std::string& name) -> std::string
        {
            const auto charid = findCharIdByName(name);
            return charid ? spawn(*charid, std::nullopt) : fmt::format("No character named {}.", name);
        };

        bots["despawn"] = [this](const std::string& name) -> std::string
        {
            const auto charid = findCharIdByName(name);
            return charid ? despawn(*charid) : fmt::format("No character named {}.", name);
        };

        bots["summon"] = [this](const std::string& name, CLuaBaseEntity* PLuaPlayer) -> std::string
        {
            const auto charid  = findCharIdByName(name);
            auto*      PPlayer = PLuaPlayer ? dynamic_cast<CCharEntity*>(PLuaPlayer->GetBaseEntity()) : nullptr;
            if (!charid)
            {
                return fmt::format("No character named {}.", name);
            }

            if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
            {
                return "You need to be in a zone.";
            }

            return summon(*charid, placementNear(PPlayer));
        };

        bots["list"] = [this]() -> std::string
        {
            return list();
        };

        xi["bots"] = bots;

        ShowInfo("bot_players: module loaded");
    }

    void OnTimeServerTick() override
    {
        if (!bootSpawnDone_)
        {
            bootSpawnDone_ = true;
            spawnAutospawnBots();
        }

        processCommands();

        if (++tickCount_ % kStatusEveryNTicks == 0)
        {
            writeStatus();
        }
    }

    void OnZoneTick(CZone* PZone) override
    {
        // Free bots removed on a previous tick, once nothing is mid-iteration over them.
        pendingDelete_.clear();

        // A deleted bot's entity writes its char_history row once more when it is freed (above).
        // Left behind, that row makes the char_insert trigger fail for the next character that
        // gets the charid, bot or player.
        if (!purgeHistory_.empty())
        {
            for (const auto charid : purgeHistory_)
            {
                db::preparedStmt("DELETE FROM char_history WHERE charid = ? AND charid NOT IN (SELECT charid FROM chars)", charid);
            }

            purgeHistory_.clear();
        }

        const auto it = byZone_.find(PZone);
        if (it == byZone_.end() || it->second.empty())
        {
            return;
        }

        const auto now = timer::now();

        for (Bot* bot : it->second)
        {
            // Nobody is listening on the other end of these.
            bot->PChar->clearPacketList();

            think(*bot, now);
        }

        // think() only queues zone changes: they edit the list iterated above.
        if (!zoneMoves_.empty())
        {
            const auto moves = std::move(zoneMoves_);
            zoneMoves_.clear();

            for (const auto& [charid, placement] : moves)
            {
                if (const auto found = live_.find(charid); found != live_.end())
                {
                    moveToZone(found->second, placement);
                }
            }
        }
    }

private:
    //
    // AI
    //

    void think(Bot& bot, const timer::time_point now)
    {
        auto* PBot = bot.PChar.get();

        // Something asked for a zone change (Warp, Teleport, setPos, the homepoint after a KO).
        // That needs a session; CZoneEntities::charTick leaves session-less chars alone.
        if (PBot->requestedZoneChange || PBot->requestedWarp != WarpRequest::None)
        {
            cancelZoneRequest(bot);
        }

        if (bot.puppet)
        {
            // No AI, but an engaged puppet walks up to and faces its target (`engage` command)
            if (PBot->isAlive() && PBot->PAI->IsEngaged())
            {
                if (auto* PTarget = PBot->GetBattleTarget())
                {
                    melee(bot, PTarget, now);
                }
            }

            return;
        }

        if (now < bot.busyUntil)
        {
            return;
        }

        if (PBot->isDead())
        {
            handleDeath(bot, now);
            return;
        }

        bot.dead = false;

        if (PBot->InvitePending.entity.UniqueNo != 0)
        {
            handleInvite(bot, now);
        }

        if (bot.trading || PBot->TradePending.entity.isSet())
        {
            handleTrade(bot, now);
        }

        if (now >= bot.nextLootCheck)
        {
            bot.nextLootCheck = now + kLootInterval;
            handleLoot(bot);
        }

        if (bot.level != PBot->GetMLevel())
        {
            // New spells and gear may be usable now
            bot.level      = PBot->GetMLevel();
            bot.checkItems = true;
        }

        // Casting or using an item: moving would interrupt it
        if (PBot->PAI->IsCurrentState<CMagicState>() || PBot->PAI->IsCurrentState<CItemState>())
        {
            return;
        }

        if (PBot->PParty != nullptr)
        {
            auto* PLeader = partyLeaderOf(PBot);

            if (PLeader != nullptr && PLeader != PBot && !isAiBot(PLeader->id))
            {
                bot.leaderLost = false;

                if (!assist(bot, PLeader, now))
                {
                    follow(bot, PLeader, now);
                }

                return;
            }

            if (PLeader == nullptr)
            {
                // The leader is changing zone (its entity is rebuilt on the other side), or gone.
                if (!bot.leaderLost)
                {
                    bot.leaderLost   = true;
                    bot.leaderLostAt = now;
                    bot.state        = AiState::Wait;
                    calm(bot);
                    stop(bot);
                }

                if (now - bot.leaderLostAt < kLeaderGrace)
                {
                    return;
                }
            }

            // No player is leading: the party lead was handed to a bot when the leader left or
            // logged off, or the leader never came back.
            ShowInfoFmt("bot_players: {} leaves the party (no player leading it)", PBot->getName());

            bot.leaderLost = false;
            charutils::LeaveParty(PBot);

            if (PBot->PParty != nullptr)
            {
                return;
            }
        }

        if (bot.state != AiState::Idle)
        {
            returnHome(bot, now);
        }
        else if (bot.checkItems && now >= bot.nextItemCheck)
        {
            useItems(bot, now);
        }
    }

    // Bots can't be sent to another zone by the core. Undo what CLuaBaseEntity::setPos() and
    // friends set up; a bot in a party catches up with its leader through follow().
    void cancelZoneRequest(Bot& bot)
    {
        auto* PBot = bot.PChar.get();

        PBot->requestedWarp = WarpRequest::None;

        if (PBot->requestedZoneChange)
        {
            PBot->requestedZoneChange = false;
            PBot->loc.destination     = PBot->loc.zone->GetID();
            PBot->status              = xi::Status::Normal;
            PBot->updatemask |= UPDATE_ALL_CHAR;
        }
    }

    //
    // Knocked out
    //

    void handleDeath(Bot& bot, const timer::time_point now)
    {
        auto* PBot = bot.PChar.get();

        if (!bot.dead)
        {
            bot.dead   = true;
            bot.deadAt = now;
            bot.state  = AiState::Dead;
            bot.still  = false;
            PBot->PAI->PathFind->Clear();

            ShowInfoFmt("bot_players: {} was knocked out", PBot->getName());
            return;
        }

        if (now - bot.deadAt < kRaiseAcceptDelay)
        {
            return;
        }

        if (PBot->m_hasRaise == 0)
        {
            if (now - bot.deadAt < kSelfRaiseDelay)
            {
                return;
            }

            // Nobody raised it. The homepoint needs a session, so get up here instead.
            PBot->m_hasRaise = 1;
        }

        // Same as answering "yes" to the raise prompt (GP_CLI_COMMAND_ACTION, RaiseMenu)
        PBot->Raise();
        bot.busyUntil = now + kRaiseAnimation;

        ShowInfoFmt("bot_players: {} was raised", PBot->getName());
    }

    //
    // Trade
    //

    // A player's trade request is accepted like a client answering "yes"
    // (GP_CLI_COMMAND_TRADE_RES), then the bot confirms and takes what it is given. It never
    // offers anything itself.
    void handleTrade(Bot& bot, const timer::time_point now)
    {
        auto* PBot     = bot.PChar.get();
        auto* PPartner = PBot->tradePartner();

        if (PPartner == nullptr)
        {
            // Finished, cancelled, or the other side is gone
            PBot->TradePending.clean();
            bot.tradeSeen = false;

            if (bot.trading)
            {
                bot.trading    = false;
                bot.checkItems = true;
            }

            return;
        }

        if (auto* transaction = PBot->activePlayerTradeTransaction())
        {
            if (!bot.tradeConfirmed && now - bot.tradeStartedAt >= kTradeConfirm)
            {
                bot.tradeConfirmed = true;
                PPartner->pushPacket<GP_SERV_COMMAND_ITEM_TRADE_RES>(PBot, GP_ITEM_TRADE_RES_KIND::Make);
            }

            // Changing a slot clears both confirmations, so keep confirming. When the player
            // confirmed first, this completes the trade; otherwise their confirmation does.
            if (bot.tradeConfirmed && transaction->accept(PBot))
            {
                ShowInfoFmt("bot_players: {} traded with {}", PBot->getName(), PPartner->getName());
                transaction->commitAndClose();
            }

            return;
        }

        if (PBot->TradePending.initiator)
        {
            return;
        }

        if (!bot.tradeSeen)
        {
            bot.tradeSeen   = true;
            bot.tradeSeenAt = now;
            return;
        }

        if (now - bot.tradeSeenAt < kTradeDelay)
        {
            return;
        }

        bot.tradeSeen = false;

        if (isAiBot(PPartner->id) || !PlayerTradeTransaction::start(PBot, PPartner))
        {
            // Too far away, or already trading
            PlayerTradeTransaction::cancel(PBot);
            return;
        }

        bot.trading        = true;
        bot.tradeConfirmed = false;
        bot.tradeStartedAt = now;

        PPartner->pushPacket<GP_SERV_COMMAND_ITEM_TRADE_RES>(PBot, GP_ITEM_TRADE_RES_KIND::Start);
    }

    //
    // Treasure
    //

    // Bots pass on everything. With one player in the pool, the item is then lotted for that
    // player, which hands it over at once, as if they were soloing.
    void handleLoot(Bot& bot)
    {
        auto* PBot  = bot.PChar.get();
        auto* PPool = PBot->PTreasurePool;

        if (PPool == nullptr || PPool->itemCount() == 0)
        {
            return;
        }

        CCharEntity* PPlayer = nullptr;
        uint8        players = 0;

        for (auto* PMember : PPool->getMembers())
        {
            if (!isAiBot(PMember->id))
            {
                PPlayer = PMember;
                ++players;
            }
        }

        if (players == 0)
        {
            return;
        }

        const auto& items = PPool->getItems();

        for (uint8 slot = 0; slot < TREASUREPOOL_SIZE; ++slot)
        {
            if (items[slot].ID == 0)
            {
                continue;
            }

            if (!PPool->hasLottedItem(PBot, slot))
            {
                PPool->passItem(PBot, slot);
            }

            // Once every bot in the pool has passed (each does on its own tick)
            if (players != 1 || items[slot].ID == 0 || PPool->hasLottedItem(PPlayer, slot))
            {
                continue;
            }

            const bool botsPassed = std::ranges::all_of(PPool->getMembers(),
                                                        [&](auto* PMember)
                                                        {
                                                            return PMember == PPlayer || PPool->hasPassedItem(PMember, slot);
                                                        });
            if (botsPassed)
            {
                // Refused (and left in the pool) when their inventory is full or it is a rare item they have
                PPool->lotItem(PPlayer, slot, xirand::GetRandomNumber<uint16>(1, 1000));
            }
        }
    }

    //
    // Items
    //

    // Puts on gear that fits an empty slot and learns one scroll. Returns true when the bot
    // started using something. Only runs when flagged, see Bot::checkItems.
    auto useItems(Bot& bot, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        bot.nextItemCheck = now + kItemCheckInterval;

        autoEquipEmptySlots(PBot);

        auto* PInventory = PBot->getStorage(LOC_INVENTORY);

        for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
        {
            auto* PItem = dynamic_cast<CItemUsable*>(PInventory->GetItem(slot));
            if (PItem == nullptr || !PItem->hasFlag(xi::ItemFlag::Scroll) || PItem->isType(ITEM_EQUIPMENT))
            {
                continue;
            }

            // 0: the bot's job can learn it at this level and doesn't know it yet
            const auto [error, param, value] = luautils::OnItemCheck(PBot, PItem, PBot);
            if (error != 0)
            {
                continue;
            }

            standUp(bot);

            if (PBot->PAI->UseItem(EntityId(PBot), LOC_INVENTORY, slot))
            {
                ShowInfoFmt("bot_players: {} uses item {}", PBot->getName(), PItem->getID());
                return true;
            }

            // Can't act right now. The flag stays set, so this is tried again.
            return false;
        }

        bot.checkItems = false;
        return false;
    }

    //
    // Combat
    //

    // Heals the party and fights the leader's target. Returns false when there is nothing to
    // do, and the bot should follow the leader instead.
    auto assist(Bot& bot, CCharEntity* PLeader, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (PLeader->loc.zone != PBot->loc.zone || PLeader->status != xi::Status::Normal)
        {
            calm(bot);
            return false;
        }

        if (now >= bot.nextCureCheck)
        {
            bot.nextCureCheck = now + kCureCheckInterval;

            if (tryCure(bot, now))
            {
                return true;
            }
        }

        auto* PTarget = PLeader->PAI->IsEngaged() ? PLeader->GetBattleTarget() : nullptr;

        if (PTarget == nullptr || PTarget->objtype != TYPE_MOB || PTarget->isDead() ||
            !isWithinDistance(PBot->loc.p, PTarget->loc.p, kAssistRange))
        {
            calm(bot);
            return false;
        }

        if (!PBot->PAI->IsEngaged())
        {
            if (now < bot.nextEngage)
            {
                return false;
            }

            bot.nextEngage = now + kEngageRetry;
            bot.still      = false;
            standUp(bot);

            if (!PBot->PAI->Engage(EntityId(PTarget)))
            {
                return false;
            }

            bot.nextRepath = now;
        }
        else if (PBot->GetBattleTarget() != PTarget)
        {
            PBot->PAI->ChangeTarget(EntityId(PTarget));
        }

        bot.state = AiState::Fight;
        melee(bot, PTarget, now);
        return true;
    }

    // Walks into melee range of the target and faces it. The attack state (ticked by the
    // zone, like a player's) swings when the weapon delay is up.
    void melee(Bot& bot, const CBattleEntity* PTarget, const timer::time_point now)
    {
        auto* PBot  = bot.PChar.get();
        auto* PPath = PBot->PAI->PathFind.get();

        const float range = PBot->GetMeleeRange(PTarget);

        if (!isWithinDistance(PBot->loc.p, PTarget->loc.p, range - kMeleeMargin))
        {
            const bool moving = PPath->IsFollowingPath();

            if (now >= bot.nextRepath && (!moving || !isWithinDistance(bot.pathGoal, PTarget->loc.p, kTargetDrift)))
            {
                bot.nextRepath = now + kRepathInterval;

                if (PPath->PathAround(PTarget->loc.p, std::max(range - kMeleeInside, 0.5f), PATHFLAG_RUN))
                {
                    bot.pathGoal = PTarget->loc.p;
                }
            }

            step(bot, now);
            return;
        }

        stop(bot);

        if (!facing(PBot->loc.p, PTarget->loc.p, 32))
        {
            PPath->LookAt(PTarget->loc.p);
            PBot->updatemask |= UPDATE_POS;
        }
    }

    // Casts the smallest Cure that covers what the most hurt party member in range is
    // missing. Returns true when a cast started.
    auto tryCure(Bot& bot, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (now < bot.nextCast || PBot->health.mp <= 0)
        {
            return false;
        }

        CBattleEntity* PPatient = nullptr;
        uint8          lowest   = kCureBelowHpp;

        for (auto* PMember : PBot->PParty->members)
        {
            if (PMember->isDead() || PMember->loc.zone != PBot->loc.zone)
            {
                continue;
            }

            const auto hpp = PMember->GetHPP();
            if (hpp < lowest && isWithinDistance(PBot->loc.p, PMember->loc.p, kCureRange))
            {
                lowest   = hpp;
                PPatient = PMember;
            }
        }

        if (PPatient == nullptr)
        {
            return false;
        }

        // Somebody needs healing: whatever happens next, don't look again right away
        bot.nextCast = now + kCastRetry;

        const int32            missing = PPatient->GetMaxHP() - PPatient->health.hp;
        std::optional<SpellID> pick;

        for (size_t tier = 0; tier < kCurePotency.size(); ++tier)
        {
            const auto spellId = static_cast<SpellID>(tier + 1); // Cure .. Cure VI
            auto*      PSpell  = spell::GetSpell(spellId);

            if (PSpell == nullptr || PSpell->getMPCost() > PBot->health.mp ||
                !charutils::hasSpell(PBot, static_cast<uint16>(spellId)) || !spell::CanUseSpell(PBot, PSpell))
            {
                continue;
            }

            pick = spellId;

            if (kCurePotency[tier] >= missing)
            {
                break;
            }
        }

        if (!pick)
        {
            return false;
        }

        bot.still = false;
        standUp(bot);
        stop(bot);

        if (!PBot->PAI->Cast(EntityId(PPatient), *pick))
        {
            return false;
        }

        ShowDebugFmt("bot_players: {} casts Cure (tier {}) on {}", PBot->getName(), static_cast<uint16>(*pick), PPatient->getName());
        return true;
    }

    // Sits down to recover HP and MP, like /heal (GP_CLI_COMMAND_CAMP)
    void rest(Bot& bot)
    {
        auto* PBot = bot.PChar.get();

        PBot->PAI->ClearStateStack();

        const auto tickDelay = std::chrono::seconds(settings::get<uint8>("map.HEALING_TICK_DELAY"));
        PBot->StatusEffectContainer->AddStatusEffect(xi::StatusEffect::Healing, 0, 0, tickDelay, 0s);
        PBot->updatemask |= UPDATE_HP;
    }

    void standUp(Bot& bot)
    {
        auto* PBot = bot.PChar.get();

        if (PBot->animation == xi::Animation::Healing)
        {
            PBot->StatusEffectContainer->DelStatusEffectSilent(xi::StatusEffect::Healing);
            PBot->updatemask |= UPDATE_HP;
        }
    }

    // Stops fighting and resting
    void calm(Bot& bot)
    {
        auto* PBot = bot.PChar.get();

        if (PBot->PAI->IsEngaged())
        {
            PBot->PAI->Disengage();
        }

        bot.still = false;
        standUp(bot);
    }

    // Standing next to the leader with nothing to do: learn scrolls, rest.
    void relax(Bot& bot, const CCharEntity* PLeader, const timer::time_point now)
    {
        auto* PBot = bot.PChar.get();

        if (!bot.still)
        {
            bot.still      = true;
            bot.stillSince = now;
        }

        const bool resting      = PBot->animation == xi::Animation::Healing;
        const bool leaderFights = PLeader->PAI->IsEngaged();

        if (resting)
        {
            if (leaderFights || (PBot->health.hp >= PBot->GetMaxHP() && PBot->health.mp >= PBot->GetMaxMP()))
            {
                standUp(bot);
            }
            else
            {
                bot.state = AiState::Rest;
            }

            return;
        }

        if (leaderFights || now - bot.stillSince < kSettleDelay)
        {
            return;
        }

        if (bot.checkItems && now >= bot.nextItemCheck && useItems(bot, now))
        {
            return;
        }

        if (now - bot.stillSince >= kRestDelay && (PBot->health.hp < PBot->GetMaxHP() || PBot->health.mp < PBot->GetMaxMP()))
        {
            rest(bot);
        }
    }

    // Same outcome as the invitee's client answering "yes" (GP_CLI_COMMAND_GROUP_SOLICIT_RES).
    void handleInvite(Bot& bot, const timer::time_point now)
    {
        auto* PBot = bot.PChar.get();

        if (!bot.inviteSeen)
        {
            bot.inviteSeen   = true;
            bot.inviteSeenAt = now;
            return;
        }

        if (now - bot.inviteSeenAt < kInviteDelay)
        {
            return;
        }

        bot.inviteSeen = false;

        const auto invite = PBot->InvitePending;
        PBot->InvitePending.clean();

        auto* PInviter = zoneutils::GetCharFromWorld(invite.entity.UniqueNo, invite.entity.ActIndex);
        if (PInviter == nullptr)
        {
            // Gone, or on another map server (not supported)
            return;
        }

        // Bots join a player's party, nothing else: no alliances, no bot leaders. A party with
        // trusts out can't take a new member (CParty::AddMember refuses).
        const bool canJoin = invite.kind == PartyKind::Party &&
                             PBot->PParty == nullptr &&
                             !isAiBot(PInviter->id) &&
                             PInviter->PTrusts.empty() &&
                             (PInviter->PParty == nullptr || PInviter->PParty->GetLeader() == PInviter);

        if (canJoin)
        {
            if (PInviter->PParty == nullptr)
            {
                PInviter->PParty = new CParty(PInviter);
            }

            if (!PInviter->PParty->IsFull())
            {
                PInviter->PParty->AddMember(PBot);
            }
        }

        if (PBot->PParty == nullptr)
        {
            ShowInfoFmt("bot_players: {} declined a party invite from {}", PBot->getName(), PInviter->getName());
            PInviter->pushPacket<GP_SERV_COMMAND_MESSAGE>(PInviter, 0, 0, MsgStd::InvitationDeclined);
            return;
        }

        ShowInfoFmt("bot_players: {} joined {}'s party", PBot->getName(), PInviter->getName());
    }

    void follow(Bot& bot, CCharEntity* PLeader, const timer::time_point now)
    {
        auto* PBot = bot.PChar.get();

        // Still loading in, or somewhere bots don't go: wait here.
        if (PLeader->loc.zone == nullptr || PLeader->status != xi::Status::Normal || PLeader->inMogHouse() || PLeader->PInstance != nullptr)
        {
            bot.state = AiState::Wait;
            stop(bot);
            return;
        }

        bot.state = AiState::Follow;

        if (PLeader->loc.zone != PBot->loc.zone)
        {
            calm(bot);
            zoneMoves_.emplace_back(PBot->id, placementBehind(PLeader));
            return;
        }

        auto*       PPath  = PBot->PAI->PathFind.get();
        const float dist   = distance(PBot->loc.p, PLeader->loc.p);
        const float stopAt = kFollowStop + kSlotSpacing * static_cast<float>(followSlot(bot));

        const bool moving = PPath->IsFollowingPath();

        // The common case: standing near the leader.
        if (!moving && dist <= stopAt + kFollowSlack)
        {
            relax(bot, PLeader, now);
            return;
        }

        bot.still = false;
        standUp(bot);

        if (dist > kWarpDistance)
        {
            PPath->WarpTo(PLeader->loc.p, stopAt);
            bot.pathFails = 0;
            return;
        }

        if (dist <= stopAt)
        {
            stop(bot);
            return;
        }

        if (now >= bot.nextRepath && (!moving || !isWithinDistance(bot.pathGoal, PLeader->loc.p, kRepathDrift)))
        {
            bot.nextRepath = now + kRepathInterval;

            if (PPath->PathAround(PLeader->loc.p, stopAt, PATHFLAG_RUN))
            {
                bot.pathGoal  = PLeader->loc.p;
                bot.pathFails = 0;
            }
            else if (++bot.pathFails >= kMaxPathFails)
            {
                PPath->WarpTo(PLeader->loc.p, stopAt);
                bot.pathFails = 0;
                return;
            }
        }

        step(bot, now);
    }

    void returnHome(Bot& bot, const timer::time_point now)
    {
        auto* PBot  = bot.PChar.get();
        auto* PPath = PBot->PAI->PathFind.get();

        if (bot.state != AiState::Return)
        {
            bot.state          = AiState::Return;
            bot.returnDeadline = now + kReturnTimeout;
            bot.nextRepath     = now;
            bot.pathFails      = 0;
            PPath->Clear();
            calm(bot);

            ShowInfoFmt("bot_players: {} heads home", PBot->getName());
        }

        if (PBot->loc.zone->GetID() != bot.home.zone)
        {
            // Lands exactly on the home spot, and settles on the next tick
            zoneMoves_.emplace_back(PBot->id, bot.home);
            return;
        }

        const bool moving = PPath->IsFollowingPath();

        if (isWithinDistance(PBot->loc.p, bot.home.pos, kHomeRadius) ||
            (!moving && isWithinDistance(PBot->loc.p, bot.home.pos, kHomeSnapRadius)) ||
            now >= bot.returnDeadline)
        {
            settleAtHome(bot);
            return;
        }

        if (!moving)
        {
            if (now < bot.nextRepath)
            {
                return;
            }

            bot.nextRepath = now + kReturnRepathInterval;

            if (!PPath->PathTo(bot.home.pos, PATHFLAG_RUN))
            {
                if (++bot.pathFails >= kMaxPathFails)
                {
                    settleAtHome(bot);
                }

                return;
            }

            bot.pathFails = 0;
        }

        step(bot, now);
    }

    // Puts the bot on its exact home spot and facing, and goes idle.
    void settleAtHome(Bot& bot)
    {
        auto* PBot = bot.PChar.get();

        PBot->PAI->PathFind->Clear();

        PBot->loc.p.x        = bot.home.pos.x;
        PBot->loc.p.y        = bot.home.pos.y;
        PBot->loc.p.z        = bot.home.pos.z;
        PBot->loc.p.rotation = bot.home.pos.rotation;
        PBot->loc.zone->onEntityMoved(PBot);
        PBot->updatemask |= UPDATE_POS;

        bot.state = AiState::Idle;

        charutils::SaveCharPosition(PBot);

        ShowInfoFmt("bot_players: {} is back home", PBot->getName());
    }

    void step(Bot& bot, const timer::time_point now)
    {
        auto* PPath = bot.PChar->PAI->PathFind.get();

        for (int i = 0; i < kStepsPerTick && PPath->IsFollowingPath(); ++i)
        {
            PPath->FollowPath(now);
        }
    }

    void stop(Bot& bot)
    {
        auto* PPath = bot.PChar->PAI->PathFind.get();

        if (PPath->IsFollowingPath())
        {
            PPath->Clear();
            bot.PChar->updatemask |= UPDATE_POS;
        }
    }

    // 0 for the first bot in the party's member list, 1 for the next, ... Bots following the
    // same leader stop at different distances instead of on top of each other.
    auto followSlot(const Bot& bot) const -> uint8
    {
        uint8 slot = 0;

        for (const auto* PMember : bot.PChar->PParty->members)
        {
            if (PMember == bot.PChar.get())
            {
                break;
            }

            if (isAiBot(PMember->id))
            {
                ++slot;
            }
        }

        return slot;
    }

    // Moves a spawned bot to another zone (or another spot in the same one). The entity is
    // kept, so its party membership survives, unlike a despawn and spawn.
    auto moveToZone(Bot& bot, const Placement& to) -> bool
    {
        auto*  PBot  = bot.PChar.get();
        CZone* PFrom = PBot->loc.zone;
        CZone* PDest = zoneutils::GetZone(to.zone);

        if (PFrom == nullptr || PDest == nullptr)
        {
            return false;
        }

        PBot->PAI->PathFind->Clear();

        if (PFrom == PDest)
        {
            PBot->loc.p = to.pos;
            PFrom->onEntityMoved(PBot);
            PBot->updatemask |= UPDATE_POS;
            return true;
        }

        // Sends the 0x00D despawn to everyone who can see the bot
        PFrom->DecreaseZoneCounter(PBot);
        unindexBot(bot, PFrom);

        PBot->clearPacketList();

        // CZone::CharZoneOut leaves the pool; CZone::IncreaseZoneCounter refuses a char that has one
        PBot->PTreasurePool = nullptr;

        PBot->loc.p           = to.pos;
        PBot->loc.prevzone    = PFrom->GetID();
        PBot->loc.destination = to.zone;
        PBot->loc.boundary    = 0;

        // The party window takes each member's zone from the database (GP_SERV_COMMAND_GROUP_TBL),
        // and only shows HP/MP for members in the viewer's zone. SaveCharPosition() doesn't write it.
        db::preparedStmt("UPDATE chars SET pos_zone = ?, pos_prevzone = ? WHERE charid = ?",
                         static_cast<uint16>(to.zone),
                         static_cast<uint16>(PFrom->GetID()),
                         PBot->id);

        // Also flags the party for a reload, which shows the new zone in the members' party lists
        PDest->IncreaseZoneCounter(PBot);
        if (PBot->loc.zone == nullptr)
        {
            ShowErrorFmt("bot_players: {} could not enter {}, despawning", PBot->getName(), PDest->getName());
            despawn(PBot->id);
            return false;
        }

        indexBot(bot);

        PBot->status = xi::Status::Normal;
        PBot->updatemask |= UPDATE_ALL_CHAR;
        PBot->clearPacketList();

        // The target index is per zone
        db::preparedStmt("UPDATE accounts_sessions SET targid = ?, server_addr = ?, server_port = ? WHERE charid = ?",
                         PBot->targid,
                         PDest->GetIP(),
                         PDest->GetPort(),
                         PBot->id);

        charutils::SaveCharPosition(PBot);

        ShowInfoFmt("bot_players: {} moved from {} to {}", PBot->getName(), PFrom->getName(), PDest->getName());
        return true;
    }

    // A spawned bot that runs the AI (not a puppet)
    auto isAiBot(const uint32 charid) const -> bool
    {
        const auto it = live_.find(charid);
        return it != live_.end() && !it->second.puppet;
    }

    void indexBot(Bot& bot)
    {
        byZone_[bot.PChar->loc.zone].emplace_back(&bot);
    }

    void unindexBot(Bot& bot, CZone* PZone)
    {
        if (const auto it = byZone_.find(PZone); it != byZone_.end())
        {
            std::erase(it->second, &bot);
        }
    }

    //
    // Commands
    //

    void processCommands()
    {
        std::vector<Command> commands;
        {
            const auto rset = db::preparedStmt("SELECT id, command, "
                                               "COALESCE(charid, 0) AS charid, COALESCE(name, '') AS name, "
                                               "COALESCE(race, 0) AS race, COALESCE(face, 0) AS face, COALESCE(size, 0) AS size, "
                                               "COALESCE(job, 0) AS job, COALESCE(level, 1) AS level, COALESCE(nation, 255) AS nation, "
                                               "COALESCE(autospawn, 1) AS autospawn, COALESCE(anchor_charid, 0) AS anchor_charid, "
                                               "zone IS NOT NULL AS has_zone, COALESCE(zone, 0) AS zone, "
                                               "COALESCE(pos_x, 0) AS pos_x, COALESCE(pos_y, 0) AS pos_y, COALESCE(pos_z, 0) AS pos_z "
                                               "FROM bot_commands WHERE status = 'pending' ORDER BY id LIMIT 20");

            FOR_DB_MULTIPLE_RESULTS(rset)
            {
                Command cmd;
                cmd.id           = rset->get<uint32>("id");
                cmd.command      = rset->get<std::string>("command");
                cmd.charid       = rset->get<uint32>("charid");
                cmd.name         = rset->get<std::string>("name");
                cmd.race         = rset->get<uint8>("race");
                cmd.face         = rset->get<uint8>("face");
                cmd.size         = rset->get<uint8>("size");
                cmd.job          = rset->get<uint8>("job");
                cmd.level        = rset->get<uint8>("level");
                cmd.nation       = rset->get<uint8>("nation");
                cmd.autospawn    = rset->get<uint8>("autospawn");
                cmd.anchorCharId = rset->get<uint32>("anchor_charid");

                if (rset->get<uint32>("has_zone"))
                {
                    position_t pos{};
                    pos.x         = rset->get<float>("pos_x");
                    pos.y         = rset->get<float>("pos_y");
                    pos.z         = rset->get<float>("pos_z");
                    cmd.placement = Placement{ static_cast<xi::ZoneId>(rset->get<uint16>("zone")), pos };
                }

                commands.emplace_back(std::move(cmd));
            }
        }

        for (auto& cmd : commands)
        {
            // An anchor overrides an explicit zone/position (for `invite` it is the inviter,
            // for `trade` the one who offers the trade)
            if (cmd.anchorCharId != 0 && cmd.command != "invite" && cmd.command != "trade")
            {
                auto* PAnchor = zoneutils::GetChar(cmd.anchorCharId);
                if (PAnchor == nullptr || PAnchor->loc.zone == nullptr || live_.contains(cmd.anchorCharId))
                {
                    finishCommand(cmd.id, false, "The character to place the bot next to is not online.");
                    continue;
                }
                cmd.placement = placementNear(PAnchor);
            }

            std::string result;
            bool        ok = true;

            if (cmd.command == "create")
            {
                std::tie(ok, result) = create(cmd.name, cmd.race, cmd.face, cmd.size, cmd.job, cmd.level, cmd.nation, cmd.autospawn != 0, cmd.placement);
            }
            else if (cmd.command == "spawn")
            {
                result = spawn(cmd.charid, cmd.placement);
                ok     = live_.contains(cmd.charid);
            }
            else if (cmd.command == "despawn")
            {
                result = despawn(cmd.charid);
                ok     = !live_.contains(cmd.charid);
            }
            else if (cmd.command == "summon")
            {
                if (!cmd.placement)
                {
                    ok     = false;
                    result = "summon needs an anchor character or a zone and position.";
                }
                else
                {
                    result = summon(cmd.charid, *cmd.placement);
                    ok     = live_.contains(cmd.charid);
                }
            }
            else if (cmd.command == "autospawn")
            {
                ok = isBot(cmd.charid);
                if (ok)
                {
                    db::preparedStmt("UPDATE bot_players SET autospawn = ? WHERE charid = ?", cmd.autospawn ? 1 : 0, cmd.charid);
                    result = cmd.autospawn ? "Autospawn on." : "Autospawn off.";
                }
                else
                {
                    result = fmt::format("Character {} is not a bot.", cmd.charid);
                }
            }
            else if (cmd.command == "delete")
            {
                std::tie(ok, result) = remove(cmd.charid);
            }
            else if (cmd.command == "invite")
            {
                std::tie(ok, result) = invite(cmd.charid, cmd.anchorCharId);
            }
            else if (cmd.command == "engage")
            {
                std::tie(ok, result) = engageNearest(cmd.charid);
            }
            else if (cmd.command == "trade")
            {
                std::tie(ok, result) = puppetTrade(cmd.charid, cmd.anchorCharId);
            }
            else if (cmd.command == "move" || cmd.command == "puppet")
            {
                // Testing aids, see the README
                const auto it = live_.find(cmd.charid);
                if (it == live_.end())
                {
                    ok     = false;
                    result = fmt::format("Character {} is not a spawned bot.", cmd.charid);
                }
                else if (cmd.command == "puppet")
                {
                    it->second.puppet = cmd.autospawn != 0;
                    it->second.state  = AiState::Idle;
                    stop(it->second);
                    result = it->second.puppet ? "Puppet on (no AI, treated as a player)." : "Puppet off.";
                }
                else if (!cmd.placement)
                {
                    ok     = false;
                    result = "move needs an anchor character or a zone and position.";
                }
                else
                {
                    ok     = moveToZone(it->second, *cmd.placement);
                    result = ok ? "Moved." : "Could not move the bot there.";
                }
            }
            else
            {
                ok     = false;
                result = fmt::format("Unknown command '{}'.", cmd.command);
            }

            finishCommand(cmd.id, ok, result);
        }
    }

    //
    // Bot lifecycle
    //

    auto create(const std::string& name, const uint8 race, const uint8 face, const uint8 size, const uint8 job,
                const uint8 level, uint8 nation, const bool autospawn, const std::optional<Placement>& placement)
        -> std::pair<bool, std::string>
    {
        if (!isValidName(name))
        {
            return { false, "Names are 3-15 letters, first letter upper case, the rest lower case." };
        }

        if (findCharIdByName(name))
        {
            return { false, fmt::format("The name {} is taken.", name) };
        }

        if (race < 1 || race > 8 || face > 15 || size > 2)
        {
            return { false, "Invalid race, face or size." };
        }

        // Only the six starting jobs are unlocked on a new character (char_jobs.unlocked = 126)
        if (job < 1 || job > 6)
        {
            return { false, "Bots can start as WAR, MNK, WHM, BLM, RDM or THF." };
        }

        if (level < 1 || level > kMaxLevel)
        {
            return { false, "Level must be 1-99." };
        }

        if (!placement)
        {
            return { false, "A new bot needs an anchor character or a zone and position." };
        }

        if (zoneutils::GetZone(placement->zone) == nullptr)
        {
            return { false, fmt::format("Zone {} is not running on this map server.", static_cast<uint16>(placement->zone)) };
        }

        if (nation > 2)
        {
            nation = homeNationForRace(race);
        }

        uint32 charid = 0;
        {
            const auto rset = db::preparedStmt("SELECT COALESCE(MAX(charid), 0) + 1 AS next_id FROM chars");
            if (rset && rset->rowsCount() && rset->next())
            {
                charid = rset->get<uint32>("next_id");
            }
        }

        if (charid == 0)
        {
            return { false, "Could not pick a character id." };
        }

        const auto accid = getOrCreateBotAccount(fmt::format("bot{}", charid));
        if (accid == 0)
        {
            return { false, "Could not create the bot account." };
        }

        // NOTE: The `char_insert` trigger on `chars` creates the char_equip, char_exp, char_history,
        //     : char_inventory, char_jobs, char_pet, char_points, char_profile, char_storage and
        //     : char_unlocks rows itself, and `char_delete` removes all per-character rows.
        // clang-format off
        const bool ok =
            db::preparedStmt("INSERT INTO chars(charid, accid, charname, pos_zone, pos_x, pos_y, pos_z, pos_rot, nation) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)",
                             charid, accid, name, static_cast<uint16>(placement->zone), placement->pos.x, placement->pos.y, placement->pos.z, placement->pos.rotation, nation) &&
            db::preparedStmt("INSERT INTO char_look(charid, face, race, size) VALUES(?, ?, ?, ?)", charid, face, race, size) &&
            db::preparedStmt("INSERT INTO char_stats(charid, mjob) VALUES(?, ?)", charid, job) &&
            db::preparedStmt("INSERT INTO char_flags(charid) VALUES(?)", charid) &&
            db::preparedStmt("INSERT INTO bot_players(charid, autospawn) VALUES(?, ?)", charid, autospawn ? 1 : 0);
        // clang-format on

        if (!ok)
        {
            // The char_delete trigger cleans up whatever got inserted, except char_flags
            db::preparedStmt("DELETE FROM chars WHERE charid = ?", charid);
            db::preparedStmt("DELETE FROM char_flags WHERE charid = ?", charid);
            db::preparedStmt("DELETE FROM bot_players WHERE charid = ?", charid);
            return { false, "Database error while creating the character (see map server log)." };
        }

        ShowInfoFmt("bot_players: created bot '{}' ({})", name, charid);

        // Spawning runs the first-login setup (starting gear etc.) and sets the level.
        const auto spawnResult = spawn(charid, placement, level);
        if (!live_.contains(charid))
        {
            return { false, fmt::format("Created {}, but spawning failed: {}", name, spawnResult) };
        }

        return { true, fmt::format("Created {}. {}", name, spawnResult) };
    }

    auto spawn(const uint32 charid, const std::optional<Placement>& placement, const uint8 setLevel = 0) -> std::string
    {
        if (const auto it = live_.find(charid); it != live_.end())
        {
            return fmt::format("{} is already spawned.", it->second.PChar->getName());
        }

        if (!isBot(charid))
        {
            return fmt::format("Character {} is not a bot.", charid);
        }

        if (zoneutils::GetChar(charid) != nullptr)
        {
            return fmt::format("Character {} is already in the world.", charid);
        }

        auto PBot = charutils::LoadChar(charid);
        if (!PBot || PBot->name.empty())
        {
            return fmt::format("Failed to load character {}.", charid);
        }

        // A placement becomes the bot's home. Without one the bot spawns at its home, which for
        // bots created before homes existed is where the character was last saved.
        auto home = placement;
        if (!home)
        {
            home = loadHome(charid);
        }

        const bool newHome = placement.has_value() || !home.has_value();
        if (!home)
        {
            home = Placement{ PBot->loc.destination, PBot->loc.p };
        }

        const auto zoneId = home->zone;
        CZone*     PZone  = zoneutils::GetZone(zoneId);
        if (PZone == nullptr)
        {
            return fmt::format("Zone {} is not running on this map server.", static_cast<uint16>(zoneId));
        }

        if (newHome)
        {
            saveHome(charid, *home);
        }

        PBot->loc.p        = home->pos;
        PBot->loc.p.moving = 0;

        // A bot never keeps a party across a despawn or a server restart
        db::preparedStmt("DELETE FROM accounts_parties WHERE charid = ?", charid);

        // PCs have no pathfinder of their own (the client moves them)
        PBot->PAI->PathFind = std::make_unique<CPathFind>(PBot.get());

        PBot->loc.prevzone    = zoneId;
        PBot->loc.destination = zoneId;
        PBot->loc.boundary    = 0;
        PBot->m_moghouseID    = 0;
        PBot->animation       = xi::Animation::None;

        // Home can be in another zone than where the character was saved
        db::preparedStmt("UPDATE chars SET pos_zone = ?, pos_prevzone = ? WHERE charid = ?",
                         static_cast<uint16>(zoneId),
                         static_cast<uint16>(zoneId),
                         charid);

        PZone->IncreaseZoneCounter(PBot.get());
        if (PBot->loc.zone == nullptr)
        {
            return "The zone refused the bot (see map server log).";
        }

        PBot->status = xi::Status::Normal;

        // A session row makes the bot "online" for /sea and lets the world server route /tells
        // and party invites to this map server. The map server clears this process's rows on boot.
        db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", charid);
        if (!db::preparedStmt("INSERT INTO accounts_sessions(accid, charid, targid, server_addr, server_port) VALUES(?, ?, ?, ?, ?)",
                              PBot->accid,
                              charid,
                              PBot->targid,
                              PZone->GetIP(),
                              PZone->GetPort()))
        {
            ShowWarningFmt("bot_players: no session row for {} (another character on account {} is online?), it won't show in /sea",
                           PBot->getName(),
                           PBot->accid);
        }

        // First spawn: the same first-login setup a new player gets (starting gear, ring, gil, ...)
        if (!charutils::hasTitle(PBot.get(), kTitleNewAdventurer))
        {
            luautils::callGlobal<void>("xi.player.charCreate", PBot.get());
        }

        if (setLevel > 1)
        {
            const auto job = PBot->GetMJob();

            PBot->jobs.job[static_cast<uint8>(job)] = setLevel;
            PBot->SetMLevel(setLevel);
            charutils::SaveCharJob(PBot.get(), job);
            charutils::CalculateStats(PBot.get());
            charutils::BuildingCharSkillsTable(PBot.get());
            charutils::BuildingCharAbilityTable(PBot.get());
            charutils::BuildingCharTraitsTable(PBot.get());
        }

        // Players equip their starting weapon and ring themselves; bots can't.
        autoEquipEmptySlots(PBot.get());

        PBot->UpdateHealth();
        PBot->health.hp = PBot->GetMaxHP();
        PBot->health.mp = PBot->GetMaxMP();
        PBot->updatemask |= UPDATE_ALL_CHAR;

        // Zone-in and the setup above queued packets for the bot's (nonexistent) client
        PBot->clearPacketList();

        charutils::SaveCharPosition(PBot.get());

        ShowInfoFmt("bot_players: spawned {} ({}) in {} at ({:.2f}, {:.2f}, {:.2f})",
                    PBot->getName(),
                    PBot->id,
                    PZone->getName(),
                    PBot->loc.p.x,
                    PBot->loc.p.y,
                    PBot->loc.p.z);

        const auto message = fmt::format("Spawned {} in {}.", PBot->getName(), PZone->getName());

        auto& bot = live_[charid];
        bot.PChar = std::move(PBot);
        bot.home  = *home;
        bot.level = bot.PChar->GetMLevel();
        indexBot(bot);
        writeStatusFor(bot, true);

        return message;
    }

    // Makes `inviterCharId` (an online player) invite the bot, as if they had used the party
    // menu. The bot answers on its next ticks like any other invite.
    auto invite(const uint32 charid, const uint32 inviterCharId) -> std::pair<bool, std::string>
    {
        const auto it = live_.find(charid);
        if (it == live_.end())
        {
            return { false, fmt::format("Character {} is not a spawned bot.", charid) };
        }

        auto* PBot     = it->second.PChar.get();
        auto* PInviter = zoneutils::GetChar(inviterCharId);
        if (PInviter == nullptr || PInviter->loc.zone == nullptr || isAiBot(inviterCharId))
        {
            return { false, "The inviting character is not an online player." };
        }

        if (PBot->PParty != nullptr || PBot->InvitePending.entity.UniqueNo != 0)
        {
            return { false, fmt::format("{} is already in a party or has an invite pending.", PBot->getName()) };
        }

        if (PInviter->PParty != nullptr && PInviter->PParty->GetLeader() != PInviter)
        {
            return { false, fmt::format("{} is not the leader of their party.", PInviter->getName()) };
        }

        PBot->InvitePending.entity.UniqueNo = PInviter->id;
        PBot->InvitePending.entity.ActIndex = PInviter->targid;
        PBot->InvitePending.kind            = PartyKind::Party;

        return { true, fmt::format("{} invited {}.", PInviter->getName(), PBot->getName()) };
    }

    // Testing aid: the bot (usually a puppet) attacks the closest monster within 30 yalms.
    auto engageNearest(const uint32 charid) -> std::pair<bool, std::string>
    {
        const auto it = live_.find(charid);
        if (it == live_.end())
        {
            return { false, fmt::format("Character {} is not a spawned bot.", charid) };
        }

        auto* PBot = it->second.PChar.get();

        CMobEntity* PNearest = nullptr;
        float       nearest  = 1000.0f;

        PBot->loc.zone->ForEachMob([&](CMobEntity* PMob)
        {
            if (PMob->isAlive() && PMob->PAI->IsSpawned() && PMob->PMaster == nullptr)
            {
                if (const float dist = distance(PBot->loc.p, PMob->loc.p); dist < nearest)
                {
                    nearest  = dist;
                    PNearest = PMob;
                }
            }
        });

        if (PNearest == nullptr)
        {
            return { false, "No monster in this zone." };
        }

        if (nearest >= 30.0f)
        {
            return { false, fmt::format("Nearest monster is {} at ({:.1f}, {:.1f}, {:.1f}), {:.0f} yalms away. Move closer.", PNearest->getName(), PNearest->loc.p.x, PNearest->loc.p.y, PNearest->loc.p.z, nearest) };
        }

        if (!PBot->PAI->Engage(EntityId(PNearest)))
        {
            return { false, fmt::format("Could not engage {}.", PNearest->getName()) };
        }

        return { true, fmt::format("{} attacks {} ({:.1f} yalms away).", PBot->getName(), PNearest->getName(), nearest) };
    }

    // Testing aid: `puppetId` (a puppet) trades with the bot the way a client would. The first
    // call sends the trade request (GP_CLI_COMMAND_TRADE_REQ); once the bot has accepted, a
    // second call offers 5 gil (GP_CLI_COMMAND_TRADE_LIST) and confirms (TRADE_RES, Make).
    auto puppetTrade(const uint32 charid, const uint32 puppetId) -> std::pair<bool, std::string>
    {
        const auto botIt    = live_.find(charid);
        const auto puppetIt = live_.find(puppetId);
        if (botIt == live_.end() || puppetIt == live_.end() || !puppetIt->second.puppet || botIt->second.puppet)
        {
            return { false, "trade needs a spawned bot (charid) and a puppet (anchor_charid)." };
        }

        auto* PBot    = botIt->second.PChar.get();
        auto* PPuppet = puppetIt->second.PChar.get();

        if (auto* transaction = PPuppet->activePlayerTradeTransaction())
        {
            if (PPuppet->tradePartner() != PBot)
            {
                return { false, "The puppet is trading with somebody else." };
            }

            if (transaction->setSlot(PPuppet, PlayerTradeTransaction::GilSlot, 0, 65535, 5) == nullptr)
            {
                return { false, "Could not offer 5 gil." };
            }

            const bool complete = transaction->accept(PPuppet);
            if (complete)
            {
                transaction->commitAndClose();
            }

            return { true, complete ? "Offered 5 gil and confirmed: trade complete." : "Offered 5 gil and confirmed; waiting for the bot to confirm." };
        }

        if (PBot->TradePending.entity.isSet() || PPuppet->TradePending.entity.isSet())
        {
            return { false, "A trade request is already pending." };
        }

        const auto now        = timer::now();
        PPuppet->TradePending = { .entity = EntityId(PBot), .invitedAt = now, .initiator = true };
        PBot->TradePending    = { .entity = EntityId(PPuppet), .invitedAt = now, .initiator = false };

        return { true, fmt::format("{} asked {} to trade.", PPuppet->getName(), PBot->getName()) };
    }

    auto despawn(const uint32 charid) -> std::string
    {
        auto it = live_.find(charid);
        if (it == live_.end())
        {
            return fmt::format("Character {} is not spawned.", charid);
        }

        auto& bot = it->second;

        // Leave properly, so the other members' party lists are updated
        if (bot.PChar->PParty != nullptr)
        {
            charutils::LeaveParty(bot.PChar.get());
        }

        unindexBot(bot, bot.PChar->loc.zone);
        writeStatusFor(bot, false);

        auto PBot = std::move(bot.PChar);
        live_.erase(it);

        const auto name = PBot->getName();

        if (PBot->loc.zone != nullptr)
        {
            charutils::SaveCharPosition(PBot.get());

            // Sends the 0x00D despawn to everyone who can see the bot
            PBot->loc.zone->DecreaseZoneCounter(PBot.get());
        }

        PBot->clearPacketList();

        db::preparedStmt("DELETE FROM accounts_sessions WHERE charid = ?", charid);

        pendingDelete_.emplace_back(std::move(PBot));

        ShowInfoFmt("bot_players: despawned {}", name);
        return fmt::format("Despawned {}.", name);
    }

    auto summon(const uint32 charid, const Placement& placement) -> std::string
    {
        if (live_.contains(charid))
        {
            despawn(charid);
        }

        return spawn(charid, placement);
    }

    auto remove(const uint32 charid) -> std::pair<bool, std::string>
    {
        if (!isBot(charid))
        {
            return { false, fmt::format("Character {} is not a bot.", charid) };
        }

        if (live_.contains(charid))
        {
            despawn(charid);
        }

        // The char_delete trigger removes the per-character rows, except these two. Left behind,
        // they make the char_insert trigger fail for the next bot that gets this charid.
        db::preparedStmt("DELETE FROM chars WHERE charid = ?", charid);
        db::preparedStmt("DELETE FROM char_flags WHERE charid = ?", charid);
        db::preparedStmt("DELETE FROM char_history WHERE charid = ?", charid);
        db::preparedStmt("DELETE FROM bot_players WHERE charid = ?", charid);

        // And once more after the entity is freed, see OnZoneTick
        purgeHistory_.emplace_back(charid);

        ShowInfoFmt("bot_players: deleted bot {}", charid);
        return { true, fmt::format("Deleted bot {}.", charid) };
    }

    void spawnAutospawnBots()
    {
        std::vector<uint32> charids;
        {
            const auto rset = db::preparedStmt("SELECT charid FROM bot_players WHERE autospawn = 1 ORDER BY charid");
            FOR_DB_MULTIPLE_RESULTS(rset)
            {
                charids.emplace_back(rset->get<uint32>("charid"));
            }
        }

        for (const auto charid : charids)
        {
            ShowInfoFmt("bot_players: boot spawn {}: {}", charid, spawn(charid, std::nullopt));
        }
    }

    auto list() const -> std::string
    {
        if (live_.empty())
        {
            return "No bots are spawned.";
        }

        std::string out = "Spawned bots:";
        for (const auto& [charid, bot] : live_)
        {
            out += fmt::format(" {} ({}, {})", bot.PChar->getName(), bot.PChar->loc.zone ? bot.PChar->loc.zone->getName() : "no zone", aiStateName(bot.state));
        }
        return out;
    }

    //
    // Status
    //

    void writeStatus()
    {
        for (auto& [charid, bot] : live_)
        {
            writeStatusFor(bot, true);
        }

        db::preparedStmt("INSERT INTO server_variables(name, value) VALUES('BotsHeartbeat', UNIX_TIMESTAMP()) "
                         "ON DUPLICATE KEY UPDATE value = UNIX_TIMESTAMP()");
    }

    bool   bootSpawnDone_ = false;
    uint32 tickCount_     = 0;

    // Spawned bots by charid. Node-based, so the Bot* in byZone_ stay valid.
    std::unordered_map<uint32, Bot> live_;

    // The bots in each zone, so a zone's tick only touches its own
    std::unordered_map<CZone*, std::vector<Bot*>> byZone_;

    // Zone changes queued by think(), applied at the end of OnZoneTick
    std::vector<std::pair<uint32, Placement>> zoneMoves_;

    std::vector<std::unique_ptr<CCharEntity>> pendingDelete_;

    // Deleted bots whose char_history row has to be removed after their entity is freed
    std::vector<uint32> purgeHistory_;
};

REGISTER_CPP_MODULE(BotPlayersModule);
