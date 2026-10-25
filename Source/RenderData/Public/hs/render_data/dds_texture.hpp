#pragma once

#include <hs/render_data/texture_package.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hs::render_data
{
// A checked byte view, not a file loader or a second runtime asset format. All
// spans borrow the caller's immutable bytes; keep their owner alive through use.
struct DdsSubresourceView
{
    std::uint32_t array_slice{}, mip_level{}, width{}, height{}, depth{};
    std::uint32_t row_pitch{}, slice_pitch{};
    std::span<const std::byte> bytes;
};
struct DdsTextureView
{
    TextureDescription description;
    std::uint32_t flags{}, pitch{}, depth{}, pixel_flags{}, four_cc{}, rgba_bits{};
    std::array<std::uint32_t, 4> masks{};
    std::array<std::uint32_t, 5> caps{};
    std::array<std::uint32_t, 11> reserved{};
    std::uint32_t dimension{3}, misc{}, misc2{}, payload_offset{128};
    std::span<const std::byte> payload;
    std::vector<DdsSubresourceView> subresources;
};
enum class DdsTextureRole { CharacterLegacy, CharacterFinalBc7, Environment, VfxTexture, VfxVolume };

namespace dds_detail
{
inline void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("DDS: " + std::string(message));
}
inline std::uint32_t U32(std::span<const std::byte> bytes, std::size_t offset)
{
    Require(offset <= bytes.size() && 4 <= bytes.size() - offset, "truncated little-endian field");
    std::uint32_t result{};
    for (std::uint32_t byte = 0; byte < 4; ++byte)
        result |= std::uint32_t(std::to_integer<unsigned>(bytes[offset + byte])) << (byte * 8);
    return result;
}
inline std::uint32_t UnitBytes(std::uint32_t format)
{
    switch (format)
    {
    case kTextureFormatR8: return 1;
    case kTextureFormatRg8Snorm: case kTextureFormatR16Float: return 2;
    case kTextureFormatRgba8: case kTextureFormatRgba8Srgb: return 4;
    case kTextureFormatBc4: case kTextureFormatRgba16Float: return 8;
    case kTextureFormatBc5: case 84: case kTextureFormatBc7:
    case kTextureFormatBc7Srgb: case kTextureFormatRgba32Float: return 16;
    default: throw std::runtime_error("DDS: unsupported format");
    }
}
inline bool Block(std::uint32_t format)
{
    return format == kTextureFormatBc4 || format == kTextureFormatBc5 || format == 84 ||
        format == kTextureFormatBc7 || format == kTextureFormatBc7Srgb;
}
} // namespace dds_detail

[[nodiscard]] inline DdsTextureView DecodeDdsTexture(std::span<const std::byte> bytes)
{
    using namespace dds_detail;
    constexpr std::uint64_t max_bytes = 512ull * 1024 * 1024;
    Require(bytes.size() >= 128 && bytes.size() <= max_bytes, "input byte ceiling or truncated header");
    const auto word = [&](std::size_t offset) { return U32(bytes, offset); };
    Require(word(0) == 0x20534444 && word(4) == 124 && word(76) == 32, "magic/header size");
    DdsTextureView result;
    auto &shape = result.description;
    result.flags = word(8); shape.height = word(12); shape.width = word(16);
    result.pitch = word(20); result.depth = word(24); shape.mip_count = word(28);
    result.pixel_flags = word(80); result.four_cc = word(84); result.rgba_bits = word(88);
    for (std::size_t i = 0; i < result.reserved.size(); ++i) result.reserved[i] = word(32 + i * 4);
    for (std::size_t i = 0; i < result.masks.size(); ++i) result.masks[i] = word(92 + i * 4);
    for (std::size_t i = 0; i < result.caps.size(); ++i) result.caps[i] = word(108 + i * 4);
    if (result.pixel_flags == 4 && result.four_cc == 0x30315844)
    {
        Require(bytes.size() >= 148, "truncated DX10 header");
        shape.format = word(128); result.dimension = word(132); result.misc = word(136);
        shape.array_size = word(140); result.misc2 = word(144); result.payload_offset = 148;
    }
    else
    {
        Require(result.pixel_flags == 65 && result.four_cc == 0 && result.rgba_bits == 32 &&
            result.masks == std::array<std::uint32_t, 4>{0xff, 0xff00, 0xff0000, 0xff000000},
            "unsupported legacy pixel format");
        shape.format = kTextureFormatRgba8; shape.array_size = 1;
    }
    Require((result.dimension == 3 || result.dimension == 4) && shape.width && shape.width <= 16384 &&
        shape.height && shape.height <= 16384 && shape.array_size && shape.array_size <= 2048 &&
        (result.dimension != 4 || (shape.array_size == 1 && result.depth && result.depth <= 2048)),
        "resource dimension/extent");
    const auto depth = result.dimension == 4 ? result.depth : 1u;
    std::uint32_t full_mips = 1;
    for (auto edge = std::max({shape.width, shape.height, depth}); edge > 1; edge >>= 1) ++full_mips;
    Require(shape.mip_count && shape.mip_count <= full_mips &&
        std::uint64_t(shape.array_size) * shape.mip_count <= 4096, "mip/subresource ceiling");
    const auto unit = UnitBytes(shape.format);
    const bool block = Block(shape.format);
    std::size_t offset = result.payload_offset;
    result.subresources.reserve(std::size_t(shape.array_size) * shape.mip_count);
    for (std::uint32_t slice = 0; slice < shape.array_size; ++slice)
        for (std::uint32_t mip = 0; mip < shape.mip_count; ++mip)
        {
            const auto width = std::max(1u, shape.width >> mip), height = std::max(1u, shape.height >> mip);
            const auto z = std::max(1u, depth >> mip);
            const std::uint64_t row = std::uint64_t(block ? (width + 3) / 4 : width) * unit;
            const auto plane = row * (block ? (height + 3) / 4 : height);
            const auto count = plane * z;
            Require(row <= UINT32_MAX && plane <= UINT32_MAX && offset <= bytes.size() &&
                count <= bytes.size() - offset, "subresource payload range");
            result.subresources.push_back({slice, mip, width, height, z, static_cast<std::uint32_t>(row),
                static_cast<std::uint32_t>(plane), bytes.subspan(offset, static_cast<std::size_t>(count))});
            offset += static_cast<std::size_t>(count);
        }
    Require(offset == bytes.size(), "trailing payload bytes");
    result.payload = bytes.subspan(result.payload_offset);
    return result;
}

