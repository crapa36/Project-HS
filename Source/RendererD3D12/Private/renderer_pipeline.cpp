#include "renderer_impl.hpp"
#include "renderer_development_catalog.hpp"

namespace hs
{
Result D3D12Renderer::Impl::PreparePresentationGuides(const RendererConfig &candidate, ViewExtent output,
    ViewExtent capacity, const DisplayState &display, PresentationGuidesCandidate &guides) const
{
    if (!sdk_presentation.IsEnabled()) return Result::Success();
    return presentation_guides.Prepare(device.Get(), allocator, capacity, output, display.format, display.color_space,
        sdk_presentation.PresentationOwner(), kFrameCount,
        [&](std::string_view logical, std::vector<std::byte> &bytes) {
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(candidate, logical, path); !resolved) return resolved;
            return ReadBinary(path, bytes);
        }, guides);
}
Result D3D12Renderer::Impl::SelectTextureFeedbackShaders(const RendererConfig &request,
    RendererConfig &selected) const
{
    selected = request;
    if (request.packed_content) selected.selected_texture_feedback = false;
    if (!texture_streaming_capabilities.sampler_feedback || !texture_streaming_capabilities.tiled_resources)
        return Result::Success();
    // Resolve both physical variants before enabling compiled-pack selection.
    for (const std::string_view name : kAuthoredMaterialNames)
    {
        const bool masked = name.starts_with("leaf_") || name.starts_with("grass_");
        for (const std::string_view family : {"scene", "resolve", "probe_capture", "visibility", "shadow", "probe_shadow"})
        {
            if (!masked && (family == "visibility" || family == "shadow" || family == "probe_shadow")) continue;
            const auto baseline = "render/shader/" + std::string(name) + "_" + std::string(family) + ".dxil";
            const auto feedback = "render/shader/" + std::string(name) + "_" + std::string(family) + "_feedback.dxil";
            if (request.packed_content)
            {
                if (!FindRendererBulk(selected, baseline) || !FindRendererBulk(selected, feedback))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                        "Selected feedback shader or its baseline has no compiled bulk: " + feedback);
                continue;
            }
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(request, feedback, path); !resolved) return resolved;
            const auto digest = request.product_digests.find(feedback);
            if (digest == request.product_digests.end() || !selected.product_paths.contains(baseline) ||
                !selected.product_digests.contains(baseline))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Selected feedback shader or its baseline has no immutable product identity: " + feedback);
            selected.product_paths.at(baseline) = std::move(path.path);
            selected.product_digests.at(baseline) = digest->second;
        }
    }
    if (request.packed_content) selected.selected_texture_feedback = true;
    return Result::Success();
}
Result D3D12Renderer::Impl::CreatePipeline(const RendererConfig &request)
{
    RendererConfig pipeline_config;
    if (auto selected = SelectTextureFeedbackShaders(request, pipeline_config); !selected) return selected;
    mesh_pipeline_pairs.clear();
    virtual_shadow_indexed_pipelines.clear();
    virtual_shadow_mesh_pipelines.clear();
    D3D12_DESCRIPTOR_RANGE1 texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = kPostTextureDescriptorCount;
    texture_range.BaseShaderRegister = 2;
    texture_range.RegisterSpace = 0;
    texture_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                          D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    D3D12_DESCRIPTOR_RANGE1 character_range{};
    character_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    character_range.NumDescriptors = kCharacterDescriptorCount;
    character_range.BaseShaderRegister = 14;
    character_range.RegisterSpace = 0;
    character_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    D3D12_DESCRIPTOR_RANGE1 monster_pbr_range{};
    monster_pbr_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    monster_pbr_range.NumDescriptors = kMonsterPbrDescriptorCount;
    monster_pbr_range.BaseShaderRegister = 0;
    monster_pbr_range.RegisterSpace = 1;
    monster_pbr_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    monster_pbr_range.OffsetInDescriptorsFromTableStart = kCharacterDescriptorCount;
    D3D12_DESCRIPTOR_RANGE1 environment_range{};
    environment_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    environment_range.NumDescriptors = kEnvironmentDescriptorCount;
    environment_range.BaseShaderRegister = 0;
    environment_range.RegisterSpace = 2;
    environment_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                              D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    environment_range.OffsetInDescriptorsFromTableStart =
        kCharacterDescriptorCount + kMonsterPbrDescriptorCount;
    D3D12_DESCRIPTOR_RANGE1 gradient_range{};
    gradient_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    gradient_range.NumDescriptors = kVfxGradientDescriptorCount;
    gradient_range.BaseShaderRegister = 1;
    gradient_range.RegisterSpace = 3;
    // Shared bindings can precede the graph pass that first samples these textures.
    gradient_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    gradient_range.OffsetInDescriptorsFromTableStart =
        kCharacterDescriptorCount + kMonsterPbrDescriptorCount + kEnvironmentDescriptorCount;
    D3D12_DESCRIPTOR_RANGE1 ribbon_detail_range{};
    ribbon_detail_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ribbon_detail_range.NumDescriptors = 1;
    ribbon_detail_range.BaseShaderRegister = 6;
    ribbon_detail_range.RegisterSpace = 3;
    ribbon_detail_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    ribbon_detail_range.OffsetInDescriptorsFromTableStart =
        gradient_range.OffsetInDescriptorsFromTableStart + kVfxGradientDescriptorCount;
    D3D12_DESCRIPTOR_RANGE1 impact_range = ribbon_detail_range;
    impact_range.NumDescriptors = 2;
    impact_range.BaseShaderRegister = 7;
    impact_range.OffsetInDescriptorsFromTableStart = ribbon_detail_range.OffsetInDescriptorsFromTableStart + 1;
    D3D12_DESCRIPTOR_RANGE1 smoke_range = impact_range;
    smoke_range.NumDescriptors = 3;
    smoke_range.BaseShaderRegister = 12;
    smoke_range.OffsetInDescriptorsFromTableStart = impact_range.OffsetInDescriptorsFromTableStart + 3;
    D3D12_DESCRIPTOR_RANGE1 material_texture_range = environment_range;
    material_texture_range.NumDescriptors = kMaterialTextureDescriptorCount;
    material_texture_range.RegisterSpace = 4;
    material_texture_range.OffsetInDescriptorsFromTableStart =
        kMaterialTextureDescriptorOffset - kPostTextureDescriptorCount;
    D3D12_DESCRIPTOR_RANGE1 geometry_range = material_texture_range;
    geometry_range.NumDescriptors = kGeometryDescriptorCount;
    geometry_range.RegisterSpace = 5;
    geometry_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                           D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    geometry_range.OffsetInDescriptorsFromTableStart =
        kGeometryDescriptorOffset - kTemporalTextureDescriptorOffset;
    const std::array ranges{character_range, monster_pbr_range, environment_range,
                            gradient_range, ribbon_detail_range, impact_range, smoke_range,
                            material_texture_range};

    D3D12_DESCRIPTOR_RANGE1 curve_range = impact_range;
    curve_range.NumDescriptors = 1;
    curve_range.BaseShaderRegister = 9;
    curve_range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE1 distortion_range = curve_range;
    distortion_range.BaseShaderRegister = 15;
    distortion_range.NumDescriptors = 3;
    distortion_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_DESCRIPTOR_RANGE1 decal_range = distortion_range;
    decal_range.NumDescriptors = 2;
    decal_range.BaseShaderRegister = 20;
    decal_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_DESCRIPTOR_RANGE1 bloom_range = distortion_range;
    bloom_range.BaseShaderRegister = 22;
    bloom_range.NumDescriptors = 3;
    D3D12_DESCRIPTOR_RANGE1 temporal_input_range = bloom_range;
    temporal_input_range.BaseShaderRegister = 26;
    temporal_input_range.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE1 temporal_output_range = temporal_input_range;
    temporal_output_range.BaseShaderRegister = 27;
    // Reuse the fixed frame-relative history table for geometry without adding
    // a 65th root DWORD. Mutable records and pass-transitioned page data use
    // volatile declarations.
    D3D12_DESCRIPTOR_RANGE1 surface_range = geometry_range;
    surface_range.NumDescriptors = kSurfaceDescriptorCount;
    surface_range.BaseShaderRegister = 0;
    surface_range.RegisterSpace = 6;
    surface_range.OffsetInDescriptorsFromTableStart =
        kSurfaceDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 visibility_srv_range{};
    visibility_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    visibility_srv_range.NumDescriptors = kVisibilitySrvDescriptorCount;
    visibility_srv_range.BaseShaderRegister = 0;
    visibility_srv_range.RegisterSpace = 7;
    visibility_srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                 D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    visibility_srv_range.OffsetInDescriptorsFromTableStart =
        kVisibilitySrvDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 visibility_uav_range{};
    visibility_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    visibility_uav_range.NumDescriptors = kVisibilityUavDescriptorCount;
    visibility_uav_range.BaseShaderRegister = 0;
    visibility_uav_range.RegisterSpace = 7;
    visibility_uav_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                 D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    visibility_uav_range.OffsetInDescriptorsFromTableStart =
        kVisibilityUavDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 material_parameter_range = geometry_range;
    material_parameter_range.NumDescriptors = 1;
    material_parameter_range.BaseShaderRegister = 0;
    material_parameter_range.RegisterSpace = 8;
    material_parameter_range.OffsetInDescriptorsFromTableStart =
        kMaterialParameterDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 view_range = geometry_range;
    view_range.NumDescriptors = 5;
    view_range.BaseShaderRegister = 0;
    view_range.RegisterSpace = 9;
    view_range.OffsetInDescriptorsFromTableStart = kViewDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 exposure_srv_range = view_range;
    exposure_srv_range.NumDescriptors = 2;
    exposure_srv_range.RegisterSpace = 10;
    exposure_srv_range.OffsetInDescriptorsFromTableStart += 5;
    D3D12_DESCRIPTOR_RANGE1 exposure_uav_range = exposure_srv_range;
    exposure_uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    exposure_uav_range.OffsetInDescriptorsFromTableStart += 2;
    D3D12_DESCRIPTOR_RANGE1 exposure_cbv_range = exposure_uav_range;
    exposure_cbv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    exposure_cbv_range.NumDescriptors = 1;
    exposure_cbv_range.OffsetInDescriptorsFromTableStart += 2;
    D3D12_DESCRIPTOR_RANGE1 monster_skin_range = geometry_range;
    monster_skin_range.NumDescriptors = 1;
    monster_skin_range.BaseShaderRegister = 18;
    monster_skin_range.RegisterSpace = 0;
    monster_skin_range.OffsetInDescriptorsFromTableStart =
        kViewDescriptorOffset + 10 - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 ambient_range = view_range;
    ambient_range.NumDescriptors = 2;
    ambient_range.BaseShaderRegister = 5;
    ambient_range.OffsetInDescriptorsFromTableStart += 11;
    D3D12_DESCRIPTOR_RANGE1 hero_binding_range = material_parameter_range;
    hero_binding_range.BaseShaderRegister = 1;
    hero_binding_range.OffsetInDescriptorsFromTableStart = kHeroBindingDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 probe_range = material_parameter_range;
    probe_range.NumDescriptors = 3;
    probe_range.BaseShaderRegister = 0;
    probe_range.RegisterSpace = 15;
    probe_range.OffsetInDescriptorsFromTableStart = kProbeDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 light_range = probe_range;
    light_range.NumDescriptors = 6;
    light_range.RegisterSpace = 19;
    light_range.OffsetInDescriptorsFromTableStart = kLightListDescriptorOffset - kTemporalTextureDescriptorOffset;
    const D3D12_DESCRIPTOR_RANGE1 scene_geometry_ranges[]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 20,
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
            kSceneGeometryDescriptorOffset - kTemporalTextureDescriptorOffset},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 20,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kSceneGeometryDescriptorOffset - kTemporalTextureDescriptorOffset + 1},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 14, 20,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kSceneGeometryDescriptorOffset - kTemporalTextureDescriptorOffset + 2},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 20,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kSceneGeometryDescriptorOffset - kTemporalTextureDescriptorOffset + 3}};
    const std::array history_geometry_ranges{
        temporal_input_range, geometry_range, surface_range,
        visibility_srv_range, visibility_uav_range, material_parameter_range,
        view_range, exposure_srv_range, exposure_uav_range, exposure_cbv_range, monster_skin_range, ambient_range, hero_binding_range, probe_range, light_range,
        scene_geometry_ranges[0], scene_geometry_ranges[1], scene_geometry_ranges[2], scene_geometry_ranges[3],
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kVirtualShadowDescriptorCount, 0, 21,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kVirtualShadowDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 23,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kLocalProbeDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 24,
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
            kVolumetricDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 14, 0, 24,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kVolumetricDescriptorOffset - kTemporalTextureDescriptorOffset + 1},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 9, 0, 24,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kVolumetricDescriptorOffset - kTemporalTextureDescriptorOffset + 15},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 14, 24,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kVolumetricDescriptorOffset - kTemporalTextureDescriptorOffset + 24},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 25,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kHlodDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kLocalShadowDescriptorCount, 0, 27,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kLocalShadowDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 31,
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
            kBrixelizerDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 15, 0, 31,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kBrixelizerDescriptorOffset - kTemporalTextureDescriptorOffset + 1},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 9, 0, 31,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kBrixelizerDescriptorOffset - kTemporalTextureDescriptorOffset + 16},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 32,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kTextureFeedbackDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kTextureFeedbackBindingCount, 0, 32,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kTextureFeedbackDescriptorOffset - kTemporalTextureDescriptorOffset + 1},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 32,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kVfxScreenReadDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 35,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kEntitySurfaceNoiseDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 36,
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
            kDecalSurfaceDescriptorOffset - kTemporalTextureDescriptorOffset + kDecalSurfaceCbvOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kDecalSurfaceSrvCount, 0, 36,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kDecalSurfaceDescriptorOffset - kTemporalTextureDescriptorOffset + kDecalSurfaceSrvOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kDecalSurfaceUavCount, 0, 36,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kDecalSurfaceDescriptorOffset - kTemporalTextureDescriptorOffset + kDecalSurfaceUavOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kDecalSurfaceExportCount, 10, 36,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kDecalSurfaceDescriptorOffset - kTemporalTextureDescriptorOffset + kDecalSurfaceExportOffset}};
    D3D12_DESCRIPTOR_RANGE1 sampler_range{};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = 2;
    sampler_range.BaseShaderRegister = 3;
    sampler_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    const D3D12_DESCRIPTOR_RANGE1 hero_transparency_ranges[]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 14,
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 21, 0, 14,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 1},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 14,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 22}};
    // Existing parameters consume 63 DWORDs. One combined table uses the last
    // DWORD while retaining the immutable per-phase CBV/SRV/UAV contracts.
    std::array<D3D12_ROOT_PARAMETER1, 37> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &sampler_range};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[3].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[4].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[5].DescriptorTable = {1, &texture_range};
    parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[6].Constants = {1, 0, 2};
    parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        auto &parameter = parameters[7 + index];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor = {2 + index, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[11].Descriptor = {12, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[12].Descriptor = {13, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[13].DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
    parameters[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[14].Descriptor = {17, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[15].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[16].Descriptor = {0, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[16].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[17].Descriptor = {2, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[17].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[18].Descriptor = {3, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[18].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[19].Descriptor = {4, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[19].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[20].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[20].Descriptor = {5, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[20].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[21].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[21].Descriptor = {1, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[21].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[22].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[22].Descriptor = {2, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[22].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[23].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[23].Descriptor = {3, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[23].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[24].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[24].DescriptorTable = {1, &curve_range};
    parameters[24].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[25].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[25].Descriptor = {11, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[25].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    parameters[26].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[26].DescriptorTable = {1, &distortion_range};
    parameters[26].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[27].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[27].Descriptor = {18, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[27].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    parameters[28].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[28].Descriptor = {19, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[28].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[29].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[29].DescriptorTable = {1, &decal_range};
    parameters[29].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[30].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[30].DescriptorTable = {1, &bloom_range};
    parameters[30].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[31].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[31].Descriptor = {25, 3,
        D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[31].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[32].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[32].DescriptorTable = {static_cast<UINT>(history_geometry_ranges.size()),
                                     history_geometry_ranges.data()};
    parameters[32].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[33].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[33].DescriptorTable = {1, &temporal_output_range};
    parameters[33].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[34].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[34].Descriptor = {28, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[34].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[35].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[35].Descriptor = {29, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[35].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[36].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[36].DescriptorTable = {3, hero_transparency_ranges};
    parameters[36].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 3> samplers{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[2] = samplers[0];
    samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].ShaderRegister = 2;
    samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_description{};
    root_description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_description.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    root_description.Desc_1_1.pParameters = parameters.data();
    root_description.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    root_description.Desc_1_1.pStaticSamplers = samplers.data();
    root_description.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> error_blob;
    auto result = D3D12SerializeVersionedRootSignature(&root_description, &root_blob, &error_blob);
    if (FAILED(result))
    {
        const auto message = error_blob
                                 ? std::string(static_cast<const char *>(error_blob->GetBufferPointer()),
                                               error_blob->GetBufferSize())
                                 : "unknown root signature error";
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", message);
    }
    result = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature));
    if (FAILED(result))
    {
        return HResultFailure("CreateRootSignature", result);
    }
    if (auto registered = pipeline_cache.RegisterRootSignature(root_signature.Get(),
            {static_cast<const std::byte *>(root_blob->GetBufferPointer()), root_blob->GetBufferSize()});
        !registered) return registered;

    const auto read_shader = [&](std::string_view name, std::vector<std::byte> &bytes) {
        RendererAssetSource path;
        if (auto resolved = ResolveRendererAsset(pipeline_config, "render/shader/" + std::string(name), path);
            !resolved) return resolved;
        return ReadBinary(path, bytes);
    };
    const auto read_material_shader = [&](std::string_view material_id, render_data::MaterialProgramKind kind,
        std::string_view loose_name, std::vector<std::byte> &bytes) {
        if (!pipeline_config.packed_content && !pipeline_config.development_catalog)
            return read_shader(loose_name, bytes);
        const auto handle = FindRendererMaterial(pipeline_config, MakeAssetId(material_id));
        if (!handle) return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Missing shader material descriptor.");
        const auto &material = pipeline_config.packed_materials[handle.value];
        const auto program = std::ranges::find(material.programs, kind, &render_data::MaterialProgramDescriptor::kind);
        if (program == material.programs.end())
            return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Missing declared material program.");
        if (pipeline_config.development_catalog)
        {
            if (program->product_id != "render/shader/" + std::string(loose_name))
                return Result::Failure(ErrorCode::InvalidState, "RendererDevelopmentCatalog", "Material program identity differs from selected shader.");
            return read_shader(loose_name, bytes);
        }
        StartupOperationScope timing(RendererStartupOperation::CheckedBinaryRead);
        return ReadRendererMaterialProgram(pipeline_config, *program, bytes);
    };
    std::vector<std::byte> scene_vertex;
    std::vector<std::byte> cluster_vertex, cluster_shadow_vertex;
    std::vector<std::byte> scene_mesh;
    std::vector<std::byte> shadow_mesh;
    std::vector<std::byte> scene_pixel;
    std::vector<std::byte> hero_scene_pixel, hero_visibility_pixel, hero_shadow_pixel;
    std::vector<std::byte> hero_resolve_compute, hero_translucent_pixel;
    std::array<std::vector<std::byte>, kAuthoredMaterialNames.size()> material_scene_pixels;
    std::array<std::vector<std::byte>, kAuthoredMaterialNames.size()> material_shadow_pixels;
    std::vector<std::byte> visibility_pixel;
    std::array<std::vector<std::byte>, kAuthoredMaterialNames.size()>
        material_visibility_pixels;
    std::vector<std::byte> surface_resolve_compute;
    std::array<std::vector<std::byte>, kAuthoredMaterialNames.size()>
        material_resolve_compute;
    std::vector<std::byte> shadow_vertex;
    std::vector<std::byte> shadow_pixel;
    std::vector<std::byte> environment_opaque_shadow_pixel;
    std::vector<std::byte> particle_vertex;
    std::vector<std::byte> ground_ring_vertex;
    std::vector<std::byte> ground_add_pixel;
    std::vector<std::byte> owner_mesh_vertex;
    std::vector<std::byte> owner_mesh_pixel;
    std::vector<std::byte> vfx_distortion_vertex, vfx_distortion_pixel;
    std::vector<std::byte> vfx_flash_vertex;
    std::vector<std::byte> vfx_flash_pixel;
    std::vector<std::byte> vfx_sprite_oit_pixel;
    std::vector<std::byte> particle_pixel;
    std::vector<std::byte> slime_pixel;
    std::vector<std::byte> fresnel_shell_vertex;
    std::vector<std::byte> fresnel_shell_pixel;
    std::vector<std::byte> particle_compute;
    std::vector<std::byte> static_visibility_compute;
    std::vector<std::byte> visibility_retest_compute;
    std::vector<std::byte> depth_pyramid_init_compute;
    std::vector<std::byte> depth_pyramid_reduce_compute;
    std::vector<std::byte> skin_cache_compute;
    std::vector<std::byte> skin_bounds_compute;
    std::vector<std::byte> raster_records_compute;
    std::vector<std::byte> visibility_clear_compute;
    std::vector<std::byte> visibility_count_compute;
    std::vector<std::byte> visibility_prefix_compute;
    std::vector<std::byte> visibility_scatter_compute;
    std::vector<std::byte> full_screen_vertex;
    std::vector<std::byte> temporal_pixel;
    std::vector<std::byte> pre_exposure_pixel, exposure_clear, exposure_build, exposure_reduce;
    std::vector<std::byte> deferred_pixel;
    std::vector<std::byte> composite_pixel;
    std::vector<std::byte> bloom_extract_pixel;
    std::vector<std::byte> bloom_downsample_pixel;
    std::vector<std::byte> bloom_upsample_pixel;
    std::vector<std::byte> bloom_pixel;
    std::vector<std::byte> tone_map_pixel;
    std::vector<std::byte> outline_pixel;
    std::vector<std::byte> fxaa_pixel;
    std::vector<std::byte> ui_pixel;
    std::vector<std::byte> ribbon_update;
    std::vector<std::byte> ribbon_args;
    std::vector<std::byte> ribbon_vs;
    std::vector<std::byte> ribbon_add_ps;
    std::vector<std::byte> ribbon_oit_ps;
    for (auto [name, bytes] : {
             std::pair{"scene_vs.dxil", &scene_vertex},
             std::pair{"cluster_scene_vs.dxil", &cluster_vertex},
             std::pair{"cluster_shadow_vs.dxil", &cluster_shadow_vertex},
             std::pair{"shadow_vs.dxil", &shadow_vertex},
             std::pair{"shadow_ps.dxil", &shadow_pixel},
             std::pair{"environment_opaque_shadow_ps.dxil", &environment_opaque_shadow_pixel},
             std::pair{"particle_vs.dxil", &particle_vertex},
              std::pair{"ground_ring_vs.dxil", &ground_ring_vertex},
              std::pair{"ground_add_ps.dxil", &ground_add_pixel},
              std::pair{"owner_mesh_vs.dxil", &owner_mesh_vertex},
              std::pair{"owner_mesh_ps.dxil", &owner_mesh_pixel},
             std::pair{"vfx_distortion_vs.dxil", &vfx_distortion_vertex},
             std::pair{"vfx_distortion_ps.dxil", &vfx_distortion_pixel},
             std::pair{"vfx_flash_vs.dxil", &vfx_flash_vertex},
             std::pair{"vfx_flash_ps.dxil", &vfx_flash_pixel},
             std::pair{"vfx_sprite_oit_ps.dxil", &vfx_sprite_oit_pixel},
             std::pair{"particle_ps.dxil", &particle_pixel},
             std::pair{"slime_ps.dxil", &slime_pixel},
             std::pair{"fresnel_shell_vs.dxil", &fresnel_shell_vertex},
             std::pair{"fresnel_shell_ps.dxil", &fresnel_shell_pixel},
             std::pair{"particle_cs.dxil", &particle_compute},
             std::pair{"static_visibility_cs.dxil", &static_visibility_compute},
             std::pair{"visibility_retest_cs.dxil", &visibility_retest_compute},
             std::pair{"depth_pyramid_init_cs.dxil", &depth_pyramid_init_compute},
             std::pair{"depth_pyramid_reduce_cs.dxil", &depth_pyramid_reduce_compute},
             std::pair{"skin_cache_cs.dxil", &skin_cache_compute},
             std::pair{"skin_bounds_cs.dxil", &skin_bounds_compute},
             std::pair{"fullscreen_vs.dxil", &full_screen_vertex},
             std::pair{"temporal_ps.dxil", &temporal_pixel},
             std::pair{"pre_exposure_ps.dxil", &pre_exposure_pixel},
             std::pair{"exposure_clear_cs.dxil", &exposure_clear},
             std::pair{"exposure_build_cs.dxil", &exposure_build},
             std::pair{"exposure_reduce_cs.dxil", &exposure_reduce},
             std::pair{"composite_ps.dxil", &composite_pixel},
             std::pair{"bloom_extract_ps.dxil", &bloom_extract_pixel},
             std::pair{"bloom_downsample_ps.dxil", &bloom_downsample_pixel},
             std::pair{"bloom_upsample_ps.dxil", &bloom_upsample_pixel},
             std::pair{"bloom_ps.dxil", &bloom_pixel},
             std::pair{"tonemap_ps.dxil", &tone_map_pixel},
             std::pair{"outline_ps.dxil", &outline_pixel},
             std::pair{"fxaa_ps.dxil", &fxaa_pixel},
             std::pair{"ui_ps.dxil", &ui_pixel},
             std::pair{"ribbon_update_cs.dxil", &ribbon_update},
             std::pair{"ribbon_args_cs.dxil", &ribbon_args},
             std::pair{"ribbon_vs.dxil", &ribbon_vs},
             std::pair{"ribbon_add_ps.dxil", &ribbon_add_ps},
             std::pair{"ribbon_oit_ps.dxil", &ribbon_oit_ps},
         })
    {
        if (auto read = read_shader(name, *bytes); !read)
        {
            return read;
        }
    }
    // Direct evaluation and the sampled LUT are pinned to the same immutable
    // selected Generation, including during development Generation reload.
    RendererAssetSource deferred_path;
    if (auto resolved = ResolveRendererAsset(pipeline_config, "render/shader/deferred_ps.dxil", deferred_path);
        !resolved) return resolved;
    if (auto read = ReadBinary(deferred_path, deferred_pixel); !read) return read;
    if (auto read = read_material_shader("render/material/archer_skin/material.rpkg",
        render_data::MaterialProgramKind::HeroShadowPS, "archer_skin_shadow.dxil", hero_shadow_pixel); !read) return read;
    if (auto read = read_material_shader("render/material/slime_body/material.rpkg",
        render_data::MaterialProgramKind::HeroTranslucentPS, "slime_body_translucent.dxil", hero_translucent_pixel); !read) return read;
    if (config.surface_frontend == SurfaceFrontend::CompactDeferred)
    {
        if (auto read = read_material_shader("render/material/archer_skin/material.rpkg",
            render_data::MaterialProgramKind::HeroScenePS, "archer_skin_scene.dxil", hero_scene_pixel); !read) return read;
    }
    else
    {
        if (auto read = read_material_shader("render/material/archer_skin/material.rpkg",
            render_data::MaterialProgramKind::HeroVisibilityPS, "archer_skin_visibility.dxil", hero_visibility_pixel); !read) return read;
        if (auto read = read_material_shader("render/material/archer_skin/material.rpkg",
            render_data::MaterialProgramKind::HeroSurfaceResolveCS, "archer_skin_resolve.dxil", hero_resolve_compute); !read) return read;
    }
    if (config.surface_frontend == SurfaceFrontend::CompactDeferred)
        if (auto read = read_shader("scene_ps.dxil", scene_pixel); !read) return read;
    for (std::size_t i = 0; i < kAuthoredMaterialNames.size(); ++i)
    {
        const std::string base = "render/shader/" +
            std::string(kAuthoredMaterialNames[i]);
        const auto material_id = (i == kTerrainMaterialOrdinal || i == render_data::kGroundDetailMaterialOrdinal
            ? "render/material/" : "render/") + std::string(kAuthoredMaterialNames[i]) + ".rpkg";
        const bool masked = base.starts_with("render/shader/leaf_") || base.starts_with("render/shader/grass_");
        if (config.surface_frontend == SurfaceFrontend::CompactDeferred)
        {
            if (auto read = read_material_shader(material_id, masked ? render_data::MaterialProgramKind::MaskedScenePS :
                render_data::MaterialProgramKind::OpaqueScenePS, std::string(kAuthoredMaterialNames[i]) + "_scene.dxil", material_scene_pixels[i]); !read)
                return read;
        }
        if (base.starts_with("render/shader/leaf_") ||
            base.starts_with("render/shader/grass_"))
        {
            if (auto read = read_material_shader(material_id, render_data::MaterialProgramKind::MaskedShadowPS,
                std::string(kAuthoredMaterialNames[i]) + "_shadow.dxil", material_shadow_pixels[i]); !read)
                return read;
        }
        if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
        {
            for (auto [logical_id, bytes] : {
                     std::pair{base + "_visibility.dxil", &material_visibility_pixels[i]},
                     std::pair{base + "_resolve.dxil", &material_resolve_compute[i]}})
            {
                RendererAssetSource shader_path;
                if (auto resolved = ResolveRendererAsset(pipeline_config, logical_id, shader_path); !resolved)
                    return resolved;
                if (auto read = ReadBinary(shader_path, *bytes); !read)
                    return read;
            }
        }
    }

    if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
    {
        for (auto [name, bytes] : {
                 std::pair{"visibility_ps.dxil", &visibility_pixel},
                 std::pair{"surface_resolve_cs.dxil", &surface_resolve_compute},
                 std::pair{"raster_records_cs.dxil", &raster_records_compute},
                 std::pair{"visibility_clear_cs.dxil", &visibility_clear_compute},
                 std::pair{"visibility_count_cs.dxil", &visibility_count_compute},
                 std::pair{"visibility_prefix_cs.dxil", &visibility_prefix_compute},
                 std::pair{"visibility_scatter_cs.dxil", &visibility_scatter_compute}})
        {
            if (auto read = read_shader(name, *bytes); !read)
                return read;
        }
    }

    ComPtr<ID3D12Device2> mesh_device;
    if (mesh_shader_enabled)
    {
        if (auto read = read_shader("scene_ms.dxil", scene_mesh); !read)
            return read;
        if (auto read = read_shader("shadow_ms.dxil", shadow_mesh); !read)
            return read;
        result = device.As(&mesh_device);
        if (FAILED(result)) return HResultFailure("Query mesh pipeline device", result);
    }
    const auto is_conventional_shadow = [](const D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics) {
        return graphics.NumRenderTargets == 0 && graphics.DSVFormat == kDepthFormat &&
               graphics.DepthStencilState.DepthEnable &&
               graphics.DepthStencilState.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL &&
               graphics.DepthStencilState.DepthFunc == D3D12_COMPARISON_FUNC_GREATER_EQUAL &&
               graphics.RasterizerState.FillMode == D3D12_FILL_MODE_SOLID &&
               graphics.RasterizerState.DepthClipEnable &&
               graphics.RasterizerState.DepthBias == -800 &&
               graphics.RasterizerState.SlopeScaledDepthBias == -1.5f;
    };
    const auto create_shadow_graphics = [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC *graphics,
                                            REFIID iid, void **output) -> HRESULT {
        const auto created = pipeline_cache.CreateGraphics(graphics, iid, output);
        if (FAILED(created) || !is_conventional_shadow(*graphics)) return created;
        auto virtual_graphics = *graphics;
        virtual_graphics.RasterizerState.DepthBias = 0;
        ComPtr<ID3D12PipelineState> virtual_pipeline;
        const auto virtual_created = pipeline_cache.CreateGraphics(
            &virtual_graphics, IID_PPV_ARGS(&virtual_pipeline));
        if (FAILED(virtual_created)) return virtual_created;
        virtual_shadow_indexed_pipelines.insert_or_assign(
            static_cast<ID3D12PipelineState *>(*output), std::move(virtual_pipeline));
        return created;
    };
    const auto create_mesh_pair = [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics,
                                     ID3D12PipelineState *indexed,
                                     const std::vector<std::byte> &shader) -> Result {
        if (!mesh_shader_enabled) return Result::Success();
        MeshPipelinePair pair;
        pair.indexed = indexed;
        const auto created = pipeline_cache.CreateMesh(mesh_device.Get(), graphics,
            {shader.data(), shader.size()}, pair.mesh.ReleaseAndGetAddressOf());
        if (FAILED(created)) return HResultFailure("Create mesh frontend PSO", created);
        if (is_conventional_shadow(graphics))
        {
            auto virtual_graphics = graphics;
            virtual_graphics.RasterizerState.DepthBias = 0;
            ComPtr<ID3D12PipelineState> virtual_pipeline;
            const auto virtual_created = pipeline_cache.CreateMesh(mesh_device.Get(), virtual_graphics,
                {shader.data(), shader.size()}, virtual_pipeline.ReleaseAndGetAddressOf());
            if (FAILED(virtual_created))
                return HResultFailure("Create virtual shadow mesh frontend PSO", virtual_created);
            virtual_shadow_mesh_pipelines.insert_or_assign(indexed, std::move(virtual_pipeline));
        }
        mesh_pipeline_pairs.push_back(std::move(pair));
        return Result::Success();
    };

    constexpr D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"MATERIAL", 0, DXGI_FORMAT_R16_UINT, 0, 72,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC scene{};
    scene.pRootSignature = root_signature.Get();
    scene.VS = {scene_vertex.data(), scene_vertex.size()};
    scene.BlendState.AlphaToCoverageEnable = FALSE;
    scene.BlendState.IndependentBlendEnable = FALSE;
    scene.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    scene.SampleMask = UINT_MAX;
    scene.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    scene.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    scene.RasterizerState.FrontCounterClockwise = FALSE;
    scene.RasterizerState.DepthClipEnable = TRUE;
    scene.DepthStencilState.DepthEnable = TRUE;
    scene.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    scene.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    scene.InputLayout = {input_layout, static_cast<UINT>(std::size(input_layout))};
    scene.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    scene.NumRenderTargets = render_data::SurfaceLayout::render_target_count +
                             material_sidecar_layout.plane_count;
    scene.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    scene.RTVFormats[1] = DXGI_FORMAT_R16G16_SNORM;
    // Preserve world position precision for deferred shadow comparisons.
    scene.RTVFormats[2] = DXGI_FORMAT_R32G32_UINT;
    scene.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;
    scene.RTVFormats[4] = DXGI_FORMAT_R16G16_FLOAT;
    scene.RTVFormats[5] = DXGI_FORMAT_R32G32_FLOAT;
    for (std::uint32_t plane = 0; plane < material_sidecar_layout.plane_count; ++plane)
        scene.RTVFormats[6 + plane] = MaterialSidecarFormat(plane);
    scene.DSVFormat = kDepthFormat;
    scene.SampleDesc = {1, 0};
    if (config.surface_frontend == SurfaceFrontend::CompactDeferred)
    {
        scene.PS = {scene_pixel.data(), scene_pixel.size()};
        result = pipeline_cache.CreateGraphics(&scene, IID_PPV_ARGS(&scene_pipeline));
        if (FAILED(result))
        {
            return HResultFailure("Create scene PSO", result);
        }
        if (auto created = create_mesh_pair(scene, scene_pipeline.Get(), scene_mesh); !created)
            return created;

    }

    auto shadow_state = scene;
    shadow_state.VS = {shadow_vertex.data(), shadow_vertex.size()};
    shadow_state.PS = {shadow_pixel.data(), shadow_pixel.size()};
    shadow_state.NumRenderTargets = 0;
    std::fill(std::begin(shadow_state.RTVFormats), std::end(shadow_state.RTVFormats),
              DXGI_FORMAT_UNKNOWN);
    // Reversed-Z stores larger depths toward the light. Bias casters away
    // from the light, matching the GREATER_EQUAL comparison and reversed projection.
    shadow_state.RasterizerState.DepthBias = -800;
    shadow_state.RasterizerState.SlopeScaledDepthBias = -1.5f;
    result = create_shadow_graphics(&shadow_state, IID_PPV_ARGS(&shadow_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create shadow PSO", result);
    }
    if (auto created = create_mesh_pair(shadow_state, shadow_pipeline.Get(), shadow_mesh); !created)
        return created;

    auto environment_shadow = shadow_state;
    environment_shadow.PS = {environment_opaque_shadow_pixel.data(),
                             environment_opaque_shadow_pixel.size()};
    environment_shadow.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    result = create_shadow_graphics(
        &environment_shadow, IID_PPV_ARGS(&environment_opaque_shadow_pipeline));
    if (FAILED(result)) return HResultFailure("Create opaque environment shadow PSO", result);
    if (auto created = create_mesh_pair(environment_shadow,
            environment_opaque_shadow_pipeline.Get(), shadow_mesh); !created) return created;
    for (std::size_t i = 0; i < kAuthoredMaterialNames.size(); ++i)
    {
        const std::string_view name = kAuthoredMaterialNames[i];
        if (!(name.starts_with("leaf_") || name.starts_with("grass_")))
        {
            // Cluster/HLOD mesh draws select by material; opaque materials
            // share the existing depth-only PSO and its mesh frontend pair.
            environment_material_shadow_pipelines[i] = environment_opaque_shadow_pipeline;
            continue;
        }
        auto material_shadow = environment_shadow;
        material_shadow.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        material_shadow.PS = {material_shadow_pixels[i].data(),
                              material_shadow_pixels[i].size()};
        result = create_shadow_graphics(
            &material_shadow, IID_PPV_ARGS(&environment_material_shadow_pipelines[i]));
        if (FAILED(result))
            return HResultFailure("Create " + std::string(name) + " shadow PSO", result);
        if (auto created = create_mesh_pair(material_shadow,
                environment_material_shadow_pipelines[i].Get(), shadow_mesh); !created)
            return created;
    }
    for (std::size_t i = 0; i < cluster_shadow_pipelines.size(); ++i)
    {
        const std::string_view name = kAuthoredMaterialNames[i];
        const bool masked = name.starts_with("leaf_") || name.starts_with("grass_");
        auto cluster_shadow = environment_shadow;
        cluster_shadow.VS = {cluster_shadow_vertex.data(), cluster_shadow_vertex.size()};
        cluster_shadow.InputLayout = {};
        cluster_shadow.RasterizerState.CullMode = masked ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
        const auto &pixel = masked ? material_shadow_pixels[i] : environment_opaque_shadow_pixel;
        cluster_shadow.PS = {pixel.data(), pixel.size()};
        result = create_shadow_graphics(&cluster_shadow, IID_PPV_ARGS(&cluster_shadow_pipelines[i]));
        if (FAILED(result)) return HResultFailure("Create cluster shadow PSO", result);
    }
    auto hero_shadow = environment_shadow;
    // HeroShadowPS discards backfaces; cull them before pixel shading.
    hero_shadow.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    hero_shadow.PS = {hero_shadow_pixel.data(), hero_shadow_pixel.size()};
    result = create_shadow_graphics(&hero_shadow, IID_PPV_ARGS(&hero_shadow_pipeline));
    if (FAILED(result)) return HResultFailure("Create hero shadow PSO", result);
    if (auto created = create_mesh_pair(hero_shadow, hero_shadow_pipeline.Get(), shadow_mesh); !created) return created;
    if (config.surface_frontend == SurfaceFrontend::CompactDeferred)
    {
        auto hero_scene = scene;
        // HeroScenePS discards backfaces; cull them before pixel shading.
        hero_scene.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        hero_scene.PS = {hero_scene_pixel.data(), hero_scene_pixel.size()};
        result = pipeline_cache.CreateGraphics(&hero_scene, IID_PPV_ARGS(&hero_scene_pipeline));
        if (FAILED(result)) return HResultFailure("Create hero surface PSO", result);
        if (auto created = create_mesh_pair(hero_scene, hero_scene_pipeline.Get(), scene_mesh); !created) return created;
        for (std::size_t i = 0; i < kAuthoredMaterialNames.size(); ++i)
        {
            const std::string_view name = kAuthoredMaterialNames[i];
            const bool masked = name.starts_with("leaf_") || name.starts_with("grass_");
            auto material_scene = scene;
            material_scene.RasterizerState.CullMode = masked
                ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
            material_scene.PS = {material_scene_pixels[i].data(), material_scene_pixels[i].size()};
            result = pipeline_cache.CreateGraphics(
                &material_scene, IID_PPV_ARGS(&environment_material_scene_pipelines[i]));
            if (FAILED(result))
                return HResultFailure("Create " + std::string(name) + " scene PSO", result);
            if (auto created = create_mesh_pair(material_scene,
                    environment_material_scene_pipelines[i].Get(), scene_mesh); !created)
                return created;
            if (i < cluster_scene_pipelines.size())
            {
                material_scene.VS = {cluster_vertex.data(), cluster_vertex.size()};
                material_scene.InputLayout = {};
                result = pipeline_cache.CreateGraphics(&material_scene, IID_PPV_ARGS(&cluster_scene_pipelines[i]));
                if (FAILED(result)) return HResultFailure("Create cluster surface PSO", result);
            }
        }
    }

    if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
    {
        auto visibility = scene;
        visibility.PS = {visibility_pixel.data(), visibility_pixel.size()};
        visibility.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        visibility.NumRenderTargets = 2;
        visibility.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
        visibility.RTVFormats[1] = DXGI_FORMAT_R8_UINT;
        std::fill(std::begin(visibility.RTVFormats) + 2,
                  std::end(visibility.RTVFormats), DXGI_FORMAT_UNKNOWN);
        visibility.BlendState.IndependentBlendEnable = TRUE;
        visibility.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        visibility.BlendState.RenderTarget[1].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        result = pipeline_cache.CreateGraphics(
            &visibility, IID_PPV_ARGS(&visibility_pipeline));
        if (FAILED(result)) return HResultFailure("Create visibility PSO", result);
        if (auto created = create_mesh_pair(visibility, visibility_pipeline.Get(), scene_mesh);
            !created)
            return created;
        auto hero_visibility = visibility;
        hero_visibility.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        hero_visibility.PS = {hero_visibility_pixel.data(), hero_visibility_pixel.size()};
        result = pipeline_cache.CreateGraphics(&hero_visibility, IID_PPV_ARGS(&hero_visibility_pipeline));
        if (FAILED(result)) return HResultFailure("Create hero visibility PSO", result);
        if (auto created = create_mesh_pair(hero_visibility, hero_visibility_pipeline.Get(), scene_mesh); !created) return created;

        for (std::size_t i = 0; i < kAuthoredMaterialNames.size(); ++i)
        {
            const std::string_view name = kAuthoredMaterialNames[i];
            const bool masked = name.starts_with("leaf_") || name.starts_with("grass_");
            auto material_visibility = visibility;
            material_visibility.RasterizerState.CullMode = masked
                ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
            material_visibility.PS = {material_visibility_pixels[i].data(),
                                      material_visibility_pixels[i].size()};
            result = pipeline_cache.CreateGraphics(
                &material_visibility,
                IID_PPV_ARGS(&environment_material_visibility_pipelines[i]));
            if (FAILED(result))
                return HResultFailure("Create " + std::string(name) + " visibility PSO", result);
            if (auto created = create_mesh_pair(material_visibility,
                    environment_material_visibility_pipelines[i].Get(), scene_mesh); !created)
                return created;
            if (i < cluster_visibility_pipelines.size())
            {
                material_visibility.VS = {cluster_vertex.data(), cluster_vertex.size()};
                material_visibility.InputLayout = {};
                result = pipeline_cache.CreateGraphics(&material_visibility, IID_PPV_ARGS(&cluster_visibility_pipelines[i]));
                if (FAILED(result)) return HResultFailure("Create cluster visibility PSO", result);
            }
        }
    }

    auto particle = scene;
    particle.VS = {particle_vertex.data(), particle_vertex.size()};
    particle.PS = {particle_pixel.data(), particle_pixel.size()};
    particle.InputLayout = {};
    particle.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    particle.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    particle.BlendState.RenderTarget[0].BlendEnable = TRUE;
    particle.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    particle.BlendState.IndependentBlendEnable = TRUE;
    particle.BlendState.RenderTarget[1].BlendEnable = TRUE;
    particle.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
    particle.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
    particle.BlendState.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ZERO;
    particle.BlendState.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    particle.NumRenderTargets = 2;
    particle.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    particle.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
    std::fill(std::begin(particle.RTVFormats) + 2, std::end(particle.RTVFormats),
              DXGI_FORMAT_UNKNOWN);
    result = pipeline_cache.CreateGraphics(&particle, IID_PPV_ARGS(&particle_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle PSO", result);
    }

    auto ground_ring = particle;
    ground_ring.VS = {ground_ring_vertex.data(), ground_ring_vertex.size()};
    result = pipeline_cache.CreateGraphics(&ground_ring,
                                                 IID_PPV_ARGS(&ground_ring_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create ground ring PSO", result);
    }

    auto owner_mesh = particle;
    owner_mesh.VS = {owner_mesh_vertex.data(), owner_mesh_vertex.size()};
    owner_mesh.PS = {owner_mesh_pixel.data(), owner_mesh_pixel.size()};
    result = pipeline_cache.CreateGraphics(&owner_mesh,
                                                 IID_PPV_ARGS(&owner_mesh_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create owner mesh PSO", result);
    }

    auto vfx_sprite_oit = particle;
    vfx_sprite_oit.VS = {vfx_flash_vertex.data(), vfx_flash_vertex.size()};
    vfx_sprite_oit.PS = {vfx_sprite_oit_pixel.data(), vfx_sprite_oit_pixel.size()};
    vfx_sprite_oit.DepthStencilState.DepthEnable = FALSE;
    vfx_sprite_oit.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    // Transparent pass binds the shared DSV; soft depth uses the G-buffer position.
    result = pipeline_cache.CreateGraphics(&vfx_sprite_oit, IID_PPV_ARGS(&vfx_sprite_oit_pipeline));
    if (FAILED(result)) return HResultFailure("Create VFX sprite OIT PSO", result);

    auto vfx_flash = scene;
    vfx_flash.VS = {vfx_flash_vertex.data(), vfx_flash_vertex.size()};
    vfx_flash.PS = {vfx_flash_pixel.data(), vfx_flash_pixel.size()};
    vfx_flash.InputLayout = {};
    vfx_flash.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    vfx_flash.DepthStencilState.DepthEnable = FALSE;
    vfx_flash.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    vfx_flash.DSVFormat = DXGI_FORMAT_UNKNOWN;
    vfx_flash.BlendState.RenderTarget[0].BlendEnable = TRUE;
    vfx_flash.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    vfx_flash.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    vfx_flash.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    vfx_flash.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    vfx_flash.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    vfx_flash.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    vfx_flash.NumRenderTargets = 1;
    std::fill(std::begin(vfx_flash.RTVFormats), std::end(vfx_flash.RTVFormats),
              DXGI_FORMAT_UNKNOWN);
    vfx_flash.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    result = pipeline_cache.CreateGraphics(&vfx_flash,
                                                 IID_PPV_ARGS(&vfx_flash_pipeline));
    if (FAILED(result)) return HResultFailure("Create VFX flash PSO", result);

    auto ground_add = vfx_flash;
    ground_add.VS = {ground_ring_vertex.data(), ground_ring_vertex.size()};
    ground_add.PS = {ground_add_pixel.data(), ground_add_pixel.size()};
    ground_add.DepthStencilState.DepthEnable = TRUE;
    ground_add.DSVFormat = kDepthFormat;
    result = pipeline_cache.CreateGraphics(&ground_add,
                                                 IID_PPV_ARGS(&ground_add_pipeline));
    if (FAILED(result)) return HResultFailure("Create additive ground PSO", result);

    auto distortion = vfx_flash;
    distortion.VS = {vfx_distortion_vertex.data(), vfx_distortion_vertex.size()};
    distortion.PS = {vfx_distortion_pixel.data(), vfx_distortion_pixel.size()};
    result = pipeline_cache.CreateGraphics(&distortion, IID_PPV_ARGS(&vfx_distortion_pipeline));
    if (FAILED(result)) return HResultFailure("Create VFX distortion PSO", result);

    auto slime = particle;
    slime.VS = scene.VS;
    slime.PS = {slime_pixel.data(), slime_pixel.size()};
    slime.InputLayout = scene.InputLayout;
    slime.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    result = pipeline_cache.CreateGraphics(&slime, IID_PPV_ARGS(&slime_pipeline));
    if (FAILED(result)) return HResultFailure("Create slime PSO", result);
    if (auto created = create_mesh_pair(slime, slime_pipeline.Get(), scene_mesh); !created)
        return created;
    slime.PS = {hero_translucent_pixel.data(), hero_translucent_pixel.size()};
    slime.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    result = pipeline_cache.CreateGraphics(&slime, IID_PPV_ARGS(&hero_translucent_pipeline));
    if (FAILED(result)) return HResultFailure("Create hero OIT PSO", result);
    if (auto created = create_mesh_pair(slime, hero_translucent_pipeline.Get(), scene_mesh); !created) return created;

    auto fresnel_shell = slime;
    fresnel_shell.VS = {fresnel_shell_vertex.data(), fresnel_shell_vertex.size()};
    fresnel_shell.PS = {fresnel_shell_pixel.data(), fresnel_shell_pixel.size()};
    result = pipeline_cache.CreateGraphics(&fresnel_shell,
                                                 IID_PPV_ARGS(&fresnel_shell_pipeline));
    if (FAILED(result)) return HResultFailure("Create Fresnel shell PSO", result);

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = root_signature.Get();
    compute.CS = {particle_compute.data(), particle_compute.size()};
    result =
        pipeline_cache.CreateCompute(&compute, IID_PPV_ARGS(&particle_compute_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle compute PSO", result);
    }
    compute.CS = {static_visibility_compute.data(), static_visibility_compute.size()};
    result = pipeline_cache.CreateCompute(
        &compute, IID_PPV_ARGS(&static_visibility_pipeline));
    if (FAILED(result)) return HResultFailure("Create static visibility PSO", result);
    compute.CS = {visibility_retest_compute.data(), visibility_retest_compute.size()};
    result = pipeline_cache.CreateCompute(
        &compute, IID_PPV_ARGS(&visibility_retest_pipeline));
    if (FAILED(result)) return HResultFailure("Create visibility retest PSO", result);
    compute.CS = {depth_pyramid_init_compute.data(), depth_pyramid_init_compute.size()};
    result = pipeline_cache.CreateCompute(
        &compute, IID_PPV_ARGS(&depth_pyramid_init_pipeline));
    if (FAILED(result)) return HResultFailure("Create depth pyramid init PSO", result);
    compute.CS = {depth_pyramid_reduce_compute.data(), depth_pyramid_reduce_compute.size()};
    result = pipeline_cache.CreateCompute(
        &compute, IID_PPV_ARGS(&depth_pyramid_reduce_pipeline));
    if (FAILED(result)) return HResultFailure("Create depth pyramid reduce PSO", result);
    compute.CS = {skin_cache_compute.data(), skin_cache_compute.size()};
    result = pipeline_cache.CreateCompute(&compute, IID_PPV_ARGS(&skin_cache_pipeline));
    if (FAILED(result)) return HResultFailure("Create skin cache PSO", result);
    compute.CS = {skin_bounds_compute.data(), skin_bounds_compute.size()};
    result = pipeline_cache.CreateCompute(&compute, IID_PPV_ARGS(&skin_bounds_pipeline));
    if (FAILED(result)) return HResultFailure("Create skin bounds PSO", result);
    if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
    {
        const auto create_visibility_compute =
            [&](std::span<const std::byte> shader,
                ComPtr<ID3D12PipelineState> &pipeline,
                std::string_view label) -> Result {
                compute.CS = {shader.data(), shader.size()};
                const auto create_result = pipeline_cache.CreateCompute(
                    &compute, IID_PPV_ARGS(pipeline.ReleaseAndGetAddressOf()));
                return SUCCEEDED(create_result) ? Result::Success()
                                                : HResultFailure(
                                                    "Create " + std::string(label) + " PSO",
                                                    create_result);
            };
        if (auto created = create_visibility_compute(surface_resolve_compute,
                surface_resolve_pipeline, "surface resolve"); !created)
            return created;
        if (auto created = create_visibility_compute(hero_resolve_compute,
                hero_resolve_pipeline, "hero surface resolve"); !created) return created;
        for (std::size_t i = 0; i < kAuthoredMaterialNames.size(); ++i)
        {
            if (auto created = create_visibility_compute(material_resolve_compute[i],
                    environment_material_resolve_pipelines[i],
                    std::string(kAuthoredMaterialNames[i]) + " resolve"); !created)
                return created;
        }
        if (auto created = create_visibility_compute(raster_records_compute,
                raster_records_pipeline, "raster records"); !created)
            return created;
        if (auto created = create_visibility_compute(visibility_clear_compute,
                visibility_clear_pipeline, "visibility clear"); !created)
            return created;
        if (auto created = create_visibility_compute(visibility_count_compute,
                visibility_count_pipeline, "visibility count"); !created)
            return created;
        if (auto created = create_visibility_compute(visibility_prefix_compute,
                visibility_prefix_pipeline, "visibility prefix"); !created)
            return created;
        if (auto created = create_visibility_compute(visibility_scatter_compute,
                visibility_scatter_pipeline, "visibility scatter"); !created)
            return created;
    }
    compute.CS = {ribbon_update.data(), ribbon_update.size()};
    result = pipeline_cache.CreateCompute(&compute, IID_PPV_ARGS(&ribbon_update_pipeline));
    if (FAILED(result)) return HResultFailure("Create ribbon update PSO", result);
    compute.CS = {ribbon_args.data(), ribbon_args.size()};
    result = pipeline_cache.CreateCompute(&compute, IID_PPV_ARGS(&ribbon_args_pipeline));
    if (FAILED(result)) return HResultFailure("Create ribbon args PSO", result);
    for (auto [shader, target] : {
            std::pair{std::span<const std::byte>(exposure_clear), exposure_clear_pipeline.ReleaseAndGetAddressOf()},
            std::pair{std::span<const std::byte>(exposure_build), exposure_build_pipeline.ReleaseAndGetAddressOf()},
            std::pair{std::span<const std::byte>(exposure_reduce), exposure_reduce_pipeline.ReleaseAndGetAddressOf()}})
    {
        compute.CS = {shader.data(), shader.size()};
        result = pipeline_cache.CreateCompute(&compute, IID_PPV_ARGS(target));
        if (FAILED(result)) return HResultFailure("Create exposure PSO", result);
    }

    auto ribbon_add = particle;
    ribbon_add.VS = {ribbon_vs.data(), ribbon_vs.size()};
    ribbon_add.PS = {ribbon_add_ps.data(), ribbon_add_ps.size()};
    ribbon_add.InputLayout = {};
    ribbon_add.BlendState.IndependentBlendEnable = FALSE;
    ribbon_add.NumRenderTargets = 1;
    ribbon_add.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    std::fill(std::begin(ribbon_add.RTVFormats) + 1, std::end(ribbon_add.RTVFormats),
              DXGI_FORMAT_UNKNOWN);
    ribbon_add.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    ribbon_add.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    ribbon_add.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    ribbon_add.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    result = pipeline_cache.CreateGraphics(&ribbon_add,
                                                  IID_PPV_ARGS(&ribbon_add_pipeline));
    if (FAILED(result)) return HResultFailure("Create ribbon add PSO", result);
    auto ribbon_oit = ribbon_add;
    ribbon_oit.PS = {ribbon_oit_ps.data(), ribbon_oit_ps.size()};
    ribbon_oit.BlendState.IndependentBlendEnable = TRUE;
    ribbon_oit.NumRenderTargets = 2;
    ribbon_oit.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ribbon_oit.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
    ribbon_oit.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    ribbon_oit.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    ribbon_oit.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
    ribbon_oit.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
    result = pipeline_cache.CreateGraphics(&ribbon_oit,
                                                  IID_PPV_ARGS(&ribbon_oit_pipeline));
    if (FAILED(result)) return HResultFailure("Create ribbon OIT PSO", result);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC post{};
    post.pRootSignature = root_signature.Get();
    post.VS = {full_screen_vertex.data(), full_screen_vertex.size()};
    post.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    post.SampleMask = UINT_MAX;
    post.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    post.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    post.RasterizerState.DepthClipEnable = TRUE;
    post.DepthStencilState.DepthEnable = FALSE;
    post.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    post.NumRenderTargets = 1;
    post.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    post.SampleDesc = {1, 0};
    auto create_post_pipeline = [&](std::span<const std::byte> pixel_shader,
                                    ID3D12PipelineState **pipeline) -> Result {
        post.PS = {pixel_shader.data(), pixel_shader.size()};
        const auto create_result = pipeline_cache.CreateGraphics(
            &post, IID_PPV_ARGS(pipeline));
        if (FAILED(create_result)) CountValidationErrors();
        return SUCCEEDED(create_result) ? Result::Success()
                                        : HResultFailure("Create post-process PSO", create_result);
    };
    post.NumRenderTargets = 4;
    for (const UINT mask : {0u, 1u, 2u, 3u, 7u})
    {
        for (UINT slot = 0; slot < 3; ++slot)
            post.RTVFormats[slot + 1] = mask & (1u << slot)
                ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_UNKNOWN;
        if (auto created = create_post_pipeline(deferred_pixel,
                deferred_pipeline[mask].ReleaseAndGetAddressOf()); !created) return created;
    }
    post.NumRenderTargets = 1;
    std::fill(std::begin(post.RTVFormats) + 1, std::end(post.RTVFormats), DXGI_FORMAT_UNKNOWN);
    for (auto [shader, pipeline] :
         std::array{
             std::pair{std::span<const std::byte>(composite_pixel),
                       composite_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(pre_exposure_pixel),
                       pre_exposure_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(bloom_extract_pixel),
                       bloom_extract_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(bloom_downsample_pixel),
                       bloom_downsample_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(bloom_upsample_pixel),
                       bloom_upsample_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(bloom_pixel),
                       bloom_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(tone_map_pixel),
                       tone_map_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(outline_pixel),
                       outline_pipeline.ReleaseAndGetAddressOf()},
         })
    {
        if (auto pipeline_result = create_post_pipeline(shader, pipeline); !pipeline_result)
        {
            return pipeline_result;
        }
    }

    post.NumRenderTargets = 3;
    post.RTVFormats[1] = DXGI_FORMAT_R16G16_SNORM;
    post.RTVFormats[2] = DXGI_FORMAT_R32G32_UINT;
    if (auto created = create_post_pipeline(temporal_pixel, temporal_pipeline.ReleaseAndGetAddressOf()); !created)
        return created;
    post.NumRenderTargets = 1;
    post.RTVFormats[1] = post.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    if (auto pipeline_result =
            create_post_pipeline(fxaa_pixel, fxaa_pipeline.ReleaseAndGetAddressOf());
        !pipeline_result)
    {
        return pipeline_result;
    }
    post.RTVFormats[0] = display_state.format;
    if (auto pipeline_result =
            create_post_pipeline(ui_pixel, ui_pipeline.ReleaseAndGetAddressOf());
        !pipeline_result)
    {
        return pipeline_result;
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    result = device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&draw_signature));
    if (FAILED(result)) return HResultFailure("CreateCommandSignature", result);
    std::array<D3D12_INDIRECT_ARGUMENT_DESC, 2> geometry_arguments{};
    geometry_arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    geometry_arguments[0].Constant = {6, 0, 1};
    geometry_arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    signature.ByteStride = sizeof(UINT) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    signature.NumArgumentDescs = static_cast<UINT>(geometry_arguments.size());
    signature.pArgumentDescs = geometry_arguments.data();
    result = device->CreateCommandSignature(&signature, root_signature.Get(),
        IID_PPV_ARGS(&indexed_geometry_signature));
    if (FAILED(result)) return HResultFailure("Create indexed geometry signature", result);
    if (mesh_shader_enabled)
    {
        geometry_arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        // Both GPU geometry frontends emit a 24-byte command slot.
        signature.ByteStride = sizeof(UINT) + sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        result = device->CreateCommandSignature(&signature, root_signature.Get(),
            IID_PPV_ARGS(&mesh_geometry_signature));
        if (FAILED(result)) return HResultFailure("Create mesh geometry signature", result);
    }
    if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
    {
        D3D12_INDIRECT_ARGUMENT_DESC dispatch_argument{};
        dispatch_argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC dispatch_signature_description{};
        dispatch_signature_description.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        dispatch_signature_description.NumArgumentDescs = 1;
        dispatch_signature_description.pArgumentDescs = &dispatch_argument;
        result = device->CreateCommandSignature(
            &dispatch_signature_description, nullptr, IID_PPV_ARGS(&dispatch_signature));
        if (FAILED(result)) return HResultFailure("Create visibility dispatch signature", result);
    }
    std::array<D3D12_INDIRECT_ARGUMENT_DESC, 2> ribbon_arguments{};
    ribbon_arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    ribbon_arguments[0].Constant.RootParameterIndex = 6;
    ribbon_arguments[0].Constant.DestOffsetIn32BitValues = 0;
    ribbon_arguments[0].Constant.Num32BitValuesToSet = 1;
    ribbon_arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    signature.ByteStride = kRibbonArgumentStride;
    signature.NumArgumentDescs = static_cast<UINT>(ribbon_arguments.size());
    signature.pArgumentDescs = ribbon_arguments.data();
    result = device->CreateCommandSignature(&signature, root_signature.Get(), IID_PPV_ARGS(&ribbon_draw_signature));
    if (FAILED(result)) return HResultFailure("CreateCommandSignature", result);
    return CreateClusterPipelines(pipeline_config);
}

Result D3D12Renderer::ReloadShaderGeneration(
    const std::string &generation_id,
    const std::unordered_map<std::string, std::filesystem::path> &product_paths,
    const std::unordered_map<std::string, std::string> &product_digests)
{
    if (!impl_->initialized || impl_->frame_submission_faulted)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Shader Generation adoption requires an active renderer.");
    if (generation_id.empty() || product_paths.size() != impl_->config.product_paths.size() ||
        product_digests.size() != product_paths.size() ||
        impl_->config.product_digests.size() != impl_->config.product_paths.size())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Shader Generation product identity is incomplete.");
    bool shaders_changed = false;
    bool parameters_changed = false;
    bool probe_changed = false;
    bool ltc_changed = false;
    const auto parameter_product = [](std::string_view id) {
        const auto matches = [&](std::string_view name) {
            return id == "render/material/" + std::string(name) + "/parameters.rpkg";
        };
        return std::ranges::any_of(kAuthoredMaterialNames, matches) ||
               std::ranges::any_of(kHeroMaterialNames, matches) ||
               std::ranges::any_of(render_data::kDecalMaterialNames, matches);
    };
    for (const auto &[id, previous_digest] : impl_->config.product_digests)
    {
        const auto found = product_digests.find(id);
        if (!product_paths.contains(id) || found == product_digests.end())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Shader Generation changes the product set: " + id);
        if (found->second == previous_digest) continue;
        if (id.starts_with("render/shader/")) shaders_changed = true;
        else if (parameter_product(id)) parameters_changed = true;
        else if (id == "render/lighting/default_probe.rpkg") probe_changed = true;
        else if (id == "render/lighting/default_ltc.rpkg") ltc_changed = true;
        else
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Shader Generation changes resident content: " + id);
    }
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
    candidate.product_paths = product_paths;
    candidate.product_digests = product_digests;
    try
    {
        candidate.development_catalog = RendererDevelopmentCatalog::Build(candidate);
        candidate.development_catalog->Bind(candidate);
    }
    catch (const std::exception &error)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "RendererDevelopmentCatalog", error.what());
    }
    if (parameters_changed)
        if (auto prepared = impl_->PrepareMaterialParameters(candidate, true); !prepared)
        {
            if (!impl_->material_parameter_upload_pending)
            {
                impl_->material_parameter_candidate.Reset();
                impl_->material_parameter_upload.Reset();
                impl_->material_parameter_upload_device.Reset();
            }
            return prepared;
        }
    if (!shaders_changed)
    {
        LightingProbeCandidate probe;
        LtcLutCandidate ltc;
        if (probe_changed)
            if (auto prepared = impl_->PrepareLightingProbe(candidate, probe); !prepared) return prepared;
        if (ltc_changed)
            if (auto prepared = impl_->PrepareLtcLut(candidate, ltc); !prepared) return prepared;
        if (probe_changed || ltc_changed)
            if (auto drained = impl_->WaitForGpu(); !drained) return drained;
        if (probe_changed)
            if (auto committed = impl_->lighting_probe.Commit(std::move(probe),
                    impl_->graph_queue_executor.Completed()); !committed) return committed;
        if (ltc_changed)
            if (auto committed = impl_->ltc_lut.Commit(std::move(ltc),
                    impl_->graph_queue_executor.Completed()); !committed) return committed;
        // Parameter/lighting-data adoption preserves the native root and PSOs.
        if (parameters_changed) impl_->CommitMaterialParameters();
        impl_->config = std::move(candidate);
        impl_->temporal_history_valid = false;
        impl_->hzb_history_valid = false;
        return Result::Success();
    }
    return impl_->ReloadPipeline(std::move(candidate));
}

Result D3D12Renderer::Impl::ReloadPipeline(RendererConfig candidate_config)
{
    AdaptiveShadingCandidate adaptive;
    if (auto prepared = PrepareAdaptiveShading(candidate_config, view_state.render_capacity,
            adaptive); !prepared) return prepared;
    ScreenOverlayCandidate screen_overlay;
    if (auto prepared = PrepareScreenOverlay(candidate_config, view_state.render_capacity,
            screen_overlay); !prepared) return prepared;
    DecalSurfaceCandidate decals;
    if (auto prepared = PrepareDecalSurface(candidate_config, view_state.render_capacity,
            false, decals); !prepared) return prepared;
    AmbientOcclusionCandidate ambient;
    SubsurfaceCandidate diffusion;
    ScreenReflectionsCandidate reflections;
    ReconstructionGuidesCandidate guides;
    PresentationGuidesCandidate presentation;
    if (auto prepared = PreparePresentationGuides(candidate_config, {width, height},
            view_state.render_capacity, display_state, presentation); !prepared) return prepared;
    HdrPostCandidate hdr_post;
    LightListCandidate light_list;
    if (auto prepared = PrepareLightList(candidate_config, view_state.render_capacity, light_list); !prepared)
        return prepared;
    if (auto prepared = PrepareHdrPost(candidate_config, view_state.render_capacity,
            {width, height}, hdr_post); !prepared) return prepared;
    SceneGeometryCandidate scene_geometry;
    if (auto prepared = PrepareSceneGeometry(candidate_config, scene_geometry); !prepared) return prepared;
    BrixelizerCandidate brixelizer;
    if (auto prepared = PrepareBrixelizerGi(candidate_config, view_state.render_active, brixelizer); !prepared) return prepared;
    VirtualShadowCandidate virtual_shadows;
    AdditionalShadowPrepared additional_shadows;
    if (auto prepared = PrepareAdditionalShadows(candidate_config, view_state.render_capacity,
            additional_shadows); !prepared) return prepared;
    VolumetricPrepared volumetrics;
    if (auto prepared = PrepareVolumetrics(candidate_config, view_state.render_capacity, volumetrics); !prepared) return prepared;
    LocalProbeRuntimeCandidate local_probes;
    if (auto prepared = PrepareLocalProbes(candidate_config, local_probes); !prepared) return prepared;
    if (auto prepared = PrepareVirtualShadows(candidate_config, view_state.render_capacity,
            virtual_shadows); !prepared) return prepared;
    LightingProbeCandidate probe;
    constexpr std::string_view probe_id = "render/lighting/default_probe.rpkg";
    const auto candidate_probe = RendererProductDigest(candidate_config, probe_id);
    const auto resident_probe = RendererProductDigest(config, probe_id);
    const bool replace_probe = !candidate_probe.empty() && candidate_probe != resident_probe;
    if (replace_probe)
        if (auto prepared = PrepareLightingProbe(candidate_config, probe); !prepared) return prepared;
    LtcLutCandidate ltc;
    constexpr std::string_view ltc_id = "render/lighting/default_ltc.rpkg";
    const auto candidate_ltc = RendererProductDigest(candidate_config, ltc_id);
    const auto resident_ltc = RendererProductDigest(config, ltc_id);
    const bool replace_ltc = !candidate_ltc.empty() && candidate_ltc != resident_ltc;
    if (replace_ltc)
        if (auto prepared = PrepareLtcLut(candidate_config, ltc); !prepared) return prepared;
    if (auto prepared = PrepareAmbientOcclusion(candidate_config, view_state.render_capacity, ambient,
            adaptive.Status().compute_ready); !prepared)
        return prepared;
    if (auto prepared = PrepareSurfaceLighting(candidate_config, view_state.render_capacity,
            diffusion, reflections); !prepared) return prepared;
    if (reconstruction_provider)
        if (auto prepared = PrepareReconstructionGuides(candidate_config, view_state.render_capacity,
                {width, height}, guides, reconstruction_provider->NeedsCompositionGuide()); !prepared) return prepared;
    if (sdk_presentation.IsEnabled())
    {
        std::vector<ID3D12CommandQueue *> queues;
        for (const auto hint : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
            if (auto *native = graph_queue_executor.NativeQueue(hint)) queues.push_back(native);
        if (auto retired = sdk_presentation.BeforeReuse(queues); !retired) return retired;
    }
    if (auto waited = WaitForGpu(); !waited)
    {
        if (!material_parameter_upload_pending) material_parameter_candidate.Reset();
        return waited;
    }
    auto previous_root = std::move(root_signature);
    auto previous_scene = std::move(scene_pipeline);
    auto previous_shadow = std::move(shadow_pipeline);
    auto previous_environment_opaque_shadow = std::move(environment_opaque_shadow_pipeline);
    auto previous_material_scenes = std::move(environment_material_scene_pipelines);
    std::array previous_heroes{std::move(hero_scene_pipeline), std::move(hero_visibility_pipeline),
        std::move(hero_shadow_pipeline), std::move(hero_resolve_pipeline), std::move(hero_translucent_pipeline)};
    auto previous_material_shadows = std::move(environment_material_shadow_pipelines);
    auto previous_visibility = std::move(visibility_pipeline);
    auto previous_material_visibility = std::move(environment_material_visibility_pipelines);
    auto previous_surface_resolve = std::move(surface_resolve_pipeline);
    auto previous_material_resolve = std::move(environment_material_resolve_pipelines);
    auto previous_raster_records = std::move(raster_records_pipeline);
    auto previous_visibility_clear = std::move(visibility_clear_pipeline);
    auto previous_visibility_count = std::move(visibility_count_pipeline);
    auto previous_visibility_prefix = std::move(visibility_prefix_pipeline);
    auto previous_visibility_scatter = std::move(visibility_scatter_pipeline);
    auto previous_particle = std::move(particle_pipeline);
    auto previous_ground_ring = std::move(ground_ring_pipeline);
    auto previous_ground_add = std::move(ground_add_pipeline);
    auto previous_owner_mesh = std::move(owner_mesh_pipeline);
    auto previous_vfx_distortion = std::move(vfx_distortion_pipeline);
    auto previous_vfx_flash = std::move(vfx_flash_pipeline);
    auto previous_vfx_sprite_oit = std::move(vfx_sprite_oit_pipeline);
    auto previous_slime = std::move(slime_pipeline);
    auto previous_fresnel_shell = std::move(fresnel_shell_pipeline);
    auto previous_particle_compute = std::move(particle_compute_pipeline);
    auto previous_static_visibility = std::move(static_visibility_pipeline);
    auto previous_visibility_retest = std::move(visibility_retest_pipeline);
    auto previous_depth_pyramid_init = std::move(depth_pyramid_init_pipeline);
    auto previous_depth_pyramid_reduce = std::move(depth_pyramid_reduce_pipeline);
    auto previous_skin_cache = std::move(skin_cache_pipeline);
    auto previous_skin_bounds = std::move(skin_bounds_pipeline);
    auto previous_deferred = std::move(deferred_pipeline);
    auto previous_composite = std::move(composite_pipeline);
    auto previous_temporal = std::move(temporal_pipeline);
    auto previous_pre_exposure = std::move(pre_exposure_pipeline);
    auto previous_exposure_clear = std::move(exposure_clear_pipeline);
    auto previous_exposure_build = std::move(exposure_build_pipeline);
    auto previous_exposure_reduce = std::move(exposure_reduce_pipeline);
    auto previous_bloom_extract = std::move(bloom_extract_pipeline);
    auto previous_bloom_downsample = std::move(bloom_downsample_pipeline);
    auto previous_bloom_upsample = std::move(bloom_upsample_pipeline);
    auto previous_bloom = std::move(bloom_pipeline);
    auto previous_tone_map = std::move(tone_map_pipeline);
    auto previous_outline = std::move(outline_pipeline);
    auto previous_fxaa = std::move(fxaa_pipeline);
    auto previous_ui = std::move(ui_pipeline);
    auto previous_ribbon_update = std::move(ribbon_update_pipeline);
    auto previous_ribbon_args = std::move(ribbon_args_pipeline);
    auto previous_ribbon_add = std::move(ribbon_add_pipeline);
    auto previous_ribbon_oit = std::move(ribbon_oit_pipeline);
    auto previous_signature = std::move(draw_signature);
    auto previous_indexed_signature = std::move(indexed_geometry_signature);
    auto previous_mesh_signature = std::move(mesh_geometry_signature);
    auto previous_cluster_root = std::move(cluster_root_signature);
    auto previous_cluster_count = std::move(cluster_count_pipeline);
    auto previous_cluster_prefix = std::move(cluster_prefix_pipeline);
    auto previous_cluster_emit = std::move(cluster_emit_pipeline);
    auto previous_cluster_occlusion = std::move(cluster_occlusion_pipeline);
    auto previous_cluster_retest = std::move(cluster_retest_pipeline);
    auto previous_cluster_scene = std::move(cluster_scene_pipelines);
    auto previous_cluster_shadow = std::move(cluster_shadow_pipelines);
    auto previous_cluster_visibility = std::move(cluster_visibility_pipelines);
    auto previous_cluster_draw_signature = std::move(cluster_draw_signature);
    auto previous_cluster_mesh_signature = std::move(cluster_mesh_signature);
    auto previous_mesh_pipelines = std::move(mesh_pipeline_pairs);
    auto previous_virtual_shadow_indexed = std::move(virtual_shadow_indexed_pipelines);
    auto previous_virtual_shadow_mesh = std::move(virtual_shadow_mesh_pipelines);
    auto previous_dispatch_signature = std::move(dispatch_signature);
    auto previous_ribbon_signature = std::move(ribbon_draw_signature);

    auto result = CreatePipeline(candidate_config);
    HeroTransparencyCandidate transparency;
    if (result)
        result = PrepareHeroTransparency(candidate_config, view_state.render_capacity, transparency);
    if (result)
        result = CreateEnvironmentBrdfLut(candidate_config, environment_brdf_candidate);
    if (!result)
    {
        if (environment_brdf_init_pending)
            frame_submission_faulted = true;
        else
            environment_brdf_candidate.Reset();
        if (!material_parameter_upload_pending) material_parameter_candidate.Reset();
        root_signature = std::move(previous_root);
        scene_pipeline = std::move(previous_scene);
        shadow_pipeline = std::move(previous_shadow);
        environment_opaque_shadow_pipeline = std::move(previous_environment_opaque_shadow);
        environment_material_scene_pipelines = std::move(previous_material_scenes);
        hero_scene_pipeline = std::move(previous_heroes[0]);
        hero_visibility_pipeline = std::move(previous_heroes[1]);
        hero_shadow_pipeline = std::move(previous_heroes[2]);
        hero_resolve_pipeline = std::move(previous_heroes[3]);
        hero_translucent_pipeline = std::move(previous_heroes[4]);
        environment_material_shadow_pipelines = std::move(previous_material_shadows);
        visibility_pipeline = std::move(previous_visibility);
        environment_material_visibility_pipelines = std::move(previous_material_visibility);
        surface_resolve_pipeline = std::move(previous_surface_resolve);
        environment_material_resolve_pipelines = std::move(previous_material_resolve);
        raster_records_pipeline = std::move(previous_raster_records);
        visibility_clear_pipeline = std::move(previous_visibility_clear);
        visibility_count_pipeline = std::move(previous_visibility_count);
        visibility_prefix_pipeline = std::move(previous_visibility_prefix);
        visibility_scatter_pipeline = std::move(previous_visibility_scatter);
        particle_pipeline = std::move(previous_particle);
        ground_ring_pipeline = std::move(previous_ground_ring);
        ground_add_pipeline = std::move(previous_ground_add);
        owner_mesh_pipeline = std::move(previous_owner_mesh);
        vfx_distortion_pipeline = std::move(previous_vfx_distortion);
        vfx_flash_pipeline = std::move(previous_vfx_flash);
        vfx_sprite_oit_pipeline = std::move(previous_vfx_sprite_oit);
        slime_pipeline = std::move(previous_slime);
        fresnel_shell_pipeline = std::move(previous_fresnel_shell);
        particle_compute_pipeline = std::move(previous_particle_compute);
        static_visibility_pipeline = std::move(previous_static_visibility);
        visibility_retest_pipeline = std::move(previous_visibility_retest);
        depth_pyramid_init_pipeline = std::move(previous_depth_pyramid_init);
        depth_pyramid_reduce_pipeline = std::move(previous_depth_pyramid_reduce);
        skin_cache_pipeline = std::move(previous_skin_cache);
        skin_bounds_pipeline = std::move(previous_skin_bounds);
        deferred_pipeline = std::move(previous_deferred);
        composite_pipeline = std::move(previous_composite);
        temporal_pipeline = std::move(previous_temporal);
        pre_exposure_pipeline = std::move(previous_pre_exposure);
        exposure_clear_pipeline = std::move(previous_exposure_clear);
        exposure_build_pipeline = std::move(previous_exposure_build);
        exposure_reduce_pipeline = std::move(previous_exposure_reduce);
        bloom_extract_pipeline = std::move(previous_bloom_extract);
        bloom_downsample_pipeline = std::move(previous_bloom_downsample);
        bloom_upsample_pipeline = std::move(previous_bloom_upsample);
        bloom_pipeline = std::move(previous_bloom);
        tone_map_pipeline = std::move(previous_tone_map);
        outline_pipeline = std::move(previous_outline);
        fxaa_pipeline = std::move(previous_fxaa);
        ui_pipeline = std::move(previous_ui);
        ribbon_update_pipeline = std::move(previous_ribbon_update);
        ribbon_args_pipeline = std::move(previous_ribbon_args);
        ribbon_add_pipeline = std::move(previous_ribbon_add);
        ribbon_oit_pipeline = std::move(previous_ribbon_oit);
        draw_signature = std::move(previous_signature);
        indexed_geometry_signature = std::move(previous_indexed_signature);
        mesh_geometry_signature = std::move(previous_mesh_signature);
        cluster_root_signature = std::move(previous_cluster_root);
        cluster_count_pipeline = std::move(previous_cluster_count);
        cluster_prefix_pipeline = std::move(previous_cluster_prefix);
        cluster_emit_pipeline = std::move(previous_cluster_emit);
        cluster_occlusion_pipeline = std::move(previous_cluster_occlusion);
        cluster_retest_pipeline = std::move(previous_cluster_retest);
        cluster_scene_pipelines = std::move(previous_cluster_scene);
        cluster_shadow_pipelines = std::move(previous_cluster_shadow);
        cluster_visibility_pipelines = std::move(previous_cluster_visibility);
        cluster_draw_signature = std::move(previous_cluster_draw_signature);
        cluster_mesh_signature = std::move(previous_cluster_mesh_signature);
        mesh_pipeline_pairs = std::move(previous_mesh_pipelines);
        virtual_shadow_indexed_pipelines = std::move(previous_virtual_shadow_indexed);
        virtual_shadow_mesh_pipelines = std::move(previous_virtual_shadow_mesh);
        dispatch_signature = std::move(previous_dispatch_signature);
        ribbon_draw_signature = std::move(previous_ribbon_signature);
    }
    else
    {
        const auto completed = graph_queue_executor.Completed();
        if (auto committed = adaptive_shading.Commit(std::move(adaptive), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = this->screen_overlay.Commit(std::move(screen_overlay), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = decal_surface.Commit(std::move(decals), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = ambient_occlusion.Commit(std::move(ambient), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = subsurface.Commit(std::move(diffusion), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = screen_reflections.Commit(std::move(reflections), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = hero_transparency.Commit(std::move(transparency), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = this->hdr_post.Commit(std::move(hdr_post), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = this->light_list.Commit(std::move(light_list), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = this->scene_geometry.Commit(std::move(scene_geometry), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = brixelizer_gi.Commit(std::move(brixelizer), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        brixelizer_history_valid = false;
        if (auto committed = this->virtual_shadows.Commit(std::move(virtual_shadows), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = CommitAdditionalShadows(std::move(additional_shadows)); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = CommitVolumetrics(std::move(volumetrics)); !committed)
        { frame_submission_faulted = true; return committed; }
        if (auto committed = this->local_probes.Commit(std::move(local_probes), completed); !committed)
        { frame_submission_faulted = true; return committed; }
        if (replace_probe)
            if (auto committed = lighting_probe.Commit(std::move(probe), completed); !committed)
            { frame_submission_faulted = true; return committed; }
        if (replace_ltc)
            if (auto committed = ltc_lut.Commit(std::move(ltc), completed); !committed)
            { frame_submission_faulted = true; return committed; }
        if (reconstruction_provider)
            if (auto committed = reconstruction_guides.Commit(std::move(guides), completed); !committed)
            { frame_submission_faulted = true; return committed; }
        if (sdk_presentation.IsEnabled())
            if (auto committed = presentation_guides.Commit(std::move(presentation), completed); !committed)
            { frame_submission_faulted = true; return committed; }
        environment_brdf_lut = std::move(environment_brdf_candidate);
        WriteEnvironmentBrdfLutDescriptors();
        if (material_parameter_candidate.resource) CommitMaterialParameters();
        ++pipeline_revision;
        config = std::move(candidate_config);
        temporal_history_valid = false;
        hzb_history_valid = false;
    }
    return result;
}


} // namespace hs
