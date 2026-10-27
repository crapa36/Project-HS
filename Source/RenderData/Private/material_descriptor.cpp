#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace hs::render_data
{
namespace
{
void Require(bool valid, const char *message)
{ if (!valid) throw std::runtime_error(std::string("material descriptor: ") + message); }

std::string_view Name(std::string_view product) noexcept
{
    if (product.starts_with("render/material/") && product.ends_with("/material.rpkg"))
    {
        const auto name = product.substr(16, product.size() - 16 - 14);
        if (std::ranges::find(kAuthoredMaterialNames, name) != kAuthoredMaterialNames.end() ||
            std::ranges::find(kHeroMaterialNames, name) != kHeroMaterialNames.end() ||
            std::ranges::find(kDecalMaterialNames, name) != kDecalMaterialNames.end()) return name;
    }
    if (product == "render/material/terrain.rpkg") return "terrain";
    if (product == "render/material/ground_detail.rpkg") return "ground_detail";
    return {};
}
std::uint32_t BulkIndex(AssetId id, std::span<const AssetId> assets)
{
    const auto found = std::lower_bound(assets.begin(), assets.end(), id.value,
        [](AssetId candidate, std::uint64_t value) { return candidate.value < value; });
    Require(found != assets.end() && found->value == id.value && found - assets.begin() < UINT32_MAX,
            "referenced bulk is absent");
    return static_cast<std::uint32_t>(found - assets.begin());
}
TextureHandle TextureIndex(std::string_view product, std::span<const TextureDescriptor> textures)
{
    const auto id = MakeAssetId(product);
    const auto found = std::ranges::find_if(textures, [&](const auto &entry) { return entry.asset_id.value == id.value; });
    Require(found != textures.end() && found - textures.begin() < UINT32_MAX, "referenced texture is absent");
    return {static_cast<std::uint32_t>(found - textures.begin())};
}
const ChunkView &RequiredChunk(const PackageView &package, ChunkType type)
{
    const auto found = std::ranges::find(package.chunks, type, &ChunkView::type);
    Require(found != package.chunks.end() && found->required, "mandatory material chunk is absent");
    return *found;
}
void Surface(MaterialSurfaceLayout layout)
{
    Require(!(layout.capability_mask & ~511u) && (layout.capability_mask & 65u) &&
            (layout.capability_mask & 65u) != 65u && !(layout.varying_mask & ~31u), "invalid surface capabilities");
    const auto transport = layout.capability_mask & (16u | 128u | 256u);
    Require(!transport || !(transport & (transport - 1)), "conflicting surface transport");
    Require(!(layout.capability_mask & 64u) || !(layout.capability_mask & (2u|4u|8u|16u|128u|256u)), "unlit scattering surface");
    constexpr std::array<std::uint32_t,5> capabilities{4u,2u,8u,16u,32u|64u};
    for (std::size_t i=0;i<capabilities.size();++i)
        Require(!(layout.varying_mask & (1u<<i)) || (layout.capability_mask & capabilities[i]), "varying field lacks capability");
}
} // namespace

bool IsMaterialDescriptorProduct(std::string_view product_id) noexcept { return !Name(product_id).empty(); }
std::string MaterialProduct(std::string_view name)
{
    Require(std::ranges::find(kAuthoredMaterialNames, name) != kAuthoredMaterialNames.end() ||
            std::ranges::find(kHeroMaterialNames, name) != kHeroMaterialNames.end() ||
            std::ranges::find(kDecalMaterialNames, name) != kDecalMaterialNames.end(),
            "unknown material name");
    if (name == "terrain" || name == "ground_detail") return "render/material/" + std::string(name) + ".rpkg";
    return "render/material/" + std::string(name) + "/material.rpkg";
}
std::string MaterialParametersProduct(std::string_view material_product)
{
    const auto name = Name(material_product);
    Require(!name.empty(), "unknown material product");
    return "render/material/" + std::string(name) + "/parameters.rpkg";
}

MaterialDescriptorStorage CompileMaterialDescriptor(std::string_view product_id,
    std::span<const std::byte> material_bytes, std::span<const std::byte> parameter_bytes,
    std::uint32_t bulk_index, std::span<const AssetId> bulk_assets, std::span<const TextureDescriptor> textures)
{
    const auto name = Name(product_id);
    Require(!name.empty(), "unknown material product");
    const auto package = ReadPackage(material_bytes, MakeAssetId(product_id));
    const auto parameter_id = MaterialParametersProduct(product_id);
    const auto parameters = ReadMaterialParameterPackage(parameter_bytes, MakeAssetId(parameter_id));
    const auto &layout = RequiredChunk(package, ChunkType::MaterialParameterLayout);
    ValidateMaterialParameterLayout(layout.bytes, parameters);
    MaterialDescriptorStorage result;
    result.value.asset_id = package.asset_id;
    result.value.bulk_index = bulk_index;
    std::copy(layout.bytes.begin(), layout.bytes.end(), result.value.parameter_layout.begin());
    for (const auto &parameter : parameters) result.defaults.push_back({parameter.default_words});
    const auto keep = [&](std::string_view id) -> std::string_view { result.product_ids.emplace_back(id); return result.product_ids.back(); };
    const auto program = [&](const MaterialProgram &source) {
        const auto &id = package.dependencies.at(source.dependency_index);
        result.programs.push_back({source.kind, BulkIndex(MakeAssetId(id), bulk_assets), MakeAssetId(id), keep(id)});
    };
    const auto texture = [&](std::uint16_t slot, std::string_view id, MaterialTextureSemantic semantic,
                             std::uint32_t required_layer, std::uint32_t abi = kMaterialShaderBindingAbi) {
        const auto handle = TextureIndex(id, textures);
        const auto &source = textures[handle.value];
        Require(source.dimension == TextureDimension::Texture2DArray, "material texture must be a 2D array");
        ValidateMaterialTexturePackage({"texture", std::string(id), MaterialTextureDimension::Texture2DArray, semantic}, source.description, required_layer);
        result.textures.push_back({slot, handle, MaterialTextureDimension::Texture2DArray, semantic, abi, keep(id)});
    };
    const auto required_float = [&](std::string_view id, float maximum) {
        const auto found = std::ranges::find(parameters, id, &MaterialParameterDefinition::id);
        Require(found != parameters.end() && found->type == MaterialParameterType::Float, "required float parameter is absent");
        const auto value = std::bit_cast<float>(found->default_words[0]);
        Require(std::isfinite(value) && value >= 0 && value <= maximum, "required float default exceeds bounds");
    };
    if (std::ranges::find(kHeroMaterialNames, name) != kHeroMaterialNames.end())
    {
        result.value.kind = MaterialKind::Hero;
        const auto hero = ReadHeroMaterialPackage(package, name);
        result.value.surface = hero.surface;
        result.value.domain = hero.domain;
        for (const auto &entry : hero.programs) program(entry);
        for (const auto &dependency : package.dependencies) static_cast<void>(BulkIndex(MakeAssetId(dependency), bulk_assets));
        // Fixed Hero adapter textures are not declared material bindings.
        if (name == "monster_pbr")
            for (const auto *suffix : {"monster_basecolor", "monster_emissive", "monster_ram"})
            {
                const auto &source = textures[TextureIndex("render/texture/" + std::string(suffix) + ".rpkg", textures).value];
                Require(source.dimension == TextureDimension::Texture2DArray && source.description.array_size == 1 &&
                        source.description.width == 512 && source.description.height == 512 && source.description.mip_count == 10 &&
                        source.description.format == (std::string_view(suffix) == "monster_ram" ? kTextureFormatBc7 : kTextureFormatBc7Srgb),
                        "Hero adapter texture shape or format mismatch");
            }
    }
    else if (std::ranges::find(kDecalMaterialNames, name) != kDecalMaterialNames.end())
    {
        result.value.kind = MaterialKind::Decal;
        const auto decal = ReadDecalMaterialPackage(package, name);
        result.value.receivers = decal.receivers;
        required_float("target_roughness", 1); required_float("roughness_blend", 1);
        program(decal.program);
        const bool fracture = name == "decal_fracture";
        const auto &id = package.dependencies[decal.texture_dependency_index];
        Require(id == (fracture ? "render/texture/vfx_fracture_decal_array.rpkg" : "render/texture/vfx_organic_decal_array.rpkg"),
                "decal texture disagrees with its adapter");
        texture(0, id, fracture ? MaterialTextureSemantic::DecalStructure : MaterialTextureSemantic::DecalMask, 0, kDecalShaderBindingAbi);
        for (const auto &dependency : package.dependencies) static_cast<void>(BulkIndex(MakeAssetId(dependency), bulk_assets));
    }
    else
    {
        result.value.kind = MaterialKind::Environment;
        required_float("metallic", 1); required_float("roughness_scale", 2);
        Require(std::ranges::find(package.dependencies, parameter_id) != package.dependencies.end(), "environment parameter dependency is absent");
        result.value.surface = DecodeMaterialSurfaceLayout(RequiredChunk(package, ChunkType::MaterialSurfaceLayout).bytes);
        const auto material = DecodeRockMaterial(RequiredChunk(package, ChunkType::MaterialMetadata).bytes, package.dependencies);
        result.value.metallic = material.metallic; result.value.roughness_scale = material.roughness_scale;
        result.value.flags = (material.two_sided ? kMaterialFlagTwoSided : 0) | (material.alpha_mask ? kMaterialFlagAlphaMask : 0);
        const bool masked = name.starts_with("leaf_") || name.starts_with("grass_");
        Require((ClassifyEnvironmentMaterial(material) == EnvironmentMaterialVariant::MaskedTwoSided) == masked, "environment domain mismatch");
        const auto programs = DecodeMaterialPrograms(RequiredChunk(package, ChunkType::MaterialProgram).bytes, package.dependencies);
        Require(programs.size() == (masked ? 2u : 1u), "environment program count mismatch");
        for (std::size_t i=0;i<programs.size();++i)
        {
            Require(programs[i].kind == (i ? MaterialProgramKind::MaskedShadowPS : masked ? MaterialProgramKind::MaskedScenePS : MaterialProgramKind::OpaqueScenePS) &&
                    package.dependencies[programs[i].dependency_index] == "render/shader/" + std::string(name) + (i ? "_shadow.dxil" : "_scene.dxil"),
                    "environment program identity mismatch");
            program(programs[i]);
        }
        std::uint32_t layer = name == "terrain" ? 3 : masked ? (name.starts_with("grass_") ? 1 : 2) : 0;
        const auto geometry = std::ranges::find(package.chunks, ChunkType::GeometryMetadata, &ChunkView::type);
        if (!masked && geometry != package.chunks.end()) layer = DecodeGeometryMetadata(geometry->bytes, package.dependencies, product_id).max_material_index;
        for (const auto &binding : DecodeMaterialBindings(RequiredChunk(package, ChunkType::MaterialBindings).bytes, package.dependencies))
        {
            if (binding.dependency_index == 0xffff) { Require(material.textures[binding.slot].empty(), "declared texture lacks binding"); continue; }
            const auto &id = package.dependencies[binding.dependency_index];
            Require(material.textures[binding.slot] == id, "material texture and binding disagree");
            texture(binding.slot, id, binding.semantic, layer);
        }
    }
    ValidateMaterialDescriptor(result.View(), bulk_assets, textures);
    return result;
}

void ValidateMaterialDescriptor(const MaterialDescriptor &value, std::span<const AssetId> bulk_assets,
    std::span<const TextureDescriptor> textures)
{
    Require(value.asset_id.value && value.bulk_index < bulk_assets.size() && bulk_assets[value.bulk_index].value == value.asset_id.value &&
            value.defaults.size() <= 64 && value.kind <= MaterialKind::Decal && !(value.flags & ~3u) &&
            value.domain <= HeroMaterialDomain::Translucent, "invalid material identity, defaults or flags");
    Require(std::isfinite(value.metallic) && value.metallic >= 0 && value.metallic <= 1 &&
            std::isfinite(value.roughness_scale) && value.roughness_scale >= 0 && value.roughness_scale <= 2, "invalid material uniforms");
    if (value.kind != MaterialKind::Decal) Surface(value.surface);
    else { Require(value.surface == MaterialSurfaceLayout{}, "decal has a surface closure"); ValidateDecalReceiverPolicy(value.receivers); }
    std::uint32_t program_mask{};
    for (const auto &program : value.programs)
    {
        const auto kind = static_cast<unsigned>(program.kind);
        Require(kind >= 1 && kind <= 9 && !(program_mask & (1u<<kind)) && program.bulk_index < bulk_assets.size() &&
                bulk_assets[program.bulk_index].value == program.asset_id.value && MakeAssetId(program.product_id).value == program.asset_id.value &&
                program.product_id.starts_with("render/shader/") && program.product_id.ends_with(".dxil"), "invalid material program reference");
        program_mask |= 1u<<kind;
    }
    if (value.kind == MaterialKind::Environment)
        Require(value.domain == HeroMaterialDomain::Opaque && value.defaults.size() >= 2 &&
                program_mask == (value.flags ? ((1u<<2)|(1u<<3)) : (1u<<1)), "environment program domain mismatch");
    else if (value.kind == MaterialKind::Hero)
    {
        Require(!value.flags && value.textures.empty() && program_mask == (value.domain == HeroMaterialDomain::Translucent ? (1u<<8) : 240u) &&
                bool(value.surface.capability_mask & 256u) == (value.domain == HeroMaterialDomain::Translucent) &&
                (value.domain != HeroMaterialDomain::Translucent || !(value.surface.capability_mask & 128u)), "Hero domain or adapter bindings mismatch");
    }
    else Require(!value.flags && value.domain == HeroMaterialDomain::Opaque && value.defaults.size() >= 2 &&
                 program_mask == (1u<<9) && value.textures.size() == 1, "decal descriptor shape mismatch");
    std::uint32_t texture_mask{};
    std::vector<MaterialTextureDefinition> definitions;
    for (const auto &binding : value.textures)
    {
        Require(binding.slot < kMaterialTextureSlots && !(texture_mask & (1u<<binding.slot)) && binding.texture.value < textures.size() &&
                binding.dimension == MaterialTextureDimension::Texture2DArray &&
                binding.shader_binding_abi == (value.kind == MaterialKind::Decal ? kDecalShaderBindingAbi : kMaterialShaderBindingAbi),
                "invalid material texture slot or ABI");
        texture_mask |= 1u<<binding.slot;
        const auto &texture = textures[binding.texture.value];
        Require(texture.dimension == TextureDimension::Texture2DArray && texture.asset_id.value == MakeAssetId(binding.product_id).value,
                "invalid material texture handle");
        MaterialTextureDefinition definition{"slot_" + std::to_string(binding.slot), std::string(binding.product_id), binding.dimension, binding.semantic};
        ValidateMaterialTexturePackage(definition, texture.description, 0);
        if (value.kind == MaterialKind::Decal) { Require(binding.slot == 0, "invalid decal texture slot"); ValidateDecalMaterialTextureDefinition(definition); }
        definitions.push_back(std::move(definition));
    }
    if (value.kind == MaterialKind::Environment)
    {
        Require(texture_mask == ((1u<<value.textures.size())-1), "environment texture slots are not contiguous");
        ValidateMaterialTextureDefinitions(definitions);
        RockMaterial material;
        material.metallic = value.metallic; material.roughness_scale = value.roughness_scale;
        material.two_sided = (value.flags & kMaterialFlagTwoSided) != 0;
        material.alpha_mask = (value.flags & kMaterialFlagAlphaMask) != 0;
        for (const auto &binding : value.textures) material.textures[binding.slot] = binding.product_id;
        ValidateRockMaterial(material);
    }
}
} // namespace hs::render_data
