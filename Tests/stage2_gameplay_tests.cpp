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
    constexpr std::array<float, 7> expected_spawn_rates{
        0.9f, 1.2f, 2.1f, 3.3f, 4.8f, 4.8f, 4.8f};
    for (std::size_t index = 0; index < expected_spawn_rates.size(); ++index)
        Check(std::abs(data.spawn_stages[index].per_second -
                       expected_spawn_rates[index]) < 0.0001f,
              "continuous spawn rates are increased by twenty percent");
    Check(data.enemies[0].health == 15 && data.enemies[1].health == 12 &&
              data.enemies[2].health == 13,
          "starting enemy health is halved and rounded");
    Check(std::abs(data.enemies[0].move_speed - 1.445f) < 0.0001f &&
              std::abs(data.enemies[1].move_speed - 1.19f) < 0.0001f &&
              std::abs(data.enemies[2].move_speed - 3.825f) < 0.0001f,
          "suicide enemy movement speed is doubled");
    Check(data.spawn_stages[2].weights == std::array<std::uint8_t, 3>{74, 21, 5} &&
              data.spawn_stages[6].weights == std::array<std::uint8_t, 3>{49, 36, 15},
          "suicide spawn share is approximately halved while total spawn rate stays fixed");
    Check(data.heal_pickup_chance_multiplier == 0.5f &&
              data.magnet_pickup_chance_multiplier == 0.25f,
          "utility pickup chances use separate heal and magnet multipliers");
    Check(std::abs(data.relic_chest_base_chance - 0.00001f) < 0.0000001f &&
              std::abs(data.relic_chest_miss_increment - 0.000004f) < 0.0000001f,
          "relic chest miss growth targets roughly four normal chests per run");
    Check(data.spawn_stages[5].per_second == 4.8f &&
              data.spawn_stages[6].per_second == 4.8f,
          "late continuous spawning keeps the twenty percent increase");
    Check(std::abs(data.enemies[1].projectile_speed - 6.875f) < 0.0001f &&
              std::abs(data.enemies[1].attack_cooldown_seconds - 2.444444f) < 0.0001f,
          "ranged projectiles are faster while attack frequency is ten percent lower");

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

    for (std::uint32_t tick = 0; tick < 66; ++tick)
    {
        (void)Tick(simulation);
    }
    Check(simulation.Probe().normal_enemy_count == 0,
          "opening spawn does not occur before its accumulated interval");
    (void)Tick(simulation);
    Check(simulation.Probe().normal_enemy_count == 1,
          "opening spawn emits the first enemy after sixty-seven ticks");
    Check(simulation.Shutdown().Succeeded(), "starting balance shutdown");
}

