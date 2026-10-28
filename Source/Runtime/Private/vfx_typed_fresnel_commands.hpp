#pragma once

#include <hs/render_data/vfx_fresnel_input.hpp>
#include <hs/runtime/vfx_image.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxFresnelInput> BuildVfxTypedFresnelCommands(
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs);
void AppendVfxTypedFresnelCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, std::vector<VfxFresnelInput> &result);
}
