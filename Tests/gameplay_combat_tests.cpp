#include "gameplay_test_support.hpp"
#include <hs/presentation/meadow_surface.hpp>

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
void TestArrowReleaseCarriesAuthoritativeDirection();
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
void TestAuthoritativeCombatGeometryEmission();
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
void TestDetailedBalanceSwitchPreservesSimulation();
void TestSelectiveUpgradeInheritance();
void TestHighFanoutChainsTerminate();
void TestRetreatSlowTrailUsesPath();
void TestSecondRetreatRespectsCollision();
void TestRicochetCooldownBudget();
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
    for (const auto &signal : enemy.PendingDomainSignals())
        if (signal.kind == hs::DomainSignalKind::MeleeEnemyWindup)
            Check(signal.geometry.kind == hs::DomainSignalGeometryKind::Circle &&
                      signal.geometry.radius == data.enemies[0].attack_range &&
                      signal.geometry.start_tick == signal.tick &&
                      signal.geometry.end_tick > signal.geometry.start_tick && signal.geometry.source_id != 0 &&
                      signal.scale == 1.0f,
                  "melee warning carries its actual attack range and resolve interval");
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
    bool ranged_release{};
    std::size_t ranged_impacts{};
    std::vector<std::uint64_t> ranged_projectile_ids;
    std::vector<hs::Sequence> ranged_seen_sequences;
    for (std::uint32_t tick = 0; tick < 90; ++tick)
    {
        (void)Tick(ranged);
        const auto signals = ranged.PendingDomainSignals();
        std::size_t tick_impacts{};
        for (const auto &signal : signals)
        {
            ranged_release |= signal.kind == hs::DomainSignalKind::RangedEnemyReleased;
            if (signal.kind == hs::DomainSignalKind::EnemyRangedProjectileImpact)
            {
                if (std::ranges::find(ranged_seen_sequences, signal.sequence) !=
                    ranged_seen_sequences.end())
                    continue;
                ranged_seen_sequences.push_back(signal.sequence);
                ++tick_impacts;
                Check(signal.geometry.kind == hs::DomainSignalGeometryKind::Projectile &&
                          signal.geometry.source_id != 0 &&
                          std::ranges::find(ranged_projectile_ids,
                                            signal.geometry.source_id) ==
                              ranged_projectile_ids.end() &&
                          signal.geometry.radius > 0.0f &&
                          std::abs(signal.geometry.velocity.z) +
                                  std::abs(signal.geometry.velocity.x) > 0.0001f &&
                          std::abs(signal.position.y - 0.45f) < 0.0001f,
                      "ranged projectile impact lost real contact payload");
                ranged_projectile_ids.push_back(signal.geometry.source_id);
            }
        }
        Check(tick_impacts <= 1, "ranged projectile emitted duplicate actor impact");
        ranged_impacts += tick_impacts;
    }
    Check(ranged_release && ranged_impacts == ranged_projectile_ids.size() &&
              ranged_impacts > 0,
          "ranged projectile did not produce one impact per actual projectile");
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
        for (const auto &signal : suicide.PendingDomainSignals())
            if (signal.kind == hs::DomainSignalKind::SuicideEnemyExploded)
                Check(signal.geometry.kind == hs::DomainSignalGeometryKind::Circle &&
                          std::abs(signal.geometry.radius - data.enemies[static_cast<std::size_t>(hs::EnemyKind::Suicide)].suicide_explosion_radius) < .0001f &&
                          signal.scale == 1.0f,
                      "suicide explosion carries authoritative radius and preserves legacy scale");
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
    for (const auto &signal : bosses.PendingDomainSignals())
        if (signal.kind == hs::DomainSignalKind::BossDashImpact)
            Check(signal.geometry.kind == hs::DomainSignalGeometryKind::Circle &&
                      signal.geometry.radius == bosses.Rules().boss_common.collision_radius + bosses.Rules().stats.player_collision_radius &&
                      signal.geometry.start_tick == signal.tick && signal.geometry.end_tick == signal.tick &&
                      signal.geometry.source_id != 0 && signal.scale == 1.0f,
                  "boss dash endpoint carries its actual swept contact radius");
    Check(bosses.Shutdown().Succeeded(), "boss VFX shutdown");
}

