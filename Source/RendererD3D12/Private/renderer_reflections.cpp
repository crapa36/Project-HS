#include "renderer_reflections.hpp"
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
constexpr std::uint32_t kSrvCount = SsrSrvCount, kUavCount = SsrUavCount, kDescriptorCount = kSrvCount + kUavCount;
constexpr std::uint32_t kConstantStride = 768;
constexpr std::array<std::string_view, 5> kShaderProducts{
    "render/shader/ssr_depth_init_cs.dxil", "render/shader/ssr_depth_reduce_cs.dxil",
    "render/shader/ssr_trace_cs.dxil", "render/shader/ssr_temporal_cs.dxil",
    "render/shader/ssr_spatial_cs.dxil"};

Result ReflectionFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_reflections", std::move(message));
}
Result NativeFailure(std::string_view operation, HRESULT error)
{
    return ReflectionFailure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool ValidQuality(GraphicsOptionalQuality quality)
{
    return quality == GraphicsOptionalQuality::Off || quality == GraphicsOptionalQuality::Low ||
        quality == GraphicsOptionalQuality::Medium || quality == GraphicsOptionalQuality::High ||
        quality == GraphicsOptionalQuality::Ultra;
}
ViewExtent TraceExtent(ViewExtent render, const render_data::QualityProfile &profile)
{
    return {std::max(1u, static_cast<UINT>(std::ceil(render.width * profile.ssr_axis_ratio))),
            std::max(1u, static_cast<UINT>(std::ceil(render.height * profile.ssr_axis_ratio)))};
}
DirectX::XMFLOAT4 SizeConstants(ViewExtent extent)
{
    return {float(extent.width), float(extent.height), 1.0f / float(extent.width), 1.0f / float(extent.height)};
}
bool FitsTexture(const RenderGraphBuilder &graph, const ScreenReflectionResource &input, ViewExtent active, DXGI_FORMAT format)
{
    if (!input.resource || !input.version.resource || input.version.resource.kind != ResourceKind::Texture ||
        graph.Resolve(input.version.resource) != input.resource) return false;
    const auto d = graph.ResourceDescription(input.version.resource);
    return d && d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d->DepthOrArraySize == 1 &&
        d->SampleDesc.Count == 1 && d->Width >= active.width && d->Height >= active.height && d->Format == format;
}
bool FitsDepth(const RenderGraphBuilder &graph, const ScreenReflectionResource &input, ViewExtent active)
{
    if (!input.resource || !input.version.resource || input.version.resource.kind != ResourceKind::Texture ||
        graph.Resolve(input.version.resource) != input.resource) return false;
    const auto d = graph.ResourceDescription(input.version.resource);
    return d && d->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d->DepthOrArraySize == 1 &&
        d->SampleDesc.Count == 1 && d->Width >= active.width && d->Height >= active.height &&
        (d->Format == DXGI_FORMAT_R32_TYPELESS || d->Format == DXGI_FORMAT_D32_FLOAT || d->Format == DXGI_FORMAT_R32_FLOAT);
}
bool FiniteMatrix(const DirectX::XMFLOAT4X4 &matrix)
{
    for (const auto &row : matrix.m)
        for (float value : row) if (!std::isfinite(value)) return false;
    return true;
}
Result CheckFormat(ID3D12Device *device, DXGI_FORMAT format, bool load)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    const auto native = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(native)) return NativeFailure("Check reflection format", native);
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) ||
        (load && !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)))
        return ReflectionFailure(ErrorCode::TaskFailed, "Reflection format lacks required texture/UAV load/store support.");
    return Result::Success();
}
Result CreateTexture(D3D12MA::Allocator *allocator, ViewExtent extent, DXGI_FORMAT format, AllocationResource &target)
{
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = extent.width; d.Height = extent.height;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.Format = format; d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto native = allocator->CreateResource(&allocation, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
        &target.allocation, IID_PPV_ARGS(target.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create reflection texture", native);
    target.graph_access = Access::Common;
    return Result::Success();
}
} // namespace

struct ScreenReflectionsTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, kShaderProducts.size()> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors, export_descriptors;
    AllocationResource constants, hierarchy, raw, composed;
    std::array<AllocationResource, 2> history, validation_history;
    std::array<DirectX::XMUINT4, 16> hierarchy_levels{};
    std::uint32_t hierarchy_count{}, hierarchy_elements{};
    ViewExtent render_capacity, trace_capacity, previous_active;
    DirectX::XMFLOAT4 previous_depth_parameters{};
    DirectX::XMFLOAT2 previous_jitter{};
    GraphicsOptionalQuality quality{};
    render_data::QualityProfile profile;
    std::byte *mapped{};
    std::uint32_t frame_slot_count{}, descriptor_stride{}, read_index{};
    std::uint64_t bytes{}, previous_frame{};
    bool history_valid{}, usable{true};
    std::vector<CompletionSet> slot_completion;
    CompletionSet completion;
    ~ScreenReflectionsTargets()
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
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &view, Cpu(slot, offset));
    }
    void Uav(std::uint32_t slot, std::uint32_t offset, ID3D12Resource *resource, DXGI_FORMAT format)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = format; view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource, nullptr, &view, Cpu(slot, kSrvCount + offset));
    }
};
struct ScreenReflectionsSystem::Impl
{
    std::shared_ptr<ScreenReflectionsTargets> targets;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<ScreenReflectionsTargets> targets;
        std::vector<Import> imports;
        ViewExtent active;
        DirectX::XMFLOAT4 depth_parameters{};
        DirectX::XMFLOAT2 jitter{};
        std::uint32_t slot{}, write_index{};
        std::uint64_t frame{};
    } pending;
};
ScreenReflectionsSystem::ScreenReflectionsSystem() : impl_(std::make_unique<Impl>()) {}
ScreenReflectionsSystem::~ScreenReflectionsSystem() = default;
std::uint64_t ScreenReflectionsCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t ScreenReflectionsSystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }
#if defined(HS_DEVELOPMENT_TOOLS)
std::array<const AllocationResource *, 3> ScreenReflectionsSystem::DiagnosticResources(
    std::uint64_t frame_index) const noexcept
{
    const auto &storage = impl_->targets;
    if (!storage || !storage->history_valid || storage->previous_frame != frame_index) return {};
    return {&storage->raw, &storage->history[storage->read_index],
            &storage->validation_history[storage->read_index]};
}
#endif

