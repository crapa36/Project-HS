#pragma once

#include <cstdint>
#include <string_view>

namespace hs::render_data
{
// GPU ABI 2 stores material payload in bits 0-23, its original namespace in
// bits 24-27, and the emitted flag tuple in bits 28-30; bit 31 is reserved.
struct DecodedSurfaceMetadata
{
    std::uint32_t material;
    std::uint32_t flags;
};

[[nodiscard]] constexpr DecodedSurfaceMetadata DecodeSurfaceMetadata(std::uint32_t packed) noexcept
{
    std::uint32_t flags{};
    switch ((packed >> 28u) & 0x7u)
    {
    case 1u: flags = 3u; break;
    case 2u: flags = 65539u; break;
    case 3u: flags = 7u; break;
    case 4u: flags = 5u; break;
    case 5u: flags = 1285u; break;
    case 6u: flags = 1541u; break;
    default: break;
    }
    const auto material = (packed & 0x00ffffffu) | ((packed & 0x0f000000u) << 4u);
    return {material, flags};
}

// Surface ABI 5 also pairs compact VSM casters/group metadata with indexed instancing.
// Logical material identity is distinct from a texture-array/atlas entry.
struct SurfaceLayout
{
    static constexpr std::uint32_t version = 5;
    static constexpr std::string_view abi_product = "render/abi/surface_layout.abi";
    static constexpr std::uint32_t render_target_count = 6;
    static constexpr std::uint32_t invalid_material = 0;
    static constexpr std::uint32_t valid_flag = 1u;
    static constexpr std::uint32_t model_shift = 1u;
    static constexpr std::uint32_t model_mask = 0xfeu;
    static constexpr std::uint32_t outline_shift = 16u;
};
} // namespace hs::render_data
