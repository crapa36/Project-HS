#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "../CharacterContent/character_sources.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/dds_texture.hpp>

#include <Windows.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;

struct ScopedComInitialization
{
    HRESULT result{CoInitializeEx(nullptr, COINIT_MULTITHREADED)};
    ~ScopedComInitialization()
    {
        if (SUCCEEDED(result)) CoUninitialize();
    }
};

struct Texture
{
    std::string name;
    rd::TextureDescription description;
};

void Require(bool valid, std::string_view reason)
{
    if (!valid) throw std::runtime_error("hs_render_character_texture_package: " +
                                         std::string(reason));
}

constexpr std::array<std::string_view, 8> kCharacterModes{
    "archer", "slime-melee", "slime-ranged", "slime-suicide",
    "slime-projectile", "boss-5m", "boss-10m", "boss-final"};

std::vector<Texture> Textures(std::string_view mode)
{
    std::vector<Texture> result;
    if (mode == "monster-materials")
    {
        for (const auto *name : {"monster_basecolor", "monster_emissive", "monster_ram"})
            result.push_back({name, {512, 512, 1, 10,
                std::string_view(name) == "monster_ram" ? rd::kTextureFormatBc7 :
                    rd::kTextureFormatBc7Srgb}});
        return result;
    }
    std::string_view stem;
    std::uint32_t size = 1, mips = 1, count = 1;
    if (mode == "archer") { stem = "archer"; size = 2048; mips = 12; count = 5; }
    else if (mode == "slime-melee") { stem = "enemy_melee"; size = 1024; mips = 11; }
    else if (mode == "slime-ranged") { stem = "enemy_ranged"; size = 1024; mips = 11; }
    else if (mode == "slime-suicide") { stem = "enemy_suicide"; size = 1024; mips = 11; }
    else if (mode == "slime-projectile") { stem = "enemy_gel_projectile"; size = 1024; mips = 11; }
    else if (mode == "boss-5m") stem = "boss_5m";
    else if (mode == "boss-10m") stem = "boss_10m";
    else if (mode == "boss-final") stem = "boss_final";
    else Require(false, "unknown mode");
    for (std::uint32_t index = 0; index < count; ++index)
    {
        const auto suffix = std::to_string(index);
        result.push_back({std::string(stem) + "_diffuse_" + suffix,
                          {size, size, 1, mips, rd::kTextureFormatBc7Srgb}});
        result.push_back({std::string(stem) + "_normal_" + suffix,
                          {size, size, 1, mips, rd::kTextureFormatBc7}});
    }
    return result;
}

std::span<const std::byte> DdsPayload(std::span<const std::byte> bytes,
                                      const rd::TextureDescription &expected)
{
    const auto decoded = rd::DecodeDdsTexture(bytes);
    rd::ValidateDdsTextureRole(decoded, rd::DdsTextureRole::CharacterFinalBc7, expected);
    return decoded.payload;
}

std::string SourceId(const Texture &texture)
{
    return "character/" + texture.name + ".dds";
}

std::string ProductId(const Texture &texture)
{
    return "render/texture/" + texture.name + ".rpkg";
}

bool Current(const fs::path &build_root, const ap::ActionSpec &spec,
             const std::vector<Texture> &textures)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try
    {
        const ap::ActionResultSnapshot result(build_root, spec.namespace_path);
        if (result.ActionId() != spec.action_id ||
            result.ActionKeyValue() != ap::ActionKey(spec) ||
            result.ProductCount() != textures.size() * 2) return false;
        for (const auto &texture : textures)
        {
            ap::UpstreamProduct ignored;
            fs::path dds_path;
            result.ReadProduct(SourceId(texture), ignored, dds_path);
            const auto contents = result.ReadProductBytes(ProductId(texture), ignored);
            const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
            const auto package = rd::ReadTexturePackage(bytes, hs::MakeAssetId(ProductId(texture)));
            const auto &actual = package.description;
            if (actual.width != texture.description.width ||
                actual.height != texture.description.height ||
                actual.array_size != 1 || actual.mip_count != texture.description.mip_count ||
                actual.format != texture.description.format ||
                package.dependencies != std::vector<std::string>{SourceId(texture)})
                return false;
        }
        return true;
    }
    catch (const std::exception &) { return false; }
}

