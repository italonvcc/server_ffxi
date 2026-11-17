-----------------------------------
-- Custom spell changes
--
-- Applied to the loaded spells at server start (spell_list is left as retail). Each one has
-- a toggle in settings/default/custom.lua. Some also need the game client patched, or the
-- client keeps showing or enforcing the retail values: modules/custom/tools/patch_client.py.
--
--   ENSPELL_PARTY_TARGET   En-spells can be cast on a party member within 20 yalms, like
--                          Phalanx II. Client patch needed (target cursor).
--   RDM_HASTE_LEVEL_8      Red Mage learns Haste at level 8 (retail 48). Client patch needed.
--   HASTE_PROTECT_RECAST   Haste recasts in 5 s like Protect (retail 20 s). Client patch needed.
--   ENSPELL_PARTY_AOE      Every en-spell is cast on yourself and covers the party around you,
--                          like Protectra (its area and radius). Replaces ENSPELL_PARTY_TARGET.
--                          Client patch needed (target cursor: self, like Protectra).
--   ENSPELLS_LEVEL_2       Every en-spell at level 2 for its jobs (RDM Enfire-Enwater and II,
--                          PLD Enlight, DRK Endark). Client patch needed.
--   ENSPELLS_NO_LEVEL_PENALTY Every RDM en-spell lasts its full time on targets below its
--                          level. Server only.
--   HASTE_MP_10            Haste costs 10 MP (retail 40). Client patch needed (MP shown and
--                          checked by the client).
--   HASTE_NO_LEVEL_PENALTY Haste lasts its full time on targets below level 40 (retail: cut
--                          to level/40 of it). Server only.
--   HASTE_30_PERCENT       Haste gives 30% haste like Haste II (retail 14.65%). Server only.
--   ENSTONE_NO_LEVEL_PENALTY Enstone lasts its full time on targets below level 18 (retail:
--                          cut to level/18 of it). Server only.
--   BLM_TIER1_NUKES_LEVEL_1  Black Mage learns Water, Aero, Fire, Blizzard and Thunder at
--                          level 1 (retail 5/9/13/17/21). Client patch needed.
--   TIER1_NUKE_LEVEL_SCALING Player-cast Water, Aero, Fire, Blizzard and Thunder are equally
--                          strong: power and dINT multipliers scale with the caster's level,
--                          Stone's at level 1 rising in a straight line to Thunder's at 21 (top
--                          power TIER1_NUKE_POWER_CAP). MP and cast time are always Thunder's. Mobs, trusts and pets are unchanged. Client patch needed
--                          (the client checks MP against its own table: patched to 9).
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('custom_spells')

xi = xi or {}
xi.custom = xi.custom or {}
xi.custom.spells = xi.custom.spells or {}

local setting = function(name)
    return xi.settings.custom ~= nil and xi.settings.custom[name] == true
end

local partyRange = 20 -- Yalms, as Phalanx II (spell_list.spell_range 200)

-- Every en-spell with a scroll: ENSPELLS_LEVEL_2
local allEnspells =
{
    xi.magic.spell.ENFIRE,     xi.magic.spell.ENBLIZZARD,    xi.magic.spell.ENAERO,
    xi.magic.spell.ENSTONE,    xi.magic.spell.ENTHUNDER,     xi.magic.spell.ENWATER,
    xi.magic.spell.ENFIRE_II,  xi.magic.spell.ENBLIZZARD_II, xi.magic.spell.ENAERO_II,
    xi.magic.spell.ENSTONE_II, xi.magic.spell.ENTHUNDER_II,  xi.magic.spell.ENWATER_II,
    xi.magic.spell.ENLIGHT,    xi.magic.spell.ENDARK,
}

local enspells =
{
    xi.magic.spell.ENFIRE,     xi.magic.spell.ENBLIZZARD,    xi.magic.spell.ENAERO,
    xi.magic.spell.ENSTONE,    xi.magic.spell.ENTHUNDER,     xi.magic.spell.ENWATER,
    xi.magic.spell.ENFIRE_II,  xi.magic.spell.ENBLIZZARD_II, xi.magic.spell.ENAERO_II,
    xi.magic.spell.ENSTONE_II, xi.magic.spell.ENTHUNDER_II,  xi.magic.spell.ENWATER_II,
    xi.magic.spell.ENLIGHT,
}

