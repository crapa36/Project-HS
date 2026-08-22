#pragma once

#include <hs/gameplay/game_simulation.hpp>
#include <hs/core/cooked_format.hpp>

#include "cast_runtime.hpp"
#include "relic_rule_table.hpp"
#include "damage_command_buffer.hpp"
#include "simulation_pipeline.hpp"
#include "status_state.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <iterator>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace hs::gameplay_detail
{

constexpr Tick Seconds(float value) noexcept
{
    return static_cast<Tick>(value * 60.0f + 0.5f);
}

constexpr float kTickSeconds = 1.0f / 60.0f;
constexpr Tick kInputBufferTicks = 9;
constexpr Tick kBasicArrowReleaseTicks = 14;
constexpr Tick kSkillArrowReleaseTicks = 6;
constexpr Tick kRecoilClipTicks = 41;
constexpr Tick kAnimationBlendOutTicks = 6;
constexpr float kBasicAttackRate = 60.0f /
                                   static_cast<float>(kRecoilClipTicks +
                                                      kAnimationBlendOutTicks);
constexpr float kAttackSpeedPerPoint = 0.07625f;

constexpr Tick AnimationMarkerTicks(Tick source_marker, Tick playback_ticks) noexcept
{
    return std::max<Tick>(
        1, (source_marker * playback_ticks + kRecoilClipTicks / 2) /
               kRecoilClipTicks);
}

constexpr float kPi = std::numbers::pi_v<float>;
constexpr int kGridDimension = 30;
constexpr float kGridCellSize = 4.0f;
constexpr std::size_t kGridCellCount = kGridDimension * kGridDimension;
constexpr std::uint8_t kNoTelemetrySource = 0xFF;
constexpr std::uint64_t kPlayerRenderId = 1ull << 60;
constexpr std::uint64_t kEnemyRenderId = 2ull << 60;
constexpr std::uint64_t kProjectileRenderId = 3ull << 60;
constexpr std::uint64_t kAreaRenderId = 4ull << 60;
constexpr std::uint64_t kPickupRenderId = 5ull << 60;


struct PlayerState
{
    Float2 position{};
    Float2 previous_position{};
    Float2 aim{0.0f, 1.0f};
    Float2 facing{0.0f, 1.0f};
    Float2 move_target{};
    bool has_move_target{};
    std::int32_t health{100};
    std::int32_t max_health{100};
    float attack{10.0f};
    float attack_speed{kBasicAttackRate};
    float move_speed{5.0f};
    float magnet_radius{3.0f};
    std::uint32_t level{1};
    std::uint32_t experience{};
    std::uint32_t pending_levels{};
    std::uint8_t pending_stat_points{};
    std::uint8_t level_rerolls{3};
    std::uint8_t relic_rerolls{3};
    std::array<std::uint8_t, kStatCount> stats{};
    std::array<SkillKind, 4> loadout{SkillKind::Count, SkillKind::Count,
                                     SkillKind::Count, SkillKind::Count};
    std::array<std::uint8_t, kCombatSkillCount> skill_levels{};
    std::array<std::uint8_t, kCombatSkillCount> upgrades{};
    std::array<Tick, kActiveSkillCount> cooldowns{};
    std::uint16_t relic_mask{};
    Tick next_basic_attack{};
    Tick basic_attack_animation_start{};
    Tick basic_attack_animation_until{};
    std::uint64_t basic_sequence{};
    std::uint64_t basic_arrow_sequence{};
    Tick active_basic_empower_until{};
    float movement_since_echo{};
    Float2 one_second_ago{};
    Tick last_position_sample{};
    SkillKind last_active{SkillKind::Count};
    Tick last_active_tick{};
    Tick alternating_refund_until{};
    SkillKind alternating_refund_source{SkillKind::Count};
    Tick damage_relic_ready{};
    Tick bleed_heal_ready{};
    std::uint32_t revives_used{};
    Tick revive_invulnerable_until{};
    std::uint32_t kill_cooldown_progress{};
    std::uint32_t combat_hit_progress{};
    bool charging{};
    SkillKind charging_skill{SkillKind::Count};
    std::uint8_t charging_slot{0xFF};
    Tick charge_start{};
    Tick retreat_until{};
    Float2 retreat_velocity{};
    Tick active_cast_tick{};
    Tick active_animation_start{};
    Tick active_animation_until{};
    float locomotion_blend{};
    Tick retreat_followup_tick{};
    Float2 retreat_followup_direction{};
    std::uint64_t retreat_followup_cast{};
    std::uint8_t retreat_upgrade_mask{};
    bool retreat_landing_pending{};
    SkillKind forced_move_skill{SkillKind::Count};
    Tick next_active_refund_until{};
    SkillKind next_active_refund_source{SkillKind::Count};
    SkillKind buffered_skill{SkillKind::Count};
    Tick buffered_skill_expires{};
};

struct EnemyActor
{
    EntityId id{};
    std::uint64_t random_key{};
    EnemyKind kind{EnemyKind::Melee};
    std::optional<BossKind> boss;
    Float2 position{};
    Float2 previous_position{};
    Float2 velocity{};
    Float2 displacement_per_tick{};
    Tick displacement_ticks{};
    Tick spawned_tick{};
    std::int32_t health{};
    std::int32_t max_health{};
    std::int32_t damage{};
    float move_speed{};
    float attack_range{};
    float warning_extent{};
    Tick warning_ticks{};
    Tick attack_cooldown_ticks{};
    Tick next_attack{};
    Tick attack_resolve{};
    std::uint64_t attack_cast_id{};
    Float2 locked_aim{};
    Tick pattern_ready{};
    std::uint8_t last_pattern{255};
    std::uint8_t repeat_count{};
    std::uint8_t final_phase{1};
    Tick invulnerable_until{};
    Tick dash_until{};
    std::int32_t dash_damage{};
    bool dash_hit{};
    std::uint8_t phase_pattern_count{};
    StatusState status;
    SkillKind last_damage_skill{SkillKind::Count};
    EffectOrigin last_damage_origin{EffectOrigin::Original};
    std::uint64_t last_damage_cast{};
    std::uint8_t last_damage_upgrade{kNoTelemetrySource};
    SkillKind marked_by_skill{SkillKind::Count};
    float marked_damage_coefficient{};
    Tick mark_expires{};
    Tick bleed_burn_ready{};
    Tick different_skill_ready{};
    SkillKind last_active_hit{SkillKind::Count};
    Tick last_active_hit_tick{};
    bool attacking{};
    bool dead{};
};

struct ProjectileActor
{
    EntityId id{};
    bool player_owned{};
    Float2 position{};
    Float2 previous_position{};
    Float2 velocity{};
    Tick spawned_tick{};
    float remaining_range{};
    float radius{};
    std::int32_t damage{};
    SkillKind skill{SkillKind::BasicAttack};
    std::uint8_t upgrade_mask{};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint64_t cast_id{};
    std::uint8_t source_upgrade{kNoTelemetrySource};
    std::uint8_t source_relic{kNoTelemetrySource};
    std::uint8_t source_enemy{kNoTelemetrySource};
    std::uint16_t pierce_remaining{};
    std::uint8_t bounce_remaining{};
    std::uint32_t hit_count{};
    std::vector<std::uint64_t> hit_ids;
    float explosion_radius{};
    std::int32_t explosion_damage{};
    std::uint8_t explosion_source_upgrade{kNoTelemetrySource};
    std::uint8_t bleed_stacks{};
    bool burn{};
    bool slow{};
    bool returning{};
    bool homing{};
    std::uint64_t homing_target{};
    bool full_charge{};
    float charge_ratio{};
    float carried_burn_attack{};
    Tick carried_burn_expires{};
    bool dead{};
};

enum class AreaKind : std::uint8_t
{
    Damage,
    Slow,
    Trap,
    EnemyDamage,
};

struct AreaActor
{
    EntityId id{};
    AreaKind kind{AreaKind::Damage};
    Float2 position{};
    Float2 direction{0.0f, 1.0f};
    float radius{};
    float half_length{};
    float effect_radius{};
    std::int32_t damage{};
    Tick active_tick{};
    Tick next_tick{};
    Tick expires{};
    Tick interval{30};
    SkillKind skill{SkillKind::ArrowRain};
    std::uint8_t upgrade_mask{};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint64_t cast_id{};
    std::uint8_t source_upgrade{kNoTelemetrySource};
    std::uint8_t source_relic{kNoTelemetrySource};
    std::uint8_t source_enemy{kNoTelemetrySource};
    float slow_reduction{};
    Tick slow_duration{};
    std::uint8_t trigger_count{};
    float ring_inner_radius{};
    float ring_outer_radius{};
    float safe_gap_degrees{};
    std::uint8_t safe_gap_count{};
    bool applies_burn{};
    bool dead{};
};

struct PickupActor
{
    EntityId id{};
    PickupKind kind{PickupKind::Experience};
    Float2 position{};
    std::uint32_t value{};
    Tick spawned_tick{};
    bool guaranteed_boss_chest{};
    bool attracted_by_magnet_stat{};
    bool globally_attracted{};
    bool dead{};
};

enum class ScheduledKind : std::uint8_t
{
    Projectile,
    Explosion,
    Area,
    Volley,
};

struct ScheduledAction
{
    Tick due{};
    ScheduledKind kind{};
    SkillKind skill{SkillKind::BasicAttack};
    Float2 position{};
    Float2 direction{0.0f, 1.0f};
    float damage_coefficient{};
    float radius{};
    float duration{};
    std::uint8_t projectile_count{1};
    std::uint8_t upgrade_mask{};
    EffectOrigin origin{EffectOrigin::Derived};
    std::uint64_t cast_id{};
    std::uint8_t source_upgrade{kNoTelemetrySource};
    std::uint8_t source_relic{kNoTelemetrySource};
};

enum class BossActionKind : std::uint8_t
{
    Dash,
    Volley,
    Area,
    Shockwave,
};

struct BossAction
{
    Tick due{};
    BossActionKind kind{};
    std::uint64_t boss_id{};
    Float2 position{};
    Float2 direction{};
    float speed{};
    float distance{};
    std::int32_t damage{};
    std::uint8_t projectile_count{};
    float arc_degrees{};
    float angle_offset{};
    float radius{};
    float duration{};
    std::uint64_t cast_id{};
};

struct CastHitRecord
{
    std::uint64_t cast_id{};
    std::uint64_t target{};
    std::uint8_t count{};
};

struct AreaHitRecord
{
    std::uint64_t area_id{};
    std::uint64_t target{};
    std::uint8_t count{};
};

struct BalanceObserver
{
    BalanceTelemetry metrics{};
    std::vector<std::uint64_t> enemy_hit_casts;
    std::vector<std::uint64_t> player_hit_casts;

    void Reset()
    {
        metrics = {};
        enemy_hit_casts.clear();
        player_hit_casts.clear();
    }
};

struct ActiveWave
{
    Tick start{};
    Tick duration{};
    std::uint16_t total{};
    std::uint16_t emitted{};
};

struct PendingBossSpawn
{
    BossKind kind{};
    Float2 position{};
    Tick due{};
};

inline float LengthSquared(Float2 value) noexcept
{
    return value.x * value.x + value.y * value.y;
}

inline float Length(Float2 value) noexcept
{
    return std::sqrt(LengthSquared(value));
}

inline Float2 Normalize(Float2 value, Float2 fallback = {0.0f, 1.0f}) noexcept
{
    const auto length = Length(value);
    if (length <= 0.00001f)
    {
        return fallback;
    }
    return {value.x / length, value.y / length};
}

inline Float2 Add(Float2 left, Float2 right) noexcept
{
    return {left.x + right.x, left.y + right.y};
}

inline Float2 Subtract(Float2 left, Float2 right) noexcept
{
    return {left.x - right.x, left.y - right.y};
}

inline Float2 Multiply(Float2 value, float scalar) noexcept
{
    return {value.x * scalar, value.y * scalar};
}

inline Float2 Rotate(Float2 value, float degrees) noexcept
{
    const auto radians = degrees * kPi / 180.0f;
    const auto sine = std::sin(radians);
    const auto cosine = std::cos(radians);
    return {value.x * cosine + value.y * sine,
            -value.x * sine + value.y * cosine};
}

inline float DistanceSquared(Float2 left, Float2 right) noexcept
{
    return LengthSquared(Subtract(left, right));
}

inline bool HasUpgrade(std::uint8_t mask, std::uint8_t one_based_index) noexcept
{
    return (mask & (1u << (one_based_index - 1u))) != 0;
}

inline bool HasRelic(std::uint16_t mask, RelicKind relic) noexcept
{
    return (mask & (1u << static_cast<unsigned>(relic))) != 0;
}

inline std::int32_t RoundDamage(float value) noexcept
{
    return std::max(1, static_cast<std::int32_t>(std::floor(value + 0.5f)));
}

inline std::uint32_t ExperienceForLevel(std::uint32_t level) noexcept
{
    const auto offset = level - 1u;
    return 20u + static_cast<std::uint32_t>(
                     std::floor(3.5 * offset + 0.75 * offset * offset));
}

inline std::uint64_t Mix(std::uint64_t value) noexcept
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

inline void HashBytes(GameplayChecksum &hash, const void *data, std::size_t size) noexcept
{
    const auto *bytes = static_cast<const std::byte *>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= std::to_integer<std::uint8_t>(bytes[index]);
        hash *= 1099511628211ull;
    }
}

