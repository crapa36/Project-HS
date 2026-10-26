#include "renderer_impl.hpp"
#include "renderer_options.hpp"
#include "renderer_packed_assets.hpp"
#include "renderer_xess_fg.hpp"
#include "renderer_xell.hpp"

#include <hs/core/graphics_settings_request.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <string_view>
#include <utility>

namespace hs
{

Result D3D12Renderer::Impl::LoadQualityProfiles(const RendererConfig &source)
{
    RendererAssetSource path;
    if (auto resolved = ResolveRendererAsset(source, render_data::kQualityProfilesProduct, path); !resolved) return resolved;
    std::vector<std::byte> bytes;
    if (auto read = ReadBinary(path, bytes); !read) return read;
    try
    {
        const auto selected = RendererProductDigest(source, render_data::kQualityProfilesProduct);
        if (selected.empty())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Selected Generation omits quality product digest.");
        constexpr char hex[] = "0123456789abcdef";
        std::string digest = "sha256:";
        for (const auto value : path.pack ? path.range.expected_digest : render_data::Sha256(bytes))
        {
            const auto byte = std::to_integer<unsigned>(value);
            digest += hex[byte >> 4]; digest += hex[byte & 15];
        }
        if (digest != selected)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Selected quality product SHA256 differs from Generation.");
        quality_profiles = std::make_shared<const render_data::QualityProfiles>(render_data::DecodeQualityProfiles(bytes));
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what()); }
}
float D3D12Renderer::DecorativeParticleFraction() const noexcept
{
    if (!impl_->quality_profiles) return 0;
    const auto quality = impl_->last_applied_graphics_request_available
        ? impl_->last_applied_graphics_request.particle_quality : impl_->config.requested_graphics
            ? impl_->config.requested_graphics->particle_quality : GraphicsQuality::High;
    return impl_->quality_profiles->profiles[static_cast<std::size_t>(quality)].decorative_particle_fraction;
}

RendererOptions ProjectRendererOptions(const GraphicsSettings &graphics,
                                       const RendererConfig &legacy_defaults,
                                       const render_data::QualityProfiles &profiles)
{
    RendererOptions options;
    options.vsync = graphics.vsync;
    options.bloom = graphics.bloom;
    options.outline = graphics.outline;
    options.render_scale_percent = legacy_defaults.render_scale_percent;
    if (graphics.resolution_mode == GraphicsResolutionMode::Manual)
        options.render_scale_percent = static_cast<std::uint32_t>(
            std::lround(std::clamp(graphics.manual_scale_percent, 50.0f, 200.0f)));
    options.dynamic_resolution = graphics.dynamic_resolution;
    options.target_render_fps = graphics.target_render_fps;
    options.dynamic_min_percent = graphics.dynamic_min_percent;
    options.dynamic_max_percent = graphics.dynamic_max_percent;
    options.shadow_resolution = profiles.profiles.at(static_cast<std::size_t>(graphics.shadow_quality)).shadow_resolution;
    options.particle_percentage = static_cast<std::uint32_t>(std::lround(100.0f *
        profiles.profiles.at(static_cast<std::size_t>(graphics.particle_quality)).decorative_particle_fraction));
    options.vfx_quality = graphics.particle_quality == GraphicsQuality::Low ? VfxQuality::Low
        : graphics.particle_quality == GraphicsQuality::Medium ? VfxQuality::Medium : VfxQuality::High;
    options.anisotropic_filtering = graphics.anisotropic_filtering;
    options.bloom_strength = graphics.bloom_strength;
    options.outline_width_px = graphics.outline_width_px;
    options.color_grading = graphics.color_grading;
    options.motion_blur = graphics.motion_blur;
    options.adaptive_shading = graphics.adaptive_shading;
    // This setting belongs to the legacy UI/audio document and is deliberately
    // carried from the existing renderer configuration.
    options.screen_overlay_intensity = legacy_defaults.screen_overlay_intensity;
    options.depth_of_field = graphics.depth_of_field;
    options.shadow_distance_m = graphics.shadow_distance_m;
    options.texture_quality = graphics.texture_quality;
    options.texture_budget_mib = graphics.texture_budget_mib;
    options.geometry_quality = graphics.geometry_quality;
    options.foliage_quality = graphics.foliage_quality;
    options.material_quality = graphics.material_quality;
    options.pom_quality = graphics.pom_quality;
    options.lighting_quality = graphics.lighting_quality;
    options.ao_mode = graphics.ao_mode;
    options.ao_quality = graphics.ao_quality;
    options.contact_shadows = graphics.contact_shadows;
    options.reflection_mode = graphics.reflection_mode;
    options.reflection_quality = graphics.reflection_quality;
    options.transparency_quality = graphics.transparency_quality;
    options.refraction_quality = graphics.refraction_quality;
    options.volumetric_quality = graphics.volumetric_quality;
    options.gi_mode = graphics.gi_mode;
    options.gi_quality = graphics.gi_quality;
    options.shadow_mode = graphics.shadow_mode;
    options.shadow_quality = graphics.shadow_quality;
    options.temporal_reconstruction = graphics.reconstruction_provider.value != "off";
    options.reconstruction_request = RendererReconstructionRequest{
        graphics.reconstruction_provider, graphics.reconstruction_quality,
        graphics.resolution_mode, graphics.manual_scale_percent};
    options.presentation_request = {graphics.frame_generation_provider,
        graphics.frame_generation_mode, graphics.frame_generation_multiplier,
        graphics.low_latency, graphics.frame_limit_fps};
    options.sharpening = graphics.sharpening;
    options.hdr_mode = graphics.hdr_mode;
    options.hdr_paper_white_nits = graphics.hdr_paper_white_nits;
    options.hdr_peak_nits = graphics.hdr_peak_nits;
    // Startup may provide a concrete output resolved from the window monitor
    // for canonical Auto. An explicit request always owns its output choice.
    options.display_output = graphics.display_output.value == "auto" &&
        !legacy_defaults.display_output.value.empty() &&
        legacy_defaults.display_output.value != "auto"
        ? legacy_defaults.display_output : graphics.display_output;
    return options;
}

