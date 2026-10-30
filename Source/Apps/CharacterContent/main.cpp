#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "character_sources.hpp"
#include "../Content/character_products.hpp"

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
namespace fs = std::filesystem;
namespace ap = hs::asset_pipeline;

struct ScopedComInitialization
{
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~ScopedComInitialization()
    {
        if (SUCCEEDED(result)) CoUninitialize();
    }
};

void Require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error("hs_character_content: " + message);
}

std::string RelativeId(const fs::path &source_root, const fs::path &path)
{
    const auto relative = path.lexically_relative(source_root);
    Require(!relative.empty() && !relative.is_absolute() &&
            relative.begin() != relative.end() && *relative.begin() != "..",
            "input escapes source root: " + path.string());
    return relative.generic_string();
}

void AddInput(ap::ActionSpec &spec, const fs::path &source_root,
              const fs::path &path)
{
    spec.inputs.push_back(ap::HashInput(RelativeId(source_root, path), path));
}

void AddCharacterInputs(ap::ActionSpec &spec, const fs::path &source_root,
                        const hs::content::CharacterAssetSource &source)
{
    AddInput(spec, source_root, source.model);
    for (const auto &clip : source.animations)
        AddInput(spec, source_root, clip);
    for (const auto &clip : source.named_animations)
        if (std::ranges::find(source.animations, clip.file) == source.animations.end())
            AddInput(spec, source_root, clip.file);

    const auto fbm = source.model.parent_path() /
        (source.model.stem().string() + ".fbm");
    if (fs::exists(fbm))
    {
        ap::RequireDirectory(fbm);
        std::vector<fs::path> textures;
        for (const auto &entry : fs::directory_iterator(fbm))
        {
            ap::RequireRegularFile(entry.path());
            textures.push_back(entry.path());
        }
        std::ranges::sort(textures);
        for (const auto &texture : textures)
            AddInput(spec, source_root, texture);
    }
}

std::vector<hs::content::CharacterMaterialDomainSelection> ReadCharacterMaterialDomains(
    const fs::path &build_root, std::string_view stem, const ap::Json &authoring,
    ap::ActionSpec &spec)
{
    const auto &rules = authoring.at("characters").at(stem);
    Require(rules.is_array(), "character material rules must be an array");
    std::vector<std::uint32_t> ordinals;
    for (const auto &rule : rules)
    {
        Require(rule.is_object() && rule.contains("material") && rule.at("material").is_string(),
            "character rule must select one material identity");
        const auto name = rule.at("material").get<std::string>();
        const auto found = std::ranges::find(hs::render_data::kHeroMaterialNames, name);
        Require(found != hs::render_data::kHeroMaterialNames.end(), "unknown selected Hero material " + name);
        ordinals.push_back(static_cast<std::uint32_t>(found - hs::render_data::kHeroMaterialNames.begin()));
    }
    std::ranges::sort(ordinals);
    ordinals.erase(std::unique(ordinals.begin(), ordinals.end()), ordinals.end());
    std::vector<hs::content::CharacterMaterialDomainSelection> result;
    result.reserve(ordinals.size());
    for (const auto ordinal : ordinals)
    {
        const auto name = hs::render_data::kHeroMaterialNames[ordinal];
        const std::string namespace_path = "render/shader/" + std::string(name);
        const ap::ActionResultSnapshot producer(build_root, namespace_path);
        Require(producer.ActionId() == "render.shader." + std::string(name),
            "Hero material producer identity mismatch for " + std::string(name));
        const auto material_id = "render/material/" + std::string(name) + "/material.rpkg";
        ap::UpstreamProduct upstream;
        const auto bytes = producer.ReadProductBytes(material_id, upstream);
        const auto package = hs::render_data::ReadHeroMaterialPackage(
            std::as_bytes(std::span(bytes.data(), bytes.size())), name);
        result.push_back({ordinal, package.domain});
        spec.upstream.push_back(std::move(upstream));
    }
    return result;
}

std::vector<std::string> ProductNames(const std::string &output_name,
                                      std::size_t material_count, bool named_animations,
                                      std::size_t reduced_lods = 0)
{
    std::vector<std::string> names{output_name + ".rpkg", output_name + ".rpage"};
    for(std::size_t lod=1;lod<=reduced_lods;++lod)
        names.push_back(output_name+"_lod"+std::to_string(lod)+".rpage");
    for (std::size_t index = 0; index < material_count; ++index)
    {
        const auto suffix = std::to_string(index) + ".dds";
        names.push_back(output_name + "_diffuse_" + suffix);
        names.push_back(output_name + "_normal_" + suffix);
    }
    if (named_animations) names.push_back(output_name + "_animation.rpkg");
    return names;
}

std::string ProductId(const std::string &name)
{
    constexpr std::string_view suffix = "_animation.rpkg";
    if (name.ends_with(suffix))
        return "render/animation/" + name.substr(0, name.size() - suffix.size()) + ".rpkg";
    if (name.ends_with(".rpage"))
        return "render/geometry/character/" + name;
    if (name.ends_with(".rpkg"))
        return "render/character/" + name;
    return "character/" + name;
}

