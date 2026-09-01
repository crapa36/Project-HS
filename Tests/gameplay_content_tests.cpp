#include "gameplay_test_support.hpp"

#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <filesystem>
#include <fstream>
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

    Check(std::abs(data.relics.bleed_kill_heal.maximum_hp_heal_fraction - 0.05f) <
                  0.0001f &&
              std::abs(data.relics.burn_propagation.copied_burn_strength - 0.50f) <
                  0.0001f &&
              data.relics.kill_cooldown_surge.cooldown_reduction_ticks == 120 &&
              data.relics.radial_basic_attack.cadence_interval == 6 &&
              std::abs(data.relics.radial_basic_attack.damage_multiplier - 0.15f) <
                  0.0001f &&
              std::abs(data.relics.basic_kill_tracker.damage_multiplier - 0.20f) <
                  0.0001f &&
              std::abs(data.relics.alternating_skills.cooldown_refund_fraction - 0.20f) <
                  0.0001f,
          "tuned original relic values are cooked from JSON");

    const auto &counter = data.relics.damage_knockback;
    Check(std::abs(counter.radius - 4.0f) < 0.0001f &&
              std::abs(counter.push_distance - 0.25f) < 0.0001f &&
              std::abs(counter.slow_fraction - 0.5f) < 0.0001f &&
              counter.slow_duration_ticks == 120 && counter.cooldown_ticks == 60,
          "damage counter uses cooked JSON values");
    Check(std::string_view(content.presentation.relic_names[3].data()) == "피와 불" &&
              std::string_view(content.presentation.relic_names[9].data()) == "충격 반격" &&
              !std::string_view(content.presentation.relic_rules[3].data()).empty() &&
              !std::string_view(content.presentation.relic_rules[9].data()).empty(),
          "relic UI text is cooked from JSON");
    Check(data.relics.projectile_cadence_reward.hits_per_trigger == 8 &&
              data.relics.projectile_cadence_reward.cooldown_reduction_ticks == 45 &&
              std::abs(data.relics.pre_damage_guard.damage_reduction_fraction - 0.20f) < 0.0001f &&
              data.relics.pre_damage_guard.cooldown_ticks == 360 &&
              std::abs(data.relics.slow_synergy.damage_multiplier - 0.25f) < 0.0001f &&
              data.relics.slow_synergy.per_target_cooldown_ticks == 30 &&
              std::abs(data.relics.area_resonance.damage_multiplier - 0.60f) < 0.0001f &&
              data.relics.area_resonance.cooldown_ticks == 60 &&
              std::abs(data.relics.boss_pressure.damage_multiplier - 0.25f) < 0.0001f &&
              data.relics.boss_pressure.per_target_cooldown_ticks == 60 &&
              data.relics.hit_streak_reward.direct_hits_per_trigger == 10 &&
              std::abs(data.relics.hit_streak_reward.damage_multiplier - 1.50f) < 0.0001f &&
              std::abs(data.relics.pickup_reward.attack_power_fraction - 0.10f) < 0.0001f &&
              data.relics.pickup_reward.duration_ticks == 90 &&
              std::abs(data.relics.low_health_survival.health_threshold_fraction - 0.35f) < 0.0001f &&
              std::abs(data.relics.low_health_survival.damage_reduction_fraction - 0.40f) < 0.0001f &&
              data.relics.low_health_survival.cooldown_ticks == 480,
          "eight appended relic contracts are cooked from JSON");

    std::ifstream slime(std::filesystem::current_path() / "Cooked" /
                            "enemy_melee.meshbin",
                        std::ios::binary);
    hs::CharacterAssetHeader slime_header;
    slime.read(reinterpret_cast<char *>(&slime_header), sizeof(slime_header));
    Check(slime && slime_header.vertex_count == 3'051,
          "Slime cook seals only the ten-edge eye socket with eight triangles");
}

void TestTypedSimulationRulesAreCookedFromJson()
{
    const auto &content = DefaultContent();
    const auto &data = content.simulation_rules;
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
    const auto &spawn = data.spawn_placement;
    Check(spawn.require_outside_max_zoom_view && spawn.fallback_warning_ticks == 30 &&
              spawn.max_zoom_view_min_forward_m < 0.0f &&
              spawn.max_zoom_view_max_forward_m > 0.0f &&
              spawn.max_zoom_view_half_right_m > 0.0f &&
              std::abs(spawn.max_zoom_view_forward_x - 0.70710678f) < 0.0001f &&
              std::abs(spawn.max_zoom_view_forward_z - 0.70710678f) < 0.0001f,
          "spawn visibility bounds are derived from the authored maximum camera view");
    Check(content.presentation.camera.yaw_degrees == 45.0f &&
              content.presentation.camera.pitch_degrees == 55.0f &&
              content.presentation.camera.vertical_fov_degrees == 45.0f &&
              content.presentation.camera.distance_m == 28.0f &&
              content.presentation.camera.minimum_distance_percent == 80 &&
              content.presentation.camera.maximum_distance_percent == 120,
          "runtime camera settings are cooked from the same character authoring source");
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
