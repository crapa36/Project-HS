#pragma once

#include <hs/game_domain/game_types.hpp>

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
    Tick basic_attack_animation_start{};
    Tick basic_attack_animation_until{};
    Tick active_cast_tick{};
    Tick active_animation_start{};
    Tick active_animation_until{};
    Tick retreat_until{};
    std::array<SkillKind, 4> loadout{SkillKind::Count, SkillKind::Count,
                                     SkillKind::Count, SkillKind::Count};
    std::array<std::uint8_t, kCombatSkillCount> upgrades{};
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
};

struct PickupView
{
    EntityId id{};
    PickupKind kind{PickupKind::Experience};
    Float2 position{};
    bool dead{};
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
    float charge_range{};
    float charge_radius{};
    std::array<SkillRuntimeView, kCombatSkillCount> skills{};
    std::array<WaveView, 5> waves{};
    SessionSummaryView summary{};
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
    float charge_range{};
    float charge_radius{};
    std::array<SkillRuntimeView, kCombatSkillCount> skills{};
    std::array<WaveView, 5> waves{};
    SessionSummaryView summary{};

  private:
    std::vector<EnemyView> enemies_;
    std::vector<ProjectileView> projectiles_;
    std::vector<AreaView> areas_;
    std::vector<PickupView> pickups_;
    std::vector<BossActionView> boss_actions_;
};

} // namespace hs
