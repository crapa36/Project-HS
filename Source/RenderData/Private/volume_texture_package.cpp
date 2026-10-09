#include <hs/render_data/volume_texture_package.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hs::render_data
{
namespace
{
constexpr std::uint32_t kMetadataMagic = 0x31565854; // TXV1
constexpr std::uint32_t kMetadataVersion = 1;
constexpr std::size_t kMetadataHeader = 40;
constexpr std::size_t kRecordStride = 20;
constexpr std::uint64_t kMaxPayload = 64ull * 1024 * 1024;
constexpr std::uint32_t kMaxMips = 15;

void Require(bool ok, std::string_view message)
{
    if (!ok) throw std::runtime_error("volume texture package: " + std::string(message));
}

void Put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value,
         std::size_t size)
{
    Require(offset <= bytes.size() && size <= bytes.size() - offset, "write bounds");
    for (std::size_t i = 0; i < size; ++i)
        bytes[offset + i] = std::byte((value >> (8 * i)) & 0xff);
}

std::uint64_t Get(std::span<const std::byte> bytes, std::size_t offset, std::size_t size)
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
    std::uint32_t volume_pitch{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t depth{};
    std::uint32_t mip_level{};
};

std::vector<LayoutRecord> Layout(VolumeTextureDescription description)
{
    Require(description.format == kTextureFormatRgba8 && description.width > 0 &&
                description.width <= 16384 && description.height > 0 &&
                description.height <= 16384 && description.depth > 0 &&
                description.depth <= 2048 && description.mip_count > 0 &&
                description.mip_count <= kMaxMips,
            "invalid volume texture description");
    std::uint32_t full_mips = 1;
    for (auto edge = std::max({description.width, description.height, description.depth});
         edge > 1; edge >>= 1)
        ++full_mips;
    Require(description.mip_count <= full_mips, "invalid mip count");

    std::vector<LayoutRecord> records;
    records.reserve(description.mip_count);
    std::uint64_t offset = 0;
    for (std::uint32_t mip = 0; mip < description.mip_count; ++mip)
    {
        const auto width = std::max(1u, description.width >> mip);
        const auto height = std::max(1u, description.height >> mip);
        const auto depth = std::max(1u, description.depth >> mip);
        const auto row = std::uint64_t(width) * 4;
        const auto slice = row * height;
        const auto volume = slice * depth;
        Require(row <= UINT32_MAX && slice <= UINT32_MAX && volume <= UINT32_MAX &&
                    offset <= kMaxPayload && volume <= kMaxPayload - offset,
                "volume texture payload exceeds package bounds");
        records.push_back({offset, static_cast<std::uint32_t>(row),
                           static_cast<std::uint32_t>(slice),
                           static_cast<std::uint32_t>(volume), width, height, depth, mip});
        offset += volume;
    }
    return records;
}
} // namespace

std::vector<std::byte> BuildVolumeTexturePackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    VolumeTextureDescription description, std::span<const std::byte> payload)
{
    const auto layout = Layout(description);
    Require(!layout.empty() && layout.back().offset + layout.back().volume_pitch == payload.size(),
            "volume texture payload size mismatch");

    std::vector<std::byte> metadata(kMetadataHeader + layout.size() * kRecordStride);
    Put(metadata, 0, kMetadataMagic, 4);
    Put(metadata, 4, kMetadataVersion, 4);
    Put(metadata, 8, description.width, 4);
    Put(metadata, 12, description.height, 4);
    Put(metadata, 16, description.depth, 4);
    Put(metadata, 20, description.format, 4);
    Put(metadata, 24, description.mip_count, 4);
    Put(metadata, 28, layout.size(), 4);
    Put(metadata, 32, payload.size(), 8);
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto entry = kMetadataHeader + i * kRecordStride;
        Put(metadata, entry, layout[i].offset, 8);
        Put(metadata, entry + 8, layout[i].row_pitch, 4);
        Put(metadata, entry + 12, layout[i].slice_pitch, 4);
        Put(metadata, entry + 16, layout[i].volume_pitch, 4);
    }
    const auto stride = payload.size() % 16 == 0 ? 16u :
        payload.size() % 8 == 0 ? 8u : 1u;
    const std::array chunks{
        ChunkSource{static_cast<std::uint32_t>(ChunkType::TextureMetadata), 1, true,
                    metadata, 1, static_cast<std::uint32_t>(metadata.size())},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::TextureSubresources), 1, true,
                    payload, payload.size() / stride, stride}};
    return BuildPackage(asset_id, dependencies, chunks);
}

VolumeTexturePackageView ReadVolumeTexturePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id)
{
    const auto package = ReadPackage(package_bytes, expected_asset_id);
    Require(package.chunks.size() == 2 &&
                package.chunks[0].type == ChunkType::TextureMetadata &&
                package.chunks[1].type == ChunkType::TextureSubresources &&
                package.chunks[0].required && package.chunks[1].required,
            "expected required texture metadata and subresource chunks");
    const auto metadata = package.chunks[0].bytes;
    const auto payload = package.chunks[1].bytes;
    Require(metadata.size() >= kMetadataHeader && Get(metadata, 0, 4) == kMetadataMagic &&
                Get(metadata, 4, 4) == kMetadataVersion,
            "invalid volume texture metadata header");

    VolumeTexturePackageView view;
    view.description.width = static_cast<std::uint32_t>(Get(metadata, 8, 4));
    view.description.height = static_cast<std::uint32_t>(Get(metadata, 12, 4));
    view.description.depth = static_cast<std::uint32_t>(Get(metadata, 16, 4));
    view.description.format = static_cast<std::uint32_t>(Get(metadata, 20, 4));
    view.description.mip_count = static_cast<std::uint32_t>(Get(metadata, 24, 4));
    const auto layout = Layout(view.description);
    Require(Get(metadata, 28, 4) == layout.size() &&
                metadata.size() == kMetadataHeader + layout.size() * kRecordStride &&
                Get(metadata, 32, 8) == payload.size() &&
                layout.back().offset + layout.back().volume_pitch == payload.size(),
            "volume texture metadata layout mismatch");
    view.dependencies = package.dependencies;
    view.subresources.reserve(layout.size());
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto entry = kMetadataHeader + i * kRecordStride;
        const auto &record = layout[i];
        Require(Get(metadata, entry, 8) == record.offset &&
                    Get(metadata, entry + 8, 4) == record.row_pitch &&
                    Get(metadata, entry + 12, 4) == record.slice_pitch &&
                    Get(metadata, entry + 16, 4) == record.volume_pitch,
                "volume texture pitch or offset mismatch");
        view.subresources.push_back({record.mip_level, record.width, record.height,
                                     record.depth, record.row_pitch, record.slice_pitch,
                                     record.volume_pitch,
                                     payload.subspan(static_cast<std::size_t>(record.offset),
                                                     record.volume_pitch)});
    }
    return view;
}
} // namespace hs::render_data
