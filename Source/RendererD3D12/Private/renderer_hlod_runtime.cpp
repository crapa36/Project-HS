#include "renderer_impl.hpp"
#include "renderer_hlod_runtime.hpp"
#include <hs/render_data/render_asset_format.hpp>

#include <bit>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hs
{
namespace
{
Result HlodFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}
std::string HlodDigest(std::span<const std::byte> bytes)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "sha256:";
    for (const auto value : render_data::Sha256(bytes))
    {
        const auto byte = std::to_integer<unsigned>(value);
        result += hex[byte >> 4]; result += hex[byte & 15];
    }
    return result;
}
std::string ProductDigest(const RendererConfig &config, const std::string &id)
{
    const auto digest = RendererProductDigest(config, id);
    if (digest.size() != 71 || !digest.starts_with("sha256:"))
        throw std::runtime_error("selected Generation omits HLOD product digest: " + id);
    return digest;
}
std::uint32_t MaterialOrdinal(const render_data::SceneHlod &scene, const render_data::SceneHlodProxy &proxy)
{
    const auto &id = scene.dependencies.at(proxy.material_dependency);
    if (id == "render/material/ground_detail.rpkg")
    {
        if (proxy.mesh_semantic != static_cast<std::uint32_t>(RenderMesh::GroundDetail) ||
            proxy.variant >= kGroundDetailVariantCount)
            throw std::runtime_error("HLOD ground-detail material differs from its mesh/variant binding");
        return render_data::kGroundDetailMaterialOrdinal;
    }
    for (std::uint32_t i = 0; i < 10; ++i)
        if (id == "render/" + std::string(kEnvironmentMaterialNames[i]) + ".rpkg")
        {
            const auto semantic = static_cast<std::uint32_t>(i < 3 ? RenderMesh::TreeTrunk :
                i < 6 ? RenderMesh::TreeCanopy : RenderMesh::Rock);
            const auto variant = i < 3 ? i : i < 6 ? i - 3 : i - 6;
            if (proxy.mesh_semantic != semantic || proxy.variant != variant)
                throw std::runtime_error("HLOD material product differs from its mesh/variant binding");
            return i;
        }
    throw std::runtime_error("HLOD proxy material has no selected native environment pipeline: " + id);
}
void NullHlodExports(ID3D12Device *device, ID3D12DescriptorHeap *heap, std::uint32_t stride,
                     std::uint32_t frame_slot)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = 1; view.Buffer.StructureByteStride = 16;
    auto descriptor = heap->GetCPUDescriptorHandleForHeapStart();
    descriptor.ptr += static_cast<SIZE_T>(frame_slot * kTextureDescriptorCount + kHlodDescriptorOffset) * stride;
    device->CreateShaderResourceView(nullptr, &view, descriptor);
    descriptor.ptr += stride;
    device->CreateShaderResourceView(nullptr, &view, descriptor);
}
Result WithdrawHlodPage(ClusterPageStream &stream, RendererAssetIo &io,
                        StreamingManager &streaming, ResidencyManager &residency)
{
    stream.hlod_admitted = false;
    stream.priority = 0u; stream.last_demand_epoch = 0u;
    // Submitted/pending uploads and resident/retiring pages retain their
    // existing GPU leases. They become normal shared-pressure victims later.
    if (!stream.request || stream.page || stream.upload_pending || stream.upload_submitted || stream.retiring)
        return Result::Success();
    const auto *request = streaming.Find(stream.request);
    if (!request || (request->state != StreamingPageState::Requested &&
        request->state != StreamingPageState::Reading && request->state != StreamingPageState::Decoded))
        return HlodFailure("Withdrawn HLOD page has an invalid pre-upload request state.");
    // RendererAssetIo retains submitted reads until terminal completion. Only
    // completed read storage and decoder staging belong to this stream here.
    io.Cancel(stream.request, streaming);
    for (const auto ticket : {stream.read_residency, stream.decoded_residency})
        static_cast<void>(residency.ReleaseNow(ticket));
    stream.read_residency = {}; stream.decoded_residency = {};
    stream.decoded = {}; std::vector<std::byte>{}.swap(stream.read_storage);
    stream.request = {}; stream.read_bytes = 0u;
    return Result::Success();
}
void InvalidateHlodQualification(HlodViewRuntime &view)
{
    ++view.qualification_epoch;
    std::fill(view.group_priorities.begin(), view.group_priorities.end(), 0u);
}
Result ResetHlodView(HlodViewRuntime &view, ResidencyManager &residency, const CompletionSet &completed)
{
    for (const auto &frame : view.frames)
        if (!frame.completion.IsSatisfiedBy(completed) || !frame.readback_completion.IsSatisfiedBy(completed))
            return HlodFailure("HLOD view reset requires completed frame/demand leases.");
    if (auto result = view.system.ResetAfterCompletion(completed); !result) return result;
    for (const auto &frame : view.frames)
        static_cast<void>(residency.ReleaseNow(frame.readback_residency));
    static_cast<void>(residency.ReleaseNow(view.residency));
    return Result::Success();
}
}

