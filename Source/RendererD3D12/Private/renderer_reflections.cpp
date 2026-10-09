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
constexpr std::uint32_t kSrvCount = 14, kUavCount = 7, kDescriptorCount = 21;
constexpr std::uint32_t kConstantStride = 768;
constexpr std::array<std::string_view, 6> kShaderProducts{
    "render/shader/ssr_depth_init_cs.dxil", "render/shader/ssr_depth_reduce_cs.dxil",
    "render/shader/ssr_trace_cs.dxil", "render/shader/ssr_temporal_cs.dxil",
    "render/shader/ssr_spatial_cs.dxil", "render/shader/ssr_compose_cs.dxil"};

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
bool FitsTexture(const ScreenReflectionResource &input, ViewExtent active, DXGI_FORMAT format)
{
    if (!input.resource || !input.version.resource || input.version.resource.kind != ResourceKind::Texture) return false;
    const auto d = input.resource->GetDesc();
    return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize == 1 &&
        d.SampleDesc.Count == 1 && d.Width >= active.width && d.Height >= active.height && d.Format == format;
}
bool FitsDepth(const ScreenReflectionResource &input, ViewExtent active)
{
    return FitsTexture(input, active, DXGI_FORMAT_R32_TYPELESS) || FitsTexture(input, active, DXGI_FORMAT_D32_FLOAT) ||
        FitsTexture(input, active, DXGI_FORMAT_R32_FLOAT);
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
    std::array<ComPtr<ID3D12PipelineState>, 6> pipelines;
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
    if (auto result = CreateTexture(allocator, capacity, DXGI_FORMAT_R32G32B32A32_FLOAT, next->raw); !result) return result;
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
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 16, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0, 16, 0};
    D3D12_ROOT_PARAMETER parameters[6]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[0].Descriptor = {0, 16};
    parameters[1].ParameterType = parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[0]}; parameters[2].DescriptorTable = {1, &ranges[1]};
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[3].Constants = {1, 16, 4};
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[4].Descriptor = {0, 0};
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[5].Descriptor = {0, 8};
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
    heap.NumDescriptors = 2; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->export_descriptors));
    if (FAILED(native)) return NativeFailure("Create reflection export descriptors", native);
    D3D12_SHADER_RESOURCE_VIEW_DESC exportView{};
    exportView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    exportView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; exportView.Texture2D.MipLevels = 1;
    auto exportHandle = next->export_descriptors->GetCPUDescriptorHandleForHeapStart();
    exportView.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    device->CreateShaderResourceView(next->raw.resource.Get(), &exportView, exportHandle);
    exportHandle.ptr += next->descriptor_stride; exportView.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
    ScreenReflectionsConstants constants, ScreenReflectionsOutputs &outputs)
{
    outputs = {};
    auto storage = impl_->targets;
    if (!storage) return ReflectionFailure(ErrorCode::InvalidState, "Prepare and commit reflection targets before registration.");
    if (storage->quality == GraphicsOptionalQuality::Off) return Result::Success();
    if (impl_->pending.graph || !storage->usable)
        return ReflectionFailure(ErrorCode::InvalidState, "Reflection submission is pending or its previous graph failed.");
    const auto active = inputs.render_active;
    if (!ValidExtent(active) || active.width > storage->render_capacity.width || active.height > storage->render_capacity.height ||
        inputs.frame_slot >= storage->frame_slot_count || !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed))
        return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection active extent or completed frame slot.");
    if (!FitsDepth(inputs.depth, active) || !FitsTexture(inputs.base, active, DXGI_FORMAT_R8G8B8A8_UNORM) ||
        !FitsTexture(inputs.normal, active, DXGI_FORMAT_R16G16_SNORM) || !FitsTexture(inputs.metadata, active, DXGI_FORMAT_R32G32_UINT) ||
        !FitsTexture(inputs.material, active, DXGI_FORMAT_R8G8B8A8_UNORM) || !FitsTexture(inputs.motion, active, DXGI_FORMAT_R16G16_FLOAT) ||
        !FitsTexture(inputs.surface_history, active, DXGI_FORMAT_R32G32_FLOAT) ||
        !FitsTexture(inputs.opaque_hdr, active, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !FitsTexture(inputs.environment_specular, active, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        inputs.sidecar_plane_count > inputs.sidecars.size())
        return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection compact-surface or radiance input.");
    for (std::uint32_t index = 0; index < inputs.sidecar_plane_count; ++index)
        if (!FitsTexture(inputs.sidecars[index], active, DXGI_FORMAT_R32_UINT) &&
            !FitsTexture(inputs.sidecars[index], active, DXGI_FORMAT_R32G32_UINT) &&
            !FitsTexture(inputs.sidecars[index], active, DXGI_FORMAT_R32G32B32A32_UINT))
            return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection sidecar input.");
    if (inputs.ambient_visibility_enabled && !FitsTexture(inputs.ambient_visibility, active, DXGI_FORMAT_R16_FLOAT))
        return ReflectionFailure(ErrorCode::InvalidArgument, "Invalid reflection ambient visibility input.");
    if (!inputs.material_parameters.resource || !inputs.material_parameters.version.resource ||
        inputs.material_parameters.version.resource.kind != ResourceKind::Buffer ||
        inputs.material_parameters.resource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        inputs.material_parameters.resource->GetDesc().Width < sizeof(DirectX::XMUINT4) ||
        inputs.material_parameters.resource->GetDesc().Width / sizeof(DirectX::XMUINT4) > UINT32_MAX ||
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
    const bool historyValid = storage->history_valid && !inputs.reset_history && inputs.frame_index == storage->previous_frame + 1;
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
    storage->Srv(inputs.frame_slot, 0, inputs.depth.resource, DXGI_FORMAT_R32_FLOAT);
    storage->Srv(inputs.frame_slot, 1, inputs.base.resource, DXGI_FORMAT_R8G8B8A8_UNORM);
    storage->Srv(inputs.frame_slot, 2, inputs.normal.resource, DXGI_FORMAT_R16G16_SNORM);
    storage->Srv(inputs.frame_slot, 3, inputs.metadata.resource, DXGI_FORMAT_R32G32_UINT);
    storage->Srv(inputs.frame_slot, 4, inputs.material.resource, DXGI_FORMAT_R8G8B8A8_UNORM);
    storage->Srv(inputs.frame_slot, 5, inputs.motion.resource, DXGI_FORMAT_R16G16_FLOAT);
    storage->Srv(inputs.frame_slot, 6, inputs.surface_history.resource, DXGI_FORMAT_R32G32_FLOAT);
    storage->Srv(inputs.frame_slot, 7, inputs.opaque_hdr.resource, DXGI_FORMAT_R16G16B16A16_FLOAT);
    storage->Srv(inputs.frame_slot, 8, inputs.environment_specular.resource, DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_SHADER_RESOURCE_VIEW_DESC parameterView{};
    parameterView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    parameterView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    parameterView.Buffer.NumElements = UINT(inputs.material_parameters.resource->GetDesc().Width / sizeof(DirectX::XMUINT4));
    parameterView.Buffer.StructureByteStride = sizeof(DirectX::XMUINT4);
    storage->device->CreateShaderResourceView(inputs.material_parameters.resource, &parameterView, storage->Cpu(inputs.frame_slot, 9));
    for (std::uint32_t index = 0; index < 2; ++index)
        storage->Srv(inputs.frame_slot, 10 + index, index < inputs.sidecar_plane_count ? inputs.sidecars[index].resource : nullptr,
            index < inputs.sidecar_plane_count ? inputs.sidecars[index].resource->GetDesc().Format : DXGI_FORMAT_R32G32B32A32_UINT);
    storage->Srv(inputs.frame_slot, 12, inputs.ambient_visibility_enabled ? inputs.ambient_visibility.resource : nullptr, DXGI_FORMAT_R16_FLOAT);
    storage->Srv(inputs.frame_slot, 13, storage->raw.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Uav(inputs.frame_slot, 0, storage->raw.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Uav(inputs.frame_slot, 1, storage->history[storage->read_index].resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Uav(inputs.frame_slot, 2, storage->history[writeIndex].resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
    storage->Uav(inputs.frame_slot, 3, storage->validation_history[storage->read_index].resource.Get(), DXGI_FORMAT_R32G32B32A32_UINT);
    storage->Uav(inputs.frame_slot, 4, storage->validation_history[writeIndex].resource.Get(), DXGI_FORMAT_R32G32B32A32_UINT);
    storage->Uav(inputs.frame_slot, 5, storage->composed.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_UNORDERED_ACCESS_VIEW_DESC hierarchyView{};
    hierarchyView.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    hierarchyView.Buffer.NumElements = storage->hierarchy_elements; hierarchyView.Buffer.StructureByteStride = sizeof(float);
    storage->device->CreateUnorderedAccessView(storage->hierarchy.resource.Get(), nullptr, &hierarchyView,
                                             storage->Cpu(inputs.frame_slot, kSrvCount + 6));
    Impl::Pending pending;
    pending.graph = &graph; pending.targets = storage; pending.active = traceActive;
    pending.slot = inputs.frame_slot; pending.write_index = writeIndex; pending.frame = inputs.frame_index;
    pending.jitter = {constants.jitter_uv.x, constants.jitter_uv.y};
    pending.depth_parameters = {constants.projection._33, constants.projection._34, constants.projection._43, constants.projection._44};
    auto import = [&](AllocationResource &resource, std::string_view name) {
        const auto description = resource.resource->GetDesc();
        ResourceHandle handle;
        if (description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
            handle = graph.ImportBuffer({resource.resource.Get(), resource.graph_completion, resource.graph_queue}, resource.graph_access, name);
        else
            handle = graph.ImportTexture({resource.resource.Get(), resource.graph_completion, resource.graph_queue}, resource.graph_access, name);
        pending.imports.push_back({&resource, handle});
        return graph.Version(handle);
    };
    auto hierarchy = import(storage->hierarchy, "SSR.ClosestDepthHierarchy");
    auto raw = import(storage->raw, "SSR.TraceAndResolved");
    auto previous = import(storage->history[storage->read_index], "SSR.PreviousRadiance");
    auto previousValidation = import(storage->validation_history[storage->read_index], "SSR.PreviousValidation");
    auto current = import(storage->history[writeIndex], "SSR.CurrentRadiance");
    auto validation = import(storage->validation_history[writeIndex], "SSR.CurrentValidation");
    auto composed = import(storage->composed, "SSR.ComposedOpaqueHDR");
    auto execute = [storage, inputs, hierarchyHandle = hierarchy.resource](std::uint32_t pipeline, ViewExtent extent) {
        return [storage, inputs, hierarchyHandle, pipeline, extent](RenderPassContext &context) -> Result {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()}; commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(storage->root.Get());
            commands->SetComputeRootConstantBufferView(0, storage->constants.resource->GetGPUVirtualAddress() + UINT64(inputs.frame_slot) * kConstantStride);
            commands->SetComputeRootDescriptorTable(1, storage->Gpu(inputs.frame_slot, 0));
            commands->SetComputeRootDescriptorTable(2, storage->Gpu(inputs.frame_slot, kSrvCount));
            commands->SetComputeRootConstantBufferView(4, inputs.frame_constants_address);
            commands->SetComputeRootShaderResourceView(5, inputs.material_parameters.resource->GetGPUVirtualAddress());
            const std::array<std::uint32_t, 4> pass{0, inputs.ambient_visibility_enabled ? 1u : 0u, inputs.sidecar_plane_count, 0};
            commands->SetComputeRoot32BitConstants(3, UINT(pass.size()), pass.data(), 0);
            if (pipeline == 2)
            {
                // The reflection pyramid is closest-depth MAX in reversed Z,
                // rebuilt after the complete opaque surface, unlike culling HZB.
                commands->SetPipelineState(storage->pipelines[0].Get());
                commands->Dispatch((inputs.render_active.width + 7) / 8, (inputs.render_active.height + 7) / 8, 1);
                context.UavBarrier(hierarchyHandle);
                ViewExtent levelActive = inputs.render_active;
                for (std::uint32_t level = 1; level < storage->hierarchy_count; ++level)
                {
                    levelActive = {(levelActive.width + 1) / 2, (levelActive.height + 1) / 2};
                    commands->SetComputeRoot32BitConstant(3, level, 0);
                    commands->SetPipelineState(storage->pipelines[1].Get());
                    commands->Dispatch((levelActive.width + 7) / 8, (levelActive.height + 7) / 8, 1);
                    context.UavBarrier(hierarchyHandle);
                }
                commands->SetComputeRoot32BitConstant(3, 0, 0);
            }
            commands->SetPipelineState(storage->pipelines[pipeline].Get());
            commands->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            return Result::Success();
        };
    };
    auto readSurface = [&](PassBuilder &pass) {
        for (const auto &input : {inputs.depth, inputs.base, inputs.normal, inputs.metadata, inputs.material, inputs.material_parameters})
            pass.Read(input.version, Access::ComputeRead);
        for (std::uint32_t index = 0; index < inputs.sidecar_plane_count; ++index)
            pass.Read(inputs.sidecars[index].version, Access::ComputeRead);
    };
    auto trace = graph.AddPass(70, "SSRTrace", QueueHint::Direct);
    readSurface(trace); trace.Read(inputs.opaque_hdr.version, Access::ComputeRead);
    trace.Read(inputs.environment_specular.version, Access::ComputeRead);
    if (inputs.ambient_visibility_enabled) trace.Read(inputs.ambient_visibility.version, Access::ComputeRead);
    hierarchy = trace.Write(hierarchy, Access::UnorderedWrite); raw = trace.Write(raw, Access::UnorderedWrite);
    trace.SetExecuteChecked(execute(2, traceActive));
    auto temporal = graph.AddPass(71, "SSRTemporal", QueueHint::Direct);
    readSurface(temporal); temporal.Read(raw, Access::UnorderedRead);
    temporal.Read(inputs.motion.version, Access::ComputeRead); temporal.Read(inputs.surface_history.version, Access::ComputeRead);
    if (historyValid)
    {
        temporal.Read(previous, Access::UnorderedRead); temporal.Read(previousValidation, Access::UnorderedRead);
    }
    current = temporal.Write(current, Access::UnorderedWrite); validation = temporal.Write(validation, Access::UnorderedWrite);
    temporal.SetExecuteChecked(execute(3, traceActive));
    auto spatial = graph.AddPass(72, "SSRSpatial", QueueHint::Direct);
    readSurface(spatial); spatial.Read(current, Access::UnorderedRead); spatial.Read(validation, Access::UnorderedRead);
    raw = spatial.Write(raw, Access::UnorderedWrite); spatial.SetExecuteChecked(execute(4, active));
    auto compose = graph.AddPass(73, "SSRCompose", QueueHint::Direct);
    compose.Read(raw, Access::ComputeRead); compose.Read(inputs.opaque_hdr.version, Access::ComputeRead);
    compose.Read(inputs.environment_specular.version, Access::ComputeRead);
    composed = compose.Write(composed, Access::UnorderedWrite); compose.SetExecuteChecked(execute(5, active));
    graph.SetFinalAccess(current, Access::UnorderedRead); graph.SetFinalAccess(validation, Access::UnorderedRead);
    graph.SetFinalAccess(raw, Access::ComputeRead); graph.SetFinalAccess(composed, Access::ComputeRead);
    outputs = {raw, composed, storage->raw.resource.Get(), storage->composed.resource.Get(),
               storage->export_descriptors->GetCPUDescriptorHandleForHeapStart(), storage->export_descriptors->GetCPUDescriptorHandleForHeapStart()};
    outputs.composed_srv.ptr += storage->descriptor_stride;
    impl_->pending = std::move(pending);
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
