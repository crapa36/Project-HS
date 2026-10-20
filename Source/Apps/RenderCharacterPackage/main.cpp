#include "../AssetPipeline/action.hpp"
#include "../Content/geometry_cooker.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/character_animation_extension.hpp>
#include <hs/render_data/texture_package.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;

struct CharacterName { std::string_view mode, stem; };
constexpr std::array kCharacters{
    CharacterName{"archer", "archer"},
    CharacterName{"slime-melee", "enemy_melee"},
    CharacterName{"slime-ranged", "enemy_ranged"},
    CharacterName{"slime-suicide", "enemy_suicide"},
    CharacterName{"slime-projectile", "enemy_gel_projectile"},
    CharacterName{"boss-5m", "boss_5m"},
    CharacterName{"boss-10m", "boss_10m"},
    CharacterName{"boss-final", "boss_final"}};

static_assert(std::endian::native == std::endian::little &&
              sizeof(hs::SkinnedVertex) == 76 &&
              sizeof(hs::CharacterClipHeader) == 16 &&
              sizeof(hs::CharacterLocalTransform) == 48 &&
              sizeof(hs::CharacterAssetHeader) == 84);

void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("hs_render_character_package: " + std::string(message));
}

rd::CharacterPackageData ReadLegacy(std::string_view file)
{
    Require(file.size() >= sizeof(hs::CharacterAssetHeader), "truncated v6 header");
    hs::CharacterAssetHeader header;
    std::memcpy(&header, file.data(), sizeof(header));
    const auto vertices_end = std::uint64_t(sizeof(header)) +
        std::uint64_t(header.vertex_count) * sizeof(hs::SkinnedVertex);
    const auto clips_end = vertices_end +
        std::uint64_t(header.clip_count) * sizeof(hs::CharacterClipHeader);
    const auto parents_end = clips_end +
        std::uint64_t(header.bone_count) * sizeof(std::uint16_t);
    const auto inverse_end = parents_end +
        std::uint64_t(header.bone_count) * sizeof(std::array<float, 16>);
    const auto mask_end = inverse_end +
        std::uint64_t(header.bone_count) * sizeof(float);
    Require(header.magic == hs::CharacterAssetHeader{}.magic &&
            header.version == hs::kCharacterAssetVersion &&
            header.vertex_count > 0 && header.vertex_count <= 3'000'000 &&
            header.bone_count > 0 && header.bone_count <= hs::kMaxCharacterBones &&
            header.material_count > 0 &&
            header.material_count <= hs::kMaxCharacterMaterials &&
            header.clip_count == static_cast<std::uint32_t>(hs::CharacterAnimationClip::Count) &&
            header.vertices_offset == sizeof(header) &&
            header.clips_offset == vertices_end &&
            header.parents_offset == clips_end &&
            header.inverse_bind_matrices_offset == parents_end &&
            header.upper_body_weights_offset == inverse_end &&
            header.transforms_offset == mask_end &&
            mask_end <= file.size() &&
            (file.size() - mask_end) % sizeof(hs::CharacterLocalTransform) == 0 &&
            std::uint64_t(sizeof(header)) + header.payload_size == file.size() &&
            std::isfinite(header.bounds_min[1]) &&
            std::isfinite(header.bounds_max[1]) &&
            header.bounds_min[1] < header.bounds_max[1],
            "invalid v6 character layout");
    const auto payload = std::as_bytes(std::span(file.data() + sizeof(header),
                                                 header.payload_size));
    Require(hs::Crc32(payload) == header.payload_crc32, "invalid v6 payload CRC");

    rd::CharacterPackageData result;
    result.bone_count = header.bone_count;
    result.material_count = header.material_count;
    result.ground_offset = -header.bounds_min[1];
    result.vertices.resize(header.vertex_count);
    std::memcpy(result.vertices.data(), file.data() + header.vertices_offset,
                result.vertices.size() * sizeof(result.vertices.front()));
    result.clips.resize(header.clip_count);
    std::memcpy(result.clips.data(), file.data() + header.clips_offset,
                result.clips.size() * sizeof(result.clips.front()));
    result.parents.resize(header.bone_count);
    std::memcpy(result.parents.data(), file.data() + header.parents_offset,
                result.parents.size() * sizeof(result.parents.front()));
    result.inverse_bind_matrices.resize(header.bone_count);
    std::memcpy(result.inverse_bind_matrices.data(),
                file.data() + header.inverse_bind_matrices_offset,
                result.inverse_bind_matrices.size() * sizeof(result.inverse_bind_matrices.front()));
    result.upper_body_weights.resize(header.bone_count);
    std::memcpy(result.upper_body_weights.data(),
                file.data() + header.upper_body_weights_offset,
                result.upper_body_weights.size() * sizeof(float));
    result.transforms.resize((file.size() - mask_end) / sizeof(hs::CharacterLocalTransform));
    std::memcpy(result.transforms.data(), file.data() + header.transforms_offset,
                result.transforms.size() * sizeof(result.transforms.front()));
    return result;
}

