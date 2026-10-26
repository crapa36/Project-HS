#include "renderer_hdr_post.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kSrvCount = 14, kUavCount = 6, kTableSize = kSrvCount + kUavCount;
constexpr std::uint32_t kPhaseCount = 6, kConstantStride = 512;
constexpr std::uint32_t kLutEdge = 33;
constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R16G16B16A16_FLOAT, kGuide = DXGI_FORMAT_R32G32B32A32_FLOAT;
enum Target : std::uint32_t { Guide, Motion, CoC, Dilated, DoF, Grade, Lut, Normal, TargetCount };
constexpr std::array<std::string_view, kPhaseCount> kProducts{
    "render/shader/hdr_post_guides_cs.dxil", "render/shader/hdr_post_motion_blur_cs.dxil",
    "render/shader/hdr_post_dilate_cs.dxil",
    "render/shader/hdr_post_dof_cs.dxil", "render/shader/hdr_post_lut_init_cs.dxil",
    "render/shader/hdr_post_grade_cs.dxil"};
constexpr std::array<DXGI_FORMAT, TargetCount> kFormats{
    kGuide, kColor, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kColor, kColor, kColor, DXGI_FORMAT_R16G16_FLOAT};
std::atomic<std::uint64_t> g_hdr_post_target_generation{};
static_assert(sizeof(HdrPostConstants) <= kConstantStride);
static_assert(kConstantStride % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0);
Result Failure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_hdr_post", std::move(message));
}
Result NativeFailure(std::string_view operation, HRESULT error)
{
    return Failure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool GeometryEffects(const HdrPostSettings &settings)
{
    return settings.motion_blur != GraphicsMotionBlur::Off || settings.depth_of_field;
}
bool Enabled(const HdrPostSettings &settings) { return GeometryEffects(settings) || settings.color_grading; }
bool Fits(const HdrPostResource &input, ViewExtent active, DXGI_FORMAT format)
{
    if (!input.resource || !input.version.resource || input.version.resource.kind != ResourceKind::Texture) return false;
    const auto desc = input.resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.Width >= active.width && desc.Height >= active.height && desc.Format == format;
}
bool FitsDepth(const HdrPostResource &input, ViewExtent active)
{
    return Fits(input, active, DXGI_FORMAT_R32_TYPELESS) || Fits(input, active, DXGI_FORMAT_D32_FLOAT) ||
        Fits(input, active, DXGI_FORMAT_R32_FLOAT);
}
bool ValidGrade(const DirectX::XMFLOAT4 &value, bool positive, bool nonnegative)
{
    for (const auto channel : {value.x, value.y, value.z})
        if (!std::isfinite(channel) || (positive && channel <= 0) || (nonnegative && channel < 0)) return false;
    return true;
}
bool ValidSettings(const HdrPostSettings &settings)
{
    if (settings.motion_blur != GraphicsMotionBlur::Off && settings.motion_blur != GraphicsMotionBlur::Low &&
        settings.motion_blur != GraphicsMotionBlur::High) return false;
    if (GeometryEffects(settings) && (!std::isfinite(settings.focus_distance_m) || !std::isfinite(settings.focal_length_m) ||
        !std::isfinite(settings.f_number) || !std::isfinite(settings.sensor_width_m) || !std::isfinite(settings.max_coc_pixels) ||
        settings.focal_length_m <= 0 || settings.focus_distance_m <= settings.focal_length_m || settings.f_number <= 0 ||
        settings.sensor_width_m <= 0 || settings.max_coc_pixels <= 0 || settings.max_coc_pixels > 32)) return false;
    return !settings.color_grading || (ValidGrade(settings.grade_lift, false, false) &&
        ValidGrade(settings.grade_contrast, true, false) && ValidGrade(settings.grade_gain, false, true));
}
Result CheckFormat(ID3D12Device *device, DXGI_FORMAT format, bool volume)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    const auto native = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(native)) return NativeFailure("Check HDR post format", native);
    const auto dimension = volume ? D3D12_FORMAT_SUPPORT1_TEXTURE3D : D3D12_FORMAT_SUPPORT1_TEXTURE2D;
    if (!(support.Support1 & dimension) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
        !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) || !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return Failure(ErrorCode::TaskFailed, "HDR post format lacks required texture, sample or typed UAV-store support.");
    return Result::Success();
}
Result CreateTexture(D3D12MA::Allocator *allocator, ViewExtent capacity, DXGI_FORMAT format, bool volume,
    AllocationResource &texture)
{
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = volume ? kLutEdge : capacity.width; desc.Height = volume ? kLutEdge : capacity.height;
    desc.DepthOrArraySize = volume ? kLutEdge : 1; desc.MipLevels = 1; desc.Format = format;
    desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto native = allocator->CreateResource(&allocation, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        &texture.allocation, IID_PPV_ARGS(texture.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create HDR post texture", native);
    texture.graph_access = Access::Common;
    return Result::Success();
}
void PopulateConstants(HdrPostConstants &constants, const HdrPostSettings &settings, const HdrPostInputs &inputs)
{
    constants.render_size = {float(inputs.render_active.width), float(inputs.render_active.height),
        1.0f / float(inputs.render_active.width), 1.0f / float(inputs.render_active.height)};
    constants.output_size = {float(inputs.output_active.width), float(inputs.output_active.height),
        1.0f / float(inputs.output_active.width), 1.0f / float(inputs.output_active.height)};
    // Fixed 180-degree shutter at the project's 60-Hz reference; authored MV
    // spans actual render dt, so divide by dt once to keep exposure time stable.
    constants.frame_parameters.z = GeometryEffects(settings) && constants.frame_parameters.y > 0.0f &&
        constants.frame_parameters.x > 0.0f ? (1.0f / 120.0f) / constants.frame_parameters.x : 0.0f;
    constants.frame_parameters.w = inputs.sharpening;
    constants.lens = {settings.focus_distance_m, settings.focal_length_m, settings.f_number, settings.sensor_width_m};
    constants.grade_lift = settings.grade_lift; constants.grade_contrast = settings.grade_contrast;
    constants.grade_gain = settings.grade_gain;
    constants.effect_parameters = {settings.max_coc_pixels, std::log2(65505.0f), 1.0f / 60.0f, 180.0f};
    constants.options = {settings.motion_blur == GraphicsMotionBlur::Off ? 0u :
        (settings.motion_blur == GraphicsMotionBlur::High ? 16u : 8u),
        inputs.transparent_guides_enabled ? 1u : 0u, kLutEdge, settings.depth_of_field ? 1u : 0u};
}
} // namespace

struct HdrPostTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, kPhaseCount> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors, exports;
    std::array<AllocationResource, TargetCount> textures;
    AllocationResource constants;
    std::byte *mapped{};
    ViewExtent render_capacity, output_capacity;
    HdrPostSettings settings;
    bool usable{true}, lut_initialized{};
    std::uint64_t generation{};
    std::uint32_t frame_slot_count{}, descriptor_stride{};
    std::vector<CompletionSet> slot_completion;
    CompletionSet completion;
    std::uint64_t bytes{};
    ~HdrPostTargets() { if (mapped && constants.resource) constants.resource->Unmap(0, nullptr); }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot, std::uint32_t phase, std::uint32_t offset) const
    {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T((slot * kPhaseCount + phase) * kTableSize + offset) * descriptor_stride; return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t slot, std::uint32_t phase, std::uint32_t offset) const
    {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64((slot * kPhaseCount + phase) * kTableSize + offset) * descriptor_stride; return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Export(std::uint32_t offset) const
    {
        auto handle = exports->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(offset) * descriptor_stride; return handle;
    }
    void Srv(ID3D12Resource *resource, DXGI_FORMAT format, bool volume, D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = format;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = volume ? D3D12_SRV_DIMENSION_TEXTURE3D : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (volume) view.Texture3D.MipLevels = 1; else view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &view, handle);
    }
    void Uav(ID3D12Resource *resource, DXGI_FORMAT format, bool volume, D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format = format;
        view.ViewDimension = volume ? D3D12_UAV_DIMENSION_TEXTURE3D : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (volume) view.Texture3D.WSize = kLutEdge;
        device->CreateUnorderedAccessView(resource, nullptr, &view, handle);
    }
    void Table(const HdrPostInputs &inputs, const HdrPostConstants &values, std::uint32_t phase,
        ID3D12Resource *source, ID3D12Resource *colorOutput)
    {
        std::memcpy(mapped + SIZE_T(inputs.frame_slot * kPhaseCount + phase) * kConstantStride, &values, sizeof(values));
        const std::array<ID3D12Resource *, kSrvCount> resources{source, inputs.depth.resource, inputs.motion.resource,
            inputs.surface_history.resource, inputs.transparent_guides_enabled ? inputs.transparent_depth.resource : nullptr,
            inputs.transparent_guides_enabled ? inputs.transparent_motion.resource : nullptr,
            inputs.transparent_guides_enabled ? inputs.transparent_history.resource : nullptr,
            inputs.transparent_guides_enabled ? inputs.transparent_classification.resource : nullptr,
            textures[Guide].resource.Get(), textures[CoC].resource.Get(), textures[Dilated].resource.Get(), textures[Lut].resource.Get(),
            settings.motion_blur != GraphicsMotionBlur::Off ? inputs.normal.resource : nullptr, textures[Normal].resource.Get()};
        constexpr std::array<DXGI_FORMAT, kSrvCount> formats{kColor, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
            kColor, kGuide, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kColor, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_FLOAT};
        for (std::uint32_t index = 0; index < kSrvCount; ++index)
            Srv(resources[index], formats[index], index == 11, Cpu(inputs.frame_slot, phase, index));
        const std::array<ID3D12Resource *, kUavCount> outputs{colorOutput, textures[Guide].resource.Get(),
            textures[CoC].resource.Get(), textures[Dilated].resource.Get(), textures[Lut].resource.Get(), textures[Normal].resource.Get()};
        constexpr std::array<DXGI_FORMAT, kUavCount> outputFormats{kColor, kGuide, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, kColor, DXGI_FORMAT_R16G16_FLOAT};
        for (std::uint32_t index = 0; index < kUavCount; ++index)
            Uav(outputs[index], outputFormats[index], index == 4, Cpu(inputs.frame_slot, phase, kSrvCount + index));
    }
    auto Execute(std::uint32_t slot, std::uint32_t phase, ViewExtent active, std::shared_ptr<HdrPostTargets> lease)
    {
        return [lease = std::move(lease), slot, phase, active](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            ID3D12DescriptorHeap *heaps[]{lease->descriptors.Get()}; commands->SetDescriptorHeaps(1, heaps);
            commands->SetComputeRootSignature(lease->root.Get()); commands->SetPipelineState(lease->pipelines[phase].Get());
            commands->SetComputeRootConstantBufferView(0, lease->constants.resource->GetGPUVirtualAddress() +
                UINT64(slot * kPhaseCount + phase) * kConstantStride);
            commands->SetComputeRootDescriptorTable(1, lease->Gpu(slot, phase, 0));
            commands->SetComputeRootDescriptorTable(2, lease->Gpu(slot, phase, kSrvCount));
            if (phase == 4) commands->Dispatch((kLutEdge + 3) / 4, (kLutEdge + 3) / 4, (kLutEdge + 3) / 4);
            else commands->Dispatch((active.width + 7) / 8, (active.height + 7) / 8, 1);
        };
    }
};
struct HdrPostSystem::Impl
{
    std::shared_ptr<HdrPostTargets> targets;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<HdrPostTargets> targets;
        std::vector<Import> imports;
        std::array<ResourceVersion, TargetCount> versions;
        HdrPostConstants constants;
        std::uint32_t slot{};
        bool geometry_registered{}, grade_registered{}, initializing_lut{};
    } pending;
    Result Begin(RenderGraphBuilder &graph, const HdrPostInputs &inputs,
        std::array<ResourceHandle, TargetCount> *captured,
        const std::array<ResourceHandle, TargetCount> *retained)
    {
        auto storage = targets;
        if (!storage) return Failure(ErrorCode::InvalidState, "Prepare and commit HDR post before registration.");
        if (!storage->usable) return Failure(ErrorCode::InvalidState, "HDR post requires a completed drain and replacement after failed submission.");
        if (!ValidExtent(inputs.render_active) || inputs.render_active.width > storage->render_capacity.width ||
            inputs.render_active.height > storage->render_capacity.height || !ValidExtent(inputs.output_active) ||
            inputs.output_active.width > storage->output_capacity.width || inputs.output_active.height > storage->output_capacity.height ||
            inputs.frame_slot >= storage->frame_slot_count || !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed) ||
            !Fits(inputs.hdr, inputs.output_active, kColor) || !std::isfinite(inputs.sharpening) ||
            inputs.sharpening < 0 || inputs.sharpening > 1)
            return Failure(ErrorCode::InvalidArgument, "HDR post extent, frame slot, completion or output-resolution HDR input is invalid.");
        if (pending.graph)
        {
            if (pending.graph != &graph || pending.slot != inputs.frame_slot)
                return Failure(ErrorCode::InvalidState, "Complete the previous HDR post graph before registering another graph or frame slot.");
            return Result::Success();
        }
        for (auto &allocation : storage->textures)
            if (allocation.resource)
                if (auto certified = allocation.EnsureNativeContract(storage->device.Get()); !certified)
                    return certified;
        pending.graph = &graph; pending.targets = storage; pending.slot = inputs.frame_slot;
        for (std::uint32_t index = 0; index < TargetCount; ++index)
        {
            auto &allocation = storage->textures[index];
            if (!allocation.resource) continue;
            const auto current = ExternalTexture{allocation.resource.Get(), allocation.graph_completion,
                allocation.graph_queue, {}, allocation.NativeContract()};
            const auto handle = retained ? (*retained)[index] :
                graph.ImportTexture(current, allocation.graph_access, "HdrPost.PersistentTarget");
            if (retained && !graph.BindTexture(handle, current, allocation.graph_access))
                return Failure(ErrorCode::InvalidState, "HDR post retained target binding changed.");
            if (captured) (*captured)[index] = handle;
            pending.imports.push_back({&allocation, handle}); pending.versions[index] = graph.Version(handle);
        }
        return Result::Success();
    }
};

