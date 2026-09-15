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

constexpr hs::Tick kTicksPerMinute = 60 * 60;

} // namespace

void TestStartingEnemyBalanceAndBoundary()
{
    const auto data = DefaultContent().simulation_rules;
    constexpr std::array expected_handlers{
        hs::AbilityHandlerId::BasicProjectileCadence,
        hs::AbilityHandlerId::PiercingProjectile,
        hs::AbilityHandlerId::UniformFanProjectiles,
        hs::AbilityHandlerId::HoldReleaseLinearCharge,
        hs::AbilityHandlerId::ProjectileToAreaExplosion,
        hs::AbilityHandlerId::NearestUnhitTargetRicochet,
        hs::AbilityHandlerId::TargetedPeriodicArea,
        hs::AbilityHandlerId::ForwardRollLeaveTrap,
        hs::AbilityHandlerId::ForcedRetreatAndProjectile};
    for (std::size_t index = 0; index < expected_handlers.size(); ++index)
        Check(data.skills[index].handler == expected_handlers[index],
              "every default skill has an explicit ability handler");
    const auto rules_hash = hs::SimulationRulesHash(data);
    auto changed_rules = data;
    changed_rules.upgrades.piercing_shot.align_hit_normal_enemy.move_distance += 0.25f;
    Check(hs::SimulationRulesHash(data) == rules_hash,
          "simulation rules hash is stable for identical values");
    Check(hs::SimulationRulesHash(changed_rules) != rules_hash,
          "simulation rules hash changes with gameplay data");
    constexpr std::array<float, 7> expected_spawn_rates{
        0.9f, 1.2f, 2.1f, 3.3f, 4.8f, 4.8f, 4.8f};
    for (std::size_t index = 0; index < expected_spawn_rates.size(); ++index)
        Check(std::abs(data.spawn_stages[index].per_second -
                       expected_spawn_rates[index]) < 0.0001f,
              "continuous spawn rates are increased by twenty percent");
    Check(data.enemies[0].health == 10 && data.enemies[1].health == 5 &&
              data.enemies[2].health == 10,
          "starting enemy health is cooked from GameData");
    Check(std::abs(data.enemies[0].move_speed - 1.5f) < 0.0001f &&
              std::abs(data.enemies[1].move_speed - 1.2f) < 0.0001f &&
              std::abs(data.enemies[2].move_speed - 3.8f) < 0.0001f,
          "starting enemy movement speed is cooked from GameData");
    Check(data.spawn_stages[2].weights == std::array<std::uint8_t, 3>{74, 21, 5} &&
              data.spawn_stages[6].weights == std::array<std::uint8_t, 3>{49, 36, 15},
          "suicide spawn share is approximately halved while total spawn rate stays fixed");
    Check(data.growth.heal_pickup_chance_multiplier == 0.5f &&
              data.growth.magnet_pickup_chance_multiplier == 0.25f,
          "utility pickup chances use separate heal and magnet multipliers");
    Check(std::abs(data.relic_drop.normal_enemy_base_probability - 0.00001f) < 0.0000001f &&
              std::abs(data.relic_drop.normal_enemy_probability_increment_per_kill -
                       0.000004f) < 0.0000001f,
          "relic chest miss growth targets roughly four normal chests per run");
    Check(data.spawn_stages[5].per_second == 4.8f &&
              data.spawn_stages[6].per_second == 4.8f,
          "late continuous spawning keeps the twenty percent increase");
    Check(std::abs(data.enemies[1].projectile_speed - 6.875f) < 0.0001f &&
              data.enemies[1].attack_cooldown_ticks == 147,
          "ranged projectiles are faster while attack frequency is ten percent lower");

    hs::GameSimulation simulation;
    Check(simulation.Initialize({10}, data).Succeeded(), "starting balance initialize");
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    Check(WriteSnapshot(simulation, snapshot), "boundary snapshot capacity");
    std::uint32_t ground_count{};
    std::uint32_t tree_count{};
    std::uint32_t grass_count{};
    std::uint32_t dirt_count{};
    std::uint32_t rock_count{};
    for (const auto &instance : snapshot.View().instances)
    {
        if (instance.mesh == hs::RenderMesh::Ground) ++ground_count;
        if (instance.mesh == hs::RenderMesh::TreeTrunk) ++tree_count;
        if (instance.mesh == hs::RenderMesh::Grass) ++grass_count;
        if (instance.mesh == hs::RenderMesh::DirtPatch) ++dirt_count;
        if (instance.mesh == hs::RenderMesh::Rock) ++rock_count;
    }
    Check(ground_count == 1, "continuous ground covers the arena and decorative tree perimeter");
    Check(tree_count > data.arena_obstacle_count,
          "dense tree wall surrounds the authored interior obstacles");
    Check(grass_count > 100 && dirt_count == 0 && rock_count > 0,
          "village projects grass and authored rocks; dirt is blended in the ground material");

    for (std::uint32_t tick = 0; tick < 66; ++tick)
    {
        (void)Tick(simulation);
    }
    Check(simulation.GetObservation().normal_enemy_count == 0,
          "opening spawn does not occur before its accumulated interval");
    (void)Tick(simulation);
    Check(simulation.GetObservation().normal_enemy_count == 1,
          "opening spawn emits the first enemy after sixty-seven ticks");
    hs::RenderSnapshotStorage spawn_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, spawn_snapshot), "spawn snapshot");
    const auto spawned_enemy = std::ranges::find_if(
        spawn_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::MonsterMelee ||
                   instance.mesh == hs::RenderMesh::MonsterRanged ||
                   instance.mesh == hs::RenderMesh::MonsterSuicide;
        });
    const auto spawn_distance = spawned_enemy != spawn_snapshot.View().instances.end()
                                    ? std::hypot(spawned_enemy->position.x,
                                                 spawned_enemy->position.z)
                                    : 0.0f;
    Check(spawn_distance >= 20.0f && spawn_distance <= 30.0f,
          "normal enemies spawn in the original twenty-to-thirty metre ring");
    Check(simulation.Shutdown().Succeeded(), "starting balance shutdown");

    auto fallback_rules = QuietGameData();
    fallback_rules.spawn_stages.front().per_second = data.spawn_stages.front().per_second;
    fallback_rules.spawn_placement.require_outside_max_zoom_view = true;
    fallback_rules.spawn_placement.fallback_warning_ticks = 30;
    fallback_rules.spawn_placement.max_zoom_view_min_forward_m = -100.0f;
    fallback_rules.spawn_placement.max_zoom_view_max_forward_m = 100.0f;
    fallback_rules.spawn_placement.max_zoom_view_half_right_m = 100.0f;
    hs::GameSimulation fallback;
    Check(fallback.Initialize({12}, fallback_rules).Succeeded(), "spawn fallback initialize");
    for (std::uint32_t tick = 0; tick < 67; ++tick) (void)Tick(fallback);
    Check(fallback.GetObservation().normal_enemy_count == 0 &&
              std::ranges::any_of(fallback.PendingDomainSignals(), [](const hs::DomainSignal &signal) {
                  return signal.kind == hs::DomainSignalKind::EnemySpawnWarning;
              }),
          "normal enemy fallback warns before the farthest arena edge spawn");
    const auto warning = std::ranges::find_if(
        fallback.PendingDomainSignals(), [](const hs::DomainSignal &signal) {
            return signal.kind == hs::DomainSignalKind::EnemySpawnWarning;
        });
    std::array<hs::PresentationEvent, 4> projected{};
    const auto projected_count = warning != fallback.PendingDomainSignals().end()
                                     ? hs::ProjectDomainSignal(*warning, projected)
                                     : 0u;
    const auto parameters = hs::DecodeVfxParameters(projected[0].parameters);
    Check(warning != fallback.PendingDomainSignals().end() &&
              projected_count == 1 &&
              projected[0].kind == hs::PresentationKind::Vfx &&
              projected[0].asset.value ==
                  hs::MakeAssetId("particle.enemy.spawn_warning").value,
           "normal enemy fallback warning projects to a visible world effect");
    Check(std::hypot(parameters.direction.x, parameters.direction.z) > 0.0001f,
          "spawn warning VFX projection carries a valid direction");
    for (std::uint32_t tick = 0; tick < 29; ++tick) (void)Tick(fallback);
    Check(fallback.GetObservation().normal_enemy_count == 0,
          "normal enemy fallback remains absent during its warning window");
    (void)Tick(fallback);
    Check(fallback.GetObservation().normal_enemy_count == 1,
          "normal enemy fallback commits after the authored warning duration");
    Check(fallback.Shutdown().Succeeded(), "spawn fallback shutdown");
}

