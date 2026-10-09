#include "renderer_directsr.hpp"

#include <Windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#ifndef HS_WITH_DIRECTSR
#define HS_WITH_DIRECTSR 0
#endif
#if HS_WITH_DIRECTSR
#include <directsr.h>
#endif

namespace hs
{
namespace
{
Result Failure(ErrorCode code, std::string message)
{ return Result::Failure(code, "hs_renderer_directsr", std::move(message)); }
Result Native(std::string_view operation, HRESULT hr)
{ return Failure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " + std::to_string(std::int32_t(hr)) + ")."); }
bool ValidExtent(ViewExtent extent)
{ return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION && extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; }
}

struct DirectSrReconstruction::Impl
{
    ReconstructionAvailability availability{HS_WITH_DIRECTSR ? ReconstructionAvailability::Unavailable : ReconstructionAvailability::CompiledOut};
    std::string reason{HS_WITH_DIRECTSR ? "DirectSR has not been selected." : "DirectSR was explicitly excluded from this build."};
    DirectSrInputExtent input;
#if HS_WITH_DIRECTSR
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct QueueState
    {
        struct Slot
        {
            std::array<ComPtr<ID3D12CommandAllocator>, 2> allocators;
            std::array<ComPtr<ID3D12GraphicsCommandList>, 2> commands;
            std::uint64_t completion{};
        };
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12SDKConfiguration1> sdk;
        ComPtr<ID3D12DeviceFactory> device_factory;
        ComPtr<ID3D12DSRDeviceFactory> dsr_factory;
        ComPtr<IDSRDevice> dsr_device;
        ComPtr<IDSRSuperResEngine> engine;
        ComPtr<IDSRSuperResUpscaler> upscaler;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Fence> fence;
        std::array<Slot, 3> slots;
        DSR_SUPERRES_VARIANT_DESC variant{};
        ViewExtent output, capacity, jitter_active;
        std::vector<DSR_FLOAT2> jitter;
        std::thread::id owner;
        std::uint64_t next_fence{}, last_signal{};
        std::string failure;
        bool usable{true}, reset{true};
        bool unfenced_work{};
        std::shared_ptr<QueueState> unfenced_owner;
        std::shared_ptr<const void> unfenced_resources;
        Result Wait()
        {
            ComPtr<ID3D12Fence> drain;
            ID3D12Fence *completion = fence.Get();
            auto value = last_signal;
            if (unfenced_work)
            {
                // A failed Signal does not prove that earlier queue work retired.
                // Keep its native state/backing owners until a fresh post-work
                // fence or device removal proves that they can be released.
                if (device && FAILED(device->GetDeviceRemovedReason()))
                {
                    unfenced_work = false; unfenced_resources.reset(); unfenced_owner.reset();
                    return Result::Success();
                }
                auto hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain));
                if (FAILED(hr)) return Native("Create DirectSR retirement fence", hr);
                hr = queue->Signal(drain.Get(), 1);
                if (FAILED(hr)) return Native("Signal DirectSR retirement fence", hr);
                completion = drain.Get(); value = 1;
            }
            if (!completion || completion->GetCompletedValue() >= value)
            {
                unfenced_work = false; unfenced_resources.reset(); unfenced_owner.reset();
                return Result::Success();
            }
            const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) return Native("Create DirectSR completion event", HRESULT_FROM_WIN32(GetLastError()));
            const auto hr = completion->SetEventOnCompletion(value, event);
            const auto wait = SUCCEEDED(hr) ? WaitForSingleObject(event, INFINITE) : WAIT_FAILED;
            CloseHandle(event);
            if (FAILED(hr)) return Native("Set DirectSR completion event", hr);
            if (wait != WAIT_OBJECT_0) return Native("Wait DirectSR completion", HRESULT_FROM_WIN32(GetLastError()));
            unfenced_work = false; unfenced_resources.reset(); unfenced_owner.reset();
            return Result::Success();
        }
        ~QueueState()
        {
            // Candidate initialization also submits work. It must retire even
            // when no graph ever adopted the candidate.
            if (device && SUCCEEDED(device->GetDeviceRemovedReason())) (void)Wait();
            upscaler.Reset(); engine.Reset(); dsr_device.Reset(); dsr_factory.Reset();
            device_factory.Reset();
            if (sdk) sdk->FreeUnusedSDKs();
        }
    };
    struct FrameLease
    {
        std::shared_ptr<QueueState> state;
        std::vector<std::shared_ptr<const void>> owners;
        std::vector<ComPtr<ID3D12Resource>> resources;
        SdkReconstructionFrame frame;
        std::uint32_t slot{};
    };
    std::shared_ptr<QueueState> state;
