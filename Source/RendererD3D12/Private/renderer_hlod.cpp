#include "renderer_hlod.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kSrvCount = 11, kUavCount = 8, kTable = kSrvCount + kUavCount, kStages = 5;
constexpr std::uint32_t kConstantStride = 768;
enum Buffer : std::uint32_t { Members, Bindings, Buckets, Choice, Leaves, Selection, Indexed, Mesh, Counts, Demand, Status, BufferCount };
struct GpuGroup
{
    std::uint32_t parent{}, first_member{}, member_count{}, first_proxy{};
    std::uint32_t proxy_count{}, first_child{}, child_count{}, ancestor_steps{};
    std::array<float, 4> sphere{}, error{};
};
struct GpuMember
{
    std::uint32_t stable_low{}, stable_high{}, leaf_group{}, instance_slot{UINT32_MAX};
    std::uint32_t instance_generation{}, content_generation{}, object_low{}, object_high{};
};
struct GpuProxy
{
    std::uint32_t group{}, logical_page{}, first_index{}, index_count{};
    std::uint32_t first_meshlet{}, meshlet_count{}, mesh_semantic{}, atlas_selector{};
    std::uint32_t tint{}, variant{}, geometry_flags{}, reserved{};
};
struct alignas(16) Constants
{
    DirectX::XMUINT4 counts{}, limits{}, identity{}, options{};
    std::array<GpuHlodView, 4> views;
};
static_assert(sizeof(GpuGroup) == 64 && sizeof(GpuMember) == 32 && sizeof(GpuProxy) == 48);
static_assert(sizeof(Constants) == 640 && offsetof(Constants, views) == 64);
static_assert(offsetof(GpuHlodProxyBinding, geometry_record_id) == 16);
static_assert(offsetof(GpuHlodProxyBinding, instance_slot) == 32);
Result Failure(ErrorCode code, std::string text)
{
    return Result::Failure(code, "hs_renderer_hlod", std::move(text));
}
Result Native(std::string_view name, HRESULT error)
{
    return Failure(ErrorCode::TaskFailed, std::string(name) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
}
std::uint64_t Align(std::uint64_t value) { return (value + 255u) & ~UINT64(255); }
bool ExternalShape(const HlodExternalBuffer &b, std::uint64_t minimum)
{
    if(!b.resource||(b.contract&&b.contract->native_resource!=b.resource))return false;
    const auto description=b.contract?b.contract->description:b.resource->GetDesc();
    return description.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&description.Width>=minimum;
}
bool ValidView(const GpuHlodView &view)
{
    for (const auto &value : {view.origin_near, view.forward, view.error_policy})
        for (const auto channel : {value.x, value.y, value.z, value.w}) if (!std::isfinite(channel)) return false;
    for (const auto &plane : view.frustum_planes)
        for (const auto channel : {plane.x, plane.y, plane.z, plane.w}) if (!std::isfinite(channel)) return false;
    const double norm = double(view.forward.x) * view.forward.x + double(view.forward.y) * view.forward.y +
        double(view.forward.z) * view.forward.z;
    return view.origin_near.w > 0 && std::abs(norm - 1) < 0.001 && view.error_policy.x > 0 &&
        view.error_policy.y > 0 && view.error_policy.z >= 0 && view.error_policy.z < 1 &&
        (view.error_policy.w == 0 || view.error_policy.w == 1);
}
Result BufferResource(D3D12MA::Allocator *allocator, std::uint64_t bytes, bool upload,
                      bool uav, AllocationResource &output)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; description.Width = bytes;
    description.Height = 1; description.DepthOrArraySize = 1; description.MipLevels = 1;
    description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    const auto result = allocator->CreateResource(&allocation, &description,
        upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON, nullptr,
        &output.allocation, IID_PPV_ARGS(output.resource.ReleaseAndGetAddressOf()));
    if (FAILED(result)) return Native("Create HLOD metadata/scratch buffer", result);
    output.graph_access = upload ? Access::CopySource : Access::Common;
    return Result::Success();
}
void Srv(ID3D12Device *device, ID3D12Resource *resource, std::uint32_t stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
    view.Buffer.StructureByteStride = stride; view.Buffer.Flags = stride ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW;
    view.Buffer.NumElements = resource ? UINT(resource->GetDesc().Width / (stride ? stride : 4u)) : 1u;
    device->CreateShaderResourceView(resource, &view, handle);
}
void Uav(ID3D12Device *device, ID3D12Resource *resource, std::uint32_t stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; view.Format = stride ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
    view.Buffer.StructureByteStride = stride; view.Buffer.Flags = stride ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW;
    view.Buffer.NumElements = UINT(resource->GetDesc().Width / (stride ? stride : 4u));
    device->CreateUnorderedAccessView(resource, nullptr, &view, handle);
}
void Dispatch(ID3D12GraphicsCommandList *list, std::uint32_t elements)
{
    const auto groups = (UINT64(elements) + 63u) / 64u;
    if (groups) list->Dispatch(UINT(std::min<UINT64>(groups, 65535u)), UINT((groups + 65534u) / 65535u), 1u);
}
} // namespace

