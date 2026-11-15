#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <utility>
#include <vector>

namespace gameplay_test
{

namespace
{
void CheckDerivedFireAreaCadence(hs::GameSimulation &simulation,
                                 hs::SkillKind skill, std::uint8_t source_upgrade,
                                 hs::Tick interval)
{
    hs::GameReadModelStorage model;
    const auto fire_area = [&]() -> const hs::AreaView * {
        hs::CopyReadModelForDevelopment(simulation, model);
        const auto found = std::ranges::find_if(model.View().areas,
            [skill, source_upgrade](const hs::AreaView &area) {
                return area.skill == skill && area.origin == hs::EffectOrigin::Derived &&
                       area.source_upgrade == source_upgrade;
            });
        return found == model.View().areas.end() ? nullptr : &*found;
    };
    const hs::AreaView *derived{};
    for (std::uint32_t tick = 0; tick < 120 && !derived; ++tick)
    {
        (void)Tick(simulation);
        derived = fire_area();
    }
    Check(derived != nullptr, "burning skill creates its derived fire area");
    if (!derived) return;
    const auto derived_id = derived->id;
    const auto fire_active_tick = derived->active_tick;
    const auto expected_damage_count =
        (derived->expires - fire_active_tick + interval - 1) / interval;
    Check(!model.View().enemies.empty(), "fire area target remains observable");
    if (model.View().enemies.empty()) return;
    auto enemy_health = model.View().enemies.front().health;
    std::vector<hs::Tick> fire_damage_ticks;
    bool fire_area_alive = true;
    for (std::uint32_t tick = 0; tick < 30 && fire_area_alive; ++tick)
    {
        (void)Tick(simulation);
        hs::CopyReadModelForDevelopment(simulation, model);
        if (!model.View().enemies.empty() && model.View().enemies.front().health < enemy_health)
        {
            fire_damage_ticks.push_back(model.View().tick);
            enemy_health = model.View().enemies.front().health;
        }
        fire_area_alive = std::ranges::any_of(model.View().areas,
            [derived_id](const hs::AreaView &area) { return area.id.value == derived_id.value; });
    }
    Check(fire_damage_ticks.size() == expected_damage_count,
          "derived fire area delivers every scheduled damage tick before retirement");
    if (!fire_damage_ticks.empty())
        Check(fire_damage_ticks.front() == fire_active_tick + 1,
              "directly materialized fire area first damages on the following tick");
    if (fire_damage_ticks.size() >= 2)
        Check(fire_damage_ticks[1] - fire_damage_ticks[0] == interval - 1,
              "first fire-area spacing includes its creation-tick phase");
    for (std::size_t index = 2; index < fire_damage_ticks.size(); ++index)
        if (fire_damage_ticks[index] - fire_damage_ticks[index - 1] != interval)
        {
            auto diagnostic = std::format(
                "derived fire area expected later interval {} but damage ticks were", interval);
            for (const auto tick : fire_damage_ticks)
                diagnostic += std::format(" {}", tick);
            Check(false, diagnostic);
            break;
        }
    Check(!fire_area_alive && std::ranges::none_of(model.View().areas,
        [derived_id](const hs::AreaView &area) { return area.id.value == derived_id.value; }),
        "derived fire area expires on its authored duration");
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
    const auto charge_started = TickEdge(simulation, hs::GameAction::SkillQ,
                                         hs::EdgeKind::Pressed, sequence, held);
    const auto paused = TickEdge(simulation, hs::GameAction::Pause,
                                 hs::EdgeKind::Pressed, sequence, held);
    Check(paused.phase == hs::SessionPhase::Paused && paused.tick == charge_started.tick &&
              paused.world_changed &&
              HasVfx(simulation, hs::DomainSignalKind::ChargedShotCancelled),
          "same-tick pause publishes charge cancellation without advancing time");
    const auto resumed = TickEdge(simulation, hs::GameAction::Pause,
                                  hs::EdgeKind::Pressed, sequence, held);
    Check(resumed.phase == hs::SessionPhase::Playing && resumed.tick == paused.tick + 1,
          "resume advances from the held canceled-charge sample");
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


void TestAuthoritativeCombatGeometryEmission()
{
    hs::SimulationConfig config{0x47454F4Du};
    config.scenario = {.player_stationary = true, .player_invulnerable = true,
                       .progression_enabled = false};
    hs::GameSimulation simulation;
    Check(simulation.Initialize(config, QuietGameData()).Succeeded(),
          "geometry emission initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::FiveMinute));
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    for (std::uint32_t tick = 0; tick < 2'400; ++tick) (void)Tick(simulation);

    bool saw_cone = false;
    bool saw_circle = false;
    bool saw_ring = false;
    for (const auto &signal : simulation.PendingDomainSignals())
    {
        if (signal.kind == hs::DomainSignalKind::BossVolleyReleased)
        {
            saw_cone = signal.geometry.kind == hs::DomainSignalGeometryKind::Cone &&
                signal.geometry.range > 0.0f && signal.geometry.half_angle_degrees > 0.0f &&
                signal.geometry.source_id != 0 &&
                std::abs(std::sqrt(signal.direction.x * signal.direction.x +
                                   signal.direction.z * signal.direction.z) - 1.0f) < 0.001f;
        }
        else if (signal.kind == hs::DomainSignalKind::BossAreaActivated)
        {
            saw_circle = signal.geometry.kind == hs::DomainSignalGeometryKind::Circle &&
                signal.geometry.radius > 0.0f &&
                signal.geometry.end_tick > signal.geometry.start_tick;
        }
        else if (signal.kind == hs::DomainSignalKind::BossShockwaveReleased)
        {
            saw_ring = signal.geometry.kind == hs::DomainSignalGeometryKind::RingGaps &&
                signal.geometry.inner_radius < signal.geometry.outer_radius &&
                signal.geometry.gap_offset_degrees ==
                    static_cast<float>(signal.geometry.source_id % 360);
        }
    }
    Check(saw_cone && saw_circle && saw_ring,
          "boss combat signals lost authoritative geometry");
    Check(simulation.Shutdown().Succeeded(), "geometry emission shutdown");
}

void TestArrowRainFireAreasUseTheirAuthoredInterval()
{
    auto data = QuietGameData();
    auto &arrow_rain = data.skills[static_cast<std::size_t>(hs::SkillKind::ArrowRain)];
    arrow_rain.duration_ticks = 2;
    arrow_rain.tick_interval_ticks = 30;
    arrow_rain.area_radius = 6.0f;
    auto &fire = data.upgrades.arrow_rain.apply_burn_and_create_fire_areas;
    fire.radius = 5.0f;
    fire.duration_ticks = 23;
    fire.tick_interval_ticks = 5;
    fire.damage_multiplier_per_tick = 1.0f;
    fire.maximum_areas_per_cast = 1;
    data.statuses.first_tick_delay_ticks = 120;
    data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)].health = 1'000'000;
    data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)].move_speed = 0.0f;

