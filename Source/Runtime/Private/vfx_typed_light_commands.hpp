#pragma once
#include "vfx_event_view.hpp"
#include <hs/render_data/vfx_light_input.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
// Reconstruct live lights from absolute owner/event time. Renderer applies the global cap.
[[nodiscard]] std::vector<VfxLightInput> BuildVfxTypedLightCommands(
    const VfxImage &image, VfxEventView inputs, double current_tick);
}
