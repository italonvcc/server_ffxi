-----------------------------------
-- Fork gameplay mods (settings/default/custom.lua). The test runner starts with every
-- custom.* setting at vanilla, so each test turns on the mod it checks.
-----------------------------------

describe('Custom exp settings', function()
    local originalCalculate
    local calls

    local mob
    local low
    local high

    local function party(lowLevel, highLevel)
        mob = nil
        low  = xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON, level = lowLevel })
        high = xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON, level = highLevel })

        mob = low.entities:get('Volcanic_Bomb')
        mob:respawn()
        mob:setMobLevel(12)

        low:setPos(mob:getXPos(), mob:getYPos(), mob:getZPos())
        high:setPos(mob:getXPos(), mob:getYPos(), mob:getZPos())

        low.actions:inviteToParty(high)
        high.actions:acceptPartyInvite()
    end

    -- What a member earns with no level scaling and the given party share (no chain, no bonuses)
    local function expected(call, share)
        local cap = call.memberLevel <= 50 and 400 or (call.memberLevel <= 60 and 500 or 600)
        return math.floor(math.min(call.baseExp * share, cap))
    end

    before_each(function()
        calls             = {}
        originalCalculate = xi.experiencePoints.calculate

        xi.experiencePoints.calculate = function(member, target, data)
            local result = originalCalculate(member, target, data)
            table.insert(calls,
            {
                name               = member:getName(),
                memberLevel        = data.memberLevel,
                highestMemberLevel = data.highestMemberLevel,
                baseExp            = data.baseExp,
                exp                = result.exp,
            })
            return result
        end
    end)

    after_each(function()
        xi.experiencePoints.calculate = originalCalculate
    end)

    it('vanilla: a mob too weak for the highest member gives nobody exp', function()
        xi.test.world:setSetting('custom.EXP_NO_PARTY_PENALTY', false)
        xi.test.world:setSetting('custom.EXP_USE_OWN_LEVEL', false)
        party(10, 30)

        low:claimAndKillMob(mob)

        assert(#calls == 0, string.format('expected no exp calculations, got %d', #calls))
    end)

    it('vanilla: two members share 0.60 each', function()
        xi.test.world:setSetting('custom.EXP_NO_PARTY_PENALTY', false)
        xi.test.world:setSetting('custom.EXP_USE_OWN_LEVEL', false)
        party(10, 10)

        low:claimAndKillMob(mob)

        assert(#calls == 2, string.format('expected 2 exp calculations, got %d', #calls))
        for _, call in ipairs(calls) do
            assert(call.exp == expected(call, 0.60), string.format('%s: exp=%d, expected %d', call.name, call.exp, expected(call, 0.60)))
        end
    end)

    it('own level: the low member is graded against their own level', function()
        xi.test.world:setSetting('custom.EXP_NO_PARTY_PENALTY', false)
        xi.test.world:setSetting('custom.EXP_USE_OWN_LEVEL', true)
        party(10, 30)

        low:claimAndKillMob(mob)

        -- Too weak for the level 30 member, so only the level 10 member is calculated
        assert(#calls == 1, string.format('expected 1 exp calculation, got %d', #calls))
        local call = calls[1]
        assert(call.memberLevel == 10 and call.highestMemberLevel == 10,
            string.format('expected grading at 10, got member=%d highest=%d', call.memberLevel, call.highestMemberLevel))
        assert(call.exp > 0 and call.exp == expected(call, 0.60), string.format('exp=%d, expected %d', call.exp, expected(call, 0.60)))
    end)

    it('own level + no party penalty: the low member earns the full solo amount', function()
        xi.test.world:setSetting('custom.EXP_NO_PARTY_PENALTY', true)
        xi.test.world:setSetting('custom.EXP_USE_OWN_LEVEL', true)
        party(10, 30)

        low:claimAndKillMob(mob)

        assert(#calls == 1, string.format('expected 1 exp calculation, got %d', #calls))
        local call = calls[1]
        assert(call.exp > 0 and call.exp == expected(call, 1.00), string.format('exp=%d, expected %d', call.exp, expected(call, 1.00)))
    end)

    it('no party penalty: every share is 1.00', function()
        xi.test.world:setSetting('custom.EXP_NO_PARTY_PENALTY', true)
        for size = 1, 18 do
            assert(xi.experiencePoints.getExperienceShare(size, false) == 1.0, string.format('size %d share is not 1.00', size))
        end
    end)
end)

describe('Custom skill-up multipliers', function()
    local function gainsWith(player, multiplier, tries, oneTry)
        local gains = {}
        for _ = 1, tries do
            local gain = oneTry(player)
            gains[gain] = (gains[gain] or 0) + 1
        end

        return gains
    end

    local function assertOnly(gains, allowed, label)
        for gain, count in pairs(gains) do
            assert(allowed[gain], string.format('%s: unexpected gain of %d tenths (%d times)', label, gain, count))
        end
    end

    it('combat: x2 doubles each skill-up, past the vanilla 0.9 cap', function()
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA, job = xi.job.WAR, level = 50 })
        xi.test.world:setSeed(1)

        local function oneSkillUp(p)
            p:setSkillLevel(xi.skill.SWORD, 100)
            p:trySkillUp(xi.skill.SWORD, 50, true)
            local gain = p:getCharSkillLevel(xi.skill.SWORD) - 100
            -- The shown skill level follows the real one, even when one skill-up crosses a level
            assert(p:getSkillLevel(xi.skill.SWORD) == math.floor((100 + gain) / 10),
                string.format('gain %d: shown skill %d', gain, p:getSkillLevel(xi.skill.SWORD)))
            return gain
        end

        xi.test.world:setSetting('custom.COMBAT_SKILLUP_MULTIPLIER', 1.0)
        local vanilla = gainsWith(player, 1.0, 200, oneSkillUp)
        assertOnly(vanilla, { [1] = true, [2] = true, [3] = true, [4] = true, [5] = true }, 'vanilla')

        xi.test.world:setSetting('custom.COMBAT_SKILLUP_MULTIPLIER', 2.0)
        local doubled = gainsWith(player, 2.0, 200, oneSkillUp)
        assertOnly(doubled, { [2] = true, [4] = true, [6] = true, [8] = true, [10] = true }, 'x2')
        assert((doubled[10] or 0) > 0, 'expected some +1.0 skill-ups')
    end)

    it('combat: COMBAT_SKILLUP_GUARANTEED makes every try a skill-up', function()
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA, job = xi.job.WAR, level = 50 })
        xi.test.world:setSeed(1)

        -- Skill-ups out of `tries` unforced tries
        local function successes(tries)
            local count = 0
            for _ = 1, tries do
                player:setSkillLevel(xi.skill.SWORD, 100)
                player:trySkillUp(xi.skill.SWORD, 50)
                if player:getCharSkillLevel(xi.skill.SWORD) > 100 then
                    count = count + 1
                end
            end

            return count
        end

        xi.test.world:setSetting('custom.COMBAT_SKILLUP_GUARANTEED', false)
        local vanilla = successes(100)
        assert(vanilla > 25 and vanilla < 75, string.format('vanilla: expected about 50 of 100, got %d', vanilla))

        xi.test.world:setSetting('custom.COMBAT_SKILLUP_GUARANTEED', true)
        local guaranteed = successes(100)
        assert(guaranteed == 100, string.format('guaranteed: expected 100 of 100, got %d', guaranteed))
    end)

    describe('melee swings that miss', function()
        local player
        local mob

        -- Swing at a mob that dodges everything; returns the sword skill gained (tenths)
        local function swingAndMiss()
            player = xi.test.world:spawnPlayer({ zone = xi.zone.WEST_RONFAURE, job = xi.job.WAR, level = 50 })
            xi.test.world:setSeed(1)
            player:addItem(xi.item.BRONZE_SWORD)
            player:equipItem(xi.item.BRONZE_SWORD, nil, xi.slot.MAIN)
            player:setSkillLevel(xi.skill.SWORD, 100)

            mob = player.entities:moveTo('Wild_Rabbit')
            mob:respawn()
            mob:setMobLevel(50)
            mob:setUnkillable(true)
            mob:addStatusEffect(xi.effect.PERFECT_DODGE, { power = 0, duration = 3600, origin = mob, icon = 0 })

            player.actions:engage(mob)
            for _ = 1, 30 do
                xi.test.world:tickEntity(player)
                xi.test.world:skipTime(5)
            end

            return player:getCharSkillLevel(xi.skill.SWORD) - 100
        end

        it('vanilla: give no weapon skill-ups', function()
            xi.test.world:setSetting('custom.SKILLUP_ON_MISS', false)
            local gain = swingAndMiss()
            assert(gain == 0, string.format('expected no gain, got %d tenths', gain))
        end)

        it('SKILLUP_ON_MISS: roll weapon skill-ups', function()
            xi.test.world:setSetting('custom.SKILLUP_ON_MISS', true)
            local gain = swingAndMiss()
            assert(gain > 0, 'expected some sword skill from missed swings')
        end)
    end)

    it('synthesis: x5 multiplies each skill-up, capped at 0.9', function()
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA })
        xi.test.world:setSeed(1)
        xi.test.world:setSetting('custom.CRAFT_SKILLUP_MULTIPLIER', 5.0)

        player:setSkillRank(xi.skill.WOODWORKING, 9)
        player:setMod(xi.mod.SYNTH_SUCCESS_RATE, 300)
        player:setMod(xi.mod.SYNTH_SPEED_WOODWORKING, 17000)

        -- Willow Lumber, skill 11.0: vanilla gives +0.1 (85%) or +0.2 (15%), so x5 gives +0.5 or +0.9 (capped from 1.0)
        local gains = gainsWith(player, 5.0, 150, function(p)
            p:setSkillLevel(xi.skill.WOODWORKING, 110)
            p:addItem(xi.item.WIND_CRYSTAL)
            p:addItem(xi.item.WILLOW_LOG)
            p.actions:craft(xi.item.WIND_CRYSTAL, { xi.item.WILLOW_LOG })
            xi.test.world:skipTime(15)
            p:delContainerItems(xi.inv.INVENTORY)
            return p:getCharSkillLevel(xi.skill.WOODWORKING) - 110
        end)

        assertOnly(gains, { [0] = true, [5] = true, [9] = true }, 'x5')
        assert((gains[5] or 0) > 0, 'expected some +0.5 skill-ups')
    end)
