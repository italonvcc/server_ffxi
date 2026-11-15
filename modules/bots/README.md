# Bot Players

Server-controlled characters that the FFXI client sees as real players. The goal is
party members for soloing on a private server: bots that follow, heal and fight.

> Not retail behaviour. Nothing here can be checked against retail captures. The only
> test that counts is how bots look and behave in the client.

## Status

| Works | Not done yet |
| --- | --- |
| Bots are real `chars` rows, spawned into zones as PCs (0x00D packets, white names) | Smarter combat: weapon skills, job abilities, spells other than Cure, sub-targets |
| New bots get the normal new-character setup (`xi.player.charCreate`): race gear, job weapon, nation ring, gil, home point | `/tell` to a bot (routed, unanswered, untested) |
| Starting weapon/ring auto-equipped; inspect shows real gear | Bots can only start as WAR/MNK/WHM/BLM/RDM/THF (the 6 jobs unlocked on a new char) |
| Bots with `autospawn = 1` spawn at their saved position on server boot | Gil from kills is still split with bots (`charutils::DistributeGil`, core) |
| Spawned bots appear in `/sea` | Subjobs; gear upgrades (a bot only fills empty slots) |
| Web dashboard: create, inspect, spawn/despawn, move next to a player, autospawn, delete | Multi map-process setups (only tested with one xi_map hosting all zones) |
| In-game `!bot list/spawn/despawn/here` | Alliances (alliance invites are declined) |
| **AI:** accepts party invites, follows the party leader (also into other zones), walks home when it has no party | Following into Mog Houses and instances (the bot waits outside) |
| **AI:** fights the leader's target, cures the party (if it knows Cure), rests, takes a Raise or gets up by itself | Bots stand on the same spot when they fight the same target |
| **AI:** accepts trades, passes on loot (a lone player gets every drop), learns scrolls it carries | In-client checks of all of the above (see "Not checked in a client") |
| Exp, levels and skill-ups work like a player's; the dashboard shows exp, skills and spells | |

Current bots: **Lamaza** (charid 4), Mithra White Mage, Southern San d'Oria, autospawn on.

## Layout

```
modules/bots/
  cpp/bot_players.cpp      C++ module (CPPModule): spawning, command queue, status, Lua bindings
  cpp/bot_market.cpp       C++ module: auction house stocked and bought from a price snapshot
  market/crawl.js          ffxiah.com scraper, runs in a browser tab
  market/load_snapshot.py  imports scraper dumps, builds data/snapshot.json, fills bot_market_prices
  market/data/             the snapshot (browse_raw.json, sales_raw.json, snapshot.json)
  lua/bot_command.lua      !bot GM command (permission 0)
  sql/bot_players.sql      bot_players + bot_commands tables (also auto-created, see below)
  dashboard/dashboard.py   local web dashboard (Python stdlib + mariadb connector)
  dashboard/index.html     dashboard page (vanilla JS, no build step)
```

The module is enabled by the `bots` line in `modules/init.txt`. CMake globs the `.cpp`,
the Lua loader picks up the `.lua`. Changing the C++ needs a rebuild of `xi_map`; changing
the Lua needs a map server restart (or file-watch reload). CMake only re-globs when
`modules/init.txt` changes: after adding a `.cpp`, run `cmake build` before building.

## How it works

### A bot is a character without a session

1. **Database.** Each bot is a normal character: a `chars` row plus everything the
   `char_insert` trigger creates, on its **own account** (`accounts.login = 'bot<charid>'`,
   password `'!'`, which is not a valid hash, so nobody can log in). Lamaza predates that
   and uses the `lsb_bots` account. `bot_players` marks which characters are bots.
2. **Loading.** `charutils::LoadChar(charid)` (the same path a login uses), then
   `CZone::IncreaseZoneCounter()` inserts it into the zone's char list. `PSession` stays
   `nullptr`.
3. **Visibility.** Because it is in `m_charList`, the zone's normal `SpawnPCs` /
   `TryAddToNearbySpawnLists` send it to nearby players as a PC. Appearance changes are
   broadcast by `CCharEntity::PostTick` (driven by the AI tick, not the session), so
   equipping after spawn shows up for everyone.
