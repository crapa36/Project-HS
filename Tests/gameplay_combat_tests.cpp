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

void TestCombatVfxCoverage();
void TestAttackStopsMovementAndFacesAim();
void TestBasicAttackStopsAtFirstEnemy();
void TestQwerInputBuffer();
void TestSkillMovementPauseAndResume();
void TestQwerSkills();
void TestChargedShotDamageFormula();
void TestChargedShotCancelsForLevelSelection();
void TestCombatPresentationContracts();
void TestAttackSpeedAnimationRate();
void TestArrowRainPulseAudioProjection();
void TestEnemyDisplacementInterpolates();
void TestArrowRainTrackingProjectileMoves();
void TestProjectileAndAreaVisualTruth();
void TestMultiShotCastVfxIsPerFan();
void TestPiercingDamageTrailMatchesArrowPath();
void TestTrapRollsForwardAndLeavesOriginTrap();
void TestTenMinuteBossApproachesAttackRange();
void TestTenMinuteBossGroundAreasStaySeparated();
void TestRangedWarningAndExplosiveArea();
void TestCombatUpgradeCombinations();
void TestUpgradeDamageAttribution();
void TestSelectiveUpgradeInheritance();
void TestHighFanoutChainsTerminate();
void TestRelicCombinations();
void TestAlternatingSkillRelicTelemetry();
void TestRemadeRelics();
void TestIncomingDamageRelics();
void TestSameTickLethalDamageAttribution();

void TestCombatVfxCoverage()
{
    auto data = QuietGameData();
    data.enemies[0].move_speed = 0.0f;
    hs::GameSimulation enemy;
    Check(enemy.Initialize({0x564658u}, data).Succeeded(), "combat VFX initialize");
    Debug(enemy, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0, {0.5f, 0.0f});
    for (std::uint32_t tick = 0; tick < 30; ++tick) (void)Tick(enemy);
    Check(HasVfx(enemy, hs::DomainSignalKind::MeleeEnemyWindup) &&
              HasVfx(enemy, hs::DomainSignalKind::MeleeEnemyHit) &&
              HasVfx(enemy, hs::DomainSignalKind::PlayerDamaged),
          "melee windup, melee hit, and player hit effects are connected");
    Debug(enemy, hs::DebugCommandKind::DamagePlayer, 1'000);
    (void)Tick(enemy);
    Check(HasVfx(enemy, hs::DomainSignalKind::PlayerDied),
          "fatal damage emits the player death effect");
    Check(enemy.Shutdown().Succeeded(), "combat VFX shutdown");

    const auto emitted_during = [](hs::GameSimulation &simulation,
                                   hs::DomainSignalKind kind,
                                   std::uint32_t ticks) {
        bool emitted{};
        for (std::uint32_t tick = 0; tick < ticks; ++tick)
        {
            (void)Tick(simulation);
            emitted |= HasVfx(simulation, kind);
        }
        return emitted;
    };
    hs::GameSimulation ranged;
    Check(ranged.Initialize({0x52414E474544u}, data).Succeeded(),
          "ranged attack VFX initialize");
    Debug(ranged, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {5.0f, 0.0f});
    Check(emitted_during(ranged, hs::DomainSignalKind::RangedEnemyReleased, 90),
          "ranged enemy release effect is connected");
    Check(ranged.Shutdown().Succeeded(), "ranged attack VFX shutdown");

    hs::GameSimulation suicide;
    Check(suicide.Initialize({0x53554943494445u}, data).Succeeded(),
          "suicide attack VFX initialize");
    Debug(suicide, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Suicide), 0, {1.0f, 0.0f});
    bool charged{};
    bool exploded{};
    for (std::uint32_t tick = 0; tick < 90; ++tick)
    {
        (void)Tick(suicide);
        charged |= HasVfx(suicide, hs::DomainSignalKind::SuicideEnemyCharging);
        exploded |= HasVfx(suicide, hs::DomainSignalKind::SuicideEnemyExploded);
    }
    Check(charged && exploded, "suicide charge and explosion effects are connected");
    Check(suicide.Shutdown().Succeeded(), "suicide attack VFX shutdown");

    hs::SimulationConfig config{0x424F5353u};
    config.scenario = {.player_stationary = true, .player_invulnerable = true,
                       .progression_enabled = false};
    hs::GameSimulation bosses;
    Check(bosses.Initialize(config, QuietGameData()).Succeeded(),
          "boss VFX initialize");
    Debug(bosses, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::FiveMinute));
    Debug(bosses, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    for (std::uint32_t tick = 0; tick < 2'400; ++tick) (void)Tick(bosses);
    Check(HasVfx(bosses, hs::DomainSignalKind::BossDashStarted) &&
              HasVfx(bosses, hs::DomainSignalKind::BossDashImpact) &&
              HasVfx(bosses, hs::DomainSignalKind::BossVolleyReleased) &&
              HasVfx(bosses, hs::DomainSignalKind::BossAreaActivated) &&
              HasVfx(bosses, hs::DomainSignalKind::BossShockwaveReleased),
          "every boss attack family emits its dedicated effect");
    Check(bosses.Shutdown().Succeeded(), "boss VFX shutdown");
}

void TestAttackStopsMovementAndFacesAim()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({14}, QuietGameData()).Succeeded(),
          "attack movement initialize");

    hs::HeldInputState held;
    held.move_held = true;
    held.move_target_world = {20.0f, 0.0f, 0.0f};
    held.aim_world = {0.0f, 0.0f, 20.0f};
    (void)Tick(simulation, held);
    for (std::uint32_t tick = 0; tick < 9; ++tick) (void)Tick(simulation, held);
    const auto moving = simulation.GetObservation();
    Check(moving.player_position.x > 0.0f &&
              moving.facing_direction.x > 0.99f,
          "RMB movement faces movement direction");

    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    const auto attacking = simulation.GetObservation();
    Check(std::abs(attacking.player_position.x - moving.player_position.x) < 0.0001f &&
              attacking.facing_direction.y > 0.99f &&
              attacking.player_projectile_count == 0,
          "LMB attack stops movement, faces cursor, and waits for release marker");

    hs::RenderSnapshotStorage turn_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, turn_snapshot), "facing snapshot");
    Check(!turn_snapshot.View().poses.empty() &&
              turn_snapshot.View().poses.front().clip ==
                  hs::CharacterAnimationClip::Idle,
          "attack direction change does not select a turn animation");
    Check(!turn_snapshot.View().instances.empty() &&
              std::abs(turn_snapshot.View().instances.front().yaw -
                       std::atan2(attacking.facing_direction.x,
                                  attacking.facing_direction.y)) < 0.0001f,
          "render facing follows gameplay facing without turn lag");

    for (std::uint32_t tick = 0; tick < 24; ++tick) (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_projectile_count == 1,
          "basic arrow appears on the animation release marker");
    hs::RenderSnapshotStorage projectile_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, projectile_snapshot),
          "first basic projectile snapshot");
    hs::GameReadModelStorage projectile_model;
    simulation.WriteReadModel(projectile_model);
    const auto projectile_radius = projectile_model.View().projectiles.front().radius;
    Check(std::abs(projectile_radius - 0.36f) < 0.0001f,
          "basic arrow uses the migrated final collision radius");
    const auto projectile = std::ranges::find_if(
        projectile_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::PlayerProjectile;
        });
    Check(projectile != projectile_snapshot.View().instances.end() &&
              std::abs(projectile->scale.x - 0.24f) < 0.0001f &&
              std::abs(projectile->scale.y - 0.24f) < 0.0001f &&
              std::abs(projectile->position.x -
                       projectile_model.View().projectiles.front().position.x) < 0.0001f &&
              std::abs(projectile->position.z -
                       projectile_model.View().projectiles.front().position.y) < 0.0001f,
          "basic arrow keeps its compact visual size and collision center");
    Check(std::ranges::any_of(
              projectile_snapshot.View().persistent_vfx,
              [projectile_radius](const hs::PersistentVfxVisual &visual) {
                  return visual.kind == hs::PersistentVfxKind::ProjectileTrail &&
                         visual.radius < projectile_radius && visual.radius >= 0.05f &&
                         visual.length <= 0.45f &&
                         std::abs(visual.position.y - 1.17f) < 0.0001f;
              }),
          "basic arrow trail stays visible at the GPU arrow body's center height");

    held.basic_attack_held = false;
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_position.x > attacking.player_position.x,
          "held RMB reissues movement after the basic attack ends");
    Check(simulation.Shutdown().Succeeded(), "attack movement shutdown");
}