HdrPostSystem::HdrPostSystem() : impl_(std::make_unique<Impl>()) {}
HdrPostSystem::~HdrPostSystem() = default;
std::uint64_t HdrPostCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t HdrPostSystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }
Result HdrPostSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator, ViewExtent renderCapacity,
    ViewExtent outputCapacity, const HdrPostSettings &settings, std::uint32_t frameSlots, const ShaderReader &readShader,
    HdrPostCandidate &candidate) const
{
    if (!device || !allocator || !ValidExtent(renderCapacity) || !ValidExtent(outputCapacity) || !ValidSettings(settings) ||
        !frameSlots || frameSlots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / (kTableSize * kPhaseCount) ||
        (Enabled(settings) && !readShader)) return Failure(ErrorCode::InvalidArgument, "Invalid HDR post device, capacities, settings, frame slots or shader reader.");
    auto next = std::make_shared<HdrPostTargets>();
    next->device = device; next->allocator = allocator; next->render_capacity = renderCapacity; next->output_capacity = outputCapacity;
    next->settings = settings; next->frame_slot_count = frameSlots; next->slot_completion.resize(frameSlots);
    if (!Enabled(settings)) { candidate.targets_ = std::move(next); return Result::Success(); }
    const std::array<bool, TargetCount> selected{GeometryEffects(settings), settings.motion_blur != GraphicsMotionBlur::Off,
        settings.depth_of_field, settings.depth_of_field, settings.depth_of_field, settings.color_grading, settings.color_grading,
        settings.motion_blur != GraphicsMotionBlur::Off};
    for (std::uint32_t index = 0; index < TargetCount; ++index)
        if (selected[index])
        {
            if (auto supported = CheckFormat(device, kFormats[index], index == Lut); !supported) return supported;
            if (auto created = CreateTexture(allocator, outputCapacity, kFormats[index], index == Lut, next->textures[index]); !created) return created;
        }
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 17, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0, 17, 0}};
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[0].Descriptor = {0, 17};
    parameters[1].ParameterType = parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[0]}; parameters[2].DescriptorTable = {1, &ranges[1]};
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.MaxAnisotropy = 1;
    sampler.RegisterSpace = 17; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters = 3; root.pParameters = parameters;
    root.NumStaticSamplers = 1; root.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> signature, diagnostics;
    auto native = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics);
    if (FAILED(native))
    {
        std::string message = std::string(NativeFailure("Serialize HDR post root", native).Message());
        if (diagnostics && diagnostics->GetBufferSize()) message += " " + std::string(static_cast<const char *>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize());
        return Failure(ErrorCode::TaskFailed, std::move(message));
    }
    native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&next->root));
    if (FAILED(native)) return NativeFailure("Create HDR post root", native);
    const std::array<bool, kPhaseCount> phases{GeometryEffects(settings), settings.motion_blur != GraphicsMotionBlur::Off,
        settings.depth_of_field, settings.depth_of_field, settings.color_grading, settings.color_grading};
    for (std::uint32_t index = 0; index < kPhaseCount; ++index)
        if (phases[index])
        {
            std::vector<std::byte> bytecode;
            if (auto read = readShader(kProducts[index], bytecode); !read) return read;
            if (bytecode.empty()) return Failure(ErrorCode::TaskFailed, "HDR post shader product is empty: " + std::string(kProducts[index]));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature = next->root.Get();
            pipeline.CS = {bytecode.data(), bytecode.size()};
            native = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&next->pipelines[index]));
            if (FAILED(native)) return NativeFailure("Create HDR post pipeline " + std::string(kProducts[index]), native);
        }
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = frameSlots * kPhaseCount * kTableSize; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->descriptors));
    if (FAILED(native)) return NativeFailure("Create HDR post phase descriptors", native);
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors = 3; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->exports));
    if (FAILED(native)) return NativeFailure("Create HDR post exports", native);
    next->Srv(next->textures[Motion].resource.Get(), kColor, false, next->Export(0));
    next->Srv(next->textures[DoF].resource.Get(), kColor, false, next->Export(1));
    next->Srv(next->textures[Grade].resource.Get(), kColor, false, next->Export(2));
    D3D12MA::ALLOCATION_DESC uploadAllocation{}; uploadAllocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload{}; upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload.Width = UINT64(frameSlots) * kPhaseCount * kConstantStride; upload.Height = 1; upload.DepthOrArraySize = 1;
    upload.MipLevels = 1; upload.SampleDesc.Count = 1; upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    native = allocator->CreateResource(&uploadAllocation, &upload, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        &next->constants.allocation, IID_PPV_ARGS(next->constants.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create HDR post constants", native);
    void *mapped{}; D3D12_RANGE noRead{0, 0}; native = next->constants.resource->Map(0, &noRead, &mapped);
    if (FAILED(native)) return NativeFailure("Map HDR post constants", native);
    next->mapped = static_cast<std::byte *>(mapped);
    for (const auto &texture : next->textures) if (texture.allocation) next->bytes += texture.allocation->GetSize();
    next->bytes += next->constants.allocation->GetSize(); candidate.targets_ = std::move(next);
    return Result::Success();
}
Result HdrPostSystem::Commit(HdrPostCandidate &&candidate, const CompletionSet &completed)
{
    if (!candidate.targets_) return Failure(ErrorCode::InvalidArgument, "HDR post candidate is empty.");
    if (impl_->pending.graph || (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState, "Complete and drain HDR post GPU uses before replacing its targets.");
    candidate.targets_->generation = g_hdr_post_target_generation.fetch_add(1,std::memory_order_relaxed)+1;
    impl_->targets = std::move(candidate.targets_); return Result::Success();
}
bool HdrPostSystem::MatchesMotionLayout(const HdrPostInputs &in,
    const HdrPostMotionLayout &layout) const noexcept
{
    const auto storage=impl_->targets;
    if(!storage||!storage->usable||storage->generation!=layout.target_generation||
        layout.enabled!=GeometryEffects(storage->settings)||
        layout.motion!=(storage->settings.motion_blur!=GraphicsMotionBlur::Off)||
        layout.dof!=storage->settings.depth_of_field||
        (layout.enabled&&layout.transparent_guides!=in.transparent_guides_enabled))return false;
    if(!layout.enabled)return true;
    const std::array<ResourceVersion,9> versions{in.hdr.version,in.depth.version,in.motion.version,
        in.surface_history.version,in.normal.version,in.transparent_depth.version,
        in.transparent_motion.version,in.transparent_history.version,
        in.transparent_classification.version};
    for(std::size_t i=0;i<versions.size();++i)
        if((i<4||(i==4&&layout.motion)||(i>=5&&layout.transparent_guides))&&
            layout.inputs[i]!=versions[i])return false;
    return true;
}
Result HdrPostSystem::RegisterMotionAndDoF(RenderGraphBuilder &graph, const HdrPostInputs &inputs,
    HdrPostConstants constants, HdrPostOutputs &outputs,HdrPostMotionLayout *layout)
{return RegisterMotionImpl(graph,inputs,constants,outputs,layout,nullptr);}
Result HdrPostSystem::BindRetainedMotionAndDoF(RenderGraphBuilder &graph,const HdrPostInputs &inputs,
    HdrPostConstants constants,const HdrPostMotionLayout &layout,HdrPostOutputs &outputs)
{return RegisterMotionImpl(graph,inputs,constants,outputs,nullptr,&layout);}
Result HdrPostSystem::RegisterMotionImpl(RenderGraphBuilder &graph, const HdrPostInputs &inputs,
    HdrPostConstants constants, HdrPostOutputs &outputs,HdrPostMotionLayout *layout,
    const HdrPostMotionLayout *retained)
{
    outputs = {inputs.hdr.version, inputs.hdr.resource, {}};
    const auto storage = impl_->targets;
    if (!storage) return Failure(ErrorCode::InvalidState, "Prepare and commit HDR post before registration.");
    if(layout){*layout={};layout->target_generation=storage->generation;
        layout->enabled=GeometryEffects(storage->settings);
        layout->motion=storage->settings.motion_blur!=GraphicsMotionBlur::Off;
        layout->dof=storage->settings.depth_of_field;
        layout->transparent_guides=inputs.transparent_guides_enabled;
        layout->inputs={inputs.hdr.version,inputs.depth.version,inputs.motion.version,
            inputs.surface_history.version,inputs.normal.version,inputs.transparent_depth.version,
            inputs.transparent_motion.version,inputs.transparent_history.version,
            inputs.transparent_classification.version};}
    if(retained&&!MatchesMotionLayout(inputs,*retained))
        return Failure(ErrorCode::InvalidState,"HDR motion retained topology changed.");
    if (!GeometryEffects(storage->settings)) return Result::Success();
    if(retained)
    {
        for(UINT index=0;index<TargetCount;++index)
            if(storage->textures[index].resource&&
                (!retained->imports[index]||retained->imports[index].kind!=ResourceKind::Texture||
                    retained->imports[index].value>graph.ResourceCount()))
                return Failure(ErrorCode::InvalidState,"HDR motion retained import slot changed.");
        std::array<std::uint32_t,4> ids{110,111,113,114};
        UINT count=1+UINT(retained->motion)+2*UINT(retained->dof);
        if(retained->pass_count!=count)return Failure(ErrorCode::InvalidState,"HDR motion retained pass count changed.");
        UINT cursor{};
        for(UINT id:ids)if(id==110||(id==111&&retained->motion)||
            ((id==113||id==114)&&retained->dof))
            if(!graph.BindPass(retained->pass_indices[cursor++],id))
                return Failure(ErrorCode::InvalidState,"HDR motion retained pass changed.");
    }
    if (impl_->pending.geometry_registered || impl_->pending.grade_registered)
        return Failure(ErrorCode::InvalidState, "Register HDR motion/DoF once, before grading.");
    if (!FitsDepth(inputs.depth, inputs.render_active) || !Fits(inputs.motion, inputs.render_active, DXGI_FORMAT_R16G16_FLOAT) ||
        !Fits(inputs.surface_history, inputs.render_active, DXGI_FORMAT_R32G32_FLOAT) ||
        (storage->settings.motion_blur != GraphicsMotionBlur::Off && !Fits(inputs.normal, inputs.render_active, DXGI_FORMAT_R16G16_SNORM)) ||
        (inputs.transparent_guides_enabled && (!Fits(inputs.transparent_depth, inputs.render_active, DXGI_FORMAT_R32_FLOAT) ||
            !Fits(inputs.transparent_motion, inputs.render_active, DXGI_FORMAT_R16G16_FLOAT) ||
            !Fits(inputs.transparent_history, inputs.render_active, DXGI_FORMAT_R32G32_FLOAT) ||
            !Fits(inputs.transparent_classification, inputs.render_active, kColor))))
        return Failure(ErrorCode::InvalidArgument, "HDR post render/transparent deformation guides are invalid.");
    for (const auto &row : constants.inverse_jittered_view_projection.m)
        for (const auto value : row) if (!std::isfinite(value)) return Failure(ErrorCode::InvalidArgument, "HDR post inverse matrix is nonfinite.");
    for (const auto &row : constants.previous_unjittered_view_projection.m)
        for (const auto value : row) if (!std::isfinite(value)) return Failure(ErrorCode::InvalidArgument, "HDR post previous sky projection is nonfinite.");
    for (const auto value : {constants.camera_relative_position.x, constants.camera_relative_position.y, constants.camera_relative_position.z,
        constants.camera_forward.x, constants.camera_forward.y, constants.camera_forward.z, constants.jitter_uv.x, constants.jitter_uv.y,
        constants.frame_parameters.x, constants.frame_parameters.y})
        if (!std::isfinite(value)) return Failure(ErrorCode::InvalidArgument, "HDR post camera, jitter, render dt or history state is nonfinite.");
    const float forwardLength = std::sqrt(constants.camera_forward.x * constants.camera_forward.x +
        constants.camera_forward.y * constants.camera_forward.y + constants.camera_forward.z * constants.camera_forward.z);
    if (constants.frame_parameters.x < 0 || (constants.frame_parameters.y > 0 && constants.frame_parameters.x == 0) ||
        forwardLength <= 0 || !std::isfinite(forwardLength))
        return Failure(ErrorCode::InvalidArgument, "HDR post requires positive render dt for valid motion history and a finite camera forward direction.");
    if (constants.frame_parameters.y > 0 && !std::isfinite((1.0f / 120.0f) / constants.frame_parameters.x))
        return Failure(ErrorCode::InvalidArgument, "HDR post render dt produces a nonfinite shutter ratio.");
    constants.camera_forward.x /= forwardLength; constants.camera_forward.y /= forwardLength; constants.camera_forward.z /= forwardLength;
    if (auto started = impl_->Begin(graph, inputs,layout?&layout->imports:nullptr,
            retained?&retained->imports:nullptr); !started) return started;
    auto &pending = impl_->pending; pending.geometry_registered = true;
    PopulateConstants(constants, storage->settings, inputs); pending.constants = constants;
    UINT pass_cursor{};
    const auto make_pass=[&](UINT id,std::string_view name)
    {
        const UINT index=pass_cursor++;
        if(layout)layout->pass_indices[index]=graph.PassCount();
        return retained?*graph.BindPass(retained->pass_indices[index],id):
            graph.AddPass(id,name,QueueHint::Direct);
    };
    storage->Table(inputs, constants, 0, inputs.hdr.resource, nullptr);
    auto guide = make_pass(110, "HdrPostNearestGuides");
    if(!retained)for (const auto version : {inputs.depth.version, inputs.motion.version, inputs.surface_history.version}) guide.Read(version, Access::ComputeRead);
    if (!retained && storage->settings.motion_blur != GraphicsMotionBlur::Off) guide.Read(inputs.normal.version, Access::ComputeRead);
    if (!retained && inputs.transparent_guides_enabled)
        for (const auto version : {inputs.transparent_depth.version, inputs.transparent_motion.version, inputs.transparent_history.version,
                                  inputs.transparent_classification.version}) guide.Read(version, Access::ComputeRead);
    pending.versions[Guide] = retained?retained->versions[Guide]:guide.Write(pending.versions[Guide], Access::UnorderedWrite);
    if (storage->settings.motion_blur != GraphicsMotionBlur::Off)
        pending.versions[Normal] = retained?retained->versions[Normal]:guide.Write(pending.versions[Normal], Access::UnorderedWrite);
    if (storage->settings.depth_of_field)
        pending.versions[CoC] = retained?retained->versions[CoC]:guide.Write(pending.versions[CoC], Access::UnorderedWrite);
    guide.SetExecute(storage->Execute(inputs.frame_slot, 0, inputs.output_active, storage));
    auto source = inputs.hdr.version; auto *sourceNative = inputs.hdr.resource;
    if (storage->settings.motion_blur != GraphicsMotionBlur::Off)
    {
        storage->Table(inputs, constants, 1, sourceNative, storage->textures[Motion].resource.Get());
        auto pass = make_pass(111, "HdrPostMotionBlur");
        if(!retained){pass.Read(source, Access::ComputeRead); pass.Read(pending.versions[Guide], Access::ComputeRead);
            pass.Read(pending.versions[Normal], Access::ComputeRead);}
        pending.versions[Motion] = retained?retained->versions[Motion]:pass.Write(pending.versions[Motion], Access::UnorderedWrite);
        pass.SetExecute(storage->Execute(inputs.frame_slot, 1, inputs.output_active, storage));
        source = pending.versions[Motion]; sourceNative = storage->textures[Motion].resource.Get();
        outputs = {source, sourceNative, storage->Export(0)};
    }
    if (storage->settings.depth_of_field)
    {
        storage->Table(inputs, constants, 2, sourceNative, nullptr);
        auto dilate = make_pass(113, "HdrPostForegroundDilation");
        if(!retained)dilate.Read(pending.versions[CoC], Access::ComputeRead);
        pending.versions[Dilated] = retained?retained->versions[Dilated]:dilate.Write(pending.versions[Dilated], Access::UnorderedWrite);
        dilate.SetExecute(storage->Execute(inputs.frame_slot, 2, inputs.output_active, storage));
        storage->Table(inputs, constants, 3, sourceNative, storage->textures[DoF].resource.Get());
        auto pass = make_pass(114, "HdrPostDepthOfField");
        if(!retained)for (const auto version : {source, pending.versions[Guide], pending.versions[CoC], pending.versions[Dilated]}) pass.Read(version, Access::ComputeRead);
        pending.versions[DoF] = retained?retained->versions[DoF]:pass.Write(pending.versions[DoF], Access::UnorderedWrite);
        pass.SetExecute(storage->Execute(inputs.frame_slot, 3, inputs.output_active, storage));
        outputs = {pending.versions[DoF], storage->textures[DoF].resource.Get(), storage->Export(1)};
    }
    if(layout){layout->versions=pending.versions;layout->output=outputs.hdr;layout->pass_count=pass_cursor;}
    return Result::Success();
}
bool HdrPostSystem::MatchesGradeLayout(const HdrPostInputs &in,
    const HdrPostGradeLayout &layout,bool geometry_registered_before_grade) const noexcept
{
    const auto storage=impl_->targets;
    return storage&&storage->usable&&storage->generation==layout.target_generation&&
        layout.enabled==storage->settings.color_grading&&
        (!layout.enabled||(layout.input==in.hdr.version&&
            layout.initialize_lut==!storage->lut_initialized&&
            layout.imports_in_grade==!geometry_registered_before_grade));
}
Result HdrPostSystem::RegisterGrade(RenderGraphBuilder &graph,const HdrPostInputs &inputs,
    HdrPostOutputs &outputs,HdrPostGradeLayout *layout)
{return RegisterGradeImpl(graph,inputs,outputs,layout,nullptr);}
Result HdrPostSystem::BindRetainedGrade(RenderGraphBuilder &graph,const HdrPostInputs &inputs,
    const HdrPostGradeLayout &layout,HdrPostOutputs &outputs)
{return RegisterGradeImpl(graph,inputs,outputs,nullptr,&layout);}
Result HdrPostSystem::RegisterGradeImpl(RenderGraphBuilder &graph, const HdrPostInputs &inputs,
    HdrPostOutputs &outputs,HdrPostGradeLayout *layout,const HdrPostGradeLayout *retained)
{
    outputs = {inputs.hdr.version, inputs.hdr.resource, {}};
    const auto storage = impl_->targets;
    if (!storage) return Failure(ErrorCode::InvalidState, "Prepare and commit HDR post before registration.");
    if(layout){*layout={};layout->target_generation=storage->generation;
        layout->enabled=storage->settings.color_grading;
        layout->input=inputs.hdr.version;
        layout->initialize_lut=!storage->lut_initialized;
        layout->imports_in_grade=!impl_->pending.graph;}
    if(retained&&!MatchesGradeLayout(inputs,*retained,impl_->pending.geometry_registered))
        return Failure(ErrorCode::InvalidState,"HDR grade retained topology changed.");
    if (!storage->settings.color_grading) return Result::Success();
    if(retained)
    {
        if(retained->pass_count!=1+UINT(retained->initialize_lut)||
            (retained->imports_in_grade!=!impl_->pending.graph))
            return Failure(ErrorCode::InvalidState,"HDR grade retained phase branch changed.");
        if(retained->imports_in_grade)
            for(UINT index=0;index<TargetCount;++index)
                if(storage->textures[index].resource&&
                    (!retained->imports[index]||retained->imports[index].kind!=ResourceKind::Texture||
                        retained->imports[index].value>graph.ResourceCount()))
                    return Failure(ErrorCode::InvalidState,"HDR grade retained import slot changed.");
        UINT index{};
        if(retained->initialize_lut&&!graph.BindPass(retained->pass_indices[index++],115))
            return Failure(ErrorCode::InvalidState,"HDR LUT retained pass changed.");
        if(!graph.BindPass(retained->pass_indices[index],116))
            return Failure(ErrorCode::InvalidState,"HDR grade retained pass changed.");
    }
    if (impl_->pending.grade_registered) return Failure(ErrorCode::InvalidState, "Register HDR grading once per graph.");
    if (auto started = impl_->Begin(graph, inputs,
            layout&&layout->imports_in_grade?&layout->imports:nullptr,
            retained&&retained->imports_in_grade?&retained->imports:nullptr); !started) return started;
    auto &pending = impl_->pending; pending.grade_registered = true;
    auto constants = pending.constants;
    // Grade-only registration has no camera/depth dependency or shutter division.
    PopulateConstants(constants, storage->settings, inputs);
    UINT pass_cursor{};
    const auto make_pass=[&](UINT id,std::string_view name)
    {
        const UINT index=pass_cursor++;
        if(layout)layout->pass_indices[index]=graph.PassCount();
        return retained?*graph.BindPass(retained->pass_indices[index],id):
            graph.AddPass(id,name,QueueHint::Direct);
    };
    if (!storage->lut_initialized)
    {
        storage->Table(inputs, constants, 4, inputs.hdr.resource, nullptr);
        auto init = make_pass(115, "HdrPostLutInitialize");
        pending.versions[Lut] = retained?retained->lut:init.Write(pending.versions[Lut], Access::UnorderedWrite);
        init.SetExecute(storage->Execute(inputs.frame_slot, 4, inputs.output_active, storage)); pending.initializing_lut = true;
    }
    storage->Table(inputs, constants, 5, inputs.hdr.resource, storage->textures[Grade].resource.Get());
    auto grade = make_pass(116, "HdrPostColorGrade");
    if(!retained){grade.Read(inputs.hdr.version, Access::ComputeRead); grade.Read(pending.versions[Lut], Access::ComputeRead);}
    pending.versions[Grade] = retained?retained->output:grade.Write(pending.versions[Grade], Access::UnorderedWrite);
    grade.SetExecute(storage->Execute(inputs.frame_slot, 5, inputs.output_active, storage));
    outputs = {pending.versions[Grade], storage->textures[Grade].resource.Get(), storage->Export(2)};
    if(layout){layout->lut=pending.versions[Lut];layout->output=outputs.hdr;layout->pass_count=pass_cursor;}
    return Result::Success();
}
Result HdrPostSystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph)
    {
        if (impl_->targets && !Enabled(impl_->targets->settings)) return Result::Success();
        return Failure(ErrorCode::InvalidState, "HDR post has no registered submission to complete.");
    }
    if (pending.graph != &graph) return Failure(ErrorCode::InvalidArgument, "HDR post completion refers to a different graph.");
    auto storage = pending.targets; storage->completion.Merge(completion); storage->slot_completion[pending.slot].Merge(completion);
    for (const auto &imported : pending.imports)
    {
        const auto resourceCompletion = graph.ResourceCompletion(imported.handle);
        storage->completion.Merge(resourceCompletion); storage->slot_completion[pending.slot].Merge(resourceCompletion);
        imported.allocation->graph_completion.Merge(completion); imported.allocation->graph_completion.Merge(resourceCompletion);
        if (success) { imported.allocation->graph_access = graph.FinalAccess(imported.handle); imported.allocation->graph_queue = graph.FinalQueue(imported.handle); }
    }
    if (success && pending.initializing_lut) storage->lut_initialized = true;
    if (!success) storage->usable = false;
    pending = {}; return Result::Success();
}
Result HdrPostSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete the HDR post graph before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed)) return Failure(ErrorCode::InvalidState, "Drain HDR post GPU uses before reset.");
    impl_->targets.reset(); return Result::Success();
}
} // namespace hs
