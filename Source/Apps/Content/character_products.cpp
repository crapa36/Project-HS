#include "character_products.hpp"
#include "geometry_cooker.hpp"
#include <hs/render_data/texture_package.hpp>
#include <hs/core/sha256.hpp>
#ifdef _WIN32
#include <DirectXMath.h>
#endif

#include <algorithm>
#include <cmath>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace hs::content
{
namespace
{
namespace rd = render_data;
void Require(bool value, const std::string &message)
{
    if (!value) throw std::runtime_error("Character products: " + message);
}

std::uint32_t HeroOrdinal(const std::string &name)
{
    const auto found = std::ranges::find(rd::kHeroMaterialNames, name);
    Require(found != rd::kHeroMaterialNames.end(), "unknown authored hero material: " + name);
    return static_cast<std::uint32_t>(found - rd::kHeroMaterialNames.begin());
}
} // namespace

render_data::MonsterCharacterData CookMonsterCharacterData(const CharacterIR &character,
    const render_data::GeometryData &geometry, const render_data::GeometryMetadata &metadata,
    bool suicide_bubble)
{
#ifndef _WIN32
    (void)character; (void)geometry; (void)metadata; (void)suicide_bubble;
    throw std::runtime_error("Monster palette cooking requires the native DirectXMath float contract");
#else
    rd::ValidateCharacterSource(character,false);
    rd::ValidateGeometryPage(metadata,geometry);
    Require(character.bone_names.size()==character.bone_count,"monster skeleton name count mismatch");
    for (const auto &vertex : geometry.vertices)
    {
        float sum{};
        for (std::size_t lane=0;lane<4;++lane)
        {
            Require(vertex.bone_indices[lane]<character.bone_count &&
                std::isfinite(vertex.bone_weights[lane]) && vertex.bone_weights[lane]>=0 && vertex.bone_weights[lane]<=1,
                "invalid final geometry skin influence");
            sum+=vertex.bone_weights[lane];
        }
        Require(std::abs(sum-1.0f)<=0.001f,"final geometry skin weights are not normalized");
    }
    rd::MonsterCharacterData data; data.bone_count=character.bone_count; data.lods.push_back(metadata);
    data.flags=suicide_bubble?rd::kMonsterDeformationSuicideBubble:rd::kMonsterBoundsPositionEnclosure;
    data.bubble_amplitude=suicide_bubble?0.006f:0.0f;
    hs::Sha256Stream identity;
    const auto integer=[&](std::uint64_t value,std::size_t width) {
        std::array<std::byte,8> bytes{};
        for(std::size_t i=0;i<width;++i) bytes[i]=std::byte((value>>(i*8))&255);
        identity.Add(std::span(bytes).first(width));
    };
    integer(rd::kMonsterDeformationAbi,4); integer(character.bone_count,4);
    for(std::size_t bone=0;bone<character.bone_count;++bone)
    {
        const auto &name=character.bone_names[bone]; integer(name.size(),4);
        identity.Add(std::as_bytes(std::span(name.data(),name.size())));
        integer(character.parents[bone],2);
        for(float value:character.inverse_bind_matrices[bone]) integer(std::bit_cast<std::uint32_t>(value),4);
    }
    data.skeleton_binding=identity.Finish();
    data.final_matrices.reserve(character.transforms.size());
    // This is the former renderer initialization sequence, including its float
    // stores between parent steps, matrix convention and positive determinant
    // admission. Raw FBX double globals are not a substitute for these values.
    for(const auto &clip:character.clips)
    {
        Require(clip.first_transform==data.final_matrices.size(),"monster source clip ranges are not contiguous");
        for(std::uint32_t frame=0;frame<clip.frame_count;++frame)
        {
            std::array<DirectX::XMFLOAT4X4,kMaxCharacterBones> globals{};
            for(std::uint32_t bone=0;bone<character.bone_count;++bone)
            {
                const auto &local=character.transforms[clip.first_transform+frame*character.bone_count+bone];
                const auto local_matrix=DirectX::XMMatrixScaling(local.scale[0],local.scale[1],local.scale[2]) *
                    DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(local.rotation[0],local.rotation[1],local.rotation[2],local.rotation[3])) *
                    DirectX::XMMatrixTranslation(local.translation[0],local.translation[1],local.translation[2]);
                const auto parent=character.parents[bone];
                const auto global=parent==std::numeric_limits<std::uint16_t>::max()?local_matrix:
                    local_matrix*DirectX::XMLoadFloat4x4(&globals[parent]);
                DirectX::XMStoreFloat4x4(&globals[bone],global);
                DirectX::XMFLOAT4X4 inverse_bind{};
                std::memcpy(&inverse_bind,character.inverse_bind_matrices[bone].data(),sizeof(inverse_bind));
                const auto skin=DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&inverse_bind))*global;
                const auto determinant=DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(skin));
                Require(std::isfinite(determinant) && determinant>0.0001f &&
                    !DirectX::XMMatrixIsNaN(skin) && !DirectX::XMMatrixIsInfinite(skin),"Monster animation produced an invalid skin pose.");
                DirectX::XMFLOAT4X4 stored{};
                DirectX::XMStoreFloat4x4(&stored,DirectX::XMMatrixTranspose(skin));
                auto &matrix=data.final_matrices.emplace_back(); std::memcpy(matrix.data(),&stored,sizeof(stored));
            }
        }
    }
    // For position, linear final-matrix interpolation is enclosed by its two
    // exact endpoints. gamma64 covers FP32 multiply/add/lerp/dot rounding even
    // without FMA; the absolute-product bound avoids cancellation assumptions.
    // Bubble uses a normalized cofactor normal. Endpoints do not prove that
    // every blended normal is nonsingular, so its rows remain reduction-only.
    constexpr double u=0x1p-24;
    constexpr double gamma=(64*u)/(1-64*u);
    for(const auto &clip:character.clips)
        for(std::uint32_t frame=0;frame<clip.frame_count;++frame)
        {
            rd::CharacterIntervalBound bound; bound.clip=clip.clip; bound.frame=frame;
            std::array<double,3> low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY},magnitude{};
            double position_l1{};
            for(const auto &vertex:geometry.vertices)
            {
                position_l1=std::max(position_l1,std::abs(double(vertex.position[0]))+
                    std::abs(double(vertex.position[1]))+std::abs(double(vertex.position[2])));
                for(const auto endpoint:{frame,std::min(frame+1,clip.frame_count-1)})
                    for(std::size_t row=0;row<3;++row)
                    {
                        double position{},absolute{};
                        for(std::size_t column=0;column<4;++column)
                        {
                            double matrix{},matrix_absolute{};
                            for(std::size_t lane=0;lane<4;++lane)
                            {
                                const auto &m=data.final_matrices[clip.first_transform+endpoint*character.bone_count+vertex.bone_indices[lane]];
                                const double term=double(m[row*4+column])*vertex.bone_weights[lane];
                                matrix+=term; matrix_absolute+=std::abs(term);
                            }
                            const double p=column==3?1.0:double(vertex.position[column]);
                            position+=matrix*p; absolute+=matrix_absolute*std::abs(p);
                        }
                        low[row]=std::min(low[row],position); high[row]=std::max(high[row],position);
                        magnitude[row]=std::max(magnitude[row],absolute);
                    }
            }
            for(std::size_t row=0;row<3;++row)
            {
                // Shader FTZ may drop a subnormal before a subsequent multiply
                // by a large source position; a relative gamma bound alone is
                // insufficient. Bound those absolute losses as well.
                const double margin=gamma*magnitude[row]+64*double(std::numeric_limits<float>::min())*(1+position_l1);
                const double pad=margin+data.bubble_amplitude;
                if(!std::isfinite(low[row]) || !std::isfinite(high[row]) ||
                    magnitude[row]>=std::numeric_limits<float>::max()/128 ||
                    std::max(std::abs(low[row]-pad),std::abs(high[row]+pad))>=std::numeric_limits<float>::max()/4)
                {
                    data.flags&=~rd::kMonsterBoundsPositionEnclosure;
                    bound.minimum[row]=-std::numeric_limits<float>::max();
                    bound.maximum[row]=std::numeric_limits<float>::max();
                    continue;
                }
                data.numerical_margin=std::max(data.numerical_margin,static_cast<float>(margin));
                bound.minimum[row]=std::nextafter(static_cast<float>(low[row]-pad),-std::numeric_limits<float>::infinity());
                bound.maximum[row]=std::nextafter(static_cast<float>(high[row]+pad),std::numeric_limits<float>::infinity());
            }
            data.interval_bounds.push_back(bound);
        }
    return data;
