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
    -- Multiply the skill gained per skill-up (not the chance of one). Rounded, at least 0.1,
    -- and capped at 0.9 per skill-up (the most one skill-up can grant). 1.0 = vanilla.

    -- Combat and magic skills, players and automatons.
    COMBAT_SKILLUP_MULTIPLIER = 2.0,

    -- Synthesis (crafting) skills.
    CRAFT_SKILLUP_MULTIPLIER = 5.0,

    -- --------------------------------
    -- Drops
    -- --------------------------------

    -- Multiplies every mob item drop chance (drop groups, single drops and scripted drops),
    -- on top of map.DROP_RATE_MULTIPLIER. A chance pushed past 100% always drops. 1.0 = vanilla.
    DROP_RATE_MULTIPLIER = 5.0,
}
