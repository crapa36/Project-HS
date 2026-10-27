#include "renderer_impl.hpp"

#include <array>
#include <cstring>
#include <utility>

namespace hs
{
Result D3D12Renderer::Impl::PrepareViewTargets(OptionTargets &targets,
                                             std::uint32_t capacity_width,
                                             std::uint32_t capacity_height,
                                             std::uint32_t output_width,
                                             std::uint32_t output_height,
                                             bool native_resolve)
{
    if (!capacity_width || !capacity_height || !output_width || !output_height)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "View target extents must be nonzero.");
    for (const auto format : {DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_R32G32_UINT,
                              DXGI_FORMAT_R16G16B16A16_FLOAT})
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        const auto queried = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                         &support, sizeof(support));
        if (FAILED(queried)) return HResultFailure("Query view target format", queried);
        const auto required = D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
                              D3D12_FORMAT_SUPPORT1_SHADER_LOAD;
        if ((support.Support1 & required) != required)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Selected temporal view target format is unsupported.");
    }

    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    const auto create = [&](AllocationResource &resource, DXGI_FORMAT format,
                             std::uint32_t target_width,
                             std::uint32_t target_height) -> Result {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = target_width;
        description.Height = target_height;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        return CreateAllocation(resource, allocation, description,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear);
    };
    if (native_resolve)
    {
    for (auto &history : targets.temporal_normal_history)
        if (auto result = create(history, DXGI_FORMAT_R16G16_SNORM,
                                  output_width, output_height); !result)
            return result;
    for (auto &history : targets.temporal_material_history)
        if (auto result = create(history, DXGI_FORMAT_R32G32_UINT,
                                  output_width, output_height); !result)
            return result;
    }
    if (auto result = create(targets.scene_work, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              capacity_width, capacity_height); !result)
        return result;
    if (auto result = create(targets.display_linear, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              output_width, output_height); !result)
        return result;
    return create(targets.composite_hdr, DXGI_FORMAT_R16G16B16A16_FLOAT,
                   capacity_width, capacity_height);
}

