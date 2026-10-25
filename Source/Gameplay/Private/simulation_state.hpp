#pragma once

#include <unordered_map>

namespace hs
{

using namespace gameplay_detail;

struct ActorState
{
    PlayerState player{};
    // Tick-indexed ring, including tick zero; bounded by the authored echo age.
    std::vector<Float2> player_position_history;
    EnemyStore enemies;
    ProjectileStore projectiles;
    std::vector<AreaActor> areas;
    std::vector<PickupActor> pickups;
    EnemyStore pending_enemy_spawns;
    ProjectileStore pending_projectile_spawns;
    std::vector<AreaActor> pending_area_spawns;
};

struct CombatState
{
    struct HitKeyHash
    {
        std::size_t operator()(const std::pair<std::uint64_t, std::uint64_t> &key) const noexcept
        {
            const auto mix = [](std::uint64_t value) {
                value += 0x9e3779b97f4a7c15ull;
                value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
                value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
                return value ^ (value >> 31);
            };
            return static_cast<std::size_t>(mix(key.first ^ mix(key.second)));
        }
    };
    using HitIndex = std::unordered_map<std::pair<std::uint64_t, std::uint64_t>,
                                       std::size_t, HitKeyHash>;
    DamageCommandBuffer combat;
    std::vector<ScheduledAction> scheduled_actions;
    std::vector<BossAction> boss_actions;
    std::vector<CastHitRecord> cast_hits;
    std::vector<AreaHitRecord> area_hits;
    // Derived lookup only; authoritative traversal and checksums use the vectors above.
    HitIndex cast_hit_index, area_hit_index;
    std::vector<std::uint64_t> live_area_ids;
    std::vector<CastRuntime> cast_runtimes;
    CastCleanupMembership cast_cleanup;
    std::vector<DomainSignal> domain_signals;
    // Presentation metadata only; intentionally excluded from the checksum.
    std::vector<VisualLinkView> visual_links;
    std::optional<Tick> visual_links_pruned_tick;
    // Fixed backing avoids allocation/order changes in the death phase. No
    // visual TTL enters Gameplay; Runtime uses the cooked death source clock.
    std::array<BossDeathVisualView, kCosmeticOwnerCapacity> boss_death_visuals{};
    std::size_t boss_death_visual_count{};
    EnemySpatialGrid enemy_grid;
    EnemyQuery nearest_query, area_query, chain_query;
    // Transient compact working set for the ordered separation pass; never authoritative.
    std::vector<Float2> separation_positions;
    std::vector<std::size_t> collision_candidates;
    struct CollisionHit { std::size_t enemy; float time; };
    std::vector<CollisionHit> collision_hits;
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
    PickupWorkOwner pickup_work;
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
