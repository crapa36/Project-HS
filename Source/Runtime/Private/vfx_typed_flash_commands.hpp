#pragma once
#include "vfx_event_view.hpp"

#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/runtime/vfx_image.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{

// Decoded additive impact sprites. Runtime owns age/retention after decode;
// this record carries the authored event tick so repeated render frames cannot
// restart the flash. Successful pickup bursts identify the legacy collection
// layers they replace; rejected recipes retain their compatibility visuals.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxTypedFlashCommands(
    const VfxImage &image,
    VfxEventView inputs,
    std::vector<Sequence> *superseded_pickup_sequences = nullptr);

// Rebuild these attachment sprites from the live owner every frame. The returned
// age is local to status_stamp's authored knot interval; callers do not retain it.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxPersistentStatusStampCommands(
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs);

// Real area presence owns the source interval. Rate births use the 60 Hz
// schedule; each tongue ends at that interval's end, with no invented TTL.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxPersistentFireFlameCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> inputs,
    double current_tick);

struct VfxOwnedStatusTickFlashCommand
{
    Sequence event_sequence{};
    VfxFlashSpawnInput flash;
};

// A fixed status tick is converted only while its matching persistent entity
// owner and status episode are live. The sequence identifies the event whose
// legacy visuals may be suppressed after this conversion succeeds.
[[nodiscard]] std::vector<VfxOwnedStatusTickFlashCommand> BuildVfxOwnedStatusTickFlashCommands(
    const VfxImage &image,
    VfxEventView events,
    std::span<const VfxPersistentInput> persistent);

} // namespace hs::runtime_detail
