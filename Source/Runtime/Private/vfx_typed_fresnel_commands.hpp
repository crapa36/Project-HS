#pragma once

#include <hs/renderer/vfx_fresnel_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxFresnelInput> BuildVfxTypedFresnelCommands(
    const VfxProgramData &program,
    std::span<const VfxPersistentInput> inputs);
}
