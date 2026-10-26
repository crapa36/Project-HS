#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
namespace
{
constexpr std::uint32_t kMetadataMagic = 0x314d5854; // TXM1
constexpr std::uint32_t kMetadataVersion = 1;
constexpr std::size_t kMetadataHeader = 40;
constexpr std::size_t kRecordStride = 16;
constexpr std::uint64_t kMaxPayload = 32ull * 1024 * 1024;
constexpr std::uint32_t kMaxSubresources = 4096;

void Require(bool ok, std::string_view message)
{
    if (!ok) throw std::runtime_error("texture package: " + std::string(message));
}

void Put(std::span<std::byte> bytes, std::size_t offset,
         std::uint64_t value, std::size_t size)
{
    Require(offset <= bytes.size() && size <= bytes.size() - offset, "write bounds");
    for (std::size_t i = 0; i < size; ++i)
        bytes[offset + i] = std::byte((value >> (8 * i)) & 0xff);
}

std::uint64_t Get(std::span<const std::byte> bytes, std::size_t offset,
                  std::size_t size)
{
    Require(offset <= bytes.size() && size <= bytes.size() - offset, "read bounds");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
        value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    return value;
}

struct LayoutRecord
{
    std::uint64_t offset{};
    std::uint32_t row_pitch{};
    std::uint32_t slice_pitch{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t array_slice{};
    std::uint32_t mip_level{};
};

std::vector<LayoutRecord> Layout(TextureDescription description)
{
    const bool bc4 = description.format == kTextureFormatBc4;
    const auto bytes_per_pixel = description.format == kTextureFormatRgba32Float ? 16u :
        description.format == kTextureFormatR8 ? 1u :
        description.format == kTextureFormatRg8Snorm ||
        description.format == kTextureFormatR16Float ? 2u :
        description.format == kTextureFormatRgba8 ||
        description.format == kTextureFormatRgba8Srgb ? 4u :
        description.format == kTextureFormatRgba16Float ? 8u : 0u;
    Require((bc4 || description.format == kTextureFormatBc5 ||
             description.format == kTextureFormatBc7 ||
             description.format == kTextureFormatBc7Srgb || bytes_per_pixel) &&
            description.width > 0 && description.width <= 16384 &&
            description.height > 0 && description.height <= 16384 &&
            description.array_size > 0 && description.array_size <= 2048 &&
            description.mip_count > 0 && description.mip_count <= 15,
            "invalid texture description");
    std::uint32_t full_mips = 1;
    for (auto edge = std::max(description.width, description.height); edge > 1; edge >>= 1)
        ++full_mips;
    Require(description.mip_count <= full_mips &&
            std::uint64_t(description.array_size) * description.mip_count <= kMaxSubresources,
            "invalid mip or subresource count");
    std::vector<LayoutRecord> records;
    records.reserve(std::size_t(description.array_size) * description.mip_count);
    std::uint64_t offset = 0;
    for (std::uint32_t slice = 0; slice < description.array_size; ++slice)
        for (std::uint32_t mip = 0; mip < description.mip_count; ++mip)
        {
            const auto width = std::max(1u, description.width >> mip);
            const auto height = std::max(1u, description.height >> mip);
            const auto row = bytes_per_pixel ? std::uint64_t(width) * bytes_per_pixel :
                std::uint64_t((width + 3) / 4) * (bc4 ? 8 : 16);
            const auto size = row * (bytes_per_pixel ? height : (height + 3) / 4);
            Require(row <= UINT32_MAX && size <= UINT32_MAX &&
                    offset <= kMaxPayload && size <= kMaxPayload - offset,
                    "texture payload exceeds package bounds");
            records.push_back({offset, static_cast<std::uint32_t>(row),
                               static_cast<std::uint32_t>(size), width, height, slice, mip});
            offset += size;
        }
    return records;
}
} // namespace

std::vector<std::byte> BuildTexturePackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    TextureDescription description, std::span<const std::byte> payload)
{
    const auto layout = Layout(description);
    Require(!layout.empty() &&
            layout.back().offset + layout.back().slice_pitch == payload.size(),
            "texture payload size mismatch");
    std::vector<std::byte> metadata(kMetadataHeader + layout.size() * kRecordStride);
    Put(metadata, 0, kMetadataMagic, 4);
    Put(metadata, 4, kMetadataVersion, 4);
    Put(metadata, 8, description.width, 4);
    Put(metadata, 12, description.height, 4);
    Put(metadata, 16, description.format, 4);
    Put(metadata, 20, description.array_size, 4);
    Put(metadata, 24, description.mip_count, 4);
    Put(metadata, 28, layout.size(), 4);
    Put(metadata, 32, payload.size(), 8);
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto entry = kMetadataHeader + i * kRecordStride;
        Put(metadata, entry, layout[i].offset, 8);
        Put(metadata, entry + 8, layout[i].row_pitch, 4);
        Put(metadata, entry + 12, layout[i].slice_pitch, 4);
    }
    const auto stride = description.format == kTextureFormatRgba8 ||
                        description.format == kTextureFormatRgba8Srgb ||
                        description.format == kTextureFormatR8 ||
                        description.format == kTextureFormatRg8Snorm ||
                        description.format == kTextureFormatR16Float ||
                        description.format == kTextureFormatRgba16Float ||
                        description.format == kTextureFormatRgba32Float ? 1u :
                        description.format == kTextureFormatBc4 && payload.size() % 16 ?
                            8u : 16u;
    const std::array chunks{
        ChunkSource{static_cast<std::uint32_t>(ChunkType::TextureMetadata), 1, true,
                    metadata, 1, static_cast<std::uint32_t>(metadata.size())},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::TextureSubresources), 1, true,
                    payload, payload.size() / stride, stride}};
    return BuildPackage(asset_id, dependencies, chunks);
}