void TestArrowReleaseCarriesAuthoritativeDirection()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x4152524F57u}, QuietGameData()).Succeeded(),
          "arrow release geometry initialize");
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    const hs::DomainSignal *release = nullptr;
    for (std::uint32_t tick = 0; tick < 30 && release == nullptr; ++tick)
    {
        (void)Tick(simulation, held);
        const auto signals = simulation.PendingDomainSignals();
        const auto found = std::ranges::find_if(
            signals, [](const hs::DomainSignal &signal) {
                return signal.kind == hs::DomainSignalKind::ArrowReleased;
            });
        if (found != signals.end()) release = &*found;
    }
    Check(release != nullptr && std::abs(release->direction.x - 1.0f) < 0.0001f &&
              std::abs(release->direction.z) < 0.0001f &&
              std::abs(release->position.y - 1.05f) < 0.0001f,
          "arrow release keeps normalized aim direction and release height");
    Check(simulation.Shutdown().Succeeded(), "arrow release geometry shutdown");
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
    hs::CopyReadModelForDevelopment(simulation, projectile_model);
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
    const auto body_center_height = projectile->position.y + projectile->scale.y * 0.5f;
    const auto ground_height = projectile_model.View().arena_half_extent >= 14.0f
        ? hs::MeadowGroundHeight({projectile->position.x, projectile->position.z}) : 0.0f;
    Check(std::abs(body_center_height - ground_height - 1.17f) < 0.0001f,
          "basic arrow body retains its terrain-relative center height");
    const auto trail = std::ranges::find_if(projectile_snapshot.View().persistent_vfx,
        [&](const hs::PersistentVfxVisual &visual) {
            return visual.kind == hs::PersistentVfxKind::ProjectileTrail &&
                   visual.stable_id == projectile->stable_id;
        });
    Check(trail != projectile_snapshot.View().persistent_vfx.end(),
          "basic arrow has its own persistent trail");
    Check(trail->radius < projectile_radius && trail->radius >= 0.05f && trail->length <= 0.45f,
          "basic arrow trail keeps its bounded radius and length");
    Check(std::abs(trail->position.y - body_center_height) < 0.0001f,
          std::format("basic arrow trail must share the rendered body center: trail={} body={}",
                      trail->position.y, body_center_height));

    held.basic_attack_held = false;
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_position.x > attacking.player_position.x,
          "held RMB reissues movement after the basic attack ends");
    Check(simulation.Shutdown().Succeeded(), "attack movement shutdown");
}

void TestProjectileTrailsShareBodyGrounding()
{
    constexpr std::array skills{hs::SkillKind::BasicAttack, hs::SkillKind::MultiShot,
        hs::SkillKind::ChargedShot, hs::SkillKind::RicochetArrow};
    constexpr std::array positions{hs::Float2{0.6f, 1.2f}, hs::Float2{-2.6f, -7.0f},
        hs::Float2{12.2f, 0.0f}};
    bool different_ground_samples = false;
    for (const auto extent : {12.0f, 24.0f})
    for (const auto skill : skills)
    for (const auto position : positions)
    {
        hs::GameReadModelStorage model;
        model.tick = 20;
        model.session_id = 7;
        model.session.phase = hs::SessionPhase::Playing;
        model.arena_half_extent = extent;
        hs::ProjectileView source;
        source.id = {37}; source.player_owned = true; source.position = position;
        source.previous_position = {position.x - 0.15f, position.y - 0.2f};
        source.velocity = {9.0f, 12.0f}; source.spawned_tick = 10;
        source.skill = skill; source.radius = 0.36f; source.charge_ratio = 0.7f;
        model.AddProjectile(source);
        hs::AreaView area;
        area.id = {38}; area.kind = hs::AreaViewKind::Slow;
        area.position = {-2.6f, -7.0f}; area.radius = 0.5f;
        area.active_tick = 10; area.expires = 40;
        model.AddArea(area);
        hs::RenderSnapshotStorage snapshot(32, 2, 2, 64);
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
            hs::PresentationUiState{}, hs::SettingsData{}, snapshot), "project terrain trail fixture");
        const auto ground = [extent](hs::Float2 point) {
            return extent >= 14.0f ? hs::MeadowGroundHeight(point) : 0.0f;
        };
        const auto body = std::ranges::find_if(snapshot.View().instances,
            [](const auto &instance) { return instance.mesh == hs::RenderMesh::PlayerProjectile; });
        Check(body != snapshot.View().instances.end(), "terrain trail fixture has a body");
        const auto body_center = body->position.y + body->scale.y * 0.5f;
        Check(std::abs(body->position.y - (1.05f + ground(source.position))) < 0.0001f,
              "projectile body uses current-position ground exactly once");
        std::size_t trails = 0;
        for (const auto &visual : snapshot.View().persistent_vfx)
        {
            if (visual.kind != hs::PersistentVfxKind::ProjectileTrail &&
                visual.kind != hs::PersistentVfxKind::RicochetProjectileTrail &&
                visual.kind != hs::PersistentVfxKind::ProjectileTrailOuter) continue;
            ++trails;
            Check(visual.projectile_owner_id == source.id.value &&
                      std::abs(visual.position.y - body_center) < 0.0001f,
                  "every projectile trail shares its owner's rendered center height");
            Check(std::abs(visual.projectile_current_position.y - body->position.y) < 0.0001f &&
                      std::abs(visual.projectile_previous_position.y -
                          (1.05f + ground(source.previous_position))) < 0.0001f,
                  "trail motion endpoints retain their independent terrain samples");
            different_ground_samples |= std::abs(ground({visual.position.x, visual.position.z}) -
                ground(source.position)) > 0.001f;
        }
        Check(trails == (skill == hs::SkillKind::ChargedShot ? 2u : 1u),
              "fixture covers inner, outer and ricochet trail kinds without losing outputs");
        const auto ground_visual = std::ranges::find_if(snapshot.View().persistent_vfx,
            [](const auto &visual) { return visual.kind == hs::PersistentVfxKind::SlowArea; });
        Check(ground_visual != snapshot.View().persistent_vfx.end() &&
                  std::abs(ground_visual->position.y - (0.018f + ground(area.position))) < 0.0001f,
              "ground-bound area effects still use their own terrain anchor");
    }
    Check(different_ground_samples, "terrain trail fixture exercises non-flat anchor differences");
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

