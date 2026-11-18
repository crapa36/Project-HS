#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "character_sources.hpp"
#include "../Content/character_products.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
namespace fs = std::filesystem;
namespace ap = hs::asset_pipeline;

void Require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error("hs_character_content: " + message);
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
        Require(rule.is_object() && (rule.contains("material") != rule.contains("materials")),
            "character rule must select material identity or identities");
        const auto append_ordinal = [&](const std::string &name) {
            const auto found = std::ranges::find(hs::render_data::kCharacterMaterialNames, name);
            Require(found != hs::render_data::kCharacterMaterialNames.end(), "unknown selected Hero material " + name);
            ordinals.push_back(static_cast<std::uint32_t>(found - hs::render_data::kCharacterMaterialNames.begin()));
        };
        if (rule.contains("material"))
        {
            Require(rule.at("material").is_string(), "character material identity must be a string");
            append_ordinal(rule.at("material").get<std::string>());
        }
        else
        {
            const auto &materials = rule.at("materials");
            Require(materials.is_array() && !materials.empty() && materials.size() <= 2,
                "character material identity list must contain one or two materials");
            for (const auto &material : materials)
            {
                Require(material.is_string(), "character material identity must be a string");
                append_ordinal(material.get<std::string>());
            }
        }
    }
    std::ranges::sort(ordinals);
    ordinals.erase(std::unique(ordinals.begin(), ordinals.end()), ordinals.end());
    std::vector<hs::content::CharacterMaterialDomainSelection> result;
    result.reserve(ordinals.size());
    for (const auto ordinal : ordinals)
    {
        const auto name = hs::render_data::kCharacterMaterialNames[ordinal];
        const std::string namespace_path = "render/shader/" + std::string(name);
        const ap::ActionResultSnapshot producer(build_root, namespace_path);
        Require(producer.ActionId() == "render.shader." + std::string(name),
            "Hero material producer identity mismatch for " + std::string(name));
        const auto material_id = "render/material/" + std::string(name) + "/material.rpkg";
        ap::UpstreamProduct upstream;
        const auto bytes = producer.ReadProductBytes(material_id, upstream);
        const auto package = hs::render_data::ReadCharacterMaterialPackage(
            std::as_bytes(std::span(bytes.data(), bytes.size())), name);
        result.push_back({ordinal, package.domain, package.surface});
        spec.upstream.push_back(std::move(upstream));
    }
    return result;
}

std::vector<std::string> ProductNames(const std::string &output_name,
                                      bool named_animations,
                                      std::size_t reduced_lods = 0)
{
    std::vector<std::string> names{output_name + ".rpkg", output_name + ".rpage"};
    for(std::size_t lod=1;lod<=reduced_lods;++lod)
        names.push_back(output_name+"_lod"+std::to_string(lod)+".rpage");
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
    throw std::runtime_error("unsupported character product name: " + name);
}

