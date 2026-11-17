-----------------------------------
-- En-spell element on melee and ranged strikes
--
-- While a character (player or bot) has an en-spell on, the physical damage of their melee
-- swings (auto-attacks, kicks, counters) and ranged attacks takes the en-spell element's
-- multiplier against the target, the same one nukes get:
--   the target's SDT for the element x its resistance rank damage rate (custom
--   RESIST_RANK_DAMAGE_RATE table, or retail's 1/2 at rank 4+) x day and weather
-- So a Bomb (fire rank -3) takes more from an Enfire-ed sword, and less from an Enblizzard-ed
-- one (ice rank 4). Weapon skills, absorb and null are left out. The en-spell's own added
-- damage already gets these.
--
-- Toggle: xi.settings.custom.ENSPELL_ELEMENT_STRIKES (settings/default/custom.lua).
-- false = vanilla.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('enspell_element_strikes')

xi = xi or {}
xi.custom = xi.custom or {}

-- En-spell effect -> element. The II tiers share their tier I element.
local enspellElements =
{
    { xi.effect.ENFIRE,        xi.element.FIRE    },
    { xi.effect.ENFIRE_II,     xi.element.FIRE    },
    { xi.effect.ENBLIZZARD,    xi.element.ICE     },
    { xi.effect.ENBLIZZARD_II, xi.element.ICE     },
    { xi.effect.ENAERO,        xi.element.WIND    },
    { xi.effect.ENAERO_II,     xi.element.WIND    },
    { xi.effect.ENSTONE,       xi.element.EARTH   },
    { xi.effect.ENSTONE_II,    xi.element.EARTH   },
    { xi.effect.ENTHUNDER,     xi.element.THUNDER },
    { xi.effect.ENTHUNDER_II,  xi.element.THUNDER },
    { xi.effect.ENWATER,       xi.element.WATER   },
    { xi.effect.ENWATER_II,    xi.element.WATER   },
    { xi.effect.ENLIGHT,       xi.element.LIGHT   },
    { xi.effect.ENDARK,        xi.element.DARK    },
}

local function enabled()
    return xi.settings.custom ~= nil and xi.settings.custom.ENSPELL_ELEMENT_STRIKES == true
end

-- The element of the actor's en-spell, or nil
xi.custom.enspellElement = function(actor)
    for _, entry in ipairs(enspellElements) do
        if actor:hasStatusEffect(entry[1]) then
            return entry[2]
        end
    end

    return nil
end

-- The multiplier for a strike from this actor on this target: 1 without the toggle, for a
-- non-character, or without an en-spell.
xi.custom.enspellStrikeMultiplier = function(actor, target)
    if not enabled() or actor == nil or target == nil or not actor:isPC() then
        return 1
    end

    local element = xi.custom.enspellElement(actor)
    if element == nil then
        return 1
    end

    return xi.combat.damage.magicalElementSDT(target, element) *
        xi.spells.damage.calculateAdditionalResistTier(actor, target, element) *
        xi.spells.damage.calculateDayAndWeather(actor, element, false)
end

-- Auto-attacks, kicks and counters (battleutils::GetDamageRatio). Weapon skills pass true.
m:addOverride('xi.combat.physical.calculateMeleePDIF', function(actor, target, weaponType, wsAttackMod, isCritical, applyLevelCorrection, tpIgnoresDefense, tpFactor, isWeaponskill, weaponSlot, isCannonball)
    local pDIF = super(actor, target, weaponType, wsAttackMod, isCritical, applyLevelCorrection, tpIgnoresDefense, tpFactor, isWeaponskill, weaponSlot, isCannonball)
    if isWeaponskill then
        return pDIF
    end

    return pDIF * xi.custom.enspellStrikeMultiplier(actor, target)
end)

-- Ranged attacks (battleutils::GetRangedDamageRatio). Weapon skills pass true.
m:addOverride('xi.combat.physical.calculateRangedPDIF', function(actor, target, weaponType, wsAttackMod, isCritical, applyLevelCorrection, tpIgnoresDefense, tpFactor, isWeaponskill, bonusRangedAttack)
    local pDIF = super(actor, target, weaponType, wsAttackMod, isCritical, applyLevelCorrection, tpIgnoresDefense, tpFactor, isWeaponskill, bonusRangedAttack)
    if isWeaponskill then
        return pDIF
    end

    return pDIF * xi.custom.enspellStrikeMultiplier(actor, target)
end)

return m
