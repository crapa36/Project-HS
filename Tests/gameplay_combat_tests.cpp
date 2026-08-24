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

std::size_t CountVfx(const hs::GameSimulation &simulation, hs::DomainSignalKind kind)
{
    return std::ranges::count_if(
        simulation.PendingDomainSignals(),
        [kind](const hs::DomainSignal &event) { return event.kind == kind; });
}

template <typename Function>
std::size_t ForEachFourOfEight(Function &&function)
{
    std::size_t count{};
    for (std::uint8_t first = 0; first < hs::kUpgradeCount; ++first)
    {
        for (std::uint8_t second = first + 1; second < hs::kUpgradeCount; ++second)
        {
            for (std::uint8_t third = second + 1; third < hs::kUpgradeCount; ++third)
            {
                for (std::uint8_t fourth = third + 1; fourth < hs::kUpgradeCount;
                     ++fourth)
                {
                    function(std::array{first, second, third, fourth});
                    ++count;
                }
            }
        }
    }
    return count;
}

void ExerciseCombatCombination(hs::SkillKind skill,
                               const std::array<std::uint8_t, 4> &upgrades)
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({31}, QuietGameData()).Succeeded(),
          "combat combination initialize");
    if (skill != hs::SkillKind::BasicAttack)
    {
        Debug(simulation, hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(skill));
    }

    std::uint8_t expected_mask{};
    for (const auto upgrade : upgrades)
    {
        Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(skill), upgrade);
        expected_mask |= static_cast<std::uint8_t>(1u << upgrade);
    }
    const auto granted = simulation.GetObservation();
    const auto index = static_cast<std::size_t>(skill);
    Check(granted.skill_levels[index] == 5 && granted.upgrade_masks[index] == expected_mask,
          std::format("combat upgrades skill={} mask={}", index, expected_mask));

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    if (skill == hs::SkillKind::BasicAttack)
    {
        held.basic_attack_held = true;
        (void)Tick(simulation, held);
    }
    else
    {
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                       sequence, held);
        if (skill == hs::SkillKind::ChargedShot)
        {
            (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released,
                           sequence, held);
        }
        else
        {
            (void)Tick(simulation, held);
        }
    }
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing,
          "combat combination remains playable");
    Check(simulation.Shutdown().Succeeded(), "combat combination shutdown");
}

void ExerciseRelics(std::span<const std::uint8_t> relics)
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({41}, QuietGameData()).Succeeded(), "relic initialize");
    std::uint16_t expected_mask{};
    for (const auto relic : relics)
    {
        Debug(simulation, hs::DebugCommandKind::GrantRelic, relic);
        expected_mask |= static_cast<std::uint16_t>(1u << relic);
    }
    Check(simulation.GetObservation().relic_mask == expected_mask, "relic mask");

    hs::HeldInputState held;
    held.move_held = true;
    held.move_target_world = {10.0f, 0.0f, 0.0f};
    held.aim_world = {10.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing,
          "relic combination remains playable");
    Check(simulation.Shutdown().Succeeded(), "relic shutdown");
}

} // namespace

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

void TestSkillMovementPauseAndResume()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({141}, QuietGameData()).Succeeded(),
          "skill movement initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));

    hs::HeldInputState held;
    held.move_held = true;
    held.move_target_world = {20.0f, 0.0f, 0.0f};
    held.aim_world = {0.0f, 0.0f, 20.0f};
    (void)Tick(simulation, held);
    held.move_held = false;
    const auto before_cast = simulation.GetObservation().player_position;
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(std::abs(simulation.GetObservation().player_position.x - before_cast.x) < 0.0001f,
          "successful skill pauses movement during its action delay");
    for (std::uint32_t tick = 0; tick < 15; ++tick) (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_position.x > before_cast.x,
          "movement resumes toward the saved destination after skill recovery");

    const auto before_cooldown_press = simulation.GetObservation().player_position;
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().player_position.x > before_cooldown_press.x,
          "cooldown skill input does not cancel or pause movement");
    Check(simulation.Shutdown().Succeeded(), "skill movement shutdown");
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
            return instance.mesh == hs::RenderMesh::Boss;
        });
    Check(before != before_snapshot.View().instances.end(), "ten minute boss visible");
    const auto before_distance = std::hypot(before->position.x, before->position.z);
    for (std::uint32_t tick = 0; tick < 151; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage after_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, after_snapshot), "boss after snapshot");
    const auto after = std::ranges::find_if(
        after_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Boss;
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
            return instance.mesh == hs::RenderMesh::Boss;
        });
    Check(warning != warning_snapshot.View().instances.end(),
          "ten minute boss visible during warning");
    const auto warning_position = warning->position;
    for (std::uint32_t tick = 0; tick < 20; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage casting_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, casting_snapshot), "boss casting snapshot");
    const auto casting = std::ranges::find_if(
        casting_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Boss;
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

void TestQwerSkills()
{
    const auto &cooked_rules = DefaultContent().simulation_rules;
    const auto charged = cooked_rules.skills[
        static_cast<std::size_t>(hs::SkillKind::ChargedShot)];
    Check(charged.cooldown_ticks == 240 &&
              std::abs(charged.range - 16.8f) < 0.0001f &&
              std::abs(charged.damage_coefficient - 5.0f) < 0.0001f &&
              std::abs(charged.collision_radius - 0.88f) < 0.0001f &&
              charged.pierce_count == 12,
          "charged shot uses 200-500% damage and compact collision and pierce count");
    Check(charged.minimum_damage_multiplier == 2.0f &&
              cooked_rules.upgrades.charged_shot.extended_full_charge_explosion
                      .maximum_damage_multiplier == 6.0f &&
              cooked_rules.upgrades.charged_shot.increase_minimum_charge_damage
                      .minimum_damage_multiplier == 5.0f,
          "charged shot damage interpolation uses cooked values");
    hs::GameSimulation simulation;
    Check(simulation.Initialize({13}, QuietGameData()).Succeeded(), "QWER initialize");
    for (const auto skill : {hs::SkillKind::PiercingShot, hs::SkillKind::MultiShot,
                             hs::SkillKind::ChargedShot, hs::SkillKind::ExplosiveArrow})
    {
        Debug(simulation, hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(skill));
    }
    Check(simulation.GetObservation().active_skill_count == 4, "four active skills granted");

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    (void)Tick(simulation, held);
    hs::Sequence sequence{};

    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[0] > 0, "Q casts first skill");

    for (std::uint32_t tick = 0; tick < 15; ++tick) (void)Tick(simulation, held);

    (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[1] > 0, "W casts second skill");

    for (std::uint32_t tick = 0; tick < 18; ++tick) (void)Tick(simulation, held);

    held.move_held = true;
    held.move_target_world = {20.0f, 0.0f, 20.0f};
    const auto before_charge = simulation.GetObservation().player_position;
    (void)TickEdge(simulation, hs::GameAction::SkillE, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[2] == 0, "E starts charged skill");
    for (std::uint32_t tick = 0; tick < 5; ++tick)
    {
        (void)Tick(simulation, held);
    }
    const auto charging = simulation.GetObservation();
    const auto charge_move = std::hypot(
        charging.player_position.x - before_charge.x,
        charging.player_position.y - before_charge.y);
    Check(charge_move > 0.1f && charge_move < 0.5f &&
              charging.facing_direction.x > 0.65f &&
              charging.facing_direction.y > 0.65f,
          "charged skill moves slowly and faces its movement direction");
    (void)TickEdge(simulation, hs::GameAction::SkillE, hs::EdgeKind::Released,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[2] > 0, "E releases charged skill");
    const auto released = simulation.GetObservation();
    const auto aim_x = 20.0f - released.player_position.x;
    const auto aim_y = -released.player_position.y;
    const auto aim_length = std::hypot(aim_x, aim_y);
    Check((released.facing_direction.x * aim_x +
           released.facing_direction.y * aim_y) / aim_length > 0.999f,
          "charged skill turns toward aim only when released");

    for (std::uint32_t tick = 0; tick < 9; ++tick) (void)Tick(simulation, held);

    (void)TickEdge(simulation, hs::GameAction::SkillR, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[3] > 0, "R casts fourth skill");
    Check(simulation.Shutdown().Succeeded(), "QWER shutdown");
}

