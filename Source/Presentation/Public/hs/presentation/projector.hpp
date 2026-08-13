#pragma once

#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/game_read_model.hpp>
#include <hs/game_rules/simulation_rules.hpp>

#include <optional>

namespace hs
{

struct UiInteraction
{
    std::optional<UiAction> gameplay;
    std::optional<UiCommand> runtime;
};

[[nodiscard]] PresentationEvent ProjectPresentation(const DomainSignal &signal) noexcept;
[[nodiscard]] UiInteraction ResolveUiInteraction(const SessionProbe &session,
                                                 const SettingsData &settings,
                                                 Float2 cursor_normalized);
[[nodiscard]] bool ProjectRenderSnapshot(const GameReadModel &model,
                                         const SimulationRules &rules,
                                         const PresentationCatalog &presentation,
                                         const SettingsData &settings,
                                         RenderSnapshotStorage &snapshot,
                                         std::uint8_t pending_rebind_slot = 0xFF);

} // namespace hs