void ApplyProjectedRendererOptions(RendererConfig &config, const RendererOptions &options)
{
    config.vsync = options.vsync;
    config.bloom = options.bloom;
    config.outline = options.outline;
    config.render_scale_percent = options.render_scale_percent;
    config.dynamic_resolution = options.dynamic_resolution;
    config.target_render_fps = options.target_render_fps;
    config.dynamic_min_percent = options.dynamic_min_percent;
    config.dynamic_max_percent = options.dynamic_max_percent;
    config.shadow_resolution = options.shadow_resolution;
    config.particle_percentage = options.particle_percentage;
    config.vfx_quality = options.vfx_quality;
    config.anisotropic_filtering = options.anisotropic_filtering;
    config.bloom_strength = options.bloom_strength;
    config.outline_width_px = options.outline_width_px;
    config.color_grading = options.color_grading;
    config.motion_blur = options.motion_blur;
    config.depth_of_field = options.depth_of_field;
    config.shadow_distance_m = options.shadow_distance_m;
    config.texture_quality = options.texture_quality;
    config.texture_budget_mib = options.texture_budget_mib;
    config.geometry_quality = options.geometry_quality;
    config.foliage_quality = options.foliage_quality;
    config.material_quality = options.material_quality;
    config.pom_quality = options.pom_quality;
    config.lighting_quality = options.lighting_quality;
    config.adaptive_shading = options.adaptive_shading;
    config.screen_overlay_intensity = options.screen_overlay_intensity;
    config.ao_mode = options.ao_mode;
    config.ao_quality = options.ao_quality;
    config.contact_shadows = options.contact_shadows;
    config.reflection_mode = options.reflection_mode;
    config.reflection_quality = options.reflection_quality;
    config.transparency_quality = options.transparency_quality;
    config.refraction_quality = options.refraction_quality;
    config.volumetric_quality = options.volumetric_quality;
    config.gi_mode = options.gi_mode;
    config.gi_quality = options.gi_quality;
    config.shadow_mode = options.shadow_mode;
    config.shadow_quality = options.shadow_quality;
    config.temporal_reconstruction = options.temporal_reconstruction;
    config.reconstruction_request = options.reconstruction_request;
    config.presentation_request = options.presentation_request;
    config.sharpening = options.sharpening;
    config.hdr_mode = options.hdr_mode;
    config.hdr_paper_white_nits = options.hdr_paper_white_nits;
    config.hdr_peak_nits = options.hdr_peak_nits;
    config.display_output = options.display_output;
}

