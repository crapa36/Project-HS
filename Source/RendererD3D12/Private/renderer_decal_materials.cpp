#include "renderer_impl.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>

namespace hs
{
Result D3D12Renderer::Impl::PrepareDecalMaterialParameters(const RendererConfig &candidate,
    bool require_compatible_layout, std::vector<render_data::MaterialParameterSlot> &slots)
{
    for (std::size_t ordinal = 0; ordinal < render_data::kDecalMaterialNames.size(); ++ordinal)
    {
        const std::string name(render_data::kDecalMaterialNames[ordinal]);
        const std::string parameter_id = "render/material/" + name + "/parameters.rpkg";
        const std::string material_id = "render/material/" + name + "/material.rpkg";
        std::filesystem::path path;
        if (auto resolved = candidate.ResolveProduct(parameter_id, path); !resolved) return resolved;
        std::vector<std::byte> bytes;
        if (auto read = ReadBinary(path, bytes); !read) return read;
        const auto definitions = render_data::ReadMaterialParameterPackage(bytes, MakeAssetId(parameter_id));
        for (const std::string_view id : {"target_roughness", "roughness_blend"})
        {
            const auto definition = std::ranges::find(definitions, id,
                &render_data::MaterialParameterDefinition::id);
            if (definition == definitions.end() || definition->type != render_data::MaterialParameterType::Float)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Decal material requires a scalar float parameter: " + std::string(id));
            const auto value = std::bit_cast<float>(definition->default_words[0]);
            if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Decal material parameter default is outside [0,1]: " + std::string(id));
        }
        if (auto resolved = candidate.ResolveProduct(material_id, path); !resolved) return resolved;
        if (auto read = ReadBinary(path, bytes); !read) return read;
        auto material = render_data::ReadDecalMaterialPackage(bytes, name);
        const auto layout = render_data::MaterialParameterLayoutHash(definitions);
        if (material.parameter_layout_hash != layout)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Decal parameter layout disagrees with its selected program: " + parameter_id);
        // Both programs have dedicated typed IR and immutable bindings. Their
        // samplers read the canonical resident VFX arrays; no asset pool or
        // shader register is selected by an unauthenticated runtime enum.
        const std::string texture_id = ordinal == 0 ? "render/texture/vfx_fracture_decal_array.rpkg"
                                                  : "render/texture/vfx_organic_decal_array.rpkg";
        if (material.dependencies[material.texture_dependency_index] != texture_id)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Decal program binding disagrees with its resident texture array: " + material_id);
        for (const auto &dependency : material.dependencies)
            if (auto resolved = candidate.ResolveProduct(dependency, path); !resolved) return resolved;
        const auto base = static_cast<std::uint32_t>(slots.size());
        const auto count = static_cast<std::uint32_t>(definitions.size());
        if (require_compatible_layout &&
            (layout != decal_parameter_layouts[ordinal] || base != decal_parameter_bases[ordinal] ||
             count != decal_parameter_counts[ordinal] || material.receivers != decal_material_policies[ordinal] ||
             material.dependencies != decal_material_programs[ordinal].dependencies))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Decal parameter reload changes its resident layout, receiver policy or bindings: " + parameter_id);
        decal_parameter_candidate_bases[ordinal] = base;
        decal_parameter_candidate_counts[ordinal] = count;
        decal_parameter_candidate_layouts[ordinal] = layout;
        decal_material_candidate_policies[ordinal] = material.receivers;
        decal_material_candidate_programs[ordinal] = std::move(material);
        for (const auto &definition : definitions) slots.push_back({definition.default_words});
    }
    return Result::Success();
}

void D3D12Renderer::Impl::CommitDecalMaterialParameters() noexcept
{
    decal_parameter_bases = decal_parameter_candidate_bases;
    decal_parameter_counts = decal_parameter_candidate_counts;
    decal_parameter_layouts = std::move(decal_parameter_candidate_layouts);
    decal_material_policies = decal_material_candidate_policies;
    decal_material_programs = std::move(decal_material_candidate_programs);
}
} // namespace hs
