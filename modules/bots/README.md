# Bot Players

Server-controlled characters that the FFXI client sees as real players. The goal is
party members for soloing on a private server: bots that follow, heal and fight.

> Not retail behaviour. Nothing here can be checked against retail captures. The only
> test that counts is how bots look and behave in the client.

## Status

| Works | Not done yet |
| --- | --- |
| Bots are real `chars` rows, spawned into zones as PCs (0x00D packets, white names) | Any AI: bots stand where they are placed |
| New bots get the normal new-character setup (`xi.player.charCreate`): race gear, job weapon, nation ring, gil, home point | Party invites, trades and `/tell` to a bot (routed, unanswered, untested) |
| Starting weapon/ring auto-equipped; inspect shows real gear | Bots can only start as WAR/MNK/WHM/BLM/RDM/THF (the 6 jobs unlocked on a new char) |
| Bots with `autospawn = 1` spawn at their saved position on server boot | Learning spells from scrolls (Cure scroll sits in the inventory) |
| Spawned bots appear in `/sea` | Subjobs, levelling, gear changes after creation |
| Web dashboard: create, inspect, spawn/despawn, move next to a player, autospawn, delete | Multi map-process setups (only tested with one xi_map hosting all zones) |
| In-game `!bot list/spawn/despawn/here` | |

Current bots: **Lamaza** (charid 4), Mithra White Mage lv1, Southern San d'Oria, autospawn on.

## Layout

```
modules/bots/
  cpp/bot_players.cpp      C++ module (CPPModule): spawning, command queue, status, Lua bindings
  lua/bot_command.lua      !bot GM command (permission 0)
  sql/bot_players.sql      bot_players + bot_commands tables (also auto-created, see below)
  dashboard/dashboard.py   local web dashboard (Python stdlib + mariadb connector)
  dashboard/index.html     dashboard page (vanilla JS, no build step)
```

The module is enabled by the `bots` line in `modules/init.txt`. CMake globs the `.cpp`,
the Lua loader picks up the `.lua`. Changing the C++ needs a rebuild of `xi_map`; changing
the Lua needs a map server restart (or file-watch reload).

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

Placement is either `anchor_charid` (an online non-bot player: the bot stands 2 yalms
east, facing them) or `zone, pos_x, pos_y, pos_z` (server coordinates, `pos_y` is height).

### Status

Every 2nd time server tick the module writes `online, zone, pos_*, hp/max_hp, mp/max_mp,
status_updated` to `bot_players`, and a heartbeat to `server_variables.BotsHeartbeat`
(unix time). The dashboard treats the map server as down if the heartbeat is older than 15 s.
On boot, `OnInit` resets every `online` to 0.

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
!bot here <name>     move the bot next to you
```

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
- **Look vs equipment.** Setting `look` alone gives the right appearance but an empty
  inspect window. Give real items and equip them with `charutils::EquipItem`.

## Next steps (ideas)

1. AI controller: follow a leader, assist, cure when party HP is low (`CPlayerController`
   is what bots have today; a bot controller or Lua-driven behaviour on top of `PAI`).
2. Party support: let a player invite a bot (handle `InvitePending` for bots), keep the
   party across zoning.
3. Learn spells from scrolls on creation; subjobs; levelling with the party.
4. Dashboard: edit gear/level, more jobs, per-bot behaviour settings.
