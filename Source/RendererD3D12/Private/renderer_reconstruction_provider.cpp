#include "renderer_reconstruction_provider.hpp"

#include <utility>

namespace hs
{
namespace
{
Result ProviderFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_reconstruction_provider", std::move(message));
}
}

std::string_view ReconstructionProvider::ProviderId() const noexcept
{
    switch (kind_)
    {
    case Kind::Xess: return "xess";
    case Kind::Dlss: return "dlss";
    case Kind::Fsr: return "fsr";
    case Kind::DirectSr: return "directsr";
    default: return {};
    }
}

ReconstructionAvailability ReconstructionProvider::Availability() const noexcept
{
    if (availability_ != ReconstructionAvailability::Available) return availability_;
    if (!configured_) return availability_;
    if (kind_ == Kind::Dlss) return streamline_->Availability(StreamlineFeature::SuperResolution);
    if (kind_ == Kind::Fsr) return fsr_->Availability(FsrEffect::SuperResolution);
    if (kind_ == Kind::Xess) return xess_->Availability();
    if (kind_ == Kind::DirectSr) return directsr_->Availability();
    return availability_;
}

std::string_view ReconstructionProvider::FailureReason() const noexcept
{
    if (!failure_.empty()) return failure_;
    if (kind_ == Kind::Dlss && streamline_) return streamline_->FailureReason(StreamlineFeature::SuperResolution);
    if (kind_ == Kind::Fsr && fsr_) return fsr_->FailureReason(FsrEffect::SuperResolution);
    if (kind_ == Kind::Xess && xess_) return xess_->FailureReason();
    if (kind_ == Kind::DirectSr && directsr_) return directsr_->FailureReason();
    return failure_;
}

Result ReconstructionProvider::Prepare(std::string_view provider, ID3D12Device *device,
    StreamlineSession *streamline, FsrSession *fsr, const GraphicsModeId &quality, ViewExtent output)
{
    if (kind_ != Kind::None || !device || !output.width || !output.height)
        return ProviderFailure(ErrorCode::InvalidArgument, "Invalid or already prepared reconstruction candidate.");
    device_ = device; quality_ = quality; output_ = output;
    streamline_ = streamline; fsr_ = fsr;
    Result result = Result::Success();
    if (provider == "xess" || provider == "xess_sr")
    {
        kind_ = Kind::Xess;
        xess_ = std::make_unique<XessReconstruction>();
        result = xess_->Initialize(device, output, quality, {true, true, true});
        if (result)
        {
            XessInputExtent range;
            result = xess_->QueryInputExtent(quality, output, range);
            input_ = {range.optimal, range.minimum, range.maximum};
        }
        availability_ = xess_->Availability(); failure_ = xess_->FailureReason();
        configured_ = result.Succeeded();
    }
    else if (provider == "dlss" || provider == "dlss_sr")
    {
        kind_ = Kind::Dlss;
        if (!streamline)
        {
            const StreamlineSession status;
            availability_ = status.Availability(StreamlineFeature::SuperResolution);
            if (availability_ == ReconstructionAvailability::CompiledOut)
                failure_ = status.FailureReason(StreamlineFeature::SuperResolution);
            else
            {
                availability_ = ReconstructionAvailability::Unavailable;
                failure_ = "DLSS requires Streamline startup intent before DXGI/device creation; reinitialize the renderer to enable it.";
            }
            return ProviderFailure(ErrorCode::InvalidState, failure_);
        }
        result = streamline->QueryInputExtent(quality, output, false, input_);
        availability_ = streamline->Availability(StreamlineFeature::SuperResolution);
        failure_ = streamline->FailureReason(StreamlineFeature::SuperResolution);
    }
    else if (provider == "fsr" || provider == "fsr_sr")
    {
        kind_ = Kind::Fsr;
        if (!fsr)
        {
            availability_ = ReconstructionAvailability::Unavailable;
            failure_ = "FSR has no renderer-owned effect session.";
            return ProviderFailure(ErrorCode::InvalidState, failure_);
        }
        result = fsr->QueryInputExtent(quality, output, input_.optimal);
        if (result)
        {
            FsrUpscaleRequirements requirements;
            result = fsr->QueryUpscaleRequirements(requirements);
            composition_guide_ = requirements.composition;
        }
        // FSR dynamic dispatch has no per-quality min/max query. Its zero-origin
        // render domain is bounded by maxRenderSize, never by a guessed ratio.
        input_.minimum = {1, 1}; input_.maximum = output;
        availability_ = fsr->Availability(FsrEffect::SuperResolution);
        failure_ = fsr->FailureReason(FsrEffect::SuperResolution);
    }
    else if (provider == "directsr")
    {
        kind_ = Kind::DirectSr; directsr_ = std::make_unique<DirectSrReconstruction>();
        result = directsr_->Initialize(device, output, quality);
        const auto &range = directsr_->InputRange(); input_ = {range.optimal, range.minimum, range.maximum};
        availability_ = directsr_->Availability(); failure_ = directsr_->FailureReason();
    }
    else return ProviderFailure(ErrorCode::InvalidArgument, "Unknown concrete reconstruction provider.");
    if (!result)
    {
        if (failure_.empty()) failure_ = result.Message();
        if (result.Code() == ErrorCode::InvalidArgument) availability_ = ReconstructionAvailability::Unsupported;
        else if (availability_ == ReconstructionAvailability::Available) availability_ = ReconstructionAvailability::Unavailable;
        return result;
    }
    availability_ = ReconstructionAvailability::Available; failure_.clear();
    return Result::Success();
}

