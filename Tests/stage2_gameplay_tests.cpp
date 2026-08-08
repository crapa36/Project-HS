#include <hs/gameplay/game_simulation.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr auto kFixedStep = std::chrono::nanoseconds{16'666'667};
constexpr hs::Tick kTicksPerMinute = 60 * 60;

void Check(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

hs::GameData QuietGameData()
{
    auto data = hs::GameData::Defaults();
    for (auto &stage : data.spawn_stages)
    {
        stage.per_second = 0;
    }
    for (auto &wave : data.waves)
    {
        wave.count = 0;
    }
    data.utility_pickup_base_chance = 0.0f;
    data.utility_pickup_miss_increment = 0.0f;
    return data;
}

hs::TickResult Tick(hs::GameSimulation &simulation,
                    const hs::HeldInputState &held = {},
                    std::span<const hs::ActionEdge> edges = {})
{
    hs::InputFrame input;
    input.target_tick = simulation.Probe().tick + 1;
    input.held = held;
    input.ordered_edges = edges;
    return simulation.TickFixed(input, kFixedStep);
}

hs::TickResult TickEdge(hs::GameSimulation &simulation, hs::GameAction action,
                        hs::EdgeKind kind, hs::Sequence &sequence,
                        const hs::HeldInputState &held = {})
{
    const std::array edges{hs::ActionEdge{++sequence, action, kind}};
    return Tick(simulation, held, edges);
}

void Debug(hs::GameSimulation &simulation, hs::DebugCommandKind kind,
           std::uint64_t value = 0, std::uint32_t secondary = 0,
           hs::Float2 position = {})
{
    Check(simulation.ApplyDebugCommand({kind, value, secondary, position}).Succeeded(),
          "debug command");
}

void TestStartingEnemyBalanceAndBoundary()
{
    const auto data = hs::GameData::Defaults();
    Check(data.spawn_stages[0].per_second == 0.5f,
          "starting enemy spawn rate is one every two seconds");
    Check(data.enemies[0].health == 15 && data.enemies[1].health == 12 &&
              data.enemies[2].health == 13,
          "starting enemy health is halved and rounded");
    Check(std::abs(data.enemies[0].move_speed - 1.445f) < 0.0001f &&
              std::abs(data.enemies[1].move_speed - 1.19f) < 0.0001f &&
              std::abs(data.enemies[2].move_speed - 1.9125f) < 0.0001f,
          "normal enemy movement speed is 85 percent of the prior balance");

    hs::GameSimulation simulation;
    Check(simulation.Initialize({10}, data).Succeeded(), "starting balance initialize");
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    Check(simulation.WriteRenderSnapshot(snapshot), "boundary snapshot capacity");
    std::uint32_t boundary_count{};
    std::uint32_t ground_count{};
    for (const auto &instance : snapshot.View().instances)
    {
        if (instance.mesh == hs::RenderMesh::Area &&
            instance.color_rgba == 0xFF20A0FFu)
        {
            ++boundary_count;
        }
        if (instance.mesh == hs::RenderMesh::Ground)
        {
            ++ground_count;
        }
    }
    Check(boundary_count == 4, "four visible arena boundaries");
    Check(ground_count == 1, "one test grid ground");

    for (std::uint32_t tick = 0; tick < 119; ++tick)
    {
        (void)Tick(simulation);
    }
    Check(simulation.Probe().normal_enemy_count == 0,
          "halved spawn rate does not spawn before two seconds");
    (void)Tick(simulation);
    Check(simulation.Probe().normal_enemy_count == 1,
          "halved spawn rate spawns one normal enemy every two seconds");
    Check(simulation.Shutdown().Succeeded(), "starting balance shutdown");
}

void TestCursorMovement()
{
    auto data = QuietGameData();
    data.arena_half_extent = 10.0f;

    hs::GameSimulation simulation;
    Check(simulation.Initialize({11}, data).Succeeded(), "movement initialize");

    hs::HeldInputState held;
    held.move_held = true;
    held.move_target_world = {10.0f, 0.0f, 0.0f};
    held.aim_world = {10.0f, 0.0f, 0.0f};
    (void)Tick(simulation, held);
    const auto click_position = simulation.Probe().player_position;

    held.move_target_world = {0.0f, 0.0f, 10.0f};
    (void)Tick(simulation, held);
    const auto updated_target_position = simulation.Probe().player_position;
    Check(updated_target_position.y > click_position.y,
          "held RMB continuously updates the destination");

    held.move_held = false;
    for (std::uint32_t tick = 0; tick < 30; ++tick)
    {
        (void)Tick(simulation, held);
    }
    const auto released = simulation.Probe().player_position;
    Check(released.y > updated_target_position.y + 2.0f,
          "one RMB click keeps moving after button release");

    held.move_target_world = {-10.0f, 0.0f, 0.0f};
    held.move_held = true;
    (void)Tick(simulation, held);
    held.move_held = false;
    for (std::uint32_t tick = 0; tick < 240; ++tick)
    {
        (void)Tick(simulation, held);
    }
    const auto negative_edge = simulation.Probe().player_position;
    Check(std::abs(negative_edge.x + 10.0f) < 0.0001f &&
              std::abs(negative_edge.y) < 0.0001f,
          "new RMB click replaces destination and reaches it");
    Check(simulation.Shutdown().Succeeded(), "movement shutdown");
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
    for (std::uint32_t tick = 0; tick < 180 && simulation.Probe().kills == 0; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.Probe().pickup_count == 1,
          "enemy death creates one experience pickup");
    hs::RenderSnapshotStorage pickup_snapshot(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(pickup_snapshot),
          "death tick pickup snapshot");
    Check(std::ranges::any_of(pickup_snapshot.View().instances,
                              [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::Pickup &&
                     instance.color_rgba == 0xFFFFD040u && instance.scale.x >= 0.34f;
          }), "dropped experience is visible in the first available snapshot");
    for (std::uint32_t tick = 0; tick < 3'600; ++tick)
        (void)Tick(simulation);
    Check(simulation.Probe().pickup_count == 1,
          "experience pickup remains after one minute");
    Check(simulation.Shutdown().Succeeded(), "experience persistence shutdown");
}

void TestExperienceAttractSpeed()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.player_magnet_radius = 20.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({120}, data).Succeeded(),
          "experience speed initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {10.0f, 0.0f});
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    while (simulation.Probe().kills == 0) (void)Tick(simulation, held);

    hs::RenderSnapshotStorage first(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(first), "experience speed first snapshot");
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
    Check(simulation.WriteRenderSnapshot(second), "experience speed second snapshot");
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

hs::Float2 NormalizedCursor(float x, float y)
{
    return {x / 960.0f - 1.0f, 1.0f - y / 540.0f};
}

void SetCursor(hs::HeldInputState &held, float x, float y)
{
    held.cursor_normalized = NormalizedCursor(x, y);
    held.ui_cursor_pixels = {x, y};
}

void TestUiHitRegionsMatchAnchors()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({13, false, true}, QuietGameData()).Succeeded(),
          "UI hit initialize");
    hs::HeldInputState held;
    hs::Sequence sequence{};

    SetCursor(held, 960.0f, 466.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::MainMenu &&
              simulation.Probe().menu_page == 1,
          "collection anchor opens collection instead of starting");

    SetCursor(held, 960.0f, 792.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().menu_page == 0, "page back anchor returns to main menu");

    SetCursor(held, 960.0f, 616.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().menu_page == 2, "settings anchor opens settings");

    SetCursor(held, 700.0f, 348.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.PendingUiCommands().size() == 1 &&
              simulation.PendingUiCommands()[0].kind == hs::UiCommandKind::SetVsync &&
              simulation.PendingUiCommands()[0].value == 0,
          "settings VSync button emits exact target value");
    simulation.ClearUiCommands();

    SetCursor(held, 1'050.0f, 278.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.PendingUiCommands().size() == 1 &&
              simulation.PendingUiCommands()[0].kind ==
                  hs::UiCommandKind::SetMasterVolumePercent &&
              simulation.PendingUiCommands()[0].value == 90,
          "settings volume control emits clamped percent");
    simulation.ClearUiCommands();

    SetCursor(held, 1'100.0f, 578.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.PendingUiCommands().size() == 1 &&
              simulation.PendingUiCommands()[0].kind ==
                  hs::UiCommandKind::BeginSkillRebind &&
              simulation.PendingUiCommands()[0].value == 0,
          "settings key button begins the selected slot rebind");
    simulation.ClearUiCommands();

    hs::SettingsData rebound;
    rebound.skill_virtual_keys = {'W', 'Q', 'E', 'R'};
    simulation.ApplySettings(rebound);

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().menu_page == 0, "Esc returns from settings");

    SetCursor(held, 960.0f, 316.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::Playing,
          "start anchor begins session");
    Check(simulation.Shutdown().Succeeded(), "UI hit shutdown");
}