Result ScreenReflectionsSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ViewExtent capacity, GraphicsOptionalQuality quality, const render_data::QualityProfile &profile, std::uint32_t frameSlots,
    const ShaderReader &readShader, ScreenReflectionsCandidate &candidate) const
{
    if (!device || !allocator || !ValidExtent(capacity) || !ValidQuality(quality) || !frameSlots ||
        frameSlots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / kDescriptorCount ||
        (quality != GraphicsOptionalQuality::Off && !readShader))
        return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection device, capacity, quality, frame slots or shader reader.");
    auto next = std::make_shared<ScreenReflectionsTargets>();
    next->device = device; next->allocator = allocator; next->render_capacity = capacity;
    next->trace_capacity = TraceExtent(capacity, profile); next->quality = quality; next->profile = profile;
    next->frame_slot_count = frameSlots; next->slot_completion.resize(frameSlots);
    if (quality == GraphicsOptionalQuality::Off)
    {
        candidate.targets_ = std::move(next);
        return Result::Success();
    }
    if (auto result = CheckFormat(device, DXGI_FORMAT_R32G32B32A32_FLOAT, true); !result) return result;
    if (auto result = CheckFormat(device, DXGI_FORMAT_R32G32B32A32_UINT, true); !result) return result;
    if (auto result = CheckFormat(device, DXGI_FORMAT_R16G16B16A16_FLOAT, false); !result) return result;
    if (auto result = CreateTexture(allocator, next->trace_capacity, DXGI_FORMAT_R32G32B32A32_FLOAT, next->raw); !result) return result;
    if (auto result = CreateTexture(allocator, capacity, DXGI_FORMAT_R16G16B16A16_FLOAT, next->composed); !result) return result;
    for (std::uint32_t index = 0; index < 2; ++index)
    {
        if (auto result = CreateTexture(allocator, next->trace_capacity, DXGI_FORMAT_R32G32B32A32_FLOAT, next->history[index]); !result) return result;
        if (auto result = CreateTexture(allocator, next->trace_capacity, DXGI_FORMAT_R32G32B32A32_UINT, next->validation_history[index]); !result) return result;
    }
    auto levelExtent = capacity;
    do
    {
        if (next->hierarchy_count == next->hierarchy_levels.size())
            return ReflectionFailure(ErrorCode::InvalidArgument, "Reflection hierarchy capacity exceeds its level ABI.");
        next->hierarchy_levels[next->hierarchy_count++] = {levelExtent.width, levelExtent.height, next->hierarchy_elements, 0};
        next->hierarchy_elements += levelExtent.width * levelExtent.height;
        if (levelExtent.width == 1 && levelExtent.height == 1) break;
        levelExtent = {(levelExtent.width + 1) / 2, (levelExtent.height + 1) / 2};
    } while (true);
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = UINT64(next->hierarchy_elements) * sizeof(float);
    buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto native = allocator->CreateResource(&allocation, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr,
        &next->hierarchy.allocation, IID_PPV_ARGS(next->hierarchy.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create reflection closest-depth hierarchy", native);
    next->hierarchy.graph_access = Access::Common;

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, SsrSrvDepth, kSsrSpace, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, SsrUavRaw, kSsrSpace, 0};
    D3D12_ROOT_PARAMETER parameters[SsrRootCount]{};
    parameters[SsrRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[SsrRootConstants].Descriptor = {kSsrConstantRegister, kSsrSpace};
    parameters[SsrRootSrvs].ParameterType = parameters[SsrRootUavs].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[SsrRootSrvs].DescriptorTable = {1, &ranges[0]}; parameters[SsrRootUavs].DescriptorTable = {1, &ranges[1]};
    parameters[SsrRootPass].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[SsrRootPass].Constants = {kSsrPassRegister, kSsrSpace, SsrPassCount};
    parameters[SsrRootFrame].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[SsrRootFrame].Descriptor = {0, 0};
    parameters[SsrRootMaterial].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[SsrRootMaterial].Descriptor = {0, 8};
    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = UINT(std::size(parameters)); rootDescription.pParameters = parameters;
    ComPtr<ID3DBlob> signature, diagnostics;
    native = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics);
    if (FAILED(native))
    {
        auto failure = NativeFailure("Serialize reflection root signature", native);
        if (diagnostics && diagnostics->GetBufferSize())
            return ReflectionFailure(ErrorCode::TaskFailed, std::string(failure.Message()) + " " +
                std::string(static_cast<const char *>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()));
        return failure;
    }
    native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&next->root));
    if (FAILED(native)) return NativeFailure("Create reflection root signature", native);
    for (std::uint32_t index = 0; index < kShaderProducts.size(); ++index)
    {
        std::vector<std::byte> bytecode;
        if (auto result = readShader(kShaderProducts[index], bytecode); !result) return result;
        if (bytecode.empty()) return ReflectionFailure(ErrorCode::TaskFailed, "Reflection shader product is empty: " + std::string(kShaderProducts[index]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = next->root.Get(); pipeline.CS = {bytecode.data(), bytecode.size()};
        native = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&next->pipelines[index]));
        if (FAILED(native)) return NativeFailure("Create reflection pipeline " + std::string(kShaderProducts[index]), native);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = frameSlots * kDescriptorCount;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->descriptors));
    if (FAILED(native)) return NativeFailure("Create reflection descriptors", native);
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors = 1; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->export_descriptors));
    if (FAILED(native)) return NativeFailure("Create reflection export descriptors", native);
    D3D12_SHADER_RESOURCE_VIEW_DESC exportView{};
    exportView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    exportView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; exportView.Texture2D.MipLevels = 1;
    auto exportHandle = next->export_descriptors->GetCPUDescriptorHandleForHeapStart();
    exportView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(next->composed.resource.Get(), &exportView, exportHandle);
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD; buffer.Width = UINT64(frameSlots) * kConstantStride;
    buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    native = allocator->CreateResource(&allocation, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        &next->constants.allocation, IID_PPV_ARGS(next->constants.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create reflection constants", native);
    D3D12_RANGE noRead{0, 0}; void *mapped{};
    native = next->constants.resource->Map(0, &noRead, &mapped);
    if (FAILED(native)) return NativeFailure("Map reflection constants", native);
    next->mapped = static_cast<std::byte *>(mapped);
    for (const auto *resource : {&next->constants, &next->hierarchy, &next->raw, &next->composed,
         &next->history[0], &next->history[1], &next->validation_history[0], &next->validation_history[1]})
        next->bytes += resource->allocation->GetSize();
    candidate.targets_ = std::move(next);
    return Result::Success();
}

Result ScreenReflectionsSystem::Commit(ScreenReflectionsCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return ReflectionFailure(ErrorCode::InvalidState, "Complete registered reflection submission before replacing targets.");
    if (!candidate.targets_) return ReflectionFailure(ErrorCode::InvalidArgument, "Reflection candidate was not prepared.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return ReflectionFailure(ErrorCode::InvalidState, "Old reflection targets remain in GPU use.");
    impl_->targets = std::move(candidate.targets_);
    return Result::Success();
}

Result ScreenReflectionsSystem::Register(RenderGraphBuilder &graph, const ScreenReflectionsInputs &inputs,
    ScreenReflectionsConstants constants, ScreenReflectionsOutputs &outputs, ScreenReflectionsLayout *layout)
{
    return RegisterFrame(graph, inputs, constants, outputs, layout, nullptr);
}
bool ScreenReflectionsSystem::MatchesShape(const ScreenReflectionsLayout &layout,
    bool reset_history, std::uint64_t frame_index, bool ambient_visibility,
    std::uint32_t sidecar_planes) const noexcept
{
    const auto &s=impl_->targets;
    if(!layout.valid||!s||layout.active!=(s->quality!=GraphicsOptionalQuality::Off))return false;
    return !layout.active||(layout.history==(s->history_valid&&!reset_history&&frame_index==s->previous_frame+1)&&
        layout.ambient==ambient_visibility&&layout.sidecars==sidecar_planes&&
        layout.render_capacity.width==s->render_capacity.width&&layout.render_capacity.height==s->render_capacity.height&&
        layout.trace_capacity.width==s->trace_capacity.width&&layout.trace_capacity.height==s->trace_capacity.height&&
        layout.hierarchy_elements==s->hierarchy_elements);
}
bool ScreenReflectionsSystem::MatchesLayout(const ScreenReflectionsInputs &inputs,const ScreenReflectionsLayout &layout)const noexcept
{
    return MatchesShape(layout, inputs.reset_history, inputs.frame_index,
        inputs.ambient_visibility_enabled, inputs.sidecar_plane_count) && (!layout.active ||
        layout.guides==ReflectionGuides(inputs));
}
Result ScreenReflectionsSystem::BindRetained(RenderGraphBuilder &graph, const ScreenReflectionsInputs &inputs,
    ScreenReflectionsConstants constants, const ScreenReflectionsLayout &layout, ScreenReflectionsOutputs &outputs)
{
    return RegisterFrame(graph, inputs, constants, outputs, nullptr, &layout);
}
Result ScreenReflectionsSystem::RegisterFrame(RenderGraphBuilder &graph, const ScreenReflectionsInputs &inputs,
    ScreenReflectionsConstants constants, ScreenReflectionsOutputs &outputs,
    ScreenReflectionsLayout *capture, const ScreenReflectionsLayout *retained)
{
    outputs = {};
    if (capture) *capture = {};
    auto storage = impl_->targets;
    if (!storage) return ReflectionFailure(ErrorCode::InvalidState, "Prepare and commit reflection targets before registration.");
    const bool active_path = storage->quality != GraphicsOptionalQuality::Off;
    const bool historyValid = storage->history_valid && !inputs.reset_history && inputs.frame_index == storage->previous_frame + 1;
    if (retained && !MatchesLayout(inputs,*retained))
        return ReflectionFailure(ErrorCode::InvalidState, "Reflection retained topology has changed.");
    if (capture)
    {
        capture->active = active_path; capture->history = historyValid;
        capture->ambient = inputs.ambient_visibility_enabled; capture->sidecars = inputs.sidecar_plane_count;
        capture->render_capacity=storage->render_capacity;capture->trace_capacity=storage->trace_capacity;
        capture->hierarchy_elements=storage->hierarchy_elements;
    }
    if (!active_path) { if (capture) capture->valid = true; return Result::Success(); }
    if (impl_->pending.graph || !storage->usable)
        return ReflectionFailure(ErrorCode::InvalidState, "Reflection submission is pending or its previous graph failed.");
    const auto active = inputs.render_active;
    if (!ValidExtent(active) || active.width > storage->render_capacity.width || active.height > storage->render_capacity.height ||
        inputs.frame_slot >= storage->frame_slot_count || !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed))
        return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection active extent or completed frame slot.");
    if (!FitsDepth(graph, inputs.depth, active) || !FitsTexture(graph, inputs.base, active, DXGI_FORMAT_R8G8B8A8_UNORM) ||
        !FitsTexture(graph, inputs.normal, active, DXGI_FORMAT_R16G16_SNORM) || !FitsTexture(graph, inputs.metadata, active, DXGI_FORMAT_R32_UINT) ||
        !FitsTexture(graph, inputs.material, active, DXGI_FORMAT_R8G8B8A8_UNORM) || !FitsTexture(graph, inputs.motion, active, DXGI_FORMAT_R16G16_FLOAT) ||
        !FitsTexture(graph, inputs.surface_history, active, DXGI_FORMAT_R32G32_FLOAT) ||
        !FitsTexture(graph, inputs.opaque_hdr, active, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !FitsTexture(graph, inputs.environment_specular, active, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        inputs.sidecar_plane_count > inputs.sidecars.size())
        return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection compact-surface or radiance input.");
    const auto guides = ReflectionGuides(inputs);
    if (capture) capture->guides = guides;
    for (std::uint32_t index = 0; index < inputs.sidecar_plane_count; ++index)
    {
        const auto &sidecar = inputs.sidecars[index];
        const auto desc = sidecar.version.resource ? graph.ResourceDescription(sidecar.version.resource) : std::nullopt;
        if (!desc || !sidecar.resource || sidecar.version.resource.kind != ResourceKind::Texture ||
            graph.Resolve(sidecar.version.resource) != sidecar.resource || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc->DepthOrArraySize != 1 || desc->SampleDesc.Count != 1 || desc->Width < active.width || desc->Height < active.height ||
            (desc->Format != DXGI_FORMAT_R32_UINT && desc->Format != DXGI_FORMAT_R32G32_UINT &&
                desc->Format != DXGI_FORMAT_R32G32B32A32_UINT))
            return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection sidecar input.");
    }
    if (inputs.ambient_visibility_enabled && !FitsTexture(graph, inputs.ambient_visibility, active, DXGI_FORMAT_R16_FLOAT))
        return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection ambient visibility input.");
    const auto parameterDescription = inputs.material_parameters.version.resource ?
        graph.ResourceDescription(inputs.material_parameters.version.resource) : std::nullopt;
    if (!inputs.material_parameters.resource || !inputs.material_parameters.version.resource ||
        inputs.material_parameters.version.resource.kind != ResourceKind::Buffer ||
        graph.Resolve(inputs.material_parameters.version.resource) != inputs.material_parameters.resource || !parameterDescription ||
        parameterDescription->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        parameterDescription->Width < sizeof(DirectX::XMUINT4) ||
        parameterDescription->Width / sizeof(DirectX::XMUINT4) > UINT32_MAX ||
        !inputs.frame_constants_address || inputs.frame_constants_address % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
        return ReflectionFailure(ErrorCode::InvalidArgument, "Reflection requires material parameter buffer and aligned live frame constants.");
    if (!FiniteMatrix(constants.inverse_view_projection) || !FiniteMatrix(constants.world_to_view) || !FiniteMatrix(constants.projection) ||
        !std::isfinite(constants.jitter_uv.x) || !std::isfinite(constants.jitter_uv.y) ||
        std::abs(constants.jitter_uv.x * float(active.width)) > 0.5f || std::abs(constants.jitter_uv.y * float(active.height)) > 0.5f ||
        !std::isfinite(constants.trace_parameters.x) || constants.trace_parameters.x <= 0.0f ||
        !std::isfinite(constants.trace_parameters.y) || constants.trace_parameters.y <= 0.0f ||
        !std::isfinite(constants.trace_parameters.z) || constants.trace_parameters.z < 0.0f ||
        !std::isfinite(constants.trace_parameters.w) || constants.trace_parameters.w <= 0.0f || constants.trace_parameters.w > 1.0f ||
        !std::isfinite(constants.filter_parameters.x) || constants.filter_parameters.x <= 0.0f ||
        !std::isfinite(constants.filter_parameters.y) || constants.filter_parameters.y < -1.0f || constants.filter_parameters.y > 1.0f ||
        !std::isfinite(constants.filter_parameters.z) || constants.filter_parameters.z < 0.0f || constants.filter_parameters.z > 1.0f ||
        !std::isfinite(constants.filter_parameters.w) || constants.filter_parameters.w < 0.0f || constants.filter_parameters.w > 1.0f)
        return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection transforms, jitter or physical tracing parameters.");
    const auto traceActive = TraceExtent(active, storage->profile);
    const auto writeIndex = 1u - storage->read_index;
    constants.render_size = SizeConstants(active); constants.trace_size = SizeConstants(traceActive);
    constants.previous_trace_size = SizeConstants(historyValid ? storage->previous_active : traceActive);
    constants.previous_depth_parameters = historyValid ? storage->previous_depth_parameters : DirectX::XMFLOAT4{};
    constants.jitter_uv.z = historyValid ? storage->previous_jitter.x : constants.jitter_uv.x;
    constants.jitter_uv.w = historyValid ? storage->previous_jitter.y : constants.jitter_uv.y;
    constants.budget = DirectX::XMUINT4{storage->profile.ssr_samples, storage->profile.ssr_max_steps,
        storage->profile.ssr_refine_steps, storage->hierarchy_count - 1};
    constants.frame_info = {historyValid ? 1u : 0u, std::uint32_t(inputs.frame_index), storage->hierarchy_count, 1u};
    constants.hierarchy_levels = storage->hierarchy_levels;
    std::memcpy(storage->mapped + SIZE_T(inputs.frame_slot) * kConstantStride, &constants, sizeof(constants));
    D3D12_SHADER_RESOURCE_VIEW_DESC parameterView{};
    parameterView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    parameterView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    parameterView.Buffer.NumElements = UINT(parameterDescription->Width / sizeof(DirectX::XMUINT4));
    parameterView.Buffer.StructureByteStride = sizeof(DirectX::XMUINT4);
    const auto sidecar_resource = [&](std::size_t index) -> ID3D12Resource * {
        return index < inputs.sidecar_plane_count ? inputs.sidecars[index].resource : nullptr;
    };
    const auto sidecar_format = [&](std::size_t index) {
        const auto description = index < inputs.sidecar_plane_count
            ? graph.ResourceDescription(inputs.sidecars[index].version.resource) : std::nullopt;
        return description ? description->Format : DXGI_FORMAT_R32G32B32A32_UINT;
    };
#define HS_SSR_WRITE_SRV_Texture(name,resource,format) storage->Srv(inputs.frame_slot, SsrSrv##name, resource, format);
#define HS_SSR_WRITE_SRV_Buffer(name,resource,format) storage->device->CreateShaderResourceView(resource, &parameterView, storage->Cpu(inputs.frame_slot, SsrSrv##name));
#define HS_SSR_WRITE_SRV(kind,name,slot,type,shader,version,resource,format) HS_SSR_WRITE_SRV_##kind(name,resource,format)
    HS_SSR_SRVS(HS_SSR_WRITE_SRV)
#undef HS_SSR_WRITE_SRV
#undef HS_SSR_WRITE_SRV_Texture
#undef HS_SSR_WRITE_SRV_Buffer
    D3D12_UNORDERED_ACCESS_VIEW_DESC hierarchyView{};
    hierarchyView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    hierarchyView.Buffer.NumElements = storage->hierarchy_elements; hierarchyView.Buffer.StructureByteStride = sizeof(float);
#define HS_SSR_WRITE_UAV_Texture(name,resource,format) storage->Uav(inputs.frame_slot, SsrUav##name, resource, format);
#define HS_SSR_WRITE_UAV_Buffer(name,resource,format) storage->device->CreateUnorderedAccessView(resource, nullptr, &hierarchyView, storage->Cpu(inputs.frame_slot, kSrvCount + SsrUav##name));
#define HS_SSR_WRITE_UAV(kind,name,slot,type,shader,version,resource,format) HS_SSR_WRITE_UAV_##kind(name,resource,format)
    HS_SSR_UAVS(HS_SSR_WRITE_UAV)
#undef HS_SSR_WRITE_UAV
#undef HS_SSR_WRITE_UAV_Texture
#undef HS_SSR_WRITE_UAV_Buffer
    Impl::Pending pending;
    pending.graph = &graph; pending.targets = storage; pending.active = traceActive;
    pending.slot = inputs.frame_slot; pending.write_index = writeIndex; pending.frame = inputs.frame_index;
    pending.jitter = {constants.jitter_uv.x, constants.jitter_uv.y};
    pending.depth_parameters = {constants.projection._33, constants.projection._34, constants.projection._43, constants.projection._44};
    std::size_t import_index{};
    Result binding_result = Result::Success();
    auto import = [&](AllocationResource &resource, std::string_view name) {
        if (auto checked = resource.EnsureNativeContract(storage->device.Get()); !checked)
        { binding_result = checked; return ResourceVersion{}; }
        const auto &description = resource.NativeContract()->description;
        ResourceHandle handle = retained ? retained->resources[import_index] : ResourceHandle{};
        if (retained)
        {
            const bool bound = description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                ? graph.BindBuffer(handle, {resource.resource.Get(), resource.graph_completion, resource.graph_queue, {}, resource.NativeContract()}, resource.graph_access)
                : graph.BindTexture(handle, {resource.resource.Get(), resource.graph_completion, resource.graph_queue, {}, resource.NativeContract()}, resource.graph_access);
            if (!bound) binding_result = ReflectionFailure(ErrorCode::InvalidState, "Reflection retained resource binding failed.");
        }
        else if (description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            handle = graph.ImportBuffer({resource.resource.Get(), resource.graph_completion, resource.graph_queue, {}, resource.NativeContract()}, resource.graph_access, name);
        else handle = graph.ImportTexture({resource.resource.Get(), resource.graph_completion, resource.graph_queue, {}, resource.NativeContract()}, resource.graph_access, name);
        if (capture) capture->resources[import_index] = handle;
        ++import_index;
        pending.imports.push_back({&resource, handle});
        return graph.Version(handle);
    };
    auto hierarchy = import(storage->hierarchy, "SSR.ClosestDepthHierarchy");
    auto raw = import(storage->raw, "SSR.Trace");
    auto previous = import(storage->history[storage->read_index], "SSR.PreviousRadiance");
    auto previousValidation = import(storage->validation_history[storage->read_index], "SSR.PreviousValidation");
    auto current = import(storage->history[writeIndex], "SSR.CurrentRadiance");
    auto validation = import(storage->validation_history[writeIndex], "SSR.CurrentValidation");
    auto composed = import(storage->composed, "SSR.ComposedOpaqueHDR");
    if (!binding_result) return binding_result;
    const std::array<ResourceVersion *, SsrUavCount> graphUavs{{
#define HS_SSR_UAV_VERSION(kind,name,slot,type,shader,version,resource,format) &version,
        HS_SSR_UAVS(HS_SSR_UAV_VERSION)
#undef HS_SSR_UAV_VERSION
    }};
    auto execute = [storage, inputs, hierarchyHandle = hierarchy.resource](std::uint32_t pipeline, ViewExtent extent) {
        return [storage, inputs, hierarchyHandle, pipeline, extent](RenderPassContext &context) -> Result {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()}; commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(storage->root.Get());
            commands->SetComputeRootConstantBufferView(SsrRootConstants, storage->constants.resource->GetGPUVirtualAddress() + UINT64(inputs.frame_slot) * kConstantStride);
            commands->SetComputeRootDescriptorTable(SsrRootSrvs, storage->Gpu(inputs.frame_slot, 0));
            commands->SetComputeRootDescriptorTable(SsrRootUavs, storage->Gpu(inputs.frame_slot, kSrvCount));
            commands->SetComputeRootConstantBufferView(SsrRootFrame, inputs.frame_constants_address);
            commands->SetComputeRootShaderResourceView(SsrRootMaterial, inputs.material_parameters.resource->GetGPUVirtualAddress());
            const SsrPassConstants pass{0, inputs.ambient_visibility_enabled ? 1u : 0u, inputs.sidecar_plane_count, 0};
            commands->SetComputeRoot32BitConstants(SsrRootPass, SsrPassCount, &pass, 0);
            if (pipeline == 2)
            {
                // The reflection pyramid is closest-depth MAX in reversed Z,
                // rebuilt after the complete opaque surface, unlike culling HZB.
                commands->SetPipelineState(storage->pipelines[0].Get());
                commands->Dispatch((inputs.render_active.width + 7) / 8, (inputs.render_active.height + 7) / 8, 1);
                context.UavBarrier(hierarchyHandle);
                // Each group reduces three levels without cross-group reads of its new outputs.
                for (std::uint32_t level = 1; level < storage->hierarchy_count; level += 3)
                {
                    const auto scale = 1u << level;
                    const ViewExtent levelActive{(inputs.render_active.width + scale - 1) / scale,
                                                 (inputs.render_active.height + scale - 1) / scale};
                    commands->SetComputeRoot32BitConstant(SsrRootPass, level, SsrPassHierarchyLevel);
                    commands->SetPipelineState(storage->pipelines[1].Get());
                    commands->Dispatch((levelActive.width + 7) / 8, (levelActive.height + 7) / 8, 1);
                    context.UavBarrier(hierarchyHandle);
                }
                commands->SetComputeRoot32BitConstant(SsrRootPass, 0, SsrPassHierarchyLevel);
            }
            commands->SetPipelineState(storage->pipelines[pipeline].Get());
            commands->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            return Result::Success();
        };
    };
    auto readSurface = [&](PassBuilder &pass) {
#define HS_SSR_READ(name) pass.Read(guides[SsrSrv##name], Access::ComputeRead);
        HS_SSR_SURFACE_READS(HS_SSR_READ)
#undef HS_SSR_READ
        for (std::uint32_t index = 0; index < inputs.sidecar_plane_count; ++index)
            pass.Read(guides[SsrSrvSidecar0 + index], Access::ComputeRead);
    };
    const auto pass_binding = [&](std::size_t index, std::uint32_t id, std::string_view name) -> std::optional<PassBuilder> {
        if (retained) return graph.BindPass(retained->passes[index], id);
        if (capture) capture->passes[index] = graph.PassCount();
        return graph.AddPass(id, name, QueueHint::Direct);
    };
    auto traceBinding = pass_binding(0,70,"SSRTrace");
    if (!traceBinding) return ReflectionFailure(ErrorCode::InvalidState, "Reflection retained trace pass is missing.");
    auto &trace = *traceBinding;
    if (retained) { hierarchy = retained->hierarchy; raw = retained->raw; validation = retained->validation; }
    else
    {
#define HS_SSR_READ(name) trace.Read(guides[SsrSrv##name], Access::ComputeRead);
#define HS_SSR_WRITE(name) *graphUavs[SsrUav##name] = trace.Write(*graphUavs[SsrUav##name], Access::UnorderedWrite);
        readSurface(trace); HS_SSR_TRACE_READS(HS_SSR_READ)
        if (inputs.ambient_visibility_enabled) { HS_SSR_READ(Ambient) }
        HS_SSR_TRACE_WRITES(HS_SSR_WRITE)
#undef HS_SSR_READ
#undef HS_SSR_WRITE
    }
    trace.SetExecuteChecked(execute(2, traceActive));
    auto temporalBinding = pass_binding(1,71,"SSRTemporal");
    if (!temporalBinding) return ReflectionFailure(ErrorCode::InvalidState, "Reflection retained temporal pass is missing.");
    auto &temporal = *temporalBinding;
    if (retained) current = retained->current;
    else
    {
#define HS_SSR_READ(name) temporal.Read(guides[SsrSrv##name], Access::ComputeRead);
#define HS_SSR_UAV_READ(name) temporal.Read(*graphUavs[SsrUav##name], Access::UnorderedRead);
#define HS_SSR_WRITE(name) *graphUavs[SsrUav##name] = temporal.Write(*graphUavs[SsrUav##name], Access::UnorderedWrite);
        readSurface(temporal); HS_SSR_TEMPORAL_READS(HS_SSR_READ,HS_SSR_UAV_READ)
        if (historyValid) { HS_SSR_HISTORY_READS(HS_SSR_UAV_READ) }
        HS_SSR_TEMPORAL_WRITES(HS_SSR_WRITE)
#undef HS_SSR_READ
#undef HS_SSR_UAV_READ
#undef HS_SSR_WRITE
    }
    temporal.SetExecuteChecked(execute(3, traceActive));
    auto spatialBinding = pass_binding(2,72,"SSRSpatialCompose");
    if (!spatialBinding) return ReflectionFailure(ErrorCode::InvalidState, "Reflection retained spatial pass is missing.");
    auto &spatial = *spatialBinding;
    if (retained) composed = retained->composed;
    else
    {
#define HS_SSR_READ(name) spatial.Read(guides[SsrSrv##name], Access::ComputeRead);
#define HS_SSR_UAV_READ(name) spatial.Read(*graphUavs[SsrUav##name], Access::UnorderedRead);
#define HS_SSR_WRITE(name) *graphUavs[SsrUav##name] = spatial.Write(*graphUavs[SsrUav##name], Access::UnorderedWrite);
        readSurface(spatial); HS_SSR_SPATIAL_READS(HS_SSR_READ,HS_SSR_UAV_READ)
        HS_SSR_SPATIAL_WRITES(HS_SSR_WRITE)
#undef HS_SSR_READ
#undef HS_SSR_UAV_READ
#undef HS_SSR_WRITE
        graph.SetFinalAccess(current, Access::UnorderedRead); graph.SetFinalAccess(validation, Access::UnorderedRead);
        graph.SetFinalAccess(composed, Access::ComputeRead);
    }
    spatial.SetExecuteChecked(execute(4, active));
    outputs = {composed, storage->composed.resource.Get(),
               storage->export_descriptors->GetCPUDescriptorHandleForHeapStart()};
    impl_->pending = std::move(pending);
    if (capture)
    {
        capture->hierarchy = hierarchy; capture->raw = raw; capture->current = current;
        capture->validation = validation; capture->composed = composed; capture->valid = true;
    }
    return Result::Success();
}

Result ScreenReflectionsSystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph)
    {
        if (impl_->targets && impl_->targets->quality == GraphicsOptionalQuality::Off) return Result::Success();
        return ReflectionFailure(ErrorCode::InvalidState, "Reflection completion has no registered graph.");
    }
    if (pending.graph != &graph) return ReflectionFailure(ErrorCode::InvalidArgument, "Reflection completion refers to a different graph.");
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
    if (success)
    {
        storage->read_index = pending.write_index; storage->previous_active = pending.active;
        storage->previous_depth_parameters = pending.depth_parameters; storage->previous_jitter = pending.jitter;
        storage->previous_frame = pending.frame; storage->history_valid = true;
    }
    else
    {
        storage->history_valid = false; storage->usable = false;
    }
    pending = {};
    return Result::Success();
}
void ScreenReflectionsSystem::ResetHistory() noexcept
{
    if (impl_->targets) impl_->targets->history_valid = false;
}
Result ScreenReflectionsSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return ReflectionFailure(ErrorCode::InvalidState, "Complete pending reflections before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return ReflectionFailure(ErrorCode::InvalidState, "Reflection reset requires completed GPU uses.");
    impl_->targets.reset();
    return Result::Success();
}
} // namespace hs
