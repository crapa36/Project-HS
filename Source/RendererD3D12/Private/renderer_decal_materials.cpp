#include "renderer_impl.hpp"

#include <stdexcept>

namespace hs
{
Result D3D12Renderer::Impl::PrepareDecalMaterialParameters(const RendererConfig &candidate,
    std::vector<render_data::MaterialParameterSlot> &slots)
{
    for (std::size_t ordinal = 0; ordinal < render_data::kDecalMaterialNames.size(); ++ordinal)
    {
        const std::string name(render_data::kDecalMaterialNames[ordinal]);
        const std::string parameter_id = "render/material/" + name + "/parameters.rpkg";
        const std::string material_id = "render/material/" + name + "/material.rpkg";
        const auto handle = FindRendererMaterial(candidate, MakeAssetId(material_id));
        if (!handle) throw std::runtime_error("Compiled decal material is missing");
        const auto &descriptor = candidate.packed_materials[handle.value];
        if (descriptor.kind != render_data::MaterialKind::Decal || descriptor.programs.size() != 1 || descriptor.textures.size() != 1)
            throw std::runtime_error("Compiled decal program or texture shape is invalid");
        render_data::DecalMaterialPackageData material;
        material.parameter_layout_hash = MaterialLayoutDigest(descriptor);
        material.receivers = descriptor.receivers;
        material.dependencies = {parameter_id, std::string(descriptor.programs[0].product_id),
            std::string(descriptor.textures[0].product_id)};
        std::ranges::sort(material.dependencies);
        material.program.kind = descriptor.programs[0].kind;
        material.program.dependency_index = static_cast<std::uint16_t>(std::ranges::find(material.dependencies,
            descriptor.programs[0].product_id) - material.dependencies.begin());
        material.texture_dependency_index = static_cast<std::uint16_t>(std::ranges::find(material.dependencies,
            descriptor.textures[0].product_id) - material.dependencies.begin());
        const std::string_view texture_id = ordinal == 0 ? "render/texture/vfx_fracture_decal_array.rpkg"
            : "render/texture/vfx_organic_decal_array.rpkg";
        if (descriptor.textures[0].product_id != texture_id)
            throw std::runtime_error("Compiled decal binding disagrees with its resident texture array");
        const auto base = static_cast<std::uint32_t>(slots.size());
        const auto count = static_cast<std::uint32_t>(descriptor.defaults.size());
        decal_parameter_bases[ordinal] = base;
        decal_parameter_counts[ordinal] = count;
        decal_material_policies[ordinal] = material.receivers;
        decal_material_programs[ordinal] = std::move(material);
        slots.insert(slots.end(), descriptor.defaults.begin(), descriptor.defaults.end());
    }
    return Result::Success();
}

} // namespace hs