void TestExperienceBalance()
{
    hs::GameSimulation progression;
    Check(progression.Initialize({11}, QuietGameData()).Succeeded(),
          "experience progression initialize");
    Check(progression.GetObservation().experience_to_next == 12,
          "level one requirement keeps its base value");
    Debug(progression, hs::DebugCommandKind::GrantExperience, 12);
    (void)Tick(progression);
    Debug(progression, hs::DebugCommandKind::SelectCard, 0);
    Debug(progression, hs::DebugCommandKind::AssignStat,
          static_cast<std::uint64_t>(hs::StatKind::MaxHealth));
    Check(progression.GetObservation().level == 2 &&
              progression.GetObservation().experience_to_next == 16,
          "per-level experience requirement uses cooked coefficients");
    Check(progression.Shutdown().Succeeded(), "experience progression shutdown");

    const auto collected_xp = [](hs::EnemyKind kind) {
        auto data = QuietGameData();
        data.enemies[static_cast<std::size_t>(kind)].health = 1;
        data.stats.base_magnet_radius_m = 100.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({12}, data).Succeeded(), "enemy XP initialize");
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(kind), 0, {5.0f, 0.0f});
        hs::HeldInputState held;
        held.basic_attack_held = true;
        held.aim_world = {20.0f, 0.0f, 0.0f};
        for (std::uint32_t tick = 0; tick < 180 && simulation.GetObservation().kills == 0; ++tick)
            (void)Tick(simulation, held);
        held.basic_attack_held = false;
        for (std::uint32_t tick = 0; tick < 180 && simulation.GetObservation().experience == 0; ++tick)
            (void)Tick(simulation, held);
        const auto experience = simulation.GetObservation().experience;
        Check(simulation.Shutdown().Succeeded(), "enemy XP shutdown");
        return experience;
    };
    Check(collected_xp(hs::EnemyKind::Melee) == 1 &&
              collected_xp(hs::EnemyKind::Suicide) == 2,
          "suicide enemies grant exactly twice the melee enemy experience");
}

