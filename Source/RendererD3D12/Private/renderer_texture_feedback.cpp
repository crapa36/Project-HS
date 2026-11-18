#include "renderer_texture_feedback.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <unordered_map>

namespace hs
{
std::array<std::uint32_t, 2> texture_feedback_detail::DecodeTerrainMipRequests(std::uint32_t word) noexcept
{
    const auto low = word & ((1u << HS_TERRAIN_REQUEST_MIP_BITS) - 1u);
    const auto high = word >> HS_TERRAIN_REQUEST_MIP_BITS;
    return {low ? static_cast<std::uint32_t>(std::countr_zero(low)) : kNoMipDemand,
            high ? static_cast<std::uint32_t>(std::countr_zero(high)) : kNoMipDemand};
}
std::uint32_t texture_feedback_detail::MinimumRequestedMip(
    std::span<const std::byte> row, std::uint32_t mip_count) noexcept
{
    auto minimum = std::uint8_t{0xff};
    for (const auto value : row)
    {
        if (minimum == 0) break;
        minimum = std::min(minimum, std::to_integer<std::uint8_t>(value));
    }
    if (minimum == 0xff) return texture_feedback_detail::kNoMipDemand;
    // Preserve the byte sentinel and the prior uint32 clamp arithmetic exactly.
    return std::min(static_cast<std::uint32_t>(minimum), mip_count - 1u);
}
namespace
{
using Microsoft::WRL::ComPtr;
Result FeedbackFailure(std::string text)
{ return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(text)); }
Result NativeFailure(const char *operation, HRESULT hr)
{ return FeedbackFailure(std::string(operation) + " (HRESULT " + std::to_string(hr) + ")."); }
D3D12_RESOURCE_DESC Buffer(std::uint64_t size)
{
    D3D12_RESOURCE_DESC result{}; result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size; result.Height = 1; result.DepthOrArraySize = 1; result.MipLevels = 1;
    result.SampleDesc = {1, 0}; result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return result;
}
} // namespace
struct RendererTextureFeedback::FeedbackOwner
{
    TextureStreamId id;
    ComPtr<ID3D12Resource> target;
    AllocationResource feedback, decoded, readback;
    std::vector<ResidencyTicket> tickets;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::uint32_t first_slot{}, mip_count{}, width{}, height{}, slices{};
    std::uint64_t readback_bytes{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    bool opacity_sampling{true};
};
struct RendererTextureFeedback::FeedbackFrame
{
    AllocationResource metadata, terrain_requests, terrain_readback;
    ResidencyTicket metadata_ticket;
    std::vector<ResidencyTicket> terrain_tickets;
    std::vector<TextureStreamId> terrain_planes;
    D3D12_CPU_DESCRIPTOR_HANDLE terrain_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE terrain_gpu{};
    std::vector<FeedbackOwner> owners;
    std::vector<D3D12_RESOURCE_BARRIER> begin_decode, end_decode;
    std::array<std::uint32_t, kTextureFeedbackBindingCount> aliases;
    ComPtr<ID3D12DescriptorHeap> heap, cpu_heap, clear_descriptors;
    D3D12_CPU_DESCRIPTOR_HANDLE written_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE written_gpu{};
    UINT written_stride{};
    CompletionSet completion;
    bool scheduled{}, pending{};
};

void RendererTextureFeedback::SampleAllocatorStats(ResidencyManager &residency)
{ static_cast<void>(residency.SampleAllocatorStats(allocator_)); }
ResidencyTicket RendererTextureFeedback::RegisterNativeAllocation(ResidencyManager &residency,
    ResidencyCategory category, std::uint64_t bytes, bool pinned, D3D12_HEAP_TYPE heap)
{
    SampleAllocatorStats(residency);
    const auto segment = unified_memory_ || heap == D3D12_HEAP_TYPE_DEFAULT ||
        heap == D3D12_HEAP_TYPE_GPU_UPLOAD ? ResidencySegment::Local : ResidencySegment::NonLocal;
    return residency.Register(category, bytes, pinned, ResidencyStorage::GpuAllocation, segment);
}
Result RendererTextureFeedback::Allocate(AllocationResource &resource, const D3D12_RESOURCE_DESC &description,
                D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = heap;
    const auto hr = allocator_->CreateResource(&allocation, &description, state, nullptr,
        &resource.allocation, IID_PPV_ARGS(&resource.resource));
    return FAILED(hr) ? NativeFailure("Allocate sampler feedback resource", hr) : Result::Success();
}
Result RendererTextureFeedback::MakeOwner(FeedbackOwner &owner, const TextureStreamView &view, ResidencyManager &residency)
{
    owner.target = view.resource; owner.mip_count = view.logical_description.mip_count;
    owner.opacity_sampling = view.opacity_sampling;
    const auto target = view.resource->GetDesc();
    // The consumer keeps only the whole-texture minimum, so spatial feedback precision is unused.
    const auto region_width = std::bit_floor(static_cast<UINT>(target.Width) / 2u);
    const auto region_height = std::bit_floor(target.Height / 2u);
    if (region_width < 4u || region_height < 4u) return FeedbackFailure("Selected feedback mip region is too small.");
    owner.width = (static_cast<std::uint32_t>(target.Width) + region_width - 1u) / region_width;
    owner.height = (target.Height + region_height - 1u) / region_height; owner.slices = target.DepthOrArraySize;
    D3D12_RESOURCE_DESC1 feedback{}; feedback.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    feedback.Width = target.Width; feedback.Height = target.Height;
    feedback.DepthOrArraySize = target.DepthOrArraySize; feedback.MipLevels = target.MipLevels;
    feedback.Format = DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
    feedback.SampleDesc = {1, 0}; feedback.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    feedback.SamplerFeedbackMipRegion = {region_width, region_height, 1};
    D3D12_RESOURCE_DESC decoded{}; decoded.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    decoded.Width = owner.width; decoded.Height = owner.height;
    decoded.DepthOrArraySize = static_cast<UINT16>(owner.slices); decoded.MipLevels = 1;
    decoded.Format = DXGI_FORMAT_R8_UINT; decoded.SampleDesc = {1, 0};
    const auto count = owner.slices;
    owner.footprints.resize(count); std::vector<UINT> rows(count); std::vector<UINT64> row_sizes(count);
    device_->GetCopyableFootprints(&decoded, 0, count, 0, owner.footprints.data(),
        rows.data(), row_sizes.data(), &owner.readback_bytes);
    if (!owner.readback_bytes || owner.readback_bytes == UINT64_MAX || owner.readback_bytes > SIZE_MAX)
        return FeedbackFailure("Sampler feedback readback footprint size is invalid.");
    for (UINT slice = 0; slice < count; ++slice)
    {
        const auto &footprint = owner.footprints[slice];
        if (rows[slice] != owner.height || row_sizes[slice] != owner.width ||
            footprint.Offset > owner.readback_bytes || row_sizes[slice] > footprint.Footprint.RowPitch ||
            static_cast<UINT64>(owner.height - 1u) * footprint.Footprint.RowPitch + owner.width >
                owner.readback_bytes - footprint.Offset)
            return FeedbackFailure("Sampler feedback readback footprint disagrees with its decoded array.");
    }
    const auto readback = Buffer(owner.readback_bytes);
    const auto info = device8_->GetResourceAllocationInfo2(0, 1, &feedback, nullptr);
    const auto decoded_info = device_->GetResourceAllocationInfo(0, 1, &decoded);
    const auto readback_info = device_->GetResourceAllocationInfo(0, 1, &readback);
    if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX || decoded_info.SizeInBytes == UINT64_MAX ||
        readback_info.SizeInBytes == UINT64_MAX || info.SizeInBytes > UINT64_MAX - decoded_info.SizeInBytes)
        return FeedbackFailure("Sampler feedback native allocation size is invalid.");
    SampleAllocatorStats(residency);
    ScopedGpuAdmission admission(residency, info.SizeInBytes + decoded_info.SizeInBytes, readback_info.SizeInBytes);
    if (!admission) return FeedbackFailure("Sampler feedback exceeds the shared GPU budget.");
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    auto hr = allocator_->CreateResource2(&allocation, &feedback, D3D12_RESOURCE_STATE_COMMON,
        nullptr, &owner.feedback.allocation, IID_PPV_ARGS(&owner.feedback.resource));
    if (FAILED(hr)) return NativeFailure("Create native opaque sampler feedback map", hr);
    if (auto result = Allocate(owner.decoded, decoded, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_SOURCE); !result) return result;
    if (auto result = Allocate(owner.readback, readback, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST); !result) return result;
    owner.tickets.reserve(3);
    for (const auto *resource : {&owner.feedback, &owner.decoded, &owner.readback})
    {
        const auto ticket = RegisterNativeAllocation(residency,
            resource == &owner.readback ? ResidencyCategory::Staging : ResidencyCategory::Texture,
            resource->allocation->GetSize(), false,
            resource == &owner.readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT);
        if (!ticket) return FeedbackFailure("Could not account native sampler feedback storage.");
        owner.tickets.push_back(ticket);
    }
    admission.Complete(); return Result::Success();
}
void RendererTextureFeedback::ReleaseAccounting(const FeedbackFrame &frame, ResidencyManager &residency) noexcept
{
    for (auto ticket : frame.terrain_tickets) static_cast<void>(residency.ReleaseNow(ticket));
    if (frame.metadata_ticket) static_cast<void>(residency.ReleaseNow(frame.metadata_ticket));
    for (const auto &owner : frame.owners)
        for (auto ticket : owner.tickets) static_cast<void>(residency.ReleaseNow(ticket));
}

