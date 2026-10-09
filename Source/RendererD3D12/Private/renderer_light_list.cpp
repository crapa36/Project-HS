#include "renderer_light_list.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kTilePixels = 16, kSlices = 24;
constexpr std::uint32_t kDescriptorCount = 4, kHeaderBytes = sizeof(LightListConstants);
constexpr std::uint64_t kMaxBufferBytes = UINT64(UINT32_MAX) & ~UINT64(3);

Result LightFailure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_light_list", std::move(message));
}
Result NativeFailure(std::string_view operation, HRESULT error)
{
    return LightFailure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool Finite(Float3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool Finite(const DirectX::XMFLOAT4X4 &matrix)
{
    for (const auto &row : matrix.m)
        for (float value : row) if (!std::isfinite(value)) return false;
    return true;
}
Result EncodeLight(const LightView &light, GpuLightRecord &record)
{
    if (!light.stable_id || !light.generation || !light.revision || !Finite(light.color) ||
        light.color.x < 0 || light.color.y < 0 || light.color.z < 0 ||
        !std::isfinite(light.quantity) || light.quantity < 0 ||
        (light.shadow_flags & ~kLightShadowRequested))
        return LightFailure(ErrorCode::InvalidArgument, "Invalid light identity, revision, quantity, color or shadow flags.");
    const bool directional = light.type == LightType::Directional;
    const bool area = light.type == LightType::Rectangle || light.type == LightType::Disk;
    if (!directional && !area && light.type != LightType::Point && light.type != LightType::Spot)
        return LightFailure(ErrorCode::InvalidArgument, "Unsupported light type.");
    if (light.unit != (directional ? LightUnit::Lux : area ? LightUnit::Luminance : LightUnit::Lumen) ||
        (light.legacy_attenuation && light.type != LightType::Point) || (light.two_sided && !area))
        return LightFailure(ErrorCode::InvalidArgument, "Light type/unit or legacy Point attenuation contract does not match.");

    double intensity = double(light.quantity) / kLightWhiteLumensPerWatt;
    record = {};
    if (!directional && !area)
    {
        const double rangeSquared = double(light.range) * light.range;
        const double radiusSquared = double(light.emitter_radius) * light.emitter_radius;
        if (!Finite(light.position) || !std::isfinite(light.range) || light.range <= 0 ||
            rangeSquared < std::numeric_limits<float>::min() || rangeSquared > std::numeric_limits<float>::max() ||
            !std::isfinite(light.emitter_radius) || light.emitter_radius < 0 ||
            (!light.legacy_attenuation && (light.emitter_radius <= 0 || radiusSquared < std::numeric_limits<float>::min())) ||
            radiusSquared > std::numeric_limits<float>::max())
            return LightFailure(ErrorCode::InvalidArgument, "Local light needs a finite range and an authored finite emitter radius.");
        record.position_range = {light.position.x, light.position.y, light.position.z, light.range};
        record.shape_cone.x = float(radiusSquared);
        intensity /= 4.0 * std::numbers::pi_v<double>;
    }
    if (directional || area || light.type == LightType::Spot)
    {
        const double lengthSquared = double(light.direction.x) * light.direction.x +
            double(light.direction.y) * light.direction.y + double(light.direction.z) * light.direction.z;
        if (!Finite(light.direction) || lengthSquared <= 0 || !std::isfinite(lengthSquared))
            return LightFailure(ErrorCode::InvalidArgument, "Directional, Spot and area lights need a finite propagation direction.");
        const double inverseLength = 1.0 / std::sqrt(lengthSquared);
        record.direction_cos_outer = {float(light.direction.x * inverseLength),
            float(light.direction.y * inverseLength), float(light.direction.z * inverseLength), -1.0f};
    }
    if (area)
    {
        const double width=double(light.shape.x)*(light.type==LightType::Rectangle?0.5:1.0);
        const double height=double(light.shape.y)*(light.type==LightType::Rectangle?0.5:1.0);
        const double emitterArea=width*height*(light.type==LightType::Rectangle?4.0:std::numbers::pi_v<double>);
        const double bound=light.type==LightType::Rectangle?std::hypot(width,height):std::max(width,height);
        const double influenceBound=double(light.range)+bound;
        if (!Finite(light.position) || !Finite(light.area_tangent) || !std::isfinite(light.range) || light.range<=0 ||
            !std::isfinite(width) || !std::isfinite(height) || width<=0 || height<=0 ||
            emitterArea<std::numeric_limits<float>::min() || emitterArea>std::numeric_limits<float>::max() ||
            influenceBound*influenceBound>std::numeric_limits<float>::max() ||
            double(light.range)*light.range<std::numeric_limits<float>::min())
            return LightFailure(ErrorCode::InvalidArgument, "Area light needs finite positive dimensions/range and an authored tangent.");
        // Cross the original float inputs in double before normalization. A
        // parallel authored tangent stays exactly degenerate instead of turning
        // float normal-rounding residue into an invented emitter orientation.
        const double bx=double(light.direction.y)*light.area_tangent.z-double(light.direction.z)*light.area_tangent.y;
        const double by=double(light.direction.z)*light.area_tangent.x-double(light.direction.x)*light.area_tangent.z;
        const double bz=double(light.direction.x)*light.area_tangent.y-double(light.direction.y)*light.area_tangent.x;
        const double length=std::hypot(bx,by,bz);
        if (!(length>0) || !std::isfinite(length))
            return LightFailure(ErrorCode::InvalidArgument, "Area tangent must span the authored emitter plane.");
        const double directionLength=std::hypot(double(light.direction.x),double(light.direction.y),double(light.direction.z));
        const double nx=light.direction.x/directionLength,ny=light.direction.y/directionLength,nz=light.direction.z/directionLength;
        const double ux=bx/length,uy=by/length,uz=bz/length;
        record.area_tangent_half_width={float(uy*nz-uz*ny),float(uz*nx-ux*nz),float(ux*ny-uy*nx),float(width)};
        record.area_bitangent_half_height={float(ux),float(uy),float(uz),float(height)};
        record.position_range={light.position.x,light.position.y,light.position.z,light.range};
        record.shape_cone={float(emitterArea),float(bound),0,0};
        // Luminance/683 is emitted radiance per square meter per steradian.
        // Area geometry supplies its own projected solid angle, not 1/r^2 at center.
    }
    if (light.type == LightType::Spot)
    {
        if (!std::isfinite(light.inner_cone_radians) || !std::isfinite(light.outer_cone_radians) ||
            light.inner_cone_radians < 0 || light.inner_cone_radians >= light.outer_cone_radians ||
            light.outer_cone_radians >= std::numbers::pi_v<float> * 0.5f)
            return LightFailure(ErrorCode::InvalidArgument, "Spot half angles require 0 <= inner < outer < pi/2.");
        // Normalize the exact squared cosine ramp, including its fully lit cap.
        const float inner = std::cos(light.inner_cone_radians), outer = std::cos(light.outer_cone_radians);
        const double cosineWidth = double(inner) - outer;
        if (cosineWidth <= 0 || !std::isfinite(1.0 / cosineWidth) ||
            1.0 / cosineWidth > std::numeric_limits<float>::max())
            return LightFailure(ErrorCode::InvalidArgument, "Spot cone ramp cannot be represented by the GPU light ABI.");
        const double effectiveSolidAngle = 2.0 * std::numbers::pi_v<double> *
            ((1.0 - inner) + cosineWidth / 3.0);
        intensity = double(light.quantity) / (kLightWhiteLumensPerWatt * effectiveSolidAngle);
        record.direction_cos_outer.w = outer;
        record.shape_cone.y = inner;
        record.shape_cone.z = float(1.0 / cosineWidth);
        record.shape_cone.w = float(-double(outer) / cosineWidth);
    }
    if (!std::isfinite(intensity) || intensity > std::numeric_limits<float>::max() ||
        double(light.color.x) * intensity > std::numeric_limits<float>::max() ||
        double(light.color.y) * intensity > std::numeric_limits<float>::max() ||
        double(light.color.z) * intensity > std::numeric_limits<float>::max())
        return LightFailure(ErrorCode::InvalidArgument, "Converted radiometric light quantity exceeds the GPU ABI.");
    if (!directional && !area && !light.legacy_attenuation)
    {
        const double maximumIncident = intensity * std::max({double(light.color.x),
            double(light.color.y), double(light.color.z)}) / record.shape_cone.x;
        if (!std::isfinite(maximumIncident) || maximumIncident > std::numeric_limits<float>::max())
            return LightFailure(ErrorCode::InvalidArgument, "Finite-emitter light intensity exceeds the GPU ABI at its authored radius.");
    }
    record.color_intensity = {light.color.x, light.color.y, light.color.z, float(intensity)};
    record.identity = {std::uint32_t(light.stable_id), std::uint32_t(light.stable_id >> 32),
        light.generation, std::uint32_t(light.type)};
    record.metadata = {std::uint32_t(light.revision), std::uint32_t(light.revision >> 32),
        light.shadow_flags, (light.legacy_attenuation ? 1u : 0u) | (light.two_sided ? 4u : 0u)};
    return Result::Success();
}
Result CreateBuffer(D3D12MA::Allocator *allocator, std::uint64_t bytes, bool upload, bool uav,
    AllocationResource &target)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; description.Width = bytes;
    description.Height = 1; description.DepthOrArraySize = 1; description.MipLevels = 1;
    description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    const auto native = allocator->CreateResource(&allocation, &description,
        upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON, nullptr,
        &target.allocation, IID_PPV_ARGS(target.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create light-list buffer", native);
    target.graph_access = upload ? Access::CopySource : Access::Common;
    return Result::Success();
}
void CreateSrv(ID3D12Device *device, ID3D12Resource *resource, std::uint32_t stride,
    D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
    view.Buffer.NumElements = UINT(resource->GetDesc().Width / (stride ? stride : 4));
    view.Buffer.StructureByteStride = stride;
    view.Buffer.Flags = stride ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(resource, &view, handle);
}
void CreateUav(ID3D12Device *device, ID3D12Resource *resource, std::uint32_t stride,
    D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    view.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
    view.Buffer.NumElements = UINT(resource->GetDesc().Width / (stride ? stride : 4));
    view.Buffer.StructureByteStride = stride;
    view.Buffer.Flags = stride ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(resource, nullptr, &view, handle);
}
} // namespace

struct LightListTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors, export_descriptors;
    struct Frame
    {
        std::array<AllocationResource, 4> buffers;
        CompletionSet completion;
    };
    std::vector<Frame> frames;
    AllocationResource upload;
    std::byte *mapped{};
    ViewExtent capacity;
    std::uint32_t max_lights{}, inline_capacity{}, spill_capacity{}, descriptor_stride{};
    std::uint64_t upload_stride{}, bytes{};
    CompletionSet completion;
    bool usable{true};
    ~LightListTargets() { if (mapped && upload.resource) upload.resource->Unmap(0, nullptr); }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot, std::uint32_t index, bool exported = false) const
    {
        auto handle = (exported ? export_descriptors : descriptors)->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slot * kDescriptorCount + index) * descriptor_stride;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t slot) const
    {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64(slot * kDescriptorCount) * descriptor_stride;
        return handle;
    }
};
struct LightListSystem::Impl
{
    std::shared_ptr<LightListTargets> targets;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<LightListTargets> targets;
        std::array<Import, 4> imports{};
        std::uint32_t slot{};
    } pending;
};
LightListSystem::LightListSystem() : impl_(std::make_unique<Impl>()) {}
LightListSystem::~LightListSystem() = default;
std::uint64_t LightListCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t LightListSystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }

