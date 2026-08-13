#pragma once

#include <array>
#include <cstdint>

namespace hs::gameplay_detail
{

enum class SimulationPhaseId : std::uint8_t
{
    SessionTimer,
    Spawn,
    SpawnBarrier,
    AiIntent,
    CastAttack,
    AbilitySpawnBarrier,
    Movement,
    SpatialGrid,
    CollisionHit,
    DamageStatus,
    DeathDrop,
    XpCard,
    CleanupBarrier,
    GameplayHash,
};

inline constexpr std::array kSimulationPipeline{
    SimulationPhaseId::SessionTimer,
    SimulationPhaseId::Spawn,
    SimulationPhaseId::SpawnBarrier,
    SimulationPhaseId::AiIntent,
    SimulationPhaseId::CastAttack,
    SimulationPhaseId::AbilitySpawnBarrier,
    SimulationPhaseId::Movement,
    SimulationPhaseId::SpatialGrid,
    SimulationPhaseId::CollisionHit,
    SimulationPhaseId::DamageStatus,
    SimulationPhaseId::DeathDrop,
    SimulationPhaseId::XpCard,
    SimulationPhaseId::CleanupBarrier,
    SimulationPhaseId::GameplayHash,
};

} // namespace hs::gameplay_detail