TexturePackageView ReadTexturePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id)
{
    return ReadTexturePackage(ReadPackage(package_bytes, expected_asset_id));
}

TexturePackageView ReadTexturePackage(const PackageView &package)
{
    Require(package.chunks.size() == 2 &&
            package.chunks[0].type == ChunkType::TextureMetadata &&
            package.chunks[1].type == ChunkType::TextureSubresources &&
            package.chunks[0].required && package.chunks[1].required,
            "expected required texture metadata and subresource chunks");
    const auto metadata = package.chunks[0].bytes;
    const auto payload = package.chunks[1].bytes;
    Require(metadata.size() >= kMetadataHeader &&
            Get(metadata, 0, 4) == kMetadataMagic &&
            Get(metadata, 4, 4) == kMetadataVersion,
            "invalid texture metadata header");
    TexturePackageView view;
    view.description.width = static_cast<std::uint32_t>(Get(metadata, 8, 4));
    view.description.height = static_cast<std::uint32_t>(Get(metadata, 12, 4));
    view.description.format = static_cast<std::uint32_t>(Get(metadata, 16, 4));
    view.description.array_size = static_cast<std::uint32_t>(Get(metadata, 20, 4));
    view.description.mip_count = static_cast<std::uint32_t>(Get(metadata, 24, 4));
    const auto layout = Layout(view.description);
    Require(Get(metadata, 28, 4) == layout.size() &&
            metadata.size() == kMetadataHeader + layout.size() * kRecordStride &&
            Get(metadata, 32, 8) == payload.size() &&
            layout.back().offset + layout.back().slice_pitch == payload.size(),
            "texture metadata layout mismatch");
    view.dependencies = package.dependencies;
    view.subresources.reserve(layout.size());
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto entry = kMetadataHeader + i * kRecordStride;
        const auto &record = layout[i];
        Require(Get(metadata, entry, 8) == record.offset &&
                Get(metadata, entry + 8, 4) == record.row_pitch &&
                Get(metadata, entry + 12, 4) == record.slice_pitch,
                "texture subresource pitch or offset mismatch");
        view.subresources.push_back({record.array_slice, record.mip_level,
                                     record.width, record.height, record.row_pitch,
                                     record.slice_pitch,
                                     payload.subspan(static_cast<std::size_t>(record.offset),
                                                     record.slice_pitch)});
    }
    return view;
}
TextureDescriptorStorage CompileTextureDescriptor(
    std::span<const std::byte> bytes, AssetId asset_id, std::uint32_t bulk_index)
{
    const auto package = ReadPackage(bytes, asset_id);
    Require(!package.chunks.empty() && package.chunks[0].type == ChunkType::TextureMetadata,
            "texture metadata is absent");
    const auto magic = Get(package.chunks[0].bytes, 0, 4);
    TextureDescriptorStorage descriptor;
    descriptor.asset_id = asset_id;
    descriptor.bulk_index = bulk_index;
    const auto retain_layout = [&](const auto &view) {
        descriptor.subresources.reserve(view.subresources.size());
        for (const auto &subresource : view.subresources)
            descriptor.subresources.push_back({static_cast<std::uint64_t>(subresource.bytes.data() - bytes.data()),
                subresource.row_pitch, subresource.slice_pitch, static_cast<std::uint32_t>(subresource.bytes.size())});
    };
    if (magic == 0x31565854) // TXV1
    {
        const auto view = ReadVolumeTexturePackage(package);
        const auto &volume = view.description;
        descriptor.description = {volume.width, volume.height, 1, volume.mip_count, volume.format};
        descriptor.dimension = TextureDimension::Texture3D;
        descriptor.depth = volume.depth;
        retain_layout(view);
    }
    else
    {
        Require(magic == kMetadataMagic, "unknown texture metadata kind");
        const auto view = ReadTexturePackage(package);
        descriptor.description = view.description;
        retain_layout(view);
    }
    descriptor.dependencies.reserve(package.dependencies.size());
    for (const auto &dependency : package.dependencies)
        descriptor.dependencies.push_back(MakeAssetId(dependency));
    return descriptor;
}

