#include "renderer_impl.hpp"

namespace hs
{
namespace
{
bool Contains(ViewExtent value, ViewExtent minimum, ViewExtent maximum)
{
    return value.width >= minimum.width && value.height >= minimum.height &&
           value.width <= maximum.width && value.height <= maximum.height;
}
}

Result D3D12Renderer::Impl::InitializeReconstructionSessionsBeforeDevice()
{
    // All four integrated features are selectable during this device's lifetime.
    // slInit cannot add a feature after DXGI/device creation. Bootstrap plugins
    // once; SR/RR evaluation contexts and FG/latency activation remain lazy.
    const StreamlineFeatures features{true, true, true, true};
    if (!streamline_session) streamline_session = std::make_unique<StreamlineSession>();
    // Selection reads the concrete failure for fallback; missing SDKs do not
    // prevent InternalTAAU or an unrelated native provider from initializing.
    if (auto initialized = streamline_session->InitializeBeforeDevice(features); !initialized)
        OutputDebugStringA((std::string(initialized.Message()) + "\n").c_str());
    return Result::Success();
}

Result D3D12Renderer::Impl::BindReconstructionSessionsToDevice()
{
    if (streamline_session && streamline_session->Initialized())
        if (auto bound = streamline_session->BindDevice(device.Get()); !bound)
            OutputDebugStringA((std::string(bound.Message()) + "\n").c_str());
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareReconstructionProvider(
    const RendererReconstructionRequest *request, bool dynamic, float minimum_percent,
    float maximum_percent, ViewExtent output, ReconstructionPlan &plan, ViewExtent &capacity,
    std::unique_ptr<ReconstructionProvider> &candidate, const RendererConfig *ray_config)
{
    candidate.reset();
    const auto &cfg = ray_config ? *ray_config : config;
    if (cfg.grid_floor_probe && cfg.grid_floor_disable_aa &&
        ((request && request->provider.value != "auto" &&
          request->provider.value != "off" && request->provider.value != "native_taa" &&
          request->provider.value != "internal_taau") ||
         (cfg.neural_ray_reconstruction != GraphicsAutoToggle::Off &&
          cfg.ray_reconstruction_provider.value == "dlss_rr")))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_reconstruction",
            "Grid AA Off cannot be combined with external temporal reconstruction.");
    if (cfg.neural_ray_reconstruction != GraphicsAutoToggle::Off &&
        cfg.ray_reconstruction_provider.value == "fsr_rr" && !fsr_session)
    {
        fsr_session = std::make_unique<FsrSession>();
        if (auto r = fsr_session->Initialize(device.Get()); !r)
            OutputDebugStringA((std::string(r.Message()) + "\n").c_str());
    }
    if (cfg.neural_ray_reconstruction != GraphicsAutoToggle::Off && cfg.ray_reconstruction_provider.value == "fsr_rr" && fsr_session)
    {
        std::vector<FsrVariant> variants;
        if (auto r = fsr_session->QueryVariants(FsrEffect::RayRegeneration, variants); !r)
            OutputDebugStringA((std::string(r.Message()) + "\n").c_str());
    }
    const bool rr_requested = cfg.neural_ray_reconstruction != GraphicsAutoToggle::Off &&
        (cfg.ray_reconstruction_provider.value == "dlss_rr" || (request && request->provider.value == "dlss_rr"));
    if (rr_requested && RayReconstructionModeFor(cfg) == RayReconstructionMode::DlssFused)
    {
        StreamlineInputExtent range;
        const GraphicsModeId quality = request ? request->quality : GraphicsModeId{"quality"};
        if (auto r = streamline_session->QueryInputExtent(quality, output, true, range); !r) return r;
        if (dynamic || (request && request->resolution_mode == GraphicsResolutionMode::Manual &&
            ViewExtent{std::uint32_t(std::round(output.width * request->manual_scale_percent / 100.0f)),
                       std::uint32_t(std::round(output.height * request->manual_scale_percent / 100.0f))} != range.optimal))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_reconstruction", "DLSS RR requires its fixed native optimal input extent and DRS Off.");
        plan.effective_provider.value = "dlss_rr"; plan.effective_quality = quality;
        plan.requested_availability = ReconstructionAvailability::Available; plan.fallback = ReconstructionFallback::None;
        plan.fallback_reason.clear(); plan.temporal_enabled = true;
        plan.render_active = plan.input_minimum = plan.input_maximum = capacity = range.optimal;
        plan.effective_scale_percent = 100.0f * capacity.width / output.width;
        return Result::Success();
    }
    if (!request || !plan.temporal_enabled) return Result::Success();
    const auto &id = request->provider.value;
    if (id == "dlss_rr")
    {
        plan.requested_availability = streamline_session ? streamline_session->Availability(StreamlineFeature::RayReconstruction) : ReconstructionAvailability::Unavailable;
        plan.fallback = ReconstructionFallback::ProviderUnavailable;
        plan.fallback_reason = RayReconstructionModeFor(cfg) == RayReconstructionMode::Off &&
            plan.requested_availability == ReconstructionAvailability::Available
            ? "DLSS RR requires both active RT diffuse and hybrid specular producers and temporal reconstruction."
            : streamline_session ? std::string(streamline_session->FailureReason(StreamlineFeature::RayReconstruction)) : "Streamline RR was not initialized before device creation.";
        if (plan.requested_availability == ReconstructionAvailability::Available)
            plan.requested_availability = ReconstructionAvailability::Unsupported;
        return Result::Success();
    }
    if (id != "auto" && id != "xess" && id != "xess_sr" && id != "dlss" && id != "dlss_sr" &&
        id != "fsr" && id != "fsr_sr" && id != "directsr") return Result::Success();
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format{DXGI_FORMAT_R16G16B16A16_FLOAT};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format))) ||
        !(format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
    {
        plan.requested_availability = ReconstructionAvailability::Unsupported;
        plan.fallback = ReconstructionFallback::ProviderUnavailable;
        plan.fallback_reason = "Native reconstruction requires typed RGBA16F UAV stores.";
        return Result::Success();
    }
    std::array<std::string_view, 4> attempts{id, {}, {}, {}};
    if (id == "auto")
    {
        DXGI_ADAPTER_DESC3 description{};
        if (adapter) adapter->GetDesc3(&description);
        attempts = description.VendorId == 0x1002 ? std::array<std::string_view, 4>{"fsr", "xess", "dlss", "directsr"} :
            description.VendorId == 0x8086 ? std::array<std::string_view, 4>{"xess", "fsr", "dlss", "directsr"} :
            std::array<std::string_view, 4>{"dlss", "xess", "fsr", "directsr"};
    }
    std::string failures;
    auto automatic_availability = ReconstructionAvailability::CompiledOut;
    for (const auto selected : attempts)
    {
        if (selected.empty()) continue;
        if ((selected == "fsr" || selected == "fsr_sr") && !fsr_session)
        {
            fsr_session = std::make_unique<FsrSession>();
            if (auto initialized = fsr_session->Initialize(device.Get()); !initialized)
                OutputDebugStringA((std::string(initialized.Message()) + "\n").c_str());
        }
        auto prepared = std::make_unique<ReconstructionProvider>();
        auto result = prepared->Prepare(selected, device.Get(), streamline_session.get(), fsr_session.get(), request->quality, output);
        const auto removed = device->GetDeviceRemovedReason();
        if (FAILED(removed)) return CheckDevice(removed, "Prepare reconstruction provider");
        auto availability = prepared->Availability();
        std::string reason(prepared->FailureReason());
        if (result)
        {
            const auto &supported = prepared->InputRange();
            auto active = supported.optimal;
            if (request->resolution_mode == GraphicsResolutionMode::Manual)
            {
                const auto scaled = [&](std::uint32_t pixels) {
                    return static_cast<std::uint32_t>(std::max(1.0, std::round(double(pixels) * request->manual_scale_percent / 100.0)));
                };
                active = {scaled(output.width), scaled(output.height)};
            }
            auto minimum = supported.minimum;
            auto maximum = dynamic ? supported.maximum : active;
            if (!Contains(active, supported.minimum, supported.maximum))
            {
                availability = ReconstructionAvailability::Unsupported;
                reason = "Requested manual extent is outside the selected native quality's render domain.";
            }
            else
            {
                if (dynamic)
                {
                    const auto scaled = [](std::uint32_t pixels, float percent) {
                        return static_cast<std::uint32_t>(std::max(1.0, std::ceil(double(pixels) * percent / 100.0)));
                    };
                    minimum = {std::max(minimum.width, scaled(output.width, minimum_percent)), std::max(minimum.height, scaled(output.height, minimum_percent))};
                    maximum = {std::min(maximum.width, scaled(output.width, maximum_percent)), std::min(maximum.height, scaled(output.height, maximum_percent))};
                }
                if (minimum.width > maximum.width || minimum.height > maximum.height)
                {
                    availability = ReconstructionAvailability::Unsupported;
                    reason = "DRS range has no intersection with the selected native quality's render domain.";
                }
                else
                {
                    active.width = std::clamp(active.width, minimum.width, maximum.width);
                    active.height = std::clamp(active.height, minimum.height, maximum.height);
                    plan.effective_provider.value = prepared->ProviderId(); plan.effective_quality = request->quality;
                    plan.requested_availability = ReconstructionAvailability::Available;
                    plan.fallback = ReconstructionFallback::None; plan.fallback_reason.clear(); plan.temporal_enabled = true;
                    plan.render_active = active; plan.input_minimum = minimum; plan.input_maximum = maximum;
                    plan.effective_scale_percent = 100.0f * active.width / output.width;
                    capacity = maximum; candidate = std::move(prepared);
                    return Result::Success();
                }
            }
        }
        if (reason.empty()) reason = result.Message();
        if (!failures.empty()) failures += " ";
        failures += std::string(selected) + ": " + reason;
        if (availability == ReconstructionAvailability::Unavailable)
            automatic_availability = ReconstructionAvailability::Unavailable;
        else if (availability == ReconstructionAvailability::Unsupported &&
                 automatic_availability == ReconstructionAvailability::CompiledOut)
            automatic_availability = ReconstructionAvailability::Unsupported;
        plan.requested_availability = availability;
    }
    if (id == "auto") plan.requested_availability = automatic_availability;
    plan.fallback_reason = std::move(failures);
    plan.fallback = ReconstructionFallback::ProviderUnavailable;
    return Result::Success();
}