#endif
};

DirectSrReconstruction::DirectSrReconstruction() = default;
DirectSrReconstruction::~DirectSrReconstruction()
{
#if HS_WITH_DIRECTSR
    if (impl_ && impl_->state)
    {
        auto state = impl_->state;
        if (state->device && SUCCEEDED(state->device->GetDeviceRemovedReason()))
            if (auto retired = state->Wait(); !retired) state->unfenced_owner = state;
    }
#endif
}
ReconstructionAvailability DirectSrReconstruction::Availability() const noexcept
{
#if HS_WITH_DIRECTSR
    if (impl_ && impl_->state && !impl_->state->usable) return ReconstructionAvailability::Unavailable;
#endif
    return impl_ ? impl_->availability : HS_WITH_DIRECTSR ? ReconstructionAvailability::Unavailable : ReconstructionAvailability::CompiledOut;
}
std::string_view DirectSrReconstruction::FailureReason() const noexcept
{
#if HS_WITH_DIRECTSR
    if (impl_ && impl_->state && !impl_->state->failure.empty()) return impl_->state->failure;
#endif
    return impl_ ? std::string_view(impl_->reason) : HS_WITH_DIRECTSR ? "DirectSR has not been selected." : "DirectSR was explicitly excluded from this build.";
}
const DirectSrInputExtent &DirectSrReconstruction::InputRange() const noexcept
{ static const DirectSrInputExtent empty; return impl_ ? impl_->input : empty; }
bool DirectSrReconstruction::Initialized() const noexcept
{
#if HS_WITH_DIRECTSR
    return impl_ && impl_->state && impl_->state->upscaler && impl_->state->usable;
#else
    return false;
#endif
}

