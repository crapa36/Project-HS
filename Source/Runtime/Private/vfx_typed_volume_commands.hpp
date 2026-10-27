#pragma once
#include "vfx_event_view.hpp"

#include <hs/core/result.hpp>
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/render_data/vfx_volume_input.hpp>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
[[nodiscard]] bool IsVfxTypedVolumeEffect(const VfxProgramView &, VfxEffectHandle) noexcept;

// Runtime retains event inputs until their cooked fixed effect deadline, then
// rebuilds these commands at the fractional display tick (60 ticks/second). Persistent commands
// exist only while their actual snapshot owner exists. Below High, output is
// empty. Malformed allowlisted recipes fail the whole conversion visibly.
[[nodiscard]] Result BuildVfxTypedVolumeCommands(const VfxProgramView &,
    VfxEventView, std::span<const VfxPersistentInput>, double current_tick,
    VfxQuality admission, std::vector<VfxVolumeInput> &);
} // namespace hs::runtime_detail
