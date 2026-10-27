#include "renderer_impl.hpp"
#include <hs/render_data/volumetric_medium.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/renderer/brixelizer_shaders.hpp>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>

namespace hs
{
Result D3D12Renderer::Impl::PrepareLocalProbes(const RendererConfig &candidate_config,
    LocalProbeRuntimeCandidate &candidate) const
{
    RendererConfig selected;
    if (auto prepared = SelectTextureFeedbackShaders(candidate_config, selected); !prepared) return prepared;
    // Shader/parameter adoption preserves the resident geometry and immutable scene owner.
    return local_probes.Prepare(device.Get(), allocator, root_signature.Get(), selected,
        resource_generation_id, candidate, &pipeline_cache);
}
Result D3D12Renderer::Impl::PrepareVolumetrics(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent capacity, VolumetricPrepared &candidate) const
{
    candidate = {};
    if (candidate_config.volumetric_quality != GraphicsOptionalQuality::Off)
    {
        constexpr std::string_view id = "render/lighting/volumetric_media.rpkg";
        RendererAssetSource path;
        if (auto resolved = ResolveRendererAsset(candidate_config, id, path); !resolved) return resolved;
        std::vector<std::byte> bytes;
        if (auto read = ReadBinary(path, bytes); !read) return read;
        try
        {
            const auto package = render_data::ReadPackage(bytes, MakeAssetId(id));
            if (package.chunks.size() != 1 || package.chunks[0].type != render_data::ChunkType::VolumetricVolumes)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid volumetric medium package.");
            const auto media = render_data::DecodeVolumetricMedia(package.chunks[0].bytes);
            for (const auto &source : media)
            {
                VolumetricVolume volume;
                for (std::size_t row = 0; row != 4; ++row)
                    for (std::size_t column = 0; column != 4; ++column)
                        volume.world_to_local.m[row][column] = source.world_to_local[row * 4 + column];
                volume.density_extinction = {source.density_extinction[0], source.density_extinction[1], source.density_extinction[2], 0};
                volume.scattering = {source.scattering[0], source.scattering[1], source.scattering[2], 0};
                volume.identity = {std::uint32_t(source.stable_id), std::uint32_t(source.stable_id >> 32), source.generation, source.shape};
                candidate.media.push_back(volume);
            }
            candidate.revision = 14695981039346656037ull;
            for (const auto byte : bytes) candidate.revision = (candidate.revision ^ std::uint8_t(byte)) * 1099511628211ull;
        }
        catch (const std::exception &error)
        { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what()); }
    }
    return owner.volumetrics.Prepare(device.Get(), allocator, root_signature.Get(),
        candidate_config.volumetric_quality, QualityProfileFor(candidate_config.volumetric_quality == GraphicsOptionalQuality::Off
            ? GraphicsQuality::Low : static_cast<GraphicsQuality>(static_cast<unsigned>(candidate_config.volumetric_quality) - 1u)), capacity, kFrameCount, 256,
        [candidate_config](std::string_view id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate.service, &pipeline_cache, candidate_config.vfx_quality == VfxQuality::High);
}
Result D3D12Renderer::Impl::CommitVolumetrics(OwnedView &owner, VolumetricPrepared &&candidate)
{
    if (auto committed = owner.volumetrics.Commit(std::move(candidate.service), graph_queue_executor.Completed()); !committed) return committed;
    volumetric_media = std::move(candidate.media);
    volumetric_revision = candidate.revision;
    owner.previous_volumetric_camera = {};
    return Result::Success();
}
RendererFeaturePlan D3D12Renderer::Impl::ResolveFeatures(const RendererConfig &request) const noexcept
{
    auto available = feature_capabilities;
    for (const auto id : {"render/shader/scene_geometry_cs.dxil", "render/shader/brixelizer_guides_cs.dxil",
            "render/shader/brixelizer_gi_resolve_cs.dxil", "render/shader/brixelizer_cache_export_cs.dxil",
            "render/shader/brixelizer_gi_compose_cs.dxil", "render/shader/brixelizer_cache_direct_ps.dxil"})
        available.software_gi_consumer = available.software_gi_consumer && (request.product_paths.contains(id) || FindRendererBulk(request, id));
#if HS_WITH_BRIXELIZER
    if (available.software_gi_consumer &&
        request.gi_mode == GraphicsGiMode::SoftwareHybrid)
    {
        // Validate the exact pinned official permutation table once per query;
        // lookup does not read or allocate any native context/shader storage.
        try
        {
            static const auto products = [] {
                std::vector<std::string> ids;
                if (!brixelizer_native::EnumerateShaders([&](std::string_view id,std::span<const std::byte>) {
                        ids.emplace_back(id);return true;
                    })) ids.clear();
                for(UINT noise=0;noise<16;++noise)ids.push_back("render/texture/brixelizer_noise_"+std::to_string(noise)+".rpkg");
                return ids;
            }();
            available.software_gi_consumer = products.size() == 748 && std::ranges::all_of(products,
                [&](const auto &id) {return (request.product_paths.contains(id) || FindRendererBulk(request, id));});
        }
        catch (...) { available.software_gi_consumer = false; }
    }
#else
    available.software_gi_consumer = false;
#endif
    available.adaptive_raster_consumer = available.adaptive_raster_consumer &&
        (request.product_paths.contains("render/shader/adaptive_shading_classify_cs.dxil") || FindRendererBulk(request, "render/shader/adaptive_shading_classify_cs.dxil")) &&
        (request.product_paths.contains("render/shader/adaptive_shading_rate_cs.dxil") || FindRendererBulk(request, "render/shader/adaptive_shading_rate_cs.dxil"));
    available.adaptive_compute_consumer = available.adaptive_compute_consumer &&
        (request.product_paths.contains("render/shader/adaptive_shading_classify_cs.dxil") || FindRendererBulk(request, "render/shader/adaptive_shading_classify_cs.dxil")) &&
        (request.product_paths.contains("render/shader/adaptive_gtao_horizon_cs.dxil") || FindRendererBulk(request, "render/shader/adaptive_gtao_horizon_cs.dxil")) &&
        (request.product_paths.contains("render/shader/adaptive_gtao_resolve_cs.dxil") || FindRendererBulk(request, "render/shader/adaptive_gtao_resolve_cs.dxil"));
    return ResolveRendererFeatures({request.ao_mode,
        request.gi_mode, request.reflection_mode, request.shadow_mode, request.adaptive_shading}, available);
}
std::array<bool, 3> D3D12Renderer::Impl::RequiredLightingAuxiliaries(const RendererConfig &request) const noexcept
{
    const bool have_subsurface = std::ranges::any_of(material_surface_layouts, [](const auto &layout) {
        return (layout.capability_mask & static_cast<std::uint32_t>(
            render_data::MaterialClosureCapability::Subsurface)) != 0;
    });
    return LightingAuxiliaryTargets(ResolveFeatures(request), have_subsurface);
}
Result D3D12Renderer::Impl::PrepareLightList(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent capacity, LightListCandidate &candidate) const
{
    return owner.light_list.Prepare(device.Get(), allocator, capacity, 128, kFrameCount,
        [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate, QualityProfileFor(candidate_config.lighting_quality).lighting_cluster_capacity);
}
Result D3D12Renderer::Impl::PrepareVirtualShadows(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent capacity, VirtualShadowCandidate &candidate) const
{
    constexpr std::array products{"render/shader/virtual_shadow_init_cs.dxil",
        "render/shader/virtual_shadow_feedback_cs.dxil", "render/shader/virtual_shadow_resident_cs.dxil",
        "render/shader/virtual_shadow_cull_cs.dxil", "render/shader/virtual_shadow_publish_cs.dxil",
        "render/shader/virtual_shadow_clear_vs.dxil", "render/shader/virtual_shadow_clear_ps.dxil",
        "render/shader/virtual_shadow_seed_ps.dxil"};
    VirtualShadowProfile profile;
    profile.enabled = candidate_config.shadow_mode == GraphicsShadowMode::Raster &&
        std::ranges::all_of(products, [&](const auto id) { return (candidate_config.product_paths.contains(id) || FindRendererBulk(candidate_config, id)); });
    constexpr std::uint64_t mib = 1024ull * 1024ull;
    const auto cache_bytes = UINT64(QualityProfileFor(candidate_config.shadow_quality).shadow_cache_mib) * mib;
    // Two D32 banks with guarded 132x132 tiles. A rectangular native atlas can
    // add unused tiles, so use complete square banks within the authored limit.
    auto columns = std::min<UINT>(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION / kVirtualShadowTile,
        static_cast<UINT>(std::floor(std::sqrt(double(cache_bytes / (2ull * kVirtualShadowTile * kVirtualShadowTile * sizeof(float)))))));
    while (columns)
    {
        D3D12_RESOURCE_DESC bank{};
        bank.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        bank.Width = bank.Height = columns * kVirtualShadowTile;
        bank.DepthOrArraySize = bank.MipLevels = 1;
        bank.Format = DXGI_FORMAT_R32_TYPELESS; bank.SampleDesc = {1, 0};
        bank.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &bank);
        if (allocation.SizeInBytes != UINT64_MAX && allocation.SizeInBytes <= cache_bytes / 2) break;
        --columns;
    }
    if (!columns)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Authored shadow cache cannot admit one native guarded bank pair.");
    profile.physical_pages = columns * columns;
    profile.cache_budget_bytes = cache_bytes;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    const auto queried = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
    if (FAILED(queried)) return HResultFailure("Query virtual shadow admission budget", queried);
    profile.gpu_budget_bytes = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
    profile.filter_samples = QualityProfileFor(candidate_config.shadow_quality).shadow_filter_samples;
    profile.max_casters = 65536;
    // Keep the volume/probe reserve in addition to one receiver per monster caster.
    profile.max_receivers = profile.max_casters + 256;
    profile.max_draw_groups = 128;
    profile.command_capacity = 65536;
    profile.frame_slots = kFrameCount;
    profile.virtual_sides = {32, 8, 8};
    profile.render_capacity = capacity;
    profile.fixed_root_depth_span = candidate_config.shadow_distance_m * 2.0f;
    if(auto loaded=VirtualShadowSystem::LoadBake(candidate_config,profile,baked_shadow_source);!loaded) return loaded;
    return owner.virtual_shadows.Prepare(device.Get(), allocator, profile,
        [candidate_config](std::string_view logical_id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate, &pipeline_cache);
}
Result D3D12Renderer::Impl::PrepareHdrPost(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent render_capacity, ViewExtent output_extent, HdrPostCandidate &candidate) const
{
    HdrPostSettings settings;
    settings.motion_blur = candidate_config.motion_blur;
    settings.depth_of_field = candidate_config.depth_of_field;
    settings.color_grading = candidate_config.color_grading;
    return owner.hdr_post.Prepare(device.Get(), allocator, render_capacity, output_extent, settings,
        kFrameCount, [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate);
}
Result D3D12Renderer::Impl::PrepareAdditionalShadows(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent capacity, AdditionalShadowPrepared &candidate) const
{
    candidate = {};
    const auto shader_reader = [candidate_config](std::string_view id, std::vector<std::byte> &bytes) {
        RendererAssetSource path;
        if (auto resolved = ResolveRendererAsset(candidate_config, id, path); !resolved) return resolved;
        return ReadBinary(path, bytes);
    };
    constexpr std::uint64_t mib = 1024ull * 1024ull;
    LocalShadowProfile local;
    local.enabled = true;
    const auto &quality = QualityProfileFor(candidate_config.shadow_quality);
    local.filter_samples = quality.shadow_filter_samples;
    local.maximum_lights = quality.local_shadow_max_lights;
    local.face_resolution = quality.local_shadow_resolution;
    local.area_samples = quality.local_shadow_area_samples;
    local.byte_budget = UINT64(quality.local_shadow_budget_mib) * mib;
    local.max_casters = 65536; local.max_draw_groups = 128; local.command_capacity = 65536;
    local.frame_slots = kFrameCount;
    if (auto prepared = owner.local_shadows.Prepare(device.Get(), allocator, local, shader_reader,
            candidate.local, &pipeline_cache); !prepared) return prepared;
    return Result::Success();
}
Result D3D12Renderer::Impl::CommitAdditionalShadows(OwnedView &owner, AdditionalShadowPrepared &&candidate)
{
    const auto completed = graph_queue_executor.Completed();
    if (auto result = owner.local_shadows.Commit(std::move(candidate.local), completed); !result) return result;
    return Result::Success();
}
Result D3D12Renderer::Impl::PrepareSceneGeometry(const RendererConfig &candidate_config,
    SceneGeometryCandidate &geometry) const
{
    const auto read_shader = [candidate_config](std::string_view id, std::vector<std::byte> &bytes) {
        RendererAssetSource path;
        if (auto resolved = ResolveRendererAsset(candidate_config, id, path); !resolved) return resolved;
        return ReadBinary(path, bytes);
    };
    return scene_geometry.Prepare(device.Get(), allocator, root_signature.Get(),
        ResolveFeatures(candidate_config).software_gi_enabled, kFrameCount, read_shader, geometry);
}
Result D3D12Renderer::Impl::PrepareLightingProbe(const RendererConfig &candidate_config,
    LightingProbeCandidate &candidate) const
{
    RendererAssetSource path;
    if (auto resolved = ResolveRendererAsset(candidate_config, "render/lighting/default_probe.rpkg", path); !resolved)
        return resolved;
    std::vector<std::byte> bytes;
    if (auto read = ReadBinary(path, bytes); !read) return read;
    return lighting_probe.Prepare(device.Get(), allocator, bytes, candidate);
}
Result D3D12Renderer::Impl::PrepareLtcLut(const RendererConfig &candidate_config,
    LtcLutCandidate &candidate) const
{
    RendererAssetSource path;
    if (auto resolved = ResolveRendererAsset(candidate_config, "render/lighting/default_ltc.rpkg", path); !resolved)
        return resolved;
    std::vector<std::byte> bytes;
    if (auto read = ReadBinary(path, bytes); !read) return read;
    return ltc_lut.Prepare(device.Get(), allocator, bytes, candidate);
}
Result D3D12Renderer::Impl::PrepareAdaptiveShading(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent capacity, AdaptiveShadingCandidate &candidate) const
{
    const auto plan = ResolveFeatures(candidate_config);
    AdaptiveShadingProfile profile;
    profile.raster_enabled = plan.adaptive_raster_enabled;
    profile.compute_enabled = plan.adaptive_compute_enabled;
    profile.render_capacity = capacity;
    profile.frame_slots = kFrameCount;
    profile.maximum_materials = static_cast<std::uint32_t>(kResidentMaterialCount);
    if (profile.raster_enabled || profile.compute_enabled)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        const auto queried = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
        if (FAILED(queried)) return HResultFailure("Query adaptive shading native byte budget", queried);
        const auto available = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
        // CurrentUsage already includes live storage; candidate allocations must
        // fit the observed free budget while the old completed service survives.
        profile.byte_budget = std::min(profile.byte_budget, available);
        if (!profile.byte_budget)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "No native memory budget remains for adaptive shading preparation.");
    }
    return owner.adaptive_shading.Prepare(device.Get(), allocator, profile,
        [&](std::string_view id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate);
}
Result D3D12Renderer::Impl::BuildAdaptiveMaterialPolicies(std::vector<AdaptiveShadingMaterialPolicy> &policies) const
{
    policies.clear();
    try
    {
        policies.reserve(kResidentMaterialCount);
        constexpr auto basic = static_cast<std::uint32_t>(render_data::MaterialClosureCapability::Basic);
        for (std::size_t ordinal = 0; ordinal < kAuthoredMaterialNames.size(); ++ordinal)
        {
            bool eligible{};
            if (config.packed_content || config.development_catalog)
            {
                const std::string name(kAuthoredMaterialNames[ordinal]);
                const auto id = render_data::MaterialProduct(name);
                const auto handle = FindRendererMaterial(config, MakeAssetId(id));
                if (!handle) throw std::runtime_error("Compiled adaptive material is missing");
                const auto &material = config.packed_materials[handle.value];
                eligible = material.surface.capability_mask == basic && material.flags == 0 &&
                    std::ranges::none_of(material.textures, [](const auto &texture) { return texture.slot >= 3; });
            }
            else
            {
                const auto &header = material_headers[ordinal];
                const auto metadata = std::ranges::find(header.view.chunks, render_data::ChunkType::MaterialMetadata,
                    &render_data::ChunkView::type);
                if (metadata != header.view.chunks.end())
                {
                    const auto material = render_data::DecodeRockMaterial(metadata->bytes, header.view.dependencies);
                    // Cooked alpha coverage and actual high-frequency authored
                    // detail/height/histogram domains stay full rate. Basic base,
                    // roughness and normal texture variation is checked per pixel.
                    eligible = material_surface_layouts[ordinal].capability_mask == basic &&
                        !material.alpha_mask && !material.two_sided && material.textures[3].empty() &&
                        material.textures[4].empty() && material.textures[5].empty();
                }
            }
            // This exact renderer profile replaces terrain by the unlayered
            // analytic grid. Its actual line/normal/depth edges remain protected
            // by the pixel+halo classifier; authored terrain never inherits it.
            if (ordinal == kTerrainMaterialOrdinal && config.grid_floor_probe) eligible = true;
            policies.push_back({0x10000000u + static_cast<std::uint32_t>(ordinal), eligible ? 1u : 0u});
        }
        for (std::size_t hero = 0; hero < kHeroMaterialNames.size(); ++hero)
            // The current hero package ABI supplies closure/domain bindings but
            // no explicit opacity/frequency policy. A masked opaque binding or
            // advanced closure must never be inferred eligible from its ID.
            policies.push_back({render_data::kHeroMaterialIdBase + static_cast<std::uint32_t>(hero), 0u});
        std::ranges::sort(policies, {}, &AdaptiveShadingMaterialPolicy::material_id);
        return Result::Success();
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what()); }
}
Result D3D12Renderer::Impl::PrepareAmbientOcclusion(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent capacity, AmbientOcclusionCandidate &candidate, bool adaptive_compute) const
{
    return owner.ambient_occlusion.Prepare(device.Get(), allocator, capacity,
        candidate_config.ao_quality, QualityProfileFor(candidate_config.ao_quality), ResolveFeatures(candidate_config).ao_mode == GraphicsAoMode::GTAO,
        candidate_config.contact_shadows, kFrameCount,
        [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate, adaptive_compute);
}
Result D3D12Renderer::Impl::PrepareSurfaceLighting(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent capacity, SubsurfaceCandidate &diffusion, ScreenReflectionsCandidate &reflections) const
{
    const auto read_shader = [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
        RendererAssetSource path;
        if (auto resolved = ResolveRendererAsset(candidate_config, logical_id, path); !resolved) return resolved;
        return ReadBinary(path, bytes);
    };
    const bool have_subsurface = std::ranges::any_of(material_surface_layouts, [](const auto &layout) {
        return (layout.capability_mask & static_cast<std::uint32_t>(
            render_data::MaterialClosureCapability::Subsurface)) != 0;
    });
    if (auto prepared = owner.subsurface.Prepare(device.Get(), allocator, capacity, have_subsurface,
            kFrameCount, read_shader, diffusion); !prepared) return prepared;
    const bool screen_space = ResolveFeatures(candidate_config).reflection_mode == GraphicsReflectionMode::ScreenSpace;
    const auto quality = screen_space ? static_cast<GraphicsOptionalQuality>(
        static_cast<unsigned>(candidate_config.reflection_quality) + 1u) : GraphicsOptionalQuality::Off;
    return owner.screen_reflections.Prepare(device.Get(), allocator, capacity, quality, QualityProfileFor(candidate_config.reflection_quality),
        kFrameCount, read_shader, reflections);
}
Result D3D12Renderer::Impl::PrepareHeroTransparency(const OwnedView &owner, const RendererConfig &candidate_config,
    ViewExtent capacity, HeroTransparencyCandidate &candidate) const
{
    const bool authored_transmission = std::ranges::any_of(material_surface_layouts, [](const auto &layout) {
        return (layout.capability_mask & static_cast<std::uint32_t>(
            render_data::MaterialClosureCapability::SolidTransmission)) != 0;
    });
    const auto row = candidate_config.refraction_quality == GraphicsOptionalQuality::Off
        ? GraphicsQuality::Low : static_cast<GraphicsQuality>(static_cast<unsigned>(candidate_config.refraction_quality) - 1u);
    const auto layers = candidate_config.refraction_quality == GraphicsOptionalQuality::Off ? 0u :
        std::min(QualityProfileFor(row).hero_refraction_layers,
            QualityProfileFor(candidate_config.transparency_quality).hero_refraction_layers);
    const bool enabled = authored_transmission && layers != 0;
    return owner.hero_transparency.Prepare(device.Get(), allocator, root_signature.Get(), capacity,
        layers, enabled, kFrameCount,
        [&](std::string_view logical_id, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate_config, logical_id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, pipeline_cache, candidate, candidate_config.capture_hero_work, candidate_config.hero_work_mode);
}
} // namespace hs
