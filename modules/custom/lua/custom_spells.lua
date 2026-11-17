-----------------------------------
-- Custom spell changes
--
-- Applied to the loaded spells at server start (spell_list is left as retail). Each one has
-- a toggle in settings/default/custom.lua. Some also need the game client patched, or the
-- client keeps showing or enforcing the retail values: modules/custom/tools/patch_client.py.
--
--   ENSPELL_PARTY_TARGET   En-spells can be cast on a party member within 20 yalms, like
--                          Phalanx II. Client patch needed (target cursor).
--   RDM_HASTE_LEVEL_2      Red Mage learns Haste at level 2 (retail 48). Client patch needed.
--   HASTE_MP_10            Haste costs 10 MP (retail 40). Client patch needed (MP shown and
--                          checked by the client).
--   HASTE_NO_LEVEL_PENALTY Haste lasts its full time on targets below level 40 (retail: cut
--                          to level/40 of it). Server only.
--   HASTE_30_PERCENT       Haste gives 30% haste like Haste II (retail 14.65%). Server only.
--   ENSTONE_NO_LEVEL_PENALTY Enstone lasts its full time on targets below level 18 (retail:
--                          cut to level/18 of it). Server only.
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
    local partyTarget = setting('ENSPELL_PARTY_TARGET')
    for _, spellId in ipairs(enspells) do
        withSpell(spellId, function(spell, base)
            spell:setValidTarget(partyTarget and bit.bor(base.validTarget, xi.targetType.PLAYER_PARTY) or base.validTarget)
            spell:setRange(partyTarget and math.max(base.range, partyRange) or base.range)
        end)
    end

    -- getJobLevel answers 255 for "never"; setJobLevel takes 0 for it
    local function level(value)
        return value == 255 and 0 or value
    end

    withSpell(xi.magic.spell.HASTE, function(spell, base)
        spell:setJobLevel(xi.job.RDM, setting('RDM_HASTE_LEVEL_2') and 2 or level(base.rdmLevel))
        spell:setMPCost(setting('HASTE_MP_10') and 10 or base.mpCost)
    end)
end

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

m:addOverride('xi.spells.enhancing.calculateEnhancingDuration', function(caster, target, spell, spellId, spellGroup, spellEffect)
    local duration = super(caster, target, spell, spellId, spellGroup, spellEffect)

    local entry = noLevelPenalty[spellId]
    if entry ~= nil and setting(entry[1]) then
        local spellLevel  = entry[2]
        local targetLevel = target:getMainLvl()
        if targetLevel > 0 and targetLevel < spellLevel then
            duration = duration * spellLevel / targetLevel
        end
    end

    return duration
end)
