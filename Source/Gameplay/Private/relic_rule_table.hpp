#pragma once

#include <hs/game_domain/game_types.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace hs::gameplay_detail
{

enum class RelicRuleHook : std::uint8_t
{
    OnCast,
    OnProjectileHit,
    BeforeDamage,
    AfterDamage,
    OnStatusApplied,
    OnPlayerDamaged,
    OnPlayerDeath,
    OnEnemyKilled,
    OnAbilityUsed,
    OnDistanceMoved,
    Count,
};

enum class RelicRuleHandlerId : std::uint8_t
{
    BleedKillHeal,
    BurnPropagation,
    KillCooldownSurge,
    BleedBurnExplosion,
    RadialBasicAttack,
    BasicKillTracker,
    MovementEcho,
    AlternatingSkills,
    DifferentSkillTracker,
    DamageKnockback,
    OnceRevive,
    CombatHitChain,
};

struct ActiveRelicRule
{
    RelicKind id{RelicKind::Count};
    std::int16_t priority{};
    RelicRuleHandlerId handler{RelicRuleHandlerId::BleedKillHeal};
};

struct RelicRuleTable
{
    std::array<std::vector<ActiveRelicRule>, static_cast<std::size_t>(RelicRuleHook::Count)> hooks;

    void Rebuild(std::uint16_t relic_mask)
    {
        for (auto &rules : hooks) rules.clear();
        const auto add = [&](RelicRuleHook hook, RelicKind relic, RelicRuleHandlerId handler,
                             std::int16_t priority = 0) {
            if ((relic_mask & (1u << static_cast<unsigned>(relic))) == 0) return;
            hooks[static_cast<std::size_t>(hook)].push_back({relic, priority, handler});
        };
        add(RelicRuleHook::OnEnemyKilled, RelicKind::BleedKillHeal,
            RelicRuleHandlerId::BleedKillHeal);
        add(RelicRuleHook::OnEnemyKilled, RelicKind::BurnPropagation,
            RelicRuleHandlerId::BurnPropagation);
        add(RelicRuleHook::OnEnemyKilled, RelicKind::KillCooldownSurge,
            RelicRuleHandlerId::KillCooldownSurge);
        add(RelicRuleHook::OnStatusApplied, RelicKind::BleedBurnExplosion,
            RelicRuleHandlerId::BleedBurnExplosion);
        add(RelicRuleHook::OnCast, RelicKind::RadialBasicAttack,
            RelicRuleHandlerId::RadialBasicAttack);
        add(RelicRuleHook::OnEnemyKilled, RelicKind::BasicKillTracker,
            RelicRuleHandlerId::BasicKillTracker);
        add(RelicRuleHook::OnDistanceMoved, RelicKind::MovementEcho,
            RelicRuleHandlerId::MovementEcho);
        add(RelicRuleHook::OnAbilityUsed, RelicKind::AlternatingSkills,
            RelicRuleHandlerId::AlternatingSkills);
        add(RelicRuleHook::AfterDamage, RelicKind::DifferentSkillTracker,
            RelicRuleHandlerId::DifferentSkillTracker);
        add(RelicRuleHook::OnPlayerDamaged, RelicKind::DamageKnockback,
            RelicRuleHandlerId::DamageKnockback);
        add(RelicRuleHook::OnPlayerDeath, RelicKind::OnceRevive,
            RelicRuleHandlerId::OnceRevive);
        add(RelicRuleHook::AfterDamage, RelicKind::CombatHitChain,
            RelicRuleHandlerId::CombatHitChain);
        for (auto &rules : hooks)
            std::ranges::sort(rules, [](const ActiveRelicRule &left,
                                        const ActiveRelicRule &right) {
                return std::tie(left.priority, left.id) <
                       std::tie(right.priority, right.id);
            });
    }

    [[nodiscard]] std::span<const ActiveRelicRule> RulesFor(RelicRuleHook hook) const noexcept
    {
        return hooks[static_cast<std::size_t>(hook)];
    }

};

} // namespace hs::gameplay_detail
