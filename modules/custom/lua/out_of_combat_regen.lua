-----------------------------------
-- Out-of-combat recovery (server_ffxi)
-----------------------------------
-- Less downtime between fights, for players and bots alike.
--
-- "Out of combat" = nobody in the character's party (just the character when solo) has a
-- weapon drawn or is on a monster's enmity list.
--
-- OUT_OF_COMBAT_REGEN: out of combat, a character recovers HP and MP as if resting while
--   standing or moving. Every map.HEALING_TICK_DELAY seconds it gets one resting tick, with
--   the formulas of scripts/effects/healing.lua (the Signet bonus, and each tick in a row
--   gives more). Entering combat starts the ramp over. No TP is lost.
--
-- OUT_OF_COMBAT_REST_MULTIPLIER: sitting down (/heal) while out of combat recovers this many
--   times the normal amount per tick. Sitting during a fight is unchanged.
--
-- Toggles: xi.settings.custom.* (settings/custom.lua). false / 1.0 = vanilla.
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('out_of_combat_regen')

xi.custom                  = xi.custom or {}
xi.custom.outOfCombatRegen = xi.custom.outOfCombatRegen or {}

local regen = xi.custom.outOfCombatRegen

local ticksVar = '[custom]oocRegenTicks' -- resting ticks in a row while standing
local nextTick = 0
local zones    = nil -- zones that exist on this server, found at the first tick

local function inCombat(entity)
    return entity:isEngaged() or entity:hasEnmity()
end

-- True when nobody in the character's party is fighting. `known` (optional) remembers the
-- answer for the other members during one pass over all players.
regen.partyOutOfCombat = function(player, known)
    if known and known[player:getID()] ~= nil then
        return known[player:getID()]
    end

    local party = player:getParty() or { player }
    local calm  = true

    for _, member in ipairs(party) do
        if inCombat(member) then
            calm = false
            break
        end
    end

    if known then
        for _, member in ipairs(party) do
            known[member:getID()] = calm
        end
    end

    return calm
end

local function cannotRecover(player)
    return player:hasStatusEffect(xi.effect.DISEASE) or
        player:hasStatusEffect(xi.effect.PLAGUE) or
        player:hasStatusEffect(xi.effect.CURSE_II)
end

-- One resting tick for a character that is not sitting. Returns the HP and MP given.
regen.tickPlayer = function(player, known)
    if not player:isAlive() then
        return 0, 0
    end

    if not regen.partyOutOfCombat(player, known) then
        player:setLocalVar(ticksVar, 0)
        return 0, 0
    end

    -- Sitting: the Healing effect does the recovering (see the override below)
    if player:hasStatusEffect(xi.effect.HEALING) or cannotRecover(player) then
        return 0, 0
    end

    local ramp = player:getLocalVar(ticksVar) -- 0 on the first tick, like the first heal of /heal
    player:setLocalVar(ticksVar, ramp + 1)

    if player:getHP() >= player:getMaxHP() and player:getMP() >= player:getMaxMP() then
        return 0, 0
    end

    -- As in scripts/effects/healing.lua
    local region = player:getCurrentRegion()
    local healHP = 0
    local healMP = 12 + ramp * (1 + player:getMod(xi.mod.CLEAR_MIND)) + player:getMod(xi.mod.MPHEAL)

    if
        (region <= xi.region.LIMBUS and player:hasStatusEffect(xi.effect.SIGNET)) or
        (region >= xi.region.RONFAURE_FRONT and region <= xi.region.VALDEAUNIA_FRONT and player:hasStatusEffect(xi.effect.SIGIL))
    then
        healHP = 10 + 3 * math.floor(player:getMainLvl() / 10) +
            ramp * (1 + math.floor(player:getMaxHP() / 300)) + player:getMod(xi.mod.HPHEAL)
    else
        healHP = 10 + ramp + player:getMod(xi.mod.HPHEAL)
    end

    local hpBefore = player:getHP()
    local mpBefore = player:getMP()

    player:addHPLeaveSleeping(healHP)
    player:addMP(healMP)

    return player:getHP() - hpBefore, player:getMP() - mpBefore
end

-- One pass over every player (and bot) on this server
regen.tick = function()
    if not zones then
        zones = {}
        for _, zoneId in pairs(xi.zone) do
            local zone = GetZone(zoneId)
            if zone then
                table.insert(zones, zone)
            end
        end
    end

    local known = {}
    for _, zone in ipairs(zones) do
        for _, player in ipairs(zone:getPlayers()) do
            regen.tickPlayer(player, known)
        end
    end
end

m:addOverride('xi.server.onTimeServerTick', function()
    super()

    if not xi.settings.custom.OUT_OF_COMBAT_REGEN then
        return
    end

    local now = GetSystemTime()
    if now >= nextTick then
        nextTick = now + xi.settings.map.HEALING_TICK_DELAY
        regen.tick()
    end
end)

-- Sitting out of combat: the normal tick, then (multiplier - 1) times what it gave
m:addOverride('xi.effects.healing.onEffectTick', function(target, effect)
    local multiplier = xi.settings.custom.OUT_OF_COMBAT_REST_MULTIPLIER or 1

    if multiplier <= 1 or target:getObjType() ~= xi.objType.PC then
        super(target, effect)
        return
    end

    local hpBefore = target:getHP()
    local mpBefore = target:getMP()

    super(target, effect)

    local gainedHP = target:getHP() - hpBefore
    local gainedMP = target:getMP() - mpBefore

    if (gainedHP > 0 or gainedMP > 0) and regen.partyOutOfCombat(target) then
        if gainedHP > 0 then
            target:addHPLeaveSleeping(math.floor(gainedHP * (multiplier - 1)))
        end

        if gainedMP > 0 then
            target:addMP(math.floor(gainedMP * (multiplier - 1)))
        end
    end
end)

return m