-- What spell_list loaded, by spell id, so a setting turned off puts it back
local vanilla = {}

local function remember(spellId, spell)
    if vanilla[spellId] == nil then
        vanilla[spellId] =
        {
            validTarget = spell:getValidTarget(),
            range       = spell:getRange(),
            mpCost      = spell:getMPCost(),
            rdmLevel    = spell:getJobLevel(xi.job.RDM),
            pldLevel    = spell:getJobLevel(xi.job.PLD),
            drkLevel    = spell:getJobLevel(xi.job.DRK),
            recastTime  = spell:getRecastTime(),
            blmLevel    = spell:getJobLevel(xi.job.BLM),
            castTime    = spell:getCastTime(),
            aoe         = spell:getAoE(),
            radius      = spell:getRadius(),
        }
    end

    return vanilla[spellId]
end

local function withSpell(spellId, fn)
    local spell = GetSpell(spellId)
    if spell ~= nil then
        fn(spell, remember(spellId, spell))
    end
end

-- Sets the spells to the settings. Called at server start; tests call it after a setSetting.
xi.custom.spells.apply = function()
    local partyAoe    = setting('ENSPELL_PARTY_AOE')
    local partyTarget = setting('ENSPELL_PARTY_TARGET') and not partyAoe
    for _, spellId in ipairs(enspells) do
        withSpell(spellId, function(spell, base)
            spell:setValidTarget(partyTarget and bit.bor(base.validTarget, xi.targetType.PLAYER_PARTY) or base.validTarget)
            spell:setRange(partyTarget and math.max(base.range, partyRange) or base.range)
        end)
    end

    -- Party area like Protectra: on yourself, everyone in the party within its radius
    local protectra = GetSpell(xi.magic.spell.PROTECTRA)
    local area      = protectra and remember(xi.magic.spell.PROTECTRA, protectra)
    for _, spellId in ipairs(allEnspells) do
        withSpell(spellId, function(spell, base)
            if partyAoe and area ~= nil then
                spell:setValidTarget(area.validTarget)
                spell:setAoE(area.aoe)
                spell:setRadius(area.radius)
            else
                if not partyTarget or spellId == xi.magic.spell.ENDARK then
                    spell:setValidTarget(base.validTarget)
                end

                spell:setAoE(base.aoe)
                spell:setRadius(base.radius)
            end
        end)
    end

    -- getJobLevel answers 255 for "never"; setJobLevel takes 0 for it
    local function level(value)
        return value == 255 and 0 or value
    end

    withSpell(xi.magic.spell.HASTE, function(spell, base)
        spell:setJobLevel(xi.job.RDM, setting('RDM_HASTE_LEVEL_8') and 8 or level(base.rdmLevel))
        spell:setMPCost(setting('HASTE_MP_10') and 10 or base.mpCost)
    end)

    local protect = GetSpell(xi.magic.spell.PROTECT)
    withSpell(xi.magic.spell.HASTE, function(spell, base)
        local recast = base.recastTime
        if setting('HASTE_PROTECT_RECAST') and protect ~= nil then
            recast = remember(xi.magic.spell.PROTECT, protect).recastTime
        end

        spell:setRecastTime(recast)
    end)

    -- En-spells at level 2 for each job that has them
    local enspellsLevel2 = setting('ENSPELLS_LEVEL_2')
    for _, spellId in ipairs(allEnspells) do
        withSpell(spellId, function(spell, base)
            for _, entry in ipairs({ { xi.job.RDM, base.rdmLevel }, { xi.job.PLD, base.pldLevel }, { xi.job.DRK, base.drkLevel } }) do
                if entry[2] ~= 255 then
                    spell:setJobLevel(entry[1], enspellsLevel2 and 2 or entry[2])
                end
            end
        end)
    end

    -- Remembered first, so their retail values are there to scale from
    withSpell(xi.magic.spell.STONE, function()
    end)
    withSpell(xi.magic.spell.THUNDER, function()
    end)

    local levelOne = setting('BLM_TIER1_NUKES_LEVEL_1')
    local scaling  = setting('TIER1_NUKE_LEVEL_SCALING')
    local thunder  = vanilla[xi.magic.spell.THUNDER]
    for spellId in pairs(xi.custom.spells.tier1Nukes) do
        withSpell(spellId, function(spell, base)
            spell:setJobLevel(xi.job.BLM, levelOne and 1 or level(base.blmLevel))
            -- Thunder's cost and cast time; onMagicCastingCheck puts the retail ones back on a
            -- mob's, trust's or pet's own cast.
            spell:setMPCost(scaling and thunder.mpCost or base.mpCost)
            spell:setCastTime(scaling and thunder.castTime or base.castTime)
        end)
    end
