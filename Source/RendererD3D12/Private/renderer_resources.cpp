#include "renderer_impl.hpp"

namespace hs
{

Result D3D12Renderer::Impl::CreateAllocation(AllocationResource &output,
                                            const D3D12MA::ALLOCATION_DESC &allocation,
                                            const D3D12_RESOURCE_DESC &resource,
                                            D3D12_RESOURCE_STATES initial_state,
                                            const D3D12_CLEAR_VALUE *clear)
{
    const auto effective_state =
        enhanced && resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
            ? D3D12_RESOURCE_STATE_COMMON
            : initial_state;
    const auto result = allocator->CreateResource(&allocation, &resource, effective_state, clear,
                                                  &output.allocation,
                                                  IID_PPV_ARGS(output.resource.ReleaseAndGetAddressOf()));
    return SUCCEEDED(result) ? Result::Success() : HResultFailure("D3D12MA::CreateResource", result);
}

Result D3D12Renderer::Impl::CreateSwapChainAndTargets()
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = kBackBufferFormat;
    description.SampleDesc = {1, 0};
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kFrameCount;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swap_chain1;
    auto result = factory->CreateSwapChainForHwnd(
        queue.Get(), static_cast<HWND>(config.window), &description, nullptr, nullptr, &swap_chain1);
    if (FAILED(result))
    {
        return HResultFailure("CreateSwapChainForHwnd", result);
    }
    result = swap_chain1.As(&swap_chain);
    if (FAILED(result))
    {
        return HResultFailure("Query IDXGISwapChain4", result);
    }
    factory->MakeWindowAssociation(static_cast<HWND>(config.window), DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
    rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_description.NumDescriptors = 12;
    result = device->CreateDescriptorHeap(&rtv_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create RTV heap", result);
    }
    rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        result = swap_chain->GetBuffer(index, IID_PPV_ARGS(&back_buffers[index]));
        if (FAILED(result))
        {
            return HResultFailure("Get swap-chain buffer", result);
        }
        device->CreateRenderTargetView(back_buffers[index].Get(), nullptr, handle);
        handle.ptr += rtv_stride;
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreatePostProcessTargets()
{
    transient_textures_common = enhanced;
    gbuffer_base.Reset();
    gbuffer_normal.Reset();
    gbuffer_position.Reset();
    gbuffer_material.Reset();
    hdr_color.Reset();
    oit_accumulation.Reset();
    oit_revealage.Reset();
    post_a.Reset();
    post_b.Reset();

    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    auto create_target = [&](AllocationResource &output, DXGI_FORMAT format,
                             const float *clear_color) -> Result {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = render_width;
        description.Height = render_height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc = {1, 0};
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        std::copy_n(clear_color, 4, clear.Color);
        return CreateAllocation(output, allocation, description,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear);
    };

    constexpr float black[4] = {0, 0, 0, 0};
    constexpr float normal_clear[4] = {0.5f, 1.0f, 0.5f, 0};
    constexpr float reveal_clear[4] = {1, 1, 1, 1};
    for (auto [resource, format, clear] : {
             std::tuple{&gbuffer_base, DXGI_FORMAT_R8G8B8A8_UNORM, black},
             std::tuple{&gbuffer_normal, DXGI_FORMAT_R16G16B16A16_FLOAT, normal_clear},
             std::tuple{&gbuffer_position, DXGI_FORMAT_R32G32B32A32_FLOAT, black},
             std::tuple{&hdr_color, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&oit_accumulation, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&oit_revealage, DXGI_FORMAT_R16_FLOAT, reveal_clear},
             std::tuple{&post_a, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&post_b, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&gbuffer_material, DXGI_FORMAT_R8G8B8A8_UNORM, black},
         })
    {
        if (auto created = create_target(*resource, format, clear); !created)
        {
            return created;
        }
    }

    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * rtv_stride;
    for (auto [resource, format] : {
             std::pair{gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM},
             std::pair{gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{gbuffer_position.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT},
             std::pair{hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{oit_accumulation.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT},
             std::pair{post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{gbuffer_material.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM},
         })
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource, &view, rtv);
        rtv.ptr += rtv_stride;
    }

    if (!srv_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = kTextureDescriptorCount * kFrameCount;
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const auto result = device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&srv_heap));
        if (FAILED(result))
        {
            return HResultFailure("Create SRV heap", result);
        }
        srv_stride =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    for (std::uint32_t frame_index = 0; frame_index < kFrameCount; ++frame_index)
    {
        auto srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount * srv_stride;
        auto create_srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &view, srv);
            srv.ptr += srv_stride;
        };
        create_srv(gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        create_srv(gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(gbuffer_position.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
        D3D12_SHADER_RESOURCE_VIEW_DESC shadow_view{};
        shadow_view.Format = DXGI_FORMAT_R32_FLOAT;
        shadow_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        shadow_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadow_view.Texture2D.MipLevels = 1;

        device->CreateShaderResourceView(shadow.resource.Get(), &shadow_view, srv);
        srv.ptr += srv_stride;
        create_srv(hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(oit_accumulation.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT);
        create_srv(post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(ui_textures[frame_index].resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
        srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
        srv.ptr += (static_cast<SIZE_T>(frame_index + 1) * kTextureDescriptorCount - 1) * srv_stride;
        create_srv(gbuffer_material.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateDepthAndShadow()
{
    transient_textures_common = enhanced;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_description{};
    dsv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_description.NumDescriptors = 4;
    auto result = device->CreateDescriptorHeap(&dsv_description, IID_PPV_ARGS(&dsv_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create DSV heap", result);
    }
    dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_CLEAR_VALUE clear{};
    clear.Format = kDepthFormat;
    clear.DepthStencil = {0.0f, 0};
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depth_description{};
    depth_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_description.Width = render_width;
    depth_description.Height = render_height;
    depth_description.DepthOrArraySize = 1;
    depth_description.MipLevels = 1;
    depth_description.Format = kDepthFormat;
    depth_description.SampleDesc = {1, 0};
    depth_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (auto created = CreateAllocation(depth, allocation, depth_description,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        !created)
    {
        return created;
    }
    device->CreateDepthStencilView(depth.resource.Get(), nullptr,
                                   dsv_heap->GetCPUDescriptorHandleForHeapStart());

    auto shadow_description = depth_description;
    shadow_description.Format = DXGI_FORMAT_R32_TYPELESS;
    const auto base_shadow = std::clamp(config.shadow_resolution, 1024u, 2048u);
    shadow_description.Width = base_shadow * 5u;
    shadow_description.Height = base_shadow * 4u;
    shadow_description.DepthOrArraySize = 1;
    if (auto created = CreateAllocation(shadow, allocation, shadow_description,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        !created)
    {
        return created;
    }
    auto shadow_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    shadow_handle.ptr += dsv_stride;
    for (std::uint32_t cascade = 0; cascade < 3; ++cascade)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = kDepthFormat;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;

        device->CreateDepthStencilView(shadow.resource.Get(), &view, shadow_handle);
        shadow_handle.ptr += dsv_stride;
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateGpuData()
{
    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    const auto vertex_description = BufferDescription(sizeof(kCubeVertices));
    if (auto created = CreateAllocation(vertices, upload_allocation, vertex_description,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
    {
        return created;
    }
    std::byte *mapped{};
    D3D12_RANGE no_read{};
    auto result = vertices.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(result))
    {
        return HResultFailure("Map vertex buffer", result);
    }
    std::memcpy(mapped, kCubeVertices.data(), sizeof(kCubeVertices));
    vertices.resource->Unmap(0, nullptr);
    vertex_view = {vertices.resource->GetGPUVirtualAddress(), sizeof(kCubeVertices),
                   sizeof(Vertex)};

    constexpr std::array<const char *, 6> monster_names{
        "enemy_melee", "enemy_ranged", "enemy_suicide", "boss_5m", "boss_10m", "boss_final"};
    std::vector<DirectX::XMFLOAT4X4> skin_matrices;
    for (std::size_t asset_index = 0; asset_index < monster_names.size(); ++asset_index)
    {
        auto &asset = monster_assets[asset_index];
        std::vector<SkinnedVertex> vertices_for_asset;
        std::vector<CharacterClipHeader> clips;
        std::vector<std::uint16_t> parents;
        std::vector<std::array<float, 16>> inverse_bind_matrices;
        std::vector<CharacterLocalTransform> transforms;
        std::vector<float> upper_body_weights;
        if (auto loaded = LoadCharacterAsset(
                CurrentExecutableDirectory() / "Cooked" /
                    (std::string(monster_names[asset_index]) + ".meshbin"),
                vertices_for_asset, clips, parents, inverse_bind_matrices,
                upper_body_weights, transforms,
                asset.bone_count, asset.ground_offset, asset.material_count, false);
            !loaded)
        {
            return loaded;
        }
        asset.vertex_count = static_cast<std::uint32_t>(vertices_for_asset.size());
        const auto bytes_for_asset = vertices_for_asset.size() * sizeof(vertices_for_asset.front());
        if (auto created = CreateAllocation(asset.vertices, upload_allocation,
                                            BufferDescription(bytes_for_asset),
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            !created)
        {
            return created;
        }
        std::byte *asset_mapped{};
        result = asset.vertices.resource->Map(0, &no_read,
                                              reinterpret_cast<void **>(&asset_mapped));
        if (FAILED(result)) return HResultFailure("Map monster vertex buffer", result);
        std::memcpy(asset_mapped, vertices_for_asset.data(), bytes_for_asset);
        asset.vertices.resource->Unmap(0, nullptr);
        asset.vertex_view = {asset.vertices.resource->GetGPUVirtualAddress(),
                             static_cast<UINT>(bytes_for_asset), sizeof(SkinnedVertex)};

        asset.skin_offset = static_cast<std::uint32_t>(skin_matrices.size());
        for (const auto &clip : clips)
        {
            asset.clip_meta[static_cast<std::size_t>(clip.clip)] = {
                static_cast<std::uint32_t>(skin_matrices.size() - asset.skin_offset),
                clip.frame_count, clip.looping, asset.bone_count};
            for (std::uint32_t frame = 0; frame < clip.frame_count; ++frame)
            {
                std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> globals{};
                for (std::uint32_t bone = 0; bone < asset.bone_count; ++bone)
                {
                    const auto &local = transforms[clip.first_transform +
                        frame * asset.bone_count + bone];
                    const auto local_matrix = DirectX::XMMatrixScaling(
                        local.scale[0], local.scale[1], local.scale[2]) *
                        DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(
                            local.rotation[0], local.rotation[1], local.rotation[2], local.rotation[3])) *
                        DirectX::XMMatrixTranslation(local.translation[0], local.translation[1],
                                                     local.translation[2]);
                    const auto parent = parents[bone];
                    const auto global = parent == std::numeric_limits<std::uint16_t>::max()
                                            ? local_matrix
                                            : local_matrix * DirectX::XMLoadFloat4x4(&globals[parent]);
                    DirectX::XMStoreFloat4x4(&globals[bone], global);
                    DirectX::XMFLOAT4X4 inverse_bind{};
                    std::memcpy(&inverse_bind, inverse_bind_matrices[bone].data(),
                                sizeof(inverse_bind));
                    // Cooked FBX inverse-bind matrices use column-vector layout;
                    // DirectX runtime transforms use row-vector layout.
                    const auto skin = DirectX::XMMatrixTranspose(
                                          DirectX::XMLoadFloat4x4(&inverse_bind)) *
                                      global;
                    const auto determinant = DirectX::XMVectorGetX(
                        DirectX::XMMatrixDeterminant(skin));
                    if (!std::isfinite(determinant) || determinant <= 0.0001f ||
                        DirectX::XMMatrixIsNaN(skin) || DirectX::XMMatrixIsInfinite(skin))
                    {
                        return Result::Failure(
                            ErrorCode::InvalidState, "hs_renderer_d3d12",
                            "Monster animation produced an invalid skin pose.");
                    }
                    auto &stored_skin = skin_matrices.emplace_back();
                    DirectX::XMStoreFloat4x4(&stored_skin,
                                             DirectX::XMMatrixTranspose(skin));
                }
            }
        }
    }
    const auto skin_bytes = skin_matrices.size() * sizeof(skin_matrices.front());
    if (auto created = CreateAllocation(monster_skin_matrices, upload_allocation,
                                        BufferDescription(skin_bytes),
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
        return created;
    result = monster_skin_matrices.resource->Map(0, &no_read,
                                                 reinterpret_cast<void **>(&mapped));
    if (FAILED(result)) return HResultFailure("Map monster skin buffer", result);
    std::memcpy(mapped, skin_matrices.data(), skin_bytes);
    monster_skin_matrices.resource->Unmap(0, nullptr);

    std::vector<SkinnedVertex> cooked_vertices;
    if (auto loaded = LoadCharacterAsset(
            CurrentExecutableDirectory() / "Cooked" / "archer.meshbin",
            cooked_vertices, archer_clips, archer_parents,
            archer_inverse_bind_matrices, archer_upper_body_weights,
            archer_transforms, archer_bone_count,
            archer_ground_offset, archer_material_count);
        !loaded)
    {
        return loaded;
    }
    archer_support_vertices.clear();
    for (const auto &vertex : cooked_vertices)
    {
        if (vertex.position[1] > -archer_ground_offset + 0.12f) continue;
        const auto duplicate = std::ranges::any_of(archer_support_vertices, [&](const auto &existing) {
            return existing.position == vertex.position &&
                   existing.bone_indices == vertex.bone_indices &&
                   existing.bone_weights == vertex.bone_weights;
        });
        if (!duplicate) archer_support_vertices.push_back(vertex);
    }
    archer_vertex_count = static_cast<std::uint32_t>(cooked_vertices.size());
    const auto archer_vertex_bytes = cooked_vertices.size() * sizeof(cooked_vertices.front());
    const auto archer_description = BufferDescription(archer_vertex_bytes);
    if (auto created = CreateAllocation(archer_vertices, upload_allocation,
                                        archer_description,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
    {
        return created;
    }
    mapped = nullptr;
    result = archer_vertices.resource->Map(0, &no_read,
                                           reinterpret_cast<void **>(&mapped));
    if (FAILED(result))
    {
        return HResultFailure("Map archer vertex buffer", result);
    }
    std::memcpy(mapped, cooked_vertices.data(), archer_vertex_bytes);
    archer_vertices.resource->Unmap(0, nullptr);
    archer_vertex_view = {archer_vertices.resource->GetGPUVirtualAddress(),
                           static_cast<UINT>(archer_vertex_bytes),
                           sizeof(SkinnedVertex)};

    std::vector<SkinnedVertex> projectile_vertices;
    std::vector<CharacterClipHeader> projectile_clips;
    std::vector<std::uint16_t> projectile_parents;
    std::vector<std::array<float, 16>> projectile_inverse_bind;
    std::vector<float> projectile_weights;
    std::vector<CharacterLocalTransform> projectile_transforms;
    std::uint32_t projectile_bones{}, projectile_materials{};
    float projectile_ground{};
    if (auto loaded = LoadCharacterAsset(
            CurrentExecutableDirectory() / "Cooked" / "enemy_gel_projectile.meshbin",
            projectile_vertices, projectile_clips, projectile_parents,
            projectile_inverse_bind, projectile_weights, projectile_transforms,
            projectile_bones, projectile_ground, projectile_materials, false); !loaded)
        return loaded;
    gel_projectile_vertex_count = static_cast<std::uint32_t>(projectile_vertices.size());
    const auto projectile_bytes = projectile_vertices.size() * sizeof(SkinnedVertex);
    if (auto created = CreateAllocation(gel_projectile_vertices, upload_allocation,
                                        BufferDescription(projectile_bytes),
                                        D3D12_RESOURCE_STATE_GENERIC_READ); !created)
        return created;
    result = gel_projectile_vertices.resource->Map(0, &no_read,
                                                    reinterpret_cast<void **>(&mapped));
    if (FAILED(result)) return HResultFailure("Map gel projectile vertex buffer", result);
    std::memcpy(mapped, projectile_vertices.data(), projectile_bytes);
    gel_projectile_vertices.resource->Unmap(0, nullptr);
    gel_projectile_vertex_view = {gel_projectile_vertices.resource->GetGPUVirtualAddress(),
                                  static_cast<UINT>(projectile_bytes), sizeof(SkinnedVertex)};

    D3D12MA::ALLOCATION_DESC default_allocation{};
    default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    const auto particle_description =
        BufferDescription(kParticleCount * sizeof(GpuParticle),
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (auto created = CreateAllocation(particles, default_allocation, particle_description,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        !created)
    {
        return created;
    }
    const auto particle_index_description =
        BufferDescription(kParticleCount * sizeof(std::uint32_t),
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (auto &alive : particle_alive)
    {
        if (auto created = CreateAllocation(alive, default_allocation,
                                            particle_index_description,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            !created)
        {
            return created;
        }
    }
    if (auto created = CreateAllocation(particle_dead, default_allocation,
                                        particle_index_description,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        !created)
    {
        return created;
    }
    if (auto created = CreateAllocation(
            particle_counters, default_allocation,
            BufferDescription(16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        !created)
    {
        return created;
    }
    const auto indirect_description =
        BufferDescription(sizeof(D3D12_DRAW_ARGUMENTS),
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return CreateAllocation(indirect_arguments, default_allocation, indirect_description,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

Result D3D12Renderer::Impl::CreateCharacterTextures()
{
    auto result = frames[0].allocator->Reset();
    if (FAILED(result))
    {
        return HResultFailure("Reset character texture allocator", result);
    }
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return HResultFailure("Reset character texture command list", result);
    }

    std::vector<AllocationResource> uploads;
    uploads.reserve(static_cast<std::size_t>(archer_material_count) * 2);
    const auto cooked = CurrentExecutableDirectory() / "Cooked";
    auto upload_array = [&](AllocationResource &texture,
                            std::wstring_view prefix) -> Result {
        D3D12_RESOURCE_DESC texture_description{};
        texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_description.Width = kCharacterTextureSize;
        texture_description.Height = kCharacterTextureSize;
        texture_description.DepthOrArraySize =
            static_cast<std::uint16_t>(archer_material_count);
        texture_description.MipLevels = 1;
        texture_description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        texture_description.SampleDesc = {1, 0};
        texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12MA::ALLOCATION_DESC default_allocation{};
        default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(texture, default_allocation,
                                            texture_description,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
            !created)
        {
            return created;
        }

        for (std::uint32_t index = 0; index < archer_material_count; ++index)
        {
            std::vector<std::byte> storage;
            std::span<const std::byte> pixels;
            std::uint32_t source_width{};
            std::uint32_t source_height{};
            const auto path = cooked / (std::wstring(prefix) + std::to_wstring(index) +
                                        L".dds");
            if (auto loaded = LoadRgbaDds(path, source_width, source_height, pixels,
                                          storage);
                !loaded)
            {
                return loaded;
            }
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT rows{};
            UINT64 row_size{};
            UINT64 upload_size{};
            device->GetCopyableFootprints(&texture_description, index, 1, 0,
                                          &footprint, &rows, &row_size,
                                          &upload_size);
            uploads.emplace_back();
            D3D12MA::ALLOCATION_DESC upload_allocation{};
            upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
            if (auto created = CreateAllocation(uploads.back(), upload_allocation,
                                                BufferDescription(upload_size),
                                                D3D12_RESOURCE_STATE_GENERIC_READ);
                !created)
            {
                return created;
            }
            std::byte *mapped{};
            D3D12_RANGE no_read{};
            result = uploads.back().resource->Map(
                0, &no_read, reinterpret_cast<void **>(&mapped));
            if (FAILED(result))
            {
                return HResultFailure("Map character texture upload", result);
            }
            for (std::uint32_t row = 0; row < kCharacterTextureSize; ++row)
            {
                auto *destination = mapped + footprint.Offset +
                                    static_cast<std::size_t>(row) *
                                        footprint.Footprint.RowPitch;
                const auto source_row = static_cast<std::uint64_t>(row) *
                                        source_height / kCharacterTextureSize;
                for (std::uint32_t column = 0; column < kCharacterTextureSize;
                     ++column)
                {
                    const auto source_column =
                        static_cast<std::uint64_t>(column) * source_width /
                        kCharacterTextureSize;
                    std::memcpy(destination + static_cast<std::size_t>(column) * 4,
                                pixels.data() +
                                    (source_row * source_width + source_column) * 4,
                                4);
                }
            }
            uploads.back().resource->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = index;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = uploads.back().resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprint;
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source,
                                            nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
        return Result::Success();
    };

    auto upload_shared = [&](AllocationResource &texture, std::wstring_view filename) -> Result {
        std::vector<std::byte> storage;
        std::span<const std::byte> pixels;
        std::uint32_t source_width{}, source_height{};
        if (auto loaded = LoadRgbaDds(cooked / filename, source_width, source_height,
                                      pixels, storage); !loaded)
            return loaded;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = source_width;
        description.Height = source_height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        description.SampleDesc = {1, 0};
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12MA::ALLOCATION_DESC default_allocation{};
        default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(texture, default_allocation, description,
                                             D3D12_RESOURCE_STATE_COPY_DEST);
            !created)
            return created;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows{};
        UINT64 row_size{};
        UINT64 upload_size{};
        device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rows,
                                      &row_size, &upload_size);
        uploads.emplace_back();
        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), upload_allocation,
                                             BufferDescription(upload_size),
                                             D3D12_RESOURCE_STATE_GENERIC_READ);
            !created)
            return created;
        std::byte *mapped{};
        D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(0, &no_read,
                                               reinterpret_cast<void **>(&mapped));
        if (FAILED(result))
            return HResultFailure("Map monster texture upload", result);
        for (std::uint32_t row = 0; row < source_height; ++row)
        {
            auto *destination = mapped + footprint.Offset +
                                static_cast<std::size_t>(row) * footprint.Footprint.RowPitch;
            const auto source_row = row;
            for (std::uint32_t column = 0; column < source_width; ++column)
            {
                const auto source_column = column;
                std::memcpy(destination + static_cast<std::size_t>(column) * 4,
                            pixels.data() + (source_row * source_width + source_column) * 4,
                            4);
            }
        }
        uploads.back().resource->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = uploads.back().resource.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
        return Result::Success();
    };

    auto upload_family_array = [&](AllocationResource &texture,
                                   std::wstring_view prefix) -> Result {
        constexpr std::uint32_t slices = 3;
        constexpr std::uint32_t mips = 11;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = 1024u;
        description.Height = 1024u;
        description.DepthOrArraySize = slices;
        description.MipLevels = mips;
        description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        description.SampleDesc = {1, 0};
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(texture, allocation, description,
                                             D3D12_RESOURCE_STATE_COPY_DEST); !created)
            return created;
        const auto count = slices * mips;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> row_sizes(count);
        UINT64 upload_size{};
        device->GetCopyableFootprints(&description, 0, count, 0, footprints.data(),
                                      rows.data(), row_sizes.data(), &upload_size);
        uploads.emplace_back();
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), allocation,
                                             BufferDescription(upload_size),
                                             D3D12_RESOURCE_STATE_GENERIC_READ); !created)
            return created;
        std::byte *mapped{}; D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(0, &no_read,
                                               reinterpret_cast<void **>(&mapped));
        if (FAILED(result)) return HResultFailure("Map family texture upload", result);

        for (std::uint32_t slice = 0; slice < slices; ++slice)
        {
            std::size_t source_offset{};
            std::vector<std::byte> storage; std::span<const std::byte> pixels;
            std::uint32_t width{}, height{}, level_count{};
            static constexpr std::wstring_view names[] = {L"melee", L"ranged", L"suicide"};
            const auto stem = prefix == L"enemy_" ?
                (std::wstring(prefix) + std::wstring(names[slice]) + L"_diffuse_") :
                (std::wstring(prefix) + std::wstring(names[slice]) + L"_0.dds");
            const auto path = prefix == L"enemy_" ? cooked / (stem + L"0.dds") :
                cooked / (std::wstring(L"enemy_") + std::wstring(names[slice]) + L"_normal_0.dds");
            if (auto loaded = LoadRgbaDds(path, width, height, pixels, storage, &level_count);
                !loaded) return loaded;
            if (width != 1024u || height != 1024u || level_count != mips)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Family DDS dimensions or mips are invalid.");
            for (std::uint32_t mip = 0; mip < mips; ++mip)
            {
                const auto index = slice * mips + mip;
                const auto w = std::max(1u, width >> mip);
                const auto h = std::max(1u, height >> mip);
                const auto bytes_per_row = static_cast<std::size_t>(w) * 4;
                for (std::uint32_t row = 0; row < h; ++row)
                    std::memcpy(mapped + footprints[index].Offset + static_cast<std::size_t>(row) * footprints[index].Footprint.RowPitch,
                                pixels.data() + source_offset + static_cast<std::size_t>(row) * bytes_per_row,
                                bytes_per_row);
                source_offset += bytes_per_row * h;
            }
        }
        uploads.back().resource->Unmap(0, nullptr);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = texture.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = index;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = uploads.back().resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprints[index];
            command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get(); barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; command_list->ResourceBarrier(1, &barrier);
        return Result::Success();
    };

    if (auto uploaded = upload_array(archer_diffuse, L"archer_diffuse_");
        !uploaded)
    {
        return uploaded;
    }
    if (auto uploaded = upload_shared(monster_basecolor, L"monster_basecolor.dds"); !uploaded)
        return uploaded;
    if (auto uploaded = upload_shared(monster_emissive, L"monster_emissive.dds"); !uploaded)
        return uploaded;
    if (auto uploaded = upload_shared(monster_ram, L"monster_ram.dds"); !uploaded)
        return uploaded;
    if (auto uploaded = upload_family_array(family_diffuse, L"enemy_"); !uploaded)
        return uploaded;
    if (auto uploaded = upload_family_array(family_normal, L"enemy_normal_"); !uploaded)
        return uploaded;
    if (auto uploaded = upload_array(archer_normal, L"archer_normal_"); !uploaded)
    {
        return uploaded;
    }
    {
        DdsHeader header{};
        std::vector<std::byte> storage;
        std::span<const std::byte> pixels;
        if (auto loaded = LoadVfxMaskDds(cooked / "vfx_masks.dds", header,
                                         vfx_sprite_count,
                                         pixels, storage);
            !loaded)
            return loaded;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = header.width;
        description.Height = header.height;
        description.DepthOrArraySize = static_cast<UINT16>(vfx_sprite_count);
        description.MipLevels = static_cast<std::uint16_t>(header.mip_count);
        description.Format = DXGI_FORMAT_BC4_UNORM;
        description.SampleDesc = {1, 0};
        D3D12MA::ALLOCATION_DESC default_allocation{};
        default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(vfx_masks, default_allocation,
                                            description,
                                            D3D12_RESOURCE_STATE_COPY_DEST);
            !created)
            return created;

        const auto subresource_count = vfx_sprite_count * header.mip_count;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresource_count);
        std::vector<UINT> rows(subresource_count);
        std::vector<UINT64> row_sizes(subresource_count);
        UINT64 upload_size{};
        device->GetCopyableFootprints(&description, 0, subresource_count, 0,
                                      footprints.data(), rows.data(),
                                      row_sizes.data(), &upload_size);
        uploads.emplace_back();
        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), upload_allocation,
                                            BufferDescription(upload_size),
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            !created)
            return created;
        std::byte *mapped{};
        D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(
            0, &no_read, reinterpret_cast<void **>(&mapped));
        if (FAILED(result)) return HResultFailure("Map VFX mask upload", result);
        std::size_t source_offset{};

        for (std::uint32_t subresource = 0; subresource < subresource_count;
             ++subresource)
        {
            for (std::uint32_t row = 0; row < rows[subresource]; ++row)
            {
                std::memcpy(mapped + footprints[subresource].Offset +
                                static_cast<std::size_t>(row) *
                                    footprints[subresource].Footprint.RowPitch,
                            pixels.data() + source_offset,
                            static_cast<std::size_t>(row_sizes[subresource]));
                source_offset += static_cast<std::size_t>(row_sizes[subresource]);
            }
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = vfx_masks.resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = uploads.back().resource.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[subresource];
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source,
                                            nullptr);
        }
        uploads.back().resource->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = vfx_masks.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
    }
    result = command_list->Close();
    if (FAILED(result))
    {
        return HResultFailure("Close character texture upload", result);
    }
    ID3D12CommandList *lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (auto waited = WaitForGpu(); !waited)
    {
        return waited;
    }

    for (std::uint32_t frame_index = 0; frame_index < kFrameCount; ++frame_index)
    {
        auto handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount +
                       kPostTextureDescriptorCount) * srv_stride;
        auto create_view = [&](ID3D12Resource *texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray.MipLevels = 1;
            view.Texture2DArray.ArraySize = archer_material_count;
            device->CreateShaderResourceView(texture, &view, handle);
            handle.ptr += srv_stride;
        };
        create_view(archer_diffuse.resource.Get(),
                    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_view(archer_normal.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12_SHADER_RESOURCE_VIEW_DESC vfx_view{};
        vfx_view.Format = DXGI_FORMAT_BC4_UNORM;
        vfx_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        vfx_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vfx_view.Texture2DArray.MipLevels = 10;
        vfx_view.Texture2DArray.ArraySize = vfx_sprite_count;
        device->CreateShaderResourceView(vfx_masks.resource.Get(), &vfx_view, handle);
        handle.ptr += srv_stride;
        auto create_shared_view = [&](ID3D12Resource *texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(texture, &view, handle);
            handle.ptr += srv_stride;
        };
        create_shared_view(monster_basecolor.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_shared_view(monster_emissive.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_shared_view(monster_ram.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        auto create_family_view = [&](ID3D12Resource *texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray.MipLevels = 11; view.Texture2DArray.ArraySize = 3;
            device->CreateShaderResourceView(texture, &view, handle); handle.ptr += srv_stride;
        };
        create_family_view(family_diffuse.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_family_view(family_normal.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    return Result::Success();
}


} // namespace hs
