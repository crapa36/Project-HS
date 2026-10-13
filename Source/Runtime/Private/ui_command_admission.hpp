#pragma once
#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/presentation/projector.hpp>

namespace hs::runtime_detail
{
// Resolve into a candidate. A click emits at most one command; publication is
// the commit point. Pure navigation does not depend on either queue's capacity.
inline bool AdmitUiInteraction(UiInteraction &interaction, PresentationUiState &candidate,
    const PresentationUiState &previous, BoundedSpscQueue<UiAction, 64> &actions,
    BoundedSpscQueue<UiCommand, 64> &commands, std::uint64_t &rejected) noexcept
{
    const bool admitted = !(interaction.gameplay_action && interaction.runtime_command) &&
        (!interaction.gameplay_action || actions.TryPush(*interaction.gameplay_action)) &&
        (!interaction.runtime_command || commands.TryPush(*interaction.runtime_command));
    if (!admitted)
    {
        candidate = previous;
        interaction.gameplay_action.reset();
        interaction.runtime_command.reset();
        interaction.invalid = true;
        ++rejected;
    }
    return admitted;
}
}