#endif
}

std::vector<CharacterMaterialSelection> PrepareCharacterMaterialSelections(
    const CharacterIR &character, std::string_view stem, const nlohmann::json &authoring)
{
    Require(authoring.is_object() && authoring.size() == 2 && authoring.at("version") == 1 &&
        authoring.at("characters").is_object() && authoring.at("characters").contains(stem),
        "missing authored character material table for " + std::string(stem));
    const auto &rules = authoring.at("characters").at(stem);
    Require(rules.is_array() && !rules.empty(), "expected material identity rules");
    Require(character.materials.size() == character.material_count, "imported material slot count mismatch");
    std::vector<CharacterMaterialSelection> result;
    for (std::size_t slot = 0; slot < character.materials.size(); ++slot)
    {
        const auto &material = character.materials[slot];
        const nlohmann::json *selected{};
        for (const auto &rule : rules)
        {
            Require(rule.is_object() && (rule.contains("name") != rule.contains("diffuse")) &&
                rule.contains("opaque") && (rule.size() == 2 ||
                (rule.size() == 3 && rule.contains("translucent"))), "invalid material identity rule");
            const auto key = rule.contains("name") ? "name" : "diffuse";
            Require(rule.at(key).is_string() && rule.at("opaque").is_string(), "material identity must be a string");
            const auto matches = rule.at(key) == (rule.contains("name") ? material.name : material.diffuse.filename().string());
            if (!matches) continue;
            Require(selected == nullptr, "ambiguous material identity for slot " + std::to_string(slot));
            selected = &rule;
        }
        Require(selected != nullptr, "unknown FBX material slot " + std::to_string(slot) +
            " name=" + material.name + " diffuse=" + material.diffuse.filename().string());
        const auto opaque = selected->at("opaque").get<std::string>();
        const auto opaque_ordinal = HeroOrdinal(opaque);
        Require(opaque != "slime_body", "opaque binding is not an authored opaque hero material");
        result.push_back({static_cast<std::uint16_t>(slot), opaque_ordinal, rd::HeroMaterialDomain::Opaque});
        if (selected->contains("translucent"))
        {
            Require(selected->at("translucent") == "slime_body" && opaque == "slime_features",
                "transparent jelly must have a matching opaque feature material");
            result.push_back({static_cast<std::uint16_t>(slot), HeroOrdinal("slime_body"), rd::HeroMaterialDomain::Translucent});
        }
        else Require(opaque != "slime_features", "jelly feature material is missing its transparent body");
    }
    return result;
}

