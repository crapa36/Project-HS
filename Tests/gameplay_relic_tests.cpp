#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace gameplay_test
{

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

} // namespace gameplay_test
