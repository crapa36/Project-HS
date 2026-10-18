#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../AssetPipeline/action.hpp"
#include "../Content/environment_histogram_cooker.hpp"
#include "../Content/environment_mesh_cooker.hpp"
#include "../Content/environment_texture_cooker.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
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

void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error("hs_environment: " + std::string(message));
}

std::vector<std::string> MeshProductNames()
{
    constexpr std::array names{"trunk_0", "trunk_1", "trunk_2", "leaf_0", "leaf_1",
        "leaf_2", "rock_0", "rock_1", "rock_2", "rock_3", "grass_0", "grass_1",
        "grass_2", "grass_3", "flower_daisy", "flower_buttercup", "flower_violet", "clover_patch",
    "ground_leaves", "twig_cluster", "fence_short",
    "shrub_low", "stump_roots", "crate_intact", "crate_broken", "barrel_intact", "barrel_broken", "cart_broken", "meadow_ground", "meadow_water"};
    std::vector<std::string> products;
    for (const auto* name : names) for (const auto* lod : {"", "_lod1", "_lod2"})
        products.push_back("environment_" + std::string(name) + lod + ".meshbin");
    return products;
}

Microsoft::WRL::ComPtr<ID3D11Device> HardwareCompressor()
{
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    const HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, levels, 1, D3D11_SDK_VERSION, device.GetAddressOf(),
        nullptr, nullptr);
    if (FAILED(result))
        std::cout << "Environment BC7: D3D11 hardware unavailable (HRESULT " << result
                  << "); using CPU compressor\n";
    else
        std::cout << "Environment BC7: D3D11 GPU compressor\n";
    return device;
}

std::string EncoderIdentity(ID3D11Device *device)
{
    if (!device) return ".cpu-bc7";
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    LARGE_INTEGER driver{};
    Require(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
            SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&description)) &&
            SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)),
            "GPU compressor adapter/driver identity is unavailable");
    // The actual compression device supplies this identity; adapter enumeration
    // order and machine-specific LUIDs do not participate in content identity.
    return ".d3d11-bc7." + std::to_string(description.VendorId) + "." +
           std::to_string(description.DeviceId) + "." +
           std::to_string(description.SubSysId) + "." +
           std::to_string(description.Revision) + "." +
           std::to_string(static_cast<std::uint64_t>(driver.QuadPart));
}

void AddInputs(ap::ActionSpec& spec, const fs::path& source_root,
               const std::vector<fs::path>& paths)
{
    for (const auto& path : paths) {
        const auto relative = path.lexically_relative(source_root);
        Require(!relative.empty() && !relative.is_absolute() &&
                relative.begin() != relative.end() && *relative.begin() != "..",
                "input escapes source root");
        spec.inputs.push_back(ap::HashInput(relative.generic_string(), path));
    }
}

bool IsCurrent(const fs::path& build_root, const ap::ActionSpec& spec,
               const std::vector<std::string>& names,
               ap::ProductReadPolicy product_read_policy = ap::ProductReadPolicy::VerifyIntegrity)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    const auto result = ap::Json::parse(ap::ReadBytes(result_path));
    if (result.at("action_id") != spec.action_id ||
        result.at("recipe_id") != spec.recipe_id ||
        result.at("recipe_version") != spec.recipe_version ||
        result.at("action_key") != ap::ActionKey(spec) ||
        !result.at("products").is_array() || result.at("products").size() != names.size())
        return false;
    for (const auto& name : names) {
        ap::UpstreamProduct dependency;
        fs::path product_path;
        try {
            ap::ReadActionResult(build_root, spec.namespace_path,
                                 "environment/" + name, dependency, product_path,
                                 product_read_policy);
        } catch (const std::exception&) {
            return false;
        }
    }
    return true;
}

