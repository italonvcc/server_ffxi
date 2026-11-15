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
//   NOTE: This is not retail behaviour, and a lot of core code assumes every CCharEntity
//   has a session. Bots currently only stand where they are placed. Trading with them,
//   inviting them to a party or sending them a /tell are untested and may crash.
//

#include "common/database.h"
#include "common/utils.h"

#include "map/entities/char_entity.h"
#include "map/item_container.h"
#include "map/items/item_equipment.h"
#include "map/lua/lua_base_entity.h"
#include "map/lua/luautils.h"
#include "map/status_effect_container.h"
#include "map/utils/charutils.h"
#include "map/utils/moduleutils.h"
#include "map/utils/zoneutils.h"
#include "map/zone.h"

#include <cctype>
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

// Where to place a bot.
struct Placement
{
    xi::ZoneId zone;
    position_t pos;
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

void writeStatusFor(const CCharEntity& bot, const bool online)
{
    db::preparedStmt("UPDATE bot_players SET online = ?, zone = ?, pos_x = ?, pos_y = ?, pos_z = ?, "
                     "hp = ?, max_hp = ?, mp = ?, max_mp = ?, status_updated = NOW() WHERE charid = ?",
                     online ? 1 : 0,
                     static_cast<uint16>(bot.loc.zone ? bot.loc.zone->GetID() : bot.loc.destination),
                     bot.loc.p.x,
                     bot.loc.p.y,
                     bot.loc.p.z,
                     bot.health.hp,
                     bot.GetMaxHP(),
                     bot.health.mp,
                     bot.GetMaxMP(),
                     bot.id);
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

        for (auto& [charid, PBot] : live_)
        {
            if (PBot->loc.zone == PZone)
            {
                // Nobody is listening on the other end of these.
                PBot->clearPacketList();
            }
        }
    }

private:
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
            // An anchor overrides an explicit zone/position
            if (cmd.anchorCharId != 0)
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
            // The char_delete trigger cleans up whatever got inserted
            db::preparedStmt("DELETE FROM chars WHERE charid = ?", charid);
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
            return fmt::format("{} is already spawned.", it->second->getName());
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

        // Without a placement, the bot stands where it was last saved
        const auto zoneId = placement ? placement->zone : PBot->loc.destination;
        CZone*     PZone  = zoneutils::GetZone(zoneId);
        if (PZone == nullptr)
        {
            return fmt::format("Zone {} is not running on this map server.", static_cast<uint16>(zoneId));
        }

        if (placement)
        {
            PBot->loc.p = placement->pos;
        }

        PBot->loc.prevzone    = zoneId;
        PBot->loc.destination = zoneId;
        PBot->loc.boundary    = 0;
        PBot->m_moghouseID    = 0;
        PBot->animation       = xi::Animation::None;

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

        writeStatusFor(*PBot, true);
        live_[charid] = std::move(PBot);
        return message;
    }

    auto despawn(const uint32 charid) -> std::string
    {
        auto it = live_.find(charid);
        if (it == live_.end())
        {
            return fmt::format("Character {} is not spawned.", charid);
        }

        auto PBot = std::move(it->second);
        live_.erase(it);

        const auto name = PBot->getName();

        if (PBot->loc.zone != nullptr)
        {
            charutils::SaveCharPosition(PBot.get());

            // Sends the 0x00D despawn to everyone who can see the bot
            PBot->loc.zone->DecreaseZoneCounter(PBot.get());
        }

        PBot->clearPacketList();
        writeStatusFor(*PBot, false);

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

        // The char_delete trigger removes every per-character row
        db::preparedStmt("DELETE FROM chars WHERE charid = ?", charid);
        db::preparedStmt("DELETE FROM bot_players WHERE charid = ?", charid);

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
        for (const auto& [charid, PBot] : live_)
        {
            out += fmt::format(" {} ({})", PBot->getName(), PBot->loc.zone ? PBot->loc.zone->getName() : "no zone");
        }
        return out;
    }

    //
    // Status
    //

    void writeStatus()
    {
        for (const auto& [charid, PBot] : live_)
        {
            writeStatusFor(*PBot, true);
        }

        db::preparedStmt("INSERT INTO server_variables(name, value) VALUES('BotsHeartbeat', UNIX_TIMESTAMP()) "
                         "ON DUPLICATE KEY UPDATE value = UNIX_TIMESTAMP()");
    }

    bool   bootSpawnDone_ = false;
    uint32 tickCount_     = 0;

    std::unordered_map<uint32, std::unique_ptr<CCharEntity>> live_;
    std::vector<std::unique_ptr<CCharEntity>>                pendingDelete_;
};

REGISTER_CPP_MODULE(BotPlayersModule);
