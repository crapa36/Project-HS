#pragma once

#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/domain_signal.hpp>
#include <hs/game_domain/game_read_model.hpp>
#include <hs/presentation/presentation_catalog.hpp>

#include <optional>
#include <span>

namespace hs
{

struct UiInteraction
{
    std::optional<UiAction> gameplay;
    std::optional<UiCommand> runtime;
};

struct PresentationUiState
{
    std::uint8_t page{};
    std::uint8_t collection_skill{};
    std::uint8_t character_skill{};
    std::uint8_t loadout_source{0xFF};
};

[[nodiscard]] std::size_t ProjectPresentation(
    const DomainSignal &signal, std::span<PresentationEvent> output) noexcept;
[[nodiscard]] UiInteraction ResolveUiInteraction(const SessionProbe &session,
                                                 PresentationUiState &ui,
                                                 const SettingsData &settings,
                                                 Float2 cursor_normalized);
[[nodiscard]] bool ProjectRenderSnapshot(const GameReadModel &model,
                                         const PresentationCatalog &presentation,
                                         const PresentationUiState &ui,
                                         const SettingsData &settings,
                                         RenderSnapshotStorage &snapshot,
                                         std::uint8_t pending_rebind_slot = 0xFF);

} // namespace hs
