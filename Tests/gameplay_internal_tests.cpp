#include "relic_rule_table.hpp"
#include "simulation_pipeline.hpp"
#include "spatial_grid.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void TestSpatialGridAgainstBruteForce()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    constexpr float arena_half_extent = 60.0f;
    constexpr float collision_radius = 1.35f;
    const auto segment_circle = [](Float2 from, Float2 to, Float2 center,
                                   float radius) {
        const Float2 segment{to.x - from.x, to.y - from.y};
        const Float2 offset{center.x - from.x, center.y - from.y};
        const auto length_squared = segment.x * segment.x + segment.y * segment.y;
        const auto t = length_squared > 0.0f
                           ? std::clamp((offset.x * segment.x + offset.y * segment.y) /
                                            length_squared,
                                        0.0f, 1.0f)
                           : 0.0f;
        const Float2 closest{from.x + segment.x * t, from.y + segment.y * t};
        const auto x = center.x - closest.x;
        const auto y = center.y - closest.y;
        return x * x + y * y <= radius * radius;
    };
    EnemySpatialGrid grid;
    std::vector<Float2> positions;
    std::uint64_t random = 0xC001D00Du;
    const auto next = [&]() {
        random = random * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((random >> 40) & 0xFFFFFFu) /
               static_cast<float>(0xFFFFFFu);
    };
    for (std::size_t index = 0; index < 300; ++index)
    {
        const Float2 position{(next() * 2.0f - 1.0f) * arena_half_extent,
                              (next() * 2.0f - 1.0f) * arena_half_extent};
        positions.push_back(position);
        const auto x = SpatialGridCoordinate(position.x, arena_half_extent);
        const auto y = SpatialGridCoordinate(position.y, arena_half_extent);
        grid[static_cast<std::size_t>(y * kGridDimension + x)].push_back(index);
    }

    std::vector<std::size_t> candidates;
    for (std::uint32_t query = 0; query < 256; ++query)
    {
        const Float2 from{(next() * 2.0f - 1.0f) * arena_half_extent,
                          (next() * 2.0f - 1.0f) * arena_half_extent};
        const Float2 to{(next() * 2.0f - 1.0f) * arena_half_extent,
                        (next() * 2.0f - 1.0f) * arena_half_extent};
        CollectSpatialGridCandidates(
            grid,
            {std::min(from.x, to.x) - collision_radius,
             std::min(from.y, to.y) - collision_radius},
            {std::max(from.x, to.x) + collision_radius,
             std::max(from.y, to.y) + collision_radius},
            arena_half_extent, candidates);
        std::vector<std::size_t> grid_hits;
        for (const auto index : candidates)
            if (segment_circle(from, to, positions[index], collision_radius))
                grid_hits.push_back(index);
        std::vector<std::size_t> brute_hits;
        for (std::size_t index = 0; index < positions.size(); ++index)
            if (segment_circle(from, to, positions[index], collision_radius))
                brute_hits.push_back(index);
        std::ranges::sort(grid_hits);
        std::ranges::sort(brute_hits);
        Check(grid_hits == brute_hits,
              "spatial grid collision query matches brute force");
    }
}
}

int main()
{
    using namespace hs;
    using namespace hs::gameplay_detail;
    try
    {
        RelicRuleTable table;
        const auto acquired = static_cast<RelicMask>(
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
                  table.RulesFor(RelicRuleHook::BeforeDamage).empty() &&
                  table.RulesFor(RelicRuleHook::OnPickup).empty(),
              "unacquired semantic hooks remain explicit and allocation free");

        const auto new_acquired = static_cast<RelicMask>(
            (RelicMask{1} << static_cast<unsigned>(RelicKind::ProjectileCadenceReward)) |
            (RelicMask{1} << static_cast<unsigned>(RelicKind::PreDamageGuard)) |
            (RelicMask{1} << static_cast<unsigned>(RelicKind::PickupReward)));
        table.Rebuild(new_acquired);
        Check(table.RulesFor(RelicRuleHook::OnProjectileHit).size() == 1 &&
                  table.RulesFor(RelicRuleHook::OnProjectileHit)[0].handler ==
                      RelicRuleHandlerId::ProjectileCadenceReward,
              "projectile cadence reward registers on projectile hit");
        Check(table.RulesFor(RelicRuleHook::BeforeDamage).size() == 1 &&
                  table.RulesFor(RelicRuleHook::BeforeDamage)[0].handler ==
                      RelicRuleHandlerId::PreDamageGuard,
              "pre-damage guard registers before incoming damage");
        Check(table.RulesFor(RelicRuleHook::OnPickup).size() == 1 &&
                  table.RulesFor(RelicRuleHook::OnPickup)[0].handler ==
                      RelicRuleHandlerId::PickupReward,
              "pickup reward registers on pickup collection");
        constexpr auto &pipeline = kSimulationPipeline;
        Check(pipeline[2] == SimulationPhaseId::SpawnBarrier &&
                  pipeline[5] == SimulationPhaseId::AbilitySpawnBarrier &&
                  pipeline[pipeline.size() - 2] == SimulationPhaseId::CleanupBarrier &&
                  pipeline.back() == SimulationPhaseId::GameplayHash,
              "phase-specific visibility barriers are explicit and ordered");
        TestSpatialGridAgainstBruteForce();
        std::cout << "gameplay_internal_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "gameplay_internal_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
