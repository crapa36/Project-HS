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

enum class RuleHook : std::uint8_t
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

enum class RuleHandlerId : std::uint8_t
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

struct ActiveRule
{
    RelicKind id{RelicKind::Count};
    std::int16_t priority{};
    RuleHandlerId handler{RuleHandlerId::BleedKillHeal};
};

struct ActiveRuleTable
{
    std::array<std::vector<ActiveRule>, static_cast<std::size_t>(RuleHook::Count)> hooks;

    void Rebuild(std::uint16_t relic_mask)
    {
        for (auto &rules : hooks) rules.clear();
        const auto add = [&](RuleHook hook, RelicKind relic, RuleHandlerId handler,
                             std::int16_t priority = 0) {
            if ((relic_mask & (1u << static_cast<unsigned>(relic))) == 0) return;
            hooks[static_cast<std::size_t>(hook)].push_back({relic, priority, handler});
        };
        add(RuleHook::OnEnemyKilled, RelicKind::BleedKillHeal,
            RuleHandlerId::BleedKillHeal);
        add(RuleHook::OnEnemyKilled, RelicKind::BurnPropagation,
            RuleHandlerId::BurnPropagation);
        add(RuleHook::OnEnemyKilled, RelicKind::KillCooldownSurge,
            RuleHandlerId::KillCooldownSurge);
        add(RuleHook::OnStatusApplied, RelicKind::BleedBurnExplosion,
            RuleHandlerId::BleedBurnExplosion);
        add(RuleHook::OnCast, RelicKind::RadialBasicAttack,
            RuleHandlerId::RadialBasicAttack);
        add(RuleHook::OnEnemyKilled, RelicKind::BasicKillTracker,
            RuleHandlerId::BasicKillTracker);
        add(RuleHook::OnDistanceMoved, RelicKind::MovementEcho,
            RuleHandlerId::MovementEcho);
        add(RuleHook::OnAbilityUsed, RelicKind::AlternatingSkills,
            RuleHandlerId::AlternatingSkills);
        add(RuleHook::AfterDamage, RelicKind::DifferentSkillTracker,
            RuleHandlerId::DifferentSkillTracker);
        add(RuleHook::OnPlayerDamaged, RelicKind::DamageKnockback,
            RuleHandlerId::DamageKnockback);
        add(RuleHook::OnPlayerDeath, RelicKind::OnceRevive,
            RuleHandlerId::OnceRevive);
        add(RuleHook::AfterDamage, RelicKind::CombatHitChain,
            RuleHandlerId::CombatHitChain);
        for (auto &rules : hooks)
            std::ranges::sort(rules, [](const ActiveRule &left,
                                        const ActiveRule &right) {
                return std::tie(left.priority, left.id) <
                       std::tie(right.priority, right.id);
            });
    }

    [[nodiscard]] std::span<const ActiveRule> For(RuleHook hook) const noexcept
    {
        return hooks[static_cast<std::size_t>(hook)];
    }

};

} // namespace hs::gameplay_detail
