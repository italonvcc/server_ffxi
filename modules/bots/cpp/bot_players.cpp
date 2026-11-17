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
//     - What it does in a party depends on its job: an ordered rule list (bot_roles.h) of
//       heal, buff, debuff, nuke, melee and rest. Bots look at what the rest of the party
//       is casting, and the better-suited bot goes first, so they don't double up.
//     - A trade request from a player is accepted, and whatever the player offers is taken.
//       Gear is put on; what the bot can't wear, and the piece it replaced, goes back into
//       that player's inventory. Food is eaten, and again whenever the effect runs out.
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
//     - Combat decisions are made every kDecisionInterval per bot, from the party list and
//       the bot's cached spell kit. No Lua in that path (the kit is built on events).
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

#include "map/ability.h"
#include "map/ai/ai_container.h"
#include "map/ai/helpers/pathfind.h"
#include "map/ai/states/item_state.h"
#include "map/ai/states/magic_state.h"
#include "map/ai/states/range_state.h"
#include "map/ai/states/weaponskill_state.h"
#include "map/enmity_container.h"
#include "map/entities/mob_entity.h"
#include "map/entities/char_entity.h"
#include "map/enums/msg_std.h"
#include "map/enums/party_kind.h"
#include "map/item_container.h"
#include "map/job_points.h"
#include "map/latent_effect_container.h"
#include "map/items/item_equipment.h"
#include "map/items/item_usable.h"
#include "map/items/item_weapon.h"
#include "map/items/transactions/item_claim.h"
#include "map/items/transactions/player_trade.h"
#include "map/lua/lua_base_entity.h"
#include "map/lua/luautils.h"
#include "map/navmesh/navmesh.h"
#include "map/packets/s2c/0x009_message.h"
#include "map/packets/s2c/0x022_item_trade_res.h"
#include "map/party.h"
#include "map/recast_container.h"
#include "map/spell.h"
#include "map/status_effect_container.h"
#include "map/trait.h"
#include "map/universal_container.h"
#include "map/treasure_pool.h"
#include "map/utils/battleutils.h"
#include "map/utils/charutils.h"
#include "map/utils/itemutils.h"
#include "map/utils/moduleutils.h"
#include "map/utils/zoneutils.h"
#include "map/weapon_skill.h"
#include "map/zone.h"

#include "bot_roles.h"

#include <algorithm>
#include <array>
#include <bitset>
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

// Every this many time server ticks (2.4 s each) the bots' session rows are counted and
// the missing ones put back (see restoreSessions).
constexpr uint32 kSessionCheckEveryNTicks = 12;

//
// AI tuning
//

// A bot waits this long before accepting a party invite, like a player reading the prompt.
constexpr auto kInviteDelay = std::chrono::milliseconds(1200);

// The leader's entity is destroyed and rebuilt when it changes zone, so for a moment the
// party has no leader. Past this, the leader is gone for good and the bot leaves.
constexpr auto kLeaderGrace = std::chrono::seconds(120);

// Following: each bot has its own spot behind or beside the leader (kFollowSpots, by its
// place among the party's bots), moved a little per bot so the group is never regular. A
// bot has arrived within kSpotRadius of its spot. A standing bot only starts moving again
// once the leader is kFollowSlack further away than its spot is (so the leader turning or
// shifting a step doesn't make bots shuffle), and waits up to kSetOffDelay before it does,
// each bot its own time, so they don't all step off together.
struct FollowSpot
{
    float distance; // From the leader
    float angle;    // Degrees from straight behind the leader
};

constexpr std::array kFollowSpots{
    FollowSpot{ 2.2f, -40.0f }, // Behind, to one side
    FollowSpot{ 2.2f, 40.0f },  // Behind, to the other
    FollowSpot{ 3.4f, 0.0f },   // Second row
    FollowSpot{ 3.8f, -70.0f }, // Wide
    FollowSpot{ 3.8f, 70.0f },
};

constexpr float kSpotDistanceJitter = 0.5f;  // Yalms, either way
constexpr float kSpotAngleJitter    = 12.0f; // Degrees, either way
constexpr float kSpotRadius         = 0.7f;
constexpr float kFollowSlack        = 2.0f;
constexpr int   kSetOffDelay        = 500;   // Milliseconds
constexpr uint8 kFacingJitter       = 24;    // Of 256 to a full turn, either way

// Further than this from the leader (teleport, fall, stuck): warp instead of walking.
constexpr float kWarpDistance = 45.0f;

// A bot that finds no path only warps when it is at least this far from the leader, and
// not while the party is fighting: next to the leader it just stays where it is.
constexpr float kWarpMinDistance = 15.0f;

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
// PCs don't). A client moves another player's character at speed / 10 yalms per second
// (5.0 at speed 50) towards where the server says it is, and makes it jump there when it
// falls too far behind. So a bot moves exactly that much per tick, in two path steps: any
// faster and it is seen teleporting at the end of a long run.
constexpr float kYalmsPerTickPerSpeed = 0.04f; // (speed / 10) yalms/s x the 0.4 s tick

// A bot answers a trade request after this long, and confirms its side a moment later.
constexpr auto kTradeDelay   = std::chrono::milliseconds(1200);
constexpr auto kTradeConfirm = std::chrono::milliseconds(1500);

// How often a bot looks at its treasure pool.
constexpr auto kLootInterval = std::chrono::seconds(1);

// Inventory scans (scrolls to learn, gear to put on) are at least this far apart.
constexpr auto kItemCheckInterval = std::chrono::seconds(4);

// Items a bot owes the player it traded with are offered again this often (their inventory
// was full, or they were in another zone).
constexpr auto kReturnRetry = std::chrono::seconds(30);

// Food: how often a bot checks whether its food effect has run out. An item is food when it
// is in one of these auction house categories (fish ... ingredients) and its onItemCheck
// answers 0 or "is full"; "cannot eat" (raw food, wrong race) is handed back.
constexpr auto  kFoodCheckInterval = std::chrono::seconds(5);
constexpr uint8 kAhFoodFirst       = 51;
constexpr uint8 kAhFoodLast        = 59;
constexpr int32 kMsgIsFull         = 246;
constexpr int32 kMsgCannotEat      = 247;

// Shooting: a bot stops kShootHold yalms from the target and shoots kShotGap after the last
// shot ended. After kShotFailsClose attempts that never started (no line of sight) it walks
// up to kShootClose.
constexpr float kShootHold      = 12.0f;
constexpr float kShootClose     = 3.0f;
constexpr auto  kShotGap        = std::chrono::milliseconds(1500);
constexpr uint8 kShotFailsClose = 3;

// Provoke: looked at this often per bot, and after one bot provokes, no other does on that
// enemy for kProvokeShare (the enemy needs a moment to turn).
constexpr auto kProvokeCheck = std::chrono::seconds(1);
constexpr auto kProvokeShare = std::chrono::seconds(5);

// Steal: looked at this often, and what it brought is looked for in the inventory
// kStolenCheck after it was used.
constexpr auto kStealCheck  = std::chrono::seconds(1);
constexpr auto kStolenCheck = std::chrono::seconds(3);

// An order from a player (!ja, !ws) is dropped when the bot could not start it this long
// after it was given.
constexpr auto kOrderLife = std::chrono::seconds(8);

// A spell order (!ma) gets longer: the bot may have to walk into range first
constexpr auto kSpellOrderLife = std::chrono::seconds(20);

// Skillchains. The game refuses to chain a weapon skill that lands less than 3 s after the
// last one (and restarts the chain with it instead), and the window closes 10 s after the
// opener, 1 s sooner per step. A bot's skill lands up to one tick (0.4 s) after it starts,
// so it starts kChainEarliest after the last one landed, and not in the last kChainLatest.
// Only one bot answers each step: the others wait kChainShare for that skill to land.
constexpr auto  kChainEarliest = std::chrono::milliseconds(3500);
constexpr auto  kChainLatest   = std::chrono::milliseconds(1200);
constexpr auto  kChainShare    = std::chrono::milliseconds(2500);
constexpr int16 kWeaponSkillTp = 1000;

// Fighting: a bot helps with the leader's target when it is this close, walks up to
// kMeleeInside yalms inside its melee range and only moves again once it is out of range.
constexpr float kAssistRange = 25.0f;
constexpr float kMeleeInside = 1.2f;

// Bots in melee spread around the enemy: each takes a spot kMeleeSpread degrees further
// round from the leader's side than the last, to either side in turn, moved by up to
// kMeleeSpreadJitter per bot.
constexpr float kMeleeSpread       = 40.0f;
constexpr float kMeleeSpreadJitter = 10.0f;
constexpr float kMeleeMargin = 0.3f;
constexpr float kTargetDrift = 1.0f;
constexpr auto  kEngageRetry = std::chrono::seconds(1);

// Casting. A bot looks for something to cast every kDecisionInterval. After a spell it
// waits kCastGap (the game refuses anything sooner than 2.5 s), and after a refused cast
// kCastRetry, during which other bots don't count on it.
constexpr auto kDecisionInterval = std::chrono::milliseconds(800);
constexpr auto kCastGap          = std::chrono::seconds(3);
constexpr auto kCastRetry        = std::chrono::seconds(3);
constexpr float kCastRange       = 18.0f;

// Smart Heal. A member is a patient under kHealBelowHpp (counting the cures already on
// their way). The Cure may heal up to kOverheal times what is missing; under kEmergencyHpp
// the biggest Cure is used whatever it wastes. A bot with a lower heal rank helps a busy
// healer only for patients under kHelpBelowHpp.
constexpr uint8 kHealBelowHpp = 85;
constexpr uint8 kHelpBelowHpp = 70;
constexpr uint8 kEmergencyHpp = 35;

// A healer looks for patients up to kHealReach yalms away and walks to one that is out of
// cast range (kHealWalkLife: how long it keeps walking without the Heal rule asking again).
constexpr float kHealReach    = 30.0f;
constexpr auto  kHealWalkLife = std::chrono::seconds(4);
constexpr float kOverheal     = 1.1f;

// MP% a bot keeps for heals: buffs and debuffs during a fight, and nukes, are only cast
// while it stays above this.
constexpr uint8 kBuffReserve   = 40;
constexpr uint8 kDebuffReserve = 40;
constexpr uint8 kNukeReserve   = 50;

// Regen goes on members under this HP% during a fight
constexpr uint8 kRegenBelowHpp = 80;

// Debuffs: not on an enemy that is nearly dead or resists them this much. The same effect
// is not tried again on the same target for kDebuffRetry (it may have been resisted), a
// buff for kBuffRetry.
constexpr uint8 kDebuffAboveHpp = 25;
constexpr int16 kMaxResistRank  = 4;
constexpr auto  kDebuffRetry    = std::chrono::seconds(45);
constexpr auto  kBuffRetry      = std::chrono::seconds(15);

// Auction house category of ninja tools; the only usable items in it are the toolbags
constexpr uint8 kAhNinjaTools = 49;

// Status removal: the same ailment on the same member is not tried again for kCleanseRetry
// (the spell is on its way, or it failed, as Cursna can).
constexpr auto kCleanseRetry = std::chrono::seconds(6);

// Songs reach about 10 yalms around the bard. To sing for members who lack a song it walks
// up to kSongReach of the one with the most others within kSongCluster. A song is sung
// again when it has less than kSongRefresh left, and not twice within kSongRetry.
constexpr float kSongReach   = 7.0f;
constexpr float kSongCluster = 5.0f;
constexpr auto  kSongRefresh = std::chrono::seconds(10);
constexpr auto  kSongRetry   = std::chrono::seconds(12);

// A mage that does not melee stays kMageHold yalms from the enemy (most area attacks reach
// 10 to 15): it backs off when closer than kMageNear and closes in when out of cast range.
constexpr float kMageHold = 17.0f;
constexpr float kMageNear = 15.0f;

// It also stays within kMageLeash of the party leader, so it can't be left behind when the
// fight moves.
constexpr float kMageLeash = 22.0f;

// No elemental spell on an enemy for the first kNukeHold of the fight (counted from the
// moment the party is on its enmity list): debuffs go first and the tank gets ahead.
constexpr auto kNukeHold = std::chrono::seconds(8);

// How often a standing bot looks whether a buff is waiting to be cast (it does not sit then)
constexpr auto kBuffWaitCheck = std::chrono::seconds(1);

// Standing still this long with nothing to do: sit down and rest.
constexpr auto kSettleDelay = std::chrono::seconds(2);

// How long the dashboard shows the last spell a bot cast
constexpr auto kActionShown = std::chrono::seconds(8);

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

    std::string action;

    // CCharEntity::m_WeaponSkills: the weapon skills the bot can use with what it holds
    std::array<uint8, 32> weaponSkills{};

    auto operator==(const StatusSnapshot&) const -> bool = default;
};

// The spells a bot can cast at its level, per use. Built on spawn, on a level up and after a
// scroll: checking a spell asks Lua, so it is not done per decision.
struct Kit
{
    std::array<uint16, bots::kCureTiers>      cures{};   // By tier, 0 where the bot has none
    std::array<uint16, bots::kBuffs.size()>   buffs{};   // By bots::kBuffs line, the highest tier
    std::array<uint16, bots::kDebuffs.size()> debuffs{}; // By bots::kDebuffs line
    std::vector<uint16>                       nukes;
    std::array<bool, bots::kCleanses.size()>  cleanse{};        // By bots::kCleanses line
    bool                                      erase    = false;
    bool                                      cleanses = false; // Any of cleanse, or erase
    bool                                      songs    = false; // Knows a bard song
    std::array<uint16, bots::kShadows.size()> shadows{};        // By bots::kShadows entry, 0 where not known
    bool                                      dispel  = false;
    bool                                      hasCure = false;
    bool                                      any     = false;
};

// An effect somebody cast on a target a moment ago
struct Tried
{
    uint32            targetId;
    xi::StatusEffect  effect;
    timer::time_point until;
};

// An item a bot hands back to the player it traded with
struct Owed
{
    uint8  slot; // In the bot's inventory
    uint16 itemId;
};

// Something one bot is doing to an enemy, so the others leave it alone until `until`
struct Claim
{
    uint32            mobId = 0;
    timer::time_point until{};
    timer::time_point step{}; // Skillchains: when the weapon skill being answered landed
};

// Something a player told a bot to do (!ja, !ws, !ma)
struct Order
{
    enum class Kind : uint8
    {
        None,
        Ability,
        WeaponSkill,
        Spell,
    };

    Kind              kind     = Kind::None;
    uint16            id       = 0; // Ability, weapon skill or spell id
    uint32            targetId = 0;
    uint16            targid   = 0;
    uint32            playerId = 0; // Who gave it
    std::string       name;         // Of the ability or weapon skill, for messages
    timer::time_point until{};
};

// A song a player asked a bard for (!ma): sung again whenever it wears off
struct Song
{
    uint16           spellId;
    xi::StatusEffect effect;
    uint32           targetId; // 0: for everybody around the bard. Else the one member it is for (Etudes)
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

    // The last player it traded with, the inventory slots that were in use before that trade
    // (anything else arrived with it), and what it still has to hand back to that player
    uint32            traderId = 0;
    std::bitset<256>  slotsBefore;
    bool              tradedPending = false;
    std::vector<Owed> giveBack;
    timer::time_point nextReturn{};

    // The food it eats whenever it has no food effect (bot_players.food), 0 for none
    uint16            foodId = 0;
    bool              eatNow = false; // Just handed over: eat it even when not in a party
    timer::time_point nextFoodCheck{};

    timer::time_point nextLootCheck{};

    // Set by events that can change what is in the inventory or what the bot can use
    bool              checkItems = true;
    timer::time_point nextItemCheck{};
    uint8             level = 0;

    // Combat
    const bots::Role* role = &bots::kFighter;
    Kit               kit;
    bool              kitDirty = true;
    bool              inFight  = false; // A party member is fighting something
    timer::time_point nextEngage{};
    timer::time_point nextDecision{};
    timer::time_point nextCast{};
    timer::time_point unableUntil{}; // A cast was refused: don't count on this bot until then

    timer::time_point nextProvokeCheck{};

    // Steal: the inventory slots in use when it was used; what is new after it was stolen
    timer::time_point nextStealCheck{};
    std::bitset<256>  slotsBeforeSteal;
    bool              stolenPending = false;
    timer::time_point stolenCheckAt{};

    // A mage keeping its distance from the enemy: 1 closing in, -1 backing off, 0 neither
    int8 spacing = 0;

    // The enemy this mage has already taken its distance from. It backs off once per
    // enemy, as the fight starts, and never again.
    uint32 spacedFor = 0;

    // Melee: the enemy it has a spot around, the direction of that spot from the enemy
    // (radians), whether it has pathed there and whether it got there (or as near as it can)
    uint32 meleeFor     = 0;
    float  meleeAngle   = 0.0f;
    bool   meleeAimed   = false;
    bool   meleeSettled = false;

    // The enemy the party is fighting and when the party first had its attention
    uint32            foeId = 0;
    timer::time_point foeSince{};

    // The party member this healer is walking into cast range of (0 = nobody)
    uint32            patientId = 0;
    timer::time_point patientUntil{};
    float             patientReach = kCastRange; // How close it has to get

    // The songs it keeps up, oldest first
    std::vector<Song> songs;

    // Whether a buff is waiting to be cast, looked at every kBuffWaitCheck while standing
    bool              buffWaiting = false;
    timer::time_point nextBuffWaitCheck{};

    Order order;

    // Ranged attacks
    bool              ammoDirty = true;  // Look through the inventory for (better) ammo
    bool              hadAmmo   = false; // To notice the last arrow leaving the ammo slot
    uint8             shotFails = 0;     // Attempts since the last shot that started
    timer::time_point nextShot{};

    // The last spell cast, for the dashboard
    std::string       action;
    timer::time_point actionUntil{};

    // Following: waiting a moment before setting off after the leader, and where the leader
    // was on the last tick (to tell whether they are on the move)
    bool              settingOff = false;
    timer::time_point setOffAt{};
    position_t        leaderAt{};

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

    // bot_players.weapon_skills, rebuilt when the set changes
    std::string weaponSkillNames;
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

    // Added with the per-job AI: the last spell the bot cast (live column)
    db::preparedStmt("ALTER TABLE `bot_players` ADD COLUMN IF NOT EXISTS `action` VARCHAR(48) NOT NULL DEFAULT ''");

