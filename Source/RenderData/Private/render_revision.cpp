#include <hs/render_data/render_revision.hpp>
#include <hs/render_data/material_schema.hpp>

namespace hs::render_data
{
bool IsRendererMaterialRevisionProduct(std::string_view id)
{
    if (id.starts_with("render/material/") || id.starts_with("render/texture/") ||
        id.starts_with("render/shader/") || id.starts_with("render/geometry/") || id.starts_with("environment/")) return true;
    if (id.starts_with("render/"))
        for (const auto name : kEnvironmentMaterialNames)
            if (id.substr(7).starts_with(name)) return true;
    return false;
}
} // namespace hs::render_data