end)

describe('Custom drop rate multiplier', function()
    -- Devil Manta (Kuftal Tunnel): Manta Skin is a 15% drop
    local function mantaSkinRate(player, mob, kills)
        local got = 0
        for _ = 1, kills do
            player:delContainerItems(xi.inv.INVENTORY)
            mob:spawn()
            player:claimAndKillMob(mob, { waitForDespawn = true })
            for _, item in ipairs(player:getItems()) do
                if item:getID() == xi.item.MANTA_SKIN then
                    got = got + 1
                end
            end
        end

        return got * 100.0 / kills
    end

    it('x5 multiplies the drop chance', function()
        -- WAR: no Treasure Hunter
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.KUFTAL_TUNNEL, job = xi.job.WAR, level = 75 })
        local mob    = player.entities:get(17489925)
        xi.test.world:setSeed(1)

        xi.test.world:setSetting('custom.DROP_RATE_MULTIPLIER', 1.0)
        local vanilla = mantaSkinRate(player, mob, 300)

        xi.test.world:setSetting('custom.DROP_RATE_MULTIPLIER', 5.0)
        local boosted = mantaSkinRate(player, mob, 300)

        print(string.format('Manta Skin: vanilla %.1f%%, x5 %.1f%%', vanilla, boosted))
        assert(vanilla >= 7 and vanilla <= 23, string.format('vanilla rate %.1f%% is not near 15%%', vanilla))
        assert(boosted >= 60, string.format('x5 rate %.1f%% is not near 75%%', boosted))
    end)
