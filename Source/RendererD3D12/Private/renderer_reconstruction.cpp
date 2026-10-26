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
    // All three integrated features are selectable during this device's lifetime.
    // slInit cannot add a feature after DXGI/device creation. Bootstrap plugins
    // once; SR evaluation contexts and FG/latency activation remain lazy.
    const StreamlineFeatures features{true, true, true};
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
    std::unique_ptr<ReconstructionProvider> &candidate, const RendererConfig *candidate_config)
{
    candidate.reset();
    const auto &cfg = candidate_config ? *candidate_config : config;
    if (cfg.grid_floor_probe && cfg.grid_floor_disable_aa &&
        request && request->provider.value != "auto" &&
          request->provider.value != "off" && request->provider.value != "native_taa" &&
          request->provider.value != "internal_taau")
        return Result::Failure(ErrorCode::InvalidArgument, "hs_reconstruction",
            "Grid AA Off cannot be combined with external temporal reconstruction.");
    if (!request || !plan.temporal_enabled) return Result::Success();
    const auto &id = request->provider.value;
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
                    ViewExtent requested_minimum, requested_maximum;
                    if (minimum_percent > maximum_percent ||
                        !ResolveDynamicResolutionExtent(output, minimum_percent, requested_minimum) ||
                        !ResolveDynamicResolutionExtent(output, maximum_percent, requested_maximum))
                        return Result::Failure(ErrorCode::InvalidArgument, "hs_reconstruction", "Invalid integer DRS bounds.");
                    minimum = {std::max(minimum.width, requested_minimum.width), std::max(minimum.height, requested_minimum.height)};
                    maximum = {std::min(maximum.width, requested_maximum.width), std::min(maximum.height, requested_maximum.height)};
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
    ReconstructionGuidesCandidate &&guides, ViewExtent capacity)
{
    if (reconstruction_provider)
        if (auto reset = reconstruction_provider->ResetAfterCompletion(); !reset)
        { initialized = false; return reset; }
    const auto restore = [&]() -> Result {
        if (candidate) if (auto reset = candidate->ResetAfterCompletion(); !reset) return reset;
        if (reconstruction_provider)
            return reconstruction_provider->ConfigureAfterCompletion(reconstruction_provider->RenderCapacity());
        return Result::Success();
    };
    Result result = Result::Success();
    if (candidate) result = candidate->ConfigureAfterCompletion(capacity);
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

Result D3D12Renderer::Impl::PrepareReconstructionGuides(const RendererConfig &candidate_config,
    ViewExtent capacity, ViewExtent output, ReconstructionGuidesCandidate &candidate, bool composition) const
{
    return reconstruction_guides.Prepare(device.Get(), allocator, capacity, output, kFrameCount,
        [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate, composition);
}

} // namespace hs
