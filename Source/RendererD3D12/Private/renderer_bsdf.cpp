#include "renderer_impl.hpp"

namespace hs
{

Result D3D12Renderer::Impl::CreateEnvironmentBrdfLut(const RendererConfig &lut_config,
                                                  AllocationResource &lut)
{
    constexpr UINT extent = 128;
    constexpr DXGI_FORMAT format = DXGI_FORMAT_R16G16_FLOAT;
    std::filesystem::path shader_path;
    if (auto resolved = lut_config.ResolveProduct("render/shader/bsdf_preintegrate_cs.dxil",
                                               shader_path); !resolved) return resolved;
    std::vector<std::byte> shader;
    if (auto read = ReadBinary(shader_path, shader); !read) return read;
    auto result = device.As(&environment_brdf_init_device);
    if (FAILED(result)) return HResultFailure("Query BSDF initialization device", result);

    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_ROOT_PARAMETER1 parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_description{};
    root_description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_description.Desc_1_1 = {1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized, errors;
    result = D3D12SerializeVersionedRootSignature(&root_description, &serialized, &errors);
    if (FAILED(result)) return HResultFailure("Serialize BSDF integration root signature", result);
    ComPtr<ID3D12RootSignature> root;
    result = device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(result)) return HResultFailure("Create BSDF integration root signature", result);
    if (auto registered = pipeline_cache.RegisterRootSignature(root.Get(),
            {static_cast<const std::byte *>(serialized->GetBufferPointer()), serialized->GetBufferSize()});
        !registered) return registered;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    result = pipeline_cache.CreateCompute(&pipeline_description, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) return HResultFailure("Create BSDF integration pipeline", result);

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = extent;
    texture.Height = extent;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = format;
    texture.SampleDesc = {1, 0};
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(lut, allocation, texture,
                                        D3D12_RESOURCE_STATE_COMMON); !created) return created;
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 1;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    result = device->CreateDescriptorHeap(&heap_description, IID_PPV_ARGS(&heap));
    if (FAILED(result)) return HResultFailure("Create BSDF integration UAV heap", result);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(lut.resource.Get(), nullptr, &uav,
                                     heap->GetCPUDescriptorHandleForHeapStart());

    // One immutable initialization; all helper COM objects outlive the submission.
    // The texture survives resize and is only released after the renderer drain.
    result = frames[0].allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset BSDF integration allocator", result);
    result = command_list->Reset(frames[0].allocator.Get(), pipeline.Get());
    if (FAILED(result)) return HResultFailure("Reset BSDF integration list", result);
    TransitionTexture(lut.resource.Get(), D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_COMMON,
        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetComputeRootSignature(root.Get());
    command_list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    command_list->Dispatch(extent / 8, extent / 8, 1);
    TransitionTexture(lut.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close BSDF integration list", result);
    environment_brdf_init_root = std::move(root);
    environment_brdf_init_pipeline = std::move(pipeline);
    environment_brdf_init_heap = std::move(heap);
    environment_brdf_init_pending = true;
    ID3D12CommandList *lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (auto waited = WaitForGpu(); !waited) return waited;
    environment_brdf_init_pending = false;
    environment_brdf_init_root.Reset();
    environment_brdf_init_pipeline.Reset();
    environment_brdf_init_heap.Reset();
    environment_brdf_init_device.Reset();
    lut.graph_access = Access::AllShaderRead;
    lut.graph_queue = QueueHint::Direct;
    return Result::Success();
}

void D3D12Renderer::Impl::WriteEnvironmentBrdfLutDescriptors()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    for (UINT frame = 0; frame < kFrameCount; ++frame)
    {
        auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        // Terrain height retains its resource and authored space4 binding.
        // This replaces the unused legacy environment t4,space2 descriptor only.
        handle.ptr += static_cast<SIZE_T>(frame * kTextureDescriptorCount +
            kPostTextureDescriptorCount + kCharacterDescriptorCount +
            kMonsterPbrDescriptorCount + 4) * srv_stride;
        device->CreateShaderResourceView(environment_brdf_lut.resource.Get(), &srv, handle);
    }
}

} // namespace hs