end)

describe('Custom regional vendor settings', function()
    local originalGeneral
    local opened

    before_each(function()
        opened          = false
        originalGeneral = xi.shop.general

        xi.shop.general = function(...)
            opened = true
            return originalGeneral(...)
        end

        -- Antonian sells Aragoneu goods for San d'Oria; give the region to Bastok.
        xi.test.world:setRegionOwner(xi.region.ARAGONEU, xi.nation.BASTOK)
    end)

    after_each(function()
        xi.shop.general = originalGeneral
    end)

    it('vanilla: a regional vendor is closed when their nation does not own the region', function()
        xi.test.world:setSetting('custom.REGIONAL_VENDORS_ALWAYS_OPEN', false)
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.NORTHERN_SAN_DORIA })
        player.entities:gotoAndTrigger('Antonian')

        assert(not opened, 'Expected the shop to stay closed')
    end)

    it('REGIONAL_VENDORS_ALWAYS_OPEN: the vendor sells whoever owns the region', function()
        xi.test.world:setSetting('custom.REGIONAL_VENDORS_ALWAYS_OPEN', true)
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.NORTHERN_SAN_DORIA })
        player.entities:gotoAndTrigger('Antonian')

        assert(opened, 'Expected the shop to open')
    end)
end)

describe('Custom inventory size', function()
    it('vanilla: the inventory stays at its size', function()
        xi.test.world:setSetting('custom.INVENTORY_SIZE', 1.0)
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.NORTHERN_SAN_DORIA })
        local before = player:getContainerSize(xi.inv.INVENTORY)

        assert(xi.custom.applyInventorySize(player) == 0, 'Expected no slots added')
        assert(player:getContainerSize(xi.inv.INVENTORY) == before, 'Expected the size to stay the same')
    end)

    it('INVENTORY_SIZE: raises the inventory to the setting, capped at 80', function()
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.NORTHERN_SAN_DORIA })

        xi.test.world:setSetting('custom.INVENTORY_SIZE', 60)
        xi.custom.applyInventorySize(player)
        assert(player:getContainerSize(xi.inv.INVENTORY) == 60, 'Expected 60 slots')

        xi.test.world:setSetting('custom.INVENTORY_SIZE', 100)
        xi.custom.applyInventorySize(player)
        assert(player:getContainerSize(xi.inv.INVENTORY) == 80, 'Expected the 80-slot cap')

        -- Never shrinks
        xi.test.world:setSetting('custom.INVENTORY_SIZE', 40)
        assert(xi.custom.applyInventorySize(player) == 0, 'Expected no change')
        assert(player:getContainerSize(xi.inv.INVENTORY) == 80, 'Expected 80 slots kept')
    end)
end)