std::vector<std::string> Dependencies(std::string_view stem,
                                      std::uint32_t material_count)
{
    std::vector<std::string> ids{"character/" + std::string(stem) + ".meshbin"};
    for (std::uint32_t i = 0; i < material_count; ++i)
    {
        ids.push_back("character/" + std::string(stem) + "_diffuse_" +
                      std::to_string(i) + ".dds");
        ids.push_back("character/" + std::string(stem) + "_normal_" +
                      std::to_string(i) + ".dds");
    }
    std::ranges::sort(ids);
    return ids;
}

bool Current(const fs::path &build_root, const ap::ActionSpec &spec,
             const std::string &logical_id, const std::vector<std::string> &dependencies,
             const std::string &page_id,const std::string &mesh_id,
             const std::vector<rd::CharacterMaterialBinding> &bindings)
{
    const auto path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(path) || !fs::exists(path.parent_path() / "result.d")) return false;
    try
    {
        const auto result = ap::Json::parse(ap::ReadBytes(path));
        if (result.at("action_id") != spec.action_id ||
            result.at("action_key") != ap::ActionKey(spec) ||
            result.at("products").size() != 2)
            return false;
        const auto found=std::ranges::find_if(result.at("products"),[&](const ap::Json&p){return p.at("logical_id")==logical_id;});
        if(found==result.at("products").end() || found->at("dependencies")!=dependencies)return false;
        ap::UpstreamProduct ignored;
        fs::path product_path;
        (void)ap::ReadActionResult(build_root, spec.namespace_path, logical_id,
                                   ignored, product_path);
        const auto product = ap::ReadBytes(product_path);
        const auto bytes = std::as_bytes(std::span(product.data(), product.size()));
        const auto package = rd::ReadPackage(bytes, hs::MakeAssetId(logical_id));
        const auto metadata=rd::DecodeCharacterMetadataPackage(bytes, hs::MakeAssetId(logical_id),
                                          logical_id == "render/character/archer.rpkg");
        fs::path page_path;(void)ap::ReadActionResult(build_root,spec.namespace_path,page_id,ignored,page_path);
        const auto page=ap::ReadBytes(page_path);const auto page_bytes=std::as_bytes(std::span(page.data(),page.size()));
        if(rd::ReadPackage(page_bytes,hs::MakeAssetId(page_id)).dependencies!=std::vector<std::string>{mesh_id})return false;
        const auto geometry=rd::DecodeGeometryPage(page_bytes,hs::MakeAssetId(page_id),metadata.geometry);rd::ValidateCharacterGeometry(metadata,geometry);
        return package.dependencies == dependencies && package.chunks.size() == 8 &&
               metadata.material_bindings == bindings;
    }
    catch (const std::exception &) { return false; }
}

