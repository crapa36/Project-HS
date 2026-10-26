#include "renderer_reconstruction_guides.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kSrvCount = 17, kUavCount = 6, kDescriptorCount = kSrvCount + kUavCount, kConstantStride = 256;
std::atomic<std::uint64_t> g_reconstruction_guide_generation{};
constexpr std::array<std::string_view, 2> kShaderProducts{
    "render/shader/reconstruction_guides_cs.dxil", "render/shader/reconstruction_pre_exposure_cs.dxil"};
Result GuideFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_reconstruction_guides", std::move(message));
}
Result NativeFailure(std::string_view operation, HRESULT error)
{
    return GuideFailure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width != 0 && extent.height != 0 &&
        extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
DirectX::XMFLOAT4 SizeConstants(ViewExtent extent)
{
    return {static_cast<float>(extent.width), static_cast<float>(extent.height),
            1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};
}
bool FitsTexture(const ReconstructionGuideResource &resource, ViewExtent extent, DXGI_FORMAT format)
{
    if (!resource.resource || !resource.version.resource || resource.version.resource.kind != ResourceKind::Texture)
        return false;
    const auto description = resource.resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && description.DepthOrArraySize == 1 &&
        description.SampleDesc.Count == 1 && description.Width >= extent.width && description.Height >= extent.height &&
        description.Format == format;
}
bool FitsDepth(const ReconstructionGuideResource &resource, ViewExtent extent)
{
    return FitsTexture(resource, extent, DXGI_FORMAT_R32_TYPELESS) ||
        FitsTexture(resource, extent, DXGI_FORMAT_D32_FLOAT) || FitsTexture(resource, extent, DXGI_FORMAT_R32_FLOAT);
}
Result CreateTexture(ID3D12Device *device, D3D12MA::Allocator *allocator, ViewExtent extent,
                     DXGI_FORMAT format, AllocationResource &resource)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    auto native = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(native)) return NativeFailure("Query reconstruction guide format", native);
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return GuideFailure(ErrorCode::TaskFailed, "Reconstruction guide format lacks texture, shader-load or UAV-store support.");
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = extent.width; description.Height = extent.height;
    description.DepthOrArraySize = 1; description.MipLevels = 1;
    description.Format = format; description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    native = allocator->CreateResource(&allocation, &description, D3D12_RESOURCE_STATE_COMMON, nullptr,
        &resource.allocation, IID_PPV_ARGS(resource.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create reconstruction guide texture", native);
    resource.graph_access = Access::Common;
    return Result::Success();
}
} // namespace

struct ReconstructionGuidesTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 2> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors, export_descriptors;
    AllocationResource constants, motion, responsive, exposure, raw_output, pre_exposed, depth, composition;
    std::byte *mapped{};
    ViewExtent render_capacity, output_extent;
    std::uint32_t frame_slot_count{}, descriptor_stride{};
    std::vector<CompletionSet> slot_completion;
    CompletionSet completion;
    std::uint64_t bytes{};
    std::uint64_t generation{};
    bool usable{true};
    ~ReconstructionGuidesTargets()
    {
        if (mapped && constants.resource) constants.resource->Unmap(0, nullptr);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot, std::uint32_t offset) const
    {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slot * kDescriptorCount + offset) * descriptor_stride;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t slot, std::uint32_t offset) const
    {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64(slot * kDescriptorCount + offset) * descriptor_stride;
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
struct ReconstructionGuidesSystem::Impl
{
    std::shared_ptr<ReconstructionGuidesTargets> targets;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<ReconstructionGuidesTargets> targets;
        std::vector<Import> imports;
        std::uint32_t slot{};
    } pending;
};
ReconstructionGuidesSystem::ReconstructionGuidesSystem() : impl_(std::make_unique<Impl>()) {}
ReconstructionGuidesSystem::~ReconstructionGuidesSystem() = default;
std::uint64_t ReconstructionGuidesCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t ReconstructionGuidesSystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }

AllocationResource ReconstructionGuidesSystem::RetainPreExposedOutput() const
{
    AllocationResource retained;
    const auto storage = impl_->targets;
    if (!storage || !storage->pre_exposed.resource || !storage->pre_exposed.allocation)
        return retained;
    retained.resource = storage->pre_exposed.resource;
    retained.allocation = storage->pre_exposed.allocation;
    retained.allocation->AddRef();
    retained.graph_access = storage->pre_exposed.graph_access;
    retained.graph_queue = storage->pre_exposed.graph_queue;
    retained.graph_completion = storage->pre_exposed.graph_completion;
    return retained;
}

Result ReconstructionGuidesSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ViewExtent capacity, ViewExtent output, std::uint32_t frameSlots, const ShaderReader &readShader,
    ReconstructionGuidesCandidate &candidate, bool compositionGuide) const
{
    if (!device || !allocator || !ValidExtent(capacity) || !ValidExtent(output) || !readShader ||
        frameSlots == 0 || frameSlots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / kDescriptorCount)
        return GuideFailure(ErrorCode::InvalidArgument, "Invalid reconstruction guide device, extents, frame slots or shader reader.");
    auto next = std::make_shared<ReconstructionGuidesTargets>();
    next->device = device; next->allocator = allocator;
    next->render_capacity = capacity; next->output_extent = output;
    next->frame_slot_count = frameSlots; next->slot_completion.resize(frameSlots);
    for (auto [target, extent, format] : {
        std::tuple{&next->motion, capacity, DXGI_FORMAT_R16G16_FLOAT},
        std::tuple{&next->depth, capacity, DXGI_FORMAT_R32_FLOAT},
        std::tuple{&next->responsive, capacity, DXGI_FORMAT_R8_UNORM},
        std::tuple{&next->exposure, ViewExtent{1, 1}, DXGI_FORMAT_R32_FLOAT},
        std::tuple{&next->raw_output, output, DXGI_FORMAT_R16G16B16A16_FLOAT},
        std::tuple{&next->pre_exposed, output, DXGI_FORMAT_R16G16B16A16_FLOAT}})
    {
        if (auto created = CreateTexture(device, allocator, extent, format, *target); !created) return created;
        next->bytes += target->allocation->GetSize();
    }
    if (compositionGuide)
    {
        if (auto created = CreateTexture(device, allocator, capacity, DXGI_FORMAT_R8_UNORM, next->composition); !created) return created;
        next->bytes += next->composition.allocation->GetSize();
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 13, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0, 13, 0};
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[0].Descriptor = {0, 13};
    parameters[1].ParameterType = parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[0]}; parameters[2].DescriptorTable = {1, &ranges[1]};
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 3; rootDescription.pParameters = parameters;
    ComPtr<ID3DBlob> signature, diagnostics;
    auto native = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics);
    if (FAILED(native))
    {
        auto failure = NativeFailure("Serialize reconstruction guide root", native);
        if (diagnostics && diagnostics->GetBufferSize())
            return GuideFailure(ErrorCode::TaskFailed, std::string(failure.Message()) + " " +
                std::string(static_cast<const char *>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()));
        return failure;
    }
    native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&next->root));
    if (FAILED(native)) return NativeFailure("Create reconstruction guide root", native);
    for (std::uint32_t index = 0; index < kShaderProducts.size(); ++index)
    {
        std::vector<std::byte> bytecode;
        if (auto read = readShader(kShaderProducts[index], bytecode); !read) return read;
        if (bytecode.empty()) return GuideFailure(ErrorCode::TaskFailed, "Selected reconstruction guide shader is empty: " + std::string(kShaderProducts[index]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = next->root.Get(); pipeline.CS = {bytecode.data(), bytecode.size()};
        native = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&next->pipelines[index]));
        if (FAILED(native)) return NativeFailure("Create reconstruction guide pipeline " + std::string(kShaderProducts[index]), native);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = frameSlots * kDescriptorCount; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->descriptors));
    if (FAILED(native)) return NativeFailure("Create reconstruction guide descriptors", native);
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors = 1; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->export_descriptors));
    if (FAILED(native)) return NativeFailure("Create reconstructed output SRV", native);
    D3D12_SHADER_RESOURCE_VIEW_DESC exportView{};
    exportView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; exportView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    exportView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; exportView.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(next->pre_exposed.resource.Get(), &exportView,
                                   next->export_descriptors->GetCPUDescriptorHandleForHeapStart());
    D3D12MA::ALLOCATION_DESC uploadAllocation{}; uploadAllocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload{};
    upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; upload.Width = UINT64(frameSlots) * kConstantStride;
    upload.Height = 1; upload.DepthOrArraySize = 1; upload.MipLevels = 1;
    upload.SampleDesc.Count = 1; upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    native = allocator->CreateResource(&uploadAllocation, &upload, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        &next->constants.allocation, IID_PPV_ARGS(next->constants.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create reconstruction guide constants", native);
    D3D12_RANGE noRead{0, 0}; void *mapped{};
    native = next->constants.resource->Map(0, &noRead, &mapped);
    if (FAILED(native)) return NativeFailure("Map reconstruction guide constants", native);
    next->mapped = static_cast<std::byte *>(mapped);
    next->bytes += next->constants.allocation->GetSize();
    candidate.targets_ = std::move(next);
    return Result::Success();
}

Result ReconstructionGuidesSystem::Commit(ReconstructionGuidesCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return GuideFailure(ErrorCode::InvalidState, "Complete the reconstruction submission before target replacement.");
    if (!candidate.targets_) return GuideFailure(ErrorCode::InvalidArgument, "Reconstruction guide candidate was not prepared.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return GuideFailure(ErrorCode::InvalidState, "Old reconstruction guide targets remain in GPU use.");
    candidate.targets_->generation=g_reconstruction_guide_generation.fetch_add(1,std::memory_order_relaxed)+1;
    impl_->targets = std::move(candidate.targets_);
    return Result::Success();
}

bool ReconstructionGuidesSystem::MatchesLayout(const ReconstructionGuidesInputs &in,bool external,
    std::string_view provider,const ReconstructionGuidesLayout &layout) const noexcept
{
    const auto storage=impl_->targets;
    if(!storage||!storage->usable||layout.target_generation!=storage->generation||
        layout.external!=external||layout.provider_name!=provider||
        layout.transparent_guides!=in.transparent_guides_enabled||
        layout.screen_overlay!=bool(in.screen_overlay_alpha.version.resource)||
        layout.composition!=bool(storage->composition.resource))return false;
    const std::array<ResourceVersion,16> current{in.depth.version,in.motion.version,in.metadata.version,
        in.surface_history.version,in.composite_hdr.version,in.opaque_hdr.version,
        in.exposure.version,in.transparency_revealage.version,in.transparent_depth.version,
        in.transparent_motion.version,in.transparent_history.version,in.transparent_classification.version,
        in.transparent_metadata.version,in.transparent_overflow.version,in.transparent_transport.version,
        in.screen_overlay_alpha.version};
    for(std::size_t i=0;i<current.size();++i)
        if((i<8||(i<15&&layout.transparent_guides)||(i==15&&layout.screen_overlay))&&
            layout.inputs[i]!=current[i])return false;
    return true;
}
Result ReconstructionGuidesSystem::Register(RenderGraphBuilder &graph, const ReconstructionGuidesInputs &inputs,
    ReconstructionGuidesConstants constants, SdkReconstructionFrame frame,
    ExecuteReconstruction executeReconstruction, std::string_view providerName, ReconstructionGuidesOutputs &outputs,
    RegisterExternalReconstruction external,ReconstructionGuidesLayout *layout)
{return RegisterImpl(graph,inputs,constants,std::move(frame),std::move(executeReconstruction),
    providerName,outputs,std::move(external),layout,nullptr);}
Result ReconstructionGuidesSystem::BindRetained(RenderGraphBuilder &graph,const ReconstructionGuidesInputs &inputs,
    ReconstructionGuidesConstants constants,SdkReconstructionFrame frame,
    ExecuteReconstruction executeReconstruction,std::string_view providerName,
    ReconstructionGuidesOutputs &outputs,const ReconstructionGuidesLayout &layout,
    RegisterExternalReconstruction externalBind)
{return RegisterImpl(graph,inputs,constants,std::move(frame),std::move(executeReconstruction),
    providerName,outputs,std::move(externalBind),nullptr,&layout);}
Result ReconstructionGuidesSystem::RegisterImpl(RenderGraphBuilder &graph,const ReconstructionGuidesInputs &inputs,
    ReconstructionGuidesConstants constants,SdkReconstructionFrame frame,
    ExecuteReconstruction executeReconstruction,std::string_view providerName,
    ReconstructionGuidesOutputs &outputs,RegisterExternalReconstruction external,
    ReconstructionGuidesLayout *layout,const ReconstructionGuidesLayout *retained)
{
    outputs = {};
    auto storage = impl_->targets;
    if (!storage || !storage->usable || impl_->pending.graph || (!executeReconstruction && !external))
        return GuideFailure(ErrorCode::InvalidState, "Reconstruction guides require reusable targets and a configured native execution callback.");
    if(layout){*layout={};layout->target_generation=storage->generation;
        layout->external=bool(external);layout->provider_name=providerName;
        layout->transparent_guides=inputs.transparent_guides_enabled;
        layout->screen_overlay=bool(inputs.screen_overlay_alpha.version.resource);
        layout->composition=bool(storage->composition.resource);
        layout->inputs={inputs.depth.version,inputs.motion.version,inputs.metadata.version,
            inputs.surface_history.version,inputs.composite_hdr.version,inputs.opaque_hdr.version,
            inputs.exposure.version,inputs.transparency_revealage.version,inputs.transparent_depth.version,
            inputs.transparent_motion.version,inputs.transparent_history.version,
            inputs.transparent_classification.version,inputs.transparent_metadata.version,
            inputs.transparent_overflow.version,inputs.transparent_transport.version,
            inputs.screen_overlay_alpha.version};}
    if(retained)
    {
        if(!MatchesLayout(inputs,bool(external),providerName,*retained))
            return GuideFailure(ErrorCode::InvalidState,"Retained reconstruction topology changed.");
        for(UINT i=0;i<retained->imports.size();++i)
            if((i<6||retained->composition)&&
                (!retained->imports[i]||retained->imports[i].kind!=ResourceKind::Texture||
                    retained->imports[i].value>graph.ResourceCount()))
                return GuideFailure(ErrorCode::InvalidState,"Retained reconstruction import slot changed.");
        for(UINT i=0;i<3;++i)
            if(!graph.BindPass(retained->pass_indices[i],i==0?49u:(i==1?9u:51u)))
                return GuideFailure(ErrorCode::InvalidState,"Retained reconstruction pass changed.");
    }
    const auto active = inputs.render_active;
    if (!ValidExtent(active) || active.width > storage->render_capacity.width || active.height > storage->render_capacity.height ||
        inputs.frame_slot >= storage->frame_slot_count || !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed))
        return GuideFailure(ErrorCode::InvalidArgument, "Invalid active reconstruction extent or completed frame slot.");
    if (!FitsDepth(inputs.depth, active) || !FitsTexture(inputs.motion, active, DXGI_FORMAT_R16G16_FLOAT) ||
        !FitsTexture(inputs.metadata, active, DXGI_FORMAT_R32G32_UINT) ||
        !FitsTexture(inputs.surface_history, active, DXGI_FORMAT_R32G32_FLOAT) ||
        !FitsTexture(inputs.composite_hdr, active, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !FitsTexture(inputs.opaque_hdr, active, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !FitsTexture(inputs.transparency_revealage, active, DXGI_FORMAT_R16_FLOAT))
        return GuideFailure(ErrorCode::InvalidArgument, "Reconstruction guide texture, version, format or capacity is invalid.");
    if (!inputs.exposure.resource || !inputs.exposure.version.resource || inputs.exposure.version.resource.kind != ResourceKind::Buffer ||
        inputs.exposure.resource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        inputs.exposure.resource->GetDesc().Width < 2 * sizeof(float))
        return GuideFailure(ErrorCode::InvalidArgument, "Reconstruction requires the current/previous exposure float buffer.");
    if (inputs.transparent_guides_enabled &&
        (!FitsTexture(inputs.transparent_depth, active, DXGI_FORMAT_R32_FLOAT) ||
         !FitsTexture(inputs.transparent_motion, active, DXGI_FORMAT_R16G16_FLOAT) ||
         !FitsTexture(inputs.transparent_history, active, DXGI_FORMAT_R32G32_FLOAT) ||
         !FitsTexture(inputs.transparent_classification, active, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
         !FitsTexture(inputs.transparent_metadata, active, DXGI_FORMAT_R32G32_UINT) ||
         !FitsTexture(inputs.transparent_overflow, active, DXGI_FORMAT_R32_UINT) ||
         !FitsTexture(inputs.transparent_transport, active, DXGI_FORMAT_R16G16_FLOAT)))
        return GuideFailure(ErrorCode::InvalidArgument, "Transparent reconstruction guides require matching active textures and formats.");
    if (!std::isfinite(constants.jitter_uv.x) || !std::isfinite(constants.jitter_uv.y) ||
        std::abs(constants.jitter_uv.x * active.width) > 0.5f ||
        std::abs(constants.jitter_uv.y * active.height) > 0.5f)
        return GuideFailure(ErrorCode::InvalidArgument, "Reconstruction jitter is outside the SDK pixel range.");
    constants.render_size = SizeConstants(active); constants.output_size = SizeConstants(storage->output_extent);
    constants.frame_info.y = inputs.transparent_guides_enabled ? 1u : 0u;
    constants.frame_info.z = storage->composition.resource ? 1u : 0u;
    constants.frame_info.w = providerName == "xess" ? 1u : 0u;
    constants.reactive_parameters.z = inputs.screen_overlay_alpha.version.resource ? 1.0f : 0.0f;
    if (inputs.screen_overlay_alpha.version.resource &&
        !FitsTexture(inputs.screen_overlay_alpha, inputs.render_active, DXGI_FORMAT_R8_UNORM))
        return GuideFailure(ErrorCode::InvalidArgument, "Screen overlay alpha guide has an invalid extent or format.");
    std::memcpy(storage->mapped + SIZE_T(inputs.frame_slot) * kConstantStride, &constants, sizeof(constants));
    storage->Srv(inputs.frame_slot, 0, inputs.depth.resource, DXGI_FORMAT_R32_FLOAT);
    storage->Srv(inputs.frame_slot, 1, inputs.motion.resource, DXGI_FORMAT_R16G16_FLOAT);
    storage->Srv(inputs.frame_slot, 2, inputs.metadata.resource, DXGI_FORMAT_R32G32_UINT);
    storage->Srv(inputs.frame_slot, 3, inputs.surface_history.resource, DXGI_FORMAT_R32G32_FLOAT);
    storage->Srv(inputs.frame_slot, 4, inputs.composite_hdr.resource, DXGI_FORMAT_R16G16B16A16_FLOAT);
    storage->Srv(inputs.frame_slot, 5, inputs.opaque_hdr.resource, DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_SHADER_RESOURCE_VIEW_DESC exposureView{};
    exposureView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    exposureView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    exposureView.Buffer.NumElements = 2; exposureView.Buffer.StructureByteStride = sizeof(float);
    storage->device->CreateShaderResourceView(inputs.exposure.resource, &exposureView, storage->Cpu(inputs.frame_slot, 6));
    storage->Srv(inputs.frame_slot, 7, inputs.transparency_revealage.resource, DXGI_FORMAT_R16_FLOAT);
    storage->Srv(inputs.frame_slot, 8, storage->raw_output.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    storage->Srv(inputs.frame_slot, 9, inputs.transparent_guides_enabled ? inputs.transparent_depth.resource : nullptr, DXGI_FORMAT_R32_FLOAT);
    storage->Srv(inputs.frame_slot, 10, inputs.transparent_guides_enabled ? inputs.transparent_motion.resource : nullptr, DXGI_FORMAT_R16G16_FLOAT);
    storage->Srv(inputs.frame_slot, 11, inputs.transparent_guides_enabled ? inputs.transparent_history.resource : nullptr, DXGI_FORMAT_R32G32_FLOAT);
    storage->Srv(inputs.frame_slot, 12, inputs.transparent_guides_enabled ? inputs.transparent_classification.resource : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    storage->Srv(inputs.frame_slot, 13, inputs.transparent_guides_enabled ? inputs.transparent_metadata.resource : nullptr, DXGI_FORMAT_R32G32_UINT);
    storage->Srv(inputs.frame_slot, 14, inputs.transparent_guides_enabled ? inputs.transparent_overflow.resource : nullptr, DXGI_FORMAT_R32_UINT);
    storage->Srv(inputs.frame_slot, 15, inputs.transparent_guides_enabled ? inputs.transparent_transport.resource : nullptr, DXGI_FORMAT_R16G16_FLOAT);
    storage->Srv(inputs.frame_slot, 16, inputs.screen_overlay_alpha.resource, DXGI_FORMAT_R8_UNORM);
    storage->Uav(inputs.frame_slot, 0, storage->motion.resource.Get(), DXGI_FORMAT_R16G16_FLOAT);
    storage->Uav(inputs.frame_slot, 1, storage->responsive.resource.Get(), DXGI_FORMAT_R8_UNORM);
    storage->Uav(inputs.frame_slot, 2, storage->exposure.resource.Get(), DXGI_FORMAT_R32_FLOAT);
    storage->Uav(inputs.frame_slot, 3, storage->pre_exposed.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    storage->Uav(inputs.frame_slot, 4, storage->depth.resource.Get(), DXGI_FORMAT_R32_FLOAT);
    storage->Uav(inputs.frame_slot, 5, storage->composition.resource.Get(), DXGI_FORMAT_R8_UNORM);
    Impl::Pending pending;
    pending.graph = &graph; pending.targets = storage; pending.slot = inputs.frame_slot;
    for(auto *allocation:{&storage->motion,&storage->depth,&storage->responsive,
            &storage->exposure,&storage->raw_output,&storage->pre_exposed,&storage->composition})
        if(allocation->resource)
            if(auto certified=allocation->EnsureNativeContract(storage->device.Get());!certified)
                return certified;
    UINT import_index{};
    bool imports_valid=true;
    auto import = [&](AllocationResource &allocation, std::string_view name) {
        const UINT index=import_index++;
        const auto current=ExternalTexture{allocation.resource.Get(),allocation.graph_completion,
            allocation.graph_queue,{},allocation.NativeContract()};
        const auto handle=retained?retained->imports[index]:
            graph.ImportTexture(current,allocation.graph_access,name);
        if(retained)imports_valid&=graph.BindTexture(handle,current,allocation.graph_access);
        if(layout)layout->imports[index]=handle;
        pending.imports.push_back({&allocation, handle});
        return graph.Version(handle);
    };
    auto motion = import(storage->motion, "Reconstruction.Motion");
    auto depth = import(storage->depth, "Reconstruction.Depth");
    auto responsive = import(storage->responsive, "Reconstruction.Responsive");
    auto exposure = import(storage->exposure, "Reconstruction.Exposure");
    auto raw = import(storage->raw_output, "Reconstruction.RawOutput");
    auto preExposed = import(storage->pre_exposed, "Reconstruction.PreExposedOutput");
    ResourceVersion composition;
    if (storage->composition.resource) composition = import(storage->composition, "Reconstruction.CompositionCoverage");
    if(!imports_valid)return GuideFailure(ErrorCode::InvalidState,"Retained reconstruction binding changed.");
    auto execute = [storage, slot = inputs.frame_slot](std::uint32_t pipeline, ViewExtent extent) {
        return [storage, slot, pipeline, extent](RenderPassContext &context) -> Result {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(storage->root.Get());
            commands->SetPipelineState(storage->pipelines[pipeline].Get());
            commands->SetComputeRootConstantBufferView(0, storage->constants.resource->GetGPUVirtualAddress() + UINT64(slot) * kConstantStride);
            commands->SetComputeRootDescriptorTable(1, storage->Gpu(slot, 0));
            commands->SetComputeRootDescriptorTable(2, storage->Gpu(slot, kSrvCount));
            commands->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            return Result::Success();
        };
    };
    if(layout)layout->pass_indices[0]=graph.PassCount();
    auto guides=retained?*graph.BindPass(retained->pass_indices[0],49):
        graph.AddPass(49,"Reconstruction Guides",QueueHint::Direct);
    if (!retained&&inputs.screen_overlay_alpha.version.resource)
        guides.Read(inputs.screen_overlay_alpha.version, Access::ComputeRead);
    if(!retained)for (const auto &guide : {inputs.depth, inputs.motion, inputs.metadata, inputs.surface_history,
         inputs.composite_hdr, inputs.opaque_hdr, inputs.exposure, inputs.transparency_revealage})
        guides.Read(guide.version, Access::ComputeRead);
    if (!retained&&inputs.transparent_guides_enabled)
        for (const auto &guide : {inputs.transparent_depth, inputs.transparent_motion, inputs.transparent_history,
             inputs.transparent_classification, inputs.transparent_metadata, inputs.transparent_overflow,
             inputs.transparent_transport})
            guides.Read(guide.version, Access::ComputeRead);
    motion=retained?retained->outputs[2]:guides.Write(motion,Access::UnorderedWrite);
    depth=retained?retained->outputs[5]:guides.Write(depth,Access::UnorderedWrite);
    responsive=retained?retained->outputs[3]:guides.Write(responsive,Access::UnorderedWrite);
    exposure=retained?retained->outputs[4]:guides.Write(exposure,Access::UnorderedWrite);
    if(composition.resource)composition=retained?retained->outputs[6]:guides.Write(composition,Access::UnorderedWrite);
    guides.SetExecuteChecked(execute(0, active));
    if(layout)layout->pass_indices[1]=graph.PassCount();
    auto reconstruct=retained?*graph.BindPass(retained->pass_indices[1],9):
        graph.AddPass(9,std::string(providerName)+" Reconstruction",external?QueueHint::Compute:QueueHint::Direct);
    if(!retained)for (const auto version : {inputs.composite_hdr.version, depth, motion, responsive, exposure})
        reconstruct.Read(version, Access::ComputeRead);
    if(!retained&&composition.resource)reconstruct.Read(composition,Access::ComputeRead);
    raw=retained?retained->outputs[1]:reconstruct.Write(raw,Access::UnorderedWrite);
    // The adapter is owned by the central renderer. It remains initialized and
    // alive until all graph uses drain; the callback itself retains native targets.
    const auto fillFrame = [storage, inputs, constants](SdkReconstructionFrame frame) {
        frame.color = inputs.composite_hdr.resource; // Raw scene-linear Rec.709, not engine pre-exposed color.
        frame.depth = storage->depth.resource.Get(); frame.motion = storage->motion.resource.Get();
        frame.responsive = storage->responsive.resource.Get(); frame.composition = storage->composition.resource.Get();
        frame.exposure = storage->exposure.resource.Get(); frame.output = storage->raw_output.resource.Get();
        frame.active = {0, 0, inputs.render_active.width, inputs.render_active.height};
        frame.output_rect = {0, 0, storage->output_extent.width, storage->output_extent.height};
        frame.camera.jitter_pixels[0] = constants.jitter_uv.x * inputs.render_active.width;
        frame.camera.jitter_pixels[1] = constants.jitter_uv.y * inputs.render_active.height;
        frame.camera.reset_history = inputs.reset_history || constants.frame_info.x == 0;
        frame.input_pre_exposure = 1.0f; frame.exposure_scale = 1.0f;
        return frame;
    };
    if (external)
    {
        std::vector<ExternalResourceAccess> boundaries;
        for (const auto version : {inputs.composite_hdr.version, depth, motion, responsive, exposure, raw})
            boundaries.push_back({version.resource, Access::Common, Access::Common});
        if (composition.resource) boundaries.push_back({composition.resource, Access::Common, Access::Common});
        if (auto registered = external(reconstruct, fillFrame(frame), inputs.frame_slot, boundaries, {storage}); !registered)
            return registered;
    }
    else
    {
        std::vector<ExternalResourceAccess> packet_accesses{
            {inputs.composite_hdr.version.resource, Access::ComputeRead, Access::ComputeRead},
            {depth.resource, Access::ComputeRead, Access::ComputeRead},
            {motion.resource, Access::ComputeRead, Access::ComputeRead},
            {responsive.resource, Access::ComputeRead, Access::ComputeRead},
            {exposure.resource, Access::ComputeRead, Access::ComputeRead},
        };
        if (composition.resource)
            packet_accesses.push_back({composition.resource, Access::ComputeRead, Access::ComputeRead});
        packet_accesses.push_back({raw.resource, Access::UnorderedWrite, Access::UnorderedWrite});
        reconstruct.SetExecuteOnOwner([executeReconstruction = std::move(executeReconstruction), fillFrame, frame,
                                       packet_accesses = std::move(packet_accesses)](RenderPassContext &context) -> Result {
            return context.RecordLegacyResourceCommands(packet_accesses,
                [executeReconstruction, commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList()),
                 packet = fillFrame(frame)]() mutable -> Result {
                    return executeReconstruction(commands, packet);
                });
        });
    }
    if(layout)layout->pass_indices[2]=graph.PassCount();
    auto preExposure=retained?*graph.BindPass(retained->pass_indices[2],51):
        graph.AddPass(51,"Reconstruction PreExposure",QueueHint::Direct);
    if(!retained){preExposure.Read(raw,Access::ComputeRead);preExposure.Read(inputs.exposure.version,Access::ComputeRead);}
    preExposed=retained?retained->outputs[0]:preExposure.Write(preExposed,Access::UnorderedWrite);
    preExposure.SetExecuteChecked(execute(1, storage->output_extent));
    outputs = {preExposed, raw, motion, responsive, exposure, depth, composition, storage->pre_exposed.resource.Get(),
               storage->export_descriptors->GetCPUDescriptorHandleForHeapStart(), storage->depth.resource.Get(),
               storage->motion.resource.Get(), storage};
    if(layout)layout->outputs={preExposed,raw,motion,responsive,exposure,depth,composition};
    impl_->pending = std::move(pending);
    return Result::Success();
}

Result ReconstructionGuidesSystem::CompleteSubmission(const RenderGraphBuilder &graph,
    const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph || pending.graph != &graph)
        return GuideFailure(ErrorCode::InvalidState, "Reconstruction completion does not match its registered graph.");
    auto storage = pending.targets;
    storage->completion.Merge(completion); storage->slot_completion[pending.slot].Merge(completion);
    for (const auto &imported : pending.imports)
    {
        const auto resourceCompletion = graph.ResourceCompletion(imported.handle);
        storage->completion.Merge(resourceCompletion); storage->slot_completion[pending.slot].Merge(resourceCompletion);
        imported.allocation->graph_completion.Merge(completion); imported.allocation->graph_completion.Merge(resourceCompletion);
        if (success)
        {
            imported.allocation->graph_access = graph.FinalAccess(imported.handle);
            imported.allocation->graph_queue = graph.FinalQueue(imported.handle);
        }
    }
    if (!success) storage->usable = false;
    pending = {};
    return Result::Success();
}
Result ReconstructionGuidesSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return GuideFailure(ErrorCode::InvalidState, "Complete pending reconstruction before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return GuideFailure(ErrorCode::InvalidState, "Reconstruction guide reset requires completed GPU uses.");
    impl_->targets.reset();
    return Result::Success();
}
} // namespace hs
