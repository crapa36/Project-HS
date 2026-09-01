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

} // namespace

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

} // namespace gameplay_test