void TestExperienceBalance()
{
    hs::GameSimulation progression;
    Check(progression.Initialize({11}, QuietGameData()).Succeeded(),
          "experience progression initialize");
    Check(progression.Probe().experience_to_next == 20,
          "level one requirement keeps its base value");
    Debug(progression, hs::DebugCommandKind::GrantExperience, 20);
    (void)Tick(progression);
    Debug(progression, hs::DebugCommandKind::SelectCard, 0);
    Debug(progression, hs::DebugCommandKind::AssignStat,
          static_cast<std::uint64_t>(hs::StatKind::MaxHealth));
    Check(progression.Probe().level == 2 &&
              progression.Probe().experience_to_next == 24,
          "per-level experience requirement growth is halved");
    Check(progression.Shutdown().Succeeded(), "experience progression shutdown");

    const auto collected_xp = [](hs::EnemyKind kind) {
        auto data = QuietGameData();
        data.enemies[static_cast<std::size_t>(kind)].health = 1;
        data.player_magnet_radius = 100.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({12}, data).Succeeded(), "enemy XP initialize");
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(kind), 0, {5.0f, 0.0f});
        hs::HeldInputState held;
        held.basic_attack_held = true;
        held.aim_world = {20.0f, 0.0f, 0.0f};
        for (std::uint32_t tick = 0; tick < 180 && simulation.Probe().kills == 0; ++tick)
            (void)Tick(simulation, held);
        held.basic_attack_held = false;
        for (std::uint32_t tick = 0; tick < 180 && simulation.Probe().experience == 0; ++tick)
            (void)Tick(simulation, held);
        const auto experience = simulation.Probe().experience;
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
    held.ui_cursor_pixels = {500.0f, 400.0f};
    (void)Tick(simulation, held);
    Check(simulation.Probe().phase == hs::SessionPhase::CardSelection,
          "level up opens card selection");

    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::CardSelection,
          "held combat click cannot select a level card");

    held.basic_attack_held = false;
    for (std::uint32_t frame = 0; frame < 12; ++frame) (void)Tick(simulation, held);
    held.basic_attack_held = true;
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::StatAllocation,
          "released deliberate click selects a level card");

    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::StatAllocation &&
              simulation.Probe().pending_stat_points == 1,
          "card click cannot spill into stat allocation");
    held.basic_attack_held = false;
    for (std::uint32_t frame = 0; frame < 12; ++frame) (void)Tick(simulation, held);
    held.basic_attack_held = true;
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::Playing &&
              simulation.Probe().pending_stat_points == 0,
          "released deliberate click assigns a stat");
    Check(simulation.Shutdown().Succeeded(), "selection input guard shutdown");
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

    const auto contains_text = [](const hs::RenderSnapshot &snapshot,
                                  std::string_view expected) {
        return std::ranges::any_of(snapshot.ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    hs::RenderSnapshotStorage snapshot(4, 2, 2, 64);
    Check(simulation.WriteRenderSnapshot(snapshot), "collection snapshot");
    Check(contains_text(snapshot.View(), "스킬 도감") &&
              contains_text(snapshot.View(), "연속 추가 화살") &&
              contains_text(snapshot.View(), "액티브 연계 사격"),
          "collection shows all eight basic attack upgrades");

    SetCursor(held, 390.0f, 248.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "selected collection snapshot");
    Check(contains_text(snapshot.View(), "관통 사격") &&
              contains_text(snapshot.View(), "후속 화살") &&
              contains_text(snapshot.View(), "빠른 재사용") &&
              contains_text(snapshot.View(), "관통 사격을 발사한 직후"),
          "collection selection shows the skill description and all upgrades");

    SetCursor(held, 390.0f, 928.0f);
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
    Check(simulation.WriteRenderSnapshot(turn_snapshot), "facing snapshot");
    Check(!turn_snapshot.View().poses.empty() &&
              turn_snapshot.View().poses.front().clip ==
                  hs::CharacterAnimationClip::Idle,
          "attack direction change does not select a turn animation");
    Check(!turn_snapshot.View().instances.empty() &&
              std::abs(turn_snapshot.View().instances.front().yaw -
                       std::atan2(attacking.facing_direction.x,
                                  attacking.facing_direction.y)) < 0.0001f,
          "render facing follows gameplay facing without turn lag");

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
    auto data = QuietGameData();
    data.player_health = 10'000;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({143}, data).Succeeded(),
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
    for (std::uint32_t tick = 0; tick < 151; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage after_snapshot(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(after_snapshot), "boss after snapshot");
    const auto after = std::ranges::find_if(
        after_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Boss;
        });
    Check(after != after_snapshot.View().instances.end() &&
              std::hypot(after->position.x, after->position.z) < before_distance - 4.0f,
          "ten minute boss uses doubled movement speed toward its 18 meter attack range");
    for (std::uint32_t tick = 151;
         tick < 1200 && simulation.Probe().boss_warning_count == 0; ++tick)
        (void)Tick(simulation);
    Check(simulation.Probe().boss_warning_count > 0,
          "ten minute boss starts a skill warning");
    hs::RenderSnapshotStorage warning_snapshot(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(warning_snapshot), "boss warning snapshot");
    const auto warning = std::ranges::find_if(
        warning_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Boss;
        });
    Check(warning != warning_snapshot.View().instances.end(),
          "ten minute boss visible during warning");
    const auto warning_position = warning->position;
    for (std::uint32_t tick = 0; tick < 20; ++tick) (void)Tick(simulation);
    hs::RenderSnapshotStorage casting_snapshot(32, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(casting_snapshot), "boss casting snapshot");
    const auto casting = std::ranges::find_if(
        casting_snapshot.View().instances, [](const hs::RenderInstance &instance) {
            return instance.mesh == hs::RenderMesh::Boss;
        });
    Check(casting != casting_snapshot.View().instances.end() &&
              std::hypot(casting->position.x - warning_position.x,
                         casting->position.z - warning_position.z) < 0.001f,
          "ten minute boss remains stationary while warning and casting");
    for (std::uint32_t tick = 0;
         tick < 1200 && simulation.Probe().balance.enemy_attack_attempts[4] == 0; ++tick)
        (void)Tick(simulation);
    Check(simulation.Probe().balance.enemy_attack_attempts[4] > 0,
          "ten minute boss begins ranged attacks from the expanded 18 meter range");
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
    const auto charge_move = std::hypot(
        charging.player_position.x - before_charge.x,
        charging.player_position.y - before_charge.y);
    Check(charge_move > 0.1f && charge_move < 0.5f &&
              charging.facing_direction.x > 0.99f,
          "charged skill moves slowly and faces cursor aim");
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
    Check(!snapshot.View().poses.empty(), "basic attack writes an animation pose");
    Check(snapshot.View().poses.front().upper_body_clip ==
              hs::CharacterAnimationClip::Recoil,
          "basic attack selects the recoil clip");
    Check(snapshot.View().poses.front().upper_body_weight > 0.0f &&
              snapshot.View().poses.front().upper_body_weight < 1.0f,
          std::format("basic attack blend weight is {} before the release marker",
                      snapshot.View().poses.front().upper_body_weight));

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

    held.basic_attack_held = false;
    for (std::uint32_t tick = 0; tick < 34; ++tick)
    {
        (void)Tick(simulation, held);
    }
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "late attack clip snapshot");
    Check(snapshot.View().poses.front().upper_body_normalized_time *
                  snapshot.View().poses.front().upper_body_playback_rate < 1.0f &&
              snapshot.View().poses.front().upper_body_weight == 1.0f,
          "attack remains fully weighted until the recoil clip completes");

    (void)Tick(simulation, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "completed attack clip snapshot");
    Check(snapshot.View().poses.front().upper_body_normalized_time *
                  snapshot.View().poses.front().upper_body_playback_rate == 1.0f &&
              snapshot.View().poses.front().upper_body_weight == 1.0f,
          "attack reaches its final pose before blending out");

    (void)Tick(simulation, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "attack blend-out snapshot");
    Check(snapshot.View().poses.front().upper_body_normalized_time *
                  snapshot.View().poses.front().upper_body_playback_rate >= 1.0f &&
              snapshot.View().poses.front().upper_body_weight < 1.0f,
          "attack blend-out starts from the completed pose");

    for (std::uint32_t tick = 0; tick < 5; ++tick) (void)Tick(simulation, held);
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
    (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Released,
                   sequence, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "charged release snapshot");
    Check(std::abs(snapshot.View().poses.front().upper_body_playback_rate -
                   41.0f / 9.0f) < 0.0001f,
          "skill recoil playback fits the charged-shot recovery delay");
    Check(simulation.Shutdown().Succeeded(), "presentation shutdown");
}

