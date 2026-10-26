#include "scene_probe_action.hpp"
#include "../AssetPipeline/action.hpp"

#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/sha256.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/scene_probe.hpp>

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace hs::render_test
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
constexpr DWORD kNativeBakeTimeoutMs=20u*60u*1000u;

void Require(bool value,const std::string &message) { if(!value) throw std::runtime_error(message); }

std::wstring Quote(const std::wstring &value)
{
    std::wstring result=L"\""; std::size_t slashes{};
    for(const wchar_t ch:value)
    {
        if(ch==L'\\') { ++slashes; continue; }
        if(ch==L'\"') { result.append(slashes*2+1,L'\\'); result+=ch; slashes=0; continue; }
        result.append(slashes,L'\\'); slashes=0; result+=ch;
    }
    result.append(slashes*2,L'\\'); result+=L'\"'; return result;
}

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

void RunNativeBake(const fs::path &generation,const fs::path &output,const fs::path &artifacts)
{
    std::wstring executable(32768,L'\0');
    const auto count=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    Require(count>0&&count<executable.size(),"Cannot locate render-test-runner executable");
    executable.resize(count);
    const auto command=Quote(executable)+L" --scene-probe-bake-only "+
        Quote(L"--generation="+generation.wstring())+L" "+Quote(L"--scene-probe-output="+output.wstring())+
        L" "+Quote(L"--artifacts="+artifacts.wstring());
    std::vector<wchar_t> mutable_command(command.begin(),command.end()); mutable_command.push_back(L'\0');
    STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(),mutable_command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
        nullptr,nullptr,&startup,&process)!=FALSE,"Cannot launch bounded native scene probe render");
    CloseHandle(process.hThread);
    auto wait=WaitForSingleObject(process.hProcess,kNativeBakeTimeoutMs);
    const bool timed_out=wait==WAIT_TIMEOUT;
    if(timed_out)
    {
        (void)TerminateProcess(process.hProcess,ERROR_TIMEOUT);
        // Keep the owned handle until termination completes so stage cleanup
        // cannot race this child writing its package or diagnostic artifacts.
        wait=WaitForSingleObject(process.hProcess,INFINITE);
    }
    DWORD exit_code=1; const bool got_code=GetExitCodeProcess(process.hProcess,&exit_code)!=FALSE;
    CloseHandle(process.hProcess);
    Require(!timed_out&&wait==WAIT_OBJECT_0&&got_code&&exit_code==0,
        timed_out ? "Native scene probe render exceeded its 20-minute timeout; diagnostics: " + artifacts.string()
                  : "Native scene probe render failed (exit " + std::to_string(exit_code) + "); diagnostics: " + artifacts.string());
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
        Require(generation.product_paths.contains(std::string(kProduct)),"Bake input Generation lacks layout-only SPB1");
        const auto layout_path=generation.product_paths.at(std::string(kProduct));
        std::ifstream layout_stream(layout_path,std::ios::binary|std::ios::ate);
        Require(static_cast<bool>(layout_stream)&&layout_stream.tellg()>0,"Cannot read layout-only SPB1 input");
        std::vector<std::byte> layout_bytes(static_cast<std::size_t>(layout_stream.tellg())); layout_stream.seekg(0);
        Require(static_cast<bool>(layout_stream.read(reinterpret_cast<char *>(layout_bytes.data()),static_cast<std::streamsize>(layout_bytes.size()))),
            "Cannot load layout-only SPB1 input");
        const auto layout=rd::ReadSceneProbePackage(layout_bytes,MakeAssetId(kProduct));
        Require(layout.payload==rd::SceneProbePayload::Layout,"Bake input must be layout-only SPB1");

        ap::ActionSpec spec; spec.action_id=kAction; spec.namespace_path=kNamespace;
        spec.recipe_id="project-hs.render.scene-probes/native-four-payload"; spec.recipe_version=1;
        // The bake layout is an explicit byte input. Its logical product id is
        // also the final baked product id, so it must not be represented as
        // an upstream product of this Action.
        spec.inputs.push_back(ap::HashInput("scene-probe/layout-input",layout_path));
        for(const auto &[id,path]:std::initializer_list<std::pair<std::string_view,fs::path>>{
            {"scene-probe/source",source/"ContentSource/Lighting/local_probes.json"},
            {"scene-probe/schema",source/"Schemas/scene_probe.schema.json"},
            {"scene-probe/package-format",source/"Source/RenderData/Private/scene_probe.cpp"},
            {"scene-probe/package-contract",source/"Source/RenderData/Public/hs/render_data/scene_probe.hpp"},
            {"scene-probe/renderer-capture",source/"Source/RendererD3D12/Private/renderer_local_probe.cpp"},
            {"scene-probe/renderer-capture-interface",source/"Source/RendererD3D12/Private/renderer_local_probe.hpp"},
            {"scene-probe/renderer-runtime",source/"Source/RendererD3D12/Private/renderer_local_probe_runtime.cpp"},
            {"scene-probe/renderer-frame",source/"Source/RendererD3D12/Private/renderer_frame.cpp"},
            {"scene-probe/renderer-export",source/"Source/RendererD3D12/Private/renderer_capture.cpp"},
            {"scene-probe/capture-request",source/"Source/RendererD3D12/Public/hs/renderer/capture_request.hpp"},
            {"scene-probe/render-action",source/"Source/Apps/RenderTestRunner/main.cpp"},
            {"scene-probe/action-interface",source/"Source/Apps/RenderTestRunner/scene_probe_action.hpp"},
            {"scene-probe/action-coordinator",source/"Source/Apps/RenderTestRunner/scene_probe_action.cpp"}})
            spec.inputs.push_back(ap::HashInput(std::string(id),path));

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
        std::set<std::string> closure; std::vector<std::string> pending;
        for(const auto &[id,_]:generation.product_paths) pending.push_back(id);
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
            pending.insert(pending.end(),dependencies.begin(),dependencies.end());
        }
        std::ranges::sort(spec.inputs,{},&ap::Input::logical_id); std::ranges::sort(spec.upstream,{},&ap::UpstreamProduct::logical_id);
        if(Current(build,spec,layout.dependencies)) { ap::TouchActionResult(build/"Actions"/kNamespace/"result.json"); std::cout<<kAction<<" current\n"; return 0; }
        const auto stage=ap::CreateStage(build/"Actions"/kNamespace);
        try
        {
            const auto output=stage/kFilename;
            const auto artifacts=build/"Artifacts"/"scene-probe-bake"/stage.filename();
            ap::MakeDirectory(artifacts); RunNativeBake(pointer,output,artifacts);
            std::ifstream baked_stream(output,std::ios::binary|std::ios::ate);
            Require(static_cast<bool>(baked_stream)&&baked_stream.tellg()>0,"Native scene probe output is missing");
            std::vector<std::byte> baked_bytes(static_cast<std::size_t>(baked_stream.tellg())); baked_stream.seekg(0);
            Require(static_cast<bool>(baked_stream.read(reinterpret_cast<char *>(baked_bytes.data()),static_cast<std::streamsize>(baked_bytes.size()))),"Cannot read native scene probe output");
            baked_stream.close(); // Windows publication renames the staged ProductSet.
            const auto baked=rd::ReadSceneProbePackage(baked_bytes,MakeAssetId(kProduct));
            Require(baked.payload==rd::SceneProbePayload::Baked,"Native scene probe output is not four-payload SPB1");
            auto product=ap::HashProduct(std::string(kProduct),std::string(kFilename),output);
            product.dependencies=layout.dependencies;
            ap::Publish(build,spec,stage,{std::move(product)});
        }
        catch(...) { ap::RemoveStage(stage); throw; }
        ap::RemoveStage(stage); std::cout<<kAction<<" published\n"; return 0;
    }
    catch(const std::exception &error) { ap::ReportFailure(kAction,error.what(),{{"build_root",build.string()}}); return 1; }
}
}
