#include "reconstruction_plan.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace hs
{
namespace
{
Result InvalidRequest(const char *message)
{
    return Result::Failure(ErrorCode::InvalidArgument, "hs_reconstruction", message);
}

bool IsExternalProvider(std::string_view provider)
{
    return provider == "dlss" || provider == "dlss_sr" || provider == "xess" ||
           provider == "xess_sr" || provider == "fsr" || provider == "fsr_sr" ||
           provider == "directsr";
}

bool InternalQualityScale(std::string_view quality, double &scale)
{
    if (quality == "native") scale = 1.0;
    else if (quality == "quality") scale = 2.0 / 3.0;
    else if (quality == "balanced") scale = 1.0 / 1.7;
    else if (quality == "performance") scale = 0.5;
    else return false;
    return true;
}

bool ScaledExtent(ViewExtent output, double scale, ViewExtent &extent)
{
    const auto dimension = [scale](std::uint32_t value, std::uint32_t &result) {
        const double pixels = std::max(1.0, std::round(value * scale));
        if (pixels > std::numeric_limits<std::uint32_t>::max()) return false;
        result = static_cast<std::uint32_t>(pixels);
        return true;
    };
    return dimension(output.width, extent.width) && dimension(output.height, extent.height);
}
} // namespace

Result ResolveInternalReconstruction(const ReconstructionRequest &request,
                                     ViewExtent output, ReconstructionPlan &plan)
{
    if (output.width == 0 || output.height == 0)
        return InvalidRequest("Reconstruction output extent must be nonzero.");
    if (request.resolution_mode != GraphicsResolutionMode::Manual &&
        request.resolution_mode != GraphicsResolutionMode::ProviderQuality)
        return InvalidRequest("Reconstruction resolution mode is invalid.");
    if (request.resolution_mode == GraphicsResolutionMode::Manual &&
        (!std::isfinite(request.manual_scale_percent) || request.manual_scale_percent < 50.0f ||
         request.manual_scale_percent > 200.0f))
        return InvalidRequest("Manual reconstruction scale must be in [50, 200].");

    ReconstructionPlan candidate;
    candidate.requested_provider = request.provider;
    candidate.requested_quality = request.quality;
    candidate.output_extent = output;
    candidate.temporal_enabled = request.temporal_enabled && request.provider.value != "off";
    candidate.requested_availability = ReconstructionAvailability::Available;
    const bool native = request.provider.value == "native_taa";
    const bool off = request.provider.value == "off";
    const bool internal = request.provider.value == "internal_taau";
    const bool automatic = request.provider.value == "auto";
    candidate.effective_provider.value = native ? "native_taa" : (off ? "off" : "internal_taau");

    if (!native && !off && !internal && !automatic)
    {
        const bool known = IsExternalProvider(request.provider.value);
        candidate.requested_availability = known ? ReconstructionAvailability::NotIntegrated
                                                 : ReconstructionAvailability::Unavailable;
        candidate.fallback = known ? ReconstructionFallback::ProviderNotIntegrated
                                   : ReconstructionFallback::UnknownProvider;
        candidate.fallback_reason = known
            ? "Requested external reconstruction provider has no connected execution path."
            : "Requested reconstruction provider ID is unknown.";
    }

    double scale = 1.0;
    if (request.resolution_mode == GraphicsResolutionMode::Manual)
    {
        scale = request.manual_scale_percent * 0.01;
        candidate.effective_quality.value = "manual";
        // NativeTAA is a native-resolution mode. Arbitrary render extents use the
        // already connected InternalTAAU kernel; do not multiply a quality ratio.
        if (native && request.manual_scale_percent != 100.0f)
        {
            candidate.effective_provider.value = "internal_taau";
            candidate.fallback = ReconstructionFallback::NativeRequiresOutputExtent;
            candidate.fallback_reason = "NativeTAA requires 100 percent; manual scale uses InternalTAAU.";
        }
        else if (native || (off && request.manual_scale_percent == 100.0f))
            candidate.effective_quality.value = "native";
    }
    else if (native || off)
    {
        candidate.effective_quality.value = "native";
        if (native && request.quality.value != "native")
        {
            candidate.fallback = ReconstructionFallback::UnsupportedQuality;
            candidate.fallback_reason = "NativeTAA provides only Native quality.";
        }
    }
    else
    {
        candidate.effective_quality = request.quality;
        if (!InternalQualityScale(request.quality.value, scale))
        {
            candidate.effective_quality.value = "quality";
            scale = 2.0 / 3.0;
            if (candidate.fallback == ReconstructionFallback::None)
            {
                candidate.fallback = ReconstructionFallback::UnsupportedQuality;
                candidate.fallback_reason = "Requested quality is unsupported by InternalTAAU; using Quality.";
            }
        }
    }
    if (!ScaledExtent(output, scale, candidate.render_active))
        return InvalidRequest("Reconstruction render extent overflows.");
    candidate.effective_scale_percent = static_cast<float>(scale * 100.0);
    plan = std::move(candidate);
    return Result::Success();
}
} // namespace hs