describe('Custom permanent buffs', function()
    local function spawn()
        return xi.test.world:spawnPlayer({ zone = xi.zone.NORTHERN_SAN_DORIA })
    end

    -- Lets the deferred re-apply (entity:timer) run
    local function settle(player)
        xi.test.world:skipTime(2)
        xi.test.world:tickEntity(player)
    end

    it('vanilla: no Signet or Imagery is given', function()
        local player = spawn()

        xi.custom.permanentBuffs.apply(player)
        assert(not player:hasStatusEffect(xi.effect.SIGNET), 'Expected no Signet')
        assert(not player:hasStatusEffect(xi.effect.IMAGERY_1), 'Expected no Imagery')
    end)

    it('PERMANENT_SIGNET: a permanent Signet that comes back after a timed one or Sanction', function()
        xi.test.world:setSetting('custom.PERMANENT_SIGNET', true)
        local player = spawn()

        xi.custom.permanentBuffs.apply(player)
        local signet = player:getStatusEffect(xi.effect.SIGNET)
        assert(signet and signet:getDuration() == 0, 'Expected a permanent Signet')

        -- A gate guard's timed Signet replaces it, then the permanent one comes back
        xi.conquest.bestowSignet(player, player:getNation(), 1, 0)
        settle(player)
        signet = player:getStatusEffect(xi.effect.SIGNET)
        assert(signet and signet:getDuration() == 0, 'Expected the permanent Signet back')

        -- Getting Sanction removes every INFLUENCE effect (besieged.lua); Signet comes back next to it
        player:delStatusEffectsByFlag(xi.effectFlag.INFLUENCE, true)
        player:addStatusEffect(xi.effect.SANCTION, { duration = 3600, origin = player })
        settle(player)
        assert(player:hasStatusEffect(xi.effect.SIGNET), 'Expected Signet next to Sanction')
        assert(player:hasStatusEffect(xi.effect.SANCTION), 'Expected Sanction')

        -- Turned off: the permanent Signet is removed
        xi.test.world:setSetting('custom.PERMANENT_SIGNET', false)
        xi.custom.permanentBuffs.apply(player)
        settle(player)
        assert(not player:hasStatusEffect(xi.effect.SIGNET), 'Expected the permanent Signet removed')
    end)

    it('PERMANENT_IMAGERY: every craft gets advanced image support at once', function()
        xi.test.world:setSetting('custom.PERMANENT_IMAGERY', true)
        local player = spawn()

        local mods =
        {
            { xi.mod.WOOD,      xi.mod.SYNTH_MATERIAL_LOSS_WOODWORKING  },
            { xi.mod.SMITH,     xi.mod.SYNTH_MATERIAL_LOSS_SMITHING     },
            { xi.mod.GOLDSMITH, xi.mod.SYNTH_MATERIAL_LOSS_GOLDSMITHING },
            { xi.mod.CLOTH,     xi.mod.SYNTH_MATERIAL_LOSS_CLOTHCRAFT   },
            { xi.mod.LEATHER,   xi.mod.SYNTH_MATERIAL_LOSS_LEATHERCRAFT },
            { xi.mod.BONE,      xi.mod.SYNTH_MATERIAL_LOSS_BONECRAFT    },
            { xi.mod.ALCHEMY,   xi.mod.SYNTH_MATERIAL_LOSS_ALCHEMY      },
            { xi.mod.COOK,      xi.mod.SYNTH_MATERIAL_LOSS_COOKING      },
        }

        local function assertAllCrafts()
            for _, pair in ipairs(mods) do
                assert(player:getMod(pair[1]) == 3, string.format('Expected +3 on mod %d, got %d', pair[1], player:getMod(pair[1])))
                assert(player:getMod(pair[2]) == 10, string.format('Expected 10 on mod %d, got %d', pair[2], player:getMod(pair[2])))
            end

            assert(player:getMod(xi.mod.FISH) == 2, 'Expected +2 fishing')
        end

        xi.custom.permanentBuffs.apply(player)
        xi.custom.permanentBuffs.apply(player) -- no double-stacking
        assertAllCrafts()

        -- A guild NPC or craft belt replaces it with one craft; it comes back
        player:addStatusEffect(xi.effect.SMITHING_IMAGERY, { power = 3, duration = 480, origin = player, subPower = 10 })
        assert(not player:hasStatusEffect(xi.effect.IMAGERY_1), 'Expected the exclusion group to replace it')
        settle(player)
        assert(not player:hasStatusEffect(xi.effect.SMITHING_IMAGERY), 'Expected the single-craft support gone')
        assertAllCrafts()

        -- Imagery has the death flag: removed on death, then put back
        player:delStatusEffectsByFlag(xi.effectFlag.DEATH, true)
        settle(player)
        assertAllCrafts()

        -- Turned off: removed, mods gone
        xi.test.world:setSetting('custom.PERMANENT_IMAGERY', false)
        xi.custom.permanentBuffs.apply(player)
        settle(player)
        assert(not player:hasStatusEffect(xi.effect.IMAGERY_1), 'Expected Imagery removed')
        assert(player:getMod(xi.mod.SMITH) == 0, 'Expected no smithing bonus')
    end)
end)

describe('Custom out-of-combat recovery', function()
    local regen = xi.custom.outOfCombatRegen

    local function hurtPlayer()
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.NORTHERN_SAN_DORIA })
        player:setLevel(50) -- enough HP that nothing below fills it
        player:setHP(1)
        return player
    end

    -- Sits the player down and lets the Healing effect tick `ticks` times
    local function rest(player, ticks)
        player:addStatusEffect(xi.effect.HEALING, { origin = player, tick = xi.settings.map.HEALING_TICK_DELAY, icon = 0, silent = true })
        for _ = 1, ticks do
            xi.test.world:skipTime(xi.settings.map.HEALING_TICK_DELAY)
            xi.test.world:tickEntity(player)
        end
    end

    it('vanilla: standing out of combat recovers nothing', function()
        local player = hurtPlayer()

        xi.server.onTimeServerTick()
        assert(player:getHP() == 1, 'Expected no recovery')
    end)

    it('OUT_OF_COMBAT_REGEN: a resting tick while standing, growing tick after tick', function()
        local player = hurtPlayer()

        -- No Signet: 10 HP on the first tick, one more on each after it
        local first = regen.tickPlayer(player)
        assert(first == 10, string.format('Expected 10 HP on the first tick, got %d', first))

        local second = regen.tickPlayer(player)
        assert(second == 11, string.format('Expected 11 HP on the second tick, got %d', second))
    end)

    it('OUT_OF_COMBAT_REGEN: nothing while the character is fighting', function()
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.WEST_RONFAURE })
        player:setHP(1)
        local mob    = player.entities:moveTo('Wild_Rabbit')

        regen.tickPlayer(player)
        player.actions:engage(mob)
        xi.test.world:skipTime(2)
        xi.test.world:tickEntity(player)

        local hp = player:getHP()
        assert(regen.tickPlayer(player) == 0, 'Expected no recovery while engaged')
        assert(player:getHP() == hp, 'Expected the HP to stay')
    end)

    it('OUT_OF_COMBAT_REGEN: the server tick recovers players', function()
        xi.test.world:setSetting('custom.OUT_OF_COMBAT_REGEN', true)
        local player = hurtPlayer()

        xi.test.world:skipTime(xi.settings.map.HEALING_TICK_DELAY)
        xi.server.onTimeServerTick()
        assert(player:getHP() > 1, 'Expected recovery from the server tick')
    end)

    it('OUT_OF_COMBAT_REST_MULTIPLIER: sitting recovers that many times more', function()
        local vanilla = hurtPlayer()
        rest(vanilla, 3)
        local normal = vanilla:getHP() - 1
        assert(normal > 0, 'Expected resting to recover HP')

        xi.test.world:setSetting('custom.OUT_OF_COMBAT_REST_MULTIPLIER', 5.0)
        local player = hurtPlayer()
        rest(player, 3)
        local gained = player:getHP() - 1
        assert(gained == normal * 5, string.format('Expected %d HP, got %d', normal * 5, gained))
    end)
