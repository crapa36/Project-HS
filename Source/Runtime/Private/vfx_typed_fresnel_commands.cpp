#include "vfx_typed_fresnel_commands.hpp"

#include <algorithm>
#include <cmath>

namespace hs::runtime_detail
{
namespace
{
template<class T>
bool InRange(VfxRange range, std::span<const T> values) noexcept
{
    return range.first <= values.size() && range.count <= values.size() - range.first;
}

bool FiniteTransform(const VfxTransform &transform) noexcept
{
    return std::all_of(transform.begin(), transform.end(),
                       [](float value) { return std::isfinite(value); });
}
}

std::vector<VfxFresnelInput> BuildVfxTypedFresnelCommands(
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs)
{
    std::vector<VfxFresnelInput> result;
    AppendVfxTypedFresnelCommands(image, inputs, result);
    return result;
}

void AppendVfxTypedFresnelCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, std::vector<VfxFresnelInput> &result)
{
    const auto original_size = result.size();
    try
    {
    for (const auto &owner : inputs)
    {
        const auto *entity = std::get_if<VfxEntityPayload>(&owner.payload);
        if (!entity || owner.effect_handle == 0 ||
            owner.effect_handle > image.fresnel_effect_recipes.size() || owner.stable_id == 0 ||
            entity->render_instance_id == 0 ||
            !FiniteTransform(owner.current_transform) || !FiniteTransform(owner.previous_transform) ||
            !std::isfinite(entity->footprint_radius) || entity->footprint_radius <= 0.0f ||
            !std::isfinite(entity->lifetime01) || entity->lifetime01 < 0.0f ||
            entity->lifetime01 >= 1.0f || !std::isfinite(entity->health_fraction) ||
            entity->health_fraction < 0.0f || entity->health_fraction > 1.0f ||
            !std::isfinite(owner.normalized_age) || owner.normalized_age != entity->lifetime01 ||
            !std::isfinite(owner.elapsed_seconds) || owner.elapsed_seconds < 0.0f ||
            static_cast<std::uint32_t>(owner.quality) > 2) continue;
        const auto &effect = image.fresnel_effect_recipes[owner.effect_handle - 1];
        if (!effect.route_valid || effect.effect_handle != owner.effect_handle ||
            !InRange(effect.outputs, image.fresnel_recipes)) continue;
        for (const auto &recipe : std::span(image.fresnel_recipes).subspan(
                 effect.outputs.first, effect.outputs.count))
        {
            if (recipe.effect_handle != effect.effect_handle || recipe.kind != effect.kind ||
                static_cast<std::uint32_t>(owner.quality) < recipe.min_quality ||
                owner.normalized_age < recipe.knot_start || owner.normalized_age >= recipe.knot_end)
                continue;
            const auto progress = (owner.normalized_age - recipe.knot_start) /
                                  (recipe.knot_end - recipe.knot_start);
            if (!std::isfinite(progress) || progress < 0.0f || progress >= 1.0f) continue;

            VfxFresnelInput command;
            command.render_instance_id = entity->render_instance_id;
            command.stable_id = owner.stable_id;
            command.effect_handle = effect.effect_handle;
            command.kind = recipe.kind;
            command.current_transform = owner.current_transform;
            command.previous_transform = owner.previous_transform;
            command.footprint_radius = entity->footprint_radius;
            command.source_progress = progress;
            command.elapsed_seconds = owner.elapsed_seconds;
            command.color = {recipe.color.x, recipe.color.y, recipe.color.z,
                             recipe.color.w};
            command.hdr = recipe.hdr;
            command.gradient_row = recipe.gradient_row;
            command.motion_rate_hz = recipe.motion_rate_hz;
            command.motion_amplitude = recipe.motion_amplitude;
            command.motion_inset_fraction = recipe.motion_inset_fraction;
            command.sector_count = recipe.sector_count;
            command.rotation_hz = recipe.rotation_hz;
            command.fresnel_power = recipe.fresnel_power;
            command.noise_amount = recipe.noise_amount;
            command.end_crack = recipe.end_crack;
            result.push_back(command);
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
