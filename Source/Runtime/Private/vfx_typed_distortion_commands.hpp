#pragma once
#include "vfx_event_view.hpp"
#include <hs/render_data/vfx_distortion_input.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxDistortionInput> BuildVfxTypedDistortionCommands(
    const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> persistent, double current_tick);
void AppendVfxTypedDistortionCommands(const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> persistent, double current_tick,
    std::vector<VfxDistortionInput> &result);
// The caller clears/reuses this frame pool only after its previous Render call returned.
void RebuildVfxTypedDistortionCommands(const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> persistent, double current_tick,
    std::vector<VfxDistortionInput> &result, std::vector<float> &frame_gaps);
}