4. **Discarded packets.** Everything queued for the bot's own (nonexistent) client is
   dropped in `OnZoneTick` via `clearPacketList()`. Without this the queue grows forever.
5. **Session row.** While spawned, the bot has an `accounts_sessions` row pointing at this
   map server. That is what `/sea` (xi_search) lists, and what the world server uses to
   route tells/invites. Removed on despawn; the map server also deletes its own rows on boot.
6. **First spawn.** If the character lacks title 206 (`NEW_ADVENTURER`), the module calls
   `xi.player.charCreate` like a first login, then sets the level (if > 1) and equips
   anything in the inventory that fits an empty slot.

### AI

Runs in `OnZoneTick` (every 400 ms logic tick), only for the bots in the zone that is
ticking (`byZone_` index). Per bot, `think()` does, in order:

1. **Invite.** If `InvitePending` is set (a player invited the bot from the party menu or
   with `/pcmd add`), it is answered after `kInviteDelay` (1.2 s) with the same outcome as
   the client answering "yes": the inviter's party is created if needed and the bot is added
   (`CParty::AddMember`). Declined (the inviter gets the normal "declined" message) when it
   is an alliance invite, the bot already has a party, the inviter is a bot, the inviter
   has trusts out, or the party is full.
2. **Follow.** In a party whose leader is a real player: stay within
   `kFollowStop + kFollowSlack` of the leader. Paths come from the zone navmesh through
   `CPathFind`, the same code mobs and trusts use; PCs have no pathfinder, so `spawn()`
   gives the bot one. Each bot in the party stops `kSlotSpacing` further out than the one
   before, so they don't stack. More than `kWarpDistance` away, or 3 failed path queries in
   a row: warp to the leader. Leader in another zone: `moveToZone()` puts the bot next to
   them once they have finished loading.
3. **Wait.** The leader's entity is destroyed and rebuilt when it changes zone, so the
   party briefly has no leader; the bot stands still for up to `kLeaderGrace` (120 s).
   It also waits while the leader is in a Mog House or an instance.
4. **Leave.** If the leader is a bot (the lead is handed to the next member when a leader
   leaves the party or logs off) or the grace period ran out, the bot leaves the party.
5. **Return.** With no party (never invited, kicked, disbanded, left as above) and away
   from home, walk home; teleport if it is in another zone, no path is found, or it takes
   longer than `kReturnTimeout`. At home the bot is `idle`.

Before the party logic, every tick: a pending **trade** is answered, the **treasure pool**
is looked at (once a second), and nothing else happens while the bot is casting or using
an item (moving would interrupt it). With a leader in the same zone, `assist()` runs
before `follow()`:

- **Cure.** Every 0.8 s: the party member with the lowest HP% under `kCureBelowHpp` (65)
  within 18 yalms gets the smallest Cure (I-VI) the bot knows and can afford that covers
  the missing HP. Any job that knows a Cure does this.
- **Fight.** If the leader is engaged with a monster within `kAssistRange` (25), the bot
  engages it too (`PAI->Engage`, the same path as a client), walks into melee range,
  faces it, and lets the normal attack state swing. It switches target with the leader and
  disengages when the leader does. State `fight`.
- **Rest.** Standing next to the leader for 4 s with HP or MP missing while the leader is
  not fighting: sits down (`Healing` effect, like `/heal`). Gets up to move, fight, cure,
  or when full. State `rest`.
- **Knocked out.** State `dead`. A Raise cast on the bot is accepted after 4 s. With no
  Raise, the bot gets up by itself after `kSelfRaiseDelay` (60 s) as if raised (weakened,
  normal exp loss): the homepoint warp needs a session, see the gotchas.

**Trade.** A player's trade request is accepted after 1.2 s and confirmed 1.5 s later; the
bot keeps confirming (changing an offer clears confirmations), so the trade completes when
the player confirms. The bot never offers anything. Requests from other bots are refused.

**Loot.** A bot passes on every item in its treasure pool. When the pool has exactly one
player, each item is then lotted on that player's behalf, which awards it immediately, so
drops behave as if the player were solo. With two or more players the bots just pass and
the players lot as usual. A full inventory or an already-owned rare item leaves the item
in the pool for the player to deal with. Gil is not covered: it is split in the core.