void TestGameplayDataHotReloadBoundary()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({17, false, true}, QuietGameData()).Succeeded(),
          "Hot Reload initialize");
    auto replacement = QuietGameData();
    replacement.player_health = 177;
    Check(simulation.ApplyGameData(replacement).Succeeded(),
          "Hot Reload applies at main menu boundary");

    hs::HeldInputState held;
    SetCursor(held, 960, 316);
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::Playing &&
              simulation.Probe().health == 177,
          "next session uses immutable replacement data");
    Check(!simulation.ApplyGameData(QuietGameData()),
          "Hot Reload rejects active gameplay");
    Check(simulation.Shutdown().Succeeded(), "Hot Reload shutdown");
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
    const auto moving = simulation.Probe();
    Check(moving.player_position.x > 0.0f &&
              moving.facing_direction.x > 0.99f,
          "RMB movement faces movement direction");

    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    const auto attacking = simulation.Probe();
    Check(std::abs(attacking.player_position.x - moving.player_position.x) < 0.0001f &&
              attacking.facing_direction.y > 0.99f &&
              attacking.player_projectile_count == 0,
          "LMB attack stops movement, faces cursor, and waits for release marker");

    hs::RenderSnapshotStorage turn_snapshot(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(turn_snapshot), "turn animation snapshot");
    Check(!turn_snapshot.View().poses.empty() &&
              turn_snapshot.View().poses.front().clip ==
                  hs::CharacterAnimationClip::TurnLeft &&
              !turn_snapshot.View().instances.empty() &&
              turn_snapshot.View().instances.front().yaw > 0.0f &&
              turn_snapshot.View().instances.front().yaw < 1.5708f,
          "attack direction change uses left-turn animation and smooth visual yaw");

    for (std::uint32_t tick = 0; tick < 6; ++tick) (void)Tick(simulation, held);
    Check(simulation.Probe().player_projectile_count == 1,
          "basic arrow appears on the animation release marker");
    hs::RenderSnapshotStorage projectile_snapshot(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(projectile_snapshot),
          "first basic projectile snapshot");
    Check(std::ranges::any_of(projectile_snapshot.View().instances,
                              [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::PlayerProjectile &&
                     std::abs(instance.scale.x - 0.24f) < 0.0001f &&
                     std::abs(instance.scale.z - 0.825f) < 0.0001f;
          }), "first basic arrow is visible at 75 percent visual size");

    held.basic_attack_held = false;
    (void)Tick(simulation, held);
    Check(simulation.Probe().player_position.x > attacking.player_position.x,
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
    const auto before_cast = simulation.Probe().player_position;
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(std::abs(simulation.Probe().player_position.x - before_cast.x) < 0.0001f,
          "successful skill pauses movement during its action delay");
    for (std::uint32_t tick = 0; tick < 15; ++tick) (void)Tick(simulation, held);
    Check(simulation.Probe().player_position.x > before_cast.x,
          "movement resumes toward the saved destination after skill recovery");

    const auto before_cooldown_press = simulation.Probe().player_position;
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().player_position.x > before_cooldown_press.x,
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
    Check(simulation.Probe().damage_by_skill[0] == 10,
          "basic arrow damages only the first enemy without an upgrade");
    Check(simulation.Shutdown().Succeeded(), "basic pierce shutdown");
}

