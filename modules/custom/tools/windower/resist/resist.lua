--[[
Resist: shows the current target's elemental resistances, read live from the server.

Needs the server_ffxi !resist command (modules/custom/lua/resist_command.lua, toggle
xi.settings.custom.RESIST_COMMAND). The client never receives mob resistances, so on target
change the addon sends "!resist <mobId>", hides the [RES] reply lines from the chat log and
draws them in a text box.

Each element shows the fixed damage multiplier your nukes take on that mob: elemental damage
taken x the 1/2 (rank 4+) or 1/8 (rank 11) cut x magic damage taken x day/weather with your
gear (obis, Iridescence, Twilight Cape, Zodiac Ring). Not included: resists rolled from hit
rate, MAB vs MDB, Magic Burst. The rank label says how the rank moves the hit rate.

Install: copy this folder to <Windower>/addons/resist and run "//lua load resist".
Commands:
  //resist            refresh the current target
  //resist show|hide  show or hide the box
  //resist reset      put the box back at its default position
]]

_addon.name     = 'resist'
_addon.author   = 'server_ffxi'
_addon.version  = '1.0'
_addon.commands = { 'resist' }

local config = require('config')
local texts  = require('texts')

local defaults =
{
    pos   = { x = 200, y = 300 },
    bg    = { alpha = 180, red = 0, green = 0, blue = 0 },
    text  = { font = 'Consolas', size = 10 },
    flags = { draggable = true },
    padding = 4,
    cache_seconds = 30, -- re-ask the server after this long while the mob stays targeted
}

local settings = config.load(defaults)
local box      = texts.new('', settings)

local ELEMENTS =
{
    { key = 'fire',    label = 'Fire',    color = '255,110,80'  },
    { key = 'ice',     label = 'Ice',     color = '130,200,255' },
    { key = 'wind',    label = 'Wind',    color = '120,230,120' },
    { key = 'earth',   label = 'Earth',   color = '210,180,110' },
    { key = 'thunder', label = 'Thunder', color = '200,140,255' },
    { key = 'water',   label = 'Water',   color = '90,140,255'  },
    { key = 'light',   label = 'Light',   color = '255,255,200' },
    { key = 'dark',    label = 'Dark',    color = '170,170,170' },
}

local cache     = {}   -- [mobId] = { header = {...}, elements = {...}, time = t }
local currentId = nil  -- mob currently targeted
local pending   = nil  -- { id = mobId, at = clock } query waiting to be sent
local lastSent  = 0
local errorText = nil
local visible   = true

-- The server drops a second chat packet within 200 ms; stay well clear of it.
local SEND_GAP = 0.5
local DEBOUNCE = 0.25

local function c(color, s)
    return string.format('\\cs(%s)%s\\cr', color, s)
end

-- What a resistance rank does to the hit rate (scripts/combat/basic/magic_hit_rate.lua).
-- Its fixed damage cut (1/2 at rank 4+, 1/8 at rank 11) is already in the multiplier.
local function describeRank(rank)
    if rank >= 11 then
        return c('255,80,80', 'auto-resist')
    elseif rank >= 1 then
        return c('230,230,120', 'resists more')
    elseif rank == 0 then
        return nil
    else
        return c('120,255,120', 'resists less')
    end
end

-- x1.35 style text, green above 1, red below
local function multiplierText(pct)
    local s = string.format('x%.2f', pct / 100)
    if pct > 100 then
        return c('120,255,120', s)
    elseif pct < 100 then
        return c('255,120,120', s)
    end

    return s
end

local function render()
    if not visible or currentId == nil then
        box:hide()
        return
    end

    local data = cache[currentId]
    local lines = {}

    if errorText then
        table.insert(lines, c('255,120,120', errorText))
    elseif data == nil or data.elements == nil then
        table.insert(lines, 'Resist: asking the server...')
    else
        local h = data.header
        local name = (h.name or '?'):gsub('_', ' ')
        table.insert(lines, string.format('%s  Lv%s', name, h.lv or '?'))

        local magicTaken = 100 + (tonumber(h.mdt) or 0) / 100 + (tonumber(h.umdt) or 0) / 100
        table.insert(lines, string.format('MEVA %s  MDB %s  Magic dmg %d%%', h.meva or '?', h.mdb or '?', magicTaken))

        if (tonumber(h.mabs) or 0) > 0 then
            table.insert(lines, c('255,80,80', string.format('Absorbs all magic %s%%', h.mabs)))
        end

        if data.dwAlways == false then
            table.insert(lines, c('200,200,200', 'Day/weather: 33% chance per cast'))
        end

        for _, e in ipairs(ELEMENTS) do
            local v = data.elements[e.key]
            if v then
                local parts = {}

                local rankText = describeRank(v.rank)
                if rankText then
                    table.insert(parts, rankText)
                end

                local dw = data.dw and data.dw[e.key]
                if dw and dw ~= 100 then
                    table.insert(parts, string.format('day/wthr x%.2f', dw / 100))
                end

                if v.sdt ~= 0 then
                    table.insert(parts, string.format('elem dmg %d%%', 100 + v.sdt / 100))
                end

                if v.meva ~= 0 then
                    table.insert(parts, string.format('meva %+d', v.meva))
                end

                if v.absorb > 0 then
                    table.insert(parts, c('255,80,80', string.format('ABSORBS %d%%', v.absorb)))
                end

                if v.null > 0 then
                    table.insert(parts, c('255,80,80', string.format('NULL %d%%', v.null)))
                end

                local mult = data.mult and data.mult[e.key]
                table.insert(lines, string.format('%s %s  rank %d%s%s',
                    c(e.color, string.format('%-7s', e.label)),
                    mult and multiplierText(mult) or '  ?  ',
                    v.rank,
                    #parts > 0 and '  ' or '',
                    table.concat(parts, ', ')))
            end
        end
    end

    box:text(table.concat(lines, '\n'))
    box:show()
