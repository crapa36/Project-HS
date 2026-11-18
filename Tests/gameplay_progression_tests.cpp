#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>

namespace gameplay_test
{

namespace
{

constexpr hs::Tick kTicksPerMinute = 60 * 60;

} // namespace


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


void TestPickupCollectionTargetsCurrentPlayer()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.growth.utility_pickup_base_chance = 1.0f;
    data.growth.heal_pickup_chance_multiplier = 1.0f;
    data.growth.magnet_pickup_chance_multiplier = 1.0f;
    data.relic_drop.healing_pickup_probability = 1.0f;
    data.relic_drop.normal_enemy_base_probability = 1.0f;
    data.relic_drop.normal_enemy_probability_cap = 1.0f;

    hs::SimulationConfig config{121, true};
    config.scenario.player_stationary = true;
    config.scenario.auto_collect_progression = true;
    hs::GameSimulation simulation;
    Check(simulation.Initialize(config, data).Succeeded(), "pickup endpoint initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {1.0f, 0.0f});
    hs::HeldInputState held;
    held.basic_attack_held = true;
    held.aim_world = {20.0f, 0.0f, 0.0f};
    std::array<bool, 4> collected{};
    hs::GameReadModelStorage before;
    for (std::uint32_t tick = 0; tick < 180 &&
         !std::ranges::all_of(collected, [](bool found) { return found; }); ++tick)
    {
        hs::CopyReadModelForDevelopment(simulation, before);
        simulation.ClearDomainSignals();
        (void)Tick(simulation, held);
        const auto player = simulation.GetObservation().player_position;
        for (const auto &signal : simulation.PendingDomainSignals())
        {
            hs::PickupKind kind{};
            switch (signal.kind)
            {
            case hs::DomainSignalKind::ExperienceCollected: kind = hs::PickupKind::Experience; break;
            case hs::DomainSignalKind::HealCollected: kind = hs::PickupKind::Heal; break;
            case hs::DomainSignalKind::MagnetCollected: kind = hs::PickupKind::Magnet; break;
            case hs::DomainSignalKind::RelicCollected: kind = hs::PickupKind::RelicChest; break;
            default: continue;
            }
            const auto index = static_cast<std::size_t>(kind);
            collected[index] = true;
            const auto height = kind == hs::PickupKind::Experience ? 0.2f : 0.3f;
            Check(signal.flags == static_cast<std::uint8_t>(hs::DomainSignalFlag::HasTarget) &&
                      signal.target.x == player.x && signal.target.y == height &&
                      signal.target.z == player.y && signal.position.y == height,
                  "pickup signal carries the current player endpoint at its source height");
            if (kind == hs::PickupKind::Heal || kind == hs::PickupKind::Magnet)
            {
                const auto prior = std::ranges::find_if(before.View().pickups,
                    [kind](const hs::PickupView &pickup) { return pickup.kind == kind; });
                Check(prior != before.View().pickups.end(),
                      "utility pickup exists before collection tick");
                Check(signal.position.x == prior->position.x -
                          data.growth.experience_pickup_speed * (1.0f / 60.0f) &&
                          signal.position.z == prior->position.y,
                      "utility collection source is the pickup's moved position");
            }
        }
        if (simulation.GetObservation().kills != 0) held.basic_attack_held = false;
    }
    Check(std::ranges::all_of(collected, [](bool found) { return found; }),
          std::format("pickup collection signals XP={} heal={} magnet={} relic={}",
                      collected[0], collected[1], collected[2], collected[3]));
    Check(simulation.Shutdown().Succeeded(), "pickup endpoint shutdown");
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
    const auto boss_warning=std::ranges::find_if(simulation.PendingDomainSignals(),[](const auto &signal){return signal.kind==hs::DomainSignalKind::BossSpawnWarning;});
    Check(boss_warning!=simulation.PendingDomainSignals().end()&&boss_warning->geometry.kind==hs::DomainSignalGeometryKind::Circle&&
        boss_warning->geometry.radius==hs::SimulationRules::Defaults().boss_common.collision_radius&&
        boss_warning->geometry.source_id==boss_warning->sequence&&boss_warning->geometry.end_tick-boss_warning->geometry.start_tick==90,
        "boss spawn warning uses actual footprint and schedule");
    hs::GameReadModelStorage spawn_model; hs::CopyReadModelForDevelopment(simulation, spawn_model);
    const auto boss_owner=boss_warning!=simulation.PendingDomainSignals().end()?boss_warning->sequence:0;
    Check(std::ranges::any_of(spawn_model.View().spawn_warnings,[&](const auto &owner){return owner.boss&&owner.owner_id==boss_owner;}),
        "pending boss spawn publishes warning owner");
    for (std::uint32_t tick = 0; tick < 89; ++tick) (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 0, "5-minute warning window");
    (void)Tick(simulation);
    Check(simulation.GetObservation().boss_count == 1, "5-minute boss event");
    hs::CopyReadModelForDevelopment(simulation, spawn_model);
    Check(std::ranges::none_of(spawn_model.View().spawn_warnings,[&](const auto &owner){return owner.owner_id==boss_owner;}),
        "boss spawn commit removes warning owner");

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
    Check(final_spawn.final_boss_spawned && final_spawn.boss_count == 3 &&
              std::ranges::any_of(
                  simulation.PendingDomainSignals(), [](const hs::DomainSignal &signal) {
                      return signal.kind == hs::DomainSignalKind::BossSpawned &&
                             signal.context ==
                                 static_cast<std::uint8_t>(hs::BossKind::Final);
                  }),
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

void RunGameplayProgressionTests()
{
    TestLevelUpSelectionInputGuard();
    TestExperiencePickupDoesNotExpire();
    TestPickupCollectionTargetsCurrentPlayer();
    TestMagnetPickupCollectsAllExperience();
    TestTimedBossEventsAndSpawnStop();
    TestBossWarningExecutionAndPhaseCancellation();
    TestRerollExcludesDisplayedCards();
}

} // namespace gameplay_test