void TestLevelUpSelectionInputGuard()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({84}, QuietGameData()).Succeeded(),
          "selection input guard initialize");
    Debug(simulation, hs::DebugCommandKind::GrantExperience, 20);
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.cursor_normalized = NormalizedCursor(500.0f, 400.0f);
    (void)Tick(simulation, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::CardSelection,
          "level up opens card selection");

    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::CardSelection,
          "held combat click cannot select a level card");

    held.basic_attack_held = false;
    for (std::uint32_t frame = 0; frame < 12; ++frame) (void)Tick(simulation, held);
    held.basic_attack_held = true;
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::StatAllocation,
          "released deliberate click selects a level card");

    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::StatAllocation &&
              simulation.GetObservation().pending_stat_points == 1,
          "card click cannot spill into stat allocation");
    held.basic_attack_held = false;
    for (std::uint32_t frame = 0; frame < 12; ++frame) (void)Tick(simulation, held);
    held.basic_attack_held = true;
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing &&
              simulation.GetObservation().pending_stat_points == 0,
          "released deliberate click assigns a stat");
    Check(simulation.Shutdown().Succeeded(), "selection input guard shutdown");
}

void TestExperiencePickupDoesNotExpire()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({12}, QuietGameData()).Succeeded(),
          "experience persistence initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {10.0f, 0.0f});
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    for (std::uint32_t tick = 0; tick < 180 && simulation.GetObservation().kills == 0; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.GetObservation().pickup_count == 1,
          "enemy death creates one experience pickup");
    Check(std::ranges::none_of(simulation.PendingDomainSignals(),
                              [](const hs::DomainSignal &signal) {
              return signal.kind == hs::DomainSignalKind::ExperienceSpawned;
          }), "experience drop does not create a duplicate spawn effect");
    hs::RenderSnapshotStorage pickup_snapshot(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, pickup_snapshot),
          "death tick pickup snapshot");
    Check(std::ranges::any_of(pickup_snapshot.View().instances,
                              [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::Pickup &&
                     instance.color_rgba == 0xFFFFD040u && instance.scale.x >= 0.34f;
          }), "dropped experience is visible in the first available snapshot");
    for (std::uint32_t tick = 0; tick < 3'600; ++tick)
        (void)Tick(simulation);
    Check(simulation.GetObservation().pickup_count == 1,
          "experience pickup remains after one minute");
    Check(simulation.Shutdown().Succeeded(), "experience persistence shutdown");
}

