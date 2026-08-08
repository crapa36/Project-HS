#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace hs
{

inline constexpr std::size_t kActiveSkillCount = 8;
inline constexpr std::size_t kCombatSkillCount = 9;
inline constexpr std::size_t kUpgradeCount = 8;
inline constexpr std::size_t kRelicCount = 12;
inline constexpr std::size_t kStatCount = 6;
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
    SlowBurst,
    BleedBurnExplosion,
    RadialBasicAttack,
    BasicKillTracker,
    MovementEcho,
    AlternatingSkills,
    DifferentSkillTracker,
    DamageKnockback,
    LowHealthRecovery,
    ExperiencePulse,
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
    std::uint8_t rerolls_remaining{3};
    std::uint8_t pending_stat_points{};
    std::uint8_t active_skill_count{};
    std::array<std::uint8_t, kCombatSkillCount> skill_levels{};
    std::array<std::uint8_t, kCombatSkillCount> upgrade_masks{};
    std::array<std::uint32_t, kActiveSkillCount> cooldown_ticks{};
    std::array<std::uint8_t, kStatCount> stat_points{};
    std::uint16_t relic_mask{};
    std::array<CardView, 3> cards{};
    std::uint8_t card_count{};
    bool final_boss_spawned{};
    bool final_boss_phase_two{};
    std::uint8_t menu_page{};
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
