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

    it('combat: x2 doubles each skill-up, capped at 0.9', function()
        local player = xi.test.world:spawnPlayer({ zone = xi.zone.SOUTHERN_SAN_DORIA, job = xi.job.WAR, level = 50 })
        xi.test.world:setSeed(1)

        local function oneSkillUp(p)
            p:setSkillLevel(xi.skill.SWORD, 100)
            p:trySkillUp(xi.skill.SWORD, 50, true)
            return p:getCharSkillLevel(xi.skill.SWORD) - 100
        end

        xi.test.world:setSetting('custom.COMBAT_SKILLUP_MULTIPLIER', 1.0)
        local vanilla = gainsWith(player, 1.0, 200, oneSkillUp)
        assertOnly(vanilla, { [1] = true, [2] = true, [3] = true, [4] = true, [5] = true }, 'vanilla')

        xi.test.world:setSetting('custom.COMBAT_SKILLUP_MULTIPLIER', 2.0)
        local doubled = gainsWith(player, 2.0, 200, oneSkillUp)
        assertOnly(doubled, { [2] = true, [4] = true, [6] = true, [8] = true, [9] = true }, 'x2')
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