Result RendererTextureFeedback::Initialize(ID3D12Device *device, D3D12MA::Allocator *allocator,
                                            bool hardware, std::uint32_t frame_count)
{
    if (!device || !allocator || !frame_count || device_)
        return FeedbackFailure("Sampler feedback initialization requires a device, allocator and frame banks.");
    if (hardware)
    {
        const auto hr = device->QueryInterface(IID_PPV_ARGS(&device8_));
        if (FAILED(hr)) return NativeFailure("Query native sampler feedback device", hr);
    }
    device_ = device; allocator_ = allocator; hardware_ = hardware;
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
    architecture.NodeIndex = 0;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture))))
        unified_memory_ = architecture.UMA != FALSE;
    else
    {
        D3D12_FEATURE_DATA_ARCHITECTURE legacy{};
        legacy.NodeIndex = 0;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &legacy, sizeof(legacy))))
            unified_memory_ = legacy.UMA != FALSE;
    }
    frames_.resize(frame_count); return Result::Success();
}
Result RendererTextureFeedback::PrepareFrame(std::uint32_t slot, const CompletionSet &completed,
    std::span<const TextureStreamId> bindings, std::span<const TextureStreamId> terrain_planes,
    const RendererTextureStreaming &streaming, ResidencyManager &residency)
{
    if (slot >= frames_.size() || bindings.size() != kTextureFeedbackBindingCount ||
        terrain_planes.size() > HS_TERRAIN_PAGE_PLANE_CAPACITY || terrain_planes.size() % 3 != 0)
        return FeedbackFailure("Sampler feedback material/frame binding count is invalid.");
    const auto previous = frames_[slot];
    if (previous && (!previous->completion.IsSatisfiedBy(completed) || previous->pending))
        return FeedbackFailure("Sampler feedback frame bank is still in flight or unconsumed.");
    std::array<TextureMipMetadata, kTextureFeedbackBindingCount> metadata{};
    std::array<TextureStreamView, kTextureFeedbackBindingCount> views{};
    bool reuse = previous && std::equal(terrain_planes.begin(), terrain_planes.end(),
        previous->terrain_planes.begin(), previous->terrain_planes.end());
    for (UINT index = 0; index < bindings.size(); ++index)
    {
        views[index] = streaming.View(bindings[index]);
        const auto &view = views[index];
        if (view.resource) metadata[index] = {view.first_mip, view.resident_min_mip,
            view.logical_description.mip_count, static_cast<UINT>(hardware_ && view.reserved && view.feedback_supported)};
        if (!previous) continue;
        const auto alias = previous->aliases[index];
        if (!metadata[index].feedback_enabled) { reuse = reuse && alias == UINT32_MAX; continue; }
        reuse = reuse && alias < previous->owners.size() && previous->owners[alias].id == bindings[index] &&
            previous->owners[alias].target.Get() == view.resource;
    }
    if (reuse)
    {
        void *mapped{}; D3D12_RANGE no_read{};
        const auto hr = previous->metadata.resource->Map(0, &no_read, &mapped);
        if (FAILED(hr)) return NativeFailure("Map completed texture metadata bank", hr);
        std::memcpy(mapped, metadata.data(), sizeof(metadata)); previous->metadata.resource->Unmap(0, nullptr);
        previous->scheduled = false; return Result::Success();
    }
    auto candidate = std::make_shared<FeedbackFrame>(); candidate->aliases.fill(UINT32_MAX);
    candidate->terrain_planes.assign(terrain_planes.begin(), terrain_planes.end());
    std::unordered_map<std::uint32_t, std::uint32_t> owners;
    // Construct a complete bank first. Old metadata/pairs remain intact if any
    // native creation or accounting fails.
    try
    {
        candidate->owners.reserve(bindings.size());
        for (UINT index = 0; index < bindings.size(); ++index)
        {
            const auto &view = views[index];
            if (!metadata[index].feedback_enabled) continue;
            const auto found = owners.find(bindings[index].value);
            if (found != owners.end()) { candidate->aliases[index] = found->second; continue; }
            const auto ordinal = static_cast<UINT>(candidate->owners.size());
            candidate->owners.emplace_back(); auto &owner = candidate->owners.back();
            owner.id = bindings[index]; owner.first_slot = index;
            if (auto result = MakeOwner(owner, view, residency); !result)
            { ReleaseAccounting(*candidate, residency); return result; }
            candidate->aliases[index] = ordinal; owners.emplace(bindings[index].value, ordinal);
        }
        if (!candidate->owners.empty())
        {
            const auto add_transition = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES idle,
                                            D3D12_RESOURCE_STATES resolve) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, idle, resolve};
                candidate->begin_decode.push_back(barrier);
                std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                candidate->end_decode.push_back(barrier);
            };
            candidate->begin_decode.reserve(candidate->owners.size() * 2);
            candidate->end_decode.reserve(candidate->owners.size() * 2);
            for (const auto &owner : candidate->owners)
            {
                add_transition(owner.feedback.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                add_transition(owner.decoded.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            }
            // UAV clears require a CPU descriptor in a non-visible heap.
            // Retain one per physical owner; shader slots copy these aliases.
            D3D12_DESCRIPTOR_HEAP_DESC description{};
            description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            description.NumDescriptors = static_cast<UINT>(candidate->owners.size());
            const auto hr = device_->CreateDescriptorHeap(&description, IID_PPV_ARGS(&candidate->clear_descriptors));
            if (FAILED(hr)) { ReleaseAccounting(*candidate, residency); return NativeFailure("Create sampler feedback clear descriptors", hr); }
            const auto stride = device_->GetDescriptorHandleIncrementSize(description.Type);
            auto cpu = candidate->clear_descriptors->GetCPUDescriptorHandleForHeapStart();
            for (auto &owner : candidate->owners)
            {
                owner.cpu = cpu;
                device8_->CreateSamplerFeedbackUnorderedAccessView(owner.target.Get(), owner.feedback.resource.Get(), cpu);
                cpu.ptr += stride;
            }
        }
        if (!terrain_planes.empty())
        {
            auto request_desc = Buffer(HS_TERRAIN_PAGE_TILE_CAPACITY * sizeof(std::uint32_t));
            request_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const auto readback_desc = Buffer(request_desc.Width);
            const auto request_info = device_->GetResourceAllocationInfo(0, 1, &request_desc);
            const auto readback_info = device_->GetResourceAllocationInfo(0, 1, &readback_desc);
            if (!request_info.SizeInBytes || request_info.SizeInBytes == UINT64_MAX ||
                !readback_info.SizeInBytes || readback_info.SizeInBytes == UINT64_MAX)
            { ReleaseAccounting(*candidate, residency); return FeedbackFailure("Terrain feedback allocation size is invalid."); }
            SampleAllocatorStats(residency);
            ScopedGpuAdmission admission(residency, request_info.SizeInBytes, readback_info.SizeInBytes);
            if (!admission) { ReleaseAccounting(*candidate, residency); return FeedbackFailure("Terrain feedback exceeds the shared GPU budget."); }
            if (auto result = Allocate(candidate->terrain_requests, request_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON); !result)
            { ReleaseAccounting(*candidate, residency); return result; }
            if (auto result = Allocate(candidate->terrain_readback, readback_desc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST); !result)
            { ReleaseAccounting(*candidate, residency); return result; }
            candidate->terrain_tickets.reserve(2);
            for (const auto *resource : {&candidate->terrain_requests, &candidate->terrain_readback})
            {
                const bool readback = resource == &candidate->terrain_readback;
                const auto ticket = RegisterNativeAllocation(residency, readback ? ResidencyCategory::Staging : ResidencyCategory::Texture,
                    resource->allocation->GetSize(), false, readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT);
                if (!ticket) { ReleaseAccounting(*candidate, residency); return FeedbackFailure("Could not account terrain feedback storage."); }
                candidate->terrain_tickets.push_back(ticket);
            }
            admission.Complete();
        }
        const auto description = Buffer(sizeof(metadata));
        const auto info = device_->GetResourceAllocationInfo(0, 1, &description);
        SampleAllocatorStats(residency);
        ScopedGpuAdmission admission(residency, info.SizeInBytes,
            unified_memory_ ? ResidencySegment::Local : ResidencySegment::NonLocal);
        if (!admission) { ReleaseAccounting(*candidate, residency); return FeedbackFailure("Texture metadata exceeds the shared budget."); }
        if (auto result = Allocate(candidate->metadata, description, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ); !result) { ReleaseAccounting(*candidate, residency); return result; }
        candidate->metadata_ticket = RegisterNativeAllocation(residency, ResidencyCategory::Staging,
            candidate->metadata.allocation->GetSize(), true, D3D12_HEAP_TYPE_UPLOAD);
        if (!candidate->metadata_ticket) { ReleaseAccounting(*candidate, residency); return FeedbackFailure("Could not account texture mip metadata."); }
        void *mapped{}; D3D12_RANGE no_read{};
        const auto hr = candidate->metadata.resource->Map(0, &no_read, &mapped);
        if (FAILED(hr)) { ReleaseAccounting(*candidate, residency); return NativeFailure("Map immutable texture mip metadata", hr); }
        std::memcpy(mapped, metadata.data(), sizeof(metadata)); candidate->metadata.resource->Unmap(0, nullptr);
        admission.Complete();
    }
    catch (...) { ReleaseAccounting(*candidate, residency); throw; }
    if (previous) ReleaseAccounting(*previous, residency);
    frames_[slot] = std::move(candidate); return Result::Success();
}
Result RendererTextureFeedback::WriteDescriptors(std::uint32_t slot, ID3D12DescriptorHeap *heap,
    ID3D12DescriptorHeap *cpu_heap,
    D3D12_CPU_DESCRIPTOR_HANDLE first, D3D12_GPU_DESCRIPTOR_HANDLE gpu_first,
    D3D12_CPU_DESCRIPTOR_HANDLE terrain_cpu, D3D12_GPU_DESCRIPTOR_HANDLE terrain_gpu, UINT stride)
{
    if (slot >= frames_.size() || !frames_[slot] || !heap || !cpu_heap || !stride)
        return FeedbackFailure("Sampler feedback descriptor bank is not prepared.");
    auto &frame = *frames_[slot];
    // Metadata and target/alias owners are immutable for this bank. PrepareFrame
    // replaces the bank when they change, after its existing lease checks.
    if (frame.heap.Get() == heap && frame.cpu_heap.Get() == cpu_heap && frame.written_cpu.ptr == first.ptr &&
        frame.written_gpu.ptr == gpu_first.ptr && frame.terrain_cpu.ptr == terrain_cpu.ptr &&
        frame.terrain_gpu.ptr == terrain_gpu.ptr && frame.written_stride == stride) return Result::Success();
    if (stride != device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) ||
        heap->GetDesc().Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        !(heap->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
        return FeedbackFailure("Sampler feedback descriptor bank is not prepared.");
    frame.heap = heap;
    frame.cpu_heap = cpu_heap;
    D3D12_SHADER_RESOURCE_VIEW_DESC metadata{}; metadata.Format = DXGI_FORMAT_UNKNOWN;
    metadata.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; metadata.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    metadata.Buffer.NumElements = kTextureFeedbackBindingCount; metadata.Buffer.StructureByteStride = sizeof(TextureMipMetadata);
    device_->CreateShaderResourceView(frame.metadata.resource.Get(), &metadata, first);
    for (UINT index = 0; index < kTextureFeedbackBindingCount; ++index)
    {
        auto cpu = first; cpu.ptr += static_cast<SIZE_T>(index + 1u) * stride;
        auto gpu = gpu_first; gpu.ptr += static_cast<UINT64>(index + 1u) * stride;
        if (frame.aliases[index] != UINT32_MAX)
        {
            auto &owner = frame.owners[frame.aliases[index]];
            device_->CopyDescriptorsSimple(1, cpu, owner.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (owner.first_slot == index) owner.gpu = gpu;
        }
        else
        {
            // Inactive slots have feedback_enabled=0 and are never written by
            // the shader. Initialize a typed null view with an explicit shape;
            // the feedback constructor cannot infer one from two null resources.
            D3D12_UNORDERED_ACCESS_VIEW_DESC empty{}; empty.Format = DXGI_FORMAT_R32_UINT;
            empty.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; empty.Buffer.NumElements = 1;
            device_->CreateUnorderedAccessView(nullptr, nullptr, &empty, cpu);
        }
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC terrain_view{};
    terrain_view.Format = DXGI_FORMAT_R32_UINT;
    terrain_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    terrain_view.Buffer.NumElements = HS_TERRAIN_PAGE_TILE_CAPACITY;
    device_->CreateUnorderedAccessView(frame.terrain_requests.resource.Get(), nullptr, &terrain_view, terrain_cpu);
    frame.terrain_cpu = terrain_cpu; frame.terrain_gpu = terrain_gpu;
    frame.written_cpu = first;
    frame.written_gpu = gpu_first;
    frame.written_stride = stride;
    return Result::Success();
}
bool RendererTextureFeedback::MatchesLayout(std::uint32_t slot,
    const TextureFeedbackLayout &layout) const noexcept
{
    if(slot>=frames_.size()||!frames_[slot])return false;
    const auto &frame=*frames_[slot];
    if (!frame.metadata.resource || ((!frame.owners.empty() || frame.terrain_requests.resource) && !frame.heap) ||
        layout.owners.size() != frame.owners.size() ||
        bool(layout.terrain_requests) != bool(frame.terrain_requests.resource)) return false;
    for(std::size_t index=0;index<frame.owners.size();++index)
    {
        const auto &owner=frame.owners[index];const auto &saved=layout.owners[index];
        if(saved.texture!=owner.id || saved.opacity_sampling!=owner.opacity_sampling ||
            saved.width!=owner.width||saved.height!=owner.height||
            saved.slices!=owner.slices||saved.mip_count!=owner.mip_count)return false;
    }
    return true;
}
Result RendererTextureFeedback::AddClearPass(RenderGraphBuilder &graph, std::uint32_t slot,
    TextureFeedbackLayout &layout, bool restore, std::vector<TextureFeedbackGraphBinding> &bindings)
{
    if (slot >= frames_.size() || !frames_[slot]) return FeedbackFailure("Sampler feedback frame is missing.");
    const auto frame = frames_[slot];
    if (frame->owners.empty() && !frame->terrain_requests.resource) return Result::Success();
    if (!frame->heap) return FeedbackFailure("Sampler feedback GPU descriptors were not written.");
    if (!restore)
    {
        layout.clear_pass_index = graph.PassCount();
        layout.owners.reserve(frame->owners.size());
        bindings.reserve(frame->owners.size() + (frame->terrain_requests.resource ? 1u : 0u));
    }
    auto pass = restore ? graph.BindPass(layout.clear_pass_index, 1200) :
        graph.AddPass(1200, "Clear actual material feedback", QueueHint::Direct);
    if (!pass) return FeedbackFailure("Sampler feedback retained clear pass changed.");
    for (std::size_t index=0;index<frame->owners.size();++index)
    {
        auto &owner=frame->owners[index];
        if(auto certified=owner.feedback.EnsureNativeContract(device_.Get());!certified)return certified;
        const auto external=ExternalTexture{owner.feedback.resource.Get(),frame->completion,
            QueueHint::Direct,{},owner.feedback.NativeContract()};
        if (restore)
        {
            if (!graph.BindTexture(layout.owners[index].imported, external, Access::Common))
                return FeedbackFailure("Sampler feedback retained map import changed.");
            continue;
        }
        const auto handle = graph.ImportTexture(external, Access::Common, "actual material sampler feedback");
        bindings.push_back({pass->Write(graph.Version(handle), Access::UnorderedWrite), owner.opacity_sampling});
        layout.owners.push_back({owner.id, handle, owner.width, owner.height, owner.slices, owner.mip_count, owner.opacity_sampling});
    }
    if (frame->terrain_requests.resource)
    {
        if (auto certified=frame->terrain_requests.EnsureNativeContract(device_.Get()); !certified) return certified;
        const auto external=ExternalBuffer{frame->terrain_requests.resource.Get(),frame->completion,
            QueueHint::Direct,{},frame->terrain_requests.NativeContract()};
        if (restore)
        {
            if (!graph.BindBuffer(layout.terrain_requests, external, Access::Common))
                return FeedbackFailure("Terrain feedback retained buffer import changed.");
        }
        else
        {
            layout.terrain_requests = graph.ImportBuffer(external, Access::Common, "actual terrain mip requests");
            bindings.push_back({pass->Write(graph.Version(layout.terrain_requests), Access::UnorderedWrite), false});
        }
    }
    pass->SetExecuteChecked([frame](RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ID3D12DescriptorHeap *heaps[]{frame->heap.Get()}; commands->SetDescriptorHeaps(1, heaps);
        const UINT empty[]{0xffu, 0xffu, 0xffu, 0xffu};
        for (const auto &owner : frame->owners)
            commands->ClearUnorderedAccessViewUint(owner.gpu, owner.cpu, owner.feedback.resource.Get(), empty, 0, nullptr);
        if (frame->terrain_requests.resource)
        {
            const UINT zero[4]{};
            commands->ClearUnorderedAccessViewUint(frame->terrain_gpu, frame->terrain_cpu,
                frame->terrain_requests.resource.Get(), zero, 0, nullptr);
        }
        return Result::Success();
    });
    return Result::Success();
}
Result RendererTextureFeedback::ImportMetadata(RenderGraphBuilder &graph, std::uint32_t slot,
    TextureFeedbackLayout &layout, bool restore, ResourceVersion &out) const
{
    if (slot >= frames_.size() || !frames_[slot])
        return FeedbackFailure("Sampler feedback metadata frame is missing.");
    auto &frame = *frames_[slot];
    if (auto certified = frame.metadata.EnsureNativeContract(device_.Get()); !certified) return certified;
    const ExternalBuffer external{frame.metadata.resource.Get(), frame.completion,
        QueueHint::Direct, {}, frame.metadata.NativeContract()};
    if (restore)
    {
        if (!graph.BindBuffer(layout.metadata_version.resource, external, Access::AllShaderRead))
            return FeedbackFailure("Sampler feedback retained metadata import changed.");
    }
    else
    {
        layout.metadata_version = graph.Version(graph.ImportBuffer(external, Access::AllShaderRead,
            "Material resident mip metadata"));
    }
    out = layout.metadata_version;
    return Result::Success();
}
void RendererTextureFeedback::DeclareSampling(PassBuilder &pass, std::vector<TextureFeedbackGraphBinding> &bindings,
    bool opacity_only)
{
    for (auto &binding : bindings)
        if (!opacity_only || binding.opacity_sampling)
            binding.feedback = pass.ReadWrite(binding.feedback, Access::UnorderedWrite);
}
Result RendererTextureFeedback::AddDecodePass(RenderGraphBuilder &graph, std::uint32_t slot,
    TextureFeedbackLayout &layout, bool restore, std::span<const TextureFeedbackGraphBinding> bindings)
{
    if (slot >= frames_.size() || !frames_[slot]) return FeedbackFailure("Sampler feedback frame is missing.");
    const auto frame = frames_[slot];
    if (frame->owners.empty() && !frame->terrain_requests.resource) return Result::Success();
    if (!restore) layout.decode_pass_index = graph.PassCount();
    auto pass = restore ? graph.BindPass(layout.decode_pass_index, 1201) :
        graph.AddPass(1201, "Decode actual material feedback", QueueHint::Direct);
    if (!pass) return FeedbackFailure("Sampler feedback retained decode pass changed.");
    if (!restore)
    {
        if (bindings.size() != frame->owners.size() + (frame->terrain_requests.resource ? 1u : 0u))
            return FeedbackFailure("Sampler feedback graph alias set changed.");
        // The native feedback resolve enters and leaves COMMON between graph passes.
        for (const auto &binding : bindings.first(frame->owners.size()))
        {
            pass->Read(binding.feedback, Access::Common);
            graph.SetFinalAccess(binding.feedback, Access::Common);
        }
        if (frame->terrain_requests.resource)
        {
            const auto terrain = bindings.back().feedback;
            pass->Read(terrain, Access::CopySource);
            graph.SetFinalAccess(terrain, Access::Common);
        }
    }
    pass->SetExecuteChecked([frame](RenderPassContext &context) {
        auto *base = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ComPtr<ID3D12GraphicsCommandList1> commands;
        if (!frame->owners.empty())
        {
            const auto hr = base->QueryInterface(IID_PPV_ARGS(&commands));
            if (FAILED(hr)) return NativeFailure("Query sampler feedback resolve command list", hr);
            commands->ResourceBarrier(static_cast<UINT>(frame->begin_decode.size()), frame->begin_decode.data());
            for (const auto &owner : frame->owners)
                commands->ResolveSubresourceRegion(owner.decoded.resource.Get(), UINT32_MAX, 0, 0,
                    owner.feedback.resource.Get(), UINT32_MAX, nullptr, DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
            commands->ResourceBarrier(static_cast<UINT>(frame->end_decode.size()), frame->end_decode.data());
        }
        for (const auto &owner : frame->owners)
        {
            for (UINT slice = 0; slice < owner.slices; ++slice)
            {
                D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
                source.pResource = owner.decoded.resource.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = slice; destination.pResource = owner.readback.resource.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = owner.footprints[slice];
                commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
        }
        if (frame->terrain_requests.resource)
            base->CopyBufferRegion(frame->terrain_readback.resource.Get(), 0,
                frame->terrain_requests.resource.Get(), 0, HS_TERRAIN_PAGE_TILE_CAPACITY * sizeof(std::uint32_t));
        return Result::Success();
    });
    frame->scheduled = true; return Result::Success();
}
void RendererTextureFeedback::MarkSubmitted(std::uint32_t slot, const CompletionSet &completion) noexcept
{
    if (slot >= frames_.size() || !frames_[slot]) return;
    auto &frame = *frames_[slot]; frame.completion = completion;
    frame.pending = frame.scheduled;
}
Result RendererTextureFeedback::Consume(std::uint32_t slot, const CompletionSet &completed,
    std::uint64_t demand_frame, RendererTextureStreaming &streaming)
{
    if (slot >= frames_.size()) return FeedbackFailure("Sampler feedback bank index is invalid.");
    const auto frame = frames_[slot];
    if (!frame || !frame->pending) return Result::Success();
    if (!frame->completion.IsSatisfiedBy(completed)) return FeedbackFailure("Sampler feedback readback was requested before all queues completed.");
    for (const auto &owner : frame->owners)
    {
        void *address{}; D3D12_RANGE range{0, static_cast<SIZE_T>(owner.readback_bytes)};
        const auto hr = owner.readback.resource->Map(0, &range, &address);
        if (FAILED(hr)) return NativeFailure("Map decoded material feedback", hr);
        const auto *mapped = static_cast<const std::byte *>(address);
        UINT mip = texture_feedback_detail::kNoMipDemand;
        for (UINT slice = 0; slice < owner.slices && mip != 0u; ++slice)
            for (UINT y = 0; y < owner.height && mip != 0u; ++y)
            {
                const auto offset = owner.footprints[slice].Offset +
                    static_cast<SIZE_T>(y) * owner.footprints[slice].Footprint.RowPitch;
                const auto row = std::span<const std::byte>(mapped + offset, owner.width);
                const auto row_mip = texture_feedback_detail::MinimumRequestedMip(row, owner.mip_count);
                if (row_mip != texture_feedback_detail::kNoMipDemand)
                {
                    // MinMip stores ideal, unclamped LOD. Coarser demand can
                    // exceed the actual chain without requesting new backing.
                    mip = std::min(mip, row_mip);
                }
            }
        D3D12_RANGE none{}; owner.readback.resource->Unmap(0, &none);
        if (mip != texture_feedback_detail::kNoMipDemand)
            streaming.Demand(owner.id, mip, UINT32_MAX - mip, demand_frame);
    }
    if (frame->terrain_requests.resource)
    {
        void *address{};
        D3D12_RANGE range{0, HS_TERRAIN_PAGE_TILE_CAPACITY * sizeof(std::uint32_t)};
        const auto hr = frame->terrain_readback.resource->Map(0, &range, &address);
        if (FAILED(hr)) return NativeFailure("Map terrain feedback", hr);
        const auto *words = static_cast<const std::uint32_t *>(address);
        for (std::size_t tile = 0; tile < frame->terrain_planes.size() / 3; ++tile)
        {
            const auto mip = texture_feedback_detail::DecodeTerrainMipRequests(words[tile]);
            if (mip[0] != texture_feedback_detail::kNoMipDemand)
            {
                streaming.Demand(frame->terrain_planes[tile * 3], mip[0], 0, demand_frame);
                streaming.Demand(frame->terrain_planes[tile * 3 + 1], mip[0], 0, demand_frame);
            }
            if (mip[1] != texture_feedback_detail::kNoMipDemand)
                streaming.Demand(frame->terrain_planes[tile * 3 + 2], mip[1], 0, demand_frame);
        }
        D3D12_RANGE none{}; frame->terrain_readback.resource->Unmap(0, &none);
    }
    frame->pending = false; return Result::Success();
}
#ifdef HS_DEVELOPMENT_TOOLS
Result RendererTextureFeedback::ReadTerrainRequests(std::uint32_t slot, const CompletionSet &completed,
    std::span<std::uint32_t, HS_TERRAIN_PAGE_TILE_CAPACITY> words,
    std::span<const TextureStreamId> &planes) const
{
    planes = {};
    if (slot >= frames_.size()) return FeedbackFailure("Sampler feedback bank index is invalid.");
    const auto frame = frames_[slot];
    if (!frame || !frame->scheduled || frame->terrain_planes.empty()) return Result::Success();
    if (!frame->completion.IsSatisfiedBy(completed)) return FeedbackFailure("Sampler feedback readback was requested before all queues completed.");
    void *address{};
    D3D12_RANGE range{0, HS_TERRAIN_PAGE_TILE_CAPACITY * sizeof(std::uint32_t)};
    const auto hr = frame->terrain_readback.resource->Map(0, &range, &address);
    if (FAILED(hr)) return NativeFailure("Map terrain feedback", hr);
    std::memcpy(words.data(), address, HS_TERRAIN_PAGE_TILE_CAPACITY * sizeof(std::uint32_t));
    D3D12_RANGE none{};
    frame->terrain_readback.resource->Unmap(0, &none);
    planes = frame->terrain_planes;
    return Result::Success();
}
#endif
} // namespace hs
