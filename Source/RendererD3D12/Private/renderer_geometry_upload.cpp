#include "renderer_impl.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace hs
{
namespace
{
std::span<const std::byte> GeometryChunk(const render_data::PackageView &package,
                                         render_data::ChunkType type)
{
    for (const auto &chunk : package.chunks)
        if (chunk.type == type) return chunk.bytes;
    throw std::runtime_error("geometry page omits a required GPU chunk");
}
}

Result D3D12Renderer::Impl::PrepareGeometryPageUpload(
    GeometryPageGpu &page, const render_data::GeometryMetadata &metadata,
    std::span<const std::byte> package_bytes, AssetId page_asset,
    std::uint32_t page_slot, AllocationResource &upload,
    PreparedEnvironmentGeometry *preparation)
{
    if (page_slot >= kGeometryPageCapacity || page.storage.resource || upload.resource)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry page upload has an invalid slot or occupied destination.");
    render_data::PackageView package;
    std::vector<std::byte> auxiliary;
    std::vector<render_data::GeometrySubmesh> submeshes;
    std::array<std::span<const std::byte>, kGeometryBuffersPerPage> bytes;
    std::array<std::byte, 32> source_digest{};
    try
    {
        // Streaming workers validate before upload; startup callers decode and validate here.
        if (preparation)
        {
            if (preparation->package.asset_id.value != page_asset.value)
                throw std::runtime_error("prepared geometry page identity changed before upload");
            submeshes = std::move(preparation->geometry.submeshes);
            package = std::move(preparation->package);
            source_digest = preparation->digest;
        }
        else
        {
            auto decoded = render_data::DecodeGeometryPage(package_bytes, page_asset, metadata);
            submeshes = std::move(decoded.submeshes);
            package = render_data::ReadPackage(package_bytes, page_asset);
            source_digest = render_data::Sha256(package_bytes);
        }
        bytes[0] = GeometryChunk(package, render_data::ChunkType::GeometryVertices);
        bytes[1] = GeometryChunk(package, render_data::ChunkType::GeometryIndices);
        bytes[2] = GeometryChunk(package, render_data::ChunkType::GeometryMeshlets);
        for (const auto type : {render_data::ChunkType::GeometryMeshletVertexRefs,
                               render_data::ChunkType::GeometryMeshletTriangles,
                               render_data::ChunkType::GeometryCanonicalPrimitiveRemap})
        {
            const auto chunk = GeometryChunk(package, type);
            auxiliary.insert(auxiliary.end(), chunk.begin(), chunk.end());
        }
        bytes[3] = auxiliary;
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::string("Invalid geometry page: ") + error.what());
    }
    std::array<std::uint64_t, 4> upload_offsets{};
    std::uint64_t upload_size{};
    const bool raster_suffix = page_slot >= kMonsterGeometryPage && page_slot < kProjectileGeometryPage &&
        (metadata.flags & render_data::kGeometryFlagSkinned) != 0;
    const std::array<std::uint64_t, 4> sizes{bytes[0].size(),
        std::uint64_t(bytes[1].size()) * (raster_suffix ? 2u : 1u),bytes[2].size(),bytes[3].size()};
    if (!GeometryPageLayout(sizes, upload_offsets, upload_size))
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Geometry page GPU buffer size exceeds its ABI.");
    GeometryPageGpu next;
    next.source_asset = page_asset;
    next.source_digest = source_digest;
    for (const auto &[id, path] : config.product_paths)
        if ((id.starts_with("render/geometry/") || id.starts_with("render/hlod/")) &&
            id.ends_with(".rpage") && MakeAssetId(id).value == page_asset.value)
        {
            if (!next.source_product.empty())
                return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Selected geometry AssetId is ambiguous.");
            next.source_product = id;
        }
    next.offsets = upload_offsets;
    next.sizes = sizes;
    next.raster_index_offset = raster_suffix ? static_cast<std::uint32_t>(bytes[1].size()) : 0;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto result = CreateAllocation(next.storage, allocation, BufferDescription(upload_size),
            D3D12_RESOURCE_STATE_COPY_DEST); !result) return result;
    next.storage.graph_access = Access::CopyDest;
    const auto resident_size = next.storage.allocation->GetSize();
    AllocationResource next_upload;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(next_upload, allocation, BufferDescription(upload_size),
                                        D3D12_RESOURCE_STATE_GENERIC_READ); !created)
        return created;
    void *mapped{};
    D3D12_RANGE no_read{};
    const auto result = next_upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(result)) return HResultFailure("Map geometry page upload", result);
    std::memset(mapped, 0, static_cast<std::size_t>(upload_size));
    for (std::size_t index = 0; index < bytes.size(); ++index)
        std::memcpy(static_cast<std::byte *>(mapped) + upload_offsets[index],
                    bytes[index].data(), bytes[index].size());
    if (raster_suffix && !WriteGeometryRasterIndexSuffix(
        {static_cast<std::byte *>(mapped) + upload_offsets[1] + next.raster_index_offset, bytes[1].size()},
        bytes[1], GeometryChunk(package, render_data::ChunkType::GeometryCanonicalPrimitiveRemap)))
    {
        next_upload.resource->Unmap(0, nullptr);
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid geometry raster index suffix.");
    }
    next_upload.resource->Unmap(0, nullptr);
    next.residency = residency.Register(ResidencyCategory::Geometry, resident_size,
        metadata.lod != 0 ||
            metadata.fallback_dependency_index == render_data::kNoGeometryDependency);
    if (!next.residency)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Could not account geometry page allocations.");
    next.metadata = metadata;
    next.submeshes = std::move(submeshes);
    next.page_slot = page_slot;
    next.fallback_page_slot = page.fallback_page_slot;
    next.generation = page.generation;
    next.vertex_count = metadata.vertex_count;
    next.index_count = metadata.index_count;
    next.meshlet_count = metadata.meshlet_count;
    next.triangle_offset = metadata.vertex_ref_count * sizeof(std::uint32_t);
    next.remap_offset = next.triangle_offset +
        static_cast<std::uint32_t>(GeometryChunk(package,
            render_data::ChunkType::GeometryMeshletTriangles).size());
    next.vertex_view = {next.storage.resource->GetGPUVirtualAddress() + next.offsets[0],
        static_cast<UINT>(bytes[0].size()), sizeof(Vertex)};
    next.index_view = {next.storage.resource->GetGPUVirtualAddress() + next.offsets[1] + next.raster_index_offset,
        static_cast<UINT>(bytes[1].size()), DXGI_FORMAT_R32_UINT};
    page = std::move(next);
    upload = std::move(next_upload);
    return Result::Success();
}

