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
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
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
    "shrub_low", "stump_roots", "crate_intact", "crate_broken", "barrel_intact", "barrel_broken", "cart_broken", "fence_broken", "fence_splitrail", "sack_tied", "fallen_log", "rock_mossy", "meadow_ground", "meadow_water"};
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

std::string DefaultEncoderIdentity()
{
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    LARGE_INTEGER driver{};
    if(FAILED(CreateDXGIFactory(IID_PPV_ARGS(&factory))) ||
       FAILED(factory->EnumAdapters(0,&adapter)) ||
       FAILED(adapter->GetDesc(&description)) ||
       FAILED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver)))
        return ".cpu-bc7";
    return ".d3d11-bc7."+std::to_string(description.VendorId)+"."+
        std::to_string(description.DeviceId)+"."+std::to_string(description.SubSysId)+"."+
        std::to_string(description.Revision)+"."+
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
        const auto logical_id=relative.generic_string();
        if(std::ranges::none_of(spec.inputs,[&](const auto& input){return input.logical_id==logical_id;}))
            spec.inputs.push_back(ap::HashInput(logical_id,path));
    }
}

struct InputSnapshot
{
    fs::path source_root;
    fs::path snapshot_root;
    std::map<std::string, ap::Input> captured;

    void Capture(const fs::path& snapshot_path)
    {
        const auto relative = snapshot_path.lexically_relative(snapshot_root);
        Require(!relative.empty() && !relative.is_absolute() &&
                relative.begin() != relative.end() && *relative.begin() != "..",
                "snapshot input escapes its root");
        const auto logical_id = relative.generic_string();
        if (captured.contains(logical_id)) return;
        const auto source_path = source_root / relative;
        const auto target_path = snapshot_root / relative;
        ap::MakeDirectory(target_path.parent_path());
        captured.emplace(logical_id,
            ap::CopyAndHashInput(logical_id, source_path, target_path));
    }

    void SetInputs(ap::ActionSpec& spec, const std::vector<fs::path>& snapshot_paths)
    {
        spec.inputs.clear();
        for (const auto& path : snapshot_paths)
        {
            Capture(path);
            const auto relative = path.lexically_relative(snapshot_root).generic_string();
            spec.inputs.push_back(captured.at(relative));
        }
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        spec.inputs.erase(std::unique(spec.inputs.begin(), spec.inputs.end(),
            [](const auto& left, const auto& right) { return left.logical_id == right.logical_id; }),
            spec.inputs.end());
    }
};

