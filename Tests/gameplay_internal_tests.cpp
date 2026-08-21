#include "relic_rule_table.hpp"
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
        RelicRuleTable table;
        const auto acquired = static_cast<std::uint16_t>(
            (1u << static_cast<unsigned>(RelicKind::BurnPropagation)) |
            (1u << static_cast<unsigned>(RelicKind::BleedKillHeal)) |
            (1u << static_cast<unsigned>(RelicKind::KillCooldownSurge)));
        table.Rebuild(acquired);
        const auto rules = table.RulesFor(RelicRuleHook::OnEnemyKilled);
        Check(rules.size() == 3, "only acquired rules are active");
        Check(std::ranges::is_sorted(rules, {}, &ActiveRelicRule::id),
              "equal-priority rules use stable typed ids");
        Check(rules[0].handler == RelicRuleHandlerId::BleedKillHeal &&
                  rules[1].handler == RelicRuleHandlerId::BurnPropagation &&
                  rules[2].handler == RelicRuleHandlerId::KillCooldownSurge,
              "active rules resolve to static handlers");
        std::vector<RelicRuleHandlerId> executed;
        for (const auto &rule : table.RulesFor(RelicRuleHook::OnEnemyKilled))
            executed.push_back(rule.handler);
        Check(executed == std::vector{RelicRuleHandlerId::BleedKillHeal,
                                     RelicRuleHandlerId::BurnPropagation,
                                     RelicRuleHandlerId::KillCooldownSurge},
              "dispatcher preserves the ordered hook table");
        Check(table.RulesFor(RelicRuleHook::OnProjectileHit).empty() &&
                  table.RulesFor(RelicRuleHook::BeforeDamage).empty(),
              "unused semantic hooks remain explicit and allocation free");
        constexpr auto &pipeline = kSimulationPipeline;
        Check(pipeline[2] == SimulationPhaseId::SpawnBarrier &&
                  pipeline[5] == SimulationPhaseId::AbilitySpawnBarrier &&
                  pipeline[pipeline.size() - 2] == SimulationPhaseId::CleanupBarrier &&
                  pipeline.back() == SimulationPhaseId::GameplayHash,
              "phase-specific visibility barriers are explicit and ordered");
        std::cout << "gameplay_internal_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "gameplay_internal_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
