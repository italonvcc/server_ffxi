# Bot Players

Server-controlled characters that the FFXI client sees as real players. The goal is
party members for soloing on a private server: bots that follow, heal and fight.

> Not retail behaviour. Nothing here can be checked against retail captures. The only
> test that counts is how bots look and behave in the client.

## Status

| Works | Not done yet |
| --- | --- |
| Bots are real `chars` rows, spawned into zones as PCs (0x00D packets, white names) | Opening a skillchain, weapon skills outside one, job abilities other than Provoke, Curaga, status removal (-na, Erase), Raise, sleep/crowd control |
| New bots get the normal new-character setup (`xi.player.charCreate`): race gear, job weapon, nation ring, gil, home point | `/tell` to a bot (routed, unanswered, untested) |
| Starting weapon/ring auto-equipped; inspect shows real gear | Bots can start as WAR/MNK/WHM/BLM/RDM/THF and RNG/SAM/NIN; other jobs need starting gear and an unlock (see "First spawn") |
| Gear traded to a bot is put on; what it can't wear and what it replaced goes back to the player. Traded food is eaten and re-eaten | Subjobs; choosing gear by itself (it wears whatever it is handed) |
| Ranger shoots, keeps its distance, re-equips the best arrows it carries, melees without ammo | Trading, food and the Ranger are untested (see "Not checked") |
| Bots with `autospawn = 1` spawn at their saved position on server boot | Gil from kills is still split with bots (`charutils::DistributeGil`, core) |
| Spawned bots appear in `/sea` | |
| Web dashboard: create, inspect, spawn/despawn, move next to a player, autospawn, delete | Multi map-process setups (only tested with one xi_map hosting all zones) |
| In-game `!bot list/spawn/despawn/here` | Alliances (alliance invites are declined) |
| **AI:** accepts party invites, follows the party leader (also into other zones), walks home when it has no party | Following into Mog Houses and instances (the bot waits outside) |
| **AI per job** (`cpp/bot_roles.h`): White Mage, Red Mage and Black Mage have rule lists of heal, buff, debuff, nuke, melee, rest; other jobs fight and cure. Bots coordinate so they don't overheal or double up | Rule lists for the other jobs; bots stand on the same spot when they melee the same target |
| **AI:** takes a Raise or gets up by itself when knocked out | The per-job AI is untested in a fight (see "Not checked") |
| **AI:** accepts trades, passes on loot (a lone player gets every drop), learns scrolls it carries | In-client checks of all of the above (see "Not checked in a client") |
| Exp, levels and skill-ups work like a player's; the dashboard shows exp, skills and spells | |

Current bots: **Lamaza** (charid 4), Mithra White Mage, Southern San d'Oria, autospawn on.

## Layout

