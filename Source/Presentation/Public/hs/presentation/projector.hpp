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
#include <cstdint>

namespace hs
{

struct UiInteraction
{
    std::optional<UiAction> gameplay_action;
    std::optional<UiCommand> runtime_command;
};

enum class UiPage : std::uint8_t
{
    Root = 0,
    Collection = 1,
    MainMenuSettings = 2,
    CharacterOverview = 3,
    CharacterSkills = 4,
    CharacterStats = 5,
    PauseSettings = 6,
};

struct PresentationUiState
{
    UiPage page{UiPage::Root};
    std::uint8_t selected_collection_skill{};
    std::uint8_t selected_character_skill{};
    std::uint8_t loadout_source_slot{0xFF};
};

[[nodiscard]] std::size_t ProjectDomainSignal(
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
