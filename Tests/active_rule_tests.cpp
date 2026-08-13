#include "active_rules.hpp"
#include "rule_dispatcher.hpp"
#include "simulation_pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    try
    {
        ActiveRuleTable table;
        const auto acquired = static_cast<std::uint16_t>(
            (1u << static_cast<unsigned>(RelicKind::BurnPropagation)) |
            (1u << static_cast<unsigned>(RelicKind::BleedKillHeal)) |
            (1u << static_cast<unsigned>(RelicKind::KillCooldownSurge)));
        table.Rebuild(acquired);
        const auto rules = table.For(RuleHook::OnEnemyKilled);
        Check(rules.size() == 3, "only acquired rules are active");
        Check(std::ranges::is_sorted(rules, {}, &ActiveRule::id),
              "equal-priority rules use stable typed ids");
        Check(rules[0].handler == RuleHandlerId::BleedKillHeal &&
                  rules[1].handler == RuleHandlerId::BurnPropagation &&
                  rules[2].handler == RuleHandlerId::KillCooldownSurge,
              "active rules resolve to static handlers");
        std::vector<RuleHandlerId> executed;
        DispatchRule<RuleHandlerId::BleedKillHeal>(
            table, RuleHook::OnEnemyKilled,
            [&](const ActiveRule &rule) { executed.push_back(rule.handler); });
        DispatchRule<RuleHandlerId::BurnPropagation>(
            table, RuleHook::OnEnemyKilled,
            [&](const ActiveRule &rule) { executed.push_back(rule.handler); });
        DispatchRule<RuleHandlerId::DamageKnockback>(
            table, RuleHook::OnPlayerDamaged,
            [&](const ActiveRule &rule) { executed.push_back(rule.handler); });
        Check(executed == std::vector{RuleHandlerId::BleedKillHeal,
                                     RuleHandlerId::BurnPropagation},
              "dispatcher executes each acquired handler exactly once");
        Check(table.For(RuleHook::OnProjectileHit).empty() &&
                  table.For(RuleHook::BeforeDamage).empty(),
              "unused semantic hooks remain explicit and allocation free");
        constexpr auto &pipeline = kSimulationPipeline;
        Check(pipeline[2] == SimulationPhaseId::SpawnBarrier &&
                  pipeline[5] == SimulationPhaseId::AbilitySpawnBarrier &&
                  pipeline[pipeline.size() - 2] == SimulationPhaseId::CleanupBarrier &&
                  pipeline.back() == SimulationPhaseId::GameplayHash,
              "phase-specific visibility barriers are explicit and ordered");
        std::cout << "active_rule_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "active_rule_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
