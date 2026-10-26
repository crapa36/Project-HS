#pragma once

#include <hs/core/types.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
struct VfxMeshDescriptor
{
    std::uint32_t vertex_count{};
    float transverse_radius{}, minimum_z{}, widest_z{}, maximum_z{};
};
struct VfxMeshAtlasDescriptor
{
    AssetId asset_id;
    std::array<VfxMeshDescriptor, 7> meshes;
};

// Keep the HLSL-visible 25-word header and vertex layout unchanged.
inline VfxMeshAtlasDescriptor CompileVfxMeshAtlasDescriptor(std::span<const std::byte> bytes, AssetId asset)
{
    constexpr std::array<std::string_view, 7> names{
        "arrowhead_mesh", "shard_mesh", "debris_shard_mesh", "small_shard_mesh",
        "enemy_thorn_mesh", "boss_crest_lance_mesh", "needle_shard_mesh"};
    static_assert(std::endian::native == std::endian::little && sizeof(float) == 4);
    if (!asset.value || bytes.size() < 100 || bytes.size() > 64ull * 1024 * 1024 || bytes.size() % 4)
        throw std::runtime_error("Invalid VFX mesh atlas extent");
    const auto word = [&](std::size_t index) {
        std::uint32_t value{};
        std::memcpy(&value, bytes.data() + index * 4, sizeof(value));
        return value;
    };
    const auto vertex_count = word(3);
    if (word(0) != 0x4d565348u || word(1) != 1 || word(2) != names.size() ||
        vertex_count > (bytes.size() / 4 - 25) / 12 || bytes.size() / 4 != 25ull + 12ull * vertex_count)
        throw std::runtime_error("Invalid VFX mesh atlas header");
    VfxMeshAtlasDescriptor result{asset};
    std::uint32_t next_vertex{};
    for (std::size_t mesh = 0; mesh < names.size(); ++mesh)
    {
        std::uint32_t expected_hash = 2166136261u;
        for (const auto character : names[mesh])
            expected_hash = (expected_hash ^ static_cast<unsigned char>(character)) * 16777619u;
        const auto first = word(5 + mesh * 3);
        const auto count = word(6 + mesh * 3);
        if (word(4 + mesh * 3) != expected_hash || first != next_vertex || !count || count % 3 ||
            first > vertex_count || count > vertex_count - first)
            throw std::runtime_error("Invalid VFX mesh atlas table");
        auto &entry = result.meshes[mesh];
        entry.vertex_count = count;
        entry.minimum_z = std::numeric_limits<float>::infinity();
        entry.maximum_z = -entry.minimum_z;
        for (std::uint32_t vertex = first; vertex < first + count; ++vertex)
        {
            for (std::size_t component = 0; component < 12; ++component)
                if (!std::isfinite(std::bit_cast<float>(word(25 + vertex * 12 + component))))
                    throw std::runtime_error("Invalid VFX mesh atlas vertex");
            const float x = std::bit_cast<float>(word(25 + vertex * 12));
            const float y = std::bit_cast<float>(word(26 + vertex * 12));
            const float z = std::bit_cast<float>(word(27 + vertex * 12));
            entry.minimum_z = std::min(entry.minimum_z, z);
            entry.maximum_z = std::max(entry.maximum_z, z);
            const auto radius = std::hypot(x, y);
            if (radius > entry.transverse_radius) { entry.transverse_radius = radius; entry.widest_z = z; }
        }
        if (!std::isfinite(entry.transverse_radius) || entry.transverse_radius <= 0 ||
            entry.minimum_z >= entry.maximum_z)
            throw std::runtime_error("Invalid VFX mesh atlas bounds");
        next_vertex += count;
    }
    if (next_vertex != vertex_count) throw std::runtime_error("Incomplete VFX mesh atlas table");
    return result;
}
} // namespace hs::render_data
