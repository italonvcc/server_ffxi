-----------------------------------
-- City Teleporter (server_ffxi)
-----------------------------------
-- An NPC that teleports the player for free to a major city (its first home point in
-- scripts/globals/homepoint.lua, so the arrival spot is safe) or to a Field Manual, each
-- of which has a teleporter and a Nomad Moogle beside it.
--
-- The menu: 'Cities', then one entry per area (Zulkheim, Elshimo...) > zone > manual. A
-- zone with one manual is picked directly. A teleporter does not list the city it stands
-- in, nor the manual it stands by.
--
-- Toggle: xi.settings.custom.CITY_TELEPORTER (settings/custom.lua).
-- To place another one, add a { x, y, z, rotation } entry under the zone's folder name,
-- and a line in `cities` if it should be a destination. The Field Manual ones come from
-- modules/custom/lua/field_manuals.lua (areas, zones and labels are edited there).
-- Each menu is split into pages of 3 entries plus 'More...' to stay within the GM prompt.
--
-- No restart needed: saving this file makes the running server re-run it (FileWatcher).
-- The re-run spawns any listed teleporter that is missing in a zone already loaded and
-- points the existing ones at the new menu (so new destinations show up at once).
-----------------------------------
require('modules/module_utils')
local fieldManuals = require('modules/custom/lua/field_manuals')
-----------------------------------
local m = Module:new('city_teleporter')

local spots =
{
    -- Each one 2 yalms beside a Nomad Moogle (modules/custom/lua/nomad_moogles.lua),
    -- where Kenji stood on 2026-11-15.
    Southern_San_dOria = { { -19.858, 1.700, -31.189, 161 } },
    Windurst_Woods     = { { 59.302, -2.500, -117.514, 130 } },
    Windurst_Walls     = { { 47.338, -2.500, -62.361, 63 } },
    La_Theine_Plateau  = { { -559.711, 0.992, 660.137, 65 } },
}

-- One beside every Field Manual that has no hand-placed set (the La Theine Plateau pair
-- above stands by a manual). The exact spot needs the zone's navmesh: see place().
fieldManuals.each(function(zoneEntry, manual)
    if not manual.placed then
        spots[zoneEntry.folder] = spots[zoneEntry.folder] or {}
        table.insert(spots[zoneEntry.folder], { manual = manual })
    end
end)

-- name, x, y, z, rotation, zone
local cities =
{
    { 'Southern San d\'Oria',  -85.554,  1, -64.554,  45, xi.zone.SOUTHERN_SAN_DORIA   },
    { 'Bastok Markets',       -342.000, -10, -154.000, 0, xi.zone.BASTOK_MARKETS       },
    { 'Windurst Woods',          9.000, -2,    0.000,   0, xi.zone.WINDURST_WOODS       },
    { 'Lower Jeuno',            18.000, -1,   54.000,   0, xi.zone.LOWER_JEUNO          },
    { 'Aht Urhgan Whitegate',  -21.000,  0,  -21.000,   0, xi.zone.AHT_URHGAN_WHITEGATE },
    { 'Western Adoulin',       -84.000,  4,  -32.000, 128, xi.zone.WESTERN_ADOULIN      },
}

-- The menu tree. A node is { name, children = { nodes } } or a destination:
-- { name, dest = { x, y, z, rotation, zone }, city = true/nil }.
local menu = { name = 'Where would you like to go?', children = {} }