namespace
{

GraphicsCapabilityReason CapabilityReason(std::string_view availability,
                                          std::string_view reason)
{
    if (availability == "available") return GraphicsCapabilityReason::None;
    if (availability == "unsupported" || reason.find("unsupported") != std::string_view::npos)
        return GraphicsCapabilityReason::Unsupported;
    if (reason.find("Cannot load") != std::string_view::npos ||
        reason.find("missing export") != std::string_view::npos ||
        reason.find("missing a required") != std::string_view::npos ||
        reason.find("missing provider/plugin") != std::string_view::npos ||
        reason.find("distribution is incomplete") != std::string_view::npos ||
        reason.find("provider ID is unknown") != std::string_view::npos)
        return GraphicsCapabilityReason::MissingProvider;
    if (reason.find("display") != std::string_view::npos ||
        reason.find("monitor") != std::string_view::npos)
        return GraphicsCapabilityReason::MissingDisplay;
    if (reason.find("budget") != std::string_view::npos)
        return GraphicsCapabilityReason::BudgetLimited;
    if (availability == "unavailable" || availability == "compiled_out")
        return GraphicsCapabilityReason::Unavailable;
    return GraphicsCapabilityReason::Rejected;
}

void AddProvider(GraphicsCapabilitiesView &view, GraphicsProviderId provider,
                 bool available, GraphicsCapabilityReason reason)
{
    if (provider.value.empty()) return;
    const auto found = std::find_if(view.providers.begin(), view.providers.end(),
        [&](const auto &entry) { return entry.provider == provider; });
    if (found == view.providers.end())
        view.providers.push_back({std::move(provider), available,
                                  available ? GraphicsCapabilityReason::None : reason});
    else if (available)
    {
        found->available = true;
        found->reason = GraphicsCapabilityReason::None;
    }
}

void AddOption(GraphicsCapabilitiesView &view, GraphicsModeId option,
               bool available, GraphicsCapabilityReason reason)
{
    if (option.value.empty()) return;
    const auto found = std::find_if(view.options.begin(), view.options.end(),
        [&](const auto &entry) { return entry.option == option; });
    if (found == view.options.end())
        view.options.push_back({std::move(option), available, available ? GraphicsCapabilityReason::None : reason});
    else if (available)
    {
        found->available = true;
        found->reason = GraphicsCapabilityReason::None;
    }
}

struct CapabilityAdmission
{
    bool available{};
    GraphicsCapabilityReason reason{GraphicsCapabilityReason::Unavailable};
};
CapabilityAdmission Admission(ReconstructionAvailability availability, std::string_view reason)
{
    if (availability == ReconstructionAvailability::Available) return {true, GraphicsCapabilityReason::None};
    return {false, CapabilityReason(availability == ReconstructionAvailability::Unsupported ? "unsupported" :
        availability == ReconstructionAvailability::CompiledOut ? "compiled_out" : "unavailable", reason)};
}
CapabilityAdmission QueryAdmission(const Result &result, ReconstructionAvailability availability, std::string_view reason)
{
    if (result) return Admission(availability, reason);
    if (result.Code() == ErrorCode::InvalidArgument) return {false, GraphicsCapabilityReason::Unsupported};
    auto admission = Admission(availability, reason.empty() ? result.Message() : reason);
    if (admission.available) admission = {false, CapabilityReason("unavailable", result.Message())};
    return admission;
}
CapabilityAdmission Both(CapabilityAdmission a, CapabilityAdmission b)
{ return a.available ? b : a; }
CapabilityAdmission Either(CapabilityAdmission a, CapabilityAdmission b)
{ return a.available ? a : b.available ? b : a; }

GraphicsSettings ActualAppliedGraphics(const GraphicsSettings &requested,
                                       const RendererConfig &config,
                                       const DisplayState &display,
                                       const RendererLightingStatus &lighting,
                                       const RendererReconstructionStatus &reconstruction,
                                       const RendererPresentationStatus &presentation)
{
    auto actual = requested;
    actual.vsync = config.vsync;
    actual.frame_limit_fps = presentation.requested.frame_limit_fps;
    actual.hdr_mode = display.hdr_active ? GraphicsHdrMode::On : GraphicsHdrMode::Off;
    actual.hdr_paper_white_nits = display.paper_white_nits;
    actual.hdr_peak_nits = display.peak_nits;
    actual.dynamic_resolution = config.dynamic_resolution;
    actual.target_render_fps = config.target_render_fps;
    actual.dynamic_min_percent = config.dynamic_min_percent;
    actual.dynamic_max_percent = config.dynamic_max_percent;
    actual.sharpening = config.sharpening;
    // Configured policy is stable; SDK present counts remain separate telemetry.
    actual.frame_generation_multiplier = presentation.requested.multiplier;
    actual.low_latency = presentation.low_latency_active ? GraphicsAutoToggle::On : GraphicsAutoToggle::Off;
    actual.texture_quality = config.texture_quality;
    actual.texture_budget_mib = config.texture_budget_mib;
    actual.anisotropic_filtering = config.anisotropic_filtering;
    actual.geometry_quality = config.geometry_quality;
    actual.foliage_quality = config.foliage_quality;
    actual.material_quality = config.material_quality;
    actual.pom_quality = config.pom_quality;
    actual.lighting_quality = config.lighting_quality;
    actual.shadow_mode = lighting.active.shadow_mode;
    actual.shadow_quality = config.shadow_quality;
    actual.shadow_distance_m = config.shadow_distance_m;
    actual.contact_shadows = config.contact_shadows;
    actual.ao_mode = lighting.active.ao_mode;
    actual.ao_quality = config.ao_quality;
    actual.gi_mode = lighting.active.gi_mode;
    actual.gi_quality = config.gi_quality;
    actual.reflection_mode = lighting.active.reflection_mode;
    actual.reflection_quality = config.reflection_quality;
    actual.transparency_quality = config.transparency_quality;
    actual.refraction_quality = config.refraction_quality;
    actual.volumetric_quality = config.volumetric_quality;
    actual.bloom = config.bloom;
    actual.bloom_strength = config.bloom_strength;
    actual.motion_blur = config.motion_blur;
    actual.depth_of_field = config.depth_of_field;
    actual.outline = config.outline;
    actual.outline_width_px = config.outline_width_px;
    actual.color_grading = config.color_grading;
    actual.adaptive_shading = config.adaptive_shading;
    actual.particle_quality = requested.particle_quality;
    actual.display_output = display.native_output_id.empty()
        ? config.display_output : GraphicsDisplayId{display.native_output_id};
    actual.reconstruction_provider = reconstruction.effective_provider;
    actual.reconstruction_quality = reconstruction.effective_quality;
    actual.output_extent = GraphicsExtent{reconstruction.output_extent.width,
                                          reconstruction.output_extent.height};
    actual.resolution_mode = config.reconstruction_request
        ? config.reconstruction_request->resolution_mode : requested.resolution_mode;
    actual.manual_scale_percent = reconstruction.render_extent.width && reconstruction.output_extent.width
        ? 100.0f * static_cast<float>(reconstruction.render_extent.width) /
            static_cast<float>(reconstruction.output_extent.width)
        : requested.manual_scale_percent;
    actual.frame_generation_provider = presentation.effective_provider;
    actual.frame_generation_mode = presentation.active_mode;
    return actual;
}

GraphicsSettingsResolutionSummary ResolveSummary(
    const GraphicsSettings &requested,
    const RendererReconstructionStatus &reconstruction,
    const RendererPresentationStatus &presentation)
{
    GraphicsSettingsResolutionSummary summary;
    summary.resolved_profile = requested.preset;
    summary.native_provider = reconstruction.effective_provider;
    summary.used_native_fallback =
        (!requested.reconstruction_provider.value.empty() &&
         requested.reconstruction_provider.value != "auto" &&
         reconstruction.effective_provider != requested.reconstruction_provider) ||
        reconstruction.effective_quality != requested.reconstruction_quality ||
        (!requested.frame_generation_provider.value.empty() &&
         requested.frame_generation_provider.value != "auto" &&
         presentation.effective_provider != requested.frame_generation_provider) ||
        presentation.active_mode != requested.frame_generation_mode ||
        !reconstruction.fallback_reason.empty() || !presentation.fallback_reason.empty();
    if (!reconstruction.fallback_reason.empty())
        summary.native_fallback_reason = CapabilityReason(
            reconstruction.requested_availability, reconstruction.fallback_reason);
    else if (!presentation.fallback_reason.empty())
        summary.native_fallback_reason = CapabilityReason(
            presentation.availability, presentation.fallback_reason);
    return summary;
}

} // namespace

