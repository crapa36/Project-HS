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
void TestBasicAttackSkillCancellationAndBufferRules();
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

void TestBasicAttackSkillCancellationAndBufferRules()
{
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xCA11u}, QuietGameData()).Succeeded(), "cast cancellation initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
        hs::HeldInputState held; held.aim_world = {20.0f, 0.0f, 0.0f}; held.basic_attack_held = true;
        (void)Tick(simulation, held);
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        held.basic_attack_held = false;
        bool basic_emitted{}, skill_emitted{};
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            (void)Tick(simulation, held);
            hs::GameReadModelStorage model; simulation.WriteReadModel(model);
            for (const auto &projectile : model.View().projectiles)
            {
                basic_emitted |= projectile.skill == hs::SkillKind::BasicAttack;
                skill_emitted |= projectile.skill == hs::SkillKind::PiercingShot;
            }
        }
        Check(!basic_emitted && skill_emitted,
              "successful skill cancels only the pending basic release");
        Check(simulation.Shutdown().Succeeded(), "cast cancellation shutdown");
    }
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xCA12u}, QuietGameData()).Succeeded(), "failed skill preservation initialize");
        hs::HeldInputState held; held.aim_world = {20.0f, 0.0f, 0.0f}; held.basic_attack_held = true;
        (void)Tick(simulation, held);
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        held.basic_attack_held = false;
        bool basic_emitted{};
        for (std::uint32_t i = 0; i < 30; ++i)
        {
            (void)Tick(simulation, held);
            hs::GameReadModelStorage model; simulation.WriteReadModel(model);
            for (const auto &projectile : model.View().projectiles)
                basic_emitted |= projectile.skill == hs::SkillKind::BasicAttack;
        }
        Check(basic_emitted, "failed skill leaves pending basic release intact");
        Check(simulation.Shutdown().Succeeded(), "failed skill preservation shutdown");
    }
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xCA13u}, QuietGameData()).Succeeded(), "tap buffer initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
        hs::HeldInputState held; held.aim_world = {20.0f, 0.0f, 0.0f};
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        while (simulation.GetObservation().cooldown_ticks[0] > 9) (void)Tick(simulation, held);
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        bool recast{};
        for (std::uint32_t i = 0; i < 12; ++i) { (void)Tick(simulation, held); recast |= simulation.GetObservation().cooldown_ticks[0] > 9; }
        Check(recast, "tap release preserves buffered skill intent until recovery");
        Check(simulation.Shutdown().Succeeded(), "tap buffer shutdown");
    }
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

void TestAttacksRespectTerrain()
{
    const auto player_damage = [](bool wall, float enemy_x) {
        auto data = QuietGameData();
        data.arena_obstacle_count = wall ? 1 : 0;
        data.arena_obstacles[0] = {hs::ArenaObstacleKind::Rock, {5.0f, 0.0f}, 1.0f};
        data.enemies[0].health = 1000;
        data.enemies[0].move_speed = 0.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0x0B51u}, data).Succeeded(), "player terrain initialize");
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {enemy_x, 0});
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; held.basic_attack_held = true;
        (void)Tick(simulation, held); held.basic_attack_held = false;
        for (int i = 0; i < 80; ++i) (void)Tick(simulation, held);
        const auto damage = simulation.GetObservation().damage_by_skill[0];
        Check(simulation.Shutdown().Succeeded(), "player terrain shutdown");
        return damage;
    };
    Check(player_damage(true, 8.0f) == 0, "rock blocks player arrow behind it");
    Check(player_damage(false, 8.0f) > 0, "clear control arrow reaches same enemy");
    Check(player_damage(true, 2.0f) > 0, "enemy before rock receives arrow damage");
    const auto incoming_damage = [](hs::EnemyKind kind, bool wall) {
        auto data = QuietGameData();
        data.arena_obstacle_count = wall ? 1 : 0;
        data.arena_obstacles[0] = {hs::ArenaObstacleKind::Tree, {2.5f, 0.0f}, 0.5f};
        auto &enemy = data.enemies[static_cast<std::size_t>(kind)];
        enemy.move_speed = 0.0f;
        enemy.attack_range = 7.0f;
        enemy.projectile_range = 20.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0x0B52u}, data).Succeeded(), "enemy terrain initialize");
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, static_cast<std::uint64_t>(kind), 0, {5, 0});
        const auto health = simulation.GetObservation().health;
        for (int i = 0; i < 180; ++i) (void)Tick(simulation);
        const auto damage = health - simulation.GetObservation().health;
        Check(simulation.Shutdown().Succeeded(), "enemy terrain shutdown");
        return damage;
    };
    for (const auto kind : {hs::EnemyKind::Melee, hs::EnemyKind::Ranged})
    {
        Check(incoming_damage(kind, true) == 0, "tree blocks enemy attack against player");
        Check(incoming_damage(kind, false) > 0, "clear control enemy attack damages player");
    }
}