Result D3D12Renderer::Impl::AppendHlodPages(ClusterRuntimeState &runtime)
{
    if (hlod_runtime || config.scene_hlod_product_id.empty()) return Result::Success();
    try
    {
        auto next = std::make_unique<HlodRuntime>();
        next->logical_id = config.scene_hlod_product_id;
        next->content_generation = resource_generation_id;
        next->generation = runtime.generation;
        RendererAssetSource path;
        if (auto result = ResolveRendererAsset(config, next->logical_id, path); !result) return result;
        std::vector<std::byte> bytes;
        if (auto result = ReadBinary(path, bytes); !result) return result;
        if (!path.pack && HlodDigest(bytes) != ProductDigest(config, next->logical_id))
            return HlodFailure("SHL1 bytes differ from selected Generation digest.");
        next->scene = render_data::DecodeSceneHlod(bytes);
        if (next->scene.scene_asset_id.value != MakeAssetId(next->logical_id).value)
            return HlodFailure("SHL1 scene asset identity differs from selected product.");
        for (const auto &dependency : next->scene.dependencies)
        {
            RendererAssetSource resolved;
            if (auto result = ResolveRendererAsset(config, dependency, resolved); !result) return result;
            (void)ProductDigest(config, dependency);
        }
        next->page_base = static_cast<std::uint32_t>(runtime.cpu.pages.size());
        if (next->scene.pages.size() > UINT32_MAX - next->page_base)
            return HlodFailure("Shared HLOD page directory exceeds32-bit addressing.");
        for (const auto &page : next->scene.pages)
        {
            RendererAssetSource resolved;
            if (auto result = ResolveRendererAsset(config, page.logical_id, resolved); !result) return result;
            next->page_sources.push_back(std::move(resolved));
            next->page_digests.push_back(ProductDigest(config, page.logical_id));
        }
        for (const auto &proxy : next->scene.proxies)
            next->proxy_material_ordinals.push_back(MaterialOrdinal(next->scene, proxy));
        next->group_pages.resize(next->scene.groups.size());
        next->group_priorities.resize(next->scene.groups.size());
        next->admitted_groups.resize(next->scene.groups.size());
        std::vector<std::uint32_t> page_groups(next->scene.pages.size(), render_data::kNoSceneHlodGroup);
        for (std::uint32_t group = 0; group < next->scene.groups.size(); ++group)
        {
            const auto &entry = next->scene.groups[group];
            auto &pages = next->group_pages[group];
            for (std::uint32_t proxy = entry.first_proxy; proxy < entry.first_proxy + entry.proxy_count; ++proxy)
            {
                const auto page = next->scene.proxies[proxy].page_index;
                if (page_groups[page] != render_data::kNoSceneHlodGroup && page_groups[page] != group)
                    return HlodFailure("A scene HLOD page is shared by distinct group transactions.");
                page_groups[page] = group;
                pages.push_back(page);
            }
            std::ranges::sort(pages);
            pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        }
        hlod_runtime = std::move(next);
        auto &hlod = *hlod_runtime;
        for (std::uint32_t local = 0; local < hlod.scene.pages.size(); ++local)
        {
            const auto &page = hlod.scene.pages[local];
            ClusterPageStream stream;
            stream.catalog = kClusterRuntimeCatalogs; stream.local_page = local;
            stream.logical_slot = hlod.page_base + local;
            // Scene proxies are always optional. Original-member mesh
            // hierarchy roots provide the mandatory complete fallback.
            stream.pinned = false;
            stream.source_id = hlod.logical_id; stream.logical_id = page.logical_id;
            stream.source = hlod.page_sources[local];
            stream.development_catalog = resident_development_catalog;
            if (config.packed_content)
            {
                const auto handle = FindRendererGeometry(config, MakeAssetId(stream.logical_id));
                if (!handle) return HlodFailure("Compiled HLOD page descriptor is missing.");
                stream.compiled = config.packed_geometry[handle.value];
            }
            stream.decode = [owner = hlod_runtime.get(), local,
                development_catalog = stream.development_catalog](std::span<const std::byte> payload, render_data::GeometryData &output) {
                try
                {
                    const auto &value = *owner;
                    const auto &entry = value.scene.pages.at(local);
                    if (HlodDigest(payload) != value.page_digests.at(local))
                        return HlodFailure("HLOD page differs from selected Generation digest.");
                    const auto package = render_data::ReadPackage(payload, MakeAssetId(entry.logical_id),
                        render_data::PackageReadMode::GeometryDecoderValidatesVertices);
                    if (package.dependencies != value.scene.dependencies)
                        return HlodFailure("HLOD page dependencies differ from its scene directory.");
                    auto geometry = render_data::DecodeGeometryPagePayload(package, entry.geometry_flags);
                    render_data::ValidateSceneHlodPage(value.scene, local, geometry);
                    if (development_catalog)
                        static_cast<void>(development_catalog->AdmitGeometry(
                            MakeAssetId(entry.logical_id), payload, package, geometry));
                    output = std::move(geometry);
                    return Result::Success();
                }
                catch (const std::exception &error) { return HlodFailure(error.what()); }
            };
            runtime.cpu.pages.push_back({runtime.generation, 0u, UINT32_MAX, 0u, 0u, 0u, 0u, 0u});
            runtime.streams.push_back(std::move(stream));
        }
        return Result::Success();
    }
    catch (const std::exception &error) { return HlodFailure(error.what()); }
}

HlodViewRuntime *D3D12Renderer::Impl::ActiveHlodView() noexcept
{
    return const_cast<HlodViewRuntime *>(std::as_const(*this).ActiveHlodView());
}

const HlodViewRuntime *D3D12Renderer::Impl::ActiveHlodView() const noexcept
{
    if (!hlod_runtime) return nullptr;
    const auto session = explicit_frame
        ? (explicit_frame->snapshots.has_current ? explicit_frame->snapshots.current.header.session_id : 0u)
        : view_session_id;
    const auto found = hlod_runtime->views.find({active_view_id.value, session});
    return found == hlod_runtime->views.end() ? nullptr : found->second.get();
}

