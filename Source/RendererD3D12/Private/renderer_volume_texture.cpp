#include "renderer_impl.hpp"

#include <hs/render_data/volume_texture_package.hpp>

#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace hs
{
namespace
{
constexpr D3D12_RESOURCE_STATES kAllShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr std::string_view kNoiseVolumePackage =
    "render/texture/vfx_noise_volume_64.rpkg";
constexpr std::string_view kNoiseVolumeSource =
    "vfx/Content/Textures/VFX/vfx_noise_volume_64.dds";

Result Invalid(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}
} // namespace

Result D3D12Renderer::Impl::UploadVfxNoiseVolume(std::vector<AllocationResource> &uploads)
{
    std::filesystem::path path;
    if (auto resolved = config.ResolveProduct(std::string(kNoiseVolumePackage), path); !resolved)
        return resolved;
    std::vector<std::byte> storage;
    if (auto read = ReadBinary(path, storage); !read) return read;

    render_data::VolumeTexturePackageView package;
    try
    {
        package = render_data::ReadVolumeTexturePackage(
            storage, MakeAssetId(std::string(kNoiseVolumePackage)));
    }
    catch (const std::exception &error)
    {
        return Invalid("Invalid VFX volume texture package " + path.string() + ": " +
                       error.what());
    }
    const auto &description = package.description;
    if (description.width != 64 || description.height != 64 || description.depth != 64 ||
        description.mip_count != 1 || description.format != render_data::kTextureFormatRgba8 ||
        package.dependencies != std::vector<std::string>{std::string(kNoiseVolumeSource)} ||
        package.subresources.size() != 1)
        return Invalid("VFX volume texture package shape or dependency changed: " + path.string());

    const auto &source = package.subresources.front();
    if (source.mip_level != 0 || source.width != 64 || source.height != 64 || source.depth != 64 ||
        source.row_pitch != 64u * 4u || source.slice_pitch != 64u * 64u * 4u ||
        source.volume_pitch != 64u * 64u * 64u * 4u ||
        source.volume_pitch != source.slice_pitch * source.depth ||
        source.bytes.size() != source.volume_pitch)
        return Invalid("VFX volume texture package pitches changed: " + path.string());

    D3D12_RESOURCE_DESC description_native{};
    description_native.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    description_native.Width = description.width;
    description_native.Height = description.height;
    description_native.DepthOrArraySize = static_cast<UINT16>(description.depth);
    description_native.MipLevels = static_cast<UINT16>(description.mip_count);
    description_native.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description_native.SampleDesc = {1, 0};
    D3D12MA::ALLOCATION_DESC default_allocation{};
    default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    AllocationResource volume;
    if (auto created = CreateAllocation(volume, default_allocation, description_native,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
        !created)
        return created;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{};
    UINT64 upload_size{};
    device->GetCopyableFootprints(&description_native, 0, 1, 0, &footprint, &rows,
                                  &row_size, &upload_size);
    if (footprint.Footprint.Width != source.width ||
        footprint.Footprint.Height != source.height ||
        footprint.Footprint.Depth != source.depth || rows != source.height ||
        row_size != source.row_pitch || upload_size < source.volume_pitch ||
        footprint.Offset > upload_size ||
        source.depth > 0 &&
            static_cast<UINT64>(source.depth - 1) * footprint.Footprint.RowPitch *
                    footprint.Footprint.Height +
                    static_cast<UINT64>(source.height - 1) * footprint.Footprint.RowPitch +
                    source.row_pitch >
                upload_size - footprint.Offset)
        return Invalid("VFX volume texture package does not match the native copy footprint: " +
                       path.string());

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
    auto result = uploads.back().resource->Map(0, &no_read,
                                                reinterpret_cast<void **>(&mapped));
    if (FAILED(result)) return HResultFailure("Map VFX volume texture upload", result);
    for (UINT z = 0; z < source.depth; ++z)
        for (UINT y = 0; y < source.height; ++y)
        {
            const auto source_offset = static_cast<std::size_t>(z) * source.slice_pitch +
                static_cast<std::size_t>(y) * source.row_pitch;
            const auto destination_offset = footprint.Offset +
                static_cast<std::size_t>(z) * footprint.Footprint.RowPitch *
                    footprint.Footprint.Height +
                static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
            std::memcpy(mapped + destination_offset, source.bytes.data() + source_offset,
                        static_cast<std::size_t>(row_size));
        }
    uploads.back().resource->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = volume.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = kAllShaderRead;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list->ResourceBarrier(1, &barrier);
    vfx_noise_volume = std::move(volume);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    view.Texture3D.MipLevels = 1;
    for (std::size_t slot = 0; slot < kFrameCount; ++slot)
    {
        auto target = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        target.ptr += (slot * kTextureDescriptorCount + kEntitySurfaceNoiseDescriptorOffset) * srv_stride;
        device->CreateShaderResourceView(vfx_noise_volume.resource.Get(), &view, target);
    }
    return Result::Success();
}
} // namespace hs