Result D3D12Renderer::Impl::PrepareExposureResources(OwnedView &owner)
{
    if (!sampler_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        heap.NumDescriptors = 2 * kFrameCount;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const auto created = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&sampler_heap));
        if (FAILED(created)) return HResultFailure("Create view sampler heap", created);
        sampler_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }
    // Build the complete initial set before publishing it. Resizes replace view
    // textures only, preserving these buffers and the adaptation read index.
    std::array<AllocationResource, 2> values;
    AllocationResource histogram;
    AllocationResource initial;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    for (auto &value : values)
        if (auto result = CreateAllocation(value, allocation,
                BufferDescription(2 * sizeof(float), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_COPY_DEST); !result)
            return result;
    if (auto result = CreateAllocation(histogram, allocation,
            BufferDescription(256 * sizeof(std::uint32_t),
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS); !result)
        return result;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    constexpr std::array initial_values{1.0f, 1.0f};
    if (auto result = CreateAllocation(initial, allocation,
            BufferDescription(sizeof(initial_values)), D3D12_RESOURCE_STATE_GENERIC_READ); !result)
        return result;
    void *mapped{};
    const D3D12_RANGE no_read{};
    const auto map_result = initial.resource->Map(0, &no_read, &mapped);
    if (FAILED(map_result)) return HResultFailure("Map initial exposure upload", map_result);
    std::memcpy(mapped, initial_values.data(), sizeof(initial_values));
    const D3D12_RANGE written{0, sizeof(initial_values)};
    initial.resource->Unmap(0, &written);

    using std::swap;
    swap(owner.exposure_values, values);
    swap(owner.exposure_histogram, histogram);
    swap(owner.initial_exposure, initial);
    owner.exposure_read_index = 0;
    owner.exposure_initialized = false;
    // The first frame graph copies the immutable upload into both values. No
    // command submission or blocking startup upload occurs in resource creation.
    return Result::Success();
}

void D3D12Renderer::Impl::WriteViewDescriptors(OwnedView &owner, std::uint32_t frame,
                                              std::uint32_t read_history,
                                              std::uint32_t write_history,
                                              std::uint32_t exposure_read,
                                              std::uint32_t exposure_write,
                                              D3D12_GPU_VIRTUAL_ADDRESS exposure_cb_gpu_address) noexcept
{
    // Both guide targets have stable RTV slots; read/write indices select the
    // shader history and the frame's target bindings independently.
    (void)write_history;
    auto shadow_null = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    shadow_null.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + kVirtualShadowDescriptorOffset) * srv_stride;
    for (UINT index = 0; index < kVirtualShadowDescriptorCount; ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (index == 2 || index == 3 || index == 5 || index == 6)
        {
            view.Format = DXGI_FORMAT_R32_FLOAT;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
        }
        else
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Buffer.NumElements = index == 0 ? sizeof(VirtualShadowConstants) / 4 : 1;
            if (index == 0) { view.Format = DXGI_FORMAT_R32_TYPELESS; view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW; }
            else view.Buffer.StructureByteStride = 32;
        }
        device->CreateShaderResourceView(nullptr, &view, shadow_null);
        shadow_null.ptr += srv_stride;
    }
    auto hlod_null = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    hlod_null.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + kHlodDescriptorOffset) * srv_stride;
    for (UINT index = 0; index != 2; ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Buffer.NumElements = 1;
        view.Buffer.StructureByteStride = 16;
        device->CreateShaderResourceView(nullptr, &view, hlod_null);
        hlod_null.ptr += srv_stride;
    }
    auto local_probe_null = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    local_probe_null.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + kLocalProbeDescriptorOffset) * srv_stride;
    for (UINT index = 0; index != 5; ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = index == 3 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (index < 2) { view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; view.TextureCubeArray.MipLevels = 1; view.TextureCubeArray.NumCubes = 1; }
        else if (index == 3) { view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; view.Texture2DArray.MipLevels = 1; view.Texture2DArray.ArraySize = 6; }
        else
        {
            view.Format = DXGI_FORMAT_UNKNOWN; view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Buffer.NumElements = 1; view.Buffer.StructureByteStride = index == 2 ? 16 : 128;
        }
        device->CreateShaderResourceView(nullptr, &view, local_probe_null);
        local_probe_null.ptr += srv_stride;
    }
    auto volume_null = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    volume_null.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + kVolumetricDescriptorOffset) * srv_stride;
    device->CreateConstantBufferView(nullptr, volume_null);
    volume_null.ptr += srv_stride;
    for (UINT index = 0; index < 14; ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = index == 0 || index == 4 || index == 7 || index >= 11 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        if (index == 2)
        {
            view.Format = DXGI_FORMAT_UNKNOWN;
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Buffer.NumElements = 1;
            view.Buffer.StructureByteStride = sizeof(VolumetricVolume);
        }
        else if (index == 0 || index == 1 || index == 7 || index == 12 || index == 13)
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = 1;
        }
        else
        {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            view.Texture3D.MipLevels = 1;
        }
        device->CreateShaderResourceView(nullptr, &view, volume_null);
        volume_null.ptr += srv_stride;
    }
    for (UINT index = 0; index < 9; ++index)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = index == 1 || index == 4 || index == 7 ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        view.ViewDimension = index == 4 || index == 8 ? D3D12_UAV_DIMENSION_TEXTURE2D : D3D12_UAV_DIMENSION_TEXTURE3D;
        if (view.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D) view.Texture3D.WSize = 1;
        device->CreateUnorderedAccessView(nullptr, nullptr, &view, volume_null);
        volume_null.ptr += srv_stride;
    }
    // A valid null table remains bound when hero layering is disabled. Enabled
    // registration replaces it with immutable tables after this slot is waited.
    auto null_table = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    null_table.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount +
        kHeroTransparencyDescriptorOffset) * srv_stride;
    device->CreateConstantBufferView(nullptr, null_table);
    null_table.ptr += srv_stride;
    for (std::uint32_t index = 0; index < 21; ++index)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = index == 19 || index == 13 ? DXGI_FORMAT_R32_UINT
            : index == 18 || (index >= 4 && index < 8) ? DXGI_FORMAT_R32G32_UINT
            : index == 15 || index == 20 ? DXGI_FORMAT_R16G16_FLOAT
            : index == 16 ? DXGI_FORMAT_R32G32_FLOAT
            : index == 17 || index == 12 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(nullptr, &view, null_table);
        null_table.ptr += srv_stride;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC overflow{};
    overflow.Format = DXGI_FORMAT_R32_UINT;
    overflow.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(nullptr, nullptr, &overflow, null_table);
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount + frame * kRtvBankStride + kViewRtvOffset) * rtv_stride;
    const auto create_rtv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource, &view, rtv);
        rtv.ptr += rtv_stride;
    };
    for (auto &history : owner.targets.temporal_normal_history)
        create_rtv(history.resource.Get(), DXGI_FORMAT_R16G16_SNORM);
    for (auto &history : owner.targets.temporal_material_history)
        create_rtv(history.resource.Get(), DXGI_FORMAT_R32G32_UINT);
    create_rtv(owner.targets.scene_work.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_rtv(owner.targets.display_linear.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_rtv(owner.targets.composite_hdr.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);

    auto descriptor = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    descriptor.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount +
                       kViewDescriptorOffset) * srv_stride;
    const auto create_texture_srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += srv_stride;
    };
    create_texture_srv(owner.targets.temporal_normal_history[read_history].resource.Get(), DXGI_FORMAT_R16G16_SNORM);
    create_texture_srv(owner.targets.temporal_material_history[read_history].resource.Get(), DXGI_FORMAT_R32G32_UINT);
    create_texture_srv(owner.targets.scene_work.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_texture_srv(owner.targets.display_linear.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_texture_srv(owner.targets.composite_hdr.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_texture_srv(owner.targets.scene_work.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);

    D3D12_SHADER_RESOURCE_VIEW_DESC exposure_srv{};
    exposure_srv.Format = DXGI_FORMAT_UNKNOWN;
    exposure_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    exposure_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    exposure_srv.Buffer.NumElements = 2;
    exposure_srv.Buffer.StructureByteStride = sizeof(float);
    device->CreateShaderResourceView(owner.exposure_values[exposure_read].resource.Get(),
                                     &exposure_srv, descriptor);
    descriptor.ptr += srv_stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC buffer_uav{};
    buffer_uav.Format = DXGI_FORMAT_UNKNOWN;
    buffer_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    buffer_uav.Buffer.NumElements = 2;
    buffer_uav.Buffer.StructureByteStride = sizeof(float);
    device->CreateUnorderedAccessView(owner.exposure_values[exposure_write].resource.Get(),
                                      nullptr, &buffer_uav, descriptor);
    descriptor.ptr += srv_stride;
    buffer_uav.Buffer.NumElements = 256;
    buffer_uav.Buffer.StructureByteStride = sizeof(std::uint32_t);
    device->CreateUnorderedAccessView(owner.exposure_histogram.resource.Get(), nullptr,
                                      &buffer_uav, descriptor);
    descriptor.ptr += srv_stride;
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
    cbv.BufferLocation = exposure_cb_gpu_address;
    cbv.SizeInBytes = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
    device->CreateConstantBufferView(exposure_cb_gpu_address ? &cbv : nullptr, descriptor);
    // Keep the native table's ambient views at offsets 11/12.
    descriptor.ptr += 2u * srv_stride;
    auto sampler = sampler_heap->GetCPUDescriptorHandleForHeapStart();
    create_texture_srv(nullptr, DXGI_FORMAT_R16_FLOAT);
    create_texture_srv(nullptr, DXGI_FORMAT_R16_FLOAT);
    sampler.ptr += static_cast<SIZE_T>(frame) * 2 * sampler_stride;
    D3D12_SAMPLER_DESC sampling{};
    sampling.Filter = D3D12_FILTER_ANISOTROPIC;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampling.MaxAnisotropy = config.anisotropic_filtering;
    sampling.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampling.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&sampling, sampler);
    sampler.ptr += sampler_stride;
    sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampling.MaxLOD = 5.0f;
    device->CreateSampler(&sampling, sampler);
}
} // namespace hs