void TestChargedShotDamageFormula()
{
    const auto fire = [](std::uint32_t hold_ticks,
                         std::span<const std::uint8_t> upgrades = {}) {
        auto data = QuietGameData();
        const auto &cooked = DefaultContent().simulation_rules;
        data.skills[static_cast<std::size_t>(hs::SkillKind::ChargedShot)] =
            cooked.skills[static_cast<std::size_t>(hs::SkillKind::ChargedShot)];
        data.enemies[0].health = 1'000;
        data.enemies[0].move_speed = 0.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({151 + hold_ticks}, data).Succeeded(),
              "charged formula initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
        for (const auto upgrade : upgrades)
            Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
                  static_cast<std::uint64_t>(hs::SkillKind::ChargedShot), upgrade);
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {3.0f, 0.0f});
        hs::HeldInputState held;
        held.aim_world = {20.0f, 0.0f, 0.0f};
        hs::Sequence sequence{};
        if (hold_ticks == 0)
        {
            const std::array edges{
                hs::ActionEdge{++sequence, hs::GameAction::SkillQ, hs::EdgeKind::Pressed},
                hs::ActionEdge{++sequence, hs::GameAction::SkillQ, hs::EdgeKind::Released}};
            hs::InputFrame input{simulation.GetObservation().tick + 1, held, edges};
            (void)simulation.TickFixed(input, kFixedStep);
        }
        else
        {
            (void)TickEdge(simulation, hs::GameAction::SkillQ,
                           hs::EdgeKind::Pressed, sequence, held);
            for (std::uint32_t tick = 0; tick < hold_ticks; ++tick)
                (void)Tick(simulation, held);
            (void)TickEdge(simulation, hs::GameAction::SkillQ,
                           hs::EdgeKind::Released, sequence, held);
        }
        for (std::uint32_t tick = 0; tick < 90; ++tick)
            (void)Tick(simulation, held);
        const auto damage = simulation.GetObservation().damage_by_skill[
            static_cast<std::size_t>(hs::SkillKind::ChargedShot)];
        const auto upgrade_damage = simulation.GetObservation().balance.upgrade_damage[
            static_cast<std::size_t>(hs::SkillKind::ChargedShot)];
        Check(simulation.Shutdown().Succeeded(), "charged formula shutdown");
        return std::pair{damage, upgrade_damage};
    };
    Check(fire(0).first == 20, "charged shot immediate damage is 200 percent");
    Check(fire(29).first == 35, "charged shot midpoint damage is 350 percent");
    Check(fire(60).first == 50, "charged shot full damage is 500 percent");
    const std::array extended_upgrade{std::uint8_t{0}};
    const std::array minimum_upgrade{std::uint8_t{2}};
    Check(fire(84, extended_upgrade).first == 60,
          "charged shot extended full damage is 600 percent");
    const auto upgraded = fire(0, minimum_upgrade);
    Check(upgraded.first == 50 && upgraded.second[2] == 30,
          "charged shot minimum upgrade applies damage and amplifier at zero charge");
}

void TestChargedShotCancelsForLevelSelection()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({131}, QuietGameData()).Succeeded(),
          "charged level-up initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Debug(simulation, hs::DebugCommandKind::GrantExperience, 20);
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::CardSelection &&
              simulation.GetObservation().cooldown_ticks[2] == 0 &&
              simulation.GetObservation().player_projectile_count == 0,
          "level selection cancels an active charge without firing or cooldown");
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released,
                   sequence, held);
    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::SelectCard, 0}).Succeeded(),
          "charged level-up card selection");
    Debug(simulation, hs::DebugCommandKind::AssignStat,
          static_cast<std::uint64_t>(hs::StatKind::AttackPower));
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[2] > 0,
          "charged shot can start normally after level selection");
    Check(simulation.Shutdown().Succeeded(), "charged level-up shutdown");
}

void TestCombatPresentationContracts()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({19}, QuietGameData()).Succeeded(),
          "presentation initialize");

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    hs::RenderSnapshotStorage snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(simulation, snapshot), "attack snapshot");
    Check(!snapshot.View().poses.empty(), "basic attack writes an animation pose");
    Check(snapshot.View().poses.front().upper_body_clip ==
              hs::CharacterAnimationClip::Recoil,
          "basic attack selects the recoil clip");
    Check(snapshot.View().poses.front().upper_body_weight > 0.0f &&
              snapshot.View().poses.front().upper_body_weight < 1.0f,
          std::format("basic attack blend weight is {} before the release marker",
                      snapshot.View().poses.front().upper_body_weight));

    for (std::uint32_t tick = 0; tick < 23; ++tick) (void)Tick(simulation, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "pre-release attack snapshot");
    Check(std::ranges::none_of(snapshot.View().instances,
                              [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::PlayerProjectile;
          }), "basic arrow waits for the animation release marker");
    (void)Tick(simulation, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "release attack snapshot");
    Check(std::ranges::any_of(snapshot.View().instances,
                             [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::PlayerProjectile;
          }), "first basic arrow is visible on its release snapshot");
    Check(std::ranges::none_of(
              simulation.PendingDomainSignals(),
              [](const hs::DomainSignal &event) {
                  return event.kind == hs::DomainSignalKind::BasicAttackImpact;
              }),
          "basic attack has no release VFX");
    const auto arrow = std::ranges::find_if(
        snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::PlayerProjectile;
        });
    hs::GameReadModelStorage release_model;
    simulation.WriteReadModel(release_model);
    Check(arrow != snapshot.View().instances.end() &&
              !release_model.View().projectiles.empty() &&
              std::abs(arrow->position.x -
                       release_model.View().projectiles.front().position.x) < 0.0001f &&
              std::abs(arrow->position.z -
                       release_model.View().projectiles.front().position.y) < 0.0001f,
          "basic arrow visual center matches its collision center on release");

    held.basic_attack_held = false;
    for (std::uint32_t tick = 0; tick < 44; ++tick)
    {
        (void)Tick(simulation, held);
    }
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "late attack clip snapshot");
    Check(snapshot.View().poses.front().upper_body_normalized_time *
                  snapshot.View().poses.front().upper_body_playback_rate < 1.0f &&
              snapshot.View().poses.front().upper_body_weight == 1.0f,
          "attack remains fully weighted until the recoil clip completes");

    (void)Tick(simulation, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "completed attack clip snapshot");
    Check(std::abs(snapshot.View().poses.front().upper_body_normalized_time *
                       snapshot.View().poses.front().upper_body_playback_rate -
                   1.0f) < 0.0001f &&
              snapshot.View().poses.front().upper_body_weight == 1.0f,
          "attack reaches its final pose before blending out");

    (void)Tick(simulation, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "attack blend-out snapshot");
    Check(snapshot.View().poses.front().upper_body_normalized_time *
                  snapshot.View().poses.front().upper_body_playback_rate >= 1.0f &&
              snapshot.View().poses.front().upper_body_weight < 1.0f,
          "attack blend-out starts from the completed pose");

    for (std::uint32_t tick = 0; tick < 5; ++tick) (void)Tick(simulation, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "between attacks snapshot");
    Check(snapshot.View().poses.front().upper_body_weight == 0.0f,
          "basic attack upper-body action completes instead of looping while held");

    hs::GameSimulation hit_simulation;
    Check(hit_simulation.Initialize({20}, QuietGameData()).Succeeded(),
          "basic hit VFX initialize");
    Debug(hit_simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {2.0f, 0.0f});
    held.basic_attack_held = true;
    for (std::uint32_t tick = 0; tick < 60; ++tick) (void)Tick(hit_simulation, held);
    Check(std::ranges::any_of(
              hit_simulation.PendingDomainSignals(),
              [](const hs::DomainSignal &event) {
                  return event.kind == hs::DomainSignalKind::BasicAttackImpact &&
                         event.position.y > 0.5f;
              }),
          "basic attack hit VFX is emitted at the target");
    Check(hit_simulation.Shutdown().Succeeded(), "basic hit VFX shutdown");

    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
    hs::Sequence sequence{};
    held.basic_attack_held = false;
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "charge telegraph snapshot");
    Check(std::ranges::any_of(snapshot.View().persistent_vfx, [](const auto &visual) {
              return visual.kind == hs::PersistentVfxKind::ChargeGuide &&
                     visual.length >= 4.2f && visual.length < 5.0f &&
                     visual.stable_id ==
                         (1ull << 60 | static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
          }), "charged shot renders a stable white range guide");
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released,
                   sequence, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "charged release snapshot");
    Check(std::abs(snapshot.View().poses.front().upper_body_playback_rate -
                   41.0f / 9.0f) < 0.0001f,
          "skill recoil playback fits the charged-shot recovery delay");
    Check(simulation.Shutdown().Succeeded(), "presentation shutdown");
}

