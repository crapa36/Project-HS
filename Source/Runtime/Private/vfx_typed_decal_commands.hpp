#pragma once
#include "vfx_event_view.hpp"
#include <hs/render_data/vfx_decal_input.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
// The image must be immutable and prepared by the loader; event and owner inputs remain mutable.
std::vector<VfxDecalInput> BuildVfxTypedDecalCommands(const VfxImage &image,
 VfxEventView events, std::span<const VfxPersistentInput> persistent, double current_tick);
void AppendVfxTypedDecalCommands(const VfxImage &image, VfxEventView events,
 std::span<const VfxPersistentInput> persistent, double current_tick,
 std::vector<VfxDecalInput> &result);
}