Result D3D12Renderer::InitializeGraphicsSettings(const GraphicsSettings &requested)
{
    try
    {
        const auto reconstruction = GetReconstructionStatus();
        const auto presentation = GetPresentationStatus();
        const auto lighting = GetLightingStatus();
        const auto actual = ActualAppliedGraphics(requested, impl_->config,
            impl_->display_state, lighting, reconstruction, presentation);
        SettingsApplyResult status;
        status.generation = 0;
        status.state = SettingsApplyState::Applied;
        status.requested = requested;
        status.actual_applied = actual;
        status.actual_applied_available = true;
        status.resolution = ResolveSummary(requested, reconstruction, presentation);
        status.confirmation_required = false;
        status.error = ErrorCode::None;
        impl_->applied_graphics = actual;
        impl_->applied_graphics_available = true;
        impl_->last_applied_graphics_request = requested;
        impl_->last_applied_graphics_request_available = true;
        impl_->graphics_settings_status = std::move(status);
        return Result::Success();
    }
    catch (const std::bad_alloc &)
    {
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               "Initial graphics settings status allocation failed.");
    }
}

Result D3D12Renderer::RequestGraphicsSettings(const GraphicsSettingsRequest &request)
{
    if (!impl_ || !impl_->initialized)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");

    const auto reject = [&](ErrorCode error, std::string_view message) {
        const bool owns_pending_request =
            impl_->graphics_settings_status.state == SettingsApplyState::Preparing;
        impl_->graphics_settings_status.generation = request.generation;
        impl_->graphics_settings_status.state = SettingsApplyState::Rejected;
        if (!owns_pending_request)
            impl_->graphics_settings_status.requested = request.requested;
        impl_->graphics_settings_status.confirmation_required = false;
        impl_->graphics_settings_status.error = error;
        return Result::Failure(error, "hs_renderer_d3d12", std::string(message));
    };

    switch (request.cause)
    {
    case GraphicsSettingsRequestCause::User:
    case GraphicsSettingsRequestCause::System:
    case GraphicsSettingsRequestCause::Recovery:
        break;
    default:
        return reject(ErrorCode::InvalidArgument, "Graphics settings request cause is invalid.");
    }
    if (request.cause != GraphicsSettingsRequestCause::Recovery &&
        (request.generation == 0 ||
         request.generation <= impl_->graphics_settings_generation_high_watermark))
    {
        // A stale transport message is not a new receipt.  Keep the current
        // status generation intact while the independent high-water mark
        // continues to reject duplicates.
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Graphics settings generation is stale.");
    }
    if (request.generation > impl_->graphics_settings_generation_high_watermark)
        impl_->graphics_settings_generation_high_watermark = request.generation;
    if (!ValidateGraphicsSettings(request.requested).empty())
        return reject(ErrorCode::InvalidArgument, "Graphics settings request is invalid.");
    if (request.resolved_display_output &&
        (request.resolved_display_output->value.empty() ||
         request.resolved_display_output->value == "auto"))
        return reject(ErrorCode::InvalidArgument,
                      "Resolved display output must identify a concrete display.");

    try
    {
        // Own the full request before entering the native prepare/commit
        // transaction so status publication cannot allocate halfway through
        // a resource change.
        const auto requested = request.requested;
        auto next_applied_request = requested;
        SettingsApplyResult pending;
        pending.generation = request.generation;
        pending.state = SettingsApplyState::Preparing;
        pending.requested = requested;
        pending.actual_applied = impl_->applied_graphics;
        pending.actual_applied_available = impl_->applied_graphics_available;
        impl_->graphics_settings_status = std::move(pending);

        auto options = ProjectRendererOptions(requested, impl_->config, *impl_->quality_profiles);
        if (request.resolved_display_output)
            options.display_output = *request.resolved_display_output;
        if (auto applied = ApplyOptions(options); !applied)
            return reject(applied.Code(), applied.Message());

        // Native prepare/commit has completed.  Publish only data already
        // owned before entering it; detailed actual fields are queried later.
        impl_->last_applied_graphics_request = std::move(next_applied_request);
        impl_->last_applied_graphics_request_available = true;
        impl_->applied_graphics_available = false;
        impl_->graphics_settings_status.generation = request.generation;
        impl_->graphics_settings_status.state = request.cause == GraphicsSettingsRequestCause::Recovery
            ? SettingsApplyState::Restored : SettingsApplyState::Applied;
        // pending.requested already owns the full canonical request.
        impl_->graphics_settings_status.actual_applied_available = false;
        impl_->graphics_settings_status.confirmation_required = false;
        impl_->graphics_settings_status.error = ErrorCode::None;
        return Result::Success();
    }
    catch (const std::bad_alloc &)
    {
        if (impl_->graphics_settings_status.state == SettingsApplyState::Preparing)
            return reject(ErrorCode::TaskFailed, "Graphics settings request allocation failed.");
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               "Graphics settings request allocation failed.");
    }
}

