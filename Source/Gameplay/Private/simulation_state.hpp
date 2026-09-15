#pragma once

namespace hs
{

using namespace gameplay_detail;

struct ActorState
{
    PlayerState player{};
    std::vector<EnemyActor> enemies;
    std::vector<ProjectileActor> projectiles;
    std::vector<AreaActor> areas;
    std::vector<PickupActor> pickups;
    std::vector<EnemyActor> pending_enemy_spawns;
    std::vector<ProjectileActor> pending_projectile_spawns;
    std::vector<AreaActor> pending_area_spawns;
};

struct CombatState
{
    DamageCommandBuffer combat;
    RelicRuleTable relic_rules;
    std::vector<ScheduledAction> scheduled_actions;
    std::vector<BossAction> boss_actions;
    std::vector<CastHitRecord> cast_hits;
    std::vector<AreaHitRecord> area_hits;
    std::vector<CastRuntime> cast_runtimes;
    std::vector<DomainSignal> domain_signals;
    EnemySpatialGrid enemy_grid;
    std::vector<std::size_t> collision_candidates;
};

struct NavigationState
{
    NavigationGrid player_navigation;
    NavigationGrid enemy_navigation;
    NavigationGrid boss_navigation;
    std::pair<int, int> player_navigation_target{-1, -1};
    std::pair<int, int> enemy_navigation_target{-1, -1};
};

struct ProgressionState
{
    std::array<CardView, 3> cards{};
    std::uint8_t card_count{};
    std::vector<ActiveWave> waves;
    std::vector<PendingBossSpawn> pending_boss_spawns;
    std::vector<PendingEnemySpawn> pending_enemy_spawns_delayed;
    std::uint32_t normal_chest_kills{};
    std::uint32_t heal_pickup_misses{};
    std::uint32_t magnet_pickup_misses{};
    std::uint64_t level_reroll_sequence{};
    std::uint64_t relic_reroll_sequence{};
};

struct TelemetryState
{
    std::uint32_t kills{};
    std::uint64_t damage_dealt{};
    std::array<std::uint64_t, kCombatSkillCount> damage_by_skill{};
    std::uint64_t damage_taken{};
    std::uint64_t healing{};
    BalanceObserver balance_observer{};

    BalanceTelemetry &Metrics() noexcept { return balance_observer.metrics; }
    const BalanceTelemetry &Metrics() const noexcept { return balance_observer.metrics; }
};

} // namespace hs
