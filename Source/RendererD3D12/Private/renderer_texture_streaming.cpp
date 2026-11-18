#include "renderer_texture_streaming.hpp"
#include "view_state.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <array>
#include <cassert>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
Result Failure(std::string text)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(text));
}
Result NativeFailure(const char *operation, HRESULT hr)
{
    return Failure(std::string(operation) + " (HRESULT " + std::to_string(hr) + ").");
}
D3D12_RESOURCE_DESC Description(const TextureStreamRegistration &registration,
                               std::uint32_t first_mip)
{
    const auto &source = registration.description;
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.Width = std::max(1u, source.width >> first_mip);
    result.Height = std::max(1u, source.height >> first_mip);
    result.DepthOrArraySize = static_cast<UINT16>(source.array_size);
    result.MipLevels = static_cast<UINT16>(source.mip_count - first_mip);
    result.Format = static_cast<DXGI_FORMAT>(source.format);
    if (registration.srgb_view)
        result.Format = result.Format == DXGI_FORMAT_BC7_UNORM
            ? DXGI_FORMAT_BC7_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    result.SampleDesc = {1, 0};
    return result;
}
D3D12_RESOURCE_DESC Buffer(std::uint64_t bytes)
{
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = bytes; result.Height = 1; result.DepthOrArraySize = 1;
    result.MipLevels = 1; result.SampleDesc = {1, 0}; result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
}
bool Same(const render_data::TextureDescription &a, const render_data::TextureDescription &b) noexcept
{
    return a.width == b.width && a.height == b.height && a.array_size == b.array_size &&
        a.mip_count == b.mip_count && a.format == b.format;
}
struct TileMapping
{
    D3D12_TILED_RESOURCE_COORDINATE coordinate{};
    std::uint32_t tiles{}, heap_tile{};
};
struct MipBacking
{
    std::uint32_t mip{};
    AllocationResource memory; // allocation owns a heap range; resource is absent.
    ResidencyTicket ticket;
    std::vector<TileMapping> mappings;
};
struct UploadBatch
{
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Resource> software_copy_source;
    AllocationResource candidate_owner, upload;
    ResidencyTicket upload_ticket;
    std::vector<MipBacking> maps;
    std::vector<std::uint32_t> unmap_mips;
    std::vector<TileMapping> unmaps;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> destinations;
    UINT16 copy_array_size{}, copy_source_mips{}, copy_destination_mips{};
    StreamingRequestId request;
    std::uint32_t target_mip{};
    std::size_t transaction_chunk_end{};
    CompletionSet completion;
    bool reserved{}, software_upload_window{}, software_final_chunk{}, software_copy{}, scheduled{}, started{}, submitted{};
};
struct SoftwareUploadFootprint
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT row_count{};
    UINT64 row_size{}, byte_count{}, allocation_bytes{};
};
struct RetiredTexture
{
    TextureStreamId owner;
    AllocationResource texture;
    ResidencyTicket ticket;
    CompletionSet completion;
};
struct TextureOwner
{
    TextureStreamRegistration registration;
    ResidencyTicket fallback_ticket;
    ComPtr<ID3D12Resource> reserved;
    NativeResourceContract reserved_contract{};
    D3D12_PACKED_MIP_INFO packed{};
    std::vector<D3D12_SUBRESOURCE_TILING> tilings;
    std::vector<std::uint64_t> mip_costs;
    std::vector<MipBacking> backing;
    AllocationResource software;
    ResidencyTicket software_ticket;
    AllocationResource software_candidate;
    ResidencyTicket software_candidate_ticket;
    AllocationResource software_upload_window;
    ResidencyTicket software_upload_window_ticket;
    std::vector<std::uint64_t> transaction_upload_footprints;
    std::vector<SoftwareUploadFootprint> transaction_upload_plan;
    std::uint64_t software_upload_window_width{}, software_upload_window_bytes{};
    std::shared_ptr<UploadBatch> pending;
    StreamingRequestId request;
    std::vector<std::uint32_t> request_subresources;
    std::vector<ImmutableAssetRange> request_ranges;
    std::vector<std::uint32_t> transaction_subresources;
    std::size_t transaction_next{};
    AssetReadBuffer bytes;
    render_data::TexturePackageView prepared_package;
    ResidencyTicket read_ticket, decode_ticket;
    std::uint64_t metadata_reservation_bytes{};
    std::uint64_t host_target_budget{};
    std::uint32_t minimum_host_target_mip{};
    bool metadata_ticket_committed{};
    bool host_target_cache_valid{};
    std::uint64_t revision{1}, requests{}, uploads{}, publications{}, reclaims{};
    std::uint64_t demand_frame{}, observed_mip_frame{}, publication_frame{};
    std::uint32_t first_mip{}, resident_mip{}, tail_mip{}, desired_mip{}, observed_mip{}, priority{};
    std::uint32_t request_target_mip{};
    std::uint32_t transaction_target_mip{};
    bool request_reserved{};
    bool software_transaction{};
    bool budget_coarsened{};
    CompletionSet last_use, publication;
    // Mutable state of the pinned fallback allocation. The registration value
    // remains the immutable state produced by its upload publication.
    Access fallback_access{Access::Undefined};
    Access active_access{Access::Undefined};
    QueueHint active_queue{QueueHint::Direct};
    bool reserved_published{}, demand_valid{};
    bool feedback_supported{};
    bool preferred{};

    [[nodiscard]] bool IsStreamed() const noexcept
    {
        return reserved_published || software.resource.Get() != nullptr;
    }
};
static_assert(std::is_nothrow_move_constructible_v<TextureOwner>);

Result TextureContract(TextureOwner &texture, ID3D12Resource *resource, ID3D12Device *device,
                       const NativeResourceContract *&contract)
{
    contract = nullptr;
    if (resource && texture.reserved.Get() == resource)
    {
        texture.reserved_contract.generation = texture.revision;
        contract = &texture.reserved_contract;
    }
    else if (resource && texture.software.resource.Get() == resource)
    {
        if (auto certified = texture.software.EnsureNativeContract(device); !certified) return certified;
        contract = texture.software.NativeContract();
    }
    else if (resource && texture.pending && texture.pending->candidate_owner.resource.Get() == resource)
    {
        if (auto certified = texture.pending->candidate_owner.EnsureNativeContract(device); !certified) return certified;
        contract = texture.pending->candidate_owner.NativeContract();
    }
    else if (resource && texture.registration.fallback.Get() == resource &&
             texture.registration.fallback_contract.backing != NativeBackingKind::Unknown)
        contract = &texture.registration.fallback_contract;
    return Result::Success();
}

