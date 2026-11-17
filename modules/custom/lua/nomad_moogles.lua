-----------------------------------
-- Extra Nomad Moogles (server_ffxi)
-----------------------------------
-- Places Nomad Moogles that work like the retail ones (Mhaura, Selbina, Norg...): their
-- Mog menu gives the Mog Safe, Mog Locker, delivery box and job change, and with
-- custom.NOMAD_MOOGLE_STORAGE also Storage.
--
-- The menu only works in a zone with the mogmenu flag, so this turns the flag on for each
-- zone listed below. The client reads that flag when a player enters the zone: anyone
-- already there when the moogle appeared has to zone (or relog) before the menu works.
--
-- Toggle: xi.settings.custom.NOMAD_MOOGLES (settings/custom.lua).
-- To add a moogle, add a { x, y, z, rotation } entry under the zone's folder name.
-- !getpos (or chars.pos_* in the DB) gives a spot; rotation is 0-255, 0 = east.
-- Every Field Manual also gets one beside it (the list is modules/custom/lua/field_manuals.lua),
-- with a City Teleporter on its other side.
--
-- No restart needed: saving this file makes the running server re-run it (FileWatcher),
-- and the re-run spawns any listed moogle that is missing in a zone already loaded.
-----------------------------------
require('modules/module_utils')
local fieldManuals = require('modules/custom/lua/field_manuals')
-----------------------------------
local m = Module:new('nomad_moogles')

local spots =
{
    -- Near the Southern San d'Oria auction house, where Kenji stood on 2026-11-15.
    Southern_San_dOria = { { -17.921, 1.700, -29.507, 152 } },
    -- Where Kenji stood on 2026-11-15; each City Teleporter is 2 yalms beside them.
    Windurst_Woods     = { { 59.302, -2.500, -119.514, 130 } },
    Windurst_Walls     = { { 45.338, -2.500, -62.361, 63 } },
    La_Theine_Plateau  = { { -561.711, 0.992, 660.137, 65 } },
}

-- One beside every Field Manual that has no hand-placed set. The exact spot needs the
-- zone's navmesh, so it is worked out in place().
fieldManuals.each(function(zoneEntry, manual)
    if not manual.placed then
        spots[zoneEntry.folder] = spots[zoneEntry.folder] or {}
        table.insert(spots[zoneEntry.folder], { manual = manual })
    end
end)

local moogleLook = '0x0000520000000000000000000000000000000000' -- model 82, same as the Mog House moogle

local onTrigger = function(player, npc)
    player:printToPlayer(
        'I\'m a traveling moogle, kupo. I help adventurers access items they have stored in a Mog House elsewhere, kupo.',
        xi.msg.channel.NS_SAY,
        npc:getPacketName()
    )
    player:sendMenu(xi.menuType.MOOGLE)
end

local function npcName(i)
    return fmt('Nomad Moogle {}', i) -- internal lookup name, must be unique in the zone
end

-- Dynamic NPCs register their callbacks under xi.zones[zone].npcs['DE_<name>'].
local function cached(zoneName, i)
    local z = xi.zones and xi.zones[zoneName] -- xi.zones does not exist yet when modules load at boot
    return z and z.npcs and z.npcs['DE_' .. npcName(i)]
end

-- Spawns the zone's listed moogles that are not there yet.
local function place(zone, zoneName)
    zone:addMiscFlag(xi.zoneMisc.MOGMENU)

    local added = 0
    for i, spot in ipairs(spots[zoneName]) do
        local entry = cached(zoneName, i)
        if entry then
            entry.onTrigger = onTrigger -- pick up edits to onTrigger on a re-run
        else
            local x, y, z, rotation = spot[1], spot[2], spot[3], spot[4]
            if spot.manual then
                x, y, z, rotation = fieldManuals.beside(zone, spot.manual, 1)
            end

            zone:insertDynamicEntity({
                objtype    = xi.objType.NPC,
                name       = npcName(i),
                packetName = 'Nomad Moogle',
                look       = moogleLook,
                x          = x,
                y          = y,
                z          = z,
                rotation   = rotation,
                widescan   = 1,
                onTrigger  = onTrigger,
            })
            added = added + 1
        end
    end

    print(fmt('nomad_moogles: {} Nomad Moogle(s) in {} ({} new)', #spots[zoneName], zoneName, added))
end

local function enabled()
    return xi.settings.custom and xi.settings.custom.NOMAD_MOOGLES
end

-- True on a FileWatcher re-run: some listed moogle already exists. At boot none do yet.
local function serverRunning()
    for zoneName in pairs(spots) do
        if cached(zoneName, 1) then
            return true
        end
    end

    return false
end

for zoneName in pairs(spots) do
    require('scripts/zones/' .. zoneName .. '/Zone')

    m:addOverride(fmt('xi.zones.{}.Zone.onInitialize', zoneName), function(zone)
        super(zone)

        if enabled() then
            place(zone, zoneName)
        end
    end)
end

-- On a re-run the overrides above are discarded, so place the moogles in the zones
-- that are already up.
if enabled() and serverRunning() then
    for zoneName in pairs(spots) do
        local zone = GetZone(xi.zone[zoneName:upper()])
        if zone then
            place(zone, zoneName)
        end
    end
end

return m