void TestTenMinuteBossApproachesAttackRange()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({143}, QuietGameData()).Succeeded(),
          "ten minute boss approach initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::TenMinute));
    hs::RenderSnapshotStorage before_snapshot(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(before_snapshot), "boss before snapshot");
    const auto before = std::ranges::find_if(
        before_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Boss;
        });
    Check(before != before_snapshot.View().instances.end(), "ten minute boss visible");
    const auto before_distance = std::hypot(before->position.x, before->position.z);
    for (std::uint32_t tick = 0; tick < 60; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage after_snapshot(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(after_snapshot), "boss after snapshot");
    const auto after = std::ranges::find_if(
        after_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Boss;
        });
    Check(after != after_snapshot.View().instances.end() &&
              std::hypot(after->position.x, after->position.z) < before_distance - 2.0f,
          "ten minute boss approaches until its 12 meter attack range");
    Check(simulation.Shutdown().Succeeded(), "ten minute boss approach shutdown");
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
    while (simulation.Probe().cooldown_ticks[0] > 9)
    {
        (void)Tick(simulation, held);
    }
    Check(simulation.Probe().cooldown_ticks[0] == 9,
          "skill reaches input buffer window");

    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().cooldown_ticks[0] == 8,
          "early Q is buffered instead of cast immediately");
    bool buffered_cast{};
    for (std::uint32_t attempt = 0; attempt < 9; ++attempt)
    {
        (void)Tick(simulation, held);
        if (simulation.Probe().cooldown_ticks[0] > 9)
        {
            buffered_cast = true;
            break;
        }
    }
    Check(buffered_cast, "buffered Q casts when cooldown becomes ready");
    Check(simulation.Shutdown().Succeeded(), "input buffer shutdown");
}

