#pragma once

#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/render_revision.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace hs::render_data
{
// Shader products that can contribute to local-probe capture or integration.
// Kept narrower than renderer-wide material revision inputs so unrelated
// screen-space shaders do not invalidate probe payload identity.
[[nodiscard]] inline bool IsLocalProbeCaptureShaderProduct(std::string_view product_id)
{
    constexpr std::array fixed_products{
        std::string_view{"render/shader/fullscreen_vs.dxil"},
        std::string_view{"render/shader/local_probe_capture_source_base_vs.dxil"},
        std::string_view{"render/shader/local_probe_capture_sky_ps.dxil"},
        std::string_view{"render/shader/local_probe_sh_cs.dxil"},
        std::string_view{"render/shader/local_probe_ggx_cs.dxil"},
    };
    if (std::ranges::find(fixed_products, product_id) != fixed_products.end())
        return true;

    constexpr std::string_view prefix{"render/shader/"};
    if (!product_id.starts_with(prefix))
        return false;
    const auto filename = product_id.substr(prefix.size());
    constexpr std::string_view capture_suffix{"_probe_capture.dxil"};
    constexpr std::string_view shadow_suffix{"_probe_shadow.dxil"};
    for (const auto material : kAuthoredMaterialNames)
    {
        if (filename.starts_with(material))
        {
            const auto suffix = filename.substr(material.size());
            if (suffix == capture_suffix || suffix == shadow_suffix)
                return true;
        }
    }
    return false;
}

// Capture compatibility covers static environment products, not unrelated
// character, decal or VFX products present only in the full runtime catalog.
[[nodiscard]] inline bool IsLocalProbeMaterialRevisionProduct(std::string_view product_id)
{
    if (!IsRendererMaterialRevisionProduct(product_id)) return false;
    if (product_id.starts_with("render/shader/")) return IsLocalProbeCaptureShaderProduct(product_id);
    if (product_id.starts_with("render/texture/"))
        return product_id.starts_with("render/texture/environment_") ||
            product_id.starts_with("render/texture/environment/");
    if (product_id.starts_with("render/geometry/"))
        return product_id.starts_with("render/geometry/environment");
    constexpr std::string_view material_prefix{"render/material/"};
    if (product_id.starts_with(material_prefix))
    {
        const auto material = product_id.substr(material_prefix.size());
        for (const auto name : kAuthoredMaterialNames)
            if (material.starts_with(name))
            {
                const auto suffix = material.substr(name.size());
                if (suffix == ".rpkg" || suffix.starts_with('/')) return true;
            }
        return false;
    }
    return true;
}
} // namespace hs::render_data
