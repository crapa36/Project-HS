#pragma once

#include <hs/runtime/vfx_image.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace hs::runtime_detail
{
[[nodiscard]] constexpr std::uint32_t VfxRibbonBoundsHash32(std::string_view value) noexcept
{
    std::uint32_t hash = 2166136261u;
    for (const auto character : value)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 16777619u;
    }
    return hash;
}

struct VfxRibbonFrameBounds
{
    std::uint32_t event_sources{}, event_outputs{};
    std::uint32_t persistent_sources{}, persistent_outputs{};
};

// Pure recipe-count calculation. Cache one result per handle when preparing an
// image; this uses only native-free prepared spans and standard-library values.
[[nodiscard]] inline VfxRibbonFrameBounds VfxRibbonFrameBoundsForEffect(
    const VfxImage &image, VfxEffectHandle handle)
{
    VfxRibbonFrameBounds result{};
    if (handle == 0) return result;
    const auto checked_range = [](VfxRange range, std::size_t size) {
        if (range.first > size || range.count > size - range.first)
            throw std::logic_error("Prepared ribbon recipe range is invalid.");
        return range;
    };
    const auto add = [](std::uint32_t &target, std::uint32_t value) {
        if (value > std::numeric_limits<std::uint32_t>::max() - target)
            throw std::overflow_error("Prepared ribbon frame bound exceeds uint32 range.");
        target += value;
    };
    struct Bound { std::uint32_t sources{}, outputs{}; };
    const auto include_max = [](Bound &target, Bound candidate) {
        target.sources = std::max(target.sources, candidate.sources);
        target.outputs = std::max(target.outputs, candidate.outputs);
    };
    const auto count_link_outputs = [&](VfxRange range, std::uint32_t quality) {
        range = checked_range(range, image.ribbon_link_output_recipes.size());
        std::uint32_t count{};
        for (const auto &output : std::span(image.ribbon_link_output_recipes).subspan(
                 range.first, range.count))
            if (output.min_quality <= quality && output.details[quality].valid)
                add(count, 1);
        return count;
    };
    const auto link_bound = [&](const VfxRibbonLinkEffectRecipe &recipe) {
        Bound bound{};
        for (std::uint32_t quality = 0; quality <= 2; ++quality)
        {
            const auto outputs = count_link_outputs(recipe.outputs, quality);
            include_max(bound, {outputs == 0 ? 0u : 1u, outputs});
        }
        return bound;
    };

    // Persistent line sources may publish both the wake recipe and the
    // captured-line link. Every other payload follows one exclusive builder route.
    Bound persistent_line{};
    if (handle <= image.ribbon_wake_effect_ranges.size())
    {
        auto range = checked_range(image.ribbon_wake_effect_ranges[handle - 1],
                                   image.ribbon_wake_recipes.size());
        for (std::uint32_t quality = 0; quality <= 2; ++quality)
        {
            std::uint32_t eligible{};
            for (const auto &recipe : std::span(image.ribbon_wake_recipes).subspan(
                     range.first, range.count))
                if (recipe.effect_handle == handle && recipe.min_quality <= quality &&
                    recipe.detail_min_quality <= quality)
                    add(eligible, 1);
            // The builder accepts exactly one matching wake recipe. A second
            // match rejects the whole row, so the realizable maximum is one.
            if (eligible == 1) include_max(persistent_line, {1, 1});
        }
    }

    Bound persistent_link{};
    Bound captured_line{};
    if (handle <= image.ribbon_link_effect_recipes.size())
    {
        const auto &recipe = image.ribbon_link_effect_recipes[handle - 1];
        if (recipe.effect_handle == handle &&
            recipe.kind != VfxRibbonLinkKind::Unsupported &&
            recipe.kind != VfxRibbonLinkKind::PickupCollect &&
            recipe.kind != VfxRibbonLinkKind::ArrowRain)
            persistent_link = link_bound(recipe);
        if (recipe.effect_handle == handle && recipe.kind == VfxRibbonLinkKind::CapturedLine)
            captured_line = link_bound(recipe);
    }
    add(persistent_line.sources, captured_line.sources);
    add(persistent_line.outputs, captured_line.outputs);

    Bound projectile{};
    if (handle <= image.ribbon_projectile_effect_ranges.size())
    {
        auto range = checked_range(image.ribbon_projectile_effect_ranges[handle - 1],
                                   image.ribbon_projectile_sources.size());
        for (std::uint32_t quality = 0; quality <= 2; ++quality)
        {
            Bound at_quality{};
            for (const auto &source : std::span(image.ribbon_projectile_sources).subspan(
                     range.first, range.count))
            {
                const auto outputs = checked_range(source.outputs,
                    image.ribbon_projectile_outputs.size());
                std::uint32_t source_outputs{};
                for (const auto &output : std::span(image.ribbon_projectile_outputs).subspan(
                         outputs.first, outputs.count))
                    if (output.min_quality <= quality) add(source_outputs, 1);
                if (source_outputs != 0)
                {
                    add(at_quality.sources, 1);
                    add(at_quality.outputs, source_outputs);
                }
            }
            include_max(projectile, at_quality);
        }
    }
    include_max(projectile, persistent_link);
    include_max(persistent_line, projectile);
    result.persistent_sources = persistent_line.sources;
    result.persistent_outputs = persistent_line.outputs;

    Bound event_routes{};
    if (handle <= image.ribbon_cancel_effect_ranges.size())
    {
        auto range = checked_range(image.ribbon_cancel_effect_ranges[handle - 1],
                                   image.ribbon_cancel_recipes.size());
        for (std::uint32_t quality = 0; quality <= 2; ++quality)
        {
            Bound at_quality{};
            for (const auto &recipe : std::span(image.ribbon_cancel_recipes).subspan(
                     range.first, range.count))
                if (recipe.effect_handle == handle && recipe.min_quality <= quality)
                {
                    add(at_quality.sources, 1);
                    add(at_quality.outputs, 1);
                }
            include_max(event_routes, at_quality);
        }
    }
    if (handle <= image.ribbon_fixed_effect_ranges.size())
    {
        const auto range = checked_range(image.ribbon_fixed_effect_ranges[handle - 1],
                                         image.ribbon_fixed_recipes.size());
        if (range.count == 1)
        {
            const auto &recipe = image.ribbon_fixed_recipes[range.first];
            if (recipe.effect_handle == handle && recipe.kind != VfxRibbonFixedKind::Unsupported)
                for (std::uint32_t quality = 0; quality <= 2; ++quality)
                {
                    const auto outputs = static_cast<std::uint32_t>(recipe.core_min_quality <= quality) +
                        static_cast<std::uint32_t>(recipe.outer_min_quality <= quality);
                    include_max(event_routes, {outputs == 0 ? 0u : 1u, outputs});
                }
        }
    }
    if (handle <= image.ribbon_link_effect_recipes.size())
    {
        const auto &recipe = image.ribbon_link_effect_recipes[handle - 1];
        if (recipe.effect_handle == handle &&
            (recipe.kind == VfxRibbonLinkKind::PickupCollect ||
             recipe.kind == VfxRibbonLinkKind::ArrowRain))
            include_max(event_routes, link_bound(recipe));
    }
    if (handle <= image.ribbon_event_effect_recipes.size())
    {
        const auto &effect = image.ribbon_event_effect_recipes[handle - 1];
        if (effect.effect_handle == handle)
        {
            const auto sources = checked_range(effect.sources, image.ribbon_event_sources.size());
            const bool projectile_event = effect.payload_kind ==
                VfxRibbonBoundsHash32("ProjectilePayload");
            for (std::uint32_t quality = 0; quality <= 2; ++quality)
            {
                Bound at_quality{};
                for (const auto &source : std::span(image.ribbon_event_sources).subspan(
                         sources.first, sources.count))
                {
                    if (source.effect_handle != handle ||
                        (projectile_event && !source.axial_source)) continue;
                    const auto outputs = checked_range(source.outputs,
                        image.ribbon_event_outputs.size());
                    std::uint32_t source_outputs{};
                    for (const auto &output : std::span(image.ribbon_event_outputs).subspan(
                             outputs.first, outputs.count))
                        if ((!projectile_event || (output.axial &&
                                output.output.profile == VfxOutputProfile::RibbonAdd)) &&
                            (projectile_event || !output.axial) && output.min_quality <= quality)
                            add(source_outputs, 1);
                    if (source_outputs != 0)
                    {
                        add(at_quality.sources, 1);
                        add(at_quality.outputs, source_outputs);
                    }
                }
                include_max(event_routes, at_quality);
            }
        }
    }
    result.event_sources = event_routes.sources;
    result.event_outputs = event_routes.outputs;
    return result;
}
}