void TestPauseStopsTicks()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({12}, QuietGameData()).Succeeded(), "pause initialize");
    (void)Tick(simulation);
    const auto before_pause = simulation.Probe();

    hs::Sequence sequence{};
    const auto paused = TickEdge(simulation, hs::GameAction::Pause,
                                 hs::EdgeKind::Pressed, sequence);
    Check(paused.phase == hs::SessionPhase::Paused && paused.tick == before_pause.tick,
          "pause edge stops current tick");
    for (std::uint32_t attempt = 0; attempt < 3; ++attempt)
    {
        (void)Tick(simulation);
    }
    const auto still_paused = simulation.Probe();
    Check(still_paused.tick == before_pause.tick &&
              still_paused.growth_ticks == before_pause.growth_ticks,
          "paused simulation does not advance");

    const auto resumed = TickEdge(simulation, hs::GameAction::Pause,
                                  hs::EdgeKind::Pressed, sequence);
    Check(resumed.phase == hs::SessionPhase::Playing &&
              resumed.tick == before_pause.tick + 1,
          "resume advances exactly one tick");
    Check(simulation.Shutdown().Succeeded(), "pause shutdown");
}

void TestQwerSkills()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({13}, QuietGameData()).Succeeded(), "QWER initialize");
    for (const auto skill : {hs::SkillKind::PiercingShot, hs::SkillKind::MultiShot,
                             hs::SkillKind::ChargedShot, hs::SkillKind::ExplosiveArrow})
    {
        Debug(simulation, hs::DebugCommandKind::GrantSkill,
              static_cast<std::uint64_t>(skill));
    }
    Check(simulation.Probe().active_skill_count == 4, "four active skills granted");

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    (void)Tick(simulation, held);
    hs::Sequence sequence{};

    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().cooldown_ticks[0] > 0, "Q casts first skill");

    for (std::uint32_t tick = 0; tick < 15; ++tick) (void)Tick(simulation, held);

    (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().cooldown_ticks[1] > 0, "W casts second skill");

    for (std::uint32_t tick = 0; tick < 18; ++tick) (void)Tick(simulation, held);

    held.move_held = true;
    held.move_target_world = {20.0f, 0.0f, 20.0f};
    const auto before_charge = simulation.Probe().player_position;
    (void)TickEdge(simulation, hs::GameAction::SkillE, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().cooldown_ticks[2] == 0, "E starts charged skill");
    for (std::uint32_t tick = 0; tick < 5; ++tick)
    {
        (void)Tick(simulation, held);
    }
    const auto charging = simulation.Probe();
    Check(std::abs(charging.player_position.x - before_charge.x) < 0.0001f &&
              std::abs(charging.player_position.y - before_charge.y) < 0.0001f &&
              charging.facing_direction.x > 0.99f,
          "charged skill stays stationary and faces cursor aim");
    (void)TickEdge(simulation, hs::GameAction::SkillE, hs::EdgeKind::Released,
                   sequence, held);
    Check(simulation.Probe().cooldown_ticks[2] > 0, "E releases charged skill");

    for (std::uint32_t tick = 0; tick < 9; ++tick) (void)Tick(simulation, held);

    (void)TickEdge(simulation, hs::GameAction::SkillR, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().cooldown_ticks[3] > 0, "R casts fourth skill");
    Check(simulation.Shutdown().Succeeded(), "QWER shutdown");
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
    Check(simulation.WriteRenderSnapshot(snapshot), "attack snapshot");
    Check(!snapshot.View().poses.empty() &&
              snapshot.View().poses.front().upper_body_clip ==
                  hs::CharacterAnimationClip::Recoil &&
              snapshot.View().poses.front().upper_body_weight == 1.0f,
          "basic attack layers recoil over locomotion until the release marker");

    for (std::uint32_t tick = 0; tick < 5; ++tick) (void)Tick(simulation, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "pre-release attack snapshot");
    Check(std::ranges::none_of(snapshot.View().instances,
                              [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::PlayerProjectile;
          }), "basic arrow waits for the animation release marker");
    (void)Tick(simulation, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "release attack snapshot");
    Check(std::ranges::any_of(snapshot.View().instances,
                             [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::PlayerProjectile;
          }), "first basic arrow is visible on its release snapshot");

    for (std::uint32_t tick = 0; tick < 24; ++tick)
    {
        (void)Tick(simulation, held);
    }
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "between attacks snapshot");
    Check(snapshot.View().poses.front().upper_body_weight == 0.0f,
          "basic attack upper-body action completes instead of looping while held");

    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ChargedShot));
    hs::Sequence sequence{};
    held.basic_attack_held = false;
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "charge telegraph snapshot");
    Check(std::ranges::any_of(snapshot.View().instances, [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::Area &&
                     instance.color_rgba == 0xFFFFFFFFu && instance.scale.z >= 12.0f;
          }), "charged shot renders a white range line");
    Check(simulation.Shutdown().Succeeded(), "presentation shutdown");
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
    for (std::uint32_t tick = 0;
         tick < 180 && simulation.Probe().player_projectile_count == 0; ++tick)
        (void)Tick(simulation, held);

    hs::RenderSnapshotStorage first(64, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(first), "arrow rain tracking first snapshot");
    const auto arrow = std::ranges::find_if(
        first.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::PlayerProjectile;
        });
    Check(arrow != first.View().instances.end(), "arrow rain creates tracking arrow");
    const auto first_position = arrow->position;

    (void)Tick(simulation, held);
    hs::RenderSnapshotStorage second(64, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(second), "arrow rain tracking second snapshot");
    const auto moved = std::ranges::find_if(
        second.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::PlayerProjectile;
        });
    Check(moved != second.View().instances.end() &&
              (std::abs(moved->position.x - first_position.x) > 0.1f ||
               std::abs(moved->position.z - first_position.z) > 0.1f),
          "arrow rain tracking arrow advances instead of standing still");
    Check(simulation.Shutdown().Succeeded(), "arrow rain tracking shutdown");
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
    Check(warning_simulation.WriteRenderSnapshot(warning_snapshot),
          "ranged warning snapshot");
    Check(std::ranges::any_of(
              warning_snapshot.View().instances, [](const hs::RenderInstance &instance) {
                  return instance.mesh == hs::RenderMesh::Area &&
                         instance.color_rgba == 0xA03030FFu && instance.scale.z >= 18.0f;
              }), "ranged enemy renders a red pre-attack range line");
    for (std::uint32_t tick = 0; tick < 90; ++tick)
        (void)Tick(warning_simulation);
    warning_snapshot.Clear();
    Check(warning_simulation.WriteRenderSnapshot(warning_snapshot),
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

    hs::GameSimulation explosion_simulation;
    Check(explosion_simulation.Initialize({21}, QuietGameData()).Succeeded(),
          "explosive area initialize");
    Debug(explosion_simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::ExplosiveArrow));
    Debug(explosion_simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
    Debug(explosion_simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 1.5f});
    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(explosion_simulation, hs::GameAction::SkillQ,
                   hs::EdgeKind::Pressed, sequence, held);
    for (std::uint32_t tick = 0; tick < 30 && explosion_simulation.Probe().kills < 2; ++tick)
    {
        (void)Tick(explosion_simulation, held);
    }
    Check(explosion_simulation.Probe().kills == 2,
          "explosive arrow impact damages every enemy in its radius");
    const auto combat_stats = explosion_simulation.Probe();
    Check(combat_stats.damage_by_skill[
              static_cast<std::size_t>(hs::SkillKind::ExplosiveArrow)] ==
              combat_stats.damage_dealt && combat_stats.damage_dealt > 0,
          "damage statistics attribute applied damage to the source skill");
    for (std::uint32_t tick = 0; tick < 60; ++tick)
        (void)Tick(explosion_simulation);
    Check(explosion_simulation.Probe().pickup_count == 2,
          "experience pickups remain separate in the same spatial cell");
    Check(explosion_simulation.Shutdown().Succeeded(), "explosive area shutdown");
}