end

local function queue(id, force)
    errorText = nil

    local entry = cache[id]
    if not force and entry and entry.elements and os.clock() - entry.time < settings.cache_seconds then
        render()
        return
    end

    pending = { id = id, at = os.clock() + DEBOUNCE }
    render()
end

local function onTarget()
    local mob = windower.ffxi.get_mob_by_target('t')

    -- spawn_type 16 = monster. Pets and others are left to //resist.
    if mob == nil or mob.spawn_type ~= 16 then
        currentId = nil
        pending   = nil
        render()
        return
    end

    if mob.id ~= currentId then
        currentId = mob.id
        queue(mob.id, false)
    end
end

-- [RES] <id> H key=value ... | [RES] <id> E elem=r,s,m,a,n ... | [RES] ERR <msg>
local function parse(text)
    local err = text:match('%[RES%] ERR (.+)')
    if err then
        errorText = 'Resist: ' .. err
        render()
        return
    end

    local id, kind, rest = text:match('%[RES%] (%d+) ([HEDM]) (.+)')
    id = tonumber(id)
    if not id then
        return
    end

    local entry = cache[id] or { header = {}, elements = {} }
    cache[id]   = entry
    entry.time  = os.clock()

    if kind == 'D' or kind == 'M' then
        local values = {}
        for key, pct in rest:gmatch('(%a+)=(%-?%d+)') do
            values[key] = tonumber(pct)
        end

        if kind == 'D' then
            entry.dw = values
        else
            entry.dwAlways = values.dw == 1
            values.dw      = nil
            entry.mult     = values
        end
    elseif kind == 'H' then
        entry.elements = {}
        entry.dw       = nil
        entry.mult     = nil
        -- The client may append control characters to system lines
        local name = (rest:match('name=([^%c]+)') or '?')
        for k, v in rest:gmatch('(%w+)=(%-?%d+)') do
            entry.header[k] = v
        end
        entry.header.name = name
    else
        for key, r, s, m, a, n in rest:gmatch('(%a+)=(%-?%d+),(%-?%d+),(%-?%d+),(%-?%d+),(%-?%d+)') do
            entry.elements[key] =
            {
                rank   = tonumber(r),
                sdt    = tonumber(s),
                meva   = tonumber(m),
                absorb = tonumber(a),
                null   = tonumber(n),
            }
        end
    end

    if id == currentId then
        render()
    end
end

-- Catch the replies as raw chat packets (0x017) and drop them, so the client never prints
-- them. Layout: 4-byte header, Kind, Attr, Data (2), sName (15), then the message from
-- byte offset 23 (Lua index 24), zero-terminated.
windower.register_event('incoming chunk', function(id, data)
    if id ~= 0x017 then
        return
    end

    local message = data:sub(24):match('^[^%z]*')
    if message and message:sub(1, 5) == '[RES]' then
        parse(message)
        return true -- block the packet
    end
end)

-- Fallback in case a reply still reaches the chat log
windower.register_event('incoming text', function(original)
    if original:find('[RES]', 1, true) then
        parse(original)
        return true -- hide it from the chat log
    end
end)

windower.register_event('target change', onTarget)

-- Send "!resist <id>" as a raw say packet (0x0B5) instead of typing it with "input", so the
-- client never echoes the command into the chat log. Layout: id (9 bits) + size in 4-byte
-- words (7 bits), sync (2 bytes, filled in by Windower), Kind (0 = say), unused byte, then
-- the zero-terminated text, padded to a multiple of 4.
local function sendCommand(text)
    local body = string.char(0, 0) .. text .. '\0'
    body = body .. string.rep('\0', (4 - (4 + #body) % 4) % 4)

    local words  = (4 + #body) / 4
    local header = string.char(0xB5, words * 2, 0, 0)

    windower.packets.inject_outgoing(0x0B5, header .. body)
end

-- The multipliers depend on the day and the weather: drop the cache when either changes.
local lastInfoCheck = 0
local lastDay       = nil
local lastWeather   = nil

local function checkDayWeather()
    if os.clock() - lastInfoCheck < 1 then
        return
    end

    lastInfoCheck = os.clock()

    local info = windower.ffxi.get_info()
    if not info then
        return
    end

    if lastDay ~= nil and (info.day ~= lastDay or info.weather ~= lastWeather) then
        cache = {}
        if currentId then
            queue(currentId, true)
        end
    end

    lastDay, lastWeather = info.day, info.weather
end

windower.register_event('prerender', function()
    checkDayWeather()

    if pending and os.clock() >= pending.at and os.clock() - lastSent >= SEND_GAP then
        sendCommand(string.format('!resist %d', pending.id))
        lastSent = os.clock()
        pending  = nil
    end
end)

windower.register_event('zone change', function()
    cache     = {}
    currentId = nil
    pending   = nil
    render()
end)

windower.register_event('addon command', function(cmd)
    cmd = cmd and cmd:lower() or ''

    if cmd == 'hide' then
        visible = false
    elseif cmd == 'show' then
        visible = true
    elseif cmd == 'reset' then
        box:pos(defaults.pos.x, defaults.pos.y)
        settings.pos.x, settings.pos.y = defaults.pos.x, defaults.pos.y
        config.save(settings)
    else
        local mob = windower.ffxi.get_mob_by_target('t')
        if mob then
            currentId = mob.id
            queue(mob.id, true)
        else
            windower.add_to_chat(207, 'resist: no target.')
        end
    end

    render()
end)

windower.register_event('load', onTarget)

windower.register_event('unload', function()
    settings.pos.x, settings.pos.y = box:pos()
    config.save(settings)
end)
