#pragma once

#include <hs/core/result.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <hs/renderer/vfx_volume_input.hpp>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
[[nodiscard]] bool IsVfxTypedVolumeEffect(const VfxProgramData &, VfxEffectHandle) noexcept;

// Runtime retains event inputs until their cooked fixed effect deadline, then
// rebuilds these commands at the authoritative 60 Hz tick. Persistent commands
// exist only while their actual snapshot owner exists. Below High, output is
// empty. Malformed allowlisted recipes fail the whole conversion visibly.
[[nodiscard]] Result BuildVfxTypedVolumeCommands(const VfxProgramData &,
    std::span<const VfxEventInput>, std::span<const VfxPersistentInput>, Tick current_tick,
    VfxQuality admission, std::vector<VfxVolumeInput> &);
} // namespace hs::runtime_detail