std::vector<CharacterProduct> BuildCharacterProducts(
    const CharacterIR &character, std::string_view stem_view,
    std::span<const CharacterMaterialSelection> materials)
{
    const std::string stem(stem_view);
    Require(!stem.empty() && std::ranges::all_of(stem, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    }), "invalid character product stem");
    rd::ValidateCharacterSource(character, stem == "archer");
    const auto metadata_id = "render/character/" + stem + ".rpkg";
    const auto page_id = "render/geometry/character/" + stem + ".rpage";
    const auto animation_id = "render/animation/" + stem + ".rpkg";
    std::vector<std::string> dependencies{page_id};
    for (std::uint32_t slot = 0; slot < character.material_count; ++slot)
        for (const auto *semantic : {"diffuse", "normal"})
        {
            const auto name = stem + "_" + semantic + "_" + std::to_string(slot);
            dependencies.push_back("character/" + name + ".dds");
            dependencies.push_back("render/texture/" + name + ".rpkg");
        }
    bool monster_pbr{};
    for (const auto &binding : materials)
    {
        Require(binding.hero_ordinal < rd::kHeroMaterialNames.size(), "invalid hero material ordinal");
        const auto name = rd::kHeroMaterialNames[binding.hero_ordinal];
        dependencies.push_back("render/material/" + std::string(name) + "/material.rpkg");
        monster_pbr |= name == "monster_pbr";
    }
    if (monster_pbr)
        for (const auto *name : {"monster_basecolor", "monster_emissive", "monster_ram"})
            dependencies.push_back("render/texture/" + std::string(name) + ".rpkg");
    const auto &named = character.animation_extension;
    if (!named.clips.empty())
    {
        rd::ValidateCharacterAnimationBinding(named, character, MakeAssetId(metadata_id));
        dependencies.push_back(animation_id);
    }
    std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    Require(dependencies.size() < 65536u, "too many character dependencies");
    std::vector<rd::CharacterMaterialBinding> bindings;
    for (const auto &binding : materials)
    {
        const auto id = "render/material/" + std::string(rd::kHeroMaterialNames[binding.hero_ordinal]) + "/material.rpkg";
        bindings.push_back({binding.slot,
            static_cast<std::uint16_t>(std::ranges::find(dependencies, id) - dependencies.begin()),
            binding.hero_ordinal, binding.domain});
    }
    (void)rd::EncodeCharacterMaterialBindings(bindings, dependencies, character.material_count);

    std::vector<rd::ExpandedVertex> expanded;
    expanded.reserve(character.vertices.size());
    for (const auto &v : character.vertices)
        expanded.push_back({v.position, v.normal, v.bone_indices, v.bone_weights,
            v.uv, v.tangent, v.material_index, v.reserved});
    const auto geometry = geometry_content::CookGeometry(expanded, rd::kGeometryFlagSkinned);
    rd::GeometryMetadata metadata;
    metadata.flags = geometry.flags;
    metadata.vertex_count = static_cast<std::uint32_t>(geometry.vertices.size());
    metadata.index_count = static_cast<std::uint32_t>(geometry.indices.size());
    metadata.submesh_count = static_cast<std::uint32_t>(geometry.submeshes.size());
    metadata.meshlet_count = static_cast<std::uint32_t>(geometry.meshlets.size());
    metadata.vertex_ref_count = static_cast<std::uint32_t>(geometry.meshlet_vertex_refs.size());
    metadata.bounds = geometry.bounds;
    for (const auto &v : geometry.vertices)
        metadata.max_material_index = std::max(metadata.max_material_index, std::uint32_t(v.material_index));
    metadata.page_dependency_index = static_cast<std::uint16_t>(
        std::ranges::find(dependencies, page_id) - dependencies.begin());
    std::vector<CharacterProduct> products;
    std::optional<rd::MonsterCharacterData> monster;
    if(stem!="archer") monster=CookMonsterCharacterData(character,geometry,metadata,stem=="enemy_suicide");
    products.push_back({metadata_id, stem + ".rpkg", dependencies,
        rd::BuildCharacterMetadataPackage(MakeAssetId(metadata_id), dependencies, character, metadata, bindings,
            monster?&*monster:nullptr)});
    products.push_back({page_id, stem + ".rpage", {},
        rd::BuildGeometryPage(MakeAssetId(page_id), {}, geometry)});
    if (!named.clips.empty())
        products.push_back({animation_id, stem + "_animation.rpkg", {},
            rd::BuildCharacterAnimationExtension(MakeAssetId(animation_id), {}, named)});
    return products;
}