void TestChargeBufferAndRecovery()
{
    const auto player = [](hs::GameSimulation &simulation) {
        hs::GameReadModelStorage model; simulation.WriteReadModel(model);
        return model.View().player;
    };
    // A charge started during windup replaces the pending basic projectile.
    {
        hs::GameSimulation simulation;
        auto data = QuietGameData(); data.arena_obstacle_count = 0;
        Check(simulation.Initialize({0xC401u}, data).Succeeded(), "charge cancel initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; held.basic_attack_held = true;
        (void)Tick(simulation, held); held.basic_attack_held = false;
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        Check(player(simulation).charging, "charge starts during basic windup");
        bool basic_emitted{};
        for (int i = 0; i < 30; ++i)
        {
            (void)Tick(simulation, held);
            hs::GameReadModelStorage model; simulation.WriteReadModel(model);
            for (const auto &shot : model.View().projectiles)
                basic_emitted |= shot.skill == hs::SkillKind::BasicAttack;
        }
        Check(!basic_emitted, "charge cancels pending basic emission");
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        const auto recovery_end = player(simulation).active_cast_tick;
        Check(!player(simulation).charging && recovery_end > simulation.GetObservation().tick,
              "charge release retains recovery");
        held.basic_attack_held = true;
        const auto uses = simulation.GetObservation().balance.skill_uses[0];
        while (simulation.GetObservation().tick + 1 < recovery_end)
        {
            (void)Tick(simulation, held);
            Check(simulation.GetObservation().balance.skill_uses[0] == uses,
                  "held basic cannot bypass charged shot recovery");
        }
        Check(simulation.Shutdown().Succeeded(), "charge cancel shutdown");
    }
    // Skills cancel basic recovery while preserving the already released arrow.
    for (const auto skill : {hs::SkillKind::PiercingShot, hs::SkillKind::ChargedShot})
    {
        hs::GameSimulation simulation;
        auto data = QuietGameData(); data.arena_obstacle_count = 0;
        Check(simulation.Initialize({0xC403u}, data).Succeeded(), "basic recovery initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(skill));
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; held.basic_attack_held = true;
        (void)Tick(simulation, held); held.basic_attack_held = false;
        bool emitted{};
        for (int i = 0; i < 60 && !emitted; ++i)
        {
            (void)Tick(simulation, held);
            emitted = simulation.GetObservation().player_projectile_count != 0;
        }
        const auto recovery_end = player(simulation).basic_attack_animation_until;
        Check(emitted && simulation.GetObservation().tick + 1 < recovery_end,
              "basic emission precedes recovery boundary");
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        if (skill == hs::SkillKind::ChargedShot)
            Check(player(simulation).charging, "charge cancels basic recovery immediately");
        else
            Check(simulation.GetObservation().balance.skill_uses[static_cast<std::size_t>(skill)] == 1,
                  "instant skill cancels basic recovery immediately");
        Check(player(simulation).basic_attack_animation_until <= simulation.GetObservation().tick,
              "successful skill ends the basic animation");
        hs::GameReadModelStorage model; simulation.WriteReadModel(model);
        bool basic_survives{};
        for (const auto &shot : model.View().projectiles)
            basic_survives |= shot.skill == hs::SkillKind::BasicAttack;
        Check(basic_survives, "recovery cancel preserves the released basic arrow");
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        held.basic_attack_held = true;
        const auto uses = simulation.GetObservation().balance.skill_uses[0];
        while (simulation.GetObservation().tick + 1 < recovery_end)
        {
            (void)Tick(simulation, held);
            Check(simulation.GetObservation().balance.skill_uses[0] == uses,
                  "skill cancel does not reset the basic attack rate limiter");
        }
        Check(simulation.Shutdown().Succeeded(), "basic recovery shutdown");
    }
    // Queue near charge cooldown completion; release either cancels intent or ends started charge.
    for (const bool release_before_ready : {true, false})
    {
        hs::GameSimulation simulation;
        auto data = QuietGameData(); data.arena_obstacle_count = 0;
        Check(simulation.Initialize({0xC402u}, data).Succeeded(), "queued charge initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        const auto cooldown_index = static_cast<std::size_t>(hs::SkillKind::ChargedShot) - 1;
        for (int i = 0; i < 2000 && simulation.GetObservation().cooldown_ticks[cooldown_index] > 5; ++i)
            (void)Tick(simulation, held);
        Check(simulation.GetObservation().cooldown_ticks[cooldown_index] == 5, "charge reaches bounded buffer window");
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        Check(!player(simulation).charging, "queued charge does not bypass cooldown");
        if (release_before_ready)
            (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
        for (int i = 0; i < 8; ++i) (void)Tick(simulation, held);
        Check(player(simulation).charging == !release_before_ready,
              "released queued charge cancels while held queued charge starts");
        if (!release_before_ready)
        {
            (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Released, sequence, held);
            Check(player(simulation).charging, "unrelated slot release cannot end queued charge");
            (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released, sequence, held);
            Check(!player(simulation).charging && simulation.GetObservation().cooldown_ticks[cooldown_index] > 5,
                  "original slot release fires queued charge and applies cooldown");
        }
        Check(simulation.Shutdown().Succeeded(), "queued charge shutdown");
    }
}

void TestExplosionUpgradesRespectTerrain()
{
    const auto run = [](bool wall) {
        auto data = QuietGameData();
        data.arena_obstacle_count = wall ? 1 : 0;
        data.arena_obstacles[0] = {hs::ArenaObstacleKind::Rock, {5, 0}, 1.0f};
        data.enemies[0].health = 10000; data.enemies[0].move_speed = 0.0f;
        data.upgrades.explosive_arrow.apply_bleed_and_blood_explosions.radius = 8.0f;
        data.upgrades.explosive_arrow.pre_explosion_pull.pull_radius = 8.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xEB10u}, data).Succeeded(), "explosion terrain initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow));
        for (const auto upgrade : {2u, 5u, 6u})
            Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
                  static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow), upgrade);
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {7, 0});
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        for (int i = 0; i < 180; ++i) (void)Tick(simulation, held);
        hs::GameReadModelStorage model; simulation.WriteReadModel(model);
        Check(model.View().enemies.size() == 1, "explosion test target survives");
        if (wall && !model.View().enemies.empty())
        {
            const auto &enemy = model.View().enemies.front();
            Check(enemy.health == 10000 && enemy.status_flags == 0,
                  "blocked blood and satellite explosions cannot damage or bleed target");
            Check(std::abs(enemy.position.x - 7.0f) < 0.0001f && std::abs(enemy.position.y) < 0.0001f,
                  "blocked explosion pull cannot displace target");
        }
        const auto damage = simulation.GetObservation().damage_by_skill[static_cast<std::size_t>(hs::SkillKind::ExplosiveArrow)];
        Check(simulation.Shutdown().Succeeded(), "explosion terrain shutdown");
        return damage;
    };
    Check(run(true) == 0, "all explosion derivatives respect terrain");
    Check(run(false) > 0, "explosion derivative clear control deals damage");
}

