#pragma once

#include "ui_draw_packet.hpp"
#include "vfx_session_state.hpp"
#include <hs/renderer/renderer.hpp>

namespace hs::runtime_detail
{
struct RendererReceiptFacts
{
    bool ui_receipt_rejected{};
    bool native_batch_accepted{};
};

// A later Render failure cannot undo a successful Present or accepted GPU work.
[[nodiscard]] inline RendererReceiptFacts AcceptRendererReceipts(
    const RendererFrameResult &frame, UiDrawPackets &ui_packets,
    const std::shared_ptr<const UiFrame> &ui_frame, std::uint64_t session_id,
    VfxSessionState &vfx_session) noexcept
{
    const bool ui_rejected = frame.presented_ui_packet_generation != 0 &&
        !ui_packets.AcceptPresentedUiPacket(frame.presented_ui_packet_generation,
            ui_frame, session_id, frame.rendered_view.output_extent);
    const bool native_accepted = vfx_session.AcknowledgeNativeBatch(
        {.session = frame.vfx_world_session_id, .stateful_target = frame.vfx_world_tick,
            .sequence = frame.vfx_world_batch_sequence}, frame.vfx_world_step_submitted);
    return {ui_rejected, native_accepted};
}
}