end)

describe('Custom guild shops always open', function()
    ---@type CClientEntityPair
    local player

    local statOpen  = 0
    local statClose = 1

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.MHAURA })
    end)

    after_each(function()
        xi.settings.main.GUILD_SHOP_HOLIDAYS = false
    end)

    local function guildStatAt(hour)
        xi.test.world:setVanaTime(hour, 0)
        player.packets:clear()
        player.entities:gotoAndTrigger('Kamilah')

        local stat
        for _, pkt in pairs(player.packets:getIncoming()) do
            if pkt.type == 0x086 then
                stat = pkt.data[4]
            end
        end

        return stat
    end

    it('vanilla: Kamilah is closed outside her hours', function()
        xi.test.world:setSetting('custom.GUILD_SHOPS_ALWAYS_OPEN', false)
        assert(guildStatAt(7) == statClose, 'Expected closed at 7:00')
    end)

    it('GUILD_SHOPS_ALWAYS_OPEN: open at any hour and on the guild holiday', function()
        xi.test.world:setSetting('custom.GUILD_SHOPS_ALWAYS_OPEN', true)
        assert(guildStatAt(0) == statOpen, 'Expected open at 0:00')
        assert(guildStatAt(7) == statOpen, 'Expected open at 7:00')
        assert(guildStatAt(23) == statOpen, 'Expected open at 23:00')

        -- Kamilah's holiday is Watersday.
        xi.settings.main.GUILD_SHOP_HOLIDAYS = true
        xi.test.world:setVanaDay(xi.day.WATERSDAY)
        assert(guildStatAt(12) == statOpen, 'Expected open on the holiday')
    end)
end)