bool IsCurrent(const fs::path& build_root, const ap::ActionSpec& spec,
               const std::vector<std::string>& names)
{
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (!fs::exists(result_path) || !fs::exists(result_path.parent_path() / "result.d"))
        return false;
    try {
        const ap::ActionResultSnapshot snapshot(build_root, spec.namespace_path);
        if (snapshot.ActionId() != spec.action_id ||
            snapshot.ActionKeyValue() != ap::ActionKey(spec) ||
            snapshot.ProductCount() != names.size()) return false;
        for (const auto& name : names) {
            ap::UpstreamProduct dependency;
            fs::path product_path;
            snapshot.ReadProduct("environment/" + name, dependency, product_path);
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

struct PendingAction
{
    ap::ActionSpec spec;
    std::vector<std::string> names;
    fs::path stage;
    std::string group;
    std::string role;
    bool needs_cook{};
};

void AddRoleAction(PendingAction& action, const fs::path& source_root,
                   const fs::path& texture_source, const std::string& group,
                   const std::string& role, const std::string& gpu_identity)
{
    action.group=group;
    action.role=role;
    action.spec.action_id="environment.texture."+group+"."+role;
    action.spec.recipe_id="project-hs.environment.texture-role"+
        (role=="base_color"?gpu_identity:".cpu-bc7");
    action.spec.recipe_version=1;
    action.spec.namespace_path="environment/textures/"+group+"/"+role;
    auto inputs=hs::content::EnvironmentTextureRoleInputs(texture_source,group,role);
    inputs.push_back(texture_source/"baked_materials.json");
    AddInputs(action.spec,source_root,inputs);
    action.names={hs::content::EnvironmentTextureRoleProduct(texture_source,group,role)};
}

std::string EscapeDepfile(const fs::path& path)
{
    std::string result;
    for(const char ch:path.generic_string()) {
        Require(ch!='\n'&&ch!='\r',"newline in environment batch depfile path");
        if(ch==' '||ch=='#'||ch==':'||ch=='\\') result.push_back('\\');
        result.push_back(ch=='$'?'$':ch);
        if(ch=='$') result.push_back('$');
    }
    return result;
}

void ProduceTextureBatch(const fs::path& source_root,const fs::path& build_root)
{
    const auto texture_source=source_root/"ContentSource/Textures/Environment";
    const std::array<std::pair<const char*,std::vector<const char*>>,6> roles{{
        {"terrain",{"base_color","surface_ra","normal","detail_normal","height"}},
        {"rock",{"base_color","surface_ra","normal","detail_normal"}},
        {"bark",{"base_color","surface_ra","normal","detail_normal"}},
        {"grass",{"base_color","normal","roughness"}},
        {"leaf",{"base_color","normal","roughness"}},
        {"ground_detail",{"base_color","surface_ra","normal"}}}};
    const auto guessed_gpu_identity=DefaultEncoderIdentity();
    std::vector<PendingAction> actions;
    actions.reserve(23);
    for(const auto& [group,group_roles]:roles) for(const auto* role:group_roles) {
        PendingAction action;
        AddRoleAction(action,source_root,texture_source,group,role,guessed_gpu_identity);
        actions.push_back(std::move(action));
    }
    PendingAction histogram;
    histogram.spec.action_id="environment.histogram";
    histogram.spec.recipe_id="project-hs.environment.histogram"+guessed_gpu_identity;
    histogram.spec.recipe_version=1;
    histogram.spec.namespace_path="environment/histogram";
    AddInputs(histogram.spec,source_root,hs::content::EnvironmentHistogramInputs(texture_source));
    histogram.names={"environment_terrain_gaussian.dds","environment_terrain_histogram.dds","environment_terrain_histogram.qa.json"};
    actions.push_back(std::move(histogram));
    Require(actions.size()==23,"environment family batch must contain 22 texture roles and histogram");

    std::vector<hs::content::EnvironmentTextureRoleRequest> requests;
    std::vector<std::string> all_inputs;
    std::vector<bool> current(actions.size());
    fs::path input_stage;
    try {
    for(size_t i=0;i<actions.size();++i) {
        std::ranges::sort(actions[i].spec.inputs,{},&ap::Input::logical_id);
        current[i]=IsCurrent(build_root,actions[i].spec,actions[i].names);
    }
    bool need_gpu=false;
    for(size_t i=0;i<actions.size();++i)
        if(!current[i] && (actions[i].role=="base_color" || actions[i].spec.action_id=="environment.histogram"))
            need_gpu=true;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if(need_gpu) {
        device=HardwareCompressor();
        const auto actual_identity=EncoderIdentity(device.Get());
        if(actual_identity!=guessed_gpu_identity) {
            for(auto& action:actions) {
                if(action.role=="base_color") action.spec.recipe_id="project-hs.environment.texture-role"+actual_identity;
                else if(action.spec.action_id=="environment.histogram")
                    action.spec.recipe_id="project-hs.environment.histogram"+actual_identity;
            }
            for(size_t i=0;i<actions.size();++i)
                current[i]=IsCurrent(build_root,actions[i].spec,actions[i].names);
        }
    }
    if(std::ranges::any_of(current,[](bool value){return !value;})) {
        input_stage=ap::CreateStage(build_root/"Actions/environment/textures");
        InputSnapshot snapshot{source_root,input_stage,{}};
        const auto frozen_texture_source=input_stage/"ContentSource/Textures/Environment";
        snapshot.Capture(frozen_texture_source/"project_hs_environment_materials_v9_LEAN.json");
        snapshot.Capture(frozen_texture_source/"baked_materials.json");
        for(size_t i=0;i<actions.size();++i) {
            auto& action=actions[i];
            const auto has_same_snapshot_input=[&](const fs::path& frozen_path) {
                const auto relative=frozen_path.lexically_relative(input_stage).generic_string();
                const auto frozen=snapshot.captured.find(relative);
                const auto original=std::ranges::find(action.spec.inputs,relative,&ap::Input::logical_id);
                return frozen!=snapshot.captured.end() && original!=action.spec.inputs.end() &&
                    frozen->second.digest==original->digest;
            };
            const bool metadata_unchanged=has_same_snapshot_input(
                frozen_texture_source/"project_hs_environment_materials_v9_LEAN.json") &&
                (!action.spec.action_id.starts_with("environment.texture.") ||
                 has_same_snapshot_input(frozen_texture_source/"baked_materials.json"));
            if(current[i] && metadata_unchanged) continue;
            if(action.spec.action_id.starts_with("environment.texture.")) {
                auto paths=hs::content::EnvironmentTextureRoleInputs(
                    frozen_texture_source,action.group,action.role);
                paths.push_back(frozen_texture_source/"baked_materials.json");
                snapshot.SetInputs(action.spec,paths);
                action.names={hs::content::EnvironmentTextureRoleProduct(
                    frozen_texture_source,action.group,action.role)};
            } else {
                snapshot.SetInputs(action.spec,hs::content::EnvironmentHistogramInputs(frozen_texture_source));
            }
            current[i]=IsCurrent(build_root,action.spec,action.names);
        }
        need_gpu=false;
        for(size_t i=0;i<actions.size();++i)
            if(!current[i] && (actions[i].role=="base_color" || actions[i].spec.action_id=="environment.histogram"))
                need_gpu=true;
        if(need_gpu && !device) {
            device=HardwareCompressor();
            const auto actual_identity=EncoderIdentity(device.Get());
            if(actual_identity!=guessed_gpu_identity) {
                const auto was_current=current;
                for(auto& action:actions) {
                    if(action.role=="base_color") action.spec.recipe_id="project-hs.environment.texture-role"+actual_identity;
                    else if(action.spec.action_id=="environment.histogram")
                        action.spec.recipe_id="project-hs.environment.histogram"+actual_identity;
                }
                for(size_t i=0;i<actions.size();++i) {
                    current[i]=IsCurrent(build_root,actions[i].spec,actions[i].names);
                    if(was_current[i] && !current[i]) {
                        auto& action=actions[i];
                        if(action.spec.action_id.starts_with("environment.texture.")) {
                            auto paths=hs::content::EnvironmentTextureRoleInputs(
                                frozen_texture_source,action.group,action.role);
                            paths.push_back(frozen_texture_source/"baked_materials.json");
                            snapshot.SetInputs(action.spec,paths);
                            action.names={hs::content::EnvironmentTextureRoleProduct(
                                frozen_texture_source,action.group,action.role)};
                        } else {
                            snapshot.SetInputs(action.spec,
                                hs::content::EnvironmentHistogramInputs(frozen_texture_source));
                        }
                        current[i]=IsCurrent(build_root,action.spec,action.names);
                    }
                }
            }
        }
    }
    for(size_t i=0;i<actions.size();++i) {
        auto& action=actions[i];
        for(const auto& input:action.spec.inputs) all_inputs.push_back(input.path.generic_string());
        if(current[i]) {
            ap::TouchActionResult(build_root/"Actions"/action.spec.namespace_path/"result.json");
            std::cout<<action.spec.action_id<<" current\n";
            continue;
        }
        action.needs_cook=true;
        action.stage=ap::CreateStage(build_root/"Actions"/action.spec.namespace_path);
        if(action.spec.action_id.starts_with("environment.texture.")) {
            requests.push_back({action.group,action.role,action.stage});
        }
    }
        const auto cook_texture_source=input_stage.empty()?texture_source:
            input_stage/"ContentSource/Textures/Environment";
        if(!requests.empty()) hs::content::CookEnvironmentTextureRoles(cook_texture_source,requests,device.Get());
        for(auto& action:actions) if(action.needs_cook&&action.spec.action_id=="environment.histogram")
            hs::content::CookEnvironmentHistogram(cook_texture_source,action.stage,device.Get());
        for(auto& action:actions) if(action.needs_cook) {
            std::vector<ap::Product> products;
            for(const auto& name:action.names)
                products.push_back(ap::HashProduct("environment/"+name,name,action.stage/name));
            Require(std::distance(fs::directory_iterator(action.stage),fs::directory_iterator{})==
                    static_cast<std::ptrdiff_t>(action.names.size()),"unexpected environment batch stage files");
            ap::Publish(build_root,action.spec,action.stage,std::move(products));
            ap::RemoveStage(action.stage);
            action.stage.clear();
            std::cout<<action.spec.action_id<<" published\n";
        }

        std::ranges::sort(all_inputs);
        all_inputs.erase(std::unique(all_inputs.begin(),all_inputs.end()),all_inputs.end());
        const auto completion=build_root/"Actions/environment/textures/family.stamp";
        const auto depfile=build_root/"Actions/environment/textures/family.d";
        std::string dep=EscapeDepfile(completion)+":";
        for(const auto& input:all_inputs) dep+=" "+EscapeDepfile(input);
        dep+='\n';
        ap::AtomicWriteIfDifferent(depfile,dep);
        const auto timestamp=std::chrono::steady_clock::now().time_since_epoch().count();
        ap::AtomicWriteIfDifferent(completion,"environment-texture-family-v1\n"+std::to_string(timestamp)+"\n");
        if(!input_stage.empty()) { ap::RemoveStage(input_stage); input_stage.clear(); }
    } catch(...) {
        for(auto& action:actions) if(!action.stage.empty()) ap::RemoveStageAfterFailure(action.stage);
        if(!input_stage.empty()) ap::RemoveStageAfterFailure(input_stage);
        throw;
    }
}

void Produce(std::string_view mode, const fs::path& source_root,
             const fs::path& build_root, const std::string& group = {},
             const std::string& role = {})
{
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "source and build roots must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);

    if(mode=="texture-batch") {
        ProduceTextureBatch(source_root,build_root);
        return;
    }

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
        auto inputs=hs::content::EnvironmentTextureRoleInputs(texture_source,group,role);
        inputs.push_back(texture_source/"baked_materials.json");
        AddInputs(spec,source_root,inputs);
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

    if (IsCurrent(build_root, spec, names)) {
        ap::TouchActionResult(build_root / "Actions" / spec.namespace_path / "result.json");
        std::cout << spec.action_id << " current\n";
        return;
    }

    const auto input_stage=ap::CreateStage(build_root/"Actions"/spec.namespace_path);
    try {
        InputSnapshot snapshot{source_root,input_stage,{}};
        const auto frozen_texture_source=input_stage/"ContentSource/Textures/Environment";
        const auto frozen_mesh_source=input_stage/"ContentSource/Models/Environment/environment_meshes.json";
        if(mode=="histogram")
            snapshot.Capture(frozen_texture_source/"project_hs_environment_materials_v9_LEAN.json");
        else if(mode=="texture-role" || mode=="textures") {
            snapshot.Capture(frozen_texture_source/"project_hs_environment_materials_v9_LEAN.json");
            snapshot.Capture(frozen_texture_source/"baked_materials.json");
        } else {
            snapshot.Capture(frozen_mesh_source);
            snapshot.Capture(frozen_mesh_source.parent_path()/"meadow_surface_meshes.json");
        }

        std::vector<fs::path> frozen_inputs;
        if(mode=="histogram")
            frozen_inputs=hs::content::EnvironmentHistogramInputs(frozen_texture_source);
        else if(mode=="texture-role") {
            frozen_inputs=hs::content::EnvironmentTextureRoleInputs(frozen_texture_source,group,role);
            frozen_inputs.push_back(frozen_texture_source/"baked_materials.json");
            names={hs::content::EnvironmentTextureRoleProduct(frozen_texture_source,group,role)};
        } else if(mode=="textures") {
            frozen_inputs=hs::content::EnvironmentTextureInputs(frozen_texture_source);
            names=hs::content::EnvironmentTextureProducts(frozen_texture_source);
        } else {
            frozen_inputs={frozen_mesh_source,frozen_mesh_source.parent_path()/"meadow_surface_meshes.json"};
        }
        snapshot.SetInputs(spec,frozen_inputs);
        if(IsCurrent(build_root,spec,names)) {
            ap::TouchActionResult(build_root/"Actions"/spec.namespace_path/"result.json");
            ap::RemoveStage(input_stage);
            std::cout<<spec.action_id<<" current\n";
            return;
        }

        const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
        try {
            if (mode == "histogram")
                hs::content::CookEnvironmentHistogram(frozen_texture_source, stage, device.Get());
            else if (mode == "texture-role")
                hs::content::CookEnvironmentTextureRole(frozen_texture_source, stage, group, role,
                                                        device.Get());
            else if (mode == "textures")
                hs::content::CookEnvironmentTextures(frozen_texture_source, stage, device.Get());
            else
                hs::content::CookEnvironmentMeshes(frozen_mesh_source, stage);

            std::vector<ap::Product> products;
            for (const auto& name : names)
                products.push_back(ap::HashProduct("environment/" + name, name, stage / name));
            const auto staged_count = std::distance(fs::directory_iterator(stage), fs::directory_iterator{});
            Require(staged_count == static_cast<std::ptrdiff_t>(names.size()),
                    "producer wrote unexpected staged files");
            ap::Publish(build_root, spec, stage, std::move(products));
        } catch (...) {
            ap::RemoveStageAfterFailure(stage);
            throw;
        }
        ap::RemoveStage(stage);
        ap::RemoveStage(input_stage);
        std::cout << spec.action_id << " published\n";
    } catch (...) {
        ap::RemoveStageAfterFailure(input_stage);
        throw;
    }
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
            else if ((arg == L"--histogram" || arg == L"--textures" || arg == L"--meshes" || arg == L"--texture-batch") && mode.empty())
                mode.assign(arg.begin() + 2, arg.end());
            else
                throw std::runtime_error("hs_environment: invalid command line");
        }
        Require(!mode.empty() && !source_root.empty() && !build_root.empty(),
                "usage: hs_environment --histogram|--textures|--texture-role <group> <role>|--texture-batch|--meshes --source-root <absolute> --build-root <absolute>");
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