Result DirectSrReconstruction::Initialize(ID3D12Device *device, ViewExtent output, const GraphicsModeId &quality)
{
    if (!device || !ValidExtent(output) || impl_) return Failure(ErrorCode::InvalidArgument, "Invalid or already prepared DirectSR candidate.");
    impl_ = std::make_unique<Impl>();
#if HS_WITH_DIRECTSR
    DSR_OPTIMIZATION_TYPE optimization;
    if (quality.value == "quality") optimization = DSR_OPTIMIZATION_TYPE_HIGH_QUALITY;
    else if (quality.value == "balanced") optimization = DSR_OPTIMIZATION_TYPE_BALANCED;
    else if (quality.value == "performance") optimization = DSR_OPTIMIZATION_TYPE_HIGH_PERFORMANCE;
    else if (quality.value == "ultra_performance") optimization = DSR_OPTIMIZATION_TYPE_MAX_PERFORMANCE;
    else if (quality.value == "ultra_quality" || quality.value == "native") optimization = DSR_OPTIMIZATION_TYPE_MAX_QUALITY;
    else return Failure(ErrorCode::InvalidArgument, "Unknown DirectSR optimization quality.");
    auto state = std::make_shared<Impl::QueueState>(); state->device = device; state->output = output; state->owner = std::this_thread::get_id();
    const auto failed = [&](Result result, ReconstructionAvailability availability = ReconstructionAvailability::Unavailable) {
        impl_->availability = availability; impl_->reason = result.Message(); return result;
    };
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length == executable.size()) return failed(Failure(ErrorCode::TaskFailed, "Cannot locate the executable's DirectSR distribution."));
    executable.resize(length); const auto runtime = std::filesystem::path(executable).parent_path() / "DirectSR";
    for (const auto *file : {L"D3D12Core.dll", L"DirectSR.dll"})
        if (!std::filesystem::is_regular_file(runtime / file)) return failed(Failure(ErrorCode::TaskFailed, "Pinned DirectSR Agility distribution is incomplete."));
    auto hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&state->sdk));
    if (FAILED(hr)) return failed(Native("D3D12GetInterface SDKConfiguration1", hr));
    // The DirectSR pair comes from one official package. A scoped factory
    // keeps the renderer's main Agility runtime and feature level unchanged.
    hr = state->sdk->CreateDeviceFactory(715, ".\\DirectSR\\", IID_PPV_ARGS(&state->device_factory));
    if (FAILED(hr)) return failed(Native("Create pinned DirectSR device factory", hr));
    hr = state->device_factory->GetConfigurationInterface(CLSID_D3D12DSRDeviceFactory, IID_PPV_ARGS(&state->dsr_factory));
    if (FAILED(hr)) return failed(Native("Get pinned DirectSR factory", hr));
    hr = state->dsr_factory->CreateDSRDevice(device, 1, IID_PPV_ARGS(&state->dsr_device));
    if (FAILED(hr)) return failed(Native("CreateDSRDevice", hr), hr == E_NOINTERFACE ? ReconstructionAvailability::Unsupported : ReconstructionAvailability::Unavailable);
    const auto count = state->dsr_device->GetNumSuperResVariants();
    if (!count || count > 4096) return failed(Failure(ErrorCode::InvalidState, "The device exposes no valid DirectSR variants."), ReconstructionAvailability::Unsupported);
    const auto flags = DSR_SUPERRES_CREATE_ENGINE_FLAGS(DSR_SUPERRES_CREATE_ENGINE_FLAG_ALLOW_DRS | DSR_SUPERRES_CREATE_ENGINE_FLAG_AUTO_EXPOSURE);
    unsigned best_rank = UINT_MAX; DSR_SUPERRES_SOURCE_SETTINGS selected{}; bool found = false;
    for (UINT index = 0; index < count; ++index)
    {
        DSR_SUPERRES_VARIANT_DESC variant{}; hr = state->dsr_device->GetSuperResVariantDesc(index, &variant);
        if (FAILED(hr)) return failed(Native("GetSuperResVariantDesc", hr));
        DSR_SUPERRES_SOURCE_SETTINGS settings{};
        hr = state->dsr_device->QuerySuperResSourceSettings(index, {output.width, output.height}, DXGI_FORMAT_R16G16B16A16_FLOAT, optimization, flags, &settings);
        if (FAILED(hr)) continue; // This variant does not accept the engine's actual HDR/output domain.
        if (!ValidExtent({settings.OptimalSize.Width, settings.OptimalSize.Height}) ||
            !ValidExtent({settings.MinDynamicSize.Width, settings.MinDynamicSize.Height}) ||
            !ValidExtent({settings.MaxDynamicSize.Width, settings.MaxDynamicSize.Height}) ||
            settings.MinDynamicSize.Width > settings.MaxDynamicSize.Width || settings.MinDynamicSize.Height > settings.MaxDynamicSize.Height ||
            settings.OptimalSize.Width < settings.MinDynamicSize.Width || settings.OptimalSize.Height < settings.MinDynamicSize.Height ||
            settings.OptimalSize.Width > settings.MaxDynamicSize.Width || settings.OptimalSize.Height > settings.MaxDynamicSize.Height)
            return failed(Failure(ErrorCode::TaskFailed, "DirectSR returned an invalid optimal/dynamic extent domain."));
        const auto position = std::find(std::begin(variant.OptimizationRankings), std::end(variant.OptimizationRankings), optimization);
        if (position == std::end(variant.OptimizationRankings)) return failed(Failure(ErrorCode::TaskFailed, "DirectSR variant omitted a required optimization ranking."));
        const unsigned rank = unsigned(position - std::begin(variant.OptimizationRankings)) + ((variant.Flags & DSR_SUPERRES_VARIANT_FLAG_NATIVE) ? 0U : DSR_NUM_OPTIMIZATION_TYPES);
        if (rank < best_rank) { state->variant = variant; selected = settings; best_rank = rank; found = true; }
    }
    if (!found) return failed(Failure(ErrorCode::InvalidState, "No DirectSR variant supports the engine's native HDR output domain."), ReconstructionAvailability::Unsupported);
    impl_->input = {{selected.OptimalSize.Width, selected.OptimalSize.Height}, {selected.MinDynamicSize.Width, selected.MinDynamicSize.Height}, {selected.MaxDynamicSize.Width, selected.MaxDynamicSize.Height}};
    impl_->state = std::move(state); impl_->availability = ReconstructionAvailability::Available; impl_->reason.clear(); return Result::Success();
