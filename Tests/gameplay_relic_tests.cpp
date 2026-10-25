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

static_assert(sizeof(hs::RelicMask) == sizeof(std::uint32_t));

namespace
{
void ExerciseRelics(std::span<const std::uint8_t> relics)
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({41}, QuietGameData()).Succeeded(), "relic initialize");
    hs::RelicMask expected_mask{};
    for (const auto relic : relics)
    {
        Debug(simulation, hs::DebugCommandKind::GrantRelic, relic);
        expected_mask |= hs::RelicMask{1} << relic;
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

void TestBasicKillTrackerHitExclusion()
{
    for (const auto first_side : {1.0f, -1.0f})
    {
        auto data = QuietGameData();
        data.arena_obstacle_count = 0;
        for (auto &enemy : data.enemies) enemy.move_speed = 0.0f;
        data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)].health = 10'000;
        data.enemies[static_cast<std::size_t>(hs::EnemyKind::Ranged)].health = 1;
        auto &basic = data.skills[static_cast<std::size_t>(hs::SkillKind::BasicAttack)];
        basic.pierce_count = 1;
        basic.range = 30.0f;
        data.relics.basic_kill_tracker.search_radius = 10.0f;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({229}, data).Succeeded(), "basic tracker initialize");
        Debug(simulation, hs::DebugCommandKind::GrantRelic,
              static_cast<std::uint64_t>(hs::RelicKind::BasicKillTracker));
        // One original piercing arrow hits the durable nearest enemy, then kills the ranged enemy.
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0, {6.0f, 0.0f});
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Ranged), 0, {8.0f, 0.0f});
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0, {8.0f, first_side * 4.0f});
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0, {8.0f, -first_side * 4.0f});
        hs::HeldInputState held;
        held.aim_world = {20.0f, 0.0f, 0.0f};
        held.basic_attack_held = true;
        bool tracked{};
        for (unsigned tick = 0; tick < 180 && !tracked; ++tick)
        {
            (void)Tick(simulation, held);
            for (const auto &signal : simulation.PendingDomainSignals())
            {
                if (signal.kind != hs::DomainSignalKind::TrackingArrowFired) continue;
                Check(std::abs(signal.position.x - 8.0f) < 0.001f &&
                          std::abs(signal.direction.x) < 0.001f &&
                          signal.direction.z * first_side > 0.999f,
                      "basic tracker excludes already-hit nearest target and preserves first equal-distance candidate");
                tracked = true;
                break;
            }
        }
        Check(tracked && simulation.GetObservation().kills == 1,
              "basic tracker fixture produces the original attack kill");
        hs::GameReadModelStorage model;
        simulation.CopyReadModelForDevelopment(model);
        const auto durable = std::ranges::find_if(model.View().enemies, [](const hs::EnemyView &enemy) {
            return enemy.position.x == 6.0f && enemy.position.y == 0.0f;
        });
        Check(durable != model.View().enemies.end() && durable->health > 0 &&
                  durable->health < durable->max_health,
              "excluded nearest enemy was hit by the original cast and remains alive");
        Check(simulation.Shutdown().Succeeded(), "basic tracker shutdown");
    }
}

} // namespace

