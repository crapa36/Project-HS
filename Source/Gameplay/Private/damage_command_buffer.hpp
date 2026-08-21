#pragma once

#include <hs/game_domain/game_types.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace hs::gameplay_detail
{

inline constexpr std::uint8_t kNoCombatSource = 0xFF;

struct DamageCommand
{
    std::uint64_t sequence{};
    std::uint64_t target{};
    std::int32_t amount{};
    SkillKind skill{SkillKind::BasicAttack};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint64_t cast_id{};
    std::uint8_t source_upgrade{kNoCombatSource};
    std::uint8_t source_relic{kNoCombatSource};
    std::uint8_t source_enemy{kNoCombatSource};
    std::uint8_t bleed_stacks{};
    bool burn{};
    float slow_reduction{};
    Tick slow_duration{};
    std::array<std::uint8_t, 3> amplified_upgrades{};
    std::array<std::int32_t, 3> amplified_damage{};
    std::uint8_t amplified_count{};
};

enum class ProcPermission : std::uint8_t
{
    Damage = 1u << 0,
    Status = 1u << 1,
    ReactiveRule = 1u << 2,
};

struct ProcContext
{
    std::uint64_t root_cast{};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint8_t permissions{};

    [[nodiscard]] bool Allows(ProcPermission permission) const noexcept
    {
        return (permissions & static_cast<std::uint8_t>(permission)) != 0;
    }
};

[[nodiscard]] inline ProcContext MakeProcContext(std::uint64_t cast_id,
                                                  EffectOrigin origin) noexcept
{
    const auto damage = static_cast<std::uint8_t>(ProcPermission::Damage);
    const auto status = static_cast<std::uint8_t>(ProcPermission::Status);
    const auto reactive = static_cast<std::uint8_t>(ProcPermission::ReactiveRule);
    return {cast_id, origin,
            origin == EffectOrigin::Original
                ? static_cast<std::uint8_t>(damage | status | reactive)
                : origin == EffectOrigin::Derived
                      ? static_cast<std::uint8_t>(damage | status)
                      : damage};
}

struct DamageCommandBuffer
{
    std::vector<DamageCommand> commands;

    void Reserve(std::size_t count) { commands.reserve(count); }
    void Enqueue(DamageCommand command) { commands.push_back(std::move(command)); }
    void SortByTargetAndSequence()
    {
        std::ranges::sort(commands, [](const DamageCommand &left,
                                      const DamageCommand &right) {
            return std::tie(left.target, left.sequence) <
                   std::tie(right.target, right.sequence);
        });
    }
    void Clear() noexcept { commands.clear(); }
};

} // namespace hs::gameplay_detail