Result LightListSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ViewExtent capacity, std::uint32_t maxLights, std::uint32_t slots,
    const ShaderReader &readShader, LightListCandidate &candidate, std::uint32_t clusterCapacity) const
{
    if (!device || !allocator || !ValidExtent(capacity) || !maxLights || !slots || !readShader ||
        !clusterCapacity || clusterCapacity > 128 ||
        slots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / kDescriptorCount)
        return LightFailure(ErrorCode::InvalidArgument, "Invalid light-list device, capacity, light count, frame slots or shader reader.");
    const std::uint64_t cells = UINT64((capacity.width + kTilePixels - 1) / kTilePixels) *
        ((capacity.height + kTilePixels - 1) / kTilePixels) * kSlices;
    // Overflow keeps the existing exact world query. Quality changes the real
    // cluster backing and fast-path capacity without discarding accepted lights.
    const auto inlineCapacity = std::min(clusterCapacity, maxLights);
    const auto spillCapacity = std::min(clusterCapacity, maxLights - inlineCapacity);
    const std::array<std::uint64_t, 4> widths{UINT64(maxLights) * sizeof(GpuLightRecord),
        kHeaderBytes + cells * 16, std::max(UINT64(4), cells * inlineCapacity * 4),
        std::max(UINT64(4), cells * spillCapacity * 4)};
    const std::uint64_t uploadStride = (kHeaderBytes + widths[0] + 255) & ~UINT64(255);
    if (std::ranges::any_of(widths, [](std::uint64_t size) { return size > kMaxBufferBytes; }) ||
        uploadStride > std::numeric_limits<SIZE_T>::max() / slots)
        return LightFailure(ErrorCode::InvalidArgument, "Light-list capacity exceeds raw/index buffer addressing or host upload limits.");
    auto next = std::make_shared<LightListTargets>();
    next->device = device; next->allocator = allocator; next->capacity = capacity;
    next->max_lights = maxLights; next->inline_capacity = inlineCapacity; next->spill_capacity = spillCapacity;
    next->upload_stride = uploadStride; next->frames.resize(slots);
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (auto &frame : next->frames)
        for (std::uint32_t index = 0; index < widths.size(); ++index)
        {
            if (auto result = CreateBuffer(allocator, widths[index], false, index != 0, frame.buffers[index]); !result) return result;
            next->bytes += frame.buffers[index].allocation->GetSize();
        }
    if (auto result = CreateBuffer(allocator, uploadStride * slots, true, false, next->upload); !result) return result;
    next->bytes += next->upload.allocation->GetSize();
    void *mapped{}; D3D12_RANGE noRead{0, 0};
    auto native = next->upload.resource->Map(0, &noRead, &mapped);
    if (FAILED(native)) return NativeFailure("Map light-list snapshots", native);
    next->mapped = static_cast<std::byte *>(mapped);

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = slots * kDescriptorCount;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->descriptors));
    if (FAILED(native)) return NativeFailure("Create light-list compute descriptors", native);
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->export_descriptors));
    if (FAILED(native)) return NativeFailure("Create light-list exported descriptors", native);
    for (std::uint32_t slot = 0; slot < slots; ++slot)
    {
        const auto &buffers = next->frames[slot].buffers;
        CreateSrv(device, buffers[0].resource.Get(), sizeof(GpuLightRecord), next->Cpu(slot, 0));
        for (std::uint32_t index = 0; index < 4; ++index)
        {
            const std::uint32_t stride = index == 0 ? sizeof(GpuLightRecord) : index == 1 ? 0 : sizeof(std::uint32_t);
            CreateSrv(device, buffers[index].resource.Get(), stride, next->Cpu(slot, index, true));
            if (index) CreateUav(device, buffers[index].resource.Get(), stride, next->Cpu(slot, index));
        }
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 19, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 19, 1}};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[0].Descriptor = {0, 19};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = UINT(std::size(parameters)); description.pParameters = parameters;
    ComPtr<ID3DBlob> signature, diagnostics;
    native = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics);
    if (FAILED(native))
    {
        auto result = NativeFailure("Serialize light-list root signature", native);
        if (diagnostics && diagnostics->GetBufferSize())
            return LightFailure(ErrorCode::TaskFailed, std::string(result.Message()) + " " +
                std::string(static_cast<const char *>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()));
        return result;
    }
    native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&next->root));
    if (FAILED(native)) return NativeFailure("Create light-list root signature", native);
    std::vector<std::byte> bytecode;
    if (auto result = readShader("render/shader/light_list_cs.dxil", bytecode); !result) return result;
    if (bytecode.empty()) return LightFailure(ErrorCode::TaskFailed, "Light-list shader product is empty.");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = next->root.Get(); pipeline.CS = {bytecode.data(), bytecode.size()};
    native = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&next->pipeline));
    if (FAILED(native)) return NativeFailure("Create light-list pipeline", native);
    candidate.targets_ = std::move(next);
    return Result::Success();
}

