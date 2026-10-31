#include "renderer_impl.hpp"
#include <hs/render_data/static_shadow.hpp>
#include "renderer_quality.hpp"
#include "renderer_instance_order.hpp"
#include "renderer_monster_lod.hpp"
#include "renderer_grass_benders.hpp"
#include "visibility_encoding.hpp"
#include "renderer_caster_storage.hpp"
#include "vfx_ribbon_resources.hpp"
#include <DirectXPackedVector.h>
#include "renderer_post_subgraphs.hpp"
#include "renderer_named_animation.hpp"
#include "renderer_brixelizer_scene.hpp"
#include "renderer_entity_surface.hpp"
#include "hero_coverage.hpp"
#include "renderer_vfx_compatibility.hpp"
#include "renderer_vfx_candidates.hpp"
#include <DirectXPackedVector.h>
#include <array>
#include <bit>
#include <cassert>
#include <cfloat>
#include <cstddef>
#include <exception>
#include <limits>
#include <iterator>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <tuple>

#include <numbers>
#include <optional>
#include <ranges>
#include <utility>
#include <unordered_map>
#include <unordered_set>

namespace hs
{

namespace
{
struct ExposureFrameRecordData
{
    std::array<std::move_only_function<void(RenderPassContext &)>, 4> record;
};

Result BuildExposureGraph(RenderGraphBuilder &graph, ResourceVersion composite_hdr,
                          ResourceVersion exposure_read, ResourceVersion scene_work,
                          ResourceVersion histogram, ResourceVersion exposure_write,
                          ExposureFrameRecordData records, ExposureGraphLayout &layout,
                          bool restore)
{
    constexpr std::array ids{33u, 34u, 35u, 36u};
    constexpr std::array names{"PreExposure", "ExposureHistogramClear",
                               "ExposureHistogramBuild", "ExposureReduce"};
    constexpr std::array queues{QueueHint::Direct, QueueHint::Compute,
                                QueueHint::Compute, QueueHint::Compute};
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        if (restore)
        {
            auto pass = graph.BindPass(layout.pass_indices[index], ids[index]);
            if (!pass)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Exposure pass does not match the retained graph plan.");
            pass->SetExecute(std::move(records.record[index]));
            continue;
        }
        layout.pass_indices[index] = graph.PassCount();
        auto pass = graph.AddPass(ids[index], names[index], queues[index]);
        switch (index)
        {
        case 0:
            pass.Read(composite_hdr, Access::ShaderRead);
            pass.Read(exposure_read, Access::ShaderRead);
            scene_work = pass.Write(scene_work, Access::RenderTarget);
            break;
        case 1:
            histogram = pass.Write(histogram, Access::UnorderedWrite);
            break;
        case 2:
            pass.Read(scene_work, Access::ComputeRead);
            pass.Read(exposure_read, Access::ComputeRead);
            histogram = pass.ReadWrite(histogram, Access::UnorderedWrite);
            break;
        case 3:
            pass.Read(exposure_read, Access::ComputeRead);
            histogram = pass.ReadWrite(histogram, Access::UnorderedWrite);
            exposure_write = pass.Write(exposure_write, Access::UnorderedWrite);
            break;
        }
        pass.SetExecute(std::move(records.record[index]));
    }
    if (!restore)
    {
        layout.scene_work = scene_work;
        layout.histogram = histogram;
        layout.exposure_write = exposure_write;
    }
    return Result::Success();
}

Result BuildFinalGraph(RenderGraphBuilder &graph, ResourceVersion back_buffer,
                       bool output_dependency_interval, bool output_ready, bool presentation,
                       FinalGraphLayout &layout, bool restore)
{
    assert(!presentation || output_ready);
    if (restore)
    {
        if (layout.output_dependency_interval != output_dependency_interval || layout.output_ready != output_ready ||
            layout.presentation != presentation ||
            layout.back_buffer != back_buffer)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Final pass does not match the retained graph plan.");
        for (std::uint32_t index = 0; index < (output_dependency_interval ? 2u : output_ready ? 1u : 0u); ++index)
        {
            auto pass = graph.BindPass(layout.pass_indices[index], output_dependency_interval ? 37u + index : 38u);
            if (!pass)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Final pass binding is missing from the retained graph plan.");
            pass->SetExecute([](RenderPassContext &) {});
        }
        return Result::Success();
    }
    layout.output_dependency_interval = output_dependency_interval;
    layout.output_ready = output_ready;
    layout.presentation = presentation;
    layout.back_buffer = back_buffer;
    if (output_dependency_interval)
    {
        layout.pass_indices[0] = graph.PassCount();
        auto begin = graph.AddPass(37, "OutputDependencyIntervalBegin", QueueHint::Direct);
        begin.SetExecute([](RenderPassContext &) {});
        layout.pass_indices[1] = graph.PassCount();
        auto end = graph.AddPass(38, "OutputDependencyIntervalEnd", QueueHint::Direct);
        end.Read(back_buffer, Access::Present);
        end.SetExecute([](RenderPassContext &) {});
        for (std::uint32_t pass = 0; pass < graph.PassCount(); ++pass)
        {
            const auto id = graph.DeclaredPassId(pass);
            if (id == 37 || id == 38) continue;
            graph.AddOrderingDependency(37, id);
        }
        graph.AddOrderingDependency(37, 38);
        // This joins output ancestors through the back-buffer Present read;
        // unrelated Copy work is deliberately not an end dependency.
        if (presentation) graph.AddOrderingDependency(904, 38);
    }
    else if (output_ready)
    {
        layout.pass_indices[0] = graph.PassCount();
        auto ready = graph.AddPass(38, "SourceRenderOutputReady", QueueHint::Direct);
        ready.Read(back_buffer, Access::Present);
        ready.SetExecute([](RenderPassContext &) {});
        // Tagging reads only guide producers; unrelated streaming/readback stays separate.
        if (presentation) graph.AddOrderingDependency(904, 38);
    }
    return Result::Success();
}
}


Result D3D12Renderer::RenderView(const RenderSnapshotPair &snapshots,
                             std::span<const ParticleSpawnCommand> particle_spawns,
                             std::span<const VfxLineSpawnCommand> effect_lines,
                             std::uint32_t typed_vfx_event_count,
                             std::uint32_t typed_vfx_persistent_count,
                             std::span<const VfxGroundSpawnInput> typed_ground_spawns,
                             std::span<const VfxFlashSpawnInput> typed_flash_spawns,
                             std::span<const VfxOwnedMeshInput> typed_mesh_spawns,
                             std::span<const VfxFresnelInput> typed_fresnels,
                             const VfxRibbonFrameInput &typed_ribbons,
                             std::span<const VfxLightInput> typed_lights,
                             std::span<const VfxDistortionInput> typed_distortions,
                             std::span<const VfxDecalInput> typed_decals,
                             const DevToolsFrameData &devtools,
                             RendererFrameResult &frame_result,
                             VfxVolumeInputs typed_volumes, VfxScreenInputs typed_screen,
                             VfxEntitySurfaceInputs typed_entity_surfaces)
{
    RendererCpuTimings cpu_timings;
#if defined(HS_DEVELOPMENT_TOOLS)
    auto cpu_stage_start = std::chrono::steady_clock::now();
    const auto measure_stage = [&] {
        const auto end = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - cpu_stage_start).count();
        cpu_stage_start = end;
        return static_cast<std::uint64_t>(elapsed);
    };
    const auto capture_clock = [&] {
        return impl_->config.capture_timing_stream ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
    };
    const auto capture_elapsed = [&](std::chrono::steady_clock::time_point begin) -> std::uint64_t {
        if (!impl_->config.capture_timing_stream) return 0;
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count());
    };
#else
    const auto measure_stage = [] { return std::uint64_t{}; };
    const auto capture_clock = [] { return std::chrono::steady_clock::time_point{}; };
    const auto capture_elapsed = [](std::chrono::steady_clock::time_point) { return std::uint64_t{}; };
#endif
    const bool presented_view = impl_->active_view_role == RenderViewRole::Main;
    const auto &view_input = *impl_->explicit_view;
    const auto near_plane = view_input.near_plane;
    const auto far_plane = view_input.far_plane;
    static_cast<void>(effect_lines);
    if (impl_->frame_submission_faulted)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "A partial frame submission requires renderer reinitialization.");
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }
    auto &owned_view = *impl_->owned_views.at(impl_->active_view_id.value);
    const bool wait_for_streaming = impl_->explicit_frame && impl_->explicit_frame->wait_for_streaming;
    struct GrassHistoryAttempt
    {
        DirectX::XMFLOAT4 &state;
        bool succeeded{};
        ~GrassHistoryAttempt() { if (!succeeded) state.z = 0.0f; }
    } grass_history_attempt{owned_view.previous_grass_state};
    impl_->asset_io.SetWaitForReads(wait_for_streaming);
    if (wait_for_streaming)
        if (auto waited = impl_->WaitForGpu(); !waited) return waited;
    const auto output_index = presented_view ? impl_->swap_chain->GetCurrentBackBufferIndex()
        : static_cast<std::uint32_t>(owned_view.frame_number % kFrameCount);
    const auto frame_slot = static_cast<std::uint32_t>(impl_->rendered_submission_count % kFrameCount);
    const auto frame_output = presented_view ? impl_->back_buffers[output_index] : owned_view.outputs[output_index];
    auto &output_completion = presented_view ? impl_->back_buffer_completion[output_index]
                                               : owned_view.output_completion[output_index];
    auto &back_buffer_contract = presented_view ? impl_->back_buffer_contracts[output_index]
                                                  : owned_view.output_contracts[output_index];
    auto &back_buffer_contract_device = presented_view ? impl_->back_buffer_contract_devices[output_index]
                                                        : owned_view.output_contract_devices[output_index];
    auto &back_buffer_contract_generation = presented_view ? impl_->back_buffer_contract_generations[output_index]
                                                            : owned_view.output_contract_generations[output_index];
    auto &frame = impl_->frames[frame_slot];
    auto &visibility_frame = impl_->visibility_frames[frame_slot];
    const bool use_visibility =
        impl_->config.surface_frontend == SurfaceFrontend::VisibilityBuffer;
    constexpr UINT kDefaultSceneDrawMaterial = 0u;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.capture_timing_stream)
        cpu_timings.frame_slot_wait_begin_qpc = RendererDiagnosticsCounter();
#endif
    if (auto result = impl_->WaitForFrame(frame); !result)
    {
        return result;
    }
    if (frame.auto_fg_cadence_pending)
    {
        const auto sequence = frame.auto_fg_cadence_sequence;
        impl_->auto_fg_cadence_samples[sequence % impl_->auto_fg_cadence_samples.size()] = {
            sequence, frame.submitted_frame_id, frame.auto_fg_cadence_submit_qpc,
            frame.auto_fg_cadence_qpc_frequency, frame.auto_fg_cadence_validation,
            frame.auto_fg_cadence_gpu_validation};
        impl_->auto_fg_cadence_completed_sequence =
            std::max(impl_->auto_fg_cadence_completed_sequence, sequence);
        frame.auto_fg_cadence_pending = false;
    }
    impl_->WriteOptionTargetDescriptors(owned_view, frame_slot);
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.capture_timing_stream)
        cpu_timings.frame_slot_wait_end_qpc = RendererDiagnosticsCounter();
#endif
    if (presented_view && impl_->sdk_presentation.LatencyOwner() != SdkLatencyOwner::None && impl_->native_input_phase != 3)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Native latency rendering requires acknowledged input sampling and simulation for this frame token.");
    if (presented_view && impl_->sdk_presentation.IsEnabled())
    {
        std::vector<ID3D12CommandQueue *> queues;
        for (const auto hint : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
            if (auto *native = impl_->graph_queue_executor.NativeQueue(hint)) queues.push_back(native);
        if (auto reusable = impl_->sdk_presentation.BeforeReuse(queues); !reusable) return reusable;
    }
    cpu_timings.wait_us = measure_stage();
    const auto cpu_recording_start = std::chrono::steady_clock::now();
    if (auto completed = impl_->ReadFrameGpuTimings(frame); completed.valid)
    {
#if defined(HS_DEVELOPMENT_TOOLS)
        impl_->diagnostics.RecordCompleted(completed);
#endif
        if (completed.view_id == impl_->active_view_id.value)
        {
            if (completed.completed_frame_id > owned_view.completed_gpu_timings.completed_frame_id)
                owned_view.completed_gpu_timings = std::move(completed);
        }
        else if (const auto found = impl_->owned_views.find(completed.view_id); found != impl_->owned_views.end())
        {
            if (completed.completed_frame_id > found->second->completed_gpu_timings.completed_frame_id)
                found->second->completed_gpu_timings = std::move(completed);
        }
    }
    frame.constants_submission_id = 0;
    owned_view.state.view_id = impl_->active_view_id.value;
    owned_view.state.metric_generation = impl_->view_configuration_revision;
    const auto active_features = impl_->ResolveFeatures(impl_->config);
    const bool dynamic_resolution_active = EffectiveDynamicResolution(
        impl_->config.dynamic_resolution, active_features);
    if (!owned_view.state.quality_generation ||
        owned_view.timing_last_active != owned_view.state.render_active)
    {
        ++owned_view.state.quality_generation;
        owned_view.timing_last_active = owned_view.state.render_active;
    }
    if (!owned_view.state.provider_generation ||
        owned_view.timing_last_provider != owned_view.reconstruction_provider.get())
    {
        ++owned_view.state.provider_generation;
        owned_view.timing_last_provider = owned_view.reconstruction_provider.get();
    }
    if (const auto &timing = owned_view.completed_gpu_timings;
        timing.completed_frame_id > owned_view.dynamic_resolution_sample_id)
    {
        owned_view.dynamic_resolution_sample_id = timing.completed_frame_id;
        const DynamicResolutionConfig resolution_config{
            dynamic_resolution_active,
            static_cast<double>(impl_->config.target_render_fps),
            impl_->config.dynamic_min_percent, impl_->config.dynamic_max_percent};
        const bool current_timing = timing.main_view && timing.view_id == impl_->active_view_id.value &&
            timing.metric_generation == impl_->view_configuration_revision &&
            timing.resize_epoch == owned_view.state.resize_epoch &&
            timing.render_active == owned_view.state.render_active;
        DynamicResolutionSample sample;
        if (timing.metric_kind == GpuFrameTimingKind::SourceOutputReady)
        {
            sample.completed = current_timing && timing.fence_completed;
            sample.generated = timing.generated_frame;
            sample.metric_kind = DynamicResolutionMetricKind::SourceOutputReady;
            sample.view_id = timing.view_id;
            sample.source_frame_id = timing.source_frame_id;
            sample.metric_generation = timing.metric_generation;
            sample.quality_generation = timing.quality_generation;
            sample.provider_generation = timing.provider_generation;
            sample.render_active = timing.render_active;
            sample.calibration_valid = timing.calibration_valid;
            sample.calibration_drift = timing.calibration_drift;
            sample.cadence_valid = timing.cadence_valid;
            sample.span_valid = timing.span_valid;
            sample.gpu_backlog_proven = timing.gpu_backlog_proven;
            sample.cadence_gap = timing.cadence_gap;
            sample.closure_span_gap = timing.closure_span_gap;
            sample.source_ready_cadence_ms = timing.source_ready_cadence_ms;
            sample.output_dependency_span_ms = timing.output_dependency_span_ms;
        }
        else
            sample = {current_timing && timing.critical_path_valid, timing.generated_frame,
                timing.gpu_limited, timing.critical_path_ms,
                timing.metric_kind == GpuFrameTimingKind::OutputDependencyInterval
                    ? DynamicResolutionMetricKind::OutputDependencyInterval
                    : DynamicResolutionMetricKind::Unavailable};
        auto selected = owned_view.dynamic_resolution_controller.Select(
            owned_view.state, resolution_config, sample);
        if (owned_view.reconstruction_provider && owned_view.reconstruction_provider->Initialized())
        {
            const auto &plan = owned_view.reconstruction_plan;
            selected.active_extent.width = std::clamp(selected.active_extent.width,
                plan.input_minimum.width, plan.input_maximum.width);
            selected.active_extent.height = std::clamp(selected.active_extent.height,
                plan.input_minimum.height, plan.input_maximum.height);
            selected.changed = selected.active_extent != owned_view.state.render_active;
        }
        if (selected.changed)
        {
            (void)UpdateViewState(owned_view.state, owned_view.state.output_extent,
                                  owned_view.state.render_capacity, selected.active_extent);
            ++owned_view.state.quality_generation;
            owned_view.timing_last_active = selected.active_extent;
            owned_view.render_width = selected.active_extent.width;
            owned_view.render_height = selected.active_extent.height;
            owned_view.targets.hzb_layout = MakeHzbLayout(owned_view.render_width, owned_view.render_height);
            owned_view.hzb_history_valid = false;
        }
    }
    const auto completed_streaming = impl_->graph_queue_executor.Completed();
    if (auto result = impl_->UpdateResidencyBudget(); !result) return result;
    if (auto result = impl_->asset_io.CollectCancelled(impl_->streaming, impl_->residency); !result) return result;
    if (auto result = impl_->texture_feedback.Consume(frame_slot, completed_streaming,
            owned_view.frame_number, impl_->texture_streaming); !result) return result;
    if (auto result = impl_->PumpEnvironmentGeometryStreaming(); !result)
    {
        return result;
    }
    if (auto result = impl_->PumpHlodDemand(); !result) return result;
    if (auto result = impl_->PumpClusterStreaming(frame_slot); !result) return result;
    if (auto result = impl_->CaptureClusterFrame(frame_slot); !result) return result;
    if (auto result = impl_->BeginGeometryDrawRecords(frame_slot); !result)
        return result;
    impl_->WriteGeometryPageDescriptors(frame_slot);
    frame.timestamps_recorded = false;
    struct PendingScenePreparation
    {
        PersistentGpuSceneSlots &slots;
        SkinCache &skin;
        ~PendingScenePreparation()
        {
            slots.Abort();
            skin.Abort();
        }
    } pending_scene{impl_->dynamic_scene_slots,
                    owned_view.skin_history};
    if (!impl_->static_scene_fence_unreliable)
    {
        const auto completed = impl_->graph_queue_executor.Completed();
        std::erase_if(impl_->retired_static_gpu_scenes, [&](const auto &scene) {
            if (!scene->last_completion.IsSatisfiedBy(completed) ||
                (scene->refs && !scene->refs->last_completion.IsSatisfiedBy(completed)))
                return false;
            return std::ranges::all_of(scene->retired_refs, [&](const auto &refs) {
                return refs->last_completion.IsSatisfiedBy(completed);
            });
        });
        if (impl_->static_gpu_scene && impl_->static_gpu_scene->submitted &&
            impl_->static_gpu_scene->upload.resource &&
            impl_->static_gpu_scene->upload_completion.IsSatisfiedBy(completed))
            impl_->static_gpu_scene->upload.Reset();
        if (impl_->static_gpu_scene)
        {
            std::erase_if(impl_->static_gpu_scene->retired_refs, [&](const auto &refs) {
                return refs->last_completion.IsSatisfiedBy(completed);
            });
            auto &refs = impl_->static_gpu_scene->refs;
            if (refs && refs->submitted && refs->upload.resource &&
                refs->upload_completion.IsSatisfiedBy(completed)) refs->upload.Reset();
        }
    }
    auto &transient_pool = owned_view.targets.transient_pools[frame_slot];
    transient_pool.ResetCompletedClaims(impl_->graph_queue_executor.Completed());

    cpu_timings.streaming_us = measure_stage();
    auto snapshot = snapshots.has_current ? snapshots.current : RenderSnapshot{};
    const auto &world_batch = impl_->explicit_frame->vfx.world_batch;
    const Tick stateful_target = world_batch ? world_batch->stateful_target : snapshot.header.tick;
    const bool world_step_requested = !world_batch || world_batch->sequence != 0;
    const double native_display_tick = world_batch
        ? static_cast<double>(snapshot.header.tick) -
            (snapshots.has_previous && impl_->render_package_alpha < 1.0f
                ? 1.0 - impl_->render_package_alpha : 0.0)
        : static_cast<double>(snapshot.header.tick);
    struct WorldStepReceipt
    {
        RendererFrameResult &output;
        Tick tick;
        std::uint64_t session;
        std::uint64_t sequence;
        bool submitted{};
        bool history_committed{};
        std::uint64_t presented_ui_generation{};
        ~WorldStepReceipt()
        {
            output.render_history_committed = history_committed;
            output.presented_ui_packet_generation = presented_ui_generation;
            if (!submitted) return;
            output.vfx_world_step_submitted = true;
            output.vfx_world_tick = tick;
            output.vfx_world_session_id = session;
            output.vfx_world_batch_sequence = sequence;
        }
    } world_step_receipt{frame_result, stateful_target, snapshot.header.session_id,
        world_batch ? world_batch->sequence : 0};
    snapshot.camera.vertical_fov_degrees = view_input.camera.vertical_fov_degrees;
    const bool scene_new_session = snapshot.header.session_id != impl_->render_session_id;
    const bool new_session = scene_new_session || snapshot.header.session_id != owned_view.view_session_id;
    const auto *last_sample = owned_view.previous_instances.get();
    const bool clock_discontinuity = last_sample &&
        (last_sample->clock_epoch != snapshot.header.clock.epoch ||
         last_sample->clock_mode != snapshot.header.clock.mode ||
         snapshot.header.simulation_time < last_sample->simulation_time);
    const bool motion_history_reset = new_session || clock_discontinuity;
    if (motion_history_reset)
    {
        if (scene_new_session) impl_->particles_initialized = false;
        owned_view.temporal_history_valid = false;
        owned_view.hzb_history_valid = false;
        owned_view.previous_origins_valid = false;
    }
    // A rewound simulation clock starts a fresh shared particle schedule/pool.
    if (impl_->render_scene_inputs &&
        ((world_batch && world_batch->reset_particle_state) ||
         (impl_->particles_initialized && stateful_target < impl_->last_particle_tick)))
        impl_->particles_initialized = false;
    auto &prepared_scene = impl_->prepared_scene_package;
    const bool reconcile_packet_scene = impl_->render_scene_inputs || !prepared_scene.gpu_slots_ready;
    auto &pose_by_id = prepared_scene.poses_by_id;
    auto &pose_by_source = prepared_scene.poses_by_source;
    auto &entity_surface_frame = prepared_scene.entity_surfaces;
    const auto now = impl_->render_package_time;
    const auto interpolation = impl_->render_package_alpha;
    const auto is_monster = [](RenderMesh mesh) {
        return mesh >= RenderMesh::MonsterMelee && mesh <= RenderMesh::BossFinal;
    };
    constexpr std::uint32_t kCachedSkinFlag = 0x40000u;
    constexpr std::uint32_t kCachedSkinHistoryFlag = 0x80000u;
    if (!prepared_scene.ready)
    {
        auto &prepared_instances = prepared_scene.instances;
        pose_by_source.resize(snapshot.instances.size());
        pose_by_id.reserve(snapshot.poses.size());
        for (const auto &pose : snapshot.poses)
            if (pose.instance_index < snapshot.instances.size())
            {
                pose_by_source[pose.instance_index] = &pose;
                const auto stable_id = snapshot.instances[pose.instance_index].stable_id;
                if (stable_id != 0) pose_by_id.insert_or_assign(stable_id, &pose);
            }
        prepared_instances.assign(snapshot.instances.begin(), snapshot.instances.end());
        const auto retained_vfx_start = capture_clock();
        for (const auto &command : typed_entity_surfaces)
        {
            if (!command.has_retained_instance || std::ranges::any_of(prepared_instances,
                    [&](const auto &instance) { return instance.stable_id == command.render_instance_id; })) continue;
            const auto &instance = command.retained_instance;
            if (instance.stable_id != command.render_instance_id ||
                instance.mesh < RenderMesh::BossFiveMinute || instance.mesh > RenderMesh::BossFinal ||
                instance.asset_id.value == 0 || instance.material_id.value == 0 ||
                (instance.animation_clip_id.value == 0 && !command.has_retained_animation_pose))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                    "Entity surface retention requires its actual boss mesh and captured pose.");
            prepared_instances.push_back(instance);
            if (command.has_retained_animation_pose)
                pose_by_id.insert_or_assign(instance.stable_id, &command.retained_animation_pose);
        }
        std::vector<VfxEntitySurfaceCommand> admitted_entity_surfaces;
        for (const auto &command : typed_entity_surfaces)
            if (std::ranges::any_of(prepared_instances, [&](const auto &instance) {
                    return instance.stable_id == command.render_instance_id &&
                        instance.mesh >= RenderMesh::BossFiveMinute && instance.mesh <= RenderMesh::BossFinal;
                })) admitted_entity_surfaces.push_back(command);
        if (auto prepared = entity_surface_frame.Build(admitted_entity_surfaces); !prepared) return prepared;
        if (!entity_surface_frame.Bytes().empty() &&
            (impl_->material_surface_layouts[kAuthoredMaterialNames.size() + 6].varying_mask & 16u) == 0)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Boss surface effects require the actual monster emission sidecar.");
        cpu_timings.retained_vfx_us = capture_elapsed(retained_vfx_start);
        if (impl_->config.slime_family_preview_count == 0 &&
            (!typed_ground_spawns.empty() || !typed_mesh_spawns.empty()))
            FilterRenderInstancesForOwnedVfx(prepared_instances, typed_ground_spawns,
                typed_mesh_spawns, prepared_scene.vfx_replacements);
        if (const auto count = impl_->config.slime_family_preview_count; count != 0)
        {
            prepared_instances.clear();
            const auto columns = count == 1 ? 3u : static_cast<std::uint32_t>(std::ceil(std::sqrt(count * 3.0f)));
            for (std::uint32_t i = 0; i < count * 3; ++i)
            {
                const auto row = i / columns, column = i % columns;
                const auto x = count == 1 ? (static_cast<float>(i) - 1.0f) * 2.5f
                                          : (static_cast<float>(column) - (columns - 1) * 0.5f) * 1.1f;
                const auto rows = (count * 3 + columns - 1) / columns;
                const auto z = count == 1 ? 0.0f : (static_cast<float>(row) - (rows - 1) * 0.5f) * 1.1f;
                const auto kind = i % 3;
                const auto scale = kind == 0 ? 1.25f : kind == 1 ? 1.40625f : 1.5625f;
                RenderInstance preview{};
                preview.position = {x, 0.0f, z};
                preview.scale = {scale, scale, scale};
                preview.color_rgba = 0xffffffffu;
                preview.mesh = static_cast<RenderMesh>(10 + kind);
                preview.stable_id = 0x53504c4d00000000ull | static_cast<std::uint64_t>(i + 1);
                prepared_instances.push_back(preview);
                if (kind == 2)
                {
                    RenderInstance projectile{};
                    projectile.position = {x - 1.25f, 0.45f, z - 1.5f};
                    projectile.scale = {.24f, .24f, .8f};
                    projectile.color_rgba = 0xff60d060u;
                    projectile.mesh = RenderMesh::EnemyProjectile;
                    projectile.stable_id =
                        0x53504c4d00100000ull | static_cast<std::uint64_t>(i + 1);
                    prepared_instances.push_back(projectile);
                }
            }
        }
        if (impl_->config.monster_preview_asset < 6 && impl_->config.slime_family_preview_count == 0)
        {
            const auto preview_mesh = static_cast<RenderMesh>(
                static_cast<std::uint32_t>(RenderMesh::MonsterMelee) +
                impl_->config.monster_preview_asset);
            const auto found = std::ranges::find_if(
                prepared_instances, [&](const auto &source) { return source.mesh == preview_mesh; });
            if (found != prepared_instances.end())
            {
                auto preview = *found;
                preview.position = {};
                preview.yaw = 0.0f;
                prepared_instances.assign(1, preview);
            }
            else
            {
                prepared_instances.clear();
            }
        }
        if (impl_->config.environment_preview)
        {
            prepared_instances.clear();
            const auto add = [&](RenderMesh mesh, Float3 position, Float3 scale, std::uint32_t variant) {
                RenderInstance instance{};
                instance.mesh = mesh;
                instance.position = position;
                instance.scale = scale;
                instance.environment_variant = variant;
                instance.environment_seed = 0x4853454eu;
                prepared_instances.push_back(instance);
            };
            for (std::uint32_t variant = 0; variant < 3; ++variant)
            {
                const float x = static_cast<float>(variant)*5.0f-5.0f;
                add(RenderMesh::TreeTrunk, {x,0,0}, {1,1,1}, variant);
                add(RenderMesh::TreeCanopy, {x,0,0}, {1,1,1}, variant);
            }
            for (std::uint32_t variant = 0; variant < 4; ++variant)
            {
                const float x = static_cast<float>(variant)*3.0f-4.5f;
                add(RenderMesh::Rock, {x,0,-5}, {.8f,.8f,.8f}, variant);
                add(RenderMesh::Grass, {x,0,-8}, {1,1,1}, variant);
            }
            for (int z=-5; z<=2; ++z)
                for (int x=-5; x<=5; ++x)
                    add(RenderMesh::Ground, {static_cast<float>(x)*2.5f,-.055f,static_cast<float>(z)*2.5f},
                        {2.5f,.1f,2.5f}, 0);
        }
        for (auto &source : prepared_instances) PopulateRenderInstanceIdentity(source);
        if (!typed_ribbons.sources.empty() && !snapshot.persistent_vfx.empty())
        {
            for (const auto &source : typed_ribbons.sources)
                prepared_scene.ribbon_head_ids.insert(source.owner_id);
            for (const auto &visual : snapshot.persistent_vfx)
                if (visual.kind == PersistentVfxKind::ProjectileHead &&
                    prepared_scene.ribbon_head_ids.contains(visual.stable_id))
                    prepared_scene.ribbon_projectile_ids.insert(visual.projectile_owner_id);
        }
        prepared_scene.ready = true;
    }
    if (impl_->config.environment_preview)
    {
        snapshot.camera.target = {0,1,0};
        if (!impl_->config.preview_camera_override)
        {
            snapshot.camera.yaw_degrees = 0.0f;
            snapshot.camera.pitch_degrees = 22.0f;
            snapshot.camera.distance = 22.0f;
        }
    }
    const auto &render_instances = prepared_scene.ordered_instances;
    const auto family_preview = impl_->config.slime_family_preview_count != 0;
#if defined(HS_DEVELOPMENT_TOOLS)
    const auto monster_preview = impl_->config.monster_preview_asset < 6 || family_preview;
    const auto camera_yaw = impl_->config.preview_camera_override
                                ? impl_->config.preview_camera_yaw
                            : impl_->config.character_preview ? impl_->preview_yaw
                                                              : snapshot.camera.yaw_degrees;
    const auto camera_distance = impl_->config.preview_camera_override
                                     ? impl_->config.preview_camera_distance
                                 : family_preview ? std::max(10.0f, std::sqrt(impl_->config.slime_family_preview_count * 3.0f) * 1.9f)
                                 : monster_preview
                                     ? (impl_->config.monster_preview_asset >= 3 ? 18.0f : 8.0f)
                                 : impl_->config.character_preview ? impl_->preview_distance
                                     : snapshot.camera.distance;
#else
    const auto camera_yaw = impl_->config.preview_camera_override
                                ? impl_->config.preview_camera_yaw
                                : snapshot.camera.yaw_degrees;
    const auto camera_distance = impl_->config.preview_camera_override
                                     ? impl_->config.preview_camera_distance
                                     : family_preview ? std::max(10.0f, std::sqrt(impl_->config.slime_family_preview_count * 3.0f) * 1.9f)
                                     : snapshot.camera.distance;
#endif
    const auto scene_local = [&](RenderWorldPosition p) {
        return DirectX::XMVectorSet(static_cast<float>(p.x - view_input.scene_origin.x),
            static_cast<float>(p.y - view_input.scene_origin.y),
            static_cast<float>(p.z - view_input.scene_origin.z), 1);
    };
    const auto eye = scene_local(view_input.camera.position);
    const auto target = scene_local(view_input.camera.target);
    const auto direction = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(target, eye));
    const auto camera_up = DirectX::XMVectorSet(view_input.camera.up.x, view_input.camera.up.y, view_input.camera.up.z, 0);
    DirectX::XMFLOAT3 lod_eye{};
    DirectX::XMStoreFloat3(&lod_eye, eye);
    const auto ribbon_prepare_start = capture_clock();
    const auto ribbon_thread_cpu = [](std::uint64_t &ticks) noexcept {
        FILETIME created{}, exited{}, kernel{}, user{};
        const auto previous_error = GetLastError();
        const bool available = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != FALSE;
        SetLastError(previous_error);
        if (!available) return false;
        ticks = (static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                (static_cast<std::uint64_t>(user.dwHighDateTime) << 32 | user.dwLowDateTime);
        return true;
    };
    std::uint64_t ribbon_cpu_begin{};
    const bool ribbon_cpu_available = impl_->config.capture_timing_stream && ribbon_thread_cpu(ribbon_cpu_begin);
    const auto ribbon_state_start = capture_clock();
    const Float3 ribbon_eye{lod_eye.x, lod_eye.y, lod_eye.z};
    if (scene_new_session || impl_->rendered_submission_count == 0) impl_->previous_ribbon_eye = ribbon_eye;
    const auto ribbon_sources = impl_->config.slime_family_preview_count == 0
        ? typed_ribbons.sources : std::span<const VfxRibbonSourceInput>{};
    const std::array<std::uint64_t, 3> ribbon_buffer_widths{
        impl_->ribbon_history.resource ? impl_->ribbon_history.resource->GetDesc().Width : 0,
        impl_->ribbon_previous.resource ? impl_->ribbon_previous.resource->GetDesc().Width : 0,
        frame.ribbon_arguments.resource ? frame.ribbon_arguments.resource->GetDesc().Width : 0};
    const auto expected_history_bytes = static_cast<std::uint64_t>(
        impl_->config.ribbon_history_points / kRibbonHistorySamples) * kRibbonHistoryStride;
    // Failed allocation must not consume a slot's pending GPU history reset.
    std::optional<VfxRibbonState> ribbon_checkpoint;
    if (impl_->render_scene_inputs &&
        (ribbon_buffer_widths[0] != expected_history_bytes || ribbon_buffer_widths[1] != expected_history_bytes ||
         ribbon_buffer_widths[2] == 0 || impl_->ribbon_state.OutputsMayExceed(
             ribbon_sources, ribbon_buffer_widths[2] / kRibbonArgumentStride, typed_ribbons.outputs)))
        ribbon_checkpoint = impl_->ribbon_state;
    const auto reject_ribbon_update = [&](Result failure) {
        if (ribbon_checkpoint) impl_->ribbon_state = std::move(*ribbon_checkpoint);
        return failure;
    };
    std::string vfx_error;
    if (impl_->render_scene_inputs && !impl_->ribbon_state.UpdateValidated(
            ribbon_sources,
            snapshot.header.tick, snapshot.header.session_id, typed_ribbons.catalog_generation,
            impl_->config.ribbon_history_points, interpolation, impl_->previous_ribbon_eye, vfx_error,
            typed_ribbons.outputs))
        return reject_ribbon_update(Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", vfx_error));
    const auto ribbon_updates = impl_->ribbon_state.Updates();
    const auto ribbon_outputs = impl_->ribbon_state.Outputs();
    const auto &ribbon_projectile_ids = prepared_scene.ribbon_projectile_ids;
    cpu_timings.ribbon_state_us = capture_elapsed(ribbon_state_start);
    const auto ribbon_history_bytes = static_cast<UINT64>(impl_->ribbon_state.Capacity()) * kRibbonHistoryStride;
    const auto ribbon_args_bytes = RibbonArgumentBufferBytes(ribbon_buffer_widths[2],
        std::max<std::size_t>(ribbon_outputs.size(), 1) * kRibbonArgumentStride);
    const auto ribbon_replacements = RibbonBufferReplacements(ribbon_buffer_widths, ribbon_history_bytes, ribbon_args_bytes);
    if (std::ranges::any_of(ribbon_replacements, [](bool replace) { return replace; }))
    {
        // Arguments are scratch for this already-completed frame slot; only shared history needs all readers.
        if (ribbon_replacements[0] || ribbon_replacements[1])
        {
            const auto ribbon_wait_start = capture_clock();
            const auto waited = impl_->WaitForGpu();
            cpu_timings.ribbon_wait_us = capture_elapsed(ribbon_wait_start);
            if (!waited) return reject_ribbon_update(waited);
        }
        const auto ribbon_allocation_start = capture_clock();
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        const auto allocate = [&](AllocationResource &resource, UINT64 bytes) {
            if (impl_->config.capture_timing_stream)
            {
                ++cpu_timings.ribbon_replacement_count;
                cpu_timings.ribbon_allocation_bytes += bytes;
            }
            return impl_->CreateAllocation(resource, allocation,
                BufferDescription(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        if (auto allocated = ReplaceRibbonBuffers(
                std::array{&impl_->ribbon_history, &impl_->ribbon_previous, &frame.ribbon_arguments},
                ribbon_replacements, ribbon_history_bytes, ribbon_args_bytes, allocate); !allocated)
            return reject_ribbon_update(allocated);
        cpu_timings.ribbon_allocation_us = capture_elapsed(ribbon_allocation_start);
    }
    ribbon_checkpoint.reset();
    std::uint64_t ribbon_cpu_end{};
    if (ribbon_cpu_available && ribbon_thread_cpu(ribbon_cpu_end) && ribbon_cpu_end >= ribbon_cpu_begin)
    {
        cpu_timings.ribbon_prepare_thread_cpu_us = (ribbon_cpu_end - ribbon_cpu_begin) / 10;
        cpu_timings.ribbon_prepare_thread_cpu_valid = true;
    }
    cpu_timings.ribbon_prepare_us = capture_elapsed(ribbon_prepare_start);
    const auto is_environment_mesh = [](RenderMesh mesh) {
        return render_data::IsEnvironmentMesh(mesh);
    };
    const auto environment_mesh_index = [](const RenderInstance &source) -> std::size_t {
        return render_data::EnvironmentGeometryOrdinal(source.mesh, source.environment_variant);
    };
    const auto authored_source = [&](const RenderInstance &source) {
        return is_environment_mesh(source.mesh) || source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch;
    };
    const auto masked_source = [&](const RenderInstance &source) {
        return is_environment_mesh(source.mesh) &&
            impl_->environment_material_variants[render_data::EnvironmentGeometryMaterialOrdinal(
                static_cast<std::uint32_t>(environment_mesh_index(source)))] ==
                render_data::EnvironmentMaterialVariant::MaskedTwoSided;
    };
    // Scene identity belongs to the unique input objects. Sorting and LOD
    // expansion produce draw references to those objects, including anonymous
    // fallback identities based on this canonical source index.
    const auto &canonical_instances = prepared_scene.instances;
    const auto foliage_density = impl_->QualityProfileFor(impl_->config.foliage_quality).foliage_density;
    if (!prepared_scene.order)
    {
        auto &selected_monster_lods = prepared_scene.selected_lods;
        selected_monster_lods.assign(canonical_instances.size(), 0);
        const double lod_shadow_distance = impl_->config.shadow_distance_m;
        const std::array<double, 3> lod_cascade_extents{
            lod_shadow_distance * (18.0 / 160.0),
            lod_shadow_distance * (48.0 / 160.0), lod_shadow_distance};
        const double lod_shadow_resolution = std::clamp(impl_->config.shadow_resolution, 1024u, 2048u);
        double finest_gi_voxel = owned_view.brixelizer_gi.FinestVoxelSizeMeters();
        bool vsm_active = owned_view.virtual_shadows.AllocatedBytes() != 0;
        bool local_shadows_active = owned_view.local_shadows.AllocatedBytes() != 0;
        // One canonical LOD must satisfy every view's actual native consumers.
        for (const auto &view : impl_->explicit_frame->views)
        {
            if (view.id == impl_->active_view_id) continue;
            const auto &owner = *impl_->owned_views.at(view.id.value);
            finest_gi_voxel = MergeActiveGiVoxelSize(finest_gi_voxel, owner.brixelizer_gi.FinestVoxelSizeMeters());
            vsm_active |= owner.virtual_shadows.AllocatedBytes() != 0;
            local_shadows_active |= owner.local_shadows.AllocatedBytes() != 0;
        }
        const MonsterLodProjectionInputs monster_projection_inputs{
            std::span<const RenderViewInput>{impl_->explicit_frame->views},
            snapshot.lights, view_input.scene_origin,
            {lod_cascade_extents[0] / lod_shadow_resolution,
             lod_cascade_extents[1] / lod_shadow_resolution,
             lod_cascade_extents[2] / lod_shadow_resolution},
            {lod_cascade_extents[0] / (32.0 * kVirtualShadowInner),
             lod_cascade_extents[1] / (8.0 * kVirtualShadowInner),
             lod_cascade_extents[2] / (8.0 * kVirtualShadowInner)},
            finest_gi_voxel,
            impl_->QualityProfileFor(impl_->config.geometry_quality).geometry_screen_error_px,
            impl_->QualityProfileFor(impl_->config.shadow_quality).local_shadow_resolution,
            true, vsm_active, local_shadows_active,
            active_features.software_gi_enabled, true, true};
        const auto monster_consumers = PrepareMonsterLodConsumers(monster_projection_inputs);
        prepared_scene.monster_lod_consumers_valid = monster_consumers.baseline.valid;
        prepared_scene.vsm_skin_consumer = vsm_active;
        prepared_scene.local_shadow_consumers = monster_consumers.local_lights;
        std::array<std::uint32_t, 6> monster_own_resident_masks{};
        for (std::size_t family = 0; family < monster_own_resident_masks.size(); ++family)
        {
            const auto &asset = impl_->monster_assets[family];
            auto &mask = monster_own_resident_masks[family];
            mask = 1u;
            for (std::size_t lod = 1; lod < asset.geometry_lods.size() && lod <= 2; ++lod)
            {
                const auto &page = impl_->monster_reduced_geometry[family][lod - 1u];
                const auto &expected = asset.geometry_lods[lod];
                if (page.page_slot == kMonsterReducedGeometryPage + family * 2u + lod - 1u &&
                    page.storage.resource && page.residency && impl_->residency.Contains(page.residency) &&
                    page.generation != 0 && page.metadata.lod == lod &&
                    page.metadata.error_method_abi == expected.error_method_abi &&
                    page.vertex_count == expected.vertex_count && page.index_count == expected.index_count &&
                    page.source_asset.value != 0)
                    mask |= 1u << lod;
            }
        }
        for (std::size_t source_index = 0; source_index < canonical_instances.size(); ++source_index)
        {
            const auto &source = canonical_instances[source_index];
            if (!is_monster(source.mesh)) continue;
            const auto family = static_cast<std::size_t>(source.mesh) -
                static_cast<std::size_t>(RenderMesh::MonsterMelee);
            const auto &asset = impl_->monster_assets[family];
            if (asset.geometry_lods.size() < 2 || asset.geometry_lods.size() > 3 ||
                !asset.interval_lookup_valid ||
                asset.deformation_flags != render_data::kMonsterBoundsPositionEnclosure) continue;
            AnimationPoseRef preview_pose;
            const AnimationPoseRef *pose = nullptr;
            if (impl_->config.monster_preview_asset < 6 || family_preview)
            {
                preview_pose.clip = static_cast<CharacterAnimationClip>(
                    std::min(impl_->config.monster_preview_clip, 4u));
                preview_pose.normalized_time = impl_->config.monster_preview_time;
                pose = &preview_pose;
            }
            else if (const auto found = pose_by_id.find(source.stable_id);
                     found != pose_by_id.end()) pose = found->second;
            if (source.animation_clip_id.value != 0 ||
                (pose && (pose->secondary_weight != 0.0f || pose->upper_body_weight != 0.0f)))
                continue;
            const auto clip = pose ? pose->clip : CharacterAnimationClip::Idle;
            const float time = pose ? pose->normalized_time : 0.0f;
            const auto clip_index = static_cast<std::size_t>(clip);
            if (clip_index >= asset.clip_meta.size() || !std::isfinite(time) ||
                !std::isfinite(source.scale.x) || !std::isfinite(source.scale.y) ||
                !std::isfinite(source.scale.z)) continue;
            const auto meta = asset.clip_meta[clip_index];
            if (meta.y == 0 || meta.w != asset.bone_count || asset.bone_count == 0 ||
                meta.x % asset.bone_count != 0) continue;
            const auto frame_position = std::clamp(time, 0.0f, 1.0f) * float(meta.y - 1u);
            const auto frame_index = std::min(static_cast<std::uint32_t>(std::floor(frame_position)), meta.y - 1u);
            const auto &extent = asset.interval_maximum_extent;
            const bool bounds_qualified = asset.IntervalBound(0, clip, frame_index) != nullptr;
            // All certified clip rows enclose the current and last submitted poses.
            const double world_scale = std::max({std::abs(double(source.scale.x)),
                std::abs(double(source.scale.y)), std::abs(double(source.scale.z))});
            const double radius = std::hypot(extent[0] * std::abs(double(source.scale.x)),
                extent[1] * std::abs(double(source.scale.y)),
                extent[2] * std::abs(double(source.scale.z))) * 1.0011 + 0.001;
            const MonsterLodWorldSphere sphere{{view_input.scene_origin.x + source.position.x,
                view_input.scene_origin.y + source.position.y + asset.ground_offset * source.scale.y,
                view_input.scene_origin.z + source.position.z}, radius, bounds_qualified};
            auto projection = PrepareMonsterLodProjection(sphere, monster_consumers);
            if (!std::isfinite(world_scale) || world_scale <= 0) projection.valid = false;
            auto selected = SelectMonsterLod(asset.geometry_lods, monster_own_resident_masks[family],
                static_cast<float>(world_scale), projection);
            const auto key = MakeRenderSceneInstanceKey(source.stable_id,
                static_cast<std::uint32_t>(source_index));
            // RenderAllViews sets this ID once before its sequential view loop;
            // a submitted first view publishes the matching canonical skin frame.
            bool committed_package_choice{};
            if (impl_->canonical_skin_current &&
                impl_->canonical_skin_current->package_id == impl_->active_render_package_id)
                if (const auto *committed = impl_->canonical_skin_current->vertex_ranges.Find(key))
                    for (std::uint16_t lod = 0; lod < asset.geometry_lods.size(); ++lod)
                    {
                        const GeometryPageGpu &page = lod == 0
                            ? static_cast<const GeometryPageGpu &>(asset)
                            : impl_->monster_reduced_geometry[family][lod - 1u];
                        if (committed->geometry_identity ==
                                ((std::uint64_t(page.generation) << 32u) | page.page_slot) &&
                            committed->count == page.vertex_count &&
                            (lod == 0 || (monster_own_resident_masks[family] & (1u << lod)) != 0))
                        {
                            selected = lod;
                            committed_package_choice = true;
                            break;
                        }
                    }
            if (committed_package_choice)
            {
                selected_monster_lods[source_index] = selected;
                continue;
            }
            if (selected && impl_->canonical_skin_current)
            {
                std::uint16_t previous_lod{};
                if (const auto *previous = impl_->canonical_skin_current->vertex_ranges.Find(key))
                    for (std::uint16_t lod = 1; lod < asset.geometry_lods.size(); ++lod)
                    {
                        const auto &page = impl_->monster_reduced_geometry[family][lod - 1u];
                        if (previous->geometry_identity ==
                            ((std::uint64_t(page.generation) << 32u) | page.page_slot))
                            previous_lod = lod;
                    }
                if (selected > previous_lod)
                {
                    projection.maximum_normalized_projection *= 1.25;
                    selected = SelectMonsterLod(asset.geometry_lods, monster_own_resident_masks[family],
                        static_cast<float>(world_scale), projection);
                }
            }
            selected_monster_lods[source_index] = selected;
        }
        auto &ordered_instances = prepared_scene.ordered_instances;
        auto &ordered_source_indices = prepared_scene.ordered_source_indices;
        ordered_instances.clear();
        ordered_instances.reserve(canonical_instances.size());
        ordered_source_indices.reserve(canonical_instances.size());
        auto &lod_order = prepared_scene.order;
        lod_order = BuildRenderInstanceOrderWithMonsterLods(
            canonical_instances, selected_monster_lods, foliage_density, ordered_source_indices,
            prepared_scene.order_scratch);
        if (!lod_order) return Result::Failure(ErrorCode::InvalidState,
            "hs_renderer_d3d12", "Monster LOD draw order is invalid.");
        auto &ordered_monster_lods = prepared_scene.ordered_lods;
        ordered_monster_lods.clear();
        ordered_monster_lods.reserve(ordered_source_indices.size());
        for (const auto source_index : ordered_source_indices)
        {
            ordered_instances.push_back(canonical_instances[source_index]);
            ordered_monster_lods.push_back(selected_monster_lods[source_index]);
        }
        prepared_scene.instance_lookup.Update(ordered_instances);
        auto &attachment_ids = prepared_scene.skin_attachment_ids;
        attachment_ids.clear();
        attachment_ids.reserve(typed_fresnels.size() + typed_entity_surfaces.size());
        for (const auto &effect : typed_fresnels)
            if (effect.render_instance_id) attachment_ids.push_back(effect.render_instance_id);
        for (const auto &effect : typed_entity_surfaces)
            if (effect.render_instance_id) attachment_ids.push_back(effect.render_instance_id);
        std::ranges::sort(attachment_ids);
        attachment_ids.erase(std::unique(attachment_ids.begin(), attachment_ids.end()), attachment_ids.end());
    }
    const auto &instance_ranges = prepared_scene.order->ranges;
    const auto &monster_lod_ranges = prepared_scene.order->monster_lod_ranges;
    const auto &ordered_source_indices = prepared_scene.ordered_source_indices;
    const auto &ordered_monster_lods = prepared_scene.ordered_lods;
    constexpr std::size_t kEnvironmentVisibilityBucketCount = kEnvironmentMaterialNames.size() * render_data::kEnvironmentGeometryLodCount;
    constexpr std::size_t kArcherVisibilityBucket = kEnvironmentVisibilityBucketCount;
    constexpr std::size_t kMonsterVisibilityBucketBegin = kArcherVisibilityBucket + 1;
    std::array<std::pair<std::size_t, std::size_t>,
               kEnvironmentVisibilityBucketCount> environment_ranges{};
    const auto static_scene = impl_->config.slime_family_preview_count == 0 &&
                              impl_->config.monster_preview_asset >= 6 &&
                              !impl_->config.environment_preview
        ? snapshot.static_scene : nullptr;
    std::span<const RenderInstance> hlod_static_instances;
    if (auto prepared = impl_->PrepareHlodStaticScene(static_scene, hlod_static_instances); !prepared) return prepared;
    const bool view_scene_changed = owned_view.frame_number != 0 &&
        impl_->owned_views.at(impl_->active_view_id.value)->scene_lease != static_scene;
    if (view_scene_changed)
    {
        owned_view.hzb_history_valid = false; owned_view.temporal_history_valid = false;
        owned_view.ambient_occlusion.ResetHistory(); owned_view.screen_reflections.ResetHistory();
        owned_view.volumetrics.ResetHistory();
    }
    if (static_scene)
    {
        const bool replace_static_scene = !impl_->static_gpu_scene || impl_->static_gpu_scene->source != static_scene;
        if (replace_static_scene && impl_->next_static_instance_generation == UINT32_MAX)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Static instance generation exhausted.");
        const auto static_generation = replace_static_scene ? impl_->next_static_instance_generation++
            : impl_->static_gpu_scene->instance_generation;
        if (static_scene->instances.size() > std::numeric_limits<std::uint32_t>::max())
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Static scene instance index overflow.");
        const bool rebuild_draws = replace_static_scene || !impl_->static_gpu_scene->draws.valid ||
            impl_->static_gpu_scene->draws.foliage_density != foliage_density;
        std::optional<Impl::StaticDrawData> rebuilt_draws;
        if (rebuild_draws)
        {
            auto &next_draws = rebuilt_draws.emplace();
            std::array<std::vector<GpuStaticDrawRef>, kEnvironmentVisibilityBucketCount>
                static_environment_buckets;
            std::array<std::vector<GeometryDemandPoint>, kEnvironmentMaterialNames.size()> demand_points;
            std::vector<GpuStaticDrawRef> static_ground_bucket;
            for (std::size_t index = 0; index < static_scene->instances.size(); ++index)
            {
                const auto &source = static_scene->instances[index];
                const auto instance_index = static_cast<std::uint32_t>(index);
                if (is_environment_mesh(source.mesh))
                {
                    if (!RetainDecorativeFoliage(source, index, foliage_density)) continue;
                    const auto ordinal = environment_mesh_index(source);
                    const auto base = ordinal * 3;
                    static_environment_buckets[base].push_back({instance_index, 0, 1.0f, static_generation});
                    demand_points[ordinal].push_back({{source.position.x, source.position.y, source.position.z},
                        std::max({std::abs(source.scale.x), std::abs(source.scale.y), std::abs(source.scale.z)}),
                        instance_index, HasEnvironmentWind(source.mesh, source.environment_variant)});
                }
                else if (source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch)
                    static_ground_bucket.push_back({instance_index, 0, 1.0f, static_generation});
                else
                    return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                           "Unsupported mesh in static scene.");
            }
            for (std::size_t asset = 0; asset < static_environment_buckets.size(); asset += 3)
            {
                const auto &bucket = static_environment_buckets[asset];
                const auto begin = next_draws.refs.size();
                next_draws.refs.insert(next_draws.refs.end(), bucket.begin(), bucket.end());
                for (std::size_t lod = 0; lod < 3; ++lod)
                    next_draws.ranges[asset + lod] = {begin, bucket.size()};
                next_draws.geometry_demand_cells[asset / 3] = BuildGeometryDemandCells(demand_points[asset / 3]);
            }
            next_draws.ground_range = {next_draws.refs.size(), static_ground_bucket.size()};
            next_draws.refs.insert(next_draws.refs.end(), static_ground_bucket.begin(), static_ground_bucket.end());
            next_draws.leaf_count = next_draws.refs.size();
            next_draws.foliage_density = foliage_density;
            next_draws.valid = true;
        }
        if (!impl_->static_gpu_scene || impl_->static_gpu_scene->source != static_scene)
        {
            // Previous HZB/temporal history belongs to the old immutable static
            // scene generation. Never use it to reject geometry from a newly
            // published scene.
            if (impl_->static_gpu_scene)
            {
                owned_view.hzb_history_valid = false;
                owned_view.temporal_history_valid = false;
            }
            auto next = std::make_shared<Impl::StaticGpuScene>();
            next->source = static_scene;
            next->instance_generation = static_generation;
            const auto static_slot_count = static_scene->instances.size() + hlod_static_instances.size();
            if (!BuildGpuSceneTableLayout(
                    std::max<std::size_t>(static_slot_count, 1),
                    static_slot_count, next->layout))
                return Result::Failure(
                    ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                    "Static split GPU scene table size overflow.");
            const auto bytes = next->layout.total_bytes;
            D3D12MA::ALLOCATION_DESC allocation{};
            allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            if (auto created = impl_->CreateAllocation(next->instances, allocation,
                    BufferDescription(bytes), D3D12_RESOURCE_STATE_COPY_DEST); !created)
                return created;
            allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            if (auto created = impl_->CreateAllocation(next->upload, allocation,
                    BufferDescription(bytes), D3D12_RESOURCE_STATE_GENERIC_READ); !created)
                return created;
            std::byte *mapped{};
            D3D12_RANGE no_read{};
            const auto map_result = next->upload.resource->Map(
                0, &no_read, reinterpret_cast<void **>(&mapped));
            if (FAILED(map_result)) return HResultFailure("Map static scene upload", map_result);
            std::memcpy(mapped, &next->layout.header, sizeof(next->layout.header));
            for (std::size_t index = 0; index < static_slot_count; ++index)
            {
                const auto &source = index < static_scene->instances.size() ? static_scene->instances[index]
                    : hlod_static_instances[index - static_scene->instances.size()];
                if (index < static_scene->instances.size())
                {
                    next->has_authored_material |= authored_source(source);
                    next->has_masked_material |= masked_source(source);
                }
                const auto environment = is_environment_mesh(source.mesh);
                const float vertical_offset = environment ? 0.0f : source.scale.y * 0.5f;
                const auto mesh = static_cast<std::uint32_t>(source.mesh) |
                    (environment ? kEnvironmentMeshFlag : 0u) |
                    (source.mesh == RenderMesh::GroundDetail ? source.environment_variant << 8u : 0u);
                const auto seed = source.mesh == RenderMesh::Ground ||
                                  source.mesh == RenderMesh::DirtPatch || source.mesh == RenderMesh::MeadowSurface
                    ? std::bit_cast<float>(source.environment_seed) : 0.0f;
                const float radius = environment
                    ? impl_->environment_variant_radii[environment_mesh_index(source)] *
                        std::max({std::abs(source.scale.x), std::abs(source.scale.y),
                                  std::abs(source.scale.z)})
                    : 0.0f;
                const GpuInstance gpu_instance{
                    {source.position.x, source.position.y + vertical_offset,
                     source.position.z, radius},
                    {source.scale.x, source.scale.y, source.scale.z, 1.0f},
                    source.color_rgba, mesh, source.yaw, seed};
                const auto slot = static_cast<std::uint32_t>(index);
                const auto serialized = SerializeGpuSceneSlot(
                    slot, gpu_instance,
                    {source.asset_id.value, source.material_id.value,
                     std::numeric_limits<std::uint32_t>::max(), 0u, static_generation, 0u});
                const auto &header = next->layout.header;
                std::memcpy(mapped + header.instance_offset +
                                index * sizeof(GpuSceneInstanceEntry),
                            &serialized.instance, sizeof(serialized.instance));
                std::memcpy(mapped + header.transform_offset +
                                index * sizeof(GpuSceneTransformEntry),
                            &serialized.transform, sizeof(serialized.transform));
                std::memcpy(mapped + header.mesh_offset +
                                index * sizeof(GpuSceneMeshEntry),
                            &serialized.mesh, sizeof(serialized.mesh));
                std::memcpy(mapped + header.material_offset +
                                index * sizeof(GpuSceneMaterialEntry),
                            &serialized.material, sizeof(serialized.material));
                std::memcpy(mapped + header.bounds_offset +
                                index * sizeof(GpuSceneBoundsEntry),
                            &serialized.bounds, sizeof(serialized.bounds));
                std::memcpy(mapped + header.skin_offset +
                                index * sizeof(GpuSceneSkinEntry),
                            &serialized.skin, sizeof(serialized.skin));
            }
            next->upload.resource->Unmap(0, nullptr);
            if (impl_->static_gpu_scene)
                impl_->retired_static_gpu_scenes.push_back(std::move(impl_->static_gpu_scene));
            impl_->static_gpu_scene = std::move(next);
        }
        if (rebuild_draws)
        {
            impl_->static_gpu_scene->draws = std::move(*rebuilt_draws);
            impl_->static_gpu_scene->refs_dirty = true;
        }
    }
    if (static_scene && impl_->static_gpu_scene)
        if (auto prepared = impl_->PrepareLodStateBuffer(impl_->static_gpu_scene->lod_states,
                impl_->static_gpu_scene->layout.header.capacity); !prepared) return prepared;
    std::optional<Impl::StaticDrawData> empty_static_draws;
    auto &static_draws = static_scene ? impl_->static_gpu_scene->draws : empty_static_draws.emplace();
    auto &static_draw_refs = static_draws.refs;
    // Discard the preceding frame's HLOD tail, including any partially prepared failed frame.
    auto &cluster_candidates = static_draws.candidates;
    std::array<ClusterCutInput, kEnvironmentMaterialNames.size()> cluster_inputs{};
    std::array<bool, kEnvironmentMaterialNames.size()> cluster_active{};
    const auto &static_environment_ranges = static_draws.ranges;
    std::array<std::uint32_t, kGpuVisibilityBucketCount> static_output_begins{};
    std::size_t static_output_count{};
    for (std::size_t asset = 0; asset < static_environment_ranges.size(); asset += 3)
    {
        const auto [begin, count] = static_environment_ranges[asset];
        const auto ordinal = asset / 3;
        cluster_active[ordinal] = ordinal < render_data::kMeadowGeometryBegin && impl_->ClusterHierarchyReady(static_cast<std::uint32_t>(ordinal));
        if (cluster_active[ordinal])
        {
            const auto &catalog = impl_->cluster_runtime->catalogs[ordinal];
            const std::array<std::uint32_t, 3> catalog_key{catalog.group_base, catalog.group_count, catalog.first_root};
            auto &candidates = cluster_candidates[ordinal];
            if (!static_draws.candidates_valid[ordinal] || static_draws.candidate_catalogs[ordinal] != catalog_key)
            {
                auto next_candidates = std::make_shared<std::vector<GpuClusterCandidate>>();
                next_candidates->reserve(count);
                const auto mesh = static_cast<std::uint32_t>(render_data::EnvironmentMeshForOrdinal(static_cast<std::uint32_t>(ordinal)));
                for (std::size_t index = begin; index < begin + count; ++index)
                {
                    const auto &reference = static_draw_refs[index];
                    next_candidates->push_back({reference.instance_index, reference.instance_generation,
                        static_cast<std::uint32_t>(ordinal), 15u, catalog.group_base, catalog.group_count,
                        mesh, 0u, 0.0f, reference.mesh_flags, catalog.first_root, 0u});
                }
                candidates = std::move(next_candidates);
                static_draws.candidate_catalogs[ordinal] = catalog_key;
                static_draws.candidates_valid[ordinal] = true;
            }
            cluster_inputs[ordinal].candidates = candidates;
            cluster_inputs[ordinal].max_instance_count = static_cast<std::uint32_t>(candidates->size());
            cluster_inputs[ordinal].constants.raster_record_base = UINT32_MAX;
        }
        for (std::size_t lod = 0; lod < 3; ++lod)
        {
            static_output_begins[asset + lod] = static_cast<std::uint32_t>(static_output_count);
            static_output_count += count;
        }
    }
    const auto &static_ground_range = static_draws.ground_range;
    constexpr std::size_t kGpuVisibilityViewCount = 4; // Camera and three light cascades.
    if (static_draw_refs.size() > std::numeric_limits<std::uint32_t>::max())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Static draw reference index overflow.");
    std::array<GpuStaticVisibilityRange, kGpuVisibilityBucketCount + 1> visibility_ranges{};
    for (auto &range : visibility_ranges)
        range.argument_stride = static_cast<std::uint32_t>(kGpuVisibilityBucketCount);
    for (std::size_t asset = 0; asset < static_environment_ranges.size(); ++asset)
    {
        const auto [begin, count] = static_environment_ranges[asset];
        visibility_ranges[asset] = {
            static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(count),
            0,
            static_cast<std::uint32_t>(kGpuVisibilityBucketCount)};
        if (cluster_active[asset / 3]) visibility_ranges[asset].count = 0;
    }
    const auto visible_ref_count = static_output_count;
    const auto visible_ref_stride = std::max<std::size_t>(visible_ref_count, 1);
    if (visible_ref_stride > std::numeric_limits<std::uint32_t>::max() /
                                 kGpuVisibilityViewCount ||
        visible_ref_stride > std::numeric_limits<std::size_t>::max() /
                                 (kGpuVisibilityViewCount * sizeof(GpuStaticDrawRef)))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Static visibility output index overflow.");
    const auto visible_ref_capacity = visible_ref_stride * kGpuVisibilityViewCount;
    for (std::size_t asset = 0; asset < environment_ranges.size(); asset += 3)
    {
        for (std::size_t lod = 0; lod < 3; ++lod)
            environment_ranges[asset + lod] =
                instance_ranges[kInstanceEnvironmentBucketBegin + asset / 3];
    }
    std::array<GpuStaticVisibilityRange, kGpuVisibilityBucketCount + 1>
        dynamic_visibility_ranges{};
    for (auto &range : dynamic_visibility_ranges)
        range.argument_stride = static_cast<std::uint32_t>(kGpuVisibilityBucketCount);
    std::size_t dynamic_visibility_ref_count{};
    std::array<std::uint32_t, kGpuVisibilityBucketCount> dynamic_output_begins{};
    std::size_t dynamic_output_count = environment_ranges.front().first;
    for (std::size_t asset = 0; asset < environment_ranges.size(); ++asset)
    {
        const auto [begin, count] = environment_ranges[asset];
        dynamic_visibility_ranges[asset] = {
            static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(count),
            0,
            static_cast<std::uint32_t>(kGpuVisibilityBucketCount)};
        if (asset % 3 == 0) dynamic_visibility_ref_count += count;
        dynamic_output_begins[asset] = static_cast<std::uint32_t>(dynamic_output_count);
        dynamic_output_count += count;
    }
    const auto dynamic_visible_ref_stride = std::max<std::size_t>(
        dynamic_output_count, 1);
    if (dynamic_visible_ref_stride >
            std::numeric_limits<std::uint32_t>::max() / kGpuVisibilityViewCount ||
        dynamic_visible_ref_stride >
            std::numeric_limits<std::size_t>::max() /
                (kGpuVisibilityViewCount * sizeof(GpuStaticDrawRef)))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Dynamic visibility output index overflow.");
    const auto dynamic_visible_ref_capacity =
        dynamic_visible_ref_stride * kGpuVisibilityViewCount;
    const auto [dynamic_terrain_begin, dynamic_terrain_count] =
        instance_ranges[kInstanceTerrainBucket];
    const auto previous_instance_snapshot = !motion_history_reset && !view_input.camera_cut &&
        owned_view.previous_instances && owned_view.previous_instances->submitted &&
        owned_view.previous_instances->device == impl_->device.Get() &&
        owned_view.previous_instances->scene.resource
            ? owned_view.previous_instances : nullptr;
    std::array<std::pair<std::size_t, std::size_t>, 6> monster_ranges{};
    std::array<std::pair<std::size_t, std::size_t>, 18> monster_bucket_ranges{};
    const auto actor_range = [&](std::size_t bucket) {
        return instance_ranges[bucket].second != 0 ? instance_ranges[bucket]
            : std::pair<std::size_t, std::size_t>{};
    };
    const auto archer_range = actor_range(kInstanceArcherBucket);
    const auto projectile_range = actor_range(kInstanceProjectileBucket);
    for (std::size_t asset = 0; asset < monster_ranges.size(); ++asset)
    {
        monster_ranges[asset] = actor_range(kInstanceMonsterBucketBegin + asset);
        monster_bucket_ranges[asset] = monster_lod_ranges[asset * 3];
        monster_bucket_ranges[6 + asset * 2] = monster_lod_ranges[asset * 3 + 1];
        monster_bucket_ranges[7 + asset * 2] = monster_lod_ranges[asset * 3 + 2];
    }
    const auto has_translucent_hero_binding = [&](std::size_t asset) {
        return asset < impl_->hero_binding_ranges.size() && impl_->hero_binding_ranges[asset].w != 0;
    };
    const auto has_translucent_monster_bucket = [&](std::size_t bucket) {
        const auto asset = bucket < 6 ? bucket : (bucket - 6) / 2;
        return bucket < monster_bucket_ranges.size() && has_translucent_hero_binding(asset + 1);
    };
    if (archer_range.second != 0)
    {
        dynamic_output_begins[kArcherVisibilityBucket] = static_cast<std::uint32_t>(archer_range.first);
        dynamic_visibility_ranges[kArcherVisibilityBucket] = {
            static_cast<std::uint32_t>(archer_range.first),
            static_cast<std::uint32_t>(archer_range.second),
            impl_->archer_geometry.index_count,
            static_cast<std::uint32_t>(kGpuVisibilityBucketCount)};
        dynamic_visibility_ref_count += archer_range.second;
    }
    for (std::size_t asset = 0; asset < monster_bucket_ranges.size(); ++asset)
    {
        const auto [begin, count] = monster_bucket_ranges[asset];
        dynamic_output_begins[kMonsterVisibilityBucketBegin + asset] = static_cast<std::uint32_t>(begin);
        if (count == 0) continue;
        dynamic_visibility_ranges[kMonsterVisibilityBucketBegin + asset] = {
            static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(count),
            asset < 6 ? impl_->monster_assets[asset].index_count
                : impl_->monster_reduced_geometry[(asset - 6) / 2][(asset - 6) % 2].index_count,
            static_cast<std::uint32_t>(kGpuVisibilityBucketCount)};
        dynamic_visibility_ref_count += count;
    }
    const bool new_persistent_tick = impl_->render_scene_inputs && world_step_requested &&
        (!impl_->particles_initialized || stateful_target != impl_->last_particle_tick);
    const auto environment_pipeline = [&](std::size_t asset, bool shadow_draw) {
        const auto material = render_data::EnvironmentGeometryMaterialOrdinal(static_cast<std::uint32_t>(asset / 3));
        const bool masked = impl_->environment_material_variants[material] ==
            render_data::EnvironmentMaterialVariant::MaskedTwoSided;
        return shadow_draw
            ? (masked ? impl_->environment_material_shadow_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(static_cast<std::uint32_t>(asset / 3))].Get()
                      : impl_->environment_opaque_shadow_pipeline.Get())
            : (use_visibility ? impl_->environment_material_visibility_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(static_cast<std::uint32_t>(asset / 3))].Get()
                              : impl_->environment_material_scene_pipelines[render_data::EnvironmentGeometryMaterialOrdinal(static_cast<std::uint32_t>(asset / 3))].Get());
    };
    using GeometryPasses = std::array<std::array<std::uint32_t, 4>, kGpuVisibilityBucketCount>;
    GeometryPasses static_geometry_passes{}, dynamic_geometry_passes{};
    std::array<std::uint32_t, kGpuVisibilityBucketCount> retest_geometry_passes{};
    auto retest_visibility_ranges = visibility_ranges;
    std::array<const GeometryPageGpu *, kGpuVisibilityBucketCount> bucket_pages{};
    const auto prepare_geometry_range = [&](GpuStaticVisibilityRange &range,
            const GeometryPageGpu &page, ID3D12PipelineState *pipeline,
            std::size_t stride, std::uint32_t output_begin, std::uint32_t mesh_id,
            std::span<std::uint32_t> passes, std::uint32_t ref_source,
            bool static_instances = false) -> Result {
        range.index_count = page.index_count;
        range.meshlet_count = page.meshlet_count;
        range.flags = impl_->GeometryUsesMesh(pipeline, page, range.count)
            ? kGeometryMeshEligible : 0u;
        for (std::uint32_t view = 0; view < passes.size(); ++view)
        {
            if (auto result = impl_->AddGeometryDrawRecord(frame_slot, page,
                    static_cast<std::uint32_t>(view * stride + output_begin), range.count,
                    kGeometryGpuSceneRefs | range.flags |
                        (static_instances ? kGeometryStaticInstances : 0u),
                    view, mesh_id, passes[view]); !result)
                return result;
            frame.geometry_draw_records_cpu[passes[view] >> 2].reserved1 = ref_source;
        }
        range.draw_record_base = passes[0] >> 2;
        return Result::Success();
    };
    for (std::size_t bucket = 0; bucket < kGpuVisibilityBucketCount; ++bucket)
    {
        const auto monster_bucket = bucket - kMonsterVisibilityBucketBegin;
        const auto page_slot = bucket < kEnvironmentVisibilityBucketCount ? static_cast<std::uint32_t>(bucket)
            : bucket == kArcherVisibilityBucket ? kArcherGeometryPage
            : monster_bucket < 6 ? kMonsterGeometryPage + static_cast<std::uint32_t>(monster_bucket)
            : kMonsterReducedGeometryPage + static_cast<std::uint32_t>(monster_bucket - 6);
        const auto *page = impl_->FindGeometryPage(page_slot);
        if (!page)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Geometry bucket has no resident page or terminal fallback.");
        if (bucket >= kMonsterVisibilityBucketBegin + 6 &&
            dynamic_visibility_ranges[bucket].count != 0 && page->page_slot != page_slot)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Selected monster LOD resolved to a fallback page.");
        bucket_pages[bucket] = page;
        auto *pipeline = bucket < kEnvironmentVisibilityBucketCount ? environment_pipeline(bucket, false)
            : (use_visibility ? impl_->hero_visibility_pipeline.Get() : impl_->hero_scene_pipeline.Get());
        // Depth peeling uses indexed SceneVS for every admitted translucent actor and water layer.
        const bool translucent_hero_bucket = bucket == kArcherVisibilityBucket
            ? has_translucent_hero_binding(0)
            : bucket >= kMonsterVisibilityBucketBegin && has_translucent_monster_bucket(monster_bucket);
        if (owned_view.hero_transparency.AllocatedBytes() != 0 &&
            (translucent_hero_bucket ||
             (bucket < kEnvironmentVisibilityBucketCount &&
              bucket / 3 == render_data::kMeadowWaterGeometry)))
            pipeline = nullptr;
        const auto mesh_id = bucket < kEnvironmentVisibilityBucketCount
            ? static_cast<std::uint32_t>(render_data::EnvironmentMeshForOrdinal(static_cast<std::uint32_t>(bucket / 3)))
            : bucket == kArcherVisibilityBucket ? static_cast<std::uint32_t>(RenderMesh::Archer)
            : static_cast<std::uint32_t>(RenderMesh::MonsterMelee) +
                  static_cast<std::uint32_t>(monster_bucket < 6 ? monster_bucket : (monster_bucket - 6) / 2);
        if (auto result = prepare_geometry_range(visibility_ranges[bucket], *page,
                pipeline, visible_ref_stride, static_output_begins[bucket], mesh_id,
                static_geometry_passes[bucket], 1u, true); !result)
            return result;
        if (auto result = prepare_geometry_range(dynamic_visibility_ranges[bucket], *page,
                pipeline, dynamic_visible_ref_stride, dynamic_output_begins[bucket], mesh_id,
                dynamic_geometry_passes[bucket], 2u); !result)
            return result;
        retest_visibility_ranges[bucket].begin = static_output_begins[bucket];
        if (auto result = prepare_geometry_range(retest_visibility_ranges[bucket], *page,
                pipeline, 0, static_output_begins[bucket], mesh_id,
                std::span{&retest_geometry_passes[bucket], 1}, 3u, true); !result)
            return result;
    }
    // Each family has one candidate owner. Its three records describe complete
    // resident cuts; aliasing fallback pages are deduplicated by the GPU.
    for (std::size_t family = 0; family < kEnvironmentMaterialNames.size(); ++family)
    {
        std::uint32_t epoch = 2166136261u;
        const auto hash = [&](std::uint32_t value) { epoch = (epoch ^ value) * 16777619u; };
        hash(static_cast<std::uint32_t>(family));
        for (std::size_t lod = 0; lod < 3; ++lod)
        {
            const auto &page = *bucket_pages[family * 3 + lod];
            hash(page.page_slot);
            hash(page.generation);
        }
        for (std::size_t lod = 0; lod < 3; ++lod)
        {
            const auto bucket = family * 3 + lod;
            visibility_ranges[bucket].flags |= 4u;
            dynamic_visibility_ranges[bucket].flags |= 4u;
            visibility_ranges[bucket].reserved = epoch;
            dynamic_visibility_ranges[bucket].reserved = epoch;
        }
    }
    std::uint32_t projectile_geometry_pass{};
    if (projectile_range.second != 0)
        if (auto result = impl_->AddGeometryDrawRecord(frame_slot,
                impl_->gel_projectile_geometry, static_cast<std::uint32_t>(projectile_range.first),
                static_cast<std::uint32_t>(projectile_range.second), kGeometryGpuSceneRefs |
                    (impl_->GeometryUsesMesh(impl_->slime_pipeline.Get(), impl_->gel_projectile_geometry,
                        static_cast<std::uint32_t>(projectile_range.second)) ? kGeometryMeshEligible : 0u),
                0, static_cast<std::uint32_t>(RenderMesh::EnemyProjectile), projectile_geometry_pass); !result)
            return result;
    const auto procedural_begin = std::ranges::find_if(
        render_instances, [&](const auto &source) {
            return source.mesh != RenderMesh::Archer && !is_environment_mesh(source.mesh) &&
                   !(source.mesh >= RenderMesh::MonsterMelee &&
                      source.mesh <= RenderMesh::BossFinal) &&
                   source.mesh != RenderMesh::EnemyProjectile &&
                   source.mesh != RenderMesh::Ground && source.mesh != RenderMesh::DirtPatch;
        });
    const std::size_t procedural_first = static_cast<std::size_t>(
        procedural_begin - render_instances.begin());
    const std::size_t procedural_count = render_instances.size() - procedural_first;
    std::array<std::uint32_t, 4> ground_geometry_passes{}, dynamic_terrain_geometry_passes{},
        procedural_geometry_passes{};
    const auto terrain_draw_material =
        (impl_->material_parameter_bases[kTerrainMaterialOrdinal] << 1) | 1u;
    auto *terrain_pipeline = use_visibility
        ? impl_->environment_material_visibility_pipelines[kTerrainMaterialOrdinal].Get()
        : impl_->environment_material_scene_pipelines[kTerrainMaterialOrdinal].Get();
    const auto prepare_builtin = [&](std::size_t begin, std::size_t count,
            bool static_instances, bool terrain, std::span<std::uint32_t, 4> passes) -> Result {
        if (count == 0) return Result::Success();
        for (std::uint32_t view = 0; view < passes.size(); ++view)
        {
            auto *pipeline = view == 0
                ? (terrain ? terrain_pipeline
                    : (use_visibility ? impl_->visibility_pipeline.Get() : impl_->scene_pipeline.Get()))
                : impl_->shadow_pipeline.Get();
            const auto flags = kGeometryGpuSceneRefs | kGeometryBuiltin |
                (static_instances ? kGeometryStaticInstances : 0u) |
                (impl_->GeometryUsesMesh(pipeline, impl_->procedural_geometry,
                    static_cast<std::uint32_t>(count)) ? kGeometryMeshEligible : 0u);
            if (auto result = impl_->AddGeometryDrawRecord(frame_slot,
                    impl_->procedural_geometry, static_cast<std::uint32_t>(begin),
                    static_cast<std::uint32_t>(count), flags, view,
                    terrain ? static_cast<std::uint32_t>(RenderMesh::Ground) : 0u,
                    passes[view]); !result) return result;
            frame.geometry_draw_records_cpu[passes[view] >> 2].reserved1 =
                static_instances ? 4u : 5u;
        }
        return Result::Success();
    };
    if (auto result = prepare_builtin(static_ground_range.first, static_ground_range.second,
            true, true, ground_geometry_passes); !result) return result;
    if (auto result = prepare_builtin(dynamic_terrain_begin, dynamic_terrain_count,
            false, true, dynamic_terrain_geometry_passes); !result) return result;
    if (auto result = prepare_builtin(procedural_first, procedural_count,
            false, false, procedural_geometry_passes); !result) return result;
    const auto static_leaf_ref_count = static_draws.leaf_count;
    bool hlod_refs_changed{};
    const auto prepared_hlod_refs = impl_->PrepareHlodFrame(frame_slot,
        static_draw_refs, hlod_refs_changed);
    if (static_scene && impl_->static_gpu_scene && hlod_refs_changed)
        impl_->static_gpu_scene->refs_dirty = true;
    if (!prepared_hlod_refs) return prepared_hlod_refs;
    if (static_draw_refs.size() > std::numeric_limits<UINT>::max() ||
        static_draw_refs.size() > std::numeric_limits<std::size_t>::max() /
            sizeof(GpuStaticDrawRef))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
            "Static draw reference overflow.");
    if (static_scene && impl_->static_gpu_scene &&
        (impl_->static_gpu_scene->refs_dirty || !impl_->static_gpu_scene->refs))
    {
        using RefTable = Impl::StaticGpuScene::RefTable;
        auto next = std::make_shared<RefTable>();
        next->cpu = std::make_shared<const std::vector<GpuStaticDrawRef>>(static_draw_refs);
        const auto bytes = std::max<std::size_t>(sizeof(GpuStaticDrawRef),
            static_draw_refs.size() * sizeof(GpuStaticDrawRef));
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = impl_->CreateAllocation(next->storage, allocation,
                BufferDescription(bytes), D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = impl_->CreateAllocation(next->upload, allocation,
                BufferDescription(bytes), D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
        void *mapped{};
        const D3D12_RANGE no_read{};
        const auto result = next->upload.resource->Map(0, &no_read, &mapped);
        if (FAILED(result)) return HResultFailure("Map static draw refs", result);
        std::memset(mapped, 0, bytes);
        if (!static_draw_refs.empty())
            std::memcpy(mapped, static_draw_refs.data(),
                static_draw_refs.size() * sizeof(GpuStaticDrawRef));
        next->upload.resource->Unmap(0, nullptr);
        if (impl_->static_gpu_scene->refs)
            impl_->static_gpu_scene->retired_refs.push_back(std::move(impl_->static_gpu_scene->refs));
        impl_->static_gpu_scene->refs = std::move(next);
        impl_->static_gpu_scene->refs_dirty = false;
    }
    if (static_scene && impl_->static_gpu_scene && impl_->static_gpu_scene->refs)
        if (auto certified = impl_->static_gpu_scene->refs->storage.EnsureNativeContract(
                impl_->device.Get()); !certified) return certified;
    if (use_visibility)
    {
        visibility_frame.Clear();
        std::uint32_t raster_cursor = 1;
        const auto add_raster_range = [&](std::uint32_t packed, std::uint32_t material) -> Result {
            if (packed == 0) return Result::Success();
            auto &record = frame.geometry_draw_records_cpu[packed >> 2];
            if (record.instance_count == 0) return Result::Success();
            if (record.instance_count >= UINT32_MAX - raster_cursor)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Visibility raster record ID capacity exceeded.");
            record.reserved2 = raster_cursor;
            visibility_frame.ranges_cpu.push_back({packed >> 2, raster_cursor,
                record.instance_count, material});
            raster_cursor += record.instance_count;
            return Result::Success();
        };
        for (std::size_t bucket = 0; bucket < kGpuVisibilityBucketCount; ++bucket)
        {
            std::uint32_t material = kDefaultSceneDrawMaterial | 1u;
            if (bucket < kEnvironmentVisibilityBucketCount)
            {
                material = (impl_->material_parameter_bases[render_data::EnvironmentGeometryMaterialOrdinal(static_cast<std::uint32_t>(bucket / 3))] << 1) | 1u;
            }
            for (const auto packed : {static_geometry_passes[bucket][0],
                    dynamic_geometry_passes[bucket][0], retest_geometry_passes[bucket]})
                if (auto result = add_raster_range(packed, material); !result) return result;
        }
        for (const auto packed : {ground_geometry_passes[0], dynamic_terrain_geometry_passes[0]})
            if (auto result = add_raster_range(packed, terrain_draw_material); !result) return result;
        if (auto result = add_raster_range(procedural_geometry_passes[0],
                kDefaultSceneDrawMaterial | 1u); !result) return result;
        for (std::size_t ordinal = 0; ordinal < cluster_inputs.size(); ++ordinal)
        {
            if (!cluster_active[ordinal] || !cluster_candidates[ordinal] || cluster_candidates[ordinal]->empty()) continue;
            const auto count = std::max<std::uint64_t>(
                static_cast<std::uint64_t>(cluster_candidates[ordinal]->size()) *
                    impl_->cluster_runtime->catalogs[ordinal].worst_cut_records,
                impl_->cluster_runtime->frames[frame_slot].cuts[ordinal].required_capacity);
            if (count > UINT32_MAX - raster_cursor)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Cluster raster record capacity overflows.");
            cluster_inputs[ordinal].constants.raster_record_base = raster_cursor;
            raster_cursor += static_cast<std::uint32_t>(count);
        }
        if (auto *hlod_view = impl_->ActiveHlodView(); hlod_view && hlod_view->frames[frame_slot].enabled)
            for (const auto &binding : hlod_view->frames[frame_slot].bindings)
                if ((binding.flags & kHlodProxyResident) != 0u)
                    if (auto result = add_raster_range(binding.geometry_record_id << 2u,
                        hlod_view->frames[frame_slot].buckets[binding.bucket_index].material_word); !result) return result;
        visibility_frame.raster_capacity = raster_cursor;
        visibility_frame.range_capacity = std::max<std::uint32_t>(1,
            static_cast<std::uint32_t>(visibility_frame.ranges_cpu.size()));
        if (auto result = EnsureVisibilityBuffers(impl_->device.Get(), impl_->allocator,
                visibility_frame); !result) return result;
    }
    const auto particle_spawn_prepare_start = capture_clock();
    auto &frame_particle_spawns = owned_view.particle_spawn_scratch;
    frame_particle_spawns.clear();
    frame_particle_spawns.reserve(particle_spawns.size());
    if (impl_->config.slime_family_preview_count == 0)
    {
        for (const auto &spawn : particle_spawns)
            if (spawn.count != 0) frame_particle_spawns.push_back(spawn);
    }
    if (new_persistent_tick)
    {
        std::unordered_map<std::uint64_t, std::array<const PersistentVfxVisual *, 4>> status_episodes;
        for (const auto &visual : snapshot.persistent_vfx)
        {
            std::size_t index;
            switch (visual.kind)
            {
            case PersistentVfxKind::EnemyBleedStatus: index = 0; break;
            case PersistentVfxKind::EnemyBurnStatus: index = 1; break;
            case PersistentVfxKind::EnemySlowStatus: index = 2; break;
            case PersistentVfxKind::EnemyMarkStatus: index = 3; break;
            default: continue;
            }
            auto &episode = status_episodes[visual.entity_render_id][index];
            if (!episode) episode = &visual;
        }
        std::vector<renderer_detail::VfxStatusCoverage> authored_status_coverage;
        std::vector<renderer_detail::VfxGroundCoverage> typed_ground_coverage;
        renderer_detail::PrepareVfxGroundCoverage(typed_ground_spawns,
            impl_->config.slime_family_preview_count != 0, typed_ground_coverage);
        renderer_detail::PrepareVfxStatusCoverage(snapshot.persistent_vfx,
            typed_ground_coverage, authored_status_coverage);
        std::vector<renderer_detail::PreviousVfxVisualRow> previous_by_identity;
        if (interpolation < 1.0f)
            renderer_detail::PreparePreviousVfxVisualIndex(
                snapshots.previous.persistent_vfx, previous_by_identity);
        for (const auto &source : snapshot.instances)
        {
            const auto episodes = status_episodes.find(source.stable_id);
            const auto add_status = [&](StatusVisual status,
                                        const ParticleSpriteBinding &binding,
                                        ParticleFacing facing, VfxRenderer renderer,
                                        VfxPrimitive primitive, float height,
                                        float size, Float4 color, std::uint32_t count) {
                if ((source.status_visual_mask & static_cast<std::uint32_t>(status)) == 0)
                    return;
                if (impl_->config.slime_family_preview_count == 0 &&
                    (status == StatusVisual::Slow || status == StatusVisual::Mark))
                {
                    const auto kind = status == StatusVisual::Slow
                        ? PersistentVfxKind::EnemySlowStatus
                        : PersistentVfxKind::EnemyMarkStatus;
                    const auto authored = renderer_detail::HasVfxStatusCoverage(
                        authored_status_coverage, source.stable_id,
                        static_cast<std::uint8_t>(kind));
                    if (authored) return;
                }
                ParticleSpawnCommand command;
                command.position = {source.position.x, source.position.y + height,
                                    source.position.z};
                command.shape = ParticleShape::Point;
                command.velocity_mode = ParticleVelocity::Direction;
                command.facing = facing;
                command.renderer = renderer;
                command.primitive = primitive;
                command.sprite = binding.sprite;
                command.frame_columns = binding.frame_columns;
                command.frame_rows = binding.frame_rows;
                command.direction = {0.0f, 1.0f, 0.0f};
                command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
                command.start_color = command.end_color = color;
                command.start_size_min = command.start_size_max = size;
                command.end_size_min = command.end_size_max = size;
                command.count = count;
                if (status == StatusVisual::Bleed)
                {
                    command.position.y = source.position.y + source.scale.y * 0.85f;
                    command.shape = ParticleShape::Sphere;
                    command.shape_extent = {source.scale.x * 0.35f,
                                            source.scale.y * 0.35f,
                                            source.scale.z * 0.35f};
                    command.direction = {0.0f, -1.0f, 0.0f};
                    command.speed_min = 0.25f;
                    command.speed_max = 0.6f;
                    command.lifetime_min = 0.35f;
                    command.lifetime_max = 0.5f;
                    command.end_color.w = 0.0f;
                    command.end_size_min = command.end_size_max = size * 0.45f;
                    command.gravity = -3.0f;
                    command.stretch = 2.0f;
                }
                else if (status == StatusVisual::Burn)
                {
                    command.position.y = source.position.y + source.scale.y * 0.2f;
                    command.shape = ParticleShape::Sphere;
                    command.velocity_mode = ParticleVelocity::Upward;
                    command.shape_extent = {source.scale.x * 0.4f,
                                            source.scale.y * 0.35f,
                                            source.scale.z * 0.4f};
                    command.speed_min = 0.3f;
                    command.speed_max = 0.9f;
                    command.lifetime_min = 0.28f;
                    command.lifetime_max = 0.45f;
                    command.end_color.w = 0.0f;
                    command.end_size_min = size * 0.35f;
                    command.end_size_max = size * 0.6f;
                    command.gravity = -0.4f;
                }
                const auto *episode = episodes == status_episodes.end() ? nullptr
                    : episodes->second[std::countr_zero(static_cast<std::uint32_t>(status))];
                const auto births = renderer_detail::StatusParticleBirths(episode,
                    source.stable_id, status, stateful_target, impl_->last_particle_tick,
                    !impl_->particles_initialized,
                    static_cast<Tick>(std::ceil(command.lifetime_max * 60.0f)));
                if (!births) return;
                for (Tick birth = births->first;; birth += births->cadence)
                {
                    command.tick = birth;
                    command.sequence = source.stable_id ^ birth ^ static_cast<std::uint32_t>(status);
                    command.seed = static_cast<std::uint32_t>(command.sequence);
                    // Older owner poses are not retained: preserve the observed
                    // owner position while GPU spawn reconstructs the known age.
                    frame_particle_spawns.push_back(command);
                    if (births->last - birth < births->cadence) break;
                }
            };
            add_status(StatusVisual::Bleed, impl_->config.bleed_status_sprite,
                       ParticleFacing::Velocity, VfxRenderer::Sprite,
                       VfxPrimitive::Shard, source.scale.y * 0.55f,
                       source.scale.y * 0.16f, {1.8f, 0.04f, 0.05f, 0.42f}, 1);
            add_status(StatusVisual::Burn, impl_->config.burn_status_sprite,
                       ParticleFacing::Camera, VfxRenderer::Sprite,
                       VfxPrimitive::Flame, source.scale.y * 0.5f,
                       source.scale.y * 0.18f, {2.2f, 0.7f, 0.08f, 0.38f}, 1);
            add_status(StatusVisual::Slow, impl_->config.slow_status_sprite,
                       ParticleFacing::Ground, VfxRenderer::Ground,
                       VfxPrimitive::Rune, 0.025f, source.scale.x * 0.72f,
                       {0.25f, 0.85f, 1.8f, 0.28f}, 1);
            add_status(StatusVisual::Mark, impl_->config.mark_status_sprite,
                       ParticleFacing::Ground, VfxRenderer::Ground,
                       VfxPrimitive::Rune, 0.025f,
                       source.scale.x * 0.42f, {2.1f, 1.0f, 0.15f, 0.26f}, 1);
        }
        for (const auto &visual : snapshot.persistent_vfx)
        {
            // Boss areas use authored direct outputs rather than a legacy emitter.
            if (visual.kind == PersistentVfxKind::BossAreaActive ||
                visual.kind == PersistentVfxKind::ProjectileHead ||
                visual.kind == PersistentVfxKind::BossDashWarning ||
                visual.kind == PersistentVfxKind::BossVolleyWarning ||
                visual.kind == PersistentVfxKind::RangedEnemyWarning ||
                visual.kind == PersistentVfxKind::MiniBombWarning ||
                visual.kind == PersistentVfxKind::EnemySpawnWarning ||
                visual.kind == PersistentVfxKind::BossSpawnWarning ||
                visual.kind == PersistentVfxKind::BossAreaWarning ||
                visual.kind == PersistentVfxKind::SuicideEnemyWarning ||
                visual.kind == PersistentVfxKind::BossShockwaveWavefront ||
                visual.kind == PersistentVfxKind::BossShockwaveWarning ||
                visual.kind == PersistentVfxKind::RicochetLink ||
                visual.kind == PersistentVfxKind::BurnTransferLink ||
                visual.kind == PersistentVfxKind::RelicChainLink) continue;
            // The authored head recipe owns the shared history. Do not add the
            // compatibility straight segment on top of the same projectile.
            if ((visual.kind == PersistentVfxKind::ProjectileTrail ||
                 visual.kind == PersistentVfxKind::ProjectileTrailOuter ||
                 visual.kind == PersistentVfxKind::RicochetProjectileTrail) &&
                ribbon_projectile_ids.contains(visual.projectile_owner_id)) continue;
            if ((visual.kind == PersistentVfxKind::DamageTrail ||
                 visual.kind == PersistentVfxKind::ChargeGuide) &&
                renderer_detail::HasVfxGroundCoverage(typed_ground_coverage,
                    visual.stable_id, static_cast<std::uint8_t>(visual.kind),
                    VfxGroundShape::LineBorder)) continue;
            const bool authored_exact_ring =
                impl_->config.slime_family_preview_count == 0 &&
                renderer_detail::HasVfxGroundCoverage(typed_ground_coverage,
                    visual.stable_id, static_cast<std::uint8_t>(visual.kind),
                    VfxGroundShape::Circle, VfxPrimitive::ExactRing);
            auto position = visual.position;
            auto visual_yaw = visual.yaw;
            if (interpolation < 1.0f)
            {
                if (const auto *previous = renderer_detail::FindPreviousVfxVisual(
                        previous_by_identity, visual.stable_id, visual.kind))
                {
                    const auto &prior = *previous;
                    position.x = std::lerp(prior.position.x, position.x, interpolation);
                    position.y = std::lerp(prior.position.y, position.y, interpolation);
                    position.z = std::lerp(prior.position.z, position.z, interpolation);
                    const auto yaw_delta = std::remainder(visual_yaw - prior.yaw,
                                                          2.0f * std::numbers::pi_v<float>);
                    visual_yaw = prior.yaw + yaw_delta * interpolation;
                }
            }
            ParticleSpawnCommand command;
            command.sequence = visual.stable_id ^ stateful_target;
            command.tick = stateful_target;
            command.position = position;
            command.shape = ParticleShape::Point;
            command.velocity_mode = ParticleVelocity::Direction;
            command.facing = ParticleFacing::Ground;
            command.renderer = VfxRenderer::Ground;
            command.direction = {0.0f, 1.0f, 0.0f};
            command.lifetime_min = command.lifetime_max = 2.0f / 60.0f;
            command.start_size_min = command.start_size_max = visual.radius;
            command.end_size_min = command.end_size_max = visual.radius;
            command.rotation_min = command.rotation_max = visual_yaw;
            switch (visual.kind)
            {
            case PersistentVfxKind::TrapPending:
                command.primitive = VfxPrimitive::Cracks;
                command.start_color = command.end_color = {0.45f, 0.8f, 1.3f, 0.18f};
                break;
            case PersistentVfxKind::TrapArmed:
                command.primitive = VfxPrimitive::Rune;
                command.start_color = command.end_color = {1.55f, 0.8f, 0.16f, 0.24f};
                break;
            case PersistentVfxKind::FireArea:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {3.2f, 0.62f, 0.035f, 0.34f};
                break;
            case PersistentVfxKind::SlowArea:
                command.primitive = VfxPrimitive::Rune;
                command.start_color = command.end_color = {0.2f, 0.75f, 1.65f, 0.16f};
                break;
            case PersistentVfxKind::ArrowRainArea:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {1.55f, 1.05f, 0.28f, 0.18f};
                break;
            case PersistentVfxKind::RangeIndicator:
                command.primitive = VfxPrimitive::Ring;
                command.start_color = command.end_color = {0.2f, 1.45f, 0.45f, 0.3f};
                break;
            case PersistentVfxKind::DamageTrail:
            case PersistentVfxKind::ChargeGuide:
            case PersistentVfxKind::ProjectileTrail:
            case PersistentVfxKind::ProjectileTrailOuter:
            case PersistentVfxKind::RicochetProjectileTrail:
            {
                command.renderer = VfxRenderer::Segment;
                command.primitive = visual.kind == PersistentVfxKind::ProjectileTrail
                                        ? VfxPrimitive::DashWake
                                        : VfxPrimitive::SolidTrail;
                const auto direction = Float3{std::sin(visual_yaw), 0.0f,
                                              std::cos(visual_yaw)};
                command.direction = direction;
                // Segment orientation is carried through the particle's initial velocity.
                command.speed_min = command.speed_max = 0.001f;
                command.rotation_min = command.rotation_max = 0.0f;
                command.start_size_min = command.start_size_max = visual.radius;
                command.end_size_min = command.end_size_max = visual.radius;
                command.stretch = visual.length / std::max(visual.radius * 2.0f, 0.001f);
                if (visual.kind == PersistentVfxKind::ChargeGuide)
                    command.start_color = command.end_color =
                        {0.25f, 1.2f, 2.4f, 0.28f};
                else if (visual.kind == PersistentVfxKind::ProjectileTrail)
                {
                    command.start_color = {0.65f, 2.2f, 3.6f, 0.58f};
                    command.end_color = {0.1f, 0.45f, 1.2f, 0.0f};
                }
                else if (visual.kind == PersistentVfxKind::ProjectileTrailOuter)
                {
                    command.start_color = {0.3f, 1.1f, 2.1f, 0.18f};
                    command.end_color = {0.08f, 0.25f, 0.7f, 0.0f};
                }
                else if (visual.kind == PersistentVfxKind::RicochetProjectileTrail)
                {
                    command.start_color = {1.4f, 0.5f, 0.08f, 0.36f};
                    command.end_color = {0.35f, 0.1f, 0.02f, 0.0f};
                }
                else
                    command.start_color = command.end_color =
                        {0.35f, 1.0f, 1.8f, 0.16f};
                break;
            }
            default:
                // New authored owners have no legacy primitive; never emit the default white ground quad.
                continue;
            }
            if (visual.kind == PersistentVfxKind::ChargeGuide)
            {
                constexpr auto border_radius = 0.035f;
                const auto direction = Float3{std::sin(visual_yaw), 0.0f,
                                              std::cos(visual_yaw)};
                const auto right = Float3{direction.z, 0.0f, -direction.x};
                const auto border_offset = std::max(visual.radius - border_radius, 0.0f);
                command.primitive = VfxPrimitive::SolidTrail;
                command.start_size_min = command.start_size_max = border_radius;
                command.end_size_min = command.end_size_max = border_radius;
                command.stretch = visual.length / (border_radius * 2.0f);
                command.start_color = command.end_color = {0.25f, 1.2f, 2.4f, 0.28f};
                for (const auto side : {-1.0f, 1.0f})
                {
                    auto border = command;
                    border.sequence ^= side < 0.0f ? 0xD1B54A32D192ED03ull
                                                   : 0x94D049BB133111EBull;
                    border.seed = static_cast<std::uint32_t>(border.sequence);
                    border.position.x += right.x * border_offset * side;
                    border.position.z += right.z * border_offset * side;
                    border.count = 1;
                    frame_particle_spawns.push_back(border);
                }
                const auto cap_length = visual.radius * 2.0f;
                for (const auto side : {-1.0f, 1.0f})
                {
                    auto cap = command;
                    cap.sequence ^= side < 0.0f ? 0x4CF5AD432745937Full
                                                : 0x8A5CD789635D2DFFull;
                    cap.seed = static_cast<std::uint32_t>(cap.sequence);
                    cap.position.x += direction.x * visual.length * 0.5f * side;
                    cap.position.z += direction.z * visual.length * 0.5f * side;
                    cap.direction = right;
                    cap.rotation_min = cap.rotation_max =
                        visual_yaw + std::numbers::pi_v<float> * 0.5f;
                    cap.stretch = cap_length / (border_radius * 2.0f);
                    cap.count = 1;
                    frame_particle_spawns.push_back(cap);
                }
                continue;
            }
            if (visual.kind == PersistentVfxKind::DamageTrail)
            {
                const auto direction = Float3{std::sin(visual_yaw), 0.0f,
                                              std::cos(visual_yaw)};
                const auto right = Float3{direction.z, 0.0f, -direction.x};
                constexpr auto border_radius = 0.035f;
                const auto border_offset = std::max(visual.radius - border_radius, 0.0f);
                command.start_size_min = command.start_size_max = border_radius;
                command.end_size_min = command.end_size_max = border_radius;
                command.stretch = visual.length / (border_radius * 2.0f);
                command.start_color = command.end_color = {0.35f, 1.0f, 1.8f, 0.24f};
                for (const auto side : {-1.0f, 1.0f})
                {
                    auto border = command;
                    border.sequence ^= side < 0.0f ? 0xD1B54A32D192ED03ull
                                                   : 0x94D049BB133111EBull;
                    border.seed = static_cast<std::uint32_t>(border.sequence);
                    border.position.x += right.x * border_offset * side;
                    border.position.z += right.z * border_offset * side;
                    border.count = 1;
                    frame_particle_spawns.push_back(border);
                }
                auto streak = command;
                streak.sequence ^= 0x9E3779B97F4A7C15ull;
                streak.seed = static_cast<std::uint32_t>(streak.sequence);
                streak.primitive = VfxPrimitive::DashedRicochet;
                streak.start_size_min = streak.start_size_max = 0.025f;
                streak.end_size_min = streak.end_size_max = 0.025f;
                streak.stretch = visual.length / 0.05f;
                streak.start_color = streak.end_color = {0.2f, 0.75f, 1.5f, 0.10f};
                streak.count = 1;
                frame_particle_spawns.push_back(streak);
                continue;
            }
            if (visual.kind == PersistentVfxKind::SlowArea)
            {
                command.count = 1;
                command.seed = static_cast<std::uint32_t>(command.sequence);
                if (!authored_exact_ring) frame_particle_spawns.push_back(command);
                constexpr auto notch_count = 4;
                for (auto index = 0; index < notch_count; ++index)
                {
                    const auto angle = static_cast<float>(index) *
                                       (2.0f * std::numbers::pi_v<float> / notch_count);
                    auto notch = command;
                    notch.sequence ^= 0x9E3779B97F4A7C15ull +
                                      static_cast<std::uint64_t>(index);
                    notch.seed = static_cast<std::uint32_t>(notch.sequence);
                    notch.primitive = VfxPrimitive::Chevron;
                    notch.position.x += std::sin(angle) * visual.radius * 0.52f;
                    notch.position.z += std::cos(angle) * visual.radius * 0.52f;
                    notch.rotation_min = notch.rotation_max = angle + std::numbers::pi_v<float>;
                    notch.start_size_min = notch.start_size_max = visual.radius * 0.12f;
                    notch.end_size_min = notch.end_size_max = visual.radius * 0.12f;
                    notch.count = 1;
                    frame_particle_spawns.push_back(notch);
                }
                continue;
            }
            command.count = 1;
            command.seed = static_cast<std::uint32_t>(command.sequence);
            if (!authored_exact_ring ||
                (visual.kind != PersistentVfxKind::FireArea &&
                 visual.kind != PersistentVfxKind::ArrowRainArea &&
                 visual.kind != PersistentVfxKind::RangeIndicator))
                frame_particle_spawns.push_back(command);
            if (visual.kind == PersistentVfxKind::ArrowRainArea)
            {
                constexpr std::array marks{
                    Float2{-0.34f, -0.18f}, Float2{0.22f, -0.30f},
                    Float2{0.0f, 0.0f}, Float2{-0.18f, 0.32f},
                    Float2{0.36f, 0.20f}};
                for (std::size_t index = 0; index < marks.size(); ++index)
                {
                    auto mark = command;
                    mark.sequence ^= 0x9E3779B97F4A7C15ull + index;
                    mark.seed = static_cast<std::uint32_t>(mark.sequence);
                    mark.position.x += marks[index].x * visual.radius;
                    mark.position.z += marks[index].y * visual.radius;
                    mark.primitive = VfxPrimitive::Arrow;
                    mark.start_size_min = mark.start_size_max = visual.radius * 0.11f;
                    mark.end_size_min = mark.end_size_max = visual.radius * 0.11f;
                    mark.start_color = mark.end_color = {1.3f, 0.85f, 0.2f, 0.09f};
                    frame_particle_spawns.push_back(mark);
                }
                continue;
            }
            if (visual.kind == PersistentVfxKind::RangeIndicator)
            {
                constexpr auto mark_count = 6;
                for (auto index = 0; index < mark_count; ++index)
                {
                    const auto angle = static_cast<float>(index) *
                                       (2.0f * std::numbers::pi_v<float> / mark_count);
                    auto mark = command;
                    mark.sequence ^= 0xD1B54A32D192ED03ull +
                                     static_cast<std::uint64_t>(index);
                    mark.seed = static_cast<std::uint32_t>(mark.sequence);
                    mark.position.x += std::sin(angle) * visual.radius * 0.58f;
                    mark.position.z += std::cos(angle) * visual.radius * 0.58f;
                    mark.rotation_min = mark.rotation_max = angle + std::numbers::pi_v<float>;
                    mark.primitive = VfxPrimitive::Chevron;
                    mark.start_size_min = mark.start_size_max = visual.radius * 0.10f;
                    mark.end_size_min = mark.end_size_max = visual.radius * 0.10f;
                    mark.start_color = mark.end_color = {0.18f, 0.8f, 0.35f, 0.10f};
                    frame_particle_spawns.push_back(mark);
                }
                continue;
            }
            if (visual.kind == PersistentVfxKind::FireArea)
            {
                auto interior = command;
                interior.sequence ^= 0x9E3779B97F4A7C15ull;
                interior.seed = static_cast<std::uint32_t>(interior.sequence);
                interior.start_size_min = interior.start_size_max = visual.radius * 0.78f;
                interior.end_size_min = interior.end_size_max = visual.radius * 0.78f;
                interior.primitive = VfxPrimitive::Flame;
                interior.start_color = interior.end_color =
                    {2.2f, 0.42f, 0.025f, 0.11f};
                frame_particle_spawns.push_back(interior);
            }
        }
    }
    for (const auto &line : effect_lines)
    {
        if (std::ranges::any_of(typed_ribbons.sources, [&](const VfxRibbonSourceInput &source) {
            return source.analytic && source.owner_id == line.sequence;
        })) continue;
        const auto dx = line.end.x - line.start.x;
        const auto dz = line.end.z - line.start.z;
        const auto length = std::hypot(dx, dz);
        if (length <= 0.0001f) continue;
        ParticleSpawnCommand command;
        command.sequence = line.sequence;
        command.tick = line.tick;
        command.position = {(line.start.x + line.end.x) * 0.5f, 0.035f,
                            (line.start.z + line.end.z) * 0.5f};
        command.shape = ParticleShape::Line;
        command.velocity_mode = ParticleVelocity::Direction;
        command.facing = ParticleFacing::Ground;
        command.renderer = VfxRenderer::Segment;
        command.primitive = line.primitive;
        command.sprite = line.sprite;
        command.frame_columns = line.frame_columns;
        command.frame_rows = line.frame_rows;
        command.shape_extent = {length * 0.5f, 0.0f, 0.0f};
        command.direction = {dx / length, 0.0f, dz / length};
        command.speed_min = command.speed_max = 0.001f;
        command.lifetime_min = command.lifetime_max = line.lifetime;
        command.start_color = command.end_color = line.color;
        command.start_size_min = command.start_size_max = line.width * 2.5f;
        command.end_size_min = command.end_size_max = line.width * 2.5f;
        command.stretch = length / std::max(line.width * 5.0f, 0.001f);
        command.rotation_min = command.rotation_max = 0.0f;
        command.uv_repeat = line.uv_repeat;
        command.scroll_speed = line.scroll_speed;
        command.count = 1;
        command.seed = static_cast<std::uint32_t>(line.sequence);
        frame_particle_spawns.push_back(command);
    }
    cpu_timings.particle_spawn_prepare_us = capture_elapsed(particle_spawn_prepare_start);
    if (render_instances.size() >
        (std::numeric_limits<std::size_t>::max() -
         kInstanceDataOffset - sizeof(GpuSceneTableHeader) - 255u - entity_surface_frame.Bytes().size()) /
            kGpuSceneSlotPayloadBytes)
        return Result::Failure(
            ErrorCode::InvalidArgument, "hs_renderer_d3d12",
            "Dynamic split GPU scene staging size overflow.");
    const auto dynamic_scene_staging_capacity =
        sizeof(GpuSceneTableHeader) +
        kGpuSceneSlotPayloadBytes * render_instances.size() + entity_surface_frame.Bytes().size();
    const auto particle_spawn_data_offset =
        (kInstanceDataOffset + dynamic_scene_staging_capacity + 255u) &
        ~std::size_t{255u};
    const auto particle_owner_data_offset =
        (particle_spawn_data_offset + sizeof(GpuParticleSpawnCommand) *
             std::max<std::size_t>(frame_particle_spawns.size(), 1) + 255u) & ~std::size_t{255u};
    const auto ground_ring_data_offset =
        (particle_owner_data_offset + sizeof(std::uint32_t) * kParticleCount + 255u) &
        ~std::size_t{255u};
    const auto flash_data_offset =
        (ground_ring_data_offset + sizeof(GpuParticleSpawnCommand) *
             std::max<std::size_t>(typed_ground_spawns.size(), 1) + 255u) &
        ~std::size_t{255u};
    if (typed_ground_spawns.size() > std::numeric_limits<UINT>::max() ||
        std::max<std::size_t>(typed_ground_spawns.size(), 1) >
            (std::numeric_limits<std::size_t>::max() - ground_ring_data_offset) /
                sizeof(GpuParticleSpawnCommand))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Too many authored ground rings for frame upload.");
    if (typed_flash_spawns.size() > std::numeric_limits<UINT>::max() ||
        std::max<std::size_t>(typed_flash_spawns.size(), 1) >
            (std::numeric_limits<std::size_t>::max() - flash_data_offset) /
                sizeof(GpuParticleSpawnCommand))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Too many authored flashes for frame upload.");
    const auto owner_mesh_data_offset = (flash_data_offset +
        sizeof(GpuParticleSpawnCommand) * std::max<std::size_t>(typed_flash_spawns.size(), 1) + 255u) & ~std::size_t{255u};
    if (typed_mesh_spawns.size() > std::numeric_limits<UINT>::max() ||
        std::max<std::size_t>(typed_mesh_spawns.size(), 1) >
            (std::numeric_limits<std::size_t>::max() - owner_mesh_data_offset) / sizeof(GpuParticleSpawnCommand))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Too many owner mesh commands.");
    const auto ribbon_update_offset = (owner_mesh_data_offset +
        sizeof(GpuParticleSpawnCommand) * std::max<std::size_t>(typed_mesh_spawns.size(), 1) + 255u) & ~std::size_t{255u};
    const auto ribbon_output_offset = (ribbon_update_offset +
        sizeof(GpuRibbonSourceUpdate) * std::max<std::size_t>(ribbon_updates.size(), 1) + 255u) & ~std::size_t{255u};
    const auto ground_gap_offset = (ribbon_output_offset +
        sizeof(GpuRibbonOutput) * std::max<std::size_t>(ribbon_outputs.size(), 1) + 255u) & ~std::size_t{255u};
    std::size_t ground_gap_count{};
    for (const auto &spawn : typed_ground_spawns)
    {
        if (spawn.geometry.GapCount() > std::numeric_limits<UINT>::max() - ground_gap_count)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Ground gap index overflow.");
        ground_gap_count += spawn.geometry.GapCount();
    }
    for (const auto &distortion : typed_distortions)
    {
        if (distortion.shape != VfxDistortionShape::GappedAnnulus) continue;
        if (distortion.GapCount() > std::numeric_limits<UINT>::max() - ground_gap_count)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Distortion gap index overflow.");
        ground_gap_count += distortion.GapCount();
    }
    if (std::max<std::size_t>(ground_gap_count, 1) > (std::numeric_limits<std::size_t>::max() - ground_gap_offset) / sizeof(float))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Ground gap upload overflow.");
    struct alignas(16) GpuDistortion
    {
        DirectX::XMFLOAT4 position_radius, direction_strength, phase_alpha_shape_seed, geometry;
        DirectX::XMUINT4 gap_range;
    };
    static_assert(sizeof(GpuDistortion) == 80);
    constexpr std::uint32_t kMaxDistortions = 32;
    const auto distortion_data_offset = (ground_gap_offset + sizeof(float) * std::max<std::size_t>(ground_gap_count, 1) + 255u) & ~std::size_t{255u};
    struct alignas(16) GpuDecal
    {
        DirectX::XMFLOAT4 position_radius, axis_alpha_age, color_hdr, fracture, slab;
        DirectX::XMUINT4 metadata;
    };
    static_assert(sizeof(GpuDecal) == 96);
    constexpr std::uint32_t kMaxDecals = VfxDecalCandidates::kCapacity;
    const auto decal_data_offset = (distortion_data_offset + sizeof(GpuDistortion) * kMaxDistortions + 255u) & ~std::size_t{255u};
    constexpr std::size_t kMaxFresnelShells = 32;
    if (typed_fresnels.size() > kMaxFresnelShells)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Too many Fresnel shell commands.");
    const auto fresnel_data_offset =
        (decal_data_offset + sizeof(GpuDecal) * kMaxDecals + 255u) & ~std::size_t{255u};
    if (static_draw_refs.size() > std::numeric_limits<UINT>::max() ||
        static_draw_refs.size() > std::numeric_limits<std::size_t>::max() /
            sizeof(GpuStaticDrawRef))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Static draw reference overflow.");
    const auto dynamic_ref_data_offset =
        (fresnel_data_offset + sizeof(GpuFresnelShell) * kMaxFresnelShells + 255u) &
        ~std::size_t{255u};
    if (render_instances.size() > std::numeric_limits<UINT>::max() ||
        render_instances.size() >
            (std::numeric_limits<std::size_t>::max() - dynamic_ref_data_offset) /
                sizeof(GpuStaticDrawRef))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Dynamic draw reference overflow.");
    const auto visibility_range_offset =
        (dynamic_ref_data_offset +
         sizeof(GpuStaticDrawRef) * std::max<std::size_t>(render_instances.size(), 1) +
         255u) & ~std::size_t{255u};
    const auto dynamic_visibility_range_offset =
        visibility_range_offset + sizeof(visibility_ranges);
    const auto retest_visibility_range_offset =
        dynamic_visibility_range_offset + sizeof(dynamic_visibility_ranges);
    const auto archer_palette_data_offset =
        (retest_visibility_range_offset + sizeof(retest_visibility_ranges) + 255u) &
        ~std::size_t{255u};
    const auto archer_instance_count = static_cast<std::size_t>(std::ranges::count_if(
        render_instances, [](const RenderInstance &instance) {
            return instance.mesh == RenderMesh::Archer;
        }));
    if ((archer_instance_count != 0 && impl_->archer_bone_count == 0) ||
        impl_->archer_bone_count > kMaxCharacterBones ||
        impl_->archer_bone_count > std::numeric_limits<std::size_t>::max() /
            sizeof(DirectX::XMFLOAT4X4))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Archer skeleton exceeds the frame palette ABI.");
    const auto archer_palette_stride =
        static_cast<std::size_t>(impl_->archer_bone_count) * sizeof(DirectX::XMFLOAT4X4);
    if (archer_instance_count > 0 &&
        archer_palette_stride > std::numeric_limits<std::size_t>::max() /
            archer_instance_count)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Archer palette upload size overflow.");
    const auto archer_palette_bytes = archer_palette_stride * archer_instance_count;
    if (archer_palette_bytes > std::numeric_limits<std::size_t>::max() - 255u ||
        archer_palette_data_offset > std::numeric_limits<std::size_t>::max() -
            archer_palette_bytes - 255u)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Archer palette upload range overflow.");
    const auto view_skin_data_offset =
        (archer_palette_data_offset + archer_palette_bytes + 255u) & ~std::size_t{255u};
    // Slot growth can append at most one slot per input before reconciliation below.
    const auto view_skin_slot_capacity = std::max<std::size_t>(1,
        impl_->dynamic_scene_slots.SlotCount() + render_instances.size());
    if (view_skin_slot_capacity > UINT32_MAX / sizeof(GpuSceneSkinEntry) ||
        view_skin_data_offset > std::numeric_limits<std::size_t>::max() -
            view_skin_slot_capacity * sizeof(GpuSceneSkinEntry))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "View skin upload range exceeds its byte-address ABI.");
    const auto required_upload_size = view_skin_data_offset +
        view_skin_slot_capacity * sizeof(GpuSceneSkinEntry);
    if (required_upload_size > frame.upload_size)
    {
        std::size_t new_size = frame.upload_size;
        while (new_size < required_upload_size) new_size *= 2;
        frame.upload.resource->Unmap(0, nullptr);
        frame.mapped = nullptr;
        frame.upload.Reset();
        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto result = impl_->CreateAllocation(
                frame.upload, upload_allocation, BufferDescription(new_size),
                D3D12_RESOURCE_STATE_GENERIC_READ);
            !result)
        {
            return result;
        }
        D3D12_RANGE no_read{};
        const auto mapped = frame.upload.resource->Map(
            0, &no_read, reinterpret_cast<void **>(&frame.mapped));
        if (FAILED(mapped)) return HResultFailure("Map grown frame upload", mapped);
        frame.upload_size = new_size;
    }
    if (!ribbon_updates.empty()) std::memcpy(frame.mapped + ribbon_update_offset, ribbon_updates.data(), ribbon_updates.size_bytes());
    if (!ribbon_outputs.empty()) std::memcpy(frame.mapped + ribbon_output_offset, ribbon_outputs.data(), ribbon_outputs.size_bytes());
    // Trailing metadata record consumed only by HZB lookup. The preceding
    // bucket records and their 32-byte stride remain unchanged. Target/DRS
    // changes invalidate previous HZB before this producer layout is reused.
    const GpuStaticVisibilityRange hzb_metadata{
        owned_view.targets.hzb_layout.levels[0].width, owned_view.targets.hzb_layout.levels[0].height,
        owned_view.targets.hzb_layout.count,
        owned_view.targets.hzb_layout.elements <= UINT32_MAX
            ? static_cast<std::uint32_t>(owned_view.targets.hzb_layout.elements) : 0u};
    visibility_ranges.back() = hzb_metadata;
    dynamic_visibility_ranges.back() = hzb_metadata;
    retest_visibility_ranges.back() = hzb_metadata;
    std::memcpy(frame.mapped + visibility_range_offset, visibility_ranges.data(),
                sizeof(visibility_ranges));
    std::memcpy(frame.mapped + dynamic_visibility_range_offset,
                dynamic_visibility_ranges.data(),
                sizeof(dynamic_visibility_ranges));
    std::memcpy(frame.mapped + retest_visibility_range_offset,
                retest_visibility_ranges.data(), sizeof(retest_visibility_ranges));
    D3D12MA::ALLOCATION_DESC visibility_allocation{};
    visibility_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (!frame.static_visible_refs.resource ||
        frame.static_visible_capacity < visible_ref_capacity)
    {
        frame.static_visible_refs.Reset();
        frame.static_visible_capacity = visible_ref_capacity;
        if (auto created = impl_->CreateAllocation(
                frame.static_visible_refs, visibility_allocation,
                BufferDescription(frame.static_visible_capacity * sizeof(GpuStaticDrawRef),
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS); !created)
            return created;
    }
    constexpr auto visibility_argument_bytes = kGpuVisibilityBucketCount *
        kGpuVisibilityViewCount * kGeometryArgumentStride;
    if (!frame.static_draw_args.resource ||
        frame.static_draw_args.resource->GetDesc().Width < visibility_argument_bytes)
    {
        frame.static_draw_args.Reset();
        if (auto created = impl_->CreateAllocation(
                frame.static_draw_args, visibility_allocation,
                BufferDescription(visibility_argument_bytes,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS); !created)
            return created;
    }
    if (!frame.dynamic_visible_refs.resource ||
        frame.dynamic_visible_capacity < dynamic_visible_ref_capacity)
    {
        frame.dynamic_visible_refs.Reset();
        // Match the existing heap granularity so one extra ref does not replace the bank or its graph shape.
        constexpr auto refs_per_allocation = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT / sizeof(GpuStaticDrawRef);
        frame.dynamic_visible_capacity = std::min<std::size_t>(UINT32_MAX,
            (dynamic_visible_ref_capacity + refs_per_allocation - 1) / refs_per_allocation * refs_per_allocation);
        if (auto created = impl_->CreateAllocation(
                frame.dynamic_visible_refs, visibility_allocation,
                BufferDescription(
                    frame.dynamic_visible_capacity * sizeof(GpuStaticDrawRef),
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            !created)
            return created;
    }
    if (!frame.dynamic_draw_args.resource ||
        frame.dynamic_draw_args.resource->GetDesc().Width < visibility_argument_bytes)
    {
        frame.dynamic_draw_args.Reset();
        if (auto created = impl_->CreateAllocation(
                frame.dynamic_draw_args, visibility_allocation,
                BufferDescription(visibility_argument_bytes,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            !created)
            return created;
    }
    const auto retest_ref_capacity = std::max<std::size_t>(visible_ref_count, 1);
    const auto ensure_visibility_refs =
        [&](AllocationResource &resource, std::size_t &capacity) -> Result {
            if (resource.resource && capacity >= retest_ref_capacity)
                return Result::Success();
            resource.Reset();
            capacity = retest_ref_capacity;
            return impl_->CreateAllocation(
                resource, visibility_allocation,
                BufferDescription(capacity * sizeof(GpuStaticDrawRef),
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
    if (auto created = ensure_visibility_refs(
            frame.static_rejected_refs, frame.static_rejected_capacity);
        !created)
        return created;
    if (auto created = ensure_visibility_refs(
            frame.static_retest_refs, frame.static_retest_capacity);
        !created)
        return created;
    constexpr auto retest_argument_bytes =
        kGpuVisibilityBucketCount * kGeometryArgumentStride;
    const auto ensure_visibility_args = [&](AllocationResource &resource) -> Result {
        if (resource.resource && resource.resource->GetDesc().Width >= retest_argument_bytes)
            return Result::Success();
        resource.Reset();
        return impl_->CreateAllocation(
            resource, visibility_allocation,
            BufferDescription(retest_argument_bytes,
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    if (auto created = ensure_visibility_args(frame.static_rejected_args); !created)
        return created;
    if (auto created = ensure_visibility_args(frame.static_retest_args); !created)
        return created;
    std::uint8_t debug_command{};
    std::uint64_t debug_value{};
    std::uint32_t debug_secondary{};
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.devtools_visible)
    {
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(430.0f, 720.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Project HS DevTools");
    ImGui::Text("Tick: %llu", static_cast<unsigned long long>(snapshot.header.tick));
    ImGui::Text("Checksum: %llu",
                static_cast<unsigned long long>(snapshot.header.checksum));
    ImGui::Text("Instances: %zu", snapshot.instances.size());
    ImGui::Text("Render entities: %zu", snapshot.instances.size());
    ImGui::Text("UI models: %zu", snapshot.ui.size());
    ImGui::Text("GPU particles: %u / %u", impl_->last_particle_count,
                kParticleCount * std::clamp(impl_->config.particle_percentage, 50u, 100u) /
                    100u);
    D3D12MA::Budget local_budget{};
    impl_->allocator->GetBudget(&local_budget, nullptr);
    ImGui::Text("Video memory: %.1f / %.1f MiB",
                static_cast<double>(local_budget.UsageBytes) / (1024.0 * 1024.0),
                static_cast<double>(local_budget.BudgetBytes) / (1024.0 * 1024.0));
    ImGui::Text("Threads: Main / Simulation / Render + %u workers",
                devtools.worker_count);
    ImGui::Text("Queues I/P/V/G/D: %u/%u/%u/%u/%u",
                devtools.input_queue_depth, devtools.presentation_queue_depth,
                devtools.particle_queue_depth, devtools.resize_queue_depth,
                devtools.debug_queue_depth);
    ImGui::Text("Dropped I/P/V: %llu/%llu/%llu",
                static_cast<unsigned long long>(devtools.dropped_input),
                static_cast<unsigned long long>(devtools.dropped_presentation),
                static_cast<unsigned long long>(devtools.dropped_particles));
    ImGui::SeparatorText("Character animation inspection");
    ImGui::Checkbox("Close-up preview", &impl_->config.character_preview);
    if (impl_->config.character_preview)
    {
        ImGui::SliderFloat("Camera distance", &impl_->preview_distance, 2.5f, 8.0f);
        ImGui::SliderFloat("Camera yaw", &impl_->preview_yaw, 0.0f, 360.0f);
        ImGui::SliderFloat("Camera pitch", &impl_->preview_pitch, -20.0f, 60.0f);
        ImGui::Checkbox("Override pose", &impl_->preview_pose_override);
        if (impl_->preview_pose_override)
        {
            constexpr const char *clips = "Idle\0Run\0Draw\0Recoil\0Death\0";
            ImGui::Combo("Base clip", &impl_->preview_base_clip, clips);
            ImGui::SliderFloat("Base time", &impl_->preview_base_time, 0.0f, 1.0f);
            ImGui::Combo("Blend clip", &impl_->preview_secondary_clip, clips);
            ImGui::SliderFloat("Blend time", &impl_->preview_secondary_time, 0.0f, 1.0f);
            ImGui::SliderFloat("Blend weight", &impl_->preview_secondary_weight, 0.0f, 1.0f);
            ImGui::Combo("Upper clip", &impl_->preview_upper_clip, clips);
            ImGui::SliderFloat("Upper time", &impl_->preview_upper_time, 0.0f, 1.0f);
            ImGui::SliderFloat("Upper weight", &impl_->preview_upper_weight, 0.0f, 1.0f);
        }
    }
    ImGui::SeparatorText("Simulation");
    if (ImGui::Button("Start Session")) debug_command = 1;
    if (ImGui::Button("Pause / Resume")) debug_command = 15;
    if (ImGui::Button("Grant 100 XP")) { debug_command = 6; debug_value = 100; }
    if (ImGui::Button("Damage Player 10")) { debug_command = 3; debug_value = 10; }
    ImGui::SameLine();
    if (ImGui::Button("Heal Player 10")) { debug_command = 4; debug_value = 10; }
    if (ImGui::Button("Damage Final Boss 1000")) { debug_command = 5; debug_value = 1000; }
    if (ImGui::Button("Spawn Melee")) { debug_command = 10; debug_value = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Ranged")) { debug_command = 10; debug_value = 1; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Suicide")) { debug_command = 10; debug_value = 2; }
    if (ImGui::Button("Growth 5m")) { debug_command = 2; debug_value = 18'000; }
    ImGui::SameLine();
    if (ImGui::Button("Growth 10m")) { debug_command = 2; debug_value = 36'000; }
    ImGui::SameLine();
    if (ImGui::Button("Growth 15m")) { debug_command = 2; debug_value = 54'000; }
    if (ImGui::Button("Spawn 5m Boss")) { debug_command = 11; debug_value = 0; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn 10m Boss")) { debug_command = 11; debug_value = 1; }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Final Boss")) { debug_command = 11; debug_value = 2; }

    static int skill{};
    static int upgrade{};
    static int relic{};
    static int stat{};
    ImGui::SeparatorText("Build controls");
    ImGui::Combo("Skill", &skill, kDebugSkillNames.data(),
                 static_cast<int>(kDebugSkillNames.size()));
    if (ImGui::Button("Grant Skill")) { debug_command = 7; debug_value = skill + 1; }
    ImGui::Combo("Upgrade", &upgrade, kDebugUpgradeNames[skill].data(),
                 static_cast<int>(kDebugUpgradeNames[skill].size()));
    if (ImGui::Button("Grant Upgrade")) {
        debug_command = 8;
        debug_value = skill + 1;
        debug_secondary = static_cast<std::uint32_t>(upgrade);
    }
    ImGui::Combo("Relic", &relic, kDebugRelicNames.data(),
                 static_cast<int>(kDebugRelicNames.size()));
    if (ImGui::Button("Grant Relic")) { debug_command = 9; debug_value = relic; }
    ImGui::Combo("Stat", &stat, kDebugStatNames.data(),
                 static_cast<int>(kDebugStatNames.size()));
    if (ImGui::Button("Assign Stat")) { debug_command = 13; debug_value = stat; }
    ImGui::SameLine();
    if (ImGui::Button("Reroll")) debug_command = 14;
    ImGui::SeparatorText("RenderGraph");
    for (std::uint32_t index = 0; index < impl_->graph.PassCount(); ++index)
    {
        const auto pass = impl_->graph.CompiledPassName(index);
        ImGui::BulletText("%.*s", static_cast<int>(pass.size()), pass.data());
    }
    ImGui::End();
    ImGui::Render();
    }
#endif
#if defined(HS_DEVELOPMENT_TOOLS)
    auto *const imgui_draw_data = impl_->config.devtools_visible ? ImGui::GetDrawData() : nullptr;
#endif
    cpu_timings.scene_prepare_us = measure_stage();
    const auto ui_content = impl_->explicit_frame->ui_draw_content;
    const auto ui_models = ui_content ? std::span<const UiModel>(ui_content->models) : snapshot.ui;
    if (auto result = impl_->RasterizeUi(owned_view, frame_slot, ui_models, ui_content); !result)
    {
        return result;
    }
    cpu_timings.ui_us = measure_stage();
    const auto instance_count = render_instances.size();

    auto *constants = reinterpret_cast<FrameConstants *>(frame.mapped);
    auto &next_draw_instances = frame.next_draw_instances_scratch;
    if (reconcile_packet_scene) next_draw_instances.resize(instance_count);
    else next_draw_instances.clear();
    auto *instances = next_draw_instances.data();
    auto *gpu_particle_spawns =
        reinterpret_cast<GpuParticleSpawnCommand *>(frame.mapped + particle_spawn_data_offset);
    auto *gpu_particle_owners =
        reinterpret_cast<std::uint32_t *>(frame.mapped + particle_owner_data_offset);
    const auto typed_vfx_upload_start = capture_clock();
    auto *gpu_ground_rings = reinterpret_cast<GpuParticleSpawnCommand *>(
        frame.mapped + ground_ring_data_offset);
    auto *gpu_flashes = reinterpret_cast<GpuParticleSpawnCommand *>(
        frame.mapped + flash_data_offset);
    auto *gpu_owner_meshes = reinterpret_cast<GpuParticleSpawnCommand *>(frame.mapped + owner_mesh_data_offset);
    auto *gpu_fresnels = reinterpret_cast<GpuFresnelShell *>(frame.mapped + fresnel_data_offset);
    struct FresnelDraw
    {
        std::size_t instance_index{};
        std::size_t asset_index{};
        std::size_t command_index{};
        bool archer{};
        std::uint16_t lod{};
        std::uint32_t geometry_pass{};
    };
    std::vector<FresnelDraw> fresnel_draws;
    fresnel_draws.reserve(typed_fresnels.size());
    // Immutable typed payloads were admitted by ValidateFramePayload before any view submission.
    for (std::size_t index = 0; index < typed_fresnels.size(); ++index)
    {
        const auto &source = typed_fresnels[index];
        const bool player = source.kind == VfxFresnelShellKind::PlayerInvulnerable;
        const auto *match = prepared_scene.instance_lookup.Find(source.render_instance_id);
        if (!match || !(player ? match->mesh == RenderMesh::Archer :
                match->mesh >= RenderMesh::BossFiveMinute && match->mesh <= RenderMesh::BossFinal)) continue;
        const auto instance_index = static_cast<std::size_t>(match - render_instances.data());
        if (instance_index >= ordered_monster_lods.size() || ordered_monster_lods[instance_index] >= 3u)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Fresnel owner has no valid selected LOD.");
        const auto lod = ordered_monster_lods[instance_index];
        const auto asset_index = player ? 0 : static_cast<std::size_t>(match->mesh) -
            static_cast<std::size_t>(RenderMesh::MonsterMelee);
        auto &target = gpu_fresnels[index];
        target.current_transform = source.current_transform;
        target.previous_transform = source.previous_transform;
        target.color = {source.color.x, source.color.y, source.color.z, source.color.w};
        target.geometry = {source.footprint_radius, source.source_progress,
                           source.elapsed_seconds, source.hdr};
        target.motion = {source.motion_rate_hz, source.motion_amplitude,
                         source.motion_inset_fraction, source.rotation_hz};
        target.signature = {source.fresnel_power, source.noise_amount, 0.0f, 0.0f};
        target.metadata = {static_cast<std::uint32_t>(source.kind), source.sector_count,
                           source.end_crack, source.gradient_row};
        target.identity = {static_cast<std::uint32_t>(source.render_instance_id),
                           static_cast<std::uint32_t>(source.render_instance_id >> 32),
                           static_cast<std::uint32_t>(source.stable_id),
                           static_cast<std::uint32_t>(source.stable_id >> 32)};
        std::uint32_t geometry_pass{};
        const GeometryPageGpu &page = player ? impl_->archer_geometry
            : lod == 0 ? static_cast<const GeometryPageGpu &>(impl_->monster_assets[asset_index])
            : impl_->monster_reduced_geometry[asset_index][lod - 1u];
        if (auto result = impl_->AddGeometryDrawRecord(frame_slot, page,
                static_cast<std::uint32_t>(instance_index), 1, kGeometryGpuSceneRefs, 0,
                static_cast<std::uint32_t>(render_instances[instance_index].mesh), geometry_pass); !result)
            return result;
        fresnel_draws.push_back({instance_index, asset_index, index, player, lod, geometry_pass});
    }
    std::uint32_t owner_mesh_count{};
    if (impl_->config.slime_family_preview_count == 0)
        for (const auto &source : typed_mesh_spawns)
        {
            auto &target_mesh = gpu_owner_meshes[owner_mesh_count++];
            target_mesh = {};
            target_mesh.position_lifetime_min = {
                std::lerp(source.previous_position.x, source.position.x, interpolation),
                std::lerp(source.previous_position.y, source.position.y, interpolation),
                std::lerp(source.previous_position.z, source.position.z, interpolation),0};
            target_mesh.direction_lifetime_max = {source.velocity.x,source.velocity.y,source.velocity.z,0};
            const float transverse = source.radius / impl_->vfx_mesh_transverse_radii[source.mesh_index-1];
            target_mesh.shape_extent_speed_min = {transverse,transverse,2.0f*source.radius,0};
            target_mesh.start_color = {source.color.x,source.color.y,source.color.z,source.color.w};
            target_mesh.size_range.y = source.hdr;
            target_mesh.rotation_range.x = source.fresnel;
            target_mesh.rotation_range.y = source.head_to_tail_ratio;
            target_mesh.rotation_range.z = source.shell_thickness;
            const auto bounds = impl_->vfx_mesh_longitudinal_bounds[source.mesh_index - 1];
            if (source.head_to_tail_ratio > 0 && (bounds.y <= bounds.x || bounds.y >= bounds.z))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Authored head ratio requires the actual interior widest mesh ring.");
            target_mesh.end_color = {bounds.x, bounds.y, bounds.z, 0};
            target_mesh.metadata.x = source.mesh_index;
            target_mesh.metadata.y = source.gradient_row;
        }
    std::uint32_t flash_count{};
    std::uint32_t flash_add_count{};
    for (const bool oit_group : {false, true})
    for (const auto &source : typed_flash_spawns)
    {
        if (source.oit != oit_group) continue;
        if (!source.oit) ++flash_add_count;
        auto &flash = gpu_flashes[flash_count++];
        flash = {};
        const float attachment_alpha = source.interpolate_owner_position ? interpolation : 1.0f;
        // Avoid touching unused burst history (including caller sentinel values).
        const auto previous_position = source.interpolate_owner_position ? source.previous_position : source.position;
        flash.position_lifetime_min = {
            std::lerp(previous_position.x, source.position.x, attachment_alpha),
            std::lerp(previous_position.y, source.position.y, attachment_alpha),
            std::lerp(previous_position.z, source.position.z, attachment_alpha),
            source.ground_base_anchor ? 1.0f : 0.0f};
        flash.start_color = {source.color.x, source.color.y,
                             source.color.z, source.color.w};
        flash.size_range.x = source.size;
        flash.size_range.y = source.hdr;
        flash.size_range.z = source.normalized_age;
        flash.metadata.y = source.gradient_row;
        flash.metadata.x = static_cast<std::uint32_t>(source.shape);
        flash.metadata.z = source.curve_row;
        flash.metadata.w = source.mask_slice;
        flash.direction_lifetime_max = {source.direction.x, source.direction.y, source.direction.z, source.lifetime};
        flash.shape_extent_speed_min = {source.aspect, source.mask_strength, static_cast<float>(source.stable_seed & 65535u), source.opacity_scale};
        flash.end_color = {source.initial_offset.x, source.initial_offset.y, source.initial_offset.z, source.drag};
        flash.speed_cone_gravity_stretch = {source.initial_velocity.x, source.initial_velocity.y, source.initial_velocity.z, source.orbit_radius};
        flash.rotation_range = {source.orbit_phase, source.orbit_rate, source.domain_warp,
            source.shape == VfxImpactShape::Flame ? source.procedural_flow_speed : source.motion_strength};
        flash.modes = {source.fbm_octaves, source.smoke_first_frame, source.smoke_frame_count, std::bit_cast<std::uint32_t>(source.smoke_fps)};
    }
    std::uint32_t ground_ring_count{};
    std::uint32_t ground_add_count{};
    auto *gpu_ground_gaps = reinterpret_cast<float *>(frame.mapped + ground_gap_offset);
    gpu_ground_gaps[0] = 0.0f;
    std::uint32_t gap_write_index{};
    if (impl_->config.slime_family_preview_count == 0)
    {
        for (const auto &input : typed_ground_spawns)
        {
            const auto &source = input.command;
            const auto &geometry = input.geometry;
            const bool line = geometry.shape == VfxGroundShape::LineBorder ||
                              geometry.shape == VfxGroundShape::LineHatch;
            const bool cone = geometry.shape == VfxGroundShape::ConeBorder ||
                              geometry.shape == VfxGroundShape::ConeHatch;
            const bool safe_sector = geometry.shape == VfxGroundShape::SafeSectorMarker;
            const bool hex = geometry.shape == VfxGroundShape::HexConstellation;
            const bool cross_ring = geometry.shape == VfxGroundShape::CrossRing;
            const bool broken_hex = geometry.shape == VfxGroundShape::BrokenHex;
            const bool axial_fracture = geometry.shape == VfxGroundShape::AxialFracture;
            const bool repeating_chevron = geometry.shape == VfxGroundShape::RepeatingChevron;
            const bool broken_crown = geometry.shape == VfxGroundShape::BrokenCrown;
            const bool closed_crown = geometry.shape == VfxGroundShape::ClosedCrownRing;
            const bool state_ring = geometry.shape == VfxGroundShape::StateRing;
            const bool polar_rune = geometry.shape == VfxGroundShape::PolarRune;
            const bool authored_additive_shape = cross_ring || broken_hex || axial_fracture ||
                repeating_chevron || broken_crown || closed_crown;
            const bool circle_preview = geometry.shape == VfxGroundShape::CirclePreviewBorder || geometry.shape == VfxGroundShape::CirclePreviewTicks;
            const bool preview = circle_preview || geometry.shape == VfxGroundShape::RingGapsPreviewBorder || geometry.shape == VfxGroundShape::RingGapsTicks;
            const bool annulus = preview || geometry.shape == VfxGroundShape::RingGapsBorder || geometry.shape == VfxGroundShape::RingGapsFill;
            const bool boss_signature = input.additive && geometry.animation_phase == 1.0f &&
                (geometry.shape == VfxGroundShape::Circle ||
                 geometry.shape == VfxGroundShape::ConeBorder ||
                 geometry.shape == VfxGroundShape::RingGapsBorder);
            const float authored_radius = geometry.outer_radius;
            const float authored_edge = geometry.edge_width;
            // OIT commands grow from the front; additive rings grow from the
            // back of the same upload allocation.
            auto &gpu_ring = input.additive
                ? gpu_ground_rings[typed_ground_spawns.size() - ++ground_add_count]
                : gpu_ground_rings[ground_ring_count++];
            gpu_ring = {};
            gpu_ring.position_lifetime_min = {source.position.x, source.position.y,
                                              source.position.z, 0.0f};
            gpu_ring.speed_cone_gravity_stretch.w = source.stretch;
            gpu_ring.start_color = {source.start_color.x, source.start_color.y,
                                    source.start_color.z, source.start_color.w};
            gpu_ring.size_range.x = source.start_size_min;
            gpu_ring.size_range.y = input.hdr;
            gpu_ring.metadata.x = static_cast<std::uint32_t>(source.primitive);
            gpu_ring.metadata.y = input.gradient_row;
            gpu_ring.rotation_range = {input.motion.rate_hz, input.motion.amplitude,
                                       input.motion.inset_fraction, 0.0f};
            if (line || cone)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.direction_lifetime_max = {geometry.direction.x, 0.0f, geometry.direction.z, 0.0f};
                gpu_ring.shape_extent_speed_min = {cone ? geometry.range : geometry.half_width,
                                                  cone ? geometry.half_angle_radians : geometry.half_length,
                                                  geometry.edge_width, 0.0f};
                gpu_ring.speed_cone_gravity_stretch = {geometry.spacing, geometry.scroll, 0.0f, 0.0f};
            }
            if (safe_sector)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.direction_lifetime_max = {geometry.direction.x, 0, geometry.direction.z, 0};
                gpu_ring.shape_extent_speed_min = {geometry.inner_radius, geometry.outer_radius, geometry.edge_width, geometry.half_angle_radians};
                gpu_ring.end_color = {0, geometry.animation_phase, 0, 0};
                gpu_ring.modes.x = geometry.spokes;
                gpu_ring.modes.y = geometry.rings;
            }
            if (hex)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {0.0f, geometry.outer_radius,
                                                   geometry.edge_width, geometry.mask_strength};
                gpu_ring.end_color = {geometry.progress, geometry.animation_phase, 0.0f, 0.0f};
                gpu_ring.modes.x = geometry.spokes;
                gpu_ring.modes.y = geometry.mask_slice;
            }
            if (state_ring)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {geometry.inner_radius,
                    geometry.outer_radius, geometry.edge_width, 0.0f};
                gpu_ring.end_color = {geometry.progress, geometry.animation_phase, 0.0f, 0.0f};
                gpu_ring.modes.x = geometry.spokes;
                gpu_ring.modes.y = geometry.rings;
            }
            if (polar_rune)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {geometry.inner_radius,
                    geometry.outer_radius, geometry.edge_width, 0.0f};
                gpu_ring.end_color = {geometry.progress, geometry.animation_phase, 0.0f, 0.0f};
                gpu_ring.modes.x = geometry.spokes;
                gpu_ring.modes.y = geometry.rings;
            }
            if (authored_additive_shape)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {
                    broken_crown ? geometry.inner_radius :
                        (axial_fracture ? authored_radius :
                         repeating_chevron ? geometry.half_width : 0.0f),
                    axial_fracture ? geometry.range :
                        (repeating_chevron ? geometry.half_length : authored_radius),
                    authored_edge,
                    broken_hex ? geometry.gap_half_angle_radians :
                        (repeating_chevron ? geometry.spacing : 0.0f)};
                gpu_ring.end_color = {geometry.progress, geometry.animation_phase, 0.0f, 0.0f};
                if (axial_fracture || repeating_chevron)
                    gpu_ring.direction_lifetime_max = {geometry.direction.x, 0.0f,
                                                         geometry.direction.z, 0.0f};
                if (repeating_chevron) gpu_ring.modes.x = geometry.spokes;
            }
            if (annulus)
            {
                gpu_ring.metadata.z = static_cast<std::uint32_t>(geometry.shape);
                gpu_ring.shape_extent_speed_min = {geometry.inner_radius, geometry.outer_radius,
                    geometry.edge_width, geometry.gap_half_angle_radians};
                if (preview) gpu_ring.end_color = {geometry.progress, geometry.animation_phase,
                    std::bit_cast<float>(geometry.tick_count), 0.0f};
                gpu_ring.modes.x = gap_write_index;
                gpu_ring.modes.y = static_cast<std::uint32_t>(geometry.GapCount());
                for (std::uint32_t index = 0; index < geometry.GapCount(); ++index)
                    gpu_ground_gaps[gap_write_index++] = geometry.GapAngleRadians(
                        index, impl_->explicit_frame->vfx.ground_gap_angles);
            }
            if (boss_signature)
            {
                gpu_ring.end_color = {geometry.progress, 1.0f, 0.0f, 0.0f};
                if (geometry.shape == VfxGroundShape::Circle)
                    gpu_ring.speed_cone_gravity_stretch.w =
                        geometry.outer_radius / source.start_size_min;
            }
        }
    }
    const auto ground_quad_end = std::partition(gpu_ground_rings, gpu_ground_rings + ground_ring_count,
        [](const GpuParticleSpawnCommand &command) {
            return !((command.metadata.z == 5u || command.metadata.z == 6u) &&
                command.shape_extent_speed_min.x > command.shape_extent_speed_min.y * 0.45f);
        });
    const auto ground_quad_count = static_cast<std::uint32_t>(ground_quad_end - gpu_ground_rings);
    constexpr auto particle_capacity = kParticleCount;
    cpu_timings.typed_vfx_upload_prepare_us = capture_elapsed(typed_vfx_upload_start);
    const auto particle_upload_start = capture_clock();
    std::uint32_t gpu_particle_spawn_count{};
    std::uint32_t total_particles_to_spawn{};
    for (const auto &source : frame_particle_spawns)
    {
        // Public commands were admitted once; local commands keep mesh_index=0 and authored_ballistic=false.
        if (source.authored_ballistic && (stateful_target < source.authored_birth_tick ||
            (stateful_target == source.authored_birth_tick && source.authored_birth_fraction > 0))) continue;
        if (total_particles_to_spawn == particle_capacity)
        {
            break;
        }
        const auto age = source.authored_ballistic
            ? (static_cast<float>(stateful_target - source.authored_birth_tick) - source.authored_birth_fraction) / 60.0f
            : static_cast<float>(std::max(0.0, native_display_tick - static_cast<double>(source.tick)) / 60.0);
        if (age >= source.lifetime_max || source.count == 0)
        {
            continue;
        }
        const auto count = std::min(source.count, particle_capacity - total_particles_to_spawn);
        auto &target_spawn = gpu_particle_spawns[gpu_particle_spawn_count++];
        target_spawn = {};
        if (source.authored_ballistic)
        {
            target_spawn.current_velocity_drag = {source.authored_initial_velocity.x, source.authored_initial_velocity.y, source.authored_initial_velocity.z, source.authored_drag};
            target_spawn.collision_material = {source.authored_bounce, source.authored_ground_y, source.authored_collision_radius, source.authored_hdr};
            target_spawn.authored = {1u, std::bit_cast<std::uint32_t>(source.authored_birth_fraction), static_cast<std::uint32_t>(source.authored_birth_tick), source.authored_gradient_row};
        }
        target_spawn.position_lifetime_min = {source.position.x, source.position.y,
                                              source.position.z, source.lifetime_min};
        target_spawn.direction_lifetime_max = {source.direction.x, source.direction.y,
                                               source.direction.z, source.lifetime_max};
        target_spawn.shape_extent_speed_min = {source.shape_extent.x, source.shape_extent.y,
                                               source.shape_extent.z, source.speed_min};
        target_spawn.speed_cone_gravity_stretch = {source.speed_max, source.cone_radians,
                                                   source.gravity, source.stretch};
        target_spawn.start_color = {source.start_color.x, source.start_color.y,
                                    source.start_color.z, source.start_color.w};
        target_spawn.end_color = {source.end_color.x, source.end_color.y,
                                  source.end_color.z, source.end_color.w};
        target_spawn.size_range = {source.start_size_min, source.start_size_max,
                                   source.end_size_min, source.end_size_max};
        target_spawn.rotation_range = source.renderer == VfxRenderer::Segment
            ? DirectX::XMFLOAT4{source.uv_repeat, source.uv_repeat,
                                source.scroll_speed, source.scroll_speed}
            : DirectX::XMFLOAT4{source.rotation_min, source.rotation_max,
                                source.angular_velocity_min,
                                source.angular_velocity_max};
        const auto sprite_metadata = static_cast<std::uint32_t>(source.sprite) |
                                     (static_cast<std::uint32_t>(source.frame_columns) << 16u) |
                                     (static_cast<std::uint32_t>(source.frame_rows) << 24u);
        const auto visual_metadata = static_cast<std::uint32_t>(source.facing) |
                                     (static_cast<std::uint32_t>(source.renderer) << 8u) |
                                     (static_cast<std::uint32_t>(source.primitive) << 16u) |
                                     (static_cast<std::uint32_t>(source.mesh_index) << 24u);
        target_spawn.modes = {static_cast<std::uint32_t>(source.shape),
                              static_cast<std::uint32_t>(source.velocity_mode),
                              visual_metadata,
                              sprite_metadata};
        target_spawn.metadata = {count, source.seed, total_particles_to_spawn,
                                 std::bit_cast<std::uint32_t>(age)};
        std::fill_n(gpu_particle_owners + total_particles_to_spawn, count,
                    gpu_particle_spawn_count - 1);
        total_particles_to_spawn += count;
    }
    cpu_timings.particle_upload_prepare_us = capture_elapsed(particle_upload_start);
    const auto particle_delta_ticks =
        impl_->particles_initialized && stateful_target > impl_->last_particle_tick
            ? stateful_target - impl_->last_particle_tick
            : 0;
    const auto view =
        DirectX::XMMatrixLookAtLH(eye, target, camera_up);
    const bool grid_aa_disabled = impl_->config.grid_floor_probe &&
                                  impl_->config.grid_floor_disable_aa;
    const bool temporal_enabled = impl_->config.temporal_reconstruction &&
                                  !grid_aa_disabled;
    const bool use_sr_reconstruction = temporal_enabled && owned_view.reconstruction_provider &&
                          owned_view.reconstruction_provider->Initialized();
    const bool use_native_reconstruction = use_sr_reconstruction;
    const auto projection = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees),
        static_cast<float>(owned_view.render_width) / static_cast<float>(owned_view.render_height), far_plane,
        near_plane);
    const auto halton = [](std::uint64_t index, std::uint32_t base) noexcept {
        float value = 0.0f;
        float scale = 1.0f / static_cast<float>(base);
        while (index != 0)
        {
            value += static_cast<float>(index % base) * scale;
            index /= base;
            scale /= static_cast<float>(base);
        }
        return value;
    };
    auto jitter_sequence_length = std::uint64_t{8};
    if (use_sr_reconstruction && owned_view.reconstruction_provider->ProviderId() == "xess")
    {
        // Pinned XeSS-SR guide: at least 8 * (targetWidth / inputWidth)^2
        // samples, derived from the actual active extent rather than a preset.
        const auto scale = static_cast<double>(owned_view.width) / owned_view.render_width;
        jitter_sequence_length = static_cast<std::uint64_t>(std::max(8.0, std::ceil(8.0 * scale * scale)));
    }
    // Each view advances only after its successful real render/presentation. History resets
    // keep the same sampling phase convention and reset the native history.
    const auto jitter_index = 1u + owned_view.frame_number % jitter_sequence_length;
    auto jitter_x = temporal_enabled
        ? (halton(jitter_index, 2) - 0.5f) * 2.0f /
              static_cast<float>(owned_view.render_width)
        : 0.0f;
    auto jitter_y = temporal_enabled
        ? (0.5f - halton(jitter_index, 3)) * 2.0f /
              static_cast<float>(owned_view.render_height)
        : 0.0f;
    if (use_sr_reconstruction)
    {
        std::optional<DirectX::XMFLOAT2> native_jitter;
        if (auto queried = owned_view.reconstruction_provider->NativeJitter(owned_view.state.render_active,
                owned_view.frame_number, native_jitter); !queried) return queried;
        if (native_jitter)
        {
            jitter_x = native_jitter->x * 2.0f / owned_view.render_width;
            jitter_y = -native_jitter->y * 2.0f / owned_view.render_height;
        }
    }
    const auto clip_matrix = view * projection *
        DirectX::XMMatrixTranslation(jitter_x, jitter_y, 0.0f);
    DirectX::XMStoreFloat4x4(&constants->view_projection,
                             DirectX::XMMatrixTranspose(clip_matrix));
    const auto previous_scene_shift = owned_view.previous_origins_valid
        ? DirectX::XMMatrixTranslation(static_cast<float>(view_input.scene_origin.x - owned_view.previous_scene_origin.x),
            static_cast<float>(view_input.scene_origin.y - owned_view.previous_scene_origin.y),
            static_cast<float>(view_input.scene_origin.z - owned_view.previous_scene_origin.z))
        : DirectX::XMMatrixIdentity();
    const auto previous_clip = previous_scene_shift *
        DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&owned_view.previous_view_projection));
    DirectX::XMStoreFloat4x4(&constants->previous_view_projection,
                             (owned_view.temporal_history_valid || owned_view.hzb_history_valid)
                                 ? DirectX::XMMatrixTranspose(previous_clip)
                                 : DirectX::XMMatrixTranspose(clip_matrix));
    DirectX::XMFLOAT3 current_temporal_eye{};
    DirectX::XMStoreFloat3(&current_temporal_eye, eye);
    const auto camera_delta = std::hypot(
        double(current_temporal_eye.x) - owned_view.temporal_previous_eye.x +
            (view_input.scene_origin.x - owned_view.previous_scene_origin.x),
        double(current_temporal_eye.y) - owned_view.temporal_previous_eye.y +
            (view_input.scene_origin.y - owned_view.previous_scene_origin.y),
        double(current_temporal_eye.z) - owned_view.temporal_previous_eye.z +
            (view_input.scene_origin.z - owned_view.previous_scene_origin.z));
    if ((owned_view.temporal_history_valid || owned_view.hzb_history_valid) &&
        (camera_delta > std::max(2.0f, camera_distance * 0.25f) ||
         std::abs(snapshot.camera.vertical_fov_degrees - owned_view.temporal_previous_fov) > 1.0f))
    {
        owned_view.temporal_history_valid = false;
        owned_view.hzb_history_valid = false;
    }
    constants->temporal_options = {temporal_enabled ? 1.0f : 0.0f,
                                   owned_view.temporal_history_valid ? 1.0f : 0.0f,
                                   far_plane, near_plane};
    const auto current_view_origin = view_input.view_origin;
    const DirectX::XMFLOAT3 view_origin_scene{
        static_cast<float>(current_view_origin.x - view_input.scene_origin.x),
        static_cast<float>(current_view_origin.y - view_input.scene_origin.y),
        static_cast<float>(current_view_origin.z - view_input.scene_origin.z)};
    const auto view_origin_vector = DirectX::XMLoadFloat3(&view_origin_scene);
    DirectX::XMStoreFloat4x4(&constants->inverse_view_projection,
        DirectX::XMMatrixInverse(nullptr, DirectX::XMMatrixTranslationFromVector(view_origin_vector) * clip_matrix));
    // Existing volume/AO services use the actual eye as their relative
    // basis. Keep that ABI separate from the explicit Surface ViewOrigin.
    DirectX::XMFLOAT4X4 camera_relative_inverse_view_projection;
    DirectX::XMStoreFloat4x4(&camera_relative_inverse_view_projection,
        DirectX::XMMatrixInverse(nullptr, DirectX::XMMatrixTranslationFromVector(eye) * clip_matrix));
    DirectX::XMStoreFloat4x4(&constants->current_unjittered_view_projection, view * projection);
    if (owned_view.temporal_history_valid || owned_view.hzb_history_valid)
        DirectX::XMStoreFloat4x4(&constants->previous_unjittered_view_projection,
            previous_scene_shift * DirectX::XMLoadFloat4x4(&owned_view.previous_unjittered_view_projection));
    else constants->previous_unjittered_view_projection = constants->current_unjittered_view_projection;
    constants->previous_instance_unjittered_view_projection = previous_instance_snapshot
        ? owned_view.previous_unjittered_view_projection : constants->current_unjittered_view_projection;
    constants->surface_view_origin = {view_origin_scene.x, view_origin_scene.y,
        view_origin_scene.z, (owned_view.temporal_history_valid || owned_view.hzb_history_valid) ? 1.0f : 0.0f};
    constants->surface_jitter_uv = {jitter_x * 0.5f, -jitter_y * 0.5f,
        owned_view.temporal_history_valid ? owned_view.previous_jitter_uv.x : jitter_x * 0.5f,
        owned_view.temporal_history_valid ? owned_view.previous_jitter_uv.y : -jitter_y * 0.5f};
    // LOD is measured in output pixels, independent of temporal jitter and
    // internal render scale. View columns provide the world-to-view basis.
    DirectX::XMFLOAT4X4 lod_view{}, lod_projection{};
    DirectX::XMStoreFloat4x4(&lod_view, view);
    DirectX::XMStoreFloat4x4(&lod_projection, projection);
    constants->lod_view_right = {lod_view._11, lod_view._21, lod_view._31, 0};
    constants->lod_view_up = {lod_view._12, lod_view._22, lod_view._32, 0};
    constants->lod_view_forward = {lod_view._13, lod_view._23, lod_view._33, 0};
    const auto lod_shadow_pixels = static_cast<float>(std::clamp(impl_->config.shadow_resolution, 1024u, 2048u));
    const auto shadow_distance = impl_->config.shadow_distance_m;
    const std::array cascade_extents{shadow_distance * (18.0f / 160.0f),
                                    shadow_distance * (48.0f / 160.0f), shadow_distance};
    constants->lod_projection = {
        0.5f * std::max(owned_view.width * std::abs(lod_projection._11),
                        owned_view.height * std::abs(lod_projection._22)),
        lod_shadow_pixels / cascade_extents[0], lod_shadow_pixels / cascade_extents[1],
        lod_shadow_pixels / cascade_extents[2]};
    constants->lod_options = {impl_->QualityProfileFor(impl_->config.geometry_quality).geometry_screen_error_px,
                             0.15f, 0.1f, 0.125f};
    if (new_session) ++owned_view.lod_session_epoch;
    constants->lod_clock = {static_cast<UINT>(owned_view.frame_number + 1),
        owned_view.lod_session_epoch,
        (owned_view.temporal_history_valid || owned_view.hzb_history_valid) ? 1u : 0u,
        (impl_->config.grid_floor_probe ? 1u : 0u) | (grid_aa_disabled ? 2u : 0u)};
    static_assert(kAuthoredMaterialNames.size() <= 16);
    for (std::size_t group = 0; group < constants->material_parameter_bases.size(); ++group)
    {
        std::array<UINT, 4> bases{};
        for (std::size_t lane = 0; lane < bases.size(); ++lane)
            if (const auto ordinal = group * 4 + lane; ordinal < kAuthoredMaterialNames.size())
                bases[lane] = impl_->material_parameter_bases[ordinal];
        constants->material_parameter_bases[group] = {bases[0], bases[1], bases[2], bases[3]};
        std::array<UINT, 4> masks{};
        for (std::size_t lane = 0; lane < masks.size(); ++lane)
            if (const auto ordinal = group * 4 + lane; ordinal < kAuthoredMaterialNames.size())
                masks[lane] = impl_->material_surface_layouts[ordinal].varying_mask;
        constants->material_varying_masks[group] = {masks[0], masks[1], masks[2], masks[3]};
    }
    const auto &sidecar = impl_->material_sidecar_layout;
    for (std::size_t group = 0; group < constants->hero_material_parameter_bases.size(); ++group)
    {
        std::array<UINT, 4> bases{}, masks{};
        for (std::size_t lane = 0; lane < bases.size(); ++lane)
            if (const auto hero = group * 4 + lane; hero < kHeroMaterialNames.size())
            {
                const auto ordinal = kAuthoredMaterialNames.size() + hero;
                bases[lane] = impl_->material_parameter_bases[ordinal];
                masks[lane] = impl_->material_surface_layouts[ordinal].varying_mask;
            }
        constants->hero_material_parameter_bases[group] = {bases[0], bases[1], bases[2], bases[3]};
        constants->hero_material_varying_masks[group] = {masks[0], masks[1], masks[2], masks[3]};
    }
    constants->hero_binding_ranges = impl_->hero_binding_ranges;
    constants->probe_options = impl_->lighting_probe.FrameOptions();
    constants->material_sidecar_offsets[0] = {sidecar.offsets[0], sidecar.offsets[1],
                                             sidecar.offsets[2], sidecar.offsets[3]};
    constants->material_sidecar_offsets[1] = {sidecar.offsets[4], UINT32_MAX, UINT32_MAX, UINT32_MAX};
    constants->material_sidecar_info = {sidecar.mask, sidecar.word_count, sidecar.plane_count, 0};
    constants->output_size = {static_cast<float>(owned_view.width), static_cast<float>(owned_view.height),
                             1.0f / owned_view.width, 1.0f / owned_view.height};
    constants->post_options = {impl_->config.bloom_strength, impl_->config.outline_width_px,
                              impl_->config.color_grading ? 1.0f : 0.0f, impl_->config.sharpening};
    const bool pom_enabled = impl_->config.pom_quality != GraphicsOptionalQuality::Off;
    const auto pom_row = pom_enabled ? static_cast<GraphicsQuality>(static_cast<unsigned>(impl_->config.pom_quality) - 1u) : GraphicsQuality::Low;
    const auto &pom = impl_->QualityProfileFor(pom_row);
    const auto &material = impl_->QualityProfileFor(impl_->config.material_quality);
    // Z is bit-reinterpreted payload: positive binary16 fade start in the low
    // half, fade end in the high half. HLSL decodes both with f16tof32.
    const auto fade_bits = UINT(DirectX::PackedVector::XMConvertFloatToHalf(material.terrain_detail_fade_start_m)) |
        (UINT(DirectX::PackedVector::XMConvertFloatToHalf(material.terrain_detail_fade_end_m)) << 16u);
    constants->material_quality_options = {pom_enabled ? float(pom.pom_max_steps) : 0.0f, pom_enabled ? float(pom.pom_refinement_steps) : 0.0f,
        std::bit_cast<float>(fade_bits), static_cast<float>(impl_->config.refraction_quality)};
    constants->display_options = {owned_view.display_state.hdr_active ? 1.0f : 0.0f,
        owned_view.display_state.paper_white_nits, owned_view.display_state.peak_nits, 0};
#if defined(HS_DEVELOPMENT_TOOLS)
    constants->display_options.w = imgui_draw_data && !impl_->sdk_presentation.IsEnabled() ? 1.0f : 0.0f;
#endif
    DirectX::XMStoreFloat4x4(&constants->inverse_unjittered_view_projection,
                            DirectX::XMMatrixInverse(nullptr, view * projection));
    ExposureConstants exposure_constants;
    // Keep the daylight art target after adaptation instead of lifting dark terrain back to 18% grey.
    exposure_constants.target_grey = 0.12f;
    exposure_constants.width = owned_view.render_width;
    exposure_constants.height = owned_view.render_height;
    exposure_constants.delta_seconds = owned_view.exposure_sample_time.time_since_epoch().count() == 0
        ? 1.0f / 60.0f : std::chrono::duration<float>(now - owned_view.exposure_sample_time).count();
    if (impl_->explicit_frame->delta_seconds) exposure_constants.delta_seconds = *impl_->explicit_frame->delta_seconds;
    exposure_constants.reset = new_session || view_input.camera_cut ? 1u : 0u;
    std::memcpy(frame.mapped + kExposureConstantsOffset, &exposure_constants, sizeof(exposure_constants));
    const auto previous_sdk_camera = owned_view.sdk_camera;
    auto &sdk_camera = owned_view.sdk_camera;
    DirectX::XMStoreFloat4x4(&sdk_camera.view, view);
    DirectX::XMStoreFloat4x4(&sdk_camera.projection, projection);
    DirectX::XMStoreFloat4x4(&sdk_camera.inverse_view, DirectX::XMMatrixInverse(nullptr, view));
    DirectX::XMStoreFloat4x4(&sdk_camera.inverse_projection, DirectX::XMMatrixInverse(nullptr, projection));
    sdk_camera.current_unjittered_view_projection = constants->current_unjittered_view_projection;
    sdk_camera.previous_unjittered_view_projection = constants->previous_unjittered_view_projection;
    sdk_camera.position = current_temporal_eye;
    sdk_camera.right = {lod_view._11, lod_view._21, lod_view._31};
    sdk_camera.up = {lod_view._12, lod_view._22, lod_view._32};
    sdk_camera.forward = {lod_view._13, lod_view._23, lod_view._33};
    sdk_camera.reset_history = !owned_view.temporal_history_valid || new_session;
        if (sdk_camera.reset_history) sdk_camera.previous_view = sdk_camera.view;
    else DirectX::XMStoreFloat4x4(&sdk_camera.previous_view,
        previous_scene_shift * DirectX::XMLoadFloat4x4(&previous_sdk_camera.view));
    sdk_camera.previous_projection = sdk_camera.reset_history ? sdk_camera.projection : previous_sdk_camera.projection;
    sdk_camera.previous_position = sdk_camera.reset_history ? sdk_camera.position : DirectX::XMFLOAT3{
        static_cast<float>(double(previous_sdk_camera.position.x) + (owned_view.previous_scene_origin.x - view_input.scene_origin.x)),
        static_cast<float>(double(previous_sdk_camera.position.y) + (owned_view.previous_scene_origin.y - view_input.scene_origin.y)),
        static_cast<float>(double(previous_sdk_camera.position.z) + (owned_view.previous_scene_origin.z - view_input.scene_origin.z))};
    sdk_camera.near_plane = near_plane; sdk_camera.far_plane = far_plane;
    sdk_camera.vertical_fov_radians = DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees);
    sdk_camera.delta_seconds = exposure_constants.delta_seconds;
    sdk_camera.jitter_pixels[0] = constants->surface_jitter_uv.x * owned_view.render_width;
    sdk_camera.jitter_pixels[1] = constants->surface_jitter_uv.y * owned_view.render_height;
    sdk_camera.reversed_depth = true;
    if (!presented_view || !impl_->native_input_phase)
        owned_view.sdk_frame_identity = {impl_->active_render_package_id, impl_->active_render_package_id, impl_->active_native_viewport_id};
    DirectX::XMStoreFloat4(&constants->camera_time, eye);
    constants->camera_time.w =
        static_cast<float>(snapshot.header.simulation_time.count()) / 1'000'000'000.0f;
    std::vector<LightView> owned_frame_lights;
    std::span<const LightView> frame_lights = snapshot.lights;
    if (frame_lights.empty())
    {
        LightView sun{{-0.4f, -0.8f, 0.3f}, 3.0f * kLightWhiteLumensPerWatt, {1, 1, 1}};
        sun.stable_id = 1;
        sun.shadow_flags = kLightShadowRequested;
        owned_frame_lights.push_back(sun);
        frame_lights = owned_frame_lights;
    }
    const auto authoritative_light_count = frame_lights.size();
    std::vector<std::uint64_t> transient_light_ids;
    std::optional<LightView> selected_primary_sun;
    for (const auto &candidate : frame_lights)
        if (candidate.type == LightType::Directional && (candidate.shadow_flags & kLightShadowRequested) != 0 &&
            (!selected_primary_sun || candidate.stable_id < selected_primary_sun->stable_id)) selected_primary_sun = candidate;
    const auto *primary_sun = selected_primary_sun ? std::addressof(*selected_primary_sun) : nullptr;
    const LightView light = primary_sun ? *primary_sun : LightView{{-0.4f, -0.8f, 0.3f}, 0, {1, 1, 1}};
    constants->light_direction_intensity = {light.direction.x, light.direction.y,
                                             light.direction.z, light.quantity / kLightWhiteLumensPerWatt};
    constants->light_color = {light.color.x, light.color.y, light.color.z, 1.0f};
    constants->screen_size = {
        static_cast<float>(owned_view.render_width), static_cast<float>(owned_view.render_height),
        1.0f / static_cast<float>(owned_view.render_width),
        1.0f / static_cast<float>(owned_view.render_height)};
    DirectX::XMFLOAT3 camera_forward;
    DirectX::XMStoreFloat3(&camera_forward, direction);
    constants->camera_forward_softness = {camera_forward.x, camera_forward.y,
                                          camera_forward.z, 0.35f};
    // Numeric VFX budgets use the current four-tier graphics intent. Individual
    // output min-quality admission remains in the typed command adapters.
    const auto local_vfx_prepare_start = capture_clock();
    const auto vfx_graphics_quality = impl_->last_applied_graphics_request_available
        ? impl_->last_applied_graphics_request.particle_quality : impl_->config.requested_graphics
            ? impl_->config.requested_graphics->particle_quality : impl_->config.vfx_quality == VfxQuality::Low
                ? GraphicsQuality::Low : impl_->config.vfx_quality == VfxQuality::Medium
                    ? GraphicsQuality::Medium : GraphicsQuality::High;
    const auto &vfx_quality_profile = impl_->QualityProfileFor(vfx_graphics_quality);
    const float vfx_tangent_y = std::tan(DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees) * .5f);
    const float vfx_tangent_x = vfx_tangent_y * static_cast<float>(owned_view.render_width) / owned_view.render_height;
    const float vfx_plane_x_scale = std::sqrt(1.0f + vfx_tangent_x * vfx_tangent_x);
    const float vfx_plane_y_scale = std::sqrt(1.0f + vfx_tangent_y * vfx_tangent_y);
    const auto vfx_light_limit = static_cast<UINT>(std::floor(impl_->config.vfx_light_count *
        vfx_quality_profile.vfx_light_fraction));
    const auto vfx_distortion_limit = static_cast<UINT>(std::floor(kMaxDistortions *
        vfx_quality_profile.vfx_distortion_fraction));
    const auto vfx_distortion_coverage_limit = impl_->config.distortion_coverage *
        vfx_quality_profile.vfx_distortion_fraction;
    const auto vfx_decal_limit = static_cast<UINT>(std::floor(kMaxDecals *
        vfx_quality_profile.vfx_decal_fraction));
    struct LightCandidate { const VfxLightInput *light; float screen_area; };
    std::vector<LightCandidate> light_candidates;
    for (const auto &source : typed_lights)
    {
        if (vfx_light_limit == 0 || source.intensity == 0 || impl_->config.slime_family_preview_count != 0) continue;
        const auto world = DirectX::XMVectorSet(source.position.x, source.position.y, source.position.z, 1);
        DirectX::XMFLOAT3 view_position;
        DirectX::XMStoreFloat3(&view_position, DirectX::XMVector3TransformCoord(world, view));
        bool on_screen = view_position.z + source.radius > near_plane && view_position.z - source.radius < far_plane;
        const float distance = std::max(view_position.z, near_plane);
        // Sphere distance to each slanted frustum plane, not a box at center depth.
        if (std::abs(view_position.x) - view_position.z * vfx_tangent_x > source.radius * vfx_plane_x_scale ||
            std::abs(view_position.y) - view_position.z * vfx_tangent_y > source.radius * vfx_plane_y_scale) on_screen = false;
        light_candidates.push_back({&source, on_screen ? std::min(1.0f, source.radius * source.radius / (distance * distance)) : 0.0f});
    }
    std::sort(light_candidates.begin(), light_candidates.end(), [](const auto &a, const auto &b) {
        if (a.light->importance != b.light->importance) return a.light->importance > b.light->importance;
        if (a.screen_area != b.screen_area) return a.screen_area > b.screen_area;
        return a.light->stable_id < b.light->stable_id;
    });
    constants->vfx_light_count = {};
    for (const auto &candidate : light_candidates)
    {
        const auto &source = *candidate.light;
        const auto index = constants->vfx_light_count.x;
        if (index >= vfx_light_limit) break;
        constants->vfx_light_position_radius[index] = {source.position.x,source.position.y,source.position.z,source.radius};
        constants->vfx_light_color_intensity[index] = {source.linear_rgb.x,source.linear_rgb.y,source.linear_rgb.z,source.intensity};
        LightView point;
        point.stable_id = MakeAssetId("vfx/light/" + std::to_string(source.stable_id)).value;
        point.type = LightType::Point;
        point.unit = LightUnit::Lumen;
        point.position = source.position;
        point.range = source.radius;
        point.color = source.linear_rgb;
        point.quantity = source.intensity * (4.0f * DirectX::XM_PI * kLightWhiteLumensPerWatt);
        point.legacy_attenuation = true;
        point.revision = owned_view.frame_number + 1;
        if (owned_frame_lights.empty())
            owned_frame_lights.assign(frame_lights.begin(), frame_lights.end());
        transient_light_ids.push_back(point.stable_id);
        owned_frame_lights.push_back(point);
        ++constants->vfx_light_count.x;
    }
    if (!owned_frame_lights.empty()) frame_lights = owned_frame_lights;
    struct DistortionCandidate { const VfxDistortionInput *source; float area; };
    std::vector<DistortionCandidate> distortion_candidates;
    for (const auto &source : typed_distortions)
    {
        if (vfx_distortion_limit == 0 || source.alpha == 0 || source.strength == 0 || impl_->config.slime_family_preview_count != 0) continue;
        DirectX::XMFLOAT3 center;
        DirectX::XMStoreFloat3(&center, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(source.position.x,source.position.y,source.position.z,1),view));
        const float radius = source.radius;
        if (center.z + radius <= near_plane || center.z - radius >= far_plane) continue;
        if (std::abs(center.x) - center.z * vfx_tangent_x > radius * vfx_plane_x_scale ||
            std::abs(center.y) - center.z * vfx_tangent_y > radius * vfx_plane_y_scale) continue;
        const float distance = std::max(center.z - radius,near_plane);
        const float area = std::min(1.0f, 3.14159265f * radius * radius / (4 * distance * distance * vfx_tangent_x * vfx_tangent_y));
        if (area > vfx_distortion_coverage_limit) continue;
        distortion_candidates.push_back({&source,area});
    }
    std::stable_sort(distortion_candidates.begin(), distortion_candidates.end(), [](const auto &a, const auto &b) {
        if (a.source->importance != b.source->importance) return a.source->importance > b.source->importance;
        if (a.area != b.area) return a.area > b.area;
        if (a.source->stable_id != b.source->stable_id) return a.source->stable_id < b.source->stable_id;
        return a.source->stable_seed < b.source->stable_seed;
    });
    auto *gpu_distortions = reinterpret_cast<GpuDistortion *>(frame.mapped + distortion_data_offset);
    std::uint32_t distortion_count{};
    float distortion_area{};
    for (const auto &candidate : distortion_candidates)
    {
        if (distortion_count >= vfx_distortion_limit) break;
        const auto &source = *candidate.source;
        if (distortion_area + candidate.area > vfx_distortion_coverage_limit) continue;
        auto &gpu = gpu_distortions[distortion_count++];
        gpu.position_radius = {source.position.x,source.position.y,source.position.z,source.radius};
        gpu.direction_strength = {source.direction.x,source.direction.y,source.direction.z,source.strength};
        gpu.phase_alpha_shape_seed = {source.normalized_age,source.alpha,static_cast<float>(source.shape),static_cast<float>(source.stable_seed & 0xffffu)};
        gpu.geometry = {source.inner_radius,
                        DirectX::XMConvertToRadians(source.half_angle_degrees),
                        DirectX::XMConvertToRadians(source.gap_half_width_degrees),0.0f};
        gpu.gap_range = {gap_write_index,static_cast<UINT>(source.GapCount()),0,0};
        for (std::uint32_t index = 0; index < source.GapCount(); ++index)
            gpu_ground_gaps[gap_write_index++] = DirectX::XMConvertToRadians(
                source.GapAngleDegrees(index, impl_->explicit_frame->vfx.distortion_gap_angles));
        distortion_area += candidate.area;
    }
    VfxDecalCandidates decal_candidates(vfx_decal_limit);
    for (const auto &source : typed_decals)
    {
        if (vfx_decal_limit == 0 || source.alpha == 0 || impl_->config.slime_family_preview_count != 0) continue;
        // Ground-only projector slab follows its source anchor, including elevated death events.
        DirectX::XMFLOAT3 center;
        DirectX::XMStoreFloat3(&center,DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(source.position.x,source.position.y-.825f,source.position.z,1),view));
        const float radius = std::hypot(source.radius,1.175f);
        if (center.z+radius <= near_plane || center.z-radius >= far_plane ||
            std::abs(center.x)-center.z*vfx_tangent_x > radius*vfx_plane_x_scale ||
            std::abs(center.y)-center.z*vfx_tangent_y > radius*vfx_plane_y_scale) continue;
        const float distance = std::max(center.z,near_plane);
        decal_candidates.Add({&source,std::min(1.0f,source.radius*source.radius/(distance*distance))});
    }
    const auto selected_decals = decal_candidates.Sorted();
    auto *gpu_decals = reinterpret_cast<GpuDecal *>(frame.mapped+decal_data_offset);
    constants->vfx_decal_count = {};
    for(const auto &candidate:selected_decals)
    {
        const auto &source=*candidate.source;
        if(constants->vfx_decal_count.x>=vfx_decal_limit) break;
        auto &gpu=gpu_decals[constants->vfx_decal_count.x++];
        std::uint32_t hash=source.stable_seed*747796405u+2891336453u; hash=(hash^(hash>>16))*2246822519u;
        const float angle=std::atan2(source.direction.z,source.direction.x)+static_cast<float>(hash&0x00ffffffu)*(6.283185307f/16777216.0f);
        gpu.position_radius={source.position.x,source.position.y,source.position.z,source.radius};
        gpu.axis_alpha_age={std::cos(angle),std::sin(angle),source.alpha,source.normalized_age};
        gpu.color_hdr={source.linear_rgb.x,source.linear_rgb.y,source.linear_rgb.z,source.hdr};
        gpu.fracture={source.edge_glow,static_cast<float>(source.voronoi_cells),0,0};
        gpu.slab={source.position.y-2.0f,source.position.y+.35f,0,0};
        gpu.metadata={source.gradient_row,source.texture_slice,static_cast<std::uint32_t>(source.kind),source.stable_seed};
    }
    cpu_timings.local_vfx_prepare_us = capture_elapsed(local_vfx_prepare_start);
    bool cascades_prepared = false;
    const auto prepare_cascades = [&] {
        if (cascades_prepared) return;
        const auto &cascade_extent = cascade_extents;
        const auto base_shadow = std::clamp(impl_->config.shadow_resolution, 1024u, 2048u);
        constants->shadow_atlas_texel_size = {1.0f / float(base_shadow * 2u), 1.0f / float(base_shadow * 2u), float(base_shadow * 2u), float(impl_->QualityProfileFor(impl_->config.shadow_quality).shadow_filter_samples)};
        const auto light_direction = DirectX::XMVector3Normalize(DirectX::XMVectorSet(light.direction.x, light.direction.y, light.direction.z, 0.0f));
        const auto light_up = std::abs(DirectX::XMVectorGetY(light_direction)) > 0.99f
            ? DirectX::XMVectorSet(0, 0, 1, 0) : DirectX::XMVectorSet(0, 1, 0, 0);
        const auto light_view = DirectX::XMMatrixLookAtLH(
            DirectX::XMVectorScale(light_direction, -shadow_distance), DirectX::XMVectorZero(), light_up);
        for (std::size_t cascade = 0; cascade < 3; ++cascade)
        {
            const float resolution = float(base_shadow);
            const float extent = cascade_extent[cascade];
            const auto center = DirectX::XMVector3TransformCoord(target, light_view);
            const float texel = extent / resolution;
            const float cx = std::round(DirectX::XMVectorGetX(center) / texel) * texel;
            const float cy = std::round(DirectX::XMVectorGetY(center) / texel) * texel;
            const float cz = DirectX::XMVectorGetZ(center);
            const float half = extent * 0.5f;
            const auto projection = DirectX::XMMatrixOrthographicOffCenterLH(cx-half, cx+half, cy-half, cy+half,
                                                                          cz+shadow_distance, cz-shadow_distance);
            DirectX::XMStoreFloat4x4(&constants->shadow_view_projection[cascade], DirectX::XMMatrixTranspose(light_view * projection));
            constants->shadow_atlas_scale_offset[cascade] = {0.5f, 0.5f, cascade == 1 ? 0.5f : 0.0f, cascade == 2 ? 0.5f : 0.0f};
        }
        cascades_prepared = true;
    };
    for (std::size_t asset = 0; asset < impl_->monster_assets.size(); ++asset)
    {
        const auto &monster = impl_->monster_assets[asset];
        constants->monster_asset_meta[asset] = {monster.skin_offset, monster.bone_count, 0, 0};
        for (std::size_t clip = 0; clip < monster.clip_meta.size(); ++clip)
            constants->monster_clip_meta[asset][clip] = monster.clip_meta[clip];
    }
    AnimationPoseRef pose{};
    if (const auto source = std::ranges::find(snapshot.instances, RenderMesh::Archer,
                                             &RenderInstance::mesh);
        source != snapshot.instances.end())
    {
        const auto source_index = static_cast<std::uint32_t>(source - snapshot.instances.begin());
        if (const auto *selected = pose_by_source[source_index]) pose = *selected;
    }
    bool preview_pose_override = false;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.character_preview && impl_->preview_pose_override)
    {
        preview_pose_override = true;
        pose.clip = static_cast<CharacterAnimationClip>(impl_->preview_base_clip);
        pose.normalized_time = impl_->preview_base_time;
        pose.secondary_clip =
            static_cast<CharacterAnimationClip>(impl_->preview_secondary_clip);
        pose.secondary_normalized_time = impl_->preview_secondary_time;
        pose.secondary_weight = impl_->preview_secondary_weight;
        pose.upper_body_clip =
            static_cast<CharacterAnimationClip>(impl_->preview_upper_clip);
        pose.upper_body_normalized_time = impl_->preview_upper_time;
        pose.upper_body_weight = impl_->preview_upper_weight;
    }
#endif
    const auto eased_weight = [](float weight) {
        const auto clamped = std::clamp(weight, 0.0f, 1.0f);
        return clamped * clamped * (3.0f - 2.0f * clamped);
    };
    const auto blend_transform = [](const CharacterLocalTransform &first,
                                    const CharacterLocalTransform &second,
                                    float weight) {
        CharacterLocalTransform output;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            output.translation[axis] = std::lerp(first.translation[axis],
                                                 second.translation[axis], weight);
            output.scale[axis] = std::lerp(first.scale[axis], second.scale[axis], weight);
        }
        auto first_rotation = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(
            first.rotation[0], first.rotation[1], first.rotation[2], first.rotation[3]));
        auto second_rotation = DirectX::XMQuaternionNormalize(DirectX::XMVectorSet(
            second.rotation[0], second.rotation[1], second.rotation[2], second.rotation[3]));
        if (DirectX::XMVectorGetX(
                DirectX::XMQuaternionDot(first_rotation, second_rotation)) < 0.0f)
        {
            second_rotation = DirectX::XMVectorNegate(second_rotation);
        }
        DirectX::XMFLOAT4 rotation;
        DirectX::XMStoreFloat4(
            &rotation, DirectX::XMQuaternionNormalize(DirectX::XMQuaternionSlerp(
                           first_rotation, second_rotation, weight)));
        output.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
        return output;
    };
    const auto sample = [&](CharacterAnimationClip clip, float time,
                            std::uint32_t bone_index,
                            CharacterLocalTransform &output) {
        const auto found = std::ranges::find(impl_->archer_clips, clip,
                                              &CharacterClipHeader::clip);
        if (found == impl_->archer_clips.end()) return false;
        const auto normalized = found->looping ? time - std::floor(time)
                                               : std::clamp(time, 0.0f, 1.0f);
        const auto frame_position = normalized * static_cast<float>(found->frame_count - 1);
        const auto first_frame = static_cast<std::uint32_t>(frame_position);
        const auto second_frame = std::min(first_frame + 1, found->frame_count - 1);
        const auto first_index = found->first_transform + first_frame * impl_->archer_bone_count + bone_index;
        const auto second_index = found->first_transform + second_frame * impl_->archer_bone_count + bone_index;
        const auto first = impl_->archer_packed_transforms.empty() ? impl_->archer_transforms[first_index]
            : impl_->archer_packed_transforms[first_index].Value();
        const auto second = impl_->archer_packed_transforms.empty() ? impl_->archer_transforms[second_index]
            : impl_->archer_packed_transforms[second_index].Value();
        output = blend_transform(first, second,
                                 frame_position - static_cast<float>(first_frame));
        return true;
    };
    const auto pose_for_archer = [&](const RenderInstance &instance) -> const AnimationPoseRef & {
        if (!preview_pose_override)
        {
            if (instance.stable_id != 0)
                if (const auto found = pose_by_id.find(instance.stable_id); found != pose_by_id.end())
                    return *found->second;
            const auto ordered = static_cast<std::size_t>(&instance - render_instances.data());
            if (ordered < ordered_source_indices.size() &&
                ordered_source_indices[ordered] < pose_by_source.size())
                if (const auto *candidate = pose_by_source[ordered_source_indices[ordered]])
                    return *candidate;
        }
        return pose;
    };
    const auto sample_legacy_pose = [&](const AnimationPoseRef &pose_ref,
                                        std::span<DirectX::XMFLOAT4X4> output) -> Result {
        if (output.size() < impl_->archer_bone_count)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Legacy Archer palette destination is too small.");
        std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> global_transforms{};
        for (std::uint32_t bone_index = 0; bone_index < impl_->archer_bone_count; ++bone_index)
        {
            CharacterLocalTransform base, secondary, upper;
            if (!sample(pose_ref.clip,
                        pose_ref.normalized_time * std::max(0.0f, pose_ref.playback_rate),
                        bone_index, base) ||
                !sample(pose_ref.secondary_clip,
                        pose_ref.secondary_normalized_time *
                            std::max(0.0f, pose_ref.secondary_playback_rate),
                        bone_index, secondary))
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Cooked animation transform cannot be blended.");
            }
            base = blend_transform(base, secondary, eased_weight(pose_ref.secondary_weight));
            const auto upper_weight = eased_weight(pose_ref.upper_body_weight) *
                                      impl_->archer_upper_body_weights[bone_index];
            if (upper_weight > 0.0f &&
                !sample(pose_ref.upper_body_clip,
                        pose_ref.upper_body_normalized_time *
                            std::max(0.0f, pose_ref.upper_body_playback_rate),
                        bone_index, upper))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Cooked upper-body animation cannot be blended.");
            if (upper_weight > 0.0f)
                base = blend_transform(base, upper, upper_weight);
            const auto local =
                DirectX::XMMatrixScaling(base.scale[0], base.scale[1], base.scale[2]) *
                DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(
                    base.rotation[0], base.rotation[1], base.rotation[2], base.rotation[3])) *
                DirectX::XMMatrixTranslation(base.translation[0], base.translation[1],
                                             base.translation[2]);
            const auto parent = impl_->archer_parents[bone_index];
            const auto global = parent == std::numeric_limits<std::uint16_t>::max()
                                    ? local
                                    : local * DirectX::XMLoadFloat4x4(
                                                  &global_transforms[parent]);
            DirectX::XMStoreFloat4x4(&global_transforms[bone_index], global);
            DirectX::XMFLOAT4X4 inverse_bind;
            std::memcpy(&inverse_bind,
                        impl_->archer_inverse_bind_matrices[bone_index].data(),
                        sizeof(inverse_bind));
            const auto skin = DirectX::XMMatrixTranspose(
                                  DirectX::XMLoadFloat4x4(&inverse_bind)) * global;
            const auto determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(skin));
            if (!std::isfinite(determinant) || std::abs(determinant) <= 0.0001f ||
                DirectX::XMMatrixIsNaN(skin) || DirectX::XMMatrixIsInfinite(skin))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Blended animation produced an invalid skin pose.");
            DirectX::XMStoreFloat4x4(&output[bone_index],
                                     DirectX::XMMatrixTranspose(skin));
        }
        // Local-TRS blending changes foot height even when both clips are grounded.
        // Correct the final skin pose before the instance ground offset. Death uses
        // the cooker's whole-body grounding instead of the standing support surface.
        if (pose_ref.clip != CharacterAnimationClip::Death &&
            !impl_->archer_support_vertices.empty())
        {
            float support_y = std::numeric_limits<float>::max();
            for (const auto &vertex : impl_->archer_support_vertices)
            {
                float y = 0.0f;
                for (std::size_t influence = 0; influence < vertex.bone_weights.size(); ++influence)
                {
                    const auto &skin = output[vertex.bone_indices[influence]];
                    y += vertex.bone_weights[influence] *
                         (skin._21 * vertex.position[0] + skin._22 * vertex.position[1] +
                          skin._23 * vertex.position[2] + skin._24);
                }
                support_y = std::min(support_y, y);
            }
            const float correction = -impl_->archer_ground_offset - support_y;
            for (std::uint32_t bone = 0; bone < impl_->archer_bone_count; ++bone)
                output[bone]._24 += correction;
        }
        return Result::Success();
    };
    const auto palette_prepare_start = capture_clock();
    // All views consume the same package-owned sampled palette.
    using ArcherPaletteKey = RenderPoseKey;
    std::vector<ArcherPaletteKey> palette_keys;
    std::vector<std::uint32_t> archer_palette_slots(instance_count,
                                                     std::numeric_limits<std::uint32_t>::max());
    std::size_t palette_count{};
    for (std::size_t index = 0; index < render_instances.size(); ++index)
    {
        const auto &instance = render_instances[index];
        if (instance.mesh != RenderMesh::Archer) continue;
        const auto &instance_pose = pose_for_archer(instance);
        const auto key = MakeRenderPoseKey(instance, &instance_pose);
        const auto found = std::ranges::find(palette_keys, key);
        if (found != palette_keys.end())
        {
            archer_palette_slots[index] = static_cast<std::uint32_t>(found - palette_keys.begin());
            continue;
        }
        const auto slot = palette_count++;
        if (slot >= archer_instance_count)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Archer palette count exceeded prepared frame capacity.");
        auto prepared = std::ranges::find_if(impl_->prepared_archer_palettes,
            [&](const auto &entry) { return entry.key == key; });
        if (prepared == impl_->prepared_archer_palettes.end())
        {
            auto &next = impl_->prepared_archer_palettes.emplace_back();
            next.key = key;
            const auto output = std::span(next.bones.data(), impl_->archer_bone_count);
            if (instance.animation_clip_id.value != 0)
            {
                const auto sampled = impl_->archer_compiled_animation
                    ? SampleNamedCharacterAnimation(*impl_->archer_compiled_animation,
                        impl_->archer_named_packed_transforms, instance, output)
                    : SampleNamedCharacterAnimation(impl_->archer_named_animations, instance, output);
                if (!sampled) return sampled;
            }
            else if (auto sampled = sample_legacy_pose(instance_pose, output); !sampled)
                return sampled;
            prepared = std::prev(impl_->prepared_archer_palettes.end());
        }
        std::memcpy(frame.mapped + archer_palette_data_offset + slot * archer_palette_stride,
                    prepared->bones.data(), archer_palette_stride);
        palette_keys.push_back(key);
        archer_palette_slots[index] = static_cast<std::uint32_t>(slot);
    }
    cpu_timings.palette_prepare_us = capture_elapsed(palette_prepare_start);
    constants->render_options = {
        static_cast<float>(particle_capacity), impl_->config.bloom ? 1.0f : 0.0f,
        impl_->config.outline ? 1.0f : 0.0f,
        static_cast<float>(particle_delta_ticks) / 60.0f};
    constants->authored_particle_clock = {static_cast<std::uint32_t>(stateful_target), 0, 0, 0};
    if (!impl_->render_scene_inputs) { gpu_particle_spawn_count = 0; total_particles_to_spawn = 0; }
    constants->particle_options = {particle_capacity, gpu_particle_spawn_count,
                                   total_particles_to_spawn,
                                   impl_->archer_material_count};
    const bool meadow_collision = static_environment_ranges[render_data::kMeadowGeometryBegin * 3u].second != 0 ||
        std::ranges::any_of(render_instances, [](const RenderInstance &instance) {
            return instance.mesh == RenderMesh::MeadowSurface && (instance.environment_variant % 2u) == 0u;
        });
    constants->particle_terrain = {static_cast<float>(view_input.scene_origin.x),
        static_cast<float>(view_input.scene_origin.y), static_cast<float>(view_input.scene_origin.z),
        meadow_collision ? 1.0f : 0.0f};
    const auto grass_benders = SelectGrassBenders(render_instances);
    constants->grass_bender_count = {grass_benders.count, 0, 0, 0};
    for (std::size_t index = 0; index < grass_benders.count; ++index)
    {
        const auto &position = grass_benders.instances[index]->position;
        constants->grass_benders[index] = {position.x, position.y, position.z,
                                          index == 0 ? 1.25f : 1.0f};
    }
    constants->previous_grass_benders = owned_view.previous_grass_benders;
    constants->previous_grass_state = owned_view.previous_grass_state;
    if (new_session || view_input.camera_cut || !previous_instance_snapshot ||
        constants->surface_view_origin.w < 0.5f ||
        (static_scene && impl_->owned_views.at(impl_->active_view_id.value)->scene_lease != static_scene))
        constants->previous_grass_state.z = 0.0f;
    constants->previous_grass_origin_delta = {
        static_cast<float>(view_input.scene_origin.x - owned_view.previous_scene_origin.x),
        static_cast<float>(view_input.scene_origin.y - owned_view.previous_scene_origin.y),
        static_cast<float>(view_input.scene_origin.z - owned_view.previous_scene_origin.z), 0.0f};
    const auto current_grass_grid = BuildGrassBenderGrid(
        std::span{constants->grass_benders}.first(grass_benders.count));
    const auto previous_grass_count = constants->previous_grass_state.z > 0.5f
        ? std::min(32u, static_cast<std::uint32_t>(constants->previous_grass_state.y)) : 0u;
    const auto previous_grass_grid = BuildGrassBenderGrid(
        std::span{constants->previous_grass_benders}.first(previous_grass_count));
    const auto store_grass_grid = [](const GrassBenderGrid &grid,
        DirectX::XMFLOAT4 &descriptor, std::array<DirectX::XMUINT4, 16> &masks) {
        descriptor = {grid.world_to_cell.x, grid.world_to_cell.y,
            grid.world_to_cell.z, grid.world_to_cell.w};
        for (std::size_t i = 0; i != masks.size(); ++i)
            masks[i] = {grid.masks[i][0], grid.masks[i][1], grid.masks[i][2], grid.masks[i][3]};
    };
    store_grass_grid(current_grass_grid, constants->grass_bender_grid, constants->grass_bender_masks);
    store_grass_grid(previous_grass_grid, constants->previous_grass_bender_grid, constants->previous_grass_bender_masks);
    // BendGrass adds wind plus delta * (1 - |delta| / radius) * 0.42.
    // Each bender's maximum displacement is 0.42 * radius / 4, and its
    // height weight is at most one. Keep this per-frame allowance separate
    // from the persistent undeformed scene bound so wind/bending does not
    // force static scene uploads or lose casters outside the camera frustum.
    float grass_visibility_radius = 0.035f * std::sqrt(1.0f + 0.7f * 0.7f);
    for (std::size_t index = 0; index < constants->grass_bender_count.x; ++index)
        grass_visibility_radius += 0.105f *
            std::max(constants->grass_benders[index].w, 0.1f);
    const auto skin_prepare_start = capture_clock();
    owned_view.skin_history.BeginFrame(motion_history_reset);
    static_assert(sizeof(Vertex) == 76 && SkinVertexRanges::kVertexBytes == 36);
    constants->skin_cache_options = {};
    SkinVertexRanges skin_vertex_ranges;
    const auto packet_skin_deformation = prepared_scene.skin_deformation;
    const bool packet_skin_ready = packet_skin_deformation &&
        packet_skin_deformation->package_id == impl_->active_render_package_id &&
        packet_skin_deformation->skin_cache_valid &&
        packet_skin_deformation->skin_cache_vertices.resource &&
        packet_skin_deformation->skin_cache_bounds.resource;
    if (packet_skin_deformation && !packet_skin_ready)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Prepared packet skin deformation lost its submitted current owner.");
    const SkinVertexRanges *current_skin_ranges = packet_skin_ready
        ? &packet_skin_deformation->vertex_ranges : &skin_vertex_ranges;
    auto &dynamic_skin_offsets = frame.dynamic_skin_offsets_scratch;
    auto &dynamic_previous_skin_offsets = frame.dynamic_previous_skin_offsets_scratch;
    auto &dynamic_skin_vertex_counts = frame.dynamic_skin_vertex_counts_scratch;
    auto &dynamic_skin_geometry_ids = frame.dynamic_skin_geometry_ids_scratch;
    dynamic_skin_offsets.assign(instance_count, 0);
    dynamic_previous_skin_offsets.assign(instance_count, 0);
    dynamic_skin_vertex_counts.assign(instance_count, 0);
    dynamic_skin_geometry_ids.assign(instance_count, 0);
    auto *previous_skin_frame = !motion_history_reset && !view_input.camera_cut
        ? owned_view.previous_skin_deformation.get() : nullptr;
    const bool previous_skin_frame_valid = previous_skin_frame && previous_skin_frame->skin_cache_valid &&
        previous_skin_frame->skin_cache_vertices.resource;
    auto &dynamic_pose_flags = frame.dynamic_pose_flags_scratch;
    auto &dynamic_pose_times = frame.dynamic_pose_times_scratch;
    auto &dynamic_skin_history = frame.dynamic_skin_history_scratch;
    auto &dynamic_cooked_skin_bounds = frame.dynamic_cooked_skin_bounds_scratch;
    auto &dynamic_skin_needed = frame.dynamic_skin_needed_scratch;
    auto &dynamic_canonical_inputs = frame.dynamic_canonical_inputs_scratch;
    auto &dynamic_skin_slots = frame.dynamic_skin_slots_scratch;
    dynamic_pose_flags.assign(instance_count, 0);
    dynamic_pose_times.assign(instance_count, 1.0f);
    dynamic_skin_history.assign(instance_count, 0);
    dynamic_cooked_skin_bounds.assign(instance_count, 0);
    dynamic_skin_needed.assign(instance_count, 0);
    if (reconcile_packet_scene) dynamic_canonical_inputs.resize(instance_count);
    else dynamic_canonical_inputs.clear();
    dynamic_skin_slots.assign(instance_count, std::numeric_limits<std::uint32_t>::max());
    const bool non_view_skin_consumer =
        active_features.software_gi_enabled || !typed_volumes.empty() ||
        !prepared_scene.monster_lod_consumers_valid || prepared_scene.vsm_skin_consumer;
    using SkinCullVec = std::array<double, 3>;
    const auto skin_cull_dot = [](SkinCullVec a, SkinCullVec b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto skin_cull_cross = [](SkinCullVec a, SkinCullVec b) -> SkinCullVec {
        return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto prepare_skin_cull_views = [&] {
        auto &package_views = prepared_scene.skin_cull_views;
        package_views.clear();
        prepared_scene.skin_cull_views_valid = true;
        package_views.reserve(impl_->explicit_frame->views.size());
        for (const auto &consumer : impl_->explicit_frame->views)
        {
            auto &basis = package_views.emplace_back();
            basis.near_plane = consumer.near_plane;
            basis.far_plane = consumer.far_plane;
            if (consumer.render_active.width == 0 || consumer.render_active.height == 0 ||
                !std::isfinite(consumer.near_plane) || !std::isfinite(consumer.far_plane) ||
                consumer.near_plane <= 0 || consumer.far_plane <= consumer.near_plane)
            {
                basis.valid = false;
                prepared_scene.skin_cull_views_valid = false;
                continue;
            }
            basis.eye = {consumer.camera.position.x, consumer.camera.position.y,
                consumer.camera.position.z};
            basis.target = {consumer.camera.target.x, consumer.camera.target.y,
                consumer.camera.target.z};
            basis.scene_origin = {consumer.scene_origin.x, consumer.scene_origin.y,
                consumer.scene_origin.z};
            const SkinCullVec up{consumer.camera.up.x, consumer.camera.up.y,
                consumer.camera.up.z};
            basis.forward = {basis.target[0] - basis.eye[0], basis.target[1] - basis.eye[1],
                basis.target[2] - basis.eye[2]};
            const double forward_length = std::sqrt(skin_cull_dot(basis.forward, basis.forward));
            if (!std::isfinite(forward_length) || forward_length <= 0)
            {
                basis.valid = false;
                prepared_scene.skin_cull_views_valid = false;
                continue;
            }
            for (auto &value : basis.forward) value /= forward_length;
            basis.right = skin_cull_cross(up, basis.forward);
            const double right_length = std::sqrt(skin_cull_dot(basis.right, basis.right));
            if (!std::isfinite(right_length) || right_length <= 0)
            {
                basis.valid = false;
                prepared_scene.skin_cull_views_valid = false;
                continue;
            }
            for (auto &value : basis.right) value /= right_length;
            basis.vertical = skin_cull_cross(basis.forward, basis.right);
            basis.tangent_y = std::tan(double(consumer.camera.vertical_fov_degrees) *
                std::numbers::pi / 360.0);
            basis.tangent_x = basis.tangent_y * double(consumer.render_active.width) /
                double(consumer.render_active.height);
            basis.valid = std::isfinite(basis.tangent_x) && std::isfinite(basis.tangent_y) &&
                basis.tangent_x > 0 && basis.tangent_y > 0;
            basis.side_x = std::hypot(1.0, basis.tangent_x);
            basis.side_y = std::hypot(1.0, basis.tangent_y);
            prepared_scene.skin_cull_views_valid &= basis.valid;
        }
        SkinCullVec sun{light.direction.x, light.direction.y, light.direction.z};
        const double sun_length = std::sqrt(skin_cull_dot(sun, sun));
        prepared_scene.skin_cull_sun_valid = std::isfinite(sun_length) && sun_length > 0;
        if (prepared_scene.skin_cull_sun_valid)
        {
            for (auto &value : sun) value /= sun_length;
            const SkinCullVec sun_up = std::abs(sun[1]) > 0.99 ? SkinCullVec{0, 0, 1} : SkinCullVec{0, 1, 0};
            auto &sun_right = prepared_scene.skin_cull_sun_right;
            sun_right = skin_cull_cross(sun_up, sun);
            const double sun_right_length = std::sqrt(skin_cull_dot(sun_right, sun_right));
            prepared_scene.skin_cull_sun_valid = std::isfinite(sun_right_length) && sun_right_length > 0;
            if (prepared_scene.skin_cull_sun_valid)
            {
                for (auto &value : sun_right) value /= sun_right_length;
                prepared_scene.skin_cull_sun_vertical = skin_cull_cross(sun, sun_right);
            }
        }
        prepared_scene.skin_cull_views_prepared = true;
    };
    const auto intersects_package_view = [&](Float3 center, float vertical_offset, float radius) {
        if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) ||
            !std::isfinite(radius) || impl_->explicit_frame->views.empty() ||
            !std::isfinite(shadow_distance) || shadow_distance <= 0) return true;
        if (!prepared_scene.skin_cull_views_prepared) prepare_skin_cull_views();
        if (!prepared_scene.skin_cull_views_valid) return true;
        for (const auto &basis : prepared_scene.skin_cull_views)
        {
            const SkinCullVec point{basis.scene_origin[0] + center.x,
                basis.scene_origin[1] + center.y + vertical_offset,
                basis.scene_origin[2] + center.z};
            const SkinCullVec delta{point[0] - basis.eye[0], point[1] - basis.eye[1], point[2] - basis.eye[2]};
            const double depth = skin_cull_dot(delta, basis.forward);
            const double x = skin_cull_dot(delta, basis.right), y = skin_cull_dot(delta, basis.vertical);
            const double safe_radius = double(radius) + 2.0 +
                0.0001 * std::sqrt(skin_cull_dot(delta, delta));
            if (!std::isfinite(safe_radius)) return true;
            const bool camera_outside = depth + safe_radius < basis.near_plane ||
                depth - safe_radius > basis.far_plane ||
                std::abs(x) > depth * basis.tangent_x + safe_radius * basis.side_x ||
                std::abs(y) > depth * basis.tangent_y + safe_radius * basis.side_y;
            if (!camera_outside) return true;
            // Without virtual shadows this view still records three CSM cuts.
            // Their largest snapped square can admit offscreen casters; ignore
            // the light-space depth limit here to keep this test conservative.
            if (!prepared_scene.skin_cull_sun_valid) return true;
            const SkinCullVec shadow_delta{point[0] - basis.target[0], point[1] - basis.target[1],
                point[2] - basis.target[2]};
            const double half_extent = double(shadow_distance) * 0.5 + safe_radius + 2.0 +
                double(shadow_distance) / std::clamp(impl_->config.shadow_resolution, 1024u, 2048u);
            if (!std::isfinite(half_extent) || half_extent <= 0 ||
                (std::abs(skin_cull_dot(shadow_delta, prepared_scene.skin_cull_sun_right)) <= half_extent &&
                 std::abs(skin_cull_dot(shadow_delta, prepared_scene.skin_cull_sun_vertical)) <= half_extent))
                return true;
        }
        const std::array<double, 3> world_center{
            view_input.scene_origin.x + center.x,
            view_input.scene_origin.y + center.y + vertical_offset,
            view_input.scene_origin.z + center.z};
        for (const auto &light : prepared_scene.local_shadow_consumers)
        {
            const double distance = std::hypot(world_center[0] - light.center[0],
                world_center[1] - light.center[1], world_center[2] - light.center[2]);
            if (!std::isfinite(distance) || distance <= light.far_plane + radius + light.emitter_bound)
                return true;
        }
        return false;
    };
    for (std::size_t index = 0; index < instance_count; ++index)
    {
        const auto &source = render_instances[index];
        auto position = source.position;
        auto yaw_value = source.yaw;
        const auto vertical_offset = is_environment_mesh(source.mesh) ? 0.0f : source.mesh == RenderMesh::Archer
                                         ? impl_->archer_ground_offset * source.scale.y
                                         : is_monster(source.mesh)
                                             ? impl_->monster_assets[static_cast<std::size_t>(
                                                   static_cast<std::uint32_t>(source.mesh) -
                                                   static_cast<std::uint32_t>(RenderMesh::MonsterMelee))]
                                                   .ground_offset * source.scale.y
                                             : source.scale.y * 0.5f;
        auto mesh = static_cast<std::uint32_t>(source.mesh);
        if (is_environment_mesh(source.mesh))
            mesh |= kEnvironmentMeshFlag;
        if (source.mesh == RenderMesh::GroundDetail) mesh |= source.environment_variant << 8u;
        float animation_time = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch || source.mesh == RenderMesh::MeadowSurface
            ? std::bit_cast<float>(source.environment_seed) : 0.0f;
        AnimationPoseRef resolved_skin_pose;
        const AnimationPoseRef *skin_pose = nullptr;
        RenderPoseKey pose_key{};
        if (source.mesh == RenderMesh::Archer) skin_pose = &pose_for_archer(source);
        if (is_monster(source.mesh))
        {
            if (impl_->config.monster_preview_asset < 6 || family_preview)
            {
                resolved_skin_pose.clip = static_cast<CharacterAnimationClip>(
                    std::min(impl_->config.monster_preview_clip, 4u));
                resolved_skin_pose.normalized_time = impl_->config.monster_preview_time;
                skin_pose = &resolved_skin_pose;
            }
            else if (const auto pose_it = pose_by_id.find(source.stable_id);
                     pose_it != pose_by_id.end())
            {
                skin_pose = pose_it->second;
            }
        }
        float conservative_radius = 1.0f;
        if (is_environment_mesh(source.mesh))
        {
            conservative_radius =
                impl_->environment_variant_radii[environment_mesh_index(source)] *
                std::max({std::abs(source.scale.x), std::abs(source.scale.y),
                          std::abs(source.scale.z)});
        }
        else if (source.mesh == RenderMesh::Archer || is_monster(source.mesh))
        {
            const auto key = MakeRenderSceneInstanceKey(
                source.stable_id, ordered_source_indices[index]);
            const auto skin = owned_view.skin_history.Resolve(
                key, source, skin_pose,
                source.mesh == RenderMesh::Archer ? 1.75f : 1.5f);
            pose_key = skin.current_pose;
            conservative_radius = skin.conservative_radius;
            const auto monster_family = static_cast<std::uint32_t>(source.mesh) -
                static_cast<std::uint32_t>(RenderMesh::MonsterMelee);
            const auto selected_lod = ordered_monster_lods[index];
            const GeometryPageGpu *skin_geometry = source.mesh == RenderMesh::Archer
                ? &impl_->archer_geometry
                : selected_lod == 0 ? static_cast<const GeometryPageGpu *>(&impl_->monster_assets[monster_family])
                : &impl_->monster_reduced_geometry[monster_family][selected_lod - 1u];
            const auto vertex_count = skin_geometry->vertex_count;
            const auto geometry_identity = (std::uint64_t(skin_geometry->generation) << 32u) | skin_geometry->page_slot;
            dynamic_skin_geometry_ids[index] = geometry_identity;
            if (is_monster(source.mesh))
            {
                dynamic_pose_flags[index] =
                    static_cast<std::uint32_t>(skin.current_clip) << 8u;
                dynamic_pose_times[index] = skin.current_time;
                const auto &asset = impl_->monster_assets[static_cast<std::uint32_t>(source.mesh) -
                    static_cast<std::uint32_t>(RenderMesh::MonsterMelee)];
                const auto clip = static_cast<std::size_t>(skin.current_clip);
                if (asset.deformation_flags == render_data::kMonsterBoundsPositionEnclosure &&
                    source.animation_clip_id.value == 0 &&
                    (!skin_pose || (skin_pose->secondary_weight == 0.0f &&
                        skin_pose->upper_body_weight == 0.0f)) &&
                    selected_lod < asset.geometry_lods.size() &&
                    (asset.geometry_lods[selected_lod].flags & render_data::kGeometryFlagSkinned) != 0 &&
                    asset.geometry_lods[selected_lod].vertex_count == skin_geometry->vertex_count &&
                    clip < asset.clip_meta.size() &&
                    std::isfinite(skin.current_time) &&
                    std::isfinite(source.scale.x) && std::isfinite(source.scale.y) &&
                    std::isfinite(source.scale.z))
                {
                    const auto meta = asset.clip_meta[clip];
                    if (meta.y != 0)
                    {
                        const auto frame_position = std::clamp(skin.current_time, 0.0f, 1.0f) *
                            static_cast<float>(meta.y - 1u);
                        const auto frame_index = std::min(static_cast<std::uint32_t>(std::floor(frame_position)), meta.y - 1u);
                        const auto *bound = asset.IntervalBound(selected_lod, skin.current_clip, frame_index);
                        if (bound)
                        {
                            std::array<double, 3> extent{};
                            const std::array<double, 3> scale{source.scale.x, source.scale.y, source.scale.z};
                            for (std::size_t axis = 0; axis < 3; ++axis)
                            {
                                extent[axis] = std::max(std::abs(double(bound->minimum[axis])),
                                    std::abs(double(bound->maximum[axis]))) *
                                    std::abs(scale[axis]);
                            }
                            const double radius = std::hypot(extent[0], extent[1], extent[2]) * 1.0011 + 0.001;
                            if (std::isfinite(radius) && radius < FLT_MAX / 2)
                            {
                                conservative_radius = std::max(conservative_radius,
                                    std::nextafter(static_cast<float>(radius), FLT_MAX));
                                dynamic_cooked_skin_bounds[index] = 1;
                            }
                        }
                    }
                }
            }
            const bool attached_skin_consumer = source.stable_id != 0 &&
                std::ranges::binary_search(prepared_scene.skin_attachment_ids, source.stable_id);
            const bool needed = !dynamic_cooked_skin_bounds[index] || non_view_skin_consumer ||
                attached_skin_consumer ||
                intersects_package_view(position, vertical_offset, conservative_radius);
#ifndef NDEBUG
            if (!needed)
                assert(is_monster(source.mesh) && dynamic_cooked_skin_bounds[index] &&
                    !non_view_skin_consumer && !attached_skin_consumer);
#endif
            if (needed)
            {
                dynamic_skin_needed[index] = 1;
                SkinVertexRange range;
                const auto *packet_range = packet_skin_ready
                    ? packet_skin_deformation->vertex_ranges.Find(key) : nullptr;
                const bool has_range = packet_skin_ready
                    ? packet_range && packet_range->count == vertex_count &&
                        packet_range->geometry_identity == geometry_identity
                    : skin_vertex_ranges.Resolve(key, vertex_count, range, geometry_identity);
                if (packet_skin_ready && packet_range) range = *packet_range;
                if (vertex_count >= (1u << 20u))
                    return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                        "Skin cache vertex range exceeds shader addressing.");
                if (!has_range)
                    return Result::Failure(packet_skin_ready ? ErrorCode::InvalidState : ErrorCode::InvalidArgument,
                        "hs_renderer_d3d12", packet_skin_ready
                            ? "Current skin geometry changed after packet deformation was submitted."
                            : "Skin cache vertex range conflicts with its instance.");
                dynamic_skin_offsets[index] = range.offset;
                dynamic_skin_vertex_counts[index] = range.count;
                const auto *previous_range = previous_skin_frame_valid
                    ? previous_skin_frame->vertex_ranges.Find(key) : nullptr;
                dynamic_skin_history[index] = skin.history_valid && previous_range &&
                    previous_range->count == vertex_count &&
                    previous_range->geometry_identity == geometry_identity &&
                    SkinVertexRangeFits(previous_range->offset, vertex_count, previous_skin_frame->vertex_stride,
                        previous_skin_frame->skin_cache_vertices.resource->GetDesc().Width) ? 1u : 0u;
                if (dynamic_skin_history[index])
                    dynamic_previous_skin_offsets[index] = previous_range->offset;
            }
        }
        else pose_key = MakeRenderPoseKey(source, nullptr);
        if (reconcile_packet_scene)
        {
            instances[index] = {{position.x, position.y + vertical_offset, position.z,
                                 conservative_radius},
                                {source.scale.x, source.scale.y, source.scale.z, 1.0f},
                                source.color_rgba,
                                mesh,
                                yaw_value,
                                animation_time};
            dynamic_canonical_inputs[index] = {
                pose_key, MakeRenderContentKey(source),
                dynamic_skin_geometry_ids[index], source.revisions.transform,
                source.revisions.material, source.status_visual_mask};
        }
    }
    owned_view.skin_history.EndFrame();
    cpu_timings.skin_prepare_us = capture_elapsed(skin_prepare_start);

    // Keep transform/material/deformation payloads in stable GPU slots. Draw order,
    // LOD fade and environment complement state live in a compact per-frame
    // indirection stream instead of forcing the instance table to be reordered.
    const auto scene_reconcile_start = capture_clock();
    // The first submitted view reconciles current membership and canonical
    // payloads. Later views reuse its immutable table and write view skin in waited upload scratch.
    if (!reconcile_packet_scene && prepared_scene.gpu_slots.size() != instance_count)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Prepared packet scene slot map no longer matches its canonical instance list.");
    if (reconcile_packet_scene)
    {
        impl_->dynamic_scene_slots.BeginFrame(scene_new_session);
        prepared_scene.gpu_slots.assign(instance_count, {});
    }
    const auto current_gpu_instance = [&](std::size_t index) -> const GpuInstance & {
        return reconcile_packet_scene
            ? next_draw_instances[index]
            : impl_->dynamic_scene_slots.Instances()[prepared_scene.gpu_slots[index].index];
    };
    auto &dynamic_draw_refs = frame.dynamic_draw_refs_scratch;
    dynamic_draw_refs.resize(instance_count);
    bool reads_previous_instance_scene = false;
    bool have_skinned_instances = false;
    frame.static_input_refs = static_scene && impl_->static_gpu_scene && impl_->static_gpu_scene->refs
        ? impl_->static_gpu_scene->refs->cpu : nullptr;
    frame.dynamic_input_ref_offset = dynamic_ref_data_offset;
    frame.static_input_ref_count = static_cast<std::uint32_t>(static_draw_refs.size());
    frame.dynamic_input_ref_count = static_cast<std::uint32_t>(dynamic_draw_refs.size());
    for (std::size_t index = 0; index < instance_count; ++index)
    {
        const auto &source = render_instances[index];
        const auto environment = is_environment_mesh(source.mesh);
        const auto monster = is_monster(source.mesh);
        const auto skinned = dynamic_skin_needed[index] != 0;
        GpuSceneMetadata scene_metadata{
            source.asset_id.value,
            source.material_id.value,
            std::numeric_limits<std::uint32_t>::max(),
            skinned ? kCachedSkinFlag : 0u,
            0u,
            entity_surface_frame.InstancePayload(source.stable_id),
            dynamic_skin_offsets[index], dynamic_previous_skin_offsets[index],
            SkinVertexRanges::kVertexBytes,
            previous_skin_frame_valid ? previous_skin_frame->vertex_stride : SkinVertexRanges::kVertexBytes};
        GpuSceneSkinEntry view_skin{
            scene_metadata.skin_index, scene_metadata.skin_flags,
            scene_metadata.skin_vertex_offset, scene_metadata.previous_skin_vertex_offset,
            scene_metadata.skin_vertex_stride, scene_metadata.previous_skin_vertex_stride};
        scene_metadata.previous_skin_vertex_offset = 0;
        scene_metadata.previous_skin_vertex_stride = SkinVertexRanges::kVertexBytes;
        PersistentGpuSceneSlot slot;
        const auto &current_instance = current_gpu_instance(index);
        if (reconcile_packet_scene)
        {
            slot = impl_->dynamic_scene_slots.Upsert(
                MakeRenderSceneInstanceKey(source.stable_id, ordered_source_indices[index]),
                current_instance, scene_metadata, dynamic_canonical_inputs[index]);
            if (slot.conflict)
                return Result::Failure(
                    ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "A stable render instance produced conflicting persistent GPU payloads.");
            prepared_scene.gpu_slots[index] = slot;
        }
        else
        {
            slot = prepared_scene.gpu_slots[index];
            const auto metadata = impl_->dynamic_scene_slots.Metadata();
            if (slot.conflict || !slot.generation || slot.index >= metadata.size() ||
                metadata[slot.index].generation != slot.generation)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "A later view changed the packet's canonical GPU scene payload or lost its retained slot.");
        }
        view_skin.skin_index = skinned ? slot.index : std::numeric_limits<std::uint32_t>::max();
        std::memcpy(frame.mapped + view_skin_data_offset + slot.index * sizeof(GpuSceneSkinEntry),
                    &view_skin, sizeof(view_skin));
        if (skinned)
        {
            dynamic_skin_slots[index] = slot.index;
            have_skinned_instances = true;
        }
        const bool previous_slot_valid = previous_instance_snapshot &&
            slot.index < previous_instance_snapshot->identities.size() &&
            previous_instance_snapshot->identities[slot.index].generation == slot.generation;
        const bool continuous_motion = previous_slot_valid &&
            slot.index < previous_instance_snapshot->identities.size() &&
            previous_instance_snapshot->identities[slot.index].motion_epoch == source.motion_epoch;
        auto draw_flags = environment ? 0u : dynamic_pose_flags[index];
        if (skinned) draw_flags |= kCachedSkinFlag;
        if (dynamic_cooked_skin_bounds[index]) draw_flags |= 0x400000u;
        if (HasEnvironmentWind(source.mesh, source.environment_variant) && continuous_motion &&
            previous_instance_snapshot->identities[slot.index].mesh == current_instance.mesh)
            draw_flags |= 0x200000u;
        if (skinned && dynamic_skin_history[index] && continuous_motion)
            draw_flags |= kCachedSkinHistoryFlag;
        dynamic_draw_refs[index] = {
            slot.index,
            draw_flags,
            environment ? 1.0f : (monster ? dynamic_pose_times[index] : 1.0f),
            slot.generation};
        reads_previous_instance_scene |= previous_instance_snapshot &&
            (draw_flags & (kCachedSkinHistoryFlag | 0x200000u)) != 0;
    }
    if (reconcile_packet_scene) impl_->dynamic_scene_slots.EndFrame();
    if (reconcile_packet_scene) prepared_scene.gpu_slots_ready = true;
    const auto current_scene_payload_revision = impl_->dynamic_scene_slots.PayloadRevision();
    const auto current_scene_slot_count = impl_->dynamic_scene_slots.SlotCount();
    const auto &canonical_scene = impl_->canonical_instance_current;
    // An unchanged packet can keep the already-submitted scene owner as both
    // current and previous. This is read-only reuse; changed packets still get
    // a distinct owner so motion history retains the prior payload.
    const bool reuse_canonical_instance_scene = impl_->render_scene_inputs && !reconcile_packet_scene &&
        entity_surface_frame.Bytes().empty() && canonical_scene && canonical_scene->submitted &&
        canonical_scene->scene.resource && canonical_scene->package_id == impl_->active_render_package_id &&
        canonical_scene->device == impl_->device.Get() &&
        canonical_scene->uploaded_revision == current_scene_payload_revision &&
        canonical_scene->slot_count == current_scene_slot_count &&
        canonical_scene->capacity >= current_scene_slot_count &&
        canonical_scene->entity_surface_bytes == 0 &&
        canonical_scene->clock_epoch == snapshot.header.clock.epoch &&
        canonical_scene->clock_mode == snapshot.header.clock.mode &&
        canonical_scene->simulation_time == snapshot.header.simulation_time &&
        canonical_scene->surface_package_id == impl_->active_render_package_id;
    const bool select_current_instance_scene =
        (impl_->render_scene_inputs && !reuse_canonical_instance_scene) ||
        !canonical_scene || !canonical_scene->submitted || !canonical_scene->scene.resource ||
        canonical_scene->package_id != impl_->active_render_package_id ||
        canonical_scene->device != impl_->device.Get();
    std::shared_ptr<Impl::SubmittedInstanceFrame> current_instance_snapshot =
        impl_->canonical_instance_current;
    if (select_current_instance_scene)
    {
        const auto current_scene_instances = impl_->dynamic_scene_slots.Instances();
        const auto current_scene_metadata = impl_->dynamic_scene_slots.Metadata();
        const auto completed = impl_->graph_queue_executor.Completed();
        auto reusable = std::ranges::find_if(impl_->instance_history_frames,
            [&](const auto &owner) {
                return owner.use_count() == 1 &&
                    (!owner->device || owner->device == impl_->device.Get()) &&
                    owner->scene.graph_completion.IsSatisfiedBy(completed);
            });
        if (reusable == impl_->instance_history_frames.end())
        {
            current_instance_snapshot = std::make_shared<Impl::SubmittedInstanceFrame>();
            impl_->instance_history_frames.push_back(current_instance_snapshot);
        }
        else current_instance_snapshot = *reusable;
        auto &identities = current_instance_snapshot->identities;
        identities.resize(current_scene_metadata.size());
        for (std::size_t slot = 0; slot < identities.size(); ++slot)
            identities[slot] = {current_scene_metadata[slot].generation,
                slot < current_scene_instances.size() ? current_scene_instances[slot].mesh : 0u, 0};
        for (std::size_t index = 0; index < dynamic_draw_refs.size(); ++index)
            identities[dynamic_draw_refs[index].instance_index].motion_epoch = render_instances[index].motion_epoch;
        current_instance_snapshot->package_id = impl_->active_render_package_id;
        current_instance_snapshot->submitted = false;
    }
    auto &dynamic_scene = current_instance_snapshot->scene;
    const bool separate_previous_instance_scene = reads_previous_instance_scene &&
        previous_instance_snapshot != current_instance_snapshot;
    const auto scene_changes = reconcile_packet_scene
        ? impl_->dynamic_scene_slots.Changes() : GpuSceneChanges{};
    cpu_timings.scene_reconcile_us = capture_elapsed(scene_reconcile_start);

    const auto scene_gpu_prepare_start = capture_clock();
    auto scene_substage_start = scene_gpu_prepare_start;
    const auto record_scene_substage = [&](std::uint64_t &duration) {
        if (!impl_->config.capture_timing_stream) return;
        const auto end = std::chrono::steady_clock::now();
        duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            end - scene_substage_start).count());
        scene_substage_start = end;
    };
    const auto required_slots = impl_->dynamic_scene_slots.SlotCount();
    constexpr std::size_t kSkinBoundStride = sizeof(float) * 4;
    // The packet owns immutable current poses, palette bytes, union LODs and
    // canonical GPU slots. A submitted packet owner therefore proves current
    // deformation identity; only previous offsets/history remain per view.
    const bool reuse_canonical_skin = have_skinned_instances && packet_skin_ready;
    const auto completed_skin = impl_->graph_queue_executor.Completed();
    std::erase_if(impl_->canonical_skin_frames, [&](const auto &lease) {
        return lease.use_count() == 1 && lease->skin_cache_use_completion.IsSatisfiedBy(completed_skin);
    });
    if (reuse_canonical_skin)
    {
        const auto &canonical = *packet_skin_deformation;
        frame.skin_cache_vertices = Impl::RetainViewAllocation(canonical.skin_cache_vertices);
        frame.skin_cache_bounds = Impl::RetainViewAllocation(canonical.skin_cache_bounds);
        frame.skin_cache_vertex_capacity = canonical.skin_cache_vertex_capacity;
        frame.skin_cache_bound_capacity = canonical.skin_cache_bound_capacity;
        frame.skin_cache_vertex_read = frame.skin_cache_bounds_read = true;
    }
    // The pool retains immutable scene deformation snapshots referenced by a
    // owned_view. A reused mutable frame slot gets fresh storage only if still leased.
    const bool leased_skin_slot = !reuse_canonical_skin && std::ranges::any_of(impl_->canonical_skin_frames,
        [&](const auto &lease) {
            return (lease.use_count() > 1 || !lease->skin_cache_use_completion.IsSatisfiedBy(completed_skin)) &&
                lease->skin_cache_vertices.resource.Get() == frame.skin_cache_vertices.resource.Get();
        });
    if (have_skinned_instances && !reuse_canonical_skin)
        impl_->canonical_skin_frames.reserve(impl_->canonical_skin_frames.size() + 1);
    if (have_skinned_instances)
    {
        const auto required_skin_vertices = current_skin_ranges->VertexCount();
        if (leased_skin_slot || !frame.skin_cache_vertices.resource ||
            frame.skin_cache_vertex_capacity < required_skin_vertices)
        {
            // Grow only compact live geometry; each new enemy must not reallocate the whole buffer.
            const auto capacity = std::min<std::size_t>(SkinVertexRanges::kMaxVertices,
                required_skin_vertices + std::max<std::size_t>(required_skin_vertices / 4, 256));
            AllocationResource next_skin_buffer;
            D3D12MA::ALLOCATION_DESC allocation{};
            allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            if (auto created = impl_->CreateAllocation(
                    next_skin_buffer, allocation,
                    BufferDescription(capacity * SkinVertexRanges::kVertexBytes,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                !created)
                return created;
            // WaitForFrame covered this allocation's producer and last reader.
            // The replacement starts in UAV state with no submitted history.
            frame.skin_cache_vertices = std::move(next_skin_buffer);
            frame.skin_cache_vertex_capacity = capacity;
            frame.skin_cache_vertex_read = false;
            frame.skin_cache_valid = false;
        }
        if (leased_skin_slot || !frame.skin_cache_bounds.resource ||
            frame.skin_cache_bound_capacity < required_slots)
        {
            const auto capacity = std::max<std::size_t>(required_slots + required_slots / 4, 256);
            AllocationResource next_skin_bounds;
            D3D12MA::ALLOCATION_DESC allocation{};
            allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            if (auto created = impl_->CreateAllocation(
                    next_skin_bounds, allocation,
                    BufferDescription(capacity * kSkinBoundStride,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                !created)
                return created;
            frame.skin_cache_bounds = std::move(next_skin_bounds);
            frame.skin_cache_bound_capacity = capacity;
            frame.skin_cache_bounds_read = false;
            frame.skin_cache_valid = false;
        }
    }
    std::shared_ptr<Impl::CanonicalSkinFrame> staged_current_skin_deformation;
    if (have_skinned_instances && !reuse_canonical_skin)
    {
        // Hold the exact backing and immutable ranges before submission, but keep
        // the candidate invalid and unpublished until Execute accepts the graph.
        staged_current_skin_deformation = std::make_shared<Impl::CanonicalSkinFrame>();
        auto &canonical = *staged_current_skin_deformation;
        canonical.skin_cache_vertices = Impl::RetainViewAllocation(frame.skin_cache_vertices);
        canonical.skin_cache_bounds = Impl::RetainViewAllocation(frame.skin_cache_bounds);
        canonical.skin_cache_vertex_capacity = frame.skin_cache_vertex_capacity;
        canonical.skin_cache_bound_capacity = frame.skin_cache_bound_capacity;
        canonical.vertex_stride = SkinVertexRanges::kVertexBytes;
        canonical.vertex_ranges = std::move(skin_vertex_ranges);
        canonical.package_id = impl_->active_render_package_id;
    }
    std::size_t dynamic_scene_capacity =
        std::max<std::size_t>(current_instance_snapshot->capacity, 256);
    while (dynamic_scene_capacity < required_slots)
    {
        if (dynamic_scene_capacity > std::numeric_limits<std::size_t>::max() / 2)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Dynamic instance buffer size overflow.");
        dynamic_scene_capacity *= 2;
    }
    GpuSceneTableLayout dynamic_scene_layout;
    if (!BuildGpuSceneTableLayout(
            dynamic_scene_capacity, required_slots, dynamic_scene_layout, entity_surface_frame.Bytes().size()))
        return Result::Failure(
            ErrorCode::InvalidArgument, "hs_renderer_d3d12",
            "Dynamic split GPU scene table size overflow.");
    constants->scene_table_bounds = {
        static_cast<std::uint32_t>(dynamic_scene_layout.total_bytes), 0u, 0u, 0u};
    if (required_slots > current_instance_snapshot->capacity ||
        !dynamic_scene.resource ||
        dynamic_scene.resource->GetDesc().Width < dynamic_scene_layout.total_bytes)
    {
        AllocationResource next_buffer;
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = impl_->CreateAllocation(
                next_buffer, allocation,
                BufferDescription(dynamic_scene_layout.total_bytes),
                D3D12_RESOURCE_STATE_COPY_DEST);
            !created) return created;
        // A selected pool owner has no logical leases and all native readers completed.
        dynamic_scene = std::move(next_buffer);
        current_instance_snapshot->capacity = dynamic_scene_capacity;
        current_instance_snapshot->device = impl_->device.Get();
        current_instance_snapshot->initialized = false;
        current_instance_snapshot->uploaded_revision = 0;
    }
    if (auto prepared = impl_->PrepareLodStateBuffer(
            owned_view.dynamic_lod_states, dynamic_scene_capacity); !prepared) return prepared;

    struct DynamicSceneCopyRegion
    {
        std::size_t destination_offset{};
        std::size_t byte_count{};
        std::size_t staging_offset{};
    };
    auto &dirty_instance_ranges = frame.scene_dirty_scratch;
    std::vector<DynamicSceneCopyRegion> dynamic_scene_copy_regions;
    const auto dynamic_instance_base =
        dynamic_scene.resource->GetGPUVirtualAddress();
#if defined(HS_DEVELOPMENT_TOOLS)
    std::size_t dynamic_instance_upload_bytes{};
#endif
    std::size_t dynamic_scene_staging_bytes{};
    const auto persistent_instances = impl_->dynamic_scene_slots.Instances();
    const auto persistent_metadata = impl_->dynamic_scene_slots.Metadata();
    // Plan shadows from all current casters before demand and graph imports.
    record_scene_substage(cpu_timings.scene_skin_prepare_us);
    VirtualShadowOutputs virtual_shadow_outputs;
    owned_view.virtual_shadows.BeginFramePolicy();
    LocalShadowOutputs local_shadow_outputs;
    decltype(owned_view.previous_shadow_casters) current_shadow_casters;
    std::shared_ptr<Impl::StaticShadowCasterBatch> static_shadow_batch;
    struct ShadowBinding
    {
        const GeometryPageGpu *page{};
        ID3D12PipelineState *pipeline{};
        UINT material{};
        bool immutable{};
    };
    std::vector<ShadowBinding> bindings;
    std::vector<VirtualShadowDrawGroup> groups;
    std::vector<VirtualShadowCaster> casters;
    std::vector<VirtualShadowReceiver> receivers;
    std::vector<ShadowSurfaceReceiver> dynamic_surface_receivers;
    std::vector<VirtualShadowNativeRead> reads;
    VirtualShadowInputs inputs;
    VirtualShadowFramePlan shadow_plan;
    bool csm_required = true;
    if ((primary_sun && owned_view.virtual_shadows.AllocatedBytes() != 0) || owned_view.local_shadows.AllocatedBytes() != 0)
    {
        auto &shadow_owner = *impl_->owned_views.at(impl_->active_view_id.value);
        const auto caster_preparation_epoch = BeginShadowCasterPreparation(
            shadow_owner.scratch_shadow_casters, shadow_owner.shadow_caster_preparation_epoch);
        current_shadow_casters = std::move(shadow_owner.scratch_shadow_casters);
        const auto static_caster_generation = impl_->static_gpu_scene ? impl_->static_gpu_scene->instance_generation : 0u;
        // Static groups occupy the retained prefix; dynamic bindings are resolved per frame.
        std::array<std::array<UINT, kGpuVisibilityBucketCount + 2>, 2> shadow_group_indices;
        for (auto &indices : shadow_group_indices) indices.fill(UINT32_MAX);
        std::vector<std::size_t> removed_static_indices;
        std::size_t matched_previous_casters{};
        std::size_t prepared_casters{};
#ifndef NDEBUG
        auto &caster_key_oracle = shadow_owner.shadow_caster_key_oracle;
        if (impl_->config.validation || impl_->config.gpu_validation)
            BeginShadowCasterKeyOracle(caster_key_oracle, caster_preparation_epoch,
                shadow_owner.shadow_caster_key_oracle_prepared);
#endif
        current_shadow_casters.reserve(dynamic_draw_refs.size());
        casters.reserve(dynamic_draw_refs.size());
        std::uint32_t froxel_receiver_index = UINT32_MAX;
        bool camera_receiver_domain_closed=true;
        std::optional<VirtualShadowReceiver> water_receiver, gel_receiver;
        const auto merge_receiver = [](std::optional<VirtualShadowReceiver> &target,
            DirectX::XMFLOAT4 center, double radius) -> Result {
            if (!std::isfinite(center.x) || !std::isfinite(center.y) || !std::isfinite(center.z) ||
                !std::isfinite(radius) || radius < 0 ||
                std::abs(double(center.x)) + radius > FLT_MAX ||
                std::abs(double(center.y)) + radius > FLT_MAX ||
                std::abs(double(center.z)) + radius > FLT_MAX)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Virtual shadow receiver has nonfinite or unbounded world bounds.");
            VirtualShadowReceiver bounds{
                {std::nextafter(float(double(center.x) - radius), -FLT_MAX),
                 std::nextafter(float(double(center.y) - radius), -FLT_MAX),
                 std::nextafter(float(double(center.z) - radius), -FLT_MAX), 0},
                {std::nextafter(float(double(center.x) + radius), FLT_MAX),
                 std::nextafter(float(double(center.y) + radius), FLT_MAX),
                 std::nextafter(float(double(center.z) + radius), FLT_MAX), 0}};
            if (!target) target = bounds;
            else
            {
                target->minimum.x = std::min(target->minimum.x, bounds.minimum.x);
                target->minimum.y = std::min(target->minimum.y, bounds.minimum.y);
                target->minimum.z = std::min(target->minimum.z, bounds.minimum.z);
                target->maximum.x = std::max(target->maximum.x, bounds.maximum.x);
                target->maximum.y = std::max(target->maximum.y, bounds.maximum.y);
                target->maximum.z = std::max(target->maximum.z, bounds.maximum.z);
            }
            return Result::Success();
        };
        for (const auto &hero : typed_volumes)
            receivers.push_back({{hero.bounds_min.x, hero.bounds_min.y, hero.bounds_min.z, 0},
                {hero.bounds_max.x, hero.bounds_max.y, hero.bounds_max.z, 0}});
        // Captures use their private static CSM, so retain only their existing
        // depth-interval contribution. Froxels still request world shadow pages.
        for (const auto &probe : impl_->local_probes.Probes())
            receivers.push_back({{std::min({probe.influence_min.x, probe.projection_min.x, probe.position.x - probe.capture_distance}),
                    std::min({probe.influence_min.y, probe.projection_min.y, probe.position.y - probe.capture_distance}),
                    std::min({probe.influence_min.z, probe.projection_min.z, probe.position.z - probe.capture_distance}), 1.0f},
                {std::max({probe.influence_max.x, probe.projection_max.x, probe.position.x + probe.capture_distance}),
                    std::max({probe.influence_max.y, probe.projection_max.y, probe.position.y + probe.capture_distance}),
                    std::max({probe.influence_max.z, probe.projection_max.z, probe.position.z + probe.capture_distance}), 0}});
        if (owned_view.volumetrics.AllocatedBytes() != 0)
        {
            DirectX::XMFLOAT3 minimum{FLT_MAX, FLT_MAX, FLT_MAX}, maximum{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            const auto inverse = DirectX::XMLoadFloat4x4(&camera_relative_inverse_view_projection);
            for (const float z : {0.0f, 1.0f})
                for (const float y : {-1.0f, 1.0f})
                    for (const float x : {-1.0f, 1.0f})
                    {
                        DirectX::XMFLOAT3 corner;
                        DirectX::XMStoreFloat3(&corner, DirectX::XMVector3TransformCoord(
                            DirectX::XMVectorSet(x, y, z, 1), inverse));
                        corner.x += lod_eye.x; corner.y += lod_eye.y; corner.z += lod_eye.z;
                        minimum.x = std::min(minimum.x, corner.x); minimum.y = std::min(minimum.y, corner.y); minimum.z = std::min(minimum.z, corner.z);
                        maximum.x = std::max(maximum.x, corner.x); maximum.y = std::max(maximum.y, corner.y); maximum.z = std::max(maximum.z, corner.z);
                    }
            froxel_receiver_index = static_cast<std::uint32_t>(receivers.size());
            receivers.push_back({{minimum.x, minimum.y, minimum.z, 0}, {maximum.x, maximum.y, maximum.z, 0}});
            for (const auto &volume : impl_->volumetric_media)
            {
                if (volume.identity.w == static_cast<UINT>(VolumetricShape::Global)) continue;
                minimum = {FLT_MAX, FLT_MAX, FLT_MAX}; maximum = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
                const auto local_to_world = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&volume.world_to_local));
                for (const float z : {-1.0f, 1.0f}) for (const float y : {-1.0f, 1.0f}) for (const float x : {-1.0f, 1.0f})
                {
                    DirectX::XMFLOAT3 corner;
                    DirectX::XMStoreFloat3(&corner, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(x, y, z, 1), local_to_world));
                    minimum.x = std::min(minimum.x, corner.x); minimum.y = std::min(minimum.y, corner.y); minimum.z = std::min(minimum.z, corner.z);
                    maximum.x = std::max(maximum.x, corner.x); maximum.y = std::max(maximum.y, corner.y); maximum.z = std::max(maximum.z, corner.z);
                }
                receivers.push_back({{minimum.x, minimum.y, minimum.z, 0}, {maximum.x, maximum.y, maximum.z, 0}});
            }
        }
        const auto previous_static_batch=shadow_owner.static_shadow_casters;
        Impl::StaticShadowCasterBatch signature;
        signature.source=static_scene; signature.generation=static_caster_generation;
        signature.leaf_count=static_leaf_ref_count;
        signature.receiver_proxy_count=hlod_static_instances.size();
        signature.density=static_draws.foliage_density; signature.wind_radius=grass_visibility_radius;
        signature.material_revision=impl_->material_parameter_revision; signature.pipeline_revision=impl_->pipeline_revision;
        signature.mesh_shader=impl_->mesh_shader_enabled;
        for(std::size_t bucket=0;bucket<kGpuVisibilityBucketCount;++bucket)
        {
            const auto *page=bucket_pages[bucket];
            signature.geometry[bucket]=page ? (std::uint64_t(page->generation)<<32)|page->page_slot : 0;
            signature.passes[bucket]=static_geometry_passes[bucket][1];
        }
        signature.geometry.back()=(std::uint64_t(impl_->procedural_geometry.generation)<<32)|impl_->procedural_geometry.page_slot;
        signature.passes.back()=ground_geometry_passes[1];
        const bool static_warm=!new_session && previous_static_batch &&
            previous_static_batch->source==signature.source && previous_static_batch->generation==signature.generation &&
            previous_static_batch->leaf_count==signature.leaf_count && previous_static_batch->density==signature.density &&
            previous_static_batch->receiver_proxy_count==signature.receiver_proxy_count &&
            previous_static_batch->wind_radius==signature.wind_radius && previous_static_batch->material_revision==signature.material_revision &&
            previous_static_batch->pipeline_revision==signature.pipeline_revision && previous_static_batch->mesh_shader==signature.mesh_shader &&
            previous_static_batch->geometry==signature.geometry && previous_static_batch->passes==signature.passes;
        std::unordered_map<Impl::ShadowCasterMap::key_type,std::size_t,Impl::ShadowCasterKeyHash> old_static_indices,static_indices;
        if(static_warm) static_shadow_batch=previous_static_batch;
        else
        {
            static_shadow_batch=std::make_shared<Impl::StaticShadowCasterBatch>(std::move(signature));
            if(shadow_owner.static_shadow_caster_revision>UINT64_MAX-2) throw std::runtime_error("Static shadow caster revision exhausted");
            static_shadow_batch->revision=++shadow_owner.static_shadow_caster_revision;
            static_indices.reserve(static_leaf_ref_count);
            if(!new_session && previous_static_batch)
                for(std::size_t i=0;i<previous_static_batch->keys.size();++i) old_static_indices.emplace(previous_static_batch->keys[i],i);
        }
        // Camera shadow queries originate at raster surfaces, independently of
        // shadow-caster eligibility. Retain all static draw bounds once in the
        // existing immutable batch, including shadow-off surfaces and HLOD proxies.
        const auto append_surface=[&](const RenderInstance &source,const GpuInstance *draw,
            std::size_t dynamic_index,std::vector<ShadowSurfaceReceiver> &destination,
            const render_data::GeometryBounds *proxy_bounds=nullptr) -> Result {
            const bool environment=is_environment_mesh(source.mesh);
            const auto scale=draw?draw->scale:DirectX::XMFLOAT4{source.scale.x,source.scale.y,source.scale.z,1};
            const auto center=draw?draw->position:DirectX::XMFLOAT4{source.position.x,
                source.position.y+(environment?0.0f:source.scale.y*0.5f),source.position.z,0};
            const double yaw=draw?draw->yaw:source.yaw;
            if(!std::isfinite(center.x)||!std::isfinite(center.y)||!std::isfinite(center.z)||
               !std::isfinite(scale.x)||!std::isfinite(scale.y)||!std::isfinite(scale.z)||!std::isfinite(yaw))
                return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Camera shadow receiver transform is nonfinite.");
            std::array<double,3> lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
            const auto merge_local=[&](const auto &bounds) {
                for(unsigned axis=0;axis<3;++axis)
                { lo[axis]=std::min(lo[axis],double(bounds.minimum[axis]));hi[axis]=std::max(hi[axis],double(bounds.maximum[axis])); }
            };
            if(proxy_bounds) merge_local(*proxy_bounds); // HLOD pages contain baked world-space vertices.
            else if(environment)
            {
                const auto ordinal=environment_mesh_index(source);
                for(unsigned lod=0;lod<3;++lod) merge_local(impl_->environment_meshes[ordinal*3+lod].metadata.bounds);
            }
            else if(source.mesh==RenderMesh::Archer)
            {
                // The exact prepared palette is the SkinCache input. Every
                // nonnegative weighted matrix result is inside this bone/AABB
                // envelope; the source codec admits weight sums 1 +/- .001.
                if(dynamic_index>=archer_palette_slots.size()||archer_palette_slots[dynamic_index]==UINT32_MAX)
                { camera_receiver_domain_closed=false;return Result::Success(); }
                const auto &bounds=impl_->archer_geometry.metadata.bounds;
                const auto offset=archer_palette_data_offset+std::size_t(archer_palette_slots[dynamic_index])*archer_palette_stride;
                lo={0,0,0};hi={0,0,0};std::array<double,3> magnitude{};
                double source_l1{};
                for(unsigned bone=0;bone<impl_->archer_bone_count;++bone)
                {
                    DirectX::XMFLOAT4X4 matrix;
                    std::memcpy(&matrix,frame.mapped+offset+bone*sizeof(matrix),sizeof(matrix));
                    for(unsigned corner=0;corner<8;++corner)
                    {
                        const std::array<double,4> p{bounds.minimum[0],bounds.minimum[1],bounds.minimum[2],1};
                        auto q=p;
                        for(unsigned axis=0;axis<3;++axis) if(corner&(1u<<axis)) q[axis]=bounds.maximum[axis];
                        source_l1=std::max(source_l1,std::abs(q[0])+std::abs(q[1])+std::abs(q[2]));
                        for(unsigned axis=0;axis<3;++axis)
                        {
                            double value{},absolute{};
                            for(unsigned column=0;column<4;++column)
                            { const double term=double(matrix.m[axis][column])*q[column];value+=term;absolute+=std::abs(term); }
                            lo[axis]=std::min(lo[axis],value);hi[axis]=std::max(hi[axis],value);
                            magnitude[axis]=std::max(magnitude[axis],absolute);
                        }
                    }
                }
                constexpr double gamma64=(64*0x1p-24)/(1-64*0x1p-24);
                for(unsigned axis=0;axis<3;++axis)
                {
                    const double pad=gamma64*magnitude[axis]*1.0011+64*double(FLT_MIN)*(1+source_l1);
                    lo[axis]=lo[axis]*1.0011-pad;hi[axis]=hi[axis]*1.0011+pad;
                }
            }
            else if(is_monster(source.mesh))
            {
                const auto &asset=impl_->monster_assets[static_cast<UINT>(source.mesh)-static_cast<UINT>(RenderMesh::MonsterMelee)];
                if(!(asset.deformation_flags&render_data::kMonsterBoundsPositionEnclosure)||
                   dynamic_index>=dynamic_pose_times.size())
                { camera_receiver_domain_closed=false;return Result::Success(); }
                const auto clip=std::min((dynamic_pose_flags[dynamic_index]>>8u)&0xfu,4u);
                const auto meta=asset.clip_meta[clip];
                if(!meta.y) { camera_receiver_domain_closed=false;return Result::Success(); }
                if(!std::isfinite(dynamic_pose_times[dynamic_index]))
                    return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Camera skin receiver time is nonfinite.");
                const float frame_position=std::clamp(dynamic_pose_times[dynamic_index],0.0f,1.0f)*float(meta.y-1u);
                const auto frame0=std::min(static_cast<UINT>(std::floor(frame_position)),meta.y-1u);
                const auto selected_lod=dynamic_index<ordered_monster_lods.size()?ordered_monster_lods[dynamic_index]:0u;
                const auto *found=asset.IntervalBound(selected_lod,
                    static_cast<CharacterAnimationClip>(clip),frame0);
                if(!found) { camera_receiver_domain_closed=false;return Result::Success(); }
                for(unsigned axis=0;axis<3;++axis) { lo[axis]=found->minimum[axis];hi[axis]=found->maximum[axis]; }
            }
            else
            {
                merge_local(source.mesh==RenderMesh::EnemyProjectile?impl_->gel_projectile_geometry.metadata.bounds:
                    impl_->procedural_geometry.metadata.bounds);
                if(source.mesh==RenderMesh::Area||source.mesh==RenderMesh::Ground) { lo[1]*=.1;hi[1]*=.1; }
                else if(source.mesh==RenderMesh::DirtPatch) { lo[1]*=.035;hi[1]*=.035; }
                else if(source.mesh==RenderMesh::PlayerProjectile) { lo[1]*=.55;hi[1]*=.55; }
                else if(source.mesh==RenderMesh::Pickup)
                {
                    const double extent=std::max(std::abs(lo[0]),std::abs(hi[0]))+std::max(std::abs(lo[1]),std::abs(hi[1]));
                    lo[0]=lo[1]=-extent;hi[0]=hi[1]=extent;
                }
                // Other placeholder operations only shrink the source box.
            }
            const double sine=std::sin(yaw),cosine=std::cos(yaw);
            std::array<double,3> low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY},magnitude{};
            for(unsigned corner=0;corner<8;++corner)
            {
                const double x=(corner&1?hi[0]:lo[0])*scale.x,y=(corner&2?hi[1]:lo[1])*scale.y,z=(corner&4?hi[2]:lo[2])*scale.z;
                const std::array<double,3> p{center.x+cosine*x+sine*z,center.y+y,center.z-sine*x+cosine*z};
                const std::array<double,3> absolute{std::abs(double(center.x))+std::abs(x)+std::abs(z),std::abs(double(center.y))+std::abs(y),
                    std::abs(double(center.z))+std::abs(x)+std::abs(z)};
                for(unsigned axis=0;axis<3;++axis)
                { low[axis]=std::min(low[axis],p[axis]);high[axis]=std::max(high[axis],p[axis]);magnitude[axis]=std::max(magnitude[axis],absolute[axis]); }
            }
            const double wind=HasEnvironmentWind(source.mesh,source.environment_variant)?grass_visibility_radius:0;
            std::array<float,3> minimum{},maximum{};
            for(unsigned axis=0;axis<3;++axis)
            {
                const double pad=wind+magnitude[axis]*0x1p-18+1.e-5;
                if(!std::isfinite(low[axis])||!std::isfinite(high[axis])||low[axis]-pad<=-FLT_MAX||high[axis]+pad>=FLT_MAX)
                    return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Camera shadow receiver bounds are unbounded.");
                minimum[axis]=std::nextafter(float(low[axis]-pad),-FLT_MAX);
                maximum[axis]=std::nextafter(float(high[axis]+pad),FLT_MAX);
            }
            const ShadowSurfaceReceiver receiver{{minimum[0],minimum[1],minimum[2],0},{maximum[0],maximum[1],maximum[2],1}};
            destination.push_back(receiver);return Result::Success();
        };
        const auto append_caster = [&](const RenderInstance &source, const GpuStaticDrawRef &reference,
                std::uint32_t ref_index, bool immutable, const GpuInstance *instance,
                const ShadowSurfaceReceiver *qualified_surface=nullptr) -> Result {
            const bool environment = is_environment_mesh(source.mesh);
            const bool terrain = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch;
            const bool hero = source.mesh == RenderMesh::Archer || is_monster(source.mesh);
            if (!immutable && is_monster(source.mesh) &&
                (ref_index >= ordered_monster_lods.size() || ordered_monster_lods[ref_index] >= 3u))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Monster shadow reference has no valid selected LOD.");
            const auto monster_lod = !immutable && is_monster(source.mesh)
                ? ordered_monster_lods[ref_index] : 0u;
            const auto monster_family = static_cast<UINT>(source.mesh) - static_cast<UINT>(RenderMesh::MonsterMelee);
            const std::uint32_t bucket = environment ? static_cast<UINT>(environment_mesh_index(source) * 3 + 2)
                : source.mesh == RenderMesh::Archer ? kArcherVisibilityBucket
                : is_monster(source.mesh) ? kMonsterVisibilityBucketBegin +
                    (monster_lod == 0 ? monster_family : 6u + 2u * monster_family + monster_lod - 1u)
                : UINT32_MAX;
            const auto *page = bucket != UINT32_MAX ? bucket_pages[bucket] : &impl_->procedural_geometry;
            const auto pass = bucket != UINT32_MAX
                ? (immutable ? static_geometry_passes[bucket][1] : dynamic_geometry_passes[bucket][1])
                : immutable ? ground_geometry_passes[1] : terrain ? dynamic_terrain_geometry_passes[1] : procedural_geometry_passes[1];
            if (!page || pass == 0) return Result::Failure(ErrorCode::InvalidState,
                "hs_renderer_d3d12", "Virtual caster has no complete geometry record.");
            const UINT ordinal = environment ? render_data::EnvironmentGeometryMaterialOrdinal(bucket / 3) : terrain ? kTerrainMaterialOrdinal : UINT32_MAX;
            const UINT material = ordinal < kAuthoredMaterialNames.size()
                ? (impl_->material_parameter_bases[ordinal] << 1) | 1u : kDefaultSceneDrawMaterial | 1u;
            const auto binding_key = (std::uint64_t(material) << 32) | (immutable ? 0x80000000u : 0u) | page->page_slot;
            const auto binding_slot = bucket != UINT32_MAX ? static_cast<std::size_t>(bucket)
                : kGpuVisibilityBucketCount + (terrain ? 0u : 1u);
            const auto draw_group = ResolveShadowCasterGroup(
                shadow_group_indices[immutable ? 1u : 0u][binding_slot], groups, binding_key, [&] {
                    // The pipeline belongs to the draw group, not each static instance.
                    auto *pipeline = environment ? environment_pipeline(bucket, true)
                        : hero ? impl_->hero_shadow_pipeline.Get() : impl_->shadow_pipeline.Get();
                    groups.push_back({binding_key, impl_->GeometryUsesMesh(pipeline, *page, 1)
                        ? VirtualShadowFrontend::Mesh : VirtualShadowFrontend::Indexed,
                        page->index_count,0,0,page->meshlet_count});
                    bindings.push_back({page, pipeline, material, immutable});
                });
            VirtualShadowCaster caster;
            const auto key = std::tuple{immutable, source.stable_id,
                source.stable_id == 0 ? reference.instance_index + 1u : 0u};
            Impl::ShadowCasterRecord previous_static_record;
            const Impl::ShadowCasterRecord *previous{};
            if(immutable)
            {
                if(const auto found=old_static_indices.find(key);found!=old_static_indices.end())
                { previous_static_record.caster=previous_static_batch->records[found->second]; previous=&previous_static_record; }
            }
            else if(const auto found=owned_view.previous_shadow_casters.find(key);found!=owned_view.previous_shadow_casters.end()) previous=&found->second;
            const auto cut_revision = (std::uint64_t(page->generation) << 32) | page->page_slot;
            const auto instance_index = reference.instance_index | (immutable ? 0x80000000u : 0u);
            const auto &sphere = page->metadata.bounds.sphere;
            const float sx = sphere[0] * source.scale.x, sy = sphere[1] * source.scale.y, sz = sphere[2] * source.scale.z;
            const auto center = instance ? instance->position : DirectX::XMFLOAT4{
                source.position.x, source.position.y + (environment ? 0.0f : source.scale.y * 0.5f), source.position.z, 0};
            const float maximum_scale = std::max({std::abs(source.scale.x), std::abs(source.scale.y), std::abs(source.scale.z)});
            // StaticGpuScene assigns a fresh, nonwrapping instance generation
            // whenever its strong immutable scene lease changes. Together with
            // the page revision, this proves the local sphere transform is unchanged.
            const bool reuse_static_center = immutable && !new_session &&
                previous && previous->caster.immutable_source &&
                previous->caster.instance_index == instance_index &&
                previous->caster.instance_generation == reference.instance_generation &&
                previous->caster.current_cut_revision == cut_revision;
            if (reuse_static_center)
            {
                const auto &old = previous->caster.current_bounds;
                caster.current_bounds = {old.x, old.y, old.z, sphere[3] * maximum_scale};
            }
            else
            {
                const auto caster_yaw = instance ? instance->yaw : source.yaw;
                const float sine = std::sin(caster_yaw), cosine = std::cos(caster_yaw);
                caster.current_bounds = {center.x + cosine * sx + sine * sz, center.y + sy,
                    center.z - sine * sx + cosine * sz, sphere[3] * maximum_scale};
            }
            caster.skin = hero;
            caster.wind = HasEnvironmentWind(source.mesh, source.environment_variant);
            caster.animated_alpha = source.mesh == RenderMesh::Grass;
            if (caster.skin && instance)
            {
                // SkinCache's conservative pose radius is centered on the actual
                // interpolated draw origin, independently of the rest-pose sphere.
                caster.current_bounds = {center.x, center.y, center.z, std::max(center.w,
                    std::sqrt(sx*sx + sy*sy + sz*sz) + sphere[3] * maximum_scale)};
            }
            if (caster.wind) caster.current_bounds.w += grass_visibility_radius;
            if(qualified_surface && hero)
            {
                // Share the qualified actual-pose enclosure with page overlap,
                // command admission and invalidation, not only receiver proof.
                const auto sphere=ShadowSurfaceCasterSphere(*qualified_surface);
                if(!sphere)
                    return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Qualified skin shadow radius is unbounded.");
                caster.current_bounds={sphere->x,sphere->y,sphere->z,sphere->w};
            }
            if (environment && environment_mesh_index(source) == render_data::kMeadowWaterGeometry)
            {
                const auto scale = instance ? instance->scale : DirectX::XMFLOAT4{source.scale.x, source.scale.y, source.scale.z, 1};
                if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Water receiver scale is nonfinite.");
                // This origin-centered radius includes the local offset and every resident LOD.
                const double radius = double(impl_->environment_variant_radii[render_data::kMeadowWaterGeometry]) *
                    std::max({std::abs(double(scale.x)), std::abs(double(scale.y)), std::abs(double(scale.z))});
                if (auto merged = merge_receiver(water_receiver, center, radius); !merged) return merged;
            }
            caster.current_revisions = immutable ? source.revisions
                : impl_->dynamic_scene_slots.Revisions(reference.instance_index);
            caster.current_revisions.material ^= impl_->material_parameter_revision;
            caster.current_cut_revision = cut_revision;
            caster.instance_index = instance_index;
            caster.instance_generation = reference.instance_generation;
            caster.source_ref = ref_index;
            caster.geometry_record = pass >> 2;
            caster.draw_group = draw_group;
            caster.immutable_source = immutable;
            caster.baked_source = immutable && render_data::IsStaticShadowCaster(source);
            Impl::ShadowCasterRecord *current{};
            if(immutable)
            {
                if(!static_indices.emplace(key,static_shadow_batch->records.size()).second) return Result::Success();
                static_shadow_batch->keys.push_back(key);
            }
            else
            {
                current=&current_shadow_casters.try_emplace(key).first->second;
                const bool inserted=MarkShadowCasterPrepared(current->prepared_epoch,caster_preparation_epoch);
#ifndef NDEBUG
                if(impl_->config.validation || impl_->config.gpu_validation)
                    assert(inserted==MarkShadowCasterKeyOracle(caster_key_oracle,key,caster_preparation_epoch));
#endif
                if(!inserted) return Result::Success();
                ++prepared_casters;
            }
            if (!new_session && previous)
            {
                if(!immutable) ++matched_previous_casters;
                const auto &old = previous->caster;
                caster.previous_bounds = old.current_bounds;
                caster.previous_revisions = old.current_revisions;
                caster.previous_cut_revision = old.current_cut_revision;
                caster.previous_generation = old.instance_generation;
                caster.previous_present = true;
                caster.previous_immutable_source = old.immutable_source;
                caster.previous_skin = old.skin;
                caster.previous_wind = old.wind;
                caster.previous_animated_alpha = old.animated_alpha;
            }
            if(immutable) static_shadow_batch->records.push_back(caster);
            else { current->caster=caster; casters.push_back(caster); }
            if (is_monster(source.mesh) && static_cast<UINT>(source.mesh) - static_cast<UINT>(RenderMesh::MonsterMelee) < 3)
            {
                const auto &b = caster.current_bounds;
                receivers.push_back({{b.x-b.w,b.y-b.w,b.z-b.w,0}, {b.x+b.w,b.y+b.w,b.z+b.w,1}});
            }
            return Result::Success();
        };
        // Static groups form a stable prefix; fluctuating enemy families cannot renumber them.
        if(static_warm)
        {
            for(const auto &binding:static_shadow_batch->bindings)
            {
                groups.push_back({binding.key,binding.frontend,binding.page->index_count,0,0,
                    binding.page->meshlet_count});
                bindings.push_back({binding.page,binding.pipeline,binding.material,true});
            }
            water_receiver=static_shadow_batch->water;
            receivers.insert(receivers.end(),static_shadow_batch->receivers.begin(),static_shadow_batch->receivers.end());
        }
        else
        {
            const auto receiver_begin=receivers.size();
            if(static_scene && impl_->static_gpu_scene)
                for(std::size_t index=0;index<static_leaf_ref_count;++index)
                {
                    if(auto added=append_surface(static_scene->instances[static_draw_refs[index].instance_index],nullptr,
                        SIZE_MAX,static_shadow_batch->surface_receivers);!added)return added;
                    if(auto added=append_caster(static_scene->instances[static_draw_refs[index].instance_index],
                        static_draw_refs[index],static_cast<UINT>(index),true,nullptr);!added) return added;
                }
            for(std::size_t index=0;index<hlod_static_instances.size();++index)
                if(auto added=append_surface(hlod_static_instances[index],nullptr,SIZE_MAX,static_shadow_batch->surface_receivers,
                    &impl_->hlod_runtime->scene.proxies.at(index).bounds);!added)return added;
            for(const auto &surface:static_shadow_batch->surface_receivers)
            {
                auto &aggregate=static_shadow_batch->surface_aggregate;
                if(!aggregate) aggregate=surface;
                else
                {
                    aggregate->minimum.x=std::min(aggregate->minimum.x,surface.minimum.x);
                    aggregate->minimum.y=std::min(aggregate->minimum.y,surface.minimum.y);
                    aggregate->minimum.z=std::min(aggregate->minimum.z,surface.minimum.z);
                    aggregate->maximum.x=std::max(aggregate->maximum.x,surface.maximum.x);
                    aggregate->maximum.y=std::max(aggregate->maximum.y,surface.maximum.y);
                    aggregate->maximum.z=std::max(aggregate->maximum.z,surface.maximum.z);
                }
            }
            static_shadow_batch->water=water_receiver;
            static_shadow_batch->receivers.assign(receivers.begin()+receiver_begin,receivers.end());
            for(std::size_t i=0;i<bindings.size();++i)
                static_shadow_batch->bindings.push_back({bindings[i].page,bindings[i].pipeline,bindings[i].material,
                    groups[i].frontend,groups[i].binding_key});
            if(!new_session && previous_static_batch)
                for(std::size_t i=0;i<previous_static_batch->keys.size();++i)
                    if(!static_indices.contains(previous_static_batch->keys[i])) removed_static_indices.push_back(i);

        }
        dynamic_surface_receivers.reserve(dynamic_draw_refs.size());
        for (std::size_t index = 0; index < dynamic_draw_refs.size(); ++index)
        {
            if (dynamic_cooked_skin_bounds[index] && !dynamic_skin_needed[index])
                continue;
            const auto surface_begin=dynamic_surface_receivers.size();
            if(auto added=append_surface(render_instances[index],&persistent_instances[dynamic_draw_refs[index].instance_index],
                index,dynamic_surface_receivers);!added)return added;
            const auto *qualified_surface=dynamic_surface_receivers.size()>surface_begin?&dynamic_surface_receivers.back():nullptr;
            // Gel is a receiver even though it has no opaque shadow representation.
            if (render_instances[index].mesh == RenderMesh::EnemyProjectile)
            {
                const auto &draw = persistent_instances[dynamic_draw_refs[index].instance_index];
                if (!std::isfinite(draw.scale.x) || !std::isfinite(draw.scale.y) ||
                    !std::isfinite(draw.scale.z) || !std::isfinite(draw.yaw) || !std::isfinite(draw.position.w))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Gel receiver transform is nonfinite.");
                const auto &sphere = impl_->gel_projectile_geometry.metadata.bounds.sphere;
                const double origin_radius = std::sqrt(double(sphere[0])*sphere[0] +
                    double(sphere[1])*sphere[1] + double(sphere[2])*sphere[2]) + sphere[3];
                // Mesh6 has no skin/wind/charge deformation. Retain any larger current pose bound.
                const double radius = std::max(double(draw.position.w), origin_radius *
                    std::max({std::abs(double(draw.scale.x)), std::abs(double(draw.scale.y)), std::abs(double(draw.scale.z))}));
                if (auto merged = merge_receiver(gel_receiver, draw.position, radius); !merged) return merged;
                continue;
            }
            if (auto added = append_caster(render_instances[index], dynamic_draw_refs[index], static_cast<UINT>(index),
                    false, &persistent_instances[dynamic_draw_refs[index].instance_index],qualified_surface); !added) return added;
        }
        // At most two additional entries, independent of water/projectile instance counts.
        if (water_receiver) { water_receiver->maximum.w=1;receivers.push_back(*water_receiver); }
        if (gel_receiver) { gel_receiver->maximum.w=1;receivers.push_back(*gel_receiver); }
        PruneShadowCasterStorage(current_shadow_casters, caster_preparation_epoch, prepared_casters);
#ifndef NDEBUG
        if (impl_->config.validation || impl_->config.gpu_validation)
        {
            PruneShadowCasterKeyOracle(caster_key_oracle, caster_preparation_epoch, prepared_casters,
                shadow_owner.shadow_caster_key_oracle_prepared);
            assert(caster_key_oracle.size() == prepared_casters);
        }
#endif
        assert(current_shadow_casters.size() == prepared_casters);
        // Unique current keys matching every previous key prove that no caster was removed.
        if (!new_session && matched_previous_casters != owned_view.previous_shadow_casters.size())
        {
            std::vector<const Impl::ShadowCasterMap::value_type *> removed_casters;
            for (const auto &entry : owned_view.previous_shadow_casters)
                if (!current_shadow_casters.contains(entry.first)) removed_casters.push_back(&entry);
            // Keep the former ordered-map order when removed casters invalidate cached pages.
            std::ranges::sort(removed_casters, {}, [](const auto *entry) { return entry->first; });
            for (const auto *entry : removed_casters)
            {
                const auto &old = entry->second.caster;
                auto removed = old;
                removed.present = false;
                removed.previous_present = true;
                removed.previous_bounds = old.current_bounds;
                removed.previous_revisions = old.current_revisions;
                removed.previous_cut_revision = old.current_cut_revision;
                removed.previous_generation = old.instance_generation;
                removed.previous_immutable_source = old.immutable_source;
                removed.previous_skin = old.skin;
                removed.previous_wind = old.wind;
                removed.previous_animated_alpha = old.animated_alpha;
                casters.push_back(removed);
            }
        }
        std::ranges::sort(removed_static_indices,{},[&](std::size_t index) { return previous_static_batch->keys[index]; });
        for(const auto index:removed_static_indices)
        {
            auto removed=previous_static_batch->records[index];
            AdvanceShadowCasterHistory(removed); removed.present=false;
            casters.push_back(removed);
        }
        inputs.frustum_receiver_index = froxel_receiver_index;
        inputs.prepare_main_csm = [&](std::array<ShadowViewProjection,3> &matrices) {
            prepare_cascades();
            for(std::size_t cascade=0;cascade<3;++cascade)
                for(std::size_t row=0;row<4;++row)for(std::size_t column=0;column<4;++column)
                    matrices[cascade][row*4+column]=constants->shadow_view_projection[cascade].m[column][row];
            return true;
        };
        inputs.view_id = impl_->active_view_id;
        inputs.required_receiver_domains=ShadowReceiverCamera;
        inputs.closed_receiver_domains=camera_receiver_domain_closed?ShadowReceiverCamera:0u;
        // Brixelizer's sun-shadow cache seed uses these same GBuffer surfaces.
        // Only actual world/froxel queries add receiver bounds beyond the camera.
        if(!typed_volumes.empty()||owned_view.volumetrics.AllocatedBytes()!=0)
        { inputs.required_receiver_domains|=ShadowReceiverWorld;inputs.closed_receiver_domains|=ShadowReceiverWorld; }
        DirectX::XMStoreFloat4x4(&inputs.receiver_view_projection,clip_matrix);
        inputs.surface_receivers={static_shadow_batch->surface_receivers,dynamic_surface_receivers,
            static_shadow_batch->surface_aggregate?std::span<const ShadowSurfaceReceiver>(&*static_shadow_batch->surface_aggregate,1):
                std::span<const ShadowSurfaceReceiver>{}};
        if (primary_sun) inputs.light = *primary_sun;
        DirectX::XMStoreFloat3(&inputs.clipmap_center, target);
        inputs.view_origin = lod_eye;
        inputs.inverse_view_projection = camera_relative_inverse_view_projection;
        for (UINT i = 0; i < 3; ++i) inputs.coverage_extents[i] = cascade_extents[i] * 0.5f;
        inputs.baked_static=primary_sun && impl_->material_parameter_revision==impl_->baked_generation_material_revision &&
            impl_->pipeline_revision==impl_->baked_generation_pipeline_revision &&
            owned_view.virtual_shadows.MatchesBake(static_scene,*primary_sun,inputs.coverage_extents);
        inputs.depth_span = shadow_distance * 2;
        inputs.render_active = owned_view.state.render_active;
        inputs.frame_slot = frame_slot;
        inputs.rendered_frame_id = owned_view.frame_number;
        inputs.completed = impl_->graph_queue_executor.Completed();
        inputs.casters = ShadowCasterRange{static_shadow_batch->records,casters};
        inputs.stable_caster_revision=static_shadow_batch->revision;
        inputs.receivers = receivers; inputs.draw_groups = groups;
        if (primary_sun && owned_view.virtual_shadows.AllocatedBytes() != 0)
        {
            if (auto prepared = owned_view.virtual_shadows.PrepareFrame(inputs, shadow_plan); !prepared) return prepared;
            csm_required = shadow_plan.guarantee.csm_required;
        }
    }
    assert(shadow_plan.constants.root_options.w == 0u || !cascades_prepared);
    if (csm_required) prepare_cascades();
    const UINT active_view_mask = csm_required ? 15u : 1u;
    const UINT active_view_count = csm_required ? kGpuVisibilityViewCount : 1u;
    record_scene_substage(cpu_timings.scene_shadow_prepare_us);
    // Software streaming demand is independent of GPU draw compaction. Merge
    // camera and cascade error before requesting a family's finer resident page.
    std::array<std::array<DirectX::XMFLOAT4, 6>, 4> demand_planes{};
    const auto make_demand_planes = [&](std::size_t index, DirectX::CXMMATRIX matrix) {
        const auto columns = DirectX::XMMatrixTranspose(matrix);
        const DirectX::XMVECTOR planes[]{
            DirectX::XMVectorAdd(columns.r[3], columns.r[0]),
            DirectX::XMVectorSubtract(columns.r[3], columns.r[0]),
            DirectX::XMVectorAdd(columns.r[3], columns.r[1]),
            DirectX::XMVectorSubtract(columns.r[3], columns.r[1]),
            columns.r[2], DirectX::XMVectorSubtract(columns.r[3], columns.r[2])};
        for (std::size_t plane = 0; plane < 6; ++plane)
            DirectX::XMStoreFloat4(&demand_planes[index][plane],
                                  DirectX::XMPlaneNormalize(planes[plane]));
    };
    make_demand_planes(0, view * projection);
    for (std::size_t cascade = 0; cascade + 1 < active_view_count; ++cascade)
        make_demand_planes(cascade + 1, DirectX::XMMatrixTranspose(
            DirectX::XMLoadFloat4x4(&constants->shadow_view_projection[cascade])));
    std::array<std::array<float, 4>, 6> demand_plane_lengths{};
    for (std::size_t consumer = 0; consumer < active_view_count; ++consumer)
        for (std::size_t plane = 0; plane < 6; ++plane)
        {
            const auto &p = demand_planes[consumer][plane];
            const float length = std::sqrt((p.x * p.x + p.y * p.y) + p.z * p.z);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                !std::isfinite(p.w) || !std::isfinite(length))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                    "Cluster refinement requires finite active frustum planes.");
            demand_plane_lengths[plane][consumer] = length;
        }
    for (std::size_t ordinal = 0; ordinal < cluster_inputs.size(); ++ordinal)
    {
        auto &cut = cluster_inputs[ordinal].constants;
        cut.camera_origin_near = {constants->camera_time.x, constants->camera_time.y,
            constants->camera_time.z, constants->lod_options.z};
        cut.view_right = {constants->lod_view_right.x, constants->lod_view_right.y, constants->lod_view_right.z, 0};
        cut.view_up = {constants->lod_view_up.x, constants->lod_view_up.y, constants->lod_view_up.z, 0};
        cut.view_forward = {constants->lod_view_forward.x, constants->lod_view_forward.y,
            constants->lod_view_forward.z, 0};
        cut.projection_scale = {constants->lod_projection.x, constants->lod_projection.y,
            constants->lod_projection.z, constants->lod_projection.w};
        cut.error_tolerance.fill(constants->lod_options.x);
        cut.geometry_frontend_flags = impl_->mesh_shader_enabled ? kGeometryMeshEligible : 0u;
        cut.material_word = (impl_->material_parameter_bases[render_data::EnvironmentGeometryMaterialOrdinal(ordinal)] << 1u) | 1u;
        for (std::size_t consumer = 0; consumer < active_view_count; ++consumer)
            for (std::size_t plane = 0; plane < 6; ++plane)
            {
                const auto &p = demand_planes[consumer][plane];
                cut.frustum_planes[consumer][plane] = {p.x, p.y, p.z, p.w};
                cut.frustum_plane_lengths[plane][consumer] = demand_plane_lengths[plane][consumer];
            }
    }
    if (auto *hlod_view = impl_->ActiveHlodView(); hlod_view && hlod_view->frames[frame_slot].enabled)
    {
        auto &hlod_views = hlod_view->frames[frame_slot].views;
        for (std::size_t index = 0; index < active_view_count; ++index)
        {
            auto &hlod_constants = hlod_views[index];
            hlod_constants.origin_near = {constants->camera_time.x, constants->camera_time.y,
                constants->camera_time.z, constants->lod_options.z};
            hlod_constants.forward = constants->lod_view_forward;
            for (std::size_t plane = 0; plane < 6; ++plane)
                hlod_constants.frustum_planes[plane] = demand_planes[index][plane];
            const float projection_scale = index == 0 ? constants->lod_projection.x
                : index == 1 ? constants->lod_projection.y : index == 2 ? constants->lod_projection.z : constants->lod_projection.w;
            hlod_constants.error_policy = {projection_scale, constants->lod_options.x, 0.15f, index == 0 ? 0.0f : 1.0f};
        }
    }
    for (auto &input : cluster_inputs) input.constants.active_view_mask = active_view_mask;
    if (auto prepared = impl_->PrepareClusterFrame(owned_view, frame_slot, cluster_inputs); !prepared) return prepared;
    record_scene_substage(cpu_timings.scene_cluster_prepare_us);
    std::array<float, kEnvironmentMaterialNames.size()> geometry_demand{};
    const float shadow_demand_projection[]{constants->lod_projection.y,
        constants->lod_projection.z, constants->lod_projection.w};
    const auto accumulate_geometry_demand = [&](const auto &source, std::uint32_t consumers,
                                                std::uint32_t inside_consumers) {
        if (!is_environment_mesh(source.mesh)) return;
        const auto ordinal = environment_mesh_index(source);
        const auto &coarse = impl_->environment_meshes[ordinal * 3 + 1];
        const auto scale = std::max({std::abs(source.scale.x), std::abs(source.scale.y),
                                     std::abs(source.scale.z)});
        const auto deformation = HasEnvironmentWind(source.mesh, source.environment_variant) ? grass_visibility_radius : 0.0f;
        const auto radius = impl_->environment_variant_radii[ordinal] * scale + deformation;
        const auto error = coarse.metadata.geometric_error * scale + 2.0f * deformation;
        const auto &center = source.position;
        for (std::size_t consumer = 0; consumer < active_view_count; ++consumer)
        {
            if ((consumers & (1u << consumer)) == 0) continue;
            const float shadow_error = consumer == 0 ? 0.0f
                : error * shadow_demand_projection[consumer - 1];
            // A shadow view cannot raise the per-catalog maximum after a
            // larger visible instance/view has already supplied its demand.
            if (consumer != 0 && shadow_error <= geometry_demand[ordinal]) continue;
            bool visible = true;
            if ((inside_consumers & (1u << consumer)) == 0)
                for (const auto &plane : demand_planes[consumer])
                    visible = visible && center.x * plane.x + center.y * plane.y +
                        center.z * plane.z + plane.w >= -radius;
            if (!visible) continue;
            float projected_error{};
            if (consumer == 0)
            {
                DirectX::XMFLOAT3 view_center{};
                DirectX::XMStoreFloat3(&view_center, DirectX::XMVector3Transform(
                    DirectX::XMVectorSet(center.x, center.y, center.z, 1), view));
                const auto distance = view_center.z - radius - error;
                if (distance <= constants->lod_options.z)
                    projected_error = constants->lod_options.x * 2.0f;
                else
                {
                    const auto safe_distance = std::max(distance, constants->lod_options.z);
                    const auto rho = std::hypot(std::abs(view_center.x) + radius + error,
                                                std::abs(view_center.y) + radius + error) / safe_distance;
                    projected_error = error * constants->lod_projection.x *
                        std::sqrt(1.0f + rho * rho) / safe_distance;
                }
            }
            else
            {
                projected_error = shadow_error;
            }
            geometry_demand[ordinal] = std::max(geometry_demand[ordinal], projected_error);
        }
    };
    const auto all_demand_consumers = (1u << active_view_count) - 1u;
    for (const auto &source : render_instances) accumulate_geometry_demand(source, all_demand_consumers, 0);
    if (static_scene)
        for (std::size_t ordinal = 0; ordinal < cluster_active.size(); ++ordinal)
        {
            if (cluster_active[ordinal]) continue;
            for (const auto &cell : static_draws.geometry_demand_cells[ordinal])
            {
                const float deformation = cell.any_wind ? grass_visibility_radius : 0.0f;
                const float radius = impl_->environment_variant_radii[ordinal] * cell.max_scale + deformation;
                const float error = impl_->environment_meshes[ordinal * 3 + 1].metadata.geometric_error *
                    cell.max_scale + 2.0f * deformation;
                std::uint32_t consumers{}, inside_consumers{};
                for (std::size_t consumer = 0; consumer < active_view_count; ++consumer)
                {
                    const double upper = consumer == 0
                        ? GeometryDemandCellErrorUpperBound(cell, radius, error, lod_view.m,
                            constants->lod_projection.x, constants->lod_options.z)
                        : error * shadow_demand_projection[consumer - 1];
                    if (upper <= geometry_demand[ordinal]) continue;
                    const auto visibility = ClassifyGeometryDemandCell(cell, radius, demand_planes[consumer]);
                    if (visibility == GeometryDemandCellVisibility::Outside) continue;
                    consumers |= 1u << consumer;
                    if (visibility == GeometryDemandCellVisibility::Inside) inside_consumers |= 1u << consumer;
                }
                if (!consumers) continue;
                // Cells only prune work; the original float calculation still chooses the exact priority.
                for (const auto instance : cell.instances)
                    accumulate_geometry_demand(static_scene->instances[instance], consumers, inside_consumers);
            }
        }
    for (std::uint32_t ordinal = 0; ordinal < geometry_demand.size(); ++ordinal)
        if (geometry_demand[ordinal] > constants->lod_options.x)
            if (auto requested = impl_->DemandEnvironmentGeometry(ordinal,
                    std::bit_cast<UINT>(std::max(geometry_demand[ordinal] / constants->lod_options.x, 1.0f)));
                !requested) return requested;
    record_scene_substage(cpu_timings.scene_geometry_demand_us);
    // Measure ground UV derivatives through the actual unjittered owned_view. The
    // smallest authored terrain tile is 9m (TerrainTileSize, material_terrain).
    float terrain_footprint = std::numeric_limits<float>::infinity();
    if (!impl_->config.grid_floor_probe && impl_->environment_textures[0].resource)
    {
        const auto inverse = DirectX::XMLoadFloat4x4(&constants->inverse_unjittered_view_projection);
        const auto ground_point = [&](float u, float v, DirectX::XMFLOAT3 &point) {
            const auto near_point = DirectX::XMVector3TransformCoord(
                DirectX::XMVectorSet(u * 2 - 1, 1 - v * 2, 1, 1), inverse);
            DirectX::XMFLOAT3 origin{}, direction{};
            DirectX::XMStoreFloat3(&origin, eye);
            DirectX::XMStoreFloat3(&direction, DirectX::XMVectorSubtract(near_point, eye));
            if (direction.y >= 0.0f || origin.y <= 0.0f) return false;
            const auto scale = -origin.y / direction.y;
            point = {origin.x + direction.x * scale, 0, origin.z + direction.z * scale};
            return std::isfinite(point.x) && std::isfinite(point.z) &&
                   std::hypot(point.x - origin.x, point.z - origin.z) <= 500.0f;
        };
        for (const auto v : {0.1f, 0.5f, 0.9f})
            for (const auto u : {0.1f, 0.5f, 0.9f})
            {
                DirectX::XMFLOAT3 p{}, dx{}, dy{};
                if (!ground_point(u, v, p) || !ground_point(u + 1.0f / owned_view.render_width, v, dx) ||
                    !ground_point(u, v + 1.0f / owned_view.render_height, dy)) continue;
                const auto footprint = std::max(std::hypot(dx.x - p.x, dx.z - p.z),
                                               std::hypot(dy.x - p.x, dy.z - p.z));
                if (footprint > 0.0f)
                    terrain_footprint = std::min(terrain_footprint, footprint);
            }
    }
    const auto demand_material = [&](std::uint32_t material, std::uint32_t mip, std::uint32_t priority,
                                     bool baseline_consumer) {
        for (std::uint32_t slot = 0; slot < render_data::kMaterialTextureSlots; ++slot)
        {
            const auto id = impl_->material_texture_stream_ids[material * render_data::kMaterialTextureSlots + slot];
            if (id && (baseline_consumer || !impl_->texture_streaming.HardwareBacked(id)))
                impl_->texture_streaming.Demand(id, mip, priority, owned_view.frame_number);
        }
    };
    const bool capture_demand=static_scene&&impl_->local_probes.AllocatedBytes()&&
        impl_->local_probes.CaptureIdentityPending(completed_streaming);
    std::array<bool, kEnvironmentMaterialNames.size()> environment_material_requested{};
    const auto demand_environment_material = [&](const RenderInstance &source, bool capture_consumer) {
        if (!is_environment_mesh(source.mesh)) return;
        const auto material = static_cast<std::uint32_t>(environment_mesh_index(source));
        if (environment_material_requested[material]) return;
        const auto radius = impl_->environment_variant_radii[material] *
            std::max({std::abs(source.scale.x), std::abs(source.scale.y), std::abs(source.scale.z)}) +
            (HasEnvironmentWind(source.mesh, source.environment_variant) ? grass_visibility_radius : 0.0f);
        bool visible = capture_consumer || active_features.software_gi_enabled;
        for (const auto &planes : demand_planes)
        {
            if (visible) break;
            bool in_view = true;
            for (const auto &plane : planes)
                in_view = in_view && source.position.x * plane.x + source.position.y * plane.y +
                    source.position.z * plane.z + plane.w >= -radius;
            visible = visible || in_view;
        }
        if (!visible) return;
        environment_material_requested[material] = true;
        // Geometry has no authored UV density. Software conservatively requests
        // the finest allowed mip for actual visible owners; raster hardware
        // demand comes from real samples. Software GI consumers use software demand.
        demand_material(render_data::EnvironmentGeometryMaterialOrdinal(material), 0, std::bit_cast<UINT>(std::max(geometry_demand[material], 1.0f)),
            active_features.software_gi_enabled);
    };
    for (const auto &source : render_instances) demand_environment_material(source, false);
    if (static_scene)
    {
        const bool capture_consumer = capture_demand;
        for (std::size_t ordinal = 0; ordinal < environment_material_requested.size(); ++ordinal)
        {
            const auto [begin, count] = static_environment_ranges[ordinal * 3];
            for (std::size_t index = begin; index < begin + count && !environment_material_requested[ordinal]; ++index)
                demand_environment_material(static_scene->instances[static_draw_refs[index].instance_index], capture_consumer);
        }
    }
    if (!impl_->config.grid_floor_probe && std::isfinite(terrain_footprint))
        for (std::uint32_t slot = 0; slot < render_data::kMaterialTextureSlots; ++slot)
        {
            const auto id = impl_->material_texture_stream_ids[kTerrainMaterialOrdinal * render_data::kMaterialTextureSlots + slot];
            if (!id || (impl_->texture_streaming.HardwareBacked(id) && !active_features.software_gi_enabled)) continue;
            const auto logical = impl_->texture_streaming.View(id).logical_description;
            const auto rho = std::max(static_cast<float>(logical.width) * terrain_footprint / 9.0f, 1.0f);
            impl_->texture_streaming.Demand(id, static_cast<std::uint32_t>(std::floor(std::log2(rho))),
                std::bit_cast<UINT>(1.0f / terrain_footprint), owned_view.frame_number);
        }
    if (static_draws.ground_range.second && capture_demand)
        demand_material(kTerrainMaterialOrdinal, 0, 1, true);
    if(capture_demand)
    {
        for(const auto material:impl_->local_probes.CachedCaptureMaterialOrdinals(static_scene,foliage_density))
            demand_material(material,0,1,true);
    }
    if (auto result = impl_->texture_streaming.Pump(owned_view.frame_number, completed_streaming,
            impl_->asset_io, impl_->streaming, impl_->residency); !result) return result;
    impl_->PublishEnvironmentMaterialTextures(frame_slot);
    if (auto result = impl_->texture_feedback.PrepareFrame(frame_slot, completed_streaming,
            impl_->material_texture_stream_ids, impl_->texture_streaming, impl_->residency); !result) return result;
    auto feedback_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    feedback_cpu.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
        kTextureFeedbackDescriptorOffset) * impl_->srv_stride;
    auto feedback_gpu = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    feedback_gpu.ptr += (static_cast<UINT64>(frame_slot) * kTextureDescriptorCount +
        kTextureFeedbackDescriptorOffset) * impl_->srv_stride;
    if (auto result = impl_->texture_feedback.WriteDescriptors(frame_slot, impl_->srv_heap.Get(),
            impl_->srv_cpu_heap.Get(), feedback_cpu, feedback_gpu, impl_->srv_stride); !result) return result;
    record_scene_substage(cpu_timings.scene_texture_prepare_us);
    const bool upload_dynamic_header =
        !current_instance_snapshot->initialized ||
        required_slots != current_instance_snapshot->slot_count;
    if (upload_dynamic_header)
    {
        std::memcpy(frame.mapped + kInstanceDataOffset,
                    &dynamic_scene_layout.header,
                    sizeof(dynamic_scene_layout.header));
        dynamic_scene_copy_regions.push_back(
            {0, sizeof(dynamic_scene_layout.header), 0});
        dynamic_scene_staging_bytes = sizeof(dynamic_scene_layout.header);
    }
    auto &serialized = frame.scene_serialization_scratch;
    serialized.resize(required_slots);
    const auto uploaded_scene_revision = current_instance_snapshot->uploaded_revision;
    impl_->dynamic_scene_slots.CollectDirtyRangesSince(
        uploaded_scene_revision, dirty_instance_ranges, frame.scene_channel_dirty_scratch);
    for (const auto &range : dirty_instance_ranges)
    {
        for (std::size_t index = 0; index < range.count; ++index)
        {
            const auto slot = range.begin + index;
            serialized[slot] = SerializeGpuSceneSlot(
                static_cast<std::uint32_t>(slot),
                persistent_instances[slot], persistent_metadata[slot]);
        }
    }
    const auto append_entries =
        [&](GpuSceneChannel channel, std::size_t destination_base, std::size_t stride, auto member) {
            const auto &ranges = frame.scene_channel_dirty_scratch[static_cast<std::size_t>(channel)];
            for (const auto &range : ranges)
            {
                const auto staging_offset = dynamic_scene_staging_bytes;
                for (std::size_t index = 0; index < range.count; ++index)
                {
                    const auto &entry = serialized[range.begin + index].*member;
                    std::memcpy(
                        frame.mapped + kInstanceDataOffset +
                            dynamic_scene_staging_bytes,
                        &entry, stride);
                    dynamic_scene_staging_bytes += stride;
                }
                dynamic_scene_copy_regions.push_back(
                    {destination_base + range.begin * stride,
                     range.count * stride, staging_offset});
#if defined(HS_DEVELOPMENT_TOOLS)
                dynamic_instance_upload_bytes += range.count * stride;
#endif
            }
        };
    const auto &header = dynamic_scene_layout.header;
    append_entries(GpuSceneChannel::Instance, header.instance_offset, sizeof(GpuSceneInstanceEntry),
                       &GpuSceneSerializedSlot::instance);
    append_entries(GpuSceneChannel::Transform, header.transform_offset, sizeof(GpuSceneTransformEntry),
                       &GpuSceneSerializedSlot::transform);
    append_entries(GpuSceneChannel::Mesh, header.mesh_offset, sizeof(GpuSceneMeshEntry),
                       &GpuSceneSerializedSlot::mesh);
    append_entries(GpuSceneChannel::Material, header.material_offset, sizeof(GpuSceneMaterialEntry),
                       &GpuSceneSerializedSlot::material);
    append_entries(GpuSceneChannel::Bounds, header.bounds_offset, sizeof(GpuSceneBoundsEntry),
                       &GpuSceneSerializedSlot::bounds);
    append_entries(GpuSceneChannel::Skin, header.skin_offset, sizeof(GpuSceneSkinEntry),
                       &GpuSceneSerializedSlot::skin);
    // Each submitted backing owns its packet's entity-surface records.
    const bool upload_entity_surface_data =
        (!current_instance_snapshot->initialized ||
         current_instance_snapshot->surface_package_id != impl_->active_render_package_id) &&
        !entity_surface_frame.Bytes().empty();
    if (upload_entity_surface_data)
    {
        const auto bytes = entity_surface_frame.Bytes();
        std::memcpy(frame.mapped + kInstanceDataOffset + dynamic_scene_staging_bytes,
                    bytes.data(), bytes.size());
        dynamic_scene_copy_regions.push_back(
            {dynamic_scene_layout.entity_surface_offset, bytes.size(), dynamic_scene_staging_bytes});
        dynamic_scene_staging_bytes += bytes.size();
    }
    if (dynamic_scene_staging_bytes > dynamic_scene_staging_capacity)
        return Result::Failure(
            ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Dynamic split GPU scene staging exceeded its reserved frame-upload range.");
    if (!dynamic_draw_refs.empty())
        std::memcpy(frame.mapped + dynamic_ref_data_offset, dynamic_draw_refs.data(),
                    dynamic_draw_refs.size() * sizeof(GpuStaticDrawRef));

    record_scene_substage(cpu_timings.scene_upload_plan_us);
    cpu_timings.scene_gpu_prepare_us = capture_elapsed(scene_gpu_prepare_start);
    const bool upload_static_scene = static_scene && impl_->static_gpu_scene &&
                                      !impl_->static_gpu_scene->submitted;
    const bool upload_static_refs = static_scene && impl_->static_gpu_scene &&
        impl_->static_gpu_scene->refs && !impl_->static_gpu_scene->refs->submitted;
    const bool upload_dynamic_scene =
        upload_dynamic_header || !dirty_instance_ranges.empty() || upload_entity_surface_data;
    const bool upload_environment_stream = impl_->environment_stream.upload_pending;
    const bool upload_environment_geometry =
        std::ranges::any_of(impl_->environment_geometry_streams,
                           [](const auto &stream) { return stream.upload_pending; });
    auto &ui_texture = owned_view.ui.textures[frame_slot];
    HRESULT result{};

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(owned_view.render_width),
                                  static_cast<float>(owned_view.render_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(owned_view.render_width),
                             static_cast<LONG>(owned_view.render_height)};
    const D3D12_VIEWPORT output_viewport{0.0f, 0.0f, static_cast<float>(owned_view.width),
                                         static_cast<float>(owned_view.height), 0.0f, 1.0f};
    const D3D12_RECT output_scissor{0, 0, static_cast<LONG>(owned_view.width),
                                    static_cast<LONG>(owned_view.height)};
    const auto rtv_start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto rtv_at = [&](std::uint32_t index) {
        return D3D12_CPU_DESCRIPTOR_HANDLE{
            rtv_start.ptr + static_cast<SIZE_T>(kFrameCount + frame_slot * kRtvBankStride + index) * impl_->rtv_stride};
    };
    const auto output_rtv = presented_view
        ? D3D12_CPU_DESCRIPTOR_HANDLE{rtv_start.ptr + static_cast<SIZE_T>(output_index) * impl_->rtv_stride}
        : rtv_at(kOutputRtvOffset + output_index);
    const auto rtv = output_rtv;
    if (!presented_view)
        impl_->device->CreateRenderTargetView(frame_output.Get(), nullptr, rtv);
    const auto gbuffer_base_rtv = rtv_at(0);
    const auto gbuffer_normal_rtv = rtv_at(1);
    const auto gbuffer_metadata_rtv = rtv_at(2);
    const auto hdr_rtv = rtv_at(3);
    const auto oit_accumulation_rtv = rtv_at(4);
    const auto oit_revealage_rtv = rtv_at(5);
    const auto post_a_rtv = rtv_at(6);
    const auto post_b_rtv = rtv_at(7);
    const auto scene_work_rtv = rtv_at(kViewRtvOffset + 4);
    const auto display_linear_rtv = rtv_at(kViewRtvOffset + 5);
    const auto composite_hdr_rtv = rtv_at(kViewRtvOffset + 6);
    const auto gbuffer_material_rtv = rtv_at(8);
    const auto gbuffer_motion_rtv = rtv_at(9);
    const auto gbuffer_surface_history_rtv = rtv_at(kSurfaceHistoryRtvOffset);
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> surface_rtvs{
        gbuffer_base_rtv, gbuffer_normal_rtv, gbuffer_metadata_rtv, gbuffer_material_rtv,
        gbuffer_motion_rtv, gbuffer_surface_history_rtv,
        rtv_at(kMaterialSidecarRtvOffset), rtv_at(kMaterialSidecarRtvOffset + 1)};
    const auto surface_target_count = 6u + impl_->material_sidecar_layout.plane_count;
    const auto distortion_rtv = rtv_at(kDistortionRtvOffset);
    const auto bloom_half_rtv = rtv_at(kBloomRtvOffset);
    const auto bloom_quarter_rtv = rtv_at(kBloomRtvOffset + 1);
    const auto bloom_half_combined_rtv = rtv_at(kBloomRtvOffset + 2);
    const auto temporal_read_index = owned_view.temporal_read_index;
    const auto temporal_write_index = temporal_read_index ^ 1u;
    const auto temporal_write_rtv = rtv_at(kTemporalRtvOffset + temporal_write_index);
    const std::array temporal_rtvs{temporal_write_rtv,
        rtv_at(kViewRtvOffset + temporal_write_index),
        rtv_at(kViewRtvOffset + 2 + temporal_write_index)};
    const auto exposure_read_index = owned_view.exposure_read_index;
    const auto exposure_write_index = exposure_read_index ^ 1u;
    impl_->WriteViewDescriptors(owned_view, frame_slot, temporal_read_index, temporal_write_index,
        exposure_read_index, exposure_write_index,
        frame.upload.resource->GetGPUVirtualAddress() + kExposureConstantsOffset);
    // Optional fog sampling reads VmCounts even when fog is Off. Replace the
    // view's null default before descriptor clones; active fog overrides this.
    constexpr auto disabled_volumetric_offset = kDisabledVolumetricConstantsOffset;
    constexpr auto disabled_volumetric_size = kDisabledVolumetricConstantsSize;
    static_assert(disabled_volumetric_offset + disabled_volumetric_size <= kExposureConstantsOffset);
    static_assert(kExposureConstantsOffset < kInstanceDataOffset);
    std::memset(frame.mapped + disabled_volumetric_offset, 0, disabled_volumetric_size);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC disabled_volumetric_cbv{
        frame.upload.resource->GetGPUVirtualAddress() + disabled_volumetric_offset,
        disabled_volumetric_size};
    auto volumetric_cbv = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    volumetric_cbv.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
        kVolumetricDescriptorOffset) * impl_->srv_stride;
    impl_->device->CreateConstantBufferView(&disabled_volumetric_cbv, volumetric_cbv);
    const bool initialize_temporal_targets = owned_view.targets.temporal_history[0].resource &&
        !owned_view.temporal_targets_initialized;
    const bool initialize_offscreen_output = !presented_view &&
        !output_completion.direct && !output_completion.compute && !output_completion.copy;
    auto dsv = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(frame_slot * 5) * impl_->dsv_stride;
    auto readonly_dsv = dsv;
    readonly_dsv.ptr += 4 * impl_->dsv_stride;
    ID3D12DescriptorHeap *descriptor_heaps[] = {impl_->srv_heap.Get(), impl_->sampler_heap.Get()};
    auto sampler_table = impl_->sampler_heap->GetGPUDescriptorHandleForHeapStart();
    sampler_table.ptr += static_cast<UINT64>(frame_slot) * 2 * impl_->sampler_stride;
    auto texture_table = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    texture_table.ptr += static_cast<UINT64>(frame_slot) *
                         kTextureDescriptorCount * impl_->srv_stride;
    auto scene_skin_table = texture_table;
    scene_skin_table.ptr += static_cast<UINT64>(kViewSkinDescriptorOffset) * impl_->srv_stride;
    auto ribbon_skin_table = scene_skin_table;
    ribbon_skin_table.ptr += 2u * impl_->srv_stride;
    auto view_skin_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    view_skin_cpu.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
        kViewSkinDescriptorOffset) * impl_->srv_stride;
    const auto write_view_skin_srv = [&](std::uint32_t entry, ID3D12Resource *resource,
                                         std::uint64_t offset, std::uint64_t bytes) {
        D3D12_SHADER_RESOURCE_VIEW_DESC descriptor{};
        descriptor.Format = DXGI_FORMAT_R32_TYPELESS;
        descriptor.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        descriptor.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        descriptor.Buffer.FirstElement = offset / 4u;
        descriptor.Buffer.NumElements = static_cast<UINT>(bytes / 4u);
        descriptor.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        auto destination = view_skin_cpu;
        destination.ptr += entry * impl_->srv_stride;
        impl_->device->CreateShaderResourceView(resource, &descriptor, destination);
    };
    const auto &previous_scene = reads_previous_instance_scene
        ? previous_instance_snapshot->scene : dynamic_scene;
    write_view_skin_srv(0, previous_scene.resource.Get(), 0, previous_scene.resource->GetDesc().Width);
    write_view_skin_srv(1, frame.upload.resource.Get(), view_skin_data_offset,
        view_skin_slot_capacity * sizeof(GpuSceneSkinEntry));
    write_view_skin_srv(2, impl_->ribbon_history.resource.Get(), 0,
        impl_->ribbon_history.resource ? impl_->ribbon_history.resource->GetDesc().Width : 4u);
    auto view_skin_source = view_skin_cpu;
    view_skin_source.ptr += impl_->srv_stride;
    auto ribbon_skin_destination = view_skin_cpu;
    ribbon_skin_destination.ptr += 3u * impl_->srv_stride;
    impl_->device->CopyDescriptorsSimple(1, ribbon_skin_destination, view_skin_source,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto distortion_table = texture_table;
    // Share the frame-transient SRV layout used by descriptor creation. The
    // preceding environment table can grow without shifting this binding.
    distortion_table.ptr += static_cast<UINT64>(kVfxDistortionDescriptorOffset) * impl_->srv_stride;
    auto decal_table = texture_table;
    decal_table.ptr += static_cast<UINT64>(kVfxDistortionDescriptorOffset + 3u) * impl_->srv_stride;
    auto bloom_table = texture_table;
    bloom_table.ptr += static_cast<UINT64>(kBloomTextureDescriptorOffset) * impl_->srv_stride;
    auto temporal_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    temporal_cpu.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
                         kTemporalTextureDescriptorOffset) * impl_->srv_stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC temporal_view{};
    temporal_view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    temporal_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    temporal_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    temporal_view.Texture2D.MipLevels = 1;
    impl_->device->CreateShaderResourceView(
        owned_view.temporal_history_valid
            ? owned_view.targets.temporal_history[temporal_read_index].resource.Get() : nullptr, &temporal_view,
        temporal_cpu);
    temporal_cpu.ptr += impl_->srv_stride;
    impl_->device->CreateShaderResourceView(
        owned_view.targets.temporal_history[temporal_write_index].resource.Get(), &temporal_view,
        temporal_cpu);
    auto temporal_table = texture_table;
    temporal_table.ptr += static_cast<UINT64>(kTemporalTextureDescriptorOffset) *
                          impl_->srv_stride;
    temporal_table.ptr += impl_->srv_stride;
    const auto alternate_texture_table = [&](std::uint32_t offset,
                                              ID3D12Resource *input) {
        auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        destination.ptr += (static_cast<SIZE_T>(frame_slot) *
                            kTextureDescriptorCount + offset) * impl_->srv_stride;
        destination.ptr += 8 * impl_->srv_stride; // t10 (PostB) in the t2..t11 table.
        impl_->device->CreateShaderResourceView(input, &temporal_view, destination);
        auto table = texture_table;
        table.ptr += static_cast<UINT64>(offset) * impl_->srv_stride;
        return table;
    };
    auto tone_map_input = texture_table;
    if (!impl_->config.bloom)
        tone_map_input = alternate_texture_table(
            kToneMapInputDescriptorOffset,
            owned_view.targets.temporal_history[temporal_write_index].resource.Get());
    auto fxaa_input = texture_table;
    if (!impl_->config.outline)
        fxaa_input = alternate_texture_table(kFxaaInputDescriptorOffset,
                                             owned_view.targets.post_a.resource.Get());
    auto character_table = texture_table;
    character_table.ptr += static_cast<UINT64>(kPostTextureDescriptorCount) *
                           impl_->srv_stride;
    auto curve_table = character_table;
    curve_table.ptr += static_cast<UINT64>(kCharacterDescriptorCount + kMonsterPbrDescriptorCount +
        kEnvironmentDescriptorCount + kVfxGradientDescriptorCount + 3) * impl_->srv_stride;
    const auto bind_graphics = [&](ID3D12GraphicsCommandList *list) {
        list->SetGraphicsRootSignature(impl_->root_signature.Get());
        list->SetDescriptorHeaps(static_cast<UINT>(std::size(descriptor_heaps)), descriptor_heaps);
        list->SetGraphicsRootDescriptorTable(0, sampler_table);
        const std::array<UINT, 2> draw_defaults{0u, kDefaultSceneDrawMaterial};
        list->SetGraphicsRoot32BitConstants(6, 2, draw_defaults.data(), 0);
        list->SetGraphicsRootDescriptorTable(5, texture_table);
        list->SetGraphicsRootDescriptorTable(13, character_table);
        list->SetGraphicsRootDescriptorTable(19, scene_skin_table);
        list->SetGraphicsRootDescriptorTable(24, curve_table);
        list->SetGraphicsRootDescriptorTable(26, distortion_table);
        list->SetGraphicsRootDescriptorTable(29, decal_table);
        list->SetGraphicsRootDescriptorTable(30, bloom_table);
        auto history_table = texture_table;
        history_table.ptr += static_cast<UINT64>(kTemporalTextureDescriptorOffset) * impl_->srv_stride;
        list->SetGraphicsRootDescriptorTable(32, history_table);
        history_table.ptr += impl_->srv_stride;
        list->SetGraphicsRootDescriptorTable(33, history_table);
        auto hero_table = texture_table;
        hero_table.ptr += static_cast<UINT64>(kHeroTransparencyDescriptorOffset) * impl_->srv_stride;
        list->SetGraphicsRootDescriptorTable(36, hero_table);
        list->SetGraphicsRootConstantBufferView(15, frame.upload.resource->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(25, frame.upload.resource->GetGPUVirtualAddress() + ground_gap_offset);
        list->SetGraphicsRootShaderResourceView(28, frame.upload.resource->GetGPUVirtualAddress() + decal_data_offset);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    const auto bind_compute = [&](ID3D12GraphicsCommandList *list) {
        list->SetComputeRootSignature(impl_->root_signature.Get());
        list->SetComputeRootConstantBufferView(15, frame.upload.resource->GetGPUVirtualAddress());
        list->SetDescriptorHeaps(static_cast<UINT>(std::size(descriptor_heaps)), descriptor_heaps);
        list->SetComputeRootDescriptorTable(0, sampler_table);
        list->SetComputeRootDescriptorTable(19, scene_skin_table);
        auto history_table = texture_table;
        history_table.ptr += static_cast<UINT64>(kTemporalTextureDescriptorOffset) * impl_->srv_stride;
        list->SetComputeRootDescriptorTable(32, history_table);
        const std::array<UINT, 2> defaults{};
        list->SetComputeRoot32BitConstants(6, 2, defaults.data(), 0);
    };
    const auto bind_surface_compute = [&](ID3D12GraphicsCommandList *list) {
        bind_compute(list);
        list->SetComputeRootDescriptorTable(13, character_table);
        list->SetComputeRootDescriptorTable(26, distortion_table);
        list->SetComputeRootShaderResourceView(18, previous_skin_frame_valid
            ? previous_skin_frame->skin_cache_vertices.resource->GetGPUVirtualAddress()
            : frame.upload.resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(20, have_skinned_instances
            ? frame.skin_cache_vertices.resource->GetGPUVirtualAddress()
            : frame.upload.resource->GetGPUVirtualAddress());
    };
    const auto draw_fullscreen_with_table =
        [&](ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline, D3D12_CPU_DESCRIPTOR_HANDLE target,
            D3D12_GPU_DESCRIPTOR_HANDLE input_table) {
            list->OMSetRenderTargets(1, &target, FALSE, nullptr);
            list->SetPipelineState(pipeline);
            list->SetGraphicsRootDescriptorTable(5, input_table);
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        };
    const auto draw_fullscreen =
        [&](ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline, D3D12_CPU_DESCRIPTOR_HANDLE target) {
            draw_fullscreen_with_table(list, pipeline, target, texture_table);
        };

    ID3D12Resource *static_ref_resource = static_scene && impl_->static_gpu_scene &&
        impl_->static_gpu_scene->refs
        ? impl_->static_gpu_scene->refs->storage.resource.Get() : frame.upload.resource.Get();
    if (use_visibility)
        if (auto result = impl_->WriteVisibilityDescriptors(owned_view, frame_slot,
                static_scene && impl_->static_gpu_scene
                    ? impl_->static_gpu_scene->instances.resource.Get()
                    : dynamic_scene.resource.Get(),
                dynamic_scene.resource.Get(), static_ref_resource, 0,
                static_cast<std::uint32_t>(static_draw_refs.size()), dynamic_ref_data_offset,
                static_cast<std::uint32_t>(dynamic_draw_refs.size())); !result) return result;
    cpu_timings.constants_upload_us = measure_stage();
    auto graph_substage_start = capture_clock();
    const auto record_graph_substage = [&](std::uint64_t &duration) {
        if (!impl_->config.capture_timing_stream) return;
        const auto end = std::chrono::steady_clock::now();
        duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            end - graph_substage_start).count());
        graph_substage_start = end;
    };
    auto graph_resource_detail_start = capture_clock();
    const auto record_graph_resource_detail = [&](std::uint64_t &duration) {
        duration += capture_elapsed(graph_resource_detail_start);
        graph_resource_detail_start = capture_clock();
    };
    std::size_t hero_candidate_count{};
    if (has_translucent_hero_binding(0))
        hero_candidate_count += archer_range.second;
    for (std::size_t asset = 0; asset < monster_ranges.size(); ++asset)
        if (has_translucent_hero_binding(asset + 1))
            hero_candidate_count += monster_ranges[asset].second;
    if (impl_->static_gpu_scene)
        hero_candidate_count += static_environment_ranges[render_data::kMeadowWaterGeometry * 3].second;
    const auto hero_active = owned_view.state.render_active;
    const D3D12_RECT full_hero_rect{0, 0, static_cast<LONG>(hero_active.width),
        static_cast<LONG>(hero_active.height)};
    D3D12_RECT hero_rect{full_hero_rect.right, full_hero_rect.bottom, 0, 0};
    bool hero_bounds_complete = hero_candidate_count != 0 &&
        hero_active.width == owned_view.render_width && hero_active.height == owned_view.render_height;
    bool hero_all_outside = hero_bounds_complete;
    const renderer_detail::HeroCoverageProjection hero_projection{hero_active.width, hero_active.height,
        lod_projection._11, lod_projection._22, jitter_x, jitter_y, near_plane, far_plane};
    const auto include_hero_sphere = [&](DirectX::XMFLOAT4 sphere) {
        const double position_error = 128.0 * FLT_EPSILON *
            (std::abs(double(sphere.x)) + std::abs(double(sphere.y)) +
             std::abs(double(sphere.z)) + std::abs(double(sphere.w)) + 1.0);
        const auto eye = DirectX::XMVector3TransformCoord(
            DirectX::XMVectorSet(sphere.x, sphere.y, sphere.z, 1.0f), view);
        const auto projected = renderer_detail::ProjectHeroSphere(hero_projection,
            DirectX::XMVectorGetX(eye), DirectX::XMVectorGetY(eye), DirectX::XMVectorGetZ(eye),
            double(sphere.w) + position_error + 0.01);
        using Kind = renderer_detail::HeroProjectedBounds::Kind;
        if (projected.kind == Kind::Outside) return true;
        hero_all_outside = false;
        if (projected.kind == Kind::Unknown) return false;
        hero_rect.left = std::min(hero_rect.left, static_cast<LONG>(projected.left));
        hero_rect.top = std::min(hero_rect.top, static_cast<LONG>(projected.top));
        hero_rect.right = std::max(hero_rect.right, static_cast<LONG>(projected.right));
        hero_rect.bottom = std::max(hero_rect.bottom, static_cast<LONG>(projected.bottom));
        const auto area = std::uint64_t(hero_rect.right - hero_rect.left) * (hero_rect.bottom - hero_rect.top);
        const auto full_area = std::uint64_t(hero_active.width) * hero_active.height;
        // A visible dense set cannot be empty; keep its existing full-copy policy.
        return hero_candidate_count <= 8 && area * 4 < full_area * 3;
    };
    if (hero_bounds_complete && has_translucent_hero_binding(0))
        for (std::size_t index = archer_range.first;
             index < archer_range.first + archer_range.second && hero_bounds_complete; ++index)
            if (index >= render_instances.size() || !dynamic_cooked_skin_bounds[index] ||
                !include_hero_sphere(current_gpu_instance(index).position))
            { hero_bounds_complete = false; hero_all_outside = false; break; }
    if (hero_bounds_complete)
        for (std::size_t asset = 0; asset < monster_ranges.size() && hero_bounds_complete; ++asset)
        {
            const auto [begin, count] = monster_ranges[asset];
            if (!has_translucent_hero_binding(asset + 1)) continue;
            for (std::size_t index = begin; index < begin + count; ++index)
                if (index >= render_instances.size() || !dynamic_cooked_skin_bounds[index] ||
                    !include_hero_sphere(current_gpu_instance(index).position))
                { hero_bounds_complete = false; hero_all_outside = false; break; }
        }
    if (hero_bounds_complete && static_scene)
    {
        const auto [begin, count] = static_environment_ranges[render_data::kMeadowWaterGeometry * 3u];
        for (std::size_t index = begin; index < begin + count; ++index)
        {
            const auto slot = static_draw_refs[index].instance_index;
            if (slot >= static_scene->instances.size())
            { hero_bounds_complete = false; hero_all_outside = false; break; }
            const auto &water = static_scene->instances[slot];
            const auto scale = std::max({std::abs(water.scale.x), std::abs(water.scale.y), std::abs(water.scale.z)});
            if (!include_hero_sphere({water.position.x, water.position.y, water.position.z,
                    impl_->environment_variant_radii[render_data::kMeadowWaterGeometry] * scale}))
            { hero_bounds_complete = false; hero_all_outside = false; break; }
        }
    }
    const auto hero_composition_rect = hero_bounds_complete && !hero_all_outside ? hero_rect : full_hero_rect;
    // Explicit work capture retains its diagnostic phases even for an empty raster domain.
#if defined(HS_DEVELOPMENT_TOOLS)
    const bool capture_hero_work = impl_->config.capture_hero_work;
#else
    constexpr bool capture_hero_work = false;
#endif
    const bool have_hero_contributors = hero_candidate_count != 0 &&
        (!hero_all_outside || capture_hero_work);
    const bool hero_occupancy_topology = have_hero_contributors && hero_candidate_count <= 2 &&
        !capture_hero_work;
    std::uint32_t hero_work_source_mask{};
#if defined(HS_DEVELOPMENT_TOOLS)
    if (capture_hero_work && have_hero_contributors)
    {
        if (has_translucent_hero_binding(0) && archer_range.second)
            hero_work_source_mask |= 1u;
        for (std::size_t asset = 0; asset < monster_bucket_ranges.size(); ++asset)
            if (has_translucent_monster_bucket(asset) && monster_bucket_ranges[asset].second)
                hero_work_source_mask |= 1u << (asset + 1);
        if (impl_->static_gpu_scene)
            for (std::size_t lod = 0; lod < 3; ++lod)
                if (static_environment_ranges[render_data::kMeadowWaterGeometry * 3 + lod].second)
                    hero_work_source_mask |= 1u << (lod + 19);
    }
#endif
    const bool composite_gradient_read = flash_add_count || ground_add_count ||
        std::ranges::any_of(ribbon_outputs, [](const GpuRibbonOutput &output) {
            return (output.metadata[3] & 1u) == 0;
        });
    const auto screen_intensity = impl_->explicit_frame->screen_overlay_intensity
        ? *impl_->explicit_frame->screen_overlay_intensity : impl_->config.screen_overlay_intensity;
    const bool fxaa_identity = constants->temporal_options.x > 0.5f ||
        (constants->lod_clock.w & 2u) != 0;
    const bool has_static_authored_material_draw = static_scene && impl_->static_gpu_scene->has_authored_material;
    const bool has_static_masked_material_draw = static_scene && impl_->static_gpu_scene->has_masked_material;
    const bool has_authored_material_draw = has_static_authored_material_draw ||
        std::ranges::any_of(render_instances, authored_source);
    const bool has_masked_material_draw = has_static_masked_material_draw ||
        std::ranges::any_of(render_instances, masked_source);
    const auto cluster_record = impl_->PrepareClusterFrameRecordData(frame_slot, use_visibility);
    const auto ao_csm_adaptive_status = owned_view.adaptive_shading.Status();
    const bool overlap_ao_csm = presented_view &&
        impl_->explicit_frame->views.size() == 1 && !impl_->config.environment_preview &&
        !impl_->mesh_shader_enabled && !use_visibility && impl_->enhanced &&
        active_features.ao_mode == GraphicsAoMode::GTAO &&
        active_features.gi_mode == GraphicsGiMode::Probe &&
        active_features.reflection_mode == GraphicsReflectionMode::ScreenSpace &&
        active_features.shadow_mode == GraphicsShadowMode::Raster &&
        !active_features.software_gi_enabled &&
        active_features.adaptive_compute_enabled && ao_csm_adaptive_status.compute_ready &&
        ao_csm_adaptive_status.usable && owned_view.ambient_occlusion.AdaptiveComputeApplied() &&
        !(owned_view.reconstruction_provider && owned_view.reconstruction_provider->Initialized()) &&
        !impl_->sdk_presentation.IsEnabled() &&
        impl_->sdk_presentation.LatencyOwner() == SdkLatencyOwner::None;
    std::uint64_t brixelizer_light_epoch{};
    if (active_features.software_gi_enabled)
    {
        brixelizer_light_epoch = impl_->pipeline_revision ^ (impl_->material_parameter_revision * 1099511628211ull);
        if (const auto digest = RendererProductDigest(impl_->config, "render/lighting/default_probe.rpkg"); !digest.empty())
            brixelizer_light_epoch ^= MakeAssetId(digest).value;
        if (owned_view.brixelizer_history_valid && (new_session ||
                !(owned_view.temporal_history_valid || owned_view.hzb_history_valid) ||
                owned_view.brixelizer_gi.Status().light_revision != brixelizer_light_epoch))
        {
            if (auto drained = impl_->WaitForGpu(); !drained) return drained;
            if (auto reset = owned_view.brixelizer_gi.ResetHistoryAfterCompletion(
                    impl_->graph_queue_executor.Completed(), owned_view.state.render_active, new_session); !reset) return reset;
            owned_view.brixelizer_history_valid = false;
        }
    }
    if (constants->vfx_decal_count.x && !owned_view.decal_surface.AllocatedBytes())
    {
        DecalSurfaceCandidate candidate;
        if (auto prepared = impl_->PrepareDecalSurface(impl_->config,
                owned_view.state.render_capacity, true, candidate); !prepared) return prepared;
#if defined(HS_DEVELOPMENT_TOOLS)
        auto transition = impl_->DiagnosticsMemory();
        transition.renderer_view_bytes += candidate.AllocatedBytes();
        impl_->diagnostics.ObserveTransition(transition);
#endif
        if (auto committed = owned_view.decal_surface.Commit(std::move(candidate),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    }
    if (!typed_screen.empty() && screen_intensity > 0 && !owned_view.screen_overlay.AllocatedBytes())
    {
        auto overlay_config = impl_->config;
        overlay_config.screen_overlay_intensity = screen_intensity;
        ScreenOverlayCandidate candidate;
        if (auto prepared = impl_->PrepareScreenOverlay(overlay_config,
                owned_view.state.render_capacity, candidate); !prepared) return prepared;
        if (auto committed = owned_view.screen_overlay.Commit(std::move(candidate),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    }
    bool devtools_ui_hook{};
#if defined(HS_DEVELOPMENT_TOOLS)
    devtools_ui_hook = imgui_draw_data && !impl_->sdk_presentation.IsEnabled();
#endif
    record_graph_resource_detail(cpu_timings.graph_resource_pre_shadow_us);
    LocalShadowInputs local_shadow_input;
    LocalShadowFramePlan local_shadow_plan;
    const bool local_shadow_planned = owned_view.local_shadows.AllocatedBytes() != 0;
#if defined(HS_DEVELOPMENT_TOOLS)
    const bool full_timing_capture = impl_->config.capture_timing_stream && impl_->config.capture_gpu_timings;
#else
    constexpr bool full_timing_capture = false;
#endif
    const bool output_dependency_interval = presented_view && dynamic_resolution_active && !full_timing_capture;
    const bool source_output_ready = full_timing_capture ||
        (presented_view && impl_->sdk_presentation.IsEnabled());
    const FrameGraphLayout::EarlyKey early_key{
        frame_slot, owned_view.render_width, owned_view.render_height, owned_view.width, owned_view.height,
        impl_->material_sidecar_layout.plane_count,
        presented_view, use_visibility, impl_->enhanced, have_skinned_instances,
        previous_skin_frame_valid, static_scene != nullptr,
        !owned_view.exposure_initialized, owned_view.ui_dirty[frame_slot],
        initialize_temporal_targets, initialize_offscreen_output,
        upload_static_scene, upload_static_refs, upload_dynamic_scene, upload_environment_stream,
        upload_environment_geometry, separate_previous_instance_scene,
        !frame.geometry_draw_records_cpu.empty(),
        have_hero_contributors, hero_occupancy_topology, capture_hero_work,
        composite_gradient_read,
        owned_view.dynamic_lod_states.initialized,
        impl_->static_gpu_scene && impl_->static_gpu_scene->lod_states.initialized,
        static_cast<bool>(owned_view.dynamic_lod_states.prefix_source.resource),
        impl_->static_gpu_scene && static_cast<bool>(impl_->static_gpu_scene->lod_states.prefix_source.resource),
        use_visibility && !owned_view.visibility_targets_initialized, csm_required,
        has_authored_material_draw, has_masked_material_draw,
        has_static_authored_material_draw, has_static_masked_material_draw,
        use_native_reconstruction, impl_->config.bloom, impl_->config.outline,
        fxaa_identity, !owned_view.view_targets_initialized,
        !typed_screen.empty() && screen_intensity > 0, !entity_surface_frame.Bytes().empty(),
        constants->vfx_decal_count.x != 0,
        impl_->config.motion_blur != GraphicsMotionBlur::Off, impl_->config.depth_of_field,
        impl_->config.color_grading, presented_view && impl_->sdk_presentation.IsEnabled(),
        devtools_ui_hook,
        output_dependency_interval, source_output_ready,
        active_features.software_gi_enabled, impl_->config.contact_shadows,
        active_features.adaptive_raster_enabled, active_features.adaptive_compute_enabled,
        overlap_ao_csm,
        active_features.ao_mode, active_features.gi_mode,
        active_features.reflection_mode, active_features.shadow_mode,
        static_cast<std::uint32_t>(std::ranges::count_if(impl_->environment_geometry_streams,
            [](const auto &stream) { return stream.upload_pending; })),
        hero_work_source_mask};
    std::vector<std::uint32_t> geometry_page_slots;
    std::array<bool, kMonsterReducedGeometryPage + 12u> geometry_page_seen{};
    for (const auto &record : frame.geometry_draw_records_cpu)
    {
        if (record.page_slot >= geometry_page_seen.size() || !record.geometry_generation ||
            geometry_page_seen[record.page_slot]) continue;
        geometry_page_seen[record.page_slot] = true;
        geometry_page_slots.push_back(record.page_slot);
    }
    std::ranges::sort(geometry_page_slots);
    std::uint32_t excluded_masked_count{}, excluded_thin_count{}, excluded_translucent_count{};
    record_graph_resource_detail(cpu_timings.graph_resource_scene_hlod_us);
    std::vector<SceneGeometryRecord> software_scene_job_inputs;
    std::vector<SceneDrawInstance> software_scene_draw_inputs;
    std::vector<const GeometryPageGpu *> software_scene_pages;
    struct SoftwareSceneSource
    {
        const RenderInstance *source{};
        const Impl::StaticGpuScene::SoftwareSceneBase *static_base{};
    };
    std::vector<SoftwareSceneSource> software_scene_sources;
    std::shared_ptr<const Impl::StaticGpuScene::SoftwareSceneCache::Rows> software_scene_static_rows;
    std::size_t software_scene_current_job_count{};
    SceneGeometryPreparedFrame software_scene_prepared;
    std::shared_ptr<const SceneGeometryStaticSection> software_scene_static_section;
    SceneGeometryRecordData scene_shape;
    std::vector<std::uint32_t> software_scene_page_slots;
    const auto prepare_software_scene = [&]() -> Result {
    if (active_features.software_gi_enabled)
    {
        const auto append_scene = [&](const RenderInstance &source, const GpuInstance &draw,
                std::uint32_t stable_slot, std::uint32_t generation, std::uint32_t skin_slot,
                std::uint16_t selected_lod) -> Result {
            const bool environment = is_environment_mesh(source.mesh);
            const bool terrain = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch;
            const bool hero = source.mesh == RenderMesh::Archer || is_monster(source.mesh);
            const auto geometry_ordinal = environment
                ? static_cast<std::uint32_t>(environment_mesh_index(source)) : UINT32_MAX;
            const auto ordinal = environment
                ? render_data::EnvironmentGeometryMaterialOrdinal(geometry_ordinal)
                : terrain ? kTerrainMaterialOrdinal : UINT32_MAX;
            const auto page_slot = environment
                ? geometry_ordinal * render_data::kEnvironmentGeometryLodCount
                : source.mesh == RenderMesh::Archer ? kArcherGeometryPage
                : is_monster(source.mesh)
                    ? (selected_lod == 0 ? kMonsterGeometryPage +
                        static_cast<std::uint32_t>(source.mesh) - static_cast<std::uint32_t>(RenderMesh::MonsterMelee)
                        : kMonsterReducedGeometryPage + 2u *
                            (static_cast<std::uint32_t>(source.mesh) - static_cast<std::uint32_t>(RenderMesh::MonsterMelee)) +
                            selected_lod - 1u)
                    : source.mesh == RenderMesh::EnemyProjectile ? kProjectileGeometryPage
                        : kProceduralGeometryPage;
            if (selected_lod >= 3u) return Result::Failure(ErrorCode::InvalidState,
                "hs_renderer_d3d12", "Software scene monster LOD is invalid.");
            const auto *page = impl_->FindGeometryPage(page_slot);
            if (!page || (is_monster(source.mesh) && selected_lod != 0 && page->page_slot != page_slot))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Software scene candidate has no complete resident geometry page.");
            const bool masked = environment &&
                impl_->environment_material_variants[render_data::EnvironmentGeometryMaterialOrdinal(
                    static_cast<std::uint32_t>(geometry_ordinal))] ==
                    render_data::EnvironmentMaterialVariant::MaskedTwoSided;
            const bool builtin = page->page_slot == kProceduralGeometryPage;
            std::uint32_t flags = hero ? SceneGeometryCachedSkin : 0u;
            if (masked || source.mesh == RenderMesh::TreeCanopy || source.mesh == RenderMesh::Grass)
                flags |= SceneGeometryTwoSided;
            if (builtin) flags |= SceneGeometryBuiltin;
            if (!hero && (builtin || HasEnvironmentWind(source.mesh, source.environment_variant)))
                flags |= SceneGeometryDeformed;
            if (source.mesh == RenderMesh::EnemyProjectile) flags |= SceneGeometryTransmissive;
            const SceneGeometryRecord job{1u + page->page_slot * kGeometryBuffersPerPage,
                2u + page->page_slot * kGeometryBuffersPerPage, 0u, page->index_count,
                0u, sizeof(Vertex), page->vertex_count, 0u, stable_slot, generation,
                skin_slot, static_cast<std::uint32_t>(source.mesh), ordinal, flags};
            const SceneDrawInstance instance{draw.position, draw.scale, draw.color, draw.mesh,
                draw.yaw, draw.padding};
            if (environment && !hero)
            {
                const auto capabilities = ordinal < kAuthoredMaterialNames.size()
                    ? impl_->material_surface_layouts[ordinal].capability_mask : 0u;
                const auto domain = ClassifyBrixelizerDomain(capabilities,
                    (flags & SceneGeometryTwoSided) != 0, (flags & SceneGeometryTransmissive) != 0);
                if (domain != BrixelizerGeometryDomain::Opaque &&
                    domain != BrixelizerGeometryDomain::AuthoredCoverage)
                {
                    if (auto valid = ValidateSceneGeometryRecord(job, instance); !valid) return valid;
                    auto &count = domain == BrixelizerGeometryDomain::UnsupportedMasked
                        ? excluded_masked_count
                        : domain == BrixelizerGeometryDomain::UnsupportedThin
                            ? excluded_thin_count : excluded_translucent_count;
                    return AddBrixelizerExcludedCount(count, page->submeshes.size());
                }
            }
            software_scene_job_inputs.push_back(job);
            software_scene_draw_inputs.push_back(instance);
            software_scene_pages.push_back(page);
            software_scene_sources.push_back({&source, nullptr});
            return Result::Success();
        };
        std::vector<bool> included(persistent_instances.size());
        for (std::size_t index = 0; index < render_instances.size(); ++index)
        {
            const auto &reference = dynamic_draw_refs[index];
            if (included[reference.instance_index]) continue;
            included[reference.instance_index] = true;
            if (auto appended = append_scene(render_instances[index],
                    persistent_instances[reference.instance_index], reference.instance_index,
                    reference.instance_generation, dynamic_skin_slots[index],
                    ordered_monster_lods[index]); !appended) return appended;
        }
        if (static_scene && impl_->static_gpu_scene)
        {
            auto &cache = impl_->static_gpu_scene->software_scene;
            bool page_shape_changed{};
            if(cache.rows)
                for(const auto &shape:cache.rows->page_shapes)
                {
                    const auto *page=impl_->FindGeometryPage(shape.page_slot);
                    if(!page||page->generation!=shape.generation||page->vertex_count!=shape.vertex_count||
                        page->index_count!=shape.index_count||page->submeshes.size()!=shape.submesh_count||
                        page->storage.resource.Get()!=shape.resource||page->storage.allocation!=shape.allocation||
                        page->offsets!=shape.offsets||page->source_digest!=shape.source_digest)
                    {page_shape_changed=true;break;}
                }
            const bool rebuild = !cache.rows || cache.rows->source!=static_scene || !cache.valid || page_shape_changed || cache.foliage_density != foliage_density ||
                cache.material_layouts != impl_->material_surface_layouts ||
                cache.material_variants != impl_->environment_material_variants;
            if (rebuild)
            {
                auto next_rows=std::make_shared<Impl::StaticGpuScene::SoftwareSceneCache::Rows>();
                next_rows->source=static_scene;
                next_rows->bases.reserve(static_scene->instances.size());
                std::array<bool,kGeometryPageCapacity> page_shape_seen{};
                for (std::size_t index = 0; index < static_scene->instances.size(); ++index)
                {
                    const auto &source = static_scene->instances[index];
                    if (!RetainDecorativeFoliage(source, index, foliage_density)) continue;
                    const bool environment = is_environment_mesh(source.mesh);
                    const bool hero = source.mesh == RenderMesh::Archer || is_monster(source.mesh);
                    const auto geometry_ordinal = environment
                        ? static_cast<std::uint32_t>(environment_mesh_index(source)) : UINT32_MAX;
                    const auto ordinal = environment
                        ? render_data::EnvironmentGeometryMaterialOrdinal(geometry_ordinal)
                        : source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch
                            ? kTerrainMaterialOrdinal : UINT32_MAX;
                    const auto page_slot = environment
                        ? geometry_ordinal * render_data::kEnvironmentGeometryLodCount
                        : source.mesh == RenderMesh::Archer ? kArcherGeometryPage
                        : is_monster(source.mesh)
                            ? kMonsterGeometryPage + static_cast<std::uint32_t>(source.mesh) -
                                static_cast<std::uint32_t>(RenderMesh::MonsterMelee)
                        : source.mesh == RenderMesh::EnemyProjectile ? kProjectileGeometryPage
                            : kProceduralGeometryPage;
                    const auto variant_ordinal = environment
                        ? render_data::EnvironmentGeometryMaterialOrdinal(geometry_ordinal) : UINT32_MAX;
                    const bool masked = environment && variant_ordinal <
                            impl_->environment_material_variants.size() &&
                        impl_->environment_material_variants[variant_ordinal] ==
                            render_data::EnvironmentMaterialVariant::MaskedTwoSided;
                    std::uint32_t flags = hero ? SceneGeometryCachedSkin : 0u;
                    if (masked || source.mesh == RenderMesh::TreeCanopy || source.mesh == RenderMesh::Grass)
                        flags |= SceneGeometryTwoSided;
                    if (source.mesh == RenderMesh::EnemyProjectile) flags |= SceneGeometryTransmissive;
                    const auto *page=impl_->FindGeometryPage(page_slot);
                    if(!page)return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",
                        "Software scene candidate has no complete resident geometry page.");
                    if(page_slot>=page_shape_seen.size())return Result::Failure(ErrorCode::InvalidState,
                        "hs_renderer_d3d12","Static software scene page exceeds admitted page capacity.");
                    if(!page_shape_seen[page_slot])
                    {
                        page_shape_seen[page_slot]=true;
                        next_rows->page_shapes.push_back({page_slot,page->generation,page->vertex_count,
                            page->index_count,static_cast<std::uint32_t>(page->submeshes.size()),
                            page->storage.resource.Get(),page->storage.allocation,page->offsets,page->source_digest});
                    }
                    const bool builtin=page->page_slot==kProceduralGeometryPage;
                    auto record_flags=flags;
                    if(builtin)record_flags|=SceneGeometryBuiltin;
                    if(!hero&&(builtin||HasEnvironmentWind(source.mesh,source.environment_variant)))
                        record_flags|=SceneGeometryDeformed;
                    auto domain = BrixelizerGeometryDomain::Opaque;
                    if (environment && !hero)
                    {
                        const auto capabilities = ordinal < kAuthoredMaterialNames.size()
                            ? impl_->material_surface_layouts[ordinal].capability_mask : 0u;
                        domain = ClassifyBrixelizerDomain(capabilities,
                            (flags & SceneGeometryTwoSided) != 0,
                            (flags & SceneGeometryTransmissive) != 0);
                    }
                    const GpuInstance draw{{source.position.x,
                        source.position.y + (environment ? 0.0f : source.scale.y * 0.5f),
                        source.position.z, 0}, {source.scale.x, source.scale.y, source.scale.z, 1},
                        source.color_rgba, static_cast<std::uint32_t>(source.mesh) |
                        (environment ? kEnvironmentMeshFlag : 0u) |
                        (source.mesh == RenderMesh::GroundDetail ? source.environment_variant << 8u : 0u),
                        source.yaw, environment && source.mesh != RenderMesh::MeadowSurface
                            ? 0.0f : std::bit_cast<float>(source.environment_seed)};
                    SceneGeometryRecord geometry{1u+page->page_slot*kGeometryBuffersPerPage,
                        2u+page->page_slot*kGeometryBuffersPerPage,0u,page->index_count,0u,
                        sizeof(Vertex),page->vertex_count,0u,
                        0x80000000u|static_cast<std::uint32_t>(index),
                        impl_->static_gpu_scene->instance_generation,UINT32_MAX,
                        static_cast<std::uint32_t>(source.mesh),ordinal,record_flags};
                    SceneDrawInstance draw_instance{draw.position,draw.scale,draw.color,
                        draw.mesh,draw.yaw,draw.padding};
                    (void)FoldStaticBuiltinTerrainShape(geometry,draw_instance);
                    Impl::StaticGpuScene::SoftwareSceneBase row{
                        index, page_slot, domain};
                    DirectX::XMStoreFloat4x4(&row.world,
                        DirectX::XMMatrixScaling(draw_instance.scale.x, draw_instance.scale.y, draw_instance.scale.z) *
                        DirectX::XMMatrixRotationY(draw_instance.yaw) *
                        DirectX::XMMatrixTranslation(draw_instance.position.x, draw_instance.position.y, draw_instance.position.z));
                    row.yaw_sine = std::sin(draw_instance.yaw);
                    row.yaw_cosine = std::cos(draw_instance.yaw);
                    const bool included_domain=domain==BrixelizerGeometryDomain::Opaque||
                        domain==BrixelizerGeometryDomain::AuthoredCoverage;
                    if(included_domain)
                    {
                        if(auto appended=AppendSceneGeometryStaticRow(next_rows->section,geometry,draw_instance);!appended)
                            return appended;
                    }
                    else
                    {
                        if(auto valid=ValidateSceneGeometryRecord(geometry,draw_instance);!valid)return valid;
                    }
                    next_rows->bases.push_back(std::move(row));
                }
                cache.rows=std::move(next_rows);
                cache.material_layouts = impl_->material_surface_layouts;
                cache.material_variants = impl_->environment_material_variants;
                cache.foliage_density = foliage_density;
                cache.valid = true;
            }
            const auto rows=cache.rows;
            software_scene_static_section=std::shared_ptr<const SceneGeometryStaticSection>(rows,&rows->section);
            software_scene_static_rows=rows;
            for (std::size_t base_index=0;base_index<rows->bases.size();++base_index)
            {
                const auto &base=rows->bases[base_index];
                if (base.source_index >= static_scene->instances.size())
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                        "Cached software scene source index exceeds its admitted static scene.");
                const auto &source = static_scene->instances[base.source_index];
                const auto *page = impl_->FindGeometryPage(base.page_slot);
                if (!page)
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                        "Software scene candidate has no complete resident geometry page.");
                if (base.domain != BrixelizerGeometryDomain::Opaque &&
                    base.domain != BrixelizerGeometryDomain::AuthoredCoverage)
                {
                    auto &count = base.domain == BrixelizerGeometryDomain::UnsupportedMasked
                        ? excluded_masked_count
                        : base.domain == BrixelizerGeometryDomain::UnsupportedThin
                            ? excluded_thin_count : excluded_translucent_count;
                    if (auto counted = AddBrixelizerExcludedCount(count, page->submeshes.size()); !counted)
                        return counted;
                    continue;
                }
                software_scene_pages.push_back(page);
                software_scene_sources.push_back({&source, &base});
            }
        }
    }
    if (!software_scene_job_inputs.empty()||
        (software_scene_static_section&&!software_scene_static_section->geometries.empty()))
    {
        software_scene_current_job_count=software_scene_job_inputs.size();
        if (auto prepared = impl_->scene_geometry.PrepareFrame(
                std::move(software_scene_job_inputs), std::move(software_scene_draw_inputs),
                software_scene_static_section,frame_slot,
                impl_->graph_queue_executor.Completed(), software_scene_prepared);
            !prepared) return prepared;
        scene_shape = *software_scene_prepared.RecordData();
    }
    std::array<bool, kGeometryPageCapacity> software_scene_page_seen{};
    software_scene_page_slots.reserve(software_scene_pages.size());
    for (const auto *page : software_scene_pages)
    {
        if (page->page_slot >= software_scene_page_seen.size())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Software scene page slot exceeds the admitted geometry-page capacity.");
        if (software_scene_page_seen[page->page_slot]) continue;
        software_scene_page_seen[page->page_slot] = true;
        software_scene_page_slots.push_back(page->page_slot);
    }
    std::ranges::sort(software_scene_page_slots);
    return Result::Success();
    };
    Result local_shadow_prepare_result = Result::Success();
    Result local_shadow_submit_result = Result::Success();
    Result local_shadow_join_result = Result::Success();
    std::exception_ptr local_shadow_submit_exception;
    std::exception_ptr local_shadow_join_exception;
    std::exception_ptr software_scene_prepare_exception;
    bool local_shadow_task_attempted{};
    if (local_shadow_planned)
    {
        if (!static_shadow_batch)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Local shadows have no prepared caster batch.");
        local_shadow_input.lights = frame_lights;
        local_shadow_input.camera_position = lod_eye;
        local_shadow_input.rendered_frame_id = owned_view.frame_number;
        local_shadow_input.frame_slot = frame_slot;
        local_shadow_input.completed = impl_->graph_queue_executor.Completed();
        local_shadow_input.reset_cache = new_session;
        local_shadow_input.casters = ShadowCasterRange{static_shadow_batch->records, casters};
        local_shadow_input.draw_groups = groups;
        // Planning uses current lights/casters/groups. Native reads and callbacks
        // are attached to this same object after logical graph slots are known.
        local_shadow_input.draw = [](RenderPassContext &, const LocalShadowOutputs &,
                                     std::span<const VirtualShadowDrawBatch>) { return Result::Success(); };
        local_shadow_input.declare_material_sampling = [](PassBuilder &) {};
        const bool independent_software_scene_work = active_features.software_gi_enabled &&
            ((static_scene && impl_->static_gpu_scene && !static_scene->instances.empty()) ||
                !render_instances.empty());
        const auto &executor = impl_->config.task_executor;
        if (independent_software_scene_work && executor.submit && executor.join)
        {
            local_shadow_task_attempted = true;
            try
            {
                auto submitted = executor.submit([&] {
                    const auto task_start = capture_clock();
                    local_shadow_prepare_result = owned_view.local_shadows.PrepareFrameRecordData(
                        local_shadow_input, local_shadow_plan);
                    cpu_timings.graph_local_shadow_task_us = capture_elapsed(task_start);
                });
                if (!submitted) local_shadow_submit_result = std::move(submitted);
            }
            catch (...)
            {
                // A throwing submit callback may have admitted the task before throwing.
                local_shadow_submit_exception = std::current_exception();
            }
        }
        else
        {
            local_shadow_prepare_result = owned_view.local_shadows.PrepareFrameRecordData(
                local_shadow_input, local_shadow_plan);
        }
    }
    record_graph_resource_detail(cpu_timings.graph_local_shadow_prepare_us);
    if (!local_shadow_task_attempted && !local_shadow_prepare_result)
        return local_shadow_prepare_result;
    Result software_scene_prepare_result = Result::Success();
    try
    {
        software_scene_prepare_result = prepare_software_scene();
    }
    catch (...)
    {
        software_scene_prepare_exception = std::current_exception();
    }
    if (local_shadow_task_attempted)
    {
        try
        {
            local_shadow_join_result = impl_->config.task_executor.join();
        }
        catch (...)
        {
            local_shadow_join_exception = std::current_exception();
        }
    }
    if (local_shadow_join_exception) std::rethrow_exception(local_shadow_join_exception);
    if (local_shadow_submit_exception) std::rethrow_exception(local_shadow_submit_exception);
    if (!local_shadow_prepare_result) return local_shadow_prepare_result;
    if (!local_shadow_submit_result) return local_shadow_submit_result;
    if (!local_shadow_join_result) return local_shadow_join_result;
    if (software_scene_prepare_exception) std::rethrow_exception(software_scene_prepare_exception);
    if (!software_scene_prepare_result) return software_scene_prepare_result;
    const auto software_scene_job_count = software_scene_prepared.GeometryCount();
    record_graph_resource_detail(cpu_timings.graph_software_scene_prepare_us);
    bool direct_contract_failed{};
    const auto shape_of = [&](AllocationResource *resource) {
        FrameGraphLayout::ResourceShape shape;
        if (resource && resource->resource)
        {
            if (resource->allocation)
                if (auto certified = resource->EnsureNativeContract(impl_->device.Get()); !certified)
                    direct_contract_failed = true;
            const auto *contract = resource->NativeContract();
            const auto desc = contract ? contract->description : resource->resource->GetDesc();
            shape.dimension = desc.Dimension; shape.format = desc.Format;
            shape.width = desc.Width; shape.height = desc.Height;
            shape.depth_or_array = desc.DepthOrArraySize; shape.mip_levels = desc.MipLevels;
            shape.sample_count = desc.SampleDesc.Count; shape.sample_quality = desc.SampleDesc.Quality;
            shape.layout = desc.Layout; shape.flags = desc.Flags;
            if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
                shape.buffer_heap = contract ? contract->buffer_heap : D3D12_HEAP_TYPE_DEFAULT;
        }
        return shape;
    };
    const auto visit_direct_imports = [&](const auto &append_shape) {
    for (auto *resource : {&frame.upload, &frame.ui_upload, &frame.geometry_draw_records,
            &frame.skin_cache_vertices, &frame.skin_cache_bounds, &frame.static_visible_refs,
            &frame.static_draw_args, &frame.static_rejected_refs, &frame.static_rejected_args,
            &frame.static_retest_refs, &frame.static_retest_args, &frame.dynamic_visible_refs,
            &frame.dynamic_draw_args, &frame.ribbon_arguments}) append_shape(resource);
    append_shape(previous_skin_frame ? &previous_skin_frame->skin_cache_vertices : nullptr);
    for (auto *resource : {&impl_->environment_brdf_lut, &impl_->material_parameters,
            &impl_->hero_material_bindings_gpu, &owned_view.dynamic_lod_states.states,
            &owned_view.dynamic_lod_states.prefix_source, &impl_->ribbon_history,
            &impl_->ribbon_previous, &impl_->particles, &impl_->particle_dead,
            &impl_->particle_counters, &impl_->indirect_arguments, &dynamic_scene,
            &owned_view.targets.depth, &owned_view.targets.shadow, &owned_view.targets.gbuffer_base, &owned_view.targets.gbuffer_normal,
            &owned_view.targets.gbuffer_metadata, &owned_view.targets.gbuffer_material, &owned_view.targets.gbuffer_motion,
            &owned_view.targets.gbuffer_surface_history, &owned_view.targets.hdr_color, &owned_view.targets.subsurface_diffuse,
            &owned_view.targets.environment_specular, &owned_view.targets.indirect_diffuse,
            &owned_view.targets.oit_accumulation, &owned_view.targets.oit_revealage, &owned_view.targets.post_a, &owned_view.targets.post_b,
            &owned_view.targets.scene_work, &owned_view.targets.display_linear, &owned_view.targets.composite_hdr,
            &owned_view.exposure_histogram, &owned_view.initial_exposure,
            &owned_view.targets.visibility_ids, &owned_view.targets.visibility_front_face, &owned_view.targets.visibility_pixels,
            &owned_view.targets.visibility_counts, &owned_view.targets.visibility_dispatch_args,
            &impl_->vfx_gradient, &impl_->vfx_noise_volume, &impl_->vfx_stbn_scalar,
            &impl_->vfx_fracture_decals, &impl_->vfx_organic_decals,
            &impl_->archer_diffuse, &impl_->archer_normal, &impl_->monster_basecolor,
            &impl_->monster_emissive, &impl_->monster_ram, &impl_->family_diffuse,
            &impl_->family_normal}) append_shape(resource);
    if (separate_previous_instance_scene)
        append_shape(&previous_instance_snapshot->scene);
    for (auto &resource : impl_->particle_alive) append_shape(&resource);
    for (auto &resource : owned_view.targets.material_sidecars) append_shape(&resource);
    for (auto &resource : owned_view.targets.temporal_history) append_shape(&resource);
    for (auto &resource : owned_view.targets.temporal_normal_history) append_shape(&resource);
    for (auto &resource : owned_view.targets.temporal_material_history) append_shape(&resource);
    for (auto &resource : owned_view.exposure_values) append_shape(&resource);
    for (auto &resource : owned_view.targets.hzb_history) append_shape(&resource);
    for (auto &resource : owned_view.ui.textures) append_shape(&resource);
    append_shape(&visibility_frame.raster_records);
    append_shape(static_scene && impl_->static_gpu_scene ? &impl_->static_gpu_scene->instances : nullptr);
    append_shape(static_scene && impl_->static_gpu_scene && impl_->static_gpu_scene->refs
        ? &impl_->static_gpu_scene->refs->storage : nullptr);
    append_shape(impl_->static_gpu_scene ? &impl_->static_gpu_scene->lod_states.states : nullptr);
    append_shape(impl_->static_gpu_scene ? &impl_->static_gpu_scene->lod_states.prefix_source : nullptr);
    append_shape(&impl_->environment_stream.candidate);
    for (auto &stream : impl_->environment_geometry_streams)
        if (stream.upload_pending) append_shape(&stream.candidate.storage);
    for (const auto page_slot : geometry_page_slots)
        if (const auto *page = impl_->FindGeometryPage(page_slot))
            append_shape(&const_cast<GeometryPageGpu *>(page)->storage);
    for (const auto page_slot : software_scene_page_slots)
        if (const auto *page = impl_->FindGeometryPage(page_slot))
            append_shape(&const_cast<GeometryPageGpu *>(page)->storage);
    };
    bool direct_shapes_ready{};
    const auto collect_direct_shapes = [&] {
        auto &shapes = impl_->graph_direct_shapes_scratch;
        auto &aliases = impl_->graph_alias_scratch;
        shapes.clear();
        aliases.clear();
        visit_direct_imports([&](AllocationResource *resource) {
            aliases.push_back({resource ? resource->resource.Get() : nullptr,
                static_cast<std::uint32_t>(shapes.size()), 0u});
            shapes.push_back(shape_of(resource));
        });
        render_graph::CanonicalizeResourceAliases(aliases);
        for (const auto &alias : aliases) shapes[alias.index].alias_group = alias.group;
        aliases.clear();
        direct_shapes_ready = true;
    };
    const auto matches_direct_shapes = [&](const std::vector<FrameGraphLayout::ResourceShape> &saved) {
        if (!direct_shapes_ready) collect_direct_shapes();
        return saved == impl_->graph_direct_shapes_scratch && !direct_contract_failed;
    };
    // A value-only first gate precedes the stateful GI and probe record
    // preparation. Later guide and import checks must also qualify a restore.
    AmbientOcclusionConstants ambient_constants;
    DirectX::XMStoreFloat4x4(&ambient_constants.world_to_view,
        DirectX::XMMatrixTranslationFromVector(eye) * view);
    DirectX::XMStoreFloat4x4(&ambient_constants.projection,
        projection * DirectX::XMMatrixTranslation(jitter_x, jitter_y, 0.0f));
    DirectX::XMStoreFloat4x4(&ambient_constants.inverse_projection,
        DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&ambient_constants.projection)));
    const auto matches_target_shapes = [&](const FrameGraphLayout &saved) {
        if (!owned_view.adaptive_shading.MatchesShape(saved.adaptive)) return false;
        if (!owned_view.ambient_occlusion.MatchesShape(saved.ambient,
                !(owned_view.temporal_history_valid || owned_view.hzb_history_valid) || new_session,
                overlap_ao_csm ? QueueHint::Compute : QueueHint::Direct,
                owned_view.state.render_active, ambient_constants.inverse_projection)) return false;
        if (!owned_view.subsurface.MatchesShape(saved.subsurface)) return false;

        if (!owned_view.screen_reflections.MatchesShape(saved.reflections,
                !(owned_view.temporal_history_valid || owned_view.hzb_history_valid) || new_session,
                owned_view.frame_number, saved.ambient.ao, impl_->material_sidecar_layout.plane_count)) return false;

        if (constants->vfx_decal_count.x)
        {
            if (!owned_view.decal_surface.MatchesShape(saved.decal_surface, true)) return false;
        }
        if (!typed_screen.empty() && screen_intensity > 0)
        {
            const bool active = std::ranges::any_of(typed_screen,
                [](const auto &command) { return command.alpha != 0; });
            if (!owned_view.screen_overlay.MatchesShape(saved.screen_overlay, active)) return false;
        }
        const bool pre_bloom = impl_->config.motion_blur != GraphicsMotionBlur::Off ||
            impl_->config.depth_of_field;
        const bool pre_tone_map = impl_->config.color_grading;
        if (!MatchesPostSubgraphShape(impl_->config.bloom, impl_->config.outline, fxaa_identity,
                !owned_view.view_targets_initialized, pre_bloom, pre_tone_map,
                devtools_ui_hook, saved.post)) return false;
        const bool transparent_guide_state = have_hero_contributors &&
            owned_view.hero_transparency.AllocatedBytes() != 0;
        if (pre_bloom)
        {
            if (!owned_view.hdr_post.MatchesMotionShape(saved.hdr_motion, transparent_guide_state)) return false;
        }
        if (pre_tone_map)
        {
            if (!owned_view.hdr_post.MatchesGradeShape(saved.hdr_grade, pre_bloom)) return false;
        }
        if (use_native_reconstruction)
        {
            const bool screen_overlay_active = screen_intensity != 0 && std::ranges::any_of(typed_screen,
                [](const auto &command) { return command.alpha != 0; });
            if (!owned_view.reconstruction_guides.MatchesShape(saved.reconstruction_guides,
                    owned_view.reconstruction_provider->UsesExternalQueue(),
                    owned_view.reconstruction_plan.effective_provider.value,
                    transparent_guide_state, screen_overlay_active)) return false;
        }
        if (presented_view && impl_->sdk_presentation.IsEnabled())
        {
            const bool presentation_transparent = !use_native_reconstruction && transparent_guide_state;
            if (!impl_->presentation_guides.MatchesShape(saved.presentation_guides, 900, 901, 902,
                    presentation_transparent) ||
                !impl_->presentation_guides.MatchesTagShape(saved.presentation_tag, 904)) return false;
        }
        return true;
    };
    LocalProbeRuntimeFrameInputs local_probe_inputs;
    LocalProbeRuntimeFramePlan local_probe_plan;
    bool local_probe_revision_preflight{};
    const bool capture_source = impl_->local_probes.AllocatedBytes() != 0 &&
        static_scene && impl_->static_gpu_scene;
    local_probe_inputs.config = &impl_->config;
    local_probe_inputs.pipeline_cache = &impl_->pipeline_cache;
    local_probe_inputs.scene = static_scene;
    local_probe_inputs.lights = std::span<const LightView>(frame_lights.data(), authoritative_light_count);
    local_probe_inputs.static_instance_generation = impl_->static_gpu_scene
        ? impl_->static_gpu_scene->instance_generation : 0;
    local_probe_inputs.material_parameter_revision = impl_->material_parameter_revision;
    local_probe_inputs.pipeline_revision = impl_->pipeline_revision;
    local_probe_inputs.foliage_density = foliage_density;
    local_probe_inputs.frame_constants = constants;
    local_probe_inputs.completed = impl_->graph_queue_executor.Completed();
    if (has_static_authored_material_draw)
        local_probe_inputs.declare_material_sampling = [](PassBuilder &) {};
    if (has_static_masked_material_draw)
        local_probe_inputs.declare_shadow_sampling = [](PassBuilder &) {};
    std::array<LocalProbePageRevision, kEnvironmentMaterialNames.size() + 1> page_revisions;
    if (capture_source)
    {
        for (std::size_t material = 0; material < page_revisions.size(); ++material)
        {
            const auto &page = material == kEnvironmentMaterialNames.size()
                ? impl_->procedural_geometry : *bucket_pages[material * 3];
            page_revisions[material] = {page.page_slot,page.generation,page.index_count,
                page.source_digest,page.metadata.bounds};
        }
        local_probe_inputs.page_revisions = page_revisions;
        if (auto prepared = impl_->local_probes.PrepareFrameDecisionData(
                local_probe_inputs, local_probe_plan); !prepared) return prepared;
        local_probe_revision_preflight = true;
    }
    const bool previous_view_valid = owned_view.temporal_history_valid || owned_view.hzb_history_valid;
    const bool volumetric_reset_history = !previous_view_valid || new_session;
    VolumetricCamera retained_volumetric_camera;
    retained_volumetric_camera.inverse_view_projection = camera_relative_inverse_view_projection;
    DirectX::XMStoreFloat4x4(&retained_volumetric_camera.view_projection,
        DirectX::XMMatrixInverse(nullptr,
            DirectX::XMLoadFloat4x4(&retained_volumetric_camera.inverse_view_projection)));
    DirectX::XMStoreFloat4x4(&retained_volumetric_camera.world_to_view,
        DirectX::XMMatrixTranslationFromVector(eye) * view);
    retained_volumetric_camera.origin = lod_eye;
    retained_volumetric_camera.near_plane = near_plane;
    retained_volumetric_camera.far_plane = far_plane;
    const auto retained_volumetric_previous_camera = owned_view.previous_volumetric_camera.near_plane > 0
        ? owned_view.previous_volumetric_camera : retained_volumetric_camera;
    VolumetricRevisions retained_volumetric_revisions{};
    auto projected_volumetric_inputs = VolumetricInputs{};
    const bool projected_volumetric_shadows = primary_sun &&
        owned_view.virtual_shadows.AllocatedBytes() != 0;
    if (impl_->config.volumetric_quality != GraphicsOptionalQuality::Off)
    {
        retained_volumetric_revisions.scene = local_probe_plan.revisions.static_layout;
        retained_volumetric_revisions.dynamic_scene = impl_->dynamic_scene_slots.ContentRevision();
        retained_volumetric_revisions.light = local_probe_plan.revisions.light;
        const auto mix_volumetric_revision = [](std::uint64_t &hash, std::uint64_t value) {
            hash = (hash ^ value) * 1099511628211ull;
        };
        for (const auto &source : frame_lights)
        {
            mix_volumetric_revision(retained_volumetric_revisions.light, source.stable_id);
            mix_volumetric_revision(retained_volumetric_revisions.light, source.generation);
            mix_volumetric_revision(retained_volumetric_revisions.light, source.revision);
            for (const auto value : {source.direction.x, source.direction.y, source.direction.z,
                    source.color.x, source.color.y, source.color.z, source.position.x, source.position.y,
                    source.position.z, source.quantity, source.range, source.shape.x, source.shape.y,
                    source.emitter_radius, source.inner_cone_radians, source.outer_cone_radians,
                    source.area_tangent.x, source.area_tangent.y, source.area_tangent.z})
                mix_volumetric_revision(retained_volumetric_revisions.light, std::bit_cast<UINT>(value));
            mix_volumetric_revision(retained_volumetric_revisions.light, static_cast<UINT>(source.type));
            mix_volumetric_revision(retained_volumetric_revisions.light, static_cast<UINT>(source.unit));
            mix_volumetric_revision(retained_volumetric_revisions.light, source.shadow_flags);
        }
        retained_volumetric_revisions.volume = impl_->volumetric_revision;
        projected_volumetric_inputs.hero_blue_noise.resource = impl_->vfx_stbn_scalar.resource.Get();
        projected_volumetric_inputs.hero_gradient.resource = impl_->vfx_gradient.resource.Get();
        projected_volumetric_inputs.hero_noise_volume.resource = impl_->vfx_noise_volume.resource.Get();
        projected_volumetric_inputs.volumes = impl_->volumetric_media;
        if (impl_->config.vfx_quality == VfxQuality::High)
            projected_volumetric_inputs.hero_volumes = typed_volumes;
        projected_volumetric_inputs.frame_slot = frame_slot;
        projected_volumetric_inputs.frame_index = owned_view.frame_number;
        projected_volumetric_inputs.reset_history = volumetric_reset_history;
        projected_volumetric_inputs.revisions = retained_volumetric_revisions;
        projected_volumetric_inputs.camera = retained_volumetric_camera;
        projected_volumetric_inputs.previous_camera = retained_volumetric_previous_camera;
        if (primary_sun && csm_required) projected_volumetric_inputs.compact_shadow.identity.w = 1;
    }
    const auto matches_volumetric_layout = [&](const FrameGraphLayout &saved) {
        const auto &v = saved.volumetrics.input_versions;
        auto projected = projected_volumetric_inputs;
        projected.depth.version = v[0]; projected.hdr.version = v[1];
        projected.lights.lights = v[2]; projected.lights.ranges = v[3];
        projected.lights.inline_indices = v[4]; projected.lights.spill_indices = v[5];
        projected.shadows.metadata = v[6]; projected.shadows.page_table = v[7];
        projected.shadows.static_bank = v[8]; projected.shadows.dynamic_bank = v[9];
        projected.shadows.draw_records = v[10]; projected.shadows.root_static = v[11];
        projected.shadows.root_dynamic = v[12];
        projected.local_shadows.metadata = v[13]; projected.local_shadows.records = v[14];
        projected.local_shadows.static_bank = v[15]; projected.local_shadows.dynamic_bank = v[16];
        projected.local_shadows.draw_records = v[17];
        projected.compact_shadow.atlas.version = v[18];
        projected.hero_blue_noise.version = v[19];
        projected.hero_gradient.version = v[20];
        projected.hero_noise_volume.version = v[21];
        return owned_view.volumetrics.MatchesProjectedLayout(projected, saved.volumetrics,
            projected_volumetric_shadows, local_shadow_planned,
            projected_volumetric_shadows && shadow_plan.constants.root_options.x != 0u,
            projected_volumetric_shadows && shadow_plan.constants.root_options.w != 0u &&
                shadow_plan.constants.root_options.x != 0u);
    };
    FrameGraphLayout *source_candidate{};
    for (auto &saved : impl_->graph_layouts)
    {
        if (!saved.handle || !saved.certified_imports || saved.early != early_key ||
            saved.geometry_page_slots != geometry_page_slots ||
            saved.software_scene_page_slots != software_scene_page_slots)
            continue;
        if (!matches_direct_shapes(saved.direct_import_shapes) ||
            (software_scene_job_count==0
                ? saved.scene_geometry.valid && saved.scene_geometry.record.record_count != 0
                : !saved.scene_geometry.valid ||
                    saved.scene_geometry.record.frame_slot != scene_shape.frame_slot ||
                    (saved.scene_geometry.record.deformed_position_bytes != 0) !=
                        (scene_shape.deformed_position_bytes != 0) ||
                    saved.scene_geometry.record.import_widths != scene_shape.import_widths) ||
            !impl_->texture_streaming.MatchesLayout(1000, saved.texture_streaming) ||
            !impl_->texture_feedback.MatchesLayout(frame_slot, 1200, 1201, saved.texture_feedback) ||
            !owned_view.light_list.MatchesLayout(saved.light_list) ||
            !impl_->ltc_lut.MatchesLayout(saved.ltc) ||
            !impl_->lighting_probe.MatchesLayout(saved.probe) ||
            !impl_->MatchesClusterGraphLayout(cluster_record, saved.cluster) ||
            !matches_target_shapes(saved))
            continue;
        if (primary_sun && owned_view.virtual_shadows.AllocatedBytes() != 0)
        {
            inputs.depth = saved.virtual_shadows.depth;
            inputs.normal = saved.virtual_shadows.normal;
            inputs.native_reads = saved.virtual_shadows.native_reads;
            inputs.draw = [](RenderPassContext &, const VirtualShadowOutputs &,
                             std::span<const VirtualShadowDrawBatch>) { return Result::Success(); };
            inputs.declare_material_sampling = [](PassBuilder &) {};
            if (!owned_view.virtual_shadows.MatchesLayout(inputs, shadow_plan,
                    saved.virtual_shadows))
            {
                continue;
            }
        }
        else if (saved.virtual_shadows.target_generation)
        {
            continue;
        }
        if (local_shadow_planned)
        {
            local_shadow_input.native_reads = saved.local_shadows.native_reads;
            if (!owned_view.local_shadows.MatchesLayout(local_shadow_input, local_shadow_plan,
                    saved.local_shadows))
            {
                continue;
            }
        }
        else if (saved.local_shadows.storage_generation)
        {
            continue;
        }
        if (have_hero_contributors)
        {
            if (!saved.hero.target_generation)
            {
                continue;
            }
            HeroTransparencyInputs hero_match;
            hero_match.opaque_depth.version = saved.hero.inputs[0];
            hero_match.hdr.version = saved.hero.inputs[1];
            hero_match.accumulation.version = saved.hero.inputs[2];
            hero_match.revealage.version = saved.hero.inputs[3];
            hero_match.use_occupancy_queries = hero_occupancy_topology;
            hero_match.peel_reads = std::span<const HeroTransparencySceneRead>(saved.hero.peel_reads);
            hero_match.scene_reads = std::span<const HeroTransparencySceneRead>(saved.hero.scene_reads);
#if defined(HS_DEVELOPMENT_TOOLS)
            std::array<HeroWorkContributorInput, HeroWorkRetainedLayout::kMaxContributors> work{};
            std::size_t work_count{};
            for (std::uint32_t bit = 0; bit < HeroWorkRetainedLayout::kMaxContributors; ++bit)
                if (hero_work_source_mask & (1u << bit))
                {
                    work[work_count].arguments = saved.hero.work.argument_versions[work_count];
                    ++work_count;
                }
            hero_match.work_contributors = std::span<const HeroWorkContributorInput>(work.data(), work_count);
#endif
            if (!owned_view.hero_transparency.MatchesLayout(hero_match,
                    HeroTransparencyGuide{saved.hero.background, nullptr}, saved.hero))
            {
                continue;
            }
        }
        else if (saved.hero.target_generation)
        {
            continue;
        }
        if (impl_->config.volumetric_quality != GraphicsOptionalQuality::Off)
        {
            if (!matches_volumetric_layout(saved))
            {
                continue;
            }
        }
        else if (saved.volumetrics.storage_generation)
        {
            continue;
        }
        if (!source_candidate || saved.last_used > source_candidate->last_used)
            source_candidate = &saved;
    }
    HlodRuntimeFrameRecordData hlod_record;
    if (auto prepared = impl_->PrepareHlodFrameRecordData(frame_slot,
            active_view_mask,
            source_candidate ? source_candidate->hlod_input_geometry_records : ResourceVersion{},
            source_candidate ? source_candidate->hlod_input_static_instances : ResourceVersion{},
            source_candidate ? source_candidate->hlod.selection.guides[2] : ResourceVersion{},
            hlod_record); !prepared) return prepared;
    if (source_candidate && !impl_->MatchesHlodFrameLayout(hlod_record, source_candidate->hlod))
    {
        source_candidate = nullptr;
    }
    record_graph_resource_detail(cpu_timings.graph_resource_scene_hlod_us);
    std::vector<SoftwareSceneGeometry> brixelizer_geometries;
    std::vector<BrixelizerSceneInstance> brixelizer_instances;
    std::shared_ptr<const void> brixelizer_immutable_owner;
    BrixelizerGeometryLookup brixelizer_geometry_lookup;
    std::vector<std::shared_ptr<const void>> brixelizer_owners;
    std::unordered_set<D3D12MA::Allocation *> brixelizer_backing;
    std::vector<std::size_t> brixelizer_leaf_jobs;
    if (active_features.software_gi_enabled)
    {
        brixelizer_owners.push_back(impl_->resident_generation_lease);
        if (software_scene_prepared.Owner())
            brixelizer_owners.push_back(software_scene_prepared.Owner());
    }
    const auto retain_brixelizer_allocation = [&](const AllocationResource &backing) {
        if (!backing.allocation || !brixelizer_backing.insert(backing.allocation).second) return;
        backing.allocation->AddRef();
        std::shared_ptr<const void> owner(backing.allocation, [](const void *value) {
            const_cast<D3D12MA::Allocation *>(static_cast<const D3D12MA::Allocation *>(value))->Release();
        });
        brixelizer_owners.push_back(std::move(owner));
    };
    auto *const prepared_positions = software_scene_prepared.PositionResource();
    const auto append_brixelizer_job = [&](std::size_t index, BrixelizerGeometryLookup &lookup,
        std::vector<SoftwareSceneGeometry> &geometries, std::vector<BrixelizerSceneInstance> &instances,
        std::vector<std::size_t> &leaf_jobs) -> Result {
        SceneGeometryRecord job_scratch{};
        const auto &job = software_scene_prepared.GeometryAt(index,job_scratch);
        const auto &draw = software_scene_prepared.InstanceAt(index);
        const auto &page = *software_scene_pages[index];
        const bool deformed = (job.flags & SceneGeometryDeformed) != 0;
        const bool skin = (job.flags & SceneGeometryCachedSkin) != 0;
        const std::uint64_t vertex_offset = deformed ? job.position_byte_offset : skin ?
            std::uint64_t(persistent_metadata[job.skin_slot].skin_vertex_offset) *
                persistent_metadata[job.skin_slot].skin_vertex_stride : 0;
        if (skin && !SkinVertexRangeFits(persistent_metadata[job.skin_slot].skin_vertex_offset,
            page.vertex_count, persistent_metadata[job.skin_slot].skin_vertex_stride,
            frame.skin_cache_vertices.resource->GetDesc().Width))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Software scene SkinCache range exceeds shader addressing.");
        SoftwareSceneGeometry geometry{std::uint64_t(page.page_slot) + 1, page.generation, owned_view.frame_number + 1,
            deformed ? prepared_positions : skin ? frame.skin_cache_vertices.resource.Get() : page.storage.resource.Get(),
            page.storage.resource.Get(), {},
            ResourceVersion{}, vertex_offset, page.offsets[1], deformed ? 12u :
                skin ? persistent_metadata[job.skin_slot].skin_vertex_stride : static_cast<std::uint32_t>(sizeof(Vertex)),
            page.vertex_count, page.index_count, DXGI_FORMAT_R32_UINT, false, deformed || skin, new_session};
        // Immutable shared pages retain their revision through frames.
        if (!deformed && !skin) geometry.current_revision = page.generation;
        const auto &scene_source = software_scene_sources[index];
        DirectX::XMFLOAT4X4 world;
        if (scene_source.static_base)
            world = scene_source.static_base->world;
        else
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixScaling(draw.scale.x, draw.scale.y, draw.scale.z) *
                DirectX::XMMatrixRotationY(draw.yaw) * DirectX::XMMatrixTranslation(draw.position.x, draw.position.y, draw.position.z));
        const auto material_id = job.environment_ordinal < kAuthoredMaterialNames.size() ? 0x10000000u + job.environment_ordinal : 0x30000000u + job.mesh_id;
        const SoftwareSceneInstance instance{job.stable_gpu_scene_slot, job.instance_generation,
            static_cast<std::uint32_t>(index), material_id, 0, page.generation, world};
        retain_brixelizer_allocation(page.storage);
        if (skin) retain_brixelizer_allocation(frame.skin_cache_vertices);
        const auto &source = *scene_source.source;
        const auto &sphere = page.metadata.bounds.sphere;
        const float sx = sphere[0] * draw.scale.x, sy = sphere[1] * draw.scale.y, sz = sphere[2] * draw.scale.z;
        const float sine = scene_source.static_base ? scene_source.static_base->yaw_sine : std::sin(draw.yaw);
        const float cosine = scene_source.static_base ? scene_source.static_base->yaw_cosine : std::cos(draw.yaw);
        const float maximum_scale = std::max({std::abs(draw.scale.x), std::abs(draw.scale.y), std::abs(draw.scale.z)});
        DirectX::XMFLOAT3 center{draw.position.x + cosine * sx + sine * sz, draw.position.y + sy,
            draw.position.z - sine * sx + cosine * sz};
        float radius = sphere[3] * maximum_scale;
        if (skin)
        {
            center = {draw.position.x, draw.position.y, draw.position.z};
            radius = std::max(draw.position.w, std::sqrt(sx*sx + sy*sy + sz*sz) + radius);
        }
        if (HasEnvironmentWind(source.mesh, source.environment_variant)) radius += grass_visibility_radius;
        std::span<const render_data::CharacterMaterialBinding> bindings;
        if (source.mesh == RenderMesh::Archer) bindings = impl_->archer_material_bindings;
        else if (is_monster(source.mesh)) bindings = impl_->monster_assets[
            static_cast<std::size_t>(source.mesh) - static_cast<std::size_t>(RenderMesh::MonsterMelee)].material_bindings;
        const auto ordinal = job.environment_ordinal;
        const auto capabilities = ordinal < kAuthoredMaterialNames.size()
            ? impl_->material_surface_layouts[ordinal].capability_mask : 0u;
        const auto previous_leaf_count = geometries.size();
        if (auto appended = AppendBrixelizerLeaf(geometry, instance, page.submeshes,
                page.brixelizer_submesh_keys, bindings,
                std::span(impl_->material_surface_layouts).subspan(kAuthoredMaterialNames.size()), capabilities,
                (job.flags & SceneGeometryTwoSided) != 0,
                (job.flags & SceneGeometryTransmissive) != 0,
                {center.x-radius,center.y-radius,center.z-radius},
                {center.x+radius,center.y+radius,center.z+radius},
                ((job.stable_gpu_scene_slot & 0x80000000u) ? source.revisions.material
                    : impl_->dynamic_scene_slots.Revisions(job.stable_gpu_scene_slot).material) ^
                    impl_->material_parameter_revision,
                lookup,geometries,instances); !appended) return appended;
        for (auto leaf = previous_leaf_count; leaf < geometries.size(); ++leaf)
            leaf_jobs.push_back(index);
        return Result::Success();
    };
    std::vector<BrixelizerSceneInstanceRange> brixelizer_instance_ranges;
    if (active_features.software_gi_enabled && software_scene_job_count!=0)
    {
        using NativeRows = Impl::StaticGpuScene::SoftwareSceneCache::NativeRows;
        std::shared_ptr<const NativeRows> cached;
        if (software_scene_static_rows)
        {
            auto &cache = impl_->static_gpu_scene->software_scene;
            if (!cache.native_rows || cache.native_rows->source != software_scene_static_rows ||
                cache.native_rows->generation_owner != impl_->resident_generation_lease ||
                cache.native_rows->material_revision != impl_->material_parameter_revision)
            {
                auto next = std::make_shared<NativeRows>();
                next->source = software_scene_static_rows;
                next->generation_owner = impl_->resident_generation_lease;
                next->material_revision = impl_->material_parameter_revision;
                BrixelizerGeometryLookup lookup;
                for (std::size_t local = 0; local < software_scene_static_rows->section.geometries.size(); ++local)
                {
                    const auto &row = software_scene_static_rows->section.geometries[local];
                    if (row.flags & (SceneGeometryDeformed | SceneGeometryCachedSkin)) continue;
                    if (next->runs.empty() || next->runs.back().job_begin + next->runs.back().job_count != local)
                        next->runs.push_back({local, 0, next->instances.size(), 0});
                    if (auto appended = append_brixelizer_job(software_scene_current_job_count + local,
                        lookup, next->geometries, next->instances, next->leaf_jobs); !appended) return appended;
                    auto &run = next->runs.back();
                    ++run.job_count;
                    run.instance_count = next->instances.size() - run.instance_begin;
                }
                for (auto &geometry : next->geometries) geometry.force_rebuild = false;
                for (auto &job : next->leaf_jobs) job -= software_scene_current_job_count;
                cache.native_rows = std::move(next);
            }
            cached = cache.native_rows;
            brixelizer_immutable_owner = cached;
            brixelizer_owners.push_back(cached);
            brixelizer_geometries.reserve(cached->geometries.size());
            brixelizer_leaf_jobs.reserve(cached->leaf_jobs.size());
            for (std::size_t leaf = 0; leaf < cached->geometries.size(); ++leaf)
            {
                auto geometry = cached->geometries[leaf];
                geometry.force_rebuild = new_session;
                const auto added = brixelizer_geometry_lookup.FindOrAdd(geometry, brixelizer_geometries);
                assert(added == leaf);
                brixelizer_leaf_jobs.push_back(software_scene_current_job_count + cached->leaf_jobs[leaf]);
                const auto &page = *software_scene_pages[brixelizer_leaf_jobs.back()];
                retain_brixelizer_allocation(page.storage);
            }
        }
        struct InstanceRange { bool cached; std::size_t begin, count; };
        std::vector<InstanceRange> ranges;
        std::size_t run_index{}, current_begin{};
        const auto append_current_range = [&]() {
            if (brixelizer_instances.size() != current_begin)
                ranges.push_back({false, current_begin, brixelizer_instances.size() - current_begin});
            current_begin = brixelizer_instances.size();
        };
        for (std::size_t index = 0; index < software_scene_job_count;)
        {
            if (cached && run_index < cached->runs.size() &&
                index == software_scene_current_job_count + cached->runs[run_index].job_begin)
            {
                append_current_range();
                const auto &run = cached->runs[run_index++];
                if (run.instance_count) ranges.push_back({true, run.instance_begin, run.instance_count});
                index += run.job_count;
            }
            else
            {
                if (auto appended = append_brixelizer_job(index, brixelizer_geometry_lookup,
                    brixelizer_geometries, brixelizer_instances, brixelizer_leaf_jobs); !appended) return appended;
                ++index;
            }
        }
        append_current_range();
        brixelizer_instance_ranges.reserve(ranges.size());
        for (const auto &range : ranges)
            brixelizer_instance_ranges.push_back({range.cached
                ? std::span(cached->instances).subspan(range.begin, range.count)
                : std::span(brixelizer_instances).subspan(range.begin, range.count), range.cached});
    }
    const auto brixelizer_leaf_key = [&](std::size_t leaf) {
        const auto job_index = brixelizer_leaf_jobs[leaf];
        SceneGeometryRecord job_scratch{};
        const auto &job=software_scene_prepared.GeometryAt(job_index,job_scratch);
        return std::pair{software_scene_pages[job_index]->page_slot,
            job.flags &
                (SceneGeometryDeformed | SceneGeometryCachedSkin)};
    };
    if (source_candidate && active_features.software_gi_enabled)
    {
        const auto &reads = source_candidate->brixelizer.record.geometry_reads;
        const auto &saved_keys = source_candidate->brixelizer_leaf_reads;
        if (reads.size() != saved_keys.size() * 2u)
        {
            source_candidate = nullptr;
        }
        else
            for (std::size_t leaf = 0; leaf < brixelizer_geometries.size(); ++leaf)
            {
                const auto key = brixelizer_leaf_key(leaf);
                const auto found = std::lower_bound(saved_keys.begin(), saved_keys.end(), key,
                    [](const FrameGraphLayout::BrixelizerLeafRead &entry, const auto &value) {
                        return entry.key < value;
                    });
                if (found == saved_keys.end() || found->key != key)
                {
                    source_candidate = nullptr;
                    break;
                }
                const auto old_leaf = found->read_pair;
                brixelizer_geometries[leaf].vertex_version = reads[old_leaf * 2u].version;
                brixelizer_geometries[leaf].index_version = reads[old_leaf * 2u + 1u].version;
            }
    }
    BrixelizerSceneInputs native_scene;
    BrixelizerFrameRecordData brixelizer_record;
    if (active_features.software_gi_enabled)
    {
        native_scene.geometries = brixelizer_geometries; native_scene.instance_ranges = brixelizer_instance_ranges;
        native_scene.immutable_owner = brixelizer_immutable_owner;
        native_scene.excluded_masked_count = excluded_masked_count;
        native_scene.excluded_thin_count = excluded_thin_count;
        native_scene.excluded_translucent_count = excluded_translucent_count;
        native_scene.sdf_center = lod_eye; native_scene.frame_slot = frame_slot;
        native_scene.frame_index = owned_view.frame_number; native_scene.scene_revision = impl_->pipeline_revision + 1;
        native_scene.completed = impl_->graph_queue_executor.Completed(); native_scene.owners = brixelizer_owners;
    }
    record_graph_resource_detail(cpu_timings.graph_resource_brixelizer_input_us);
    if (active_features.software_gi_enabled)
    {
        if (auto prepared = owned_view.brixelizer_gi.PrepareFrameRecordData(native_scene, brixelizer_record); !prepared)
            return prepared;
        if (source_candidate && !MatchesBrixelizerGraphLayout(
                brixelizer_record, source_candidate->brixelizer))
        {
            source_candidate = nullptr;
        }
    }
    record_graph_resource_detail(cpu_timings.graph_resource_brixelizer_prepare_us);
    struct PreparedGiGuard
    {
        BrixelizerGiSystem *service{};
        ~PreparedGiGuard() { if (service) service->AbandonPreparedFrame(); }
    } prepared_gi_guard{active_features.software_gi_enabled ? &owned_view.brixelizer_gi : nullptr};
    std::optional<std::array<LocalProbeCapturePage, kEnvironmentMaterialNames.size() + 1>> capture_pages;
    std::vector<LocalProbeSceneRead> capture_reads;
    bool local_probe_prepared{};
    // Capture texture residency is not final until streaming bindings are assembled;
    // avoid restoring a capture layout whose face count was planned from no mip facts.
    if(source_candidate&&(local_probe_plan.capture||
        impl_->local_probes.CaptureIdentityPending(local_probe_inputs.completed)))
    {
        source_candidate=nullptr;
    }
    if (source_candidate)
    {
        // The freshly computed revision decision is authoritative. A layout can
        // only be reused when its capture topology matches today's decision.
        if (source_candidate->local_probe.capture != local_probe_plan.capture)
        {
            source_candidate = nullptr;
        }
    }
    if (source_candidate)
    {
        const auto count_present = [](const AllocationResource &resource) {
            return resource.resource ? std::size_t{1} : std::size_t{0};
        };
        std::size_t expected_reads{};
        if (local_probe_plan.capture)
        {
            expected_reads = count_present(frame.upload) + count_present(impl_->material_parameters) +
                count_present(impl_->hero_material_bindings_gpu) + count_present(impl_->environment_brdf_lut) + 9u;
            expected_reads += count_present(impl_->static_gpu_scene->refs->storage);
            if (source_candidate->virtual_shadows.target_generation)
            {
                expected_reads += 5u;
                expected_reads += source_candidate->virtual_shadows.root_static_present ? 1u : 0u;
                expected_reads += source_candidate->virtual_shadows.root_dynamic_present ? 1u : 0u;
            }
            if (source_candidate->local_shadows.storage_generation) expected_reads += 5u;
        }
        if (source_candidate->local_probe.scene_reads.size() != expected_reads)
        {
            source_candidate = nullptr;
        }
        else
        {
            local_probe_inputs.completed = impl_->graph_queue_executor.Completed();
            capture_reads = source_candidate->local_probe.scene_reads;
            local_probe_inputs.scene_reads = capture_reads;
            if (local_probe_plan.capture)
            {
                capture_pages.emplace();
                const auto &saved_reads = source_candidate->local_probe.shadow_reads;
                if (saved_reads.size() < expected_reads + 2u + capture_pages->size())
                {
                    source_candidate = nullptr;
                }
                else
                {
                    local_probe_inputs.static_version = saved_reads[expected_reads + 1u].version;
                    for (std::size_t material = 0; material < capture_pages->size(); ++material)
                    {
                        const auto &page = material == kEnvironmentMaterialNames.size()
                            ? impl_->procedural_geometry : *bucket_pages[material * 3];
                        (*capture_pages)[material].vertex_version = saved_reads[expected_reads + 2u + material].version;
                        (*capture_pages)[material].index_version = (*capture_pages)[material].vertex_version;
                        (*capture_pages)[material].vertices = page.storage.resource;
                        (*capture_pages)[material].indices = page.storage.resource;
                        (*capture_pages)[material].vertex_allocation = page.storage.allocation;
                        (*capture_pages)[material].index_allocation = page.storage.allocation;
                        (*capture_pages)[material].vertex_view = page.vertex_view;
                        (*capture_pages)[material].index_view = page.index_view;
                        (*capture_pages)[material].index_count = page.index_count;
                        (*capture_pages)[material].generation = page.generation;
                        (*capture_pages)[material].page_slot = page.page_slot;
                        (*capture_pages)[material].source_digest = page.source_digest;
                        (*capture_pages)[material].bounds = page.metadata.bounds;
                        (*capture_pages)[material].complete = page.storage.resource && page.index_count != 0;
                    }
                    if (std::ranges::any_of(*capture_pages, [](const auto &page) {
                            return !page.complete || !page.generation || !page.index_count ||
                                std::ranges::none_of(page.source_digest, [](std::byte value) {
                                    return value != std::byte{};
                                });
                        }))
                    {
                        source_candidate = nullptr;
                    }
                }
                local_probe_inputs.pages = *capture_pages;
            }
            if (source_candidate)
            {
                if (auto prepared = impl_->local_probes.PrepareFrameDecisionData(
                        local_probe_inputs, local_probe_plan); !prepared) return prepared;
                local_probe_prepared = true;
                if (!impl_->local_probes.MatchesLayout(local_probe_inputs,
                        local_probe_plan, source_candidate->local_probe))
                {
                    source_candidate = nullptr;
                }
            }
        }
    }
    // GetBuffer publishes a DXGI-owned swapchain surface. Its owner guarantees
    // that it is independent of application placed heaps; retain only value
    // metadata while back_buffers owns the COM lifetime.
    auto *native_back_buffer = frame_output.Get();
    if (!native_back_buffer)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Current swapchain buffer is missing.");
    if (back_buffer_contract.native_resource != native_back_buffer ||
        back_buffer_contract_device != impl_->device.Get() ||
        back_buffer_contract.description.Width != owned_view.width ||
        back_buffer_contract.description.Height != owned_view.height ||
        back_buffer_contract.description.Format != owned_view.display_state.format)
    {
        ComPtr<IUnknown> resource_identity, owner_identity;
        if (FAILED(native_back_buffer->QueryInterface(IID_PPV_ARGS(&resource_identity))))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Cannot certify the current swapchain buffer owner.");
        ComPtr<IUnknown> renderer_device_identity;
        if (FAILED(impl_->device->QueryInterface(IID_PPV_ARGS(&renderer_device_identity))))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Cannot identify the renderer device for the output contract.");
        if (presented_view && impl_->sdk_presentation.PresentationOwner() == SdkPresentationOwner::None)
            // Native DXGI buffers come from the swapchain created with our owned presentation queue.
            owner_identity = renderer_device_identity;
        else if (FAILED(native_back_buffer->GetDevice(IID_PPV_ARGS(&owner_identity))) ||
            renderer_device_identity.Get() != owner_identity.Get())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Output buffer belongs to a different device.");
        auto &generation = back_buffer_contract_generation;
        if (++generation == 0) ++generation;
        back_buffer_contract = {};
        back_buffer_contract.native_resource = native_back_buffer;
        back_buffer_contract.resource_identity = resource_identity.Get();
        back_buffer_contract.device_identity = owner_identity.Get();
        back_buffer_contract.description = native_back_buffer->GetDesc();
        back_buffer_contract.generation = generation;
        back_buffer_contract.backing = presented_view ? NativeBackingKind::DxgiSwapchain : NativeBackingKind::Allocation;
        back_buffer_contract_device = impl_->device.Get();
    }
    const bool restored_graph = source_candidate &&
        impl_->graph.RestorePlan(source_candidate->handle);
    if (!restored_graph)
    {
        // A later layout check may reject a candidate after the local-probe
        // preflight succeeded. Rebuild native capture bindings for this cold graph.
        local_probe_prepared = false;
        capture_reads.clear();
        local_probe_inputs.scene_reads = {};
    }
    if (!restored_graph) impl_->graph.Reset();
    cpu_timings.graph_frontend_reused = restored_graph;
    FrameGraphLayout cold_graph_layout;
    FrameGraphLayout &pending_graph_layout = restored_graph
        ? *source_candidate : cold_graph_layout;
    if (!restored_graph)
    {
        pending_graph_layout.early = early_key;
        pending_graph_layout.geometry_page_slots = geometry_page_slots;
        pending_graph_layout.software_scene_page_slots = software_scene_page_slots;
        pending_graph_layout.brixelizer_leaf_reads.reserve(brixelizer_leaf_jobs.size());
        for (std::size_t leaf = 0; leaf < brixelizer_leaf_jobs.size(); ++leaf)
            pending_graph_layout.brixelizer_leaf_reads.push_back({brixelizer_leaf_key(leaf), leaf});
        std::ranges::sort(pending_graph_layout.brixelizer_leaf_reads,
            [](const auto &left, const auto &right) {
                return left.key != right.key ? left.key < right.key
                                             : left.read_pair < right.read_pair;
            });
#ifndef NDEBUG
        for (std::size_t index = 1; index < pending_graph_layout.brixelizer_leaf_reads.size(); ++index)
        {
            const auto &previous = pending_graph_layout.brixelizer_leaf_reads[index - 1];
            const auto &current = pending_graph_layout.brixelizer_leaf_reads[index];
            if (previous.key == current.key)
                assert(previous.read_pair < current.read_pair);
        }
#endif
        // Provider preparation may have replaced direct resources after a rejected candidate.
        collect_direct_shapes();
        pending_graph_layout.direct_import_shapes = impl_->graph_direct_shapes_scratch;
        if (direct_contract_failed)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Current allocation owner could not certify its native import shape.");
    }
    std::vector<TextureStreamingGraphBinding> texture_bindings;
    std::vector<LocalProbeTextureFact> local_probe_texture_facts;
    auto copy_queue = std::make_shared<ComPtr<ID3D12CommandQueue>>(
        impl_->graph_queue_executor.NativeQueue(QueueHint::Copy));
    ExternalQueueBinding texture_copy_queue{std::shared_ptr<void>(copy_queue, copy_queue->Get()), QueueHint::Copy, {}};
    const auto streaming_result = restored_graph
        ? impl_->texture_streaming.BindRetained(impl_->graph, std::move(texture_copy_queue), 1000,
            pending_graph_layout.texture_streaming, texture_bindings)
        : impl_->texture_streaming.AddPendingPasses(impl_->graph, std::move(texture_copy_queue),
            1000, texture_bindings, &pending_graph_layout.texture_streaming);
    if (!streaming_result) return streaming_result;
    alignas(std::max_align_t) std::array<std::byte, 16 * 1024> graph_resource_scratch;
    std::pmr::monotonic_buffer_resource graph_resource_arena{
        graph_resource_scratch.data(), graph_resource_scratch.size()};
    std::pmr::unordered_map<ID3D12Resource *, ResourceHandle> canonical_textures{
        &graph_resource_arena};
    for (const auto &binding : texture_bindings)
        canonical_textures.emplace(impl_->texture_streaming.View(binding.texture).resource, binding.sampled.resource);
    const bool capture_facts_needed=local_probe_plan.capture||
        impl_->local_probes.CaptureIdentityPending(local_probe_inputs.completed);
    const bool strict_capture=capture_demand&&wait_for_streaming;
    if(capture_facts_needed)
    {
        if(auto materials=impl_->local_probes.PrepareCaptureMaterialOrdinals(static_scene,foliage_density);!materials)return materials;
        const auto capture_materials=impl_->local_probes.CachedCaptureMaterialOrdinals(static_scene,foliage_density);
        std::unordered_map<std::uint32_t,const TextureStreamingGraphBinding *> by_id;
        by_id.reserve(texture_bindings.size());
        for(const auto &binding:texture_bindings)by_id.emplace(binding.texture.value,&binding);
        std::unordered_set<std::uint32_t> seen;
        std::vector<TextureStreamMipRequirement> capture_texture_requirements;
        if(strict_capture)capture_texture_requirements.reserve(
            capture_materials.size()*render_data::kMaterialTextureSlots);
        for(const auto material:capture_materials)
            for(std::uint32_t slot=0;slot<render_data::kMaterialTextureSlots;++slot)
            {
                const auto id=impl_->material_texture_stream_ids[material*render_data::kMaterialTextureSlots+slot];
                if(!id)continue;
                const auto found=by_id.find(id.value);
                if(found==by_id.end())return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12",
                    "Static probe material has no admitted texture binding.");
                const auto &binding=*found->second;
                if(seen.insert(id.value).second)
                {
                    if(strict_capture)
                        if(const auto fits=impl_->texture_streaming.RequiredSourceMipFitsMaximumHostBudget(
                                id,binding.required_mip);fits&&!*fits)
                            return Result::Failure(ErrorCode::TaskFailed,"hs_renderer_d3d12",
                                "Scene-probe required texture source mip exceeds the maximum host staging budget: "+
                                std::string(binding.logical_id)+" (required mip "+std::to_string(binding.required_mip)+").");
                    if(strict_capture)capture_texture_requirements.push_back({id,binding.required_mip});
                    local_probe_texture_facts.push_back({binding.logical_id,binding.resident_mip,binding.ready});
                }
            }
        if(strict_capture)
            if(const auto fits=impl_->texture_streaming.RequiredMipSetFitsConfiguredBudget(
                    capture_texture_requirements);fits&&!*fits)
                return Result::Failure(ErrorCode::TaskFailed,"hs_renderer_d3d12",
                    "Scene-probe required material texture set exceeds the configured texture residency budget.");
    }
    local_probe_inputs.texture_facts=local_probe_texture_facts;
    local_probe_inputs.texture_facts_valid=capture_facts_needed;
    ResourceVersion texture_metadata;
    const auto imported_feedback = restored_graph
        ? impl_->texture_feedback.BindRetainedMetadata(impl_->graph, frame_slot,
            pending_graph_layout.texture_feedback, texture_metadata)
        : impl_->texture_feedback.ImportMetadata(impl_->graph, frame_slot,
            texture_metadata, &pending_graph_layout.texture_feedback);
    if (!imported_feedback) return imported_feedback;
    std::vector<TextureFeedbackGraphBinding> feedback_bindings;
    const auto clear_feedback = restored_graph
        ? impl_->texture_feedback.BindRetainedClear(impl_->graph, frame_slot, 1200,
            pending_graph_layout.texture_feedback, feedback_bindings)
        : impl_->texture_feedback.AddClearPass(impl_->graph, frame_slot,
            1200, feedback_bindings, &pending_graph_layout.texture_feedback);
    if (!clear_feedback) return clear_feedback;
    ResourceVersion entity_surface_noise_version, entity_surface_gradient_version, entity_surface_stbn_version;
    enum class MaterialSampling { Surface, Opacity };
    const auto read_material_sampling = [&](PassBuilder &pass, bool feedback_writer,
                                           MaterialSampling domain = MaterialSampling::Surface,
                                           Access sampling = Access::AllShaderRead) {
        const bool opacity_only = domain == MaterialSampling::Opacity;
        for (const auto &binding : texture_bindings)
            if (!opacity_only || binding.opacity_sampling) pass.Read(binding.sampled, sampling);
        pass.Read(texture_metadata, sampling);
        if (entity_surface_noise_version.resource)
        {
            pass.Read(entity_surface_noise_version, sampling);
            pass.Read(entity_surface_gradient_version, sampling);
            pass.Read(entity_surface_stbn_version, sampling);
        }
        if (feedback_writer && !restored_graph)
            RendererTextureFeedback::DeclareSampling(pass, feedback_bindings, opacity_only);
    };
    // Keep deduplication local to this frame and indexed by the allocation
    // owner; do not rescan all prior imports on each declaration request.
    std::pmr::unordered_map<AllocationResource *, ResourceHandle> imported_resources{
        &graph_resource_arena};
    imported_resources.reserve(128);
    std::size_t import_cursor{};
    bool import_binding_failed{};
    const auto import_buffer = [&](AllocationResource &resource, Access initial,
                                   std::string_view name) {
        if (const auto existing = imported_resources.find(&resource); existing != imported_resources.end())
            return existing->second;
        if (resource.allocation)
            if (auto certified = resource.EnsureNativeContract(impl_->device.Get()); !certified)
                import_binding_failed = true;
        const auto access = resource.graph_access == Access::Undefined
            ? initial : resource.graph_access;
        ResourceHandle handle;
        if (restored_graph)
        {
            if (import_cursor >= pending_graph_layout.imports.size() ||
                pending_graph_layout.imports[import_cursor].name != name ||
                pending_graph_layout.imports[import_cursor].handle.kind != ResourceKind::Buffer)
                import_binding_failed = true;
            else
            {
                handle = pending_graph_layout.imports[import_cursor].handle;
                import_binding_failed |= !impl_->graph.BindBuffer(handle,
                    {resource.resource.Get(), resource.graph_completion, resource.graph_queue, {},
                     resource.NativeContract()}, access);
            }
            ++import_cursor;
        }
        else
            handle = impl_->graph.ImportBuffer(
                {resource.resource.Get(), resource.graph_completion, resource.graph_queue, {},
                 resource.NativeContract()}, access, name);
        imported_resources.emplace(&resource, handle);
        if (!restored_graph)
        {
            const auto *contract = resource.NativeContract();
            pending_graph_layout.certified_imports &= contract != nullptr;
            pending_graph_layout.imports.push_back({std::string(name), handle,
                contract ? contract->description : resource.resource ? resource.resource->GetDesc() : D3D12_RESOURCE_DESC{},
                contract ? contract->buffer_heap : D3D12_HEAP_TYPE_DEFAULT, contract != nullptr});
        }
        return handle;
    };
    const auto import_texture = [&](AllocationResource &resource, Access initial,
                                    std::string_view name) {
        if (const auto existing = imported_resources.find(&resource); existing != imported_resources.end())
            return existing->second;
        if (resource.allocation)
            if (auto certified = resource.EnsureNativeContract(impl_->device.Get()); !certified)
                import_binding_failed = true;
        if (const auto existing = canonical_textures.find(resource.resource.Get()); existing != canonical_textures.end())
        {
            imported_resources.emplace(&resource, existing->second);
            return existing->second;
        }
        const auto access = resource.graph_access == Access::Undefined
            ? initial : resource.graph_access;
        ResourceHandle handle;
        if (restored_graph)
        {
            if (import_cursor >= pending_graph_layout.imports.size() ||
                pending_graph_layout.imports[import_cursor].name != name ||
                pending_graph_layout.imports[import_cursor].handle.kind != ResourceKind::Texture)
                import_binding_failed = true;
            else
            {
                handle = pending_graph_layout.imports[import_cursor].handle;
                import_binding_failed |= !impl_->graph.BindTexture(handle,
                    {resource.resource.Get(), resource.graph_completion, resource.graph_queue, {},
                     resource.NativeContract()}, access);
            }
            ++import_cursor;
        }
        else
            handle = impl_->graph.ImportTexture(
                {resource.resource.Get(), resource.graph_completion, resource.graph_queue, {},
                 resource.NativeContract()}, access, name);
        imported_resources.emplace(&resource, handle);
        if (!restored_graph)
        {
            const auto *contract = resource.NativeContract();
            pending_graph_layout.certified_imports &= contract != nullptr;
            pending_graph_layout.imports.push_back({std::string(name), handle,
                contract ? contract->description : resource.resource ? resource.resource->GetDesc() : D3D12_RESOURCE_DESC{},
                contract ? contract->buffer_heap : D3D12_HEAP_TYPE_DEFAULT, contract != nullptr});
        }
        return handle;
    };
    TextureHandle environment_stream_texture{};
    const auto environment_brdf_lut = import_texture(
        impl_->environment_brdf_lut, Access::ShaderRead, "EnvironmentBrdfLut");
    const auto material_parameter_buffer = import_buffer(
        impl_->material_parameters, Access::ShaderRead, "MaterialParameters");
    const auto material_parameter_version = impl_->graph.Version(material_parameter_buffer);
    const auto hero_binding_buffer = import_buffer(impl_->hero_material_bindings_gpu, Access::AllShaderRead,
        "HeroMaterialBindings");
    const auto hero_binding_version = impl_->graph.Version(hero_binding_buffer);
    if (!entity_surface_frame.Bytes().empty())
    {
        entity_surface_noise_version = impl_->graph.Version(import_texture(
            impl_->vfx_noise_volume, Access::AllShaderRead, "EntitySurfaceNoiseVolume"));
        entity_surface_gradient_version = impl_->graph.Version(import_texture(
            impl_->vfx_gradient, Access::AllShaderRead, "EntitySurfaceGradient"));
        entity_surface_stbn_version = impl_->graph.Version(import_texture(
            impl_->vfx_stbn_scalar, Access::AllShaderRead, "EntitySurfaceStbn"));
    }
    ResourceVersion environment_stream_version{};
    if (upload_environment_stream)
    {
        environment_stream_texture = import_texture(
            impl_->environment_stream.candidate,
            impl_->enhanced ? Access::Common : Access::CopyDest,
            "EnvironmentStreamCandidate");
        environment_stream_version = impl_->graph.Version(environment_stream_texture);
    }
    auto &dynamic_lod_state = owned_view.dynamic_lod_states;
    auto *static_lod_state = static_scene && impl_->static_gpu_scene
        ? &impl_->static_gpu_scene->lod_states : nullptr;
    const auto dynamic_lod_buffer = import_buffer(dynamic_lod_state.states,
        dynamic_lod_state.initialized ? Access::UnorderedWrite : Access::CopyDest, "DynamicLodStates");
    BufferHandle static_lod_buffer{}, dynamic_lod_prefix{}, static_lod_prefix{};
    if (static_lod_state)
        static_lod_buffer = import_buffer(static_lod_state->states,
            static_lod_state->initialized ? Access::UnorderedWrite : Access::CopyDest, "StaticLodStates");
    if (dynamic_lod_state.prefix_source.resource)
        dynamic_lod_prefix = import_buffer(dynamic_lod_state.prefix_source, Access::UnorderedWrite,
            "DynamicLodPrefix");
    if (static_lod_state && static_lod_state->prefix_source.resource)
        static_lod_prefix = import_buffer(static_lod_state->prefix_source, Access::UnorderedWrite,
            "StaticLodPrefix");
    auto dynamic_lod_version = impl_->graph.Version(dynamic_lod_buffer);
    auto static_lod_version = impl_->graph.Version(static_lod_buffer);
    const bool initialize_dynamic_lod = !dynamic_lod_state.initialized;
    const bool initialize_static_lod = static_lod_state && !static_lod_state->initialized;
    struct GeometryStreamUploadGraph
    {
        Impl::EnvironmentGeometryStream *stream{};
        BufferHandle buffer{};
        ResourceVersion version{};
    };
    std::vector<GeometryStreamUploadGraph> environment_geometry_uploads;
    for (auto &stream : impl_->environment_geometry_streams)
    {
        if (!stream.upload_pending) continue;
        GeometryStreamUploadGraph upload{&stream};
        auto &candidate = stream.candidate;
        upload.buffer = import_buffer(candidate.storage, Access::CopyDest, "GeometryStreamPage");
        upload.version = impl_->graph.Version(upload.buffer);
        environment_geometry_uploads.push_back(upload);
    }
    const auto distortion_vectors = restored_graph ? pending_graph_layout.distortion : impl_->graph.CreateTexture(
        {owned_view.render_width,owned_view.render_height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET}, "VFX Distortion Vectors");
    if (!restored_graph) pending_graph_layout.distortion = distortion_vectors;
    const auto ribbon_history = import_buffer(impl_->ribbon_history, Access::UnorderedWrite, "RibbonHistory");
    const auto ribbon_previous = import_buffer(impl_->ribbon_previous, Access::UnorderedWrite, "RibbonPrevious");
    const auto ribbon_arguments = import_buffer(frame.ribbon_arguments, Access::UnorderedWrite, "RibbonArguments");
    const auto draw_ribbons = [&](ID3D12GraphicsCommandList *list, bool oit) {
        list->SetPipelineState(oit ? impl_->ribbon_oit_pipeline.Get() : impl_->ribbon_add_pipeline.Get());
        list->SetGraphicsRootShaderResourceView(18, frame.upload.resource->GetGPUVirtualAddress() + ribbon_output_offset);
        list->SetGraphicsRootDescriptorTable(19, ribbon_skin_table);
        list->SetGraphicsRootShaderResourceView(20, impl_->ribbon_previous.resource->GetGPUVirtualAddress());
        const auto split = std::ranges::find_if(ribbon_outputs, [](const GpuRibbonOutput &output) {
            return (output.metadata[3] & 1u) != 0;
        });
        const auto first = oit ? static_cast<std::size_t>(split - ribbon_outputs.begin()) : 0;
        const auto count = oit ? ribbon_outputs.size() - first : static_cast<std::size_t>(split - ribbon_outputs.begin());
        if (count != 0)
            list->ExecuteIndirect(impl_->ribbon_draw_signature.Get(), static_cast<UINT>(count),
                frame.ribbon_arguments.resource.Get(), first * kRibbonArgumentStride, nullptr, 0);
        list->SetGraphicsRoot32BitConstant(6, 0, 0);
    };
    const auto particles = import_buffer(impl_->particles, Access::UnorderedWrite, "Particles");
    const auto active_particle_alive_index = impl_->particle_input_is_a ? 0u : 1u;
    // In an explicit multi-view package, the first accepted view has already
    // produced the shared world result. Later views bind that result as output
    // for drawing but must not mutate the particle world a second time.
    const auto particle_input_index = impl_->render_scene_inputs
        ? active_particle_alive_index : (active_particle_alive_index ^ 1u);
    const auto particle_output_index = impl_->render_scene_inputs
        ? (active_particle_alive_index ^ 1u) : active_particle_alive_index;
    const auto particle_alive_input = import_buffer(impl_->particle_alive[particle_input_index], Access::UnorderedWrite, "ParticleAliveInput");
    const auto particle_alive_output = import_buffer(impl_->particle_alive[particle_output_index], Access::UnorderedWrite, "ParticleAliveOutput");
    const auto particle_dead = import_buffer(impl_->particle_dead, Access::UnorderedWrite, "ParticleDead");
    const auto particle_counters = import_buffer(impl_->particle_counters, Access::UnorderedWrite, "ParticleCounters");
    const auto indirect_arguments = import_buffer(impl_->indirect_arguments, Access::UnorderedWrite, "IndirectArguments");
    const auto dynamic_instances_buffer = import_buffer(dynamic_scene,
        current_instance_snapshot->initialized ? Access::ShaderRead : Access::CopyDest,
        "DynamicSceneInstances");
    BufferHandle previous_instance_snapshot_buffer{};
    ResourceVersion previous_instance_snapshot_version{};
    if (separate_previous_instance_scene)
    {
        previous_instance_snapshot_buffer = import_buffer(previous_instance_snapshot->scene,
            Access::CopyDest, "PreviousInstanceSnapshot");
        previous_instance_snapshot_version = impl_->graph.Version(previous_instance_snapshot_buffer);
    }
    const auto geometry_records = import_buffer(frame.geometry_draw_records, Access::CopyDest,
                                                "GeometryDrawRecords");
    struct GeometryPageGraph
    {
        const GeometryPageGpu *page{};
        ResourceVersion version{};
    };
    std::vector<GeometryPageGraph> geometry_pages;
    geometry_pages.reserve(geometry_page_slots.size());
    for (const auto page_slot : geometry_page_slots)
    {
        const auto *page = impl_->FindGeometryPage(page_slot);
        if (!page)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Geometry graph has no page for a prepared draw record.");
        GeometryPageGraph entry{page};
        entry.version = impl_->graph.Version(import_buffer(
            const_cast<AllocationResource &>(page->storage), Access::VertexShaderRead, "GeometryPage"));
        geometry_pages.push_back(entry);
    }
    const auto read_geometry_pages = [&](auto &pass, bool skin_only = false) {
        for (const auto &entry : geometry_pages)
        {
            if (skin_only)
            {
                if ((entry.page->page_slot >= kArcherGeometryPage &&
                        entry.page->page_slot < kProjectileGeometryPage) ||
                    (entry.page->page_slot >= kMonsterReducedGeometryPage &&
                        entry.page->page_slot < kMonsterReducedGeometryPage + 12u))
                    pass.Read(entry.version, Access::ComputeRead);
                continue;
            }
            pass.Read(entry.version, Access::VertexShaderRead);
        }
    };
    BufferHandle skin_cache_buffer{};
    if (have_skinned_instances)
        skin_cache_buffer = import_buffer(frame.skin_cache_vertices, frame.skin_cache_vertex_read ? Access::ShaderRead : Access::UnorderedWrite, "SkinCacheVertices");
    BufferHandle skin_bounds_buffer{};
    if (have_skinned_instances)
        skin_bounds_buffer = import_buffer(frame.skin_cache_bounds,
            frame.skin_cache_bounds_read ? Access::ShaderRead : Access::UnorderedWrite,
            "SkinCacheBounds");
    BufferHandle previous_skin_cache_buffer{};
    if (have_skinned_instances && previous_skin_frame_valid)
        previous_skin_cache_buffer = import_buffer(previous_skin_frame->skin_cache_vertices, Access::ShaderRead, "PreviousSkinCacheVertices");
    BufferHandle static_instances_buffer{};
    if (static_scene && impl_->static_gpu_scene)
        static_instances_buffer = import_buffer(impl_->static_gpu_scene->instances, impl_->static_gpu_scene->submitted ? Access::ShaderRead : Access::CopyDest, "StaticSceneInstances");
    BufferHandle static_refs_buffer{};
    if (static_scene && impl_->static_gpu_scene && impl_->static_gpu_scene->refs)
        static_refs_buffer = import_buffer(impl_->static_gpu_scene->refs->storage,
            upload_static_refs ? Access::CopyDest : Access::AllShaderRead, "StaticSceneDrawRefs");
    const auto static_visible_refs = import_buffer(frame.static_visible_refs, Access::UnorderedWrite, "StaticVisibleRefs");
    const auto static_draw_args = import_buffer(frame.static_draw_args, Access::UnorderedWrite, "StaticDrawArguments");
    const auto dynamic_visible_refs = import_buffer(frame.dynamic_visible_refs, Access::UnorderedWrite, "DynamicVisibleRefs");
    const auto dynamic_draw_args = import_buffer(frame.dynamic_draw_args, Access::UnorderedWrite, "DynamicDrawArguments");
    const auto static_rejected_refs = import_buffer(frame.static_rejected_refs, Access::UnorderedWrite, "StaticRejectedRefs");
    const auto static_rejected_args = import_buffer(frame.static_rejected_args, Access::UnorderedWrite, "StaticRejectedArguments");
    const auto static_retest_refs = import_buffer(frame.static_retest_refs, Access::UnorderedWrite, "StaticRetestRefs");
    const auto static_retest_args = import_buffer(frame.static_retest_args, Access::UnorderedWrite, "StaticRetestArguments");
    const auto hzb_write_index = owned_view.hzb_read_index ^ 1u;
    const auto previous_hzb = import_buffer(owned_view.targets.hzb_history[owned_view.hzb_read_index], Access::ComputeRead, "PreviousHZB");
    const auto current_hzb = import_buffer(owned_view.targets.hzb_history[hzb_write_index], Access::ComputeRead, "CurrentHZB");
    const auto initial_depth_access =
        owned_view.transient_textures_common ? Access::Common : Access::DepthWrite;
    const auto initial_color_access =
        owned_view.transient_textures_common ? Access::Common : Access::ShaderRead;
    TextureHandle shadow{};
    const auto depth =
        import_texture(owned_view.targets.depth, initial_depth_access, "Depth");
    const auto gbuffer_base = import_texture(owned_view.targets.gbuffer_base, initial_color_access, "GBufferBase");
    const auto gbuffer_normal = import_texture(owned_view.targets.gbuffer_normal, initial_color_access, "GBufferNormal");
    const auto gbuffer_metadata = import_texture(owned_view.targets.gbuffer_metadata, initial_color_access, "GBufferMetadata");
    const auto hdr = import_texture(owned_view.targets.hdr_color, initial_color_access, "HdrColor");
    const auto subsurface_diffuse = owned_view.targets.subsurface_diffuse.resource
        ? import_texture(owned_view.targets.subsurface_diffuse, initial_color_access, "SubsurfaceDiffuse") : TextureHandle{};
    const auto environment_specular = owned_view.targets.environment_specular.resource
        ? import_texture(owned_view.targets.environment_specular, initial_color_access, "EnvironmentSpecular") : TextureHandle{};
    const auto indirect_diffuse = owned_view.targets.indirect_diffuse.resource
        ? import_texture(owned_view.targets.indirect_diffuse, initial_color_access, "IndirectDiffuse") : TextureHandle{};
    const auto oit_accumulation = import_texture(owned_view.targets.oit_accumulation, initial_color_access, "OitAccumulation");
    const auto oit_revealage = import_texture(owned_view.targets.oit_revealage, initial_color_access, "OitRevealage");
    const auto post_a = import_texture(owned_view.targets.post_a, initial_color_access, "PostA");
    const auto gbuffer_material = import_texture(owned_view.targets.gbuffer_material, initial_color_access, "GBufferMaterial");
    const auto gbuffer_motion = import_texture(owned_view.targets.gbuffer_motion, initial_color_access, "GBufferMotion");
    const auto gbuffer_surface_history = import_texture(owned_view.targets.gbuffer_surface_history,
        initial_color_access, "SurfaceHistory");
    std::array<ResourceVersion, 2> material_sidecar_versions{};
    for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
    {
        const auto access = !owned_view.material_sidecars_initialized && impl_->enhanced
            ? Access::Common : Access::ShaderRead;
        const auto handle = import_texture(owned_view.targets.material_sidecars[plane], access,
                                           "MaterialSidecar" + std::to_string(plane));
        material_sidecar_versions[plane] = impl_->graph.Version(handle);
    }
    const auto post_b = owned_view.targets.post_b.resource
        ? import_texture(owned_view.targets.post_b, initial_color_access, "PostB") : TextureHandle{};
    const auto temporal_initial_access = owned_view.temporal_targets_initialized
        ? Access::ShaderRead
        : (impl_->enhanced ? Access::Common : Access::ShaderRead);
    const auto temporal_read = owned_view.targets.temporal_history[temporal_read_index].resource ? import_texture(
        owned_view.targets.temporal_history[temporal_read_index], temporal_initial_access,
        "TemporalHistoryRead") : TextureHandle{};
    const auto temporal_write = owned_view.targets.temporal_history[temporal_write_index].resource ? import_texture(
        owned_view.targets.temporal_history[temporal_write_index], temporal_initial_access,
        "TemporalHistoryWrite") : TextureHandle{};
    const auto view_initial_access = !owned_view.view_targets_initialized && impl_->enhanced
        ? Access::Common : Access::ShaderRead;
    const auto scene_work = import_texture(owned_view.targets.scene_work, view_initial_access, "SceneWork");
    const auto display_linear = import_texture(owned_view.targets.display_linear, view_initial_access, "DisplayLinear");
    const auto composite_hdr = import_texture(owned_view.targets.composite_hdr, view_initial_access, "CompositeHdr");
    const auto temporal_normal_read = owned_view.targets.temporal_normal_history[temporal_read_index].resource
        ? import_texture(owned_view.targets.temporal_normal_history[temporal_read_index], view_initial_access, "TemporalNormalRead") : TextureHandle{};
    const auto temporal_normal_write = owned_view.targets.temporal_normal_history[temporal_write_index].resource
        ? import_texture(owned_view.targets.temporal_normal_history[temporal_write_index], view_initial_access, "TemporalNormalWrite") : TextureHandle{};
    const auto temporal_material_read = owned_view.targets.temporal_material_history[temporal_read_index].resource
        ? import_texture(owned_view.targets.temporal_material_history[temporal_read_index], view_initial_access, "TemporalMaterialRead") : TextureHandle{};
    const auto temporal_material_write = owned_view.targets.temporal_material_history[temporal_write_index].resource
        ? import_texture(owned_view.targets.temporal_material_history[temporal_write_index], view_initial_access, "TemporalMaterialWrite") : TextureHandle{};
    const auto exposure_read = import_buffer(owned_view.exposure_values[exposure_read_index],
        owned_view.exposure_initialized ? Access::AllShaderRead : Access::CopyDest, "ExposureCurrent");
    const auto exposure_write = import_buffer(owned_view.exposure_values[exposure_write_index],
        owned_view.exposure_initialized ? Access::AllShaderRead : Access::CopyDest, "ExposureNext");
    const auto exposure_histogram = import_buffer(owned_view.exposure_histogram, Access::UnorderedWrite, "ExposureHistogram");
    TextureHandle bloom_half{};
    TextureHandle bloom_quarter{};
    TextureHandle bloom_half_combined{};
    const auto half_width = owned_view.width / 2 + owned_view.width % 2;
    const auto half_height = owned_view.height / 2 + owned_view.height % 2;
    const auto quarter_width = half_width / 2 + half_width % 2;
    const auto quarter_height = half_height / 2 + half_height % 2;
    if (impl_->config.bloom)
    {
        constexpr auto format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr auto flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (restored_graph)
        {
            bloom_half = pending_graph_layout.bloom_half;
            bloom_quarter = pending_graph_layout.bloom_quarter;
            bloom_half_combined = pending_graph_layout.bloom_combined;
        }
        else
        {
            bloom_half = impl_->graph.CreateTexture({half_width, half_height, format, flags}, "BloomHalf");
            bloom_quarter = impl_->graph.CreateTexture({quarter_width, quarter_height, format, flags}, "BloomQuarter");
            bloom_half_combined = impl_->graph.CreateTexture({half_width, half_height, format, flags}, "BloomHalfCombined");
        }
    }
    if (!restored_graph)
    {
        pending_graph_layout.bloom_half = bloom_half;
        pending_graph_layout.bloom_quarter = bloom_quarter;
        pending_graph_layout.bloom_combined = bloom_half_combined;
    }
    const auto ui = import_texture(ui_texture,
        owned_view.ui_initialized[frame_slot] ? Access::ShaderRead : Access::Common, "UiTexture");
    const auto back_buffer = restored_graph ? pending_graph_layout.back_buffer : impl_->graph.ImportTexture(
        {frame_output.Get(),
         output_completion, QueueHint::Direct, {},
         &back_buffer_contract}, Access::Present, "BackBuffer");
    if (restored_graph)
        import_binding_failed |= !impl_->graph.BindTexture(back_buffer,
            {frame_output.Get(),
             output_completion, QueueHint::Direct, {},
             &back_buffer_contract}, Access::Present);
    else
        pending_graph_layout.back_buffer = back_buffer;

    // Keep physical handles for descriptors/callbacks; declarations below carry
    // the contents version consumed or produced by each pass.
    auto particles_version = impl_->graph.Version(particles);
    auto particle_alive_input_version = impl_->graph.Version(particle_alive_input);
    auto particle_alive_output_version = impl_->graph.Version(particle_alive_output);
    auto particle_dead_version = impl_->graph.Version(particle_dead);
    auto particle_counters_version = impl_->graph.Version(particle_counters);
    auto indirect_arguments_version = impl_->graph.Version(indirect_arguments);
    auto dynamic_instances_version = impl_->graph.Version(dynamic_instances_buffer);
    auto geometry_records_version = impl_->graph.Version(geometry_records);
    ResourceVersion skin_cache_version{};
    if (have_skinned_instances) skin_cache_version = impl_->graph.Version(skin_cache_buffer);
    ResourceVersion skin_bounds_version{};
    if (have_skinned_instances) skin_bounds_version = impl_->graph.Version(skin_bounds_buffer);
    ResourceVersion previous_skin_cache_version{};
    if (have_skinned_instances && previous_skin_frame_valid)
        previous_skin_cache_version = impl_->graph.Version(previous_skin_cache_buffer);
    auto static_instances_version = impl_->graph.Version(static_instances_buffer);
    auto static_refs_version = impl_->graph.Version(static_refs_buffer);
    auto static_visible_refs_version = impl_->graph.Version(static_visible_refs);
    auto static_draw_args_version = impl_->graph.Version(static_draw_args);
    auto dynamic_visible_refs_version = impl_->graph.Version(dynamic_visible_refs);
    auto dynamic_draw_args_version = impl_->graph.Version(dynamic_draw_args);
    auto static_rejected_refs_version = impl_->graph.Version(static_rejected_refs);
    auto static_rejected_args_version = impl_->graph.Version(static_rejected_args);
    auto static_retest_refs_version = impl_->graph.Version(static_retest_refs);
    auto static_retest_args_version = impl_->graph.Version(static_retest_args);
    auto previous_hzb_version = impl_->graph.Version(previous_hzb);
    auto current_hzb_version = impl_->graph.Version(current_hzb);
    auto ribbon_history_version = impl_->graph.Version(ribbon_history);
    auto ribbon_previous_version = impl_->graph.Version(ribbon_previous);
    auto ribbon_arguments_version = impl_->graph.Version(ribbon_arguments);
    ResourceVersion shadow_version{};
    auto depth_version = impl_->graph.Version(depth);
    auto gbuffer_base_version = impl_->graph.Version(gbuffer_base);
    auto gbuffer_normal_version = impl_->graph.Version(gbuffer_normal);
    auto gbuffer_metadata_version = impl_->graph.Version(gbuffer_metadata);
    auto gbuffer_material_version = impl_->graph.Version(gbuffer_material);
    auto gbuffer_motion_version = impl_->graph.Version(gbuffer_motion);
    auto gbuffer_surface_history_version = impl_->graph.Version(gbuffer_surface_history);
    TextureHandle visibility_ids{}, visibility_front_face{};
    BufferHandle raster_records{}, visibility_pixels{}, visibility_counts{}, visibility_args{};
    ResourceVersion visibility_ids_version{}, visibility_face_version{}, raster_records_version{};
    ResourceVersion visibility_pixels_version{}, visibility_counts_version{}, visibility_args_version{};
    if (use_visibility)
    {
        visibility_ids = import_texture(owned_view.targets.visibility_ids, Access::UnorderedWrite, "VisibilityIds");
        visibility_front_face = import_texture(owned_view.targets.visibility_front_face, Access::UnorderedWrite, "VisibilityFrontFace");
        raster_records = import_buffer(visibility_frame.raster_records, Access::CopyDest, "RasterDrawRecords");
        visibility_pixels = import_buffer(owned_view.targets.visibility_pixels, Access::CopyDest, "ClassifiedPixels");
        visibility_counts = import_buffer(owned_view.targets.visibility_counts, Access::CopyDest, "MaterialVariantCounts");
        visibility_args = import_buffer(owned_view.targets.visibility_dispatch_args, Access::CopyDest, "MaterialDispatchArgs");
        visibility_ids_version = impl_->graph.Version(visibility_ids);
        visibility_face_version = impl_->graph.Version(visibility_front_face);
        raster_records_version = impl_->graph.Version(raster_records);
        visibility_pixels_version = impl_->graph.Version(visibility_pixels);
        visibility_counts_version = impl_->graph.Version(visibility_counts);
        visibility_args_version = impl_->graph.Version(visibility_args);
    }
    auto hdr_version = impl_->graph.Version(hdr);
    auto *opaque_hdr_resource = owned_view.targets.hdr_color.resource.Get();
    auto subsurface_diffuse_version = impl_->graph.Version(subsurface_diffuse);
    auto environment_specular_version = impl_->graph.Version(environment_specular);
    auto indirect_diffuse_version = impl_->graph.Version(indirect_diffuse);
    auto distortion_vectors_version = impl_->graph.Version(distortion_vectors);
    auto oit_accumulation_version = impl_->graph.Version(oit_accumulation);
    auto oit_revealage_version = impl_->graph.Version(oit_revealage);
    auto post_a_version = impl_->graph.Version(post_a);
    auto post_b_version = impl_->graph.Version(post_b);
    auto temporal_read_version = impl_->graph.Version(temporal_read);
    auto temporal_write_version = impl_->graph.Version(temporal_write);
    auto scene_work_version = impl_->graph.Version(scene_work);
    auto display_linear_version = impl_->graph.Version(display_linear);
    auto composite_hdr_version = impl_->graph.Version(composite_hdr);
    auto *composite_hdr_resource = owned_view.targets.composite_hdr.resource.Get();
    auto temporal_normal_read_version = impl_->graph.Version(temporal_normal_read);
    auto temporal_normal_write_version = impl_->graph.Version(temporal_normal_write);
    auto temporal_material_read_version = impl_->graph.Version(temporal_material_read);
    auto temporal_material_write_version = impl_->graph.Version(temporal_material_write);
    auto exposure_read_version = impl_->graph.Version(exposure_read);
    auto exposure_write_version = impl_->graph.Version(exposure_write);
    auto histogram_version = impl_->graph.Version(exposure_histogram);
    auto bloom_half_version = impl_->graph.Version(bloom_half);
    auto bloom_quarter_version = impl_->graph.Version(bloom_quarter);
    auto bloom_half_combined_version = impl_->graph.Version(bloom_half_combined);
    auto ui_version = impl_->graph.Version(ui);
    auto back_buffer_version = impl_->graph.Version(back_buffer);

    const auto capture_surface_versions = [&](SurfacePassGraphLayout &layout) {
        layout.versions = {visibility_ids_version, visibility_face_version, visibility_counts_version,
            gbuffer_base_version, gbuffer_normal_version, gbuffer_metadata_version,
            gbuffer_material_version, gbuffer_motion_version, gbuffer_surface_history_version, depth_version};
        layout.sidecars.assign(material_sidecar_versions.begin(),
            material_sidecar_versions.begin() + impl_->material_sidecar_layout.plane_count);
    };
    const auto restore_surface_versions = [&](const SurfacePassGraphLayout &layout) {
        if (layout.sidecars.size() != impl_->material_sidecar_layout.plane_count) return false;
        visibility_ids_version = layout.versions[0];
        visibility_face_version = layout.versions[1];
        visibility_counts_version = layout.versions[2];
        gbuffer_base_version = layout.versions[3];
        gbuffer_normal_version = layout.versions[4];
        gbuffer_metadata_version = layout.versions[5];
        gbuffer_material_version = layout.versions[6];
        gbuffer_motion_version = layout.versions[7];
        gbuffer_surface_history_version = layout.versions[8];
        depth_version = layout.versions[9];
        std::copy(layout.sidecars.begin(), layout.sidecars.end(), material_sidecar_versions.begin());
        return true;
    };

    record_graph_resource_detail(cpu_timings.graph_resource_layout_remainder_us);
    record_graph_substage(cpu_timings.graph_resources_us);
    if (!owned_view.exposure_initialized)
    {
        const auto exposure_upload = import_buffer(owned_view.initial_exposure, Access::CopySource, "InitialExposure");
        auto &layout = pending_graph_layout.initial.exposure;
        std::optional<PassBuilder> initialize;
        if (restored_graph)
        {
            initialize = impl_->graph.BindPass(layout.pass_index, 32);
            if (!initialize) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Exposure initialization does not match the retained graph plan.");
            exposure_read_version = layout.first;
            exposure_write_version = layout.second;
        }
        else
        {
            layout.pass_index = impl_->graph.PassCount();
            initialize = impl_->graph.AddPass(32, "ExposureInitialization", QueueHint::Copy);
            initialize->Read(impl_->graph.Version(exposure_upload), Access::CopySource);
            exposure_read_version = initialize->Write(exposure_read_version, Access::CopyDest);
            exposure_write_version = initialize->Write(exposure_write_version, Access::CopyDest);
            layout.first = exposure_read_version;
            layout.second = exposure_write_version;
        }
        initialize->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            for (auto &value : owned_view.exposure_values)
                list->CopyBufferRegion(value.resource.Get(), 0, owned_view.initial_exposure.resource.Get(), 0, 2 * sizeof(float));
        });
    }

    if (owned_view.ui_dirty[frame_slot])
    {
    auto &layout = pending_graph_layout.initial.ui;
    std::optional<PassBuilder> ui_upload_pass;
    if (restored_graph)
    {
        ui_upload_pass = impl_->graph.BindPass(layout.pass_index, 23);
        if (!ui_upload_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "UI upload does not match the retained graph plan.");
        ui_version = layout.output;
    }
    else
    {
        layout.pass_index = impl_->graph.PassCount();
        ui_upload_pass = impl_->graph.AddPass(23, kRenderPassNames[23], QueueHint::Copy);
        ui_version = ui_upload_pass->Write(ui_version, Access::CopyDest);
        layout.output = ui_version;
    }
    ui_upload_pass->SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = ui_texture.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = frame.ui_upload.resource.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = owned_view.ui.footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    });
    }

    if (initialize_temporal_targets)
    {
        auto &layout = pending_graph_layout.initial;
        std::optional<PassBuilder> initialize_pass;
        if (restored_graph)
        {
            initialize_pass = impl_->graph.BindPass(layout.temporal_pass_index, 24);
            if (!initialize_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Temporal initialization does not match the retained graph plan.");
            temporal_read_version = layout.temporal[0];
            temporal_write_version = layout.temporal[1];
            temporal_normal_read_version = layout.temporal[2];
            temporal_normal_write_version = layout.temporal[3];
            temporal_material_read_version = layout.temporal[4];
            temporal_material_write_version = layout.temporal[5];
        }
        else
        {
            layout.temporal_pass_index = impl_->graph.PassCount();
            initialize_pass = impl_->graph.AddPass(24, kRenderPassNames[24], QueueHint::Direct);
            temporal_read_version = initialize_pass->Write(temporal_read_version, Access::RenderTarget);
            temporal_write_version = initialize_pass->Write(temporal_write_version, Access::RenderTarget);
            temporal_normal_read_version = initialize_pass->Write(temporal_normal_read_version, Access::RenderTarget);
            temporal_normal_write_version = initialize_pass->Write(temporal_normal_write_version, Access::RenderTarget);
            temporal_material_read_version = initialize_pass->Write(temporal_material_read_version, Access::RenderTarget);
            temporal_material_write_version = initialize_pass->Write(temporal_material_write_version, Access::RenderTarget);
            layout.temporal = {temporal_read_version, temporal_write_version, temporal_normal_read_version,
                temporal_normal_write_version, temporal_material_read_version, temporal_material_write_version};
        }
        initialize_pass->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            // Discard initializes newly allocated render-target metadata after
            // the graph's transition; the clear defines every history pixel.
            list->DiscardResource(owned_view.targets.temporal_history[0].resource.Get(), nullptr);
            list->DiscardResource(owned_view.targets.temporal_history[1].resource.Get(), nullptr);
            for (auto &history : owned_view.targets.temporal_normal_history) list->DiscardResource(history.resource.Get(), nullptr);
            for (auto &history : owned_view.targets.temporal_material_history) list->DiscardResource(history.resource.Get(), nullptr);
            constexpr float clear[] = {0, 0, 0, 0};
            list->ClearRenderTargetView(rtv_at(kTemporalRtvOffset), clear, 0, nullptr);
            list->ClearRenderTargetView(rtv_at(kTemporalRtvOffset + 1), clear, 0, nullptr);
            for (std::uint32_t plane = 0; plane < 4; ++plane)
                list->ClearRenderTargetView(rtv_at(kViewRtvOffset + plane), clear, 0, nullptr);
        });
    }

    const bool has_geometry_draw_records = !frame.geometry_draw_records_cpu.empty();
    const bool needs_scene_upload_pass = has_geometry_draw_records || upload_dynamic_scene ||
        upload_static_scene || upload_static_refs || upload_environment_stream ||
        upload_environment_geometry || initialize_dynamic_lod || initialize_static_lod ||
        dynamic_lod_prefix || static_lod_prefix;
    if (needs_scene_upload_pass)
    {
        std::optional<PassBuilder> upload_pass;
        auto &upload_layout = pending_graph_layout.upload;
        if (restored_graph)
        {
            upload_pass = impl_->graph.BindPass(upload_layout.pass_index, 0);
            if (!upload_pass || upload_layout.geometry_streams.size() != environment_geometry_uploads.size())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Frame upload does not match the retained graph plan.");
            dynamic_lod_version = upload_layout.dynamic_lod;
            static_lod_version = upload_layout.static_lod;
            geometry_records_version = upload_layout.geometry_records;
            dynamic_instances_version = upload_layout.dynamic_instances;
            static_instances_version = upload_layout.static_instances;
            static_refs_version = upload_layout.static_refs;
            environment_stream_version = upload_layout.environment_stream;
            for (std::size_t index = 0; index < environment_geometry_uploads.size(); ++index)
                environment_geometry_uploads[index].version = upload_layout.geometry_streams[index];
        }
        else
        {
        upload_layout.pass_index = impl_->graph.PassCount();
        upload_pass = impl_->graph.AddPass(0, kRenderPassNames[0], QueueHint::Copy);
        if (initialize_dynamic_lod)
            dynamic_lod_version = upload_pass->Write(dynamic_lod_version, Access::CopyDest);
        if (initialize_static_lod)
            static_lod_version = upload_pass->Write(static_lod_version, Access::CopyDest);
        if (dynamic_lod_prefix)
            upload_pass->Read(impl_->graph.Version(dynamic_lod_prefix), Access::CopySource);
        if (static_lod_prefix)
            upload_pass->Read(impl_->graph.Version(static_lod_prefix), Access::CopySource);
        if (has_geometry_draw_records)
            geometry_records_version = upload_pass->Write(geometry_records_version, Access::CopyDest);
        if (upload_dynamic_scene)
            dynamic_instances_version = upload_pass->ReadWrite(dynamic_instances_version,
                                                              Access::CopyDest);
        if (upload_static_scene)
            static_instances_version = upload_pass->Write(static_instances_version,
                                                         Access::CopyDest);
        if (upload_static_refs)
            static_refs_version = upload_pass->Write(static_refs_version, Access::CopyDest);
        if (upload_environment_stream)
            environment_stream_version =
                upload_pass->Write(environment_stream_version, Access::CopyDest);
        if (upload_environment_geometry)
            for (auto &upload : environment_geometry_uploads)
                upload.version = upload_pass->Write(upload.version, Access::CopyDest);
        upload_layout.dynamic_lod = dynamic_lod_version;
        upload_layout.static_lod = static_lod_version;
        upload_layout.geometry_records = geometry_records_version;
        upload_layout.dynamic_instances = dynamic_instances_version;
        upload_layout.static_instances = static_instances_version;
        upload_layout.static_refs = static_refs_version;
        upload_layout.environment_stream = environment_stream_version;
        for (const auto &upload : environment_geometry_uploads)
            upload_layout.geometry_streams.push_back(upload.version);
        }
        upload_pass->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            const auto initialize_lod = [&](GpuLodStateBuffer &state) {
                list->CopyBufferRegion(state.states.resource.Get(), 0, state.zero_upload.resource.Get(),
                    0, state.capacity * kGpuLodStateBytesPerSlot);
                if (state.prefix_source.resource)
                    list->CopyBufferRegion(state.states.resource.Get(), 0, state.prefix_source.resource.Get(),
                        0, state.prefix_bytes);
            };
            if (initialize_dynamic_lod) initialize_lod(dynamic_lod_state);
            if (initialize_static_lod) initialize_lod(*static_lod_state);
            if (has_geometry_draw_records)
                if (auto result = impl_->UploadGeometryDrawRecords(list, frame_slot); !result)
                    throw std::runtime_error(std::string(result.Message()));
            if (upload_static_scene)
            {
                const auto bytes = impl_->static_gpu_scene->layout.total_bytes;
                if (bytes != 0)
                    list->CopyBufferRegion(
                        impl_->static_gpu_scene->instances.resource.Get(), 0,
                        impl_->static_gpu_scene->upload.resource.Get(), 0, bytes);
            }
            if (upload_static_refs)
            {
                const auto &refs = *impl_->static_gpu_scene->refs;
                list->CopyBufferRegion(refs.storage.resource.Get(), 0,
                    refs.upload.resource.Get(), 0,
                    std::max<std::size_t>(sizeof(GpuStaticDrawRef),
                        refs.cpu->size() * sizeof(GpuStaticDrawRef)));
            }
            for (const auto &region : dynamic_scene_copy_regions)
            {
                list->CopyBufferRegion(
                    dynamic_scene.resource.Get(),
                    region.destination_offset,
                    frame.upload.resource.Get(),
                    kInstanceDataOffset + region.staging_offset,
                    region.byte_count);
            }
            if (upload_environment_stream)
            {
                for (UINT subresource = 0;
                     subresource < impl_->environment_stream.subresource_count;
                     ++subresource)
                {
                    D3D12_TEXTURE_COPY_LOCATION destination{};
                    destination.pResource =
                        impl_->environment_stream.candidate.resource.Get();
                    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    destination.SubresourceIndex = subresource;
                    D3D12_TEXTURE_COPY_LOCATION source{};
                    source.pResource = impl_->environment_stream.upload.resource.Get();
                    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    source.PlacedFootprint =
                        impl_->environment_stream.footprints[subresource];
                    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                }
            }
            if (upload_environment_geometry)
                for (const auto &upload : environment_geometry_uploads)
                {
                    const auto &stream = *upload.stream;
                    impl_->RecordGeometryPageUpload(list, stream.candidate, stream.upload);
                }
        });
    }
    std::optional<PassBuilder> particle_pass;
    if (restored_graph)
    {
        particle_pass = impl_->graph.BindPass(pending_graph_layout.particle.pass_index, 1);
        if (!particle_pass)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Particle pass does not match the retained graph plan.");
    }
    else
    {
        pending_graph_layout.particle.pass_index = impl_->graph.PassCount();
        particle_pass = impl_->graph.AddPass(1, kRenderPassNames[1], QueueHint::Compute);
    }
    std::array<ResourceVersion *, 9> particle_outputs{
        &particles_version, &particle_alive_input_version, &particle_alive_output_version,
        &particle_dead_version, &particle_counters_version, &indirect_arguments_version,
        &ribbon_history_version, &ribbon_previous_version, &ribbon_arguments_version};
    for (std::size_t index = 0; index < particle_outputs.size(); ++index)
    {
        if (restored_graph) *particle_outputs[index] = pending_graph_layout.particle.outputs[index];
        else
        {
            *particle_outputs[index] = particle_pass->ReadWrite(*particle_outputs[index], Access::UnorderedWrite);
            pending_graph_layout.particle.outputs[index] = *particle_outputs[index];
        }
    }
    const auto initialize_particles = !impl_->particles_initialized;
    particle_pass->SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->particle_compute_pipeline.Get());
        list->SetComputeRootShaderResourceView(16, impl_->vfx_mesh_atlas.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            3, impl_->particles.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            4, impl_->indirect_arguments.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            7, impl_->particle_alive[particle_input_index].resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            8, impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            9, impl_->particle_dead.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            10, impl_->particle_counters.resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(
            11, frame.upload.resource->GetGPUVirtualAddress() + particle_spawn_data_offset);
        list->SetComputeRootShaderResourceView(
            14, frame.upload.resource->GetGPUVirtualAddress() + particle_owner_data_offset);

        const auto dispatch_phase = [&](std::uint32_t phase, std::uint32_t item_count) {
            list->SetComputeRoot32BitConstant(6, phase, 0);
            list->Dispatch((std::max(item_count, 1u) + 255) / 256, 1, 1);
        };
        const auto synchronize_particle_state = [&] {
            context.UavBarrier(particles);
            context.UavBarrier(particle_alive_output);
            context.UavBarrier(particle_dead);
            context.UavBarrier(particle_counters);
        };

        if (impl_->render_scene_inputs)
        {
            if (initialize_particles)
            {
                dispatch_phase(0, particle_capacity);
                context.UavBarrier(particle_dead);
                context.UavBarrier(particle_counters);
            }
            dispatch_phase(1, 1);
            context.UavBarrier(particle_counters);
            dispatch_phase(2, particle_capacity);
            synchronize_particle_state();
            if (total_particles_to_spawn != 0)
            {
                dispatch_phase(3, total_particles_to_spawn);
                synchronize_particle_state();
            }
            dispatch_phase(5, 1);
            context.UavBarrier(particle_counters);
            context.UavBarrier(indirect_arguments);
        }
        list->SetComputeRootShaderResourceView(17, frame.upload.resource->GetGPUVirtualAddress() + ribbon_update_offset);
        list->SetComputeRootShaderResourceView(18, frame.upload.resource->GetGPUVirtualAddress() + ribbon_output_offset);
        list->SetComputeRootUnorderedAccessView(21, impl_->ribbon_history.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(22, impl_->ribbon_previous.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(23, frame.ribbon_arguments.resource->GetGPUVirtualAddress());
        // Ribbon history is package world-space state and was updated by the
        // first accepted view. RibbonArgsCS below remains per-view because it
        // culls the same history against this view's projection.
        if (impl_->render_scene_inputs && !ribbon_updates.empty())
        {
            list->SetPipelineState(impl_->ribbon_update_pipeline.Get());
            list->Dispatch(static_cast<UINT>(ribbon_updates.size()), 1, 1);
            context.UavBarrier(ribbon_history);
            context.UavBarrier(ribbon_previous);
        }
        if (!ribbon_outputs.empty())
        {
            list->SetPipelineState(impl_->ribbon_args_pipeline.Get());
            list->Dispatch(static_cast<UINT>(ribbon_outputs.size()), 1, 1);
            context.UavBarrier(ribbon_arguments);
        }
    });

    struct SkinDispatch
    {
        UINT64 source, palette, output, bounds;
        UINT vertex_count;
        std::array<UINT, 2> constants;
        bool reduce_bounds;
    };
    std::vector<SkinDispatch> skin_dispatches;
    if (have_skinned_instances && !reuse_canonical_skin)
    {
        skin_dispatches.reserve(owned_view.skin_history.ActiveCount());
        for (std::size_t index = 0; index < render_instances.size(); ++index)
        {
            const auto slot = dynamic_skin_slots[index];
            if (slot == std::numeric_limits<std::uint32_t>::max()) continue;
            const auto mesh = render_instances[index].mesh;
            const auto archer = mesh == RenderMesh::Archer;
            const auto asset = static_cast<std::size_t>(static_cast<std::uint32_t>(mesh) -
                static_cast<std::uint32_t>(RenderMesh::MonsterMelee));
            if (!archer && asset >= impl_->monster_assets.size())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Skinned instance has no monster geometry asset.");
            if (index >= ordered_monster_lods.size() || ordered_monster_lods[index] >= 3)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Skinned instance LOD order is invalid.");
            const auto lod = ordered_monster_lods[index];
            const GeometryPageGpu &geometry = archer ? impl_->archer_geometry
                : lod == 0 ? static_cast<const GeometryPageGpu &>(impl_->monster_assets[asset])
                : impl_->monster_reduced_geometry[asset][lod - 1u];
            const auto vertex_count = dynamic_skin_vertex_counts[index];
            if (!geometry.storage.resource || vertex_count == 0 || vertex_count != geometry.vertex_count ||
                dynamic_skin_geometry_ids[index] !=
                    ((std::uint64_t(geometry.generation) << 32u) | geometry.page_slot))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Skinned instance has no complete geometry source.");
            const auto clip = (dynamic_pose_flags[index] >> 8u) & 0x0fu;
            const auto palette = archer
                ? frame.upload.resource->GetGPUVirtualAddress() + archer_palette_data_offset +
                    static_cast<UINT64>(archer_palette_slots[index]) * archer_palette_stride
                : impl_->monster_skin_matrices.resource->GetGPUVirtualAddress();
            skin_dispatches.push_back({geometry.storage.resource->GetGPUVirtualAddress(), palette,
                static_cast<UINT64>(dynamic_skin_offsets[index]) * SkinVertexRanges::kVertexBytes,
                static_cast<UINT64>(slot) * kSkinBoundStride, vertex_count,
                {static_cast<UINT>(static_cast<std::uint32_t>(mesh) & 0xffu) |
                    static_cast<UINT>(clip << 8u) | static_cast<UINT>(vertex_count << 12u),
                    std::bit_cast<UINT>(dynamic_pose_times[index])},
                dynamic_cooked_skin_bounds[index] == 0});
        }
        auto &layout = pending_graph_layout.skin;
        std::optional<PassBuilder> skin_cache_pass;
        if (restored_graph)
        {
            skin_cache_pass = impl_->graph.BindPass(layout.pass_index, 22);
            if (!skin_cache_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Skin cache does not match the retained graph plan.");
            skin_cache_version = layout.first;
            skin_bounds_version = layout.second;
        }
        else
        {
            layout.pass_index = impl_->graph.PassCount();
            skin_cache_pass = impl_->graph.AddPass(22, kRenderPassNames[22], QueueHint::Compute);
            read_geometry_pages(*skin_cache_pass, true);
            skin_cache_version = skin_cache_pass->Write(skin_cache_version, Access::UnorderedWrite);
            skin_bounds_version = skin_cache_pass->Write(skin_bounds_version, Access::UnorderedWrite);
            layout.first = skin_cache_version;
            layout.second = skin_bounds_version;
        }
        skin_cache_pass->SetExecute([&](RenderPassContext &context) {
            auto *list =
                static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->skin_cache_pipeline.Get());
            for (const auto &dispatch : skin_dispatches)
            {
                list->SetComputeRootShaderResourceView(16, dispatch.source);
                list->SetComputeRootShaderResourceView(17, dispatch.palette);
                list->SetComputeRootUnorderedAccessView(
                    3, frame.skin_cache_vertices.resource->GetGPUVirtualAddress() +
                           dispatch.output);
                list->SetComputeRoot32BitConstants(
                    6, static_cast<UINT>(dispatch.constants.size()), dispatch.constants.data(), 0);
                list->Dispatch((dispatch.vertex_count + 63u) / 64u, 1, 1);
            }
            // All current deformation must be visible before the per-instance
            // reduction reads it back through the same UAV root binding.
            context.UavBarrier(skin_cache_buffer);
            list->SetPipelineState(impl_->skin_bounds_pipeline.Get());
            for (const auto &dispatch : skin_dispatches)
            {
                if (!dispatch.reduce_bounds) continue;
                list->SetComputeRootUnorderedAccessView(
                    3, frame.skin_cache_vertices.resource->GetGPUVirtualAddress() +
                           dispatch.output);
                list->SetComputeRootUnorderedAccessView(
                    21, frame.skin_cache_bounds.resource->GetGPUVirtualAddress() +
                            dispatch.bounds);
                list->SetComputeRoot32BitConstants(
                    6, static_cast<UINT>(dispatch.constants.size()),
                    dispatch.constants.data(), 0);
                list->Dispatch(1, 1, 1);
            }
            context.UavBarrier(skin_bounds_buffer);
        });
    }

    const auto bind_scene_deformation_roots = [&](ID3D12GraphicsCommandList *list,
        bool cached_skin_enabled, bool history_enabled) {
        // Scene and ribbon shaders share these root slots. Bind the complete
        // scene contract after a signature change or the last ribbon draw.
        // Static-only draws keep the cache branches disabled and use the
        // resident undeformed vertex source instead of unrelated upload data.
        const bool cached_skin = cached_skin_enabled && have_skinned_instances;
        const auto current_vertices_base = cached_skin
            ? frame.skin_cache_vertices.resource->GetGPUVirtualAddress()
            : impl_->archer_geometry.storage.resource->GetGPUVirtualAddress();
        const auto previous_vertices_base = cached_skin && history_enabled && previous_skin_frame_valid
            ? previous_skin_frame->skin_cache_vertices.resource->GetGPUVirtualAddress()
            : current_vertices_base;
        list->SetGraphicsRootShaderResourceView(18, previous_vertices_base);
        list->SetGraphicsRootDescriptorTable(19, scene_skin_table);
        list->SetGraphicsRootShaderResourceView(20, current_vertices_base);
    };
    const auto draw_character_instances = [&](ID3D12GraphicsCommandList *list, bool shadow_draw, std::uint32_t cascade = 0) {
        const auto base = dynamic_instance_base;
        auto *generic_pipeline = shadow_draw ? impl_->shadow_pipeline.Get()
            : (use_visibility ? impl_->visibility_pipeline.Get() : impl_->scene_pipeline.Get());
        auto *hero_pipeline = shadow_draw ? impl_->hero_shadow_pipeline.Get()
            : (use_visibility ? impl_->hero_visibility_pipeline.Get() : impl_->hero_scene_pipeline.Get());
        list->SetPipelineState(generic_pipeline);
        const auto bind_dynamic_draws = [&](std::size_t begin) {
            list->SetGraphicsRootShaderResourceView(34, base);
            list->SetGraphicsRootShaderResourceView(
                35, frame.upload.resource->GetGPUVirtualAddress() +
                        dynamic_ref_data_offset + begin * sizeof(GpuStaticDrawRef));
            bind_scene_deformation_roots(list, true, !shadow_draw);
            list->SetGraphicsRoot32BitConstant(
                6, kDefaultSceneDrawMaterial | 1u, 1);
        };
        bind_dynamic_draws(0);
        const auto set_environment_material = [&](std::size_t asset, UINT static_mode) {
            list->SetGraphicsRoot32BitConstant(
                6, (impl_->material_parameter_bases[render_data::EnvironmentGeometryMaterialOrdinal(static_cast<std::uint32_t>(asset / 3))] << 1) | static_mode, 1);
        };
        const auto visibility_view = shadow_draw ? cascade + 1u : 0u;
        const auto bind_visible_draws = [&] {
            bind_dynamic_draws(0);
            list->SetGraphicsRootShaderResourceView(
                35, frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
        };
        const auto draw_bucket = [&](std::size_t bucket, bool static_draw,
                                      ID3D12PipelineState *pipeline) {
            const auto &passes = static_draw ? static_geometry_passes : dynamic_geometry_passes;
            auto *arguments = static_draw ? frame.static_draw_args.resource.Get()
                                          : frame.dynamic_draw_args.resource.Get();
            if (auto result = impl_->DrawGeometry(list, frame_slot, pipeline,
                    *bucket_pages[bucket], passes[bucket][visibility_view], arguments,
                    (visibility_view * kGpuVisibilityBucketCount + bucket) *
                        kGeometryArgumentStride); !result)
                throw std::runtime_error(std::string(result.Message()));
        };
        if (archer_range.second != 0)
        {
            bind_visible_draws();
            draw_bucket(kArcherVisibilityBucket, false, hero_pipeline);
        }
        for (std::size_t asset = 0; asset < monster_bucket_ranges.size(); ++asset)
        {
            const auto [begin, count] = monster_bucket_ranges[asset];
            if (count == 0) continue;
            bind_visible_draws();
            draw_bucket(kMonsterVisibilityBucketBegin + asset, false, hero_pipeline);
        }
        for (std::size_t asset = 0; asset < environment_ranges.size(); ++asset)
        {
            const auto [begin, count] = environment_ranges[asset];
            if (count == 0 || asset / 3 == render_data::kMeadowWaterGeometry) continue;
            set_environment_material(asset, 1);
            list->SetGraphicsRootShaderResourceView(34, base);
            list->SetGraphicsRootShaderResourceView(
                35, frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
            draw_bucket(asset, false, environment_pipeline(asset, shadow_draw));
        }
        list->SetPipelineState(generic_pipeline);
        if (!static_draw_refs.empty())
        {
            const auto ref_base = static_ref_resource->GetGPUVirtualAddress();
            list->SetGraphicsRootShaderResourceView(
                34, impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress());
            list->SetGraphicsRoot32BitConstant(6, kDefaultSceneDrawMaterial | 1u, 1);
            for (std::size_t asset = 0; asset < static_environment_ranges.size(); ++asset)
            {
                const auto [begin, count] = static_environment_ranges[asset];
                if (count == 0 || visibility_ranges[asset].count == 0 || asset / 3 == render_data::kMeadowWaterGeometry) continue;
                set_environment_material(asset, 1);
                list->SetGraphicsRootShaderResourceView(
                    35, frame.static_visible_refs.resource->GetGPUVirtualAddress());
                draw_bucket(asset, true, environment_pipeline(asset, shadow_draw));
            }
            list->SetPipelineState(generic_pipeline);
            if (static_ground_range.second != 0)
            {
                list->SetGraphicsRoot32BitConstant(
                    6, kDefaultSceneDrawMaterial | 1u, 1);
                list->SetGraphicsRootShaderResourceView(
                    35, ref_base);
                list->SetGraphicsRoot32BitConstant(6, terrain_draw_material, 1);
                auto *ground_pipeline = shadow_draw ? generic_pipeline : terrain_pipeline;
                if (auto result = impl_->DrawGeometry(list, frame_slot, ground_pipeline,
                        impl_->procedural_geometry, ground_geometry_passes[visibility_view]); !result)
                    throw std::runtime_error(std::string(result.Message()));
            }
        }
        if (dynamic_terrain_count != 0)
        {
            bind_dynamic_draws(0);
            list->SetGraphicsRoot32BitConstant(6, terrain_draw_material, 1);
            auto *ground_pipeline = shadow_draw ? generic_pipeline : terrain_pipeline;
            if (auto result = impl_->DrawGeometry(list, frame_slot, ground_pipeline,
                    impl_->procedural_geometry, dynamic_terrain_geometry_passes[visibility_view]); !result)
                throw std::runtime_error(std::string(result.Message()));
        }
        if (procedural_count != 0)
        {
            bind_dynamic_draws(0);
            if (auto result = impl_->DrawGeometry(list, frame_slot, generic_pipeline,
                    impl_->procedural_geometry, procedural_geometry_passes[visibility_view]); !result)
                throw std::runtime_error(std::string(result.Message()));
        }
    };
    const auto bind_scene_roots = [&](ID3D12GraphicsCommandList *list,
        bool cached_skin_enabled = true, bool history_enabled = true) {
        bind_scene_deformation_roots(list, cached_skin_enabled, history_enabled);
        list->SetGraphicsRootShaderResourceView(
            34, static_scene && impl_->static_gpu_scene
                    ? impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress()
                    : dynamic_instance_base);
        list->SetGraphicsRootShaderResourceView(
            35, static_ref_resource->GetGPUVirtualAddress());
    };
    HlodFrameGraph hlod_graph;
    struct PendingHlodFrame
    {
        Impl &renderer;
        FrameContext &frame;
        HlodFrameGraph &state;
        bool completed{};
        ~PendingHlodFrame()
        {
            if (completed || !state.registered || !renderer.hlod_runtime) return;
            CompletionSet completion;
            for (const auto version : {state.outputs.leaf_masks, state.outputs.proxy_selection,
                state.outputs.indexed_commands, state.outputs.mesh_commands, state.outputs.command_counts,
                state.outputs.page_demand, state.outputs.status})
                completion.Merge(renderer.graph.ResourceCompletion(version.resource));
            completion.Merge(renderer.graph.ResourceCompletion(state.refs));
            completion.Merge(renderer.graph.ResourceCompletion(state.readback));
            frame.completion.Merge(completion);
            (void)state.view->system.CompleteSubmission(renderer.graph, completion, false);
            renderer.frame_submission_faulted = true;
        }
    } pending_hlod{*impl_, frame, hlod_graph};
    if (!restored_graph)
    {
        pending_graph_layout.hlod_input_geometry_records = geometry_records_version;
        pending_graph_layout.hlod_input_static_instances = static_instances_version;
    }
    if (auto prepared = impl_->AddHlodFramePasses(frame_slot,
        active_view_mask, geometry_records_version,
        static_instances_version, static_refs_version, hlod_graph,
        restored_graph ? nullptr : &pending_graph_layout.hlod, &hlod_record,
        restored_graph ? &pending_graph_layout.hlod : nullptr); !prepared) return prepared;
    auto &visibility_layout = pending_graph_layout.visibility;
    std::optional<PassBuilder> visibility_pass;
    if (restored_graph)
    {
        visibility_pass = impl_->graph.BindPass(visibility_layout.pass_index, 3);
        if (!visibility_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Visibility does not match the retained graph plan.");
        dynamic_lod_version = visibility_layout.outputs[0];
        static_lod_version = visibility_layout.outputs[1];
        geometry_records_version = visibility_layout.outputs[2];
        static_visible_refs_version = visibility_layout.outputs[3];
        static_draw_args_version = visibility_layout.outputs[4];
        dynamic_visible_refs_version = visibility_layout.outputs[5];
        dynamic_draw_args_version = visibility_layout.outputs[6];
        static_rejected_refs_version = visibility_layout.outputs[7];
        static_rejected_args_version = visibility_layout.outputs[8];
    }
    else
    {
        visibility_layout.pass_index = impl_->graph.PassCount();
        visibility_pass = impl_->graph.AddPass(3, kRenderPassNames[3], QueueHint::Compute);
        impl_->ReadHlodFrame(*visibility_pass, hlod_graph, false);
        dynamic_lod_version = visibility_pass->ReadWrite(dynamic_lod_version, Access::UnorderedWrite);
        if (static_lod_state)
            static_lod_version = visibility_pass->ReadWrite(static_lod_version, Access::UnorderedWrite);
        geometry_records_version = visibility_pass->ReadWrite(geometry_records_version, Access::UnorderedWrite);
        visibility_pass->Read(dynamic_instances_version, Access::ComputeRead);
        if (have_skinned_instances)
            visibility_pass->Read(skin_bounds_version, Access::ComputeRead);
        if (static_instances_buffer)
            visibility_pass->Read(static_instances_version, Access::ComputeRead);
        if (static_refs_buffer)
            visibility_pass->Read(static_refs_version, Access::ComputeRead);
        visibility_pass->Read(previous_hzb_version, Access::ComputeRead);
        static_visible_refs_version = visibility_pass->Write(static_visible_refs_version, Access::UnorderedWrite);
        static_draw_args_version = visibility_pass->Write(static_draw_args_version, Access::UnorderedWrite);
        dynamic_visible_refs_version = visibility_pass->Write(dynamic_visible_refs_version, Access::UnorderedWrite);
        dynamic_draw_args_version = visibility_pass->Write(dynamic_draw_args_version, Access::UnorderedWrite);
        static_rejected_refs_version = visibility_pass->Write(static_rejected_refs_version, Access::UnorderedWrite);
        static_rejected_args_version = visibility_pass->Write(static_rejected_args_version, Access::UnorderedWrite);
        visibility_layout.outputs = {dynamic_lod_version, static_lod_version, geometry_records_version,
            static_visible_refs_version, static_draw_args_version, dynamic_visible_refs_version,
            dynamic_draw_args_version, static_rejected_refs_version, static_rejected_args_version};
    }
    visibility_pass->SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->static_visibility_pipeline.Get());
        list->SetComputeRootUnorderedAccessView(22,
            (static_lod_state ? static_lod_state->states : dynamic_lod_state.states).resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(21, frame.geometry_draw_records.resource->GetGPUVirtualAddress());
        const std::array<UINT, 2> visibility_constants{
            static_cast<UINT>(visible_ref_stride),
            std::bit_cast<UINT>(EncodeGrassDeformationRadius(
                grass_visibility_radius, owned_view.hzb_history_valid
                    ? OcclusionMode::Enabled : OcclusionMode::Disabled))};
        list->SetComputeRoot32BitConstants(
            6, static_cast<UINT>(visibility_constants.size()), visibility_constants.data(), 0);
        list->SetComputeRootShaderResourceView(
            16, frame.upload.resource->GetGPUVirtualAddress() + visibility_range_offset);
        list->SetComputeRootShaderResourceView(
            17, owned_view.targets.hzb_history[owned_view.hzb_read_index].resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(
            18, have_skinned_instances
                    ? frame.skin_cache_bounds.resource->GetGPUVirtualAddress()
                    : frame.upload.resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(
            34, static_scene && impl_->static_gpu_scene
                ? impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress()
                : dynamic_instance_base);
        list->SetComputeRootShaderResourceView(
            35, static_ref_resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            3, frame.static_visible_refs.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            4, frame.static_draw_args.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            7, frame.static_rejected_refs.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            8, frame.static_rejected_args.resource->GetGPUVirtualAddress());
        list->Dispatch(kGpuVisibilityBucketCount, active_view_count, 1);
        if (dynamic_visibility_ref_count != 0)
        {
            list->SetComputeRootUnorderedAccessView(22, dynamic_lod_state.states.resource->GetGPUVirtualAddress());
            const std::array<UINT, 2> dynamic_constants{
                static_cast<UINT>(dynamic_visible_ref_stride),
                std::bit_cast<UINT>(EncodeGrassDeformationRadius(
                    grass_visibility_radius, OcclusionMode::Disabled))};
            list->SetComputeRoot32BitConstants(
                6, static_cast<UINT>(dynamic_constants.size()),
                dynamic_constants.data(), 0);
            list->SetComputeRootShaderResourceView(
                16, frame.upload.resource->GetGPUVirtualAddress() +
                        dynamic_visibility_range_offset);
            list->SetComputeRootShaderResourceView(
                34, dynamic_instance_base);
            list->SetComputeRootShaderResourceView(
                35, frame.upload.resource->GetGPUVirtualAddress() +
                        dynamic_ref_data_offset);
            list->SetComputeRootUnorderedAccessView(
                3, frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(
                4, frame.dynamic_draw_args.resource->GetGPUVirtualAddress());
            list->Dispatch(
                kGpuVisibilityBucketCount, active_view_count, 1);
        }
    });

    ClusterFrameGraph cluster_graph;
    if (auto prepared = impl_->AddClusterFramePasses(frame_slot, geometry_records_version,
            use_visibility ? &raster_records_version : nullptr, static_instances_version, cluster_graph,
            cluster_record, restored_graph ? nullptr : &pending_graph_layout.cluster,
            restored_graph ? &pending_graph_layout.cluster : nullptr); !prepared)
        return prepared;
    if (auto prepared = impl_->AddClusterOcclusionPass(owned_view, frame_slot, cluster_graph,
            previous_hzb_version, false, restored_graph ? nullptr : &pending_graph_layout.cluster,
            restored_graph ? &pending_graph_layout.cluster : nullptr); !prepared) return prepared;
    // Preserve the original fallback input versions while delaying its declaration
    // until the CPU virtual-shadow plan can prove that CSM is unnecessary.
    std::vector<VirtualShadowNativeRead> csm_reads;
    csm_reads.reserve(32 + cluster_graph.geometry.size() + kClusterRuntimeCatalogs*2);
    const auto csm_read = [&](ResourceVersion version, Access access) { csm_reads.push_back({version, access}); };
    const auto collect_csm_reads = [&] {
        csm_reads.clear();
        if (cluster_graph.prepared)
        {
            if (cluster_graph.draws) csm_read(cluster_graph.refs.version, Access::ShaderRead);
            csm_read(cluster_graph.pages.version, Access::ComputeRead);
            for (const auto &page : cluster_graph.geometry)
                if (page.resident) csm_read(page.buffer.version, Access::VertexShaderRead);
            for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
            {
                const auto &cut = cluster_graph.cuts[ordinal];
                if (!cut.active) continue;
                const bool mesh = impl_->cluster_runtime->frames[cluster_graph.frame_slot].cuts[ordinal].geometry_frontend_flags == kGeometryMeshEligible;
                csm_read(mesh ? cut.mesh_commands.version : cut.draw_commands.version, Access::IndirectArgs);
                csm_read(cut.command_counts.version, Access::IndirectArgs);
            }
        }
        if (hlod_graph.registered)
        {
            csm_read(hlod_graph.outputs.leaf_masks, Access::ShaderRead);
            csm_read(hlod_graph.outputs.proxy_selection, Access::ShaderRead);
            csm_read(hlod_graph.outputs.indexed_commands, Access::IndirectArgs);
            csm_read(hlod_graph.outputs.mesh_commands, Access::IndirectArgs);
            csm_read(hlod_graph.outputs.command_counts, Access::IndirectArgs);
        }
        csm_read(geometry_records_version, Access::ShaderRead);
        csm_read(material_parameter_version, Access::ShaderRead);
        csm_read(hero_binding_version, Access::ShaderRead);
        csm_read(dynamic_instances_version, Access::ShaderRead);
        if (have_skinned_instances) csm_read(skin_cache_version, Access::ShaderRead);
        if (static_instances_buffer) csm_read(static_instances_version, Access::ShaderRead);
        if (static_refs_buffer) csm_read(static_refs_version, Access::ShaderRead);
        csm_read(static_visible_refs_version, Access::ShaderRead);
        csm_read(static_draw_args_version, Access::IndirectArgs);
        csm_read(dynamic_visible_refs_version, Access::ShaderRead);
        csm_read(dynamic_draw_args_version, Access::IndirectArgs);
    };
    bool csm_registered = false;
    const auto register_cascaded_shadow = [&]() -> Result {
        if (!restored_graph) collect_csm_reads();
        const auto shadow_initial = impl_->enhanced && owned_view.targets.shadow.graph_access == Access::Undefined
            ? Access::Common : initial_depth_access;
        shadow = import_texture(owned_view.targets.shadow, shadow_initial, "Shadow");
        shadow_version = impl_->graph.Version(shadow);
        csm_registered = true;
        auto &layout = pending_graph_layout.cascaded_shadow;
        std::optional<PassBuilder> shadow_pass;
        if (restored_graph)
        {
            shadow_pass = impl_->graph.BindPass(layout.pass_index, 2);
            if (!shadow_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Cascaded shadow does not match the retained graph plan.");
            shadow_version = layout.output;
        }
        else
        {
            layout.pass_index = impl_->graph.PassCount();
            shadow_pass = impl_->graph.AddPass(2, kRenderPassNames[2], QueueHint::Direct);
            read_material_sampling(*shadow_pass, has_masked_material_draw, MaterialSampling::Opacity);
            read_geometry_pages(*shadow_pass);
            for (const auto &read : csm_reads) shadow_pass->Read(read.version, read.access);
            shadow_version = shadow_pass->Write(shadow_version, Access::DepthWrite);
            layout.output = shadow_version;
        }
        shadow_pass->SetExecuteChecked([&](RenderPassContext &context) -> Result {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_graphics(list);
            bind_scene_roots(list, true, false);
            const auto shadow_size =
                static_cast<float>(std::clamp(impl_->config.shadow_resolution, 1024u, 2048u));
            const D3D12_VIEWPORT shadow_viewport{0, 0, shadow_size, shadow_size, 0, 1};
            const D3D12_RECT shadow_scissor{0, 0, static_cast<LONG>(shadow_size),
                                            static_cast<LONG>(shadow_size)};
            list->RSSetViewports(1, &shadow_viewport);
            list->RSSetScissorRects(1, &shadow_scissor);
            list->SetPipelineState(impl_->shadow_pipeline.Get());
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            auto handle = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(frame_slot * 5) * impl_->dsv_stride;
            handle.ptr += impl_->dsv_stride;
            list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            for (std::uint32_t cascade = 0; cascade < 3; ++cascade)
            {
                list->OMSetRenderTargets(0, nullptr, FALSE, &handle);
                const UINT tile = static_cast<UINT>(shadow_size);
                const UINT x = cascade == 1 ? tile : 0u;
                const UINT y = cascade == 2 ? tile : 0u;
                const D3D12_VIEWPORT tile_view{static_cast<float>(x), static_cast<float>(y), static_cast<float>(tile), static_cast<float>(tile), 0, 1};
                const D3D12_RECT tile_rect{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + tile), static_cast<LONG>(y + tile)};
                list->RSSetViewports(1, &tile_view );
                list->RSSetScissorRects(1, &tile_rect);
                list->SetGraphicsRoot32BitConstant(6, cascade, 0);
                draw_character_instances(list,true,cascade);
                if (auto drawn = impl_->DrawClusterGeometry(list, frame_slot, cascade + 1); !drawn)
                    return drawn;
                if (auto drawn = impl_->DrawHlodGeometry(list, frame_slot, cascade + 1); !drawn) return drawn;
                handle.ptr += impl_->dsv_stride;
            }
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            return Result::Success();
        });
        return Result::Success();
    };

    if (use_visibility)
    {
        // D3D12 render-target allocations must be initialized by an API discard,
        // clear or copy before shader writes. Keep this Direct operation in the
        // graph so Compute clear waits for initialization after create/resize.
        if (!owned_view.visibility_targets_initialized)
        {
            auto &layout = pending_graph_layout.visibility_discard;
            std::optional<PassBuilder> initialize;
            if (restored_graph)
            {
                initialize = impl_->graph.BindPass(layout.pass_index, 31);
                if (!initialize || !restore_surface_versions(layout))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                        "Visibility discard does not match the retained graph plan.");
            }
            else
            {
                layout.pass_index = impl_->graph.PassCount();
                initialize = impl_->graph.AddPass(31, "VisibilityDiscard", QueueHint::Direct);
                visibility_ids_version = initialize->Write(visibility_ids_version, Access::RenderTarget);
                visibility_face_version = initialize->Write(visibility_face_version, Access::RenderTarget);
                gbuffer_base_version = initialize->Write(gbuffer_base_version, Access::RenderTarget);
                gbuffer_normal_version = initialize->Write(gbuffer_normal_version, Access::RenderTarget);
                gbuffer_metadata_version = initialize->Write(gbuffer_metadata_version, Access::RenderTarget);
                gbuffer_material_version = initialize->Write(gbuffer_material_version, Access::RenderTarget);
                gbuffer_motion_version = initialize->Write(gbuffer_motion_version, Access::RenderTarget);
                gbuffer_surface_history_version = initialize->Write(gbuffer_surface_history_version, Access::RenderTarget);
                for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
                    material_sidecar_versions[plane] = initialize->Write(material_sidecar_versions[plane], Access::RenderTarget);
                capture_surface_versions(layout);
            }
            initialize->SetExecute([&](RenderPassContext &context) {
                auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                for (auto *resource : {owned_view.targets.visibility_ids.resource.Get(),
                        owned_view.targets.visibility_front_face.resource.Get(), owned_view.targets.gbuffer_base.resource.Get(),
                        owned_view.targets.gbuffer_normal.resource.Get(), owned_view.targets.gbuffer_metadata.resource.Get(),
                        owned_view.targets.gbuffer_material.resource.Get(), owned_view.targets.gbuffer_motion.resource.Get(),
                        owned_view.targets.gbuffer_surface_history.resource.Get()})
                    list->DiscardResource(resource, nullptr);
                for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
                    list->DiscardResource(owned_view.targets.material_sidecars[plane].resource.Get(), nullptr);
            });
        }
        auto &layout = pending_graph_layout.visibility_clear;
        std::optional<PassBuilder> clear;
        if (restored_graph)
        {
            clear = impl_->graph.BindPass(layout.pass_index, 25);
            if (!clear || !restore_surface_versions(layout))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Visibility clear does not match the retained graph plan.");
        }
        else
        {
            layout.pass_index = impl_->graph.PassCount();
            clear = impl_->graph.AddPass(25, "VisibilityClear", QueueHint::Compute);
            visibility_ids_version = clear->Write(visibility_ids_version, Access::UnorderedWrite);
            visibility_face_version = clear->Write(visibility_face_version, Access::UnorderedWrite);
            visibility_counts_version = clear->Write(visibility_counts_version, Access::UnorderedWrite);
            gbuffer_base_version = clear->Write(gbuffer_base_version, Access::UnorderedWrite);
            gbuffer_normal_version = clear->Write(gbuffer_normal_version, Access::UnorderedWrite);
            gbuffer_metadata_version = clear->Write(gbuffer_metadata_version, Access::UnorderedWrite);
            gbuffer_material_version = clear->Write(gbuffer_material_version, Access::UnorderedWrite);
            gbuffer_motion_version = clear->Write(gbuffer_motion_version, Access::UnorderedWrite);
            gbuffer_surface_history_version = clear->Write(gbuffer_surface_history_version, Access::UnorderedWrite);
            for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
                material_sidecar_versions[plane] = clear->Write(material_sidecar_versions[plane], Access::UnorderedWrite);
            capture_surface_versions(layout);
        }
        clear->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->visibility_clear_pipeline.Get());
            list->Dispatch((owned_view.render_width + 7) / 8, (owned_view.render_height + 7) / 8, 1);
        });
    }
    auto &gbuffer_layout = pending_graph_layout.gbuffer;
    std::optional<PassBuilder> gbuffer_pass;
    if (restored_graph)
    {
        gbuffer_pass = impl_->graph.BindPass(gbuffer_layout.pass_index, 4);
        if (!gbuffer_pass || !restore_surface_versions(gbuffer_layout))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "G-buffer does not match the retained graph plan.");
    }
    else
    {
        gbuffer_layout.pass_index = impl_->graph.PassCount();
        gbuffer_pass = impl_->graph.AddPass(4, kRenderPassNames[4], QueueHint::Direct);
        read_material_sampling(*gbuffer_pass, use_visibility ? has_masked_material_draw : has_authored_material_draw,
            use_visibility ? MaterialSampling::Opacity : MaterialSampling::Surface);
        impl_->ReadClusterFrame(*gbuffer_pass, cluster_graph, true);
        impl_->ReadHlodFrame(*gbuffer_pass, hlod_graph, true);
        gbuffer_pass->Read(geometry_records_version, Access::ShaderRead);
        gbuffer_pass->Read(material_parameter_version, Access::ShaderRead);
        gbuffer_pass->Read(hero_binding_version, Access::ShaderRead);
        read_geometry_pages(*gbuffer_pass);
        gbuffer_pass->Read(dynamic_instances_version, Access::ShaderRead);
        if (separate_previous_instance_scene)
            gbuffer_pass->Read(previous_instance_snapshot_version, Access::VertexShaderRead);
        if (have_skinned_instances)
            gbuffer_pass->Read(skin_cache_version, Access::ShaderRead);
        if (have_skinned_instances && previous_skin_frame_valid)
            gbuffer_pass->Read(previous_skin_cache_version, Access::ShaderRead);
        if (static_instances_buffer) gbuffer_pass->Read(static_instances_version, Access::ShaderRead);
        if (static_refs_buffer) gbuffer_pass->Read(static_refs_version, Access::ShaderRead);
        gbuffer_pass->Read(static_visible_refs_version, Access::ShaderRead);
        gbuffer_pass->Read(static_draw_args_version, Access::IndirectArgs);
        gbuffer_pass->Read(dynamic_visible_refs_version, Access::ShaderRead);
        gbuffer_pass->Read(dynamic_draw_args_version, Access::IndirectArgs);
        if (use_visibility)
        {
            visibility_ids_version = gbuffer_pass->ReadWrite(visibility_ids_version, Access::RenderTarget);
            visibility_face_version = gbuffer_pass->ReadWrite(visibility_face_version, Access::RenderTarget);
        }
        else
        {
            gbuffer_base_version = gbuffer_pass->Write(gbuffer_base_version, Access::RenderTarget);
            gbuffer_normal_version = gbuffer_pass->Write(gbuffer_normal_version, Access::RenderTarget);
            gbuffer_metadata_version = gbuffer_pass->Write(gbuffer_metadata_version, Access::RenderTarget);
            gbuffer_material_version = gbuffer_pass->Write(gbuffer_material_version, Access::RenderTarget);
            gbuffer_motion_version = gbuffer_pass->Write(gbuffer_motion_version, Access::RenderTarget);
            gbuffer_surface_history_version = gbuffer_pass->Write(gbuffer_surface_history_version, Access::RenderTarget);
            for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
                material_sidecar_versions[plane] = gbuffer_pass->Write(material_sidecar_versions[plane], Access::RenderTarget);
        }
        depth_version = gbuffer_pass->Write(depth_version, Access::DepthWrite);
        capture_surface_versions(gbuffer_layout);
    }
    gbuffer_pass->SetExecuteChecked([&](RenderPassContext &context) -> Result {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        bind_scene_roots(list);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        if (use_visibility)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{rtv_at(kVisibilityRtvOffset),
                rtv_at(kVisibilityRtvOffset + 1)};
            list->OMSetRenderTargets(2, targets, FALSE, &dsv);
            draw_character_instances(list, false);
            if (auto drawn = impl_->DrawClusterGeometry(list, frame_slot, 0); !drawn) return drawn;
            return impl_->DrawHlodGeometry(list, frame_slot, 0);
        }
        constexpr float clear_base[] = {0, 0, 0, 0};
        constexpr float clear_normal[] = {0, 0, 0, 0};
        constexpr float clear_metadata[] = {0, 0, 0, 0};
        list->ClearRenderTargetView(gbuffer_base_rtv, clear_base, 0, nullptr);
        list->ClearRenderTargetView(gbuffer_material_rtv, clear_base, 0, nullptr);
        list->ClearRenderTargetView(gbuffer_motion_rtv, clear_base, 0, nullptr);
        list->ClearRenderTargetView(gbuffer_surface_history_rtv, clear_base, 0, nullptr);
        for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
            list->ClearRenderTargetView(surface_rtvs[6 + plane], clear_base, 0, nullptr);
        list->ClearRenderTargetView(gbuffer_normal_rtv, clear_normal, 0, nullptr);
        list->ClearRenderTargetView(gbuffer_metadata_rtv, clear_metadata, 0,
                                                   nullptr);
        list->OMSetRenderTargets(surface_target_count, surface_rtvs.data(), FALSE, &dsv);
        list->SetPipelineState(impl_->scene_pipeline.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        draw_character_instances(list, false);
        if (auto drawn = impl_->DrawClusterGeometry(list, frame_slot, 0); !drawn) return drawn;
            return impl_->DrawHlodGeometry(list, frame_slot, 0);
    });

    const auto record_depth_pyramid = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetComputeRootUnorderedAccessView(
            3, owned_view.targets.hzb_history[hzb_write_index].resource->GetGPUVirtualAddress());
        const auto &base = owned_view.targets.hzb_layout.levels[0];
        std::array<UINT, 2> constants{0u, 0u};
        list->SetPipelineState(impl_->depth_pyramid_init_pipeline.Get());
        list->SetComputeRoot32BitConstants(
            6, static_cast<UINT>(constants.size()), constants.data(), 0);
        list->Dispatch((base.width + 7u) / 8u,
                                     (base.height + 7u) / 8u, 1);
        context.UavBarrier(current_hzb);
        list->SetPipelineState(impl_->depth_pyramid_reduce_pipeline.Get());
        for (std::uint32_t level = 1; level < owned_view.targets.hzb_layout.count; ++level)
        {
            const auto &destination = owned_view.targets.hzb_layout.levels[level];
            constants = {level, 0u};
            list->SetComputeRoot32BitConstants(
                6, static_cast<UINT>(constants.size()), constants.data(), 0);
            list->Dispatch((destination.width + 7u) / 8u,
                                         (destination.height + 7u) / 8u, 1);
            context.UavBarrier(current_hzb);
        }
    };

    auto &depth_layout = pending_graph_layout.depth_pyramid;
    std::optional<PassBuilder> depth_pyramid_pass;
    if (restored_graph)
    {
        depth_pyramid_pass = impl_->graph.BindPass(depth_layout.pass_index, 18);
        if (!depth_pyramid_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Depth pyramid does not match the retained graph plan.");
        current_hzb_version = depth_layout.output;
    }
    else
    {
        depth_layout.pass_index = impl_->graph.PassCount();
        depth_pyramid_pass = impl_->graph.AddPass(18, kRenderPassNames[18], QueueHint::Compute);
        depth_pyramid_pass->Read(depth_version, Access::ComputeRead);
        current_hzb_version = depth_pyramid_pass->Write(current_hzb_version, Access::UnorderedWrite);
        depth_layout.output = current_hzb_version;
    }
    depth_pyramid_pass->SetExecute(
        [&](RenderPassContext &context) { record_depth_pyramid(context); });

    if (auto prepared = impl_->AddClusterOcclusionPass(owned_view, frame_slot, cluster_graph,
            current_hzb_version, true, restored_graph ? nullptr : &pending_graph_layout.cluster,
            restored_graph ? &pending_graph_layout.cluster : nullptr); !prepared) return prepared;
    auto &retest_layout = pending_graph_layout.retest;
    std::optional<PassBuilder> retest_pass;
    if (restored_graph)
    {
        retest_pass = impl_->graph.BindPass(retest_layout.pass_index, 19);
        if (!retest_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Visibility retest does not match the retained graph plan.");
        geometry_records_version = retest_layout.records;
        static_retest_refs_version = retest_layout.refs;
        static_retest_args_version = retest_layout.args;
    }
    else
    {
        retest_layout.pass_index = impl_->graph.PassCount();
        retest_pass = impl_->graph.AddPass(19, kRenderPassNames[19], QueueHint::Compute);
        if (static_instances_buffer)
            retest_pass->Read(static_instances_version, Access::ComputeRead);
        if (static_refs_buffer)
            retest_pass->Read(static_refs_version, Access::ComputeRead);
        retest_pass->Read(static_rejected_refs_version, Access::ComputeRead);
        retest_pass->Read(static_rejected_args_version, Access::UnorderedRead);
        geometry_records_version = retest_pass->ReadWrite(geometry_records_version, Access::UnorderedWrite);
        retest_pass->Read(current_hzb_version, Access::ComputeRead);
        static_retest_refs_version = retest_pass->Write(static_retest_refs_version, Access::UnorderedWrite);
        static_retest_args_version = retest_pass->Write(static_retest_args_version, Access::UnorderedWrite);
        retest_layout.records = geometry_records_version;
        retest_layout.refs = static_retest_refs_version;
        retest_layout.args = static_retest_args_version;
    }
    retest_pass->SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->visibility_retest_pipeline.Get());
        list->SetComputeRootUnorderedAccessView(22,
            (static_lod_state ? static_lod_state->states : dynamic_lod_state.states).resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(21, frame.geometry_draw_records.resource->GetGPUVirtualAddress());
        const std::array<UINT, 2> visibility_constants{
            static_cast<UINT>(visible_ref_stride),
            std::bit_cast<UINT>(EncodeGrassDeformationRadius(
                grass_visibility_radius, OcclusionMode::Enabled))};
        list->SetComputeRoot32BitConstants(
            6, static_cast<UINT>(visibility_constants.size()),
            visibility_constants.data(), 0);
        list->SetComputeRootShaderResourceView(
            16, frame.upload.resource->GetGPUVirtualAddress() + retest_visibility_range_offset);
        list->SetComputeRootShaderResourceView(
            17, owned_view.targets.hzb_history[hzb_write_index].resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(
            34, static_scene && impl_->static_gpu_scene
                ? impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress()
                : dynamic_instance_base);
        list->SetComputeRootShaderResourceView(
            35, frame.static_rejected_refs.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            3, frame.static_retest_refs.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            4, frame.static_retest_args.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            8, frame.static_rejected_args.resource->GetGPUVirtualAddress());
        list->Dispatch(kGpuVisibilityBucketCount, 1, 1);
    });

    auto &gbuffer_retest_layout = pending_graph_layout.gbuffer_retest;
    std::optional<PassBuilder> gbuffer_retest_pass;
    if (restored_graph)
    {
        gbuffer_retest_pass = impl_->graph.BindPass(gbuffer_retest_layout.pass_index, 20);
        if (!gbuffer_retest_pass || !restore_surface_versions(gbuffer_retest_layout))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "G-buffer retest does not match the retained graph plan.");
    }
    else
    {
        gbuffer_retest_layout.pass_index = impl_->graph.PassCount();
        gbuffer_retest_pass = impl_->graph.AddPass(20, kRenderPassNames[20], QueueHint::Direct);
        read_material_sampling(*gbuffer_retest_pass, use_visibility ? has_masked_material_draw : has_authored_material_draw,
            use_visibility ? MaterialSampling::Opacity : MaterialSampling::Surface);
        impl_->ReadClusterFrame(*gbuffer_retest_pass, cluster_graph, true, true);
        impl_->ReadHlodFrame(*gbuffer_retest_pass, hlod_graph, false);
        gbuffer_retest_pass->Read(geometry_records_version, Access::ShaderRead);
        gbuffer_retest_pass->Read(material_parameter_version, Access::ShaderRead);
        read_geometry_pages(*gbuffer_retest_pass);
        if (static_instances_buffer)
            gbuffer_retest_pass->Read(static_instances_version, Access::ShaderRead);
        if (static_refs_buffer)
            gbuffer_retest_pass->Read(static_refs_version, Access::ShaderRead);
        gbuffer_retest_pass->Read(static_retest_refs_version, Access::ShaderRead);
        gbuffer_retest_pass->Read(static_retest_args_version, Access::IndirectArgs);
        if (use_visibility)
        {
            visibility_ids_version = gbuffer_retest_pass->ReadWrite(visibility_ids_version, Access::RenderTarget);
            visibility_face_version = gbuffer_retest_pass->ReadWrite(visibility_face_version, Access::RenderTarget);
        }
        else
        {
            gbuffer_base_version = gbuffer_retest_pass->ReadWrite(gbuffer_base_version, Access::RenderTarget);
            gbuffer_normal_version = gbuffer_retest_pass->ReadWrite(gbuffer_normal_version, Access::RenderTarget);
            gbuffer_metadata_version = gbuffer_retest_pass->ReadWrite(gbuffer_metadata_version, Access::RenderTarget);
            gbuffer_material_version = gbuffer_retest_pass->ReadWrite(gbuffer_material_version, Access::RenderTarget);
            gbuffer_motion_version = gbuffer_retest_pass->ReadWrite(gbuffer_motion_version, Access::RenderTarget);
            gbuffer_surface_history_version = gbuffer_retest_pass->ReadWrite(gbuffer_surface_history_version, Access::RenderTarget);
            for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
                material_sidecar_versions[plane] = gbuffer_retest_pass->ReadWrite(material_sidecar_versions[plane], Access::RenderTarget);
        }
        depth_version = gbuffer_retest_pass->ReadWrite(depth_version, Access::DepthWrite);
        capture_surface_versions(gbuffer_retest_layout);
    }
    gbuffer_retest_pass->SetExecuteChecked([&](RenderPassContext &context) -> Result {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        bind_scene_roots(list, false);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
            gbuffer_base_rtv, gbuffer_normal_rtv, gbuffer_metadata_rtv,
            gbuffer_material_rtv, gbuffer_motion_rtv, gbuffer_surface_history_rtv};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        if (use_visibility)
        {
            const D3D12_CPU_DESCRIPTOR_HANDLE visibility_targets[]{rtv_at(kVisibilityRtvOffset),
                rtv_at(kVisibilityRtvOffset + 1)};
            list->OMSetRenderTargets(2, visibility_targets, FALSE, &dsv);
        }
        else list->OMSetRenderTargets(surface_target_count, surface_rtvs.data(), FALSE, &dsv);
        if (auto drawn = impl_->DrawClusterGeometry(list, frame_slot, 0, true); !drawn) return drawn;
        if (!static_scene || !impl_->static_gpu_scene) return Result::Success();
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->SetGraphicsRootShaderResourceView(
            34, impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress());
        for (std::size_t asset = 0; asset < static_environment_ranges.size(); ++asset)
        {
            if (static_environment_ranges[asset].second == 0 ||
                retest_visibility_ranges[asset].count == 0) continue;
            list->SetGraphicsRoot32BitConstant(
                6, (impl_->material_parameter_bases[render_data::EnvironmentGeometryMaterialOrdinal(static_cast<std::uint32_t>(asset / 3))] << 1) | 1u, 1);
            list->SetGraphicsRootShaderResourceView(
                35, frame.static_retest_refs.resource->GetGPUVirtualAddress());
            if (auto result = impl_->DrawGeometry(list, frame_slot,
                    environment_pipeline(asset, false), *bucket_pages[asset],
                    retest_geometry_passes[asset], frame.static_retest_args.resource.Get(),
                    asset * kGeometryArgumentStride); !result)
                return result;
        }
        return Result::Success();
    });

    // Retest19/53 only changed main-view records/counts.
    auto &final_depth_layout = pending_graph_layout.final_depth_pyramid;
    std::optional<PassBuilder> final_depth_pyramid_pass;
    if (restored_graph)
    {
        final_depth_pyramid_pass = impl_->graph.BindPass(final_depth_layout.pass_index, 21);
        if (!final_depth_pyramid_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Final depth pyramid does not match the retained graph plan.");
        current_hzb_version = final_depth_layout.output;
    }
    else
    {
        final_depth_layout.pass_index = impl_->graph.PassCount();
        final_depth_pyramid_pass = impl_->graph.AddPass(21, kRenderPassNames[21], QueueHint::Compute);
        final_depth_pyramid_pass->Read(depth_version, Access::ComputeRead);
        current_hzb_version = final_depth_pyramid_pass->ReadWrite(current_hzb_version, Access::UnorderedWrite);
        final_depth_layout.output = current_hzb_version;
    }
    final_depth_pyramid_pass->SetExecute(
        [&](RenderPassContext &context) { record_depth_pyramid(context); });

    if (use_visibility)
    {
        auto &records_layout = pending_graph_layout.raster_records;
        std::optional<PassBuilder> records;
        if (restored_graph)
        {
            records = impl_->graph.BindPass(records_layout.pass_index, 26);
            if (!records) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Raster draw records do not match the retained graph plan.");
            raster_records_version = records_layout.output;
        }
        else
        {
            records_layout.pass_index = impl_->graph.PassCount();
            records = impl_->graph.AddPass(26, "RasterDrawRecords", QueueHint::Compute);
            records->Read(geometry_records_version, Access::ComputeRead);
            records->Read(static_visible_refs_version, Access::ComputeRead);
            records->Read(dynamic_visible_refs_version, Access::ComputeRead);
            records->Read(static_retest_refs_version, Access::ComputeRead);
            raster_records_version = records->ReadWrite(raster_records_version, Access::UnorderedWrite);
            records_layout.output = raster_records_version;
        }
        records->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->raster_records_pipeline.Get());
            for (std::uint32_t i = 0; i < visibility_frame.ranges_cpu.size(); ++i)
            {
                const auto groups = (visibility_frame.ranges_cpu[i].instance_capacity + 63u) / 64u;
                list->SetComputeRoot32BitConstant(6, i, 0);
                list->Dispatch(std::min(groups, 65535u), (groups + 65534u) / 65535u, 1);
            }
        });
        auto &count_layout = pending_graph_layout.material_count;
        std::optional<PassBuilder> count;
        if (restored_graph)
        {
            count = impl_->graph.BindPass(count_layout.pass_index, 27);
            if (!count) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Material count does not match the retained graph plan.");
            visibility_counts_version = count_layout.output;
        }
        else
        {
            count_layout.pass_index = impl_->graph.PassCount();
            count = impl_->graph.AddPass(27, "MaterialCount", QueueHint::Compute);
            count->Read(geometry_records_version, Access::ComputeRead);
            count->Read(hero_binding_version, Access::ComputeRead);
            for (const auto &page : geometry_pages) count->Read(page.version, Access::ComputeRead);
            count->Read(visibility_ids_version, Access::ComputeRead);
            count->Read(raster_records_version, Access::ComputeRead);
            visibility_counts_version = count->ReadWrite(visibility_counts_version, Access::UnorderedWrite);
            count_layout.output = visibility_counts_version;
        }
        count->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->visibility_count_pipeline.Get());
            list->Dispatch((owned_view.render_width + 7) / 8, (owned_view.render_height + 7) / 8, 1);
        });
        auto &prefix_layout = pending_graph_layout.material_prefix;
        std::optional<PassBuilder> prefix;
        if (restored_graph)
        {
            prefix = impl_->graph.BindPass(prefix_layout.pass_index, 28);
            if (!prefix) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Material prefix does not match the retained graph plan.");
            visibility_counts_version = prefix_layout.first;
            visibility_args_version = prefix_layout.second;
        }
        else
        {
            prefix_layout.pass_index = impl_->graph.PassCount();
            prefix = impl_->graph.AddPass(28, "MaterialPrefix", QueueHint::Compute);
            visibility_counts_version = prefix->ReadWrite(visibility_counts_version, Access::UnorderedWrite);
            visibility_args_version = prefix->Write(visibility_args_version, Access::UnorderedWrite);
            prefix_layout.first = visibility_counts_version;
            prefix_layout.second = visibility_args_version;
        }
        prefix->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->visibility_prefix_pipeline.Get());
            list->Dispatch(1, 1, 1);
        });
        auto &scatter_layout = pending_graph_layout.material_scatter;
        std::optional<PassBuilder> scatter;
        if (restored_graph)
        {
            scatter = impl_->graph.BindPass(scatter_layout.pass_index, 29);
            if (!scatter) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Material scatter does not match the retained graph plan.");
            visibility_counts_version = scatter_layout.first;
            visibility_pixels_version = scatter_layout.second;
        }
        else
        {
            scatter_layout.pass_index = impl_->graph.PassCount();
            scatter = impl_->graph.AddPass(29, "MaterialScatter", QueueHint::Compute);
            scatter->Read(geometry_records_version, Access::ComputeRead);
            scatter->Read(hero_binding_version, Access::ComputeRead);
            for (const auto &page : geometry_pages) scatter->Read(page.version, Access::ComputeRead);
            scatter->Read(visibility_ids_version, Access::ComputeRead);
            scatter->Read(raster_records_version, Access::ComputeRead);
            visibility_counts_version = scatter->ReadWrite(visibility_counts_version, Access::UnorderedWrite);
            visibility_pixels_version = scatter->Write(visibility_pixels_version, Access::UnorderedWrite);
            scatter_layout.first = visibility_counts_version;
            scatter_layout.second = visibility_pixels_version;
        }
        scatter->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->visibility_scatter_pipeline.Get());
            list->Dispatch((owned_view.render_width + 7) / 8, (owned_view.render_height + 7) / 8, 1);
        });
        auto &resolve_layout = pending_graph_layout.surface_resolve;
        std::optional<PassBuilder> resolve;
        if (restored_graph)
        {
            resolve = impl_->graph.BindPass(resolve_layout.pass_index, 30);
            if (!resolve || !restore_surface_versions(resolve_layout))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Surface resolve does not match the retained graph plan.");
        }
        else
        {
            resolve_layout.pass_index = impl_->graph.PassCount();
            resolve = impl_->graph.AddPass(30, "SurfaceResolve", QueueHint::Compute);
            read_material_sampling(*resolve, has_authored_material_draw, MaterialSampling::Surface, Access::ComputeRead);
            impl_->ReadClusterFrame(*resolve, cluster_graph, false);
            impl_->ReadHlodFrame(*resolve, hlod_graph, false);
            resolve->Read(visibility_ids_version, Access::ComputeRead);
            resolve->Read(visibility_face_version, Access::ComputeRead);
            resolve->Read(depth_version, Access::ComputeRead);
            resolve->Read(raster_records_version, Access::ComputeRead);
            resolve->Read(geometry_records_version, Access::ComputeRead);
            resolve->Read(material_parameter_version, Access::ComputeRead);
            resolve->Read(hero_binding_version, Access::ComputeRead);
            resolve->Read(visibility_pixels_version, Access::ComputeRead);
            resolve->Read(visibility_counts_version, Access::ComputeRead);
            resolve->Read(visibility_args_version, Access::IndirectArgs);
            resolve->Read(static_visible_refs_version, Access::ComputeRead);
            resolve->Read(dynamic_visible_refs_version, Access::ComputeRead);
            resolve->Read(static_retest_refs_version, Access::ComputeRead);
            resolve->Read(dynamic_instances_version, Access::ComputeRead);
            if (separate_previous_instance_scene)
                resolve->Read(previous_instance_snapshot_version, Access::ComputeRead);
            if (static_instances_buffer) resolve->Read(static_instances_version, Access::ComputeRead);
            if (have_skinned_instances) resolve->Read(skin_cache_version, Access::ComputeRead);
            if (have_skinned_instances && previous_skin_frame_valid)
                resolve->Read(previous_skin_cache_version, Access::ComputeRead);
            for (const auto &page : geometry_pages) resolve->Read(page.version, Access::ComputeRead);
            gbuffer_base_version = resolve->ReadWrite(gbuffer_base_version, Access::UnorderedWrite);
            gbuffer_normal_version = resolve->ReadWrite(gbuffer_normal_version, Access::UnorderedWrite);
            gbuffer_metadata_version = resolve->ReadWrite(gbuffer_metadata_version, Access::UnorderedWrite);
            gbuffer_material_version = resolve->ReadWrite(gbuffer_material_version, Access::UnorderedWrite);
            gbuffer_motion_version = resolve->ReadWrite(gbuffer_motion_version, Access::UnorderedWrite);
            gbuffer_surface_history_version = resolve->ReadWrite(gbuffer_surface_history_version, Access::UnorderedWrite);
            for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
                material_sidecar_versions[plane] = resolve->ReadWrite(material_sidecar_versions[plane], Access::UnorderedWrite);
            capture_surface_versions(resolve_layout);
        }
        resolve->SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_surface_compute(list);
            list->SetComputeRootShaderResourceView(34, dynamic_instance_base);
            for (std::uint32_t variant = 0; variant < kVisibilityVariantCount; ++variant)
            {
                list->SetComputeRoot32BitConstant(6, variant, 0);
                list->SetPipelineState(variant == 0 ? impl_->surface_resolve_pipeline.Get()
                    : variant <= kAuthoredMaterialNames.size()
                        ? impl_->environment_material_resolve_pipelines[variant - 1].Get()
                        : impl_->hero_resolve_pipeline.Get());
                list->ExecuteIndirect(impl_->dispatch_signature.Get(), 1,
                    owned_view.targets.visibility_dispatch_args.resource.Get(), variant * 12u, nullptr, 0);
            }
        });
    }
    // Every successful feature registration needs a completion, including early
    // returns during graph construction/preparation and partial submissions.
    struct PendingFrameEffects
    {
        Impl &renderer;
        Impl::OwnedView &view;
        FrameContext &frame;
        CompletionSet completion;
        bool ambient{}, reconstruction{}, diffusion{}, reflections{}, transparency{}, probe{}, ltc{}, hdr_post{}, lights{}, scene_geometry{}, shadows{};
        bool local_probes{}, volumetrics{}, presentation{}, local_shadows{}, brixelizer{}, screen_overlay{}, adaptive{}, decal_surface{};
        std::shared_ptr<const StaticRenderScene> probe_scene;
        render_data::SceneProbeRevisions probe_revisions;
        ~PendingFrameEffects()
        {
            if (!ambient && !reconstruction && !diffusion && !reflections && !transparency && !probe && !ltc && !hdr_post && !lights && !scene_geometry && !shadows && !local_probes && !volumetrics && !presentation && !local_shadows && !brixelizer && !screen_overlay && !adaptive && !decal_surface) return;
            frame.completion.Merge(completion);
            if (decal_surface)
                (void)view.decal_surface.CompleteSubmission(renderer.graph, completion, false);
            if (adaptive)
                (void)view.adaptive_shading.CompleteSubmission(renderer.graph, completion, false);
            if (ambient)
                (void)view.ambient_occlusion.CompleteSubmission(renderer.graph, completion, false);
            if (reconstruction)
                (void)view.reconstruction_guides.CompleteSubmission(renderer.graph, completion, false);
            if (presentation)
                (void)renderer.presentation_guides.CompleteSubmission(renderer.graph, completion, false);
            if (diffusion)
                (void)view.subsurface.CompleteSubmission(renderer.graph, completion, false);
            if (reflections)
                (void)view.screen_reflections.CompleteSubmission(renderer.graph, completion, false);
            if (transparency)
                (void)view.hero_transparency.CompleteSubmission(renderer.graph, completion, false);
            if (probe)
                (void)renderer.lighting_probe.CompleteSubmission(renderer.graph, completion, false);
            if (ltc)
                (void)renderer.ltc_lut.CompleteSubmission(renderer.graph, completion, false);
            if (hdr_post)
                (void)view.hdr_post.CompleteSubmission(renderer.graph, completion, false);
            if (screen_overlay)
                (void)view.screen_overlay.CompleteSubmission(renderer.graph, completion, false);
            if (lights)
                (void)view.light_list.CompleteSubmission(renderer.graph, completion, false);
            if (scene_geometry)
                (void)renderer.scene_geometry.CompleteSubmission(renderer.graph, completion, false);
            if (brixelizer)
            {
                (void)view.brixelizer_gi.CompleteSubmission(renderer.graph, completion, false);
                view.brixelizer_history_valid = false;
            }
            if (shadows)
                (void)view.virtual_shadows.CompleteSubmission(renderer.graph, completion, false);
            if (local_shadows)
                (void)view.local_shadows.CompleteSubmission(renderer.graph, completion, false);

            if (local_probes)
                (void)renderer.local_probes.CompleteSubmission(renderer.graph, completion, false, probe_scene, probe_revisions);
            if (volumetrics)
                (void)view.volumetrics.CompleteSubmission(renderer.graph, completion, false);
            view.temporal_history_valid = false;
            view.hzb_history_valid = false;
            // Aborted service targets cannot be reused; SDK recording may also
            // have mutated its private context before GPU submission.
            renderer.frame_submission_faulted = true;
        }
    } pending_effects{*impl_, owned_view, frame};
    auto *surface_base_resource = owned_view.targets.gbuffer_base.resource.Get();
    auto *surface_normal_resource = owned_view.targets.gbuffer_normal.resource.Get();
    auto *surface_material_resource = owned_view.targets.gbuffer_material.resource.Get();
    DecalSurfaceOutputs decal_surface_outputs;
    if (constants->vfx_decal_count.x)
    {
        DecalSurfaceInputs inputs;
        inputs.depth = {depth_version, owned_view.targets.depth.resource.Get()};
        inputs.metadata = {gbuffer_metadata_version, owned_view.targets.gbuffer_metadata.resource.Get()};
        inputs.base = {gbuffer_base_version, surface_base_resource};
        inputs.normal = {gbuffer_normal_version, surface_normal_resource};
        inputs.material = {gbuffer_material_version, surface_material_resource};
        inputs.commands = {impl_->graph.Version(import_buffer(frame.upload,
            Access::AllShaderRead, "DecalFrameCommands")), frame.upload.resource.Get()};
        inputs.parameters = {material_parameter_version, impl_->material_parameters.resource.Get()};
        inputs.gradient = {impl_->graph.Version(import_texture(impl_->vfx_gradient,
            Access::AllShaderRead, "DecalGradient")), impl_->vfx_gradient.resource.Get()};
        std::array<AllocationResource *, 2> textures{&impl_->vfx_fracture_decals, &impl_->vfx_organic_decals};
        for (std::size_t i = 0; i < textures.size(); ++i)
        {
            inputs.textures[i] = {impl_->graph.Version(import_texture(*textures[i],
                Access::AllShaderRead, i == 0 ? "DecalFractureArray" : "DecalOrganicArray")), textures[i]->resource.Get()};
            inputs.materials[i] = {impl_->decal_material_programs[i], impl_->decal_parameter_bases[i],
                impl_->decal_parameter_counts[i]};
        }
        inputs.command_byte_offset = decal_data_offset;
        inputs.command_count = constants->vfx_decal_count.x;
        inputs.parameter_slot_count = impl_->material_parameter_slot_count;
        inputs.render_active = owned_view.state.render_active;
        inputs.frame_slot = frame_slot;
        inputs.completed = impl_->graph_queue_executor.Completed();
        inputs.descriptor_suffix_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        inputs.descriptor_suffix_cpu.ptr += (SIZE_T(frame_slot) * kTextureDescriptorCount +
            kDecalSurfaceDescriptorOffset) * impl_->srv_stride;
        inputs.main_table_gpu = texture_table;
        inputs.main_table_gpu.ptr += UINT64(kTemporalTextureDescriptorOffset) * impl_->srv_stride;
        inputs.main_heap = impl_->srv_heap.Get();
        DecalSurfaceConstants camera;
        camera.inverse_view_projection = constants->inverse_view_projection;
        camera.view_origin = constants->surface_view_origin;
        pending_effects.decal_surface = true;
        const auto registered = restored_graph
            ? owned_view.decal_surface.BindRetained(impl_->graph, inputs, camera,
                pending_graph_layout.decal_surface, decal_surface_outputs)
            : owned_view.decal_surface.Register(impl_->graph, inputs, camera,
                decal_surface_outputs, &pending_graph_layout.decal_surface);
        if (!registered) return registered;
        gbuffer_base_version = decal_surface_outputs.base.version;
        gbuffer_normal_version = decal_surface_outputs.normal.version;
        gbuffer_material_version = decal_surface_outputs.material.version;
        surface_base_resource = decal_surface_outputs.base.resource;
        surface_normal_resource = decal_surface_outputs.normal.resource;
        surface_material_resource = decal_surface_outputs.material.resource;
    }
    // Every waited bank publishes the current Surface packet. Empty frames
    // restore original inputs instead of retaining an earlier decal export.
    const auto surface_descriptor = [&](UINT offset, ID3D12Resource *resource, DXGI_FORMAT format) {
        auto cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T(frame_slot) * kTextureDescriptorCount + offset) * impl_->srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        impl_->device->CreateShaderResourceView(resource, &srv, cpu);
    };
    for (const auto offset : {0u, kToneMapInputDescriptorOffset, kFxaaInputDescriptorOffset})
    {
        surface_descriptor(offset, surface_base_resource, DXGI_FORMAT_R8G8B8A8_UNORM);
        surface_descriptor(offset + 1u, surface_normal_resource, DXGI_FORMAT_R16G16_SNORM);
    }
    surface_descriptor(kPostTextureDescriptorCount + kCharacterDescriptorCount +
        kMonsterPbrDescriptorCount + kEnvironmentDescriptorCount - 2u,
        surface_material_resource, DXGI_FORMAT_R8G8B8A8_UNORM);
    surface_descriptor(kDecalSurfaceDescriptorOffset + kDecalSurfaceExportOffset + 3u,
        decal_surface_outputs.active ? decal_surface_outputs.emission.resource : nullptr,
        DXGI_FORMAT_R16G16B16A16_FLOAT);
    record_graph_substage(cpu_timings.graph_base_passes_us);
    if ((primary_sun && owned_view.virtual_shadows.AllocatedBytes() != 0) || owned_view.local_shadows.AllocatedBytes() != 0)
    {
        reads.push_back({geometry_records_version, Access::ShaderRead});
        reads.push_back({dynamic_instances_version, Access::ShaderRead});
        reads.push_back({material_parameter_version, Access::ShaderRead});
        reads.push_back({hero_binding_version, Access::ShaderRead});
        reads.push_back({impl_->graph.Version(import_buffer(frame.upload, Access::ShaderRead, "ShadowFrameInputs")), Access::ShaderRead});
        if (static_instances_buffer) reads.push_back({static_instances_version, Access::ShaderRead});
        if (static_refs_buffer) reads.push_back({static_refs_version, Access::ShaderRead});
        if (have_skinned_instances) reads.push_back({skin_cache_version, Access::ShaderRead});
        for (const auto &entry : geometry_pages)
        {
            reads.push_back({entry.version, Access::VertexShaderRead});
        }
        inputs.declare_material_sampling = [&](PassBuilder &pass) {
            read_material_sampling(pass, has_masked_material_draw, MaterialSampling::Opacity);
        };
        inputs.depth = depth_version; inputs.normal = gbuffer_normal_version;
        inputs.depth_resource = owned_view.targets.depth.resource.Get(); inputs.normal_resource = surface_normal_resource;
        inputs.native_reads = reads;
        const auto draw_shadows = [&, bindings](RenderPassContext &context, const auto &outputs,
                std::span<const VirtualShadowDrawBatch> batches) -> Result {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_graphics(list);
            auto extent=outputs.bank_extent;
            if constexpr(requires { outputs.root_extent; })
                if(!batches.empty()&&batches.front().layer==2)extent=outputs.root_extent;
            const D3D12_VIEWPORT bank_view{0,0,float(extent.width),float(extent.height),0,1};
            const D3D12_RECT bank_rect{0,0,LONG(extent.width),LONG(extent.height)};
            list->RSSetViewports(1, &bank_view); list->RSSetScissorRects(1, &bank_rect);
            bind_scene_deformation_roots(list, true, false);
            for (const auto &batch : batches)
            {
                const auto &binding = bindings[batch.group_index];
                list->SetGraphicsRootShaderResourceView(34, binding.immutable
                    ? impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress() : dynamic_instance_base);
                list->SetGraphicsRootShaderResourceView(35, binding.immutable
                    ? static_ref_resource->GetGPUVirtualAddress()
                    : frame.upload.resource->GetGPUVirtualAddress() + dynamic_ref_data_offset);
                list->SetGraphicsRoot32BitConstant(6, binding.material, 1);
                const auto &pipelines = batch.group.frontend == VirtualShadowFrontend::Mesh
                    ? impl_->virtual_shadow_mesh_pipelines : impl_->virtual_shadow_indexed_pipelines;
                const auto pipeline = pipelines.find(binding.pipeline);
                if (pipeline == pipelines.end()) return Result::Failure(ErrorCode::InvalidState,
                    "hs_renderer_d3d12", "Virtual draw has no zero-bias native shadow pipeline.");
                list->SetPipelineState(pipeline->second.Get());
                list->OMSetRenderTargets(0, nullptr, FALSE, &outputs.depth_views[batch.layer]);
                list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->IASetVertexBuffers(0, 1, &binding.page->vertex_view);
                list->IASetIndexBuffer(&binding.page->index_view);
                list->ExecuteIndirect(batch.group.frontend == VirtualShadowFrontend::Mesh
                    ? impl_->cluster_mesh_signature.Get() : impl_->indexed_geometry_signature.Get(), batch.maximum_commands,
                    batch.group.frontend == VirtualShadowFrontend::Mesh ? outputs.mesh_arguments : outputs.indexed_arguments,
                    batch.argument_offset, batch.indexed_instanced ? nullptr : outputs.counts_resource,
                    batch.indexed_instanced ? 0 : batch.count_offset);
            }
            return Result::Success();
        };
        inputs.draw = draw_shadows;
        if (primary_sun && owned_view.virtual_shadows.AllocatedBytes() != 0)
        {
            if (csm_required)
                if (auto registered = register_cascaded_shadow(); !registered) return registered;
            pending_effects.shadows = true;
            const auto registered_virtual_shadows = restored_graph
                ? owned_view.virtual_shadows.BindRetained(impl_->graph, inputs, shadow_plan,
                    pending_graph_layout.virtual_shadows, virtual_shadow_outputs)
                : owned_view.virtual_shadows.Register(impl_->graph, inputs, shadow_plan,
                    virtual_shadow_outputs, &pending_graph_layout.virtual_shadows);
            if (!registered_virtual_shadows) return registered_virtual_shadows;
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount + kVirtualShadowDescriptorOffset) * impl_->srv_stride;
            for (const auto srv : virtual_shadow_outputs.srvs)
            {
                impl_->device->CopyDescriptorsSimple(1, destination, srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                destination.ptr += impl_->srv_stride;
            }
        }
        if (owned_view.local_shadows.AllocatedBytes() != 0)
        {
            local_shadow_input.declare_material_sampling = inputs.declare_material_sampling;
            local_shadow_input.native_reads = reads;
            local_shadow_input.draw = draw_shadows;
            pending_effects.local_shadows = true;
            const auto registered_local_shadows = restored_graph
                ? owned_view.local_shadows.BindRetained(impl_->graph, local_shadow_input, local_shadow_plan,
                    pending_graph_layout.local_shadows, local_shadow_outputs)
                : owned_view.local_shadows.Register(impl_->graph, local_shadow_input, local_shadow_plan,
                    local_shadow_outputs, &pending_graph_layout.local_shadows);
            if (!registered_local_shadows) return registered_local_shadows;
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount + kLocalShadowDescriptorOffset) * impl_->srv_stride;
            for (const auto srv : local_shadow_outputs.srvs)
            {
                impl_->device->CopyDescriptorsSimple(1, destination, srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                destination.ptr += impl_->srv_stride;
            }
        }
    }
    if (csm_required && !csm_registered)
        if (auto registered = register_cascaded_shadow(); !registered) return registered;
    // t5 is post-table slot3. Bind a typed null when there is no current CSM;
    // even speculative shader sampling cannot touch a stale/unwritten atlas.
    for (const auto offset : {0u,kToneMapInputDescriptorOffset,kFxaaInputDescriptorOffset})
        surface_descriptor(offset+3u, csm_required ? owned_view.targets.shadow.resource.Get() : nullptr, DXGI_FORMAT_R32_FLOAT);
    record_graph_substage(cpu_timings.graph_shadows_us);
    LightingProbeOutputs probe_outputs;
    const auto registered_probe = restored_graph
        ? impl_->lighting_probe.BindRetained(impl_->graph,
            impl_->graph_queue_executor.Completed(), pending_graph_layout.probe, probe_outputs)
        : impl_->lighting_probe.Register(impl_->graph,
            impl_->graph_queue_executor.Completed(), probe_outputs, &pending_graph_layout.probe);
    if (!registered_probe) return registered_probe;
    pending_effects.probe = true;
    auto probe_descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    probe_descriptor.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
        kProbeDescriptorOffset) * impl_->srv_stride;
    for (const auto source : probe_outputs.srvs)
    {
        impl_->device->CopyDescriptorsSimple(1, probe_descriptor, source,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        probe_descriptor.ptr += impl_->srv_stride;
    }
    LightListInputs light_inputs;
    light_inputs.lights = frame_lights;
    light_inputs.local_shadow_records = local_shadow_outputs.light_records;
    light_inputs.transient_light_ids = transient_light_ids;
    light_inputs.render_active = owned_view.state.render_active;
    light_inputs.frame_slot = frame_slot;
    light_inputs.completed = impl_->graph_queue_executor.Completed();
    DirectX::XMStoreFloat4x4(&light_inputs.unjittered_view, view);
    DirectX::XMStoreFloat4x4(&light_inputs.unjittered_projection, projection);
    light_inputs.camera_origin = lod_eye;
    light_inputs.near_plane = near_plane;
    light_inputs.far_plane = far_plane;
    LightListOutputs light_outputs;
    const auto registered_lights = restored_graph
        ? owned_view.light_list.BindRetained(impl_->graph, light_inputs,
            pending_graph_layout.light_list, light_outputs)
        : owned_view.light_list.Register(impl_->graph, light_inputs, light_outputs,
            &pending_graph_layout.light_list);
    if (!registered_lights) return registered_lights;
    pending_effects.lights = true;
    auto light_descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    light_descriptor.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
        kLightListDescriptorOffset) * impl_->srv_stride;
    for (const auto source : light_outputs.srvs)
    {
        impl_->device->CopyDescriptorsSimple(1, light_descriptor, source,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        light_descriptor.ptr += impl_->srv_stride;
    }
    LtcLutOutputs ltc_outputs;
    const auto registered_ltc = restored_graph
        ? impl_->ltc_lut.BindRetained(impl_->graph, impl_->graph_queue_executor.Completed(),
            pending_graph_layout.ltc, ltc_outputs)
        : impl_->ltc_lut.Register(impl_->graph, impl_->graph_queue_executor.Completed(),
            ltc_outputs, &pending_graph_layout.ltc);
    if (!registered_ltc) return registered_ltc;
    pending_effects.ltc = static_cast<bool>(ltc_outputs.matrix.resource);
    for (const auto source : ltc_outputs.srvs)
    {
        impl_->device->CopyDescriptorsSimple(1, light_descriptor, source,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        light_descriptor.ptr += impl_->srv_stride;
    }
    if (!restored_graph) capture_reads.clear();
    local_probe_inputs.config = &impl_->config;
    local_probe_inputs.pipeline_cache = &impl_->pipeline_cache;
    local_probe_inputs.scene = static_scene;
    local_probe_inputs.lights = std::span<const LightView>(frame_lights.data(), authoritative_light_count);
    local_probe_inputs.static_instance_generation = impl_->static_gpu_scene ? impl_->static_gpu_scene->instance_generation : 0;
    local_probe_inputs.material_parameter_revision = impl_->material_parameter_revision;
    local_probe_inputs.pipeline_revision = impl_->pipeline_revision;
    local_probe_inputs.foliage_density = foliage_density;
    local_probe_inputs.frame_constants = constants;
    if (has_static_authored_material_draw)
        local_probe_inputs.declare_material_sampling = [&](PassBuilder &pass) { read_material_sampling(pass, true); };
    if (has_static_masked_material_draw)
        local_probe_inputs.declare_shadow_sampling = [&](PassBuilder &pass) {
            read_material_sampling(pass, true, MaterialSampling::Opacity);
        };
    local_probe_inputs.frame_srv_heap = impl_->srv_cpu_heap;
    local_probe_inputs.sampler_heap = impl_->sampler_heap;
    local_probe_inputs.frame_srv_base = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    local_probe_inputs.frame_srv_base.ptr += static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount * impl_->srv_stride;
    local_probe_inputs.sampler_table = sampler_table;
    local_probe_inputs.completed = impl_->graph_queue_executor.Completed();
    local_probe_inputs.bind_graphics = [&](ID3D12GraphicsCommandList *list) { bind_graphics(list); bind_scene_roots(list, false); };
    local_probe_inputs.restore_graphics = [&](ID3D12GraphicsCommandList *list) {
        bind_graphics(list); bind_scene_roots(list, false);
        list->OMSetRenderTargets(surface_target_count, surface_rtvs.data(), FALSE, &dsv);
    };
    if (!local_probe_prepared && local_probe_revision_preflight)
        if (auto decided = impl_->local_probes.PrepareFrameDecisionData(
                local_probe_inputs, local_probe_plan); !decided) return decided;
    if ((!local_probe_prepared || local_probe_plan.capture) &&
        (!local_probe_revision_preflight || local_probe_plan.capture) &&
        impl_->local_probes.AllocatedBytes() != 0 && static_scene && impl_->static_gpu_scene)
    {
        if (!capture_pages) capture_pages.emplace();
        local_probe_inputs.static_instances = impl_->static_gpu_scene->instances.resource;
        local_probe_inputs.static_allocation = impl_->static_gpu_scene->instances.allocation;
        if (!restored_graph) local_probe_inputs.static_version = static_instances_version;
        local_probe_inputs.binding_owners.push_back(impl_->static_gpu_scene);
        struct CaptureNativeLease { ComPtr<D3D12MA::Allocation> allocation; ComPtr<ID3D12Resource> resource; };
        const auto capture_read = [&](AllocationResource &resource, bool texture, std::string_view name,
                                      ResourceVersion produced = {}) {
            if (!resource.resource) return;
            const auto version = produced.resource ? produced : impl_->graph.Version(texture
                ? import_texture(resource, Access::AllShaderRead, name)
                : import_buffer(resource, Access::AllShaderRead, name));
            if (!restored_graph && std::ranges::none_of(capture_reads, [&](const auto &read) {
                    return read.version.resource.value == version.resource.value &&
                           read.version.resource.kind == version.resource.kind;
                }))
                capture_reads.push_back({version, Access::AllShaderRead});
            local_probe_inputs.binding_owners.push_back(std::make_shared<CaptureNativeLease>(CaptureNativeLease{resource.allocation, resource.resource}));
        };
        capture_read(impl_->static_gpu_scene->refs->storage, false, "ProbeStaticDrawRefs", static_refs_version);
        for (std::size_t material = 0; material < capture_pages->size(); ++material)
        {
            const auto &page = material == kEnvironmentMaterialNames.size() ? impl_->procedural_geometry : *bucket_pages[material * 3];
            auto &capture = (*capture_pages)[material];
            capture.vertices = page.storage.resource; capture.indices = page.storage.resource;
            capture.vertex_allocation = page.storage.allocation; capture.index_allocation = page.storage.allocation;
            if (!restored_graph)
                capture.vertex_version = impl_->graph.Version(import_buffer(
                    const_cast<AllocationResource &>(page.storage), Access::VertexShaderRead, "ProbeCompleteGeometry"));
            capture.index_version = capture.vertex_version;
            capture.vertex_view = page.vertex_view; capture.index_view = page.index_view;
            capture.index_count = page.index_count; capture.generation = page.generation; capture.page_slot = page.page_slot;
            capture.source_digest = page.source_digest;
            capture.bounds = page.metadata.bounds;
            capture.complete = page.storage.resource && page.index_count != 0;
        }
        capture_read(frame.upload, false, "ProbeMainBindings");
        capture_read(impl_->material_parameters, false, "ProbeMaterialParameters", material_parameter_version);
        capture_read(impl_->hero_material_bindings_gpu, false, "ProbeHeroBindings", hero_binding_version);
        capture_read(impl_->environment_brdf_lut, true, "ProbeBrdfLut");
        if (!restored_graph)
        {
        for (const auto version : {probe_outputs.radiance, probe_outputs.prefiltered, probe_outputs.irradiance,
                light_outputs.lights, light_outputs.ranges, light_outputs.inline_indices, light_outputs.spill_indices,
                ltc_outputs.matrix, ltc_outputs.amplitude}) capture_reads.push_back({version, Access::AllShaderRead});
        if (virtual_shadow_outputs.resources[0])
            for (const auto version : {virtual_shadow_outputs.metadata, virtual_shadow_outputs.page_table,
                    virtual_shadow_outputs.static_bank, virtual_shadow_outputs.dynamic_bank, virtual_shadow_outputs.draw_records})
                capture_reads.push_back({version, Access::AllShaderRead});
        if(virtual_shadow_outputs.root_static.resource)
            capture_reads.push_back({virtual_shadow_outputs.root_static,Access::AllShaderRead});
        if(virtual_shadow_outputs.root_dynamic.resource)
            capture_reads.push_back({virtual_shadow_outputs.root_dynamic,Access::AllShaderRead});
        if (local_shadow_outputs.resources[0])
            for (const auto version : {local_shadow_outputs.metadata, local_shadow_outputs.records,
                    local_shadow_outputs.static_bank, local_shadow_outputs.dynamic_bank, local_shadow_outputs.draw_records})
                capture_reads.push_back({version, Access::AllShaderRead});
        }
        local_probe_inputs.pages = *capture_pages;
        local_probe_inputs.scene_reads = capture_reads;
    }
    LocalProbeOutputs local_probe_outputs;
    pending_effects.probe_scene = static_scene;
    pending_effects.local_probes = impl_->local_probes.AllocatedBytes() != 0;
    if (auto prepared = impl_->local_probes.PrepareFrameRecordData(
            local_probe_inputs, local_probe_plan); !prepared) return prepared;
    const auto registered_local_probe = restored_graph
        ? impl_->local_probes.BindRetained(impl_->graph, local_probe_inputs,
            local_probe_plan, pending_graph_layout.local_probe, local_probe_outputs)
        : impl_->local_probes.Register(impl_->graph, local_probe_inputs,
            local_probe_plan, local_probe_outputs, &pending_graph_layout.local_probe);
    if (!registered_local_probe) return registered_local_probe;
    pending_effects.probe_revisions = local_probe_outputs.revisions;
    if (local_probe_outputs.srvs[0].ptr)
    {
        auto destination = local_probe_inputs.frame_srv_base;
        destination.ptr += kLocalProbeDescriptorOffset * impl_->srv_stride;
        for (const auto descriptor : local_probe_outputs.srvs)
        {
            impl_->device->CopyDescriptorsSimple(1, destination, descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            destination.ptr += impl_->srv_stride;
        }
    }
    const auto read_probe = [&](PassBuilder &pass) {
        if (local_shadow_outputs.resources[0])
            for (const auto version : {local_shadow_outputs.metadata, local_shadow_outputs.records,
                    local_shadow_outputs.static_bank, local_shadow_outputs.dynamic_bank, local_shadow_outputs.draw_records})
                pass.Read(version, Access::AllShaderRead);
        if (local_probe_outputs.record_resource)
            for (const auto version : {local_probe_outputs.radiance, local_probe_outputs.prefiltered,
                    local_probe_outputs.irradiance, local_probe_outputs.first_hit_distance, local_probe_outputs.records})
                if (version.resource) pass.Read(version, Access::AllShaderRead);
        if (virtual_shadow_outputs.resources[0])
            for (const auto version : {virtual_shadow_outputs.metadata, virtual_shadow_outputs.page_table,
                    virtual_shadow_outputs.static_bank, virtual_shadow_outputs.dynamic_bank, virtual_shadow_outputs.draw_records})
                pass.Read(version, Access::ShaderRead);
        if(virtual_shadow_outputs.root_static.resource)pass.Read(virtual_shadow_outputs.root_static,Access::ShaderRead);
        if(virtual_shadow_outputs.root_dynamic.resource)pass.Read(virtual_shadow_outputs.root_dynamic,Access::ShaderRead);
        pass.Read(probe_outputs.radiance, Access::ShaderRead);
        pass.Read(probe_outputs.prefiltered, Access::ShaderRead);
        pass.Read(probe_outputs.irradiance, Access::ShaderRead);
        pass.Read(light_outputs.lights, Access::ShaderRead);
        pass.Read(light_outputs.ranges, Access::ShaderRead);
        pass.Read(light_outputs.inline_indices, Access::ShaderRead);
        pass.Read(light_outputs.spill_indices, Access::ShaderRead);
        pass.Read(ltc_outputs.matrix, Access::ShaderRead);
        pass.Read(ltc_outputs.amplitude, Access::ShaderRead);
    };
    record_graph_substage(cpu_timings.graph_light_probes_us);
    const auto decode_feedback = restored_graph
        ? impl_->texture_feedback.BindRetainedDecode(impl_->graph, frame_slot, 1201,
            pending_graph_layout.texture_feedback, feedback_bindings)
        : impl_->texture_feedback.AddDecodePass(impl_->graph, frame_slot,
            1201, feedback_bindings, &pending_graph_layout.texture_feedback);
    if (!decode_feedback) return decode_feedback;
    SceneGeometryOutputs scene_geometry_outputs;
    std::vector<SceneShaderRead> scene_reads;
    const auto make_scene_bindings = [&](std::uint32_t table_offset) -> SceneFrameTableBuilder {
        return [&, table_offset](D3D12_CPU_DESCRIPTOR_HANDLE suffix, std::uint32_t count,
                SceneShaderBinder &binder) -> Result {
            if (count != kSceneGeometryDescriptorCount || !suffix.ptr)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid scene geometry descriptor suffix.");
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
                table_offset) * impl_->srv_stride;
            auto source = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            source.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
                kTemporalTextureDescriptorOffset) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kSceneFrameTableCount, destination, source,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto scene_destination = destination;
            scene_destination.ptr += static_cast<SIZE_T>(kSceneGeometryDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kSceneGeometryDescriptorCount, scene_destination, suffix,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            auto table = texture_table;
            table.ptr += static_cast<UINT64>(table_offset) * impl_->srv_stride;
            binder = [&, table](ID3D12GraphicsCommandList *list) -> Result {
                bind_surface_compute(list);
                list->SetComputeRootDescriptorTable(32, table);
                list->SetComputeRootDescriptorTable(26, distortion_table);
                list->SetComputeRootShaderResourceView(34, dynamic_instance_base);
                return Result::Success();
            };
            return Result::Success();
        };
    };
    if (active_features.software_gi_enabled)
    {
        const auto add_read = [&](ResourceVersion version, ID3D12Resource *resource) {
            if (resource && std::ranges::none_of(scene_reads, [&](const auto &read) { return read.resource == resource; }))
                scene_reads.push_back({version, resource, Access::ComputeRead});
        };
        add_read(material_parameter_version, impl_->material_parameters.resource.Get());
        add_read(hero_binding_version, impl_->hero_material_bindings_gpu.resource.Get());
        add_read(impl_->graph.Version(import_buffer(frame.upload, Access::ComputeRead, "SceneFrameConstants")),
            frame.upload.resource.Get());
        add_read(dynamic_instances_version, dynamic_scene.resource.Get());
        if (entity_surface_noise_version.resource)
        {
            add_read(entity_surface_noise_version, impl_->vfx_noise_volume.resource.Get());
            add_read(entity_surface_gradient_version, impl_->vfx_gradient.resource.Get());
            add_read(entity_surface_stbn_version, impl_->vfx_stbn_scalar.resource.Get());
        }
        for (std::size_t i = 0; i < light_outputs.resources.size(); ++i)
        {
            const std::array versions{light_outputs.lights, light_outputs.ranges,
                light_outputs.inline_indices, light_outputs.spill_indices};
            add_read(versions[i], light_outputs.resources[i]);
        }
        add_read(probe_outputs.irradiance, probe_outputs.irradiance_resource);
        for (const auto &[version, resource] : {
                std::pair{local_probe_outputs.radiance, local_probe_outputs.radiance_resource},
                std::pair{local_probe_outputs.prefiltered, local_probe_outputs.prefiltered_resource},
                std::pair{local_probe_outputs.irradiance, local_probe_outputs.irradiance_resource},
                std::pair{local_probe_outputs.first_hit_distance, local_probe_outputs.distance_resource},
                std::pair{local_probe_outputs.records, local_probe_outputs.record_resource}})
            if (resource) scene_reads.push_back({version, resource, Access::AllShaderRead});
        for (const auto &[version, resource] : {
                std::pair{probe_outputs.radiance, probe_outputs.radiance_resource},
                std::pair{probe_outputs.prefiltered, probe_outputs.prefiltered_resource},
                std::pair{impl_->graph.Version(environment_brdf_lut), impl_->environment_brdf_lut.resource.Get()},
                std::pair{ltc_outputs.matrix, ltc_outputs.matrix_resource},
                std::pair{ltc_outputs.amplitude, ltc_outputs.amplitude_resource}})
            if (resource) scene_reads.push_back({version, resource, Access::AllShaderRead});
        if (have_skinned_instances) add_read(skin_cache_version, frame.skin_cache_vertices.resource.Get());
        const auto add_texture = [&](AllocationResource &texture) {
            if (texture.resource && std::ranges::none_of(scene_reads, [&](const auto &read) { return read.resource == texture.resource.Get(); }))
                scene_reads.push_back({impl_->graph.Version(import_texture(texture, Access::AllShaderRead, "SceneMaterialTexture")),
                    texture.resource.Get(), Access::AllShaderRead});
        };
        for (const auto &binding : texture_bindings)
            add_read(binding.sampled, impl_->texture_streaming.View(binding.texture).resource);
        add_read(texture_metadata, static_cast<ID3D12Resource *>(impl_->graph.Resolve(texture_metadata.resource)));
        for (auto *texture : {&impl_->archer_diffuse, &impl_->archer_normal, &impl_->monster_basecolor,
                &impl_->monster_emissive, &impl_->monster_ram, &impl_->family_diffuse, &impl_->family_normal}) add_texture(*texture);


        const auto job_count = software_scene_job_count;
        auto &scene_pages = software_scene_pages;
        std::vector<ResourceVersion> scene_page_versions;
        scene_page_versions.reserve(scene_pages.size());
        std::unordered_map<std::uint32_t, ResourceVersion> scene_versions_by_slot;
        scene_versions_by_slot.reserve(software_scene_page_slots.size());
        for (const auto page_slot : software_scene_page_slots)
        {
            const auto *page = impl_->FindGeometryPage(page_slot);
            if (!page) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Software GI canonical page disappeared after preflight.");
            const auto version = impl_->graph.Version(import_buffer(
                const_cast<AllocationResource &>(page->storage), Access::VertexShaderRead, "SceneCanonicalGeometry"));
            add_read(version, page->storage.resource.Get());
            scene_versions_by_slot.emplace(page_slot, version);
        }
        for (const auto *page : scene_pages)
            scene_page_versions.push_back(scene_versions_by_slot.at(page->page_slot));
        if (job_count!=0)
        {
            const SceneGeometryInputs scene_inputs{scene_reads,
                make_scene_bindings(kSceneGeometryTableOffset), frame_slot,
                impl_->graph_queue_executor.Completed()};
            const auto registered_scene_geometry = restored_graph
                ? impl_->scene_geometry.BindRetainedPrepared(impl_->graph, scene_inputs,
                    software_scene_prepared, pending_graph_layout.scene_geometry, scene_geometry_outputs)
                : impl_->scene_geometry.RegisterPrepared(impl_->graph, scene_inputs,
                    software_scene_prepared, scene_geometry_outputs, &pending_graph_layout.scene_geometry);
            if (!registered_scene_geometry) return registered_scene_geometry;
            pending_effects.scene_geometry = true;
            for (std::size_t leaf = 0; leaf < brixelizer_leaf_jobs.size(); ++leaf)
            {
                auto &geometry = brixelizer_geometries[leaf];
                const auto job_index = brixelizer_leaf_jobs[leaf];
                SceneGeometryRecord job_scratch{};
                const auto &job = software_scene_prepared.GeometryAt(job_index,job_scratch);
                geometry.vertex_version = (job.flags & SceneGeometryDeformed) ? scene_geometry_outputs.positions
                    : (job.flags & SceneGeometryCachedSkin) ? skin_cache_version : scene_page_versions[job_index];
                geometry.index_version = scene_page_versions[job_index];
            }
        }
    }
    const auto texture_guide = [&](ResourceVersion version) {
        return AmbientOcclusionGuide{version, version.resource
            ? static_cast<ID3D12Resource *>(impl_->graph.Resolve(version.resource)) : nullptr};
    };
    const auto register_software_gi_scene = [&]() -> Result {
        BrixelizerSceneOutputs native_scene_outputs;
        pending_effects.brixelizer = true;
        prepared_gi_guard.service = nullptr;
        const auto registered = restored_graph
            ? owned_view.brixelizer_gi.RegisterScene(impl_->graph, native_scene, native_scene_outputs,
                nullptr, &pending_graph_layout.brixelizer)
            : owned_view.brixelizer_gi.RegisterScene(impl_->graph, native_scene, native_scene_outputs,
                &pending_graph_layout.brixelizer);
        if (!registered) return registered;
        return Result::Success();
    };
    ambient_constants.previous_depth_parameters = previous_view_valid
        ? owned_view.previous_projection_depth
        : DirectX::XMFLOAT4{lod_projection._33, lod_projection._34, lod_projection._43, lod_projection._44};
    ambient_constants.jitter_uv = constants->surface_jitter_uv;
    ambient_constants.toward_light = {-constants->light_direction_intensity.x,
        -constants->light_direction_intensity.y, -constants->light_direction_intensity.z, 0};
    AdaptiveShadingOutputs adaptive_outputs;
    const auto adaptive_status = owned_view.adaptive_shading.Status();
    if (adaptive_status.raster_ready || adaptive_status.compute_ready)
    {
        AdaptiveShadingInputs adaptive_inputs;
        adaptive_inputs.depth = {depth_version, owned_view.targets.depth.resource.Get()};
        adaptive_inputs.normal = {gbuffer_normal_version, surface_normal_resource};
        adaptive_inputs.base = {gbuffer_base_version, surface_base_resource};
        adaptive_inputs.material = {gbuffer_material_version, surface_material_resource};
        adaptive_inputs.metadata = {gbuffer_metadata_version, owned_view.targets.gbuffer_metadata.resource.Get()};
        adaptive_inputs.motion = {gbuffer_motion_version, owned_view.targets.gbuffer_motion.resource.Get()};
        adaptive_inputs.render_active = owned_view.state.render_active;
        adaptive_inputs.depth_parameters = {ambient_constants.projection._33, ambient_constants.projection._34,
            ambient_constants.projection._43, ambient_constants.projection._44};
        adaptive_inputs.material_policy = impl_->adaptive_material_policies;
        adaptive_inputs.frame_slot = frame_slot;
        adaptive_inputs.completed = impl_->graph_queue_executor.Completed();
        adaptive_inputs.force_full_rate = !previous_view_valid || new_session;
        adaptive_inputs.owners.push_back(impl_->resident_generation_lease);
        // Set the completion guard before any graph registration can mutate the
        // service. Policies are copied to this completed frame bank immediately.
        pending_effects.adaptive = true;
        const auto registered_adaptive = restored_graph
            ? owned_view.adaptive_shading.BindRetained(impl_->graph, adaptive_inputs,
                pending_graph_layout.adaptive, adaptive_outputs)
            : owned_view.adaptive_shading.Register(impl_->graph, adaptive_inputs, adaptive_outputs,
                &pending_graph_layout.adaptive);
        if (!registered_adaptive) return registered_adaptive;
        // A deferred CSM must have its actual density producer.
        if (!restored_graph && overlap_ao_csm && csm_required)
        {
            if (!adaptive_outputs.compute_ready || !adaptive_outputs.density.version.resource)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "AO/CSM overlap requires the current adaptive density producer.");
            impl_->graph.AddOrderingDependency(280, 2);
        }
        // Classify shared guides before independent AO/virtual-shadow readers.
        if (!restored_graph && impl_->enhanced &&
            adaptive_outputs.density.version.resource && virtual_shadow_outputs.resources[0])
            impl_->graph.AddOrderingDependency(280, 149);
    }
    record_graph_substage(cpu_timings.graph_scene_selection_us);
    AmbientOcclusionOutputs ambient_outputs;
    AmbientOcclusionInputs ambient_inputs{
        {depth_version, owned_view.targets.depth.resource.Get()},
        {gbuffer_normal_version, surface_normal_resource},
        {gbuffer_metadata_version, owned_view.targets.gbuffer_metadata.resource.Get()},
        {gbuffer_motion_version, owned_view.targets.gbuffer_motion.resource.Get()},
        {gbuffer_surface_history_version, owned_view.targets.gbuffer_surface_history.resource.Get()},
        owned_view.state.render_active, frame_slot, owned_view.frame_number,
        impl_->graph_queue_executor.Completed(), !previous_view_valid || new_session};
    ambient_inputs.queue = overlap_ao_csm ? QueueHint::Compute : QueueHint::Direct;
    if (active_features.ao_mode == GraphicsAoMode::GTAO || impl_->config.contact_shadows)
    {
        ambient_inputs.adaptive_density = texture_guide(adaptive_outputs.density.version);
        const auto registered_ambient = restored_graph
            ? owned_view.ambient_occlusion.BindRetained(impl_->graph, ambient_inputs,
                ambient_constants, pending_graph_layout.ambient, ambient_outputs)
            : owned_view.ambient_occlusion.Register(impl_->graph, ambient_inputs,
                ambient_constants, ambient_outputs, &pending_graph_layout.ambient);
        if (!registered_ambient) return registered_ambient;
        pending_effects.ambient = ambient_outputs.ao.resource || ambient_outputs.contact.resource;
        if (overlap_ao_csm && !ambient_outputs.adaptive_density_used)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "AO/CSM overlap requires the actual adaptive GTAO consumer.");
    }
    constants->lighting_options = {ambient_outputs.ao.resource ? 1.0f : 0.0f,
        ambient_outputs.contact.resource ? 1.0f : 0.0f,
        active_features.gi_mode != GraphicsGiMode::Off ? 1.0f : 0.0f,
        active_features.reflection_mode != GraphicsReflectionMode::Off ? 1.0f : 0.0f};
    auto ambient_descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    ambient_descriptor.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
        kViewDescriptorOffset + 11) * impl_->srv_stride;
    if (ambient_outputs.ao_srv.ptr)
        impl_->device->CopyDescriptorsSimple(1, ambient_descriptor, ambient_outputs.ao_srv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ambient_descriptor.ptr += impl_->srv_stride;
    if (ambient_outputs.contact_srv.ptr)
        impl_->device->CopyDescriptorsSimple(1, ambient_descriptor, ambient_outputs.contact_srv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    record_graph_substage(cpu_timings.graph_ao_us);
    auto &lighting_layout = pending_graph_layout.lighting;
    std::optional<PassBuilder> lighting_pass;
    if (restored_graph)
    {
        lighting_pass = impl_->graph.BindPass(lighting_layout.pass_index, 5);
        if (!lighting_pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Lighting does not match the retained graph plan.");
        hdr_version = lighting_layout.outputs[0];
        subsurface_diffuse_version = lighting_layout.outputs[1];
        environment_specular_version = lighting_layout.outputs[2];
        indirect_diffuse_version = lighting_layout.outputs[3];
    }
    else
    {
        lighting_layout.pass_index = impl_->graph.PassCount();
        lighting_pass = impl_->graph.AddPass(5, kRenderPassNames[5], QueueHint::Direct);
        if (adaptive_outputs.raster_ready)
            lighting_pass->Read(adaptive_outputs.rate.version, Access::ShadingRateSource);
        read_probe(*lighting_pass);
        if (ambient_outputs.ao.resource) lighting_pass->Read(ambient_outputs.ao, Access::ShaderRead);
        if (ambient_outputs.contact.resource) lighting_pass->Read(ambient_outputs.contact, Access::ShaderRead);
        lighting_pass->Read(gbuffer_base_version, Access::ShaderRead);
        if (decal_surface_outputs.active)
            lighting_pass->Read(decal_surface_outputs.emission.version, Access::ShaderRead);
        lighting_pass->Read(gbuffer_normal_version, Access::ShaderRead);
        lighting_pass->Read(gbuffer_metadata_version, Access::ShaderRead);
        lighting_pass->Read(gbuffer_material_version, Access::ShaderRead);
        lighting_pass->Read(depth_version, Access::ShaderRead);
        if (shadow_version.resource) lighting_pass->Read(shadow_version, Access::ShaderRead);
        lighting_pass->Read(impl_->graph.Version(environment_brdf_lut), Access::ShaderRead);
        lighting_pass->Read(material_parameter_version, Access::ShaderRead);
        for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
            lighting_pass->Read(material_sidecar_versions[plane], Access::ShaderRead);
        hdr_version = lighting_pass->Write(hdr_version, Access::RenderTarget);
        if (subsurface_diffuse_version.resource)
            subsurface_diffuse_version = lighting_pass->Write(subsurface_diffuse_version, Access::RenderTarget);
        if (environment_specular_version.resource)
            environment_specular_version = lighting_pass->Write(environment_specular_version, Access::RenderTarget);
        if (indirect_diffuse_version.resource)
            indirect_diffuse_version = lighting_pass->Write(indirect_diffuse_version, Access::RenderTarget);
        lighting_layout.outputs = {hdr_version, subsurface_diffuse_version,
            environment_specular_version, indirect_diffuse_version};
    }
    const auto raw_hdr_version = hdr_version;
    lighting_pass->SetExecuteChecked([&](RenderPassContext &context) -> Result {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0, 0, 0, 0};
        const std::array targets{hdr_rtv, rtv_at(kLightingAuxRtvOffset), rtv_at(kLightingAuxRtvOffset + 1),
            rtv_at(kLightingAuxRtvOffset + 2)};
        list->ClearRenderTargetView(hdr_rtv, clear, 0, nullptr);
        const std::array auxiliary_resources{owned_view.targets.subsurface_diffuse.resource.Get(),
            owned_view.targets.environment_specular.resource.Get(), owned_view.targets.indirect_diffuse.resource.Get()};
        for (std::size_t index = 0; index < auxiliary_resources.size(); ++index)
            if (auxiliary_resources[index]) list->ClearRenderTargetView(targets[index + 1], clear, 0, nullptr);
        list->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), FALSE, nullptr);
        const auto auxiliary_mask = static_cast<unsigned>(auxiliary_resources[0] != nullptr) |
            (static_cast<unsigned>(auxiliary_resources[1] != nullptr) << 1) |
            (static_cast<unsigned>(auxiliary_resources[2] != nullptr) << 2);
        list->SetPipelineState(impl_->deferred_pipeline[auxiliary_mask].Get());
        list->SetGraphicsRootDescriptorTable(5, texture_table);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        AdaptiveShadingRasterScope rate_scope;
        if (auto bound = owned_view.adaptive_shading.BeginRaster(context, adaptive_outputs, rate_scope); !bound)
            return bound;
        list->DrawInstanced(3, 1, 0, 0);
        return Result::Success();
    });
    BrixelizerLightingOutputs brixelizer_lighting;
    UINT brixelizer_bank{};
    const SceneFrameTableBuilder make_brixelizer_bindings = [&](D3D12_CPU_DESCRIPTOR_HANDLE suffix,
            UINT count, SceneShaderBinder &binder) -> Result {
        if (!suffix.ptr || count != kBrixelizerDescriptorCount || brixelizer_bank >= 4)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid software GI descriptor stage bank.");
        const auto offset = kBrixelizerFrameTableOffset + brixelizer_bank++ * kSceneFrameTableCount;
        auto destination = local_probe_inputs.frame_srv_base; destination.ptr += offset * impl_->srv_stride;
        auto source = local_probe_inputs.frame_srv_base; source.ptr += kTemporalTextureDescriptorOffset * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(kSceneFrameTableCount, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        destination.ptr += (kBrixelizerDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(count, destination, suffix, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto table = texture_table; table.ptr += UINT64(offset) * impl_->srv_stride;
        binder = [&, table](ID3D12GraphicsCommandList *list) -> Result {
            bind_surface_compute(list); list->SetComputeRootDescriptorTable(32, table); return Result::Success();
        };
        return Result::Success();
    };
    struct SoftwareGiBackingRead { ResourceVersion version; Access access; };
    std::vector<SoftwareGiBackingRead> software_gi_backing;
    if (active_features.software_gi_enabled)
    {
        const auto append = [&](ResourceVersion version, Access access) {
            if (version.resource) software_gi_backing.push_back({version, access});
        };
        append(shadow_version, Access::ShaderRead);
        append(material_parameter_version, Access::ShaderRead);
        if (decal_surface_outputs.active) append(decal_surface_outputs.emission.version, Access::ShaderRead);
        if (local_shadow_outputs.resources[0])
            for (const auto version : {local_shadow_outputs.metadata, local_shadow_outputs.records,
                    local_shadow_outputs.static_bank, local_shadow_outputs.dynamic_bank, local_shadow_outputs.draw_records})
                append(version, Access::AllShaderRead);
        if (local_probe_outputs.record_resource)
            for (const auto version : {local_probe_outputs.radiance, local_probe_outputs.prefiltered,
                    local_probe_outputs.irradiance, local_probe_outputs.first_hit_distance, local_probe_outputs.records})
                append(version, Access::AllShaderRead);
        if (virtual_shadow_outputs.resources[0])
            for (const auto version : {virtual_shadow_outputs.metadata, virtual_shadow_outputs.page_table,
                    virtual_shadow_outputs.static_bank, virtual_shadow_outputs.dynamic_bank, virtual_shadow_outputs.draw_records})
                append(version, Access::ShaderRead);
        if(virtual_shadow_outputs.root_static.resource)
            append(virtual_shadow_outputs.root_static,Access::ShaderRead);
        if(virtual_shadow_outputs.root_dynamic.resource)
            append(virtual_shadow_outputs.root_dynamic,Access::ShaderRead);
        for (const auto version : {probe_outputs.radiance, probe_outputs.prefiltered, probe_outputs.irradiance,
                light_outputs.lights, light_outputs.ranges, light_outputs.inline_indices, light_outputs.spill_indices,
                ltc_outputs.matrix, ltc_outputs.amplitude}) append(version, Access::ShaderRead);
        if (auto registered = register_software_gi_scene(); !registered) return registered;
        std::vector<SceneShaderRead> cache_reads;
        const auto append_cache_read = [&](AmbientOcclusionGuide guide, Access access = Access::ShaderRead) {
            const auto existing = std::ranges::find_if(cache_reads, [&](const auto &read) {
                return read.version.resource.value == guide.version.resource.value &&
                    read.version.resource.kind == guide.version.resource.kind &&
                    read.version.ordinal == guide.version.ordinal;
            });
            if (existing == cache_reads.end()) cache_reads.push_back({guide.version, guide.resource, access});
            else if (existing->access != access) existing->access = Access::AllShaderRead;
        };
        for (const auto version : {gbuffer_base_version, gbuffer_normal_version,
                gbuffer_metadata_version, gbuffer_material_version, depth_version,
                // This dependency places the seed after the real DeferredLight pass.
                indirect_diffuse_version})
            append_cache_read(texture_guide(version));
        for (const auto &read : software_gi_backing)
            append_cache_read(texture_guide(read.version), read.access);
        for (const auto &read : scene_reads)
            // This pass evaluates the shared closure in the pixel shader.
            append_cache_read({read.version,read.resource},
                read.access == Access::ComputeRead ? Access::AllShaderRead : read.access);
        for (UINT plane=0;plane<impl_->material_sidecar_layout.plane_count;++plane)
            append_cache_read(texture_guide(material_sidecar_versions[plane]));
        if (ambient_outputs.ao.resource) append_cache_read(texture_guide(ambient_outputs.ao));
        if (ambient_outputs.contact.resource) append_cache_read(texture_guide(ambient_outputs.contact));
        BrixelizerCacheInjectionInputs cache;
        cache.reads=cache_reads;
        cache.bind_graphics=[&](ID3D12GraphicsCommandList *list) -> Result {
            bind_graphics(list); list->SetGraphicsRootDescriptorTable(5,texture_table); return Result::Success();
        };
        cache.render_active=owned_view.state.render_active; cache.frame_slot=frame_slot;
        cache.owners=brixelizer_owners;
        AmbientOcclusionGuide cache_injection;
        if (auto registered=owned_view.brixelizer_gi.RegisterCacheInjection(impl_->graph,cache,cache_injection);!registered) return registered;
        BrixelizerLightingInputs native;
        native.depth=texture_guide(depth_version); native.normal=texture_guide(gbuffer_normal_version);
        native.metadata=texture_guide(gbuffer_metadata_version); native.motion=texture_guide(gbuffer_motion_version);
        native.base=texture_guide(gbuffer_base_version); native.material=texture_guide(gbuffer_material_version);
        native.sidecar_plane_count=impl_->material_sidecar_layout.plane_count;
        for(UINT plane=0;plane<native.sidecar_plane_count;++plane)
            native.sidecars[plane]=texture_guide(material_sidecar_versions[plane]);
        native.cache_injection=cache_injection; native.fallback_diffuse=texture_guide(indirect_diffuse_version);
        native.environment_specular=texture_guide(environment_specular_version);
        native.subsurface_diffuse=texture_guide(subsurface_diffuse_version);
        native.environment_cube=texture_guide(probe_outputs.radiance);
        native.reads=scene_reads; native.make_bindings=make_brixelizer_bindings;
        native.restore_bindings=[&](ID3D12GraphicsCommandList *list) -> Result {bind_surface_compute(list);return Result::Success();};
        native.render_active=owned_view.state.render_active; native.frame_slot=frame_slot;
        native.frame_index=owned_view.frame_number; native.light_revision=brixelizer_light_epoch;
        native.completed=impl_->graph_queue_executor.Completed(); native.owners=brixelizer_owners;
        native.camera=owned_view.sdk_camera;
        DirectX::XMStoreFloat4x4(&native.camera.projection,projection*DirectX::XMMatrixTranslation(jitter_x,jitter_y,0));
        DirectX::XMStoreFloat4x4(&native.camera.inverse_projection,DirectX::XMMatrixInverse(nullptr,DirectX::XMLoadFloat4x4(&native.camera.projection)));
        native.camera.previous_view=owned_view.brixelizer_history_valid ? previous_sdk_camera.view : native.camera.view;
        const auto previous_native_projection=owned_view.brixelizer_history_valid
            ? DirectX::XMLoadFloat4x4(&previous_sdk_camera.projection) * DirectX::XMMatrixTranslation(
                2.0f*constants->surface_jitter_uv.z,-2.0f*constants->surface_jitter_uv.w,0)
            : DirectX::XMLoadFloat4x4(&native.camera.projection);
        DirectX::XMStoreFloat4x4(&native.camera.previous_projection,previous_native_projection);
        native.camera.reset_history=false; // native history was recreated only after a proved drain
        BrixelizerConstants native_constants;
        native_constants.inverse_view_projection=camera_relative_inverse_view_projection;
        native_constants.jitter_uv=constants->surface_jitter_uv;
        native_constants.world_origin={lod_eye.x,lod_eye.y,lod_eye.z,0};
        if(auto registered=owned_view.brixelizer_gi.RegisterLighting(impl_->graph,native,native_constants,brixelizer_lighting);!registered) return registered;
    }
    SubsurfaceConstants diffusion_constants;
    diffusion_constants.inverse_view_projection = camera_relative_inverse_view_projection;
    diffusion_constants.quality_options.x = impl_->QualityProfileFor(impl_->config.material_quality).subsurface_half_samples;
    std::copy(constants->material_parameter_bases.begin(), constants->material_parameter_bases.end(),
              diffusion_constants.material_parameter_bases);
    std::copy(constants->hero_material_parameter_bases.begin(), constants->hero_material_parameter_bases.end(),
              diffusion_constants.hero_material_parameter_bases);
    const SubsurfaceInputs diffusion_inputs{
        {depth_version, owned_view.targets.depth.resource.Get()},
        {gbuffer_normal_version, surface_normal_resource},
        {gbuffer_metadata_version, owned_view.targets.gbuffer_metadata.resource.Get()},
        brixelizer_lighting.adjusted_subsurface.resource
            ? SubsurfaceGuide{brixelizer_lighting.adjusted_subsurface.version, brixelizer_lighting.adjusted_subsurface.resource}
            : SubsurfaceGuide{subsurface_diffuse_version, owned_view.targets.subsurface_diffuse.resource.Get()},
        {hdr_version, opaque_hdr_resource}, {material_parameter_version, impl_->material_parameters.resource.Get()},
        owned_view.state.render_active, frame_slot, impl_->graph_queue_executor.Completed()};
    SubsurfaceOutputs diffusion_outputs;
    const auto registered_subsurface = restored_graph
        ? owned_view.subsurface.BindRetained(impl_->graph, diffusion_inputs,
            diffusion_constants, pending_graph_layout.subsurface, diffusion_outputs)
        : owned_view.subsurface.Register(impl_->graph, diffusion_inputs,
            diffusion_constants, diffusion_outputs, &pending_graph_layout.subsurface);
    if (!registered_subsurface) return registered_subsurface;
    pending_effects.diffusion = static_cast<bool>(diffusion_outputs.corrected.resource);
    if (diffusion_outputs.corrected.resource)
    {
        hdr_version = diffusion_outputs.corrected;
        opaque_hdr_resource = diffusion_outputs.corrected_resource;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE opaque_hdr_srv = diffusion_outputs.corrected_srv;
    if (active_features.software_gi_enabled)
    {
        BrixelizerCompositeInputs native;
        native.hdr=texture_guide(hdr_version);native.make_bindings=make_brixelizer_bindings;
        native.render_active=owned_view.state.render_active;native.frame_slot=frame_slot;native.owners=brixelizer_owners;
        // Lighting registration retains the exact delta/environment versions.
        BrixelizerTexture composed;
        if(auto registered=owned_view.brixelizer_gi.RegisterComposite(impl_->graph,native,composed);!registered) return registered;
        hdr_version=composed.version;opaque_hdr_resource=composed.resource;opaque_hdr_srv=composed.srv;
    }
    ScreenReflectionsConstants reflection_constants;
    reflection_constants.inverse_view_projection = camera_relative_inverse_view_projection;
    // Camera-relative reconstruction requires a translation-free view matrix.
    DirectX::XMStoreFloat4x4(&reflection_constants.world_to_view,
        DirectX::XMMatrixTranslationFromVector(eye) * view);
    DirectX::XMStoreFloat4x4(&reflection_constants.projection,
        projection * DirectX::XMMatrixTranslation(jitter_x, jitter_y, 0.0f));
    reflection_constants.previous_depth_parameters = ambient_constants.previous_depth_parameters;
    reflection_constants.jitter_uv = constants->surface_jitter_uv;
    ScreenReflectionsInputs reflection_inputs;
    reflection_inputs.depth = {depth_version, owned_view.targets.depth.resource.Get()};
    reflection_inputs.base = {gbuffer_base_version, surface_base_resource};
    reflection_inputs.normal = {gbuffer_normal_version, surface_normal_resource};
    reflection_inputs.metadata = {gbuffer_metadata_version, owned_view.targets.gbuffer_metadata.resource.Get()};
    reflection_inputs.material = {gbuffer_material_version, surface_material_resource};
    reflection_inputs.motion = {gbuffer_motion_version, owned_view.targets.gbuffer_motion.resource.Get()};
    reflection_inputs.surface_history = {gbuffer_surface_history_version, owned_view.targets.gbuffer_surface_history.resource.Get()};
    reflection_inputs.opaque_hdr = {hdr_version, opaque_hdr_resource};
    reflection_inputs.environment_specular = brixelizer_lighting.specular.resource
        ? ScreenReflectionResource{brixelizer_lighting.specular.version,brixelizer_lighting.specular.resource}
        : ScreenReflectionResource{environment_specular_version,owned_view.targets.environment_specular.resource.Get()};
    reflection_inputs.material_parameters = {material_parameter_version, impl_->material_parameters.resource.Get()};
    reflection_inputs.sidecar_plane_count = impl_->material_sidecar_layout.plane_count;
    for (std::uint32_t plane = 0; plane < reflection_inputs.sidecar_plane_count; ++plane)
        reflection_inputs.sidecars[plane] = {material_sidecar_versions[plane], owned_view.targets.material_sidecars[plane].resource.Get()};
    reflection_inputs.frame_constants_address = frame.upload.resource->GetGPUVirtualAddress();
    reflection_inputs.render_active = owned_view.state.render_active;
    reflection_inputs.frame_slot = frame_slot;
    reflection_inputs.frame_index = owned_view.frame_number;
    reflection_inputs.completed = impl_->graph_queue_executor.Completed();
    reflection_inputs.reset_history = !previous_view_valid || new_session;
    reflection_inputs.ambient_visibility = {ambient_outputs.ao, ambient_outputs.ao_resource};
    reflection_inputs.ambient_visibility_enabled = static_cast<bool>(ambient_outputs.ao.resource);
    ScreenReflectionsOutputs reflection_outputs;
    const auto registered_reflections = restored_graph
        ? owned_view.screen_reflections.BindRetained(impl_->graph, reflection_inputs,
            reflection_constants, pending_graph_layout.reflections, reflection_outputs)
        : owned_view.screen_reflections.Register(impl_->graph, reflection_inputs,
            reflection_constants, reflection_outputs, &pending_graph_layout.reflections);
    if (!registered_reflections) return registered_reflections;
    pending_effects.reflections = static_cast<bool>(reflection_outputs.composed.resource);
    if (reflection_outputs.composed.resource)
    {
        hdr_version = reflection_outputs.composed;
        opaque_hdr_resource = reflection_outputs.composed_resource;
        opaque_hdr_srv = reflection_outputs.composed_srv;
    }
    VolumetricOutputs volumetric_outputs;
    const auto volumetric_camera = retained_volumetric_camera;
    const auto volumetric_raw_hdr = hdr_version;
    if (impl_->config.volumetric_quality != GraphicsOptionalQuality::Off)
    {
        VolumetricInputs inputs;
        inputs.depth = {depth_version, owned_view.targets.depth.resource.Get()}; inputs.hdr = {hdr_version, opaque_hdr_resource};
        inputs.lights = light_outputs; inputs.shadows = virtual_shadow_outputs; inputs.volumes = impl_->volumetric_media;
        inputs.local_shadows = local_shadow_outputs;
        if (impl_->config.vfx_quality == VfxQuality::High) inputs.hero_volumes = typed_volumes;
        if (!typed_volumes.empty() && impl_->config.vfx_quality == VfxQuality::High)
        {
            const auto blue_noise = import_texture(impl_->vfx_stbn_scalar, Access::ShaderRead, "VfxHeroBlueNoise");
            inputs.hero_blue_noise = {impl_->graph.Version(blue_noise), impl_->vfx_stbn_scalar.resource.Get()};
            const auto gradient = import_texture(impl_->vfx_gradient, Access::ShaderRead, "VfxHeroGradient");
            inputs.hero_gradient = {impl_->graph.Version(gradient), impl_->vfx_gradient.resource.Get()};
            const auto noise_volume = import_texture(impl_->vfx_noise_volume, Access::ShaderRead, "VfxHeroNoiseVolume");
            inputs.hero_noise_volume = {impl_->graph.Version(noise_volume), impl_->vfx_noise_volume.resource.Get()};
        }
        inputs.camera = volumetric_camera;
        inputs.previous_camera = retained_volumetric_previous_camera;
        inputs.render_active = owned_view.state.render_active; inputs.frame_slot = frame_slot; inputs.frame_index = owned_view.frame_number;
        inputs.completed = impl_->graph_queue_executor.Completed(); inputs.reset_history = volumetric_reset_history;
        inputs.revisions = retained_volumetric_revisions;
        if (primary_sun && csm_required)
        {
            inputs.compact_shadow.atlas = {shadow_version, owned_view.targets.shadow.resource.Get()};
            inputs.compact_shadow.identity = {static_cast<UINT>(primary_sun->stable_id), static_cast<UINT>(primary_sun->stable_id >> 32), primary_sun->generation, 1};
            inputs.compact_shadow.receiver_bias = 0.0f;
            for (std::size_t cascade = 0; cascade < 3; ++cascade)
            {
                DirectX::XMStoreFloat4x4(&inputs.compact_shadow.view_projection[cascade],
                    DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&constants->shadow_view_projection[cascade])));
                inputs.compact_shadow.atlas_scale_offset[cascade] = constants->shadow_atlas_scale_offset[cascade];
            }
        }
        inputs.make_bindings = [&](D3D12_CPU_DESCRIPTOR_HANDLE suffix, std::uint32_t count, SceneShaderBinder &binder) -> Result {
            if (count != kVolumetricDescriptorCount || !suffix.ptr)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid volumetric descriptor suffix.");
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount + kVolumetricFrameTableOffset) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(count, destination, suffix, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const auto copy_range = [&](std::uint32_t target_offset, std::uint32_t source_offset, std::uint32_t length) {
                auto target = destination; target.ptr += SIZE_T(target_offset) * impl_->srv_stride;
                auto source = local_probe_inputs.frame_srv_base; source.ptr += SIZE_T(source_offset) * impl_->srv_stride;
                impl_->device->CopyDescriptorsSimple(length, target, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            };
            for (const auto &copy : kVolumetricFrameInputCopies)
                copy_range(copy.target, copy.source, copy.count);
            auto table = texture_table; table.ptr += UINT64(kVolumetricFrameTableOffset) * impl_->srv_stride;
            binder = [heap = impl_->srv_heap, table](ID3D12GraphicsCommandList *list) -> Result {
                ID3D12DescriptorHeap *heaps[]{heap.Get()};
                list->SetDescriptorHeaps(1, heaps);
                list->SetComputeRootDescriptorTable(0, table); return Result::Success();
            };
            return Result::Success();
        };
        pending_effects.volumetrics = true;
        const auto registered_volumetrics = restored_graph
            ? owned_view.volumetrics.BindRetained(impl_->graph, inputs,
                pending_graph_layout.volumetrics, volumetric_outputs)
            : owned_view.volumetrics.Register(impl_->graph, inputs, volumetric_outputs,
                &pending_graph_layout.volumetrics);
        if (!registered_volumetrics) return registered_volumetrics;
        auto destination = local_probe_inputs.frame_srv_base; destination.ptr += kVolumetricDescriptorOffset * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(kVolumetricDescriptorCount, destination, volumetric_outputs.descriptor_suffix,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        hdr_version = volumetric_outputs.resolved.version; opaque_hdr_resource = volumetric_outputs.resolved.resource;
        opaque_hdr_srv = volumetric_outputs.resolved.srv;
    }
    const auto read_volumetrics = [&](PassBuilder &pass) {
        if (!volumetric_outputs.constant_resource) return;
        for (const auto version : {volumetric_outputs.constants, volumetric_outputs.filtered_scattering.version,
                volumetric_outputs.column_depth.version, volumetric_outputs.scattering.version,
                volumetric_outputs.transmittance.version, volumetric_raw_hdr}) pass.Read(version, Access::AllShaderRead);
    };
    // All later raster consumers use t6/space0 from the same immutable post
    // table as the main texture table; patch both aliases before recording.
    if (opaque_hdr_srv.ptr)
    {
        for (const auto offset : {0u, kToneMapInputDescriptorOffset, kFxaaInputDescriptorOffset})
        {
            auto descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            descriptor.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
                offset + 4) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(1, descriptor, opaque_hdr_srv,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }
    std::optional<PassBuilder> distortion_pass;
    if (restored_graph)
    {
        distortion_pass = impl_->graph.BindPass(pending_graph_layout.distortion_pass.pass_index, 6);
        if (!distortion_pass)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Distortion pass does not match the retained graph plan.");
        distortion_vectors_version = pending_graph_layout.distortion_pass.output;
    }
    else
    {
        pending_graph_layout.distortion_pass.pass_index = impl_->graph.PassCount();
        distortion_pass = impl_->graph.AddPass(6, kRenderPassNames[6], QueueHint::Direct);
        distortion_pass->Read(gbuffer_metadata_version, Access::ShaderRead);
        distortion_pass->Read(gbuffer_normal_version, Access::ShaderRead);
        distortion_pass->Read(depth_version, Access::ShaderRead);
        distortion_vectors_version = distortion_pass->Write(distortion_vectors_version, Access::RenderTarget);
        pending_graph_layout.distortion_pass.output = distortion_vectors_version;
    }
    distortion_pass->SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0,0,0,0};
        list->ClearRenderTargetView(distortion_rtv,clear,0,nullptr);
        if (distortion_count == 0) return;
        list->OMSetRenderTargets(1,&distortion_rtv,FALSE,nullptr);
        list->SetPipelineState(impl_->vfx_distortion_pipeline.Get());
        list->SetGraphicsRootDescriptorTable(5,texture_table);
        list->SetGraphicsRootShaderResourceView(27,frame.upload.resource->GetGPUVirtualAddress()+distortion_data_offset);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(6,distortion_count,0,0);
    });

    const auto vfx_gradient_version = impl_->graph.Version(import_texture(
        impl_->vfx_gradient, Access::AllShaderRead, "VfxGradient"));
    std::optional<PassBuilder> transparent_pass;
    if (restored_graph)
    {
        transparent_pass = impl_->graph.BindPass(pending_graph_layout.transparent_pass.pass_index, 7);
        if (!transparent_pass)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Transparent pass does not match the retained graph plan.");
        oit_accumulation_version = pending_graph_layout.transparent_pass.first;
        oit_revealage_version = pending_graph_layout.transparent_pass.second;
    }
    else
    {
    pending_graph_layout.transparent_pass.pass_index = impl_->graph.PassCount();
    transparent_pass = impl_->graph.AddPass(7, kRenderPassNames[7], QueueHint::Direct);
    read_probe(*transparent_pass);
    read_volumetrics(*transparent_pass);
    // Retained GPU particles can sample the gradient even without new spawns.
    // Root 13 has ALL visibility, so declare every shader stage's read access.
    transparent_pass->Read(vfx_gradient_version, Access::AllShaderRead);
    transparent_pass->Read(geometry_records_version, Access::ShaderRead);
    transparent_pass->Read(material_parameter_version, Access::ShaderRead);
    transparent_pass->Read(hero_binding_version, Access::ShaderRead);
    transparent_pass->Read(hdr_version, Access::ShaderRead);
    read_geometry_pages(*transparent_pass);
    transparent_pass->Read(dynamic_instances_version, Access::ShaderRead);
    if (separate_previous_instance_scene)
        transparent_pass->Read(previous_instance_snapshot_version, Access::VertexShaderRead);
    transparent_pass->Read(dynamic_visible_refs_version, Access::ShaderRead);
    transparent_pass->Read(dynamic_draw_args_version, Access::IndirectArgs);
    if (static_instances_buffer) transparent_pass->Read(static_instances_version, Access::ShaderRead);
    if (static_refs_buffer) transparent_pass->Read(static_refs_version, Access::ShaderRead);
    transparent_pass->Read(static_visible_refs_version, Access::ShaderRead);
    transparent_pass->Read(static_draw_args_version, Access::IndirectArgs);
    if (have_skinned_instances)
        transparent_pass->Read(skin_cache_version, Access::ShaderRead);
    if (have_skinned_instances && previous_skin_frame_valid)
        transparent_pass->Read(previous_skin_cache_version, Access::ShaderRead);
    transparent_pass->Read(depth_version, Access::DepthShaderRead);
    if (shadow_version.resource) transparent_pass->Read(shadow_version, Access::ShaderRead);
    transparent_pass->Read(particles_version, Access::ShaderRead);
    transparent_pass->Read(particle_alive_output_version, Access::ShaderRead);
    transparent_pass->Read(indirect_arguments_version, Access::IndirectArgs);
    transparent_pass->Read(ribbon_history_version, Access::ShaderRead);
    transparent_pass->Read(ribbon_previous_version, Access::ShaderRead);
    transparent_pass->Read(ribbon_arguments_version, Access::IndirectArgs);
    transparent_pass->Read(gbuffer_metadata_version, Access::ShaderRead);
    transparent_pass->Read(gbuffer_normal_version, Access::ShaderRead);
    oit_accumulation_version = transparent_pass->Write(oit_accumulation_version, Access::RenderTarget);
    oit_revealage_version = transparent_pass->Write(oit_revealage_version, Access::RenderTarget);
    pending_graph_layout.transparent_pass.first = oit_accumulation_version;
    pending_graph_layout.transparent_pass.second = oit_revealage_version;
    }
    transparent_pass->SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear_accumulation[] = {0, 0, 0, 0};
        constexpr float clear_revealage[] = {1, 1, 1, 1};
        list->ClearRenderTargetView(
            oit_accumulation_rtv, clear_accumulation, 0, nullptr);
        list->ClearRenderTargetView(
            oit_revealage_rtv, clear_revealage, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
            oit_accumulation_rtv, oit_revealage_rtv};
        list->OMSetRenderTargets(2, targets, FALSE, &readonly_dsv);
        list->SetPipelineState(impl_->particle_pipeline.Get());
        list->SetGraphicsRootShaderResourceView(16, impl_->vfx_mesh_atlas.resource->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(
            2, impl_->particles.resource->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(
            12,
            impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // Reuse the existing root-constant + draw signature for all exact-size bins.
        list->ExecuteIndirect(impl_->ribbon_draw_signature.Get(), kParticleDrawBinCount,
            impl_->indirect_arguments.resource.Get(), 0, nullptr, 0);
        list->SetGraphicsRoot32BitConstant(6, 0, 0);
        if (ground_ring_count != 0)
        {
            list->SetPipelineState(impl_->ground_ring_pipeline.Get());
            list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() +
                        ground_ring_data_offset);
            if (ground_quad_count != 0u) list->DrawInstanced(6, ground_quad_count, 0, 0);
            if (ground_quad_count != ground_ring_count)
            {
                list->SetGraphicsRootShaderResourceView(11,
                    frame.upload.resource->GetGPUVirtualAddress() + ground_ring_data_offset +
                    ground_quad_count * sizeof(GpuParticleSpawnCommand));
                list->DrawInstanced(32u * 6u, ground_ring_count - ground_quad_count, 0, 0);
            }
        }

        if (owner_mesh_count != 0)
        {
            list->SetPipelineState(impl_->owner_mesh_pipeline.Get());
            list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() + owner_mesh_data_offset);
            for (std::size_t first = 0; first < owner_mesh_count;)
            {
                const auto run = renderer_detail::NextVfxOwnedMeshDrawRun(
                    typed_mesh_spawns.first(owner_mesh_count), impl_->vfx_mesh_vertex_counts, first);
                list->SetGraphicsRoot32BitConstant(6, static_cast<UINT>(run.first), 0);
                list->DrawInstanced(run.vertex_count, static_cast<UINT>(run.count), 0, 0);
                first += run.count;
            }
            list->SetGraphicsRoot32BitConstant(6, 0, 0);
        }

        if (flash_count > flash_add_count)
        {
            list->SetPipelineState(impl_->vfx_sprite_oit_pipeline.Get());
            list->SetGraphicsRootShaderResourceView(11,
                frame.upload.resource->GetGPUVirtualAddress() + flash_data_offset + flash_add_count * sizeof(GpuParticleSpawnCommand));
            list->DrawInstanced(6, flash_count - flash_add_count, 0, 0);
        }
        draw_ribbons(list, true);
        // Family bodies and gel projectiles use the weighted OIT mesh pipeline.
        list->SetPipelineState(impl_->slime_pipeline.Get());
        list->SetGraphicsRoot32BitConstant(
            6, kDefaultSceneDrawMaterial | 1u, 1);
        bind_scene_deformation_roots(list, true, true);
        const auto draw_cached_hero_range = [&](std::size_t bucket) {
            if (bucket_pages[bucket]->index_count == 0) return;
            list->SetGraphicsRootShaderResourceView(34, dynamic_instance_base);
            list->SetGraphicsRootShaderResourceView(
                35, frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
            if (auto result = impl_->DrawGeometry(list, frame_slot,
                    impl_->hero_translucent_pipeline.Get(), *bucket_pages[bucket],
                    dynamic_geometry_passes[bucket][0], frame.dynamic_draw_args.resource.Get(),
                    bucket * kGeometryArgumentStride); !result)
                throw std::runtime_error(std::string(result.Message()));
        };
        if (owned_view.hero_transparency.AllocatedBytes() == 0)
        {
            if (has_translucent_hero_binding(0) && archer_range.second != 0)
                draw_cached_hero_range(kArcherVisibilityBucket);
            for (std::size_t asset = 0; asset < monster_bucket_ranges.size(); ++asset)
                if (has_translucent_monster_bucket(asset) && monster_bucket_ranges[asset].second != 0)
                    draw_cached_hero_range(kMonsterVisibilityBucketBegin + asset);
            if (impl_->static_gpu_scene)
            {
                list->SetGraphicsRootShaderResourceView(34,impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress());
                list->SetGraphicsRootShaderResourceView(35,frame.static_visible_refs.resource->GetGPUVirtualAddress());
                for(std::size_t lod=0;lod<3;++lod)
                {
                    const auto bucket=render_data::kMeadowWaterGeometry*3+lod;
                    if(static_environment_ranges[bucket].second==0)continue;
                    if(auto drawn=impl_->DrawGeometry(list,frame_slot,impl_->hero_translucent_pipeline.Get(),*bucket_pages[bucket],
                        static_geometry_passes[bucket][0],frame.static_draw_args.resource.Get(),bucket*kGeometryArgumentStride);!drawn)
                        throw std::runtime_error(std::string(drawn.Message()));
                }
            }
        }
        if (projectile_range.second != 0)
        {
            list->SetGraphicsRootShaderResourceView(34, dynamic_instance_base);
            list->SetGraphicsRootShaderResourceView(
                35, frame.upload.resource->GetGPUVirtualAddress() +
                        dynamic_ref_data_offset);
            if (auto result = impl_->DrawGeometry(list, frame_slot,
                    impl_->slime_pipeline.Get(), impl_->gel_projectile_geometry,
                    projectile_geometry_pass); !result)
                throw std::runtime_error(std::string(result.Message()));
        }
        if (!fresnel_draws.empty())
        {
            list->SetPipelineState(impl_->fresnel_shell_pipeline.Get());
            list->SetGraphicsRootDescriptorTable(5, texture_table);
            for (const auto &draw : fresnel_draws)
            {
                list->SetGraphicsRootShaderResourceView(
                    34, dynamic_instance_base);
                list->SetGraphicsRootShaderResourceView(
                    35, frame.upload.resource->GetGPUVirtualAddress() +
                            dynamic_ref_data_offset);
                list->SetGraphicsRootShaderResourceView(
                    31, frame.upload.resource->GetGPUVirtualAddress() + fresnel_data_offset +
                            draw.command_index * sizeof(GpuFresnelShell));
                const GeometryPageGpu &page = draw.archer ? impl_->archer_geometry
                    : draw.lod == 0 ? static_cast<const GeometryPageGpu &>(impl_->monster_assets[draw.asset_index])
                    : impl_->monster_reduced_geometry[draw.asset_index][draw.lod - 1u];
                if (auto result = impl_->DrawGeometry(list, frame_slot,
                        impl_->fresnel_shell_pipeline.Get(), page, draw.geometry_pass); !result)
                    throw std::runtime_error(std::string(result.Message()));
            }
        }
    });

    record_graph_substage(cpu_timings.graph_opaque_us);
    HeroTransparencyOutputs hero_outputs;
    HeroTransparencyInputs hero_inputs;
    std::vector<HeroTransparencySceneRead> hero_scene_reads, hero_peel_reads;
#if defined(HS_DEVELOPMENT_TOOLS)
    struct HeroWorkBinding { std::size_t bucket{}; bool immutable{}; };
    std::vector<HeroWorkContributorInput> hero_work_contributors;
    std::vector<HeroWorkBinding> hero_work_bindings;
#endif
    // All three water LOD ranges above refer to the same conservative source set.
    // Empty contributors export absent guides; downstream consumers select their opaque path.
    if (have_hero_contributors)
    {
        hero_peel_reads = {
            {geometry_records_version, Access::VertexShaderRead}, {material_parameter_version, Access::ShaderRead},
            {hero_binding_version, Access::ShaderRead}, {dynamic_instances_version, Access::VertexShaderRead},
            {dynamic_visible_refs_version, Access::VertexShaderRead}, {dynamic_draw_args_version, Access::IndirectArgs},
            {static_visible_refs_version, Access::VertexShaderRead}, {static_draw_args_version, Access::IndirectArgs}};
        if (separate_previous_instance_scene)
            hero_peel_reads.push_back({previous_instance_snapshot_version, Access::VertexShaderRead});
        if (static_instances_buffer) hero_peel_reads.push_back({static_instances_version, Access::VertexShaderRead});
        if (static_refs_buffer) hero_peel_reads.push_back({static_refs_version, Access::VertexShaderRead});
        if (have_skinned_instances) hero_peel_reads.push_back({skin_cache_version, Access::VertexShaderRead});
        for (const auto &page : geometry_pages) hero_peel_reads.push_back({page.version, Access::VertexShaderRead});
        if (impl_->family_diffuse.resource)
            hero_peel_reads.push_back({impl_->graph.Version(import_texture(
                impl_->family_diffuse, Access::AllShaderRead, "Hero.OpacityColor")), Access::AllShaderRead});
        hero_scene_reads = {
            {geometry_records_version, Access::ShaderRead}, {material_parameter_version, Access::ShaderRead},
            {hero_binding_version, Access::ShaderRead}, {dynamic_instances_version, Access::ShaderRead},
            {dynamic_visible_refs_version, Access::ShaderRead}, {dynamic_draw_args_version, Access::IndirectArgs},
            {impl_->graph.Version(environment_brdf_lut), Access::ShaderRead},
            {probe_outputs.radiance, Access::ShaderRead}, {probe_outputs.prefiltered, Access::ShaderRead},
            {probe_outputs.irradiance, Access::ShaderRead}, {light_outputs.lights, Access::ShaderRead},
            {light_outputs.ranges, Access::ShaderRead}, {light_outputs.inline_indices, Access::ShaderRead},
            {light_outputs.spill_indices, Access::ShaderRead}, {ltc_outputs.matrix, Access::ShaderRead},
            {ltc_outputs.amplitude, Access::ShaderRead}};
        if (separate_previous_instance_scene)
            hero_scene_reads.push_back({previous_instance_snapshot_version, Access::VertexShaderRead});
        for (auto *texture : {&impl_->family_diffuse, &impl_->family_normal})
            if (texture->resource)
                hero_scene_reads.push_back({impl_->graph.Version(import_texture(
                    *texture, Access::AllShaderRead, "Hero.MaterialTexture")), Access::AllShaderRead});
        if (shadow_version.resource) hero_scene_reads.push_back({shadow_version, Access::ShaderRead});
        if (static_instances_buffer) hero_scene_reads.push_back({static_instances_version, Access::ShaderRead});
        if (static_refs_buffer) hero_scene_reads.push_back({static_refs_version, Access::ShaderRead});
        hero_scene_reads.push_back({static_visible_refs_version, Access::ShaderRead});
        hero_scene_reads.push_back({static_draw_args_version, Access::IndirectArgs});
        if (have_skinned_instances) hero_scene_reads.push_back({skin_cache_version, Access::ShaderRead});
        if (local_probe_outputs.record_resource)
            for (const auto version : {local_probe_outputs.radiance, local_probe_outputs.prefiltered,
                    local_probe_outputs.irradiance, local_probe_outputs.first_hit_distance, local_probe_outputs.records})
                if (version.resource) hero_scene_reads.push_back({version, Access::AllShaderRead});
        if (volumetric_outputs.constant_resource)
            for (const auto version : {volumetric_outputs.constants, volumetric_outputs.filtered_scattering.version,
                    volumetric_outputs.column_depth.version, volumetric_outputs.scattering.version,
                    volumetric_outputs.transmittance.version, volumetric_raw_hdr}) hero_scene_reads.push_back({version, Access::AllShaderRead});
        if (virtual_shadow_outputs.resources[0])
            for (const auto version : {virtual_shadow_outputs.metadata, virtual_shadow_outputs.page_table,
                    virtual_shadow_outputs.static_bank, virtual_shadow_outputs.dynamic_bank, virtual_shadow_outputs.draw_records})
                hero_scene_reads.push_back({version, Access::ShaderRead});
        if(virtual_shadow_outputs.root_static.resource)
            hero_scene_reads.push_back({virtual_shadow_outputs.root_static,Access::ShaderRead});
        if(virtual_shadow_outputs.root_dynamic.resource)
            hero_scene_reads.push_back({virtual_shadow_outputs.root_dynamic,Access::ShaderRead});
        if (local_shadow_outputs.resources[0])
            for (const auto version : {local_shadow_outputs.metadata, local_shadow_outputs.records,
                    local_shadow_outputs.static_bank, local_shadow_outputs.dynamic_bank, local_shadow_outputs.draw_records})
                hero_scene_reads.push_back({version, Access::ShaderRead});
        if (have_skinned_instances && previous_skin_frame_valid)
            hero_scene_reads.push_back({previous_skin_cache_version, Access::ShaderRead});
        if (ambient_outputs.ao.resource) hero_scene_reads.push_back({ambient_outputs.ao, Access::ShaderRead});
        if (ambient_outputs.contact.resource) hero_scene_reads.push_back({ambient_outputs.contact, Access::ShaderRead});
        for (const auto &page : geometry_pages)
        {
            hero_scene_reads.push_back({page.version, Access::VertexShaderRead});
        }
        hero_inputs.opaque_depth = {depth_version, owned_view.targets.depth.resource.Get()};
        hero_inputs.hdr = {hdr_version, opaque_hdr_resource};
        hero_inputs.composition_rect = hero_composition_rect;
        // Four occupied layers were common in the dense baseline. Query and
        // resolve only when a small source set can plausibly leave layers empty.
        hero_inputs.use_occupancy_queries = hero_occupancy_topology;
        hero_inputs.accumulation = {oit_accumulation_version, owned_view.targets.oit_accumulation.resource.Get()};
        hero_inputs.revealage = {oit_revealage_version, owned_view.targets.oit_revealage.resource.Get()};
        hero_inputs.accumulation_rtv = oit_accumulation_rtv;
        hero_inputs.revealage_rtv = oit_revealage_rtv;
        hero_inputs.opaque_readonly_dsv = readonly_dsv;
        hero_inputs.render_active = owned_view.state.render_active;
        hero_inputs.frame_slot = frame_slot;
        hero_inputs.completed = impl_->graph_queue_executor.Completed();
        hero_inputs.shader_heap = impl_->srv_heap.Get();
        hero_inputs.descriptor_source_heap = impl_->srv_cpu_heap.Get();
        hero_inputs.table_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        hero_inputs.table_cpu.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
            kHeroTransparencyDescriptorOffset) * impl_->srv_stride;
        hero_inputs.table_gpu = texture_table;
        hero_inputs.table_gpu.ptr += static_cast<UINT64>(kHeroTransparencyDescriptorOffset) * impl_->srv_stride;
        hero_inputs.descriptor_stride = impl_->srv_stride;
        hero_inputs.scene_reads = hero_scene_reads;
        hero_inputs.peel_reads = hero_peel_reads;
        hero_inputs.draw = [&](ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline,
                              HeroTransparencyPhase, std::uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE table) {
            bind_graphics(list);
            bind_scene_roots(list);
            list->SetGraphicsRootDescriptorTable(36, table);
            list->SetGraphicsRoot32BitConstant(6, kDefaultSceneDrawMaterial | 1u, 1);
            list->SetGraphicsRootShaderResourceView(34, dynamic_instance_base);
            list->SetGraphicsRootShaderResourceView(35, frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
            if (has_translucent_hero_binding(0) && archer_range.second != 0)
                if (auto drawn = impl_->DrawGeometry(list, frame_slot, pipeline, *bucket_pages[kArcherVisibilityBucket],
                        dynamic_geometry_passes[kArcherVisibilityBucket][0], frame.dynamic_draw_args.resource.Get(),
                        kArcherVisibilityBucket * kGeometryArgumentStride); !drawn)
                    throw std::runtime_error(std::string(drawn.Message()));
            for (std::size_t asset = 0; asset < monster_bucket_ranges.size(); ++asset)
            {
                if (!has_translucent_monster_bucket(asset)) continue;
                if (monster_bucket_ranges[asset].second == 0) continue;
                const auto bucket = kMonsterVisibilityBucketBegin + asset;
                if (auto drawn = impl_->DrawGeometry(list, frame_slot, pipeline, *bucket_pages[bucket],
                        dynamic_geometry_passes[bucket][0], frame.dynamic_draw_args.resource.Get(),
                        bucket * kGeometryArgumentStride); !drawn)
                    throw std::runtime_error(std::string(drawn.Message()));
            }
            if (impl_->static_gpu_scene)
            {
                list->SetGraphicsRootShaderResourceView(34, impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress());
                list->SetGraphicsRootShaderResourceView(35, frame.static_visible_refs.resource->GetGPUVirtualAddress());
                for (std::size_t lod=0;lod<3;++lod)
                {
                    const auto bucket=render_data::kMeadowWaterGeometry*3+lod;
                    if (static_environment_ranges[bucket].second==0) continue;
                    if(auto drawn=impl_->DrawGeometry(list,frame_slot,pipeline,*bucket_pages[bucket],
                        static_geometry_passes[bucket][0],frame.static_draw_args.resource.Get(),bucket*kGeometryArgumentStride);!drawn)
                        throw std::runtime_error(std::string(drawn.Message()));
                }
            }
        };
#if defined(HS_DEVELOPMENT_TOOLS)
        if (capture_hero_work)
        {
            static_assert(kGeometryArgumentStride == 24);
            if (impl_->rendered_submission_count == UINT64_MAX)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Hero work submission identity is exhausted.");
            hero_work_contributors.reserve(HeroWorkRetainedLayout::kMaxContributors);
            hero_work_bindings.reserve(HeroWorkRetainedLayout::kMaxContributors);
            const auto append_work = [&](std::size_t bucket, bool immutable, std::uint32_t candidates, std::string label) {
                const auto &page = *bucket_pages[bucket];
                HeroWorkContributorInput contributor;
                contributor.source.asset_id = page.source_asset.value;
                contributor.source.product = page.source_product;
                contributor.source.label = std::move(label);
                contributor.source.lod = page.metadata.lod;
                contributor.source.material = UINT32_MAX;
                if (!page.submeshes.empty())
                {
                    const auto material = page.submeshes.front().material_index;
                    if (std::ranges::all_of(page.submeshes, [&](const auto &part) { return part.material_index == material; }))
                        contributor.source.material = material;
                }
                contributor.source.candidate_instances = candidates;
                contributor.source.geometry_indices = page.index_count;
                if (!immutable)
                {
                    if (impl_->monster_basecolor.resource)
                    {
                        const auto desc = impl_->monster_basecolor.resource->GetDesc();
                        contributor.source.color_format = desc.Format;
                        contributor.source.color_mips = desc.MipLevels;
                    }
                    if (impl_->monster_ram.resource)
                    {
                        const auto desc = impl_->monster_ram.resource->GetDesc();
                        contributor.source.data_format = desc.Format;
                        contributor.source.data_mips = desc.MipLevels;
                    }
                }
                // Water's analytic closure has no corresponding character color/data pair.
                contributor.arguments = immutable ? static_draw_args_version : dynamic_draw_args_version;
                contributor.argument_resource = immutable ? frame.static_draw_args.resource.Get() : frame.dynamic_draw_args.resource.Get();
                contributor.argument_offset = bucket*kGeometryArgumentStride;
                hero_work_contributors.push_back(std::move(contributor));
                hero_work_bindings.push_back({bucket,immutable});
            };
            if (has_translucent_hero_binding(0) && archer_range.second)
                append_work(kArcherVisibilityBucket, false, static_cast<std::uint32_t>(archer_range.second), "archer");
            for (std::size_t asset=0;asset<monster_bucket_ranges.size();++asset)
                if (has_translucent_monster_bucket(asset) && monster_bucket_ranges[asset].second)
                    append_work(kMonsterVisibilityBucketBegin+asset,false,
                        static_cast<std::uint32_t>(monster_bucket_ranges[asset].second),
                        "monster"+std::to_string(asset < 6 ? asset : (asset-6)/2)+
                        "_lod"+std::to_string(asset < 6 ? 0 : 1+(asset-6)%2));
            if (impl_->static_gpu_scene)
                for (std::size_t lod=0;lod<3;++lod)
                {
                    const auto bucket = render_data::kMeadowWaterGeometry*3+lod;
                    if (static_environment_ranges[bucket].second)
                        append_work(bucket,true,static_cast<std::uint32_t>(static_environment_ranges[bucket].second),"water");
                }
            hero_inputs.work_contributors = hero_work_contributors;
            hero_inputs.submitted_frame = impl_->rendered_submission_count+1;
            hero_inputs.tick = snapshot.header.tick;
            hero_inputs.view_id = impl_->active_view_id.value;
            hero_inputs.direct_queue = impl_->graph_queue_executor.NativeQueue(QueueHint::Direct);
            hero_inputs.direct_timestamp_frequency = impl_->graph_queue_executor.TimestampFrequency(QueueHint::Direct);
            hero_inputs.work_draw = [&, bindings=hero_work_bindings](ID3D12GraphicsCommandList *list,
                ID3D12PipelineState *pipeline, HeroTransparencyPhase, std::uint32_t,
                D3D12_GPU_DESCRIPTOR_HANDLE table, std::uint32_t contributor, std::uint64_t diagnostic_uav) -> HeroWorkDrawCalls {
                const auto binding = bindings.at(contributor);
                bind_graphics(list);
                bind_scene_roots(list);
                list->SetGraphicsRootUnorderedAccessView(4,diagnostic_uav); // Separate T01 UAV; never particle arguments.
                list->SetGraphicsRootDescriptorTable(36,table);
                list->SetGraphicsRoot32BitConstant(6,kDefaultSceneDrawMaterial|1u,1);
                list->SetGraphicsRootShaderResourceView(34,binding.immutable
                    ? impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress() : dynamic_instance_base);
                list->SetGraphicsRootShaderResourceView(35,binding.immutable
                    ? frame.static_visible_refs.resource->GetGPUVirtualAddress() : frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
                const auto pass = binding.immutable ? static_geometry_passes[binding.bucket][0] : dynamic_geometry_passes[binding.bucket][0];
                if ((frame.geometry_draw_records_cpu.at(pass >> 2).flags & kGeometryMeshEligible) != 0)
                    throw std::runtime_error("Hero work capture requires the 24-byte indexed geometry command path.");
                auto *arguments = binding.immutable ? frame.static_draw_args.resource.Get() : frame.dynamic_draw_args.resource.Get();
                if (auto drawn = impl_->DrawGeometry(list,frame_slot,pipeline,*bucket_pages[binding.bucket],
                        pass,arguments,binding.bucket*kGeometryArgumentStride); !drawn)
                    throw std::runtime_error(std::string(drawn.Message()));
                return {1,0,0}; // Hero/water records explicitly use the indexed SceneVS path.
            };
        }
#endif
        const auto registered_hero = restored_graph
            ? owned_view.hero_transparency.BindRetainedLayers(impl_->graph, hero_inputs,
                pending_graph_layout.hero, hero_outputs)
            : owned_view.hero_transparency.RegisterLayers(impl_->graph, hero_inputs, hero_outputs,
                &pending_graph_layout.hero);
        if (!registered_hero) return registered_hero;
        pending_effects.transparency = static_cast<bool>(hero_outputs.depth.resource);
        if (pending_effects.transparency)
        {
            oit_accumulation_version = hero_outputs.accumulation;
            oit_revealage_version = hero_outputs.revealage;
            constants->lod_clock.w |= 4u;
        }
    }
    std::optional<PassBuilder> composite_pass;
    if (restored_graph)
    {
        composite_pass = impl_->graph.BindPass(pending_graph_layout.composite_pass.pass_index, 8);
        if (!composite_pass)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Composite pass does not match the retained graph plan.");
        composite_hdr_version = pending_graph_layout.composite_pass.output;
    }
    else
    {
    pending_graph_layout.composite_pass.pass_index = impl_->graph.PassCount();
    composite_pass = impl_->graph.AddPass(8, kRenderPassNames[8], QueueHint::Direct);
    read_probe(*composite_pass);
    read_volumetrics(*composite_pass);
    if (composite_gradient_read)
        composite_pass->Read(vfx_gradient_version, Access::AllShaderRead);
    composite_pass->Read(hdr_version, Access::ShaderRead);
    composite_pass->Read(distortion_vectors_version, Access::ShaderRead);
    composite_pass->Read(oit_accumulation_version, Access::ShaderRead);
    composite_pass->Read(oit_revealage_version, Access::ShaderRead);
    composite_pass->Read(gbuffer_metadata_version, Access::ShaderRead);
    composite_pass->Read(gbuffer_normal_version, Access::ShaderRead);
    composite_pass->Read(ribbon_history_version, Access::ShaderRead);
    composite_pass->Read(ribbon_previous_version, Access::ShaderRead);
    composite_pass->Read(ribbon_arguments_version, Access::IndirectArgs);
    composite_pass->Read(depth_version, Access::DepthShaderRead);
    composite_hdr_version = composite_pass->Write(composite_hdr_version, Access::RenderTarget);
    pending_graph_layout.composite_pass.output = composite_hdr_version;
    }
    composite_pass->SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0, 0, 0, 0};
        list->ClearRenderTargetView(composite_hdr_rtv, clear, 0, nullptr);
        draw_fullscreen(list, impl_->composite_pipeline.Get(), composite_hdr_rtv);
        if (flash_add_count != 0)
        {
            list->SetPipelineState(impl_->vfx_flash_pipeline.Get());
            list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() + flash_data_offset);
            list->DrawInstanced(6, flash_add_count, 0, 0);
        }
        if (ground_add_count != 0)
        {
            list->OMSetRenderTargets(1, &composite_hdr_rtv, FALSE, &readonly_dsv);
            list->SetPipelineState(impl_->ground_add_pipeline.Get());
            list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() + ground_ring_data_offset +
                        (typed_ground_spawns.size() - ground_add_count) *
                            sizeof(GpuParticleSpawnCommand));
            list->DrawInstanced(6, ground_add_count, 0, 0);
        }
        list->OMSetRenderTargets(1, &composite_hdr_rtv, FALSE, &readonly_dsv);
        draw_ribbons(list, false);
    });

    const auto raw_composite_version = composite_hdr_version;
    if (pending_effects.transparency)
    {
        const HeroTransparencyGuide background{composite_hdr_version, composite_hdr_resource};
        const auto registered_composition = restored_graph
            ? owned_view.hero_transparency.BindRetainedComposition(impl_->graph, background,
                pending_graph_layout.hero, hero_outputs)
            : owned_view.hero_transparency.RegisterComposition(impl_->graph, background,
                hero_outputs, &pending_graph_layout.hero);
        if (!registered_composition) return registered_composition;
    }
    if (hero_outputs.corrected.resource)
    {
        composite_hdr_version = hero_outputs.corrected;
        composite_hdr_resource = hero_outputs.corrected_resource;
        auto descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        descriptor.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
            kViewDescriptorOffset + 4) * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(1, descriptor, hero_outputs.corrected_srv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    record_graph_substage(cpu_timings.graph_hero_us);
    ScreenOverlayOutputs screen_overlay_outputs;
    ResourceVersion screen_overlay_gradient;
    if (!typed_screen.empty() && screen_intensity > 0)
    {
        const auto gradient = import_texture(impl_->vfx_gradient, Access::ShaderRead, "VfxScreenGradient");
        screen_overlay_gradient = impl_->graph.Version(gradient);
        ScreenOverlayInputs overlay_inputs;
        overlay_inputs.commands = typed_screen;
        overlay_inputs.gradient = screen_overlay_gradient;
        overlay_inputs.gradient_resource = impl_->vfx_gradient.resource.Get();
        overlay_inputs.render_active = owned_view.state.render_active;
        overlay_inputs.output_active = {owned_view.width, owned_view.height};
        const auto inverse_view = DirectX::XMMatrixInverse(nullptr, view);
        DirectX::XMStoreFloat3(&overlay_inputs.camera_right, inverse_view.r[0]);
        DirectX::XMStoreFloat3(&overlay_inputs.camera_up, inverse_view.r[1]);
        overlay_inputs.intensity = screen_intensity;
        overlay_inputs.frame_slot = frame_slot;
        overlay_inputs.completed = impl_->graph_queue_executor.Completed();
        const auto registered_overlay = restored_graph
            ? owned_view.screen_overlay.BindRetained(impl_->graph, overlay_inputs,
                pending_graph_layout.screen_overlay, screen_overlay_outputs)
            : owned_view.screen_overlay.RegisterMask(impl_->graph, overlay_inputs,
                screen_overlay_outputs, &pending_graph_layout.screen_overlay);
        if (!registered_overlay) return registered_overlay;
        pending_effects.screen_overlay = screen_overlay_outputs.active;
    }
    {
        auto target = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        target.ptr += (SIZE_T(frame_slot) * kTextureDescriptorCount + kVfxScreenReadDescriptorOffset) * impl_->srv_stride;
        if (screen_overlay_outputs.active)
        {
            constants->lod_clock.w |= 8u;
            impl_->device->CopyDescriptorsSimple(1, target, screen_overlay_outputs.reactive_srv,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        else
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC null_view{};
            null_view.Format = DXGI_FORMAT_R8_UNORM;
            null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            null_view.Texture2D.MipLevels = 1;
            impl_->device->CreateShaderResourceView(nullptr, &null_view, target);
        }
    }
    ExposureFrameRecordData exposure_records;
    exposure_records.record[0] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[]{0, 0, 0, 0};
        list->ClearRenderTargetView(scene_work_rtv, clear, 0, nullptr);
        draw_fullscreen(list, impl_->pre_exposure_pipeline.Get(), scene_work_rtv);
    };
    exposure_records.record[1] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->exposure_clear_pipeline.Get());
        list->Dispatch(1, 1, 1);
    };
    exposure_records.record[2] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->exposure_build_pipeline.Get());
        list->Dispatch((owned_view.render_width + 15) / 16, (owned_view.render_height + 15) / 16, 1);
    };
    exposure_records.record[3] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->exposure_reduce_pipeline.Get());
        list->Dispatch(1, 1, 1);
    };
    if (auto built = BuildExposureGraph(impl_->graph, composite_hdr_version, exposure_read_version,
            scene_work_version, histogram_version, exposure_write_version,
            std::move(exposure_records), pending_graph_layout.exposure, restored_graph); !built) return built;
    scene_work_version = pending_graph_layout.exposure.scene_work;
    histogram_version = pending_graph_layout.exposure.histogram;
    exposure_write_version = pending_graph_layout.exposure.exposure_write;

    ReconstructionGuidesOutputs reconstruction_outputs;
    const auto reconstruction_guide = [&](ResourceVersion version) {
        return ReconstructionGuideResource{version, version.resource
            ? static_cast<ID3D12Resource *>(impl_->graph.Resolve(version.resource)) : nullptr};
    };
    if (use_sr_reconstruction)
    {
        ReconstructionGuidesConstants guides_constants;
        guides_constants.inverse_unjittered_view_projection = constants->inverse_unjittered_view_projection;
        guides_constants.previous_unjittered_view_projection = constants->previous_unjittered_view_projection;
        guides_constants.camera_origin = {current_temporal_eye.x, current_temporal_eye.y,
            current_temporal_eye.z, 0};
        guides_constants.jitter_uv = constants->surface_jitter_uv;
        guides_constants.frame_info.x = owned_view.temporal_history_valid ? 1u : 0u;
        ReconstructionGuidesInputs guides_inputs;
        guides_inputs.depth = reconstruction_guide(depth_version);
        guides_inputs.motion = reconstruction_guide(gbuffer_motion_version);
        guides_inputs.metadata = reconstruction_guide(gbuffer_metadata_version);
        guides_inputs.surface_history = reconstruction_guide(gbuffer_surface_history_version);
        guides_inputs.composite_hdr = reconstruction_guide(composite_hdr_version);
        guides_inputs.opaque_hdr = reconstruction_guide(hdr_version);
        guides_inputs.exposure = reconstruction_guide(exposure_read_version);
        guides_inputs.transparency_revealage = reconstruction_guide(oit_revealage_version);
        guides_inputs.render_active = owned_view.state.render_active; guides_inputs.frame_slot = frame_slot;
        guides_inputs.completed = impl_->graph_queue_executor.Completed(); guides_inputs.reset_history = !owned_view.temporal_history_valid || new_session;
        if (pending_effects.transparency)
        {
            guides_inputs.transparent_depth = reconstruction_guide(hero_outputs.depth);
            guides_inputs.transparent_motion = reconstruction_guide(hero_outputs.motion);
            guides_inputs.transparent_history = reconstruction_guide(hero_outputs.history);
            guides_inputs.transparent_classification = reconstruction_guide(hero_outputs.classification);
            guides_inputs.transparent_metadata = reconstruction_guide(hero_outputs.metadata);
            guides_inputs.transparent_overflow = reconstruction_guide(hero_outputs.overflow);
            guides_inputs.transparent_transport = reconstruction_guide(hero_outputs.transport);
        }
        guides_inputs.transparent_guides_enabled = pending_effects.transparency;
        guides_inputs.screen_overlay_alpha = reconstruction_guide(screen_overlay_outputs.reactive);
        SdkReconstructionFrame sdk_frame;
        sdk_frame.identity = owned_view.sdk_frame_identity; sdk_frame.camera = owned_view.sdk_camera;
        const auto execute_reconstruction = [provider = owned_view.reconstruction_provider.get()](
                ID3D12GraphicsCommandList *commands, const SdkReconstructionFrame &frame) {
            return provider->Execute(commands, frame);
        };
        const bool external_reconstruction = owned_view.reconstruction_provider->UsesExternalQueue();
        const auto external_callback = [provider = owned_view.reconstruction_provider.get(), restored_graph](
                PassBuilder &pass, const SdkReconstructionFrame &packet, std::uint32_t slot,
                std::span<const ExternalResourceAccess> boundaries,
                std::vector<std::shared_ptr<const void>> owners) {
            return restored_graph
                ? provider->BindExternal(pass, packet, slot, boundaries, std::move(owners))
                : provider->RegisterExternal(pass, packet, slot, boundaries, std::move(owners));
        };
        const auto registered_reconstruction = restored_graph
            ? owned_view.reconstruction_guides.BindRetained(impl_->graph, guides_inputs,
                guides_constants, sdk_frame, execute_reconstruction,
                owned_view.reconstruction_plan.effective_provider.value, reconstruction_outputs,
                pending_graph_layout.reconstruction_guides,
                external_reconstruction
                    ? ReconstructionGuidesSystem::RegisterExternalReconstruction{external_callback}
                    : ReconstructionGuidesSystem::RegisterExternalReconstruction{})
            : owned_view.reconstruction_guides.Register(impl_->graph, guides_inputs,
                guides_constants, sdk_frame, execute_reconstruction,
                owned_view.reconstruction_plan.effective_provider.value, reconstruction_outputs,
                external_reconstruction
                    ? ReconstructionGuidesSystem::RegisterExternalReconstruction{external_callback}
                    : ReconstructionGuidesSystem::RegisterExternalReconstruction{},
                &pending_graph_layout.reconstruction_guides);
        if (!registered_reconstruction) return registered_reconstruction;
        pending_effects.reconstruction = true;
    }
    if (use_native_reconstruction)
    {
        auto output_descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        output_descriptor.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
            kTemporalTextureDescriptorOffset + 1) * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(1, output_descriptor, reconstruction_outputs.pre_exposed_srv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!impl_->config.bloom)
            tone_map_input = alternate_texture_table(kToneMapInputDescriptorOffset,
                reconstruction_outputs.pre_exposed_resource);
    }
    const auto tone_map_output_rtv = fxaa_identity && !impl_->config.outline
        ? display_linear_rtv : post_a_rtv;
    const auto outline_output_rtv = fxaa_identity ? display_linear_rtv : post_b_rtv;
    PostPassCallbacks post_callbacks;
    post_callbacks[0] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0, 0, 0, 0};
        list->ClearRenderTargetView(temporal_write_rtv, clear, 0, nullptr);
        list->ClearRenderTargetView(temporal_rtvs[1], clear, 0, nullptr);
        list->ClearRenderTargetView(temporal_rtvs[2], clear, 0, nullptr);
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        list->OMSetRenderTargets(static_cast<UINT>(temporal_rtvs.size()), temporal_rtvs.data(), FALSE, nullptr);
        list->SetPipelineState(impl_->temporal_pipeline.Get());
        list->SetGraphicsRootDescriptorTable(5, texture_table);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
    };

    const auto set_bloom_viewport = [&](ID3D12GraphicsCommandList *list, std::uint32_t width, std::uint32_t height) {
        const D3D12_VIEWPORT target_viewport{0.0f, 0.0f, static_cast<float>(width),
                                            static_cast<float>(height), 0.0f, 1.0f};
        const D3D12_RECT target_scissor{0, 0, static_cast<LONG>(width),
                                       static_cast<LONG>(height)};
        list->RSSetViewports(1, &target_viewport);
        list->RSSetScissorRects(1, &target_scissor);
    };
    post_callbacks[1] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0, 0, 0, 0};
        list->ClearRenderTargetView(bloom_half_rtv, clear, 0, nullptr);
        set_bloom_viewport(list, half_width, half_height);
        draw_fullscreen(list, impl_->bloom_extract_pipeline.Get(), bloom_half_rtv);
    };

    post_callbacks[2] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0, 0, 0, 0};
        list->ClearRenderTargetView(bloom_quarter_rtv, clear, 0, nullptr);
        set_bloom_viewport(list, quarter_width, quarter_height);
        draw_fullscreen(list, impl_->bloom_downsample_pipeline.Get(), bloom_quarter_rtv);
    };

    post_callbacks[3] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0, 0, 0, 0};
        list->ClearRenderTargetView(bloom_half_combined_rtv, clear, 0, nullptr);
        set_bloom_viewport(list, half_width, half_height);
        draw_fullscreen(list, impl_->bloom_upsample_pipeline.Get(), bloom_half_combined_rtv);
    };

    post_callbacks[4] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0, 0, 0, 0};
        list->ClearRenderTargetView(post_b_rtv, clear, 0, nullptr);
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        draw_fullscreen(list, impl_->bloom_pipeline.Get(), post_b_rtv);
    };

    post_callbacks[5] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        if (!owned_view.view_targets_initialized)
        {
            constexpr float clear[]{0, 0, 0, 0};
            list->ClearRenderTargetView(tone_map_output_rtv, clear, 0, nullptr);
            if (fxaa_identity && !impl_->config.outline)
                list->ClearRenderTargetView(post_a_rtv, clear, 0, nullptr);
        }
        draw_fullscreen_with_table(list, impl_->tone_map_pipeline.Get(), tone_map_output_rtv,
                                   impl_->config.bloom && !impl_->config.color_grading ? texture_table : tone_map_input);
    };

    post_callbacks[6] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        if (fxaa_identity ? !owned_view.view_targets_initialized : !impl_->config.bloom)
        {
            constexpr float clear[] = {0, 0, 0, 0};
            list->ClearRenderTargetView(outline_output_rtv, clear, 0, nullptr);
        }
        draw_fullscreen(list, impl_->outline_pipeline.Get(), outline_output_rtv);
    };

    post_callbacks[7] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        if (!fxaa_identity)
        {
            if (!owned_view.view_targets_initialized)
            {
                constexpr float clear[]{0, 0, 0, 0};
                list->ClearRenderTargetView(display_linear_rtv, clear, 0, nullptr);
            }
            draw_fullscreen_with_table(list, impl_->fxaa_pipeline.Get(), display_linear_rtv,
                                       impl_->config.outline ? texture_table : fxaa_input);
        }
        if (screen_overlay_outputs.active)
            owned_view.screen_overlay.Draw(list, frame_slot, display_linear_rtv);

    };

    post_callbacks[8] = [&](RenderPassContext &context) {
        if (presented_view && impl_->sdk_presentation.IsEnabled()) return;
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        if (initialize_offscreen_output)
        {
            list->DiscardResource(frame_output.Get(), nullptr);
            constexpr float clear[]{0, 0, 0, 0};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        }
        draw_fullscreen(list, impl_->ui_pipeline.Get(), rtv);
    };

    PostSubgraphInputs post_inputs{
        impl_->config.bloom, impl_->config.outline, hdr_version, scene_work_version,
        depth_version, gbuffer_normal_version, gbuffer_metadata_version,
        gbuffer_motion_version, gbuffer_surface_history_version, temporal_read_version, ui_version,
        temporal_normal_read_version, temporal_material_read_version, exposure_read_version,
        reconstruction_outputs.pre_exposed,
        {hero_outputs.depth, hero_outputs.motion, hero_outputs.history,
         hero_outputs.classification, hero_outputs.metadata, hero_outputs.overflow, hero_outputs.transport}};
    post_inputs.reactive_composite_hdr = composite_hdr_version;
    post_inputs.fxaa_identity = fxaa_identity;
    post_inputs.initialize_post_targets = !owned_view.view_targets_initialized;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (imgui_draw_data && !impl_->sdk_presentation.IsEnabled())
    {
        const auto record_devtools_ui = [&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            constexpr float clear[]{0, 0, 0, 0};
            list->ClearRenderTargetView(post_a_rtv, clear, 0, nullptr);
            list->OMSetRenderTargets(1, &post_a_rtv, FALSE, nullptr);
            ID3D12DescriptorHeap *heaps[]{impl_->imgui_heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            ImGui_ImplDX12_RenderDrawData(imgui_draw_data, list);
        };
        post_inputs.devtools_ui = [&, record_devtools_ui](RenderGraphBuilder &graph, ResourceVersion previous, ResourceVersion &encoded) {
            pending_graph_layout.devtools_ui.pass_index = graph.PassCount();
            auto pass = graph.AddPass(904, "DevTools UI", QueueHint::Direct);
            encoded = pass.Write(previous, Access::RenderTarget);
            pending_graph_layout.devtools_ui.output = encoded;
            pass.SetExecute(record_devtools_ui);
            return Result::Success();
        };
        if (restored_graph)
        {
            auto pass = impl_->graph.BindPass(pending_graph_layout.devtools_ui.pass_index, 904);
            if (!pass) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Devtools UI does not match the retained graph plan.");
            pass->SetExecute(record_devtools_ui);
        }
    }
#endif
    HdrPostOutputs hdr_processed;
    if (screen_overlay_outputs.active)
    {
        post_inputs.screen_overlay_alpha = screen_overlay_outputs.reactive;
        post_inputs.screen_overlay_upload = screen_overlay_outputs.upload;
        post_inputs.screen_overlay_gradient = screen_overlay_gradient;
    }
    auto reconstructed_hdr_resource = use_native_reconstruction ? reconstruction_outputs.pre_exposed_resource
        : owned_view.targets.temporal_history[temporal_write_index].resource.Get();
    const auto hdr_post_inputs = [&](ResourceVersion hdr, ID3D12Resource *resource) {
        HdrPostInputs inputs;
        inputs.hdr = {hdr, resource};
        inputs.depth = {depth_version, owned_view.targets.depth.resource.Get()};
        inputs.normal = {gbuffer_normal_version, surface_normal_resource};
        inputs.motion = {gbuffer_motion_version, owned_view.targets.gbuffer_motion.resource.Get()};
        inputs.surface_history = {gbuffer_surface_history_version, owned_view.targets.gbuffer_surface_history.resource.Get()};
        inputs.transparent_depth = {hero_outputs.depth, hero_outputs.depth_resource};
        inputs.transparent_motion = {hero_outputs.motion, hero_outputs.motion_resource};
        inputs.transparent_history = {hero_outputs.history, hero_outputs.history_resource};
        inputs.transparent_classification = {hero_outputs.classification, hero_outputs.classification_resource};
        inputs.transparent_guides_enabled = pending_effects.transparency;
        inputs.render_active = owned_view.state.render_active;
        inputs.output_active = {owned_view.width, owned_view.height};
        inputs.frame_slot = frame_slot;
        inputs.completed = impl_->graph_queue_executor.Completed();
        return inputs;
    };
    if (impl_->config.motion_blur != GraphicsMotionBlur::Off || impl_->config.depth_of_field)
        post_inputs.pre_bloom = [&](RenderGraphBuilder &graph, ResourceVersion hdr, ResourceVersion &processed) -> Result {
            HdrPostConstants post_constants;
            post_constants.inverse_jittered_view_projection = camera_relative_inverse_view_projection;
            post_constants.previous_unjittered_view_projection = constants->previous_unjittered_view_projection;
            DirectX::XMStoreFloat4(&post_constants.camera_forward, direction);
            post_constants.jitter_uv = constants->surface_jitter_uv;
            const float render_dt = owned_view.render_submission_time.time_since_epoch().count() == 0 ? 0.0f
                : std::chrono::duration<float>(cpu_recording_start - owned_view.render_submission_time).count();
            post_constants.frame_parameters = {render_dt,
                previous_view_valid && !new_session && render_dt > 0.0f ? 1.0f : 0.0f, 0, 0};
            const auto current_inputs = hdr_post_inputs(hdr, reconstructed_hdr_resource);
            const auto registered = restored_graph
                ? owned_view.hdr_post.BindRetainedMotionAndDoF(graph, current_inputs,
                    post_constants, pending_graph_layout.hdr_motion, hdr_processed)
                : owned_view.hdr_post.RegisterMotionAndDoF(graph, current_inputs,
                    post_constants, hdr_processed, &pending_graph_layout.hdr_motion);
            if (!registered) return registered;
            pending_effects.hdr_post = true;
            processed = hdr_processed.hdr;
            reconstructed_hdr_resource = hdr_processed.hdr_resource;
            auto descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            descriptor.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
                kTemporalTextureDescriptorOffset + 1) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(1, descriptor, hdr_processed.hdr_srv,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            return Result::Success();
        };
    if (impl_->config.color_grading)
        post_inputs.pre_tone_map = [&](RenderGraphBuilder &graph, ResourceVersion hdr, ResourceVersion &graded) -> Result {
            auto inputs = hdr_post_inputs(hdr, impl_->config.bloom
                ? owned_view.targets.post_b.resource.Get() : reconstructed_hdr_resource);
            inputs.sharpening = impl_->config.bloom ? 0.0f : impl_->config.sharpening;
            HdrPostOutputs output;
            const auto registered = restored_graph
                ? owned_view.hdr_post.BindRetainedGrade(graph, inputs,
                    pending_graph_layout.hdr_grade, output)
                : owned_view.hdr_post.RegisterGrade(graph, inputs, output,
                    &pending_graph_layout.hdr_grade);
            if (!registered) return registered;
            pending_effects.hdr_post = true;
            graded = output.hdr;
            tone_map_input = alternate_texture_table(kToneMapInputDescriptorOffset, output.hdr_resource);
            return Result::Success();
        };
    const PostSubgraphTargets post_targets{
        temporal_write_version, bloom_half_version, bloom_quarter_version,
        bloom_half_combined_version, post_a_version, post_b_version, back_buffer_version,
        temporal_normal_write_version, temporal_material_write_version, display_linear_version};
    if (restored_graph && post_inputs.pre_bloom)
    {
        ResourceVersion rebound;
        if (auto bound = post_inputs.pre_bloom(impl_->graph,
                pending_graph_layout.hdr_motion.inputs[0], rebound); !bound) return bound;
        if (rebound != pending_graph_layout.hdr_motion.output)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "HDR motion output does not match the retained graph plan.");
    }
    if (restored_graph && post_inputs.pre_tone_map)
    {
        ResourceVersion rebound;
        if (auto bound = post_inputs.pre_tone_map(impl_->graph,
                pending_graph_layout.hdr_grade.input, rebound); !bound) return bound;
        if (rebound != pending_graph_layout.hdr_grade.output)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "HDR grade output does not match the retained graph plan.");
    }
    PostSubgraphOutputs post_outputs;
    const auto built_post = restored_graph
        ? BindRetainedPostSubgraphs(impl_->graph, post_inputs, post_targets,
            pending_graph_layout.post, std::move(post_callbacks), post_outputs)
        : BuildPostSubgraphs(impl_->graph, post_inputs, post_targets,
            std::move(post_callbacks), post_outputs, &pending_graph_layout.post);
    if (!built_post) return built_post;
    temporal_write_version = post_outputs.temporal;
    bloom_half_version = post_outputs.bloom_half;
    bloom_quarter_version = post_outputs.bloom_quarter;
    bloom_half_combined_version = post_outputs.bloom_half_combined;
    post_a_version = post_outputs.post_a;
    post_b_version = post_outputs.post_b;
    back_buffer_version = post_outputs.back_buffer;
    temporal_normal_write_version = post_outputs.temporal_normal;
    temporal_material_write_version = post_outputs.temporal_material;
    display_linear_version = post_outputs.display_linear;

    if (presented_view && impl_->sdk_presentation.IsEnabled())
    {
        PresentationGuidesInputs input;
        input.display_linear = {display_linear_version, owned_view.targets.display_linear.resource.Get()};
        input.encoded_ui = {ui_version, ui_texture.resource.Get()};
        input.depth = use_native_reconstruction
            ? PresentationGuideResource{reconstruction_outputs.depth, reconstruction_outputs.depth_resource}
            : PresentationGuideResource{depth_version, owned_view.targets.depth.resource.Get()};
        input.motion = use_native_reconstruction
            ? PresentationGuideResource{reconstruction_outputs.motion, reconstruction_outputs.motion_resource}
            : PresentationGuideResource{gbuffer_motion_version, owned_view.targets.gbuffer_motion.resource.Get()};
        input.motion_contains_sky = use_native_reconstruction;
        input.transparent_guides_enabled = !use_native_reconstruction && pending_effects.transparency;
        input.transparent_depth = {hero_outputs.depth, hero_outputs.depth_resource};
        input.transparent_motion = {hero_outputs.motion, hero_outputs.motion_resource};
        input.transparent_classification = {hero_outputs.classification, hero_outputs.classification_resource};
        input.transparent_metadata = {hero_outputs.metadata, hero_outputs.metadata_resource};
        input.camera = owned_view.sdk_camera; input.render_active = owned_view.state.render_active;
        input.frame_slot = frame_slot; input.completed = impl_->graph_queue_executor.Completed();
        input.paper_white_nits = owned_view.display_state.paper_white_nits;
        input.peak_nits = owned_view.display_state.peak_nits;
#if defined(HS_DEVELOPMENT_TOOLS)
        if (impl_->config.devtools_visible)
            input.draw_devtools = [&](ID3D12GraphicsCommandList *list) -> Result {
                ID3D12DescriptorHeap *heaps[]{impl_->imgui_heap.Get()}; list->SetDescriptorHeaps(1, heaps);
                ImGui_ImplDX12_RenderDrawData(imgui_draw_data, list);
                return Result::Success();
            };
#endif
        PresentationGuidesOutputs output;
        const auto registered_presentation = restored_graph
            ? impl_->presentation_guides.BindRetained(impl_->graph, input, output,
                pending_graph_layout.presentation_guides)
            : impl_->presentation_guides.Register(impl_->graph, input, output,
                900, 901, 902, &pending_graph_layout.presentation_guides);
        if (!registered_presentation) return registered_presentation;
        pending_effects.presentation = true;
        const auto source = impl_->sdk_presentation.PresentationOwner() == SdkPresentationOwner::Fsr
            ? output.hudless : output.composite;
        auto &copy_layout = pending_graph_layout.sdk_copy;
        std::optional<PassBuilder> copy;
        if (restored_graph)
        {
            copy = impl_->graph.BindPass(copy_layout.pass_index, 903);
            if (!copy) return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "SDK backbuffer copy does not match the retained graph plan.");
            back_buffer_version = copy_layout.output;
        }
        else
        {
            copy_layout.pass_index = impl_->graph.PassCount();
            copy = impl_->graph.AddPass(903, "SDK Backbuffer Copy", QueueHint::Direct);
            copy->Read(source.version, Access::CopySource);
            back_buffer_version = copy->Write(back_buffer_version, Access::CopyDest);
            copy_layout.output = back_buffer_version;
        }
        copy->SetExecute([source, destination = frame_output](RenderPassContext &context) {
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyResource(destination.Get(), source.resource);
        });
        SdkPresentationFrame packet;
        packet.identity = owned_view.sdk_frame_identity; packet.camera = owned_view.sdk_camera;
        packet.minimum_luminance = owned_view.display_state.min_luminance_nits;
        if (reconstruction_outputs.owner) packet.owners.push_back(reconstruction_outputs.owner);
        const PresentationGuideResource presented_buffer{
            back_buffer_version, frame_output.Get()};
        const auto tagged = restored_graph
            ? impl_->presentation_guides.BindRetainedTagPass(impl_->graph, output,
                presented_buffer, packet, impl_->sdk_presentation,
                pending_graph_layout.presentation_tag)
            : impl_->presentation_guides.RegisterTagPass(impl_->graph, output,
                presented_buffer, packet, impl_->sdk_presentation, 904,
                &pending_graph_layout.presentation_tag);
        if (!tagged) return tagged;
    }

    if (import_binding_failed ||
        (restored_graph && import_cursor != pending_graph_layout.imports.size()))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Frame imports do not match the retained graph plan.");
    if (!restored_graph)
    {
    impl_->graph.SetFinalAccess(particles_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_history_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_previous_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_arguments_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_input_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_output_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_dead_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_counters_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(indirect_arguments_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(dynamic_instances_version, Access::ShaderRead);
    if (separate_previous_instance_scene)
        impl_->graph.SetFinalAccess(previous_instance_snapshot_version, Access::VertexShaderRead);
    if (have_skinned_instances)
        impl_->graph.SetFinalAccess(skin_cache_version, Access::ShaderRead);
    if (have_skinned_instances && previous_skin_frame_valid)
        impl_->graph.SetFinalAccess(previous_skin_cache_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(static_visible_refs_version, Access::UnorderedWrite);
    if (static_refs_buffer)
        impl_->graph.SetFinalAccess(static_refs_version, Access::AllShaderRead);
    impl_->graph.SetFinalAccess(static_draw_args_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(dynamic_visible_refs_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(dynamic_draw_args_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(static_rejected_refs_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(static_rejected_args_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(static_retest_refs_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(static_retest_args_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(dynamic_lod_version, Access::UnorderedWrite);
    if (static_lod_state)
        impl_->graph.SetFinalAccess(static_lod_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(previous_hzb_version, Access::ComputeRead);
    impl_->graph.SetFinalAccess(current_hzb_version, Access::ComputeRead);
    if (shadow_version.resource) impl_->graph.SetFinalAccess(shadow_version, Access::DepthWrite);
    impl_->graph.SetFinalAccess(depth_version, Access::DepthWrite);
    impl_->graph.SetFinalAccess(gbuffer_base_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_normal_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_metadata_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_material_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_motion_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_surface_history_version, Access::ShaderRead);
    for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
        impl_->graph.SetFinalAccess(material_sidecar_versions[plane], Access::ShaderRead);
    impl_->graph.SetFinalAccess(hdr_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(raw_hdr_version, Access::ShaderRead);
    for (const auto version : {subsurface_diffuse_version, environment_specular_version, indirect_diffuse_version})
        if (version.resource) impl_->graph.SetFinalAccess(version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(oit_accumulation_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(oit_revealage_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(post_a_version, Access::ShaderRead);
    for (const auto optional_output : {post_b_version, temporal_read_version, temporal_write_version,
            temporal_normal_read_version, temporal_normal_write_version,
            temporal_material_read_version, temporal_material_write_version})
        if (optional_output.resource) impl_->graph.SetFinalAccess(optional_output, Access::ShaderRead);
    impl_->graph.SetFinalAccess(scene_work_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(composite_hdr_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(raw_composite_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(display_linear_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(exposure_read_version, Access::AllShaderRead);
    impl_->graph.SetFinalAccess(exposure_write_version, Access::AllShaderRead);
    impl_->graph.SetFinalAccess(histogram_version, Access::UnorderedWrite);
    // Bloom intermediates have no retained export; their final consumers own their lifetime.
    if (upload_environment_stream)
        impl_->graph.SetFinalAccess(environment_stream_version, Access::AllShaderRead);
    if (upload_environment_geometry)
        for (const auto &upload : environment_geometry_uploads)
            impl_->graph.SetFinalAccess(upload.version, Access::Common);
    impl_->graph.SetFinalAccess(ui_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(back_buffer_version, Access::Present);
    }
    const auto timing_mode = full_timing_capture ? GpuTimingMode::Full :
        output_dependency_interval ? GpuTimingMode::Critical : GpuTimingMode::Off;
    const bool collect_gpu_timing = timing_mode != GpuTimingMode::Off;
    if (auto built = BuildFinalGraph(impl_->graph, back_buffer_version, output_dependency_interval,
            source_output_ready, pending_effects.presentation, pending_graph_layout.final, restored_graph); !built) return built;
    record_graph_substage(cpu_timings.graph_post_us);
    cpu_timings.graph_build_us = measure_stage();
    GraphExecutionStats prepare_cpu;
    if (auto prepared = impl_->graph.Prepare(transient_pool,
            impl_->config.capture_timing_stream ? &prepare_cpu : nullptr); !prepared) return prepared;
    if (!restored_graph)
    {
        pending_graph_layout.certified_imports = impl_->graph.AllImportsCertified();
        pending_graph_layout.passes.reserve(impl_->graph.PassCount());
        for (std::uint32_t index = 0; index < impl_->graph.PassCount(); ++index)
            pending_graph_layout.passes.push_back({impl_->graph.DeclaredPassId(index), index});
        pending_graph_layout.handle = impl_->graph.PreparedPlan();
    }
    cpu_timings.graph_prepare_us = measure_stage();
    cpu_timings.graph_prepare_validation_us = prepare_cpu.cpu_prepare_validation_us;
    cpu_timings.graph_prepare_info_key_us = prepare_cpu.cpu_prepare_info_key_us;
    cpu_timings.graph_prepare_compile_us = prepare_cpu.cpu_prepare_compile_us;
    cpu_timings.graph_prepare_transient_us = prepare_cpu.cpu_prepare_transient_us;
    frame.timestamp_passes.clear();
    frame.timestamp_frequencies.clear();
    frame.timestamp_pass_query_slots.clear();
    frame.timestamp_query_slots.clear();
    frame.timing_closure_slots.clear();
    frame.timing_closure_imports_ready = false;
    std::uint32_t timing_query_pair_count{};
    if (collect_gpu_timing)
    {
        frame.timestamp_query_slots.assign(impl_->graph.PassCount(), UINT32_MAX);
        timing_query_pair_count = timing_mode == GpuTimingMode::Critical ? 2u : impl_->graph.PassCount();
        if (auto prepared = impl_->PrepareFrameTimestamps(frame, timing_query_pair_count,
                timing_mode == GpuTimingMode::Full && impl_->copy_queue_timestamp_queries_supported);
            !prepared)
            return prepared;
        bool critical_begin_seen = false, critical_end_seen = false;
        for (std::uint32_t slot = 0; slot < impl_->graph.PassCount(); ++slot)
        {
            const auto pass_id = impl_->graph.CompiledPassId(slot);
            if (timing_mode == GpuTimingMode::Critical)
            {
                if (pass_id == 37)
                {
                    if (critical_begin_seen) return Result::Failure(ErrorCode::InvalidState,
                        "hs_renderer_d3d12", "Critical timing begin endpoint is duplicated.");
                    critical_begin_seen = true;
                }
                else if (pass_id == 38)
                {
                    if (critical_end_seen) return Result::Failure(ErrorCode::InvalidState,
                        "hs_renderer_d3d12", "Critical timing end endpoint is duplicated.");
                    critical_end_seen = true;
                }
            }
            const auto query_slot = timing_mode == GpuTimingMode::Critical
                ? (pass_id == 37 ? 0u : pass_id == 38 ? 1u : UINT32_MAX) : slot;
            if (query_slot == UINT32_MAX && timing_mode == GpuTimingMode::Critical) continue;
            frame.timestamp_query_slots[slot] = query_slot;
            const auto queue = impl_->graph.CompiledPassQueue(slot);
            const auto pass_name = timing_mode == GpuTimingMode::Full
                ? impl_->graph.CompiledPassName(slot) : std::string_view{};
            if (timing_mode == GpuTimingMode::Full && pass_name.empty())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "RenderGraph compiled an unnamed diagnostic pass.");
            const auto frequency = impl_->graph_queue_executor.TimestampFrequency(queue);
            frame.timestamp_passes.push_back({pass_id, timing_mode == GpuTimingMode::Full ? std::string(pass_name) : std::string{},
                static_cast<RenderQueue>(queue), 0, frequency != 0});
            frame.timestamp_frequencies.push_back(frequency);
            frame.timestamp_pass_query_slots.push_back(query_slot);
        }
        if (timing_mode == GpuTimingMode::Critical &&
            (!critical_begin_seen || !critical_end_seen || frame.timestamp_passes.size() != 2))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Critical timing endpoints are missing from the compiled graph.");
        if (timing_mode == GpuTimingMode::Full && !output_dependency_interval)
        {
            if (!pending_graph_layout.output_closure_cached)
            {
                pending_graph_layout.output_closure_slots = impl_->graph.OutputDependencyClosureSlots(38);
                pending_graph_layout.output_closure_cached = !pending_graph_layout.output_closure_slots.empty();
            }
            frame.timing_closure_slots = pending_graph_layout.output_closure_slots;
            frame.timing_closure_imports_ready = impl_->graph.OutputClosureImportsReady(
                frame.timing_closure_slots, impl_->graph_queue_executor.Completed());
        }
    }
    auto *distortion_resource = static_cast<ID3D12Resource *>(impl_->graph.Resolve(distortion_vectors));
    if (!distortion_resource) return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Prepared distortion resource is missing.");
    D3D12_RENDER_TARGET_VIEW_DESC distortion_rtv_description{};
    distortion_rtv_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    distortion_rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    impl_->device->CreateRenderTargetView(distortion_resource,&distortion_rtv_description,distortion_rtv);
    D3D12_SHADER_RESOURCE_VIEW_DESC distortion_srv_description{};
    distortion_srv_description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    distortion_srv_description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    distortion_srv_description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    distortion_srv_description.Texture2D.MipLevels = 1;
    auto distortion_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    distortion_cpu.ptr +=
        (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
         kVfxDistortionDescriptorOffset) * impl_->srv_stride;
    impl_->device->CreateShaderResourceView(distortion_resource,&distortion_srv_description,distortion_cpu);
    if (impl_->config.bloom)
    {
        // WaitForFrame completed this slot's prior GPU work before releasing claims;
        // the resolved pool resources and these descriptors stay alive through
        // the current submission and are rewritten only when this slot returns.
        auto bloom_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        bloom_cpu.ptr += (static_cast<SIZE_T>(frame_slot) * kTextureDescriptorCount +
                          kBloomTextureDescriptorOffset) * impl_->srv_stride;
        const std::array bloom_handles{bloom_half, bloom_quarter, bloom_half_combined};
        const std::array bloom_rtvs{bloom_half_rtv, bloom_quarter_rtv,
                                   bloom_half_combined_rtv};
        for (std::size_t index = 0; index < bloom_handles.size(); ++index)
        {
            auto *resource = static_cast<ID3D12Resource *>(
                impl_->graph.Resolve(bloom_handles[index]));
            if (!resource)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Prepared bloom resource is missing.");
            impl_->device->CreateRenderTargetView(resource, &distortion_rtv_description,
                                                  bloom_rtvs[index]);
            impl_->device->CreateShaderResourceView(resource, &distortion_srv_description,
                                                    bloom_cpu);
            bloom_cpu.ptr += impl_->srv_stride;
        }
    }
    // Original prefix/suffix descriptors live in the canonical CPU owner.
    // Publish the complete waited GPU bank after all services finalized it.
    impl_->PublishFrameDescriptors(frame_slot);
    GraphExecutionOptions execution_options;
    if (collect_gpu_timing)
    {
        constexpr std::array timing_queues{QueueHint::Direct, QueueHint::Compute, QueueHint::Copy};
        for (const auto hint : timing_queues)
        {
            if (hint == QueueHint::Copy &&
                (timing_mode != GpuTimingMode::Full || !impl_->copy_queue_timestamp_queries_supported))
                continue;
            const auto queue_index = static_cast<std::size_t>(hint);
            execution_options.timestamp_query_heaps[queue_index] = frame.timestamp_heaps[queue_index].Get();
            execution_options.timestamp_readbacks[queue_index] =
                frame.timestamp_readbacks[queue_index].resource.Get();
        }
        execution_options.timestamp_query_slots = frame.timestamp_query_slots.data();
        execution_options.timestamp_query_slot_count = frame.timestamp_query_slots.size();
    }
    execution_options.task_executor = &impl_->config.task_executor;
    frame.timing_prior_output_completion = owned_view.timing_source_output_completion;
    frame.timing_clock_before = {};
    if (collect_gpu_timing)
    {
        LARGE_INTEGER frequency{};
        if (timing_mode == GpuTimingMode::Full && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            for (std::size_t index = 0; index < frame.timing_clock_before.size(); ++index)
            {
                const auto hint = static_cast<QueueHint>(index);
                auto *native = impl_->graph_queue_executor.NativeQueue(hint);
                auto &anchor = frame.timing_clock_before[index];
                anchor.gpu_frequency = impl_->graph_queue_executor.TimestampFrequency(hint);
                anchor.cpu_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
                anchor.valid = native && anchor.gpu_frequency &&
                    SUCCEEDED(native->GetClockCalibration(&anchor.gpu, &anchor.cpu));
            }
    }
    frame.cpu_qpc_frequency = 0;
    if (impl_->config.capture_timing_stream)
    {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
            frame.cpu_qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
        if (timing_mode == GpuTimingMode::Full)
        {
            execution_options.cpu_pass_timings = frame.timestamp_passes.data();
            execution_options.cpu_pass_timing_count = frame.timestamp_passes.size();
        }
    }
    auto &submitted_completion = pending_effects.completion;
    if (auto marked = presented_view ? impl_->sdk_presentation.Marker(owned_view.sdk_frame_identity, SdkLatencyMarker::RenderSubmitStart) : Result::Success(); !marked)
        return marked;
    // A new submission may overwrite this view's HDR source. Failed partial
    // work must not expose it as the previously completed image.
    owned_view.hdr_capture_valid = false;
    owned_view.hdr_capture_source.Reset();
    cpu_timings.descriptors_us = measure_stage();
#if defined(HS_DEVELOPMENT_TOOLS)
    frame.timing_execute_begin_qpc = RendererDiagnosticsCounter();
#endif
    if (auto graph_result = impl_->graph_queue_executor.Execute(
            impl_->graph, frame_slot,
            impl_->enhanced ? BarrierMode::Enhanced : BarrierMode::Legacy,
            submitted_completion, &execution_options);
        !graph_result)
    {
        // Partial submission cannot publish CPU history or permit storage reuse.
        // Retain every resource until Drain establishes completion for teardown.
        frame.completion.Merge(submitted_completion);
        dynamic_scene.graph_completion.Merge(submitted_completion);
        if (separate_previous_instance_scene)
            previous_instance_snapshot->scene.graph_completion.Merge(submitted_completion);
        impl_->texture_feedback.MarkSubmitted(frame_slot, submitted_completion);
        if (submitted_completion.direct || submitted_completion.compute || submitted_completion.copy ||
            impl_->texture_streaming.HasStartedWork())
        {
            impl_->render_package_id = impl_->active_render_package_id;
            impl_->static_scene_fence_unreliable = true;
            impl_->frame_submission_faulted = true;
        }
        owned_view.temporal_history_valid = false;
        owned_view.hzb_history_valid = false;
        impl_->MarkClusterFrameUse(frame_slot, submitted_completion);
        for (auto &skin_frame : impl_->frames) skin_frame.skin_cache_valid = false;
        return graph_result;
    }
    // Execute accepted the shared particle mutation. Publish its ping-pong
    // owner before any fallible post-submit service can return an error.
    impl_->particles_initialized = true;
    if (impl_->render_scene_inputs)
    {
        impl_->particle_input_is_a = !impl_->particle_input_is_a;
        if (world_step_requested) impl_->last_particle_tick = stateful_target;
    }
    // Accepted GPU payloads and their CPU revision must survive later service/Present errors together.
    impl_->dynamic_scene_slots.Commit();
    dynamic_scene.graph_access = impl_->graph.FinalAccess(dynamic_instances_buffer);
    dynamic_scene.graph_queue = impl_->graph.FinalQueue(dynamic_instances_buffer);
    dynamic_scene.graph_completion.Merge(impl_->graph.ResourceCompletion(dynamic_instances_buffer));
    current_instance_snapshot->uploaded_revision = impl_->dynamic_scene_slots.PayloadRevision();
    current_instance_snapshot->slot_count = required_slots;
    current_instance_snapshot->surface_package_id = impl_->active_render_package_id;
    current_instance_snapshot->entity_surface_bytes = entity_surface_frame.Bytes().size();
    current_instance_snapshot->clock_epoch = snapshot.header.clock.epoch;
    current_instance_snapshot->clock_mode = snapshot.header.clock.mode;
    current_instance_snapshot->simulation_time = snapshot.header.simulation_time;
    current_instance_snapshot->initialized = current_instance_snapshot->submitted = true;
    impl_->canonical_instance_current = current_instance_snapshot;
    std::shared_ptr<Impl::CanonicalSkinFrame> current_skin_deformation;
    if (new_session)
        for (auto &skin_frame : impl_->frames) skin_frame.skin_cache_valid = false;
    frame.skin_cache_valid = have_skinned_instances;
    if (have_skinned_instances)
    {
        const auto current_skin_vertex_completion =
            impl_->graph.ResourceCompletion(skin_cache_buffer);
        const auto current_skin_bounds_completion =
            impl_->graph.ResourceCompletion(skin_bounds_buffer);
        frame.skin_cache_use_completion = current_skin_vertex_completion;
        frame.skin_cache_use_completion.Merge(current_skin_bounds_completion);
        frame.skin_cache_vertices.graph_access = impl_->graph.FinalAccess(skin_cache_buffer);
        frame.skin_cache_vertices.graph_queue = impl_->graph.FinalQueue(skin_cache_buffer);
        frame.skin_cache_vertices.graph_completion.Merge(current_skin_vertex_completion);
        frame.skin_cache_bounds.graph_access = impl_->graph.FinalAccess(skin_bounds_buffer);
        frame.skin_cache_bounds.graph_queue = impl_->graph.FinalQueue(skin_bounds_buffer);
        frame.skin_cache_bounds.graph_completion.Merge(current_skin_bounds_completion);
        if (previous_skin_frame_valid)
        {
            const auto previous_completion =
                impl_->graph.ResourceCompletion(previous_skin_cache_buffer);
            previous_skin_frame->skin_cache_use_completion.Merge(previous_completion);
            previous_skin_frame->skin_cache_vertices.graph_access =
                impl_->graph.FinalAccess(previous_skin_cache_buffer);
            previous_skin_frame->skin_cache_vertices.graph_queue =
                impl_->graph.FinalQueue(previous_skin_cache_buffer);
            previous_skin_frame->skin_cache_vertices.graph_completion.Merge(previous_completion);
        }
        frame.skin_cache_vertex_read = true;
        frame.skin_cache_bounds_read = true;
        frame.skin_cache_valid = true;
        current_skin_deformation = reuse_canonical_skin
            ? packet_skin_deformation : std::move(staged_current_skin_deformation);
        current_skin_deformation->skin_cache_vertices.graph_access =
            impl_->graph.FinalAccess(skin_cache_buffer);
        current_skin_deformation->skin_cache_vertices.graph_queue =
            impl_->graph.FinalQueue(skin_cache_buffer);
        current_skin_deformation->skin_cache_vertices.graph_completion.Merge(
            impl_->graph.ResourceCompletion(skin_cache_buffer));
        current_skin_deformation->skin_cache_bounds.graph_access =
            impl_->graph.FinalAccess(skin_bounds_buffer);
        current_skin_deformation->skin_cache_bounds.graph_queue =
            impl_->graph.FinalQueue(skin_bounds_buffer);
        current_skin_deformation->skin_cache_bounds.graph_completion.Merge(
            impl_->graph.ResourceCompletion(skin_bounds_buffer));
        current_skin_deformation->skin_cache_valid = true;
        current_skin_deformation->skin_cache_use_completion.Merge(frame.skin_cache_use_completion);
        // Capacity was reserved before Execute; publication and lease linking are
        // non-allocating once the GPU work has been accepted.
        if (!reuse_canonical_skin)
            impl_->canonical_skin_frames.push_back(current_skin_deformation);
        impl_->canonical_skin_current = current_skin_deformation;
        prepared_scene.skin_deformation = current_skin_deformation;
    }
    if (separate_previous_instance_scene)
    {
        auto &scene = previous_instance_snapshot->scene;
        scene.graph_access = impl_->graph.FinalAccess(previous_instance_snapshot_buffer);
        scene.graph_queue = impl_->graph.FinalQueue(previous_instance_snapshot_buffer);
        scene.graph_completion.Merge(impl_->graph.ResourceCompletion(previous_instance_snapshot_buffer));
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    frame.timing_execute_end_qpc = RendererDiagnosticsCounter();
#endif
    const auto completed_at_submit = impl_->graph_queue_executor.Completed();
    frame.timing_backlog_at_submit = frame.timing_prior_output_completion.direct &&
        frame.timing_prior_output_completion.direct > completed_at_submit.direct;
    owned_view.timing_source_output_completion = impl_->graph.ResourceCompletion(back_buffer);
    world_step_receipt.submitted = impl_->render_scene_inputs && world_step_requested;
    if (owned_view.ui_dirty[frame_slot])
    {
        auto &surface = owned_view.ui.surfaces[frame_slot];
        assert(surface.bitmap_valid && surface.bitmap_version != 0);
        surface.submitted_version = surface.bitmap_version;
        surface.copy_completion = impl_->graph.ResourceCompletion(ui);
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    cpu_stage_start = std::chrono::steady_clock::now();
#endif
    const auto graph_cpu = impl_->graph_queue_executor.LastExecutionStats();
    cpu_timings.graph_plan_us = graph_cpu.cpu_plan_us;
    cpu_timings.graph_record_us = graph_cpu.cpu_record_us;
    cpu_timings.graph_submit_us = graph_cpu.cpu_submit_us;
#if defined(HS_DEVELOPMENT_TOOLS)
    const auto submit_qpc = RendererDiagnosticsCounter();
#else
    const auto submit_qpc = impl_->auto_fg_cadence_tracking ? RendererDiagnosticsCounter() : 0;
#endif
    // A submitted package identity is consumed even if a later auxiliary view
    // or post-submit service fails. Only rendered submissions advance it.
    impl_->render_package_id = impl_->active_render_package_id;
    frame.submitted_frame_id = ++impl_->rendered_submission_count;
    frame.constants_submission_id = frame.submitted_frame_id;
    owned_view.last_submitted_frame_slot = frame_slot;
    owned_view.last_submitted_frame_id = frame.submitted_frame_id;
    frame.submitted_view_id = impl_->active_view_id.value;
    frame.timing_source_sequence = ++owned_view.timing_source_sequence;
    frame.timing_kind = GpuFrameTimingKind::Unavailable;
    frame.timing_mode = timing_mode;
    frame.timing_layout_capacity = timing_query_pair_count;
    if (collect_gpu_timing) ++frame.timing_layout_generation;
    if (collect_gpu_timing)
    {
        frame.timing_kind = output_dependency_interval ? GpuFrameTimingKind::OutputDependencyInterval :
            GpuFrameTimingKind::SourceOutputReady;
        frame.timing_generation = impl_->view_configuration_revision;
        frame.timing_resize_epoch = owned_view.state.resize_epoch;
        frame.timing_render_active = owned_view.state.render_active;
        frame.timing_quality_generation = owned_view.state.quality_generation;
        frame.timing_provider_generation = owned_view.state.provider_generation;
        frame.timing_main_view = presented_view;
        frame.timing_frame_generation_enabled = presented_view && impl_->sdk_presentation.IsEnabled();
    }
    frame.completion = submitted_completion;
    impl_->texture_streaming.MarkSubmitted(submitted_completion, impl_->streaming);
    impl_->texture_feedback.MarkSubmitted(frame_slot, submitted_completion);
    for (const auto &binding : texture_bindings)
        impl_->texture_streaming.MarkUse(binding.texture, impl_->graph.ResourceCompletion(binding.sampled.resource));
    if (pending_effects.adaptive)
    {
        if (auto completed = owned_view.adaptive_shading.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.adaptive = false;
    }
    if (pending_effects.decal_surface)
    {
        if (auto completed = owned_view.decal_surface.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.decal_surface = false;
    }
    if (auto marked = presented_view ? impl_->sdk_presentation.Marker(owned_view.sdk_frame_identity, SdkLatencyMarker::RenderSubmitEnd) : Result::Success(); !marked)
        return marked;
    if (pending_effects.presentation)
    {
        if (auto submitted = impl_->sdk_presentation.NotifySubmission(owned_view.sdk_frame_identity,
                impl_->graph_queue_executor.NativeQueue(QueueHint::Direct),
                impl_->graph_queue_executor.NativeFence(QueueHint::Direct),
                impl_->graph.ResourceCompletion(back_buffer_version.resource).direct); !submitted) return submitted;
        if (auto completed = impl_->presentation_guides.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.presentation = false;
    }
    if (pending_effects.local_probes)
    {
        if (auto completed = impl_->local_probes.CompleteSubmission(impl_->graph,
                submitted_completion, true, static_scene, pending_effects.probe_revisions); !completed) return completed;
        pending_effects.local_probes = false;
    }
    if (pending_effects.volumetrics)
    {
        if (auto completed = owned_view.volumetrics.CompleteSubmission(impl_->graph, submitted_completion, true); !completed) return completed;
        pending_effects.volumetrics = false;
        owned_view.previous_volumetric_camera = volumetric_camera;
    }
    if (pending_effects.screen_overlay)
    {
        if (auto completed = owned_view.screen_overlay.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.screen_overlay = false;
    }
    if (pending_effects.lights)
    {
        pending_effects.lights = false;
        if (auto completed = owned_view.light_list.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }


    if (pending_effects.scene_geometry)
    {
        pending_effects.scene_geometry = false;
        if (auto completed = impl_->scene_geometry.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }



    if (pending_effects.brixelizer)
    {
        if (auto completed = owned_view.brixelizer_gi.CompleteSubmission(impl_->graph,submitted_completion,true); !completed) return completed;
        pending_effects.brixelizer=false;owned_view.brixelizer_history_valid=true;
    }
    if (pending_effects.shadows)
    {
        if (auto completed = owned_view.virtual_shadows.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.shadows = false;
    }
    if (pending_effects.local_shadows)
    {
        if (auto completed = owned_view.local_shadows.CompleteSubmission(impl_->graph, submitted_completion, true); !completed) return completed;
        pending_effects.local_shadows = false;
    }
    if ((primary_sun && owned_view.virtual_shadows.AllocatedBytes() != 0) || owned_view.local_shadows.AllocatedBytes() != 0)
    {
        auto &shadow_owner = *impl_->owned_views.at(impl_->active_view_id.value);
        CommitShadowCasterStorage(owned_view.previous_shadow_casters,shadow_owner.scratch_shadow_casters,current_shadow_casters);
        if(static_shadow_batch && !static_shadow_batch->settled)
        {
            for(auto &caster:static_shadow_batch->records) AdvanceShadowCasterHistory(caster);
            static_shadow_batch->revision=++shadow_owner.static_shadow_caster_revision;
            static_shadow_batch->settled=true;
        }
        shadow_owner.static_shadow_casters=std::move(static_shadow_batch);
    }

    owned_view.render_submission_time = cpu_recording_start;
    if (pending_effects.hdr_post)
    {
        pending_effects.hdr_post = false;
        if (auto completed = owned_view.hdr_post.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.probe)
    {
        pending_effects.probe = false;
        if (auto completed = impl_->lighting_probe.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.ltc)
    {
        pending_effects.ltc = false;
        if (auto completed = impl_->ltc_lut.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.transparency)
    {
        pending_effects.transparency = false;
        if (auto completed = owned_view.hero_transparency.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.diffusion)
    {
        pending_effects.diffusion = false;
        if (auto completed = owned_view.subsurface.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.reflections)
    {
        pending_effects.reflections = false;
        if (auto completed = owned_view.screen_reflections.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.ambient)
    {
        pending_effects.ambient = false;
        if (auto completed = owned_view.ambient_occlusion.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.reconstruction)
    {
        pending_effects.reconstruction = false;
        if (auto completed = owned_view.reconstruction_guides.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (auto committed = impl_->CommitClusterFrameGraph(owned_view, frame_slot, cluster_graph,
            submitted_completion); !committed) return committed;
    if (auto committed = impl_->CommitHlodFrameGraph(frame_slot, hlod_graph, submitted_completion); !committed) return committed;
    pending_hlod.completed = true;
#if defined(HS_DEVELOPMENT_TOOLS)
    frame.cpu_recording_ms = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - cpu_recording_start).count();
    frame.cpu_recording_ms_valid = std::isfinite(frame.cpu_recording_ms) && frame.cpu_recording_ms > 0;
#else
    frame.cpu_recording_ms = 0;
    frame.cpu_recording_ms_valid = false;
#endif
    if (use_visibility) owned_view.visibility_targets_initialized = true;
    owned_view.decal_surface_active = decal_surface_outputs.active;
    for (const auto &[resource, handle] : imported_resources)
    {
        resource->graph_access = impl_->graph.FinalAccess(handle);
        resource->graph_queue = impl_->graph.FinalQueue(handle);
        resource->graph_completion = impl_->graph.ResourceCompletion(handle);
    }
    output_completion = impl_->graph.ResourceCompletion(back_buffer);
    if (initialize_dynamic_lod) impl_->CommitLodStateBuffer(dynamic_lod_state);
    if (initialize_static_lod) impl_->CommitLodStateBuffer(*static_lod_state);
    if (upload_environment_stream)
    {
        impl_->environment_stream.upload_completion =
            impl_->graph.ResourceCompletion(environment_stream_texture);
        if (!impl_->streaming.SubmitUpload(
                impl_->environment_stream.request, impl_->resource_generation_id,
                impl_->environment_stream.upload_completion))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Streamed environment texture upload completion was rejected.");
        if (!impl_->residency.Retire(
                impl_->environment_stream.upload_residency,
                impl_->environment_stream.upload_completion))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Streamed environment staging retirement was rejected.");
        impl_->environment_stream.upload_pending = false;
        impl_->environment_stream.upload_submitted = true;
        ++impl_->environment_stream.uploads;
    }
    if (upload_environment_geometry)
    for (const auto &upload : environment_geometry_uploads)
    {
        auto &stream = *upload.stream;
        stream.upload_completion = impl_->graph.ResourceCompletion(upload.buffer);
        if (!impl_->streaming.SubmitUpload(
                stream.request,
                impl_->resource_generation_id,
                stream.upload_completion))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Streamed environment geometry upload completion was rejected.");
        if (!impl_->residency.Retire(
                stream.upload_residency,
                stream.upload_completion))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Streamed environment geometry staging retirement was rejected.");
        stream.upload_pending = false;
        stream.upload_submitted = true;
        ++stream.uploads;
    }
    impl_->MarkEnvironmentStreamingUse(frame_slot, submitted_completion);
    transient_pool.SetClaimCompletion(submitted_completion);
    frame.timestamps_recorded = collect_gpu_timing;
    owned_view.ui_initialized[frame_slot] = true;
    owned_view.ui_dirty[frame_slot] = false;
    impl_->render_session_id = snapshot.header.session_id;
    owned_view.last_presented_index = output_index;
    owned_view.transient_textures_common = false;
    owned_view.material_sidecars_initialized = true;
    owned_view.view_targets_initialized = true;
    owned_view.exposure_initialized = true;
    owned_view.exposure_read_index = exposure_write_index;
    owned_view.exposure_sample_time = now;
    if (initialize_temporal_targets) owned_view.temporal_targets_initialized = true;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->config.validation)
    {
        const auto skin_gpu_dispatches = static_cast<std::size_t>(std::ranges::count_if(
            dynamic_skin_slots, [](std::uint32_t slot) {
                return slot != std::numeric_limits<std::uint32_t>::max();
            }));
        const auto skin_gpu_history_reads = static_cast<std::size_t>(std::ranges::count_if(
            dynamic_draw_refs, [&](const GpuStaticDrawRef &draw) {
                return (draw.mesh_flags & kCachedSkinHistoryFlag) != 0;
            }));
        if (!impl_->dynamic_instance_upload_log.is_open())
        {
            impl_->dynamic_instance_upload_log.open(
                impl_->config.artifact_directory / "dynamic_instance_upload.csv",
                std::ios::trunc);
            impl_->dynamic_instance_upload_log <<
                "frame,instances,uploaded_bytes,copy_regions,capacity_bytes,stable_slots,slot_count,anonymous_instances,created,updated,destroyed,skin_cache_active,skin_history_valid,skin_gpu_dispatches,skin_gpu_history_reads,dynamic_visibility_refs,hzb_history_input\n";
        }
        impl_->dynamic_instance_upload_log
            << (owned_view.frame_number + 1) << ',' << instance_count << ','
            << dynamic_instance_upload_bytes << ',' << dirty_instance_ranges.size() << ','
            << dynamic_scene_layout.total_bytes << ','
            << impl_->dynamic_scene_slots.ActiveCount() << ','
            << impl_->dynamic_scene_slots.SlotCount() << ','
            << impl_->dynamic_scene_slots.AnonymousCount() << ','
            << scene_changes.created << ',' << scene_changes.updated << ','
            << scene_changes.destroyed << ','
            << owned_view.skin_history.ActiveCount() << ','
            << owned_view.skin_history.HistoryValidCount() << ','
            << skin_gpu_dispatches << ',' << skin_gpu_history_reads << ','
            << dynamic_visibility_ref_count << ','
            << (owned_view.hzb_history_valid ? 1 : 0) << '\n';
    }
#endif
    if (static_scene && impl_->static_gpu_scene)
    {
        impl_->static_gpu_scene->last_completion =
            impl_->graph.ResourceCompletion(static_instances_buffer);
        impl_->static_gpu_scene->last_completion.Merge(
            impl_->static_gpu_scene->lod_states.states.graph_completion);
        if (static_refs_buffer)
        {
            auto &refs = *impl_->static_gpu_scene->refs;
            refs.storage.graph_access = impl_->graph.FinalAccess(static_refs_buffer);
            refs.storage.graph_queue = impl_->graph.FinalQueue(static_refs_buffer);
            refs.storage.graph_completion = impl_->graph.ResourceCompletion(static_refs_buffer);
            refs.last_completion.Merge(refs.storage.graph_completion);
            impl_->static_gpu_scene->last_completion.Merge(refs.last_completion);
            if (upload_static_refs)
            {
                refs.submitted = true;
                refs.upload_completion = refs.storage.graph_completion;
            }
        }
        if (upload_static_scene)
        {
            impl_->static_gpu_scene->submitted = true;
            impl_->static_gpu_scene->upload_completion =
                impl_->graph.ResourceCompletion(static_instances_buffer);
            ++impl_->static_scene_upload_count;
        }
    }
    // All graph batches and service receipts are accepted; Present does not own history.
    owned_view.previous_scene_origin = view_input.scene_origin;
    owned_view.previous_view_origin = current_view_origin;
    owned_view.previous_origins_valid = true;
    owned_view.previous_skin_deformation = current_skin_deformation;
    std::copy_n(constants->grass_benders, 32, owned_view.previous_grass_benders.begin());
    owned_view.previous_grass_state = {constants->camera_time.w,
        static_cast<float>(constants->grass_bender_count.x), 1.0f, 0.0f};
    owned_view.previous_instances = impl_->canonical_instance_current;
    owned_view.skin_history.Commit();
    auto &view_owner = *impl_->owned_views.at(impl_->active_view_id.value);
    view_owner.last_rendered_sample = snapshot.header;
    view_owner.has_rendered_sample = true;
    if (view_scene_changed)
    {
        ++view_owner.history_generation;
    }
    view_owner.scene_lease = static_scene;
    owned_view.hdr_capture_source = use_sr_reconstruction
        ? owned_view.reconstruction_guides.RetainPreExposedOutput()
        : Impl::RetainViewAllocation(owned_view.targets.temporal_history[temporal_write_index]);
    owned_view.hdr_capture_extent = {owned_view.width, owned_view.height};
    owned_view.hdr_capture_pre_exposed = true;
    owned_view.hdr_capture_submission_id = frame.submitted_frame_id;
    owned_view.hdr_capture_package_id = impl_->active_render_package_id;
    owned_view.hdr_capture_snapshot_tick = snapshot.header.tick;
    owned_view.hdr_capture_valid = static_cast<bool>(owned_view.hdr_capture_source.resource);
    owned_view.view_session_id = snapshot.header.session_id;
    if (!use_native_reconstruction) owned_view.temporal_read_index = temporal_write_index;
    owned_view.temporal_history_valid = temporal_enabled;
    owned_view.hzb_read_index = hzb_write_index;
    owned_view.hzb_history_valid = true;
    owned_view.previous_view_projection = constants->view_projection;
    owned_view.previous_unjittered_view_projection = constants->current_unjittered_view_projection;
    owned_view.previous_projection_depth = {lod_projection._33, lod_projection._34,
        lod_projection._43, lod_projection._44};
    owned_view.previous_jitter_uv = {constants->surface_jitter_uv.x, constants->surface_jitter_uv.y};
    owned_view.previous_geometry_page_versions.fill(0);
    for (const auto &record : frame.geometry_draw_records_cpu)
        if (record.instance_count != 0 && record.page_slot < kGeometryPageCapacity)
            owned_view.previous_geometry_page_versions[record.page_slot] = record.geometry_generation;
    if (impl_->cluster_runtime)
        for (const auto &page : impl_->cluster_runtime->frames[frame_slot].snapshot)
            if (page.resident && page.physical_page_slot < kGeometryPageCapacity)
                owned_view.previous_geometry_page_versions[page.physical_page_slot] = page.page_version;
    owned_view.temporal_previous_eye = {current_temporal_eye.x,
                                    current_temporal_eye.y,
                                    current_temporal_eye.z};
    owned_view.temporal_previous_fov = snapshot.camera.vertical_fov_degrees;
    if (view_input.camera_cut)
    {
        ++view_owner.history_generation;
        if (impl_->cluster_runtime && frame_slot < kClusterRuntimeFrames)
            impl_->cluster_runtime->frames[frame_slot].history_generation = view_owner.history_generation;
    }
    ++owned_view.frame_number;
    world_step_receipt.history_committed = true;
    cpu_timings.post_submit_us = measure_stage();
#if defined(HS_DEVELOPMENT_TOOLS)
    const auto present_begin_qpc = presented_view ? RendererDiagnosticsCounter() : 0;
#else
    constexpr std::uint64_t present_begin_qpc = 0;
#endif
    const auto presented = presented_view ? impl_->sdk_presentation.Present(impl_->swap_chain.Get(), owned_view.sdk_frame_identity,
        impl_->config.vsync ? 1 : 0, 0) : Result::Success();
    if (presented_view) impl_->native_input_phase = 0;
    if (!presented)
    {
        const auto device_reason = impl_->device->GetDeviceRemovedReason();
        if (FAILED(device_reason)) return impl_->CheckDevice(device_reason, "SDK Present");
        return presented;
    }
    frame.auto_fg_cadence_pending = presented_view && impl_->auto_fg_cadence_tracking;
    if (frame.auto_fg_cadence_pending)
    {
        LARGE_INTEGER frequency{};
        frame.auto_fg_cadence_submit_qpc = submit_qpc;
        frame.auto_fg_cadence_sequence = ++impl_->auto_fg_cadence_sequence;
        frame.auto_fg_cadence_qpc_frequency = QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0
            ? static_cast<std::uint64_t>(frequency.QuadPart) : 0;
        frame.auto_fg_cadence_validation = impl_->config.validation;
        frame.auto_fg_cadence_gpu_validation = impl_->config.gpu_validation;
    }
    if (presented_view)
        world_step_receipt.presented_ui_generation = impl_->explicit_frame->ui_packet_generation;

    cpu_timings.present_us = measure_stage();
#if defined(HS_DEVELOPMENT_TOOLS)
    impl_->diagnostics.RecordRenderedFrame(frame.submitted_frame_id, submit_qpc, present_begin_qpc,
        presented_view ? RendererDiagnosticsCounter() : 0, LastQueueStats(), impl_->DiagnosticsMemory(),
        impl_->config.validation, impl_->config.gpu_validation, presented_view, impl_->active_view_id.value);
#endif
    grass_history_attempt.succeeded = true;
    frame_result = {owned_view.frame_number, snapshot.header.tick,
#if defined(HS_DEVELOPMENT_TOOLS)
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureMouse,
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureKeyboard,
#else
                    false, false,
#endif
                    debug_command, debug_value, debug_secondary,
                    typed_vfx_event_count,
                    typed_vfx_persistent_count,
                    ground_ring_count + ground_add_count, flash_count, owner_mesh_count,
                    static_cast<std::uint32_t>(fresnel_draws.size()),
                    static_cast<std::uint32_t>(ribbon_outputs.size()), impl_->ribbon_state.DroppedSources(), constants->vfx_light_count.x, distortion_count, constants->vfx_decal_count.x,
                    impl_->static_scene_upload_count,
                    volumetric_outputs.constant_resource && impl_->config.vfx_quality == VfxQuality::High
                        ? static_cast<UINT>(typed_volumes.size()) : 0u};
    frame_result.identity = {snapshot.header.tick, impl_->active_render_package_id,
        impl_->active_view_id, presented_view ? owned_view.sdk_frame_identity.presentation_id : 0, 0, false};
    if (impl_->streamline_session)
        if (const auto token = impl_->streamline_session->NativeFrameToken(owned_view.sdk_frame_identity))
        {
            frame_result.identity.provider_token = *token;
            frame_result.identity.provider_token_known = true;
        }
    frame_result.rendered_view = view_input;
    frame_result.rendered_view.render_active = owned_view.state.render_active;
    frame_result.view_history_generation = impl_->owned_views.at(impl_->active_view_id.value)->history_generation;
    std::memcpy(frame_result.unjittered_view_projection.data(), &constants->current_unjittered_view_projection,
        sizeof(constants->current_unjittered_view_projection));
    if (impl_->render_scene_inputs) impl_->previous_ribbon_eye = ribbon_eye;
    impl_->CountValidationErrors();
    cpu_timings.diagnostics_us = measure_stage();
    // Current CPU records and graph callbacks can borrow saved logical-token
    // arrays during graph recording and synchronous completion. Publish or
    // evict a cold layout only after successful submission and presentation.
    if (!restored_graph && cold_graph_layout.handle)
    {
        auto layout_slot = std::ranges::find_if(impl_->graph_layouts, [&](const auto &layout) {
            return layout.handle == cold_graph_layout.handle && layout.early == cold_graph_layout.early;
        });
        if (layout_slot == impl_->graph_layouts.end())
            layout_slot = std::ranges::min_element(impl_->graph_layouts,
                {}, &FrameGraphLayout::last_used);
        cold_graph_layout.last_used = ++impl_->graph_layout_use_serial;
        *layout_slot = std::move(cold_graph_layout);
    }
    else if (restored_graph) source_candidate->last_used = ++impl_->graph_layout_use_serial;
    frame_result.cpu_timings = cpu_timings;
    frame_result.streamline_present = presented_view ? impl_->sdk_presentation.ActualPresentCount() : RendererStreamlinePresentCount{};
    return Result::Success();
}


} // namespace hs
