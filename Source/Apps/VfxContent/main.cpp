#include "../AssetPipeline/action.hpp"
#include "../Content/content_cooker.hpp"
#include "../Content/vfx_gradient_cooker.hpp"
#include "../Content/vfx_mesh_cooker.hpp"
#include "../Content/vfx_program_cooker.hpp"
#include "../Content/vfx_spec_validation.hpp"
#include "../Content/vfx_texture_validation.hpp"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace hs::content
{
bool WriteVfxMaskArray(const std::filesystem::path &path,
                       std::span<const ParticleSpriteSource> sprites,
                       const std::filesystem::path &source_textures,
                       std::string &error_message);
}

namespace
{
namespace fs = std::filesystem;
namespace ap = hs::asset_pipeline;
using Json = nlohmann::json;

[[noreturn]] void Fail(const std::string &message)
{
    throw std::runtime_error("hs_vfx_content: " + message);
}

void Require(bool condition, const std::string &message)
{
    if (!condition)
        Fail(message);
}

class ComScope
{
  public:
    ComScope()
    {
        const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Require(SUCCEEDED(result), "cannot initialize COM for VFX mask decoding: " +
                                     std::to_string(static_cast<unsigned long>(result)));
    }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;
    ~ComScope() { CoUninitialize(); }
};

std::span<const std::byte> Bytes(std::string_view text)
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

bool SafeFilename(std::string_view filename, std::string_view extension)
{
    if (filename.empty() || filename.front() == '.' || !filename.ends_with(extension) ||
        filename.find("..") != std::string_view::npos)
        return false;
    for (const unsigned char ch : filename)
        if (!std::isalnum(ch) && ch != '_' && ch != '-' && ch != '.')
            return false;
    return true;
}

Json ReadJson(const fs::path &path)
{
    return Json::parse(ap::ReadBytes(path));
}

std::vector<hs::content::ParticleSpriteSource> SpriteLayout(const Json &document,
                                                              std::string &projection)
{
    const auto &array = document.at("sprites");
    Require(array.is_array() && !array.empty() &&
                array.size() <= std::numeric_limits<hs::ParticleSprite>::max(),
            "invalid sprite layout");
    Json projected = Json::array();
    std::vector<hs::content::ParticleSpriteSource> sprites;
    std::set<std::string> ids;
    for (const auto &sprite : array)
    {
        Require(sprite.is_object() && sprite.size() == 3 &&
                    sprite.contains("id") && sprite.contains("file") && sprite.contains("frames"),
                "sprite layout requires exactly id, file, and frames");
        const auto id = sprite.at("id").get<std::string>();
        const auto file = sprite.at("file").get<std::string>();
        const auto &frames = sprite.at("frames");
        Require(!id.empty() && ids.insert(id).second && SafeFilename(file, ".png"),
                "invalid sprite ID or PNG filename");
        Require(frames.is_array() && frames.size() == 2,
                "sprite frames must have two dimensions");
        auto dimension = [](const Json &value) -> std::uint8_t {
            Require(value.is_number_integer() && value.get<std::int64_t>() > 0 &&
                        value.get<std::int64_t>() <= 255,
                    "sprite frame dimensions must be 1..255");
            return value.get<std::uint8_t>();
        };
        const auto columns = dimension(frames[0]);
        const auto rows = dimension(frames[1]);
        projected.push_back({{"id", id}, {"file", file},
                             {"frames", Json::array({columns, rows})}});
        sprites.push_back({id, file, columns, rows});
    }
    projection = projected.dump();
    return sprites;
}

struct CatalogTexture
{
    std::string filename;
    fs::path source;
};

std::vector<CatalogTexture> Catalog(const Json &spec, const fs::path &source_root)
{
    constexpr std::string_view prefix = "Content/Textures/VFX/";
    std::set<std::string> names;
    std::vector<CatalogTexture> textures;
    for (const auto &[id, texture] : spec.at("libraries").at("texture_catalog").items())
    {
        static_cast<void>(id);
        const auto asset = texture.at("runtime_asset").get<std::string>();
        Require(asset.starts_with(prefix), "invalid VFX catalog runtime asset path");
        const auto filename = asset.substr(prefix.size());
        Require(SafeFilename(filename, ".dds") && names.insert(filename).second,
                "invalid or duplicate VFX catalog DDS filename");
        textures.push_back({filename,
            source_root / "ContentSource" / "Textures" / "VFX" / filename});
    }
    Require(names.contains("vfx_gradient_lut.dds"), "gradient LUT is missing from VFX catalog");
    std::ranges::sort(textures, {}, &CatalogTexture::filename);
    return textures;
}

std::string ProductId(std::string_view filename)
{
    return "vfx/" + std::string(filename);
}

std::string TextureProductId(std::string_view filename)
{
    return "vfx/Content/Textures/VFX/" + std::string(filename);
}

void AddProduct(std::vector<ap::Product> &products, const fs::path &stage,
                std::string logical_id, std::string filename)
{
    products.push_back(ap::HashProduct(std::move(logical_id), filename, stage / filename));
}

bool Current(const fs::path &build_root, const ap::ActionSpec &spec,
             const std::vector<std::string> &expected)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path))
        return false;
    if (!fs::exists(result_path.parent_path() / "result.d"))
        return false;
    const auto result = ReadJson(result_path);
    if (result.at("schema_version") != 1 || result.at("action_id") != spec.action_id ||
        result.at("namespace_path") != spec.namespace_path ||
        result.at("action_key") != ap::ActionKey(spec))
        return false;
    const auto set_id = result.at("product_set_id").get<std::string>();
    Require(set_id.size() == 64 && std::ranges::all_of(set_id, [](unsigned char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
            }), "invalid current ProductSetId");
    const auto &products = result.at("products");
    if (!products.is_array() || products.size() != expected.size())
        return false;
    std::set<std::string> found;
    for (const auto &product : products)
    {
        const auto id = product.at("logical_id").get<std::string>();
        const auto locator = product.at("locator").get<std::string>();
        const auto filename = fs::path(locator).filename().string();
        if (!found.insert(id).second ||
            locator != (fs::path("Products") / spec.namespace_path / set_id / filename).generic_string())
            return false;
        const auto path = build_root / fs::path(locator);
        if (!fs::exists(path))
            return false;
        ap::RequireRegularFile(path);
        if (fs::file_size(path) != product.at("size").get<std::uint64_t>() ||
            ap::Sha256File(path) != product.at("digest").get<std::string>())
            return false;
    }
    return found == std::set<std::string>(expected.begin(), expected.end());
}

