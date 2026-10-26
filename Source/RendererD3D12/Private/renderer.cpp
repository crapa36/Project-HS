#include "renderer_impl.hpp"
#include "renderer_development_catalog.hpp"
#include "renderer_reconstruction.hpp"
#include "renderer_packed_assets.hpp"
#include <new>
#include <tuple>

namespace hs
{
namespace
{
template<class Options>
Result ValidateNativeOptions(const Options &options)
{
    const auto within = [](float value, float minimum, float maximum) {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    };
    const auto quality = [](auto value, auto maximum) { return value <= maximum; };
    if (options.anisotropic_filtering < 1 || options.anisotropic_filtering > 16 ||
        !within(options.bloom_strength, 0, 2) || !within(options.outline_width_px, 0, 4) ||
        !within(options.sharpening, 0, 1) || !within(options.shadow_distance_m, 20, 500) ||
        !within(options.screen_overlay_intensity, 0, 1) ||
        !quality(options.adaptive_shading, GraphicsAutoToggle::On) ||
        !quality(options.geometry_quality, GraphicsQuality::Ultra) ||
        !quality(options.texture_quality, GraphicsQuality::Ultra) ||
        !quality(options.foliage_quality, GraphicsQuality::Ultra) ||
        !quality(options.material_quality, GraphicsQuality::Ultra) ||
        !quality(options.pom_quality, GraphicsOptionalQuality::Ultra) ||
        !quality(options.lighting_quality, GraphicsQuality::Ultra) ||
        !quality(options.ao_mode, GraphicsAoMode::GTAO) ||
        !quality(options.ao_quality, GraphicsQuality::Ultra) ||
        !quality(options.reflection_mode, GraphicsReflectionMode::ScreenSpace) ||
        !quality(options.reflection_quality, GraphicsQuality::Ultra) ||
        !quality(options.transparency_quality, GraphicsQuality::Ultra) ||
        !quality(options.refraction_quality, GraphicsOptionalQuality::Ultra) ||
        !quality(options.volumetric_quality, GraphicsOptionalQuality::Ultra) ||
        !quality(options.motion_blur, GraphicsMotionBlur::High) ||
        !quality(options.gi_mode, GraphicsGiMode::SoftwareHybrid) ||
        !quality(options.gi_quality, GraphicsQuality::Ultra) ||
        !quality(options.shadow_mode, GraphicsShadowMode::Raster) ||
        !quality(options.shadow_quality, GraphicsQuality::Ultra) ||
        options.target_render_fps < 30 || options.target_render_fps > 240 ||
        !within(options.dynamic_min_percent, 25, 100) || !within(options.dynamic_max_percent, 25, 100) ||
        options.dynamic_min_percent > options.dynamic_max_percent)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Renderer options violate the graphics setting ranges.");
    return Result::Success();
}
}

D3D12Renderer::D3D12Renderer() : impl_(std::make_unique<Impl>())
{
}

Result D3D12Renderer::Impl::PrepareScreenOverlay(const RendererConfig &candidate_config,
    ViewExtent capacity, ScreenOverlayCandidate &candidate) const
{
    return screen_overlay.Prepare(device.Get(), allocator, capacity, kFrameCount,
        [&](std::string_view name, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, "render/shader/" + std::string(name), path);
                    !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate_config.screen_overlay_intensity > 0, pipeline_cache, candidate);
}

D3D12Renderer::~D3D12Renderer()
{
    // Failed native retirement must retain the complete device/resource lease.
    // Runtime calls Shutdown explicitly and reports its concrete failure.
    if (!Shutdown()) (void)impl_.release();
}

Result D3D12Renderer::Initialize(const RendererConfig &config)
{
    if (impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer already initialized.");
    }
    StartupCapture startup(impl_->startup_timings, config.capture_timing_stream);
    if (config.packed_content)
    {
        if (auto valid = ValidatePackedRendererAssets(*config.packed_content, config.packed_bulks); !valid) return valid;
        if (auto valid = ValidatePackedTextureCatalog(config); !valid) return valid;
        if (auto valid = ValidatePackedGeometryCatalog(config); !valid) return valid;
        if (auto valid = ValidatePackedMaterialCatalog(config); !valid) return valid;
        if (auto valid = ValidatePackedCharacterCatalog(config); !valid) return valid;
        if (auto valid = ValidatePackedVfxMeshCatalog(config); !valid) return valid;
        if (auto valid = ValidatePackedRevisionCatalog(config); !valid) return valid;
    }
    else if (!config.packed_bulks.empty() || !config.packed_textures.empty() || !config.packed_geometry.empty() ||
        !config.packed_materials.empty() || !config.packed_characters.empty() || !config.packed_animations.empty() ||
        config.packed_vfx_mesh_atlas || !config.packed_source_revisions.empty() || !config.packed_revision_bulks.empty())
        return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Catalog requires its opened pack owner.");
    if (!config.window)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Renderer requires HWND.");
    }
    impl_->auto_fg_cadence_suppressed = false;
    impl_->auto_fg_cadence_sample_id = 0;
    if (auto loaded = MeasureStartupStep(RendererStartupStep::QualityProfiles, [&] { return impl_->LoadQualityProfiles(config); }); !loaded) return loaded;
    RendererConfig native_config = config;
    if (!native_config.packed_content)
    {
        try
        {
            native_config.development_catalog = RendererDevelopmentCatalog::Build(native_config);
            native_config.development_catalog->Bind(native_config);
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidArgument, "RendererDevelopmentCatalog", error.what());
        }
        if (auto valid = ValidatePackedTextureCatalog(native_config); !valid) return valid;
        if (auto valid = ValidatePackedGeometryCatalog(native_config); !valid) return valid;
        if (auto valid = ValidatePackedMaterialCatalog(native_config); !valid) return valid;
        if (auto valid = ValidatePackedCharacterCatalog(native_config); !valid) return valid;
    }
    if (config.requested_graphics)
    {
        if (!ValidateGraphicsSettings(*config.requested_graphics).empty())
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Initial graphics settings request is invalid.");
        const auto options = ProjectRendererOptions(*config.requested_graphics, config, *impl_->quality_profiles);
        ApplyProjectedRendererOptions(native_config, options);
    }
    if (auto validated = MeasureStartupStep(RendererStartupStep::Configuration, [&] { return ValidateNativeOptions(native_config); }); !validated) return validated;
    if (!std::isfinite(native_config.distortion_coverage) || native_config.distortion_coverage < 0 || native_config.distortion_coverage > 1)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "Distortion coverage budget must be finite and within 0..1.");
    if (native_config.vfx_light_count > 32)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", "VFX light budget exceeds 32.");
    impl_->applied_graphics = {};
    impl_->applied_graphics_available = false;
    impl_->last_applied_graphics_request = {};
    impl_->last_applied_graphics_request_available = false;
    impl_->graphics_settings_generation_high_watermark = 0;
    impl_->graphics_settings_status = {};
    impl_->static_scene_fence_unreliable = false;
    impl_->static_scene_upload_count = 0;
    impl_->dynamic_scene_slots.Clear();
    impl_->skin_cache.Clear();
    impl_->previous_geometry_page_versions.fill(0);
    impl_->dynamic_lod_states = {};
    impl_->lod_session_epoch = 0;
    impl_->frame_submission_faulted = false;
    impl_->material_parameter_revision = 0;
    impl_->retired_dynamic_instance_buffers.clear();
    impl_->dynamic_instance_capacity = 0;
    impl_->dynamic_instance_slot_count = 0;
    impl_->dynamic_instances_shader_read = false;
    if (auto result = MeasureStartupStep(RendererStartupStep::Device, [&] { return impl_->CreateDevice(native_config); }); !result)
    {
        return result;
    }
    impl_->resource_generation_id = native_config.content_generation_id;
    impl_->resident_generation_lease = native_config.content_generation_lease;
    impl_->resident_development_catalog = native_config.development_catalog;
    if (auto result = MeasureStartupStep(RendererStartupStep::MaterialSurfaceProfile, [&] { return impl_->LoadMaterialSurfaceProfile(impl_->config); }); !result) return result;
    if (auto result = MeasureStartupStep(RendererStartupStep::AdaptiveMaterialPolicies, [&] { return impl_->BuildAdaptiveMaterialPolicies(impl_->adaptive_material_policies); }); !result) return result;
    for (auto &pool : impl_->transient_pools)
        if (auto result = MeasureStartupStep(RendererStartupStep::TransientPools, [&] { return pool.Initialize(impl_->allocator, impl_->device.Get()); }); !result) return result;
    if (auto result = MeasureStartupStep(RendererStartupStep::SwapchainTargets, [&] { return impl_->CreateSwapChainAndTargets(); }); !result)
    {
        return result;
    }
    if (auto result = MeasureStartupStep(RendererStartupStep::DepthShadow, [&] { return impl_->CreateDepthAndShadow(); }); !result)
    {
        return result;
    }
    if (auto result = MeasureStartupStep(RendererStartupStep::UiTexture, [&] { return impl_->CreateUiTexture(); }); !result)
    {
        return result;
    }
    if (auto result = MeasureStartupStep(RendererStartupStep::PostProcessTargets, [&] { return impl_->CreatePostProcessTargets(); }); !result)
    {
        return result;
    }
    if (auto result = MeasureStartupStep(RendererStartupStep::MainPipeline, [&] { return impl_->CreatePipeline(impl_->config); }); !result)
    {
        return result;
    }
    impl_->pipeline_revision = 1;
    if (auto result = MeasureStartupStep(RendererStartupStep::CharacterGeometryAnimation, [&] { return impl_->CreateGpuData(); }); !result)
    {
        return result;
    }
    if (auto result = MeasureStartupStep(RendererStartupStep::CharacterTextures, [&] { return impl_->CreateCharacterTextures(); }); !result)
    {
        return result;
    }
    if (auto result = MeasureStartupStep(RendererStartupStep::EnvironmentTextures, [&] { return impl_->CreateEnvironmentTextures(); }); !result)
    {
        return result;
    }
    MeasureStartupStep(RendererStartupStep::EnvironmentBrdfDescriptors, [&] { return impl_->WriteEnvironmentBrdfLutDescriptors(); });
    if (auto result = MeasureStartupStep(RendererStartupStep::MaterialParameters, [&] { return impl_->PrepareMaterialParameters(impl_->config, false); }); !result)
        return result;
    MeasureStartupStep(RendererStartupStep::MaterialParameters, [&] { return impl_->CommitMaterialParameters(); });
    impl_->baked_generation_material_revision = impl_->material_parameter_revision;
    impl_->baked_generation_pipeline_revision = impl_->pipeline_revision;
    if (auto result = MeasureStartupStep(RendererStartupStep::EnvironmentMeshes, [&] { return impl_->CreateEnvironmentMeshes(); }); !result)
    {
        return result;
    }
    if (auto prepared = MeasureStartupStep(RendererStartupStep::HardwareTextureStreaming, [&] { return impl_->PrepareHardwareTextureStreaming(); }); !prepared) return prepared;
    if (impl_->sdk_presentation.IsEnabled())
    {
        PresentationGuidesCandidate guides;
        if (auto prepared = MeasureStartupStep(RendererStartupStep::PresentationGuides, [&] { return impl_->PreparePresentationGuides(impl_->config, {impl_->width, impl_->height},
                impl_->view_state.render_capacity, impl_->display_state, guides); }); !prepared) return prepared;
        if (auto committed = MeasureStartupStep(RendererStartupStep::PresentationGuides, [&] { return impl_->presentation_guides.Commit(std::move(guides),
                impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    }
    if (auto result = MeasureStartupStep(RendererStartupStep::ClusterResources, [&] { return impl_->CreateClusterResources(1); }); !result) return result;
    VirtualShadowCandidate virtual_shadows;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::VirtualShadows, [&] { return impl_->PrepareVirtualShadows(impl_->config,
            impl_->view_state.render_capacity, virtual_shadows); }); !prepared) return prepared;
    Impl::AdditionalShadowPrepared additional_shadows;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::AdditionalShadows, [&] { return impl_->PrepareAdditionalShadows(impl_->config,
            impl_->view_state.render_capacity, additional_shadows); }); !prepared) return prepared;
    AdaptiveShadingCandidate adaptive;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::AdaptiveShading, [&] { return impl_->PrepareAdaptiveShading(impl_->config,
            impl_->view_state.render_capacity, adaptive); }); !prepared) return prepared;
    AmbientOcclusionCandidate ambient;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::AmbientOcclusion, [&] { return impl_->PrepareAmbientOcclusion(impl_->config,
            impl_->view_state.render_capacity, ambient, adaptive.Status().compute_ready); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::AdaptiveShading, [&] { return impl_->adaptive_shading.Commit(std::move(adaptive),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    if (auto committed = MeasureStartupStep(RendererStartupStep::AmbientOcclusion, [&] { return impl_->ambient_occlusion.Commit(std::move(ambient),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    SubsurfaceCandidate diffusion;
    ScreenReflectionsCandidate reflections;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::SurfaceLighting, [&] { return impl_->PrepareSurfaceLighting(impl_->config,
            impl_->view_state.render_capacity, diffusion, reflections); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::SurfaceLighting, [&] { return impl_->subsurface.Commit(std::move(diffusion),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    if (auto committed = MeasureStartupStep(RendererStartupStep::SurfaceLighting, [&] { return impl_->screen_reflections.Commit(std::move(reflections),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    HeroTransparencyCandidate transparency;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::HeroTransparency, [&] { return impl_->PrepareHeroTransparency(impl_->config,
            impl_->view_state.render_capacity, transparency); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::HeroTransparency, [&] { return impl_->hero_transparency.Commit(std::move(transparency),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    LightingProbeCandidate probe;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::LightingProbe, [&] { return impl_->PrepareLightingProbe(impl_->config, probe); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::LightingProbe, [&] { return impl_->lighting_probe.Commit(std::move(probe),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    LtcLutCandidate ltc;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::LtcLut, [&] { return impl_->PrepareLtcLut(impl_->config, ltc); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::LtcLut, [&] { return impl_->ltc_lut.Commit(std::move(ltc),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    HdrPostCandidate hdr_post;
    ScreenOverlayCandidate screen_overlay;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::ScreenOverlay, [&] { return impl_->PrepareScreenOverlay(impl_->config, impl_->view_state.render_capacity,
            screen_overlay); }); !prepared) return prepared;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::HdrPost, [&] { return impl_->PrepareHdrPost(impl_->config, impl_->view_state.render_capacity,
            {impl_->width, impl_->height}, hdr_post); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::HdrPost, [&] { return impl_->hdr_post.Commit(std::move(hdr_post),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    if (auto committed = MeasureStartupStep(RendererStartupStep::ScreenOverlay, [&] { return impl_->screen_overlay.Commit(std::move(screen_overlay),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    SceneGeometryCandidate scene_geometry;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::SceneGeometry, [&] { return impl_->PrepareSceneGeometry(impl_->config, scene_geometry); }); !prepared) return prepared;
    BrixelizerCandidate brixelizer;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::BrixelizerGi, [&] { return impl_->PrepareBrixelizerGi(impl_->config, impl_->view_state.render_active, brixelizer); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::SceneGeometry, [&] { return impl_->scene_geometry.Commit(std::move(scene_geometry),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    if (auto committed = MeasureStartupStep(RendererStartupStep::BrixelizerGi, [&] { return impl_->brixelizer_gi.Commit(std::move(brixelizer),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    impl_->brixelizer_history_valid = false;
    LightListCandidate light_list;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::LightList, [&] { return impl_->PrepareLightList(impl_->config, impl_->view_state.render_capacity, light_list); }); !prepared)
        return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::LightList, [&] { return impl_->light_list.Commit(std::move(light_list),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    if (auto committed = MeasureStartupStep(RendererStartupStep::VirtualShadows, [&] { return impl_->virtual_shadows.Commit(std::move(virtual_shadows),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    if (auto committed = MeasureStartupStep(RendererStartupStep::AdditionalShadows, [&] { return impl_->CommitAdditionalShadows(std::move(additional_shadows)); }); !committed) return committed;
    Impl::VolumetricPrepared volumetrics;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::Volumetrics, [&] { return impl_->PrepareVolumetrics(impl_->config, impl_->view_state.render_capacity,
            volumetrics); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::Volumetrics, [&] { return impl_->CommitVolumetrics(std::move(volumetrics)); }); !committed) return committed;
    LocalProbeRuntimeCandidate local_probes;
    if (auto prepared = MeasureStartupStep(RendererStartupStep::LocalProbes, [&] { return impl_->PrepareLocalProbes(impl_->config, local_probes); }); !prepared) return prepared;
    if (auto committed = MeasureStartupStep(RendererStartupStep::LocalProbes, [&] { return impl_->local_probes.Commit(std::move(local_probes),
            impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    if (impl_->reconstruction_provider)
    {
        ReconstructionGuidesCandidate guides;
        if (auto prepared = MeasureStartupStep(RendererStartupStep::ReconstructionGuides, [&] { return impl_->PrepareReconstructionGuides(impl_->config,
                impl_->view_state.render_capacity, {impl_->width, impl_->height}, guides,
                impl_->reconstruction_provider->NeedsCompositionGuide()); }); !prepared) return prepared;
        if (auto committed = MeasureStartupStep(RendererStartupStep::ReconstructionGuides, [&] { return impl_->reconstruction_guides.Commit(std::move(guides),
                impl_->graph_queue_executor.Completed()); }); !committed) return committed;
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    {
    StartupStepScope devtools_timing(RendererStartupStep::Devtools);
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = 1;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(impl_->device->CreateDescriptorHeap(&description,
                                                    IID_PPV_ARGS(&impl_->imgui_heap))))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot create Dear ImGui descriptor heap.");
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    if (config.font_bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        ImGui::DestroyContext();
        return Result::Failure(ErrorCode::InvalidArgument, "hs_devtools", "Font exceeds Dear ImGui byte limit.");
    }
    auto *font_data = ImGui::MemAlloc(config.font_bytes.size());
    if (!font_data)
    {
        ImGui::DestroyContext();
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools", "Cannot allocate Dear ImGui font data.");
    }
    std::memcpy(font_data, config.font_bytes.data(), config.font_bytes.size());
    // The atlas owns its copy; caller font storage may expire after initialization.
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font_data, static_cast<int>(config.font_bytes.size()),
        16.0f, nullptr, ImGui::GetIO().Fonts->GetGlyphRangesKorean());
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(static_cast<HWND>(config.window)))
    {
        ImGui::DestroyContext();
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot initialize Dear ImGui Win32 backend.");
    }
    ImGui_ImplDX12_InitInfo info{};
    info.Device = impl_->device.Get();
    info.CommandQueue = impl_->queue.Get();
    info.NumFramesInFlight = kFrameCount;
    info.RTVFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    info.DSVFormat = kDepthFormat;
    info.SrvDescriptorHeap = impl_->imgui_heap.Get();
    info.LegacySingleSrvCpuDescriptor =
        impl_->imgui_heap->GetCPUDescriptorHandleForHeapStart();
    info.LegacySingleSrvGpuDescriptor =
        impl_->imgui_heap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(&info))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot initialize Dear ImGui D3D12 backend.");
    }
    impl_->imgui_initialized = true;
    }
#endif
    impl_->render_width = impl_->view_state.render_active.width;
    impl_->render_height = impl_->view_state.render_active.height;
    impl_->diagnostics = RendererDiagnosticsRecorder{};
    impl_->pending_gpu_timings.clear();
    impl_->pending_gpu_timing_passes = 0;
    impl_->gpu_timing_stream_overflowed = false;
    impl_->hero_work_pending.clear();
    impl_->hero_work_retired = {};
    for (auto &frame : impl_->frames) frame.timing_stream_published_frame_id = 0;
    impl_->rendered_submission_count = 0;
    impl_->render_package_id = impl_->active_render_package_id = 0;
    impl_->canonical_skin_current.reset();
    impl_->canonical_instance_current.reset();
    impl_->previous_instances.reset();
    impl_->previous_skin_deformation.reset();
    impl_->canonical_skin_frames.clear();
    impl_->view_session_id = 0;
    impl_->active_view_id = kMainRenderViewId;
    impl_->active_view_role = RenderViewRole::Main;
    impl_->active_native_viewport_id = 0;
    impl_->next_native_viewport_id = 1;
    impl_->initialized = true;
    if (native_config.requested_graphics)
        if (auto result = MeasureStartupStep(RendererStartupStep::InitialGraphicsSettings, [&] { return InitializeGraphicsSettings(*native_config.requested_graphics); }); !result)
        {
            impl_->initialized = false;
            return result;
        }
    startup.Complete();
    return Result::Success();
}

void D3D12Renderer::HandleWindowMessage(const NativeWindowMessage &message) noexcept
{
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->imgui_initialized)
    {
        ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(impl_->config.window),
                                       message.message,
                                       static_cast<WPARAM>(message.wparam),
                                       static_cast<LPARAM>(message.lparam));
    }
#else
    (void)message;
#endif
}

Result D3D12Renderer::Resize(std::uint32_t width, std::uint32_t height)
{
    if (!impl_->initialized)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Cannot resize an uninitialized renderer.");
    if (width == 0 || height == 0 ||
        (width == impl_->width && height == impl_->height))
    {
        return Result::Success();
    }
    ReconstructionPlan reconstruction;
    ViewExtent capacity;
    if (auto resolved = ResolveNativeReconstruction(impl_->config, {width, height},
            reconstruction, capacity); !resolved) return resolved;
    std::unique_ptr<ReconstructionProvider> provider;
    if (auto prepared = impl_->PrepareReconstructionProvider(impl_->config.reconstruction_request
            ? &*impl_->config.reconstruction_request : nullptr, impl_->config.dynamic_resolution,
            impl_->config.dynamic_min_percent, impl_->config.dynamic_max_percent, {width, height},
            reconstruction, capacity, provider, &impl_->config); !prepared) return prepared;
    ReconstructionGuidesCandidate guides;
    if (provider)
        if (auto prepared = impl_->PrepareReconstructionGuides(impl_->config, capacity,
                {width, height}, guides, provider->NeedsCompositionGuide()); !prepared) return prepared;
    const auto render_width = capacity.width;
    const auto render_height = capacity.height;
    AdaptiveShadingCandidate adaptive;
    if (auto prepared = impl_->PrepareAdaptiveShading(impl_->config, capacity, adaptive); !prepared)
        return prepared;
    AmbientOcclusionCandidate ambient;
    if (auto prepared = impl_->PrepareAmbientOcclusion(impl_->config, capacity, ambient,
            adaptive.Status().compute_ready); !prepared)
        return prepared;
    SubsurfaceCandidate diffusion;
    ScreenReflectionsCandidate reflections;
    if (auto prepared = impl_->PrepareSurfaceLighting(impl_->config, capacity,
            diffusion, reflections); !prepared) return prepared;
    HeroTransparencyCandidate transparency;
    if (auto prepared = impl_->PrepareHeroTransparency(impl_->config, capacity, transparency); !prepared)
        return prepared;
    HdrPostCandidate hdr_post;
    if (auto prepared = impl_->PrepareHdrPost(impl_->config, capacity, {width, height}, hdr_post); !prepared)
        return prepared;
    ScreenOverlayCandidate screen_overlay;
    if (auto prepared = impl_->PrepareScreenOverlay(impl_->config, capacity, screen_overlay); !prepared)
        return prepared;
    DecalSurfaceCandidate decal_surface;
    if (auto prepared = impl_->PrepareDecalSurface(impl_->config, capacity, false, decal_surface); !prepared) return prepared;
    LightListCandidate light_list;
    if (auto prepared = impl_->PrepareLightList(impl_->config, capacity, light_list); !prepared) return prepared;
    SceneGeometryCandidate scene_geometry;
    if (auto prepared = impl_->PrepareSceneGeometry(impl_->config, scene_geometry); !prepared) return prepared;
    BrixelizerCandidate brixelizer;
    if (auto prepared = impl_->PrepareBrixelizerGi(impl_->config, reconstruction.render_active, brixelizer); !prepared) return prepared;
    VirtualShadowCandidate virtual_shadows;
    if (auto prepared = impl_->PrepareVirtualShadows(impl_->config, capacity,
            virtual_shadows); !prepared) return prepared;
    Impl::AdditionalShadowPrepared additional_shadows;
    if (auto prepared = impl_->PrepareAdditionalShadows(impl_->config, capacity, additional_shadows); !prepared) return prepared;
    Impl::VolumetricPrepared volumetrics;
    if (auto prepared = impl_->PrepareVolumetrics(impl_->config, capacity, volumetrics); !prepared) return prepared;
    Impl::OptionTargets targets;
    Impl::UiResources ui_targets;
    if (auto prepared = impl_->PrepareUiResources(ui_targets, width, height); !prepared)
        return prepared;
    if (auto result = impl_->PrepareOptionTargets(targets, render_width, render_height,
                                                   impl_->config.shadow_resolution,
                                                   impl_->config.bloom, width, height,
                                                   UsesInternalResolveTargets(reconstruction),
                                                   impl_->config.outline,
                                                   impl_->RequiredLightingAuxiliaries(impl_->config)); !result)
    {
        const auto device_reason = impl_->device->GetDeviceRemovedReason();
        if (FAILED(device_reason))
        {
            (void)impl_->CheckDevice(device_reason, "Prepare resized render targets");
            impl_->CountValidationErrors();
            impl_->initialized = false;
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   std::format("{}; device is unusable.", result.Message()));
        }
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               std::string(result.Message()));
    }

    if (auto result = impl_->WaitForGpu(); !result)
    {
        impl_->CountValidationErrors();
        impl_->initialized = false;
        return result;
    }

    if (impl_->sdk_presentation.PresentationOwner() != SdkPresentationOwner::None)
    {
        auto display = impl_->display_state;
        if (auto recreated = impl_->RecreatePresentation(impl_->config, {width, height}, capacity, display); !recreated)
            return recreated;
    }
    else
    {
    // DXGI requires all direct back-buffer references to be released before
    // ResizeBuffers. Keep every other active resource until the new buffers
    // have been acquired; a failed ResizeBuffers leaves those resources valid.
    for (std::size_t index = 0; index < impl_->back_buffers.size(); ++index)
    {
        impl_->back_buffers[index].Reset();
        impl_->back_buffer_contracts[index] = {};
        impl_->back_buffer_contract_devices[index] = nullptr;
    }

    const auto write_back_buffer_rtvs = [&] {
        auto handle = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (const auto &back_buffer : impl_->back_buffers)
        {
            impl_->device->CreateRenderTargetView(back_buffer.Get(), nullptr, handle);
            handle.ptr += impl_->rtv_stride;
        }
    };
    const auto acquire_back_buffers = [&](std::string_view operation) -> Result {
        std::array<ComPtr<ID3D12Resource>, kFrameCount> buffers;
        for (std::uint32_t index = 0; index < kFrameCount; ++index)
        {
            const auto result = impl_->swap_chain->GetBuffer(index, IID_PPV_ARGS(&buffers[index]));
            if (FAILED(result))
                return impl_->CheckDevice(result, operation);
        }
        impl_->back_buffers.swap(buffers);
        write_back_buffer_rtvs();
        return Result::Success();
    };

    const auto resize_result = impl_->swap_chain->ResizeBuffers(
        kFrameCount, width, height, impl_->display_state.format, 0);
    if (FAILED(resize_result))
    {
        const auto failure = impl_->CheckDevice(resize_result, "ResizeBuffers");
        const auto device_reason = impl_->device->GetDeviceRemovedReason();
        if (resize_result == DXGI_ERROR_DEVICE_REMOVED ||
            resize_result == DXGI_ERROR_DEVICE_RESET ||
            resize_result == DXGI_ERROR_DEVICE_HUNG || FAILED(device_reason))
        {
            impl_->CountValidationErrors();
            impl_->initialized = false;
            return failure;
        }
        DXGI_SWAP_CHAIN_DESC1 description{};
        const auto desc_result = impl_->swap_chain->GetDesc1(&description);
        if (FAILED(desc_result) || description.Width != impl_->width ||
            description.Height != impl_->height)
        {
            impl_->CountValidationErrors();
            impl_->initialized = false;
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   std::format("{}; original swap-chain size could not be "
                                               "confirmed (GetDesc1: 0x{:08X}); renderer "
                                               "is unusable.", failure.Message(),
                                               static_cast<std::uint32_t>(desc_result)));
        }
        if (auto recovery = acquire_back_buffers("Recover original swap-chain buffers");
            !recovery)
        {
            impl_->CountValidationErrors();
            impl_->initialized = false;
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   std::format("{}; original swap-chain buffers could not "
                                               "be reacquired: {}; renderer is unusable.",
                                               failure.Message(), recovery.Message()));
        }
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               std::string(failure.Message()));
    }

    if (auto result = acquire_back_buffers("Get resized swap-chain buffer"); !result)
    {
        impl_->CountValidationErrors();
        impl_->initialized = false;
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::format("{}; renderer is unusable.", result.Message()));
    }

    impl_->CommitOptionTargets(targets);
    impl_->CommitUiResources(ui_targets);
    impl_->transient_textures_common = impl_->enhanced;
    impl_->width = width;
    impl_->height = height;
    if (auto committed = impl_->CommitReconstructionProvider(provider, std::move(guides), capacity); !committed)
    {
        // ResizeBuffers has succeeded; old native output dimensions cannot be
        // restored into this swapchain. Report the failed configuration exactly.
        impl_->initialized = false;
        return committed;
    }
    }
    impl_->reconstruction_plan = std::move(reconstruction);
    if (auto committed = impl_->adaptive_shading.Commit(std::move(adaptive),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->ambient_occlusion.Commit(std::move(ambient),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->subsurface.Commit(std::move(diffusion),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->screen_reflections.Commit(std::move(reflections),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->hero_transparency.Commit(std::move(transparency),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->hdr_post.Commit(std::move(hdr_post),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->screen_overlay.Commit(std::move(screen_overlay),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->decal_surface.Commit(std::move(decal_surface),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->light_list.Commit(std::move(light_list),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->scene_geometry.Commit(std::move(scene_geometry),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->brixelizer_gi.Commit(std::move(brixelizer),
            impl_->graph_queue_executor.Completed()); !committed)
    { impl_->initialized = false; return committed; }
    impl_->brixelizer_history_valid = false;
    if (auto committed = impl_->virtual_shadows.Commit(std::move(virtual_shadows),
            impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (auto committed = impl_->CommitAdditionalShadows(std::move(additional_shadows)); !committed) return committed;
    impl_->render_width = impl_->reconstruction_plan.render_active.width;
    if (auto committed = impl_->CommitVolumetrics(std::move(volumetrics)); !committed) return committed;
    impl_->render_height = impl_->reconstruction_plan.render_active.height;
    (void)UpdateViewState(impl_->view_state, {width, height}, {render_width, render_height},
                          impl_->reconstruction_plan.render_active);
    impl_->dynamic_resolution_controller.Reset();
    impl_->temporal_history_valid = false;
    impl_->temporal_read_index = 0;
    return Result::Success();
}

Result D3D12Renderer::ApplyOptions(const RendererOptions &options)
{
    if (!impl_->initialized)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    if (auto validated = ValidateNativeOptions(options); !validated) return validated;
    if (auto validated = impl_->texture_streaming.ValidateConfiguration(
            options.texture_quality, options.texture_budget_mib); !validated) return validated;
    const bool change_texture_policy = options.texture_quality != impl_->config.texture_quality ||
        options.texture_budget_mib != impl_->config.texture_budget_mib;

    auto feature_config = impl_->config;
    // Stage the complete projected configuration before any native prepare or
    // commit work.  The final publication can then move this owned snapshot
    // without allocating after a successful commit.
    ApplyProjectedRendererOptions(feature_config, options);
    const auto previous_features = impl_->ResolveFeatures(impl_->config);
    const auto next_features = impl_->ResolveFeatures(feature_config);

    const auto render_scale = std::clamp(options.render_scale_percent, 50u, 200u);
    const auto shadow_resolution = std::clamp(options.shadow_resolution, 1024u, 2048u);
    ReconstructionPlan reconstruction;
    ViewExtent capacity;
    if (auto resolved = ResolveNativeReconstruction(options, {impl_->width, impl_->height},
            reconstruction, capacity); !resolved) return resolved;
    const bool reuse_provider = impl_->reconstruction_provider &&
        reconstruction.temporal_enabled && options.temporal_reconstruction == impl_->config.temporal_reconstruction &&
        options.reconstruction_request == impl_->config.reconstruction_request &&
        options.dynamic_resolution == impl_->config.dynamic_resolution &&
        options.dynamic_min_percent == impl_->config.dynamic_min_percent &&
        options.dynamic_max_percent == impl_->config.dynamic_max_percent &&
        options.gi_mode == impl_->config.gi_mode && options.reflection_mode == impl_->config.reflection_mode;
    std::unique_ptr<ReconstructionProvider> provider;
    if (reuse_provider)
    {
        reconstruction = impl_->reconstruction_plan;
        capacity = impl_->view_state.render_capacity;
    }
    else if (auto prepared = impl_->PrepareReconstructionProvider(options.reconstruction_request
            ? &*options.reconstruction_request : nullptr, options.dynamic_resolution,
            options.dynamic_min_percent, options.dynamic_max_percent, {impl_->width, impl_->height},
            reconstruction, capacity, provider, &feature_config); !prepared) return prepared;
    const bool replace_provider = !reuse_provider && (impl_->reconstruction_provider || provider);
    ReconstructionGuidesCandidate guides;
    if (provider)
        if (auto prepared = impl_->PrepareReconstructionGuides(impl_->config, capacity,
                {impl_->width, impl_->height}, guides, provider->NeedsCompositionGuide()); !prepared) return prepared;
    const auto recreate_surfaces = capacity != impl_->view_state.render_capacity ||
        shadow_resolution != impl_->config.shadow_resolution ||
        UsesInternalResolveTargets(reconstruction) != UsesInternalResolveTargets(impl_->reconstruction_plan) ||
        (options.bloom || options.outline) != (impl_->config.bloom || impl_->config.outline) ||
        impl_->RequiredLightingAuxiliaries(feature_config) != impl_->RequiredLightingAuxiliaries(impl_->config);
    const auto recreate_pools = recreate_surfaces || options.bloom != impl_->config.bloom;
    const auto adaptive_status = impl_->adaptive_shading.Status();
    const bool recreate_adaptive = capacity != impl_->view_state.render_capacity ||
        previous_features.adaptive_raster_enabled != next_features.adaptive_raster_enabled ||
        previous_features.adaptive_compute_enabled != next_features.adaptive_compute_enabled ||
        !adaptive_status.usable;
    AdaptiveShadingCandidate adaptive;
    if (recreate_adaptive)
        if (auto prepared = impl_->PrepareAdaptiveShading(feature_config, capacity, adaptive); !prepared)
            return prepared;
    const bool adaptive_compute_ready = recreate_adaptive ? adaptive.Status().compute_ready : adaptive_status.compute_ready;
    const auto recreate_ambient = capacity != impl_->view_state.render_capacity ||
        options.ao_quality != impl_->config.ao_quality || next_features.ao_mode != previous_features.ao_mode ||
        options.contact_shadows != impl_->config.contact_shadows ||
        adaptive_compute_ready != adaptive_status.compute_ready;
    AmbientOcclusionCandidate ambient;
    if (recreate_ambient)
    {
        auto candidate_config = feature_config;
        candidate_config.ao_quality = options.ao_quality;
        candidate_config.ao_mode = options.ao_mode;
        candidate_config.contact_shadows = options.contact_shadows;
        if (auto prepared = impl_->PrepareAmbientOcclusion(candidate_config, capacity, ambient,
                adaptive_compute_ready); !prepared)
            return prepared;
    }
    const auto target_width = capacity.width;
    const bool recreate_transparency = capacity != impl_->view_state.render_capacity ||
        options.transparency_quality != impl_->config.transparency_quality ||
        options.refraction_quality != impl_->config.refraction_quality;
    HeroTransparencyCandidate transparency;
    if (recreate_transparency)
    {
        auto candidate_config = feature_config;
        candidate_config.transparency_quality = options.transparency_quality;
        candidate_config.refraction_quality = options.refraction_quality;
        if (auto prepared = impl_->PrepareHeroTransparency(candidate_config, capacity, transparency); !prepared)
            return prepared;
    }
    const bool recreate_surface_lighting = capacity != impl_->view_state.render_capacity ||
        next_features.reflection_mode != previous_features.reflection_mode ||
        options.reflection_quality != impl_->config.reflection_quality;
    SubsurfaceCandidate diffusion;
    ScreenReflectionsCandidate reflections;
    if (recreate_surface_lighting)
    {
        auto candidate_config = feature_config;
        candidate_config.reflection_mode = options.reflection_mode;
        candidate_config.reflection_quality = options.reflection_quality;
        if (auto prepared = impl_->PrepareSurfaceLighting(candidate_config, capacity,
                diffusion, reflections); !prepared) return prepared;
    }
    const auto target_height = capacity.height;
    Impl::OptionTargets targets;
    std::array<TransientResourcePool, kFrameCount> pools;
    if (recreate_surfaces)
    {
        if (auto prepared = impl_->PrepareOptionTargets(targets, target_width, target_height,
            shadow_resolution, options.bloom, impl_->width, impl_->height,
            UsesInternalResolveTargets(reconstruction), options.outline,
            impl_->RequiredLightingAuxiliaries(feature_config)); !prepared) return prepared;
    }
    else if (recreate_pools)
    {
        if (auto prepared = impl_->PrepareOptionPools(pools,
            impl_->view_state.render_capacity.width, impl_->view_state.render_capacity.height,
            options.bloom, impl_->width, impl_->height); !prepared) return prepared;
    }
    const bool recreate_hdr_post = capacity != impl_->view_state.render_capacity ||
        options.motion_blur != impl_->config.motion_blur ||
        options.depth_of_field != impl_->config.depth_of_field ||
        options.color_grading != impl_->config.color_grading;
    const bool recreate_screen_overlay = capacity != impl_->view_state.render_capacity ||
        (options.screen_overlay_intensity > 0) != (impl_->config.screen_overlay_intensity > 0);
    ScreenOverlayCandidate screen_overlay;
    if (recreate_screen_overlay)
        if (auto prepared = impl_->PrepareScreenOverlay(feature_config, capacity, screen_overlay); !prepared)
            return prepared;
    HdrPostCandidate hdr_post;
    if (recreate_hdr_post)
    {
        auto candidate_config = feature_config;
        candidate_config.motion_blur = options.motion_blur;
        candidate_config.depth_of_field = options.depth_of_field;
        candidate_config.color_grading = options.color_grading;
        if (auto prepared = impl_->PrepareHdrPost(candidate_config, capacity,
                {impl_->width, impl_->height}, hdr_post); !prepared) return prepared;
    }
    const bool recreate_decal_surface = capacity != impl_->view_state.render_capacity;
    DecalSurfaceCandidate decal_surface;
    if (recreate_decal_surface)
        if (auto prepared = impl_->PrepareDecalSurface(feature_config, capacity, false, decal_surface); !prepared) return prepared;
    const bool recreate_light_list = capacity != impl_->view_state.render_capacity ||
        options.lighting_quality != impl_->config.lighting_quality;
    const bool recreate_volumetrics = capacity != impl_->view_state.render_capacity ||
        options.volumetric_quality != impl_->config.volumetric_quality ||
        options.vfx_quality != impl_->config.vfx_quality;
    const bool recreate_local_probes =
        (options.gi_mode == GraphicsGiMode::Off && options.reflection_mode == GraphicsReflectionMode::Off) !=
        (impl_->config.gi_mode == GraphicsGiMode::Off && impl_->config.reflection_mode == GraphicsReflectionMode::Off);
    LocalProbeRuntimeCandidate local_probes;
    if (recreate_local_probes)
    {
        auto candidate_config = feature_config;
        candidate_config.gi_mode = options.gi_mode;
        candidate_config.reflection_mode = options.reflection_mode;
        if (auto prepared = impl_->PrepareLocalProbes(candidate_config, local_probes); !prepared) return prepared;
    }
    Impl::VolumetricPrepared volumetrics;
    if (recreate_volumetrics)
    {
        auto candidate_config = feature_config;
        candidate_config.volumetric_quality = options.volumetric_quality;
        candidate_config.vfx_quality = options.vfx_quality;
        if (auto prepared = impl_->PrepareVolumetrics(candidate_config, capacity, volumetrics); !prepared) return prepared;
    }
    LightListCandidate light_list;
    if (recreate_light_list)
        if (auto prepared = impl_->PrepareLightList(feature_config, capacity, light_list); !prepared) return prepared;
    const bool recreate_scene_geometry = next_features.software_gi_enabled != previous_features.software_gi_enabled;
    SceneGeometryCandidate scene_geometry;
    if (recreate_scene_geometry)
        if (auto prepared = impl_->PrepareSceneGeometry(feature_config, scene_geometry); !prepared) return prepared;
    const bool recreate_brixelizer = next_features.software_gi_enabled != previous_features.software_gi_enabled ||
        (next_features.software_gi_enabled && (options.gi_quality != impl_->config.gi_quality ||
            reconstruction.render_active != impl_->view_state.render_active || replace_provider));
    BrixelizerCandidate brixelizer;
    if (recreate_brixelizer)
        if (auto prepared = impl_->PrepareBrixelizerGi(feature_config, reconstruction.render_active, brixelizer); !prepared) return prepared;
    // Apply inherits the selected product maps unchanged. Product adoption
    // prepares this service through ReloadPipeline; only quality/capacity can
    // alter its allocation here because both requested shadow modes use it.
    const bool recreate_virtual_shadows = capacity != impl_->view_state.render_capacity ||
        options.shadow_quality != impl_->config.shadow_quality ||
        options.shadow_mode != impl_->config.shadow_mode;
    const bool recreate_additional_shadows = recreate_virtual_shadows;
    Impl::AdditionalShadowPrepared additional_shadows;
    if (recreate_additional_shadows)
        if (auto prepared = impl_->PrepareAdditionalShadows(feature_config, capacity, additional_shadows); !prepared) return prepared;
    VirtualShadowCandidate virtual_shadows;
    if (recreate_virtual_shadows)
        if (auto prepared = impl_->PrepareVirtualShadows(feature_config, capacity,
                virtual_shadows); !prepared) return prepared;
    impl_->diagnostics.ObserveTransition(impl_->DiagnosticsMemory());
    const auto previous_hdr = impl_->display_state.hdr_active;
    bool presentation_recreated{};
    if (options.presentation_request != impl_->config.presentation_request ||
        (impl_->sdk_presentation.PresentationOwner() != SdkPresentationOwner::None &&
            (capacity != impl_->view_state.render_capacity || options.vsync != impl_->config.vsync ||
             options.hdr_mode != impl_->config.hdr_mode || options.dynamic_resolution != impl_->config.dynamic_resolution ||
             options.display_output != impl_->config.display_output ||
             options.hdr_paper_white_nits != impl_->config.hdr_paper_white_nits || options.hdr_peak_nits != impl_->config.hdr_peak_nits ||
             options.target_render_fps != impl_->config.target_render_fps)))
    {
        DisplayState display;
        if (auto queried = QueryDisplayState(impl_->adapter.Get(), static_cast<HWND>(impl_->config.window),
                feature_config, display); !queried) return queried;
        if (auto recreated = impl_->RecreatePresentation(feature_config, {impl_->width, impl_->height}, capacity, display); !recreated)
            return recreated;
        impl_->config.hdr_mode = options.hdr_mode;
        impl_->config.hdr_paper_white_nits = options.hdr_paper_white_nits;
        impl_->config.hdr_peak_nits = options.hdr_peak_nits;
        impl_->config.display_output = options.display_output;
        presentation_recreated = true;
    }
    if (!presentation_recreated)
        if (auto display = impl_->ApplyDisplayOptions(options); !display) return display;
    if (previous_hdr != impl_->display_state.hdr_active) impl_->temporal_history_valid = false;
    if (change_texture_policy || recreate_pools || recreate_adaptive || recreate_ambient || recreate_surface_lighting || recreate_transparency || replace_provider || recreate_hdr_post || recreate_screen_overlay || recreate_light_list || recreate_scene_geometry || recreate_brixelizer || recreate_virtual_shadows || recreate_additional_shadows || recreate_volumetrics || recreate_decal_surface)
        if (auto drained = impl_->WaitForGpu(); !drained) return drained;
    if (replace_provider)
    {
        if (auto committed = impl_->CommitReconstructionProvider(provider, std::move(guides), capacity); !committed)
            return committed;
        impl_->temporal_history_valid = false;
    }
    if (recreate_adaptive)
        if (auto committed = impl_->adaptive_shading.Commit(std::move(adaptive),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_ambient)
        if (auto committed = impl_->ambient_occlusion.Commit(std::move(ambient),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_surface_lighting)
    {
        if (auto committed = impl_->subsurface.Commit(std::move(diffusion),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
        if (auto committed = impl_->screen_reflections.Commit(std::move(reflections),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    }
    if (recreate_transparency)
        if (auto committed = impl_->hero_transparency.Commit(std::move(transparency),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_hdr_post)
        if (auto committed = impl_->hdr_post.Commit(std::move(hdr_post),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_screen_overlay)
        if (auto committed = impl_->screen_overlay.Commit(std::move(screen_overlay),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_decal_surface)
        if (auto committed = impl_->decal_surface.Commit(std::move(decal_surface),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_light_list)
        if (auto committed = impl_->light_list.Commit(std::move(light_list),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_volumetrics)
        if (auto committed = impl_->CommitVolumetrics(std::move(volumetrics)); !committed) return committed;
    if (recreate_local_probes)
        if (auto committed = impl_->local_probes.Commit(std::move(local_probes),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_scene_geometry)
        if (auto committed = impl_->scene_geometry.Commit(std::move(scene_geometry),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_brixelizer)
    {
        if (auto committed = impl_->brixelizer_gi.Commit(std::move(brixelizer),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
        impl_->brixelizer_history_valid = false;
    }
    if (recreate_virtual_shadows)
        if (auto committed = impl_->virtual_shadows.Commit(std::move(virtual_shadows),
                impl_->graph_queue_executor.Completed()); !committed) return committed;
    if (recreate_additional_shadows)
        if (auto committed = impl_->CommitAdditionalShadows(std::move(additional_shadows)); !committed) return committed;
    if (recreate_surfaces)
    {
        impl_->CommitOptionTargets(targets);
        // Candidate textures start in COMMON with Enhanced barriers. The next
        // frame must import that state even if the previous frame did not.
        impl_->transient_textures_common = impl_->enhanced;
        (void)UpdateViewState(impl_->view_state, {impl_->width, impl_->height},
            {target_width, target_height}, reconstruction.render_active);
        impl_->dynamic_resolution_controller.Reset();
        impl_->temporal_history_valid = false;
        impl_->temporal_read_index = 0;
    }
    else if (recreate_pools)
    {
        impl_->SwapOptionPools(pools);
        impl_->ClearTransientDescriptors();
    }
    if (reconstruction.temporal_enabled != impl_->config.temporal_reconstruction ||
        reconstruction.effective_provider != impl_->reconstruction_plan.effective_provider ||
        reconstruction.effective_quality != impl_->reconstruction_plan.effective_quality)
        impl_->temporal_history_valid = false;
    if (next_features.software_gi_enabled || !options.dynamic_resolution || options.dynamic_resolution != impl_->config.dynamic_resolution ||
        options.dynamic_min_percent != impl_->config.dynamic_min_percent ||
        options.dynamic_max_percent != impl_->config.dynamic_max_percent || replace_provider)
    {
        const auto active = reconstruction.render_active;
        impl_->view_state.render_active = active;
        impl_->render_width = active.width;
        impl_->render_height = active.height;
        impl_->dynamic_resolution_controller.Reset();
    }
    else if (recreate_surfaces)
    {
        impl_->render_width = reconstruction.render_active.width;
        impl_->render_height = reconstruction.render_active.height;
    }
    impl_->reconstruction_plan = std::move(reconstruction);

    if (auto applied = impl_->texture_streaming.Configure(options.texture_quality,
            impl_->QualityProfileFor(options.texture_quality).texture_finest_mip,
            options.texture_budget_mib); !applied) return applied;
    // Every operation above either fails without touching the active resources
    // or completes before the effective options become visible to Render().
    if (options.material_quality != impl_->config.material_quality ||
        options.pom_quality != impl_->config.pom_quality)
    {
        impl_->temporal_history_valid = false;
        impl_->brixelizer_history_valid = false;
    }
    feature_config.temporal_reconstruction = impl_->reconstruction_plan.temporal_enabled;
    feature_config.render_scale_percent = render_scale;
    feature_config.shadow_resolution = shadow_resolution;
    feature_config.particle_percentage = std::min(options.particle_percentage, 100u);
    // All allocating projection work happened before native commit.  Move the
    // fully staged snapshot into the active configuration at the publication
    // boundary, keeping only scalar history/planning updates above.
    if (impl_->config.presentation_request.mode != feature_config.presentation_request.mode)
    {
        impl_->auto_fg_cadence_suppressed = false;
        impl_->auto_fg_cadence_sample_id = 0;
    }
    impl_->config = std::move(feature_config);
    impl_->hdr_capture_valid = false;
    impl_->hdr_capture_source.Reset();
    ++impl_->view_configuration_revision;
    return Result::Success();
}

Result D3D12Renderer::SetFrameGenerationAllowed(bool allowed)
{
    if (!impl_->initialized || impl_->native_input_phase == 1 || impl_->native_input_phase == 2 ||
        (allowed && impl_->native_input_phase))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Frame generation policy changes require an input boundary or a completed simulation disabling FG.");
    if (impl_->config.frame_generation_allowed == allowed) return Result::Success();
    if (impl_->config.presentation_request.mode == GraphicsFrameGenerationMode::Off)
    {
        impl_->config.frame_generation_allowed = allowed;
        return Result::Success();
    }
    try
    {
        auto candidate = impl_->config;
        candidate.frame_generation_allowed = allowed;
        auto display = impl_->display_state;
        if (auto recreated = impl_->RecreatePresentation(candidate, {impl_->width, impl_->height},
                impl_->view_state.render_capacity, display); !recreated) return recreated;
        impl_->config.frame_generation_allowed = allowed;
        return Result::Success();
    }
    catch (const std::bad_alloc &)
    {
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
            "Frame generation policy transition allocation failed.");
    }
}

Result D3D12Renderer::BeginInputFrame(std::uint64_t &identity)
{
    identity = 0;
    if (!impl_->initialized || impl_->native_input_phase)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Input requires an initialized renderer with no outstanding frame token.");
    if (impl_->config.frame_generation_allowed &&
        impl_->config.presentation_request.mode == GraphicsFrameGenerationMode::Auto &&
        impl_->quality_profiles && impl_->sdk_presentation.Status().availability == "available")
    {
        const auto &policy = impl_->quality_profiles->auto_fg_cadence;
        const auto cadence = impl_->diagnostics.RecentCompletedMainCadence(policy.minimum_samples);
        if (cadence.intervals >= policy.minimum_samples && cadence.newest_frame_id > impl_->auto_fg_cadence_sample_id)
        {
            impl_->auto_fg_cadence_sample_id = cadence.newest_frame_id;
            const bool suppress = impl_->auto_fg_cadence_suppressed
                ? cadence.fps < policy.reenable_above_fps : cadence.fps < policy.disable_below_fps;
            if (suppress != impl_->auto_fg_cadence_suppressed ||
                (suppress && impl_->sdk_presentation.IsEnabled()))
            {
                auto candidate = impl_->config;
                auto display = impl_->display_state;
                if (auto recreated = impl_->RecreatePresentation(candidate, {impl_->width, impl_->height},
                        impl_->view_state.render_capacity, display, suppress); !recreated) return recreated;
                impl_->auto_fg_cadence_suppressed = suppress;
                impl_->sdk_presentation.ReportFrameGenerationPolicy(impl_->config.presentation_request, suppress, true);
            }
        }
    }
    else impl_->auto_fg_cadence_suppressed = false;
    if (impl_->sdk_presentation.LatencyOwner() == SdkLatencyOwner::None) return Result::Success();
    impl_->sdk_frame_identity = {impl_->render_package_id + 1, impl_->render_package_id + 1, 0};
    if (auto slept = impl_->sdk_presentation.InputSleep(impl_->sdk_frame_identity); !slept) return slept;
    if (auto marked = impl_->sdk_presentation.Marker(impl_->sdk_frame_identity, SdkLatencyMarker::SimulationStart); !marked) return marked;
    impl_->native_input_phase = 1; identity = impl_->sdk_frame_identity.render_id;
    return Result::Success();
}
Result D3D12Renderer::InputSampled(std::uint64_t identity)
{
    if (!identity || impl_->native_input_phase != 1 || impl_->sdk_frame_identity.render_id != identity)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Sampled input token does not match the pending native frame.");
    if (auto marked = impl_->sdk_presentation.Marker(impl_->sdk_frame_identity, SdkLatencyMarker::InputSample); !marked) return marked;
    impl_->native_input_phase = 2;
    return Result::Success();
}
Result D3D12Renderer::SimulationCompleted(std::uint64_t identity)
{
    if (!identity || impl_->native_input_phase != 2 || impl_->sdk_frame_identity.render_id != identity)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Simulation token does not match the sampled native frame.");
    if (auto marked = impl_->sdk_presentation.Marker(impl_->sdk_frame_identity, SdkLatencyMarker::SimulationEnd); !marked) return marked;
    impl_->native_input_phase = 3;
    return Result::Success();
}
bool D3D12Renderer::UsesNativeFrameLimiter() const noexcept
{
    return impl_ && impl_->sdk_presentation.UsesFrameLimiter();
}
RendererPresentationStatus D3D12Renderer::GetPresentationStatus() const
{
    if (!impl_) return {};
    auto status = impl_->sdk_presentation.Status();
    status.allocated_bytes += impl_->presentation_guides.AllocatedBytes();
    return status;
}

RendererReconstructionStatus D3D12Renderer::GetReconstructionStatus() const
{
    const auto &plan = impl_->reconstruction_plan;
    const auto availability = [](ReconstructionAvailability value) -> std::string {
        switch (value)
        {
        case ReconstructionAvailability::Available: return "available";
        case ReconstructionAvailability::CompiledOut: return "compiled_out";
        case ReconstructionAvailability::Unsupported: return "unsupported";
        case ReconstructionAvailability::Unavailable: return "unavailable";
        case ReconstructionAvailability::NotIntegrated: return "not_integrated";
        }
        return "unavailable";
    };
    const auto native_availability = impl_->reconstruction_provider
        ? impl_->reconstruction_provider->Availability() : plan.requested_availability;
    const auto native_reason = impl_->reconstruction_provider && native_availability != ReconstructionAvailability::Available
        ? std::string(impl_->reconstruction_provider->FailureReason()) : plan.fallback_reason;
    return {plan.requested_provider, plan.effective_provider, plan.requested_quality,
        plan.effective_quality, {impl_->render_width, impl_->render_height},
        {impl_->width, impl_->height}, availability(native_availability),
        native_reason, plan.temporal_enabled};
}

RendererTextureStreamingStatus D3D12Renderer::GetTextureStreamingStatus() const
{
    RendererTextureStreamingStatus status;
    if (!impl_ || !impl_->initialized) { status.reason = "Renderer is not initialized."; return status; }
    status.applied_quality = impl_->config.texture_quality;
    status.applied_budget_mib = impl_->config.texture_budget_mib;
    status.texture_count = impl_->texture_streaming.Count();
    status.physical_bytes = impl_->texture_streaming.PhysicalBytes();
    status.ready = status.texture_count != 0 && !impl_->frame_submission_faulted;
    status.native_asset_io = impl_->asset_io.NativeEnabled();
    for (std::uint32_t index = 1; index <= status.texture_count; ++index)
        status.sampler_feedback = status.sampler_feedback || impl_->texture_streaming.View({index}).reserved;
    if (impl_->frame_submission_faulted) status.reason = "Partial frame submission requires a reliable drain and reinitialization.";
    else if (!impl_->texture_streaming_capabilities.tiled_resources)
        status.reason = "Native sampler feedback is unavailable; environment textures use real reduced mip resources. Hero textures remain pinned.";
    else if (!status.sampler_feedback)
        status.reason = "No native mip tail has published yet; environment fallback resources are active. Hero textures remain pinned.";
    else if (!impl_->texture_streaming_capabilities.packed_mip_arrays)
        status.reason = "Whole-mip hardware residency covers supported single-slice materials; mip arrays use reduced resources because tiled tier4 is unavailable. Hero textures remain pinned.";
    else status.reason = "Whole-mip hardware residency covers supported environment materials and mip arrays. Hero textures remain pinned.";
    return status;
}

RendererLightingStatus D3D12Renderer::GetLightingStatus() const
{
    if (!impl_ || !impl_->initialized) return {};
    const auto &request = impl_->config;
    const auto plan = impl_->ResolveFeatures(request);
    const auto reason = [](RendererFeatureFallbackReason value) -> std::string {
        switch (value)
        {
        case RendererFeatureFallbackReason::None: return {};
        case RendererFeatureFallbackReason::AutoSelection: return "auto_selection";
        case RendererFeatureFallbackReason::UnsupportedHardware: return "unsupported_hardware";
        case RendererFeatureFallbackReason::ConsumerUnavailable: return "consumer_unavailable";
        case RendererFeatureFallbackReason::RequestedOff: return "requested_off";
        }
        return "consumer_unavailable";
    };
    const auto local = impl_->local_shadows.Status();
    const auto software = impl_->brixelizer_gi.Status();
    const auto software_support = QueryBrixelizerNativeSupport(impl_->device.Get());
    const auto adaptive = impl_->adaptive_shading.Status();
    const auto adaptive_reason = [&](bool ready, RendererFeatureFallbackReason fallback) {
        if (request.adaptive_shading == GraphicsAutoToggle::Off) return std::string("requested_off");
        if (ready) return std::string{};
        if (!adaptive.reason.empty()) return adaptive.reason;
        if (fallback == RendererFeatureFallbackReason::None) return std::string("native_consumer_unavailable");
        return reason(fallback);
    };
    RendererLightingStatus status{{request.ao_mode, request.gi_mode, request.reflection_mode, request.shadow_mode},
        {plan.ao_mode, plan.gi_mode, plan.reflection_mode, plan.shadow_mode},
        reason(plan.ao_reason), reason(plan.gi_reason), reason(plan.reflection_reason), reason(plan.shadow_reason),
        {local.enabled, local.requested_lights, local.selected_lights, local.unsupported_lights,
            local.budget_lights, local.overflow_lights, local.allocated_bytes, local.reason},
        {software_support.compiled, software_support.supported, software.enabled, software.native_ready, software.faulted,
            software.static_instances, software.dynamic_instances, software.unsupported_masked, software.unsupported_thin,
            software.excluded_translucent, software.physical_cascades, software.gpu_bytes, software.scratch_bytes,
            !software_support.supported ? std::string(software_support.reason) :
            request.gi_mode == GraphicsGiMode::SoftwareHybrid && !plan.software_gi_enabled
                ? "Selected Generation is missing required native software GI products." : software.reason},
        {request.adaptive_shading, plan.adaptive_raster_enabled, plan.adaptive_compute_enabled,
            adaptive.raster_ready, adaptive.compute_ready, adaptive.raster_applied,
            impl_->ambient_occlusion.AdaptiveComputeApplied(), adaptive.usable,
            impl_->feature_capabilities.hardware.vrs_tier, impl_->feature_capabilities.hardware.vrs_tile_size,
            adaptive.allocated_bytes,
            request.adaptive_shading != GraphicsAutoToggle::Off && !adaptive.raster_ready &&
                impl_->feature_capabilities.hardware.vrs_tier == 1
                ? "Tier1 cannot protect mixed-material fullscreen DeferredLight selectively; shading stays1x1."
                : adaptive_reason(adaptive.raster_ready, plan.adaptive_raster_reason),
            adaptive_reason(adaptive.compute_ready, plan.adaptive_compute_reason)}};
    const auto cache = impl_->virtual_shadows.CacheStatus();
    status.shadow_cache = {UINT64(impl_->QualityProfileFor(request.shadow_quality).shadow_cache_mib) * 1024u * 1024u,
        cache.admitted_bytes, cache.physical_pages, cache.baked_active, cache.baked_pages, cache.baked_staging_bytes,
        cache.cpu_plan_prepared, cache.guarantee.csm_required,
        cache.cpu_plan_prepared ? 1u << static_cast<std::uint32_t>(cache.guarantee.reason) : 0u,
        cache.guarantee.required_coarse_pages, cache.guarantee.maximum_coarse_commands,
        cache.guarantee.coarse_command_total, cache.guarantee.command_capacity};
    return status;
}

std::uint64_t D3D12Renderer::MaterialParameterRevision() const noexcept
{
    return impl_->material_parameter_revision;
}
std::vector<RendererCharacterAnimationClip> D3D12Renderer::GetCharacterAnimationClips() const
{
    std::vector<RendererCharacterAnimationClip> clips;
    if (!impl_) return clips;
    if (impl_->archer_compiled_animation)
    {
        for (const auto &clip : impl_->archer_compiled_animation->clips)
            clips.push_back({clip.id, std::string(clip.name), clip.duration_seconds, clip.frame_count, clip.looping});
        return clips;
    }
    for (const auto &clip : impl_->archer_named_animations.clips)
        clips.push_back({clip.id, clip.name, clip.duration_seconds, clip.frame_count, clip.looping});
    return clips;
}

RendererStartupTimings D3D12Renderer::GetStartupTimings() const noexcept
{
    return impl_ ? impl_->startup_timings : RendererStartupTimings{};
}

RendererPsoCacheStats D3D12Renderer::GetPipelineCacheStats() const
{
    if (!impl_) return {};
    const auto stats = impl_->pipeline_cache.Stats();
    return {stats.hits, stats.misses, stats.native_creates, stats.stores, stats.store_failures, stats.load_failures,
        stats.io_failures, stats.discarded_files, stats.unsupported_libraries, stats.uncached_mesh_creates,
        stats.root_registrations, stats.key_failures, stats.loaded_bytes, stats.serialized_bytes,
        stats.initialized, stats.library_supported, stats.mesh_library_supported, stats.persistence_enabled,
        stats.last_cache_hresult, stats.last_io_error,
        stats.key_build_ns, stats.key_build_max_ns, stats.library_load_ns, stats.library_load_max_ns,
        stats.native_create_ns, stats.native_create_max_ns, stats.library_store_ns, stats.library_store_max_ns,
        stats.serialize_ns, stats.cache_write_ns, stats.mutex_wait_ns, stats.mutex_wait_max_ns, stats.capture_timings};
}

std::uint64_t D3D12Renderer::PipelineRevision() const noexcept
{
    return impl_->pipeline_revision;
}

Result D3D12Renderer::Shutdown()
{
    if (!impl_)
    {
        return Result::Success();
    }
    Result result = Result::Success();
    if (auto drained = impl_->asset_io.DrainSubmitted(impl_->streaming, impl_->residency); !drained) return drained;
    for (auto &candidate : impl_->unresolved_resident_candidates)
        if (auto drained = candidate->asset_io.DrainSubmitted(candidate->streaming, candidate->residency); !drained)
            return drained;
    if (impl_->queue || impl_->initialized || impl_->reconstruction_provider || impl_->environment_brdf_init_pending ||
        impl_->material_parameter_upload_pending)
    {
        result = impl_->WaitForGpu();
        if (!result && impl_->environment_brdf_init_pending)
        {
            // Failed initialization retains submitted objects until this drain.
            // If completion cannot be established, end this device's execution
            // before releasing them. Keep the original failure result visible.
            impl_->environment_brdf_init_device->RemoveDevice();
        }
        if (!result && impl_->material_parameter_upload_pending)
            impl_->material_parameter_upload_device->RemoveDevice();
    }
    impl_->material_parameter_upload_pending = false;
    if (!result) return result;
    if (auto released = impl_->graph_queue_executor.ReleaseRecordersAfterCompletion(); !released) return released;
    impl_->material_parameter_slots.clear();
    impl_->material_parameter_candidate_slots.clear();
    impl_->resident_utility_submissions.clear();
    for (auto &candidate : impl_->unresolved_resident_candidates)
    {
        if (auto cleared = candidate->brixelizer_gi.ResetAfterCompletion(
                impl_->graph_queue_executor.Completed()); !cleared) return cleared;
    }
    impl_->unresolved_resident_candidates.clear();
    impl_->graph.Reset();
    {
        // IO and all GPU queues are complete. Destroy backing/maps/readback
        // while their actual allocator and native device are still retained.
        RendererTextureFeedback retired_feedback;
        RendererTextureStreaming retired_textures;
        RendererAssetIo retired_io;
        impl_->texture_feedback.SwapState(retired_feedback);
        impl_->texture_streaming.SwapState(retired_textures);
        impl_->asset_io.SwapState(retired_io);
    }
    impl_->environment_texture_stream_ids = {};
    impl_->environment_material_texture_stream_ids.clear();
    impl_->material_texture_stream_ids = {};
    if (auto r = impl_->ResetOwnedViewsAfterCompletion(); !r) return r;
    // The active view retains the completed HDR allocation for capture even
    // after its producing history/provider releases its own reference.
    impl_->hdr_capture_source.Reset();
    impl_->hdr_capture_extent = {};
    impl_->hdr_capture_valid = impl_->hdr_capture_pre_exposed = false;
    impl_->hdr_capture_submission_id = impl_->hdr_capture_package_id =
        impl_->hdr_capture_snapshot_tick = 0;
    impl_->canonical_skin_current.reset();
    impl_->canonical_instance_current.reset();
    impl_->previous_instances.reset();
    impl_->previous_skin_deformation.reset();
    impl_->canonical_skin_frames.clear();
    if (auto drained = impl_->sdk_presentation.DrainBeforeSwapChainMutation(true); !drained) return drained;
    for (auto &buffer : impl_->back_buffers) buffer.Reset();
    impl_->swap_chain.Reset();
    if (auto destroyed = impl_->sdk_presentation.DestroyAfterGpuCompletion(true); !destroyed) return destroyed;
    if (auto cleared = impl_->presentation_guides.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared) return cleared;
    impl_->native_input_phase = 0;
    if (auto cleared = impl_->reconstruction_guides.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (result && impl_->reconstruction_provider)
    {
        if (auto reset = impl_->reconstruction_provider->ResetAfterCompletion(); !reset) result = reset;
        else impl_->reconstruction_provider.reset();
    }
    if (!result) return result;
    if (auto cleared = impl_->brixelizer_gi.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared) return cleared;
    if (impl_->fsr_session)
        if (auto closed = impl_->fsr_session->ShutdownAfterCompletion(true); !closed) return closed;
    if (impl_->streamline_session)
        if (auto closed = impl_->streamline_session->ShutdownAfterCompletion(); !closed) return closed;
    if (auto cleared = impl_->adaptive_shading.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->ambient_occlusion.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->subsurface.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->screen_reflections.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto archived = impl_->ArchiveHeroWork(impl_->hero_transparency); !archived) return archived;
    if (auto cleared = impl_->hero_transparency.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->lighting_probe.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->ltc_lut.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->hdr_post.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->screen_overlay.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->scene_geometry.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->local_shadows.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->volumetrics.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    impl_->volumetric_media.clear();
    impl_->archer_named_animations = {};
    impl_->archer_compiled_animation = nullptr;
    impl_->archer_packed_transforms = {}; impl_->archer_named_packed_transforms = {};
    impl_->archer_clips = {}; impl_->archer_parents = {}; impl_->archer_inverse_bind_matrices = {};
    impl_->archer_transforms = {}; impl_->archer_upper_body_weights = {}; impl_->archer_material_bindings = {};
    impl_->archer_owned_character = {};
    if (auto cleared = impl_->local_probes.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->virtual_shadows.Commit(VirtualShadowCandidate{},
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->decal_surface.ResetAfterCompletion(impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto cleared = impl_->light_list.ResetAfterCompletion(
            impl_->graph_queue_executor.Completed()); !cleared && result) result = cleared;
    if (auto saved = impl_->pipeline_cache.Flush(); !saved)
    {
        const auto message = std::string("Optional PSO cache persistence failed: ") +
            std::string(saved.Message()) + "\n";
        OutputDebugStringA(message.c_str());
    }
    impl_->pipeline_cache.Reset();
    impl_->ResetHlodResources();
    impl_->ResetClusterResources();
    impl_->hero_material_bindings_gpu.Reset();
    impl_->hero_scene_pipeline.Reset();
    impl_->hero_visibility_pipeline.Reset();
    impl_->hero_shadow_pipeline.Reset();
    impl_->hero_resolve_pipeline.Reset();
    impl_->hero_translucent_pipeline.Reset();
    impl_->material_parameter_upload.Reset();
    impl_->material_parameter_upload_device.Reset();
    impl_->material_parameter_candidate.Reset();
    impl_->material_parameters.Reset();
    impl_->material_parameter_slot_count = 0;
    impl_->material_parameter_layouts = {};
    impl_->decal_material_programs = {}; impl_->decal_material_candidate_programs = {};
    impl_->decal_material_policies = {}; impl_->decal_material_candidate_policies = {};
    impl_->decal_parameter_bases = {}; impl_->decal_parameter_candidate_bases = {};
    impl_->decal_parameter_counts = {}; impl_->decal_parameter_candidate_counts = {};
    impl_->decal_parameter_layouts = {}; impl_->decal_parameter_candidate_layouts = {};
    impl_->environment_brdf_init_pending = false;
    impl_->environment_brdf_init_upload.Reset();
    impl_->environment_brdf_init_device.Reset();
    impl_->graph_queue_executor.Clear();
    impl_->ResetEnvironmentStreaming();
    if (impl_->initialized)
    {
        impl_->CountValidationErrors();
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->imgui_initialized)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        impl_->imgui_initialized = false;
    }
    impl_->imgui_heap.Reset();
#endif
    for (auto &frame : impl_->frames)
    {
        if (frame.upload.resource && frame.mapped)
        {
            frame.upload.resource->Unmap(0, nullptr);
            frame.mapped = nullptr;
        }
        if (frame.ui_upload.resource && frame.ui_mapped)
        {
            frame.ui_upload.resource->Unmap(0, nullptr);
            frame.ui_mapped = nullptr;
        }
        if (frame.timestamp_readback.resource && frame.mapped_timestamps)
        {
            D3D12_RANGE no_write{};
            frame.timestamp_readback.resource->Unmap(0, &no_write);
        }
        frame.timestamp_readback.Reset();
        frame.timestamp_heap.Reset();
        frame.mapped_timestamps = nullptr;
        frame.timestamp_capacity = 0;
        frame.timestamp_passes.clear();
        frame.timestamp_frequencies.clear();
        frame.completion = {};
        frame.skin_cache_use_completion = {};
        frame.upload.Reset();
        frame.ui_upload.Reset();
        frame.static_visible_refs.Reset();
        frame.static_draw_args.Reset();
        frame.static_rejected_refs.Reset();
        frame.static_rejected_args.Reset();
        frame.static_retest_refs.Reset();
        frame.static_retest_args.Reset();
        frame.dynamic_visible_refs.Reset();
        frame.dynamic_draw_args.Reset();
        frame.ribbon_arguments.Reset();
        frame.geometry_draw_records.Reset();
        frame.geometry_draw_records_upload.Reset();
        frame.geometry_draw_records_cpu.clear();
        frame.skin_cache_vertices.Reset();
        frame.skin_cache_bounds.Reset();
        frame.static_visible_capacity = 0;
        frame.static_rejected_capacity = 0;
        frame.static_retest_capacity = 0;
        frame.dynamic_visible_capacity = 0;
        frame.skin_cache_vertex_capacity = 0;
        frame.skin_cache_bound_capacity = 0;
        frame.skin_cache_vertex_read = false;
        frame.skin_cache_bounds_read = false;
        frame.skin_cache_valid = false;
    }
    for (auto &pool : impl_->transient_pools) pool.Clear();
    impl_->depth.Reset();
    impl_->shadow.Reset();
    impl_->archer_geometry.Reset();
    impl_->gel_projectile_geometry.Reset();
    impl_->procedural_geometry.Reset();
    impl_->mesh_pipeline_pairs.clear();
    impl_->virtual_shadow_indexed_pipelines.clear();
    impl_->virtual_shadow_mesh_pipelines.clear();
    impl_->previous_shadow_casters.clear();
    impl_->indexed_geometry_signature.Reset();
    impl_->mesh_geometry_signature.Reset();
    impl_->cluster_draw_signature.Reset();
    impl_->cluster_mesh_signature.Reset();
    impl_->cluster_root_signature.Reset();
    impl_->cluster_count_pipeline.Reset();
    impl_->cluster_prefix_pipeline.Reset();
    impl_->cluster_emit_pipeline.Reset();
    for (auto &pipeline : impl_->cluster_scene_pipelines) pipeline.Reset();
    for (auto &pipeline : impl_->cluster_shadow_pipelines) pipeline.Reset();
    for (auto &pipeline : impl_->cluster_visibility_pipelines) pipeline.Reset();
    impl_->slime_pipeline.Reset();
    impl_->vfx_distortion_pipeline.Reset();
    for (auto &asset : impl_->monster_assets)
        asset.Reset();
    for(auto &asset:impl_->monster_reduced_geometry)
        for(auto &page:asset) page.Reset();
    impl_->monster_skin_matrices.Reset();
    impl_->archer_diffuse.Reset();
    impl_->archer_normal.Reset();
    impl_->monster_basecolor.Reset();
    impl_->monster_emissive.Reset();
    impl_->monster_ram.Reset();
    impl_->family_diffuse.Reset();
    impl_->family_normal.Reset();
    impl_->vfx_masks.Reset();
    impl_->ribbon_history.Reset();
    impl_->ribbon_previous.Reset();
    impl_->vfx_smoke_motion.Reset();
    impl_->vfx_flow_curl.Reset();
    impl_->vfx_stbn_scalar.Reset();
    impl_->vfx_fracture_decals.Reset();
    impl_->vfx_organic_decals.Reset();
    impl_->vfx_smoke_neg.Reset();
    impl_->vfx_smoke_pos.Reset();
    impl_->vfx_curve_lut.Reset();
    impl_->vfx_noise_basis.Reset();
    impl_->vfx_noise_volume.Reset();
    impl_->vfx_authored_mask.Reset();
    impl_->ribbon_detail.Reset();
    impl_->vfx_gradient.Reset();
    impl_->vfx_gradient_rows = 0;
    impl_->vfx_mesh_atlas.Reset();
    impl_->particles.Reset();
    for (auto &alive : impl_->particle_alive)
    {
        alive.Reset();
    }
    impl_->particle_dead.Reset();
    impl_->particle_counters.Reset();
    impl_->indirect_arguments.Reset();
    impl_->gbuffer_base.Reset();
    impl_->gbuffer_normal.Reset();
    impl_->gbuffer_metadata.Reset();
    impl_->gbuffer_material.Reset();
    impl_->gbuffer_motion.Reset();
    impl_->gbuffer_surface_history.Reset();
    for (auto &sidecar : impl_->material_sidecars) sidecar.Reset();
    impl_->material_sidecar_layout = {};
    impl_->material_surface_layouts = {};
    impl_->material_headers = {};
    impl_->visibility_ids.Reset();
    impl_->visibility_front_face.Reset();
    impl_->visibility_pixels.Reset();
    impl_->visibility_counts.Reset();
    impl_->visibility_dispatch_args.Reset();
    for (auto &visibility : impl_->visibility_frames) visibility.Reset();
    impl_->visibility_pipeline.Reset();
    impl_->surface_resolve_pipeline.Reset();
    for (auto &pipeline : impl_->environment_material_visibility_pipelines) pipeline.Reset();
    for (auto &pipeline : impl_->environment_material_resolve_pipelines) pipeline.Reset();
    impl_->raster_records_pipeline.Reset();
    impl_->visibility_clear_pipeline.Reset();
    impl_->visibility_count_pipeline.Reset();
    impl_->visibility_prefix_pipeline.Reset();
    impl_->visibility_scatter_pipeline.Reset();
    impl_->dispatch_signature.Reset();
    for (auto &texture : impl_->environment_textures) texture.Reset();
    impl_->environment_brdf_lut.Reset();
    impl_->environment_brdf_candidate.Reset();
    for (auto &mesh : impl_->environment_meshes) mesh.Reset();
    impl_->static_gpu_scene.reset();
    impl_->retired_static_gpu_scenes.clear();
    impl_->dynamic_instance_upload_log.close();
    impl_->dynamic_instances.Reset();
    impl_->dynamic_lod_states = {};
    impl_->dynamic_scene_slots.Clear();
    impl_->skin_cache.Clear();
    impl_->retired_dynamic_instance_buffers.clear();
    impl_->dynamic_instance_capacity = 0;
    impl_->dynamic_instance_slot_count = 0;
    impl_->dynamic_instances_shader_read = false;
    impl_->hdr_color.Reset();
    impl_->subsurface_diffuse.Reset();
    impl_->environment_specular.Reset();
    impl_->indirect_diffuse.Reset();
    impl_->oit_accumulation.Reset();
    impl_->oit_revealage.Reset();
    impl_->post_a.Reset();
    impl_->sampler_heap.Reset();
    impl_->srv_heap.Reset();
    impl_->srv_cpu_heap.Reset();
    impl_->sampler_stride = 0;
    impl_->scene_work.Reset();
    impl_->display_linear.Reset();
    impl_->composite_hdr.Reset();
    for (auto &history : impl_->temporal_normal_history) history.Reset();
    for (auto &history : impl_->temporal_material_history) history.Reset();
    for (auto &value : impl_->exposure_values) value.Reset();
    impl_->exposure_histogram.Reset();
    impl_->initial_exposure.Reset();
    impl_->exposure_initialized = impl_->view_targets_initialized = false;
    impl_->exposure_read_index = 0;
    impl_->exposure_sample_time = {};
    impl_->pre_exposure_pipeline.Reset();
    impl_->exposure_clear_pipeline.Reset();
    impl_->exposure_build_pipeline.Reset();
    impl_->exposure_reduce_pipeline.Reset();
    impl_->post_b.Reset();
    for (auto &history : impl_->temporal_history) history.Reset();
    for (auto &history : impl_->hzb_history) history.Reset();
    impl_->temporal_history_valid = false;
    impl_->hzb_history_valid = false;
    for (auto &texture : impl_->ui_textures)
    {
        texture.Reset();
    }
    for (auto &surface : impl_->ui_surfaces)
    {
        surface.brush.Reset();
        surface.target.Reset();
        surface.bitmap.Reset();
    }
    impl_->ui_text_layouts.clear();
    impl_->ui_font_collection.Reset();
    if (impl_->ui_font_loader && impl_->ui_dwrite_factory)
        impl_->ui_dwrite_factory->UnregisterFontFileLoader(impl_->ui_font_loader.Get());
    impl_->ui_font_loader.Reset();
    impl_->ui_dwrite_factory.Reset();
    impl_->ui_d2d_factory.Reset();
    impl_->ui_wic_factory.Reset();
    for (auto &buffer : impl_->back_buffers)
    {
        buffer.Reset();
    }
    if (impl_->allocator)
    {
        impl_->allocator->Release();
        impl_->allocator = nullptr;
    }
    if (impl_->fence_event)
    {
        CloseHandle(impl_->fence_event);
        impl_->fence_event = nullptr;
    }
    impl_->initialized = false;
    impl_->streamline_factory_proxy.Reset();
    impl_->streamline_queue_proxy.Reset();
    impl_->streamline_device_proxy.Reset();
    if (impl_->streamline_session)
        if (auto unloaded = impl_->streamline_session->UnloadAfterProxyRelease(); !unloaded) return unloaded;
    impl_->streamline_session.reset();
    impl_->fsr_session.reset();
    impl_->enhanced_command_list.Reset();
    impl_->command_list.Reset();
    impl_->fence.Reset();
    impl_->queue.Reset();
    impl_->device.Reset();
    impl_->adapter.Reset();
    impl_->factory.Reset();
    if (impl_->com_initialized)
    {
        CoUninitialize();
        impl_->com_initialized = false;
    }
    return result;
}

bool D3D12Renderer::UsesEnhancedBarriers() const noexcept
{
    return impl_->enhanced;
}

std::uint64_t D3D12Renderer::ValidationErrorCount() const noexcept
{
    return impl_->validation_errors;
}

std::uint32_t D3D12Renderer::LastParticleCount() const noexcept
{
    return impl_->last_particle_count;
}

GpuPassTimings D3D12Renderer::LastGpuPassTimings() const
{
    const auto &frame = impl_->frames[impl_->last_presented_index];
    auto timings = impl_->ReadFrameGpuTimings(frame);
    return timings.valid && timings.view_id == impl_->active_view_id.value ? timings : impl_->completed_gpu_timings;
}

const GpuPassTimings &D3D12Renderer::LastCompletedGpuPassTimings() const noexcept
{
    return impl_->completed_gpu_timings;
}

namespace
{
void AddHeroWorkStatus(RendererHeroWorkStatus &into, const RendererHeroWorkStatus &from, bool allocations)
{
    into.enabled |= from.enabled; into.overflowed |= from.overflowed;
    into.mesh_statistics_supported |= from.mesh_statistics_supported;
    into.shader_counters_enabled |= from.shader_counters_enabled;
    into.submitted_frames += from.submitted_frames; into.completed_frames += from.completed_frames;
    into.failed_frames += from.failed_frames; into.pending_frames += from.pending_frames; into.samples += from.samples;
    into.shader_invalid_samples += from.shader_invalid_samples;
    into.shader_clear_commands += from.shader_clear_commands;
    into.shader_copy_commands += from.shader_copy_commands;
    if (allocations)
    {
        into.readback_bytes += from.readback_bytes; into.query_heap_count += from.query_heap_count;
        into.query_count += from.query_count;
        into.shader_gpu_bytes += from.shader_gpu_bytes;
        into.shader_readback_bytes += from.shader_readback_bytes;
        into.shader_zero_upload_bytes += from.shader_zero_upload_bytes;
    }
}
}
Result D3D12Renderer::Impl::ArchiveHeroWork(HeroTransparencySystem &service)
{
    if (!config.capture_hero_work) return Result::Success();
    if (auto result = service.ConsumeCompletedWork(graph_queue_executor.Completed(), hero_work_pending); !result) return result;
    const auto status = service.WorkStatus();
    if (status.pending_frames)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Cannot retire pending hero contributor native queries.");
    AddHeroWorkStatus(hero_work_retired, status, false);
    return Result::Success();
}
Result D3D12Renderer::ConsumeCompletedHeroWork(std::vector<RendererHeroWorkSample> &output)
{
    output.clear();
    if (!impl_ || !impl_->config.capture_hero_work) return Result::Success();
    const auto completed = impl_->graph_queue_executor.Completed();
    output.swap(impl_->hero_work_pending);
    if (auto result = impl_->hero_transparency.ConsumeCompletedWork(completed, output); !result) return result;
    for (auto &[id, view] : impl_->owned_views)
    {
        (void)id;
        if (auto result = view->hero_transparency.ConsumeCompletedWork(completed, output); !result) return result;
    }
    for (auto &view : impl_->unresolved_view_candidates)
        if (auto result = view->hero_transparency.ConsumeCompletedWork(completed, output); !result) return result;
    std::ranges::sort(output, [](const auto &a, const auto &b) {
        return std::tie(a.completed_frame, a.view_id, a.phase, a.layer, a.contributor) <
            std::tie(b.completed_frame, b.view_id, b.phase, b.layer, b.contributor);
    });
    return Result::Success();
}
Result D3D12Renderer::DrainHeroWork(std::vector<RendererHeroWorkSample> &output)
{
    output.clear();
    if (!impl_ || !impl_->initialized || !impl_->config.capture_hero_work)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Hero work drain requires an initialized capture_hero_work renderer.");
    if (auto waited = impl_->WaitForGpu(); !waited) return waited;
    return ConsumeCompletedHeroWork(output);
}
RendererHeroWorkStatus D3D12Renderer::HeroWorkStatus() const
{
    if (!impl_) return {};
    auto status = impl_->hero_work_retired; status.enabled = impl_->config.capture_hero_work;
    AddHeroWorkStatus(status, impl_->hero_transparency.WorkStatus(), true);
    for (const auto &[id, view] : impl_->owned_views)
    { (void)id; AddHeroWorkStatus(status, view->hero_transparency.WorkStatus(), true); }
    for (const auto &view : impl_->unresolved_view_candidates) AddHeroWorkStatus(status, view->hero_transparency.WorkStatus(), true);
    // Full resident adoption archived old rows/counters before swapping. A
    // separately retained old candidate still owns its physical allocations.
    for (const auto &resident : impl_->unresolved_resident_candidates)
    {
        const auto retained = resident->hero_transparency.WorkStatus();
        status.readback_bytes += retained.readback_bytes;
        status.query_heap_count += retained.query_heap_count; status.query_count += retained.query_count;
        status.shader_gpu_bytes += retained.shader_gpu_bytes;
        status.shader_readback_bytes += retained.shader_readback_bytes;
        status.shader_zero_upload_bytes += retained.shader_zero_upload_bytes;
    }
    return status;
}
Result D3D12Renderer::ConsumeCompletedGpuPassTimings(std::vector<GpuPassTimings> &output)
{
    output.clear();
    std::ranges::sort(impl_->pending_gpu_timings, [](const auto &left, const auto &right) {
        return left.completed_frame_id != right.completed_frame_id
            ? left.completed_frame_id < right.completed_frame_id : left.view_id < right.view_id;
    });
    output.swap(impl_->pending_gpu_timings);
    impl_->pending_gpu_timing_passes = 0;
    if (impl_->gpu_timing_stream_overflowed)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Completed GPU timing capture overflowed its 240-frame or 262144-pass pending limit.");
    return Result::Success();
}

Result D3D12Renderer::DrainGpuPassTimings(std::vector<GpuPassTimings> &output)
{
    output.clear();
    if (!impl_->initialized || !impl_->config.capture_timing_stream)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "GPU timing drain requires an initialized capture_timing_stream renderer.");
    if (auto drained = impl_->WaitForGpu(); !drained) return drained;
    for (const auto &frame : impl_->frames)
    {
        if (!frame.timestamps_recorded) continue;
        auto timings = impl_->ReadFrameGpuTimings(frame);
        if (!timings.valid) continue;
        if (!impl_->diagnostics.HasCompleted(timings.completed_frame_id, timings.view_id))
            impl_->diagnostics.RecordCompleted(timings);
        if (timings.view_id == impl_->active_view_id.value)
        {
            if (timings.completed_frame_id > impl_->completed_gpu_timings.completed_frame_id)
                impl_->completed_gpu_timings = std::move(timings);
        }
        else if (const auto found = impl_->owned_views.find(timings.view_id); found != impl_->owned_views.end())
            if (timings.completed_frame_id > found->second->completed_gpu_timings.completed_frame_id)
                found->second->completed_gpu_timings = std::move(timings);
    }
    return ConsumeCompletedGpuPassTimings(output);
}

const RendererDiagnosticsFrame &D3D12Renderer::LastRenderedFrameDiagnostics() const noexcept
{
    return impl_->diagnostics.LastRenderedFrame();
}

RendererQueueStats D3D12Renderer::LastQueueStats() const noexcept
{
    const auto stats = impl_->graph_queue_executor.LastExecutionStats();
    return {stats.direct_batches, stats.compute_batches, stats.copy_batches,
            stats.cross_queue_waits, stats.record_jobs, stats.transient_resource_count,
            stats.transient_block_count, stats.alias_activation_count,
            stats.transient_bytes_without_aliasing, stats.transient_backing_bytes};
}

RendererResidencyStats D3D12Renderer::ResidencyStats() const noexcept
{
    const auto accounting = impl_->residency.Accounting();
    const auto completed = impl_->graph_queue_executor.Completed();
    const auto texture = impl_->texture_streaming.Statistics(impl_->streaming, completed);
    std::uint64_t geometry_requests{}, geometry_uploads{}, geometry_publications{}, geometry_reclaims{};
    std::uint64_t geometry_generation{2};
    auto geometry_state = StreamingPageState::Resident;
    for (const auto &stream : impl_->environment_geometry_streams)
    {
        geometry_requests += stream.requests;
        geometry_uploads += stream.uploads;
        geometry_publications += stream.publications;
        geometry_reclaims += stream.reclaims;
        if (!stream.request) continue;
        geometry_generation = std::min(geometry_generation, stream.generation);
        const auto *geometry_page = impl_->streaming.Find(stream.request);
        const auto state = geometry_page ? geometry_page->state : StreamingPageState::Cancelled;
        if (state == StreamingPageState::Cancelled ||
            (geometry_state != StreamingPageState::Cancelled && state < geometry_state))
            geometry_state = state;
    }
    if (geometry_requests == 0)
    {
        geometry_generation = 1;
        geometry_state = StreamingPageState::Cancelled;
    }
    return {
        accounting.budget.valid,
        accounting.budget.local_budget,
        accounting.budget.local_usage,
        accounting.live_total,
        accounting.retiring_total,
        accounting.reclaimed_total,
        texture.requests,
        texture.uploads,
        texture.publications,
        texture.reclaims,
        texture.revision,
        texture.state,
        geometry_requests,
        geometry_uploads,
        geometry_publications,
        geometry_reclaims,
        geometry_generation,
        static_cast<std::uint8_t>(geometry_state),
    };
}

RendererTextureProductStatus D3D12Renderer::GetTextureStreamingStatus(std::string_view logical_id) const noexcept
{
    if (!impl_ || !impl_->initialized) return {};
    const auto status = impl_->texture_streaming.ProductStatus(logical_id, impl_->streaming,
        impl_->graph_queue_executor.Completed());
    return {status.registered, status.pending, status.publication_complete,
        status.requests, status.uploads, status.publications, status.reclaims, status.revision,
        status.actual_resident_mip, status.effective_desired_mip, status.target_mip};
}


} // namespace hs
