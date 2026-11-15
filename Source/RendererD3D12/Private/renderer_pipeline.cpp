#include "renderer_impl.hpp"
#include "renderer_development_catalog.hpp"
#include <DirectXPackedVector.h>
#include <bit>

namespace hs
{
Result D3D12Renderer::Impl::PreparePresentationGuides(const RendererConfig &candidate, ViewExtent output,
    ViewExtent capacity, const DisplayState &display, SdkPresentationOwner presentation_owner,
    PresentationGuidesCandidate &guides) const
{
    if (presentation_owner == SdkPresentationOwner::None) return Result::Success();
    return presentation_guides.Prepare(device.Get(), allocator, capacity, output, display.format, display.color_space,
        presentation_owner, kFrameCount,
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
    selected.selected_texture_feedback = false;
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
                if (!FindCanonicalRendererBulk(selected, baseline) || !FindCanonicalRendererBulk(selected, feedback))
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                        "Selected feedback shader or its baseline has no compiled bulk: " + feedback);
                continue;
            }
            RendererAssetSource path;
            if (auto resolved = ResolveRendererAsset(selected, feedback, path); !resolved) return resolved;
            if (path.IsPacked())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Loose feedback shader resolved to a packed source: " + path.Describe(feedback));
            const auto digest = request.product_digests.find(feedback);
            if (digest == request.product_digests.end() || !selected.product_paths.contains(baseline) ||
                !selected.product_digests.contains(baseline))
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Selected feedback shader or its baseline has no immutable product identity: " + feedback);
        }
    }
    selected.selected_texture_feedback = true;
    return Result::Success();
}
Result D3D12Renderer::Impl::CreateRootSignature()
{
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
    auto terrain_page_range = material_texture_range;
    terrain_page_range.NumDescriptors = HS_TERRAIN_SRV_DESCRIPTOR_COUNT;
    terrain_page_range.RegisterSpace = HS_TERRAIN_PAGE_SPACE;
    terrain_page_range.BaseShaderRegister = HS_TERRAIN_SECTION_SRV;
    terrain_page_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    terrain_page_range.OffsetInDescriptorsFromTableStart = kTerrainPageDescriptorOffset - kPostTextureDescriptorCount;
    D3D12_DESCRIPTOR_RANGE1 terrain_request_range{};
    terrain_request_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    terrain_request_range.NumDescriptors = 1;
    terrain_request_range.BaseShaderRegister = HS_TERRAIN_REQUEST_UAV;
    terrain_request_range.RegisterSpace = HS_TERRAIN_PAGE_SPACE;
    terrain_request_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                                  D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    terrain_request_range.OffsetInDescriptorsFromTableStart = terrain_page_range.OffsetInDescriptorsFromTableStart + HS_TERRAIN_REQUEST_DESCRIPTOR;
    const std::array ranges{character_range, monster_pbr_range, environment_range,
                            gradient_range, ribbon_detail_range, impact_range, smoke_range,
                            material_texture_range, terrain_page_range, terrain_request_range};

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
    D3D12_DESCRIPTOR_RANGE1 ambient_range = view_range;
    ambient_range.NumDescriptors = 2;
    ambient_range.BaseShaderRegister = 5;
    ambient_range.OffsetInDescriptorsFromTableStart += 11;
    D3D12_DESCRIPTOR_RANGE1 character_binding_range = material_parameter_range;
    character_binding_range.BaseShaderRegister = 1;
    character_binding_range.OffsetInDescriptorsFromTableStart = kCharacterBindingDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 probe_range = material_parameter_range;
    probe_range.NumDescriptors = 3;
    probe_range.BaseShaderRegister = 0;
    probe_range.RegisterSpace = 15;
    probe_range.OffsetInDescriptorsFromTableStart = kProbeDescriptorOffset - kTemporalTextureDescriptorOffset;
    D3D12_DESCRIPTOR_RANGE1 light_range = probe_range;
    light_range.NumDescriptors = kLightListResourceCount;
    light_range.RegisterSpace = 19;
    light_range.OffsetInDescriptorsFromTableStart = kLightListDescriptorOffset - kTemporalTextureDescriptorOffset;
    const std::array history_geometry_ranges{
        temporal_input_range, geometry_range, surface_range,
        visibility_srv_range, visibility_uav_range, material_parameter_range,
        view_range, exposure_srv_range, ambient_range, character_binding_range, probe_range, light_range,
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kVirtualShadowDescriptorCount, 0, 21,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kVirtualShadowDescriptorOffset - kTemporalTextureDescriptorOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kLocalProbeSrvCount, 0, 23,
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
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kLocalShadowDescriptorCount, 0, HS_LOCAL_SHADOW_SPACE,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kLocalShadowDescriptorOffset - kTemporalTextureDescriptorOffset},
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
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, HS_DECAL_CONSTANT_REGISTER, HS_DECAL_SPACE,
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE,
            kDecalSurfaceDescriptorOffset - kTemporalTextureDescriptorOffset + kDecalSurfaceCbvOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kDecalSurfaceSrvCount, DecalSurfaceSrvDepth, HS_DECAL_SPACE,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kDecalSurfaceDescriptorOffset - kTemporalTextureDescriptorOffset + kDecalSurfaceSrvOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kDecalSurfaceUavCount, DecalSurfaceUavBase, HS_DECAL_SPACE,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
            kDecalSurfaceDescriptorOffset - kTemporalTextureDescriptorOffset + kDecalSurfaceUavOffset},
        D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kDecalSurfaceExportCount, HS_DECAL_EXPORT_REGISTER_BASE, HS_DECAL_SPACE,
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
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kLayeredTranslucencySrvCount, 0, 14,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 1},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 14,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, kLayeredTranslucencyUavOffset}};
    const D3D12_DESCRIPTOR_RANGE1 view_skin_ranges[]{
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 3,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 30, 3,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 1}};
    // Table 19 shares the previous-scene and view-skin bindings within the 64-DWORD limit.
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
    parameters[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[19].DescriptorTable = {static_cast<UINT>(std::size(view_skin_ranges)), view_skin_ranges};
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
    return Result::Success();
}

