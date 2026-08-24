#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gameplay_test
{

void TestRelicDataIsCookedFromJson()
{
    const auto &content = DefaultContent();
    const auto &data = content.simulation_rules;
    const auto &blood_and_fire = data.relics.bleed_burn_explosion;
    Check(std::abs(blood_and_fire.radius - 5.0f) < 0.0001f &&
              std::abs(blood_and_fire.damage_multiplier - 2.0f) < 0.0001f &&
              blood_and_fire.per_target_cooldown_ticks == 120,
          "blood and fire uses cooked JSON values");

    const auto &counter = data.relics.damage_knockback;
    Check(std::abs(counter.radius - 4.0f) < 0.0001f &&
              std::abs(counter.push_distance - 3.0f) < 0.0001f &&
              std::abs(counter.slow_fraction - 0.5f) < 0.0001f &&
              counter.slow_duration_ticks == 120 && counter.cooldown_ticks == 6,
          "damage counter uses cooked JSON values");
    Check(std::string_view(content.presentation.relic_names[3].data()) == "피와 불" &&
              std::string_view(content.presentation.relic_names[9].data()) == "충격 반격" &&
              !std::string_view(content.presentation.relic_rules[3].data()).empty() &&
              !std::string_view(content.presentation.relic_rules[9].data()).empty(),
          "relic UI text is cooked from JSON");
}

void TestTypedSimulationRulesAreCookedFromJson()
{
    const auto &data = DefaultContent().simulation_rules;
    const auto &split = data.upgrades.charged_shot.boss_or_fifth_pierce_split;
    constexpr std::array expected_angles{-35.0f, -25.0f, -15.0f, -5.0f,
                                          5.0f,  15.0f,  25.0f, 35.0f};

    Check(std::abs(data.upgrades.piercing_shot.align_hit_normal_enemy.move_distance -
                   3.0f) < 0.0001f,
          "upgrade scalar is cooked from JSON");
    Check(data.upgrades.charged_shot.extended_full_charge_explosion
              .maximum_charge_time_ticks == 84,
          "upgrade seconds are cooked to ticks");
    Check(split.angles_degrees == expected_angles && split.projectile_count == 8,
          "upgrade fixed array is cooked from JSON");
    Check(std::abs(data.enemies[1].ranged_projectile_radius - 0.25f) < 0.0001f,
          "enemy parameter is cooked from JSON");
    Check(std::abs(data.stats.allocations[1].amount_per_point - 0.1f) < 0.0001f &&
              data.progression.required_xp_base == 12.0f &&
              data.progression.boss_spawn_ticks[2] == 54'000,
          "stat and progression parameters are cooked from JSON");
    Check(data.relic_drop.maximum_choices_per_box == 3 &&
              std::abs(data.relic_drop.healing_pickup_probability - 0.01f) <
                  0.0001f,
          "relic top-level parameters are cooked from JSON");
}

void TestGameplayDataHotReloadBoundary()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({17, false, true}, QuietGameData()).Succeeded(),
          "Hot Reload initialize");
    auto replacement = QuietGameData();
    replacement.stats.base_maximum_hp = 177.0f;
    replacement.stats.base_current_hp = 177.0f;
    Check(simulation.ApplySimulationRules(replacement).Succeeded(),
          "Hot Reload applies at main menu boundary");

    hs::HeldInputState held;
    SetCursor(held, 960, 316);
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing &&
              simulation.GetObservation().health == 177,
          "next session uses immutable replacement data");
    Check(!simulation.ApplySimulationRules(QuietGameData()),
          "Hot Reload rejects active gameplay");
    Check(simulation.Shutdown().Succeeded(), "Hot Reload shutdown");
}

void RunGameplayContentTests()
{
    TestRelicDataIsCookedFromJson();
    TestTypedSimulationRulesAreCookedFromJson();
    TestGameplayDataHotReloadBoundary();
}

} // namespace gameplay_test
