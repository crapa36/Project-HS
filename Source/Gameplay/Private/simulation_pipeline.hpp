#pragma once

#include <cstdint>

namespace hs::gameplay_detail
{

#define HS_SIMULATION_PHASES(X) \
    X(SessionTimer, SessionTimerPhase) \
    X(Spawn, SpawnPhase) \
    X(SpawnBarrier, CommitSpawnBarrier) \
    X(AiIntent, AiIntentPhase) \
    X(CastAttack, CastAttackPhase) \
    X(AbilitySpawnBarrier, CommitAbilitySpawnBarrier) \
    X(Movement, MovementPhase) \
    X(SpatialGrid, SpatialGridPhase) \
    X(CollisionHit, CollisionHitPhase) \
    X(DamageStatus, DamageStatusPhase) \
    X(DeathDrop, DeathDropPhase) \
    X(XpCard, XpCardPhase) \
    X(CleanupBarrier, CommitCleanupBarrier) \
    X(GameplayHash, UpdateChecksum)

#define HS_SIMULATION_PHASE_ID(name, method) name,
enum class SimulationPhaseId : std::uint8_t
{
    HS_SIMULATION_PHASES(HS_SIMULATION_PHASE_ID)
};
#undef HS_SIMULATION_PHASE_ID

} // namespace hs::gameplay_detail