void Produce(std::string_view mode, const fs::path &source_root,
             const fs::path &build_root)
{
    Require(source_root.is_absolute(), "source root must be absolute");
    Require(build_root.is_absolute(), "build root must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    const auto textures = Textures(mode);
    ap::ActionSpec spec;
    spec.action_id = "render.texture.character." + std::string(mode);
    spec.recipe_id = "project-hs.render.texture.character.bc7-semantic-v1." +
        hs::content::CharacterTextureCookRecipeIdentity();
    spec.recipe_version = 4;
    spec.namespace_path = "render/texture/character/" + std::string(mode);

    hs::content::CharacterAssetSource source;
    if (mode == "monster-materials")
    {
        const auto texture_root = source_root / "ContentSource/Textures/Monsters/PBR";
        for (const auto *name : {"BasecolorDefault_TEX.png", "Emissive_TEX.png", "RAM_TEX.png"})
            hs::content::AddCharacterInput(spec, source_root, texture_root / name);
    }
    else
    {
        const auto found = std::ranges::find(kCharacterModes, mode);
        Require(found != kCharacterModes.end(), "unknown mode");
        source = hs::content::CharacterAssetSources(source_root)
            [static_cast<std::size_t>(found - kCharacterModes.begin())];
        hs::content::AddCharacterModelInputs(spec, source_root, source);
    }
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);

    if (Current(build_root, spec, textures))
    {
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto snapshot = hs::content::SnapshotActionInputs(stage, spec.inputs);
        std::string error;
        if (mode == "monster-materials")
        {
            const auto source_directory = snapshot.RemapDirectory(
                source_root, source_root / "ContentSource/Textures/Monsters/PBR");
            if (!hs::content::CookMonsterMaterialTextures(stage, error, source_directory))
                throw std::runtime_error(error);
        }
        else
        {
            auto staged_source = source;
            staged_source.model = snapshot.Remap(source_root, source.model);
            hs::content::CharacterIR character;
            if (!hs::content::ImportCharacterMaterials(character, error, staged_source))
                throw std::runtime_error(error);
            if (!hs::content::WriteCharacterTextures(stage, character, staged_source, error))
                throw std::runtime_error(error);
            Require(character.materials.size() * 2 == textures.size(),
                    "character material count does not match texture product contract");
        }

        std::error_code remove_error;
        fs::remove_all(snapshot.root, remove_error);
        Require(!remove_error, "cannot remove private character source snapshot: " +
                remove_error.message());

        std::vector<ap::Product> products;
        products.reserve(textures.size() * 2);
        for (const auto &texture : textures)
        {
            const auto dds_filename = texture.name + ".dds";
            const auto dds_path = stage / dds_filename;
            const auto dds = ap::ReadBytes(dds_path);
            const auto dds_bytes = std::as_bytes(std::span(dds.data(), dds.size()));
            const auto payload = DdsPayload(dds_bytes, texture.description);
            const std::array dependencies{SourceId(texture)};
            const auto package = rd::BuildTexturePackage(
                hs::MakeAssetId(ProductId(texture)), dependencies,
                texture.description, payload);
            const auto decoded = rd::ReadTexturePackage(package, hs::MakeAssetId(ProductId(texture)));
            Require(decoded.dependencies == std::vector<std::string>{dependencies[0]} &&
                    decoded.subresources.size() == texture.description.mip_count,
                    "package self-validation failed");

            products.push_back(ap::HashProduct(SourceId(texture), dds_filename, dds_path));
            const auto package_filename = texture.name + ".rpkg";
            const auto package_path = stage / package_filename;
            ap::WriteBytes(package_path, package);
            auto product = ap::HashProduct(ProductId(texture), package_filename, package_path);
            product.dependencies.assign(dependencies.begin(), dependencies.end());
            products.push_back(std::move(product));
        }
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
    fs::path source_root, build_root;
    std::string mode;
    ap::Json action_id = nullptr;
    try
    {
        const ScopedComInitialization com;
        Require(SUCCEEDED(com.result), "COM initialization failed");
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
                Require(std::ranges::all_of(value, [](wchar_t ch) {
                    return (ch >= L'a' && ch <= L'z') || ch == L'-' ||
                           (ch >= L'0' && ch <= L'9');
                }), "invalid mode token");
                for (const auto ch : value) mode.push_back(static_cast<char>(ch));
            }
            else Require(false, "invalid command line");
        }
        Require(!source_root.empty() && !build_root.empty() && !mode.empty(),
                "usage: hs_render_character_texture_package --mode <mode> --source-root <absolute> --build-root <absolute>");
        if (mode == "monster-materials" || std::ranges::find(kCharacterModes, mode) != kCharacterModes.end())
            action_id = "render.texture.character." + mode;
        Produce(mode, source_root.lexically_normal(), build_root.lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_character_texture_package", error.what(),
                          {{"action_id", action_id}, {"mode", mode},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
