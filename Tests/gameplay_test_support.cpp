#include "gameplay_test_support.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <ranges>
#include <stdexcept>
#include <string>

namespace gameplay_test
{

hs::SettingsData test_settings;
hs::PresentationUiState test_ui;
std::optional<hs::UiCommand> last_ui_command;

void Check(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

const TestContent &DefaultContent()
{
    static const auto data = [] {
        TestContent loaded;
        Check(hs::LoadSimulationRules(
                  std::filesystem::current_path() / "Cooked" /
                      "simulation_rules.hsbin",
                  loaded.simulation_rules).Succeeded(),
              "load cooked simulation rules");
        Check(hs::LoadPresentationCatalog(
                  std::filesystem::current_path() / "Cooked" /
                      "presentation_catalog.hsbin",
                  loaded.presentation).Succeeded(),
              "load cooked presentation catalog");
        return loaded;
    }();
    return data;
}

hs::SimulationRules QuietGameData()
{
    auto data = DefaultContent().simulation_rules;
    for (auto &stage : data.spawn_stages)
    {
        stage.per_second = 0;
    }
    for (auto &wave : data.waves)
    {
        wave.count = 0;
    }
    data.growth.utility_pickup_base_chance = 0.0f;
    data.growth.utility_pickup_miss_increment = 0.0f;
    data.relic_drop.normal_enemy_base_probability = 0.0f;
    data.relic_drop.normal_enemy_probability_increment_per_kill = 0.0f;
    data.relic_drop.healing_pickup_probability = 0.0f;
    return data;
}

hs::TickResult Tick(hs::GameSimulation &simulation,
                    const hs::HeldInputState &held,
                    std::span<const hs::ActionEdge> edges)
{
    hs::InputFrame input;
    input.target_tick = simulation.GetObservation().tick + 1;
    input.held = held;
    input.ordered_edges = edges;
    return simulation.TickFixed(input, kFixedStep);
}

hs::TickResult TickEdge(hs::GameSimulation &simulation, hs::GameAction action,
                        hs::EdgeKind kind, hs::Sequence &sequence,
                        const hs::HeldInputState &held)
{
    if (kind == hs::EdgeKind::Pressed && action == hs::GameAction::CharacterPage)
    {
        test_ui.page = simulation.GetObservation().phase == hs::SessionPhase::Playing
                           ? hs::UiPage::CharacterOverview
                           : hs::UiPage::Root;
        test_ui.loadout_source_slot = 0xFF;
    }
    if (kind == hs::EdgeKind::Pressed && action == hs::GameAction::Pause)
    {
        const auto phase = simulation.GetObservation().phase;
        if ((phase == hs::SessionPhase::Paused && test_ui.page == hs::UiPage::PauseSettings) ||
            (phase == hs::SessionPhase::MainMenu && test_ui.page != hs::UiPage::Root))
        {
            test_ui.page = hs::UiPage::Root;
            return Tick(simulation, held);
        }
        test_ui.page = hs::UiPage::Root;
    }
    if (action == hs::GameAction::BasicAttack && kind == hs::EdgeKind::Pressed &&
        simulation.GetObservation().phase != hs::SessionPhase::Playing)
    {
        const auto interaction = hs::ResolveUiInteraction(
            simulation.GetObservation(), test_ui, test_settings, held.cursor_normalized);
        if (interaction.gameplay_action) simulation.ApplyUiAction(*interaction.gameplay_action);
        last_ui_command = interaction.runtime_command;
    }
    const std::array edges{hs::ActionEdge{++sequence, action, kind}};
    return Tick(simulation, held, edges);
}

void Debug(hs::GameSimulation &simulation, hs::DebugCommandKind kind,
           std::uint64_t value, std::uint32_t secondary,
           hs::Float2 position)
{
    const auto result = simulation.ApplyDebugCommand({kind, value, secondary, position});
    if (!result.Succeeded()) throw std::runtime_error(std::string(result.Message()));
}

bool WriteSnapshot(hs::GameSimulation &simulation, hs::RenderSnapshotStorage &snapshot)
{
    hs::GameReadModelStorage model;
    simulation.WriteReadModel(model);
    static const hs::SettingsData settings;
    return hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation, test_ui,
                                     settings, snapshot);
}

bool HasVfx(const hs::GameSimulation &simulation, hs::DomainSignalKind kind)
{
    return std::ranges::any_of(
        simulation.PendingDomainSignals(), [&](const hs::DomainSignal &event) {
            return event.kind == kind;
        });
}

hs::Float2 NormalizedCursor(float x, float y)
{
    return {x / 960.0f - 1.0f, 1.0f - y / 540.0f};
}

void SetCursor(hs::HeldInputState &held, float x, float y)
{
    held.cursor_normalized = NormalizedCursor(x, y);
}

} // namespace gameplay_test