[[nodiscard]] bool IsSoftwareCopyCoarsening(const TextureOwner &texture) noexcept
{
    return texture.software_transaction && !texture.request_reserved && texture.software.resource &&
        texture.transaction_target_mip > texture.first_mip;
}
bool BuildSoftwareUploadWindowPlan(ID3D12Device *device, const TextureOwner &texture,
    std::uint32_t target, std::vector<std::uint64_t> &footprint_bytes,
    std::vector<SoftwareUploadFootprint> &native_plan,
    std::uint64_t &window_width, std::uint64_t &window_bytes)
{
    const auto &descriptor = texture.registration.descriptor;
    if (!device || !descriptor.description.mip_count || !descriptor.description.array_size ||
        descriptor.subresources.empty() || target > texture.tail_mip ||
        texture.transaction_next != 0 ||
        texture.transaction_next > texture.transaction_subresources.size()) return false;
    const auto description = Description(texture.registration, target);
    std::vector<std::uint64_t> planned;
    std::vector<SoftwareUploadFootprint> planned_native;
    planned.reserve(texture.transaction_subresources.size() - texture.transaction_next);
    planned_native.reserve(texture.transaction_subresources.size() - texture.transaction_next);
    window_width = window_bytes = 0;
    for (auto cursor = texture.transaction_next; cursor < texture.transaction_subresources.size(); ++cursor)
    {
        const auto index = texture.transaction_subresources[cursor];
        if (index >= descriptor.subresources.size()) return false;
        const auto &source = descriptor.subresources[index];
        const auto mip = index % descriptor.description.mip_count;
        const auto slice = index / descriptor.description.mip_count;
        if (mip < target || slice >= descriptor.description.array_size ||
            !source.has_digest || !source.byte_count) return false;
        const auto destination = slice * description.MipLevels + mip - target;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT row_count{};
        UINT64 row_size{}, bytes{};
        device->GetCopyableFootprints(&description, destination, 1, 0,
            &footprint, &row_count, &row_size, &bytes);
        if (!row_count || !row_size || !bytes || bytes == UINT64_MAX ||
            row_size != source.row_pitch || row_size > UINT64_MAX / row_count ||
            static_cast<UINT64>(row_count) * row_size != source.slice_pitch)
            return false;
        const auto upload_description = Buffer(bytes);
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &upload_description);
        if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX) return false;
        planned.push_back(bytes);
        planned_native.push_back({footprint, row_count, row_size, bytes, allocation.SizeInBytes});
        window_width = std::max(window_width, allocation.SizeInBytes);
    }
    if (planned.empty() || !window_width) return false;
    const auto window_description = Buffer(window_width);
    const auto window_allocation = device->GetResourceAllocationInfo(0, 1, &window_description);
    if (!window_allocation.SizeInBytes || window_allocation.SizeInBytes == UINT64_MAX) return false;
    window_bytes = window_allocation.SizeInBytes;
    footprint_bytes = std::move(planned);
    native_plan = std::move(planned_native);
    return true;
}
// Call only with no pending copy, or after its completion has been observed.
bool ReleaseSoftwareUploadWindow(TextureOwner &texture, ResidencyManager &residency) noexcept
{
    if (texture.pending && texture.pending->software_upload_window)
        texture.pending->upload.resource.Reset();
    const auto ticket = texture.software_upload_window_ticket;
    texture.software_upload_window_ticket = {};
    texture.software_upload_window.Reset();
    const bool released = ticket && residency.ReleaseNow(ticket);
    texture.transaction_upload_footprints.clear();
    texture.transaction_upload_plan.clear();
    texture.software_upload_window_width = texture.software_upload_window_bytes = 0;
    return released;
}
bool MetadataReservationBytes(const TextureOwner &texture, std::uint64_t &bytes) noexcept
{
    const auto count = texture.registration.descriptor.subresources.empty()
        ? static_cast<std::uint64_t>(texture.registration.description.array_size) * texture.registration.description.mip_count
        : static_cast<std::uint64_t>(texture.registration.descriptor.subresources.size());
    // Admission covers the view and peak upload-layout arrays; retained layouts
    // stay reserved until GPU completion, after the payload/view can be freed.
    constexpr auto per_subresource = sizeof(render_data::TextureSubresourceView) +
        sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) + 3 * sizeof(UINT) + sizeof(UINT64);
    if (count > (UINT64_MAX - sizeof(render_data::TexturePackageView)) / per_subresource)
        return false;
    bytes = sizeof(render_data::TexturePackageView) + count * per_subresource;
    return true;
}
bool RefreshTextureHostTarget(TextureOwner &texture, const ResidencyBudgetSnapshot &budget) noexcept
{
    if (!budget.host_budget_valid || !budget.host_fresh || texture.registration.descriptor.subresources.empty())
        return true;
    if (texture.host_target_cache_valid && texture.host_target_budget == budget.host_staging_budget)
        return true;
    std::uint64_t metadata_bytes{};
    if (!MetadataReservationBytes(texture, metadata_bytes)) return false;
    std::uint32_t minimum_mip{};
    if (!TextureReadMinimumHostMip(texture.registration.description,
            texture.registration.descriptor.subresources, metadata_bytes,
            budget.host_staging_budget, minimum_mip)) return false;
    texture.host_target_budget = budget.host_staging_budget;
    texture.minimum_host_target_mip = minimum_mip;
    texture.host_target_cache_valid = true;
    return true;
}
enum class HostTargetFit : std::uint8_t { Unbounded, Fits, NoFit, Invalid };
HostTargetFit TextureTargetFitsHost(TextureOwner &texture, std::uint32_t target,
                                    const ResidencyBudgetSnapshot &budget) noexcept
{
    if (!RefreshTextureHostTarget(texture, budget)) return HostTargetFit::Invalid;
    if (!budget.host_budget_valid || !budget.host_fresh || texture.registration.descriptor.subresources.empty())
        return HostTargetFit::Unbounded;
    return target >= texture.minimum_host_target_mip && target <= texture.tail_mip
        ? HostTargetFit::Fits : HostTargetFit::NoFit;
}
} // namespace
struct RendererTextureStreaming::Impl
{
    ComPtr<ID3D12Device> device;
    D3D12MA::Allocator *allocator{};
    TextureStreamingCapabilities capabilities;
    GraphicsQuality quality{GraphicsQuality::Medium};
    std::uint32_t finest_mip{};
    std::uint32_t frame_count{};
    std::uint64_t budget{};
    std::vector<TextureOwner> textures;
    std::uint32_t pending_uploads{};
    std::size_t next_read_owner{};
    std::vector<TextureStreamId> preferred_working_set;
    bool settled_preferred{};
    std::uint64_t settled_planned{}, settled_physical{}, settled_host_budget{};
    bool settled_unified_memory{};
    std::uint64_t minimum_budget_bytes{};
    std::vector<std::uint32_t> budget_candidate_indices;
    std::vector<std::uint32_t> budget_priority_scratch;
    std::vector<std::uint32_t> budget_desired_scratch, budget_tail_scratch;
    std::vector<RetiredTexture> retired;
    void SampleAllocatorStats(ResidencyManager &residency)
    {
        static_cast<void>(residency.SampleAllocatorStats(allocator));
    }
    [[nodiscard]] ResidencyTicket RegisterNativeAllocation(ResidencyManager &residency,
        ResidencyCategory category, std::uint64_t bytes, bool pinned, D3D12_HEAP_TYPE heap)
    {
        SampleAllocatorStats(residency);
        const auto segment = capabilities.unified_memory || heap == D3D12_HEAP_TYPE_DEFAULT ||
            heap == D3D12_HEAP_TYPE_GPU_UPLOAD ? ResidencySegment::Local : ResidencySegment::NonLocal;
        return residency.Register(category, bytes, pinned, ResidencyStorage::GpuAllocation, segment);
    }
    Result Create(AllocationResource &target, D3D12_RESOURCE_DESC description,
                  D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
    {
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = heap;
        const auto hr = allocator->CreateResource(&allocation, &description, state, nullptr,
            &target.allocation, IID_PPV_ARGS(&target.resource));
        return FAILED(hr) ? NativeFailure("Allocate streamed texture resource", hr) : Result::Success();
    }
    std::vector<TileMapping> Mappings(const TextureOwner &texture, std::uint32_t mip) const
    {
        if (texture.packed.NumPackedMips && mip == texture.packed.NumStandardMips)
        {
            std::vector<TileMapping> result;
            if (!texture.packed.NumTilesForPackedMips ||
                texture.registration.description.array_size > UINT32_MAX / texture.packed.NumTilesForPackedMips)
                throw std::runtime_error("Invalid native packed mip tiling.");
            for (std::uint32_t slice = 0; slice < texture.registration.description.array_size; ++slice)
                result.push_back({{0, 0, 0, slice * texture.registration.description.mip_count + mip},
                    texture.packed.NumTilesForPackedMips, slice * texture.packed.NumTilesForPackedMips});
            return result;
        }
        std::vector<TileMapping> result;
        std::uint32_t offset{};
        for (std::uint32_t slice = 0; slice < texture.registration.description.array_size; ++slice)
        {
            const auto subresource = slice * texture.registration.description.mip_count + mip;
            const auto &tiling = texture.tilings[subresource];
            const auto tiles = static_cast<std::uint64_t>(tiling.WidthInTiles) * tiling.HeightInTiles * tiling.DepthInTiles;
            if (!tiles || tiles > UINT32_MAX - offset) throw std::runtime_error("Invalid native mip tiling.");
            result.push_back({{0, 0, 0, subresource}, static_cast<std::uint32_t>(tiles), offset});
            offset += static_cast<std::uint32_t>(tiles);
        }
        return result;
    }
    bool BuildMipCosts(TextureOwner &texture) const
    {
        const auto mip_count = texture.registration.description.mip_count;
        texture.mip_costs.assign(mip_count, 0);
        if (texture.registration.pinned) return true;
        if (!texture.reserved)
        {
            for (std::uint32_t mip = 0; mip < mip_count; ++mip)
            {
                if (mip == texture.registration.fallback_first_mip) continue;
                const auto description = Description(texture.registration, mip);
                const auto bytes = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
                if (!bytes || bytes == UINT64_MAX) return false;
                texture.mip_costs[mip] = bytes;
            }
            return true;
        }
        const auto &description = texture.registration.description;
        const auto end = texture.packed.NumPackedMips ? texture.packed.NumStandardMips : description.mip_count;
        if (end > mip_count) return false;
        std::uint64_t packed_tiles{};
        if (texture.packed.NumPackedMips)
        {
            const auto tiles_per_slice = static_cast<std::uint64_t>(texture.packed.NumTilesForPackedMips);
            if (!tiles_per_slice || tiles_per_slice > UINT64_MAX / description.array_size) return false;
            packed_tiles = tiles_per_slice * description.array_size;
        }
        std::uint64_t packed_bytes{};
        if (packed_tiles > UINT64_MAX / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) return false;
        packed_bytes = packed_tiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
        for (auto mip = end; mip < mip_count; ++mip) texture.mip_costs[mip] = packed_bytes;
        std::uint64_t suffix_tiles{};
        for (auto mip = end; mip > 0;)
        {
            --mip;
            std::uint64_t mip_tiles{};
            for (UINT slice = 0; slice < description.array_size; ++slice)
            {
                const auto &tiling = texture.tilings[slice * description.mip_count + mip];
                const auto width_height = static_cast<std::uint64_t>(tiling.WidthInTiles) * tiling.HeightInTiles;
                if ((tiling.HeightInTiles && width_height / tiling.HeightInTiles != tiling.WidthInTiles) ||
                    (tiling.DepthInTiles && width_height > UINT64_MAX / tiling.DepthInTiles)) return false;
                const auto tiles = width_height * tiling.DepthInTiles;
                if (!tiles) return false;
                if (mip_tiles > UINT64_MAX - tiles) return false;
                mip_tiles += tiles;
            }
            if (suffix_tiles > UINT64_MAX - mip_tiles) return false;
            suffix_tiles += mip_tiles;
            if (suffix_tiles > (UINT64_MAX - packed_bytes) / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES)
                return false;
            texture.mip_costs[mip] = suffix_tiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES + packed_bytes;
        }
        return true;
    }
    std::uint64_t Cost(const TextureOwner &texture, std::uint32_t mip) const noexcept
    {
        return mip < texture.mip_costs.size() ? texture.mip_costs[mip] : UINT64_MAX;
    }
    std::uint64_t MinimumBudget() const
    {
        return minimum_budget_bytes;
    }
    Result Prepare(TextureOwner &, std::uint32_t, ResidencyManager &, StreamingManager &);
    Result PrepareSoftwareCopy(TextureOwner &, ResidencyManager &, StreamingManager &);
    Result EnsureSoftwareUploadWindow(TextureOwner &, ResidencyManager &, bool &ready);
    std::uint64_t PhysicalBytes(bool include_uploads) const noexcept;
};
RendererTextureStreaming::RendererTextureStreaming() : impl_(std::make_unique<Impl>()) {}
RendererTextureStreaming::~RendererTextureStreaming() = default;
Result RendererTextureStreaming::Initialize(ID3D12Device *device, D3D12MA::Allocator *allocator,
    GraphicsQuality quality, std::uint32_t finest_mip, std::uint32_t budget_mib, TextureStreamingCapabilities capabilities,
    std::uint32_t frame_count)
{
    if (!device || !allocator || impl_->device || !frame_count)
        return Failure("Texture streaming initialization requires one device, allocator and budget.");
    impl_->device = device; impl_->allocator = allocator; impl_->capabilities = capabilities;
    impl_->frame_count = frame_count;
    return Configure(quality, finest_mip, budget_mib);
}
Result RendererTextureStreaming::ValidateConfiguration(GraphicsQuality quality, std::uint32_t budget_mib) const
{
    if (quality > GraphicsQuality::Ultra)
        return Failure("Invalid texture quality or budget.");
    const auto budget = static_cast<std::uint64_t>(budget_mib) * 1024u * 1024u;
    const auto minimum = impl_->MinimumBudget();
    if (minimum == UINT64_MAX || (budget && budget < minimum))
        return Failure("Texture budget cannot contain the actual pinned selected texture tails.");
    return Result::Success();
}
Result RendererTextureStreaming::Configure(GraphicsQuality quality, std::uint32_t finest_mip, std::uint32_t budget_mib)
{
    if (auto valid = ValidateConfiguration(quality, budget_mib); !valid) return valid;
    if (impl_->quality != quality || impl_->finest_mip != finest_mip ||
        impl_->budget != static_cast<std::uint64_t>(budget_mib) * 1024u * 1024u)
        impl_->settled_preferred = false;
    impl_->quality = quality;
    impl_->finest_mip = finest_mip;
    impl_->budget = static_cast<std::uint64_t>(budget_mib) * 1024u * 1024u;
    return Result::Success();
}
Result RendererTextureStreaming::SetPreferredWorkingSet(std::span<const TextureStreamId> ids)
{
    std::vector<TextureStreamId> selected(ids.begin(), ids.end());
    std::ranges::sort(selected, {}, &TextureStreamId::value);
    if (std::ranges::any_of(selected, [&](TextureStreamId id) {
            return !id || id.value > impl_->textures.size();
        })) return Failure("Preferred texture working set has an invalid owner.");
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (selected == impl_->preferred_working_set) return Result::Success();
    for (auto id : impl_->preferred_working_set) impl_->textures[id.value - 1].preferred = false;
    for (auto id : selected) impl_->textures[id.value - 1].preferred = true;
    impl_->preferred_working_set = std::move(selected);
    impl_->settled_preferred = false;
    return Result::Success();
}
Result RendererTextureStreaming::Register(TextureStreamRegistration registration,
                                          ResidencyManager &residency, TextureStreamId &id)
{
    id = {};
    bool admitted{};
    if (auto result = RegisterBatch(std::span(&registration, 1), residency, std::span(&id, 1), admitted); !result)
        return result;
    return admitted ? Result::Success() : Failure("Texture budget cannot contain the actual pinned selected texture tails.");
}
Result RendererTextureStreaming::RegisterBatch(std::span<TextureStreamRegistration> registrations,
    ResidencyManager &residency, std::span<TextureStreamId> ids, bool &admitted,
    std::uint64_t *required_budget_bytes)
{
    if (required_budget_bytes) *required_budget_bytes = impl_->minimum_budget_bytes;
    admitted = false;
    std::ranges::fill(ids, TextureStreamId{});
    if (registrations.size() != ids.size()) return Failure("Texture registration batch output size mismatch.");
    if (registrations.empty()) { admitted = true; return Result::Success(); }
    std::vector<TextureOwner> prepared;
    std::vector<TextureStreamId> pending_ids(registrations.size());
    prepared.reserve(registrations.size());
    auto next_minimum_budget = impl_->minimum_budget_bytes;
    const auto matches = [](const TextureStreamRegistration &existing, const TextureStreamRegistration &registration) {
        return existing.generation == registration.generation && existing.source == registration.source &&
            existing.descriptor.bulk_index == registration.descriptor.bulk_index &&
            Same(existing.description, registration.description) && existing.fallback.Get() == registration.fallback.Get() &&
            existing.fallback_first_mip == registration.fallback_first_mip &&
            existing.fallback_allocation_bytes == registration.fallback_allocation_bytes &&
            existing.fallback_residency == registration.fallback_residency &&
            !existing.generation_lease.owner_before(registration.generation_lease) &&
            !registration.generation_lease.owner_before(existing.generation_lease) &&
            existing.fallback_graph_access == registration.fallback_graph_access &&
            existing.sampler_feedback == registration.sampler_feedback &&
            existing.fallback_mode == registration.fallback_mode &&
            existing.pinned == registration.pinned && existing.srgb_view == registration.srgb_view &&
            existing.opacity_sampling == registration.opacity_sampling;
    };
    for (std::size_t batch_index = 0; batch_index < registrations.size(); ++batch_index)
    {
    auto &registration = registrations[batch_index];
    if (registration.descriptor.dimension != render_data::TextureDimension::Texture2DArray ||
        registration.descriptor.asset_id.value != MakeAssetId(registration.logical_id).value ||
        registration.descriptor.subresources.empty() ||
        !Same(registration.descriptor.description, registration.description))
        return Failure("Texture registration does not match its immutable descriptor.");
    if (!impl_->device || !registration.generation_lease ||
        registration.generation.empty() || registration.logical_id.empty() || registration.source.empty() ||
        !registration.description.width || !registration.description.height ||
        !registration.description.mip_count ||
        registration.description.mip_count > 32 || !registration.description.array_size ||
        registration.description.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        registration.description.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        registration.description.array_size > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
        registration.description.mip_count > std::bit_width(std::max(registration.description.width, registration.description.height)) ||
        registration.fallback_first_mip >= registration.description.mip_count ||
        (registration.pinned && registration.fallback_first_mip))
        return Failure("Invalid selected texture streaming registration.");
    if (registration.fallback_mode != TextureStreamFallback::NativeTexture &&
        registration.fallback_mode != TextureStreamFallback::SourceMaterial)
        return Failure("Invalid selected texture fallback mode.");
    if (registration.fallback_mode == TextureStreamFallback::NativeTexture)
    {
        if (!registration.fallback || !registration.fallback_allocation_bytes ||
            registration.fallback_graph_access == Access::Undefined)
            return Failure("Invalid selected native texture fallback.");
    }
    else if (registration.pinned || registration.fallback || registration.fallback_allocation_bytes ||
        registration.fallback_residency || registration.fallback_contract.backing != NativeBackingKind::Unknown ||
        registration.fallback_contract.native_resource || registration.fallback_graph_access != Access::Undefined ||
        registration.opacity_sampling || registration.sampler_feedback)
        return Failure("Invalid selected source-material fallback.");
    if (registration.srgb_view &&
        registration.description.format != static_cast<std::uint32_t>(DXGI_FORMAT_BC7_UNORM) &&
        registration.description.format != static_cast<std::uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM))
        return Failure("sRGB texture streaming requires BC7_UNORM or R8G8B8A8_UNORM source format.");
    const auto expected = Description(registration, registration.fallback_first_mip);
    D3D12_FEATURE_DATA_FORMAT_INFO planes{expected.Format, 0};
    if (FAILED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &planes, sizeof(planes))) ||
        planes.PlaneCount != 1)
        return Failure("Selected texture native format is not single-plane.");
    if (registration.fallback_mode == TextureStreamFallback::NativeTexture)
    {
        const auto fallback = registration.fallback->GetDesc();
        if (registration.fallback_contract.backing != NativeBackingKind::Unknown &&
            (registration.fallback_contract.native_resource != registration.fallback.Get() ||
             !registration.fallback_contract.resource_identity || !registration.fallback_contract.device_identity ||
             !registration.fallback_contract.generation))
            return Failure("Selected texture fallback contract does not belong to its owner.");
        if (fallback.Dimension != expected.Dimension || fallback.Width != expected.Width ||
            fallback.Height != expected.Height || fallback.DepthOrArraySize != expected.DepthOrArraySize ||
            fallback.MipLevels != expected.MipLevels || fallback.Format != expected.Format ||
            fallback.SampleDesc.Count != 1 || fallback.SampleDesc.Quality)
            return Failure("Selected texture fallback/native subresources disagree with immutable metadata.");
    }
    for (std::size_t index = 0; index < impl_->textures.size(); ++index)
    {
        const auto &existing = impl_->textures[index].registration;
        if (existing.logical_id != registration.logical_id) continue;
        if (!matches(existing, registration))
            return Failure("Texture aliases disagree with the actual selected product owner.");
        pending_ids[batch_index].value = static_cast<std::uint32_t>(index + 1); break;
    }
    if (pending_ids[batch_index]) continue;
    for (std::size_t index = 0; index < prepared.size(); ++index)
    {
        const auto &existing = prepared[index].registration;
        if (existing.logical_id != registration.logical_id) continue;
        if (!matches(existing, registration))
            return Failure("Texture aliases disagree within the registration batch.");
        pending_ids[batch_index].value = static_cast<std::uint32_t>(impl_->textures.size() + index + 1);
        break;
    }
    if (pending_ids[batch_index]) continue;
    if (impl_->textures.size() >= UINT32_MAX || prepared.size() >= UINT32_MAX - impl_->textures.size())
        return Failure("Texture registration batch exceeds owner identity capacity.");
    TextureOwner texture; texture.registration = std::move(registration);
    texture.fallback_access = texture.registration.fallback_graph_access;
    texture.active_access = texture.fallback_access;
    texture.first_mip = texture.resident_mip = texture.registration.fallback_first_mip;
    texture.tail_mip = std::min(4u, texture.registration.description.mip_count - 1u);
    texture.desired_mip = texture.tail_mip;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format{static_cast<DXGI_FORMAT>(texture.registration.description.format)};
    const bool typed_supported = SUCCEEDED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
        &format, sizeof(format)));
    auto native_format = format;
    native_format.Format = Description(texture.registration, 0).Format;
    const bool native_supported = native_format.Format == format.Format ? typed_supported :
        SUCCEEDED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &native_format, sizeof(native_format)));
    bool tiled_format = typed_supported && native_supported &&
        (format.Support2 & D3D12_FORMAT_SUPPORT2_TILED) != 0 &&
        (native_format.Support2 & D3D12_FORMAT_SUPPORT2_TILED) != 0;
    texture.feedback_supported = texture.registration.sampler_feedback && impl_->capabilities.sampler_feedback && typed_supported &&
        (format.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
    if (texture.registration.srgb_view)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT srgb{format.Format == DXGI_FORMAT_BC7_UNORM
            ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
        const bool supported = SUCCEEDED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &srgb, sizeof(srgb)));
        tiled_format = tiled_format && supported && (srgb.Support2 & D3D12_FORMAT_SUPPORT2_TILED) != 0;
        texture.feedback_supported = texture.feedback_supported && supported &&
            (srgb.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) != 0;
    }
    // Full-chain authored arrays contain packed tails. D3D12 prohibits that
    // reserved layout before tier4; select their software owner before allocation.
    const bool array_supported = texture.registration.description.array_size == 1u ||
        impl_->capabilities.packed_mip_arrays;
    if (!texture.registration.pinned && impl_->capabilities.tiled_resources && array_supported && tiled_format &&
        texture.registration.description.width >= 8u && texture.registration.description.height >= 8u)
    {
        auto description = Description(texture.registration, 0);
        description.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        const auto hr = impl_->device->CreateReservedResource(&description, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&texture.reserved));
        if (FAILED(hr)) return NativeFailure("Create reserved selected texture", hr);
        ComPtr<IUnknown> resource_identity, device_identity;
        if (auto identity = texture.reserved.As(&resource_identity); FAILED(identity))
            return NativeFailure("Identify reserved selected texture", identity);
        if (auto identity = impl_->device.As(&device_identity); FAILED(identity))
            return NativeFailure("Identify reserved texture device", identity);
        auto &contract = texture.reserved_contract;
        contract.native_resource = texture.reserved.Get();
        contract.resource_identity = resource_identity.Get();
        contract.device_identity = device_identity.Get();
        contract.description = texture.reserved->GetDesc();
        // Every mapped mip owns a distinct D3D12MA allocation until unmapping completes.
        contract.backing = NativeBackingKind::OwnedReservedTiles;
        contract.generation = texture.revision;
        UINT total_tiles{}, count = texture.registration.description.array_size * texture.registration.description.mip_count;
        texture.tilings.resize(count); D3D12_TILE_SHAPE shape{};
        impl_->device->GetResourceTiling(texture.reserved.Get(), &total_tiles, &texture.packed, &shape,
            &count, 0, texture.tilings.data());
        if (!total_tiles || count != texture.tilings.size()) return Failure("Native selected texture tiling is incomplete.");
        if (!texture.packed.NumStandardMips)
        {
            // A wholly packed chain cannot release tiles when requesting coarser mips.
            texture.reserved_contract = {};
            texture.reserved.Reset();
            texture.tilings.clear();
            texture.packed = {};
            texture.feedback_supported = false;
        }
        else if (texture.packed.NumPackedMips) texture.tail_mip = std::min(texture.tail_mip,
            static_cast<std::uint32_t>(texture.packed.NumStandardMips));
    }
    if (!impl_->BuildMipCosts(texture))
        return Failure("Could not precompute the selected texture's native mip allocation costs.");
    const auto tail_cost = texture.registration.pinned ? 0u : impl_->Cost(texture, texture.tail_mip);
    const auto fallback_cost = texture.registration.fallback_allocation_bytes;
    if (tail_cost == UINT64_MAX || !TextureAccumulateBudgetBytes(next_minimum_budget, tail_cost) ||
        !TextureAccumulateBudgetBytes(next_minimum_budget, fallback_cost))
        return Failure("Selected texture minimum allocation total overflowed.");
    texture.fallback_ticket = texture.registration.fallback_residency;
    if (texture.fallback_ticket && (!residency.Contains(texture.fallback_ticket) || !residency.IsPinned(texture.fallback_ticket)))
        return Failure("Selected texture's existing fallback accounting is invalid.");
    texture.observed_mip = texture.desired_mip = texture.tail_mip;
    pending_ids[batch_index].value = static_cast<std::uint32_t>(impl_->textures.size() + prepared.size() + 1);
    prepared.push_back(std::move(texture));
    }
    if (required_budget_bytes) *required_budget_bytes = next_minimum_budget;
    if (impl_->budget && next_minimum_budget > impl_->budget) return Result::Success();
    impl_->textures.reserve(impl_->textures.size() + prepared.size());
    std::vector<ResidencyTicket> owned_tickets;
    owned_tickets.reserve(prepared.size());
    try
    {
        for (auto &texture : prepared)
        {
            if (texture.fallback_ticket || !texture.registration.fallback_allocation_bytes) continue;
            texture.fallback_ticket = impl_->RegisterNativeAllocation(residency,
                ResidencyCategory::Texture, texture.registration.fallback_allocation_bytes, true, D3D12_HEAP_TYPE_DEFAULT);
            if (!texture.fallback_ticket)
            {
                for (const auto ticket : owned_tickets) static_cast<void>(residency.ReleaseNow(ticket));
                return Failure("Could not account selected pinned texture tail.");
            }
            owned_tickets.push_back(texture.fallback_ticket);
        }
    }
    catch (...)
    {
        for (const auto ticket : owned_tickets) static_cast<void>(residency.ReleaseNow(ticket));
        throw;
    }
    for (auto &texture : prepared)
    {
        impl_->textures.push_back(std::move(texture));
    }
    impl_->minimum_budget_bytes = next_minimum_budget;
    if (!prepared.empty()) impl_->settled_preferred = false;
    std::ranges::copy(pending_ids, ids.begin());
    admitted = true;
    return Result::Success();
}
void RendererTextureStreaming::Demand(TextureStreamId id, std::uint32_t mip, std::uint32_t priority,
                                      std::uint64_t frame) noexcept
{
    if (!id || id.value > impl_->textures.size()) return;
    auto &texture = impl_->textures[id.value - 1];
    if (texture.registration.pinned) return;
    impl_->settled_preferred = false;
    if (!texture.demand_valid || texture.demand_frame != frame)
    {
        // Recurring fine samples must survive the native camera jitter cycle.
        const bool keep_mip = texture.demand_valid && frame >= texture.observed_mip_frame &&
            frame - texture.observed_mip_frame < std::max(impl_->frame_count, kNativeJitterPhaseCount);
        if (!keep_mip) texture.observed_mip = texture.tail_mip;
        texture.priority = 0;
    }
    texture.demand_valid = true;
    texture.demand_frame = frame; texture.priority = std::max(texture.priority, priority);
    mip = std::min(mip, texture.tail_mip);
    if (mip <= texture.observed_mip)
    {
        texture.observed_mip = mip;
        texture.observed_mip_frame = frame;
    }
}
Result RendererTextureStreaming::Impl::EnsureSoftwareUploadWindow(TextureOwner &texture,
    ResidencyManager &residency, bool &ready)
{
    ready = false;
    if (!texture.software_transaction || texture.registration.descriptor.subresources.empty())
    {
        ready = true;
        return Result::Success();
    }
    if (texture.software_candidate.resource && texture.software_candidate_ticket &&
        texture.software_upload_window.resource && texture.software_upload_window_ticket)
    {
        ready = true;
        return Result::Success();
    }
    if (texture.transaction_upload_plan.empty() &&
        !BuildSoftwareUploadWindowPlan(device.Get(), texture, texture.transaction_target_mip,
            texture.transaction_upload_footprints, texture.transaction_upload_plan,
            texture.software_upload_window_width, texture.software_upload_window_bytes))
        return Failure("Selected software texture upload window metadata is invalid.");
    const auto window_width = texture.software_upload_window_width;
    const auto window_bytes = texture.software_upload_window_bytes;
    const auto candidate_bytes = Cost(texture, texture.transaction_target_mip);
    if (!candidate_bytes || candidate_bytes == UINT64_MAX || !window_bytes || window_bytes == UINT64_MAX)
        return Failure("Selected software texture candidate or upload window size overflowed.");
    SampleAllocatorStats(residency);
    ScopedGpuAdmission admission(residency, candidate_bytes, window_bytes);
    if (!admission) return Result::Success();

    const auto rollback = [&] {
        const auto upload_ticket = texture.software_upload_window_ticket;
        texture.software_upload_window_ticket = {};
        texture.software_upload_window.Reset();
        if (upload_ticket) static_cast<void>(residency.ReleaseNow(upload_ticket));
        const auto candidate_ticket = texture.software_candidate_ticket;
        texture.software_candidate_ticket = {};
        texture.software_candidate.Reset();
        if (candidate_ticket) static_cast<void>(residency.ReleaseNow(candidate_ticket));
    };
    if (auto result = Create(texture.software_candidate,
            Description(texture.registration, texture.transaction_target_mip),
            D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON); !result)
    {
        texture.software_candidate.Reset();
        return result;
    }
    if (!texture.software_candidate.allocation ||
        texture.software_candidate.allocation->GetSize() > candidate_bytes)
    {
        rollback();
        return Failure("Software texture candidate exceeded its admitted native size.");
    }
    texture.software_candidate_ticket = RegisterNativeAllocation(residency, ResidencyCategory::Texture,
        texture.software_candidate.allocation->GetSize(), false, D3D12_HEAP_TYPE_DEFAULT);
    if (!texture.software_candidate_ticket)
    {
        rollback();
        return Failure("Could not account the admitted software texture candidate.");
    }
    if (auto result = Create(texture.software_upload_window, Buffer(window_width),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ); !result)
    {
        rollback();
        return result;
    }
    if (!texture.software_upload_window.allocation ||
        texture.software_upload_window.allocation->GetSize() > window_bytes)
    {
        rollback();
        return Failure("Software texture upload window exceeded its admitted native size.");
    }
    texture.software_upload_window_ticket = RegisterNativeAllocation(residency,
        ResidencyCategory::Staging, texture.software_upload_window.allocation->GetSize(), false,
        D3D12_HEAP_TYPE_UPLOAD);
    if (!texture.software_upload_window_ticket)
    {
        rollback();
        return Failure("Could not account the admitted software texture upload window.");
    }
    admission.Complete();
    ready = true;
    return Result::Success();
}

