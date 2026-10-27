#pragma once
#include "vfx_event_view.hpp"
#include <hs/render_data/vfx_decal_input.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
std::vector<VfxDecalInput> BuildVfxTypedDecalCommands(const VfxImage &image,
 VfxEventView events, std::span<const VfxPersistentInput> persistent, double current_tick);
}