SettingsApplyResult D3D12Renderer::GetGraphicsSettingsStatus() const
{
    if (!impl_) return {};
    SettingsApplyResult status;
    try
    {
        // Copy the basic receipt first.  Native state queries below are
        // advisory detail and must never rewrite a successful commit.
        status = impl_->graphics_settings_status;
        if (!impl_->last_applied_graphics_request_available)
            return status;

        const auto &requested = impl_->last_applied_graphics_request;
        const auto reconstruction = GetReconstructionStatus();
        const auto presentation = GetPresentationStatus();
        const auto lighting = GetLightingStatus();
        status.actual_applied = ActualAppliedGraphics(requested, impl_->config,
            impl_->display_state, lighting, reconstruction, presentation);
        status.actual_applied_available = true;
        status.resolution = ResolveSummary(requested, reconstruction, presentation);
        return status;
    }
    catch (const std::bad_alloc &)
    {
        // Preserve the already-copied state/generation.  In particular, do
        // not convert a native Applied/Restored result into Rejected here.
        return status;
    }
}

GraphicsCapabilitiesView D3D12Renderer::GetGraphicsCapabilities() const
{
    GraphicsCapabilitiesView view;
    if (!impl_ || !impl_->initialized) return view;
    const auto &cfg = impl_->config;
    const auto sr_requested = cfg.reconstruction_request ? cfg.reconstruction_request->provider.value : "internal_taau";
    const auto sr_selected = sr_requested == "auto" ? impl_->reconstruction_plan.effective_provider.value : sr_requested;
    const auto &fg_requested = cfg.presentation_request.provider.value;
    const ViewExtent output{impl_->width, impl_->height};
    const auto key = sr_requested + "|" + sr_selected + "|" + fg_requested + "|" +
        std::to_string(output.width) + "x" + std::to_string(output.height) + "|" +
        std::string(GraphicsEnumToken(cfg.presentation_request.mode)) + "|" +
        std::string(GraphicsEnumToken(cfg.presentation_request.low_latency)) + "|" +
        (cfg.vsync ? "vsync" : "immediate");
    if (impl_->graphics_capabilities_key != key)
    {
        // SDK discovery is CPU/device-only and copied into an immutable catalog.
        // Repeated UI reads create no SDK objects. A changed output/provider
        // queries the real quality/mode domain again without dummy evaluations.
        constexpr CapabilityAdmission available{true, GraphicsCapabilityReason::None};
        constexpr CapabilityAdmission unsupported{false, GraphicsCapabilityReason::Unsupported};
        constexpr CapabilityAdmission incompatible{false, GraphicsCapabilityReason::Incompatible};
        constexpr std::array<std::string_view, 7> qualities{
            "native", "ultra_quality_plus", "ultra_quality", "quality", "balanced", "performance", "ultra_performance"};
        std::array<CapabilityAdmission, qualities.size()> dlss_quality{}, fsr_quality{}, xess_quality{}, dsr_quality{};
        auto &sl = impl_->streamline_session;
        const auto sl_admission = [&](StreamlineFeature feature) {
            return sl ? Admission(sl->Availability(feature), sl->FailureReason(feature)) :
                CapabilityAdmission{false, GraphicsCapabilityReason::MissingProvider};
        };
        D3D12_FEATURE_DATA_FORMAT_SUPPORT hdr_format{DXGI_FORMAT_R16G16B16A16_FLOAT};
        const auto hdr_store = SUCCEEDED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
            &hdr_format, sizeof(hdr_format))) && (hdr_format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)
            ? available : unsupported;
        auto dlss_sr = Both(sl_admission(StreamlineFeature::SuperResolution), hdr_store);
        for (std::size_t index = 0; index < qualities.size(); ++index)
        {
            StreamlineInputExtent range;
            const GraphicsModeId quality{std::string(qualities[index])};
            dlss_quality[index] = dlss_sr;
            if (dlss_sr.available)
            {
                const auto result = sl->QueryInputExtent(quality, output, range);
                dlss_quality[index] = QueryAdmission(result, ReconstructionAvailability::Available, result.Message());
            }
        }

        FsrSession fsr;
        const auto fsr_bound = fsr.Initialize(impl_->device.Get());
        const auto fsr_effect = [&](FsrEffect effect) {
            std::vector<FsrVariant> variants;
            const auto result = fsr_bound ? fsr.QueryVariants(effect, variants) : fsr_bound;
            return QueryAdmission(result, fsr.Availability(effect), fsr.FailureReason(effect));
        };
        auto fsr_sr = Both(fsr_effect(FsrEffect::SuperResolution), hdr_store);
        auto fsr_fg = Both(fsr_effect(FsrEffect::FrameGeneration), fsr_effect(FsrEffect::Presentation));
        for (std::size_t index = 0; index < qualities.size(); ++index)
        {
            fsr_quality[index] = fsr_sr;
            if (fsr_sr.available)
            {
                ViewExtent optimal;
                const auto result = fsr.QueryInputExtent(GraphicsModeId{std::string(qualities[index])}, output, optimal);
                fsr_quality[index] = QueryAdmission(result, ReconstructionAvailability::Available, result.Message());
            }
        }
        if (const auto released = fsr.ShutdownAfterCompletion(true); !released)
        {
            fsr_sr = fsr_fg = {false, CapabilityReason("unavailable", released.Message())};
            fsr_quality.fill(fsr_sr);
        }

        XessReconstruction xess;
        const auto xess_probe = xess.ProbeDevice(impl_->device.Get());
        auto xess_sr = Both(QueryAdmission(xess_probe, xess.Availability(), xess.FailureReason()), hdr_store);
        for (std::size_t index = 0; index < qualities.size(); ++index)
        {
            xess_quality[index] = xess_sr;
            if (xess_sr.available)
            {
                XessInputExtent extent;
                const auto result = xess.QueryInputExtent(GraphicsModeId{std::string(qualities[index])}, output, extent);
                xess_quality[index] = QueryAdmission(result, ReconstructionAvailability::Available, result.Message());
            }
        }
        if (const auto released = xess.ResetAfterCompletion(); !released)
        {
            xess_sr = {false, CapabilityReason("unavailable", released.Message())};
            xess_quality.fill(xess_sr);
        }

        CapabilityAdmission directsr;
        for (std::size_t index = 0; index < qualities.size(); ++index)
        {
            // Initialize enumerates IDSRDevice variants/source settings only.
            // Configure creates the engine/upscaler/queue and is never called.
            DirectSrReconstruction dsr;
            const auto result = dsr.Initialize(impl_->device.Get(), output, GraphicsModeId{std::string(qualities[index])});
            dsr_quality[index] = QueryAdmission(result, dsr.Availability(), dsr.FailureReason());
            if (qualities[index] == "quality") directsr = dsr_quality[index];
        }

        StreamlineReflexState reflex_state;
        auto reflex = sl_admission(StreamlineFeature::Reflex);
        if (reflex.available)
        {
            const auto result = sl->QueryReflexState(reflex_state);
            reflex = result ? reflex_state.low_latency_available ? available : unsupported :
                QueryAdmission(result, sl->Availability(StreamlineFeature::Reflex), sl->FailureReason(StreamlineFeature::Reflex));
        }
        StreamlineFrameGenerationCapabilities sl_fg_cap;
        auto dlss_fg = sl_admission(StreamlineFeature::FrameGeneration);
        if (dlss_fg.available)
        {
            const auto result = sl->QueryFrameGenerationCapabilities(sl_fg_cap);
            dlss_fg = QueryAdmission(result, sl->Availability(StreamlineFeature::FrameGeneration), sl->FailureReason(StreamlineFeature::FrameGeneration));
            if (dlss_fg.available && (!impl_->streamline_queue_proxy || sl_fg_cap.maximum_multiplier < 2))
                dlss_fg = {false, GraphicsCapabilityReason::Unavailable};
            if (dlss_fg.available && (output.width < sl_fg_cap.minimum_dimension || output.height < sl_fg_cap.minimum_dimension ||
                (cfg.vsync && !sl_fg_cap.vsync_supported))) dlss_fg = incompatible;
            dlss_fg = Both(dlss_fg, reflex);
        }
        XellLatency xell;
        const auto xell_probe = xell.Initialize(impl_->device.Get());
        // Non-Intel XeLL Availability describes the current inactive FG pairing.
        // Successful device-context admission is usable with future XeSS-FG.
        auto xell_fg = xell_probe ? available : QueryAdmission(xell_probe, xell.Availability(), xell.FailureReason());
        auto xell_standalone = Both(xell_fg, xell.RequiresFrameGeneration() ? incompatible : available);
        if (const auto released = xell.ResetAfterCompletion(true); !released)
            xell_fg = xell_standalone = {false, CapabilityReason("unavailable", released.Message())};
        XessFrameGeneration xess_fg;
        XessFrameGenerationProperties xess_fg_cap;
        const auto fg_probe = xess_fg.Probe(impl_->device.Get(), 1, xess_fg_cap);
        auto intel_fg = Both(QueryAdmission(fg_probe, xess_fg.Availability(), xess_fg.FailureReason()), xell_fg);

        const auto provider = [&](std::string_view category, std::string_view token, CapabilityAdmission admission) {
            AddOption(view, GraphicsModeId{std::string(category) + ":" + std::string(token)}, admission.available, admission.reason);
            AddProvider(view, GraphicsProviderId{std::string(token)}, admission.available, admission.reason);
        };
        for (const auto token : {"auto", "off", "native_taa", "internal_taau"}) provider("sr_provider", token, available);
        for (const auto token : {"dlss", "dlss_sr"}) provider("sr_provider", token, dlss_sr);
        for (const auto token : {"fsr", "fsr_sr"}) provider("sr_provider", token, fsr_sr);
        for (const auto token : {"xess", "xess_sr"}) provider("sr_provider", token, xess_sr);
        provider("sr_provider", "directsr", directsr);
        provider("fg_provider", "off", available);
        provider("fg_provider", "auto", available);
        provider("fg_provider", "dlss", dlss_fg);
        provider("fg_provider", "fsr", fsr_fg);
        provider("fg_provider", "xess", intel_fg);
        AddProvider(view, GraphicsProviderId{"reflex"}, reflex.available, reflex.reason);
        AddProvider(view, GraphicsProviderId{"xell"}, xell_fg.available, xell_fg.reason);

        const auto option = [&](std::string_view category, std::string_view token, CapabilityAdmission admission) {
            AddOption(view, GraphicsModeId{std::string(category) + ":" + std::string(token)}, admission.available, admission.reason);
        };
        for (std::size_t index = 0; index < qualities.size(); ++index)
        {
            CapabilityAdmission admission = unsupported;
            if (sr_selected == "dlss" || sr_selected == "dlss_sr") admission = dlss_quality[index];
            else if (sr_selected == "fsr" || sr_selected == "fsr_sr") admission = fsr_quality[index];
            else if (sr_selected == "xess" || sr_selected == "xess_sr") admission = xess_quality[index];
            else if (sr_selected == "directsr") admission = dsr_quality[index];
            else if (sr_selected == "off" || sr_selected == "native_taa") admission = qualities[index] == "native" ? available : incompatible;
            else if (sr_selected == "internal_taau") admission = qualities[index] == "native" || qualities[index] == "quality" ||
                qualities[index] == "balanced" || qualities[index] == "performance" ? available : unsupported;
            option("reconstruction", qualities[index], admission);
        }
        const auto selected_fg = [&](CapabilityAdmission dlss, CapabilityAdmission intel, CapabilityAdmission amd) {
            return fg_requested == "auto" ? Either(Either(dlss, intel), amd) : fg_requested == "dlss" ? dlss :
                fg_requested == "xess" ? intel : fg_requested == "fsr" ? amd : incompatible;
        };
        option("frame_generation", "off", available);
        option("frame_generation", "auto", selected_fg(dlss_fg, intel_fg, fsr_fg));
        option("frame_generation", "fixed", selected_fg(dlss_fg, intel_fg, incompatible));
        option("frame_generation", "dynamic", selected_fg(Both(dlss_fg,
            sl_fg_cap.dynamic_supported ? available : incompatible), incompatible, incompatible));
        // These are queried SDK limits even when generation is currently Off.
        // FSR has no maximum/requested-multiplier ABI, so no limit is invented.
        const auto maximum_multiplier = fg_requested == "dlss" ? sl_fg_cap.maximum_multiplier :
            fg_requested == "xess" ? xess_fg_cap.maximum_generated_frames + (xess_fg_cap.maximum_generated_frames ? 1u : 0u) :
            fg_requested == "auto" ? std::max(sl_fg_cap.maximum_multiplier,
                xess_fg_cap.maximum_generated_frames + (xess_fg_cap.maximum_generated_frames ? 1u : 0u)) : 0u;
        for (std::uint32_t multiplier = 2; multiplier && multiplier <= maximum_multiplier; ++multiplier)
            option("frame_generation_multiplier", std::to_string(multiplier), selected_fg(
                multiplier <= sl_fg_cap.maximum_multiplier ? dlss_fg : incompatible,
                multiplier - 1 <= xess_fg_cap.maximum_generated_frames ? intel_fg : incompatible, incompatible));
        option("latency", "off", available);
        option("latency", "auto", available);
        option("latency", "on", fg_requested == "xess" ?
            cfg.presentation_request.mode == GraphicsFrameGenerationMode::Off ? xell_standalone : xell_fg :
            fg_requested == "auto" ? Either(reflex, xell_standalone) : reflex);
        for (const auto token : {"off", "auto", "gtao"}) option("ao", token, available);
        for (const auto token : {"off", "probe"}) option("gi", token, available);
        const auto brixelizer = QueryBrixelizerNativeSupport(impl_->device.Get());
        const auto software_gi = brixelizer.supported ? available :
            brixelizer.compiled ? unsupported : CapabilityAdmission{false, GraphicsCapabilityReason::Unavailable};
        option("gi", "software_hybrid", software_gi);
        AddProvider(view, GraphicsProviderId{"brixelizer"}, software_gi.available, software_gi.reason);
        for (const auto token : {"off", "probe", "screen_space"}) option("reflection", token, available);
        option("shadow", "raster", available);
        option("adaptive_shading", "off", available);
        option("adaptive_shading", "auto", available);
        option("adaptive_shading", "on", ((impl_->feature_capabilities.hardware.vrs_tier >= 2 &&
            impl_->feature_capabilities.hardware.vrs_tile_size && impl_->feature_capabilities.adaptive_raster_consumer) ||
            (impl_->feature_capabilities.hardware.compute_shader60 && impl_->feature_capabilities.adaptive_compute_consumer)) ? available : unsupported);
        // Preserve unknown persisted selections as explicitly unavailable choices.
        for (const auto &[category, token] : {std::pair<std::string_view, std::string_view>{"sr_provider", sr_requested},
            {"fg_provider", fg_requested}})
            provider(category, token, {false, GraphicsCapabilityReason::MissingProvider});
        impl_->graphics_capabilities = view;
        impl_->graphics_capabilities_key = key;
    }
    else view = impl_->graphics_capabilities;

    // Display/HDR admission may change without a device or provider change.
    AddOption(view, GraphicsModeId{"hdr:off"}, true, GraphicsCapabilityReason::None);
    AddOption(view, GraphicsModeId{"hdr:auto"}, true, GraphicsCapabilityReason::None);
    auto hdr_config = cfg; hdr_config.hdr_mode = GraphicsHdrMode::On;
    DisplayState hdr;
    const auto hdr_query = QueryDisplayState(impl_->adapter.Get(), static_cast<HWND>(cfg.window), hdr_config, hdr);
    const auto hdr_reason = !hdr_query ? GraphicsCapabilityReason::Unavailable :
        hdr.reason == DisplayUnavailableReason::NoOutput ? GraphicsCapabilityReason::MissingDisplay :
        hdr.hdr_active ? GraphicsCapabilityReason::None : GraphicsCapabilityReason::Unsupported;
    AddOption(view, GraphicsModeId{"hdr:on"}, hdr_query && hdr.hdr_active, hdr_reason);
    std::vector<RendererDisplayOutput> displays;
    if (EnumerateRendererDisplays(displays))
    {
        view.displays.push_back({GraphicsDisplayId{"auto"}, true, GraphicsCapabilityReason::None});
        for (const auto &display : displays)
            view.displays.push_back({GraphicsDisplayId{display.id}, true,
                                     GraphicsCapabilityReason::None});
        if (cfg.display_output.value != "auto" && std::none_of(displays.begin(), displays.end(),
            [&](const auto &display) { return display.id == cfg.display_output.value; }))
            view.displays.push_back({cfg.display_output, false, GraphicsCapabilityReason::MissingDisplay});
    }
    return view;
}

} // namespace hs