void TestRelicCombinations()
{
    std::size_t singles{};
    for (std::uint8_t relic = 0; relic < hs::kRelicCount; ++relic)
    {
        const std::array set{relic};
        ExerciseRelics(set);
        ++singles;
    }
    Check(singles == 20, "twenty standalone relic slots");

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
    Check(pairs == 190, "C(20,2) relic pairs");

    hs::GameSimulation first;
    hs::GameSimulation second;
    Check(first.Initialize({43}, QuietGameData()).Succeeded() &&
              second.Initialize({43}, QuietGameData()).Succeeded(),
          "wide relic mask initialize");
    for (auto *simulation : {&first, &second})
    {
        Debug(*simulation, hs::DebugCommandKind::GrantRelic, 0);
        Debug(*simulation, hs::DebugCommandKind::GrantRelic, 19);
    }
    constexpr auto expected = hs::RelicMask{1} |
                              (hs::RelicMask{1} << 19);
    Check(first.GetObservation().relic_mask == expected &&
              second.GetObservation().relic_mask == expected,
          "wide relic mask preserves low and high slots");
    Check(first.ComputeChecksum() == second.ComputeChecksum(),
          "wide relic mask serialization remains deterministic");
    Check(first.Shutdown().Succeeded() && second.Shutdown().Succeeded(),
          "wide relic mask shutdown");
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
    TestBasicKillTrackerHitExclusion();
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

void TestIncomingDamageRelics()
{
    const auto first_hit = [](hs::RelicKind relic, bool low_health, bool both = false) {
        auto data = QuietGameData();
        auto &melee = data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)];
        melee.move_speed = 0.0f;
        melee.damage = both ? 7 : 20;
        melee.warning_ticks = 1;

        hs::GameSimulation simulation;
        Check(simulation.Initialize({229}, data).Succeeded(),
              "incoming damage relic initialize");
        Debug(simulation, hs::DebugCommandKind::GrantRelic,
              static_cast<std::uint64_t>(relic));
        if (both)
            Debug(simulation, hs::DebugCommandKind::GrantRelic,
                  static_cast<std::uint64_t>(hs::RelicKind::LowHealthSurvival));
        if (low_health)
            Debug(simulation, hs::DebugCommandKind::DamagePlayer, 65);
        Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
              static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0,
              {0.5f, 0.0f});
        for (std::uint32_t tick = 0;
             tick < 120 && simulation.GetObservation().damage_taken == 0; ++tick)
            (void)Tick(simulation);
        const auto damage = simulation.GetObservation().damage_taken;
        const auto activations = simulation.GetObservation().balance.relic_effects[
            static_cast<std::size_t>(relic)][static_cast<std::size_t>(
                hs::UpgradeEffectMetric::Activations)];
        Check(simulation.Shutdown().Succeeded(),
              "incoming damage relic shutdown");
        return std::pair{damage, activations};
    };

    const auto guard = first_hit(hs::RelicKind::PreDamageGuard, false);
    Check(guard.first == 16 && guard.second == 1,
          "pre-damage guard reduces one 20 damage hit by twenty percent");
    const auto survival = first_hit(hs::RelicKind::LowHealthSurvival, true);
    Check(survival.first == 12 && survival.second == 1,
          "low-health survival reduces one threshold hit by forty percent");
    const auto combined = first_hit(hs::RelicKind::PreDamageGuard, true, true);
    Check(combined.first == 4 && combined.second == 1,
          "incoming relics preserve sequential rounding and pre-damage guard attribution");
}

void TestSameTickLethalDamageAttribution()
{
    auto data = QuietGameData();
    data.enemies[0].health = 1;
    data.enemies[0].move_speed = 0.0f;
    data.relics.combat_hit_chain.direct_hits_per_trigger = 2;
    hs::GameSimulation simulation;
    Check(simulation.Initialize({239}, data).Succeeded(),
          "same-tick lethal attribution initialize");
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot));
    Debug(simulation, hs::DebugCommandKind::GrantRelic,
          static_cast<std::uint64_t>(hs::RelicKind::CombatHitChain));
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy,
          static_cast<std::uint64_t>(hs::EnemyKind::Melee), 0, {1.0f, 0.0f});
    hs::HeldInputState held;
    held.aim_world = {10.0f, 0.0f};
    hs::Sequence sequence{};
    const std::array edges{
        hs::ActionEdge{++sequence, hs::GameAction::SkillQ, hs::EdgeKind::Pressed},
        hs::ActionEdge{++sequence, hs::GameAction::SkillW, hs::EdgeKind::Pressed}};
    (void)Tick(simulation, held, edges);
    for (std::uint32_t tick = 0; tick < 10; ++tick) (void)Tick(simulation, held);
    const auto relic = static_cast<std::size_t>(hs::RelicKind::CombatHitChain);
    const auto activation = static_cast<std::size_t>(hs::UpgradeEffectMetric::Activations);
    Check(simulation.GetObservation().kills == 1 &&
              simulation.GetObservation().balance.relic_effects[relic][activation] == 0,
          "same-tick lethal damage is applied once and does not trigger hit chain");
    Check(simulation.Shutdown().Succeeded(), "same-tick lethal attribution shutdown");
}

} // namespace gameplay_test