describe('Custom spells', function()
    local caster
    local ally

    local toggles = { 'ENSPELL_PARTY_TARGET', 'RDM_HASTE_LEVEL_2', 'HASTE_MP_10', 'HASTE_NO_LEVEL_PENALTY', 'ENSTONE_NO_LEVEL_PENALTY', 'HASTE_30_PERCENT' }

    local function set(name, enabled)
        xi.test.world:setSetting('custom.' .. name, enabled)
        xi.custom.spells.apply()
    end

    local function party(allyLevel)
        caster = xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON, job = xi.job.RDM, level = 75 })
        ally   = xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON, level = allyLevel or 75 })
        ally:setPos(caster:getXPos() + 2, caster:getYPos(), caster:getZPos())

        caster.actions:inviteToParty(ally)
        ally.actions:acceptPartyInvite()
        caster:setMP(500)
    end

    local function cast(target, spellId)
        caster:addSpell(spellId)
        caster.actions:useSpell(target, spellId)
        xi.test.world:skipTime(10)
    end

    local function rdm(level)
        return xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON, job = xi.job.RDM, level = level })
    end

    after_each(function()
        -- The spells are shared by every test: put them back to vanilla
        for _, name in ipairs(toggles) do
            xi.test.world:setSetting('custom.' .. name, false)
        end

        xi.custom.spells.apply()
    end)

    it('vanilla: Enfire is self only', function()
        set('ENSPELL_PARTY_TARGET', false)
        party()
        assert(GetSpell(xi.magic.spell.ENFIRE):getValidTarget() == xi.targetType.SELF, 'Expected Enfire to be self only')

        cast(ally, xi.magic.spell.ENFIRE)
        assert(not ally:hasStatusEffect(xi.effect.ENFIRE), 'Expected no Enfire on the party member')
    end)

    it('ENSPELL_PARTY_TARGET: Enfire can be cast on a party member', function()
        set('ENSPELL_PARTY_TARGET', true)
        party()

        cast(ally, xi.magic.spell.ENFIRE)
        assert(ally:hasStatusEffect(xi.effect.ENFIRE), 'Expected Enfire on the party member')
        assert(not caster:hasStatusEffect(xi.effect.ENFIRE), 'Expected no Enfire on the caster')
    end)

    it('ENSPELL_PARTY_TARGET: Enfire on yourself still works', function()
        set('ENSPELL_PARTY_TARGET', true)
        party()

        cast(caster, xi.magic.spell.ENFIRE)
        assert(caster:hasStatusEffect(xi.effect.ENFIRE), 'Expected Enfire on the caster')
    end)

    it('ENSPELL_PARTY_TARGET: not on a party member out of range', function()
        set('ENSPELL_PARTY_TARGET', true)
        party()
        ally:setPos(caster:getXPos() + 40, caster:getYPos(), caster:getZPos())

        cast(ally, xi.magic.spell.ENFIRE)
        assert(not ally:hasStatusEffect(xi.effect.ENFIRE), 'Expected no Enfire on the far party member')
    end)

    it('vanilla: a level 1 Red Mage cannot learn Enstone, nor a level 2 one Haste', function()
        assert(rdm(1):canLearnSpell(xi.magic.spell.ENSTONE) ~= 0, 'Expected Enstone to need a higher level')
        assert(rdm(2):canLearnSpell(xi.magic.spell.HASTE) ~= 0, 'Expected Haste to need a higher level')
        assert(GetSpell(xi.magic.spell.ENSTONE):getJobLevel(xi.job.RDM) == 18, 'Expected Enstone at RDM 18')
        assert(GetSpell(xi.magic.spell.HASTE):getJobLevel(xi.job.RDM) == 48, 'Expected Haste at RDM 48')
    end)

    it('RDM_HASTE_LEVEL_2: a level 2 Red Mage learns Haste; White Mage is unchanged', function()
        set('RDM_HASTE_LEVEL_2', true)
        assert(rdm(2):canLearnSpell(xi.magic.spell.HASTE) == 0, 'Expected Haste to be learnable at 2')
        assert(GetSpell(xi.magic.spell.HASTE):getJobLevel(xi.job.WHM) == 40, 'Expected Haste still at WHM 40')
    end)

    it('HASTE_MP_10: Haste costs 10 MP', function()
        assert(GetSpell(xi.magic.spell.HASTE):getMPCost() == 40, 'Expected vanilla Haste at 40 MP')
        set('HASTE_MP_10', true)
        assert(GetSpell(xi.magic.spell.HASTE):getMPCost() == 10, 'Expected Haste at 10 MP')

        party()
        cast(ally, xi.magic.spell.HASTE)
        assert(ally:hasStatusEffect(xi.effect.HASTE), 'Expected Haste on the party member')
        assert(caster:getMP() == 490, string.format('Expected 10 MP spent, have %d left', caster:getMP()))
    end)

    it('HASTE_NO_LEVEL_PENALTY: full duration on a level 10 target', function()
        party(10)
        cast(ally, xi.magic.spell.HASTE)
        local short = ally:getStatusEffect(xi.effect.HASTE):getDuration()

        set('HASTE_NO_LEVEL_PENALTY', true)
        party(10)
        cast(ally, xi.magic.spell.HASTE)
        local full = ally:getStatusEffect(xi.effect.HASTE):getDuration()

        assert(full >= short * 3.9, string.format('Expected about 4x the duration, got %s vs %s', full, short))
    end)

    it('ENSTONE_NO_LEVEL_PENALTY: full duration on a level 10 party member', function()
        set('ENSPELL_PARTY_TARGET', true)
        party(10)
        cast(ally, xi.magic.spell.ENSTONE)
        local short = ally:getStatusEffect(xi.effect.ENSTONE):getDuration()

        set('ENSTONE_NO_LEVEL_PENALTY', true)
        party(10)
        cast(ally, xi.magic.spell.ENSTONE)
        local full = ally:getStatusEffect(xi.effect.ENSTONE):getDuration()

        assert(full >= short * 1.75, string.format('Expected about 1.8x the duration, got %s vs %s', full, short))
    end)

    it('HASTE_30_PERCENT: Haste gives as much haste as Haste II', function()
        party()
        cast(ally, xi.magic.spell.HASTE)
        local vanilla = ally:getStatusEffect(xi.effect.HASTE):getPower()
        assert(vanilla == 1465, string.format('Expected vanilla Haste at 1465, got %s', vanilla))

        set('HASTE_30_PERCENT', true)
        party()
        cast(ally, xi.magic.spell.HASTE)
        local power = ally:getStatusEffect(xi.effect.HASTE):getPower()
        assert(power == 2998, string.format('Expected Haste at 2998, got %s', power))
        assert(ally:getMod(xi.mod.HASTE_MAGIC) == 2998, string.format('Expected 2998 magic haste, got %s', ally:getMod(xi.mod.HASTE_MAGIC)))
    end)
end)

