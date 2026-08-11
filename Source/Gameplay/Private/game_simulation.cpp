#include <hs/gameplay/game_simulation.hpp>
#include <hs/core/cooked_format.hpp>

#include <flecs.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace hs
{
namespace
{

constexpr Tick Seconds(float value) noexcept
{
    return static_cast<Tick>(value * 60.0f + 0.5f);
}

constexpr float kTickSeconds = 1.0f / 60.0f;
constexpr Tick kInputBufferTicks = 9;
constexpr Tick kBasicArrowReleaseTicks = 6;
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

constexpr Tick kStatusTickInterval = 20;
constexpr Tick kStatusTickCount = 12;
constexpr float kBleedTickCoefficient = 0.25f;
constexpr float kBurnTickCoefficient = 0.45f;
static_assert(kBleedTickCoefficient * kStatusTickCount > 1.8f);
static_assert(kBurnTickCoefficient * kStatusTickCount > 1.8f);
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

struct DeterministicKey
{
    std::uint64_t value{};
};
struct PlayerTag
{
};
struct EnemyTag
{
};
struct BossTag
{
};
struct ProjectileTag
{
};
struct AreaTag
{
};
struct PickupTag
{
};

struct SlowEffect
{
    float reduction{};
    Tick expires{};
    SkillKind source_skill{SkillKind::Count};
    std::uint8_t source_upgrade{kNoTelemetrySource};
    std::uint8_t source_relic{kNoTelemetrySource};
};

struct BleedEffect
{
    float attack_snapshot{};
    Tick expires{};
    Tick next_tick{};
    SkillKind source_skill{SkillKind::BasicAttack};
    std::uint8_t source_upgrade{kNoTelemetrySource};
    std::uint8_t source_relic{kNoTelemetrySource};
};

struct BurnEffect
{
    float attack_snapshot{};
    Tick expires{};
    Tick next_tick{};
    SkillKind source_skill{SkillKind::BasicAttack};
    bool propagated{};
    std::uint8_t source_upgrade{kNoTelemetrySource};
    std::uint8_t source_relic{kNoTelemetrySource};
};

struct StatusState
{
    std::vector<SlowEffect> slows;
    std::array<BleedEffect, 5> bleeds{};
    std::uint8_t bleed_count{};
    std::optional<BurnEffect> burn;
};

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
    flecs::entity_t ecs{};
    EnemyKind kind{EnemyKind::Melee};
    std::optional<BossKind> boss;
    Float2 position{};
    Float2 previous_position{};
    Float2 velocity{};
    std::int32_t health{};
    std::int32_t max_health{};
    std::int32_t damage{};
    float move_speed{};
    float attack_range{};
    Tick warning_ticks{};
    Tick attack_cooldown_ticks{};
    Tick next_attack{};
    Tick attack_resolve{};
    std::uint64_t attack_cast_id{};
    Float2 locked_aim{};
    Tick spawned_tick{};
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
    flecs::entity_t ecs{};
    bool player_owned{};
    Float2 position{};
    Float2 previous_position{};
    Float2 velocity{};
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
    flecs::entity_t ecs{};
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
    flecs::entity_t ecs{};
    PickupKind kind{PickupKind::Experience};
    Float2 position{};
    std::uint32_t value{};
    Tick spawned_tick{};
    bool guaranteed_boss_chest{};
    bool attracted_by_magnet_stat{};
    bool globally_attracted{};
    bool dead{};
};

struct DamageEvent
{
    std::uint64_t sequence{};
    std::uint64_t target{};
    std::int32_t amount{};
    SkillKind skill{SkillKind::BasicAttack};
    EffectOrigin origin{EffectOrigin::Original};
    std::uint64_t cast_id{};
    std::uint8_t source_upgrade{kNoTelemetrySource};
    std::uint8_t source_relic{kNoTelemetrySource};
    std::uint8_t source_enemy{kNoTelemetrySource};
    std::uint8_t bleed_stacks{};
    bool burn{};
    float slow_reduction{};
    Tick slow_duration{};
    std::array<std::uint8_t, 3> amplified_upgrades{};
    std::array<std::int32_t, 3> amplified_damage{};
    std::uint8_t amplified_count{};
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

struct CastRuntime
{
    std::uint64_t cast_id{};
    SkillKind skill{SkillKind::Count};
    std::uint8_t upgrade_mask{};
    std::uint16_t normal_hits{};
    std::uint16_t kills{};
    std::uint8_t transfer_count{};
    std::uint8_t spawn_count{};
    bool triggered{};
    std::uint32_t basic_relic_triggers{};
    bool refund_triggered{};
    bool full_charge{};
    float stored_burn_attack{};
    Tick stored_burn_remaining{};
    bool has_stored_burn{};
    std::uint64_t terminal_target{};
    bool telemetry_hit{};
};

struct ActiveWave
{
    Tick start{};
    Tick duration{};
    std::uint16_t total{};
    std::uint16_t emitted{};
};

float LengthSquared(Float2 value) noexcept
{
    return value.x * value.x + value.y * value.y;
}

float Length(Float2 value) noexcept
{
    return std::sqrt(LengthSquared(value));
}

Float2 Normalize(Float2 value, Float2 fallback = {0.0f, 1.0f}) noexcept
{
    const auto length = Length(value);
    if (length <= 0.00001f)
    {
        return fallback;
    }
    return {value.x / length, value.y / length};
}

Float2 Add(Float2 left, Float2 right) noexcept
{
    return {left.x + right.x, left.y + right.y};
}

Float2 Subtract(Float2 left, Float2 right) noexcept
{
    return {left.x - right.x, left.y - right.y};
}

Float2 Multiply(Float2 value, float scalar) noexcept
{
    return {value.x * scalar, value.y * scalar};
}

Float2 Rotate(Float2 value, float degrees) noexcept
{
    const auto radians = degrees * kPi / 180.0f;
    const auto sine = std::sin(radians);
    const auto cosine = std::cos(radians);
    return {value.x * cosine + value.y * sine,
            -value.x * sine + value.y * cosine};
}

float DistanceSquared(Float2 left, Float2 right) noexcept
{
    return LengthSquared(Subtract(left, right));
}

bool HasUpgrade(std::uint8_t mask, std::uint8_t one_based_index) noexcept
{
    return (mask & (1u << (one_based_index - 1u))) != 0;
}

bool HasRelic(std::uint16_t mask, RelicKind relic) noexcept
{
    return (mask & (1u << static_cast<unsigned>(relic))) != 0;
}

std::int32_t RoundDamage(float value) noexcept
{
    return std::max(1, static_cast<std::int32_t>(std::floor(value + 0.5f)));
}

std::uint32_t ExperienceForLevel(std::uint32_t level) noexcept
{
    const auto offset = level - 1u;
    return 20u + static_cast<std::uint32_t>(
                     std::floor(3.5 * offset + 0.75 * offset * offset));
}

std::uint64_t Mix(std::uint64_t value) noexcept
{
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

void HashBytes(GameplayChecksum &hash, const void *data, std::size_t size) noexcept
{
    const auto *bytes = static_cast<const std::byte *>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= std::to_integer<std::uint8_t>(bytes[index]);
        hash *= 1099511628211ull;
    }
}

bool SegmentCircle(Float2 from, Float2 to, Float2 center, float radius) noexcept
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

} // namespace

struct GameSimulation::Impl
{
    Impl()
    {
        enemies.reserve(1'024);
        projectiles.reserve(3'072);
        areas.reserve(256);
        pickups.reserve(1'536);
        damage_events.reserve(4'096);
        scheduled.reserve(512);
        boss_actions.reserve(32);
        cast_hits.reserve(1'024);
        area_hits.reserve(1'024);
        cast_runtime.reserve(512);
        enemy_hit_casts.reserve(256);
        presentation_events.reserve(512);
        ui_commands.reserve(32);
        collision_candidates.reserve(128);
    }

    flecs::world world;
    GameData data{GameData::Defaults()};
    SimulationConfig config{};
    PlayerState player{};
    std::vector<EnemyActor> enemies;
    std::vector<ProjectileActor> projectiles;
    std::vector<AreaActor> areas;
    std::vector<PickupActor> pickups;
    std::vector<DamageEvent> damage_events;
    std::vector<ScheduledAction> scheduled;
    std::vector<BossAction> boss_actions;
    std::vector<CastHitRecord> cast_hits;
    std::vector<AreaHitRecord> area_hits;
    std::vector<CastRuntime> cast_runtime;
    std::vector<std::uint64_t> enemy_hit_casts;
    std::vector<PresentationEvent> presentation_events;
    std::vector<UiCommand> ui_commands;
    std::array<std::vector<std::size_t>, kGridCellCount> enemy_grid;
    std::vector<std::size_t> collision_candidates;
    std::array<CardView, 3> cards{};
    std::uint8_t card_count{};
    std::vector<ActiveWave> waves;
    InputFrame input{};
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
    BalanceTelemetry balance{};
    GameplayChecksum checksum{};
    SessionPhase phase{SessionPhase::Playing};
    bool initialized{};
    bool final_boss_spawned{};
    std::uint8_t menu_page{};
    std::uint8_t collection_skill_index{};
    std::uint8_t character_skill_index{};
    std::uint8_t character_slot_source{0xFF};
    std::uint8_t pending_rebind_slot{0xFF};
    std::uint8_t selection_input_guard_frames{};
    bool selection_waiting_for_release{};

    std::uint64_t Random(std::uint64_t entity, std::uint64_t purpose) const noexcept
    {
        return Mix(config.seed ^ Mix(tick) ^ Mix(entity) ^ Mix(purpose));
    }

    float RandomUnit(std::uint64_t entity, std::uint64_t purpose) const noexcept
    {
        return static_cast<float>((Random(entity, purpose) >> 40) & 0xFFFFFFu) /
               static_cast<float>(0x1000000u);
    }

    EntityId AllocateId() noexcept
    {
        return {next_entity_id++};
    }

    flecs::entity_t CreateEcsEntity(EntityId id, bool boss, bool projectile, bool area,
                                    bool pickup)
    {
        auto entity = world.entity().set<DeterministicKey>({id.value});
        if (projectile)
        {
            entity.add<ProjectileTag>();
        }
        else if (area)
        {
            entity.add<AreaTag>();
        }
        else if (pickup)
        {
            entity.add<PickupTag>();
        }
        else if (boss)
        {
            entity.add<EnemyTag>().add<BossTag>();
        }
        else
        {
            entity.add<EnemyTag>();
        }
        return entity.id();
    }

    float EffectiveAttack() const noexcept
    {
        return data.player_attack *
               (1.0f + 0.06f * player.stats[static_cast<std::size_t>(StatKind::AttackPower)]);
    }

    float EffectiveMoveSpeed() const noexcept
    {
        return data.player_move_speed *
               (1.0f + 0.03f * player.stats[static_cast<std::size_t>(StatKind::MoveSpeed)]);
    }

    float EffectiveAttackSpeed() const noexcept
    {
        return data.player_attack_speed *
               (1.0f + kAttackSpeedPerPoint *
                           player.stats[static_cast<std::size_t>(StatKind::AttackSpeed)]);
    }

    void RecordUpgradeEffect(SkillKind skill, std::uint8_t upgrade,
                             UpgradeEffectMetric metric,
                             std::uint64_t amount = 1) noexcept
    {
        if (skill >= SkillKind::Count || upgrade >= kUpgradeCount || amount == 0) return;
        balance.upgrade_effects[static_cast<std::size_t>(skill)][upgrade]
                               [static_cast<std::size_t>(metric)] += amount;
    }

    void RecordRelicEffect(RelicKind relic, UpgradeEffectMetric metric,
                           std::uint64_t amount = 1) noexcept
    {
        if (relic >= RelicKind::Count || amount == 0) return;
        balance.relic_effects[static_cast<std::size_t>(relic)]
                             [static_cast<std::size_t>(metric)] += amount;
    }

    void RecordUpgradeRelicSynergy(SkillKind skill, std::uint8_t upgrade,
                                   RelicKind relic,
                                   UpgradeRelicSynergyMetric metric,
                                   std::uint64_t amount = 1) noexcept
    {
        if (skill >= SkillKind::Count || upgrade >= kUpgradeCount ||
            relic >= RelicKind::Count || amount == 0)
            return;
        auto entries = std::span(balance.upgrade_relic_synergies.data(),
                                 balance.upgrade_relic_synergy_count);
        auto found = std::ranges::find_if(entries, [&](const auto &entry) {
            return entry.skill == skill && entry.upgrade == upgrade &&
                   entry.relic == relic;
        });
        UpgradeRelicSynergyTelemetry *entry{};
        if (found == entries.end())
        {
            if (balance.upgrade_relic_synergy_count ==
                balance.upgrade_relic_synergies.size())
                return;
            entry = &balance.upgrade_relic_synergies[
                balance.upgrade_relic_synergy_count++];
            entry->skill = skill;
            entry->upgrade = upgrade;
            entry->relic = relic;
        }
        else
        {
            entry = &*found;
        }
        entry->metrics[static_cast<std::size_t>(metric)] += amount;
    }

    void RecordUpgradeDamage(SkillKind skill, std::uint8_t upgrade,
                             std::uint64_t amount) noexcept
    {
        if (skill >= SkillKind::Count || upgrade >= kUpgradeCount || amount == 0)
            return;
        balance.upgrade_damage[static_cast<std::size_t>(skill)][upgrade] += amount;
        ++balance.upgrade_triggers[static_cast<std::size_t>(skill)][upgrade];
        RecordUpgradeEffect(skill, upgrade, UpgradeEffectMetric::DamageAmplified,
                            amount);
    }

    void RecordUpgradeDisplacement(SkillKind skill, std::uint8_t upgrade,
                                   Float2 before, Float2 after) noexcept
    {
        RecordUpgradeEffect(
            skill, upgrade, UpgradeEffectMetric::DisplacementMillimetres,
            static_cast<std::uint64_t>(std::llround(DistanceSquared(before, after) > 0.0f
                                                        ? Length(Subtract(after, before)) * 1000.0f
                                                        : 0.0f)));
    }

    float EffectiveMagnetRadius() const noexcept
    {
        return data.player_magnet_radius +
               0.4f * player.stats[static_cast<std::size_t>(StatKind::MagnetRadius)];
    }

    static std::size_t EnemyTelemetryIndex(const EnemyActor &enemy) noexcept
    {
        return enemy.boss ? 3u + static_cast<std::size_t>(*enemy.boss)
                          : static_cast<std::size_t>(enemy.kind);
    }

    Tick CooldownTicks(SkillKind skill) const noexcept
    {
        const auto index = static_cast<std::size_t>(skill);
        auto seconds = data.skills[index].cooldown_seconds;
        const auto reduction =
            0.03f * player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
        seconds *= 1.0f - reduction;
        if ((skill == SkillKind::PiercingShot || skill == SkillKind::ExplosiveArrow ||
             skill == SkillKind::RicochetArrow) &&
            HasUpgrade(player.upgrades[index], 8))
        {
            seconds *= skill == SkillKind::RicochetArrow ? 0.85f : 0.75f;
        }
        return std::max<Tick>(15, Seconds(seconds));
    }

    EnemyActor *FindEnemy(std::uint64_t id) noexcept
    {
        const auto iterator = std::ranges::find(enemies, id, [](const EnemyActor &enemy) {
            return enemy.id.value;
        });
        return iterator == enemies.end() ? nullptr : &*iterator;
    }

    const EnemyActor *FindEnemy(std::uint64_t id) const noexcept
    {
        const auto iterator = std::ranges::find(enemies, id, [](const EnemyActor &enemy) {
            return enemy.id.value;
        });
        return iterator == enemies.end() ? nullptr : &*iterator;
    }

    bool IsBoss(const EnemyActor &enemy) const noexcept
    {
        return enemy.boss.has_value();
    }

    std::uint32_t NormalEnemyCount() const noexcept
    {
        return static_cast<std::uint32_t>(std::ranges::count_if(
            enemies, [](const EnemyActor &enemy) { return !enemy.boss && !enemy.dead; }));
    }

    std::uint32_t BossCount() const noexcept
    {
        return static_cast<std::uint32_t>(std::ranges::count_if(
            enemies, [](const EnemyActor &enemy) { return enemy.boss && !enemy.dead; }));
    }

    std::uint32_t ProjectileCount(bool player_owned) const noexcept
    {
        return static_cast<std::uint32_t>(std::ranges::count_if(
            projectiles, [player_owned](const ProjectileActor &projectile) {
                return projectile.player_owned == player_owned && !projectile.dead;
            }));
    }

    void EmitPresentation(PresentationKind kind, Float2 position, std::uint64_t asset)
    {
        PresentationEvent event;
        event.sequence = ++event_sequence;
        event.tick = tick;
        event.kind = kind;
        event.position = {position.x, 0.2f, position.y};
        event.asset = {asset};
        presentation_events.push_back(event);
    }

    void EmitUiCommand(UiCommandKind kind, std::uint32_t value)
    {
        ui_commands.push_back({kind, value});
    }

    void EmitVfx(std::string_view effect, Float2 position,
                 Float2 direction = {0.0f, 1.0f}, float scale = 1.0f,
                 float height = 0.3f)
    {
        const auto length = std::hypot(direction.x, direction.y);
        if (length <= 0.0001f) direction = {0.0f, 1.0f};
        else direction = {direction.x / length, direction.y / length};
        PresentationEvent event;
        event.sequence = ++event_sequence;
        event.tick = tick;
        event.kind = PresentationKind::Vfx;
        event.position = {position.x, height, position.y};
        event.asset = MakeAssetId(effect);
        event.parameters = EncodeVfxParameters(
            {{direction.x, 0.0f, direction.y}, scale, {}, 0});
        presentation_events.push_back(event);
    }

    void EmitVfxLine(std::string_view effect, Float2 start, Float2 end,
                     float height = 0.75f)
    {
        PresentationEvent event;
        event.sequence = ++event_sequence;
        event.tick = tick;
        event.kind = PresentationKind::Vfx;
        event.position = {start.x, height, start.y};
        event.asset = MakeAssetId(effect);
        event.parameters = EncodeVfxParameters(
            {{0.0f, 0.0f, 1.0f}, 1.0f, {end.x, height, end.y},
             static_cast<std::uint32_t>(VfxEventFlag::HasTarget)});
        presentation_events.push_back(event);
    }

    bool SpawnEnemy(EnemyKind kind, Float2 position,
                    std::uint64_t random_key = 0)
    {
        const auto minute = static_cast<std::uint32_t>(growth_ticks / Seconds(60.0f));
        const auto &definition = data.enemies[static_cast<std::size_t>(kind)];
        EnemyActor enemy;
        enemy.id = AllocateId();
        enemy.random_key = random_key != 0 ? random_key : next_enemy_random_key++;
        enemy.ecs = CreateEcsEntity(enemy.id, false, false, false, false);
        enemy.kind = kind;
        enemy.position = enemy.previous_position = position;
        enemy.max_health = enemy.health = RoundDamage(
            static_cast<float>(definition.health) * (1.0f + 0.15f * minute));
        enemy.damage = RoundDamage(
            static_cast<float>(definition.damage) * (1.0f + 0.05f * minute));
        enemy.move_speed = definition.move_speed;
        enemy.attack_range = definition.attack_range;
        enemy.warning_ticks = Seconds(definition.warning_seconds);
        enemy.attack_cooldown_ticks = Seconds(definition.attack_cooldown_seconds);
        enemy.spawned_tick = tick;
        enemy.status.slows.reserve(8);
        enemies.push_back(std::move(enemy));
        ++balance.enemy_spawned[static_cast<std::size_t>(kind)];
        return true;
    }

    bool SpawnBoss(BossKind kind)
    {
        const auto &definition = data.bosses[static_cast<std::size_t>(kind)];
        EnemyActor boss;
        boss.id = AllocateId();
        boss.random_key = next_enemy_random_key++;
        boss.ecs = CreateEcsEntity(boss.id, true, false, false, false);
        boss.boss = kind;
        boss.position = boss.previous_position =
            player.position.x >= 0.0f ? Float2{-59.0f, -59.0f} : Float2{59.0f, 59.0f};
        boss.max_health = boss.health = definition.health;
        boss.damage = kind == BossKind::FiveMinute ? 18 : kind == BossKind::TenMinute ? 12 : 25;
        boss.move_speed = kind == BossKind::TenMinute ? 4.8f : 2.4f;
        boss.attack_range = 18.0f;
        boss.spawned_tick = tick;
        boss.pattern_ready = tick + 90;
        boss.status.slows.reserve(8);
        enemies.push_back(std::move(boss));
        ++balance.enemy_spawned[3u + static_cast<std::size_t>(kind)];
        EmitPresentation(PresentationKind::Ui, enemies.back().position,
                         0x626F73735F737061ull);
        EmitVfx("particle.boss.spawn", enemies.back().position);
        return true;
    }

    void SpawnPickup(PickupKind kind, Float2 position, std::uint32_t value,
                     bool guaranteed = false)
    {
        PickupActor pickup;
        pickup.id = AllocateId();
        pickup.ecs = CreateEcsEntity(pickup.id, false, false, false, true);
        pickup.kind = kind;
        pickup.position = position;
        pickup.value = value;
        pickup.spawned_tick = tick;
        pickup.guaranteed_boss_chest = guaranteed;
        pickups.push_back(pickup);
        ++balance.pickup_drops[static_cast<std::size_t>(kind)];
        if (kind == PickupKind::Experience)
            EmitVfx("particle.pickup.xp_spawn", position);
    }

    ProjectileActor *FireProjectile(SkillKind skill, Float2 position, Float2 direction,
                                    float coefficient, EffectOrigin origin,
                                    std::uint64_t cast_id,
                                    std::uint8_t applied_upgrade_mask,
                                    bool player_owned = true,
                                    std::uint8_t source_upgrade = kNoTelemetrySource,
                                    std::uint8_t source_relic = kNoTelemetrySource,
                                    std::uint8_t source_enemy = kNoTelemetrySource)
    {
        assert(skill >= SkillKind::Count ||
               (applied_upgrade_mask &
                ~player.upgrades[static_cast<std::size_t>(skill)]) == 0);
        const auto &definition = data.skills[static_cast<std::size_t>(skill)];
        const auto projectile_speed =
            definition.projectile_speed > 0.0f
                ? definition.projectile_speed
                : data.skills[static_cast<std::size_t>(SkillKind::BasicAttack)]
                      .projectile_speed;
        ProjectileActor projectile;
        projectile.id = AllocateId();
        projectile.ecs = CreateEcsEntity(projectile.id, false, true, false, false);
        projectile.player_owned = player_owned;
        projectile.position = projectile.previous_position = position;
        const auto normalized = Normalize(direction);
        projectile.velocity = Multiply(
            normalized, player_owned ? projectile_speed
                                     : data.enemies[static_cast<std::size_t>(
                                           EnemyKind::Ranged)].projectile_speed);
        projectile.remaining_range = player_owned ? definition.range : 18.0f;
        projectile.radius = player_owned ? definition.collision_radius * 2.0f : 0.25f;
        projectile.damage = player_owned ? RoundDamage(EffectiveAttack() * coefficient)
                                         : RoundDamage(coefficient);
        projectile.skill = skill;
        projectile.upgrade_mask = applied_upgrade_mask;
        projectile.origin = origin;
        projectile.cast_id = cast_id;
        projectile.source_upgrade = source_upgrade;
        projectile.source_relic = source_relic;
        projectile.source_enemy = source_enemy;
        const auto full_skill_effect = origin == EffectOrigin::Original ||
                                       applied_upgrade_mask != 0;
        projectile.pierce_remaining = full_skill_effect ? definition.pierce_count : 0;
        if (skill == SkillKind::BasicAttack && HasUpgrade(projectile.upgrade_mask, 2))
        {
            ++projectile.pierce_remaining;
        }
        if (skill == SkillKind::MultiShot && HasUpgrade(projectile.upgrade_mask, 3))
        {
            ++projectile.pierce_remaining;
        }
        if (skill == SkillKind::RicochetArrow && full_skill_effect)
        {
            projectile.bounce_remaining = 5;
        }
        if (skill == SkillKind::ExplosiveArrow && full_skill_effect)
        {
            projectile.explosion_radius = 3.0f;
            projectile.explosion_damage = projectile.damage;
        }
        projectiles.push_back(projectile);
        if (player_owned && skill < SkillKind::Count)
        {
            constexpr std::array<std::string_view, kCombatSkillCount> effects{
                "particle.basic_attack", "particle.skill.piercing_shot",
                "particle.skill.multishot", "particle.skill.charged_shot",
                "particle.skill.explosive_arrow", "particle.skill.ricochet_arrow",
                "particle.skill.arrow_rain", "particle.skill.trap",
                "particle.skill.retreat_shot"};
            const auto release_position = Add(position, Multiply(normalized, 0.65f));
            EmitVfx(effects[static_cast<std::size_t>(skill)], release_position, direction,
                    origin == EffectOrigin::Original ? 1.0f : 0.6f, 1.1f);
        }
        else if (!player_owned && source_enemy ==
                                      static_cast<std::uint8_t>(EnemyKind::Ranged))
            EmitVfx("particle.enemy.ranged.release",
                    Add(position, Multiply(normalized, 0.5f)), direction, 1.0f, 0.8f);
        RecordUpgradeEffect(skill, source_upgrade,
                            UpgradeEffectMetric::ProjectilesCreated);
        return &projectiles.back();
    }

    void SpawnArea(AreaKind kind, SkillKind skill, Float2 position, float radius,
                   float coefficient, float duration, float activation_delay,
                   EffectOrigin origin, std::uint64_t cast_id,
                   std::uint8_t applied_upgrade_mask,
                   float slow_reduction = 0.0f, float slow_duration = 0.0f,
                   float effect_radius = 0.0f, bool applies_burn = false,
                   std::uint8_t source_upgrade = kNoTelemetrySource,
                   std::uint8_t source_relic = kNoTelemetrySource,
                   std::uint8_t source_enemy = kNoTelemetrySource)
    {
        assert(skill >= SkillKind::Count ||
               (applied_upgrade_mask &
                ~player.upgrades[static_cast<std::size_t>(skill)]) == 0);
        AreaActor area;
        area.id = AllocateId();
        area.ecs = CreateEcsEntity(area.id, false, false, true, false);
        area.kind = kind;
        area.position = position;
        area.radius = radius;
        area.effect_radius = effect_radius > 0.0f ? effect_radius : radius;
        area.damage = RoundDamage(EffectiveAttack() * coefficient);
        area.active_tick = tick + Seconds(activation_delay);
        area.next_tick = area.active_tick;
        area.expires = area.active_tick + Seconds(duration);
        area.skill = skill;
        area.upgrade_mask = applied_upgrade_mask;
        area.origin = origin;
        area.cast_id = cast_id;
        area.source_upgrade = source_upgrade;
        area.source_relic = source_relic;
        area.source_enemy = source_enemy;
        area.slow_reduction = slow_reduction;
        area.slow_duration = Seconds(slow_duration);
        area.applies_burn = applies_burn;
        areas.push_back(area);
        RecordUpgradeEffect(skill, source_upgrade,
                            UpgradeEffectMetric::AreasCreated);
    }

    void Schedule(const ScheduledAction &action)
    {
        scheduled.push_back(action);
    }

    void DealDamage(std::uint64_t target, std::int32_t amount, SkillKind skill,
                    EffectOrigin origin, std::uint64_t cast_id,
                    std::uint8_t bleed = 0, bool burn = false,
                    float slow = 0.0f, Tick slow_duration = 0,
                    std::uint8_t source_upgrade = kNoTelemetrySource,
                    std::uint8_t source_relic = kNoTelemetrySource,
                    std::uint8_t source_enemy = kNoTelemetrySource)
    {
        if (target == 0 && tick < player.revive_invulnerable_until)
            return;
        damage_events.push_back({++damage_sequence, target, std::max(1, amount), skill,
                                 origin, cast_id, source_upgrade, source_relic, source_enemy,
                                 bleed, burn, slow, slow_duration});
    }

    void DamageArea(Float2 position, float radius, std::int32_t damage, SkillKind skill,
                    EffectOrigin origin, std::uint64_t cast_id,
                    std::uint8_t bleed = 0, bool burn = false,
                    float slow = 0.0f, Tick slow_duration = 0,
                    std::uint8_t source_upgrade = kNoTelemetrySource,
                    std::uint8_t source_relic = kNoTelemetrySource)
    {
        for (const auto &enemy : enemies)
        {
            if (!enemy.dead && DistanceSquared(position, enemy.position) <= radius * radius)
            {
                DealDamage(enemy.id.value, damage, skill, origin, cast_id, bleed, burn,
                           slow, slow_duration, source_upgrade, source_relic);
            }
        }
    }

    void StartSession()
    {
        const auto destroy = [this](flecs::entity_t entity) {
            if (entity != 0 && world.is_alive(entity)) world.entity(entity).destruct();
        };
        for (const auto &enemy : enemies) destroy(enemy.ecs);
        for (const auto &projectile : projectiles) destroy(projectile.ecs);
        for (const auto &area : areas) destroy(area.ecs);
        for (const auto &pickup : pickups) destroy(pickup.ecs);
        phase = SessionPhase::Playing;
        player = {};
        player.health = player.max_health = data.player_health;
        player.attack = data.player_attack;
        player.attack_speed = data.player_attack_speed;
        player.move_speed = data.player_move_speed;
        player.magnet_radius = data.player_magnet_radius;
        player.skill_levels[0] = 1;
        player.level_rerolls = 3;
        player.relic_rerolls = 3;
        growth_ticks = boss_fight_ticks = tick = 0;
        final_boss_spawned = false;
        spawn_accumulator = 0;
        kills = normal_chest_kills = heal_pickup_misses = magnet_pickup_misses = 0;
        level_reroll_sequence = 0;
        relic_reroll_sequence = 0;
        menu_page = 0;
        collection_skill_index = 0;
        character_skill_index = 0;
        character_slot_source = 0xFF;
        selection_input_guard_frames = 0;
        selection_waiting_for_release = false;
        next_enemy_attack_id = 1;
        next_enemy_random_key = 1;
        damage_dealt = damage_taken = healing = 0;
        damage_by_skill = {};
        balance = {};
        enemies.clear();
        projectiles.clear();
        areas.clear();
        pickups.clear();
        damage_events.clear();
        scheduled.clear();
        boss_actions.clear();
        cast_hits.clear();
        area_hits.clear();
        cast_runtime.clear();
        enemy_hit_casts.clear();
        presentation_events.clear();
        waves.clear();
        cards = {};
        card_count = 0;
    }

    void ProcessInput()
    {
        const bool selection_input_locked =
            selection_input_guard_frames > 0 || selection_waiting_for_release;
        if (selection_input_guard_frames > 0) --selection_input_guard_frames;
        if (!input.held.basic_attack_held) selection_waiting_for_release = false;
        const auto cursor_pixels = input.held.ui_cursor_pixels;
        const auto clicked = [&](float x, float y, float width, float height) {
            return cursor_pixels.x >= x && cursor_pixels.x <= x + width &&
                   cursor_pixels.y >= y && cursor_pixels.y <= y + height;
        };
        const auto aim_delta = Float2{input.held.aim_world.x - player.position.x,
                                      input.held.aim_world.z - player.position.y};
        if (LengthSquared(aim_delta) > 0.0001f)
        {
            player.aim = Normalize(aim_delta, player.aim);
        }
        if (input.held.move_held && phase == SessionPhase::Playing)
        {
            player.move_target = {input.held.move_target_world.x,
                                  input.held.move_target_world.z};
            player.has_move_target = true;
        }
        for (const auto &edge : input.ordered_edges)
        {
            if (edge.action == GameAction::CharacterPage &&
                edge.kind == EdgeKind::Pressed)
            {
                if (phase == SessionPhase::Playing)
                {
                    CancelChargedShot();
                    phase = SessionPhase::Paused;
                    menu_page = 3;
                    character_slot_source = 0xFF;
                }
                else if (phase == SessionPhase::Paused && menu_page >= 3 &&
                         menu_page <= 5)
                {
                    phase = SessionPhase::Playing;
                    menu_page = 0;
                    character_slot_source = 0xFF;
                }
                continue;
            }
            if (edge.action == GameAction::Pause && edge.kind == EdgeKind::Pressed)
            {
                if (phase == SessionPhase::Playing)
                {
                    CancelChargedShot();
                    phase = SessionPhase::Paused;
                    menu_page = 0;
                }
                else if (phase == SessionPhase::Paused)
                {
                    if (menu_page == 6)
                    {
                        pending_rebind_slot = 0xFF;
                        menu_page = 0;
                    }
                    else
                    {
                        phase = SessionPhase::Playing;
                        menu_page = 0;
                    }
                }
                else if (phase == SessionPhase::MainMenu && menu_page != 0)
                {
                    menu_page = 0;
                }
                continue;
            }
            if (edge.action == GameAction::BasicAttack && edge.kind == EdgeKind::Pressed)
            {
                if (phase == SessionPhase::MainMenu ||
                    (phase == SessionPhase::Paused && menu_page == 6))
                {
                    if (menu_page == 1)
                    {
                        bool selected_skill = false;
                        for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
                        {
                            if (clicked(210.0f, 150.0f + static_cast<float>(skill) * 70.0f,
                                        360.0f, 56.0f))
                            {
                                collection_skill_index = static_cast<std::uint8_t>(skill);
                                selected_skill = true;
                                break;
                            }
                        }
                        if (!selected_skill && clicked(210.0f, 900.0f, 360.0f, 56.0f))
                            menu_page = 0;
                    }
                    else if (menu_page == 2 || menu_page == 6)
                    {
                        auto &settings = config.settings;
                        if (clicked(500, 250, 420, 56))
                        {
                            settings.borderless = !settings.borderless;
                            EmitUiCommand(UiCommandKind::SetBorderless,
                                          settings.borderless);
                        }
                        else if (clicked(500, 320, 420, 56))
                        {
                            settings.vsync = !settings.vsync;
                            EmitUiCommand(UiCommandKind::SetVsync, settings.vsync);
                        }
                        else if (clicked(500, 390, 420, 56))
                        {
                            constexpr std::array caps{30u, 60u, 120u, 0u};
                            const auto current = std::ranges::find(caps, settings.frame_cap);
                            const auto index = current == caps.end()
                                ? 0u : static_cast<unsigned>(current - caps.begin() + 1) % caps.size();
                            settings.frame_cap = caps[index];
                            EmitUiCommand(UiCommandKind::SetFrameCap, settings.frame_cap);
                        }
                        else if (clicked(500, 460, 420, 56))
                        {
                            settings.render_scale_percent =
                                settings.render_scale_percent == 100 ? 75 : 100;
                            EmitUiCommand(UiCommandKind::SetRenderScale,
                                          settings.render_scale_percent);
                        }
                        else if (clicked(500, 530, 420, 56))
                        {
                            settings.shadow_resolution =
                                settings.shadow_resolution == 2048 ? 1024 : 2048;
                            EmitUiCommand(UiCommandKind::SetShadowResolution,
                                          settings.shadow_resolution);
                        }
                        else if (clicked(500, 600, 420, 56))
                        {
                            settings.particle_percentage =
                                settings.particle_percentage == 100 ? 50 : 100;
                            EmitUiCommand(UiCommandKind::SetParticlePercentage,
                                          settings.particle_percentage);
                        }
                        else if (clicked(500, 670, 420, 56))
                        {
                            settings.bloom = !settings.bloom;
                            EmitUiCommand(UiCommandKind::SetBloom, settings.bloom);
                        }
                        else if (clicked(500, 740, 420, 56))
                        {
                            settings.outline = !settings.outline;
                            EmitUiCommand(UiCommandKind::SetOutline, settings.outline);
                        }
                        else
                        {
                            const auto adjust_volume = [&](float &volume, UiCommandKind kind,
                                                           float y) {
                                if (!clicked(1'000, y, 420, 56)) return false;
                                const auto delta = cursor_pixels.x < 1'210 ? -0.1f : 0.1f;
                                volume = std::round(std::clamp(volume + delta, 0.0f, 1.0f) *
                                                    10.0f) / 10.0f;
                                EmitUiCommand(kind, static_cast<std::uint32_t>(
                                    std::lround(volume * 100.0f)));
                                return true;
                            };
                            if (adjust_volume(settings.master_volume,
                                              UiCommandKind::SetMasterVolumePercent, 250) ||
                                adjust_volume(settings.bgm_volume,
                                              UiCommandKind::SetBgmVolumePercent, 320) ||
                                adjust_volume(settings.sfx_volume,
                                              UiCommandKind::SetSfxVolumePercent, 390) ||
                                adjust_volume(settings.ui_volume,
                                              UiCommandKind::SetUiVolumePercent, 460))
                            {
                            }
                            else
                            {
                                for (std::uint32_t slot = 0; slot < 4; ++slot)
                                {
                                    if (clicked(1'000, 550.0f + slot * 70.0f, 420, 56))
                                    {
                                        pending_rebind_slot = static_cast<std::uint8_t>(slot);
                                        EmitUiCommand(UiCommandKind::BeginSkillRebind, slot);
                                        break;
                                    }
                                }
                            }
                        }
                        if (clicked(760, 870, 400, 64))
                        {
                            pending_rebind_slot = 0xFF;
                            menu_page = 0;
                        }
                    }
                    else if (clicked(760.0f, 270.0f, 400.0f, 92.0f))
                    {
                        StartSession();
                    }
                    else if (clicked(760.0f, 420.0f, 400.0f, 92.0f))
                    {
                        menu_page = 1;
                    }
                    else if (clicked(760.0f, 570.0f, 400.0f, 92.0f))
                    {
                        menu_page = 2;
                    }
                    else if (clicked(760.0f, 720.0f, 400.0f, 92.0f))
                    {
                        phase = SessionPhase::QuitRequested;
                    }
                    continue;
                }
                if (phase == SessionPhase::Victory || phase == SessionPhase::Defeat)
                {
                    phase = SessionPhase::MainMenu;
                    continue;
                }
                if (phase == SessionPhase::CardSelection ||
                    phase == SessionPhase::RelicSelection)
                {
                    if (selection_input_locked) continue;
                    if (clicked(760.0f, 790.0f, 400.0f, 72.0f))
                    {
                        auto &rerolls = phase == SessionPhase::RelicSelection
                                            ? player.relic_rerolls
                                            : player.level_rerolls;
                        auto &sequence = phase == SessionPhase::RelicSelection
                                             ? relic_reroll_sequence
                                             : level_reroll_sequence;
                        if (rerolls > 0)
                        {
                            --rerolls;
                            ++sequence;
                            if (phase == SessionPhase::RelicSelection) GenerateRelicCards(true);
                            else GenerateLevelCards(true);
                        }
                    }
                    else
                    {
                        for (std::size_t index = 0; index < 3; ++index)
                        {
                            if (clicked(360.0f + static_cast<float>(index) * 420.0f,
                                        300.0f, 360.0f, 420.0f))
                            {
                                (void)SelectCard(index);
                                break;
                            }
                        }
                    }
                    continue;
                }
                if (phase == SessionPhase::StatAllocation)
                {
                    if (selection_input_locked) continue;
                    for (std::size_t index = 0; index < kStatCount; ++index)
                    {
                        if (clicked(390.0f + static_cast<float>(index % 3) * 400.0f,
                                    310.0f + static_cast<float>(index / 3) * 260.0f,
                                    340.0f, 180.0f))
                        {
                            AssignStat(static_cast<StatKind>(index));
                            break;
                        }
                    }
                    continue;
                }
                if (phase == SessionPhase::Paused && menu_page >= 3 &&
                    menu_page <= 5)
                {
                    if (clicked(350, 140, 280, 58))
                    {
                        menu_page = 3;
                        character_slot_source = 0xFF;
                    }
                    else if (clicked(650, 140, 280, 58))
                    {
                        menu_page = 4;
                        character_slot_source = 0xFF;
                    }
                    else if (clicked(950, 140, 280, 58))
                    {
                        menu_page = 5;
                        character_slot_source = 0xFF;
                    }
                    else if (clicked(1'520, 140, 120, 58))
                    {
                        phase = SessionPhase::Playing;
                        menu_page = 0;
                        character_slot_source = 0xFF;
                    }
                    else if (menu_page == 4)
                    {
                        bool slot_clicked{};
                        for (std::size_t slot = 0; slot < player.loadout.size(); ++slot)
                        {
                            if (!clicked(780.0f + static_cast<float>(slot) * 195.0f,
                                         225.0f, 180.0f, 54.0f))
                                continue;
                            slot_clicked = true;
                            if (character_slot_source == 0xFF)
                            {
                                if (player.loadout[slot] != SkillKind::Count)
                                    character_slot_source = static_cast<std::uint8_t>(slot);
                            }
                            else if (character_slot_source == slot)
                            {
                                character_slot_source = 0xFF;
                            }
                            else
                            {
                                std::swap(player.loadout[character_slot_source],
                                          player.loadout[slot]);
                                character_slot_source = 0xFF;
                            }
                            break;
                        }
                        if (slot_clicked) continue;
                        for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
                        {
                            if (player.skill_levels[skill] > 0 &&
                                clicked(350, 240.0f + static_cast<float>(skill) * 78.0f,
                                        360, 64))
                            {
                                character_skill_index = static_cast<std::uint8_t>(skill);
                                break;
                            }
                        }
                    }
                    continue;
                }
                if (phase == SessionPhase::Paused && menu_page == 0)
                {
                    if (clicked(760, 420, 400, 72))
                    {
                        phase = SessionPhase::Playing;
                    }
                    else if (clicked(760, 520, 400, 72))
                    {
                        menu_page = 6;
                    }
                    else if (clicked(760, 620, 400, 72))
                    {
                        phase = SessionPhase::QuitRequested;
                    }
                    continue;
                }
            }
            if (phase != SessionPhase::Playing)
            {
                continue;
            }
            if (edge.action == GameAction::BasicAttack && edge.kind == EdgeKind::Pressed)
            {
                player.has_move_target = false;
                continue;
            }
            const auto skill_slot = [](GameAction action) -> std::optional<std::size_t> {
                switch (action)
                {
                case GameAction::SkillQ: return 0;
                case GameAction::SkillW: return 1;
                case GameAction::SkillE: return 2;
                case GameAction::SkillR: return 3;
                default: return std::nullopt;
                }
            }(edge.action);
            if (!skill_slot)
            {
                continue;
            }
            const auto skill = player.loadout[*skill_slot];
            if (edge.kind == EdgeKind::Released)
            {
                if (player.buffered_skill == skill)
                {
                    player.buffered_skill = SkillKind::Count;
                    player.buffered_skill_expires = 0;
                }
                if (player.charging && player.charging_slot == *skill_slot)
                {
                    ReleaseChargedShot();
                }
                continue;
            }
            if (skill == SkillKind::Count)
            {
                continue;
            }

            if (TryBeginSkill(skill))
            {
                if (skill == SkillKind::ChargedShot)
                    player.charging_slot = static_cast<std::uint8_t>(*skill_slot);
            }
            else
            {
                player.buffered_skill = skill;
                player.buffered_skill_expires = tick + kInputBufferTicks;
            }
        }

        ConsumeBufferedSkill();
    }

    void CastBasicAttack()
    {
        if (!input.held.basic_attack_held || player.charging ||
            player.active_cast_tick > tick || tick < player.next_basic_attack)
        {
            return;
        }
        player.has_move_target = false;
        const auto attack_interval = std::max<Tick>(1, static_cast<Tick>(
            std::floor(60.0f / EffectiveAttackSpeed() + 0.5f)));
        const auto animation_ticks = std::max<Tick>(
            1, attack_interval > kAnimationBlendOutTicks
                   ? attack_interval - kAnimationBlendOutTicks
                   : 1);
        const auto release_ticks = AnimationMarkerTicks(kBasicArrowReleaseTicks,
                                                        animation_ticks);
        player.next_basic_attack = tick + attack_interval;
        ++balance.skill_uses[static_cast<std::size_t>(SkillKind::BasicAttack)];
        const auto base_interval = std::max<Tick>(
            1, static_cast<Tick>(std::floor(60.0f / data.player_attack_speed + 0.5f)));
        balance.stat_utility[static_cast<std::size_t>(StatKind::AttackSpeed)] +=
            base_interval - std::min(base_interval, attack_interval);
        player.basic_attack_animation_start = tick;
        player.basic_attack_animation_until = player.next_basic_attack;
        ++player.basic_sequence;
        player.facing = player.aim;
        const auto cast_id = next_cast_id++;
        const auto mask = player.upgrades[0];
        RuntimeForCast(cast_id, SkillKind::BasicAttack).upgrade_mask = mask;
        const auto fire = [&](Float2 direction, float coefficient,
                              std::uint8_t source_upgrade = kNoTelemetrySource) {
            Schedule({tick + release_ticks, ScheduledKind::Projectile,
                      SkillKind::BasicAttack, player.position, direction, coefficient,
                      0.0f, 0.0f, 1, mask, EffectOrigin::Original, cast_id,
                      source_upgrade});
        };

        if (HasUpgrade(mask, 8) && tick <= player.active_basic_empower_until)
        {
            fire(player.aim, 0.7f);
            fire(Rotate(player.aim, -15.0f), 0.7f, 7);
            fire(Rotate(player.aim, 15.0f), 0.7f, 7);
            player.active_basic_empower_until = 0;
        }
        else
        {
            fire(player.aim, 1.0f);
        }
        if (HasUpgrade(mask, 1) && player.basic_sequence % 3 == 0)
        {
            Schedule({tick + release_ticks + 5, ScheduledKind::Projectile,
                      SkillKind::BasicAttack,
                      player.position, player.aim, 0.7f, 0.0f, 0.0f, 1,
                      static_cast<std::uint8_t>(mask & ~std::uint8_t{1}),
                      EffectOrigin::Original, cast_id, 0});
        }
        const auto &radial = data.relics.radial_basic_attack;
        if (HasRelic(player.relic_mask, RelicKind::RadialBasicAttack) &&
            player.basic_sequence % radial.cadence_interval == 0)
        {
            EmitVfx("particle.relic.radial_arrows", player.position,
                    player.aim, 1.0f, 0.15f);
            RecordRelicEffect(RelicKind::RadialBasicAttack,
                              UpgradeEffectMetric::Activations);
            RecordRelicEffect(RelicKind::RadialBasicAttack,
                              UpgradeEffectMetric::ProjectilesCreated,
                              radial.direction_count);
            for (std::uint32_t index = 0; index < radial.direction_count; ++index)
            {
                Schedule({tick + release_ticks, ScheduledKind::Projectile,
                          SkillKind::BasicAttack, player.position,
                          Rotate({0.0f, 1.0f}, index * 360.0f /
                                                      radial.direction_count),
                          radial.damage_multiplier, 0.0f, 0.0f,
                          1, 0, EffectOrigin::Derived, cast_id,
                          kNoTelemetrySource,
                          static_cast<std::uint8_t>(RelicKind::RadialBasicAttack)});
            }
        }
        const auto &echo = data.relics.movement_echo;
        if (HasRelic(player.relic_mask, RelicKind::MovementEcho) &&
            player.movement_since_echo >= echo.required_distance)
        {
            RecordRelicEffect(RelicKind::MovementEcho,
                              UpgradeEffectMetric::Activations);
            RecordRelicEffect(RelicKind::MovementEcho,
                              UpgradeEffectMetric::ProjectilesCreated);
            Schedule({tick + release_ticks, ScheduledKind::Projectile,
                      SkillKind::BasicAttack, player.one_second_ago, player.aim,
                      echo.damage_multiplier,
                      0.0f, 0.0f, 1, 0, EffectOrigin::Derived, cast_id,
                      kNoTelemetrySource,
                      static_cast<std::uint8_t>(RelicKind::MovementEcho)});
            player.movement_since_echo = 0.0f;
        }
    }

    bool CastSkill(SkillKind skill)
    {
        const auto skill_index = static_cast<std::size_t>(skill);
        const auto cooldown_index = skill_index - 1;
        if (skill_index == 0 || skill_index >= kCombatSkillCount ||
            player.cooldowns[cooldown_index] != 0 || player.charging ||
            player.active_cast_tick > tick || tick < player.retreat_until)
        {
            return false;
        }
        player.cooldowns[cooldown_index] = CooldownTicks(skill) + 1;
        if (HasUpgrade(player.upgrades[skill_index], 8) &&
            (skill == SkillKind::PiercingShot || skill == SkillKind::ExplosiveArrow ||
             skill == SkillKind::RicochetArrow))
        {
            const auto stat_reduction =
                1.0f - 0.03f * player.stats[static_cast<std::size_t>(
                                     StatKind::CooldownReduction)];
            const auto without_upgrade = std::max<Tick>(
                15, Seconds(data.skills[skill_index].cooldown_seconds * stat_reduction));
            RecordUpgradeEffect(
                skill, 7, UpgradeEffectMetric::CooldownTicksSaved,
                without_upgrade - std::min(without_upgrade,
                                             player.cooldowns[cooldown_index] - 1));
        }
        ++balance.skill_uses[skill_index];
        const auto cooldown_reduction =
            0.03f * player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
        if (cooldown_reduction > 0.0f)
        {
            const auto without_stat = static_cast<Tick>(std::llround(
                static_cast<double>(player.cooldowns[cooldown_index] - 1) /
                (1.0 - cooldown_reduction)));
            balance.stat_utility[static_cast<std::size_t>(StatKind::CooldownReduction)] +=
                without_stat - std::min(without_stat, player.cooldowns[cooldown_index] - 1);
        }
        player.active_animation_start = tick;
        player.basic_attack_animation_until = tick;
        constexpr std::array<Tick, kCombatSkillCount> recovery_ticks{
            0, 15, 18, 9, 18, 15, 15, 15, 13};
        player.active_cast_tick = tick + recovery_ticks[skill_index];
        player.active_animation_until = player.active_cast_tick + kAnimationBlendOutTicks;
        const auto cast_id = next_cast_id++;
        const auto mask = player.upgrades[skill_index];
        RuntimeForCast(cast_id, skill).upgrade_mask = mask;
        const auto skill_release_ticks = AnimationMarkerTicks(
            kBasicArrowReleaseTicks, recovery_ticks[skill_index]);
        player.facing = player.aim;
        const auto target_delta = Float2{input.held.aim_world.x - player.position.x,
                                         input.held.aim_world.z - player.position.y};
        const auto target_distance = Length(target_delta);
        const auto target = Add(player.position,
                                Multiply(Normalize(target_delta, player.aim),
                                         std::min(target_distance, data.skills[skill_index].range)));

        switch (skill)
        {
        case SkillKind::PiercingShot:
            Schedule({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                      player.position, player.aim,
                      data.skills[skill_index].damage_coefficient,
                      0.0f, 0.0f, 1, mask,
                      EffectOrigin::Original, cast_id});
            if (HasUpgrade(mask, 1))
            {
                Schedule({tick + skill_release_ticks + 5, ScheduledKind::Projectile,
                          skill, player.position,
                          player.aim, 5.0f, 0.0f, 0.0f, 1, 0,
                          EffectOrigin::Derived, cast_id, 0});
            }
            if (HasUpgrade(mask, 4))
            {
                const auto half_length = data.skills[skill_index].range * 0.5f;
                SpawnArea(AreaKind::Damage, skill,
                          Add(player.position, Multiply(player.aim, half_length)), 0.5f,
                          0.4f, 2.0f, 0.0f, EffectOrigin::Derived, cast_id, 0,
                          0.2f, 1.0f);
                areas.back().direction = player.aim;
                areas.back().half_length = half_length;
                areas.back().source_upgrade = 3;
                EmitVfx("particle.skill.piercing_shot.trail_pulse",
                        areas.back().position, player.aim, 1.0f, 0.12f);
            }
            break;
        case SkillKind::MultiShot:
        {
            const auto miss_retarget_mask = static_cast<std::uint8_t>(
                mask & (std::uint8_t{1} << 7));
            Schedule({tick + skill_release_ticks, ScheduledKind::Volley, skill,
                      player.position, player.aim,
                      data.skills[skill_index].damage_coefficient,
                      0.0f, 0.0f, data.skills[skill_index].projectile_count, mask,
                      EffectOrigin::Original, cast_id});
            if (HasUpgrade(mask, 1))
            {
                Schedule({tick + 15, ScheduledKind::Volley, skill, player.position,
                          player.aim, 1.5f, 0.0f, 0.0f, 5, miss_retarget_mask,
                          EffectOrigin::Derived, cast_id, 0});
            }
            if (HasUpgrade(mask, 4))
            {
                for (const auto angle : {-15.0f, 0.0f, 15.0f})
                {
                    Schedule({tick + skill_release_ticks, ScheduledKind::Projectile,
                              skill, player.position,
                              Rotate(Multiply(player.aim, -1.0f), angle), 1.0f,
                              0.0f, 0.0f, 1, miss_retarget_mask,
                              EffectOrigin::Derived, cast_id, 3});
                }
            }
            if (HasUpgrade(mask, 7))
            {
                for (const auto angle : {-31.25f, 31.25f})
                    Schedule({tick + skill_release_ticks, ScheduledKind::Projectile,
                              skill, player.position, Rotate(player.aim, angle), 1.2f,
                              0.0f, 0.0f, 1, mask, EffectOrigin::Original, cast_id, 6});
            }
            break;
        }
        case SkillKind::ExplosiveArrow:
            Schedule({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                      player.position, player.aim,
                      data.skills[skill_index].damage_coefficient,
                      0.0f, 0.0f, 1, mask,
                      EffectOrigin::Original, cast_id});
            break;
        case SkillKind::RicochetArrow:
            Schedule({tick + skill_release_ticks, ScheduledKind::Projectile, skill,
                      player.position, player.aim,
                      data.skills[skill_index].damage_coefficient,
                      0.0f, 0.0f, 1, mask,
                      EffectOrigin::Original, cast_id});
            break;
        case SkillKind::ArrowRain:
            SpawnArea(AreaKind::Damage, skill, target, 4.0f,
                      data.skills[skill_index].damage_coefficient,
                      HasUpgrade(mask, 8) ? 5.0f : 3.0f, 0.4f,
                      EffectOrigin::Original, cast_id, mask,
                      HasUpgrade(mask, 6) ? 0.3f : 0.0f, 1.0f);
            EmitVfx("particle.skill.arrow_rain", target, player.aim, 1.0f, 0.1f);
            if (HasUpgrade(mask, 1))
            {
                Schedule({tick + 60, ScheduledKind::Area, skill,
                          Add(target, Multiply(player.aim, 4.0f)), player.aim,
                          0.5f, 3.0f, 2.0f, 1, 0,
                          EffectOrigin::Derived, cast_id, 0});
            }
            break;
        case SkillKind::Trap:
        {
            const auto origin = player.position;
            if (HasUpgrade(mask, 1))
            {
                for (std::uint32_t index = 0; index < 3; ++index)
                {
                    SpawnArea(AreaKind::Trap, skill,
                              Add(origin, Multiply(player.aim, 1.5f * index)),
                              2.0f, 0.9f, 12.0f, 0.6f,
                              EffectOrigin::Original, cast_id, mask,
                              0.3f, 3.0f, 3.0f,
                              false, 0);
                }
            }
            else
            {
                SpawnArea(AreaKind::Trap, skill, origin, 2.0f,
                          data.skills[skill_index].damage_coefficient,
                          12.0f, 0.6f,
                          EffectOrigin::Original, cast_id, mask, 0.3f, 3.0f, 3.0f);
            }
            player.retreat_until = tick + 15;
            player.retreat_velocity = Multiply(player.aim, 20.0f);
            player.retreat_followup_cast = cast_id;
            player.retreat_upgrade_mask = mask;
            player.retreat_landing_pending = true;
            player.forced_move_skill = SkillKind::Trap;
            EmitVfx("particle.skill.trap", origin);
            break;
        }
        case SkillKind::RetreatShot:
        {
            EmitVfx("particle.skill.retreat_shot.move", player.position,
                    Multiply(player.aim, -1.0f), 1.0f, 0.1f);
            const auto retreat_release_ticks = AnimationMarkerTicks(
                4, recovery_ticks[skill_index]);
            Schedule({tick + retreat_release_ticks, ScheduledKind::Projectile, skill,
                       player.position, player.aim,
                       HasUpgrade(mask, 1)
                           ? 0.9f
                           : data.skills[skill_index].damage_coefficient,
                       0.0f, 0.0f, 1, mask,
                       EffectOrigin::Original, cast_id,
                       static_cast<std::uint8_t>(HasUpgrade(mask, 1)
                                                     ? 0
                                                     : kNoTelemetrySource)});
            if (HasUpgrade(mask, 1))
            {
                for (const auto angle : {-12.0f, 12.0f})
                    Schedule({tick + retreat_release_ticks,
                              ScheduledKind::Projectile, skill, player.position,
                              Rotate(player.aim, angle), 0.9f, 0.0f, 0.0f, 1,
                              mask, EffectOrigin::Original, cast_id, 0});
            }
            player.retreat_until = tick + 13;
            player.retreat_velocity = Multiply(player.aim, -25.0f);
            player.retreat_followup_cast = cast_id;
            player.retreat_upgrade_mask = mask;
            player.retreat_landing_pending = true;
            player.forced_move_skill = SkillKind::RetreatShot;
            if (HasUpgrade(mask, 2))
            {
                SpawnArea(AreaKind::Trap, skill, player.position, 2.5f, 3.0f, 12.0f,
                          0.6f, EffectOrigin::Derived, cast_id, 0, 0.0f, 0.0f, 0.0f,
                          false, 1);
            }
            if (HasUpgrade(mask, 3))
            {
                SpawnArea(AreaKind::Slow, skill,
                          Add(player.position, Multiply(player.aim, -2.5f)), 2.5f,
                          0.0f, 2.0f, 0.0f, EffectOrigin::Derived, cast_id, 0, 0.35f,
                          1.0f, 0.0f, false, 2);
            }
            if (HasUpgrade(mask, 6))
            {
                player.next_active_refund_until = tick + 180;
                player.next_active_refund_source = skill;
            }
            if (HasUpgrade(mask, 8))
            {
                player.retreat_followup_tick = tick + 15;
                player.retreat_followup_direction = player.aim;
            }
            break;
        }
        default: break;
        }

        player.active_basic_empower_until = tick + 180;
        if (player.next_active_refund_until >= tick &&
            player.next_active_refund_source != SkillKind::Count &&
            player.next_active_refund_source != skill)
        {
            const auto before = player.cooldowns[cooldown_index];
            player.cooldowns[cooldown_index] -=
                player.cooldowns[cooldown_index] * 30 / 100;
            RecordUpgradeEffect(player.next_active_refund_source, 5,
                                UpgradeEffectMetric::CooldownTicksSaved,
                                before - player.cooldowns[cooldown_index]);
            player.next_active_refund_until = 0;
            player.next_active_refund_source = SkillKind::Count;
        }
        if (HasRelic(player.relic_mask, RelicKind::AlternatingSkills) &&
            player.last_active != SkillKind::Count && player.last_active != skill &&
            tick - player.last_active_tick <=
                Seconds(data.relics.alternating_skills.window_seconds))
        {
            const auto before = player.cooldowns[cooldown_index];
            player.cooldowns[cooldown_index] -= static_cast<Tick>(
                player.cooldowns[cooldown_index] *
                data.relics.alternating_skills.cooldown_refund_fraction);
            RecordRelicEffect(RelicKind::AlternatingSkills,
                              UpgradeEffectMetric::Activations);
            RecordRelicEffect(RelicKind::AlternatingSkills,
                              UpgradeEffectMetric::CooldownTicksSaved,
                              before - player.cooldowns[cooldown_index]);
        }
        player.last_active = skill;
        player.last_active_tick = tick;
        EmitPresentation(PresentationKind::Audio, player.position, 0x736B696C6C5F7573ull);
        return true;
    }

    bool TryBeginSkill(SkillKind skill)
    {
        if (skill == SkillKind::ChargedShot)
        {
            const auto cooldown = static_cast<std::size_t>(skill) - 1;
            if (player.cooldowns[cooldown] != 0 || player.charging ||
                player.active_cast_tick > tick || tick < player.retreat_until)
            {
                return false;
            }
            player.charging = true;
            player.charging_skill = skill;
            player.charge_start = tick;
            return true;
        }
        return CastSkill(skill);
    }

    void ConsumeBufferedSkill()
    {
        if (phase != SessionPhase::Playing ||
            player.buffered_skill == SkillKind::Count)
        {
            return;
        }
        if (tick > player.buffered_skill_expires)
        {
            player.buffered_skill = SkillKind::Count;
            player.buffered_skill_expires = 0;
            return;
        }
        if (TryBeginSkill(player.buffered_skill))
        {
            player.buffered_skill = SkillKind::Count;
            player.buffered_skill_expires = 0;
        }
    }

    void ReleaseChargedShot()
    {
        const auto mask = player.upgrades[static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto maximum_ticks = HasUpgrade(mask, 1) ? Seconds(1.4f) : Seconds(1.0f);
        auto elapsed = std::min(tick - player.charge_start, maximum_ticks);
        if (HasUpgrade(mask, 2))
        {
            const auto unaccelerated = elapsed;
            elapsed = std::min(maximum_ticks, static_cast<Tick>(elapsed / 0.65f));
            RecordUpgradeEffect(SkillKind::ChargedShot, 1,
                                UpgradeEffectMetric::ChargeTicksSaved,
                                elapsed - unaccelerated);
        }
        const auto ratio = static_cast<float>(elapsed) / static_cast<float>(maximum_ticks);
        const auto minimum = HasUpgrade(mask, 3) ? 11.5f : 5.0f;
        const auto maximum = HasUpgrade(mask, 1)
                                 ? 14.0f
                                 : data.skills[static_cast<std::size_t>(
                                       SkillKind::ChargedShot)].damage_coefficient;
        const auto coefficient = std::lerp(minimum, maximum, ratio);
        const auto cast_id = next_cast_id++;
        player.facing = player.aim;
        auto &runtime = RuntimeForCast(cast_id, SkillKind::ChargedShot);
        runtime.upgrade_mask = mask;
        runtime.full_charge = ratio >= 0.999f;
        if (auto *projectile = FireProjectile(SkillKind::ChargedShot, player.position,
                                              player.aim, coefficient,
                                              EffectOrigin::Original, cast_id, mask))
        {
            projectile->remaining_range = std::lerp(
                4.2f,
                data.skills[static_cast<std::size_t>(SkillKind::ChargedShot)].range,
                ratio);
            projectile->radius = std::lerp(0.5f, 1.1f, ratio);
            if (HasUpgrade(mask, 4))
            {
                projectile->pierce_remaining += 4;
            }
            if (HasUpgrade(mask, 5))
            {
                projectile->bleed_stacks = 3;
            }
            if (HasUpgrade(mask, 1) && ratio >= 0.999f)
            {
                projectile->explosion_radius = 2.0f;
                projectile->explosion_damage = RoundDamage(EffectiveAttack() * 5.0f);
                projectile->explosion_source_upgrade = 0;
            }
            projectile->full_charge = ratio >= 0.999f;
            projectile->charge_ratio = ratio;
        }
        player.cooldowns[static_cast<std::size_t>(SkillKind::ChargedShot) - 1] =
            CooldownTicks(SkillKind::ChargedShot) + 1;
        ++balance.skill_uses[static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto cooldown_reduction =
            0.03f * player.stats[static_cast<std::size_t>(StatKind::CooldownReduction)];
        if (cooldown_reduction > 0.0f)
        {
            const auto cooldown =
                player.cooldowns[static_cast<std::size_t>(SkillKind::ChargedShot) - 1] - 1;
            const auto without_stat = static_cast<Tick>(std::llround(
                static_cast<double>(cooldown) / (1.0 - cooldown_reduction)));
            balance.stat_utility[static_cast<std::size_t>(StatKind::CooldownReduction)] +=
                without_stat - std::min(without_stat, cooldown);
        }
        player.active_animation_start = tick;
        player.basic_attack_animation_until = tick;
        player.active_cast_tick = tick + 9;
        player.active_animation_until = player.active_cast_tick + kAnimationBlendOutTicks;
        CancelChargedShot();
        player.active_basic_empower_until = tick + 180;
    }

    EnemyActor *NearestEnemy(Float2 position, float radius,
                             std::span<const std::uint64_t> excluded = {}) noexcept
    {
        EnemyActor *best{};
        auto best_distance = radius * radius;
        for (auto &enemy : enemies)
        {
            if (enemy.dead || std::ranges::find(excluded, enemy.id.value) != excluded.end())
            {
                continue;
            }
            const auto distance = DistanceSquared(position, enemy.position);
            if (distance < best_distance ||
                (distance == best_distance && best && enemy.id.value < best->id.value))
            {
                best = &enemy;
                best_distance = distance;
            }
        }
        return best;
    }

    const EnemyActor *NearestEnemy(Float2 position, float radius) const noexcept
    {
        const EnemyActor *best{};
        auto best_distance = radius * radius;
        for (const auto &enemy : enemies)
        {
            if (enemy.dead)
            {
                continue;
            }
            const auto distance = DistanceSquared(position, enemy.position);
            if (distance < best_distance ||
                (distance == best_distance && best && enemy.id.value < best->id.value))
            {
                best = &enemy;
                best_distance = distance;
            }
        }
        return best;
    }

    void SessionTimerPhase()
    {
        if (phase != SessionPhase::Playing)
        {
            return;
        }
        if (!final_boss_spawned)
        {
            ++growth_ticks;
        }
        else
        {
            ++boss_fight_ticks;
        }
        for (auto &cooldown : player.cooldowns)
        {
            cooldown -= cooldown != 0;
        }
        if (player.charging && (tick + kPlayerRenderId) % 6 == 0)
            EmitVfx("particle.skill.charged_shot.pulse",
                    Add(player.position, Multiply(player.aim, 0.65f)),
                    player.aim, 1.0f, 1.1f);
        if (player.charging)
        {
            const auto mask = player.upgrades[
                static_cast<std::size_t>(SkillKind::ChargedShot)];
            const auto maximum_ticks = HasUpgrade(mask, 1)
                                           ? Seconds(1.4f)
                                           : Seconds(1.0f);
            const auto ready_ticks = HasUpgrade(mask, 2)
                                         ? static_cast<Tick>(std::ceil(
                                               maximum_ticks * 0.65f))
                                         : maximum_ticks;
            if (tick - player.charge_start == ready_ticks)
                EmitVfx("particle.skill.charged_shot.ready",
                        Add(player.position, Multiply(player.aim, 0.65f)),
                        player.aim, 1.0f, 1.1f);
        }
    }

    const SpawnStage &CurrentSpawnStage() const noexcept
    {
        const auto minute = growth_ticks / Seconds(60.0f);
        const SpawnStage *selected = &data.spawn_stages.front();
        for (const auto &stage : data.spawn_stages)
        {
            if (stage.start_minute <= minute)
            {
                selected = &stage;
            }
        }
        return *selected;
    }

    Float2 SpawnPosition(std::uint64_t salt) const noexcept
    {
        const auto angle = RandomUnit(salt, 0x535041574Eull) * 2.0f * kPi;
        const auto distance = 20.0f + RandomUnit(salt, 0x44495354ull) * 10.0f;
        auto position = Add(
            player.position,
            {std::cos(angle) * distance, std::sin(angle) * distance});
        const auto extent = data.arena_half_extent - 1.0f;
        position.x = std::clamp(position.x, -extent, extent);
        position.y = std::clamp(position.y, -extent, extent);
        return position;
    }

    EnemyKind ChooseEnemyKind(std::uint64_t salt) const noexcept
    {
        const auto &stage = CurrentSpawnStage();
        const auto roll = Random(salt, 0x4B494E44ull) % 100;
        if (roll < stage.weights[0]) return EnemyKind::Melee;
        if (roll < stage.weights[0] + stage.weights[1]) return EnemyKind::Ranged;
        return EnemyKind::Suicide;
    }

    void SpawnPhase()
    {
        if (phase != SessionPhase::Playing)
        {
            return;
        }
        if (!final_boss_spawned)
        {
            const auto &stage = CurrentSpawnStage();
            spawn_accumulator += stage.per_second;
            while (spawn_accumulator >= 60.0f)
            {
                spawn_accumulator -= 60.0f;
                const auto salt = next_enemy_random_key++;
                SpawnEnemy(ChooseEnemyKind(salt), SpawnPosition(salt), salt);
            }
        }
        for (const auto &wave : data.waves)
        {
            if (growth_ticks == static_cast<Tick>(wave.minute) * Seconds(60.0f))
            {
                waves.push_back({growth_ticks, wave.duration_ticks, wave.count, 0});
            }
        }
        for (auto &wave : waves)
        {
            const auto elapsed = growth_ticks - wave.start;
            const auto desired = static_cast<std::uint16_t>(
                std::min<std::uint64_t>(wave.total,
                    (static_cast<std::uint64_t>(elapsed + 1) * wave.total +
                     wave.duration - 1) / wave.duration));
            while (wave.emitted < desired && !final_boss_spawned)
            {
                const auto salt = next_enemy_random_key++;
                SpawnEnemy(ChooseEnemyKind(salt), SpawnPosition(salt), salt);
                ++wave.emitted;
            }
        }
        if (growth_ticks == Seconds(300.0f))
        {
            SpawnBoss(BossKind::FiveMinute);
        }
        if (growth_ticks == Seconds(600.0f))
        {
            SpawnBoss(BossKind::TenMinute);
        }
        if (!final_boss_spawned && growth_ticks == Seconds(900.0f))
        {
            final_boss_spawned = true;
            spawn_accumulator = 0;
            waves.clear();
            SpawnBoss(BossKind::Final);
        }
    }

    float SlowMultiplier(EnemyActor &enemy)
    {
        std::erase_if(enemy.status.slows,
                      [this](const SlowEffect &slow) { return slow.expires <= tick; });
        float multiplier = 1.0f;
        for (const auto &slow : enemy.status.slows)
        {
            multiplier *= 1.0f - slow.reduction;
        }
        return multiplier;
    }

    void BossAi(EnemyActor &boss)
    {
        if (tick < boss.invulnerable_until)
        {
            boss.velocity = {};
            return;
        }
        if (tick < boss.dash_until) return;
        if (boss.dash_until != 0)
        {
            EmitVfx("particle.boss.dash.impact", boss.position,
                    Normalize(boss.velocity), 1.0f, 0.15f);
            boss.velocity = {};
            boss.dash_until = 0;
        }
        const auto kind = *boss.boss;
        const auto to_player = Subtract(player.position, boss.position);
        const auto distance = Length(to_player);
        const auto direction = Normalize(to_player);
        if (tick < boss.pattern_ready)
        {
            boss.velocity = {};
            return;
        }
        if (kind == BossKind::TenMinute && distance > 18.0f)
        {
            boss.velocity = Multiply(direction, boss.move_speed * SlowMultiplier(boss));
            return;
        }

        const auto preferred = kind == BossKind::TenMinute
                                   ? static_cast<std::uint8_t>(distance <= 12.0f ? 1 : 0)
                                   : kind == BossKind::Final
                                         ? static_cast<std::uint8_t>(distance <= 7.0f ? 2 :
                                                                        distance <= 14.0f ? 1 : 0)
                                         : static_cast<std::uint8_t>(distance <= 7.0f ? 1 : 0);
        const auto pattern_count = kind == BossKind::Final ? 3u : 2u;
        auto pattern = Random(boss.random_key, 0x424F5353504154ull) % 4 < 3
                           ? preferred
                           : static_cast<std::uint8_t>((preferred + 1) % pattern_count);
        if (pattern == boss.last_pattern && boss.repeat_count >= 2)
            pattern = static_cast<std::uint8_t>((pattern + 1) % pattern_count);
        boss.repeat_count = pattern == boss.last_pattern ? boss.repeat_count + 1 : 1;
        boss.last_pattern = pattern;
        boss.velocity = {};

        const auto player_velocity = Multiply(
            Subtract(player.position, player.previous_position), 60.0f);
        const auto cast_id = next_cast_id++;
        ++balance.enemy_attack_attempts[EnemyTelemetryIndex(boss)];
        const auto add = [&](BossAction action) { boss_actions.push_back(action); };
        Tick last_due{};

        if (kind == BossKind::FiveMinute)
        {
            if (pattern == 0)
            {
                last_due = tick + Seconds(0.9f);
                add({last_due, BossActionKind::Dash, boss.id.value, {}, direction, 12.0f,
                     18.0f, 18, 0, 0, 0, 0, 0, cast_id});
            }
            else
            {
                last_due = tick + Seconds(1.35f);
                add({last_due, BossActionKind::Shockwave, boss.id.value, boss.position, {},
                     0, 3.0f, 15, 0, 0, 0, 9.75f, 0.75f, cast_id});
            }
        }
        else if (kind == BossKind::TenMinute)
        {
            last_due = tick + Seconds(pattern == 0 ? 0.6f : 1.0f);
            if (pattern == 0)
                add({last_due, BossActionKind::Volley, boss.id.value, {}, direction, 4.5f,
                     0, 12, 7, 70.0f, 0, 0, 0, cast_id});
            else
            {
                constexpr float kAreaRadius = 2.2f;
                const auto center = Add(player.position, Multiply(player_velocity, 0.75f));
                const auto safe_extent = data.arena_half_extent - kAreaRadius * 2.0f;
                const Float2 clamped_center{
                    std::clamp(center.x, -safe_extent, safe_extent),
                    std::clamp(center.y, -safe_extent, safe_extent)};
                const auto angle_offset = RandomUnit(
                    boss.random_key, cast_id ^ 0x47524F554E44ull) * 360.0f;
                for (std::uint32_t index = 0; index < 3; ++index)
                {
                    const auto position = Add(
                        clamped_center,
                        Multiply(Rotate({0.0f, 1.0f}, angle_offset + 120.0f * index),
                                 kAreaRadius));
                    add({last_due, BossActionKind::Area, boss.id.value, position, {}, 0, 0,
                         6, 0, 0, 0, kAreaRadius, 3.0f, cast_id});
                }
            }
        }
        else if (boss.final_phase == 1)
        {
            last_due = tick + Seconds(pattern == 1 ? 0.6f : 0.9f);
            if (pattern == 0)
                add({last_due, BossActionKind::Dash, boss.id.value, {}, direction, 14.0f,
                     20.0f, 25, 0, 0, 0, 0, 0, cast_id});
            else if (pattern == 1)
                add({last_due, BossActionKind::Volley, boss.id.value, {}, direction, 5.0f,
                     0, 14, 9, 90.0f, 0, 0, 0, cast_id});
            else
                add({last_due, BossActionKind::Shockwave, boss.id.value, boss.position, {},
                     0, 4.0f, 20, 0, 0, 0, 14.0f, 0.75f, cast_id});
        }
        else
        {
            last_due = tick + Seconds(pattern == 1 ? 0.35f : pattern == 2 ? 0.8f : 0.4f);
            if (pattern == 0)
            {
                add({last_due, BossActionKind::Dash, boss.id.value, {}, {}, 14.0f, 20.0f,
                     25, 0, 0, 0, 0, 0, cast_id});
                last_due += Seconds(20.0f / 14.0f + 0.4f);
                add({last_due, BossActionKind::Dash, boss.id.value, {}, {}, 14.0f, 20.0f,
                     25, 0, 0, 0, 0, 0, cast_id});
            }
            else if (pattern == 1)
            {
                add({last_due, BossActionKind::Volley, boss.id.value, {}, direction, 5.0f,
                     0, 14, 11, 90.0f, 0, 0, 0, cast_id});
                last_due += Seconds(0.35f);
                add({last_due, BossActionKind::Volley, boss.id.value, {}, direction, 5.0f,
                     0, 14, 11, 90.0f, 8.0f, 0, 0, cast_id});
            }
            else
            {
                const auto predicted = Add(player.position, Multiply(player_velocity, 0.8f));
                for (std::uint32_t index = 0; index < 5; ++index)
                {
                    const auto angle = 72.0f * index +
                                       static_cast<float>(Random(boss.random_key, cast_id) % 72);
                    add({last_due, BossActionKind::Area, boss.id.value,
                         Add(predicted, Multiply(Rotate({0, 1}, angle), 3.0f)), {}, 0, 0,
                         10, 0, 0, 0, 2.5f, 4.0f, cast_id});
                }
            }
            ++boss.phase_pattern_count;
        }

        const auto recovery = kind == BossKind::Final && boss.final_phase == 2
                                  ? Seconds(boss.phase_pattern_count % 3 == 0 ? 2.0f : 1.2f)
                                  : Seconds(data.bosses[static_cast<std::size_t>(kind)]
                                                .recovery_seconds);
        boss.pattern_ready = last_due + recovery;
    }

    void AiIntentPhase()
    {
        for (auto &enemy : enemies)
        {
            if (enemy.dead)
            {
                continue;
            }
            if (enemy.boss)
            {
                BossAi(enemy);
                continue;
            }
            const auto direction = Normalize(Subtract(player.position, enemy.position));
            const auto distance = Length(Subtract(player.position, enemy.position));
            enemy.velocity = {};
            if (enemy.attacking)
            {
                if (tick >= enemy.attack_resolve)
                {
                    const auto &definition = data.enemies[static_cast<std::size_t>(enemy.kind)];
                    if (enemy.kind == EnemyKind::Melee)
                    {
                        if (distance <= 1.0f)
                        {
                            EmitVfx("particle.enemy.melee.hit", player.position,
                                    enemy.locked_aim, 1.0f, 0.1f);
                            DealDamage(0, enemy.damage, SkillKind::Count,
                                       EffectOrigin::Original, enemy.attack_cast_id,
                                       0, false, 0.0f, 0, kNoTelemetrySource,
                                       kNoTelemetrySource,
                                       static_cast<std::uint8_t>(
                                           EnemyTelemetryIndex(enemy)));
                        }
                    }
                    else if (enemy.kind == EnemyKind::Ranged)
                    {
                        auto *shot = FireProjectile(SkillKind::BasicAttack, enemy.position,
                                                    enemy.locked_aim,
                                                     static_cast<float>(enemy.damage),
                                                     EffectOrigin::Original,
                                                     enemy.attack_cast_id, 0, false,
                                                     kNoTelemetrySource,
                                                     kNoTelemetrySource,
                                                     static_cast<std::uint8_t>(
                                                         EnemyTelemetryIndex(enemy)));
                        if (shot)
                        {
                            shot->velocity = Multiply(enemy.locked_aim,
                                                       definition.projectile_speed);
                            shot->remaining_range = definition.projectile_range;
                        }
                    }
                    else if (distance <= 3.0f)
                    {
                        EmitVfx("particle.enemy.suicide.explosion",
                                enemy.position, {}, 1.0f, 0.15f);
                        DealDamage(0, enemy.damage, SkillKind::Count,
                                   EffectOrigin::Original, enemy.attack_cast_id,
                                   0, false, 0.0f, 0, kNoTelemetrySource,
                                   kNoTelemetrySource,
                                   static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)));
                        enemy.dead = true;
                    }
                    enemy.attacking = false;
                    enemy.next_attack = tick + enemy.attack_cooldown_ticks;
                }
                continue;
            }
            if (tick >= enemy.next_attack &&
                ((enemy.kind == EnemyKind::Melee && distance <= 1.0f) ||
                 (enemy.kind == EnemyKind::Ranged && distance <= enemy.attack_range) ||
                 (enemy.kind == EnemyKind::Suicide && distance <= 2.2f)))
            {
                ++balance.enemy_attack_attempts[EnemyTelemetryIndex(enemy)];
                enemy.attack_cast_id = next_enemy_attack_id++;
                enemy.attacking = true;
                enemy.attack_resolve = tick + enemy.warning_ticks;
                enemy.locked_aim = direction;
                if (enemy.kind == EnemyKind::Melee)
                    EmitVfx("particle.enemy.melee.windup", enemy.position,
                            direction, 1.0f, 0.1f);
                else if (enemy.kind == EnemyKind::Suicide)
                    EmitVfx("particle.enemy.suicide.charge", enemy.position,
                            direction, 1.0f, 0.15f);
                continue;
            }
            if (enemy.kind == EnemyKind::Ranged)
            {
                if (distance > enemy.attack_range) enemy.velocity = direction;
            }
            else
            {
                enemy.velocity = direction;
            }
            enemy.velocity = Multiply(enemy.velocity, enemy.move_speed * SlowMultiplier(enemy));
        }
    }

    void CastAttackPhase()
    {
        CastBasicAttack();
        std::ranges::sort(boss_actions, {}, &BossAction::due);
        std::size_t boss_actions_executed{};
        while (boss_actions_executed < boss_actions.size() &&
               boss_actions[boss_actions_executed].due <= tick)
        {
            const auto action = boss_actions[boss_actions_executed++];
            auto *boss = FindEnemy(action.boss_id);
            if (!boss || boss->dead) continue;
            boss->attack_cast_id = action.cast_id;
            auto direction = action.direction;
            if (LengthSquared(direction) <= 0.0001f)
                direction = Normalize(Subtract(player.position, boss->position));
            if (action.kind == BossActionKind::Dash)
            {
                EmitVfx("particle.boss.dash.start", boss->position,
                        direction, 1.0f, 0.12f);
                boss->velocity = Multiply(direction, action.speed);
                boss->dash_until = tick + Seconds(action.distance / action.speed);
                boss->dash_damage = action.damage;
                boss->dash_hit = false;
            }
            else if (action.kind == BossActionKind::Volley)
            {
                EmitVfx("particle.boss.volley.release", boss->position,
                        direction, 1.0f, 1.0f);
                for (std::uint32_t index = 0; index < action.projectile_count; ++index)
                {
                    const auto angle = action.projectile_count == 1 ? action.angle_offset :
                        -action.arc_degrees * 0.5f +
                        action.arc_degrees * index /
                            static_cast<float>(action.projectile_count - 1) +
                        action.angle_offset;
                    if (auto *shot = FireProjectile(
                            SkillKind::BasicAttack, boss->position,
                             Rotate(direction, angle), static_cast<float>(action.damage),
                             EffectOrigin::Original, action.cast_id, 0, false,
                             kNoTelemetrySource, kNoTelemetrySource,
                             static_cast<std::uint8_t>(EnemyTelemetryIndex(*boss))))
                    {
                        shot->velocity = Multiply(Normalize(shot->velocity), action.speed);
                        shot->remaining_range = 24.0f;
                    }
                }
            }
            else if (action.kind == BossActionKind::Area)
            {
                EmitVfx("particle.boss.area.activate", action.position, {},
                        action.radius / 2.2f, 0.12f);
                SpawnArea(AreaKind::EnemyDamage, SkillKind::Count, action.position,
                          action.radius,
                          static_cast<float>(action.damage) /
                              std::max(EffectiveAttack(), 1.0f),
                           action.duration, 0.0f, EffectOrigin::Original,
                           action.cast_id, 0, 0.0f, 0.0f, 0.0f, false,
                           kNoTelemetrySource, kNoTelemetrySource,
                           static_cast<std::uint8_t>(EnemyTelemetryIndex(*boss)));
            }
            else
            {
                EmitVfx("particle.boss.shockwave.release", action.position, {},
                        action.radius / 9.75f, 0.15f);
                SpawnArea(AreaKind::EnemyDamage, SkillKind::Count, action.position,
                          action.radius,
                          static_cast<float>(action.damage) /
                              std::max(EffectiveAttack(), 1.0f),
                           action.duration, 0.0f, EffectOrigin::Original,
                           action.cast_id, 0, 0.0f, 0.0f, 0.0f, false,
                           kNoTelemetrySource, kNoTelemetrySource,
                           static_cast<std::uint8_t>(EnemyTelemetryIndex(*boss)));
                auto &area = areas.back();
                area.ring_inner_radius = action.distance;
                area.ring_outer_radius = action.radius;
                area.safe_gap_count = 4;
                area.safe_gap_degrees = 25.0f;
                area.interval = 1;
            }
        }
        boss_actions.erase(boss_actions.begin(),
                           boss_actions.begin() + boss_actions_executed);
        std::ranges::sort(scheduled, {}, &ScheduledAction::due);
        std::size_t executed{};
        while (executed < scheduled.size() && scheduled[executed].due <= tick)
        {
            const auto action = scheduled[executed++];
            if (action.kind == ScheduledKind::Projectile)
            {
                    auto *projectile = FireProjectile(
                        action.skill, action.position, action.direction,
                        action.damage_coefficient, action.origin, action.cast_id,
                        action.upgrade_mask, true,
                        action.source_upgrade, action.source_relic);
                if (projectile && action.skill == SkillKind::BasicAttack &&
                    action.origin == EffectOrigin::Original)
                {
                    if (action.source_upgrade == 0)
                    {
                        ++player.basic_sequence;
                        ++balance.skill_uses[
                            static_cast<std::size_t>(SkillKind::BasicAttack)];
                    }
                    ++player.basic_arrow_sequence;
                    if (HasUpgrade(action.upgrade_mask, 4) &&
                        player.basic_arrow_sequence % 3 == 0)
                    {
                        projectile->bleed_stacks = 1;
                    }
                    if (HasUpgrade(action.upgrade_mask, 5) &&
                        player.basic_arrow_sequence % 4 == 0)
                    {
                        projectile->burn = true;
                    }
                    EmitPresentation(PresentationKind::Audio, action.position,
                                     0x6172726F775F7368ull);
                }
                if (projectile && action.skill == SkillKind::PiercingShot &&
                    action.source_upgrade == 0)
                {
                    if (const auto *target = NearestEnemy(action.position, 24.0f))
                    {
                        projectile->homing = true;
                        projectile->homing_target = target->id.value;
                    }
                }
                if (projectile && action.skill == SkillKind::RicochetArrow &&
                    action.origin == EffectOrigin::Original)
                {
                    projectile->homing = true;
                    if (const auto *target = NearestEnemy(action.position, 18.0f))
                        projectile->homing_target = target->id.value;
                }
            }
            else if (action.kind == ScheduledKind::Volley)
            {
                for (std::uint32_t index = 0; index < action.projectile_count; ++index)
                {
                    FireProjectile(action.skill, action.position,
                                   Rotate(action.direction, -25.0f + index * 12.5f),
                                   action.damage_coefficient, action.origin, action.cast_id,
                                   action.upgrade_mask, true, action.source_upgrade,
                                   action.source_relic);
                }
            }
            else if (action.kind == ScheduledKind::Explosion)
            {
                RecordUpgradeEffect(action.skill, action.source_upgrade,
                                    UpgradeEffectMetric::ExplosionsCreated);
                DamageArea(action.position, action.radius,
                           RoundDamage(EffectiveAttack() * action.damage_coefficient),
                           action.skill, action.origin, action.cast_id, 0, false, 0.0f, 0,
                           action.source_upgrade, action.source_relic);
                EmitVfx(action.skill == SkillKind::ExplosiveArrow &&
                                action.source_upgrade == 0
                            ? "particle.skill.explosive_arrow.secondary"
                            : "particle.common.explosion_small",
                        action.position, {}, action.radius / 3.0f, 0.15f);
            }
            else
            {
                SpawnArea(AreaKind::Damage, action.skill, action.position, action.radius,
                           action.damage_coefficient, action.duration, 0.0f,
                           action.origin, action.cast_id, action.upgrade_mask,
                           0.0f, 0.0f, 0.0f, false,
                           action.source_upgrade, action.source_relic);
            }
        }
        scheduled.erase(scheduled.begin(), scheduled.begin() + executed);
    }

    void MovementPhase()
    {
        player.previous_position = player.position;
        const auto stationary_attack = input.held.basic_attack_held ||
                                       tick < player.active_cast_tick;
        if (player.retreat_landing_pending && tick >= player.retreat_until)
        {
            if (player.forced_move_skill == SkillKind::RetreatShot)
                EmitVfx("particle.skill.retreat_shot.land", player.position,
                        player.facing, 1.0f, 0.12f);
            if (player.forced_move_skill == SkillKind::Trap &&
                HasUpgrade(player.retreat_upgrade_mask, 2))
            {
                SpawnArea(AreaKind::Slow, SkillKind::Trap, player.position, 3.0f,
                          0.0f, 3.0f, 0.0f, EffectOrigin::Derived,
                          player.retreat_followup_cast, 0, 0.45f, 3.0f, 0.0f,
                          false, 1);
            }
            else if (player.forced_move_skill == SkillKind::RetreatShot &&
                     HasUpgrade(player.retreat_upgrade_mask, 5))
            {
                EmitVfx("particle.common.push", player.position, {}, 1.0f, 0.12f);
                DamageArea(player.position, 5.0f,
                            RoundDamage(EffectiveAttack() * 3.0f),
                           SkillKind::RetreatShot, EffectOrigin::Derived,
                           player.retreat_followup_cast, 0, false, 0.0f, 0, 4);
                for (auto &enemy : enemies)
                {
                    if (!enemy.dead && !enemy.boss &&
                        DistanceSquared(player.position, enemy.position) <= 25.0f)
                    {
                        const auto before = enemy.position;
                        enemy.position = Add(
                            enemy.position,
                            Multiply(Normalize(Subtract(enemy.position, player.position)),
                                     3.0f));
                        RecordUpgradeDisplacement(SkillKind::RetreatShot, 4, before,
                                                  enemy.position);
                    }
                }
            }
            player.retreat_landing_pending = false;
            player.forced_move_skill = SkillKind::Count;
        }
        if (player.retreat_followup_tick != 0 && tick >= player.retreat_followup_tick)
        {
            const auto before = player.position;
            if (!config.stationary_combat_simulation)
                player.position = Add(player.position,
                                      Multiply(player.retreat_followup_direction, -2.5f));
            RecordUpgradeDisplacement(SkillKind::RetreatShot, 7, before,
                                      player.position);
            auto direction = player.retreat_followup_direction;
            auto *target = NearestEnemy(player.position,
                                        data.skills[static_cast<std::size_t>(
                                            SkillKind::RetreatShot)].range);
            if (target)
                direction = Normalize(Subtract(target->position, player.position));
            if (auto *arrow = FireProjectile(
                    SkillKind::RetreatShot, player.position, direction, 2.0f,
                    EffectOrigin::Derived, player.retreat_followup_cast, 0, true, 7);
                arrow && target)
            {
                arrow->homing = true;
                arrow->homing_target = target->id.value;
            }
            player.retreat_followup_tick = 0;
        }
        if (tick < player.retreat_until && !config.stationary_combat_simulation)
        {
            player.position = Add(player.position, Multiply(player.retreat_velocity, kTickSeconds));
        }
        else if (player.has_move_target && !stationary_attack)
        {
            const auto delta = Subtract(player.move_target, player.position);
            const auto charge_multiplier = player.charging ? 0.7f : 1.0f;
            const auto step = EffectiveMoveSpeed() * charge_multiplier * kTickSeconds;
            if (LengthSquared(delta) > step * step)
            {
                player.position = Add(player.position,
                                      Multiply(Normalize(delta), step));
                player.facing = Normalize(delta, player.facing);
            }
            else
            {
                player.position = player.move_target;
                player.has_move_target = false;
            }
        }
        if (stationary_attack)
        {
            player.facing = player.aim;
        }
        player.position.x = std::clamp(player.position.x, -data.arena_half_extent,
                                       data.arena_half_extent);
        player.position.y = std::clamp(player.position.y, -data.arena_half_extent,
                                       data.arena_half_extent);
        player.movement_since_echo += Length(Subtract(player.position,
                                                       player.previous_position));
        const auto moved = Length(Subtract(player.position, player.previous_position));
        const auto move_points = player.stats[static_cast<std::size_t>(StatKind::MoveSpeed)];
        if (move_points != 0 && moved > 0.0f && tick >= player.retreat_until)
        {
            const auto extra = moved * (1.0f - 1.0f / (1.0f + 0.03f * move_points));
            balance.stat_utility[static_cast<std::size_t>(StatKind::MoveSpeed)] +=
                static_cast<std::uint64_t>(std::llround(extra * 1000.0f));
        }
        const auto locomotion_target =
            LengthSquared(Subtract(player.position, player.previous_position)) > 0.000001f
                ? 1.0f : 0.0f;
        const auto locomotion_step = 1.0f / (Seconds(0.12f));
        player.locomotion_blend += std::clamp(locomotion_target - player.locomotion_blend,
                                               -locomotion_step, locomotion_step);
        if (tick - player.last_position_sample >=
            Seconds(data.relics.movement_echo.position_history_age_seconds))
        {
            player.one_second_ago = player.previous_position;
            player.last_position_sample = tick;
        }

        for (auto &enemy : enemies)
        {
            if (enemy.dead) continue;
            std::erase_if(enemy.status.slows,
                          [this](const SlowEffect &slow) {
                              return slow.expires <= tick;
                          });
            for (const auto &slow : enemy.status.slows)
                RecordUpgradeEffect(slow.source_skill, slow.source_upgrade,
                                    UpgradeEffectMetric::SlowActiveTicks);
            enemy.previous_position = enemy.position;
            enemy.position = Add(enemy.position, Multiply(enemy.velocity, kTickSeconds));
            enemy.position.x = std::clamp(enemy.position.x, -data.arena_half_extent,
                                          data.arena_half_extent);
            enemy.position.y = std::clamp(enemy.position.y, -data.arena_half_extent,
                                          data.arena_half_extent);
            if (enemy.boss && tick < enemy.dash_until && !enemy.dash_hit &&
                SegmentCircle(enemy.previous_position, enemy.position,
                              player.position, 1.5f))
            {
                DealDamage(0, enemy.dash_damage, SkillKind::Count,
                           EffectOrigin::Original, enemy.attack_cast_id, 0, false,
                           0.0f, 0, kNoTelemetrySource, kNoTelemetrySource,
                           static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)));
                enemy.dash_hit = true;
            }
            if (enemy.boss && enemy.attacking && tick >= enemy.attack_resolve)
            {
                if (DistanceSquared(enemy.position, player.position) <= 2.25f)
                {
                    DealDamage(0, enemy.damage, SkillKind::Count,
                               EffectOrigin::Original, enemy.attack_cast_id, 0, false,
                               0.0f, 0, kNoTelemetrySource, kNoTelemetrySource,
                               static_cast<std::uint8_t>(EnemyTelemetryIndex(enemy)));
                }
                enemy.attacking = false;
                enemy.velocity = {};
            }
        }
        for (auto &projectile : projectiles)
        {
            if (projectile.dead) continue;
            projectile.previous_position = projectile.position;
            if (projectile.homing && projectile.homing_target != 0)
            {
                if (const auto *target = FindEnemy(projectile.homing_target))
                {
                    projectile.velocity = Multiply(
                        Normalize(Subtract(target->position, projectile.position)),
                        Length(projectile.velocity));
                }
            }
            const auto step = Multiply(projectile.velocity, kTickSeconds);
            projectile.position = Add(projectile.position, step);
            projectile.remaining_range -= Length(step);
        }
    }

    void SpatialGridPhase()
    {
        const auto coordinate = [this](float value) {
            return std::clamp(static_cast<int>(std::floor(
                                  (value + data.arena_half_extent) / kGridCellSize)),
                              0, kGridDimension - 1);
        };
        const auto rebuild = [&]() {
            for (auto &cell : enemy_grid) cell.clear();
            for (std::size_t index = 0; index < enemies.size(); ++index)
            {
                if (enemies[index].dead) continue;
                const auto x = coordinate(enemies[index].position.x);
                const auto y = coordinate(enemies[index].position.y);
                enemy_grid[static_cast<std::size_t>(y * kGridDimension + x)].push_back(index);
            }
        };
        const auto separate = [this](std::size_t left, std::size_t right) {
            if (enemies[left].dead || enemies[right].dead || enemies[left].boss ||
                enemies[right].boss)
            {
                return;
            }
            const auto delta = Subtract(enemies[left].position, enemies[right].position);
            const auto distance_squared = LengthSquared(delta);
            if (distance_squared <= 0.0001f || distance_squared >= 0.64f) return;
            const auto push = Multiply(Normalize(delta),
                                       (0.8f - std::sqrt(distance_squared)) * 0.5f);
            enemies[left].position = Add(enemies[left].position, push);
            enemies[right].position = Subtract(enemies[right].position, push);
        };

        rebuild();
        constexpr std::array<std::pair<int, int>, 4> kForwardNeighbors{{
            {1, 0}, {-1, 1}, {0, 1}, {1, 1},
        }};
        for (int y = 0; y < kGridDimension; ++y)
        {
            for (int x = 0; x < kGridDimension; ++x)
            {
                const auto &cell = enemy_grid[static_cast<std::size_t>(y * kGridDimension + x)];
                for (std::size_t left = 0; left < cell.size(); ++left)
                {
                    for (std::size_t right = left + 1; right < cell.size(); ++right)
                    {
                        separate(cell[left], cell[right]);
                    }
                }
                for (const auto [offset_x, offset_y] : kForwardNeighbors)
                {
                    const auto neighbor_x = x + offset_x;
                    const auto neighbor_y = y + offset_y;
                    if (neighbor_x < 0 || neighbor_x >= kGridDimension ||
                        neighbor_y >= kGridDimension)
                    {
                        continue;
                    }
                    const auto &neighbor = enemy_grid[static_cast<std::size_t>(
                        neighbor_y * kGridDimension + neighbor_x)];
                    for (const auto left : cell)
                    {
                        for (const auto right : neighbor) separate(left, right);
                    }
                }
            }
        }
        for (auto &enemy : enemies)
        {
            enemy.position.x = std::clamp(enemy.position.x, -data.arena_half_extent,
                                          data.arena_half_extent);
            enemy.position.y = std::clamp(enemy.position.y, -data.arena_half_extent,
                                          data.arena_half_extent);
        }
        rebuild();
    }

    bool AlreadyHit(const ProjectileActor &projectile, std::uint64_t target) const noexcept
    {
        const auto hits = std::span(projectile.hit_ids);
        return std::ranges::find(hits, target) != hits.end();
    }

    void RecordHit(ProjectileActor &projectile, std::uint64_t target)
    {
        projectile.hit_ids.push_back(target);
        ++projectile.hit_count;
    }

    std::uint8_t IncrementCastHit(std::uint64_t cast, std::uint64_t target)
    {
        const auto iterator = std::ranges::find_if(cast_hits, [=](const CastHitRecord &record) {
            return record.cast_id == cast && record.target == target;
        });
        if (iterator != cast_hits.end())
        {
            return ++iterator->count;
        }
        cast_hits.push_back({cast, target, 1});
        return 1;
    }

    std::uint8_t IncrementAreaHit(std::uint64_t area, std::uint64_t target)
    {
        const auto iterator = std::ranges::find_if(area_hits, [=](const AreaHitRecord &record) {
            return record.area_id == area && record.target == target;
        });
        if (iterator != area_hits.end()) return ++iterator->count;
        area_hits.push_back({area, target, 1});
        return 1;
    }

    CastRuntime &RuntimeForCast(std::uint64_t cast, SkillKind skill)
    {
        const auto iterator = std::ranges::find(cast_runtime, cast,
                                                &CastRuntime::cast_id);
        if (iterator != cast_runtime.end()) return *iterator;
        cast_runtime.push_back({cast, skill});
        return cast_runtime.back();
    }

    void OnProjectileHit(ProjectileActor &projectile, EnemyActor &enemy)
    {
        RecordHit(projectile, enemy.id.value);
        const auto cast_hit = IncrementCastHit(projectile.cast_id, enemy.id.value);
        auto &runtime = RuntimeForCast(projectile.cast_id, projectile.skill);
        if (!enemy.boss) ++runtime.normal_hits;
        auto damage = projectile.damage;
        if (projectile.skill == SkillKind::PiercingShot)
        {
            const auto multiplier = std::max(0.55f, 1.0f - 0.15f *
                                                           (projectile.hit_count - 1));
            damage = RoundDamage(projectile.damage * multiplier);
        }
        auto bleed = projectile.bleed_stacks;
        auto burn = projectile.burn;
        auto slow = 0.0f;
        auto slow_duration = Tick{};
        auto damage_upgrade = projectile.source_upgrade;
        if (damage_upgrade == kNoTelemetrySource &&
            projectile.skill == SkillKind::BasicAttack &&
            HasUpgrade(projectile.upgrade_mask, 2) && projectile.hit_count > 1)
            damage_upgrade = 1;
        if (damage_upgrade == kNoTelemetrySource &&
            projectile.skill == SkillKind::MultiShot)
        {
            if (HasUpgrade(projectile.upgrade_mask, 3) && projectile.hit_count > 2)
                damage_upgrade = 2;
        }
        if (damage_upgrade == kNoTelemetrySource &&
            projectile.skill == SkillKind::ChargedShot &&
            HasUpgrade(projectile.upgrade_mask, 4) && projectile.hit_count > 4)
            damage_upgrade = 3;
        if (damage_upgrade == kNoTelemetrySource &&
            projectile.skill == SkillKind::RicochetArrow &&
            HasUpgrade(projectile.upgrade_mask, 3) && projectile.hit_count > 6)
            damage_upgrade = 2;
        if (damage_upgrade < kUpgradeCount &&
            projectile.source_upgrade == kNoTelemetrySource)
            RecordUpgradeEffect(projectile.skill, damage_upgrade,
                                UpgradeEffectMetric::ExtraTargetsHit);
        if (projectile.skill == SkillKind::PiercingShot && HasUpgrade(projectile.upgrade_mask, 3))
        {
            bleed = std::max<std::uint8_t>(bleed, 2);
        }
        if (projectile.skill == SkillKind::MultiShot &&
            HasUpgrade(projectile.upgrade_mask, 5) && cast_hit == 1)
        {
            bleed = std::max<std::uint8_t>(bleed, 1);
        }
        const auto multi_hit_limit = projectile.skill == SkillKind::MultiShot
            ? 2u
            : std::numeric_limits<unsigned>::max();
        if (cast_hit <= multi_hit_limit)
        {
            if (projectile.carried_burn_attack > 0.0f)
            {
                ApplyBurn(enemy, projectile.carried_burn_attack, false,
                          projectile.carried_burn_expires > tick
                              ? projectile.carried_burn_expires - tick
                              : 1,
                          projectile.skill, 3);
                projectile.carried_burn_attack = 0.0f;
            }
            if (projectile.skill == SkillKind::ChargedShot && projectile.full_charge &&
                HasUpgrade(projectile.upgrade_mask, 5) &&
                enemy.status.bleed_count == 5)
            {
                DealDamage(enemy.id.value, RoundDamage(EffectiveAttack() * 1.5f),
                           projectile.skill, EffectOrigin::Derived,
                           projectile.cast_id, 0, false, 0.0f, 0, 4);
            }
            if (projectile.skill != SkillKind::ExplosiveArrow ||
                projectile.explosion_radius <= 0.0f)
            {
                DealDamage(enemy.id.value, damage, projectile.skill, projectile.origin,
                           projectile.cast_id, bleed, burn, slow, slow_duration,
                           damage_upgrade, projectile.source_relic);
                if (projectile.skill == SkillKind::ChargedShot &&
                    projectile.charge_ratio > 0.0f)
                {
                    auto &event = damage_events.back();
                    const auto add_amplifier = [&](std::uint8_t upgrade,
                                                   float coefficient) {
                        if (coefficient <= 0.0f ||
                            event.amplified_count == event.amplified_damage.size())
                            return;
                        const auto index = event.amplified_count++;
                        event.amplified_upgrades[index] = upgrade;
                        event.amplified_damage[index] =
                            RoundDamage(EffectiveAttack() * coefficient);
                    };
                    const auto decay = 1.0f;
                    if (HasUpgrade(projectile.upgrade_mask, 1))
                        add_amplifier(0, 3.0f * projectile.charge_ratio * decay);
                    if (HasUpgrade(projectile.upgrade_mask, 3))
                        add_amplifier(2,
                                      7.0f * (1.0f - projectile.charge_ratio) * decay);
                }
            }
        }

        if (projectile.skill == SkillKind::BasicAttack &&
            HasUpgrade(projectile.upgrade_mask, 6) && projectile.hit_count == 1)
        {
            enemy.status.slows.push_back({0.2f, tick + Seconds(1.0f),
                                          SkillKind::BasicAttack, 5});
            RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                                UpgradeEffectMetric::SlowApplications);
            RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                                UpgradeEffectMetric::SlowTargetTicks,
                                Seconds(1.0f));
            const auto longest = std::ranges::max_element(player.cooldowns);
            if (longest != player.cooldowns.end())
            {
                const auto before = *longest;
                *longest = *longest > 12 ? *longest - 12 : 0;
                RecordUpgradeEffect(SkillKind::BasicAttack, 5,
                                    UpgradeEffectMetric::CooldownTicksSaved,
                                    before - *longest);
            }
        }
        if (projectile.skill == SkillKind::PiercingShot &&
            HasUpgrade(projectile.upgrade_mask, 6) && !enemy.boss)
        {
            const auto before = enemy.position;
            enemy.position = Add(enemy.position,
                                 Multiply(Normalize(projectile.velocity), 1.5f));
            RecordUpgradeDisplacement(SkillKind::PiercingShot, 5, before,
                                      enemy.position);
        }
        if (projectile.skill == SkillKind::PiercingShot &&
            HasUpgrade(projectile.upgrade_mask, 7))
        {
            if (runtime.transfer_count == 0)
            {
                if (!enemy.status.burn)
                    ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                              projectile.skill, 6);
                runtime.has_stored_burn = true;
                runtime.stored_burn_attack = enemy.status.burn->attack_snapshot;
                runtime.stored_burn_remaining = enemy.status.burn->expires > tick
                    ? enemy.status.burn->expires - tick
                    : 1;
                runtime.transfer_count = 1;
            }
            else if (runtime.transfer_count < 4 && runtime.has_stored_burn)
            {
                ApplyBurn(enemy, runtime.stored_burn_attack, false,
                          runtime.stored_burn_remaining, projectile.skill, 6);
                ++runtime.transfer_count;
            }
        }
        if (projectile.skill == SkillKind::ChargedShot &&
            HasUpgrade(projectile.upgrade_mask, 6) && !runtime.triggered &&
            (enemy.boss || runtime.normal_hits >= 3))
        {
            runtime.triggered = true;
            const auto direction = Normalize(projectile.velocity);
            for (const auto angle : {-35.0f, -25.0f, -15.0f, -5.0f,
                                     5.0f, 15.0f, 25.0f, 35.0f})
                FireProjectile(projectile.skill, projectile.position,
                               Rotate(direction, angle), 3.5f,
                               EffectOrigin::Derived, projectile.cast_id, 0, true, 5);
        }
        if (projectile.skill == SkillKind::ChargedShot &&
            HasUpgrade(projectile.upgrade_mask, 7) && runtime.transfer_count == 0)
        {
            if (!enemy.status.burn)
                ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                           projectile.skill, 6);
            runtime.transfer_count = 1;
            RecordUpgradeEffect(SkillKind::ChargedShot, 6,
                                UpgradeEffectMetric::ExplosionsCreated);
            DamageArea(enemy.position, 3.5f,
                       RoundDamage(EffectiveAttack() * 2.0f), projectile.skill,
                       EffectOrigin::Derived, projectile.cast_id, 0, false, 0.0f, 0, 6);
        }
        if (projectile.skill == SkillKind::MultiShot &&
            HasUpgrade(projectile.upgrade_mask, 6) &&
            projectile.origin == EffectOrigin::Original && runtime.transfer_count < 3)
        {
            if (!enemy.status.burn)
                ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                           projectile.skill, 5);
            EnemyActor *target{};
            auto best = std::numeric_limits<float>::max();
            for (auto &candidate : enemies)
            {
                if (candidate.dead || candidate.id.value == enemy.id.value ||
                    candidate.status.burn)
                    continue;
                const auto distance = DistanceSquared(enemy.position, candidate.position);
                if (distance < best)
                {
                    best = distance;
                    target = &candidate;
                }
            }
            if (target)
            {
                if (auto *arrow = FireProjectile(
                        projectile.skill, enemy.position,
                        Normalize(Subtract(target->position, enemy.position)), 0.7f,
                        EffectOrigin::Derived, projectile.cast_id,
                        static_cast<std::uint8_t>(projectile.upgrade_mask &
                                                  (std::uint8_t{1} << 7)),
                        true, 5))
                {
                    arrow->homing = true;
                    arrow->homing_target = target->id.value;
                    arrow->burn = true;
                    ++runtime.transfer_count;
                }
            }
        }
        if (projectile.skill == SkillKind::ExplosiveArrow &&
            projectile.origin == EffectOrigin::Original &&
            HasUpgrade(projectile.upgrade_mask, 7))
        {
            enemy.marked_by_skill = projectile.skill;
            enemy.marked_damage_coefficient = 4.0f;
            enemy.mark_expires = tick + Seconds(8.0f);
            EmitVfx("particle.common.mark_apply", enemy.position, {},
                    1.0f, enemy.boss ? 1.4f : 0.75f);
            RecordUpgradeEffect(SkillKind::ExplosiveArrow, 6,
                                UpgradeEffectMetric::ExplosionsCreated);
            DamageArea(enemy.position, 2.0f,
                       RoundDamage(EffectiveAttack() * 3.0f), projectile.skill,
                       EffectOrigin::Derived, projectile.cast_id, 0, false,
                       0.0f, 0, 6);
            RecordUpgradeEffect(SkillKind::ExplosiveArrow, 6,
                                UpgradeEffectMetric::MarksApplied);
        }
        if (projectile.skill == SkillKind::RetreatShot &&
            HasUpgrade(projectile.upgrade_mask, 4) && runtime.spawn_count < 3)
        {
            if (enemy.status.bleed_count == 0)
                ApplyBleed(enemy, EffectiveAttack(), 1, projectile.skill, 3);
            if (auto *target = NearestEnemy(enemy.position, 8.0f,
                    std::span(projectile.hit_ids)))
            {
                if (auto *arrow = FireProjectile(projectile.skill, enemy.position,
                        Normalize(Subtract(target->position, enemy.position)), 1.5f,
                        EffectOrigin::Derived, projectile.cast_id, 0, true, 3))
                {
                    arrow->homing = true;
                    arrow->homing_target = target->id.value;
                    ++runtime.spawn_count;
                }
            }
        }
        if (projectile.skill == SkillKind::RetreatShot &&
            HasUpgrade(projectile.upgrade_mask, 7) && !runtime.triggered &&
            (enemy.boss || runtime.normal_hits >= 3))
        {
            runtime.triggered = true;
            RecordUpgradeEffect(
                SkillKind::RetreatShot, 6, UpgradeEffectMetric::Activations);
            RecordUpgradeEffect(
                SkillKind::RetreatShot, 6, UpgradeEffectMetric::Healing,
                Heal(RoundDamage(player.max_health * 0.05f)));
        }

        if (projectile.origin == EffectOrigin::Original)
        {
            const auto direction = Normalize(projectile.velocity);
            if (projectile.skill == SkillKind::BasicAttack &&
                HasUpgrade(projectile.upgrade_mask, 3) && projectile.hit_count == 1)
            {
                FireProjectile(projectile.skill, projectile.position, Rotate(direction, -30.0f),
                               0.5f, EffectOrigin::Derived, projectile.cast_id, 0, true, 2);
                FireProjectile(projectile.skill, projectile.position, Rotate(direction, 30.0f),
                               0.5f, EffectOrigin::Derived, projectile.cast_id, 0, true, 2);
            }
            if (projectile.skill == SkillKind::MultiShot &&
                HasUpgrade(projectile.upgrade_mask, 2) && projectile.hit_count == 1)
            {
                FireProjectile(projectile.skill, projectile.position, Rotate(direction, -25.0f),
                               0.35f, EffectOrigin::Derived, projectile.cast_id,
                               static_cast<std::uint8_t>(projectile.upgrade_mask &
                                                         (std::uint8_t{1} << 7)),
                               true, 1);
                FireProjectile(projectile.skill, projectile.position, Rotate(direction, 25.0f),
                               0.35f, EffectOrigin::Derived, projectile.cast_id,
                               static_cast<std::uint8_t>(projectile.upgrade_mask &
                                                         (std::uint8_t{1} << 7)),
                               true, 1);
            }
            if (projectile.skill == SkillKind::PiercingShot &&
                HasUpgrade(projectile.upgrade_mask, 5) && projectile.hit_count % 3 == 0)
            {
                FireProjectile(projectile.skill, projectile.position, Rotate(direction, -90.0f),
                               2.0f, EffectOrigin::Derived, projectile.cast_id, 0, true, 4);
                FireProjectile(projectile.skill, projectile.position, Rotate(direction, 90.0f),
                               2.0f, EffectOrigin::Derived, projectile.cast_id, 0, true, 4);
            }
        }
        const auto hit_height = enemy.boss ? 1.4f : 0.75f;
        const auto hit_scale = enemy.boss ? 1.35f : 1.0f;
        const auto hit_direction = Normalize(projectile.velocity);
        if (projectile.skill == SkillKind::ChargedShot && projectile.full_charge)
            EmitVfx("particle.common.heavy_hit", enemy.position,
                    hit_direction, hit_scale, hit_height);
        else if (projectile.skill == SkillKind::RicochetArrow)
            EmitVfx("particle.skill.ricochet_arrow.hit", enemy.position,
                    hit_direction, hit_scale, hit_height);
        else
            EmitVfx("particle.common.hit", enemy.position,
                    hit_direction, hit_scale, hit_height);

        if (projectile.skill == SkillKind::RicochetArrow && projectile.bounce_remaining > 0)
        {
            if (HasUpgrade(projectile.upgrade_mask, 3))
            {
                if (enemy.status.bleed_count == 0)
                    ApplyBleed(enemy, EffectiveAttack(), 1, projectile.skill, 2);
            }
            if (HasUpgrade(projectile.upgrade_mask, 3) && runtime.transfer_count < 3)
            {
                ++projectile.bounce_remaining;
                ++runtime.transfer_count;
                RecordUpgradeEffect(SkillKind::RicochetArrow, 2,
                                    UpgradeEffectMetric::ExtraBounces);
            }
            --projectile.bounce_remaining;
            EnemyActor *next{};
            auto best_bleed = false;
            auto best_distance = 36.0f;
            const auto excluded = std::span(projectile.hit_ids);
            for (auto &candidate : enemies)
            {
                if (candidate.dead ||
                    std::ranges::find(excluded, candidate.id.value) != excluded.end())
                    continue;
                const auto distance = DistanceSquared(projectile.position, candidate.position);
                if (distance > 36.0f) continue;
                const auto bleeding = candidate.status.bleed_count > 0;
                if (!next || (bleeding && !best_bleed) ||
                    (bleeding == best_bleed && distance < best_distance))
                {
                    next = &candidate;
                    best_bleed = bleeding;
                    best_distance = distance;
                }
            }
            if (next)
            {
                EmitVfxLine("particle.line.ricochet", projectile.position,
                            next->position);
                if (projectile.origin == EffectOrigin::Original &&
                    HasUpgrade(projectile.upgrade_mask, 7))
                {
                    Tick *longest{};
                    for (std::size_t index = 0; index < player.cooldowns.size(); ++index)
                    {
                        if (index == static_cast<std::size_t>(
                                         SkillKind::RicochetArrow) - 1)
                            continue;
                        if (!longest || player.cooldowns[index] > *longest)
                            longest = &player.cooldowns[index];
                    }
                    if (longest)
                    {
                        const auto before = *longest;
                        *longest = before > 12 ? before - 12 : 0;
                        RecordUpgradeEffect(SkillKind::RicochetArrow, 6,
                                            UpgradeEffectMetric::CooldownTicksSaved,
                                            before - *longest);
                    }
                }
                if (HasUpgrade(projectile.upgrade_mask, 4))
                {
                    if (!enemy.status.burn)
                        ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                                  projectile.skill, 3);
                    projectile.carried_burn_attack = enemy.status.burn->attack_snapshot;
                    projectile.carried_burn_expires = enemy.status.burn->expires;
                }
                if (projectile.origin == EffectOrigin::Original &&
                    HasUpgrade(projectile.upgrade_mask, 2) && projectile.hit_count == 1)
                {
                    if (auto *branch = FireProjectile(
                            projectile.skill, projectile.position,
                            Normalize(Subtract(next->position, projectile.position)), 1.0f,
                            EffectOrigin::Derived, projectile.cast_id, 0, true, 1))
                    {
                        branch->homing = true;
                        branch->homing_target = next->id.value;
                        branch->bounce_remaining = 2;
                    }
                }
                projectile.homing = true;
                projectile.homing_target = next->id.value;
                projectile.velocity = Multiply(Normalize(Subtract(next->position,
                                                                  projectile.position)),
                                               data.skills[5].projectile_speed);
                projectile.remaining_range = 6.0f;
                return;
            }
            if (projectile.origin == EffectOrigin::Original)
            {
                runtime.terminal_target = enemy.id.value;
            }
        }
        if (projectile.skill == SkillKind::RicochetArrow &&
            HasUpgrade(projectile.upgrade_mask, 1) && !projectile.returning)
        {
            projectile.returning = true;
            projectile.source_upgrade = 0;
            projectile.homing = false;
            projectile.homing_target = 0;
            projectile.hit_count = 0;
            projectile.hit_ids.clear();
            projectile.pierce_remaining = 5;
            projectile.damage = RoundDamage(EffectiveAttack() * 0.8f);
            projectile.remaining_range = std::max(0.1f,
                Length(Subtract(player.position, projectile.position)));
            projectile.velocity = Multiply(
                Normalize(Subtract(player.position, projectile.position)),
                data.skills[5].projectile_speed);
            return;
        }
        if (projectile.pierce_remaining > 0 && (!enemy.boss || projectile.pierce_remaining != 255))
        {
            --projectile.pierce_remaining;
        }
        else if (projectile.pierce_remaining == 255 && !enemy.boss)
        {
        }
        else
        {
            projectile.dead = true;
        }
    }

    void ExplodeProjectile(ProjectileActor &projectile)
    {
        if (projectile.explosion_radius <= 0.0f)
        {
            return;
        }
        if (projectile.origin == EffectOrigin::Original &&
            projectile.skill == SkillKind::ExplosiveArrow &&
            HasUpgrade(projectile.upgrade_mask, 6))
        {
            EmitVfx("particle.common.pull", projectile.position, {},
                    projectile.explosion_radius, 0.12f);
            for (auto &enemy : enemies)
            {
                if (!enemy.dead && !enemy.boss &&
                    DistanceSquared(projectile.position, enemy.position) <= 6.25f)
                {
                    const auto before = enemy.position;
                    enemy.position = Add(enemy.position,
                        Multiply(Normalize(Subtract(projectile.position, enemy.position)),
                                 std::min(2.5f, Length(Subtract(projectile.position,
                                                                enemy.position)))));
                    RecordUpgradeDisplacement(SkillKind::ExplosiveArrow, 5, before,
                                              enemy.position);
                }
            }
        }
        const auto explosion_source =
            projectile.explosion_source_upgrade < kUpgradeCount
                ? projectile.explosion_source_upgrade
                : projectile.source_upgrade;
        RecordUpgradeEffect(projectile.skill, explosion_source,
                            UpgradeEffectMetric::ExplosionsCreated);
        DamageArea(projectile.position, projectile.explosion_radius,
                   projectile.explosion_damage != 0 ? projectile.explosion_damage
                                                    : projectile.damage,
                   projectile.skill, projectile.origin, projectile.cast_id, 0, false, 0.0f, 0,
                   explosion_source,
                   projectile.source_relic);
        if (projectile.origin == EffectOrigin::Original &&
            projectile.skill == SkillKind::ExplosiveArrow)
        {
            if (HasUpgrade(projectile.upgrade_mask, 1))
            {
                Schedule({tick + 18, ScheduledKind::Explosion, projectile.skill,
                          projectile.position, {}, 3.0f, 4.5f, 0.0f, 1,
                          0, EffectOrigin::Derived,
                          projectile.cast_id, 0});
            }
            if (HasUpgrade(projectile.upgrade_mask, 2))
            {
                for (std::uint32_t index = 0; index < 3; ++index)
                {
                    Schedule({tick + 9, ScheduledKind::Explosion, projectile.skill,
                              Add(projectile.position, Multiply(
                                  Rotate({0.0f, 1.0f}, index * 120.0f), 1.5f)),
                              {}, 1.5f, 4.5f, 0.0f, 1, 0,
                              EffectOrigin::Derived, projectile.cast_id, 1});
                }
            }
            if (HasUpgrade(projectile.upgrade_mask, 3))
            {
                for (std::uint32_t index = 0; index < 8; ++index)
                {
                    if (auto *fragment = FireProjectile(
                            projectile.skill, projectile.position,
                            Rotate({0.0f, 1.0f}, index * 45.0f), 0.7f,
                            EffectOrigin::Derived, projectile.cast_id, 0, true, 2))
                    {
                        fragment->radius *= 2.5f;
                        fragment->pierce_remaining = 1;
                    }
                }
            }
            if (HasUpgrade(projectile.upgrade_mask, 4))
            {
                SpawnArea(AreaKind::Damage, projectile.skill, projectile.position,
                          3.0f, 0.35f, 4.0f, 0.0f, EffectOrigin::Derived,
                          projectile.cast_id, 0, 0.0f, 0.0f, 0.0f, true, 3);
            }
            if (HasUpgrade(projectile.upgrade_mask, 5))
            {
                std::uint8_t blood_explosions{};
                for (auto &enemy : enemies)
                {
                    if (blood_explosions == 8) break;
                    if (!enemy.dead &&
                        DistanceSquared(projectile.position, enemy.position) <= 9.0f)
                    {
                        if (enemy.status.bleed_count == 0)
                            ApplyBleed(enemy, EffectiveAttack(), 1, projectile.skill, 4);
                        DamageArea(enemy.position, 2.0f,
                                   RoundDamage(EffectiveAttack() * 1.2f),
                                   projectile.skill, EffectOrigin::Derived,
                                   projectile.cast_id, 0, false, 0.0f, 0, 4);
                        EmitVfx("particle.common.explosion_small", enemy.position,
                                {}, 1.0f, 0.15f);
                        RecordUpgradeEffect(SkillKind::ExplosiveArrow, 4,
                                            UpgradeEffectMetric::ExplosionsCreated);
                        ++blood_explosions;
                    }
                }
            }
        }
        EmitVfx(projectile.skill == SkillKind::ExplosiveArrow &&
                        projectile.origin == EffectOrigin::Original
                    ? "particle.skill.explosive_arrow.main"
                    : "particle.common.explosion_large",
                projectile.position, {}, projectile.explosion_radius / 3.0f, 0.15f);
    }

    void CollisionHitPhase()
    {
        const auto initial_projectile_count = projectiles.size();
        for (std::size_t projectile_index = 0;
             projectile_index < initial_projectile_count; ++projectile_index)
        {
            auto &projectile = projectiles[projectile_index];
            if (projectile.dead) continue;
            if (projectile.player_owned)
            {
                const auto coordinate = [this](float value) {
                    return std::clamp(static_cast<int>(std::floor(
                                          (value + data.arena_half_extent) /
                                          kGridCellSize)),
                                      0, kGridDimension - 1);
                };
                constexpr float kLargestEnemyRadius = 1.1f;
                const auto margin = projectile.radius + kLargestEnemyRadius;
                const auto minimum_x = coordinate(std::min(projectile.previous_position.x,
                                                            projectile.position.x) - margin);
                const auto maximum_x = coordinate(std::max(projectile.previous_position.x,
                                                            projectile.position.x) + margin);
                const auto minimum_y = coordinate(std::min(projectile.previous_position.y,
                                                            projectile.position.y) - margin);
                const auto maximum_y = coordinate(std::max(projectile.previous_position.y,
                                                            projectile.position.y) + margin);
                collision_candidates.clear();
                for (auto y = minimum_y; y <= maximum_y; ++y)
                {
                    for (auto x = minimum_x; x <= maximum_x; ++x)
                    {
                        const auto &cell = enemy_grid[static_cast<std::size_t>(
                            y * kGridDimension + x)];
                        collision_candidates.insert(collision_candidates.end(), cell.begin(),
                                                    cell.end());
                    }
                }
                const auto travel = Subtract(projectile.position,
                                             projectile.previous_position);
                const auto travel_length_squared = LengthSquared(travel);
                std::ranges::sort(collision_candidates, [&](std::size_t left,
                                                            std::size_t right) {
                    const auto projection = [&](std::size_t index) {
                        if (travel_length_squared <= 0.000001f) return 0.0f;
                        const auto offset = Subtract(enemies[index].position,
                                                     projectile.previous_position);
                        return (offset.x * travel.x + offset.y * travel.y) /
                               travel_length_squared;
                    };
                    const auto left_projection = projection(left);
                    const auto right_projection = projection(right);
                    if (left_projection != right_projection)
                        return left_projection < right_projection;
                    return enemies[left].id.value < enemies[right].id.value;
                });
                for (const auto enemy_index : collision_candidates)
                {
                    auto &enemy = enemies[enemy_index];
                    if (enemy.dead || AlreadyHit(projectile, enemy.id.value)) continue;
                    const auto combined = projectile.radius + (enemy.boss ? 1.1f : 0.45f);
                    if (SegmentCircle(projectile.previous_position, projectile.position,
                                      enemy.position, combined))
                    {
                        OnProjectileHit(projectile, enemy);
                        if (projectile.dead)
                        {
                            ExplodeProjectile(projectile);
                            break;
                        }
                    }
                }
            }
            else if (SegmentCircle(projectile.previous_position, projectile.position,
                                   player.position, projectile.radius + 0.4f))
            {
                DealDamage(0, projectile.damage, SkillKind::Count,
                           EffectOrigin::Original, projectile.cast_id, 0, false, 0.0f, 0,
                           kNoTelemetrySource, kNoTelemetrySource,
                           projectile.source_enemy);
                projectile.dead = true;
            }
            if (projectile.remaining_range <= 0.0f)
            {
                if (projectile.skill == SkillKind::PiercingShot &&
                    projectile.origin == EffectOrigin::Original &&
                    HasUpgrade(projectile.upgrade_mask, 2))
                {
                    const auto direction = Normalize(projectile.velocity);
                    for (const auto angle : {-20.0f, 20.0f})
                    {
                        auto child_direction = Rotate(direction, angle);
                        const auto *target = NearestEnemy(projectile.position, 12.0f);
                        if (target)
                            child_direction = Normalize(Subtract(target->position,
                                                                 projectile.position));
                        if (auto *branch = FireProjectile(
                                projectile.skill, projectile.position, child_direction,
                                0.9f, EffectOrigin::Derived, projectile.cast_id, 0,
                                true, 1); branch && target)
                        {
                            branch->homing = true;
                            branch->homing_target = target->id.value;
                        }
                    }
                    projectile.dead = true;
                }
                else if (projectile.skill == SkillKind::MultiShot &&
                         HasUpgrade(projectile.upgrade_mask, 8) &&
                         projectile.hit_count == 0)
                {
                    if (auto *target = NearestEnemy(projectile.position, 12.0f))
                    {
                        if (auto *arrow = FireProjectile(
                                projectile.skill, projectile.position,
                                Normalize(Subtract(target->position,
                                                   projectile.position)),
                                0.55f, EffectOrigin::Derived,
                                projectile.cast_id, 0, true, 7))
                        {
                            arrow->homing = true;
                            arrow->homing_target = target->id.value;
                        }
                    }
                    projectile.dead = true;
                }
                else if (projectile.skill == SkillKind::RicochetArrow &&
                         !projectile.returning &&
                         HasUpgrade(projectile.upgrade_mask, 1))
                {
                    projectile.returning = true;
                    projectile.source_upgrade = 0;
                    projectile.hit_count = 0;
                    projectile.hit_ids.clear();
                    projectile.pierce_remaining = 5;
                    projectile.remaining_range = std::max(
                        0.1f, Length(Subtract(player.position, projectile.position)));
                    projectile.velocity = Multiply(
                        Normalize(Subtract(player.position, projectile.position)),
                        data.skills[5].projectile_speed);
                    projectile.damage = RoundDamage(EffectiveAttack() * 2.5f);
                    projectile.source_upgrade = 0;
                }
                else if (projectile.skill == SkillKind::RicochetArrow &&
                         projectile.returning)
                {
                    projectile.dead = true;
                }
                else if (projectile.skill == SkillKind::BasicAttack &&
                    HasUpgrade(projectile.upgrade_mask, 7) && !projectile.returning &&
                    projectile.hit_count == 0)
                {
                    if (auto *target = NearestEnemy(projectile.position,
                                                    data.skills[0].range))
                    {
                        projectile.returning = true;
                        projectile.source_upgrade = 6;
                        projectile.damage = RoundDamage(EffectiveAttack() * 1.75f);
                        projectile.remaining_range = data.skills[0].range;
                        projectile.velocity = Multiply(
                            Normalize(Subtract(target->position, projectile.position)),
                            data.skills[0].projectile_speed);
                        projectile.radius *= 2.0f;
                        projectile.homing = true;
                        projectile.homing_target = target->id.value;
                    }
                    else
                    {
                        projectile.dead = true;
                    }
                }
                else
                {
                    ExplodeProjectile(projectile);
                    projectile.dead = true;
                }
            }
        }

        std::vector<ScheduledAction> pending_areas;
        const auto initial_area_count = areas.size();
        for (std::size_t area_index = 0; area_index < initial_area_count; ++area_index)
        {
            auto &area = areas[area_index];
            if (area.dead || tick < area.active_tick || tick < area.next_tick) continue;
            if (area.kind == AreaKind::Trap && tick == area.active_tick)
                EmitVfx("particle.skill.trap.arm", area.position, {},
                        area.radius / 2.0f, 0.12f);
            if (area.kind == AreaKind::EnemyDamage)
            {
                if (area.ring_outer_radius > 0.0f)
                {
                    const auto duration = std::max<Tick>(area.expires - area.active_tick, 1);
                    const auto progress = std::clamp(
                        static_cast<float>(tick - area.active_tick) /
                            static_cast<float>(duration),
                        0.0f, 1.0f);
                    const auto radius = std::lerp(area.ring_inner_radius,
                                                  area.ring_outer_radius, progress);
                    const auto delta = Subtract(player.position, area.position);
                    auto angle = std::atan2(delta.y, delta.x) * 180.0f / kPi;
                    if (angle < 0.0f) angle += 360.0f;
                    const auto spacing = 360.0f / area.safe_gap_count;
                    const auto offset = static_cast<float>(area.cast_id % 360);
                    auto nearest_gap = std::fmod(angle - offset + spacing * 0.5f + 360.0f,
                                                 spacing) - spacing * 0.5f;
                    const auto safe = std::abs(nearest_gap) <= area.safe_gap_degrees * 0.5f;
                    if (!safe && std::abs(Length(delta) - radius) <= 0.75f &&
                        IncrementAreaHit(area.id.value, 0) == 1)
                    {
                        DealDamage(0, area.damage, SkillKind::Count,
                                   area.origin, area.cast_id, 0, false, 0.0f, 0,
                                   kNoTelemetrySource, kNoTelemetrySource,
                                   area.source_enemy);
                    }
                }
                else if (DistanceSquared(area.position, player.position) <=
                         area.radius * area.radius)
                {
                    DealDamage(0, area.damage, SkillKind::Count,
                               area.origin, area.cast_id, 0, false, 0.0f, 0,
                               kNoTelemetrySource, kNoTelemetrySource,
                               area.source_enemy);
                }
            }
            else if (area.kind == AreaKind::Trap)
            {
                if (auto *target = NearestEnemy(area.position, area.radius))
                {
                    if (HasUpgrade(area.upgrade_mask, 6))
                    {
                        EmitVfx("particle.common.pull", area.position, {},
                                area.effect_radius, 0.12f);
                        for (auto &enemy : enemies)
                        {
                            if (!enemy.dead && !enemy.boss &&
                                DistanceSquared(area.position, enemy.position) <=
                                    area.effect_radius * area.effect_radius)
                            {
                                const auto delta = Subtract(area.position, enemy.position);
                                const auto before = enemy.position;
                                enemy.position = Add(enemy.position, Multiply(
                                    Normalize(delta), std::min(2.0f, Length(delta))));
                                RecordUpgradeDisplacement(SkillKind::Trap, 5, before,
                                                          enemy.position);
                            }
                        }
                    }
                    if (HasUpgrade(area.upgrade_mask, 7))
                    {
                        target->marked_by_skill = SkillKind::Trap;
                        target->marked_damage_coefficient = 4.0f;
                        target->mark_expires = tick + Seconds(8.0f);
                        EmitVfx("particle.common.mark_apply", target->position,
                                {}, 1.0f, target->boss ? 1.4f : 0.75f);
                        RecordUpgradeEffect(SkillKind::Trap, 6,
                                            UpgradeEffectMetric::ExplosionsCreated);
                        DealDamage(target->id.value,
                                   RoundDamage(EffectiveAttack() * 3.0f),
                                   SkillKind::Trap, EffectOrigin::Derived,
                                   area.cast_id, 0, false, 0.0f, 0, 6);
                        RecordUpgradeEffect(SkillKind::Trap, 6,
                                            UpgradeEffectMetric::MarksApplied);
                    }
                    const auto damage_source = area.trigger_count > 0 &&
                                                       HasUpgrade(area.upgrade_mask, 3)
                                                   ? std::uint8_t{2}
                                                   : area.source_upgrade;
                    DamageArea(area.position, area.effect_radius, area.damage, area.skill,
                               area.origin, area.cast_id,
                               HasUpgrade(area.upgrade_mask, 4) ? 3 : 0,
                               HasUpgrade(area.upgrade_mask, 5),
                               HasUpgrade(area.upgrade_mask, 6) ? 0.6f : area.slow_reduction,
                               HasUpgrade(area.upgrade_mask, 6) ? Seconds(2.0f)
                                                                : area.slow_duration,
                               damage_source, area.source_relic);
                    if (HasUpgrade(area.upgrade_mask, 5))
                    {
                        pending_areas.push_back(
                            {tick, ScheduledKind::Area, area.skill, target->position, {},
                             0.7f, 1.5f, 2.0f, 1, 0,
                             EffectOrigin::Derived, area.cast_id, 4});
                    }
                    ++area.trigger_count;
                    if (HasUpgrade(area.upgrade_mask, 3) && area.trigger_count == 1)
                    {
                        area.next_tick = tick + 120;
                    }
                    else
                    {
                        area.dead = true;
                    }
                    EmitVfx("particle.skill.trap.trigger", target->position);
                }
                continue;
            }
            else if (area.kind == AreaKind::Slow)
            {
                EmitVfx("particle.status.slow_area", area.position, {},
                        area.radius, 0.03f);
                for (auto &enemy : enemies)
                {
                    if (!enemy.dead &&
                        DistanceSquared(area.position, enemy.position) <= area.radius * area.radius)
                    {
                        enemy.status.slows.push_back(
                            {area.slow_reduction, tick + area.slow_duration,
                             area.skill, area.source_upgrade, area.source_relic});
                        EmitVfx("particle.status.slow_apply", enemy.position, {},
                                enemy.boss ? 1.4f : 0.75f, 0.12f);
                        RecordUpgradeEffect(area.skill, area.source_upgrade,
                                            UpgradeEffectMetric::SlowApplications);
                        RecordUpgradeEffect(area.skill, area.source_upgrade,
                                            UpgradeEffectMetric::SlowTargetTicks,
                                            area.slow_duration);
                    }
                }
            }
            else
            {
                if (area.skill == SkillKind::ArrowRain &&
                    area.origin == EffectOrigin::Original)
                {
                    EmitVfx(area.trigger_count == 0
                                ? "particle.skill.arrow_rain.impact"
                                : "particle.skill.arrow_rain.area_pulse",
                            area.position, {}, area.radius / 4.0f, 0.12f);
                    if (area.trigger_count == 0 && HasUpgrade(area.upgrade_mask, 2))
                        EmitVfx("particle.common.pull", area.position, {},
                                area.radius / 4.0f, 0.12f);
                    if (HasUpgrade(area.upgrade_mask, 8) && area.trigger_count >= 6)
                        RecordUpgradeEffect(area.skill, 7,
                                            UpgradeEffectMetric::DurationTicksAdded,
                                            area.interval);
                    auto &runtime = RuntimeForCast(area.cast_id, area.skill);
                    for (auto &enemy : enemies)
                    {
                        if (enemy.dead || DistanceSquared(area.position, enemy.position) >
                                              area.radius * area.radius)
                        {
                            continue;
                        }
                        if (area.trigger_count == 0 &&
                            HasUpgrade(area.upgrade_mask, 2) && !enemy.boss)
                        {
                            const auto delta = Subtract(area.position, enemy.position);
                            const auto before = enemy.position;
                            enemy.position = Add(enemy.position, Multiply(
                                Normalize(delta), std::min(2.0f, Length(delta))));
                            RecordUpgradeDisplacement(SkillKind::ArrowRain, 1, before,
                                                      enemy.position);
                        }
                        const auto hit_count = IncrementAreaHit(area.id.value,
                                                                enemy.id.value);
                        if (HasUpgrade(area.upgrade_mask, 4) && !enemy.status.burn)
                            ApplyBurn(enemy, EffectiveAttack(), false, Seconds(4.0f),
                                      area.skill, 3);
                        const auto damage_source = HasUpgrade(area.upgrade_mask, 8) &&
                                                           area.trigger_count >= 6
                                                       ? std::uint8_t{7}
                                                       : area.source_upgrade;
                        DealDamage(enemy.id.value, area.damage, area.skill, area.origin,
                                   area.cast_id,
                                    HasUpgrade(area.upgrade_mask, 3) && hit_count % 2 == 0 ? 3 : 0,
                                   false, area.slow_reduction, area.slow_duration,
                                   damage_source, area.source_relic);
                        if (HasUpgrade(area.upgrade_mask, 4) && enemy.status.burn &&
                            runtime.spawn_count < 4)
                        {
                            pending_areas.push_back(
                                {tick, ScheduledKind::Area, area.skill, enemy.position, {},
                                  0.3f, 1.5f, 2.0f, 1, 0,
                                  EffectOrigin::Derived, area.cast_id, 3});
                            ++runtime.spawn_count;
                        }
                    }
                    if (HasUpgrade(area.upgrade_mask, 5))
                    {
                        if (auto *target = NearestEnemy(area.position, 7.0f))
                        {
                            if (auto *arrow = FireProjectile(
                                    area.skill, area.position,
                                    Normalize(Subtract(target->position, area.position)),
                                    4.0f, EffectOrigin::Derived, area.cast_id, 0, true, 4))
                            {
                                arrow->homing = true;
                                arrow->homing_target = target->id.value;
                            }
                        }
                    }
                }
                else
                {
                    if (area.half_length <= 0.0f)
                    {
                        const auto fire_area = area.applies_burn ||
                            (area.skill == SkillKind::ExplosiveArrow &&
                             area.source_upgrade == 3) ||
                            (area.skill == SkillKind::Trap &&
                             area.source_upgrade == 4) ||
                            (area.skill == SkillKind::ArrowRain &&
                             area.source_upgrade == 3);
                        EmitVfx(fire_area ? "particle.skill.fire_area.pulse"
                                          : "particle.skill.damage_area.pulse",
                                area.position, {}, area.radius / 1.5f, 0.12f);
                    }
                    if (area.half_length > 0.0f)
                    {
                        const auto from = Subtract(
                            area.position, Multiply(area.direction, area.half_length));
                        const auto to = Add(
                            area.position, Multiply(area.direction, area.half_length));
                        for (const auto &enemy : enemies)
                        {
                            if (enemy.dead) continue;
                            const auto enemy_radius = enemy.boss ? 1.1f : 0.45f;
                            if (SegmentCircle(from, to, enemy.position,
                                              area.radius + enemy_radius))
                            {
                                DealDamage(enemy.id.value, area.damage, area.skill,
                                           area.origin, area.cast_id, 0,
                                           area.applies_burn, area.slow_reduction,
                                           area.slow_duration, area.source_upgrade,
                                           area.source_relic);
                            }
                        }
                    }
                    else
                    {
                        DamageArea(area.position, area.radius, area.damage, area.skill,
                                   area.origin, area.cast_id, 0, area.applies_burn,
                                   area.slow_reduction, area.slow_duration,
                                   area.source_upgrade, area.source_relic);
                    }
                }
            }
            ++area.trigger_count;
            area.next_tick += area.interval;
            if (area.next_tick >= area.expires)
            {
                area.dead = true;
                if (area.skill == SkillKind::ArrowRain &&
                    area.origin == EffectOrigin::Original &&
                    HasUpgrade(area.upgrade_mask, 8))
                {
                    SpawnArea(AreaKind::Slow, area.skill, area.position, area.radius,
                              0.0f, 3.0f, 0.0f, EffectOrigin::Derived,
                              area.cast_id, 0, 0.4f, 1.0f, 0.0f, false, 7);
                }
            }
        }
        for (const auto &pending : pending_areas)
        {
            SpawnArea(AreaKind::Damage, pending.skill, pending.position,
                      pending.radius, pending.damage_coefficient, pending.duration,
                      0.0f, pending.origin, pending.cast_id, pending.upgrade_mask,
                      0.0f, 0.0f, 0.0f, false,
                      pending.source_upgrade, pending.source_relic);
        }
    }

    void ApplyBleed(EnemyActor &enemy, float attack, std::uint8_t stacks,
                    SkillKind source_skill = SkillKind::BasicAttack,
                    std::uint8_t source_upgrade = kNoTelemetrySource,
                    std::uint8_t source_relic = kNoTelemetrySource)
    {
        if (stacks != 0) EmitVfx("particle.status.bleed_apply", enemy.position);
        RecordUpgradeEffect(source_skill, source_upgrade,
                            UpgradeEffectMetric::BleedStacksApplied, stacks);
        for (std::uint8_t stack = 0; stack < stacks; ++stack)
        {
            BleedEffect effect{attack, tick + kStatusTickInterval * kStatusTickCount + 1,
                               tick + Seconds(0.1f), source_skill,
                               source_upgrade, source_relic};
            if (enemy.status.bleed_count < enemy.status.bleeds.size())
            {
                enemy.status.bleeds[enemy.status.bleed_count++] = effect;
            }
            else
            {
                const auto oldest = std::ranges::min_element(
                    enemy.status.bleeds, {}, &BleedEffect::expires);
                *oldest = effect;
            }
        }
    }

    void ApplyBurn(EnemyActor &enemy, float attack, bool propagated = false,
                   Tick duration = 240,
                   SkillKind source_skill = SkillKind::BasicAttack,
                   std::uint8_t source_upgrade = kNoTelemetrySource,
                   std::uint8_t source_relic = kNoTelemetrySource)
    {
        EmitVfx("particle.status.burn_apply", enemy.position);
        RecordUpgradeEffect(source_skill, source_upgrade,
                            UpgradeEffectMetric::BurnApplications);
        const BurnEffect incoming{attack, tick + duration + 1, tick + Seconds(0.1f),
                                  source_skill, propagated, source_upgrade, source_relic};
        if (!enemy.status.burn || incoming.attack_snapshot > enemy.status.burn->attack_snapshot)
        {
            enemy.status.burn = incoming;
        }
        else
        {
            enemy.status.burn->expires = tick + duration + 1;
        }
    }

    void DamageStatusPhase()
    {
        for (auto &enemy : enemies)
        {
            if (enemy.dead) continue;
            for (std::size_t index = 0; index < enemy.status.bleed_count;)
            {
                auto &bleed = enemy.status.bleeds[index];
                if (bleed.expires <= tick)
                {
                    enemy.status.bleeds[index] =
                        enemy.status.bleeds[--enemy.status.bleed_count];
                    continue;
                }
                RecordUpgradeEffect(bleed.source_skill, bleed.source_upgrade,
                                    UpgradeEffectMetric::BleedActiveTicks);
                if (bleed.next_tick <= tick)
                {
                    EmitVfx("particle.status.bleed_tick", enemy.position);
                    DealDamage(enemy.id.value,
                               RoundDamage(bleed.attack_snapshot * kBleedTickCoefficient),
                               bleed.source_skill, EffectOrigin::DamageOverTime, 0,
                               0, false, 0.0f, 0,
                               bleed.source_upgrade, bleed.source_relic);
                    bleed.next_tick += kStatusTickInterval;
                }
                ++index;
            }
            if (enemy.status.burn)
            {
                if (enemy.status.burn->expires <= tick)
                {
                    enemy.status.burn.reset();
                }
                else if (enemy.status.burn->next_tick <= tick)
                {
                    EmitVfx("particle.status.burn_tick", enemy.position);
                    DealDamage(enemy.id.value,
                               RoundDamage(enemy.status.burn->attack_snapshot *
                                           kBurnTickCoefficient),
                               enemy.status.burn->source_skill,
                               EffectOrigin::DamageOverTime, 0, 0, false, 0.0f, 0,
                               enemy.status.burn->source_upgrade,
                               enemy.status.burn->source_relic);
                    enemy.status.burn->next_tick += kStatusTickInterval;
                }
                if (enemy.status.burn)
                    RecordUpgradeEffect(enemy.status.burn->source_skill,
                                        enemy.status.burn->source_upgrade,
                                        UpgradeEffectMetric::BurnActiveTicks);
            }
        }

        std::ranges::sort(damage_events, [](const DamageEvent &left,
                                            const DamageEvent &right) {
            return std::tie(left.target, left.sequence) <
                   std::tie(right.target, right.sequence);
        });
        for (std::size_t event_index = 0; event_index < damage_events.size(); ++event_index)
        {
            const auto event = damage_events[event_index];
            if (event.target == 0)
            {
                auto source_enemy = event.source_enemy;
                if (source_enemy == kNoTelemetrySource)
                {
                    if (const auto *source = FindEnemy(event.cast_id))
                        source_enemy = static_cast<std::uint8_t>(EnemyTelemetryIndex(*source));
                }
                const auto applied_damage = config.stationary_combat_simulation
                    ? static_cast<std::uint64_t>(event.amount)
                    : static_cast<std::uint64_t>(
                          std::min(event.amount, std::max(player.health, 0)));
                if (!config.stationary_combat_simulation)
                    player.health -= event.amount;
                damage_taken += event.amount;
                EmitVfx("particle.common.player_hit", player.position, {},
                        1.0f, 0.1f);
                if (source_enemy < kEnemyArchetypeCount)
                {
                    const auto hit_key = (static_cast<std::uint64_t>(source_enemy) << 56) |
                                         (event.cast_id & 0x00FFFFFFFFFFFFFFull);
                    if (std::ranges::find(enemy_hit_casts, hit_key) == enemy_hit_casts.end())
                    {
                        enemy_hit_casts.push_back(hit_key);
                        ++balance.enemy_hits[source_enemy];
                    }
                    balance.enemy_damage[source_enemy] += applied_damage;
                }
                if (HasRelic(player.relic_mask, RelicKind::DamageKnockback) &&
                    tick >= player.damage_relic_ready)
                {
                    const auto &relic = data.relics.damage_knockback;
                    EmitVfx("particle.relic.damage_push", player.position,
                            {}, 1.0f, 0.15f);
                    std::uint64_t affected{};
                    for (auto &enemy : enemies)
                    {
                        if (!enemy.dead && !enemy.boss &&
                            DistanceSquared(player.position, enemy.position) <=
                                relic.radius * relic.radius)
                        {
                            const auto before = enemy.position;
                            const auto direction = Normalize(Subtract(enemy.position,
                                                                      player.position));
                            enemy.position = Add(enemy.position,
                                                 Multiply(direction, relic.push_distance));
                            enemy.status.slows.push_back(
                                {relic.slow_fraction,
                                 tick + Seconds(relic.slow_duration_seconds)});
                            RecordRelicEffect(
                                RelicKind::DamageKnockback,
                                UpgradeEffectMetric::DisplacementMillimetres,
                                static_cast<std::uint64_t>(std::llround(
                                    Length(Subtract(enemy.position, before)) * 1000.0f)));
                            ++affected;
                        }
                    }
                    RecordRelicEffect(RelicKind::DamageKnockback,
                                      UpgradeEffectMetric::Activations);
                    RecordRelicEffect(RelicKind::DamageKnockback,
                                      UpgradeEffectMetric::SlowApplications, affected);
                    RecordRelicEffect(RelicKind::DamageKnockback,
                                      UpgradeEffectMetric::SlowTargetTicks,
                                      affected * Seconds(relic.slow_duration_seconds));
                    player.damage_relic_ready = tick + Seconds(relic.cooldown_seconds);
                }
                continue;
            }
            auto *enemy = FindEnemy(event.target);
            if (!enemy || enemy->dead || tick < enemy->invulnerable_until)
            {
                continue;
            }
            const auto applied_damage = std::min(event.amount, std::max(enemy->health, 0));
            enemy->health -= event.amount;
            damage_dealt += applied_damage;
            if (event.skill < SkillKind::Count)
            {
                const auto skill_index = static_cast<std::size_t>(event.skill);
                damage_by_skill[skill_index] += applied_damage;
                if (enemy->boss)
                    balance.skill_boss_damage[skill_index] += applied_damage;
                ++balance.skill_hit_events[skill_index];
                if (event.cast_id != 0)
                {
                    auto &runtime = RuntimeForCast(event.cast_id, event.skill);
                    if (!runtime.telemetry_hit)
                    {
                        runtime.telemetry_hit = true;
                        ++balance.skill_casts_with_hit[skill_index];
                    }
                }
                if (event.source_upgrade < kUpgradeCount)
                {
                    balance.upgrade_damage[skill_index][event.source_upgrade] += applied_damage;
                    ++balance.upgrade_triggers[skill_index][event.source_upgrade];
                }
                for (std::size_t index = 0; index < event.amplified_count; ++index)
                {
                    const auto attributed = static_cast<std::uint64_t>(std::llround(
                        static_cast<double>(applied_damage) *
                        event.amplified_damage[index] /
                        std::max(1, event.amount)));
                    RecordUpgradeDamage(event.skill,
                                        event.amplified_upgrades[index], attributed);
                }
            }
            if (event.source_relic < kRelicCount)
            {
                balance.relic_damage[event.source_relic] += applied_damage;
                ++balance.relic_triggers[event.source_relic];
            }
            if (event.skill < SkillKind::Count && event.source_upgrade < kUpgradeCount &&
                event.source_relic < kRelicCount)
            {
                const auto relic = static_cast<RelicKind>(event.source_relic);
                RecordUpgradeRelicSynergy(
                    event.skill, event.source_upgrade, relic,
                    UpgradeRelicSynergyMetric::Damage, applied_damage);
                RecordUpgradeRelicSynergy(
                    event.skill, event.source_upgrade, relic,
                    UpgradeRelicSynergyMetric::DamageEvents);
            }
            if (event.origin == EffectOrigin::Original) balance.direct_damage += applied_damage;
            else if (event.origin == EffectOrigin::Derived) balance.derived_damage += applied_damage;
            else balance.damage_over_time += applied_damage;
            const auto attack_points =
                player.stats[static_cast<std::size_t>(StatKind::AttackPower)];
            if (attack_points != 0)
            {
                const auto multiplier = 1.0 + 0.06 * attack_points;
                const auto without_stat = static_cast<std::uint64_t>(
                    std::llround(static_cast<double>(applied_damage) / multiplier));
                balance.stat_utility[static_cast<std::size_t>(StatKind::AttackPower)] +=
                    static_cast<std::uint64_t>(applied_damage) -
                    std::min(static_cast<std::uint64_t>(applied_damage), without_stat);
            }
            enemy->last_damage_skill = event.skill;
            enemy->last_damage_origin = event.origin;
            enemy->last_damage_cast = event.cast_id;
            enemy->last_damage_upgrade = event.source_upgrade;
            if (event.skill < SkillKind::Count &&
                event.origin == EffectOrigin::Original &&
                HasRelic(player.relic_mask, RelicKind::CombatHitChain))
            {
                const auto &relic = data.relics.combat_hit_chain;
                ++player.combat_hit_progress;
                if (player.combat_hit_progress >= relic.direct_hits_per_trigger)
                {
                    player.combat_hit_progress -= relic.direct_hits_per_trigger;
                    RecordRelicEffect(RelicKind::CombatHitChain,
                                      UpgradeEffectMetric::Activations);
                    std::vector<std::uint64_t> selected;
                    selected.reserve(relic.maximum_targets);
                    while (selected.size() < relic.maximum_targets)
                    {
                        EnemyActor *target{};
                        auto best_distance = relic.search_radius * relic.search_radius;
                        for (auto &candidate : enemies)
                        {
                            if (candidate.dead || candidate.health <= 0) continue;
                            bool already_selected{};
                            for (const auto id : selected)
                                already_selected |= id == candidate.id.value;
                            const auto distance =
                                DistanceSquared(player.position, candidate.position);
                            if (!already_selected && distance <= best_distance)
                            {
                                target = &candidate;
                                best_distance = distance;
                            }
                        }
                        if (!target) break;
                        selected.push_back(target->id.value);
                        DealDamage(target->id.value,
                                   RoundDamage(EffectiveAttack() * relic.damage_multiplier),
                                   SkillKind::Count, EffectOrigin::Derived,
                                   event.cast_id, 0, false, 0.0f, 0,
                                   kNoTelemetrySource,
                                   static_cast<std::uint8_t>(RelicKind::CombatHitChain));
                        EmitVfx("particle.relic.combat_chain", target->position,
                                {}, 0.5f, 0.15f);
                    }
                    RecordRelicEffect(RelicKind::CombatHitChain,
                                      UpgradeEffectMetric::ExtraTargetsHit,
                                      selected.size());
                }
            }
            if (event.bleed_stacks)
            {
                auto source_upgrade = event.source_upgrade;
                if (event.skill == SkillKind::BasicAttack) source_upgrade = 3;
                else if (event.skill == SkillKind::PiercingShot) source_upgrade = 2;
                else if (event.skill == SkillKind::MultiShot) source_upgrade = 4;
                else if (event.skill == SkillKind::ChargedShot) source_upgrade = 4;
                else if (event.skill == SkillKind::Trap) source_upgrade = 3;
                else if (event.skill == SkillKind::ArrowRain) source_upgrade = 2;
                ApplyBleed(*enemy, EffectiveAttack(), event.bleed_stacks, event.skill,
                           source_upgrade, event.source_relic);
            }
            if (event.burn)
            {
                const auto had_bleed = enemy->status.bleed_count != 0;
                auto source_upgrade = event.source_upgrade;
                if (event.skill == SkillKind::BasicAttack) source_upgrade = 4;
                else if (event.skill == SkillKind::ExplosiveArrow) source_upgrade = 3;
                else if (event.skill == SkillKind::Trap) source_upgrade = 4;
                else if (event.skill == SkillKind::ArrowRain) source_upgrade = 3;
                ApplyBurn(*enemy, EffectiveAttack(), false, 240, event.skill,
                          source_upgrade, event.source_relic);
                if (had_bleed && HasRelic(player.relic_mask,
                                          RelicKind::BleedBurnExplosion) &&
                    event.origin == EffectOrigin::Original &&
                    tick >= enemy->bleed_burn_ready)
                {
                    const auto &relic = data.relics.bleed_burn_explosion;
                    enemy->bleed_burn_ready =
                        tick + Seconds(relic.per_target_cooldown_seconds);
                    DamageArea(enemy->position, relic.radius,
                               RoundDamage(EffectiveAttack() * relic.damage_multiplier),
                               event.skill,
                               EffectOrigin::Derived, event.cast_id, 0, false, 0.0f, 0,
                               source_upgrade,
                               static_cast<std::uint8_t>(RelicKind::BleedBurnExplosion));
                    EmitVfx("particle.relic.bleed_burn_explosion",
                            enemy->position, {}, 1.0f, 0.15f);
                    RecordUpgradeRelicSynergy(
                        event.skill, source_upgrade, RelicKind::BleedBurnExplosion,
                        UpgradeRelicSynergyMetric::Activations);
                    std::array<std::array<bool, kUpgradeCount>, kCombatSkillCount>
                        recorded_bleed_sources{};
                    for (std::size_t bleed = 0;
                         bleed < enemy->status.bleed_count; ++bleed)
                    {
                        const auto &source = enemy->status.bleeds[bleed];
                        if (source.source_skill >= SkillKind::Count ||
                            source.source_upgrade >= kUpgradeCount ||
                            std::exchange(
                                recorded_bleed_sources[static_cast<std::size_t>(
                                    source.source_skill)][source.source_upgrade], true))
                            continue;
                        RecordUpgradeRelicSynergy(
                            source.source_skill, source.source_upgrade,
                            RelicKind::BleedBurnExplosion,
                            UpgradeRelicSynergyMetric::Activations);
                    }
                }
            }
            if (event.slow_reduction > 0.0f)
            {
                auto source_upgrade = event.source_upgrade;
                if (event.skill == SkillKind::ArrowRain) source_upgrade = 5;
                else if (event.skill == SkillKind::Trap) source_upgrade = 5;
                enemy->status.slows.push_back(
                    {event.slow_reduction, tick + event.slow_duration,
                     event.skill, source_upgrade, event.source_relic});
                EmitVfx("particle.status.slow_apply", enemy->position, {},
                        1.0f, enemy->boss ? 1.4f : 0.75f);
                RecordUpgradeEffect(event.skill, source_upgrade,
                                    UpgradeEffectMetric::SlowApplications);
                RecordUpgradeEffect(event.skill, source_upgrade,
                                    UpgradeEffectMetric::SlowTargetTicks,
                                    event.slow_duration);
            }
            const auto original_player_hit = event.skill < SkillKind::Count &&
                                             event.origin == EffectOrigin::Original;
            const auto active_hit = event.skill > SkillKind::BasicAttack &&
                                    event.skill < SkillKind::Count &&
                                    event.origin == EffectOrigin::Original;
            if (original_player_hit && enemy->marked_by_skill != SkillKind::Count &&
                enemy->marked_by_skill != event.skill && tick <= enemy->mark_expires)
            {
                const auto coefficient = enemy->marked_damage_coefficient;
                const auto marked_skill = enemy->marked_by_skill;
                enemy->marked_by_skill = SkillKind::Count;
                enemy->mark_expires = 0;
                EmitVfx("particle.common.mark_trigger", enemy->position, {},
                        1.0f, enemy->boss ? 1.4f : 0.75f);
                RecordUpgradeEffect(marked_skill, 6,
                                    UpgradeEffectMetric::ExplosionsCreated);
                DamageArea(enemy->position, 2.0f,
                           RoundDamage(EffectiveAttack() * coefficient), marked_skill,
                           EffectOrigin::Derived, event.cast_id, 0, false, 0.0f, 0,
                           6);
            }
            if (active_hit && HasRelic(player.relic_mask,
                                       RelicKind::DifferentSkillTracker))
            {
                const auto &relic = data.relics.different_skill_tracker;
                if (enemy->last_active_hit != SkillKind::Count &&
                    enemy->last_active_hit != event.skill &&
                    tick - enemy->last_active_hit_tick <= Seconds(relic.window_seconds) &&
                    tick >= enemy->different_skill_ready)
                {
                    EmitVfxLine("particle.line.relic_chain", player.position,
                                enemy->position);
                    if (auto *arrow = FireProjectile(
                            event.skill, player.position,
                             Normalize(Subtract(enemy->position, player.position)),
                             relic.damage_multiplier, EffectOrigin::Derived,
                             event.cast_id, 0, true,
                             kNoTelemetrySource,
                             static_cast<std::uint8_t>(RelicKind::DifferentSkillTracker)))
                    {
                        arrow->homing = true;
                        arrow->homing_target = enemy->id.value;
                        RecordRelicEffect(RelicKind::DifferentSkillTracker,
                                          UpgradeEffectMetric::Activations);
                        RecordRelicEffect(RelicKind::DifferentSkillTracker,
                                          UpgradeEffectMetric::ProjectilesCreated);
                    }
                    enemy->different_skill_ready =
                        tick + Seconds(relic.per_target_cooldown_seconds);
                }
                enemy->last_active_hit = event.skill;
                enemy->last_active_hit_tick = tick;
            }
        }
        damage_events.clear();
    }

    std::uint64_t Heal(std::int32_t amount)
    {
        const auto before = player.health;
        player.health = std::min(player.max_health, player.health + amount);
        const auto applied = static_cast<std::uint64_t>(
            std::max(0, player.health - before));
        healing += applied;
        if (applied != 0) EmitVfx("particle.common.heal", player.position);
        return applied;
    }

    void CancelChargedShot() noexcept
    {
        player.charging = false;
        player.charging_skill = SkillKind::Count;
        player.charging_slot = 0xFF;
    }

    std::uint32_t EnemyExperience(const EnemyActor &enemy) const noexcept
    {
        if (enemy.boss)
        {
            if (*enemy.boss == BossKind::FiveMinute) return 100;
            if (*enemy.boss == BossKind::TenMinute) return 200;
            return 0;
        }
        return enemy.kind == EnemyKind::Melee ? 1u : 2u;
    }

    void HandleEnemyDeath(EnemyActor &enemy)
    {
        EmitVfx("particle.common.enemy_death", enemy.position,
                {0.0f, 1.0f}, enemy.boss ? 1.5f : 1.0f);
        ++kills;
        if (HasRelic(player.relic_mask, RelicKind::KillCooldownSurge))
        {
            const auto &relic = data.relics.kill_cooldown_surge;
            ++player.kill_cooldown_progress;
            if (player.kill_cooldown_progress >= relic.kills_per_trigger)
            {
                player.kill_cooldown_progress -= relic.kills_per_trigger;
                std::uint64_t saved{};
                for (auto &cooldown : player.cooldowns)
                {
                    const auto before = cooldown;
                    cooldown -= std::min<Tick>(
                        cooldown, Seconds(relic.cooldown_reduction_seconds));
                    saved += before - cooldown;
                }
                RecordRelicEffect(RelicKind::KillCooldownSurge,
                                  UpgradeEffectMetric::Activations);
                RecordRelicEffect(RelicKind::KillCooldownSurge,
                                  UpgradeEffectMetric::CooldownTicksSaved, saved);
            }
        }
        const auto enemy_index = EnemyTelemetryIndex(enemy);
        ++balance.enemy_killed[enemy_index];
        balance.enemy_lifetime_ticks[enemy_index] += tick - enemy.spawned_tick;
        if (enemy.last_damage_skill < SkillKind::Count)
            ++balance.skill_kills[static_cast<std::size_t>(enemy.last_damage_skill)];
        RecordUpgradeEffect(enemy.last_damage_skill, enemy.last_damage_upgrade,
                            UpgradeEffectMetric::Kills);
        CastRuntime *runtime{};
        if (enemy.last_damage_cast != 0)
        {
            const auto iterator = std::ranges::find(cast_runtime,
                                                    enemy.last_damage_cast,
                                                    &CastRuntime::cast_id);
            if (iterator != cast_runtime.end()) runtime = &*iterator;
        }
        const auto xp = EnemyExperience(enemy);
        if (xp)
        {
            ++balance.pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Experience)];
            SpawnPickup(PickupKind::Experience, enemy.position, xp);
        }
        if (enemy.boss)
        {
            if (*enemy.boss != BossKind::Final &&
                (!config.stationary_combat_simulation ||
                 config.stationary_progression_simulation))
            {
                ++balance.pickup_drop_attempts[
                    static_cast<std::size_t>(PickupKind::RelicChest)];
                SpawnPickup(PickupKind::RelicChest, enemy.position, 1, true);
                Heal(RoundDamage(player.max_health * 0.25f));
            }
        }
        else
        {
            if (runtime)
            {
                ++runtime->kills;
                if (runtime->skill == SkillKind::ChargedShot && runtime->full_charge &&
                    HasUpgrade(runtime->upgrade_mask, 8) &&
                    runtime->kills >= 3 && !runtime->refund_triggered)
                {
                    auto &cooldown = player.cooldowns[
                        static_cast<std::size_t>(SkillKind::ChargedShot) - 1];
                    const auto before = cooldown;
                    cooldown -= cooldown * 40 / 100;
                    RecordUpgradeEffect(SkillKind::ChargedShot, 7,
                                        UpgradeEffectMetric::CooldownTicksSaved,
                                        before - cooldown);
                    runtime->refund_triggered = true;
                }
                if (runtime->skill == SkillKind::BasicAttack &&
                    enemy.last_damage_origin == EffectOrigin::Original &&
                    HasRelic(player.relic_mask, RelicKind::BasicKillTracker) &&
                    runtime->basic_relic_triggers <
                        data.relics.basic_kill_tracker.maximum_triggers_per_attack)
                {
                    const auto &relic = data.relics.basic_kill_tracker;
                    EnemyActor *target{};
                    auto best = relic.search_radius * relic.search_radius;
                    for (auto &candidate : enemies)
                    {
                        if (candidate.dead || candidate.id.value == enemy.id.value ||
                            std::ranges::any_of(cast_hits, [&](const CastHitRecord &hit) {
                                return hit.cast_id == runtime->cast_id &&
                                       hit.target == candidate.id.value;
                            }))
                        {
                            continue;
                        }
                        const auto distance = DistanceSquared(enemy.position,
                                                              candidate.position);
                        if (distance < best)
                        {
                            target = &candidate;
                            best = distance;
                        }
                    }
                    if (target)
                    {
                        if (auto *arrow = FireProjectile(
                                SkillKind::BasicAttack, enemy.position,
                                 Normalize(Subtract(target->position, enemy.position)),
                                 relic.damage_multiplier, EffectOrigin::Derived,
                                 runtime->cast_id, 0, true,
                                 kNoTelemetrySource,
                                 static_cast<std::uint8_t>(RelicKind::BasicKillTracker)))
                        {
                            arrow->homing = true;
                            arrow->homing_target = target->id.value;
                            ++runtime->basic_relic_triggers;
                            RecordRelicEffect(RelicKind::BasicKillTracker,
                                              UpgradeEffectMetric::Activations);
                            RecordRelicEffect(RelicKind::BasicKillTracker,
                                              UpgradeEffectMetric::ProjectilesCreated);
                        }
                    }
                }
                if (runtime->skill == SkillKind::RicochetArrow &&
                    HasUpgrade(runtime->upgrade_mask, 5) &&
                    enemy.last_damage_origin == EffectOrigin::Original &&
                    runtime->spawn_count < 9)
                {
                    std::array<std::uint64_t, 4> excluded{enemy.id.value};
                    for (std::size_t index = 0; index < 3; ++index)
                    {
                        if (runtime->spawn_count == 9) break;
                        auto *target = NearestEnemy(
                            enemy.position, 12.0f,
                            std::span(excluded.data(), index + 1));
                        if (!target) break;
                        if (auto *arrow = FireProjectile(
                                SkillKind::RicochetArrow, enemy.position,
                                Normalize(Subtract(target->position, enemy.position)),
                                1.0f, EffectOrigin::Derived, runtime->cast_id, 0,
                                true, 4))
                        {
                            arrow->homing = true;
                            arrow->homing_target = target->id.value;
                            excluded[index + 1] = target->id.value;
                            ++runtime->spawn_count;
                        }
                    }
                }
                if (runtime->skill == SkillKind::RicochetArrow &&
                    enemy.last_damage_origin == EffectOrigin::Original &&
                    HasUpgrade(runtime->upgrade_mask, 6) &&
                    !runtime->refund_triggered)
                {
                    if (auto *target = NearestEnemy(enemy.position, 12.0f))
                    {
                        if (auto *arrow = FireProjectile(
                                SkillKind::RicochetArrow, enemy.position,
                                 Normalize(Subtract(target->position, enemy.position)),
                                  2.4f, EffectOrigin::Derived, runtime->cast_id, 0, true, 5))
                        {
                            arrow->homing = true;
                            arrow->homing_target = target->id.value;
                            arrow->bounce_remaining = 2;
                            runtime->refund_triggered = true;
                        }
                    }
                }
                if (runtime->skill == SkillKind::ArrowRain &&
                    HasUpgrade(runtime->upgrade_mask, 7) &&
                    enemy.last_damage_origin == EffectOrigin::Original &&
                    runtime->transfer_count < 6)
                {
                    const auto source = std::ranges::find_if(areas, [&](const AreaActor &area) {
                        return !area.dead && area.cast_id == runtime->cast_id &&
                               area.skill == SkillKind::ArrowRain &&
                               area.origin == EffectOrigin::Original;
                    });
                    if (source != areas.end())
                    {
                        EnemyActor *target{};
                        auto best = std::numeric_limits<float>::max();
                        for (auto &candidate : enemies)
                        {
                            if (candidate.dead ||
                                DistanceSquared(source->position, candidate.position) <=
                                    source->radius * source->radius)
                            {
                                continue;
                            }
                            const auto distance = DistanceSquared(enemy.position,
                                                                  candidate.position);
                            if (distance < best)
                            {
                                target = &candidate;
                                best = distance;
                            }
                        }
                        if (target)
                        {
                            if (auto *arrow = FireProjectile(
                                    SkillKind::ArrowRain, enemy.position,
                                     Normalize(Subtract(target->position, enemy.position)),
                                      2.0f, EffectOrigin::Derived, runtime->cast_id, 0, true, 6))
                            {
                                arrow->homing = true;
                                arrow->homing_target = target->id.value;
                                ++runtime->transfer_count;
                            }
                        }
                    }
                }
                if (runtime->skill == SkillKind::Trap &&
                    HasUpgrade(runtime->upgrade_mask, 8) &&
                    enemy.last_damage_origin == EffectOrigin::Original &&
                    runtime->spawn_count < 3)
                {
                    if (auto *target = NearestEnemy(enemy.position, 120.0f))
                    {
                        SpawnArea(AreaKind::Trap, SkillKind::Trap, target->position,
                                  1.5f, 5.0f, 12.0f, 0.6f,
                                  EffectOrigin::Derived, runtime->cast_id, 0,
                                  0.3f, 3.0f, 2.0f, false, 7);
                        ++runtime->spawn_count;
                    }
                }
            }
            ++normal_chest_kills;
            ++balance.pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Heal)];
            ++balance.pickup_drop_attempts[static_cast<std::size_t>(PickupKind::Magnet)];
            const auto utility_drop = [&](std::uint32_t &misses,
                                          std::uint64_t purpose,
                                          float chance_multiplier) {
                const auto chance = std::min(
                    1.0f, (data.utility_pickup_base_chance +
                           data.utility_pickup_miss_increment * misses) *
                              chance_multiplier);
                const auto dropped = RandomUnit(enemy.random_key, purpose) < chance;
                misses = dropped ? 0 : misses + 1;
                return dropped;
            };
            if (utility_drop(heal_pickup_misses, 0x4845414Cull,
                             data.heal_pickup_chance_multiplier))
            {
                SpawnPickup(PickupKind::Heal, enemy.position, 8);
            }
            if (utility_drop(magnet_pickup_misses, 0x4D41474E4554ull,
                             data.magnet_pickup_chance_multiplier))
            {
                SpawnPickup(PickupKind::Magnet, enemy.position, 1);
            }
            if ((!config.stationary_combat_simulation ||
                 config.stationary_progression_simulation) &&
                player.relic_mask != 0x0FFFu)
            {
                ++balance.pickup_drop_attempts[
                    static_cast<std::size_t>(PickupKind::RelicChest)];
                const auto chance = std::min(
                    1.0f, data.relic_chest_base_chance +
                              data.relic_chest_miss_increment * normal_chest_kills);
                if (RandomUnit(enemy.random_key, 0x4348455354ull) < chance)
                {
                    SpawnPickup(PickupKind::RelicChest, enemy.position, 1);
                    normal_chest_kills = 0;
                }
            }
        }
        if (enemy.status.bleed_count > 0 &&
            HasRelic(player.relic_mask, RelicKind::BleedKillHeal) &&
            tick >= player.bleed_heal_ready)
        {
            const auto &relic = data.relics.bleed_kill_heal;
            const auto healed = Heal(RoundDamage(
                player.max_health * relic.maximum_hp_heal_fraction));
            RecordRelicEffect(RelicKind::BleedKillHeal,
                              UpgradeEffectMetric::Activations);
            RecordRelicEffect(RelicKind::BleedKillHeal,
                              UpgradeEffectMetric::Healing, healed);
            std::array<std::array<bool, kUpgradeCount>, kCombatSkillCount>
                recorded_sources{};
            for (std::size_t bleed = 0; bleed < enemy.status.bleed_count; ++bleed)
            {
                const auto &source = enemy.status.bleeds[bleed];
                if (source.source_skill >= SkillKind::Count ||
                    source.source_upgrade >= kUpgradeCount ||
                    std::exchange(
                        recorded_sources[static_cast<std::size_t>(
                            source.source_skill)][source.source_upgrade], true))
                    continue;
                RecordUpgradeRelicSynergy(
                    source.source_skill, source.source_upgrade,
                    RelicKind::BleedKillHeal,
                    UpgradeRelicSynergyMetric::Healing, healed);
            }
            player.bleed_heal_ready =
                tick + Seconds(relic.internal_cooldown_seconds);
        }
        if (enemy.status.burn && !enemy.status.burn->propagated &&
            HasRelic(player.relic_mask, RelicKind::BurnPropagation))
        {
            const auto &relic = data.relics.burn_propagation;
            std::vector<std::uint64_t> excluded{enemy.id.value};
            excluded.reserve(static_cast<std::size_t>(relic.maximum_targets) + 1);
            for (std::uint32_t index = 0; index < relic.maximum_targets; ++index)
            {
                if (auto *target = NearestEnemy(
                        enemy.position, relic.search_radius, std::span(excluded)))
                {
                    EmitVfxLine("particle.line.burn_transfer", enemy.position,
                                target->position);
                    ApplyBurn(*target,
                              enemy.status.burn->attack_snapshot *
                                  relic.copied_burn_strength,
                              true,
                              enemy.status.burn->expires > tick
                                  ? enemy.status.burn->expires - tick
                                  : 1,
                              enemy.status.burn->source_skill,
                              enemy.status.burn->source_upgrade,
                              static_cast<std::uint8_t>(RelicKind::BurnPropagation));
                    RecordRelicEffect(RelicKind::BurnPropagation,
                                      UpgradeEffectMetric::Activations);
                    RecordRelicEffect(RelicKind::BurnPropagation,
                                      UpgradeEffectMetric::BurnApplications);
                    RecordUpgradeRelicSynergy(
                        enemy.status.burn->source_skill,
                        enemy.status.burn->source_upgrade,
                        RelicKind::BurnPropagation,
                        UpgradeRelicSynergyMetric::BurnApplications);
                    excluded.push_back(target->id.value);
                }
            }
        }
    }

    void DeathDropPhase()
    {
        bool final_dead{};
        for (auto &enemy : enemies)
        {
            if (!enemy.dead && enemy.health <= 0)
            {
                enemy.dead = true;
                HandleEnemyDeath(enemy);
                final_dead |= enemy.boss && *enemy.boss == BossKind::Final;
            }
            if (enemy.boss && *enemy.boss == BossKind::Final && enemy.final_phase == 1 &&
                enemy.health > 0 && enemy.health * 2 <= enemy.max_health)
            {
                enemy.final_phase = 2;
                enemy.invulnerable_until = tick + 90;
                enemy.pattern_ready = enemy.invulnerable_until;
                enemy.dash_until = 0;
                enemy.velocity = {};
                std::erase_if(boss_actions, [&](const BossAction &action) {
                    return action.boss_id == enemy.id.value;
                });
                std::erase_if(projectiles, [](const ProjectileActor &projectile) {
                    return !projectile.player_owned;
                });
                std::erase_if(areas, [](const AreaActor &area) {
                    return area.kind == AreaKind::EnemyDamage;
                });
                EmitVfx("particle.boss.phase_change", enemy.position,
                        {}, 1.0f, 1.25f);
            }
        }
        if (final_dead)
        {
            phase = SessionPhase::Victory;
        }
        else if (player.health <= 0)
        {
            if (HasRelic(player.relic_mask, RelicKind::OnceRevive) &&
                player.revives_used <
                    data.relics.once_revive.maximum_triggers_per_session)
            {
                const auto &relic = data.relics.once_revive;
                ++player.revives_used;
                player.health = std::max(
                    1, RoundDamage(player.max_health * relic.health_fraction));
                player.revive_invulnerable_until =
                    tick + Seconds(relic.invulnerability_seconds);
                RecordRelicEffect(RelicKind::OnceRevive,
                                  UpgradeEffectMetric::Activations);
                RecordRelicEffect(RelicKind::OnceRevive,
                                  UpgradeEffectMetric::Healing,
                                  static_cast<std::uint64_t>(player.health));
                EmitVfx("particle.common.heal", player.position, {}, 1.5f, 0.5f);
            }
            else
            {
                EmitVfx("particle.common.player_death", player.position, {},
                        1.0f, 0.2f);
                phase = SessionPhase::Defeat;
            }
        }
    }

    void CollectPickup(PickupActor &pickup)
    {
        pickup.dead = true;
        ++balance.pickup_collected[static_cast<std::size_t>(pickup.kind)];
        if (pickup.attracted_by_magnet_stat)
            ++balance.stat_utility[static_cast<std::size_t>(StatKind::MagnetRadius)];
        if (pickup.kind != PickupKind::Experience)
        {
            constexpr std::array<std::string_view, kPickupKindCount> effects{
                "", "particle.pickup.heal_collect", "particle.pickup.magnet_collect",
                "particle.pickup.relic_collect"};
            EmitVfx(effects[static_cast<std::size_t>(pickup.kind)], pickup.position);
        }
        if (pickup.kind == PickupKind::Experience)
        {
            player.experience += pickup.value;
        }
        else if (pickup.kind == PickupKind::Heal)
        {
            Heal(RoundDamage(player.max_health * 0.08f));
        }
        else if (pickup.kind == PickupKind::Magnet)
        {
            for (auto &experience : pickups)
                if (!experience.dead && experience.kind == PickupKind::Experience)
                    experience.globally_attracted = true;
        }
        else
        {
            GenerateRelicCards();
            if (config.automatic_choices)
            {
                SelectCard(0);
            }
            else
            {
                phase = SessionPhase::RelicSelection;
                GuardSelectionInput();
            }
        }
    }

    SkillTagMask BuildTags() const
    {
        SkillTagMask tags{};
        for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
        {
            if (player.skill_levels[skill] == 0) continue;
            tags |= SkillTags(static_cast<SkillKind>(skill));
            for (std::uint8_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
                if ((player.upgrades[skill] & (1u << upgrade)) != 0)
                    tags |= UpgradeTags(static_cast<SkillKind>(skill), upgrade);
        }
        return tags;
    }

    void WeightedShuffle(std::vector<CardView> &candidates,
                         std::uint64_t sequence, std::uint64_t purpose)
    {
        const auto build_tags = BuildTags();
        const auto weight = [&](const CardView &card) {
            const auto tags = card.kind == CardKind::LearnSkill
                                  ? SkillTags(static_cast<SkillKind>(card.subject))
                                  : UpgradeTags(static_cast<SkillKind>(card.subject),
                                                card.upgrade);
            return 1u + std::popcount(static_cast<unsigned>(tags & build_tags));
        };
        for (std::size_t first = 0; first + 1 < candidates.size(); ++first)
        {
            std::uint32_t total{};
            for (std::size_t index = first; index < candidates.size(); ++index)
                total += weight(candidates[index]);
            auto roll = static_cast<std::uint32_t>(
                Random(player.level ^ Mix(sequence), purpose + first) % total);
            auto selected = first;
            while (roll >= weight(candidates[selected]))
                roll -= weight(candidates[selected++]);
            std::swap(candidates[first], candidates[selected]);
        }
    }

    void GenerateLevelCards(bool exclude_current = false)
    {
        const auto previous_cards = cards;
        const auto previous_count = card_count;
        std::vector<CardView> candidates;
        for (std::size_t index = 1; index < kCombatSkillCount; ++index)
        {
            if (player.skill_levels[index] == 0 && player.loadout.back() == SkillKind::Count)
            {
                candidates.push_back({CardKind::LearnSkill,
                                      static_cast<std::uint8_t>(index), 0});
            }
        }
        for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
        {
            if (player.skill_levels[skill] == 0 || std::popcount(player.upgrades[skill]) >= 4)
            {
                continue;
            }
            for (std::uint8_t upgrade = 0; upgrade < 8; ++upgrade)
            {
                if ((player.upgrades[skill] & (1u << upgrade)) == 0)
                {
                    candidates.push_back({skill == 0 ? CardKind::BasicUpgrade
                                                     : CardKind::SkillUpgrade,
                                          static_cast<std::uint8_t>(skill), upgrade});
                }
            }
        }
        if (exclude_current && candidates.size() > cards.size())
        {
            std::erase_if(candidates, [&](const CardView &candidate) {
                return std::ranges::any_of(
                    std::span(previous_cards.data(), previous_count),
                    [&](const CardView &previous) {
                        return std::tie(candidate.kind, candidate.subject, candidate.upgrade) ==
                               std::tie(previous.kind, previous.subject, previous.upgrade);
                    });
            });
        }
        std::ranges::sort(candidates, [](const CardView &left, const CardView &right) {
            return std::tie(left.kind, left.subject, left.upgrade) <
                   std::tie(right.kind, right.subject, right.upgrade);
        });
        WeightedShuffle(candidates, level_reroll_sequence, 0x43415244ull);
        card_count = 0;
        if (player.loadout.back() == SkillKind::Count)
        {
            const auto guaranteed = std::ranges::find(candidates, CardKind::LearnSkill,
                                                       &CardView::kind);
            if (guaranteed != candidates.end())
            {
                cards[card_count++] = *guaranteed;
                candidates.erase(guaranteed);
            }
        }
        for (const auto candidate : candidates)
        {
            if (card_count == cards.size()) break;
            cards[card_count++] = candidate;
        }
        while (card_count < cards.size())
        {
            cards[card_count++] = {CardKind::BonusStatPoint, 0, 0};
        }
    }

    void GuardSelectionInput()
    {
        selection_input_guard_frames = 12;
        selection_waiting_for_release = input.held.basic_attack_held;
    }

    void GenerateRelicCards(bool exclude_current = false)
    {
        const auto previous_cards = cards;
        const auto previous_count = card_count;
        std::vector<CardView> candidates;
        const auto build_tags = BuildTags();
        for (std::uint8_t relic = 0; relic < kRelicCount; ++relic)
        {
            const auto prerequisite = RelicPrerequisiteTags(
                static_cast<RelicKind>(relic));
            if ((player.relic_mask & (1u << relic)) == 0 &&
                (build_tags & prerequisite) == prerequisite)
            {
                candidates.push_back({CardKind::Relic, relic, 0});
            }
        }
        if (candidates.empty())
        {
            for (std::uint8_t relic = 0; relic < kRelicCount; ++relic)
                if ((player.relic_mask & (1u << relic)) == 0)
                    candidates.push_back({CardKind::Relic, relic, 0});
        }
        if (exclude_current && candidates.size() > cards.size())
        {
            std::erase_if(candidates, [&](const CardView &candidate) {
                return std::ranges::any_of(
                    std::span(previous_cards.data(), previous_count),
                    [&](const CardView &previous) {
                        return candidate.subject == previous.subject;
                    });
            });
        }
        std::ranges::sort(candidates, {}, &CardView::subject);
        for (std::size_t index = candidates.size(); index > 1; --index)
        {
            const auto swap = Random(player.level ^ Mix(relic_reroll_sequence),
                                     0x52454C4943ull + index) % index;
            std::swap(candidates[index - 1], candidates[swap]);
        }
        card_count = static_cast<std::uint8_t>(std::min<std::size_t>(3, candidates.size()));
        for (std::size_t index = 0; index < card_count; ++index)
        {
            cards[index] = candidates[index];
        }
    }

    bool SelectCard(std::size_t index)
    {
        if (index >= card_count)
        {
            return false;
        }
        const auto card = cards[index];
        if (card.kind == CardKind::LearnSkill)
        {
            const auto skill = static_cast<SkillKind>(card.subject);
            player.skill_levels[card.subject] = 1;
            const auto slot = std::ranges::find(player.loadout, SkillKind::Count);
            if (slot != player.loadout.end()) *slot = skill;
        }
        else if (card.kind == CardKind::SkillUpgrade ||
                 card.kind == CardKind::BasicUpgrade)
        {
            player.upgrades[card.subject] |= 1u << card.upgrade;
            player.skill_levels[card.subject] = static_cast<std::uint8_t>(
                1 + std::popcount(player.upgrades[card.subject]));
        }
        else if (card.kind == CardKind::BonusStatPoint)
        {
            ++player.pending_stat_points;
        }
        else
        {
            player.relic_mask |= 1u << card.subject;
            card_count = 0;
            phase = SessionPhase::Playing;
            return true;
        }
        ++player.pending_stat_points;
        card_count = 0;
        phase = SessionPhase::StatAllocation;
        GuardSelectionInput();
        if (config.automatic_choices)
        {
            AssignAutomaticStats();
        }
        return true;
    }

    void AssignStat(StatKind stat)
    {
        const auto index = static_cast<std::size_t>(stat);
        if (player.pending_stat_points == 0 || player.stats[index] >= 10)
        {
            return;
        }
        ++player.stats[index];
        --player.pending_stat_points;
        if (stat == StatKind::MaxHealth)
        {
            player.max_health = RoundDamage(100.0f * (1.0f + 0.08f * player.stats[index]));
            balance.stat_utility[index] += 8;
            Heal(8);
        }
        if (player.pending_stat_points == 0)
        {
            if (player.pending_levels > 0)
            {
                --player.pending_levels;
                GenerateLevelCards();
                phase = SessionPhase::CardSelection;
                GuardSelectionInput();
                if (config.automatic_choices) SelectCard(0);
            }
            else
            {
                phase = SessionPhase::Playing;
            }
        }
    }

    void AssignAutomaticStats()
    {
        while (player.pending_stat_points > 0)
        {
            bool assigned{};
            for (std::size_t offset = 0; offset < kStatCount; ++offset)
            {
                const auto index = (player.level + offset) % kStatCount;
                if (player.stats[index] < 10)
                {
                    AssignStat(static_cast<StatKind>(index));
                    assigned = true;
                    break;
                }
            }
            if (!assigned)
            {
                player.pending_stat_points = 0;
                phase = SessionPhase::Playing;
            }
        }
    }

    void XpCardPhase()
    {
        constexpr float kPickupAttractSpeed = 15.0f;
        for (auto &pickup : pickups)
        {
            if (pickup.dead) continue;
            if (config.stationary_progression_simulation &&
                (pickup.kind == PickupKind::Experience ||
                 pickup.kind == PickupKind::RelicChest))
            {
                pickup.position = player.position;
            }
            const auto delta = Subtract(player.position, pickup.position);
            const auto global_experience_magnet =
                pickup.kind == PickupKind::Experience && pickup.globally_attracted;
            const auto radius = global_experience_magnet
                                    ? data.arena_half_extent * 3.0f
                                    : EffectiveMagnetRadius();
            if (LengthSquared(delta) <= radius * radius)
            {
                if (!global_experience_magnet &&
                    LengthSquared(delta) > data.player_magnet_radius *
                                                   data.player_magnet_radius)
                    pickup.attracted_by_magnet_stat = true;
                pickup.position = Add(pickup.position,
                                      Multiply(Normalize(delta),
                                               kPickupAttractSpeed * kTickSeconds));
            }
            if (DistanceSquared(pickup.position, player.position) <= 0.36f)
            {
                CollectPickup(pickup);
            }
        }
        while ((!config.stationary_combat_simulation ||
                config.stationary_progression_simulation) &&
               player.experience >= ExperienceForLevel(player.level))
        {
            player.experience -= ExperienceForLevel(player.level);
            ++player.level;
            ++player.pending_levels;
        }
        if (player.pending_levels > 0 && phase == SessionPhase::Playing)
        {
            CancelChargedShot();
            --player.pending_levels;
            GenerateLevelCards();
            phase = SessionPhase::CardSelection;
            GuardSelectionInput();
            if (config.automatic_choices) SelectCard(0);
        }
    }

    void StructuralMergePhase()
    {
        const auto destroy = [this](flecs::entity_t entity) {
            if (entity != 0 && world.is_alive(entity)) world.entity(entity).destruct();
        };
        for (const auto &enemy : enemies) if (enemy.dead) destroy(enemy.ecs);
        for (const auto &projectile : projectiles) if (projectile.dead) destroy(projectile.ecs);
        for (const auto &area : areas) if (area.dead) destroy(area.ecs);
        for (const auto &pickup : pickups) if (pickup.dead) destroy(pickup.ecs);
        std::erase_if(enemies, [](const EnemyActor &enemy) { return enemy.dead; });
        std::erase_if(projectiles, [](const ProjectileActor &projectile) { return projectile.dead; });
        std::erase_if(areas, [](const AreaActor &area) { return area.dead; });
        std::erase_if(pickups, [](const PickupActor &pickup) { return pickup.dead; });
        std::erase_if(cast_hits, [this](const CastHitRecord &record) {
            return record.cast_id + 256 < next_cast_id;
        });
        std::erase_if(cast_runtime, [this](const CastRuntime &runtime) {
            return runtime.cast_id + 256 < next_cast_id;
        });
        std::erase_if(area_hits, [this](const AreaHitRecord &record) {
            return std::ranges::none_of(areas, [&](const AreaActor &area) {
                return area.id.value == record.area_id;
            });
        });
    }

    GameplayChecksum CalculateChecksum() const
    {
        GameplayChecksum hash = 14695981039346656037ull;
        const auto value = [&]<typename T>(const T &item) {
            static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
            HashBytes(hash, &item, sizeof(item));
        };
        const auto vector2 = [&](Float2 item) {
            value(item.x);
            value(item.y);
        };
        const auto count = [&](std::size_t size) {
            value(static_cast<std::uint64_t>(size));
        };

        value(config.seed);
        value(config.stationary_combat_simulation);
        value(config.stationary_progression_simulation);
        value(tick);
        value(growth_ticks);
        value(boss_fight_ticks);
        value(phase);
        value(menu_page);
        value(collection_skill_index);
        value(character_skill_index);
        value(character_slot_source);
        value(selection_input_guard_frames);
        value(selection_waiting_for_release);
        value(next_entity_id);
        value(next_enemy_random_key);
        value(next_cast_id);
        value(next_enemy_attack_id);
        value(damage_sequence);
        value(spawn_accumulator);
        value(normal_chest_kills);
        value(heal_pickup_misses);
        value(magnet_pickup_misses);
        value(level_reroll_sequence);
        value(relic_reroll_sequence);
        value(kills);
        value(damage_dealt);
        for (const auto damage : damage_by_skill) value(damage);
        value(damage_taken);
        value(healing);
        const auto hash_array = [&](const auto &items) {
            for (const auto &item : items)
            {
                if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(item)>>)
                    value(item);
                else
                    for (const auto nested : item) value(nested);
            }
        };
        hash_array(balance.skill_uses);
        hash_array(balance.skill_casts_with_hit);
        hash_array(balance.skill_hit_events);
        hash_array(balance.skill_kills);
        hash_array(balance.skill_boss_damage);
        hash_array(balance.upgrade_damage);
        hash_array(balance.upgrade_triggers);
        for (const auto &skill : balance.upgrade_effects)
            for (const auto &upgrade : skill)
                for (const auto metric : upgrade) value(metric);
        value(balance.upgrade_relic_synergy_count);
        for (std::size_t index = 0; index < balance.upgrade_relic_synergy_count;
             ++index)
        {
            const auto &entry = balance.upgrade_relic_synergies[index];
            value(entry.skill);
            value(entry.upgrade);
            value(entry.relic);
            for (const auto metric : entry.metrics) value(metric);
        }
        hash_array(balance.relic_damage);
        hash_array(balance.relic_triggers);
        for (const auto &relic : balance.relic_effects)
            hash_array(relic);
        hash_array(balance.enemy_spawned);
        hash_array(balance.enemy_killed);
        hash_array(balance.enemy_attack_attempts);
        hash_array(balance.enemy_hits);
        hash_array(balance.enemy_damage);
        hash_array(balance.enemy_lifetime_ticks);
        hash_array(balance.pickup_drop_attempts);
        hash_array(balance.pickup_drops);
        hash_array(balance.pickup_collected);
        hash_array(balance.stat_utility);
        value(balance.direct_damage);
        value(balance.derived_damage);
        value(balance.damage_over_time);
        value(final_boss_spawned);

        vector2(player.position);
        vector2(player.previous_position);
        vector2(player.aim);
        vector2(player.facing);
        vector2(player.move_target);
        value(player.has_move_target);
        value(player.health);
        value(player.max_health);
        value(player.attack);
        value(player.attack_speed);
        value(player.move_speed);
        value(player.magnet_radius);
        value(player.level);
        value(player.experience);
        value(player.pending_levels);
        value(player.pending_stat_points);
        value(player.level_rerolls);
        value(player.relic_rerolls);
        for (const auto item : player.stats) value(item);
        for (const auto item : player.loadout) value(item);
        for (const auto item : player.skill_levels) value(item);
        for (const auto item : player.upgrades) value(item);
        for (const auto item : player.cooldowns) value(item);
        value(player.relic_mask);
        value(player.next_basic_attack);
        value(player.basic_sequence);
        value(player.basic_arrow_sequence);
        value(player.active_basic_empower_until);
        value(player.movement_since_echo);
        vector2(player.one_second_ago);
        value(player.last_position_sample);
        value(player.last_active);
        value(player.last_active_tick);
        value(player.alternating_refund_until);
        value(player.alternating_refund_source);
        value(player.damage_relic_ready);
        value(player.bleed_heal_ready);
        value(player.revives_used);
        value(player.revive_invulnerable_until);
        value(player.kill_cooldown_progress);
        value(player.combat_hit_progress);
        value(player.charging);
        value(player.charging_skill);
        value(player.charging_slot);
        value(player.charge_start);
        value(player.retreat_until);
        vector2(player.retreat_velocity);
        value(player.active_cast_tick);
        value(player.active_animation_start);
        value(player.active_animation_until);
        value(player.retreat_followup_tick);
        vector2(player.retreat_followup_direction);
        value(player.retreat_followup_cast);
        value(player.retreat_upgrade_mask);
        value(player.retreat_landing_pending);
        value(player.forced_move_skill);
        value(player.next_active_refund_until);
        value(player.next_active_refund_source);
        value(player.buffered_skill);
        value(player.buffered_skill_expires);

        count(enemies.size());
        for (const auto &enemy : enemies)
        {
            value(enemy.id.value);
            value(enemy.random_key);
            value(enemy.kind);
            value(enemy.boss.has_value());
            if (enemy.boss) value(*enemy.boss);
            vector2(enemy.position);
            vector2(enemy.previous_position);
            vector2(enemy.velocity);
            value(enemy.health);
            value(enemy.max_health);
            value(enemy.damage);
            value(enemy.move_speed);
            value(enemy.attack_range);
            value(enemy.warning_ticks);
            value(enemy.attack_cooldown_ticks);
            value(enemy.next_attack);
            value(enemy.attack_resolve);
            value(enemy.attack_cast_id);
            vector2(enemy.locked_aim);
            value(enemy.spawned_tick);
            value(enemy.pattern_ready);
            value(enemy.last_pattern);
            value(enemy.repeat_count);
            value(enemy.final_phase);
            value(enemy.invulnerable_until);
            value(enemy.dash_until);
            value(enemy.dash_damage);
            value(enemy.dash_hit);
            value(enemy.phase_pattern_count);
            value(enemy.status.bleed_count);
            for (std::size_t index = 0; index < enemy.status.bleed_count; ++index)
            {
                const auto &bleed = enemy.status.bleeds[index];
                value(bleed.attack_snapshot);
                value(bleed.expires);
                value(bleed.next_tick);
                value(bleed.source_skill);
                value(bleed.source_upgrade);
                value(bleed.source_relic);
            }
            value(enemy.status.burn.has_value());
            if (enemy.status.burn)
            {
                value(enemy.status.burn->attack_snapshot);
                value(enemy.status.burn->expires);
                value(enemy.status.burn->next_tick);
                value(enemy.status.burn->propagated);
                value(enemy.status.burn->source_skill);
                value(enemy.status.burn->source_upgrade);
                value(enemy.status.burn->source_relic);
            }
            count(enemy.status.slows.size());
            for (const auto &slow : enemy.status.slows)
            {
                value(slow.reduction);
                value(slow.expires);
                value(slow.source_skill);
                value(slow.source_upgrade);
                value(slow.source_relic);
            }
            value(enemy.last_damage_skill);
            value(enemy.last_damage_origin);
            value(enemy.last_damage_cast);
            value(enemy.last_damage_upgrade);
            value(enemy.marked_by_skill);
            value(enemy.marked_damage_coefficient);
            value(enemy.mark_expires);
            value(enemy.bleed_burn_ready);
            value(enemy.different_skill_ready);
            value(enemy.last_active_hit);
            value(enemy.last_active_hit_tick);
            value(enemy.attacking);
            value(enemy.dead);
        }

        count(enemy_hit_casts.size());
        for (const auto hit : enemy_hit_casts) value(hit);

        count(projectiles.size());
        for (const auto &projectile : projectiles)
        {
            value(projectile.id.value);
            value(projectile.player_owned);
            vector2(projectile.position);
            vector2(projectile.previous_position);
            vector2(projectile.velocity);
            value(projectile.remaining_range);
            value(projectile.radius);
            value(projectile.damage);
            value(projectile.skill);
            value(projectile.upgrade_mask);
            value(projectile.origin);
            value(projectile.cast_id);
            value(projectile.source_upgrade);
            value(projectile.source_relic);
            value(projectile.source_enemy);
            value(projectile.pierce_remaining);
            value(projectile.bounce_remaining);
            value(projectile.hit_count);
            for (std::size_t index = 0; index < projectile.hit_count; ++index)
                value(projectile.hit_ids[index]);
            value(projectile.explosion_radius);
            value(projectile.explosion_damage);
            value(projectile.explosion_source_upgrade);
            value(projectile.bleed_stacks);
            value(projectile.burn);
            value(projectile.slow);
            value(projectile.returning);
            value(projectile.homing);
            value(projectile.homing_target);
            value(projectile.full_charge);
            value(projectile.charge_ratio);
            value(projectile.carried_burn_attack);
            value(projectile.carried_burn_expires);
            value(projectile.dead);
        }

        count(areas.size());
        for (const auto &area : areas)
        {
            value(area.id.value);
            value(area.kind);
            vector2(area.position);
            vector2(area.direction);
            value(area.radius);
            value(area.half_length);
            value(area.effect_radius);
            value(area.damage);
            value(area.active_tick);
            value(area.next_tick);
            value(area.expires);
            value(area.interval);
            value(area.skill);
            value(area.upgrade_mask);
            value(area.origin);
            value(area.cast_id);
            value(area.source_upgrade);
            value(area.source_relic);
            value(area.source_enemy);
            value(area.slow_reduction);
            value(area.slow_duration);
            value(area.trigger_count);
            value(area.ring_inner_radius);
            value(area.ring_outer_radius);
            value(area.safe_gap_degrees);
            value(area.safe_gap_count);
            value(area.applies_burn);
            value(area.dead);
        }

        count(pickups.size());
        for (const auto &pickup : pickups)
        {
            value(pickup.id.value);
            value(pickup.kind);
            vector2(pickup.position);
            value(pickup.value);
            value(pickup.spawned_tick);
            value(pickup.guaranteed_boss_chest);
            value(pickup.attracted_by_magnet_stat);
            value(pickup.globally_attracted);
            value(pickup.dead);
        }

        count(damage_events.size());
        for (const auto &event : damage_events)
        {
            value(event.sequence);
            value(event.target);
            value(event.amount);
            value(event.skill);
            value(event.origin);
            value(event.cast_id);
            value(event.source_upgrade);
            value(event.source_relic);
            value(event.source_enemy);
            value(event.bleed_stacks);
            value(event.burn);
            value(event.slow_reduction);
            value(event.slow_duration);
            value(event.amplified_count);
            for (std::size_t index = 0; index < event.amplified_count; ++index)
            {
                value(event.amplified_upgrades[index]);
                value(event.amplified_damage[index]);
            }
        }
        count(scheduled.size());
        for (const auto &action : scheduled)
        {
            value(action.due);
            value(action.kind);
            value(action.skill);
            vector2(action.position);
            vector2(action.direction);
            value(action.damage_coefficient);
            value(action.radius);
            value(action.duration);
            value(action.projectile_count);
            value(action.upgrade_mask);
            value(action.origin);
            value(action.cast_id);
            value(action.source_upgrade);
            value(action.source_relic);
        }
        count(boss_actions.size());
        for (const auto &action : boss_actions)
        {
            value(action.due);
            value(action.kind);
            value(action.boss_id);
            vector2(action.position);
            vector2(action.direction);
            value(action.speed);
            value(action.distance);
            value(action.damage);
            value(action.projectile_count);
            value(action.arc_degrees);
            value(action.angle_offset);
            value(action.radius);
            value(action.duration);
            value(action.cast_id);
        }
        count(cast_hits.size());
        for (const auto &record : cast_hits)
        {
            value(record.cast_id);
            value(record.target);
            value(record.count);
        }
        count(area_hits.size());
        for (const auto &record : area_hits)
        {
            value(record.area_id);
            value(record.target);
            value(record.count);
        }
        count(cast_runtime.size());
        for (const auto &runtime : cast_runtime)
        {
            value(runtime.cast_id);
            value(runtime.skill);
            value(runtime.upgrade_mask);
            value(runtime.normal_hits);
            value(runtime.kills);
            value(runtime.transfer_count);
            value(runtime.spawn_count);
            value(runtime.basic_relic_triggers);
            value(runtime.triggered);
            value(runtime.refund_triggered);
            value(runtime.full_charge);
            value(runtime.stored_burn_attack);
            value(runtime.stored_burn_remaining);
            value(runtime.has_stored_burn);
            value(runtime.terminal_target);
            value(runtime.telemetry_hit);
        }
        count(waves.size());
        for (const auto &wave : waves)
        {
            value(wave.start);
            value(wave.total);
            value(wave.emitted);
        }
        value(card_count);
        for (std::size_t index = 0; index < card_count; ++index)
        {
            value(cards[index].kind);
            value(cards[index].subject);
            value(cards[index].upgrade);
        }
        return hash;
    }

    void BuildPipeline()
    {
        world.set_threads(1);
        auto pipeline_phase =
            world.entity("InputPhase").add(flecs::Phase).depends_on(flecs::OnUpdate);
        auto add_phase = [&](const char *name) {
            auto next = world.entity(name).add(flecs::Phase).depends_on(pipeline_phase);
            pipeline_phase = next;
            return next;
        };
        auto session = add_phase("SessionTimerPhase");
        auto spawn = add_phase("SpawnPhase");
        auto ai = add_phase("AiIntentPhase");
        auto cast = add_phase("CastAttackPhase");
        auto movement = add_phase("MovementPhase");
        auto grid = add_phase("SpatialGridPhase");
        auto collision = add_phase("CollisionHitPhase");
        auto damage = add_phase("DamageStatusPhase");
        auto death = add_phase("DeathDropPhase");
        auto xp = add_phase("XpCardPhase");
        auto merge = add_phase("StructuralMergePhase");
        auto checksum_phase = add_phase("ChecksumSnapshotPhase");
        world.system<>("SessionTimer").kind(session).run([this](flecs::iter &iterator) {
            while (iterator.next()) SessionTimerPhase();
        });
        world.system<>("Spawn").kind(spawn).run([this](flecs::iter &iterator) {
            while (iterator.next()) SpawnPhase();
        });
        world.system<>("AiIntent").kind(ai).run([this](flecs::iter &iterator) {
            while (iterator.next()) AiIntentPhase();
        });
        world.system<>("CastAttack").kind(cast).run([this](flecs::iter &iterator) {
            while (iterator.next()) CastAttackPhase();
        });
        world.system<>("Movement").kind(movement).run([this](flecs::iter &iterator) {
            while (iterator.next()) MovementPhase();
        });
        world.system<>("SpatialGrid").kind(grid).run([this](flecs::iter &iterator) {
            while (iterator.next()) SpatialGridPhase();
        });
        world.system<>("CollisionHit").kind(collision).run([this](flecs::iter &iterator) {
            while (iterator.next()) CollisionHitPhase();
        });
        world.system<>("DamageStatus").kind(damage).run([this](flecs::iter &iterator) {
            while (iterator.next()) DamageStatusPhase();
        });
        world.system<>("DeathDrop").kind(death).run([this](flecs::iter &iterator) {
            while (iterator.next()) DeathDropPhase();
        });
        world.system<>("XpCard").kind(xp).run([this](flecs::iter &iterator) {
            while (iterator.next()) XpCardPhase();
        });
        world.system<>("StructuralMerge").kind(merge).run([this](flecs::iter &iterator) {
            while (iterator.next()) StructuralMergePhase();
        });
        world.system<>("ChecksumSnapshot").kind(checksum_phase).run(
            [this](flecs::iter &iterator) {
                while (iterator.next()) checksum = CalculateChecksum();
            });
    }
};

GameSimulation::GameSimulation() : impl_(std::make_unique<Impl>())
{
}

GameSimulation::~GameSimulation() = default;

Result GameSimulation::Initialize(const SimulationConfig &config)
{
    return Initialize(config, GameData::Defaults());
}

Result GameSimulation::Initialize(const SimulationConfig &config, const GameData &data)
{
    if (impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "GameSimulation already initialized.");
    }
    impl_->config = config;
    impl_->data = data;
    impl_->phase = config.start_in_main_menu ? SessionPhase::MainMenu : SessionPhase::Playing;
    impl_->player.health = impl_->player.max_health = data.player_health;
    impl_->player.attack = data.player_attack;
    impl_->player.attack_speed = data.player_attack_speed;
    impl_->player.move_speed = data.player_move_speed;
    impl_->player.magnet_radius = data.player_magnet_radius;
    impl_->player.skill_levels[0] = 1;
    impl_->world.entity("Archer").set<DeterministicKey>({0}).add<PlayerTag>();
    impl_->BuildPipeline();
    impl_->initialized = true;
    impl_->checksum = impl_->CalculateChecksum();
    return Result::Success();
}