void HlodSlotSet::Resize(std::uint32_t capacity)
{
    capacity_ = capacity;
    words_.resize((std::uint64_t(capacity) + 63u) / 64u);
    Clear();
}
void HlodSlotSet::Clear() noexcept
{
    std::fill(words_.begin(), words_.end(), 0u);
}
bool HlodSlotSet::Insert(std::uint32_t slot) noexcept
{
    if (slot >= capacity_) return false;
    const auto mask = std::uint64_t{1} << (slot & 63u);
    auto &word = words_[slot >> 6u];
    if (word & mask) return false;
    word |= mask;
    return true;
}
bool HlodSlotSet::Contains(std::uint32_t slot) const noexcept
{
    return slot < capacity_ && (words_[slot >> 6u] & (std::uint64_t{1} << (slot & 63u))) != 0;
}

struct HlodStorage
{
    struct Frame
    {
        std::array<AllocationResource, BufferCount> buffers;
        CompletionSet completion;
        std::uint64_t frame_id{}, static_generation{}, history_generation{};
    };
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, kStages> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors, exports;
    AllocationResource groups, proxies, upload;
    std::vector<GpuMember> members_cpu;
    std::vector<Frame> frames;
    render_data::SceneHlod scene;
    std::string content_id;
    std::uint32_t generation{}, slot_capacity{}, descriptor_stride{}, maximum_depth{1};
    HlodSlotSet member_slot_bits, command_ordinals;
    std::uint32_t history_slot{UINT32_MAX};
    std::uint64_t bytes{}, metadata_bytes{}, upload_stride{};
    std::array<std::uint64_t, 3> snapshot_offsets{};
    CompletionSet completion;
    std::byte *mapped{};
    bool metadata_ready{}, usable{true};
    ~HlodStorage() { if (mapped && upload.resource) upload.resource->Unmap(0, nullptr); }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot, std::uint32_t item) const
    {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slot * kTable + item) * descriptor_stride; return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t slot) const
    {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64(slot * kTable) * descriptor_stride; return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Export(std::uint32_t slot, std::uint32_t item) const
    {
        auto handle = exports->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slot * 2u + item) * descriptor_stride; return handle;
    }
};
struct HlodSystem::Impl
{
    std::shared_ptr<HlodStorage> storage;
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; std::uint32_t owner{UINT32_MAX}; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<HlodStorage> storage;
        std::vector<Import> imports;
        std::uint32_t slot{}, previous{UINT32_MAX};
        std::uint64_t frame_id{}, static_generation{}, history_generation{};
        bool enabled{}, uploaded{};
    } pending;
};
HlodSystem::HlodSystem() : impl_(std::make_unique<Impl>()) {}
HlodSystem::~HlodSystem() = default;
std::uint64_t HlodCandidate::AllocatedBytes() const noexcept { return storage_ ? storage_->bytes : 0; }
std::uint64_t HlodSystem::AllocatedBytes() const noexcept { return impl_->storage ? impl_->storage->bytes : 0; }
CompletionSet HlodSystem::FrameCompletion(std::uint32_t slot) const noexcept
{
    return impl_->storage && slot < impl_->storage->frames.size() ? impl_->storage->frames[slot].completion : CompletionSet{};
}