**Items.** An inventory scan runs only when flagged: on spawn, after a trade, after a level
up. It equips gear that fits an empty slot and uses one scroll the bot can learn
(`onItemCheck` says so), through the normal item-use state, so the scroll is consumed and
the spell is saved. Scrolls for a higher level are kept and tried again on level up.

**Home** is `bot_players.home_*`: wherever the bot was last placed (`create`, `summon`,
`!bot here`, `spawn` with a position). `spawn` without a position, and autospawn on boot,
put the bot at home, not where it happened to be when the server stopped.

`bot_players.ai_state` (`idle` / `follow` / `wait` / `return` / `fight` / `rest` / `dead`),
`leader`, `level`, `exp` and `exp_next` are live columns; the dashboard shows them, plus
skills (`char_skills`) and spells (`char_spells`).

Performance rules (bots must never cost players a slow tick):

- No database access in the tick. Only events touch it: joining or leaving a party,
  changing zone, arriving home.
- An idle bot costs a packet-queue clear and a few pointer checks. A bot standing next to
  its leader costs one distance check.
- Navmesh queries are throttled per bot: at most one per `kRepathInterval` (0.8 s) while
  following, and only if the leader has moved `kRepathDrift` from where the current path
  ends; one per 2 s while walking home.
- The status write (every 4.8 s) is skipped for bots whose row would not change.
- Fighting costs a distance check per tick and at most one navmesh query per 0.8 s, only
  while the target is out of reach and has moved. The cure check walks the party (6
  entries) every 0.8 s. The treasure pool check is one pointer and one counter per second
  when the pool is empty.
- Inventory scans call Lua, so they only run when flagged by an event, never per tick.
- A zone with a bot in it stays awake (its mobs keep ticking) like a zone with a player.
  Park idle bots in towns.

Movement speed: `CPathFind` moves `speed / 17` yalms per second, 2.9 for a PC's speed of
50, which is slower than a player runs. Bots take `kStepsPerTick` (2) path steps per tick,
about 5.9 yalms/s, the same as a running trust. Measured: 16.6 yalms home in 3.1 s.

### Control: command queue

The dashboard never touches live bots. It inserts rows into `bot_commands`; the module
reads up to 20 `pending` rows every time server tick (2.4 s), runs them, and writes
`status` (`done` / `failed`) and `result`.

| command | fields used |
| --- | --- |
| `create` | `name, race (1-8), face (0-15), size (0-2), job (1-6), level, nation (NULL = race home), autospawn`, plus placement |
| `spawn` | `charid`, optional placement (default: saved position) |
| `despawn` | `charid` |
| `summon` | `charid`, placement (despawn + spawn there) |
| `autospawn` | `charid, autospawn` |
| `delete` | `charid` (despawns, deletes `chars` row; the `char_delete` trigger removes the rest) |
| `invite` | `charid`, `anchor_charid` = an online player who invites the bot (dashboard: "Join party of") |
| `move` | `charid`, placement. Testing aid: relocates without a despawn, keeps party and home |
| `puppet` | `charid, autospawn` (1 on, 0 off). Testing aid, see below. Not saved, off after a respawn |
| `engage` | `charid`. Testing aid: attack the nearest monster within 30 yalms (the result says where it is if further) |
| `trade` | `charid` = bot, `anchor_charid` = puppet. Testing aid: first call requests a trade, second offers 5 gil and confirms |

`despawn` (and so `summon` and `delete`) makes the bot leave its party first.

Placement is either `anchor_charid` (an online non-bot player: the bot stands 2 yalms
east, facing them) or `zone, pos_x, pos_y, pos_z` (server coordinates, `pos_y` is height).

### Status

Every 2nd time server tick the module writes `online, zone, pos_*, hp/max_hp, mp/max_mp,
status_updated` to `bot_players`, and a heartbeat to `server_variables.BotsHeartbeat`
(unix time). The dashboard treats the map server as down if the heartbeat is older than 15 s.
On boot, `OnInit` resets every `online` to 0.

### Market (auction house)

A simulated AH economy priced from ffxiah.com (server Asura), so a solo player can buy
what they need and sell what they find. `bot_market.cpp`, table `bot_market_prices`.