Result ReconstructionProvider::ConfigureAfterCompletion(ViewExtent capacity)
{
    if (!capacity.width || !capacity.height || availability_ != ReconstructionAvailability::Available)
        return ProviderFailure(ErrorCode::InvalidState, "Cannot configure an unavailable reconstruction candidate.");
    Result result = Result::Success();
    if (kind_ == Kind::Dlss) result = streamline_->SelectReconstructionViewAfterCompletion(viewport_id_);
    else if (kind_ == Kind::Fsr) result = fsr_->SelectReconstructionViewAfterCompletion(viewport_id_);
    if (!result) return result;
    if (!configured_)
    {
        configuration_attempted_ = true;
        if (kind_ == Kind::Dlss) result = streamline_->ConfigureReconstruction(quality_, output_, false, viewport_id_);
        else if (kind_ == Kind::Fsr) result = fsr_->ConfigureUpscaling(capacity, output_);
        else if (kind_ == Kind::Xess) result = xess_->Initialize(device_.Get(), output_, quality_, {true, true, true});
        else if (kind_ == Kind::DirectSr) result = directsr_->ConfigureAfterCompletion(capacity);
        else return ProviderFailure(ErrorCode::InvalidState, "Reconstruction candidate was not prepared.");
    }
    if (!result)
    {
        failure_ = result.Message();
        availability_ = kind_ == Kind::Dlss ? streamline_->Availability(StreamlineFeature::SuperResolution) :
            kind_ == Kind::Fsr ? fsr_->Availability(FsrEffect::SuperResolution) :
            kind_ == Kind::DirectSr ? directsr_->Availability() : xess_->Availability();
        if (availability_ == ReconstructionAvailability::Available) availability_ = ReconstructionAvailability::Unavailable;
        return result;
    }
    capacity_ = capacity; configured_ = true; configuration_attempted_ = false;
    return Result::Success();
}

Result ReconstructionProvider::ResetAfterCompletion()
{
    if (!configured_ && !configuration_attempted_) return Result::Success();
    if (kind_ == Kind::Dlss)
        if (auto r = streamline_->SelectReconstructionViewAfterCompletion(viewport_id_); !r) return r;
    if (kind_ == Kind::Fsr)
        if (auto r = fsr_->SelectReconstructionViewAfterCompletion(viewport_id_); !r) return r;
    Result result = Result::Success();
    if (kind_ == Kind::Dlss) result = streamline_->ReleaseReconstructionAfterCompletion();
    else if (kind_ == Kind::Fsr) result = fsr_->ReleaseReconstructionAfterCompletion(true);
    else if (kind_ == Kind::Xess) result = xess_->ResetAfterCompletion();
    else if (kind_ == Kind::DirectSr) result = directsr_->ResetAfterCompletion();
    if (result) { configured_ = false; configuration_attempted_ = false; }
    return result;
}

Result ReconstructionProvider::Execute(ID3D12GraphicsCommandList *commands, const SdkReconstructionFrame &frame)
{
    if (!configured_) return ProviderFailure(ErrorCode::InvalidState, "Reconstruction provider is not configured.");
    if (kind_ == Kind::DirectSr) return ProviderFailure(ErrorCode::InvalidState, "DirectSR must execute as an external queue graph operation.");
    if (kind_ == Kind::Dlss) return streamline_->ExecuteSuperResolution(commands, frame);
    if (kind_ == Kind::Fsr)
    {
        auto packet = frame;
        const auto &requirements = fsr_->UpscaleRequirements();
        if (!requirements.exposure && !requirements.optional_exposure) packet.exposure = nullptr;
        if (!requirements.reactive && !requirements.optional_reactive) packet.responsive = nullptr;
        if (!requirements.composition && !requirements.optional_composition) packet.composition = nullptr;
        return fsr_->ExecuteSuperResolution(commands, packet);
    }
    XessReconstructionInputs inputs;
    inputs.pre_exposed_color = frame.color; inputs.depth = frame.depth; inputs.motion = frame.motion;
    inputs.responsive = frame.responsive; inputs.exposure_scale_texture = frame.exposure; inputs.output = frame.output;
    inputs.active_rect = {frame.active.x, frame.active.y, frame.active.width, frame.active.height};
    inputs.output_rect = {frame.output_rect.x, frame.output_rect.y, frame.output_rect.width, frame.output_rect.height};
    inputs.jitter_pixels[0] = frame.camera.jitter_pixels[0]; inputs.jitter_pixels[1] = frame.camera.jitter_pixels[1];
    inputs.reset_history = frame.camera.reset_history;
    inputs.exposure_scale = frame.exposure_scale / frame.input_pre_exposure;
    inputs.motion_scale[0] = static_cast<float>(frame.output_rect.width);
    inputs.motion_scale[1] = static_cast<float>(frame.output_rect.height);
    return xess_->Execute(commands, inputs);
}

Result ReconstructionProvider::RegisterExternal(PassBuilder &pass, const SdkReconstructionFrame &frame,
    std::uint32_t frame_slot, std::span<const ExternalResourceAccess> accesses,
    std::vector<std::shared_ptr<const void>> owners)
{
    if (!configured_ || kind_ != Kind::DirectSr)
        return ProviderFailure(ErrorCode::InvalidState, "External reconstruction requires the selected configured DirectSR provider.");
    return directsr_->RegisterExternal(pass, frame, frame_slot, accesses, std::move(owners));
}

Result ReconstructionProvider::NativeJitter(ViewExtent active, std::uint64_t frame,
    std::optional<DirectX::XMFLOAT2> &sample)
{
    sample.reset();
    return kind_ == Kind::DirectSr ? directsr_->Jitter(active, frame, sample) : Result::Success();
}
} // namespace hs
