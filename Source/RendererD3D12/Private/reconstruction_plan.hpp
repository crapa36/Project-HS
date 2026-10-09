#pragma once

#include "view_state.hpp"

#include <hs/core/graphics_settings.hpp>
#include <hs/core/result.hpp>

#include <string>

namespace hs
{
// NotIntegrated is a source implementation gap, not a device capability result.
// CompiledOut becomes meaningful only when an implemented adapter is excluded.
enum class ReconstructionAvailability : std::uint8_t
{
    Available,
    CompiledOut,
    Unsupported,
    Unavailable,
    NotIntegrated,
};

enum class ReconstructionFallback : std::uint8_t
{
    None,
    ProviderNotIntegrated,
    UnknownProvider,
    UnsupportedQuality,
    NativeRequiresOutputExtent,
    ProviderUnavailable,
};

struct ReconstructionRequest
{
    GraphicsProviderId provider{"auto"};
    GraphicsModeId quality{"quality"};
    GraphicsResolutionMode resolution_mode{GraphicsResolutionMode::ProviderQuality};
    float manual_scale_percent{100.0f};
    bool temporal_enabled{true};
};

struct ReconstructionPlan
{
    GraphicsProviderId requested_provider;
    GraphicsModeId requested_quality;
    GraphicsProviderId effective_provider;
    GraphicsModeId effective_quality;
    ViewExtent output_extent;
    ViewExtent render_active;
    ViewExtent input_minimum, input_maximum;
    float effective_scale_percent{100.0f};
    bool temporal_enabled{};
    ReconstructionAvailability requested_availability{ReconstructionAvailability::Unavailable};
    ReconstructionFallback fallback{ReconstructionFallback::None};
    std::string fallback_reason;
};

[[nodiscard]] inline bool UsesInternalResolveTargets(const ReconstructionPlan &plan) noexcept
{
    return plan.effective_provider.value == "internal_taau" ||
           plan.effective_provider.value == "native_taa" ||
           plan.effective_provider.value == "off";
}

// Resolves the existing output-history temporal/spatial kernel. It does not
// create SDK contexts or claim that an external provider executes.
[[nodiscard]] Result ResolveInternalReconstruction(const ReconstructionRequest &request,
                                                   ViewExtent output,
                                                   ReconstructionPlan &plan);
} // namespace hs
