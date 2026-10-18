#pragma once

#include "renderer_brixelizer_gi.hpp"
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/material_schema.hpp>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace hs
{
// A frame-local geometry identity. The stable key alone is insufficient for
// SkinCache and rewritten buffers, where one mesh key can have distinct byte
// ranges or graph versions in the same frame.
struct BrixelizerGeometryKey
{
    std::uint64_t stable_key{};
    ID3D12Resource *vertices{}, *indices{};
    std::uint64_t generation{}, current_revision{};
    std::uint32_t vertex_resource{}, vertex_kind{}, vertex_ordinal{};
    std::uint32_t index_resource{}, index_kind{}, index_ordinal{};
    std::uint64_t vertex_byte_offset{}, index_byte_offset{};
    std::uint32_t vertex_stride{}, vertex_count{}, index_count{};
    DXGI_FORMAT index_format{};
    bool dynamic{}, opaque{}, force_rebuild{};
    bool operator==(const BrixelizerGeometryKey &) const = default;
};

struct BrixelizerGeometryKeyHash
{
    std::size_t operator()(const BrixelizerGeometryKey &key) const noexcept
    {
        std::size_t hash = std::hash<std::uint64_t>{}(key.stable_key);
        const auto mix = [&](std::uint64_t value) {
            const auto part = std::hash<std::uint64_t>{}(value);
            hash ^= part + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) +
                (hash << 6) + (hash >> 2);
        };
        mix(reinterpret_cast<std::uintptr_t>(key.vertices));
        mix(reinterpret_cast<std::uintptr_t>(key.indices));
        mix(key.generation); mix(key.current_revision);
        mix(key.vertex_resource); mix(key.vertex_kind); mix(key.vertex_ordinal);
        mix(key.index_resource); mix(key.index_kind); mix(key.index_ordinal);
        mix(key.vertex_byte_offset); mix(key.index_byte_offset);
        mix(key.vertex_stride); mix(key.vertex_count); mix(key.index_count);
        mix(static_cast<std::uint64_t>(key.index_format));
        mix(key.dynamic); mix(key.opaque); mix(key.force_rebuild);
        return hash;
    }
};

class BrixelizerGeometryLookup
{
public:
    [[nodiscard]] std::uint32_t FindOrAdd(const SoftwareSceneGeometry &geometry,
        std::vector<SoftwareSceneGeometry> &geometries)
    {
        const BrixelizerGeometryKey key{
            geometry.stable_key, geometry.vertices, geometry.indices,
            geometry.generation, geometry.current_revision,
            geometry.vertex_version.resource.value,
            static_cast<std::uint32_t>(geometry.vertex_version.resource.kind),
            geometry.vertex_version.ordinal,
            geometry.index_version.resource.value,
            static_cast<std::uint32_t>(geometry.index_version.resource.kind),
            geometry.index_version.ordinal,
            geometry.vertex_byte_offset, geometry.index_byte_offset,
            geometry.vertex_stride, geometry.vertex_count, geometry.index_count,
            geometry.index_format, geometry.dynamic, geometry.opaque,
            geometry.force_rebuild};
        const auto [found, inserted] = entries_.try_emplace(key,
            static_cast<std::uint32_t>(geometries.size()));
        if (inserted) geometries.push_back(geometry);
        return found->second;
    }

private:
    std::unordered_map<BrixelizerGeometryKey, std::uint32_t,
        BrixelizerGeometryKeyHash> entries_;
};

// Preserve actual submesh ranges when a page mixes opaque and translucent
// character slots. The vectors are CPU registration inputs for this frame.
[[nodiscard]] Result AppendBrixelizerLeaf(
    const SoftwareSceneGeometry &, const SoftwareSceneInstance &,
    std::span<const render_data::GeometrySubmesh>,
    std::span<const render_data::CharacterMaterialBinding>,
    std::span<const render_data::MaterialSurfaceLayout> hero_layouts,
    std::uint32_t environment_capabilities, bool masked, bool transmissive,
    DirectX::XMFLOAT3 bounds_min, DirectX::XMFLOAT3 bounds_max,
    std::uint64_t material_revision,
    BrixelizerGeometryLookup &,
    std::vector<SoftwareSceneGeometry> &, std::vector<BrixelizerSceneInstance> &);
} // namespace hs