Result LightListSystem::Commit(LightListCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return LightFailure(ErrorCode::InvalidState, "Complete registered light-list graph before replacing targets.");
    if (!candidate.targets_) return LightFailure(ErrorCode::InvalidArgument, "Light-list candidate was not prepared.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return LightFailure(ErrorCode::InvalidState, "Old light-list snapshots remain in GPU use.");
    impl_->targets = std::move(candidate.targets_);
    return Result::Success();
}

Result LightListSystem::Register(RenderGraphBuilder &graph, const LightListInputs &inputs, LightListOutputs &outputs)
{
    outputs = {};
    auto storage = impl_->targets;
    if (!storage || impl_->pending.graph || !storage->usable)
        return LightFailure(ErrorCode::InvalidState, "Light-list targets are absent, pending or a previous graph failed.");
    if (!ValidExtent(inputs.render_active) || inputs.render_active.width > storage->capacity.width ||
        inputs.render_active.height > storage->capacity.height || inputs.frame_slot >= storage->frames.size() ||
        !storage->frames[inputs.frame_slot].completion.IsSatisfiedBy(inputs.completed) ||
        inputs.lights.size() > storage->max_lights)
        return LightFailure(ErrorCode::InvalidArgument, "Invalid light-list active extent, completed frame slot or light capacity; inputs are never truncated.");
    if (!Finite(inputs.unjittered_view) || !Finite(inputs.unjittered_projection) ||
        !Finite(Float3{inputs.camera_origin.x, inputs.camera_origin.y, inputs.camera_origin.z}) ||
        !std::isfinite(inputs.near_plane) || !std::isfinite(inputs.far_plane) ||
        inputs.near_plane <= 0 || inputs.far_plane <= inputs.near_plane)
        return LightFailure(ErrorCode::InvalidArgument, "Light-list camera transforms and physical depth interval must be finite.");
    auto rotation = inputs.unjittered_view;
    rotation._41 = rotation._42 = rotation._43 = 0; rotation._44 = 1;
    // The range sphere remains conservative only under a rigid camera rotation.
    for (std::uint32_t row = 0; row < 3; ++row)
        for (std::uint32_t other = row; other < 3; ++other)
        {
            float product{};
            for (std::uint32_t column = 0; column < 3; ++column) product += rotation.m[row][column] * rotation.m[other][column];
            if (std::abs(product - (row == other ? 1.0f : 0.0f)) > 0.001f)
                return LightFailure(ErrorCode::InvalidArgument, "Light-list view transform must contain a rigid camera rotation.");
        }
    const auto &projection = inputs.unjittered_projection;
    if (rotation._14 != 0 || rotation._24 != 0 || rotation._34 != 0 ||
        projection._14 != 0 || projection._24 != 0 || projection._34 != 1 || projection._44 != 0)
        return LightFailure(ErrorCode::InvalidArgument, "Light-list grid requires the renderer's positive-depth perspective camera.");
    const auto projectionMatrix = DirectX::XMLoadFloat4x4(&projection);
    DirectX::XMVECTOR determinant;
    const auto inverseProjection = DirectX::XMMatrixInverse(&determinant, projectionMatrix);
    DirectX::XMFLOAT4X4 inverse;
    DirectX::XMStoreFloat4x4(&inverse, inverseProjection);
    if (!Finite(inverse) || !std::isfinite(DirectX::XMVectorGetX(determinant)) || DirectX::XMVectorGetX(determinant) == 0)
        return LightFailure(ErrorCode::InvalidArgument, "Light-list projection must be invertible.");
    for (float y : {-1.0f, 1.0f})
        for (float x : {-1.0f, 1.0f})
        {
            const auto ray = DirectX::XMVector4Transform(DirectX::XMVectorSet(x, y, 1, 1), inverseProjection);
            const float z = DirectX::XMVectorGetZ(ray), w = DirectX::XMVectorGetW(ray);
            if (!std::isfinite(z) || !std::isfinite(w) || w == 0 || z / w <= 0)
                return LightFailure(ErrorCode::InvalidArgument, "Light-list perspective corners must have positive physical view depth.");
        }

    std::vector<const LightView *> sorted;
    sorted.reserve(inputs.lights.size());
    for (const auto &light : inputs.lights) sorted.push_back(&light);
    std::ranges::sort(sorted, [](const auto *left, const auto *right) { return left->stable_id < right->stable_id; });
    for (std::size_t index = 1; index < sorted.size(); ++index)
        if (sorted[index - 1]->stable_id == sorted[index]->stable_id)
            return LightFailure(ErrorCode::InvalidArgument, "A light snapshot contains duplicate stable IDs.");
    std::vector<std::uint64_t> transient(inputs.transient_light_ids.begin(), inputs.transient_light_ids.end());
    std::ranges::sort(transient);
    for (std::size_t index = 0; index < transient.size(); ++index)
    {
        const auto id = transient[index];
        if (!id || (index && transient[index - 1] == id) ||
            !std::ranges::binary_search(sorted, id, {}, [](const LightView *light) { return light->stable_id; }))
            return LightFailure(ErrorCode::InvalidArgument, "Transient light IDs must be unique exact identities in the light snapshot.");
    }
    std::stable_partition(sorted.begin(), sorted.end(), [](const auto *light) { return light->type == LightType::Directional; });
    std::vector<GpuLightRecord> records(std::max(std::size_t(1), sorted.size()));
    std::uint32_t directionalCount{};
    bool primaryShadowAssigned{};
    for (std::size_t index = 0; index < sorted.size(); ++index)
    {
        if (auto result = EncodeLight(*sorted[index], records[index]); !result) return result;
        if (std::ranges::binary_search(transient, sorted[index]->stable_id)) records[index].metadata.w |= 8u;
        if (sorted[index]->type == LightType::Directional)
        {
            ++directionalCount;
            if (!primaryShadowAssigned && (sorted[index]->shadow_flags & kLightShadowRequested))
            {
                records[index].metadata.w |= 2u;
                primaryShadowAssigned = true;
            }
        }
    }
    LightListConstants constants{};
    DirectX::XMStoreFloat4x4(&constants.world_to_view, DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&rotation)));
    DirectX::XMStoreFloat4x4(&constants.projection, DirectX::XMMatrixTranspose(projectionMatrix));
    DirectX::XMStoreFloat4x4(&constants.inverse_projection, DirectX::XMMatrixTranspose(inverseProjection));
    constants.camera_origin_near = {inputs.camera_origin.x, inputs.camera_origin.y, inputs.camera_origin.z, inputs.near_plane};
    const float logarithmicRange = float(std::log2(double(inputs.far_plane) / inputs.near_plane));
    if (!std::isfinite(logarithmicRange) || logarithmicRange <= 0)
        return LightFailure(ErrorCode::InvalidArgument, "Light-list logarithmic depth interval is not representable.");
    constants.extent_far_log = {float(inputs.render_active.width), float(inputs.render_active.height), inputs.far_plane, logarithmicRange};
    constants.grid = {(inputs.render_active.width + kTilePixels - 1) / kTilePixels,
        (inputs.render_active.height + kTilePixels - 1) / kTilePixels, kSlices, directionalCount};
    constants.limits = {UINT(sorted.size()), storage->inline_capacity, storage->spill_capacity,
        constants.grid.x * constants.grid.y * kSlices};
    const auto slot = inputs.frame_slot;
    const auto uploadOffset = UINT64(slot) * storage->upload_stride;
    std::memcpy(storage->mapped + uploadOffset, &constants, sizeof(constants));
    std::memcpy(storage->mapped + uploadOffset + kHeaderBytes, records.data(), records.size() * sizeof(GpuLightRecord));
    auto &frame = storage->frames[slot];
    Impl::Pending pending; pending.graph = &graph; pending.targets = storage; pending.slot = slot;
    std::array<ResourceVersion, 4> versions;
    constexpr std::array<std::string_view, 4> names{"LightList.Records", "LightList.GridAndRanges", "LightList.Inline", "LightList.Spill"};
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        auto &resource = frame.buffers[index];
        const auto handle = graph.ImportBuffer({resource.resource.Get(), resource.graph_completion, resource.graph_queue}, resource.graph_access, names[index]);
        pending.imports[index] = {&resource, handle}; versions[index] = graph.Version(handle);
    }
    auto upload = graph.AddPass(130, "LightListUpload", QueueHint::Direct);
    versions[0] = upload.Write(versions[0], Access::CopyDest);
    versions[1] = upload.Write(versions[1], Access::CopyDest);
    upload.SetExecuteChecked([storage, slot, uploadOffset, recordBytes = UINT64(records.size()) * sizeof(GpuLightRecord)](RenderPassContext &context) -> Result {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        const auto &buffers = storage->frames[slot].buffers;
        commands->CopyBufferRegion(buffers[0].resource.Get(), 0, storage->upload.resource.Get(), uploadOffset + kHeaderBytes, recordBytes);
        commands->CopyBufferRegion(buffers[1].resource.Get(), 0, storage->upload.resource.Get(), uploadOffset, kHeaderBytes);
        return Result::Success();
    });
    auto cluster = graph.AddPass(131, "LightListClusters", QueueHint::Direct);
    cluster.Read(versions[0], Access::ComputeRead);
    versions[1] = cluster.ReadWrite(versions[1], Access::UnorderedWrite);
    versions[2] = cluster.Write(versions[2], Access::UnorderedWrite);
    versions[3] = cluster.Write(versions[3], Access::UnorderedWrite);
    cluster.SetExecuteChecked([storage, slot, uploadOffset, cells = constants.limits.w](RenderPassContext &context) -> Result {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()}; commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(storage->root.Get()); commands->SetPipelineState(storage->pipeline.Get());
        commands->SetComputeRootConstantBufferView(0, storage->upload.resource->GetGPUVirtualAddress() + uploadOffset);
        commands->SetComputeRootDescriptorTable(1, storage->Gpu(slot));
        const std::uint32_t groups = (cells + 63) / 64;
        commands->Dispatch(std::min(groups, 65535u), (groups + 65534u) / 65535u, 1);
        return Result::Success();
    });
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        outputs.resources[index] = frame.buffers[index].resource.Get(); outputs.srvs[index] = storage->Cpu(slot, index, true);
        graph.SetFinalAccess(versions[index], Access::AllShaderRead);
    }
    outputs.lights = versions[0]; outputs.ranges = versions[1];
    outputs.inline_indices = versions[2]; outputs.spill_indices = versions[3];
    outputs.light_count = UINT(sorted.size()); outputs.directional_count = directionalCount;
    impl_->pending = std::move(pending);
    return Result::Success();
}

Result LightListSystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph) return LightFailure(ErrorCode::InvalidState, "Light-list completion has no registered graph.");
    if (pending.graph != &graph) return LightFailure(ErrorCode::InvalidArgument, "Light-list completion refers to a different graph.");
    auto storage = pending.targets;
    storage->completion.Merge(completion); storage->frames[pending.slot].completion.Merge(completion);
    storage->upload.graph_completion.Merge(completion);
    for (const auto &imported : pending.imports)
    {
        const auto resourceCompletion = graph.ResourceCompletion(imported.handle);
        storage->completion.Merge(resourceCompletion); storage->frames[pending.slot].completion.Merge(resourceCompletion);
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
Result LightListSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return LightFailure(ErrorCode::InvalidState, "Complete pending light-list graph before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return LightFailure(ErrorCode::InvalidState, "Light-list reset requires completed GPU uses.");
    impl_->targets.reset();
    return Result::Success();
}
} // namespace hs