void TestEnemyLookupAcrossCleanupAndSpawn()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.enemies[0].move_speed = 0.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({143}, data).Succeeded(), "enemy lookup initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 10.0f});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, -10.0f});
    hs::GameReadModelStorage model;
    hs::CopyReadModelForDevelopment(simulation, model);
    const auto first = model.View().enemies.front().id;
    const auto last = model.View().enemies.back().id;
    const auto attack = [&] {
        hs::HeldInputState held;
        held.aim_world = {20.0f, 0.0f, 0.0f};
        held.basic_attack_held = true;
        (void)Tick(simulation, held);
        held.basic_attack_held = false;
        for (unsigned tick = 0; tick < 90; ++tick) (void)Tick(simulation, held);
        hs::CopyReadModelForDevelopment(simulation, model);
        Check(model.View().enemies.size() == 2 &&
                    model.View().enemies.front().id.value == first.value &&
                    model.View().enemies.back().id.value == last.value,
              "target lookup preserves surviving IDs across middle removal and respawn");
    };
    attack();
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    attack();
    Debug(simulation, hs::DebugCommandKind::StartSession);
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    hs::CopyReadModelForDevelopment(simulation, model);
    Check(model.View().enemies.size() == 1 &&
              model.View().enemies.front().id.value > last.value,
          "session reset clears actors without invalidating ID ordering");
    Check(simulation.Shutdown().Succeeded(), "enemy lookup shutdown");
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
            hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
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
            hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
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

void TestRedirectedArrowUsesRemainingTick()
{
    std::array<hs::GameplayChecksum, 2> checksums{};
    for (const auto repeat : {false, true})
    for (const bool returning : {false, true})
    {
        auto data = QuietGameData();
        data.arena_obstacle_count = 1;
        data.arena_obstacles[0] = {hs::ArenaObstacleKind::Rock, {1.1f, -0.5f}, 0.1f};
        data.enemies[0].health = 1000;
        data.enemies[0].move_speed = 0;
        data.enemies[0].damage = 0;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xC05u}, data).Succeeded(), "redirected arrow initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
        Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow), returning ? 0 : 1);
        hs::HeldInputState held; held.aim_world = {20, 0, 0};
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        hs::GameReadModelStorage model;
        hs::ProjectileView before;
        for (int i = 0; i < 30 && before.id.value == 0; ++i)
        {
            hs::CopyReadModelForDevelopment(simulation, model);
            if (!model.View().projectiles.empty()) before = model.View().projectiles.front();
            else (void)Tick(simulation, held);
        }
        Check(before.id.value != 0 && std::abs(before.position.x - 28.0f / 60.0f) < 0.0001f,
              "fixture launches one unhomed authored-speed arrow");
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {0.7f, 1.069f});
        if (!returning)
            Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {-1.3f, 2});
        const auto result = Tick(simulation, held);
        hs::CopyReadModelForDevelopment(simulation, model);
        const auto view = model.View();
        const auto arrow = std::ranges::find_if(view.projectiles, [&](const auto &p) {
            return p.id.value == before.id.value;
        });
        Check(arrow != view.projectiles.end(), "redirected arrow survives the obsolete wall intersection");
        Check(arrow->returning == returning && arrow->position.x < 0.66f,
              "redirect uses its contact point and travels only the remaining tick");
        Check(std::abs(arrow->previous_position.x - before.position.x) < 0.0001f,
              "render interpolation retains the real tick start");
        if (returning)
        {
            Check(std::abs(arrow->return_start_position.x - 0.6537507f) < 0.0001f &&
                      std::abs(arrow->position.y) < 0.0001f,
                  "return starts at contact and heads back toward the player");
        }
        else
        {
            Check(std::abs(arrow->position.x - 0.4583818f) < 0.0002f &&
                      std::abs(arrow->position.y - 0.1999937f) < 0.0002f,
                  "ricochet endpoint matches the contact-to-target remaining-time segment");
            const auto child = std::ranges::find_if(view.projectiles, [](const auto &p) {
                return p.origin == hs::EffectOrigin::Derived && p.source_upgrade == 1;
            });
            Check(child != view.projectiles.end() && child->spawned_tick == result.tick &&
                      std::abs(child->position.x - 0.6537507f) < 0.0001f &&
                      child->position.x == child->previous_position.x &&
                      child->position.y == child->previous_position.y,
                  "split child starts at contact and cannot travel during its spawn tick");
        }
        Check(simulation.GetObservation().damage_by_skill[5] > 0, "first enemy still receives damage");
        Check(std::ranges::count_if(simulation.PendingDomainSignals(), [&](const auto &signal) {
                  return signal.tick == result.tick && signal.kind == hs::DomainSignalKind::RicochetArrowHit;
              }) == 1, "redirect departure cannot immediately rehit the same contact");
        if (repeat) Check(checksums[returning] == result.checksum, "redirected paths replay deterministically");
        else checksums[returning] = result.checksum;
        Check(simulation.Shutdown().Succeeded(), "redirected arrow shutdown");
    }
}