Result D3D12Renderer::Impl::RetireHlodView(ViewId id)
{
    if (!hlod_runtime) return Result::Success();
    const auto &views = hlod_runtime->views;
    CompletionSet leases;
    for (const auto &[key, view] : views)
        if (key.first == id.value)
            for (std::uint32_t slot = 0; slot < view->frames.size(); ++slot)
            {
                leases.Merge(view->frames[slot].completion);
                leases.Merge(view->frames[slot].readback_completion);
                leases.Merge(view->system.FrameCompletion(slot));
            }
    if (!leases.IsSatisfiedBy(graph_queue_executor.Completed()))
        if (auto result = WaitForGpu(); !result) return result;
    const auto completed = graph_queue_executor.Completed();
    for (auto found = hlod_runtime->views.begin(); found != hlod_runtime->views.end();)
    {
        if (found->first.first != id.value) { ++found; continue; }
        if (auto result = ResetHlodView(*found->second, residency, completed); !result) return result;
        found = hlod_runtime->views.erase(found);
    }
    return PumpHlodDemand();
}

Result D3D12Renderer::Impl::BeginHlodViewPackage(const RenderFrameInput &input)
{
    if (!hlod_runtime) return Result::Success();
    auto &runtime = *hlod_runtime;
    const auto session = input.snapshots.has_current ? input.snapshots.current.header.session_id : 0u;
    const auto scene = input.scene ? input.scene->lease
        : input.snapshots.has_current ? input.snapshots.current.static_scene : nullptr;
    // A valid frame package owns participation. Dormant histories remain leased
    // and reusable, while omitted views immediately withdraw IO qualification.
    for (auto &[key, view] : runtime.views)
    {
        const bool participating = key.second == session && std::ranges::any_of(input.views,
            [&](const auto &input_view) { return input_view.id.value == key.first; });
        if (view->participating && !participating) InvalidateHlodQualification(*view);
        view->participating = participating;
    }
    for (const auto &input_view : input.views)
    {
        if (std::ranges::any_of(runtime.views, [&](const auto &entry) {
                return entry.first.first == input_view.id.value && entry.first.second != session; }))
            if (auto result = RetireHlodView(input_view.id); !result) return result;
        const auto key = std::pair{input_view.id.value, session};
        auto &owner = runtime.views[key];
        if (!owner)
        {
            owner = std::make_unique<HlodViewRuntime>();
            owner->view_id = input_view.id; owner->session_id = session;
            owner->group_priorities.resize(runtime.scene.groups.size());
        }
        owner->participating = true;
        const auto owned = owned_views.find(input_view.id.value);
        const bool prior_origins = input_view.id == active_view_id ? previous_origins_valid
            : owned != owned_views.end() && owned->second->previous_origins_valid;
        const auto prior_origin = input_view.id == active_view_id ? previous_scene_origin
            : owned != owned_views.end() ? owned->second->previous_scene_origin : RenderWorldPosition{};
        if (input_view.camera_cut || (prior_origins && input_view.scene_origin != prior_origin))
            InvalidateHlodQualification(*owner);
        if (!scene || scene->content_generation_id != runtime.content_generation ||
            scene->layout_abi != runtime.scene.layout_abi || scene->eligible_layout_digest != runtime.scene.layout_digest ||
            scene->eligible_static_members.size() != runtime.scene.members.size())
        {
            InvalidateHlodQualification(*owner);
            owner->matching_scene.reset();
        }
    }
    return PumpHlodDemand();
}

