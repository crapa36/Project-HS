#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hs
{

inline constexpr std::size_t kActiveSkillCount = 8;
inline constexpr std::size_t kCombatSkillCount = 9;
inline constexpr std::size_t kUpgradeCount = 8;
inline constexpr std::size_t kRelicCount = 20;
inline constexpr std::size_t kStatCount = 6;
inline constexpr std::size_t kEnemyArchetypeCount = 6;
inline constexpr std::size_t kPickupKindCount = 4;
inline constexpr std::uint32_t kSimulationVersion = 4;
inline constexpr std::uint32_t kGameplayHashVersion = 6;
inline constexpr std::uint32_t kDeterminismProfile = 1;
using RelicMask = std::uint32_t;
static_assert(kRelicCount < 32);
inline constexpr RelicMask kAllRelicsMask =
    (RelicMask{1} << kRelicCount) - RelicMask{1};
enum class UpgradeEffectMetric : std::uint8_t
{
    ProjectilesCreated,
    AreasCreated,
    ExplosionsCreated,
    BleedStacksApplied,
    BurnApplications,
    SlowApplications,
    SlowTargetTicks,
    BleedActiveTicks,
    BurnActiveTicks,
    SlowActiveTicks,
    CooldownTicksSaved,
    Healing,
    DisplacementMillimetres,
    ExtraTargetsHit,
    ExtraBounces,
    ChargeTicksSaved,
    DurationTicksAdded,
    MarksApplied,
    Kills,
    DamageAmplified,
    Activations,
    DamagePrevented,
    DamageOverTime,
    DamageOverTimeEvents,
    DamageOverTimeKills,
    EffectActiveTicks,
    Count,
};
inline constexpr std::size_t kUpgradeEffectMetricCount =
    static_cast<std::size_t>(UpgradeEffectMetric::Count);
enum class UpgradeRelicSynergyMetric : std::uint8_t
{
    Damage,
    DamageEvents,
    Activations,
    Healing,
    BurnApplications,
    SlowApplications,
    SlowTargetTicks,
    Count,
};
inline constexpr std::size_t kUpgradeRelicSynergyMetricCount =
    static_cast<std::size_t>(UpgradeRelicSynergyMetric::Count);
inline constexpr std::array<std::string_view, kUpgradeEffectMetricCount>
    kUpgradeEffectMetricIds{
        "projectiles_created", "areas_created", "explosions_created",
        "bleed_stacks_applied", "burn_applications", "slow_applications",
        "slow_target_ticks", "bleed_active_ticks", "burn_active_ticks",
        "slow_active_ticks", "cooldown_ticks_saved", "healing",
        "displacement_millimetres", "extra_targets_hit", "extra_bounces",
        "charge_ticks_saved", "duration_ticks_added", "marks_applied", "kills",
        "damage_amplified", "activations", "damage_prevented", "damage_over_time",
        "damage_over_time_events", "damage_over_time_kills", "effect_active_ticks"};
inline constexpr std::array<std::string_view, kUpgradeRelicSynergyMetricCount>
    kUpgradeRelicSynergyMetricIds{"damage", "damage_events", "activations", "healing",
                                   "burn_applications", "slow_applications",
                                   "slow_target_ticks"};
inline constexpr std::size_t kMaxUpgradeRelicSynergies = 256;
enum class SessionPhase : std::uint8_t
{
    MainMenu,
    Playing,
    Paused,
    CardSelection,
    StatAllocation,
    RelicSelection,
    Victory,
    Defeat,
    QuitRequested,
};

enum class SkillKind : std::uint8_t
{
    BasicAttack,
    PiercingShot,
    MultiShot,
    ChargedShot,
    ExplosiveArrow,
    RicochetArrow,
    ArrowRain,
    Trap,
    RetreatShot,
    Count,
};

enum class StatKind : std::uint8_t
{
    MaxHealth,
    MoveSpeed,
    AttackPower,
    AttackSpeed,
    CooldownReduction,
    MagnetRadius,
    Count,
};

enum class RelicKind : std::uint8_t
{
    BleedKillHeal,
    BurnPropagation,
    KillCooldownSurge,
    BleedBurnExplosion,
    RadialBasicAttack,
    BasicKillTracker,
    MovementEcho,
    AlternatingSkills,
    DifferentSkillTracker,
    DamageKnockback,
    OnceRevive,
    CombatHitChain,
    ProjectileCadenceReward,
    PreDamageGuard,
    SlowSynergy,
    AreaResonance,
    BossPressure,
    HitStreakReward,
    PickupReward,
    LowHealthSurvival,
    Count,
};

enum class EnemyKind : std::uint8_t
{
    Melee,
    Ranged,
    Suicide,
};

enum class BossKind : std::uint8_t
{
    FiveMinute,
    TenMinute,
    Final,
};

enum class PickupKind : std::uint8_t
{
    Experience,
    Heal,
    Magnet,
    RelicChest,
};

enum class CardKind : std::uint8_t
{
    LearnSkill,
    SkillUpgrade,
    BasicUpgrade,
    BonusStatPoint,
    Relic,
};

enum class EffectOrigin : std::uint8_t
{
    Original,
    Derived,
    DamageOverTime,
};

struct CardView
{
    CardKind kind{};
    std::uint8_t subject{};
    std::uint8_t upgrade{};
};

struct UpgradeRelicSynergyTelemetry
{
    SkillKind skill{SkillKind::Count};
    std::uint8_t upgrade{};
    RelicKind relic{RelicKind::Count};
    std::array<std::uint64_t, kUpgradeRelicSynergyMetricCount> metrics{};
};

struct BalanceTelemetry
{
    std::array<std::uint64_t, kCombatSkillCount> skill_uses{};
    std::array<std::uint64_t, kCombatSkillCount> skill_casts_with_hit{};
    std::array<std::uint64_t, kCombatSkillCount> skill_hit_events{};
    std::array<std::uint64_t, kCombatSkillCount> skill_kills{};
    std::array<std::uint64_t, kCombatSkillCount> skill_boss_damage{};
    std::array<std::array<std::uint64_t, kUpgradeCount>, kCombatSkillCount>
        upgrade_damage{};
    std::array<std::array<std::uint64_t, kUpgradeCount>, kCombatSkillCount>
        upgrade_triggers{};
    std::array<std::array<std::array<std::uint64_t, kUpgradeEffectMetricCount>,
                          kUpgradeCount>,
               kCombatSkillCount>
        upgrade_effects{};
    std::array<UpgradeRelicSynergyTelemetry, kMaxUpgradeRelicSynergies>
        upgrade_relic_synergies{};
    std::uint16_t upgrade_relic_synergy_count{};
    std::array<std::uint64_t, kRelicCount> relic_damage{};
    std::array<std::uint64_t, kRelicCount> relic_triggers{};
    std::array<std::uint64_t, kRelicCount> relic_kills{};
    std::array<std::array<std::uint64_t, kUpgradeEffectMetricCount>, kRelicCount>
        relic_effects{};
    std::array<std::uint64_t, kEnemyArchetypeCount> enemy_spawned{};
    std::array<std::uint64_t, kEnemyArchetypeCount> enemy_killed{};
    std::array<std::uint64_t, kEnemyArchetypeCount> enemy_attack_attempts{};
    std::array<std::uint64_t, kEnemyArchetypeCount> enemy_hits{};
    std::array<std::uint64_t, kEnemyArchetypeCount> enemy_damage{};
    std::array<std::uint64_t, kEnemyArchetypeCount> enemy_lifetime_ticks{};
    std::array<std::uint64_t, kPickupKindCount> pickup_drop_attempts{};
    std::array<std::uint64_t, kPickupKindCount> pickup_drops{};
    std::array<std::uint64_t, kPickupKindCount> pickup_collected{};
    // max-health HP, movement millimetres, attack damage, attack/cooldown ticks saved,
    // and extra magnet pickups respectively.
    std::array<std::uint64_t, kStatCount> stat_utility{};
    std::uint64_t direct_damage{};
    std::uint64_t derived_damage{};
    std::uint64_t damage_over_time{};
};

struct SessionProbe
{
    Tick tick{};
    Tick growth_ticks{};
    Tick boss_fight_ticks{};
    SessionPhase phase{SessionPhase::MainMenu};
    std::uint32_t level{1};
    std::uint32_t experience{};
    std::uint32_t experience_to_next{20};
    std::int32_t health{100};
    std::int32_t max_health{100};
    Float2 player_position{};
    Float2 aim_direction{0.0f, 1.0f};
    Float2 facing_direction{0.0f, 1.0f};
    std::uint32_t normal_enemy_count{};
    std::uint32_t boss_count{};
    std::uint32_t boss_warning_count{};
    std::uint32_t boss_dashing_count{};
    std::uint32_t enemy_area_count{};
    std::uint32_t player_projectile_count{};
    std::uint32_t enemy_projectile_count{};
    std::uint32_t pickup_count{};
    std::uint32_t kills{};
    std::uint64_t damage_dealt{};
    std::array<std::uint64_t, kCombatSkillCount> damage_by_skill{};
    std::uint64_t damage_taken{};
    std::uint64_t healing{};
    std::uint8_t level_rerolls_remaining{3};
    std::uint8_t relic_rerolls_remaining{3};
    std::uint8_t pending_stat_points{};
    std::uint8_t active_skill_count{};
    std::array<SkillKind, 4> skill_loadout{SkillKind::Count, SkillKind::Count,
                                           SkillKind::Count, SkillKind::Count};
    std::array<std::uint8_t, kCombatSkillCount> skill_levels{};
    std::array<std::uint8_t, kCombatSkillCount> upgrade_masks{};
    std::array<std::uint32_t, kActiveSkillCount> cooldown_ticks{};
    std::array<std::uint8_t, kStatCount> stat_points{};
    RelicMask relic_mask{};
    std::array<CardView, 3> cards{};
    std::uint8_t card_count{};
    bool final_boss_spawned{};
    bool final_boss_phase_two{};
};

struct SimulationObservation : SessionProbe
{
    BalanceTelemetry balance{};
};

struct SimulationDiagnostics
{
    BalanceTelemetry balance{};
};

enum class DebugCommandKind : std::uint8_t
{
    StartSession,
    SetGrowthTick,
    DamagePlayer,
    HealPlayer,
    DamageFinalBoss,
    GrantExperience,
    GrantSkill,
    GrantUpgrade,
    GrantRelic,
    SpawnEnemy,
    SpawnBoss,
    SelectCard,
    AssignStat,
    SetStat,
    Reroll,
    TogglePause,
};

struct DebugCommand
{
    DebugCommandKind kind{};
    std::uint64_t value{};
    std::uint32_t secondary{};
    Float2 position{};
};

} // namespace hs