void TestCharacterInformationPage()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({18}, QuietGameData()).Succeeded(),
          "character page initialize");
    Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
          static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), 0);
    Debug(simulation, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::BleedKillHeal));
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot));

    hs::HeldInputState held;
    hs::Sequence sequence{};
    const auto before = simulation.Probe().tick;
    const auto opened = TickEdge(simulation, hs::GameAction::CharacterPage,
                                 hs::EdgeKind::Pressed, sequence, held);
    Check(opened.phase == hs::SessionPhase::Paused && opened.tick == before &&
              simulation.Probe().menu_page == 3,
          "Tab opens the character page and pauses simulation");

    const auto contains_text = [](const hs::RenderSnapshot &snapshot,
                                  std::string_view expected) {
        return std::ranges::any_of(snapshot.ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    Check(simulation.WriteRenderSnapshot(snapshot), "character stats snapshot");
    Check(contains_text(snapshot.View(), "상세 능력치") &&
              contains_text(snapshot.View(), "기본 공격 피해") &&
              contains_text(snapshot.View(), "투자 포인트") &&
              contains_text(snapshot.View(), "현재 전투 기록"),
          "character overview shows effective stats and damage totals");

    SetCursor(held, 700, 165);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "character skills snapshot");
    Check(simulation.Probe().menu_page == 4 &&
              contains_text(snapshot.View(), "연속 추가 화살") &&
              contains_text(snapshot.View(), "기본 공격을 유지하면"),
          "skill page shows current upgrades and detailed skill explanation");

    SetCursor(held, 850, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 1'250, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    auto reordered = simulation.Probe();
    Check(reordered.skill_loadout[0] == hs::SkillKind::Count &&
              reordered.skill_loadout[2] == hs::SkillKind::PiercingShot,
          "clicking an occupied then empty slot moves the skill");

    SetCursor(held, 1'050, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 1'250, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    reordered = simulation.Probe();
    Check(reordered.skill_loadout[1] == hs::SkillKind::PiercingShot &&
              reordered.skill_loadout[2] == hs::SkillKind::MultiShot,
          "clicking two occupied slots swaps their QWER bindings");

    SetCursor(held, 1'000, 165);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "character relic snapshot");
    Check(simulation.Probe().menu_page == 5 &&
              contains_text(snapshot.View(), "피의 회복") &&
              contains_text(snapshot.View(), "출혈 중인 적을 처치하면"),
          "relic page shows acquired relic effects");

    const auto closed = TickEdge(simulation, hs::GameAction::CharacterPage,
                                 hs::EdgeKind::Pressed, sequence, held);
    Check(closed.phase == hs::SessionPhase::Playing &&
              simulation.Probe().menu_page == 0,
          "Tab closes the character page");
    held.aim_world = {20.0f, 0.0f, 0.0f};
    (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().cooldown_ticks[0] > 0,
          "the swapped W binding immediately casts piercing shot");
    Check(simulation.Shutdown().Succeeded(), "character page shutdown");
}

void TestPauseMenuActions()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({19}, QuietGameData()).Succeeded(),
          "pause menu initialize");
    hs::HeldInputState held;
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);

    hs::RenderSnapshotStorage snapshot(16, 2, 2, 32);
    Check(simulation.WriteRenderSnapshot(snapshot), "pause menu snapshot");
    const auto contains_text = [&](std::string_view expected) {
        return std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    Check(contains_text("계속") && contains_text("설정") &&
              contains_text("게임 종료"),
          "Esc menu shows continue, settings, and quit actions");

    SetCursor(held, 960, 556);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::Paused &&
              simulation.Probe().menu_page == 6,
          "pause settings keeps the game paused");
    snapshot.Clear();
    Check(simulation.WriteRenderSnapshot(snapshot), "in-game settings snapshot");
    Check(contains_text("화면:") && !contains_text("상세 능력치") &&
              !contains_text("캐릭터 정보") && !contains_text("스킬·강화"),
          "in-game settings does not render the character information page");

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::Paused &&
              simulation.Probe().menu_page == 0,
          "Esc returns from settings to the pause menu");

    SetCursor(held, 960, 456);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::Playing,
          "continue resumes gameplay");

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 960, 656);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.Probe().phase == hs::SessionPhase::QuitRequested,
          "pause menu quit requests application exit");
    Check(simulation.Shutdown().Succeeded(), "pause menu shutdown");
}