TextureDescriptorStorage CompileTextureMetadataDescriptor(const PackageDirectory &directory,
    std::span<const std::byte> metadata, std::uint32_t bulk_index, std::uint64_t product_bytes)
{
    Require(directory.asset_id.value && directory.chunks.size() == 2 &&
        directory.chunks[0].type == ChunkType::TextureMetadata &&
        directory.chunks[1].type == ChunkType::TextureSubresources &&
        directory.chunks[0].required && directory.chunks[1].required &&
        metadata.size() == directory.chunks[0].byte_count && metadata.size() >= kMetadataHeader &&
        Get(metadata, 0, 4) == kMetadataMagic && Get(metadata, 4, 4) == kMetadataVersion,
        "invalid metadata-only texture proof");
    TextureDescriptorStorage descriptor;
    descriptor.asset_id = directory.asset_id;
    descriptor.bulk_index = bulk_index;
    descriptor.description.width = static_cast<std::uint32_t>(Get(metadata, 8, 4));
    descriptor.description.height = static_cast<std::uint32_t>(Get(metadata, 12, 4));
    descriptor.description.format = static_cast<std::uint32_t>(Get(metadata, 16, 4));
    descriptor.description.array_size = static_cast<std::uint32_t>(Get(metadata, 20, 4));
    descriptor.description.mip_count = static_cast<std::uint32_t>(Get(metadata, 24, 4));
    const auto layout = Layout(descriptor.description);
    const auto &payload = directory.chunks[1];
    Require(Get(metadata, 28, 4) == layout.size() &&
        metadata.size() == kMetadataHeader + layout.size() * kRecordStride &&
        Get(metadata, 32, 8) == payload.byte_count &&
        layout.back().offset + layout.back().slice_pitch == payload.byte_count,
        "texture proof layout differs from directory");
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto entry = kMetadataHeader + i * kRecordStride;
        const auto &record = layout[i];
        Require(Get(metadata, entry, 8) == record.offset &&
            Get(metadata, entry + 8, 4) == record.row_pitch &&
            Get(metadata, entry + 12, 4) == record.slice_pitch,
            "texture proof subresource layout mismatch");
        descriptor.subresources.push_back({payload.offset + record.offset,
            record.row_pitch, record.slice_pitch, record.slice_pitch});
    }
    for (const auto &dependency : directory.dependencies)
        descriptor.dependencies.push_back(MakeAssetId(dependency));
    ValidateTextureDescriptor(descriptor.View(), product_bytes);
    return descriptor;
}

void ValidateTextureDescriptor(const TextureDescriptor &descriptor, std::uint64_t product_bytes)
{
    if (descriptor.dimension == TextureDimension::Texture3D)
        return ValidateVolumeTextureDescriptor(descriptor, product_bytes);
    Require(descriptor.dimension == TextureDimension::Texture2DArray && descriptor.depth == 1,
            "invalid 2D texture dimension or depth");
    Require(product_bytes <= std::numeric_limits<std::size_t>::max(), "texture product is not addressable");
    const auto layout = Layout(descriptor.description);
    Require(descriptor.subresources.size() == layout.size(), "texture descriptor subresource count mismatch");
    const auto base = descriptor.subresources.front().offset;
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto &record = descriptor.subresources[i];
        Require(record.offset <= product_bytes && record.byte_count <= product_bytes - record.offset,
                "texture descriptor subresource exceeds product bounds");
        Require(record.offset >= base && record.offset - base == layout[i].offset &&
                record.row_pitch == layout[i].row_pitch && record.slice_pitch == layout[i].slice_pitch &&
                record.byte_count == layout[i].slice_pitch,
                "texture descriptor pitch or offset mismatch");
    }
}

TexturePackageView BindTextureDescriptor(std::span<const std::byte> bytes, const TextureDescriptor &descriptor)
{
    Require(descriptor.dimension == TextureDimension::Texture2DArray, "cannot bind volume as 2D texture");
    TexturePackageView view;
    view.description = descriptor.description;
    // Admission checks semantics; this guard keeps index arithmetic defined even for a bad caller.
    Require(view.description.mip_count > 0 && view.description.mip_count <= 32, "invalid bound texture mip count");
    view.subresources.reserve(descriptor.subresources.size());
    for (std::size_t i = 0; i < descriptor.subresources.size(); ++i)
    {
        const auto &record = descriptor.subresources[i];
        Require(record.offset <= bytes.size() && record.byte_count <= bytes.size() - record.offset,
                "bound texture subresource exceeds bytes");
        const auto mip = static_cast<std::uint32_t>(i % view.description.mip_count);
        view.subresources.push_back({static_cast<std::uint32_t>(i / view.description.mip_count), mip,
            std::max(1u, view.description.width >> mip), std::max(1u, view.description.height >> mip),
            record.row_pitch, record.slice_pitch,
            bytes.subspan(static_cast<std::size_t>(record.offset), record.byte_count)});
    }
    return view;
}
} // namespace hs::render_data