void TestAttackSpeedAnimationRate()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({83}, QuietGameData()).Succeeded(),
          "attack-speed animation initialize");
    for (std::uint32_t level = 0; level < 10; ++level)
    {
        const auto before = simulation.GetObservation();
        Debug(simulation, hs::DebugCommandKind::GrantExperience,
              before.experience_to_next - before.experience);
        (void)Tick(simulation);
        Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::SelectCard, 0}).Succeeded(),
              "attack-speed level-up card selection");
        while (simulation.GetObservation().phase == hs::SessionPhase::StatAllocation)
        {
            const auto stat = simulation.GetObservation().stat_points[
                                  static_cast<std::size_t>(hs::StatKind::AttackSpeed)] < 10
                                  ? hs::StatKind::AttackSpeed
                                  : hs::StatKind::AttackPower;
            Debug(simulation, hs::DebugCommandKind::AssignStat,
                  static_cast<std::uint64_t>(stat));
        }
    }
    Check(simulation.GetObservation().stat_points[
              static_cast<std::size_t>(hs::StatKind::AttackSpeed)] == 10,
          "attack speed reaches level ten");

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_projectile_count == 0,
          "level-ten attack waits for the animation release marker");
    hs::RenderSnapshotStorage snapshot(128, 4, 2, 64);
    Check(WriteSnapshot(simulation, snapshot), "level-ten attack snapshot");
    hs::GameReadModelStorage model;
    simulation.WriteReadModel(model);
    const auto attack_interval = static_cast<std::uint32_t>(
        std::floor(60.0f / model.View().effective_attack_speed + 0.5f));
    Check(std::abs(snapshot.View().poses.front().upper_body_playback_rate -
                   41.0f / static_cast<float>(attack_interval - 6)) < 0.0001f,
          "level-ten authored attack speed scales the recoil animation");
    const auto release_ticks =
        (14u * (attack_interval - 6u) + 41u / 2u) / 41u;
    for (std::uint32_t tick = 1; tick < release_ticks; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_projectile_count == 0,
          "level-ten attack remains pending before the scaled release marker");
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().player_projectile_count == 1,
          "level-ten attack fires on the scaled animation release marker");
    Check(simulation.Shutdown().Succeeded(), "attack-speed animation shutdown");
}

void TestArrowRainPulseAudioProjection()
{
    hs::DomainSignal signal;
    signal.kind = hs::DomainSignalKind::ArrowRainPulse;
    std::array<hs::PresentationEvent, 3> projected{};
    const auto count = hs::ProjectDomainSignal(signal, projected);
    Check(count == 3 && projected[0].kind == hs::PresentationKind::Vfx &&
              projected[1].kind == hs::PresentationKind::Audio &&
              projected[1].asset.value ==
                  hs::MakeAssetId("audio.skill.arrow_rain.incoming").value &&
              projected[2].kind == hs::PresentationKind::Audio &&
              projected[2].asset.value ==
                  hs::MakeAssetId("audio.skill.arrow_rain.impact").value,
          "every arrow rain pulse projects falling and impact audio");
}

void TestEnemyDisplacementInterpolates()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1'000;
    data.enemies[0].move_speed = 0.0f;
    data.enemies[0].damage = 0;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({231}, data).Succeeded(),
          "enemy displacement initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot), 5);
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);

    float first_moved_x{};
    float final_x{5.0f};
    for (std::uint32_t tick = 0; tick < 60; ++tick)
    {
        (void)Tick(simulation, held);
        hs::RenderSnapshotStorage snapshot(64, 2, 2, 16);
        Check(WriteSnapshot(simulation, snapshot), "enemy displacement snapshot");
        const auto enemy = std::ranges::find_if(
            snapshot.View().instances, [](const hs::RenderInstance &instance) {
                return instance.mesh == hs::RenderMesh::Enemy;
            });
        if (enemy == snapshot.View().instances.end()) continue;
        final_x = enemy->position.x;
        if (first_moved_x == 0.0f && final_x > 5.001f) first_moved_x = final_x;
    }
    Check(first_moved_x > 5.0f && first_moved_x < 5.5f,
          "push begins with a partial displacement instead of teleporting");
    Check(std::abs(final_x - 8.0f) < 0.001f,
          "interpolated push preserves the requested total distance");
    Check(simulation.Shutdown().Succeeded(), "enemy displacement shutdown");
}

