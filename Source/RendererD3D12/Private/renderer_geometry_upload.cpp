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

Result D3D12Renderer::Impl::PrepareReadGeometry(const RendererConfig &input, std::string_view id,
    std::span<const std::byte> bytes, const render_data::GeometryMetadata &metadata,
    PreparedGeometryUpload &output) const
{
    try
    {
        output.source_product = id;
        if (input.packed_content)
        {
            const auto handle = FindRendererGeometry(input, MakeAssetId(id));
            if (!handle) throw std::runtime_error("Missing compiled geometry descriptor: " + std::string(id));
            output.descriptor = &input.packed_geometry[handle.value];
            render_data::ValidateGeometryDescriptorMetadata(*output.descriptor, metadata);
            output.digest = input.packed_bulks[output.descriptor->bulk_index].range.expected_digest;
        }
        else if (input.development_catalog)
        {
            const auto asset=MakeAssetId(id);
            const auto &bulk=input.development_catalog->bulks[
                input.development_catalog->BulkIndex(asset)];
            if(bytes.size()!=bulk.range.byte_count || render_data::Sha256(bytes)!=bulk.range.expected_digest)
                throw std::runtime_error("Developer geometry differs from selected Generation");
            output.digest=bulk.range.expected_digest;
            output.descriptor=input.development_catalog->FindGeometry(asset);
            if(!output.descriptor || !output.descriptor->payload_ready)
            {
                output.page=render_data::PrepareGeometryPage(bytes,asset,metadata);
                output.descriptor=&input.development_catalog->AdmitGeometry(asset,bytes,output.page);
            }
            render_data::ValidateGeometryDescriptorMetadata(*output.descriptor,metadata);
        }
        else
        {
            output.page = render_data::PrepareGeometryPage(bytes, MakeAssetId(id), metadata);
            output.digest = render_data::Sha256(bytes);
        }
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what()); }
}

Result D3D12Renderer::Impl::PrepareGeometryPageUpload(
    GeometryPageGpu &page, const render_data::GeometryMetadata &metadata,
    std::span<const std::byte> package_bytes, AssetId page_asset,
    std::uint32_t page_slot, AllocationResource &upload,
    PreparedGeometryUpload *preparation)
{
    if (page_slot >= kGeometryPageCapacity || page.storage.resource || upload.resource)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry page upload has an invalid slot or occupied destination.");
    render_data::PackageView package;
    std::vector<render_data::GeometrySubmesh> submeshes;
    std::array<std::span<const std::byte>, 6> bytes;
    std::array<std::byte, 32> source_digest{};
    try
    {
        if (preparation && preparation->descriptor)
        {
            const auto &descriptor = *preparation->descriptor;
            if (descriptor.asset_id.value != page_asset.value)
                throw std::runtime_error("compiled geometry page identity changed before upload");
            const auto view = render_data::BindGeometryDescriptor(package_bytes, descriptor);
            bytes = {view.chunks[0], view.chunks[1], view.chunks[3], view.chunks[4], view.chunks[5], view.chunks[6]};
            submeshes.assign(view.submeshes.begin(), view.submeshes.end());
            source_digest = preparation->digest;
        }
        else
        {
            if (preparation)
            {
                if (preparation->page.package.asset_id.value != page_asset.value)
                    throw std::runtime_error("prepared geometry page identity changed before upload");
                submeshes = std::move(preparation->page.geometry.submeshes);
                package = std::move(preparation->page.package);
                source_digest = preparation->digest;
            }
            else
            {
                auto prepared = render_data::PrepareGeometryPage(package_bytes, page_asset, metadata);
                submeshes = std::move(prepared.geometry.submeshes);
                package = std::move(prepared.package);
                source_digest = render_data::Sha256(package_bytes);
            }
            bytes = {GeometryChunk(package, render_data::ChunkType::GeometryVertices),
                GeometryChunk(package, render_data::ChunkType::GeometryIndices),
                GeometryChunk(package, render_data::ChunkType::GeometryMeshlets),
                GeometryChunk(package, render_data::ChunkType::GeometryMeshletVertexRefs),
                GeometryChunk(package, render_data::ChunkType::GeometryMeshletTriangles),
                GeometryChunk(package, render_data::ChunkType::GeometryCanonicalPrimitiveRemap)};
        }
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::string("Invalid geometry page: ") + error.what());
    }
    std::array<std::uint64_t, 4> upload_offsets{};
    std::uint64_t upload_size{};
    const bool monster_slot = (page_slot >= kMonsterGeometryPage && page_slot < kProjectileGeometryPage) ||
        (page_slot >= kMonsterReducedGeometryPage && page_slot < kMonsterReducedGeometryPage + 12u);
    const bool raster_suffix = monster_slot &&
        (metadata.flags & render_data::kGeometryFlagSkinned) != 0;
    const std::array<std::uint64_t, 4> sizes{bytes[0].size(),
        std::uint64_t(bytes[1].size()) * (raster_suffix ? 2u : 1u),bytes[2].size(),bytes[3].size() + bytes[4].size() + bytes[5].size()};
    if (!GeometryPageLayout(sizes, upload_offsets, upload_size))
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Geometry page GPU buffer size exceeds its ABI.");
    GeometryPageGpu next;
    next.source_asset = page_asset;
    next.source_digest = source_digest;
    if (preparation) next.source_product = std::move(preparation->source_product);
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
    for (std::size_t index = 0; index < 3; ++index)
        std::memcpy(static_cast<std::byte *>(mapped) + upload_offsets[index],
                    bytes[index].data(), bytes[index].size());
    auto auxiliary_offset = upload_offsets[3];
    for (std::size_t index = 3; index < bytes.size(); ++index)
    {
        std::memcpy(static_cast<std::byte *>(mapped) + auxiliary_offset, bytes[index].data(), bytes[index].size());
        auxiliary_offset += bytes[index].size();
    }
    if (raster_suffix && !WriteGeometryRasterIndexSuffix(
        {static_cast<std::byte *>(mapped) + upload_offsets[1] + next.raster_index_offset, bytes[1].size()},
        bytes[1], bytes[5]))
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
        static_cast<std::uint32_t>(bytes[4].size());
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
    std::vector<AllocationResource> &uploads, PreparedGeometryUpload *preparation)
{
    AllocationResource upload;
    if (auto prepared = PrepareGeometryPageUpload(page, metadata, package_bytes, page_asset,
                                                  page_slot, upload, preparation); !prepared)
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
    if (page_slot >= kMonsterReducedGeometryPage + 12u) return FindClusterGeometryPage(page_slot);
    for (std::uint32_t attempt = 0; attempt < kGeometryPageCapacity; ++attempt)
    {
        const GeometryPageGpu *page{};
        if (page_slot < environment_meshes.size()) page = &environment_meshes[page_slot];
        else if (page_slot == kArcherGeometryPage) page = &archer_geometry;
        else if (page_slot >= kMonsterGeometryPage && page_slot < kProjectileGeometryPage)
            page = &monster_assets[page_slot - kMonsterGeometryPage];
        else if (page_slot == kProjectileGeometryPage) page = &gel_projectile_geometry;
        else if (page_slot == kProceduralGeometryPage) page = &procedural_geometry;
        else if (page_slot >= kMonsterReducedGeometryPage && page_slot < kMonsterReducedGeometryPage + 12u)
        {
            const auto reduced = page_slot - kMonsterReducedGeometryPage;
            page = &monster_reduced_geometry[reduced / 2u][reduced % 2u];
        }
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
