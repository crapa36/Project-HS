#include "../AssetPipeline/action.hpp"

#include <hs/core/cooked_format.hpp>
#include <hs/render_data/lighting_probe.hpp>
#include <hs/render_data/scene_probe.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace
{
namespace ap=hs::asset_pipeline;
namespace rd=hs::render_data;
namespace fs=std::filesystem;
constexpr std::string_view kProduct="render/lighting/local_probes.spkg";
constexpr std::string_view kNamespace="render/lighting/local-probes";
constexpr std::string_view kArenaLayout="game-data/arena-layout";
constexpr std::array<std::string_view,14> kEnvironmentMaterials{
    "rock_0","rock_1","rock_2","rock_3","trunk_0","trunk_1","trunk_2",
    "leaf_0","leaf_1","leaf_2","grass_0","grass_1","grass_2","grass_3"};
void Require(bool valid,std::string_view message)
{ if(!valid) throw std::runtime_error("hs_render_scene_probe: "+std::string(message)); }
std::span<const std::byte> Bytes(std::string_view text)
{ return std::as_bytes(std::span(text.data(),text.size())); }
void Keys(const ap::Json &value,std::initializer_list<std::string_view> names)
{
    Require(value.is_object() && value.size()==names.size(),"invalid authored object keys");
    for(auto name:names) Require(value.contains(std::string(name)),"missing required authored key");
}
double Number(const ap::Json &value,double minimum,double maximum)
{
    Require(value.is_number(),"authored value must be numeric"); const auto number=value.get<double>();
    Require(std::isfinite(number) && number>=minimum && number<=maximum,"authored numeric value outside finite bounds"); return number;
}
std::uint32_t Integer(const ap::Json &value,std::uint32_t minimum,std::uint32_t maximum)
{
    Require(value.is_number_integer(),"authored value must be an integer");
    return static_cast<std::uint32_t>(Number(value,minimum,maximum));
}
hs::Float3 Position(const ap::Json &value)
{
    Require(value.is_array() && value.size()==3,"position/bounds require three coordinates");
    return {float(Number(value[0],-100000,100000)),float(Number(value[1],-100000,100000)),float(Number(value[2],-100000,100000))};
}
struct Source
{
    rd::SceneProbeDescription description;
    std::vector<rd::SceneProbeRecord> probes;
    std::string environment;
};
Source ParseSource(std::string_view text)
{
    std::vector<std::unordered_set<std::string>> objectKeys;
    const auto source=ap::Json::parse(text,[&](int,ap::Json::parse_event_t event,ap::Json &parsed) {
        if(event==ap::Json::parse_event_t::object_start) objectKeys.emplace_back();
        else if(event==ap::Json::parse_event_t::key)
            Require(!objectKeys.empty() && objectKeys.back().insert(parsed.get<std::string>()).second,"duplicate authored object key");
        else if(event==ap::Json::parse_event_t::object_end) objectKeys.pop_back();
        return true;
    });
    Keys(source,{"version","id","environment_product","color_space","radiance_unit","capture","blend_policy","revision_policy","probes","author_notes"});
    Require(source.at("version")==1 && source.at("id")=="local_probes" && source.at("environment_product")=="render/lighting/default_probe.rpkg" &&
        source.at("color_space")=="rec709_linear" && source.at("radiance_unit")=="scene_linear_radiance_per_steradian" &&
        source.at("blend_policy")=="normalized_influence_visibility_with_single_environment_baseline" &&
        source.at("revision_policy")=="hash_actual_environment_material_light_static_layout_inputs","unsupported authored probe convention");
    Require(source.at("author_notes").is_string() && !source.at("author_notes").get_ref<const std::string &>().empty() &&
        source.at("author_notes").get_ref<const std::string &>().size()<=4096,"invalid author notes");
    const auto &capture=source.at("capture");
    Keys(capture,{"kind","cube_edge","ggx_samples","maximum_cube_views_per_graph","include","exclude"});
    Require(capture.at("kind")=="native_static_common_material_cube_views" &&
        capture.at("include")==ap::Json::array({"static_geometry","common_material_opacity","static_direct_lights","emission","global_environment_sky"}) &&
        capture.at("exclude")==ap::Json::array({"dynamic_characters","dynamic_vfx","ui","post","exposure","recursive_local_probes"}),"invalid clean static capture scope");
    (void)Integer(capture.at("maximum_cube_views_per_graph"),1,6);
    Source result; result.environment=source.at("environment_product").get<std::string>();
    result.description.edge=Integer(capture.at("cube_edge"),16,128);
    result.description.ggx_samples=Integer(capture.at("ggx_samples"),64,2048);
    const auto &probes=source.at("probes");
    Require(probes.is_array() && !probes.empty() && probes.size()<=rd::kSceneProbeMaximumCount,"invalid authored probe count");
    std::uint64_t previous=0;
    for(const auto &entry:probes)
    {
        Keys(entry,{"stable_id","position","weight","influence_min","influence_max","influence_fade","projection_min","projection_max","visibility_bias","visibility_fade","capture_distance"});
        Require(entry.at("stable_id").is_number_integer(),"stable probe identity must be an integer");
        const auto id=static_cast<std::uint64_t>(Number(entry.at("stable_id"),1,9007199254740991.0));
        Require(id>previous,"authored probe identities must be sorted unique"); previous=id;
        rd::SceneProbeRecord p; p.stable_id=id; p.position=Position(entry.at("position"));
        p.weight=float(Number(entry.at("weight"),0,1)); p.influence_min=Position(entry.at("influence_min")); p.influence_max=Position(entry.at("influence_max"));
        p.influence_fade=float(Number(entry.at("influence_fade"),0,100000));
        p.projection_min=Position(entry.at("projection_min")); p.projection_max=Position(entry.at("projection_max"));
        p.visibility_bias=float(Number(entry.at("visibility_bias"),0,100000)); p.visibility_fade=float(Number(entry.at("visibility_fade"),0,100000));
        p.capture_distance=float(Number(entry.at("capture_distance"),0.1,100000)); result.probes.push_back(p);
    }
    // The public encoder validates power-of-two bake bounds, strict positive
    // weights/fades, box containment and capture distance after real revisions
    // and dependency references have been derived below.
    return result;
}
using Identities=std::map<std::string,std::string>;
std::uint64_t Revision(rd::SceneProbeRevisionDomain domain,const Identities &identities)
{
    std::vector<rd::SceneProbeRevisionInput> inputs; inputs.reserve(identities.size());
    for(const auto &[id,digest]:identities)
    {
        Require(digest.starts_with("sha256:") && digest.size() == 71, "revision SHA256 is invalid for " + id +
            ": length=" + std::to_string(digest.size()));
        // Action digests include their algorithm; the revision codec consumes
        // the validated lowercase hexadecimal payload and hashes its bytes.
        inputs.push_back({id,std::string_view(digest).substr(7)});
    }
    return rd::DeriveSceneProbeRevision(domain,inputs);
}

// Action results identify the real producer namespaces for dependency closure
// products (including texture roles, static geometry pages and shader groups).
// Selected upstream results are independently verified by ReadActionResult;
// this index never supplies trusted digests or paths by itself.
class Products
{
  public:
    explicit Products(const fs::path &buildRoot):root_(buildRoot)
    {
        const auto actions=buildRoot/"Actions"; ap::RequireDirectory(actions);
        for(const auto &entry:fs::recursive_directory_iterator(actions))
        {
            if(!entry.is_regular_file() || entry.path().filename()!="result.json") continue;
            const auto bytes=ap::ReadBytes(entry.path()); const auto result=ap::Json::parse(bytes,nullptr,false);
            if(result.is_discarded() || !result.is_object() || !result.contains("namespace_path") ||
                !result.at("namespace_path").is_string() || !result.contains("products") || !result.at("products").is_array()) continue;
            const auto namespacePath=result.at("namespace_path").get<std::string>();
            if(entry.path().parent_path().lexically_relative(actions).generic_string()!=namespacePath) continue;
            for(const auto &product:result.at("products"))
                if(product.is_object() && product.contains("logical_id") && product.at("logical_id").is_string())
                {
                    const auto id=product.at("logical_id").get<std::string>();
                    const auto [found,inserted]=namespaces_.emplace(id,namespacePath);
                    Require(inserted || found->second==namespacePath,"upstream logical product has multiple producer namespaces");
                }
        }
    }
    void Read(std::string_view id,std::string_view expectedNamespace,Identities &closure)
    {
        const std::string key(id);
        if(const auto existing=verified_.find(key);existing!=verified_.end())
        { AddClosure(key,closure); return; }
        const auto found=namespaces_.find(key);
        Require(found!=namespaces_.end(),"required upstream product has no current ActionResult: "+key);
        Require(expectedNamespace.empty() || found->second==expectedNamespace,"upstream product namespace disagrees with producer contract");
        Require(visiting_.insert(key).second,"cycle in actual upstream product dependency closure");
        ap::UpstreamProduct upstream; fs::path path;
        const auto action=ap::ReadActionResult(root_,found->second,key,upstream,path);
        std::vector<std::string> dependencies;
        for(const auto &product:action.at("products")) if(product.at("logical_id")==key && product.contains("dependencies"))
        {
            Require(product.at("dependencies").is_array(),"upstream product dependencies must be an array");
            for(const auto &dependency:product.at("dependencies"))
            { Require(dependency.is_string(),"upstream product dependency must be a logical ID"); dependencies.push_back(dependency.get<std::string>()); }
        }
        if(key=="render/lighting/default_probe.rpkg")
        {
            const auto bytes=ap::ReadBytes(path);
            (void)rd::ReadLightingProbePackage(Bytes(bytes),hs::MakeAssetId(key));
        }
        for(const auto &dependency:dependencies) Read(dependency,{},closure);
        visiting_.erase(key); verified_.emplace(key,Entry{upstream,std::move(dependencies)});
        closure[key]=upstream.digest;
    }
    void Append(ap::ActionSpec &spec) const
    { for(const auto &[id,entry]:verified_) { (void)id; spec.upstream.push_back(entry.upstream); } }
  private:
    struct Entry { ap::UpstreamProduct upstream; std::vector<std::string> dependencies; };
    void AddClosure(const std::string &id,Identities &closure) const
    {
        if(closure.contains(id)) return;
        const auto &entry=verified_.at(id); closure[id]=entry.upstream.digest;
        for(const auto &dependency:entry.dependencies) AddClosure(dependency,closure);
    }
    fs::path root_;
    std::map<std::string,std::string> namespaces_;
    std::map<std::string,Entry> verified_;
    std::set<std::string> visiting_;
};

void Produce(const fs::path &sourceRoot,const fs::path &buildRoot,const fs::path &executable)
{
    Require(sourceRoot.is_absolute() && buildRoot.is_absolute(),"source/build roots must be absolute");
    ap::RequireDirectory(sourceRoot); ap::MakeDirectory(buildRoot);
    ap::ActionSpec spec; spec.action_id="render.lighting.local-probes";
    spec.recipe_id="project-hs.render.scene-probe"; spec.recipe_version=1; spec.namespace_path=kNamespace;
    spec.inputs.push_back(ap::HashInput("tool/hs_render_scene_probe",executable));
    const auto sourcePath=sourceRoot/"ContentSource/Lighting/local_probes.json";
    const auto sourceBytes=ap::ReadBytes(sourcePath);
    spec.inputs.push_back({"ContentSource/Lighting/local_probes.json",sourcePath,ap::Sha256Bytes(Bytes(sourceBytes))});
    auto source=ParseSource(sourceBytes);
    for(const auto *relative:{"Schemas/scene_probe.schema.json","Source/Apps/RenderSceneProbe/main.cpp",
        "Source/Apps/AssetPipeline/action.hpp","Source/Apps/AssetPipeline/action.cpp",
        "Source/RenderData/Public/hs/render_data/scene_probe.hpp","Source/RenderData/Private/scene_probe.cpp",
        "Source/Core/Public/hs/core/cooked_format.hpp","Source/Core/Private/cooked_format.cpp",
        "Source/Core/Public/hs/core/sha256.hpp","Source/Core/Private/sha256.cpp",
        "Content/Shaders/local_probe_bake.hlsl","Content/Shaders/local_probe_sampling.hlsli"})
        spec.inputs.push_back(ap::HashInput(relative,sourceRoot/relative));
    Products products(buildRoot); Identities environment,materials,lights,layout;
    std::set<std::string> directDependencies;
    auto read=[&](std::string id,std::string nameSpace,Identities &identity) {
        products.Read(id,nameSpace,identity); directDependencies.insert(std::move(id));
    };
    read(source.environment,"render/lighting/default_probe",environment);
    read("render/material/terrain.rpkg","render/terrain",materials);
    read("render/material/terrain/parameters.rpkg","render/material/terrain/parameters",materials);
    for(const auto name:kEnvironmentMaterials)
    {
        read("render/"+std::string(name)+".rpkg","render/"+std::string(name),materials);
        const auto parameters="render/material/"+std::string(name)+"/parameters";
        read(parameters+".rpkg",parameters,materials);
    }
    read(std::string(kArenaLayout),"game-data/simulation-payload",layout);
    products.Read(kArenaLayout,"game-data/simulation-payload",lights);
    auto sourceIdentity=[&](const char *relative,Identities &identity) {
        auto input=ap::HashInput(relative,sourceRoot/relative); identity[input.logical_id]=input.digest; spec.inputs.push_back(std::move(input));
    };
    // There is no separate cooked light/layout product in the current project.
    // Hash their actual authoritative producer/consumer inputs, anchored by the
    // real arena-layout upstream product. Runtime instance/LightView hashes
    // qualify the final native capture generation, not this layout-only package.
    for(const auto *relative:{"Source/Presentation/Private/snapshot_projector.cpp",
        "Source/RendererD3D12/Private/renderer_frame.cpp","Source/Core/Public/hs/core/render_snapshot.hpp"}) sourceIdentity(relative,lights);
    for(const auto *relative:{"Source/Presentation/Private/environment_layout.cpp",
        "Source/Presentation/Public/hs/presentation/environment_layout.hpp","Source/Presentation/Private/environment_projector.cpp"}) sourceIdentity(relative,layout);
    layout["ContentSource/Lighting/local_probes.json"]=ap::Sha256Bytes(Bytes(sourceBytes));
    source.description.revisions={Revision(rd::SceneProbeRevisionDomain::Environment,environment),
        Revision(rd::SceneProbeRevisionDomain::Material,materials),Revision(rd::SceneProbeRevisionDomain::Light,lights),
        Revision(rd::SceneProbeRevisionDomain::StaticLayout,layout)};
    std::vector<std::string> dependencies(directDependencies.begin(),directDependencies.end());
    auto index=[&](std::string_view id) {
        const auto found=std::ranges::lower_bound(dependencies,id);
        Require(found!=dependencies.end() && *found==id,"missing actual scene dependency reference");
        return static_cast<std::uint32_t>(found-dependencies.begin());
    };
    source.description.environment_dependency=index(source.environment);
    source.description.material_dependency=index("render/material/terrain.rpkg");
    source.description.light_dependency=index(kArenaLayout); source.description.static_layout_dependency=index(kArenaLayout);
    products.Append(spec); std::ranges::sort(spec.inputs,{},&ap::Input::logical_id);
    std::ranges::sort(spec.upstream,{},&ap::UpstreamProduct::logical_id);
    const auto bytes=rd::BuildSceneProbePackage(hs::MakeAssetId(kProduct),dependencies,source.description,source.probes);
    const auto decoded=rd::ReadSceneProbePackage(bytes,hs::MakeAssetId(kProduct));
    Require(decoded.payload==rd::SceneProbePayload::Layout && decoded.probes.size()==source.probes.size() &&
        decoded.description.revisions==source.description.revisions,"SPB1 layout self validation failed");
    const auto resultPath=buildRoot/"Actions"/kNamespace/"result.json";
    if(fs::exists(resultPath) && fs::exists(resultPath.parent_path()/"result.d"))
    {
        try
        {
            const auto current=ap::Json::parse(ap::ReadBytes(resultPath));
            if(current.at("action_id")==spec.action_id && current.at("action_key")==ap::ActionKey(spec) && current.at("products").size()==1)
            {
                ap::UpstreamProduct ignored; fs::path path; (void)ap::ReadActionResult(buildRoot,kNamespace,kProduct,ignored,path);
                const auto existing=ap::ReadBytes(path); (void)rd::ReadSceneProbePackage(Bytes(existing),hs::MakeAssetId(kProduct));
                Require(Bytes(existing).size()==bytes.size() && std::equal(bytes.begin(),bytes.end(),Bytes(existing).begin()),"current SPB1 differs from exact authored/upstream snapshot");
                ap::TouchActionResult(resultPath); std::cout<<spec.action_id<<" current\n"; return;
            }
        }
        catch(const std::exception &) { /* Invalid current immutable product is rebuilt by the normal Action. */ }
    }
    const auto stage=ap::CreateStage(buildRoot/"Actions"/kNamespace);
    try
    {
        const auto path=stage/"local_probes.spkg"; ap::WriteBytes(path,bytes);
        auto product=ap::HashProduct(std::string(kProduct),"local_probes.spkg",path); product.dependencies=std::move(dependencies);
        ap::Publish(buildRoot,spec,stage,{std::move(product)});
    }
    catch(...) { ap::RemoveStage(stage); throw; }
    ap::RemoveStage(stage); std::cout<<spec.action_id<<" published\n";
}
} // namespace
#ifdef _WIN32
int wmain(int argc,wchar_t **argv)
#else
int main(int argc,char **argv)
#endif
{
    fs::path sourceRoot,buildRoot; bool produce=false;
    try
    {
        for(int i=1;i<argc;++i)
        {
            const fs::path arg(argv[i]);
            if(arg=="--produce" && !produce) produce=true;
            else if(arg=="--source-root" && sourceRoot.empty() && ++i<argc) sourceRoot=argv[i];
            else if(arg=="--build-root" && buildRoot.empty() && ++i<argc) buildRoot=argv[i];
            else throw std::runtime_error("usage: hs_render_scene_probe [--produce] --source-root <absolute> --build-root <absolute>");
        }
        Require(!sourceRoot.empty() && !buildRoot.empty(),"usage: hs_render_scene_probe [--produce] --source-root <absolute> --build-root <absolute>");
        Produce(sourceRoot.lexically_normal(),buildRoot.lexically_normal(),fs::absolute(fs::path(argv[0])).lexically_normal()); return 0;
    }
    catch(const std::exception &error)
    {
        ap::ReportFailure("hs_render_scene_probe", error.what(),
                          {{"action_id", "render.lighting.local-probes"}, {"produce", produce},
                           {"source_root", ap::DiagnosticPath(sourceRoot)}, {"build_root", ap::DiagnosticPath(buildRoot)}});
        return 1;
    }
}
