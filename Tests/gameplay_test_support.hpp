#pragma once

#include <hs/gameplay/game_simulation.hpp>
#include <hs/presentation/projector.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace gameplay_test
{

inline constexpr auto kFixedStep = std::chrono::nanoseconds{16'666'667};

void Check(bool condition, std::string_view message);

struct TestContent
{
    hs::SimulationRules simulation_rules;
    hs::PresentationCatalog presentation;
};

const TestContent &DefaultContent();
hs::SimulationRules QuietGameData();

hs::TickResult Tick(hs::GameSimulation &simulation,
                    const hs::HeldInputState &held = {},
                    std::span<const hs::ActionEdge> edges = {});

extern hs::SettingsData test_settings;
extern hs::PresentationUiState test_ui;
extern std::optional<hs::UiCommand> last_ui_command;

hs::TickResult TickEdge(hs::GameSimulation &simulation, hs::GameAction action,
                        hs::EdgeKind kind, hs::Sequence &sequence,
                        const hs::HeldInputState &held = {});

void Debug(hs::GameSimulation &simulation, hs::DebugCommandKind kind,
           std::uint64_t value = 0, std::uint32_t secondary = 0,
           hs::Float2 position = {});

bool WriteSnapshot(hs::GameSimulation &simulation, hs::RenderSnapshotStorage &snapshot);
bool HasVfx(const hs::GameSimulation &simulation, hs::DomainSignalKind kind);
hs::Float2 NormalizedCursor(float x, float y);
void SetCursor(hs::HeldInputState &held, float x, float y);

} // namespace gameplay_test