#else
    return Failure(ErrorCode::InvalidState, impl_->reason);
#endif
}

Result DirectSrReconstruction::ConfigureAfterCompletion(ViewExtent capacity)
{
#if HS_WITH_DIRECTSR
    if (!impl_ || !impl_->state || impl_->availability != ReconstructionAvailability::Available || !ValidExtent(capacity))
        return Failure(ErrorCode::InvalidState, "DirectSR candidate is not available or its capacity is invalid.");
    auto state = impl_->state;
    if (state->owner != std::this_thread::get_id() || state->upscaler || capacity.width < impl_->input.minimum.width || capacity.height < impl_->input.minimum.height ||
        capacity.width > impl_->input.maximum.width || capacity.height > impl_->input.maximum.height)
        return Failure(ErrorCode::InvalidArgument, "DirectSR configuration must use its owner and queried capacity domain exactly once.");
    DSR_SUPERRES_CREATE_ENGINE_PARAMETERS parameters{}; parameters.VariantId = state->variant.VariantId;
    parameters.TargetFormat = parameters.SourceColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    parameters.SourceDepthFormat = DXGI_FORMAT_R32_FLOAT;
    // The adapted exposure is GPU-owned. DirectSR performs native HDR exposure
    // analysis instead of receiving a CPU scalar that cannot match that texture.
    parameters.ExposureScaleFormat = DXGI_FORMAT_UNKNOWN;
    parameters.Flags = DSR_SUPERRES_CREATE_ENGINE_FLAGS(DSR_SUPERRES_CREATE_ENGINE_FLAG_ALLOW_DRS | DSR_SUPERRES_CREATE_ENGINE_FLAG_AUTO_EXPOSURE);
    parameters.MaxSourceSize = {capacity.width, capacity.height}; parameters.TargetSize = {state->output.width, state->output.height};
    auto hr = state->dsr_device->CreateSuperResEngine(&parameters, IID_PPV_ARGS(&state->engine));
    if (FAILED(hr)) return Native("CreateSuperResEngine", hr);
    D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    hr = state->device->CreateCommandQueue(&queue, IID_PPV_ARGS(&state->queue)); if (FAILED(hr)) return Native("Create DirectSR compute queue", hr);
    hr = state->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence)); if (FAILED(hr)) return Native("Create DirectSR fence", hr);
    for (auto &slot : state->slots) for (UINT index = 0; index < 2; ++index)
    {
        hr = state->device->CreateCommandAllocator(queue.Type, IID_PPV_ARGS(&slot.allocators[index])); if (FAILED(hr)) return Native("Create DirectSR boundary allocator", hr);
        hr = state->device->CreateCommandList(0, queue.Type, slot.allocators[index].Get(), nullptr, IID_PPV_ARGS(&slot.commands[index])); if (FAILED(hr)) return Native("Create DirectSR boundary commands", hr);
        hr = slot.commands[index]->Close(); if (FAILED(hr)) return Native("Close DirectSR boundary commands", hr);
    }
    hr = state->engine->CreateUpscaler(state->queue.Get(), IID_PPV_ARGS(&state->upscaler));
    // CreateUpscaler may have submitted initialization even on an error.
    const auto initialization = ++state->next_fence;
    const auto signaled = state->queue->Signal(state->fence.Get(), initialization);
    if (SUCCEEDED(signaled)) state->last_signal = initialization;
    if (FAILED(signaled)) { state->usable = false; state->unfenced_work = true; state->unfenced_owner = state; return Native("Signal DirectSR initialization completion", signaled); }
    if (FAILED(hr)) { state->usable = false; return Native("CreateUpscaler", hr); }
    state->capacity = capacity; return Result::Success();
