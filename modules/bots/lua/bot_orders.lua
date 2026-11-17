-----------------------------------
-- func: ja, ws
-- desc: Tell a bot in your party to use a job ability or a weapon skill
--       (see "Orders" in modules/bots/cpp/bot_players.cpp).
--       !ja <bot> "<job ability>"     e.g. !ja Lamaza "Benediction"
--       !ws <bot> "<weapon skill>"    e.g. !ws Sigyni "Fast Blade"
--
--       The target is whatever you have targeted (<t>). An ability that can't be used on
--       it is used on the bot itself when that is possible; a weapon skill with no enemy
--       targeted goes on what the bot is fighting. The quotes are optional, and anything
--       after the name is ignored, so `!ws "Sigyni" "Fast Blade" <t>` works too (the client
--       replaces <t> with the target's name).
-----------------------------------
require('modules/module_utils')
-----------------------------------

-- Called from C++ when an order is dropped later on (out of time, target gone...)
xi.botOrders = xi.botOrders or {}

xi.botOrders.tell = function(player, msg)
    player:printToPlayer(msg, xi.msg.channel.SYSTEM_3, '')
end

-- '"Sigyni" "Fast Blade" Wild Rabbit' -> 'Sigyni', 'Fast Blade'
-- 'Sigyni fast blade'                 -> 'Sigyni', 'fast blade'
local function split(text)
    text = text or ''

    local name, rest = text:match('^%s*"([^"]+)"%s*(.*)$')
    if not name then
        name, rest = text:match('^%s*(%S+)%s*(.*)$')
    end

    if not name then
        return nil, nil
    end

    local quoted = rest:match('^"([^"]*)"')

    return name, quoted or rest
end

local function register(command, weaponSkill)
    ---@type TCommand
    local commandObj = {}

    commandObj.cmdprops =
    {
        -- NOTE: 0 lets any character use this (only on bots in their own party).
        permission = 0,
        parameters = 's', -- the whole line
    }

    commandObj.onTrigger = function(player, text)
        if xi.bots == nil or xi.bots.order == nil then
            xi.botOrders.tell(player, 'bot_players C++ module is not loaded.')
            return
        end

        local name, what = split(text)
        if not name or what == '' then
            xi.botOrders.tell(player, fmt('!{} <bot> "<{}>"', command, weaponSkill and 'weapon skill' or 'job ability'))
            return
        end

        xi.botOrders.tell(player, xi.bots.order(player, name, what, weaponSkill))
    end

    xi.module.registerCommand(command, commandObj)
end

register('ja', false)
register('ws', true)
