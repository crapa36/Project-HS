#include "renderer_ambient_occlusion.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kSrvCount = 9, kUavCount = 5, kDescriptorCount = 14;
constexpr std::uint32_t kConstantStride = 512;
constexpr std::array<std::string_view, 5> kShaderProducts{
    "render/shader/gtao_horizon_cs.dxil", "render/shader/gtao_temporal_cs.dxil",
    "render/shader/gtao_bilateral_cs.dxil", "render/shader/contact_shadow_cs.dxil",
    "render/shader/adaptive_gtao_cs.dxil"};

Result AoFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_ambient_occlusion", std::move(message));
}
Result NativeFailure(std::string_view operation, HRESULT error)
{
    return AoFailure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width != 0 && extent.height != 0 &&
        extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool ValidQuality(GraphicsQuality quality)
{
    return quality == GraphicsQuality::Low || quality == GraphicsQuality::Medium ||
        quality == GraphicsQuality::High || quality == GraphicsQuality::Ultra;
}
ViewExtent AoExtent(ViewExtent render, const render_data::QualityProfile &profile)
{
    return {std::max(1u, static_cast<UINT>(std::ceil(render.width * profile.ao_axis_ratio))),
            std::max(1u, static_cast<UINT>(std::ceil(render.height * profile.ao_axis_ratio)))};
}
DirectX::XMFLOAT4 SizeConstants(ViewExtent extent)
{
    return {static_cast<float>(extent.width), static_cast<float>(extent.height),
            1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};
}
bool SupportsFastAoUnprojection(const DirectX::XMFLOAT4X4 &inverse) noexcept
{
    for (const auto &row : inverse.m)
        for (const float value : row)
            if (!std::isfinite(value)) return false;
    if (inverse._12 != 0.0f || inverse._13 != 0.0f || inverse._14 != 0.0f ||
        inverse._21 != 0.0f || inverse._23 != 0.0f || inverse._24 != 0.0f ||
        inverse._31 != 0.0f || inverse._32 != 0.0f)
        return false;
    const double depth_determinant = static_cast<double>(inverse._33) * inverse._44 -
        static_cast<double>(inverse._34) * inverse._43;
    return inverse._11 != 0.0f && inverse._22 != 0.0f && depth_determinant != 0.0 &&
        std::isfinite(depth_determinant);
}
bool FitsGuide(const AmbientOcclusionGuide &guide, ViewExtent active, DXGI_FORMAT format)
{
    if (!guide.resource || !guide.version.resource || guide.version.resource.kind != ResourceKind::Texture)
        return false;
    const auto d = guide.resource->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
        d.SampleDesc.Count == 1 && d.Width >= active.width && d.Height >= active.height && d.Format == format;
}
bool FitsDepth(const AmbientOcclusionGuide &guide, ViewExtent active)
{
    return FitsGuide(guide, active, DXGI_FORMAT_R32_TYPELESS) ||
        FitsGuide(guide, active, DXGI_FORMAT_D32_FLOAT) || FitsGuide(guide, active, DXGI_FORMAT_R32_FLOAT);
}
Result CheckUavFormat(ID3D12Device *device, DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    const auto result = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(result)) return NativeFailure("Check AO format support", result);
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return AoFailure(ErrorCode::TaskFailed, "AO resource format lacks texture, shader-load or typed UAV-store support.");
    return Result::Success();
}
Result CreateTexture(D3D12MA::Allocator *allocator, ViewExtent extent, DXGI_FORMAT format,
                     AllocationResource &texture)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = extent.width; description.Height = extent.height;
    description.DepthOrArraySize = 1; description.MipLevels = 1;
    description.Format = format; description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto result = allocator->CreateResource(&allocation, &description, D3D12_RESOURCE_STATE_COMMON,
        nullptr, &texture.allocation, IID_PPV_ARGS(texture.resource.ReleaseAndGetAddressOf()));
    if (FAILED(result)) return NativeFailure("Create AO texture", result);
    texture.graph_access = Access::Common;
    return Result::Success();
}
} // namespace