    // Added with food: the item id of the food the bot keeps eating, 0 for none
    db::preparedStmt("ALTER TABLE `bot_players` ADD COLUMN IF NOT EXISTS `food` SMALLINT(5) UNSIGNED NOT NULL DEFAULT 0");

    // For the dashboard: the weapon skills the bot can use right now, by name, comma separated
    db::preparedStmt("ALTER TABLE `bot_players` ADD COLUMN IF NOT EXISTS `weapon_skills` VARCHAR(1024) NOT NULL DEFAULT ''");

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

    // The `jobs` command: the new subjob (NULL = keep, 0 = none)
    db::preparedStmt("ALTER TABLE `bot_commands` ADD COLUMN IF NOT EXISTS `subjob` TINYINT(3) UNSIGNED NULL DEFAULT NULL AFTER `job`");
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

// The containers a bot keeps gear in: the inventory, then the eight wardrobes (the same ones
// charutils::LoadJobChangeGear looks in).
inline constexpr std::array<uint8, 9> kGearContainers{
    LOC_INVENTORY, LOC_WARDROBE, LOC_WARDROBE2, LOC_WARDROBE3, LOC_WARDROBE4, LOC_WARDROBE5, LOC_WARDROBE6, LOC_WARDROBE7, LOC_WARDROBE8,
};

// Equips anything in the inventory or a wardrobe that fits an empty slot (starting weapon,
// nation ring, gear stored on another job, ...). EquipItem() does the level/job/race checks and
// quietly refuses what doesn't fit.
void autoEquipEmptySlots(CCharEntity* PBot)
{
    for (const auto containerId : kGearContainers)
    {
        auto* PContainer = PBot->getStorage(containerId);

        for (uint8 itemSlot = 1; itemSlot <= PContainer->GetSize(); ++itemSlot)
        {
            auto* PItem = dynamic_cast<CItemEquipment*>(PContainer->GetItem(itemSlot));
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
                    charutils::EquipItem(PBot, itemSlot, slot, containerId);
                    break;
                }
            }
        }
    }
}

// Bots have no client to show a bag, so they all get the largest one (the size the Gobbie bag
// quests end at) instead of the 30 slots of a new character.
constexpr uint8 kBotInventorySize = 80;

void growInventory(CCharEntity* PBot)
{
    auto* PInventory = PBot->getStorage(LOC_INVENTORY);
    if (PInventory->GetBuff() >= kBotInventorySize)
    {
        return;
    }

    PInventory->AddBuff(static_cast<int8>(kBotInventorySize - PInventory->GetBuff()));
    charutils::SaveCharInventoryCapacity(PBot);
}

// When the inventory has fewer free slots than this, the gear the bot isn't wearing goes to its
// wardrobes. Ammo stays: equipBestAmmo only looks in the inventory.
constexpr uint8 kStowBelowFreeSlots = 10;

// Moves gear the bot isn't wearing from its inventory to the first wardrobe with room, the way
// the client's item move packet (0x029) does. Returns how many items were moved.
auto stowSpareGear(CCharEntity* PBot) -> int
{
    auto* PInventory = PBot->getStorage(LOC_INVENTORY);
    if (PInventory->GetFreeSlotsCount() >= kStowBelowFreeSlots)
    {
        return 0;
    }

    int moved = 0;
    for (uint8 invSlot = 1; invSlot <= PInventory->GetSize(); ++invSlot)
    {
        auto* PItem = dynamic_cast<CItemEquipment*>(PInventory->GetItem(invSlot));
        if (PItem == nullptr || PItem->isBusy() || PItem->getEquipSlotId() == (1 << SLOT_AMMO))
        {
            continue;
        }

        bool worn = false;
        for (uint8 slot = 0; slot < MAX_SLOTTYPE && !worn; ++slot)
        {
            worn = PBot->getEquip(static_cast<SLOTTYPE>(slot)) == PItem;
        }

        if (worn)
        {
            continue;
        }

        for (const auto containerId : kGearContainers)
        {
            if (containerId == LOC_INVENTORY)
            {
                continue;
            }

            auto* PWardrobe = PBot->getStorage(containerId);
            if (PWardrobe->GetFreeSlotsCount() == 0)
            {
                continue;
            }

            const auto newSlot = PInventory->MoveItemTo(invSlot, *PWardrobe);
            if (newSlot == ERROR_SLOTID)
            {
                continue;
            }

            const auto rset = db::preparedStmt("UPDATE char_inventory SET location = ?, slot = ? WHERE charid = ? AND location = ? AND slot = ?",
                                               containerId,
                                               newSlot,
                                               PBot->id,
                                               static_cast<uint8>(LOC_INVENTORY),
                                               invSlot);
            if (rset && rset->rowsAffected())
            {
                ++moved;
            }
            else
            {
                // Keep memory and the DB the same
                PWardrobe->MoveItemTo(newSlot, *PInventory, invSlot);
            }

            break;
        }
    }

    if (moved > 0)
    {
        ShowInfoFmt("bot_players: {} moved {} piece(s) of gear to its wardrobes", PBot->getName(), moved);
    }

    return moved;
}

// In a town (a zone of the City type): bots don't eat or cast buffs there
auto inTown(const CCharEntity* PBot) -> bool
{
    return PBot->loc.zone != nullptr && (PBot->loc.zone->GetTypeMask() & xi::ZoneType::City) != xi::ZoneType::Unknown;
}

auto isWorn(const CCharEntity* PBot, const CItem* PItem) -> bool
{
    for (uint8 slot = 0; slot < MAX_SLOTTYPE; ++slot)
    {
        if (PBot->getEquip(static_cast<SLOTTYPE>(slot)) == PItem)
        {
            return true;
        }
    }

    return false;
}

// Whether the bot's job, level and race allow the item. These are the checks of
// charutils::EquipArmor, made up front because that empties the slot before it is done checking.
auto canWear(CCharEntity* PBot, CItemEquipment* PItem) -> bool
{
    const auto job = static_cast<uint8>(PBot->GetMJob());

    return (PItem->getJobs() & (1U << (job - 1))) != 0 &&
           PItem->getReqLvl() <= PBot->jobs.job[job] &&
           PItem->getSuperiorLevel() <= PBot->getMod(xi::Mod::SUPERIOR_LEVEL) &&
           PItem->isEquippableByRace(PBot->look.race);
}

// Ammo the bot can shoot with what is in its ranged slot (arrows for a bow, bolts for a
// crossbow, bullets for a gun), or throw when that slot is empty.
auto isAmmoFor(CCharEntity* PBot, CItemEquipment* PItem) -> bool
{
    auto* PAmmo = dynamic_cast<CItemWeapon*>(PItem);
    if (PAmmo == nullptr || !PAmmo->isRanged() || !(PAmmo->getEquipSlotId() & (1 << SLOT_AMMO)))
    {
        return false;
    }

    auto* PRanged = dynamic_cast<CItemWeapon*>(PBot->getEquip(SLOT_RANGED));
    if (PRanged == nullptr || !PRanged->isRanged())
    {
        return PAmmo->getSkillType() == xi::SkillType::Throwing;
    }

    return PAmmo->getSkillType() == PRanged->getSkillType() &&
           (PAmmo->getSkillType() == xi::SkillType::Archery || PAmmo->getSubSkillType() == PRanged->getSubSkillType());
}

// Ammo the bot keeps in its inventory for when the equipped stack runs out
auto isSpareAmmo(CCharEntity* PBot, CItemEquipment* PItem) -> bool
{
    return isAmmoFor(PBot, PItem) && canWear(PBot, PItem);
}

// The equipment slot a new piece goes in. Weapons go in the main hand, or the off hand when
// the bot can dual wield and that hand is free; rings and earrings in an empty slot first.
auto slotFor(CCharEntity* PBot, CItemEquipment* PItem) -> std::optional<uint8>
{
    const auto mask = PItem->getEquipSlotId();

    if (mask & (1 << SLOT_MAIN))
    {
        auto* PMain = dynamic_cast<CItemWeapon*>(PBot->getEquip(SLOT_MAIN));
        auto* PNew  = dynamic_cast<CItemWeapon*>(PItem);

        const bool offHand = (mask & (1 << SLOT_SUB)) && PNew != nullptr && PMain != nullptr &&
                             !PMain->isTwoHanded() && !PMain->isHandToHand() &&
                             PBot->getEquip(SLOT_SUB) == nullptr && charutils::hasTrait(PBot, TRAIT_DUAL_WIELD);

        return offHand ? SLOT_SUB : SLOT_MAIN;
    }

    std::optional<uint8> first;

    for (uint8 slot = 0; slot < MAX_SLOTTYPE; ++slot)
    {
        if (!(mask & (1 << slot)))
        {
            continue;
        }

        if (PBot->getEquip(static_cast<SLOTTYPE>(slot)) == nullptr)
        {
            return slot;
        }

        if (!first)
        {
            first = slot;
        }
    }

    return first;
}

auto loadFood(const uint32 charid) -> uint16
{
    const auto rset = db::preparedStmt("SELECT food FROM bot_players WHERE charid = ? LIMIT 1", charid);
    return rset && rset->rowsCount() && rset->next() ? rset->get<uint16>("food") : 0;
}

// The weapons of a job xi.player.charCreate has no starting gear for
void giveStarterGear(CCharEntity* PBot)
{
    auto transaction = ItemClaimTransaction::start(PBot);
    if (!transaction)
    {
        return;
    }

    bool any = false;

    for (const auto& starter : bots::kStarterGear)
    {
        if (starter.job == PBot->GetMJob() && transaction->give(LOC_INVENTORY, starter.item, starter.quantity, Silence::Yes).has_value())
        {
            any = true;
        }
    }

    if (any && !transaction->commit())
    {
        ShowWarningFmt("bot_players: could not give {} its starting gear", PBot->getName());
    }
}

// Char var: bit per job the bot has had its starting gear for
const std::string kGearedJobsVar = "[bot]gearedJobs";

// The first time a bot has `job` as its main job after a job change: the gear a new
// character of that job starts with.
void giveFirstJobGear(CCharEntity* PBot)
{
    const auto job    = PBot->GetMJob();
    const auto bit    = 1 << static_cast<uint8>(job);
    const auto geared = PBot->getCharVar(kGearedJobsVar);

    if (geared & bit)
    {
        return;
    }

    PBot->setCharVar(kGearedJobsVar, geared | bit);

    auto transaction = ItemClaimTransaction::start(PBot);
    if (!transaction)
    {
        return;
    }

    bool any = false;

    const auto give = [&](const bots::StarterItem& starter)
    {
        if (starter.job == job && transaction->give(LOC_INVENTORY, starter.item, starter.quantity, Silence::Yes).has_value())
        {
            any = true;
        }
    };

    std::ranges::for_each(bots::kStarterGear, give);
    std::ranges::for_each(bots::kFirstJobGear, give);

    if (any && !transaction->commit())
    {
        ShowWarningFmt("bot_players: could not give {} the starting gear of its new job", PBot->getName());
    }
}

struct Command
{
    uint32                   id{};
    std::string              command;
    uint32                   charid{};
    std::string              name;
    uint8                    race{}, face{}, size{}, job{}, level{}, nation{}, autospawn{};
    uint8                    subjob{ 255 }; // `jobs`: 255 = keep
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

    StatusSnapshot snapshot{
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
        .action   = online && timer::now() < bot.actionUntil ? bot.action : std::string(),
    };

    std::ranges::copy(entity.m_WeaponSkills, snapshot.weaponSkills.begin());

    if (bot.lastStatus && *bot.lastStatus == snapshot)
    {
        return;
    }

    if (!bot.lastStatus || bot.lastStatus->weaponSkills != snapshot.weaponSkills)
    {
        // The core rebuilds the set on a skill up, a level up and a weapon change
        bot.weaponSkillNames.clear();

        for (uint16 id = 1; id < MAX_WEAPONSKILL_ID; ++id)
        {
            auto* PSkill = charutils::hasWeaponSkill(bot.PChar.get(), id) ? battleutils::GetWeaponSkill(id) : nullptr;
            if (PSkill != nullptr)
            {
                if (!bot.weaponSkillNames.empty())
                {
                    bot.weaponSkillNames += ',';
                }

                bot.weaponSkillNames += PSkill->getName();
            }
        }
    }

    bot.lastStatus = snapshot;

