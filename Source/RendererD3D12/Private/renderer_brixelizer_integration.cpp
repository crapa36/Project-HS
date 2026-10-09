#include "renderer_impl.hpp"
#include "renderer_brixelizer_scene.hpp"
#include <hs/render_data/material_ir.hpp>

namespace hs
{
Result AppendBrixelizerLeaf(const RaySceneGeometry &source, const RaySceneInstance &instance,
    std::span<const render_data::GeometrySubmesh> submeshes,
    std::span<const render_data::CharacterMaterialBinding> bindings,
    std::span<const render_data::MaterialSurfaceLayout> hero_layouts,
    std::uint32_t environment_capabilities, bool masked, bool transmissive,
    DirectX::XMFLOAT3 bounds_min, DirectX::XMFLOAT3 bounds_max,
    std::uint64_t material_revision,
    BrixelizerGeometryLookup &lookup,
    std::vector<RaySceneGeometry> &geometries, std::vector<BrixelizerSceneInstance> &instances)
{
    constexpr auto thin = static_cast<std::uint32_t>(render_data::MaterialClosureCapability::ThinTransmission) |
        static_cast<std::uint32_t>(render_data::MaterialClosureCapability::SolidTransmission);
    for (std::size_t part = 0; part < submeshes.size(); ++part)
    {
        const auto &range = submeshes[part];
        auto domain = transmissive ? BrixelizerGeometryDomain::Translucent :
            masked ? BrixelizerGeometryDomain::UnsupportedMasked :
            (environment_capabilities & thin) ? BrixelizerGeometryDomain::UnsupportedThin :
            BrixelizerGeometryDomain::Opaque;
        auto material = instance.material_id;
        if (!bindings.empty())
        {
            const auto binding = std::ranges::find(bindings, range.material_index,
                &render_data::CharacterMaterialBinding::slot);
            if (binding == bindings.end() || binding->hero_ordinal >= hero_layouts.size())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Native SDF character submesh has no exact material binding.");
            material = 0x20000000u + binding->hero_ordinal;
            if (binding->domain == render_data::HeroMaterialDomain::Translucent)
                domain = BrixelizerGeometryDomain::Translucent;
            else if (render_data::kHeroMaterialNames[binding->hero_ordinal] == "archer_cutout")
                // The fixed hero catalog's authored alpha_mode=mask has no
                // coverage-triangle producer. Its source must not voxelize opaque.
                domain = BrixelizerGeometryDomain::UnsupportedMasked;
            else if (hero_layouts[binding->hero_ordinal].capability_mask & thin)
                domain = BrixelizerGeometryDomain::UnsupportedThin;
        }

        if (domain != BrixelizerGeometryDomain::Opaque && domain != BrixelizerGeometryDomain::AuthoredCoverage)
        {
            // Keep the exact submesh identity and authored material/domain for
            // native status accounting, while preventing unsupported triangles
            // from consuming a geometry or SDK instance slot.
            instances.push_back({instance.stable_gpu_scene_slot, instance.generation, UINT32_MAX,
                material, material_revision, source.current_revision, instance.world,
                bounds_min, bounds_max, domain});
            continue;
        }
        if (source.index_format != DXGI_FORMAT_R32_UINT && source.index_format != DXGI_FORMAT_R16_UINT)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Native SDF requires exact R16 or R32 triangle indices.");
        const auto index_stride = source.index_format == DXGI_FORMAT_R16_UINT ? 2u : 4u;
        if (std::uint64_t(range.first_index) + range.index_count > source.index_count ||
            !range.index_count || range.index_count % 3 || geometries.size() >= UINT32_MAX)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid native SDF submesh range.");
        auto geometry = source;
        geometry.stable_key = MakeAssetId("render/sdf/" + std::to_string(source.stable_key) +
            "/submesh/" + std::to_string(part)).value;
        geometry.index_byte_offset += std::uint64_t(range.first_index) * index_stride;
        geometry.index_count = range.index_count;
        geometry.opaque = domain == BrixelizerGeometryDomain::Opaque;
        const auto index = lookup.FindOrAdd(geometry, geometries);
        instances.push_back({instance.stable_gpu_scene_slot, instance.generation, index,
            material, material_revision, source.current_revision, instance.world,
            bounds_min, bounds_max, domain});
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareBrixelizerGi(const RendererConfig &candidate_config,
    ViewExtent capacity, BrixelizerCandidate &candidate) const
{
    BrixelizerSettings settings;
    settings.enabled = ResolveFeatures(candidate_config).software_gi_enabled;
    settings.quality = candidate_config.gi_quality;
    if (settings.enabled)
    {
        // The driver's current usage already includes live SDF/GI storage.
        // Prepare compares old+candidate service sizes, so restore exactly the
        // live service portion to the observed remaining native budget.
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        const auto queried = adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
        if (FAILED(queried)) return HResultFailure("Query software GI preparation budget", queried);
        const auto available = memory.Budget > memory.CurrentUsage ? memory.Budget - memory.CurrentUsage : 0;
        const auto live = brixelizer_gi.AllocatedBytes();
        if (available > UINT64_MAX - live)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Software GI preparation budget overflow.");
        settings.memory_budget_bytes = available + live;
        const auto &quality = QualityProfileFor(candidate_config.gi_quality);
        settings.logical_cascades = quality.gi_logical_cascades;
        settings.internal_resolution_percent = quality.gi_internal_resolution_percent;
    }
    return brixelizer_gi.Prepare(device.Get(), allocator, root_signature.Get(), capacity,
        kFrameCount, settings,
        [&](std::string_view id, std::vector<std::byte> &bytes) {
            std::filesystem::path path;
            if (auto resolved = candidate_config.ResolveProduct(id, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, candidate);
}
} // namespace hs