void TestMagnetPickupCollectsAllExperience()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.utility_pickup_base_chance = 1.0f;
    data.utility_pickup_miss_increment = 0.0f;

    hs::GameSimulation simulation;
    Check(simulation.Initialize({22}, data).Succeeded(), "magnet pickup initialize");

    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {-20.0f, 0.0f, 0.0f};
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {-10.0f, 0.0f});
    for (std::uint32_t tick = 0; tick < 180 && simulation.Probe().kills < 1; ++tick)
        (void)Tick(simulation, held);

    held.aim_world = {20.0f, 0.0f, 0.0f};
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {10.0f, 0.0f});
    for (std::uint32_t tick = 0; tick < 180 && simulation.Probe().kills < 2; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.Probe().pickup_count == 6,
          "two kills independently drop experience, heal, and magnet pickups");
    hs::RenderSnapshotStorage pickup_snapshot(64, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(pickup_snapshot), "utility pickup snapshot");
    const auto heal = std::ranges::find_if(
        pickup_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Pickup &&
                   instance.color_rgba == 0xFF40E060u;
        });
    const auto magnet = std::ranges::find_if(
        pickup_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Pickup &&
                   instance.color_rgba == 0xFFFFB020u;
        });
    Check(heal != pickup_snapshot.View().instances.end() &&
              magnet != pickup_snapshot.View().instances.end() &&
              heal->scale.y > heal->scale.x && magnet->scale.x > magnet->scale.y,
          "heal and magnet pickups use distinct colors and silhouettes");

    held.basic_attack_held = false;
    held.move_held = true;
    held.move_target_world = {10.0f, 0.0f, 0.0f};
    for (std::uint32_t tick = 0; tick < 240 && simulation.Probe().experience < 2; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.Probe().experience == 2,
          "collecting a magnet pulls every individual experience pickup to the player");
    Check(simulation.Probe().pickup_count == 2,
          "magnet pull leaves unrelated distant utility pickups on the field");
    Check(simulation.Shutdown().Succeeded(), "magnet pickup shutdown");
}