void TestProjectilesHitMovingTargets()
{
    struct Case { float projectile_speed, enemy_speed; bool hit; };
    for (const auto fixture : {Case{0, 0, false}, Case{0, 720, true},
                                Case{600, 720, true}, Case{600, 0, true}})
    {
        hs::GameplayChecksum first{};
        for (int repeat = 0; repeat < 2; ++repeat)
        {
            auto data = QuietGameData();
            data.arena_obstacle_count = 0;
            data.enemies[0].health = 1000;
            data.enemies[0].move_speed = fixture.enemy_speed;
            data.enemies[0].damage = 0;
            data.enemies[0].attack_range = 0;
            data.skills[0].projectile_speed = fixture.projectile_speed;
            data.skills[5].projectile_speed = fixture.projectile_speed;
            data.skills[5].range = 100;
            hs::GameSimulation simulation;
            Check(simulation.Initialize({0xC010u}, data).Succeeded(), "moving target initialize");
            Debug(simulation, hs::DebugCommandKind::GrantSkill,
                  static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
            hs::HeldInputState held; held.aim_world = {40, 0, 0};
            hs::Sequence sequence{};
            (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
            hs::GameReadModelStorage model;
            hs::ProjectileView before;
            for (int i = 0; i < 30 && before.id.value == 0; ++i)
            {
                hs::CopyReadModelForDevelopment(simulation, model);
                if (!model.View().projectiles.empty()) before = model.View().projectiles.front();
                else (void)Tick(simulation, held);
            }
            Check(before.id.value != 0, "moving target fixture launches an original arrow");
            Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0,
                  {before.position.x + 6, fixture.projectile_speed > 0 ? .6f : 0});
            const auto result = Tick(simulation, held);
            hs::CopyReadModelForDevelopment(simulation, model);
            const auto enemies = model.View().enemies;
            Check(enemies.size() == 1 && (enemies[0].health < 1000) == fixture.hit,
                  "moving target sweep catches intermediate contacts and preserves static controls");
            if (fixture.enemy_speed > 0)
                Check(enemies[0].position.x < before.position.x - 4,
                      "crossing fixture ends outside the projectile's final-center broadphase cells");
            if (repeat) Check(first == result.checksum, "moving target collision replay is deterministic");
            else first = result.checksum;
            Check(simulation.Shutdown().Succeeded(), "moving target shutdown");
        }
    }
}

void TestMovingPlayerHitsStationaryEnemyProjectile()
{
    auto data = QuietGameData();
    data.arena_obstacle_count = 0;
    data.stats.base_movement_speed_mps = 720;
    data.enemies[1].move_speed = 0;
    data.enemies[1].projectile_speed = 0;
    data.enemies[1].attack_range = 10;
    data.enemies[1].damage = 5;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xC011u}, data).Succeeded(), "moving player collision initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {6, 0});
    hs::GameReadModelStorage model;
    hs::ProjectileView incoming;
    for (int i = 0; i < 120 && incoming.id.value == 0; ++i)
    {
        (void)Tick(simulation);
        hs::CopyReadModelForDevelopment(simulation, model);
        const auto found = std::ranges::find_if(model.View().projectiles,
            [](const auto &projectile) { return !projectile.player_owned; });
        if (found != model.View().projectiles.end()) incoming = *found;
    }
    Check(incoming.id.value != 0 && std::abs(incoming.position.x - 6) < .001f,
          "moving player fixture creates a real stationary ranged projectile");
    const auto health = simulation.GetObservation().health;
    hs::HeldInputState held; held.move_held = true; held.move_target_world = {20, 0, 0};
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_position.x > incoming.position.x + 2 &&
              simulation.GetObservation().health < health,
          "enemy projectile hits a player crossing during the tick rather than only its endpoint");
    Check(simulation.Shutdown().Succeeded(), "moving player collision shutdown");
}