void TestAttackSpeedAnimationRate()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({83}, QuietGameData()).Succeeded(),
          "attack-speed animation initialize");
    for (std::uint32_t level = 0; level < 10; ++level)
    {
        const auto before = simulation.Probe();
        Debug(simulation, hs::DebugCommandKind::GrantExperience,
              before.experience_to_next - before.experience);
        (void)Tick(simulation);
        Debug(simulation, hs::DebugCommandKind::SelectCard, 0);
        Debug(simulation, hs::DebugCommandKind::AssignStat,
              static_cast<std::uint64_t>(hs::StatKind::AttackSpeed));
    }
    Check(simulation.Probe().stat_points[
              static_cast<std::size_t>(hs::StatKind::AttackSpeed)] == 10,
          "attack speed reaches level ten");

    hs::HeldInputState held;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    held.basic_attack_held = true;
    (void)Tick(simulation, held);
    hs::RenderSnapshotStorage snapshot(128, 4, 2, 64);
    Check(simulation.WriteRenderSnapshot(snapshot), "level-ten attack snapshot");
    Check(std::abs(snapshot.View().poses.front().upper_body_playback_rate -
                   41.0f / 21.0f) < 0.0001f,
          "level-ten attack keeps the prior 2.25 attacks per second and speeds animation");
    Check(simulation.Shutdown().Succeeded(), "attack-speed animation shutdown");
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
    Check(simulation.Probe().damage_by_skill[
              static_cast<std::size_t>(hs::SkillKind::PiercingShot)] == 4,
          "one-meter trail damages only enemies on the arrow path");

    hs::RenderSnapshotStorage snapshot(64, 2, 2, 8);
    Check(simulation.WriteRenderSnapshot(snapshot), "piercing trail snapshot");
    Check(std::ranges::any_of(snapshot.View().instances,
                              [](const hs::RenderInstance &instance) {
              return instance.mesh == hs::RenderMesh::Area &&
                     instance.color_rgba == 0x6080D040u &&
                     std::abs(instance.position.x - 12.0f) < 0.0001f &&
                     std::abs(instance.scale.x - 1.0f) < 0.0001f &&
                     std::abs(instance.scale.z - 24.0f) < 0.0001f;
          }), "piercing trail visual matches its 24 by 1 meter damage path");
    Check(simulation.Shutdown().Succeeded(), "piercing trail shutdown");
}

