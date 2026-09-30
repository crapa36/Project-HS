#pragma once

#include <hs/game_domain/game_types.hpp>
#include <hs/game_domain/arena_boundary.hpp>
#include <hs/game_domain/arena_obstacle.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace hs
{

enum class StatusFlag : std::uint8_t
{
    Bleed = 1u << 0,
    Burn = 1u << 1,
    Slow = 1u << 2,
    Mark = 1u << 3,
};

enum class AreaViewKind : std::uint8_t
{
    Damage,
    Slow,
    Trap,
    EnemyDamage,
};

enum class BossActionViewKind : std::uint8_t
{
    Dash,
    Volley,
    Area,
    Shockwave,
};

struct PlayerView
{
    Float2 position{};
    Float2 aim{0.0f, 1.0f};
    Float2 facing{0.0f, 1.0f};
    float locomotion_blend{};
    bool charging{};
    SkillKind charging_skill{SkillKind::Count};
    Tick charge_start{};
    Tick charge_full_ready_tick{};
    Tick basic_attack_animation_start{};
    Tick basic_attack_animation_until{};
    Tick basic_attack_release_tick{};
    std::uint64_t basic_attack_cast_id{};
    Tick active_basic_empower_started{};
    Tick active_basic_empower_until{};
    Tick active_cast_tick{};
    Tick active_animation_start{};
    Tick active_animation_until{};
    Tick retreat_until{};
    std::array<SkillKind, 4> loadout{SkillKind::Count, SkillKind::Count,
                                     SkillKind::Count, SkillKind::Count};
    std::array<std::uint8_t, kCombatSkillCount> upgrades{};
    SkillKind forced_move_skill{SkillKind::Count};
    std::int32_t health{};
    std::int32_t max_health{};
    Tick revive_invulnerable_until{};
    Tick revive_invulnerable_started{};
};

struct StatusEpisodeView
{
    Tick started{}, expires{};
    std::uint64_t generation{};
};

struct EnemyView
{
    EntityId id{};
    EnemyKind kind{EnemyKind::Melee};
    std::optional<BossKind> boss;
    Float2 position{};
    Float2 velocity{};
    Float2 locked_aim{};
    float warning_extent{};
    Tick spawned_tick{};
    Tick attack_started{};
    Tick attack_resolve{};
    Tick boss_action_started{};
    Tick boss_action_until{};
    bool boss_action_recoil{};
    std::int32_t health{};
    std::int32_t max_health{};
    std::uint8_t status_flags{};
    bool attacking{};
    bool dead{};
    Sequence warning_sequence{};
    float warning_half_width{};
    float footprint_radius{};
    std::array<StatusEpisodeView, 4> status_episodes{};
    std::uint8_t final_phase{1};
    Tick phase2_started{};
    Tick invulnerable_until{};
    Tick dash_started{};
    Tick dash_until{};
    Float2 dash_origin{};
};

struct ProjectileView
{
    EntityId id{};
    bool player_owned{};
    Float2 position{};
    Float2 velocity{};
    Tick spawned_tick{};
    SkillKind skill{SkillKind::BasicAttack};
    bool dead{};
    float radius{};
    float charge_ratio{};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint8_t source_upgrade{0xFF};
    Float2 previous_position{};
    float remaining_range{};
    std::uint64_t cast_id{};
    std::uint8_t source_relic{0xFF};
    std::uint8_t source_enemy{0xFF};
    bool returning{};
    bool homing{};
    Tick return_started_tick{};
    Float2 return_start_position{};
    std::uint8_t upgrade_mask{};
    std::vector<Tick> bleed_extend_ticks;
};

struct AreaView
{
    EntityId id{};
    AreaViewKind kind{AreaViewKind::Damage};
    Float2 position{};
    Float2 direction{0.0f, 1.0f};
    float radius{};
    float half_length{};
    Tick active_tick{};
    Tick expires{};
    SkillKind skill{SkillKind::ArrowRain};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint64_t cast_id{};
    std::uint8_t source_upgrade{0xFF};
    float ring_inner_radius{};
    float ring_outer_radius{};
    float safe_gap_degrees{};
    std::uint8_t safe_gap_count{};
    bool applies_burn{};
    bool applies_slow{};
    bool dead{};
    float ring_half_width{};
    std::uint8_t upgrade_mask{};
};

struct PickupView
{
    EntityId id{};
    PickupKind kind{PickupKind::Experience};
    Float2 position{};
    bool dead{};
    Tick spawned_tick{};
};

struct BossActionView
{
    BossActionViewKind kind{BossActionViewKind::Dash};
    std::uint64_t boss_id{};
    Tick animation_started{};
    Tick execute_tick{};
    Float2 position{};
    Float2 direction{};
    float distance{};
    float arc_degrees{};
    float angle_offset{};
    float radius{};
    std::uint64_t cast_id{};
    // Authored boss geometry copied from the scheduled rule action.
    float volley_range{};
    std::uint8_t safe_gap_count{};
    float safe_gap_degrees{};
    float half_width{};
    Sequence warning_sequence{};
    Tick warning_started{};
    float warning_half_width{};
    float footprint_radius{};
    std::array<StatusEpisodeView, 4> status_episodes{};
};

// Cosmetic links retain their captured endpoints for a bounded simulation-tick
// lifetime. They never affect collision, damage, or GameplayChecksum.
enum class VisualLinkKind : std::uint8_t { Ricochet, BurnTransfer, RelicChain };
struct VisualLinkView
{
    Sequence owner_id{};
    VisualLinkKind kind{};
    Float3 source_position{}, target_position{};
    float width{};
    Tick started{}, expires{};
};

struct SpawnWarningView
{
    Sequence owner_id{};
    Float2 position{};
    float radius{};
    Tick started{}, expires{};
    bool boss{};
};

struct MiniBombView
{
    std::uint64_t owner_id{}, cast_id{};
    Float2 position{};
    float radius{};
    Tick started{}, expires{};
};

struct SkillRuntimeView
{
    Tick effective_cooldown{};
    std::int32_t displayed_damage{};
    float effective_range{};
    float area_radius{};
    Tick duration{};
    std::uint8_t projectile_count{};
    std::uint8_t pierce_count{};
};

struct WaveView
{
    Tick start{};
    Tick duration{};
};

struct SessionSummaryView
{
    std::uint64_t direct_damage{};
    std::uint64_t derived_damage{};
    std::uint64_t damage_over_time{};
    std::array<std::array<std::uint64_t, kUpgradeCount>, kCombatSkillCount>
        upgrade_damage{};
    std::array<std::uint64_t, kRelicCount> relic_damage{};
    std::array<std::uint64_t, kRelicCount> relic_triggers{};
    std::array<std::uint64_t, kRelicCount> relic_kills{};
    std::array<std::array<std::uint64_t, kUpgradeEffectMetricCount>, kRelicCount>
        relic_effects{};
};

struct GameReadModel
{
    Tick tick{};
    GameplayChecksum checksum{};
    std::uint64_t seed{};
    SessionProbe session{};
    PlayerView player{};
    std::span<const EnemyView> enemies;
    std::span<const ProjectileView> projectiles;
    std::span<const AreaView> areas;
    std::span<const PickupView> pickups;
    std::span<const BossActionView> boss_actions;
    float effective_attack{};
    float effective_attack_speed{};
    float effective_move_speed{};
    float effective_magnet_radius{};
    float arena_half_extent{};
    ArenaBoundary arena_boundary{};
    std::span<const ArenaObstacle2D> arena_obstacles;
    float charge_range{};
    float charge_radius{};
    // Charge progression copied from gameplay for charge-owned presentation.
    Tick charge_normal_ready_tick{};
    float charge_ratio{};
    std::array<SkillRuntimeView, kCombatSkillCount> skills{};
    std::array<WaveView, 5> waves{};
    SessionSummaryView summary{};
    // Monotonic identity for the active simulation session. This is presentation
    // metadata and is intentionally excluded from GameplayChecksum.
    std::uint64_t session_id{};
    std::span<const VisualLinkView> visual_links;
    std::span<const MiniBombView> mini_bombs;
    std::span<const SpawnWarningView> spawn_warnings;
};

class GameReadModelStorage
{
  public:
    GameReadModelStorage(std::size_t enemy_capacity = 1'024,
                         std::size_t projectile_capacity = 3'072,
                         std::size_t area_capacity = 256,
                         std::size_t pickup_capacity = 1'536,
                         std::size_t boss_action_capacity = 32);

    void Clear() noexcept;
    void AddEnemy(const EnemyView &value);
    void AddProjectile(const ProjectileView &value);
    void AddArea(const AreaView &value);
    void AddPickup(const PickupView &value);
    void AddBossAction(const BossActionView &value);
    void AddVisualLink(const VisualLinkView &value);
    void AddMiniBomb(const MiniBombView &value);
    void AddSpawnWarning(const SpawnWarningView &value);
    [[nodiscard]] GameReadModel View() const noexcept;

    Tick tick{};
    GameplayChecksum checksum{};
    std::uint64_t seed{};
    SessionProbe session{};
    PlayerView player{};
    float effective_attack{};
    float effective_attack_speed{};
    float effective_move_speed{};
    float effective_magnet_radius{};
    float arena_half_extent{};
    ArenaBoundary arena_boundary{};
    std::array<ArenaObstacle2D, kArenaObstacleMaxCount> arena_obstacles{};
    std::uint8_t arena_obstacle_count{};
    float charge_range{};
    float charge_radius{};
    Tick charge_normal_ready_tick{};
    float charge_ratio{};
    std::array<SkillRuntimeView, kCombatSkillCount> skills{};
    std::array<WaveView, 5> waves{};
    SessionSummaryView summary{};
    std::uint64_t session_id{};

  private:
    std::vector<EnemyView> enemies_;
    std::vector<ProjectileView> projectiles_;
    std::vector<AreaView> areas_;
    std::vector<PickupView> pickups_;
    std::vector<BossActionView> boss_actions_;
    std::vector<VisualLinkView> visual_links_;
    std::vector<MiniBombView> mini_bombs_;
    std::vector<SpawnWarningView> spawn_warnings_;
};

} // namespace hs