bool IsCurrent(const fs::path &build_root, const ap::ActionSpec &spec,
               const hs::content::CharacterAssetSource *source)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try
    {
        const auto result = ap::Json::parse(ap::ReadBytes(result_path));
        if (result.at("action_id") != spec.action_id ||
            result.at("action_key") != ap::ActionKey(spec) ||
            result.at("namespace_path") != spec.namespace_path ||
            !result.at("products").is_array())
            return false;
        const auto &products = result.at("products");
        if (products.empty()) return false;
        std::unordered_set<std::string> actual_names;
        fs::path metadata_path, page_path, animation_path;
        for (const auto &product : products)
        {
            const auto name = fs::path(product.at("locator").get<std::string>())
                                  .filename().string();
            if (product.at("logical_id") != ProductId(name))
                return false;
            if (!actual_names.insert(name).second)
                return false;
            ap::UpstreamProduct dependency;
            fs::path product_path;
            ap::ReadActionResult(build_root, spec.namespace_path,
                                 ProductId(name), dependency, product_path);
            if (source && name == source->output_name + ".rpkg") metadata_path = product_path;
            if (source && name == source->output_name + ".rpage") page_path = product_path;
            if (source && name == source->output_name + "_animation.rpkg") animation_path = product_path;
        }
        std::vector<std::string> expected;
        if (source)
        {
            if (metadata_path.empty() || page_path.empty()) return false;
            const auto contents = ap::ReadBytes(metadata_path);
            const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
            const auto id = "render/character/" + source->output_name + ".rpkg";
            const auto prepared = hs::content::PrepareCharacterBinding(id, bytes);
            const auto &metadata = prepared.character;
            const auto page = ap::ReadBytes(page_path);
            const auto page_bytes = std::as_bytes(std::span(page.data(), page.size()));
            const auto page_id = "render/geometry/character/" + source->output_name + ".rpage";
            if (!hs::render_data::ReadPackage(page_bytes, hs::MakeAssetId(page_id)).dependencies.empty()) return false;
            hs::render_data::ValidateCharacterGeometry(metadata,
                hs::render_data::DecodeGeometryPage(page_bytes, hs::MakeAssetId(page_id), metadata.geometry));
            if(metadata.monster)
                for(std::size_t lod=1;lod<metadata.monster->lods.size();++lod)
                {
                    const auto &entry=metadata.monster->lods[lod];
                    const auto &lod_id=prepared.package.dependencies[entry.page_dependency_index];
                    ap::UpstreamProduct dependency;
                    fs::path lod_path;
                    ap::ReadActionResult(build_root,spec.namespace_path,lod_id,dependency,lod_path);
                    const auto content=ap::ReadBytes(lod_path);
                    const auto lod_bytes=std::as_bytes(std::span(content.data(),content.size()));
                    if(!hs::render_data::ReadPackage(lod_bytes,hs::MakeAssetId(lod_id)).dependencies.empty()) return false;
                    auto view=metadata;view.geometry=entry;
                    hs::render_data::ValidateCharacterGeometry(view,
                        hs::render_data::DecodeGeometryPage(lod_bytes,hs::MakeAssetId(lod_id),entry));
                }
            if (!source->named_animations.empty())
            {
                if (animation_path.empty()) return false;
                const auto animation = ap::ReadBytes(animation_path);
                const auto named = hs::render_data::DecodeCharacterAnimationExtension(
                    std::as_bytes(std::span(animation.data(), animation.size())),
                    hs::MakeAssetId("render/animation/" + source->output_name + ".rpkg"));
                hs::render_data::ValidateCharacterAnimationBinding(named, metadata, hs::MakeAssetId(id));
                if (named.sample_rate_hz != 60 || named.clips.size() != source->named_animations.size() || !named.sockets.empty()) return false;
            }
            expected = ProductNames(source->output_name, metadata.material_count,
                                    !source->named_animations.empty(),
                                    metadata.monster?metadata.monster->lods.size()-1:0);
        }
        else
            expected = {"monster_basecolor.dds", "monster_emissive.dds",
                        "monster_ram.dds"};
        if (actual_names.size() != expected.size()) return false;
        return std::ranges::all_of(expected, [&](const std::string &name) {
            return actual_names.contains(name);
        });
    }
    catch (const std::exception &)
    {
        return false;
    }
}

