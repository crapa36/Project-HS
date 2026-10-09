#pragma once
#include <hs/core/result.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <hs/renderer/vfx_screen_input.hpp>
#include <vector>
namespace hs::runtime_detail
{
[[nodiscard]] bool IsVfxTypedScreenEffect(const VfxProgramData &, VfxEffectHandle) noexcept;
[[nodiscard]] Result BuildVfxTypedScreenCommands(const VfxProgramData &,
    std::span<const VfxEventInput>, std::span<const VfxPersistentInput>, Tick,
    VfxQuality, std::vector<VfxScreenInput> &);
}