TickResult GameSimulation::TickFixed(const InputFrame &input,
                                     std::chrono::nanoseconds fixed_delta)
{
    if (!impl_->initialized)
    {
        return {};
    }
    impl_->input = input;
    impl_->ProcessInput();
    if (impl_->phase != SessionPhase::Playing)
    {
        impl_->player.buffered_skill = SkillKind::Count;
        impl_->player.buffered_skill_expires = 0;
        impl_->checksum = impl_->CalculateChecksum();
        return {impl_->tick, impl_->checksum, impl_->phase};
    }
    ++impl_->tick;
    impl_->world.progress(std::chrono::duration<float>(fixed_delta).count());
    return {impl_->tick, impl_->checksum, impl_->phase};
}

GameplayChecksum GameSimulation::ComputeChecksum() const
{
    return impl_->checksum;
}

SessionProbe GameSimulation::Probe() const noexcept
{
    SessionProbe probe;
    probe.tick = impl_->tick;
    probe.growth_ticks = impl_->growth_ticks;
    probe.boss_fight_ticks = impl_->boss_fight_ticks;
    probe.phase = impl_->phase;
    probe.level = impl_->player.level;
    probe.experience = impl_->player.experience;
    probe.experience_to_next = ExperienceForLevel(impl_->player.level);
    probe.health = impl_->player.health;
    probe.max_health = impl_->player.max_health;
    probe.player_position = impl_->player.position;
    probe.aim_direction = impl_->player.aim;
    probe.facing_direction = impl_->player.facing;
    probe.normal_enemy_count = impl_->NormalEnemyCount();
    probe.boss_count = impl_->BossCount();
    probe.boss_warning_count = static_cast<std::uint32_t>(impl_->boss_actions.size());
    probe.boss_dashing_count = static_cast<std::uint32_t>(
        std::ranges::count_if(impl_->enemies, [&](const EnemyActor &enemy) {
            return enemy.boss.has_value() && !enemy.dead && impl_->tick < enemy.dash_until;
        }));
    probe.enemy_area_count = static_cast<std::uint32_t>(
        std::ranges::count_if(impl_->areas, [](const AreaActor &area) {
            return !area.dead && area.kind == AreaKind::EnemyDamage;
        }));
    probe.player_projectile_count = impl_->ProjectileCount(true);
    probe.enemy_projectile_count = impl_->ProjectileCount(false);
    probe.pickup_count = static_cast<std::uint32_t>(impl_->pickups.size());
    probe.kills = impl_->kills;
    probe.damage_dealt = impl_->damage_dealt;
    probe.damage_by_skill = impl_->damage_by_skill;
    probe.balance = impl_->balance;
    probe.damage_taken = impl_->damage_taken;
    probe.healing = impl_->healing;
    probe.level_rerolls_remaining = impl_->player.level_rerolls;
    probe.relic_rerolls_remaining = impl_->player.relic_rerolls;
    probe.pending_stat_points = impl_->player.pending_stat_points;
    probe.active_skill_count = static_cast<std::uint8_t>(std::ranges::count_if(
        impl_->player.loadout, [](SkillKind skill) { return skill != SkillKind::Count; }));
    probe.skill_loadout = impl_->player.loadout;
    probe.skill_levels = impl_->player.skill_levels;
    probe.upgrade_masks = impl_->player.upgrades;
    for (std::size_t index = 0; index < probe.cooldown_ticks.size(); ++index)
    {
        probe.cooldown_ticks[index] = static_cast<std::uint32_t>(
            std::min<Tick>(impl_->player.cooldowns[index],
                           std::numeric_limits<std::uint32_t>::max()));
    }
    probe.stat_points = impl_->player.stats;
    probe.relic_mask = impl_->player.relic_mask;
    probe.cards = impl_->cards;
    probe.card_count = impl_->card_count;
    probe.final_boss_spawned = impl_->final_boss_spawned;
    probe.final_boss_phase_two = std::ranges::any_of(impl_->enemies, [](const EnemyActor &enemy) {
        return enemy.boss == BossKind::Final && enemy.final_phase == 2;
    });
    probe.menu_page = impl_->menu_page;
    return probe;
}