void TestStationaryCombatSimulationContract()
{
    auto data = QuietGameData();
    data.spawn_stages.front().per_second = 10.0f;
    data.spawn_stages.front().weights = {50, 50, 0};
    hs::SimulationConfig config{123};
    config.stationary_combat_simulation = true;
    hs::GameSimulation attacking;
    hs::GameSimulation idle;
    Check(attacking.Initialize(config, data).Succeeded() &&
              idle.Initialize(config, data).Succeeded(),
          "stationary combat initialize");
    Debug(attacking, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::Trap));
    Debug(attacking, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::RetreatShot));
    Debug(attacking, hs::DebugCommandKind::SetStat,
          static_cast<std::uint64_t>(hs::StatKind::AttackPower), 10);
    Debug(attacking, hs::DebugCommandKind::GrantExperience, 1'000);
    Debug(attacking, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {10.0f, 0.0f});
    Debug(idle, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {10.0f, 0.0f});

    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    hs::Sequence sequence{};
    (void)TickEdge(attacking, hs::GameAction::SkillQ, hs::EdgeKind::Pressed,
                   sequence, held);
    (void)Tick(idle);
    for (std::uint32_t tick = 1; tick < 600; ++tick)
    {
        if (tick == 30)
            (void)TickEdge(attacking, hs::GameAction::SkillW,
                           hs::EdgeKind::Pressed, sequence, held);
        else
            (void)Tick(attacking, held);
        (void)Tick(idle);
    }
    const auto active = attacking.Probe();
    const auto passive = idle.Probe();
    Check(active.player_position.x == 0.0f && active.player_position.y == 0.0f &&
              active.health == active.max_health && active.damage_taken > 0,
          "combat simulation keeps the player stationary and invulnerable while recording threat");
    Check(active.level == 1 && active.stat_points[
              static_cast<std::size_t>(hs::StatKind::AttackPower)] == 10,
          "combat simulation freezes progression and applies the requested build stats");
    Check(active.balance.enemy_spawned == passive.balance.enemy_spawned,
          "build-created entities do not change the paired enemy spawn stream");
    Check(attacking.Shutdown().Succeeded() && idle.Shutdown().Succeeded(),
          "stationary combat shutdown");
}