void TestBasicAttackStopsAtFirstEnemy()
{
    auto data = QuietGameData();
    data.enemies[0].health = 100;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({142}, data).Succeeded(), "basic pierce initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {7.0f, 0.0f});
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    held.basic_attack_held = false;
    for (std::uint32_t tick = 0; tick < 30; ++tick) (void)Tick(simulation, held);
    Check(simulation.GetObservation().damage_by_skill[0] == 10,
          "basic arrow damages only the first enemy without an upgrade");
    Check(simulation.Shutdown().Succeeded(), "basic pierce shutdown");
}

void TestQwerInputBuffer()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({15}, QuietGameData()).Succeeded(),
          "input buffer initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    while (simulation.GetObservation().cooldown_ticks[0] > 9)
    {
        (void)Tick(simulation, held);
    }
    Check(simulation.GetObservation().cooldown_ticks[0] == 9,
          "skill reaches input buffer window");

    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[0] == 8,
          "early Q is buffered instead of cast immediately");
    bool buffered_cast{};
    for (std::uint32_t attempt = 0; attempt < 9; ++attempt)
    {
        (void)Tick(simulation, held);
        if (simulation.GetObservation().cooldown_ticks[0] > 9)
        {
            buffered_cast = true;
            break;
        }
    }
    Check(buffered_cast, "buffered Q casts when cooldown becomes ready");
    Check(simulation.Shutdown().Succeeded(), "input buffer shutdown");
}

void RunGameplayCombatTests()
{
    TestCombatVfxCoverage();
    TestAttackStopsMovementAndFacesAim();
    TestSkillMovementPauseAndResume();
    TestBasicAttackStopsAtFirstEnemy();
    TestTenMinuteBossApproachesAttackRange();
    TestTenMinuteBossGroundAreasStaySeparated();
    TestQwerInputBuffer();
    TestQwerSkills();
    TestChargedShotDamageFormula();
    TestEnemyDisplacementInterpolates();
    TestChargedShotCancelsForLevelSelection();
    TestCombatPresentationContracts();
    TestArrowRainPulseAudioProjection();
    TestAttackSpeedAnimationRate();
    TestArrowRainTrackingProjectileMoves();
    TestProjectileAndAreaVisualTruth();
    TestMultiShotCastVfxIsPerFan();
    TestPiercingDamageTrailMatchesArrowPath();
    TestRangedWarningAndExplosiveArea();
    TestTrapRollsForwardAndLeavesOriginTrap();
    TestCombatUpgradeCombinations();
    TestRelicCombinations();
    TestAlternatingSkillRelicTelemetry();
    TestRemadeRelics();
    TestIncomingDamageRelics();
    TestSameTickLethalDamageAttribution();
    TestUpgradeDamageAttribution();
    TestSelectiveUpgradeInheritance();
    TestHighFanoutChainsTerminate();
}

} // namespace gameplay_test