void Produce(std::string_view mode, const fs::path &source_root,
             const fs::path &build_root)
{
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source and build roots must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    ap::ActionSpec action;
    action.action_id = "vfx." + std::string(mode);
    action.recipe_id = "project-hs.vfx-" + std::string(mode);
    action.recipe_version = mode == "particle-definitions" ? 2 : 1;
    action.namespace_path = "vfx/" + std::string(mode);
    const auto particles = source_root / "ContentSource/GameData/particles.json";
    const auto texture_root = source_root / "ContentSource/Textures/VFX";
    const auto spec_path = source_root / "ContentSource/VFX/production_spec.v4.json";
    const auto mesh_path = source_root / "ContentSource/VFX/Meshes/vfx_mesh_atlas.json";
    std::vector<std::string> expected;
    std::vector<hs::content::ParticleSpriteSource> sprites;
    std::vector<CatalogTexture> textures;
    Json document;
    if (mode == "particle-definitions")
    {
        action.inputs.push_back(ap::HashInput("ContentSource/GameData/particles.json", particles));
        expected.push_back(ProductId("particle_effects.hsbin"));
    }
    else if (mode == "masks")
    {
        document = ReadJson(particles);
        std::string projection;
        sprites = SpriteLayout(document, projection);
        // The path remains a depfile prerequisite, but only the ordered layout
        // projection contributes to the expensive mask ActionKey.
        action.inputs.push_back({"ContentSource/GameData/particles.json#sprite_layout", particles,
                                 ap::Sha256Bytes(Bytes(projection))});
        std::set<std::string> files;
        for (const auto &sprite : sprites)
        {
            const auto filename = sprite.file.string();
            if (files.insert(filename).second)
                action.inputs.push_back(ap::HashInput(
                    "ContentSource/Textures/VFX/" + filename, texture_root / filename));
        }
        expected.push_back(ProductId("vfx_masks.dds"));
    }
    else if (mode == "vfx-runtime")
    {
        action.inputs.push_back(ap::HashInput("ContentSource/VFX/production_spec.v4.json", spec_path));
        document = ReadJson(spec_path);
        textures = Catalog(document, source_root);
        for (const auto &texture : textures)
        {
            action.inputs.push_back(ap::HashInput(
                "ContentSource/Textures/VFX/" + texture.filename, texture.source));
            expected.push_back(TextureProductId(texture.filename));
        }
        expected.push_back(ProductId("vfx_program.hsbin"));
    }
    else if (mode == "mesh-atlas")
    {
        action.inputs.push_back(ap::HashInput("ContentSource/VFX/Meshes/vfx_mesh_atlas.json", mesh_path));
        expected.push_back(ProductId("vfx_mesh_atlas.hsbin"));
    }
    else
        Fail("unknown VFX action mode");
    std::ranges::sort(action.inputs, {}, &ap::Input::logical_id);
    if (Current(build_root, action, expected))
    {
        ap::TouchActionResult(build_root / "Actions" / action.namespace_path / "result.json");
        std::cout << "vfx." << mode << " current action_key=" << ap::ActionKey(action) << '\n';
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / action.namespace_path);
    try
    {
        std::vector<ap::Product> products;
        std::string error;
        if (mode == "particle-definitions")
        {
            document = ReadJson(particles);
            const bool written = hs::content::WriteCookedParticleEffects(
                stage / "particle_effects.hsbin", document, sprites, error);
            Require(written, "particle definitions: " + error);
            AddProduct(products, stage, ProductId("particle_effects.hsbin"),
                       "particle_effects.hsbin");
        }
        else if (mode == "masks")
        {
            ComScope com;
            const bool written = hs::content::WriteVfxMaskArray(
                stage / "vfx_masks.dds", sprites, texture_root, error);
            Require(written, "VFX masks: " + error);
            AddProduct(products, stage, ProductId("vfx_masks.dds"), "vfx_masks.dds");
        }
        else if (mode == "vfx-runtime")
        {
            hs::content::ValidateVfxSpecification(document);
            hs::content::ValidateVfxTextures(document, source_root);
            for (const auto &texture : textures)
            {
                if (texture.filename == "vfx_gradient_lut.dds")
                    continue;
                const auto copied = ap::CopyAndHashInput(
                    "ContentSource/Textures/VFX/" + texture.filename, texture.source,
                    stage / texture.filename);
                const auto original = std::ranges::find(action.inputs, copied.logical_id,
                                                        &ap::Input::logical_id);
                Require(original != action.inputs.end() && copied.digest == original->digest,
                        "VFX source DDS changed during cooking");
            }
            const auto gradient = std::ranges::find(textures, std::string("vfx_gradient_lut.dds"),
                                                    &CatalogTexture::filename);
            const auto rows = hs::content::CookVfxGradientDefaults(
                document, gradient->source, stage / "vfx_gradient_lut.dds");
            hs::content::CookVfxProgram(document, rows, stage / "vfx_program.hsbin");
            for (const auto &texture : textures)
                AddProduct(products, stage, TextureProductId(texture.filename), texture.filename);
            AddProduct(products, stage, ProductId("vfx_program.hsbin"), "vfx_program.hsbin");
        }
        else
        {
            hs::content::CookVfxMeshAtlas(mesh_path, stage / "vfx_mesh_atlas.hsbin");
            AddProduct(products, stage, ProductId("vfx_mesh_atlas.hsbin"),
                       "vfx_mesh_atlas.hsbin");
        }
        ap::Publish(build_root, action, stage, std::move(products));
        std::cout << "vfx." << mode << " action_key=" << ap::ActionKey(action) << '\n';
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
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
            const std::wstring_view argument = argv[i];
            if ((argument == L"--particle-definitions" || argument == L"--masks" ||
                 argument == L"--vfx-runtime" || argument == L"--mesh-atlas") && mode.empty())
                mode.assign(argument.begin() + 2, argument.end());
            else if (argument == L"--source-root" && source_root.empty() && ++i < argc)
                source_root = fs::path(argv[i]);
            else if (argument == L"--build-root" && build_root.empty() && ++i < argc)
                build_root = fs::path(argv[i]);
            else
                Fail("usage: hs_vfx_content --particle-definitions|--masks|--vfx-runtime|--mesh-atlas --source-root <absolute> --build-root <absolute>");
        }
        Require(!mode.empty() && !source_root.empty() && !build_root.empty(),
                "usage: hs_vfx_content --particle-definitions|--masks|--vfx-runtime|--mesh-atlas --source-root <absolute> --build-root <absolute>");
        action_id = "vfx." + mode;
        Produce(mode, source_root.lexically_normal(), build_root.lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_vfx_content", error.what(),
                          {{"action_id", action_id}, {"mode", mode},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
