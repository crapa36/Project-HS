#include <hs/game_rules/simulation_rules_io.hpp>
#include "gameplay_test_support.hpp"
#include <hs/presentation/process_scene.hpp>
#include "../Source/Apps/AssetPipeline/action.hpp"
#include <hs/core/process_info.hpp>
#include <hs/presentation/presentation_catalog_io.hpp>

#include <algorithm>
#include <array>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ap = hs::asset_pipeline;

namespace gameplay_test
{

template <typename Value, typename Decode>
Value LoadGameData(std::string_view id, Decode decode)
{
    ap::ActionResultSnapshot snapshot(hs::CurrentExecutableDirectory().parent_path(), id);
    ap::UpstreamProduct dependency;
    const auto bytes = snapshot.ReadProductBytes(id, dependency);
    Value loaded;
    Check(decode(std::as_bytes(std::span(bytes.data(), bytes.size())), loaded), id);
    return loaded;
}

void Check(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

void Check(const hs::Result &result, std::string_view message)
{
    Check(result.Succeeded(), message);
}

static const hs::SimulationRules &DefaultSimulationRules()
{
    static const auto data = LoadGameData<hs::SimulationRules>(
        "game-data/simulation-payload", hs::DecodeSimulationRules);
    return data;
}

const hs::PresentationCatalog &DefaultPresentation()
{
    static const auto data = LoadGameData<hs::PresentationCatalog>(
        "game-data/presentation-payload", hs::DecodePresentationCatalog);
    return data;
}

hs::SimulationRules QuietGameData()
{
    auto data = DefaultSimulationRules();
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
    return simulation.TickFixed(input);
}

hs::TickResult TickEdge(hs::GameSimulation &simulation, hs::GameAction action,
                        hs::EdgeKind kind, hs::Sequence &sequence,
                        const hs::HeldInputState &held)
{
    if (action == hs::GameAction::BasicAttack && kind == hs::EdgeKind::Pressed &&
        simulation.GetObservation().phase != hs::SessionPhase::Playing)
    {
        hs::PresentationUiState ui;
        const auto interaction = hs::ResolveUiInteraction(
            simulation.GetObservation(), ui, hs::SettingsData{}, held.cursor_normalized);
        if (interaction.gameplay_action) simulation.ApplyUiAction(*interaction.gameplay_action);
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
    auto read = simulation.BorrowTickReadView();
    static const hs::SettingsData settings;
    return hs::ProjectTick(read.View(), DefaultPresentation(), {},
        settings, snapshot, hs::ProcessSceneView(hs::BuildProcessSceneAtLaunch(read.View(), {}))).Succeeded();
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


} // namespace gameplay_test
