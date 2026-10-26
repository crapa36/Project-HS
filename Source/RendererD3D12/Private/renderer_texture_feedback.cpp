#include "renderer_texture_feedback.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace hs
{
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
};
struct FeedbackFrame
{
    AllocationResource metadata;
    ResidencyTicket metadata_ticket;
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
    bool hardware{};
    std::vector<std::shared_ptr<FeedbackFrame>> frames;
    Result Allocate(AllocationResource &resource, const D3D12_RESOURCE_DESC &description,
                    D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = heap;
        const auto hr = allocator->CreateResource(&allocation, &description, state, nullptr,
            &resource.allocation, IID_PPV_ARGS(&resource.resource));
        return FAILED(hr) ? NativeFailure("Allocate sampler feedback resource", hr) : Result::Success();
    }
    Result MakeOwner(FeedbackOwner &owner, TextureStreamView view, ResidencyManager &residency)
    {
        owner.target = view.resource; owner.mip_count = view.logical_description.mip_count;
        const auto target = view.resource->GetDesc();
        const auto region_width = std::min(16u, std::bit_floor(static_cast<UINT>(target.Width) / 2u));
        const auto region_height = std::min(16u, std::bit_floor(target.Height / 2u));
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
            readback_info.SizeInBytes == UINT64_MAX || info.SizeInBytes > UINT64_MAX - decoded_info.SizeInBytes ||
            info.SizeInBytes + decoded_info.SizeInBytes > UINT64_MAX - readback_info.SizeInBytes)
            return FeedbackFailure("Sampler feedback native allocation size is invalid.");
        ScopedGpuAdmission admission(residency, info.SizeInBytes + decoded_info.SizeInBytes + readback_info.SizeInBytes);
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
            const auto ticket = residency.Register(resource == &owner.readback ? ResidencyCategory::Staging : ResidencyCategory::Texture,
                resource->allocation->GetSize(), false);
            if (!ticket) return FeedbackFailure("Could not account native sampler feedback storage.");
            owner.tickets.push_back(ticket);
        }
        admission.Complete(); return Result::Success();
    }
    static void ReleaseAccounting(const FeedbackFrame &frame, ResidencyManager &residency) noexcept
    {
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
    impl_->frames.resize(frame_count); return Result::Success();
}
Result RendererTextureFeedback::PrepareFrame(std::uint32_t slot, const CompletionSet &completed,
    std::span<const TextureStreamId> bindings, const RendererTextureStreaming &streaming, ResidencyManager &residency)
{
    if (slot >= impl_->frames.size() || bindings.size() != kTextureFeedbackBindingCount)
        return FeedbackFailure("Sampler feedback material/frame binding count is invalid.");
    const auto previous = impl_->frames[slot];
    if (previous && (!previous->completion.IsSatisfiedBy(completed) || previous->pending))
        return FeedbackFailure("Sampler feedback frame bank is still in flight or unconsumed.");
    std::array<TextureMipMetadata, kTextureFeedbackBindingCount> metadata{};
    bool reuse = static_cast<bool>(previous);
    for (UINT index = 0; index < bindings.size(); ++index)
    {
        const auto view = streaming.View(bindings[index]);
        if (view.resource) metadata[index] = {view.first_mip, view.resident_min_mip,
            view.logical_description.mip_count, static_cast<UINT>(impl_->hardware && view.reserved)};
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
    std::unordered_map<std::uint32_t, std::uint32_t> owners;
    // Construct a complete bank first. Old metadata/pairs remain intact if any
    // native creation or accounting fails.
    try
    {
        candidate->owners.reserve(bindings.size());
        for (UINT index = 0; index < bindings.size(); ++index)
        {
            const auto view = streaming.View(bindings[index]);
            if (!view.resource) continue;
            metadata[index] = {view.first_mip, view.resident_min_mip, view.logical_description.mip_count,
                static_cast<UINT>(impl_->hardware && view.reserved)};
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
        const auto description = Buffer(sizeof(metadata));
        const auto info = impl_->device->GetResourceAllocationInfo(0, 1, &description);
        ScopedGpuAdmission admission(residency, info.SizeInBytes);
        if (!admission) { Impl::ReleaseAccounting(*candidate, residency); return FeedbackFailure("Texture metadata exceeds the shared budget."); }
        if (auto result = impl_->Allocate(candidate->metadata, description, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ); !result) { Impl::ReleaseAccounting(*candidate, residency); return result; }
        candidate->metadata_ticket = residency.Register(ResidencyCategory::Staging, candidate->metadata.allocation->GetSize(), true);
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
    D3D12_CPU_DESCRIPTOR_HANDLE first, D3D12_GPU_DESCRIPTOR_HANDLE gpu_first, UINT stride)
{
    if (slot >= impl_->frames.size() || !impl_->frames[slot] || !heap || !cpu_heap || !stride)
        return FeedbackFailure("Sampler feedback descriptor bank is not prepared.");
    auto &frame = *impl_->frames[slot];
    // Metadata and target/alias owners are immutable for this bank. PrepareFrame
    // replaces the bank when they change, after its existing lease checks.
    if (frame.heap.Get() == heap && frame.cpu_heap.Get() == cpu_heap && frame.written_cpu.ptr == first.ptr &&
        frame.written_gpu.ptr == gpu_first.ptr && frame.written_stride == stride) return Result::Success();
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
    frame.written_cpu = first;
    frame.written_gpu = gpu_first;
    frame.written_stride = stride;
    return Result::Success();
}
Result RendererTextureFeedback::AddClearPass(RenderGraphBuilder &graph, std::uint32_t slot,
    std::uint32_t pass_id, std::vector<TextureFeedbackGraphBinding> &bindings)
{
    bindings.clear();
    if (slot >= impl_->frames.size() || !impl_->frames[slot]) return FeedbackFailure("Sampler feedback frame is missing.");
    const auto frame = impl_->frames[slot];
    if (frame->owners.empty()) return Result::Success();
    if (!frame->heap) return FeedbackFailure("Sampler feedback GPU descriptors were not written.");
    auto pass = graph.AddPass(pass_id, "Clear actual material feedback", QueueHint::Direct);
    bindings.reserve(frame->owners.size());
    for (const auto &owner : frame->owners)
    {
        const auto handle = graph.ImportTexture({owner.feedback.resource.Get(), frame->completion, QueueHint::Direct},
            Access::Common, "actual material sampler feedback");
        bindings.push_back({owner.id, pass.Write(graph.Version(handle), Access::UnorderedWrite)});
    }
    pass.SetExecuteChecked([frame](RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ID3D12DescriptorHeap *heaps[]{frame->heap.Get()}; commands->SetDescriptorHeaps(1, heaps);
        const UINT empty[]{0xffu, 0xffu, 0xffu, 0xffu};
        for (const auto &owner : frame->owners)
            commands->ClearUnorderedAccessViewUint(owner.gpu, owner.cpu, owner.feedback.resource.Get(), empty, 0, nullptr);
        return Result::Success();
    });
    return Result::Success();
}
ResourceVersion RendererTextureFeedback::ImportMetadata(RenderGraphBuilder &graph, std::uint32_t slot) const
{
    if (slot >= impl_->frames.size() || !impl_->frames[slot]) return {};
    const auto &frame = *impl_->frames[slot];
    return graph.Version(graph.ImportBuffer({frame.metadata.resource.Get(), frame.completion, QueueHint::Direct},
        Access::AllShaderRead, "Material resident mip metadata"));
}
void RendererTextureFeedback::DeclareSampling(PassBuilder &pass, std::vector<TextureFeedbackGraphBinding> &bindings)
{
    for (auto &binding : bindings) binding.feedback = pass.ReadWrite(binding.feedback, Access::UnorderedWrite);
}
Result RendererTextureFeedback::AddDecodePass(RenderGraphBuilder &graph, std::uint32_t slot,
    std::uint32_t pass_id, std::span<const TextureFeedbackGraphBinding> bindings)
{
    if (slot >= impl_->frames.size() || !impl_->frames[slot]) return FeedbackFailure("Sampler feedback frame is missing.");
    const auto frame = impl_->frames[slot];
    if (frame->owners.empty()) return Result::Success();
    if (bindings.size() != frame->owners.size()) return FeedbackFailure("Sampler feedback graph alias set changed.");
    auto pass = graph.AddPass(pass_id, "Decode actual material feedback", QueueHint::Direct);
    for (const auto &binding : bindings)
    {
        // The opaque source is read only during decode. COMMON is an explicit
        // bridge between graph enhanced barriers and the private legacy resolve
        // sequence; both the callback and graph leave it in COMMON.
        pass.Read(binding.feedback, Access::Common);
        graph.SetFinalAccess(binding.feedback, Access::Common);
    }
    pass.SetExecuteChecked([frame](RenderPassContext &context) {
        auto *base = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ComPtr<ID3D12GraphicsCommandList1> commands;
        const auto hr = base->QueryInterface(IID_PPV_ARGS(&commands));
        if (FAILED(hr)) return NativeFailure("Query sampler feedback resolve command list", hr);
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
        UINT mip = UINT32_MAX;
        for (UINT slice = 0; slice < owner.slices && mip != 0u; ++slice)
            for (UINT y = 0; y < owner.height && mip != 0u; ++y)
                for (UINT x = 0; x < owner.width && mip != 0u; ++x)
                {
                    const auto value = std::to_integer<UINT>(mapped[owner.footprints[slice].Offset +
                        static_cast<SIZE_T>(y) * owner.footprints[slice].Footprint.RowPitch + x]);
                    if (value == 0xffu) continue;
                    // MinMip stores ideal, unclamped LOD. Coarser demand can
                    // exceed the actual chain without requesting new backing.
                    mip = std::min(mip, std::min(value, owner.mip_count - 1u));
                }
        D3D12_RANGE none{}; owner.readback.resource->Unmap(0, &none);
        if (mip != UINT32_MAX) streaming.Demand(owner.id, mip, UINT32_MAX - mip, demand_frame);
    }
    frame->pending = false; return Result::Success();
}
void RendererTextureFeedback::SwapState(RendererTextureFeedback &other) noexcept { impl_.swap(other.impl_); }
} // namespace hs