struct AmbientOcclusionTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 5> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12DescriptorHeap> export_descriptors;
    AllocationResource constants, raw, resolved, contact;
    std::array<AllocationResource, 2> history, metadata_history;
    std::byte *mapped{};
    ViewExtent render_capacity, ao_capacity, previous_active;
    GraphicsQuality quality{};
    render_data::QualityProfile profile;
    bool ao_enabled{}, contact_enabled{}, history_valid{}, usable{true};
    bool adaptive_compute{},adaptive_applied{};
    std::uint32_t descriptor_count{kDescriptorCount};
    std::uint32_t frame_slot_count{}, descriptor_stride{}, read_index{};
    std::vector<CompletionSet> slot_completion;
    CompletionSet completion;
    std::uint64_t bytes{};

    ~AmbientOcclusionTargets()
    {
        if (mapped && constants.resource) constants.resource->Unmap(0, nullptr);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot, std::uint32_t offset) const
    {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot*descriptor_count + offset) * descriptor_stride;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t slot, std::uint32_t offset) const
    {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(slot*descriptor_count + offset) * descriptor_stride;
        return handle;
    }
    void Srv(std::uint32_t slot, std::uint32_t offset, ID3D12Resource *resource, DXGI_FORMAT format)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &view, Cpu(slot, offset));
    }
    void Uav(std::uint32_t slot, std::uint32_t offset, ID3D12Resource *resource, DXGI_FORMAT format)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = format; view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource, nullptr, &view, Cpu(slot, kSrvCount + offset));
    }
};

struct AmbientOcclusionSystem::Impl
{
    std::shared_ptr<AmbientOcclusionTargets> targets;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<AmbientOcclusionTargets> targets;
        std::vector<Import> imports;
        ViewExtent active;
        std::uint32_t slot{}, write_index{};
        bool adaptive{};
    } pending;
};

AmbientOcclusionSystem::AmbientOcclusionSystem() : impl_(std::make_unique<Impl>()) {}
AmbientOcclusionSystem::~AmbientOcclusionSystem() = default;
std::uint64_t AmbientOcclusionCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t AmbientOcclusionSystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }
bool AmbientOcclusionSystem::AdaptiveComputeApplied()const noexcept{return impl_->targets&&impl_->targets->adaptive_applied;}
std::array<const AllocationResource *, 5> AmbientOcclusionSystem::DiagnosticResources() const noexcept
{
    if (!impl_->targets) return {};
    const auto &storage = *impl_->targets;
    return {&storage.raw, &storage.resolved, &storage.contact,
            &storage.history[storage.read_index], &storage.history[1u - storage.read_index]};
}

