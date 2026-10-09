#include <hs/render_data/lighting_probe.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
namespace
{
constexpr std::uint32_t kMagic = 0x3142504c; // LPB1.
constexpr std::uint32_t kRec709Linear = 1, kSeparateSun = 1, kCosineSh9 = 1;
void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("lighting probe: " + std::string(message));
}
void Put(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value, std::size_t count)
{
    Require(offset <= bytes.size() && count <= bytes.size() - offset, "write bounds");
    for (std::size_t i = 0; i < count; ++i) bytes[offset + i] = std::byte((value >> (8 * i)) & 255);
}
std::uint64_t Get(std::span<const std::byte> bytes, std::size_t offset, std::size_t count)
{
    Require(offset <= bytes.size() && count <= bytes.size() - offset, "read bounds");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i)
        value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (8 * i);
    return value;
}
void PutFloat(std::span<std::byte> bytes, std::size_t offset, float value)
{
    Put(bytes, offset, std::bit_cast<std::uint32_t>(value), 4);
}
float GetFloat(std::span<const std::byte> bytes, std::size_t offset)
{
    return std::bit_cast<float>(static_cast<std::uint32_t>(Get(bytes, offset, 4)));
}
std::uint32_t MipCount(LightingProbeDescription description)
{
    Require(description.edge >= 16 && description.edge <= 256 &&
            std::has_single_bit(description.edge), "invalid cube edge");
    Require(std::isfinite(description.orientation_y_radians) &&
            std::abs(description.orientation_y_radians) <= std::numbers::pi_v<float>, "invalid orientation");
    return std::bit_width(description.edge);
}
std::uint64_t TexelCount(std::uint32_t edge, std::uint32_t mips)
{
    std::uint64_t count = 0;
    for (std::uint32_t mip = 0; mip < mips; ++mip)
    {
        const auto extent = std::max(1u, edge >> mip);
        count += std::uint64_t(extent) * extent * 6;
    }
    return count;
}
void ValidateTexel(Float4 value, bool sh)
{
    Require(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
            std::isfinite(value.w), "nonfinite radiance or irradiance");
    Require(sh ? value.w == 0.0f : value.w == 1.0f && value.x >= 0 && value.y >= 0 && value.z >= 0,
            "invalid radiance alpha/negative radiance or SH reserved component");
}
void EncodeTexel(std::span<std::byte> bytes, std::size_t offset, Float4 value, bool sh)
{
    ValidateTexel(value, sh);
    PutFloat(bytes, offset, value.x); PutFloat(bytes, offset + 4, value.y);
    PutFloat(bytes, offset + 8, value.z); PutFloat(bytes, offset + 12, value.w);
}
Float4 DecodeTexel(std::span<const std::byte> bytes, std::size_t offset, bool sh)
{
    Float4 result{GetFloat(bytes, offset), GetFloat(bytes, offset + 4),
                  GetFloat(bytes, offset + 8), GetFloat(bytes, offset + 12)};
    ValidateTexel(result, sh);
    return result;
}
TexturePackageView CubeView(std::span<const std::byte> payload, std::uint32_t edge,
                            std::uint32_t mips, const std::vector<std::string> &dependencies)
{
    Require(payload.size() == TexelCount(edge, mips) * 16, "cube payload size mismatch");
    TexturePackageView result;
    result.description = {edge, edge, 6, mips, kTextureFormatRgba32Float};
    result.dependencies = dependencies;
    std::size_t offset = 0;
    for (std::uint32_t face = 0; face < 6; ++face)
        for (std::uint32_t mip = 0; mip < mips; ++mip)
        {
            const auto extent = std::max(1u, edge >> mip), row = extent * 16, slice = extent * row;
            result.subresources.push_back({face, mip, extent, extent, row, slice, payload.subspan(offset, slice)});
            for (std::size_t texel = 0; texel < slice; texel += 16)
                (void)DecodeTexel(payload, offset + texel, false);
            offset += slice;
        }
    return result;
}
} // namespace

