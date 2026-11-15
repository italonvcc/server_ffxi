-----------------------------------
-- func: bot
-- desc: Control bot players in-game (see modules/bots/cpp/bot_players.cpp).
--       Creating and deleting bots is done from the web dashboard (modules/bots/dashboard).
--       !bot list            -- bots that are spawned right now
--       !bot spawn <name>    -- spawn a bot at its saved position
--       !bot despawn <name>  -- remove a bot from the world
--       !bot here <name>     -- move a bot next to you
-----------------------------------
require('modules/module_utils')
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    -- NOTE: 0 lets any character use this. Fine for a private server, raise it otherwise.
    permission = 0,
    parameters = 'ss',
}

local function reply(player, msg)
    player:printToPlayer(msg, xi.msg.channel.SYSTEM_3, '')
end

local function usage(player)
    reply(player, '!bot list | !bot spawn <name> | !bot despawn <name> | !bot here <name>')
end

commandObj.onTrigger = function(player, action, name)
    if xi.bots == nil then
        reply(player, 'bot_players C++ module is not loaded.')
        return
    end

    if action == 'list' then
        reply(player, xi.bots.list())
        return
    end

    if name == nil or name == '' then
        usage(player)
        return
    end

    -- Character names are stored capitalized
    name = name:sub(1, 1):upper() .. name:sub(2):lower()

    if action == 'spawn' then
        reply(player, xi.bots.spawn(name))
    elseif action == 'despawn' then
        reply(player, xi.bots.despawn(name))
    elseif action == 'here' then
        reply(player, xi.bots.summon(name, player))
    else
        usage(player)
    end
end

xi.module.registerCommand('bot', commandObj)