void TestUtilityPickupMissChanceGrowth()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.utility_pickup_base_chance = 0.0f;
    data.utility_pickup_miss_increment = 1.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({23}, data).Succeeded(), "utility pity initialize");
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    for (std::uint32_t kill = 0; kill < 2; ++kill)
    {
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
        for (std::uint32_t tick = 0; tick < 180 && simulation.Probe().kills <= kill; ++tick)
            (void)Tick(simulation, held);
    }
    Check(simulation.Probe().pickup_count == 4,
          "each utility drop is guaranteed after one miss when increment is 100 percent");
    Check(simulation.Shutdown().Succeeded(), "utility pity shutdown");
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
    Check(simulation.Probe().player_position.x > 0.0f,
          "trap skill starts a forward forced move");
    hs::RenderSnapshotStorage snapshot(8, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(snapshot), "trap roll snapshot");
    Check(std::ranges::any_of(snapshot.View().instances, [](const auto &instance) {
              return instance.mesh == hs::RenderMesh::Area &&
                     instance.color_rgba == 0x6080D040u &&
                     std::abs(instance.position.x) < 0.01f;
          }), "trap remains at the roll origin");
    Check(simulation.Shutdown().Succeeded(), "trap roll shutdown");
}

std::vector<hs::GameplayChecksum> RunDeterministicOracle()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x12345678u, true}, hs::GameData::Defaults()).Succeeded(),
          "determinism initialize");

    std::vector<hs::GameplayChecksum> checksums;
    checksums.reserve(240);
    for (hs::Tick tick = 1; tick <= 240; ++tick)
    {
        hs::HeldInputState held;
        held.move_held = true;
        held.move_target_world = tick <= 120 ? hs::Float3{30.0f, 0.0f, 20.0f}
                                             : hs::Float3{-20.0f, 0.0f, -30.0f};
        held.aim_world = {30.0f, 0.0f, 0.0f};
        held.basic_attack_held = true;
        checksums.push_back(Tick(simulation, held).checksum);
        simulation.ClearParticleSpawns();
        simulation.ClearPresentationEvents();
    }
    Check(simulation.Shutdown().Succeeded(), "determinism shutdown");
    return checksums;
}

void TestSingleWorkerOracleDeterminism()
{
    const auto first = RunDeterministicOracle();
    const auto second = RunDeterministicOracle();
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        Check(first[index] == second[index],
              std::format("single-worker oracle differs at tick {}: {} != {}",
                          index + 1, first[index], second[index]));
    }
    Check(!first.empty() && first.back() != 0, "determinism checksum produced");
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
    const auto granted = simulation.Probe();
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
    Check(simulation.Probe().phase == hs::SessionPhase::Playing,
          "combat combination remains playable");
    Check(simulation.Shutdown().Succeeded(), "combat combination shutdown");
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
    Check(simulation.Probe().relic_mask == expected_mask, "relic mask");

    hs::HeldInputState held;
    held.move_held = true;
    held.move_target_world = {10.0f, 0.0f, 0.0f};
    held.aim_world = {10.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    (void)Tick(simulation, held);
    Check(simulation.Probe().phase == hs::SessionPhase::Playing,
          "relic combination remains playable");
    Check(simulation.Shutdown().Succeeded(), "relic shutdown");
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

void TestTimedBossEventsAndSpawnStop()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({51}, hs::GameData::Defaults()).Succeeded(),
          "timeline initialize");

    Debug(simulation, hs::DebugCommandKind::SetGrowthTick, 5 * kTicksPerMinute - 2);
    (void)Tick(simulation);
    Check(simulation.Probe().boss_count == 0, "5-minute pre-boundary");
    (void)Tick(simulation);
    Check(simulation.Probe().boss_count == 1, "5-minute boss event");

    Debug(simulation, hs::DebugCommandKind::SetGrowthTick, 10 * kTicksPerMinute - 2);
    (void)Tick(simulation);
    Check(simulation.Probe().boss_count == 1, "10-minute pre-boundary");
    (void)Tick(simulation);
    Check(simulation.Probe().boss_count == 2, "10-minute boss event");

    Debug(simulation, hs::DebugCommandKind::SetGrowthTick, 15 * kTicksPerMinute - 2);
    (void)Tick(simulation);
    Check(!simulation.Probe().final_boss_spawned, "15-minute pre-boundary");
    (void)Tick(simulation);
    const auto final_spawn = simulation.Probe();
    Check(final_spawn.final_boss_spawned && final_spawn.boss_count == 3,
          "15-minute final boss event");

    const auto normal_count = final_spawn.normal_enemy_count;
    for (std::uint32_t tick = 0; tick < 10; ++tick)
    {
        (void)Tick(simulation);
    }
    const auto after_final = simulation.Probe();
    Check(after_final.phase == hs::SessionPhase::Playing &&
              after_final.normal_enemy_count == normal_count,
          "final boss stops new normal enemy spawns");
    Check(after_final.growth_ticks == 15 * kTicksPerMinute &&
              after_final.boss_fight_ticks == 10,
          "final boss switches growth clock to boss clock");
    Check(simulation.Shutdown().Succeeded(), "timeline shutdown");
}