void Produce(std::string_view mode, const fs::path &source_root,
             const fs::path &build_root)
{
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source/build roots must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    const auto found = std::ranges::find_if(kCharacters, [&](const CharacterName &candidate) {
        return candidate.mode == mode;
    });
    Require(found != kCharacters.end(), "unknown character mode");
    const auto mesh_id = "character/" + std::string(found->stem) + ".meshbin";
    ap::UpstreamProduct mesh;
    fs::path mesh_path;
    (void)ap::ReadActionResult(build_root, "character/" + std::string(mode),
                               mesh_id, mesh, mesh_path);
    const auto source = ReadLegacy(ap::ReadBytes(mesh_path));
    auto dependencies = Dependencies(found->stem, source.material_count);
    // All listed DDS products must exist, but their digests are deliberately
    // absent from the package ActionKey: pixels do not affect this mesh package.
    for (const auto &id : dependencies)
    {
        ap::UpstreamProduct ignored;
        fs::path ignored_path;
        (void)ap::ReadActionResult(build_root, "character/" + std::string(mode),
                                   id, ignored, ignored_path);
    }
    ap::UpstreamProduct animation_upstream;
    std::optional<rd::CharacterAnimationExtensionData> animation;
    if (mode == "archer")
    {
        constexpr std::string_view animation_id = "render/animation/archer.rpkg";
        fs::path animation_path;
        (void)ap::ReadActionResult(build_root, "character/archer", animation_id,
                                   animation_upstream, animation_path);
        const auto contents = ap::ReadBytes(animation_path);
        const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
        Require(rd::ReadPackage(bytes, hs::MakeAssetId(animation_id)).dependencies ==
                std::vector<std::string>{mesh_id}, "animation extension source dependency mismatch");
        animation = rd::DecodeCharacterAnimationExtension(bytes, hs::MakeAssetId(animation_id));
        rd::CharacterMetadataPackageData skeleton;
        skeleton.bone_count = source.bone_count;
        skeleton.parents = source.parents;
        skeleton.inverse_bind_matrices = source.inverse_bind_matrices;
        rd::ValidateCharacterAnimationBinding(*animation, skeleton,
            hs::MakeAssetId("render/character/archer.rpkg"));
        Require(animation->sample_rate_hz == 60 && animation->clips.size() == 11 &&
                animation->sockets.empty(), "authored Archer animation extension shape mismatch");
        dependencies.emplace_back(animation_id);
    }
    ap::UpstreamProduct binding_source;
    fs::path binding_path;
    const auto binding_id = "character/" + std::string(found->stem) + "_materials.json";
    (void)ap::ReadActionResult(build_root, "character/" + std::string(mode), binding_id,
                               binding_source, binding_path);
    const auto authored = ap::Json::parse(ap::ReadBytes(binding_path));
    Require(authored.is_object() && authored.size() == 3 && authored.at("version") == 1 &&
            authored.at("character").is_string() &&
            authored.at("character").get<std::string>() == found->stem && authored.at("slots").is_array() &&
            authored.at("slots").size() == source.material_count, "invalid character material slot authoring");
    struct PendingBinding { std::uint16_t slot; std::string name; rd::HeroMaterialDomain domain; };
    std::vector<PendingBinding> pending_bindings;
    std::unordered_set<std::uint32_t> seen_slots;
    for (const auto &slot : authored.at("slots"))
    {
        Require(slot.is_object() && (slot.size() == 5 || (slot.size() == 6 && slot.contains("translucent"))) &&
                slot.at("slot").is_number_unsigned() && slot.at("name").is_string() &&
                slot.at("source_diffuse").is_string() && slot.at("source_normal").is_string() &&
                slot.at("opaque").is_string(), "invalid gathered FBX material binding");
        const auto index = slot.at("slot").get<std::uint32_t>();
        Require(index < source.material_count && seen_slots.insert(index).second, "duplicate or invalid FBX material slot");
        pending_bindings.push_back({static_cast<std::uint16_t>(index), slot.at("opaque").get<std::string>(), rd::HeroMaterialDomain::Opaque});
        if (slot.contains("translucent"))
        {
            Require(slot.at("translucent").is_string(), "invalid transparent material identity");
            pending_bindings.push_back({static_cast<std::uint16_t>(index), slot.at("translucent").get<std::string>(), rd::HeroMaterialDomain::Translucent});
        }
    }
    dependencies.push_back(binding_id);
    for (const auto &binding : pending_bindings)
    {
        Require(std::ranges::find(rd::kHeroMaterialNames, binding.name) != rd::kHeroMaterialNames.end(),
                "unknown authored hero material: " + binding.name);
        dependencies.push_back("render/material/" + binding.name + "/material.rpkg");
    }
    for (std::uint32_t slot = 0; slot < source.material_count; ++slot)
        for (const auto *semantic : {"diffuse", "normal"})
        {
            const auto id = "render/texture/" + std::string(found->stem) + "_" + semantic + "_" + std::to_string(slot) + ".rpkg";
            ap::UpstreamProduct ignored;
            fs::path texture_path;
            (void)ap::ReadActionResult(build_root, "render/texture/character/" + std::string(mode),
                                       id, ignored, texture_path);
            const auto texture_bytes = ap::ReadBytes(texture_path);
            const auto texture = rd::ReadTexturePackage(std::as_bytes(std::span(texture_bytes.data(), texture_bytes.size())),
                hs::MakeAssetId(id));
            Require(texture.description.array_size == 1 &&
                    texture.dependencies == std::vector<std::string>{"character/" + std::string(found->stem) + "_" + semantic + "_" + std::to_string(slot) + ".dds"},
                    "character texture dependency disagrees with its FBX slot");
            dependencies.push_back(id);
        }
    if (std::ranges::any_of(pending_bindings, [](const auto &binding) { return binding.name == "monster_pbr"; }))
        for (const auto *name : {"monster_basecolor", "monster_emissive", "monster_ram"})
        {
            const auto id = "render/texture/" + std::string(name) + ".rpkg";
            ap::UpstreamProduct ignored;
            fs::path texture_path;
            (void)ap::ReadActionResult(build_root, "render/texture/character/monster-materials", id, ignored, texture_path);
            const auto texture_bytes = ap::ReadBytes(texture_path);
            const auto texture = rd::ReadTexturePackage(std::as_bytes(std::span(texture_bytes.data(), texture_bytes.size())),
                hs::MakeAssetId(id));
            Require(texture.description.array_size == 1 &&
                    texture.dependencies == std::vector<std::string>{"character/" + std::string(name) + ".dds"},
                    "monster sampled-context texture has an invalid source dependency");
            dependencies.push_back(id);
        }
    ap::ActionSpec spec;
    spec.action_id = "render.character." + std::string(mode);
    spec.recipe_id = "project-hs.render.character";
    spec.recipe_version = 4;
    spec.namespace_path = "render/character/" + std::string(mode);
    spec.upstream.push_back(mesh);
    spec.upstream.push_back(binding_source);
    if (animation) spec.upstream.push_back(animation_upstream);
    std::unordered_set<std::string> material_ids;
    for (const auto &binding : pending_bindings)
    {
        const auto id = "render/material/" + binding.name + "/material.rpkg";
        if (!material_ids.insert(id).second) continue;
        ap::UpstreamProduct material;
        fs::path material_path;
        (void)ap::ReadActionResult(build_root, "render/shader/" + binding.name, id, material, material_path);
        const auto material_bytes = ap::ReadBytes(material_path);
        const auto material_package = rd::ReadHeroMaterialPackage(
            std::as_bytes(std::span(material_bytes.data(), material_bytes.size())), binding.name);
        Require(material_package.domain == binding.domain,
                "character material binding disagrees with the cooked closure domain");
        spec.upstream.push_back(std::move(material));
    }
    std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
    const std::string logical_id = "render/character/" + std::string(found->stem) + ".rpkg";
    const std::string page_id="render/geometry/character/"+std::string(found->stem)+".rpage";
    dependencies.push_back(page_id);std::ranges::sort(dependencies);
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    Require(dependencies.size() < 65536u, "too many character material dependencies");
    std::vector<rd::CharacterMaterialBinding> bindings;
    for (const auto &pending : pending_bindings)
    {
        const auto ordinal = std::ranges::find(rd::kHeroMaterialNames, pending.name);
        Require(ordinal != rd::kHeroMaterialNames.end(), "unknown authored hero material: " + pending.name);
        const auto id = "render/material/" + pending.name + "/material.rpkg";
        const auto dependency = std::ranges::find(dependencies, id);
        bindings.push_back({pending.slot, static_cast<std::uint16_t>(dependency - dependencies.begin()),
            static_cast<std::uint32_t>(ordinal - rd::kHeroMaterialNames.begin()), pending.domain});
    }
    (void)rd::EncodeCharacterMaterialBindings(bindings, dependencies, source.material_count);
    if (Current(build_root, spec, logical_id, dependencies,page_id,mesh_id,bindings))
    {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }
    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        std::vector<rd::ExpandedVertex>expanded;for(const auto&v:source.vertices)expanded.push_back({v.position,v.normal,v.bone_indices,v.bone_weights,v.uv,v.tangent,v.material_index,v.reserved});
        const auto geometry=hs::geometry_content::CookGeometry(expanded,rd::kGeometryFlagSkinned);
        rd::GeometryMetadata gm;gm.flags=geometry.flags;gm.vertex_count=static_cast<std::uint32_t>(geometry.vertices.size());gm.index_count=static_cast<std::uint32_t>(geometry.indices.size());gm.submesh_count=static_cast<std::uint32_t>(geometry.submeshes.size());gm.meshlet_count=static_cast<std::uint32_t>(geometry.meshlets.size());gm.vertex_ref_count=static_cast<std::uint32_t>(geometry.meshlet_vertex_refs.size());gm.bounds=geometry.bounds;for(const auto&v:geometry.vertices)gm.max_material_index=std::max(gm.max_material_index,std::uint32_t(v.material_index));gm.page_dependency_index=static_cast<std::uint16_t>(std::ranges::find(dependencies,page_id)-dependencies.begin());
        (void)rd::EncodeGeometryMetadata(gm,dependencies,logical_id);
        const auto package = rd::BuildCharacterMetadataPackage(hs::MakeAssetId(logical_id),
                                                       dependencies, source,gm,bindings);
        const auto verified = rd::DecodeCharacterMetadataPackage(
            package, hs::MakeAssetId(logical_id), mode == "archer");
        if (animation)
            rd::ValidateCharacterAnimationBinding(*animation, verified, hs::MakeAssetId(logical_id));
        Require(verified.geometry.index_count == source.vertices.size() &&
                verified.transforms.size() == source.transforms.size(),
                "package self-validation failed");
        const std::vector<std::string>page_dependencies{mesh_id};const auto page=rd::BuildGeometryPage(hs::MakeAssetId(page_id),page_dependencies,geometry);
        rd::ValidateCharacterGeometry(verified,rd::DecodeGeometryPage(page,hs::MakeAssetId(page_id),gm));
        const auto filename = std::string(found->stem) + ".rpkg";
        const auto output = stage / filename;
        ap::WriteBytes(output, package);
        auto product = ap::HashProduct(logical_id, filename, output);
        product.dependencies = dependencies;
        const auto page_filename=std::string(found->stem)+".rpage";const auto page_output=stage/page_filename;ap::WriteBytes(page_output,page);auto page_product=ap::HashProduct(page_id,page_filename,page_output);page_product.dependencies=page_dependencies;
        std::cout<<logical_id<<" expanded_bytes="<<source.vertices.size()*76<<" indexed_bytes="<<geometry.vertices.size()*76+geometry.indices.size()*4<<" page_bytes="<<page.size()<<" metadata_bytes="<<package.size()<<'\n';
        ap::Publish(build_root, spec, stage, {std::move(product),std::move(page_product)});
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    fs::path source_root, build_root;
    std::string mode;
    ap::Json action_id = nullptr;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--source-root" && source_root.empty() && ++i < argc)
                source_root = fs::path(argv[i]);
            else if (arg == L"--build-root" && build_root.empty() && ++i < argc)
                build_root = fs::path(argv[i]);
            else if (arg == L"--mode" && mode.empty() && ++i < argc)
            {
                const std::wstring_view value = argv[i];
                Require(std::ranges::all_of(value, [](wchar_t character) {
                    return (character >= L'a' && character <= L'z') ||
                           character == L'-' || (character >= L'0' && character <= L'9');
                }), "invalid mode token");
                for (const wchar_t character : value)
                    mode.push_back(static_cast<char>(character));
            }
            else
                throw std::runtime_error("hs_render_character_package: invalid command line");
        }
        Require(!mode.empty() && !source_root.empty() && !build_root.empty(),
                "usage: hs_render_character_package --mode <mode> --source-root <absolute> --build-root <absolute>");
        if (std::ranges::find(kCharacters, mode, &CharacterName::mode) != kCharacters.end())
            action_id = "render.character." + mode;
        Produce(mode, source_root.lexically_normal(), build_root.lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_character_package", error.what(),
                          {{"action_id", action_id}, {"mode", mode},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