void TestRicochetClipsMovingTargetTime()
{
    auto data = QuietGameData();
    data.arena_obstacle_count = 0;
    for (const auto kind : {hs::EnemyKind::Melee, hs::EnemyKind::Ranged})
    {
        auto &enemy = data.enemies[static_cast<std::size_t>(kind)];
        enemy.health = 1000;
        enemy.move_speed = 0;
        enemy.damage = 0;
        enemy.attack_range = 0;
        enemy.collision_radius = .1f;
    }
    data.enemies[1].move_speed = 120;
    auto &skill = data.skills[5];
    skill.projectile_speed = 600;
    skill.collision_radius = .1f;
    skill.range = 100;
    skill.pierce_count = 1;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xC012u}, data).Succeeded(), "moving ricochet initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
    hs::HeldInputState held; held.aim_world = {40, 0, 0};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
    hs::GameReadModelStorage model;
    hs::ProjectileView before;
    for (int i = 0; i < 30 && before.id.value == 0; ++i)
    {
        hs::CopyReadModelForDevelopment(simulation, model);
        if (!model.View().projectiles.empty()) before = model.View().projectiles.front();
        else (void)Tick(simulation, held);
    }
    Check(before.id.value != 0 && std::abs(before.position.x - 10) < .001f,
          "moving ricochet fixture launches one ten-metre step");
    // The first hit is at 0.38 ticks; replaying the second target's whole path misses it.
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {14, 0});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 1, 0, {11, 1.5f});
    const auto result = Tick(simulation, held);
    hs::CopyReadModelForDevelopment(simulation, model);
    Check(model.View().enemies.size() == 2 &&
              std::ranges::all_of(model.View().enemies, [](const auto &enemy) { return enemy.health < 1000; }),
          "ricochet tests the moving target only during the remaining global tick interval");
    Check(std::ranges::count_if(simulation.PendingDomainSignals(), [&](const auto &signal) {
              return signal.tick == result.tick && signal.kind == hs::DomainSignalKind::RicochetArrowHit;
          }) == 2, "moving ricochet hits each target once in the same tick");
    Check(simulation.Shutdown().Succeeded(), "moving ricochet shutdown");
}

void TestPiercingAndWallContactOrder()
{
    const auto run = [](bool tie, bool wall) {
        auto data = QuietGameData();
        data.arena_obstacle_count = wall ? 1 : 0;
        data.arena_obstacles[0] = tie
            ? hs::ArenaObstacle2D{hs::ArenaObstacleKind::Rock, {5, 1}, 0.63f}
            : hs::ArenaObstacle2D{hs::ArenaObstacleKind::Rock, {6, 0}, 0.3f};
        data.enemies[0].health = 1000;
        data.enemies[0].move_speed = 0;
        data.enemies[0].damage = 0;
        data.skills[0].projectile_speed = 600;
        data.skills[0].collision_radius = 0.44f;
        data.skills[0].pierce_count = 3;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xC051u}, data).Succeeded(), "piercing order initialize");
        if (tie) Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5, -1});
        else
            for (const auto x : {2.0f, 4.0f, 8.0f})
                Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {x, 0});
        hs::HeldInputState held; held.aim_world = {20, 0, 0}; held.basic_attack_held = true;
        (void)Tick(simulation, held); held.basic_attack_held = false;
        for (int i = 0; i < 30; ++i) (void)Tick(simulation, held);
        hs::GameReadModelStorage model;
        hs::CopyReadModelForDevelopment(simulation, model);
        std::vector<std::int32_t> health;
        for (const auto &enemy : model.View().enemies) health.push_back(enemy.health);
        Check(simulation.Shutdown().Succeeded(), "piercing order shutdown");
        return health;
    };
    const auto pierce = run(false, true);
    Check(pierce.size() == 3 && pierce[0] < 1000 && pierce[1] < 1000 && pierce[2] == 1000,
          "one straight segment pierces both front enemies and stops at the wall");
    const auto tie = run(true, true), clear = run(true, false);
    Check(tie.size() == 1 && clear.size() == 1 && tie[0] == 1000 && clear[0] < 1000,
          "opaque terrain wins an equal actor TOI without dropping the clear control hit");
}