Result HlodSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    const render_data::SceneHlod &scene, std::string_view content_id, std::uint32_t generation,
    std::uint32_t slot_capacity, std::uint32_t frame_slots, const ShaderReader &read_shader, HlodCandidate &candidate) const
{
    if (!device || !allocator || content_id.empty() || content_id.size() > 256 || !generation || !slot_capacity ||
        slot_capacity > 1'000'000 || !frame_slots || frame_slots > 8 || !read_shader)
        return Failure(ErrorCode::InvalidArgument, "Invalid HLOD device, identity, slot capacity or shader reader.");
    try { render_data::ValidateSceneHlod(scene); }
    catch (const std::exception &error) { return Failure(ErrorCode::InvalidArgument, error.what()); }
    auto next = std::make_shared<HlodStorage>();
    next->device = device; next->allocator = allocator; next->scene = scene; next->content_id = content_id;
    next->generation = generation; next->slot_capacity = slot_capacity; next->frames.resize(frame_slots);
    next->member_slot_bits.Resize(slot_capacity);
    next->command_ordinals.Resize(static_cast<std::uint32_t>(scene.proxies.size()));
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<GpuGroup> groups(scene.groups.size()); std::vector<GpuProxy> proxies(scene.proxies.size());
    next->members_cpu.resize(scene.members.size());
    for (std::uint32_t id = 0; id < scene.groups.size(); ++id)
    {
        const auto &source = scene.groups[id]; auto &target = groups[id];
        target.parent = source.parent; target.first_member = source.first_member; target.member_count = source.member_count;
        target.first_proxy = source.first_proxy; target.proxy_count = source.proxy_count;
        target.first_child = source.first_child; target.child_count = source.child_count;
        target.sphere = source.bounds.sphere; target.error[0] = source.world_error;
        auto ancestor = id;
        do { ++target.ancestor_steps; ancestor = scene.groups[ancestor].parent; } while (ancestor != render_data::kNoSceneHlodGroup);
        next->maximum_depth = std::max(next->maximum_depth, target.ancestor_steps);
        if (!source.child_count)
            for (auto m = source.first_member; m < source.first_member + source.member_count; ++m) next->members_cpu[m].leaf_group = id;
        for (auto p = source.first_proxy; p < source.first_proxy + source.proxy_count; ++p) proxies[p].group = id;
    }
    for (std::uint32_t id = 0; id < scene.proxies.size(); ++id)
    {
        const auto &source = scene.proxies[id]; auto &target = proxies[id];
        target.logical_page = source.page_index; target.first_index = source.first_index; target.index_count = source.index_count;
        target.first_meshlet = source.first_meshlet; target.meshlet_count = source.meshlet_count;
        target.mesh_semantic = source.mesh_semantic; target.atlas_selector = source.atlas_selector;
        target.tint = source.tint_rgba; target.variant = source.variant; target.geometry_flags = scene.pages[source.page_index].geometry_flags;
    }
    for (std::uint32_t id = 0; id < scene.members.size(); ++id)
    {
        const auto &source = scene.members[id]; auto &target = next->members_cpu[id];
        target.stable_low = std::uint32_t(source.stable_id); target.stable_high = std::uint32_t(source.stable_id >> 32u);
        target.object_low = std::uint32_t(source.object_id); target.object_high = std::uint32_t(source.object_id >> 32u);
        target.content_generation = generation;
    }
    const auto group_bytes = std::max(UINT64(64), UINT64(groups.size()) * sizeof(GpuGroup));
    const auto proxy_bytes = std::max(UINT64(48), UINT64(proxies.size()) * sizeof(GpuProxy));
    const std::array<UINT64, BufferCount> widths{
        std::max(UINT64(32), UINT64(scene.members.size()) * 32), std::max(UINT64(48), UINT64(scene.proxies.size()) * 48),
        std::max(UINT64(16), UINT64(scene.proxies.size()) * 16), std::max(UINT64(16), UINT64(scene.groups.size()) * 16),
        UINT64(slot_capacity) * 16, std::max(UINT64(16), UINT64(scene.proxies.size()) * 16),
        std::max(UINT64(4), UINT64(scene.proxies.size()) * 4 * 24), std::max(UINT64(4), UINT64(scene.proxies.size()) * 4 * 16),
        std::max(UINT64(4), UINT64(scene.proxies.size()) * 4 * 4), std::max(UINT64(4), UINT64(scene.pages.size()) * 4), UINT64(64)};
    if (auto result = BufferResource(allocator, group_bytes, false, false, next->groups); !result) return result;
    if (auto result = BufferResource(allocator, proxy_bytes, false, false, next->proxies); !result) return result;
    next->bytes += next->groups.allocation->GetSize() + next->proxies.allocation->GetSize();
    for (auto &frame : next->frames)
        for (std::uint32_t i = 0; i < BufferCount; ++i)
        {
            if (auto result = BufferResource(allocator, widths[i], false, i >= Choice, frame.buffers[i]); !result) return result;
            next->bytes += frame.buffers[i].allocation->GetSize();
        }
    next->metadata_bytes = Align(group_bytes) + Align(proxy_bytes);
    next->snapshot_offsets = {kConstantStride, kConstantStride + widths[Members], kConstantStride + widths[Members] + widths[Bindings]};
    next->upload_stride = Align(next->snapshot_offsets[2] + widths[Buckets]);
    if (auto result = BufferResource(allocator, next->metadata_bytes + next->upload_stride * frame_slots, true, false, next->upload); !result) return result;
    next->bytes += next->upload.allocation->GetSize();
    void *mapped{}; D3D12_RANGE no_read{0, 0};
    auto native = next->upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(native)) return Native("Map HLOD snapshots", native);
    next->mapped = static_cast<std::byte *>(mapped);
    std::memset(next->mapped, 0, SIZE_T(next->metadata_bytes + next->upload_stride * frame_slots));
    if (!groups.empty()) std::memcpy(next->mapped, groups.data(), groups.size() * sizeof(GpuGroup));
    if (!proxies.empty()) std::memcpy(next->mapped + Align(group_bytes), proxies.data(), proxies.size() * sizeof(GpuProxy));
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = frame_slots * kTable;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->descriptors));
    if (FAILED(native)) return Native("Create HLOD stage descriptor banks", native);
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; heap.NumDescriptors = frame_slots * 2;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->exports));
    if (FAILED(native)) return Native("Create HLOD export descriptors", native);
    for (std::uint32_t slot = 0; slot < frame_slots; ++slot)
    {
        const auto &frame = next->frames[slot];
        const std::array<ID3D12Resource *, 5> owned_srvs{next->groups.resource.Get(), frame.buffers[Members].resource.Get(),
            next->proxies.resource.Get(), frame.buffers[Bindings].resource.Get(), frame.buffers[Buckets].resource.Get()};
        constexpr std::array<std::uint32_t, 5> strides{64, 32, 48, 48, 16};
        for (std::uint32_t i = 0; i < owned_srvs.size(); ++i)
            Srv(device, owned_srvs[i], strides[i], next->Cpu(slot, i));
        constexpr std::array<std::uint32_t, kUavCount> buffers{Choice, Leaves, Selection, Indexed, Mesh, Counts, Demand, Status};
        constexpr std::array<std::uint32_t, kUavCount> uav_strides{16, 16, 16, 0, 0, 0, 4, 0};
        for (std::uint32_t i = 0; i < kUavCount; ++i)
            Uav(device, frame.buffers[buffers[i]].resource.Get(), uav_strides[i], next->Cpu(slot, kSrvCount + i));
        Srv(device, next->frames[slot].buffers[Leaves].resource.Get(), 16, next->Export(slot, 0));
        Srv(device, next->frames[slot].buffers[Selection].resource.Get(), 16, next->Export(slot, 1));
    }
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 25, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0, 25, kSrvCount}};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[0].Descriptor = {0, 25};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; parameters[1].DescriptorTable = {2, ranges};
    D3D12_ROOT_SIGNATURE_DESC description{}; description.NumParameters = 2; description.pParameters = parameters;
    ComPtr<ID3DBlob> signature, diagnostics;
    native = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics);
    if (FAILED(native))
    {
        const auto text = diagnostics ? std::string(static_cast<const char *>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()) : "";
        return Failure(ErrorCode::TaskFailed, "Serialize HLOD root signature failed: " + text);
    }
    native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&next->root));
    if (FAILED(native)) return Native("Create HLOD compute root", native);
    constexpr std::array<std::string_view, kStages> products{"render/shader/scene_hlod_clear_cs.dxil",
        "render/shader/scene_hlod_select_cs.dxil", "render/shader/scene_hlod_emit_cs.dxil",
        "render/shader/scene_hlod_publish_cs.dxil", "render/shader/scene_hlod_history_cs.dxil"};
    for (std::uint32_t stage = 0; stage < kStages; ++stage)
    {
        std::vector<std::byte> bytecode;
        if (auto result = read_shader(products[stage], bytecode); !result) return result;
        if (bytecode.empty()) return Failure(ErrorCode::TaskFailed, "HLOD shader product is empty: " + std::string(products[stage]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = next->root.Get(); pipeline.CS = {bytecode.data(), bytecode.size()};
        native = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&next->pipelines[stage]));
        if (FAILED(native)) return Native("Create HLOD selection pipeline", native);
    }
    candidate.storage_ = std::move(next); return Result::Success();
}