bool IsCurrent(const fs::path &build_root, const ap::ActionSpec &spec,
               const hs::content::CharacterAssetSource &source)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try
    {
        const ap::ActionResultSnapshot snapshot(build_root, spec.namespace_path);
        if (snapshot.ActionId() != spec.action_id ||
            snapshot.ActionKeyValue() != ap::ActionKey(spec) ||
            snapshot.ProductCount() == 0)
            return false;
        const auto product_ids = snapshot.ProductIds();
        std::unordered_set<std::string> actual_names;
        std::string contents, page, animation;
        std::unordered_map<std::string, std::string> lod_pages;
        bool has_metadata=false, has_page=false, has_animation=false;
        for (const auto product_id : product_ids)
        {
            const auto &product = snapshot.ProductMetadata(product_id);
            const auto name = fs::path(product.at("locator").get<std::string>())
                                  .filename().string();
            if (product_id != ProductId(name))
                return false;
            if (!actual_names.insert(name).second)
                return false;
            ap::UpstreamProduct dependency;
            if (name == source.output_name + ".rpkg")
            { contents=snapshot.ReadProductBytes(product_id,dependency); has_metadata=true; }
            else if (name == source.output_name + ".rpage")
            { page=snapshot.ReadProductBytes(product_id,dependency); has_page=true; }
            else if (name == source.output_name + "_animation.rpkg")
            { animation=snapshot.ReadProductBytes(product_id,dependency); has_animation=true; }
            else if (name.starts_with(source.output_name + "_lod") && name.ends_with(".rpage"))
            { lod_pages.emplace(std::string(product_id), snapshot.ReadProductBytes(product_id,dependency)); }
            else
            { fs::path product_path; snapshot.ReadProduct(product_id, dependency, product_path); }
        }
        if (!has_metadata || !has_page) return false;
        const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
        const auto id = "render/character/" + source.output_name + ".rpkg";
        const auto prepared = hs::content::PrepareCharacterBinding(id, bytes);
        const auto &metadata = prepared.character;
        const auto page_bytes = std::as_bytes(std::span(page.data(), page.size()));
        const auto page_id = "render/geometry/character/" + source.output_name + ".rpage";
        const auto prepared_page=hs::render_data::PrepareGeometryPage(page_bytes,hs::MakeAssetId(page_id),metadata.geometry);
        if (!prepared_page.package.dependencies.empty()) return false;
        hs::render_data::ValidateCharacterGeometry(metadata,prepared_page);
        if(metadata.monster)
            for(std::size_t lod=1;lod<metadata.monster->lods.size();++lod)
            {
                const auto &entry=metadata.monster->lods[lod];
                const auto &lod_id=prepared.package.dependencies[entry.page_dependency_index];
                std::string other_content;
                const std::string *content = nullptr;
                if (lod_id == page_id) content = &page;
                else if (const auto found = lod_pages.find(lod_id); found != lod_pages.end())
                    content = &found->second;
                else
                {
                    ap::UpstreamProduct dependency;
                    other_content = snapshot.ReadProductBytes(lod_id,dependency);
                    content = &other_content;
                }
                const auto lod_bytes=std::as_bytes(std::span(content->data(),content->size()));
                const auto prepared_lod=hs::render_data::PrepareGeometryPage(lod_bytes,hs::MakeAssetId(lod_id),entry);
                if(!prepared_lod.package.dependencies.empty()) return false;
                hs::render_data::ValidateCharacterGeometry(metadata,entry,prepared_lod);
            }
        if (!source.named_animations.empty())
        {
            if (!has_animation) return false;
            const auto named = hs::render_data::DecodeCharacterAnimationExtension(
                std::as_bytes(std::span(animation.data(), animation.size())),
                hs::MakeAssetId("render/animation/" + source.output_name + ".rpkg"));
            hs::render_data::ValidateCharacterAnimationBinding(named, metadata, hs::MakeAssetId(id));
            if (named.sample_rate_hz != 60 || named.clips.size() != source.named_animations.size() || !named.sockets.empty()) return false;
        }
        const auto expected = ProductNames(source.output_name,
                                           !source.named_animations.empty(),
                                           metadata.monster?metadata.monster->lods.size()-1:0);
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
    Require(found != modes.end(), "unsupported action mode");

    ap::ActionSpec spec;
    spec.action_id = "character." + std::string(mode);
    spec.recipe_id = "project-hs.character." + std::string(mode) + ".geometry";
    spec.recipe_version = 9;
    spec.namespace_path = "character/" + std::string(mode);

    const auto character_source = hs::content::CharacterAssetSources(source_root)
        [static_cast<std::size_t>(found - modes.begin())];
    ap::Json material_authoring;
    std::vector<hs::content::CharacterMaterialDomainSelection> material_domains;
    hs::content::AddCharacterInputs(spec, source_root, character_source);
    const auto authoring_path = source_root / "ContentSource/Materials/hero_bindings.json";
    const auto authoring_bytes = ap::ReadBytes(authoring_path);
    spec.inputs.push_back({hs::content::RelativeCharacterInputId(source_root, authoring_path),
        authoring_path, ap::Sha256Bytes(std::as_bytes(std::span(authoring_bytes.data(), authoring_bytes.size())))});
    material_authoring = ap::Json::parse(authoring_bytes);
    material_domains = ReadCharacterMaterialDomains(build_root, character_source.output_name,
        material_authoring, spec);
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);

    if (IsCurrent(build_root, spec, character_source))
    {
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        std::string error_message;
        std::vector<std::string> names;
        std::vector<hs::content::CharacterProduct> final_products;
        const auto source_snapshot = hs::content::SnapshotActionInputs(stage, spec.inputs);
        hs::content::CharacterIR character;
        const auto staged_character_source = hs::content::SnapshotCharacterSource(
            source_root, source_snapshot, character_source);
        if (!hs::content::ImportCharacterAsset(character, error_message, staged_character_source))
            throw std::runtime_error(error_message);
        const auto materials = hs::content::PrepareCharacterMaterialSelections(
            character, character_source.output_name, material_authoring);
        final_products = hs::content::BuildCharacterProducts(character, character_source.output_name,
            materials, material_domains);
        for (const auto &product : final_products)
            ap::WriteBytes(stage / product.filename, product.bytes);
        names = ProductNames(character_source.output_name,
                             !character_source.named_animations.empty(),
                             std::ranges::count_if(final_products,[&](const auto &product) {
                                 return product.filename.starts_with(character_source.output_name+"_lod") &&
                                     product.filename.ends_with(".rpage");
                             }));
        std::error_code remove_error;
        fs::remove_all(source_snapshot.root, remove_error);
        Require(!remove_error, "cannot remove private character source snapshot: " +
                remove_error.message());
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
                     arg == L"--boss-10m" || arg == L"--boss-final")
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
