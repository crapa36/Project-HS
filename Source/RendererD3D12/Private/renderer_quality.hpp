#pragma once

#include <hs/render_data/render_snapshot_values.hpp>
#include <cstddef>

namespace hs
{
// Select decorative grass by canonical source identity, before any view-local
// visibility ordering. Trees, structural HLOD members and VFX owners retain
// their authored geometry.
[[nodiscard]] inline bool RetainDecorativeFoliage(const RenderInstance &source,
    std::size_t source_index, float density) noexcept
{
    if (source.mesh != RenderMesh::Grass || source.vfx_owner_id || density >= 1) return true;
    auto key = source.stable_id ? source.stable_id : static_cast<std::uint64_t>(source_index) + 1;
    key ^= key >> 30; key *= 0xbf58476d1ce4e5b9ull;
    key ^= key >> 27; key *= 0x94d049bb133111ebull; key ^= key >> 31;
    return double(static_cast<std::uint32_t>(key >> 32)) < double(density) * 4294967296.0;
}
} // namespace hs