Result D3D12Renderer::Impl::CommitReconstructionProvider(std::unique_ptr<ReconstructionProvider> &candidate,
    ReconstructionGuidesCandidate &&guides, ViewExtent capacity, const ReconstructionPlan *next_plan, RayReconstructionCandidate *rr_candidate)
{
    const bool was_rr = ray_reconstruction.Mode() == RayReconstructionMode::DlssFused;
    const bool next_rr = next_plan && next_plan->effective_provider.value == "dlss_rr";
    if (reconstruction_provider)
        if (auto reset = reconstruction_provider->ResetAfterCompletion(); !reset)
        { initialized = false; return reset; }
    const auto restore = [&]() -> Result {
        if (candidate) if (auto reset = candidate->ResetAfterCompletion(); !reset) return reset;
        if (streamline_session && (was_rr || next_rr))
            if (auto r = streamline_session->ReleaseReconstructionAfterCompletion(); !r) return r;
        if (was_rr) return streamline_session->ConfigureReconstruction(reconstruction_plan.effective_quality,
            reconstruction_plan.output_extent, true);
        if (reconstruction_provider)
            return reconstruction_provider->ConfigureAfterCompletion(reconstruction_provider->RenderCapacity());
        return Result::Success();
    };
    Result result = Result::Success();
    if (was_rr) result = streamline_session->ReleaseReconstructionAfterCompletion();
    if (result && next_rr) result = streamline_session->ConfigureReconstruction(next_plan->effective_quality,
        next_plan->output_extent, true);
    else if (result && candidate) result = candidate->ConfigureAfterCompletion(capacity);
    if (result && next_rr && rr_candidate) result = ValidateRayReconstructionBudget(*rr_candidate);
    if (result) result = candidate
        ? reconstruction_guides.Commit(std::move(guides), graph_queue_executor.Completed())
        : reconstruction_guides.ResetAfterCompletion(graph_queue_executor.Completed());
    if (!result)
    {
        if (auto restored = restore(); !restored)
        {
            initialized = false;
            return Result::Failure(ErrorCode::InvalidState, "hs_reconstruction", std::string(result.Message()) +
                "; restoring the previous native reconstruction configuration failed: " + std::string(restored.Message()));
        }
        return result;
    }
    reconstruction_provider = std::move(candidate);
    return Result::Success();
}

