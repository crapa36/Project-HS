#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace gameplay_test
{

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

void TestRicochetCooldownBudget()
{
    for (const hs::Tick maximum : {hs::Tick{60}, hs::Tick{25}})
    {
        auto data = QuietGameData();
        data.arena_obstacle_count = 0;
        data.enemies[0].health = 10000;
        data.enemies[0].move_speed = 0;
        data.enemies[0].damage = 0;
        data.skills[1].cooldown_ticks = 600;
        data.skills[5].cooldown_ticks = 1;
        data.skills[5].projectile_speed = 2400;
        data.upgrades.ricochet_arrow.ricochet_cooldown_reduction.maximum_reduction_ticks = maximum;
        hs::GameSimulation simulation;
        Check(simulation.Initialize({0xC06u}, data).Succeeded(), "ricochet budget initialize");
        for (const auto skill : {hs::SkillKind::PiercingShot, hs::SkillKind::RicochetArrow})
            Debug(simulation, hs::DebugCommandKind::GrantSkill, static_cast<std::uint64_t>(skill));
        for (const auto upgrade : {2u, 6u})
            Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
                  static_cast<std::uint64_t>(hs::SkillKind::RicochetArrow), upgrade);
        hs::HeldInputState held; held.aim_world = {0, 0, 20};
        hs::Sequence sequence{};
        (void)TickEdge(simulation, hs::GameAction::SkillQ, hs::EdgeKind::Pressed, sequence, held);
        for (int i = 0; i < 20; ++i) (void)Tick(simulation, held);
        for (int i = 0; i < 9; ++i)
            Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {2.0f + i * 2.0f, 0});
        held.aim_world = {20, 0, 0};
        for (std::uint64_t cast = 1; cast <= 2; ++cast)
        {
            (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Pressed, sequence, held);
            for (int i = 0; i < 25; ++i) (void)Tick(simulation, held);
            Check(simulation.GetObservation().balance.upgrade_effects[5][6][
                      static_cast<std::size_t>(hs::UpgradeEffectMetric::CooldownTicksSaved)] == cast * maximum,
                  "extended ricochets consume one actual-saved budget and new casts reset it");
        }
        Check(simulation.Shutdown().Succeeded(), "ricochet budget shutdown");
    }
}

} // namespace gameplay_test
