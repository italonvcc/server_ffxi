-----------------------------------
-- Permanent Signet and crafting image support (server_ffxi)
-----------------------------------
-- Players (at login and on every zone-in) and bots (at spawn, from modules/bots) always have:
--
-- PERMANENT_SIGNET: Signet with no timer. Its benefits still follow the retail rules, e.g.
--   crystals and conquest points only drop in the original conquest regions. Getting
--   Sanction or Sigil removes it as in retail, and it comes back next to them a second later.
--
-- PERMANENT_IMAGERY: advanced image support for every craft at once (+3 to each synthesis
--   craft and 10% less material loss, +2 fishing). Retail allows one image support at a
--   time (the imagery effects share an exclusion group), so this is a single generic
--   Imagery effect (id 244, unused anywhere else) that carries every craft's bonuses.
--
-- A permanent effect (duration 0) has no timer and survives zoning, logout, Erase and
-- Dispel. Whatever still removes one (death for Imagery, Sanction/Sigil or a gate guard's
-- timed Signet, a guild image support NPC or craft belt) is undone a second later: losing
-- the permanent effect schedules a re-apply.
--
-- Turning a toggle off removes the permanent effect at the next login or zone-in.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('permanent_buffs')

local imageryEffect = xi.effect.IMAGERY_1
local craftPower    = 3  -- Advanced image support, synthesis crafts
local fishingPower  = 2  -- Advanced image support, fishing
local lossReduction = 10 -- Advanced image support material loss reduction (%)
local reapplyDelay  = 1000

local craftMods =
{
    { xi.mod.WOOD,      xi.mod.SYNTH_MATERIAL_LOSS_WOODWORKING  },
    { xi.mod.SMITH,     xi.mod.SYNTH_MATERIAL_LOSS_SMITHING     },
    { xi.mod.GOLDSMITH, xi.mod.SYNTH_MATERIAL_LOSS_GOLDSMITHING },
    { xi.mod.CLOTH,     xi.mod.SYNTH_MATERIAL_LOSS_CLOTHCRAFT   },
    { xi.mod.LEATHER,   xi.mod.SYNTH_MATERIAL_LOSS_LEATHERCRAFT },
    { xi.mod.BONE,      xi.mod.SYNTH_MATERIAL_LOSS_BONECRAFT    },
    { xi.mod.ALCHEMY,   xi.mod.SYNTH_MATERIAL_LOSS_ALCHEMY      },
    { xi.mod.COOK,      xi.mod.SYNTH_MATERIAL_LOSS_COOKING      },
}

xi.custom                = xi.custom or {}
xi.custom.permanentBuffs = xi.custom.permanentBuffs or {}

local function isPermanent(effect)
    return effect ~= nil and effect:getDuration() == 0
end

-- Gives (or takes away, when the toggle is off) the permanent effects. Safe to call any
-- number of times: it only changes something that is missing or wrong.
xi.custom.permanentBuffs.apply = function(entity)
    if entity == nil or entity:getObjType() ~= xi.objType.PC then
        return
    end

    -- Signet: replace a missing or timed one; drop only our permanent one when off
    local signet = entity:getStatusEffect(xi.effect.SIGNET)

    if xi.settings.custom.PERMANENT_SIGNET then
        if not isPermanent(signet) then
            if signet then
                entity:delStatusEffectSilent(xi.effect.SIGNET)
            end

            entity:addStatusEffect(xi.effect.SIGNET, { duration = 0, origin = entity, silent = true })
        end
    elseif isPermanent(signet) then
        entity:delStatusEffectSilent(xi.effect.SIGNET)
    end

    -- Imagery: id 244 is only ever given by this module
    local imagery = entity:getStatusEffect(imageryEffect)

    if xi.settings.custom.PERMANENT_IMAGERY then
        if
            imagery == nil or
            imagery:getPower() ~= craftPower or
            imagery:getSubPower() ~= lossReduction
        then
            if imagery then
                entity:delStatusEffectSilent(imageryEffect)
            end

            -- Also replaces any single-craft image support (same exclusion group)
            entity:addStatusEffect(imageryEffect, { power = craftPower, subPower = lossReduction, duration = 0, origin = entity, silent = true })
        end
    elseif imagery then
        entity:delStatusEffectSilent(imageryEffect)
    end
end

-- Puts the effects back shortly after something replaced them. Deferred because a removal
-- happens in the middle of adding the replacing effect.
local function reapplyLater(entity)
    if
        entity:getObjType() == xi.objType.PC and
        (xi.settings.custom.PERMANENT_SIGNET or xi.settings.custom.PERMANENT_IMAGERY)
    then
        entity:timer(reapplyDelay, function(entityArg)
            xi.custom.permanentBuffs.apply(entityArg)
        end)
    end
end

m:addOverride('xi.player.onGameIn', function(player, firstLogin, zoning)
    super(player, firstLogin, zoning)

    xi.custom.permanentBuffs.apply(player)
end)

m:addOverride('xi.effects.signet.onEffectLose', function(target, effect)
    super(target, effect)

    if isPermanent(effect) then
        reapplyLater(target)
    end
end)

-- The generic Imagery effect has no bonuses of its own in the base game
m:addOverride('xi.effects.imagery.onEffectGain', function(target, effect)
    super(target, effect)

    if effect:getEffectType() ~= imageryEffect then
        return
    end

    for _, mods in ipairs(craftMods) do
        effect:addMod(mods[1], effect:getPower())
        effect:addMod(mods[2], effect:getSubPower())
    end

    effect:addMod(xi.mod.FISH, math.min(effect:getPower(), fishingPower))
end)

m:addOverride('xi.effects.imagery.onEffectLose', function(target, effect)
    super(target, effect)

    if effect:getEffectType() == imageryEffect and isPermanent(effect) then
        reapplyLater(target)
    end
end)

return m