Result D3D12Renderer::Impl::ValidateRayReconstructionBudget(RayReconstructionCandidate &candidate)
{
    return ray_reconstruction.ValidateNativeBudgetAfterConfiguration(candidate,
        [this](ID3D12GraphicsCommandList *commands, ID3D12CommandAllocator *command_allocator) -> Result {
            std::vector<AllocationResource> uploads;
            return SubmitResidentUtilityUpload(commands, command_allocator, uploads);
        });
}

Result D3D12Renderer::Impl::ValidateRayReconstructionBudget(RayReconstructionSystem &system)
{
    return system.ValidateNativeBudgetAfterConfiguration(
        [this](ID3D12GraphicsCommandList *commands, ID3D12CommandAllocator *command_allocator) -> Result {
            std::vector<AllocationResource> uploads;
            return SubmitResidentUtilityUpload(commands, command_allocator, uploads);
        });
}

Result D3D12Renderer::Impl::PrepareReconstructionGuides(const RendererConfig &candidate_config,
    ViewExtent capacity, ViewExtent output, ReconstructionGuidesCandidate &candidate, bool composition) const
{
    return reconstruction_guides.Prepare(device.Get(), allocator, capacity, output, kFrameCount,
        [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate, composition);
}

RayReconstructionMode D3D12Renderer::Impl::RayReconstructionModeFor(const RendererConfig &cfg) const noexcept
{
    if (cfg.neural_ray_reconstruction == GraphicsAutoToggle::Off) return RayReconstructionMode::Off;
    const auto features = ResolveFeatures(cfg);
    const bool diffuse = features.gi_mode == GraphicsGiMode::RayTraced;
    const bool specular = features.reflection_mode == GraphicsReflectionMode::HybridRT;
    const bool dlss = cfg.ray_reconstruction_provider.value == "dlss_rr" ||
        (cfg.reconstruction_request && cfg.reconstruction_request->provider.value == "dlss_rr");
    if (dlss && diffuse && specular && cfg.temporal_reconstruction && streamline_session &&
        streamline_session->Availability(StreamlineFeature::RayReconstruction) == ReconstructionAvailability::Available)
        return RayReconstructionMode::DlssFused;
    if (cfg.ray_reconstruction_provider.value == "fsr_rr" && (diffuse || specular) && fsr_session &&
        fsr_session->Availability(FsrEffect::RayRegeneration) == ReconstructionAvailability::Available)
        return RayReconstructionMode::FsrStandalone;
    return RayReconstructionMode::Off;
}

Result D3D12Renderer::Impl::PrepareRayReconstruction(const RendererConfig &cfg, ViewExtent capacity,
    ViewExtent output, RayReconstructionCandidate &candidate, StreamlineSession *shared_sl, FsrSession *shared_fsr) const
{
    RayReconstructionProfile profile;
    // Resident candidates use the live sessions explicitly; they never create
    // a second Streamline global or mutate the live FSR signal contexts.
    profile.mode = RayReconstructionModeFor(cfg);
    profile.render_capacity = capacity; profile.output_extent = output; profile.frame_slots = kFrameCount;
    const auto features = ResolveFeatures(cfg);
    profile.diffuse = features.gi_mode == GraphicsGiMode::RayTraced;
    profile.specular = features.reflection_mode == GraphicsReflectionMode::HybridRT;
    if (shared_sl && profile.diffuse && profile.specular && cfg.temporal_reconstruction &&
        cfg.neural_ray_reconstruction != GraphicsAutoToggle::Off &&
        (cfg.ray_reconstruction_provider.value == "dlss_rr" ||
         (cfg.reconstruction_request && cfg.reconstruction_request->provider.value == "dlss_rr")) &&
        shared_sl->Availability(StreamlineFeature::RayReconstruction) == ReconstructionAvailability::Available)
        profile.mode = RayReconstructionMode::DlssFused;
    if (shared_fsr && (profile.diffuse || profile.specular) && cfg.neural_ray_reconstruction != GraphicsAutoToggle::Off &&
        cfg.ray_reconstruction_provider.value == "fsr_rr" &&
        shared_fsr->Availability(FsrEffect::RayRegeneration) == ReconstructionAvailability::Available)
        profile.mode = RayReconstructionMode::FsrStandalone;
    profile.diffuse_quality = cfg.gi_quality; profile.specular_quality = cfg.reflection_quality;
    profile.diffuse_axis_ratio = QualityProfileFor(cfg.gi_quality).rt_axis_ratio;
    profile.specular_axis_ratio = QualityProfileFor(cfg.reflection_quality).rt_axis_ratio;
    if (cfg.reconstruction_request) profile.reconstruction_quality = cfg.reconstruction_request->quality;
    if (profile.mode != RayReconstructionMode::Off)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)) ||
            memory.Budget <= memory.CurrentUsage)
            return Result::Failure(ErrorCode::InvalidState, "hs_reconstruction", "Native RR candidate requires a positive observed DXGI local memory budget.");
        profile.byte_budget = memory.Budget - memory.CurrentUsage;
    }
    return ray_reconstruction.Prepare(device.Get(), allocator, root_signature.Get(), profile,
        shared_sl ? shared_sl : streamline_session.get(), shared_fsr ? shared_fsr : fsr_session.get(),
        [&](std::string_view id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto r = cfg.ResolveProduct(id, path); !r) return r;
            return ReadBinary(path, bytes);
        }, candidate, &pipeline_cache);
}
} // namespace hs
