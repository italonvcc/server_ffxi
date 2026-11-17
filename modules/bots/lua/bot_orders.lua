-----------------------------------
-- func: ja, ws, ma
-- desc: Tell a bot in your party to use a job ability, a weapon skill or a spell
--       (see "Orders" in modules/bots/cpp/bot_players.cpp).
--       !ja <bot> "<job ability>"     e.g. !ja Lamaza "Benediction"
--       !ws <bot> "<weapon skill>"    e.g. !ws Sigyni "Fast Blade"
--       !ma <bot> "<spell or song>"   e.g. !ma Lamaza "Cure II", !ma Lyra "Valor Minuet"
--       !ma <bot> stop                the bard stops keeping its songs up
--
--       The target is whatever you have targeted (<t>). An ability that can't be used on
--       it is used on the bot itself when that is possible; a weapon skill with no enemy
--       targeted goes on what the bot is fighting. A spell that can't be cast on your
--       target goes on you, else on the bot, else on what the party is fighting. A song for
--       the party is sung around the bard, and sung again whenever it wears off: a bard
--       keeps up the last two songs it was asked for (one without an instrument). Name the
--       tier: "Valor Minuet" is the first one. The quotes are optional, and anything
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

local function register(command, what)
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

        local name, spell = split(text)
        if not name or spell == '' then
            xi.botOrders.tell(player, fmt('!{} <bot> "<{}>"', command, what))
            return
        end

        xi.botOrders.tell(player, xi.bots.order(player, name, spell, command))
    end

    xi.module.registerCommand(command, commandObj)
end

register('ja', 'job ability')
register('ws', 'weapon skill')
register('ma', 'spell or song')