PreparedCharacterBinding PrepareCharacterBinding(
    std::string_view logical_id, std::span<const std::byte> metadata_bytes)
{
    const auto prefix = std::string_view("render/character/");
    Require(logical_id.starts_with(prefix) && logical_id.ends_with(".rpkg"), "invalid character identity");
    auto package = rd::ReadPackage(metadata_bytes, MakeAssetId(logical_id));
    auto character = rd::DecodeCharacterMetadataPackage(package, logical_id == "render/character/archer.rpkg");
    Require((logical_id == "render/character/archer.rpkg") == !character.monster,
        "character product has the wrong deformation representation; recook the character product");
    Require(std::ranges::none_of(package.dependencies, [](const std::string &id) {
        return id.ends_with(".meshbin") || id.ends_with("_materials.json");
    }), "legacy character exchange dependency");
    return {std::string(logical_id), std::move(package), std::move(character)};
}

void ValidateCharacterProductBindings(const PreparedCharacterBinding &prepared,
    std::span<const CharacterDependencyProduct> products)
{
    const std::string_view logical_id(prepared.logical_id);
    constexpr std::string_view prefix = "render/character/";
    const std::string stem(logical_id.substr(prefix.size(), logical_id.size() - prefix.size() - 5));
    const auto bytes = [&](std::string_view id) {
        const auto found = std::ranges::find(products, id, &CharacterDependencyProduct::logical_id);
        Require(found != products.end(), "missing checked product " + std::string(id));
        Require(std::ranges::count(products, id, &CharacterDependencyProduct::logical_id) == 1,
            "duplicate checked product " + std::string(id));
        return found->bytes;
    };
    const auto &package = prepared.package;
    const auto &character = prepared.character;
    const auto &page_id = package.dependencies[character.geometry.page_dependency_index];
    const auto page = bytes(page_id);
    Require(rd::ReadPackage(page, MakeAssetId(page_id)).dependencies.empty(),
        "direct character geometry must not depend on a legacy exchange");
    rd::ValidateCharacterGeometry(character, rd::DecodeGeometryPage(page, MakeAssetId(page_id), character.geometry));
    if (stem == "archer")
    {
        constexpr std::string_view animation_id = "render/animation/archer.rpkg";
        Require(std::ranges::find(package.dependencies, animation_id) != package.dependencies.end(),
            "Archer metadata is missing its named-animation dependency");
        const auto animation_bytes = bytes(animation_id);
        Require(rd::ReadPackage(animation_bytes, MakeAssetId(animation_id)).dependencies.empty(),
            "direct character animation must not depend on a legacy exchange");
        const auto animation = rd::DecodeCharacterAnimationExtension(animation_bytes, MakeAssetId(animation_id));
        rd::ValidateCharacterAnimationBinding(animation, character, MakeAssetId(logical_id));
        Require(animation.sample_rate_hz == 60 && animation.clips.size() == 11 && animation.sockets.empty(),
            "authored Archer animation extension shape mismatch");
    }
    bool monster_pbr{};
    for (const auto &binding : character.material_bindings)
    {
        const auto name = rd::kHeroMaterialNames[binding.hero_ordinal];
        const auto &id = package.dependencies[binding.dependency_index];
        const auto material = rd::ReadHeroMaterialPackage(bytes(id), name);
        Require(material.domain == binding.domain, "character material binding disagrees with the cooked closure domain");
        monster_pbr |= name == "monster_pbr";
    }
    const auto texture = [&](const std::string &id, const std::string &source) {
        Require(std::ranges::find(package.dependencies, id) != package.dependencies.end(),
            "missing character texture dependency " + id);
        const auto value = rd::ReadTexturePackage(bytes(id), MakeAssetId(id));
        std::uint32_t full_mips = 1;
        for (auto side = std::max(value.description.width, value.description.height); side > 1; side >>= 1) ++full_mips;
        const bool linear = source.find("_normal_") != std::string::npos || source == "character/monster_ram.dds";
        Require(value.description.array_size == 1 && value.description.mip_count == full_mips &&
            value.description.format == (linear ? rd::kTextureFormatBc7 : rd::kTextureFormatBc7Srgb) &&
            value.dependencies == std::vector<std::string>{source},
            "character texture dependency disagrees with its FBX slot");
    };
    for (std::uint32_t slot = 0; slot < character.material_count; ++slot)
        for (const auto *semantic : {"diffuse", "normal"})
        {
            const auto name = stem + "_" + semantic + "_" + std::to_string(slot);
            texture("render/texture/" + name + ".rpkg", "character/" + name + ".dds");
        }
    if (monster_pbr)
        for (const auto *name : {"monster_basecolor", "monster_emissive", "monster_ram"})
            texture("render/texture/" + std::string(name) + ".rpkg", "character/" + std::string(name) + ".dds");
}
} // namespace hs::content
