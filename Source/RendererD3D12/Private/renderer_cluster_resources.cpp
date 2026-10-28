#include "renderer_impl.hpp"
#include "renderer_cluster_runtime.hpp"

#include <hs/core/cooked_format.hpp>

#include <numeric>
#include <stdexcept>

namespace hs
{
ClusterPageStream::ClusterPageStream() = default;
ClusterPageStream::~ClusterPageStream() = default;
ClusterPageStream::ClusterPageStream(ClusterPageStream &&) noexcept = default;
ClusterPageStream &ClusterPageStream::operator=(ClusterPageStream &&) noexcept = default;

namespace
{
Result ClusterFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}
std::span<const std::byte> PageChunk(const render_data::PackageView &package,
                                  render_data::ChunkType type)
{
    for (const auto &chunk : package.chunks)
        if (chunk.type == type) return chunk.bytes;
    throw std::runtime_error("validated hierarchy page lost a required topology chunk");
}
std::uint32_t WorstCut(const ClusterGpuTables &tables, std::uint32_t group_base,
                       std::uint32_t child_base)
{
    std::vector<std::size_t> order(tables.groups.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return tables.groups[a].ancestor_steps > tables.groups[b].ancestor_steps;
    });
    std::vector<std::uint64_t> counts(tables.groups.size());
    std::uint64_t total{};
    // Rebased IDs can be arbitrary; depth order, rather than record order,
    // ensures every complete child cut is known before its parent.
    for (const auto i : order)
    {
        const auto &group = tables.groups[i];
        std::uint64_t children{};
        for (std::uint32_t c = 0; c < group.child_count; ++c)
            children += counts.at(tables.children.at(group.first_child - child_base + c) - group_base);
        counts[i] = std::max<std::uint64_t>(group.cluster_count, children);
        if (group.parent == UINT32_MAX) total += counts[i];
        if (counts[i] > UINT32_MAX || total > UINT32_MAX)
            throw std::runtime_error("hierarchy maximum complete cut exceeds32-bit GPU capacity");
    }
    if (!total) throw std::runtime_error("hierarchy maximum complete cut is empty");
    return static_cast<std::uint32_t>(total);
}
Result MapCopy(AllocationResource &upload, std::span<const std::byte> bytes)
{
    void *mapped{};
    const D3D12_RANGE no_read{};
    const auto hr = upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(hr)) return HResultFailure("Map cluster upload", hr);
    std::memcpy(mapped, bytes.data(), bytes.size());
    const D3D12_RANGE written{0, bytes.size()};
    upload.resource->Unmap(0, &written);
    return Result::Success();
}
}

Result D3D12Renderer::Impl::PrepareClusterPageUpload(std::uint32_t logical_slot,
                                                    std::uint32_t physical_slot)
{
    if (!cluster_runtime || logical_slot >= cluster_runtime->streams.size() ||
        physical_slot < cluster_runtime->first_physical_slot || physical_slot >= kGeometryPageCapacity)
        return ClusterFailure("Cluster page upload has an invalid logical/physical slot.");
    auto &runtime = *cluster_runtime;
    auto &stream = runtime.streams[logical_slot];
    if ((!stream.compiled && (stream.decoded.vertices.empty() || stream.decoded.indices.empty() || stream.decoded.meshlets.empty())) ||
        stream.read_storage.empty() || stream.page || stream.upload.resource || runtime.physical_owner[physical_slot] != UINT32_MAX ||
        runtime.physical_versions[physical_slot] == UINT32_MAX)
        return ClusterFailure("Cluster page upload destination is occupied or its version is exhausted.");
    std::array<std::span<const std::byte>, 6> chunks;
    try
    {
        if (stream.compiled)
        {
            const auto view = render_data::BindGeometryDescriptor(stream.read_storage, *stream.compiled);
            chunks = {view.chunks[0], view.chunks[1], view.chunks[3], view.chunks[4], view.chunks[5], view.chunks[6]};
        }
        else
        {
            const auto package = render_data::ReadPackage(stream.read_storage,
                MakeAssetId(stream.logical_id));
            chunks[0] = PageChunk(package, render_data::ChunkType::GeometryVertices);
            chunks[1] = PageChunk(package, render_data::ChunkType::GeometryIndices);
            chunks[2] = PageChunk(package, render_data::ChunkType::GeometryMeshlets);
            std::size_t auxiliary_index = 3;
            for (const auto type : {render_data::ChunkType::GeometryMeshletVertexRefs,
                render_data::ChunkType::GeometryMeshletTriangles,
                render_data::ChunkType::GeometryCanonicalPrimitiveRemap})
            {
                chunks[auxiliary_index++] = PageChunk(package, type);
            }
        }
    }
    catch (const std::exception &error) { return ClusterFailure(error.what()); }
    auto page = std::make_unique<GeometryPageGpu>();
    std::uint64_t upload_size{}, resident_size{};
    std::array<std::uint64_t, 4> offsets{};
    const std::array<std::uint64_t, 4> sizes{chunks[0].size(), chunks[1].size(), chunks[2].size(),
        chunks[3].size() + chunks[4].size() + chunks[5].size()};
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (!GeometryPageLayout(sizes, offsets, upload_size)) return ClusterFailure("Cluster page exceeds the raw-buffer ABI.");
    page->offsets = offsets;
    page->sizes = sizes;
    if (auto result = CreateAllocation(page->storage, allocation, BufferDescription(upload_size),
            D3D12_RESOURCE_STATE_COPY_DEST); !result) return result;
    page->storage.graph_access = Access::CopyDest;
    resident_size = page->storage.allocation->GetSize();
    AllocationResource upload;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto result = CreateAllocation(upload, allocation, BufferDescription(upload_size),
        D3D12_RESOURCE_STATE_GENERIC_READ); !result) return result;
    void *mapped{};
    const D3D12_RANGE no_read{};
    auto hr = upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(hr)) return HResultFailure("Map hierarchy page upload", hr);
    std::memset(mapped, 0, static_cast<std::size_t>(upload_size));
    for (std::size_t i = 0; i < 3; ++i)
        std::memcpy(static_cast<std::byte *>(mapped) + offsets[i], chunks[i].data(), chunks[i].size());
    auto copied = offsets[3];
    for (std::size_t i = 3; i < chunks.size(); ++i)
    {
        std::memcpy(static_cast<std::byte *>(mapped) + copied, chunks[i].data(), chunks[i].size());
        copied += chunks[i].size();
    }
    upload.resource->Unmap(0, nullptr);
    const auto page_ticket = RegisterNativeAllocation(ResidencyCategory::Geometry, resident_size, stream.pinned,
        ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
    const auto upload_ticket = RegisterNativeAllocation(ResidencyCategory::Staging,
        upload.allocation->GetSize(), false, ResidencySegmentForHeap(D3D12_HEAP_TYPE_UPLOAD));
    if (!page_ticket || !upload_ticket)
    {
        if (page_ticket) static_cast<void>(residency.ReleaseNow(page_ticket));
        if (upload_ticket) static_cast<void>(residency.ReleaseNow(upload_ticket));
        return ClusterFailure("Could not account hierarchy page allocations.");
    }
    const auto version = runtime.physical_versions[physical_slot] + 1u;
    page->residency = page_ticket;
    page->page_slot = physical_slot;
    page->generation = version;
    page->vertex_count = stream.compiled ? stream.compiled->vertex_count : static_cast<std::uint32_t>(stream.decoded.vertices.size());
    page->index_count = stream.compiled ? stream.compiled->index_count : static_cast<std::uint32_t>(stream.decoded.indices.size());
    page->meshlet_count = stream.compiled ? stream.compiled->meshlet_count : static_cast<std::uint32_t>(stream.decoded.meshlets.size());
    page->triangle_offset = static_cast<std::uint32_t>(chunks[3].size());
    page->remap_offset = page->triangle_offset + static_cast<std::uint32_t>(chunks[4].size());
    page->vertex_view = {page->storage.resource->GetGPUVirtualAddress() + offsets[0], static_cast<UINT>(sizes[0]), sizeof(Vertex)};
    page->index_view = {page->storage.resource->GetGPUVirtualAddress() + offsets[1], static_cast<UINT>(sizes[1]), DXGI_FORMAT_R32_UINT};
    // Hierarchy pages have no authored whole-LOD metadata. Their payload and
    // cluster intervals were validated against the actual hierarchy directory.
    stream.page = std::move(page);
    stream.upload = std::move(upload);
    stream.upload_residency = upload_ticket;
    stream.physical_slot = physical_slot;
    stream.version = version;
    runtime.physical_owner[physical_slot] = logical_slot;
    runtime.physical_versions[physical_slot] = version;
    return Result::Success();
}