void TestArrowRainTrackingProjectileMoves()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({121}, QuietGameData()).Succeeded(),
          "arrow rain tracking initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ArrowRain));
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::ArrowRain), 4);
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {16.0f, 0.0f});

    hs::HeldInputState held;
    held.aim_world = {10.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    hs::RenderSnapshotStorage pending(64, 2, 2, 8);
    Check(WriteSnapshot(simulation, pending), "arrow rain pre-active area snapshot");
    const auto pending_range = std::ranges::find_if(
        pending.View().persistent_vfx, [](const hs::PersistentVfxVisual &visual) {
        return visual.kind == hs::PersistentVfxKind::ArrowRainArea;
        });
    Check(pending_range != pending.View().persistent_vfx.end() &&
              pending_range->radius > 0.0f && pending_range->stable_id != 0,
          "pre-active arrow rain shows its stable persistent area");
    const auto pending_radius = pending_range == pending.View().persistent_vfx.end()
                                    ? 0.0f
                                    : pending_range->radius;
    const auto pending_stable_id = pending_range == pending.View().persistent_vfx.end()
                                       ? 0ull
                                       : pending_range->stable_id;
    for (std::uint32_t tick = 0;
         tick < 180 && simulation.GetObservation().player_projectile_count == 0; ++tick)
        (void)Tick(simulation, held);

    hs::RenderSnapshotStorage first(64, 2, 2, 8);
    Check(WriteSnapshot(simulation, first), "arrow rain tracking first snapshot");
    const auto arrow = std::ranges::find_if(
        first.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::PlayerProjectile;
        });
    Check(arrow != first.View().instances.end(), "arrow rain creates tracking arrow");
    const auto first_position = arrow->position;
    const auto active_area = std::ranges::find_if(
        first.View().persistent_vfx, [](const hs::PersistentVfxVisual &visual) {
            return visual.kind == hs::PersistentVfxKind::ArrowRainArea;
        });
    Check(active_area != first.View().persistent_vfx.end() &&
              active_area->stable_id == pending_stable_id &&
              std::abs(active_area->radius - pending_radius) < 0.0001f,
          "arrow rain keeps the same persistent area through activation");

    (void)Tick(simulation, held);
    hs::RenderSnapshotStorage second(64, 2, 2, 8);
    Check(WriteSnapshot(simulation, second), "arrow rain tracking second snapshot");
    const auto moved = std::ranges::find_if(
        second.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::PlayerProjectile;
        });
    Check(moved != second.View().instances.end() &&
              (std::abs(moved->position.x - first_position.x) > 0.1f ||
               std::abs(moved->position.z - first_position.z) > 0.1f),
          "arrow rain tracking arrow advances instead of standing still");
    hs::GameReadModelStorage second_model;
    simulation.WriteReadModel(second_model);
    const auto current_projectile = std::ranges::find_if(
        second_model.View().projectiles, [](const hs::ProjectileView &projectile) {
            return projectile.player_owned;
        });
    Check(moved != second.View().instances.end() &&
              current_projectile != second_model.View().projectiles.end() &&
              std::abs(moved->yaw - std::atan2(current_projectile->velocity.x,
                                               current_projectile->velocity.y)) < 0.0001f,
          "tracking arrow orientation follows its current velocity");
    Check(simulation.Shutdown().Succeeded(), "arrow rain tracking shutdown");
}

void TestProjectileAndAreaVisualTruth()
{
    hs::GameReadModelStorage model;
    model.tick = 30;
    constexpr std::array projectile_skills{
        hs::SkillKind::BasicAttack, hs::SkillKind::PiercingShot,
        hs::SkillKind::MultiShot, hs::SkillKind::ChargedShot,
        hs::SkillKind::ChargedShot,
        hs::SkillKind::ExplosiveArrow, hs::SkillKind::RicochetArrow};
    constexpr std::array collision_radii{0.12f, 0.2f, 0.09f, 0.4f,
                                         0.88f, 0.35f, 0.16f};
    constexpr std::array charge_ratios{0.0f, 0.0f, 0.0f, 0.0f,
                                        1.0f, 0.0f, 0.0f};
    for (std::size_t index = 0; index < projectile_skills.size(); ++index)
        model.AddProjectile({hs::EntityId{101 + index}, true,
                             {1.0f + static_cast<float>(index), 2.0f},
                             {4.0f, 0.0f}, 30, projectile_skills[index], false,
                             collision_radii[index], charge_ratios[index]});
    hs::AreaView area;
    area.id = hs::EntityId{201};
    area.kind = hs::AreaViewKind::Damage;
    area.position = {5.0f, 6.0f};
    area.radius = 3.0f;
    area.active_tick = 20;
    area.expires = 120;
    area.skill = hs::SkillKind::ArrowRain;
    area.origin = hs::EffectOrigin::Derived;
    area.applies_slow = true;
    model.AddArea(area);

    hs::RenderSnapshotStorage snapshot(32, 2, 2, 16);
    static const hs::SettingsData settings;
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot),
          "projectile and area truth snapshot");
    constexpr std::array expected_sizes{
        hs::Float3{0.24f, 0.24f, 0.825f}, hs::Float3{0.28f, 0.28f, 1.25f},
        hs::Float3{0.18f, 0.18f, 0.7f}, hs::Float3{0.228f, 0.228f, 0.81f},
        hs::Float3{0.42f, 0.42f, 1.26f},
        hs::Float3{0.34f, 0.34f, 1.0f}, hs::Float3{0.25f, 0.25f, 0.85f}};
    std::size_t projectile_count{};
    for (std::size_t index = 0; index < expected_sizes.size(); ++index)
    {
        const auto stable_id = (3ull << 60u) | (101u + index);
        const auto instance = std::ranges::find_if(
            snapshot.View().instances, [stable_id](const hs::RenderInstance &candidate) {
                return candidate.mesh == hs::RenderMesh::PlayerProjectile &&
                       candidate.stable_id == stable_id;
            });
        Check(instance != snapshot.View().instances.end() &&
                  std::abs(instance->scale.x - expected_sizes[index].x) < 0.0001f &&
                  std::abs(instance->scale.y - expected_sizes[index].y) < 0.0001f &&
                  std::abs(instance->scale.z - expected_sizes[index].z) < 0.0001f,
              "each arrow uses its skill-specific visual size");
        projectile_count += instance != snapshot.View().instances.end();
    }
    Check(projectile_count == expected_sizes.size() &&
              std::ranges::equal(model.View().projectiles, collision_radii,
                                 {}, &hs::ProjectileView::radius),
          "arrow visual sizes are independent from collision radii");
    Check(std::ranges::any_of(snapshot.View().persistent_vfx,
                              [](const hs::PersistentVfxVisual &visual) {
              return visual.kind == hs::PersistentVfxKind::ArrowRainArea &&
                     std::abs(visual.radius - 3.0f) < 0.0001f;
          }), "derived arrow rain keeps its actual three-metre boundary");
    Check(std::ranges::any_of(snapshot.View().persistent_vfx,
                              [](const hs::PersistentVfxVisual &visual) {
              return visual.kind == hs::PersistentVfxKind::SlowArea &&
                     std::abs(visual.radius - 3.0f) < 0.0001f;
          }), "slowing arrow rain adds a shape-distinct slow motif");
    Check(std::ranges::any_of(snapshot.View().persistent_vfx,
                              [](const hs::PersistentVfxVisual &visual) {
                  return visual.kind == hs::PersistentVfxKind::ProjectileTrailOuter &&
                         std::abs(visual.radius - 0.075f) < 0.0001f;
              }), "charged arrow adds a separate low-opacity outer trail");
    Check(std::ranges::any_of(snapshot.View().persistent_vfx,
                              [](const hs::PersistentVfxVisual &visual) {
                  return visual.kind ==
                         hs::PersistentVfxKind::RicochetProjectileTrail;
              }), "ricochet arrow uses its continuous ribbon trail");

    model.tick = area.expires;
    snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot),
          "expired area snapshot projects successfully");
    Check(std::ranges::none_of(
              snapshot.View().persistent_vfx,
              [](const hs::PersistentVfxVisual &visual) {
                  return (visual.stable_id >> 60u) == 4u;
              }),
          "expired area stops rendering at its explicit expiry tick");

    model.Clear();
    snapshot.Clear();
    model.tick = 10;
    hs::AreaView preactive_area;
    preactive_area.id = hs::EntityId{901};
    preactive_area.kind = hs::AreaViewKind::EnemyDamage;
    preactive_area.position = {2.0f, 3.0f};
    preactive_area.radius = 2.0f;
    preactive_area.active_tick = 20;
    preactive_area.expires = 40;
    model.AddArea(preactive_area);
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot),
          "pre-active area snapshot projects successfully");
    Check(std::ranges::none_of(
              snapshot.View().instances,
              [](const hs::RenderInstance &instance) {
                  return instance.stable_id == (4ull << 60u | 901ull);
              }),
          "ordinary area stays hidden before its active tick");

    model.Clear();
    snapshot.Clear();
    model.tick = 30;
    area.kind = hs::AreaViewKind::Slow;
    area.source_upgrade = 7;
    model.AddArea(area);
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot) &&
              std::ranges::any_of(snapshot.View().persistent_vfx,
                                  [](const hs::PersistentVfxVisual &visual) {
                  return visual.kind == hs::PersistentVfxKind::SlowArea;
              }) &&
              std::ranges::any_of(snapshot.View().persistent_vfx,
                                  [](const hs::PersistentVfxVisual &visual) {
                  return visual.kind == hs::PersistentVfxKind::ArrowRainArea;
              }),
          "finished arrow rain keeps its persistent area and slow follow-up shapes");

    model.Clear();
    snapshot.Clear();
    model.tick = 10;
    model.player.charging = true;
    model.player.charging_skill = hs::SkillKind::ChargedShot;
    model.player.charge_start = 0;
    model.player.position = {5.0f, -3.0f};
    constexpr hs::Float2 target{17.0f, 5.0f};
    const auto aim_length = std::hypot(target.x - model.player.position.x,
                                       target.y - model.player.position.y);
    model.player.aim = {(target.x - model.player.position.x) / aim_length,
                        (target.y - model.player.position.y) / aim_length};
    model.charge_range = 10.0f;
    model.charge_radius = 0.88f;
    model.skills[static_cast<std::size_t>(hs::SkillKind::ChargedShot)]
        .effective_range = 10.0f;
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot) &&
              std::ranges::any_of(snapshot.View().persistent_vfx,
                                  [&model](const hs::PersistentVfxVisual &visual) {
                  return visual.kind == hs::PersistentVfxKind::ChargeGuide &&
                         std::abs(visual.radius - 0.88f) < 0.0001f &&
                         std::abs(visual.length - 10.0f) < 0.0001f &&
                         std::abs(visual.position.x -
                                  (model.player.position.x + model.player.aim.x * 5.0f)) <
                             0.0001f &&
                         std::abs(visual.position.z -
                                  (model.player.position.y + model.player.aim.y * 5.0f)) <
                             0.0001f &&
                         std::abs(visual.yaw -
                                  std::atan2(model.player.aim.x, model.player.aim.y)) <
                             0.0001f;
              }),
          "charge guide follows a target distinct from the player");

    model.player.charging = false;
    model.Clear();
    snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot) &&
              snapshot.View().persistent_vfx.empty(),
          "persistent area visuals disappear with their gameplay areas");
}