void Produce(std::string_view mode, const fs::path& source_root,
             const fs::path& build_root, const std::string& group = {},
             const std::string& role = {})
{
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source and build roots must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);

    const auto texture_source = source_root / "ContentSource/Textures/Environment";
    const auto mesh_source = source_root /
        "ContentSource/Models/Environment/environment_meshes.json";
    ap::ActionSpec spec;
    spec.action_id = "environment." + std::string(mode);
    spec.recipe_id = "project-hs.environment." + std::string(mode);
    spec.recipe_version = 1;
    spec.namespace_path = "environment/" + std::string(mode);
    std::vector<std::string> names;
    Microsoft::WRL::ComPtr<ID3D11Device> device;

    if (mode == "histogram") {
        AddInputs(spec, source_root, hs::content::EnvironmentHistogramInputs(texture_source));
        names = {"environment_terrain_gaussian.dds",
                 "environment_terrain_histogram.dds",
                 "environment_terrain_histogram.qa.json"};
        device = HardwareCompressor();
        spec.recipe_id += EncoderIdentity(device.Get());
    } else if (mode == "texture-role") {
        AddInputs(spec, source_root,
                  hs::content::EnvironmentTextureRoleInputs(texture_source, group, role));
        names = {hs::content::EnvironmentTextureRoleProduct(texture_source, group, role)};
        spec.action_id = "environment.texture." + group + "." + role;
        spec.recipe_id = "project-hs.environment.texture-role";
        spec.namespace_path = "environment/textures/" + group + "/" + role;
        if (role == "base_color") device = HardwareCompressor();
        spec.recipe_id += EncoderIdentity(device.Get());
    } else if (mode == "textures") {
        AddInputs(spec, source_root, hs::content::EnvironmentTextureInputs(texture_source));
        names = hs::content::EnvironmentTextureProducts(texture_source);
        device = HardwareCompressor();
        spec.recipe_id += EncoderIdentity(device.Get());
    } else if (mode == "meshes") {
        AddInputs(spec, source_root, {mesh_source, mesh_source.parent_path() / "meadow_surface_meshes.json"});
        names = MeshProductNames();
    } else {
        throw std::runtime_error("hs_environment: unsupported action mode");
    }
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);

    const auto product_read_policy = mode == "texture-role" || mode == "histogram"
        ? ap::ProductReadPolicy::MetadataOnly
        : ap::ProductReadPolicy::VerifyIntegrity;
    if (IsCurrent(build_root, spec, names, product_read_policy)) {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try {
        if (mode == "histogram")
            hs::content::CookEnvironmentHistogram(texture_source, stage, device.Get(), false);
        else if (mode == "texture-role")
            hs::content::CookEnvironmentTextureRole(texture_source, stage, group, role,
                                                    device.Get());
        else if (mode == "textures")
            hs::content::CookEnvironmentTextures(texture_source, stage, device.Get(), false);
        else
            hs::content::CookEnvironmentMeshes(mesh_source, stage);

        std::vector<ap::Product> products;
        for (const auto& name : names)
            products.push_back(ap::HashProduct("environment/" + name, name, stage / name));
        const auto staged_count = std::distance(fs::directory_iterator(stage), fs::directory_iterator{});
        Require(staged_count == static_cast<std::ptrdiff_t>(names.size()),
                "producer wrote unexpected staged files");
        ap::Publish(build_root, spec, stage, std::move(products));
    } catch (...) {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::string mode;
    ap::Json action_id = nullptr;
    std::string group;
    std::string role;
    fs::path source_root;
    fs::path build_root;
    try {
        const ScopedComInitialization com;
        Require(SUCCEEDED(com.result) || com.result == RPC_E_CHANGED_MODE,
                "COM initialization failed");
        for (int i = 1; i < argc; ++i) {
            const std::wstring_view arg = argv[i];
            if (arg == L"--source-root" && source_root.empty() && ++i < argc)
                source_root = fs::path(argv[i]);
            else if (arg == L"--build-root" && build_root.empty() && ++i < argc)
                build_root = fs::path(argv[i]);
            else if (arg == L"--texture-role" && mode.empty() && i + 2 < argc) {
                mode = "texture-role";
                const std::wstring_view group_arg = argv[++i];
                const std::wstring_view role_arg = argv[++i];
                const auto ascii = [](std::wstring_view value) {
                    return !value.empty() && std::ranges::all_of(value, [](wchar_t ch) {
                        return (ch >= L'a' && ch <= L'z') || ch == L'_';
                    });
                };
                Require(ascii(group_arg) && ascii(role_arg), "invalid texture role");
                for (const auto ch : group_arg) group += static_cast<char>(ch);
                for (const auto ch : role_arg) role += static_cast<char>(ch);
            }
            else if ((arg == L"--histogram" || arg == L"--textures" || arg == L"--meshes") && mode.empty())
                mode.assign(arg.begin() + 2, arg.end());
            else
                throw std::runtime_error("hs_environment: invalid command line");
        }
        Require(!mode.empty() && !source_root.empty() && !build_root.empty(),
                "usage: hs_environment --histogram|--textures|--texture-role <group> <role>|--meshes --source-root <absolute> --build-root <absolute>");
        action_id = mode == "texture-role"
            ? "environment.texture." + group + "." + role
            : "environment." + mode;
        Produce(mode, source_root.lexically_normal(), build_root.lexically_normal(), group, role);
        return 0;
    } catch (const std::exception& error) {
        ap::ReportFailure("hs_environment", error.what(),
                          {{"action_id", action_id}, {"mode", mode},
                           {"group", group}, {"role", role},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)}});
        return 1;
    }
}
