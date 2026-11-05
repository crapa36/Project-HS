#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/material_schema.hpp>

namespace hs::render_data
{
bool IsTerrainSurfaceSectionProduct(std::string_view id)
{
    constexpr std::string_view prefix{"render/terrain/arena_surface_section_"};
    constexpr std::string_view suffix{".rpkg"};
    if (!id.starts_with(prefix) || !id.ends_with(suffix)) return false;
    const auto index = id.substr(prefix.size(), id.size() - prefix.size() - suffix.size());
    if (index.empty()) return false;
    for (const char c : index) if (c < '0' || c > '9') return false;
    return true;
}
bool IsTerrainSurfaceProduct(std::string_view id)
{
    return id == "render/terrain/arena_surface.rpkg" || IsTerrainSurfaceSectionProduct(id);
}
bool IsRendererMaterialRevisionProduct(std::string_view id)
{
    if (IsTerrainSurfaceProduct(id) || id.starts_with("render/material/") || id.starts_with("render/texture/") ||
        id.starts_with("render/shader/") || id.starts_with("render/geometry/") || id.starts_with("environment/")) return true;
    if (id.starts_with("render/"))
        for (const auto name : kEnvironmentMaterialNames)
            if (id.substr(7).starts_with(name)) return true;
    return false;
}
} // namespace hs::render_data
