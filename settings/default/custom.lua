-----------------------------------
-- CUSTOM GAMEPLAY SETTINGS (server_ffxi fork)
-----------------------------------
-- Optional changes to retail behaviour. Every toggle here is false (or 1.0 for multipliers) = vanilla.
-- Copy this file to settings/custom.lua to override it locally.
-- Readable from C++ (settings::get<bool>("custom.NAME")) and Lua (xi.settings.custom.NAME).
-- Changes need an xi_map restart.
-----------------------------------

xi          = xi or {}
xi.settings = xi.settings or {}

xi.settings.custom =
{
    -- --------------------------------
    -- Experience points
    -- --------------------------------

    -- No party size penalty: every member in range gets the full solo share (1.00).
    -- Vanilla: 1.00 / 0.60 / 0.45 / 0.40 / 0.37 / 0.35 for 1-6 members (better under Signet).
    EXP_NO_PARTY_PENALTY = true,

    -- Each member's exp is computed from their own level vs. the mob (base exp, difficulty
    -- check, chains), as if they had killed it alone. A lower-level member gets more exp
    -- per kill than a higher-level one and catches up.
    -- Vanilla: graded against the highest level in range, lower members get a scaled-down
    -- share, and nobody gets exp if the mob is Too Weak for the highest member.
    EXP_USE_OWN_LEVEL = true,

    -- --------------------------------
    -- Skill-ups
    -- --------------------------------
    -- Multiply the skill gained per skill-up (not the chance of one). Rounded, at least 0.1.
    -- 1.0 = vanilla.

    -- Combat and magic skills, players and automatons. Not capped at vanilla's 0.9 per
    -- skill-up: one skill-up can give over 1.0 and raise the skill by more than one level.
    COMBAT_SKILLUP_MULTIPLIER = 5.0,

    -- A melee swing that does no damage (miss, parry, shadow, anticipate, counter) still rolls
    -- a weapon skill-up, the same way a hit does. false = vanilla (only damaging hits roll).
    SKILLUP_ON_MISS = true,

    -- Every combat or magic skill-up try succeeds instead of about 50% of them (players and
    -- automatons). Only the gain per skill-up stays random. Mobs still have to be high enough
    -- level, and capped skills still don't rise. false = vanilla.
    COMBAT_SKILLUP_GUARANTEED = true,

    -- Synthesis (crafting) skills: capped at 0.9 per skill-up.
    CRAFT_SKILLUP_MULTIPLIER = 5.0,

    -- --------------------------------
    -- Recovery
    -- --------------------------------
    -- modules/custom/lua/out_of_combat_regen.lua. "Out of combat": nobody in the character's
    -- party has a weapon drawn or is on a monster's enmity list. Players and bots.

    -- Out of combat, HP and MP come back as if resting while standing or moving (one resting
    -- tick every map.HEALING_TICK_DELAY seconds, growing tick after tick, no TP lost).
    -- false = vanilla (only /heal recovers).
    OUT_OF_COMBAT_REGEN = true,

    -- Sitting down (/heal) while out of combat recovers this many times the normal amount
    -- per tick. 1.0 = vanilla.
    OUT_OF_COMBAT_REST_MULTIPLIER = 5.0,

    -- --------------------------------
    -- Drops
    -- --------------------------------

    -- Multiplies every mob item drop chance (drop groups, single drops and scripted drops),
    -- on top of map.DROP_RATE_MULTIPLIER. A chance pushed past 100% always drops. 1.0 = vanilla.
    DROP_RATE_MULTIPLIER = 5.0,

    -- --------------------------------
    -- Auction house
    -- --------------------------------

    -- Simulated AH market (modules/bots, bot_market.cpp): stocks the AH from the ffxiah price
    -- snapshot in bot_market_prices and buys any player listing priced at or below the
    -- snapshot price. false = vanilla (the module does nothing; stocked listings already on
    -- the AH stay until bought or removed by hand).
    BOT_MARKET = true,

    -- --------------------------------
    -- Nomad Moogles
    -- --------------------------------

    -- Extra Nomad Moogles (Mog Safe, Locker, delivery box, job change) placed by
    -- modules/custom/lua/nomad_moogles.lua: one beside every Field Manual
    -- (modules/custom/lua/field_manuals.lua), plus the spots listed in that file.
    -- false = vanilla (only the retail Nomad Moogles).
    NOMAD_MOOGLES = true,

    -- Storage (the furniture container) at any Nomad Moogle, not only in your own Mog House.
    -- false = vanilla.
    NOMAD_MOOGLE_STORAGE = true,

    -- --------------------------------
    -- Teleports
    -- --------------------------------

    -- City Teleporter NPCs (modules/custom/lua/city_teleporter.lua): free teleport to
    -- San d'Oria, Bastok, Windurst, Jeuno, Whitegate, Adoulin or any Field Manual. One
    -- stands beside every Field Manual; the other spots are in that file.
    -- false = vanilla (no teleporter NPCs).
    CITY_TELEPORTER = true,

    -- The !home command (modules/custom/lua/home_command.lua): any player can warp to their
    -- home point, like the Warp spell. Not while fighting or knocked out.
    -- false = vanilla (the command answers that it is turned off).
    HOME_COMMAND = true,

    -- The !resist command (modules/custom/lua/resist_command.lua): any player can read a
    -- mob's live elemental resistances (rank, damage taken, magic evasion, absorb, null).
    -- Used by the Windower addon in modules/custom/tools/windower/resist.
    -- false = vanilla (the command answers that it is turned off).
    RESIST_COMMAND = true,

    -- --------------------------------
    -- Magic
    -- --------------------------------

    -- Day and weather always count: no 33% roll. Every elemental action (spells, blue magic,
    -- mob TP moves, skillchains, additional effects) gets its day/weather damage bonus and
    -- penalty every time, and weather always changes magic accuracy. Applies to mobs too.
    -- false = vanilla (bonuses and penalties apply on a 33% roll; an elemental obi forces
    -- only the bonus, Hachirin-no-Obi forces both).
    DAY_WEATHER_ALWAYS = true,

    -- Elemental damage rate by the target's resistance rank (the table is in
    -- scripts/globals/spells/damage_spell.lua):
    --   rank -3: x1.5   -2: x1.25   -1..1: x1   2: x0.75   3..5: x0.5   6..11: x0.25
    -- Weak elements now do extra damage. Rank 11 nukes roll resists like rank 10 instead of an
    -- automatic 1/4 (status effects stay immune). Magic evasion from the rank is unchanged.
    -- Used by nukes, blue magic, Drain/Aspir, Quick Draw, additional effects and spikes.
    -- Subtle Sorcery ignores the reductions, not the bonuses.
    -- false = vanilla (x0.5 at rank 4+, nukes at rank 11 always 1/4).
    RESIST_RANK_DAMAGE_RATE = true,

    -- --------------------------------
    -- Shops
    -- --------------------------------

    -- Regional vendors in the cities (Antonian, Oggodett, etc.) sell their region's goods
    -- whoever controls the region in conquest. The Tavnazia CoP mission gate still applies.
    -- false = vanilla (closed unless their nation owns the region).
    REGIONAL_VENDORS_ALWAYS_OPEN = true,

    -- Guild shops (crafting guild vendors) are open at every hour and on their guild
    -- holiday. false = vanilla (open only during their hours, e.g. 8:00-23:00, closed on
    -- their holiday when main.GUILD_SHOP_HOLIDAYS is on).
    GUILD_SHOPS_ALWAYS_OPEN = true,

    -- --------------------------------
    -- Inventory
    -- --------------------------------

    -- Main inventory size, applied at login (modules/custom/lua/inventory_size.lua). It only
    -- raises the inventory, never shrinks it, and is capped at 80 (the client's limit). The
    -- new size is saved, so it stays if this is set back. 30 or less = vanilla (30 slots,
    -- grown by the Gobbie bag quests).
    INVENTORY_SIZE = 80,

    -- --------------------------------
    -- Auction House
    -- --------------------------------

    -- Put an item up for sale as soon as the price is entered: the server lists it and charges
    -- the fee right away instead of asking the client to confirm the fee first.
    -- false = vanilla (fee and sale confirmation prompts).
    AUCTION_QUICK_LIST = true,

    -- Gil from an auction house sale goes straight into the seller's gil (within a few
    -- seconds, online or offline) instead of the delivery box
    -- (modules/custom/cpp/ah_gil_to_inventory.cpp). Unclaimed AH gil already in a delivery
    -- box is moved too. false = vanilla (collect it from the delivery box).
    AUCTION_GIL_TO_INVENTORY = true,

    -- --------------------------------
    -- Permanent buffs
    -- --------------------------------
    -- modules/custom/lua/permanent_buffs.lua. Given to players at login and zone-in and to
    -- bots at spawn, with no timer; put back a second after something removes them. Turning one off
    -- removes it at the next login or zone-in.

    -- Signet, always on. Its benefits still follow the retail rules (crystals and conquest
    -- points only from the original conquest regions). Comes back next to Sanction or Sigil.
    -- false = vanilla (Signet from a gate guard, on a timer).
    PERMANENT_SIGNET = true,

    -- Advanced image support for every craft at once: +3 to each synthesis craft, 10% less
    -- material loss, +2 fishing. Guild image support NPCs and craft belts are no longer needed
    -- (what they give is replaced by this a second later).
    -- false = vanilla (one craft at a time, from a guild NPC or belt, on a timer).
    PERMANENT_IMAGERY = true,

    -- --------------------------------
    -- Magic
    -- --------------------------------

    -- modules/custom/lua/custom_spells.lua. Where it says so, the game client has to be
    -- patched too (modules/custom/tools/patch_client.py), or it keeps showing and enforcing
    -- the retail values.

    -- En-spells (Enfire to Enwater, their II tiers, Enlight) can be cast on a party member
    -- within 20 yalms, like Phalanx II. Client patch. false = vanilla (self only).
    ENSPELL_PARTY_TARGET = true,

    -- Red Mage learns Haste at level 2. Client patch. false = vanilla (level 48).
    RDM_HASTE_LEVEL_2 = true,

    -- Haste costs 10 MP. Client patch. false = vanilla (40 MP).
    HASTE_MP_10 = true,

    -- Haste lasts its full time on targets below level 40. Server only.
    -- false = vanilla (cut to target level / 40 of it).
    HASTE_NO_LEVEL_PENALTY = true,

    -- Haste gives 30% haste, as much as Haste II (both 2998 of 10000). Server only.
    -- false = vanilla (14.65%).
    HASTE_30_PERCENT = true,

    -- Enstone lasts its full time on targets below level 18 (a party member under 18 gets
    -- all 3 minutes). Server only. false = vanilla (cut to target level / 18 of it).
    ENSTONE_NO_LEVEL_PENALTY = true,
}
