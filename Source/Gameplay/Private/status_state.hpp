#pragma once

#include <hs/game_domain/game_types.hpp>

#include <array>
#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace hs::gameplay_detail
{

inline constexpr std::uint8_t kNoEffectSource = 0xFF;

struct SlowEffect
{
    float reduction{};
    Tick expires{};
    SkillKind source_skill{SkillKind::Count};
    std::uint8_t source_upgrade{kNoEffectSource};
    std::uint8_t source_relic{kNoEffectSource};
};

struct BleedEffect
{
    float attack_snapshot{};
    Tick expires{};
    Tick next_tick{};
    SkillKind source_skill{SkillKind::BasicAttack};
    std::uint8_t source_upgrade{kNoEffectSource};
    std::uint8_t source_relic{kNoEffectSource};
};

struct BurnEffect
{
    float attack_snapshot{};
    Tick expires{};
    Tick next_tick{};
    SkillKind source_skill{SkillKind::BasicAttack};
    bool propagated{};
    std::uint8_t source_upgrade{kNoEffectSource};
    std::uint8_t source_relic{kNoEffectSource};
};

struct StatusState
{
    std::vector<SlowEffect> slows;
    std::array<BleedEffect, 5> bleeds{};
    std::uint8_t bleed_count{};
    std::optional<BurnEffect> burn;

    void AddBleed(const BleedEffect &effect, std::size_t maximum_stacks)
    {
        maximum_stacks = std::min(maximum_stacks, bleeds.size());
        if (maximum_stacks == 0) return;
        if (bleed_count < maximum_stacks) bleeds[bleed_count++] = effect;
        else *std::ranges::min_element(std::span(bleeds).first(bleed_count),
                                       {}, &BleedEffect::expires) = effect;
    }
};

} // namespace hs::gameplay_detail
