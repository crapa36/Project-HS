#pragma once

#include <hs/core/result.hpp>
#include <hs/game_domain/game_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace hs
{

enum class SkillTag : std::uint16_t
{
    Bleed = 1u << 0,
    Burn = 1u << 1,
    Slow = 1u << 2,
};

using SkillTagMask = std::uint16_t;

enum class AbilityHandlerId : std::uint8_t
{
    BasicProjectileCadence,
    PiercingProjectile,
    UniformFanProjectiles,
    HoldReleaseLinearCharge,
    ProjectileToAreaExplosion,
    NearestUnhitTargetRicochet,
    TargetedPeriodicArea,
    ForwardRollLeaveTrap,
    ForcedRetreatAndProjectile,
};

[[nodiscard]] SkillTagMask SkillTags(SkillKind skill) noexcept;
[[nodiscard]] SkillTagMask UpgradeTags(SkillKind skill,
                                       std::uint8_t zero_based_upgrade) noexcept;
[[nodiscard]] SkillTagMask RelicPrerequisiteTags(RelicKind relic) noexcept;

struct SkillDefinition
{
    Tick cooldown_ticks{};
    float damage_coefficient{};
    float projectile_speed{};
    float range{};
    float collision_radius{};
    float area_radius{};
    Tick duration_ticks{};
    std::uint8_t projectile_count{1};
    std::uint8_t pierce_count{};
    AbilityHandlerId handler{AbilityHandlerId::BasicProjectileCadence};
};

struct EnemyDefinition
{
    std::int32_t health{};
    float move_speed{};
    std::int32_t damage{};
    float attack_range{};
    Tick warning_ticks{};
    Tick attack_cooldown_ticks{};
    float projectile_speed{};
    float projectile_range{};
};

struct BossDefinition
{
    std::int32_t health{};
    Tick recovery_ticks{};
};

struct SpawnStage
{
    std::uint16_t start_minute{};
    float per_second{};
    std::array<std::uint8_t, 3> weights{};
};

struct WaveDefinition
{
    std::uint16_t minute{};
    std::uint16_t count{};
    Tick duration_ticks{};
};

struct RelicDefinitions
{
    struct BleedKillHeal
    {
        float maximum_hp_heal_fraction{};
        Tick internal_cooldown_ticks{};
    } bleed_kill_heal;
    struct BurnPropagation
    {
        float search_radius{};
        float copied_burn_strength{};
        std::uint32_t maximum_targets{};
    } burn_propagation;
    struct KillCooldownSurge
    {
        std::uint32_t kills_per_trigger{};
        Tick cooldown_reduction_ticks{};
    } kill_cooldown_surge;
    struct BleedBurnExplosion
    {
        float radius{};
        float damage_multiplier{};
        Tick per_target_cooldown_ticks{};
    } bleed_burn_explosion;
    struct RadialBasicAttack
    {
        std::uint32_t cadence_interval{};
        std::uint32_t direction_count{};
        float damage_multiplier{};
    } radial_basic_attack;
    struct BasicKillTracker
    {
        float search_radius{};
        float damage_multiplier{};
        std::uint32_t maximum_triggers_per_attack{};
    } basic_kill_tracker;
    struct MovementEcho
    {
        float required_distance{};
        Tick position_history_age_ticks{};
        float damage_multiplier{};
    } movement_echo;
    struct AlternatingSkills
    {
        Tick window_ticks{};
        float cooldown_refund_fraction{};
    } alternating_skills;
    struct DifferentSkillTracker
    {
        Tick window_ticks{};
        float damage_multiplier{};
        Tick per_target_cooldown_ticks{};
    } different_skill_tracker;
    struct DamageKnockback
    {
        float radius{};
        float push_distance{};
        float slow_fraction{};
        Tick slow_duration_ticks{};
        Tick cooldown_ticks{};
    } damage_knockback;
    struct OnceRevive
    {
        float health_fraction{};
        Tick invulnerability_ticks{};
        std::uint32_t maximum_triggers_per_session{};
    } once_revive;
    struct CombatHitChain
    {
        std::uint32_t direct_hits_per_trigger{};
        float search_radius{};
        std::uint32_t maximum_targets{};
        float damage_multiplier{};
    } combat_hit_chain;
};

inline constexpr std::size_t kRelicNameBytes = 64;
inline constexpr std::size_t kRelicRuleBytes = 512;

struct SimulationRules
{
    std::uint32_t version{4};
    float arena_half_extent{60.0f};
    std::int32_t player_health{100};
    float player_attack{10.0f};
    float player_attack_speed{60.0f / 47.0f};
    float player_move_speed{5.0f};
    float player_magnet_radius{3.0f};
    float utility_pickup_base_chance{0.01f};
    float utility_pickup_miss_increment{0.001f};
    float heal_pickup_chance_multiplier{0.5f};
    float magnet_pickup_chance_multiplier{0.25f};
    float relic_chest_base_chance{0.00001f};
    float relic_chest_miss_increment{0.000004f};
    std::array<SkillDefinition, kCombatSkillCount> skills{};
    std::array<EnemyDefinition, 3> enemies{};
    std::array<BossDefinition, 3> bosses{};
    std::array<SpawnStage, 7> spawn_stages{};
    std::array<WaveDefinition, 5> waves{};
    RelicDefinitions relics{};
    [[nodiscard]] static SimulationRules Defaults() noexcept;
};

struct PresentationCatalog
{
    std::array<std::array<char, kRelicNameBytes>, kRelicCount> relic_names{};
    std::array<std::array<char, kRelicRuleBytes>, kRelicCount> relic_rules{};
};

struct CookedContentBundle
{
    SimulationRules simulation_rules{};
    PresentationCatalog presentation{};
};

[[nodiscard]] std::uint64_t SimulationRulesSchemaHash() noexcept;
[[nodiscard]] Result LoadCookedContent(const std::filesystem::path &path,
                                       CookedContentBundle &content,
                                       std::uint64_t *content_hash = nullptr);

} // namespace hs
