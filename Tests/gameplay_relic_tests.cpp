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
    const auto first_hit = [](hs::RelicKind relic, bool low_health) {
        auto data = QuietGameData();
        auto &melee = data.enemies[static_cast<std::size_t>(hs::EnemyKind::Melee)];
        melee.move_speed = 0.0f;
        melee.damage = 20;
        melee.warning_ticks = 1;

        hs::GameSimulation simulation;
        Check(simulation.Initialize({229}, data).Succeeded(),
              "incoming damage relic initialize");
        Debug(simulation, hs::DebugCommandKind::GrantRelic,
              static_cast<std::uint64_t>(relic));
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
