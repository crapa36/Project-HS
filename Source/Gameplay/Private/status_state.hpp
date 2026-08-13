#pragma once

#include <hs/game_domain/game_types.hpp>

#include <array>
#include <optional>
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
};

} // namespace hs::gameplay_detail