end

-----------------------------------
-- Tier I nukes: level 1 for BLM, and level scaling for players
-----------------------------------
-- The spells and the script names of their onMagicCastingCheck
xi.custom.spells.tier1Nukes =
{
    [xi.magic.spell.WATER]    = 'water',
    [xi.magic.spell.AERO]     = 'aero',
    [xi.magic.spell.FIRE]     = 'fire',
    [xi.magic.spell.BLIZZARD] = 'blizzard',
    [xi.magic.spell.THUNDER]  = 'thunder',
}

local pColumn = { v = 6, m0 = 8, m50 = 9 } -- xi.spells.damage.pTable: vPC, M0, M50
local retailRows = {} -- pTable rows as loaded, by spell id

local function retailRow(spellId)
    if retailRows[spellId] == nil then
        local row = xi.spells.damage.pTable[spellId]
        retailRows[spellId] = row and { unpack(row) } or nil
    end

    return retailRows[spellId]
end

local function round(x)
    return math.floor(x + 0.5)
end

-- The caster's level in the job that casts the spell: the main level when the main job can
-- cast it at that level, else the sub level (WAR/BLM: the BLM sub level).
local function castLevel(caster, spell)
    local mainLevel = caster:getMainLvl()
    if spell:getJobLevel(caster:getMainJob()) <= mainLevel then
        return mainLevel
    end

    return caster:getSubLvl()
end