Result GameSimulation::ApplyDebugCommand(const DebugCommand &command)
{
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "Simulation is not initialized.");
    }
    switch (command.kind)
    {
    case DebugCommandKind::StartSession:
        impl_->StartSession();
        break;
    case DebugCommandKind::SetGrowthTick:
        impl_->growth_ticks = command.value;
        break;
    case DebugCommandKind::DamagePlayer:
        impl_->player.health -= static_cast<std::int32_t>(command.value);
        break;
    case DebugCommandKind::HealPlayer:
        impl_->player.health = std::min(
            impl_->player.max_health,
            impl_->player.health + static_cast<std::int32_t>(command.value));
        break;
    case DebugCommandKind::DamageFinalBoss:
        for (auto &enemy : impl_->enemies)
        {
            if (enemy.boss == BossKind::Final)
            {
                enemy.health -= static_cast<std::int32_t>(command.value);
                break;
            }
        }
        break;
    case DebugCommandKind::GrantExperience:
        impl_->player.experience += static_cast<std::uint32_t>(command.value);
        break;
    case DebugCommandKind::GrantSkill:
    {
        const auto skill = static_cast<SkillKind>(command.value);
        if (skill <= SkillKind::BasicAttack || skill >= SkillKind::Count)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug skill is invalid.");
        }
        if (impl_->player.skill_levels[static_cast<std::size_t>(skill)] == 0)
        {
            impl_->player.skill_levels[static_cast<std::size_t>(skill)] = 1;
            const auto slot = std::ranges::find(impl_->player.loadout, SkillKind::Count);
            if (slot != impl_->player.loadout.end()) *slot = skill;
        }
        break;
    }
    case DebugCommandKind::GrantUpgrade:
        if (command.value >= kCombatSkillCount || command.secondary >= kUpgradeCount)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug upgrade is invalid.");
        }
        impl_->player.upgrades[command.value] |= 1u << command.secondary;
        impl_->player.skill_levels[command.value] = static_cast<std::uint8_t>(
            1 + std::popcount(impl_->player.upgrades[command.value]));
        break;
    case DebugCommandKind::GrantRelic:
        if (command.value >= kRelicCount)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug relic is invalid.");
        }
        impl_->player.relic_mask |= 1u << command.value;
        break;
    case DebugCommandKind::SpawnEnemy:
        impl_->SpawnEnemy(static_cast<EnemyKind>(command.value % 3), command.position);
        break;
    case DebugCommandKind::SpawnBoss:
        if (command.value >= 3 ||
            !impl_->SpawnBoss(static_cast<BossKind>(command.value)))
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                   "Debug boss could not be spawned.");
        }
        break;
    case DebugCommandKind::SelectCard:
        if (!impl_->SelectCard(static_cast<std::size_t>(command.value)))
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug card index is invalid.");
        }
        break;
    case DebugCommandKind::AssignStat:
        impl_->AssignStat(static_cast<StatKind>(command.value % kStatCount));
        break;
    case DebugCommandKind::SetStat:
        if (command.value >= kStatCount || command.secondary > 10)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_gameplay",
                                   "Debug stat or level is invalid.");
        }
        impl_->player.stats[command.value] =
            static_cast<std::uint8_t>(command.secondary);
        if (command.value == static_cast<std::uint64_t>(StatKind::MaxHealth))
        {
            impl_->player.max_health = RoundDamage(
                100.0f * (1.0f + 0.08f * command.secondary));
            impl_->player.health = impl_->player.max_health;
        }
        break;
    case DebugCommandKind::Reroll:
    {
        auto &rerolls = impl_->phase == SessionPhase::RelicSelection
                            ? impl_->player.relic_rerolls
                            : impl_->player.level_rerolls;
        auto &sequence = impl_->phase == SessionPhase::RelicSelection
                             ? impl_->relic_reroll_sequence
                             : impl_->level_reroll_sequence;
        if (rerolls == 0 || impl_->card_count == 0)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                   "No reroll is available.");
        }
        --rerolls;
        ++sequence;
        if (impl_->phase == SessionPhase::RelicSelection) impl_->GenerateRelicCards(true);
        else impl_->GenerateLevelCards(true);
        break;
    }
    case DebugCommandKind::TogglePause:
        impl_->phase = impl_->phase == SessionPhase::Paused ? SessionPhase::Playing
                                                            : SessionPhase::Paused;
        break;
    }
    impl_->checksum = impl_->CalculateChecksum();
    return Result::Success();
}

