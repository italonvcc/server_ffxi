-----------------------------------
-- func: home
-- desc: Sends the player back to their home point, like the Warp spell (3 seconds, and
--       cancelled if they are knocked out meanwhile). Not while fighting.
--
-- Toggle: xi.settings.custom.HOME_COMMAND (settings/default/custom.lua). false = vanilla,
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
    parameters = '',
}

commandObj.onTrigger = function(player)
    if not (xi.settings.custom and xi.settings.custom.HOME_COMMAND) then
        player:printToPlayer('!home is turned off on this server.')
        return
    end

    if player:isDead() then
        player:printToPlayer('!home: not while knocked out.')
        return
    end

    if player:isEngaged() or player:hasEnmity() then
        player:printToPlayer('!home: not while fighting.')
        return
    end

    if player:hasStatusEffect(xi.effect.TELEPORT) then
        return
    end

    -- The same effect the Warp spell, a Warp Ring and an Instant Warp scroll use
    player:addStatusEffect(xi.effect.TELEPORT, { power = xi.teleport.id.WARP, duration = 3, origin = player, icon = 0 })
    player:printToPlayer('Returning to your home point...')
end

xi.module.registerCommand('home', commandObj)
