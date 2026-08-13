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
    Tick spawned_tick{};
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
    Float2 position{};
    Float2 direction{};
    float distance{};
    float arc_degrees{};
    float angle_offset{};
    float radius{};
    std::uint64_t cast_id{};
};

struct GameReadModel
{
    Tick tick{};
    GameplayChecksum checksum{};
    std::uint64_t seed{};
    SimulationObservation session{};
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
    std::uint8_t collection_skill_index{};
    std::uint8_t character_skill_index{};
    std::uint8_t character_slot_source{0xFF};
    std::uint8_t pending_rebind_slot{0xFF};
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
    SimulationObservation session{};
    PlayerView player{};
    float effective_attack{};
    float effective_attack_speed{};
    float effective_move_speed{};
    float effective_magnet_radius{};
    std::uint8_t collection_skill_index{};
    std::uint8_t character_skill_index{};
    std::uint8_t character_slot_source{0xFF};
    std::uint8_t pending_rebind_slot{0xFF};

  private:
    std::vector<EnemyView> enemies_;
    std::vector<ProjectileView> projectiles_;
    std::vector<AreaView> areas_;
    std::vector<PickupView> pickups_;
    std::vector<BossActionView> boss_actions_;
};

} // namespace hs