Result D3D12Renderer::Impl::PrepareHlodStaticScene(
    const std::shared_ptr<const StaticRenderScene> &source, std::span<const RenderInstance> &proxies)
{
    proxies = {};
    if (!hlod_runtime) return Result::Success();
    auto &runtime = *hlod_runtime;
    auto *context = ActiveHlodView();
    if (!context) return HlodFailure("HLOD static preparation has no keyed view context.");
    auto &view = *context;
    const auto invalidate = [&]() -> Result {
        view.matching_scene.reset(); InvalidateHlodQualification(view);
        return PumpHlodDemand();
    };
    if (!runtime.prepared_scene.Resolve(source, runtime.scene, runtime.content_generation,
                                         runtime.proxy_material_ordinals)) return invalidate();
    const auto total = source->instances.size() + runtime.scene.proxies.size();
    if (total > UINT32_MAX) return HlodFailure("HLOD static slot span exceeds32-bit addressing.");
    if (total > view.static_capacity)
    {
        if (view.static_capacity)
        {
            if (auto result = WaitForGpu(); !result) return result;
            if (auto result = view.system.ResetAfterCompletion(graph_queue_executor.Completed()); !result) return result;
            static_cast<void>(residency.ReleaseNow(view.residency)); view.residency = {};
        }
        HlodCandidate candidate;
        const auto reader = [this](std::string_view id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto result = ResolveRendererAsset(config, std::string(id), path); !result) return result;
            return ReadBinary(path, bytes);
        };
        if (auto result = view.system.Prepare(device.Get(), allocator, runtime.scene,
            runtime.content_generation, runtime.generation, static_cast<std::uint32_t>(total),
            kFrameCount, reader, candidate); !result) return result;
        const auto bytes = candidate.AllocatedBytes();
        SampleAllocatorBudgetStats(false, false);
        ScopedGpuAdmission admission(residency, bytes);
        if (!admission) return HlodFailure("HLOD selection tables cannot fit the shared residency budget.");
        const auto ticket = RegisterNativeAllocation(ResidencyCategory::Geometry, bytes, true,
            ResidencySegmentForHeap(D3D12_HEAP_TYPE_DEFAULT));
        if (!ticket) return HlodFailure("Could not account HLOD selection allocations.");
        if (auto result = view.system.Commit(std::move(candidate), graph_queue_executor.Completed()); !result)
        { static_cast<void>(residency.ReleaseNow(ticket)); return result; }
        admission.Complete(); view.residency = ticket;
        view.static_capacity = static_cast<std::uint32_t>(total);
    }
    view.matching_scene = source;
    view.proxy_slot_base = static_cast<std::uint32_t>(source->instances.size());
    proxies = runtime.prepared_scene.proxies;
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareHlodFrame(std::uint32_t frame_slot,
                                           std::vector<GpuStaticDrawRef> &refs, bool &refs_changed)
{
    refs_changed = false;
    NullHlodExports(device.Get(), srv_cpu_heap.Get(), srv_stride, frame_slot);
    const auto disabled = [&]() {
        if (static_gpu_scene && &refs == &static_gpu_scene->draws.refs)
        {
            refs_changed = refs.size() != static_gpu_scene->draws.leaf_count;
            refs.resize(static_gpu_scene->draws.leaf_count);
        }
        return Result::Success();
    };
    if (!hlod_runtime || frame_slot >= kFrameCount) return disabled();
    auto &runtime = *hlod_runtime; auto *context = ActiveHlodView();
    if (!context) return HlodFailure("HLOD frame preparation has no keyed view context.");
    auto &view = *context; auto &native = view.frames[frame_slot];
    native.enabled = false; native.registered = false; native.outputs = {};
    if (!view.matching_scene || !static_gpu_scene || static_gpu_scene->source != view.matching_scene)
        return disabled();
    if (!native.completion.IsSatisfiedBy(graph_queue_executor.Completed()) || native.readback_pending)
        return HlodFailure("HLOD frame bank is still leased by GPU/history/demand readers.");
    const auto generation = static_gpu_scene->instance_generation;
    const auto leaf_count = static_gpu_scene->draws.leaf_count;
    const auto &order = runtime.prepared_scene.proxy_order;
    refs_changed = refs.size() != leaf_count + order.size();
    refs.resize(leaf_count + order.size());
    for (std::uint32_t command = 0; command < order.size(); ++command)
    {
        const auto index = order[command];
        const auto &proxy = runtime.scene.proxies[index];
        const GpuStaticDrawRef expected{view.proxy_slot_base + index,
            proxy.mesh_semantic, 1.0f, generation};
        auto &ref = refs[leaf_count + command];
        if (refs_changed || ref.instance_index != expected.instance_index ||
            ref.mesh_flags != expected.mesh_flags ||
            ref.lod_threshold != expected.lod_threshold ||
            ref.instance_generation != expected.instance_generation)
        {
            ref = expected;
            refs_changed = true;
        }
    }
    const bool rebuild_bindings = native.prepared_source != view.matching_scene ||
        native.prepared_generation != generation || native.prepared_leaf_count != leaf_count ||
        native.prepared_hlod_generation != runtime.generation ||
        native.bindings.size() != order.size() || refs_changed ||
        native.prepared_pipeline_revision != pipeline_revision;
    if (rebuild_bindings)
    {
        native.members.clear(); native.bindings.clear(); native.buckets.clear(); native.material_ordinals.clear();
        native.members.reserve(runtime.scene.members.size());
        for (std::size_t index = 0; index < runtime.scene.members.size(); ++index)
            native.members.push_back({runtime.scene.members[index].stable_id,
                runtime.prepared_scene.member_slots[index], generation});
        native.bindings.resize(order.size());
    }
    for (std::uint32_t command = 0; command < order.size(); ++command)
    {
        const auto index = order[command];
        const auto &proxy = runtime.scene.proxies[index];
        const auto material = runtime.proxy_material_ordinals[index];
        const auto ref_index = static_cast<std::uint32_t>(leaf_count + command);
        const auto slot = view.proxy_slot_base + index;
        auto &binding = native.bindings[index];
        if (rebuild_bindings)
        {
            if (native.material_ordinals.empty() || native.material_ordinals.back() != material)
            {
                native.material_ordinals.push_back(material);
                native.buckets.push_back({command, 0u, (material_parameter_bases[material] << 1u) | 1u, 0u});
            }
            ++native.buckets.back().command_count;
            binding.logical_page = proxy.page_index; binding.content_generation = runtime.generation;
            binding.command_ordinal = command; binding.bucket_index = static_cast<std::uint32_t>(native.buckets.size() - 1u);
            binding.instance_slot = slot; binding.instance_generation = generation; binding.visible_ref_index = ref_index;
        }
        binding.flags = 0u; binding.geometry_record_id = 0u;
        binding.physical_page_slot = 0u; binding.page_version = 0u;
        const auto &snapshot = cluster_runtime->frames[frame_slot].snapshot.at(runtime.page_base + proxy.page_index);
        if (!snapshot.resident || snapshot.generation != runtime.generation) continue;
        const auto *page = FindClusterGeometryPage(snapshot.physical_page_slot);
        if (!page || page->generation != snapshot.page_version) continue;
        // Native prefix pressure is an unavailable proxy range. Selection
        // retains complete children/leaves instead of suppressing any member.
        if (frames[frame_slot].geometry_draw_records_cpu.size() >= kGeometryDrawRecordCapacity) continue;
        std::uint32_t packed{};
        const auto flags = kGeometryGpuSceneRefs | kGeometryStaticInstances | kGeometryExplicitMaterialBinding |
            (mesh_shader_enabled && GeometryMeshDispatchFits(proxy.meshlet_count, 1u) ? kGeometryMeshEligible : 0u);
        if (auto result = AddGeometryDrawRecord(frame_slot, *page, ref_index, 1u, flags, 0,
            proxy.mesh_semantic, packed); !result) return result;
        auto &record = frames[frame_slot].geometry_draw_records_cpu[packed >> 2u];
        record.first_index = proxy.first_index; record.index_count = proxy.index_count;
        record.first_meshlet = proxy.first_meshlet; record.meshlet_count = proxy.meshlet_count;
        // Shared physical page slots carry no environment material ordinal.
        // Preserve the same explicit surface identity used by cluster Emit.
        record.lod = 0x10000000u + material;
        record.reserved1 = 4u; // Existing complete immutable static input refs.
        binding.flags = kHlodProxyResident; binding.geometry_record_id = packed >> 2u;
        binding.physical_page_slot = page->page_slot; binding.page_version = page->generation;
    }
    native.prepared_source = view.matching_scene;
    native.prepared_generation = generation;
    native.prepared_hlod_generation = runtime.generation;
    native.prepared_leaf_count = leaf_count;
    native.prepared_pipeline_revision = pipeline_revision;
    const auto readback_bytes = std::max<std::uint64_t>(4u, runtime.scene.pages.size() * 4u) + 64u;
    const auto ensure = [&](AllocationResource &resource, ResidencyTicket &ticket, std::uint64_t bytes,
                            D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) -> Result {
        if (resource.resource && resource.resource->GetDesc().Width >= bytes) return Result::Success();
        const auto description = BufferDescription(bytes);
        const auto allocated = device->GetResourceAllocationInfo(0, 1, &description).SizeInBytes;
        SampleAllocatorBudgetStats(false, false);
        ScopedGpuAdmission admission(residency, allocated, ResidencySegmentForHeap(heap));
        if (allocated == UINT64_MAX || !admission) return HlodFailure("HLOD frame staging exceeds shared residency budget.");
        AllocationResource next;
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = heap;
        if (auto result = CreateAllocation(next, allocation, description, state); !result) return result;
        const auto next_ticket = RegisterNativeAllocation(ResidencyCategory::Staging,
            next.allocation->GetSize(), false, ResidencySegmentForHeap(heap));
        if (!next_ticket) return HlodFailure("Could not account HLOD frame staging.");
        static_cast<void>(residency.ReleaseNow(ticket));
        next.graph_access = heap == D3D12_HEAP_TYPE_UPLOAD ? Access::ShaderRead : Access::CopyDest;
        resource = std::move(next); ticket = next_ticket; admission.Complete();
        return Result::Success();
    };
    if (auto result = ensure(native.readback, native.readback_residency, readback_bytes, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST); !result) return result;
    native.enabled = true; native.frame_id = frame_number + 1u;
    native.qualification_epoch = view.qualification_epoch;
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareHlodFrameRecordData(std::uint32_t frame_slot,std::uint32_t active_view_mask,
    ResourceVersion geometry_records,ResourceVersion static_scene,ResourceVersion proxy_refs,
    HlodRuntimeFrameRecordData &record)
{
    record={};record.inputs.frame_slot=frame_slot;record.inputs.active_view_mask=active_view_mask;
    if(active_view_mask!=1u&&active_view_mask!=15u)
        return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","HLOD active view mask must select camera or all shadow views.");
    auto *context=ActiveHlodView();
    if(!context||!context->frames.at(frame_slot).enabled)return Result::Success();
    record.view=context;
    auto &view=*context;auto &native=view.frames[frame_slot];auto &inputs=record.inputs;
    inputs.frame_slot = frame_slot; inputs.active_view_mask = active_view_mask;
    inputs.rendered_frame_id = native.frame_id; inputs.static_scene_generation = static_gpu_scene->instance_generation;
    const auto &owner = *owned_views.at(active_view_id.value);
    const bool camera_cut = explicit_view && explicit_view->camera_cut;
    const bool scene_changed = frame_number != 0u && owner.scene_lease != view.matching_scene;
    // These increments match successful publication in renderer_frame/views.
    // The cut frame cannot read history, but its output owns the new generation.
    inputs.history_generation = owner.history_generation + (camera_cut ? 1u : 0u) + (scene_changed ? 1u : 0u);
    inputs.history_valid = !camera_cut && !scene_changed && view.session_id == view_session_id &&
        previous_origins_valid && explicit_view && explicit_view->scene_origin == previous_scene_origin;
    inputs.completed = graph_queue_executor.Completed(); inputs.content_generation_id = resource_generation_id;
    inputs.layout_abi = view.matching_scene->layout_abi; inputs.layout_digest = view.matching_scene->eligible_layout_digest;
    inputs.members = native.members; inputs.proxies = native.bindings; inputs.buckets = native.buckets; inputs.views = native.views;
    inputs.static_scene = {static_gpu_scene->instances.resource.Get(), static_scene,static_gpu_scene->instances.NativeContract()};
    inputs.geometry_records = {frames[frame_slot].geometry_draw_records.resource.Get(), geometry_records,frames[frame_slot].geometry_draw_records.NativeContract()};
    auto &refs = static_gpu_scene->refs->storage;
    inputs.proxy_refs = {refs.resource.Get(), proxy_refs, refs.NativeContract()};
    return view.system.PrepareFrameRecordData(inputs,record.selection);
}

bool D3D12Renderer::Impl::MatchesHlodFrameLayout(const HlodRuntimeFrameRecordData &record,
    const HlodRuntimeFrameLayout &layout)const noexcept
{
    const auto *view=ActiveHlodView();
    if(!layout.valid||layout.active!=bool(record.view))return false;
    if(!record.view)return !view||!view->frames[record.inputs.frame_slot].enabled;
    return record.view==view&&view->frames[record.inputs.frame_slot].enabled&&
        record.inputs.proxy_refs.version==layout.selection.guides[2]&&
        view->system.MatchesLayout(record.inputs,record.selection,layout.selection);
}

Result D3D12Renderer::Impl::AddHlodFramePasses(std::uint32_t frame_slot,std::uint32_t active_view_mask,
    ResourceVersion geometry_records,ResourceVersion static_scene,ResourceVersion static_refs,
    HlodFrameGraph &state,HlodRuntimeFrameLayout *capture,HlodRuntimeFrameRecordData *prepared,
    const HlodRuntimeFrameLayout *retained)
{
    state={};if(capture)*capture={};
    if(active_view_mask!=1u&&active_view_mask!=15u)
        return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","HLOD active view mask must select camera or all shadow views.");
    auto *context=ActiveHlodView();
    const bool active=context&&context->frames.at(frame_slot).enabled;
    if(retained&&(!retained->valid||retained->active!=active))
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","HLOD retained active topology changed.");
    if(!active){if(capture)capture->valid=true;return Result::Success();}
    auto &view=*context;auto &native=view.frames[frame_slot];state.view=context;
    if (!static_gpu_scene || !static_gpu_scene->refs || !static_refs.resource ||
        (retained && retained->refs != static_refs.resource))
        return HlodFailure("HLOD static refs do not match the scene-owned graph import.");
    state.refs = static_refs.resource;
    HlodRuntimeFrameRecordData local;
    if(!prepared)
    {
        if(auto result=PrepareHlodFrameRecordData(frame_slot,active_view_mask,geometry_records,static_scene,static_refs,local);!result)return result;
        prepared=&local;
    }
    if(prepared->view!=context||prepared->inputs.frame_slot!=frame_slot||prepared->inputs.active_view_mask!=active_view_mask)
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","HLOD prepared runtime frame changed.");
    // Logical slots can be assigned cold after the CPU plan was prepared.
    prepared->inputs.geometry_records.version=geometry_records;prepared->inputs.static_scene.version=static_scene;
    prepared->inputs.proxy_refs.version=static_refs;
    if(retained&&!MatchesHlodFrameLayout(*prepared,*retained))
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","HLOD retained runtime layout changed.");
    auto result=retained?view.system.BindRetained(graph,prepared->inputs,prepared->selection,retained->selection,state.outputs):
        view.system.Register(graph,prepared->inputs,state.outputs,capture?&capture->selection:nullptr,&prepared->selection);
    if(!result)return result;
    state.registered=true;native.registered=true;native.outputs=state.outputs;
    auto descriptor=srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    descriptor.ptr+=static_cast<SIZE_T>(frame_slot*kTextureDescriptorCount+kHlodDescriptorOffset)*srv_stride;
    for(const auto source:state.outputs.srvs)
    {
        device->CopyDescriptorsSimple(1,descriptor,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        descriptor.ptr+=srv_stride;
    }
    if(auto checked=native.readback.EnsureNativeContract(device.Get());!checked)return checked;
    state.readback=retained?retained->readback:graph.ImportBuffer({native.readback.resource.Get(),native.readback.graph_completion,
        native.readback.graph_queue,{},native.readback.NativeContract()},native.readback.graph_access,"HlodDemandStatusReadback");
    if(retained&&!graph.BindBuffer(state.readback,{native.readback.resource.Get(),native.readback.graph_completion,
        native.readback.graph_queue,{},native.readback.NativeContract()},native.readback.graph_access))
        return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","HLOD retained demand readback binding failed.");
    if(capture)capture->copy_pass=graph.PassCount();
    auto copyBinding=retained?graph.BindPass(retained->copy_pass,196u):
        std::optional<PassBuilder>{graph.AddPass(196u,"HlodDemandStatusReadback",QueueHint::Copy)};
    if(!copyBinding)return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","HLOD retained demand copy pass is missing.");
    auto &copy=*copyBinding;
    if(!retained)
    {
        copy.Read(state.outputs.page_demand,Access::CopySource);copy.Read(state.outputs.status,Access::CopySource);
        (void)copy.Write(graph.Version(state.readback),Access::CopyDest);
    }
    copy.SetExecute([frame=&native,bytes=hlod_runtime->scene.pages.size()*4ull](RenderPassContext &context){
        auto *list=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        if(bytes)list->CopyBufferRegion(frame->readback.resource.Get(),0,frame->outputs.resources[5],0,bytes);
        list->CopyBufferRegion(frame->readback.resource.Get(),bytes,frame->outputs.resources[6],0,64u);
    });
    if(capture){capture->valid=true;capture->active=true;capture->refs=state.refs;capture->readback=state.readback;}
    return Result::Success();
}

void D3D12Renderer::Impl::ReadHlodFrame(PassBuilder &pass, const HlodFrameGraph &state, bool draw) const
{
    if (!state.registered) return;
    pass.Read(state.outputs.leaf_masks, draw ? Access::ShaderRead : Access::ComputeRead);
    pass.Read(state.outputs.proxy_selection, draw ? Access::ShaderRead : Access::ComputeRead);
    if (draw)
    {
        pass.Read(state.outputs.indexed_commands, Access::IndirectArgs);
        pass.Read(state.outputs.mesh_commands, Access::IndirectArgs);
        pass.Read(state.outputs.command_counts, Access::IndirectArgs);
        // The owning pass declares shared scene refs once with its ordinary static draws.
    }
}

Result D3D12Renderer::Impl::DrawHlodGeometry(ID3D12GraphicsCommandList *list,
    std::uint32_t frame_slot, std::uint32_t view)
{
    const auto *context = ActiveHlodView();
    if (!context || !context->frames.at(frame_slot).registered) return Result::Success();
    if (!list || view >= 4u) return HlodFailure("HLOD draw has an invalid native list/view.");
    const auto &native = context->frames[frame_slot];
    list->SetGraphicsRootShaderResourceView(34, static_gpu_scene->instances.resource->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(35,
        static_gpu_scene->refs->storage.resource->GetGPUVirtualAddress());
    for (std::uint32_t index = 0; index < native.buckets.size(); ++index)
    {
        const auto &bucket = native.buckets[index]; const auto material = native.material_ordinals[index];
        ID3D12PipelineState *pipeline{};
        bool mesh = mesh_shader_enabled;
        // A bucket is uniform by native material/frontend. The cooker page
        // limit keeps every range in the mesh dispatch bound when enabled.
        for (const auto &binding : native.bindings)
            if (binding.bucket_index == index && (binding.flags & kHlodProxyResident) != 0u &&
                (frames[frame_slot].geometry_draw_records_cpu[binding.geometry_record_id].flags & kGeometryMeshEligible) == 0u)
                mesh = false;
        if (mesh)
        {
            auto *indexed = view ? environment_material_shadow_pipelines[material].Get() :
                config.surface_frontend == SurfaceFrontend::VisibilityBuffer ? environment_material_visibility_pipelines[material].Get() :
                environment_material_scene_pipelines[material].Get();
            for (const auto &pair : mesh_pipeline_pairs) if (pair.indexed == indexed) { pipeline = pair.mesh.Get(); break; }
        }
        else pipeline = view ? cluster_shadow_pipelines[material].Get() :
            config.surface_frontend == SurfaceFrontend::VisibilityBuffer ? cluster_visibility_pipelines[material].Get() : cluster_scene_pipelines[material].Get();
        auto *signature = mesh ? cluster_mesh_signature.Get() : cluster_draw_signature.Get();
        if (!pipeline || !signature) return HlodFailure("HLOD material has no native pipeline/signature.");
        list->SetPipelineState(pipeline);
        list->SetGraphicsRoot32BitConstant(6, bucket.material_word, 1u);
        const auto stride = mesh ? 16ull : 24ull;
        const auto offset = (static_cast<std::uint64_t>(view) * native.outputs.proxy_command_capacity + bucket.first_command) * stride;
        list->ExecuteIndirect(signature, bucket.command_count, native.outputs.resources[mesh ? 3u : 2u], offset,
            native.outputs.resources[4], (static_cast<std::uint64_t>(view) * native.outputs.bucket_count + index) * 4u);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CommitHlodFrameGraph(std::uint32_t frame_slot,
    HlodFrameGraph &state, const CompletionSet &completion)
{
    if (!hlod_runtime || !state.registered) return Result::Success();
    if (!state.view) return HlodFailure("Registered HLOD graph lost its view owner.");
    auto &view = *state.view; auto &native = view.frames.at(frame_slot);
    if (auto result = view.system.CompleteSubmission(graph, completion, true); !result) return result;
    native.completion = completion;
    native.readback.graph_access = graph.FinalAccess(state.readback);
    native.readback.graph_queue = graph.FinalQueue(state.readback);
    native.readback.graph_completion = graph.ResourceCompletion(state.readback);
    native.readback_pending = true; native.readback_completion = native.readback.graph_completion;
    for (std::uint32_t slot = 0; slot < kFrameCount; ++slot)
    {
        const auto readers = view.system.FrameCompletion(slot);
        view.frames[slot].completion.Merge(readers); frames[slot].completion.Merge(readers);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::PumpHlodDemand()
{
    if (!hlod_runtime) return Result::Success();
    if (!cluster_runtime) return HlodFailure("HLOD demand has no shared page runtime.");
    auto &runtime = *hlod_runtime; auto &pool = *cluster_runtime;
    const auto completed = graph_queue_executor.Completed();
    if (pool.first_physical_slot > pool.physical_owner.size())
        return HlodFailure("HLOD shared physical page range is invalid.");
    const auto mandatory = static_cast<std::uint32_t>(std::count_if(pool.streams.begin(), pool.streams.end(),
        [](const auto &stream) { return stream.catalog < kClusterRuntimeCatalogs && stream.pinned; }));
    const auto shared_capacity = static_cast<std::uint32_t>(pool.physical_owner.size()) - pool.first_physical_slot;
    if (mandatory > shared_capacity) return HlodFailure("Mandatory mesh roots exceed the shared physical page range.");
    const auto capacity = shared_capacity - mandatory;
    std::fill(runtime.group_priorities.begin(), runtime.group_priorities.end(), 0u);
    for (auto &[key, context] : runtime.views)
    {
        auto &view = *context;
        std::vector<std::uint32_t> latest_demand;
        auto latest_frame_id = view.demand_frame_id;
        for (auto &frame : view.frames)
        {
            if (!frame.readback_pending || !frame.readback_completion.IsSatisfiedBy(completed)) continue;
            const auto bytes = runtime.scene.pages.size() * 4u;
            void *mapped{}; const D3D12_RANGE read{0, bytes + 64u};
            const auto hr = frame.readback.resource->Map(0, &read, &mapped);
            if (FAILED(hr)) return HResultFailure("Map HLOD page demand/status", hr);
            std::vector<std::uint32_t> demand(runtime.scene.pages.size());
            std::array<std::uint32_t, 16> status{};
            if (bytes) std::memcpy(demand.data(), mapped, bytes);
            std::memcpy(status.data(), static_cast<const std::byte *>(mapped) + bytes, 64u);
            const D3D12_RANGE no_write{}; frame.readback.resource->Unmap(0, &no_write);
            frame.readback_pending = false;
            if (status[8] != runtime.generation ||
                (static_cast<std::uint64_t>(status[10]) << 32u | status[9]) != frame.frame_id)
                return HlodFailure("HLOD demand/status belongs to a stale Generation/rendered view frame.");
            const auto status_frame = static_cast<std::uint64_t>(view.last_status[10]) << 32u | view.last_status[9];
            if (frame.frame_id >= status_frame) view.last_status = status;
            // Frame numbers order only this view/session. Omission or a layout
            // mismatch invalidates qualification without destroying history.
            if (!view.participating || !view.matching_scene || frame.qualification_epoch != view.qualification_epoch) continue;
            if (frame.frame_id >= latest_frame_id && (latest_demand.empty() || frame.frame_id > latest_frame_id))
            {
                latest_frame_id = frame.frame_id; latest_demand = std::move(demand);
            }
        }
        if (!latest_demand.empty())
        {
            view.demand_frame_id = latest_frame_id;
            for (const auto priority : latest_demand)
                if (priority && (!std::isfinite(std::bit_cast<float>(priority)) || std::bit_cast<float>(priority) <= 0.0f))
                    return HlodFailure("HLOD demand priority must encode a finite positive value.");
            std::fill(view.group_priorities.begin(), view.group_priorities.end(), 0u);
            for (std::uint32_t group = 0; group < runtime.scene.groups.size(); ++group)
            {
                const auto &pages = runtime.group_pages[group];
                if (!runtime.scene.groups[group].child_count || pages.empty() || pages.size() > capacity) continue;
                std::uint32_t priority{}; bool complete = true;
                for (const auto page : pages)
                {
                    complete = complete && latest_demand[page] != 0u;
                    priority = std::max(priority, latest_demand[page]);
                }
                if (complete) view.group_priorities[group] = priority;
            }
        }
        if (!view.participating || !view.matching_scene) continue;
        for (std::uint32_t group = 0; group < runtime.group_priorities.size(); ++group)
            runtime.group_priorities[group] = std::max(runtime.group_priorities[group], view.group_priorities[group]);
    }
    std::vector<std::uint8_t> admitted(runtime.scene.groups.size());
    std::vector<std::uint32_t> frontier;
    std::uint32_t admitted_pages{};
    const auto admit = [&](std::uint32_t group) {
        const auto &pages = runtime.group_pages[group];
        if (!runtime.group_priorities[group] || pages.size() > capacity - admitted_pages) return;
        const auto &entry = runtime.scene.groups[group];
        const auto first = static_cast<std::uint64_t>(entry.first_member), end = first + entry.member_count;
        for (const auto other : frontier)
        {
            const auto &interval = runtime.scene.groups[other];
            const auto other_first = static_cast<std::uint64_t>(interval.first_member);
            if (first < other_first + interval.member_count && other_first < end) return;
        }
        admitted[group] = 1u; frontier.push_back(group);
        admitted_pages += static_cast<std::uint32_t>(pages.size());
    };
    // Keep qualified loading/resident groups before admitting new competitors;
    // otherwise each new priority snapshot can evict another partial group.
    for (std::uint32_t group = 0; group < runtime.admitted_groups.size(); ++group)
        if (runtime.admitted_groups[group]) admit(group);
    const auto retained = admitted;
    std::vector<std::uint32_t> order(runtime.scene.groups.size());
    std::iota(order.begin(), order.end(), 0u);
    std::ranges::sort(order, [&](auto left, auto right) {
        const auto a = runtime.group_priorities[left], b = runtime.group_priorities[right];
        return a == b ? left < right : a > b;
    });
    for (const auto group : order) if (!admitted[group]) admit(group);
    const auto apply = [&](const std::vector<std::uint8_t> &groups) -> Result {
        std::vector<std::uint8_t> pages(runtime.scene.pages.size());
        for (std::uint32_t group = 0; group < groups.size(); ++group)
            if (groups[group]) for (const auto page : runtime.group_pages[group]) pages[page] = 1u;
        for (std::uint32_t page = 0; page < pages.size(); ++page)
        {
            auto &stream = pool.streams.at(runtime.page_base + page);
            if (pages[page]) stream.hlod_admitted = true;
            else if (auto result = WithdrawHlodPage(stream, asset_io, streaming, residency); !result) return result;
        }
        runtime.admitted_groups = groups;
        return Result::Success();
    };
    if (auto result = apply(admitted); !result) return result;
    for (const auto group : frontier)
        for (const auto page : runtime.group_pages[group])
            if (auto result = DemandClusterPage(runtime.page_base + page, runtime.generation,
                runtime.group_priorities[group]); !result)
            {
                // A registration failure releases new reservations and their
                // pre-upload requests, while retained groups and GPU leases
                // remain intact. The failure is still reported to the caller.
                if (auto rollback = apply(retained); !rollback) return rollback;
                return result;
            }
    runtime.admission_status[0] = static_cast<std::uint32_t>(frontier.size());
    runtime.admission_status[1] = admitted_pages;
    runtime.admission_status[2] = static_cast<std::uint32_t>(std::count_if(pool.streams.begin(), pool.streams.end(),
        [](const auto &page) { return page.hlod_admitted && page.physical_slot == UINT32_MAX; }));
    runtime.admission_status[3] = capacity;
    return Result::Success();
}

void D3D12Renderer::Impl::ResetHlodResources() noexcept
{
    if (!hlod_runtime) return;
    const auto completed = graph_queue_executor.Completed();
    for (const auto &[key, view] : hlod_runtime->views)
        static_cast<void>(ResetHlodView(*view, residency, completed));
    hlod_runtime.reset();
}
} // namespace hs
