#include "renderer_impl.hpp"

namespace hs
{

Result D3D12Renderer::Impl::CreatePipeline()
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
    gradient_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
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
    const std::array ranges{character_range, monster_pbr_range, environment_range,
                            gradient_range, ribbon_detail_range, impact_range, smoke_range};

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
    decal_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    D3D12_DESCRIPTOR_RANGE1 bloom_range = distortion_range;
    bloom_range.BaseShaderRegister = 22;
    bloom_range.NumDescriptors = 3;
    D3D12_DESCRIPTOR_RANGE1 temporal_input_range = bloom_range;
    temporal_input_range.BaseShaderRegister = 26;
    temporal_input_range.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE1 temporal_output_range = temporal_input_range;
    temporal_output_range.BaseShaderRegister = 27;
    std::array<D3D12_ROOT_PARAMETER1, 34> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
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
    parameters[6].Constants = {1, 0, 1};
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
    parameters[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[13].DescriptorTable = {static_cast<UINT>(ranges.size()), ranges.data()};
    parameters[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[14].Descriptor = {17, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[15].Descriptor = {18, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[16].Descriptor = {0, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
    parameters[16].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[17].Descriptor = {2, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
    parameters[17].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[18].Descriptor = {3, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC};
    parameters[18].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[19].Descriptor = {4, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[19].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[20].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[20].Descriptor = {5, 3, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[20].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
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
    parameters[26].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
    parameters[32].DescriptorTable = {1, &temporal_input_range};
    parameters[32].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[33].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[33].DescriptorTable = {1, &temporal_output_range};
    parameters[33].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, 5> samplers{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
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
    samplers[2].ShaderRegister = 2;
    samplers[3] = samplers[2];
    samplers[3].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[3].Filter = D3D12_FILTER_ANISOTROPIC;
    samplers[3].MaxAnisotropy = 16;
    samplers[3].ShaderRegister = 3;
    samplers[4] = samplers[3];
    samplers[4].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[4].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[4].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[4].MaxLOD = 5.0f;
    samplers[4].ShaderRegister = 4;

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

    const auto shader_directory = CurrentExecutableDirectory() / "Shaders";
    std::vector<std::byte> scene_vertex;
    std::vector<std::byte> scene_pixel;
    std::vector<std::byte> shadow_vertex;
    std::vector<std::byte> shadow_pixel;
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
    std::vector<std::byte> full_screen_vertex;
    std::vector<std::byte> temporal_pixel;
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
             std::pair{"scene_ps.dxil", &scene_pixel},
             std::pair{"shadow_vs.dxil", &shadow_vertex},
             std::pair{"shadow_ps.dxil", &shadow_pixel},
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
             std::pair{"fullscreen_vs.dxil", &full_screen_vertex},
             std::pair{"temporal_ps.dxil", &temporal_pixel},
             std::pair{"deferred_ps.dxil", &deferred_pixel},
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
        if (auto read = ReadBinary(shader_directory / name, *bytes); !read)
        {
            return read;
        }
    }

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
    scene.PS = {scene_pixel.data(), scene_pixel.size()};
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
    scene.NumRenderTargets = 4;
    scene.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    scene.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    // Preserve world position precision for deferred shadow comparisons.
    scene.RTVFormats[2] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    scene.RTVFormats[3] = DXGI_FORMAT_R8G8B8A8_UNORM;
    scene.DSVFormat = kDepthFormat;
    scene.SampleDesc = {1, 0};
    result = device->CreateGraphicsPipelineState(&scene, IID_PPV_ARGS(&scene_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create scene PSO", result);
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
    result = device->CreateGraphicsPipelineState(&shadow_state, IID_PPV_ARGS(&shadow_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create shadow PSO", result);
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
    particle.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    particle.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
    particle.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    particle.RTVFormats[3] = DXGI_FORMAT_UNKNOWN;
    result = device->CreateGraphicsPipelineState(&particle, IID_PPV_ARGS(&particle_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle PSO", result);
    }

    auto ground_ring = particle;
    ground_ring.VS = {ground_ring_vertex.data(), ground_ring_vertex.size()};
    result = device->CreateGraphicsPipelineState(&ground_ring,
                                                 IID_PPV_ARGS(&ground_ring_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create ground ring PSO", result);
    }

    auto owner_mesh = particle;
    owner_mesh.VS = {owner_mesh_vertex.data(), owner_mesh_vertex.size()};
    owner_mesh.PS = {owner_mesh_pixel.data(), owner_mesh_pixel.size()};
    result = device->CreateGraphicsPipelineState(&owner_mesh,
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
    result = device->CreateGraphicsPipelineState(&vfx_sprite_oit, IID_PPV_ARGS(&vfx_sprite_oit_pipeline));
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
    result = device->CreateGraphicsPipelineState(&vfx_flash,
                                                 IID_PPV_ARGS(&vfx_flash_pipeline));
    if (FAILED(result)) return HResultFailure("Create VFX flash PSO", result);

    auto ground_add = vfx_flash;
    ground_add.VS = {ground_ring_vertex.data(), ground_ring_vertex.size()};
    ground_add.PS = {ground_add_pixel.data(), ground_add_pixel.size()};
    ground_add.DepthStencilState.DepthEnable = TRUE;
    ground_add.DSVFormat = kDepthFormat;
    result = device->CreateGraphicsPipelineState(&ground_add,
                                                 IID_PPV_ARGS(&ground_add_pipeline));
    if (FAILED(result)) return HResultFailure("Create additive ground PSO", result);

    auto distortion = vfx_flash;
    distortion.VS = {vfx_distortion_vertex.data(), vfx_distortion_vertex.size()};
    distortion.PS = {vfx_distortion_pixel.data(), vfx_distortion_pixel.size()};
    result = device->CreateGraphicsPipelineState(&distortion, IID_PPV_ARGS(&vfx_distortion_pipeline));
    if (FAILED(result)) return HResultFailure("Create VFX distortion PSO", result);

    auto slime = particle;
    slime.VS = scene.VS;
    slime.PS = {slime_pixel.data(), slime_pixel.size()};
    slime.InputLayout = scene.InputLayout;
    slime.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    result = device->CreateGraphicsPipelineState(&slime, IID_PPV_ARGS(&slime_pipeline));
    if (FAILED(result)) return HResultFailure("Create slime PSO", result);

    auto fresnel_shell = slime;
    fresnel_shell.VS = {fresnel_shell_vertex.data(), fresnel_shell_vertex.size()};
    fresnel_shell.PS = {fresnel_shell_pixel.data(), fresnel_shell_pixel.size()};
    result = device->CreateGraphicsPipelineState(&fresnel_shell,
                                                 IID_PPV_ARGS(&fresnel_shell_pipeline));
    if (FAILED(result)) return HResultFailure("Create Fresnel shell PSO", result);

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = root_signature.Get();
    compute.CS = {particle_compute.data(), particle_compute.size()};
    result =
        device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&particle_compute_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle compute PSO", result);
    }
    compute.CS = {ribbon_update.data(), ribbon_update.size()};
    result = device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&ribbon_update_pipeline));
    if (FAILED(result)) return HResultFailure("Create ribbon update PSO", result);
    compute.CS = {ribbon_args.data(), ribbon_args.size()};
    result = device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&ribbon_args_pipeline));
    if (FAILED(result)) return HResultFailure("Create ribbon args PSO", result);

    auto ribbon_add = particle;
    ribbon_add.VS = {ribbon_vs.data(), ribbon_vs.size()};
    ribbon_add.PS = {ribbon_add_ps.data(), ribbon_add_ps.size()};
    ribbon_add.InputLayout = {};
    ribbon_add.BlendState.IndependentBlendEnable = FALSE;
    ribbon_add.NumRenderTargets = 1;
    ribbon_add.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ribbon_add.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    ribbon_add.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    ribbon_add.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    ribbon_add.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    ribbon_add.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    result = device->CreateGraphicsPipelineState(&ribbon_add,
                                                  IID_PPV_ARGS(&ribbon_add_pipeline));
    if (FAILED(result)) return HResultFailure("Create ribbon add PSO", result);
    auto ribbon_oit = ribbon_add;
    ribbon_oit.PS = {ribbon_oit_ps.data(), ribbon_oit_ps.size()};
    ribbon_oit.BlendState.IndependentBlendEnable = TRUE;
    ribbon_oit.NumRenderTargets = 2;
    ribbon_oit.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
    ribbon_oit.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    ribbon_oit.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    ribbon_oit.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
    ribbon_oit.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_COLOR;
    result = device->CreateGraphicsPipelineState(&ribbon_oit,
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
        const auto create_result = device->CreateGraphicsPipelineState(
            &post, IID_PPV_ARGS(pipeline));
        return SUCCEEDED(create_result) ? Result::Success()
                                        : HResultFailure("Create post-process PSO", create_result);
    };
    for (auto [shader, pipeline] :
         std::array{
             std::pair{std::span<const std::byte>(deferred_pixel),
                       deferred_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(composite_pixel),
                       composite_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(temporal_pixel),
                       temporal_pipeline.ReleaseAndGetAddressOf()},
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

    post.RTVFormats[0] = kBackBufferFormat;
    if (auto pipeline_result =
            create_post_pipeline(fxaa_pixel, fxaa_pipeline.ReleaseAndGetAddressOf());
        !pipeline_result)
    {
        return pipeline_result;
    }
    post.BlendState.RenderTarget[0].BlendEnable = TRUE;
    post.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    post.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    post.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    post.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    post.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    post.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
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
    return SUCCEEDED(result) ? Result::Success()
                             : HResultFailure("CreateCommandSignature", result);
}

Result D3D12Renderer::Impl::ReloadPipeline()
{
    if (auto waited = WaitForGpu(); !waited)
        return waited;
    auto previous_root = std::move(root_signature);
    auto previous_scene = std::move(scene_pipeline);
    auto previous_shadow = std::move(shadow_pipeline);
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
    auto previous_deferred = std::move(deferred_pipeline);
    auto previous_composite = std::move(composite_pipeline);
    auto previous_temporal = std::move(temporal_pipeline);
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
    auto previous_ribbon_signature = std::move(ribbon_draw_signature);

    auto result = CreatePipeline();
    if (!result)
    {
        root_signature = std::move(previous_root);
        scene_pipeline = std::move(previous_scene);
        shadow_pipeline = std::move(previous_shadow);
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
        deferred_pipeline = std::move(previous_deferred);
        composite_pipeline = std::move(previous_composite);
        temporal_pipeline = std::move(previous_temporal);
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
        ribbon_draw_signature = std::move(previous_ribbon_signature);
    }
    else
    {
        temporal_history_valid = false;
    }
    return result;
}


} // namespace hs