void TestZeroTimeRicochetsAreBounded()
{
    auto data = QuietGameData();
    data.arena_obstacle_count = 0;
    data.enemies[0].health = 10000;
    data.enemies[0].move_speed = 0;
    data.enemies[0].damage = 0;
    data.skills[5].collision_radius = 100;
    data.skills[5].pierce_count = 200;
    data.skills[5].ricochet_search_radius = 32;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xC052u}, data).Succeeded(), "zero-time ricochet initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
    for (int i = 0; i < 80; ++i)
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0,
              {2.0f + static_cast<float>(i % 10), static_cast<float>(i / 10)});
    hs::HeldInputState held; held.aim_world = {20, 0, 0};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
    bool progressed{};
    hs::GameReadModelStorage model;
    for (int i = 0; i < 30 && !progressed; ++i)
    {
        (void)Tick(simulation, held);
        hs::CopyReadModelForDevelopment(simulation, model);
        const auto enemies = model.View().enemies;
        const auto hits = std::ranges::count_if(enemies, [](const auto &enemy) { return enemy.health < 10000; });
        if (hits == 0) continue;
        Check(hits < 80, "zero-time chains leave remaining contacts for a later tick");
        progressed = true;
    }
    Check(progressed, "zero-time fixture reaches its first contacts");
    for (int i = 0; i < 4; ++i) (void)Tick(simulation, held);
    hs::CopyReadModelForDevelopment(simulation, model);
    Check(std::ranges::all_of(model.View().enemies, [](const auto &enemy) { return enemy.health < 10000; }),
          "a bounded zero-time chain resumes and eventually visits every target");
    Check(simulation.Shutdown().Succeeded(), "zero-time ricochet shutdown");
}

void TestSoftSeparationRecoversCoincidentCenters()
{
    for (const auto distance : {0.0f, 0.005f, 1.0f})
    {
        auto data = QuietGameData();
        data.arena_obstacle_count = 0;
        data.enemies[0].move_speed = 0;
        hs::GameSimulation first, repeat;
        Check(first.Initialize({0x503u}, data).Succeeded() &&
                  repeat.Initialize({0x503u}, data).Succeeded(), "soft separation initialize");
        for (auto *simulation : {&first, &repeat})
        {
            Debug(*simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {4, 0});
            Debug(*simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {4 + distance, 0});
        }
        Check(Tick(first).checksum == Tick(repeat).checksum, "coincident recovery is deterministic");
        hs::GameReadModelStorage model;
        hs::CopyReadModelForDevelopment(first, model);
        const auto enemies = model.View().enemies;
        Check(enemies.size() == 2, "soft separation keeps both enemies");
        const auto separated = std::hypot(enemies[0].position.x - enemies[1].position.x,
                                         enemies[0].position.y - enemies[1].position.y);
        Check(std::abs(separated - (distance < 0.8f ? 0.8f : distance)) < 0.0001f,
              "coincident centers recover without changing the compressible soft radius");
        Check(first.Shutdown().Succeeded() && repeat.Shutdown().Succeeded(), "soft separation shutdown");
    }
}