- **Snapshot.** `bot_market_prices` holds, per item and stack size: `price` (what the
  server uses), `lowest` (lowest sale in the item page's sales history), `median` and
  `stock` (from the category pages), `rate` (sold/day), `sales` (sales in the history),
  `source` (`lowest`, or `median` when the item page wasn't scraped or had no sales).
  Only items on this server's AH (`item_basic.aH > 0`; stacks only if stackable) are kept.
- **Stocked listings.** Every ~5 min (and on boot) the module tops each item up to
  `min(stock, kMaxListings = 10)` unsold listings at `price`, seller `0`, seller name
  `Market`. Stocked listings whose price no longer matches the snapshot are deleted and
  relisted. Their `date` is refreshed every ~30 min, because the search server's
  `EXPIRE_DAYS` would otherwise "return" them to charid 0's delivery box. Players buy
  them through the normal AH code; seller 0 means no one is paid.
- **Buyout.** Every ~5 s, any player listing (seller != 0) that has been up for 10 s and
  is priced `<= price` is marked sold to `Market` at the asking price. The
  `auction_house_buy` trigger sends the gil to the seller's delivery box, the same as a
  sale to a player, and the seller gets a chat line if online. Items not in the snapshot,
  or priced above it, just sit until they expire like on any quiet AH.

**Taking a new snapshot.** ffxiah answers plain HTTP clients with 403, so `crawl.js` runs
in a normal browser tab on www.ffxiah.com (paste it into the console, or the built-in
browser's JS tool), at one request per second:

1. `await mktBrowse()`: ~65 category pages (about a minute).
2. `mktSales()`: ~13k item pages, busiest items first, resumable via localStorage,
   progress in `window.__crawl`. Takes hours (a background tab is throttled to ~30/min).
3. `await mktExport('browse')` and `await mktExport('sales')`: save each result to a
   file, then
   `python modules/bots/market/load_snapshot.py import-browse <file>` /
   `import-sales <file>`.
4. `python modules/bots/market/load_snapshot.py load` (`--price median` to price from
   the median instead, `--dry-run` to only print counts). The running server picks the
   new prices up at its next restock; no restart needed.

The lowest sale is sensitive to outliers (recent single Fire Crystal sales on Asura ran
from 565 to 10,000 gil): that price is both what stocked listings cost and the buyout threshold.

## Running

Map server: nothing to do once `bots` is in `modules/init.txt` and xi_map is rebuilt.
Look for `bot_players: module loaded` and `bot_players: boot spawn ...` in `log/map-server.log`.

Dashboard (from the repo root):

```
python modules/bots/dashboard/dashboard.py [--port 8088]
```

Open http://127.0.0.1:8088. It reads DB credentials from `settings/network.lua`
(falling back to `settings/default/network.lua`), creates the tables if missing, and has
**no authentication**, so it only listens on 127.0.0.1.

In game (any character, `permission = 0`):

```
!bot list
!bot spawn <name>
!bot despawn <name>
!bot here <name>     move the bot next to you (that spot becomes its home)
```

## Testing the AI without a client

A **puppet** is a bot with the AI off that other bots treat as a player, so it can stand
in for a party leader. Queue these as rows in `bot_commands` (`move` and `puppet` are not
exposed by the dashboard API):

```
create  Testlead near the bot, autospawn 0      -> charid N
puppet  charid N, autospawn 1
invite  charid <bot>, anchor_charid N           -> log: "<bot> joined Testlead's party"
move    charid N, zone/pos in the same zone     -> bot walks after it (watch bot_players.pos_*)
move    charid N, another zone                  -> bot changes zone with it
trade   charid <bot>, anchor_charid N  (twice)  -> log: "<bot> traded with Testlead", 5 gil moved
engage  charid N                                -> puppet attacks the nearest monster; the bot joins in,
                                                   cures the puppet under 65% HP, both get exp
despawn charid N                                -> bot leaves the party and goes home
delete  charid N                                   (the `bot<N>` account stays and is reused)
```

Checked this way on 2026-11-15, West Ronfaure, level 1 WAR puppet + Lamaza: party table
zone after a zone change, trade, scroll learned on spawn, walking into melee range from
26 yalms, club skill-ups, exp and a level up for both, Cure on the puppet at 24/37 HP.

### Not checked in a client

- How the party window, trade window and treasure messages look.
- Resting, the knocked-out and Raise path, and the pass-and-lot loot path: the test kills
  dropped nothing through the pool, and nobody died.
- Warp/Teleport/Escape cast on a party with a bot in it (the bot should stay and then
  follow the leader).

Every transition is logged (`bot_players: Lamaza joined ... / heads home / is back home /
moved from A to B`). What this can't check is how movement looks in the client.

## Gotchas found the hard way

- **Don't use `CPPModule::lua`.** The member is a reference bound during static
  initialization, before the global Lua state exists, and crashes on first use. Use `::lua`.
- **`char_insert` trigger.** Inserting into `chars` already creates `char_equip, char_exp,
  char_history, char_inventory, char_jobs, char_pet, char_points, char_profile,
  char_storage, char_unlocks`. Inserting those yourself fails with duplicate keys. Only
  `char_look`, `char_stats`, `char_flags` need explicit inserts. Use `UPDATE char_jobs`
  for levels. `char_delete` cleans up everything, so delete via `chars`.
- **`accounts_sessions.accid` is UNIQUE.** One online character per account, hence one
  account per bot.
- **DB queries must be compile-time strings** (`db::preparedStmt` takes a checked query),
  so no `fmt::format` for SQL. Use one statement per variant.
- **`charutils::removeCharFromZone` dereferences `PSession`.** Don't use it for bots; despawn
  calls `CZone::DecreaseZoneCounter` directly and frees the entity one zone tick later.
- **Code paths that assume a session** (448 `PSession` references in `src/map`). Anything
  new that a bot triggers (parties, trades, events, zoning) needs checking for this.
- **Moving a bot between zones** (`moveToZone`): `DecreaseZoneCounter`, clear
  `PTreasurePool` (`IncreaseZoneCounter` refuses a char that still has one), then
  `IncreaseZoneCounter`. The target index changes, so `accounts_sessions.targid` is updated.
  Queue the move from `think()` and apply it after the loop; it edits the per-zone list.
- **Party leader pointer.** `CParty::GetLeader()` is null while the leader zones, and a
  leader who logs off hands the lead to the oldest other member with a session row, which
  can be a bot. `think()` handles both; don't assume a party has a player leader.
- **`chars.pos_zone` is not updated by `SaveCharPosition`**, and the party window reads each
  member's zone from it (`GP_SERV_COMMAND_GROUP_TBL`), showing HP/MP only for members in
  the viewer's zone. `moveToZone()` and `spawn()` write `pos_zone` / `pos_prevzone`
  themselves before `IncreaseZoneCounter`.
- **Core zone changes need a session.** Warp, Teleport, `setPos` to another zone and the
  homepoint after a KO put a char in `m_charsToChangeZone`, whose processing dereferences
  `PSession`. The one core change for bots is in `CZoneEntities::charTick`
  (`src/map/zone_entities.cpp`): chars without a session are not queued. `think()` then
  clears the request (`cancelZoneRequest`).
- **`char_delete` leaves `char_flags` and `char_history` rows.** The next bot that gets the
  same charid, bot or player (the client shows FFXI-3101), then fails in the `char_insert`
  trigger. `remove()` deletes both, and `char_history` again one zone tick later: the
  deleted bot's entity writes that row once more when it is freed.
- **A lone party member gets drops directly; two members means a lottery.** That is why
  bots pass and lot for the player instead of just staying out of the pool
  (`CMobEntity::DropItems` needs every member to have one).
- **Look vs equipment.** Setting `look` alone gives the right appearance but an empty
  inspect window. Give real items and equip them with `charutils::EquipItem`.

## Next steps (ideas)

1. Combat depth: weapon skills at 1000 TP, job abilities, status removal, nukes for mages
   (who should stay at range instead of meleeing), spreading out around the target.
2. Follow through Mog House exits and into instances; hand the party lead to another
   player instead of leaving when the leader logs off and other players remain.
3. Subjobs; buying or being handed better gear and swapping it in; gil share back to players.
4. Dashboard: edit gear/level, more jobs, per-bot behaviour settings.
