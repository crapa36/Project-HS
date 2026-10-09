#pragma once

#include "reconstruction_plan.hpp"
#include <hs/renderer/renderer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

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
    const auto scaled = [](std::uint32_t pixels, float percent, std::uint32_t &result) {
        const auto value = std::max(1.0, std::ceil(static_cast<double>(pixels) * percent / 100.0));
        if (value > std::numeric_limits<std::uint32_t>::max()) return false;
        result = static_cast<std::uint32_t>(value);
        return true;
    };
    ViewExtent minimum;
    if (!scaled(output.width, options.dynamic_max_percent, capacity.width) ||
        !scaled(output.height, options.dynamic_max_percent, capacity.height) ||
        !scaled(output.width, options.dynamic_min_percent, minimum.width) ||
        !scaled(output.height, options.dynamic_min_percent, minimum.height))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_reconstruction", "DRS render capacity overflows.");
    plan.render_active.width = std::clamp(plan.render_active.width, minimum.width, capacity.width);
    plan.render_active.height = std::clamp(plan.render_active.height, minimum.height, capacity.height);
    if (plan.effective_provider.value == "native_taa" && options.dynamic_min_percent != 100.0f)
    {
        plan.effective_provider.value = "internal_taau";
        plan.fallback_reason = "Dynamic resolution uses InternalTAAU; NativeTAA requires the output extent.";
        plan.fallback = ReconstructionFallback::NativeRequiresOutputExtent;
    }
    return Result::Success();
}
} // namespace hs