void TestMultiShotCastVfxIsPerFan()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x564658u}, QuietGameData()).Succeeded(),
          "multishot VFX initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot));
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot), 0);
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot), 3);
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    std::size_t original_fan_casts{};
    for (std::uint32_t tick = 0; tick < 30 && original_fan_casts == 0; ++tick)
    {
        (void)Tick(simulation, held);
        original_fan_casts =
            CountVfx(simulation, hs::DomainSignalKind::MultiShotCast);
    }
    Check(original_fan_casts == 1,
          "multishot emits exactly one cast for the original fan");
    for (std::uint32_t tick = 0; tick < 30; ++tick) (void)Tick(simulation, held);
    Check(CountVfx(simulation, hs::DomainSignalKind::MultiShotCast) == 1,
          "derived multishot volleys do not emit a cast VFX");
    Check(simulation.GetObservation().player_projectile_count == 17,
          "multishot VFX deduplication does not change projectile count");
    Check(simulation.Shutdown().Succeeded(), "multishot VFX shutdown");
}

void TestPiercingDamageTrailMatchesArrowPath()
{
    auto data = QuietGameData();
    data.enemies[0].health = 100;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({122}, data).Succeeded(),
          "piercing trail initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot), 3);
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {10.0f, 0.0f});
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {10.0f, 2.0f});

    hs::HeldInputState held;
    held.aim_world = {24.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().damage_by_skill[
              static_cast<std::size_t>(hs::SkillKind::PiercingShot)] == 4,
          "one-meter trail damages only enemies on the arrow path");
    for (std::uint32_t tick = 0;
         tick < 30 && simulation.GetObservation().player_projectile_count == 0; ++tick)
        (void)Tick(simulation, held);

    hs::RenderSnapshotStorage snapshot(64, 2, 2, 8);
    Check(WriteSnapshot(simulation, snapshot), "piercing trail snapshot");
    Check(std::ranges::any_of(snapshot.View().persistent_vfx,
                              [](const auto &visual) {
              return visual.kind == hs::PersistentVfxKind::DamageTrail &&
                     std::abs(visual.position.x - 12.0f) < 0.0001f &&
                     std::abs(visual.radius - 0.5f) < 0.0001f &&
                     std::abs(visual.length - 24.0f) < 0.0001f;
          }), "piercing trail visual matches its 24 by 1 meter damage path");
    const auto projectile_trail = std::ranges::find_if(
        snapshot.View().persistent_vfx, [](const hs::PersistentVfxVisual &visual) {
            return visual.kind == hs::PersistentVfxKind::ProjectileTrail;
        });
    const auto projectile = projectile_trail == snapshot.View().persistent_vfx.end()
                                ? snapshot.View().instances.end()
                                : std::ranges::find_if(
                                      snapshot.View().instances,
                                      [stable_id = projectile_trail->stable_id](
                                          const hs::RenderInstance &instance) {
                                          return instance.mesh ==
                                                     hs::RenderMesh::PlayerProjectile &&
                                                 instance.stable_id == stable_id;
                                      });
    Check(projectile_trail != snapshot.View().persistent_vfx.end() &&
              projectile != snapshot.View().instances.end() &&
              projectile_trail->length >= 0.35f && projectile_trail->length <= 1.0f,
          "piercing projectile trail uses a short stable segment");
    if (projectile != snapshot.View().instances.end() &&
        projectile_trail != snapshot.View().persistent_vfx.end())
    {
        const auto direction = hs::Float2{std::sin(projectile->yaw),
                                          std::cos(projectile->yaw)};
        const auto offset = hs::Float2{
            projectile_trail->position.x - projectile->position.x,
            projectile_trail->position.z - projectile->position.z};
        const auto trail_front = hs::Float2{
            projectile_trail->position.x + direction.x * projectile_trail->length * 0.5f,
            projectile_trail->position.z + direction.y * projectile_trail->length * 0.5f};
        const auto body_rear = hs::Float2{
            projectile->position.x - direction.x * projectile->scale.z * 0.5f,
            projectile->position.z - direction.y * projectile->scale.z * 0.5f};
        Check(offset.x * direction.x + offset.y * direction.y < 0.0f &&
                  std::hypot(trail_front.x - body_rear.x,
                             trail_front.y - body_rear.y) < 0.0001f &&
                  std::abs(projectile_trail->position.y -
                           (projectile->position.y + projectile->scale.y * 0.5f)) < 0.0001f,
              "piercing projectile trail begins at the rear and matches GPU body height");
    }
    Check(simulation.Shutdown().Succeeded(), "piercing trail shutdown");
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
            return instance.mesh == hs::RenderMesh::EnemyRanged;
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

