#include "renderer_impl.hpp"

namespace hs
{
Result D3D12Renderer::Impl::CreateEnvironmentTextures()
{
    struct TextureSpec { const char *name; DXGI_FORMAT format; UINT slices; UINT mips; UINT width{2048}; UINT height{2048}; };
    constexpr TextureSpec specs[] = {
        {"terrain_gaussian", DXGI_FORMAT_BC7_UNORM,4,12},
        {"terrain_surface", DXGI_FORMAT_BC5_UNORM,4,12},
        {"terrain_normal", DXGI_FORMAT_BC5_UNORM,4,12},
        {"terrain_detail", DXGI_FORMAT_BC5_UNORM,4,12},
        {"terrain_height", DXGI_FORMAT_BC4_UNORM,4,12},
        {"rock_basecolor", DXGI_FORMAT_BC7_UNORM_SRGB,4,12},
        {"rock_surface", DXGI_FORMAT_BC5_UNORM,4,12},
        {"rock_normal", DXGI_FORMAT_BC5_UNORM,4,12},
        {"rock_detail", DXGI_FORMAT_BC5_UNORM,4,12},
        {"bark_basecolor", DXGI_FORMAT_BC7_UNORM_SRGB,3,12},
        {"bark_surface", DXGI_FORMAT_BC5_UNORM,3,12},
        {"bark_normal", DXGI_FORMAT_BC5_UNORM,3,12},
        {"bark_detail", DXGI_FORMAT_BC5_UNORM,3,12},
        {"grass_basecolor", DXGI_FORMAT_BC7_UNORM_SRGB,2,6},
        {"grass_normal", DXGI_FORMAT_BC5_UNORM,2,6},
        {"grass_roughness", DXGI_FORMAT_BC4_UNORM,2,6},
        {"leaf_basecolor", DXGI_FORMAT_BC7_UNORM_SRGB,3,6},
        {"leaf_normal", DXGI_FORMAT_BC5_UNORM,3,6},
        {"leaf_roughness", DXGI_FORMAT_BC4_UNORM,3,6},
        {"terrain_histogram", DXGI_FORMAT_R32G32B32A32_FLOAT,4,1,256,16},
    };
    static_assert(std::size(specs) == kEnvironmentTextureCount);
    auto result = frames[0].allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset environment upload allocator", result);
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result)) return HResultFailure("Reset environment upload list", result);
    std::vector<AllocationResource> uploads;
    const auto cooked = CurrentExecutableDirectory() / "Cooked";
    for (UINT texture_index = 0; texture_index < std::size(specs); ++texture_index)
    {
        const auto &spec = specs[texture_index];
        const auto path = cooked / (std::string("environment_") + spec.name + ".dds");
        std::vector<std::byte> storage;
        if (auto read = ReadBinary(path, storage); !read) return read;
        constexpr auto header_size = sizeof(kDdsMagic) + sizeof(DdsHeader) + sizeof(DdsHeaderDx10);
        auto invalid = [&] { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Invalid environment DDS: " + path.string()); };
        if (storage.size() < header_size) return invalid();
        UINT magic{}; DdsHeader header{}; DdsHeaderDx10 extension{};
        std::memcpy(&magic, storage.data(), sizeof(magic));
        std::memcpy(&header, storage.data() + sizeof(magic), sizeof(header));
        std::memcpy(&extension, storage.data() + sizeof(magic) + sizeof(header), sizeof(extension));
        if (magic != kDdsMagic || header.size != 124 || header.pixel_format.size != 32 ||
            header.pixel_format.four_cc != 0x30315844 || header.width != spec.width || header.height != spec.height ||
            header.mip_count != spec.mips || extension.array_size != spec.slices ||
            extension.format != spec.format || extension.dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            extension.misc_flag != 0) return invalid();
        const bool uncompressed = spec.format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        const UINT block_bytes = spec.format == DXGI_FORMAT_BC4_UNORM ? 8u : 16u;
        std::size_t expected{};
        for (UINT slice = 0; slice < spec.slices; ++slice)
            for (UINT mip = 0; mip < spec.mips; ++mip)
            {
                const auto width = std::max(1u, spec.width >> mip);
                const auto height = std::max(1u, spec.height >> mip);
                expected += uncompressed ? static_cast<std::size_t>(width) * height * 16u
                    : static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * block_bytes;
            }
        if (storage.size() != header_size + expected) return invalid();
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = spec.width;
        description.Height = spec.height;
        description.DepthOrArraySize = static_cast<UINT16>(spec.slices);
        description.MipLevels = static_cast<UINT16>(spec.mips);
        description.Format = spec.format;
        description.SampleDesc = {1, 0};
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        auto &texture = environment_textures[texture_index];
        if (auto created = CreateAllocation(texture, allocation, description,
                D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
        const auto count = spec.slices * spec.mips;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count); std::vector<UINT64> row_sizes(count);
        UINT64 upload_size{};
        device->GetCopyableFootprints(&description, 0, count, 0, footprints.data(),
            rows.data(), row_sizes.data(), &upload_size);
        uploads.emplace_back();
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), allocation, BufferDescription(upload_size),
                D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
        std::byte *mapped{}; D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
        if (FAILED(result)) return HResultFailure("Map environment upload", result);
        std::size_t source_offset = header_size;
        for (UINT subresource = 0; subresource < count; ++subresource)
        {
            for (UINT row = 0; row < rows[subresource]; ++row)
            {
                std::memcpy(mapped + footprints[subresource].Offset +
                    static_cast<std::size_t>(row) * footprints[subresource].Footprint.RowPitch,
                    storage.data() + source_offset, static_cast<std::size_t>(row_sizes[subresource]));
                source_offset += static_cast<std::size_t>(row_sizes[subresource]);
            }
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = texture.resource.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = uploads.back().resource.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprints[subresource];
            command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        uploads.back().resource->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
    }
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close environment upload", result);
    ID3D12CommandList *lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (auto waited = WaitForGpu(); !waited) return waited;
    for (UINT frame = 0; frame < kFrameCount; ++frame)
    {
        auto handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount +
            kPostTextureDescriptorCount + kCharacterDescriptorCount + kMonsterPbrDescriptorCount) * srv_stride;
        for (UINT index = 0; index < std::size(specs); ++index)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = specs[index].format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray.MipLevels = specs[index].mips;
            view.Texture2DArray.ArraySize = specs[index].slices;
            device->CreateShaderResourceView(environment_textures[index].resource.Get(), &view, handle);
            handle.ptr += srv_stride;
        }
    }
    return Result::Success();
}
} // namespace hs