Result RendererTextureStreaming::Impl::Prepare(TextureOwner &texture, std::uint32_t target,
                                               ResidencyManager &residency, StreamingManager &streaming)
{
    if (target > texture.tail_mip || texture.revision == UINT64_MAX)
        return Failure("Selected texture mip target or descriptor revision is invalid.");
    const bool mapping_only_shrink = texture.request_reserved && texture.reserved_published &&
        target > texture.resident_mip;
    if (!mapping_only_shrink)
    {
        try
        {
            if (!texture.registration.descriptor.subresources.empty() && !texture.request_ranges.empty())
            {
                const auto &descriptor = texture.registration.descriptor;
                if (texture.prepared_package.subresources.empty())
                {
                    texture.prepared_package.description = descriptor.description;
                    texture.prepared_package.subresources.resize(descriptor.subresources.size());
                }
                std::size_t offset{};
                for (const auto index : texture.request_subresources)
                {
                    const auto &layout = descriptor.subresources[index];
                    auto &view = texture.prepared_package.subresources[index];
                    const auto mip = index % descriptor.description.mip_count;
                    view.array_slice = index / descriptor.description.mip_count;
                    view.mip_level = mip;
                    view.width = std::max(1u, descriptor.description.width >> mip);
                    view.height = std::max(1u, descriptor.description.height >> mip);
                    view.row_pitch = layout.row_pitch; view.slice_pitch = layout.slice_pitch;
                    view.bytes = texture.bytes.Bytes().subspan(offset, layout.byte_count);
                    offset += layout.byte_count;
                }
                if (offset != texture.bytes.size()) return Failure("Selected texture range payload layout is inconsistent.");
            }
            else if (texture.prepared_package.subresources.empty())
                texture.prepared_package = render_data::ReadTexturePackage(
                    texture.bytes.Bytes(), MakeAssetId(texture.registration.logical_id));
        }
        catch (const std::exception &error) { return Failure("Selected texture decode failed: " + std::string(error.what())); }
    }
    const auto &package = texture.prepared_package;
    if (!Same(package.description, texture.registration.description))
        return Failure("Selected texture metadata changed inside its immutable Generation.");
    if (texture.decode_ticket && !texture.metadata_ticket_committed)
    {
        if (!residency.CommitHost(texture.decode_ticket, texture.metadata_reservation_bytes))
            return Failure("Selected texture prepared metadata reservation is invalid.");
        texture.metadata_ticket_committed = true;
    }
    auto batch = std::make_shared<UploadBatch>();
    if (texture.request_reserved != static_cast<bool>(texture.reserved))
        return Failure("Selected texture native representation changed during its transaction.");
    batch->target_mip = target; batch->request = texture.request; batch->reserved = texture.request_reserved;
    if (!batch->reserved && texture.software_transaction)
    {
        batch->transaction_chunk_end = texture.transaction_next + texture.request_subresources.size();
        batch->software_final_chunk = texture.transaction_subresources.empty() ||
            batch->transaction_chunk_end == texture.transaction_subresources.size();
        if (batch->transaction_chunk_end > texture.transaction_subresources.size())
            return Failure("Software texture chunk exceeds its fixed replacement transaction.");
    }
    if (batch->reserved) batch->resource = texture.reserved;
    const auto description = Description(texture.registration, batch->reserved ? 0u : target);
    const auto target_mips = description.MipLevels;
    std::uint64_t backing_bytes{};
    if (batch->reserved)
    {
        const auto end = texture.packed.NumPackedMips ? texture.packed.NumStandardMips + 1u : package.description.mip_count;
        for (UINT mip = target; mip < end; ++mip)
            if (std::ranges::find(texture.backing, mip, &MipBacking::mip) == texture.backing.end())
            {
                MipBacking backing; backing.mip = mip; backing.mappings = Mappings(texture, mip);
                UINT64 tiles{}; for (const auto &mapping : backing.mappings) tiles += mapping.tiles;
                backing_bytes += tiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
                batch->maps.push_back(std::move(backing));
            }
        for (const auto &backing : texture.backing)
            if (backing.mip < target)
            {
                batch->unmap_mips.push_back(backing.mip);
                batch->unmaps.insert(batch->unmaps.end(), backing.mappings.begin(), backing.mappings.end());
            }
    }
    if (mapping_only_shrink && (!batch->reserved || target <= texture.resident_mip ||
        !batch->maps.empty() || batch->unmaps.empty()))
        return Failure("Reserved texture coarsening has no authenticated mapping-only transition.");
    if (!batch->reserved && !texture.software_candidate.resource)
    {
        backing_bytes = Cost(texture, target);
        if (!backing_bytes)
            return Failure("Selected software texture candidate has no precomputed native mip cost.");
    }
    // A reserved batch with no new mappings only changes tile mappings. Its
    // requested mips already contain the package data, so keep the mapping
    // batch independent of GPU upload admission.
    const bool upload_needed = !batch->reserved || !batch->maps.empty();
    const bool use_software_upload_window = !batch->reserved && texture.software_transaction &&
        texture.software_candidate.resource && texture.software_candidate_ticket &&
        texture.software_upload_window.resource && texture.software_upload_window_ticket;
    if (!batch->reserved && texture.software_transaction &&
        !texture.registration.descriptor.subresources.empty() && !use_software_upload_window)
        return Failure("Selected software texture transaction lost its admitted upload window.");
    assert(!mapping_only_shrink || !upload_needed);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    std::vector<UINT64> row_bytes;
    std::vector<UINT> destinations, source_indices;
    UINT64 upload_bytes{};
    auto transaction_footprint = texture.transaction_next;
    if (upload_needed)
    {
        const auto selected_count = texture.request_subresources.empty()
            ? static_cast<std::size_t>(package.description.array_size) * (package.description.mip_count - target)
            : texture.request_subresources.size();
        footprints.reserve(selected_count); rows.reserve(selected_count); row_bytes.reserve(selected_count);
        destinations.reserve(selected_count); source_indices.reserve(selected_count);
        for (UINT slice = 0; slice < package.description.array_size; ++slice)
            for (UINT mip = target; mip < package.description.mip_count; ++mip)
            {
                const UINT source_index = slice * package.description.mip_count + mip;
                if (source_index >= package.subresources.size()) continue;
                const auto &source = package.subresources[source_index];
                if (source.bytes.empty()) continue;
                const UINT destination = slice * target_mips + mip - (batch->reserved ? 0u : target);
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT row_count{}; UINT64 row_size{}, bytes{};
                if (use_software_upload_window)
                {
                    if (transaction_footprint >= texture.transaction_upload_plan.size() ||
                        transaction_footprint >= texture.transaction_subresources.size() ||
                        texture.transaction_subresources[transaction_footprint] != source_index)
                        return Failure("Selected texture upload window lost its immutable subresource plan.");
                    const auto &planned = texture.transaction_upload_plan[transaction_footprint];
                    footprint = planned.footprint; row_count = planned.row_count;
                    row_size = planned.row_size; bytes = planned.byte_count;
                }
                else
                {
                    // Query each layout at zero, then place it at an aligned buffer
                    // offset. pTotalBytes is a size, not a subsequent placement offset.
                    device->GetCopyableFootprints(&description, destination, 1, 0,
                        &footprint, &row_count, &row_size, &bytes);
                }
                constexpr auto alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
                if (!bytes || bytes == UINT64_MAX)
                    return Failure("Selected texture upload placement size is invalid.");
                std::uint64_t next_upload_bytes{};
                const auto window_width = use_software_upload_window
                    ? texture.software_upload_window_width : UINT64_MAX;
                if (!TextureUploadPackAppend(upload_bytes, bytes, alignment, window_width, next_upload_bytes) ||
                    footprint.Offset > bytes)
                    return Failure("Selected texture upload placement overflow.");
                const auto base = next_upload_bytes - bytes;
                footprint.Offset += base;
                const auto end = next_upload_bytes;
                if (!row_count || source.array_slice != slice || source.mip_level != mip ||
                    row_size != source.row_pitch || static_cast<UINT64>(row_count) * row_size != source.slice_pitch ||
                    source.bytes.size() != source.slice_pitch || row_size > footprint.Footprint.RowPitch ||
                    footprint.Offset > end || static_cast<UINT64>(row_count - 1u) * footprint.Footprint.RowPitch + row_size > end - footprint.Offset)
                    return Failure("Selected texture source/native upload footprint disagrees.");
                if (use_software_upload_window)
                {
                    if (transaction_footprint >= texture.transaction_upload_footprints.size() ||
                        transaction_footprint >= texture.transaction_subresources.size() ||
                        texture.transaction_subresources[transaction_footprint] != source_index ||
                        texture.transaction_upload_footprints[transaction_footprint] != bytes)
                        return Failure("Selected texture upload window footprint changed within its transaction.");
                    ++transaction_footprint;
                }
                footprints.push_back(footprint); rows.push_back(row_count); row_bytes.push_back(row_size);
                destinations.push_back(destination); source_indices.push_back(source_index); upload_bytes = next_upload_bytes;
            }
    }
    if (use_software_upload_window && transaction_footprint !=
        texture.transaction_next + texture.request_subresources.size())
        return Failure("Selected texture upload window does not cover the admitted chunk.");
    const auto upload_description = Buffer(upload_bytes);
    const auto upload_info = upload_needed && !use_software_upload_window
        ? device->GetResourceAllocationInfo(0, 1, &upload_description) : D3D12_RESOURCE_ALLOCATION_INFO{};
    if ((upload_needed && !use_software_upload_window &&
            (!upload_info.SizeInBytes || upload_info.SizeInBytes == UINT64_MAX)) ||
        backing_bytes == UINT64_MAX || (use_software_upload_window &&
            upload_bytes > texture.software_upload_window_width))
        return Failure("Texture native backing/upload allocation size overflow.");
    // Window chunks reuse the existing allocation; only the copy width changes.
    const auto upload_peak = upload_needed && !use_software_upload_window ? upload_info.SizeInBytes : 0u;
    if (!use_software_upload_window) SampleAllocatorStats(residency);
    ScopedGpuAdmission admission(residency,
        use_software_upload_window ? 0u : backing_bytes,
        use_software_upload_window ? 0u : upload_peak);
    if (!use_software_upload_window && (backing_bytes || upload_peak) && !admission)
        return Result::Success(); // Keep non-window requests until both budgets admit their native peak.
    const bool candidate_created_here = !batch->reserved && !texture.software_candidate.resource;
    const auto rollback = [&] {
        for (auto &mapping : batch->maps) if (mapping.ticket) static_cast<void>(residency.ReleaseNow(mapping.ticket));
        if (batch->upload_ticket) static_cast<void>(residency.ReleaseNow(batch->upload_ticket));
        if (!batch->reserved && texture.software_transaction && !use_software_upload_window)
        {
            if (texture.software_candidate_ticket) static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
            texture.software_candidate_ticket = {};
            texture.software_candidate.Reset();
        }
    };
    try
    {
        if (batch->reserved)
        {
            for (auto &backing : batch->maps)
            {
                UINT64 tiles{}; for (const auto &mapping : backing.mappings) tiles += mapping.tiles;
                D3D12_RESOURCE_ALLOCATION_INFO info{tiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,
                    D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
                D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
                allocation.ExtraHeapFlags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
                const auto hr = allocator->AllocateMemory(&allocation, &info, &backing.memory.allocation);
                if (FAILED(hr)) { rollback(); return NativeFailure("Allocate streamed mip tiles", hr); }
                if (!backing.memory.allocation->GetHeap() ||
                    backing.memory.allocation->GetOffset() % D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES ||
                    backing.memory.allocation->GetOffset() / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES > UINT32_MAX)
                { rollback(); return Failure("Native streamed tile heap range is invalid."); }
                backing.ticket = RegisterNativeAllocation(residency, ResidencyCategory::Texture,
                    backing.memory.allocation->GetSize(), backing.mip >= texture.tail_mip, D3D12_HEAP_TYPE_DEFAULT);
                if (!backing.ticket) { rollback(); return Failure("Could not account streamed tile backing."); }
            }
        }
        else
        {
            if (candidate_created_here)
            {
                if (auto result = Create(texture.software_candidate, description, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_COMMON); !result) { rollback(); return result; }
                texture.software_candidate_ticket = RegisterNativeAllocation(residency, ResidencyCategory::Texture,
                    texture.software_candidate.allocation->GetSize(), false, D3D12_HEAP_TYPE_DEFAULT);
                if (!texture.software_candidate_ticket) { rollback(); return Failure("Could not account streamed replacement texture."); }
            }
            batch->resource = texture.software_candidate.resource;
        }
        HRESULT hr{};
        if (upload_needed)
        {
            if (use_software_upload_window)
            {
                batch->software_upload_window = true;
                batch->upload.resource = texture.software_upload_window.resource;
            }
            else
            {
                if (auto result = Create(batch->upload, upload_description, D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_STATE_GENERIC_READ); !result) { rollback(); return result; }
                batch->upload_ticket = RegisterNativeAllocation(residency, ResidencyCategory::Staging,
                    batch->upload.allocation->GetSize(), false, D3D12_HEAP_TYPE_UPLOAD);
                if (!batch->upload_ticket) { rollback(); return Failure("Could not account streamed texture upload."); }
            }
            std::byte *mapped{}; D3D12_RANGE no_read{};
            hr = batch->upload.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
            if (FAILED(hr)) { rollback(); return NativeFailure("Map streamed texture upload", hr); }
            for (std::size_t index = 0; index < footprints.size(); ++index)
            {
                const auto &source = package.subresources[source_indices[index]].bytes;
                auto *destination = mapped + footprints[index].Offset;
                const auto pitch = footprints[index].Footprint.RowPitch;
                const auto row_size = static_cast<SIZE_T>(row_bytes[index]);
                if (pitch == row_size)
                {
                    std::memcpy(destination, source.data(), source.size());
                    continue;
                }
                for (UINT row = 0; row < rows[index]; ++row)
                    std::memcpy(destination + static_cast<SIZE_T>(row) * pitch,
                        source.data() + static_cast<SIZE_T>(row) * row_size, row_size);
            }
            batch->upload.resource->Unmap(0, nullptr);
        }
        batch->footprints = std::move(footprints);
        batch->destinations = std::move(destinations);
        // Publication cannot allocate: reserve both vectors before queue work.
        texture.backing.reserve(texture.backing.size() + batch->maps.size());
        // Several textures may publish together. Every such retirement must
        // already have capacity before any GPU command can start.
        retired.reserve(retired.size() + textures.size());
        const bool began = mapping_only_shrink
            ? streaming.BeginResidentTransfer(texture.request, texture.registration.generation)
            : streaming.BeginUpload(texture.request, texture.registration.generation);
        if (!began)
        { rollback(); return Failure(mapping_only_shrink
            ? "Texture mapping-only request transition was stale or invalid."
            : "Texture upload request transition was stale."); }
        texture.pending = std::move(batch);
        ++pending_uploads;
        if (texture.software_transaction)
            texture.pending->candidate_owner = std::move(texture.software_candidate);
        if (texture.read_ticket) static_cast<void>(residency.ReleaseNow(texture.read_ticket));
        texture.read_ticket = {};
        if (!texture.software_transaction)
            texture.prepared_package = {};
        else
            for (auto &subresource : texture.prepared_package.subresources) subresource.bytes = {};
        texture.request_ranges.clear(); texture.request_subresources.clear();
        texture.bytes.Reset();
        admission.Complete(); return Result::Success();
    }
    catch (...) { rollback(); throw; }
}

Result RendererTextureStreaming::Impl::PrepareSoftwareCopy(TextureOwner &texture,
    ResidencyManager &residency, StreamingManager &streaming)
{
    if (!IsSoftwareCopyCoarsening(texture) || !texture.request ||
        !texture.software.resource || !texture.software.allocation || !texture.software_ticket ||
        static_cast<bool>(texture.software_candidate.resource) !=
            static_cast<bool>(texture.software_candidate_ticket) ||
        texture.transaction_target_mip >= texture.registration.description.mip_count ||
        texture.transaction_target_mip > texture.tail_mip ||
        texture.first_mip >= texture.transaction_target_mip ||
        texture.request_reserved || !texture.request_ranges.empty() ||
        !texture.request_subresources.empty() || !texture.transaction_subresources.empty() ||
        texture.read_ticket || texture.decode_ticket)
        return Failure("Software coarsening copy is missing its published source or fixed target.");

    const auto source_description = texture.software.resource->GetDesc();
    const auto expected_source = Description(texture.registration, texture.first_mip);
    const auto destination_description = Description(texture.registration,
        texture.transaction_target_mip);
    const auto same_texture_shape = [](const D3D12_RESOURCE_DESC &actual,
                                       const D3D12_RESOURCE_DESC &expected) noexcept {
        return actual.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            actual.Dimension == expected.Dimension && actual.Width == expected.Width &&
            actual.Height == expected.Height && actual.DepthOrArraySize == expected.DepthOrArraySize &&
            actual.MipLevels == expected.MipLevels && actual.Format == expected.Format &&
            actual.SampleDesc.Count == expected.SampleDesc.Count &&
            actual.SampleDesc.Quality == expected.SampleDesc.Quality;
    };
    if (!same_texture_shape(source_description, expected_source) ||
        !destination_description.MipLevels)
        return Failure("Published software texture does not match its coarsening copy range.");
    const auto source_subresources = static_cast<std::uint64_t>(source_description.DepthOrArraySize) *
        source_description.MipLevels;
    const auto destination_subresources = static_cast<std::uint64_t>(destination_description.DepthOrArraySize) *
        destination_description.MipLevels;
    constexpr auto max_subresource_count = static_cast<std::uint64_t>(UINT32_MAX) + 1u;
    if (!source_subresources || source_subresources > max_subresource_count ||
        !destination_subresources || destination_subresources > max_subresource_count)
        return Failure("Software coarsening copy subresource range overflowed.");

    const auto target = texture.transaction_target_mip;
    const auto candidate_cost = Cost(texture, target);
    if (candidate_cost == UINT64_MAX || !candidate_cost ||
        (texture.software_candidate.resource && (!texture.software_candidate.allocation ||
            texture.software_candidate.allocation->GetSize() > candidate_cost)))
        return Failure("Software coarsening candidate has no valid native allocation cost.");
    const auto candidate_bytes = texture.software_candidate.resource ? 0u : candidate_cost;
    if (!texture.software_candidate.resource)
        SampleAllocatorStats(residency);
    ScopedGpuAdmission admission(residency, candidate_bytes, 0);
    if (candidate_bytes && !admission) return Result::Success();

    const bool candidate_created_here = !texture.software_candidate.resource;
    const auto rollback = [&] {
        if (candidate_created_here)
        {
            const auto ticket = texture.software_candidate_ticket;
            texture.software_candidate_ticket = {};
            texture.software_candidate.Reset();
            if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
        }
    };
    try
    {
        if (candidate_created_here)
        {
            if (auto created = Create(texture.software_candidate, destination_description,
                    D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON); !created)
            {
                rollback();
                return created;
            }
            if (!texture.software_candidate.allocation ||
                texture.software_candidate.allocation->GetSize() > Cost(texture, target))
            {
                rollback();
                return Failure("Software coarsening candidate exceeded its admitted native size.");
            }
            texture.software_candidate_ticket = RegisterNativeAllocation(residency,
                ResidencyCategory::Texture, texture.software_candidate.allocation->GetSize(), false,
                D3D12_HEAP_TYPE_DEFAULT);
            if (!texture.software_candidate_ticket)
            {
                rollback();
                return Failure("Could not account the software coarsening candidate.");
            }
        }

        auto batch = std::make_shared<UploadBatch>();
        batch->resource = texture.software_candidate.resource;
        batch->software_copy_source = texture.software.resource;
        batch->request = texture.request;
        batch->target_mip = target;
        batch->software_copy = true;
        batch->software_final_chunk = true;

        batch->copy_array_size = destination_description.DepthOrArraySize;
        batch->copy_source_mips = source_description.MipLevels;
        batch->copy_destination_mips = destination_description.MipLevels;

        // Reserve publication retirement before the copy can be submitted.
        retired.reserve(retired.size() + textures.size());
        if (!streaming.BeginResidentTransfer(texture.request, texture.registration.generation))
        {
            rollback();
            return Failure("Software texture copy request was stale or invalid.");
        }
        texture.pending = std::move(batch);
        ++pending_uploads;
        texture.pending->candidate_owner = std::move(texture.software_candidate);
        admission.Complete();
        return Result::Success();
    }
    catch (...)
    {
        rollback();
        throw;
    }
}

Result RendererTextureStreaming::Pump(std::uint64_t frame, const CompletionSet &completed,
    RendererAssetIo &io, StreamingManager &streaming, ResidencyManager &residency)
{
    struct SubmitReadsOnExit
    {
        RendererAssetIo &io;
        ~SubmitReadsOnExit() { io.SubmitQueuedReads(); }
    } submit_reads{io};
    // A completed optional working set needs no per-owner planning until a
    // real budget threshold, host-fit, registration, quality or demand change.
    if (impl_->settled_preferred)
    {
        const auto pressure = residency.GpuPressureFacts();
        const auto &observed = pressure.budget;
        const auto host = residency.HostAdmissionFacts().budget;
        if (observed.valid && observed.local_budget &&
            observed.unified_memory == impl_->settled_unified_memory &&
            host.host_budget_valid && host.host_fresh && host.host_staging_budget == impl_->settled_host_budget)
        {
            const auto limit = observed.local_budget - observed.local_budget / 10u;
            const auto usage = observed.local_usage > UINT64_MAX - pressure.local_growth_since_snapshot
                ? UINT64_MAX : observed.local_usage + pressure.local_growth_since_snapshot;
            if (usage <= limit)
            {
                const auto extra = limit - usage;
                const auto allowance = impl_->settled_physical > UINT64_MAX - extra
                    ? UINT64_MAX : impl_->settled_physical + extra;
                const auto budget = impl_->budget ? std::min(impl_->budget, allowance) : allowance;
                if (impl_->settled_planned <= budget)
                {
                    static_cast<void>(residency.Collect(completed));
                    static_cast<void>(streaming.Collect(completed));
                    return Result::Success();
                }
            }
        }
        impl_->settled_preferred = false;
    }
    for (auto iterator = impl_->retired.begin(); iterator != impl_->retired.end();)
        if (iterator->completion.IsSatisfiedBy(completed))
        {
            if (residency.ReleaseNow(iterator->ticket) && iterator->owner &&
                iterator->owner.value <= impl_->textures.size())
                ++impl_->textures[iterator->owner.value - 1].reclaims;
            iterator = impl_->retired.erase(iterator);
        }
        else ++iterator;
    for (std::size_t owner_index = 0; owner_index < impl_->textures.size(); ++owner_index)
    {
        auto &texture = impl_->textures[owner_index];
        const auto owner = TextureStreamId{static_cast<std::uint32_t>(owner_index + 1u)};
        const auto batch = texture.pending;
        if (!batch || !batch->submitted || !batch->completion.IsSatisfiedBy(completed)) continue;
        // A retained graph callback may still own the batch after its GPU work.
        // Free the CPU layouts before releasing their host reservation.
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>{}.swap(batch->footprints);
        std::vector<UINT>{}.swap(batch->destinations);
        if (!streaming.TryPublish(batch->request, texture.registration.generation, completed))
        {
            const auto *request = streaming.Find(batch->request);
            if (!request)
                return Failure("Completed selected texture upload lost its streaming request ledger.");
            if (request->state != StreamingPageState::Cancelled &&
                request->content_generation == texture.registration.generation)
                return Failure("Completed selected texture upload could not publish from its current request state.");
            if (batch->reserved) return Failure("Completed selected texture upload was cancelled before publication.");
            // The copy is complete, so a stale unpublished software candidate
            // can now be discarded without releasing a live GPU lease.
            if (!batch->reserved && texture.software_candidate_ticket)
            {
                batch->resource.Reset();
                batch->candidate_owner.Reset();
                const auto candidate_ticket = texture.software_candidate_ticket;
                texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
                texture.software_transaction = false; texture.transaction_subresources.clear();
                texture.transaction_next = 0;
                if (residency.ReleaseNow(candidate_ticket)) ++texture.reclaims;
                if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
            }
            if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
            texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
            if (residency.ReleaseNow(batch->upload_ticket)) ++texture.reclaims;
            texture.pending.reset(); texture.request = {};
            texture.request_ranges.clear(); texture.request_subresources.clear();
            texture.prepared_package = {};
            texture.bytes.Reset();
            continue;
        }
        if (batch->reserved)
        {
            ++texture.publications;
            const bool already_published = texture.reserved_published;
            for (auto iterator = texture.backing.begin(); iterator != texture.backing.end();)
                if (std::ranges::find(batch->unmap_mips, iterator->mip) != batch->unmap_mips.end())
                {
                    if (residency.ReleaseNow(iterator->ticket)) ++texture.reclaims;
                    iterator = texture.backing.erase(iterator);
                }
                else ++iterator;
            for (auto &backing : batch->maps) texture.backing.push_back(std::move(backing));
            texture.first_mip = 0; texture.reserved_published = true;
            if (!already_published)
            {
                texture.active_access = Access::Common;
                texture.active_queue = QueueHint::Copy;
                texture.last_use = {}; // Earlier readers belong to the still-pinned fallback allocation.
            }
        }
        else if (texture.software_transaction && !batch->software_final_chunk)
        {
            // A completed chunk is privately resident in the candidate. Retire
            // its request, retain the candidate, and continue the same fixed target.
            if (!streaming.Retire(batch->request, batch->completion))
                return Failure("Completed software texture chunk could not retire its request.");
            texture.software_candidate = std::move(batch->candidate_owner);
            texture.transaction_next = batch->transaction_chunk_end;
            if (residency.ReleaseNow(batch->upload_ticket)) ++texture.reclaims;
            if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
            texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
            texture.pending.reset(); texture.request = {};
            texture.request_ranges.clear(); texture.request_subresources.clear();
            texture.prepared_package = {};
            texture.bytes.Reset();
            continue;
        }
        else
        {
            if (!texture.software_transaction || !batch->candidate_owner.resource)
                return Failure("Completed software texture upload lost its fixed candidate transaction.");
            ++texture.publications;
            if (texture.software.resource)
                impl_->retired.push_back({owner, std::move(texture.software), texture.software_ticket, texture.last_use});
            texture.software = std::move(batch->candidate_owner);
            texture.software_ticket = texture.software_candidate_ticket;
            texture.software_candidate_ticket = {};
            texture.software_transaction = false;
            texture.transaction_subresources.clear(); texture.transaction_next = 0;
            if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
            texture.first_mip = batch->target_mip;
            texture.active_access = Access::Common;
            texture.active_queue = QueueHint::Copy;
            texture.last_use = {};
            if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
            texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
            texture.prepared_package = {}; texture.bytes.Reset();
        }
        texture.resident_mip = batch->target_mip; texture.publication = batch->completion;
        texture.publication_frame = frame;
        ++texture.revision;
        if (residency.ReleaseNow(batch->upload_ticket)) ++texture.reclaims;
        if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
        texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
        static_cast<void>(streaming.Retire(batch->request, batch->completion));
        texture.pending.reset(); texture.request = {};
    }
    // Apply quality before the budget planner. With no feedback/view demand the
    // real coarse tail remains resident, including on software adapters.
    const auto host_budget_snapshot = residency.HostAdmissionFacts().budget;
    const auto pressure = residency.GpuPressureFacts();
    const auto &observed = pressure.budget;
    std::uint64_t planned{};
    for (auto &texture : impl_->textures)
    {
        texture.budget_coarsened = false;
        if (texture.registration.pinned)
        {
            if (!TextureAccumulateBudgetBytes(planned, texture.registration.fallback_allocation_bytes))
                return Failure("Pinned texture budget cost overflow.");
            continue;
        }
        const auto recent = [&](std::uint64_t observed_frame) {
            return observed_frame <= frame && frame - observed_frame <= impl_->frame_count;
        };
        if (texture.demand_valid && recent(texture.demand_frame)) texture.desired_mip = texture.observed_mip;
        else
        {
            // Retain demand/publication through one actual feedback-bank cycle.
            // This supplies hysteresis and a minimum residency period without
            // treating missing delayed feedback as a fresh coarse request.
            texture.priority = texture.preferred ? 1u : 0u;
            texture.desired_mip = (texture.reserved_published || texture.software.resource) &&
                (texture.preferred || recent(texture.publication_frame))
                ? texture.resident_mip : texture.tail_mip;
        }
        texture.desired_mip = std::min(texture.tail_mip, std::max(texture.desired_mip, impl_->finest_mip));
        if (!texture.reserved && !texture.registration.descriptor.subresources.empty() &&
            host_budget_snapshot.host_budget_valid && host_budget_snapshot.host_fresh)
        {
            if (!RefreshTextureHostTarget(texture, host_budget_snapshot))
                return Failure("Selected texture authenticated host range metadata is invalid.");
            const auto fallback_mip = texture.software.resource
                ? texture.resident_mip : texture.registration.fallback_first_mip;
            texture.desired_mip = TextureEffectiveHostTargetMip(texture.desired_mip,
                texture.tail_mip, texture.minimum_host_target_mip, fallback_mip,
                texture.resident_mip, texture.software.resource.Get() != nullptr);
        }
        const auto cost = impl_->Cost(texture, texture.desired_mip);
        if (cost == UINT64_MAX || !TextureAccumulateBudgetBytes(planned, texture.registration.fallback_allocation_bytes) ||
            !TextureAccumulateBudgetBytes(planned, cost))
            return Failure("Selected texture budget cost overflow.");
    }
    auto budget = impl_->budget;
    bool global_pressure{};
    if (observed.valid && observed.local_budget)
    {
        const auto limit = observed.local_budget - observed.local_budget / 10u;
        const auto usage = observed.local_usage > UINT64_MAX - pressure.local_growth_since_snapshot
            ? UINT64_MAX : observed.local_usage + pressure.local_growth_since_snapshot;
        global_pressure = usage > limit;
        // Upload heaps are nonlocal on discrete adapters, so they cannot fund local detail.
        const auto physical = impl_->PhysicalBytes(observed.unified_memory);
        const auto allowance = usage < limit
            ? physical > UINT64_MAX - (limit - usage) ? UINT64_MAX : physical + (limit - usage)
            : physical > usage - limit ? physical - (usage - limit) : 0u;
        // A configured cap and observed global pressure both constrain detail.
        // Required tails keep their authored fallback even when pressure leaves
        // no discretionary allowance for new backing.
        budget = budget ? std::min(budget, allowance) : allowance;
        budget = std::max(budget, impl_->MinimumBudget());
    }
    else if (!budget) budget = UINT64_MAX; // The same unreported-budget contract as ResidencyManager.
    if (planned > budget)
    {
        const auto count = impl_->textures.size();
        impl_->budget_priority_scratch.resize(count);
        impl_->budget_desired_scratch.resize(count);
        impl_->budget_tail_scratch.resize(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto &texture = impl_->textures[index];
            impl_->budget_priority_scratch[index] = texture.priority;
            impl_->budget_desired_scratch[index] = texture.desired_mip;
            impl_->budget_tail_scratch[index] = texture.registration.pinned ? texture.desired_mip : texture.tail_mip;
        }
        if (!TextureBuildBudgetVictimOrder(impl_->budget_priority_scratch,
                impl_->budget_desired_scratch, impl_->budget_tail_scratch,
                impl_->budget_candidate_indices))
            return Failure("Texture budget victim candidate state is invalid.");
        for (const auto index : impl_->budget_candidate_indices)
        {
            auto &victim = impl_->textures[index];
            while (planned > budget && victim.desired_mip < victim.tail_mip)
            {
                if (!TextureCoarsenOneMip(victim.desired_mip, victim.tail_mip,
                        victim.mip_costs, planned))
                    return Failure("Native texture mip costs are not monotonic or overflowed.");
                victim.budget_coarsened = true;
            }
            if (planned <= budget) break;
        }
        if (planned > budget)
            return Failure("Texture budget cannot contain the actual pinned selected texture tails.");
    }
    const auto abandon_software_transaction = [&](TextureOwner &texture) {
        if (texture.request) static_cast<void>(streaming.Cancel(texture.request));
        if (texture.read_ticket) static_cast<void>(residency.ReleaseNow(texture.read_ticket));
        if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
        texture.read_ticket = {}; texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
        texture.request = {}; texture.request_ranges.clear(); texture.request_subresources.clear();
        if (texture.software_candidate_ticket)
        {
            static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
            texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
        }
        if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
        texture.software_transaction = false;
        texture.transaction_subresources.clear(); texture.transaction_next = 0;
        texture.transaction_target_mip = 0;
        texture.prepared_package = {}; texture.bytes.Reset();
    };
    // Resolve no-payload shrink/fallback transitions globally before admitting
    // any new texture payload reads in this pump.
    for (std::size_t owner_index = 0; owner_index < impl_->textures.size(); ++owner_index)
    {
        auto &texture = impl_->textures[owner_index];
        const auto owner = TextureStreamId{static_cast<std::uint32_t>(owner_index + 1u)};
        if (texture.registration.pinned || texture.pending) continue;
        const bool fallback_target = texture.desired_mip == texture.registration.fallback_first_mip;
        const bool coarsening = texture.desired_mip > texture.resident_mip;
        // A requested-but-not-submitted GPU copy borrows the published source.
        // If the existing fallback/pressure policy drops that source, cancel
        // this copy request first so the normal payload path can rebuild from
        // the fallback instead of using a stale source plan.
        if (coarsening && (fallback_target || global_pressure) &&
            IsSoftwareCopyCoarsening(texture))
            abandon_software_transaction(texture);
        if (!texture.reserved && texture.software.resource && coarsening &&
            (fallback_target || global_pressure))
        {
            if (texture.revision == UINT64_MAX) return Failure("Texture descriptor revision exhausted.");
            impl_->retired.reserve(impl_->retired.size() + 1u);
            auto retirement = texture.last_use; retirement.Merge(texture.publication);
            impl_->retired.push_back({owner, std::move(texture.software), texture.software_ticket, retirement});
            texture.software_ticket = {}; texture.last_use = {}; texture.publication = {};
            texture.first_mip = texture.resident_mip = texture.registration.fallback_first_mip;
            texture.active_access = texture.fallback_access;
            texture.active_queue = QueueHint::Direct; ++texture.revision;
            if (fallback_target)
            {
                if (texture.request) io.Cancel(texture.request, streaming);
                if (texture.read_ticket) static_cast<void>(residency.ReleaseNow(texture.read_ticket));
                if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
                texture.read_ticket = {}; texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
                texture.request = {};
                texture.request_ranges.clear(); texture.request_subresources.clear();
                texture.prepared_package = {}; texture.bytes.Reset();
                if (texture.software_candidate_ticket)
                {
                    static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
                    texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
                }
                if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
                texture.software_transaction = false; texture.transaction_subresources.clear(); texture.transaction_next = 0;
            }
        }
        if (texture.reserved_published && coarsening && !texture.request)
        {
            texture.request_target_mip = texture.desired_mip;
            texture.request_reserved = !!texture.reserved;
            const auto request = streaming.Request(texture.registration.logical_id,
                "texture-package", texture.registration.generation, texture.priority);
            if (!request) return Failure("Could not register selected texture shrink.");
            texture.request = request; ++texture.requests;
            texture.prepared_package = {};
            texture.prepared_package.description = texture.registration.description;
            if (auto result = impl_->Prepare(texture, texture.request_target_mip, residency, streaming); !result) return result;
        }
    }
    // Advance every already-admitted request and retry retained Prepared owners
    // before allowing any new payload I/O to consume host budget.
    for (auto &texture : impl_->textures)
    {
        if (texture.registration.pinned || texture.pending || !texture.request) continue;
        const auto *page = streaming.Find(texture.request);
        if (!page) return Failure("Selected texture request ledger disappeared.");
        if (page->state == StreamingPageState::Reading)
        {
            AssetReadCompletion read;
            if (auto result = io.Poll(texture.request, streaming, residency, read); !result) return result;
            if (!read.ready) continue;
            if (read.cancelled)
            {
                if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
                texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
                texture.request = {}; texture.request_ranges.clear(); texture.request_subresources.clear();
                texture.prepared_package = {}; texture.bytes.Reset();
                if (texture.software_candidate_ticket)
                {
                    static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
                    texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
                }
                if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
                texture.software_transaction = false; texture.transaction_subresources.clear(); texture.transaction_next = 0;
                continue;
            }
            texture.bytes = std::move(read.bytes);
            if (!texture.software_transaction) texture.prepared_package = {};
            texture.read_ticket = read.storage;
            if (!streaming.CompleteRead(texture.request, texture.registration.generation))
            {
                if (page->state != StreamingPageState::Cancelled &&
                    page->content_generation == texture.registration.generation)
                    return Failure("Selected texture read completed with an invalid streaming transition.");
                if (texture.read_ticket) static_cast<void>(residency.ReleaseNow(texture.read_ticket));
                texture.read_ticket = {};
                if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
                texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
                if (texture.software_candidate_ticket)
                {
                    static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
                    texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
                }
                if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
                texture.software_transaction = false; texture.transaction_subresources.clear(); texture.transaction_next = 0;
                texture.request = {}; texture.request_ranges.clear(); texture.request_subresources.clear();
                texture.prepared_package = {}; texture.bytes.Reset();
                continue;
            }
        }
        else if (page->state == StreamingPageState::Cancelled)
        {
            if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
            texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
            texture.request = {}; texture.request_ranges.clear(); texture.request_subresources.clear();
            if (texture.software_candidate_ticket)
            {
                static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
                texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
            }
            if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
            texture.software_transaction = false; texture.transaction_subresources.clear(); texture.transaction_next = 0;
            texture.prepared_package = {}; texture.bytes.Reset();
            continue;
        }
        else if (page->state != StreamingPageState::Decoded) continue;
        if (texture.software_transaction && TextureSoftwareTransactionSuperseded(
                texture.transaction_target_mip, texture.desired_mip, global_pressure || texture.budget_coarsened))
        {
            abandon_software_transaction(texture);
            continue;
        }
        const bool software_copy_coarsening = IsSoftwareCopyCoarsening(texture);
        const auto target_fit = texture.software_transaction && !software_copy_coarsening
            ? TextureTargetFitsHost(texture, texture.transaction_target_mip, host_budget_snapshot)
            : HostTargetFit::Fits;
        if (target_fit == HostTargetFit::Invalid)
            return Failure("Selected texture authenticated host range metadata is invalid.");
        if (target_fit == HostTargetFit::NoFit)
        {
            abandon_software_transaction(texture);
            continue;
        }
        const bool active = texture.reserved_published || texture.software.resource;
        if ((active && texture.resident_mip == texture.request_target_mip) ||
            (!active && !texture.reserved && texture.first_mip == texture.request_target_mip))
        {
            static_cast<void>(streaming.Cancel(texture.request));
            if (texture.read_ticket) static_cast<void>(residency.ReleaseNow(texture.read_ticket));
            if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
            texture.read_ticket = {}; texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
            texture.request = {};
            texture.request_ranges.clear(); texture.request_subresources.clear();
            texture.prepared_package = {}; texture.bytes.Reset();
            if (texture.software_candidate_ticket)
            {
                static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
                texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
            }
            if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
            texture.software_transaction = false; texture.transaction_subresources.clear(); texture.transaction_next = 0;
            continue;
        }
        if (auto result = impl_->Prepare(texture, texture.request_target_mip, residency, streaming); !result) return result;
    }
    // Pace optional reads at ingress, before host allocation and later copy preparation.
    // One 4K BC7 mip fits the quantum; a larger indivisible read can always go first.
    constexpr std::uint64_t read_quantum = 16ull * 1024 * 1024;
    std::uint64_t remaining_read_bytes = read_quantum;
    auto owner_index = impl_->next_read_owner;
    for (std::size_t visited = 0; visited < impl_->textures.size(); ++visited)
    {
        if (owner_index >= impl_->textures.size()) owner_index = 0;
        auto &texture = impl_->textures[owner_index++];
        if (texture.registration.pinned || texture.pending) continue;
        if (texture.software_transaction && !texture.request &&
            TextureSoftwareTransactionSuperseded(texture.transaction_target_mip, texture.desired_mip,
                global_pressure || texture.budget_coarsened))
        {
            // Pressure can obsolete a candidate, but expiring demand must not
            // repeatedly cancel a multi-chunk transaction before it publishes.
            abandon_software_transaction(texture);
            continue;
        }
        const bool software_copy_coarsening = IsSoftwareCopyCoarsening(texture);
        const auto target_fit = texture.software_transaction && !software_copy_coarsening
            ? TextureTargetFitsHost(texture, texture.transaction_target_mip, host_budget_snapshot)
            : HostTargetFit::Fits;
        if (target_fit == HostTargetFit::Invalid)
            return Failure("Selected texture authenticated host range metadata is invalid.");
        if (target_fit == HostTargetFit::NoFit)
        {
            const auto *page = texture.request ? streaming.Find(texture.request) : nullptr;
            if (!texture.request || (page && page->state != StreamingPageState::Reading &&
                    page->state != StreamingPageState::Uploading && page->state != StreamingPageState::Retiring))
                abandon_software_transaction(texture);
        }
        const bool active = texture.reserved_published || texture.software.resource;
        if (!texture.software_transaction && !texture.request && active && texture.resident_mip == texture.desired_mip) continue;
        if (!texture.software_transaction && !texture.request && !active &&
            (!texture.reserved || !texture.feedback_supported) &&
            texture.first_mip == texture.desired_mip) continue;
        if (!texture.request)
        {
            const bool continuing_software = texture.software_transaction;
            if (!continuing_software)
            {
                texture.request_target_mip = NextTextureTransactionTarget(texture.resident_mip,
                    texture.desired_mip, !!texture.reserved, texture.reserved_published, texture.tail_mip);
                texture.request_reserved = !!texture.reserved;
                if (!texture.request_reserved)
                {
                    texture.software_transaction = true;
                    texture.transaction_target_mip = texture.request_target_mip;
                    texture.transaction_subresources.clear(); texture.transaction_next = 0;
                    if (texture.software_candidate_ticket)
                    {
                        static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
                        texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
                    }
                    if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
                }
            }
            else
            {
                texture.request_target_mip = texture.transaction_target_mip;
                texture.request_reserved = false;
            }
            const bool copy_resident_mips = IsSoftwareCopyCoarsening(texture);
            const auto request = streaming.Request(texture.registration.logical_id,
                copy_resident_mips ? "resident-software-mip-copy" : "texture-package",
                texture.registration.generation, texture.priority);
            if (!request) return Failure("Could not register selected texture mip demand.");
            texture.request = request;
            ++texture.requests;
            texture.request_ranges.clear(); texture.request_subresources.clear();
            if (!texture.registration.descriptor.subresources.empty() && !copy_resident_mips)
            {
                const auto &descriptor = texture.registration.descriptor;
                if (!continuing_software)
                {
                    std::uint64_t payload_bytes{};
                    for (std::uint32_t slice = 0; slice < descriptor.description.array_size; ++slice)
                        for (std::uint32_t mip = texture.request_target_mip; mip < descriptor.description.mip_count; ++mip)
                        {
                            if (texture.reserved_published)
                            {
                                const auto packed = texture.packed.NumPackedMips && mip >= texture.packed.NumStandardMips;
                                const auto mapping_mip = packed ? static_cast<std::uint32_t>(texture.packed.NumStandardMips) : mip;
                                if (std::ranges::find(texture.backing, mapping_mip, &MipBacking::mip) != texture.backing.end()) continue;
                            }
                            const auto index = slice * descriptor.description.mip_count + mip;
                            const auto &layout = descriptor.subresources[index];
                            if (!layout.byte_count || payload_bytes > UINT64_MAX - layout.byte_count)
                                return Failure("Selected texture range extent overflow.");
                            payload_bytes += layout.byte_count;
                            if (texture.request_reserved)
                            {
                                texture.request_subresources.push_back(index);
                                texture.request_ranges.push_back({layout.offset, layout.byte_count, layout.digest});
                            }
                            else texture.transaction_subresources.push_back(index);
                        }
                    if (texture.request_reserved && !payload_bytes)
                        return Failure("Selected texture transaction has no missing payload ranges.");
                }
                if (!texture.request_reserved && texture.transaction_subresources.empty())
                    return Failure("Software texture replacement has no required mip ranges.");
            }
            if (!copy_resident_mips)
            {
                if (!MetadataReservationBytes(texture, texture.metadata_reservation_bytes))
                    return Failure("Selected texture prepared metadata size overflow.");
                if (!texture.decode_ticket) texture.metadata_ticket_committed = false;
            }
            // Each transaction owns a fixed target and only its authenticated subresource ranges.
        }
        const auto *page = streaming.Find(texture.request);
        if (!page) return Failure("Selected texture request ledger disappeared.");
        static_cast<void>(streaming.UpdatePriority(texture.request, texture.priority));
        if (page->state == StreamingPageState::Requested && IsSoftwareCopyCoarsening(texture))
        {
            if (auto result = impl_->PrepareSoftwareCopy(texture, residency, streaming); !result) return result;
            continue;
        }
        if (page->state == StreamingPageState::Requested && texture.software_transaction &&
            !texture.registration.descriptor.subresources.empty() &&
            !texture.software_upload_window.resource)
        {
            const auto host = residency.HostAdmissionFacts().budget;
            if (!host.host_budget_valid || !host.host_fresh) continue;
            bool ready{};
            if (auto result = impl_->EnsureSoftwareUploadWindow(texture, residency, ready); !result) return result;
            if (!ready) continue;
        }
        if (page->state == StreamingPageState::Requested)
        {
            if (!remaining_read_bytes) continue;
            const auto metadata_needed = texture.decode_ticket ? 0u : texture.metadata_reservation_bytes;
            if (!texture.request_reserved && texture.software_transaction &&
                !texture.registration.descriptor.subresources.empty() && texture.request_subresources.empty())
            {
                const auto admission_now = residency.HostAdmissionFacts();
                const auto &host = admission_now.budget;
                if (!host.host_budget_valid || !host.host_fresh) continue;
                if (admission_now.held_overflow)
                    return Failure("Software texture host accounting overflow.");
                const auto held = admission_now.held_bytes;
                if (metadata_needed > host.host_staging_budget && texture.software_transaction)
                {
                    abandon_software_transaction(texture);
                    continue;
                }
                if (metadata_needed > host.host_staging_budget)
                    return Failure("Software texture metadata table exceeds the configured host staging budget.");
                if (held > host.host_staging_budget - metadata_needed) continue;
                const auto payload_budget = host.host_staging_budget - held - metadata_needed;
                const auto chunk_end = TextureReadChunkEndWithinUploadWindow(
                    texture.transaction_subresources, texture.registration.descriptor.subresources,
                    texture.transaction_upload_footprints, texture.transaction_next, payload_budget,
                    D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, texture.software_upload_window_width);
                if (chunk_end == texture.transaction_next)
                {
                    if (texture.transaction_next >= texture.transaction_upload_plan.size() ||
                        texture.transaction_upload_plan[texture.transaction_next].allocation_bytes >
                            texture.software_upload_window_bytes)
                        return Failure("One authenticated texture subresource exceeds its admitted upload window.");
                    const auto intrinsic_payload_budget = host.host_staging_budget > texture.metadata_reservation_bytes
                        ? host.host_staging_budget - texture.metadata_reservation_bytes : 0u;
                    if (TextureReadChunkEnd(texture.transaction_subresources,
                            texture.registration.descriptor.subresources, texture.transaction_next,
                            intrinsic_payload_budget) != texture.transaction_next)
                        continue; // Other admitted owners currently hold the remaining host capacity.
                    // The intrinsic ceiling changed after the per-pump target
                    // selection. Drop this unpublished candidate; next Pump
                    // will clamp the demand using the new cached threshold.
                    abandon_software_transaction(texture);
                    continue;
                }
                for (auto cursor = texture.transaction_next; cursor < chunk_end; ++cursor)
                {
                    const auto index = texture.transaction_subresources[cursor];
                    const auto &layout = texture.registration.descriptor.subresources[index];
                    texture.request_subresources.push_back(index);
                    texture.request_ranges.push_back({layout.offset, layout.byte_count, layout.digest});
                }
            }
            std::uint64_t size{};
            if (!texture.request_ranges.empty())
                for (const auto &range : texture.request_ranges)
                {
                    if (size > UINT64_MAX - range.byte_count) return Failure("Selected texture range byte sum overflow.");
                    size += range.byte_count;
                }
            else if (auto sized = texture.registration.source.ByteCount(size); !sized) return sized;
            if (!size || size > UINT64_MAX - texture.metadata_reservation_bytes)
                return Failure("Selected texture range or metadata extent is invalid.");
            if (size > UINT64_MAX - metadata_needed) return Failure("Selected texture host peak overflow.");
            const auto host_peak = size + metadata_needed;
            const auto host_budget = residency.HostAdmissionFacts().budget;
            if (host_budget.host_budget_valid && host_budget.host_fresh &&
                host_peak > host_budget.host_staging_budget)
            {
                if (!TextureHostFootprintFitsBudget(texture.metadata_reservation_bytes, size,
                        kMaximumHostStagingBudgetBytes))
                    return Failure("Selected texture request footprint exceeds the maximum host staging budget.");
                continue;
            }
            if (!residency.CanStage(host_peak)) continue;
            if (remaining_read_bytes != read_quantum && size > remaining_read_bytes)
            {
                impl_->next_read_owner = owner_index - 1;
                remaining_read_bytes = 0;
                continue;
            }
            texture.read_ticket = residency.ReserveHost(size);
            if (!texture.decode_ticket) texture.decode_ticket = residency.ReserveHost(texture.metadata_reservation_bytes);
            if (!texture.read_ticket || !texture.decode_ticket)
            {
                if (texture.read_ticket) static_cast<void>(residency.ReleaseNow(texture.read_ticket));
                if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
                texture.read_ticket = {};
                texture.decode_ticket = {};
                return Failure("Selected texture host reservation failed.");
            }
            bool submitted{};
            auto result = io.Enqueue({texture.request, texture.registration.generation, texture.registration.logical_id,
                texture.registration.source, size, texture.registration.generation_lease, {}, texture.request_ranges}, streaming, residency,
                texture.read_ticket, submitted, true);
            if (submitted)
            {
                remaining_read_bytes -= std::min(remaining_read_bytes, size);
                impl_->next_read_owner = owner_index;
            }
            if (!submitted)
            {
                static_cast<void>(residency.ReleaseNow(texture.read_ticket));
                if (texture.decode_ticket && !texture.metadata_ticket_committed)
                {
                    static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
                    texture.decode_ticket = {};
                }
            }
            texture.read_ticket = {};
            if (!result)
            {
                if (texture.decode_ticket) static_cast<void>(residency.ReleaseNow(texture.decode_ticket));
                texture.decode_ticket = {}; texture.metadata_ticket_committed = false;
                if (texture.software_candidate_ticket)
                {
                    static_cast<void>(residency.ReleaseNow(texture.software_candidate_ticket));
                    texture.software_candidate_ticket = {}; texture.software_candidate.Reset();
                }
                if (ReleaseSoftwareUploadWindow(texture, residency)) ++texture.reclaims;
                texture.software_transaction = false; texture.transaction_subresources.clear(); texture.transaction_next = 0;
                texture.request = {};
                texture.prepared_package = {}; texture.bytes.Reset();
                return result;
            }
        }
    }
    static_cast<void>(residency.Collect(completed)); static_cast<void>(streaming.Collect(completed));
    if (!impl_->preferred_working_set.empty() && observed.valid && observed.local_budget && !global_pressure &&
        host_budget_snapshot.host_budget_valid && host_budget_snapshot.host_fresh &&
        impl_->retired.empty() && planned <= budget)
    {
        const auto settled = std::ranges::all_of(impl_->textures, [&](const TextureOwner &texture) {
            if (texture.registration.pinned) return true;
            if (texture.pending || texture.request || texture.software_transaction || texture.budget_coarsened)
                return false;
            const auto target = texture.preferred ? texture.resident_mip : texture.tail_mip;
            if (texture.desired_mip != target || texture.resident_mip != target ||
                (!texture.IsStreamed() && ((texture.reserved && texture.feedback_supported) || texture.first_mip != target))) return false;
            return texture.preferred || !texture.demand_valid || texture.demand_frame > frame ||
                frame - texture.demand_frame > impl_->frame_count;
        });
        if (settled)
        {
            impl_->settled_planned = planned;
            impl_->settled_physical = impl_->PhysicalBytes(observed.unified_memory);
            impl_->settled_host_budget = host_budget_snapshot.host_staging_budget;
            impl_->settled_unified_memory = observed.unified_memory;
            impl_->settled_preferred = true;
        }
    }
    return Result::Success();
}
TextureStreamView RendererTextureStreaming::View(TextureStreamId id) const noexcept
{
    if (!id || id.value > impl_->textures.size()) return {};
    const auto &texture = impl_->textures[id.value - 1];
    auto *resource = texture.reserved_published ? texture.reserved.Get()
        : texture.software.resource ? texture.software.resource.Get() : texture.registration.fallback.Get();
    auto resident_mip = texture.resident_mip;
    // New frames clamp before a shrink batch can unmap fine backing. Its graph
    // import waits all older readers; growth publishes only after completion.
    if (texture.reserved_published && texture.pending)
        resident_mip = std::max(resident_mip, texture.pending->target_mip);
    auto completion = texture.last_use; completion.Merge(texture.publication);
    return {resource, texture.registration.description, texture.first_mip, resident_mip,
        texture.revision, completion, texture.active_access, texture.reserved_published,
        texture.registration.opacity_sampling, texture.feedback_supported, texture.active_queue};
}
bool RendererTextureStreaming::NeedsCpuDemand(TextureStreamId id) const noexcept
{
    if (!id || id.value > impl_->textures.size()) return false;
    const auto &texture = impl_->textures[id.value - 1];
    return !texture.registration.pinned &&
        (!texture.reserved_published || !texture.feedback_supported);
}
bool RendererTextureStreaming::HasPendingUploads() const noexcept
{
    return impl_->pending_uploads != 0;
}

bool RendererTextureStreaming::MatchesLayout(const TextureStreamingLayout &layout) const noexcept
{
    if (layout.entries.size() != impl_->textures.size()) return false;
    for (std::size_t index = 0; index < impl_->textures.size(); ++index)
    {
        const auto view = View({static_cast<std::uint32_t>(index + 1u)});
        const auto &entry = layout.entries[index];
        if (bool(view.resource) != bool(entry.sampled) ||
            !Same(entry.description, view.logical_description) || entry.first_mip != view.first_mip ||
            entry.opacity_sampling != view.opacity_sampling) return false;
    }
    return true;
}

Result RendererTextureStreaming::ImportViews(RenderGraphBuilder &graph,
    std::vector<TextureStreamingGraphBinding> &bindings, TextureStreamingLayout *layout)
{
    return ImportViewsImpl(graph, bindings, layout, nullptr);
}
Result RendererTextureStreaming::BindRetained(RenderGraphBuilder &graph,
    const TextureStreamingLayout &layout, std::vector<TextureStreamingGraphBinding> &bindings)
{
    return ImportViewsImpl(graph, bindings, nullptr, &layout);
}
Result RendererTextureStreaming::ImportViewsImpl(RenderGraphBuilder &graph,
    std::vector<TextureStreamingGraphBinding> &bindings,
    TextureStreamingLayout *layout, const TextureStreamingLayout *retained)
{
    if (retained && retained->entries.size() != impl_->textures.size())
        return Failure("Texture sampled import count changed.");
    if (layout) layout->entries.resize(impl_->textures.size());
    bindings.clear(); bindings.reserve(impl_->textures.size());
    for (std::size_t index = 0; index < impl_->textures.size(); ++index)
    {
        auto &texture = impl_->textures[index];
        const auto view = View({static_cast<std::uint32_t>(index + 1u)});
        const NativeResourceContract *contract{};
        if (auto certified = TextureContract(texture, view.resource, impl_->device.Get(), contract); !certified)
            return certified;
        const ExternalTexture external{view.resource, view.completion, view.queue, {}, contract};
        const auto handle = !view.resource ? TextureHandle{} : retained ? retained->entries[index].sampled :
            graph.ImportTexture(external, view.access, texture.registration.logical_id);
        if (view.resource && retained && !graph.BindTexture(handle, external, view.access))
            return Failure("Texture streaming sampled import changed.");
        const ResourceVersion version{handle, 0};
        if (!retained && handle) graph.SetFinalAccess(version, Access::AllShaderRead);
        if (layout) layout->entries[index] = {handle, view.logical_description, view.first_mip, view.opacity_sampling};
        const auto required_mip = std::min(texture.tail_mip, impl_->finest_mip);
        bindings.push_back({{static_cast<std::uint32_t>(index + 1u)}, version,
            texture.registration.logical_id, view.resident_min_mip, required_mip, texture.desired_mip,
            view.resource && !texture.pending && texture.desired_mip <= required_mip &&
                view.resident_min_mip <= required_mip, view.opacity_sampling});
    }
    return Result::Success();
}

Result RendererTextureStreaming::AddUploadPass(RenderGraphBuilder &graph,
    ID3D12CommandQueue *copy_queue, std::uint32_t pass_id)
{
    if (!HasPendingUploads()) return Result::Success();
    struct CopyBatch
    {
        struct Upload { std::size_t texture; std::shared_ptr<UploadBatch> batch; };
        std::vector<Upload> uploads;
    };
    auto copies = std::make_shared<CopyBatch>();
    copies->uploads.reserve(impl_->pending_uploads);
    for (std::size_t index = 0; index < impl_->textures.size(); ++index)
    {
        const auto &texture = impl_->textures[index];
        if (texture.pending && !texture.pending->submitted) copies->uploads.push_back({index, texture.pending});
    }
    const auto has_mapping = [](const auto &batch) { return !batch->maps.empty() || !batch->unmaps.empty(); };
    const auto has_copy = [](const auto &batch) { return batch->software_copy || !batch->footprints.empty(); };
    const bool maps = std::ranges::any_of(copies->uploads, [&](const auto &upload) { return has_mapping(upload.batch); });
    const bool transfer = std::ranges::any_of(copies->uploads, [&](const auto &upload) { return has_copy(upload.batch); });
    if (maps && (!copy_queue || copy_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_COPY))
        return Failure("Texture mapping requires the graph's actual owned Copy queue.");
    std::optional<PassBuilder> mapping_pass, copy_pass;
    if (maps) mapping_pass = graph.AddPass(pass_id, "Texture mip map/unmap", QueueHint::Copy);
    if (transfer) copy_pass = graph.AddPass(pass_id + 1u, "Texture mip upload/copy", QueueHint::Copy);
    std::vector<ExternalResourceAccess> mapping_accesses;
    if (maps) mapping_accesses.reserve(copies->uploads.size());
    for (const auto &upload : copies->uploads)
    {
        auto &texture = impl_->textures[upload.texture];
        const auto &batch = upload.batch;
        const auto view = View({static_cast<std::uint32_t>(upload.texture + 1u)});
        if (batch->software_copy && (batch->reserved || batch->upload.resource ||
            batch->software_copy_source.Get() != view.resource || batch->resource.Get() == view.resource))
            return Failure("Software texture copy lost its published source or separate candidate.");
        TextureHandle active{};
        if (batch->software_copy || batch->resource.Get() == view.resource)
        {
            const NativeResourceContract *contract{};
            if (auto certified = TextureContract(texture, view.resource, impl_->device.Get(), contract); !certified)
                return certified;
            active = graph.ImportTexture({view.resource, view.completion, view.queue, {}, contract},
                view.access, texture.registration.logical_id);
        }
        auto target = active;
        if (batch->resource.Get() != view.resource)
        {
            const NativeResourceContract *contract{};
            if (auto certified = TextureContract(texture, batch->resource.Get(), impl_->device.Get(), contract); !certified)
                return certified;
            target = graph.ImportTexture({batch->resource.Get(), {}, QueueHint::Copy, {}, contract},
                Access::Common, "selected texture candidate");
        }
        auto written = graph.Version(target);
        if (has_mapping(batch))
        {
            written = mapping_pass->Write(written, Access::CopyDest);
            mapping_accesses.push_back({target, Access::Common, Access::Common});
        }
        if (has_copy(batch))
        {
            if (batch->software_copy)
            {
                copy_pass->Read(graph.Version(active), Access::CopySource);
                graph.SetFinalAccess(graph.Version(active), Access::Common);
            }
            written = copy_pass->Write(written, Access::CopyDest);
        }
        graph.SetFinalAccess(written, Access::Common);
    }
    if (mapping_pass)
    {
        auto queue_owner = std::make_shared<ComPtr<ID3D12CommandQueue>>(copy_queue);
        ExternalQueueBinding queue{std::shared_ptr<void>(queue_owner, copy_queue), QueueHint::Copy, copies};
        mapping_pass->SetExternalExecute(std::move(queue), mapping_accesses,
            [copies](RenderExternalPassContext &context, ExternalQueueCompletion &completion) {
                completion.resource_lease = copies;
                auto *native_queue = static_cast<ID3D12CommandQueue *>(context.NativeCommandQueue());
                if (!native_queue || std::ranges::any_of(copies->uploads, [](const auto &upload) { return upload.batch->started; }))
                    return Failure("Texture copy/mapping batch submitted twice.");
                for (const auto &upload : copies->uploads) upload.batch->started = true;
                for (const auto &upload : copies->uploads)
                {
                    const auto &batch = upload.batch;
                    for (const auto &backing : batch->maps)
                        for (const auto &mapping : backing.mappings)
                        {
                            D3D12_TILE_REGION_SIZE region{}; region.NumTiles = mapping.tiles;
                            const D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
                            const UINT offset = static_cast<UINT>(backing.memory.allocation->GetOffset() /
                                D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) + mapping.heap_tile;
                            native_queue->UpdateTileMappings(batch->resource.Get(), 1, &mapping.coordinate, &region,
                                backing.memory.allocation->GetHeap(), 1, &flags, &offset, &mapping.tiles, D3D12_TILE_MAPPING_FLAG_NONE);
                        }
                    for (const auto &mapping : batch->unmaps)
                    {
                        D3D12_TILE_REGION_SIZE region{}; region.NumTiles = mapping.tiles;
                        const D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NULL; const UINT offset{};
                        native_queue->UpdateTileMappings(batch->resource.Get(), 1, &mapping.coordinate, &region,
                            nullptr, 1, &flags, &offset, &mapping.tiles, D3D12_TILE_MAPPING_FLAG_NONE);
                    }
                }
                return Result::Success();
            });
    }
    if (copy_pass)
        copy_pass->SetExecuteChecked([copies](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            for (const auto &upload : copies->uploads)
            {
                const auto &batch = upload.batch;
                if (batch->software_copy)
                {
                    // Both complete array chains and the subresource span were admitted by Prepare.
                    for (UINT slice = 0; slice < batch->copy_array_size; ++slice)
                        for (UINT mip = 0; mip < batch->copy_destination_mips; ++mip)
                        {
                            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
                            source.pResource = batch->software_copy_source.Get();
                            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            source.SubresourceIndex = slice * batch->copy_source_mips +
                                batch->copy_source_mips - batch->copy_destination_mips + mip;
                            destination.pResource = batch->resource.Get();
                            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            destination.SubresourceIndex = slice * batch->copy_destination_mips + mip;
                            commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                        }
                }
                else
                    for (std::size_t index = 0; index < batch->footprints.size(); ++index)
                    {
                        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
                        source.pResource = batch->upload.resource.Get();
                        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        source.PlacedFootprint = batch->footprints[index];
                        destination.pResource = batch->resource.Get();
                        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        destination.SubresourceIndex = batch->destinations[index];
                        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                    }
            }
            return Result::Success();
        });
    for (const auto &upload : copies->uploads) upload.batch->scheduled = true;
    return Result::Success();
}
void RendererTextureStreaming::MarkSubmitted(const CompletionSet &completion, StreamingManager &streaming) noexcept
{
    for (auto &texture : impl_->textures)
    {
        const auto batch = texture.pending;
        if (!batch || !batch->scheduled || batch->submitted) continue;
        batch->started = true;
        batch->completion = completion; batch->submitted = true;
        --impl_->pending_uploads;
        auto *active = texture.reserved_published ? texture.reserved.Get() :
            texture.software.resource ? texture.software.resource.Get() : texture.registration.fallback.Get();
        if (active && (batch->resource.Get() == active || batch->software_copy_source.Get() == active))
        {
            texture.last_use.Merge(completion);
            texture.active_access = Access::Common;
            texture.active_queue = QueueHint::Copy;
        }
        if (streaming.SubmitUpload(batch->request, texture.registration.generation, completion))
            ++texture.uploads;
    }
}
void RendererTextureStreaming::MarkUse(TextureStreamId id, const CompletionSet &completion) noexcept
{
    if (!id || id.value > impl_->textures.size()) return;
    auto &texture = impl_->textures[id.value - 1]; texture.last_use.Merge(completion);
    // A hardware reserved resource or software replacement owns the active
    // view once published. Until then, sampling uses the registered fallback
    // allocation and advances its mutable state as well.
    if (!texture.IsStreamed()) texture.fallback_access = Access::AllShaderRead;
    texture.active_access = Access::AllShaderRead;
    texture.active_queue = QueueHint::Direct;
}
bool RendererTextureStreaming::HasStartedWork() const noexcept
{
    return std::ranges::any_of(impl_->textures, [](const auto &texture) {
        return texture.pending && texture.pending->started;
    });
}
void RendererTextureStreaming::CancelReads(RendererAssetIo &io, StreamingManager &streaming) noexcept
{
    for (auto &texture : impl_->textures) if (texture.request && !texture.pending)
    {
        if (IsSoftwareCopyCoarsening(texture)) static_cast<void>(streaming.Cancel(texture.request));
        else io.Cancel(texture.request, streaming);
    }
}
void RendererTextureStreaming::SwapState(RendererTextureStreaming &other) noexcept { impl_.swap(other.impl_); }
std::uint64_t RendererTextureStreaming::PhysicalBytes() const noexcept
{ return impl_->PhysicalBytes(true); }

std::uint64_t RendererTextureStreaming::Impl::PhysicalBytes(bool include_uploads) const noexcept
{
    std::uint64_t bytes{};
    for (const auto &texture : textures)
    {
        bytes += texture.registration.fallback_allocation_bytes;
        if (texture.software.allocation) bytes += texture.software.allocation->GetSize();
        for (const auto &backing : texture.backing) bytes += backing.memory.allocation->GetSize();
        if (texture.pending)
        {
            if (texture.pending->candidate_owner.allocation) bytes += texture.pending->candidate_owner.allocation->GetSize();
            if (include_uploads && texture.pending->upload.allocation) bytes += texture.pending->upload.allocation->GetSize();
            for (const auto &backing : texture.pending->maps) bytes += backing.memory.allocation->GetSize();
        }
        if (texture.software_candidate.allocation) bytes += texture.software_candidate.allocation->GetSize();
        if (include_uploads && texture.software_upload_window.allocation) bytes += texture.software_upload_window.allocation->GetSize();
    }
    for (const auto &entry : retired) if (entry.texture.allocation) bytes += entry.texture.allocation->GetSize();
    return bytes;
}
std::uint32_t RendererTextureStreaming::Count() const noexcept { return static_cast<std::uint32_t>(impl_->textures.size()); }
TextureStreamingStatistics RendererTextureStreaming::Statistics(const StreamingManager &streaming,
    const CompletionSet &) const noexcept
{
    TextureStreamingStatistics result;
    if (!impl_->textures.empty())
    {
        result.revision = 1;
        result.state = static_cast<std::uint8_t>(StreamingPageState::Resident);
    }
    bool has_request{};
    bool has_publication{};
    for (const auto &texture : impl_->textures)
    {
        result.requests += texture.requests;
        result.uploads += texture.uploads;
        result.publications += texture.publications;
        result.reclaims += texture.reclaims;
        result.revision = std::max(result.revision, texture.revision);
        has_publication = has_publication || texture.publications != 0;
        if (!texture.request) continue;
        has_request = true;
        const auto *page = streaming.Find(texture.request);
        const auto state = page ? static_cast<std::uint8_t>(page->state)
                                : static_cast<std::uint8_t>(StreamingPageState::Cancelled);
        if (state == static_cast<std::uint8_t>(StreamingPageState::Cancelled) || state < result.state)
            result.state = state;
    }
    if (!has_request) result.state = has_publication
        ? static_cast<std::uint8_t>(StreamingPageState::Resident)
        : static_cast<std::uint8_t>(StreamingPageState::Cancelled);
    return result;
}
TextureStreamingProductStatus RendererTextureStreaming::ProductStatus(std::string_view logical_id,
    const StreamingManager &, const CompletionSet &completed) const noexcept
{
    for (const auto &texture : impl_->textures)
    {
        if (texture.registration.logical_id != logical_id) continue;
        TextureStreamingProductStatus result;
        result.registered = true;
        result.pending = !!texture.request || !!texture.pending;
        result.requests = texture.requests;
        result.uploads = texture.uploads;
        result.publications = texture.publications;
        result.reclaims = texture.reclaims;
        result.revision = texture.revision;
        result.actual_resident_mip = texture.resident_mip;
        result.effective_desired_mip = texture.desired_mip;
        result.target_mip = texture.pending ? texture.pending->target_mip : texture.desired_mip;
        result.publication_complete = texture.IsStreamed() && texture.publications != 0 && !result.pending &&
            texture.publication.IsSatisfiedBy(completed);
        return result;
    }
    return {};
}
std::optional<bool> RendererTextureStreaming::RequiredSourceMipFitsMaximumHostBudget(
    TextureStreamId id, std::uint32_t required_mip) const noexcept
{
    if (!id || id.value > impl_->textures.size()) return std::nullopt;
    const auto &texture = impl_->textures[id.value - 1];
    const auto view = View(id);
    if (view.resource && view.resident_min_mip <= required_mip) return true;
    if (texture.pending || texture.request || texture.registration.descriptor.subresources.empty() ||
        required_mip > texture.tail_mip) return std::nullopt;
    std::uint64_t metadata_bytes{};
    if (!MetadataReservationBytes(texture, metadata_bytes)) return std::nullopt;
    if (metadata_bytes > kMaximumHostStagingBudgetBytes) return false;
    const auto payload_limit = kMaximumHostStagingBudgetBytes - metadata_bytes;
    if (!texture.reserved)
    {
        for (std::uint32_t slice = 0; slice < texture.registration.description.array_size; ++slice)
            for (std::uint32_t mip = required_mip; mip < texture.registration.description.mip_count; ++mip)
            {
                const auto &source = texture.registration.descriptor.subresources[
                    static_cast<std::size_t>(slice) * texture.registration.description.mip_count + mip];
                if (!source.has_digest || !source.byte_count) return std::nullopt;
                if (source.byte_count > payload_limit) return false;
            }
        return true;
    }
    const auto transaction_target = texture.reserved_published
        ? NextTextureTransactionTarget(texture.resident_mip, required_mip, true, true, texture.tail_mip)
        : texture.tail_mip;
    std::uint64_t payload_bytes{};
    for (std::uint32_t slice = 0; slice < texture.registration.description.array_size; ++slice)
        for (std::uint32_t mip = transaction_target; mip < texture.registration.description.mip_count; ++mip)
        {
            if (texture.reserved_published)
            {
                const auto mapping_mip = texture.packed.NumPackedMips && mip >= texture.packed.NumStandardMips
                    ? static_cast<std::uint32_t>(texture.packed.NumStandardMips) : mip;
                if (std::ranges::find(texture.backing, mapping_mip, &MipBacking::mip) != texture.backing.end()) continue;
            }
            const auto &source = texture.registration.descriptor.subresources[
                static_cast<std::size_t>(slice) * texture.registration.description.mip_count + mip];
            if (!source.has_digest || !source.byte_count) return std::nullopt;
            if (payload_bytes > payload_limit || source.byte_count > payload_limit - payload_bytes) return false;
            payload_bytes += source.byte_count;
        }
    return TextureHostFootprintFitsBudget(metadata_bytes, payload_bytes,
        kMaximumHostStagingBudgetBytes);
}
std::optional<bool> RendererTextureStreaming::RequiredMipSetFitsConfiguredBudget(
    std::span<const TextureStreamMipRequirement> requirements) const noexcept
{
    if (requirements.empty()) return true;
    bool ready = true;
    for (const auto requirement : requirements)
    {
        if (!requirement.texture || requirement.texture.value > impl_->textures.size()) return std::nullopt;
        const auto &texture = impl_->textures[requirement.texture.value - 1];
        if (requirement.mip > texture.tail_mip) return std::nullopt;
        const auto view = View(requirement.texture);
        ready = ready && !texture.pending && texture.desired_mip <= requirement.mip &&
            view.resource && view.resident_min_mip <= requirement.mip;
    }
    if (ready || !impl_->budget) return true;

    // The registered minimum includes every pinned fallback and every
    // unpinned fallback plus its coarse streamed tail. Add only the detail
    // above each required capture tail; no transient adapter-pressure facts
    // participate in this intrinsic configured-cap check.
    auto required_bytes = impl_->MinimumBudget();
    if (required_bytes == UINT64_MAX) return std::nullopt;
    for (const auto requirement : requirements)
    {
        const auto &texture = impl_->textures[requirement.texture.value - 1];
        if (texture.registration.pinned) continue;
        const auto target_cost = impl_->Cost(texture, requirement.mip);
        const auto tail_cost = impl_->Cost(texture, texture.tail_mip);
        if (target_cost == UINT64_MAX || tail_cost == UINT64_MAX || target_cost < tail_cost ||
            !TextureAccumulateBudgetBytes(required_bytes, target_cost - tail_cost))
            return std::nullopt;
    }
    return required_bytes <= impl_->budget;
}
} // namespace hs