std::vector<std::byte> BuildLightingProbePackage(AssetId asset_id,
    std::span<const std::string> dependencies, LightingProbeDescription description,
    std::span<const Float4> radiance, std::span<const Float4> prefiltered,
    const LightingProbeIrradiance &irradiance)
{
    const auto mips = MipCount(description);
    Require(radiance.size() == TexelCount(description.edge, 1) &&
            prefiltered.size() == TexelCount(description.edge, mips), "cube texel count mismatch");
    std::vector<std::byte> metadata(kLightingProbeMetadataStride), payload((radiance.size() + prefiltered.size()) * 16), sh(144);
    Put(metadata, 0, kMagic, 4); Put(metadata, 4, kLightingProbeVersion, 4);
    Put(metadata, 8, description.edge, 4); Put(metadata, 12, 1, 4); Put(metadata, 16, mips, 4);
    Put(metadata, 20, kTextureFormatRgba32Float, 4); Put(metadata, 24, kRec709Linear, 4);
    Put(metadata, 28, kSeparateSun, 4); PutFloat(metadata, 32, description.orientation_y_radians);
    Put(metadata, 40, radiance.size() * 16, 8); Put(metadata, 48, prefiltered.size() * 16, 8);
    Put(metadata, 56, kCosineSh9, 4);
    std::size_t offset = 0;
    for (const auto texel : radiance) { EncodeTexel(payload, offset, texel, false); offset += 16; }
    for (const auto texel : prefiltered) { EncodeTexel(payload, offset, texel, false); offset += 16; }
    for (std::size_t i = 0; i < irradiance.size(); ++i) EncodeTexel(sh, i * 16, irradiance[i], true);
    const std::array chunks{
        ChunkSource{static_cast<std::uint32_t>(ChunkType::LightingProbeMetadata), 1, true, metadata, 1, kLightingProbeMetadataStride},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::LightingProbeRadiance), 1, true, payload, payload.size() / 16, 16},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::LightingProbeIrradiance), 1, true, sh, 9, 16}};
    return BuildPackage(asset_id, dependencies, chunks);
}

LightingProbePackageView ReadLightingProbePackage(std::span<const std::byte> bytes, AssetId expected_asset_id)
{
    const auto package = ReadPackage(bytes, expected_asset_id);
    Require(Get(bytes, 64, 4) == 3 && package.chunks.size() == 3 && package.chunks[0].type == ChunkType::LightingProbeMetadata &&
            package.chunks[1].type == ChunkType::LightingProbeRadiance &&
            package.chunks[2].type == ChunkType::LightingProbeIrradiance &&
            package.chunks[0].required && package.chunks[1].required && package.chunks[2].required,
            "expected three required lighting probe chunks");
    const auto metadata = package.chunks[0].bytes;
    Require(metadata.size() == kLightingProbeMetadataStride && Get(metadata, 0, 4) == kMagic &&
            Get(metadata, 4, 4) == kLightingProbeVersion && Get(metadata, 12, 4) == 1 &&
            Get(metadata, 20, 4) == kTextureFormatRgba32Float && Get(metadata, 24, 4) == kRec709Linear &&
            Get(metadata, 28, 4) == kSeparateSun && Get(metadata, 36, 4) == 0 &&
            Get(metadata, 56, 4) == kCosineSh9 && Get(metadata, 60, 4) == 0, "unsupported probe metadata/conventions");
    LightingProbePackageView result;
    result.description = {static_cast<std::uint32_t>(Get(metadata, 8, 4)), GetFloat(metadata, 32)};
    const auto mips = MipCount(result.description);
    const auto rawBytes = TexelCount(result.description.edge, 1) * 16;
    const auto filteredBytes = TexelCount(result.description.edge, mips) * 16;
    const auto payload = package.chunks[1].bytes;
    Require(Get(metadata, 16, 4) == mips && Get(metadata, 40, 8) == rawBytes &&
            Get(metadata, 48, 8) == filteredBytes && payload.size() == rawBytes + filteredBytes,
            "probe payload layout mismatch");
    result.radiance = CubeView(payload.first(static_cast<std::size_t>(rawBytes)), result.description.edge, 1, package.dependencies);
    result.prefiltered = CubeView(payload.subspan(static_cast<std::size_t>(rawBytes)), result.description.edge, mips, package.dependencies);
    for (std::size_t i = 0; i < result.irradiance.size(); ++i)
        result.irradiance[i] = DecodeTexel(package.chunks[2].bytes, i * 16, true);
    return result;
}
} // namespace hs::render_data
