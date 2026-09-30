#pragma once
#include <hs/renderer/vfx_distortion_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxDistortionInput> BuildVfxTypedDistortionCommands(
    const VfxProgramData &program, std::span<const VfxEventInput> events,
    std::span<const VfxPersistentInput> persistent, Tick current_tick);
}
