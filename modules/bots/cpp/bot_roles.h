//
// Bot Players: what each job does in a party.
//
//   Data for the AI in bot_players.cpp. A job has an ordered list of rules; every decision
//   the bot walks the list and the first rule that finds something to do wins.
//
//   To change how a job plays, edit its rule list or the spell tables below and rebuild.
//   To add a job, add a Role and a case in roleFor().
//

#pragma once

#include "map/spell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace bots
{

enum class Act : uint8
{
    Heal,   // Smart Heal: the Cure that fits what somebody is missing
    Buff,   // Smart Buff: the first buff of kBuffs somebody lacks
    Debuff, // Smart Debuff: the first debuff of kDebuffs the enemy lacks
    Nuke,   // Smart Spell: the elemental spell that hurts the enemy most
    Provoke,    // Provoke, when the enemy is attacking somebody who is not a tank
    Steal,      // Steal, whenever it is ready
    Skillchain, // A weapon skill that continues the skillchain open on the enemy
    Shoot,  // Ranged attack, while the bot has a ranged weapon and ammo for it
    Melee,  // Auto attack
    Rest,   // Sit down (/heal)
};

struct Rule
{
    Act   act;
    uint8 maxLevel = 99; // The rule is skipped above this main job level
    uint8 minMpp   = 0;  // The rule is skipped under this MP%
};

struct Role
{
    std::span<const Rule> rules;

    // Who goes first when two bots could do the same thing. The higher rank takes it; the
    // lower one only steps in when the higher one can't.
    uint8 healRank;    // Heal
    uint8 supportRank; // Buff, Debuff

    // A mage only attacks or casts on an enemy a party member is already fighting, and
    // stands well back from it while it is not meleeing.
    bool mage;
};

inline constexpr std::array kWhiteMageRules{
    Rule{ Act::Heal },
    Rule{ Act::Buff },
    Rule{ Act::Debuff },
    Rule{ Act::Skillchain },
    Rule{ Act::Melee, 6 },
    Rule{ Act::Rest },
};

// Fights in melee. Under half its MP it keeps what is left for cures and debuffs.
inline constexpr std::array kRedMageRules{
    Rule{ Act::Heal },
    Rule{ Act::Buff, 99, 50 },
    Rule{ Act::Debuff },
    Rule{ Act::Nuke, 99, 50 },
    Rule{ Act::Skillchain },
    Rule{ Act::Melee },
    Rule{ Act::Rest },
};

// Not asked for, but a Black Mage should not melee either: debuffs and nukes from range.
inline constexpr std::array kBlackMageRules{
    Rule{ Act::Heal },
    Rule{ Act::Debuff },
    Rule{ Act::Nuke },
    Rule{ Act::Skillchain },
    Rule{ Act::Melee, 6 },
    Rule{ Act::Rest },
};

// Shoots from a distance; out of ammo, it falls through to Melee.
inline constexpr std::array kRangerRules{
    Rule{ Act::Heal },
    Rule{ Act::Skillchain },
    Rule{ Act::Shoot },
    Rule{ Act::Melee },
    Rule{ Act::Rest },
};

inline constexpr std::array kThiefRules{
    Rule{ Act::Heal },
    Rule{ Act::Steal },
    Rule{ Act::Skillchain },
    Rule{ Act::Melee },
    Rule{ Act::Rest },
};

// Every other job: fights next to the leader, and cures if it happens to know a Cure.
inline constexpr std::array kFighterRules{
    Rule{ Act::Heal },
    Rule{ Act::Provoke },
    Rule{ Act::Skillchain },
    Rule{ Act::Melee },
    Rule{ Act::Rest },
};

inline constexpr Role kWhiteMage{ kWhiteMageRules, 2, 1, true };
inline constexpr Role kRedMage{ kRedMageRules, 1, 2, true };
inline constexpr Role kBlackMage{ kBlackMageRules, 0, 0, true };
inline constexpr Role kRanger{ kRangerRules, 0, 0, false };
inline constexpr Role kThief{ kThiefRules, 0, 0, false };
inline constexpr Role kFighter{ kFighterRules, 0, 0, false };

inline auto roleFor(const xi::Job job) -> const Role&
{
    switch (job)
    {
        case xi::Job::WHM:
            return kWhiteMage;
        case xi::Job::RDM:
            return kRedMage;
        case xi::Job::BLM:
            return kBlackMage;
        case xi::Job::RNG:
            return kRanger;
        case xi::Job::THF:
            return kThief;
        default:
            return kFighter;
    }
}

// Jobs the party wants the enemy to attack: a bot does not Provoke it off them
inline auto isTank(const xi::Job job) -> bool
{
    return job == xi::Job::WAR || job == xi::Job::PLD || job == xi::Job::NIN || job == xi::Job::RUN;
}

//
// Jobs a bot can be created as: the six starting jobs, and the advanced jobs below, which
// xi.player.charCreate has no starting gear for.
//

struct StarterItem
{
    xi::Job job;
    uint16  item;
    uint8   quantity;
};

inline constexpr std::array kStarterGear{
    StarterItem{ xi::Job::RNG, 17152, 1 },  // Shortbow
    StarterItem{ xi::Job::RNG, 17318, 99 }, // Wooden Arrow
    StarterItem{ xi::Job::RNG, 16640, 1 },  // Bronze Axe
    StarterItem{ xi::Job::SAM, 17809, 1 },  // Mumeito, the only level 1 great katana
    StarterItem{ xi::Job::NIN, 16896, 1 },  // Kunai
};

// What xi.player.charCreate gives a new character of the six starting jobs. A bot that
// changes to one of them for the first time gets the same.
inline constexpr std::array kFirstJobGear{
    StarterItem{ xi::Job::WAR, 16534, 1 }, // Onion Sword
    StarterItem{ xi::Job::MNK, 13184, 1 }, // White Belt
    StarterItem{ xi::Job::WHM, 17068, 1 }, // Onion Rod
    StarterItem{ xi::Job::WHM, 4608, 1 },  // Scroll of Cure
    StarterItem{ xi::Job::BLM, 17104, 1 }, // Onion Staff
    StarterItem{ xi::Job::BLM, 4607, 1 },  // Scroll of Stone
    StarterItem{ xi::Job::RDM, 16482, 1 }, // Onion Dagger
    StarterItem{ xi::Job::RDM, 4606, 1 },  // Scroll of Dia
    StarterItem{ xi::Job::THF, 16483, 1 }, // Onion Knife
};

// Whether a bot can have this job, as main job or subjob
inline auto isBotJob(const uint8 job) -> bool
{
    return (job >= 1 && job <= 6) || (job >= 11 && job <= 13);
}

// char_jobs column of an advanced job a bot can start as, nullptr for any other job
inline auto advancedJobColumn(const uint8 job) -> const char*
{
    switch (static_cast<xi::Job>(job))
    {
        case xi::Job::RNG:
            return "rng";
        case xi::Job::SAM:
            return "sam";
        case xi::Job::NIN:
            return "nin";
        default:
            return nullptr;
    }
}

//
// Spell tables. Tiers are listed lowest first; the bot uses the highest one it can cast.
// Spell ids are spell_list.spellid (SpellID in map/spell.h).
//

constexpr size_t kMaxTiers = 5;

using Tiers = std::array<uint16, kMaxTiers>;

enum class BuffFor : uint8
{
    Party,     // Each party member in range, one at a time
    PartyArea, // Cast on the bot itself when two or more members around it lack the effect
    Fighters,  // Members whose main job has no MP
    Casters,   // Members whose main job has MP
    Hurt,      // A member under kRegenBelowHpp
    Self,
};

struct BuffLine
{
    Tiers            spells;
    xi::StatusEffect effect;
    BuffFor          who;
    bool             inFight; // false: only cast while nobody in the party is fighting
};

// In priority order
inline constexpr std::array kBuffs{
    BuffLine{ { 125, 126, 127, 128, 129 }, xi::StatusEffect::Protect, BuffFor::PartyArea, true }, // Protectra I-V
    BuffLine{ { 43, 44, 45, 46, 47 }, xi::StatusEffect::Protect, BuffFor::Party, true },          // Protect I-V
    BuffLine{ { 130, 131, 132, 133, 134 }, xi::StatusEffect::Shell, BuffFor::PartyArea, true },   // Shellra I-V
    BuffLine{ { 48, 49, 50, 51, 52 }, xi::StatusEffect::Shell, BuffFor::Party, true },            // Shell I-V
    BuffLine{ { 108, 110, 111, 477, 504 }, xi::StatusEffect::Regen, BuffFor::Hurt, true },        // Regen I-V
    BuffLine{ { 57, 511 }, xi::StatusEffect::Haste, BuffFor::Fighters, true },                    // Haste I-II
    BuffLine{ { 109, 473, 894 }, xi::StatusEffect::Refresh, BuffFor::Casters, true },             // Refresh I-III
    BuffLine{ { 106 }, xi::StatusEffect::Phalanx, BuffFor::Self, true },                          // Phalanx
    BuffLine{ { 54 }, xi::StatusEffect::Stoneskin, BuffFor::Self, false },                        // Stoneskin
    BuffLine{ { 53 }, xi::StatusEffect::Blink, BuffFor::Self, false },                            // Blink
    BuffLine{ { 55 }, xi::StatusEffect::Aquaveil, BuffFor::Self, false },                         // Aquaveil
};

struct DebuffLine
{
    Tiers            spells;
    xi::StatusEffect effect;
    xi::StatusEffect blockedBy; // Not cast while the enemy has this. None: no such effect
    xi::Immunity     immunity;  // None: nothing is immune
    xi::Mod          resRank;   // The enemy's resistance rank to it. NONE: not ranked
    uint8            maxLevel;  // Not worth casting above this main job level
};

// In priority order. Dispel is handled apart (it needs something to dispel).
// Bio and Dia replace each other, so only one goes on: Bio when somebody can cast it.
// Burn, Choke and Shock are the three elemental debuffs that don't cancel one another.
inline constexpr std::array kDebuffs{
    DebuffLine{ { 230, 231, 232 }, xi::StatusEffect::Bio, xi::StatusEffect::Dia, xi::Immunity::None, xi::Mod::NONE, 99 },                   // Bio I-III
    DebuffLine{ { 23, 24, 25 }, xi::StatusEffect::Dia, xi::StatusEffect::Bio, xi::Immunity::None, xi::Mod::NONE, 99 },                   // Dia I-III
    DebuffLine{ { 58, 80 }, xi::StatusEffect::Paralysis, xi::StatusEffect::None, xi::Immunity::Paralyze, xi::Mod::PARALYZE_RES_RANK, 99 }, // Paralyze I-II
    DebuffLine{ { 56, 79 }, xi::StatusEffect::Slow, xi::StatusEffect::None, xi::Immunity::Slow, xi::Mod::SLOW_RES_RANK, 99 },              // Slow I-II
    DebuffLine{ { 254, 276 }, xi::StatusEffect::Blindness, xi::StatusEffect::None, xi::Immunity::Blind, xi::Mod::BLIND_RES_RANK, 99 },     // Blind I-II
    DebuffLine{ { 235 }, xi::StatusEffect::Burn, xi::StatusEffect::None, xi::Immunity::None, xi::Mod::NONE, 99 },                          // Burn
    DebuffLine{ { 237 }, xi::StatusEffect::Choke, xi::StatusEffect::None, xi::Immunity::None, xi::Mod::NONE, 99 },                         // Choke
    DebuffLine{ { 239 }, xi::StatusEffect::Shock, xi::StatusEffect::None, xi::Immunity::None, xi::Mod::NONE, 99 },                         // Shock
    DebuffLine{ { 220, 221 }, xi::StatusEffect::Poison, xi::StatusEffect::None, xi::Immunity::Poison, xi::Mod::POISON_RES_RANK, 20 },      // Poison I-II
};

constexpr uint16 kDispel = 260;

// Elemental spells: Fire, Blizzard, Aero, Stone, Thunder, Water, tiers I-V each
constexpr uint16 kFirstNuke = 144;
constexpr uint16 kLastNuke  = 173;

// Cure I-VI
constexpr uint16 kFirstCure = 1;
constexpr size_t kCureTiers = 6;

//
// Estimates
//

// How much a Cure of `tier` (0 = Cure, 5 = Cure VI) heals with this cure power
// (MND / 2 + VIT / 4 + healing magic skill), before cure potency. Mirrors getBaseCure() in
// scripts/globals/magic.lua and the bands in scripts/actions/spells/white/cure*.lua.
inline auto cureAmount(const size_t tier, const float power) -> float
{
    struct Band
    {
        float below, divisor, constant, basePower;
    };

    struct Cure
    {
        float               min;
        float               cap;
        std::array<Band, 6> bands;
    };

    // clang-format off
    static constexpr std::array<Cure, 5> cures{ {
        { 10.0f,  65.0f,  { { { 20, 4, 10, 0 }, { 40, 1.3333f, 15, 20 }, { 125, 8.5f, 30, 40 }, { 200, 15, 40, 125 }, { 600, 20, 40, 200 }, { 0, 0, 0, 0 } } } },
        { 60.0f,  145.0f, { { { 70, 1, 60, 40 }, { 125, 5.5f, 90, 70 }, { 200, 7.5f, 100, 125 }, { 400, 10, 110, 200 }, { 700, 20, 130, 400 }, { 0, 0, 0, 0 } } } },
        { 130.0f, 340.0f, { { { 125, 2.2f, 130, 70 }, { 200, 75.0f / 65.0f, 155, 125 }, { 300, 2.5f, 220, 200 }, { 700, 5, 260, 300 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } } },
        { 270.0f, 640.0f, { { { 200, 1, 270, 70 }, { 300, 2, 400, 200 }, { 400, 10.0f / 7.0f, 450, 300 }, { 700, 2.5f, 520, 400 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } } } },
        { 450.0f, 780.0f, { { { 150, 0.70f, 450, 80 }, { 190, 1.25f, 550, 150 }, { 260, 70.0f / 38.0f, 582, 190 }, { 300, 2, 620, 260 }, { 500, 2.5f, 640, 300 }, { 700, 10.0f / 3.0f, 720, 500 } } } },
    } };
    // clang-format on

    if (tier >= cures.size())
    {
        // Cure VI: no table here, it is well above Cure V
        return cureAmount(cures.size() - 1, power) * 1.4f;
    }

    const auto& cure = cures[tier];

    for (const auto& band : cure.bands)
    {
        if (band.divisor > 0.0f && power < band.below)
        {
            return std::max(cure.min, std::floor((power - band.basePower) / band.divisor + band.constant));
        }
    }

    return cure.cap;
}

// Damage multiplier for an elemental resistance rank (-3 weak .. 11 resistant), as in
// scripts/combat/skillchain.lua.
inline auto resistRankMultiplier(const int16 rank) -> float
{
    static constexpr std::array<float, 15> multipliers{ 1.50f, 1.30f, 1.15f, 1.00f, 0.85f, 0.70f, 0.60f, 0.50f, 0.40f, 0.30f, 0.25f, 0.20f, 0.15f, 0.10f, 0.05f };

    return multipliers[static_cast<size_t>(std::clamp<int16>(rank, -3, 11) + 3)];
}

} // namespace bots