void TestExperienceAttractSpeed()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.stats.base_magnet_radius_m = 20.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({120}, data).Succeeded(),
          "experience speed initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {10.0f, 0.0f});
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    while (simulation.GetObservation().kills == 0) (void)Tick(simulation, held);

    hs::RenderSnapshotStorage first(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, first), "experience speed first snapshot");
    const auto first_xp = std::ranges::find_if(
        first.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Pickup &&
                   instance.color_rgba == 0xFFFFD040u;
        });
    Check(first_xp != first.View().instances.end(), "experience exists before speed check");
    const auto first_x = first_xp->position.x;

    held.basic_attack_held = false;
    (void)Tick(simulation, held);
    hs::RenderSnapshotStorage second(32, 2, 2, 8);
    Check(WriteSnapshot(simulation, second), "experience speed second snapshot");
    const auto second_xp = std::ranges::find_if(
        second.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Pickup &&
                   instance.color_rgba == 0xFFFFD040u;
        });
    Check(second_xp != second.View().instances.end() &&
              std::abs((first_x - second_xp->position.x) - 0.25f) < 0.0001f,
          "experience attraction moves at 15 meters per second");
    Check(simulation.Shutdown().Succeeded(), "experience speed shutdown");
}

void TestMagnetPickupCollectsAllExperience()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.growth.utility_pickup_base_chance = 1.0f;
    data.growth.utility_pickup_miss_increment = 0.0f;
    data.growth.heal_pickup_chance_multiplier = 1.0f;
    data.growth.magnet_pickup_chance_multiplier = 1.0f;
    data.relic_drop.healing_pickup_probability = 1.0f;

    hs::GameSimulation simulation;
    Check(simulation.Initialize({22}, data).Succeeded(), "magnet pickup initialize");

    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {-20.0f, 0.0f, 0.0f};
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {-17.0f, 0.0f});
    for (std::uint32_t tick = 0; tick < 180 && simulation.GetObservation().kills < 1; ++tick)
        (void)Tick(simulation, held);

    held.basic_attack_held = false;
    held.move_held = true;
    held.move_target_world = {40.0f, 0.0f, 0.0f};
    for (std::uint32_t tick = 0;
         tick < 600 && simulation.GetObservation().player_position.x < 39.0f; ++tick)
        (void)Tick(simulation, held);

    held.move_held = false;
    held.basic_attack_held = true;
    held.aim_world = {60.0f, 0.0f, 0.0f};
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {50.0f, 0.0f});
    for (std::uint32_t tick = 0; tick < 180 && simulation.GetObservation().kills < 2; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.GetObservation().pickup_count == 6,
          "two kills independently drop experience, heal, and magnet pickups");
    hs::RenderSnapshotStorage pickup_snapshot(64, 2, 2, 8);
    Check(WriteSnapshot(simulation, pickup_snapshot), "utility pickup snapshot");
    const auto heal = std::ranges::find_if(
        pickup_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Pickup &&
                   instance.color_rgba == 0xFF40E060u;
        });
    const auto magnet = std::ranges::find_if(
        pickup_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Pickup &&
                    instance.color_rgba == 0xFFFF3030u;
        });
    Check(heal != pickup_snapshot.View().instances.end() &&
              magnet != pickup_snapshot.View().instances.end() &&
              heal->scale.y > heal->scale.x &&
              std::abs(magnet->scale.x - magnet->scale.y) < 0.0001f &&
              std::abs(magnet->scale.y - magnet->scale.z) < 0.0001f,
          "heal is green and tall while magnet is a red square");

    held.basic_attack_held = false;
    held.move_held = true;
    held.move_target_world = {50.0f, 0.0f, 0.0f};
    for (std::uint32_t tick = 0; tick < 600 && simulation.GetObservation().experience < 2; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.GetObservation().experience == 2,
          "magnet-attracted experience keeps tracking beyond the old three-second window");
    Check(simulation.GetObservation().pickup_count == 2,
          "magnet pull leaves unrelated distant utility pickups on the field");
    Check(simulation.Shutdown().Succeeded(), "magnet pickup shutdown");
}

