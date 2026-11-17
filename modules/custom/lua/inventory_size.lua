-----------------------------------
-- Inventory size (server_ffxi)
-----------------------------------
-- Raises a character's main inventory to xi.settings.custom.INVENTORY_SIZE slots when
-- they log in. It only ever raises it: a bigger inventory is left alone, and a value of
-- 30 or less (vanilla) does nothing. Capped at 80, the most the client can show.
-- The new size is saved to char_storage, so it stays if the toggle is turned off later.
--
-- Toggle: xi.settings.custom.INVENTORY_SIZE (settings/custom.lua).
-----------------------------------
require('modules/module_utils')
-----------------------------------
local m = Module:new('inventory_size')

local clientMax = 80

xi.custom = xi.custom or {}

-- Grows the player's inventory to the configured size. Returns the slots added.
xi.custom.applyInventorySize = function(player)
    local target  = math.min(math.floor(xi.settings.custom.INVENTORY_SIZE or 0), clientMax)
    local current = player:getContainerSize(xi.inv.INVENTORY)

    if target <= current then
        return 0
    end

    player:changeContainerSize(xi.inv.INVENTORY, target - current)

    return target - current
end

m:addOverride('xi.player.onGameIn', function(player, firstLogin, zoning)
    super(player, firstLogin, zoning)

    if not zoning then
        xi.custom.applyInventorySize(player)
    end
end)

return m
