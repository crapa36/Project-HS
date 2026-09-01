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

} // namespace

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
                return instance.mesh == hs::RenderMesh::MonsterMelee;
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
            return visual.kind == hs::PersistentVfxKind::TrapPending &&
                   std::abs(visual.position.x) < 0.01f;
        });
    Check(placed_trap != snapshot.View().persistent_vfx.end(),
          "trap uses its pending persistent visual before activation");
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

} // namespace gameplay_test