Result D3D12Renderer::Impl::CreatePipeline(const RendererConfig &request)
{
    pipeline_cache.ClearShaderOwners();
    struct ShaderOwnerScope
    {
        RendererPipelineCache &cache;
        ~ShaderOwnerScope() { cache.ClearShaderOwners(); }
    } shader_owner_scope{pipeline_cache};
    RendererConfig pipeline_config;
    if (auto selected = SelectTextureFeedbackShaders(request, pipeline_config); !selected) return selected;
    RendererAssetSource abi_source;
    if (auto resolved = ResolveRendererAsset(pipeline_config, render_data::SurfaceLayout::abi_product, abi_source);
        !resolved) return resolved;
    std::vector<std::byte> abi;
    if (auto read = ReadBinary(abi_source, abi); !read) return read;
    std::uint32_t abi_version{};
    if (abi.size() == sizeof(abi_version))
        for (std::size_t byte = 0; byte < sizeof(abi_version); ++byte)
            abi_version |= std::to_integer<std::uint32_t>(abi[byte]) << (byte * 8u);
    if (abi.size() != sizeof(abi_version) || abi_version != render_data::SurfaceLayout::version)
        return Result::Failure(ErrorCode::InvalidState, "RendererSurfaceLayout",
            "Selected shader generation has an incompatible surface layout ABI; recook renderer content.");
    mesh_pipeline_pairs.clear();
    virtual_shadow_indexed_pipelines.clear();
    virtual_shadow_mesh_pipelines.clear();
    if (auto root = CreateRootSignature(); !root) return root;
    HRESULT result{S_OK};

    const auto register_shader_owner = [&](std::string_view logical_id,
        const std::vector<std::byte> &bytes,bool bytes_already_verified) -> Result {
        const auto expected = RendererProductDigest(pipeline_config, logical_id);
        return pipeline_cache.RegisterShaderOwner(bytes, expected, bytes_already_verified);
    };
    const auto read_shader = [&](std::string_view name, std::vector<std::byte> &bytes) {
        RendererAssetSource path;
        const auto logical_id = "render/shader/" + std::string(name);
        if (auto resolved = ResolveRendererAsset(pipeline_config, logical_id, path);
            !resolved) return resolved;
        const bool bytes_already_verified = path.IsPacked() || path.HasAdmittedWholeFileProof();
        if (auto read = ReadBinary(path, bytes); !read) return read;
        return register_shader_owner(logical_id, bytes, bytes_already_verified);
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
        if (auto read = ReadRendererMaterialProgram(pipeline_config, *program, bytes); !read) return read;
        return register_shader_owner(program->product_id, bytes, true);
    };
    std::vector<std::byte> scene_vertex, visibility_vertex;
    std::vector<std::byte> cluster_vertex, cluster_shadow_vertex;
    std::vector<std::byte> scene_mesh;
    std::vector<std::byte> shadow_mesh;
    std::vector<std::byte> scene_pixel;
    std::vector<std::byte> hero_scene_pixel, hero_visibility_pixel, hero_shadow_pixel;
    std::vector<std::byte> character_resolve_compute, hero_translucent_pixel;
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
    std::vector<std::byte> deferred_pixel;
    std::vector<std::byte> composite_pixel;
    std::vector<std::byte> bloom_extract_pixel;
    std::vector<std::byte> bloom_downsample_pixel;
    std::vector<std::byte> bloom_upsample_pixel;
    std::vector<std::byte> bloom_pixel;
    std::vector<std::byte> tone_map_pixel;
    std::vector<std::byte> fxaa_pixel;
    std::vector<std::byte> ui_pixel, ui_draw_vs, ui_draw_ps;
    std::vector<std::byte> ribbon_update;
    std::vector<std::byte> ribbon_args;
    std::vector<std::byte> ribbon_vs;
    std::vector<std::byte> ribbon_add_ps;
    std::vector<std::byte> ribbon_oit_ps;
    for (auto [name, bytes] : {
             std::pair{"scene_vs.dxil", &scene_vertex},
             std::pair{config.surface_frontend == SurfaceFrontend::VisibilityBuffer
                 ? "cluster_scene_current_vs.dxil" : "cluster_scene_vs.dxil", &cluster_vertex},
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
             std::pair{"static_visibility_sparse_v2_cs.dxil", &static_visibility_compute},
             std::pair{"visibility_retest_sparse_v2_cs.dxil", &visibility_retest_compute},
             std::pair{"depth_pyramid_init_cs.dxil", &depth_pyramid_init_compute},
             std::pair{"depth_pyramid_reduce_cs.dxil", &depth_pyramid_reduce_compute},
             std::pair{"skin_cache_cs.dxil", &skin_cache_compute},
             std::pair{"skin_bounds_cs.dxil", &skin_bounds_compute},
             std::pair{"fullscreen_vs.dxil", &full_screen_vertex},
             std::pair{"temporal_ps.dxil", &temporal_pixel},
             std::pair{"composite_ps.dxil", &composite_pixel},
             std::pair{"bloom_extract_ps.dxil", &bloom_extract_pixel},
             std::pair{"bloom_downsample_ps.dxil", &bloom_downsample_pixel},
             std::pair{"bloom_upsample_ps.dxil", &bloom_upsample_pixel},
             std::pair{"bloom_ps.dxil", &bloom_pixel},
             std::pair{"tonemap_ps.dxil", &tone_map_pixel},
             std::pair{"fxaa_ps.dxil", &fxaa_pixel},
             std::pair{"ui_ps.dxil", &ui_pixel},
             std::pair{"ui_draw_vs.dxil", &ui_draw_vs},
             std::pair{"ui_draw_ps.dxil", &ui_draw_ps},
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
    const bool deferred_bytes_verified = deferred_path.IsPacked() || deferred_path.HasAdmittedWholeFileProof();
    if (auto read = ReadBinary(deferred_path, deferred_pixel); !read) return read;
    if (auto registered = register_shader_owner("render/shader/deferred_ps.dxil", deferred_pixel,
        deferred_bytes_verified); !registered)
        return registered;
    if (auto read = read_shader("character_shadow_ps.dxil", hero_shadow_pixel); !read) return read;
    if (auto read = read_shader("character_translucent_ps.dxil", hero_translucent_pixel); !read) return read;
    if (config.surface_frontend == SurfaceFrontend::CompactDeferred)
    {
        if (auto read = read_shader("character_scene_ps.dxil", hero_scene_pixel); !read) return read;
    }
    else
    {
        if (auto read = read_shader("character_visibility_ps.dxil", hero_visibility_pixel); !read) return read;
        if (auto read = read_shader("character_resolve_cs.dxil", character_resolve_compute); !read) return read;
    }
    if (config.surface_frontend == SurfaceFrontend::CompactDeferred)
        if (auto read = read_shader("scene_ps.dxil", scene_pixel); !read) return read;
    for (std::size_t i = 0; i < kAuthoredMaterialNames.size(); ++i)
    {
        const std::string base = "render/shader/" +
            std::string(kAuthoredMaterialNames[i]);
        const auto material_id = render_data::MaterialProduct(kAuthoredMaterialNames[i]);
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
                const bool shader_bytes_verified = shader_path.IsPacked() || shader_path.HasAdmittedWholeFileProof();
                if (auto read = ReadBinary(shader_path, *bytes); !read)
                    return read;
                if (auto registered = register_shader_owner(logical_id, *bytes,
                    shader_bytes_verified); !registered)
                    return registered;
            }
        }
    }

    if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
    {
        for (auto [name, bytes] : {
                 std::pair{"scene_current_vs.dxil", &visibility_vertex},
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
        if (auto read = read_shader(config.surface_frontend == SurfaceFrontend::VisibilityBuffer
                ? "scene_current_ms.dxil" : "scene_ms.dxil", scene_mesh); !read)
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
        {"OPTICAL_COMPONENT", 0, DXGI_FORMAT_R16_UINT, 0, 74,
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
    scene.RTVFormats[2] = DXGI_FORMAT_R32_UINT;
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
    // CharacterShadowPS retains two-sided translucent shadow coverage.
    hero_shadow.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    hero_shadow.PS = {hero_shadow_pixel.data(), hero_shadow_pixel.size()};
    result = create_shadow_graphics(&hero_shadow, IID_PPV_ARGS(&hero_shadow_pipeline));
    if (FAILED(result)) return HResultFailure("Create hero shadow PSO", result);
    if (auto created = create_mesh_pair(hero_shadow, hero_shadow_pipeline.Get(), shadow_mesh); !created) return created;
    if (config.surface_frontend == SurfaceFrontend::CompactDeferred)
    {
        auto hero_scene = scene;
        // CharacterScenePS discards backfaces; cull them before pixel shading.
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
        visibility.VS = {visibility_vertex.data(), visibility_vertex.size()};
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
        if (auto created = create_visibility_compute(character_resolve_compute,
                character_resolve_pipeline, "character surface resolve"); !created) return created;
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
        if (FAILED(create_result)) (void)CountValidationErrors();
        return SUCCEEDED(create_result) ? Result::Success()
                                        : HResultFailure("Create post-process PSO", create_result);
    };
    post.NumRenderTargets = 3;
    for (const UINT mask : {0u, 1u, 2u, 3u})
    {
        for (UINT slot = 0; slot < 2; ++slot)
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
    post.RTVFormats[0] = owned_views.at(kMainRenderViewId.value)->display_state.format;
    if (auto pipeline_result =
            create_post_pipeline(ui_pixel, ui_pipeline.ReleaseAndGetAddressOf());
        !pipeline_result)
    {
        return pipeline_result;
    }

    D3D12_ROOT_SIGNATURE_DESC ui_root{static_cast<UINT>(std::size(kUiDrawParameters)), kUiDrawParameters};
    ComPtr<ID3DBlob> ui_signature, ui_diagnostics;
    result = D3D12SerializeRootSignature(&ui_root, D3D_ROOT_SIGNATURE_VERSION_1, &ui_signature, &ui_diagnostics);
    if (FAILED(result)) return HResultFailure("Serialize UI draw root", result);
    result = device->CreateRootSignature(0, ui_signature->GetBufferPointer(), ui_signature->GetBufferSize(),
        IID_PPV_ARGS(ui_draw_root.ReleaseAndGetAddressOf()));
    if (FAILED(result)) return HResultFailure("Create UI draw root", result);
    if (auto registered = pipeline_cache.RegisterRootSignature(ui_draw_root.Get(),
            {static_cast<const std::byte *>(ui_signature->GetBufferPointer()), ui_signature->GetBufferSize()});
        !registered) return registered;
    post.pRootSignature = ui_draw_root.Get();
    post.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    post.VS = {ui_draw_vs.data(), ui_draw_vs.size()};
    auto &ui_blend = post.BlendState.RenderTarget[0];
    ui_blend.BlendEnable = TRUE;
    ui_blend.SrcBlend = ui_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    ui_blend.DestBlend = ui_blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    ui_blend.BlendOp = ui_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (auto created = create_post_pipeline(ui_draw_ps, ui_draw_pipeline.ReleaseAndGetAddressOf()); !created)
        return created;

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

void D3D12Renderer::Impl::FillMaterialFrameConstants(renderer_detail::FrameConstants &constants) const
{
    static_assert(kAuthoredMaterialNames.size() <= 16);
    for (std::size_t group = 0; group < constants.material_parameter_bases.size(); ++group)
    {
        std::array<UINT, 4> bases{};
        for (std::size_t lane = 0; lane < bases.size(); ++lane)
            if (const auto ordinal = group * 4 + lane; ordinal < kAuthoredMaterialNames.size())
                bases[lane] = material_parameter_bases[ordinal];
        constants.material_parameter_bases[group] = {bases[0], bases[1], bases[2], bases[3]};
        std::array<UINT, 4> masks{};
        for (std::size_t lane = 0; lane < masks.size(); ++lane)
            if (const auto ordinal = group * 4 + lane; ordinal < kAuthoredMaterialNames.size())
                masks[lane] = material_surface_layouts[ordinal].varying_mask;
        constants.material_varying_masks[group] = {masks[0], masks[1], masks[2], masks[3]};
    }
    const auto &sidecar = material_sidecar_layout;
    for (std::size_t group = 0; group < constants.character_material_parameter_bases.size(); ++group)
    {
        std::array<UINT, 4> bases{}, masks{};
        for (std::size_t lane = 0; lane < bases.size(); ++lane)
            if (const auto hero = group * 4 + lane; hero < kCharacterMaterialNames.size())
            {
                const auto ordinal = kAuthoredMaterialNames.size() + hero;
                bases[lane] = material_parameter_bases[ordinal];
                masks[lane] = material_surface_layouts[ordinal].varying_mask;
            }
        constants.character_material_parameter_bases[group] = {bases[0], bases[1], bases[2], bases[3]};
        constants.character_material_varying_masks[group] = {masks[0], masks[1], masks[2], masks[3]};
    }
    constants.probe_options = lighting_probe.FrameOptions();
    constants.material_sidecar_offsets[0] = {sidecar.offsets[0], sidecar.offsets[1],
                                             sidecar.offsets[2], sidecar.offsets[3]};
    constants.material_sidecar_offsets[1] = {sidecar.offsets[4], UINT32_MAX, UINT32_MAX, UINT32_MAX};
    constants.material_sidecar_info = {sidecar.mask, sidecar.word_count, sidecar.plane_count, 0};
    const bool pom_enabled = config.pom_quality != GraphicsOptionalQuality::Off;
    const auto pom_row = pom_enabled ? static_cast<GraphicsQuality>(static_cast<unsigned>(config.pom_quality) - 1u) : GraphicsQuality::Low;
    const auto &pom = QualityProfileFor(pom_row);
    const auto &material = QualityProfileFor(config.material_quality);
    // Z is bit-reinterpreted payload: positive binary16 fade start in the low
    // half, fade end in the high half. HLSL decodes both with f16tof32.
    const auto fade_bits = UINT(DirectX::PackedVector::XMConvertFloatToHalf(material.terrain_detail_fade_start_m)) |
        (UINT(DirectX::PackedVector::XMConvertFloatToHalf(material.terrain_detail_fade_end_m)) << 16u);
    constants.material_quality_options = {pom_enabled ? float(pom.pom_max_steps) : 0.0f, pom_enabled ? float(pom.pom_refinement_steps) : 0.0f,
        std::bit_cast<float>(fade_bits), static_cast<float>(config.refraction_quality)};
}

} // namespace hs
