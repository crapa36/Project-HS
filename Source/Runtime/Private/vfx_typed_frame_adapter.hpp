#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hs::runtime_detail
{

struct VfxTypedFrameInputs
{
    std::vector<VfxEventInput> events;
    std::vector<VfxPersistentInput> persistent;
    std::vector<std::uint64_t> unsupported_effect_ids;
};

// Convert the presentation contracts into typed v4 renderer inputs. The
// function owns no state and does not create a lifetime or geometry value when
// the source contract does not provide one.
[[nodiscard]] VfxTypedFrameInputs BuildVfxTypedFrameInputs(
    const VfxProgramData &cooked,
    std::span<const PresentationEvent> events,
    std::span<const PersistentVfxVisual> current,
    std::span<const PersistentVfxVisual> previous,
    Tick current_tick);

// Refresh retained warning events from their authoritative persistent
// owner. Events without owner metadata are preserved unchanged.
void RefreshVfxGroundEventOwners(std::vector<VfxEventInput> &events,
                                 std::span<const PersistentVfxVisual> owners,
                                 Tick current_tick);

// Rebind retained owner-backed warning events after a catalog reload. Events
// that are not exact warning/sector recipes in the new program are dropped.
void RebindVfxGroundEventOwners(const VfxProgramData &cooked,
                                std::vector<VfxEventInput> &events);

} // namespace hs::runtime_detail