void D3D12Renderer::Impl::RecordClusterPageUpload(ID3D12GraphicsCommandList *list,
                                                std::uint32_t logical_slot) const
{
    const auto &stream = cluster_runtime->streams.at(logical_slot);
    RecordGeometryPageUpload(list, *stream.page, stream.upload);
}

Result D3D12Renderer::Impl::CreateClusterResources(std::uint32_t generation_tag)
{
    if (cluster_runtime || !generation_tag || resource_generation_id.empty())
        return ClusterFailure("Cluster initialization requires a fresh nonzero generation tag.");
    auto next = std::make_unique<ClusterRuntimeState>();
    next->generation = generation_tag;
    next->content_generation = resource_generation_id;
    next->first_physical_slot = kMonsterReducedGeometryPage + 12u;
    next->physical_owner.fill(UINT32_MAX);
    next->physical_versions = owned_views.at(kMainRenderViewId.value)->previous_geometry_page_versions;
    try
    {
        for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
        {
            const auto &catalog = environment_cluster_catalogs[ordinal];
            if (catalog.Generation() != resource_generation_id)
                throw std::runtime_error("cluster catalog belongs to a different selected Generation");
            auto &range = next->catalogs[ordinal];
            range.group_base = static_cast<std::uint32_t>(next->cpu.groups.size());
            range.child_base = static_cast<std::uint32_t>(next->cpu.children.size());
            range.cluster_base = static_cast<std::uint32_t>(next->cpu.clusters.size());
            range.page_base = static_cast<std::uint32_t>(next->cpu.pages.size());
            ClusterGpuTables tables;
            if (auto result = catalog.PrepareGpu(ordinal, generation_tag, range.group_base,
                range.child_base, range.cluster_base, range.page_base, render_data::EnvironmentGeometryMaterialOrdinal(ordinal), tables); !result) return result;
            range.group_count = static_cast<std::uint32_t>(tables.groups.size());
            range.first_root = range.group_base + catalog.Hierarchy().root_groups.front();
            range.child_count = static_cast<std::uint32_t>(tables.children.size());
            range.cluster_count = static_cast<std::uint32_t>(tables.clusters.size());
            range.page_count = static_cast<std::uint32_t>(tables.pages.size());
            range.worst_cut_records = WorstCut(tables, range.group_base, range.child_base);
            range.static_error = catalog.Hierarchy().error_domain == render_data::GeometryHierarchyErrorDomain::Static;
            next->cpu.max_ancestor_steps = std::max(next->cpu.max_ancestor_steps, tables.max_ancestor_steps);
            next->cpu.groups.insert(next->cpu.groups.end(), tables.groups.begin(), tables.groups.end());
            next->cpu.children.insert(next->cpu.children.end(), tables.children.begin(), tables.children.end());
            next->cpu.clusters.insert(next->cpu.clusters.end(), tables.clusters.begin(), tables.clusters.end());
            next->cpu.pages.insert(next->cpu.pages.end(), tables.pages.begin(), tables.pages.end());
            for (std::uint32_t local = 0; local < range.page_count; ++local)
            {
                ClusterPageStream stream;
                stream.catalog = ordinal; stream.local_page = local;
                stream.logical_slot = range.page_base + local;
                stream.pinned = catalog.Pages()[local].pinned;
                stream.source_id = std::string(catalog.LogicalId());
                stream.logical_id = catalog.Pages()[local].logical_id;
                stream.source = catalog.Pages()[local].source;
                stream.development_catalog = resident_development_catalog;
                if (config.packed_content)
                {
                    const auto handle = FindRendererGeometry(config, MakeAssetId(stream.logical_id));
                    if (!handle) return ClusterFailure("Compiled hierarchy page descriptor is missing.");
                    stream.compiled = config.packed_geometry[handle.value];
                }
                stream.decode = [&catalog, local, development_catalog = stream.development_catalog,
                    asset = MakeAssetId(stream.logical_id)](std::span<const std::byte> bytes, render_data::GeometryData &data) {
                    return catalog.DecodePage(local, bytes, data,
                        [development_catalog, asset, bytes](const render_data::PackageView &package,
                            const render_data::GeometryData &geometry) {
                            if (development_catalog)
                                static_cast<void>(development_catalog->AdmitGeometry(asset, bytes, package, geometry));
                        });
                };
                next->streams.push_back(std::move(stream));
            }
        }
    }
    catch (const std::exception &error) { return ClusterFailure(error.what()); }
    if (auto result = AppendHlodPages(*next); !result) return result;
    const auto pinned = std::count_if(next->streams.begin(), next->streams.end(),
        [](const auto &stream) { return stream.pinned; });
    if (pinned > kGeometryPageCapacity - next->first_physical_slot)
        return ClusterFailure("Pinned hierarchy roots require " + std::to_string(pinned) +
            " pages after " + std::to_string(next->first_physical_slot) +
            " reserved slots; descriptor capacity is " + std::to_string(kGeometryPageCapacity) + ".");
    cluster_runtime = std::move(next);
    auto &runtime = *cluster_runtime;
    if (auto result = UpdateResidencyBudget(); !result) return result;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    auto hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) return HResultFailure("Create cluster initialization allocator", hr);
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    if (FAILED(hr)) return HResultFailure("Create cluster initialization list", hr);
    std::uint32_t physical = runtime.first_physical_slot;
    for (auto &stream : runtime.streams)
    {
        if (!stream.pinned) continue;
        std::uint64_t file_bytes{};
        if (auto sized = stream.source.ByteCount(file_bytes); !sized) return sized;
        const auto host_factor = stream.compiled ? 1u : 3u;
        if (!file_bytes || file_bytes > UINT64_MAX / host_factor || !residency.CanStage(file_bytes * host_factor))
            return ClusterFailure("Required pinned hierarchy root exceeds the host staging budget.");
        if (auto result = ReadBinary(stream.source, stream.read_storage); !result) return result;
        if (stream.read_storage.size() != file_bytes) return ClusterFailure("Pinned hierarchy root changed during read.");
        stream.read_bytes = stream.read_storage.size();
        stream.read_residency = residency.Register(ResidencyCategory::Staging, stream.read_bytes, false, ResidencyStorage::HostStaging);
        if (!stream.read_residency) return ClusterFailure("Could not account pinned root read storage.");
        if (!stream.compiled)
        {
            if (auto result = stream.decode(stream.read_storage, stream.decoded); !result) return result;
            const auto &data = stream.decoded;
            const auto decoded_bytes = data.vertices.capacity() * sizeof(render_data::ExpandedVertex) +
                data.indices.capacity() * 4u + data.submeshes.capacity() * sizeof(render_data::GeometrySubmesh) +
                data.meshlets.capacity() * sizeof(render_data::GeometryMeshlet) +
                (data.meshlet_vertex_refs.capacity() + data.meshlet_triangles.capacity() + data.canonical_primitive_remap.capacity()) * 4u;
            if (decoded_bytes > file_bytes * 2u) return ClusterFailure("Pinned root decode exceeds its host reservation.");
            stream.decoded_residency = residency.Register(ResidencyCategory::Staging, decoded_bytes, false, ResidencyStorage::HostStaging);
            if (!stream.decoded_residency) return ClusterFailure("Could not account pinned root decoded storage.");
        }
        const auto sizes = stream.UploadSizes();
        std::uint64_t upload_bytes{}; std::array<std::uint64_t, 4> offsets{};
        if (!GeometryPageLayout(sizes, offsets, upload_bytes)) return ClusterFailure("Pinned root allocation estimate overflow.");
        const auto resident_description = BufferDescription(upload_bytes);
        const auto resident_bytes = device->GetResourceAllocationInfo(0, 1, &resident_description).SizeInBytes;
        const auto upload_description = BufferDescription(upload_bytes);
        const auto staging_bytes = device->GetResourceAllocationInfo(0, 1, &upload_description).SizeInBytes;
        if (staging_bytes == UINT64_MAX || resident_bytes > UINT64_MAX - staging_bytes)
            return ClusterFailure("Pinned root staging estimate overflow.");
        SampleAllocatorBudgetStats(false, false);
        ScopedGpuAdmission admission(residency, resident_bytes, staging_bytes);
        if (!admission)
            return ClusterFailure("Required pinned hierarchy roots cannot fit the current residency budget.");
        if (auto result = PrepareClusterPageUpload(stream.logical_slot, physical++); !result) return result;
        admission.Complete();
        RecordClusterPageUpload(list.Get(), stream.logical_slot);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {stream.page->storage.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        stream.page->storage.graph_access = Access::ComputeRead;
        list->ResourceBarrier(1, &barrier);
        runtime.cpu.pages[stream.logical_slot] = {generation_tag, 1u, stream.physical_slot,
            stream.version, stream.page->triangle_offset, stream.page->remap_offset, 0u, 0u};
        stream.decoded = {};
        std::vector<std::byte>{}.swap(stream.read_storage);
        stream.read_bytes = 0;
        static_cast<void>(residency.ReleaseNow(stream.read_residency));
        static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
        stream.read_residency = {}; stream.decoded_residency = {};
    }
    D3D12MA::ALLOCATION_DESC allocation{};
    const auto buffer = [&](AllocationResource &target, ResidencyTicket &target_ticket,
                            std::span<const std::byte> data) -> Result {
        const auto size = std::max<std::size_t>(data.size(), 4);
        if (size > UINT32_MAX) return ClusterFailure("Cluster catalog table exceeds raw-buffer addressing.");
        const auto description = BufferDescription(size);
        const auto allocation_size = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
        if (allocation_size == UINT64_MAX || allocation_size > UINT64_MAX / 2u)
            return ClusterFailure("Cluster catalog table allocation estimate overflow.");
        SampleAllocatorBudgetStats(false, false);
        ScopedGpuAdmission admission(residency, allocation_size, allocation_size);
        if (!admission)
            return ClusterFailure("Required cluster catalog tables cannot fit the residency budget.");
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto result = CreateAllocation(target, allocation, BufferDescription(size),
            D3D12_RESOURCE_STATE_COPY_DEST); !result) return result;
        AllocationResource upload;
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto result = CreateAllocation(upload, allocation, BufferDescription(size),
            D3D12_RESOURCE_STATE_GENERIC_READ); !result) return result;
        if (data.empty())
        {
            const std::array<std::byte, 4> zero{};
            if (auto result = MapCopy(upload, zero); !result) return result;
        }
        else if (auto result = MapCopy(upload, data); !result) return result;
        list->CopyBufferRegion(target.resource.Get(), 0, upload.resource.Get(), 0, size);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {target.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        list->ResourceBarrier(1, &barrier);
        target.graph_access = Access::ComputeRead;
        target_ticket = RegisterNativeAllocation(ResidencyCategory::Geometry, target.allocation->GetSize(), true,
            ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
        const auto ticket = RegisterNativeAllocation(ResidencyCategory::Staging, upload.allocation->GetSize(), false,
            ResidencySegmentForHeap(D3D12_HEAP_TYPE_UPLOAD));
        if (!ticket || !target_ticket) return ClusterFailure("Could not account cluster table allocations.");
        admission.Complete();
        runtime.initial_upload_residency.push_back(ticket);
        runtime.initial_uploads.push_back(std::move(upload));
        return Result::Success();
    };
    if (auto result = buffer(runtime.groups, runtime.tables_residency[0], std::as_bytes(std::span(runtime.cpu.groups))); !result) return result;
    if (auto result = buffer(runtime.children, runtime.tables_residency[1], std::as_bytes(std::span(runtime.cpu.children))); !result) return result;
    if (auto result = buffer(runtime.clusters, runtime.tables_residency[2], std::as_bytes(std::span(runtime.cpu.clusters))); !result) return result;
    const std::array<std::byte, sizeof(GpuClusterCutStatus)> history_zero{};
    if (auto result = buffer(runtime.history_zero, runtime.tables_residency[3], history_zero); !result) return result;
    if (auto result = CreateClusterViewResources(runtime); !result) return result;
    hr = list->Close();
    if (FAILED(hr)) return HResultFailure("Close cluster initialization list", hr);
    std::vector<AllocationResource> uploads = std::move(runtime.initial_uploads);
    uploads.reserve(uploads.size() + static_cast<std::size_t>(pinned));
    for (auto &stream : runtime.streams)
        if (stream.pinned) uploads.push_back(std::move(stream.upload));
    if (auto result = SubmitResidentUtilityUpload(list.Get(), allocator.Get(), uploads); !result) return result;
    for (const auto ticket : runtime.initial_upload_residency) static_cast<void>(residency.ReleaseNow(ticket));
    runtime.initial_upload_residency.clear();
    for (auto &stream : runtime.streams)
        if (stream.pinned)
        {
            stream.upload.Reset();
            static_cast<void>(residency.ReleaseNow(stream.upload_residency));
            stream.upload_residency = {};
            ++runtime.publications;
        }
    for (auto &catalog : runtime.catalogs) catalog.roots_ready = true;
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateClusterViewResources(ClusterViewRuntime &target)
{
    if (!cluster_runtime || target.descriptors || target.clear_descriptors)
        return ClusterFailure("Cluster view initialization requires a fresh bank and shared catalog.");
    ClusterViewRuntime next;
    const auto prepare = [&]() -> Result {
        const auto &pages = cluster_runtime->cpu.pages;
        const auto bytes = static_cast<std::uint64_t>(pages.size()) * sizeof(GpuClusterPage);
        if (!bytes || bytes > UINT32_MAX)
            return ClusterFailure("Cluster view page snapshot exceeds raw-buffer addressing.");
        const auto description = BufferDescription(bytes);
        const auto size = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
        if (size == UINT64_MAX || size > UINT64_MAX / 2u)
            return ClusterFailure("Cluster view page snapshot allocation estimate overflow.");
        for (auto &frame : next.frames)
        {
            SampleAllocatorBudgetStats(false, false);
            ScopedGpuAdmission admission(residency, size, size);
            if (!admission) return ClusterFailure("Required cluster view snapshots cannot fit the residency budget.");
            D3D12MA::ALLOCATION_DESC allocation{};
            allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            if (auto result = CreateAllocation(frame.pages, allocation, description,
                    D3D12_RESOURCE_STATE_COPY_DEST); !result) return result;
            frame.pages.graph_access = Access::CopyDest;
            frame.pages_residency = RegisterNativeAllocation(ResidencyCategory::Geometry,
                frame.pages.allocation->GetSize(), true, ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
            if (!frame.pages_residency) return ClusterFailure("Could not account cluster view page snapshot.");
            allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            if (auto result = CreateAllocation(frame.page_upload, allocation, description,
                    D3D12_RESOURCE_STATE_GENERIC_READ); !result) return result;
            frame.page_upload_residency = RegisterNativeAllocation(ResidencyCategory::Staging,
                frame.page_upload.allocation->GetSize(), true, ResidencySegmentForHeap(D3D12_HEAP_TYPE_UPLOAD));
            if (!frame.page_upload_residency) return ClusterFailure("Could not account cluster view page upload.");
            admission.Complete();
            frame.snapshot = pages;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = kClusterRuntimeFrames * kClusterRuntimeCatalogs *
            kClusterOcclusionPhases * kClusterCutBankStride;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        auto hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next.descriptors));
        if (FAILED(hr)) return HResultFailure("Create cluster view cut descriptor heap", hr);
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next.clear_descriptors));
        if (FAILED(hr)) return HResultFailure("Create cluster view cut clear descriptor heap", hr);
        return Result::Success();
    };
    try
    {
        if (auto result = prepare(); !result)
        {
            ResetClusterViewResources(next);
            return result;
        }
    }
    catch (const std::exception &error)
    {
        ResetClusterViewResources(next);
        return ClusterFailure(error.what());
    }
    target = std::move(next);
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareClusterFrame(OwnedView &owner, std::uint32_t frame_slot,
    const std::array<ClusterCutInput, kClusterRuntimeCatalogs> &inputs)
{
    if (!cluster_runtime || frame_slot >= kClusterRuntimeFrames)
        return ClusterFailure("Cluster frame has an invalid runtime/frame slot.");
    auto &runtime = *cluster_runtime;
    auto &native = runtime.frames[frame_slot];
    auto &frame = frames[frame_slot];
    const auto completed = graph_queue_executor.Completed();
    if (!native.completion.IsSatisfiedBy(completed) || !frame.completion.IsSatisfiedBy(completed))
        return ClusterFailure("Cluster frame resources are still used by an earlier submission.");
    if (frame.geometry_draw_records_cpu.empty())
        return ClusterFailure("Authored geometry prefix must be prepared before cluster cuts.");
    if (!frame.mapped || !owner.targets.hzb_layout.count || owner.targets.hzb_layout.elements > UINT32_MAX)
        return ClusterFailure("Cluster occlusion requires prepared frame constants and HZB layout.");
    const auto &frame_constants = *reinterpret_cast<const FrameConstants *>(frame.mapped);
    const ClusterFrameRuntime *previous_frame = owner.last_presented_index < kClusterRuntimeFrames &&
        owner.last_presented_index != frame_slot ? &runtime.frames[owner.last_presented_index] : nullptr;
    const auto history_generation = owner.history_generation;
    const bool previous_history_valid = owner.hzb_history_valid && previous_frame &&
        previous_frame->history_generation == history_generation;
    const auto &hzb_extent = owner.targets.hzb_layout.levels[0];
    const bool previous_hzb_valid = previous_history_valid &&
        previous_frame->history_extent_depth[0] == static_cast<float>(hzb_extent.width) &&
        previous_frame->history_extent_depth[1] == static_cast<float>(hzb_extent.height) &&
        previous_frame->history_extent_depth[2] == frame_constants.temporal_options.z &&
        previous_frame->history_extent_depth[3] == frame_constants.temporal_options.w;
    native.history_generation = history_generation;
    native.history_extent_depth = {frame_constants.screen_size.x, frame_constants.screen_size.y,
        frame_constants.temporal_options.z, frame_constants.temporal_options.w};
    for (const auto &cut : native.cuts)
        if (cut.readback_pending)
            return ClusterFailure("Cluster demand/status must be consumed before rewriting a frame slot.");
    if (owner.frame_number == UINT64_MAX)
        return ClusterFailure("Cluster actual render frame identity overflow.");
    const auto actual_frame_id = owner.frame_number + 1u;
    std::array<std::uint32_t, kClusterRuntimeCatalogs> capacities{};
    std::array<std::uint32_t, kClusterRuntimeCatalogs> history_capacities{};
    std::uint64_t total{};
    for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
    {
        const auto &input = inputs[ordinal];
        const auto candidates = input.candidates ? std::span<const GpuClusterCandidate>(*input.candidates)
            : std::span<const GpuClusterCandidate>{};
        if (candidates.size() > input.max_instance_count)
            return ClusterFailure("Cluster candidate count exceeds its persistent scene slot capacity.");
        if (input.constants.active_view_mask & ~15u)
            return ClusterFailure("Cluster active view mask exceeds its four-view ABI.");
        if (!input.max_instance_count)
        {
            native.cuts[ordinal].candidate_count = 0;
            native.cuts[ordinal].actual_frame_id = 0;
            native.cuts[ordinal].previous_history_slot = UINT32_MAX;
            continue;
        }
        const auto product = static_cast<std::uint64_t>(input.max_instance_count) * runtime.catalogs[ordinal].worst_cut_records;
        const auto capacity = std::max(product, static_cast<std::uint64_t>(native.cuts[ordinal].required_capacity));
        if (!capacity || capacity > UINT32_MAX / 96u || total > UINT32_MAX / 64u - capacity)
            return ClusterFailure("Cluster complete-cut capacity exceeds32-bit raw-buffer addressing.");
        capacities[ordinal] = static_cast<std::uint32_t>(capacity);
        total += capacity;
        if (input.constants.raster_record_base != UINT32_MAX)
        {
            const auto first = input.constants.raster_record_base;
            if (!first || first > UINT32_MAX - capacity)
                return ClusterFailure("Cluster raster interval exceeds its32-bit address space.");
            for (std::uint32_t previous = 0; previous < ordinal; ++previous)
            {
                const auto other = inputs[previous].constants.raster_record_base;
                if (capacities[previous] && other != UINT32_MAX && first < other + capacities[previous] &&
                    other < first + capacity)
                    return ClusterFailure("Cluster catalog raster intervals overlap.");
            }
        }
        if (input.constants.material_word != ((material_parameter_bases[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)] << 1u) | 1u))
            return ClusterFailure("Cluster material word disagrees with its authored environment surface.");
        const auto &previous_upload = native.cuts[ordinal];
        const bool reused_candidates = input.candidates && previous_upload.candidate_owner == input.candidates &&
            previous_upload.candidate_generation == runtime.generation &&
            previous_upload.candidate_static_error == runtime.catalogs[ordinal].static_error;
        if (reused_candidates)
            history_capacities[ordinal] = previous_upload.history_instance_capacity;
        else for (const auto &candidate : candidates)
        {
            const auto &range = runtime.catalogs[ordinal];
            if (candidate.catalog_index != ordinal || candidate.first_group != range.group_base ||
                candidate.group_count != range.group_count || candidate.first_root != range.first_root || candidate.reserved ||
                candidate.instance_generation == 0 || candidate.view_mask > 15u ||
                (candidate.policy_flags & ~kClusterCandidateFineOnlyDeformationBound) ||
                !std::isfinite(candidate.deformation_radius) || candidate.deformation_radius < 0.0f ||
                (!range.static_error && !(candidate.policy_flags & kClusterCandidateFineOnlyDeformationBound)))
                return ClusterFailure("Cluster candidate does not match its catalog/error-domain contract.");
            if (candidate.instance_index == UINT32_MAX)
                return ClusterFailure("Cluster history persistent instance slot exceeds32-bit capacity.");
            history_capacities[ordinal] = std::max(history_capacities[ordinal], candidate.instance_index + 1u);
        }
        // Sparse persistent slot IDs determine membership size, independently
        // of the compact candidate count or the complete-cut output capacity.
        history_capacities[ordinal] = std::max(history_capacities[ordinal], 1u);
    }
    const auto record_base = frame.geometry_draw_records_cpu.size();
    if (record_base + total > UINT32_MAX / sizeof(GpuGeometryDrawRecord) || record_base + total > 0x40000000u)
        return ClusterFailure("Shared cluster geometry-record range exceeds its packed/raw ABI.");
    D3D12MA::ALLOCATION_DESC allocation{};
    const auto create = [&](AllocationResource &target, std::uint64_t bytes,
                            D3D12_HEAP_TYPE heap, bool uav) -> Result {
        if (!bytes || bytes > UINT32_MAX) return ClusterFailure("Cluster buffer byte range is invalid.");
        allocation.HeapType = heap;
        const auto state = heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
            : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
            : uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
        if (auto result = CreateAllocation(target, allocation,
            BufferDescription(bytes, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE), state); !result) return result;
        target.graph_access = heap == D3D12_HEAP_TYPE_UPLOAD ? Access::ComputeRead
            : uav ? Access::UnorderedWrite : Access::CopyDest;
        if (heap == D3D12_HEAP_TYPE_READBACK) target.graph_queue = QueueHint::Copy;
        return Result::Success();
    };
    // Only variable output storage exposes placement slack. Fixed ABI buffers,
    // constants, candidate/history inputs and readback keep their exact widths.
    constexpr auto output_bytes = [](std::uint64_t bytes) {
        constexpr auto maximum_aligned = UINT64(UINT32_MAX) & ~UINT64(65535u);
        return bytes <= maximum_aligned ? (bytes + 65535u) & ~UINT64(65535u) : bytes;
    };
    static_assert(output_bytes(UINT64(UINT32_MAX) + 1u) > UINT32_MAX);
    static_assert(output_bytes(65537u) == 131072u);
    if (total > native.ref_capacity)
    {
        AllocationResource refs;
        if (auto result = create(refs, output_bytes(total * 16u), D3D12_HEAP_TYPE_DEFAULT, true); !result) return result;
        const auto ticket = RegisterNativeAllocation(ResidencyCategory::Geometry,
            refs.allocation->GetSize(), false, ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
        if (!ticket) return ClusterFailure("Could not account shared cluster references.");
        if (native.refs_residency) static_cast<void>(residency.ReleaseNow(native.refs_residency));
        native.refs = std::move(refs); native.refs_residency = ticket;
        native.ref_capacity = static_cast<std::uint32_t>(native.refs.resource->GetDesc().Width / 16u);
    }
    const auto geometry_bytes = (record_base + total) * sizeof(GpuGeometryDrawRecord);
    if (geometry_bytes > frame.geometry_draw_records.resource->GetDesc().Width)
    {
        AllocationResource records;
        // Expose already-paid 64-KiB placement capacity instead of replacing a
        // multi-megabyte record buffer when the next frame adds one 64-byte record.
        const auto capacity = std::min<UINT64>((geometry_bytes + 65535u) & ~UINT64(65535u),
            (UINT64(UINT32_MAX) / sizeof(GpuGeometryDrawRecord)) * sizeof(GpuGeometryDrawRecord));
        if (auto result = create(records, capacity, D3D12_HEAP_TYPE_DEFAULT, true); !result) return result;
        const auto ticket = RegisterNativeAllocation(ResidencyCategory::Geometry,
            records.allocation->GetSize(), false, ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
        if (!ticket) return ClusterFailure("Could not account shared cluster geometry records.");
        if (runtime.geometry_records_residency[frame_slot])
            static_cast<void>(residency.ReleaseNow(runtime.geometry_records_residency[frame_slot]));
        runtime.geometry_records_residency[frame_slot] = ticket;
        records.graph_access = Access::UnorderedWrite;
        frame.geometry_draw_records = std::move(records);
    }
    std::uint32_t ref_base{};
    for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
    {
        const auto &input = inputs[ordinal];
        if (!capacities[ordinal]) continue;
        auto &cut = native.cuts[ordinal];
        if (cut.readback_pending)
            return ClusterFailure("Cluster demand/status must be consumed before rewriting a frame slot.");
        const auto candidate_capacity = input.max_instance_count;
        const auto output_capacity = capacities[ordinal];
        const auto history_capacity = history_capacities[ordinal];
        const auto cluster_count = runtime.catalogs[ordinal].cluster_count;
        const auto history_words = cluster_count / 32u + (cluster_count % 32u != 0u ? 1u : 0u);
        const auto history_bytes = static_cast<std::uint64_t>(std::max(cut.history_instance_capacity, history_capacity)) * history_words * 4u;
        if (!history_words || history_bytes > UINT32_MAX ||
            static_cast<std::uint64_t>(std::max(cut.history_instance_capacity, history_capacity)) * 4u > UINT32_MAX)
            return ClusterFailure("Cluster persistent membership exceeds32-bit buffer addressing.");
        if (cut.candidate_capacity < candidate_capacity || cut.allocated_output_capacity < output_capacity ||
            cut.history_instance_capacity < history_capacity)
        {
            ClusterCutBuffers next;
            next.candidate_capacity = std::max(cut.candidate_capacity, candidate_capacity);
            next.output_capacity = std::max(cut.allocated_output_capacity, output_capacity);
            // Both structured uint2 and raw-clear descriptors cover the entire allocation.
            // Check the larger raw R32 element count before allocating or casting NumElements.
            const std::uint64_t count_cache_elements = 2ull * next.candidate_capacity + next.output_capacity;
            const std::uint64_t count_cache_bytes = count_cache_elements * 8u;
            constexpr std::uint64_t maximum_buffer_elements = 1ull << D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
            if (count_cache_bytes > UINT32_MAX || count_cache_elements > UINT32_MAX ||
                count_cache_bytes / 4u > maximum_buffer_elements)
                return ClusterFailure("Cluster selected-cut cache exceeds structured/raw UAV addressing.");
            next.history_instance_capacity = std::max(cut.history_instance_capacity, history_capacity);
            next.history_words_per_instance = history_words;
            const auto blocks = (static_cast<std::uint64_t>(next.candidate_capacity) + 63u) / 64u;
            const auto make = [&](AllocationResource &buffer, std::uint64_t bytes,
                                   D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT) -> Result {
                return create(buffer, bytes, heap, heap == D3D12_HEAP_TYPE_DEFAULT);
            };
            if (auto result = make(next.candidates, static_cast<std::uint64_t>(next.candidate_capacity) * sizeof(GpuClusterCandidate), D3D12_HEAP_TYPE_UPLOAD); !result) return result;
            if (auto result = make(next.constants, 3u * kClusterConstantStride, D3D12_HEAP_TYPE_UPLOAD); !result) return result;
            if (auto result = make(next.occlusion_constants, kClusterOcclusionPhases * kClusterOcclusionConstantStride,
                D3D12_HEAP_TYPE_UPLOAD); !result) return result;
            // Counts, cut directories, then immutable selected (group, mask) entries.
            // One UAV preserves the existing Count/Prefix/Emit dependency chain.
            if (auto result = make(next.counts, output_bytes(count_cache_bytes)); !result) return result;
            if (auto result = make(next.offsets, static_cast<std::uint64_t>(next.candidate_capacity) * 8u); !result) return result;
            for (auto *buffer : {&next.block_sums, &next.block_offsets})
                if (auto result = make(*buffer, blocks * 8u); !result) return result;
            if (auto result = make(next.demand, runtime.cpu.pages.size() * 4u); !result) return result;
            if (auto result = make(next.status, sizeof(GpuClusterCutStatus)); !result) return result;
            if (auto result = make(next.draw_commands, output_bytes(static_cast<std::uint64_t>(next.output_capacity) * 96u)); !result) return result;
            if (auto result = make(next.mesh_commands, output_bytes(static_cast<std::uint64_t>(next.output_capacity) * 64u)); !result) return result;
            if (auto result = make(next.command_counts, 32u); !result) return result;
            if (auto result = make(next.selected_bounds, output_bytes(static_cast<std::uint64_t>(next.output_capacity) * 16u)); !result) return result;
            if (auto result = make(next.previous_rejected, output_bytes(static_cast<std::uint64_t>(next.output_capacity) * 4u)); !result) return result;
            if (auto result = make(next.retest_draw_commands, output_bytes(static_cast<std::uint64_t>(next.output_capacity) * 24u)); !result) return result;
            if (auto result = make(next.retest_mesh_commands, output_bytes(static_cast<std::uint64_t>(next.output_capacity) * 16u)); !result) return result;
            if (auto result = make(next.history_bits, history_bytes); !result) return result;
            if (auto result = make(next.history_generations, static_cast<std::uint64_t>(next.history_instance_capacity) * 4u); !result) return result;
            if (auto result = make(next.readback, runtime.cpu.pages.size() * 4u + sizeof(GpuClusterCutStatus), D3D12_HEAP_TYPE_READBACK); !result) return result;
            // Every output consumer must fit; never promote physical slack into
            // command counts, per-view offsets, or complete-cut logical limits.
            next.allocated_output_capacity = static_cast<std::uint32_t>(std::min({
                next.counts.resource->GetDesc().Width / 8u - 2ull * next.candidate_capacity,
                next.draw_commands.resource->GetDesc().Width / 96u,
                next.mesh_commands.resource->GetDesc().Width / 64u,
                next.selected_bounds.resource->GetDesc().Width / 16u,
                next.previous_rejected.resource->GetDesc().Width / 4u,
                next.retest_draw_commands.resource->GetDesc().Width / 24u,
                next.retest_mesh_commands.resource->GetDesc().Width / 16u}));
            std::uint64_t gpu_bytes{}, staging_bytes{};
            for (const auto *buffer : {&next.counts, &next.offsets, &next.block_sums, &next.block_offsets,
                &next.demand, &next.status, &next.draw_commands, &next.mesh_commands, &next.command_counts,
                &next.history_bits, &next.history_generations, &next.selected_bounds, &next.previous_rejected,
                &next.retest_draw_commands, &next.retest_mesh_commands}) gpu_bytes += buffer->allocation->GetSize();
            for (const auto *buffer : {&next.candidates, &next.constants, &next.occlusion_constants,
                &next.readback}) staging_bytes += buffer->allocation->GetSize();
            next.residency = RegisterNativeAllocation(ResidencyCategory::Geometry, gpu_bytes, false,
                ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
            next.staging_residency = RegisterNativeAllocation(ResidencyCategory::Staging, staging_bytes, false,
                ResidencySegmentForHeap(D3D12_HEAP_TYPE_READBACK));
            if (!next.residency || !next.staging_residency)
            {
                if (next.residency) static_cast<void>(residency.ReleaseNow(next.residency));
                if (next.staging_residency) static_cast<void>(residency.ReleaseNow(next.staging_residency));
                return ClusterFailure("Could not account cluster cut scratch.");
            }
            if (cut.residency) static_cast<void>(residency.ReleaseNow(cut.residency));
            if (cut.staging_residency) static_cast<void>(residency.ReleaseNow(cut.staging_residency));
            next.last_status = cut.last_status;
            next.required_capacity = cut.required_capacity;
            cut = std::move(next);
        }
        cut.output_capacity = output_capacity;
        cut.candidate_count = input.candidates ? static_cast<std::uint32_t>(input.candidates->size()) : 0u;
        cut.geometry_draw_base = static_cast<std::uint32_t>(record_base + ref_base);
        cut.generation = runtime.generation;
        cut.actual_frame_id = cut.candidate_count ? actual_frame_id : 0u;
        cut.previous_history_slot = UINT32_MAX;
        if (previous_history_valid && actual_frame_id > 1u)
        {
            const auto &previous = runtime.frames[owner.last_presented_index];
            const auto &previous_cut = previous.cuts[ordinal];
            if (previous.history_frame_id == actual_frame_id - 1u && previous_cut.actual_frame_id == actual_frame_id - 1u &&
                previous_cut.generation == runtime.generation && previous_cut.candidate_count &&
                previous_cut.history_bits.resource && previous_cut.history_generations.resource && previous_cut.status.resource &&
                previous_cut.history_words_per_instance == history_words)
                cut.previous_history_slot = owner.last_presented_index;
        }
        if (cut.candidate_owner != input.candidates || cut.candidate_generation != runtime.generation ||
            cut.candidate_static_error != runtime.catalogs[ordinal].static_error)
        {
            if (input.candidates && !input.candidates->empty())
                if (auto result = MapCopy(cut.candidates, std::as_bytes(std::span<const GpuClusterCandidate>(*input.candidates))); !result) return result;
            cut.candidate_owner = input.candidates;
            cut.candidate_generation = runtime.generation;
            cut.candidate_static_error = runtime.catalogs[ordinal].static_error;
        }
        auto constants = input.constants;
        constants.candidate_count = cut.candidate_count;
        constants.group_count = static_cast<std::uint32_t>(runtime.cpu.groups.size());
        constants.cluster_count = static_cast<std::uint32_t>(runtime.cpu.clusters.size());
        constants.child_count = static_cast<std::uint32_t>(runtime.cpu.children.size());
        constants.page_count = static_cast<std::uint32_t>(runtime.cpu.pages.size());
        constants.output_capacity = output_capacity;
        constants.geometry_draw_base = cut.geometry_draw_base;
        constants.generation = runtime.generation;
        constants.prefix_block_count = (cut.candidate_count + 63u) / 64u;
        constants.max_ancestor_steps = runtime.cpu.max_ancestor_steps;
        constants.cluster_ref_base = ref_base;
        constants.history_instance_capacity = cut.history_instance_capacity;
        constants.history_words_per_instance = history_words;
        constants.previous_history_valid = cut.previous_history_slot != UINT32_MAX ? 1u : 0u;
        constants.previous_history_instance_capacity = constants.previous_history_valid
            ? runtime.frames[cut.previous_history_slot].cuts[ordinal].history_instance_capacity : 0u;
        constants.frame_id_low = static_cast<std::uint32_t>(actual_frame_id);
        constants.frame_id_high = static_cast<std::uint32_t>(actual_frame_id >> 32u);
        constants.catalog_cluster_base = runtime.catalogs[ordinal].cluster_base;
        constants.catalog_cluster_count = cluster_count;
        // Caller provides each catalog's nonoverlapping raster base after its
        // authored ranges, and reserves that complete interval before binding.
        if (constants.raster_record_base != UINT32_MAX && (!constants.raster_record_base ||
            constants.raster_record_base > UINT32_MAX - output_capacity ||
            !visibility_frames[frame_slot].raster_records.resource ||
            (constants.raster_record_base + output_capacity) * 32ull > visibility_frames[frame_slot].raster_records.resource->GetDesc().Width))
            return ClusterFailure("Cluster raster-record interval was not reserved by visibility services.");
        if (constants.geometry_frontend_flags != 0u && constants.geometry_frontend_flags != kGeometryMeshEligible)
            return ClusterFailure("Cluster frontend must select pulling DRAW or Mesh.");
        cut.geometry_frontend_flags = constants.geometry_frontend_flags;
        void *mapped{};
        const D3D12_RANGE no_read{};
        auto hr = cut.constants.resource->Map(0, &no_read, &mapped);
        if (FAILED(hr)) return HResultFailure("Map cluster constants", hr);
        for (std::uint32_t stage = 0; stage < 3; ++stage)
        {
            constants.prefix_stage = stage;
            std::memcpy(static_cast<std::byte *>(mapped) + stage * kClusterConstantStride, &constants, sizeof(constants));
        }
        cut.constants.resource->Unmap(0, nullptr);
        hr = cut.occlusion_constants.resource->Map(0, &no_read, &mapped);
        if (FAILED(hr)) return HResultFailure("Map cluster occlusion constants", hr);
        for (std::uint32_t phase = 0; phase < kClusterOcclusionPhases; ++phase)
        {
            GpuClusterOcclusionConstants occlusion;
            // These are the actual jittered, origin-bearing raster matrices.
            // Previous HZB is invalidated on session/camera/extent changes by
            // the same frame history contract as authored geometry.
            std::memcpy(occlusion.current_view_projection.data(), &frame_constants.view_projection, 64u);
            std::memcpy(occlusion.previous_view_projection.data(), &frame_constants.previous_view_projection, 64u);
            const auto near_depth = phase == 0u && previous_hzb_valid ? previous_frame->history_extent_depth[2]
                : frame_constants.temporal_options.z;
            const auto far_depth = phase == 0u && previous_hzb_valid ? previous_frame->history_extent_depth[3]
                : frame_constants.temporal_options.w;
            occlusion.extent_near_far = {static_cast<float>(hzb_extent.width), static_cast<float>(hzb_extent.height),
                std::min(near_depth, far_depth), std::max(near_depth, far_depth)};
            occlusion.level_count = owner.targets.hzb_layout.count;
            occlusion.element_count = static_cast<std::uint32_t>(owner.targets.hzb_layout.elements);
            occlusion.history_valid = phase == 0u ? (previous_hzb_valid && constants.previous_history_valid ? 1u : 0u) : 1u;
            occlusion.phase = phase;
            std::memcpy(static_cast<std::byte *>(mapped) + phase * kClusterOcclusionConstantStride,
                &occlusion, sizeof(occlusion));
        }
        cut.occlusion_constants.resource->Unmap(0, nullptr);
        ref_base += output_capacity;
        WriteClusterCutDescriptors(owner, frame_slot, ordinal);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC records{};
    records.Format = DXGI_FORMAT_R32_TYPELESS;
    records.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    records.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    records.Buffer.NumElements = static_cast<UINT>(frame.geometry_draw_records.resource->GetDesc().Width / 4u);
    records.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(frame_slot * kTextureDescriptorCount + kGeometryDescriptorOffset) * srv_stride;
    device->CreateShaderResourceView(frame.geometry_draw_records.resource.Get(), &records, handle);
    return Result::Success();
}

void D3D12Renderer::Impl::WriteClusterCutDescriptors(OwnedView &owner, std::uint32_t frame_slot,
                                                  std::uint32_t ordinal)
{
    auto &runtime = *cluster_runtime;
    auto &native = runtime.frames.at(frame_slot);
    auto &cut = native.cuts.at(ordinal);
    const auto *previous = cut.previous_history_slot != UINT32_MAX ? &runtime.frames[cut.previous_history_slot] : nullptr;
    const auto *previous_cut = previous ? &previous->cuts[ordinal] : nullptr;
    for (std::uint32_t phase = 0; phase < kClusterOcclusionPhases; ++phase)
    {
        auto handle = runtime.descriptors->GetCPUDescriptorHandleForHeapStart();
        const auto bank_offset = static_cast<SIZE_T>(((frame_slot * kClusterRuntimeCatalogs + ordinal) *
            kClusterOcclusionPhases + phase) * kClusterCutBankStride) * srv_stride;
        handle.ptr += bank_offset;
        auto gpu_clear_handle = runtime.descriptors->GetCPUDescriptorHandleForHeapStart();
        gpu_clear_handle.ptr += bank_offset + static_cast<SIZE_T>(kClusterCutClearOffset) * srv_stride;
        auto clear_handle = runtime.clear_descriptors->GetCPUDescriptorHandleForHeapStart();
        clear_handle.ptr += bank_offset + static_cast<SIZE_T>(kClusterCutClearOffset) * srv_stride;
        const std::array<const AllocationResource *, kClusterCutSrvCount> srvs{&runtime.groups, &runtime.children,
            &runtime.clusters, &native.pages, &cut.candidates,
            previous_cut ? &previous_cut->history_bits : &runtime.history_zero,
            previous_cut ? &previous_cut->history_generations : &runtime.history_zero,
            previous ? &previous->pages : &native.pages,
            previous_cut ? &previous_cut->status : &runtime.history_zero,
            &owner.targets.hzb_history[owner.hzb_read_index ^ phase]};
        constexpr std::array<std::uint32_t, kClusterCutSrvCount> srv_strides{96, 4, 32, 32, 48, 4, 4, 32, 0, 4};
        for (std::size_t i = 0; i < srvs.size(); ++i)
        {
            // Tables/pages live with the view bank; candidates live with this cut.
            if (!cut.descriptors_initialized || i >= 5u)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = srv_strides[i] ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Buffer.NumElements = static_cast<UINT>(srvs[i]->resource->GetDesc().Width / (srv_strides[i] ? srv_strides[i] : 4u));
                view.Buffer.StructureByteStride = srv_strides[i];
                if (!srv_strides[i]) view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                device->CreateShaderResourceView(srvs[i]->resource.Get(), &view, handle);
            }
            handle.ptr += srv_stride;
        }
        const std::array<const AllocationResource *, kClusterCutUavCount> uavs{&cut.counts, &cut.offsets, &cut.block_sums,
            &cut.block_offsets, &cut.demand, &cut.status, &frames[frame_slot].geometry_draw_records,
            &native.refs, &cut.draw_commands, &cut.mesh_commands, &cut.command_counts,
            visibility_frames[frame_slot].raster_records.resource ? &visibility_frames[frame_slot].raster_records : &cut.status,
            &cut.history_bits, &cut.history_generations, &cut.selected_bounds, &cut.previous_rejected,
            &cut.retest_draw_commands, &cut.retest_mesh_commands};
        constexpr std::array<std::uint32_t, kClusterCutUavCount> strides{8, 8, 8, 8, 4, 0, 0, 16, 0, 0, 0, 0, 4, 4, 16, 4, 0, 0};
        for (std::size_t i = 0; i < uavs.size(); ++i)
        {
            // Shared geometry/refs/raster scratch may grow without replacing this cut.
            if (!cut.descriptors_initialized || i == 6u || i == 7u || i == 11u)
            {
                const auto bytes = uavs[i]->resource->GetDesc().Width;
                D3D12_UNORDERED_ACCESS_VIEW_DESC clear_view{};
                clear_view.Format = DXGI_FORMAT_R32_TYPELESS;
                clear_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                clear_view.Buffer.NumElements = static_cast<UINT>(bytes / 4u);
                clear_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                device->CreateUnorderedAccessView(uavs[i]->resource.Get(), nullptr, &clear_view, clear_handle);
                device->CopyDescriptorsSimple(1, gpu_clear_handle, clear_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                if (strides[i])
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
                    view.Format = DXGI_FORMAT_UNKNOWN;
                    view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    view.Buffer.NumElements = static_cast<UINT>(bytes / strides[i]);
                    view.Buffer.StructureByteStride = strides[i];
                    device->CreateUnorderedAccessView(uavs[i]->resource.Get(), nullptr, &view, handle);
                }
                else device->CopyDescriptorsSimple(1, handle, clear_handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            handle.ptr += srv_stride;
            gpu_clear_handle.ptr += srv_stride;
            clear_handle.ptr += srv_stride;
        }
    }
    cut.descriptors_initialized = true;
}

const GeometryPageGpu *D3D12Renderer::Impl::FindClusterGeometryPage(std::uint32_t physical_slot) const noexcept
{
    if (!cluster_runtime || physical_slot >= kGeometryPageCapacity) return nullptr;
    const auto logical = cluster_runtime->physical_owner[physical_slot];
    if (logical >= cluster_runtime->streams.size()) return nullptr;
    return cluster_runtime->streams[logical].page.get();
}

bool D3D12Renderer::Impl::ClusterHierarchyReady(std::uint32_t ordinal) const noexcept
{
    return cluster_runtime && ordinal < kClusterRuntimeCatalogs &&
        cluster_runtime->catalogs[ordinal].static_error && cluster_runtime->catalogs[ordinal].roots_ready;
}

void D3D12Renderer::Impl::ResetClusterViewResources(ClusterViewRuntime &bank) noexcept
{
    // Caller has completed every submission that used this view bank.
    for (auto &frame : bank.frames)
    {
        for (const auto ticket : {frame.pages_residency, frame.page_upload_residency, frame.refs_residency})
            if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
        for (auto &cut : frame.cuts)
            for (const auto ticket : {cut.residency, cut.staging_residency})
                if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
    }
    bank = {};
}

void D3D12Renderer::Impl::ResetInactiveClusterViewResources() noexcept
{
    // The active OwnedView is an empty exchange placeholder; its native bank
    // resides in cluster_runtime. All other bank leases live with their view.
    for (auto &[id, view] : owned_views) ResetClusterViewResources(view->cluster_view);
    for (auto &candidate : unresolved_view_candidates) ResetClusterViewResources(candidate->cluster_view);
}

void D3D12Renderer::Impl::ResetClusterResources() noexcept
{
    // Caller drains all GPU queues first. Async readers own only immutable paths
    // and byte vectors; join them before releasing the selected Generation.
    ResetInactiveClusterViewResources();
    if (!cluster_runtime) return;
    auto &runtime = *cluster_runtime;
    for (auto &stream : runtime.streams)
    {
        if (stream.read_future.valid()) stream.read_future.wait();
        if (stream.request) static_cast<void>(streaming.Cancel(stream.request));
        for (const auto ticket : {stream.read_residency, stream.decoded_residency, stream.upload_residency,
            stream.page ? stream.page->residency : ResidencyTicket{}})
            if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
    }
    for (const auto ticket : runtime.tables_residency) static_cast<void>(residency.ReleaseNow(ticket));
    for (const auto ticket : runtime.initial_upload_residency) static_cast<void>(residency.ReleaseNow(ticket));
    for (const auto ticket : runtime.geometry_records_residency)
        if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
    ResetClusterViewResources(runtime);
    cluster_runtime.reset();
}
} // namespace hs
