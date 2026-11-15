#include "renderer_impl.hpp"
#include <hs/render_data/render_package.hpp>
#include "renderer_reconstruction.hpp"
#include "renderer_named_animation.hpp"
#include "renderer_entity_surface.hpp"

namespace hs
{
namespace
{
Result ViewFailure(std::string_view message)
{
    return Result::Failure(ErrorCode::InvalidArgument, "hs_render_view", std::string(message));
}

}

Result D3D12Renderer::Impl::PrepareFramePayload(const RenderPackage &frame_input, PreparedVfxHeroVolumes &hero_volumes)
{
    const auto &input = frame_input;
    const auto &batch = input.vfx.world_batch;
    if (batch.session != input.snapshots.current.header.session_id ||
        (batch.sequence == 0 && (batch.reset_particle_state ||
            !input.vfx.particle_spawns.empty() || !input.vfx.effect_lines.empty())))
        return ViewFailure("Native VFX batch must match the session and identify its emission/reset submission.");
#if defined(HS_DEVELOPMENT_TOOLS) || defined(HS_OFFLINE_PROBE_BAKER)
    if (input.devtools.delta_seconds &&
        (!std::isfinite(*input.devtools.delta_seconds) || *input.devtools.delta_seconds < 0))
        return ViewFailure("Frame delta must be finite and nonnegative.");
#endif
    if (input.vfx.fresnels.size() > 32) return ViewFailure("Too many Fresnel shell commands.");
    const auto fresnel_recipes = config.vfx_fresnel_recipes.Rows();
    for (const auto &source : input.vfx.fresnels)
        if (source.recipe_index >= fresnel_recipes.size())
            return ViewFailure("Fresnel recipe index exceeds its admitted table.");
    const auto mesh_recipes = config.vfx_mesh_recipes.Rows();
    for (const auto &source : input.vfx.meshes)
        if (source.recipe_index >= mesh_recipes.size())
            return ViewFailure("Owned mesh recipe index exceeds its admitted table.");
    prepared_scene_package.additive_flash_count = 0;
    for (const auto &source : input.vfx.flashes)
    {
        if (source.appearance.get().gradient_row >= vfx_gradient_rows)
            return ViewFailure("Flash command exceeds its resident atlas range.");
        prepared_scene_package.additive_flash_count += !source.appearance.get().oit;
    }
    for (const auto &source : input.vfx.ground)
        if (!source.geometry.GapStorageConsistent(input.vfx.ground_gap_angles.size()) ||
            source.gradient_row >= vfx_gradient_rows ||
            (source.geometry.mask_slice != 0xffffffffu && source.geometry.mask_slice >= 12))
            return ViewFailure("Ground command exceeds its gap or resident atlas range.");
    for (const auto &source : input.vfx.distortions)
        if (!source.GapStorageConsistent(input.vfx.distortion_gap_angles.size()))
            return ViewFailure("Distortion command exceeds its gap storage.");
    for (const auto &source : input.vfx.decals)
        if (source.gradient_row >= vfx_gradient_rows ||
            source.texture_slice >= (source.kind == VfxDecalKind::Fracture ? 4u : 8u) ||
            source.kind > VfxDecalKind::Organic)
            return ViewFailure("Decal command exceeds its resident atlas range.");
    for (const auto &source : input.vfx.particle_spawns)
        if (source.mesh_index > 7 ||
            (source.authored_ballistic && source.authored_gradient_row >= vfx_gradient_rows))
            return ViewFailure("Particle command exceeds its resident atlas range.");
    for (const auto &source : input.vfx.screen)
        if (source.gradient_row >= vfx_gradient_rows)
            return ViewFailure("Screen command exceeds its resident gradient atlas.");
    if (auto r = PrepareVfxHeroVolumes(input.vfx.volumes, vfx_gradient_rows, hero_volumes); !r) return r;
    std::uint64_t gaps{};
    for (const auto &ground : input.vfx.ground) gaps += ground.geometry.GapCount();
    for (const auto &distortion : input.vfx.distortions)
        if (distortion.shape == VfxDistortionShape::GappedAnnulus) gaps += distortion.GapCount();
    if (gaps > UINT32_MAX || input.vfx.ground.size() > UINT32_MAX || input.vfx.flashes.size() > UINT32_MAX || input.vfx.meshes.size() > UINT32_MAX)
        return ViewFailure("Authored VFX upload counts exceed their native index capacity.");
    const auto &snapshot = input.snapshots.current;
    const auto &static_scene = snapshot.static_scene;
    const auto render_instances = snapshot.instances;
    auto &prepared_instances = prepared_scene_package.instances;
    prepared_instances.assign(render_instances.begin(), render_instances.end());
    if (!input.vfx.entity_surfaces.empty())
    {
        for (const auto &command : input.vfx.entity_surfaces)
        {
            if (!command.has_retained_instance ||
                std::ranges::any_of(prepared_instances, [&](const auto &instance) {
                    return instance.stable_id == command.render_instance_id;
                })) continue;
            const auto &instance = command.retained_instance;
            if (instance.stable_id != command.render_instance_id ||
                instance.mesh < RenderMesh::BossFiveMinute || instance.mesh > RenderMesh::BossFinal ||
                instance.asset_id.value == 0 || instance.material_id.value == 0 ||
                (instance.animation_clip_id.value == 0 && !command.has_retained_animation_pose))
                return ViewFailure("Entity surface retention requires its actual boss mesh and captured pose.");
            prepared_instances.push_back(instance);
        }
    }
    const auto render_instance_count = prepared_instances.size();
    if (render_instance_count > UINT32_MAX / 12u) return ViewFailure("Dynamic visibility output index overflow.");
    if (auto r = prepared_scene_package.entity_surfaces.Build(
            input.vfx.entity_surfaces, prepared_instances); !r) return r;
    const auto prepare_animation = [&](const RenderInstance &instance) -> Result {
        if (!instance.animation_clip_id.value) return Result::Success();
        if (instance.mesh != RenderMesh::Archer)
            return ViewFailure("Explicit named animation requires a loaded skeleton-bound character extension.");
        const auto key = MakeRenderPoseSampleKey(instance, nullptr);
        if (std::ranges::any_of(prepared_archer_palettes,
                [&](const auto &entry) { return entry.key == key; })) return Result::Success();
        auto &prepared = prepared_archer_palettes.emplace_back();
        prepared.key = key;
        const auto sampled = archer_compiled_animation
            ? SampleNamedCharacterAnimation(*archer_compiled_animation, archer_named_packed_transforms, instance,
                std::span(prepared.bones.data(), archer_bone_count))
            : SampleNamedCharacterAnimation(archer_named_animations, instance, std::span(prepared.bones.data(), archer_bone_count));
        return sampled;
    };
    for (const auto &instance : prepared_instances)
        if (auto r = prepare_animation(instance); !r) return r;
    if (static_scene && (!static_gpu_scene || static_gpu_scene->source != static_scene))
    {
        if (!static_scene->content_generation_id.empty() &&
            static_scene->content_generation_id != resource_generation_id)
            return ViewFailure("Scene generation must match the resident GPU asset generation.");
        std::uint64_t output_count{};
        for (const auto &instance : static_scene->instances)
        {
            const bool environment = render_data::IsEnvironmentMesh(instance.mesh);
            if (!environment && instance.mesh != RenderMesh::Ground && instance.mesh != RenderMesh::DirtPatch)
                return ViewFailure("Unsupported mesh in static scene.");
            output_count += environment ? 3u : 1u;
        }
        if (output_count > UINT32_MAX / 4u) return ViewFailure("Static visibility output index overflow.");
        const auto keys = static_scene->static_keys;
        if (keys.size() != static_scene->instances.size() ||
            !std::ranges::is_sorted(keys, {}, &RenderSceneInstanceKey::stable_id) ||
            std::adjacent_find(keys.begin(), keys.end()) != keys.end())
            return ViewFailure("Static scene does not carry complete unique prepared identities.");
        for (const auto &instance : static_scene->instances)
            if (instance.stable_id && !std::ranges::binary_search(
                    keys, instance.stable_id, {}, &RenderSceneInstanceKey::stable_id))
                return ViewFailure("Static scene identity does not match its prepared owner.");
    }
    std::string ribbon_error;
    if (!ribbon_state.ValidateFrameSources(input.vfx.ribbons.sources, ribbon_error,
            input.vfx.ribbons.outputs))
        return ViewFailure(ribbon_error);
    return Result::Success();
}

Result D3D12Renderer::Impl::ActivateView(ViewId id)
{
    const auto next = owned_views.find(id.value);
    if (next == owned_views.end() || !next->second->ready)
        return ViewFailure("View has no prepared GPU state.");
    if (id == active_view_id) return Result::Success();
    if (cluster_runtime && !next->second->cluster_view.descriptors)
        if (auto prepared = CreateClusterViewResources(next->second->cluster_view); !prepared) return prepared;
    // Per-view native reconstruction contexts retain their own history. Selecting
    // one does not mutate another view's resources or submission-slot descriptors.
    if (streamline_session)
        if (auto selected = streamline_session->SelectReconstructionViewAfterCompletion(next->second->native_viewport_id); !selected) return selected;
    if (fsr_session)
        if (auto selected = fsr_session->SelectReconstructionViewAfterCompletion(next->second->native_viewport_id); !selected) return selected;
    active_view_id = id;
    active_view_role = next->second->role;
    active_native_viewport_id = next->second->native_viewport_id;
    return Result::Success();
}
Result D3D12Renderer::Impl::PrepareDecalSurface(const RendererConfig &candidate_config,
    ViewExtent capacity, bool admitted_work, DecalSurfaceCandidate &candidate) const
{
    // Prepare produces a candidate independently; the selected OwnedView takes
    // ownership when it commits the candidate after its own completion checks.
    return DecalSurfaceSystem{}.Prepare(device.Get(), allocator, root_signature.Get(), pipeline_cache, capacity, kFrameCount,
        admitted_work, [&](std::string_view name, std::vector<std::byte> &bytes) {
            if (candidate_config.packed_content && name == "decal_surface_cs.dxil")
            {
                for (const auto &material : candidate_config.packed_materials)
                {
                    if (material.kind != render_data::MaterialKind::Decal) continue;
                    const auto program = std::ranges::find(material.programs, render_data::MaterialProgramKind::DecalSurfaceCS,
                        &render_data::MaterialProgramDescriptor::kind);
                    if (program != material.programs.end() && program->product_id == "render/shader/decal_surface_cs.dxil")
                    {
                        StartupOperationScope timing(RendererStartupOperation::CheckedBinaryRead);
                        return ReadRendererMaterialProgram(candidate_config, *program, bytes);
                    }
                }
                return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Missing declared decal Surface program.");
            }
            RendererAssetSource path;
            if (auto r = ResolveRendererAsset(candidate_config, "render/shader/" + std::string(name), path); !r) return r;
            return ReadBinary(path, bytes);
        }, candidate);
}


void D3D12Renderer::Impl::InvalidateCanonicalDeformationAfterCompletion() noexcept
{
#if defined(HS_DEVELOPMENT_TOOLS)
    for (auto &[id, view] : owned_views)
    {
        view->hdr_capture_source.Reset();
        view->hdr_capture_valid = false;
    }
#endif
    for (auto &[id, view] : owned_views)
    {
        if (id == active_view_id.value) continue;
        view->previous_skin_deformation.reset();
        view->previous_instances.reset();
        view->skin_history.Clear();
    }
    canonical_skin_current.reset();
    canonical_instance_current.reset();
    canonical_skin_frames.clear();
    for (auto &frame : frames) frame.skin_cache_valid = false;
}

Result D3D12Renderer::Impl::ResetOwnedViewAfterCompletion(OwnedView &view)
{
    view.last_history_frame_slot = UINT32_MAX;
    ResetClusterViewResources(view.cluster_view);
    if (view.reconstruction_provider)
        if (auto r = view.reconstruction_provider->ResetAfterCompletion(); !r) return r;
    if (auto r = view.ambient_occlusion.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.adaptive_shading.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.subsurface.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.screen_reflections.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = RetireLayeredTranslucency(view.translucency); !r) return r;
    if (auto r = view.hdr_post.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.screen_overlay.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.virtual_shadows.Commit(VirtualShadowCandidate{}, graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.local_shadows.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.volumetrics.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.light_list.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.decal_surface.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    if (auto r = view.reconstruction_guides.ResetAfterCompletion(graph_queue_executor.Completed()); !r) return r;
    return Result::Success();
}

Result D3D12Renderer::Impl::ResetOwnedViewsAfterCompletion()
{
    for (auto &[id, view] : owned_views)
        if (auto r = ResetOwnedViewAfterCompletion(*view); !r) return r;
    for (auto &candidate : unresolved_view_candidates)
        if (auto r = ResetOwnedViewAfterCompletion(*candidate); !r) return r;
    unresolved_view_candidates.clear();
    owned_views.clear();
    timing_previous_source_endpoints.clear();
    return Result::Success();
}

RenderViewInput D3D12Renderer::MainViewInput(const CameraView &source) const
{
    const auto &camera = source;
    const auto yaw_degrees = camera.yaw_degrees;
    const auto pitch_degrees = camera.pitch_degrees;
    const auto distance = camera.distance;
    const auto target_height = camera.target.y;
    const auto yaw = DirectX::XMConvertToRadians(yaw_degrees);
    const auto pitch = DirectX::XMConvertToRadians(pitch_degrees);
    const auto direction = DirectX::XMVector3Normalize(DirectX::XMVectorSet(
        std::cos(pitch) * std::sin(yaw), -std::sin(pitch), std::cos(pitch) * std::cos(yaw), 0));
    const auto target = DirectX::XMVectorSet(camera.target.x, target_height,
        camera.target.z, 1);
    DirectX::XMFLOAT3 position{};
    DirectX::XMStoreFloat3(&position, DirectX::XMVectorSubtract(target, DirectX::XMVectorScale(direction, distance)));
    RenderViewInput input;
    input.id = kMainRenderViewId;
    input.camera.position = {position.x, position.y, position.z};
    input.camera.target = {camera.target.x, target_height, camera.target.z};
    input.camera.vertical_fov_degrees = camera.vertical_fov_degrees;
    const auto &main_owner = *impl_->owned_views.at(kMainRenderViewId.value);
    input.output_extent = {main_owner.width, main_owner.height};
    input.render_capacity = main_owner.state.render_capacity;
    input.render_active = main_owner.state.render_active;
    return input;
}


} // namespace hs