Result HlodSystem::Commit(HlodCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete pending HLOD graph before replacement.");
    if (!candidate.storage_) return Failure(ErrorCode::InvalidArgument, "HLOD candidate was not prepared.");
    if (impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "Old HLOD snapshots remain in GPU use.");
    impl_->storage = std::move(candidate.storage_); return Result::Success();
}

Result HlodSystem::PrepareFrameRecordData(const HlodFrameInputs &inputs,HlodFrameRecordData &prepared)
{
    prepared = {}; auto storage = impl_->storage;
    if (!storage || impl_->pending.graph || !storage->usable)
        return Failure(ErrorCode::InvalidState, "HLOD storage is absent, pending or a previous submission failed.");
    if (inputs.frame_slot >= storage->frames.size() || !inputs.rendered_frame_id ||
        !storage->frames[inputs.frame_slot].completion.IsSatisfiedBy(inputs.completed) || !ExternalShape(inputs.static_scene, 64))
        return Failure(ErrorCode::InvalidArgument, "HLOD registration requires a completed frame slot and current static scene table.");
    const auto &scene = storage->scene;
    const bool read_geometry=ExternalShape(inputs.geometry_records,64),read_refs=ExternalShape(inputs.proxy_refs,16);
    bool enabled = inputs.layout_abi == scene.layout_abi && inputs.layout_digest == scene.layout_digest &&
        inputs.content_generation_id == storage->content_id && inputs.static_scene_generation != 0 &&
        inputs.members.size() == scene.members.size() && inputs.proxies.size() == scene.proxies.size() &&
        inputs.buckets.size() <= scene.proxies.size() && (inputs.active_view_mask & ~15u) == 0u &&
        (scene.proxies.empty() || (read_geometry && read_refs && !inputs.buckets.empty()));
    for (std::uint32_t view = 0; view < 4; ++view)
        if ((inputs.active_view_mask & (1u << view)) && !ValidView(inputs.views[view])) enabled = false;
    const auto bucket_count = inputs.buckets.size() <= scene.proxies.size() ? UINT(inputs.buckets.size()) : 0u;
    storage->member_slot_bits.Clear();
    if (inputs.members.size() == scene.members.size())
        for (std::uint32_t m = 0; m < inputs.members.size(); ++m)
        {
            const auto &binding = inputs.members[m];
            if (binding.stable_id != scene.members[m].stable_id || !binding.instance_generation || !storage->member_slot_bits.Insert(binding.instance_slot)) enabled = false;
        }
    storage->command_ordinals.Clear();
    std::uint32_t next_command = 0;
    if (bucket_count)
        for (const auto &bucket : inputs.buckets)
        {
            if (bucket.first_command != next_command || !bucket.command_count || bucket.reserved ||
                next_command > scene.proxies.size() || bucket.command_count > scene.proxies.size() - next_command)
            { enabled = false; break; }
            next_command += bucket.command_count;
        }
    if (next_command != scene.proxies.size()) enabled = false;
    if (inputs.proxies.size() == scene.proxies.size())
        for (std::uint32_t p = 0; p < inputs.proxies.size(); ++p)
        {
            const auto &binding = inputs.proxies[p];
            if (binding.logical_page != scene.proxies[p].page_index || binding.command_ordinal >= scene.proxies.size() ||
                binding.bucket_index >= bucket_count || binding.reserved || (binding.flags & ~kHlodProxyResident))
            { enabled = false; continue; }
            if (!storage->command_ordinals.Insert(binding.command_ordinal)) enabled = false;
            const auto &bucket = inputs.buckets[binding.bucket_index];
            if (binding.command_ordinal < bucket.first_command ||
                binding.command_ordinal - bucket.first_command >= bucket.command_count) enabled = false;
            if ((binding.flags & kHlodProxyResident) &&
                (binding.content_generation != storage->generation || !binding.page_version ||
                 !binding.geometry_record_id || binding.geometry_record_id > 0x3fffffffu ||
                 binding.instance_slot >= storage->slot_capacity || !binding.instance_generation ||
                 storage->member_slot_bits.Contains(binding.instance_slot))) enabled = false;
        }
    bool history = enabled && inputs.history_valid && storage->history_slot != UINT32_MAX && storage->history_slot != inputs.frame_slot;
    if (history)
    {
        const auto &previous = storage->frames[storage->history_slot];
        history = previous.frame_id != UINT64_MAX && inputs.rendered_frame_id == previous.frame_id + 1 &&
            previous.static_generation == inputs.static_scene_generation && previous.history_generation == inputs.history_generation;
    }
    prepared.owner=storage.get();prepared.inputs=&inputs;
    prepared.rendered_frame_id=inputs.rendered_frame_id;
    prepared.enabled=enabled;prepared.history=history;prepared.upload=!storage->metadata_ready;
    prepared.read_geometry=read_geometry;prepared.read_refs=read_refs;
    prepared.slot=inputs.frame_slot;prepared.previous=history?storage->history_slot:UINT32_MAX;
    prepared.bucket_count=bucket_count;return Result::Success();
}