inline bool SegmentCircle(Float2 from, Float2 to, Float2 center, float radius) noexcept
{
    const auto segment = Subtract(to, from);
    const auto to_center = Subtract(center, from);
    const auto length_squared = LengthSquared(segment);
    const auto interpolation = length_squared > 0.0f
                                   ? std::clamp((to_center.x * segment.x +
                                                 to_center.y * segment.y) /
                                                    length_squared,
                                                0.0f, 1.0f)
                                   : 0.0f;
    return DistanceSquared(Add(from, Multiply(segment, interpolation)), center) <=
           radius * radius;
}

} // namespace hs::gameplay_detail

namespace hs
{

using namespace gameplay_detail;

struct GameSimulation::SimulationWorld
{
    SimulationRules rules{SimulationRules::Defaults()};
    SimulationConfig config{};
    PlayerState player{};
    std::vector<EnemyActor> enemies;
    std::vector<ProjectileActor> projectiles;
    std::vector<AreaActor> areas;
    std::vector<PickupActor> pickups;
    std::vector<EnemyActor> pending_enemy_spawns;
    std::vector<ProjectileActor> pending_projectile_spawns;
    std::vector<AreaActor> pending_area_spawns;
    DamageCommandBuffer combat;
    RelicRuleTable relic_rules;
    std::vector<ScheduledAction> scheduled_actions;
    std::vector<BossAction> boss_actions;
    std::vector<CastHitRecord> cast_hits;
    std::vector<AreaHitRecord> area_hits;
    std::vector<CastRuntime> cast_runtimes;
    std::vector<DomainSignal> domain_signals;
    std::array<std::vector<std::size_t>, kGridCellCount> enemy_grid;
    std::vector<std::size_t> collision_candidates;
    std::array<CardView, 3> cards{};
    std::uint8_t card_count{};
    std::vector<ActiveWave> waves;
    std::vector<PendingBossSpawn> pending_boss_spawns;
    InputFrame current_input{};
    Tick tick{};
    Tick growth_ticks{};
    Tick boss_fight_ticks{};
    Sequence event_sequence{};
    std::uint64_t next_entity_id{1};
    std::uint64_t next_enemy_random_key{1};
    std::uint64_t next_cast_id{1};
    std::uint64_t next_enemy_attack_id{1};
    std::uint64_t damage_sequence{};
    float spawn_accumulator{};
    std::uint32_t normal_chest_kills{};
    std::uint32_t heal_pickup_misses{};
    std::uint32_t magnet_pickup_misses{};
    std::uint64_t level_reroll_sequence{};
    std::uint64_t relic_reroll_sequence{};
    std::uint32_t kills{};
    std::uint64_t damage_dealt{};
    std::array<std::uint64_t, kCombatSkillCount> damage_by_skill{};
    std::uint64_t damage_taken{};
    std::uint64_t healing{};
    BalanceObserver balance_observer{};
    BalanceTelemetry &balance{balance_observer.metrics};
    GameplayChecksum checksum{};
    SessionPhase session_phase{SessionPhase::Playing};
    bool initialized{};
    bool final_boss_spawned{};
    std::uint8_t selection_input_guard_frames{};
    bool selection_waiting_for_release{};
    SimulationPhaseId pipeline_phase{SimulationPhaseId::GameplayHash};

