#include "renderer_texture_streaming.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
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
D3D12_RESOURCE_DESC Description(const render_data::TextureDescription &source,
                               std::uint32_t first_mip)
{
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.Width = std::max(1u, source.width >> first_mip);
    result.Height = std::max(1u, source.height >> first_mip);
    result.DepthOrArraySize = static_cast<UINT16>(source.array_size);
    result.MipLevels = static_cast<UINT16>(source.mip_count - first_mip);
    result.Format = static_cast<DXGI_FORMAT>(source.format);
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
    AllocationResource replacement, upload;
    ResidencyTicket replacement_ticket, upload_ticket;
    std::vector<MipBacking> maps;
    std::vector<std::uint32_t> unmap_mips;
    std::vector<TileMapping> unmaps;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    StreamingRequestId request;
    std::uint32_t target_mip{};
    CompletionSet completion;
    bool reserved{}, scheduled{}, started{}, submitted{};
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
    D3D12_PACKED_MIP_INFO packed{};
    std::vector<D3D12_SUBRESOURCE_TILING> tilings;
    std::vector<MipBacking> backing;
    AllocationResource software;
    ResidencyTicket software_ticket;
    std::shared_ptr<UploadBatch> pending;
    StreamingRequestId request;
    std::vector<std::byte> bytes;
    ResidencyTicket read_ticket, decode_ticket;
    std::uint64_t revision{1}, requests{}, uploads{}, publications{}, reclaims{};
    std::uint64_t demand_frame{}, publication_frame{};
    std::uint32_t first_mip{}, resident_mip{}, tail_mip{}, desired_mip{}, observed_mip{}, priority{};
    CompletionSet last_use, publication;
    // Mutable state of the pinned fallback allocation. The registration value
    // remains the immutable state produced by its upload publication.
    Access fallback_access{Access::Undefined};
    Access active_access{Access::Undefined};
    bool reserved_published{}, demand_valid{};

    [[nodiscard]] bool IsStreamed() const noexcept
    {
        return reserved_published || software.resource.Get() != nullptr;
    }
};
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
    std::vector<RetiredTexture> retired;
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
    std::uint64_t Cost(const TextureOwner &texture, std::uint32_t mip) const
    {
        if (!texture.reserved)
        {
            if (mip == texture.registration.fallback_first_mip) return 0;
            const auto description = Description(texture.registration.description, mip);
            return device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
        }
        std::uint64_t tiles{};
        const auto &description = texture.registration.description;
        const auto end = texture.packed.NumPackedMips ? texture.packed.NumStandardMips : description.mip_count;
        for (auto level = mip; level < end; ++level)
            for (UINT slice = 0; slice < description.array_size; ++slice)
            {
                const auto &tiling = texture.tilings[slice * description.mip_count + level];
                tiles += static_cast<UINT64>(tiling.WidthInTiles) * tiling.HeightInTiles * tiling.DepthInTiles;
            }
        if (texture.packed.NumPackedMips)
            tiles += static_cast<UINT64>(texture.packed.NumTilesForPackedMips) * description.array_size;
        return tiles * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    }
    std::uint64_t MinimumBudget() const
    {
        std::uint64_t bytes{};
        for (const auto &texture : textures)
        {
            const auto tail = texture.registration.pinned ? 0u : Cost(texture, texture.tail_mip);
            const auto fallback = texture.registration.fallback_allocation_bytes;
            if (tail == UINT64_MAX || tail > UINT64_MAX - fallback || bytes > UINT64_MAX - tail - fallback)
                return UINT64_MAX;
            bytes += tail + fallback;
        }
        return bytes;
    }
    Result Prepare(TextureOwner &, std::uint32_t, ResidencyManager &, StreamingManager &);
};
RendererTextureStreaming::RendererTextureStreaming() : impl_(std::make_unique<Impl>()) {}
RendererTextureStreaming::~RendererTextureStreaming() = default;
Result RendererTextureStreaming::Initialize(ID3D12Device *device, D3D12MA::Allocator *allocator,
    GraphicsQuality quality, std::uint32_t finest_mip, std::uint32_t budget_mib, TextureStreamingCapabilities &capabilities,
    std::uint32_t frame_count)
{
    if (!device || !allocator || impl_->device || !frame_count)
        return Failure("Texture streaming initialization requires one device, allocator and budget.");
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_5};
    ComPtr<ID3D12Device8> device8;
    capabilities.tiled_resources = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
        &options, sizeof(options))) && options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2;
    capabilities.sampler_feedback = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
        &options7, sizeof(options7))) && options7.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED &&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device8))) &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shader_model, sizeof(shader_model))) &&
        shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
    // Reserved resources are sampled only by the actual feedback PSO variants.
    // This authored feedback ABI exposes 90 UAV bindings, beyond tier2's 64.
    capabilities.tiled_resources = capabilities.tiled_resources && capabilities.sampler_feedback &&
        options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
    capabilities.packed_mip_arrays = capabilities.tiled_resources &&
        options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_4;
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
    impl_->quality = quality;
    impl_->finest_mip = finest_mip;
    impl_->budget = static_cast<std::uint64_t>(budget_mib) * 1024u * 1024u;
    return Result::Success();
}
Result RendererTextureStreaming::Register(TextureStreamRegistration registration,
                                          ResidencyManager &residency, TextureStreamId &id)
{
    id = {};
    if (!impl_->device || !registration.fallback || !registration.generation_lease ||
        registration.generation.empty() || registration.logical_id.empty() || registration.path.empty() ||
        !registration.fallback_allocation_bytes || !registration.description.width || !registration.description.height ||
        !registration.description.mip_count || registration.fallback_graph_access == Access::Undefined ||
        registration.description.mip_count > 32 || !registration.description.array_size ||
        registration.description.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        registration.description.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        registration.description.array_size > D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
        registration.description.mip_count > std::bit_width(std::max(registration.description.width, registration.description.height)) ||
        registration.fallback_first_mip >= registration.description.mip_count ||
        (registration.pinned && registration.fallback_first_mip))
        return Failure("Invalid selected texture streaming registration.");
    const auto fallback = registration.fallback->GetDesc();
    const auto expected = Description(registration.description, registration.fallback_first_mip);
    D3D12_FEATURE_DATA_FORMAT_INFO planes{expected.Format, 0};
    if (fallback.Dimension != expected.Dimension || fallback.Width != expected.Width ||
        fallback.Height != expected.Height || fallback.DepthOrArraySize != expected.DepthOrArraySize ||
        fallback.MipLevels != expected.MipLevels || fallback.Format != expected.Format ||
        fallback.SampleDesc.Count != 1 || fallback.SampleDesc.Quality ||
        FAILED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &planes, sizeof(planes))) ||
        planes.PlaneCount != 1)
        return Failure("Selected texture fallback/native subresources disagree with immutable metadata.");
    for (std::size_t index = 0; index < impl_->textures.size(); ++index)
    {
        const auto &existing = impl_->textures[index].registration;
        if (existing.logical_id != registration.logical_id) continue;
        if (existing.generation != registration.generation || existing.path != registration.path ||
            !Same(existing.description, registration.description) || existing.fallback.Get() != registration.fallback.Get() ||
            existing.fallback_first_mip != registration.fallback_first_mip ||
            existing.fallback_allocation_bytes != registration.fallback_allocation_bytes ||
            existing.fallback_graph_access != registration.fallback_graph_access ||
            existing.pinned != registration.pinned)
            return Failure("Texture aliases disagree with the actual selected product owner.");
        id.value = static_cast<std::uint32_t>(index + 1); return Result::Success();
    }
    TextureOwner texture; texture.registration = std::move(registration);
    texture.fallback_access = texture.registration.fallback_graph_access;
    texture.active_access = texture.fallback_access;
    texture.first_mip = texture.resident_mip = texture.registration.fallback_first_mip;
    texture.tail_mip = std::min(4u, texture.registration.description.mip_count - 1u);
    texture.desired_mip = texture.tail_mip;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT format{static_cast<DXGI_FORMAT>(texture.registration.description.format)};
    const bool feedback_format = SUCCEEDED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
        &format, sizeof(format))) && (format.Support2 & D3D12_FORMAT_SUPPORT2_SAMPLER_FEEDBACK) != 0;
    // Full-chain authored arrays contain packed tails. D3D12 prohibits that
    // reserved layout before tier4; select their software owner before allocation.
    const bool array_supported = texture.registration.description.array_size == 1u ||
        impl_->capabilities.packed_mip_arrays;
    if (!texture.registration.pinned && impl_->capabilities.tiled_resources && array_supported && feedback_format &&
        texture.registration.description.width >= 8u && texture.registration.description.height >= 8u)
    {
        auto description = Description(texture.registration.description, 0);
        description.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
        const auto hr = impl_->device->CreateReservedResource(&description, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&texture.reserved));
        if (FAILED(hr)) return NativeFailure("Create reserved selected texture", hr);
        UINT total_tiles{}, count = texture.registration.description.array_size * texture.registration.description.mip_count;
        texture.tilings.resize(count); D3D12_TILE_SHAPE shape{};
        impl_->device->GetResourceTiling(texture.reserved.Get(), &total_tiles, &texture.packed, &shape,
            &count, 0, texture.tilings.data());
        if (!total_tiles || count != texture.tilings.size()) return Failure("Native selected texture tiling is incomplete.");
        if (texture.packed.NumPackedMips) texture.tail_mip = std::min(texture.tail_mip,
            static_cast<std::uint32_t>(texture.packed.NumStandardMips));
    }
    texture.fallback_ticket = texture.registration.fallback_residency;
    if (texture.fallback_ticket && (!residency.Contains(texture.fallback_ticket) || !residency.IsPinned(texture.fallback_ticket)))
        return Failure("Selected texture's existing fallback accounting is invalid.");
    if (!texture.fallback_ticket) texture.fallback_ticket = residency.Register(ResidencyCategory::Texture,
        texture.registration.fallback_allocation_bytes, true);
    if (!texture.fallback_ticket) return Failure("Could not account selected pinned texture tail.");
    texture.observed_mip = texture.desired_mip = texture.tail_mip;
    try { impl_->textures.push_back(std::move(texture)); }
    catch (...) { if (!texture.registration.fallback_residency) static_cast<void>(residency.ReleaseNow(texture.fallback_ticket)); throw; }
    id.value = static_cast<std::uint32_t>(impl_->textures.size());
    return Result::Success();
}
void RendererTextureStreaming::Demand(TextureStreamId id, std::uint32_t mip, std::uint32_t priority,
                                      std::uint64_t frame) noexcept
{
    if (!id || id.value > impl_->textures.size()) return;
    auto &texture = impl_->textures[id.value - 1];
    if (!texture.demand_valid || texture.demand_frame != frame)
    { texture.observed_mip = texture.tail_mip; texture.priority = 0; }
    texture.demand_valid = true;
    texture.demand_frame = frame; texture.priority = std::max(texture.priority, priority);
    texture.observed_mip = std::min(texture.observed_mip, std::min(mip, texture.tail_mip));
}
Result RendererTextureStreaming::Impl::Prepare(TextureOwner &texture, std::uint32_t target,
                                               ResidencyManager &residency, StreamingManager &streaming)
{
    if (target > texture.tail_mip || texture.revision == UINT64_MAX)
        return Failure("Selected texture mip target or descriptor revision is invalid.");
    render_data::TexturePackageView package;
    try { package = render_data::ReadTexturePackage(texture.bytes, MakeAssetId(texture.registration.logical_id)); }
    catch (const std::exception &error) { return Failure("Selected texture decode failed: " + std::string(error.what())); }
    if (!Same(package.description, texture.registration.description))
        return Failure("Selected texture metadata changed inside its immutable Generation.");
    auto batch = std::make_shared<UploadBatch>();
    batch->target_mip = target; batch->request = texture.request; batch->reserved = !!texture.reserved;
    batch->resource = texture.reserved;
    const auto description = Description(texture.registration.description, batch->reserved ? 0u : target);
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
    else backing_bytes = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
    // A reserved batch with no new mappings only changes tile mappings. Its
    // requested mips already contain the package data, so keep the mapping
    // batch independent of GPU upload admission.
    const bool upload_needed = !batch->reserved || !batch->maps.empty();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    std::vector<UINT64> row_bytes;
    std::vector<UINT> destinations, source_indices;
    UINT64 upload_bytes{};
    if (upload_needed)
    {
        for (UINT slice = 0; slice < package.description.array_size; ++slice)
            for (UINT mip = target; mip < package.description.mip_count; ++mip)
            {
                const UINT destination = slice * target_mips + mip - (batch->reserved ? 0u : target);
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT row_count{}; UINT64 row_size{}, bytes{};
                // Query each layout at zero, then place it at an aligned buffer
                // offset. pTotalBytes is a size, not a subsequent placement offset.
                device->GetCopyableFootprints(&description, destination, 1, 0,
                    &footprint, &row_count, &row_size, &bytes);
                constexpr auto alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
                if (!bytes || bytes == UINT64_MAX || upload_bytes > UINT64_MAX - (alignment - 1u))
                    return Failure("Selected texture upload placement size is invalid.");
                const auto base = (upload_bytes + alignment - 1u) & ~static_cast<UINT64>(alignment - 1u);
                if (base > UINT64_MAX - bytes || footprint.Offset > bytes)
                    return Failure("Selected texture upload placement overflow.");
                footprint.Offset += base;
                const auto end = base + bytes;
                const auto source_index = slice * package.description.mip_count + mip;
                const auto &source = package.subresources[source_index];
                if (!row_count || source.array_slice != slice || source.mip_level != mip ||
                    row_size != source.row_pitch || static_cast<UINT64>(row_count) * row_size != source.slice_pitch ||
                    source.bytes.size() != source.slice_pitch || row_size > footprint.Footprint.RowPitch ||
                    footprint.Offset > end || static_cast<UINT64>(row_count - 1u) * footprint.Footprint.RowPitch + row_size > end - footprint.Offset)
                    return Failure("Selected texture source/native upload footprint disagrees.");
                footprints.push_back(footprint); rows.push_back(row_count); row_bytes.push_back(row_size);
                destinations.push_back(destination); source_indices.push_back(source_index); upload_bytes = end;
            }
    }
    const auto upload_description = Buffer(upload_bytes);
    const auto upload_info = upload_needed ? device->GetResourceAllocationInfo(0, 1, &upload_description) : D3D12_RESOURCE_ALLOCATION_INFO{};
    if ((upload_needed && (!upload_info.SizeInBytes || upload_info.SizeInBytes == UINT64_MAX)) ||
        backing_bytes == UINT64_MAX || (upload_needed && backing_bytes > UINT64_MAX - upload_info.SizeInBytes))
        return Failure("Texture native backing/upload allocation size overflow.");
    const auto gpu_peak = backing_bytes + (upload_needed ? upload_info.SizeInBytes : 0u);
    ScopedGpuAdmission admission(residency, gpu_peak);
    if (gpu_peak && !admission) return Result::Success(); // Keep decoded request until shared budget admits its real peak.
    const auto rollback = [&] {
        for (auto &mapping : batch->maps) if (mapping.ticket) static_cast<void>(residency.ReleaseNow(mapping.ticket));
        for (auto ticket : {batch->replacement_ticket, batch->upload_ticket})
            if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
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
                backing.ticket = residency.Register(ResidencyCategory::Texture,
                    backing.memory.allocation->GetSize(), backing.mip >= texture.tail_mip);
                if (!backing.ticket) { rollback(); return Failure("Could not account streamed tile backing."); }
            }
        }
        else
        {
            if (auto result = Create(batch->replacement, description, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COMMON); !result) { rollback(); return result; }
            batch->resource = batch->replacement.resource;
            batch->replacement_ticket = residency.Register(ResidencyCategory::Texture,
                batch->replacement.allocation->GetSize(), false);
            if (!batch->replacement_ticket) { rollback(); return Failure("Could not account streamed replacement texture."); }
        }
        HRESULT hr{};
        if (upload_needed)
        {
            if (auto result = Create(batch->upload, upload_description, D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ); !result) { rollback(); return result; }
            batch->upload_ticket = residency.Register(ResidencyCategory::Staging, batch->upload.allocation->GetSize(), false);
            if (!batch->upload_ticket) { rollback(); return Failure("Could not account streamed texture upload."); }
            std::byte *mapped{}; D3D12_RANGE no_read{};
            hr = batch->upload.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
            if (FAILED(hr)) { rollback(); return NativeFailure("Map streamed texture upload", hr); }
            for (std::size_t index = 0; index < footprints.size(); ++index)
                for (UINT row = 0; row < rows[index]; ++row)
                    std::memcpy(mapped + footprints[index].Offset + static_cast<SIZE_T>(row) * footprints[index].Footprint.RowPitch,
                        package.subresources[source_indices[index]].bytes.data() + static_cast<SIZE_T>(row) * row_bytes[index],
                        static_cast<SIZE_T>(row_bytes[index]));
            batch->upload.resource->Unmap(0, nullptr);
        }
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&batch->allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
            batch->allocator.Get(), nullptr, IID_PPV_ARGS(&batch->list));
        if (FAILED(hr)) { rollback(); return NativeFailure("Create streamed texture copy list", hr); }
        if (upload_needed)
        {
            // The graph hands Copy batches textures in COMMON. Copy queues
            // cannot transition texture layouts; copies keep COMMON layout
            // (or use legacy implicit promotion/decay) through submission.
            for (std::size_t index = 0; index < footprints.size(); ++index)
            {
                D3D12_TEXTURE_COPY_LOCATION destination{}, source{};
                destination.pResource = batch->resource.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.SubresourceIndex = destinations[index]; source.pResource = batch->upload.resource.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = footprints[index];
                batch->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
        }
        hr = batch->list->Close();
        if (FAILED(hr)) { rollback(); return NativeFailure("Close streamed texture copy list", hr); }
        // Publication cannot allocate: reserve both vectors before queue work.
        texture.backing.reserve(texture.backing.size() + batch->maps.size());
        // Several textures may publish together. Every such retirement must
        // already have capacity before any GPU command can start.
        retired.reserve(retired.size() + textures.size());
        if (!streaming.BeginUpload(texture.request, texture.registration.generation))
        { rollback(); return Failure("Texture upload request transition was stale."); }
        texture.pending = std::move(batch);
        for (auto ticket : {texture.read_ticket, texture.decode_ticket})
            if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
        texture.read_ticket = {}; texture.decode_ticket = {}; std::vector<std::byte>{}.swap(texture.bytes);
        admission.Complete(); return Result::Success();
    }
    catch (...) { rollback(); throw; }
}
Result RendererTextureStreaming::Pump(std::uint64_t frame, const CompletionSet &completed,
    RendererAssetIo &io, StreamingManager &streaming, ResidencyManager &residency)
{
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
        if (!streaming.TryPublish(batch->request, texture.registration.generation, completed))
            return Failure("Completed selected texture upload could not publish.");
        ++texture.publications;
        if (batch->reserved)
        {
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
            texture.active_access = already_published ? Access::AllShaderRead : Access::Common;
        }
        else
        {
            if (texture.software.resource)
                impl_->retired.push_back({owner, std::move(texture.software), texture.software_ticket, texture.last_use});
            texture.software = std::move(batch->replacement); texture.software_ticket = batch->replacement_ticket;
            texture.first_mip = batch->target_mip;
            texture.active_access = Access::Common;
            texture.last_use = {};
        }
        texture.resident_mip = batch->target_mip; texture.publication = batch->completion;
        texture.publication_frame = frame;
        ++texture.revision;
        if (residency.ReleaseNow(batch->upload_ticket)) ++texture.reclaims;
        static_cast<void>(streaming.Retire(batch->request, batch->completion));
        texture.pending.reset(); texture.request = {};
    }
    // Apply quality before the budget planner. With no feedback/view demand the
    // real coarse tail remains resident, including on software adapters.
    std::uint64_t planned{};
    for (auto &texture : impl_->textures)
    {
        if (texture.registration.pinned)
        {
            if (planned > UINT64_MAX - texture.registration.fallback_allocation_bytes)
                return Failure("Pinned texture budget cost overflow.");
            planned += texture.registration.fallback_allocation_bytes; continue;
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
            texture.priority = 0;
            texture.desired_mip = (texture.reserved_published || texture.software.resource) && recent(texture.publication_frame)
                ? texture.resident_mip : texture.tail_mip;
        }
        texture.desired_mip = std::min(texture.tail_mip, std::max(texture.desired_mip, impl_->finest_mip));
        const auto cost = impl_->Cost(texture, texture.desired_mip);
        if (cost == UINT64_MAX || cost > UINT64_MAX - texture.registration.fallback_allocation_bytes ||
            planned > UINT64_MAX - cost - texture.registration.fallback_allocation_bytes)
            return Failure("Selected texture budget cost overflow.");
        planned += texture.registration.fallback_allocation_bytes + cost;
    }
    auto budget = impl_->budget;
    const auto accounting = residency.Accounting();
    const auto &observed = accounting.budget;
    bool global_pressure{};
    if (observed.valid && observed.local_budget)
    {
        const auto limit = observed.local_budget - observed.local_budget / 10u;
        const auto usage = observed.local_usage > UINT64_MAX - accounting.gpu_growth_since_snapshot
            ? UINT64_MAX : observed.local_usage + accounting.gpu_growth_since_snapshot;
        global_pressure = usage > limit;
        const auto physical = PhysicalBytes();
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
    while (planned > budget)
    {
        TextureOwner *victim{};
        for (auto &texture : impl_->textures)
            if (!texture.registration.pinned && texture.desired_mip < texture.tail_mip &&
                (!victim || texture.priority < victim->priority)) victim = &texture;
        if (!victim) return Failure("Texture budget cannot contain the actual pinned selected texture tails.");
        const auto old_cost = impl_->Cost(*victim, victim->desired_mip);
        ++victim->desired_mip; const auto new_cost = impl_->Cost(*victim, victim->desired_mip);
        if (old_cost < new_cost) return Failure("Native texture mip costs are not monotonic.");
        planned -= old_cost - new_cost;
    }
    for (std::size_t owner_index = 0; owner_index < impl_->textures.size(); ++owner_index)
    {
        auto &texture = impl_->textures[owner_index];
        const auto owner = TextureStreamId{static_cast<std::uint32_t>(owner_index + 1u)};
        if (texture.registration.pinned || texture.pending) continue;
        const bool fallback_target = texture.desired_mip == texture.registration.fallback_first_mip;
        const bool coarsening = texture.desired_mip > texture.resident_mip;
        if (!texture.reserved && texture.software.resource && coarsening &&
            (fallback_target || global_pressure))
        {
            if (texture.revision == UINT64_MAX) return Failure("Texture descriptor revision exhausted.");
            impl_->retired.reserve(impl_->retired.size() + 1u);
            auto retirement = texture.last_use; retirement.Merge(texture.publication);
            impl_->retired.push_back({owner, std::move(texture.software), texture.software_ticket, retirement});
            texture.software_ticket = {}; texture.last_use = {}; texture.publication = {};
            texture.first_mip = texture.resident_mip = texture.registration.fallback_first_mip;
            texture.active_access = texture.fallback_access; ++texture.revision;
            if (fallback_target)
            {
                if (texture.request) io.Cancel(texture.request, streaming);
                for (auto ticket : {texture.read_ticket, texture.decode_ticket})
                    if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
                texture.read_ticket = {}; texture.decode_ticket = {}; texture.request = {};
                std::vector<std::byte>{}.swap(texture.bytes);
                continue;
            }
        }
        const bool active = texture.reserved_published || texture.software.resource;
        if (!texture.request && active && texture.resident_mip == texture.desired_mip) continue;
        if (!texture.request && !active && !texture.reserved && texture.first_mip == texture.desired_mip) continue;
        if (!texture.request)
        {
            // IO reads the immutable complete package. The mip target is chosen
            // after that read, so settings/demand changes do not force rereads.
            const auto request = streaming.Request(texture.registration.logical_id,
                "texture-package", texture.registration.generation, texture.priority);
            if (!request) return Failure("Could not register selected texture mip demand.");
            texture.request = request;
            ++texture.requests;
        }
        const auto *page = streaming.Find(texture.request);
        if (!page) return Failure("Selected texture request ledger disappeared.");
        static_cast<void>(streaming.UpdatePriority(texture.request, texture.priority));
        if (page->state == StreamingPageState::Requested)
        {
            std::error_code error; const auto size = std::filesystem::file_size(texture.registration.path, error);
            if (error || !size || size > UINT64_MAX / 3u) return Failure("Selected texture file size is invalid.");
            if (!residency.CanStage(size * 3u)) continue;
            texture.read_ticket = residency.ReserveHost(size); texture.decode_ticket = residency.ReserveHost(size * 2u);
            if (!texture.read_ticket || !texture.decode_ticket)
            {
                for (auto ticket : {texture.read_ticket, texture.decode_ticket})
                    if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
                texture.read_ticket = {}; texture.decode_ticket = {};
                return Failure("Selected texture host reservation failed.");
            }
            bool submitted{};
            auto result = io.Enqueue({texture.request, texture.registration.generation, texture.registration.logical_id,
                texture.registration.path, size, texture.registration.generation_lease}, streaming, residency,
                texture.read_ticket, submitted);
            if (!submitted)
            {
                static_cast<void>(residency.ReleaseNow(texture.read_ticket));
                static_cast<void>(residency.ReleaseNow(texture.decode_ticket)); texture.decode_ticket = {};
            }
            texture.read_ticket = {};
            if (!result) return result;
            continue;
        }
        if (page->state == StreamingPageState::Reading)
        {
            AssetReadCompletion read;
            if (auto result = io.Poll(texture.request, streaming, residency, read); !result) return result;
            if (!read.ready) continue;
            if (read.cancelled) return Failure("Selected texture read was cancelled before publication.");
            texture.bytes = std::move(read.bytes); texture.read_ticket = read.storage;
            if (!streaming.CompleteRead(texture.request, texture.registration.generation))
                return Failure("Selected texture decode completion was stale.");
        }
        else if (page->state != StreamingPageState::Decoded) continue;
        if ((active && texture.resident_mip == texture.desired_mip) ||
            (!active && !texture.reserved && texture.first_mip == texture.desired_mip))
        {
            static_cast<void>(streaming.Cancel(texture.request));
            for (auto ticket : {texture.read_ticket, texture.decode_ticket})
                if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
            texture.read_ticket = {}; texture.decode_ticket = {}; texture.request = {};
            std::vector<std::byte>{}.swap(texture.bytes);
            continue;
        }
        if (auto result = impl_->Prepare(texture, texture.desired_mip, residency, streaming); !result) return result;
    }
    static_cast<void>(residency.Collect(completed)); static_cast<void>(streaming.Collect(completed));
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
        texture.revision, completion, texture.active_access, texture.reserved_published};
}
bool RendererTextureStreaming::HardwareBacked(TextureStreamId id) const noexcept
{
    return id && id.value <= impl_->textures.size() && !!impl_->textures[id.value - 1].reserved;
}
Result RendererTextureStreaming::AddPendingPasses(RenderGraphBuilder &graph, ExternalQueueBinding queue,
    std::uint32_t first_pass, std::vector<TextureStreamingGraphBinding> &bindings)
{
    if (!queue.native_queue_owner || queue.queue_type != QueueHint::Copy)
        return Failure("Texture mapping requires the graph's actual owned Copy queue.");
    bindings.clear(); bindings.reserve(impl_->textures.size());
    for (std::size_t index = 0; index < impl_->textures.size(); ++index)
    {
        auto &texture = impl_->textures[index]; const auto view = View({static_cast<std::uint32_t>(index + 1u)});
        const auto handle = graph.ImportTexture({view.resource, view.completion, QueueHint::Direct}, view.access,
            texture.registration.logical_id);
        auto version = graph.Version(handle); const auto batch = texture.pending;
        if (batch && !batch->submitted)
        {
            auto target = handle;
            if (batch->resource.Get() != view.resource)
                target = graph.ImportTexture({batch->resource.Get(), {}, QueueHint::Copy}, Access::Common,
                    "selected texture candidate");
            auto pass = graph.AddPass(first_pass + static_cast<std::uint32_t>(index), "Texture mip map/upload", QueueHint::Copy);
            const auto written = pass.Write(graph.Version(target), Access::CopyDest);
            const ExternalResourceAccess access{target, Access::Common, Access::Common};
            auto retained_queue = queue; retained_queue.resource_lease = batch;
            pass.SetExternalExecute(std::move(retained_queue), {&access, 1},
                [batch](RenderExternalPassContext &context, ExternalQueueCompletion &completion) {
                    completion.resource_lease = batch;
                    auto *native_queue = static_cast<ID3D12CommandQueue *>(context.NativeCommandQueue());
                    if (!native_queue || batch->started) return Failure("Texture copy/mapping batch submitted twice.");
                    batch->started = true;
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
                    ID3D12CommandList *lists[]{batch->list.Get()}; native_queue->ExecuteCommandLists(1, lists);
                    return Result::Success();
                });
            batch->scheduled = true;
            if (target.value == handle.value) version = written;
            else graph.SetFinalAccess(written, Access::Common);
        }
        graph.SetFinalAccess(version, Access::AllShaderRead);
        bindings.push_back({{static_cast<std::uint32_t>(index + 1u)}, version});
    }
    return Result::Success();
}
void RendererTextureStreaming::MarkSubmitted(const CompletionSet &completion, StreamingManager &streaming) noexcept
{
    for (auto &texture : impl_->textures)
    {
        const auto batch = texture.pending;
        if (!batch || !batch->scheduled || !batch->started || batch->submitted) continue;
        batch->completion = completion; batch->submitted = true;
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
}
bool RendererTextureStreaming::HasStartedWork() const noexcept
{
    return std::ranges::any_of(impl_->textures, [](const auto &texture) {
        return texture.pending && texture.pending->started;
    });
}
void RendererTextureStreaming::CancelReads(RendererAssetIo &io, StreamingManager &streaming) noexcept
{
    for (auto &texture : impl_->textures) if (texture.request && !texture.pending) io.Cancel(texture.request, streaming);
}
void RendererTextureStreaming::SwapState(RendererTextureStreaming &other) noexcept { impl_.swap(other.impl_); }
std::uint64_t RendererTextureStreaming::PhysicalBytes() const noexcept
{
    std::uint64_t bytes{};
    for (const auto &texture : impl_->textures)
    {
        bytes += texture.registration.fallback_allocation_bytes;
        if (texture.software.allocation) bytes += texture.software.allocation->GetSize();
        for (const auto &backing : texture.backing) bytes += backing.memory.allocation->GetSize();
        if (texture.pending)
        {
            if (texture.pending->replacement.allocation) bytes += texture.pending->replacement.allocation->GetSize();
            if (texture.pending->upload.allocation) bytes += texture.pending->upload.allocation->GetSize();
            for (const auto &backing : texture.pending->maps) bytes += backing.memory.allocation->GetSize();
        }
    }
    for (const auto &retired : impl_->retired) if (retired.texture.allocation) bytes += retired.texture.allocation->GetSize();
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
} // namespace hs