bool HlodSystem::MatchesLayout(const HlodFrameInputs &inputs,const HlodFrameRecordData &prepared,const HlodLayout &layout)const noexcept
{
    const auto &s=impl_->storage;
    return layout.valid&&s&&prepared.owner==s.get()&&prepared.inputs==&inputs&&prepared.rendered_frame_id==inputs.rendered_frame_id&&
        layout.history==prepared.history&&layout.upload==prepared.upload&&prepared.upload==!s->metadata_ready&&
        layout.read_geometry==prepared.read_geometry&&layout.read_refs==prepared.read_refs&&
        layout.shape==std::array<std::uint64_t,5>{s->slot_capacity,s->scene.groups.size(),s->scene.proxies.size(),s->scene.pages.size(),s->scene.members.size()}&&
        layout.guides==std::array{inputs.static_scene.version,inputs.geometry_records.version,inputs.proxy_refs.version};
}

Result HlodSystem::Register(RenderGraphBuilder &graph,const HlodFrameInputs &inputs,HlodFrameOutputs &outputs,
    HlodLayout *capture,const HlodFrameRecordData *prepared)
{
    return RegisterFrame(graph,inputs,outputs,capture,nullptr,prepared);
}
Result HlodSystem::BindRetained(RenderGraphBuilder &graph,const HlodFrameInputs &inputs,
    const HlodFrameRecordData &prepared,const HlodLayout &layout,HlodFrameOutputs &outputs)
{
    return RegisterFrame(graph,inputs,outputs,nullptr,&layout,&prepared);
}
Result HlodSystem::RegisterFrame(RenderGraphBuilder &graph,const HlodFrameInputs &inputs,HlodFrameOutputs &outputs,
    HlodLayout *capture,const HlodLayout *retained,const HlodFrameRecordData *prepared)
{
    outputs={};if(capture)*capture={};
    HlodFrameRecordData local;
    if(!prepared){if(auto result=PrepareFrameRecordData(inputs,local);!result)return result;prepared=&local;}
    auto storage=impl_->storage;
    if(!storage||impl_->pending.graph||!storage->usable||prepared->owner!=storage.get()||prepared->inputs!=&inputs||
        prepared->rendered_frame_id!=inputs.rendered_frame_id)
        return Failure(ErrorCode::InvalidState,"HLOD prepared frame no longer matches current inputs or storage.");
    if(retained&&!MatchesLayout(inputs,*prepared,*retained))
        return Failure(ErrorCode::InvalidState,"HLOD retained topology changed.");
    const auto bound=[&](const HlodExternalBuffer &buffer,bool required){
        return !required||(buffer.version.resource&&buffer.version.resource.kind==ResourceKind::Buffer&&
            graph.Resolve(buffer.version.resource)==buffer.resource);
    };
    if(!bound(inputs.static_scene,true)||!bound(inputs.geometry_records,prepared->read_geometry)||
        !bound(inputs.proxy_refs,prepared->read_refs))
        return Failure(ErrorCode::InvalidArgument,"HLOD graph inputs do not match prepared native buffers.");
    const auto &scene=storage->scene;
    const bool enabled=prepared->enabled,history=prepared->history;
    const auto slot=prepared->slot,previous_slot=prepared->previous,bucket_count=prepared->bucket_count;
    auto &frame=storage->frames[slot];
    const auto offset = storage->metadata_bytes + UINT64(slot) * storage->upload_stride;
    // Valid snapshots overwrite every active element; padding/bucket tails are
    // outside shader counts. Keep deterministic zero input for disabled paths.
    if (!enabled) std::memset(storage->mapped + offset, 0, SIZE_T(storage->upload_stride));
    Constants constants;
    constants.counts = {UINT(scene.groups.size()), UINT(scene.members.size()), UINT(scene.proxies.size()), storage->slot_capacity};
    constants.limits = {UINT(scene.pages.size()), bucket_count, UINT(scene.proxies.size()), enabled ? 1u : 0u};
    constants.identity = {storage->generation, UINT(inputs.rendered_frame_id), UINT(inputs.rendered_frame_id >> 32u), history ? 1u : 0u};
    constants.options = {inputs.active_view_mask & 15u, storage->maximum_depth, 0, 0}; constants.views = inputs.views;
    std::memcpy(storage->mapped + offset, &constants, sizeof(constants));
    auto *members = reinterpret_cast<GpuMember *>(storage->mapped + offset + storage->snapshot_offsets[0]);
    if (!storage->members_cpu.empty())
        std::memcpy(members, storage->members_cpu.data(), storage->members_cpu.size() * sizeof(GpuMember));
    if (inputs.members.size() == storage->members_cpu.size())
        for (std::size_t m = 0; m < storage->members_cpu.size(); ++m)
        {
            members[m].instance_slot = inputs.members[m].instance_slot;
            members[m].instance_generation = inputs.members[m].instance_generation;
        }
    if (inputs.proxies.size() == scene.proxies.size() && !inputs.proxies.empty())
        std::memcpy(storage->mapped + offset + storage->snapshot_offsets[1], inputs.proxies.data(), inputs.proxies.size_bytes());
    if (bucket_count) std::memcpy(storage->mapped + offset + storage->snapshot_offsets[2], inputs.buckets.data(), inputs.buckets.size_bytes());
    auto *previous = history ? &storage->frames[previous_slot] : nullptr;
    const std::array<ID3D12Resource *, 6> srvs{
        previous ? previous->buffers[Choice].resource.Get() : nullptr, previous ? previous->buffers[Leaves].resource.Get() : nullptr,
        previous ? previous->buffers[Bindings].resource.Get() : nullptr, inputs.static_scene.resource,
        prepared->read_geometry ? inputs.geometry_records.resource : nullptr,
        prepared->read_refs ? inputs.proxy_refs.resource : nullptr};
    constexpr std::array<std::uint32_t, 6> strides{16, 16, 48, 0, 0, 16};
    for (std::uint32_t i = 0; i < srvs.size(); ++i)
        Srv(storage->device.Get(), srvs[i], strides[i], storage->Cpu(slot, 5 + i));
    Impl::Pending pending; pending.graph = &graph; pending.storage = storage; pending.slot = slot;
    pending.previous = previous_slot; pending.frame_id = inputs.rendered_frame_id;
    pending.static_generation = inputs.static_scene_generation; pending.enabled = enabled; pending.uploaded = !storage->metadata_ready;
    pending.history_generation = inputs.history_generation;
    std::size_t import_index{};
    Result binding_result=Result::Success();
    const auto import = [&](AllocationResource &allocation, std::string_view name, std::uint32_t owner) {
        if(auto checked=allocation.EnsureNativeContract(storage->device.Get());!checked)
        {binding_result=checked;return ResourceVersion{};}
        const auto handle = retained?retained->imports[import_index]:
            graph.ImportBuffer({allocation.resource.Get(), allocation.graph_completion, allocation.graph_queue,{},allocation.NativeContract()}, allocation.graph_access, name);
        if(retained&&!graph.BindBuffer(handle,{allocation.resource.Get(),allocation.graph_completion,allocation.graph_queue,{},allocation.NativeContract()},allocation.graph_access))
            binding_result=Failure(ErrorCode::InvalidState,"HLOD retained buffer binding failed.");
        if(capture)capture->imports[import_index]=handle;
        ++import_index;
        pending.imports.push_back({&allocation, handle, owner}); return graph.Version(handle);
    };
    auto groups = import(storage->groups, "Hlod.Groups", UINT32_MAX), proxies = import(storage->proxies, "Hlod.Proxies", UINT32_MAX);
    std::array<ResourceVersion, BufferCount> versions;
    constexpr std::array<std::string_view, BufferCount> names{"Hlod.Members", "Hlod.PageBindings", "Hlod.Buckets", "Hlod.GroupChoice",
        "Hlod.LeafMasks", "Hlod.ProxySelection", "Hlod.IndexedCommands", "Hlod.MeshCommands", "Hlod.CommandCounts", "Hlod.PageDemand", "Hlod.Status"};
    for (std::uint32_t i = 0; i < BufferCount; ++i) versions[i] = import(frame.buffers[i], names[i], slot);
    std::array<ResourceVersion, 3> previous_versions{};
    if (previous)
    {
        previous_versions[0] = import(previous->buffers[Choice], "Hlod.PreviousGroups", previous_slot);
        previous_versions[1] = import(previous->buffers[Leaves], "Hlod.PreviousLeaves", previous_slot);
        previous_versions[2] = import(previous->buffers[Bindings], "Hlod.PreviousBindings", previous_slot);
    }
    if(!binding_result)return binding_result;
    const auto pass_binding=[&](std::size_t index,std::uint32_t id,std::string_view name,QueueHint queue)->std::optional<PassBuilder>{
        if(retained)return graph.BindPass(retained->passes[index],id);
        if(capture)capture->passes[index]=graph.PassCount();
        return graph.AddPass(id,name,queue);
    };
    auto uploadBinding=pass_binding(0,190,"HlodUpload",QueueHint::Copy);
    if(!uploadBinding)return Failure(ErrorCode::InvalidState,"HLOD retained upload pass is missing.");
    auto &upload=*uploadBinding;
    if(retained)versions=retained->outputs;
    else
    {
        if(pending.uploaded){groups=upload.Write(groups,Access::CopyDest);proxies=upload.Write(proxies,Access::CopyDest);}
        for(const auto i:{Members,Bindings,Buckets})versions[i]=upload.Write(versions[i],Access::CopyDest);
    }
    upload.SetExecute([storage, slot, offset, metadata = pending.uploaded](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        if (metadata)
        {
            const auto group_bytes = storage->groups.resource->GetDesc().Width;
            list->CopyBufferRegion(storage->groups.resource.Get(), 0, storage->upload.resource.Get(), 0, group_bytes);
            list->CopyBufferRegion(storage->proxies.resource.Get(), 0, storage->upload.resource.Get(), Align(group_bytes), storage->proxies.resource->GetDesc().Width);
        }
        auto &native = storage->frames[slot];
        for (std::uint32_t i = 0; i < 3; ++i)
            list->CopyBufferRegion(native.buffers[i].resource.Get(), 0, storage->upload.resource.Get(), offset + storage->snapshot_offsets[i], native.buffers[i].resource->GetDesc().Width);
    });
    const auto bind = [storage, slot, offset](RenderPassContext &context, std::uint32_t stage, std::uint32_t elements) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()}; list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(storage->root.Get()); list->SetPipelineState(storage->pipelines[stage].Get());
        list->SetComputeRootConstantBufferView(0, storage->upload.resource->GetGPUVirtualAddress() + offset);
        list->SetComputeRootDescriptorTable(1, storage->Gpu(slot)); Dispatch(list, std::max(elements, 1u));
    };
    const auto read_tables = [&](PassBuilder &pass, bool previous_leaf) {
        pass.Read(groups, Access::ComputeRead); pass.Read(proxies, Access::ComputeRead);
        for (const auto i : {Members, Bindings, Buckets}) pass.Read(versions[i], Access::ComputeRead);
        pass.Read(inputs.static_scene.version, Access::ComputeRead);
        if (prepared->read_geometry) pass.Read(inputs.geometry_records.version, Access::ComputeRead);
        if (prepared->read_refs) pass.Read(inputs.proxy_refs.version, Access::ComputeRead);
        if (previous)
        {
            pass.Read(previous_versions[0], Access::ComputeRead); pass.Read(previous_versions[2], Access::ComputeRead);
            if (previous_leaf) pass.Read(previous_versions[1], Access::ComputeRead);
        }
    };
    auto clearBinding=pass_binding(1,191,"HlodClear",QueueHint::Compute);
    if(!clearBinding)return Failure(ErrorCode::InvalidState,"HLOD retained clear pass is missing.");
    auto &clear=*clearBinding;
    if(!retained)
    {
        clear.Read(inputs.static_scene.version,Access::ComputeRead);
        for(std::uint32_t i=Choice;i<BufferCount;++i)versions[i]=clear.Write(versions[i],Access::UnorderedWrite);
    }
    const auto clear_elements = std::max({storage->slot_capacity, UINT(scene.groups.size()), UINT(scene.proxies.size()), UINT(scene.pages.size()), bucket_count * 4});
    clear.SetExecute([bind, clear_elements](RenderPassContext &context) { bind(context, 0, clear_elements); });
    auto selectBinding=pass_binding(2,192,"HlodSelect",QueueHint::Compute);
    if(!selectBinding)return Failure(ErrorCode::InvalidState,"HLOD retained select pass is missing.");
    auto &select=*selectBinding;
    if(!retained){read_tables(select,false);for(const auto i:{Choice,Demand,Status})versions[i]=select.ReadWrite(versions[i],Access::UnorderedWrite);}
    select.SetExecute([bind, count = UINT(scene.groups.size())](RenderPassContext &context) { bind(context, 1, count); });
    auto emitBinding=pass_binding(3,193,"HlodEmit",QueueHint::Compute);
    if(!emitBinding)return Failure(ErrorCode::InvalidState,"HLOD retained emit pass is missing.");
    auto &emit=*emitBinding;
    if(!retained)
    {
        read_tables(emit,false);emit.Read(versions[Choice],Access::UnorderedRead);
        for(const auto i:{Leaves,Selection,Indexed,Mesh,Status})versions[i]=emit.ReadWrite(versions[i],Access::UnorderedWrite);
    }
    emit.SetExecute([bind, count = UINT(std::max(scene.members.size(), scene.proxies.size()))](RenderPassContext &context) { bind(context, 2, count); });
    auto publishBinding=pass_binding(4,194,"HlodPublish",QueueHint::Compute);
    if(!publishBinding)return Failure(ErrorCode::InvalidState,"HLOD retained publish pass is missing.");
    auto &publish=*publishBinding;
    if(!retained)
    {
        publish.Read(versions[Status],Access::UnorderedRead);publish.Read(versions[Buckets],Access::ComputeRead);
        if(previous)publish.Read(previous_versions[1],Access::ComputeRead);
        for(const auto i:{Leaves,Selection,Counts})versions[i]=publish.ReadWrite(versions[i],Access::UnorderedWrite);
    }
    publish.SetExecute([bind, count = std::max({storage->slot_capacity, UINT(scene.proxies.size()), bucket_count * 4})](RenderPassContext &context) { bind(context, 3, count); });
    auto historyBinding=pass_binding(5,195,"HlodHistory",QueueHint::Compute);
    if(!historyBinding)return Failure(ErrorCode::InvalidState,"HLOD retained history pass is missing.");
    auto &history_pass=*historyBinding;
    if(!retained)
    {
        history_pass.Read(versions[Status],Access::UnorderedRead);history_pass.Read(versions[Selection],Access::UnorderedRead);
        history_pass.Read(versions[Bindings],Access::ComputeRead);
        for(const auto i:{Choice,Leaves})versions[i]=history_pass.ReadWrite(versions[i],Access::UnorderedWrite);
    }
    history_pass.SetExecute([bind, count = UINT(std::max(scene.groups.size(), scene.proxies.size()))](RenderPassContext &context) { bind(context, 4, count); });
    constexpr std::array exported{Leaves, Selection, Indexed, Mesh, Counts, Demand, Status};
    for (std::uint32_t i = 0; i < exported.size(); ++i) outputs.resources[i] = frame.buffers[exported[i]].resource.Get();
    outputs.leaf_masks = versions[Leaves]; outputs.proxy_selection = versions[Selection];
    outputs.indexed_commands = versions[Indexed]; outputs.mesh_commands = versions[Mesh]; outputs.command_counts = versions[Counts];
    outputs.page_demand = versions[Demand]; outputs.status = versions[Status];
    outputs.srvs = {storage->Export(slot, 0), storage->Export(slot, 1)};
    outputs.static_slot_capacity = storage->slot_capacity; outputs.proxy_command_capacity = UINT(scene.proxies.size());
    outputs.bucket_count = bucket_count; outputs.page_count = UINT(scene.pages.size());
    impl_->pending = std::move(pending);
    if(capture)
    {
        capture->valid=true;capture->history=history;capture->upload=prepared->upload;
        capture->read_geometry=prepared->read_geometry;capture->read_refs=prepared->read_refs;
        capture->shape={storage->slot_capacity,scene.groups.size(),scene.proxies.size(),scene.pages.size(),scene.members.size()};
        capture->guides={inputs.static_scene.version,inputs.geometry_records.version,inputs.proxy_refs.version};
        capture->outputs=versions;
    }
    return Result::Success();
}

