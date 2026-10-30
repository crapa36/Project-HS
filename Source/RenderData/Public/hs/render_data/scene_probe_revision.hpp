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

// Keep nonshader material, texture, geometry, and environment products in the
// established conservative domain. Narrow only shader products to the exact
// local-probe capture and integration family.
[[nodiscard]] inline bool IsLocalProbeMaterialRevisionProduct(std::string_view product_id)
{
    return IsRendererMaterialRevisionProduct(product_id) &&
        (!product_id.starts_with("render/shader/") || IsLocalProbeCaptureShaderProduct(product_id));
}
} // namespace hs::render_data
