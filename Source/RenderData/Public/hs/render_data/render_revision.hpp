#pragma once

#include <hs/render_data/render_bulk.hpp>
#include <string_view>

namespace hs::render_data
{
// Provenance has an identity but no runtime payload or filesystem path.
struct RendererSourceRevision
{
    AssetId asset_id;
    std::string_view product_id;
    std::array<std::byte, 32> digest;
};
// Reuse the existing bulk digest instead of storing it again for revision inputs.
struct RendererRevisionBulk
{
    std::string_view product_id;
    std::uint32_t bulk_index{};
};
[[nodiscard]] bool IsRendererMaterialRevisionProduct(std::string_view product_id);
} // namespace hs::render_data