describe('Custom !resist command', function()
    local ffi = require('ffi')

    pcall(ffi.cdef, [[
        typedef struct {
            uint16_t id : 9;
            uint16_t size : 7;
            uint16_t sync;
            uint8_t  Kind;
            uint8_t  unknown00;
            char     Str[128];
        } GP_CLI_COMMAND_CHAT_STD;
    ]])

    local player
    local mob

    local function sendChat(message)
        local packet = ffi.new('GP_CLI_COMMAND_CHAT_STD')
        ffi.copy(packet.Str, message)
        player.packets:send(0x0B5, packet, ffi.sizeof(packet))
        xi.test.world:skipTime(1) -- past the chat rate limit
    end

    -- Text of every CHAT_STD (0x017) message sent to the player that starts with [RES]
    local function resistLines()
        local lines = {}
        for _, pkt in pairs(player.packets:getIncoming()) do
            if pkt.type == 0x017 then
                local s = ''
                for i = 23, pkt.size - 1 do
                    local byte = pkt.data[i]
                    if byte == nil or byte == 0 then
                        break
                    end

                    s = s .. string.char(byte)
                end

                if string.sub(s, 1, 5) == '[RES]' then
                    table.insert(lines, s)
                end
            end
        end

        return lines
    end

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON })
        mob    = player.entities:get('Volcanic_Bomb')
        mob:respawn()
        -- No weather and no elemental day, so day/weather is x1.00 for every element
        player:setWeather(xi.weather.NONE)
        stub('VanadielDayElement', xi.element.NONE)
        player.packets:clear()
    end)

    it('vanilla: answers that it is turned off', function()
        sendChat(string.format('!resist %d', mob:getID()))
        local lines = resistLines()
        assert(#lines == 1 and string.find(lines[1], 'turned off', 1, true), string.format('Expected the off message, got: %s', table.concat(lines, ' | ')))
    end)

    it('RESIST_COMMAND: prints the Bomb family resistances', function()
        xi.test.world:setSetting('custom.RESIST_COMMAND', true)
        sendChat(string.format('!resist %d', mob:getID()))
        local lines = resistLines()

        assert(#lines == 6, string.format('Expected 6 lines, got %d: %s', #lines, table.concat(lines, ' | ')))
        assert(string.find(lines[1], string.format('[RES] %d H lv=', mob:getID()), 1, true), 'Bad header: ' .. lines[1])
        assert(string.find(lines[1], 'name=Volcanic_Bomb', 1, true), 'Missing name: ' .. lines[1])
        -- Bomb resistances (mob_resistances 56): fire rank -3, every other element rank 4
        assert(string.find(lines[2], 'fire=-3,', 1, true), 'Expected fire rank -3: ' .. lines[2])
        assert(string.find(lines[2], 'ice=4,', 1, true), 'Expected ice rank 4: ' .. lines[2])
        assert(string.find(lines[3], 'dark=4,', 1, true), 'Expected dark rank 4: ' .. lines[3])

        for _, line in ipairs(lines) do
            assert(#line < 150, string.format('Line over the chat limit (%d bytes): %s', #line, line))
        end
    end)

    it('RESIST_COMMAND: multipliers include the rank 4 cut', function()
        xi.test.world:setSetting('custom.RESIST_COMMAND', true)
        sendChat(string.format('!resist %d', mob:getID()))
        local lines = resistLines()
        assert(lines[4] and string.find(lines[4], 'D fire=100 ice=100', 1, true), 'Bad day/weather line: ' .. tostring(lines[4]))
        assert(lines[6] and string.find(lines[6], 'M dw=0 fire=100 ice=50 ', 1, true), 'Bad multiplier line: ' .. tostring(lines[6]))
    end)

    it('RESIST_COMMAND + DAY_WEATHER_ALWAYS: multipliers include day and weather', function()
        xi.test.world:setSetting('custom.RESIST_COMMAND', true)
        xi.test.world:setSetting('custom.DAY_WEATHER_ALWAYS', true)
        player:setWeather(xi.weather.HEAT_WAVE)
        stub('VanadielDayElement', xi.element.FIRE)
        sendChat(string.format('!resist %d', mob:getID()))
        local lines = resistLines()
        -- Fire: 1 + 0.25 (Heat Wave) + 0.10 (Firesday). Ice is beaten by fire: 1 - 0.25 - 0.10.
        -- Wind is untouched.
        assert(lines[4] and string.find(lines[4], 'D fire=135 ice=65 wind=100', 1, true), 'Bad day/weather line: ' .. tostring(lines[4]))
        -- Bomb: fire rank -3 (no cut), ice rank 4 (x0.5): 0.5 x 0.65 = 0.325
        assert(lines[6] and string.find(lines[6], 'M dw=1 fire=135 ice=33 wind=50', 1, true), 'Bad multiplier line: ' .. tostring(lines[6]))

        for _, line in ipairs(lines) do
            assert(#line < 150, string.format('Line over the chat limit (%d bytes): %s', #line, line))
        end
    end)

    it('RESIST_COMMAND + RESIST_RANK_DAMAGE_RATE: multipliers use the rank damage rates', function()
        xi.test.world:setSetting('custom.RESIST_COMMAND', true)
        xi.test.world:setSetting('custom.RESIST_RANK_DAMAGE_RATE', true)
        sendChat(string.format('!resist %d', mob:getID()))
        local lines = resistLines()
        -- Bomb: fire rank -3 -> x1.5, the rest rank 4 -> x0.5
        assert(lines[6] and string.find(lines[6], 'M dw=0 fire=150 ice=50 ', 1, true), 'Bad multiplier line: ' .. tostring(lines[6]))
    end)

    it('RESIST_COMMAND: prints the melee hit chance', function()
        xi.test.world:setSetting('custom.RESIST_COMMAND', true)
        sendChat(string.format('!resist %d', mob:getID()))
        local lines = resistLines()
        local main = lines[5] and tonumber(string.match(lines[5], '^%[RES%] %d+ P main=(%d+)'))
        assert(main and main >= 20 and main <= 99, 'Bad hit line: ' .. tostring(lines[5]))
        local expected = math.floor(xi.combat.physicalHitRate.getPhysicalHitRate(player, mob, 0, xi.attackAnimation.RIGHT_ATTACK, false) * 100)
        assert(main == expected, string.format('Expected %d, got %d', expected, main))
    end)

    it('RESIST_COMMAND: the hit chance includes accuracy buffs', function()
        xi.test.world:setSetting('custom.RESIST_COMMAND', true)
        -- A level 75 character, so the hit chance is between the 20% floor and the 99% cap
        player = xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON, level = 75 })
        player.packets:clear()
        sendChat(string.format('!resist %d', mob:getID()))
        local before = tonumber(string.match(resistLines()[5] or '', 'main=(%d+)'))

        player:addMod(xi.mod.ACC, 400)
        player.packets:clear()
        sendChat(string.format('!resist %d', mob:getID()))
        local after = tonumber(string.match(resistLines()[5] or '', 'main=(%d+)'))

        assert(before and after and after > before, string.format('Expected more hit chance with +400 accuracy: %s -> %s', tostring(before), tostring(after)))
    end)

    it('RESIST_COMMAND: shows a live absorb mod', function()
        xi.test.world:setSetting('custom.RESIST_COMMAND', true)
        mob:setMod(xi.mod.FIRE_ABSORB, 100)
        sendChat(string.format('!resist %d', mob:getID()))
        local lines = resistLines()
        assert(lines[2] and string.find(lines[2], 'fire=-3,0,0,100,0', 1, true), 'Expected fire absorb 100: ' .. tostring(lines[2]))
    end)

    it('RESIST_COMMAND: refuses a player as target', function()
        xi.test.world:setSetting('custom.RESIST_COMMAND', true)
        sendChat(string.format('!resist %d', player:getID()))
        local lines = resistLines()
        assert(#lines == 1 and string.find(lines[1], '[RES] ERR', 1, true), 'Expected an error: ' .. table.concat(lines, ' | '))
    end)
end)

describe('Custom day and weather always apply', function()
    local player

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON })
        player:setWeather(xi.weather.HOT_SPELL)
        stub('VanadielDayElement', xi.element.NONE)
    end)

    -- Distinct results of many fire day/weather multipliers for the player
    local function results(times)
        local seen = {}
        for _ = 1, times do
            local value = xi.spells.damage.calculateDayAndWeather(player, xi.element.FIRE, false)
            seen[string.format('%.2f', value)] = true
        end

        return seen
    end

    it('vanilla: Hot Spell boosts fire only on some casts', function()
        local seen = results(300)
        assert(seen['1.00'] and seen['1.10'], 'Expected both 1.00 and 1.10 over 300 casts')
    end)

    it('DAY_WEATHER_ALWAYS: Hot Spell boosts every fire cast', function()
        xi.test.world:setSetting('custom.DAY_WEATHER_ALWAYS', true)
        local seen = results(100)
        assert(seen['1.10'] and not seen['1.00'], 'Expected 1.10 on every cast')
    end)

    it('DAY_WEATHER_ALWAYS: penalties apply every time too', function()
        xi.test.world:setSetting('custom.DAY_WEATHER_ALWAYS', true)
        player:setWeather(xi.weather.RAIN)
        stub('VanadielDayElement', xi.element.WATER)
        local seen = results(50)
        -- Fire on Watersday in rain: 1 - 0.10 - 0.10
        assert(seen['0.80'] and not seen['1.00'], 'Expected 0.80 on every cast')
    end)