void TestUtilityPickupMissChanceGrowth()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.growth.utility_pickup_base_chance = 0.0f;
    data.growth.utility_pickup_miss_increment = 4.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({23}, data).Succeeded(), "utility pity initialize");
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    for (std::uint32_t kill = 0; kill < 3; ++kill)
    {
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
        for (std::uint32_t tick = 0; tick < 180 && simulation.GetObservation().kills <= kill; ++tick)
            (void)Tick(simulation, held);
    }
    Check(simulation.GetObservation().pickup_count == 4,
          "successful utility drops reset misses immediately before the next kill");
    Check(simulation.Shutdown().Succeeded(), "utility pity shutdown");
}

void TestTimedBossEventsAndSpawnStop()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({51}, hs::SimulationRules::Defaults()).Succeeded(),
          "timeline initialize");

    Debug(simulation, hs::DebugCommandKind::SetGrowthTick, 5 * kTicksPerMinute - 2);
    (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 0, "5-minute pre-boundary");
    (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 0 &&
              HasVfx(simulation, hs::DomainSignalKind::BossSpawnWarning),
          "5-minute boss warning");
    for (std::uint32_t tick = 0; tick < 89; ++tick) (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 0, "5-minute warning window");
    (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 1, "5-minute boss event");

    Debug(simulation, hs::DebugCommandKind::SetGrowthTick, 10 * kTicksPerMinute - 2);
    (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 1, "10-minute pre-boundary");
    (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 1 &&
              HasVfx(simulation, hs::DomainSignalKind::BossSpawnWarning),
          "10-minute boss warning");
    for (std::uint32_t tick = 0; tick < 89; ++tick) (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 1, "10-minute warning window");
    (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 2, "10-minute boss event");

    Debug(simulation, hs::DebugCommandKind::SetGrowthTick, 15 * kTicksPerMinute - 2);
    (void)Tick(simulation);
    Check(!simulation.GetObservation().final_boss_spawned, "15-minute pre-boundary");
    (void)Tick(simulation);
    Check(simulation.GetObservation().final_boss_spawned &&
              simulation.GetObservation().boss_count == 2 &&
              HasVfx(simulation, hs::DomainSignalKind::BossSpawnWarning),
          "15-minute final boss warning");
    for (std::uint32_t tick = 0; tick < 89; ++tick) (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 2, "15-minute warning window");
    (void)Tick(simulation);
    const auto final_spawn = simulation.GetObservation();
    Check(final_spawn.final_boss_spawned && final_spawn.boss_count == 3,
          "15-minute final boss event");

    const auto normal_count = final_spawn.normal_enemy_count;
    for (std::uint32_t tick = 0; tick < 10; ++tick)
    {
        (void)Tick(simulation);
    }
    const auto after_final = simulation.GetObservation();
    Check(after_final.phase == hs::SessionPhase::Playing &&
              after_final.normal_enemy_count == normal_count &&
              after_final.boss_count == 3,
          "final boss stops new normal enemy spawns");
    Check(after_final.growth_ticks == 15 * kTicksPerMinute &&
              after_final.boss_fight_ticks == 100,
          "final boss switches growth clock to boss clock");
    Check(simulation.Shutdown().Succeeded(), "timeline shutdown");
}