Result HlodSystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph || pending.graph != &graph) return Failure(ErrorCode::InvalidState, "HLOD completion has no matching registered graph.");
    auto storage = pending.storage;
    storage->completion.Merge(completion); storage->frames[pending.slot].completion.Merge(completion);
    storage->upload.graph_completion.Merge(completion);
    for (const auto &imported : pending.imports)
    {
        const auto use = graph.ResourceCompletion(imported.handle);
        storage->completion.Merge(use); imported.allocation->graph_completion.Merge(completion); imported.allocation->graph_completion.Merge(use);
        if (imported.owner != UINT32_MAX)
        { storage->frames[imported.owner].completion.Merge(use); storage->frames[imported.owner].completion.Merge(completion); }
        if (success)
        { imported.allocation->graph_access = graph.FinalAccess(imported.handle); imported.allocation->graph_queue = graph.FinalQueue(imported.handle); }
    }
    if (success)
    {
        if (pending.uploaded) storage->metadata_ready = true;
        auto &frame = storage->frames[pending.slot]; frame.frame_id = pending.frame_id; frame.static_generation = pending.static_generation;
        frame.history_generation = pending.history_generation;
        storage->history_slot = pending.enabled ? pending.slot : UINT32_MAX;
    }
    else { storage->usable = false; storage->history_slot = UINT32_MAX; }
    pending = {}; return Result::Success();
}
Result HlodSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete registered HLOD graph before reset.");
    if (impl_->storage && !impl_->storage->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "HLOD reset requires completed resource and history-reader leases.");
    impl_->storage.reset(); return Result::Success();
}
} // namespace hs
