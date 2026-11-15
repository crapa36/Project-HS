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
struct FeedbackOwner
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
struct FeedbackFrame
{
    AllocationResource metadata, terrain_requests, terrain_readback;
    ResidencyTicket metadata_ticket;
    std::vector<ResidencyTicket> terrain_tickets;
    std::vector<TextureStreamId> terrain_planes;
    D3D12_CPU_DESCRIPTOR_HANDLE terrain_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE terrain_gpu{};
    std::vector<FeedbackOwner> owners;
    std::array<std::uint32_t, kTextureFeedbackBindingCount> aliases;
    ComPtr<ID3D12DescriptorHeap> heap, cpu_heap, clear_descriptors;
    D3D12_CPU_DESCRIPTOR_HANDLE written_cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE written_gpu{};
    UINT written_stride{};
    CompletionSet completion;
    bool scheduled{}, pending{};
};
} // namespace
struct RendererTextureFeedback::Impl
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device8> device8;
    D3D12MA::Allocator *allocator{};
    bool hardware{}, unified_memory{};
    std::vector<std::shared_ptr<FeedbackFrame>> frames;
    void SampleAllocatorStats(ResidencyManager &residency)
    { static_cast<void>(residency.SampleAllocatorStats(allocator)); }
    [[nodiscard]] ResidencyTicket RegisterNativeAllocation(ResidencyManager &residency,
        ResidencyCategory category, std::uint64_t bytes, bool pinned, D3D12_HEAP_TYPE heap)
    {
        SampleAllocatorStats(residency);
        const auto segment = unified_memory || heap == D3D12_HEAP_TYPE_DEFAULT ||
            heap == D3D12_HEAP_TYPE_GPU_UPLOAD ? ResidencySegment::Local : ResidencySegment::NonLocal;
        return residency.Register(category, bytes, pinned, ResidencyStorage::GpuAllocation, segment);
    }
    Result Allocate(AllocationResource &resource, const D3D12_RESOURCE_DESC &description,
                    D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = heap;
        const auto hr = allocator->CreateResource(&allocation, &description, state, nullptr,
            &resource.allocation, IID_PPV_ARGS(&resource.resource));
        return FAILED(hr) ? NativeFailure("Allocate sampler feedback resource", hr) : Result::Success();
    }
    Result MakeOwner(FeedbackOwner &owner, const TextureStreamView &view, ResidencyManager &residency)
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
        device->GetCopyableFootprints(&decoded, 0, count, 0, owner.footprints.data(),
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
        const auto info = device8->GetResourceAllocationInfo2(0, 1, &feedback, nullptr);
        const auto decoded_info = device->GetResourceAllocationInfo(0, 1, &decoded);
        const auto readback_info = device->GetResourceAllocationInfo(0, 1, &readback);
        if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX || decoded_info.SizeInBytes == UINT64_MAX ||
            readback_info.SizeInBytes == UINT64_MAX || info.SizeInBytes > UINT64_MAX - decoded_info.SizeInBytes)
            return FeedbackFailure("Sampler feedback native allocation size is invalid.");
        SampleAllocatorStats(residency);
        ScopedGpuAdmission admission(residency, info.SizeInBytes + decoded_info.SizeInBytes, readback_info.SizeInBytes);
        if (!admission) return FeedbackFailure("Sampler feedback exceeds the shared GPU budget.");
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        auto hr = allocator->CreateResource2(&allocation, &feedback, D3D12_RESOURCE_STATE_COMMON,
            nullptr, &owner.feedback.allocation, IID_PPV_ARGS(&owner.feedback.resource));
        if (FAILED(hr)) return NativeFailure("Create native opaque sampler feedback map", hr);
        if (auto result = Allocate(owner.decoded, decoded, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON); !result) return result;
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
    static void ReleaseAccounting(const FeedbackFrame &frame, ResidencyManager &residency) noexcept
    {
        for (auto ticket : frame.terrain_tickets) static_cast<void>(residency.ReleaseNow(ticket));
        if (frame.metadata_ticket) static_cast<void>(residency.ReleaseNow(frame.metadata_ticket));
        for (const auto &owner : frame.owners)
            for (auto ticket : owner.tickets) static_cast<void>(residency.ReleaseNow(ticket));
    }
};
RendererTextureFeedback::RendererTextureFeedback() : impl_(std::make_unique<Impl>()) {}
RendererTextureFeedback::~RendererTextureFeedback() = default;
Result RendererTextureFeedback::Initialize(ID3D12Device *device, D3D12MA::Allocator *allocator,
                                            bool hardware, std::uint32_t frame_count)
{
    if (!device || !allocator || !frame_count || impl_->device)
        return FeedbackFailure("Sampler feedback initialization requires a device, allocator and frame banks.");
    if (hardware)
    {
        const auto hr = device->QueryInterface(IID_PPV_ARGS(&impl_->device8));
        if (FAILED(hr)) return NativeFailure("Query native sampler feedback device", hr);
    }
    impl_->device = device; impl_->allocator = allocator; impl_->hardware = hardware;
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
    architecture.NodeIndex = 0;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture))))
        impl_->unified_memory = architecture.UMA != FALSE;
    else
    {
        D3D12_FEATURE_DATA_ARCHITECTURE legacy{};
        legacy.NodeIndex = 0;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &legacy, sizeof(legacy))))
            impl_->unified_memory = legacy.UMA != FALSE;
    }
    impl_->frames.resize(frame_count); return Result::Success();
}
Result RendererTextureFeedback::PrepareFrame(std::uint32_t slot, const CompletionSet &completed,
    std::span<const TextureStreamId> bindings, std::span<const TextureStreamId> terrain_planes,
    const RendererTextureStreaming &streaming, ResidencyManager &residency)
{
    if (slot >= impl_->frames.size() || bindings.size() != kTextureFeedbackBindingCount ||
        terrain_planes.size() > HS_TERRAIN_PAGE_PLANE_CAPACITY || terrain_planes.size() % 3 != 0)
        return FeedbackFailure("Sampler feedback material/frame binding count is invalid.");
    const auto previous = impl_->frames[slot];
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
            view.logical_description.mip_count, static_cast<UINT>(impl_->hardware && view.reserved && view.feedback_supported)};
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
            if (!view.resource) continue;
            metadata[index] = {view.first_mip, view.resident_min_mip, view.logical_description.mip_count,
                static_cast<UINT>(impl_->hardware && view.reserved && view.feedback_supported)};
            if (!metadata[index].feedback_enabled) continue;
            const auto found = owners.find(bindings[index].value);
            if (found != owners.end()) { candidate->aliases[index] = found->second; continue; }
            const auto ordinal = static_cast<UINT>(candidate->owners.size());
            candidate->owners.emplace_back(); auto &owner = candidate->owners.back();
            owner.id = bindings[index]; owner.first_slot = index;
            if (auto result = impl_->MakeOwner(owner, view, residency); !result)
            { Impl::ReleaseAccounting(*candidate, residency); return result; }
            candidate->aliases[index] = ordinal; owners.emplace(bindings[index].value, ordinal);
        }
        if (!candidate->owners.empty())
        {
            // UAV clears require a CPU descriptor in a non-visible heap.
            // Retain one per physical owner; shader slots copy these aliases.
            D3D12_DESCRIPTOR_HEAP_DESC description{};
            description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            description.NumDescriptors = static_cast<UINT>(candidate->owners.size());
            const auto hr = impl_->device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&candidate->clear_descriptors));
            if (FAILED(hr)) { Impl::ReleaseAccounting(*candidate, residency); return NativeFailure("Create sampler feedback clear descriptors", hr); }
            const auto stride = impl_->device->GetDescriptorHandleIncrementSize(description.Type);
            auto cpu = candidate->clear_descriptors->GetCPUDescriptorHandleForHeapStart();
            for (auto &owner : candidate->owners)
            {
                owner.cpu = cpu;
                impl_->device8->CreateSamplerFeedbackUnorderedAccessView(owner.target.Get(), owner.feedback.resource.Get(), cpu);
                cpu.ptr += stride;
            }
        }
        if (!terrain_planes.empty())
        {
            auto request_desc = Buffer(HS_TERRAIN_PAGE_TILE_CAPACITY * sizeof(std::uint32_t));
            request_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const auto readback_desc = Buffer(request_desc.Width);
            const auto request_info = impl_->device->GetResourceAllocationInfo(0, 1, &request_desc);
            const auto readback_info = impl_->device->GetResourceAllocationInfo(0, 1, &readback_desc);
            if (!request_info.SizeInBytes || request_info.SizeInBytes == UINT64_MAX ||
                !readback_info.SizeInBytes || readback_info.SizeInBytes == UINT64_MAX)
            { Impl::ReleaseAccounting(*candidate, residency); return FeedbackFailure("Terrain feedback allocation size is invalid."); }
            impl_->SampleAllocatorStats(residency);
            ScopedGpuAdmission admission(residency, request_info.SizeInBytes, readback_info.SizeInBytes);
            if (!admission) { Impl::ReleaseAccounting(*candidate, residency); return FeedbackFailure("Terrain feedback exceeds the shared GPU budget."); }
            if (auto result = impl_->Allocate(candidate->terrain_requests, request_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON); !result)
            { Impl::ReleaseAccounting(*candidate, residency); return result; }
            if (auto result = impl_->Allocate(candidate->terrain_readback, readback_desc, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST); !result)
            { Impl::ReleaseAccounting(*candidate, residency); return result; }
            candidate->terrain_tickets.reserve(2);
            for (const auto *resource : {&candidate->terrain_requests, &candidate->terrain_readback})
            {
                const bool readback = resource == &candidate->terrain_readback;
                const auto ticket = impl_->RegisterNativeAllocation(residency, readback ? ResidencyCategory::Staging : ResidencyCategory::Texture,
                    resource->allocation->GetSize(), false, readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT);
                if (!ticket) { Impl::ReleaseAccounting(*candidate, residency); return FeedbackFailure("Could not account terrain feedback storage."); }
                candidate->terrain_tickets.push_back(ticket);
            }
            admission.Complete();
        }
        const auto description = Buffer(sizeof(metadata));
        const auto info = impl_->device->GetResourceAllocationInfo(0, 1, &description);
        impl_->SampleAllocatorStats(residency);
        ScopedGpuAdmission admission(residency, info.SizeInBytes,
            impl_->unified_memory ? ResidencySegment::Local : ResidencySegment::NonLocal);
        if (!admission) { Impl::ReleaseAccounting(*candidate, residency); return FeedbackFailure("Texture metadata exceeds the shared budget."); }
        if (auto result = impl_->Allocate(candidate->metadata, description, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ); !result) { Impl::ReleaseAccounting(*candidate, residency); return result; }
        candidate->metadata_ticket = impl_->RegisterNativeAllocation(residency, ResidencyCategory::Staging,
            candidate->metadata.allocation->GetSize(), true, D3D12_HEAP_TYPE_UPLOAD);
        if (!candidate->metadata_ticket) { Impl::ReleaseAccounting(*candidate, residency); return FeedbackFailure("Could not account texture mip metadata."); }
        void *mapped{}; D3D12_RANGE no_read{};
        const auto hr = candidate->metadata.resource->Map(0, &no_read, &mapped);
        if (FAILED(hr)) { Impl::ReleaseAccounting(*candidate, residency); return NativeFailure("Map immutable texture mip metadata", hr); }
        std::memcpy(mapped, metadata.data(), sizeof(metadata)); candidate->metadata.resource->Unmap(0, nullptr);
        admission.Complete();
    }
    catch (...) { Impl::ReleaseAccounting(*candidate, residency); throw; }
    if (previous) Impl::ReleaseAccounting(*previous, residency);
    impl_->frames[slot] = std::move(candidate); return Result::Success();
}
Result RendererTextureFeedback::WriteDescriptors(std::uint32_t slot, ID3D12DescriptorHeap *heap,
    ID3D12DescriptorHeap *cpu_heap,
    D3D12_CPU_DESCRIPTOR_HANDLE first, D3D12_GPU_DESCRIPTOR_HANDLE gpu_first,
    D3D12_CPU_DESCRIPTOR_HANDLE terrain_cpu, D3D12_GPU_DESCRIPTOR_HANDLE terrain_gpu, UINT stride)
{
    if (slot >= impl_->frames.size() || !impl_->frames[slot] || !heap || !cpu_heap || !stride)
        return FeedbackFailure("Sampler feedback descriptor bank is not prepared.");
    auto &frame = *impl_->frames[slot];
    // Metadata and target/alias owners are immutable for this bank. PrepareFrame
    // replaces the bank when they change, after its existing lease checks.
    if (frame.heap.Get() == heap && frame.cpu_heap.Get() == cpu_heap && frame.written_cpu.ptr == first.ptr &&
        frame.written_gpu.ptr == gpu_first.ptr && frame.terrain_cpu.ptr == terrain_cpu.ptr &&
        frame.terrain_gpu.ptr == terrain_gpu.ptr && frame.written_stride == stride) return Result::Success();
    if (stride != impl_->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) ||
        heap->GetDesc().Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        !(heap->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
        return FeedbackFailure("Sampler feedback descriptor bank is not prepared.");
    frame.heap = heap;
    frame.cpu_heap = cpu_heap;
    D3D12_SHADER_RESOURCE_VIEW_DESC metadata{}; metadata.Format = DXGI_FORMAT_UNKNOWN;
    metadata.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; metadata.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    metadata.Buffer.NumElements = kTextureFeedbackBindingCount; metadata.Buffer.StructureByteStride = sizeof(TextureMipMetadata);
    impl_->device->CreateShaderResourceView(frame.metadata.resource.Get(), &metadata, first);
    for (UINT index = 0; index < kTextureFeedbackBindingCount; ++index)
    {
        auto cpu = first; cpu.ptr += static_cast<SIZE_T>(index + 1u) * stride;
        auto gpu = gpu_first; gpu.ptr += static_cast<UINT64>(index + 1u) * stride;
        if (frame.aliases[index] != UINT32_MAX)
        {
            auto &owner = frame.owners[frame.aliases[index]];
            impl_->device->CopyDescriptorsSimple(1, cpu, owner.cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (owner.first_slot == index) owner.gpu = gpu;
        }
        else
        {
            // Inactive slots have feedback_enabled=0 and are never written by
            // the shader. Initialize a typed null view with an explicit shape;
            // the feedback constructor cannot infer one from two null resources.
            D3D12_UNORDERED_ACCESS_VIEW_DESC empty{}; empty.Format = DXGI_FORMAT_R32_UINT;
            empty.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; empty.Buffer.NumElements = 1;
            impl_->device->CreateUnorderedAccessView(nullptr, nullptr, &empty, cpu);
        }
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC terrain_view{};
    terrain_view.Format = DXGI_FORMAT_R32_UINT;
    terrain_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    terrain_view.Buffer.NumElements = HS_TERRAIN_PAGE_TILE_CAPACITY;
    impl_->device->CreateUnorderedAccessView(frame.terrain_requests.resource.Get(), nullptr, &terrain_view, terrain_cpu);
    frame.terrain_cpu = terrain_cpu; frame.terrain_gpu = terrain_gpu;
    frame.written_cpu = first;
    frame.written_gpu = gpu_first;
    frame.written_stride = stride;
    return Result::Success();
}
bool RendererTextureFeedback::MatchesLayout(std::uint32_t slot,
    std::uint32_t clear_id,std::uint32_t decode_id,
    const TextureFeedbackLayout &layout) const noexcept
{
    if(slot>=impl_->frames.size()||!impl_->frames[slot])return false;
    const auto &frame=*impl_->frames[slot];
    if(!frame.metadata.resource||((!frame.owners.empty()||frame.terrain_requests.resource)&&!frame.heap)||
        layout.owners.size()!=frame.owners.size()||
        layout.clear_pass_id!=((frame.owners.empty()&&!frame.terrain_requests.resource)?0u:clear_id)||
        layout.decode_pass_id!=((frame.owners.empty()&&!frame.terrain_requests.resource)?0u:decode_id)||
        !layout.metadata||layout.metadata_version.resource!=layout.metadata||
        layout.metadata_version.ordinal!=0)return false;
    if (frame.terrain_requests.resource)
    {
        if (!layout.terrain_requests || layout.terrain_cleared.resource != layout.terrain_requests ||
            layout.terrain_cleared.ordinal != 1 || layout.terrain_sampled.resource != layout.terrain_requests ||
            layout.terrain_sampled.ordinal < 1) return false;
    }
    else if (layout.terrain_requests) return false;
    for(std::size_t index=0;index<frame.owners.size();++index)
    {
        const auto &owner=frame.owners[index];const auto &saved=layout.owners[index];
        if(saved.texture!=owner.id||!saved.imported||
            saved.cleared.resource!=saved.imported||saved.cleared.ordinal!=1||
            saved.sampled.resource!=saved.imported||saved.sampled.ordinal<1||
            saved.width!=owner.width||saved.height!=owner.height||
            saved.slices!=owner.slices||saved.mip_count!=owner.mip_count)return false;
    }
    return true;
}
Result RendererTextureFeedback::AddClearPass(RenderGraphBuilder &graph, std::uint32_t slot,
    std::uint32_t pass_id, std::vector<TextureFeedbackGraphBinding> &bindings,
    TextureFeedbackLayout *layout)
{return AddClearPassImpl(graph,slot,pass_id,bindings,layout,nullptr);}
Result RendererTextureFeedback::BindRetainedClear(RenderGraphBuilder &graph,
    std::uint32_t slot,std::uint32_t pass_id,const TextureFeedbackLayout &layout,
    std::vector<TextureFeedbackGraphBinding> &bindings)
{return AddClearPassImpl(graph,slot,pass_id,bindings,nullptr,&layout);}
Result RendererTextureFeedback::AddClearPassImpl(RenderGraphBuilder &graph,
    std::uint32_t slot,std::uint32_t pass_id,
    std::vector<TextureFeedbackGraphBinding> &bindings,
    TextureFeedbackLayout *layout,const TextureFeedbackLayout *retained)
{
    bindings.clear();
    if (slot >= impl_->frames.size() || !impl_->frames[slot]) return FeedbackFailure("Sampler feedback frame is missing.");
    const auto frame = impl_->frames[slot];
    if (frame->owners.empty() && !frame->terrain_requests.resource) return Result::Success();
    if (!frame->heap) return FeedbackFailure("Sampler feedback GPU descriptors were not written.");
    if(retained&&(!MatchesLayout(slot,pass_id,retained->decode_pass_id,*retained)||
        !graph.BindPass(retained->clear_pass_index,pass_id)))
        return FeedbackFailure("Sampler feedback retained clear topology changed.");
    if(layout)
    {
        layout->owners.clear();layout->owners.reserve(frame->owners.size());
        layout->clear_pass_index=graph.PassCount();layout->clear_pass_id=pass_id;
    }
    auto pass=retained?*graph.BindPass(retained->clear_pass_index,pass_id):
        graph.AddPass(pass_id,"Clear actual material feedback",QueueHint::Direct);
    bindings.reserve(frame->owners.size() + (frame->terrain_requests.resource ? 1u : 0u));
    for (std::size_t index=0;index<frame->owners.size();++index)
    {
        auto &owner=frame->owners[index];
        if(auto certified=owner.feedback.EnsureNativeContract(impl_->device.Get());!certified)return certified;
        const auto external=ExternalTexture{owner.feedback.resource.Get(),frame->completion,
            QueueHint::Direct,{},owner.feedback.NativeContract()};
        const auto handle=retained?retained->owners[index].imported:
            graph.ImportTexture(external,Access::Common,"actual material sampler feedback");
        if(retained&&!graph.BindTexture(handle,external,Access::Common))
            return FeedbackFailure("Sampler feedback retained map import changed.");
        const auto written=retained?retained->owners[index].cleared:
            pass.Write(graph.Version(handle),Access::UnorderedWrite);
        bindings.push_back({retained?retained->owners[index].sampled:written,owner.opacity_sampling});
        if(layout)layout->owners.push_back({owner.id,handle,written,{},
            owner.width,owner.height,owner.slices,owner.mip_count});
    }
    if (frame->terrain_requests.resource)
    {
        if (auto certified=frame->terrain_requests.EnsureNativeContract(impl_->device.Get()); !certified) return certified;
        const auto external=ExternalBuffer{frame->terrain_requests.resource.Get(),frame->completion,
            QueueHint::Direct,{},frame->terrain_requests.NativeContract()};
        const auto handle=retained?retained->terrain_requests:
            graph.ImportBuffer(external,Access::Common,"actual terrain mip requests");
        if (retained && !graph.BindBuffer(handle,external,Access::Common))
            return FeedbackFailure("Terrain feedback retained buffer import changed.");
        const auto written=retained?retained->terrain_cleared:pass.Write(graph.Version(handle),Access::UnorderedWrite);
        bindings.push_back({retained?retained->terrain_sampled:written,false});
        if (layout) { layout->terrain_requests=handle; layout->terrain_cleared=written; }
    }
    pass.SetExecuteChecked([frame](RenderPassContext &context) {
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
ResourceVersion RendererTextureFeedback::ImportMetadata(RenderGraphBuilder &graph, std::uint32_t slot) const
{
    ResourceVersion version{};
    if(auto imported=ImportMetadata(graph,slot,version,nullptr);!imported)return {};
    return version;
}
Result RendererTextureFeedback::ImportMetadata(RenderGraphBuilder &graph,
    std::uint32_t slot,ResourceVersion &out,TextureFeedbackLayout *layout) const
{
    out={};
    if(slot>=impl_->frames.size()||!impl_->frames[slot])
        return FeedbackFailure("Sampler feedback metadata frame is missing.");
    auto &frame=*impl_->frames[slot];
    if(auto certified=frame.metadata.EnsureNativeContract(impl_->device.Get());!certified)return certified;
    const auto handle=graph.ImportBuffer({frame.metadata.resource.Get(),frame.completion,
        QueueHint::Direct,{},frame.metadata.NativeContract()},Access::AllShaderRead,
        "Material resident mip metadata");
    out=graph.Version(handle);
    if(layout){layout->metadata=handle;layout->metadata_version=out;}
    return Result::Success();
}
Result RendererTextureFeedback::BindRetainedMetadata(RenderGraphBuilder &graph,
    std::uint32_t slot,const TextureFeedbackLayout &layout,ResourceVersion &out) const
{
    out={};
    if(slot>=impl_->frames.size()||!impl_->frames[slot]||
        !layout.metadata||layout.metadata_version.resource!=layout.metadata)
        return FeedbackFailure("Sampler feedback retained metadata frame changed.");
    auto &frame=*impl_->frames[slot];
    if(auto certified=frame.metadata.EnsureNativeContract(impl_->device.Get());!certified)return certified;
    if(!graph.BindBuffer(layout.metadata,{frame.metadata.resource.Get(),frame.completion,
            QueueHint::Direct,{},frame.metadata.NativeContract()},Access::AllShaderRead))
        return FeedbackFailure("Sampler feedback retained metadata import changed.");
    out=layout.metadata_version;
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
    std::uint32_t pass_id, std::span<const TextureFeedbackGraphBinding> bindings,
    TextureFeedbackLayout *layout)
{return AddDecodePassImpl(graph,slot,pass_id,bindings,layout,nullptr);}
Result RendererTextureFeedback::BindRetainedDecode(RenderGraphBuilder &graph,
    std::uint32_t slot,std::uint32_t pass_id,const TextureFeedbackLayout &layout,
    std::span<const TextureFeedbackGraphBinding> bindings)
{return AddDecodePassImpl(graph,slot,pass_id,bindings,nullptr,&layout);}
Result RendererTextureFeedback::AddDecodePassImpl(RenderGraphBuilder &graph,
    std::uint32_t slot,std::uint32_t pass_id,
    std::span<const TextureFeedbackGraphBinding> bindings,
    TextureFeedbackLayout *layout,const TextureFeedbackLayout *retained)
{
    if (slot >= impl_->frames.size() || !impl_->frames[slot]) return FeedbackFailure("Sampler feedback frame is missing.");
    const auto frame = impl_->frames[slot];
    if (frame->owners.empty() && !frame->terrain_requests.resource) return Result::Success();
    if (bindings.size() != frame->owners.size() + (frame->terrain_requests.resource ? 1u : 0u)) return FeedbackFailure("Sampler feedback graph alias set changed.");
    if(retained&&(!MatchesLayout(slot,retained->clear_pass_id,pass_id,*retained)||
        !graph.BindPass(retained->decode_pass_index,pass_id)))
        return FeedbackFailure("Sampler feedback retained decode topology changed.");
    if(layout){layout->decode_pass_index=graph.PassCount();layout->decode_pass_id=pass_id;}
    auto pass=retained?*graph.BindPass(retained->decode_pass_index,pass_id):
        graph.AddPass(pass_id,"Decode actual material feedback",QueueHint::Direct);
    for(std::size_t index=0;index<frame->owners.size();++index)
    {
        const auto &binding=bindings[index];
        if ((retained && binding.feedback!=retained->owners[index].sampled) ||
            (layout && binding.feedback.resource!=layout->owners[index].imported) ||
            binding.opacity_sampling != frame->owners[index].opacity_sampling)
            return FeedbackFailure("Sampler feedback decode owner order changed.");
        if(retained)continue;
        // The opaque source is read only during decode. COMMON is an explicit
        // bridge between graph enhanced barriers and the private legacy resolve
        // sequence; both the callback and graph leave it in COMMON.
        pass.Read(binding.feedback, Access::Common);
        graph.SetFinalAccess(binding.feedback, Access::Common);
        if(layout)layout->owners[index].sampled=binding.feedback;
    }
    if (frame->terrain_requests.resource)
    {
        const auto &binding=bindings.back();
        if ((retained && binding.feedback != retained->terrain_sampled) ||
            (layout && binding.feedback.resource != layout->terrain_requests) || binding.opacity_sampling)
            return FeedbackFailure("Terrain feedback decode binding changed.");
        if (!retained)
        {
            pass.Read(binding.feedback, Access::CopySource);
            graph.SetFinalAccess(binding.feedback, Access::Common);
            if (layout) layout->terrain_sampled=binding.feedback;
        }
    }
    pass.SetExecuteChecked([frame](RenderPassContext &context) {
        auto *base = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ComPtr<ID3D12GraphicsCommandList1> commands;
        if (!frame->owners.empty())
        {
            const auto hr = base->QueryInterface(IID_PPV_ARGS(&commands));
            if (FAILED(hr)) return NativeFailure("Query sampler feedback resolve command list", hr);
        }
        for (const auto &owner : frame->owners)
        {
            D3D12_RESOURCE_BARRIER barriers[2]{};
            for (auto &barrier : barriers) { barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; }
            barriers[0].Transition.pResource = owner.feedback.resource.Get();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
            barriers[1].Transition.pResource = owner.decoded.resource.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            commands->ResourceBarrier(2, barriers);
            for (UINT slice = 0; slice < owner.slices; ++slice)
                commands->ResolveSubresourceRegion(owner.decoded.resource.Get(), slice, 0, 0,
                    owner.feedback.resource.Get(), UINT32_MAX, nullptr, DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            commands->ResourceBarrier(1, &barriers[1]);
            for (UINT slice = 0; slice < owner.slices; ++slice)
            {
                D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
                source.pResource = owner.decoded.resource.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = slice; destination.pResource = owner.readback.resource.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = owner.footprints[slice];
                commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
            std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            commands->ResourceBarrier(2, barriers);
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
    if (slot >= impl_->frames.size() || !impl_->frames[slot]) return;
    auto &frame = *impl_->frames[slot]; frame.completion = completion;
    frame.pending = frame.scheduled;
}
Result RendererTextureFeedback::Consume(std::uint32_t slot, const CompletionSet &completed,
    std::uint64_t demand_frame, RendererTextureStreaming &streaming)
{
    if (slot >= impl_->frames.size()) return FeedbackFailure("Sampler feedback bank index is invalid.");
    const auto frame = impl_->frames[slot];
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
    if (slot >= impl_->frames.size()) return FeedbackFailure("Sampler feedback bank index is invalid.");
    const auto frame = impl_->frames[slot];
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
void RendererTextureFeedback::SwapState(RendererTextureFeedback &other) noexcept { impl_.swap(other.impl_); }
} // namespace hs