void TestSuccessfulInputReplacesOldBuffer()
{
    hs::GameSimulation simulation;
    auto data = QuietGameData(); data.arena_obstacle_count = 0;
    Check(simulation.Initialize({0xB0FFu}, data).Succeeded(), "buffer replacement initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
    hs::HeldInputState held; held.aim_world = {20, 0, 0}; hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
    for (int i = 0; i < 2000 && simulation.GetObservation().cooldown_ticks[0] > 5; ++i)
        (void)Tick(simulation, held);
    Check(simulation.GetObservation().cooldown_ticks[0] == 5, "replacement starts inside nine-tick buffer window");
    // Same-tick edges preserve the critical expiry boundary: stale Q would become
    // eligible exactly when the successful W tap's nine-tick recovery ends.
    const std::array edges{
        hs::ActionEdge{++sequence, hs::GameAction::SkillQ, hs::EdgeKind::Pressed},
        hs::ActionEdge{++sequence, hs::GameAction::SkillW, hs::EdgeKind::Pressed},
        hs::ActionEdge{++sequence, hs::GameAction::SkillW, hs::EdgeKind::Released}};
    (void)Tick(simulation, held, edges);
    for (int i = 0; i < 12; ++i) (void)Tick(simulation, held);
    const auto &uses = simulation.GetObservation().balance.skill_uses;
    Check(uses[static_cast<std::size_t>(hs::SkillKind::ChargedShot)] == 1,
          "new successful input executes its charge release");
    Check(uses[static_cast<std::size_t>(hs::SkillKind::PiercingShot)] == 1,
          "successful new input removes old buffered skill instead of delayed recast");
    Check(simulation.Shutdown().Succeeded(), "buffer replacement shutdown");
}

void RunGameplayCombatTests()
{
    TestExplosionUpgradesRespectTerrain();
    TestSuccessfulInputReplacesOldBuffer();
    TestAttacksRespectTerrain();
    TestChargeBufferAndRecovery();
    TestCombatVfxCoverage();
    TestAttackStopsMovementAndFacesAim();
    TestSkillMovementPauseAndResume();
    TestBasicAttackStopsAtFirstEnemy();
    TestTenMinuteBossApproachesAttackRange();
    TestTenMinuteBossGroundAreasStaySeparated();
    TestQwerInputBuffer();
    TestBasicAttackSkillCancellationAndBufferRules();
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
