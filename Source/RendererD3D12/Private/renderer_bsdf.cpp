#include "renderer_impl.hpp"
#include <hs/render_data/dds_texture.hpp>

namespace hs
{

Result D3D12Renderer::Impl::CreateEnvironmentBrdfLut(const RendererConfig &lut_config,
                                                  AllocationResource &lut)
{
    constexpr UINT extent = 128;
    constexpr DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    std::filesystem::path path;
    if (auto resolved = lut_config.ResolveProduct("render/lighting/coated_ggx_dfg.dds", path);
        !resolved) return resolved;
    std::vector<std::byte> bytes;
    if (auto read = ReadBinary(path, bytes); !read) return read;
    render_data::DdsTextureView dds;
    try
    {
        dds = render_data::DecodeDdsTexture(bytes);
        const auto &shape = dds.description;
        if (dds.dimension != 3 || dds.misc != 0 || shape.width != extent ||
            shape.height != extent || shape.array_size != 1 || shape.mip_count != 1 ||
            shape.format != static_cast<std::uint32_t>(format))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Cooked coated DFG shape does not match the shader ABI.");
    }
    catch (const std::exception &error)
    { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", error.what()); }
    auto result = device.As(&environment_brdf_init_device);
    if (FAILED(result)) return HResultFailure("Query BSDF initialization device", result);

    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = extent;
    texture.Height = extent;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = format;
    texture.SampleDesc = {1, 0};
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(lut, allocation, texture,
                                        D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{}, upload_size{};
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, &rows, &row_size, &upload_size);
    AllocationResource upload;
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(upload, allocation, BufferDescription(upload_size),
        D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
    void *mapped{};
    const D3D12_RANGE no_read{};
    result = upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(result)) return HResultFailure("Map cooked DFG upload", result);
    const auto &source = dds.subresources.front();
    for (UINT row = 0; row < rows; ++row)
        std::memcpy(static_cast<std::byte *>(mapped) + footprint.Offset +
                    static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                    source.bytes.data() + static_cast<std::size_t>(row) * source.row_pitch,
                    static_cast<std::size_t>(row_size));
    upload.resource->Unmap(0, nullptr);

    // Staging survives a failed drain; the immutable texture survives resize.
    result = frames[0].allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset BSDF integration allocator", result);
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result)) return HResultFailure("Reset BSDF integration list", result);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = lut.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION origin{};
    origin.pResource = upload.resource.Get();
    origin.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    origin.PlacedFootprint = footprint;
    command_list->CopyTextureRegion(&destination, 0, 0, 0, &origin, nullptr);
    TransitionTexture(lut.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close BSDF integration list", result);
    environment_brdf_init_upload = std::move(upload);
    environment_brdf_init_pending = true;
    ID3D12CommandList *lists[] = {command_list.Get()};
    MeasureStartupOperation(RendererStartupOperation::UploadSubmit, [&] { return queue->ExecuteCommandLists(1, lists); });
    if (auto waited = WaitForGpu(); !waited) return waited;
    environment_brdf_init_pending = false;
    environment_brdf_init_upload.Reset();
    environment_brdf_init_device.Reset();
    lut.graph_access = Access::AllShaderRead;
    lut.graph_queue = QueueHint::Direct;
    return Result::Success();
}

void D3D12Renderer::Impl::WriteEnvironmentBrdfLutDescriptors()
{
    StartupOperationScope startup_timing(RendererStartupOperation::DescriptorWrite);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
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