do
    local cityNode = { name = 'Cities', children = {} }
    for _, c in ipairs(cities) do
        table.insert(cityNode.children, { name = c[1], dest = { c[2], c[3], c[4], c[5], c[6] }, city = true })
    end

    table.insert(menu.children, cityNode)

    for _, area in ipairs(fieldManuals.areas) do
        local areaNode = { name = area.name, children = {} }

        for _, zoneEntry in ipairs(area.zones) do
            local zoneId   = xi.zone[zoneEntry.folder:upper()]
            local zoneNode = { name = zoneEntry.name, children = {} }

            for _, manual in ipairs(zoneEntry.manuals) do
                -- Arrive on the manual itself (ground known to be good), or where its
                -- hand-placed set says.
                local a = manual.arrive or { manual.x, manual.y, manual.z, manual.rot }
                table.insert(zoneNode.children, {
                    name = manual.label ~= '' and manual.label or zoneEntry.name,
                    dest = { a[1], a[2], a[3], a[4], zoneId },
                })
            end

            -- A zone with one manual needs no menu of its own.
            table.insert(areaNode.children, #zoneNode.children == 1 and zoneNode.children[1] or zoneNode)
        end

        table.insert(menu.children, areaNode)
    end
end

local perPage    = 3  -- plus one 'More...' / 'Never mind.' option
local hereRadius = 30 -- a manual this close to the player is the one they are standing by

-- The node's entries the player can use from where they stand: not the city they are in,
-- not the manual they are at, and no menu left empty by that.
local function visibleChildren(player, node)
    local zoneId = player:getZoneID()
    local list   = {}

    for _, child in ipairs(node.children) do
        local show = true

        if child.dest then
            local d = child.dest
            if d[5] == zoneId then
                local dx, dz = d[1] - player:getXPos(), d[3] - player:getZPos()
                show = not child.city and dx * dx + dz * dz > hereRadius * hereRadius
            end
        else
            show = #visibleChildren(player, child) > 0
        end

        if show then
            table.insert(list, child)
        end
    end

    return list
end

local showPage -- forward declaration

-- the previous menu context has to clear before the next one opens
local function openLater(player, node, page)
    player:timer(50, function(p)
        showPage(p, node, page)
    end)
end

local function buildOptions(player, node, page)
    local list    = visibleChildren(player, node)
    local options = {}
    local first   = (page - 1) * perPage + 1

    for i = first, math.min(first + perPage - 1, #list) do
        local child = list[i]
        table.insert(options, {
            child.name,
            function(playerArg)
                if child.dest then
                    local d = child.dest
                    playerArg:setPos(d[1], d[2], d[3], d[4], d[5])
                else
                    openLater(playerArg, child, 1)
                end
            end,
        })
    end

    if first + perPage <= #list then
        table.insert(options, {
            'More...',
            function(playerArg)
                openLater(playerArg, node, page + 1)
            end,
        })
    else
        table.insert(options, { 'Never mind.', function(playerArg) end })
    end

    return options
end

showPage = function(player, node, page)
    player:setFreezeFlag(true)
    player:customMenu({
        title       = node.name,
        options     = buildOptions(player, node, page),
        onCancelled = function(playerArg)
            playerArg:setFreezeFlag(false)
        end,
        onEnd       = function(playerArg)
            playerArg:setFreezeFlag(false)
        end,
    })
end

local onTrigger = function(player, npc)
    showPage(player, menu, 1)
end

local function npcName(i)
    return fmt('City Teleporter {}', i) -- internal lookup name, must be unique in the zone
end

-- Dynamic NPCs register their callbacks under xi.zones[zone].npcs['DE_<name>'].
local function cached(zoneName, i)
    local z = xi.zones and xi.zones[zoneName] -- xi.zones does not exist yet when modules load at boot
    return z and z.npcs and z.npcs['DE_' .. npcName(i)]
end

-- Spawns the zone's listed teleporters that are not there yet.
local function place(zone, zoneName)
    local added = 0
    for i, spot in ipairs(spots[zoneName]) do
        local entry = cached(zoneName, i)
        if entry then
            entry.onTrigger = onTrigger -- existing teleporters get the new menu
        else
            local x, y, z, rotation = spot[1], spot[2], spot[3], spot[4]
            if spot.manual then
                x, y, z, rotation = fieldManuals.beside(zone, spot.manual, -1)
            end

            zone:insertDynamicEntity({
                objtype    = xi.objType.NPC,
                name       = npcName(i),
                packetName = 'City Teleporter',
                look       = 51, -- Home Point crystal
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

    print(fmt('city_teleporter: {} in {} ({} new)', #spots[zoneName], zoneName, added))
end

local function enabled()
    return xi.settings.custom and xi.settings.custom.CITY_TELEPORTER
end

-- True on a FileWatcher re-run: some listed teleporter already exists. At boot none do yet.
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

-- On a re-run the overrides above are discarded, so update the zones that are already up.
if enabled() and serverRunning() then
    for zoneName in pairs(spots) do
        local zone = GetZone(xi.zone[zoneName:upper()])
        if zone then
            place(zone, zoneName)
        end
    end
end

return m
