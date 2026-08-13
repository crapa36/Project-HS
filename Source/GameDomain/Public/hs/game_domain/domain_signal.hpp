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
    AbilityUsed,
    ArrowReleased,
    Count,
};

enum class DomainSignalFlag : std::uint8_t { None, HasTarget = 1u << 0 };

struct DomainSignal
{
    Sequence sequence{};
    Tick tick{};
    DomainSignalKind kind{};
    Float3 position{};
    Float3 direction{0.0f, 0.0f, 1.0f};
    Float3 target{};
    float scale{1.0f};
    std::uint8_t flags{};
};

} // namespace hs
