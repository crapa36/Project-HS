#pragma once

#include <hs/gameplay/game_simulation.hpp>
#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/gameplay/game_simulation_development.hpp>
#include <hs/presentation/projector.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace gameplay_test
{

void Check(bool condition, std::string_view message);
void Check(const hs::Result &result, std::string_view message);

const hs::PresentationCatalog &DefaultPresentation();
hs::SimulationRules QuietGameData();

hs::TickResult Tick(hs::GameSimulation &simulation,
                    const hs::HeldInputState &held = {},
                    std::span<const hs::ActionEdge> edges = {});

hs::TickResult TickEdge(hs::GameSimulation &simulation, hs::GameAction action,
                        hs::EdgeKind kind, hs::Sequence &sequence,
                        const hs::HeldInputState &held = {});

void Debug(hs::GameSimulation &simulation, hs::DebugCommandKind kind,
           std::uint64_t value = 0, std::uint32_t secondary = 0,
           hs::Float2 position = {});

bool WriteSnapshot(hs::GameSimulation &simulation, hs::RenderSnapshotStorage &snapshot);
bool HasVfx(const hs::GameSimulation &simulation, hs::DomainSignalKind kind);
hs::Float2 NormalizedCursor(float x, float y);

} // namespace gameplay_test
