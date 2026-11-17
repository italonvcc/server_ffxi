-----------------------------------
-- func: resist [mobId]
-- desc: Prints a mob's live elemental resistances, for the Windower 'resist' addon
--       (modules/custom/tools/windower/resist). Without an ID it uses the player's
--       current target. Only works on a mob or pet in the player's own zone.
--
-- Output is six machine-readable lines (each under the 150-byte chat limit):
--   [RES] <id> H lv=<level> meva=<MEVA> mdb=<MDEF> mdt=<DMGMAGIC> umdt=<UDMGMAGIC> mabs=<MAGIC_ABSORB> name=<name>
--   [RES] <id> E fire=<r,s,m,a,n> ice=... wind=... earth=...
--   [RES] <id> E thunder=... water=... light=... dark=...
--   [RES] <id> D fire=<pct> ...           day/weather multiplier for the player, x100
--   [RES] <id> P main=<pct> [off=<pct>]   the player's melee hit chance on the target
--   [RES] <id> M dw=<0|1> fire=<pct> ...  total fixed damage multiplier, x100 (sent last)
-- where per element r = resistance rank, s = SDT mod (damage x (1 + s/10000)),
-- m = element MEVA mod, a = absorb chance %, n = nullify chance %.
-- M = SDT x rank damage rate x magic damage taken x day/weather (as if it applies; dw=1 means
-- custom.DAY_WEATHER_ALWAYS is on so it always does). The rank damage rate is retail's 1/2 at
-- rank 4+ (1/8 at rank 11), or the custom.RESIST_RANK_DAMAGE_RATE table. It leaves out the
-- resist tier rolled from hit rate, MAB vs MDB, Magic Burst and absorb/null.
-- Errors are printed as: [RES] ERR <message>
--
-- Toggle: xi.settings.custom.RESIST_COMMAND (settings/default/custom.lua). false = vanilla,
--         the command answers that it is turned off.
-----------------------------------
require('modules/module_utils')
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    -- 0 lets any character use this
    permission = 0,
    parameters = 's',
}

local elements =
{
    { key = 'fire',    element = xi.element.FIRE    },
    { key = 'ice',     element = xi.element.ICE     },
    { key = 'wind',    element = xi.element.WIND    },
    { key = 'earth',   element = xi.element.EARTH   },
    { key = 'thunder', element = xi.element.THUNDER },
    { key = 'water',   element = xi.element.WATER   },
    { key = 'light',   element = xi.element.LIGHT   },
    { key = 'dark',    element = xi.element.DARK    },
}

local function elementField(target, entry)
    local e = entry.element

    return string.format('%s=%d,%d,%d,%d,%d',
        entry.key,
        target:getMod(xi.data.element.getElementalResistanceRankModifier(e)),
        target:getMod(xi.data.element.getElementalSDTModifier(e)),
        target:getMod(xi.data.element.getElementalMEVAModifier(e)),
        target:getMod(xi.data.element.getElementalAbsorptionModifier(e)),
        target:getMod(xi.data.element.getElementalNullificationModifier(e)))
end

local function elementLine(target, first, last)
    local fields = {}
    for i = first, last do
        table.insert(fields, elementField(target, elements[i]))
    end

    return string.format('[RES] %d E %s', target:getID(), table.concat(fields, ' '))
end

local function percent(x)
    return math.floor(x * 100 + 0.5)
end