void TestStationaryProgressionSimulationContract()
{
    auto data = QuietGameData();
    data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)].health = 1;
    hs::SimulationConfig config{124};
    config.automatic_choices = true;
    config.stationary_combat_simulation = true;
    config.stationary_progression_simulation = true;
    hs::GameSimulation simulation;
    Check(simulation.Initialize(config, data).Succeeded(),
          "stationary progression initialize");
    for (std::uint32_t index = 0; index < 20; ++index)
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0,
              {2.0f, static_cast<float>(index) * 0.01f});
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    for (std::uint32_t tick = 0; tick < 1'200 && simulation.Probe().level == 1;
         ++tick)
        (void)Tick(simulation, held);
    const auto probe = simulation.Probe();
    Check(probe.level >= 2 &&
              probe.balance.pickup_collected[
                  static_cast<std::size_t>(hs::PickupKind::Experience)] >= 20,
          "stationary progression instantly collects experience and levels normally");
    Check(probe.player_position.x == 0.0f && probe.player_position.y == 0.0f &&
              probe.health == probe.max_health,
          "stationary progression remains fixed and invulnerable");
    Check(simulation.Shutdown().Succeeded(), "stationary progression shutdown");
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

    hs::GameSimulation suicide_warning;
    Check(suicide_warning.Initialize({201}, QuietGameData()).Succeeded(),
          "suicide warning initialize");
    Debug(suicide_warning, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Suicide), 0, {2.0f, 0.0f});
    (void)Tick(suicide_warning);
    hs::RenderSnapshotStorage suicide_snapshot(128, 4, 2, 64);
    Check(suicide_warning.WriteRenderSnapshot(suicide_snapshot),
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
    data.heal_pickup_chance_multiplier = 1.0f;
    data.magnet_pickup_chance_multiplier = 1.0f;

    hs::GameSimulation simulation;
    Check(simulation.Initialize({22}, data).Succeeded(), "magnet pickup initialize");

    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {-20.0f, 0.0f, 0.0f};
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {-17.0f, 0.0f});
    for (std::uint32_t tick = 0; tick < 180 && simulation.Probe().kills < 1; ++tick)
        (void)Tick(simulation, held);

    held.basic_attack_held = false;
    held.move_held = true;
    held.move_target_world = {40.0f, 0.0f, 0.0f};
    for (std::uint32_t tick = 0;
         tick < 600 && simulation.Probe().player_position.x < 39.0f; ++tick)
        (void)Tick(simulation, held);

    held.move_held = false;
    held.basic_attack_held = true;
    held.aim_world = {60.0f, 0.0f, 0.0f};
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {50.0f, 0.0f});
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
    for (std::uint32_t tick = 0; tick < 600 && simulation.Probe().experience < 2; ++tick)
        (void)Tick(simulation, held);
    Check(simulation.Probe().experience == 2,
          "magnet-attracted experience keeps tracking beyond the old three-second window");
    Check(simulation.Probe().pickup_count == 2,
          "magnet pull leaves unrelated distant utility pickups on the field");
    Check(simulation.Shutdown().Succeeded(), "magnet pickup shutdown");
}