void TestLargeWaveSchedule()
{
    auto data = QuietGameData();
    data.waves = hs::SimulationRules::Defaults().waves;
    for (auto &enemy : data.enemies) enemy.damage = 0;
    constexpr std::array<std::uint32_t, 5> minutes{3, 6, 9, 12, 14};
    constexpr std::array<std::uint32_t, 5> counts{30, 45, 65, 85, 100};
    for (std::size_t event = 0; event < minutes.size(); ++event)
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({55}, data).Succeeded(), "wave schedule initialize");
        const auto start = minutes[event] * kTicksPerMinute;
        Debug(simulation, hs::DebugCommandKind::SetGrowthTick, start - 5 * 60 - 1);
        (void)Tick(simulation);
        hs::RenderSnapshotStorage snapshot(256, 4, 2, 128);
        Check(WriteSnapshot(simulation, snapshot), "wave warning snapshot");
        const auto contains_text = [&](std::string_view expected) {
            return std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &model) {
                const auto end = std::ranges::find(model.utf8_text, '\0');
                return std::string_view(model.utf8_text.data(),
                                        end - model.utf8_text.begin()).contains(expected);
            });
        };
        Check(contains_text("대규모 웨이브까지 5초"),
              "large wave displays a five-second warning");

        Debug(simulation, hs::DebugCommandKind::SetGrowthTick,
              start - 1);
        (void)Tick(simulation);
        snapshot.Clear();
        Check(WriteSnapshot(simulation, snapshot), "wave start snapshot");
        Check(contains_text("대규모 웨이브 발생"),
              "large wave displays a spawn notification");
        const auto spawned = [&] {
            const auto &values = simulation.GetObservation().balance.enemy_spawned;
            return values[0] + values[1] + values[2];
        };
        Check(spawned() == 1,
              "large wave starts with one distributed spawn");

        for (std::uint32_t tick = 1; tick < 120; ++tick)
            (void)Tick(simulation);
        Check(spawned() == (counts[event] + 9) / 10,
              "large wave emits only one tenth of its enemies in two seconds");
        for (std::uint32_t tick = 120; tick < 1200; ++tick)
            (void)Tick(simulation);
        Check(spawned() == counts[event],
              std::format("{}m wave emits {} enemies over twenty seconds",
                          minutes[event], counts[event]));
        Check(simulation.Shutdown().Succeeded(), "wave schedule shutdown");
    }
}

void TestBossWarningExecutionAndPhaseCancellation()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({53}, QuietGameData()).Succeeded(),
          "boss warning initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::FiveMinute));

    for (std::uint32_t attempt = 0;
         attempt < 100 && simulation.GetObservation().boss_warning_count == 0; ++attempt)
    {
        (void)Tick(simulation);
    }
    const auto warning = simulation.GetObservation();
    Check(warning.boss_warning_count > 0 && warning.boss_dashing_count == 0 &&
              warning.enemy_projectile_count == 0 && warning.enemy_area_count == 0,
          "boss attack is warned before it becomes damaging");

    bool executed{};
    for (std::uint32_t attempt = 0; attempt < 70; ++attempt)
    {
        (void)Tick(simulation);
        const auto probe = simulation.GetObservation();
        if (probe.boss_dashing_count > 0 || probe.enemy_projectile_count > 0 ||
            probe.enemy_area_count > 0)
        {
            executed = true;
            break;
        }
    }
    Check(executed, "warned boss attack executes after its delay");
    Check(simulation.Shutdown().Succeeded(), "boss warning shutdown");

    hs::GameSimulation phase_simulation;
    Check(phase_simulation.Initialize({54}, QuietGameData()).Succeeded(),
          "boss phase initialize");
    Debug(phase_simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::Final));
    for (std::uint32_t attempt = 0;
         attempt < 100 && phase_simulation.GetObservation().boss_warning_count == 0; ++attempt)
    {
        (void)Tick(phase_simulation);
    }
    Check(phase_simulation.GetObservation().boss_warning_count > 0,
          "final boss warning queued before phase transition");
    Debug(phase_simulation, hs::DebugCommandKind::DamageFinalBoss, 2'250);
    (void)Tick(phase_simulation);
    const auto phase_two = phase_simulation.GetObservation();
    Check(phase_two.final_boss_phase_two && phase_two.boss_warning_count == 0 &&
              phase_two.boss_dashing_count == 0 &&
              phase_two.enemy_projectile_count == 0 &&
              phase_two.enemy_area_count == 0,
          "phase transition cancels queued and active boss attacks");
    Check(phase_simulation.Shutdown().Succeeded(), "boss phase shutdown");
}