void Produce(std::string_view mode, const fs::path &source_root,
             const fs::path &build_root)
{
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source and build roots must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);

    constexpr std::array<std::string_view, 8> modes{
        "archer", "slime-melee", "slime-ranged", "slime-suicide",
        "slime-projectile", "boss-5m", "boss-10m", "boss-final"};
    const auto found = std::ranges::find(modes, mode);
    const bool monster_materials = mode == "monster-materials";
    Require(monster_materials || found != modes.end(), "unsupported action mode");

    ap::ActionSpec spec;
    spec.action_id = "character." + std::string(mode);
    spec.recipe_id = "project-hs.character." + std::string(mode) + "." +
        hs::content::CharacterTextureCookRecipeIdentity();
    spec.recipe_version = 7;
    spec.namespace_path = "character/" + std::string(mode);

    hs::content::CharacterAssetSource character_source;
    ap::Json material_authoring;
    std::vector<hs::content::CharacterMaterialDomainSelection> material_domains;
    if (monster_materials)
    {
        const auto textures = source_root / "ContentSource/Textures/Monsters/PBR";
        for (const auto *name : {"BasecolorDefault_TEX.png", "Emissive_TEX.png",
                                 "RAM_TEX.png"})
            AddInput(spec, source_root, textures / name);
    }
    else
    {
        character_source = hs::content::CharacterAssetSources(source_root)
            [static_cast<std::size_t>(found - modes.begin())];
        AddCharacterInputs(spec, source_root, character_source);
        const auto authoring_path = source_root / "ContentSource/Materials/hero_bindings.json";
        AddInput(spec, source_root, authoring_path);
        material_authoring = ap::Json::parse(ap::ReadBytes(authoring_path));
        material_domains = ReadCharacterMaterialDomains(build_root, character_source.output_name,
            material_authoring, spec);
    }
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);

    if (IsCurrent(build_root, spec, monster_materials ? nullptr : &character_source))
    {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        std::string error_message;
        std::vector<std::string> names;
        std::vector<hs::content::CharacterProduct> final_products;
        if (monster_materials)
        {
            if (!hs::content::CookMonsterMaterialTextures(
                    stage, error_message,
                    source_root / "ContentSource/Textures/Monsters/PBR"))
                throw std::runtime_error(error_message);
            names = {"monster_basecolor.dds", "monster_emissive.dds", "monster_ram.dds"};
        }
        else
        {
            hs::content::CharacterIR character;
            if (!hs::content::ImportCharacterAsset(character, error_message, character_source))
                throw std::runtime_error(error_message);
            if (!hs::content::WriteCharacterTextures(stage, character, character_source, error_message))
                throw std::runtime_error(error_message);
            const auto materials = hs::content::PrepareCharacterMaterialSelections(
                character, character_source.output_name, material_authoring);
            final_products = hs::content::BuildCharacterProducts(character, character_source.output_name,
                materials, material_domains);
            for (const auto &product : final_products)
                ap::WriteBytes(stage / product.filename, product.bytes);
            names = ProductNames(character_source.output_name, character.materials.size(),
                                 !character_source.named_animations.empty(),
                                 std::ranges::count_if(final_products,[&](const auto &product) {
                                     return product.filename.starts_with(character_source.output_name+"_lod") &&
                                         product.filename.ends_with(".rpage");
                                 }));
        }
        std::vector<ap::Product> products;
        for (const auto &name : names)
        {
            auto product = ap::HashProduct(ProductId(name), name, stage / name);
            const auto final = std::ranges::find(final_products, name, &hs::content::CharacterProduct::filename);
            if (final != final_products.end()) product.dependencies = final->dependencies;
            products.push_back(std::move(product));
        }
        const auto staged_count = std::distance(fs::directory_iterator(stage),
                                                fs::directory_iterator{});
        Require(staged_count == static_cast<std::ptrdiff_t>(names.size()),
                "producer wrote unexpected staged files");
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (...)
    {
        ap::RemoveStageAfterFailure(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    std::string mode;
    ap::Json action_id = nullptr;
    fs::path source_root;
    fs::path build_root;
    try
    {
        const ScopedComInitialization com;
        Require(SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE,
                "COM initialization failed");
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--source-root" && source_root.empty() && ++i < argc)
                source_root = fs::path(argv[i]);
            else if (arg == L"--build-root" && build_root.empty() && ++i < argc)
                build_root = fs::path(argv[i]);
            else if (arg == L"--archer" || arg == L"--slime-melee" ||
                     arg == L"--slime-ranged" || arg == L"--slime-suicide" ||
                     arg == L"--slime-projectile" || arg == L"--boss-5m" ||
                     arg == L"--boss-10m" || arg == L"--boss-final" ||
                     arg == L"--monster-materials")
            {
                Require(mode.empty(), "select exactly one action mode");
                mode.assign(arg.begin() + 2, arg.end());
            }
            else
                throw std::runtime_error("hs_character_content: invalid command line");
        }
        Require(!mode.empty() && !source_root.empty() && !build_root.empty(),
                "usage: hs_character_content --<mode> --source-root <absolute> --build-root <absolute>");
        action_id = "character." + mode;
        Produce(mode, source_root.lexically_normal(), build_root.lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_character_content", error.what(),
                          {{"action_id", action_id}, {"mode", mode},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
