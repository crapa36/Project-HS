#include "renderer_subsurface.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

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
constexpr std::uint32_t kSrvCount = 6, kUavCount = 2, kDescriptorCount = 8;
constexpr std::uint32_t kConstantStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr std::array<std::string_view, 2> kShaderProducts{
    "render/shader/sss_horizontal_cs.dxil", "render/shader/sss_vertical_cs.dxil"};
static_assert(sizeof(SubsurfaceConstants) <= kConstantStride);

Result SssFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_subsurface", std::move(message));
}
Result NativeFailure(std::string_view operation, HRESULT error)
{
    return SssFailure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width != 0 && extent.height != 0 &&
        extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool FitsGuide(const SubsurfaceGuide &guide, ViewExtent active, DXGI_FORMAT format)
{
    if (!guide.resource || !guide.version.resource || guide.version.resource.kind != ResourceKind::Texture)
        return false;
    const auto description = guide.resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        description.DepthOrArraySize == 1 && description.SampleDesc.Count == 1 &&
        description.Width >= active.width && description.Height >= active.height && description.Format == format;
}
bool FitsDepth(const SubsurfaceGuide &guide, ViewExtent active)
{
    return FitsGuide(guide, active, DXGI_FORMAT_R32_TYPELESS) ||
        FitsGuide(guide, active, DXGI_FORMAT_D32_FLOAT) || FitsGuide(guide, active, DXGI_FORMAT_R32_FLOAT);
}
bool FitsParameters(const SubsurfaceGuide &guide)
{
    if (!guide.resource || !guide.version.resource || guide.version.resource.kind != ResourceKind::Buffer)
        return false;
    const auto description = guide.resource->GetDesc();
    return description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && description.Width >= 16 &&
        description.Width % 16 == 0 && guide.resource->GetGPUVirtualAddress() != 0;
}
Result CreateTexture(D3D12MA::Allocator *allocator, ViewExtent extent, AllocationResource &texture)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = extent.width; description.Height = extent.height;
    description.DepthOrArraySize = 1; description.MipLevels = 1;
    description.Format = kColorFormat; description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto result = allocator->CreateResource(&allocation, &description, D3D12_RESOURCE_STATE_COMMON,
        nullptr, &texture.allocation, IID_PPV_ARGS(texture.resource.ReleaseAndGetAddressOf()));
    if (FAILED(result)) return NativeFailure("Create subsurface texture", result);
    texture.graph_access = Access::Common;
    return Result::Success();
}
} // namespace

struct SubsurfaceTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 2> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors, export_descriptors;
    AllocationResource constants, horizontal, corrected;
    std::byte *mapped{};
    ViewExtent capacity;
    bool enabled{}, usable{true};
    std::uint32_t frame_slot_count{}, descriptor_stride{};
    std::vector<CompletionSet> slot_completion;
    CompletionSet completion;
    std::uint64_t bytes{};

    ~SubsurfaceTargets()
    {
        if (mapped && constants.resource) constants.resource->Unmap(0, nullptr);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot, std::uint32_t offset) const
    {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot * kDescriptorCount + offset) * descriptor_stride;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t slot, std::uint32_t offset) const
    {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(slot * kDescriptorCount + offset) * descriptor_stride;
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
    void Uav(std::uint32_t slot, std::uint32_t offset, ID3D12Resource *resource)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = kColorFormat; view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource, nullptr, &view, Cpu(slot, kSrvCount + offset));
    }
};

struct SubsurfaceSystem::Impl
{
    std::shared_ptr<SubsurfaceTargets> targets;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<SubsurfaceTargets> targets;
        std::vector<Import> imports;
        std::uint32_t slot{};
    } pending;
};

SubsurfaceSystem::SubsurfaceSystem() : impl_(std::make_unique<Impl>()) {}
SubsurfaceSystem::~SubsurfaceSystem() = default;
std::uint64_t SubsurfaceCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t SubsurfaceSystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }

Result SubsurfaceSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ViewExtent capacity, bool enabled, std::uint32_t frameSlots, const ShaderReader &readShader,
    SubsurfaceCandidate &candidate) const
{
    if (!device || !allocator || !ValidExtent(capacity) || frameSlots == 0 ||
        frameSlots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / kDescriptorCount ||
        (enabled && !readShader))
        return SssFailure(ErrorCode::InvalidArgument, "Invalid subsurface device, capacity, frame slots or shader reader.");
    auto next = std::make_shared<SubsurfaceTargets>();
    next->device = device; next->allocator = allocator; next->capacity = capacity;
    next->enabled = enabled; next->frame_slot_count = frameSlots;
    next->slot_completion.resize(frameSlots);
    if (!enabled)
    {
        candidate.targets_ = std::move(next);
        return Result::Success();
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{kColorFormat};
    auto native = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(native)) return NativeFailure("Check subsurface format support", native);
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return SssFailure(ErrorCode::TaskFailed, "Subsurface RGBA16F lacks texture, shader-load or typed UAV-store support.");
    if (auto created = CreateTexture(allocator, capacity, next->horizontal); !created) return created;
    if (auto created = CreateTexture(allocator, capacity, next->corrected); !created) return created;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 13, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0, 13, 0};
    D3D12_ROOT_PARAMETER parameters[4]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor = {0, 13};
    parameters[1].ParameterType = parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[0]};
    parameters[2].DescriptorTable = {1, &ranges[1]};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[3].Descriptor = {0, 8};
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 4; rootDescription.pParameters = parameters;
    ComPtr<ID3DBlob> signature, diagnostics;
    native = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                        &signature, &diagnostics);
    if (FAILED(native))
    {
        auto failure = NativeFailure("Serialize subsurface root signature", native);
        if (diagnostics && diagnostics->GetBufferSize())
            return SssFailure(ErrorCode::TaskFailed, std::string(failure.Message()) + " " +
                std::string(static_cast<const char *>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()));
        return failure;
    }
    native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                        IID_PPV_ARGS(&next->root));
    if (FAILED(native)) return NativeFailure("Create subsurface root signature", native);
    for (std::uint32_t index = 0; index < kShaderProducts.size(); ++index)
    {
        std::vector<std::byte> bytecode;
        if (auto read = readShader(kShaderProducts[index], bytecode); !read) return read;
        if (bytecode.empty()) return SssFailure(ErrorCode::TaskFailed,
            "Selected subsurface shader product is empty: " + std::string(kShaderProducts[index]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = next->root.Get(); pipeline.CS = {bytecode.data(), bytecode.size()};
        native = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&next->pipelines[index]));
        if (FAILED(native)) return NativeFailure("Create subsurface compute pipeline " + std::string(kShaderProducts[index]), native);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = frameSlots * kDescriptorCount; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->descriptors));
    if (FAILED(native)) return NativeFailure("Create subsurface descriptors", native);
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors = 1; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->export_descriptors));
    if (FAILED(native)) return NativeFailure("Create subsurface export descriptor", native);
    D3D12_SHADER_RESOURCE_VIEW_DESC exportView{};
    exportView.Format = kColorFormat; exportView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    exportView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    exportView.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(next->corrected.resource.Get(), &exportView,
                                     next->export_descriptors->GetCPUDescriptorHandleForHeapStart());

    D3D12MA::ALLOCATION_DESC uploadAllocation{}; uploadAllocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload{};
    upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; upload.Width = UINT64(frameSlots) * kConstantStride;
    upload.Height = 1; upload.DepthOrArraySize = 1; upload.MipLevels = 1;
    upload.SampleDesc.Count = 1; upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    native = allocator->CreateResource(&uploadAllocation, &upload, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, &next->constants.allocation, IID_PPV_ARGS(next->constants.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create subsurface constant upload", native);
    D3D12_RANGE noRead{0, 0};
    void *mapped{};
    native = next->constants.resource->Map(0, &noRead, &mapped);
    if (FAILED(native)) return NativeFailure("Map subsurface constants", native);
    next->mapped = static_cast<std::byte *>(mapped);
    next->bytes = next->constants.allocation->GetSize() + next->horizontal.allocation->GetSize() +
        next->corrected.allocation->GetSize();
    candidate.targets_ = std::move(next);
    return Result::Success();
}

Result SubsurfaceSystem::Commit(SubsurfaceCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return SssFailure(ErrorCode::InvalidState, "Complete the registered subsurface submission before replacing targets.");
    if (!candidate.targets_) return SssFailure(ErrorCode::InvalidArgument, "Subsurface candidate was not prepared.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return SssFailure(ErrorCode::InvalidState, "Old subsurface targets are still in GPU use.");
    impl_->targets = std::move(candidate.targets_);
    return Result::Success();
}

Result SubsurfaceSystem::Register(RenderGraphBuilder &graph, const SubsurfaceInputs &inputs,
    SubsurfaceConstants constants, SubsurfaceOutputs &outputs, SubsurfaceLayout *layout)
{
    return RegisterFrame(graph, inputs, constants, outputs, layout, nullptr);
}

bool SubsurfaceSystem::MatchesShape(const SubsurfaceLayout &layout) const noexcept
{
    const auto &storage = impl_->targets;
    if (!layout.valid || !storage || layout.enabled != storage->enabled) return false;
    return !storage->enabled || (layout.capacity.width==storage->capacity.width&&layout.capacity.height==storage->capacity.height);
}
bool SubsurfaceSystem::MatchesLayout(const SubsurfaceInputs &inputs, const SubsurfaceLayout &layout) const noexcept
{
    return MatchesShape(layout) && (!impl_->targets->enabled ||
        layout.guides == std::array{inputs.depth.version, inputs.normal.version,
            inputs.metadata.version, inputs.material_parameters.version, inputs.diffuse.version, inputs.hdr.version});
}

Result SubsurfaceSystem::BindRetained(RenderGraphBuilder &graph, const SubsurfaceInputs &inputs,
    SubsurfaceConstants constants, const SubsurfaceLayout &layout, SubsurfaceOutputs &outputs)
{
    return RegisterFrame(graph, inputs, constants, outputs, nullptr, &layout);
}

Result SubsurfaceSystem::RegisterFrame(RenderGraphBuilder &graph, const SubsurfaceInputs &inputs,
    SubsurfaceConstants constants, SubsurfaceOutputs &outputs,
    SubsurfaceLayout *capture, const SubsurfaceLayout *retained)
{
    outputs = {};
    if (capture) *capture = {};
    auto storage = impl_->targets;
    if (!storage) return SssFailure(ErrorCode::InvalidState, "Prepare and commit subsurface targets before registration.");
    if (retained && !MatchesLayout(inputs, *retained))
        return SssFailure(ErrorCode::InvalidState, "Subsurface retained topology has changed.");
    if (capture) { capture->enabled = storage->enabled; capture->capacity = storage->capacity; }
    if (!storage->enabled)
    {
        if (capture) capture->valid = true;
        return Result::Success();
    }
    if (impl_->pending.graph || !storage->usable)
        return SssFailure(ErrorCode::InvalidState, "Subsurface submission is pending or its previous graph execution failed.");
    const auto active = inputs.render_active;
    if (!ValidExtent(active) || active.width > storage->capacity.width || active.height > storage->capacity.height ||
        inputs.frame_slot >= storage->frame_slot_count ||
        !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed))
        return SssFailure(ErrorCode::InvalidArgument, "Subsurface active extent or completed frame slot is invalid.");
    if (!FitsDepth(inputs.depth, active) || !FitsGuide(inputs.normal, active, DXGI_FORMAT_R16G16_SNORM) ||
        !FitsGuide(inputs.metadata, active, DXGI_FORMAT_R32G32_UINT) ||
        !FitsGuide(inputs.diffuse, active, kColorFormat) || !FitsGuide(inputs.hdr, active, kColorFormat) ||
        !FitsParameters(inputs.material_parameters))
        return SssFailure(ErrorCode::InvalidArgument, "Subsurface guide resource, version, format or active extent is invalid.");
    const std::array guides{inputs.depth.version, inputs.normal.version, inputs.metadata.version,
        inputs.material_parameters.version, inputs.diffuse.version, inputs.hdr.version};
    if (capture) capture->guides = guides;
    for (const auto &row : constants.inverse_view_projection.m)
        for (const auto value : row)
            if (!std::isfinite(value))
                return SssFailure(ErrorCode::InvalidArgument, "Subsurface reconstruction matrix contains a nonfinite value.");
    constants.render_size = {static_cast<float>(active.width), static_cast<float>(active.height),
        1.0f / static_cast<float>(active.width), 1.0f / static_cast<float>(active.height)};
    std::memcpy(storage->mapped + SIZE_T(inputs.frame_slot) * kConstantStride, &constants, sizeof(constants));
    storage->Srv(inputs.frame_slot, 0, inputs.depth.resource, DXGI_FORMAT_R32_FLOAT);
    storage->Srv(inputs.frame_slot, 1, inputs.normal.resource, DXGI_FORMAT_R16G16_SNORM);
    storage->Srv(inputs.frame_slot, 2, inputs.metadata.resource, DXGI_FORMAT_R32G32_UINT);
    storage->Srv(inputs.frame_slot, 3, inputs.diffuse.resource, kColorFormat);
    storage->Srv(inputs.frame_slot, 4, inputs.hdr.resource, kColorFormat);
    storage->Srv(inputs.frame_slot, 5, storage->horizontal.resource.Get(), kColorFormat);
    storage->Uav(inputs.frame_slot, 0, storage->horizontal.resource.Get());
    storage->Uav(inputs.frame_slot, 1, storage->corrected.resource.Get());

    Impl::Pending pending;
    pending.graph = &graph; pending.targets = storage; pending.slot = inputs.frame_slot;
    std::size_t import_index{};
    bool bound = true;
    Result binding_result = Result::Success();
    auto import = [&](AllocationResource &allocation, std::string_view name) {
        if (auto checked = allocation.EnsureNativeContract(storage->device.Get()); !checked)
        { binding_result = checked; return ResourceVersion{}; }
        const auto handle = retained ? retained->textures[import_index] :
            graph.ImportTexture({allocation.resource.Get(), allocation.graph_completion, allocation.graph_queue, {}, allocation.NativeContract()},
                allocation.graph_access, name);
        if (retained)
            bound &= graph.BindTexture(handle, {allocation.resource.Get(), allocation.graph_completion,
                allocation.graph_queue, {}, allocation.NativeContract()}, allocation.graph_access);
        if (capture) capture->textures[import_index] = handle;
        ++import_index;
        pending.imports.push_back({&allocation, handle});
        return graph.Version(handle);
    };
    auto horizontal = import(storage->horizontal, "Subsurface.HorizontalDiffuse");
    auto corrected = import(storage->corrected, "Subsurface.CorrectedHDR");
    if (!binding_result) return binding_result;
    if (!bound) return SssFailure(ErrorCode::InvalidState, "Subsurface retained texture binding failed.");
    const auto parameterAddress = inputs.material_parameters.resource->GetGPUVirtualAddress();
    auto execute = [storage, slot = inputs.frame_slot, active, parameterAddress](std::uint32_t pipeline) {
        return [storage, slot, active, parameterAddress, pipeline](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()};
            commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(storage->root.Get());
            commands->SetPipelineState(storage->pipelines[pipeline].Get());
            commands->SetComputeRootConstantBufferView(0, storage->constants.resource->GetGPUVirtualAddress() + UINT64(slot) * kConstantStride);
            commands->SetComputeRootDescriptorTable(1, storage->Gpu(slot, 0));
            commands->SetComputeRootDescriptorTable(2, storage->Gpu(slot, kSrvCount));
            commands->SetComputeRootShaderResourceView(3, parameterAddress);
            commands->Dispatch((active.width + 7) / 8, (active.height + 7) / 8, 1);
        };
    };
    auto readGuides = [&](PassBuilder &pass) {
        pass.Read(inputs.depth.version, Access::ComputeRead);
        pass.Read(inputs.normal.version, Access::ComputeRead);
        pass.Read(inputs.metadata.version, Access::ComputeRead);
        pass.Read(inputs.material_parameters.version, Access::ComputeRead);
        pass.Read(inputs.diffuse.version, Access::ComputeRead);
    };
    if (capture) capture->passes[0] = graph.PassCount();
    auto horizontalBinding = retained ? graph.BindPass(retained->passes[0], 60) :
        std::optional<PassBuilder>{graph.AddPass(60, "SubsurfaceHorizontal", QueueHint::Direct)};
    if (!horizontalBinding) return SssFailure(ErrorCode::InvalidState, "Subsurface retained horizontal pass is missing.");
    auto &horizontalPass = *horizontalBinding;
    if (retained) horizontal = retained->written[0];
    else
    {
        readGuides(horizontalPass);
        horizontal = horizontalPass.Write(horizontal, Access::UnorderedWrite);
    }
    if (capture) capture->written[0] = horizontal;
    horizontalPass.SetExecute(execute(0));
    if (capture) capture->passes[1] = graph.PassCount();
    auto verticalBinding = retained ? graph.BindPass(retained->passes[1], 61) :
        std::optional<PassBuilder>{graph.AddPass(61, "SubsurfaceVertical", QueueHint::Direct)};
    if (!verticalBinding) return SssFailure(ErrorCode::InvalidState, "Subsurface retained vertical pass is missing.");
    auto &verticalPass = *verticalBinding;
    if (retained) corrected = retained->written[1];
    else
    {
        readGuides(verticalPass);
        verticalPass.Read(horizontal, Access::ComputeRead);
        verticalPass.Read(inputs.hdr.version, Access::ComputeRead);
        corrected = verticalPass.Write(corrected, Access::UnorderedWrite);
    }
    if (capture) capture->written[1] = corrected;
    verticalPass.SetExecute(execute(1));
    outputs.corrected = corrected; outputs.corrected_resource = storage->corrected.resource.Get();
    outputs.corrected_srv = storage->export_descriptors->GetCPUDescriptorHandleForHeapStart();
    impl_->pending = std::move(pending);
    if (capture) capture->valid = true;
    return Result::Success();
}

Result SubsurfaceSystem::CompleteSubmission(const RenderGraphBuilder &graph,
    const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph)
    {
        if (impl_->targets && !impl_->targets->enabled) return Result::Success();
        return SssFailure(ErrorCode::InvalidState, "Subsurface has no registered submission to complete.");
    }
    if (pending.graph != &graph) return SssFailure(ErrorCode::InvalidArgument, "Subsurface completion refers to a different graph.");
    auto storage = pending.targets;
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
    // A failed graph may submit a prefix. Preserve target leases and fence
    // requirements, then require a drain/replacement instead of guessing states.
    if (!success) storage->usable = false;
    pending = {};
    return Result::Success();
}

Result SubsurfaceSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return SssFailure(ErrorCode::InvalidState, "Complete the pending subsurface submission before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return SssFailure(ErrorCode::InvalidState, "Subsurface reset requires completed GPU uses.");
    impl_->targets.reset();
    return Result::Success();
}
} // namespace hs
