#include "renderer_impl.hpp"
#include "renderer_quality.hpp"
#include "renderer_instance_order.hpp"
#include <DirectXPackedVector.h>
#include "renderer_post_subgraphs.hpp"
#include "renderer_named_animation.hpp"
#include "renderer_brixelizer_scene.hpp"
#include "renderer_entity_surface.hpp"
#include <DirectXPackedVector.h>
#include <bit>
#include <cfloat>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

#include <numbers>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace hs
{

Result D3D12Renderer::Render(const RenderSnapshotExchange::ReadPair &snapshots,
                             std::span<const PresentationEvent> events,
                             std::span<const ParticleSpawnCommand> particle_spawns,
                             std::span<const VfxLineSpawnCommand> effect_lines,
                             std::span<const VfxEventInput> typed_vfx_events,
                             std::span<const VfxPersistentInput> typed_vfx_persistent,
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
    if (!impl_->explicit_frame)
    {
        const auto view = MainViewInput(snapshots.current.camera);
        RenderFrameInput input;
        input.views = std::span(&view, 1); input.snapshots = snapshots; input.events = events;
        input.vfx = {particle_spawns, effect_lines, typed_vfx_events, typed_vfx_persistent,
            typed_ground_spawns, typed_flash_spawns, typed_mesh_spawns, typed_fresnels, typed_ribbons,
            typed_lights, typed_distortions, typed_decals, typed_volumes, typed_screen, typed_entity_surfaces};
        input.devtools = devtools;
        impl_->legacy_frame_bridge = true;
        const auto result = Render(input, frame_result);
        impl_->legacy_frame_bridge = false;
        return result;
    }
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
    const bool wait_for_streaming = impl_->explicit_frame && impl_->explicit_frame->wait_for_streaming;
    impl_->asset_io.SetWaitForReads(wait_for_streaming);
    if (wait_for_streaming)
        if (auto waited = impl_->WaitForGpu(); !waited) return waited;
    for (const auto &input : typed_vfx_events)
        if (input.effect_handle == kInvalidVfxEffectHandle)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Typed VFX event has an invalid effect handle.");
    std::string vfx_error;
    if (!impl_->typed_vfx_persistent_state.Update(typed_vfx_persistent, vfx_error))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", vfx_error);
    const auto back_buffer_index = presented_view ? impl_->swap_chain->GetCurrentBackBufferIndex()
        : static_cast<std::uint32_t>(impl_->frame_number % kFrameCount);
    auto &frame = impl_->frames[back_buffer_index];
    auto &visibility_frame = impl_->visibility_frames[back_buffer_index];
    const bool use_visibility =
        impl_->config.surface_frontend == SurfaceFrontend::VisibilityBuffer;
    constexpr UINT kDefaultSceneDrawMaterial = 0u;
    if (auto result = impl_->WaitForFrame(frame); !result)
    {
        return result;
    }
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
    const auto cpu_recording_start = std::chrono::steady_clock::now();
    if (auto completed = impl_->ReadFrameGpuTimings(frame); completed.valid)
    {
        impl_->diagnostics.RecordCompleted(completed);
        if (completed.view_id == impl_->active_view_id.value)
        {
            if (completed.completed_frame_id > impl_->completed_gpu_timings.completed_frame_id)
                impl_->completed_gpu_timings = std::move(completed);
        }
        else if (const auto found = impl_->owned_views.find(completed.view_id); found != impl_->owned_views.end())
        {
            if (completed.completed_frame_id > found->second->completed_gpu_timings.completed_frame_id)
                found->second->completed_gpu_timings = std::move(completed);
        }
    }
    if (const auto &timing = impl_->completed_gpu_timings;
        timing.completed_frame_id > impl_->dynamic_resolution_sample_id)
    {
        impl_->dynamic_resolution_sample_id = timing.completed_frame_id;
        const DynamicResolutionConfig resolution_config{
            impl_->config.dynamic_resolution && !impl_->ResolveFeatures(impl_->config).software_gi_enabled,
            static_cast<double>(impl_->config.target_render_fps),
            impl_->config.dynamic_min_percent, impl_->config.dynamic_max_percent};
        auto selected = impl_->dynamic_resolution_controller.Select(impl_->view_state,
            resolution_config, {timing.critical_path_valid, false, timing.gpu_limited, timing.critical_path_ms});
        if ((impl_->reconstruction_provider && impl_->reconstruction_provider->Initialized()) ||
            impl_->ray_reconstruction.Mode() == RayReconstructionMode::DlssFused)
        {
            const auto &plan = impl_->reconstruction_plan;
            selected.active_extent.width = std::clamp(selected.active_extent.width,
                plan.input_minimum.width, plan.input_maximum.width);
            selected.active_extent.height = std::clamp(selected.active_extent.height,
                plan.input_minimum.height, plan.input_maximum.height);
            selected.changed = selected.active_extent != impl_->view_state.render_active;
        }
        if (selected.changed)
        {
            (void)UpdateViewState(impl_->view_state, impl_->view_state.output_extent,
                                  impl_->view_state.render_capacity, selected.active_extent);
            impl_->render_width = selected.active_extent.width;
            impl_->render_height = selected.active_extent.height;
            impl_->hzb_layout = MakeHzbLayout(impl_->render_width, impl_->render_height);
            impl_->hzb_history_valid = false;
        }
    }
    const auto completed_streaming = impl_->graph_queue_executor.Completed();
    if (auto result = impl_->UpdateResidencyBudget(); !result) return result;
    if (auto result = impl_->asset_io.CollectCancelled(impl_->streaming, impl_->residency); !result) return result;
    if (auto result = impl_->texture_feedback.Consume(back_buffer_index, completed_streaming,
            impl_->frame_number, impl_->texture_streaming); !result) return result;
    const auto active_features = impl_->ResolveFeatures(impl_->config);
    if (auto result = impl_->PumpEnvironmentGeometryStreaming(); !result)
    {
        return result;
    }
    if (auto result = impl_->PumpHlodDemand(); !result) return result;
    if (auto result = impl_->PumpClusterStreaming(back_buffer_index); !result) return result;
    if (auto result = impl_->CaptureClusterFrame(back_buffer_index); !result) return result;
    if (auto result = impl_->BeginGeometryDrawRecords(back_buffer_index); !result)
        return result;
    impl_->WriteGeometryPageDescriptors(back_buffer_index);
    frame.timestamps_recorded = false;
    struct PendingScenePreparation
    {
        RenderSceneExtractor &extractor;
        PersistentGpuSceneSlots &slots;
        SkinCache &skin;
        ~PendingScenePreparation()
        {
            extractor.Abort();
            slots.Abort();
            skin.Abort();
        }
    } pending_scene{impl_->dynamic_scene_extractor, impl_->dynamic_scene_slots,
                    impl_->skin_cache};
    if (!impl_->static_scene_fence_unreliable)
    {
        const auto completed = impl_->graph_queue_executor.Completed();
        std::erase_if(impl_->retired_static_gpu_scenes, [&](const auto &scene) {
            return scene->last_completion.IsSatisfiedBy(completed);
        });
        std::erase_if(impl_->retired_dynamic_instance_buffers, [&](const auto &buffer) {
            return buffer.completion.IsSatisfiedBy(completed);
        });
        if (impl_->static_gpu_scene && impl_->static_gpu_scene->submitted &&
            impl_->static_gpu_scene->upload.resource &&
            impl_->static_gpu_scene->upload_completion.IsSatisfiedBy(completed))
            impl_->static_gpu_scene->upload.Reset();
    }
    auto &transient_pool = impl_->transient_pools[back_buffer_index];
    transient_pool.ResetCompletedClaims(impl_->graph_queue_executor.Completed());

    auto snapshot = snapshots.has_current ? snapshots.current : RenderSnapshot{};
    if (!impl_->legacy_frame_bridge) snapshot.camera.vertical_fov_degrees = view_input.camera.vertical_fov_degrees;
    const bool scene_new_session = snapshot.header.session_id != impl_->render_session_id;
    const bool new_session = scene_new_session || snapshot.header.session_id != impl_->view_session_id;
    if (new_session)
    {
        if (scene_new_session) impl_->particles_initialized = false;
        impl_->temporal_history_valid = false;
        impl_->hzb_history_valid = false;
    }
    const bool same_previous_session = snapshots.has_previous &&
        snapshots.previous.header.session_id == snapshot.header.session_id;
    std::unordered_map<std::uint64_t, const RenderInstance *> previous_by_id;
    previous_by_id.reserve(snapshots.has_previous ? snapshots.previous.instances.size() : 0);
    if (same_previous_session)
        for (const auto &instance : snapshots.previous.instances)
            if (instance.stable_id != 0) previous_by_id.emplace(instance.stable_id, &instance);
    std::unordered_map<std::uint64_t, const AnimationPoseRef *> pose_by_id;
    pose_by_id.reserve(snapshot.poses.size());
    for (const auto &pose : snapshot.poses)
        if (pose.instance_index < snapshot.instances.size())
        {
            const auto stable_id = snapshot.instances[pose.instance_index].stable_id;
            if (stable_id != 0) pose_by_id.emplace(stable_id, &pose);
        }
    const auto now = impl_->render_package_time;
    if (scene_new_session || snapshot.header.tick != impl_->observed_snapshot_tick)
    {
        impl_->observed_snapshot_tick = snapshot.header.tick;
        impl_->snapshot_arrival = now;
    }
    const auto interpolation =
        impl_->config.interpolate && same_previous_session
            ? std::clamp(
                  std::chrono::duration<float>(now - impl_->snapshot_arrival).count() * 60.0f,
                  0.0f, 1.0f)
            : 1.0f;
    std::vector<RenderInstance> render_instances(snapshot.instances.begin(),
                                                  snapshot.instances.end());
    for (const auto &command : typed_entity_surfaces)
    {
        if (!command.has_retained_instance || std::ranges::any_of(render_instances,
                [&](const auto &instance) { return instance.stable_id == command.render_instance_id; })) continue;
        const auto &instance = command.retained_instance;
        if (instance.stable_id != command.render_instance_id ||
            instance.mesh < RenderMesh::BossFiveMinute || instance.mesh > RenderMesh::BossFinal ||
            instance.asset_id.value == 0 || instance.material_id.value == 0 ||
            (instance.animation_clip_id.value == 0 && !command.has_retained_animation_pose))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                "Entity surface retention requires its actual boss mesh and captured pose.");
        render_instances.push_back(instance);
        if (command.has_retained_animation_pose)
            pose_by_id.insert_or_assign(instance.stable_id, &command.retained_animation_pose);
    }
    std::vector<VfxEntitySurfaceCommand> admitted_entity_surfaces;
    for (const auto &command : typed_entity_surfaces)
        if (std::ranges::any_of(render_instances, [&](const auto &instance) {
                return instance.stable_id == command.render_instance_id &&
                    instance.mesh >= RenderMesh::BossFiveMinute && instance.mesh <= RenderMesh::BossFinal;
            })) admitted_entity_surfaces.push_back(command);
    EntitySurfaceFrame entity_surface_frame;
    if (auto prepared = entity_surface_frame.Build(admitted_entity_surfaces); !prepared) return prepared;
    if (!entity_surface_frame.Bytes().empty() &&
        (impl_->material_surface_layouts[kAuthoredMaterialNames.size() + 6].varying_mask & 16u) == 0)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Boss surface effects require the actual monster emission sidecar.");
    for (auto &instance : render_instances)
    {
        if (instance.animation_clip_id.value != 0)
        {
            if (instance.mesh != RenderMesh::Archer)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                    "Explicit named animation requires a loaded skeleton-bound character extension.");
            if (const auto previous = previous_by_id.find(instance.stable_id);
                previous != previous_by_id.end() &&
                previous->second->animation_clip_id.value == instance.animation_clip_id.value &&
                previous->second->animation_playback_rate == instance.animation_playback_rate &&
                previous->second->animation_time_seconds <= instance.animation_time_seconds)
                instance.animation_time_seconds = std::lerp(
                    previous->second->animation_time_seconds, instance.animation_time_seconds,
                    static_cast<double>(interpolation));
        }
        PopulateRenderInstanceRevisions(instance);
        if (instance.stable_id == 0 || instance.animation_clip_id.value != 0) continue;
        if (const auto pose = pose_by_id.find(instance.stable_id);
            pose != pose_by_id.end())
        {
            instance.revisions.pose = pose->second->revision != 0
                ? pose->second->revision
                : DeriveAnimationPoseRevision(*pose->second);
        }
    }
    // Replace the compatibility area body only when its authored fill is
    // actually present. Other area shapes and their gameplay warnings remain.
    if (impl_->config.slime_family_preview_count == 0)
        std::erase_if(render_instances, [&](const RenderInstance &instance) {
            return instance.mesh == RenderMesh::Area && instance.stable_id != 0 &&
                std::ranges::any_of(typed_ground_spawns, [&](const VfxGroundSpawnInput &spawn) {
                    return spawn.source_visual_kind != 0xff &&
                        spawn.stable_id == instance.stable_id &&
                        spawn.geometry.shape == VfxGroundShape::Circle &&
                        spawn.command.primitive == VfxPrimitive::LowFrequencyFill;
                });
        });
    if (impl_->config.slime_family_preview_count == 0)
        std::erase_if(render_instances, [&](const RenderInstance &instance) {
            return instance.mesh == RenderMesh::Area && instance.vfx_owner_id != 0 &&
                std::ranges::any_of(typed_ground_spawns, [&](const VfxGroundSpawnInput &spawn) {
                    return spawn.stable_id == instance.vfx_owner_id &&
                        ((spawn.source_visual_kind == 0xff &&
                          (spawn.geometry.shape == VfxGroundShape::LineBorder || spawn.geometry.shape == VfxGroundShape::ConeBorder)) ||
                         spawn.geometry.shape == VfxGroundShape::RingGapsBorder ||
                         spawn.geometry.shape == VfxGroundShape::RingGapsPreviewBorder ||
                         spawn.geometry.shape == VfxGroundShape::CirclePreviewBorder);
                });
        });
    if (impl_->config.slime_family_preview_count == 0)
        std::erase_if(render_instances, [&](const RenderInstance &instance) {
            return (instance.mesh == RenderMesh::PlayerProjectile || instance.mesh == RenderMesh::EnemyProjectile) &&
                std::ranges::any_of(typed_mesh_spawns, [&](const VfxOwnedMeshInput &head) {
                    return head.owner_id == instance.stable_id;
                });
        });
    if (const auto count = impl_->config.slime_family_preview_count; count != 0)
    {
        render_instances.clear();
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
            render_instances.push_back(preview);
            if (kind == 2)
            {
                RenderInstance projectile{};
                projectile.position = {x - 1.25f, 0.45f, z - 1.5f};
                projectile.scale = {.24f, .24f, .8f};
                projectile.color_rgba = 0xff60d060u;
                projectile.mesh = RenderMesh::EnemyProjectile;
                projectile.stable_id =
                    0x53504c4d00100000ull | static_cast<std::uint64_t>(i + 1);
                render_instances.push_back(projectile);
            }
        }
    }
    const auto is_monster = [](RenderMesh mesh) {
        return mesh >= RenderMesh::MonsterMelee && mesh <= RenderMesh::BossFinal;
    };
    constexpr std::uint32_t kCachedSkinFlag = 0x40000u;
    constexpr std::uint32_t kCachedSkinHistoryFlag = 0x80000u;
    if (impl_->config.monster_preview_asset < 6 && impl_->config.slime_family_preview_count == 0)
    {
        const auto preview_mesh = static_cast<RenderMesh>(
            static_cast<std::uint32_t>(RenderMesh::MonsterMelee) +
            impl_->config.monster_preview_asset);
        const auto found = std::ranges::find_if(
            render_instances, [&](const auto &source) { return source.mesh == preview_mesh; });
        if (found != render_instances.end())
        {
            auto preview = *found;
            preview.position = {};
            preview.yaw = 0.0f;
            render_instances.assign(1, preview);
        }
        else
        {
            render_instances.clear();
        }
    }
    if (impl_->config.environment_preview)
    {
        render_instances.clear();
        const auto add = [&](RenderMesh mesh, Float3 position, Float3 scale, std::uint32_t variant) {
            RenderInstance instance{};
            instance.mesh = mesh;
            instance.position = position;
            instance.scale = scale;
            instance.environment_variant = variant;
            instance.environment_seed = 0x4853454eu;
            render_instances.push_back(instance);
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
        snapshot.camera.target = {0,1,0};
        if (!impl_->config.preview_camera_override)
        {
            snapshot.camera.yaw_degrees = 0.0f;
            snapshot.camera.pitch_degrees = 22.0f;
            snapshot.camera.distance = 22.0f;
        }
    }
    const auto family_preview = impl_->config.slime_family_preview_count != 0;
    const auto monster_preview = impl_->config.monster_preview_asset < 6 || family_preview;
    const auto target_height = monster_preview
                                   ? (!family_preview && impl_->config.monster_preview_asset >= 3 ? 3.0f : 1.0f)
                               : impl_->config.character_preview ? 1.0f
                                                                : snapshot.camera.target.y;
    auto target = DirectX::XMVectorSet(family_preview ? 0.0f : snapshot.camera.target.x, target_height,
                                             family_preview ? 0.0f : snapshot.camera.target.z, 1.0f);
#if defined(HS_DEVELOPMENT_TOOLS)
    const auto camera_yaw = impl_->config.preview_camera_override
                                ? impl_->config.preview_camera_yaw
                            : impl_->config.character_preview ? impl_->preview_yaw
                                                              : snapshot.camera.yaw_degrees;
    const auto camera_pitch = impl_->config.preview_camera_override
                                  ? impl_->config.preview_camera_pitch
                              : impl_->config.character_preview ? impl_->preview_pitch
                                                                : snapshot.camera.pitch_degrees;
    const auto camera_distance = impl_->config.preview_camera_override
                                     ? impl_->config.preview_camera_distance
                                 : family_preview ? std::max(10.0f, std::sqrt(impl_->config.slime_family_preview_count * 3.0f) * 1.9f)
                                 : monster_preview
                                     ? (impl_->config.monster_preview_asset >= 3 ? 18.0f : 8.0f)
                                 : impl_->config.character_preview ? impl_->preview_distance
                                     : snapshot.camera.distance;
#else
    // Command-line captures also run in the validation build without the Debug UI.
    const auto camera_yaw = impl_->config.preview_camera_override
                                ? impl_->config.preview_camera_yaw
                                : snapshot.camera.yaw_degrees;
    const auto camera_pitch = impl_->config.preview_camera_override
                                  ? impl_->config.preview_camera_pitch
                                  : snapshot.camera.pitch_degrees;
    const auto camera_distance = impl_->config.preview_camera_override
                                     ? impl_->config.preview_camera_distance
                                     : family_preview ? std::max(10.0f, std::sqrt(impl_->config.slime_family_preview_count * 3.0f) * 1.9f)
                                     : snapshot.camera.distance;
#endif
    const auto yaw = DirectX::XMConvertToRadians(camera_yaw);
    const auto pitch = DirectX::XMConvertToRadians(camera_pitch);
    auto direction = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(std::cos(pitch) * std::sin(yaw), -std::sin(pitch),
                             std::cos(pitch) * std::cos(yaw), 0.0f));
    auto eye = DirectX::XMVectorSubtract(
        target, DirectX::XMVectorScale(direction, camera_distance));
    auto camera_up = DirectX::XMVectorSet(0, 1, 0, 0);
    if (!impl_->legacy_frame_bridge)
    {
        const auto scene_local = [&](RenderWorldPosition p) {
            return DirectX::XMVectorSet(static_cast<float>(p.x - view_input.scene_origin.x),
                static_cast<float>(p.y - view_input.scene_origin.y),
                static_cast<float>(p.z - view_input.scene_origin.z), 1);
        };
        eye = scene_local(view_input.camera.position);
        target = scene_local(view_input.camera.target);
        direction = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(target, eye));
        camera_up = DirectX::XMVectorSet(view_input.camera.up.x, view_input.camera.up.y, view_input.camera.up.z, 0);
    }
    DirectX::XMFLOAT3 lod_eye{};
    DirectX::XMStoreFloat3(&lod_eye, eye);
    const Float3 ribbon_eye{lod_eye.x, lod_eye.y, lod_eye.z};
    if (scene_new_session || impl_->rendered_submission_count == 0) impl_->previous_ribbon_eye = ribbon_eye;
    if (impl_->render_scene_inputs && !impl_->ribbon_state.Update(
            impl_->config.slime_family_preview_count == 0 ? typed_ribbons.sources : std::span<const VfxRibbonSourceInput>{},
            snapshot.header.tick, snapshot.header.session_id, typed_ribbons.catalog_generation,
            impl_->config.ribbon_history_points, interpolation, impl_->previous_ribbon_eye, vfx_error))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", vfx_error);
    const auto ribbon_updates = impl_->ribbon_state.Updates();
    const auto ribbon_outputs = impl_->ribbon_state.Outputs();
    std::unordered_set<std::uint64_t> ribbon_head_ids;
    for (const auto &source : typed_ribbons.sources) ribbon_head_ids.insert(source.owner_id);
    std::unordered_set<std::uint64_t> ribbon_projectile_ids;
    for (const auto &visual : snapshot.persistent_vfx)
        if (visual.kind == PersistentVfxKind::ProjectileHead && ribbon_head_ids.contains(visual.stable_id))
            ribbon_projectile_ids.insert(visual.projectile_owner_id);
    const auto ribbon_history_bytes = static_cast<UINT64>(impl_->ribbon_state.Capacity()) * kRibbonHistoryStride;
    const auto ribbon_args_bytes = std::max<std::size_t>(ribbon_outputs.size(), 1) * kRibbonArgumentStride;
    if (!impl_->ribbon_history.resource || impl_->ribbon_history.resource->GetDesc().Width != ribbon_history_bytes ||
        !impl_->ribbon_arguments.resource || impl_->ribbon_arguments.resource->GetDesc().Width < ribbon_args_bytes)
    {
        if (auto waited = impl_->WaitForGpu(); !waited) return waited;
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        const auto allocate = [&](AllocationResource &resource, UINT64 bytes) {
            resource.Reset();
            return impl_->CreateAllocation(resource, allocation,
                BufferDescription(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        if (!impl_->ribbon_history.resource || impl_->ribbon_history.resource->GetDesc().Width != ribbon_history_bytes)
        {
            if (auto allocated = allocate(impl_->ribbon_history, ribbon_history_bytes); !allocated) return allocated;
            if (auto allocated = allocate(impl_->ribbon_previous, ribbon_history_bytes); !allocated) return allocated;
        }
        if (!impl_->ribbon_arguments.resource || impl_->ribbon_arguments.resource->GetDesc().Width < ribbon_args_bytes)
            if (auto allocated = allocate(impl_->ribbon_arguments, ribbon_args_bytes); !allocated) return allocated;
    }
    const auto is_environment_mesh = [](RenderMesh mesh) {
        return mesh >= RenderMesh::TreeTrunk && mesh <= RenderMesh::Grass;
    };
    const auto environment_mesh_index = [](const RenderInstance &source) -> std::size_t {
        const auto type = static_cast<std::size_t>(source.mesh) - static_cast<std::size_t>(RenderMesh::TreeTrunk);
        constexpr std::array<std::size_t, 4> offsets{0, 3, 6, 10};
        constexpr std::array<std::size_t, 4> counts{3, 3, 4, 4};
        return offsets[type] + source.environment_variant % counts[type];
    };
    // Scene identity belongs to the unique input objects. Sorting and LOD
    // expansion produce draw references to those objects, including anonymous
    // fallback identities based on this canonical source index.
    auto canonical_instances = std::move(render_instances);
    for (auto &source : canonical_instances) PopulateRenderInstanceRevisions(source);
    RenderSceneDelta scene_delta;
    std::uint64_t scene_generation_after_submit{};
    const auto key_for_source = [](const RenderInstance &source, std::size_t index) {
        return RenderSceneInstanceKey{
            source.stable_id,
            source.stable_id == 0 ? static_cast<std::uint32_t>(index + 1) : 0u};
    };
    bool published_delta_matches_scene =
        snapshot.scene_delta.published &&
        snapshot.scene_delta.generation != 0 &&
        snapshot.scene_delta.instances.size() == canonical_instances.size() &&
        canonical_instances.size() == snapshot.instances.size() &&
        snapshot.scene_delta.created + snapshot.scene_delta.updated +
                snapshot.scene_delta.unchanged ==
            snapshot.scene_delta.instances.size();
    if (published_delta_matches_scene)
    {
        for (std::size_t index = 0; index < canonical_instances.size(); ++index)
        {
            const auto &item = snapshot.scene_delta.instances[index];
            const auto expected_key = key_for_source(canonical_instances[index], index);
            if (item.source_index != index || item.key != expected_key ||
                item.revisions != canonical_instances[index].revisions)
            {
                published_delta_matches_scene = false;
                break;
            }
        }
    }
    const bool published_delta_contiguous =
        published_delta_matches_scene &&
        (scene_new_session ? snapshot.scene_delta.base_generation == 0
                     : snapshot.scene_delta.base_generation ==
                           impl_->dynamic_scene_generation);
    if (published_delta_contiguous)
    {
        scene_delta.instances.assign(snapshot.scene_delta.instances.begin(),
                                     snapshot.scene_delta.instances.end());
        scene_delta.destroyed.assign(snapshot.scene_delta.destroyed.begin(),
                                     snapshot.scene_delta.destroyed.end());
        scene_delta.base_generation = snapshot.scene_delta.base_generation;
        scene_delta.generation = snapshot.scene_delta.generation;
        scene_delta.created = snapshot.scene_delta.created;
        scene_delta.updated = snapshot.scene_delta.updated;
        scene_delta.unchanged = snapshot.scene_delta.unchanged;
        if (!impl_->dynamic_scene_extractor.Adopt(canonical_instances, scene_new_session))
            return Result::Failure(
                ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Presentation scene delta contained invalid dynamic scene identities.");
    }
    else if (!impl_->dynamic_scene_extractor.Build(
                 canonical_instances, scene_delta, scene_new_session))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Dynamic render scene delta extraction failed.");
    }
    // A skipped Presentation generation is repaired by the full-snapshot
    // fallback above. Once this exact canonical scene has been submitted the
    // renderer can resume consuming incremental Presentation deltas.
    if (published_delta_matches_scene)
        scene_generation_after_submit = snapshot.scene_delta.generation;
    struct EnvironmentLod { float threshold{1.0f}; bool complement{}; };
    std::vector<RenderInstance> ordered_instances;
    std::vector<std::uint32_t> ordered_source_indices;
    std::vector<EnvironmentLod> ordered_lods;
    ordered_instances.reserve(canonical_instances.size());
    ordered_source_indices.reserve(canonical_instances.size());
    ordered_lods.reserve(canonical_instances.size());
    const auto foliage_density = impl_->QualityProfileFor(impl_->config.foliage_quality).foliage_density;
    const auto instance_ranges = BuildRenderInstanceOrder(
        canonical_instances, foliage_density, ordered_source_indices);
    for (const auto source_index : ordered_source_indices)
    {
        ordered_instances.push_back(canonical_instances[source_index]);
        ordered_lods.emplace_back();
    }
    constexpr std::size_t kEnvironmentVisibilityBucketCount = 42;
    constexpr std::size_t kArcherVisibilityBucket = kEnvironmentVisibilityBucketCount;
    constexpr std::size_t kMonsterVisibilityBucketBegin = kArcherVisibilityBucket + 1;
    constexpr std::size_t kGpuVisibilityBucketCount =
        kMonsterVisibilityBucketBegin + 6;
    std::array<std::pair<std::size_t, std::size_t>,
               kEnvironmentVisibilityBucketCount> environment_ranges{};
    const auto static_scene = impl_->config.slime_family_preview_count == 0 &&
                              impl_->config.monster_preview_asset >= 6 &&
                              !impl_->config.environment_preview
        ? snapshot.static_scene : nullptr;
    std::array<std::vector<GpuStaticDrawRef>, kEnvironmentVisibilityBucketCount>
        static_environment_buckets;
    std::vector<GpuStaticDrawRef> static_ground_bucket;
    std::vector<RenderInstance> hlod_static_instances;
    if (auto prepared = impl_->PrepareHlodStaticScene(static_scene, hlod_static_instances); !prepared) return prepared;
    const bool view_scene_changed = impl_->frame_number != 0 &&
        impl_->owned_views.at(impl_->active_view_id.value)->scene_lease != static_scene;
    if (view_scene_changed)
    {
        impl_->hzb_history_valid = false; impl_->temporal_history_valid = false;
        impl_->ambient_occlusion.ResetHistory(); impl_->screen_reflections.ResetHistory();
        impl_->ray_ambient_occlusion.ResetHistory(); impl_->ray_transport.ResetHistory();
        impl_->ray_shadows.ResetHistory(); impl_->volumetrics.ResetHistory();
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
        for (std::size_t index = 0; index < static_scene->instances.size(); ++index)
        {
            const auto &source = static_scene->instances[index];
            const auto instance_index = static_cast<std::uint32_t>(index);
            if (is_environment_mesh(source.mesh))
            {
                if (!RetainDecorativeFoliage(source, index, foliage_density)) continue;
                const auto base = environment_mesh_index(source) * 3;
                static_environment_buckets[base].push_back({instance_index, 0, 1.0f, static_generation});
            }
            else if (source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch)
                static_ground_bucket.push_back({instance_index, 0, 1.0f, static_generation});
            else
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                       "Unsupported mesh in static scene.");
        }
        if (!impl_->static_gpu_scene || impl_->static_gpu_scene->source != static_scene)
        {
            // Previous HZB/temporal history belongs to the old immutable static
            // scene generation. Never use it to reject geometry from a newly
            // published scene.
            if (impl_->static_gpu_scene)
            {
                impl_->hzb_history_valid = false;
                impl_->temporal_history_valid = false;
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
                const auto environment = is_environment_mesh(source.mesh);
                const float vertical_offset = environment ? 0.0f : source.scale.y * 0.5f;
                const auto mesh = static_cast<std::uint32_t>(source.mesh) |
                    (environment ? 0x10000u : 0u);
                const auto seed = source.mesh == RenderMesh::Ground ||
                                  source.mesh == RenderMesh::DirtPatch
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
    }
    if (static_scene && impl_->static_gpu_scene)
        if (auto prepared = impl_->PrepareLodStateBuffer(impl_->static_gpu_scene->lod_states,
                impl_->static_gpu_scene->layout.header.capacity); !prepared) return prepared;
    std::vector<GpuStaticDrawRef> static_draw_refs;
    std::array<std::vector<GpuClusterCandidate>, 14> cluster_candidates;
    std::array<ClusterCutInput, 14> cluster_inputs{};
    std::array<bool, 14> cluster_active{};
    std::array<std::pair<std::size_t, std::size_t>, 42> static_environment_ranges{};
    std::array<std::uint32_t, kGpuVisibilityBucketCount> static_output_begins{};
    std::size_t static_output_count{};
    for (std::size_t asset = 0; asset < static_environment_buckets.size(); asset += 3)
    {
        const auto begin = static_draw_refs.size();
        const auto &bucket = static_environment_buckets[asset];
        const auto ordinal = asset / 3;
        cluster_active[ordinal] = impl_->ClusterHierarchyReady(static_cast<std::uint32_t>(ordinal));
        if (cluster_active[ordinal])
        {
            const auto &catalog = impl_->cluster_runtime->catalogs[ordinal];
            auto &candidates = cluster_candidates[ordinal];
            candidates.reserve(bucket.size());
            const auto mesh = static_cast<std::uint32_t>(ordinal < 3 ? RenderMesh::TreeTrunk :
                ordinal < 6 ? RenderMesh::TreeCanopy : ordinal < 10 ? RenderMesh::Rock : RenderMesh::Grass);
            for (const auto &reference : bucket)
                candidates.push_back({reference.instance_index, reference.instance_generation,
                    static_cast<std::uint32_t>(ordinal), 15u, catalog.group_base, catalog.group_count,
                    mesh, 0u, 0.0f, reference.mesh_flags, catalog.first_root, 0u});
            cluster_inputs[ordinal].candidates = candidates;
            cluster_inputs[ordinal].max_instance_count = static_cast<std::uint32_t>(candidates.size());
            cluster_inputs[ordinal].constants.raster_record_base = UINT32_MAX;
        }
        static_draw_refs.insert(static_draw_refs.end(), bucket.begin(), bucket.end());
        for (std::size_t lod = 0; lod < 3; ++lod)
        {
            static_environment_ranges[asset + lod] = {begin, bucket.size()};
            static_output_begins[asset + lod] = static_cast<std::uint32_t>(static_output_count);
            static_output_count += bucket.size();
        }
    }
    const std::pair<std::size_t, std::size_t> static_ground_range{
        static_draw_refs.size(), static_ground_bucket.size()};
    static_draw_refs.insert(static_draw_refs.end(), static_ground_bucket.begin(),
                            static_ground_bucket.end());
    constexpr std::size_t kGpuVisibilityViewCount = 4; // Camera and three light cascades.
    if (static_draw_refs.size() > std::numeric_limits<std::uint32_t>::max())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Static draw reference index overflow.");
    std::array<GpuStaticVisibilityRange, kGpuVisibilityBucketCount> visibility_ranges{};
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
    std::array<GpuStaticVisibilityRange, kGpuVisibilityBucketCount>
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
    render_instances = std::move(ordered_instances);
    const auto previous_instance_snapshot = !new_session && !view_input.camera_cut ? impl_->previous_instances : nullptr;
    std::vector<GpuInstance> previous_gpu_instances;
    if (previous_instance_snapshot)
    {
        previous_gpu_instances = previous_instance_snapshot->instances;
        // PreviousVP accepts coordinates in the current scene basis. Preserve
        // last-rendered transforms, converting their old basis in double first.
        const auto old_origin = previous_instance_snapshot->scene_origin;
        for (auto &instance : previous_gpu_instances)
        {
            instance.position.x = static_cast<float>(double(instance.position.x) + (old_origin.x - view_input.scene_origin.x));
            instance.position.y = static_cast<float>(double(instance.position.y) + (old_origin.y - view_input.scene_origin.y));
            instance.position.z = static_cast<float>(double(instance.position.z) + (old_origin.z - view_input.scene_origin.z));
        }
    }
    std::array<std::pair<std::size_t, std::size_t>, 6> monster_ranges{};
    const auto actor_range = [&](std::size_t bucket) {
        return instance_ranges[bucket].second != 0 ? instance_ranges[bucket]
            : std::pair<std::size_t, std::size_t>{};
    };
    const auto archer_range = actor_range(kInstanceArcherBucket);
    const auto projectile_range = actor_range(kInstanceProjectileBucket);
    for (std::size_t asset = 0; asset < monster_ranges.size(); ++asset)
        monster_ranges[asset] = actor_range(kInstanceMonsterBucketBegin + asset);
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
    for (std::size_t asset = 0; asset < monster_ranges.size(); ++asset)
    {
        const auto [begin, count] = monster_ranges[asset];
        dynamic_output_begins[kMonsterVisibilityBucketBegin + asset] = static_cast<std::uint32_t>(begin);
        if (count == 0) continue;
        dynamic_visibility_ranges[kMonsterVisibilityBucketBegin + asset] = {
            static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(count),
            impl_->monster_assets[asset].index_count,
            static_cast<std::uint32_t>(kGpuVisibilityBucketCount)};
        dynamic_visibility_ref_count += count;
    }
    const bool new_persistent_tick =
        snapshot.header.tick != impl_->last_status_visual_tick;
    const auto environment_pipeline = [&](std::size_t asset, bool shadow_draw) {
        const auto &mesh = impl_->environment_meshes[asset];
        const bool masked = render_data::ClassifyEnvironmentMaterial(mesh.material) ==
            render_data::EnvironmentMaterialVariant::MaskedTwoSided;
        return shadow_draw
            ? (masked ? impl_->environment_material_shadow_pipelines[asset / 3].Get()
                      : impl_->environment_opaque_shadow_pipeline.Get())
            : (use_visibility ? impl_->environment_material_visibility_pipelines[asset / 3].Get()
                              : impl_->environment_material_scene_pipelines[asset / 3].Get());
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
            if (auto result = impl_->AddGeometryDrawRecord(back_buffer_index, page,
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
        const auto page_slot = bucket < 42 ? static_cast<std::uint32_t>(bucket)
            : bucket == kArcherVisibilityBucket ? kArcherGeometryPage
            : kMonsterGeometryPage + static_cast<std::uint32_t>(bucket - kMonsterVisibilityBucketBegin);
        const auto *page = impl_->FindGeometryPage(page_slot);
        if (!page)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Geometry bucket has no resident page or terminal fallback.");
        bucket_pages[bucket] = page;
        auto *pipeline = bucket < 42 ? environment_pipeline(bucket, false)
            : (use_visibility ? impl_->hero_visibility_pipeline.Get() : impl_->hero_scene_pipeline.Get());
        // Native depth peeling uses the indexed SceneVS. Use the same canonical
        // page/record for every slime pass when that pipeline is active.
        if (impl_->hero_transparency.AllocatedBytes() != 0 &&
            bucket >= kMonsterVisibilityBucketBegin && bucket < kMonsterVisibilityBucketBegin + 3)
            pipeline = nullptr;
        const auto mesh_id = bucket < 42
            ? static_cast<std::uint32_t>(bucket / 3 < 3 ? RenderMesh::TreeTrunk
                : bucket / 3 < 6 ? RenderMesh::TreeCanopy
                : bucket / 3 < 10 ? RenderMesh::Rock : RenderMesh::Grass)
            : bucket == kArcherVisibilityBucket ? static_cast<std::uint32_t>(RenderMesh::Archer)
            : static_cast<std::uint32_t>(RenderMesh::MonsterMelee) +
                  static_cast<std::uint32_t>(bucket - kMonsterVisibilityBucketBegin);
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
    for (std::size_t family = 0; family < 14; ++family)
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
        if (auto result = impl_->AddGeometryDrawRecord(back_buffer_index,
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
            if (auto result = impl_->AddGeometryDrawRecord(back_buffer_index,
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
    const auto static_leaf_ref_count = static_draw_refs.size();
    if (auto prepared = impl_->PrepareHlodFrame(back_buffer_index, static_draw_refs); !prepared) return prepared;
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
            if (bucket < 42)
            {
                material = (impl_->material_parameter_bases[bucket / 3] << 1) | 1u;
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
            if (!cluster_active[ordinal] || cluster_candidates[ordinal].empty()) continue;
            const auto count = std::max<std::uint64_t>(
                static_cast<std::uint64_t>(cluster_candidates[ordinal].size()) *
                    impl_->cluster_runtime->catalogs[ordinal].worst_cut_records,
                impl_->cluster_runtime->frames[back_buffer_index].cuts[ordinal].required_capacity);
            if (count > UINT32_MAX - raster_cursor)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Cluster raster record capacity overflows.");
            cluster_inputs[ordinal].constants.raster_record_base = raster_cursor;
            raster_cursor += static_cast<std::uint32_t>(count);
        }
        if (auto *hlod_view = impl_->ActiveHlodView(); hlod_view && hlod_view->frames[back_buffer_index].enabled)
            for (const auto &binding : hlod_view->frames[back_buffer_index].bindings)
                if ((binding.flags & kHlodProxyResident) != 0u)
                    if (auto result = add_raster_range(binding.geometry_record_id << 2u,
                        hlod_view->frames[back_buffer_index].buckets[binding.bucket_index].material_word); !result) return result;
        visibility_frame.raster_capacity = raster_cursor;
        visibility_frame.range_capacity = std::max<std::uint32_t>(1,
            static_cast<std::uint32_t>(visibility_frame.ranges_cpu.size()));
        if (auto result = EnsureVisibilityBuffers(impl_->device.Get(), impl_->allocator,
                visibility_frame); !result) return result;
    }
    std::vector<ParticleSpawnCommand> frame_particle_spawns;
    frame_particle_spawns.reserve(particle_spawns.size());
    if (impl_->config.slime_family_preview_count == 0)
    {
        frame_particle_spawns.insert(frame_particle_spawns.end(),
                                     particle_spawns.begin(), particle_spawns.end());
    }
    if (new_persistent_tick)
    {
        impl_->last_status_visual_tick = snapshot.header.tick;
        for (const auto &source : snapshot.instances)
        {
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
                    const auto authored = std::ranges::any_of(snapshot.persistent_vfx,
                        [&](const PersistentVfxVisual &visual) {
                            return visual.kind == kind &&
                                   visual.entity_render_id == source.stable_id &&
                                   std::ranges::any_of(typed_ground_spawns,
                                       [&](const VfxGroundSpawnInput &ground) {
                                           return ground.geometry.shape == VfxGroundShape::PolarRune &&
                                                  ground.stable_id == visual.stable_id &&
                                                  ground.source_visual_kind ==
                                                      static_cast<std::uint8_t>(kind);
                                       });
                        });
                    if (authored) return;
                }
                const auto cadence = status == StatusVisual::Bleed ? 6u
                                   : status == StatusVisual::Burn ? 3u
                                                                  : 1u;
                if ((snapshot.header.tick + source.stable_id) % cadence != 0) return;
                ParticleSpawnCommand command;
                command.sequence = source.stable_id ^ snapshot.header.tick ^
                                   static_cast<std::uint32_t>(status);
                command.tick = snapshot.header.tick;
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
                command.seed = static_cast<std::uint32_t>(command.sequence);
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
                frame_particle_spawns.push_back(command);
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
                std::ranges::any_of(typed_ground_spawns, [&](const VfxGroundSpawnInput &spawn) {
                    return spawn.geometry.shape == VfxGroundShape::LineBorder &&
                        spawn.stable_id == visual.stable_id &&
                        spawn.source_visual_kind == static_cast<std::uint8_t>(visual.kind);
                })) continue;
            const bool authored_exact_ring =
                impl_->config.slime_family_preview_count == 0 &&
                std::ranges::any_of(
                    typed_ground_spawns, [&visual](const VfxGroundSpawnInput &spawn) {
                        return spawn.geometry.shape == VfxGroundShape::Circle &&
                            spawn.command.primitive == VfxPrimitive::ExactRing &&
                            spawn.stable_id == visual.stable_id &&
                            spawn.source_visual_kind ==
                                static_cast<std::uint8_t>(visual.kind);
                    });
            auto position = visual.position;
            auto visual_yaw = visual.yaw;
            if (interpolation < 1.0f)
            {
                const auto previous = std::ranges::find_if(
                    snapshots.previous.persistent_vfx,
                    [&visual](const PersistentVfxVisual &candidate) {
                        return candidate.stable_id == visual.stable_id &&
                               candidate.kind == visual.kind;
                    });
                if (previous != snapshots.previous.persistent_vfx.end())
                {
                    position.x = std::lerp(previous->position.x, position.x, interpolation);
                    position.y = std::lerp(previous->position.y, position.y, interpolation);
                    position.z = std::lerp(previous->position.z, position.z, interpolation);
                    const auto yaw_delta = std::remainder(visual_yaw - previous->yaw,
                                                          2.0f * std::numbers::pi_v<float>);
                    visual_yaw = previous->yaw + yaw_delta * interpolation;
                }
            }
            ParticleSpawnCommand command;
            command.sequence = visual.stable_id ^ snapshot.header.tick;
            command.tick = snapshot.header.tick;
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
        if (spawn.geometry.gap_angles_radians.size() > std::numeric_limits<UINT>::max() - ground_gap_count)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Ground gap index overflow.");
        ground_gap_count += spawn.geometry.gap_angles_radians.size();
    }
    for (const auto &distortion : typed_distortions)
    {
        if (distortion.shape != VfxDistortionShape::GappedAnnulus) continue;
        if (distortion.gap_angles_degrees.size() > std::numeric_limits<UINT>::max() - ground_gap_count)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Distortion gap index overflow.");
        ground_gap_count += distortion.gap_angles_degrees.size();
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
    constexpr std::uint32_t kMaxDecals = 64;
    const auto decal_data_offset = (distortion_data_offset + sizeof(GpuDistortion) * kMaxDistortions + 255u) & ~std::size_t{255u};
    constexpr std::size_t kMaxFresnelShells = 32;
    if (typed_fresnels.size() > kMaxFresnelShells)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Too many Fresnel shell commands.");
    const auto fresnel_data_offset =
        (decal_data_offset + sizeof(GpuDecal) * kMaxDecals + 255u) & ~std::size_t{255u};
    const auto static_ref_data_offset =
        (fresnel_data_offset + sizeof(GpuFresnelShell) * kMaxFresnelShells + 255u) &
        ~std::size_t{255u};
    if (static_draw_refs.size() > std::numeric_limits<UINT>::max() ||
        static_draw_refs.size() >
            (std::numeric_limits<std::size_t>::max() - static_ref_data_offset) /
                sizeof(GpuStaticDrawRef))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Static draw reference overflow.");
    const auto dynamic_ref_data_offset =
        (static_ref_data_offset +
         sizeof(GpuStaticDrawRef) * std::max<std::size_t>(static_draw_refs.size(), 1) +
         255u) & ~std::size_t{255u};
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
    const auto previous_instance_data_offset =
        (retest_visibility_range_offset + sizeof(retest_visibility_ranges) + 255u) &
        ~std::size_t{255u};
    if (previous_gpu_instances.size() >
        (std::numeric_limits<std::size_t>::max() - previous_instance_data_offset) /
            sizeof(GpuInstance))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Previous dynamic instance upload overflow.");
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
    const auto archer_palette_data_offset =
        (previous_instance_data_offset + sizeof(GpuInstance) *
             std::max<std::size_t>(previous_gpu_instances.size(), 1) + 255u) &
        ~std::size_t{255u};
    if (archer_instance_count > 0 &&
        archer_palette_stride > std::numeric_limits<std::size_t>::max() /
            archer_instance_count)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Archer palette upload size overflow.");
    const auto archer_palette_bytes = archer_palette_stride * archer_instance_count;
    if (archer_palette_data_offset > std::numeric_limits<std::size_t>::max() -
            archer_palette_bytes)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Archer palette upload range overflow.");
    const auto required_upload_size = archer_palette_data_offset + archer_palette_bytes;
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
    if (!static_draw_refs.empty())
        std::memcpy(frame.mapped + static_ref_data_offset, static_draw_refs.data(),
                    sizeof(GpuStaticDrawRef) * static_draw_refs.size());
    std::memcpy(frame.mapped + visibility_range_offset, visibility_ranges.data(),
                sizeof(visibility_ranges));
    std::memcpy(frame.mapped + dynamic_visibility_range_offset,
                dynamic_visibility_ranges.data(),
                sizeof(dynamic_visibility_ranges));
    std::memcpy(frame.mapped + retest_visibility_range_offset,
                retest_visibility_ranges.data(), sizeof(retest_visibility_ranges));
    if (!previous_gpu_instances.empty())
        std::memcpy(frame.mapped + previous_instance_data_offset,
                    previous_gpu_instances.data(),
                    previous_gpu_instances.size() * sizeof(GpuInstance));
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
        frame.dynamic_visible_capacity = dynamic_visible_ref_capacity;
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
    if (auto result = impl_->RasterizeUi(back_buffer_index, snapshot.ui); !result)
    {
        return result;
    }
    const auto instance_count = render_instances.size();

    auto *constants = reinterpret_cast<FrameConstants *>(frame.mapped);
    std::vector<GpuInstance> next_draw_instances(instance_count);
    auto *instances = next_draw_instances.data();
    auto *gpu_particle_spawns =
        reinterpret_cast<GpuParticleSpawnCommand *>(frame.mapped + particle_spawn_data_offset);
    auto *gpu_particle_owners =
        reinterpret_cast<std::uint32_t *>(frame.mapped + particle_owner_data_offset);
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
        std::uint32_t geometry_pass{};
    };
    std::vector<FresnelDraw> fresnel_draws;
    fresnel_draws.reserve(typed_fresnels.size());
    std::unordered_set<std::uint64_t> fresnel_ids;
    for (std::size_t index = 0; index < typed_fresnels.size(); ++index)
    {
        const auto &source = typed_fresnels[index];
        const auto finite_transform = [](const VfxTransform &transform) {
            return std::ranges::all_of(transform, [](float value) { return std::isfinite(value); });
        };
        const bool crown = source.kind == VfxFresnelShellKind::BossCrown;
        const bool transition = source.kind == VfxFresnelShellKind::BossTransition;
        const bool player = source.kind == VfxFresnelShellKind::PlayerInvulnerable;
        if (source.render_instance_id == 0 || source.stable_id == 0 ||
            source.effect_handle == kInvalidVfxEffectHandle ||
            !fresnel_ids.insert(source.stable_id).second || (!crown && !transition && !player) ||
            !finite_transform(source.current_transform) ||
            !finite_transform(source.previous_transform) ||
            !std::isfinite(source.footprint_radius) || source.footprint_radius <= 0.0f ||
            !std::isfinite(source.source_progress) || source.source_progress < 0.0f ||
            source.source_progress >= 1.0f ||
            !std::isfinite(source.elapsed_seconds) || source.elapsed_seconds < 0.0f ||
            !std::isfinite(source.color.x) || source.color.x < 0.0f ||
            !std::isfinite(source.color.y) || source.color.y < 0.0f ||
            !std::isfinite(source.color.z) || source.color.z < 0.0f ||
            !std::isfinite(source.color.w) || source.color.w < 0.0f || source.color.w > 1.0f ||
            !std::isfinite(source.hdr) || source.hdr <= 0.0f ||
            source.gradient_row >= impl_->vfx_gradient_rows ||
            !std::isfinite(source.motion_rate_hz) || source.motion_rate_hz < 0.0f ||
            !std::isfinite(source.motion_amplitude) || source.motion_amplitude < 0.0f ||
            source.motion_amplitude > 1.0f ||
            !std::isfinite(source.motion_inset_fraction) ||
            source.motion_inset_fraction < 0.0f || source.motion_inset_fraction > 1.0f ||
            !std::isfinite(source.noise_amount) || source.noise_amount < 0.0f ||
            source.noise_amount > 1.0f ||
            (crown && (source.sector_count != 6 || !std::isfinite(source.rotation_hz) ||
                       source.rotation_hz >= 0.0f || source.fresnel_power != 0.0f ||
                       source.end_crack != 0 || source.noise_amount != 0.0f)) ||
            (transition && (source.sector_count != 0 || source.rotation_hz != 0.0f ||
                            !std::isfinite(source.fresnel_power) || source.fresnel_power <= 0.0f ||
                            source.end_crack != 1 || source.noise_amount != 0.0f)) ||
            (player && (source.sector_count != 0 || source.rotation_hz != 0.0f ||
                        !std::isfinite(source.fresnel_power) || source.fresnel_power <= 0.0f ||
                        source.end_crack != 0)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Invalid Fresnel shell command.");
        const auto match = std::ranges::find_if(render_instances, [&](const RenderInstance &instance) {
            return instance.stable_id == source.render_instance_id &&
                   (player ? instance.mesh == RenderMesh::Archer :
                    instance.mesh >= RenderMesh::BossFiveMinute && instance.mesh <= RenderMesh::BossFinal);
        });
        if (match == render_instances.end()) continue;
        const auto instance_index = static_cast<std::size_t>(match - render_instances.begin());
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
            : static_cast<const GeometryPageGpu &>(impl_->monster_assets[asset_index]);
        if (auto result = impl_->AddGeometryDrawRecord(back_buffer_index, page,
                static_cast<std::uint32_t>(instance_index), 1, kGeometryGpuSceneRefs, 0,
                static_cast<std::uint32_t>(render_instances[instance_index].mesh), geometry_pass); !result)
            return result;
        fresnel_draws.push_back({instance_index, asset_index, index, player, geometry_pass});
    }
    std::uint32_t owner_mesh_count{};
    if (impl_->config.slime_family_preview_count == 0)
        for (const auto &source : typed_mesh_spawns)
        {
            const auto finite = [](const Float3 &v) {
                return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
            };
            if (source.mesh_index == 0 || source.mesh_index > 7 ||
                source.gradient_row >= impl_->vfx_gradient_rows || !finite(source.position) ||
                !finite(source.previous_position) || !finite(source.velocity) ||
                !finite({source.color.x,source.color.y,source.color.z}) || !std::isfinite(source.color.w) ||
                !std::isfinite(source.radius) || source.radius <= 0 ||
                !std::isfinite(source.hdr) || source.hdr <= 0 ||
                !std::isfinite(source.fresnel) || source.fresnel < 0 || source.fresnel > 1 ||
                !std::isfinite(source.head_to_tail_ratio) || source.head_to_tail_ratio < 0 || source.head_to_tail_ratio >= 1 ||
                !std::isfinite(source.shell_thickness) || source.shell_thickness < 0)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid owner mesh command.");
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
        const auto finite = std::isfinite(source.position.x) &&
            std::isfinite(source.position.y) && std::isfinite(source.position.z) &&
            std::isfinite(source.color.x) && std::isfinite(source.color.y) &&
            std::isfinite(source.color.z) && std::isfinite(source.color.w) &&
            std::isfinite(source.size) && source.size > 0.0f &&
            std::isfinite(source.delay) && source.delay >= 0.0f &&
            std::isfinite(source.lifetime) && source.lifetime > 0.0f &&
            std::isfinite(source.hdr) && source.hdr > 0.0f &&
            std::isfinite(source.normalized_age) && source.normalized_age >= 0.0f &&
            source.normalized_age <= 1.0f && source.gradient_row < impl_->vfx_gradient_rows &&
            static_cast<std::uint32_t>(source.shape) <= static_cast<std::uint32_t>(VfxImpactShape::Spark) &&
            std::isfinite(source.direction.x) && std::isfinite(source.direction.y) && std::isfinite(source.direction.z) &&
            std::isfinite(source.aspect) && source.aspect > 0 &&
            (source.curve_row == 0xffffffffu || source.curve_row < 7) &&
            (source.mask_slice == 0xffffffffu || source.mask_slice < 12) &&
            std::isfinite(source.mask_strength) && source.mask_strength >= 0 && source.mask_strength <= 1 &&
            std::isfinite(source.initial_offset.x) && std::isfinite(source.initial_offset.y) && std::isfinite(source.initial_offset.z) &&
            std::isfinite(source.initial_velocity.x) && std::isfinite(source.initial_velocity.y) && std::isfinite(source.initial_velocity.z) &&
            std::isfinite(source.orbit_radius) && source.orbit_radius >= 0 &&
            std::isfinite(source.orbit_phase) && std::isfinite(source.orbit_rate) &&
            std::isfinite(source.drag) && source.drag >= 0 &&
            source.oit == (static_cast<std::uint32_t>(source.shape) >= 10 && static_cast<std::uint32_t>(source.shape) <= 12) &&
            source.fbm_octaves > 0 && source.fbm_octaves <= 4 &&
            std::isfinite(source.domain_warp) && source.domain_warp >= 0 &&
            std::isfinite(source.opacity_scale) && source.opacity_scale > 0 &&
            std::isfinite(source.motion_strength) && source.motion_strength >= 0 && source.motion_strength <= 1 &&
            std::isfinite(source.procedural_flow_speed) && source.procedural_flow_speed >= 0 &&
            std::isfinite(source.smoke_fps) && source.smoke_fps >= 0 &&
            (source.shape != VfxImpactShape::Smoke6Way ||
             (source.smoke_frame_count > 0 && source.smoke_first_frame < 64 &&
              source.smoke_frame_count <= 64 - source.smoke_first_frame && source.smoke_fps > 0));
        const auto spark_speed = std::hypot(source.initial_velocity.x,
                                            source.initial_velocity.y,
                                            source.initial_velocity.z);
        const auto spark_direction = std::hypot(source.direction.x,
                                                source.direction.y,
                                                source.direction.z);
        const auto spark_alignment = source.direction.x * source.initial_velocity.x +
            source.direction.y * source.initial_velocity.y +
            source.direction.z * source.initial_velocity.z;
        if (!finite || (source.shape == VfxImpactShape::Spark &&
            (source.oit || source.ground_base_anchor || source.aspect != 1.0f ||
             !std::isfinite(spark_speed) || spark_speed <= 0.0f ||
             !std::isfinite(spark_direction) || std::abs(spark_direction - 1.0f) > 0.001f ||
             spark_alignment < 0.999f * spark_speed)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Invalid authored flash command.");
        if (!source.oit) ++flash_add_count;
        auto &flash = gpu_flashes[flash_count++];
        flash = {};
        flash.position_lifetime_min = {source.position.x, source.position.y,
                                       source.position.z, source.ground_base_anchor ? 1.0f : 0.0f};
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
            if (preview && (!std::isfinite(geometry.progress) || geometry.progress < 0 || geometry.progress > 1 ||
                !std::isfinite(geometry.animation_phase) || geometry.animation_phase < 0 ||
                ((geometry.shape == VfxGroundShape::RingGapsTicks || geometry.shape == VfxGroundShape::CirclePreviewTicks) && geometry.tick_count == 0)))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored annulus preview timing.");
            if (annulus && (!std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0 ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= geometry.inner_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width < 0 ||
                ((geometry.shape == VfxGroundShape::RingGapsBorder || geometry.shape == VfxGroundShape::RingGapsPreviewBorder || geometry.shape == VfxGroundShape::CirclePreviewBorder) && geometry.edge_width == 0) ||
                (circle_preview && (geometry.inner_radius != 0 || !geometry.gap_angles_radians.empty() || geometry.gap_half_angle_radians != 0)) ||
                !std::isfinite(geometry.gap_half_angle_radians) || geometry.gap_half_angle_radians < 0 ||
                geometry.gap_half_angle_radians > std::numbers::pi_v<float> ||
                std::ranges::any_of(geometry.gap_angles_radians, [](float angle) { return !std::isfinite(angle); })))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored annulus geometry.");
            const float direction_length = std::hypot(geometry.direction.x, geometry.direction.z);
            if (safe_sector && (!std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0 ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= geometry.inner_radius ||
                !std::isfinite(geometry.half_angle_radians) || geometry.half_angle_radians <= 0 || geometry.half_angle_radians > std::numbers::pi_v<float> ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0 ||
                !std::isfinite(geometry.animation_phase) || geometry.spokes == 0 || geometry.rings == 0 ||
                !std::isfinite(direction_length) || direction_length < .999f || direction_length > 1.001f ||
                !std::isfinite(geometry.direction.y) || geometry.direction.y != 0))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored safe-sector marker.");
            if (hex && (!input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes != 6 || !std::isfinite(geometry.progress) ||
                geometry.progress < 0.0f || geometry.progress >= 1.0f ||
                (geometry.animation_phase != 0.0f && geometry.animation_phase != 1.0f) ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < geometry.outer_radius + geometry.edge_width ||
                (geometry.mask_slice != 0xffffffffu && geometry.mask_slice >= 12) ||
                !std::isfinite(geometry.mask_strength) || geometry.mask_strength < 0.0f ||
                geometry.mask_strength > 1.0f ||
                (geometry.mask_slice == 0xffffffffu && geometry.mask_strength != 0.0f)))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored hex constellation.");
            if (state_ring && (input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes < 3 || geometry.spokes > 64 ||
                geometry.rings > 1 ||
                !std::isfinite(geometry.progress) ||
                geometry.progress < 0.0f || geometry.progress > 1.0f ||
                !std::isfinite(geometry.animation_phase) || geometry.animation_phase < 0.0f ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0.0f ||
                geometry.inner_radius >= geometry.outer_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                std::abs(geometry.inner_radius -
                         std::max(0.0f, geometry.outer_radius - geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min <= 0.0f ||
                std::abs(source.start_size_min -
                         (geometry.outer_radius + geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.stretch) ||
                std::abs(source.stretch - geometry.outer_radius / source.start_size_min) > 0.001f ||
                geometry.mask_slice != 0xffffffffu || geometry.mask_strength != 0.0f ||
                !geometry.gap_angles_radians.empty()))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored state ring.");
            if (polar_rune && (input.additive || source.primitive != VfxPrimitive::ExactRing ||
                geometry.spokes != 6 || geometry.rings != 2 ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f || !std::isfinite(geometry.animation_phase) ||
                geometry.animation_phase < 0.0f ||
                !std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f ||
                !std::isfinite(geometry.inner_radius) || geometry.inner_radius < 0.0f ||
                geometry.inner_radius >= geometry.outer_radius ||
                !std::isfinite(geometry.edge_width) || geometry.edge_width <= 0.0f ||
                std::abs(geometry.inner_radius -
                         std::max(0.0f, geometry.outer_radius - geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.start_size_min) ||
                std::abs(source.start_size_min -
                         (geometry.outer_radius + geometry.edge_width)) > 0.0001f ||
                !std::isfinite(source.stretch) ||
                std::abs(source.stretch - geometry.outer_radius / source.start_size_min) > 0.001f ||
                geometry.mask_slice != 0xffffffffu || geometry.mask_strength != 0.0f ||
                !geometry.gap_angles_radians.empty()))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored polar rune.");
            const float authored_radius = geometry.outer_radius;
            const float authored_edge = geometry.edge_width;
            const float authored_extent = axial_fracture
                ? std::max(geometry.range * 0.5f + authored_edge,
                           std::max(authored_radius, authored_edge * 3.0f))
                : repeating_chevron
                    ? std::max(geometry.half_length, geometry.half_width) + authored_edge
                    : authored_radius + authored_edge;
            const float authored_direction_length = std::hypot(geometry.direction.x,
                                                                geometry.direction.z);
            const bool invalid_direction = (axial_fracture || repeating_chevron) &&
                (!std::isfinite(authored_direction_length) ||
                 authored_direction_length < 0.999f ||
                 authored_direction_length > 1.001f ||
                 !std::isfinite(geometry.direction.y) || geometry.direction.y != 0.0f);
            const bool invalid_chevron = repeating_chevron &&
                (!std::isfinite(geometry.half_width) || geometry.half_width <= 0.0f ||
                 !std::isfinite(geometry.half_length) || geometry.half_length <= 0.0f ||
                 !std::isfinite(geometry.spacing) || geometry.spacing <= 0.0f ||
                 geometry.spokes != 7);
            if (authored_additive_shape && (!input.additive ||
                source.primitive != VfxPrimitive::ExactRing ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f ||
                !std::isfinite(geometry.animation_phase) ||
                (!repeating_chevron && (!std::isfinite(authored_radius) ||
                                        authored_radius <= 0.0f)) ||
                !std::isfinite(authored_edge) || authored_edge <= 0.0f ||
                !std::isfinite(authored_extent) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < authored_extent ||
                (broken_hex && (!std::isfinite(geometry.gap_half_angle_radians) ||
                    geometry.gap_half_angle_radians <= 0.0f ||
                    geometry.gap_half_angle_radians >= std::numbers::pi_v<float> / 3.0f)) ||
                (broken_crown && (!std::isfinite(geometry.inner_radius) ||
                    geometry.inner_radius <= 0.0f ||
                    geometry.inner_radius >= authored_radius)) ||
                invalid_chevron || invalid_direction ||
                (axial_fracture && (!std::isfinite(geometry.range) ||
                    geometry.range <= 0.0f))))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored additive ground geometry.");
            if (boss_signature && (source.primitive != VfxPrimitive::ExactRing ||
                !std::isfinite(geometry.progress) || geometry.progress < 0.0f ||
                geometry.progress >= 1.0f || !std::isfinite(geometry.edge_width) ||
                geometry.edge_width <= 0.0f ||
                (geometry.shape == VfxGroundShape::Circle &&
                 (!std::isfinite(geometry.outer_radius) || geometry.outer_radius <= 0.0f)) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min < (geometry.shape == VfxGroundShape::ConeBorder
                    ? geometry.range : geometry.outer_radius) + geometry.edge_width))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored boss signature.");
            if ((geometry.shape != VfxGroundShape::Circle && !line && !cone && !annulus && !safe_sector && !hex && !authored_additive_shape && !state_ring && !polar_rune) ||
                ((line || cone) && (!std::isfinite(direction_length) || direction_length < 0.999f || direction_length > 1.001f ||
                          !std::isfinite(geometry.direction.y) || geometry.direction.y != 0.0f ||
                          (line && (!std::isfinite(geometry.half_width) || geometry.half_width <= 0.0f ||
                                    !std::isfinite(geometry.half_length) || geometry.half_length <= 0.0f)) ||
                          (cone && (!std::isfinite(geometry.range) || geometry.range <= 0.0f ||
                                    !std::isfinite(geometry.half_angle_radians) || geometry.half_angle_radians <= 0.0f ||
                                    geometry.half_angle_radians > std::numbers::pi_v<float>)) ||
                          !std::isfinite(geometry.edge_width) || geometry.edge_width < 0.0f ||
                          ((geometry.shape == VfxGroundShape::LineBorder || geometry.shape == VfxGroundShape::ConeBorder) && geometry.edge_width == 0.0f) ||
                          !std::isfinite(geometry.spacing) || geometry.spacing <= 0.0f ||
                          !std::isfinite(geometry.scroll))))
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored line geometry.");
            const auto finite_color = std::isfinite(source.start_color.x) &&
                std::isfinite(source.start_color.y) &&
                std::isfinite(source.start_color.z) &&
                std::isfinite(source.start_color.w);
            if (input.effect_handle == kInvalidVfxEffectHandle ||
                source.renderer != VfxRenderer::Ground ||
                (source.primitive != VfxPrimitive::ExactRing &&
                 source.primitive != VfxPrimitive::LowFrequencyFill) || source.count != 1 ||
                (input.additive && ((geometry.shape != VfxGroundShape::Circle && !hex && !boss_signature && !authored_additive_shape) ||
                                    source.primitive != VfxPrimitive::ExactRing)) ||
                (input.additive && geometry.shape == VfxGroundShape::Circle &&
                 geometry.animation_phase != 0.0f && !boss_signature) ||
                !std::isfinite(source.position.x) ||
                !std::isfinite(source.position.y) ||
                !std::isfinite(source.position.z) ||
                !std::isfinite(source.start_size_min) ||
                source.start_size_min <= 0.0f ||
                !std::isfinite(source.stretch) ||
                (!line && !cone && !annulus && !safe_sector && !axial_fracture &&
                 !repeating_chevron &&
                 source.primitive == VfxPrimitive::ExactRing &&
                 (source.stretch <= 0.0f || source.stretch >= 1.0f)) ||
                !finite_color || !std::isfinite(input.motion.rate_hz) ||
                !std::isfinite(input.motion.amplitude) ||
                !std::isfinite(input.motion.inset_fraction) ||
                input.motion.rate_hz < 0.0f || input.motion.amplitude < 0.0f ||
                input.motion.amplitude > 1.0f || input.motion.inset_fraction < 0.0f ||
                input.motion.inset_fraction > 1.0f ||
                !std::isfinite(input.hdr) || input.hdr < 0.0f ||
                input.gradient_row >= impl_->vfx_gradient_rows)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                       "Invalid authored ground command.");
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
                gpu_ring.modes.y = static_cast<std::uint32_t>(geometry.gap_angles_radians.size());
                for (float angle : geometry.gap_angles_radians) gpu_ground_gaps[gap_write_index++] = angle;
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
    constexpr auto particle_capacity = kParticleCount;
    std::uint32_t gpu_particle_spawn_count{};
    std::uint32_t total_particles_to_spawn{};
    for (const auto &source : frame_particle_spawns)
    {
        if (source.mesh_index > 7)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "VFX mesh selector exceeds atlas range");
        if (source.authored_ballistic && (source.renderer != VfxRenderer::Mesh || source.mesh_index == 0 || source.count != 1 ||
            !std::isfinite(source.position.x) || !std::isfinite(source.position.y) || !std::isfinite(source.position.z) ||
            !std::isfinite(source.lifetime_min) || !std::isfinite(source.lifetime_max) || source.lifetime_min <= 0 || source.lifetime_max < source.lifetime_min ||
            !std::isfinite(source.start_size_min) || !std::isfinite(source.start_size_max) || source.start_size_min <= 0 || source.start_size_max < source.start_size_min ||
            !std::isfinite(source.end_size_min) || !std::isfinite(source.end_size_max) || source.end_size_min < 0 || source.end_size_max < source.end_size_min ||
            !std::isfinite(source.start_color.x) || !std::isfinite(source.start_color.y) || !std::isfinite(source.start_color.z) || !std::isfinite(source.start_color.w) ||
            !std::isfinite(source.end_color.x) || !std::isfinite(source.end_color.y) || !std::isfinite(source.end_color.z) || !std::isfinite(source.end_color.w) ||
            !std::isfinite(source.rotation_min) || !std::isfinite(source.rotation_max) || !std::isfinite(source.angular_velocity_min) || !std::isfinite(source.angular_velocity_max) ||
            !std::isfinite(source.authored_ground_y + source.authored_collision_radius) ||
            !std::isfinite(source.authored_initial_velocity.x) || !std::isfinite(source.authored_initial_velocity.y) || !std::isfinite(source.authored_initial_velocity.z) ||
            !std::isfinite(source.authored_drag) || source.authored_drag < 0 ||
            !std::isfinite(source.authored_bounce) || source.authored_bounce < 0 || source.authored_bounce > 1 ||
            !std::isfinite(source.authored_ground_y) || !std::isfinite(source.authored_collision_radius) || source.authored_collision_radius < 0 ||
            !std::isfinite(source.authored_hdr) || source.authored_hdr <= 0 || source.authored_gradient_row >= impl_->vfx_gradient_rows ||
            !std::isfinite(source.authored_birth_fraction) || source.authored_birth_fraction < 0 || source.authored_birth_fraction >= 1 ||
            !std::isfinite(source.gravity)))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored ballistic command.");
        if (source.authored_ballistic && (snapshot.header.tick < source.authored_birth_tick ||
            (snapshot.header.tick == source.authored_birth_tick && source.authored_birth_fraction > 0))) continue;
        if (total_particles_to_spawn == particle_capacity)
        {
            break;
        }
        const auto age_ticks =
            snapshot.header.tick > source.tick ? snapshot.header.tick - source.tick : 0;
        const auto age = source.authored_ballistic
            ? (static_cast<float>(snapshot.header.tick - source.authored_birth_tick) - source.authored_birth_fraction) / 60.0f
            : static_cast<float>(age_ticks) / 60.0f;
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
    const auto particle_delta_ticks =
        impl_->particles_initialized && snapshot.header.tick > impl_->last_particle_tick
            ? snapshot.header.tick - impl_->last_particle_tick
            : 0;
    const auto view =
        DirectX::XMMatrixLookAtLH(eye, target, camera_up);
    const bool grid_aa_disabled = impl_->config.grid_floor_probe &&
                                  impl_->config.grid_floor_disable_aa;
    const bool temporal_enabled = impl_->config.temporal_reconstruction &&
                                  !grid_aa_disabled;
    const bool use_sr_reconstruction = temporal_enabled && impl_->reconstruction_provider &&
                          impl_->reconstruction_provider->Initialized();
    const bool use_dlss_rr = impl_->ray_reconstruction.Mode() == RayReconstructionMode::DlssFused;
    const bool use_native_reconstruction = use_sr_reconstruction || use_dlss_rr;
    const auto projection = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees),
        static_cast<float>(impl_->render_width) / static_cast<float>(impl_->render_height), far_plane,
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
    if (use_sr_reconstruction && impl_->reconstruction_provider->ProviderId() == "xess")
    {
        // Pinned XeSS-SR guide: at least 8 * (targetWidth / inputWidth)^2
        // samples, derived from the actual active extent rather than a preset.
        const auto scale = static_cast<double>(impl_->width) / impl_->render_width;
        jitter_sequence_length = static_cast<std::uint64_t>(std::max(8.0, std::ceil(8.0 * scale * scale)));
    }
    // Each view advances only after its successful real render/presentation. History resets
    // keep the same sampling phase convention and reset the native history.
    const auto jitter_index = 1u + impl_->frame_number % jitter_sequence_length;
    auto jitter_x = temporal_enabled
        ? (halton(jitter_index, 2) - 0.5f) * 2.0f /
              static_cast<float>(impl_->render_width)
        : 0.0f;
    auto jitter_y = temporal_enabled
        ? (0.5f - halton(jitter_index, 3)) * 2.0f /
              static_cast<float>(impl_->render_height)
        : 0.0f;
    if (use_sr_reconstruction)
    {
        std::optional<DirectX::XMFLOAT2> native_jitter;
        if (auto queried = impl_->reconstruction_provider->NativeJitter(impl_->view_state.render_active,
                impl_->frame_number, native_jitter); !queried) return queried;
        if (native_jitter)
        {
            jitter_x = native_jitter->x * 2.0f / impl_->render_width;
            jitter_y = -native_jitter->y * 2.0f / impl_->render_height;
        }
    }
    const auto clip_matrix = view * projection *
        DirectX::XMMatrixTranslation(jitter_x, jitter_y, 0.0f);
    DirectX::XMStoreFloat4x4(&constants->view_projection,
                             DirectX::XMMatrixTranspose(clip_matrix));
    const auto previous_scene_shift = impl_->previous_origins_valid
        ? DirectX::XMMatrixTranslation(static_cast<float>(view_input.scene_origin.x - impl_->previous_scene_origin.x),
            static_cast<float>(view_input.scene_origin.y - impl_->previous_scene_origin.y),
            static_cast<float>(view_input.scene_origin.z - impl_->previous_scene_origin.z))
        : DirectX::XMMatrixIdentity();
    const auto previous_clip = previous_scene_shift *
        DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&impl_->previous_view_projection));
    DirectX::XMStoreFloat4x4(&constants->previous_view_projection,
                             (impl_->temporal_history_valid || impl_->hzb_history_valid)
                                 ? DirectX::XMMatrixTranspose(previous_clip)
                                 : DirectX::XMMatrixTranspose(clip_matrix));
    DirectX::XMFLOAT3 current_temporal_eye{};
    DirectX::XMStoreFloat3(&current_temporal_eye, eye);
    const auto camera_delta = std::hypot(
        double(current_temporal_eye.x) - impl_->temporal_previous_eye.x +
            (view_input.scene_origin.x - impl_->previous_scene_origin.x),
        double(current_temporal_eye.y) - impl_->temporal_previous_eye.y +
            (view_input.scene_origin.y - impl_->previous_scene_origin.y),
        double(current_temporal_eye.z) - impl_->temporal_previous_eye.z +
            (view_input.scene_origin.z - impl_->previous_scene_origin.z));
    if ((impl_->temporal_history_valid || impl_->hzb_history_valid) &&
        (camera_delta > std::max(2.0f, camera_distance * 0.25f) ||
         std::abs(snapshot.camera.vertical_fov_degrees - impl_->temporal_previous_fov) > 1.0f))
    {
        impl_->temporal_history_valid = false;
        impl_->hzb_history_valid = false;
    }
    constants->temporal_options = {temporal_enabled ? 1.0f : 0.0f,
                                   impl_->temporal_history_valid ? 1.0f : 0.0f,
                                   far_plane, near_plane};
    const auto current_view_origin = impl_->legacy_frame_bridge
        ? RenderWorldPosition{current_temporal_eye.x, current_temporal_eye.y, current_temporal_eye.z}
        : view_input.view_origin;
    const DirectX::XMFLOAT3 view_origin_scene{
        static_cast<float>(current_view_origin.x - view_input.scene_origin.x),
        static_cast<float>(current_view_origin.y - view_input.scene_origin.y),
        static_cast<float>(current_view_origin.z - view_input.scene_origin.z)};
    const auto view_origin_vector = DirectX::XMLoadFloat3(&view_origin_scene);
    DirectX::XMStoreFloat4x4(&constants->inverse_view_projection,
        DirectX::XMMatrixInverse(nullptr, DirectX::XMMatrixTranslationFromVector(view_origin_vector) * clip_matrix));
    // Existing ray/volume/AO services use the actual eye as their relative
    // basis. Keep that ABI separate from the explicit Surface ViewOrigin.
    DirectX::XMFLOAT4X4 camera_relative_inverse_view_projection;
    DirectX::XMStoreFloat4x4(&camera_relative_inverse_view_projection,
        DirectX::XMMatrixInverse(nullptr, DirectX::XMMatrixTranslationFromVector(eye) * clip_matrix));
    DirectX::XMStoreFloat4x4(&constants->current_unjittered_view_projection, view * projection);
    if (impl_->temporal_history_valid || impl_->hzb_history_valid)
        DirectX::XMStoreFloat4x4(&constants->previous_unjittered_view_projection,
            previous_scene_shift * DirectX::XMLoadFloat4x4(&impl_->previous_unjittered_view_projection));
    else constants->previous_unjittered_view_projection = constants->current_unjittered_view_projection;
    constants->surface_view_origin = {view_origin_scene.x, view_origin_scene.y,
        view_origin_scene.z, (impl_->temporal_history_valid || impl_->hzb_history_valid) ? 1.0f : 0.0f};
    constants->surface_jitter_uv = {jitter_x * 0.5f, -jitter_y * 0.5f,
        impl_->temporal_history_valid ? impl_->previous_jitter_uv.x : jitter_x * 0.5f,
        impl_->temporal_history_valid ? impl_->previous_jitter_uv.y : -jitter_y * 0.5f};
    // LOD is measured in output pixels, independent of temporal jitter and
    // internal render scale. View columns provide the world-to-view basis.
    DirectX::XMFLOAT4X4 lod_view{}, lod_projection{};
    DirectX::XMStoreFloat4x4(&lod_view, view);
    DirectX::XMStoreFloat4x4(&lod_projection, projection);
    constants->lod_view_right = {lod_view._11, lod_view._21, lod_view._31, 0};
    constants->lod_view_up = {lod_view._12, lod_view._22, lod_view._32, 0};
    constants->lod_view_forward = {lod_view._13, lod_view._23, lod_view._33, 0};
    const auto lod_shadow_resolution = std::clamp(impl_->config.shadow_resolution, 1024u, 2048u);
    const auto shadow_distance = impl_->config.shadow_distance_m;
    const std::array cascade_extents{shadow_distance * (18.0f / 160.0f),
                                    shadow_distance * (48.0f / 160.0f), shadow_distance};
    constants->lod_projection = {
        0.5f * std::max(impl_->width * std::abs(lod_projection._11),
                        impl_->height * std::abs(lod_projection._22)),
        lod_shadow_resolution / cascade_extents[0], lod_shadow_resolution / cascade_extents[1],
        lod_shadow_resolution / cascade_extents[2]};
    constants->lod_options = {impl_->QualityProfileFor(impl_->config.geometry_quality).geometry_screen_error_px,
                             0.15f, 0.1f, 0.125f};
    if (new_session) ++impl_->lod_session_epoch;
    constants->lod_clock = {static_cast<UINT>(impl_->frame_number + 1),
        impl_->lod_session_epoch,
        (impl_->temporal_history_valid || impl_->hzb_history_valid) ? 1u : 0u,
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
    constants->output_size = {static_cast<float>(impl_->width), static_cast<float>(impl_->height),
                             1.0f / impl_->width, 1.0f / impl_->height};
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
    constants->display_options = {impl_->display_state.hdr_active ? 1.0f : 0.0f,
        impl_->display_state.paper_white_nits, impl_->display_state.peak_nits, 0};
    DirectX::XMStoreFloat4x4(&constants->inverse_unjittered_view_projection,
                            DirectX::XMMatrixInverse(nullptr, view * projection));
    ExposureConstants exposure_constants;
    exposure_constants.width = impl_->render_width;
    exposure_constants.height = impl_->render_height;
    exposure_constants.delta_seconds = impl_->exposure_sample_time.time_since_epoch().count() == 0
        ? 1.0f / 60.0f : std::chrono::duration<float>(now - impl_->exposure_sample_time).count();
    if (impl_->explicit_frame->delta_seconds) exposure_constants.delta_seconds = *impl_->explicit_frame->delta_seconds;
    exposure_constants.reset = new_session || view_input.camera_cut ? 1u : 0u;
    std::memcpy(frame.mapped + kExposureConstantsOffset, &exposure_constants, sizeof(exposure_constants));
    const auto previous_sdk_camera = impl_->sdk_camera;
    auto &sdk_camera = impl_->sdk_camera;
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
    sdk_camera.reset_history = !impl_->temporal_history_valid || new_session;
        if (sdk_camera.reset_history) sdk_camera.previous_view = sdk_camera.view;
    else DirectX::XMStoreFloat4x4(&sdk_camera.previous_view,
        previous_scene_shift * DirectX::XMLoadFloat4x4(&previous_sdk_camera.view));
    sdk_camera.previous_projection = sdk_camera.reset_history ? sdk_camera.projection : previous_sdk_camera.projection;
    sdk_camera.previous_position = sdk_camera.reset_history ? sdk_camera.position : DirectX::XMFLOAT3{
        static_cast<float>(double(previous_sdk_camera.position.x) + (impl_->previous_scene_origin.x - view_input.scene_origin.x)),
        static_cast<float>(double(previous_sdk_camera.position.y) + (impl_->previous_scene_origin.y - view_input.scene_origin.y)),
        static_cast<float>(double(previous_sdk_camera.position.z) + (impl_->previous_scene_origin.z - view_input.scene_origin.z))};
    sdk_camera.near_plane = near_plane; sdk_camera.far_plane = far_plane;
    sdk_camera.vertical_fov_radians = DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees);
    sdk_camera.delta_seconds = exposure_constants.delta_seconds;
    sdk_camera.jitter_pixels[0] = constants->surface_jitter_uv.x * impl_->render_width;
    sdk_camera.jitter_pixels[1] = constants->surface_jitter_uv.y * impl_->render_height;
    sdk_camera.reversed_depth = true;
    if (!presented_view || !impl_->native_input_phase)
        impl_->sdk_frame_identity = {impl_->active_render_package_id, impl_->active_render_package_id, impl_->active_native_viewport_id};
    if (impl_->frame_number == 0)
    {
        DirectX::XMFLOAT3 projected_origin{};
        DirectX::XMFLOAT3 projected_forward{};
        DirectX::XMStoreFloat3(
            &projected_origin,
            DirectX::XMVector3TransformCoord(DirectX::XMVectorZero(), view * projection));
        DirectX::XMStoreFloat3(
            &projected_forward,
            DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(0, 0, 20, 1),
                                             view * projection));
        std::ofstream camera_log(impl_->config.artifact_directory / "camera.json",
                                 std::ios::trunc);
        camera_log << std::format(
            "{{\"origin\":[{},{},{}],\"forward\":[{},{},{}],\"yaw\":{},\"pitch\":{}}}\n",
            projected_origin.x, projected_origin.y, projected_origin.z, projected_forward.x,
            projected_forward.y, projected_forward.z, camera_yaw, camera_pitch);
    }
    DirectX::XMStoreFloat4(&constants->camera_time, eye);
    constants->camera_time.w =
        static_cast<float>(snapshot.header.simulation_time.count()) / 1'000'000'000.0f;
    std::vector<LightView> frame_lights(snapshot.lights.begin(), snapshot.lights.end());
    if (frame_lights.empty())
    {
        LightView sun{{-0.4f, -0.8f, 0.3f}, 3.0f * kLightWhiteLumensPerWatt, {1, 1, 1}};
        sun.stable_id = 1;
        sun.shadow_flags = kLightShadowRequested;
        frame_lights.push_back(sun);
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
        static_cast<float>(impl_->render_width), static_cast<float>(impl_->render_height),
        1.0f / static_cast<float>(impl_->render_width),
        1.0f / static_cast<float>(impl_->render_height)};
    DirectX::XMFLOAT3 camera_forward;
    DirectX::XMStoreFloat3(&camera_forward, direction);
    constants->camera_forward_softness = {camera_forward.x, camera_forward.y,
                                          camera_forward.z, 0.35f};
    // Numeric VFX budgets use the current four-tier graphics intent. Individual
    // output min-quality admission remains in the typed command adapters.
    const auto vfx_graphics_quality = impl_->last_applied_graphics_request_available
        ? impl_->last_applied_graphics_request.particle_quality : impl_->config.requested_graphics
            ? impl_->config.requested_graphics->particle_quality : impl_->config.vfx_quality == VfxQuality::Low
                ? GraphicsQuality::Low : impl_->config.vfx_quality == VfxQuality::Medium
                    ? GraphicsQuality::Medium : GraphicsQuality::High;
    const auto &vfx_quality_profile = impl_->QualityProfileFor(vfx_graphics_quality);
    struct LightCandidate { const VfxLightInput *light; float screen_area; };
    std::vector<LightCandidate> light_candidates;
    for (const auto &source : typed_lights)
    {
        if (!std::isfinite(source.position.x) || !std::isfinite(source.position.y) || !std::isfinite(source.position.z) ||
            !std::isfinite(source.radius) || source.radius <= 0 || !std::isfinite(source.intensity) || source.intensity < 0 ||
            !std::isfinite(source.linear_rgb.x) || !std::isfinite(source.linear_rgb.y) || !std::isfinite(source.linear_rgb.z) ||
            source.linear_rgb.x < 0 || source.linear_rgb.y < 0 || source.linear_rgb.z < 0 ||
            static_cast<std::uint32_t>(source.quality) > 2)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored VFX light.");
        if (source.intensity == 0 || impl_->config.slime_family_preview_count != 0) continue;
        const auto world = DirectX::XMVectorSet(source.position.x, source.position.y, source.position.z, 1);
        DirectX::XMFLOAT3 view_position;
        DirectX::XMStoreFloat3(&view_position, DirectX::XMVector3TransformCoord(world, view));
        bool on_screen = view_position.z + source.radius > near_plane && view_position.z - source.radius < far_plane;
        const float distance = std::max(view_position.z, near_plane);
        const float tangent_y = std::tan(DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees) * .5f);
        const float tangent_x = tangent_y * static_cast<float>(impl_->render_width) / impl_->render_height;
        // Sphere distance to each slanted frustum plane, not a box at center depth.
        if (std::abs(view_position.x) - view_position.z * tangent_x > source.radius * std::sqrt(1.0f+tangent_x*tangent_x) ||
            std::abs(view_position.y) - view_position.z * tangent_y > source.radius * std::sqrt(1.0f+tangent_y*tangent_y)) on_screen = false;
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
        const auto limit = static_cast<UINT>(std::floor(impl_->config.vfx_light_count *
            vfx_quality_profile.vfx_light_fraction));
        const auto index = constants->vfx_light_count.x;
        if (index >= limit) continue;
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
        point.revision = impl_->frame_number + 1;
        transient_light_ids.push_back(point.stable_id);
        frame_lights.push_back(point);
        ++constants->vfx_light_count.x;
    }
    struct DistortionCandidate { const VfxDistortionInput *source; float area; };
    std::vector<DistortionCandidate> distortion_candidates;
    for (const auto &source : typed_distortions)
    {
        const auto finite = [](Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        if (!finite(source.position) || !finite(source.direction) || !std::isfinite(source.radius) || source.radius <= 0 ||
            !std::isfinite(source.strength) || source.strength < 0 || !std::isfinite(source.alpha) || source.alpha < 0 || source.alpha > 1 ||
            !std::isfinite(source.normalized_age) || source.normalized_age < 0 || source.normalized_age > 1 ||
            static_cast<std::uint32_t>(source.shape) > 6 || static_cast<std::uint32_t>(source.quality) > 2)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid authored distortion command.");
        const bool cone = source.shape == VfxDistortionShape::ConeSector;
        const bool annulus = source.shape == VfxDistortionShape::GappedAnnulus;
        const float horizontal_direction = std::hypot(source.direction.x,source.direction.z);
        if ((cone && (!std::isfinite(source.half_angle_degrees) || source.half_angle_degrees <= 0 ||
                      source.half_angle_degrees > 180 || !std::isfinite(horizontal_direction) ||
                      horizontal_direction <= .0001f || source.inner_radius != 0 ||
                      source.gap_half_width_degrees != 0 || !source.gap_angles_degrees.empty())) ||
            (annulus && (!std::isfinite(source.inner_radius) || source.inner_radius < 0 ||
                         source.inner_radius >= source.radius || !std::isfinite(source.gap_half_width_degrees) ||
                         source.gap_half_width_degrees < 0 || source.gap_half_width_degrees > 180 ||
                         source.half_angle_degrees != 0 ||
                         (!source.gap_angles_degrees.empty() && source.gap_half_width_degrees == 0) ||
                         std::any_of(source.gap_angles_degrees.begin(),source.gap_angles_degrees.end(),
                             [](float angle){return !std::isfinite(angle); }))) ||
            (!cone && !annulus && (source.inner_radius != 0 || source.half_angle_degrees != 0 ||
                                   source.gap_half_width_degrees != 0 || !source.gap_angles_degrees.empty())))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Invalid exact distortion geometry.");
        if (source.alpha == 0 || source.strength == 0 || impl_->config.slime_family_preview_count != 0) continue;
        DirectX::XMFLOAT3 center;
        DirectX::XMStoreFloat3(&center, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(source.position.x,source.position.y,source.position.z,1),view));
        const float radius = source.radius;
        if (center.z + radius <= near_plane || center.z - radius >= far_plane) continue;
        const float ty = std::tan(DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees) * .5f);
        const float tx = ty * static_cast<float>(impl_->render_width) / impl_->render_height;
        if (std::abs(center.x) - center.z * tx > radius * std::sqrt(1 + tx*tx) ||
            std::abs(center.y) - center.z * ty > radius * std::sqrt(1 + ty*ty)) continue;
        const float distance = std::max(center.z - radius,near_plane);
        const float area = std::min(1.0f, 3.14159265f * radius * radius / (4 * distance * distance * tx * ty));
        distortion_candidates.push_back({&source,area});
    }
    std::stable_sort(distortion_candidates.begin(),distortion_candidates.end(),[](const auto &a,const auto &b) {
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
        const auto &source = *candidate.source;
        const auto fraction = vfx_quality_profile.vfx_distortion_fraction;
        if (distortion_count >= static_cast<UINT>(std::floor(kMaxDistortions * fraction)) ||
            distortion_area + candidate.area > impl_->config.distortion_coverage * fraction) continue;
        auto &gpu = gpu_distortions[distortion_count++];
        gpu.position_radius = {source.position.x,source.position.y,source.position.z,source.radius};
        gpu.direction_strength = {source.direction.x,source.direction.y,source.direction.z,source.strength};
        gpu.phase_alpha_shape_seed = {source.normalized_age,source.alpha,static_cast<float>(source.shape),static_cast<float>(source.stable_seed & 0xffffu)};
        gpu.geometry = {source.inner_radius,
                        DirectX::XMConvertToRadians(source.half_angle_degrees),
                        DirectX::XMConvertToRadians(source.gap_half_width_degrees),0.0f};
        gpu.gap_range = {gap_write_index,static_cast<UINT>(source.gap_angles_degrees.size()),0,0};
        for (const float degrees : source.gap_angles_degrees)
            gpu_ground_gaps[gap_write_index++] = DirectX::XMConvertToRadians(degrees);
        distortion_area += candidate.area;
    }
    struct DecalCandidate { const VfxDecalInput *source; float area; };
    std::vector<DecalCandidate> decal_candidates;
    for (const auto &source : typed_decals)
    {
        const auto finite = [](Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
        const bool fracture = source.kind == VfxDecalKind::Fracture;
        if (!finite(source.position) || !finite(source.direction) || !finite(source.linear_rgb) ||
            source.linear_rgb.x < 0 || source.linear_rgb.y < 0 || source.linear_rgb.z < 0 ||
            !std::isfinite(source.radius) || source.radius <= 0 || !std::isfinite(source.hdr) || source.hdr < 0 ||
            !std::isfinite(source.alpha) || source.alpha < 0 || source.alpha > 1 || !std::isfinite(source.normalized_age) ||
            source.normalized_age < 0 || source.normalized_age > 1 || !std::isfinite(source.edge_glow) || source.edge_glow < 0 || source.edge_glow > 1 ||
            source.gradient_row >= impl_->vfx_gradient_rows || source.texture_slice >= (fracture ? 4u : 8u) ||
            static_cast<std::uint32_t>(source.kind) > 1 || static_cast<std::uint32_t>(source.quality) > 2 ||
            (fracture && source.voronoi_cells == 0))
            return Result::Failure(ErrorCode::InvalidArgument,"hs_renderer_d3d12","Invalid authored projected decal.");
        if (source.alpha == 0 || impl_->config.slime_family_preview_count != 0) continue;
        // Ground-only projector slab follows its source anchor, including elevated death events.
        DirectX::XMFLOAT3 center;
        DirectX::XMStoreFloat3(&center,DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(source.position.x,source.position.y-.825f,source.position.z,1),view));
        const float radius = std::hypot(source.radius,1.175f);
        const float ty = std::tan(DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees)*.5f);
        const float tx = ty*static_cast<float>(impl_->render_width)/impl_->render_height;
        if (center.z+radius <= near_plane || center.z-radius >= far_plane ||
            std::abs(center.x)-center.z*tx > radius*std::sqrt(1+tx*tx) ||
            std::abs(center.y)-center.z*ty > radius*std::sqrt(1+ty*ty)) continue;
        const float distance = std::max(center.z,near_plane);
        decal_candidates.push_back({&source,std::min(1.0f,source.radius*source.radius/(distance*distance))});
    }
    std::stable_sort(decal_candidates.begin(),decal_candidates.end(),[](const auto &a,const auto &b) {
        if(a.source->importance != b.source->importance) return a.source->importance > b.source->importance;
        if(a.area != b.area) return a.area > b.area;
        if(a.source->stable_id != b.source->stable_id) return a.source->stable_id < b.source->stable_id;
        return a.source->stable_seed < b.source->stable_seed;
    });
    auto *gpu_decals = reinterpret_cast<GpuDecal *>(frame.mapped+decal_data_offset);
    constants->vfx_decal_count = {};
    for(const auto &candidate:decal_candidates)
    {
        const auto &source=*candidate.source;
        const auto limit=static_cast<UINT>(std::floor(kMaxDecals * vfx_quality_profile.vfx_decal_fraction));
        if(constants->vfx_decal_count.x>=limit) continue;
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
    for (auto &bone : constants->archer_bones)
    {
        DirectX::XMStoreFloat4x4(
            &bone, DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
    }
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
        if (const auto selected = std::ranges::find(snapshot.poses, source_index,
                                                   &AnimationPoseRef::instance_index);
            selected != snapshot.poses.end())
            pose = *selected;
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
        const auto &first = impl_->archer_transforms[
            found->first_transform + first_frame * impl_->archer_bone_count + bone_index];
        const auto &second = impl_->archer_transforms[
            found->first_transform + second_frame * impl_->archer_bone_count + bone_index];
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
            if (ordered < ordered_source_indices.size())
                for (const auto &candidate : snapshot.poses)
                    if (candidate.instance_index == ordered_source_indices[ordered]) return candidate;
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
    // Keep the legacy frame constant populated for compatibility with the existing
    // shader ABI. Cached Archer draws use their per-instance upload below.
    for (const auto &instance : render_instances)
        if (instance.mesh == RenderMesh::Archer && instance.animation_clip_id.value == 0)
        {
            if (auto sampled = sample_legacy_pose(pose_for_archer(instance),
                                                  std::span(constants->archer_bones)); !sampled)
                return sampled;
            break;
        }
    struct ArcherPaletteKey
    {
        std::array<std::uint64_t, 16> values{};
        bool operator==(const ArcherPaletteKey &) const noexcept = default;
    };
    std::vector<ArcherPaletteKey> palette_keys;
    std::vector<std::uint32_t> archer_palette_slots(instance_count,
                                                     std::numeric_limits<std::uint32_t>::max());
    std::vector<DirectX::XMFLOAT4X4> palette(kMaxCharacterBones);
    std::size_t palette_count{};
    for (std::size_t index = 0; index < render_instances.size(); ++index)
    {
        const auto &instance = render_instances[index];
        if (instance.mesh != RenderMesh::Archer) continue;
        const auto &instance_pose = pose_for_archer(instance);
        ArcherPaletteKey key;
        key.values[0] = instance.asset_id.value;
        key.values[1] = instance.animation_clip_id.value;
        key.values[2] = std::bit_cast<std::uint64_t>(instance.animation_time_seconds);
        key.values[3] = std::bit_cast<std::uint32_t>(instance.animation_playback_rate);
        key.values[4] = instance.revisions.pose != 0
            ? instance.revisions.pose
            : DeriveAnimationPoseRevision(instance_pose);
        key.values[5] = static_cast<std::uint8_t>(instance_pose.clip);
        key.values[6] = std::bit_cast<std::uint32_t>(instance_pose.normalized_time);
        key.values[7] = std::bit_cast<std::uint32_t>(instance_pose.playback_rate);
        key.values[8] = static_cast<std::uint8_t>(instance_pose.secondary_clip);
        key.values[9] = std::bit_cast<std::uint32_t>(instance_pose.secondary_normalized_time);
        key.values[10] = std::bit_cast<std::uint32_t>(instance_pose.secondary_playback_rate);
        key.values[11] = std::bit_cast<std::uint32_t>(instance_pose.secondary_weight);
        key.values[12] = static_cast<std::uint8_t>(instance_pose.upper_body_clip);
        key.values[13] = std::bit_cast<std::uint32_t>(instance_pose.upper_body_normalized_time);
        key.values[14] = std::bit_cast<std::uint32_t>(instance_pose.upper_body_playback_rate);
        key.values[15] = std::bit_cast<std::uint32_t>(instance_pose.upper_body_weight);
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
        if (instance.animation_clip_id.value != 0)
        {
            if (auto sampled = SampleNamedCharacterAnimation(
                    impl_->archer_named_animations, instance,
                    std::span(palette.data(), impl_->archer_bone_count)); !sampled)
                return sampled;
        }
        else if (auto sampled = sample_legacy_pose(instance_pose,
                                                   std::span(palette.data(), impl_->archer_bone_count));
                 !sampled)
            return sampled;
        std::memcpy(frame.mapped + archer_palette_data_offset + slot * archer_palette_stride,
                    palette.data(), archer_palette_stride);
        palette_keys.push_back(key);
        archer_palette_slots[index] = static_cast<std::uint32_t>(slot);
    }
    // Non-Archer entries intentionally carry the sentinel; only actual Archer
    // instances must have a palette prepared for the cache pass.
    for (std::size_t index = 0; index < render_instances.size(); ++index)
        if (render_instances[index].mesh == RenderMesh::Archer &&
            archer_palette_slots[index] == std::numeric_limits<std::uint32_t>::max())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Archer instance has no prepared palette.");
    constants->render_options = {
        static_cast<float>(particle_capacity), impl_->config.bloom ? 1.0f : 0.0f,
        impl_->config.outline ? 1.0f : 0.0f,
        static_cast<float>(particle_delta_ticks) / 60.0f};
    constants->authored_particle_clock = {static_cast<std::uint32_t>(snapshot.header.tick), 0, 0, 0};
    if (!impl_->render_scene_inputs) { gpu_particle_spawn_count = 0; total_particles_to_spawn = 0; }
    constants->particle_options = {particle_capacity, gpu_particle_spawn_count,
                                   total_particles_to_spawn,
                                   impl_->archer_material_count};
    std::vector<std::pair<float, std::uint64_t>> grass_candidates;
    DirectX::XMFLOAT3 player_position{};
    bool have_player = false;
    for (const auto &source : render_instances)
    {
        if (source.mesh == RenderMesh::Archer)
        {
            player_position = {source.position.x, source.position.y, source.position.z};
            have_player = true;
            break;
        }
    }
    if (have_player)
    {
        for (const auto &source : render_instances)
        {
            if (source.mesh < RenderMesh::MonsterMelee || source.mesh > RenderMesh::BossFinal)
                continue;
            const auto dx = source.position.x - player_position.x;
            const auto dz = source.position.z - player_position.z;
            grass_candidates.emplace_back(dx * dx + dz * dz, source.stable_id);
        }
        std::ranges::sort(grass_candidates);
        constants->grass_benders[0] = {player_position.x, player_position.y,
                                       player_position.z, 1.25f};
        const auto count = std::min<std::size_t>(grass_candidates.size() + 1, 32);
        std::size_t output = 1;
        for (const auto &candidate : grass_candidates)
        {
            if (output == count) break;
            const auto it = std::ranges::find_if(render_instances, [&](const auto &source) {
                return source.stable_id == candidate.second;
            });
            if (it == render_instances.end()) continue;
            constants->grass_benders[output++] = {it->position.x, it->position.y,
                                                  it->position.z, 1.0f};
        }
        constants->grass_bender_count = {static_cast<std::uint32_t>(output), 0, 0, 0};
    }
    else
    {
        constants->grass_bender_count = {};
    }
    // BendGrass adds wind plus delta * (1 - |delta| / radius) * 0.42.
    // Each bender's maximum displacement is 0.42 * radius / 4, and its
    // height weight is at most one. Keep this per-frame allowance separate
    // from the persistent undeformed scene bound so wind/bending does not
    // force static scene uploads or lose casters outside the camera frustum.
    float grass_visibility_radius = 0.035f * std::sqrt(1.0f + 0.7f * 0.7f);
    for (std::size_t index = 0; index < constants->grass_bender_count.x; ++index)
        grass_visibility_radius += 0.105f *
            std::max(constants->grass_benders[index].w, 0.1f);
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
    for (std::size_t cascade = 0; cascade < 3; ++cascade)
        make_demand_planes(cascade + 1, DirectX::XMMatrixTranspose(
            DirectX::XMLoadFloat4x4(&constants->shadow_view_projection[cascade])));
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
        cut.material_word = (impl_->material_parameter_bases[ordinal] << 1u) | 1u;
        for (std::size_t consumer = 0; consumer < 4; ++consumer)
            for (std::size_t plane = 0; plane < 6; ++plane)
            {
                const auto &p = demand_planes[consumer][plane];
                cut.frustum_planes[consumer][plane] = {p.x, p.y, p.z, p.w};
            }
    }
    if (auto *hlod_view = impl_->ActiveHlodView(); hlod_view && hlod_view->frames[back_buffer_index].enabled)
    {
        auto &hlod_views = hlod_view->frames[back_buffer_index].views;
        for (std::size_t index = 0; index < hlod_views.size(); ++index)
        {
            auto &hlod_view = hlod_views[index];
            hlod_view.origin_near = {constants->camera_time.x, constants->camera_time.y,
                constants->camera_time.z, constants->lod_options.z};
            hlod_view.forward = constants->lod_view_forward;
            for (std::size_t plane = 0; plane < 6; ++plane)
                hlod_view.frustum_planes[plane] = demand_planes[index][plane];
            const float projection_scale = index == 0 ? constants->lod_projection.x
                : index == 1 ? constants->lod_projection.y : index == 2 ? constants->lod_projection.z : constants->lod_projection.w;
            hlod_view.error_policy = {projection_scale, constants->lod_options.x, 0.15f, index == 0 ? 0.0f : 1.0f};
        }
    }
    if (auto prepared = impl_->PrepareClusterFrame(back_buffer_index, cluster_inputs); !prepared) return prepared;
    std::array<float, kEnvironmentMaterialNames.size()> geometry_demand{};
    const auto accumulate_geometry_demand = [&](const auto &source) {
        if (!is_environment_mesh(source.mesh)) return;
        const auto ordinal = environment_mesh_index(source);
        const auto &coarse = impl_->environment_meshes[ordinal * 3 + 1];
        const auto scale = std::max({std::abs(source.scale.x), std::abs(source.scale.y),
                                     std::abs(source.scale.z)});
        const auto deformation = source.mesh == RenderMesh::Grass ? grass_visibility_radius : 0.0f;
        const auto radius = impl_->environment_variant_radii[ordinal] * scale + deformation;
        const auto error = coarse.metadata.geometric_error * scale + 2.0f * deformation;
        const auto &center = source.position;
        DirectX::XMFLOAT3 view_center{};
        DirectX::XMStoreFloat3(&view_center, DirectX::XMVector3TransformCoord(
            DirectX::XMVectorSet(center.x, center.y, center.z, 1), view));
        for (std::size_t consumer = 0; consumer < demand_planes.size(); ++consumer)
        {
            bool visible = true;
            for (const auto &plane : demand_planes[consumer])
                visible = visible && center.x * plane.x + center.y * plane.y +
                    center.z * plane.z + plane.w >= -radius;
            if (!visible) continue;
            float projected_error{};
            if (consumer == 0)
            {
                const auto distance = view_center.z - radius - error;
                const auto safe_distance = std::max(distance, constants->lod_options.z);
                const auto rho = std::hypot(std::abs(view_center.x) + radius + error,
                                            std::abs(view_center.y) + radius + error) / safe_distance;
                projected_error = distance <= constants->lod_options.z
                    ? constants->lod_options.x * 2.0f
                    : error * constants->lod_projection.x * std::sqrt(1.0f + rho * rho) / safe_distance;
            }
            else
            {
                const float shadow_projection[]{constants->lod_projection.y,
                    constants->lod_projection.z, constants->lod_projection.w};
                projected_error = error * shadow_projection[consumer - 1];
            }
            geometry_demand[ordinal] = std::max(geometry_demand[ordinal], projected_error);
        }
    };
    for (const auto &source : render_instances) accumulate_geometry_demand(source);
    if (static_scene)
        for (const auto &source : static_scene->instances)
            if (!is_environment_mesh(source.mesh) || !cluster_active[environment_mesh_index(source)])
                accumulate_geometry_demand(source);
    for (std::uint32_t ordinal = 0; ordinal < geometry_demand.size(); ++ordinal)
        if (geometry_demand[ordinal] > constants->lod_options.x)
            if (auto requested = impl_->DemandEnvironmentGeometry(ordinal,
                    std::bit_cast<UINT>(std::max(geometry_demand[ordinal] / constants->lod_options.x, 1.0f)));
                !requested) return requested;
    // Measure ground UV derivatives through the actual unjittered view. The
    // smallest authored terrain tile is 4.5m (TerrainTileSize, material_terrain).
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
                if (!ground_point(u, v, p) || !ground_point(u + 1.0f / impl_->render_width, v, dx) ||
                    !ground_point(u, v + 1.0f / impl_->render_height, dy)) continue;
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
                impl_->texture_streaming.Demand(id, mip, priority, impl_->frame_number);
        }
    };
    std::array<bool, kEnvironmentMaterialNames.size()> environment_material_requested{};
    const auto demand_environment_material = [&](const RenderInstance &source, bool capture_consumer) {
        if (!is_environment_mesh(source.mesh)) return;
        const auto material = static_cast<std::uint32_t>(environment_mesh_index(source));
        if (environment_material_requested[material]) return;
        const auto radius = impl_->environment_variant_radii[material] *
            std::max({std::abs(source.scale.x), std::abs(source.scale.y), std::abs(source.scale.z)}) +
            (source.mesh == RenderMesh::Grass ? grass_visibility_radius : 0.0f);
        bool visible = capture_consumer || active_features.canonical_geometry_enabled;
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
        // demand comes from real samples. Baseline ray consumers use software demand.
        demand_material(material, 0, std::bit_cast<UINT>(std::max(geometry_demand[material], 1.0f)),
            active_features.canonical_geometry_enabled);
    };
    for (const auto &source : render_instances) demand_environment_material(source, false);
    if (static_scene)
        for (const auto &source : static_scene->instances)
            demand_environment_material(source, impl_->local_probes.AllocatedBytes() != 0);
    if (!impl_->config.grid_floor_probe && std::isfinite(terrain_footprint))
        for (std::uint32_t slot = 0; slot < render_data::kMaterialTextureSlots; ++slot)
        {
            const auto id = impl_->material_texture_stream_ids[kTerrainMaterialOrdinal * render_data::kMaterialTextureSlots + slot];
            if (!id || (impl_->texture_streaming.HardwareBacked(id) && !active_features.canonical_geometry_enabled)) continue;
            const auto logical = impl_->texture_streaming.View(id).logical_description;
            const auto rho = std::max(static_cast<float>(logical.width) * terrain_footprint / 4.5f, 1.0f);
            impl_->texture_streaming.Demand(id, static_cast<std::uint32_t>(std::floor(std::log2(rho))),
                std::bit_cast<UINT>(1.0f / terrain_footprint), impl_->frame_number);
        }
    if (static_scene && impl_->local_probes.AllocatedBytes() != 0)
        for (const auto &source : static_scene->instances)
            if (source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch)
            {
                demand_material(kTerrainMaterialOrdinal, 0, 1, active_features.canonical_geometry_enabled);
                break;
            }
    if (auto result = impl_->texture_streaming.Pump(impl_->frame_number, completed_streaming,
            impl_->asset_io, impl_->streaming, impl_->residency); !result) return result;
    impl_->PublishEnvironmentMaterialTextures(back_buffer_index);
    if (auto result = impl_->texture_feedback.PrepareFrame(back_buffer_index, completed_streaming,
            impl_->material_texture_stream_ids, impl_->texture_streaming, impl_->residency); !result) return result;
    auto feedback_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    feedback_cpu.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
        kTextureFeedbackDescriptorOffset) * impl_->srv_stride;
    auto feedback_gpu = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    feedback_gpu.ptr += (static_cast<UINT64>(back_buffer_index) * kTextureDescriptorCount +
        kTextureFeedbackDescriptorOffset) * impl_->srv_stride;
    if (auto result = impl_->texture_feedback.WriteDescriptors(back_buffer_index, impl_->srv_heap.Get(),
            feedback_cpu, feedback_gpu, impl_->srv_stride); !result) return result;
    impl_->skin_cache.BeginFrame(new_session);
    std::size_t skin_slot_stride = impl_->archer_geometry.vertex_count;
    for (const auto &asset : impl_->monster_assets)
        skin_slot_stride = std::max<std::size_t>(skin_slot_stride, asset.vertex_count);
    if (skin_slot_stride >= (1u << 20u))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Skin cache vertex count exceeds packed dispatch ABI.");
    constants->skin_cache_options = {
        static_cast<std::uint32_t>(skin_slot_stride), 0u, 0u, 0u};
    auto *previous_skin_frame = !new_session && !view_input.camera_cut
        ? impl_->previous_skin_deformation.get() : nullptr;
    const bool previous_skin_frame_valid = previous_skin_frame && previous_skin_frame->skin_cache_valid &&
        previous_skin_frame->skin_cache_vertices.resource &&
        previous_skin_frame->skin_cache_slot_stride == skin_slot_stride;
    std::vector<std::uint32_t> dynamic_pose_flags(instance_count);
    std::vector<float> dynamic_pose_times(instance_count, 1.0f);
    std::vector<std::uint8_t> dynamic_skin_history(instance_count);
    std::vector<std::uint32_t> dynamic_skin_slots(
        instance_count, std::numeric_limits<std::uint32_t>::max());
    for (std::size_t index = 0; index < instance_count; ++index)
    {
        const auto &source = render_instances[index];
        auto position = source.position;
        auto yaw_value = source.yaw;
        if (snapshots.has_previous && source.stable_id != 0)
        {
            const auto previous_it = previous_by_id.find(source.stable_id);
            if (previous_it != previous_by_id.end())
            {
                const auto &previous = *previous_it->second;
                position.x = std::lerp(previous.position.x, source.position.x, interpolation);
                position.y = std::lerp(previous.position.y, source.position.y, interpolation);
                position.z = std::lerp(previous.position.z, source.position.z, interpolation);
                const auto yaw_delta = std::remainder(source.yaw - previous.yaw,
                                                      2.0f * DirectX::XM_PI);
                yaw_value = previous.yaw + yaw_delta * interpolation;
            }
        }
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
            mesh |= 0x10000u;
        float animation_time = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch
            ? std::bit_cast<float>(source.environment_seed) : 0.0f;
        AnimationPoseRef resolved_skin_pose;
        const AnimationPoseRef *skin_pose = nullptr;
        if (source.mesh == RenderMesh::Archer) skin_pose = &pose_for_archer(source);
        if (is_monster(source.mesh))
        {
            if (impl_->config.monster_preview_asset < 6 || family_preview)
            {
                resolved_skin_pose.clip = static_cast<CharacterAnimationClip>(
                    std::min(impl_->config.monster_preview_clip, 4u));
                resolved_skin_pose.normalized_time = impl_->config.monster_preview_time;
                resolved_skin_pose.revision =
                    DeriveAnimationPoseRevision(resolved_skin_pose);
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
            const RenderSceneInstanceKey key{
                source.stable_id,
                source.stable_id == 0 ? ordered_source_indices[index] + 1 : 0u};
            const auto skin = impl_->skin_cache.Resolve(
                key, source, skin_pose,
                source.mesh == RenderMesh::Archer ? 1.75f : 1.5f);
            conservative_radius = skin.conservative_radius;
            dynamic_skin_history[index] =
                skin.history_valid && previous_skin_frame_valid ? 1u : 0u;
            if (is_monster(source.mesh))
            {
                dynamic_pose_flags[index] =
                    static_cast<std::uint32_t>(skin.current_clip) << 8u;
                dynamic_pose_times[index] = skin.current_time;
            }
        }
        instances[index] = {{position.x, position.y + vertical_offset, position.z,
                             conservative_radius},
                            {source.scale.x, source.scale.y, source.scale.z, 1.0f},
                            source.color_rgba,
                            mesh,
                            yaw_value,
                            animation_time};
    }
    impl_->skin_cache.EndFrame();

    // Keep transform/material/deformation payloads in stable GPU slots. Draw order,
    // LOD fade and environment complement state live in a compact per-frame
    // indirection stream instead of forcing the instance table to be reordered.
    impl_->dynamic_scene_slots.BeginFrame(scene_new_session);
    std::vector<GpuStaticDrawRef> dynamic_draw_refs(instance_count);
    frame.static_input_ref_offset = static_ref_data_offset;
    frame.dynamic_input_ref_offset = dynamic_ref_data_offset;
    frame.static_input_ref_count = static_cast<std::uint32_t>(static_draw_refs.size());
    frame.dynamic_input_ref_count = static_cast<std::uint32_t>(dynamic_draw_refs.size());
    for (std::size_t index = 0; index < instance_count; ++index)
    {
        const auto &source = render_instances[index];
        const auto &delta = scene_delta.instances[ordered_source_indices[index]];
        const auto environment = is_environment_mesh(source.mesh);
        const auto monster = is_monster(source.mesh);
        const auto skinned = source.mesh == RenderMesh::Archer || monster;
        GpuSceneMetadata scene_metadata{
            source.asset_id.value,
            source.material_id.value,
            std::numeric_limits<std::uint32_t>::max(),
            skinned ? kCachedSkinFlag : 0u,
            0u,
            entity_surface_frame.InstancePayload(source.stable_id)};
        const auto slot = impl_->dynamic_scene_slots.Upsert(
            delta.key, next_draw_instances[index], delta.revisions, scene_metadata);
        if (slot.conflict)
            return Result::Failure(
                ErrorCode::InvalidState, "hs_renderer_d3d12",
                "A stable render instance produced conflicting persistent GPU payloads.");
        if (skinned) dynamic_skin_slots[index] = slot.index;
        auto draw_flags = environment
            ? (ordered_lods[index].complement ? 0x20000u : 0u)
            : dynamic_pose_flags[index];
        if (skinned) draw_flags |= kCachedSkinFlag;
        if (skinned && dynamic_skin_history[index] &&
            slot.index < previous_gpu_instances.size() && previous_instance_snapshot &&
            slot.index < previous_instance_snapshot->generations.size() &&
            previous_instance_snapshot->generations[slot.index] == slot.generation &&
            (static_cast<std::size_t>(slot.index) + 1) * skin_slot_stride <=
                previous_skin_frame->skin_cache_vertex_capacity)
            draw_flags |= kCachedSkinHistoryFlag;
        dynamic_draw_refs[index] = {
            slot.index,
            draw_flags,
            environment ? ordered_lods[index].threshold
                        : (monster ? dynamic_pose_times[index] : 1.0f),
            slot.generation};
    }
    impl_->dynamic_scene_slots.EndFrame();

    const auto required_slots = impl_->dynamic_scene_slots.SlotCount();
    constexpr std::size_t kSkinBoundStride = sizeof(float) * 4;
    const bool have_skinned_instances = std::ranges::any_of(
        dynamic_skin_slots, [](std::uint32_t slot) {
            return slot != std::numeric_limits<std::uint32_t>::max();
        });
    std::uint64_t skin_signature = 1469598103934665603ull;
    const auto skin_hash = [&](const void *data, std::size_t bytes) {
        const auto *input = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < bytes; ++i) { skin_signature ^= input[i]; skin_signature *= 1099511628211ull; }
    };
    // Camera-dependent static/environment LOD expansion is not deformation.
    // Hash only canonical skinned slots and their current pose; each view uses
    // the same immutable deformation even when its draw stream differs.
    for (std::size_t index = 0; index < dynamic_skin_slots.size(); ++index)
        if (dynamic_skin_slots[index] != std::numeric_limits<std::uint32_t>::max())
        {
            skin_hash(&dynamic_skin_slots[index], sizeof(dynamic_skin_slots[index]));
            skin_hash(&dynamic_pose_flags[index], sizeof(dynamic_pose_flags[index]));
            skin_hash(&dynamic_pose_times[index], sizeof(dynamic_pose_times[index]));
        }
    skin_hash(palette_keys.data(), palette_keys.size() * sizeof(ArcherPaletteKey));
    skin_hash(constants->archer_bones, sizeof(constants->archer_bones));
    skin_hash(&required_slots, sizeof(required_slots));
    const bool reuse_canonical_skin = have_skinned_instances && impl_->canonical_skin_current &&
        impl_->canonical_skin_current->package_id == impl_->active_render_package_id &&
        impl_->canonical_skin_current->signature == skin_signature &&
        impl_->canonical_skin_current->skin_cache_slot_stride == skin_slot_stride;
    const auto completed_skin = impl_->graph_queue_executor.Completed();
    std::erase_if(impl_->canonical_skin_frames, [&](const auto &lease) {
        return lease.use_count() == 1 && lease->skin_cache_use_completion.IsSatisfiedBy(completed_skin);
    });
    if (reuse_canonical_skin)
    {
        const auto &canonical = *impl_->canonical_skin_current;
        frame.skin_cache_vertices = Impl::RetainViewAllocation(canonical.skin_cache_vertices);
        frame.skin_cache_bounds = Impl::RetainViewAllocation(canonical.skin_cache_bounds);
        frame.skin_cache_vertex_capacity = canonical.skin_cache_vertex_capacity;
        frame.skin_cache_bound_capacity = canonical.skin_cache_bound_capacity;
        frame.skin_cache_slot_stride = canonical.skin_cache_slot_stride;
        frame.skin_cache_vertex_read = frame.skin_cache_bounds_read = true;
    }
    // The pool retains immutable scene deformation snapshots referenced by a
    // view. A reused mutable frame slot gets fresh storage only if still leased.
    const bool leased_skin_slot = !reuse_canonical_skin && std::ranges::any_of(impl_->canonical_skin_frames,
        [&](const auto &lease) {
            return (lease.use_count() > 1 || !lease->skin_cache_use_completion.IsSatisfiedBy(completed_skin)) &&
                lease->skin_cache_vertices.resource.Get() == frame.skin_cache_vertices.resource.Get();
        });
    if (have_skinned_instances)
    {
        if (required_slots > std::numeric_limits<std::size_t>::max() / skin_slot_stride ||
            required_slots * skin_slot_stride >
                std::numeric_limits<std::size_t>::max() / sizeof(Vertex))
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Skin cache buffer size overflow.");
        const auto required_skin_vertices = required_slots * skin_slot_stride;
        if (leased_skin_slot || !frame.skin_cache_vertices.resource ||
            frame.skin_cache_vertex_capacity < required_skin_vertices ||
            frame.skin_cache_slot_stride != skin_slot_stride)
        {
            AllocationResource next_skin_buffer;
            D3D12MA::ALLOCATION_DESC allocation{};
            allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            if (auto created = impl_->CreateAllocation(
                    next_skin_buffer, allocation,
                    BufferDescription(required_skin_vertices * sizeof(Vertex),
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                !created)
                return created;
            // WaitForFrame covered this allocation's producer and last reader.
            // The replacement starts in UAV state with no submitted history.
            frame.skin_cache_vertices = std::move(next_skin_buffer);
            frame.skin_cache_vertex_capacity = required_skin_vertices;
            frame.skin_cache_slot_stride = skin_slot_stride;
            frame.skin_cache_vertex_read = false;
            frame.skin_cache_valid = false;
        }
        if (leased_skin_slot || !frame.skin_cache_bounds.resource ||
            frame.skin_cache_bound_capacity < required_slots)
        {
            AllocationResource next_skin_bounds;
            D3D12MA::ALLOCATION_DESC allocation{};
            allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            if (auto created = impl_->CreateAllocation(
                    next_skin_bounds, allocation,
                    BufferDescription(required_slots * kSkinBoundStride,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                !created)
                return created;
            frame.skin_cache_bounds = std::move(next_skin_bounds);
            frame.skin_cache_bound_capacity = required_slots;
            frame.skin_cache_bounds_read = false;
            frame.skin_cache_valid = false;
        }
    }
    std::size_t dynamic_scene_capacity =
        std::max<std::size_t>(impl_->dynamic_instance_capacity, 256);
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
    if (required_slots > impl_->dynamic_instance_capacity ||
        !impl_->dynamic_instances.resource ||
        impl_->dynamic_instances.resource->GetDesc().Width < dynamic_scene_layout.total_bytes)
    {
        AllocationResource next_buffer;
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = impl_->CreateAllocation(
                next_buffer, allocation,
                BufferDescription(dynamic_scene_layout.total_bytes),
                D3D12_RESOURCE_STATE_COPY_DEST);
            !created) return created;
        if (impl_->dynamic_instances.resource)
        {
            Impl::RetiredDynamicInstanceBuffer retired;
            retired.instances = std::move(impl_->dynamic_instances);
            retired.completion = retired.instances.graph_completion;
            impl_->retired_dynamic_instance_buffers.push_back(std::move(retired));
        }
        impl_->dynamic_instances = std::move(next_buffer);
        impl_->dynamic_instance_capacity = dynamic_scene_capacity;
        impl_->dynamic_instances_shader_read = false;
    }
    // An allocated replacement may survive an aborted preparation. Its first
    // successful submission still needs every live payload, even without growth.
    if (!impl_->dynamic_instances_shader_read)
        impl_->dynamic_scene_slots.MarkAllActiveDirty();
    if (auto prepared = impl_->PrepareLodStateBuffer(
            impl_->dynamic_lod_states, dynamic_scene_capacity); !prepared) return prepared;

    struct DynamicSceneCopyRegion
    {
        std::size_t destination_offset{};
        std::size_t byte_count{};
        std::size_t staging_offset{};
    };
    std::vector<PersistentGpuSceneRange> dirty_instance_ranges;
    std::vector<DynamicSceneCopyRegion> dynamic_scene_copy_regions;
    const auto dynamic_instance_base =
        impl_->dynamic_instances.resource->GetGPUVirtualAddress();
    std::size_t dynamic_instance_upload_bytes{};
    std::size_t dynamic_scene_staging_bytes{};
    const auto persistent_instances = impl_->dynamic_scene_slots.Instances();
    const auto persistent_metadata = impl_->dynamic_scene_slots.Metadata();
    const bool upload_dynamic_header =
        !impl_->dynamic_instances_shader_read ||
        required_slots != impl_->dynamic_instance_slot_count;
    if (upload_dynamic_header)
    {
        std::memcpy(frame.mapped + kInstanceDataOffset,
                    &dynamic_scene_layout.header,
                    sizeof(dynamic_scene_layout.header));
        dynamic_scene_copy_regions.push_back(
            {0, sizeof(dynamic_scene_layout.header), 0});
        dynamic_scene_staging_bytes = sizeof(dynamic_scene_layout.header);
    }
    for (const auto &range : impl_->dynamic_scene_slots.DirtyRanges())
    {
        dirty_instance_ranges.push_back(range);
        dynamic_instance_upload_bytes += range.count * kGpuSceneSlotPayloadBytes;
        std::vector<GpuSceneSerializedSlot> serialized(range.count);
        for (std::size_t index = 0; index < range.count; ++index)
        {
            const auto slot = range.begin + index;
            serialized[index] = SerializeGpuSceneSlot(
                static_cast<std::uint32_t>(slot),
                persistent_instances[slot], persistent_metadata[slot]);
        }
        const auto append_entries =
            [&](std::size_t destination_base, std::size_t stride, auto member) {
                const auto staging_offset = dynamic_scene_staging_bytes;
                for (std::size_t index = 0; index < serialized.size(); ++index)
                {
                    const auto &entry = serialized[index].*member;
                    std::memcpy(
                        frame.mapped + kInstanceDataOffset +
                            dynamic_scene_staging_bytes,
                        &entry, stride);
                    dynamic_scene_staging_bytes += stride;
                }
                dynamic_scene_copy_regions.push_back(
                    {destination_base + range.begin * stride,
                     range.count * stride, staging_offset});
            };
        const auto &header = dynamic_scene_layout.header;
        append_entries(header.instance_offset, sizeof(GpuSceneInstanceEntry),
                       &GpuSceneSerializedSlot::instance);
        append_entries(header.transform_offset, sizeof(GpuSceneTransformEntry),
                       &GpuSceneSerializedSlot::transform);
        append_entries(header.mesh_offset, sizeof(GpuSceneMeshEntry),
                       &GpuSceneSerializedSlot::mesh);
        append_entries(header.material_offset, sizeof(GpuSceneMaterialEntry),
                       &GpuSceneSerializedSlot::material);
        append_entries(header.bounds_offset, sizeof(GpuSceneBoundsEntry),
                       &GpuSceneSerializedSlot::bounds);
        append_entries(header.skin_offset, sizeof(GpuSceneSkinEntry),
                       &GpuSceneSerializedSlot::skin);
    }
    if (!entity_surface_frame.Bytes().empty())
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

    const bool upload_static_scene = static_scene && impl_->static_gpu_scene &&
                                      !impl_->static_gpu_scene->submitted;
    const bool upload_dynamic_scene =
        upload_dynamic_header || !dirty_instance_ranges.empty() || !entity_surface_frame.Bytes().empty();
    const bool upload_environment_stream = impl_->environment_stream.upload_pending;
    const bool upload_environment_geometry =
        std::ranges::any_of(impl_->environment_geometry_streams,
                           [](const auto &stream) { return stream.upload_pending; });
    auto &ui_texture = impl_->ui_textures[back_buffer_index];
    HRESULT result{};

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(impl_->render_width),
                                  static_cast<float>(impl_->render_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(impl_->render_width),
                             static_cast<LONG>(impl_->render_height)};
    const D3D12_VIEWPORT output_viewport{0.0f, 0.0f, static_cast<float>(impl_->width),
                                         static_cast<float>(impl_->height), 0.0f, 1.0f};
    const D3D12_RECT output_scissor{0, 0, static_cast<LONG>(impl_->width),
                                    static_cast<LONG>(impl_->height)};
    const auto rtv_start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto rtv_at = [&](std::uint32_t index) {
        return D3D12_CPU_DESCRIPTOR_HANDLE{
            rtv_start.ptr + static_cast<SIZE_T>(index) * impl_->rtv_stride};
    };
    const auto rtv = rtv_at(back_buffer_index);
    const auto gbuffer_base_rtv = rtv_at(kFrameCount);
    const auto gbuffer_normal_rtv = rtv_at(kFrameCount + 1);
    const auto gbuffer_metadata_rtv = rtv_at(kFrameCount + 2);
    const auto hdr_rtv = rtv_at(kFrameCount + 3);
    const auto oit_accumulation_rtv = rtv_at(kFrameCount + 4);
    const auto oit_revealage_rtv = rtv_at(kFrameCount + 5);
    const auto post_a_rtv = rtv_at(kFrameCount + 6);
    const auto post_b_rtv = rtv_at(kFrameCount + 7);
    const auto scene_work_rtv = rtv_at(kViewRtvOffset + 4);
    const auto display_linear_rtv = rtv_at(kViewRtvOffset + 5);
    const auto composite_hdr_rtv = rtv_at(kViewRtvOffset + 6);
    const auto gbuffer_material_rtv = rtv_at(kFrameCount + 8);
    const auto gbuffer_motion_rtv = rtv_at(kFrameCount + 9);
    const auto gbuffer_surface_history_rtv = rtv_at(kSurfaceHistoryRtvOffset);
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> surface_rtvs{
        gbuffer_base_rtv, gbuffer_normal_rtv, gbuffer_metadata_rtv, gbuffer_material_rtv,
        gbuffer_motion_rtv, gbuffer_surface_history_rtv,
        rtv_at(kMaterialSidecarRtvOffset), rtv_at(kMaterialSidecarRtvOffset + 1)};
    const auto surface_target_count = 6u + impl_->material_sidecar_layout.plane_count;
    const auto distortion_rtv = rtv_at(kDistortionRtvOffset + back_buffer_index);
    const auto bloom_half_rtv = rtv_at(kBloomRtvOffset + 3 * back_buffer_index);
    const auto bloom_quarter_rtv = rtv_at(kBloomRtvOffset + 3 * back_buffer_index + 1);
    const auto bloom_half_combined_rtv = rtv_at(kBloomRtvOffset + 3 * back_buffer_index + 2);
    const auto temporal_read_index = impl_->temporal_read_index;
    const auto temporal_write_index = temporal_read_index ^ 1u;
    const auto temporal_write_rtv = rtv_at(kTemporalRtvOffset + temporal_write_index);
    const std::array temporal_rtvs{temporal_write_rtv,
        rtv_at(kViewRtvOffset + temporal_write_index),
        rtv_at(kViewRtvOffset + 2 + temporal_write_index)};
    const auto exposure_read_index = impl_->exposure_read_index;
    const auto exposure_write_index = exposure_read_index ^ 1u;
    impl_->WriteViewDescriptors(back_buffer_index, temporal_read_index, temporal_write_index,
        exposure_read_index, exposure_write_index,
        frame.upload.resource->GetGPUVirtualAddress() + kExposureConstantsOffset);
    const bool initialize_temporal_targets = impl_->temporal_history[0].resource &&
        !impl_->temporal_targets_initialized;
    const bool initialize_offscreen_output = !presented_view &&
        !impl_->back_buffer_completion[back_buffer_index].direct &&
        !impl_->back_buffer_completion[back_buffer_index].compute &&
        !impl_->back_buffer_completion[back_buffer_index].copy;
    const auto dsv = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    auto readonly_dsv = dsv;
    readonly_dsv.ptr += 4 * impl_->dsv_stride;
    ID3D12DescriptorHeap *descriptor_heaps[] = {impl_->srv_heap.Get(), impl_->sampler_heap.Get()};
    auto sampler_table = impl_->sampler_heap->GetGPUDescriptorHandleForHeapStart();
    sampler_table.ptr += static_cast<UINT64>(back_buffer_index) * 2 * impl_->sampler_stride;
    auto texture_table = impl_->srv_heap->GetGPUDescriptorHandleForHeapStart();
    texture_table.ptr += static_cast<UINT64>(back_buffer_index) *
                         kTextureDescriptorCount * impl_->srv_stride;
    auto distortion_table = texture_table;
    // Share the frame-transient SRV layout used by descriptor creation. The
    // preceding environment table can grow without shifting this binding.
    distortion_table.ptr += static_cast<UINT64>(kVfxDistortionDescriptorOffset) * impl_->srv_stride;
    auto decal_table = texture_table;
    decal_table.ptr += static_cast<UINT64>(kVfxDistortionDescriptorOffset + 3u) * impl_->srv_stride;
    auto bloom_table = texture_table;
    bloom_table.ptr += static_cast<UINT64>(kBloomTextureDescriptorOffset) * impl_->srv_stride;
    auto temporal_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    temporal_cpu.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
                         kTemporalTextureDescriptorOffset) * impl_->srv_stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC temporal_view{};
    temporal_view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    temporal_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    temporal_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    temporal_view.Texture2D.MipLevels = 1;
    impl_->device->CreateShaderResourceView(
        impl_->temporal_history[temporal_read_index].resource.Get(), &temporal_view,
        temporal_cpu);
    temporal_cpu.ptr += impl_->srv_stride;
    impl_->device->CreateShaderResourceView(
        impl_->temporal_history[temporal_write_index].resource.Get(), &temporal_view,
        temporal_cpu);
    auto temporal_table = texture_table;
    temporal_table.ptr += static_cast<UINT64>(kTemporalTextureDescriptorOffset) *
                          impl_->srv_stride;
    temporal_table.ptr += impl_->srv_stride;
    const auto alternate_texture_table = [&](std::uint32_t offset,
                                              ID3D12Resource *input) {
        auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        destination.ptr += (static_cast<SIZE_T>(back_buffer_index) *
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
            impl_->temporal_history[temporal_write_index].resource.Get());
    auto fxaa_input = texture_table;
    if (!impl_->config.outline)
        fxaa_input = alternate_texture_table(kFxaaInputDescriptorOffset,
                                             impl_->post_a.resource.Get());
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
        auto history_table = texture_table;
        history_table.ptr += static_cast<UINT64>(kTemporalTextureDescriptorOffset) * impl_->srv_stride;
        list->SetComputeRootDescriptorTable(32, history_table);
        const std::array<UINT, 2> defaults{};
        list->SetComputeRoot32BitConstants(6, 2, defaults.data(), 0);
    };
    const auto bind_surface_compute = [&](ID3D12GraphicsCommandList *list) {
        bind_compute(list);
        list->SetComputeRootDescriptorTable(13, character_table);
        list->SetComputeRootShaderResourceView(18, previous_skin_frame_valid
            ? previous_skin_frame->skin_cache_vertices.resource->GetGPUVirtualAddress()
            : frame.upload.resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(19,
            frame.upload.resource->GetGPUVirtualAddress() + previous_instance_data_offset);
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

    if (use_visibility)
        if (auto result = impl_->WriteVisibilityDescriptors(back_buffer_index,
                static_scene && impl_->static_gpu_scene
                    ? impl_->static_gpu_scene->instances.resource.Get()
                    : impl_->dynamic_instances.resource.Get(),
                impl_->dynamic_instances.resource.Get(), static_ref_data_offset,
                static_cast<std::uint32_t>(static_draw_refs.size()), dynamic_ref_data_offset,
                static_cast<std::uint32_t>(dynamic_draw_refs.size())); !result) return result;
    impl_->graph.Reset();
    std::vector<TextureStreamingGraphBinding> texture_bindings;
    auto copy_queue = std::make_shared<ComPtr<ID3D12CommandQueue>>(
        impl_->graph_queue_executor.NativeQueue(QueueHint::Copy));
    ExternalQueueBinding texture_copy_queue{std::shared_ptr<void>(copy_queue, copy_queue->Get()), QueueHint::Copy, {}};
    if (auto result = impl_->texture_streaming.AddPendingPasses(impl_->graph, std::move(texture_copy_queue),
            1000, texture_bindings); !result) return result;
    std::unordered_map<ID3D12Resource *, ResourceHandle> canonical_textures;
    for (const auto &binding : texture_bindings)
        canonical_textures.emplace(impl_->texture_streaming.View(binding.texture).resource, binding.sampled.resource);
    const auto texture_metadata = impl_->texture_feedback.ImportMetadata(impl_->graph, back_buffer_index);
    std::vector<TextureFeedbackGraphBinding> feedback_bindings;
    if (auto result = impl_->texture_feedback.AddClearPass(impl_->graph, back_buffer_index,
            1200, feedback_bindings); !result) return result;
    ResourceVersion entity_surface_noise_version, entity_surface_gradient_version, entity_surface_stbn_version;
    const auto read_material_sampling = [&](PassBuilder &pass, bool feedback_writer,
                                           Access sampling = Access::AllShaderRead) {
        for (const auto &binding : texture_bindings) pass.Read(binding.sampled, sampling);
        pass.Read(texture_metadata, sampling);
        if (entity_surface_noise_version.resource)
        {
            pass.Read(entity_surface_noise_version, sampling);
            pass.Read(entity_surface_gradient_version, sampling);
            pass.Read(entity_surface_stbn_version, sampling);
        }
        if (feedback_writer) RendererTextureFeedback::DeclareSampling(pass, feedback_bindings);
    };
    const auto authored_source = [&](const RenderInstance &source) {
        return is_environment_mesh(source.mesh) || source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch;
    };
    const auto masked_source = [&](const RenderInstance &source) {
        return is_environment_mesh(source.mesh) && render_data::ClassifyEnvironmentMaterial(
            impl_->environment_meshes[environment_mesh_index(source) * 3].material) ==
                render_data::EnvironmentMaterialVariant::MaskedTwoSided;
    };
    const bool has_authored_material_draw = std::ranges::any_of(render_instances, authored_source) ||
        (static_scene && std::ranges::any_of(static_scene->instances, authored_source));
    const bool has_masked_material_draw = std::ranges::any_of(render_instances, masked_source) ||
        (static_scene && std::ranges::any_of(static_scene->instances, masked_source));
    const bool has_static_authored_material_draw = static_scene &&
        std::ranges::any_of(static_scene->instances, authored_source);
    const bool has_static_masked_material_draw = static_scene &&
        std::ranges::any_of(static_scene->instances, masked_source);
    std::vector<std::pair<AllocationResource *, ResourceHandle>> imported_resources;
    const auto import_buffer = [&](AllocationResource &resource, Access initial,
                                   std::string_view name) {
        for (const auto &[imported, handle] : imported_resources)
            if (imported == &resource) return handle;
        const auto access = resource.graph_access == Access::Undefined
            ? initial : resource.graph_access;
        const auto handle = impl_->graph.ImportBuffer(
            {resource.resource.Get(), resource.graph_completion, resource.graph_queue}, access, name);
        imported_resources.emplace_back(&resource, handle);
        return handle;
    };
    const auto import_texture = [&](AllocationResource &resource, Access initial,
                                    std::string_view name) {
        if (const auto existing = canonical_textures.find(resource.resource.Get()); existing != canonical_textures.end())
        {
            if (std::ranges::none_of(imported_resources, [&](const auto &entry) { return entry.first == &resource; }))
                imported_resources.emplace_back(&resource, existing->second);
            return existing->second;
        }
        for (const auto &[imported, handle] : imported_resources)
            if (imported == &resource) return handle;
        const auto access = resource.graph_access == Access::Undefined
            ? initial : resource.graph_access;
        const auto handle = impl_->graph.ImportTexture(
            {resource.resource.Get(), resource.graph_completion, resource.graph_queue}, access, name);
        imported_resources.emplace_back(&resource, handle);
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
    auto &dynamic_lod_state = impl_->dynamic_lod_states;
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
        std::array<BufferHandle, kGeometryBuffersPerPage> buffers{};
        std::array<ResourceVersion, kGeometryBuffersPerPage> versions{};
    };
    std::vector<GeometryStreamUploadGraph> environment_geometry_uploads;
    for (auto &stream : impl_->environment_geometry_streams)
    {
        if (!stream.upload_pending) continue;
        GeometryStreamUploadGraph upload{&stream};
        auto &candidate = stream.candidate;
        const std::array resources{&candidate.vertices, &candidate.indices,
            &candidate.meshlets, &candidate.auxiliary};
        constexpr std::array names{"GeometryStreamVertices", "GeometryStreamIndices",
            "GeometryStreamMeshlets", "GeometryStreamAuxiliary"};
        for (std::size_t i = 0; i < resources.size(); ++i)
        {
            upload.buffers[i] = import_buffer(*resources[i], Access::CopyDest, names[i]);
            upload.versions[i] = impl_->graph.Version(upload.buffers[i]);
        }
        environment_geometry_uploads.push_back(upload);
    }
    const auto distortion_vectors = impl_->graph.CreateTexture(
        {impl_->render_width,impl_->render_height,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET}, "VFX Distortion Vectors");
    const auto ribbon_history = import_buffer(impl_->ribbon_history, Access::UnorderedWrite, "RibbonHistory");
    const auto ribbon_previous = import_buffer(impl_->ribbon_previous, Access::UnorderedWrite, "RibbonPrevious");
    const auto ribbon_arguments = import_buffer(impl_->ribbon_arguments, Access::UnorderedWrite, "RibbonArguments");
    const auto draw_ribbons = [&](ID3D12GraphicsCommandList *list, bool oit) {
        list->SetPipelineState(oit ? impl_->ribbon_oit_pipeline.Get() : impl_->ribbon_add_pipeline.Get());
        list->SetGraphicsRootShaderResourceView(18, frame.upload.resource->GetGPUVirtualAddress() + ribbon_output_offset);
        list->SetGraphicsRootShaderResourceView(19, impl_->ribbon_history.resource->GetGPUVirtualAddress());
        list->SetGraphicsRootShaderResourceView(20, impl_->ribbon_previous.resource->GetGPUVirtualAddress());
        const auto split = std::ranges::find_if(ribbon_outputs, [](const GpuRibbonOutput &output) {
            return (output.metadata[3] & 1u) != 0;
        });
        const auto first = oit ? static_cast<std::size_t>(split - ribbon_outputs.begin()) : 0;
        const auto count = oit ? ribbon_outputs.size() - first : static_cast<std::size_t>(split - ribbon_outputs.begin());
        if (count != 0)
            list->ExecuteIndirect(impl_->ribbon_draw_signature.Get(), static_cast<UINT>(count),
                impl_->ribbon_arguments.resource.Get(), first * kRibbonArgumentStride, nullptr, 0);
        list->SetGraphicsRoot32BitConstant(6, 0, 0);
    };
    const auto particles = import_buffer(impl_->particles, Access::UnorderedWrite, "Particles");
    const auto particle_input_index = impl_->particle_input_is_a ? 0u : 1u;
    const auto particle_output_index = particle_input_index ^ 1u;
    const auto particle_alive_input = import_buffer(impl_->particle_alive[particle_input_index], Access::UnorderedWrite, "ParticleAliveInput");
    const auto particle_alive_output = import_buffer(impl_->particle_alive[particle_output_index], Access::UnorderedWrite, "ParticleAliveOutput");
    const auto particle_dead = import_buffer(impl_->particle_dead, Access::UnorderedWrite, "ParticleDead");
    const auto particle_counters = import_buffer(impl_->particle_counters, Access::UnorderedWrite, "ParticleCounters");
    const auto indirect_arguments = import_buffer(impl_->indirect_arguments, Access::UnorderedWrite, "IndirectArguments");
    const auto dynamic_instances_buffer = import_buffer(impl_->dynamic_instances, impl_->dynamic_instances_shader_read ? Access::ShaderRead : Access::CopyDest, "DynamicSceneInstances");
    const auto geometry_records = import_buffer(frame.geometry_draw_records, Access::CopyDest,
                                                "GeometryDrawRecords");
    struct GeometryPageGraph
    {
        const GeometryPageGpu *page{};
        std::array<ResourceVersion, kGeometryBuffersPerPage> versions{};
    };
    std::vector<GeometryPageGraph> geometry_pages;
    for (const auto &record : frame.geometry_draw_records_cpu)
    {
        // Shared hierarchy/HLOD pages are imported once by ClusterFrameGraph.
        if (record.page_slot > kProceduralGeometryPage) continue;
        if (record.geometry_generation == 0 || std::ranges::any_of(geometry_pages,
                [&](const auto &entry) { return entry.page->page_slot == record.page_slot; })) continue;
        const auto *page = impl_->FindGeometryPage(record.page_slot);
        if (!page)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Geometry graph has no page for a prepared draw record.");
        GeometryPageGraph entry{page};
        const std::array resources{&page->vertices, &page->indices, &page->meshlets, &page->auxiliary};
        constexpr std::array access{Access::VertexBuffer, Access::IndexBuffer,
                                    Access::ComputeRead, Access::ComputeRead};
        constexpr std::array names{"GeometryVertices", "GeometryIndices", "GeometryMeshlets", "GeometryAuxiliary"};
        for (std::size_t i = 0; i < resources.size(); ++i)
            entry.versions[i] = impl_->graph.Version(import_buffer(
                const_cast<AllocationResource &>(*resources[i]), access[i], names[i]));
        geometry_pages.push_back(entry);
    }
    const auto read_geometry_pages = [&](auto &pass, bool skin_only = false) {
        for (const auto &entry : geometry_pages)
        {
            if (skin_only)
            {
                if (entry.page->page_slot >= kArcherGeometryPage &&
                    entry.page->page_slot < kProjectileGeometryPage)
                    pass.Read(entry.versions[0], Access::ComputeRead);
                continue;
            }
            pass.Read(entry.versions[0], impl_->mesh_shader_enabled
                ? Access::VertexShaderRead : Access::VertexBuffer);
            pass.Read(entry.versions[1], Access::IndexBuffer);
            if (impl_->mesh_shader_enabled)
            {
                pass.Read(entry.versions[2], Access::ShaderRead);
                pass.Read(entry.versions[3], Access::ShaderRead);
            }
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
    const auto static_visible_refs = import_buffer(frame.static_visible_refs, Access::UnorderedWrite, "StaticVisibleRefs");
    const auto static_draw_args = import_buffer(frame.static_draw_args, Access::UnorderedWrite, "StaticDrawArguments");
    const auto dynamic_visible_refs = import_buffer(frame.dynamic_visible_refs, Access::UnorderedWrite, "DynamicVisibleRefs");
    const auto dynamic_draw_args = import_buffer(frame.dynamic_draw_args, Access::UnorderedWrite, "DynamicDrawArguments");
    const auto static_rejected_refs = import_buffer(frame.static_rejected_refs, Access::UnorderedWrite, "StaticRejectedRefs");
    const auto static_rejected_args = import_buffer(frame.static_rejected_args, Access::UnorderedWrite, "StaticRejectedArguments");
    const auto static_retest_refs = import_buffer(frame.static_retest_refs, Access::UnorderedWrite, "StaticRetestRefs");
    const auto static_retest_args = import_buffer(frame.static_retest_args, Access::UnorderedWrite, "StaticRetestArguments");
    const auto hzb_write_index = impl_->hzb_read_index ^ 1u;
    const auto previous_hzb = import_buffer(impl_->hzb_history[impl_->hzb_read_index], Access::ComputeRead, "PreviousHZB");
    const auto current_hzb = import_buffer(impl_->hzb_history[hzb_write_index], Access::ComputeRead, "CurrentHZB");
    const auto initial_depth_access =
        impl_->transient_textures_common ? Access::Common : Access::DepthWrite;
    const auto initial_color_access =
        impl_->transient_textures_common ? Access::Common : Access::ShaderRead;
    const auto shadow =
        import_texture(impl_->shadow, initial_depth_access, "Shadow");
    const auto depth =
        import_texture(impl_->depth, initial_depth_access, "Depth");
    const auto gbuffer_base = import_texture(impl_->gbuffer_base, initial_color_access, "GBufferBase");
    const auto gbuffer_normal = import_texture(impl_->gbuffer_normal, initial_color_access, "GBufferNormal");
    const auto gbuffer_metadata = import_texture(impl_->gbuffer_metadata, initial_color_access, "GBufferMetadata");
    const auto hdr = import_texture(impl_->hdr_color, initial_color_access, "HdrColor");
    const auto subsurface_diffuse = import_texture(impl_->subsurface_diffuse, initial_color_access, "SubsurfaceDiffuse");
    const auto environment_specular = import_texture(impl_->environment_specular, initial_color_access, "EnvironmentSpecular");
    const auto indirect_diffuse = import_texture(impl_->indirect_diffuse, initial_color_access, "IndirectDiffuse");
    const auto oit_accumulation = import_texture(impl_->oit_accumulation, initial_color_access, "OitAccumulation");
    const auto oit_revealage = import_texture(impl_->oit_revealage, initial_color_access, "OitRevealage");
    const auto post_a = import_texture(impl_->post_a, initial_color_access, "PostA");
    const auto gbuffer_material = import_texture(impl_->gbuffer_material, initial_color_access, "GBufferMaterial");
    const auto gbuffer_motion = import_texture(impl_->gbuffer_motion, initial_color_access, "GBufferMotion");
    const auto gbuffer_surface_history = import_texture(impl_->gbuffer_surface_history,
        initial_color_access, "SurfaceHistory");
    std::array<ResourceVersion, 2> material_sidecar_versions{};
    for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
    {
        const auto access = !impl_->material_sidecars_initialized && impl_->enhanced
            ? Access::Common : Access::ShaderRead;
        const auto handle = import_texture(impl_->material_sidecars[plane], access,
                                           "MaterialSidecar" + std::to_string(plane));
        material_sidecar_versions[plane] = impl_->graph.Version(handle);
    }
    const auto post_b = impl_->post_b.resource
        ? import_texture(impl_->post_b, initial_color_access, "PostB") : TextureHandle{};
    const auto temporal_initial_access = impl_->temporal_targets_initialized
        ? Access::ShaderRead
        : (impl_->enhanced ? Access::Common : Access::ShaderRead);
    const auto temporal_read = impl_->temporal_history[temporal_read_index].resource ? import_texture(
        impl_->temporal_history[temporal_read_index], temporal_initial_access,
        "TemporalHistoryRead") : TextureHandle{};
    const auto temporal_write = impl_->temporal_history[temporal_write_index].resource ? import_texture(
        impl_->temporal_history[temporal_write_index], temporal_initial_access,
        "TemporalHistoryWrite") : TextureHandle{};
    const auto view_initial_access = !impl_->view_targets_initialized && impl_->enhanced
        ? Access::Common : Access::ShaderRead;
    const auto scene_work = import_texture(impl_->scene_work, view_initial_access, "SceneWork");
    const auto display_linear = import_texture(impl_->display_linear, view_initial_access, "DisplayLinear");
    const auto composite_hdr = import_texture(impl_->composite_hdr, view_initial_access, "CompositeHdr");
    const auto temporal_normal_read = impl_->temporal_normal_history[temporal_read_index].resource
        ? import_texture(impl_->temporal_normal_history[temporal_read_index], view_initial_access, "TemporalNormalRead") : TextureHandle{};
    const auto temporal_normal_write = impl_->temporal_normal_history[temporal_write_index].resource
        ? import_texture(impl_->temporal_normal_history[temporal_write_index], view_initial_access, "TemporalNormalWrite") : TextureHandle{};
    const auto temporal_material_read = impl_->temporal_material_history[temporal_read_index].resource
        ? import_texture(impl_->temporal_material_history[temporal_read_index], view_initial_access, "TemporalMaterialRead") : TextureHandle{};
    const auto temporal_material_write = impl_->temporal_material_history[temporal_write_index].resource
        ? import_texture(impl_->temporal_material_history[temporal_write_index], view_initial_access, "TemporalMaterialWrite") : TextureHandle{};
    const auto exposure_read = import_buffer(impl_->exposure_values[exposure_read_index],
        impl_->exposure_initialized ? Access::AllShaderRead : Access::CopyDest, "ExposureCurrent");
    const auto exposure_write = import_buffer(impl_->exposure_values[exposure_write_index],
        impl_->exposure_initialized ? Access::AllShaderRead : Access::CopyDest, "ExposureNext");
    const auto exposure_histogram = import_buffer(impl_->exposure_histogram, Access::UnorderedWrite, "ExposureHistogram");
    TextureHandle bloom_half{};
    TextureHandle bloom_quarter{};
    TextureHandle bloom_half_combined{};
    const auto half_width = impl_->width / 2 + impl_->width % 2;
    const auto half_height = impl_->height / 2 + impl_->height % 2;
    const auto quarter_width = half_width / 2 + half_width % 2;
    const auto quarter_height = half_height / 2 + half_height % 2;
    if (impl_->config.bloom)
    {
        constexpr auto format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        constexpr auto flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        bloom_half = impl_->graph.CreateTexture(
            {half_width, half_height, format, flags}, "BloomHalf");
        bloom_quarter = impl_->graph.CreateTexture(
            {quarter_width, quarter_height, format, flags}, "BloomQuarter");
        bloom_half_combined = impl_->graph.CreateTexture(
            {half_width, half_height, format, flags}, "BloomHalfCombined");
    }
    const auto ui = import_texture(ui_texture, frame.ui_initialized ? Access::ShaderRead : Access::Common, "UiTexture");
    const auto back_buffer = impl_->graph.ImportTexture(
        {impl_->back_buffers[back_buffer_index].Get(),
         impl_->back_buffer_completion[back_buffer_index], QueueHint::Direct}, Access::Present, "BackBuffer");

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
    auto shadow_version = impl_->graph.Version(shadow);
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
        visibility_ids = import_texture(impl_->visibility_ids, Access::UnorderedWrite, "VisibilityIds");
        visibility_front_face = import_texture(impl_->visibility_front_face, Access::UnorderedWrite, "VisibilityFrontFace");
        raster_records = import_buffer(visibility_frame.raster_records, Access::CopyDest, "RasterDrawRecords");
        visibility_pixels = import_buffer(impl_->visibility_pixels, Access::CopyDest, "ClassifiedPixels");
        visibility_counts = import_buffer(impl_->visibility_counts, Access::CopyDest, "MaterialVariantCounts");
        visibility_args = import_buffer(impl_->visibility_dispatch_args, Access::CopyDest, "MaterialDispatchArgs");
        visibility_ids_version = impl_->graph.Version(visibility_ids);
        visibility_face_version = impl_->graph.Version(visibility_front_face);
        raster_records_version = impl_->graph.Version(raster_records);
        visibility_pixels_version = impl_->graph.Version(visibility_pixels);
        visibility_counts_version = impl_->graph.Version(visibility_counts);
        visibility_args_version = impl_->graph.Version(visibility_args);
    }
    auto hdr_version = impl_->graph.Version(hdr);
    auto *opaque_hdr_resource = impl_->hdr_color.resource.Get();
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
    auto *composite_hdr_resource = impl_->composite_hdr.resource.Get();
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

    if (!impl_->exposure_initialized)
    {
        const auto exposure_upload = import_buffer(impl_->initial_exposure, Access::CopySource, "InitialExposure");
        auto initialize = impl_->graph.AddPass(32, "ExposureInitialization", QueueHint::Copy);
        initialize.Read(impl_->graph.Version(exposure_upload), Access::CopySource);
        exposure_read_version = initialize.Write(exposure_read_version, Access::CopyDest);
        exposure_write_version = initialize.Write(exposure_write_version, Access::CopyDest);
        initialize.SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            for (auto &value : impl_->exposure_values)
                list->CopyBufferRegion(value.resource.Get(), 0, impl_->initial_exposure.resource.Get(), 0, 2 * sizeof(float));
        });
    }

    if (impl_->ui_dirty[back_buffer_index])
    {
    auto ui_upload_pass = impl_->graph.AddPass(23, kRenderPassNames[23], QueueHint::Copy);
    ui_version = ui_upload_pass.Write(ui_version, Access::CopyDest);
    ui_upload_pass.SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = ui_texture.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = frame.ui_upload.resource.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = impl_->ui_footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    });
    }

    if (initialize_temporal_targets)
    {
        auto initialize_pass = impl_->graph.AddPass(24, kRenderPassNames[24], QueueHint::Direct);
        temporal_read_version = initialize_pass.Write(temporal_read_version, Access::RenderTarget);
        temporal_write_version = initialize_pass.Write(temporal_write_version, Access::RenderTarget);
        temporal_normal_read_version = initialize_pass.Write(temporal_normal_read_version, Access::RenderTarget);
        temporal_normal_write_version = initialize_pass.Write(temporal_normal_write_version, Access::RenderTarget);
        temporal_material_read_version = initialize_pass.Write(temporal_material_read_version, Access::RenderTarget);
        temporal_material_write_version = initialize_pass.Write(temporal_material_write_version, Access::RenderTarget);
        initialize_pass.SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            // Discard initializes newly allocated render-target metadata after
            // the graph's transition; the clear defines every history pixel.
            list->DiscardResource(impl_->temporal_history[0].resource.Get(), nullptr);
            list->DiscardResource(impl_->temporal_history[1].resource.Get(), nullptr);
            for (auto &history : impl_->temporal_normal_history) list->DiscardResource(history.resource.Get(), nullptr);
            for (auto &history : impl_->temporal_material_history) list->DiscardResource(history.resource.Get(), nullptr);
            constexpr float clear[] = {0, 0, 0, 0};
            list->ClearRenderTargetView(rtv_at(kTemporalRtvOffset), clear, 0, nullptr);
            list->ClearRenderTargetView(rtv_at(kTemporalRtvOffset + 1), clear, 0, nullptr);
            for (std::uint32_t plane = 0; plane < 4; ++plane)
                list->ClearRenderTargetView(rtv_at(kViewRtvOffset + plane), clear, 0, nullptr);
        });
    }

    if (!frame.geometry_draw_records_cpu.empty())
    {
        auto upload_pass = impl_->graph.AddPass(0, kRenderPassNames[0], QueueHint::Copy);
        if (initialize_dynamic_lod)
            dynamic_lod_version = upload_pass.Write(dynamic_lod_version, Access::CopyDest);
        if (initialize_static_lod)
            static_lod_version = upload_pass.Write(static_lod_version, Access::CopyDest);
        if (dynamic_lod_prefix)
            upload_pass.Read(impl_->graph.Version(dynamic_lod_prefix), Access::CopySource);
        if (static_lod_prefix)
            upload_pass.Read(impl_->graph.Version(static_lod_prefix), Access::CopySource);
        geometry_records_version = upload_pass.Write(geometry_records_version, Access::CopyDest);
        if (upload_dynamic_scene)
            dynamic_instances_version = upload_pass.ReadWrite(dynamic_instances_version,
                                                              Access::CopyDest);
        if (upload_static_scene)
            static_instances_version = upload_pass.Write(static_instances_version,
                                                         Access::CopyDest);
        if (upload_environment_stream)
            environment_stream_version =
                upload_pass.Write(environment_stream_version, Access::CopyDest);
        if (upload_environment_geometry)
            for (auto &upload : environment_geometry_uploads)
                for (auto &version : upload.versions)
                    version = upload_pass.Write(version, Access::CopyDest);
        upload_pass.SetExecute([&](RenderPassContext &context) {
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
            if (auto result = impl_->UploadGeometryDrawRecords(list, back_buffer_index); !result)
                throw std::runtime_error(std::string(result.Message()));
            if (upload_static_scene)
            {
                const auto bytes = impl_->static_gpu_scene->layout.total_bytes;
                if (bytes != 0)
                    list->CopyBufferRegion(
                        impl_->static_gpu_scene->instances.resource.Get(), 0,
                        impl_->static_gpu_scene->upload.resource.Get(), 0, bytes);
            }
            for (const auto &region : dynamic_scene_copy_regions)
            {
                list->CopyBufferRegion(
                    impl_->dynamic_instances.resource.Get(),
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
                    impl_->RecordGeometryPageUpload(list, stream.candidate, stream.upload, stream.upload_offsets);
                }
        });
    }

    auto particle_pass = impl_->graph.AddPass(1, kRenderPassNames[1], QueueHint::Compute);
    particles_version = particle_pass.ReadWrite(particles_version, Access::UnorderedWrite);
    particle_alive_input_version = particle_pass.ReadWrite(particle_alive_input_version, Access::UnorderedWrite);
    particle_alive_output_version = particle_pass.ReadWrite(particle_alive_output_version, Access::UnorderedWrite);
    particle_dead_version = particle_pass.ReadWrite(particle_dead_version, Access::UnorderedWrite);
    particle_counters_version = particle_pass.ReadWrite(particle_counters_version, Access::UnorderedWrite);
    indirect_arguments_version = particle_pass.ReadWrite(indirect_arguments_version, Access::UnorderedWrite);
    ribbon_history_version = particle_pass.ReadWrite(ribbon_history_version, Access::UnorderedWrite);
    ribbon_previous_version = particle_pass.ReadWrite(ribbon_previous_version, Access::UnorderedWrite);
    ribbon_arguments_version = particle_pass.ReadWrite(ribbon_arguments_version, Access::UnorderedWrite);
    const auto initialize_particles = !impl_->particles_initialized;
    particle_pass.SetExecute([&](RenderPassContext &context) {
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

        if (initialize_particles)
        {
            dispatch_phase(0, particle_capacity);
            context.UavBarrier(particle_dead);
            context.UavBarrier(particle_counters);
            context.UavBarrier(indirect_arguments);
        }
        dispatch_phase(1, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
        dispatch_phase(2, particle_capacity);
        synchronize_particle_state();
        if (total_particles_to_spawn != 0)
        {
            dispatch_phase(3, total_particles_to_spawn);
            synchronize_particle_state();
        }
        dispatch_phase(4, particle_capacity);
        context.UavBarrier(particle_alive_output);
        context.UavBarrier(particle_counters);
        dispatch_phase(5, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
        list->SetComputeRootShaderResourceView(17, frame.upload.resource->GetGPUVirtualAddress() + ribbon_update_offset);
        list->SetComputeRootShaderResourceView(18, frame.upload.resource->GetGPUVirtualAddress() + ribbon_output_offset);
        list->SetComputeRootUnorderedAccessView(21, impl_->ribbon_history.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(22, impl_->ribbon_previous.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(23, impl_->ribbon_arguments.resource->GetGPUVirtualAddress());
        if (!ribbon_updates.empty())
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

    if (have_skinned_instances && !reuse_canonical_skin)
    {
        auto skin_cache_pass =
            impl_->graph.AddPass(22, kRenderPassNames[22], QueueHint::Compute);
        read_geometry_pages(skin_cache_pass, true);
        skin_cache_version = skin_cache_pass.Write(
            skin_cache_version, Access::UnorderedWrite);
        skin_bounds_version = skin_cache_pass.Write(
            skin_bounds_version, Access::UnorderedWrite);
        skin_cache_pass.SetExecute([&](RenderPassContext &context) {
            auto *list =
                static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->skin_cache_pipeline.Get());
            list->SetComputeRootShaderResourceView(
                17, impl_->monster_skin_matrices.resource->GetGPUVirtualAddress());
            for (std::size_t index = 0; index < render_instances.size(); ++index)
            {
                const auto slot = dynamic_skin_slots[index];
                if (slot == std::numeric_limits<std::uint32_t>::max()) continue;
                const auto mesh = render_instances[index].mesh;
                ID3D12Resource *source{};
                std::uint32_t vertex_count{};
                if (mesh == RenderMesh::Archer)
                {
                    source = impl_->archer_geometry.vertices.resource.Get();
                    vertex_count = impl_->archer_geometry.vertex_count;
                }
                else
                {
                    const auto asset = static_cast<std::size_t>(
                        static_cast<std::uint32_t>(mesh) -
                        static_cast<std::uint32_t>(RenderMesh::MonsterMelee));
                    if (asset >= impl_->monster_assets.size()) continue;
                    source = impl_->monster_assets[asset].vertices.resource.Get();
                    vertex_count = impl_->monster_assets[asset].vertex_count;
                }
                if (!source || vertex_count == 0) continue;
                const auto clip = (dynamic_pose_flags[index] >> 8u) & 0x0fu;
                const std::array<UINT, 2> skin_constants{
                    static_cast<UINT>(static_cast<std::uint32_t>(mesh) & 0xffu) |
                        static_cast<UINT>(clip << 8u) |
                        static_cast<UINT>(vertex_count << 12u),
                    std::bit_cast<UINT>(dynamic_pose_times[index])};
                const auto output_offset =
                    static_cast<UINT64>(slot) * skin_slot_stride * sizeof(Vertex);
                list->SetComputeRootShaderResourceView(
                    16, source->GetGPUVirtualAddress());
                if (mesh == RenderMesh::Archer)
                {
                    const auto palette_slot = archer_palette_slots[index];
                    list->SetComputeRootShaderResourceView(
                        17, frame.upload.resource->GetGPUVirtualAddress() +
                            archer_palette_data_offset +
                            static_cast<UINT64>(palette_slot) * archer_palette_stride);
                }
                else
                    list->SetComputeRootShaderResourceView(
                        17, impl_->monster_skin_matrices.resource->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(
                    3, frame.skin_cache_vertices.resource->GetGPUVirtualAddress() +
                           output_offset);
                list->SetComputeRoot32BitConstants(
                    6, static_cast<UINT>(skin_constants.size()), skin_constants.data(), 0);
                list->Dispatch((vertex_count + 63u) / 64u, 1, 1);
            }
            // All current deformation must be visible before the per-instance
            // reduction reads it back through the same UAV root binding.
            context.UavBarrier(skin_cache_buffer);
            list->SetPipelineState(impl_->skin_bounds_pipeline.Get());
            for (std::size_t index = 0; index < render_instances.size(); ++index)
            {
                const auto slot = dynamic_skin_slots[index];
                if (slot == std::numeric_limits<std::uint32_t>::max()) continue;
                const auto mesh = render_instances[index].mesh;
                std::uint32_t vertex_count{};
                if (mesh == RenderMesh::Archer)
                    vertex_count = impl_->archer_geometry.vertex_count;
                else
                {
                    const auto asset = static_cast<std::size_t>(
                        static_cast<std::uint32_t>(mesh) -
                        static_cast<std::uint32_t>(RenderMesh::MonsterMelee));
                    if (asset >= impl_->monster_assets.size()) continue;
                    vertex_count = impl_->monster_assets[asset].vertex_count;
                }
                if (vertex_count == 0) continue;
                const auto clip = (dynamic_pose_flags[index] >> 8u) & 0x0fu;
                const std::array<UINT, 2> skin_constants{
                    static_cast<UINT>(static_cast<std::uint32_t>(mesh) & 0xffu) |
                        static_cast<UINT>(clip << 8u) |
                        static_cast<UINT>(vertex_count << 12u),
                    std::bit_cast<UINT>(dynamic_pose_times[index])};
                const auto output_offset =
                    static_cast<UINT64>(slot) * skin_slot_stride * sizeof(Vertex);
                const auto bounds_offset =
                    static_cast<UINT64>(slot) * kSkinBoundStride;
                list->SetComputeRootUnorderedAccessView(
                    3, frame.skin_cache_vertices.resource->GetGPUVirtualAddress() +
                           output_offset);
                list->SetComputeRootUnorderedAccessView(
                    21, frame.skin_cache_bounds.resource->GetGPUVirtualAddress() +
                            bounds_offset);
                list->SetComputeRoot32BitConstants(
                    6, static_cast<UINT>(skin_constants.size()),
                    skin_constants.data(), 0);
                list->Dispatch(1, 1, 1);
            }
            context.UavBarrier(skin_bounds_buffer);
        });
    }

    const auto bind_scene_deformation_roots = [&](ID3D12GraphicsCommandList *list, bool cached_skin_enabled) {
        // Scene and ribbon shaders share these root slots. Bind the complete
        // scene contract after a signature change or the last ribbon draw.
        // Static-only draws keep the cache branches disabled and use the
        // resident undeformed vertex source instead of unrelated upload data.
        const bool cached_skin = cached_skin_enabled && have_skinned_instances;
        const auto current_vertices_base = cached_skin
            ? frame.skin_cache_vertices.resource->GetGPUVirtualAddress()
            : impl_->archer_geometry.vertices.resource->GetGPUVirtualAddress();
        const auto previous_vertices_base = cached_skin && previous_skin_frame_valid
            ? previous_skin_frame->skin_cache_vertices.resource->GetGPUVirtualAddress()
            : current_vertices_base;
        list->SetGraphicsRootShaderResourceView(18, previous_vertices_base);
        list->SetGraphicsRootShaderResourceView(19,
            frame.upload.resource->GetGPUVirtualAddress() + previous_instance_data_offset);
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
            bind_scene_deformation_roots(list, true);
            list->SetGraphicsRoot32BitConstant(
                6, kDefaultSceneDrawMaterial | 1u, 1);
        };
        bind_dynamic_draws(0);
        const auto set_environment_material = [&](std::size_t asset, UINT static_mode) {
            list->SetGraphicsRoot32BitConstant(
                6, (impl_->material_parameter_bases[asset / 3] << 1) | static_mode, 1);
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
            if (auto result = impl_->DrawGeometry(list, back_buffer_index, pipeline,
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
        for (std::size_t asset = 0; asset < monster_ranges.size(); ++asset)
        {
            const auto [begin, count] = monster_ranges[asset];
            if (count == 0) continue;
            bind_visible_draws();
            draw_bucket(kMonsterVisibilityBucketBegin + asset, false, hero_pipeline);
        }
        for (std::size_t asset = 0; asset < environment_ranges.size(); ++asset)
        {
            const auto [begin, count] = environment_ranges[asset];
            if (count == 0) continue;
            set_environment_material(asset, 1);
            list->SetGraphicsRootShaderResourceView(34, base);
            list->SetGraphicsRootShaderResourceView(
                35, frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
            draw_bucket(asset, false, environment_pipeline(asset, shadow_draw));
        }
        list->SetPipelineState(generic_pipeline);
        if (!static_draw_refs.empty())
        {
            const auto ref_base = frame.upload.resource->GetGPUVirtualAddress() +
                                  static_ref_data_offset;
            list->SetGraphicsRootShaderResourceView(
                34, impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress());
            list->SetGraphicsRoot32BitConstant(6, kDefaultSceneDrawMaterial | 1u, 1);
            for (std::size_t asset = 0; asset < static_environment_ranges.size(); ++asset)
            {
                const auto [begin, count] = static_environment_ranges[asset];
                if (count == 0) continue;
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
                if (auto result = impl_->DrawGeometry(list, back_buffer_index, ground_pipeline,
                        impl_->procedural_geometry, ground_geometry_passes[visibility_view]); !result)
                    throw std::runtime_error(std::string(result.Message()));
            }
        }
        if (dynamic_terrain_count != 0)
        {
            bind_dynamic_draws(0);
            list->SetGraphicsRoot32BitConstant(6, terrain_draw_material, 1);
            auto *ground_pipeline = shadow_draw ? generic_pipeline : terrain_pipeline;
            if (auto result = impl_->DrawGeometry(list, back_buffer_index, ground_pipeline,
                    impl_->procedural_geometry, dynamic_terrain_geometry_passes[visibility_view]); !result)
                throw std::runtime_error(std::string(result.Message()));
        }
        if (procedural_count != 0)
        {
            bind_dynamic_draws(0);
            if (auto result = impl_->DrawGeometry(list, back_buffer_index, generic_pipeline,
                    impl_->procedural_geometry, procedural_geometry_passes[visibility_view]); !result)
                throw std::runtime_error(std::string(result.Message()));
        }
    };
    const auto bind_scene_roots = [&](ID3D12GraphicsCommandList *list, bool cached_skin_enabled = true) {
        bind_scene_deformation_roots(list, cached_skin_enabled);
        list->SetGraphicsRootShaderResourceView(
            34, static_scene && impl_->static_gpu_scene
                    ? impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress()
                    : dynamic_instance_base);
        list->SetGraphicsRootShaderResourceView(
            35, frame.upload.resource->GetGPUVirtualAddress() + static_ref_data_offset);
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
    if (auto prepared = impl_->AddHlodFramePasses(back_buffer_index, geometry_records_version,
        static_instances_version, {}, hlod_graph); !prepared) return prepared;
    auto visibility_pass = impl_->graph.AddPass(3, kRenderPassNames[3], QueueHint::Compute);
    impl_->ReadHlodFrame(visibility_pass, hlod_graph, false);
    dynamic_lod_version = visibility_pass.ReadWrite(dynamic_lod_version, Access::UnorderedWrite);
    if (static_lod_state)
        static_lod_version = visibility_pass.ReadWrite(static_lod_version, Access::UnorderedWrite);
    geometry_records_version = visibility_pass.ReadWrite(geometry_records_version, Access::UnorderedWrite);
    visibility_pass.Read(dynamic_instances_version, Access::ComputeRead);
    if (have_skinned_instances)
        visibility_pass.Read(skin_bounds_version, Access::ComputeRead);
    if (static_instances_buffer)
        visibility_pass.Read(static_instances_version, Access::ComputeRead);
    visibility_pass.Read(previous_hzb_version, Access::ComputeRead);
    static_visible_refs_version = visibility_pass.Write(
        static_visible_refs_version, Access::UnorderedWrite);
    static_draw_args_version = visibility_pass.Write(
        static_draw_args_version, Access::UnorderedWrite);
    dynamic_visible_refs_version = visibility_pass.Write(
        dynamic_visible_refs_version, Access::UnorderedWrite);
    dynamic_draw_args_version = visibility_pass.Write(
        dynamic_draw_args_version, Access::UnorderedWrite);
    static_rejected_refs_version = visibility_pass.Write(
        static_rejected_refs_version, Access::UnorderedWrite);
    static_rejected_args_version = visibility_pass.Write(
        static_rejected_args_version, Access::UnorderedWrite);
    visibility_pass.SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->static_visibility_pipeline.Get());
        list->SetComputeRootUnorderedAccessView(22,
            (static_lod_state ? static_lod_state->states : dynamic_lod_state.states).resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(21, frame.geometry_draw_records.resource->GetGPUVirtualAddress());
        const std::array<UINT, 2> visibility_constants{
            static_cast<UINT>(visible_ref_stride),
            std::bit_cast<UINT>(impl_->hzb_history_valid
                                    ? grass_visibility_radius
                                    : -grass_visibility_radius)};
        list->SetComputeRoot32BitConstants(
            6, static_cast<UINT>(visibility_constants.size()), visibility_constants.data(), 0);
        list->SetComputeRootShaderResourceView(
            16, frame.upload.resource->GetGPUVirtualAddress() + visibility_range_offset);
        list->SetComputeRootShaderResourceView(
            17, impl_->hzb_history[impl_->hzb_read_index].resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(
            18, have_skinned_instances
                    ? frame.skin_cache_bounds.resource->GetGPUVirtualAddress()
                    : frame.upload.resource->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(
            34, static_scene && impl_->static_gpu_scene
                ? impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress()
                : dynamic_instance_base);
        list->SetComputeRootShaderResourceView(
            35, frame.upload.resource->GetGPUVirtualAddress() + static_ref_data_offset);
        list->SetComputeRootUnorderedAccessView(
            3, frame.static_visible_refs.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            4, frame.static_draw_args.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            7, frame.static_rejected_refs.resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(
            8, frame.static_rejected_args.resource->GetGPUVirtualAddress());
        list->Dispatch(kGpuVisibilityBucketCount, kGpuVisibilityViewCount, 1);
        if (dynamic_visibility_ref_count != 0)
        {
            list->SetComputeRootUnorderedAccessView(22, dynamic_lod_state.states.resource->GetGPUVirtualAddress());
            const std::array<UINT, 2> dynamic_constants{
                static_cast<UINT>(dynamic_visible_ref_stride),
                std::bit_cast<UINT>(-grass_visibility_radius)};
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
                kGpuVisibilityBucketCount, kGpuVisibilityViewCount, 1);
        }
    });

    ClusterFrameGraph cluster_graph;
    if (auto prepared = impl_->AddClusterFramePasses(back_buffer_index, geometry_records_version,
            use_visibility ? &raster_records_version : nullptr, static_instances_version, cluster_graph); !prepared)
        return prepared;
    if (auto prepared = impl_->AddClusterOcclusionPass(back_buffer_index, cluster_graph,
            previous_hzb_version, false); !prepared) return prepared;
    auto shadow_pass = impl_->graph.AddPass(2, kRenderPassNames[2], QueueHint::Direct);
    read_material_sampling(shadow_pass, has_masked_material_draw);
    impl_->ReadClusterFrame(shadow_pass, cluster_graph, true);
    impl_->ReadHlodFrame(shadow_pass, hlod_graph, true);
    shadow_pass.Read(geometry_records_version, Access::ShaderRead);
    shadow_pass.Read(material_parameter_version, Access::ShaderRead);
    shadow_pass.Read(hero_binding_version, Access::ShaderRead);
    read_geometry_pages(shadow_pass);
    shadow_pass.Read(dynamic_instances_version, Access::ShaderRead);
    if (have_skinned_instances)
        shadow_pass.Read(skin_cache_version, Access::ShaderRead);
    if (have_skinned_instances && previous_skin_frame_valid)
        shadow_pass.Read(previous_skin_cache_version, Access::ShaderRead);
    if (static_instances_buffer) shadow_pass.Read(static_instances_version, Access::ShaderRead);
    shadow_pass.Read(static_visible_refs_version, Access::ShaderRead);
    shadow_pass.Read(static_draw_args_version, Access::IndirectArgs);
    shadow_pass.Read(dynamic_visible_refs_version, Access::ShaderRead);
    shadow_pass.Read(dynamic_draw_args_version, Access::IndirectArgs);
    shadow_version = shadow_pass.Write(shadow_version, Access::DepthWrite);
    shadow_pass.SetExecuteChecked([&](RenderPassContext &context) -> Result {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        bind_scene_roots(list);
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
            draw_character_instances(list, true, cascade);
            if (auto drawn = impl_->DrawClusterGeometry(list, back_buffer_index, cascade + 1); !drawn)
                return drawn;
            if (auto drawn = impl_->DrawHlodGeometry(list, back_buffer_index, cascade + 1); !drawn) return drawn;
            handle.ptr += impl_->dsv_stride;
        }
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        return Result::Success();
    });

    if (use_visibility)
    {
        // D3D12 render-target allocations must be initialized by an API discard,
        // clear or copy before shader writes. Keep this Direct operation in the
        // graph so Compute clear waits for initialization after create/resize.
        if (!impl_->visibility_targets_initialized)
        {
            auto initialize = impl_->graph.AddPass(31, "VisibilityDiscard", QueueHint::Direct);
            visibility_ids_version = initialize.Write(visibility_ids_version, Access::RenderTarget);
            visibility_face_version = initialize.Write(visibility_face_version, Access::RenderTarget);
            gbuffer_base_version = initialize.Write(gbuffer_base_version, Access::RenderTarget);
            gbuffer_normal_version = initialize.Write(gbuffer_normal_version, Access::RenderTarget);
            gbuffer_metadata_version = initialize.Write(gbuffer_metadata_version, Access::RenderTarget);
            gbuffer_material_version = initialize.Write(gbuffer_material_version, Access::RenderTarget);
            gbuffer_motion_version = initialize.Write(gbuffer_motion_version, Access::RenderTarget);
            gbuffer_surface_history_version = initialize.Write(gbuffer_surface_history_version, Access::RenderTarget);
            for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
                material_sidecar_versions[plane] = initialize.Write(material_sidecar_versions[plane], Access::RenderTarget);
            initialize.SetExecute([&](RenderPassContext &context) {
                auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                for (auto *resource : {impl_->visibility_ids.resource.Get(),
                        impl_->visibility_front_face.resource.Get(), impl_->gbuffer_base.resource.Get(),
                        impl_->gbuffer_normal.resource.Get(), impl_->gbuffer_metadata.resource.Get(),
                        impl_->gbuffer_material.resource.Get(), impl_->gbuffer_motion.resource.Get(),
                        impl_->gbuffer_surface_history.resource.Get()})
                    list->DiscardResource(resource, nullptr);
                for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
                    list->DiscardResource(impl_->material_sidecars[plane].resource.Get(), nullptr);
            });
        }
        auto clear = impl_->graph.AddPass(25, "VisibilityClear", QueueHint::Compute);
        visibility_ids_version = clear.Write(visibility_ids_version, Access::UnorderedWrite);
        visibility_face_version = clear.Write(visibility_face_version, Access::UnorderedWrite);
        visibility_counts_version = clear.Write(visibility_counts_version, Access::UnorderedWrite);
        gbuffer_base_version = clear.Write(gbuffer_base_version, Access::UnorderedWrite);
        gbuffer_normal_version = clear.Write(gbuffer_normal_version, Access::UnorderedWrite);
        gbuffer_metadata_version = clear.Write(gbuffer_metadata_version, Access::UnorderedWrite);
        gbuffer_material_version = clear.Write(gbuffer_material_version, Access::UnorderedWrite);
        gbuffer_motion_version = clear.Write(gbuffer_motion_version, Access::UnorderedWrite);
        gbuffer_surface_history_version = clear.Write(gbuffer_surface_history_version, Access::UnorderedWrite);
        for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
            material_sidecar_versions[plane] = clear.Write(material_sidecar_versions[plane], Access::UnorderedWrite);
        clear.SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->visibility_clear_pipeline.Get());
            list->Dispatch((impl_->render_width + 7) / 8, (impl_->render_height + 7) / 8, 1);
        });
    }
    auto gbuffer_pass = impl_->graph.AddPass(4, kRenderPassNames[4], QueueHint::Direct);
    read_material_sampling(gbuffer_pass, use_visibility ? has_masked_material_draw : has_authored_material_draw);
    impl_->ReadClusterFrame(gbuffer_pass, cluster_graph, true);
    impl_->ReadHlodFrame(gbuffer_pass, hlod_graph, true);
    gbuffer_pass.Read(geometry_records_version, Access::ShaderRead);
    gbuffer_pass.Read(material_parameter_version, Access::ShaderRead);
    gbuffer_pass.Read(hero_binding_version, Access::ShaderRead);
    read_geometry_pages(gbuffer_pass);
    gbuffer_pass.Read(dynamic_instances_version, Access::ShaderRead);
    if (have_skinned_instances)
        gbuffer_pass.Read(skin_cache_version, Access::ShaderRead);
    if (have_skinned_instances && previous_skin_frame_valid)
        gbuffer_pass.Read(previous_skin_cache_version, Access::ShaderRead);
    if (static_instances_buffer) gbuffer_pass.Read(static_instances_version, Access::ShaderRead);
    gbuffer_pass.Read(static_visible_refs_version, Access::ShaderRead);
    gbuffer_pass.Read(static_draw_args_version, Access::IndirectArgs);
    gbuffer_pass.Read(dynamic_visible_refs_version, Access::ShaderRead);
    gbuffer_pass.Read(dynamic_draw_args_version, Access::IndirectArgs);
    if (use_visibility)
    {
        visibility_ids_version = gbuffer_pass.ReadWrite(visibility_ids_version, Access::RenderTarget);
        visibility_face_version = gbuffer_pass.ReadWrite(visibility_face_version, Access::RenderTarget);
    }
    else
    {
    gbuffer_base_version = gbuffer_pass.Write(gbuffer_base_version, Access::RenderTarget);
    gbuffer_normal_version = gbuffer_pass.Write(gbuffer_normal_version, Access::RenderTarget);
    gbuffer_metadata_version = gbuffer_pass.Write(gbuffer_metadata_version, Access::RenderTarget);
    gbuffer_material_version = gbuffer_pass.Write(gbuffer_material_version, Access::RenderTarget);
    gbuffer_motion_version = gbuffer_pass.Write(gbuffer_motion_version, Access::RenderTarget);
    gbuffer_surface_history_version = gbuffer_pass.Write(gbuffer_surface_history_version, Access::RenderTarget);
    for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
        material_sidecar_versions[plane] = gbuffer_pass.Write(material_sidecar_versions[plane], Access::RenderTarget);
    }
    depth_version = gbuffer_pass.Write(depth_version, Access::DepthWrite);
    gbuffer_pass.SetExecuteChecked([&](RenderPassContext &context) -> Result {
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
            if (auto drawn = impl_->DrawClusterGeometry(list, back_buffer_index, 0); !drawn) return drawn;
            return impl_->DrawHlodGeometry(list, back_buffer_index, 0);
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
        if (auto drawn = impl_->DrawClusterGeometry(list, back_buffer_index, 0); !drawn) return drawn;
            return impl_->DrawHlodGeometry(list, back_buffer_index, 0);
    });

    const auto record_depth_pyramid = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetComputeRootUnorderedAccessView(
            3, impl_->hzb_history[hzb_write_index].resource->GetGPUVirtualAddress());
        const auto &base = impl_->hzb_layout.levels[0];
        std::array<UINT, 2> constants{0u, 0u};
        list->SetPipelineState(impl_->depth_pyramid_init_pipeline.Get());
        list->SetComputeRoot32BitConstants(
            6, static_cast<UINT>(constants.size()), constants.data(), 0);
        list->Dispatch((base.width + 7u) / 8u,
                                     (base.height + 7u) / 8u, 1);
        context.UavBarrier(current_hzb);
        list->SetPipelineState(impl_->depth_pyramid_reduce_pipeline.Get());
        for (std::uint32_t level = 1; level < impl_->hzb_layout.count; ++level)
        {
            const auto &destination = impl_->hzb_layout.levels[level];
            constants = {level, 0u};
            list->SetComputeRoot32BitConstants(
                6, static_cast<UINT>(constants.size()), constants.data(), 0);
            list->Dispatch((destination.width + 7u) / 8u,
                                         (destination.height + 7u) / 8u, 1);
            context.UavBarrier(current_hzb);
        }
    };

    auto depth_pyramid_pass =
        impl_->graph.AddPass(18, kRenderPassNames[18], QueueHint::Compute);
    depth_pyramid_pass.Read(depth_version, Access::ComputeRead);
    current_hzb_version =
        depth_pyramid_pass.Write(current_hzb_version, Access::UnorderedWrite);
    depth_pyramid_pass.SetExecute(
        [&](RenderPassContext &context) { record_depth_pyramid(context); });

    if (auto prepared = impl_->AddClusterOcclusionPass(back_buffer_index, cluster_graph,
            current_hzb_version, true); !prepared) return prepared;
    auto retest_pass =
        impl_->graph.AddPass(19, kRenderPassNames[19], QueueHint::Compute);
    if (static_instances_buffer)
        retest_pass.Read(static_instances_version, Access::ComputeRead);
    retest_pass.Read(static_rejected_refs_version, Access::ComputeRead);
    retest_pass.Read(static_rejected_args_version, Access::UnorderedRead);
    geometry_records_version = retest_pass.ReadWrite(geometry_records_version, Access::UnorderedWrite);
    retest_pass.Read(current_hzb_version, Access::ComputeRead);
    static_retest_refs_version =
        retest_pass.Write(static_retest_refs_version, Access::UnorderedWrite);
    static_retest_args_version =
        retest_pass.Write(static_retest_args_version, Access::UnorderedWrite);
    retest_pass.SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->visibility_retest_pipeline.Get());
        list->SetComputeRootUnorderedAccessView(22,
            (static_lod_state ? static_lod_state->states : dynamic_lod_state.states).resource->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(21, frame.geometry_draw_records.resource->GetGPUVirtualAddress());
        const std::array<UINT, 2> visibility_constants{
            static_cast<UINT>(visible_ref_stride),
            std::bit_cast<UINT>(grass_visibility_radius)};
        list->SetComputeRoot32BitConstants(
            6, static_cast<UINT>(visibility_constants.size()),
            visibility_constants.data(), 0);
        list->SetComputeRootShaderResourceView(
            16, frame.upload.resource->GetGPUVirtualAddress() + retest_visibility_range_offset);
        list->SetComputeRootShaderResourceView(
            17, impl_->hzb_history[hzb_write_index].resource->GetGPUVirtualAddress());
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

    auto gbuffer_retest_pass =
        impl_->graph.AddPass(20, kRenderPassNames[20], QueueHint::Direct);
    read_material_sampling(gbuffer_retest_pass, use_visibility ? has_masked_material_draw : has_authored_material_draw);
    impl_->ReadClusterFrame(gbuffer_retest_pass, cluster_graph, true, true);
    impl_->ReadHlodFrame(gbuffer_retest_pass, hlod_graph, false);
    gbuffer_retest_pass.Read(geometry_records_version, Access::ShaderRead);
    gbuffer_retest_pass.Read(material_parameter_version, Access::ShaderRead);
    read_geometry_pages(gbuffer_retest_pass);
    if (static_instances_buffer)
        gbuffer_retest_pass.Read(static_instances_version, Access::ShaderRead);
    gbuffer_retest_pass.Read(static_retest_refs_version, Access::ShaderRead);
    gbuffer_retest_pass.Read(static_retest_args_version, Access::IndirectArgs);
    if (use_visibility)
    {
        visibility_ids_version = gbuffer_retest_pass.ReadWrite(visibility_ids_version, Access::RenderTarget);
        visibility_face_version = gbuffer_retest_pass.ReadWrite(visibility_face_version, Access::RenderTarget);
    }
    else
    {
    gbuffer_base_version =
        gbuffer_retest_pass.ReadWrite(gbuffer_base_version, Access::RenderTarget);
    gbuffer_normal_version =
        gbuffer_retest_pass.ReadWrite(gbuffer_normal_version, Access::RenderTarget);
    gbuffer_metadata_version =
        gbuffer_retest_pass.ReadWrite(gbuffer_metadata_version, Access::RenderTarget);
    gbuffer_material_version =
        gbuffer_retest_pass.ReadWrite(gbuffer_material_version, Access::RenderTarget);
    gbuffer_motion_version =
        gbuffer_retest_pass.ReadWrite(gbuffer_motion_version, Access::RenderTarget);
    gbuffer_surface_history_version =
        gbuffer_retest_pass.ReadWrite(gbuffer_surface_history_version, Access::RenderTarget);
    if (!use_visibility)
        for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
            material_sidecar_versions[plane] = gbuffer_retest_pass.ReadWrite(material_sidecar_versions[plane], Access::RenderTarget);
    }
    depth_version =
        gbuffer_retest_pass.ReadWrite(depth_version, Access::DepthWrite);
    gbuffer_retest_pass.SetExecuteChecked([&](RenderPassContext &context) -> Result {
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
        if (auto drawn = impl_->DrawClusterGeometry(list, back_buffer_index, 0, true); !drawn) return drawn;
        if (!static_scene || !impl_->static_gpu_scene) return Result::Success();
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->SetGraphicsRootShaderResourceView(
            34, impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress());
        for (std::size_t asset = 0; asset < static_environment_ranges.size(); ++asset)
        {
            if (static_environment_ranges[asset].second == 0) continue;
            list->SetGraphicsRoot32BitConstant(
                6, (impl_->material_parameter_bases[asset / 3] << 1) | 1u, 1);
            list->SetGraphicsRootShaderResourceView(
                35, frame.static_retest_refs.resource->GetGPUVirtualAddress());
            if (auto result = impl_->DrawGeometry(list, back_buffer_index,
                    environment_pipeline(asset, false), *bucket_pages[asset],
                    retest_geometry_passes[asset], frame.static_retest_args.resource.Get(),
                    asset * kGeometryArgumentStride); !result)
                return result;
        }
        return Result::Success();
    });

    auto final_depth_pyramid_pass =
        impl_->graph.AddPass(21, kRenderPassNames[21], QueueHint::Compute);
    final_depth_pyramid_pass.Read(depth_version, Access::ComputeRead);
    current_hzb_version = final_depth_pyramid_pass.ReadWrite(
        current_hzb_version, Access::UnorderedWrite);
    final_depth_pyramid_pass.SetExecute(
        [&](RenderPassContext &context) { record_depth_pyramid(context); });

    if (use_visibility)
    {
        auto records = impl_->graph.AddPass(26, "RasterDrawRecords", QueueHint::Compute);
        records.Read(geometry_records_version, Access::ComputeRead);
        records.Read(static_visible_refs_version, Access::ComputeRead);
        records.Read(dynamic_visible_refs_version, Access::ComputeRead);
        records.Read(static_retest_refs_version, Access::ComputeRead);
        raster_records_version = records.ReadWrite(raster_records_version, Access::UnorderedWrite);
        records.SetExecute([&](RenderPassContext &context) {
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
        auto count = impl_->graph.AddPass(27, "MaterialCount", QueueHint::Compute);
        count.Read(geometry_records_version, Access::ComputeRead);
        count.Read(hero_binding_version, Access::ComputeRead);
        for (const auto &page : geometry_pages)
        {
            count.Read(page.versions[0], Access::ComputeRead);
            count.Read(page.versions[1], Access::ComputeRead);
        }
        count.Read(visibility_ids_version, Access::ComputeRead);
        count.Read(raster_records_version, Access::ComputeRead);
        visibility_counts_version = count.ReadWrite(visibility_counts_version, Access::UnorderedWrite);
        count.SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->visibility_count_pipeline.Get());
            list->Dispatch((impl_->render_width + 7) / 8, (impl_->render_height + 7) / 8, 1);
        });
        auto prefix = impl_->graph.AddPass(28, "MaterialPrefix", QueueHint::Compute);
        visibility_counts_version = prefix.ReadWrite(visibility_counts_version, Access::UnorderedWrite);
        visibility_args_version = prefix.Write(visibility_args_version, Access::UnorderedWrite);
        prefix.SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->visibility_prefix_pipeline.Get());
            list->Dispatch(1, 1, 1);
        });
        auto scatter = impl_->graph.AddPass(29, "MaterialScatter", QueueHint::Compute);
        scatter.Read(geometry_records_version, Access::ComputeRead);
        scatter.Read(hero_binding_version, Access::ComputeRead);
        for (const auto &page : geometry_pages)
        {
            scatter.Read(page.versions[0], Access::ComputeRead);
            scatter.Read(page.versions[1], Access::ComputeRead);
        }
        scatter.Read(visibility_ids_version, Access::ComputeRead);
        scatter.Read(raster_records_version, Access::ComputeRead);
        visibility_counts_version = scatter.ReadWrite(visibility_counts_version, Access::UnorderedWrite);
        visibility_pixels_version = scatter.Write(visibility_pixels_version, Access::UnorderedWrite);
        scatter.SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_compute(list);
            list->SetPipelineState(impl_->visibility_scatter_pipeline.Get());
            list->Dispatch((impl_->render_width + 7) / 8, (impl_->render_height + 7) / 8, 1);
        });
        auto resolve = impl_->graph.AddPass(30, "SurfaceResolve", QueueHint::Compute);
        read_material_sampling(resolve, has_authored_material_draw, Access::ComputeRead);
        impl_->ReadClusterFrame(resolve, cluster_graph, false);
        impl_->ReadHlodFrame(resolve, hlod_graph, false);
        resolve.Read(visibility_ids_version, Access::ComputeRead);
        resolve.Read(visibility_face_version, Access::ComputeRead);
        resolve.Read(raster_records_version, Access::ComputeRead);
        resolve.Read(geometry_records_version, Access::ComputeRead);
        resolve.Read(material_parameter_version, Access::ComputeRead);
        resolve.Read(hero_binding_version, Access::ComputeRead);
        resolve.Read(visibility_pixels_version, Access::ComputeRead);
        resolve.Read(visibility_counts_version, Access::ComputeRead);
        resolve.Read(visibility_args_version, Access::IndirectArgs);
        resolve.Read(static_visible_refs_version, Access::ComputeRead);
        resolve.Read(dynamic_visible_refs_version, Access::ComputeRead);
        resolve.Read(static_retest_refs_version, Access::ComputeRead);
        resolve.Read(dynamic_instances_version, Access::ComputeRead);
        if (static_instances_buffer) resolve.Read(static_instances_version, Access::ComputeRead);
        if (have_skinned_instances) resolve.Read(skin_cache_version, Access::ComputeRead);
        if (have_skinned_instances && previous_skin_frame_valid)
            resolve.Read(previous_skin_cache_version, Access::ComputeRead);
        for (const auto &page : geometry_pages)
        {
            resolve.Read(page.versions[0], Access::ComputeRead);
            resolve.Read(page.versions[1], Access::ComputeRead);
        }
        gbuffer_base_version = resolve.ReadWrite(gbuffer_base_version, Access::UnorderedWrite);
        gbuffer_normal_version = resolve.ReadWrite(gbuffer_normal_version, Access::UnorderedWrite);
        gbuffer_metadata_version = resolve.ReadWrite(gbuffer_metadata_version, Access::UnorderedWrite);
        gbuffer_material_version = resolve.ReadWrite(gbuffer_material_version, Access::UnorderedWrite);
        gbuffer_motion_version = resolve.ReadWrite(gbuffer_motion_version, Access::UnorderedWrite);
        gbuffer_surface_history_version = resolve.ReadWrite(gbuffer_surface_history_version, Access::UnorderedWrite);
        for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
            material_sidecar_versions[plane] = resolve.ReadWrite(material_sidecar_versions[plane], Access::UnorderedWrite);
        resolve.SetExecute([&](RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_surface_compute(list);
            for (std::uint32_t variant = 0; variant < kVisibilityVariantCount; ++variant)
            {
                list->SetComputeRoot32BitConstant(6, variant, 0);
                list->SetPipelineState(variant == 0 ? impl_->surface_resolve_pipeline.Get()
                    : variant <= kAuthoredMaterialNames.size()
                        ? impl_->environment_material_resolve_pipelines[variant - 1].Get()
                        : impl_->hero_resolve_pipeline.Get());
                list->ExecuteIndirect(impl_->dispatch_signature.Get(), 1,
                    impl_->visibility_dispatch_args.resource.Get(), variant * 12u, nullptr, 0);
            }
        });
    }
    // Every successful feature registration needs a completion, including early
    // returns during graph construction/preparation and partial submissions.
    struct PendingFrameEffects
    {
        Impl &renderer;
        FrameContext &frame;
        CompletionSet completion;
        bool ambient{}, reconstruction{}, diffusion{}, reflections{}, transparency{}, probe{}, ltc{}, hdr_post{}, lights{}, ray_scene{}, ray_omm{}, ray_geometry{}, ray_ambient{}, ray_transport{}, shadows{};
        bool local_probes{}, volumetrics{}, presentation{}, local_shadows{}, ray_shadows{}, ray_reconstruction{}, brixelizer{}, screen_overlay{}, adaptive{}, decal_surface{};
        std::shared_ptr<const StaticRenderScene> probe_scene;
        render_data::SceneProbeRevisions probe_revisions;
        ~PendingFrameEffects()
        {
            if (!ambient && !reconstruction && !diffusion && !reflections && !transparency && !probe && !ltc && !hdr_post && !lights && !ray_scene && !ray_omm && !ray_geometry && !ray_ambient && !ray_transport && !shadows && !local_probes && !volumetrics && !presentation && !local_shadows && !ray_shadows && !ray_reconstruction && !brixelizer && !screen_overlay && !adaptive && !decal_surface) return;
            frame.completion.Merge(completion);
            if (decal_surface)
                (void)renderer.decal_surface.CompleteSubmission(renderer.graph, completion, false);
            if (adaptive)
                (void)renderer.adaptive_shading.CompleteSubmission(renderer.graph, completion, false);
            if (ambient)
                (void)renderer.ambient_occlusion.CompleteSubmission(renderer.graph, completion, false);
            if (reconstruction)
                (void)renderer.reconstruction_guides.CompleteSubmission(renderer.graph, completion, false);
            if (presentation)
                (void)renderer.presentation_guides.CompleteSubmission(renderer.graph, completion, false);
            if (diffusion)
                (void)renderer.subsurface.CompleteSubmission(renderer.graph, completion, false);
            if (reflections)
                (void)renderer.screen_reflections.CompleteSubmission(renderer.graph, completion, false);
            if (transparency)
                (void)renderer.hero_transparency.CompleteSubmission(renderer.graph, completion, false);
            if (probe)
                (void)renderer.lighting_probe.CompleteSubmission(renderer.graph, completion, false);
            if (ltc)
                (void)renderer.ltc_lut.CompleteSubmission(renderer.graph, completion, false);
            if (hdr_post)
                (void)renderer.hdr_post.CompleteSubmission(renderer.graph, completion, false);
            if (screen_overlay)
                (void)renderer.screen_overlay.CompleteSubmission(renderer.graph, completion, false);
            if (lights)
                (void)renderer.light_list.CompleteSubmission(renderer.graph, completion, false);
            if (ray_scene)
                (void)renderer.ray_scene.CompleteSubmission(renderer.graph, completion, false);
            if (ray_omm)
                (void)renderer.ray_omm.CompleteSubmission(renderer.graph, completion, false);
            if (ray_geometry)
                (void)renderer.ray_geometry.CompleteSubmission(renderer.graph, completion, false);
            if (ray_ambient)
                (void)renderer.ray_ambient_occlusion.CompleteSubmission(renderer.graph, completion, false);
            if (ray_transport)
                (void)renderer.ray_transport.CompleteSubmission(renderer.graph, completion, false);
            if (ray_reconstruction)
                (void)renderer.ray_reconstruction.CompleteSubmission(renderer.graph, completion, false);
            if (brixelizer)
            {
                (void)renderer.brixelizer_gi.CompleteSubmission(renderer.graph, completion, false);
                renderer.brixelizer_history_valid = false;
            }
            if (shadows)
                (void)renderer.virtual_shadows.CompleteSubmission(renderer.graph, completion, false);
            if (local_shadows)
                (void)renderer.local_shadows.CompleteSubmission(renderer.graph, completion, false);
            if (ray_shadows)
                (void)renderer.ray_shadows.CompleteSubmission(renderer.graph, completion, false);
            if (local_probes)
                (void)renderer.local_probes.CompleteSubmission(renderer.graph, completion, false, probe_scene, probe_revisions);
            if (volumetrics)
                (void)renderer.volumetrics.CompleteSubmission(renderer.graph, completion, false);
            renderer.temporal_history_valid = false;
            renderer.hzb_history_valid = false;
            // Aborted service targets cannot be reused; SDK recording may also
            // have mutated its private context before GPU submission.
            renderer.frame_submission_faulted = true;
        }
    } pending_effects{*impl_, frame};
    auto *surface_base_resource = impl_->gbuffer_base.resource.Get();
    auto *surface_normal_resource = impl_->gbuffer_normal.resource.Get();
    auto *surface_material_resource = impl_->gbuffer_material.resource.Get();
    DecalSurfaceOutputs decal_surface_outputs;
    if (constants->vfx_decal_count.x)
    {
        if (!impl_->decal_surface.AllocatedBytes())
        {
            DecalSurfaceCandidate candidate;
            if (auto prepared = impl_->PrepareDecalSurface(impl_->config,
                    impl_->view_state.render_capacity, true, candidate); !prepared) return prepared;
            auto transition = impl_->DiagnosticsMemory();
            transition.renderer_view_bytes += candidate.AllocatedBytes();
            impl_->diagnostics.ObserveTransition(transition);
            if (auto committed = impl_->decal_surface.Commit(std::move(candidate),
                    impl_->graph_queue_executor.Completed()); !committed) return committed;
        }
        DecalSurfaceInputs inputs;
        inputs.depth = {depth_version, impl_->depth.resource.Get()};
        inputs.metadata = {gbuffer_metadata_version, impl_->gbuffer_metadata.resource.Get()};
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
        inputs.render_active = impl_->view_state.render_active;
        inputs.frame_slot = back_buffer_index;
        inputs.completed = impl_->graph_queue_executor.Completed();
        inputs.descriptor_suffix_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        inputs.descriptor_suffix_cpu.ptr += (SIZE_T(back_buffer_index) * kTextureDescriptorCount +
            kDecalSurfaceDescriptorOffset) * impl_->srv_stride;
        inputs.main_table_gpu = texture_table;
        inputs.main_table_gpu.ptr += UINT64(kTemporalTextureDescriptorOffset) * impl_->srv_stride;
        inputs.main_heap = impl_->srv_heap.Get();
        DecalSurfaceConstants camera;
        camera.inverse_view_projection = constants->inverse_view_projection;
        camera.view_origin = constants->surface_view_origin;
        pending_effects.decal_surface = true;
        if (auto registered = impl_->decal_surface.Register(impl_->graph, inputs, camera,
                decal_surface_outputs); !registered) return registered;
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
        cpu.ptr += (SIZE_T(back_buffer_index) * kTextureDescriptorCount + offset) * impl_->srv_stride;
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
    VirtualShadowOutputs virtual_shadow_outputs;
    LocalShadowOutputs local_shadow_outputs;
    RayShadowOutputs ray_shadow_outputs;
    // Off has no private pool. A typed null metadata SRV makes the shared
    // opaque helper see zero selected contexts instead of a retired frame.
    {
        auto handle = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount + kRayShadowExportDescriptorOffset) * impl_->srv_stride;
        for (UINT i = 0; i < kRayShadowExportCount; ++i)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (i < 2)
            {
                view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                view.Format = i == 0 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
                view.Buffer.NumElements = i == 0 ? sizeof(RayShadowMetadata) / 4 : 1;
                view.Buffer.StructureByteStride = i == 1 ? sizeof(GpuRayShadowRecord) : 0;
                view.Buffer.Flags = i == 0 ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
            }
            else if (i < 4)
            {
                view.Format = DXGI_FORMAT_R16_FLOAT; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.MipLevels = 1; view.Texture2DArray.ArraySize = 1;
            }
            else
            {
                view.Format = i == 4 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R16G16_SNORM;
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
            }
            impl_->device->CreateShaderResourceView(nullptr, &view, handle);
            handle.ptr += impl_->srv_stride;
        }
    }
    decltype(impl_->previous_shadow_casters) current_shadow_casters;
    if ((primary_sun && impl_->virtual_shadows.AllocatedBytes() != 0) || impl_->local_shadows.AllocatedBytes() != 0)
    {
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
        current_shadow_casters.reserve(dynamic_draw_refs.size() + static_leaf_ref_count);
        casters.reserve(dynamic_draw_refs.size() + static_leaf_ref_count);
        std::vector<VirtualShadowReceiver> receivers;
        std::vector<VirtualShadowNativeRead> reads;
        for (const auto &hero : typed_volumes)
            receivers.push_back({{hero.bounds_min.x, hero.bounds_min.y, hero.bounds_min.z, 0},
                {hero.bounds_max.x, hero.bounds_max.y, hero.bounds_max.z, 0}});
        // Independent capture/froxel receivers must request world shadow pages
        // even when the main opaque camera did not draw a surface there.
        for (const auto &probe : impl_->local_probes.Probes())
            receivers.push_back({{std::min({probe.influence_min.x, probe.projection_min.x, probe.position.x - probe.capture_distance}),
                    std::min({probe.influence_min.y, probe.projection_min.y, probe.position.y - probe.capture_distance}),
                    std::min({probe.influence_min.z, probe.projection_min.z, probe.position.z - probe.capture_distance}), 0},
                {std::max({probe.influence_max.x, probe.projection_max.x, probe.position.x + probe.capture_distance}),
                    std::max({probe.influence_max.y, probe.projection_max.y, probe.position.y + probe.capture_distance}),
                    std::max({probe.influence_max.z, probe.projection_max.z, probe.position.z + probe.capture_distance}), 0}});
        if (impl_->volumetrics.AllocatedBytes() != 0)
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
        const auto append_caster = [&](const RenderInstance &source, const GpuStaticDrawRef &reference,
                std::uint32_t ref_index, bool immutable, const GpuInstance *instance) -> Result {
            const bool environment = is_environment_mesh(source.mesh);
            const bool terrain = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch;
            const bool hero = source.mesh == RenderMesh::Archer || is_monster(source.mesh);
            const std::uint32_t bucket = environment ? static_cast<UINT>(environment_mesh_index(source) * 3 + 2)
                : source.mesh == RenderMesh::Archer ? kArcherVisibilityBucket
                : is_monster(source.mesh) ? kMonsterVisibilityBucketBegin + static_cast<UINT>(source.mesh) - static_cast<UINT>(RenderMesh::MonsterMelee)
                : UINT32_MAX;
            const auto *page = bucket != UINT32_MAX ? bucket_pages[bucket] : &impl_->procedural_geometry;
            const auto pass = bucket != UINT32_MAX
                ? (immutable ? static_geometry_passes[bucket][1] : dynamic_geometry_passes[bucket][1])
                : immutable ? ground_geometry_passes[1] : terrain ? dynamic_terrain_geometry_passes[1] : procedural_geometry_passes[1];
            if (!page || pass == 0) return Result::Failure(ErrorCode::InvalidState,
                "hs_renderer_d3d12", "Virtual caster has no complete geometry record.");
            auto *pipeline = environment ? environment_pipeline(bucket, true)
                : hero ? impl_->hero_shadow_pipeline.Get() : impl_->shadow_pipeline.Get();
            const UINT ordinal = environment ? bucket / 3 : terrain ? kTerrainMaterialOrdinal : UINT32_MAX;
            const UINT material = ordinal < kAuthoredMaterialNames.size()
                ? (impl_->material_parameter_bases[ordinal] << 1) | 1u : kDefaultSceneDrawMaterial | 1u;
            const auto binding_key = (std::uint64_t(material) << 32) | (immutable ? 0x80000000u : 0u) | page->page_slot;
            auto group = std::ranges::find(groups, binding_key, &VirtualShadowDrawGroup::binding_key);
            if (group == groups.end())
            {
                groups.push_back({binding_key, impl_->GeometryUsesMesh(pipeline, *page, 1)
                    ? VirtualShadowFrontend::Mesh : VirtualShadowFrontend::Indexed});
                bindings.push_back({page, pipeline, material, immutable});
                group = groups.end() - 1;
            }
            VirtualShadowCaster caster;
            const auto &sphere = page->metadata.bounds.sphere;
            const auto caster_yaw = instance ? instance->yaw : source.yaw;
            const float sine = std::sin(caster_yaw), cosine = std::cos(caster_yaw);
            const float sx = sphere[0] * source.scale.x, sy = sphere[1] * source.scale.y, sz = sphere[2] * source.scale.z;
            const auto center = instance ? instance->position : DirectX::XMFLOAT4{
                source.position.x, source.position.y + (environment ? 0.0f : source.scale.y * 0.5f), source.position.z, 0};
            const float maximum_scale = std::max({std::abs(source.scale.x), std::abs(source.scale.y), std::abs(source.scale.z)});
            caster.current_bounds = {center.x + cosine * sx + sine * sz, center.y + sy,
                center.z - sine * sx + cosine * sz, sphere[3] * maximum_scale};
            caster.skin = hero;
            caster.wind = source.mesh == RenderMesh::Grass;
            caster.animated_alpha = source.mesh == RenderMesh::Grass;
            if (caster.skin && instance)
            {
                // SkinCache's conservative pose radius is centered on the actual
                // interpolated draw origin, independently of the rest-pose sphere.
                caster.current_bounds = {center.x, center.y, center.z, std::max(center.w,
                    std::sqrt(sx*sx + sy*sy + sz*sz) + sphere[3] * maximum_scale)};
            }
            if (caster.wind) caster.current_bounds.w += grass_visibility_radius;
            caster.current_revisions = source.revisions;
            caster.current_revisions.material ^= impl_->material_parameter_revision;
            caster.current_cut_revision = (std::uint64_t(page->generation) << 32) | page->page_slot;
            caster.instance_index = reference.instance_index | (immutable ? 0x80000000u : 0u);
            caster.instance_generation = reference.instance_generation;
            caster.source_ref = ref_index;
            caster.geometry_record = pass >> 2;
            caster.draw_group = static_cast<UINT>(group - groups.begin());
            caster.index_count = page->index_count;
            caster.meshlet_count = page->meshlet_count;
            caster.immutable_source = immutable;
            const auto key = std::tuple{immutable, source.stable_id,
                source.stable_id == 0 ? reference.instance_index + 1u : 0u};
            const auto [current, inserted] = current_shadow_casters.try_emplace(key);
            if (!inserted) return Result::Success();
            const auto previous = impl_->previous_shadow_casters.find(key);
            if (!new_session && previous != impl_->previous_shadow_casters.end())
            {
                const auto &old = previous->second;
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
            current->second = caster;
            casters.push_back(caster);
            if (is_monster(source.mesh) && static_cast<UINT>(source.mesh) - static_cast<UINT>(RenderMesh::MonsterMelee) < 3)
            {
                const auto &b = caster.current_bounds;
                receivers.push_back({{b.x-b.w,b.y-b.w,b.z-b.w,0}, {b.x+b.w,b.y+b.w,b.z+b.w,0}});
            }
            return Result::Success();
        };
        for (std::size_t index = 0; index < dynamic_draw_refs.size(); ++index)
        {
            // Translucent gel projectiles have no opaque shadow representation.
            if (render_instances[index].mesh == RenderMesh::EnemyProjectile) continue;
            if (auto added = append_caster(render_instances[index], dynamic_draw_refs[index], static_cast<UINT>(index),
                    false, &persistent_instances[dynamic_draw_refs[index].instance_index]); !added) return added;
        }
        if (static_scene && impl_->static_gpu_scene)
            for (std::size_t index = 0; index < static_leaf_ref_count; ++index)
                if (auto added = append_caster(static_scene->instances[static_draw_refs[index].instance_index],
                        static_draw_refs[index], static_cast<UINT>(index), true, nullptr); !added) return added;
        if (!new_session)
        {
            std::vector<const Impl::ShadowCasterMap::value_type *> removed_casters;
            for (const auto &entry : impl_->previous_shadow_casters)
                if (!current_shadow_casters.contains(entry.first)) removed_casters.push_back(&entry);
            // Keep the former ordered-map order when removed casters invalidate cached pages.
            std::ranges::sort(removed_casters, {}, [](const auto *entry) { return entry->first; });
            for (const auto *entry : removed_casters)
            {
                const auto &old = entry->second;
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
        reads.push_back({geometry_records_version, Access::ShaderRead});
        reads.push_back({dynamic_instances_version, Access::ShaderRead});
        reads.push_back({material_parameter_version, Access::ShaderRead});
        reads.push_back({hero_binding_version, Access::ShaderRead});
        reads.push_back({impl_->graph.Version(import_buffer(frame.upload, Access::ShaderRead, "ShadowFrameInputs")), Access::ShaderRead});
        if (static_instances_buffer) reads.push_back({static_instances_version, Access::ShaderRead});
        if (have_skinned_instances) reads.push_back({skin_cache_version, Access::ShaderRead});
        if (have_skinned_instances && previous_skin_frame_valid) reads.push_back({previous_skin_cache_version, Access::ShaderRead});
        for (const auto &entry : geometry_pages)
        {
            reads.push_back({entry.versions[0], impl_->mesh_shader_enabled ? Access::VertexShaderRead : Access::VertexBuffer});
            reads.push_back({entry.versions[1], Access::IndexBuffer});
            if (impl_->mesh_shader_enabled)
                for (UINT i = 2; i < 4; ++i) reads.push_back({entry.versions[i], Access::ShaderRead});
        }
        VirtualShadowInputs inputs;
        inputs.declare_material_sampling = [&](PassBuilder &pass) { read_material_sampling(pass, has_masked_material_draw); };
        if (primary_sun) inputs.light = *primary_sun;
        DirectX::XMStoreFloat3(&inputs.clipmap_center, target);
        inputs.view_origin = lod_eye;
        inputs.inverse_view_projection = camera_relative_inverse_view_projection;
        for (UINT i = 0; i < 3; ++i) inputs.coverage_extents[i] = cascade_extents[i] * 0.5f;
        inputs.depth_span = shadow_distance * 2;
        inputs.render_active = impl_->view_state.render_active;
        inputs.frame_slot = back_buffer_index;
        inputs.rendered_frame_id = impl_->frame_number;
        inputs.completed = impl_->graph_queue_executor.Completed();
        inputs.depth = depth_version; inputs.normal = gbuffer_normal_version;
        inputs.depth_resource = impl_->depth.resource.Get(); inputs.normal_resource = surface_normal_resource;
        inputs.casters = casters; inputs.receivers = receivers; inputs.draw_groups = groups; inputs.native_reads = reads;
        const auto draw_shadows = [&, bindings](RenderPassContext &context, const auto &outputs,
                std::span<const VirtualShadowDrawBatch> batches) -> Result {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            bind_graphics(list);
            const D3D12_VIEWPORT bank_view{0,0,float(outputs.bank_extent.width),float(outputs.bank_extent.height),0,1};
            const D3D12_RECT bank_rect{0,0,LONG(outputs.bank_extent.width),LONG(outputs.bank_extent.height)};
            list->RSSetViewports(1, &bank_view); list->RSSetScissorRects(1, &bank_rect);
            bind_scene_deformation_roots(list, true);
            for (const auto &batch : batches)
            {
                const auto &binding = bindings[batch.group_index];
                list->SetGraphicsRootShaderResourceView(34, binding.immutable
                    ? impl_->static_gpu_scene->instances.resource->GetGPUVirtualAddress() : dynamic_instance_base);
                list->SetGraphicsRootShaderResourceView(35, frame.upload.resource->GetGPUVirtualAddress() +
                    (binding.immutable ? static_ref_data_offset : dynamic_ref_data_offset));
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
                    batch.argument_offset, outputs.counts_resource, batch.count_offset);
            }
            return Result::Success();
        };
        inputs.draw = draw_shadows;
        if (primary_sun && impl_->virtual_shadows.AllocatedBytes() != 0)
        {
            pending_effects.shadows = true;
            if (auto registered = impl_->virtual_shadows.Register(impl_->graph, inputs, virtual_shadow_outputs); !registered)
                return registered;
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount + kVirtualShadowDescriptorOffset) * impl_->srv_stride;
            for (const auto srv : virtual_shadow_outputs.srvs)
            {
                impl_->device->CopyDescriptorsSimple(1, destination, srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                destination.ptr += impl_->srv_stride;
            }
        }
        if (impl_->local_shadows.AllocatedBytes() != 0)
        {
            LocalShadowInputs local;
            local.declare_material_sampling = inputs.declare_material_sampling;
            local.lights = frame_lights; local.camera_position = lod_eye;
            local.rendered_frame_id = impl_->frame_number; local.frame_slot = back_buffer_index;
            local.completed = inputs.completed; local.reset_cache = new_session;
            local.casters = casters; local.draw_groups = groups; local.native_reads = reads;
            local.draw = draw_shadows;
            pending_effects.local_shadows = true;
            if (auto registered = impl_->local_shadows.Register(impl_->graph, local, local_shadow_outputs); !registered)
                return registered;
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount + kLocalShadowDescriptorOffset) * impl_->srv_stride;
            for (const auto srv : local_shadow_outputs.srvs)
            {
                impl_->device->CopyDescriptorsSimple(1, destination, srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                destination.ptr += impl_->srv_stride;
            }
        }
    }
    LightingProbeOutputs probe_outputs;
    if (auto registered = impl_->lighting_probe.Register(impl_->graph,
            impl_->graph_queue_executor.Completed(), probe_outputs); !registered) return registered;
    pending_effects.probe = true;
    auto probe_descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    probe_descriptor.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
        kProbeDescriptorOffset) * impl_->srv_stride;
    for (const auto source : probe_outputs.srvs)
    {
        impl_->device->CopyDescriptorsSimple(1, probe_descriptor, source,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        probe_descriptor.ptr += impl_->srv_stride;
    }
    LightListInputs light_inputs;
    light_inputs.lights = frame_lights;
    light_inputs.transient_light_ids = transient_light_ids;
    light_inputs.render_active = impl_->view_state.render_active;
    light_inputs.frame_slot = back_buffer_index;
    light_inputs.completed = impl_->graph_queue_executor.Completed();
    DirectX::XMStoreFloat4x4(&light_inputs.unjittered_view, view);
    DirectX::XMStoreFloat4x4(&light_inputs.unjittered_projection, projection);
    light_inputs.camera_origin = lod_eye;
    light_inputs.near_plane = near_plane;
    light_inputs.far_plane = far_plane;
    LightListOutputs light_outputs;
    if (auto registered = impl_->light_list.Register(impl_->graph, light_inputs, light_outputs); !registered)
        return registered;
    pending_effects.lights = true;
    auto light_descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    light_descriptor.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
        kLightListDescriptorOffset) * impl_->srv_stride;
    for (const auto source : light_outputs.srvs)
    {
        impl_->device->CopyDescriptorsSimple(1, light_descriptor, source,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        light_descriptor.ptr += impl_->srv_stride;
    }
    LtcLutOutputs ltc_outputs;
    if (auto registered = impl_->ltc_lut.Register(impl_->graph,
            impl_->graph_queue_executor.Completed(), ltc_outputs); !registered) return registered;
    pending_effects.ltc = static_cast<bool>(ltc_outputs.matrix.resource);
    for (const auto source : ltc_outputs.srvs)
    {
        impl_->device->CopyDescriptorsSimple(1, light_descriptor, source,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        light_descriptor.ptr += impl_->srv_stride;
    }
    std::array<LocalProbeCapturePage, kAuthoredMaterialNames.size()> capture_pages;
    std::vector<LocalProbeSceneRead> capture_reads;
    LocalProbeRuntimeFrameInputs local_probe_inputs;
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
        local_probe_inputs.declare_shadow_sampling = [&](PassBuilder &pass) { read_material_sampling(pass, true); };
    local_probe_inputs.frame_srv_heap = impl_->srv_cpu_heap;
    local_probe_inputs.sampler_heap = impl_->sampler_heap;
    local_probe_inputs.frame_srv_base = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    local_probe_inputs.frame_srv_base.ptr += static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount * impl_->srv_stride;
    local_probe_inputs.sampler_table = sampler_table;
    local_probe_inputs.completed = impl_->graph_queue_executor.Completed();
    local_probe_inputs.bind_graphics = [&](ID3D12GraphicsCommandList *list) { bind_graphics(list); bind_scene_roots(list, false); };
    local_probe_inputs.restore_graphics = [&](ID3D12GraphicsCommandList *list) {
        bind_graphics(list); bind_scene_roots(list, false);
        list->OMSetRenderTargets(surface_target_count, surface_rtvs.data(), FALSE, &dsv);
    };
    if (impl_->local_probes.AllocatedBytes() != 0 && static_scene && impl_->static_gpu_scene)
    {
        local_probe_inputs.static_instances = impl_->static_gpu_scene->instances.resource;
        local_probe_inputs.static_allocation = impl_->static_gpu_scene->instances.allocation;
        local_probe_inputs.static_version = static_instances_version;
        local_probe_inputs.binding_owners.push_back(impl_->static_gpu_scene);
        struct CaptureNativeLease { ComPtr<D3D12MA::Allocation> allocation; ComPtr<ID3D12Resource> resource; };
        const auto capture_read = [&](AllocationResource &resource, bool texture, std::string_view name) {
            if (!resource.resource) return;
            const auto handle = texture ? import_texture(resource, Access::AllShaderRead, name)
                : import_buffer(resource, Access::AllShaderRead, name);
            const auto version = impl_->graph.Version(handle);
            if (std::ranges::none_of(capture_reads, [&](const auto &read) {
                    return read.version.resource.value == version.resource.value &&
                           read.version.resource.kind == version.resource.kind;
                }))
                capture_reads.push_back({version, Access::AllShaderRead});
            local_probe_inputs.binding_owners.push_back(std::make_shared<CaptureNativeLease>(CaptureNativeLease{resource.allocation, resource.resource}));
        };
        for (std::size_t material = 0; material < capture_pages.size(); ++material)
        {
            const auto &page = material == kTerrainMaterialOrdinal ? impl_->procedural_geometry : *bucket_pages[material * 3];
            auto &capture = capture_pages[material];
            capture.vertices = page.vertices.resource; capture.indices = page.indices.resource;
            capture.vertex_allocation = page.vertices.allocation; capture.index_allocation = page.indices.allocation;
            capture.vertex_version = impl_->graph.Version(import_buffer(const_cast<AllocationResource &>(page.vertices), Access::VertexBuffer, "ProbeCompleteVertices"));
            capture.index_version = impl_->graph.Version(import_buffer(const_cast<AllocationResource &>(page.indices), Access::IndexBuffer, "ProbeCompleteIndices"));
            capture.vertex_view = page.vertex_view; capture.index_view = page.index_view;
            capture.index_count = page.index_count; capture.generation = page.generation; capture.page_slot = page.page_slot;
            capture.bounds = page.metadata.bounds;
            capture.complete = page.vertices.resource && page.indices.resource && page.index_count != 0;
        }
        capture_read(frame.upload, false, "ProbeMainBindings");
        capture_read(impl_->material_parameters, false, "ProbeMaterialParameters");
        capture_read(impl_->hero_material_bindings_gpu, false, "ProbeHeroBindings");
        capture_read(impl_->environment_brdf_lut, true, "ProbeBrdfLut");
        for (const auto version : {probe_outputs.radiance, probe_outputs.prefiltered, probe_outputs.irradiance,
                light_outputs.lights, light_outputs.ranges, light_outputs.inline_indices, light_outputs.spill_indices,
                ltc_outputs.matrix, ltc_outputs.amplitude}) capture_reads.push_back({version, Access::AllShaderRead});
        if (virtual_shadow_outputs.resources[0])
            for (const auto version : {virtual_shadow_outputs.metadata, virtual_shadow_outputs.page_table,
                    virtual_shadow_outputs.static_bank, virtual_shadow_outputs.dynamic_bank, virtual_shadow_outputs.draw_records})
                capture_reads.push_back({version, Access::AllShaderRead});
        if (local_shadow_outputs.resources[0])
            for (const auto version : {local_shadow_outputs.metadata, local_shadow_outputs.records,
                    local_shadow_outputs.static_bank, local_shadow_outputs.dynamic_bank, local_shadow_outputs.draw_records})
                capture_reads.push_back({version, Access::AllShaderRead});
        local_probe_inputs.pages = capture_pages;
        local_probe_inputs.scene_reads = capture_reads;
    }
    LocalProbeOutputs local_probe_outputs;
    if (auto qualified = impl_->local_probes.CurrentRevisions(local_probe_inputs, pending_effects.probe_revisions); !qualified) return qualified;
    pending_effects.probe_scene = static_scene;
    pending_effects.local_probes = impl_->local_probes.AllocatedBytes() != 0;
    if (auto registered = impl_->local_probes.Register(impl_->graph, local_probe_inputs, local_probe_outputs); !registered) return registered;
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
    if (auto result = impl_->texture_feedback.AddDecodePass(impl_->graph, back_buffer_index,
            1201, feedback_bindings); !result) return result;
    RayGeometryOutputs ray_geometry_outputs;
    RaySceneOutputs ray_scene_outputs;
    std::vector<RayShaderRead> ray_reads;
    const auto make_ray_bindings = [&](std::uint32_t table_offset) -> RayFrameTableBuilder {
        return [&, table_offset](D3D12_CPU_DESCRIPTOR_HANDLE suffix, std::uint32_t count,
                RayShaderBinder &binder) -> Result {
            if ((count != kRayDescriptorCount && count != kRayTransportSuffixCount) || !suffix.ptr)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid ray descriptor suffix.");
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
                table_offset) * impl_->srv_stride;
            auto source = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            source.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
                kTemporalTextureDescriptorOffset) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kRayFrameTableCount, destination, source,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto ray_destination = destination;
            ray_destination.ptr += static_cast<SIZE_T>(kRayDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kRayDescriptorCount, ray_destination, suffix,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            if (count == kRayTransportSuffixCount)
            {
                destination.ptr += static_cast<SIZE_T>(kRayTransportDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
                suffix.ptr += static_cast<SIZE_T>(kRayDescriptorCount) * impl_->srv_stride;
                impl_->device->CopyDescriptorsSimple(kRayTransportDescriptorCount, destination, suffix,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
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
    std::vector<RaySceneGeometry> brixelizer_geometries;
    std::vector<BrixelizerSceneInstance> brixelizer_instances;
    BrixelizerGeometryLookup brixelizer_geometry_lookup;
    std::vector<std::shared_ptr<const void>> brixelizer_owners;
    std::unordered_set<D3D12MA::Allocation *> brixelizer_backing;
    std::uint64_t brixelizer_light_epoch = impl_->pipeline_revision ^ (impl_->material_parameter_revision * 1099511628211ull);
    if (const auto digest = impl_->config.product_digests.find("render/lighting/default_probe.rpkg");
        digest != impl_->config.product_digests.end()) brixelizer_light_epoch ^= MakeAssetId(digest->second).value;
    if (active_features.software_gi_enabled)
    {
        if (impl_->brixelizer_history_valid && (new_session ||
                !(impl_->temporal_history_valid || impl_->hzb_history_valid) ||
                impl_->brixelizer_gi.Status().light_revision != brixelizer_light_epoch))
        {
            if (auto drained = impl_->WaitForGpu(); !drained) return drained;
            if (auto reset = impl_->brixelizer_gi.ResetHistoryAfterCompletion(
                    impl_->graph_queue_executor.Completed(), impl_->view_state.render_active, new_session); !reset) return reset;
            impl_->brixelizer_history_valid = false;
        }
        brixelizer_owners.push_back(impl_->resident_generation_lease);
    }
    if (active_features.canonical_geometry_enabled)
    {
        const auto add_read = [&](ResourceVersion version, ID3D12Resource *resource) {
            if (resource && std::ranges::none_of(ray_reads, [&](const auto &read) { return read.resource == resource; }))
                ray_reads.push_back({version, resource, Access::ComputeRead});
        };
        add_read(material_parameter_version, impl_->material_parameters.resource.Get());
        add_read(hero_binding_version, impl_->hero_material_bindings_gpu.resource.Get());
        add_read(impl_->graph.Version(import_buffer(frame.upload, Access::ComputeRead, "RayFrameConstants")),
            frame.upload.resource.Get());
        add_read(dynamic_instances_version, impl_->dynamic_instances.resource.Get());
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
            if (resource) ray_reads.push_back({version, resource, Access::AllShaderRead});
        for (const auto &[version, resource] : {
                std::pair{probe_outputs.radiance, probe_outputs.radiance_resource},
                std::pair{probe_outputs.prefiltered, probe_outputs.prefiltered_resource},
                std::pair{impl_->graph.Version(environment_brdf_lut), impl_->environment_brdf_lut.resource.Get()},
                std::pair{ltc_outputs.matrix, ltc_outputs.matrix_resource},
                std::pair{ltc_outputs.amplitude, ltc_outputs.amplitude_resource}})
            if (resource) ray_reads.push_back({version, resource, Access::AllShaderRead});
        if (have_skinned_instances) add_read(skin_cache_version, frame.skin_cache_vertices.resource.Get());
        const auto add_texture = [&](AllocationResource &texture) {
            if (texture.resource && std::ranges::none_of(ray_reads, [&](const auto &read) { return read.resource == texture.resource.Get(); }))
                ray_reads.push_back({impl_->graph.Version(import_texture(texture, Access::AllShaderRead, "RayMaterialTexture")),
                    texture.resource.Get(), Access::AllShaderRead});
        };
        for (const auto &binding : texture_bindings)
            add_read(binding.sampled, impl_->texture_streaming.View(binding.texture).resource);
        add_read(texture_metadata, static_cast<ID3D12Resource *>(impl_->graph.Resolve(texture_metadata.resource)));
        for (auto *texture : {&impl_->archer_diffuse, &impl_->archer_normal, &impl_->monster_basecolor,
                &impl_->monster_emissive, &impl_->monster_ram, &impl_->family_diffuse, &impl_->family_normal}) add_texture(*texture);


        std::vector<RayHitGeometry> jobs;
        std::vector<RayDrawInstance> draws;
        std::vector<const GeometryPageGpu *> ray_pages;
        std::vector<const RenderInstance *> ray_sources;
        std::vector<std::array<ResourceVersion, 2>> ray_page_versions;
        const auto append_ray = [&](const RenderInstance &source, const GpuInstance &draw,
                std::uint32_t stable_slot, std::uint32_t generation, std::uint32_t skin_slot) -> Result {
            const bool environment = is_environment_mesh(source.mesh);
            const bool terrain = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch;
            const bool hero = source.mesh == RenderMesh::Archer || is_monster(source.mesh);
            const auto ordinal = environment ? static_cast<std::uint32_t>(environment_mesh_index(source)) : terrain ? 14u : UINT32_MAX;
            const auto page_slot = environment ? ordinal * 3u : source.mesh == RenderMesh::Archer ? kArcherGeometryPage :
                is_monster(source.mesh) ? kMonsterGeometryPage + static_cast<std::uint32_t>(source.mesh) -
                    static_cast<std::uint32_t>(RenderMesh::MonsterMelee) :
                source.mesh == RenderMesh::EnemyProjectile ? kProjectileGeometryPage : kProceduralGeometryPage;
            const auto *page = impl_->FindGeometryPage(page_slot);
            if (!page)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Ray candidate has no complete resident geometry page.");
            std::array<ResourceVersion, 2> versions{
                impl_->graph.Version(import_buffer(const_cast<AllocationResource &>(page->vertices), Access::VertexBuffer, "RayCanonicalVertices")),
                impl_->graph.Version(import_buffer(const_cast<AllocationResource &>(page->indices), Access::IndexBuffer, "RayCanonicalIndices"))};
            add_read(versions[0], page->vertices.resource.Get());
            add_read(versions[1], page->indices.resource.Get());
            const bool masked = environment && render_data::ClassifyEnvironmentMaterial(
                impl_->environment_meshes[ordinal * 3].material) == render_data::EnvironmentMaterialVariant::MaskedTwoSided;
            const bool builtin = page->page_slot == kProceduralGeometryPage;
            std::uint32_t flags = hero ? RayGeometryCachedSkin : 0u;
            if (masked || source.mesh == RenderMesh::TreeCanopy || source.mesh == RenderMesh::Grass)
                flags |= RayGeometryTwoSided;
            if (environment) flags |= RayGeometryAuthoredEnvironment;
            if (builtin) flags |= RayGeometryBuiltin;
            if (!hero && (builtin || source.mesh == RenderMesh::Grass)) flags |= RayGeometryDeformed;
            if (source.mesh == RenderMesh::EnemyProjectile) flags |= RayGeometryTransmissive;
            const std::uint32_t material_word = ordinal < 15 ?
                (impl_->material_parameter_bases[ordinal] << 1) | static_cast<std::uint32_t>(masked) : 0u;
            jobs.push_back({1u + page->page_slot * kGeometryBuffersPerPage,
                2u + page->page_slot * kGeometryBuffersPerPage, 0u, page->index_count,
                0u, sizeof(Vertex), page->vertex_count, 0u, stable_slot, generation,
                skin_slot, static_cast<std::uint32_t>(source.mesh), ordinal, material_word, flags, page->generation});
            const auto mesh_key = MakeAssetId("render/geometry/page/" + std::to_string(page->page_slot)).value;
            jobs.back().mesh_key_low = static_cast<std::uint32_t>(mesh_key);
            jobs.back().mesh_key_high = static_cast<std::uint32_t>(mesh_key >> 32);
            jobs.back().mesh_generation_low = page->generation;
            jobs.back().mesh_generation_high = 0;
            draws.push_back({draw.position, draw.scale, draw.color, draw.mesh, draw.yaw, draw.padding});
            ray_pages.push_back(page);
            ray_sources.push_back(&source);
            ray_page_versions.push_back(versions);
            return Result::Success();
        };
        std::vector<bool> included(persistent_instances.size());
        for (std::size_t index = 0; index < render_instances.size(); ++index)
        {
            const auto &reference = dynamic_draw_refs[index];
            if (included[reference.instance_index]) continue;
            included[reference.instance_index] = true;
            if (auto appended = append_ray(render_instances[index], persistent_instances[reference.instance_index],
                    reference.instance_index, reference.instance_generation, dynamic_skin_slots[index]); !appended) return appended;
        }
        if (static_scene && impl_->static_gpu_scene)
            for (std::size_t index = 0; index < static_scene->instances.size(); ++index)
            {
                const auto &source = static_scene->instances[index];
                if (!RetainDecorativeFoliage(source, index, foliage_density)) continue;
                const bool environment = is_environment_mesh(source.mesh);
                const GpuInstance draw{{source.position.x, source.position.y + (environment ? 0.0f : source.scale.y * 0.5f), source.position.z, 0},
                    {source.scale.x, source.scale.y, source.scale.z, 1}, source.color_rgba,
                    static_cast<std::uint32_t>(source.mesh) | (environment ? 0x10000u : 0u), source.yaw,
                    environment ? 0.0f : std::bit_cast<float>(source.environment_seed)};
                if (auto appended = append_ray(source, draw, 0x80000000u | static_cast<std::uint32_t>(index),
                        impl_->static_gpu_scene->instance_generation, UINT32_MAX); !appended) return appended;
            }
        if (!jobs.empty())
        {
            if (auto registered = impl_->ray_geometry.Register(impl_->graph,
                    {jobs, draws, ray_reads, make_ray_bindings(kRayGeometryTableOffset), back_buffer_index,
                     impl_->graph_queue_executor.Completed()}, ray_geometry_outputs); !registered) return registered;
            pending_effects.ray_geometry = true;
            if (active_features.software_gi_enabled && ray_geometry_outputs.owner)
                brixelizer_owners.push_back(ray_geometry_outputs.owner);
            std::vector<RaySceneGeometry> geometries;
            std::vector<RaySceneInstance> instances;
            geometries.reserve(jobs.size()); instances.reserve(jobs.size());
            for (std::size_t index = 0; index < jobs.size(); ++index)
            {
                const auto &job = ray_geometry_outputs.geometries[index];
                const auto &draw = draws[index];
                const auto &page = *ray_pages[index];
                const bool deformed = (job.flags & RayGeometryDeformed) != 0;
                const bool skin = (job.flags & RayGeometryCachedSkin) != 0;
                const std::uint64_t vertex_offset = deformed ? job.position_byte_offset : skin ?
                    std::uint64_t(job.skin_slot) * skin_slot_stride * sizeof(Vertex) : 0;
                if (skin && vertex_offset + std::uint64_t(page.vertex_count) * sizeof(Vertex) > UINT32_MAX)
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Ray SkinCache range exceeds shader addressing.");
                geometries.push_back({std::uint64_t(page.page_slot) + 1, page.generation, impl_->frame_number + 1,
                    deformed ? ray_geometry_outputs.position_resource : skin ? frame.skin_cache_vertices.resource.Get() : page.vertices.resource.Get(),
                    page.indices.resource.Get(), deformed ? ray_geometry_outputs.positions : skin ? skin_cache_version : ray_page_versions[index][0],
                    ray_page_versions[index][1], vertex_offset, 0, deformed ? 12u : static_cast<std::uint32_t>(sizeof(Vertex)),
                    page.vertex_count, page.index_count, DXGI_FORMAT_R32_UINT, false, deformed || skin, new_session});
                // Immutable shared pages retain their revision through frames.
                if (!deformed && !skin) geometries.back().current_revision = page.generation;
                DirectX::XMFLOAT4X4 world;
                DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixScaling(draw.scale.x, draw.scale.y, draw.scale.z) *
                    DirectX::XMMatrixRotationY(draw.yaw) * DirectX::XMMatrixTranslation(draw.position.x, draw.position.y, draw.position.z));
                const auto mesh_key = MakeAssetId("render/geometry/page/" + std::to_string(page.page_slot)).value;
                const auto material_id = job.environment_ordinal < 15 ? 0x10000000u + job.environment_ordinal : 0x30000000u + job.mesh_id;
                instances.push_back({job.stable_gpu_scene_slot, job.instance_generation, static_cast<std::uint32_t>(index),
                    material_id, mesh_key, page.generation, world,
                    static_cast<std::uint32_t>((job.flags & RayGeometryTwoSided) != 0 ?
                        D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE : D3D12_RAYTRACING_INSTANCE_FLAG_NONE), 0xff});
                if (active_features.software_gi_enabled)
                {
                    // Retain the allocator's actual backing in addition to COM
                    // resources; native geometry IDs can outlive this frame slot.
                    const auto retain_allocation = [&](const AllocationResource &backing) {
                        if (!backing.allocation || !brixelizer_backing.insert(backing.allocation).second) return;
                        backing.allocation->AddRef();
                        brixelizer_owners.emplace_back(backing.allocation, [](const void *value) {
                            const_cast<D3D12MA::Allocation *>(static_cast<const D3D12MA::Allocation *>(value))->Release();
                        });
                    };
                    retain_allocation(page.vertices); retain_allocation(page.indices);
                    if (skin) retain_allocation(frame.skin_cache_vertices);
                    const auto &source = *ray_sources[index];
                    const auto &sphere = page.metadata.bounds.sphere;
                    const float sx = sphere[0] * draw.scale.x, sy = sphere[1] * draw.scale.y, sz = sphere[2] * draw.scale.z;
                    const float sine = std::sin(draw.yaw), cosine = std::cos(draw.yaw);
                    const float maximum_scale = std::max({std::abs(draw.scale.x), std::abs(draw.scale.y), std::abs(draw.scale.z)});
                    DirectX::XMFLOAT3 center{draw.position.x + cosine * sx + sine * sz, draw.position.y + sy,
                        draw.position.z - sine * sx + cosine * sz};
                    float radius = sphere[3] * maximum_scale;
                    if (skin)
                    {
                        center = {draw.position.x, draw.position.y, draw.position.z};
                        radius = std::max(draw.position.w, std::sqrt(sx*sx + sy*sy + sz*sz) + radius);
                    }
                    if (source.mesh == RenderMesh::Grass) radius += grass_visibility_radius;
                    std::span<const render_data::CharacterMaterialBinding> bindings;
                    if (source.mesh == RenderMesh::Archer) bindings = impl_->archer_material_bindings;
                    else if (is_monster(source.mesh)) bindings = impl_->monster_assets[
                        static_cast<std::size_t>(source.mesh) - static_cast<std::size_t>(RenderMesh::MonsterMelee)].material_bindings;
                    const auto ordinal = job.environment_ordinal;
                    const auto capabilities = ordinal < kAuthoredMaterialNames.size()
                        ? impl_->material_surface_layouts[ordinal].capability_mask : 0u;
                    if (auto appended = AppendBrixelizerLeaf(geometries.back(), instances.back(), page.submeshes, bindings,
                            std::span(impl_->material_surface_layouts).subspan(kAuthoredMaterialNames.size()), capabilities,
                            (job.flags & RayGeometryTwoSided) != 0,
                            (job.flags & RayGeometryTransmissive) != 0,
                            {center.x-radius,center.y-radius,center.z-radius},
                            {center.x+radius,center.y+radius,center.z+radius},
                            source.revisions.material ^ impl_->material_parameter_revision,
                            brixelizer_geometry_lookup,brixelizer_geometries,brixelizer_instances); !appended) return appended;
                }
            }
            if (active_features.ray_scene_enabled)
            {
            if (impl_->ray_transport.GetNativeTraceSelection().use_omms)
            {
                std::vector<RayOmmGeometryRequest> requests(geometries.size());
                std::vector<std::vector<std::uint32_t>> words(geometries.size());
                for (std::size_t index=0;index<geometries.size();++index)
                    if (auto prepared=impl_->BuildRayOmmRequest(*ray_pages[index],ray_geometry_outputs.geometries[index],requests[index],words[index]);!prepared)return prepared;
                std::uint64_t headroom{};
                if(auto budget=impl_->NativeRayBudget(headroom);!budget)return budget;
                if(headroom>UINT64_MAX-impl_->ray_omm.AllocatedBytes())
                    return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Native OMM live/remaining budget overflow.");
                if(auto budget=impl_->ray_omm.SetNativeByteBudget(headroom+impl_->ray_omm.AllocatedBytes());!budget)return budget;
                std::vector<RayOmmBinding> bindings;
                pending_effects.ray_omm=true;
                if(auto registered=impl_->ray_omm.Register(impl_->graph,requests,back_buffer_index,impl_->graph_queue_executor.Completed(),bindings);!registered)return registered;
                for(std::size_t index=0;index<bindings.size();++index)geometries[index].omm=std::move(bindings[index]);
                // Array allocation admission precedes the additional BLAS/TLAS
                // budget query, so both consume one actual remaining headroom.
                if(auto budget=impl_->NativeRayBudget(headroom);!budget)return budget;
                if(headroom>UINT64_MAX-impl_->ray_scene.AllocatedBytes())
                    return Result::Failure(ErrorCode::InvalidState,"hs_renderer_d3d12","Native OMM scene live/remaining budget overflow.");
                if(auto budget=impl_->ray_scene.SetNativeByteBudget(headroom+impl_->ray_scene.AllocatedBytes());!budget)return budget;
            }
            pending_effects.ray_scene = true;
            if (auto registered = impl_->ray_scene.Register(impl_->graph,
                    {geometries, instances, back_buffer_index, impl_->graph_queue_executor.Completed()}, ray_scene_outputs); !registered) return registered;
            }
        }
        else
        {
            if(impl_->ray_transport.GetNativeTraceSelection().use_omms)
            {
                std::vector<RayOmmBinding> unused;
                pending_effects.ray_omm=true;
                if(auto registered=impl_->ray_omm.Register(impl_->graph,{},back_buffer_index,impl_->graph_queue_executor.Completed(),unused);!registered)return registered;
            }
            impl_->ray_ambient_occlusion.ResetHistory();
            impl_->ray_transport.ResetHistory();
        }
    }
    // Signal composition follows the existing native graph boundaries. Handles
    // retain their concrete producer, logical extent and exact graph ordinal.
    std::vector<SignalHandle> feature_signals;
    const SignalExtent feature_extent{impl_->view_state.render_active.width, impl_->view_state.render_active.height};
    const auto feature_declaration = [&](SignalSemantic semantic, SignalEncoding encoding,
            RadiometricMeaning meaning = RadiometricMeaning::None, std::uint32_t version = 0,
            SignalExtent extent = {}, ResourceKind kind = ResourceKind::Texture, std::uint32_t layers = 1) {
        if (!extent.width) extent = feature_extent;
        return SignalDeclaration{{semantic, impl_->active_view_id.value, version}, {kind, extent, encoding, meaning, layers}};
    };
    const auto import_feature_signal = [&](SignalDeclaration declaration, ResourceVersion version) {
        if (!version.resource) return;
        const auto producer = std::ranges::find_if(feature_signals, [&](const auto &signal) {
            return signal.resource.resource.value == version.resource.value && signal.resource.resource.kind == version.resource.kind &&
                signal.resource.ordinal == version.ordinal && signal.declaration.metadata == declaration.metadata;
        });
        const auto origin = producer == feature_signals.end() ? FeatureId::Imported : producer->producer;
        feature_signals.push_back({declaration, origin, version});
    };
    const auto feature_resource = [&](std::span<const SignalHandle> signals, SignalSemantic semantic,
            std::uint32_t version = 0) -> ResourceVersion {
        const auto found = std::ranges::find_if(signals, [&](const auto &signal) {
            return signal.declaration.key == SignalKey{semantic, impl_->active_view_id.value, version};
        });
        return found == signals.end() ? ResourceVersion{} : found->resource;
    };
    const auto feature_guide = [&](std::span<const SignalHandle> signals, SignalSemantic semantic,
            std::uint32_t version = 0) {
        const auto resource = feature_resource(signals, semantic, version);
        return AmbientOcclusionGuide{resource, resource.resource
            ? static_cast<ID3D12Resource *>(impl_->graph.Resolve(resource.resource)) : nullptr};
    };
    const auto require_guide = [](SignalDeclaration declaration, bool history = false) {
        declaration.usage = history ? SignalUsage::History : SignalUsage::Guide;
        return declaration;
    };
    const auto emit_feature_signal = [](std::vector<SignalHandle> &signals, FeatureId producer,
            SignalDeclaration declaration, ResourceVersion resource) {
        signals.push_back({declaration, producer, resource});
    };
    const auto compose_features = [&](std::vector<FeatureRegistration> registrations) -> Result {
        FeatureComposition composed;
        if (auto result = ComposeRenderFeatures(feature_signals, registrations, composed); !result) return result;
        feature_signals = std::move(composed.signals);
        return Result::Success();
    };
    const auto register_software_gi_scene = [&]() -> Result {
        BrixelizerSceneInputs native_scene;
        native_scene.geometries = brixelizer_geometries; native_scene.instances = brixelizer_instances;
        native_scene.sdf_center = lod_eye; native_scene.frame_slot = back_buffer_index;
        native_scene.frame_index = impl_->frame_number; native_scene.scene_revision = impl_->pipeline_revision + 1;
        native_scene.completed = impl_->graph_queue_executor.Completed(); native_scene.owners = brixelizer_owners;
        BrixelizerSceneOutputs native_scene_outputs;
        pending_effects.brixelizer = true;
        if (auto registered = impl_->brixelizer_gi.RegisterScene(impl_->graph, native_scene, native_scene_outputs); !registered) return registered;
        return Result::Success();
    };
    AmbientOcclusionConstants ambient_constants;
    const bool previous_view_valid = impl_->temporal_history_valid || impl_->hzb_history_valid;
    ambient_constants.inverse_view_projection = camera_relative_inverse_view_projection;
    DirectX::XMStoreFloat4x4(&ambient_constants.world_to_view,
        DirectX::XMMatrixTranslationFromVector(eye) * view);
    DirectX::XMStoreFloat4x4(&ambient_constants.projection,
        projection * DirectX::XMMatrixTranslation(jitter_x, jitter_y, 0.0f));
    ambient_constants.previous_depth_parameters = previous_view_valid
        ? impl_->previous_projection_depth
        : DirectX::XMFLOAT4{lod_projection._33, lod_projection._34, lod_projection._43, lod_projection._44};
    ambient_constants.jitter_uv = constants->surface_jitter_uv;
    ambient_constants.toward_light = {-constants->light_direction_intensity.x,
        -constants->light_direction_intensity.y, -constants->light_direction_intensity.z, 0};
    AdaptiveShadingOutputs adaptive_outputs;
    const auto adaptive_status = impl_->adaptive_shading.Status();
    if (adaptive_status.raster_ready || adaptive_status.compute_ready)
    {
        AdaptiveShadingInputs adaptive_inputs;
        adaptive_inputs.depth = {depth_version, impl_->depth.resource.Get()};
        adaptive_inputs.normal = {gbuffer_normal_version, surface_normal_resource};
        adaptive_inputs.base = {gbuffer_base_version, surface_base_resource};
        adaptive_inputs.material = {gbuffer_material_version, surface_material_resource};
        adaptive_inputs.metadata = {gbuffer_metadata_version, impl_->gbuffer_metadata.resource.Get()};
        adaptive_inputs.motion = {gbuffer_motion_version, impl_->gbuffer_motion.resource.Get()};
        adaptive_inputs.render_active = impl_->view_state.render_active;
        adaptive_inputs.depth_parameters = {ambient_constants.projection._33, ambient_constants.projection._34,
            ambient_constants.projection._43, ambient_constants.projection._44};
        adaptive_inputs.material_policy = impl_->adaptive_material_policies;
        adaptive_inputs.frame_slot = back_buffer_index;
        adaptive_inputs.completed = impl_->graph_queue_executor.Completed();
        adaptive_inputs.force_full_rate = !previous_view_valid || new_session;
        adaptive_inputs.owners.push_back(impl_->resident_generation_lease);
        // Set the completion guard before any graph registration can mutate the
        // service. Policies are copied to this completed frame bank immediately.
        pending_effects.adaptive = true;
        if (auto registered = impl_->adaptive_shading.Register(impl_->graph, adaptive_inputs, adaptive_outputs);
                !registered) return registered;
    }
    AmbientOcclusionOutputs ambient_outputs;
    AmbientOcclusionInputs ambient_inputs{
        {depth_version, impl_->depth.resource.Get()},
        {gbuffer_normal_version, surface_normal_resource},
        {gbuffer_metadata_version, impl_->gbuffer_metadata.resource.Get()},
        {gbuffer_motion_version, impl_->gbuffer_motion.resource.Get()},
        {gbuffer_surface_history_version, impl_->gbuffer_surface_history.resource.Get()},
        impl_->view_state.render_active, back_buffer_index, impl_->frame_number,
        impl_->graph_queue_executor.Completed(), !previous_view_valid || new_session};
    const auto depth_signal = feature_declaration(SignalSemantic::Depth, SignalEncoding::Depth);
    const auto normal_signal = feature_declaration(SignalSemantic::Normal, SignalEncoding::Normal);
    const auto metadata_signal = feature_declaration(SignalSemantic::SurfaceMetadata, SignalEncoding::Metadata);
    const auto motion_signal = feature_declaration(SignalSemantic::Motion, SignalEncoding::Motion);
    const auto surface_history_signal = feature_declaration(SignalSemantic::SurfaceHistory, SignalEncoding::SurfaceHistory);
    const auto base_signal = feature_declaration(SignalSemantic::BaseColor, SignalEncoding::RgbaUnorm);
    const auto material_signal = feature_declaration(SignalSemantic::Material, SignalEncoding::RgbaUnorm);
    const auto ambient_signal = feature_declaration(SignalSemantic::AmbientVisibility, SignalEncoding::Float);
    const auto contact_signal = feature_declaration(SignalSemantic::ContactVisibility, SignalEncoding::Float);
    for (const auto &[declaration, version] : {
            std::pair{depth_signal, depth_version}, std::pair{normal_signal, gbuffer_normal_version},
            std::pair{metadata_signal, gbuffer_metadata_version}, std::pair{motion_signal, gbuffer_motion_version},
            std::pair{surface_history_signal, gbuffer_surface_history_version},
            std::pair{base_signal, gbuffer_base_version}, std::pair{material_signal, gbuffer_material_version}})
        import_feature_signal(declaration, version);
    std::vector<SignalDeclaration> closure_requirements, ray_requirements;
    const auto import_backing = [&](SignalSemantic semantic, ResourceVersion version, std::uint32_t component,
            SignalEncoding encoding = SignalEncoding::RawBytes) {
        if (!version.resource) return;
        auto *native = static_cast<ID3D12Resource *>(impl_->graph.Resolve(version.resource));
        const auto desc = native->GetDesc();
        const auto declaration = feature_declaration(semantic, encoding, RadiometricMeaning::None, component,
            {static_cast<UINT>(std::min<UINT64>(desc.Width, UINT32_MAX)), desc.Height}, version.resource.kind,
            version.resource.kind == ResourceKind::Texture ? desc.DepthOrArraySize : 1);
        import_feature_signal(declaration, version);
        (semantic == SignalSemantic::ClosureBacking ? closure_requirements : ray_requirements).push_back(declaration);
    };
    for (UINT index = 0; index < ray_reads.size(); ++index)
        import_backing(SignalSemantic::ClosureBacking, ray_reads[index].version, index);
    import_backing(SignalSemantic::RayScene, ray_scene_outputs.tlas, 0, SignalEncoding::AccelerationStructure);
    import_backing(SignalSemantic::RayScene, ray_scene_outputs.omm_view.tlas, 1, SignalEncoding::AccelerationStructure);
    import_backing(SignalSemantic::RaySceneMetadata, ray_scene_outputs.metadata, 0);
    import_backing(SignalSemantic::RayGeometryPositions, ray_geometry_outputs.positions, 0);
    import_backing(SignalSemantic::RayGeometryRecords, ray_geometry_outputs.records, 0);
    import_backing(SignalSemantic::RayGeometryInstances, ray_geometry_outputs.instances, 0);
    const std::vector ambient_requirements{require_guide(depth_signal), require_guide(normal_signal),
        require_guide(metadata_signal), require_guide(motion_signal), require_guide(surface_history_signal, true)};
    const auto with_ray_requirements = [&](std::vector<SignalDeclaration> requirements) {
        requirements.insert(requirements.end(), ray_requirements.begin(), ray_requirements.end());
        requirements.insert(requirements.end(), closure_requirements.begin(), closure_requirements.end());
        return requirements;
    };
    const auto resolve_ambient_guides = [&](std::span<const SignalHandle> signals, AmbientOcclusionInputs &inputs) {
        inputs.depth = feature_guide(signals, SignalSemantic::Depth);
        inputs.normal = feature_guide(signals, SignalSemantic::Normal);
        inputs.metadata = feature_guide(signals, SignalSemantic::SurfaceMetadata);
        inputs.motion = feature_guide(signals, SignalSemantic::Motion);
        inputs.surface_history = feature_guide(signals, SignalSemantic::SurfaceHistory);
    };
    const auto resolve_ray_dependencies = [&](std::span<const SignalHandle> signals,
            RaySceneOutputs &scene, RayGeometryOutputs &geometry) {
        scene.tlas = feature_resource(signals, SignalSemantic::RayScene);
        scene.metadata = feature_resource(signals, SignalSemantic::RaySceneMetadata);
        if (scene.omm_view.tlas.resource) scene.omm_view.tlas = feature_resource(signals, SignalSemantic::RayScene, 1);
        geometry.positions = feature_resource(signals, SignalSemantic::RayGeometryPositions);
        geometry.records = feature_resource(signals, SignalSemantic::RayGeometryRecords);
        geometry.instances = feature_resource(signals, SignalSemantic::RayGeometryInstances);
        for (UINT index = 0; index < ray_reads.size(); ++index)
            ray_reads[index].version = feature_resource(signals, SignalSemantic::ClosureBacking, index);
    };
    std::vector<FeatureRegistration> visibility_features;
    FeatureRegistration raster_ambient{FeatureId::AmbientOcclusion,
        [&] { return active_features.ao_mode == GraphicsAoMode::GTAO || impl_->config.contact_shadows; },
        ambient_requirements, {},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            resolve_ambient_guides(signals, ambient_inputs);
            ambient_inputs.adaptive_density = feature_guide(signals, SignalSemantic::AdaptiveDensity);
            if (auto registered = impl_->ambient_occlusion.Register(impl_->graph, ambient_inputs,
                    ambient_constants, ambient_outputs); !registered) return registered;
            pending_effects.ambient = ambient_outputs.ao.resource || ambient_outputs.contact.resource;
            if (active_features.ao_mode == GraphicsAoMode::GTAO)
                emit_feature_signal(outputs, FeatureId::AmbientOcclusion, ambient_signal, ambient_outputs.ao);
            if (impl_->config.contact_shadows)
                emit_feature_signal(outputs, FeatureId::AmbientOcclusion, contact_signal, ambient_outputs.contact);
            return Result::Success();
        }};
    if (active_features.ao_mode == GraphicsAoMode::GTAO) raster_ambient.outputs.push_back(ambient_signal);
    if (impl_->config.contact_shadows) raster_ambient.outputs.push_back(contact_signal);
    if (adaptive_outputs.density.version.resource)
    {
        const auto density = feature_declaration(SignalSemantic::AdaptiveDensity, SignalEncoding::Float,
            RadiometricMeaning::None, 0, {(feature_extent.width + 7) / 8, (feature_extent.height + 7) / 8});
        import_feature_signal(density, adaptive_outputs.density.version);
        raster_ambient.inputs.push_back(require_guide(density));
    }
    visibility_features.push_back(std::move(raster_ambient));
    visibility_features.push_back({FeatureId::RayAmbientOcclusion,
        [&] { return active_features.ao_mode == GraphicsAoMode::RTAO && ray_scene_outputs.tlas.resource; },
        with_ray_requirements(ambient_requirements), {ambient_signal},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            resolve_ambient_guides(signals, ambient_inputs);
            RayAmbientOcclusionConstants ray_constants{ambient_constants,
                {lod_eye.x, lod_eye.y, lod_eye.z, 0}, {0.002f, 0, 0, 0}};
            RayAmbientOcclusionInputs ray_inputs{ray_scene_outputs, ray_geometry_outputs, ray_reads,
                ambient_inputs.depth, ambient_inputs.normal, ambient_inputs.metadata, ambient_inputs.motion, ambient_inputs.surface_history,
                make_ray_bindings(kRayAmbientTableOffset), ambient_inputs.render_active, back_buffer_index,
                impl_->frame_number, impl_->graph_queue_executor.Completed(), ambient_inputs.reset_history};
            resolve_ray_dependencies(signals, ray_inputs.scene, ray_inputs.geometry);
            ray_inputs.camera = impl_->sdk_camera; ray_inputs.identity = impl_->sdk_frame_identity;
            RayAmbientOcclusionOutputs ray_outputs;
            pending_effects.ray_ambient = true;
            if (auto registered = impl_->ray_ambient_occlusion.Register(impl_->graph,
                    ray_inputs, ray_constants, ray_outputs); !registered) return registered;
            ambient_outputs.ao = ray_outputs.visibility;
            ambient_outputs.ao_resource = ray_outputs.visibility_resource;
            ambient_outputs.ao_srv = ray_outputs.visibility_srv;
            emit_feature_signal(outputs, FeatureId::RayAmbientOcclusion, ambient_signal, ray_outputs.visibility);
            return Result::Success();
        }});
    const std::array shadow_signals{
        feature_declaration(SignalSemantic::RayShadowMetadata, SignalEncoding::RawBytes, RadiometricMeaning::None, 0,
            {sizeof(RayShadowMetadata), 1}, ResourceKind::Buffer),
        feature_declaration(SignalSemantic::RayShadowRecords, SignalEncoding::RawBytes, RadiometricMeaning::None, 0,
            {static_cast<UINT>(sizeof(GpuRayShadowRecord) * impl_->QualityProfileFor(impl_->config.shadow_quality).ray_shadow_max_lights), 1}, ResourceKind::Buffer),
        feature_declaration(SignalSemantic::RayShadowVisibility, SignalEncoding::Float, RadiometricMeaning::None, 0, feature_extent,
            ResourceKind::Texture, impl_->QualityProfileFor(impl_->config.shadow_quality).ray_shadow_max_lights),
        feature_declaration(SignalSemantic::RayShadowConfidence, SignalEncoding::Float, RadiometricMeaning::None, 0, feature_extent,
            ResourceKind::Texture, impl_->QualityProfileFor(impl_->config.shadow_quality).ray_shadow_max_lights),
        feature_declaration(SignalSemantic::RayShadowDepth, SignalEncoding::Depth),
        feature_declaration(SignalSemantic::RayShadowNormal, SignalEncoding::Normal)};
    FeatureRegistration ray_shadow_feature{FeatureId::RayShadows,
        [&] { return active_features.shadow_mode == GraphicsShadowMode::HybridRT && ray_scene_outputs.tlas.resource; },
        with_ray_requirements(ambient_requirements), {shadow_signals.begin(), shadow_signals.end()},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            RayShadowInputs inputs;
            inputs.scene = ray_scene_outputs; inputs.geometry = ray_geometry_outputs;
            resolve_ray_dependencies(signals, inputs.scene, inputs.geometry);
            inputs.reads = ray_reads; inputs.lights = frame_lights;
            const auto depth = feature_guide(signals, SignalSemantic::Depth);
            inputs.depth = {depth.version, depth.resource};
            const auto normal = feature_guide(signals, SignalSemantic::Normal);
            inputs.normal = {normal.version, normal.resource};
            const auto metadata = feature_guide(signals, SignalSemantic::SurfaceMetadata);
            inputs.metadata = {metadata.version, metadata.resource};
            const auto motion = feature_guide(signals, SignalSemantic::Motion);
            inputs.motion = {motion.version, motion.resource};
            const auto surface_history = feature_guide(signals, SignalSemantic::SurfaceHistory);
            inputs.surface_history = {surface_history.version, surface_history.resource};
            const auto noise = feature_guide(signals, SignalSemantic::BlueNoise);
            inputs.blue_noise = {noise.version, noise.resource};
            inputs.camera = impl_->sdk_camera; inputs.identity = impl_->sdk_frame_identity;
            inputs.render_active = impl_->view_state.render_active; inputs.frame_slot = back_buffer_index;
            inputs.completed = impl_->graph_queue_executor.Completed(); inputs.reset_history = !previous_view_valid || new_session;
            UINT binding_slot{};
            inputs.make_bindings = [&](D3D12_CPU_DESCRIPTOR_HANDLE ray, D3D12_CPU_DESCRIPTOR_HANDLE shadow,
                    RayShaderBinder &binder) -> Result {
                if (!ray.ptr || !shadow.ptr || binding_slot >= kRayShadowMaximumLights)
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid ray shadow table or context capacity.");
                const auto table_offset = kRayShadowFrameTableOffset + binding_slot++ * kRayFrameTableCount;
                auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
                destination.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount + table_offset) * impl_->srv_stride;
                auto source = local_probe_inputs.frame_srv_base; source.ptr += kTemporalTextureDescriptorOffset * impl_->srv_stride;
                impl_->device->CopyDescriptorsSimple(kRayFrameTableCount, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                auto ray_destination = destination;
                ray_destination.ptr += (kRayDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
                impl_->device->CopyDescriptorsSimple(kRayDescriptorCount, ray_destination, ray, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                destination.ptr += (kRayShadowDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
                impl_->device->CopyDescriptorsSimple(kRayShadowOwnDescriptorCount, destination, shadow, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                auto table = texture_table; table.ptr += UINT64(table_offset) * impl_->srv_stride;
                binder = [&, table](ID3D12GraphicsCommandList *list) -> Result {
                    bind_surface_compute(list); list->SetComputeRootDescriptorTable(32, table); return Result::Success();
                };
                return Result::Success();
            };
            pending_effects.ray_shadows = true;
            if (auto registered = impl_->ray_shadows.Register(impl_->graph, inputs, ray_shadow_outputs); !registered) return registered;
            auto destination = local_probe_inputs.frame_srv_base;
            destination.ptr += kRayShadowExportDescriptorOffset * impl_->srv_stride;
            for (const auto srv : ray_shadow_outputs.srvs)
            {
                impl_->device->CopyDescriptorsSimple(1, destination, srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                destination.ptr += impl_->srv_stride;
            }
            const std::array versions{ray_shadow_outputs.metadata, ray_shadow_outputs.records,
                ray_shadow_outputs.visibility, ray_shadow_outputs.confidence, ray_shadow_outputs.depth, ray_shadow_outputs.normal};
            for (UINT index = 0; index < versions.size(); ++index)
                emit_feature_signal(outputs, FeatureId::RayShadows, shadow_signals[index], versions[index]);
            return Result::Success();
    }};
    if (ray_shadow_feature.predicate())
    {
        const auto noise = import_texture(impl_->vfx_stbn_scalar, Access::ShaderRead, "RayShadowBlueNoise");
        const auto desc = impl_->vfx_stbn_scalar.resource->GetDesc();
        const auto declaration = feature_declaration(SignalSemantic::BlueNoise, SignalEncoding::Float,
            RadiometricMeaning::None, 0, {static_cast<UINT>(desc.Width), desc.Height}, ResourceKind::Texture, desc.DepthOrArraySize);
        import_feature_signal(declaration, impl_->graph.Version(noise));
        ray_shadow_feature.inputs.push_back(require_guide(declaration));
    }
    visibility_features.push_back(std::move(ray_shadow_feature));
    if (auto composed = compose_features(std::move(visibility_features)); !composed) return composed;
    constants->lighting_options = {ambient_outputs.ao.resource ? 1.0f : 0.0f,
        ambient_outputs.contact.resource ? 1.0f : 0.0f,
        active_features.gi_mode != GraphicsGiMode::Off ? 1.0f : 0.0f,
        active_features.reflection_mode != GraphicsReflectionMode::Off ? 1.0f : 0.0f};
    auto ambient_descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    ambient_descriptor.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
        kViewDescriptorOffset + 11) * impl_->srv_stride;
    if (ambient_outputs.ao_srv.ptr)
        impl_->device->CopyDescriptorsSimple(1, ambient_descriptor, ambient_outputs.ao_srv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ambient_descriptor.ptr += impl_->srv_stride;
    if (ambient_outputs.contact_srv.ptr)
        impl_->device->CopyDescriptorsSimple(1, ambient_descriptor, ambient_outputs.contact_srv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto lighting_pass = impl_->graph.AddPass(5, kRenderPassNames[5], QueueHint::Direct);
    if (adaptive_outputs.raster_ready)
        lighting_pass.Read(adaptive_outputs.rate.version, Access::ShadingRateSource);
    read_probe(lighting_pass);
    if (ray_shadow_outputs.resources[0])
        for (const auto version : {ray_shadow_outputs.metadata, ray_shadow_outputs.records,
                ray_shadow_outputs.visibility, ray_shadow_outputs.confidence})
            lighting_pass.Read(version, Access::ShaderRead);
    if (ambient_outputs.ao.resource) lighting_pass.Read(ambient_outputs.ao, Access::ShaderRead);
    if (ambient_outputs.contact.resource) lighting_pass.Read(ambient_outputs.contact, Access::ShaderRead);
    lighting_pass.Read(gbuffer_base_version, Access::ShaderRead);
    if (decal_surface_outputs.active)
        lighting_pass.Read(decal_surface_outputs.emission.version, Access::ShaderRead);
    lighting_pass.Read(gbuffer_normal_version, Access::ShaderRead);
    lighting_pass.Read(gbuffer_metadata_version, Access::ShaderRead);
    lighting_pass.Read(gbuffer_material_version, Access::ShaderRead);
    lighting_pass.Read(depth_version, Access::ShaderRead);
    lighting_pass.Read(shadow_version, Access::ShaderRead);
    lighting_pass.Read(impl_->graph.Version(environment_brdf_lut), Access::ShaderRead);
    lighting_pass.Read(material_parameter_version, Access::ShaderRead);
    for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
        lighting_pass.Read(material_sidecar_versions[plane], Access::ShaderRead);
    hdr_version = lighting_pass.Write(hdr_version, Access::RenderTarget);
    subsurface_diffuse_version = lighting_pass.Write(subsurface_diffuse_version, Access::RenderTarget);
    environment_specular_version = lighting_pass.Write(environment_specular_version, Access::RenderTarget);
    indirect_diffuse_version = lighting_pass.Write(indirect_diffuse_version, Access::RenderTarget);
    const auto raw_hdr_version = hdr_version;
    lighting_pass.SetExecuteChecked([&](RenderPassContext &context) -> Result {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[] = {0, 0, 0, 0};
        const std::array targets{hdr_rtv, rtv_at(kLightingAuxRtvOffset), rtv_at(kLightingAuxRtvOffset + 1),
            rtv_at(kLightingAuxRtvOffset + 2)};
        for (const auto target : targets) list->ClearRenderTargetView(target, clear, 0, nullptr);
        list->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), FALSE, nullptr);
        list->SetPipelineState(impl_->deferred_pipeline.Get());
        list->SetGraphicsRootDescriptorTable(5, texture_table);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        AdaptiveShadingRasterScope rate_scope;
        if (auto bound = impl_->adaptive_shading.BeginRaster(context, adaptive_outputs, rate_scope); !bound)
            return bound;
        list->DrawInstanced(3, 1, 0, 0);
        return Result::Success();
    });
    BrixelizerLightingOutputs brixelizer_lighting;
    UINT brixelizer_bank{};
    const RayFrameTableBuilder make_brixelizer_bindings = [&](D3D12_CPU_DESCRIPTOR_HANDLE suffix,
            UINT count, RayShaderBinder &binder) -> Result {
        if (!suffix.ptr || count != kBrixelizerDescriptorCount || brixelizer_bank >= 4)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid software GI descriptor stage bank.");
        const auto offset = kBrixelizerFrameTableOffset + brixelizer_bank++ * kRayFrameTableCount;
        auto destination = local_probe_inputs.frame_srv_base; destination.ptr += offset * impl_->srv_stride;
        auto source = local_probe_inputs.frame_srv_base; source.ptr += kTemporalTextureDescriptorOffset * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(kRayFrameTableCount, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        destination.ptr += (kBrixelizerDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(count, destination, suffix, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto table = texture_table; table.ptr += UINT64(offset) * impl_->srv_stride;
        binder = [&, table](ID3D12GraphicsCommandList *list) -> Result {
            bind_surface_compute(list); list->SetComputeRootDescriptorTable(32, table); return Result::Success();
        };
        return Result::Success();
    };
    const bool use_ray_transport = ray_scene_outputs.tlas.resource &&
        (active_features.gi_mode == GraphicsGiMode::RayTraced ||
         active_features.reflection_mode == GraphicsReflectionMode::HybridRT);
    RayTransportInputs transport_inputs;
    RayTransportConstants transport_constants;
    RayDiffuseOutputs ray_diffuse_outputs;
    RayNoisySignal noisy_diffuse, noisy_specular;
    RayDlssGuides dlss_guides;
    RayReconstructionInputs rr_inputs;
    const auto rr_mode = impl_->ray_reconstruction.Mode();
    const auto make_rr_bindings = [&](std::uint32_t bank) -> RayReconstructionTableBuilder {
        return [&, bank](D3D12_CPU_DESCRIPTOR_HANDLE ray, D3D12_CPU_DESCRIPTOR_HANDLE rr, RayShaderBinder &binder) -> Result {
            if (!ray.ptr || !rr.ptr || bank >= 2)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid RR descriptor bank.");
            const auto offset = kRayReconstructionFrameTableOffset + bank * kRayFrameTableCount;
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount + offset) * impl_->srv_stride;
            auto source = local_probe_inputs.frame_srv_base; source.ptr += kTemporalTextureDescriptorOffset * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kRayFrameTableCount, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto target = destination; target.ptr += (kRayDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kRayDescriptorCount, target, ray, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            target = destination; target.ptr += (kRayTransportDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
            ray.ptr += kRayDescriptorCount * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kRayTransportDescriptorCount, target, ray, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            target = destination; target.ptr += (kRayReconstructionDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kRayReconstructionDescriptorCount, target, rr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto table = texture_table; table.ptr += UINT64(offset) * impl_->srv_stride;
            binder = [&, table](ID3D12GraphicsCommandList *commands) -> Result {
                bind_surface_compute(commands); commands->SetComputeRootDescriptorTable(32, table); return Result::Success();
            };
            return Result::Success();
        };
    };
    bool transport_configured{};
    const auto configure_transport_inputs = [&] {
        if (transport_configured) return;
        transport_inputs.scene = ray_scene_outputs;
        transport_inputs.camera = impl_->sdk_camera; transport_inputs.identity = impl_->sdk_frame_identity;
        transport_inputs.geometry = ray_geometry_outputs;
        transport_inputs.reads = ray_reads;
        transport_inputs.depth = ambient_inputs.depth;
        transport_inputs.normal = ambient_inputs.normal;
        transport_inputs.metadata = ambient_inputs.metadata;
        transport_inputs.motion = ambient_inputs.motion;
        transport_inputs.surface_history = ambient_inputs.surface_history;
        transport_inputs.base = {gbuffer_base_version, surface_base_resource};
        transport_inputs.material = {gbuffer_material_version, surface_material_resource};
        transport_inputs.hdr = {hdr_version, opaque_hdr_resource};
        transport_inputs.fallback_diffuse = {indirect_diffuse_version, impl_->indirect_diffuse.resource.Get()};
        transport_inputs.environment_specular = {environment_specular_version, impl_->environment_specular.resource.Get()};
        transport_inputs.subsurface_diffuse = {subsurface_diffuse_version, impl_->subsurface_diffuse.resource.Get()};
        transport_inputs.sidecar_plane_count = impl_->material_sidecar_layout.plane_count;
        for (std::uint32_t plane = 0; plane < transport_inputs.sidecar_plane_count; ++plane)
            transport_inputs.sidecars[plane] = {material_sidecar_versions[plane], impl_->material_sidecars[plane].resource.Get()};
        transport_inputs.ambient_visibility = {ambient_outputs.ao, ambient_outputs.ao_resource};
        transport_inputs.ambient_visibility_enabled = static_cast<bool>(ambient_outputs.ao.resource);
        transport_inputs.render_active = impl_->view_state.render_active;
        transport_inputs.frame_slot = back_buffer_index;
        transport_inputs.frame_index = impl_->frame_number;
        transport_inputs.completed = impl_->graph_queue_executor.Completed();
        transport_inputs.reset_history = !previous_view_valid || new_session;
        const auto mix_revision = [](std::uint64_t &hash, std::uint64_t value) {
            hash ^= value; hash *= 1099511628211ull;
        };
        auto &revisions = transport_inputs.revisions;
        revisions.scene = 14695981039346656037ull;
        for (const auto &source : canonical_instances)
        {
            mix_revision(revisions.scene, source.stable_id);
            mix_revision(revisions.scene, source.asset_id.value);
            mix_revision(revisions.scene, static_cast<std::uint64_t>(source.mesh));
            mix_revision(revisions.scene, source.revisions.content);
            mix_revision(revisions.scene, source.revisions.material);
        }
        if (impl_->static_gpu_scene) mix_revision(revisions.scene, impl_->static_gpu_scene->instance_generation);
        revisions.material = impl_->material_parameter_revision;
        revisions.light = 14695981039346656037ull;
        for (const auto &source : frame_lights)
        {
            mix_revision(revisions.light, source.stable_id);
            mix_revision(revisions.light, source.generation);
            mix_revision(revisions.light, source.revision);
        }
        const auto probe_digest = impl_->config.product_digests.find("render/lighting/default_probe.rpkg");
        if (probe_digest != impl_->config.product_digests.end())
            revisions.probe = MakeAssetId(probe_digest->second).value;
        transport_constants.inverse_view_projection = ambient_constants.inverse_view_projection;
        transport_constants.world_to_view = ambient_constants.world_to_view;
        transport_constants.projection = ambient_constants.projection;
        transport_constants.previous_depth_parameters = ambient_constants.previous_depth_parameters;
        transport_constants.jitter_uv = ambient_constants.jitter_uv;
        transport_constants.world_origin = {lod_eye.x, lod_eye.y, lod_eye.z, 0};
        transport_constants.gi_parameters.x = impl_->config.rt_trace_distance_m;
        transport_constants.specular_parameters.x = impl_->config.rt_trace_distance_m;
        transport_inputs.make_bindings = make_ray_bindings(kRayGiTableOffset);
        rr_inputs.transport = transport_inputs; rr_inputs.make_bindings = make_rr_bindings(0);
        rr_inputs.exposure = {exposure_read_version, impl_->exposure_values[exposure_read_index].resource.Get(), {}};
        rr_inputs.owners.push_back(impl_->resident_generation_lease);
        transport_configured = true;
    };
    const auto hdr_signal = feature_declaration(SignalSemantic::OpaqueHdr, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear);
    const auto fallback_signal = feature_declaration(SignalSemantic::FallbackDiffuse, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear);
    const auto environment_signal = feature_declaration(SignalSemantic::EnvironmentSpecular, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear);
    const auto subsurface_signal = feature_declaration(SignalSemantic::SubsurfaceDiffuse, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear);
    import_feature_signal(hdr_signal, hdr_version);
    import_feature_signal(fallback_signal, indirect_diffuse_version);
    import_feature_signal(environment_signal, environment_specular_version);
    import_feature_signal(subsurface_signal, subsurface_diffuse_version);
    std::vector<SignalDeclaration> transport_requirements = ambient_requirements;
    for (const auto declaration : {base_signal, material_signal, hdr_signal, fallback_signal, environment_signal, subsurface_signal})
        transport_requirements.push_back(require_guide(declaration));
    for (UINT plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
    {
        const auto declaration = feature_declaration(SignalSemantic::MaterialSidecar, SignalEncoding::Metadata, RadiometricMeaning::None, plane);
        import_feature_signal(declaration, material_sidecar_versions[plane]);
        transport_requirements.push_back(require_guide(declaration));
    }
    if (ambient_outputs.ao.resource) transport_requirements.push_back(require_guide(ambient_signal));
    const auto resolve_transport_guides = [&](std::span<const SignalHandle> signals, RayTransportInputs &inputs,
            UINT hdr_stage = 0, UINT environment_stage = 0) {
        inputs.depth = feature_guide(signals, SignalSemantic::Depth);
        inputs.normal = feature_guide(signals, SignalSemantic::Normal);
        inputs.metadata = feature_guide(signals, SignalSemantic::SurfaceMetadata);
        inputs.motion = feature_guide(signals, SignalSemantic::Motion);
        inputs.surface_history = feature_guide(signals, SignalSemantic::SurfaceHistory);
        inputs.base = feature_guide(signals, SignalSemantic::BaseColor);
        inputs.material = feature_guide(signals, SignalSemantic::Material);
        inputs.hdr = feature_guide(signals, SignalSemantic::OpaqueHdr, hdr_stage);
        inputs.fallback_diffuse = feature_guide(signals, SignalSemantic::FallbackDiffuse);
        inputs.environment_specular = feature_guide(signals, SignalSemantic::EnvironmentSpecular, environment_stage);
        inputs.subsurface_diffuse = feature_guide(signals, SignalSemantic::SubsurfaceDiffuse);
        inputs.ambient_visibility = feature_guide(signals, SignalSemantic::AmbientVisibility);
        for (UINT plane = 0; plane < inputs.sidecar_plane_count; ++plane)
            inputs.sidecars[plane] = feature_guide(signals, SignalSemantic::MaterialSidecar, plane);
        resolve_ray_dependencies(signals, inputs.scene, inputs.geometry);
    };
    auto diffuse_signal = feature_declaration(SignalSemantic::DiffuseRadiance, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 1);
    diffuse_signal.metadata.filtering = rr_mode == RayReconstructionMode::DlssFused ? SignalFiltering::Noisy : SignalFiltering::Resolved;
    const auto diffuse_delta_signal = feature_declaration(SignalSemantic::DiffuseDelta, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 1);
    const auto adjusted_subsurface_signal = feature_declaration(SignalSemantic::AdjustedSubsurfaceDiffuse, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 1);
    auto software_specular_signal = feature_declaration(SignalSemantic::EnvironmentSpecular, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 1);
    software_specular_signal.metadata.filtering = SignalFiltering::Resolved;
    const auto noisy_extent = [&](GraphicsQuality quality) {
        const auto axis = rr_mode == RayReconstructionMode::DlssFused ? 1.0f : impl_->QualityProfileFor(quality).rt_axis_ratio;
        return SignalExtent{static_cast<UINT>(std::ceil(feature_extent.width * axis)),
            static_cast<UINT>(std::ceil(feature_extent.height * axis))};
    };
    auto noisy_diffuse_signal = feature_declaration(SignalSemantic::NoisyDiffuseRadiance, SignalEncoding::RgbaFloat,
        RadiometricMeaning::SceneLinear, 1, noisy_extent(impl_->config.gi_quality));
    noisy_diffuse_signal.metadata.filtering = SignalFiltering::Noisy;
    const auto diffuse_statistics_signal = feature_declaration(SignalSemantic::DiffuseStatistics, SignalEncoding::RgbaFloat,
        RadiometricMeaning::None, 1, noisy_diffuse_signal.metadata.extent);
    const auto diffuse_distance_signal = feature_declaration(SignalSemantic::DiffuseHitDistance, SignalEncoding::Float2,
        RadiometricMeaning::None, 1, noisy_diffuse_signal.metadata.extent);
    const auto emit_diffuse = [&](std::vector<SignalHandle> &outputs, FeatureId producer) {
        emit_feature_signal(outputs, producer, diffuse_signal, ray_diffuse_outputs.diffuse.version);
        emit_feature_signal(outputs, producer, diffuse_delta_signal, ray_diffuse_outputs.delta.version);
        emit_feature_signal(outputs, producer, adjusted_subsurface_signal, ray_diffuse_outputs.adjusted_subsurface.version);
    };
    struct SoftwareGiBackingRead { ResourceVersion version; Access access; };
    std::vector<SoftwareGiBackingRead> software_gi_backing;
    std::vector<SignalDeclaration> software_gi_backing_requirements;
    SignalDeclaration environment_cube_signal;
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
        for (const auto version : {probe_outputs.radiance, probe_outputs.prefiltered, probe_outputs.irradiance,
                light_outputs.lights, light_outputs.ranges, light_outputs.inline_indices, light_outputs.spill_indices,
                ltc_outputs.matrix, ltc_outputs.amplitude}) append(version, Access::ShaderRead);
        for (UINT index = 0; index < software_gi_backing.size(); ++index)
        {
            const auto version = software_gi_backing[index].version;
            auto *resource = static_cast<ID3D12Resource *>(impl_->graph.Resolve(version.resource));
            const auto desc = resource->GetDesc();
            const auto declaration = feature_declaration(SignalSemantic::SoftwareGiLightingBacking, SignalEncoding::RawBytes,
                RadiometricMeaning::None, index, {static_cast<UINT>(std::min<UINT64>(desc.Width, UINT32_MAX)), desc.Height},
                version.resource.kind, version.resource.kind == ResourceKind::Texture ? desc.DepthOrArraySize : 1);
            import_feature_signal(declaration, version); software_gi_backing_requirements.push_back(declaration);
        }
        const auto cube = probe_outputs.radiance_resource->GetDesc();
        environment_cube_signal = feature_declaration(SignalSemantic::EnvironmentCube, SignalEncoding::RgbaFloat,
            RadiometricMeaning::SceneLinear, 0, {static_cast<UINT>(cube.Width), cube.Height}, ResourceKind::Texture, cube.DepthOrArraySize);
        import_feature_signal(environment_cube_signal, probe_outputs.radiance);
    }
    std::vector<std::array<UINT, 2>> software_gi_geometry_signals(brixelizer_geometries.size());
    std::vector<FeatureRegistration> diffuse_features;
    FeatureRegistration software_gi{FeatureId::SoftwareGi, [&] { return active_features.software_gi_enabled; },
        transport_requirements, {diffuse_signal, diffuse_delta_signal, adjusted_subsurface_signal, software_specular_signal},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            for (UINT index = 0; index < ray_reads.size(); ++index)
                ray_reads[index].version = feature_resource(signals, SignalSemantic::ClosureBacking, index);
            for (UINT index = 0; index < brixelizer_geometries.size(); ++index)
            {
                brixelizer_geometries[index].vertex_version = feature_resource(signals, SignalSemantic::SoftwareGiVertices,
                    software_gi_geometry_signals[index][0]);
                brixelizer_geometries[index].index_version = feature_resource(signals, SignalSemantic::SoftwareGiIndices,
                    software_gi_geometry_signals[index][1]);
            }
            if (auto registered = register_software_gi_scene(); !registered) return registered;
            std::vector<RayShaderRead> cache_reads;
            const auto append_cache_read = [&](AmbientOcclusionGuide guide, Access access = Access::ShaderRead) {
                const auto existing = std::ranges::find_if(cache_reads, [&](const auto &read) {
                    return read.version.resource.value == guide.version.resource.value &&
                        read.version.resource.kind == guide.version.resource.kind &&
                        read.version.ordinal == guide.version.ordinal;
                });
                if (existing == cache_reads.end()) cache_reads.push_back({guide.version, guide.resource, access});
                else if (existing->access != access) existing->access = Access::AllShaderRead;
            };
            for (const auto semantic : {SignalSemantic::BaseColor, SignalSemantic::Normal,
                    SignalSemantic::SurfaceMetadata, SignalSemantic::Material, SignalSemantic::Depth,
                    // This dependency places the seed after the real DeferredLight pass.
                    SignalSemantic::FallbackDiffuse})
                append_cache_read(feature_guide(signals, semantic));
            for (UINT index = 0; index < software_gi_backing.size(); ++index)
                append_cache_read(feature_guide(signals,SignalSemantic::SoftwareGiLightingBacking,index),
                    software_gi_backing[index].access);
            for (const auto &read : ray_reads)
                // This pass evaluates the shared closure in the pixel shader.
                append_cache_read({read.version,read.resource},
                    read.access == Access::ComputeRead ? Access::AllShaderRead : read.access);
            for (UINT plane=0;plane<impl_->material_sidecar_layout.plane_count;++plane)
                append_cache_read(feature_guide(signals,SignalSemantic::MaterialSidecar,plane));
            if (ambient_outputs.ao.resource) append_cache_read(feature_guide(signals,SignalSemantic::AmbientVisibility));
            if (ambient_outputs.contact.resource) append_cache_read(feature_guide(signals,SignalSemantic::ContactVisibility));
            if (ray_shadow_outputs.resources[0])
                for (const auto declaration : shadow_signals)
                    append_cache_read(feature_guide(signals,declaration.key.semantic,declaration.key.version));
            BrixelizerCacheInjectionInputs cache;
            cache.reads=cache_reads;
            cache.bind_graphics=[&](ID3D12GraphicsCommandList *list) -> Result {
                bind_graphics(list); list->SetGraphicsRootDescriptorTable(5,texture_table); return Result::Success();
            };
            cache.render_active=impl_->view_state.render_active; cache.frame_slot=back_buffer_index;
            cache.owners=brixelizer_owners;
            AmbientOcclusionGuide cache_injection;
            if (auto registered=impl_->brixelizer_gi.RegisterCacheInjection(impl_->graph,cache,cache_injection);!registered) return registered;
            BrixelizerLightingInputs native;
            native.depth=feature_guide(signals,SignalSemantic::Depth); native.normal=feature_guide(signals,SignalSemantic::Normal);
            native.metadata=feature_guide(signals,SignalSemantic::SurfaceMetadata); native.motion=feature_guide(signals,SignalSemantic::Motion);
            native.base=feature_guide(signals,SignalSemantic::BaseColor); native.material=feature_guide(signals,SignalSemantic::Material);
            native.sidecar_plane_count=impl_->material_sidecar_layout.plane_count;
            for(UINT plane=0;plane<native.sidecar_plane_count;++plane)
                native.sidecars[plane]=feature_guide(signals,SignalSemantic::MaterialSidecar,plane);
            native.cache_injection=cache_injection; native.fallback_diffuse=feature_guide(signals,SignalSemantic::FallbackDiffuse);
            native.environment_specular=feature_guide(signals,SignalSemantic::EnvironmentSpecular);
            native.subsurface_diffuse=feature_guide(signals,SignalSemantic::SubsurfaceDiffuse);
            native.environment_cube=feature_guide(signals,SignalSemantic::EnvironmentCube);
            native.reads=ray_reads; native.make_bindings=make_brixelizer_bindings;
            native.restore_bindings=[&](ID3D12GraphicsCommandList *list) -> Result {bind_surface_compute(list);return Result::Success();};
            native.render_active=impl_->view_state.render_active; native.frame_slot=back_buffer_index;
            native.frame_index=impl_->frame_number; native.light_revision=brixelizer_light_epoch;
            native.completed=impl_->graph_queue_executor.Completed(); native.owners=brixelizer_owners;
            native.camera=impl_->sdk_camera;
            DirectX::XMStoreFloat4x4(&native.camera.projection,projection*DirectX::XMMatrixTranslation(jitter_x,jitter_y,0));
            DirectX::XMStoreFloat4x4(&native.camera.inverse_projection,DirectX::XMMatrixInverse(nullptr,DirectX::XMLoadFloat4x4(&native.camera.projection)));
            native.camera.previous_view=impl_->brixelizer_history_valid ? previous_sdk_camera.view : native.camera.view;
            const auto previous_native_projection=impl_->brixelizer_history_valid
                ? DirectX::XMLoadFloat4x4(&previous_sdk_camera.projection) * DirectX::XMMatrixTranslation(
                    2.0f*constants->surface_jitter_uv.z,-2.0f*constants->surface_jitter_uv.w,0)
                : DirectX::XMLoadFloat4x4(&native.camera.projection);
            DirectX::XMStoreFloat4x4(&native.camera.previous_projection,previous_native_projection);
            native.camera.reset_history=false; // native history was recreated only after a proved drain
            BrixelizerConstants native_constants;
            native_constants.inverse_view_projection=camera_relative_inverse_view_projection;
            native_constants.jitter_uv=constants->surface_jitter_uv;
            native_constants.world_origin={lod_eye.x,lod_eye.y,lod_eye.z,0};
            if(auto registered=impl_->brixelizer_gi.RegisterLighting(impl_->graph,native,native_constants,brixelizer_lighting);!registered) return registered;
            emit_feature_signal(outputs, FeatureId::SoftwareGi, diffuse_signal, brixelizer_lighting.diffuse.version);
            emit_feature_signal(outputs, FeatureId::SoftwareGi, diffuse_delta_signal, brixelizer_lighting.delta.version);
            emit_feature_signal(outputs, FeatureId::SoftwareGi, adjusted_subsurface_signal, brixelizer_lighting.adjusted_subsurface.version);
            emit_feature_signal(outputs, FeatureId::SoftwareGi, software_specular_signal, brixelizer_lighting.specular.version);
            return Result::Success();
    }};
    software_gi.inputs.insert(software_gi.inputs.end(), closure_requirements.begin(), closure_requirements.end());
    software_gi.inputs.insert(software_gi.inputs.end(), software_gi_backing_requirements.begin(), software_gi_backing_requirements.end());
    if (software_gi.predicate()) software_gi.inputs.push_back(require_guide(environment_cube_signal));
    if (ambient_outputs.contact.resource) software_gi.inputs.push_back(require_guide(contact_signal));
    if (ray_shadow_outputs.resources[0]) software_gi.inputs.insert(software_gi.inputs.end(), shadow_signals.begin(), shadow_signals.end());
    if (software_gi.predicate())
    {
        // A native submesh range retains its geometry identity, while graph
        // dependencies refer to the shared physical buffer and exact version.
        using BufferSignalKey = std::tuple<std::uint32_t, ResourceKind, std::uint32_t>;
        std::array<std::map<BufferSignalKey, UINT>, 2> buffer_signals;
        for (UINT index = 0; index < brixelizer_geometries.size(); ++index)
            for (UINT attribute = 0; attribute < 2; ++attribute)
            {
                const auto semantic = attribute == 0 ? SignalSemantic::SoftwareGiVertices : SignalSemantic::SoftwareGiIndices;
                const auto version = attribute == 0 ? brixelizer_geometries[index].vertex_version : brixelizer_geometries[index].index_version;
                auto &lookup = buffer_signals[attribute];
                const auto [entry, inserted] = lookup.try_emplace(
                    BufferSignalKey{version.resource.value, version.resource.kind, version.ordinal},
                    static_cast<UINT>(lookup.size()));
                software_gi_geometry_signals[index][attribute] = entry->second;
                if (!inserted) continue;
                auto *resource = static_cast<ID3D12Resource *>(impl_->graph.Resolve(version.resource));
                const auto declaration = feature_declaration(semantic, SignalEncoding::RawBytes, RadiometricMeaning::None, entry->second,
                    {static_cast<UINT>(std::min<UINT64>(resource->GetDesc().Width, UINT32_MAX)), 1}, ResourceKind::Buffer);
                import_feature_signal(declaration, version); software_gi.inputs.push_back(declaration);
            }
    }
    diffuse_features.push_back(std::move(software_gi));
    FeatureRegistration diffuse_transport{FeatureId::DiffuseTransport,
        [&] { return use_ray_transport && active_features.gi_mode == GraphicsGiMode::RayTraced; },
        with_ray_requirements(transport_requirements), {},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            configure_transport_inputs();
            resolve_transport_guides(signals, transport_inputs);
            pending_effects.ray_transport = true;
            if (rr_mode == RayReconstructionMode::Off)
            {
                if (auto registered = impl_->ray_transport.RegisterDiffuse(impl_->graph, transport_inputs,
                        transport_constants, ray_diffuse_outputs); !registered) return registered;
                emit_diffuse(outputs, FeatureId::DiffuseTransport);
            }
            else
            {
                if (auto registered = impl_->ray_transport.RegisterNoisyDiffuse(impl_->graph, transport_inputs,
                        transport_constants, noisy_diffuse, &ray_diffuse_outputs); !registered) return registered;
                rr_inputs.transport = transport_inputs;
                if (noisy_diffuse.owner) rr_inputs.owners.push_back(noisy_diffuse.owner);
                emit_feature_signal(outputs, FeatureId::DiffuseTransport, noisy_diffuse_signal, noisy_diffuse.color.version);
                emit_feature_signal(outputs, FeatureId::DiffuseTransport, diffuse_statistics_signal, noisy_diffuse.statistics.version);
                emit_feature_signal(outputs, FeatureId::DiffuseTransport, diffuse_distance_signal, noisy_diffuse.metric_count.version);
                if (rr_mode == RayReconstructionMode::DlssFused) emit_diffuse(outputs, FeatureId::DiffuseTransport);
            }
            return Result::Success();
        }};
    if (rr_mode != RayReconstructionMode::Off)
        diffuse_transport.outputs = {noisy_diffuse_signal, diffuse_statistics_signal, diffuse_distance_signal};
    if (rr_mode != RayReconstructionMode::FsrStandalone)
        for (const auto declaration : {diffuse_signal, diffuse_delta_signal, adjusted_subsurface_signal})
            diffuse_transport.outputs.push_back(declaration);
    diffuse_features.push_back(std::move(diffuse_transport));
    auto diffuse_denoising_requirements = with_ray_requirements(transport_requirements);
    for (const auto declaration : {noisy_diffuse_signal, diffuse_statistics_signal, diffuse_distance_signal})
        diffuse_denoising_requirements.push_back(declaration);
    diffuse_features.push_back({FeatureId::DiffuseDenoising,
        [&] { return use_ray_transport && active_features.gi_mode == GraphicsGiMode::RayTraced && rr_mode == RayReconstructionMode::FsrStandalone; },
        std::move(diffuse_denoising_requirements), {diffuse_signal, diffuse_delta_signal, adjusted_subsurface_signal},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            resolve_transport_guides(signals, rr_inputs.transport);
            noisy_diffuse.color.version = feature_resource(signals, SignalSemantic::NoisyDiffuseRadiance, 1);
            noisy_diffuse.statistics.version = feature_resource(signals, SignalSemantic::DiffuseStatistics, 1);
            noisy_diffuse.metric_count.version = feature_resource(signals, SignalSemantic::DiffuseHitDistance, 1);
            pending_effects.ray_reconstruction = true;
            if (auto registered = impl_->ray_reconstruction.RegisterDiffuse(impl_->graph, rr_inputs,
                    noisy_diffuse, transport_constants, ray_diffuse_outputs); !registered) return registered;
            emit_diffuse(outputs, FeatureId::DiffuseDenoising);
            return Result::Success();
        }});
    if (auto composed = compose_features(std::move(diffuse_features)); !composed) return composed;
    if (brixelizer_lighting.diffuse.resource)
    {
        ray_diffuse_outputs.diffuse = {brixelizer_lighting.diffuse.version,brixelizer_lighting.diffuse.resource,brixelizer_lighting.diffuse.srv};
        ray_diffuse_outputs.adjusted_subsurface = {brixelizer_lighting.adjusted_subsurface.version,
            brixelizer_lighting.adjusted_subsurface.resource,brixelizer_lighting.adjusted_subsurface.srv};
        transport_inputs.fallback_diffuse={brixelizer_lighting.diffuse.version,brixelizer_lighting.diffuse.resource};
        transport_inputs.environment_specular={brixelizer_lighting.specular.version,brixelizer_lighting.specular.resource};
    }
    SubsurfaceConstants diffusion_constants;
    diffusion_constants.inverse_view_projection = camera_relative_inverse_view_projection;
    diffusion_constants.quality_options.x = impl_->QualityProfileFor(impl_->config.material_quality).subsurface_half_samples;
    std::copy(constants->material_parameter_bases.begin(), constants->material_parameter_bases.end(),
              diffusion_constants.material_parameter_bases);
    std::copy(constants->hero_material_parameter_bases.begin(), constants->hero_material_parameter_bases.end(),
              diffusion_constants.hero_material_parameter_bases);
    const SubsurfaceInputs diffusion_inputs{
        {depth_version, impl_->depth.resource.Get()},
        {gbuffer_normal_version, surface_normal_resource},
        {gbuffer_metadata_version, impl_->gbuffer_metadata.resource.Get()},
        ray_diffuse_outputs.adjusted_subsurface.resource
            ? SubsurfaceGuide{ray_diffuse_outputs.adjusted_subsurface.version, ray_diffuse_outputs.adjusted_subsurface.resource}
            : SubsurfaceGuide{subsurface_diffuse_version, impl_->subsurface_diffuse.resource.Get()},
        {hdr_version, opaque_hdr_resource}, {material_parameter_version, impl_->material_parameters.resource.Get()},
        impl_->view_state.render_active, back_buffer_index, impl_->graph_queue_executor.Completed()};
    SubsurfaceOutputs diffusion_outputs;
    if (auto registered = impl_->subsurface.Register(impl_->graph, diffusion_inputs,
            diffusion_constants, diffusion_outputs); !registered) return registered;
    pending_effects.diffusion = static_cast<bool>(diffusion_outputs.corrected.resource);
    if (diffusion_outputs.corrected.resource)
    {
        hdr_version = diffusion_outputs.corrected;
        opaque_hdr_resource = diffusion_outputs.corrected_resource;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE opaque_hdr_srv = diffusion_outputs.corrected_srv;
    const auto subsurface_hdr_signal = feature_declaration(SignalSemantic::OpaqueHdr, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 1);
    const auto software_hdr_signal = feature_declaration(SignalSemantic::OpaqueHdr, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 2);
    import_feature_signal(subsurface_hdr_signal, hdr_version);
    if (auto composed = compose_features({FeatureRegistration{FeatureId::SoftwareGiComposite,
        [&] { return active_features.software_gi_enabled; },
        {subsurface_hdr_signal, diffuse_delta_signal, software_specular_signal, environment_signal}, {software_hdr_signal},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            BrixelizerCompositeInputs native;
            native.hdr=feature_guide(signals,SignalSemantic::OpaqueHdr,1);native.make_bindings=make_brixelizer_bindings;
            native.render_active=impl_->view_state.render_active;native.frame_slot=back_buffer_index;native.owners=brixelizer_owners;
            // Delta/environment are the exact versions already retained by the
            // service's lighting registration, validated here across batches.
            BrixelizerTexture composed;
            if(auto registered=impl_->brixelizer_gi.RegisterComposite(impl_->graph,native,composed);!registered) return registered;
            hdr_version=composed.version;opaque_hdr_resource=composed.resource;opaque_hdr_srv=composed.srv;
            emit_feature_signal(outputs,FeatureId::SoftwareGiComposite,software_hdr_signal,composed.version);
            return Result::Success();
        }}}); !composed) return composed;
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
    reflection_inputs.depth = {depth_version, impl_->depth.resource.Get()};
    reflection_inputs.base = {gbuffer_base_version, surface_base_resource};
    reflection_inputs.normal = {gbuffer_normal_version, surface_normal_resource};
    reflection_inputs.metadata = {gbuffer_metadata_version, impl_->gbuffer_metadata.resource.Get()};
    reflection_inputs.material = {gbuffer_material_version, surface_material_resource};
    reflection_inputs.motion = {gbuffer_motion_version, impl_->gbuffer_motion.resource.Get()};
    reflection_inputs.surface_history = {gbuffer_surface_history_version, impl_->gbuffer_surface_history.resource.Get()};
    reflection_inputs.opaque_hdr = {hdr_version, opaque_hdr_resource};
    reflection_inputs.environment_specular = brixelizer_lighting.specular.resource
        ? ScreenReflectionResource{brixelizer_lighting.specular.version,brixelizer_lighting.specular.resource}
        : ScreenReflectionResource{environment_specular_version,impl_->environment_specular.resource.Get()};
    reflection_inputs.material_parameters = {material_parameter_version, impl_->material_parameters.resource.Get()};
    reflection_inputs.sidecar_plane_count = impl_->material_sidecar_layout.plane_count;
    for (std::uint32_t plane = 0; plane < reflection_inputs.sidecar_plane_count; ++plane)
        reflection_inputs.sidecars[plane] = {material_sidecar_versions[plane], impl_->material_sidecars[plane].resource.Get()};
    reflection_inputs.frame_constants_address = frame.upload.resource->GetGPUVirtualAddress();
    reflection_inputs.render_active = impl_->view_state.render_active;
    reflection_inputs.frame_slot = back_buffer_index;
    reflection_inputs.frame_index = impl_->frame_number;
    reflection_inputs.completed = impl_->graph_queue_executor.Completed();
    reflection_inputs.reset_history = !previous_view_valid || new_session;
    reflection_inputs.ambient_visibility = {ambient_outputs.ao, ambient_outputs.ao_resource};
    reflection_inputs.ambient_visibility_enabled = static_cast<bool>(ambient_outputs.ao.resource);
    ScreenReflectionsOutputs reflection_outputs;
    if (auto registered = impl_->screen_reflections.Register(impl_->graph, reflection_inputs,
            reflection_constants, reflection_outputs); !registered) return registered;
    pending_effects.reflections = static_cast<bool>(reflection_outputs.composed.resource);
    if (reflection_outputs.composed.resource)
    {
        hdr_version = reflection_outputs.composed;
        opaque_hdr_resource = reflection_outputs.composed_resource;
        opaque_hdr_srv = reflection_outputs.composed_srv;
    }
    const auto reflected_hdr_signal = feature_declaration(SignalSemantic::OpaqueHdr, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 3);
    const auto transported_hdr_signal = feature_declaration(SignalSemantic::OpaqueHdr, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 4);
    auto specular_signal = feature_declaration(SignalSemantic::SpecularRadiance, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 1);
    specular_signal.metadata.filtering = rr_mode == RayReconstructionMode::DlssFused ? SignalFiltering::Noisy : SignalFiltering::Resolved;
    auto noisy_specular_signal = feature_declaration(SignalSemantic::NoisySpecularRadiance, SignalEncoding::RgbaFloat,
        RadiometricMeaning::SceneLinear, 1, noisy_extent(impl_->config.reflection_quality));
    noisy_specular_signal.metadata.filtering = SignalFiltering::Noisy;
    const auto specular_statistics_signal = feature_declaration(SignalSemantic::SpecularStatistics, SignalEncoding::RgbaFloat,
        RadiometricMeaning::None, 1, noisy_specular_signal.metadata.extent);
    const auto specular_distance_signal = feature_declaration(SignalSemantic::SpecularHitDistance, SignalEncoding::Float2,
        RadiometricMeaning::None, 1, noisy_specular_signal.metadata.extent);
    import_feature_signal(reflected_hdr_signal, hdr_version);
    auto specular_requirements = transport_requirements;
    for (auto &required : specular_requirements)
    {
        if (required.key == hdr_signal.key) required = require_guide(reflected_hdr_signal);
        if (active_features.software_gi_enabled && required.key == environment_signal.key) required = require_guide(software_specular_signal);
        if (active_features.software_gi_enabled && required.key == fallback_signal.key) required = require_guide(diffuse_signal);
    }
    if (active_features.gi_mode == GraphicsGiMode::RayTraced)
        specular_requirements.push_back(diffuse_signal);
    const auto resolve_specular_guides = [&](std::span<const SignalHandle> signals, RayTransportInputs &inputs) {
        resolve_transport_guides(signals, inputs, 3, active_features.software_gi_enabled ? 1 : 0);
        if (active_features.software_gi_enabled) inputs.fallback_diffuse = feature_guide(signals, SignalSemantic::DiffuseRadiance, 1);
        if (active_features.gi_mode == GraphicsGiMode::RayTraced || active_features.software_gi_enabled)
            ray_diffuse_outputs.diffuse.version = feature_resource(signals, SignalSemantic::DiffuseRadiance, 1);
    };
    RaySpecularOutputs ray_specular_outputs;
    std::vector<FeatureRegistration> specular_features;
    FeatureRegistration specular_transport{FeatureId::SpecularTransport,
        [&] { return use_ray_transport && active_features.reflection_mode == GraphicsReflectionMode::HybridRT; },
        with_ray_requirements(specular_requirements), {},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            configure_transport_inputs();
            resolve_specular_guides(signals, transport_inputs);
            transport_inputs.make_bindings = make_ray_bindings(kRaySpecularTableOffset);
            pending_effects.ray_transport = true;
            if (rr_mode == RayReconstructionMode::Off)
            {
                if (auto registered = impl_->ray_transport.RegisterSpecular(impl_->graph, transport_inputs,
                        ray_diffuse_outputs, transport_constants, ray_specular_outputs); !registered) return registered;
                emit_feature_signal(outputs, FeatureId::SpecularTransport, specular_signal, ray_specular_outputs.specular.version);
            }
            else
            {
                if (auto registered = impl_->ray_transport.RegisterNoisySpecular(impl_->graph, transport_inputs,
                        ray_diffuse_outputs, transport_constants, noisy_specular); !registered) return registered;
                rr_inputs.transport = transport_inputs;
                if (noisy_specular.owner) rr_inputs.owners.push_back(noisy_specular.owner);
                emit_feature_signal(outputs, FeatureId::SpecularTransport, noisy_specular_signal, noisy_specular.color.version);
                emit_feature_signal(outputs, FeatureId::SpecularTransport, specular_statistics_signal, noisy_specular.statistics.version);
                emit_feature_signal(outputs, FeatureId::SpecularTransport, specular_distance_signal, noisy_specular.metric_count.version);
                if (rr_mode == RayReconstructionMode::DlssFused)
                {
                    ray_specular_outputs = {noisy_specular.color, noisy_specular.color, noisy_specular.statistics};
                    emit_feature_signal(outputs, FeatureId::SpecularTransport, specular_signal, ray_specular_outputs.specular.version);
                }
            }
            return Result::Success();
        }};
    if (rr_mode != RayReconstructionMode::Off)
        specular_transport.outputs = {noisy_specular_signal, specular_statistics_signal, specular_distance_signal};
    if (rr_mode != RayReconstructionMode::FsrStandalone) specular_transport.outputs.push_back(specular_signal);
    specular_features.push_back(std::move(specular_transport));
    auto specular_denoising_requirements = with_ray_requirements(specular_requirements);
    for (const auto declaration : {noisy_specular_signal, specular_statistics_signal, specular_distance_signal})
        specular_denoising_requirements.push_back(declaration);
    specular_features.push_back({FeatureId::SpecularDenoising,
        [&] { return use_ray_transport && active_features.reflection_mode == GraphicsReflectionMode::HybridRT && rr_mode == RayReconstructionMode::FsrStandalone; },
        std::move(specular_denoising_requirements), {specular_signal},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            resolve_specular_guides(signals, rr_inputs.transport);
            noisy_specular.color.version = feature_resource(signals, SignalSemantic::NoisySpecularRadiance, 1);
            noisy_specular.statistics.version = feature_resource(signals, SignalSemantic::SpecularStatistics, 1);
            noisy_specular.metric_count.version = feature_resource(signals, SignalSemantic::SpecularHitDistance, 1);
            rr_inputs.make_bindings = make_rr_bindings(1); pending_effects.ray_reconstruction = true;
            if (auto registered = impl_->ray_reconstruction.RegisterSpecular(impl_->graph, rr_inputs,
                    noisy_specular, transport_constants, ray_specular_outputs); !registered) return registered;
            emit_feature_signal(outputs, FeatureId::SpecularDenoising, specular_signal, ray_specular_outputs.specular.version);
            return Result::Success();
        }});
    auto composite_requirements = with_ray_requirements(specular_requirements);
    if (active_features.gi_mode == GraphicsGiMode::RayTraced)
        for (const auto declaration : {diffuse_delta_signal, adjusted_subsurface_signal}) composite_requirements.push_back(declaration);
    if (active_features.reflection_mode == GraphicsReflectionMode::HybridRT) composite_requirements.push_back(specular_signal);
    specular_features.push_back({FeatureId::RayTransportComposite, [&] { return use_ray_transport; },
        std::move(composite_requirements), {transported_hdr_signal},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            resolve_specular_guides(signals, transport_inputs);
            if (active_features.gi_mode == GraphicsGiMode::RayTraced)
            {
                ray_diffuse_outputs.delta.version = feature_resource(signals, SignalSemantic::DiffuseDelta, 1);
                ray_diffuse_outputs.adjusted_subsurface.version = feature_resource(signals, SignalSemantic::AdjustedSubsurfaceDiffuse, 1);
            }
            if (active_features.reflection_mode == GraphicsReflectionMode::HybridRT)
                ray_specular_outputs.specular.version = feature_resource(signals, SignalSemantic::SpecularRadiance, 1);
            transport_inputs.make_bindings = make_ray_bindings(kRayCompositeTableOffset);
            RayTransportOutputs composed;
            if (auto registered = impl_->ray_transport.RegisterComposite(impl_->graph, transport_inputs,
                    ray_diffuse_outputs, ray_specular_outputs, transport_constants, composed); !registered) return registered;
            hdr_version = composed.composed.version; opaque_hdr_resource = composed.composed.resource; opaque_hdr_srv = composed.composed.srv;
            emit_feature_signal(outputs, FeatureId::RayTransportComposite, transported_hdr_signal, hdr_version);
            return Result::Success();
        }});
    if (auto composed = compose_features(std::move(specular_features)); !composed) return composed;
    VolumetricOutputs volumetric_outputs;
    VolumetricCamera volumetric_camera;
    const auto volumetric_raw_hdr = hdr_version;
    if (impl_->config.volumetric_quality != GraphicsOptionalQuality::Off)
    {
        volumetric_camera.inverse_view_projection = camera_relative_inverse_view_projection;
        DirectX::XMStoreFloat4x4(&volumetric_camera.view_projection, DirectX::XMMatrixInverse(nullptr,
            DirectX::XMLoadFloat4x4(&volumetric_camera.inverse_view_projection)));
        DirectX::XMStoreFloat4x4(&volumetric_camera.world_to_view, DirectX::XMMatrixTranslationFromVector(eye) * view);
        volumetric_camera.origin = lod_eye; volumetric_camera.near_plane = near_plane; volumetric_camera.far_plane = far_plane;
        VolumetricInputs inputs;
        inputs.depth = {depth_version, impl_->depth.resource.Get()}; inputs.hdr = {hdr_version, opaque_hdr_resource};
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
        inputs.previous_camera = impl_->previous_volumetric_camera.near_plane > 0 ? impl_->previous_volumetric_camera : volumetric_camera;
        inputs.render_active = impl_->view_state.render_active; inputs.frame_slot = back_buffer_index; inputs.frame_index = impl_->frame_number;
        inputs.completed = impl_->graph_queue_executor.Completed(); inputs.reset_history = !previous_view_valid || new_session;
        inputs.revisions.scene = pending_effects.probe_revisions.static_layout;
        inputs.revisions.light = pending_effects.probe_revisions.light;
        const auto mix_revision = [](std::uint64_t &hash, std::uint64_t value) { hash = (hash ^ value) * 1099511628211ull; };
        for (const auto &source : canonical_instances)
        {
            mix_revision(inputs.revisions.scene, source.stable_id); mix_revision(inputs.revisions.scene, source.asset_id.value);
            mix_revision(inputs.revisions.scene, source.revisions.content); mix_revision(inputs.revisions.scene, source.revisions.material);
        }
        for (const auto &source : frame_lights)
        {
            mix_revision(inputs.revisions.light, source.stable_id); mix_revision(inputs.revisions.light, source.generation);
            mix_revision(inputs.revisions.light, source.revision);
            for (const auto value : {source.direction.x, source.direction.y, source.direction.z,
                    source.color.x, source.color.y, source.color.z, source.position.x, source.position.y, source.position.z,
                    source.quantity, source.range, source.shape.x, source.shape.y, source.emitter_radius,
                    source.inner_cone_radians, source.outer_cone_radians, source.area_tangent.x, source.area_tangent.y, source.area_tangent.z})
                mix_revision(inputs.revisions.light, std::bit_cast<UINT>(value));
            mix_revision(inputs.revisions.light, static_cast<UINT>(source.type)); mix_revision(inputs.revisions.light, static_cast<UINT>(source.unit));
            mix_revision(inputs.revisions.light, source.shadow_flags);
        }
        inputs.revisions.volume = impl_->volumetric_revision;
        if (primary_sun)
        {
            inputs.compact_shadow.atlas = {shadow_version, impl_->shadow.resource.Get()};
            inputs.compact_shadow.identity = {static_cast<UINT>(primary_sun->stable_id), static_cast<UINT>(primary_sun->stable_id >> 32), primary_sun->generation, 1};
            inputs.compact_shadow.receiver_bias = 0.0f;
            for (std::size_t cascade = 0; cascade < 3; ++cascade)
            {
                DirectX::XMStoreFloat4x4(&inputs.compact_shadow.view_projection[cascade],
                    DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&constants->shadow_view_projection[cascade])));
                inputs.compact_shadow.atlas_scale_offset[cascade] = constants->shadow_atlas_scale_offset[cascade];
            }
        }
        inputs.make_bindings = [&](D3D12_CPU_DESCRIPTOR_HANDLE suffix, std::uint32_t count, RayShaderBinder &binder) -> Result {
            if (count != kVolumetricDescriptorCount || !suffix.ptr)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid volumetric descriptor suffix.");
            auto destination = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            destination.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount + kVolumetricFrameTableOffset) * impl_->srv_stride;
            auto source = local_probe_inputs.frame_srv_base; source.ptr += kTemporalTextureDescriptorOffset * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(kRayFrameTableCount, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            destination.ptr += (kVolumetricDescriptorOffset - kTemporalTextureDescriptorOffset) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(count, destination, suffix, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto table = texture_table; table.ptr += UINT64(kVolumetricFrameTableOffset) * impl_->srv_stride;
            binder = [&, table](ID3D12GraphicsCommandList *list) -> Result {
                bind_compute(list); list->SetComputeRootDescriptorTable(13, character_table);
                list->SetComputeRootDescriptorTable(32, table); return Result::Success();
            };
            return Result::Success();
        };
        pending_effects.volumetrics = true;
        if (auto registered = impl_->volumetrics.Register(impl_->graph, inputs, volumetric_outputs); !registered) return registered;
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
            descriptor.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
                offset + 4) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(1, descriptor, opaque_hdr_srv,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
    }

    auto distortion_pass = impl_->graph.AddPass(6, kRenderPassNames[6], QueueHint::Direct);
    distortion_pass.Read(gbuffer_metadata_version, Access::ShaderRead);
    distortion_pass.Read(gbuffer_normal_version, Access::ShaderRead);
    distortion_pass.Read(depth_version, Access::ShaderRead);
    distortion_vectors_version = distortion_pass.Write(distortion_vectors_version, Access::RenderTarget);
    distortion_pass.SetExecute([&](RenderPassContext &context) {
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
    auto transparent_pass = impl_->graph.AddPass(7, kRenderPassNames[7], QueueHint::Direct);
    read_probe(transparent_pass);
    read_volumetrics(transparent_pass);
    // Retained GPU particles can sample the gradient even without new spawns.
    // Root 13 has ALL visibility, so declare every shader stage's read access.
    transparent_pass.Read(vfx_gradient_version, Access::AllShaderRead);
    transparent_pass.Read(geometry_records_version, Access::ShaderRead);
    transparent_pass.Read(material_parameter_version, Access::ShaderRead);
    transparent_pass.Read(hero_binding_version, Access::ShaderRead);
    transparent_pass.Read(hdr_version, Access::ShaderRead);
    read_geometry_pages(transparent_pass);
    transparent_pass.Read(dynamic_instances_version, Access::ShaderRead);
    transparent_pass.Read(dynamic_visible_refs_version, Access::ShaderRead);
    transparent_pass.Read(dynamic_draw_args_version, Access::IndirectArgs);
    if (have_skinned_instances)
        transparent_pass.Read(skin_cache_version, Access::ShaderRead);
    if (have_skinned_instances && previous_skin_frame_valid)
        transparent_pass.Read(previous_skin_cache_version, Access::ShaderRead);
    transparent_pass.Read(depth_version, Access::DepthShaderRead);
    transparent_pass.Read(shadow_version, Access::ShaderRead);
    transparent_pass.Read(particles_version, Access::ShaderRead);
    transparent_pass.Read(particle_alive_output_version, Access::ShaderRead);
    transparent_pass.Read(indirect_arguments_version, Access::IndirectArgs);
    transparent_pass.Read(ribbon_history_version, Access::ShaderRead);
    transparent_pass.Read(ribbon_previous_version, Access::ShaderRead);
    transparent_pass.Read(ribbon_arguments_version, Access::IndirectArgs);
    transparent_pass.Read(gbuffer_metadata_version, Access::ShaderRead);
    transparent_pass.Read(gbuffer_normal_version, Access::ShaderRead);
    oit_accumulation_version = transparent_pass.Write(oit_accumulation_version, Access::RenderTarget);
    oit_revealage_version = transparent_pass.Write(oit_revealage_version, Access::RenderTarget);
    transparent_pass.SetExecute([&](RenderPassContext &context) {
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
        for (UINT bin = 0; bin < 2; ++bin)
        {
            list->SetGraphicsRoot32BitConstant(6, bin, 0);
            list->ExecuteIndirect(
                impl_->draw_signature.Get(), 1, impl_->indirect_arguments.resource.Get(),
                bin * sizeof(D3D12_DRAW_ARGUMENTS), nullptr, 0);
        }
        list->SetGraphicsRoot32BitConstant(6, 0, 0);
        if (ground_ring_count != 0)
        {
            list->SetPipelineState(impl_->ground_ring_pipeline.Get());
            list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() +
                        ground_ring_data_offset);
            list->DrawInstanced(6, ground_ring_count, 0, 0);
        }

        if (owner_mesh_count != 0)
        {
            list->SetPipelineState(impl_->owner_mesh_pipeline.Get());
            list->SetGraphicsRootShaderResourceView(
                11, frame.upload.resource->GetGPUVirtualAddress() + owner_mesh_data_offset);
            for (std::uint32_t index = 0; index < owner_mesh_count; ++index)
            {
                list->SetGraphicsRoot32BitConstant(6, index, 0);
                list->DrawInstanced(
                    impl_->vfx_mesh_vertex_counts[typed_mesh_spawns[index].mesh_index - 1], 1, 0, 0);
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
        bind_scene_deformation_roots(list, true);
        const auto draw_cached_slime_range =
            [&](std::size_t asset, std::pair<std::size_t, std::size_t> range) {
            if (range.second == 0) return;
            list->SetGraphicsRootShaderResourceView(34, dynamic_instance_base);
            list->SetGraphicsRootShaderResourceView(
                35, frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
            const auto bucket = kMonsterVisibilityBucketBegin + asset;
            if (auto result = impl_->DrawGeometry(list, back_buffer_index,
                    impl_->hero_translucent_pipeline.Get(), *bucket_pages[bucket],
                    dynamic_geometry_passes[bucket][0], frame.dynamic_draw_args.resource.Get(),
                    bucket * kGeometryArgumentStride); !result)
                throw std::runtime_error(std::string(result.Message()));
        };
        if (impl_->hero_transparency.AllocatedBytes() == 0)
        {
            draw_cached_slime_range(0, monster_ranges[0]);
            draw_cached_slime_range(1, monster_ranges[1]);
            draw_cached_slime_range(2, monster_ranges[2]);
        }
        if (projectile_range.second != 0)
        {
            list->SetGraphicsRootShaderResourceView(34, dynamic_instance_base);
            list->SetGraphicsRootShaderResourceView(
                35, frame.upload.resource->GetGPUVirtualAddress() +
                        dynamic_ref_data_offset);
            if (auto result = impl_->DrawGeometry(list, back_buffer_index,
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
                    : static_cast<const GeometryPageGpu &>(impl_->monster_assets[draw.asset_index]);
                if (auto result = impl_->DrawGeometry(list, back_buffer_index,
                        impl_->fresnel_shell_pipeline.Get(), page, draw.geometry_pass); !result)
                    throw std::runtime_error(std::string(result.Message()));
            }
        }
    });

    std::vector<HeroTransparencySceneRead> hero_scene_reads{
        {geometry_records_version, Access::ShaderRead}, {material_parameter_version, Access::ShaderRead},
        {hero_binding_version, Access::ShaderRead}, {dynamic_instances_version, Access::ShaderRead},
        {dynamic_visible_refs_version, Access::ShaderRead}, {dynamic_draw_args_version, Access::IndirectArgs},
        {shadow_version, Access::ShaderRead}, {impl_->graph.Version(environment_brdf_lut), Access::ShaderRead},
        {probe_outputs.radiance, Access::ShaderRead}, {probe_outputs.prefiltered, Access::ShaderRead},
        {probe_outputs.irradiance, Access::ShaderRead}, {light_outputs.lights, Access::ShaderRead},
        {light_outputs.ranges, Access::ShaderRead}, {light_outputs.inline_indices, Access::ShaderRead},
        {light_outputs.spill_indices, Access::ShaderRead}, {ltc_outputs.matrix, Access::ShaderRead},
        {ltc_outputs.amplitude, Access::ShaderRead}};
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
        hero_scene_reads.push_back({page.versions[0], Access::VertexBuffer});
        hero_scene_reads.push_back({page.versions[1], Access::IndexBuffer});
    }
    HeroTransparencyInputs hero_inputs;
    hero_inputs.opaque_depth = {depth_version, impl_->depth.resource.Get()};
    hero_inputs.hdr = {hdr_version, opaque_hdr_resource};
    hero_inputs.accumulation = {oit_accumulation_version, impl_->oit_accumulation.resource.Get()};
    hero_inputs.revealage = {oit_revealage_version, impl_->oit_revealage.resource.Get()};
    hero_inputs.accumulation_rtv = oit_accumulation_rtv;
    hero_inputs.revealage_rtv = oit_revealage_rtv;
    hero_inputs.render_active = impl_->view_state.render_active;
    hero_inputs.frame_slot = back_buffer_index;
    hero_inputs.completed = impl_->graph_queue_executor.Completed();
    hero_inputs.shader_heap = impl_->srv_heap.Get();
    hero_inputs.descriptor_source_heap = impl_->srv_cpu_heap.Get();
    hero_inputs.table_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    hero_inputs.table_cpu.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
        kHeroTransparencyDescriptorOffset) * impl_->srv_stride;
    hero_inputs.table_gpu = texture_table;
    hero_inputs.table_gpu.ptr += static_cast<UINT64>(kHeroTransparencyDescriptorOffset) * impl_->srv_stride;
    hero_inputs.descriptor_stride = impl_->srv_stride;
    hero_inputs.scene_reads = hero_scene_reads;
    hero_inputs.draw = [&](ID3D12GraphicsCommandList *list, ID3D12PipelineState *pipeline,
                          HeroTransparencyPhase, std::uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE table) {
        bind_graphics(list);
        bind_scene_roots(list);
        list->SetGraphicsRootDescriptorTable(36, table);
        list->SetGraphicsRoot32BitConstant(6, kDefaultSceneDrawMaterial | 1u, 1);
        list->SetGraphicsRootShaderResourceView(34, dynamic_instance_base);
        list->SetGraphicsRootShaderResourceView(35, frame.dynamic_visible_refs.resource->GetGPUVirtualAddress());
        for (std::size_t asset = 0; asset < 3; ++asset)
        {
            if (monster_ranges[asset].second == 0) continue;
            const auto bucket = kMonsterVisibilityBucketBegin + asset;
            if (auto drawn = impl_->DrawGeometry(list, back_buffer_index, pipeline, *bucket_pages[bucket],
                    dynamic_geometry_passes[bucket][0], frame.dynamic_draw_args.resource.Get(),
                    bucket * kGeometryArgumentStride); !drawn)
                throw std::runtime_error(std::string(drawn.Message()));
        }
    };
    HeroTransparencyOutputs hero_outputs;
    if (auto registered = impl_->hero_transparency.RegisterLayers(impl_->graph, hero_inputs, hero_outputs); !registered)
        return registered;
    pending_effects.transparency = static_cast<bool>(hero_outputs.depth.resource);
    if (pending_effects.transparency)
    {
        oit_accumulation_version = hero_outputs.accumulation;
        oit_revealage_version = hero_outputs.revealage;
        constants->lod_clock.w |= 4u;
    }
    auto composite_pass = impl_->graph.AddPass(8, kRenderPassNames[8], QueueHint::Direct);
    read_probe(composite_pass);
    read_volumetrics(composite_pass);
    if (flash_add_count || ground_add_count ||
        std::ranges::any_of(ribbon_outputs, [](const GpuRibbonOutput &output) {
            return (output.metadata[3] & 1u) == 0;
        }))
        composite_pass.Read(vfx_gradient_version, Access::AllShaderRead);
    composite_pass.Read(hdr_version, Access::ShaderRead);
    composite_pass.Read(distortion_vectors_version, Access::ShaderRead);
    composite_pass.Read(oit_accumulation_version, Access::ShaderRead);
    composite_pass.Read(oit_revealage_version, Access::ShaderRead);
    composite_pass.Read(gbuffer_metadata_version, Access::ShaderRead);
    composite_pass.Read(gbuffer_normal_version, Access::ShaderRead);
    composite_pass.Read(ribbon_history_version, Access::ShaderRead);
    composite_pass.Read(ribbon_previous_version, Access::ShaderRead);
    composite_pass.Read(ribbon_arguments_version, Access::IndirectArgs);
    composite_pass.Read(depth_version, Access::DepthShaderRead);
    composite_hdr_version = composite_pass.Write(composite_hdr_version, Access::RenderTarget);
    composite_pass.SetExecute([&](RenderPassContext &context) {
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
    if (auto registered = impl_->hero_transparency.RegisterComposition(impl_->graph,
            {composite_hdr_version, composite_hdr_resource}, hero_outputs); !registered) return registered;
    if (hero_outputs.corrected.resource)
    {
        composite_hdr_version = hero_outputs.corrected;
        composite_hdr_resource = hero_outputs.corrected_resource;
        auto descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        descriptor.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
            kViewDescriptorOffset + 4) * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(1, descriptor, hero_outputs.corrected_srv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    ScreenOverlayOutputs screen_overlay_outputs;
    ResourceVersion screen_overlay_gradient;
    const auto screen_intensity = impl_->explicit_frame->screen_overlay_intensity
        ? *impl_->explicit_frame->screen_overlay_intensity : impl_->config.screen_overlay_intensity;
    if (!typed_screen.empty() && screen_intensity > 0)
    {
        if (!impl_->screen_overlay.AllocatedBytes())
        {
            auto overlay_config = impl_->config;
            overlay_config.screen_overlay_intensity = screen_intensity;
            ScreenOverlayCandidate candidate;
            if (auto prepared = impl_->PrepareScreenOverlay(overlay_config,
                    impl_->view_state.render_capacity, candidate); !prepared) return prepared;
            if (auto committed = impl_->screen_overlay.Commit(std::move(candidate),
                    impl_->graph_queue_executor.Completed()); !committed) return committed;
        }
        const auto gradient = import_texture(impl_->vfx_gradient, Access::ShaderRead, "VfxScreenGradient");
        screen_overlay_gradient = impl_->graph.Version(gradient);
        ScreenOverlayInputs overlay_inputs;
        overlay_inputs.commands = typed_screen;
        overlay_inputs.gradient = screen_overlay_gradient;
        overlay_inputs.gradient_resource = impl_->vfx_gradient.resource.Get();
        overlay_inputs.render_active = impl_->view_state.render_active;
        overlay_inputs.output_active = {impl_->width, impl_->height};
        const auto inverse_view = DirectX::XMMatrixInverse(nullptr, view);
        DirectX::XMStoreFloat3(&overlay_inputs.camera_right, inverse_view.r[0]);
        DirectX::XMStoreFloat3(&overlay_inputs.camera_up, inverse_view.r[1]);
        overlay_inputs.intensity = screen_intensity;
        overlay_inputs.frame_slot = back_buffer_index;
        overlay_inputs.completed = impl_->graph_queue_executor.Completed();
        if (auto registered = impl_->screen_overlay.RegisterMask(impl_->graph, overlay_inputs,
                screen_overlay_outputs); !registered) return registered;
        pending_effects.screen_overlay = screen_overlay_outputs.active;
    }
    {
        auto target = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        target.ptr += (SIZE_T(back_buffer_index) * kTextureDescriptorCount + kVfxScreenReadDescriptorOffset) * impl_->srv_stride;
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
    auto pre_exposure = impl_->graph.AddPass(33, "PreExposure", QueueHint::Direct);
    pre_exposure.Read(composite_hdr_version, Access::ShaderRead);
    pre_exposure.Read(exposure_read_version, Access::ShaderRead);
    scene_work_version = pre_exposure.Write(scene_work_version, Access::RenderTarget);
    pre_exposure.SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        constexpr float clear[]{0, 0, 0, 0};
        list->ClearRenderTargetView(scene_work_rtv, clear, 0, nullptr);
        draw_fullscreen(list, impl_->pre_exposure_pipeline.Get(), scene_work_rtv);
    });
    auto clear_histogram = impl_->graph.AddPass(34, "ExposureHistogramClear", QueueHint::Compute);
    histogram_version = clear_histogram.Write(histogram_version, Access::UnorderedWrite);
    clear_histogram.SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->exposure_clear_pipeline.Get());
        list->Dispatch(1, 1, 1);
    });
    auto build_histogram = impl_->graph.AddPass(35, "ExposureHistogramBuild", QueueHint::Compute);
    build_histogram.Read(scene_work_version, Access::ComputeRead);
    build_histogram.Read(exposure_read_version, Access::ComputeRead);
    histogram_version = build_histogram.ReadWrite(histogram_version, Access::UnorderedWrite);
    build_histogram.SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->exposure_build_pipeline.Get());
        list->Dispatch((impl_->render_width + 15) / 16, (impl_->render_height + 15) / 16, 1);
    });
    auto reduce_exposure = impl_->graph.AddPass(36, "ExposureReduce", QueueHint::Compute);
    reduce_exposure.Read(exposure_read_version, Access::ComputeRead);
    histogram_version = reduce_exposure.ReadWrite(histogram_version, Access::UnorderedWrite);
    exposure_write_version = reduce_exposure.Write(exposure_write_version, Access::UnorderedWrite);
    reduce_exposure.SetExecute([&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_compute(list);
        list->SetPipelineState(impl_->exposure_reduce_pipeline.Get());
        list->Dispatch(1, 1, 1);
    });

    ReconstructionGuidesOutputs reconstruction_outputs;
    const auto composite_signal = feature_declaration(SignalSemantic::CompositeHdr, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear);
    const auto final_opaque_signal = feature_declaration(SignalSemantic::OpaqueHdr, SignalEncoding::RgbaFloat, RadiometricMeaning::SceneLinear, 5);
    const auto exposure_signal = feature_declaration(SignalSemantic::Exposure, SignalEncoding::Float, RadiometricMeaning::None, 0,
        {static_cast<UINT>(impl_->exposure_values[exposure_read_index].resource->GetDesc().Width), 1}, ResourceKind::Buffer);
    const auto revealage_signal = feature_declaration(SignalSemantic::OitRevealage, SignalEncoding::Float);
    const auto reactive_signal = feature_declaration(SignalSemantic::ScreenOverlayAlpha, SignalEncoding::Float);
    import_feature_signal(composite_signal, composite_hdr_version);
    import_feature_signal(final_opaque_signal, hdr_version);
    import_feature_signal(exposure_signal, exposure_read_version);
    import_feature_signal(revealage_signal, oit_revealage_version);
    import_feature_signal(reactive_signal, screen_overlay_outputs.reactive);
    const SignalExtent reconstructed_extent{impl_->width, impl_->height};
    auto reconstructed_signal = feature_declaration(SignalSemantic::TemporalHdr, SignalEncoding::RgbaFloat,
        RadiometricMeaning::PreExposedSceneLinear, 1, reconstructed_extent);
    reconstructed_signal.metadata.filtering = SignalFiltering::Resolved;
    const auto raw_reconstructed_signal = feature_declaration(SignalSemantic::ReconstructedSceneHdr, SignalEncoding::RgbaFloat,
        RadiometricMeaning::SceneLinear, 1, reconstructed_extent);
    const auto reconstruction_depth_signal = feature_declaration(SignalSemantic::ReconstructionDepth, SignalEncoding::Depth);
    const auto reconstruction_motion_signal = feature_declaration(SignalSemantic::ReconstructionMotion, SignalEncoding::Motion);
    const auto reconstruction_guide = [&](std::span<const SignalHandle> signals, SignalSemantic semantic, UINT version = 0) {
        const auto guide = feature_guide(signals, semantic, version);
        return ReconstructionGuideResource{guide.version, guide.resource};
    };
    std::vector<SignalDeclaration> sr_requirements{require_guide(depth_signal), require_guide(motion_signal),
        require_guide(metadata_signal), require_guide(surface_history_signal, true), composite_signal, final_opaque_signal,
        require_guide(exposure_signal), require_guide(revealage_signal)};
    if (screen_overlay_outputs.reactive.resource) sr_requirements.push_back(require_guide(reactive_signal));
    const std::array transparent_signals{
        feature_declaration(SignalSemantic::TransparentDepth, SignalEncoding::Depth),
        feature_declaration(SignalSemantic::TransparentMotion, SignalEncoding::Motion),
        feature_declaration(SignalSemantic::TransparentHistory, SignalEncoding::SurfaceHistory),
        feature_declaration(SignalSemantic::TransparentClassification, SignalEncoding::Metadata),
        feature_declaration(SignalSemantic::TransparentMetadata, SignalEncoding::Metadata),
        feature_declaration(SignalSemantic::TransparentOverflow, SignalEncoding::Metadata),
        feature_declaration(SignalSemantic::TransparentTransport, SignalEncoding::Metadata)};
    const std::array transparent_versions{hero_outputs.depth, hero_outputs.motion, hero_outputs.history,
        hero_outputs.classification, hero_outputs.metadata, hero_outputs.overflow, hero_outputs.transport};
    if (pending_effects.transparency)
        for (UINT index = 0; index < transparent_versions.size(); ++index)
        {
            import_feature_signal(transparent_signals[index], transparent_versions[index]);
            sr_requirements.push_back(require_guide(transparent_signals[index], index == 2));
        }
    std::vector<FeatureRegistration> provider_features;
    provider_features.push_back({FeatureId::Reconstruction, [&] { return use_sr_reconstruction; },
        std::move(sr_requirements), {reconstructed_signal, raw_reconstructed_signal, reconstruction_depth_signal, reconstruction_motion_signal},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            ReconstructionGuidesConstants guides_constants;
            guides_constants.inverse_unjittered_view_projection = constants->inverse_unjittered_view_projection;
            guides_constants.previous_unjittered_view_projection = constants->previous_unjittered_view_projection;
            guides_constants.camera_origin = {current_temporal_eye.x, current_temporal_eye.y,
                current_temporal_eye.z, 0};
            guides_constants.jitter_uv = constants->surface_jitter_uv;
            guides_constants.frame_info.x = impl_->temporal_history_valid ? 1u : 0u;
            ReconstructionGuidesInputs guides_inputs;
            guides_inputs.depth = reconstruction_guide(signals, SignalSemantic::Depth);
            guides_inputs.motion = reconstruction_guide(signals, SignalSemantic::Motion);
            guides_inputs.metadata = reconstruction_guide(signals, SignalSemantic::SurfaceMetadata);
            guides_inputs.surface_history = reconstruction_guide(signals, SignalSemantic::SurfaceHistory);
            guides_inputs.composite_hdr = reconstruction_guide(signals, SignalSemantic::CompositeHdr);
            guides_inputs.opaque_hdr = reconstruction_guide(signals, SignalSemantic::OpaqueHdr, 5);
            guides_inputs.exposure = reconstruction_guide(signals, SignalSemantic::Exposure);
            guides_inputs.transparency_revealage = reconstruction_guide(signals, SignalSemantic::OitRevealage);
            guides_inputs.render_active = impl_->view_state.render_active; guides_inputs.frame_slot = back_buffer_index;
            guides_inputs.completed = impl_->graph_queue_executor.Completed(); guides_inputs.reset_history = !impl_->temporal_history_valid || new_session;
            guides_inputs.transparent_depth = reconstruction_guide(signals, SignalSemantic::TransparentDepth);
            guides_inputs.transparent_motion = reconstruction_guide(signals, SignalSemantic::TransparentMotion);
            guides_inputs.transparent_history = reconstruction_guide(signals, SignalSemantic::TransparentHistory);
            guides_inputs.transparent_classification = reconstruction_guide(signals, SignalSemantic::TransparentClassification);
            guides_inputs.transparent_metadata = reconstruction_guide(signals, SignalSemantic::TransparentMetadata);
            guides_inputs.transparent_overflow = reconstruction_guide(signals, SignalSemantic::TransparentOverflow);
            guides_inputs.transparent_transport = reconstruction_guide(signals, SignalSemantic::TransparentTransport);
            guides_inputs.transparent_guides_enabled = pending_effects.transparency;
            guides_inputs.screen_overlay_alpha = reconstruction_guide(signals, SignalSemantic::ScreenOverlayAlpha);
            SdkReconstructionFrame sdk_frame;
            sdk_frame.identity = impl_->sdk_frame_identity; sdk_frame.camera = impl_->sdk_camera;
            if (auto registered = impl_->reconstruction_guides.Register(impl_->graph, guides_inputs,
                    guides_constants, sdk_frame,
                    [provider = impl_->reconstruction_provider.get()](ID3D12GraphicsCommandList *commands,
                        const SdkReconstructionFrame &frame) { return provider->Execute(commands, frame); },
                    impl_->reconstruction_plan.effective_provider.value, reconstruction_outputs,
                    impl_->reconstruction_provider->UsesExternalQueue()
                        ? ReconstructionGuidesSystem::RegisterExternalReconstruction{
                            [provider = impl_->reconstruction_provider.get()](PassBuilder &pass,
                                const SdkReconstructionFrame &packet, std::uint32_t slot,
                                std::span<const ExternalResourceAccess> boundaries, std::vector<std::shared_ptr<const void>> owners) {
                                return provider->RegisterExternal(pass, packet, slot, boundaries, std::move(owners));
                            }} : ReconstructionGuidesSystem::RegisterExternalReconstruction{}); !registered)
                return registered;
            pending_effects.reconstruction = true;
            emit_feature_signal(outputs, FeatureId::Reconstruction, reconstructed_signal, reconstruction_outputs.pre_exposed);
            emit_feature_signal(outputs, FeatureId::Reconstruction, raw_reconstructed_signal, reconstruction_outputs.raw_output);
            emit_feature_signal(outputs, FeatureId::Reconstruction, reconstruction_depth_signal, reconstruction_outputs.depth);
            emit_feature_signal(outputs, FeatureId::Reconstruction, reconstruction_motion_signal, reconstruction_outputs.motion);
            return Result::Success();
    }});
    const std::array dlss_guide_signals{
        feature_declaration(SignalSemantic::DiffuseAlbedo, SignalEncoding::RgbaFloat, RadiometricMeaning::None, 1),
        feature_declaration(SignalSemantic::SpecularAlbedo, SignalEncoding::RgbaFloat, RadiometricMeaning::None, 1),
        feature_declaration(SignalSemantic::NormalRoughness, SignalEncoding::RgbaFloat, RadiometricMeaning::None, 1),
        feature_declaration(SignalSemantic::DiffuseHitDistance, SignalEncoding::Float, RadiometricMeaning::None, 2),
        feature_declaration(SignalSemantic::SpecularHitDistance, SignalEncoding::Float, RadiometricMeaning::None, 2),
        reconstruction_depth_signal, reconstruction_motion_signal,
        feature_declaration(SignalSemantic::ReconstructionExposure, SignalEncoding::Float, RadiometricMeaning::None, 1, {1, 1}),
        feature_declaration(SignalSemantic::ResponsiveMask, SignalEncoding::Float, RadiometricMeaning::None, 1)};
    auto dlss_requirements = with_ray_requirements(specular_requirements);
    for (const auto declaration : {noisy_diffuse_signal, noisy_specular_signal, diffuse_distance_signal, specular_distance_signal, exposure_signal})
        dlss_requirements.push_back(require_guide(declaration));
    if (screen_overlay_outputs.reactive.resource) dlss_requirements.push_back(require_guide(reactive_signal));
    provider_features.push_back({FeatureId::RayReconstructionGuides,
        [&] { return use_dlss_rr && !use_sr_reconstruction; }, std::move(dlss_requirements),
        {dlss_guide_signals.begin(), dlss_guide_signals.end()},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            // The actual full-resolution noisy components and current exposure
            // feed one guide pack after every responsive producer exists.
            resolve_specular_guides(signals, rr_inputs.transport);
            noisy_diffuse.color.version = feature_resource(signals, SignalSemantic::NoisyDiffuseRadiance, 1);
            noisy_diffuse.metric_count.version = feature_resource(signals, SignalSemantic::DiffuseHitDistance, 1);
            noisy_specular.color.version = feature_resource(signals, SignalSemantic::NoisySpecularRadiance, 1);
            noisy_specular.metric_count.version = feature_resource(signals, SignalSemantic::SpecularHitDistance, 1);
            const auto exposure = feature_guide(signals, SignalSemantic::Exposure);
            rr_inputs.exposure = {exposure.version, exposure.resource, {}};
            const auto reactive = feature_guide(signals, SignalSemantic::ScreenOverlayAlpha);
            rr_inputs.screen_overlay_alpha = {reactive.version, reactive.resource, screen_overlay_outputs.reactive_srv};
            rr_inputs.make_bindings = make_rr_bindings(0); pending_effects.ray_reconstruction = true;
            if (auto registered = impl_->ray_reconstruction.RegisterDlssGuides(impl_->graph, rr_inputs,
                    noisy_diffuse, noisy_specular, transport_constants, dlss_guides); !registered) return registered;
            const std::array versions{dlss_guides.diffuse_albedo.version, dlss_guides.specular_albedo.version,
                dlss_guides.normal_roughness.version, dlss_guides.diffuse_hit_distance.version, dlss_guides.specular_hit_distance.version,
                dlss_guides.depth.version, dlss_guides.motion.version, dlss_guides.exposure.version, dlss_guides.responsive.version};
            for (UINT index = 0; index < versions.size(); ++index)
                emit_feature_signal(outputs, FeatureId::RayReconstructionGuides, dlss_guide_signals[index], versions[index]);
            return Result::Success();
        }});
    std::vector<SignalDeclaration> fused_requirements{composite_signal, final_opaque_signal};
    for (const auto declaration : dlss_guide_signals) fused_requirements.push_back(require_guide(declaration));
    provider_features.push_back({FeatureId::FusedRayReconstruction,
        [&] { return use_dlss_rr && !use_sr_reconstruction; }, std::move(fused_requirements), {reconstructed_signal, raw_reconstructed_signal},
        [&](std::span<const SignalHandle> signals, std::vector<SignalHandle> &outputs) -> Result {
            dlss_guides.diffuse_albedo.version = feature_resource(signals, SignalSemantic::DiffuseAlbedo, 1);
            dlss_guides.specular_albedo.version = feature_resource(signals, SignalSemantic::SpecularAlbedo, 1);
            dlss_guides.normal_roughness.version = feature_resource(signals, SignalSemantic::NormalRoughness, 1);
            dlss_guides.diffuse_hit_distance.version = feature_resource(signals, SignalSemantic::DiffuseHitDistance, 2);
            dlss_guides.specular_hit_distance.version = feature_resource(signals, SignalSemantic::SpecularHitDistance, 2);
            dlss_guides.depth.version = feature_resource(signals, SignalSemantic::ReconstructionDepth);
            dlss_guides.motion.version = feature_resource(signals, SignalSemantic::ReconstructionMotion);
            dlss_guides.exposure.version = feature_resource(signals, SignalSemantic::ReconstructionExposure, 1);
            dlss_guides.responsive.version = feature_resource(signals, SignalSemantic::ResponsiveMask, 1);
            RayDlssCompositeInputs input;
            const auto composite = feature_guide(signals, SignalSemantic::CompositeHdr);
            input.noisy_color = {composite.version, composite.resource, {}};
            input.exposure = dlss_guides.exposure;
            const auto opaque = feature_guide(signals, SignalSemantic::OpaqueHdr, 5);
            input.color_before_transparency = {opaque.version, opaque.resource, {}};
            input.owners = rr_inputs.owners;
            rr_inputs.make_bindings = make_rr_bindings(1);
            RayDlssOutputs output;
            if (auto registered = impl_->ray_reconstruction.RegisterDlssFused(impl_->graph, rr_inputs,
                    dlss_guides, input, transport_constants, output); !registered) return registered;
            reconstruction_outputs.pre_exposed = output.pre_exposed.version;
            reconstruction_outputs.pre_exposed_resource = output.pre_exposed.resource;
            reconstruction_outputs.pre_exposed_srv = output.pre_exposed.srv;
            reconstruction_outputs.raw_output = output.raw_output.version;
            reconstruction_outputs.depth = output.guides.depth.version;
            reconstruction_outputs.motion = output.guides.motion.version;
            reconstruction_outputs.depth_resource = output.guides.depth.resource;
            reconstruction_outputs.motion_resource = output.guides.motion.resource;
            reconstruction_outputs.owner = output.owner;
            emit_feature_signal(outputs, FeatureId::FusedRayReconstruction, reconstructed_signal, reconstruction_outputs.pre_exposed);
            emit_feature_signal(outputs, FeatureId::FusedRayReconstruction, raw_reconstructed_signal, reconstruction_outputs.raw_output);
            return Result::Success();
        }});
    if (auto composed = compose_features(std::move(provider_features)); !composed) return composed;
    if (use_native_reconstruction)
    {
        auto output_descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        output_descriptor.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
            kTemporalTextureDescriptorOffset + 1) * impl_->srv_stride;
        impl_->device->CopyDescriptorsSimple(1, output_descriptor, reconstruction_outputs.pre_exposed_srv,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!impl_->config.bloom)
            tone_map_input = alternate_texture_table(kToneMapInputDescriptorOffset,
                reconstruction_outputs.pre_exposed_resource);
    }
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
        if (!impl_->view_targets_initialized)
        {
            constexpr float clear[]{0, 0, 0, 0};
            list->ClearRenderTargetView(post_a_rtv, clear, 0, nullptr);
        }
        draw_fullscreen_with_table(list, impl_->tone_map_pipeline.Get(), post_a_rtv,
                                   impl_->config.bloom && !impl_->config.color_grading ? texture_table : tone_map_input);
    };

    post_callbacks[6] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        if (!impl_->config.bloom)
        {
            constexpr float clear[] = {0, 0, 0, 0};
            list->ClearRenderTargetView(post_b_rtv, clear, 0, nullptr);
        }
        draw_fullscreen(list, impl_->outline_pipeline.Get(), post_b_rtv);
    };

    post_callbacks[7] = [&](RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        if (!impl_->view_targets_initialized)
        {
            constexpr float clear[]{0, 0, 0, 0};
            list->ClearRenderTargetView(display_linear_rtv, clear, 0, nullptr);
        }
        draw_fullscreen_with_table(list, impl_->fxaa_pipeline.Get(), display_linear_rtv,
                                   impl_->config.outline ? texture_table : fxaa_input);
        if (screen_overlay_outputs.active)
            impl_->screen_overlay.Draw(list, back_buffer_index, display_linear_rtv);
#if defined(HS_DEVELOPMENT_TOOLS)
        if (impl_->config.devtools_visible && !impl_->sdk_presentation.IsEnabled())
        {
            ID3D12DescriptorHeap *imgui_heaps[] = {impl_->imgui_heap.Get()};
            list->SetDescriptorHeaps(1, imgui_heaps);
            ImGui_ImplDX12_RenderDrawData(imgui_draw_data, list);
        }
#endif
    };

    post_callbacks[8] = [&](RenderPassContext &context) {
        if (presented_view && impl_->sdk_presentation.IsEnabled()) return;
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        bind_graphics(list);
        (void)events;
        list->RSSetViewports(1, &output_viewport);
        list->RSSetScissorRects(1, &output_scissor);
        if (initialize_offscreen_output)
        {
            list->DiscardResource(impl_->back_buffers[back_buffer_index].Get(), nullptr);
            constexpr float clear[]{0, 0, 0, 0};
            list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        }
        draw_fullscreen(list, impl_->ui_pipeline.Get(), rtv);
    };

    const auto ui_description = ui_texture.resource->GetDesc();
    PostSubgraphInputs post_inputs{
        impl_->active_view_id.value, {impl_->render_width, impl_->render_height}, {impl_->width, impl_->height},
        {static_cast<std::uint32_t>(ui_description.Width), ui_description.Height},
        impl_->config.bloom, impl_->config.outline, hdr_version, scene_work_version,
        depth_version, gbuffer_normal_version, gbuffer_metadata_version,
        gbuffer_motion_version, gbuffer_surface_history_version, temporal_read_version, ui_version,
        temporal_normal_read_version, temporal_material_read_version, exposure_read_version,
        reconstruction_outputs.pre_exposed,
        {hero_outputs.depth, hero_outputs.motion, hero_outputs.history,
         hero_outputs.classification, hero_outputs.metadata, hero_outputs.overflow, hero_outputs.transport}};
    post_inputs.reactive_composite_hdr = composite_hdr_version;
    if (use_native_reconstruction)
    {
        const auto produced = std::ranges::find_if(feature_signals, [&](const auto &signal) {
            return signal.declaration.key == reconstructed_signal.key;
        });
        if (produced == feature_signals.end())
            return Result::Failure(ErrorCode::InvalidState,"hs_renderer_features","Post reconstruction signal has no registered producer.");
        post_inputs.external_temporal_signal = *produced;
    }
    HdrPostOutputs hdr_processed;
    if (screen_overlay_outputs.active)
    {
        post_inputs.screen_overlay_alpha = screen_overlay_outputs.reactive;
        post_inputs.screen_overlay_upload = screen_overlay_outputs.upload;
        post_inputs.screen_overlay_gradient = screen_overlay_gradient;
    }
    auto reconstructed_hdr_resource = use_native_reconstruction ? reconstruction_outputs.pre_exposed_resource
        : impl_->temporal_history[temporal_write_index].resource.Get();
    const auto hdr_post_inputs = [&](ResourceVersion hdr, ID3D12Resource *resource) {
        HdrPostInputs inputs;
        inputs.hdr = {hdr, resource};
        inputs.depth = {depth_version, impl_->depth.resource.Get()};
        inputs.normal = {gbuffer_normal_version, surface_normal_resource};
        inputs.motion = {gbuffer_motion_version, impl_->gbuffer_motion.resource.Get()};
        inputs.surface_history = {gbuffer_surface_history_version, impl_->gbuffer_surface_history.resource.Get()};
        inputs.transparent_depth = {hero_outputs.depth, hero_outputs.depth_resource};
        inputs.transparent_motion = {hero_outputs.motion, hero_outputs.motion_resource};
        inputs.transparent_history = {hero_outputs.history, hero_outputs.history_resource};
        inputs.transparent_classification = {hero_outputs.classification, hero_outputs.classification_resource};
        inputs.transparent_guides_enabled = pending_effects.transparency;
        inputs.render_active = impl_->view_state.render_active;
        inputs.output_active = {impl_->width, impl_->height};
        inputs.frame_slot = back_buffer_index;
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
            const float render_dt = impl_->render_submission_time.time_since_epoch().count() == 0 ? 0.0f
                : std::chrono::duration<float>(cpu_recording_start - impl_->render_submission_time).count();
            post_constants.frame_parameters = {render_dt,
                previous_view_valid && !new_session && render_dt > 0.0f ? 1.0f : 0.0f, 0, 0};
            if (auto registered = impl_->hdr_post.RegisterMotionAndDoF(graph,
                    hdr_post_inputs(hdr, reconstructed_hdr_resource), post_constants, hdr_processed); !registered)
                return registered;
            pending_effects.hdr_post = true;
            processed = hdr_processed.hdr;
            reconstructed_hdr_resource = hdr_processed.hdr_resource;
            auto descriptor = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            descriptor.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
                kTemporalTextureDescriptorOffset + 1) * impl_->srv_stride;
            impl_->device->CopyDescriptorsSimple(1, descriptor, hdr_processed.hdr_srv,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            return Result::Success();
        };
    if (impl_->config.color_grading)
        post_inputs.pre_tone_map = [&](RenderGraphBuilder &graph, ResourceVersion hdr, ResourceVersion &graded) -> Result {
            auto inputs = hdr_post_inputs(hdr, impl_->config.bloom
                ? impl_->post_b.resource.Get() : reconstructed_hdr_resource);
            inputs.sharpening = impl_->config.bloom ? 0.0f : impl_->config.sharpening;
            HdrPostOutputs output;
            if (auto registered = impl_->hdr_post.RegisterGrade(graph, inputs, output); !registered) return registered;
            pending_effects.hdr_post = true;
            graded = output.hdr;
            tone_map_input = alternate_texture_table(kToneMapInputDescriptorOffset, output.hdr_resource);
            return Result::Success();
        };
    const PostSubgraphTargets post_targets{
        temporal_write_version, bloom_half_version, bloom_quarter_version,
        bloom_half_combined_version, post_a_version, post_b_version, back_buffer_version,
        temporal_normal_write_version, temporal_material_write_version, display_linear_version};
    PostSubgraphOutputs post_outputs;
    if (auto built = BuildPostSubgraphs(impl_->graph, post_inputs, post_targets,
                                       std::move(post_callbacks), post_outputs); !built)
        return built;
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
        input.display_linear = {display_linear_version, impl_->display_linear.resource.Get()};
        input.encoded_ui = {ui_version, ui_texture.resource.Get()};
        input.depth = use_native_reconstruction
            ? PresentationGuideResource{reconstruction_outputs.depth, reconstruction_outputs.depth_resource}
            : PresentationGuideResource{depth_version, impl_->depth.resource.Get()};
        input.motion = use_native_reconstruction
            ? PresentationGuideResource{reconstruction_outputs.motion, reconstruction_outputs.motion_resource}
            : PresentationGuideResource{gbuffer_motion_version, impl_->gbuffer_motion.resource.Get()};
        input.motion_contains_sky = use_native_reconstruction;
        input.transparent_guides_enabled = !use_native_reconstruction && pending_effects.transparency;
        input.transparent_depth = {hero_outputs.depth, hero_outputs.depth_resource};
        input.transparent_motion = {hero_outputs.motion, hero_outputs.motion_resource};
        input.transparent_classification = {hero_outputs.classification, hero_outputs.classification_resource};
        input.transparent_metadata = {hero_outputs.metadata, hero_outputs.metadata_resource};
        input.camera = impl_->sdk_camera; input.render_active = impl_->view_state.render_active;
        input.frame_slot = back_buffer_index; input.completed = impl_->graph_queue_executor.Completed();
        input.paper_white_nits = impl_->display_state.paper_white_nits;
        input.peak_nits = impl_->display_state.peak_nits;
#if defined(HS_DEVELOPMENT_TOOLS)
        if (impl_->config.devtools_visible)
            input.draw_devtools = [&](ID3D12GraphicsCommandList *list) -> Result {
                ID3D12DescriptorHeap *heaps[]{impl_->imgui_heap.Get()}; list->SetDescriptorHeaps(1, heaps);
                ImGui_ImplDX12_RenderDrawData(imgui_draw_data, list);
                return Result::Success();
            };
#endif
        PresentationGuidesOutputs output;
        if (auto registered = impl_->presentation_guides.Register(impl_->graph, input, output,
                900, 901, 902); !registered) return registered;
        pending_effects.presentation = true;
        const auto source = impl_->sdk_presentation.PresentationOwner() == SdkPresentationOwner::Fsr
            ? output.hudless : output.composite;
        auto copy = impl_->graph.AddPass(903, "SDK Backbuffer Copy", QueueHint::Direct);
        copy.Read(source.version, Access::CopySource);
        back_buffer_version = copy.Write(back_buffer_version, Access::CopyDest);
        copy.SetExecute([source, destination = impl_->back_buffers[back_buffer_index]](RenderPassContext &context) {
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyResource(destination.Get(), source.resource);
        });
        SdkPresentationFrame packet;
        packet.identity = impl_->sdk_frame_identity; packet.camera = impl_->sdk_camera;
        packet.minimum_luminance = impl_->display_state.min_luminance_nits;
        if (reconstruction_outputs.owner) packet.owners.push_back(reconstruction_outputs.owner);
        if (auto tagged = impl_->presentation_guides.RegisterTagPass(impl_->graph, output,
                {back_buffer_version, impl_->back_buffers[back_buffer_index].Get()}, packet,
                impl_->sdk_presentation, 904); !tagged) return tagged;
    }

    impl_->graph.SetFinalAccess(particles_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_history_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_previous_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(ribbon_arguments_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_input_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_output_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_dead_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_counters_version, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(indirect_arguments_version, Access::UnorderedWrite);
    if (have_skinned_instances)
        impl_->graph.SetFinalAccess(skin_cache_version, Access::ShaderRead);
    if (have_skinned_instances && previous_skin_frame_valid)
        impl_->graph.SetFinalAccess(previous_skin_cache_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(static_visible_refs_version, Access::UnorderedWrite);
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
    impl_->graph.SetFinalAccess(shadow_version, Access::DepthWrite);
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
    impl_->graph.SetFinalAccess(subsurface_diffuse_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(environment_specular_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(indirect_diffuse_version, Access::ShaderRead);
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
            for (const auto version : upload.versions)
                impl_->graph.SetFinalAccess(version, Access::Common);
    impl_->graph.SetFinalAccess(ui_version, Access::ShaderRead);
    impl_->graph.SetFinalAccess(back_buffer_version, Access::Present);

    auto critical_path_begin = impl_->graph.AddPass(37, "RenderCriticalPathBegin", QueueHint::Direct);
    critical_path_begin.SetExecute([](RenderPassContext &) {});
    auto critical_path_end = impl_->graph.AddPass(38, "RenderCriticalPathEnd", QueueHint::Direct);
    critical_path_end.SetExecute([](RenderPassContext &) {});
    for (std::uint32_t pass = 0; pass < impl_->graph.PassCount(); ++pass)
    {
        const auto id = impl_->graph.DeclaredPassId(pass);
        if (id == 37 || id == 38) continue;
        impl_->graph.AddOrderingDependency(37, id);
        impl_->graph.AddOrderingDependency(id, 38);
    }

    if (auto prepared = impl_->graph.Prepare(transient_pool); !prepared) return prepared;
    if (auto prepared = impl_->PrepareFrameTimestamps(frame, impl_->graph.PassCount()); !prepared)
        return prepared;
    frame.timestamp_passes.clear();
    frame.timestamp_frequencies.clear();
    for (std::uint32_t slot = 0; slot < impl_->graph.PassCount(); ++slot)
    {
        const auto pass_id = impl_->graph.CompiledPassId(slot);
        const auto queue = impl_->graph.CompiledPassQueue(slot);
        const auto pass_name = impl_->graph.CompiledPassName(slot);
        if (pass_name.empty())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "RenderGraph compiled an unnamed diagnostic pass.");
        const auto frequency = impl_->graph_queue_executor.TimestampFrequency(queue);
        frame.timestamp_passes.push_back({pass_id, std::string(pass_name),
            static_cast<RenderQueue>(queue), 0, frequency != 0});
        frame.timestamp_frequencies.push_back(frequency);
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
        (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
         kVfxDistortionDescriptorOffset) * impl_->srv_stride;
    impl_->device->CreateShaderResourceView(distortion_resource,&distortion_srv_description,distortion_cpu);
    if (impl_->config.bloom)
    {
        // WaitForFrame completed this slot's prior GPU work before releasing claims;
        // the resolved pool resources and these descriptors stay alive through
        // the current submission and are rewritten only when this slot returns.
        auto bloom_cpu = impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        bloom_cpu.ptr += (static_cast<SIZE_T>(back_buffer_index) * kTextureDescriptorCount +
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
    impl_->PublishFrameDescriptors(back_buffer_index);
    GraphExecutionOptions execution_options;
    execution_options.timestamp_query_heap = frame.timestamp_heap.Get();
    execution_options.timestamp_readback = frame.timestamp_readback.resource.Get();
    execution_options.task_executor = &impl_->config.task_executor;
    auto &submitted_completion = pending_effects.completion;
    if (auto marked = presented_view ? impl_->sdk_presentation.Marker(impl_->sdk_frame_identity, SdkLatencyMarker::RenderSubmitStart) : Result::Success(); !marked)
        return marked;
    // A new submission may overwrite this view's HDR source. Failed partial
    // work must not expose it as the previously completed image.
    impl_->hdr_capture_valid = false;
    impl_->hdr_capture_source.Reset();
    if (auto graph_result = impl_->graph_queue_executor.Execute(
            impl_->graph, back_buffer_index,
            impl_->enhanced ? BarrierMode::Enhanced : BarrierMode::Legacy,
            submitted_completion, &execution_options);
        !graph_result)
    {
        // Partial submission cannot publish CPU history or permit storage reuse.
        // Retain every resource until Drain establishes completion for teardown.
        frame.completion.Merge(submitted_completion);
        impl_->texture_feedback.MarkSubmitted(back_buffer_index, submitted_completion);
        if (submitted_completion.direct || submitted_completion.compute || submitted_completion.copy ||
            impl_->texture_streaming.HasStartedWork())
        {
            impl_->render_package_id = impl_->active_render_package_id;
            impl_->static_scene_fence_unreliable = true;
            impl_->frame_submission_faulted = true;
        }
        impl_->temporal_history_valid = false;
        impl_->hzb_history_valid = false;
        impl_->MarkClusterFrameUse(back_buffer_index, submitted_completion);
        for (auto &skin_frame : impl_->frames) skin_frame.skin_cache_valid = false;
        return graph_result;
    }
    const auto submit_qpc = RendererDiagnosticsCounter();
    // A submitted package identity is consumed even if a later auxiliary view
    // or post-submit service fails. Only rendered submissions advance it.
    impl_->render_package_id = impl_->active_render_package_id;
    frame.submitted_frame_id = ++impl_->rendered_submission_count;
    frame.submitted_view_id = impl_->active_view_id.value;
    frame.completion = submitted_completion;
    impl_->texture_streaming.MarkSubmitted(submitted_completion, impl_->streaming);
    impl_->texture_feedback.MarkSubmitted(back_buffer_index, submitted_completion);
    for (const auto &binding : texture_bindings)
        impl_->texture_streaming.MarkUse(binding.texture, impl_->graph.ResourceCompletion(binding.sampled.resource));
    if (pending_effects.adaptive)
    {
        if (auto completed = impl_->adaptive_shading.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.adaptive = false;
    }
    if (pending_effects.decal_surface)
    {
        if (auto completed = impl_->decal_surface.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.decal_surface = false;
    }
    if (auto marked = presented_view ? impl_->sdk_presentation.Marker(impl_->sdk_frame_identity, SdkLatencyMarker::RenderSubmitEnd) : Result::Success(); !marked)
        return marked;
    if (pending_effects.presentation)
    {
        if (auto submitted = impl_->sdk_presentation.NotifySubmission(impl_->sdk_frame_identity,
                impl_->graph_queue_executor.NativeQueue(QueueHint::Direct),
                impl_->graph_queue_executor.NativeFence(QueueHint::Direct), submitted_completion.direct); !submitted) return submitted;
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
        if (auto completed = impl_->volumetrics.CompleteSubmission(impl_->graph, submitted_completion, true); !completed) return completed;
        pending_effects.volumetrics = false;
        impl_->previous_volumetric_camera = volumetric_camera;
    }
    if (pending_effects.screen_overlay)
    {
        if (auto completed = impl_->screen_overlay.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.screen_overlay = false;
    }
    if (pending_effects.lights)
    {
        pending_effects.lights = false;
        if (auto completed = impl_->light_list.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.ray_omm)
    {
        if (auto completed=impl_->ray_omm.CompleteSubmission(impl_->graph,submitted_completion,true);!completed)return completed;
        pending_effects.ray_omm=false;
    }
    if (pending_effects.ray_scene)
    {
        pending_effects.ray_scene = false;
        if (auto completed = impl_->ray_scene.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.ray_geometry)
    {
        pending_effects.ray_geometry = false;
        if (auto completed = impl_->ray_geometry.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.ray_ambient)
    {
        pending_effects.ray_ambient = false;
        if (auto completed = impl_->ray_ambient_occlusion.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.ray_transport)
    {
        if (auto completed = impl_->ray_transport.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.ray_transport = false;
    }
    if (pending_effects.ray_reconstruction)
    {
        if (auto completed = impl_->ray_reconstruction.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.ray_reconstruction = false;
    }
    if (pending_effects.brixelizer)
    {
        if (auto completed = impl_->brixelizer_gi.CompleteSubmission(impl_->graph,submitted_completion,true); !completed) return completed;
        pending_effects.brixelizer=false;impl_->brixelizer_history_valid=true;
    }
    if (pending_effects.shadows)
    {
        if (auto completed = impl_->virtual_shadows.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
        pending_effects.shadows = false;
    }
    if (pending_effects.local_shadows)
    {
        if (auto completed = impl_->local_shadows.CompleteSubmission(impl_->graph, submitted_completion, true); !completed) return completed;
        pending_effects.local_shadows = false;
    }
    if ((primary_sun && impl_->virtual_shadows.AllocatedBytes() != 0) || impl_->local_shadows.AllocatedBytes() != 0)
        impl_->previous_shadow_casters = std::move(current_shadow_casters);
    if (pending_effects.ray_shadows)
    {
        if (auto completed = impl_->ray_shadows.CompleteSubmission(impl_->graph, submitted_completion, true); !completed) return completed;
        pending_effects.ray_shadows = false;
    }
    impl_->render_submission_time = cpu_recording_start;
    if (pending_effects.hdr_post)
    {
        pending_effects.hdr_post = false;
        if (auto completed = impl_->hdr_post.CompleteSubmission(impl_->graph,
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
        if (auto completed = impl_->hero_transparency.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.diffusion)
    {
        pending_effects.diffusion = false;
        if (auto completed = impl_->subsurface.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.reflections)
    {
        pending_effects.reflections = false;
        if (auto completed = impl_->screen_reflections.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.ambient)
    {
        pending_effects.ambient = false;
        if (auto completed = impl_->ambient_occlusion.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (pending_effects.reconstruction)
    {
        pending_effects.reconstruction = false;
        if (auto completed = impl_->reconstruction_guides.CompleteSubmission(impl_->graph,
                submitted_completion, true); !completed) return completed;
    }
    if (auto committed = impl_->CommitClusterFrameGraph(back_buffer_index, cluster_graph,
            submitted_completion); !committed) return committed;
    if (auto committed = impl_->CommitHlodFrameGraph(back_buffer_index, hlod_graph, submitted_completion); !committed) return committed;
    pending_hlod.completed = true;
    frame.cpu_recording_ms = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - cpu_recording_start).count();
    frame.cpu_recording_ms_valid = std::isfinite(frame.cpu_recording_ms) && frame.cpu_recording_ms > 0;
    if (use_visibility) impl_->visibility_targets_initialized = true;
    for (const auto &[resource, handle] : imported_resources)
    {
        resource->graph_access = impl_->graph.FinalAccess(handle);
        resource->graph_queue = impl_->graph.FinalQueue(handle);
        resource->graph_completion = impl_->graph.ResourceCompletion(handle);
    }
    impl_->back_buffer_completion[back_buffer_index] = impl_->graph.ResourceCompletion(back_buffer);
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
        stream.upload_completion = {};
        for (const auto buffer : upload.buffers)
            stream.upload_completion.Merge(
                impl_->graph.ResourceCompletion(buffer));
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
    impl_->MarkEnvironmentStreamingUse(back_buffer_index, submitted_completion);
    transient_pool.SetClaimCompletion(submitted_completion);
    frame.timestamps_recorded = true;
    frame.ui_initialized = true;
    impl_->ui_dirty[back_buffer_index] = false;
    impl_->particles_initialized = true;
    impl_->particle_input_is_a = !impl_->particle_input_is_a;
    impl_->last_particle_tick = snapshot.header.tick;
    if (new_session)
        for (auto &skin_frame : impl_->frames) skin_frame.skin_cache_valid = false;
    if (have_skinned_instances)
        frame.skin_cache_use_completion = impl_->graph.ResourceCompletion(skin_cache_buffer);
    if (have_skinned_instances && previous_skin_frame_valid)
        previous_skin_frame->skin_cache_use_completion.Merge(
            impl_->graph.ResourceCompletion(previous_skin_cache_buffer));
    if (have_skinned_instances) frame.skin_cache_vertex_read = true;
    if (have_skinned_instances) frame.skin_cache_bounds_read = true;
    frame.skin_cache_valid = have_skinned_instances;
    impl_->dynamic_scene_extractor.Commit();
    impl_->dynamic_scene_generation = scene_generation_after_submit;
    impl_->dynamic_scene_slots.Commit();
    std::shared_ptr<Impl::CanonicalSkinFrame> current_skin_deformation;
    if (have_skinned_instances)
    {
        if (reuse_canonical_skin) current_skin_deformation = impl_->canonical_skin_current;
        else
        {
            current_skin_deformation = std::make_shared<Impl::CanonicalSkinFrame>();
            auto &canonical = *current_skin_deformation;
            canonical.skin_cache_vertices = Impl::RetainViewAllocation(frame.skin_cache_vertices);
            canonical.skin_cache_bounds = Impl::RetainViewAllocation(frame.skin_cache_bounds);
            canonical.skin_cache_vertex_capacity = frame.skin_cache_vertex_capacity;
            canonical.skin_cache_bound_capacity = frame.skin_cache_bound_capacity;
            canonical.skin_cache_slot_stride = frame.skin_cache_slot_stride;
            canonical.skin_cache_valid = true;
            canonical.package_id = impl_->active_render_package_id; canonical.signature = skin_signature;
            impl_->canonical_skin_frames.push_back(current_skin_deformation);
        }
        current_skin_deformation->skin_cache_use_completion.Merge(frame.skin_cache_use_completion);
        impl_->canonical_skin_current = current_skin_deformation;
    }
    impl_->render_session_id = snapshot.header.session_id;
    impl_->last_presented_index = back_buffer_index;
    impl_->transient_textures_common = false;
    impl_->material_sidecars_initialized = true;
    impl_->view_targets_initialized = true;
    impl_->exposure_initialized = true;
    impl_->exposure_read_index = exposure_write_index;
    impl_->exposure_sample_time = now;
    if (initialize_temporal_targets) impl_->temporal_targets_initialized = true;
    impl_->dynamic_instances_shader_read = true;
    impl_->dynamic_instance_slot_count = required_slots;
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
            << (impl_->frame_number + 1) << ',' << instance_count << ','
            << dynamic_instance_upload_bytes << ',' << dirty_instance_ranges.size() << ','
            << dynamic_scene_layout.total_bytes << ','
            << impl_->dynamic_scene_slots.ActiveCount() << ','
            << impl_->dynamic_scene_slots.SlotCount() << ','
            << impl_->dynamic_scene_slots.AnonymousCount() << ','
            << scene_delta.created << ',' << scene_delta.updated << ','
            << scene_delta.destroyed.size() << ','
            << impl_->skin_cache.ActiveCount() << ','
            << impl_->skin_cache.HistoryValidCount() << ','
            << skin_gpu_dispatches << ',' << skin_gpu_history_reads << ','
            << dynamic_visibility_ref_count << ','
            << (impl_->hzb_history_valid ? 1 : 0) << '\n';
    }
    if (static_scene && impl_->static_gpu_scene)
    {
        impl_->static_gpu_scene->last_completion =
            impl_->graph.ResourceCompletion(static_instances_buffer);
        impl_->static_gpu_scene->last_completion.Merge(
            impl_->static_gpu_scene->lod_states.states.graph_completion);
        if (upload_static_scene)
        {
            impl_->static_gpu_scene->submitted = true;
            impl_->static_gpu_scene->upload_completion =
                impl_->graph.ResourceCompletion(static_instances_buffer);
            ++impl_->static_scene_upload_count;
        }
    }
    const auto present_begin_qpc = presented_view ? RendererDiagnosticsCounter() : 0;
    const auto presented = presented_view ? impl_->sdk_presentation.Present(impl_->swap_chain.Get(), impl_->sdk_frame_identity,
        impl_->config.vsync ? 1 : 0, 0) : Result::Success();
    if (presented_view) impl_->native_input_phase = 0;
    if (!presented)
    {
        const auto device_reason = impl_->device->GetDeviceRemovedReason();
        if (FAILED(device_reason)) return impl_->CheckDevice(device_reason, "SDK Present");
        return presented;
    }

    impl_->diagnostics.RecordRenderedFrame(frame.submitted_frame_id, submit_qpc, present_begin_qpc,
        presented_view ? RendererDiagnosticsCounter() : 0, LastQueueStats(), impl_->DiagnosticsMemory(),
        impl_->config.validation, impl_->config.gpu_validation, presented_view, impl_->active_view_id.value);
    impl_->previous_scene_origin = view_input.scene_origin;
    impl_->previous_view_origin = current_view_origin;
    impl_->previous_origins_valid = true;
    impl_->previous_skin_deformation = current_skin_deformation;
    if (!impl_->canonical_instance_current || impl_->canonical_instance_current->package_id != impl_->active_render_package_id)
    {
        auto submitted = std::make_shared<Impl::SubmittedInstanceFrame>();
        const auto instances = impl_->dynamic_scene_slots.Instances();
        const auto metadata = impl_->dynamic_scene_slots.Metadata();
        submitted->instances.assign(instances.begin(), instances.end());
        submitted->generations.reserve(metadata.size());
        for (const auto &slot : metadata) submitted->generations.push_back(slot.generation);
        submitted->scene_origin = view_input.scene_origin;
        submitted->package_id = impl_->active_render_package_id;
        impl_->canonical_instance_current = std::move(submitted);
    }
    impl_->previous_instances = impl_->canonical_instance_current;
    impl_->skin_cache.Commit();
    auto &view_owner = *impl_->owned_views.at(impl_->active_view_id.value);
    if (view_scene_changed)
    {
        ++view_owner.history_generation;
        impl_->RecordViewHistoryKeys(impl_->active_view_id);
    }
    view_owner.scene_lease = static_scene;
    impl_->hdr_capture_source = use_sr_reconstruction
        ? impl_->reconstruction_guides.RetainPreExposedOutput()
        : use_dlss_rr ? impl_->ray_reconstruction.RetainPreExposedOutput()
        : Impl::RetainViewAllocation(impl_->temporal_history[temporal_write_index]);
    impl_->hdr_capture_extent = {impl_->width, impl_->height};
    impl_->hdr_capture_pre_exposed = true;
    impl_->hdr_capture_submission_id = frame.submitted_frame_id;
    impl_->hdr_capture_package_id = impl_->active_render_package_id;
    impl_->hdr_capture_snapshot_tick = snapshot.header.tick;
    impl_->hdr_capture_valid = static_cast<bool>(impl_->hdr_capture_source.resource);
    impl_->view_session_id = snapshot.header.session_id;
    if (!use_native_reconstruction) impl_->temporal_read_index = temporal_write_index;
    impl_->temporal_history_valid = temporal_enabled;
    impl_->hzb_read_index = hzb_write_index;
    impl_->hzb_history_valid = true;
    impl_->previous_view_projection = constants->view_projection;
    impl_->previous_unjittered_view_projection = constants->current_unjittered_view_projection;
    impl_->previous_projection_depth = {lod_projection._33, lod_projection._34,
        lod_projection._43, lod_projection._44};
    impl_->previous_jitter_uv = {constants->surface_jitter_uv.x, constants->surface_jitter_uv.y};
    impl_->previous_geometry_page_versions.fill(0);
    for (const auto &record : frame.geometry_draw_records_cpu)
        if (record.instance_count != 0 && record.page_slot < kGeometryPageCapacity)
            impl_->previous_geometry_page_versions[record.page_slot] = record.geometry_generation;
    if (impl_->cluster_runtime)
        for (const auto &page : impl_->cluster_runtime->frames[back_buffer_index].snapshot)
            if (page.resident && page.physical_page_slot < kGeometryPageCapacity)
                impl_->previous_geometry_page_versions[page.physical_page_slot] = page.page_version;
    impl_->temporal_previous_eye = {current_temporal_eye.x,
                                    current_temporal_eye.y,
                                    current_temporal_eye.z};
    impl_->temporal_previous_fov = snapshot.camera.vertical_fov_degrees;
    ++impl_->frame_number;
    frame_result = {impl_->frame_number, snapshot.header.tick,
#if defined(HS_DEVELOPMENT_TOOLS)
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureMouse,
                    impl_->config.devtools_visible && ImGui::GetIO().WantCaptureKeyboard,
#else
                    false, false,
#endif
                    debug_command, debug_value, debug_secondary,
                    static_cast<std::uint32_t>(typed_vfx_events.size()),
                    static_cast<std::uint32_t>(impl_->typed_vfx_persistent_state.Active().size()),
                    ground_ring_count + ground_add_count, flash_count, owner_mesh_count,
                    static_cast<std::uint32_t>(fresnel_draws.size()),
                    static_cast<std::uint32_t>(ribbon_outputs.size()), impl_->ribbon_state.DroppedSources(), constants->vfx_light_count.x, distortion_count, constants->vfx_decal_count.x,
                    impl_->static_scene_upload_count,
                    volumetric_outputs.constant_resource && impl_->config.vfx_quality == VfxQuality::High
                        ? static_cast<UINT>(typed_volumes.size()) : 0u};
    frame_result.identity = {snapshot.header.tick, impl_->active_render_package_id,
        impl_->active_view_id, presented_view ? impl_->sdk_frame_identity.presentation_id : 0, 0, false};
    if (impl_->streamline_session)
        if (const auto token = impl_->streamline_session->NativeFrameToken(impl_->sdk_frame_identity))
        {
            frame_result.identity.provider_token = *token;
            frame_result.identity.provider_token_known = true;
        }
    frame_result.rendered_view = view_input;
    frame_result.rendered_view.render_active = impl_->view_state.render_active;
    frame_result.view_history_generation = impl_->owned_views.at(impl_->active_view_id.value)->history_generation;
    std::memcpy(frame_result.unjittered_view_projection.data(), &constants->current_unjittered_view_projection,
        sizeof(constants->current_unjittered_view_projection));
    if (impl_->render_scene_inputs) impl_->previous_ribbon_eye = ribbon_eye;
    impl_->CountValidationErrors();
    return Result::Success();
}


} // namespace hs
