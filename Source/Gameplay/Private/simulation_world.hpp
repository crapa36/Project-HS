#pragma once

#include <hs/gameplay/game_simulation.hpp>
#include "navigation_grid.hpp"
#include <hs/core/cooked_format.hpp>
#include <hs/core/fixed_step_clock.hpp>

#include "cast_runtime.hpp"
#include "balance_observer.hpp"
#include "damage_command_buffer.hpp"
#include "simulation_pipeline.hpp"
#include "spatial_grid.hpp"
#include "status_state.hpp"
#include "actor_storage.hpp"
#include "pickup_work.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <thread>

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

constexpr Tick AnimationMarkerTicks(Tick source_marker, Tick playback_ticks) noexcept
{
    return std::max<Tick>(
        1, (source_marker * playback_ticks + kRecoilClipTicks / 2) /
               kRecoilClipTicks);
}

constexpr float kPi = std::numbers::pi_v<float>;
constexpr std::uint8_t kBossTelemetryBase = 3;
constexpr std::uint64_t kPlayerRenderId = 1ull << 60;
constexpr std::uint64_t kEnemyRenderId = 2ull << 60;
constexpr std::uint64_t kProjectileRenderId = 3ull << 60;
constexpr std::uint64_t kAreaRenderId = 4ull << 60;
constexpr std::uint64_t kPickupRenderId = 5ull << 60;

