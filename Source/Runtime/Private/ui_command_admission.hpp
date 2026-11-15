#pragma once
#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/presentation/projector.hpp>
#include "ui_draw_packet.hpp"

namespace hs::runtime_detail
{
// Resolve into a candidate. Publication is the commit point. Pure navigation
// does not depend on either queue's capacity.
inline bool AdmitPresentedUiInteraction(UiInteraction &interaction,
    PresentationUiState &candidate, const PresentationUiState &previous,
    const UiFrame &displayed,
    BoundedSpscQueue<UiActionEnvelope, 64> &actions,
    BoundedSpscQueue<UiCommand, 64> &commands, std::uint64_t &rejected) noexcept
{
    bool admitted = true;
    if (interaction.gameplay_action)
    {
        UiActionEnvelope envelope;
        envelope.action = *interaction.gameplay_action;
        envelope.packet_generation = displayed.generation;
        envelope.session_id = displayed.session_id;
        envelope.displayed_session = displayed.session;
        admitted = actions.TryPush(envelope);
    }
    else if (interaction.runtime_command)
        admitted = commands.TryPush(*interaction.runtime_command);
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
