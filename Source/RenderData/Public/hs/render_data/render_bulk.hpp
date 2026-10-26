#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace hs::render_data
{
enum class RenderBulkFormat : std::uint32_t
{
    Package, GeometryPage, GeometryHierarchy, SceneHlod, StaticShadowIndex,
    StaticShadowPages, DdsTexture, SceneProbe, VfxMeshAtlas, ShaderDxil
};

struct BulkRange
{
    std::uint64_t offset{}, byte_count{};
    RenderBulkFormat format{};
    std::array<std::byte, 32> expected_digest{};
};

// Its table index is a dense handle for this deployment only.
struct RendererBulkDescriptor
{
    AssetId asset_id;
    BulkRange range;
};
} // namespace hs::render_data
