#pragma once

#include <hs/render_data/vfx_fresnel_input.hpp>
#include <hs/runtime/vfx_image.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{
void AppendVfxTypedFresnelCommands(const VfxImage &image,
    const PreparedVfxFresnelRecipeView &recipes,
    std::span<const VfxPersistentInput> inputs, std::vector<VfxFresnelInput> &result);
}