Result AmbientOcclusionSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ViewExtent capacity, GraphicsQuality quality, const render_data::QualityProfile &profile, bool aoEnabled, bool contactEnabled,
    std::uint32_t frameSlots, const ShaderReader &readShader, AmbientOcclusionCandidate &candidate,bool adaptiveCompute) const
{
    if (!device || !allocator || !ValidExtent(capacity) || !ValidQuality(quality) ||
        frameSlots == 0 || frameSlots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / kDescriptorCount ||
        ((aoEnabled || contactEnabled) && !readShader))
        return AoFailure(ErrorCode::InvalidArgument, "Invalid AO device, capacity, quality, frame slots or shader reader.");
    auto next = std::make_shared<AmbientOcclusionTargets>();
    next->device = device; next->allocator = allocator;
    next->render_capacity = capacity; next->ao_capacity = AoExtent(capacity, profile);
    next->quality = quality; next->profile = profile; next->ao_enabled = aoEnabled; next->contact_enabled = contactEnabled;
    next->adaptive_compute=aoEnabled&&adaptiveCompute;
    if(next->adaptive_compute)next->descriptor_count=kDescriptorCount+1;
    if(frameSlots>D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1/next->descriptor_count)
        return AoFailure(ErrorCode::InvalidArgument,"Adaptive AO descriptors exceed the native heap limit.");
    next->frame_slot_count = frameSlots; next->slot_completion.resize(frameSlots);
    if (!aoEnabled && !contactEnabled)
    {
        candidate.targets_ = std::move(next);
        return Result::Success();
    }
    if (auto supported = CheckUavFormat(device, DXGI_FORMAT_R16_FLOAT); !supported) return supported;
    if (aoEnabled)
    {
        if (auto supported = CheckUavFormat(device, DXGI_FORMAT_R32G32B32A32_FLOAT); !supported) return supported;
        if (auto supported = CheckUavFormat(device, DXGI_FORMAT_R32G32_UINT); !supported) return supported;
        if (auto created = CreateTexture(allocator, next->ao_capacity, DXGI_FORMAT_R32G32B32A32_FLOAT, next->raw); !created) return created;
        if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R16_FLOAT, next->resolved); !created) return created;
        for (std::uint32_t index = 0; index < 2; ++index)
        {
            if (auto created = CreateTexture(allocator, next->ao_capacity, DXGI_FORMAT_R32G32B32A32_FLOAT, next->history[index]); !created) return created;
            if (auto created = CreateTexture(allocator, next->ao_capacity, DXGI_FORMAT_R32G32_UINT, next->metadata_history[index]); !created) return created;
        }
    }
    if (contactEnabled)
        if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R16_FLOAT, next->contact); !created) return created;

    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 12, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0, 12, 0};
    ranges[2] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 33, 0};
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor = {0, 12};
    parameters[1].ParameterType = parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[0]};
    parameters[2].DescriptorTable = {1, &ranges[1]};
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].DescriptorTable={1,&ranges[2]};
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = next->adaptive_compute?4:3; rootDescription.pParameters = parameters;
    ComPtr<ID3DBlob> signature, diagnostics;
    auto native = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &signature, &diagnostics);
    if (FAILED(native))
    {
        auto failure = NativeFailure("Serialize AO root signature", native);
        if (diagnostics && diagnostics->GetBufferSize())
            return AoFailure(ErrorCode::TaskFailed, std::string(failure.Message()) + " " +
                std::string(static_cast<const char *>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()));
        return failure;
    }
    native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                        IID_PPV_ARGS(&next->root));
    if (FAILED(native)) return NativeFailure("Create AO root signature", native);
    for (std::uint32_t index = 0; index < kShaderProducts.size(); ++index)
    {
        if ((index < 3 && !aoEnabled) || (index == 3 && !contactEnabled)||(index>=4&&!next->adaptive_compute)||
            (index==0&&next->adaptive_compute)) continue;
        std::vector<std::byte> bytecode;
        if (auto read = readShader(kShaderProducts[index], bytecode); !read) return read;
        if (bytecode.empty()) return AoFailure(ErrorCode::TaskFailed, "Selected AO shader product is empty: " + std::string(kShaderProducts[index]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = next->root.Get(); pipeline.CS = {bytecode.data(), bytecode.size()};
        native = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&next->pipelines[index]));
        if (FAILED(native)) return NativeFailure("Create AO compute pipeline " + std::string(kShaderProducts[index]), native);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = frameSlots * next->descriptor_count; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->descriptors));
    if (FAILED(native)) return NativeFailure("Create AO descriptors", native);
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors = 2; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->export_descriptors));
    if (FAILED(native)) return NativeFailure("Create AO export descriptors", native);
    D3D12_SHADER_RESOURCE_VIEW_DESC exportView{};
    exportView.Format = DXGI_FORMAT_R16_FLOAT;
    exportView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    exportView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    exportView.Texture2D.MipLevels = 1;
    auto exportHandle = next->export_descriptors->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(next->resolved.resource.Get(), &exportView, exportHandle);
    exportHandle.ptr += next->descriptor_stride;
    device->CreateShaderResourceView(next->contact.resource.Get(), &exportView, exportHandle);
    D3D12MA::ALLOCATION_DESC uploadAllocation{}; uploadAllocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload{};
    upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; upload.Width = UINT64(frameSlots) * kConstantStride;
    upload.Height = 1; upload.DepthOrArraySize = 1; upload.MipLevels = 1;
    upload.SampleDesc.Count = 1; upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    native = allocator->CreateResource(&uploadAllocation, &upload, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, &next->constants.allocation, IID_PPV_ARGS(next->constants.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create AO constant upload", native);
    D3D12_RANGE noRead{0, 0};
    void *mapped{};
    native = next->constants.resource->Map(0, &noRead, &mapped);
    if (FAILED(native)) return NativeFailure("Map AO constants", native);
    next->mapped = static_cast<std::byte *>(mapped);
    auto includeBytes = [&](const AllocationResource &allocation) {
        if (allocation.allocation) next->bytes += allocation.allocation->GetSize();
    };
    includeBytes(next->constants); includeBytes(next->raw); includeBytes(next->resolved); includeBytes(next->contact);
    for (const auto &allocation : next->history) includeBytes(allocation);
    for (const auto &allocation : next->metadata_history) includeBytes(allocation);
    candidate.targets_ = std::move(next);
    return Result::Success();
}

Result AmbientOcclusionSystem::Commit(AmbientOcclusionCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return AoFailure(ErrorCode::InvalidState, "Complete the registered AO submission before replacing targets.");
    if (!candidate.targets_) return AoFailure(ErrorCode::InvalidArgument, "AO candidate was not prepared.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return AoFailure(ErrorCode::InvalidState, "Old AO targets are still in GPU use.");
    impl_->targets = std::move(candidate.targets_);
    return Result::Success();
}

Result AmbientOcclusionSystem::Register(RenderGraphBuilder &graph, const AmbientOcclusionInputs &inputs,
    AmbientOcclusionConstants constants, AmbientOcclusionOutputs &outputs, AmbientOcclusionLayout *layout)
{
    return RegisterFrame(graph, inputs, constants, outputs, layout, nullptr);
}
bool AmbientOcclusionSystem::MatchesShape(const AmbientOcclusionLayout &layout,
    bool reset_history, QueueHint queue) const noexcept
{
    const auto &s=impl_->targets;
    if(!layout.valid||!s||layout.ao!=s->ao_enabled||layout.contact!=s->contact_enabled||
        layout.adaptive!=s->adaptive_compute)return false;
    return (!s->ao_enabled&&!s->contact_enabled)||
        (layout.history==(s->history_valid&&!reset_history)&&layout.queue==queue&&
         layout.render_capacity.width==s->render_capacity.width&&layout.render_capacity.height==s->render_capacity.height&&
         layout.ao_capacity.width==s->ao_capacity.width&&layout.ao_capacity.height==s->ao_capacity.height);
}
bool AmbientOcclusionSystem::MatchesLayout(const AmbientOcclusionInputs &inputs,const AmbientOcclusionLayout &layout)const noexcept
{
    return MatchesShape(layout, inputs.reset_history, inputs.queue) &&
        ((!impl_->targets->ao_enabled && !impl_->targets->contact_enabled) ||
         layout.guides==std::array{inputs.depth.version,inputs.normal.version,inputs.metadata.version,
             inputs.motion.version,inputs.surface_history.version,inputs.adaptive_density.version});
}
Result AmbientOcclusionSystem::BindRetained(RenderGraphBuilder &graph, const AmbientOcclusionInputs &inputs,
    AmbientOcclusionConstants constants, const AmbientOcclusionLayout &layout, AmbientOcclusionOutputs &outputs)
{
    return RegisterFrame(graph, inputs, constants, outputs, nullptr, &layout);
}
Result AmbientOcclusionSystem::RegisterFrame(RenderGraphBuilder &graph, const AmbientOcclusionInputs &inputs,
    AmbientOcclusionConstants constants, AmbientOcclusionOutputs &outputs,
    AmbientOcclusionLayout *capture, const AmbientOcclusionLayout *retained)
{
    outputs = {};
    if (capture) *capture = {};
    auto storage = impl_->targets;
    if (!storage) return AoFailure(ErrorCode::InvalidState, "Prepare and commit AO targets before registration.");
    const bool historyValid = storage->history_valid && !inputs.reset_history;
    if (retained && !MatchesLayout(inputs,*retained))
        return AoFailure(ErrorCode::InvalidState, "AO retained topology has changed.");
    if (capture)
    {
        capture->ao = storage->ao_enabled; capture->contact = storage->contact_enabled;
        capture->adaptive = storage->adaptive_compute; capture->history = historyValid;
        capture->queue = inputs.queue;
        capture->render_capacity = storage->render_capacity; capture->ao_capacity = storage->ao_capacity;
    }
    if (!storage->ao_enabled && !storage->contact_enabled)
    {
        if (capture) capture->valid = true;
        return Result::Success();
    }
    if (impl_->pending.graph || !storage->usable)
        return AoFailure(ErrorCode::InvalidState, "AO submission is pending or its previous graph execution failed.");
    const auto active = inputs.render_active;
    if (!ValidExtent(active) || active.width > storage->render_capacity.width || active.height > storage->render_capacity.height ||
        inputs.frame_slot >= storage->frame_slot_count ||
        !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed))
        return AoFailure(ErrorCode::InvalidArgument, "AO active extent or completed frame slot is invalid.");
    if (!FitsDepth(inputs.depth, active) || !FitsGuide(inputs.normal, active, DXGI_FORMAT_R16G16_SNORM) ||
        !FitsGuide(inputs.metadata, active, DXGI_FORMAT_R32G32_UINT) ||
        (storage->ao_enabled && (!FitsGuide(inputs.motion, active, DXGI_FORMAT_R16G16_FLOAT) ||
                                !FitsGuide(inputs.surface_history, active, DXGI_FORMAT_R32G32_FLOAT))))
        return AoFailure(ErrorCode::InvalidArgument, "AO guide resource, version, format or active extent is invalid.");
    const std::array guides{inputs.depth.version, inputs.normal.version, inputs.metadata.version,
        inputs.motion.version, inputs.surface_history.version, inputs.adaptive_density.version};
    if (capture) capture->guides = guides;
    const ViewExtent density_active{(active.width+7)/8,(active.height+7)/8};
    if(storage->adaptive_compute&&(!FitsGuide(inputs.adaptive_density,density_active,DXGI_FORMAT_R8_UINT)||
        graph.Resolve(inputs.adaptive_density.version.resource)!=inputs.adaptive_density.resource))
        return AoFailure(ErrorCode::InvalidArgument,"Selected adaptive GTAO requires its actual current R8 tile8 density graph image.");
    if (!std::isfinite(constants.gtao_parameters.x) || constants.gtao_parameters.x <= 0.0f ||
        !std::isfinite(constants.gtao_parameters.y) || constants.gtao_parameters.y < 0.0f || constants.gtao_parameters.y > 1.0f ||
        !std::isfinite(constants.contact_parameters.x) || constants.contact_parameters.x <= 0.0f ||
        !std::isfinite(constants.contact_parameters.y) || constants.contact_parameters.y <= 0.0f ||
        !std::isfinite(constants.contact_parameters.z) || constants.contact_parameters.z < 0.0f)
        return AoFailure(ErrorCode::InvalidArgument, "AO physical radius, strength or contact parameters are invalid.");
    // The shader's fast path is exact for axis-aligned homogeneous inverse
    // projections (including off-axis and jittered perspective and orthographic
    // projections). Keep arbitrary matrices on the original general multiply.
    constants.gtao_parameters.w = SupportsFastAoUnprojection(constants.inverse_projection) ? 1.0f : 0.0f;
    const auto aoActive = AoExtent(active, storage->profile);
    const auto writeIndex = 1u - storage->read_index;
    constants.render_size = SizeConstants(active); constants.ao_size = SizeConstants(aoActive);
    constants.history_size = SizeConstants(historyValid ? storage->previous_active : aoActive);
    constants.sample_counts = DirectX::XMUINT4{storage->profile.ao_directions, storage->profile.ao_steps,
        storage->profile.contact_shadow_steps, storage->profile.ao_spatial_radius};
    constants.frame_info = {historyValid ? 1u : 0u, static_cast<std::uint32_t>(inputs.frame_index),
                            storage->ao_enabled ? 1u : 0u, storage->contact_enabled ? 1u : 0u};
    std::memcpy(storage->mapped + SIZE_T(inputs.frame_slot) * kConstantStride, &constants, sizeof(constants));
    storage->Srv(inputs.frame_slot, 0, inputs.depth.resource, DXGI_FORMAT_R32_FLOAT);
    storage->Srv(inputs.frame_slot, 1, inputs.normal.resource, DXGI_FORMAT_R16G16_SNORM);
    storage->Srv(inputs.frame_slot, 2, inputs.metadata.resource, DXGI_FORMAT_R32G32_UINT);
    storage->Srv(inputs.frame_slot, 3, storage->ao_enabled ? inputs.motion.resource : nullptr, DXGI_FORMAT_R16G16_FLOAT);
    storage->Srv(inputs.frame_slot, 4, storage->ao_enabled ? inputs.surface_history.resource : nullptr, DXGI_FORMAT_R32G32_FLOAT);
    storage->Srv(inputs.frame_slot, 5, storage->raw.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Srv(inputs.frame_slot, 6, historyValid ? storage->history[storage->read_index].resource.Get() : nullptr, DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Srv(inputs.frame_slot, 7, historyValid ? storage->metadata_history[storage->read_index].resource.Get() : nullptr, DXGI_FORMAT_R32G32_UINT);
    storage->Srv(inputs.frame_slot, 8, storage->history[writeIndex].resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Uav(inputs.frame_slot, 0, storage->raw.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Uav(inputs.frame_slot, 1, storage->history[writeIndex].resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Uav(inputs.frame_slot, 2, storage->resolved.resource.Get(), DXGI_FORMAT_R16_FLOAT);
    storage->Uav(inputs.frame_slot, 3, storage->contact.resource.Get(), DXGI_FORMAT_R16_FLOAT);
    storage->Uav(inputs.frame_slot, 4, storage->metadata_history[writeIndex].resource.Get(), DXGI_FORMAT_R32G32_UINT);
    if(storage->adaptive_compute)
    {
        storage->Srv(inputs.frame_slot,kDescriptorCount,inputs.adaptive_density.resource,DXGI_FORMAT_R8_UINT);
    }

    Impl::Pending pending;
    pending.graph = &graph; pending.targets = storage; pending.active = aoActive;
    pending.slot = inputs.frame_slot; pending.write_index = writeIndex;
    pending.adaptive=storage->adaptive_compute;
    std::size_t import_index{};
    bool bound = true;
    Result binding_result = Result::Success();
    auto import = [&](AllocationResource &allocation, std::string_view name) {
        if (auto checked = allocation.EnsureNativeContract(storage->device.Get()); !checked)
        { binding_result = checked; return ResourceVersion{}; }
        const auto handle = retained ? retained->textures[import_index] :
            graph.ImportTexture({allocation.resource.Get(), allocation.graph_completion, allocation.graph_queue, {}, allocation.NativeContract()},
                allocation.graph_access, name);
        if (retained) bound &= graph.BindTexture(handle, {allocation.resource.Get(),
            allocation.graph_completion, allocation.graph_queue, {}, allocation.NativeContract()}, allocation.graph_access);
        if (capture) capture->textures[import_index] = handle;
        ++import_index;
        pending.imports.push_back({&allocation, handle});
        return graph.Version(handle);
    };
    auto execute = [storage, slot = inputs.frame_slot](std::uint32_t pipeline, ViewExtent extent) {
        return [storage, slot, pipeline, extent](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(storage->root.Get());
            commands->SetPipelineState(storage->pipelines[pipeline].Get());
            commands->SetComputeRootConstantBufferView(0, storage->constants.resource->GetGPUVirtualAddress() + UINT64(slot) * kConstantStride);
            commands->SetComputeRootDescriptorTable(1, storage->Gpu(slot, 0));
            commands->SetComputeRootDescriptorTable(2, storage->Gpu(slot, kSrvCount));
            if (pipeline == 4) commands->SetComputeRootDescriptorTable(3, storage->Gpu(slot, kDescriptorCount));
            commands->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        };
    };
    auto readGuides = [&](PassBuilder &pass) {
        pass.Read(inputs.depth.version, Access::ComputeRead);
        pass.Read(inputs.normal.version, Access::ComputeRead);
        pass.Read(inputs.metadata.version, Access::ComputeRead);
    };
    const auto pass_binding = [&](std::size_t index, std::uint32_t id,
        std::string_view name, QueueHint queue) -> std::optional<PassBuilder> {
        if (retained) return graph.BindPass(retained->passes[index], id);
        if (capture) capture->passes[index] = graph.PassCount();
        return graph.AddPass(id, name, queue);
    };
    if (storage->ao_enabled)
    {
        auto raw = import(storage->raw, "AO.Raw");
        auto previous = import(storage->history[storage->read_index], "AO.PreviousHistory");
        auto previousMetadata = import(storage->metadata_history[storage->read_index], "AO.PreviousMetadata");
        auto history = import(storage->history[writeIndex], "AO.CurrentHistory");
        auto metadata = import(storage->metadata_history[writeIndex], "AO.CurrentMetadata");
        auto resolved = import(storage->resolved, "AO.Resolved");
        if (!binding_result) return binding_result;
        if (!bound) return AoFailure(ErrorCode::InvalidState, "AO retained texture binding failed.");
        auto horizonBinding = pass_binding(0, 45, storage->adaptive_compute?"AdaptiveGTAO":"GTAOHorizon", inputs.queue);
        if (!horizonBinding) return AoFailure(ErrorCode::InvalidState, "AO horizon pass is missing.");
        auto &horizon = *horizonBinding;
        if (!retained) readGuides(horizon);
        if(storage->adaptive_compute)
        {
            if (!retained) { horizon.Read(inputs.adaptive_density.version,Access::ComputeRead); raw=horizon.Write(raw,Access::UnorderedWrite); }
            else raw = retained->raw;
            horizon.SetExecute(execute(4,aoActive)); outputs.adaptive_density_used=true;
        }
        else{raw=retained?retained->raw:horizon.Write(raw,Access::UnorderedWrite);horizon.SetExecute(execute(0,aoActive));}
        if (capture) capture->raw = raw;
        auto temporalBinding = pass_binding(1,46,"GTAOTemporal",inputs.queue);
        if (!temporalBinding) return AoFailure(ErrorCode::InvalidState, "AO retained temporal pass is missing.");
        auto &temporal = *temporalBinding;
        if (retained) { history = retained->history_output; metadata = retained->metadata; }
        else
        {
            readGuides(temporal); temporal.Read(raw, Access::ComputeRead);
            temporal.Read(inputs.motion.version, Access::ComputeRead);
            temporal.Read(inputs.surface_history.version, Access::ComputeRead);
            if (historyValid)
            {
                temporal.Read(previous, Access::ComputeRead);
                temporal.Read(previousMetadata, Access::ComputeRead);
            }
            history = temporal.Write(history, Access::UnorderedWrite);
            metadata = temporal.Write(metadata, Access::UnorderedWrite);
        }
        if (capture) { capture->history_output = history; capture->metadata = metadata; }
        temporal.SetExecute(execute(1, aoActive));
        auto bilateralBinding = pass_binding(2,47,"GTAOBilateral",inputs.queue);
        if (!bilateralBinding) return AoFailure(ErrorCode::InvalidState, "AO retained bilateral pass is missing.");
        auto &bilateral = *bilateralBinding;
        if (retained) resolved = retained->resolved;
        else { readGuides(bilateral); bilateral.Read(history, Access::ComputeRead);
            resolved = bilateral.Write(resolved, Access::UnorderedWrite); }
        if (capture) capture->resolved = resolved;
        bilateral.SetExecute(execute(2, active));
        // The next graph imports the same physical histories; record a common
        // shader-read layout even when the current frame has no history consumer.
        if (!retained)
        {
            graph.SetFinalAccess(history, Access::ComputeRead);
            graph.SetFinalAccess(metadata, Access::ComputeRead);
        }
        outputs.ao = resolved; outputs.ao_resource = storage->resolved.resource.Get();
        outputs.ao_srv = storage->export_descriptors->GetCPUDescriptorHandleForHeapStart();
    }
    if (storage->contact_enabled)
    {
        auto contact = import(storage->contact, "AO.ContactVisibility");
        if (!binding_result) return binding_result;
        if (!bound) return AoFailure(ErrorCode::InvalidState, "AO retained contact binding failed.");
        auto contactBinding = pass_binding(3,48,"ContactShadows",QueueHint::Direct);
        if (!contactBinding) return AoFailure(ErrorCode::InvalidState, "AO retained contact pass is missing.");
        auto &pass = *contactBinding;
        if (retained) contact = retained->contact_output;
        else { readGuides(pass); contact = pass.Write(contact, Access::UnorderedWrite); }
        if (capture) capture->contact_output = contact;
        pass.SetExecute(execute(3, active));
        outputs.contact = contact; outputs.contact_resource = storage->contact.resource.Get();
        outputs.contact_srv = storage->export_descriptors->GetCPUDescriptorHandleForHeapStart();
        outputs.contact_srv.ptr += storage->descriptor_stride;
    }
    impl_->pending = std::move(pending);
    if (capture) capture->valid = true;
    return Result::Success();
}

Result AmbientOcclusionSystem::CompleteSubmission(const RenderGraphBuilder &graph,
    const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph)
    {
        if (impl_->targets && !impl_->targets->ao_enabled && !impl_->targets->contact_enabled)
            return Result::Success();
        return AoFailure(ErrorCode::InvalidState, "AO has no registered submission to complete.");
    }
    if (pending.graph != &graph) return AoFailure(ErrorCode::InvalidArgument, "AO completion refers to a different graph.");
    auto storage = pending.targets;
    storage->adaptive_applied=success&&pending.adaptive;
    storage->completion.Merge(completion);
    storage->slot_completion[pending.slot].Merge(completion);
    for (const auto &imported : pending.imports)
    {
        const auto resourceCompletion = graph.ResourceCompletion(imported.handle);
        storage->completion.Merge(resourceCompletion);
        storage->slot_completion[pending.slot].Merge(resourceCompletion);
        imported.allocation->graph_completion.Merge(completion);
        imported.allocation->graph_completion.Merge(resourceCompletion);
        if (success)
        {
            imported.allocation->graph_access = graph.FinalAccess(imported.handle);
            imported.allocation->graph_queue = graph.FinalQueue(imported.handle);
        }
    }
    if (success && storage->ao_enabled)
    {
        storage->read_index = pending.write_index;
        storage->previous_active = pending.active;
        storage->history_valid = true;
    }
    else if (!success)
    {
        // A failed graph can have submitted a prefix; states are no longer a
        // reusable whole-frame contract. Retain leases/completion until drain.
        storage->history_valid = false; storage->usable = false;
    }
    pending = {};
    return Result::Success();
}

void AmbientOcclusionSystem::ResetHistory() noexcept
{
    if (impl_->targets) impl_->targets->history_valid = false;
}
Result AmbientOcclusionSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return AoFailure(ErrorCode::InvalidState, "Complete the pending AO submission before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return AoFailure(ErrorCode::InvalidState, "AO reset requires completed GPU uses.");
    impl_->targets.reset();
    return Result::Success();
}
} // namespace hs
