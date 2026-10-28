#include "vfx_typed_mesh_commands.hpp"

#include <cmath>

namespace hs::runtime_detail
{
namespace
{
bool Finite(Float3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

template<class T>
bool InRange(VfxRange range, std::span<const T> values) noexcept
{
    return range.first <= values.size() && range.count <= values.size() - range.first;
}
}

std::vector<VfxOwnedMeshInput> BuildVfxTypedMeshCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> inputs)
{
    std::vector<VfxOwnedMeshInput> result;
    AppendVfxTypedMeshCommands(image, inputs, result);
    return result;
}

void AppendVfxTypedMeshCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, std::vector<VfxOwnedMeshInput> &result)
{
    const auto original_size = result.size();
    try
    {
    for (const auto &input : inputs)
    {
        const auto *owner = std::get_if<VfxProjectilePayload>(&input.payload);
        if (!owner || input.effect_handle == 0 ||
            input.effect_handle > image.mesh_effect_recipes.size())
            continue;
        const auto &effect = image.mesh_effect_recipes[input.effect_handle - 1];
        if (!effect.route_valid || effect.effect_handle != input.effect_handle ||
            !InRange(effect.outputs, image.mesh_recipes)) continue;
        const Float3 position{input.current_transform[12], input.current_transform[13],
                              input.current_transform[14]};
        const Float3 previous{input.previous_transform[12], input.previous_transform[13],
                              input.previous_transform[14]};
        if (!Finite(position) || !Finite(previous) || !Finite(owner->velocity) ||
            !std::isfinite(owner->hitbox_radius) || owner->hitbox_radius <= 0.0f) continue;
        for (const auto &recipe : std::span(image.mesh_recipes).subspan(
                 effect.outputs.first, effect.outputs.count))
        {
            if (recipe.effect_handle != effect.effect_handle ||
                recipe.min_quality > static_cast<std::uint32_t>(input.quality)) continue;
            result.push_back({input.stable_id, position, previous, owner->velocity,
                recipe.color, owner->hitbox_radius, recipe.hdr, recipe.fresnel,
                recipe.mesh_index, recipe.gradient_row, recipe.head_to_tail_ratio,
                recipe.shell_thickness});
        }
    }
    }
    catch (...)
    {
        result.resize(original_size);
        throw;
    }
}
}