void TestTrapRollsForwardAndLeavesOriginTrap()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({24}, QuietGameData()).Succeeded(), "trap roll initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::Trap));
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().player_position.x > 0.0f,
          "trap skill starts a forward forced move");
    Check(std::ranges::any_of(
              simulation.PendingDomainSignals(), [](const auto &event) {
                  return event.kind == hs::DomainSignalKind::TrapCast &&
                         std::abs(event.position.x) < 0.01f;
              }),
          "trap effect remains at the roll origin");
    hs::RenderSnapshotStorage snapshot(64, 4, 2, 64);
    Check(WriteSnapshot(simulation, snapshot), "immediate trap snapshot");
    const auto placed_trap = std::ranges::find_if(
        snapshot.View().persistent_vfx, [](const auto &visual) {
            return visual.kind == hs::PersistentVfxKind::TrapArmed &&
                   std::abs(visual.position.x) < 0.01f;
        });
    Check(placed_trap != snapshot.View().persistent_vfx.end(),
          "trap uses its armed persistent visual on the placement tick");
    const auto trap_stable_id = placed_trap == snapshot.View().persistent_vfx.end()
                                    ? 0ull
                                    : placed_trap->stable_id;
    const auto trap_radius = placed_trap == snapshot.View().persistent_vfx.end()
                                 ? 0.0f
                                 : placed_trap->radius;
    for (std::uint32_t tick = 0; tick < 40; ++tick) (void)Tick(simulation, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "armed trap snapshot");
    Check(std::ranges::any_of(
              snapshot.View().persistent_vfx, [trap_stable_id, trap_radius](const auto &visual) {
                  return visual.kind == hs::PersistentVfxKind::TrapArmed &&
                         visual.stable_id == trap_stable_id &&
                         std::abs(visual.radius - trap_radius) < 0.0001f;
              }),
          "trap keeps its armed persistent visual through activation");
    Check(simulation.Shutdown().Succeeded(), "trap roll shutdown");
}

void TestCombatUpgradeCombinations()
{
    std::size_t total{};
    for (std::uint8_t skill = 0; skill < hs::kCombatSkillCount; ++skill)
    {
        const auto count = ForEachFourOfEight([skill](const auto &upgrades) {
            ExerciseCombatCombination(static_cast<hs::SkillKind>(skill), upgrades);
        });
        Check(count == 70, "C(8,4) count per combat target");
        total += count;
    }
    Check(total == 630, "nine combat targets produce 630 combinations");
}

void TestRelicCombinations()
{
    std::size_t singles{};
    for (std::uint8_t relic = 0; relic < hs::kRelicCount; ++relic)
    {
        const std::array set{relic};
        ExerciseRelics(set);
        ++singles;
    }
    Check(singles == 12, "twelve standalone relics");

    std::size_t pairs{};
    for (std::uint8_t first = 0; first < hs::kRelicCount; ++first)
    {
        for (std::uint8_t second = first + 1; second < hs::kRelicCount; ++second)
        {
            const std::array set{first, second};
            ExerciseRelics(set);
            ++pairs;
        }
    }
    Check(pairs == 66, "C(12,2) relic pairs");
}

void TestAlternatingSkillRelicTelemetry()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({211}, QuietGameData()).Succeeded(),
          "alternating relic telemetry initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot));
    Debug(simulation, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::AlternatingSkills));
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    for (std::uint32_t tick = 0; tick < 60; ++tick) (void)Tick(simulation, held);
    (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Pressed,
                   sequence, held);
    const auto relic = static_cast<std::size_t>(hs::RelicKind::AlternatingSkills);
    const auto &effects = simulation.GetObservation().balance.relic_effects[relic];
    Check(effects[static_cast<std::size_t>(hs::UpgradeEffectMetric::Activations)] == 1 &&
              effects[static_cast<std::size_t>(
                  hs::UpgradeEffectMetric::CooldownTicksSaved)] > 0,
          "alternating relic records activation and actual cooldown refund");
    Check(simulation.Shutdown().Succeeded(),
          "alternating relic telemetry shutdown");
}

void TestRemadeRelics()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.enemies[0].move_speed = 0.0f;
    data.skills[static_cast<std::size_t>(hs::SkillKind::ArrowRain)]
        .cooldown_ticks = 6'000;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({223}, data).Succeeded(),
          "remade relic initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ArrowRain));
    Debug(simulation, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::KillCooldownSurge));
    Debug(simulation, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::CombatHitChain));

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    for (std::uint32_t tick = 0; tick < 20; ++tick) (void)Tick(simulation, held);
    for (std::uint32_t enemy = 0; enemy < 20; ++enemy)
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0,
              {3.0f + static_cast<float>(enemy) * 0.6f, 0.0f});
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    for (std::uint32_t tick = 0;
         tick < 1'200 && simulation.GetObservation().kills < 20; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.GetObservation().kills == 20, "remade relic test kills twenty enemies");

    const auto &balance = simulation.GetObservation().balance;
    const auto activation = static_cast<std::size_t>(
        hs::UpgradeEffectMetric::Activations);
    const auto cooldown = static_cast<std::size_t>(
        hs::UpgradeEffectMetric::CooldownTicksSaved);
    const auto extra_targets = static_cast<std::size_t>(
        hs::UpgradeEffectMetric::ExtraTargetsHit);
    const auto surge = static_cast<std::size_t>(hs::RelicKind::KillCooldownSurge);
    const auto chain = static_cast<std::size_t>(hs::RelicKind::CombatHitChain);
    Check(balance.relic_effects[surge][activation] == 2 &&
              balance.relic_effects[surge][cooldown] > 0,
          "kill cooldown relic triggers twice and records actual saved cooldown");
    Check(balance.relic_effects[chain][activation] == 1 &&
              balance.relic_effects[chain][extra_targets] > 0,
          "combat chain reaches additional targets after twelve direct hits");
    Check(simulation.Shutdown().Succeeded(), "remade relic shutdown");

    hs::GameSimulation revive;
    Check(revive.Initialize({227}, QuietGameData()).Succeeded(),
          "revive relic initialize");
    Debug(revive, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::OnceRevive));
    Debug(revive, hs::DebugCommandKind::DamagePlayer, 1'000);
    (void)Tick(revive);
    Check(revive.GetObservation().phase == hs::SessionPhase::Playing &&
              revive.GetObservation().health == 50,
          "fatal damage revives once at half maximum health");
    Debug(revive, hs::DebugCommandKind::DamagePlayer, 1'000);
    (void)Tick(revive);
    Check(revive.GetObservation().phase == hs::SessionPhase::Defeat,
          "revive relic cannot trigger a second time");
    Check(revive.Shutdown().Succeeded(), "revive relic shutdown");
}

void TestUpgradeDamageAttribution()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1'000;
    data.enemies[0].move_speed = 0.0f;
    data.enemies[0].damage = 0;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({193}, data).Succeeded(),
          "upgrade damage attribution initialize");
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 0);
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    for (std::uint32_t tick = 0; tick < 240; ++tick) (void)Tick(simulation, held);
    const auto probe = simulation.GetObservation();
    Check(probe.balance.upgrade_damage[0][0] > 0 &&
              probe.balance.upgrade_damage[0][0] < probe.damage_by_skill[0],
          "extra-arrow damage is separated from the basic attack's base damage");
    Check(probe.balance.upgrade_effects[0][0][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::ProjectilesCreated)] > 0,
          "upgrade telemetry records the extra projectiles that actually spawned");
    const auto extra_attacks = probe.balance.upgrade_effects[0][0][
        static_cast<std::size_t>(hs::UpgradeEffectMetric::ProjectilesCreated)];
    Check(probe.balance.skill_uses[0] >= extra_attacks + 4,
          "extra basic arrows count as basic attacks without recursively spawning themselves");
    Check(probe.balance.skill_casts_with_hit[0] > 0 &&
              probe.balance.skill_casts_with_hit[0] <= probe.balance.skill_uses[0],
          "one cast contributes at most one cast-hit regardless of derived projectiles");
    Check(simulation.Shutdown().Succeeded(), "upgrade damage attribution shutdown");

    hs::GameSimulation interaction;
    Check(interaction.Initialize({194}, data).Succeeded(),
          "conditional upgrade attribution initialize");
    Debug(interaction, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(interaction, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 5);
    Debug(interaction, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 7);
    Debug(interaction, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    Debug(interaction, hs::DebugCommandKind::SpawnEnemy, 0, 0, {4.83f, 1.29f});
    Debug(interaction, hs::DebugCommandKind::SpawnEnemy, 0, 0, {4.83f, -1.29f});
    hs::Sequence sequence{};
    hs::HeldInputState interaction_held;
    interaction_held.aim_world = {20.0f, 0.0f, 0.0f};
    (void)TickEdge(interaction, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, interaction_held);
    for (std::uint32_t tick = 0; tick < 20; ++tick)
        (void)Tick(interaction, interaction_held);
    interaction_held.basic_attack_held = true;
    for (std::uint32_t tick = 0; tick < 100; ++tick)
        (void)Tick(interaction, interaction_held);
    const auto interaction_probe = interaction.GetObservation();
    Check(interaction_probe.balance.upgrade_damage[0][7] > 0 &&
              interaction_probe.balance.upgrade_damage[0][7] <
                  interaction_probe.damage_by_skill[0],
          "empowered basic attack attributes only the two additional arrows");
    Check(interaction_probe.balance.upgrade_effects[0][5][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::CooldownTicksSaved)] > 0,
          "slow-hit upgrade records the cooldown ticks actually removed");
    Check(interaction.Shutdown().Succeeded(),
          "conditional upgrade attribution shutdown");

    hs::GameSimulation independent;
    Check(independent.Initialize({195}, data).Succeeded(),
          "independent upgrade behavior initialize");
    Debug(independent, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(independent, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot));
    Debug(independent, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow));
    Debug(independent, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot), 6);
    Debug(independent, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow), 6);
    for (const auto position : {hs::Float2{4.0f, 0.0f}, hs::Float2{6.0f, 0.5f},
                                hs::Float2{8.0f, -0.5f}, hs::Float2{10.0f, 0.0f}})
        Debug(independent, hs::DebugCommandKind::SpawnEnemy, 0, 0, position);
    hs::HeldInputState independent_held;
    independent_held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence independent_sequence{};
    (void)TickEdge(independent, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   independent_sequence, independent_held);
    for (std::uint32_t tick = 0; tick < 20; ++tick)
        (void)Tick(independent, independent_held);
    (void)TickEdge(independent, hs::GameAction::SkillW, hs::EdgeKind::Pressed,
                   independent_sequence, independent_held);
    for (std::uint32_t tick = 0; tick < 20; ++tick)
        (void)Tick(independent, independent_held);
    Check(independent.GetObservation().balance.upgrade_effects[2][6][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::ProjectilesCreated)] == 2,
          "multishot seventh upgrade creates exactly two independent arrows");
    (void)TickEdge(independent, hs::GameAction::SkillE, hs::EdgeKind::Pressed,
                   independent_sequence, independent_held);
    for (std::uint32_t tick = 0; tick < 120; ++tick)
        (void)Tick(independent, independent_held);
    Check(independent.GetObservation().balance.upgrade_effects[5][6][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::CooldownTicksSaved)] > 0,
          "ricochet cooldown recovery works without the return-arrow upgrade");
    Check(independent.Shutdown().Succeeded(),
          "independent upgrade behavior shutdown");
}