    hs::GameSimulation simulation;
    Check(simulation.Initialize({0xA11F1u}, data).Succeeded(),
          "arrow rain fire interval initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ArrowRain));
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::ArrowRain), 3);
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0, {5.0f, 0.0f});
    hs::HeldInputState held;
    held.aim_world = {5.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);

    CheckDerivedFireAreaCadence(simulation, hs::SkillKind::ArrowRain, 3, 5);
    Check(simulation.Shutdown().Succeeded(), "arrow rain fire interval shutdown");
}

void TestTrapFireAreasUseTheirAuthoredInterval()
{
    auto data = QuietGameData();
    data.arena_obstacle_count = 0;
    auto &trap = data.skills[static_cast<std::size_t>(hs::SkillKind::Trap)];
    trap.activation_delay_ticks = 0;
    trap.active_duration_ticks = 60;
    trap.detection_radius = 6.0f;
    trap.area_radius = 6.0f;
    auto &fire = data.upgrades.trap.trigger_burn_and_fire_area;
    fire.area_radius = 5.0f;
    fire.area_duration_ticks = 23;
    fire.tick_interval_ticks = 5;
    fire.damage_multiplier_per_tick = 1.0f;
    data.statuses.first_tick_delay_ticks = 120;
    data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)].health = 1'000'000;
    data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)].move_speed = 0.0f;

    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x7A9F1u}, data).Succeeded(),
          "trap fire interval initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::Trap));
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::Trap), 4);
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0, {0.5f, 0.0f});
    CheckDerivedFireAreaCadence(simulation, hs::SkillKind::Trap, 4, 5);
    Check(simulation.Shutdown().Succeeded(), "trap fire interval shutdown");
}


} // namespace gameplay_test