-- Damage multipliers for a nuke of each element from this player on this target, as the
-- damage code in scripts/globals/spells/damage_spell.lua applies them. Only the fixed
-- parts: no resist tier rolled from hit rate, no MAB/MDB, no Magic Burst, no absorb.
local function multiplierLines(player, target)
    local dayWeather = {}
    local final      = {}
    local adjustment = xi.combat.damage.calculateDamageAdjustment(target, false, true, false, false)

    for _, entry in ipairs(elements) do
        local e    = entry.element
        local rank = target:getMod(xi.data.element.getElementalResistanceRankModifier(e))

        -- Retail: x0.5 at rank 4+, and rank 11 auto-resists nukes to 1/4 on top of it.
        -- custom.RESIST_RANK_DAMAGE_RATE: the rank's damage rate, no auto-resist.
        local rankFactor = xi.spells.damage.calculateAdditionalResistTier(player, target, e)
        if rank >= 11 and not (xi.settings.custom and xi.settings.custom.RESIST_RANK_DAMAGE_RATE) then
            rankFactor = rankFactor * 0.25
        end

        -- true = the value when day/weather apply (always, with custom.DAY_WEATHER_ALWAYS)
        local dw = xi.spells.damage.calculateDayAndWeather(player, e, true)

        table.insert(dayWeather, string.format('%s=%d', entry.key, percent(dw)))
        table.insert(final, string.format('%s=%d', entry.key,
            percent(xi.combat.damage.magicalElementSDT(target, e) * rankFactor * adjustment * dw)))
    end

    local always = (xi.settings.custom and xi.settings.custom.DAY_WEATHER_ALWAYS) and 1 or 0

    return string.format('[RES] %d M dw=%d %s', target:getID(), always, table.concat(final, ' ')),
        string.format('[RES] %d D %s', target:getID(), table.concat(dayWeather, ' '))
end

-- Melee hit chance of the player's auto-attacks on the target, in %, the way
-- battleutils::GetHitRateEx works it out (it calls this same Lua function): accuracy from
-- gear, food and every buff, Flash, Innin, Closed Position, level correction and the caps.
-- Sneak Attack from behind never misses.
local function hitRateLine(player, target)
    local function rate(slot)
        if player:hasStatusEffect(xi.effect.SNEAK_ATTACK) and player:isBehind(target, 64) then
            return 100
        end

        return math.floor(xi.combat.physicalHitRate.getPhysicalHitRate(player, target, 0, slot, false) * 100)
    end

    local line = string.format('[RES] %d P main=%d', target:getID(), rate(xi.attackAnimation.RIGHT_ATTACK))
    if player:isDualWielding() then
        line = line .. string.format(' off=%d', rate(xi.attackAnimation.LEFT_ATTACK))
    end

    return line
end

local function fail(player, msg)
    player:printToPlayer(string.format('[RES] ERR %s', msg))
end

commandObj.onTrigger = function(player, mobId)
    if not (xi.settings.custom and xi.settings.custom.RESIST_COMMAND) then
        fail(player, '!resist is turned off on this server.')
        return
    end

    local target = player:getCursorTarget()
    if mobId ~= nil and mobId ~= '' then
        local id = tonumber(mobId)
        if id == nil then
            fail(player, 'usage: !resist [mobId]')
            return
        end

        if target == nil or target:getID() ~= id then
            -- Mobs and pets, in the zone or in the player's instance. true = no warning in the log.
            target = GetEntityByID(id, player:getInstance(), true)
        end
    end

    if target == nil then
        fail(player, 'no target.')
        return
    end

    local objType = target:getObjType()
    if objType ~= xi.objType.MOB and objType ~= xi.objType.PET then
        fail(player, 'target is not a mob.')
        return
    end

    if target:getZoneID() ~= player:getZoneID() then
        fail(player, 'target is not in your zone.')
        return
    end

    player:printToPlayer(string.format('[RES] %d H lv=%d meva=%d mdb=%d mdt=%d umdt=%d mabs=%d name=%s',
        target:getID(),
        target:getMainLvl(),
        target:getMod(xi.mod.MEVA),
        target:getMod(xi.mod.MDEF),
        target:getMod(xi.mod.DMGMAGIC),
        target:getMod(xi.mod.UDMGMAGIC),
        target:getMod(xi.mod.MAGIC_ABSORB),
        target:getName()))
    player:printToPlayer(elementLine(target, 1, 4))
    player:printToPlayer(elementLine(target, 5, 8))

    local multipliers, dayWeather = multiplierLines(player, target)
    player:printToPlayer(dayWeather)
    player:printToPlayer(hitRateLine(player, target))
    player:printToPlayer(multipliers) -- last line: the addon redraws once it has it
end

xi.module.registerCommand('resist', commandObj)