    db::preparedStmt("UPDATE bot_players SET online = ?, zone = ?, pos_x = ?, pos_y = ?, pos_z = ?, "
                     "hp = ?, max_hp = ?, mp = ?, max_mp = ?, level = ?, exp = ?, exp_next = ?, "
                     "ai_state = ?, leader = ?, action = ?, weapon_skills = ?, status_updated = NOW() WHERE charid = ?",
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
                     snapshot.action,
                     bot.weaponSkillNames.substr(0, 1024),
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

        curePower_ = settings::get<float>("main.CURE_POWER");
        autoLearn_ = settings::get<bool>("custom.BOT_AUTO_LEARN_SPELLS");

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

        // !ja / !ws / !ma (modules/bots/lua/bot_orders.lua): `command` is "ja", "ws" or "ma".
        // Returns what to tell the player.
        bots["order"] = [this](CLuaBaseEntity* PLuaPlayer, const std::string& botName, const std::string& what, const std::string& command) -> std::string
        {
            auto* PPlayer = PLuaPlayer ? dynamic_cast<CCharEntity*>(PLuaPlayer->GetBaseEntity()) : nullptr;
            if (PPlayer == nullptr || PPlayer->loc.zone == nullptr)
            {
                return "You need to be in a zone.";
            }

            const auto kind = command == "ws" ? Order::Kind::WeaponSkill : command == "ma" ? Order::Kind::Spell : Order::Kind::Ability;

            return giveOrder(PPlayer, botName, what, kind);
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

        if (tickCount_ % kSessionCheckEveryNTicks == 0)
        {
            restoreSessions();
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
            bot.kitDirty   = true;
            bot.ammoDirty  = true;
        }

        // Casting or using an item: moving would interrupt it
        if (PBot->PAI->IsCurrentState<CMagicState>())
        {
            bot.nextCast = now + kCastGap;
            return;
        }

        if (PBot->PAI->IsCurrentState<CItemState>())
        {
            // It may be a scroll
            bot.kitDirty = true;
            return;
        }

        if (PBot->PAI->IsCurrentState<CWeaponSkillState>())
        {
            return;
        }

        if (PBot->PAI->IsCurrentState<CRangeState>())
        {
            // Aiming: moving would cancel the shot
            bot.shotFails = 0;
            bot.nextShot  = now + kShotGap;
            return;
        }

        if (bot.tradedPending)
        {
            handleTradedItems(bot, now);
        }
        else if (!bot.giveBack.empty() && now >= bot.nextReturn)
        {
            returnItems(bot, now);
        }

        if (bot.stolenPending && now >= bot.stolenCheckAt)
        {
            collectStolen(bot, now);
        }

        if (bot.order.kind != Order::Kind::None && carryOut(bot, now))
        {
            return;
        }

        // Not in town: food handed over there is eaten once the bot is out in the field
        if (bot.foodId != 0 && now >= bot.nextFoodCheck && (PBot->PParty != nullptr || bot.eatNow) && !inTown(PBot))
        {
            bot.nextFoodCheck = now + kFoodCheckInterval;

            if (eat(bot))
            {
                return;
            }
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
                // Looked at once the bot is not in the middle of a spell or a shot
                bot.trading       = false;
                bot.checkItems    = true;
                bot.tradedPending = true;
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

        // What the bot can't use goes back to this player. Items still owed to somebody else
        // stay with the bot.
        if (bot.traderId != PPartner->id)
        {
            bot.traderId = PPartner->id;
            bot.giveBack.clear();
        }

        bot.slotsBefore.reset();

        const auto* PInventory = PBot->getStorage(LOC_INVENTORY);
        for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
        {
            bot.slotsBefore.set(slot, PInventory->GetItem(slot) != nullptr);
        }

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

    // Puts on gear that fits an empty slot, learns one scroll or opens one toolbag (the ninja
    // tools inside go to its inventory). Returns true when the bot
    // started using something. Only runs when flagged, see Bot::checkItems.
    auto useItems(Bot& bot, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        bot.nextItemCheck = now + kItemCheckInterval;

        autoEquipEmptySlots(PBot);

        // Not while a trade or a Steal is being looked at: those compare inventory slots, and
        // Bot::giveBack holds inventory slots too
        if (!bot.tradedPending && !bot.stolenPending && bot.giveBack.empty())
        {
            stowSpareGear(PBot);
        }

        auto* PInventory = PBot->getStorage(LOC_INVENTORY);

        for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
        {
            auto* PItem = dynamic_cast<CItemUsable*>(PInventory->GetItem(slot));
            if (PItem == nullptr || PItem->isType(ITEM_EQUIPMENT) || (!PItem->hasFlag(xi::ItemFlag::Scroll) && PItem->getAHCat() != kAhNinjaTools))
            {
                continue;
            }

            // 0: the bot's job can learn it at this level and doesn't know it yet, or there
            // is room for what the toolbag holds
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
    // Traded gear and food
    //

    // Puts a piece of gear on, in place of what is in its slot. False when the bot can't wear it.
    auto wear(Bot& bot, CItemEquipment* PItem) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (!canWear(PBot, PItem))
        {
            return false;
        }

        auto slot = slotFor(PBot, PItem);
        if (!slot)
        {
            return false;
        }

        charutils::EquipItem(PBot, PItem->getSlotID(), *slot, LOC_INVENTORY);

        if (*slot == SLOT_SUB && PBot->getEquip(SLOT_SUB) != PItem && (PItem->getEquipSlotId() & (1 << SLOT_MAIN)))
        {
            // Not a weapon the bot can hold in its off hand
            slot = SLOT_MAIN;
            charutils::EquipItem(PBot, PItem->getSlotID(), *slot, LOC_INVENTORY);
        }

        return PBot->getEquip(static_cast<SLOTTYPE>(*slot)) == PItem;
    }

    enum class Edible : uint8
    {
        No,
        Yes,
        CannotEat, // Food, but not for this bot (raw fish or meat and the wrong race)
    };

    // There is no "food" flag on items. Asks the item's script; only called for traded items.
    auto edible(CCharEntity* PBot, CItem* PItem) const -> Edible
    {
        auto* PUsable = dynamic_cast<CItemUsable*>(PItem);
        if (PUsable == nullptr || PUsable->hasFlag(xi::ItemFlag::Scroll) || PUsable->getAHCat() < kAhFoodFirst || PUsable->getAHCat() > kAhFoodLast)
        {
            return Edible::No;
        }

        const auto [error, param, value] = luautils::OnItemCheck(PBot, PUsable, PBot);

        if (error == 0 || error == kMsgIsFull)
        {
            return Edible::Yes;
        }

        return error == kMsgCannotEat ? Edible::CannotEat : Edible::No;
    }

    void owe(Bot& bot, const CItem* PItem)
    {
        const Owed owed{ PItem->getSlotID(), PItem->getID() };

        const bool known = std::ranges::any_of(bot.giveBack, [&](const Owed& other)
                                               {
                                                   return other.slot == owed.slot;
                                               });
        if (!known)
        {
            bot.giveBack.push_back(owed);
        }
    }

    // After a trade: puts on the gear it was given, starts on the food, and hands back what it
    // has no use for, along with the gear that was replaced.
    void handleTradedItems(Bot& bot, const timer::time_point now)
    {
        auto* PBot       = bot.PChar.get();
        auto* PInventory = PBot->getStorage(LOC_INVENTORY);

        bot.tradedPending = false;

        std::array<CItemEquipment*, MAX_SLOTTYPE> wornBefore{};
        for (uint8 slot = 0; slot < MAX_SLOTTYPE; ++slot)
        {
            wornBefore[slot] = PBot->getEquip(static_cast<SLOTTYPE>(slot));
        }

        std::vector<CItemEquipment*> gear;
        uint16                       newFood  = 0;
        bool                         moreFood = false;

        for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
        {
            auto* PItem = PInventory->GetItem(slot);
            if (PItem == nullptr || bot.slotsBefore.test(slot))
            {
                continue;
            }

            if (auto* PGear = dynamic_cast<CItemEquipment*>(PItem))
            {
                gear.push_back(PGear);
                continue;
            }

            switch (edible(PBot, PItem))
            {
                case Edible::No:
                    break;
                case Edible::CannotEat:
                    owe(bot, PItem);
                    break;
                case Edible::Yes:
                    if (PItem->getID() == bot.foodId)
                    {
                        moreFood = true;
                    }
                    else if (newFood == 0 || newFood == PItem->getID())
                    {
                        newFood = PItem->getID();
                    }
                    else
                    {
                        // One kind of food at a time
                        owe(bot, PItem);
                    }
                    break;
            }
        }

        if (gear.empty() && newFood == 0 && !moreFood && bot.giveBack.empty())
        {
            return;
        }

        // Ammo last: what fits depends on the ranged weapon, which may be in this trade too
        std::ranges::stable_partition(gear, [](const CItemEquipment* PGear)
                                      {
                                          return !(PGear->getEquipSlotId() & (1 << SLOT_AMMO));
                                      });

        for (auto* PGear : gear)
        {
            // Usable ammo is kept as spare stacks; the best one is picked below
            if (!isSpareAmmo(PBot, PGear) && wear(bot, PGear))
            {
                ShowInfoFmt("bot_players: {} equips item {}", PBot->getName(), PGear->getID());
            }
        }

        if (!gear.empty())
        {
            bot.ammoDirty = false;
            equipBestAmmo(bot);

            // In case EquipItem emptied a slot and then refused the new piece
            autoEquipEmptySlots(PBot);

            PBot->UpdateHealth();
            PBot->updatemask |= UPDATE_ALL_CHAR;

            for (auto* PGear : gear)
            {
                if (!isWorn(PBot, PGear) && !isSpareAmmo(PBot, PGear))
                {
                    owe(bot, PGear);
                }
            }

            for (auto* PGear : wornBefore)
            {
                if (PGear != nullptr && !isWorn(PBot, PGear) && !isSpareAmmo(PBot, PGear))
                {
                    owe(bot, PGear);
                }
            }
        }

        if (newFood != 0)
        {
            // A different food: the old one goes back, and its effect makes room for the new one
            if (bot.foodId != 0)
            {
                for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
                {
                    const auto* PItem = PInventory->GetItem(slot);
                    if (PItem != nullptr && PItem->getID() == bot.foodId)
                    {
                        owe(bot, PItem);
                    }
                }
            }

            bot.foodId = newFood;
            db::preparedStmt("UPDATE bot_players SET food = ? WHERE charid = ?", newFood, PBot->id);

            PBot->StatusEffectContainer->DelStatusEffect(xi::StatusEffect::Food);
        }

        if (newFood != 0 || moreFood)
        {
            bot.eatNow        = true;
            bot.nextFoodCheck = now;
        }

        returnItems(bot, now);
    }

    // Hands the items in Bot::giveBack to the player the bot traded with. What they can't take
    // right now (inventory full, a rare item they have, another zone) is offered again later.
    void returnItems(Bot& bot, const timer::time_point now)
    {
        auto* PBot       = bot.PChar.get();
        auto* PInventory = PBot->getStorage(LOC_INVENTORY);

        bot.nextReturn = now + kReturnRetry;

        auto* PPlayer = zoneutils::GetChar(bot.traderId);
        if (PPlayer == nullptr || PPlayer->loc.zone != PBot->loc.zone || PPlayer->status == xi::Status::Disappear || isAiBot(PPlayer->id))
        {
            return;
        }

        std::erase_if(bot.giveBack, [&](const Owed& owed)
                      {
                          auto* PItem = PInventory->GetItem(owed.slot);

                          // Gone, or back in use
                          if (PItem == nullptr || PItem->getID() != owed.itemId || PItem->isBusy())
                          {
                              return true;
                          }

                          return giveTo(PBot, PPlayer, PItem);
                      });
    }

    // Moves a whole stack from the bot's inventory to the player's. The player gets the usual
    // "Obtained" message.
    auto giveTo(CCharEntity* PBot, CCharEntity* PPlayer, CItem* PItem) const -> bool
    {
        const auto itemId   = PItem->getID();
        const auto quantity = PItem->getQuantity();
        const auto slot     = PItem->getSlotID();

        // A copy, so augments and signatures survive (as a trade between players does)
        auto copy = xi::items::clone(*PItem);
        copy->setQuantity(quantity);

        // Rolled back by its destructor unless committed
        auto taken = ItemClaimTransaction::start(PBot);
        if (!taken || !taken->take(LOC_INVENTORY, slot, quantity))
        {
            return false;
        }

        auto given = ItemClaimTransaction::start(PPlayer);
        if (!given || !given->give(LOC_INVENTORY, std::move(copy)).has_value() || !given->commit())
        {
            return false;
        }

        if (!taken->commit())
        {
            ShowErrorFmt("bot_players: {} gave item {} to {} but kept its own", PBot->getName(), itemId, PPlayer->getName());
        }

        ShowInfoFmt("bot_players: {} gives item {} (x{}) back to {}", PBot->getName(), itemId, quantity, PPlayer->getName());
        return true;
    }

    // Eats its food when it has no food effect. Returns true when it started eating.
    auto eat(Bot& bot) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (PBot->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::Food))
        {
            bot.eatNow = false;
            return false;
        }

        const auto slot = PBot->getStorage(LOC_INVENTORY)->SearchItem(bot.foodId);
        if (slot == ERROR_SLOTID)
        {
            // None left. More of the same handed over later picks this up again.
            bot.eatNow = false;
            return false;
        }

        standUp(bot);
        stop(bot);

        if (!PBot->PAI->UseItem(EntityId(PBot), LOC_INVENTORY, slot))
        {
            // Can't act right now; tried again at the next check
            return false;
        }

        ShowInfoFmt("bot_players: {} eats item {}", PBot->getName(), bot.foodId);
        return true;
    }

    //
    // Combat
    //

    // One decision for a bot in a party a player leads: walks the rule list of its job
    // (bot_roles.h); the first rule that finds something to do wins. Returns false when the
    // bot should follow the leader instead.
    auto assist(Bot& bot, CCharEntity* PLeader, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (PLeader->loc.zone != PBot->loc.zone || PLeader->status != xi::Status::Normal)
        {
            bot.inFight = false;
            calm(bot);
            return false;
        }

        if (bot.kitDirty)
        {
            buildKit(bot);
        }

        const auto& role = *bot.role;

        auto* PMob  = fightTarget(bot, PLeader);
        bot.inFight = PMob != nullptr;

        const bool reach    = PMob != nullptr && isWithinDistance(PBot->loc.p, PMob->loc.p, kAssistRange);
        const bool attacked = reach && isAttacked(PBot, PMob);

        if (PMob == nullptr)
        {
            bot.foeId = 0;
        }
        else if (attacked && bot.foeId != PMob->id)
        {
            bot.foeId    = PMob->id;
            bot.foeSince = now;
        }

        const bool nukeNow = attacked && bot.foeId == PMob->id && now >= bot.foeSince + kNukeHold;

        bool decide = false;
        bool tick   = false; // A decision is due, also for a bot without MP (ninjutsu)
        if (now >= bot.nextDecision)
        {
            bot.nextDecision = now + kDecisionInterval;
            decide           = canCast(bot, now);
            tick             = true;
        }

        // On its way to a patient that is out of cast range: nothing else until it gets there
        if (walkToPatient(bot, now))
        {
            return true;
        }

        for (const auto& rule : role.rules)
        {
            if (PBot->GetMLevel() > rule.maxLevel || (rule.minMpp > 0 && PBot->GetMPP() < rule.minMpp))
            {
                continue;
            }

            switch (rule.act)
            {
                case bots::Act::Heal:
                    if (decide && tryHeal(bot, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Cleanse:
                    if (decide && tryCleanse(bot, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Song:
                    if (decide && trySong(bot, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Buff:
                    if (decide && tryBuff(bot, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Debuff:
                    if (decide && attacked && tryDebuff(bot, PMob, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Nuke:
                    if (decide && nukeNow && tryNuke(bot, PMob, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Shadows:
                    if (tick && tryShadows(bot, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Provoke:
                    if (reach && tryProvoke(bot, PMob, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Steal:
                    if (reach && trySteal(bot, PMob, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Skillchain:
                    if (reach && trySkillchain(bot, PMob, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Shoot:
                    if (reach && tryShoot(bot, PMob, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Melee:
                    // A mage waits until somebody else has the enemy's attention
                    if (reach && (!role.mage || attacked) && fight(bot, PMob, now))
                    {
                        return true;
                    }
                    break;
                case bots::Act::Rest:
                    // Sitting down is idle()'s job, once the bot stands still
                    break;
            }
        }

        if (PBot->PAI->IsEngaged())
        {
            PBot->PAI->Disengage();
        }

        // A mage that isn't meleeing stays back instead of following the leader into the fight
        if (role.mage && reach)
        {
            bot.state = AiState::Fight;

            if (!keepDistance(bot, PMob, PLeader, now))
            {
                idle(bot, PLeader, now);
            }

            return true;
        }

        bot.spacing = 0;
        return false;
    }

    // A healer whose patient is out of cast range walks to them (set by tryHeal and
    // tryCleanse), and so does a bard to the members who lack a song (trySong). Returns true
    // while it is walking.
    auto walkToPatient(Bot& bot, const timer::time_point now) -> bool
    {
        if (bot.patientId == 0)
        {
            return false;
        }

        auto* PBot = bot.PChar.get();

        CBattleEntity* PPatient = nullptr;

        if (now < bot.patientUntil && PBot->PParty != nullptr && !PBot->PAI->IsCurrentState<CMagicState>())
        {
            for (auto* PMember : PBot->PParty->members)
            {
                if (PMember->id == bot.patientId && !PMember->isDead() && PMember->loc.zone == PBot->loc.zone)
                {
                    PPatient = PMember;
                    break;
                }
            }
        }

        if (PPatient == nullptr || isWithinDistance(PBot->loc.p, PPatient->loc.p, bot.patientReach - 2.0f))
        {
            if (PPatient != nullptr)
            {
                // There: cast on the next tick, before following or spacing moves it away
                bot.nextDecision = now;
            }

            bot.patientId = 0;
            return false;
        }

        auto* PPath = PBot->PAI->PathFind.get();

        if (now >= bot.nextRepath && (!PPath->IsFollowingPath() || !isWithinDistance(bot.pathGoal, PPatient->loc.p, kTargetDrift)))
        {
            bot.nextRepath = now + kRepathInterval;

            if (!PPath->PathAround(PPatient->loc.p, std::max(bot.patientReach - 4.0f, 1.0f), PATHFLAG_RUN))
            {
                bot.patientId = 0;
                return false;
            }

            bot.pathGoal = PPatient->loc.p;
        }

        bot.state   = AiState::Fight;
        bot.spacing = 0;
        bot.still   = false;
        standUp(bot);
        step(bot, now);
        return true;
    }

    // A mage in a fight it is not meleeing in: stays about kMageHold yalms from the enemy.
    // Returns true while it is walking there.
    auto keepDistance(Bot& bot, CBattleEntity* PMob, const CCharEntity* PLeader, const timer::time_point now) -> bool
    {
        auto* PBot  = bot.PChar.get();
        auto* PPath = PBot->PAI->PathFind.get();

        const float gap    = distance(PBot->loc.p, PMob->loc.p);
        const bool  strays = !isWithinDistance(PBot->loc.p, PLeader->loc.p, kMageLeash);

        // On its way: stop once it is far (or near) enough
        if (bot.spacing != 0 && PPath->IsFollowingPath())
        {
            if (bot.spacing > 0 ? (gap <= kMageHold && !strays) : gap >= kMageHold)
            {
                stop(bot);
                bot.spacing = 0;
                return false;
            }

            step(bot, now);
            return true;
        }

        bot.spacing = 0;

        const bool tooFar = gap > kCastRange || strays;

        // Backing off from an enemy that is after the mage itself would only drag it around
        // Once per enemy, as the fight starts
        const bool tooNear = gap < kMageNear && PMob->GetBattleTarget() != PBot && bot.spacedFor != PMob->id;

        // In cast range of this enemy: whatever happens next, it has had its one try
        if (!tooFar)
        {
            bot.spacedFor = PMob->id;
        }

        if ((!tooFar && !tooNear) || now < bot.nextRepath)
        {
            stop(bot);
            return false;
        }

        bot.nextRepath = now + kRepathInterval;

        bool found = false;

        if (tooFar)
        {
            // Back into cast range of the enemy; to the leader when that is who it lost, or
            // when no way to the enemy is found
            found = (gap > kCastRange && PPath->PathAround(PMob->loc.p, kMageHold - 1.0f, PATHFLAG_RUN)) ||
                    PPath->PathAround(PLeader->loc.p, 8.0f, PATHFLAG_RUN);

            if (found)
            {
                bot.pathFails = 0;
            }
            else if (++bot.pathFails >= kMaxPathFails)
            {
                ShowDebugFmt("bot_players: {} finds no way back to the fight, warps to the leader", PBot->getName());
                PPath->WarpTo(PLeader->loc.p, 8.0f);
                bot.pathFails = 0;
            }
        }
        else if (gap > 0.1f)
        {
            // Straight away from the enemy, else at an angle to either side
            const float dx = (PBot->loc.p.x - PMob->loc.p.x) / gap;
            const float dz = (PBot->loc.p.z - PMob->loc.p.z) / gap;

            for (const float turn : { 0.0f, 0.7f, -0.7f })
            {
                position_t spot = PBot->loc.p;
                spot.x          = PMob->loc.p.x + (dx * std::cos(turn) - dz * std::sin(turn)) * (kMageHold + 0.5f);
                spot.z          = PMob->loc.p.z + (dx * std::sin(turn) + dz * std::cos(turn)) * (kMageHold + 0.5f);

                if (PBot->loc.zone->navMesh()->validPosition(spot) && PPath->PathTo(spot, PATHFLAG_RUN))
                {
                    found = true;
                    break;
                }
            }
        }

        if (!found)
        {
            stop(bot);
            return false;
        }

        bot.spacing = tooFar ? 1 : -1;
        bot.still   = false;
        standUp(bot);
        step(bot, now);
        return true;
    }

    // The monster the party is fighting: the leader's target, else another player's. Other
    // bots don't count, or two bots would keep each other in a fight the leader has left.
    auto fightTarget(const Bot& bot, CCharEntity* PLeader) const -> CBattleEntity*
    {
        const auto targetOf = [](CBattleEntity* PMember) -> CBattleEntity*
        {
            if (!PMember->PAI->IsEngaged())
            {
                return nullptr;
            }

            auto* PTarget = PMember->GetBattleTarget();
            return PTarget != nullptr && PTarget->objtype == TYPE_MOB && !PTarget->isDead() ? PTarget : nullptr;
        };

        if (auto* PTarget = targetOf(PLeader))
        {
            return PTarget;
        }

        const auto* PBot = bot.PChar.get();

        for (auto* PMember : PBot->PParty->members)
        {
            if (PMember == PLeader || PMember->loc.zone != PBot->loc.zone || isAiBot(PMember->id))
            {
                continue;
            }

            if (auto* PTarget = targetOf(PMember))
            {
                return PTarget;
            }
        }

        return nullptr;
    }

    // "Already attacked by a party member": somebody in the party is on the monster's enmity
    // list, or the party has claim on it.
    auto isAttacked(CCharEntity* PBot, CBattleEntity* PMob) const -> bool
    {
        auto* PEnmity = static_cast<CMobEntity*>(PMob)->PEnmityContainer;

        if (PEnmity != nullptr)
        {
            for (const auto* PMember : PBot->PParty->members)
            {
                if (PEnmity->HasID(PMember->id))
                {
                    return true;
                }
            }
        }

        return battleutils::HasClaim(PBot, PMob);
    }

    // Draws the weapon on the target. Returns false while that is not possible.
    auto engage(Bot& bot, CBattleEntity* PTarget, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

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

        return true;
    }

    // Draws the weapon on the target and walks up to it. Returns false while that is not
    // possible.
    auto fight(Bot& bot, CBattleEntity* PTarget, const timer::time_point now) -> bool
    {
        if (!engage(bot, PTarget, now))
        {
            return false;
        }

        bot.state = AiState::Fight;
        melee(bot, PTarget, now);
        return true;
    }

    //
    // Provoke and skillchains
    //

    void showAction(Bot& bot, const std::string& what, const CBaseEntity* PTarget, const timer::time_point now)
    {
        bot.action      = fmt::format("{} > {}", what, PTarget->getName());
        bot.actionUntil = now + kActionShown;

        ShowDebugFmt("bot_players: {} uses {} on {}", bot.PChar->getName(), what, PTarget->getName());
    }

    // The Provoke rule: when the enemy is attacking a party member who is not a tank, a bot
    // that has Provoke (a Warrior, or a Warrior subjob) pulls it onto itself.
    auto tryProvoke(Bot& bot, CBattleEntity* PMob, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (now < bot.nextProvokeCheck)
        {
            return false;
        }

        bot.nextProvokeCheck = now + kProvokeCheck;

        const auto* PProvoke = ability::GetAbility(ABILITY_PROVOKE);
        if (PProvoke == nullptr || !charutils::hasAbility(PBot, ABILITY_PROVOKE))
        {
            return false;
        }

        const auto* PVictim = PMob->GetBattleTarget();
        if (PVictim == nullptr || PVictim == PBot || PVictim->objtype != TYPE_PC || bots::isTank(PVictim->GetMJob()))
        {
            return false;
        }

        if (static_cast<const CCharEntity*>(PVictim)->PParty != PBot->PParty)
        {
            return false;
        }

        // Somebody else just did
        if (provoked_.mobId == PMob->id && now < provoked_.until)
        {
            return false;
        }

        if (PBot->PRecastContainer->HasRecast(xi::RecastType::Ability, PProvoke->getRecastId(), PProvoke->getRecastTime()))
        {
            return false;
        }

        if (distance(PBot->loc.p, PMob->loc.p) > PProvoke->getRange() + PBot->modelHitboxSize + PMob->modelHitboxSize - 1.0f)
        {
            return false;
        }

        standUp(bot);

        if (!PBot->PAI->Ability(EntityId(PMob), ABILITY_PROVOKE))
        {
            return false;
        }

        provoked_ = Claim{ PMob->id, now + kProvokeShare };
        showAction(bot, "provoke", PMob, now);
        return true;
    }

    // The Steal rule: a bot that has Steal uses it on the enemy it is fighting whenever the
    // recast is up. What it steals goes to the party leader (collectStolen).
    auto trySteal(Bot& bot, CBattleEntity* PMob, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (now < bot.nextStealCheck)
        {
            return false;
        }

        bot.nextStealCheck = now + kStealCheck;

        const auto* PSteal = ability::GetAbility(ABILITY_STEAL);
        if (PSteal == nullptr || !charutils::hasAbility(PBot, ABILITY_STEAL))
        {
            return false;
        }

        if (!PBot->PAI->IsEngaged() || PBot->GetBattleTarget() != PMob)
        {
            return false;
        }

        if (PBot->PRecastContainer->HasRecast(xi::RecastType::Ability, PSteal->getRecastId(), PSteal->getRecastTime()))
        {
            return false;
        }

        if (distance(PBot->loc.p, PMob->loc.p) > PSteal->getRange() + PBot->modelHitboxSize + PMob->modelHitboxSize - 1.0f)
        {
            return false;
        }

        auto* PInventory = PBot->getStorage(LOC_INVENTORY);
        if (PInventory->GetFreeSlotsCount() == 0)
        {
            return false;
        }

        if (!bot.stolenPending)
        {
            bot.slotsBeforeSteal.reset();
            for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
            {
                bot.slotsBeforeSteal.set(slot, PInventory->GetItem(slot) != nullptr);
            }
        }

        if (!PBot->PAI->Ability(EntityId(PMob), ABILITY_STEAL))
        {
            return false;
        }

        bot.stolenPending = true;
        bot.stolenCheckAt = now + kStolenCheck;
        showAction(bot, "steal", PMob, now);
        return true;
    }

    // A moment after Steal: whatever is new in the inventory goes to the party leader, through
    // the queue that hands traded items back.
    void collectStolen(Bot& bot, const timer::time_point now)
    {
        auto* PBot = bot.PChar.get();

        bot.stolenPending = false;

        const auto* PLeader = PBot->PParty != nullptr ? partyLeaderOf(PBot) : nullptr;
        if (PLeader == nullptr || PLeader == PBot || isAiBot(PLeader->id))
        {
            return;
        }

        // The queue has one recipient: leave it alone while it owes somebody else something
        if (!bot.giveBack.empty() && bot.traderId != PLeader->id)
        {
            return;
        }

        auto* PInventory = PBot->getStorage(LOC_INVENTORY);

        for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
        {
            const auto* PItem = PInventory->GetItem(slot);

            if (PItem != nullptr && !bot.slotsBeforeSteal.test(slot) && !isWorn(PBot, PItem))
            {
                bot.traderId = PLeader->id;
                owe(bot, PItem);
                bot.nextReturn = now;

                ShowInfoFmt("bot_players: {} stole item {}", PBot->getName(), PItem->getID());
            }
        }
    }

    //
    // Orders from a player: !ja and !ws (modules/bots/lua/bot_orders.lua)
    //

    // "Fast Blade", "fast_blade" and "FAST BLADE" are the same name
    static auto plainName(const std::string& name) -> std::string
    {
        std::string plain;
        plain.reserve(name.size());

        for (const char c : name)
        {
            plain.push_back(c == '_' ? ' ' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }

        return plain;
    }

    // Whether `text` is this name, or starts with it followed by a space (a player may type
    // more after the name, e.g. the target)
    static auto startsWithName(const std::string& text, const std::string& name) -> bool
    {
        return !name.empty() && text.compare(0, name.size(), name) == 0 && (text.size() == name.size() || text[name.size()] == ' ');
    }

    void tellPlayer(const uint32 playerId, const std::string& message) const
    {
        auto* PPlayer = zoneutils::GetChar(playerId);

        if (PPlayer != nullptr && luautils::detail::findGlobalLuaFunction("xi.botOrders.tell").valid())
        {
            luautils::callGlobal<void>("xi.botOrders.tell", PPlayer, message);
        }
    }

    // Checks an order and stores it on the bot; think() carries it out. Returns what to tell
    // the player.
    auto giveOrder(CCharEntity* PPlayer, const std::string& botName, const std::string& what, const Order::Kind kind) -> std::string
    {
        const auto wantedBot = plainName(botName);

        Bot* PFound = nullptr;
        for (auto& [charid, bot] : live_)
        {
            if (bot.PChar != nullptr && plainName(bot.PChar->getName()) == wantedBot)
            {
                PFound = &bot;
                break;
            }
        }

        if (PFound == nullptr)
        {
            return fmt::format("No bot named {} is out.", botName);
        }

        auto& bot  = *PFound;
        auto* PBot = bot.PChar.get();

        if (PBot->PParty == nullptr || PBot->PParty != PPlayer->PParty)
        {
            return fmt::format("{} is not in your party.", PBot->getName());
        }

        if (PBot->loc.zone != PPlayer->loc.zone || PBot->isDead())
        {
            return fmt::format("{} can't do that right now.", PBot->getName());
        }

        const auto text = plainName(what);
        if (text.empty())
        {
            return kind == Order::Kind::WeaponSkill ? "!ws <bot> \"<weapon skill>\"" : kind == Order::Kind::Spell ? "!ma <bot> \"<spell or song>\"" : "!ja <bot> \"<job ability>\"";
        }

        auto* PCursor = dynamic_cast<CBattleEntity*>(PPlayer->GetEntity(PPlayer->m_TargID));

        Order order;
        order.kind     = kind;
        order.playerId = PPlayer->id;
        order.until    = timer::now() + kOrderLife;

        if (kind == Order::Kind::Spell)
        {
            return giveSpellOrder(bot, PPlayer, PCursor, what, order);
        }

        CBattleEntity* PTarget = nullptr;

        if (kind == Order::Kind::Ability)
        {
            // The longest name that fits, among the abilities the bot has; failing that among
            // all of them, to say it does not have it
            CAbility* PAbility = nullptr;
            CAbility* PUnknown = nullptr;
            size_t    best     = 0;

            for (uint16 id = 1; id < MAX_ABILITY_ID; ++id)
            {
                auto* PCandidate = ability::GetAbility(id);
                if (PCandidate == nullptr)
                {
                    continue;
                }

                const auto name = plainName(PCandidate->getName());
                if (!startsWithName(text, name))
                {
                    continue;
                }

                if (!charutils::hasAbility(PBot, id))
                {
                    PUnknown = PCandidate;
                }
                else if (name.size() > best)
                {
                    best     = name.size();
                    PAbility = PCandidate;
                }
            }

            if (PAbility == nullptr)
            {
                return PUnknown != nullptr ? fmt::format("{} does not have {}.", PBot->getName(), plainName(PUnknown->getName()))
                                           : fmt::format("No job ability called {}.", what);
            }

            order.id   = PAbility->getID();
            order.name = plainName(PAbility->getName());

            if (PBot->PRecastContainer->HasRecast(xi::RecastType::Ability, PAbility->getRecastId(), PAbility->getRecastTime()))
            {
                int64 left = 0;

                if (const auto* PRecast = PBot->PRecastContainer->GetRecast(xi::RecastType::Ability, PAbility->getRecastId()))
                {
                    auto remaining = PRecast->TimeStamp - timer::now() + PRecast->RecastTime;
                    if (PRecast->maxCharges > 1)
                    {
                        remaining -= PRecast->chargeTime * (PRecast->maxCharges - 1);
                    }

                    left = std::max<int64>(timer::count_seconds(std::chrono::ceil<std::chrono::seconds>(remaining)), 0);
                }

                return fmt::format("{} can use {} again in {} s.", PBot->getName(), order.name, left);
            }

            // The player's target when the ability can be used on it, else the bot itself
            const auto valid = PAbility->getValidTarget();

            if (PCursor != nullptr && PCursor != PBot && PCursor->ValidTarget(PBot, valid))
            {
                PTarget = PCursor;
            }
            else if ((valid & TARGET_SELF) != 0)
            {
                PTarget = PBot;
            }
            else
            {
                return fmt::format("Target something {} can be used on.", order.name);
            }
        }
        else
        {
            CWeaponSkill* PSkill   = nullptr;
            CWeaponSkill* PUnknown = nullptr;
            size_t        best     = 0;

            for (uint16 id = 1; id < MAX_WEAPONSKILL_ID; ++id)
            {
                auto* PCandidate = battleutils::GetWeaponSkill(id);
                if (PCandidate == nullptr)
                {
                    continue;
                }

                const auto name = plainName(PCandidate->getName());
                if (!startsWithName(text, name))
                {
                    continue;
                }

                if (!charutils::hasWeaponSkill(PBot, id) || !charutils::canUseWeaponSkill(PBot, id))
                {
                    PUnknown = PCandidate;
                }
                else if (name.size() > best)
                {
                    best   = name.size();
                    PSkill = PCandidate;
                }
            }

            if (PSkill == nullptr)
            {
                return PUnknown != nullptr ? fmt::format("{} can't use {} (not learned, or not with this weapon).", PBot->getName(), plainName(PUnknown->getName()))
                                           : fmt::format("No weapon skill called {}.", what);
            }

            order.id   = PSkill->getID();
            order.name = plainName(PSkill->getName());

            if (PBot->health.tp < kWeaponSkillTp)
            {
                return fmt::format("{} has {} TP, {} needs 1000.", PBot->getName(), PBot->health.tp, order.name);
            }

            // The player's target, else what the bot is fighting
            if (PCursor != nullptr && PCursor->objtype == TYPE_MOB && PCursor->isAlive() && PCursor->ValidTarget(PBot, TARGET_ENEMY))
            {
                PTarget = PCursor;
            }
            else if (PBot->PAI->IsEngaged() && PBot->GetBattleTarget() != nullptr)
            {
                PTarget = PBot->GetBattleTarget();
            }
            else
            {
                return fmt::format("Target an enemy for {}.", order.name);
            }
        }

        order.targetId = PTarget->id;
        order.targid   = PTarget->targid;
        bot.order      = order;

        ShowInfoFmt("bot_players: {} orders {} to use {} on {}", PPlayer->getName(), PBot->getName(), order.name, PTarget->getName());
        return fmt::format("{}: {} > {}", PBot->getName(), order.name, PTarget->getName());
    }

    // "Army's Paeon" is armys_paeon in spell_list
    static auto plainSpellName(const std::string& name) -> std::string
    {
        auto plain = plainName(name);
        std::erase(plain, '\'');
        std::erase(plain, ':'); // "Utsusemi: Ichi" is utsusemi_ichi
        return plain;
    }

    // How many songs of one bard stay on a target: two with an instrument, one without
    // (CLuaBaseEntity::addBardSong)
    static auto songLimit(CCharEntity* PBot) -> size_t
    {
        const auto* PItem      = dynamic_cast<CItemWeapon*>(PBot->getEquip(SLOT_RANGED));
        const bool  instrument = PItem != nullptr && (PItem->getSkillType() == xi::SkillType::StringInstrument || PItem->getSkillType() == xi::SkillType::WindInstrument);

        return static_cast<size_t>(std::max<int32>(1, (instrument ? 2 : 1) + PBot->getMod(xi::Mod::MAXIMUM_SONGS_BONUS)));
    }

    // The songs the bot keeps up, for messages
    static auto songList(const Bot& bot) -> std::string
    {
        std::string names;

        for (const auto& song : bot.songs)
        {
            if (auto* PSpell = spell::GetSpell(static_cast<SpellID>(song.spellId)))
            {
                names += (names.empty() ? "" : ", ") + plainName(PSpell->getName());
            }
        }

        return names;
    }

    // !ma: a spell or a song. A song for the party is also added to the songs the bard keeps
    // up; "stop" empties that list.
    auto giveSpellOrder(Bot& bot, CCharEntity* PPlayer, CBattleEntity* PCursor, const std::string& what, Order order) -> std::string
    {
        auto*      PBot = bot.PChar.get();
        const auto text = plainSpellName(what);

        if (startsWithName(text, "stop"))
        {
            if (bot.songs.empty())
            {
                return fmt::format("{} is not keeping up any song.", PBot->getName());
            }

            bot.songs.clear();
            return fmt::format("{} stops singing.", PBot->getName());
        }

        if (bot.kitDirty)
        {
            // Also learns the spells of its level, when that is turned on
            buildKit(bot);
        }

        // The longest name that fits. A longer name the bot can't cast wins over a shorter
        // one it can: "cure iii" is not answered with Cure.
        CSpell* PSpell   = nullptr;
        CSpell* PUnknown = nullptr;
        size_t  best     = 0;
        size_t  unknown  = 0;

        for (uint16 id = 1; id < MAX_SPELL_ID; ++id)
        {
            auto* PCandidate = spell::GetSpell(static_cast<SpellID>(id));
            if (PCandidate == nullptr || PCandidate->getSpellGroup() == SPELLGROUP_TRUST)
            {
                continue;
            }

            const auto name = plainSpellName(PCandidate->getName());
            if (!startsWithName(text, name))
            {
                continue;
            }

            if (!charutils::hasSpell(PBot, id) || !spell::CanUseSpell(PBot, PCandidate))
            {
                if (name.size() > unknown)
                {
                    unknown  = name.size();
                    PUnknown = PCandidate;
                }
            }
            else if (name.size() > best)
            {
                best   = name.size();
                PSpell = PCandidate;
            }
        }

        if (PSpell == nullptr || unknown > best)
        {
            return PUnknown != nullptr ? fmt::format("{} can't cast {} (not learned, or not at its level).", PBot->getName(), plainName(PUnknown->getName()))
                                       : fmt::format("No spell called {}.", what);
        }

        const auto spellId = static_cast<uint16>(PSpell->getID());

        order.id    = spellId;
        order.name  = plainName(PSpell->getName());
        order.until = timer::now() + kSpellOrderLife;

        if (PBot->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }))
        {
            return fmt::format("{} is silenced.", PBot->getName());
        }

        if (isNinjutsu(PSpell))
        {
            if (!battleutils::HasNinjaTool(PBot, PSpell, false))
            {
                return fmt::format("{} has no ninja tool for {}.", PBot->getName(), order.name);
            }
        }
        else if (PBot->health.mp < PSpell->getMPCost())
        {
            return fmt::format("{} has {} MP, {} needs {}.", PBot->getName(), PBot->health.mp, order.name, PSpell->getMPCost());
        }

        // A song sung for the party goes on the bard and reaches who is around it
        const auto valid  = PSpell->getValidTarget();
        const auto effect = PSpell->getSpellGroup() == SPELLGROUP_SONG ? bots::songEffect(spellId) : xi::StatusEffect::None;
        const bool area   = effect != xi::StatusEffect::None && PSpell->getAOE() != SPELLAOE_NONE;

        // Else the player's target when the spell can be cast on it, the player, the bot
        // itself, or what the party is fighting
        CBattleEntity* PTarget = nullptr;

        if (area)
        {
            PTarget = PBot;
        }
        else if (PCursor != nullptr && PCursor != PBot && PCursor->ValidTarget(PBot, valid))
        {
            PTarget = PCursor;
        }
        else if (PCursor == PBot && (valid & TARGET_SELF) != 0)
        {
            PTarget = PBot;
        }
        else if (PPlayer->ValidTarget(PBot, valid))
        {
            PTarget = PPlayer;
        }
        else if ((valid & TARGET_SELF) != 0)
        {
            PTarget = PBot;
        }
        else if ((valid & TARGET_ENEMY) != 0)
        {
            if (auto* PLeader = partyLeaderOf(PBot))
            {
                PTarget = fightTarget(bot, PLeader);
            }
        }

        if (PTarget == nullptr)
        {
            return fmt::format("Target something {} can be cast on.", order.name);
        }

        std::string keeping;

        if (effect != xi::StatusEffect::None)
        {
            // One song per effect, and no more songs than stay on a target
            std::erase_if(bot.songs, [effect](const Song& song)
            {
                return song.effect == effect;
            });

            const auto limit = songLimit(PBot);
            while (bot.songs.size() >= limit)
            {
                bot.songs.erase(bot.songs.begin());
            }

            bot.songs.emplace_back(Song{ spellId, effect, area ? 0 : PTarget->id });
            keeping = fmt::format(" Keeping up: {}.", songList(bot));
        }

        int64 wait = 0;

        if (const auto* PRecast = PBot->PRecastContainer->GetRecast(xi::RecastType::Magic, static_cast<xi::Recast>(spellId)))
        {
            wait = std::max<int64>(timer::count_seconds(std::chrono::ceil<std::chrono::seconds>(PRecast->TimeStamp - timer::now() + PRecast->RecastTime)), 0);
        }

        if (wait > 0)
        {
            // Not an order then: a song it keeps up is sung as soon as it is ready
            return effect != xi::StatusEffect::None ? fmt::format("{} can sing {} again in {} s.{}", PBot->getName(), order.name, wait, keeping)
                                                    : fmt::format("{} can cast {} again in {} s.", PBot->getName(), order.name, wait);
        }

        order.targetId = PTarget->id;
        order.targid   = PTarget->targid;
        bot.order      = order;

        ShowInfoFmt("bot_players: {} orders {} to cast {} on {}", PPlayer->getName(), PBot->getName(), order.name, PTarget->getName());
        return fmt::format("{}: {} > {}.{}", PBot->getName(), order.name, PTarget->getName(), keeping);
    }

    void dropOrder(Bot& bot, const std::string& why)
    {
        tellPlayer(bot.order.playerId, fmt::format("{} did not use {}: {}", bot.PChar->getName(), bot.order.name, why));
        bot.order = Order{};
    }

    // Carries out the order a player gave, as soon as the bot can act. Returns true while the
    // order has the bot (the job rules wait).
    auto carryOut(Bot& bot, const timer::time_point now) -> bool
    {
        auto* PBot  = bot.PChar.get();
        auto& order = bot.order;

        if (now >= order.until)
        {
            dropOrder(bot, "it could not in time.");
            return false;
        }

        auto* PTarget = order.targetId == PBot->id ? static_cast<CBattleEntity*>(PBot) : dynamic_cast<CBattleEntity*>(PBot->GetEntity(order.targid));
        if (PTarget == nullptr || PTarget->id != order.targetId || (PTarget->objtype == TYPE_MOB && PTarget->isDead()))
        {
            dropOrder(bot, "the target is gone.");
            return false;
        }

        const bool  enemy = PTarget->objtype == TYPE_MOB;
        const float gap   = distance(PBot->loc.p, PTarget->loc.p) - PBot->modelHitboxSize - PTarget->modelHitboxSize;

        bot.still = false;
        standUp(bot);

        const auto face = [&]()
        {
            stop(bot);

            if (PTarget != PBot && !facing(PBot->loc.p, PTarget->loc.p, 32))
            {
                PBot->PAI->PathFind->LookAt(PTarget->loc.p);
                PBot->updatemask |= UPDATE_POS;
            }
        };

        // Walks towards the target until it is within `range`
        const auto approach = [&](const float range)
        {
            auto* PPath = PBot->PAI->PathFind.get();

            if (now >= bot.nextRepath && (!PPath->IsFollowingPath() || !isWithinDistance(bot.pathGoal, PTarget->loc.p, kTargetDrift)))
            {
                bot.nextRepath = now + kRepathInterval;

                if (PPath->PathAround(PTarget->loc.p, std::max(range, 1.0f), PATHFLAG_RUN))
                {
                    bot.pathGoal = PTarget->loc.p;
                }
            }

            step(bot, now);
        };

        if (order.kind == Order::Kind::Ability)
        {
            const auto* PAbility = ability::GetAbility(order.id);
            if (PAbility == nullptr)
            {
                bot.order = Order{};
                return false;
            }

            if (PTarget != PBot && gap > PAbility->getRange() - 1.0f)
            {
                approach(PAbility->getRange() - 1.0f);
                return true;
            }

            face();

            if (PBot->PAI->Ability(EntityId(PTarget), order.id))
            {
                showAction(bot, order.name, PTarget, now);
                bot.order = Order{};
            }

            return true;
        }

        if (order.kind == Order::Kind::Spell)
        {
            auto* PSpell = spell::GetSpell(static_cast<SpellID>(order.id));
            if (PSpell == nullptr)
            {
                bot.order = Order{};
                return false;
            }

            if (PBot->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }))
            {
                dropOrder(bot, "it is silenced.");
                return false;
            }

            if (isNinjutsu(PSpell))
            {
                if (!battleutils::HasNinjaTool(PBot, PSpell, false))
                {
                    dropOrder(bot, "no ninja tool for it.");
                    return false;
                }
            }
            else if (PBot->health.mp < PSpell->getMPCost())
            {
                dropOrder(bot, "not enough MP.");
                return false;
            }

            const float range = std::min(PSpell->getRange(), kCastRange);

            if (PTarget != PBot && !isWithinDistance(PBot->loc.p, PTarget->loc.p, range - 1.0f))
            {
                approach(range - 3.0f);
                return true;
            }

            // Between two spells, or unable to act: wait
            if (now < bot.nextCast || now < bot.unableUntil || PBot->StatusEffectContainer->HasPreventActionEffect() ||
                PBot->PRecastContainer->HasRecast(xi::RecastType::Magic, static_cast<xi::Recast>(order.id), 0s))
            {
                stop(bot);
                return true;
            }

            face();

            if (cast(bot, PTarget, PSpell, now))
            {
                if (const auto effect = bots::songEffect(order.id); effect != xi::StatusEffect::None)
                {
                    remember(PBot->id, effect, now + kSongRetry);
                }

                bot.order = Order{};
            }

            return true;
        }

        auto* PSkill = battleutils::GetWeaponSkill(order.id);
        if (PSkill == nullptr)
        {
            bot.order = Order{};
            return false;
        }

        if (PBot->health.tp < kWeaponSkillTp)
        {
            dropOrder(bot, "not enough TP.");
            return false;
        }

        if (enemy && !engage(bot, PTarget, now))
        {
            return true;
        }

        bot.state = AiState::Fight;

        if (gap > PSkill->getRange())
        {
            // Out of range the skill fails and the TP is gone all the same
            if (enemy && PSkill->getRange() <= PBot->GetMeleeRange(PTarget))
            {
                melee(bot, PTarget, now);
            }
            else
            {
                approach(PSkill->getRange() - 1.0f);
            }

            return true;
        }

        face();

        if (PBot->PAI->WeaponSkill(EntityId(PTarget), order.id))
        {
            // Inside an open skillchain this is the answer to its current step: no second bot
            // answers it too. Otherwise it opens one, which the others then continue.
            if (const auto* PEffect = PTarget->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Skillchain, 0))
            {
                chained_ = Claim{ PTarget->id, now + kChainShare, PEffect->GetStartTime() };
            }

            showAction(bot, order.name, PTarget, now);
            bot.order = Order{};
        }

        return true;
    }

    // The Skillchain rule. Bots keep their TP until somebody else's weapon skill has opened a
    // skillchain on the enemy (a bot never opens one), then one bot at a time answers with a
    // weapon skill that continues it, inside the window the game allows.
    auto trySkillchain(Bot& bot, CBattleEntity* PMob, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (PBot->health.tp < kWeaponSkillTp || !PBot->PAI->IsEngaged() || PBot->GetBattleTarget() != PMob)
        {
            return false;
        }

        const auto* PEffect = PMob->StatusEffectContainer->GetStatusEffect(xi::StatusEffect::Skillchain, 0);
        if (PEffect == nullptr)
        {
            return false;
        }

        // When the last weapon skill landed
        const auto step = PEffect->GetStartTime();

        if (now < step + kChainEarliest || now >= step + PEffect->GetDuration() - kChainLatest)
        {
            return false;
        }

        // Another bot is answering this step; a second skill would land too soon after it
        // and restart the chain
        if (chained_.mobId == PMob->id && chained_.step == step && now < chained_.until)
        {
            return false;
        }

        // What is open: the opener's properties (up to three, four bits each), or the one
        // property the last step made
        std::list<xi::SkillchainType> resonance;

        if (PEffect->GetTier() == 0)
        {
            for (const int shift : { 0, 4, 8 })
            {
                if (const auto property = (PEffect->GetPower() >> shift) & 0xF)
                {
                    resonance.push_back(static_cast<xi::SkillchainType>(property));
                }
            }
        }
        else
        {
            resonance.push_back(static_cast<xi::SkillchainType>(PEffect->GetPower()));
        }

        const float gap = distance(PBot->loc.p, PMob->loc.p) - PBot->modelHitboxSize - PMob->modelHitboxSize;

        // The usable weapon skill that makes the highest skillchain
        CWeaponSkill* PBest     = nullptr;
        uint8         bestChain = 0;

        for (uint16 id = 1; id < MAX_WEAPONSKILL_ID; ++id)
        {
            if (!charutils::hasWeaponSkill(PBot, id))
            {
                continue;
            }

            auto* PSkill = battleutils::GetWeaponSkill(id);
            if (PSkill == nullptr || gap > PSkill->getRange())
            {
                // Out of range the skill fails, and the TP is gone all the same
                continue;
            }

            const auto type = static_cast<xi::SkillType>(PSkill->getType());
            if ((type == xi::SkillType::Archery || type == xi::SkillType::Marksmanship) && PBot->getEquip(SLOT_AMMO) == nullptr)
            {
                continue;
            }

            std::list<xi::SkillchainType> properties;

            for (const auto property : { PSkill->getPrimarySkillchain(), PSkill->getSecondarySkillchain(), PSkill->getTertiarySkillchain() })
            {
                if (property != 0)
                {
                    properties.push_back(static_cast<xi::SkillchainType>(property));
                }
            }

            // The enum goes up with the skillchain level (level 1, 2, Light/Darkness)
            const auto chain = static_cast<uint8>(battleutils::FormSkillchain(resonance, properties));
            if (chain > bestChain)
            {
                bestChain = chain;
                PBest     = PSkill;
            }
        }

        if (PBest == nullptr)
        {
            return false;
        }

        // The game wants the user to face the target
        if (!facing(PBot->loc.p, PMob->loc.p, 32))
        {
            PBot->PAI->PathFind->LookAt(PMob->loc.p);
            PBot->updatemask |= UPDATE_POS;
        }

        stop(bot);

        if (!PBot->PAI->WeaponSkill(EntityId(PMob), PBest->getID()))
        {
            return false;
        }

        chained_ = Claim{ PMob->id, now + kChainShare, step };
        showAction(bot, PBest->getName(), PMob, now);
        return true;
    }

    //
    // Ranged attacks
    //

    // Whether the bot can shoot right now. Puts the best ammo it carries in the ammo slot when
    // that ran out, after a trade and on a level up.
    auto canShoot(Bot& bot) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (PBot->getEquip(SLOT_AMMO) == nullptr && bot.hadAmmo)
        {
            // The last one was shot
            bot.hadAmmo   = false;
            bot.ammoDirty = true;
        }

        if (bot.ammoDirty)
        {
            bot.ammoDirty = false;
            equipBestAmmo(bot);
        }

        const auto* PRanged = dynamic_cast<CItemWeapon*>(PBot->getEquip(SLOT_RANGED));
        if (PRanged != nullptr && PRanged->getSkillType() == xi::SkillType::Throwing)
        {
            // A boomerang needs no ammo
            return true;
        }

        auto* PAmmo = PBot->getEquip(SLOT_AMMO);
        if (PAmmo == nullptr || !isAmmoFor(PBot, PAmmo))
        {
            return false;
        }

        bot.hadAmmo = true;
        return true;
    }

    // Of the ammo in the inventory the bot can use with its ranged weapon, equips the one that
    // hits hardest.
    void equipBestAmmo(Bot& bot)
    {
        auto* PBot       = bot.PChar.get();
        auto* PInventory = PBot->getStorage(LOC_INVENTORY);
        auto* PWorn      = PBot->getEquip(SLOT_AMMO);

        CItemWeapon* PBest = nullptr;

        for (uint8 slot = 1; slot <= PInventory->GetSize(); ++slot)
        {
            auto* PAmmo = dynamic_cast<CItemWeapon*>(PInventory->GetItem(slot));
            if (PAmmo == nullptr || !isSpareAmmo(PBot, PAmmo))
            {
                continue;
            }

            if (PBest == nullptr || PAmmo->getDamage() > PBest->getDamage() || (PAmmo->getDamage() == PBest->getDamage() && PAmmo == PWorn))
            {
                PBest = PAmmo;
            }
        }

        if (PBest != nullptr && PBest != PWorn)
        {
            charutils::EquipItem(PBot, PBest->getSlotID(), SLOT_AMMO, LOC_INVENTORY);
            ShowInfoFmt("bot_players: {} readies ammo {} (x{})", PBot->getName(), PBest->getID(), PBest->getQuantity());
        }
    }

    // The Shoot rule: draws on the target, keeps its distance and shoots. Returns false when
    // the bot has nothing to shoot with (the Melee rule is next).
    auto tryShoot(Bot& bot, CBattleEntity* PTarget, const timer::time_point now) -> bool
    {
        auto* PBot  = bot.PChar.get();
        auto* PPath = PBot->PAI->PathFind.get();

        if (!canShoot(bot) || !engage(bot, PTarget, now))
        {
            return false;
        }

        bot.state = AiState::Fight;

        // Shots that never start: something is in the way, so close in
        const float hold = bot.shotFails >= kShotFailsClose ? kShootClose : kShootHold;

        if (!isWithinDistance(PBot->loc.p, PTarget->loc.p, hold))
        {
            const bool moving = PPath->IsFollowingPath();

            if (now >= bot.nextRepath && (!moving || !isWithinDistance(bot.pathGoal, PTarget->loc.p, kTargetDrift)))
            {
                bot.nextRepath = now + kRepathInterval;

                if (PPath->PathAround(PTarget->loc.p, hold - 1.0f, PATHFLAG_RUN))
                {
                    bot.pathGoal = PTarget->loc.p;
                }
            }

            step(bot, now);
            return true;
        }

        stop(bot);

        if (!facing(PBot->loc.p, PTarget->loc.p, 32))
        {
            PPath->LookAt(PTarget->loc.p);
            PBot->updatemask |= UPDATE_POS;
        }

        if (now >= bot.nextShot)
        {
            bot.nextShot = now + kShotGap;

            // Counted as failed until think() sees the bot aiming
            if (bot.shotFails < 250)
            {
                ++bot.shotFails;
            }

            PBot->PAI->RangedAttack(EntityId(PTarget));
        }

        return true;
    }

    // Walks into melee range of the target and faces it. The attack state (ticked by the
    // zone, like a player's) swings when the weapon delay is up.
    //
    // Each bot has its own spot around the enemy, so they don't stand on one another: the
    // side the leader fights from (else the side the bot came from) turned by the bot's
    // share of kMeleeSpread. The direction is fixed for the fight, so nobody shuffles when
    // the leader moves; the spot follows the enemy. A bot goes for its spot once each time
    // it has to walk up to the enemy, and stays wherever in range that leaves it.
    void melee(Bot& bot, const CBattleEntity* PTarget, const timer::time_point now)
    {
        auto* PBot  = bot.PChar.get();
        auto* PPath = PBot->PAI->PathFind.get();

        const float range   = PBot->GetMeleeRange(PTarget);
        const float standAt = std::max(range - kMeleeInside, 0.5f);
        const bool  inRange = isWithinDistance(PBot->loc.p, PTarget->loc.p, range - kMeleeMargin);
        const bool  moving  = PPath->IsFollowingPath();

        if (bot.meleeFor != PTarget->id)
        {
            bot.meleeFor     = PTarget->id;
            bot.meleeAimed   = false;
            bot.meleeSettled = false;

            const auto* PLeader = partyLeaderOf(PBot);
            const auto& from    = PLeader != nullptr && PLeader != PBot && PLeader->GetBattleTarget() == PTarget &&
                                          isWithinDistance(PLeader->loc.p, PTarget->loc.p, range + 3.0f)
                                      ? PLeader->loc.p
                                      : PBot->loc.p;

            const auto  slot   = followSlot(bot);
            const float turn   = kMeleeSpread * static_cast<float>(slot / 2 + 1) * (slot % 2 == 0 ? 1.0f : -1.0f);
            const float jitter = (static_cast<float>(botHash(PBot->id, 16)) / 127.5f - 1.0f) * kMeleeSpreadJitter;

            bot.meleeAngle = std::atan2(from.z - PTarget->loc.p.z, from.x - PTarget->loc.p.x) + (turn + jitter) * static_cast<float>(M_PI) / 180.0f;
        }

        if (!inRange && !moving)
        {
            // The enemy moved away (or the fight just started): walk up to it again
            bot.meleeAimed   = false;
            bot.meleeSettled = false;
        }
        else if (inRange && !moving && bot.meleeAimed)
        {
            bot.meleeSettled = true;
        }

        if (!inRange || !bot.meleeSettled)
        {
            if (now >= bot.nextRepath && (!moving || !isWithinDistance(bot.pathGoal, PTarget->loc.p, kTargetDrift)))
            {
                bot.nextRepath = now + kRepathInterval;

                position_t spot = PTarget->loc.p;
                spot.x += std::cos(bot.meleeAngle) * standAt;
                spot.z += std::sin(bot.meleeAngle) * standAt;

                // Its own spot; anywhere in range when that one can't be walked to
                if ((PBot->loc.zone->navMesh()->validPosition(spot) && PPath->PathTo(spot, PATHFLAG_RUN)) ||
                    PPath->PathAround(PTarget->loc.p, standAt, PATHFLAG_RUN))
                {
                    bot.pathGoal = PTarget->loc.p;
                }

                bot.meleeAimed = true;
            }

            step(bot, now);

            if (!inRange || PPath->IsFollowingPath())
            {
                return;
            }
        }

        stop(bot);

        if (!facing(PBot->loc.p, PTarget->loc.p, 32))
        {
            PPath->LookAt(PTarget->loc.p);
            PBot->updatemask |= UPDATE_POS;
        }
    }

    //
    // Spells
    //

    // Testing aid (custom.BOT_AUTO_LEARN_SPELLS): the bot knows every spell its job and
    // subjob have at its level, as if it had used the scrolls. Only in memory: char_spells
    // keeps what it really learned, which is all it knows again once this is turned off.
    static void learnSpells(CCharEntity* PBot)
    {
        const auto mainJob   = PBot->GetMJob();
        const auto subJob    = PBot->GetSJob();
        const auto mainLevel = PBot->GetMLevel();
        const auto subLevel  = PBot->GetSLevel();

        int32 learned = 0;

        for (uint16 id = 1; id < MAX_SPELL_ID; ++id)
        {
            auto* PSpell = spell::GetSpell(static_cast<SpellID>(id));
            if (PSpell == nullptr || PSpell->getSpellGroup() == SPELLGROUP_TRUST || charutils::hasSpell(PBot, id))
            {
                continue;
            }

            const auto mainAt = PSpell->getJob(mainJob);
            const auto subAt  = PSpell->getJob(subJob);

            if ((mainAt > 0 && mainAt <= mainLevel) || (subAt > 0 && subAt <= subLevel))
            {
                learned += charutils::addSpell(PBot, id);
            }
        }

        if (learned > 0)
        {
            ShowInfoFmt("bot_players: {} knows {} more spells (BOT_AUTO_LEARN_SPELLS)", PBot->getName(), learned);
        }
    }

    // Works out which spells of bot_roles.h the bot can cast at its level.
    void buildKit(Bot& bot)
    {
        auto* PBot = bot.PChar.get();

        if (autoLearn_)
        {
            learnSpells(PBot);
        }

        bot.kitDirty = false;
        bot.role     = &bots::roleFor(PBot->GetMJob());
        bot.kit      = Kit{};

        const auto knows = [PBot](const uint16 spellId) -> bool
        {
            auto* PSpell = spell::GetSpell(static_cast<SpellID>(spellId));
            return PSpell != nullptr && charutils::hasSpell(PBot, spellId) && spell::CanUseSpell(PBot, PSpell);
        };

        const auto highest = [&knows](const bots::Tiers& tiers) -> uint16
        {
            uint16 best = 0;

            for (const auto spellId : tiers)
            {
                if (spellId != 0 && knows(spellId))
                {
                    best = spellId;
                }
            }

            return best;
        };

        auto& kit = bot.kit;

        for (size_t tier = 0; tier < kit.cures.size(); ++tier)
        {
            const auto spellId = static_cast<uint16>(bots::kFirstCure + tier);
            if (knows(spellId))
            {
                kit.cures[tier] = spellId;
                kit.hasCure     = true;
            }
        }

        for (size_t i = 0; i < bots::kBuffs.size(); ++i)
        {
            kit.buffs[i] = highest(bots::kBuffs[i].spells);
            kit.any |= kit.buffs[i] != 0;
        }

        for (size_t i = 0; i < bots::kDebuffs.size(); ++i)
        {
            kit.debuffs[i] = highest(bots::kDebuffs[i].spells);
            kit.any |= kit.debuffs[i] != 0;
        }

        for (uint16 spellId = bots::kFirstNuke; spellId <= bots::kLastNuke; ++spellId)
        {
            if (knows(spellId))
            {
                kit.nukes.emplace_back(spellId);
            }
        }

        for (size_t i = 0; i < bots::kCleanses.size(); ++i)
        {
            kit.cleanse[i] = knows(bots::kCleanses[i].spell);
            kit.cleanses |= kit.cleanse[i];
        }

        kit.erase = knows(bots::kErase);
        kit.cleanses |= kit.erase;

        if (PBot->GetMJob() == xi::Job::BRD || PBot->GetSJob() == xi::Job::BRD)
        {
            for (uint16 spellId = bots::kFirstSong; spellId <= bots::kLastSong && !kit.songs; ++spellId)
            {
                kit.songs = knows(spellId);
            }
        }

        for (size_t i = 0; i < bots::kShadows.size(); ++i)
        {
            kit.shadows[i] = knows(bots::kShadows[i]) ? bots::kShadows[i] : 0;
        }

        // Songs it was keeping up and can't sing any more (job change, level sync)
        std::erase_if(bot.songs, [&knows](const Song& song)
        {
            return !knows(song.spellId);
        });

        kit.dispel = knows(bots::kDispel);
        kit.any |= kit.hasCure || kit.dispel || !kit.nukes.empty() || kit.cleanses || kit.songs;
    }

    // Able to start a spell right now, whatever spell
    auto canCast(const Bot& bot, const timer::time_point now) const -> bool
    {
        auto* PBot = bot.PChar.get();

        // Songs cost no MP
        return bot.kit.any && now >= bot.nextCast && now >= bot.unableUntil && (PBot->health.mp > 0 || bot.kit.songs) &&
               !PBot->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }) &&
               !PBot->StatusEffectContainer->HasPreventActionEffect();
    }

    // Ninjutsu costs a ninja tool instead of MP: spell_list.mpCost holds the tool's item id
    static auto isNinjutsu(CSpell* PSpell) -> bool
    {
        return PSpell->getSpellGroup() == SPELLGROUP_NINJUTSU;
    }

    // The spell, if `PChar` has the MP for it while keeping `reservePct` of its pool (for
    // ninjutsu: the tool in its inventory), and it is not on recast.
    static auto ready(CCharEntity* PChar, const uint16 spellId, const uint8 reservePct, const bool recastToo = true) -> CSpell*
    {
        auto* PSpell = spell::GetSpell(static_cast<SpellID>(spellId));
        if (PSpell == nullptr)
        {
            return nullptr;
        }

        if (isNinjutsu(PSpell))
        {
            if (!battleutils::HasNinjaTool(PChar, PSpell, false))
            {
                return nullptr;
            }
        }
        else
        {
            const int32 left = PChar->health.mp - PSpell->getMPCost();
            if (left < 0 || left * 100 < reservePct * PChar->GetMaxMP())
            {
                return nullptr;
            }
        }

        if (recastToo && PChar->PRecastContainer->HasRecast(xi::RecastType::Magic, static_cast<xi::Recast>(spellId), 0s))
        {
            return nullptr;
        }

        return PSpell;
    }

    // The share of its MP a bot keeps back from debuffs and nukes: that MP is for cures, so
    // only the healer roles (White Mage, Red Mage) keep it. A Black Mage spends everything,
    // also when a White Mage subjob gives it a Cure.
    static auto reserveOf(const Bot& bot, const uint8 reservePct) -> uint8
    {
        return bot.role->healRank > 0 ? reservePct : 0;
    }

    auto cast(Bot& bot, CBattleEntity* PTarget, CSpell* PSpell, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        bot.still = false;
        standUp(bot);
        stop(bot);

        if (!PBot->PAI->Cast(EntityId(PTarget), PSpell->getID()))
        {
            bot.unableUntil = now + kCastRetry;
            return false;
        }

        bot.nextCast    = now + kCastGap;
        bot.action      = fmt::format("{} > {}", PSpell->getName(), PTarget->getName());
        bot.actionUntil = now + kActionShown;

        ShowDebugFmt("bot_players: {} casts {} on {}", PBot->getName(), PSpell->getName(), PTarget->getName());
        return true;
    }

    // Calls func(caster, spell, target id) for every party member that is casting right now,
    // bots and players alike.
    template <typename F>
    static void forEachCast(const CCharEntity* PBot, F&& func)
    {
        for (auto* PMember : PBot->PParty->members)
        {
            if (PMember == PBot || PMember->loc.zone != PBot->loc.zone)
            {
                continue;
            }

            if (const auto* PState = dynamic_cast<CMagicState*>(PMember->PAI->GetCurrentState()))
            {
                if (auto* PSpell = PState->GetSpell())
                {
                    func(PMember, PSpell, PState->target().UniqueNo);
                }
            }
        }
    }

    // What a Cure of `tier` (0 = Cure) cast by `PCaster` restores
    auto cureEstimate(CBattleEntity* PCaster, const size_t tier) const -> int32
    {
        const float power   = std::floor(PCaster->MND() / 2.0f) + std::floor(PCaster->VIT() / 4.0f) + PCaster->GetSkill(xi::SkillType::HealingMagic);
        const float potency = std::min<int16>(PCaster->getMod(xi::Mod::CURE_POTENCY), 50) / 100.0f;

        return static_cast<int32>(bots::cureAmount(tier, power) * (1.0f + potency) * curePower_);
    }

    // HP the Cures other party members are casting on the patient will restore
    auto incomingHeal(const CCharEntity* PBot, const CBattleEntity* PPatient) const -> int32
    {
        int32 total = 0;

        forEachCast(PBot, [&](CBattleEntity* PCaster, CSpell* PSpell, const uint32 targetId)
        {
            const auto spellId = static_cast<uint16>(PSpell->getID());

            if (targetId == PPatient->id && spellId >= bots::kFirstCure && spellId < bots::kFirstCure + bots::kCureTiers)
            {
                total += cureEstimate(PCaster, spellId - bots::kFirstCure);
            }
        });

        return total;
    }

    // Another bot of this party that should go first. `rank` picks the role rank to compare,
    // `able` says whether that bot could do the job. Equal ranks: the lower charid first.
    template <typename Rank, typename Able>
    auto betterBot(const Bot& bot, const timer::time_point now, Rank&& rank, Able&& able) -> Bot*
    {
        const auto* PBot = bot.PChar.get();

        for (const auto* PMember : PBot->PParty->members)
        {
            if (PMember == PBot || PMember->loc.zone != PBot->loc.zone || PMember->isDead())
            {
                continue;
            }

            const auto it = live_.find(PMember->id);
            if (it == live_.end() || it->second.puppet)
            {
                continue;
            }

            auto&      other = it->second;
            const auto mine  = rank(*bot.role);
            const auto their = rank(*other.role);

            if (their < mine || (their == mine && PMember->id > PBot->id))
            {
                continue;
            }

            auto* POther = other.PChar.get();

            if (now < other.unableUntil || !isWithinDistance(PBot->loc.p, POther->loc.p, kAssistRange) ||
                POther->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }) ||
                POther->StatusEffectContainer->HasPreventActionEffect())
            {
                continue;
            }

            if (able(other))
            {
                return &other;
            }
        }

        return nullptr;
    }

    // Smart Heal: the biggest Cure that fits what a patient is missing, counting the cures
    // already on their way. The best healer takes the worst patient.
    auto tryHeal(Bot& bot, const timer::time_point now) -> bool
    {
        if (!bot.kit.hasCure)
        {
            return false;
        }

        auto* PBot = bot.PChar.get();

        // A mage that is no healer (a Black Mage with a Cure from its subjob) only cures in
        // an emergency; its MP is for nukes
        const uint8 healBelow = bot.role->mage && bot.role->healRank == 0 ? kEmergencyHpp : kHealBelowHpp;

        struct Patient
        {
            CBattleEntity* PEntity = nullptr;
            int32          need    = 0;
            uint8          hpp     = 100; // Once the cures on their way have landed
        };

        Patient worst;
        Patient second;

        for (auto* PMember : PBot->PParty->members)
        {
            if (PMember->isDead() || PMember->loc.zone != PBot->loc.zone || !isWithinDistance(PBot->loc.p, PMember->loc.p, kHealReach))
            {
                continue;
            }

            const int32 maxHp   = PMember->GetMaxHP();
            const int32 missing = maxHp - PMember->health.hp;
            if (missing <= 0 || maxHp <= 0)
            {
                continue;
            }

            const int32 need = missing - incomingHeal(PBot, PMember);
            if (need <= 0)
            {
                continue;
            }

            const auto hpp = static_cast<uint8>((maxHp - need) * 100 / maxHp);
            if (hpp >= healBelow)
            {
                continue;
            }

            const Patient patient{ PMember, need, hpp };

            if (hpp < worst.hpp)
            {
                second = worst;
                worst  = patient;
            }
            else if (hpp < second.hpp)
            {
                second = patient;
            }
        }

        if (worst.PEntity == nullptr)
        {
            return false;
        }

        const Patient* pick = &worst;

        // A better healer goes first only if it can reach the worst patient from where it is
        const auto* PBetter = betterBot(bot, now, [](const bots::Role& role) { return role.healRank; }, [&worst](const Bot& other)
        {
            if (!other.kit.hasCure || !isWithinDistance(other.PChar->loc.p, worst.PEntity->loc.p, kCastRange))
            {
                return false;
            }

            for (const auto spellId : other.kit.cures)
            {
                if (spellId != 0)
                {
                    return ready(other.PChar.get(), spellId, 0) != nullptr;
                }
            }

            return false;
        });

        // In an emergency every healer goes for the worst patient
        if (PBetter != nullptr && worst.hpp >= kEmergencyHpp)
        {
            const bool busy = PBetter->PChar->PAI->IsCurrentState<CMagicState>() || now < PBetter->nextCast;

            if (!busy)
            {
                // It takes the worst patient; help with the next one
                pick = second.PEntity != nullptr ? &second : nullptr;
            }
            else if (worst.hpp >= kHelpBelowHpp)
            {
                pick = nullptr;
            }
        }

        if (pick == nullptr)
        {
            return false;
        }

        // Out of cast range: walk to the patient first
        if (!isWithinDistance(PBot->loc.p, pick->PEntity->loc.p, kCastRange))
        {
            bot.patientId    = pick->PEntity->id;
            bot.patientReach = kCastRange;
            bot.patientUntil = now + kHealWalkLife;
            return walkToPatient(bot, now);
        }

        CSpell* PBest = nullptr;

        for (size_t tier = 0; tier < bot.kit.cures.size(); ++tier)
        {
            if (bot.kit.cures[tier] == 0)
            {
                continue;
            }

            auto* PSpell = ready(PBot, bot.kit.cures[tier], 0);
            if (PSpell == nullptr)
            {
                continue;
            }

            if (pick->hpp < kEmergencyHpp || static_cast<float>(cureEstimate(PBot, tier)) <= static_cast<float>(pick->need) * kOverheal)
            {
                PBest = PSpell;
            }
        }

        return PBest != nullptr && cast(bot, pick->PEntity, PBest, now);
    }

    // Status removal: the first ailment of bots::kCleanses a party member has and the bot has
    // the spell for, then Erase for anything else erasable. One bot per ailment: the best
    // healer that can reach the member, and nobody while the spell is already on its way.
    auto tryCleanse(Bot& bot, const timer::time_point now) -> bool
    {
        if (!bot.kit.cleanses)
        {
            return false;
        }

        auto* PBot = bot.PChar.get();

        // `ailmentOf(member)`: the ailment this spell would take off them, if any.
        // `otherHas(bot)`: whether another bot has the spell.
        const auto treat = [&](const uint16 spellId, const auto& ailmentOf, const auto& otherHas) -> bool
        {
            auto* PSpell = ready(PBot, spellId, 0);
            if (PSpell == nullptr)
            {
                return false;
            }

            for (auto* PMember : PBot->PParty->members)
            {
                if (PMember->isDead() || PMember->loc.zone != PBot->loc.zone || !isWithinDistance(PBot->loc.p, PMember->loc.p, kHealReach))
                {
                    continue;
                }

                const std::optional<xi::StatusEffect> ailment = ailmentOf(PMember);
                if (!ailment || wasTried(PMember->id, *ailment, now))
                {
                    continue;
                }

                bool covered = false;

                forEachCast(PBot, [&](CBattleEntity*, CSpell* PCasting, const uint32 targetId)
                {
                    covered |= targetId == PMember->id && static_cast<uint16>(PCasting->getID()) == spellId;
                });

                if (covered)
                {
                    continue;
                }

                const auto* PBetter = betterBot(bot, now, [](const bots::Role& role) { return role.healRank; }, [&](const Bot& other)
                {
                    return otherHas(other) && isWithinDistance(other.PChar->loc.p, PMember->loc.p, kCastRange) &&
                           ready(other.PChar.get(), spellId, 0) != nullptr;
                });

                if (PBetter != nullptr)
                {
                    continue;
                }

                if (!isWithinDistance(PBot->loc.p, PMember->loc.p, kCastRange))
                {
                    bot.patientId    = PMember->id;
                    bot.patientReach = kCastRange;
                    bot.patientUntil = now + kHealWalkLife;
                    return walkToPatient(bot, now);
                }

                if (cast(bot, PMember, PSpell, now))
                {
                    remember(PMember->id, *ailment, now + kCleanseRetry);
                    return true;
                }

                return false;
            }

            return false;
        };

        for (size_t i = 0; i < bots::kCleanses.size(); ++i)
        {
            if (!bot.kit.cleanse[i])
            {
                continue;
            }

            const auto& line = bots::kCleanses[i];

            const auto ailmentOf = [&line](CBattleEntity* PMember) -> std::optional<xi::StatusEffect>
            {
                for (const auto effect : line.effects)
                {
                    if (effect != xi::StatusEffect::None && PMember->StatusEffectContainer->HasStatusEffect(effect))
                    {
                        return effect;
                    }
                }

                return std::nullopt;
            };

            if (treat(line.spell, ailmentOf, [i](const Bot& other) { return other.kit.cleanse[i]; }))
            {
                return true;
            }
        }

        if (!bot.kit.erase)
        {
            return false;
        }

        // Kept under StatusEffect::None: Erase takes off whichever erasable effect it finds
        const auto erasable = [](CBattleEntity* PMember) -> std::optional<xi::StatusEffect>
        {
            if (PMember->StatusEffectContainer->HasStatusEffectByFlag(xi::StatusEffectFlag::Erasable))
            {
                return xi::StatusEffect::None;
            }

            return std::nullopt;
        };

        return treat(bots::kErase, erasable, [](const Bot& other) { return other.kit.erase; });
    }

    // How long this bard's song of `effect` still lasts on a member; nothing when it is not
    // on them. A song effect carries its singer's id (16 bits of it) as sub id.
    static auto songLeft(CBattleEntity* PMember, const CCharEntity* PBard, const xi::StatusEffect effect, const timer::time_point now) -> std::optional<timer::duration>
    {
        std::optional<timer::duration> left;

        PMember->StatusEffectContainer->ForEachEffect([&](CStatusEffect& existing)
        {
            if (existing.GetStatusID() == effect && static_cast<uint16>(existing.GetSubID()) == static_cast<uint16>(PBard->id))
            {
                left = existing.GetStartTime() + existing.GetDuration() - now;
            }
        });

        return left;
    }

    struct SongPick
    {
        CSpell*          PSpell  = nullptr;
        CBattleEntity*   PTarget = nullptr; // Who it is cast on: the bard itself for a party song
        CBattleEntity*   PNear   = nullptr; // Who the bard has to walk up to first, if anybody
        xi::StatusEffect effect  = xi::StatusEffect::None;
    };

    // The song the bard would sing now: the first one it keeps up that a member in reach
    // lacks. With `waiting`, also one whose recast is still running.
    auto pickSong(Bot& bot, const timer::time_point now, const bool waiting) -> SongPick
    {
        auto* PBot = bot.PChar.get();

        if (bot.songs.empty() || PBot->PParty == nullptr || inTown(PBot))
        {
            return SongPick{};
        }

        for (const auto& song : bot.songs)
        {
            if (wasTried(PBot->id, song.effect, now))
            {
                continue;
            }

            auto* PSpell = ready(PBot, song.spellId, 0, !waiting);
            if (PSpell == nullptr)
            {
                continue;
            }

            std::vector<CBattleEntity*> lacking;

            for (auto* PMember : PBot->PParty->members)
            {
                if (PMember->isDead() || PMember->loc.zone != PBot->loc.zone || !isWithinDistance(PBot->loc.p, PMember->loc.p, kHealReach) ||
                    (song.targetId != 0 && PMember->id != song.targetId))
                {
                    continue;
                }

                const auto left = songLeft(PMember, PBot, song.effect, now);
                if (!left || *left < kSongRefresh)
                {
                    lacking.emplace_back(PMember);
                }
            }

            if (lacking.empty())
            {
                continue;
            }

            if (song.targetId != 0)
            {
                auto* PMember = lacking.front();
                return SongPick{ PSpell, PMember, isWithinDistance(PBot->loc.p, PMember->loc.p, kCastRange) ? nullptr : PMember, song.effect };
            }

            // Stand where the song reaches the most members who lack it; the nearest such spot
            CBattleEntity* PAnchor = lacking.front();
            size_t         most    = 0;

            for (auto* PMember : lacking)
            {
                const auto count = static_cast<size_t>(std::ranges::count_if(lacking, [PMember](const CBattleEntity* POther)
                {
                    return isWithinDistance(PMember->loc.p, POther->loc.p, kSongCluster);
                }));

                if (count > most || (count == most && distance(PBot->loc.p, PMember->loc.p) < distance(PBot->loc.p, PAnchor->loc.p)))
                {
                    most    = count;
                    PAnchor = PMember;
                }
            }

            return SongPick{ PSpell, PBot, isWithinDistance(PBot->loc.p, PAnchor->loc.p, kSongReach) ? nullptr : PAnchor, song.effect };
        }

        return SongPick{};
    }

    // The Shadows rule: Utsusemi on itself once the last shadow is gone (casting Ichi over
    // Ni's shadows does nothing and still uses a tool), with the best tier that is off
    // recast and that it holds a tool for. In and out of a fight, not in town.
    auto tryShadows(Bot& bot, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (inTown(PBot) || now < bot.nextCast || now < bot.unableUntil ||
            PBot->StatusEffectContainer->HasStatusEffect(xi::StatusEffect::CopyImage) ||
            PBot->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }) ||
            PBot->StatusEffectContainer->HasPreventActionEffect() || wasTried(PBot->id, xi::StatusEffect::CopyImage, now))
        {
            return false;
        }

        for (const auto spellId : bot.kit.shadows)
        {
            auto* PSpell = spellId != 0 ? ready(PBot, spellId, 0) : nullptr;
            if (PSpell == nullptr)
            {
                continue;
            }

            if (cast(bot, PBot, PSpell, now))
            {
                remember(PBot->id, xi::StatusEffect::CopyImage, now + kBuffRetry);
                return true;
            }

            return false;
        }

        return false;
    }

    // The Song rule: keeps up the songs a player asked for with !ma.
    auto trySong(Bot& bot, const timer::time_point now) -> bool
    {
        const auto pick = pickSong(bot, now, false);
        if (pick.PSpell == nullptr)
        {
            return false;
        }

        if (pick.PNear != nullptr)
        {
            bot.patientId    = pick.PNear->id;
            bot.patientReach = pick.PTarget == bot.PChar.get() ? kSongReach + 1.0f : kCastRange;
            bot.patientUntil = now + kHealWalkLife;
            return walkToPatient(bot, now);
        }

        if (cast(bot, pick.PTarget, pick.PSpell, now))
        {
            remember(bot.PChar->id, pick.effect, now + kSongRetry);
            return true;
        }

        return false;
    }

    // Smart Buff: the first buff of bots::kBuffs that somebody lacks and nobody is casting.
    auto tryBuff(Bot& bot, const timer::time_point now) -> bool
    {
        const auto pick = pickBuff(bot, now, false);
        if (pick.PTarget == nullptr)
        {
            return false;
        }

        if (cast(bot, pick.PTarget, pick.PSpell, now))
        {
            remember(pick.PTarget->id, pick.effect, now + kBuffRetry);
            return true;
        }

        return false;
    }

    struct BuffPick
    {
        CBattleEntity*   PTarget = nullptr;
        CSpell*          PSpell  = nullptr;
        xi::StatusEffect effect  = xi::StatusEffect::None;
    };

    // The buff the bot would cast now. With `waiting`, also one whose recast is still
    // running: something it will cast shortly.
    auto pickBuff(Bot& bot, const timer::time_point now, const bool waiting) -> BuffPick
    {
        auto* PBot = bot.PChar.get();

        if (inTown(PBot))
        {
            return BuffPick{};
        }

        const uint8 reserve = bot.inFight ? kBuffReserve : 0;

        for (size_t i = 0; i < bots::kBuffs.size(); ++i)
        {
            const auto& line = bots::kBuffs[i];

            if (bot.kit.buffs[i] == 0 || (bot.inFight && !line.inFight))
            {
                continue;
            }

            auto* PSpell = ready(PBot, bot.kit.buffs[i], reserve, !waiting);
            if (PSpell == nullptr)
            {
                continue;
            }

            auto* PTarget = buffTarget(bot, line, PSpell, now);
            if (PTarget == nullptr)
            {
                continue;
            }

            // A bot that is better at support and has this effect too goes first
            if (line.who != bots::BuffFor::Self &&
                betterBot(bot, now, [](const bots::Role& role) { return role.supportRank; }, [&](const Bot& other)
            {
                for (size_t j = 0; j < bots::kBuffs.size(); ++j)
                {
                    if (bots::kBuffs[j].effect == line.effect && bots::kBuffs[j].who != bots::BuffFor::Self && other.kit.buffs[j] != 0 &&
                        ready(other.PChar.get(), other.kit.buffs[j], reserve) != nullptr)
                    {
                        return true;
                    }
                }

                return false;
            }) != nullptr)
            {
                continue;
            }

            return BuffPick{ PTarget, PSpell, line.effect };
        }

        return BuffPick{};
    }

    // Whether the bot's job has a Buff rule that applies right now and a buff it has the MP
    // for is waiting to be cast, or a Song rule and a song to sing again. Such a bot does not
    // sit down. Looked at once a second.
    auto buffWaiting(Bot& bot, const timer::time_point now) -> bool
    {
        if (now < bot.nextBuffWaitCheck)
        {
            return bot.buffWaiting;
        }

        bot.nextBuffWaitCheck = now + kBuffWaitCheck;
        bot.buffWaiting       = false;

        auto* PBot = bot.PChar.get();

        if (bot.kitDirty || !bot.kit.any || PBot->StatusEffectContainer->HasStatusEffect({ xi::StatusEffect::Silence, xi::StatusEffect::Mute }))
        {
            return false;
        }

        for (const auto& rule : bot.role->rules)
        {
            if (PBot->GetMLevel() > rule.maxLevel || PBot->GetMPP() < rule.minMpp)
            {
                continue;
            }

            if (rule.act == bots::Act::Buff)
            {
                bot.buffWaiting |= pickBuff(bot, now, true).PTarget != nullptr;
            }
            else if (rule.act == bots::Act::Song)
            {
                bot.buffWaiting |= pickSong(bot, now, true).PSpell != nullptr;
            }
        }

        return bot.buffWaiting;
    }

    // Who gets the buff of this line, if anybody needs it
    auto buffTarget(Bot& bot, const bots::BuffLine& line, CSpell* PSpell, const timer::time_point now) -> CBattleEntity*
    {
        auto* PBot = bot.PChar.get();

        const auto lacks = [&](CBattleEntity* PMember, const float range) -> bool
        {
            if (PMember->isDead() || PMember->loc.zone != PBot->loc.zone || !isWithinDistance(PBot->loc.p, PMember->loc.p, range) ||
                PMember->StatusEffectContainer->HasStatusEffect(line.effect) || wasTried(PMember->id, line.effect, now))
            {
                return false;
            }

            bool covered = false;

            forEachCast(PBot, [&](CBattleEntity*, CSpell* PCasting, const uint32 targetId)
            {
                covered |= PCasting->statusEffect() == line.effect && (targetId == PMember->id || PCasting->getAOE() != 0);
            });

            return !covered;
        };

        const auto isFighter = [](const CBattleEntity* PMember) -> bool
        {
            switch (PMember->GetMJob())
            {
                case xi::Job::WHM:
                case xi::Job::BLM:
                case xi::Job::SMN:
                case xi::Job::SCH:
                case xi::Job::GEO:
                    return false;
                default:
                    return true;
            }
        };

        if (line.who == bots::BuffFor::Self)
        {
            return lacks(PBot, kCastRange) ? PBot : nullptr;
        }

        if (line.who == bots::BuffFor::PartyArea)
        {
            const float radius = PSpell->getRadius() > 0.0f ? PSpell->getRadius() : 10.0f;
            uint8       count  = 0;

            for (auto* PMember : PBot->PParty->members)
            {
                count += lacks(PMember, radius) ? 1 : 0;
            }

            return count >= 2 ? PBot : nullptr;
        }

        for (auto* PMember : PBot->PParty->members)
        {
            switch (line.who)
            {
                case bots::BuffFor::Fighters:
                    if (!isFighter(PMember))
                    {
                        continue;
                    }
                    break;
                case bots::BuffFor::Casters:
                    if (PMember->GetMaxMP() <= 0)
                    {
                        continue;
                    }
                    break;
                case bots::BuffFor::Hurt:
                    if (!bot.inFight || PMember->GetHPP() >= kRegenBelowHpp)
                    {
                        continue;
                    }
                    break;
                default:
                    break;
            }

            if (lacks(PMember, kCastRange))
            {
                return PMember;
            }
        }

        return nullptr;
    }

    // Smart Debuff: the first debuff of bots::kDebuffs the enemy lacks, can be affected by,
    // and that nobody is casting or tried a moment ago.
    auto tryDebuff(Bot& bot, CBattleEntity* PMob, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (PMob->GetHPP() < kDebuffAboveHpp || !isWithinDistance(PBot->loc.p, PMob->loc.p, kCastRange))
        {
            return false;
        }

        const auto beingCast = [&](const auto& isThatSpell) -> bool
        {
            bool found = false;

            forEachCast(PBot, [&](CBattleEntity*, CSpell* PCasting, const uint32 targetId)
            {
                found |= targetId == PMob->id && isThatSpell(static_cast<uint16>(PCasting->getID()));
            });

            return found;
        };

        const auto supportRank = [](const bots::Role& role)
        {
            return role.supportRank;
        };

        for (size_t i = 0; i < bots::kDebuffs.size(); ++i)
        {
            const auto& line = bots::kDebuffs[i];

            if (bot.kit.debuffs[i] == 0 || PBot->GetMLevel() > line.maxLevel)
            {
                continue;
            }

            if (PMob->StatusEffectContainer->HasStatusEffect(line.effect) ||
                (line.blockedBy != xi::StatusEffect::None &&
                 (PMob->StatusEffectContainer->HasStatusEffect(line.blockedBy) || wasTried(PMob->id, line.blockedBy, now))) ||
                (line.immunity != xi::Immunity::None && PMob->hasImmunity(line.immunity)) ||
                (line.resRank != xi::Mod::NONE && PMob->getMod(line.resRank) >= kMaxResistRank) ||
                wasTried(PMob->id, line.effect, now))
            {
                continue;
            }

            auto* PSpell = ready(PBot, bot.kit.debuffs[i], reserveOf(bot, kDebuffReserve));
            if (PSpell == nullptr)
            {
                continue;
            }

            if (beingCast([&](const uint16 spellId) { return std::ranges::find(line.spells, spellId) != line.spells.end(); }))
            {
                continue;
            }

            // A better debuffer goes first, unless it is busy with another spell: then this
            // bot takes the line, so the debuffs go on together at the start of a fight
            if (betterBot(bot, now, supportRank, [&](const Bot& other)
            {
                return other.kit.debuffs[i] != 0 && !other.PChar->PAI->IsCurrentState<CMagicState>() && now >= other.nextCast &&
                       ready(other.PChar.get(), other.kit.debuffs[i], reserveOf(other, kDebuffReserve)) != nullptr;
            }) != nullptr)
            {
                continue;
            }

            if (cast(bot, PMob, PSpell, now))
            {
                remember(PMob->id, line.effect, now + kDebuffRetry);
                return true;
            }

            return false;
        }

        // Dispel, when the enemy has something to take off. Remembered under StatusEffect::None.
        if (bot.kit.dispel && PMob->StatusEffectContainer->HasStatusEffectByFlag(xi::StatusEffectFlag::Dispelable) &&
            !PMob->hasImmunity(xi::Immunity::Dispel) && !wasTried(PMob->id, xi::StatusEffect::None, now))
        {
            auto* PSpell = ready(PBot, bots::kDispel, reserveOf(bot, kDebuffReserve));

            if (PSpell != nullptr && !beingCast([](const uint16 spellId) { return spellId == bots::kDispel; }) &&
                betterBot(bot, now, supportRank, [](const Bot& other)
            {
                return other.kit.dispel && ready(other.PChar.get(), bots::kDispel, reserveOf(other, kDebuffReserve)) != nullptr;
            }) == nullptr &&
                cast(bot, PMob, PSpell, now))
            {
                remember(PMob->id, xi::StatusEffect::None, now + kBuffRetry);
                return true;
            }
        }

        return false;
    }

    // Smart Spell: the elemental spell that does the most to this enemy. Potency is the
    // spell's base damage; the enemy's resistance rank and damage taken for the element
    // scale it, and an element it absorbs or nullifies is left out.
    auto tryNuke(Bot& bot, CBattleEntity* PMob, const timer::time_point now) -> bool
    {
        auto* PBot = bot.PChar.get();

        if (bot.kit.nukes.empty() || !isWithinDistance(PBot->loc.p, PMob->loc.p, kCastRange))
        {
            return false;
        }

        const auto elementMod = [PMob](const xi::Mod fire, const uint16 element) -> int16
        {
            return PMob->getMod(static_cast<xi::Mod>(static_cast<uint16>(fire) + element - 1));
        };

        CSpell* PBest = nullptr;
        float   best  = 0.0f;

        for (const auto spellId : bot.kit.nukes)
        {
            auto* PSpell = ready(PBot, spellId, reserveOf(bot, kNukeReserve));
            if (PSpell == nullptr)
            {
                continue;
            }

            const auto element = PSpell->getElement();
            if (element < 1 || element > 8 || elementMod(xi::Mod::FIRE_ABSORB, element) > 0 || elementMod(xi::Mod::FIRE_NULL, element) > 0)
            {
                continue;
            }

            const float taken = std::clamp(1.0f + elementMod(xi::Mod::FIRE_SDT, element) / 10000.0f, 0.0f, 3.0f);
            const float score = PSpell->getBase() * bots::resistRankMultiplier(elementMod(xi::Mod::FIRE_RES_RANK, element)) * taken;

            if (score > best || (PBest != nullptr && score == best && PSpell->getMPCost() < PBest->getMPCost()))
            {
                best  = score;
                PBest = PSpell;
            }
        }

        return PBest != nullptr && best > 0.0f && cast(bot, PMob, PBest, now);
    }

    // Notes that an effect was just cast on a target, so no bot casts it again before
    // `until`: the spell may have been resisted, or not have landed yet.
    void remember(const uint32 targetId, const xi::StatusEffect effect, const timer::time_point until)
    {
        const auto now = timer::now();
        std::erase_if(tried_, [now](const Tried& tried) { return now >= tried.until; });

        tried_.emplace_back(Tried{ targetId, effect, until });
    }

    auto wasTried(const uint32 targetId, const xi::StatusEffect effect, const timer::time_point now) const -> bool
    {
        for (const auto& tried : tried_)
        {
            if (tried.targetId == targetId && tried.effect == effect && now < tried.until)
            {
                return true;
            }
        }

        return false;
    }

    //
    // Resting
    //

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

    // Standing still with nothing to do (the Rest rule): learn scrolls, sit down. Nobody sits
    // while a party member is fighting, or while it has a buff waiting to be cast.
    void idle(Bot& bot, const CCharEntity* PLeader, const timer::time_point now)
    {
        auto* PBot = bot.PChar.get();

        if (!bot.still)
        {
            bot.still      = true;
            bot.stillSince = now;
        }

        const bool resting       = PBot->animation == xi::Animation::Healing;
        const bool leaderResting = PLeader->animation == xi::Animation::Healing;
        const bool allowed       = !PBot->PAI->IsEngaged() && !bot.inFight;

        const bool needsRest = bot.role->mage
                                   ? PBot->health.mp < PBot->GetMaxMP() || PBot->GetHPP() <= 75 || leaderResting
                                   : PBot->health.hp < PBot->GetMaxHP() || PBot->health.mp < PBot->GetMaxMP() || leaderResting;

        const bool wantsRest = needsRest && allowed && !buffWaiting(bot, now);

        if (resting)
        {
            if (allowed && wantsRest)
            {
                bot.state = AiState::Rest;
            }
            else
            {
                standUp(bot);
            }

            return;
        }

        if (!allowed || now - bot.stillSince < kSettleDelay)
        {
            return;
        }

        if (!bot.inFight && bot.checkItems && now >= bot.nextItemCheck && useItems(bot, now))
        {
            return;
        }

        if (wantsRest)
        {
            rest(bot);
            bot.state = AiState::Rest;
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

        auto*       PPath = PBot->PAI->PathFind.get();
        const auto  spot  = followSpot(bot, PLeader);
        const float dist  = distance(PBot->loc.p, PLeader->loc.p);

        const bool moving       = PPath->IsFollowingPath();
        const bool leaderMoving = !isWithinDistance(bot.leaderAt, PLeader->loc.p, 0.1f);

        bot.leaderAt = PLeader->loc.p;

        // The common case: standing near the leader. While the leader is on the move the bot
        // sets off again sooner, so it doesn't fall behind and catch up in bursts.
        if (!moving && dist <= spot.reach + (leaderMoving ? kSpotRadius : kFollowSlack))
        {
            // Only a bot that was standing with a standing leader hesitates before setting off
            if (!leaderMoving)
            {
                bot.settingOff = false;
            }

            idle(bot, PLeader, now);
            return;
        }

        bot.still = false;
        standUp(bot);

        if (dist > kWarpDistance)
        {
            ShowInfoFmt("bot_players: {} is {:.0f} yalms from {}, warps to them", PBot->getName(), dist, PLeader->getName());
            PPath->WarpTo(PLeader->loc.p, spot.reach);
            bot.pathFails = 0;
            return;
        }

        if (!moving)
        {
            // The leader walked off: each bot reacts a little later than the last
            if (!bot.settingOff)
            {
                bot.settingOff = true;
                bot.setOffAt   = now + std::chrono::milliseconds(botHash(PBot->id, 24) * kSetOffDelay / 255);
            }

            if (now < bot.setOffAt)
            {
                return;
            }
        }

        if (isWithinDistance(PBot->loc.p, spot.pos, kSpotRadius))
        {
            // Arrived: look roughly where the leader looks
            stop(bot);

            PBot->loc.p.rotation = static_cast<uint8>(PLeader->loc.p.rotation + botHash(PBot->id, 0) * 2 * kFacingJitter / 255 - kFacingJitter);
            PBot->updatemask |= UPDATE_POS;
            return;
        }

        if (now >= bot.nextRepath && (!moving || !isWithinDistance(bot.pathGoal, spot.pos, kRepathDrift)))
        {
            bot.nextRepath = now + kRepathInterval;

            // The spot can be inside a wall when the leader walks along one; then anywhere
            // at that distance from the leader will do.
            const bool found = (PBot->loc.zone->navMesh()->validPosition(spot.pos) && PPath->PathTo(spot.pos, PATHFLAG_RUN)) ||
                               PPath->PathAround(PLeader->loc.p, spot.reach, PATHFLAG_RUN);

            if (found)
            {
                bot.pathGoal  = spot.pos;
                bot.pathFails = 0;
            }
            else if (++bot.pathFails >= kMaxPathFails)
            {
                bot.pathFails = 0;

                // Not into a fight, and not from a few steps away
                if (!bot.inFight && dist > kWarpMinDistance)
                {
                    ShowInfoFmt("bot_players: {} finds no path to {} ({:.0f} yalms), warps to them", PBot->getName(), PLeader->getName(), dist);
                    PPath->WarpTo(PLeader->loc.p, spot.reach);
                    return;
                }
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
        auto* PBot  = bot.PChar.get();
        auto* PPath = PBot->PAI->PathFind.get();

        if (!PPath->IsFollowingPath())
        {
            return;
        }

        // One full path step, then what is left of a player's stride at the bot's current
        // speed (so gravity, bind and movement speed gear all count)
        const position_t before = PBot->loc.p;

        PPath->FollowPath(now);

        const float left = static_cast<float>(PBot->UpdateSpeed(true)) * kYalmsPerTickPerSpeed - distance(before, PBot->loc.p);

        if (left > 0.05f && PPath->IsFollowingPath())
        {
            PPath->FollowPath(now, left);
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

    // A number from 0 to 255 that is fixed for a bot and differs between bots, to make each
    // one stand and react a little differently. `shift` picks one of four (0, 8, 16, 24).
    static auto botHash(const uint32 charid, const uint8 shift) -> int
    {
        return static_cast<int>(((charid * 2654435761U) >> shift) & 0xFF);
    }

    // The bot's place among the bots of its party: 0, 1, ... Melee jobs come before mages,
    // so mages get the spots further back.
    auto followSlot(const Bot& bot) const -> uint8
    {
        const bool mage   = bot.role->mage;
        uint8      slot   = 0;
        bool       before = true;

        for (const auto* PMember : bot.PChar->PParty->members)
        {
            if (PMember == bot.PChar.get())
            {
                before = false;
                continue;
            }

            const auto it = live_.find(PMember->id);
            if (it == live_.end() || it->second.puppet)
            {
                continue;
            }

            const bool otherMage = it->second.role->mage;

            if (otherMage != mage ? mage : before)
            {
                ++slot;
            }
        }

        return slot;
    }

    struct Spot
    {
        position_t pos;
        float      reach; // How far from the leader it is
    };

    // Where the bot stands and runs relative to the leader: its spot of kFollowSpots, turned
    // with the way the leader faces, and moved a little by an amount fixed per bot.
    auto followSpot(const Bot& bot, const CCharEntity* PLeader) const -> Spot
    {
        const auto  slot = followSlot(bot);
        const auto& base = kFollowSpots[slot % kFollowSpots.size()];
        const auto  id   = bot.PChar->id;

        const float distanceJitter = (static_cast<float>(botHash(id, 8)) / 127.5f - 1.0f) * kSpotDistanceJitter;
        const float angleJitter    = (static_cast<float>(botHash(id, 16)) / 127.5f - 1.0f) * kSpotAngleJitter;

        // More bots than spots (not possible in a party of six): a further ring
        const float reach = base.distance + distanceJitter + 1.6f * static_cast<float>(slot / kFollowSpots.size());
        const float angle = (base.angle + angleJitter) * static_cast<float>(M_PI) / 180.0f;

        return { nearPosition(PLeader->loc.p, reach, static_cast<float>(M_PI) + angle), reach };
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
                                               "COALESCE(job, 0) AS job, COALESCE(subjob, 255) AS subjob, COALESCE(level, 1) AS level, COALESCE(nation, 255) AS nation, "
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
                cmd.subjob       = rset->get<uint8>("subjob");
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
            // for `trade` the one who offers the trade, for `ma` the one who asks)
            if (cmd.anchorCharId != 0 && cmd.command != "invite" && cmd.command != "trade" && cmd.command != "ma")
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
            else if (cmd.command == "jobs")
            {
                std::tie(ok, result) = changeJobs(cmd.charid, cmd.job, cmd.subjob);
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
            else if (cmd.command == "ma")
            {
                // !ma without a client: `name` is the spell, the anchor the party member asking
                const auto bot    = live_.find(cmd.charid);
                const auto puppet = live_.find(cmd.anchorCharId);
                auto*      PAsker = puppet != live_.end() ? puppet->second.PChar.get() : zoneutils::GetChar(cmd.anchorCharId);

                ok = bot != live_.end() && PAsker != nullptr;

                result = ok ? giveOrder(PAsker, bot->second.PChar->getName(), cmd.name, Order::Kind::Spell)
                            : "The bot or the character asking is not out.";
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

        // The six starting jobs are unlocked on a new character (char_jobs.unlocked = 126); the
        // advanced jobs a bot can start as are unlocked below.
        const char* advancedJob = bots::advancedJobColumn(job);

        if ((job < 1 || job > 6) && advancedJob == nullptr)
        {
            return { false, "Bots can start as WAR, MNK, WHM, BLM, RDM, THF, BRD, RNG, SAM or NIN." };
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
        bool ok =
            db::preparedStmt("INSERT INTO chars(charid, accid, charname, pos_zone, pos_x, pos_y, pos_z, pos_rot, nation) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)",
                             charid, accid, name, static_cast<uint16>(placement->zone), placement->pos.x, placement->pos.y, placement->pos.z, placement->pos.rotation, nation) &&
            db::preparedStmt("INSERT INTO char_look(charid, face, race, size) VALUES(?, ?, ?, ?)", charid, face, race, size) &&
            db::preparedStmt("INSERT INTO char_stats(charid, mjob) VALUES(?, ?)", charid, job) &&
            db::preparedStmt("INSERT INTO char_flags(charid) VALUES(?)", charid) &&
            db::preparedStmt("INSERT INTO bot_players(charid, autospawn) VALUES(?, ?)", charid, autospawn ? 1 : 0);
        // clang-format on

        if (ok && advancedJob != nullptr)
        {
            // The char_insert trigger leaves advanced jobs locked and at level 0
            ok = !!db::preparedStmt("UPDATE char_jobs SET unlocked = unlocked | ?, "
                                    "brd = IF(? = 10, 1, brd), rng = IF(? = 11, 1, rng), sam = IF(? = 12, 1, sam), nin = IF(? = 13, 1, nin) WHERE charid = ?",
                                    1U << job,
                                    job,
                                    job,
                                    job,
                                    job,
                                    charid);
        }

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
            giveStarterGear(PBot.get());
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

        // Bots have the subjob unlocked without the quest (bit 0 of char_jobs.unlocked)
        if ((PBot->jobs.unlocked & 1) == 0)
        {
            PBot->jobs.unlocked |= 1;
            charutils::SaveCharJob(PBot.get(), PBot->GetMJob());
        }

        // Its main job has its starting gear already (see giveFirstJobGear)
        if (PBot->getCharVar(kGearedJobsVar) == 0)
        {
            PBot->setCharVar(kGearedJobsVar, 1 << static_cast<uint8>(PBot->GetMJob()));
        }

        // Players equip their starting weapon and ring themselves; bots can't.
        autoEquipEmptySlots(PBot.get());

        growInventory(PBot.get());
        stowSpareGear(PBot.get());

        // Players get these in xi.player.onGameIn, which bots never go through. Permanent
        // effects survive the bot's zone changes (moveToZone keeps the entity).
        if (luautils::detail::findGlobalLuaFunction("xi.custom.permanentBuffs.apply").valid())
        {
            luautils::callGlobal<void>("xi.custom.permanentBuffs.apply", PBot.get());
        }

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
        bot.home   = *home;
        bot.level  = bot.PChar->GetMLevel();
        bot.foodId = loadFood(charid);
        indexBot(bot);
        writeStatusFor(bot, true);

        return message;
    }

    // Changes a bot's main job and/or subjob: what GP_CLI_COMMAND_MYROOM_JOB::process does
    // for a player at a Moogle (map/packets/c2s/0x100_myroom_job.cpp), without the Moogle.
    // mainJob 0 = keep; subJob 255 = keep, 0 = none. A bot that is not spawned is spawned
    // for the change and despawned again.
    auto changeJobs(const uint32 charid, const uint8 mainJob, const uint8 subJob) -> std::pair<bool, std::string>
    {
        if (!isBot(charid))
        {
            return { false, fmt::format("Character {} is not a bot.", charid) };
        }

        if ((mainJob != 0 && !bots::isBotJob(mainJob)) || (subJob != 0 && subJob != 255 && !bots::isBotJob(subJob)))
        {
            return { false, "Bot jobs are WAR, MNK, WHM, BLM, RDM, THF, BRD, RNG, SAM and NIN." };
        }

        const bool wasLive = live_.contains(charid);
        if (!wasLive)
        {
            const auto spawned = spawn(charid, std::nullopt);
            if (!live_.contains(charid))
            {
                return { false, fmt::format("Could not load the bot for the change: {}", spawned) };
            }
        }

        auto result = changeLiveJobs(live_.at(charid), mainJob, subJob);

        if (!wasLive)
        {
            despawn(charid);
        }

        return result;
    }

    auto changeLiveJobs(Bot& bot, const uint8 mainJob, const uint8 subJob) -> std::pair<bool, std::string>
    {
        auto* PBot = bot.PChar.get();

        const xi::Job prevMain = PBot->GetMJob();
        const xi::Job prevSub  = PBot->GetSJob();

        uint8 newMain = mainJob != 0 ? mainJob : static_cast<uint8>(prevMain);
        uint8 newSub  = subJob != 255 ? subJob : static_cast<uint8>(prevSub);

        // The current subjob as the new main job: they swap, like retail
        if (subJob == 255 && newSub == newMain)
        {
            newSub = static_cast<uint8>(prevMain);
        }

        if (newSub == newMain)
        {
            return { false, "The main job and the subjob can't be the same." };
        }

        if (newMain == static_cast<uint8>(prevMain) && newSub == static_cast<uint8>(prevSub))
        {
            return { true, fmt::format("{} already has those jobs.", PBot->getName()) };
        }

        if (PBot->isDead())
        {
            return { false, fmt::format("{} is knocked out.", PBot->getName()) };
        }

        if (bot.inFight || PBot->PAI->IsEngaged() || PBot->PAI->IsCurrentState<CWeaponSkillState>())
        {
            return { false, fmt::format("{} is in a fight. Change jobs when it is over.", PBot->getName()) };
        }

        if (PBot->UContainer->GetType() != UCONTAINER_EMPTY)
        {
            return { false, fmt::format("{} is busy with a trade.", PBot->getName()) };
        }

        const bool changingMain = newMain != static_cast<uint8>(prevMain);

        // A job the bot never had: unlocked, at level 1
        for (const uint8 job : { newMain, newSub })
        {
            if (job == 0)
            {
                continue;
            }

            const bool locked = (PBot->jobs.unlocked & (1 << job)) == 0;
            if (locked || PBot->jobs.job[job] == 0)
            {
                PBot->jobs.unlocked |= (1 << job);
                PBot->jobs.job[job] = std::max<uint8>(PBot->jobs.job[job], 1);
                charutils::SaveCharJob(PBot, static_cast<xi::Job>(job));
            }
        }

        if (bot.order.kind != Order::Kind::None)
        {
            dropOrder(bot, "it changed jobs.");
        }

        calm(bot);
        stop(bot);

        if (PBot->PAI->IsCurrentState<CMagicState>() || PBot->PAI->IsCurrentState<CItemState>())
        {
            PBot->PAI->InterruptStates();
        }

        charutils::SaveJobChangeGear(PBot);

        if (changingMain)
        {
            PBot->resetPetZoningInfo();
            charutils::RemoveAllEquipment(PBot);
            PBot->SetMJob(newMain);
        }

        PBot->SetSJob(newSub);
        PBot->SetMLevel(PBot->jobs.job[newMain]);
        PBot->SetSLevel(PBot->jobs.job[newSub]);

        // A second weapon needs the subjob that allowed it
        if (!changingMain)
        {
            if (auto* PWeapon = dynamic_cast<CItemWeapon*>(PBot->m_Weapons[SLOT_SUB]))
            {
                if (PWeapon->getDmgType() > xi::DamageType::None && PWeapon->getDmgType() < xi::DamageType::HandToHand)
                {
                    charutils::UnequipItem(PBot, SLOT_SUB);
                }
            }
        }

        charutils::SetStyleLock(PBot, false);
        luautils::CheckForGearSet(PBot);

        jobpointutils::RefreshGiftMods(PBot);
        charutils::BuildingCharSkillsTable(PBot);
        charutils::CalculateStats(PBot);
        charutils::BuildingCharTraitsTable(PBot);
        PBot->PRecastContainer->ChangeJob();
        charutils::BuildingCharAbilityTable(PBot);
        charutils::BuildingCharWeaponSkills(PBot);

        if (changingMain)
        {
            // What it wore on this job before, then its starting gear the first time, then
            // whatever else in its bag fits a slot that is still empty
            charutils::LoadJobChangeGear(PBot);
            giveFirstJobGear(PBot);
            autoEquipEmptySlots(PBot);
            bot.checkItems = true; // the old job's gear is in the inventory now (stowSpareGear)
        }

        PBot->StatusEffectContainer->DelStatusEffectsByFlag(xi::StatusEffectFlag::Dispelable | xi::StatusEffectFlag::Roll | xi::StatusEffectFlag::OnJobchange);

        if (luautils::detail::findGlobalLuaFunction("xi.custom.permanentBuffs.apply").valid())
        {
            luautils::callGlobal<void>("xi.custom.permanentBuffs.apply", PBot);
        }

        // clang-format off
        PBot->ForParty([](CBattleEntity* PMember)
        {
            static_cast<CCharEntity*>(PMember)->PLatentEffectContainer->CheckLatentsPartyJobs();
        });
        // clang-format on

        PBot->UpdateHealth();
        PBot->health.hp = PBot->GetMaxHP();
        PBot->health.mp = PBot->GetMaxMP();
        PBot->updatemask |= UPDATE_ALL_CHAR;

        charutils::SaveCharStats(PBot);
        charutils::SaveCharJob(PBot, PBot->GetMJob());
        charutils::SaveCharExp(PBot, PBot->GetMJob());

        // The party list of the other members shows the job
        if (PBot->PParty != nullptr)
        {
            PBot->PParty->ReloadParty();
        }

        // Everything above queued packets for the bot's (nonexistent) client
        PBot->clearPacketList();

        // The AI starts over as the new job: role and spell kit are rebuilt on the next tick
        bot.kitDirty    = true;
        bot.level       = PBot->GetMLevel();
        bot.spacing     = 0;
        bot.spacedFor   = 0;
        bot.patientId   = 0;
        bot.foeId       = 0;
        bot.shotFails   = 0;
        bot.buffWaiting = false;

        writeStatusFor(bot, true);

        const auto jobName = [](const uint8 job) -> std::string
        {
            static constexpr std::array<const char*, 14> names{ "none", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG", "SAM", "NIN" };
            return job < names.size() ? names[job] : "?";
        };

        ShowInfoFmt("bot_players: {} changed jobs to {}{}/{}{}", PBot->getName(), jobName(newMain), PBot->GetMLevel(), jobName(newSub), PBot->GetSLevel());

        return { true, newSub != 0
                           ? fmt::format("{} is now {} {} / {} {}.", PBot->getName(), jobName(newMain), PBot->GetMLevel(), jobName(newSub), PBot->GetSLevel())
                           : fmt::format("{} is now {} {}, no subjob.", PBot->getName(), jobName(newMain), PBot->GetMLevel()) };
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

    // Puts back the session rows of spawned bots that something else deleted, so they show
    // in /sea again. xi_test does that: started against this database, it clears every row
    // of accounts_sessions (map_engine.cpp deletes "its" rows, and a test server has no
    // address, which matches all of them).
    void restoreSessions()
    {
        if (live_.empty())
        {
            return;
        }

        uint32 rows = 0;
        {
            const auto rset = db::preparedStmt("SELECT COUNT(*) AS n FROM accounts_sessions s JOIN bot_players b ON b.charid = s.charid");
            if (!rset || !rset->rowsCount() || !rset->next())
            {
                return;
            }

            rows = rset->get<uint32>("n");
        }

        if (rows >= live_.size())
        {
            return;
        }

        uint32 restored = 0;

        for (auto& [charid, bot] : live_)
        {
            auto* PBot = bot.PChar.get();
            if (PBot == nullptr || PBot->loc.zone == nullptr)
            {
                continue;
            }

            const auto rset = db::preparedStmt("INSERT IGNORE INTO accounts_sessions(accid, charid, targid, server_addr, server_port) VALUES(?, ?, ?, ?, ?)",
                                               PBot->accid,
                                               charid,
                                               PBot->targid,
                                               PBot->loc.zone->GetIP(),
                                               PBot->loc.zone->GetPort());
            if (rset && rset->rowsAffected() > 0)
            {
                ++restored;
            }
        }

        if (restored > 0)
        {
            ShowInfoFmt("bot_players: put back {} session row(s) that were deleted (bots show in /sea again)", restored);
        }
    }

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

    // Buffs and debuffs cast a moment ago, see remember()
    std::vector<Tried> tried_;

    // The enemy a bot just provoked, and the skillchain step a bot is answering
    Claim provoked_;
    Claim chained_;

    // main.CURE_POWER
    float curePower_ = 1.0f;

    // custom.BOT_AUTO_LEARN_SPELLS
    bool autoLearn_ = false;
};

REGISTER_CPP_MODULE(BotPlayersModule);