inline std::uint64_t TakeSimulationIdentity(std::uint64_t &next)
{
    if (next == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Simulation identity exhausted.");
    return next++;
}

inline std::uint64_t NextSimulationSequence(std::uint64_t &value)
{
    if (value == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Simulation sequence exhausted.");
    return ++value;
}

inline std::uint64_t PendingSimulationSequence(std::uint64_t value)
{
    if (value == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Simulation sequence exhausted.");
    return value + 1;
}


struct PlayerState
{
    Float2 position{};
    Float2 previous_position{};
    DiscMotion2D tick_motion; // Derived collision cache, rebuilt before every CollisionHit phase.
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
    RelicMask relic_mask{};
    Tick next_basic_attack{};
    Tick basic_attack_animation_start{};
    Tick basic_attack_animation_until{};
    std::uint64_t basic_attack_cast_id{};
    Tick basic_attack_release_tick{};
    std::uint64_t basic_sequence{};
    std::uint64_t basic_arrow_sequence{};
    Tick active_basic_empower_until{};
    float movement_since_echo{};
    SkillKind last_active{SkillKind::Count};
    Tick last_active_tick{};
    Tick alternating_refund_until{};
    SkillKind alternating_refund_source{SkillKind::Count};
    Tick damage_relic_ready{};
    Tick bleed_heal_ready{};
    std::uint32_t revives_used{};
    Tick revive_invulnerable_until{};
    Tick revive_invulnerable_started{};
    std::uint32_t kill_cooldown_progress{};
    std::uint32_t combat_hit_progress{};
    std::uint32_t projectile_cadence_progress{};
    std::uint32_t hit_streak_progress{};
    Tick pre_damage_guard_ready{};
    Tick area_resonance_ready{};
    Tick pickup_reward_until{};
    Tick low_health_survival_ready{};
    bool charging{};
    SkillKind charging_skill{SkillKind::Count};
    std::uint8_t charging_slot{0xFF};
    Tick charge_start{};
    std::uint64_t charge_cast_id{};
    Tick retreat_until{};
    Float2 retreat_velocity{};
    Tick active_cast_tick{};
    Tick active_animation_start{};
    Tick active_animation_until{};
    float locomotion_blend{};
    // Read-model presentation clock; no simulation decisions or checksum use it.
    Tick movement_stopped_tick{};
    bool locomotion_moving{};
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
    std::uint8_t buffered_skill_slot{0xFF};
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
    float ring_half_width{};
    float safe_gap_degrees{};
    std::uint8_t safe_gap_count{};
    bool applies_burn{};
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
    // Cosmetic schedule identity, excluded from the deterministic checksum.
    std::uint64_t visual_owner_id{};
    Tick visual_started{};
    // Scheduled member angle used to recover the frozen fan center at release.
    float visual_fan_angle{};
    std::uint64_t schedule_sequence{};
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
    Tick animation_started{};
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
    Tick interval{};
    std::uint8_t safe_gap_count{};
    float safe_gap_degrees{};
    std::uint64_t cast_id{};
    float half_width{};
    // Presentation metadata for the warning signal emitted when this action is scheduled.
    Sequence warning_sequence{};
    Tick warning_started{};
    std::uint64_t schedule_sequence{};
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
    // Presentation ownership; excluded from simulation checksum.
    Sequence warning_sequence{};
    Tick warning_started{};
};

struct PendingEnemySpawn
{
    EnemyKind kind{};
    Float2 position{};
    std::uint64_t random_key{};
    Tick due{};
    // Presentation ownership; excluded from simulation checksum.
    Sequence warning_sequence{};
    Tick warning_started{};
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

constexpr RelicMask RelicBit(RelicKind relic) noexcept
{
    return RelicMask{1} << static_cast<unsigned>(relic);
}

inline bool HasRelic(RelicMask mask, RelicKind relic) noexcept
{
    return (mask & RelicBit(relic)) != 0;
}

inline std::int32_t RoundDamage(float value) noexcept
{
    return std::max(1, static_cast<std::int32_t>(std::floor(value + 0.5f)));
}

inline std::int32_t RoundFinalDamage(float value, const SimulationRules &rules) noexcept
{
    return std::max(rules.stats.combat.minimum_final_damage,
                    static_cast<std::int32_t>(std::floor(value + 0.5f)));
}

inline std::uint32_t ExperienceForLevel(const SimulationRules &rules,
                                        std::uint32_t level) noexcept
{
    const auto offset = level > 0 ? static_cast<double>(level - 1u) : 0.0;
    const auto required = static_cast<double>(rules.progression.required_xp_base) +
                          std::floor(static_cast<double>(rules.progression.required_xp_linear) * offset +
                                     static_cast<double>(rules.progression.required_xp_quadratic) *
                                         offset * offset);
    return required <= 0.0
               ? 0u
               : required >= static_cast<double>((std::numeric_limits<std::uint32_t>::max)())
                     ? (std::numeric_limits<std::uint32_t>::max)()
                     : static_cast<std::uint32_t>(required);
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

#include "simulation_state.hpp"

namespace hs
{

using namespace gameplay_detail;

struct GameSimulation::SimulationWorld
{
    SimulationRules rules{SimulationRules::Defaults()};
    SimulationConfig config{};
    std::unique_ptr<ActorState> actors;
    std::unique_ptr<CombatState> combat_state;
    std::unique_ptr<NavigationState> navigation;
    std::unique_ptr<ProgressionState> progression;
    std::unique_ptr<TelemetryState> telemetry;
    InputFrame current_input{};
    Tick tick{};
    std::uint64_t session_id{};
    Tick growth_ticks{};
    Tick boss_fight_ticks{};
    Sequence event_sequence{};
    std::uint64_t next_entity_id{1};
    std::uint64_t next_enemy_random_key{1};
    std::uint64_t next_cast_id{1};
    std::uint64_t next_enemy_attack_id{1};
    std::uint64_t damage_sequence{};
    float spawn_accumulator{};
    GameplayChecksum checksum{};
    SessionPhase session_phase{SessionPhase::Playing};
    bool initialized{};
    bool final_boss_spawned{};
    std::uint8_t selection_input_guard_frames{};
    bool selection_waiting_for_release{};
    SimulationPhaseId pipeline_phase{SimulationPhaseId::GameplayHash};

    mutable SimulationTimingDiagnostics timings;
    class TimingScope
    {
      public:
        explicit TimingScope(SimulationTimingSample *sample) noexcept : sample_(sample)
        { if (sample_) started_ = FixedStepClock::Clock::now(); }
        ~TimingScope() noexcept { Finish(); }
        TimingScope(const TimingScope &) = delete;
        TimingScope &operator=(const TimingScope &) = delete;
        void Finish() noexcept
        {
            if (!sample_) return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                FixedStepClock::Clock::now() - started_).count();
            sample_->nanoseconds += static_cast<std::uint64_t>(std::max<std::int64_t>(elapsed, 0));
            ++sample_->calls;
            sample_ = nullptr;
        }
      private:
        SimulationTimingSample *sample_{};
        FixedStepClock::Clock::time_point started_{};
    };
    [[nodiscard]] TimingScope Measure(SimulationTimingId id) const noexcept
    {
        return TimingScope(config.capture_timings && timings.captured
            ? &timings.scopes[static_cast<std::size_t>(id)] : nullptr);
    }
    void ResetTimings() noexcept
    {
        if (!config.capture_timings) { timings.tick = tick; return; }
        const auto calibration = timings.empty_scope_calibration;
        timings = {};
        timings.tick = tick;
        timings.captured = config.capture_timings;
        timings.empty_scope_calibration = calibration;
    }
    Float2 TimedNavigationWaypoint(NavigationGrid &grid, Float2 from, Float2 target) const
    {
        auto timing = Measure(SimulationTimingId::NavigationQuery);
        return grid.NextWaypoint(from, target);
    }

    struct ObservationState
    {
        std::uint32_t mutation_depth{}, active_borrows{};
        bool small_facts_dirty{true}, checksum_dirty{true};
        std::optional<std::thread::id> owner_thread;
    };
    mutable ObservationState observation;
    struct SmallReadFacts
    {
        SessionProbe session;
        PlayerView player;
        float effective_attack{}, effective_attack_speed{}, effective_move_speed{}, effective_magnet_radius{};
        float charge_range{}, charge_radius{}, charge_ratio{};
        Tick charge_normal_ready_tick{};
        std::array<SkillRuntimeView, kCombatSkillCount> skills{};
        std::array<WaveView, 5> waves{};
        std::size_t enemies{}, projectiles{}, areas{}, pickups{}, visual_links{}, mini_bombs{}, spawn_warnings{}, deliveries{};
    };
    mutable SmallReadFacts read_facts;
    void RequireOwnerThread() const;
    struct MutationScope
    {
        SimulationWorld &world;
        explicit MutationScope(SimulationWorld &world);
        ~MutationScope() noexcept;
        MutationScope(const MutationScope &) = delete;
        MutationScope &operator=(const MutationScope &) = delete;
    };
    void InitializeContentReadFacts();
    void RefreshSmallReadFacts() const;
    SessionProbe BuildSessionProbe() const;
    TickReadView MakeTickReadView() const;

    SimulationWorld();
    void UpdateChecksum()
    {
        if (config.capture_checksums && observation.checksum_dirty) checksum = CalculateChecksum();
        if (!config.capture_checksums) checksum = 0;
        observation.checksum_dirty = false;
    }
    BalanceTelemetry &Metrics() noexcept { return telemetry->Metrics(); }
    const BalanceTelemetry &Metrics() const noexcept { return telemetry->Metrics(); }
    void InitializePlayerState();
    void ApplyUiAction(const UiAction &action);
    void RebuildNavigation();
    std::span<const ArenaObstacle2D> ArenaObstacles() const noexcept
    {
        return {rules.arena_obstacles.data(), rules.arena_obstacle_count};
    }
    std::uint64_t Random(std::uint64_t entity, std::uint64_t purpose) const noexcept;
    float RandomUnit(std::uint64_t entity, std::uint64_t purpose) const noexcept;
    EntityId AllocateEntityId();
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
    void QueueEnemyDisplacement(EnemyRow enemy, Float2 displacement) noexcept;
    float EffectiveMagnetRadius() const noexcept;
    static std::size_t EnemyTelemetryIndex(ConstEnemyRow enemy) noexcept;
    Tick EffectiveCooldownTicks(SkillKind skill) const noexcept;
    std::optional<EnemyRow> FindEnemy(std::uint64_t id) noexcept;
    std::optional<ConstEnemyRow> FindEnemy(std::uint64_t id) const noexcept;
    bool IsBoss(ConstEnemyRow enemy) const noexcept;
    std::uint32_t NormalEnemyCount() const noexcept;
    std::uint32_t BossCount() const noexcept;
    std::uint32_t ProjectileCount(bool player_owned) const noexcept;
    void EmitSignal(DomainSignalKind kind, Float2 position,
                    std::uint8_t context = 0);
    void EmitVfx(DomainSignalKind effect, Float2 position,
                 Float2 direction = {0.0f, 1.0f}, float scale = 1.0f,
                 float height = 0.3f, std::uint8_t context = 0,
                 std::uint64_t source_entity_id = 0,
                 DomainSignalGeometry geometry = {});
    void PruneVisualLinks();
    void EmitVfxLine(DomainSignalKind effect, Float2 start, Float2 end,
                     float height = 0.75f);
    void EmitUpgradeVisual(SkillKind skill, std::uint8_t ordinal,
                           UpgradeVisualStage stage, Float2 position,
                           Float2 direction, std::uint64_t cast_id,
                           std::uint64_t owner_id,
                           DomainSignalGeometry geometry = {});
    bool SpawnEnemy(EnemyKind kind, Float2 position,
                    std::uint64_t random_key = 0);
    void QueueEnemySpawn(EnemyKind kind, std::uint64_t random_key);
    bool SpawnBoss(BossKind kind, Tick warning_ticks);
    void CommitBossSpawns();
    void CommitDelayedEnemySpawns();
    void SpawnPickup(PickupKind kind, Float2 position, std::uint32_t value,
                     bool guaranteed = false);
    std::optional<ProjectileRow> FireProjectile(SkillKind skill, Float2 position, Float2 direction,
                                    float coefficient, EffectOrigin origin,
                                    std::uint64_t cast_id,
                                    std::uint8_t applied_upgrade_mask,
                                    bool player_owned = true,
                                    std::uint8_t source_upgrade = kNoTelemetrySource,
                                    std::uint8_t source_relic = kNoTelemetrySource,
                                    std::uint8_t source_enemy = kNoTelemetrySource,
                                    std::uint64_t source_entity_id = 0);
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
                    std::uint8_t source_enemy = kNoTelemetrySource,
                    std::uint64_t originating_area_id = 0,
                    Float2 incoming_direction = {}, bool shockwave_contact = false,
                    Float2 impact_position = {});
    void QueueAreaDamage(Float2 position, float radius, std::int32_t damage, SkillKind skill,
                    EffectOrigin origin, std::uint64_t cast_id,
                    std::uint8_t bleed = 0, bool burn = false,
                    float slow = 0.0f, Tick slow_duration = 0,
                    std::uint8_t source_upgrade = kNoTelemetrySource,
                    std::uint8_t source_relic = kNoTelemetrySource,
                    std::uint64_t originating_area_id = 0);
    void StartSession();
    void ProcessInput();
    void CastBasicAttack();
    bool CastSkill(SkillKind skill);
    bool TryBeginSkill(SkillKind skill);
    void ConsumeBufferedSkill();
    void ReleaseChargedShot();
    void CollectEnemies(Float2 position, float radius, EnemyQuery &query) const;
    void CollectEnemies(Float2 minimum, Float2 maximum, EnemyQuery &query,
                        bool canonical_order = true) const;
    std::optional<EnemyRow> NearestEnemy(Float2 position, float radius,
                            std::span<const std::uint64_t> excluded = {});
    std::optional<ConstEnemyRow> NearestEnemy(Float2 position, float radius) const;
    void RunPipeline();
    void CastAttackPhase();
    void MovementPhase();
    void SpatialGridPhase();
    bool AlreadyHit(ConstProjectileRow projectile, std::uint64_t target) const noexcept;
    void RecordHit(ProjectileRow projectile, std::uint64_t target);
    std::uint8_t IncrementCastHit(std::uint64_t cast, std::uint64_t target);
    std::uint8_t IncrementAreaHit(std::uint64_t area, std::uint64_t target);
    CastRuntime &FindOrCreateCastRuntime(std::uint64_t cast, SkillKind skill);
    CastRuntime *FindCastRuntime(std::uint64_t cast) noexcept;
    // True means the remaining tick must be swept again in the new direction.
    bool OnProjectileHit(ProjectileRow projectile, EnemyRow enemy,
                         Float2 contact_position);
    void ExplodeProjectile(ProjectileRow projectile);
    void CollisionHitPhase();
    void ApplyBleed(EnemyRow enemy, float attack, std::uint8_t stacks,
                    SkillKind source_skill = SkillKind::BasicAttack,
                    std::uint8_t source_upgrade = kNoTelemetrySource,
                    std::uint8_t source_relic = kNoTelemetrySource);
    void ApplyBurn(EnemyRow enemy, float attack, bool propagated = false,
                   Tick duration = 240,
                   SkillKind source_skill = SkillKind::BasicAttack,
                   std::uint8_t source_upgrade = kNoTelemetrySource,
                   std::uint8_t source_relic = kNoTelemetrySource);
    void DamageStatusPhase();
    std::uint64_t Heal(std::int32_t amount, bool emit_signal = true);
    void CancelChargedShot(bool released = false) noexcept;
    std::uint32_t EnemyExperience(ConstEnemyRow enemy) const noexcept;
    void HandleEnemyDeath(EnemyRow enemy);
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
    void RetireBalanceHitCauses();
    GameplayChecksum CalculateChecksum() const;
    [[nodiscard]] EnemyView EnemyReadView(ConstEnemyRow enemy) const;
    [[nodiscard]] BossActionView BossActionReadView(const BossAction &action) const;
    void HandleRadialBasicAttack(Tick release_ticks, std::uint64_t cast_id);
    void HandleMovementEcho(Tick release_ticks, std::uint64_t cast_id);
    void HandleAlternatingSkills(SkillKind skill, std::size_t cooldown_index);
    void HandleDamageKnockback();
    void HandleCombatHitChain(const DamageCommand &event);
    void HandleProjectileCadenceReward();
    std::int32_t ApplyIncomingDamageRelics(std::int32_t amount);
    void HandleSlowSynergy(EnemyRow enemy, const DamageCommand &event);
    void HandleAreaResonance(EnemyRow enemy, const DamageCommand &event);
    void HandleBossPressure(EnemyRow enemy, const DamageCommand &event);
    void HandleHitStreakReward(EnemyRow enemy, const DamageCommand &event);
    void HandlePickupReward(PickupKind kind);
    void HandleBleedBurnExplosion(EnemyRow enemy,
                                  const DamageCommand &event,
                                  std::uint8_t source_upgrade);
    void HandleDifferentSkillTracker(EnemyRow enemy,
                                     const DamageCommand &event);
    void HandleKillCooldownSurge();
    void HandleBasicKillTracker(EnemyRow enemy, CastRuntime *runtime);
    void HandleBleedKillHeal(ConstEnemyRow enemy);
    void HandleBurnPropagation(ConstEnemyRow enemy);
    bool HandleOnceRevive();
    void DispatchBasicAttackRules(Tick release_ticks, std::uint64_t cast_id);
    void DispatchAbilityRules(SkillKind skill, std::size_t cooldown_index);
    void DispatchPlayerDamagedRules();
    void DispatchAfterDamageRules(EnemyRow enemy, const DamageCommand &event);
    void DispatchStatusAppliedRules(EnemyRow enemy,
                                    const DamageCommand &event,
                                    std::uint8_t source_upgrade);
    void DispatchEnemyKilledRules(EnemyRow enemy, CastRuntime *runtime);
    bool DispatchPlayerDeathRules();
    void SessionTimerPhase();
    const SpawnStage &CurrentSpawnStage() const noexcept;
    Float2 SpawnPosition(std::uint64_t salt, bool &used_fallback) const noexcept;
    EnemyKind ChooseEnemyKind(std::uint64_t salt) const noexcept;
    void SpawnPhase();
    float SlowMultiplier(ConstEnemyRow enemy) const;
    void BossAi(EnemyRow boss);
    void AiIntentPhase();
};

} // namespace hs
