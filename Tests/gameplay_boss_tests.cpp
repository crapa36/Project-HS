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

namespace
{
} // namespace

void TestTenMinuteBossApproachesAttackRange()
{
    auto data = QuietGameData();
    data.stats.base_maximum_hp = 10'000.0f;
    data.stats.base_current_hp = 10'000.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({143}, data).Succeeded(),
          "ten minute boss approach initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    hs::RenderSnapshotStorage before_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, before_snapshot), "boss before snapshot");
    const auto before = std::ranges::find_if(
        before_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::BossTenMinute;
        });
    Check(before != before_snapshot.View().instances.end(), "ten minute boss visible");
    const auto before_distance = std::hypot(before->position.x, before->position.z);
    for (std::uint32_t tick = 0; tick < 151; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage after_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, after_snapshot), "boss after snapshot");
    const auto after = std::ranges::find_if(
        after_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::BossTenMinute;
        });
    Check(after != after_snapshot.View().instances.end() &&
              std::hypot(after->position.x, after->position.z) < before_distance - 4.0f,
          "ten minute boss uses doubled movement speed toward its 18 meter attack range");
    for (std::uint32_t tick = 151;
         tick < 1200 && simulation.GetObservation().boss_warning_count == 0; ++tick)
        (void)Tick(simulation);
    Check(simulation.GetObservation().boss_warning_count > 0,
          "ten minute boss starts a skill warning");
    hs::RenderSnapshotStorage warning_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, warning_snapshot), "boss warning snapshot");
    const auto warning = std::ranges::find_if(
        warning_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::BossTenMinute;
        });
    Check(warning != warning_snapshot.View().instances.end(),
          "ten minute boss visible during warning");
    const auto warning_position = warning->position;
    for (std::uint32_t tick = 0; tick < 20; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage casting_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, casting_snapshot), "boss casting snapshot");
    const auto casting = std::ranges::find_if(
        casting_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::BossTenMinute;
        });
    Check(casting != casting_snapshot.View().instances.end() &&
              std::hypot(casting->position.x - warning_position.x,
                         casting->position.z - warning_position.z) < 0.001f,
          "ten minute boss remains stationary while warning and casting");
    for (std::uint32_t tick = 0;
         tick < 1200 && simulation.GetObservation().balance.enemy_attack_attempts[4] == 0; ++tick)
        (void)Tick(simulation);
    Check(simulation.GetObservation().balance.enemy_attack_attempts[4] > 0,
          "ten minute boss begins ranged attacks from the expanded 18 meter range");
    Check(simulation.Shutdown().Succeeded(), "ten minute boss approach shutdown");
}

void TestTenMinuteBossGroundAreasStaySeparated()
{
    auto data = QuietGameData();
    data.stats.base_maximum_hp = 1'000'000.0f;
    data.stats.base_current_hp = 1'000'000.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({144}, data).Succeeded(),
          "ten minute ground area initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    for (std::uint32_t tick = 0;
         tick < 20'000 && simulation.GetObservation().enemy_area_count < 3; ++tick)
        (void)Tick(simulation);
    Check(simulation.GetObservation().enemy_area_count == 3,
          "ten minute boss creates three ground areas");

    hs::RenderSnapshotStorage snapshot(64, 2, 2, 8);
    Check(WriteSnapshot(simulation, snapshot), "ten minute ground area snapshot");
    std::vector<hs::Float3> centers;
    for (const auto &instance : snapshot.View().instances)
        if (instance.mesh == hs::RenderMesh::Area && instance.color_rgba == 0x604040FFu &&
            std::abs(instance.scale.x - 2.2f) < 0.0001f)
            centers.push_back(instance.position);
    Check(centers.size() == 3, "three ten minute ground areas are visible");
    for (std::size_t left = 0; left < centers.size(); ++left)
        for (std::size_t right = left + 1; right < centers.size(); ++right)
            Check(std::hypot(centers[left].x - centers[right].x,
                             centers[left].z - centers[right].z) >= 2.2f,
                  "ten minute ground area overlap stays below fifty percent");
    Check(simulation.Shutdown().Succeeded(), "ten minute ground area shutdown");
}

void TestRangedWarningAndExplosiveArea()
{
    hs::GameSimulation warning_simulation;
    Check(warning_simulation.Initialize({20}, QuietGameData()).Succeeded(),
          "ranged warning initialize");
    Debug(warning_simulation, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {10.0f, 0.0f});
    (void)Tick(warning_simulation);
    hs::RenderSnapshotStorage warning_snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(warning_simulation, warning_snapshot),
          "ranged warning snapshot");
    Check(std::ranges::any_of(
              warning_snapshot.View().instances, [](const hs::RenderInstance &instance) {
                  return instance.mesh == hs::RenderMesh::Area &&
                         instance.color_rgba == 0xA03030FFu && instance.scale.z >= 18.0f;
              }), "ranged enemy renders a red pre-attack range line");
    for (std::uint32_t tick = 0; tick < 90; ++tick)
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

    hs::GameSimulation suicide_warning;
    Check(suicide_warning.Initialize({201}, QuietGameData()).Succeeded(),
          "suicide warning initialize");
    Debug(suicide_warning, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Suicide), 0, {2.0f, 0.0f});
    (void)Tick(suicide_warning);
    hs::RenderSnapshotStorage suicide_snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(suicide_warning, suicide_snapshot),
          "suicide warning snapshot");
    Check(std::ranges::any_of(
              suicide_snapshot.View().instances, [](const hs::RenderInstance &instance) {
                  return instance.mesh == hs::RenderMesh::Area &&
                         instance.color_rgba == 0x803030FFu &&
                         std::abs(instance.scale.x - 3.0f) < 0.0001f &&
                         std::abs(instance.scale.z - 3.0f) < 0.0001f;
              }), "suicide enemy renders its red explosion radius while arming");
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
    hs::RenderSnapshotStorage fire_snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(explosion_simulation, fire_snapshot),
          "explosive fire area snapshot");
    Check(std::ranges::any_of(
              fire_snapshot.View().persistent_vfx, [](const auto &visual) {
                  return visual.kind == hs::PersistentVfxKind::FireArea;
              }),
          "fire area uses a persistent ground visual distinct from explosion VFX");
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