void GameSimulation::ApplySettings(const SettingsData &settings) noexcept
{
    impl_->config.settings = settings;
    impl_->pending_rebind_slot = 0xFF;
}

Result GameSimulation::ApplyGameData(const GameData &data)
{
    if (!impl_->initialized || impl_->phase != SessionPhase::MainMenu)
        return Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                               "Gameplay data Hot Reload requires the main menu.");
    impl_->data = data;
    return Result::Success();
}

bool GameSimulation::WriteRenderSnapshot(RenderSnapshotStorage &snapshot) const
{
    snapshot.header.tick = impl_->tick;
    snapshot.header.simulation_time = std::chrono::nanoseconds(16'666'667) * impl_->tick;
    snapshot.header.checksum = impl_->checksum;
    snapshot.camera.target = {impl_->player.position.x, 0.0f, impl_->player.position.y};

    bool complete = true;
    complete &= snapshot.AddInstance(
        {{impl_->player.position.x, 0.0f, impl_->player.position.y},
         std::atan2(impl_->player.facing.x, impl_->player.facing.y),
         {1.0f, 1.0f, 1.0f}, 0xFFFFFFFFu, RenderMesh::Archer,
         kPlayerRenderId});
    AnimationPoseRef pose;
    pose.instance_index = 0;
    pose.clip = CharacterAnimationClip::Idle;
    pose.normalized_time = static_cast<float>(impl_->tick % 120) / 120.0f;
    pose.secondary_clip = CharacterAnimationClip::Run;
    pose.secondary_normalized_time = static_cast<float>(impl_->tick % 45) / 45.0f;
    pose.secondary_weight = impl_->player.locomotion_blend;
    if (impl_->phase == SessionPhase::Defeat)
    {
        pose.clip = CharacterAnimationClip::Death;
        pose.normalized_time = 1.0f;
        pose.secondary_clip = CharacterAnimationClip::Death;
        pose.secondary_normalized_time = 1.0f;
        pose.secondary_weight = 0.0f;
    }
    else if (impl_->player.charging)
    {
        pose.upper_body_clip = CharacterAnimationClip::Draw;
        pose.upper_body_normalized_time = std::min(
            1.0f, static_cast<float>(impl_->tick - impl_->player.charge_start) /
                      static_cast<float>(HasUpgrade(
                          impl_->player.upgrades[static_cast<std::size_t>(
                              SkillKind::ChargedShot)], 1)
                                             ? Seconds(1.4f)
                                             : Seconds(1.0f)));
        pose.upper_body_weight = std::clamp(
            static_cast<float>(impl_->tick - impl_->player.charge_start + 1) /
                static_cast<float>(Seconds(0.10f)),
            0.0f, 1.0f);
    }
    else if (impl_->tick < impl_->player.basic_attack_animation_until)
    {
        const auto clip_until = impl_->player.basic_attack_animation_until -
                                kAnimationBlendOutTicks;
        const auto playback_ticks = std::max<Tick>(
            1, clip_until - impl_->player.basic_attack_animation_start);
        pose.upper_body_clip = CharacterAnimationClip::Recoil;
        pose.upper_body_normalized_time = std::clamp(
            static_cast<float>(impl_->tick - impl_->player.basic_attack_animation_start) /
                static_cast<float>(kRecoilClipTicks),
            0.0f, 1.0f);
        pose.upper_body_playback_rate = static_cast<float>(kRecoilClipTicks) /
                                        static_cast<float>(playback_ticks);
        const auto fade_in = static_cast<float>(
            impl_->tick - impl_->player.basic_attack_animation_start + 1) /
            static_cast<float>(Seconds(0.10f));
        const auto fade_out = impl_->tick <= clip_until
                                  ? 1.0f
                                  : static_cast<float>(
                                        impl_->player.basic_attack_animation_until -
                                        impl_->tick) /
                                        static_cast<float>(kAnimationBlendOutTicks);
        pose.upper_body_weight = std::clamp(std::min(fade_in, fade_out), 0.0f, 1.0f);
    }
    else if (impl_->tick < impl_->player.active_animation_until ||
             impl_->tick < impl_->player.retreat_until)
    {
        const auto clip_until = impl_->player.active_cast_tick;
        const auto playback_ticks = std::max<Tick>(
            1, clip_until - impl_->player.active_animation_start);
        pose.upper_body_clip = CharacterAnimationClip::Recoil;
        pose.upper_body_normalized_time = std::clamp(
            static_cast<float>(impl_->tick - impl_->player.active_animation_start) /
                static_cast<float>(kRecoilClipTicks), 0.0f, 1.0f);
        pose.upper_body_playback_rate = static_cast<float>(kRecoilClipTicks) /
                                        static_cast<float>(playback_ticks);
        const auto fade_in = static_cast<float>(
            impl_->tick - impl_->player.active_animation_start + 1) /
            static_cast<float>(Seconds(0.10f));
        const auto fade_out = impl_->tick <= clip_until
                                  ? 1.0f
                                  : static_cast<float>(
                                        impl_->player.active_animation_until - impl_->tick) /
                                        static_cast<float>(kAnimationBlendOutTicks);
        pose.upper_body_weight = std::clamp(std::min(fade_in, fade_out), 0.0f, 1.0f);
    }
    complete &= snapshot.AddPose(pose);

    if (impl_->player.charging &&
        impl_->player.charging_skill == SkillKind::ChargedShot)
    {
        const auto mask = impl_->player.upgrades[
            static_cast<std::size_t>(SkillKind::ChargedShot)];
        const auto maximum_ticks = HasUpgrade(mask, 1) ? Seconds(1.4f) : Seconds(1.0f);
        auto elapsed = std::min(impl_->tick - impl_->player.charge_start, maximum_ticks);
        if (HasUpgrade(mask, 2))
        {
            elapsed = std::min(maximum_ticks, static_cast<Tick>(elapsed / 0.65f));
        }
        const auto ratio = static_cast<float>(elapsed) /
                           static_cast<float>(maximum_ticks);
        const auto range = std::lerp(
            4.2f,
            impl_->data.skills[static_cast<std::size_t>(SkillKind::ChargedShot)].range,
            ratio);
        const auto center = Add(impl_->player.position,
                                Multiply(impl_->player.aim, range * 0.5f));
        complete &= snapshot.AddInstance(
            {{center.x, 0.025f, center.y},
             std::atan2(impl_->player.aim.x, impl_->player.aim.y),
             {0.10f, 0.03f, range}, 0xFFFFFFFFu, RenderMesh::Area});
    }

    const auto arena_size = impl_->data.arena_half_extent * 2.0f;
    complete &= snapshot.AddInstance(
        {{0.0f, -0.05f, 0.0f}, 0.0f, {arena_size, 0.1f, arena_size},
         0xFF181818u, RenderMesh::Ground});

    constexpr float kBoundaryThickness = 0.6f;
    constexpr float kBoundaryHeightScale = 12.0f;
    constexpr std::uint32_t kBoundaryColor = 0xFF20A0FFu;
    const auto boundary_center = impl_->data.arena_half_extent + kBoundaryThickness * 0.5f;
    const auto boundary_length = impl_->data.arena_half_extent * 2.0f + kBoundaryThickness * 2.0f;
    for (const auto &instance : std::array{
             RenderInstance{{-boundary_center, 0.6f, 0.0f}, 0.0f,
                            {kBoundaryThickness, kBoundaryHeightScale, boundary_length},
                            kBoundaryColor, RenderMesh::Area},
             RenderInstance{{boundary_center, 0.6f, 0.0f}, 0.0f,
                            {kBoundaryThickness, kBoundaryHeightScale, boundary_length},
                            kBoundaryColor, RenderMesh::Area},
             RenderInstance{{0.0f, 0.6f, -boundary_center}, 0.0f,
                            {boundary_length, kBoundaryHeightScale, kBoundaryThickness},
                            kBoundaryColor, RenderMesh::Area},
             RenderInstance{{0.0f, 0.6f, boundary_center}, 0.0f,
                            {boundary_length, kBoundaryHeightScale, kBoundaryThickness},
                            kBoundaryColor, RenderMesh::Area},
         })
    {
        complete &= snapshot.AddInstance(instance);
    }
    for (const auto &enemy : impl_->enemies)
    {
        if (enemy.dead) continue;
        const auto color = enemy.boss == BossKind::Final ? 0xFF4040E8u :
                           enemy.boss == BossKind::TenMinute ? 0xFFB040E8u :
                           enemy.boss == BossKind::FiveMinute ? 0xFFE86040u :
                           enemy.kind == EnemyKind::Ranged ? 0xFF40B060u :
                           enemy.kind == EnemyKind::Suicide ? 0xFF40D8E8u : 0xFF5D66E8u;
        const auto scale = enemy.boss ? Float3{2.0f, 2.4f, 2.0f}
                                      : enemy.kind == EnemyKind::Suicide
                                            ? Float3{0.6f, 0.8f, 0.6f}
                                            : Float3{0.75f, 1.1f, 0.75f};
        std::uint32_t status_visual_mask{};
        if (enemy.status.bleed_count != 0) status_visual_mask |= static_cast<std::uint32_t>(StatusVisual::Bleed);
        if (enemy.status.burn) status_visual_mask |= static_cast<std::uint32_t>(StatusVisual::Burn);
        if (!enemy.status.slows.empty()) status_visual_mask |= static_cast<std::uint32_t>(StatusVisual::Slow);
        if (enemy.marked_by_skill != SkillKind::Count && enemy.mark_expires >= impl_->tick)
            status_visual_mask |= static_cast<std::uint32_t>(StatusVisual::Mark);
        complete &= snapshot.AddInstance(
            {{enemy.position.x, 0.0f, enemy.position.y},
             std::atan2(enemy.velocity.x, enemy.velocity.y), scale, color,
             enemy.boss ? RenderMesh::Boss : enemy.kind == EnemyKind::Ranged
                                                    ? RenderMesh::EnemyRanged
                                                    : enemy.kind == EnemyKind::Suicide
                                                          ? RenderMesh::EnemySuicide
                                                          : RenderMesh::Enemy,
             kEnemyRenderId | enemy.id.value, status_visual_mask});
        if (!enemy.boss && enemy.kind == EnemyKind::Ranged && enemy.attacking)
        {
            const auto range = impl_->data.enemies[
                static_cast<std::size_t>(EnemyKind::Ranged)].projectile_range;
            const auto center = Add(enemy.position,
                                    Multiply(enemy.locked_aim, range * 0.5f));
            complete &= snapshot.AddInstance(
                {{center.x, 0.025f, center.y},
                 std::atan2(enemy.locked_aim.x, enemy.locked_aim.y),
                 {0.16f, 0.03f, range}, 0xA03030FFu, RenderMesh::Area});
        }
        if (!enemy.boss && enemy.kind == EnemyKind::Suicide && enemy.attacking)
        {
            const auto radius = impl_->data.enemies[
                    static_cast<std::size_t>(EnemyKind::Suicide)].projectile_range;
            complete &= snapshot.AddInstance(
                {{enemy.position.x, 0.025f, enemy.position.y}, 0.0f,
                 {radius, 0.03f, radius}, 0x803030FFu, RenderMesh::Area});
        }
    }
    for (const auto &projectile : impl_->projectiles)
    {
        if (projectile.dead) continue;
        auto scale = projectile.player_owned ? Float3{0.24f, 0.24f, 0.825f}
                                             : Float3{0.24f, 0.24f, 0.8f};
        auto color = projectile.player_owned ? 0xFF40E8FFu : 0xFF4040FFu;
        if (projectile.player_owned)
        {
            switch (projectile.skill)
            {
            case SkillKind::PiercingShot: scale = {0.28f, 0.28f, 1.25f}; color = 0xFFFFE8A0u; break;
            case SkillKind::MultiShot: scale = {0.18f, 0.18f, 0.7f}; color = 0xFFFFD878u; break;
            case SkillKind::ChargedShot: scale = {0.38f, 0.38f, 1.45f}; color = 0xFFFFF0C0u; break;
            case SkillKind::ExplosiveArrow: scale = {0.34f, 0.34f, 1.0f}; color = 0xFF188CFFu; break;
            case SkillKind::RicochetArrow: scale = {0.25f, 0.25f, 0.85f}; color = 0xFFFFA840u; break;
            default: break;
            }
        }
        complete &= snapshot.AddInstance(
            {{projectile.position.x, 0.25f, projectile.position.y},
             std::atan2(projectile.velocity.x, projectile.velocity.y),
             scale, color,
             projectile.player_owned ? RenderMesh::PlayerProjectile
                                      : RenderMesh::EnemyProjectile,
             kProjectileRenderId | projectile.id.value});
    }
    for (const auto &area : impl_->areas)
    {
        if (area.dead) continue;
        if (area.kind == AreaKind::Trap)
        {
            complete &= snapshot.AddPersistentVfx(
                {{area.position.x, 0.025f, area.position.y}, 0.0f, area.radius,
                 impl_->tick < area.active_tick ? PersistentVfxKind::TrapPending
                                                : PersistentVfxKind::TrapArmed,
                 kAreaRenderId | area.id.value});
        }
        const auto fire_area = area.applies_burn ||
            (area.skill == SkillKind::ExplosiveArrow && area.source_upgrade == 3) ||
            (area.skill == SkillKind::Trap && area.source_upgrade == 4) ||
            (area.skill == SkillKind::ArrowRain && area.source_upgrade == 3);
        if (fire_area && impl_->tick >= area.active_tick)
        {
            complete &= snapshot.AddPersistentVfx(
                {{area.position.x, 0.02f, area.position.y}, 0.0f, area.radius,
                 PersistentVfxKind::FireArea, kAreaRenderId | area.id.value});
        }
        const auto particle_visual = area.kind == AreaKind::Slow ||
            area.kind == AreaKind::Trap ||
            (area.kind == AreaKind::Damage && area.half_length <= 0.0f);
        if (particle_visual) continue;
        if (area.ring_outer_radius > 0.0f && area.safe_gap_count > 0)
        {
            const auto duration = std::max<Tick>(area.expires - area.active_tick, 1);
            const auto progress = std::clamp(
                static_cast<float>(impl_->tick - area.active_tick) /
                    static_cast<float>(duration),
                0.0f, 1.0f);
            const auto radius = std::lerp(area.ring_inner_radius,
                                          area.ring_outer_radius, progress);
            constexpr std::uint32_t kSegments = 64;
            for (std::uint32_t index = 0; index < kSegments; ++index)
            {
                const auto degrees = 360.0f * static_cast<float>(index) / kSegments;
                const auto spacing = 360.0f / area.safe_gap_count;
                const auto offset = static_cast<float>(area.cast_id % 360);
                const auto nearest_gap =
                    std::fmod(degrees - offset + spacing * 0.5f + 360.0f, spacing) -
                    spacing * 0.5f;
                if (std::abs(nearest_gap) <= area.safe_gap_degrees * 0.5f)
                    continue;
                const auto direction = Rotate({1.0f, 0.0f}, degrees);
                const auto position = Add(area.position, Multiply(direction, radius));
                complete &= snapshot.AddInstance(
                    {{position.x, 0.03f, position.y}, -degrees * kPi / 180.0f,
                     {0.24f, 0.04f, std::max(0.35f, radius * 0.05f)},
                     0xB04040FFu, RenderMesh::Area});
            }
        }
        else
        {
            const auto trail = area.half_length > 0.0f;
            complete &= snapshot.AddInstance(
                {{area.position.x, 0.01f, area.position.y},
                 trail ? std::atan2(area.direction.x, area.direction.y) : 0.0f,
                 trail ? Float3{area.radius * 2.0f, 0.04f,
                                area.half_length * 2.0f}
                       : Float3{area.radius, 0.04f, area.radius},
                 area.kind == AreaKind::EnemyDamage ? 0x604040FFu :
                 area.kind == AreaKind::Slow ? 0x6040A0FFu : 0x6080D040u,
                 RenderMesh::Area, kAreaRenderId | area.id.value});
        }
    }
    for (const auto &action : impl_->boss_actions)
    {
        const auto boss = std::ranges::find_if(
            impl_->enemies, [&](const EnemyActor &enemy) {
                return enemy.id.value == action.boss_id && !enemy.dead;
            });
        if (boss == impl_->enemies.end())
            continue;

        const auto warning_color = 0xA03030FFu;
        if (action.kind == BossActionKind::Dash)
        {
            auto direction = action.direction;
            if (LengthSquared(direction) <= 0.0001f)
                direction = Normalize(Subtract(impl_->player.position, boss->position));
            constexpr std::uint32_t kMarkers = 20;
            for (std::uint32_t index = 1; index <= kMarkers; ++index)
            {
                const auto position = Add(
                    boss->position,
                    Multiply(direction, action.distance * static_cast<float>(index) /
                                            static_cast<float>(kMarkers)));
                complete &= snapshot.AddInstance(
                    {{position.x, 0.025f, position.y}, 0.0f,
                     {0.28f, 0.03f, 0.28f}, warning_color, RenderMesh::Area});
            }
        }
        else if (action.kind == BossActionKind::Volley)
        {
            const auto direction = LengthSquared(action.direction) > 0.0001f
                                       ? action.direction
                                       : Normalize(Subtract(impl_->player.position,
                                                            boss->position));
            const std::array angles{-action.arc_degrees * 0.5f + action.angle_offset,
                                    action.arc_degrees * 0.5f + action.angle_offset};
            for (const auto angle : angles)
            {
                const auto edge = Rotate(direction, angle);
                for (std::uint32_t index = 1; index <= 12; ++index)
                {
                    const auto position = Add(boss->position,
                        Multiply(edge, 24.0f * static_cast<float>(index) / 12.0f));
                    complete &= snapshot.AddInstance(
                        {{position.x, 0.025f, position.y}, 0.0f,
                         {0.22f, 0.03f, 0.22f}, warning_color, RenderMesh::Area});
                }
            }
        }
        else
        {
            const auto center = action.kind == BossActionKind::Shockwave
                                    ? boss->position : action.position;
            const auto radius = action.radius;
            constexpr std::uint32_t kSegments = 48;
            for (std::uint32_t index = 0; index < kSegments; ++index)
            {
                const auto degrees = 360.0f * static_cast<float>(index) / kSegments;
                if (action.kind == BossActionKind::Shockwave)
                {
                    constexpr auto kSpacing = 90.0f;
                    const auto offset = static_cast<float>(action.cast_id % 360);
                    const auto nearest_gap =
                        std::fmod(degrees - offset + kSpacing * 0.5f + 360.0f,
                                  kSpacing) - kSpacing * 0.5f;
                    if (std::abs(nearest_gap) <= 12.5f)
                        continue;
                }
                const auto direction = Rotate({1.0f, 0.0f}, degrees);
                const auto position = Add(center, Multiply(direction, radius));
                complete &= snapshot.AddInstance(
                    {{position.x, 0.025f, position.y}, 0.0f,
                     {0.24f, 0.03f, 0.24f}, warning_color, RenderMesh::Area});
            }
        }
    }
    for (const auto &pickup : impl_->pickups)
    {
        if (pickup.dead) continue;
        complete &= snapshot.AddInstance(
            {{pickup.position.x, 0.32f, pickup.position.y},
             pickup.kind == PickupKind::Experience ? 0.785398f : 0.0f,
             pickup.kind == PickupKind::Experience ? Float3{0.34f, 0.34f, 0.34f} :
             pickup.kind == PickupKind::Heal ? Float3{0.26f, 0.55f, 0.26f} :
             pickup.kind == PickupKind::Magnet ? Float3{0.48f, 0.48f, 0.48f} :
                                                 Float3{0.40f, 0.40f, 0.40f},
             pickup.kind == PickupKind::Experience ? 0xFFFFD040u :
             pickup.kind == PickupKind::Heal ? 0xFF40E060u :
             pickup.kind == PickupKind::Magnet ? 0xFFFF3030u : 0xFFE080FFu,
             RenderMesh::Pickup, kPickupRenderId | pickup.id.value});
    }
    complete &= snapshot.AddLight(
        {{-0.45f, -0.82f, 0.35f}, 3.0f, {1.0f, 0.92f, 0.78f}});

    const auto probe = Probe();
    const auto add_ui = [&](UiModel::Kind kind, Float2 anchor, Float2 size,
                            std::uint32_t color, std::string_view text,
                            float value = 1.0f, std::uint16_t font = 28) {
        UiModel model;
        model.kind = kind;
        model.anchor_pixels = anchor;
        model.size_pixels = size;
        model.color_rgba = color;
        model.value = value;
        model.font_pixels = font;
        auto byte_count = std::min(text.size(), model.utf8_text.size() - 1);
        while (byte_count < text.size() && byte_count > 0 &&
               (static_cast<unsigned char>(text[byte_count]) & 0xC0u) == 0x80u)
            --byte_count;
        std::memcpy(model.utf8_text.data(), text.data(), byte_count);
        complete &= snapshot.AddUi(model);
    };
    constexpr std::array<std::string_view, kCombatSkillCount> skill_names{
        "기본 공격", "관통 사격", "다중 사격", "충전 사격", "폭발 화살",
        "도탄 화살", "화살비", "덫", "후퇴 사격"};
    constexpr std::array<std::string_view, kCombatSkillCount> skill_descriptions{
        "기본 공격을 유지하면 이동을 멈추고 조준 방향으로 화살을 반복 발사합니다. 화살은 처음 맞은 적에게 피해를 줍니다.",
        "조준 방향으로 즉시 관통 화살을 발사합니다. 많은 일반 적을 뚫지만 관통할수록 피해가 감소해 무리 정리에 적합합니다.",
        "조준 방향의 넓은 부채꼴에 화살 9발을 동시에 발사합니다. 가까이 모인 적이나 넓게 퍼진 무리를 상대하기 좋습니다.",
        "이동하며 최대 1초 충전하고 떼면 고화력 화살을 발사합니다. 오래 충전할수록 피해·사거리·크기가 증가하며 최대 10명을 추가 관통합니다.",
        "조준 방향으로 폭발 화살을 발사합니다. 처음 맞은 적 또는 최대 사거리에서 폭발해 주변의 모든 적을 공격합니다.",
        "사거리 안의 적을 자동 추적하는 화살을 발사합니다. 적중 후 아직 맞지 않은 가까운 적에게 연속으로 도탄합니다.",
        "커서 위치에 일정 시간 화살비를 내립니다. 범위 안의 적을 반복 공격하므로 오래 머무는 적에게 효과적입니다.",
        "조준 방향으로 전방 구르기하며 출발 지점에 덫을 설치합니다. 덫은 적이 접근하면 폭발해 주변을 공격하고 둔화시킵니다.",
        "조준 반대 방향으로 빠르게 물러나며 조준 방향으로 화살을 발사합니다. 이동 중에도 피해를 받을 수 있습니다."};
    constexpr std::array<std::array<std::string_view, 8>, kCombatSkillCount>
        skill_upgrade_names{{
            {{"연속 추가 화살", "추가 관통", "적중 분열", "출혈 화살",
              "화상 화살", "둔화 쿨타임 회수", "귀환 화살", "액티브 연계 사격"}},
            {{"후속 화살", "사거리 끝 분열", "관통 출혈", "피해 궤적",
              "관통 연쇄 사격", "적 밀어 정렬", "화상 전달", "빠른 재사용"}},
            {{"2차 부채", "적중 분열", "추가 관통", "후방 사격",
              "출혈 부채", "화상 전달", "화살 추가", "빗나감 재추적"}},
            {{"과충전 폭발", "빠른 충전", "즉시 사격 강화", "추가 관통",
              "완전 충전 출혈", "관통 분열", "화상 폭발", "다중 처치 쿨타임 회수"}},
            {{"재폭발", "소형 폭탄", "파편 폭발", "화상 지대",
              "출혈 연쇄 폭발", "폭발 흡인", "액티브 연계 표식", "빠른 재사용"}},
            {{"귀환 도탄", "분기 도탄", "출혈 도탄 연장", "화상 전달",
              "처치 소형 화살", "처치 연쇄 갱신", "도탄 쿨타임 회수", "빠른 재사용"}},
            {{"2차 화살비", "첫 타격 흡인", "반복 적중 출혈", "화상 지대",
              "추적 화살", "둔화 지대", "처치 추적 화살", "추가 타격과 둔화"}},
            {{"연속 덫", "착지 둔화", "덫 재활성", "출혈 덫",
              "화상 덫", "흡인 덫", "액티브 연계 표식", "처치 덫"}},
            {{"세 갈래 사격", "출발점 덫", "둔화 궤적", "출혈 추적 화살",
              "착지 충격", "다음 스킬 쿨타임 회수", "다중 적중 회복", "추가 후퇴"}}
        }};
    constexpr std::array<std::array<std::string_view, 8>, kCombatSkillCount>
        skill_upgrade_descriptions{{
            {{"기본 공격 3회마다 잠시 후 70% 위력의 기본 공격을 한 번 더 발사합니다. 추가 공격도 공격 횟수와 다른 기본 공격 강화를 적용합니다.",
              "기본 화살이 첫 적에게 멈추지 않고 뒤의 적 한 명까지 추가로 관통합니다.",
              "기본 화살이 처음 적중하면 그 지점에서 좌우로 약한 화살 2발이 갈라져 나갑니다.",
              "세 번째 기본 화살마다 적중한 대상에게 출혈을 부여합니다.",
              "네 번째 기본 화살마다 적중한 대상에게 화상을 부여합니다. 출혈 화살과 함께 발동할 수 있습니다.",
              "기본 화살의 첫 대상이 잠시 둔화됩니다. 동시에 남은 쿨타임이 가장 긴 액티브 스킬이 조금 회복됩니다.",
              "아무 적도 맞히지 못한 기본 화살이 한 번 되돌아오며 돌아오는 경로의 적을 공격합니다.",
              "액티브 스킬 사용 후 3초 안에 기본 공격하면 다음 공격이 세 갈래 화살로 바뀝니다."}},
            {{"관통 사격을 발사한 직후 새 대상을 추적하는 강한 후속 화살을 한 발 더 발사합니다.",
              "관통 화살이 최대 사거리에 도달하면 주변 적을 추적하는 화살 2발이 갈라져 나갑니다.",
              "관통 사격에 맞은 모든 적에게 출혈을 2중첩 부여합니다.",
              "관통 화살이 지나간 경로에 잠시 피해와 둔화를 주는 궤적이 남습니다.",
              "일반 적 3명을 관통할 때마다 진행 지점에서 좌우로 추가 화살을 발사합니다.",
              "관통 사격에 맞은 일반 적을 화살 진행 방향으로 밀어 뒤의 적과 한 줄로 모읍니다.",
              "첫 대상에게 화상을 부여하고, 화살이 다음 대상을 관통할 때 남은 화상을 전달합니다.",
              "관통 사격의 기본 쿨타임이 감소해 더 자주 사용할 수 있습니다."}},
            {{"다중 사격 후 잠시 뒤 같은 방향으로 약한 두 번째 부채 사격을 발사합니다.",
              "각 화살이 처음 적중한 지점에서 좌우로 약한 화살이 갈라져 나갑니다.",
              "다중 사격의 각 화살이 첫 적을 뚫고 뒤의 적 한 명까지 추가로 관통합니다.",
              "다중 사격과 동시에 등 뒤 방향으로도 화살 3발을 발사합니다.",
              "한 번의 다중 사격이 각 대상에게 처음 적중할 때 출혈을 1중첩 부여합니다.",
              "원래 화살이 맞힌 적을 태우고, 근처의 화상 없는 적에게 추적 화살과 화상을 최대 3회 전달합니다.",
              "부채꼴 바깥쪽에 기본 위력의 화살 2발을 더해 한 번에 11발을 발사합니다.",
              "원본과 강화로 생성된 화살이 빗나가면 약한 화살로 근처의 적을 한 번 추적합니다."}},
            {{"최대 충전 시간이 1.4초로 늘어나지만 완전 충전 피해가 크게 강해지고 화살 끝에서 폭발합니다.",
              "충전 속도가 빨라져 같은 위력의 화살을 더 짧게 눌러 발사할 수 있습니다.",
              "충전하지 않고 바로 발사해도 더 강한 피해를 줍니다. 오래 충전할수록 피해는 계속 증가합니다.",
              "추가 관통 수가 4 늘어나 최대 14명의 적을 추가 관통합니다.",
              "충전 화살이 출혈을 3중첩 부여합니다. 완전 충전으로 출혈이 가득한 적을 맞히면 추가 피해를 줍니다.",
              "보스를 처음 맞히거나 일반 적 3명을 관통하면 적중 지점에서 여덟 방향으로 강한 화살이 갈라집니다.",
              "첫 적중 대상을 태우고 그 주변을 폭발시켜 모여 있는 적을 함께 공격합니다.",
              "완전 충전 한 발로 일반 적 3명 이상을 처치하면 충전 사격의 남은 쿨타임 일부를 돌려받습니다."}},
            {{"주 폭발이 끝난 뒤 같은 위치에서 더 약한 폭발이 한 번 추가로 일어납니다.",
              "주 폭발 주변에 소형 폭탄 3개가 생겨 흩어진 적을 추가로 공격합니다.",
              "주 폭발 지점에서 여덟 방향으로 파편 화살을 발사해 바깥의 적까지 공격합니다.",
              "주 폭발 위치에 잠시 불장판이 남아 안의 적을 반복 공격하고 화상을 부여합니다.",
              "주 폭발에 맞은 적마다 출혈을 부여하고 작은 혈폭을 일으킵니다. 한 번의 시전당 최대 8회 발생합니다.",
              "폭발 직전에 주변 일반 적을 중심으로 끌어당겨 폭발 범위 안에 모읍니다.",
              "직접 맞히면 즉시 추가 폭발을 일으키고 표식을 남깁니다. 살아남은 적을 다른 공격으로 맞히면 다시 폭발합니다.",
              "폭발 화살의 기본 쿨타임이 감소해 더 자주 사용할 수 있습니다."}},
            {{"마지막 도탄 뒤 화살이 플레이어에게 돌아오며, 오는 길에 이전 대상들을 한 번 더 공격합니다.",
              "첫 번째 도탄 지점에서 화살이 세 갈래 연쇄로 나뉘어 서로 다른 적을 추적합니다.",
              "맞은 적에게 출혈을 부여합니다. 출혈 적을 맞힐수록 이번 화살의 남은 도탄 횟수가 최대 3회 늘어납니다.",
              "맞은 적을 태우고, 다음 도탄 대상에게 현재 남은 화상을 그대로 복제합니다.",
              "도탄 화살로 적을 처치하면 소형 화살 3발이 생깁니다. 한 번의 시전당 최대 9발 생성됩니다.",
              "원본 도탄 화살로 적을 처치하면 새 화살이 생겨 주변의 적 3명에게 다시 도탄합니다.",
              "원본 화살이 도탄할 때마다 다른 액티브 중 남은 쿨타임이 가장 긴 스킬을 조금씩 회복합니다.",
              "도탄 화살의 기본 쿨타임이 감소해 더 자주 사용할 수 있습니다."}},
            {{"첫 화살비가 시작된 뒤 시전 방향 앞쪽에 더 작고 짧은 두 번째 화살비가 생깁니다.",
              "첫 피해가 발생할 때 범위 안의 일반 적을 중심으로 끌어당깁니다.",
              "같은 화살비가 한 적을 세 번 맞힐 때마다 강한 출혈을 부여합니다.",
              "맞은 적을 태우고 그 자리에 작은 불장판을 만듭니다. 한 번의 시전당 최대 4개 생성됩니다.",
              "화살비가 피해를 줄 때마다 범위 근처의 가장 가까운 적에게 추적 화살을 발사합니다.",
              "화살비 안에 머무는 적에게 강한 둔화를 피해 주기마다 새로 부여합니다.",
              "화살비 안에서 적이 죽으면 범위 밖의 가까운 적에게 추적 화살을 발사합니다. 최대 6회 발동합니다.",
              "화살비가 두 번 더 공격하고, 종료된 자리에 강한 둔화 지대를 남깁니다."}},
            {{"구르는 경로를 따라 덫 3개를 설치합니다. 각 덫은 기본 덫보다 약하지만 따로 발동합니다.",
              "구르기가 끝난 위치에 강한 둔화 지대를 만들어 추격해 오는 적을 크게 늦춥니다.",
              "한 번 폭발한 덫이 사라지지 않고 잠시 뒤 다시 활성화되어 한 번 더 발동할 수 있습니다.",
              "덫이 폭발할 때 맞은 모든 적에게 출혈을 3중첩 부여합니다.",
              "덫이 폭발할 때 화상을 부여하고 작은 불장판을 남깁니다.",
              "덫이 폭발하기 직전 주변 적을 끌어당기고, 폭발에 맞은 적을 강하게 둔화시킵니다.",
              "덫을 발동시킨 적에게 즉시 추가 피해를 주고 표식을 남깁니다. 살아남은 적을 다른 공격으로 맞히면 폭발합니다.",
              "덫으로 적을 처치하면 가까운 적 옆에 소형 덫을 만듭니다. 한 번의 시전당 최대 3개 생성됩니다."}},
            {{"후퇴 사격의 한 발이 세 갈래 화살로 바뀌어 더 넓은 범위를 공격합니다.",
              "후퇴를 시작한 위치에 소형 덫을 남겨 따라오는 적을 공격합니다.",
              "후퇴한 경로에 강한 둔화 지대를 남겨 쫓아오는 적의 이동을 크게 늦춥니다.",
              "맞은 적에게 출혈을 부여하고 주변 적에게 추적 화살을 발사합니다. 최대 3회 발동합니다.",
              "후퇴가 끝난 지점에서 충격파를 일으켜 주변 적을 공격하고 일반 적을 밀어냅니다.",
              "후퇴 사격 후 3초 안에 다른 액티브를 사용하면 그 스킬의 남은 쿨타임 일부를 돌려받습니다.",
              "보스를 맞히거나 일반 적 3명 이상을 맞히면 최대 체력의 일부를 회복합니다.",
              "첫 후퇴 직후 한 번 더 물러나며 조준 방향으로 약한 추가 화살을 발사합니다."}}
        }};
    static_assert([] {
        for (const auto description : skill_descriptions)
            if (description.empty() || description.size() > 430) return false;
        for (const auto &skill : skill_upgrade_descriptions)
            for (const auto description : skill)
                if (description.empty() || description.size() > 400) return false;
        return true;
    }(), "Card descriptions must fit the UTF-8 UI text buffer.");
    constexpr std::array<std::string_view, kStatCount> stat_names{
        "최대 체력", "이동속도", "공격력", "공격속도", "쿨타임 감소", "자석 반경"};
    const auto key_name = [](std::uint16_t key) {
        if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))
            return std::string(1, static_cast<char>(key));
        if (key >= 0x70 && key <= 0x87)
            return std::format("F{}", key - 0x6F);
        return std::format("VK {}", key);
    };

    if (probe.phase == SessionPhase::MainMenu ||
        (probe.phase == SessionPhase::Paused && probe.menu_page == 6))
    {
        if (probe.phase == SessionPhase::MainMenu)
            add_ui(UiModel::Kind::Text, {760, 120}, {400, 80}, 0xFFFFFFFFu,
                   "PROJECT HS", 1.0f, 54);
        if (probe.menu_page == 1)
        {
            const auto selected = std::min<std::size_t>(impl_->collection_skill_index,
                                                         kCombatSkillCount - 1);
            add_ui(UiModel::Kind::Panel, {160, 70}, {1'600, 930}, 0xD0202430u,
                   "스킬 도감");
            add_ui(UiModel::Kind::Text, {210, 95}, {360, 42}, 0xFFFFFFFFu,
                   "스킬 도감", 1.0f, 34);
            for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
            {
                add_ui(UiModel::Kind::Button,
                       {210, 150.0f + static_cast<float>(skill) * 70.0f},
                       {360, 56}, selected == skill ? 0xFF507098u : 0xFF34495Eu,
                       skill_names[skill], 1.0f, 22);
            }

            add_ui(UiModel::Kind::Text, {620, 105}, {1'090, 46}, 0xFFFFFFFFu,
                   skill_names[selected], 1.0f, 32);
            add_ui(UiModel::Kind::Text, {620, 160}, {1'090, 125}, 0xFFE2E8F0u,
                   skill_descriptions[selected], 1.0f, 22);
            add_ui(UiModel::Kind::Text, {620, 285}, {1'090, 35}, 0xFFB8C2D0u,
                   "선택 가능한 강화 8종", 1.0f, 20);
            for (std::size_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
            {
                const auto column = static_cast<float>(upgrade % 2);
                const auto row = static_cast<float>(upgrade / 2);
                add_ui(UiModel::Kind::Panel,
                       {620.0f + column * 545.0f, 330.0f + row * 145.0f},
                       {520, 125}, 0xFF2D4058u,
                       std::format("강화 {} · {}\n{}", upgrade + 1,
                                   skill_upgrade_names[selected][upgrade],
                                   skill_upgrade_descriptions[selected][upgrade]),
                       1.0f, 19);
            }
            add_ui(UiModel::Kind::Button, {210, 900}, {360, 56}, 0xFF3A5068u,
                   "돌아가기");
        }
        else if (probe.menu_page == 2 || probe.menu_page == 6)
        {
            const auto &settings = impl_->config.settings;
            const auto enabled = [](bool value) { return value ? "켜짐" : "꺼짐"; };
            add_ui(UiModel::Kind::Panel, {450, 180}, {1'020, 790}, 0xD0202430u,
                   "설정");
            constexpr std::array<float, 8> left_y{250, 320, 390, 460, 530, 600, 670, 740};
            const std::array left_text{
                std::format("화면: {}", settings.borderless ? "테두리 없음" : "창"),
                std::format("VSync: {}", enabled(settings.vsync)),
                std::format("프레임 제한: {}", settings.frame_cap == 0
                                                   ? std::string("무제한")
                                                   : std::to_string(settings.frame_cap)),
                std::format("렌더 스케일: {}%", settings.render_scale_percent),
                std::format("그림자: {}", settings.shadow_resolution),
                std::format("파티클: {}%", settings.particle_percentage),
                std::format("Bloom: {}", enabled(settings.bloom)),
                std::format("외곽선: {}", enabled(settings.outline))};
            for (std::size_t index = 0; index < left_text.size(); ++index)
                add_ui(UiModel::Kind::Button, {500, left_y[index]}, {420, 56},
                       0xFF34495Eu, left_text[index], 1.0f, 23);

            constexpr std::array<float, 4> volume_y{250, 320, 390, 460};
            const std::array volume_text{
                std::format("-  Master {:3}%  +", std::lround(settings.master_volume * 100)),
                std::format("-  BGM {:3}%  +", std::lround(settings.bgm_volume * 100)),
                std::format("-  SFX {:3}%  +", std::lround(settings.sfx_volume * 100)),
                std::format("-  UI {:3}%  +", std::lround(settings.ui_volume * 100))};
            for (std::size_t index = 0; index < volume_text.size(); ++index)
                add_ui(UiModel::Kind::Button, {1'000, volume_y[index]}, {420, 56},
                       0xFF34495Eu, volume_text[index], 1.0f, 23);

            constexpr std::array<std::string_view, 4> slots{"Q", "W", "E", "R"};
            for (std::size_t slot = 0; slot < slots.size(); ++slot)
            {
                const auto waiting = impl_->pending_rebind_slot == slot;
                add_ui(UiModel::Kind::Button,
                       {1'000, 550.0f + static_cast<float>(slot) * 70.0f}, {420, 56},
                       waiting ? 0xFF8A5A30u : 0xFF34495Eu,
                       waiting ? std::format("{}: 새 키 입력...", slots[slot])
                               : std::format("{}: {}", slots[slot],
                                             key_name(settings.skill_virtual_keys[slot])),
                       1.0f, 23);
            }
            add_ui(UiModel::Kind::Text, {1'000, 830}, {420, 32}, 0xFFFFFFFFu,
                   "사용 중인 키 선택 시 서로 교환", 1.0f, 20);
            add_ui(UiModel::Kind::Button, {760, 870}, {400, 64}, 0xFF3A5068u,
                   "돌아가기");
        }
        else
        {
            constexpr std::array<std::string_view, 4> labels{
                "시작", "컬렉션", "설정", "종료"};
            for (std::size_t index = 0; index < labels.size(); ++index)
            {
                add_ui(UiModel::Kind::Button,
                       {760.0f, 270.0f + static_cast<float>(index) * 150.0f},
                       {400, 92}, 0xFF34495Eu, labels[index], 1.0f, 34);
            }
        }
    }
    else if (!(probe.phase == SessionPhase::Paused && probe.menu_page >= 3 &&
               probe.menu_page <= 5))
    {
        const auto time = probe.final_boss_spawned ? probe.boss_fight_ticks
                                                   : probe.growth_ticks;
        add_ui(UiModel::Kind::Text, {32, 32}, {680, 50}, 0xFFFFFFFFu,
               std::format("LV {}  {:02}:{:02}  적 {}", probe.level,
                           (time / 60) / 60, (time / 60) % 60,
                           probe.normal_enemy_count));
        add_ui(UiModel::Kind::Bar, {32, 92}, {420, 30}, 0xFFE85050u,
               std::format("HP {}/{}", probe.health, probe.max_health),
               static_cast<float>(std::max(probe.health, 0)) /
                   static_cast<float>(probe.max_health));
        add_ui(UiModel::Kind::Bar, {32, 132}, {420, 24}, 0xFFFFC840u,
               std::format("XP {}/{}", probe.experience, probe.experience_to_next),
               static_cast<float>(probe.experience) /
                   static_cast<float>(probe.experience_to_next));
        add_ui(UiModel::Kind::Button, {530, 948}, {180, 92}, 0xFF31425Au,
               std::format("LMB\n{} Lv{}", skill_names[0], probe.skill_levels[0]),
               1.0f, 22);
        std::array<std::string, 4> keys;
        for (std::size_t slot = 0; slot < keys.size(); ++slot)
            keys[slot] = key_name(impl_->config.settings.skill_virtual_keys[slot]);
        for (std::size_t slot = 0; slot < keys.size(); ++slot)
        {
            const auto skill = impl_->player.loadout[slot];
            const auto text = skill == SkillKind::Count
                ? std::format("{}\n-", keys[slot])
                : std::format("{}\n{} Lv{}  {:.1f}s", keys[slot],
                              skill_names[static_cast<std::size_t>(skill)],
                              probe.skill_levels[static_cast<std::size_t>(skill)],
                              static_cast<float>(probe.cooldown_ticks[
                                  static_cast<std::size_t>(skill) - 1]) / 60.0f);
            add_ui(UiModel::Kind::Button,
                   {730.0f + static_cast<float>(slot) * 190.0f, 948},
                   {180, 92}, 0xFF31425Au, text, 1.0f, 20);
        }
        add_ui(UiModel::Kind::Text, {1'520, 32}, {360, 50}, 0xFFFFFFFFu,
               std::format("유물 {:03X}", probe.relic_mask));

        for (const auto &wave : impl_->data.waves)
        {
            const auto start = static_cast<Tick>(wave.minute) * Seconds(60.0f);
            if (probe.growth_ticks < start &&
                start - probe.growth_ticks <= Seconds(5.0f))
            {
                const auto seconds = (start - probe.growth_ticks + 59) / 60;
                add_ui(UiModel::Kind::Text, {610, 180}, {700, 64}, 0xFFFFA030u,
                       std::format("대규모 웨이브까지 {}초", seconds), 1.0f, 36);
                break;
            }
            if (probe.growth_ticks >= start &&
                probe.growth_ticks < start + wave.duration_ticks)
            {
                add_ui(UiModel::Kind::Text, {610, 180}, {700, 64}, 0xFFFF4040u,
                       "대규모 웨이브 발생", 1.0f, 36);
                break;
            }
        }

        std::vector<const EnemyActor *> bosses;
        for (const auto &enemy : impl_->enemies)
        {
            if (enemy.boss && !enemy.dead) bosses.push_back(&enemy);
        }
        std::ranges::sort(bosses,
                          [](const EnemyActor *left, const EnemyActor *right) {
            const auto left_final = left->boss == BossKind::Final;
            const auto right_final = right->boss == BossKind::Final;
            return left_final != right_final ? left_final :
                   left->spawned_tick < right->spawned_tick;
        });
        constexpr std::array<std::string_view, 3> boss_names{
            "5분 보스", "10분 보스", "최종 보스"};
        for (std::size_t index = 0; index < bosses.size(); ++index)
        {
            const auto &boss = *bosses[index];
            add_ui(UiModel::Kind::Bar,
                   {560, 32.0f + static_cast<float>(index) * 46.0f}, {800, 38},
                   0xFFE04070u,
                   std::format("{} {}/{}",
                               boss_names[static_cast<std::size_t>(*boss.boss)],
                               boss.health, boss.max_health),
                   static_cast<float>(std::max(boss.health, 0)) /
                       static_cast<float>(boss.max_health));
        }
    }

    if (probe.phase == SessionPhase::CardSelection ||
        probe.phase == SessionPhase::RelicSelection)
    {
        add_ui(UiModel::Kind::Panel, {300, 160}, {1'320, 760}, 0xE0181D28u,
               probe.phase == SessionPhase::RelicSelection ? "유물 선택" : "레벨업");
        for (std::size_t index = 0; index < probe.card_count; ++index)
        {
            const auto card = probe.cards[index];
            std::string label;
            if (card.kind == CardKind::LearnSkill)
                label = std::format("{}\n{}", skill_names[card.subject],
                                    skill_descriptions[card.subject]);
            else if (card.kind == CardKind::Relic)
                label = std::format(
                    "{}\n{}", impl_->data.relic_names[card.subject].data(),
                    impl_->data.relic_rules[card.subject].data());
            else if (card.kind == CardKind::BonusStatPoint)
                label = "스탯 포인트 +1";
            else
                label = std::format("{}\n강화 {} · {}\n{}", skill_names[card.subject],
                                    static_cast<unsigned>(card.upgrade) + 1,
                                    skill_upgrade_names[card.subject][card.upgrade],
                                    skill_upgrade_descriptions[card.subject][card.upgrade]);
            add_ui(UiModel::Kind::Button,
                   {360.0f + static_cast<float>(index) * 420.0f, 300},
                   {360, 420}, 0xFF334A64u, label, 1.0f, 24);
        }
        add_ui(UiModel::Kind::Button, {760, 790}, {400, 72}, 0xFF5A4050u,
               std::format("재추첨 {}",
                           probe.phase == SessionPhase::RelicSelection
                               ? probe.relic_rerolls_remaining
                               : probe.level_rerolls_remaining));
    }
    else if (probe.phase == SessionPhase::StatAllocation)
    {
        add_ui(UiModel::Kind::Panel, {300, 160}, {1'320, 760}, 0xE0181D28u,
               std::format("스탯 배분  남은 포인트 {}", probe.pending_stat_points));
        for (std::size_t index = 0; index < stat_names.size(); ++index)
        {
            add_ui(UiModel::Kind::Button,
                   {390.0f + static_cast<float>(index % 3) * 400.0f,
                    310.0f + static_cast<float>(index / 3) * 260.0f},
                   {340, 180}, 0xFF334A64u,
                   std::format("{}\n{}/10", stat_names[index], probe.stat_points[index]));
        }
    }
    else if (probe.phase == SessionPhase::Paused)
    {
        if (probe.menu_page == 0)
        {
            add_ui(UiModel::Kind::Panel, {660, 300}, {600, 480}, 0xE0181D28u,
                   "일시정지", 1.0f, 38);
            add_ui(UiModel::Kind::Button, {760, 420}, {400, 72}, 0xFF34495Eu,
                   "계속", 1.0f, 28);
            add_ui(UiModel::Kind::Button, {760, 520}, {400, 72}, 0xFF34495Eu,
                   "설정", 1.0f, 28);
            add_ui(UiModel::Kind::Button, {760, 620}, {400, 72}, 0xFF5A4050u,
                   "게임 종료", 1.0f, 28);
            add_ui(UiModel::Kind::Text, {760, 710}, {400, 32}, 0xFFB8C2D0u,
                   "Esc: 계속", 1.0f, 20);
        }
        else if (probe.menu_page >= 3 && probe.menu_page <= 5)
        {
            add_ui(UiModel::Kind::Panel, {260, 80}, {1'400, 920}, 0xF0181D28u,
                   "");
            constexpr std::array<std::string_view, 3> tabs{
                "능력치·피해", "스킬·강화", "유물"};
            for (std::size_t index = 0; index < tabs.size(); ++index)
            {
                add_ui(UiModel::Kind::Button,
                       {350.0f + static_cast<float>(index) * 300.0f, 140},
                       {280, 58}, probe.menu_page == index + 3 ? 0xFF507098u
                                                               : 0xFF34495Eu,
                       tabs[index], 1.0f, 24);
            }
            add_ui(UiModel::Kind::Button, {1'520, 140}, {120, 58}, 0xFF5A4050u,
                   "닫기", 1.0f, 22);
            add_ui(UiModel::Kind::Text, {300, 95}, {1'300, 38}, 0xFFFFFFFFu,
                   "캐릭터 정보  ·  Tab 또는 Esc로 닫기", 1.0f, 26);

            if (probe.menu_page == 3)
            {
                const auto attack = impl_->EffectiveAttack();
                const auto attack_speed = impl_->EffectiveAttackSpeed();
                const auto move_speed = impl_->EffectiveMoveSpeed();
                const auto magnet_radius = impl_->EffectiveMagnetRadius();
                const auto cooldown_reduction =
                    0.03f * probe.stat_points[
                                static_cast<std::size_t>(StatKind::CooldownReduction)];
                add_ui(UiModel::Kind::Text, {350, 235}, {570, 470}, 0xFFFFFFFFu,
                       std::format(
                           "상세 능력치\n\n"
                           "레벨                 {}\n"
                           "현재 체력             {} / {}\n"
                           "공격력                {:.1f}\n"
                           "기본 공격 피해        {}\n"
                           "기본 공격속도         {:.3f}회/초\n"
                           "이동속도              {:.2f}m/초\n"
                           "쿨타임 감소           {:.0f}%\n"
                           "자석 반경             {:.1f}m",
                           probe.level, probe.health, probe.max_health, attack,
                           RoundDamage(attack * impl_->data.skills[0].damage_coefficient),
                           attack_speed, move_speed, cooldown_reduction * 100.0f,
                           magnet_radius),
                       1.0f, 23);
                add_ui(UiModel::Kind::Text, {350, 725}, {570, 160}, 0xFFFFFFFFu,
                       std::format(
                           "투자 포인트\n"
                           "체력 {} · 이동 {} · 공격 {}\n"
                           "공속 {} · 쿨감 {} · 자석 {}",
                           probe.stat_points[0], probe.stat_points[1],
                           probe.stat_points[2], probe.stat_points[3],
                           probe.stat_points[4], probe.stat_points[5]),
                       1.0f, 23);

                add_ui(UiModel::Kind::Text, {990, 235}, {570, 330}, 0xFFFFFFFFu,
                       std::format(
                    "현재 전투 기록\n\n총 피해              {}\n직접 피해            {}\n"
                    "파생 효과 피해        {}\n지속 피해            {}\n받은 피해            {}\n"
                    "회복량                {}\n처치 수              {}",
                    probe.damage_dealt, probe.balance.direct_damage,
                    probe.balance.derived_damage, probe.balance.damage_over_time,
                    probe.damage_taken, probe.healing, probe.kills),
                       1.0f, 23);
                add_ui(UiModel::Kind::Text, {990, 585}, {570, 42}, 0xFFFFFFFFu,
                       "스킬별 누적 피해", 1.0f, 23);
                std::size_t damage_row{};
                for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
                {
                    if (probe.skill_levels[skill] == 0 && probe.damage_by_skill[skill] == 0)
                        continue;
                    add_ui(UiModel::Kind::Text,
                           {990, 635.0f + static_cast<float>(damage_row) * 30.0f},
                           {570, 28}, 0xFFE2E8F0u,
                           std::format("{}  {}", skill_names[skill],
                                       probe.damage_by_skill[skill]),
                           1.0f, 20);
                    ++damage_row;
                }
            }
            else if (probe.menu_page == 4)
            {
                constexpr std::array<std::string_view, 4> slot_names{"Q", "W", "E", "R"};
                auto selected = std::min<std::size_t>(impl_->character_skill_index,
                                                       kCombatSkillCount - 1);
                if (probe.skill_levels[selected] == 0) selected = 0;
                for (std::size_t skill = 0; skill < kCombatSkillCount; ++skill)
                {
                    if (probe.skill_levels[skill] == 0) continue;
                    add_ui(UiModel::Kind::Button,
                           {350, 240.0f + static_cast<float>(skill) * 78.0f},
                           {360, 64}, selected == skill ? 0xFF507098u : 0xFF34495Eu,
                           std::format("{}  Lv{}", skill_names[skill],
                                       probe.skill_levels[skill]),
                           1.0f, 21);
                }

                add_ui(UiModel::Kind::Text, {780, 200}, {780, 22}, 0xFFB8C2D0u,
                       "두 슬롯을 차례로 선택하면 스킬을 이동하거나 교환합니다.",
                       1.0f, 16);

                for (std::size_t slot = 0; slot < probe.skill_loadout.size(); ++slot)
                {
                    const auto skill = probe.skill_loadout[slot];
                    const auto label = skill < SkillKind::Count
                        ? std::format("{}  {}", slot_names[slot],
                                      skill_names[static_cast<std::size_t>(skill)])
                        : std::format("{}  비어 있음", slot_names[slot]);
                    add_ui(UiModel::Kind::Button,
                           {780.0f + static_cast<float>(slot) * 195.0f, 225.0f},
                           {180, 54}, impl_->character_slot_source == slot
                                          ? 0xFFB87828u
                                          : 0xFF34495Eu,
                           label, 1.0f, 18);
                }

                const auto &definition = impl_->data.skills[selected];
                const auto damage = RoundDamage(
                    impl_->EffectiveAttack() * definition.damage_coefficient);
                std::string parameters = selected == 0
                    ? std::format("1발 피해 {}  ·  공격속도 {:.3f}회/초  ·  사거리 {:.1f}m",
                                  damage, impl_->EffectiveAttackSpeed(), definition.range)
                    : std::format("표기 피해 {}  ·  쿨타임 {:.2f}초  ·  사거리 {:.1f}m",
                                  damage,
                                  static_cast<float>(impl_->CooldownTicks(
                                      static_cast<SkillKind>(selected))) / 60.0f,
                                  definition.range);
                if (definition.area_radius > 0.0f)
                    parameters += std::format("  ·  범위 {:.1f}m", definition.area_radius);
                if (definition.duration_seconds > 0.0f)
                    parameters += std::format("  ·  지속 {:.1f}초", definition.duration_seconds);
                if (definition.projectile_count > 1)
                    parameters += std::format("  ·  발사 {}개", definition.projectile_count);
                if (definition.pierce_count > 0)
                    parameters += std::format("  ·  추가 관통 {}", definition.pierce_count);

                add_ui(UiModel::Kind::Text, {780, 285}, {780, 58}, 0xFFFFFFFFu,
                       std::format("{}  Lv{}  ·  누적 피해 {}", skill_names[selected],
                                   probe.skill_levels[selected],
                                   probe.damage_by_skill[selected]),
                       1.0f, 28);
                add_ui(UiModel::Kind::Text, {780, 345}, {780, 110}, 0xFFFFFFFFu,
                       skill_descriptions[selected], 1.0f, 22);
                add_ui(UiModel::Kind::Text, {780, 460}, {780, 60}, 0xFFFFFFFFu,
                       parameters, 1.0f, 20);

                std::size_t upgrade_row{};
                for (std::uint8_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
                {
                    if (!HasUpgrade(probe.upgrade_masks[selected], upgrade + 1)) continue;
                    add_ui(UiModel::Kind::Button,
                           {780, 530.0f + static_cast<float>(upgrade_row) * 98.0f},
                           {780, 86}, 0xFF2D4058u,
                            std::format("강화 {} · {}  ·  기여 피해 {}\n{}",
                                        static_cast<unsigned>(upgrade) + 1,
                                        skill_upgrade_names[selected][upgrade],
                                        probe.balance.upgrade_damage[selected][upgrade],
                                        skill_upgrade_descriptions[selected][upgrade]),
                           1.0f, 19);
                    ++upgrade_row;
                }
                if (upgrade_row == 0)
                    add_ui(UiModel::Kind::Text, {780, 540}, {780, 50}, 0xFFB8C2D0u,
                           "현재 선택한 강화가 없습니다.", 1.0f, 21);
            }
            else
            {
                std::size_t relic_count{};
                for (std::size_t relic = 0; relic < kRelicCount; ++relic)
                {
                    if (!HasRelic(probe.relic_mask, static_cast<RelicKind>(relic))) continue;
                    const auto column = relic_count / 6;
                    const auto row = relic_count % 6;
                    add_ui(UiModel::Kind::Button,
                           {350.0f + static_cast<float>(column) * 640.0f,
                            235.0f + static_cast<float>(row) * 122.0f},
                           {580, 108}, 0xFF2D4058u,
                           std::format("{}\n{}",
                                       impl_->data.relic_names[relic].data(),
                                       impl_->data.relic_rules[relic].data()),
                           1.0f, 18);
                    ++relic_count;
                }
                if (relic_count == 0)
                    add_ui(UiModel::Kind::Text, {350, 250}, {1'200, 80}, 0xFFB8C2D0u,
                           "현재 획득한 유물이 없습니다.", 1.0f, 24);
            }
        }
    }
    else if (probe.phase == SessionPhase::Victory || probe.phase == SessionPhase::Defeat)
    {
        add_ui(UiModel::Kind::Panel, {500, 100}, {920, 880}, 0xE0181D28u,
               "");
        add_ui(UiModel::Kind::Text, {550, 130}, {820, 70}, 0xFFFFFFFFu,
               std::format("{}  레벨 {}  처치 {}  보스전 {:02}:{:02}",
                           probe.phase == SessionPhase::Victory ? "승리" : "패배",
                           probe.level, probe.kills,
                           (probe.boss_fight_ticks / 60) / 60,
                           (probe.boss_fight_ticks / 60) % 60), 1.0f, 32);
        add_ui(UiModel::Kind::Text, {550, 210}, {820, 60}, 0xFFFFFFFFu,
               std::format("피해 {}  피격 {}  회복 {}",
                           probe.damage_dealt, probe.damage_taken, probe.healing), 1.0f, 28);
        std::array<std::pair<std::uint64_t, std::size_t>, kCombatSkillCount> ranking{};
        for (std::size_t index = 0; index < ranking.size(); ++index)
            ranking[index] = {probe.damage_by_skill[index], index};
        std::ranges::sort(ranking, std::greater{},
                          [](const auto &entry) { return entry.first; });
        std::string damage_table = "전투 피해 통계\n";
        for (const auto [damage, index] : ranking)
        {
            if (damage == 0) continue;
            damage_table += std::format("{}  {}\n", skill_names[index], damage);
        }
        if (damage_table == "전투 피해 통계\n") damage_table += "기록 없음";
        add_ui(UiModel::Kind::Text, {550, 285}, {820, 260}, 0xFFFFFFFFu,
               damage_table, 1.0f, 23);
        add_ui(UiModel::Kind::Text, {550, 560}, {820, 60}, 0xFFFFFFFFu,
               std::format("강화 기본~4: {:02X}/{:02X}/{:02X}/{:02X}/{:02X}",
                           probe.upgrade_masks[0], probe.upgrade_masks[1],
                           probe.upgrade_masks[2], probe.upgrade_masks[3],
                           probe.upgrade_masks[4]), 1.0f, 26);
        add_ui(UiModel::Kind::Text, {550, 630}, {820, 60}, 0xFFFFFFFFu,
               std::format("강화 5~8: {:02X}/{:02X}/{:02X}/{:02X}  유물 {:03X}",
                           probe.upgrade_masks[5], probe.upgrade_masks[6],
                           probe.upgrade_masks[7], probe.upgrade_masks[8],
                           probe.relic_mask), 1.0f, 26);
        add_ui(UiModel::Kind::Text, {550, 710}, {820, 80}, 0xFFFFFFFFu,
               std::format("스탯 {}/{}/{}/{}/{}/{}  시드 {}",
                           probe.stat_points[0], probe.stat_points[1],
                           probe.stat_points[2], probe.stat_points[3],
                           probe.stat_points[4], probe.stat_points[5], impl_->config.seed),
               1.0f, 25);
        add_ui(UiModel::Kind::Text, {550, 900}, {820, 50}, 0xFFFFFFFFu,
               "클릭: 메인 메뉴", 1.0f, 26);
    }
    return complete;
}

std::span<const PresentationEvent> GameSimulation::PendingPresentationEvents() const noexcept
{
    return impl_->presentation_events;
}

void GameSimulation::ClearPresentationEvents() noexcept
{
    impl_->presentation_events.clear();
}

std::span<const UiCommand> GameSimulation::PendingUiCommands() const noexcept
{
    return impl_->ui_commands;
}

void GameSimulation::ClearUiCommands() noexcept
{
    impl_->ui_commands.clear();
}

Result GameSimulation::Shutdown()
{
    if (!impl_->initialized)
    {
        return Result::Success();
    }
    impl_->world.quit();
    impl_->initialized = false;
    return Result::Success();
}

} // namespace hs