void TestChargeBufferAndRecovery()
{
    const auto player = [](hs::GameSimulation &simulation) {
        hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
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
            hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
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
        hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
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
        hs::GameReadModelStorage model; hs::CopyReadModelForDevelopment(simulation, model);
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

void TestExplosionEventsCarryDamageRadius()
{
    for (const auto skill : {hs::SkillKind::ExplosiveArrow, hs::SkillKind::Trap})
    {
        auto data = QuietGameData(); data.arena_obstacle_count = 0;
        data.enemies[0].health = 10000; data.enemies[0].move_speed = 0; data.enemies[0].damage = 0;
        const auto skill_index = static_cast<std::size_t>(skill);
        data.skills[skill_index].area_radius = 6.25f;
        data.upgrades.explosive_arrow.delayed_reexplosion.radius = 4.75f;
        data.upgrades.explosive_arrow.three_delayed_satellite_bombs.explosion_radius = 2.25f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xE6E0u}, data).Succeeded(), "explosion geometry initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(skill));
        if (skill == hs::SkillKind::ExplosiveArrow)
        {
            Debug(simulation, hs::DebugCommandKind::GrantUpgrade, static_cast<std::uint64_t>(skill), 0);
            Debug(simulation, hs::DebugCommandKind::GrantUpgrade, static_cast<std::uint64_t>(skill), 1);
        }
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0,
              skill == hs::SkillKind::Trap ? hs::Float2{.5f,0} : hs::Float2{5,0});
        hs::HeldInputState held; held.aim_world = {20,0,0}; hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        bool saw_mini_owner=false;
        for (int i=0;i<240;++i)
        {
            (void)Tick(simulation,held);
            hs::GameReadModelStorage model;hs::CopyReadModelForDevelopment(simulation, model);
            for(const auto &bomb:model.View().mini_bombs)
            {
                saw_mini_owner=true;
                Check(bomb.owner_id!=0&&bomb.cast_id!=0&&bomb.radius==2.25f&&bomb.started<=model.tick&&model.tick<bomb.expires,
                      "mini-bomb snapshot owns its real scheduled interval and radius");
                const auto signal=std::ranges::find_if(simulation.PendingDomainSignals(),[&](const auto &v){
                    return v.upgrade_owner_id==bomb.owner_id&&v.upgrade_stage==hs::UpgradeVisualStage::Telegraph;
                });
                Check(signal!=simulation.PendingDomainSignals().end()&&signal->geometry.start_tick==bomb.started&&
                    signal->geometry.end_tick==bomb.expires&&signal->position.x==bomb.position.x&&signal->position.z==bomb.position.y,
                    "warning event and scheduled owner disagree");
            }
        }
        if(skill==hs::SkillKind::ExplosiveArrow)
        {
            Check(saw_mini_owner,"mini-bomb schedules never published owners");
            hs::GameReadModelStorage model;hs::CopyReadModelForDevelopment(simulation, model);
            Check(model.View().mini_bombs.empty(),"resolved mini bombs retained warning owners");
            std::size_t births=0,warnings=0,resolves=0;
            for(const auto &signal:simulation.PendingDomainSignals())
            {
                if(signal.upgrade_index!=1)continue;
                births+=signal.upgrade_stage==hs::UpgradeVisualStage::Spawn;
                warnings+=signal.upgrade_stage==hs::UpgradeVisualStage::Telegraph;
                resolves+=signal.upgrade_stage==hs::UpgradeVisualStage::Resolve;
                Check(signal.upgrade_skill==static_cast<std::uint8_t>(skill)&&signal.upgrade_owner_id!=0&&signal.upgrade_cast_id!=0,
                      "mini-bomb activation loses upgrade identity");
            }
            Check(births==data.upgrades.explosive_arrow.three_delayed_satellite_bombs.bomb_count&&births==warnings&&births==resolves,
                  "mini-bomb stages must occur once per actual scheduled bomb");
        }
        bool main=false,secondary=false,satellite=false,trap=false;
        for (const auto &signal : simulation.PendingDomainSignals())
        {
            float expected=0,scale=0;
            switch(signal.kind)
            {
            case hs::DomainSignalKind::ExplosiveArrowMain: main=true;expected=6.25f;scale=expected/3;break;
            case hs::DomainSignalKind::ExplosiveArrowSecondary: secondary=true;expected=4.75f;scale=expected/3;break;
            case hs::DomainSignalKind::SmallExplosion: satellite=true;expected=2.25f;scale=expected;break;
            case hs::DomainSignalKind::TrapTriggered: trap=true;expected=6.25f;scale=expected;break;
            default:continue;
            }
            Check(signal.geometry.kind == hs::DomainSignalGeometryKind::Circle &&
                      std::abs(signal.geometry.radius-expected)<.0001f && std::abs(signal.scale-scale)<.0001f,
                  "explosion geometry uses exact damage radius without changing legacy scale");
        }
        Check(skill == hs::SkillKind::Trap ? trap : (main&&secondary&&satellite),
              "requested authoritative explosion events were exercised");
        Check(simulation.Shutdown().Succeeded(), "explosion geometry shutdown");
    }
}