void TestSameTickVictoryPriority()
{
    auto data = QuietGameData();
    hs::GameSimulation simulation;
    Check(simulation.Initialize({52}, data).Succeeded(), "victory priority initialize");
    Debug(simulation, hs::DebugCommandKind::SetGrowthTick, 15 * kTicksPerMinute - 1);
    (void)Tick(simulation);
    Check(simulation.GetObservation().final_boss_spawned &&
              simulation.GetObservation().boss_count == 0 &&
              HasVfx(simulation, hs::DomainSignalKind::BossSpawnWarning),
          "final boss warning precedes the boss by one and a half seconds");
    for (std::uint32_t tick = 0; tick < 89; ++tick) (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 0,
          "final boss remains absent throughout the warning window");
    (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 1 &&
              std::ranges::any_of(
                  simulation.PendingDomainSignals(), [](const hs::DomainSignal &signal) {
                      return signal.kind == hs::DomainSignalKind::BossSpawned &&
                             signal.context ==
                                 static_cast<std::uint8_t>(hs::BossKind::Final);
                  }),
          "final boss spawns after the warning with its audio routing context");

    Debug(simulation, hs::DebugCommandKind::DamageFinalBoss, 1'000'000);
    Debug(simulation, hs::DebugCommandKind::DamagePlayer, 1'000'000);
    const auto result = Tick(simulation);
    Check(result.phase == hs::SessionPhase::Victory &&
              simulation.GetObservation().health <= 0,
          std::format("same-tick final boss and player death resolves victory (phase={}, bosses={}, health={})",
                      static_cast<int>(result.phase),
                      simulation.GetObservation().boss_count,
                      simulation.GetObservation().health));
    Check(simulation.Shutdown().Succeeded(), "victory priority shutdown");
}

void TestNormalEnemyCountIsUnbounded()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({61}, QuietGameData()).Succeeded(), "unbounded initialize");
    constexpr std::uint32_t count = 1'101;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, index % 3, 0,
              {static_cast<float>(index % 20), static_cast<float>((index / 20) % 20)});
    }
    Check(simulation.GetObservation().normal_enemy_count == count,
          "normal enemy count grows beyond the former limit");
    Check(simulation.Shutdown().Succeeded(), "unbounded shutdown");
}

void TestRerollExcludesDisplayedCards()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({73}, QuietGameData()).Succeeded(), "reroll initialize");
    Debug(simulation, hs::DebugCommandKind::GrantExperience, 20);
    (void)Tick(simulation);
    const auto before = simulation.GetObservation();
    Check(before.phase == hs::SessionPhase::CardSelection && before.card_count == 3,
          "level cards displayed");
    Check(std::ranges::none_of(before.cards, [](const hs::CardView &card) {
              return card.kind == hs::CardKind::BonusStatPoint;
          }), "stat point fallback is absent while upgrades are available");
    Debug(simulation, hs::DebugCommandKind::Reroll);
    const auto after = simulation.GetObservation();
    Check(std::ranges::none_of(after.cards, [](const hs::CardView &card) {
              return card.kind == hs::CardKind::BonusStatPoint;
          }), "reroll does not mix stat point fallback into upgrade candidates");
    for (std::size_t left = 0; left < before.card_count; ++left)
    {
        for (std::size_t right = 0; right < after.card_count; ++right)
        {
            const auto &a = before.cards[left];
            const auto &b = after.cards[right];
            Check(a.kind != b.kind || a.subject != b.subject || a.upgrade != b.upgrade,
                  "reroll excludes displayed candidates when pool is sufficient");
        }
    }
    Check(after.level_rerolls_remaining == 2, "level reroll consumes level count");
    Check(after.relic_rerolls_remaining == 3, "level reroll preserves relic count");
    Check(simulation.Shutdown().Succeeded(), "reroll shutdown");
}