#else
    return Failure(ErrorCode::InvalidState, std::string(FailureReason()));
#endif
}

Result DirectSrReconstruction::Jitter(ViewExtent active, std::uint64_t frame, std::optional<DirectX::XMFLOAT2> &jitter)
{
    jitter = {};
#if HS_WITH_DIRECTSR
    if (!Initialized() || !ValidExtent(active)) return Failure(ErrorCode::InvalidState, "DirectSR jitter requires a configured native engine.");
    auto state = impl_->state; if (state->owner != std::this_thread::get_id()) return Failure(ErrorCode::InvalidState, "DirectSR jitter queried on the wrong owner.");
    if (active.width != state->jitter_active.width || active.height != state->jitter_active.height)
    {
        UINT count = 0; auto hr = state->engine->GetOptimalJitterPattern({active.width, active.height}, {state->output.width, state->output.height}, &count, nullptr);
        if (FAILED(hr)) return Native("Query DirectSR jitter length", hr);
        if (count > 65536) return Failure(ErrorCode::TaskFailed, "DirectSR jitter pattern exceeds the bounded native contract.");
        state->jitter.resize(count);
        if (count)
        {
            const auto capacity = count; hr = state->engine->GetOptimalJitterPattern({active.width, active.height}, {state->output.width, state->output.height}, &count, state->jitter.data());
            if (FAILED(hr)) return Native("Query DirectSR jitter pattern", hr);
            if (count > capacity) return Failure(ErrorCode::TaskFailed, "DirectSR jitter pattern changed during a fixed-domain query.");
            state->jitter.resize(count);
            for (const auto &sample : state->jitter) if (!std::isfinite(sample.X) || !std::isfinite(sample.Y)) return Failure(ErrorCode::TaskFailed, "DirectSR returned nonfinite jitter.");
        }
        state->jitter_active = active;
    }
    // A zero-length optimal pattern explicitly permits the engine's existing
    // evenly distributed jitter; it is not an unavailable provider.
    if (!state->jitter.empty())
    {
        const auto sample = state->jitter[frame % state->jitter.size()];
        // The engine stores geometry translation; DirectSR's optimal pattern
        // describes camera translation, whose raster displacement is opposite.
        jitter = DirectX::XMFLOAT2{-sample.X, -sample.Y};
    }
    return Result::Success();
#else
    return Failure(ErrorCode::InvalidState, std::string(FailureReason()));
#endif
}