    SimulationWorld();
    std::uint64_t Random(std::uint64_t entity, std::uint64_t purpose) const noexcept;
    float RandomUnit(std::uint64_t entity, std::uint64_t purpose) const noexcept;
    EntityId AllocateEntityId() noexcept;
    float EffectiveAttack() const noexcept;
    float EffectiveMoveSpeed() const noexcept;
    float EffectiveAttackSpeed() const noexcept;
    void RecordUpgradeEffect(SkillKind skill, std::uint8_t upgrade,
                             UpgradeEffectMetric metric,
                             std::uint64_t amount = 1) noexcept;
    void RecordRelicEffect(RelicKind relic, UpgradeEffectMetric metric,
                           std::uint64_t amount = 1) noexcept;
    void RecordUpgradeRelicSynergy(SkillKind skill, std::uint8_t upgrade,
                                   RelicKind relic,
                                   UpgradeRelicSynergyMetric metric,
                                   std::uint64_t amount = 1) noexcept;
    void RecordUpgradeDamage(SkillKind skill, std::uint8_t upgrade,
                             std::uint64_t amount) noexcept;
    void RecordUpgradeDisplacement(SkillKind skill, std::uint8_t upgrade,
                                   Float2 before, Float2 after) noexcept;
    void QueueEnemyDisplacement(EnemyActor &enemy, Float2 displacement) noexcept;
    float EffectiveMagnetRadius() const noexcept;
    static std::size_t EnemyTelemetryIndex(const EnemyActor &enemy) noexcept;
    Tick EffectiveCooldownTicks(SkillKind skill) const noexcept;
    EnemyActor *FindEnemy(std::uint64_t id) noexcept;
    const EnemyActor *FindEnemy(std::uint64_t id) const noexcept;
    bool IsBoss(const EnemyActor &enemy) const noexcept;
    std::uint32_t NormalEnemyCount() const noexcept;
    std::uint32_t BossCount() const noexcept;
    std::uint32_t ProjectileCount(bool player_owned) const noexcept;
    void EmitSignal(DomainSignalKind kind, Float2 position,
                    std::uint8_t context = 0);
    void EmitVfx(DomainSignalKind effect, Float2 position,
                 Float2 direction = {0.0f, 1.0f}, float scale = 1.0f,
                 float height = 0.3f, std::uint8_t context = 0);
    void EmitVfxLine(DomainSignalKind effect, Float2 start, Float2 end,
                     float height = 0.75f);
    bool SpawnEnemy(EnemyKind kind, Float2 position,
                    std::uint64_t random_key = 0);
    bool SpawnBoss(BossKind kind, Tick warning_ticks = Seconds(1.5f));
    void CommitBossSpawns();
    void SpawnPickup(PickupKind kind, Float2 position, std::uint32_t value,
                     bool guaranteed = false);
    ProjectileActor *FireProjectile(SkillKind skill, Float2 position, Float2 direction,
                                    float coefficient, EffectOrigin origin,
                                    std::uint64_t cast_id,
                                    std::uint8_t applied_upgrade_mask,
                                    bool player_owned = true,
                                    std::uint8_t source_upgrade = kNoTelemetrySource,
                                    std::uint8_t source_relic = kNoTelemetrySource,
                                    std::uint8_t source_enemy = kNoTelemetrySource);
    AreaActor *SpawnArea(AreaKind kind, SkillKind skill, Float2 position, float radius,
                         float coefficient, float duration, float activation_delay,
                         EffectOrigin origin, std::uint64_t cast_id,
                         std::uint8_t applied_upgrade_mask,
                         float slow_reduction = 0.0f, float slow_duration = 0.0f,
                         float effect_radius = 0.0f, bool applies_burn = false,
                         std::uint8_t source_upgrade = kNoTelemetrySource,
                         std::uint8_t source_relic = kNoTelemetrySource,
                         std::uint8_t source_enemy = kNoTelemetrySource);
    void ScheduleAction(const ScheduledAction &action);
    void QueueDamage(std::uint64_t target, std::int32_t amount, SkillKind skill,
                    EffectOrigin origin, std::uint64_t cast_id,
                    std::uint8_t bleed = 0, bool burn = false,
                    float slow = 0.0f, Tick slow_duration = 0,
                    std::uint8_t source_upgrade = kNoTelemetrySource,
                    std::uint8_t source_relic = kNoTelemetrySource,
                    std::uint8_t source_enemy = kNoTelemetrySource);
    void QueueAreaDamage(Float2 position, float radius, std::int32_t damage, SkillKind skill,
                    EffectOrigin origin, std::uint64_t cast_id,
                    std::uint8_t bleed = 0, bool burn = false,
                    float slow = 0.0f, Tick slow_duration = 0,
                    std::uint8_t source_upgrade = kNoTelemetrySource,
                    std::uint8_t source_relic = kNoTelemetrySource);
    void StartSession();
    void ProcessInput();
    void CastBasicAttack();
    bool CastSkill(SkillKind skill);
    bool TryBeginSkill(SkillKind skill);
    void ConsumeBufferedSkill();
    void ReleaseChargedShot();
    EnemyActor *NearestEnemy(Float2 position, float radius,
                             std::span<const std::uint64_t> excluded = {}) noexcept;
    const EnemyActor *NearestEnemy(Float2 position, float radius) const noexcept;
    void RunPipeline();
    void CastAttackPhase();
    void MovementPhase();
    void SpatialGridPhase();
    bool AlreadyHit(const ProjectileActor &projectile, std::uint64_t target) const noexcept;
    void RecordHit(ProjectileActor &projectile, std::uint64_t target);
    std::uint8_t IncrementCastHit(std::uint64_t cast, std::uint64_t target);
    std::uint8_t IncrementAreaHit(std::uint64_t area, std::uint64_t target);
    CastRuntime &FindOrCreateCastRuntime(std::uint64_t cast, SkillKind skill);
    void OnProjectileHit(ProjectileActor &projectile, EnemyActor &enemy);
    void ExplodeProjectile(ProjectileActor &projectile);
    void CollisionHitPhase();
    void ApplyBleed(EnemyActor &enemy, float attack, std::uint8_t stacks,
                    SkillKind source_skill = SkillKind::BasicAttack,
                    std::uint8_t source_upgrade = kNoTelemetrySource,
                    std::uint8_t source_relic = kNoTelemetrySource);
    void ApplyBurn(EnemyActor &enemy, float attack, bool propagated = false,
                   Tick duration = 240,
                   SkillKind source_skill = SkillKind::BasicAttack,
                   std::uint8_t source_upgrade = kNoTelemetrySource,
                   std::uint8_t source_relic = kNoTelemetrySource);
    void DamageStatusPhase();
    std::uint64_t Heal(std::int32_t amount, bool emit_signal = true);
    void CancelChargedShot() noexcept;
    std::uint32_t EnemyExperience(const EnemyActor &enemy) const noexcept;
    void HandleEnemyDeath(EnemyActor &enemy);
    void DeathDropPhase();
    void CollectPickup(PickupActor &pickup);
    SkillTagMask BuildTags() const;
    void WeightedShuffle(std::vector<CardView> &candidates,
                         std::uint64_t sequence, std::uint64_t purpose);
    void GenerateLevelCards(bool exclude_current = false);
    void GuardSelectionInput();
    void GenerateRelicCards(bool exclude_current = false);
    bool SelectCard(std::size_t index);
    void AssignStat(StatKind stat);
    void AssignAutomaticStats();
    void XpCardPhase();
    void CommitSpawnBarrier();
    void CommitAbilitySpawnBarrier();
    void CommitCleanupBarrier();
    GameplayChecksum CalculateChecksum() const;
    void HandleRadialBasicAttack(Tick release_ticks, std::uint64_t cast_id);
    void HandleMovementEcho(Tick release_ticks, std::uint64_t cast_id);
    void HandleAlternatingSkills(SkillKind skill, std::size_t cooldown_index);
    void HandleDamageKnockback();
    void HandleCombatHitChain(const DamageCommand &event);
    void HandleBleedBurnExplosion(EnemyActor &enemy,
                                  const DamageCommand &event,
                                  std::uint8_t source_upgrade);
    void HandleDifferentSkillTracker(EnemyActor &enemy,
                                     const DamageCommand &event);
    void HandleKillCooldownSurge();
    void HandleBasicKillTracker(EnemyActor &enemy, CastRuntime *runtime);
    void HandleBleedKillHeal(const EnemyActor &enemy);
    void HandleBurnPropagation(const EnemyActor &enemy);
    bool HandleOnceRevive();
    void DispatchBasicAttackRules(Tick release_ticks, std::uint64_t cast_id);
    void DispatchAbilityRules(SkillKind skill, std::size_t cooldown_index);
    void DispatchPlayerDamagedRules();
    void DispatchAfterDamageRules(EnemyActor &enemy, const DamageCommand &event);
    void DispatchStatusAppliedRules(EnemyActor &enemy,
                                    const DamageCommand &event,
                                    std::uint8_t source_upgrade);
    void DispatchEnemyKilledRules(EnemyActor &enemy, CastRuntime *runtime);
    bool DispatchPlayerDeathRules();
    void SessionTimerPhase();
    const SpawnStage &CurrentSpawnStage() const noexcept;
    Float2 SpawnPosition(std::uint64_t salt) const noexcept;
    EnemyKind ChooseEnemyKind(std::uint64_t salt) const noexcept;
    void SpawnPhase();
    float SlowMultiplier(EnemyActor &enemy);
    void BossAi(EnemyActor &boss);
    void AiIntentPhase();
};

} // namespace hs