void TestSelectiveUpgradeInheritance()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1'000;
    data.enemies[0].move_speed = 0.0f;
    data.enemies[0].damage = 0;

    hs::GameSimulation split;
    Check(split.Initialize({196}, data).Succeeded(), "isolated split initialize");
    Debug(split, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(split, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 2);
    Debug(split, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 5);
    for (const auto position : {hs::Float2{5.0f, 0.0f}, hs::Float2{6.8f, 1.5f},
                                hs::Float2{6.8f, -1.5f}})
        Debug(split, hs::DebugCommandKind::SpawnEnemy, 0, 0, position);
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    (void)Tick(split, held);
    held.basic_attack_held = false;
    for (std::uint32_t tick = 0; tick < 90; ++tick) (void)Tick(split, held);
    const auto split_probe = split.GetObservation();
    Check(split_probe.balance.upgrade_effects[0][2][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::ProjectilesCreated)] == 2,
          "basic split creates two arrows");
    Check(split_probe.balance.upgrade_effects[0][5][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::SlowApplications)] == 1,
          "basic split arrows do not inherit slow and cooldown effects");
    Check(split.Shutdown().Succeeded(), "isolated split shutdown");

    hs::GameSimulation snapshot_mask;
    Check(snapshot_mask.Initialize({200}, data).Succeeded(),
          "cast upgrade snapshot initialize");
    Debug(snapshot_mask, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(snapshot_mask, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    hs::Sequence snapshot_sequence{};
    (void)TickEdge(snapshot_mask, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   snapshot_sequence, held);
    Debug(snapshot_mask, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot), 2);
    for (std::uint32_t tick = 0; tick < 90; ++tick) (void)Tick(snapshot_mask, held);
    Check(snapshot_mask.GetObservation().balance.upgrade_effects[1][2][
              static_cast<std::size_t>(hs::UpgradeEffectMetric::BleedStacksApplied)] == 0,
          "scheduled effects keep the upgrade mask captured at cast time");
    Check(snapshot_mask.Shutdown().Succeeded(), "cast upgrade snapshot shutdown");

    hs::GameSimulation repeat;
    Check(repeat.Initialize({197}, data).Succeeded(), "full repeat initialize");
    Debug(repeat, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(repeat, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 0);
    Debug(repeat, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 5);
    Debug(repeat, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    hs::Sequence sequence{};
    (void)TickEdge(repeat, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    for (std::uint32_t tick = 0; tick < 20; ++tick) (void)Tick(repeat, held);
    held.basic_attack_held = true;
    for (std::uint32_t tick = 0; tick < 160; ++tick) (void)Tick(repeat, held);
    held.basic_attack_held = false;
    for (std::uint32_t tick = 0; tick < 30; ++tick) (void)Tick(repeat, held);
    const auto repeat_probe = repeat.GetObservation();
    const auto repeat_arrows = repeat_probe.balance.upgrade_effects[0][0][
        static_cast<std::size_t>(hs::UpgradeEffectMetric::ProjectilesCreated)];
    const auto slow_applications = repeat_probe.balance.upgrade_effects[0][5][
        static_cast<std::size_t>(hs::UpgradeEffectMetric::SlowApplications)];
    Check(repeat_arrows > 0 && slow_applications == repeat_probe.balance.skill_uses[0],
          "full basic repeats inherit other basic attack effects once");
    Check(repeat_probe.balance.upgrade_effects[0][5][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::CooldownTicksSaved)] > 0,
          "full basic repeats retain cooldown utility");
    Check(repeat.Shutdown().Succeeded(), "full repeat shutdown");

    hs::GameSimulation basic_reacquire;
    Check(basic_reacquire.Initialize({212}, data).Succeeded(),
          "basic miss reacquire initialize");
    Debug(basic_reacquire, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 6);
    Debug(basic_reacquire, hs::DebugCommandKind::SpawnEnemy, 0, 0,
          {17.0f, 3.0f});
    hs::HeldInputState basic_held;
    basic_held.aim_world = {20.0f, 0.0f, 0.0f};
    basic_held.basic_attack_held = true;
    (void)Tick(basic_reacquire, basic_held);
    basic_held.basic_attack_held = false;
    for (std::uint32_t tick = 0; tick < 120; ++tick)
        (void)Tick(basic_reacquire, basic_held);
    Check(basic_reacquire.GetObservation().balance.upgrade_damage[0][6] > 0,
          "missed basic arrow reacquires a nearby enemy and deals attributed damage");
    Check(basic_reacquire.Shutdown().Succeeded(),
          "basic miss reacquire shutdown");

    hs::GameSimulation miss_recovery;
    Check(miss_recovery.Initialize({205}, data).Succeeded(),
          "derived miss recovery initialize");
    Debug(miss_recovery, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot));
    Debug(miss_recovery, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot), 0);
    Debug(miss_recovery, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot), 7);
    Debug(miss_recovery, hs::DebugCommandKind::SpawnEnemy, 0, 0,
          {18.0f, 10.0f});
    hs::Sequence miss_sequence{};
    (void)TickEdge(miss_recovery, hs::GameAction::SkillQ,
                   hs::EdgeKind::Pressed, miss_sequence, held);
    for (std::uint32_t tick = 0; tick < 180; ++tick)
        (void)Tick(miss_recovery, held);
    Check(miss_recovery.GetObservation().balance.upgrade_effects[2][7][
              static_cast<std::size_t>(
                  hs::UpgradeEffectMetric::ProjectilesCreated)] > 5,
          "miss recovery applies to derived upgrade volleys");
    Check(miss_recovery.Shutdown().Succeeded(),
          "derived miss recovery shutdown");

    hs::GameSimulation mark;
    Check(mark.Initialize({206}, data).Succeeded(), "basic mark trigger initialize");
    Debug(mark, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow));
    Debug(mark, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow), 6);
    Debug(mark, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    hs::Sequence mark_sequence{};
    (void)TickEdge(mark, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   mark_sequence, held);
    for (std::uint32_t tick = 0; tick < 30; ++tick) (void)Tick(mark, held);
    const auto mark_explosions_before_basic =
        mark.GetObservation().balance.upgrade_effects[4][6][static_cast<std::size_t>(
            hs::UpgradeEffectMetric::ExplosionsCreated)];
    held.basic_attack_held = true;
    for (std::uint32_t tick = 0; tick < 120; ++tick) (void)Tick(mark, held);
    held.basic_attack_held = false;
    Check(mark.GetObservation().balance.upgrade_effects[4][6][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::ExplosionsCreated)] >
              mark_explosions_before_basic,
          "basic attack triggers a mark from another skill");
    Check(mark.Shutdown().Succeeded(), "basic mark trigger shutdown");

    data.enemies[0].health = 8;
    hs::GameSimulation small_trap;
    Check(small_trap.Initialize({198}, data).Succeeded(), "small trap initialize");
    Debug(small_trap, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::Trap));
    for (const auto upgrade : {2u, 3u, 4u, 7u})
        Debug(small_trap, hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::Trap), upgrade);
    for (const auto x : {0.0f, 8.0f})
        Debug(small_trap, hs::DebugCommandKind::SpawnEnemy, 0, 0, {x, 0.0f});
    hs::HeldInputState trap_held;
    trap_held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence trap_sequence{};
    (void)TickEdge(small_trap, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   trap_sequence, trap_held);
    for (std::uint32_t tick = 0; tick < 1'000; ++tick)
        (void)Tick(small_trap, trap_held);
    const auto trap_probe = small_trap.GetObservation();
    Check(trap_probe.kills == 2,
          "the first small trap retains its own explosion");
    Check(trap_probe.balance.upgrade_effects[7][3][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::BleedStacksApplied)] == 3 &&
              trap_probe.balance.upgrade_effects[7][4][static_cast<std::size_t>(
                  hs::UpgradeEffectMetric::BurnApplications)] == 1,
          "small traps do not inherit the full trap's bleed or burn");
    Check(trap_probe.balance.upgrade_effects[7][7][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::AreasCreated)] == 1,
          "small trap kills do not trigger the kill-trap upgrade again");
    Check(small_trap.Shutdown().Succeeded(), "small trap shutdown");

    hs::GameSimulation retreat_trap;
    Check(retreat_trap.Initialize({199}, data).Succeeded(),
          "retreat small trap initialize");
    Debug(retreat_trap, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RetreatShot));
    for (const auto upgrade : {1u, 2u, 3u, 4u})
        Debug(retreat_trap, hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::RetreatShot), upgrade);
    Debug(retreat_trap, hs::DebugCommandKind::SpawnEnemy, 0, 0, {0.0f, 1.2f});
    hs::Sequence retreat_sequence{};
    (void)TickEdge(retreat_trap, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   retreat_sequence, trap_held);
    for (std::uint32_t tick = 0; tick < 180; ++tick)
        (void)Tick(retreat_trap, trap_held);
    const auto retreat_probe = retreat_trap.GetObservation();
    std::uint64_t retreat_bleed{};
    std::uint64_t retreat_burn{};
    for (std::size_t upgrade = 0; upgrade < hs::kUpgradeCount; ++upgrade)
    {
        retreat_bleed += retreat_probe.balance.upgrade_effects[8][upgrade]
            [static_cast<std::size_t>(hs::UpgradeEffectMetric::BleedStacksApplied)];
        retreat_burn += retreat_probe.balance.upgrade_effects[8][upgrade]
            [static_cast<std::size_t>(hs::UpgradeEffectMetric::BurnApplications)];
    }
    Check(retreat_bleed == 0 && retreat_burn == 0,
          "retreat small trap does not reinterpret retreat upgrades as trap upgrades");
    Check(retreat_trap.Shutdown().Succeeded(), "retreat small trap shutdown");
}