void TestVisualLinksRetainCapturedEndpoints()
{
    auto data = QuietGameData(); data.arena_obstacle_count = 0;
    data.enemies[0].health = 10000; data.enemies[0].move_speed = 1.0f; data.enemies[0].damage = 0;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x11ABu},data).Succeeded(), "visual link initialize");
    Debug(simulation,hs::DebugCommandKind::GrantSkill,static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
    Debug(simulation,hs::DebugCommandKind::GrantUpgrade,static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow),3);
    Debug(simulation,hs::DebugCommandKind::SpawnEnemy,0,0,{4,0});
    Debug(simulation,hs::DebugCommandKind::SpawnEnemy,0,0,{7,0});
    Debug(simulation,hs::DebugCommandKind::SpawnEnemy,0,0,{6,2});
    hs::HeldInputState held;held.aim_world={20,0,0};hs::Sequence sequence{};
    (void)TickEdge(simulation,hs::GameAction::SkillQ,hs::EdgeKind::Pressed,sequence,held);
    hs::GameReadModelStorage model;
    std::vector<hs::VisualLinkView> captured;
    for(int i=0;i<90 && captured.empty();++i)
    {
        (void)Tick(simulation,held);hs::CopyReadModelForDevelopment(simulation, model);
        if(model.View().visual_links.size()>=2)
            captured.assign(model.View().visual_links.begin(),model.View().visual_links.end());
    }
    Check(captured.size()>=2,"actual ricochet and burn transfer create visual owners");
    if(captured.size()>=2)
    {
        Check(captured[0].owner_id!=captured[1].owner_id,"simultaneous links have distinct sequence owners");
        for(const auto &link:captured)
        {
            const bool burn=link.kind==hs::VisualLinkKind::BurnTransfer;
            Check(link.expires-link.started==(burn?11u:8u) && std::abs(link.width-(burn?.12f:.09f))<.0001f,
                  "visual link uses explicit cosmetic width and tick lifetime");
            const auto signal=std::ranges::find_if(simulation.PendingDomainSignals(),[&](const auto &event){return event.sequence==link.owner_id;});
            Check(signal!=simulation.PendingDomainSignals().end() && signal->position.x==link.source_position.x &&
                      signal->position.z==link.source_position.z && signal->target.x==link.target_position.x && signal->target.z==link.target_position.z,
                  "link endpoints and owner match the emitted domain event");
        }
        const auto checksum=simulation.ComputeChecksum();hs::CopyReadModelForDevelopment(simulation, model);
        Check(simulation.ComputeChecksum()==checksum,"visual publication does not mutate gameplay checksum");
        (void)Tick(simulation,held);hs::CopyReadModelForDevelopment(simulation, model);
        for(const auto &original:captured)
        {
            const auto current=std::ranges::find_if(model.View().visual_links,[&](const auto &link){return link.owner_id==original.owner_id;});
            Check(current!=model.View().visual_links.end() && current->started==original.started && current->expires==original.expires &&
                      current->source_position.x==original.source_position.x && current->source_position.z==original.source_position.z &&
                      current->target_position.x==original.target_position.x && current->target_position.z==original.target_position.z,
                  "moving enemies do not retarget captured link endpoints or restart lifetime");
        }
        const auto last_expiry=std::ranges::max(captured,{},&hs::VisualLinkView::expires).expires;
        while(simulation.GetObservation().tick<last_expiry) (void)Tick(simulation,held);
        hs::CopyReadModelForDevelopment(simulation, model);
        for(const auto &original:captured)
            Check(std::ranges::none_of(model.View().visual_links,[&](const auto &link){return link.owner_id==original.owner_id;}),
                  "visual link expires at its exact end tick");
    }
    Debug(simulation,hs::DebugCommandKind::StartSession);
    hs::CopyReadModelForDevelopment(simulation, model);
    Check(model.View().visual_links.empty(),"session reset clears visual links");
    Check(simulation.Shutdown().Succeeded(),"visual link shutdown");
}

void TestSkillUnlockProjectsVfxAndAudio()
{
    hs::DomainSignal signal;
    signal.kind = hs::DomainSignalKind::SkillUnlocked;
    signal.context = static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow);
    signal.sequence = 87; signal.tick = 145; signal.session_id = 9;
    signal.position = {3,.2f,-4};
    std::array<hs::PresentationEvent,4> events{};
    const auto count = hs::ProjectDomainSignal(signal,events);
    Check(count==2,"skill unlock projects one VFX and existing audio");
    bool visual=false,audio=false;
    for(std::size_t i=0;i<count;++i)
    {
        const auto &event=events[i];
        visual |= event.kind==hs::PresentationKind::Vfx && event.asset.value==hs::MakeAssetId("particle.player.skill_unlock").value;
        audio |= event.kind==hs::PresentationKind::Audio && event.asset.value==hs::MakeAssetId("audio.ui.unlock").value;
        Check(event.sequence==87 && event.tick==145 && event.session_id==9 && event.position.x==3 && event.position.z==-4,
              "unlock projection preserves event identity and player anchor");
    }
    Check(visual&&audio,"unlock projection uses authored context recipe and retains audio");
}

void RunGameplayCombatTests()
{
    TestSkillUnlockProjectsVfxAndAudio();
    TestVisualLinksRetainCapturedEndpoints();
    TestExplosionEventsCarryDamageRadius();
    TestExplosionUpgradesRespectTerrain();
    TestSuccessfulInputReplacesOldBuffer();
    TestAttacksRespectTerrain();
    TestRedirectedArrowUsesRemainingTick();
    TestProjectilesHitMovingTargets();
    TestMovingPlayerHitsStationaryEnemyProjectile();
    TestRicochetClipsMovingTargetTime();
    TestPiercingAndWallContactOrder();
    TestZeroTimeRicochetsAreBounded();
    TestSoftSeparationRecoversCoincidentCenters();
    TestSecondRetreatRespectsCollision();
    TestRicochetCooldownBudget();
    TestChargeBufferAndRecovery();
    TestCombatVfxCoverage();
    TestArrowReleaseCarriesAuthoritativeDirection();
    TestAttackStopsMovementAndFacesAim();
    TestProjectileTrailsShareBodyGrounding();
    TestSkillMovementPauseAndResume();
    TestBasicAttackStopsAtFirstEnemy();
    TestEnemyLookupAcrossCleanupAndSpawn();
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
    TestAuthoritativeCombatGeometryEmission();
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
    TestDetailedBalanceSwitchPreservesSimulation();
    TestSelectiveUpgradeInheritance();
    TestHighFanoutChainsTerminate();
    TestRetreatSlowTrailUsesPath();
}

} // namespace gameplay_test
