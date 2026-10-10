#pragma once

#include "reconstruction_plan.hpp"
#include <hs/renderer/renderer.hpp>

#include <algorithm>

namespace hs
{
template<class Options>
Result ResolveNativeReconstruction(const Options &options, ViewExtent output,
                                  ReconstructionPlan &plan, ViewExtent &capacity)
{
    ReconstructionRequest request;
    request.temporal_enabled = options.temporal_reconstruction;
    if (options.reconstruction_request)
    {
        const auto &authored = *options.reconstruction_request;
        request.provider = authored.provider;
        request.quality = authored.quality;
        request.resolution_mode = authored.resolution_mode;
        request.manual_scale_percent = authored.manual_scale_percent;
    }
    else
    {
        request.provider.value = options.temporal_reconstruction ? "internal_taau" : "off";
        request.quality.value = "native";
        request.resolution_mode = GraphicsResolutionMode::Manual;
        request.manual_scale_percent = static_cast<float>(std::clamp(options.render_scale_percent, 50u, 200u));
    }
    if (auto resolved = ResolveInternalReconstruction(request, output, plan); !resolved) return resolved;
    capacity = plan.render_active;
    if (!options.dynamic_resolution) return Result::Success();
    ViewExtent minimum;
    if (options.dynamic_min_percent > options.dynamic_max_percent ||
        !ResolveDynamicResolutionExtent(output, options.dynamic_min_percent, minimum) ||
        !ResolveDynamicResolutionExtent(output, options.dynamic_max_percent, capacity))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_reconstruction",
            "DRS bounds must be finite, positive, ordered and fit integer render extents.");
    plan.render_active.width = std::clamp(plan.render_active.width, minimum.width, capacity.width);
    plan.render_active.height = std::clamp(plan.render_active.height, minimum.height, capacity.height);
    plan.input_minimum = minimum;
    plan.input_maximum = capacity;
    plan.effective_scale_percent = 100.0f * plan.render_active.width / output.width;
    if (plan.effective_provider.value == "native_taa" && options.dynamic_min_percent != 100.0f)
    {
        plan.effective_provider.value = "internal_taau";
        plan.fallback_reason = "Dynamic resolution uses InternalTAAU; NativeTAA requires the output extent.";
        plan.fallback = ReconstructionFallback::NativeRequiresOutputExtent;
    }
    return Result::Success();
}
} // namespace hs
