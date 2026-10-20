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
    std::array<std::uint64_t, kGeometryBuffersPerPage> &upload_offsets)
{
    if (page_slot >= kGeometryPageCapacity || page.vertices.resource || upload.resource)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry page upload has an invalid slot or occupied destination.");
    render_data::PackageView package;
    std::vector<std::byte> auxiliary;
    std::vector<render_data::GeometrySubmesh> submeshes;
    std::array<std::span<const std::byte>, kGeometryBuffersPerPage> bytes;
    try
    {
        // Decode validates canonical indices, meshlets, remap and metadata together.
        auto decoded = render_data::DecodeGeometryPage(package_bytes, page_asset, metadata);
        submeshes = std::move(decoded.submeshes);
        package = render_data::ReadPackage(package_bytes, page_asset);
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
    std::uint64_t upload_size{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        if (bytes[index].empty() || bytes[index].size() % 4 != 0 ||
            bytes[index].size() > std::numeric_limits<UINT>::max() ||
            upload_size > std::numeric_limits<std::uint64_t>::max() - bytes[index].size())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Geometry page GPU buffer size exceeds its ABI.");
        upload_offsets[index] = upload_size;
        upload_size += bytes[index].size();
    }
    if (upload_size > std::numeric_limits<std::size_t>::max())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry page upload size exceeds addressable memory.");
    GeometryPageGpu next;
    next.source_asset = page_asset;
    next.source_digest = render_data::Sha256(package_bytes);
    for (const auto &[id, path] : config.product_paths)
        if ((id.starts_with("render/geometry/") || id.starts_with("render/hlod/")) &&
            id.ends_with(".rpage") && MakeAssetId(id).value == page_asset.value)
        {
            if (!next.source_product.empty())
                return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Selected geometry AssetId is ambiguous.");
            next.source_product = id;
        }
    std::array<AllocationResource *, kGeometryBuffersPerPage> resources{
        &next.vertices, &next.indices, &next.meshlets, &next.auxiliary};
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    std::uint64_t resident_size{};
    for (std::size_t index = 0; index < resources.size(); ++index)
    {
        if (auto created = CreateAllocation(*resources[index], allocation,
                BufferDescription(bytes[index].size()), D3D12_RESOURCE_STATE_COPY_DEST); !created)
            return created;
        resources[index]->graph_access = Access::CopyDest;
        const auto size = resources[index]->allocation->GetSize();
        if (resident_size > std::numeric_limits<std::uint64_t>::max() - size)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Geometry residency size overflow.");
        resident_size += size;
    }
    AllocationResource next_upload;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(next_upload, allocation, BufferDescription(upload_size),
                                        D3D12_RESOURCE_STATE_GENERIC_READ); !created)
        return created;
    void *mapped{};
    D3D12_RANGE no_read{};
    const auto result = next_upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(result)) return HResultFailure("Map geometry page upload", result);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        std::memcpy(static_cast<std::byte *>(mapped) + upload_offsets[index],
                    bytes[index].data(), bytes[index].size());
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
    next.vertex_view = {next.vertices.resource->GetGPUVirtualAddress(),
        static_cast<UINT>(bytes[0].size()), sizeof(Vertex)};
    next.index_view = {next.indices.resource->GetGPUVirtualAddress(),
        static_cast<UINT>(bytes[1].size()), DXGI_FORMAT_R32_UINT};
    page = std::move(next);
    upload = std::move(next_upload);
    return Result::Success();
}

void D3D12Renderer::Impl::RecordGeometryPageUpload(
    ID3D12GraphicsCommandList *list, const GeometryPageGpu &page,
    const AllocationResource &upload,
    const std::array<std::uint64_t, kGeometryBuffersPerPage> &upload_offsets) const
{
    const std::array<ID3D12Resource *, kGeometryBuffersPerPage> resources{
        page.vertices.resource.Get(), page.indices.resource.Get(),
        page.meshlets.resource.Get(), page.auxiliary.resource.Get()};
    for (std::size_t index = 0; index < resources.size(); ++index)
        list->CopyBufferRegion(resources[index], 0, upload.resource.Get(),
                              upload_offsets[index], resources[index]->GetDesc().Width);
}

Result D3D12Renderer::Impl::CreateGeometryPage(
    GeometryPageGpu &page, const render_data::GeometryMetadata &metadata,
    std::span<const std::byte> package_bytes, AssetId page_asset,
    std::uint32_t page_slot, ID3D12GraphicsCommandList *list,
    std::vector<AllocationResource> &uploads)
{
    AllocationResource upload;
    std::array<std::uint64_t, kGeometryBuffersPerPage> offsets{};
    if (auto prepared = PrepareGeometryPageUpload(page, metadata, package_bytes, page_asset,
                                                  page_slot, upload, offsets); !prepared)
        return prepared;
    RecordGeometryPageUpload(list, page, upload, offsets);
    const std::array<ID3D12Resource *, kGeometryBuffersPerPage> resources{
        page.vertices.resource.Get(), page.indices.resource.Get(),
        page.meshlets.resource.Get(), page.auxiliary.resource.Get()};
    const std::array<D3D12_RESOURCE_STATES, kGeometryBuffersPerPage> states{
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
        D3D12_RESOURCE_STATE_INDEX_BUFFER,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    std::array<D3D12_RESOURCE_BARRIER, kGeometryBuffersPerPage> barriers{};
    for (std::size_t index = 0; index < resources.size(); ++index)
    {
        auto &barrier = barriers[index];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resources[index];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = states[index];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    page.vertices.graph_access = Access::VertexBuffer;
    page.indices.graph_access = Access::IndexBuffer;
    page.meshlets.graph_access = Access::ComputeRead;
    page.auxiliary.graph_access = Access::ComputeRead;
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
        if (page->vertices.resource && page->indices.resource &&
            page->meshlets.resource && page->auxiliary.resource) return page;
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
        const std::array<ID3D12Resource *, kGeometryBuffersPerPage> resources{
            page ? page->vertices.resource.Get() : nullptr,
            page ? page->indices.resource.Get() : nullptr,
            page ? page->meshlets.resource.Get() : nullptr,
            page ? page->auxiliary.resource.Get() : nullptr};
        for (auto *resource : resources)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.NumElements = resource ? static_cast<UINT>(resource->GetDesc().Width / 4) : 1;
            view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            device->CreateShaderResourceView(resource, &view, descriptor);
            descriptor.ptr += srv_stride;
        }
    }
    for (auto &stream : environment_geometry_streams)
        stream.frame_generation[frame_slot] = stream.generation;
}
}
