#include "scene_probe_action.hpp"
#include "../AssetPipeline/action.hpp"

#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/sha256.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/scene_probe_revision.hpp>
#include <hs/render_data/scene_probe.hpp>


#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace hs::scene_probe_bake
{
namespace
{
namespace ap=asset_pipeline;
namespace rd=render_data;
namespace fs=std::filesystem;
constexpr std::string_view kAction="render.lighting.scene-probes-baked";
constexpr std::string_view kNamespace="render/lighting/scene-probes-baked";
constexpr std::string_view kProduct="render/lighting/local_probes.spkg";
constexpr std::string_view kFilename="local_probes.spkg";

void Require(bool value,const std::string &message) { if(!value) throw std::runtime_error(message); }

bool Current(const fs::path &build,const ap::ActionSpec &spec,std::span<const std::string> dependencies)
{
    const auto result=build/"Actions"/kNamespace/"result.json";
    if(!fs::exists(result)||!fs::exists(result.parent_path()/"result.d")) return false;
    try
    {
        const ap::ActionResultSnapshot snapshot(build,std::string(kNamespace));
        if(snapshot.ActionId()!=kAction||snapshot.ActionKeyValue()!=ap::ActionKey(spec)||snapshot.ProductCount()!=1) return false;
        std::vector<std::string> expected(dependencies.begin(),dependencies.end());
        if(snapshot.ProductMetadata(std::string(kProduct)).value("dependencies",std::vector<std::string>{})!=expected) return false;
        ap::UpstreamProduct ignored; fs::path path; snapshot.ReadProduct(std::string(kProduct),ignored,path); return true;
    }
    catch(...) { return false; }
}

}

int ProduceSceneProbe(int argc,char **argv)
{
    fs::path source,build,pointer,result_list;
    try
    {
        std::set<std::string> seen;
        for(int i=1;i<argc;++i)
        {
            std::string_view arg(argv[i]); if(arg=="--produce-scene-probe") continue;
            const auto equal=arg.find('='); const auto name=arg.substr(0,equal);
            Require(name=="--source-root"||name=="--build-root"||name=="--generation"||name=="--result-list",
                "Unknown scene probe producer argument: "+std::string(name));
            Require(seen.emplace(name).second,"Duplicate scene probe producer argument: "+std::string(name));
            std::string_view value;
            if(equal!=std::string_view::npos) value=arg.substr(equal+1);
            else { Require(i+1<argc,"Missing argument value: "+std::string(name)); value=argv[++i]; }
            Require(!value.empty(),"Empty argument value: "+std::string(name));
            if(name=="--source-root") source=value; else if(name=="--build-root") build=value;
            else if(name=="--generation") pointer=value; else result_list=value;
        }
        Require(source.is_absolute()&&build.is_absolute()&&pointer.is_absolute()&&result_list.is_absolute(),
            "Scene probe producer requires absolute source/build/generation/result-list paths");
        ap::RequireDirectory(source); ap::RequireDirectory(build); source=fs::canonical(source); build=fs::canonical(build);
        SelectedGeneration generation; const auto loaded=LoadSelectedGeneration(pointer,generation);
        Require(static_cast<bool>(loaded),std::string(loaded.Message()));
        Require(generation.product_paths.contains(std::string(kProduct)),"Bake input Generation lacks layout-only SPB2");
        const auto layout_path=generation.product_paths.at(std::string(kProduct));
        std::ifstream layout_stream(layout_path,std::ios::binary|std::ios::ate);
        Require(static_cast<bool>(layout_stream)&&layout_stream.tellg()>0&&layout_stream.tellg()<=256ll*1024*1024,
            "Invalid layout-only SPB2 input extent");
        std::vector<std::byte> layout_bytes(static_cast<std::size_t>(layout_stream.tellg())); layout_stream.seekg(0);
        Require(static_cast<bool>(layout_stream.read(reinterpret_cast<char *>(layout_bytes.data()),static_cast<std::streamsize>(layout_bytes.size()))),
            "Cannot load layout-only SPB2 input");
        layout_stream.close();
        const auto layout_digest=hs::Sha256(layout_bytes);
        constexpr char hex_digits[]="0123456789abcdef";
        std::string layout_sha="sha256:";
        for(const auto byte:layout_digest)
        {
            const auto value=std::to_integer<unsigned>(byte);
            layout_sha.push_back(hex_digits[value>>4]); layout_sha.push_back(hex_digits[value&15]);
        }
        Require(layout_sha==generation.product_digests.at(std::string(kProduct)),
            "Layout-only SPB2 differs from selected Generation");
        const auto layout=rd::ReadSceneProbePackage(layout_bytes,MakeAssetId(kProduct));
        Require(layout.payload==rd::SceneProbePayload::Layout,"Bake input must be layout-only SPB2");

        ap::ActionSpec spec; spec.action_id=kAction; spec.namespace_path=kNamespace;
        spec.recipe_id="project-hs.render.scene-probes/native-irradiance-grid"; spec.recipe_version=6;
        // The bake layout is an explicit byte input. Its logical product id is
        // also the final baked product id, so it must not be represented as
        // an upstream product of this Action.
        spec.inputs.push_back({"scene-probe/layout-input",layout_path,layout_sha});
        const auto source_path=source/"ContentSource/Lighting/local_probes.json";
        const auto source_bytes=ap::ReadBytes(source_path);
        const auto authored=ap::Json::parse(source_bytes);
        const auto &view_limit=authored.at("capture").at("maximum_cube_views_per_graph");
        Require(view_limit.is_number_integer(),"Scene probe cube-view budget must be an integer");
        const auto numeric_limit=view_limit.get<std::int64_t>();
        Require(numeric_limit>=1 && numeric_limit<=6,"Scene probe cube-view budget must be 1..6");
        const auto maximum_cube_views=static_cast<std::uint32_t>(numeric_limit);
        const auto &solve=authored.at("capture").at("offline_multibounce");
        Require(solve.is_object() && solve.size()==4 && solve.at("mode")=="previous_complete_grid",
            "Invalid offline diffuse solve mode");
        const auto &limit=solve.at("maximum_iterations");
        Require(limit.is_number_integer(),"Scene probe iteration limit must be an integer");
        const auto iteration_limit=limit.get<std::int64_t>();
        Require(iteration_limit>=1 && iteration_limit<=64,"Scene probe iteration limit must be 1..64");
        const auto &abs_value=solve.at("absolute_tolerance"), &rel_value=solve.at("relative_tolerance");
        Require(abs_value.is_number() && rel_value.is_number(),"Scene probe tolerances must be numeric");
        const auto abs_tolerance=abs_value.get<double>(),rel_tolerance=rel_value.get<double>();
        Require(std::isfinite(abs_tolerance) && std::isfinite(rel_tolerance) && abs_tolerance>=0 && abs_tolerance<=1 &&
            rel_tolerance>=0 && rel_tolerance<=1 && (abs_tolerance>0 || rel_tolerance>0),"Invalid scene probe tolerances");
        spec.inputs.push_back({"scene-probe/source",source_path,
            ap::Sha256Bytes(std::as_bytes(std::span(source_bytes.data(),source_bytes.size())))});
        spec.inputs.push_back(ap::HashInput("scene-probe/schema",source/"Schemas/scene_probe.schema.json"));

        std::map<std::string,std::string> owners;
        std::map<std::string,ap::ActionResultSnapshot> snapshots;
        std::istringstream lines(ap::ReadBytes(result_list)); std::string line;
        while(std::getline(lines,line))
        {
            if(!line.empty()&&line.back()=='\r') line.pop_back(); Require(!line.empty(),"Empty scene probe result-list entry");
            const fs::path path(line); Require(path.is_absolute()&&path.filename()=="result.json","Invalid Action result path");
            const auto relative=fs::canonical(path).lexically_relative(build/"Actions");
            Require(!relative.empty()&&!relative.is_absolute()&&*relative.begin()!="..","Scene probe Action result outside build Actions");
            const auto name=relative.parent_path().generic_string(); auto [snapshot,inserted]=snapshots.try_emplace(name,build,name);
            Require(inserted,"Duplicate Action in scene probe input list");
            const auto document=ap::Json::parse(ap::ReadBytes(path));
            for(const auto &product:document.at("products"))
            {
                const auto id=product.at("logical_id").get<std::string>();
                (void)snapshot->second.ProductMetadata(id);
                Require(owners.emplace(id,name).second,"Duplicate product in scene probe input list: "+id);
            }
        }
        const auto runtime_surface_edge=[](std::string_view owner,std::string_view dependency) {
            if(!dependency.starts_with("render/shader/")) return false;
            for(const auto name:rd::kAuthoredMaterialNames)
                if(owner==rd::MaterialProduct(name))
                    return dependency=="render/shader/"+std::string(name)+"_scene.dxil" ||
                        dependency=="render/shader/"+std::string(name)+"_shadow.dxil";
            return false;
        };
        std::set<std::string> closure; std::vector<std::string> pending;
        pending.insert(pending.end(),layout.dependencies.begin(),layout.dependencies.end());
        // Capture uploads source-only terrain records and never dispatches page-bake kernels.
        for (const auto id : {"game-data/simulation-rules", "render/config/quality_profiles.rpkg",
                "render/lighting/coated_ggx_dfg.dds", "render/lighting/default_ltc.rpkg",
                "render/shader/light_list_cs.dxil"})
            pending.emplace_back(id);
        for(const auto &[id,_]:generation.product_paths)
            if(rd::IsLocalProbeMaterialRevisionProduct(id)) pending.push_back(id);
        while(!pending.empty())
        {
            auto id=std::move(pending.back()); pending.pop_back(); if(!closure.insert(id).second) continue;
            fs::path selected; const auto resolved=generation.Require(id,selected); Require(static_cast<bool>(resolved),std::string(resolved.Message()));
            const auto owner=owners.find(id); Require(owner!=owners.end(),"Selected bake input lacks listed Action owner: "+id);
            const auto &metadata=snapshots.at(owner->second).ProductMetadata(id); const auto digest=metadata.at("digest").get<std::string>();
            const auto size=metadata.at("size").get<std::uint64_t>();
            Require(generation.product_digests.at(id)==digest&&fs::file_size(selected)==size&&ap::Sha256File(selected)==digest,
                "Selected bake input differs from Action product: "+id);
            const auto dependencies=metadata.value("dependencies",std::vector<std::string>{});
            Require(generation.product_dependencies.contains(id)&&generation.product_dependencies.at(id)==dependencies,
                "Selected bake dependency closure differs: "+id);
            if(id!=kProduct)
                spec.upstream.push_back({id,digest,size,build/"Actions"/owner->second/"result.json"});
            // Probe capture uses its named capture/shadow programs, not the material's runtime raster programs.
            for(const auto &dependency:dependencies)
                if(!runtime_surface_edge(id,dependency)) pending.push_back(dependency);
        }
        std::ranges::sort(spec.inputs,{},&ap::Input::logical_id); std::ranges::sort(spec.upstream,{},&ap::UpstreamProduct::logical_id);
        if(Current(build,spec,layout.dependencies)) { ap::TouchActionResult(build/"Actions"/kNamespace/"result.json"); return 0; }
        const auto stage=ap::CreateStage(build/"Actions"/kNamespace);
        try
        {
            const auto output=stage/kFilename;
            const auto artifacts=build/"Artifacts"/"scene-probe-bake";
            ap::MakeDirectory(artifacts); BakeSceneProbe(generation,output,artifacts,maximum_cube_views,
                static_cast<std::uint32_t>(iteration_limit),abs_tolerance,rel_tolerance);
            std::ifstream baked_stream(output,std::ios::binary|std::ios::ate);
            Require(static_cast<bool>(baked_stream)&&baked_stream.tellg()>0,"Native scene probe output is missing");
            std::vector<std::byte> baked_bytes(static_cast<std::size_t>(baked_stream.tellg())); baked_stream.seekg(0);
            Require(static_cast<bool>(baked_stream.read(reinterpret_cast<char *>(baked_bytes.data()),static_cast<std::streamsize>(baked_bytes.size()))),"Cannot read native scene probe output");
            baked_stream.close(); // Windows publication renames the staged ProductSet.
            const auto baked=rd::ReadSceneProbePackage(baked_bytes,MakeAssetId(kProduct));
            Require(baked.payload==rd::SceneProbePayload::Baked && baked.description.grid.NodeCount() &&
                baked.description.grid.dimensions==layout.description.grid.dimensions &&
                baked.probes.size()==layout.probes.size(),"Native output is missing the selected baked grid or reflection layout");
            auto product=ap::HashProduct(std::string(kProduct),std::string(kFilename),output);
            product.dependencies=layout.dependencies;
            ap::Publish(build,spec,stage,{std::move(product)});
        }
        catch(...) { ap::RemoveStageAfterFailure(stage); throw; }
        ap::RemoveStage(stage); std::cout<<kAction<<" published\n"; return 0;
    }
    catch(const std::exception &error) { ap::ReportFailure(kAction,error.what(),{{"build_root",build.string()}}); return 1; }
}
}
