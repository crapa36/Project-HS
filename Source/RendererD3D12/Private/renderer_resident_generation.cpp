#include "renderer_impl.hpp"
#include "renderer_development_catalog.hpp"
#include "renderer_reconstruction_provider.hpp"

namespace hs
{
namespace
{
Result ResidentReloadFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}
bool SameSidecarLayout(const render_data::MaterialSidecarLayout &a,
                       const render_data::MaterialSidecarLayout &b) noexcept
{
    return a.mask == b.mask && a.word_count == b.word_count &&
           a.plane_count == b.plane_count && a.offsets == b.offsets;
}
}
Result D3D12Renderer::Impl::PrepareResidentServices(Impl &next)
{
    const auto &owner = *owned_views.at(kMainRenderViewId.value);
    auto &candidate_owner = *next.owned_views.at(kMainRenderViewId.value);
    const auto &cfg = next.config;
    const auto capacity = owner.state.render_capacity;
    const CompletionSet unused;
#define HS_PREPARE(expression) if (auto prepared = (expression); !prepared) return prepared
    AdaptiveShadingCandidate adaptive;
    HS_PREPARE(next.PrepareAdaptiveShading(owner, cfg, capacity, adaptive));
    AmbientOcclusionCandidate ambient;
    HS_PREPARE(next.PrepareAmbientOcclusion(owner, cfg, capacity, ambient, adaptive.Status().compute_ready));
    HS_PREPARE(candidate_owner.adaptive_shading.Commit(std::move(adaptive), unused));
    HS_PREPARE(candidate_owner.ambient_occlusion.Commit(std::move(ambient), unused));
    SubsurfaceCandidate diffusion;
    ScreenReflectionsCandidate reflections;
    HS_PREPARE(next.PrepareSurfaceLighting(owner, cfg, capacity, diffusion, reflections));
    HS_PREPARE(candidate_owner.subsurface.Commit(std::move(diffusion), unused));
    HS_PREPARE(candidate_owner.screen_reflections.Commit(std::move(reflections), unused));
    HeroTransparencyCandidate transparency;
    HS_PREPARE(next.PrepareHeroTransparency(owner, cfg, capacity, transparency));
    HS_PREPARE(candidate_owner.hero_transparency.Commit(std::move(transparency), unused));
    LightingProbeCandidate probe;
    HS_PREPARE(next.PrepareLightingProbe(cfg, probe));
    HS_PREPARE(next.lighting_probe.Commit(std::move(probe), unused));
    LtcLutCandidate ltc;
    HS_PREPARE(next.PrepareLtcLut(cfg, ltc));
    HS_PREPARE(next.ltc_lut.Commit(std::move(ltc), unused));
    HdrPostCandidate post;
    HS_PREPARE(next.PrepareHdrPost(owner, cfg, capacity, {owner.width, owner.height}, post));
    HS_PREPARE(candidate_owner.hdr_post.Commit(std::move(post), unused));
    ScreenOverlayCandidate overlay;
    HS_PREPARE(next.PrepareScreenOverlay(cfg, capacity, overlay));
    HS_PREPARE(candidate_owner.screen_overlay.Commit(std::move(overlay), unused));
    DecalSurfaceCandidate decals;
    HS_PREPARE(next.PrepareDecalSurface(cfg, capacity, false, decals));
    HS_PREPARE(candidate_owner.decal_surface.Commit(std::move(decals), unused));
    LightListCandidate lights;
    HS_PREPARE(next.PrepareLightList(owner, cfg, capacity, lights));
    HS_PREPARE(candidate_owner.light_list.Commit(std::move(lights), unused));
    SceneGeometryCandidate geometry;
    HS_PREPARE(next.PrepareSceneGeometry(cfg, geometry));
    HS_PREPARE(next.scene_geometry.Commit(std::move(geometry), unused));
    BrixelizerCandidate brixelizer;
    HS_PREPARE(next.PrepareBrixelizerGi(owner, cfg, owner.state.render_active, brixelizer));
    HS_PREPARE(candidate_owner.brixelizer_gi.Commit(std::move(brixelizer), unused));
    VirtualShadowCandidate virtual_shadow;
    HS_PREPARE(next.PrepareVirtualShadows(owner, cfg, capacity, virtual_shadow));
    HS_PREPARE(candidate_owner.virtual_shadows.Commit(std::move(virtual_shadow), unused));
    AdditionalShadowPrepared shadows;
    HS_PREPARE(next.PrepareAdditionalShadows(owner, cfg, capacity, shadows));
    HS_PREPARE(candidate_owner.local_shadows.Commit(std::move(shadows.local), unused));
    VolumetricPrepared volume;
    HS_PREPARE(next.PrepareVolumetrics(owner, cfg, capacity, volume));
    HS_PREPARE(candidate_owner.volumetrics.Commit(std::move(volume.service), unused));
    next.volumetric_media = std::move(volume.media);
    next.volumetric_revision = volume.revision;
    LocalProbeRuntimeCandidate local_probe;
    HS_PREPARE(next.PrepareLocalProbes(cfg, local_probe));
    HS_PREPARE(next.local_probes.Commit(std::move(local_probe), unused));
    if (owner.reconstruction_provider)
    {
        ReconstructionGuidesCandidate guides;
        HS_PREPARE(next.PrepareReconstructionGuides(owner, cfg, capacity, {owner.width, owner.height}, guides,
            owner.reconstruction_provider->NeedsCompositionGuide()));
        HS_PREPARE(candidate_owner.reconstruction_guides.Commit(std::move(guides), unused));
    }
    if (sdk_presentation.IsEnabled())
    {
        PresentationGuidesCandidate guides;
        HS_PREPARE(next.PreparePresentationGuides(cfg, {owner.width, owner.height}, capacity, owner.display_state, guides));
        HS_PREPARE(next.presentation_guides.Commit(std::move(guides), unused));
    }
#undef HS_PREPARE
    return Result::Success();
}
void D3D12Renderer::Impl::SwapResidentServices(Impl &next) noexcept
{
    auto &owner = *owned_views.at(kMainRenderViewId.value);
    auto &candidate_owner = *next.owned_views.at(kMainRenderViewId.value);
#define HS_SWAP_SERVICE(field) field.SwapState(next.field)
#define HS_SWAP_VIEW_SERVICE(field) owner.field.SwapState(candidate_owner.field)
    quality_profiles.swap(next.quality_profiles);
    HS_SWAP_SERVICE(asset_io);
    HS_SWAP_SERVICE(texture_streaming);
    HS_SWAP_SERVICE(texture_feedback);
    adaptive_material_policies.swap(next.adaptive_material_policies);
    std::swap(texture_streaming_capabilities, next.texture_streaming_capabilities);
    std::swap(environment_texture_stream_ids, next.environment_texture_stream_ids);
    environment_material_texture_stream_ids.swap(next.environment_material_texture_stream_ids);
    std::swap(material_texture_stream_ids, next.material_texture_stream_ids);
    HS_SWAP_VIEW_SERVICE(ambient_occlusion);
    HS_SWAP_VIEW_SERVICE(adaptive_shading);
    HS_SWAP_VIEW_SERVICE(subsurface);
    HS_SWAP_VIEW_SERVICE(screen_reflections);
    HS_SWAP_VIEW_SERVICE(hero_transparency);
    HS_SWAP_SERVICE(lighting_probe);
    HS_SWAP_SERVICE(ltc_lut);
    HS_SWAP_VIEW_SERVICE(hdr_post);
    HS_SWAP_VIEW_SERVICE(screen_overlay);
    HS_SWAP_VIEW_SERVICE(decal_surface);
    decal_material_programs.swap(next.decal_material_programs);
    decal_material_policies.swap(next.decal_material_policies);
    decal_parameter_bases.swap(next.decal_parameter_bases);
    decal_parameter_counts.swap(next.decal_parameter_counts);
    decal_parameter_layouts.swap(next.decal_parameter_layouts);
    HS_SWAP_VIEW_SERVICE(light_list);
    HS_SWAP_SERVICE(scene_geometry);
    HS_SWAP_VIEW_SERVICE(brixelizer_gi);
    std::swap(owner.brixelizer_history_valid, candidate_owner.brixelizer_history_valid);
    HS_SWAP_VIEW_SERVICE(virtual_shadows);
    HS_SWAP_VIEW_SERVICE(local_shadows);
    HS_SWAP_VIEW_SERVICE(volumetrics);
    HS_SWAP_SERVICE(local_probes);
    if (owner.reconstruction_provider) HS_SWAP_VIEW_SERVICE(reconstruction_guides);
    if (sdk_presentation.IsEnabled()) HS_SWAP_SERVICE(presentation_guides);
#undef HS_SWAP_VIEW_SERVICE
#undef HS_SWAP_SERVICE
    volumetric_media.swap(next.volumetric_media);
    std::swap(volumetric_revision, next.volumetric_revision);
}
void D3D12Renderer::Impl::SwapResidentPipelines(Impl &next) noexcept
{
    using std::swap;
#define HS_SWAP_PIPELINE(field) swap(field, next.field)
    HS_SWAP_PIPELINE(root_signature);
    HS_SWAP_PIPELINE(scene_pipeline);
    HS_SWAP_PIPELINE(shadow_pipeline);
    HS_SWAP_PIPELINE(environment_opaque_shadow_pipeline);
    HS_SWAP_PIPELINE(environment_material_scene_pipelines);
    HS_SWAP_PIPELINE(environment_material_shadow_pipelines);
    HS_SWAP_PIPELINE(hero_scene_pipeline);
    HS_SWAP_PIPELINE(hero_visibility_pipeline);
    HS_SWAP_PIPELINE(hero_shadow_pipeline);
    HS_SWAP_PIPELINE(hero_resolve_pipeline);
    HS_SWAP_PIPELINE(hero_translucent_pipeline);
    HS_SWAP_PIPELINE(visibility_pipeline);
    HS_SWAP_PIPELINE(environment_material_visibility_pipelines);
    HS_SWAP_PIPELINE(surface_resolve_pipeline);
    HS_SWAP_PIPELINE(environment_material_resolve_pipelines);
    HS_SWAP_PIPELINE(raster_records_pipeline);
    HS_SWAP_PIPELINE(visibility_clear_pipeline);
    HS_SWAP_PIPELINE(visibility_count_pipeline);
    HS_SWAP_PIPELINE(visibility_prefix_pipeline);
    HS_SWAP_PIPELINE(visibility_scatter_pipeline);
    HS_SWAP_PIPELINE(particle_pipeline);
    HS_SWAP_PIPELINE(ground_ring_pipeline);
    HS_SWAP_PIPELINE(ground_add_pipeline);
    HS_SWAP_PIPELINE(owner_mesh_pipeline);
    HS_SWAP_PIPELINE(vfx_distortion_pipeline);
    HS_SWAP_PIPELINE(vfx_flash_pipeline);
    HS_SWAP_PIPELINE(vfx_sprite_oit_pipeline);
    HS_SWAP_PIPELINE(slime_pipeline);
    HS_SWAP_PIPELINE(fresnel_shell_pipeline);
    HS_SWAP_PIPELINE(particle_compute_pipeline);
    HS_SWAP_PIPELINE(static_visibility_pipeline);
    HS_SWAP_PIPELINE(visibility_retest_pipeline);
    HS_SWAP_PIPELINE(depth_pyramid_init_pipeline);
    HS_SWAP_PIPELINE(depth_pyramid_reduce_pipeline);
    HS_SWAP_PIPELINE(skin_cache_pipeline);
    HS_SWAP_PIPELINE(skin_bounds_pipeline);
    HS_SWAP_PIPELINE(deferred_pipeline);
    HS_SWAP_PIPELINE(composite_pipeline);
    HS_SWAP_PIPELINE(temporal_pipeline);
    HS_SWAP_PIPELINE(pre_exposure_pipeline);
    HS_SWAP_PIPELINE(exposure_clear_pipeline);
    HS_SWAP_PIPELINE(exposure_build_pipeline);
    HS_SWAP_PIPELINE(exposure_reduce_pipeline);
    HS_SWAP_PIPELINE(bloom_extract_pipeline);
    HS_SWAP_PIPELINE(bloom_downsample_pipeline);
    HS_SWAP_PIPELINE(bloom_upsample_pipeline);
    HS_SWAP_PIPELINE(bloom_pipeline);
    HS_SWAP_PIPELINE(tone_map_pipeline);
    HS_SWAP_PIPELINE(outline_pipeline);
    HS_SWAP_PIPELINE(fxaa_pipeline);
    HS_SWAP_PIPELINE(ui_pipeline);
    HS_SWAP_PIPELINE(ribbon_update_pipeline);
    HS_SWAP_PIPELINE(ribbon_args_pipeline);
    HS_SWAP_PIPELINE(ribbon_add_pipeline);
    HS_SWAP_PIPELINE(ribbon_oit_pipeline);
    HS_SWAP_PIPELINE(draw_signature);
    HS_SWAP_PIPELINE(indexed_geometry_signature);
    HS_SWAP_PIPELINE(mesh_geometry_signature);
    HS_SWAP_PIPELINE(cluster_root_signature);
    HS_SWAP_PIPELINE(cluster_count_pipeline);
    HS_SWAP_PIPELINE(cluster_prefix_pipeline);
    HS_SWAP_PIPELINE(cluster_emit_pipeline);
    HS_SWAP_PIPELINE(cluster_occlusion_pipeline);
    HS_SWAP_PIPELINE(cluster_retest_pipeline);
    HS_SWAP_PIPELINE(cluster_scene_pipelines);
    HS_SWAP_PIPELINE(cluster_shadow_pipelines);
    HS_SWAP_PIPELINE(cluster_visibility_pipelines);
    HS_SWAP_PIPELINE(cluster_draw_signature);
    HS_SWAP_PIPELINE(cluster_mesh_signature);
    HS_SWAP_PIPELINE(mesh_pipeline_pairs);
    HS_SWAP_PIPELINE(virtual_shadow_indexed_pipelines);
    HS_SWAP_PIPELINE(virtual_shadow_mesh_pipelines);
    HS_SWAP_PIPELINE(dispatch_signature);
    HS_SWAP_PIPELINE(ribbon_draw_signature);
#undef HS_SWAP_PIPELINE
}
Result D3D12Renderer::Impl::ReloadResidentGeneration(RendererConfig candidate_config)
{
    auto &main_owner = *owned_views.at(kMainRenderViewId.value);
    // Failed preparations can hold submitted uploads. Prove their retirement
    // before allocating another overlapping candidate against the same budget.
    if (!unresolved_resident_candidates.empty())
    {
        if (auto drained = WaitForGpu(); !drained) return drained;
        for (auto &retained : unresolved_resident_candidates)
        {
            if (auto drained = retained->asset_io.DrainSubmitted(retained->streaming, retained->residency); !drained) return drained;
            if (auto retired = retained->scene_geometry.ResetAfterCompletion(graph_queue_executor.Completed()); !retired) return retired;
            for (auto &[id, owner] : retained->owned_views)
                if (auto retired = ResetOwnedViewAfterCompletion(*owner); !retired) return retired;
            retained->resident_utility_submissions.clear();
            retained->material_parameter_upload_pending = false;
            retained->environment_brdf_init_pending = false;
        }
        unresolved_resident_candidates.clear();
    }
    // Reserve the exceptional retention slot before any submission can happen.
    unresolved_resident_candidates.reserve(unresolved_resident_candidates.size() + 1);
    std::unique_ptr<Impl> next;
    bool adopted{};
    const auto reject = [&](Result failure) {
        if (next && (next->asset_io.HasPendingReads() || next->texture_streaming.HasStartedWork() || next->HasUnresolvedResidentUtilityUploads() ||
                     next->material_parameter_upload_pending || next->environment_brdf_init_pending))
            unresolved_resident_candidates.push_back(std::move(next));
        return failure;
    };
    try
    {
    const auto changed_product = [&](std::string_view id) {
        const auto old_digest = config.product_digests.find(std::string(id));
        const auto new_digest = candidate_config.product_digests.find(std::string(id));
        return old_digest == config.product_digests.end() || new_digest == candidate_config.product_digests.end() ||
               old_digest->second != new_digest->second;
    };
    const bool replace_vfx_execution = changed_product("vfx/particle_effects.hsbin") ||
        changed_product("vfx/vfx_program.hsbin") || changed_product("render/texture/vfx_masks.rpkg");
    if (auto prepared = InitializeResidentAssetContext(candidate_config, next); !prepared) return reject(prepared);
    if (auto prepared = next->BuildAdaptiveMaterialPolicies(next->adaptive_material_policies); !prepared) return reject(prepared);
    next->texture_streaming_capabilities = texture_streaming_capabilities;
    if (auto prepared = next->CreatePipeline(next->config); !prepared) return reject(prepared);
    if (auto prepared = PrepareResidentAssetData(*next); !prepared) return reject(prepared);
    if (auto prepared = next->PrepareHardwareTextureStreaming(&asset_io); !prepared) return reject(prepared);
    const std::array sprite_bindings{next->config.bleed_status_sprite, next->config.burn_status_sprite,
        next->config.slow_status_sprite, next->config.mark_status_sprite,
        next->config.trap_sprite, next->config.fire_area_sprite};
    for (const auto &binding : sprite_bindings)
        if (binding.sprite >= next->vfx_sprite_count || !binding.frame_columns || !binding.frame_rows)
            return reject(ResidentReloadFailure("Candidate status sprite bindings exceed the actual resident VFX atlas or have invalid frame dimensions."));
    const bool replace_sidecars = !SameSidecarLayout(material_sidecar_layout, next->material_sidecar_layout);
    auto &candidate_owner = *next->owned_views.at(kMainRenderViewId.value);
    const auto next_auxiliaries = next->RequiredLightingAuxiliaries(next->config);
    const bool replace_auxiliaries = next_auxiliaries != RequiredLightingAuxiliaries(config);
    if (replace_auxiliaries)
        if (auto prepared = next->PrepareLightingAuxiliaries({&candidate_owner.targets.subsurface_diffuse,
                &candidate_owner.targets.environment_specular, &candidate_owner.targets.indirect_diffuse}, main_owner.state.render_capacity,
                next_auxiliaries); !prepared) return reject(prepared);
    if (replace_sidecars)
        if (auto prepared = next->PrepareMaterialSidecars(candidate_owner.targets.material_sidecars,
                main_owner.state.render_capacity.width, main_owner.state.render_capacity.height); !prepared) return reject(prepared);
    if (auto prepared = PrepareResidentServices(*next); !prepared) return reject(prepared);
    if (auto prepared = PrepareResidentPublication(*next); !prepared) return reject(prepared);
#if defined(HS_DEVELOPMENT_TOOLS)
    diagnostics.ObserveTransition(DiagnosticsMemory());
#endif
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory)) ||
        memory.CurrentUsage > memory.Budget)
        return reject(ResidentReloadFailure("Complete resident candidate exceeds the observed native memory budget."));
    std::vector<ID3D12CommandQueue *> queues;
    queues.reserve(3);
    for (const auto hint : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
        if (auto *native = graph_queue_executor.NativeQueue(hint)) queues.push_back(native);
    if (auto drained = sdk_presentation.BeforeReuse(queues); !drained) return reject(drained);
    if (auto drained = WaitForGpu(); !drained) return reject(drained);
    if (auto drained = asset_io.DrainSubmitted(streaming, residency); !drained) return reject(drained);

    // Every candidate utility submission borrows this same Direct queue. This
    // newer proved queue drain also retires earlier failed candidate signals.
    for (auto &retained : unresolved_resident_candidates)
    {
        if (auto drained = retained->asset_io.DrainSubmitted(retained->streaming, retained->residency); !drained) return reject(drained);
        if (auto retired = retained->scene_geometry.ResetAfterCompletion(graph_queue_executor.Completed()); !retired) return reject(retired);
        for (auto &[id, owner] : retained->owned_views)
            if (auto retired = ResetOwnedViewAfterCompletion(*owner); !retired) return reject(retired);
        retained->resident_utility_submissions.clear();
        retained->material_parameter_upload_pending = false;
        retained->environment_brdf_init_pending = false;
    }
    unresolved_resident_candidates.clear();
    resident_utility_submissions.clear();

    if (auto archived = RetireHeroTransparency(main_owner.hero_transparency); !archived) return reject(archived);
    // No allocation, native preparation or fallible Commit below this boundary.
    PublishResidentAssetDescriptors(*next);
    SwapResidentAssetData(*next);
    SwapResidentPipelines(*next);
    SwapResidentServices(*next);
    if (replace_auxiliaries)
    {
        std::swap(main_owner.targets.subsurface_diffuse, candidate_owner.targets.subsurface_diffuse);
        std::swap(main_owner.targets.environment_specular, candidate_owner.targets.environment_specular);
        std::swap(main_owner.targets.indirect_diffuse, candidate_owner.targets.indirect_diffuse);
        for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
            WriteOptionTargetDescriptors(main_owner, frame);
    }
    if (replace_sidecars)
    {
        main_owner.targets.material_sidecars.swap(candidate_owner.targets.material_sidecars);
        main_owner.material_sidecars_initialized = false;
        for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
            WriteMaterialSidecarDescriptors(main_owner, frame);
    }
    if (replace_vfx_execution) particles_initialized = false;
    ++pipeline_revision;
    ++material_parameter_revision;
    baked_generation_material_revision = material_parameter_revision;
    baked_generation_pipeline_revision = pipeline_revision;
    static_gpu_scene.reset();
    InvalidateCanonicalDeformationAfterCompletion();
    main_owner.previous_geometry_page_versions.fill(0);
    main_owner.dynamic_lod_states = {};
    main_owner.temporal_history_valid = false;
    main_owner.hzb_history_valid = false;
    main_owner.previous_volumetric_camera = {};
    using std::swap;
    swap(config, next->config);
    resource_generation_id.swap(next->resource_generation_id);
    swap(resident_generation_lease, next->resident_generation_lease);
    swap(resident_development_catalog, next->resident_development_catalog);
    adopted = true;
    // Old futures/assets live inside next, with their original reader lease,
    // until destruction finishes after the already-proved queue drain.
    if (next)
    {
        const auto completed = graph_queue_executor.Completed();
        const auto retain_on_failure = [&](Result retired, const char *diagnostic) {
            if (retired) return true;
#if defined(HS_DEVELOPMENT_TOOLS)
            OutputDebugStringA(diagnostic);
#else
            static_cast<void>(diagnostic);
#endif
            unresolved_resident_candidates.push_back(std::move(next));
            return false;
        };
        if (!retain_on_failure(next->scene_geometry.ResetAfterCompletion(completed),
                "Adopted resident generation; old scene geometry cleanup retained.\n")) return Result::Success();
        for (auto &[id, owner] : next->owned_views)
            if (!retain_on_failure(ResetOwnedViewAfterCompletion(*owner),
                    "Adopted resident generation; old view service cleanup retained.\n")) return Result::Success();
    }
    return Result::Success();
    }
    catch (const std::exception &error)
    {
        if (adopted)
        {
#if defined(HS_DEVELOPMENT_TOOLS)
            OutputDebugStringA("Adopted resident generation; old cleanup threw and was retained.\n");
#endif
            if (next) unresolved_resident_candidates.push_back(std::move(next));
            return Result::Success();
        }
        if (next && (next->asset_io.HasPendingReads() || next->texture_streaming.HasStartedWork() || next->HasUnresolvedResidentUtilityUploads() ||
                     next->material_parameter_upload_pending || next->environment_brdf_init_pending))
            unresolved_resident_candidates.push_back(std::move(next));
        return ResidentReloadFailure("Resident preparation failed: " + std::string(error.what()));
    }
}
Result D3D12Renderer::ReloadContentGeneration(
    const std::string &generation_id,
    const std::unordered_map<std::string, std::filesystem::path> &product_paths,
    const std::unordered_map<std::string, std::string> &product_digests,
    std::shared_ptr<const void> reader_lease,
    const RendererVfxSpriteBindings *sprite_bindings,
    bool *resident_replaced,
    bool allow_resident_replacement)
{
    if (resident_replaced) *resident_replaced = false;
    if (!impl_->initialized || impl_->frame_submission_faulted || generation_id.empty() ||
        !reader_lease || product_paths.empty() || product_paths.size() != product_digests.size())
        return ResidentReloadFailure("Resident Generation adoption requires an active renderer and complete validated leased products.");
    if (generation_id == impl_->config.content_generation_id) return Result::Success();
    RendererConfig candidate = impl_->config;
    // This API explicitly selects loose developer Generation products.
    candidate.packed_content.reset();
    candidate.packed_bulks = {};
    candidate.packed_textures = {};
    candidate.packed_geometry = {};
    candidate.packed_materials = {};
    candidate.packed_characters = {};
    candidate.packed_animations = {};
    candidate.packed_vfx_mesh_atlas = nullptr;
    candidate.packed_source_revisions = {};
    candidate.packed_revision_bulks = {};
    candidate.selected_texture_feedback = false;
    candidate.content_generation_id = generation_id;
    candidate.content_generation_lease = std::move(reader_lease);
    candidate.product_paths = product_paths;
    candidate.product_digests = product_digests;
    if (sprite_bindings)
    {
        candidate.bleed_status_sprite = sprite_bindings->bleed_status_sprite;
        candidate.burn_status_sprite = sprite_bindings->burn_status_sprite;
        candidate.slow_status_sprite = sprite_bindings->slow_status_sprite;
        candidate.mark_status_sprite = sprite_bindings->mark_status_sprite;
        candidate.trap_sprite = sprite_bindings->trap_sprite;
        candidate.fire_area_sprite = sprite_bindings->fire_area_sprite;
    }
    const auto parameter = [](std::string_view id) {
        const auto matches = [&](std::string_view name) {
            return id == "render/material/" + std::string(name) + "/parameters.rpkg";
        };
        return std::ranges::any_of(kAuthoredMaterialNames, matches) ||
               std::ranges::any_of(kHeroMaterialNames, matches) ||
               std::ranges::any_of(render_data::kDecalMaterialNames, matches);
    };
    bool narrow = !sprite_bindings && product_paths.size() == impl_->config.product_paths.size();
    for (const auto &[id, previous_digest] : impl_->config.product_digests)
    {
        const auto found = product_digests.find(id);
        if (!product_paths.contains(id) || found == product_digests.end()) { narrow = false; break; }
        if (found->second != previous_digest && !id.starts_with("render/shader/") &&
            !parameter(id) && id != "render/lighting/default_probe.rpkg" && id != "render/lighting/default_ltc.rpkg")
        { narrow = false; break; }
    }
    if (narrow)
    {
        auto result = ReloadShaderGeneration(generation_id, product_paths, product_digests);
        if (result) impl_->config.content_generation_lease = std::move(candidate.content_generation_lease);
        return result;
    }
    if (!allow_resident_replacement)
        return ResidentReloadFailure("Scene dependency closure changed; resident adoption requires a newly projected scene.");
    try
    {
        candidate.development_catalog = RendererDevelopmentCatalog::Build(candidate);
        candidate.development_catalog->Bind(candidate);
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "RendererDevelopmentCatalog", error.what());
    }
    auto result = impl_->ReloadResidentGeneration(std::move(candidate));
    if (result && resident_replaced) *resident_replaced = true;
    return result;
}
} // namespace hs
