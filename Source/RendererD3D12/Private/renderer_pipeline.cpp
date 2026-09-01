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
    const std::array ranges{character_range, monster_pbr_range};

    std::array<D3D12_ROOT_PARAMETER1, 16> parameters{};
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

    std::array<D3D12_STATIC_SAMPLER_DESC, 3> samplers{};
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
    std::vector<std::byte> particle_vertex;
    std::vector<std::byte> particle_pixel;
    std::vector<std::byte> particle_compute;
    std::vector<std::byte> full_screen_vertex;
    std::vector<std::byte> deferred_pixel;
    std::vector<std::byte> composite_pixel;
    std::vector<std::byte> bloom_pixel;
    std::vector<std::byte> tone_map_pixel;
    std::vector<std::byte> outline_pixel;
    std::vector<std::byte> fxaa_pixel;
    std::vector<std::byte> ui_pixel;
    for (auto [name, bytes] : {
             std::pair{"scene_vs.dxil", &scene_vertex},
             std::pair{"scene_ps.dxil", &scene_pixel},
             std::pair{"shadow_vs.dxil", &shadow_vertex},
             std::pair{"particle_vs.dxil", &particle_vertex},
             std::pair{"particle_ps.dxil", &particle_pixel},
             std::pair{"particle_cs.dxil", &particle_compute},
             std::pair{"fullscreen_vs.dxil", &full_screen_vertex},
             std::pair{"deferred_ps.dxil", &deferred_pixel},
             std::pair{"composite_ps.dxil", &composite_pixel},
             std::pair{"bloom_ps.dxil", &bloom_pixel},
             std::pair{"tonemap_ps.dxil", &tone_map_pixel},
             std::pair{"outline_ps.dxil", &outline_pixel},
             std::pair{"fxaa_ps.dxil", &fxaa_pixel},
             std::pair{"ui_ps.dxil", &ui_pixel},
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
    scene.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    scene.RasterizerState.FrontCounterClockwise = FALSE;
    scene.RasterizerState.DepthClipEnable = TRUE;
    scene.DepthStencilState.DepthEnable = TRUE;
    scene.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    scene.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    scene.InputLayout = {input_layout, static_cast<UINT>(std::size(input_layout))};
    scene.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    scene.NumRenderTargets = 3;
    scene.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    scene.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    scene.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    scene.DSVFormat = kDepthFormat;
    scene.SampleDesc = {1, 0};
    result = device->CreateGraphicsPipelineState(&scene, IID_PPV_ARGS(&scene_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create scene PSO", result);
    }

    auto shadow_state = scene;
    shadow_state.VS = {shadow_vertex.data(), shadow_vertex.size()};
    shadow_state.PS = {};
    shadow_state.NumRenderTargets = 0;
    std::fill(std::begin(shadow_state.RTVFormats), std::end(shadow_state.RTVFormats),
              DXGI_FORMAT_UNKNOWN);
    shadow_state.RasterizerState.DepthBias = 800;
    shadow_state.RasterizerState.SlopeScaledDepthBias = 1.5f;
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
    result = device->CreateGraphicsPipelineState(&particle, IID_PPV_ARGS(&particle_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle PSO", result);
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = root_signature.Get();
    compute.CS = {particle_compute.data(), particle_compute.size()};
    result =
        device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&particle_compute_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle compute PSO", result);
    }

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
    auto previous_particle_compute = std::move(particle_compute_pipeline);
    auto previous_deferred = std::move(deferred_pipeline);
    auto previous_composite = std::move(composite_pipeline);
    auto previous_bloom = std::move(bloom_pipeline);
    auto previous_tone_map = std::move(tone_map_pipeline);
    auto previous_outline = std::move(outline_pipeline);
    auto previous_fxaa = std::move(fxaa_pipeline);
    auto previous_ui = std::move(ui_pipeline);
    auto previous_signature = std::move(draw_signature);

    auto result = CreatePipeline();
    if (!result)
    {
        root_signature = std::move(previous_root);
        scene_pipeline = std::move(previous_scene);
        shadow_pipeline = std::move(previous_shadow);
        particle_pipeline = std::move(previous_particle);
        particle_compute_pipeline = std::move(previous_particle_compute);
        deferred_pipeline = std::move(previous_deferred);
        composite_pipeline = std::move(previous_composite);
        bloom_pipeline = std::move(previous_bloom);
        tone_map_pipeline = std::move(previous_tone_map);
        outline_pipeline = std::move(previous_outline);
        fxaa_pipeline = std::move(previous_fxaa);
        ui_pipeline = std::move(previous_ui);
        draw_signature = std::move(previous_signature);
    }
    return result;
}


} // namespace hs