```
modules/bots/
  cpp/bot_players.cpp      C++ module (CPPModule): spawning, command queue, status, AI, Lua bindings
  cpp/bot_roles.h          AI data: rule list per job, spell tables, cure and resistance estimates
  cpp/bot_market.cpp       C++ module: auction house stocked and bought from a price snapshot
  market/crawl.js          ffxiah.com scraper, runs in a browser tab
  market/load_snapshot.py  imports scraper dumps, builds data/snapshot.json, fills bot_market_prices
  market/data/             the snapshot (browse_raw.json, sales_raw.json, snapshot.json)
  lua/bot_command.lua      !bot GM command (permission 0)
  lua/bot_orders.lua       !ja and !ws: tell a bot to use an ability or weapon skill
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
   anything in the inventory that fits an empty slot. `charCreate` only knows the six
   starting jobs, so for RNG, SAM and NIN `create` unlocks the job and sets it to level 1
   in `char_jobs`, and `spawn` adds the weapons of `bots::kStarterGear` (`bot_roles.h`):
   Shortbow, 99 Wooden Arrows and a Bronze Axe; Mumeito (the only level 1 great katana);
   a Kunai. Core change for this: `scripts/globals/player.lua` tolerates a job with no
   starting gear (`startingJobGear[job] or {}`), instead of throwing before the ring and
   title are given.
7. **Permanent buffs.** Every spawn calls `xi.custom.permanentBuffs.apply`
   (`modules/custom/lua/permanent_buffs.lua`, toggles `custom.PERMANENT_SIGNET` /
   `custom.PERMANENT_IMAGERY`), because bots never go through `xi.player.onGameIn`. The
   effects have no timer and the entity survives `moveToZone`, so once is enough; the
   module puts them back itself if something removes them.

### AI

Runs in `OnZoneTick` (every 400 ms logic tick), only for the bots in the zone that is
ticking (`byZone_` index). Per bot, `think()` does, in order:

1. **Invite.** If `InvitePending` is set (a player invited the bot from the party menu or
   with `/pcmd add`), it is answered after `kInviteDelay` (1.2 s) with the same outcome as
   the client answering "yes": the inviter's party is created if needed and the bot is added
   (`CParty::AddMember`). Declined (the inviter gets the normal "declined" message) when it
   is an alliance invite, the bot already has a party, the inviter is a bot, the inviter
   has trusts out, or the party is full.
2. **Follow.** In a party whose leader is a real player, each bot has its own spot behind
   or beside the leader (`kFollowSpots`: two behind to the sides, one in a second row, two
   wide), picked by its place among the party's bots, melee jobs first so mages stand
   further back. The spot turns with the way the leader faces and is moved by an amount
   fixed per bot (up to 0.5 yalm and 12 degrees, from its charid), so the group is never
   regular. The bot paths to the spot (`PathTo`), or to anywhere at that distance from the
   leader when the spot is not walkable, and re-paths as the spot moves, so the party runs
   as a loose fan instead of a column. On arrival it faces roughly where the leader faces.
   A standing bot stays where it is until the leader is `kFollowSlack` (2 yalms) further
   away than its spot (so turning on the spot moves nobody), then waits up to 0.5 s, each
   bot its own time, before setting off. Paths come from the zone navmesh through
   `CPathFind`, the same code mobs and trusts use; PCs have no pathfinder, so `spawn()`
   gives the bot one. More than `kWarpDistance` away, or 3 failed path queries in
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
is looked at (once a second), and nothing else happens while the bot is casting, aiming
or using an item (moving would interrupt it). After that, what a finished trade brought is
dealt with, and the bot eats if its food effect ran out (see **Traded items**). With a leader in the same zone, `assist()` runs
before `follow()`:

- **Rules per job.** `cpp/bot_roles.h` has an ordered rule list per job. Every tick the
  bot walks its list and the first rule with something to do wins:

  | Job | Rules, top to bottom | Heal rank | Support rank |
  | --- | --- | --- | --- |
  | WHM | Heal, Buff, Debuff, Melee (level <= 6), Rest | 2 | 1 |
  | RDM | Heal, Buff (MP >= 50%), Debuff, Nuke (MP >= 50%), Melee, Rest | 1 | 2 |
  | BLM | Heal, Debuff, Nuke, Melee (level <= 6), Rest | 0 | 0 |
  | RNG | Heal (if it knows a Cure), Skillchain, Shoot, Melee, Rest | 0 | 0 |
  | THF | Heal (if it knows a Cure), Steal, Skillchain, Melee, Rest | 0 | 0 |
  | other | Heal (if it knows a Cure), Provoke, Skillchain, Melee, Rest | 0 | 0 |

  The three mage lists also have Skillchain just before Melee. A rule can have a highest
  level (`Rule::maxLevel`) and a lowest MP% (`Rule::minMpp`): the Red Mage melees at every
  level, and under half its MP only cures and debuffs.

  Spell rules are looked at every `kDecisionInterval` (0.8 s), never while casting, and
  not for `kCastGap` (3 s) after a spell.
- **Smart Heal.** A patient is a party member within `kHealReach` (30 yalms) under
  `kHealBelowHpp` (85%), counting the cures already being cast on them. One that is out of
  cast range (18) is walked to first (`walkToPatient`, ahead of every other rule, so a mage
  leaves its stand-off spot for it). The bot casts the biggest Cure whose
  estimate (the Lua cure formula, mirrored in `bots::cureAmount`) fits in what is missing,
  with 10% tolerance; if even Cure would overheal it waits. Under 35% it uses the biggest
  Cure it has.
- **Smart Buff.** Never in a town (`inTown`). `bots::kBuffs`, in order: Protectra/Protect, Shellra/Shell, Regen (a
  member under 80% in a fight), Haste (non-mage jobs), Refresh (jobs with MP), Phalanx,
  and Stoneskin/Blink/Aquaveil on itself when nobody is fighting. Highest tier known. A
  -ra spell is used when two or more members around the bot lack the effect. Cast any
  time; during a fight only while MP stays above 40%.
- **Smart Debuff.** `bots::kDebuffs`, in order: Bio, Dia, Paralyze, Slow, Blind, Burn,
  Choke, Shock, Poison (bot level <= 20), then Dispel if the enemy has something
  dispelable. Bio and Dia replace each other, so only one goes on (Bio when a bot has it;
  neither is cast while the other is on the enemy or was just cast). Burn, Choke and Shock
  are the elemental debuffs that don't cancel one another; Frost, Rasp and Drown are not
  used. Skipped when the enemy has the effect, is immune, has resistance rank 4+ to it,
  or is under 25% HP. Only while MP stays above 40%, for the healer roles, White Mage and Red Mage
  (`reserveOf`); every other bot spends everything, also a Black Mage with a Cure from
  its subjob, which only cures a member under 35% (`kEmergencyHpp`). A better debuffer (`supportRank`) that has the
  spell goes first, unless it is casting or in its gap between casts: then a lower one
  takes the line, so the Red Mage and the Black Mage debuff side by side as a fight starts.
- **Smart Spell (nuke).** Of the elemental spells the bot knows and can afford while
  keeping 50% MP (nothing kept outside the healer roles): highest `base damage x resistance-rank multiplier x damage-taken (SDT)`
  for the enemy; elements it absorbs or nullifies are left out. Not in the first 8 s
  (`kNukeHold`) after the party got the enemy's attention (`Bot::foeSince`), so the tank
  gets ahead on enmity; debuffs fill that time.
- **Enemy "already attacked".** Debuffs, nukes and a mage's melee need a party member on
  the monster's enmity list, or party claim. A mage never pulls. Other jobs attack as soon
  as the leader engages. The fight target is the leader's, else another player's.
- **Coordination.** Nothing is shared up front; each bot looks at the party when it decides.
  - Spells party members (bots and players) are casting right now count: a Cure on its way
    is subtracted from what the patient needs, a buff/debuff on its way counts as applied.
  - Before a job, a bot checks for another bot with a higher rank for it (heal rank for
    heals, support rank for buffs/debuffs; equal ranks: lower charid) that could do it now:
    in range, not silenced or asleep, has the spell and the MP, and no cast refused in the
    last 3 s. If there is one, it leaves the job to it.
  - Heals: the best free healer takes the worst patient, the next healer the second worst.
    While the better healer is busy casting, a lower one only helps patients under 70%
    (`kHelpBelowHpp`). A better healer that is not within cast range of the worst patient
    does not count, and under 35% (`kEmergencyHpp`) every healer goes for the worst patient.
  - After a buff or debuff is cast, no bot casts that effect on that target again for
    15 s / 45 s (`tried_`), so a resisted debuff is not spammed.
- **Provoke.** A bot that has the ability (Warrior main or sub, level 5) uses it when the
  enemy's battle target is a party member whose main job is not WAR, PLD, NIN or RUN
  (`bots::isTank`). Checked once a second per bot: ability known, recast ready, within its
  16 yalms. After one bot provokes, no other does on that enemy for 5 s (`provoked_`).
- **Skillchain.** A bot never uses a weapon skill on its own: it keeps its TP until
  somebody else's weapon skill opens a skillchain on the enemy (the core's `Skillchain`
  status effect on the mob; it is only there once a skill has landed). Then an engaged bot
  with 1000 TP looks for a weapon skill it has that continues the chain
  (`battleutils::FormSkillchain` against what is open: the opener's properties, or the one
  property the last step made), in range, and picks the one giving the highest chain.
  Timing: the core refuses to chain a skill landing less than 3 s after the last one, and
  then restarts the chain with it, and the window closes 10 s after the opener, 1 s sooner
  per step. A bot starts 3.5 s after the last skill landed and not in the last 1.2 s. One
  bot answers each step (`chained_`); the next step is then open to the others, so a party
  goes back and forth, e.g. Fast Blade > Flaming Arrow (Liquefaction) > Fast Blade
  (Scission). A bot skill that misses leaves the window as it was.
- **Steal (Thief).** Used on the enemy the bot is fighting whenever the recast is up
  (checked once a second, needs a free inventory slot). 3 s later whatever is new in its
  inventory is handed to the party leader through the give-back queue of traded items.
- **Melee.** The bot engages (`PAI->Engage`, the same path as a client), walks into melee
  range, faces the target, and lets the normal attack state swing. It switches target with
  the leader and disengages when the leader does. State `fight`.
- **Shoot (Ranger).** With a ranged weapon and ammo that fits it (arrows for a bow, bolts
  for a crossbow, bullets for a gun; same skill, and same sub-skill outside archery), the
  bot draws on the target, stops `kShootHold` (12 yalms) from it and calls
  `PAI->RangedAttack` `kShotGap` (1.5 s) after the last shot ended. `think()` does nothing
  while the bot is in `CRangeState`. Three attempts in a row that never reach that state
  (no line of sight): it walks up to 3 yalms. When the last arrow is shot the core empties
  the ammo slot; the bot then equips the hardest-hitting usable stack in its inventory
  (`equipBestAmmo`, one inventory scan, also after a trade and a level up). Nothing left:
  the rule gives up and the Melee rule below it walks in. Like other non-mage jobs it
  starts as soon as the leader engages.
- **Position.** A mage that does not melee keeps about `kMageHold` (17 yalms) from the
  enemy, out of most area attacks (`keepDistance`): closer than `kMageNear` (15) it backs
  straight away from it (or at an angle when that spot is not on the navmesh; it stays
  when neither is), further than cast range (18) it closes in. It does not back away from
  an enemy that is attacking the mage itself. It backs off once per enemy, as the fight
  starts (`Bot::spacedFor`); after that it only ever closes in. It also closes in when it is more than
  `kMageLeash` (22) from the party leader; when no path to the enemy is found it goes to
  the leader, and after 3 failures it warps there.
- **Rest.** Standing still for 2 s with nothing to do: sits down (`Healing` effect, like
  `/heal`). A mage sits when MP < 100%, HP <= 75% or the leader is resting; other jobs when
  HP or MP is missing or the leader rests. Nobody sits while a party member is fighting,
  or while the bot has a buff waiting that it has the MP for, even if the spell's recast or
  the gap between casts is still running (`buffWaiting`, looked at once a second). State
  `rest`. Out of combat everybody also recovers standing up, and faster sitting: see
  `OUT_OF_COMBAT_REGEN` in `settings/default/custom.lua`.
- **Orders.** `!ja <bot> "<ability>"` and `!ws <bot> "<weapon skill>"` (`lua/bot_orders.lua`,
  `giveOrder`/`carryOut`) for a bot in the player's own party. The client's own `/ja` can't
  carry a bot name: it rejects the line before it reaches the server. The target is the
  player's target; an ability that can't be used on it goes on the bot itself when that is
  allowed, a weapon skill with no enemy targeted goes on what the bot is fighting. Names
  ignore case, the quotes are optional and anything after the name is ignored. What can
  be told at once is answered in chat (no such bot, not in your party, not learned,
  recast, TP). Otherwise the order sits on the bot (`Bot::order`) and `think()` carries it
  out ahead of the job rules as soon as the bot can act: it stands up, walks into range
  (engaging first for a weapon skill) and uses it. Not started within 8 s, it is dropped
  and the player is told. An ordered weapon skill that lands opens a skillchain like a
  player's, and the other bots continue it.
- **Knocked out.** State `dead`. A Raise cast on the bot is accepted after 4 s. With no
  Raise, the bot gets up by itself after `kSelfRaiseDelay` (60 s) as if raised (weakened,
  normal exp loss): the homepoint warp needs a session, see the gotchas.

**Trade.** A player's trade request is accepted after 1.2 s and confirmed 1.5 s later; the
bot keeps confirming (changing an offer clears confirmations), so the trade completes when
the player confirms. The bot never offers anything. Requests from other bots are refused.

**Traded items.** When a trade opens the bot notes the trader and which inventory slots are
in use; what is in any other slot afterwards came with the trade (`handleTradedItems`).

- *Gear* is put on with `charutils::EquipItem`, always, even when it is worse. The job,
  level and race checks are made first (`canWear`): `EquipArmor` empties the slot before it
  finishes checking. Weapons go in the main hand, or the off hand when the bot has Dual
  Wield, a one-handed main weapon and nothing there; rings and earrings fill an empty slot
  first. Ammo that fits the ranged weapon is kept as spare stacks and the best is equipped.
- *Handed back* to the trader: gear the bot can't wear, the piece that was replaced, and
  whatever `EquipItem` took off on the way (the sub weapon under a two-handed weapon, ammo
  that no longer fits). `giveTo()` moves the stack with two `ItemClaimTransaction`s (take
  from the bot, give a clone to the player, the player's side committed first), so the
  player sees "Obtained". If that fails (inventory full, a rare item they already have,
  another zone, offline) the bot keeps the item and tries again every 30 s. That list is
  in memory only: after a despawn or restart the item just stays with the bot.
- *Food.* There is no food flag in the item data. An item counts as food when it is usable,
  in auction house category 51-59 and its `onItemCheck` answers 0 or "is full" (246);
  "cannot eat" (247, raw food and the wrong race) is handed back. The id is kept in
  `bot_players.food`. Every 5 s, in a party, a bot without the `Food` effect eats one
  (also mid-fight), but never in a town (`inTown`: a zone of the City type); food handed
  over there is eaten once the bot is out in the field. A different food replaces it: the old stacks go back to the trader,
  the old effect is removed and the new food is eaten at once. About 50 food items have
  no auction house category and are not recognised.
- Anything else (potions, materials) stays in the bot's inventory.

**Loot.** A bot passes on every item in its treasure pool. When the pool has exactly one
player, each item is then lotted on that player's behalf, which awards it immediately, so
drops behave as if the player were solo. With two or more players the bots just pass and
the players lot as usual. A full inventory or an already-owned rare item leaves the item
in the pool for the player to deal with. Gil is not covered: it is split in the core.

**Items.** An inventory scan runs only when flagged: on spawn, after a trade, after a level
up. It equips gear that fits an empty slot (gear that was too high a level when it was
traded is not picked up this way if it was handed back) and uses one scroll the bot can learn
(`onItemCheck` says so), through the normal item-use state, so the scroll is consumed and
the spell is saved. Scrolls for a higher level are kept and tried again on level up.

**Home** is `bot_players.home_*`: wherever the bot was last placed (`create`, `summon`,
`!bot here`, `spawn` with a position). `spawn` without a position, and autospawn on boot,
put the bot at home, not where it happened to be when the server stopped.

`bot_players.ai_state` (`idle` / `follow` / `wait` / `return` / `fight` / `rest` / `dead`),
`weapon_skills` (the names of the weapon skills the bot can use with the weapons it
holds, rewritten when the set changes), `leader`, `level`, `exp`, `exp_next` and `action` (the spell just cast, e.g.
`cure > Kenji`, empty 8 s later) are live columns; the dashboard shows them, plus skills
(`char_skills`) and spells (`char_spells`). Every cast is also a debug log line
(`bot_players: Lamaza casts cure on Kenji`).

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
  while the target is out of reach and has moved. A spell decision (every 0.8 s per bot)
  walks the party (6 entries) a few times and the bot's cached spell kit: pointer reads
  only, no Lua, DB or navmesh. The kit is rebuilt on spawn, level up and after a scroll. The treasure pool check is one pointer and one counter per second
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
| `create` | `name, race (1-8), face (0-15), size (0-2), job (1-6, 11-13), level, nation (NULL = race home), autospawn`, plus placement |
| `spawn` | `charid`, optional placement (default: saved position) |
| `despawn` | `charid` |
| `summon` | `charid`, placement (despawn + spawn there) |
| `autospawn` | `charid, autospawn` |
| `delete` | `charid` (despawns, deletes `chars` row; the `char_delete` trigger removes the rest) |
| `jobs` | `charid`, `job` (new main job, left out = keep), `subjob` (new subjob, left out = keep, 0 = none) |
| `invite` | `charid`, `anchor_charid` = an online player who invites the bot (dashboard: "Join party of") |
| `move` | `charid`, placement. Testing aid: relocates without a despawn, keeps party and home |
| `puppet` | `charid, autospawn` (1 on, 0 off). Testing aid, see below. Not saved, off after a respawn |
| `engage` | `charid`. Testing aid: attack the nearest monster within 30 yalms (the result says where it is if further) |
| `trade` | `charid` = bot, `anchor_charid` = puppet. Testing aid: first call requests a trade, second offers 5 gil and confirms |

`despawn` (and so `summon` and `delete`) makes the bot leave its party first.

**Jobs.** Every bot has the subjob unlocked (bit 0 of `char_jobs.unlocked`, set at spawn).
`jobs` (`changeJobs` / `changeLiveJobs`, the Job and Subjob selects of the dashboard's detail
panel) is a real job change: the steps of the client's own job change packet
(`GP_CLI_COMMAND_MYROOM_JOB::process`), without the Moogle. Jobs are the nine a bot can be
created as; a job the bot never had is unlocked at level 1, each job keeps its own level
and the subjob counts for half the main job. Picking the current subjob as main job swaps
the two. Refused while the bot is knocked out, in a fight or trading. Gear: what it wore
on that job before comes back, the first time on a job it gets that job's starting gear
(`bots::kStarterGear` / `kFirstJobGear`, remembered in the char var `[bot]gearedJobs`), then
anything in its bag that fits an empty slot is put on. Spells are not given: it knows what
it learned from scrolls. The AI starts over as the new job on the next tick (`kitDirty`).
A bot that is not spawned is spawned for the change and despawned again.

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
On/off: `custom.BOT_MARKET` in `settings/default/custom.lua` (false = vanilla AH).

- **Snapshot.** `bot_market_prices` holds, per item and stack size: `price` (what the
  server uses: ffxiah's median), `median`, `stock` and `rate` (sold/day) from the
  category pages, and `lowest`/`sales` from item pages when those were scraped (optional,
  see below; `source` says which one `price` came from).
  Only items on this server's AH (`item_basic.aH > 0`; stacks only if stackable) are kept.
- **Stocked listings.** Every item in the snapshot has a median, so it has sold at some
  point: each gets `clamp(stock, kMinListings = 5, kMaxListings = 10)` unsold listings at
  `price`, seller `0`, seller name `Market` (~68.5k rows). They are dated 2096
  (`kNeverExpires`): the listing date is only used by the search server's `EXPIRE_DAYS`,
  which would otherwise "return" them to charid 0's delivery box. Players buy them
  through the normal AH code; seller 0 means no one is paid.
- **Price history.** Each item also gets `kHistoryRows = 10` sold rows at `price`, seller
  and buyer `Market` (both ids 0), so the AH history window shows the median. They are
  spaced one sale per `1 / rate` days (1 h to 7 days apart) back from when they were
  written. Real sales are newer, so they push fake ones out of the 10 the client is sent.
  Real rows are never touched: buying stock has `buyer != 0`, a buyout has `seller != 0`.
- **Upkeep.** Every 2.4 s tick handles one window of 512 item ids (`kWindow`): reprice
  stale listings and history, un-expire old listings, top up listings, top up history.
  The id range keeps each query on an index (`idx_ah_active` / `idx_ah_history`) and
  under ~15 ms, so a full cycle over all ids takes ~5 min. That is how soon bought stock
  comes back (measured: 210 s) and how soon a new `load` is applied. A window that writes
  `kChunk` = 2,000 rows in one step is redone on the next tick (a fresh snapshot is ~200k
  rows). The old whole-table version took 140-480 ms per query and flooded the map log
  with slow-query warnings.
- **Buyout.** Every ~5 s, any player listing (seller != 0) that has been up for 10 s and
  is priced `<= price` is marked sold to `Market` at the asking price. The
  `auction_house_buy` trigger sends the gil to the seller's delivery box, the same as a
  sale to a player, and the seller gets a chat line if online. Items not in the snapshot,
  or priced above it, just sit until they expire like on any quiet AH.

**Taking a new snapshot.** ffxiah answers plain HTTP clients with 403, so `crawl.js` runs
in a normal browser tab on www.ffxiah.com (paste it into the console, or the built-in
browser's JS tool), at one request per second:

1. `await mktBrowse()`: ~65 category pages (about a minute).
2. Optional, only for `--price lowest`: `mktSales()`: ~13k item pages, busiest items first, resumable via localStorage,
   progress in `window.__crawl`. About 3.7 h at 1 request/s; ffxiah answers faster crawls
   with HTTP 429 (16 parallel workers were cut off within 20 s).
3. `await mktExport('browse')` and `await mktExport('sales')`: save each result to a
   file, then
   `python modules/bots/market/load_snapshot.py import-browse <file>` /
   `import-sales <file>`.
4. `python modules/bots/market/load_snapshot.py load` (median prices; `--price lowest`
   uses the lowest sale where step 2 found one; `--dry-run` only prints counts). The running server picks the
   new prices up at its next restock; no restart needed.

Prices are the median on purpose. The lowest sale is sensitive to outliers (recent single
Fire Crystal sales on Asura ran from 565 to 10,000 gil), and getting it means crawling
every item page, which takes hours at the rate ffxiah allows.

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
!ja <bot> "<job ability>"     e.g. !ja Lamaza "Benediction"
!ws <bot> "<weapon skill>"    e.g. !ws Sigyni "Fast Blade"
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
                                                   heals the puppet, both get exp
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
- Orders, Steal, mage distance, no resting in fights or before a buff, the Red Mage
  changes (2026-11-15): only that the server starts with them and `!ja` / `!ws` register.
  Not seen in a fight: any of it.
- Job and subjob change (2026-11-16): through the API on bots outside a party: main job,
  subjob, swap, an advanced job, refusals, a despawned bot; jobs, levels, HP/MP and gear
  in the DB follow. Not seen: the change from a client (party list, /check, model), a
  change while in a party, a bot fighting on its new job or using its subjob's spells.
- Mages backing off only once per enemy, and no MP reserve outside the healer roles (2026-11-16):
  only that the server starts with them. Prompted by a Gusgen Mines fight where the Black
  Mage cast two debuffs and then nothing for two minutes; the 50% / 40% reserve is the
  likely cause (it had spent about that much), not a confirmed one.
- Bio, Burn, Choke and Shock (2026-11-16): only that the server starts with them.
- Mage leash, nuke hold and shared debuffing (2026-11-16): only that the server starts
  with them. The fight that prompted the leash (Valkurm Dunes, 00:31-00:33) shows the Black
  Mage casting on the party's target to the end, so why it looked out of range is not known.
- Healers walking to a patient out of range, and the second healer not waiting for one
  that can't reach (2026-11-15): only that the server starts with it. Written after a fight
  where the White Mage, 17 yalms from the enemy, cast no Cure for 48 s and the Red Mage left
  the healing to it; the range is the likely cause from the log, not a confirmed one.
- Provoke and skillchains (2026-11-15): only that the server starts with them. Not seen: a
  Provoke, a bot closing a chain, the timing against a real client, two bots in one window.
- The follow formation (2026-11-15): only that the server starts with it. How the group
  looks on the move, stopped, along walls and after zoning has not been seen.
- Traded gear and food, and the Ranger (2026-11-15): only that new RNG, SAM and NIN bots
  are created at level 1 with their gear on. Not seen: gear being equipped and handed back,
  a full player inventory, food being eaten and replaced, shooting, running out of arrows.
- The per-job AI (2026-11-15): only that the server starts and bots spawn with it. Heal
  sizing, buff and debuff order, the WHM/RDM hand-off, nuke choice, the mage holding its
  distance and resting during a fight have not been seen in a fight.
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
- **Running `xi_test` empties `accounts_sessions`.** A test server has no address, and the
  map server's boot cleanup (`map_engine.cpp`) then deletes every row, so the bots vanish
  from `/sea` while still standing in the world. `restoreSessions()` counts the bots' rows
  every 12 time server ticks (about 30 s) and puts the missing ones back.
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

1. Combat depth: bots opening skillchains and spending spare TP, more job abilities, Curaga, status removal, Raise,
   rule lists for more jobs, spreading out around the target.
2. Follow through Mog House exits and into instances; hand the party lead to another
   player instead of leaving when the leader logs off and other players remain.
3. Subjobs; buying gear and choosing the better piece; gil share back to players.
4. Dashboard: edit gear/level, the remaining jobs, per-bot behaviour settings.