-- Values of a tier I nuke for this caster. All five are the same spell in strength: power and
-- dINT multipliers Stone's at level 1, rising evenly to Thunder's at Thunder's retail BLM level
-- (21) and above, with the top power set by TIER1_NUKE_POWER_CAP (0: Thunder's 85). MP and cast
-- time always Thunder's. nil when the spell isn't one of them.
xi.custom.spells.tier1Scaled = function(caster, spellId)
    if xi.custom.spells.tier1Nukes[spellId] == nil then
        return nil
    end

    local spell = GetSpell(spellId)
    withSpell(xi.magic.spell.STONE, function()
    end)
    withSpell(xi.magic.spell.THUNDER, function()
    end)
    withSpell(spellId, function()
    end)

    local thunder = vanilla[xi.magic.spell.THUNDER]
    local row, stoneRow = retailRow(xi.magic.spell.THUNDER), retailRow(xi.magic.spell.STONE)
    if spell == nil or thunder == nil or row == nil or stoneRow == nil then
        return nil
    end

    local retailLevel = thunder.blmLevel
    local t = 1
    if retailLevel > 1 and retailLevel < 255 then
        t = utils.clamp((castLevel(caster, spell) - 1) / (retailLevel - 1), 0, 1)
    end

    local function lerp(from, to)
        return from + (to - from) * t
    end

    local topPower = row[pColumn.v]
    local cap      = xi.settings.custom and tonumber(xi.settings.custom.TIER1_NUKE_POWER_CAP) or 0
    if cap > 0 then
        topPower = math.max(cap, stoneRow[pColumn.v])
    end

    return
    {
        v        = round(lerp(stoneRow[pColumn.v], topPower)),
        m0       = lerp(stoneRow[pColumn.m0], row[pColumn.m0]),
        m50      = lerp(stoneRow[pColumn.m50], row[pColumn.m50]),
        mp       = thunder.mpCost,
        castTime = thunder.castTime,
    }
end

local function scales(caster, spellId)
    return setting('TIER1_NUKE_LEVEL_SCALING') and caster ~= nil and caster:isPC() and
        xi.custom.spells.tier1Nukes[spellId] ~= nil
end

-- Each cast has its own copy of the spell: a mob's, trust's or pet's gets the retail MP and
-- cast time back (the shared spell holds Thunder's, for players).
for spellId, name in pairs(xi.custom.spells.tier1Nukes) do
    m:addOverride(string.format('xi.actions.spells.black.%s.onMagicCastingCheck', name), function(caster, target, spell)
        local result = super(caster, target, spell)

        if setting('TIER1_NUKE_LEVEL_SCALING') and not scales(caster, spellId) and vanilla[spellId] ~= nil then
            spell:setMPCost(vanilla[spellId].mpCost)
            spell:setCastTime(vanilla[spellId].castTime)
        end

        return result
    end)
end

-- Power and dINT multipliers: the scaled ones for this one calculation
m:addOverride('xi.spells.damage.calculateBaseDamage', function(caster, target, spellId, spellGroup, skillType, statUsed)
    if not scales(caster, spellId) then
        return super(caster, target, spellId, spellGroup, skillType, statUsed)
    end

    local scaled = xi.custom.spells.tier1Scaled(caster, spellId)
    local row    = xi.spells.damage.pTable[spellId]
    if scaled == nil or row == nil then
        return super(caster, target, spellId, spellGroup, skillType, statUsed)
    end

    local copy = { unpack(row) }
    copy[pColumn.v]   = scaled.v
    copy[pColumn.m0]  = scaled.m0
    copy[pColumn.m50] = scaled.m50

    xi.spells.damage.pTable[spellId] = copy
    local ok, result = pcall(super, caster, target, spellId, spellGroup, skillType, statUsed)
    xi.spells.damage.pTable[spellId] = row

    if not ok then
        error(result)
    end

    return result
end)

m:addOverride('xi.server.onServerStart', function()
    super()
    xi.custom.spells.apply()
end)

-- Haste's power is in ten-thousandths: retail 1465 (150/1024), Haste II 2998 (307/1024)
local hastePower = { vanilla = 1465, custom = 2998 }

m:addOverride('xi.spells.enhancing.calculateEnhancingBasePower', function(caster, target, spell, spellId, spellEffect)
    local power = super(caster, target, spell, spellId, spellEffect)

    if spellId == xi.magic.spell.HASTE and setting('HASTE_30_PERCENT') then
        power = power - hastePower.vanilla + hastePower.custom
    end

    return power
end)

-- The retail level penalty is the last step of the duration: undo it where a setting says so.
-- Spell id -> { setting, effect level (enhancing_spell.lua pTable) }
local noLevelPenalty =
{
    [xi.magic.spell.HASTE]   = { 'HASTE_NO_LEVEL_PENALTY',   40 },
    [xi.magic.spell.ENSTONE] = { 'ENSTONE_NO_LEVEL_PENALTY', 18 },
}

-- ENSPELLS_NO_LEVEL_PENALTY: every RDM en-spell, at its level in enhancing_spell.lua
local enspellPenaltyLevels =
{
    [xi.magic.spell.ENFIRE]     = 24, [xi.magic.spell.ENBLIZZARD]    = 22, [xi.magic.spell.ENAERO]     = 20,
    [xi.magic.spell.ENSTONE]    = 18, [xi.magic.spell.ENTHUNDER]     = 16, [xi.magic.spell.ENWATER]    = 27,
    [xi.magic.spell.ENFIRE_II]  = 58, [xi.magic.spell.ENBLIZZARD_II] = 56, [xi.magic.spell.ENAERO_II]  = 54,
    [xi.magic.spell.ENSTONE_II] = 52, [xi.magic.spell.ENTHUNDER_II]  = 50, [xi.magic.spell.ENWATER_II] = 60,
}

m:addOverride('xi.spells.enhancing.calculateEnhancingDuration', function(caster, target, spell, spellId, spellGroup, spellEffect)
    local duration = super(caster, target, spell, spellId, spellGroup, spellEffect)

    local entry = noLevelPenalty[spellId]
    if (entry == nil or not setting(entry[1])) and enspellPenaltyLevels[spellId] ~= nil and setting('ENSPELLS_NO_LEVEL_PENALTY') then
        entry = { 'ENSPELLS_NO_LEVEL_PENALTY', enspellPenaltyLevels[spellId] }
    end

    if entry ~= nil and setting(entry[1]) then
        local spellLevel  = entry[2]
        local targetLevel = target:getMainLvl()
        if targetLevel > 0 and targetLevel < spellLevel then
            duration = duration * spellLevel / targetLevel
        end
    end

    return duration
end)