Result DirectSrReconstruction::RegisterExternal(PassBuilder &pass, const SdkReconstructionFrame &frame, std::uint32_t frame_slot,
    std::span<const ExternalResourceAccess> accesses, std::vector<std::shared_ptr<const void>> owners)
{
#if HS_WITH_DIRECTSR
    if (!Initialized() || owners.empty() || accesses.empty()) return Failure(ErrorCode::InvalidState, "DirectSR external execution requires live native state and backing allocation owners.");
    auto state = impl_->state;
    if (state->owner != std::this_thread::get_id() || frame_slot >= state->slots.size() ||
        !frame.color || !frame.depth || !frame.motion || !frame.output || frame.active.x || frame.active.y || frame.output_rect.x || frame.output_rect.y ||
        frame.output_rect.width != state->output.width || frame.output_rect.height != state->output.height ||
        frame.active.width < impl_->input.minimum.width || frame.active.height < impl_->input.minimum.height ||
        frame.active.width > state->capacity.width || frame.active.height > state->capacity.height ||
        !std::isfinite(frame.input_pre_exposure) || frame.input_pre_exposure <= 0 || !std::isfinite(frame.exposure_scale) || frame.exposure_scale <= 0 ||
        !std::isfinite(frame.camera.delta_seconds) || frame.camera.delta_seconds <= 0 || !std::isfinite(frame.camera.near_plane) || frame.camera.near_plane <= 0 ||
        !std::isfinite(frame.camera.far_plane) || frame.camera.far_plane <= frame.camera.near_plane || !std::isfinite(frame.camera.vertical_fov_radians) || frame.camera.vertical_fov_radians <= 0)
        return Failure(ErrorCode::InvalidArgument, "DirectSR frame does not satisfy its native extent/camera/owner domain.");
    auto lease = std::make_shared<Impl::FrameLease>(); lease->state = state; lease->owners = std::move(owners); lease->frame = frame; lease->slot = frame_slot;
    auto resource = [&](ID3D12Resource *value, DXGI_FORMAT format, bool output) {
        if (!value) return false; const auto desc = value->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || desc.Format != format ||
            desc.Width < (output ? state->output.width : frame.active.width) || desc.Height < (output ? state->output.height : frame.active.height)) return false;
        lease->resources.emplace_back(value); return true;
    };
    if (!resource(frame.color, DXGI_FORMAT_R16G16B16A16_FLOAT, false) || !resource(frame.depth, DXGI_FORMAT_R32_FLOAT, false) ||
        !resource(frame.motion, DXGI_FORMAT_R16G16_FLOAT, false) || !resource(frame.output, DXGI_FORMAT_R16G16B16A16_FLOAT, true))
        return Failure(ErrorCode::InvalidArgument, "DirectSR requires actual RGBA16F color/output, R32F depth and RG16F UV motion.");
    if (frame.responsive && (state->variant.Flags & DSR_SUPERRES_VARIANT_FLAG_SUPPORTS_REACTIVE_MASK))
    { if (!resource(frame.responsive, DXGI_FORMAT_R8_UNORM, false)) return Failure(ErrorCode::InvalidArgument, "DirectSR reactive mask has an invalid native format."); }
    else lease->frame.responsive = nullptr;
    lease->frame.exposure = nullptr; // AUTO_EXPOSURE owns the native CPU/GPU exposure pair.
    for (const auto &boundary : accesses) if (boundary.incoming != Access::Common || boundary.outgoing != Access::Common)
        return Failure(ErrorCode::InvalidArgument, "DirectSR queue boundaries must publish whole-resource COMMON.");
    ExternalQueueBinding binding{std::shared_ptr<void>(state, state->queue.Get()), QueueHint::Compute, lease};
    pass.SetExternalExecute(std::move(binding), accesses, [lease](RenderExternalPassContext &context, ExternalQueueCompletion &completion) -> Result {
        auto state = lease->state; auto &slot = state->slots[lease->slot]; const auto &frame = lease->frame;
        if (context.NativeCommandQueue() != state->queue.Get() || !state->usable) return Failure(ErrorCode::InvalidState, "DirectSR external callback has an invalid queue or native state.");
        if (state->fence->GetCompletedValue() < slot.completion) return Failure(ErrorCode::InvalidState, "DirectSR frame boundary allocators are still in flight.");
        std::vector<D3D12_RESOURCE_BARRIER> before, after;
        for (const auto &resource : lease->resources)
        {
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter = resource.Get() == frame.output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            before.push_back(barrier); std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); after.push_back(barrier);
        }
        for (UINT index = 0; index < 2; ++index)
        {
            auto hr = slot.allocators[index]->Reset(); if (FAILED(hr)) return Native("Reset DirectSR boundary allocator", hr);
            hr = slot.commands[index]->Reset(slot.allocators[index].Get(), nullptr); if (FAILED(hr)) return Native("Reset DirectSR boundary commands", hr);
            const auto &barriers = index ? after : before; slot.commands[index]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            hr = slot.commands[index]->Close(); if (FAILED(hr)) return Native("Close DirectSR boundary commands", hr);
        }
        ID3D12CommandList *start[]{slot.commands[0].Get()}; state->queue->ExecuteCommandLists(1, start);
        DSR_SUPERRES_UPSCALER_EXECUTE_PARAMETERS parameters{};
        parameters.pTargetTexture = frame.output; parameters.TargetRegion = {0, 0, LONG(frame.output_rect.width), LONG(frame.output_rect.height)};
        parameters.pSourceColorTexture = frame.color; parameters.pSourceDepthTexture = frame.depth; parameters.pMotionVectorsTexture = frame.motion;
        parameters.SourceColorRegion = parameters.SourceDepthRegion = parameters.MotionVectorsRegion = {0, 0, LONG(frame.active.width), LONG(frame.active.height)};
        parameters.MotionVectorScale = {float(frame.active.width), float(frame.active.height)};
        parameters.CameraJitter = {-frame.camera.jitter_pixels[0], -frame.camera.jitter_pixels[1]};
        parameters.ExposureScale = frame.exposure_scale; parameters.PreExposure = frame.input_pre_exposure;
        parameters.CameraNear = frame.camera.reversed_depth ? frame.camera.far_plane : frame.camera.near_plane;
        parameters.CameraFar = frame.camera.reversed_depth ? frame.camera.near_plane : frame.camera.far_plane;
        parameters.CameraFovAngleVert = frame.camera.vertical_fov_radians; parameters.pReactiveMaskTexture = frame.responsive;
        parameters.pExposureScaleTexture = frame.exposure;
        parameters.ReactiveMaskRegion = parameters.SourceColorRegion;
        const auto result = state->upscaler->Execute(&parameters, frame.camera.delta_seconds,
            frame.camera.reset_history || state->reset ? DSR_SUPERRES_UPSCALER_EXECUTE_FLAG_RESET_HISTORY : DSR_SUPERRES_UPSCALER_EXECUTE_FLAG_NONE);
        // Restore the declared handoff and return a real post-SDK fence even
        // after a failed Execute, which can have queued work before failing.
        ID3D12CommandList *finish[]{slot.commands[1].Get()}; state->queue->ExecuteCommandLists(1, finish);
        const auto value = ++state->next_fence; const auto signaled = state->queue->Signal(state->fence.Get(), value);
        if (FAILED(signaled)) { state->usable = false; state->unfenced_work = true; state->unfenced_owner = state; state->unfenced_resources = lease; auto failure = Native("Signal DirectSR execution completion", signaled); state->failure = failure.Message(); return failure; }
        state->last_signal = slot.completion = value;
        completion = {std::shared_ptr<void>(state, state->fence.Get()), value, lease};
        if (FAILED(result)) { state->usable = false; state->reset = true; auto failure = Native("DirectSR Execute", result); state->failure = failure.Message(); return failure; }
        state->reset = false; return Result::Success();
    });
    return Result::Success();
#else
    return Failure(ErrorCode::InvalidState, std::string(FailureReason()));
#endif
}

Result DirectSrReconstruction::ResetAfterCompletion()
{
#if HS_WITH_DIRECTSR
    if (impl_ && impl_->state)
    {
        if (impl_->state->owner != std::this_thread::get_id()) return Failure(ErrorCode::InvalidState, "DirectSR reset must run on its owner.");
        if (auto completed = impl_->state->Wait(); !completed) return completed;
        auto state = impl_->state;
        state->upscaler.Reset(); state->engine.Reset(); state->queue.Reset(); state->fence.Reset();
        state->slots = {}; state->capacity = {}; state->jitter_active = {}; state->jitter.clear();
        state->next_fence = state->last_signal = 0; state->reset = true; state->failure.clear();
        // Native execution failures require a fresh candidate. Normal drained
        // release preserves the queried factory/domain for rollback.
        if (!state->usable) impl_.reset();
        return Result::Success();
    }
#endif
    impl_.reset(); return Result::Success();
}
} // namespace hs
