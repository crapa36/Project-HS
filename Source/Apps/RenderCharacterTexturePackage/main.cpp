#include "../AssetPipeline/action.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/texture_package.hpp>
#include <hs/render_data/dds_texture.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;

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
            result.ProductCount() != textures.size()) return false;
        for (const auto &texture : textures)
        {
            ap::UpstreamProduct ignored;
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

void Produce(std::string_view mode, const fs::path &build_root)
{
    Require(build_root.is_absolute(), "build root must be absolute");
    ap::MakeDirectory(build_root);
    const auto textures = Textures(mode);
    ap::ActionSpec spec;
    spec.action_id = "render.texture.character." + std::string(mode);
    spec.recipe_id = "project-hs.render.texture.character.bc7-semantic-v1";
    spec.recipe_version = 3;
    spec.namespace_path = "render/texture/character/" + std::string(mode);
    const ap::ActionResultSnapshot source_result(build_root, "character/" + std::string(mode));
    std::vector<std::string> source_bytes;
    source_bytes.reserve(textures.size());
    for (const auto &texture : textures)
    {
        ap::UpstreamProduct upstream;
        source_bytes.push_back(source_result.ReadProductBytes(SourceId(texture), upstream));
        spec.upstream.push_back(upstream);
    }
    std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
    if (Current(build_root, spec, textures))
    {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        return;
    }
    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        std::vector<ap::Product> products;
        for (std::size_t i = 0; i < textures.size(); ++i)
        {
            const auto &texture = textures[i];
            const auto &dds = source_bytes[i];
            const auto bytes = std::as_bytes(std::span(dds.data(), dds.size()));
            const auto payload = DdsPayload(bytes, texture.description);
            const std::array dependencies{SourceId(texture)};
            const auto package = rd::BuildTexturePackage(
                hs::MakeAssetId(ProductId(texture)), dependencies,
                texture.description, payload);
            const auto decoded = rd::ReadTexturePackage(package, hs::MakeAssetId(ProductId(texture)));
            Require(decoded.dependencies == std::vector<std::string>{dependencies[0]} &&
                    decoded.subresources.size() == texture.description.mip_count,
                    "package self-validation failed");
            const auto filename = texture.name + ".rpkg";
            const auto output = stage / filename;
            ap::WriteBytes(output, package);
            auto product = ap::HashProduct(ProductId(texture), filename, output);
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
    fs::path build_root;
    std::string mode;
    ap::Json action_id = nullptr;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--build-root" && build_root.empty() && ++i < argc)
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
        Require(!build_root.empty() && !mode.empty(),
                "usage: hs_render_character_texture_package --mode <mode> --build-root <absolute>");
        if (mode == "monster-materials" || mode == "archer" || mode == "slime-melee" ||
            mode == "slime-ranged" || mode == "slime-suicide" || mode == "slime-projectile" ||
            mode == "boss-5m" || mode == "boss-10m" || mode == "boss-final")
            action_id = "render.texture.character." + mode;
        Produce(mode, build_root.lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_character_texture_package", error.what(),
                          {{"action_id", action_id}, {"mode", mode},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
