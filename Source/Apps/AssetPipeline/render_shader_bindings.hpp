#pragma once

#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/geometry_package.hpp>

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hs::shader_content
{
// The same ordered definitions enter compilation, dependency scans and ActionKey.
inline constexpr std::array kRenderBindingDefines{
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_GEOMETRY_PAGE_CAPACITY", render_data::kGeometryPageCapacity},
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_GEOMETRY_BUFFERS_PER_PAGE", render_data::kGeometryBuffersPerPage},
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_AUTHORED_MATERIAL_COUNT", render_data::kAuthoredMaterialNames.size()},
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_HERO_MATERIAL_COUNT", render_data::kHeroMaterialNames.size()},
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_ENVIRONMENT_GEOMETRY_COUNT", render_data::kEnvironmentMaterialNames.size()},
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_ENVIRONMENT_GEOMETRY_LOD_COUNT", render_data::kEnvironmentGeometryLodCount},
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_MATERIAL_TEXTURE_SLOT_COUNT", render_data::kMaterialTextureSlots},
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_GROUND_DETAIL_GEOMETRY_BEGIN", render_data::kGroundDetailGeometryBegin},
    std::pair<std::string_view, std::uint32_t>{"HS_RENDER_GROUND_DETAIL_MATERIAL_ORDINAL", render_data::kGroundDetailMaterialOrdinal}};

[[nodiscard]] inline std::string RenderBindingContract()
{
    std::string contract;
    for (const auto &[name, value] : kRenderBindingDefines)
        contract += std::string(name) + "=" + std::to_string(value) + "\n";
    return contract;
}

inline void AppendRenderBindingArguments(std::vector<std::wstring> &arguments)
{
    for (const auto &[name, value] : kRenderBindingDefines)
        arguments.push_back(L"-D" + std::wstring(name.begin(), name.end()) + L"=" + std::to_wstring(value));
}

} // namespace hs::shader_content
