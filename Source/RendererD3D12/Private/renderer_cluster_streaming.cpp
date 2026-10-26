#include "renderer_impl.hpp"
#include "renderer_cluster_runtime.hpp"

#include <hs/core/cooked_format.hpp>

#include <bit>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace hs
{
namespace
{
Result StreamFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}
std::uint64_t DecodedBytes(const render_data::GeometryData &data)
{
    return data.vertices.capacity() * sizeof(render_data::ExpandedVertex) +
        data.indices.capacity() * sizeof(std::uint32_t) +
        data.submeshes.capacity() * sizeof(render_data::GeometrySubmesh) +
        data.meshlets.capacity() * sizeof(render_data::GeometryMeshlet) +
        (data.meshlet_vertex_refs.capacity() + data.meshlet_triangles.capacity() +
            data.canonical_primitive_remap.capacity()) * sizeof(std::uint32_t);
}
}

Result D3D12Renderer::Impl::TryRelieveStreamingPressure(std::uint64_t bytes,
    std::uint32_t priority, bool need_cluster_slot, bool for_hlod_admission)
{
    return TryRelieveStreamingPressure(bytes, 0, priority, need_cluster_slot, for_hlod_admission);
}

Result D3D12Renderer::Impl::TryRelieveStreamingPressure(std::uint64_t local_bytes,
    std::uint64_t nonlocal_bytes, std::uint32_t priority, bool need_cluster_slot,
    bool for_hlod_admission)
{
    if (adapter_unified_memory)
    {
        local_bytes = nonlocal_bytes > UINT64_MAX - local_bytes ? UINT64_MAX : local_bytes + nonlocal_bytes;
        nonlocal_bytes = 0;
    }
    auto local_admitted = !local_bytes || residency.CanAdmit(local_bytes, 10, ResidencySegment::Local);
    auto nonlocal_admitted = !nonlocal_bytes || residency.CanAdmit(nonlocal_bytes, 10, ResidencySegment::NonLocal);
    if (!need_cluster_slot && local_admitted && nonlocal_admitted) return Result::Success();
    if (!local_admitted || !nonlocal_admitted)
    {
        const auto attempted = std::max(local_budget_observation.attempted_at,
            std::max(nonlocal_budget_observation.attempted_at, host_budget_observation.attempted_at));
        if (attempted == std::chrono::steady_clock::time_point{} ||
            std::chrono::steady_clock::now() - attempted >= std::chrono::milliseconds(250))
            if (auto refreshed = RefreshBudgetObservations(); !refreshed) return refreshed;
        local_admitted = !local_bytes || residency.CanAdmit(local_bytes, 10, ResidencySegment::Local);
        nonlocal_admitted = !nonlocal_bytes || residency.CanAdmit(nonlocal_bytes, 10, ResidencySegment::NonLocal);
        if (!need_cluster_slot && local_admitted && nonlocal_admitted) return Result::Success();
    }
    // Existing streamed allocations can free local native memory; an upload-only
    // shortfall cannot be repaired by retiring local resident pages.
    if (local_admitted && !nonlocal_admitted) return Result::Success();
    const auto completed = graph_queue_executor.Completed();
    std::uint32_t kind = UINT32_MAX, index{};
    std::tuple<std::uint32_t, std::uint64_t, std::uint32_t, std::uint32_t> best{};
    const auto consider = [&](std::uint32_t candidate_priority, std::uint64_t last_demand,
                              std::uint32_t candidate_kind, std::uint32_t candidate_index) {
        const auto key = std::tuple{candidate_priority, last_demand, candidate_kind, candidate_index};
        if (kind == UINT32_MAX || key < best) { best = key; kind = candidate_kind; index = candidate_index; }
    };
    if (cluster_runtime)
        for (auto &stream : cluster_runtime->streams)
        {
            if (stream.pinned || stream.hlod_admitted || stream.retiring || stream.upload_pending || stream.upload_submitted ||
                !stream.page || !cluster_runtime->cpu.pages[stream.logical_slot].resident ||
                (!for_hlod_admission && stream.last_demand_epoch >= cluster_runtime->epoch && stream.priority >= priority)) continue;
            consider(stream.priority, stream.last_demand_epoch, 0u, stream.logical_slot);
        }
    if (!need_cluster_slot)
    {
        for (std::uint32_t ordinal = 0; ordinal < environment_geometry_streams.size(); ++ordinal)
        {
            const auto &stream = environment_geometry_streams[ordinal];
            const auto &mesh = environment_meshes[stream.target_index];
            if (stream.retiring || stream.upload_pending || stream.upload_submitted || !mesh.storage.resource ||
                !stream.request || residency.IsPinned(mesh.residency) ||
                (stream.last_demand_frame >= frame_number && stream.priority >= priority)) continue;
            consider(stream.priority, stream.last_demand_frame, 1u, ordinal);
        }
        const auto &texture = environment_stream;
        if (texture.fine_resident && !texture.retiring && !texture.upload_pending && !texture.upload_submitted &&
            texture.candidate.resource && texture.request && !residency.IsPinned(texture.candidate_residency) &&
            (texture.last_demand_frame < frame_number || texture.priority < priority))
            consider(texture.priority, texture.last_demand_frame, 2u, 0u);
    }
    if (kind == UINT32_MAX) return Result::Success();
    bool destroyed{};
    if (kind == 0u)
    {
        auto &runtime = *cluster_runtime;
        auto &stream = runtime.streams[index];
        stream.retirement = stream.last_use;
        stream.retirement.Merge(stream.page->storage.graph_completion);
        if (!streaming.Retire(stream.request, stream.retirement) ||
            !residency.Retire(stream.page->residency, stream.retirement))
            return StreamFailure("Shared pressure could not retire a hierarchy page.");
        // Only future snapshots change. Mesh cuts retain a pinned ancestor;
        // optional scene HLOD retains original members. Old cuts keep buffers.
        runtime.cpu.pages[index].resident = 0u;
        stream.retiring = true;
        ++runtime.evictions;
        if (stream.retirement.IsSatisfiedBy(completed))
        {
            static_cast<void>(residency.ReleaseNow(stream.page->residency));
            stream.page.reset();
            runtime.physical_owner[stream.physical_slot] = UINT32_MAX;
            stream.physical_slot = UINT32_MAX;
            stream.request = {}; stream.retiring = false;
            stream.last_use = {}; stream.retirement = {};
            destroyed = true;
        }
    }
    else if (kind == 1u)
    {
        auto &stream = environment_geometry_streams[index];
        auto &mesh = environment_meshes[stream.target_index];
        const auto *fallback = FindGeometryPage(stream.fallback_index);
        if (!fallback || fallback->page_slot != stream.fallback_index ||
            stream.fallback_index == stream.target_index || !residency.IsPinned(fallback->residency) ||
            stream.generation == UINT32_MAX)
            return StreamFailure("Whole-LOD pressure requires an intact pinned noncyclic fallback.");
        stream.retirement = {};
        stream.retirement.Merge(mesh.storage.graph_completion);
        if (!streaming.Retire(stream.request, stream.retirement) || !residency.Retire(mesh.residency, stream.retirement))
            return StreamFailure("Shared pressure could not retire a whole-LOD page.");
        stream.candidate = std::move(static_cast<GeometryPageGpu &>(mesh));
        static_cast<GeometryPageGpu &>(mesh) = {};
        mesh.metadata = stream.metadata;
        mesh.page_slot = stream.target_index;
        mesh.fallback_page_slot = stream.fallback_index;
        mesh.generation = static_cast<std::uint32_t>(++stream.generation);
        stream.retiring = true;
        if (stream.retirement.IsSatisfiedBy(completed))
        {
            static_cast<void>(residency.ReleaseNow(stream.candidate.residency));
            stream.candidate.Reset(); stream.request = {}; stream.retiring = false;
            stream.retirement = {}; ++stream.reclaims;
            destroyed = true;
        }
    }
    else
    {
        auto &stream = environment_stream;
        if (!environment_textures[0].resource || !residency.IsPinned(stream.fallback_residency) ||
            stream.generation == UINT64_MAX)
            return StreamFailure("Texture pressure requires its retained pinned coarse allocation.");
        stream.retirement = stream.fine_use;
        stream.retirement.Merge(stream.candidate.graph_completion);
        if (!streaming.Retire(stream.request, stream.retirement) ||
            !residency.Retire(stream.candidate_residency, stream.retirement))
            return StreamFailure("Shared pressure could not retire the fine texture.");
        stream.fine_resident = false; stream.retiring = true; ++stream.generation;
        // Every pump runs after WaitForFrame. A later geometry/hierarchy pump
        // may evict after the texture pump, so retarget this waited slot now.
        const auto frame_slot = swap_chain->GetCurrentBackBufferIndex();
        const auto description = environment_textures[0].resource->GetDesc();
        WriteEnvironmentTextureDescriptor(frame_slot, 0, environment_textures[0].resource.Get(),
            description.Format, description.MipLevels, description.DepthOrArraySize);
        stream.frame_generation[frame_slot] = stream.generation;
        PublishEnvironmentMaterialTextures(frame_slot);
        if (stream.retirement.IsSatisfiedBy(completed))
        {
            static_cast<void>(residency.ReleaseNow(stream.candidate_residency));
            stream.candidate.Reset(); stream.candidate_residency = {}; stream.request = {};
            stream.retiring = false; stream.retirement = {}; stream.fine_use = {};
            ++stream.reclaims; destroyed = true;
        }
    }
    if (destroyed)
    {
        static_cast<void>(streaming.Collect(completed));
        return UpdateResidencyBudget();
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::DemandClusterPage(std::uint32_t logical_slot,
    std::uint32_t generation_tag, std::uint32_t priority)
{
    if (!cluster_runtime || logical_slot >= cluster_runtime->streams.size())
        return StreamFailure("Cluster demand has an invalid logical directory slot.");
    auto &runtime = *cluster_runtime;
    if (generation_tag != runtime.generation || runtime.content_generation != resource_generation_id)
        return StreamFailure("Cluster demand belongs to a stale selected Generation.");
    const auto error = std::bit_cast<float>(priority);
    if (!priority || !std::isfinite(error) || error <= 0.0f)
        return StreamFailure("Cluster demand priority must encode a finite positive projected error.");
    auto &stream = runtime.streams[logical_slot];
    if (stream.catalog == kClusterRuntimeCatalogs && !stream.hlod_admitted)
        return StreamFailure("Scene HLOD page demand bypassed complete group admission.");
    if (stream.last_demand_epoch != runtime.epoch) stream.priority = 0u;
    stream.priority = std::max(stream.priority, priority);
    stream.last_demand_epoch = runtime.epoch;
    if (stream.request && !stream.retiring) static_cast<void>(streaming.UpdatePriority(stream.request, stream.priority));
    if (stream.pinned || runtime.cpu.pages[logical_slot].resident || stream.retiring || stream.request)
        return Result::Success();
    stream.request = streaming.Request(stream.logical_id, stream.logical_id, runtime.content_generation, stream.priority);
    if (!stream.request) return StreamFailure("Could not register hierarchy page demand.");
    ++runtime.requests;
    return Result::Success();
}

Result D3D12Renderer::Impl::SubmitClusterPageUpload(std::uint32_t logical_slot,
    const CompletionSet &completion)
{
    if (!cluster_runtime || logical_slot >= cluster_runtime->streams.size())
        return StreamFailure("Cluster upload submission has an invalid page.");
    auto &runtime = *cluster_runtime;
    auto &stream = runtime.streams[logical_slot];
    if (!stream.upload_pending || stream.upload_submitted || !stream.page ||
        (!completion.direct && !completion.compute && !completion.copy) ||
        runtime.content_generation != resource_generation_id)
        return StreamFailure("Cluster upload submission has an invalid state/Generation.");
    if (!streaming.SubmitUpload(stream.request, runtime.content_generation, completion) ||
        !residency.Retire(stream.upload_residency, completion))
        return StreamFailure("Cluster upload completion/staging retirement was rejected.");
    stream.upload_completion = completion;
    stream.upload_pending = false;
    stream.upload_submitted = true;
    ++runtime.uploads;
    return Result::Success();
}

Result D3D12Renderer::Impl::SubmitClusterCutReadback(std::uint32_t frame_slot,
    std::uint32_t ordinal, const CompletionSet &completion)
{
    if (!cluster_runtime || frame_slot >= kClusterRuntimeFrames || ordinal >= kClusterRuntimeCatalogs ||
        (!completion.direct && !completion.compute && !completion.copy))
        return StreamFailure("Cluster demand/status readback submission has an invalid frame/completion.");
    auto &cut = cluster_runtime->frames[frame_slot].cuts[ordinal];
    if (!cut.readback.resource || cut.readback_pending || cut.generation != cluster_runtime->generation)
        return StreamFailure("Cluster demand/status readback is occupied or stale.");
    cut.readback_completion = completion;
    cut.readback_pending = true;
    return Result::Success();
}

Result D3D12Renderer::Impl::CaptureClusterFrame(std::uint32_t frame_slot)
{
    if (!cluster_runtime || frame_slot >= kClusterRuntimeFrames)
        return StreamFailure("Cluster residency capture has an invalid frame.");
    auto &runtime = *cluster_runtime;
    auto &frame = runtime.frames[frame_slot];
    if (!frame.completion.IsSatisfiedBy(graph_queue_executor.Completed()))
        return StreamFailure("Cluster residency snapshot is still in flight.");
    if (runtime.content_generation != resource_generation_id || runtime.epoch == UINT64_MAX)
        return StreamFailure("Cluster residency generation/epoch is invalid.");
    frame.epoch = ++runtime.epoch;
    frame.snapshot = runtime.cpu.pages;
    for (auto &page : frame.snapshot)
        if (page.resident && page.physical_page_slot < kGeometryPageCapacity &&
            previous_geometry_page_versions[page.physical_page_slot] == page.page_version)
            page.draw_flags |= kGeometryPreviousPage;
    void *mapped{};
    const D3D12_RANGE no_read{};
    const auto hr = frame.page_upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(hr)) return HResultFailure("Map cluster residency snapshot", hr);
    const auto bytes = frame.snapshot.size() * sizeof(GpuClusterPage);
    std::memcpy(mapped, frame.snapshot.data(), bytes);
    const D3D12_RANGE written{0, bytes};
    frame.page_upload.resource->Unmap(0, &written);
    frame.page_upload_pending = true;
    return Result::Success();
}

void D3D12Renderer::Impl::RecordClusterPageTableUpload(ID3D12GraphicsCommandList *list,
    std::uint32_t frame_slot) const
{
    const auto &frame = cluster_runtime->frames.at(frame_slot);
    list->CopyBufferRegion(frame.pages.resource.Get(), 0, frame.page_upload.resource.Get(), 0,
        frame.snapshot.size() * sizeof(GpuClusterPage));
}

void D3D12Renderer::Impl::MarkClusterFrameUse(std::uint32_t frame_slot,
    const CompletionSet &completion) noexcept
{
    if (!cluster_runtime || frame_slot >= kClusterRuntimeFrames) return;
    auto &runtime = *cluster_runtime;
    auto &frame = runtime.frames[frame_slot];
    frame.completion = completion;
    frame.page_upload_pending = false;
    for (auto &cut : frame.cuts) cut.completion = completion;
    // Conservatively retain every resident descriptor in this immutable frame
    // snapshot. No CPU visible list or GPU draw readback is required for safety.
    for (std::size_t logical = 0; logical < frame.snapshot.size(); ++logical)
    {
        const auto &snapshot = frame.snapshot[logical];
        auto &stream = runtime.streams[logical];
        if (snapshot.resident && stream.page && snapshot.generation == runtime.generation &&
            snapshot.physical_page_slot == stream.physical_slot && snapshot.page_version == stream.version)
            stream.last_use.Merge(completion);
    }
}

Result D3D12Renderer::Impl::PumpClusterStreaming(std::uint32_t frame_slot)
{
    if (!cluster_runtime || frame_slot >= kClusterRuntimeFrames)
        return StreamFailure("Cluster streaming pump has an invalid runtime/frame.");
    auto &runtime = *cluster_runtime;
    if (runtime.content_generation != resource_generation_id)
        return StreamFailure("Cluster runtime belongs to a stale selected Generation.");
    if (auto result = UpdateResidencyBudget(); !result) return result;
    const auto completed = graph_queue_executor.Completed();
    // Demand and capacity are the only GPU readbacks; draw/ref visibility stays
    // GPU owned. A pending readback retains its generation and native buffers.
    const auto consume = [&](ClusterViewRuntime &bank) -> Result {
        for (auto &frame : bank.frames)
            for (auto &cut : frame.cuts)
            {
                if (!cut.readback_pending || !cut.readback_completion.IsSatisfiedBy(completed)) continue;
                if (cut.generation != runtime.generation)
                    return StreamFailure("Completed cluster readback belongs to a stale generation tag.");
                const auto demand_bytes = runtime.cpu.pages.size() * sizeof(std::uint32_t);
                const auto total_bytes = demand_bytes + sizeof(GpuClusterCutStatus);
                void *mapped{};
                const D3D12_RANGE read{0, total_bytes};
                const auto hr = cut.readback.resource->Map(0, &read, &mapped);
                if (FAILED(hr)) return HResultFailure("Map cluster demand/status readback", hr);
                std::vector<std::uint32_t> demand(runtime.cpu.pages.size());
                std::memcpy(demand.data(), mapped, demand_bytes);
                std::memcpy(&cut.last_status, static_cast<const std::byte *>(mapped) + demand_bytes, sizeof(cut.last_status));
                const D3D12_RANGE no_write{};
                cut.readback.resource->Unmap(0, &no_write);
                cut.readback_pending = false;
                const auto &status = cut.last_status;
                if (status.reserved || status.candidate_count != cut.candidate_count ||
                    status.content_generation != runtime.generation || status.catalog_index >= kClusterRuntimeCatalogs ||
                    &cut != &frame.cuts[status.catalog_index] ||
                    (static_cast<std::uint64_t>(status.frame_id_high) << 32u | status.frame_id_low) != cut.actual_frame_id ||
                    (status.failure_flags & ~kClusterCutFailureCapacity))
                    return StreamFailure("GPU cluster cut reported a catalog/residency/error-domain invariant failure.");
                if (status.required_records_high || status.required_commands_high ||
                    status.required_records_low > UINT32_MAX / 96u)
                    return StreamFailure("GPU required cluster capacity exceeds the native command ABI.");
                cut.required_capacity = std::max(cut.required_capacity, status.required_records_low);
                if (status.failure_flags && status.committed_records)
                    return StreamFailure("GPU cluster cut reported a partial capacity transaction.");
                for (std::uint32_t logical = 0; logical < demand.size(); ++logical)
                    if (demand[logical])
                        if (auto result = DemandClusterPage(logical, runtime.generation, demand[logical]); !result) return result;
            }
        return Result::Success();
    };
    if (auto result = consume(runtime); !result) return result;
    for (auto &[id, view] : owned_views)
        if (id != active_view_id.value)
            if (auto result = consume(view->cluster_view); !result) return result;
    for (auto &stream : runtime.streams)
    {
        if (stream.retiring && stream.retirement.IsSatisfiedBy(completed))
        {
            const auto physical = stream.physical_slot;
            static_cast<void>(residency.ReleaseNow(stream.page->residency));
            stream.page.reset();
            runtime.physical_owner[physical] = UINT32_MAX;
            stream.physical_slot = UINT32_MAX;
            stream.retiring = false;
            stream.request = {};
            stream.last_use = {}; stream.retirement = {};
            // Demand received while retirement was in flight remains pending;
            // this starts a new request only after the old physical page ends.
            if (stream.last_demand_epoch == runtime.epoch && stream.priority)
                if (auto result = DemandClusterPage(stream.logical_slot, runtime.generation, stream.priority); !result) return result;
        }
        if (stream.upload_submitted && stream.upload_completion.IsSatisfiedBy(completed))
        {
            if (!streaming.TryPublish(stream.request, runtime.content_generation, completed))
                return StreamFailure("Completed hierarchy upload could not be published.");
            if (!stream.page || runtime.physical_owner[stream.physical_slot] != stream.logical_slot ||
                stream.version != runtime.physical_versions[stream.physical_slot])
                return StreamFailure("Hierarchy upload publication lost its physical page version.");
            runtime.cpu.pages[stream.logical_slot] = {runtime.generation, 1u, stream.physical_slot,
                stream.version, stream.page->triangle_offset, stream.page->remap_offset, 0u, 0u};
            stream.last_use.Merge(stream.upload_completion);
            stream.upload.Reset();
            static_cast<void>(residency.ReleaseNow(stream.upload_residency));
            stream.upload_residency = {};
            stream.upload_submitted = false;
            ++runtime.publications;
        }
    }
    static_cast<void>(residency.Collect(completed));
    static_cast<void>(streaming.Collect(completed));
    std::vector<std::size_t> order(runtime.streams.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        const auto &left = runtime.streams[a]; const auto &right = runtime.streams[b];
        if (left.hlod_admitted != right.hlod_admitted) return left.hlod_admitted;
        if (left.last_demand_epoch != right.last_demand_epoch) return left.last_demand_epoch > right.last_demand_epoch;
        return left.priority == right.priority ? a < b : left.priority > right.priority;
    });
    std::array<bool, kClusterRuntimeCatalogs + 1> reading{};
    for (const auto &stream : runtime.streams)
        if (const auto *request = streaming.Find(stream.request);
            request && request->state == StreamingPageState::Reading)
            reading[stream.catalog] = true;
    for (const auto logical : order)
    {
        auto &stream = runtime.streams[logical];
        if (!stream.request || stream.pinned || stream.retiring || stream.upload_pending || stream.upload_submitted) continue;
        const auto *request = streaming.Find(stream.request);
        if (!request) return StreamFailure("Hierarchy stream lost its request state.");
        if (request->state == StreamingPageState::Requested)
        {
            if (stream.last_demand_epoch < runtime.epoch) continue;
            if (reading[stream.catalog]) continue;
            std::uint64_t file_bytes{};
            if (auto sized = stream.source.ByteCount(file_bytes); !sized) return sized;
            if (!file_bytes || file_bytes > std::numeric_limits<std::size_t>::max() ||
                file_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                return StreamFailure("Hierarchy page read size is invalid.");
            const auto host_factor = stream.compiled ? 1u : 3u;
            if (file_bytes > UINT64_MAX / host_factor)
                return StreamFailure("Hierarchy read admission overflow.");
            // Development decoding reserves its vector growth before IO; compiled pages retain only authenticated bytes.
            if (!residency.CanStage(file_bytes * host_factor)) continue;
            const auto ticket = residency.ReserveHost(file_bytes);
            if (!stream.compiled) stream.decoded_residency = residency.ReserveHost(file_bytes * 2u);
            if (!ticket || (!stream.compiled && !stream.decoded_residency))
            {
                if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
                static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
                stream.decoded_residency = {};
                return StreamFailure("Could not begin/account hierarchy page read.");
            }
            stream.read_bytes = file_bytes;
            if (!stream.compiled) stream.decoded_result = std::make_shared<render_data::GeometryData>();
            const auto decoded_result = stream.decoded_result;
            const auto decode = stream.decode;
            const auto compiled = stream.compiled;
            bool submitted{};
            const auto queued = asset_io.Enqueue({stream.request, runtime.content_generation,
                    stream.logical_id, stream.source, file_bytes, resident_generation_lease,
                    [decoded_result, decode, compiled](std::span<const std::byte> bytes) {
                        if (compiled) (void)render_data::BindGeometryDescriptor(bytes, *compiled);
                        else if (auto result = decode(bytes, *decoded_result); !result)
                            throw std::runtime_error(std::string(result.Message()));
                    }},
                    streaming, residency, ticket, submitted);
            if (!submitted)
            {
                stream.decoded_result.reset();
                static_cast<void>(residency.ReleaseNow(ticket));
                static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
                stream.decoded_residency = {};
                if (!queued) return queued;
                continue;
            }
            if (!queued) return queued;
            reading[stream.catalog] = true;
            request = streaming.Find(stream.request);
        }
        if (request->state == StreamingPageState::Reading)
        {
            if (stream.read_storage.empty())
            {
                AssetReadCompletion read;
                if (auto result = asset_io.Poll(stream.request, streaming, residency, read); !result) return result;
                if (!read.ready || read.cancelled)
                {
                    if (read.ready) stream.decoded_result.reset();
                    continue;
                }
                stream.read_storage = std::move(read.bytes);
                stream.read_residency = read.storage;
                if (!stream.compiled)
                {
                    if (!stream.decoded_result) return StreamFailure("Hierarchy decode result owner is missing.");
                    if (stream.development_catalog)
                        if (const auto *descriptor = stream.development_catalog->FindGeometry(MakeAssetId(stream.logical_id)))
                        {
                            stream.compiled = *descriptor;
                            static_cast<void>(residency.ReleaseNow(stream.decoded_residency));
                            stream.decoded_residency = {};
                        }
                    if (!stream.compiled) stream.decoded = std::move(*stream.decoded_result);
                    stream.decoded_result.reset();
                }
            }
            const auto decoded_bytes = DecodedBytes(stream.decoded);
            if ((!stream.compiled && !residency.CommitHost(stream.decoded_residency, decoded_bytes)) ||
                !streaming.CompleteRead(stream.request, runtime.content_generation))
                return StreamFailure("Hierarchy decode accounting/Generation transition was rejected.");
            reading[stream.catalog] = false;
            request = streaming.Find(stream.request);
        }
        if (request->state != StreamingPageState::Decoded) continue;
        if (stream.last_demand_epoch < runtime.epoch) continue;
        std::uint32_t physical = UINT32_MAX;
        std::uint32_t free_slots{};
        for (std::uint32_t slot = runtime.first_physical_slot; slot < kGeometryPageCapacity; ++slot)
            if (runtime.physical_owner[slot] == UINT32_MAX)
            {
                ++free_slots;
                if (physical == UINT32_MAX) physical = slot;
            }
        // A resident, retiring or queued upload already owns its physical slot.
        // Reserve only the still-unassigned admitted pages, so none of those
        // states is counted a second time or consumes an extra pool slot.
        const auto reserved = static_cast<std::uint32_t>(std::count_if(runtime.streams.begin(), runtime.streams.end(),
            [](const auto &page) { return page.hlod_admitted && page.physical_slot == UINT32_MAX; }));
        if (!stream.hlod_admitted && free_slots <= reserved) continue;
        if (physical == UINT32_MAX)
        {
            if (auto result = TryRelieveStreamingPressure(0u, stream.priority, true, stream.hlod_admitted); !result) return result;
            continue;
        }
        const auto sizes = stream.UploadSizes();
        std::uint64_t upload_bytes{}; std::array<std::uint64_t, 4> offsets{};
        if (!GeometryPageLayout(sizes, offsets, upload_bytes)) return StreamFailure("Hierarchy page allocation estimate overflow.");
        const auto resident_description = BufferDescription(upload_bytes);
        const auto resident_bytes = device->GetResourceAllocationInfo(0, 1, &resident_description).SizeInBytes;
        const auto upload_description = BufferDescription(upload_bytes);
        const auto staging_bytes = device->GetResourceAllocationInfo(0, 1, &upload_description).SizeInBytes;
        if (staging_bytes == UINT64_MAX || resident_bytes == UINT64_MAX)
            return StreamFailure("Hierarchy upload admission overflow.");
        SampleAllocatorBudgetStats(false, false);
        ScopedGpuAdmission admission(residency, resident_bytes, staging_bytes);
        if (!admission)
        {
            if (auto result = TryRelieveStreamingPressure(resident_bytes, staging_bytes, stream.priority,
                    false, stream.hlod_admitted); !result) return result;
            continue; // Full decoded payload stays accounted until admission.
        }
        if (auto result = PrepareClusterPageUpload(stream.logical_slot, physical); !result) return result;
        admission.Complete();
        if (!streaming.BeginUpload(stream.request, runtime.content_generation))
            return StreamFailure("Hierarchy upload transition was rejected.");
        stream.read_bytes = 0;
        for (const auto ticket : {stream.read_residency, stream.decoded_residency})
            static_cast<void>(residency.ReleaseNow(ticket));
        stream.read_residency = {}; stream.decoded_residency = {};
        stream.decoded = {};
        std::vector<std::byte>{}.swap(stream.read_storage);
        stream.upload_pending = true;
    }
    if (hlod_runtime)
        hlod_runtime->admission_status[2] = static_cast<std::uint32_t>(std::count_if(runtime.streams.begin(), runtime.streams.end(),
            [](const auto &page) { return page.hlod_admitted && page.physical_slot == UINT32_MAX; }));
    return Result::Success();
}
} // namespace hs
