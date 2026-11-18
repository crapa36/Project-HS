#include "../AssetPipeline/action.hpp"
#include "../Content/particle_effect_cooker.hpp"
#include "../Content/vfx_gradient_cooker.hpp"
#include "../Content/vfx_mesh_cooker.hpp"
#include "../Content/vfx_program_cooker.hpp"
#include "../Content/vfx_spec_validation.hpp"
#include "../Content/vfx_texture_validation.hpp"

#include <hs/core/cooked_particle_effects.hpp>

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
    action.recipe_version = mode == "particle-definitions" || mode == "vfx-runtime" ? 2 : 1;
    action.namespace_path = "vfx/" + std::string(mode);
    const auto particles = source_root / "ContentSource/GameData/particles.json";
    const auto texture_root = source_root / "ContentSource/Textures/VFX";
    const auto spec_path = source_root / "ContentSource/VFX/production_spec.v4.json";
    const auto mesh_path = source_root / "ContentSource/VFX/Meshes/vfx_mesh_atlas.json";
    std::vector<std::string> expected;
    std::vector<std::string> mask_filenames;
    std::map<std::string, std::string> mask_inputs;
    std::map<std::string, const Json *> textures;
    Json document;
    std::string mesh_bytes;
    if (mode == "particle-definitions")
    {
        const auto bytes = ap::ReadBytes(particles);
        action.inputs.push_back({"ContentSource/GameData/particles.json", particles, ap::Sha256Bytes(Bytes(bytes))});
        document = Json::parse(bytes);
        expected.push_back(ProductId("particle_effects.hsbin"));
    }
    else if (mode == "masks")
    {
        document = ReadJson(particles);
        const auto &sprites = document.at("sprites");
        Require(sprites.is_array() && !sprites.empty() &&
                    sprites.size() <= std::numeric_limits<hs::ParticleSprite>::max(),
                "invalid mask slice count");
        for (const auto &sprite : sprites)
        {
            const auto filename = sprite.at("file").get<std::string>();
            Require(SafeFilename(filename, ".png"), "invalid mask filename");
            mask_filenames.push_back(filename);
            auto [input, inserted] = mask_inputs.try_emplace(filename);
            if (!inserted) continue;
            const auto path = texture_root / filename;
            input->second = ap::ReadBytes(path);
            action.inputs.push_back({"ContentSource/Textures/VFX/" + filename, path,
                                     ap::Sha256Bytes(Bytes(input->second))});
        }
        action.inputs.push_back({"ContentSource/GameData/particles.json#mask_slices", particles,
                                 ap::Sha256Bytes(Bytes(Json(mask_filenames).dump()))});
        expected.push_back(ProductId("vfx_masks.dds"));
    }
    else if (mode == "vfx-runtime")
    {
        const auto bytes = ap::ReadBytes(spec_path);
        action.inputs.push_back({"ContentSource/VFX/production_spec.v4.json", spec_path, ap::Sha256Bytes(Bytes(bytes))});
        document = Json::parse(bytes);
        constexpr std::string_view prefix = "Content/Textures/VFX/";
        for (const auto &texture : document.at("libraries").at("texture_catalog"))
        {
            const auto &asset = texture.at("runtime_asset").get_ref<const std::string &>();
            Require(asset.starts_with(prefix), "invalid VFX catalog runtime asset path");
            const auto filename = asset.substr(prefix.size());
            Require(SafeFilename(filename, ".dds") && textures.emplace(filename, &texture).second,
                    "invalid or duplicate VFX catalog DDS filename");
            action.inputs.push_back(ap::HashInput("ContentSource/Textures/VFX/" + filename,
                                                  texture_root / filename));
            expected.push_back(TextureProductId(filename));
        }
        Require(textures.contains("vfx_gradient_lut.dds"), "gradient LUT is missing from VFX catalog");
        expected.push_back(ProductId("vfx_program.hsbin"));
    }
    else if (mode == "mesh-atlas")
    {
        mesh_bytes = ap::ReadBytes(mesh_path);
        action.inputs.push_back({"ContentSource/VFX/Meshes/vfx_mesh_atlas.json", mesh_path, ap::Sha256Bytes(Bytes(mesh_bytes))});
        expected.push_back(ProductId("vfx_mesh_atlas.hsbin"));
    }
    else
        Fail("unknown VFX action mode");
    std::ranges::sort(action.inputs, {}, &ap::Input::logical_id);
    if (Current(build_root, action, expected))
    {
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / action.namespace_path);
    try
    {
        std::vector<ap::Product> products;
        std::string error;
        if (mode == "particle-definitions")
        {
            hs::content::WriteCookedParticleEffects(stage / "particle_effects.hsbin", document);
            AddProduct(products, stage, ProductId("particle_effects.hsbin"),
                       "particle_effects.hsbin");
        }
        else if (mode == "masks")
        {
            ComScope com;
            const bool written = hs::content::WriteVfxMaskArray(
                stage / "vfx_masks.dds", mask_filenames, mask_inputs, error);
            Require(written, "VFX masks: " + error);
            AddProduct(products, stage, ProductId("vfx_masks.dds"), "vfx_masks.dds");
        }
        else if (mode == "vfx-runtime")
        {
            hs::content::ValidateVfxSpecification(document);
            constexpr std::string_view gradient = "vfx_gradient_lut.dds";
            for (const auto &[filename, description] : textures)
            {
                const auto staged = stage / filename;
                const auto copied = ap::CopyAndHashInput(
                    "ContentSource/Textures/VFX/" + filename, texture_root / filename, staged);
                const auto original = std::ranges::find(action.inputs, copied.logical_id,
                                                        &ap::Input::logical_id);
                Require(original != action.inputs.end() && copied.digest == original->digest,
                        "VFX source DDS changed during cooking");
                hs::content::ValidateVfxTexture(*description, staged);
                if (filename != gradient)
                    products.push_back({TextureProductId(filename), filename,
                                        copied.digest, fs::file_size(staged), {}});
            }
            const auto rows = hs::content::CookVfxGradientDefaults(document, stage / gradient);
            hs::content::CookVfxProgram(document, rows, stage / "vfx_program.hsbin");
            AddProduct(products, stage, TextureProductId(gradient), std::string(gradient));
            AddProduct(products, stage, ProductId("vfx_program.hsbin"), "vfx_program.hsbin");
        }
        else
        {
            hs::content::CookVfxMeshAtlas(mesh_bytes, stage / "vfx_mesh_atlas.hsbin");
            AddProduct(products, stage, ProductId("vfx_mesh_atlas.hsbin"),
                       "vfx_mesh_atlas.hsbin");
        }
        ap::Publish(build_root, action, stage, std::move(products));
    }
    catch (...)
    {
        ap::RemoveStageAfterFailure(stage);
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