void D3D12Renderer::Impl::RecordGeometryPageUpload(
    ID3D12GraphicsCommandList *list, const GeometryPageGpu &page,
    const AllocationResource &upload) const
{
    list->CopyBufferRegion(page.storage.resource.Get(), 0, upload.resource.Get(), 0,
                          page.storage.resource->GetDesc().Width);
}

Result D3D12Renderer::Impl::CreateGeometryPage(
    GeometryPageGpu &page, const render_data::GeometryMetadata &metadata,
    std::span<const std::byte> package_bytes, AssetId page_asset,
    std::uint32_t page_slot, ID3D12GraphicsCommandList *list,
    std::vector<AllocationResource> &uploads)
{
    AllocationResource upload;
    if (auto prepared = PrepareGeometryPageUpload(page, metadata, package_bytes, page_asset,
                                                  page_slot, upload); !prepared)
        return prepared;
    RecordGeometryPageUpload(list, page, upload);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {page.storage.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
        D3D12_RESOURCE_STATE_INDEX_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &barrier);
    page.storage.graph_access = Access::VertexShaderRead;
    uploads.push_back(std::move(upload));
    return Result::Success();
}

const GeometryPageGpu *D3D12Renderer::Impl::FindGeometryPage(std::uint32_t page_slot) const noexcept
{
    if (page_slot > kProceduralGeometryPage) return FindClusterGeometryPage(page_slot);
    for (std::uint32_t attempt = 0; attempt < kGeometryPageCapacity; ++attempt)
    {
        const GeometryPageGpu *page{};
        if (page_slot < environment_meshes.size()) page = &environment_meshes[page_slot];
        else if (page_slot == kArcherGeometryPage) page = &archer_geometry;
        else if (page_slot >= kMonsterGeometryPage && page_slot < kProjectileGeometryPage)
            page = &monster_assets[page_slot - kMonsterGeometryPage];
        else if (page_slot == kProjectileGeometryPage) page = &gel_projectile_geometry;
        else if (page_slot == kProceduralGeometryPage) page = &procedural_geometry;
        if (!page) return nullptr;
        if (page->storage.resource) return page;
        if (page->fallback_page_slot >= kGeometryPageCapacity ||
            page->fallback_page_slot == page_slot) return nullptr;
        page_slot = page->fallback_page_slot;
    }
    return nullptr;
}

void D3D12Renderer::Impl::WriteGeometryPageDescriptors(std::uint32_t frame_slot)
{
    if (frame_slot >= kFrameCount) return;
    auto descriptor = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    descriptor.ptr += static_cast<SIZE_T>(
        frame_slot * kTextureDescriptorCount + kGeometryDescriptorOffset + 1) * srv_stride;
    for (std::uint32_t slot = 0; slot < kGeometryPageCapacity; ++slot)
    {
        const auto *page = FindGeometryPage(slot);
        auto *resource = page ? page->storage.resource.Get() : nullptr;
        for (std::size_t slice = 0; slice < kGeometryBuffersPerPage; ++slice)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.FirstElement = page ? page->offsets[slice] / 4 : 0;
            view.Buffer.NumElements = page ? static_cast<UINT>(page->sizes[slice] / 4) : 1;
            view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            device->CreateShaderResourceView(resource, &view, descriptor);
            descriptor.ptr += srv_stride;
        }
    }
    for (auto &stream : environment_geometry_streams)
        stream.frame_generation[frame_slot] = stream.generation;
}
}