void TestLargeWaveSchedule()
{
    auto data = QuietGameData();
    data.waves = hs::GameData::Defaults().waves;
    constexpr std::array<std::uint32_t, 5> minutes{3, 6, 9, 12, 14};
    constexpr std::array<std::uint32_t, 5> counts{30, 45, 65, 85, 100};
    for (std::size_t event = 0; event < minutes.size(); ++event)
    {
        hs::GameSimulation simulation;
        Check(simulation.Initialize({55}, data).Succeeded(), "wave schedule initialize");
        Debug(simulation, hs::DebugCommandKind::SetGrowthTick,
              minutes[event] * kTicksPerMinute - 1);
        for (std::uint32_t tick = 0; tick < 120; ++tick)
            (void)Tick(simulation);
        Check(simulation.Probe().normal_enemy_count == counts[event],
              std::format("{}m wave emits {} enemies over two seconds",
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
         attempt < 100 && simulation.Probe().boss_warning_count == 0; ++attempt)
    {
        (void)Tick(simulation);
    }
    const auto warning = simulation.Probe();
    Check(warning.boss_warning_count > 0 && warning.boss_dashing_count == 0 &&
              warning.enemy_projectile_count == 0 && warning.enemy_area_count == 0,
          "boss attack is warned before it becomes damaging");

    bool executed{};
    for (std::uint32_t attempt = 0; attempt < 70; ++attempt)
    {
        (void)Tick(simulation);
        const auto probe = simulation.Probe();
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
         attempt < 100 && phase_simulation.Probe().boss_warning_count == 0; ++attempt)
    {
        (void)Tick(phase_simulation);
    }
    Check(phase_simulation.Probe().boss_warning_count > 0,
          "final boss warning queued before phase transition");
    Debug(phase_simulation, hs::DebugCommandKind::DamageFinalBoss, 2'250);
    (void)Tick(phase_simulation);
    const auto phase_two = phase_simulation.Probe();
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
    Check(simulation.Probe().final_boss_spawned, "final boss available for priority test");

    Debug(simulation, hs::DebugCommandKind::DamageFinalBoss, 1'000'000);
    Debug(simulation, hs::DebugCommandKind::DamagePlayer, 1'000'000);
    const auto result = Tick(simulation);
    Check(result.phase == hs::SessionPhase::Victory &&
              simulation.Probe().health <= 0,
          "same-tick final boss and player death resolves victory");
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
    Check(simulation.Probe().normal_enemy_count == count,
          "normal enemy count grows beyond the former limit");
    Check(simulation.Shutdown().Succeeded(), "unbounded shutdown");
}

void TestExperimentDebugCommands()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({71, true, true}, QuietGameData()).Succeeded(),
          "experiment commands initialize");
    Check(simulation.Probe().phase == hs::SessionPhase::MainMenu,
          "experiment starts at main menu");
    Debug(simulation, hs::DebugCommandKind::StartSession);
    Check(simulation.Probe().phase == hs::SessionPhase::Playing,
          "start_session command");
    Debug(simulation, hs::DebugCommandKind::DamagePlayer, 50);
    Debug(simulation, hs::DebugCommandKind::HealPlayer, 20);
    Check(simulation.Probe().health == 70, "heal_player command clamps and heals");
    Debug(simulation, hs::DebugCommandKind::SpawnBoss,
          static_cast<std::uint64_t>(hs::BossKind::FiveMinute));
    Check(simulation.Probe().boss_count == 1, "spawn_boss command");
    Check(simulation.Shutdown().Succeeded(), "experiment commands shutdown");
}

void TestRerollExcludesDisplayedCards()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({73}, QuietGameData()).Succeeded(), "reroll initialize");
    Debug(simulation, hs::DebugCommandKind::GrantExperience, 20);
    (void)Tick(simulation);
    const auto before = simulation.Probe();
    Check(before.phase == hs::SessionPhase::CardSelection && before.card_count == 3,
          "level cards displayed");
    Debug(simulation, hs::DebugCommandKind::Reroll);
    const auto after = simulation.Probe();
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
    Check(after.rerolls_remaining == 2, "reroll consumes shared count");
    Check(simulation.Shutdown().Succeeded(), "reroll shutdown");
}