void TestHighFanoutChainsTerminate()
{
    const auto run = [](hs::SkillKind skill, std::array<std::uint8_t, 4> upgrades,
                        std::uint64_t seed) {
        auto data = QuietGameData();
        data.enemies[0].health = 1;
        data.enemies[0].move_speed = 0.0f;
        data.enemies[0].damage = 0;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({seed}, data).Succeeded(), "chain initialize");
        Debug(simulation, hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(skill));
        for (const auto upgrade : upgrades)
            Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
                  static_cast<std::uint64_t>(skill), upgrade);
        for (std::uint32_t enemy = 0; enemy < 48; ++enemy)
        {
            const auto column = static_cast<float>(enemy % 8);
            const auto row = static_cast<float>(enemy / 8);
            Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0,
                  {3.0f + column * 2.0f, (row - 2.5f) * 2.0f});
        }
        hs::HeldInputState held;
        held.aim_world = {15.0f, 0.0f, 0.0f};
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                       sequence, held);
        for (std::uint32_t tick = 0; tick < 1'200; ++tick) (void)Tick(simulation, held);
        const auto settled = simulation.GetObservation();
        for (std::uint32_t tick = 0; tick < 180; ++tick) (void)Tick(simulation, held);
        const auto after = simulation.GetObservation();
        Check(settled.player_projectile_count == 0 &&
                  after.player_projectile_count == 0 &&
                  settled.damage_dealt == after.damage_dealt,
              "one cast settles without an infinite damage or projectile chain");
        Check(simulation.Shutdown().Succeeded(), "chain shutdown");
        return settled;
    };

    const auto ricochet = run(hs::SkillKind::RicochetArrow, {0, 1, 2, 4}, 201);
    Check(ricochet.balance.upgrade_effects[5][4][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::ProjectilesCreated)] <= 9,
          "ricochet kill arrows share the nine-projectile cap");
    const auto trap = run(hs::SkillKind::Trap, {0, 2, 3, 7}, 202);
    Check(trap.balance.upgrade_effects[7][7][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::AreasCreated)] <= 3,
          "kill traps share the three-area cap");
    const auto rain = run(hs::SkillKind::ArrowRain, {0, 3, 6, 7}, 203);
    Check(rain.balance.upgrade_effects[6][6][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::ProjectilesCreated)] <= 6,
          "arrow rain kill arrows share the six-projectile cap");
    (void)run(hs::SkillKind::ExplosiveArrow, {0, 1, 2, 4}, 204);
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
    TestUpgradeDamageAttribution();
    TestSelectiveUpgradeInheritance();
    TestHighFanoutChainsTerminate();
}

} // namespace gameplay_test