void TestTagWeightedCardSelection()
{
    const auto bleed = static_cast<hs::SkillTagMask>(hs::SkillTag::Bleed);
    const auto burn = static_cast<hs::SkillTagMask>(hs::SkillTag::Burn);
    const auto slow = static_cast<hs::SkillTagMask>(hs::SkillTag::Slow);
    Check(hs::RelicPrerequisiteTags(hs::RelicKind::BleedKillHeal) ==
              bleed &&
              hs::RelicPrerequisiteTags(hs::RelicKind::BurnPropagation) ==
                  burn &&
              hs::RelicPrerequisiteTags(hs::RelicKind::RadialBasicAttack) == 0,
          "conditional relics require a working build tag while standalone relics remain universal");
    Check(hs::SkillTags(hs::SkillKind::MultiShot) == 0 &&
              hs::SkillTags(hs::SkillKind::Trap) == slow &&
              hs::UpgradeTags(hs::SkillKind::BasicAttack, 0) == 0 &&
              hs::UpgradeTags(hs::SkillKind::BasicAttack, 3) == bleed &&
              hs::UpgradeTags(hs::SkillKind::BasicAttack, 4) == burn,
          "only real status application and dependency tags participate in synergy weighting");
}

void TestTwentyFourLevelChoicesAndStats()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({79}, QuietGameData()).Succeeded(),
          "24-level initialize");
    for (std::uint32_t choice = 0; choice < 24; ++choice)
    {
        const auto before = simulation.GetObservation();
        Debug(simulation, hs::DebugCommandKind::GrantExperience,
              before.experience_to_next - before.experience);
        (void)Tick(simulation);
        Check(simulation.GetObservation().phase == hs::SessionPhase::CardSelection,
              "each level pauses for a card");
        Debug(simulation, hs::DebugCommandKind::SelectCard, 0);
        Check(simulation.GetObservation().phase == hs::SessionPhase::StatAllocation,
              "card selection pauses for stat allocation");
        while (simulation.GetObservation().pending_stat_points > 0)
        {
            const auto probe = simulation.GetObservation();
            const auto available = std::ranges::find_if(
                probe.stat_points, [](std::uint8_t points) { return points < 10; });
            Check(available != probe.stat_points.end(), "stat point has available target");
            Debug(simulation, hs::DebugCommandKind::AssignStat,
                  static_cast<std::uint64_t>(available - probe.stat_points.begin()));
        }
        Check(simulation.GetObservation().phase == hs::SessionPhase::Playing,
              "all stat points are mandatory before resume");
    }
    Check(simulation.GetObservation().level == 25,
          "24 card/stat choices reach target level 25 without correction");
    Check(simulation.Shutdown().Succeeded(), "24-level shutdown");
}

void RunGameplayProgressionTests()
{
    TestStartingEnemyBalanceAndBoundary();
    TestExperienceBalance();
    TestLevelUpSelectionInputGuard();
    TestExperiencePickupDoesNotExpire();
    TestExperienceAttractSpeed();
    TestMagnetPickupCollectsAllExperience();
    TestUtilityPickupMissChanceGrowth();
    TestTimedBossEventsAndSpawnStop();
    TestLargeWaveSchedule();
    TestBossWarningExecutionAndPhaseCancellation();
    TestSameTickVictoryPriority();
    TestNormalEnemyCountIsUnbounded();
    TestRerollExcludesDisplayedCards();
    TestTagWeightedCardSelection();
    TestTwentyFourLevelChoicesAndStats();
}

} // namespace gameplay_test
