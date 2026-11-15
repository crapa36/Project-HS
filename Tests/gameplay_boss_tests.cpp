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

void TestBossAreaUsesAuthoredTickInterval()
{
    auto data = QuietGameData();
    data.arena_obstacle_count = 0;
    data.stats.base_maximum_hp = 1'000'000.0f;
    data.stats.base_current_hp = 1'000'000.0f;
    data.boss_common.initial_pattern_delay_ticks = 1;
    auto &boss = data.bosses[static_cast<std::size_t>(hs::BossKind::FiveMinute)];
    boss.movement_speed = 0.0f;
    boss.recovery_ticks = 1'000;
    boss.preferred_distance_near = 100.0f;
    boss.preferred_distance_far = 200.0f;
    boss.pattern_count = 1;
    boss.patterns[0] = {};
    boss.patterns[0].logic = hs::BossPatternLogic::PredictedPositionGroundAreas;
    boss.patterns[0].phase = 1;
    boss.patterns[0].telegraph_duration_ticks = 1;
    boss.patterns[0].area_count = 1;
    boss.patterns[0].radius = 8.0f;
    boss.patterns[0].duration_ticks = 23;
    boss.patterns[0].tick_interval_ticks = 7;
    boss.patterns[0].damage_per_tick = 1;

    hs::SimulationConfig config{0xA8EAu};
    config.scenario = {.player_stationary = true,
                       .player_invulnerable = false,
                       .progression_enabled = false};
    hs::GameSimulation simulation;
    Check(simulation.Initialize(config, data).Succeeded(),
          "custom boss area interval initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::FiveMinute));

    hs::EntityId area_id{};
    std::vector<std::pair<hs::Sequence, hs::Tick>> area_ticks;
    std::vector<hs::Sequence> seen_sequences;
    const auto collect_area_ticks = [&] {
        for (const auto &signal : simulation.PendingDomainSignals())
        {
            if (signal.kind != hs::DomainSignalKind::BossAreaTicked ||
                signal.geometry.source_id == 0 ||
                std::ranges::find(seen_sequences, signal.sequence) != seen_sequences.end())
                continue;
            if (area_id.value == 0) area_id = {signal.geometry.source_id};
            if (signal.geometry.source_id != area_id.value) continue;
            seen_sequences.push_back(signal.sequence);
            area_ticks.emplace_back(signal.sequence, signal.geometry.start_tick);
        }
    };
    for (std::uint32_t tick = 0; tick < 2'000 && area_ticks.empty(); ++tick)
    {
        (void)Tick(simulation);
        collect_area_ticks();
    }
    Check(area_id.value != 0 && !area_ticks.empty(),
          "custom boss area emits its first tick");
    for (std::uint32_t tick = 0; tick < 26; ++tick)
    {
        (void)Tick(simulation);
        collect_area_ticks();
    }
    Check(area_ticks.size() == 4,
          "boss area ticks throughout its authored duration then exits");
    for (std::size_t index = 1; index < area_ticks.size(); ++index)
        Check(area_ticks[index].second - area_ticks[index - 1].second == 7,
              "boss area uses its nondefault seven-tick interval");
    Check(simulation.GetObservation().enemy_area_count == 0,
          "boss area is removed after its authored duration");
    Check(simulation.Shutdown().Succeeded(), "custom boss area interval shutdown");
}

void TestRangedWarningAndExplosiveArea()
{
    auto ranged_data = QuietGameData();
    auto &ranged_rule = ranged_data.enemies[static_cast<std::size_t>(hs::EnemyKind::Ranged)];
    ranged_rule.warning_ticks = 47;
    ranged_rule.projectile_range = 31.0f;
    ranged_rule.ranged_projectile_radius = 0.37f;
    hs::GameSimulation warning_simulation;
    Check(warning_simulation.Initialize({20}, ranged_data).Succeeded(),
          "ranged warning initialize");
    Debug(warning_simulation, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {10.0f, 0.0f});
    (void)Tick(warning_simulation);
    const auto warning_signal = std::ranges::find_if(warning_simulation.PendingDomainSignals(),
        [](const auto &signal) { return signal.kind == hs::DomainSignalKind::RangedEnemyTelegraphed; });
    Check(warning_signal != warning_simulation.PendingDomainSignals().end() &&
          warning_signal->geometry.kind == hs::DomainSignalGeometryKind::Line &&
          warning_signal->geometry.source_id == warning_signal->sequence &&
          warning_signal->geometry.end_tick - warning_signal->geometry.start_tick == 47 &&
          warning_signal->geometry.range == 31.0f &&
          std::abs(warning_signal->geometry.width - 0.74f) < 0.0001f &&
          std::abs(warning_signal->geometry.end_position.x + 21.0f) < 0.0001f,
          "ranged telegraph carries its scheduled lifetime, projectile width and range");
    hs::RenderSnapshotStorage warning_snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(warning_simulation, warning_snapshot),
          "ranged warning snapshot");
    hs::GameReadModelStorage ranged_model;
    hs::CopyReadModelForDevelopment(warning_simulation, ranged_model);
    Check(ranged_model.View().enemies.size() == 1 &&
          ranged_model.View().enemies[0].warning_sequence != 0 &&
          ranged_model.View().enemies[0].warning_half_width == 0.37f,
          "ranged warning owner is copied to read model");
    for (std::uint32_t step = 0; step < 47; ++step)
        (void)Tick(warning_simulation);
    warning_snapshot.Clear();
    Check(WriteSnapshot(warning_simulation, warning_snapshot) &&
          std::ranges::none_of(warning_snapshot.View().persistent_vfx, [](const auto &visual) {
              return visual.kind == hs::PersistentVfxKind::RangedEnemyWarning;
          }), "ranged warning owner disappears on projectile release");
    for (std::uint32_t step = 47; step < 90; ++step)
        (void)Tick(warning_simulation);
    warning_snapshot.Clear();
    Check(WriteSnapshot(warning_simulation, warning_snapshot),
          "ranged hold position snapshot");
    const auto ranged = std::ranges::find_if(
        warning_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::MonsterRanged;
        });
    Check(ranged != warning_snapshot.View().instances.end() &&
              std::abs(ranged->position.x - 10.0f) < 0.0001f &&
              std::abs(ranged->position.z) < 0.0001f,
          "ranged enemy holds position inside attack range instead of retreating");
    Check(warning_simulation.Shutdown().Succeeded(), "ranged warning shutdown");

    hs::GameReadModelStorage displaced;
    displaced.tick = 30;
    hs::EnemyView warning_enemy{};
    warning_enemy.id = {71}; warning_enemy.kind = hs::EnemyKind::Ranged;
    warning_enemy.position = {8.0f, 3.0f}; warning_enemy.locked_aim = {-1.0f, 0.0f};
    warning_enemy.attacking = true; warning_enemy.attack_started = 20; warning_enemy.attack_resolve = 67;
    warning_enemy.warning_extent = 31.0f; warning_enemy.warning_half_width = 0.37f;
    warning_enemy.warning_sequence = 901;
    displaced.AddEnemy(warning_enemy);
    warning_enemy.id = {72}; warning_enemy.position = {2.0f, 9.0f}; warning_enemy.warning_sequence = 902;
    displaced.AddEnemy(warning_enemy);
    warning_snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(displaced.View(), DefaultContent().presentation,
          hs::PresentationUiState{}, hs::SettingsData{}, warning_snapshot), "displaced ranged warning projection");
    const auto first_warning = std::ranges::find_if(warning_snapshot.View().persistent_vfx,
        [](const auto &v) { return v.stable_id == 901; });
    Check(first_warning != warning_snapshot.View().persistent_vfx.end() &&
          first_warning->kind == hs::PersistentVfxKind::RangedEnemyWarning &&
          std::abs(first_warning->position.x + 7.5f) < 0.0001f && first_warning->position.z == 3.0f &&
          first_warning->active_tick == 20 && first_warning->expires == 67 &&
          first_warning->radius == 0.37f && first_warning->length == 31.0f,
          "ranged warning follows displaced origin while preserving locked aim and schedule");
    Check(std::ranges::count_if(warning_snapshot.View().persistent_vfx, [](const auto &v) {
              return v.kind == hs::PersistentVfxKind::RangedEnemyWarning;
          }) == 2 && std::ranges::count_if(warning_snapshot.View().instances, [](const auto &v) {
              return v.mesh == hs::RenderMesh::Area && v.stable_id == 0 &&
                     (v.vfx_owner_id == 901 || v.vfx_owner_id == 902);
          }) == 2, "ranged warnings have distinct owners without shared interpolation identities");

    hs::GameSimulation suicide_warning;
    Check(suicide_warning.Initialize({201}, QuietGameData()).Succeeded(),
          "suicide warning initialize");
    Debug(suicide_warning, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Suicide), 0, {2.0f, 0.0f});
    (void)Tick(suicide_warning);
    hs::RenderSnapshotStorage suicide_snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(suicide_warning, suicide_snapshot),
          "suicide warning owner snapshot");
    const auto charge = std::ranges::find_if(suicide_warning.PendingDomainSignals(), [](const auto &signal) {
        return signal.kind == hs::DomainSignalKind::SuicideEnemyCharging;
    });
    Check(charge != suicide_warning.PendingDomainSignals().end() &&
        charge->geometry.kind == hs::DomainSignalGeometryKind::Circle && charge->geometry.radius == 3.0f &&
        charge->geometry.source_id == charge->sequence && charge->geometry.end_tick > charge->geometry.start_tick,
        "suicide charge carries actual explosion radius and attack schedule");
    Check(std::ranges::any_of(suicide_snapshot.View().persistent_vfx, [&](const auto &visual) {
        return visual.kind == hs::PersistentVfxKind::SuicideEnemyWarning && charge != suicide_warning.PendingDomainSignals().end() &&
            visual.stable_id == charge->sequence && visual.radius == 3.0f && visual.expires == charge->geometry.end_tick;
    }), "suicide attack publishes matching warning owner");
    Check(suicide_warning.Shutdown().Succeeded(), "suicide warning shutdown");

    hs::GameSimulation explosion_simulation;
    Check(explosion_simulation.Initialize({21}, QuietGameData()).Succeeded(),
          "explosive area initialize");
    Debug(explosion_simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow));
    Debug(explosion_simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow), 3);
    Debug(explosion_simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    Debug(explosion_simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 1.5f});
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(explosion_simulation, hs::GameAction::SkillQ,
                   hs::EdgeKind::Pressed, sequence, held);
    for (std::uint32_t tick = 0; tick < 30 && explosion_simulation.GetObservation().kills < 2; ++tick)
    {
        (void)Tick(explosion_simulation, held);
    }
    Check(explosion_simulation.GetObservation().kills == 2,
          "explosive arrow impact damages every enemy in its radius");
    const auto combat_stats = explosion_simulation.GetObservation();
    Check(combat_stats.damage_by_skill[
              static_cast<std::size_t>(hs::SkillKind::ExplosiveArrow)] ==
              combat_stats.damage_dealt && combat_stats.damage_dealt > 0,
          "damage statistics attribute applied damage to the source skill");
    for (std::uint32_t tick = 0; tick < 60; ++tick)
        (void)Tick(explosion_simulation);
    Check(explosion_simulation.GetObservation().pickup_count == 2,
          "experience pickups remain separate in the same spatial cell");
    Check(explosion_simulation.Shutdown().Succeeded(), "explosive area shutdown");
}

} // namespace gameplay_test