void TestUtilityPickupMissChanceGrowth()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.utility_pickup_base_chance = 0.0f;
    data.utility_pickup_miss_increment = 4.0f;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({23}, data).Succeeded(), "utility pity initialize");
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    for (std::uint32_t kill = 0; kill < 3; ++kill)
    {
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {5.0f, 0.0f});
        for (std::uint32_t tick = 0; tick < 180 && simulation.Probe().kills <= kill; ++tick)
            (void)Tick(simulation, held);
    }
    Check(simulation.Probe().pickup_count == 5,
          "successful utility drops reset misses immediately before the next kill");
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
              after_final.normal_enemy_count == normal_count &&
              after_final.boss_count == 3,
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
        Check(simulation.WriteRenderSnapshot(snapshot), "wave warning snapshot");
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
        Check(simulation.WriteRenderSnapshot(snapshot), "wave start snapshot");
        Check(contains_text("대규모 웨이브 발생"),
              "large wave displays a spawn notification");
        const auto spawned = [&] {
            const auto &values = simulation.Probe().balance.enemy_spawned;
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
    Check(std::ranges::none_of(before.cards, [](const hs::CardView &card) {
              return card.kind == hs::CardKind::BonusStatPoint;
          }), "stat point fallback is absent while upgrades are available");
    Debug(simulation, hs::DebugCommandKind::Reroll);
    const auto after = simulation.Probe();
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
    const auto &effects = simulation.Probe().balance.relic_effects[relic];
    Check(effects[static_cast<std::size_t>(hs::UpgradeEffectMetric::Activations)] == 1 &&
              effects[static_cast<std::size_t>(
                  hs::UpgradeEffectMetric::CooldownTicksSaved)] > 0,
          "alternating relic records activation and actual cooldown refund");
    Check(simulation.Shutdown().Succeeded(),
          "alternating relic telemetry shutdown");
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
    const auto probe = simulation.Probe();
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
    const auto interaction_probe = interaction.Probe();
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
    Check(independent.Probe().balance.upgrade_effects[2][6][static_cast<std::size_t>(
              hs::UpgradeEffectMetric::ProjectilesCreated)] == 2,
          "multishot seventh upgrade creates exactly two independent arrows");
    (void)TickEdge(independent, hs::GameAction::SkillE, hs::EdgeKind::Pressed,
                   independent_sequence, independent_held);
    for (std::uint32_t tick = 0; tick < 120; ++tick)
        (void)Tick(independent, independent_held);
    Check(independent.Probe().balance.upgrade_effects[5][6][static_cast<std::size_t>(
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
    const auto split_probe = split.Probe();
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
    Check(snapshot_mask.Probe().balance.upgrade_effects[1][2][
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
    const auto repeat_probe = repeat.Probe();
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

    held.basic_attack_held = false;
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
    Check(basic_reacquire.Probe().balance.upgrade_damage[0][6] > 0,
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
    Check(miss_recovery.Probe().balance.upgrade_effects[2][7][
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
        mark.Probe().balance.upgrade_effects[4][6][static_cast<std::size_t>(
            hs::UpgradeEffectMetric::ExplosionsCreated)];
    held.basic_attack_held = true;
    for (std::uint32_t tick = 0; tick < 120; ++tick) (void)Tick(mark, held);
    held.basic_attack_held = false;
    Check(mark.Probe().balance.upgrade_effects[4][6][static_cast<std::size_t>(
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
    const auto trap_probe = small_trap.Probe();
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
    const auto retreat_probe = retreat_trap.Probe();
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
        const auto settled = simulation.Probe();
        for (std::uint32_t tick = 0; tick < 180; ++tick) (void)Tick(simulation, held);
        const auto after = simulation.Probe();
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
                       text.find('#', heading_position) == std::string_view::npos &&
                       std::ranges::count(text, '\n') >= 1 &&
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
        TestExperienceBalance();
        TestLevelUpSelectionInputGuard();
        TestCursorMovement();
        TestExperiencePickupDoesNotExpire();
        TestExperienceAttractSpeed();
        TestUiHitRegionsMatchAnchors();
        TestCharacterInformationPage();
        TestPauseMenuActions();
        TestGameplayDataHotReloadBoundary();
        TestAttackStopsMovementAndFacesAim();
        TestSkillMovementPauseAndResume();
        TestBasicAttackStopsAtFirstEnemy();
        TestTenMinuteBossApproachesAttackRange();
        TestQwerInputBuffer();
        TestPauseStopsTicks();
        TestQwerSkills();
        TestCombatPresentationContracts();
        TestAttackSpeedAnimationRate();
        TestArrowRainTrackingProjectileMoves();
        TestPiercingDamageTrailMatchesArrowPath();
        TestStationaryCombatSimulationContract();
        TestStationaryProgressionSimulationContract();
        TestRangedWarningAndExplosiveArea();
        TestMagnetPickupCollectsAllExperience();
        TestUtilityPickupMissChanceGrowth();
        TestTrapRollsForwardAndLeavesOriginTrap();
        TestSingleWorkerOracleDeterminism();
        TestCombatUpgradeCombinations();
        TestRelicCombinations();
        TestAlternatingSkillRelicTelemetry();
        TestTimedBossEventsAndSpawnStop();
        TestLargeWaveSchedule();
        TestBossWarningExecutionAndPhaseCancellation();
        TestSameTickVictoryPriority();
        TestNormalEnemyCountIsUnbounded();
        TestExperimentDebugCommands();
        TestRerollExcludesDisplayedCards();
        TestTagWeightedCardSelection();
        TestUpgradeDamageAttribution();
        TestSelectiveUpgradeInheritance();
        TestHighFanoutChainsTerminate();
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