void TestSkillUpgradeCardsHaveDescriptions()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({73}, QuietGameData()).Succeeded(),
          "upgrade description initialize");
    Debug(simulation, hs::DebugCommandKind::GrantExperience, 20);
    (void)Tick(simulation);
    const auto probe = simulation.Probe();
    Check(probe.phase == hs::SessionPhase::CardSelection,
          "upgrade description card selection");

    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    Check(simulation.WriteRenderSnapshot(snapshot), "upgrade description snapshot");
    bool checked_upgrade{};
    for (const auto &card : probe.cards)
    {
        if (card.kind != hs::CardKind::BasicUpgrade &&
            card.kind != hs::CardKind::SkillUpgrade)
            continue;
        checked_upgrade = true;
        const auto heading = std::format("강화 {} · ",
                                         static_cast<unsigned>(card.upgrade) + 1);
        const auto has_description = std::ranges::any_of(
            snapshot.View().ui, [&](const hs::UiModel &model) {
                const auto end = std::ranges::find(model.utf8_text, '\0');
                const std::string_view text(model.utf8_text.data(),
                                            end - model.utf8_text.begin());
                const auto heading_position = text.find(heading);
                return heading_position != std::string_view::npos &&
                       std::ranges::count(text, '\n') >= 2 &&
                       heading_position + heading.size() + 30 < text.size();
            });
        Check(has_description, "skill upgrade card includes readable effect description");
    }
    Check(checked_upgrade, "upgrade description test has an upgrade card");
    Check(simulation.Shutdown().Succeeded(), "upgrade description shutdown");
}

void TestTwentyFourLevelChoicesAndStats()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({79}, QuietGameData()).Succeeded(),
          "24-level initialize");
    for (std::uint32_t choice = 0; choice < 24; ++choice)
    {
        const auto before = simulation.Probe();
        Debug(simulation, hs::DebugCommandKind::GrantExperience,
              before.experience_to_next - before.experience);
        (void)Tick(simulation);
        Check(simulation.Probe().phase == hs::SessionPhase::CardSelection,
              "each level pauses for a card");
        Debug(simulation, hs::DebugCommandKind::SelectCard, 0);
        Check(simulation.Probe().phase == hs::SessionPhase::StatAllocation,
              "card selection pauses for stat allocation");
        while (simulation.Probe().pending_stat_points > 0)
        {
            const auto probe = simulation.Probe();
            const auto available = std::ranges::find_if(
                probe.stat_points, [](std::uint8_t points) { return points < 10; });
            Check(available != probe.stat_points.end(), "stat point has available target");
            Debug(simulation, hs::DebugCommandKind::AssignStat,
                  static_cast<std::uint64_t>(available - probe.stat_points.begin()));
        }
        Check(simulation.Probe().phase == hs::SessionPhase::Playing,
              "all stat points are mandatory before resume");
    }
    Check(simulation.Probe().level == 25,
          "24 card/stat choices reach target level 25 without correction");
    Check(simulation.Shutdown().Succeeded(), "24-level shutdown");
}

} // namespace

int main()
{
    try
    {
        TestStartingEnemyBalanceAndBoundary();
        TestCursorMovement();
        TestExperiencePickupDoesNotExpire();
        TestExperienceAttractSpeed();
        TestUiHitRegionsMatchAnchors();
        TestGameplayDataHotReloadBoundary();
        TestAttackStopsMovementAndFacesAim();
        TestSkillMovementPauseAndResume();
        TestBasicAttackStopsAtFirstEnemy();
        TestTenMinuteBossApproachesAttackRange();
        TestQwerInputBuffer();
        TestPauseStopsTicks();
        TestQwerSkills();
        TestCombatPresentationContracts();
        TestArrowRainTrackingProjectileMoves();
        TestRangedWarningAndExplosiveArea();
        TestMagnetPickupCollectsAllExperience();
        TestUtilityPickupMissChanceGrowth();
        TestTrapRollsForwardAndLeavesOriginTrap();
        TestSingleWorkerOracleDeterminism();
        TestCombatUpgradeCombinations();
        TestRelicCombinations();
        TestTimedBossEventsAndSpawnStop();
        TestLargeWaveSchedule();
        TestBossWarningExecutionAndPhaseCancellation();
        TestSameTickVictoryPriority();
        TestNormalEnemyCountIsUnbounded();
        TestExperimentDebugCommands();
        TestRerollExcludesDisplayedCards();
        TestSkillUpgradeCardsHaveDescriptions();
        TestTwentyFourLevelChoicesAndStats();
        std::cout << "stage2_gameplay_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "stage2_gameplay_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