// Role rules are separate from byte decoding. A legacy import proof cannot
// stand in for a final BC7 proof, even for an identical width and mip count.
inline void ValidateDdsTextureRole(const DdsTextureView &dds, DdsTextureRole role, TextureDescription expected)
{
    using namespace dds_detail;
    const auto &s = dds.description;
    Require(!dds.subresources.empty(), "role requires a decoded subresource layout");
    Require(std::ranges::all_of(dds.reserved, [](auto value) { return value == 0; }), "reserved header");
    if (role == DdsTextureRole::CharacterLegacy)
    {
        Require(dds.payload_offset == 128 && s.format == kTextureFormatRgba8 && s.array_size == 1 &&
            s.width == expected.width && s.height == expected.height && s.mip_count == expected.mip_count &&
            expected.array_size == 1 && (expected.format == kTextureFormatRgba8 || expected.format == kTextureFormatRgba8Srgb),
            "character legacy RGBA8 shape");
        Require(dds.flags == (s.mip_count > 1 ? 0x2100fu : 0x100fu) &&
            dds.pitch == std::uint64_t(s.width) * 4 && dds.depth == 0 &&
            dds.caps == std::array<std::uint32_t, 5>{s.mip_count > 1 ? 0x401008u : 0x1000u, 0, 0, 0, 0},
            "character legacy flags/pitch/caps");
        return;
    }
    Require(dds.payload_offset == 148 && dds.misc == 0 && dds.misc2 == 0 && dds.rgba_bits == 0 &&
        std::ranges::all_of(dds.masks, [](auto value) { return value == 0; }), "DX10 reserved fields");
    Require(s.width == expected.width && (!expected.height || s.height == expected.height) &&
        (!expected.array_size || s.array_size == expected.array_size) && s.mip_count == expected.mip_count &&
        s.format == expected.format, "role shape/format");
    const bool block = Block(s.format);
    const auto top_pitch = block ? dds.subresources.front().slice_pitch : dds.subresources.front().row_pitch;
    Require(dds.pitch == top_pitch, "top-level pitch/linear size");
    if (role == DdsTextureRole::VfxVolume)
    {
        Require(expected.width == 64 && expected.height == 64 && expected.array_size == 1 && expected.mip_count == 1 &&
            expected.format == kTextureFormatRgba8 && dds.dimension == 4 && s.width == 64 && s.height == 64 && dds.depth == 64 && s.array_size == 1 &&
            s.mip_count == 1 && s.format == kTextureFormatRgba8 && dds.flags == 0x0080100f &&
            dds.caps == std::array<std::uint32_t, 5>{0x1008, 0x200000, 0, 0, 0}, "VFX volume shape/flags/caps");
        return;
    }
    Require(dds.dimension == 3 && std::ranges::all_of(std::span(dds.caps).subspan(1), [](auto value) { return value == 0; }),
        "2D dimension/caps");
    if (role == DdsTextureRole::VfxTexture)
    {
        Require(s.format == kTextureFormatR8 || s.format == kTextureFormatRg8Snorm || s.format == kTextureFormatR16Float ||
            s.format == kTextureFormatRgba8 || s.format == kTextureFormatRgba16Float || s.format == kTextureFormatBc4,
            "VFX format");
        Require(dds.depth <= 1 && dds.flags == (block ? 0xa1007u : s.mip_count > 1 ? 0x2100fu : 0x100fu) &&
            dds.caps[0] == (s.mip_count > 1 ? 0x401008u : s.array_size > 1 ? 0x1008u : 0x1000u), "VFX flags/caps");
        return;
    }
    const auto flags = 0x1007u | (block ? 0x80000u : 8u) | (s.mip_count > 1 ? 0x20000u : 0u);
    const auto caps = 0x1000u | (s.mip_count > 1 ? 0x400008u : 0u) |
        (role == DdsTextureRole::CharacterFinalBc7 && s.array_size > 1 ? 8u : 0u);
    Require((dds.flags & flags) == flags && (dds.caps[0] & caps) == caps, "block flags/caps");
    if (role == DdsTextureRole::CharacterFinalBc7)
        Require(expected.height && expected.array_size && dds.depth <= 1 &&
            (s.format == kTextureFormatBc7 || s.format == kTextureFormatBc7Srgb), "character final BC7 contract");
    else
        Require(role == DdsTextureRole::Environment && expected.height && expected.array_size && dds.depth == 1 &&
            (s.format == kTextureFormatBc4 || s.format == kTextureFormatBc5 || s.format == kTextureFormatBc7 ||
             s.format == kTextureFormatBc7Srgb || s.format == kTextureFormatRgba32Float) &&
            (block || (dds.flags & 0x80000u) == 0), "environment format/depth");
}
} // namespace hs::render_data
