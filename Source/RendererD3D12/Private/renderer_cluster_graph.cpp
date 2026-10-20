#include "renderer_impl.hpp"
#include "renderer_cluster_graph.hpp"

#include <stdexcept>

namespace hs
{
namespace
{
Result GraphFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}
void DispatchCluster(ID3D12GraphicsCommandList *list, std::uint32_t elements)
{
    const auto groups = (static_cast<std::uint64_t>(elements) + 63u) / 64u;
    if (!groups) return;
    list->Dispatch(static_cast<UINT>(std::min<std::uint64_t>(groups, 65535u)),
        static_cast<UINT>((groups + 65534u) / 65535u), 1u);
}
}

Result D3D12Renderer::Impl::AddClusterFramePasses(std::uint32_t frame_slot,
    ResourceVersion &geometry_records, ResourceVersion *visibility_raster_records,
    ResourceVersion static_scene, ClusterFrameGraph &state)
{
    if (!cluster_runtime || frame_slot >= kClusterRuntimeFrames || state.prepared ||
        !geometry_records.resource || !cluster_root_signature)
        return GraphFailure("Cluster graph has an invalid runtime/frame/shared record input.");
    auto &runtime = *cluster_runtime;
    auto &native = runtime.frames[frame_slot];
    state.frame_slot = frame_slot;
    state.visibility = visibility_raster_records != nullptr;
    state.geometry_records = geometry_records;
    if (state.visibility) state.raster_records = *visibility_raster_records;
    const auto import = [&](AllocationResource &resource, std::string_view name) -> ClusterGraphBuffer {
        if (!resource.resource || resource.graph_access == Access::Undefined)
            throw std::runtime_error("cluster graph imports an absent/uninitialized native buffer");
        for (const auto &entry : state.imports)
            if (entry.native == &resource) return entry;
        const auto handle = graph.ImportBuffer({resource.resource.Get(), resource.graph_completion,
            resource.graph_queue}, resource.graph_access, name);
        ClusterGraphBuffer entry{&resource, handle, graph.Version(handle)};
        state.imports.push_back(entry);
        return entry;
    };
    try
    {
        state.groups = import(runtime.groups, "ClusterGroups");
        state.children = import(runtime.children, "ClusterChildren");
        state.clusters = import(runtime.clusters, "ClusterRecords");
        state.pages = import(native.pages, "ClusterResidency");
        for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
        {
            const auto &cut = native.cuts[ordinal];
            if (!cut.candidate_count) continue;
            if (!static_scene.resource || !static_gpu_scene || !cut.output_capacity ||
                cut.generation != runtime.generation || !native.refs.resource)
                return GraphFailure("Active cluster cuts require the persistent static scene and prepared capacity.");
            state.draws = true;
            auto &entry = state.cuts[ordinal];
            entry.active = true;
            entry.counts = import(native.cuts[ordinal].counts, "ClusterCutCounts");
            entry.offsets = import(native.cuts[ordinal].offsets, "ClusterCutOffsets");
            entry.block_sums = import(native.cuts[ordinal].block_sums, "ClusterBlockSums");
            entry.block_offsets = import(native.cuts[ordinal].block_offsets, "ClusterBlockOffsets");
            entry.demand = import(native.cuts[ordinal].demand, "ClusterPageDemand");
            entry.status = import(native.cuts[ordinal].status, "ClusterCutStatus");
            entry.draw_commands = import(native.cuts[ordinal].draw_commands, "ClusterDrawCommands");
            entry.mesh_commands = import(native.cuts[ordinal].mesh_commands, "ClusterMeshCommands");
            entry.command_counts = import(native.cuts[ordinal].command_counts, "ClusterCommandCounts");
            entry.readback = import(native.cuts[ordinal].readback, "ClusterDemandStatusReadback");
            entry.history_bits = import(native.cuts[ordinal].history_bits, "ClusterSelectedMembership");
            entry.history_generations = import(native.cuts[ordinal].history_generations, "ClusterSelectedInstanceGenerations");
            entry.selected_bounds = import(native.cuts[ordinal].selected_bounds, "ClusterSelectedWorldBounds");
            entry.previous_rejected = import(native.cuts[ordinal].previous_rejected, "ClusterPreviousHzbRejects");
            entry.retest_draw_commands = import(native.cuts[ordinal].retest_draw_commands, "ClusterRetestDrawCommands");
            entry.retest_mesh_commands = import(native.cuts[ordinal].retest_mesh_commands, "ClusterRetestMeshCommands");
            entry.previous_frame_slot = cut.previous_history_slot;
            auto *previous = cut.previous_history_slot != UINT32_MAX ? &runtime.frames[cut.previous_history_slot] : nullptr;
            auto *previous_cut = previous ? &previous->cuts[ordinal] : nullptr;
            entry.previous_bits = import(previous_cut ? previous_cut->history_bits : runtime.history_zero, "ClusterPreviousMembership");
            entry.previous_generations = import(previous_cut ? previous_cut->history_generations : runtime.history_zero, "ClusterPreviousInstanceGenerations");
            entry.previous_pages = previous ? import(previous->pages, "ClusterPreviousResidency") : state.pages;
            entry.previous_status = import(previous_cut ? previous_cut->status : runtime.history_zero, "ClusterPreviousCutStatus");
        }
        if (state.draws) state.refs = import(native.refs, "ClusterVisibleRefs");
        for (auto &stream : runtime.streams)
        {
            const auto &snapshot = native.snapshot[stream.logical_slot];
            const bool resident = snapshot.resident && snapshot.generation == runtime.generation &&
                snapshot.physical_page_slot == stream.physical_slot && snapshot.page_version == stream.version;
            if (!resident && !stream.upload_pending) continue;
            if (!stream.page) return GraphFailure("Cluster graph snapshot/upload has no native page.");
            ClusterGraphPage page;
            page.logical_slot = stream.logical_slot;
            page.resident = resident;
            page.upload = stream.upload_pending;
            const std::array buffers{&stream.page->vertices, &stream.page->indices,
                &stream.page->meshlets, &stream.page->auxiliary};
            constexpr std::array names{"ClusterVertices", "ClusterIndices", "ClusterMeshlets", "ClusterAuxiliary"};
            for (std::size_t i = 0; i < buffers.size(); ++i) page.buffers[i] = import(*buffers[i], names[i]);
            state.geometry.push_back(std::move(page));
        }
    }
    catch (const std::exception &error) { return GraphFailure(error.what()); }
    if (native.page_upload_pending || std::ranges::any_of(state.geometry, [](const auto &page) { return page.upload; }))
    {
        auto upload = graph.AddPass(39u, "ClusterPageUpload", QueueHint::Copy);
        if (native.page_upload_pending) state.pages.version = upload.Write(state.pages.version, Access::CopyDest);
        for (auto &page : state.geometry)
            if (page.upload)
                for (auto &buffer : page.buffers) buffer.version = upload.Write(buffer.version, Access::CopyDest);
        upload.SetExecute([this, frame_slot, &state](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            if (cluster_runtime->frames[frame_slot].page_upload_pending) RecordClusterPageTableUpload(list, frame_slot);
            for (const auto &page : state.geometry)
                if (page.upload) RecordClusterPageUpload(list, page.logical_slot);
        });
    }
    if (state.draws)
    {
        // The extra immutable descriptor in each phase bank carries the
        // generation-qualified scene HLOD leaf export (or typed null).
        for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
            if (state.cuts[ordinal].active)
                for (std::uint32_t phase = 0; phase < kClusterOcclusionPhases; ++phase)
                {
                    auto target = runtime.descriptors->GetCPUDescriptorHandleForHeapStart();
                    target.ptr += static_cast<SIZE_T>(((frame_slot * kClusterRuntimeCatalogs + ordinal) *
                        kClusterOcclusionPhases + phase) * kClusterCutBankStride + 28u) * srv_stride;
                    if (auto *hlod_view = ActiveHlodView(); hlod_view && hlod_view->frames[frame_slot].registered)
                        device->CopyDescriptorsSimple(1, target, hlod_view->frames[frame_slot].outputs.srvs[0],
                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    else
                    {
                        D3D12_SHADER_RESOURCE_VIEW_DESC descriptor{};
                        descriptor.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                        descriptor.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        descriptor.Buffer.NumElements = 1; descriptor.Buffer.StructureByteStride = 16;
                        device->CreateShaderResourceView(nullptr, &descriptor, target);
                    }
                }
        const auto bind = [this, frame_slot](ID3D12GraphicsCommandList *list,
                                           std::uint32_t ordinal, std::uint32_t stage) {
            auto &runtime = *cluster_runtime;
            auto &cut = runtime.frames[frame_slot].cuts[ordinal];
            ID3D12DescriptorHeap *heap[]{runtime.descriptors.Get()};
            list->SetDescriptorHeaps(1, heap);
            list->SetComputeRootSignature(cluster_root_signature.Get());
            list->SetComputeRootConstantBufferView(0, cut.constants.resource->GetGPUVirtualAddress() + stage * kClusterConstantStride);
            list->SetComputeRootShaderResourceView(1, static_gpu_scene->instances.resource->GetGPUVirtualAddress());
            list->SetComputeRootConstantBufferView(3, cut.occlusion_constants.resource->GetGPUVirtualAddress());
            auto handle = runtime.descriptors->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>((frame_slot * kClusterRuntimeCatalogs + ordinal) *
                kClusterOcclusionPhases * kClusterCutBankStride) * srv_stride;
            list->SetComputeRootDescriptorTable(2, handle);
            handle.ptr += static_cast<UINT64>(28u) * srv_stride;
            list->SetComputeRootDescriptorTable(4, handle);
        };
        const auto read_tables = [&](PassBuilder &pass) {
            pass.Read(static_scene, Access::ComputeRead);
            if (auto *hlod_view = ActiveHlodView(); hlod_view && hlod_view->frames[frame_slot].registered)
                pass.Read(hlod_view->frames[frame_slot].outputs.leaf_masks, Access::ComputeRead);
            pass.Read(state.groups.version, Access::ComputeRead);
            pass.Read(state.children.version, Access::ComputeRead);
            pass.Read(state.clusters.version, Access::ComputeRead);
            std::vector<ResourceHandle> declared;
            const auto read_once = [&](ResourceVersion version) {
                const auto found = std::ranges::find_if(declared, [&](const auto &resource) {
                    return resource.value == version.resource.value && resource.kind == version.resource.kind;
                });
                if (found == declared.end())
                {
                    declared.push_back(version.resource);
                    pass.Read(version, Access::ComputeRead);
                }
            };
            read_once(state.pages.version);
            for (const auto &cut : state.cuts)
                if (cut.active)
                {
                    read_once(cut.previous_bits.version);
                    read_once(cut.previous_generations.version);
                    if (cut.previous_frame_slot != UINT32_MAX) read_once(cut.previous_pages.version);
                    read_once(cut.previous_status.version);
                }
        };
        auto count = graph.AddPass(40u, "ClusterCutCount", QueueHint::Compute);
        read_tables(count);
        state.refs.version = count.Write(state.refs.version, Access::UnorderedWrite);
        for (auto &cut : state.cuts)
            if (cut.active)
                for (auto *buffer : {&cut.counts, &cut.offsets, &cut.block_sums, &cut.block_offsets,
                    &cut.demand, &cut.status, &cut.draw_commands, &cut.mesh_commands, &cut.command_counts,
                    &cut.selected_bounds, &cut.previous_rejected, &cut.retest_draw_commands, &cut.retest_mesh_commands})
                    buffer->version = count.Write(buffer->version, Access::UnorderedWrite);
        for (auto &cut : state.cuts)
            if (cut.active)
                for (auto *buffer : {&cut.history_bits, &cut.history_generations})
                    buffer->version = count.Write(buffer->version, Access::UnorderedWrite);
        count.SetExecute([this, frame_slot, &state, bind](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            constexpr UINT zero[4]{};
            const auto clear = [&](std::uint32_t ordinal, std::uint32_t uav, AllocationResource &resource) {
                auto gpu = cluster_runtime->descriptors->GetGPUDescriptorHandleForHeapStart();
                auto cpu = cluster_runtime->clear_descriptors->GetCPUDescriptorHandleForHeapStart();
                const auto offset = (frame_slot * kClusterRuntimeCatalogs + ordinal) *
                    kClusterOcclusionPhases * kClusterCutBankStride + kClusterCutClearOffset + uav;
                gpu.ptr += static_cast<UINT64>(offset) * srv_stride;
                cpu.ptr += static_cast<SIZE_T>(offset) * srv_stride;
                list->ClearUnorderedAccessViewUint(gpu, cpu, resource.resource.Get(), zero, 0, nullptr);
            };
            ID3D12DescriptorHeap *heaps[]{cluster_runtime->descriptors.Get()};
            list->SetDescriptorHeaps(1, heaps);
            bool refs_cleared{};
            for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
            {
                if (!state.cuts[ordinal].active) continue;
                auto &cut = cluster_runtime->frames[frame_slot].cuts[ordinal];
                // Emit overwrites every published argument; its count excludes the unused tail.
                const std::array resources{&cut.counts, &cut.offsets, &cut.block_sums, &cut.block_offsets,
                    &cut.demand, &cut.status, &cut.command_counts,
                    &cut.history_bits, &cut.history_generations, &cut.selected_bounds, &cut.previous_rejected,
                    &cut.retest_draw_commands, &cut.retest_mesh_commands};
                constexpr std::array<std::uint32_t, 13> slots{0, 1, 2, 3, 4, 5, 10, 12, 13, 14, 15, 16, 17};
                for (std::size_t i = 0; i < resources.size(); ++i) clear(ordinal, slots[i], *resources[i]);
                if (!refs_cleared)
                {
                    clear(ordinal, 7u, cluster_runtime->frames[frame_slot].refs);
                    refs_cleared = true;
                }
            }
            // Complete all initialization before shader work so the next catalog's
            // barriers cannot serialize an otherwise independent Count dispatch.
            for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
            {
                if (!state.cuts[ordinal].active) continue;
                for (const auto *buffer : {&state.cuts[ordinal].counts, &state.cuts[ordinal].offsets,
                    &state.cuts[ordinal].block_sums, &state.cuts[ordinal].block_offsets,
                    &state.cuts[ordinal].demand, &state.cuts[ordinal].status, &state.cuts[ordinal].draw_commands,
                    &state.cuts[ordinal].mesh_commands, &state.cuts[ordinal].command_counts,
                    &state.cuts[ordinal].history_bits, &state.cuts[ordinal].history_generations,
                    &state.cuts[ordinal].selected_bounds, &state.cuts[ordinal].previous_rejected,
                    &state.cuts[ordinal].retest_draw_commands, &state.cuts[ordinal].retest_mesh_commands}) context.UavBarrier(buffer->handle);
            }
            context.UavBarrier(state.refs.handle);
            list->SetPipelineState(cluster_count_pipeline.Get());
            for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
            {
                if (!state.cuts[ordinal].active) continue;
                bind(list, ordinal, 0);
                DispatchCluster(list, cluster_runtime->frames[frame_slot].cuts[ordinal].candidate_count);
            }
        });
        for (std::uint32_t stage = 0; stage < 3; ++stage)
        {
            constexpr std::array names{"ClusterCutPrefix0", "ClusterCutPrefix1", "ClusterCutPrefix2"};
            auto prefix = graph.AddPass(41u + stage, names[stage], QueueHint::Compute);
            for (auto &cut : state.cuts)
            {
                if (!cut.active) continue;
                cut.status.version = prefix.ReadWrite(cut.status.version, Access::UnorderedWrite);
                cut.command_counts.version = prefix.ReadWrite(cut.command_counts.version, Access::UnorderedWrite);
                if (stage == 0)
                {
                    prefix.Read(cut.counts.version, Access::UnorderedRead);
                    cut.offsets.version = prefix.ReadWrite(cut.offsets.version, Access::UnorderedWrite);
                    cut.block_sums.version = prefix.ReadWrite(cut.block_sums.version, Access::UnorderedWrite);
                }
                else if (stage == 1)
                {
                    prefix.Read(cut.block_sums.version, Access::UnorderedRead);
                    cut.block_offsets.version = prefix.ReadWrite(cut.block_offsets.version, Access::UnorderedWrite);
                }
                else
                {
                    prefix.Read(cut.block_offsets.version, Access::UnorderedRead);
                    cut.offsets.version = prefix.ReadWrite(cut.offsets.version, Access::UnorderedWrite);
                    cut.draw_commands.version = prefix.ReadWrite(cut.draw_commands.version, Access::UnorderedWrite);
                    cut.mesh_commands.version = prefix.ReadWrite(cut.mesh_commands.version, Access::UnorderedWrite);
                }
            }
            prefix.SetExecute([this, frame_slot, &state, bind, stage](RenderPassContext &context) {
                auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                list->SetPipelineState(cluster_prefix_pipeline.Get());
                for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
                {
                    if (!state.cuts[ordinal].active) continue;
                    bind(list, ordinal, stage);
                    const auto &cut = cluster_runtime->frames[frame_slot].cuts[ordinal];
                    if (stage == 1) list->Dispatch(1u, 1u, 1u);
                    else if (stage == 0) DispatchCluster(list, cut.candidate_count);
                    else DispatchCluster(list, std::max(cut.candidate_count, cut.output_capacity));
                }
            });
        }
        auto emit = graph.AddPass(44u, "ClusterCutEmit", QueueHint::Compute);
        read_tables(emit);
        geometry_records = emit.ReadWrite(geometry_records, Access::UnorderedWrite);
        state.geometry_records = geometry_records;
        state.refs.version = emit.ReadWrite(state.refs.version, Access::UnorderedWrite);
        if (visibility_raster_records)
        {
            *visibility_raster_records = emit.ReadWrite(*visibility_raster_records, Access::UnorderedWrite);
            state.raster_records = *visibility_raster_records;
        }
        for (auto &cut : state.cuts)
        {
            if (!cut.active) continue;
            emit.Read(cut.counts.version, Access::UnorderedRead);
            emit.Read(cut.offsets.version, Access::UnorderedRead);
            for (auto *buffer : {&cut.status, &cut.draw_commands, &cut.mesh_commands, &cut.command_counts,
                &cut.history_bits, &cut.history_generations, &cut.selected_bounds})
                buffer->version = emit.ReadWrite(buffer->version, Access::UnorderedWrite);
        }
        emit.SetExecute([this, frame_slot, &state, bind](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            list->SetPipelineState(cluster_emit_pipeline.Get());
            for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
            {
                if (!state.cuts[ordinal].active) continue;
                bind(list, ordinal, 0);
                DispatchCluster(list, cluster_runtime->frames[frame_slot].cuts[ordinal].candidate_count);
            }
        });
        auto readback = graph.AddPass(50u, "ClusterDemandStatusReadback", QueueHint::Copy);
        for (auto &cut : state.cuts)
        {
            if (!cut.active) continue;
            readback.Read(cut.demand.version, Access::CopySource);
            readback.Read(cut.status.version, Access::CopySource);
            cut.readback.version = readback.Write(cut.readback.version, Access::CopyDest);
        }
        readback.SetExecute([this, frame_slot, &state](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            const auto demand_bytes = cluster_runtime->cpu.pages.size() * sizeof(std::uint32_t);
            for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
            {
                if (!state.cuts[ordinal].active) continue;
                const auto &cut = cluster_runtime->frames[frame_slot].cuts[ordinal];
                list->CopyBufferRegion(cut.readback.resource.Get(), 0, cut.demand.resource.Get(), 0, demand_bytes);
                list->CopyBufferRegion(cut.readback.resource.Get(), demand_bytes, cut.status.resource.Get(), 0, sizeof(GpuClusterCutStatus));
            }
        });
    }
    graph.SetFinalAccess(state.groups.version, Access::ComputeRead);
    graph.SetFinalAccess(state.children.version, Access::ComputeRead);
    graph.SetFinalAccess(state.clusters.version, Access::ComputeRead);
    graph.SetFinalAccess(state.pages.version, Access::ComputeRead);
    if (state.draws) graph.SetFinalAccess(state.refs.version, Access::ShaderRead);
    for (const auto &page : state.geometry)
        for (const auto &buffer : page.buffers) graph.SetFinalAccess(buffer.version, Access::ComputeRead);
    for (const auto &cut : state.cuts)
        if (cut.active)
        {
            for (const auto *buffer : {&cut.counts, &cut.offsets, &cut.block_sums, &cut.block_offsets,
                &cut.demand, &cut.status, &cut.draw_commands, &cut.mesh_commands, &cut.command_counts,
                &cut.selected_bounds, &cut.previous_rejected, &cut.retest_draw_commands, &cut.retest_mesh_commands})
                graph.SetFinalAccess(buffer->version, Access::UnorderedWrite);
            graph.SetFinalAccess(cut.readback.version, Access::CopyDest);
            graph.SetFinalAccess(cut.history_bits.version, Access::ComputeRead);
            graph.SetFinalAccess(cut.history_generations.version, Access::ComputeRead);
            graph.SetFinalAccess(cut.previous_bits.version, Access::ComputeRead);
            graph.SetFinalAccess(cut.previous_generations.version, Access::ComputeRead);
            if (cut.previous_frame_slot != UINT32_MAX) graph.SetFinalAccess(cut.previous_pages.version, Access::ComputeRead);
            graph.SetFinalAccess(cut.previous_status.version, Access::ComputeRead);
        }
    state.prepared = true;
    return Result::Success();
}

Result D3D12Renderer::Impl::AddClusterOcclusionPass(std::uint32_t frame_slot,
    ClusterFrameGraph &state, ResourceVersion hzb, bool retest)
{
    if (!cluster_runtime || !state.prepared || state.frame_slot != frame_slot ||
        frame_slot >= kClusterRuntimeFrames || !hzb.resource ||
        (retest ? !state.previous_occlusion || state.current_retest : state.previous_occlusion))
        return GraphFailure("Cluster occlusion has an invalid frame/HZB/phase input.");
    if (retest) state.current_retest = true;
    else state.previous_occlusion = true;
    if (!state.draws) return Result::Success();
    if (!cluster_root_signature || !(retest ? cluster_retest_pipeline : cluster_occlusion_pipeline) ||
        !hzb_history[hzb_read_index ^ (retest ? 1u : 0u)].resource)
        return GraphFailure("Cluster occlusion has no compatible pipeline/HZB resource.");

    auto pass = graph.AddPass(retest ? 53u : 52u,
        retest ? "ClusterCurrentDepthRetest" : "ClusterPreviousHzbOcclusion", QueueHint::Compute);
    pass.Read(hzb, Access::ComputeRead);
    pass.Read(state.refs.version, Access::UnorderedRead);
    pass.Read(state.geometry_records, Access::UnorderedRead);
    for (auto &cut : state.cuts)
    {
        if (!cut.active) continue;
        pass.Read(cut.status.version, Access::UnorderedRead);
        pass.Read(cut.selected_bounds.version, Access::UnorderedRead);
        if (retest)
        {
            pass.Read(cut.previous_rejected.version, Access::UnorderedRead);
            cut.retest_draw_commands.version = pass.Write(cut.retest_draw_commands.version, Access::UnorderedWrite);
            cut.retest_mesh_commands.version = pass.Write(cut.retest_mesh_commands.version, Access::UnorderedWrite);
            cut.command_counts.version = pass.ReadWrite(cut.command_counts.version, Access::UnorderedWrite);
            graph.SetFinalAccess(cut.retest_draw_commands.version, Access::UnorderedWrite);
            graph.SetFinalAccess(cut.retest_mesh_commands.version, Access::UnorderedWrite);
            graph.SetFinalAccess(cut.command_counts.version, Access::UnorderedWrite);
        }
        else
        {
            cut.previous_rejected.version = pass.Write(cut.previous_rejected.version, Access::UnorderedWrite);
            cut.draw_commands.version = pass.ReadWrite(cut.draw_commands.version, Access::UnorderedWrite);
            cut.mesh_commands.version = pass.ReadWrite(cut.mesh_commands.version, Access::UnorderedWrite);
            cut.command_counts.version = pass.ReadWrite(cut.command_counts.version, Access::UnorderedWrite);
            graph.SetFinalAccess(cut.previous_rejected.version, Access::UnorderedWrite);
            graph.SetFinalAccess(cut.draw_commands.version, Access::UnorderedWrite);
            graph.SetFinalAccess(cut.mesh_commands.version, Access::UnorderedWrite);
            graph.SetFinalAccess(cut.command_counts.version, Access::UnorderedWrite);
        }
    }
    pass.SetExecute([this, frame_slot, &state, retest](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        auto &runtime = *cluster_runtime;
        ID3D12DescriptorHeap *heap[]{runtime.descriptors.Get()};
        list->SetDescriptorHeaps(1, heap);
        list->SetComputeRootSignature(cluster_root_signature.Get());
        list->SetPipelineState(retest ? cluster_retest_pipeline.Get() : cluster_occlusion_pipeline.Get());
        list->SetComputeRootShaderResourceView(1, static_gpu_scene->instances.resource->GetGPUVirtualAddress());
        const auto phase = retest ? 1u : 0u;
        for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
        {
            if (!state.cuts[ordinal].active) continue;
            const auto &cut = runtime.frames[frame_slot].cuts[ordinal];
            list->SetComputeRootConstantBufferView(0, cut.constants.resource->GetGPUVirtualAddress());
            list->SetComputeRootConstantBufferView(3, cut.occlusion_constants.resource->GetGPUVirtualAddress() +
                phase * kClusterOcclusionConstantStride);
            auto handle = runtime.descriptors->GetGPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<UINT64>(((frame_slot * kClusterRuntimeCatalogs + ordinal) *
                kClusterOcclusionPhases + phase) * kClusterCutBankStride) * srv_stride;
            list->SetComputeRootDescriptorTable(2, handle);
            DispatchCluster(list, cut.output_capacity);
        }
    });
    return Result::Success();
}

void D3D12Renderer::Impl::ReadClusterFrame(PassBuilder &pass, const ClusterFrameGraph &state,
                                         bool draw, bool retest) const
{
    if (!state.prepared) return;
    const auto access = draw ? Access::ShaderRead : Access::ComputeRead;
    if (state.draws) pass.Read(state.refs.version, access);
    pass.Read(state.pages.version, Access::ComputeRead);
    for (const auto &page : state.geometry)
        if (page.resident)
            for (const auto &buffer : page.buffers) pass.Read(buffer.version, access);
    if (draw)
        for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
        {
            const auto &cut = state.cuts[ordinal];
            if (!cut.active) continue;
            const bool mesh = cluster_runtime->frames[state.frame_slot].cuts[ordinal].geometry_frontend_flags == kGeometryMeshEligible;
            pass.Read(retest ? (mesh ? cut.retest_mesh_commands.version : cut.retest_draw_commands.version)
                : (mesh ? cut.mesh_commands.version : cut.draw_commands.version), Access::IndirectArgs);
            pass.Read(cut.command_counts.version, Access::IndirectArgs);
        }
}

Result D3D12Renderer::Impl::DrawClusterGeometry(ID3D12GraphicsCommandList *list,
    std::uint32_t frame_slot, std::uint32_t view, bool retest)
{
    if (!list || !cluster_runtime || frame_slot >= kClusterRuntimeFrames || view > 3u || (retest && view != 0u))
        return GraphFailure("Cluster drawing has an invalid command list/frame/view.");
    auto &native = cluster_runtime->frames[frame_slot];
    const bool active = std::ranges::any_of(native.cuts, [](const auto &cut) { return cut.candidate_count != 0; });
    if (!active) return Result::Success();
    if (!static_gpu_scene || !native.refs.resource)
        return GraphFailure("Cluster drawing has no persistent static scene/shared refs.");
    list->SetGraphicsRootShaderResourceView(34, static_gpu_scene->instances.resource->GetGPUVirtualAddress());
    list->SetGraphicsRootShaderResourceView(35, native.refs.resource->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const bool visibility = config.surface_frontend == SurfaceFrontend::VisibilityBuffer;
    for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
    {
        const auto &cut = native.cuts[ordinal];
        if (!cut.candidate_count) continue;
        if (!ClusterHierarchyReady(ordinal))
            return GraphFailure("Unqualified dynamic geometry cannot enter the static hierarchy draw path.");
        list->SetGraphicsRoot32BitConstant(6, (material_parameter_bases[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)] << 1u) | 1u, 1);
        const bool mesh = cut.geometry_frontend_flags == kGeometryMeshEligible;
        ID3D12PipelineState *pipeline{};
        if (mesh)
        {
            auto *indexed = view ? environment_material_shadow_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)].Get()
                : visibility ? environment_material_visibility_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)].Get()
                : environment_material_scene_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)].Get();
            for (const auto &pair : mesh_pipeline_pairs)
                if (pair.indexed == indexed) { pipeline = pair.mesh.Get(); break; }
        }
        else pipeline = view ? cluster_shadow_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)].Get()
            : visibility ? cluster_visibility_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)].Get() : cluster_scene_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)].Get();
        auto *signature = mesh ? cluster_mesh_signature.Get() : cluster_draw_signature.Get();
        if (!pipeline || !signature)
            return GraphFailure("Cluster material/frontend has no compatible pipeline/indirect signature.");
        list->SetPipelineState(pipeline);
        auto *arguments = retest ? (mesh ? cut.retest_mesh_commands.resource.Get() : cut.retest_draw_commands.resource.Get())
            : (mesh ? cut.mesh_commands.resource.Get() : cut.draw_commands.resource.Get());
        const auto stride = mesh ? 16ull : 24ull;
        const auto offset = retest ? 0ull : static_cast<UINT64>(view) * cut.output_capacity * stride;
        list->ExecuteIndirect(signature, cut.output_capacity, arguments, offset,
            cut.command_counts.resource.Get(), retest ? 16ull : static_cast<UINT64>(view) * sizeof(std::uint32_t));
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CommitClusterFrameGraph(std::uint32_t frame_slot,
    ClusterFrameGraph &state, const CompletionSet &completion)
{
    if (!cluster_runtime || !state.prepared || state.frame_slot != frame_slot ||
        (!completion.direct && !completion.compute && !completion.copy))
        return GraphFailure("Cluster graph commit has an invalid frame/completion.");
    for (const auto &entry : state.imports)
    {
        entry.native->graph_access = graph.FinalAccess(entry.handle);
        entry.native->graph_queue = graph.FinalQueue(entry.handle);
        entry.native->graph_completion = graph.ResourceCompletion(entry.handle);
    }
    for (const auto &page : state.geometry)
        if (page.upload)
        {
            CompletionSet uploaded;
            for (const auto &buffer : page.buffers) uploaded.Merge(graph.ResourceCompletion(buffer.handle));
            if (auto result = SubmitClusterPageUpload(page.logical_slot, uploaded); !result) return result;
        }
    for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
        if (state.cuts[ordinal].active)
        {
            if (auto result = SubmitClusterCutReadback(frame_slot, ordinal,
                graph.ResourceCompletion(state.cuts[ordinal].readback.handle)); !result) return result;
            const auto &cut = state.cuts[ordinal];
            if (cut.previous_frame_slot != UINT32_MAX)
            {
                CompletionSet readers;
                for (const auto *buffer : {&cut.previous_bits, &cut.previous_generations, &cut.previous_pages, &cut.previous_status})
                    readers.Merge(graph.ResourceCompletion(buffer->handle));
                // WaitForFrame and PrepareClusterFrame must include later GPU
                // readers before this old frame slot can be rewritten/grown.
                auto &previous = cluster_runtime->frames[cut.previous_frame_slot];
                previous.completion.Merge(readers);
                previous.cuts[ordinal].completion.Merge(readers);
                frames[cut.previous_frame_slot].completion.Merge(readers);
            }
        }
    MarkClusterFrameUse(frame_slot, completion);
    cluster_runtime->frames[frame_slot].history_frame_id = frame_number + 1u;
    return Result::Success();
}
} // namespace hs
