#pragma once

#include <hs/core/types.hpp>

#include <cstdint>

namespace hs
{

enum class DomainSignalKind : std::uint8_t
{
    BasicAttackImpact,
    BossAreaActivated,
    BossDashImpact,
    BossDashStarted,
    BossPhaseChanged,
    BossShockwaveReleased,
    BossSpawned,
    BossVolleyReleased,
    EnemyDied,
    LargeExplosion,
    SmallExplosion,
    PlayerHealed,
    HeavyHit,
    ProjectileHit,
    MarkApplied,
    MarkTriggered,
    PlayerDied,
    PlayerDamaged,
    Pull,
    Push,
    MeleeEnemyHit,
    MeleeEnemyWindup,
    RangedEnemyReleased,
    SuicideEnemyCharging,
    SuicideEnemyExploded,
    BurnTransferred,
    RelicChainLinked,
    RicochetLinked,
    HealCollected,
    MagnetCollected,
    RelicCollected,
    ExperienceSpawned,
    BleedBurnExploded,
    CombatChainHit,
    DamagePush,
    RadialArrowsCast,
    ArrowRainCast,
    ArrowRainPulse,
    ArrowRainImpact,
    ChargedShotCast,
    ChargedShotPulse,
    ChargedShotReady,
    DamageAreaPulse,
    ExplosiveArrowCast,
    ExplosiveArrowMain,
    ExplosiveArrowSecondary,
    FireAreaPulse,
    MultiShotCast,
    PiercingShotCast,
    PiercingTrailPulse,
    RetreatShotCast,
    RetreatLanded,
    RetreatMoved,
    RicochetArrowCast,
    RicochetArrowHit,
    TrapCast,
    TrapArmed,
    TrapTriggered,
    BleedApplied,
    BleedTicked,
    BurnApplied,
    BurnTicked,
    SlowApplied,
    SlowArea,
    BossSpawnWarning,
    EnemySpawnWarning,
    AbilityUsed,
    ArrowReleased,
    BasicAttackStarted,
    ChargedShotStarted,
    ChargedShotEnded,
    TrapDamaged,
    CooldownSurged,
    TrackingArrowFired,
    AfterimageArrowFired,
    CooldownRefunded,
    PlayerRevived,
    BossDashTelegraphed,
    BossVolleyTelegraphed,
    BossAreaTelegraphed,
    BossShockwaveTelegraphed,
    BossDied,
    ExperienceCollected,
    SkillUnlocked,
    // context is the RelicKind that produced the activation.
    RelicTriggered,
    RangedEnemyTelegraphed,
    UpgradeVisual,
    EnemyRangedProjectileImpact,
    BossVolleyProjectileImpact,
    ChargedShotProjectileImpact,
    Count,
};

// Presentation-only activation stage; selected upgrades are not activation events.
enum class UpgradeVisualStage : std::uint8_t { None, Spawn, Telegraph, Resolve };

enum class DomainSignalFlag : std::uint8_t { None, HasTarget = 1u << 0 };

enum class DomainSignalGeometryKind : std::uint8_t
{
    None,
    Circle,
    Line,
    Cone,
    RingGaps,
    Projectile,
};

// Authoritative gameplay geometry carried alongside a signal.  The legacy
// position/direction/scale fields remain available for point effects; these
// fields are only populated when the event owns an explicit shape.
struct DomainSignalGeometry
{
    DomainSignalGeometryKind kind{DomainSignalGeometryKind::None};
    float radius{};
    float inner_radius{};
    float outer_radius{};
    float width{};
    float range{};
    float half_angle_degrees{};
    std::uint8_t gap_count{};
    float gap_offset_degrees{};
    float gap_half_width_degrees{};
    Float3 velocity{};
    Tick start_tick{};
    Tick end_tick{};
    std::uint64_t source_id{};
    Float3 end_position{};
};

struct DomainSignal
{
    Sequence sequence{};
    Tick tick{};
    DomainSignalKind kind{};
    Float3 position{};
    Float3 direction{0.0f, 0.0f, 1.0f};
    Float3 target{};
    float scale{1.0f};
    // Optional authored context progress; leaves legacy particle scale intact.
    float vfx_ratio01{-1.0f};
    std::uint8_t flags{};
    // SkillKind/BossKind/other semantic source, depending on signal kind.
    std::uint8_t context{};
    // EnemyDied: normal enemy identity; zero for bosses or unrelated signals.
    std::uint64_t source_entity_id{};
    std::uint64_t status_episode_generation{};
    DomainSignalGeometry geometry{};
    std::uint64_t session_id{};
    std::uint8_t upgrade_skill{0xFF};
    std::uint8_t upgrade_index{0xFF}; // zero based; cooked VFX ordinal is index + 1
    UpgradeVisualStage upgrade_stage{};
    std::uint64_t upgrade_cast_id{}, upgrade_owner_id{};
};

} // namespace hs
