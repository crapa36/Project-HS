#pragma once

#include <hs/render_data/vfx_fresnel_input.hpp>
#include <hs/renderer/vfx_program.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxFresnelInput> BuildVfxTypedFresnelCommands(
    const VfxProgramView &program,
    std::span<const VfxPersistentInput> inputs);
}