end)

describe('Custom resistance rank damage rates', function()
    local player
    local mob

    before_each(function()
        player = xi.test.world:spawnPlayer({ zone = xi.zone.IFRITS_CAULDRON })
        mob    = player.entities:get('Volcanic_Bomb')
        mob:respawn()
    end)

    local function rate(rank)
        mob:setMod(xi.mod.FIRE_RES_RANK, rank)
        return xi.spells.damage.calculateAdditionalResistTier(player, mob, xi.element.FIRE)
    end

    it('vanilla: x0.5 from rank 4, nothing below', function()
        assert(rate(-3) == 1 and rate(2) == 1 and rate(3) == 1, 'Expected x1 below rank 4')
        assert(rate(4) == 0.5 and rate(11) == 0.5, 'Expected x0.5 from rank 4')
    end)

    it('RESIST_RANK_DAMAGE_RATE: the rank damage rate table', function()
        xi.test.world:setSetting('custom.RESIST_RANK_DAMAGE_RATE', true)
        local expected =
        {
            [-3] = 1.5, [-2] = 1.25, [-1] = 1, [0] = 1, [1] = 1, [2] = 0.75, [3] = 0.5, [4] = 0.5,
            [5] = 0.5, [6] = 0.25, [7] = 0.25, [8] = 0.25, [9] = 0.25, [10] = 0.25, [11] = 0.25,
        }
        for rank = -3, 11 do
            local got = rate(rank)
            assert(got == expected[rank], string.format('Rank %d: expected %s, got %s', rank, expected[rank], got))
        end
    end)

    it('RESIST_RANK_DAMAGE_RATE: Subtle Sorcery ignores the reduction, keeps the bonus', function()
        xi.test.world:setSetting('custom.RESIST_RANK_DAMAGE_RATE', true)
        player:addStatusEffect(xi.effect.SUBTLE_SORCERY, { power = 1, duration = 60, origin = player })
        assert(rate(6) == 1, 'Expected x1 at rank 6 with Subtle Sorcery')
        assert(rate(-3) == 1.5, 'Expected x1.5 at rank -3 with Subtle Sorcery')
    end)

    describe('rank 11 nukes', function()
        local function resist()
            mob:setMod(xi.mod.FIRE_RES_RANK, 11)
            return xi.combat.magicHitRate.calculateResistRate(player, mob, { magicalElement = xi.element.FIRE, skillType = xi.skill.ELEMENTAL_MAGIC })
        end

        before_each(function()
            stub('math.randomFloat', 0) -- every resist roll lands
        end)

        it('vanilla: always 1/4', function()
            assert(resist() == 0.25, 'Expected an automatic 1/4')
        end)

        it('RESIST_RANK_DAMAGE_RATE: roll resists like rank 10', function()
            xi.test.world:setSetting('custom.RESIST_RANK_DAMAGE_RATE', true)
            assert(resist() == 1, 'Expected an unresisted roll')
        end)

        it('status effects stay immune either way', function()
            xi.test.world:setSetting('custom.RESIST_RANK_DAMAGE_RATE', true)
            mob:setMod(xi.mod.FIRE_RES_RANK, 11)
            local r = xi.combat.magicHitRate.calculateResistRate(player, mob, { magicalElement = xi.element.FIRE, effectId = xi.effect.BURN })
            assert(r == 0, 'Expected immunity, got ' .. tostring(r))
        end)
    end)
end)
