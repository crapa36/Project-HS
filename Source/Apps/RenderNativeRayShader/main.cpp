#include "../AssetPipeline/action.hpp"
#include "../AssetPipeline/render_sdk_pin.hpp"
#include "../RenderMaterialShader/material_graph_parser.hpp"
#include <hs/core/windows_command_line.hpp>
#include <hs/render_data/material_sidecar.hpp>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;
void Require(bool condition, std::string_view reason)
{
    if (!condition) throw std::runtime_error("hs_render_native_ray_shader: " + std::string(reason));
}
ap::Input SemanticInput(std::string id, const fs::path &path, std::span<const std::byte> bytes)
{ return {std::move(id), path, ap::Sha256Bytes(bytes)}; }
#include "../RenderMaterialShader/material_context_include.hpp"

struct Program
{
    const char *filename, *source;
    const wchar_t *entry, *profile;
    bool omm, ser;
};
constexpr std::array kPrograms{
    Program{"rtgi_trace_omm_cs.dxil", "ray_gi.hlsl", L"GITraceCS", L"cs_6_9", true, false},
    Program{"hybrid_reflection_trace_omm_cs.dxil", "ray_reflections.hlsl", L"HybridReflectionTraceCS", L"cs_6_9", true, false},
    Program{"rtgi_trace_ser_lib.dxil", "ray_gi_ser.hlsl", L"GITraceRayGen", L"lib_6_9", false, true},
    Program{"hybrid_reflection_trace_ser_lib.dxil", "ray_reflections_ser.hlsl", L"HybridReflectionTraceRayGen", L"lib_6_9", false, true},
    Program{"rtgi_trace_omm_ser_lib.dxil", "ray_gi_ser.hlsl", L"GITraceRayGen", L"lib_6_9", true, true},
    Program{"hybrid_reflection_trace_omm_ser_lib.dxil", "ray_reflections_ser.hlsl", L"HybridReflectionTraceRayGen", L"lib_6_9", true, true}};

void Dxc(const fs::path &compiler, const fs::path &root, const fs::path &include,
         const Program &program, std::uint32_t planes, bool nrd,
         const fs::path &output, const fs::path &diagnostics, bool scan)
{
    std::vector<std::wstring> args{L"-nologo", L"-WX", L"-O3", L"-Ges", L"-T", program.profile,
        program.ser ? L"-exports" : L"-E", program.entry,
        L"-I", include.wstring(), L"-I", (root / "Content/Shaders").wstring(),
        L"-DHS_GENERATED_DEFERRED_MATERIALS=1", L"-DHS_GENERATED_HERO_MATERIAL=1",
        L"-DHS_RAY_OPACITY_MICROMAPS=" + std::to_wstring(program.omm),
        L"-DHS_RAY_SER=" + std::to_wstring(program.ser),
        L"-DHS_SURFACE_SIDECAR_PLANES=" + std::to_wstring(planes),
        L"-DHS_WITH_NRD=" + std::to_wstring(nrd)};
    if (scan) { args.emplace_back(L"-M"); args.emplace_back(L"-MF"); }
    else args.emplace_back(L"-Fo");
    args.push_back(output.wstring());
    args.emplace_back(L"-Fe"); args.push_back(diagnostics.wstring());
    args.push_back((root / "Content/Shaders" / program.source).wstring());
    std::wstring command = hs::QuoteWindowsCommandLineArgument(compiler.wstring());
    for (const auto &arg : args) command += L" " + hs::QuoteWindowsCommandLineArgument(arg);
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(compiler.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process) != FALSE, "DXC launch failed");
    CloseHandle(process.hThread);
    auto wait = WaitForSingleObject(process.hProcess, 120'000);
    if (wait == WAIT_TIMEOUT)
    { TerminateProcess(process.hProcess, 1); wait = WaitForSingleObject(process.hProcess, INFINITE); }
    DWORD code = 1;
    const auto got = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    Require(wait == WAIT_OBJECT_0 && got && code == 0,
        std::string(scan ? "DXC dependency scan failed: " : "DXC compilation failed: ") +
        program.filename + ": " + (fs::exists(diagnostics) ? ap::ReadBytes(diagnostics) : "no diagnostics"));
}

std::set<std::string> Dependencies(const fs::path &depfile)
{
    const auto text = ap::ReadBytes(depfile);
    std::size_t separator = std::string::npos;
    for (std::size_t i = 0; i + 1 < text.size(); ++i)
        if (text[i] == ':' && (text[i + 1] == ' ' || text[i + 1] == '\t')) { separator = i; break; }
    Require(separator != std::string::npos, "DXC depfile target separator missing");
    std::set<std::string> paths;
    // The pinned retail DXC -M writer emits one complete filename per
    // continued physical line, with spaces in filenames left unescaped.
    // Treating it as a whitespace-tokenized Make rule splits Project HS and
    // generated helper paths. Preserve the exact filename on each record;
    // callers still require the actual file to belong to a hashed source or
    // authenticated generated-helper closure.
    std::string_view records(text.data() + separator + 1, text.size() - separator - 1);
    while (!records.empty())
    {
        const auto newline = records.find('\n');
        auto record = records.substr(0, newline);
        records = newline == std::string_view::npos ? std::string_view{} : records.substr(newline + 1);
        if (!record.empty() && record.back() == '\r') record.remove_suffix(1);
        while (!record.empty() && (record.front() == ' ' || record.front() == '\t')) record.remove_prefix(1);
        if (record.size() >= 2 && record[record.size() - 2] == ' ' && record.back() == char(92))
            record.remove_suffix(2);
        Require(!record.empty() && record.find('\r') == std::string_view::npos &&
            record.find('\0') == std::string_view::npos, "malformed retail DXC dependency record");
        paths.emplace(record);
    }
    Require(!paths.empty(), "empty DXC depfile"); return paths;
}
bool Within(const fs::path &root, const fs::path &path)
{
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.has_root_path()) return false;
    return std::ranges::none_of(relative, [](const auto &part) { return part == ".."; });
}
bool ValidDxil(const fs::path &path)
{
    const auto bytes = ap::ReadBytes(path);
    return bytes.size() >= 32 && bytes.substr(0, 4) == "DXBC";
}

void Produce(const fs::path &source, const fs::path &build, const fs::path &compiler,
             const fs::path &pin, bool nrd)
{
    Require(source.is_absolute() && build.is_absolute() && compiler.is_absolute(), "absolute producer paths required");
    const auto pinned = ap::ReadRenderSdkPin(source, pin, "dxc69");
    Require(pinned.at("release") == "1.9.2602.24" && pinned.at("distribution_kind") == "release_archive" &&
        pinned.at("archive_uri") == "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602.24/dxc_2026_05_27.zip" &&
        pinned.at("archive_sha256") == "cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4",
        "retail compiler pin differs from approved native-ray recipe");
    const auto sdk = source / "Build/tooling/render-sdks/dxc69-1.9.2602.24";
    Require(fs::equivalent(compiler, sdk / "bin/x64/dxc.exe"), "native ray requires the dedicated retail compiler");
    ap::ActionSpec spec;
    spec.action_id = "render.shader.ray-native";
    spec.recipe_id = "project-hs.render.native-ray-sm69";
    spec.recipe_version = 1;
    spec.namespace_path = "render/shader/ray-native";
    spec.inputs.push_back(ap::HashInput("sdk/dxc69/pin", pin));
    auto archive = ap::HashInput("sdk/dxc69/archive", sdk.parent_path() / pinned.at("archive_filename").get<std::string>());
    Require(archive.digest == "sha256:" + pinned.at("archive_sha256").get<std::string>(), "compiler archive authentication failed");
    spec.inputs.push_back(std::move(archive));
    const std::map<std::string, std::string> expected_components{
        {"bin/x64/dxc.exe", "1367fd29d0ebba5bf10d1041a9deff85396d30090b3651d872ec65d11a476ea4"},
        {"bin/x64/dxcompiler.dll", "9b5e10ed756c461b4ec2c83a99f1d6ace20e97826e9c0b0e966b7b1cd6f2aec6"},
        {"bin/x64/dxil.dll", "cbcfe883a09fd0ca1f98abdf3a9553b560895e3283a136da82a8381253a169df"},
        {"bin/x64/dxv.exe", "f26242efb0197ffefa51eb5cb14603207d1d81fa547394020a5767bf65979f61"},
        {"LICENSE-LLVM.txt", "729615317e28dd03907e46f0fc3b5e88f7853cee61d1a1471d2749335516b46f"},
        {"LICENSE-MIT.txt", "903df5512f7d02609fed0c780a9b704f5a3eeb6e4d84ebe42a29845c81899a3c"},
        {"LICENSE-MS.txt", "734f72f239fe7b07b4c7203f294c1a7ce27095687278bab7e56d630d7c672963"},
        {"inc/hlsl/LICENSE.txt", "f47b94fc51b04f4354a6c6f41fb84b0bd22a8ebfcde9f93c1b4d9abd7f6d14c2"}};
    Require(pinned.at("components").size() == expected_components.size(), "incomplete retail compiler components");
    std::set<std::string> seen_components;
    for (const auto &component : pinned.at("components"))
    {
        const auto relative = component.at("path").get<std::string>();
        Require(!fs::path(relative).is_absolute() && relative.find("..") == std::string::npos &&
            relative.find(':') == std::string::npos, "unsafe compiler component");
        const auto expected = expected_components.find(relative);
        Require(expected != expected_components.end() && component.at("sha256") == expected->second &&
            seen_components.insert(relative).second, "retail compiler component differs from approved recipe");
        auto input = ap::HashInput("sdk/dxc69/" + relative, sdk / relative);
        Require(input.digest == "sha256:" + component.at("sha256").get<std::string>(), "compiler component authentication failed");
        spec.inputs.push_back(std::move(input));
    }
    for (const auto *relative : {"Schemas/material_graph.schema.json", "Source/Apps/RenderNativeRayShader/main.cpp",
         "Source/Apps/RenderMaterialShader/material_graph_parser.hpp", "Source/Apps/RenderMaterialShader/material_graph_parser.cpp",
         "Source/Apps/RenderMaterialShader/material_context_include.hpp", "Source/Apps/AssetPipeline/render_sdk_pin.hpp"})
        spec.inputs.push_back(ap::HashInput(relative, source / relative));
    for (auto names : {std::span<const std::string_view>(rd::kAuthoredMaterialNames),
                       std::span<const std::string_view>(rd::kHeroMaterialNames)})
        for (const auto name : names)
            spec.inputs.push_back(ap::HashInput("material/" + std::string(name) + "/source",
                source / ("ContentSource/Materials/" + std::string(name) + ".json")));
    const auto stage = ap::CreateStage(build / "Actions" / spec.namespace_path);
    try
    {
        const auto material_snapshot = stage / "material-snapshot";
        for (const auto &input : spec.inputs)
            if (input.logical_id.starts_with("material/") && input.logical_id.ends_with("/source"))
            {
                const auto target = material_snapshot / input.path.lexically_relative(source);
                ap::MakeDirectory(target.parent_path());
                const auto copy = ap::CopyAndHashInput(input.logical_id, input.path, target);
                Require(copy.digest == input.digest, "material source changed during snapshot");
            }
        const auto semantic_start = spec.inputs.size();
        const auto planes = rd::ComputeMaterialSidecarLayout(AddSurfaceReflectionInputs(spec, material_snapshot)).plane_count;
        const auto deferred = DeferredContextInclude(spec, material_snapshot);
        const auto hero = HeroContextInclude(spec, material_snapshot);
        const auto ray = RayEnvironmentContextInclude(spec, material_snapshot);
        for (std::size_t i = semantic_start; i < spec.inputs.size(); ++i)
            spec.inputs[i].path = source / spec.inputs[i].path.lexically_relative(material_snapshot);
        const auto helpers = stage / "helpers";
        ap::MakeDirectory(helpers);
        for (const auto &[name, text] : std::array{
            std::pair{"deferred_materials.generated.hlsli", &deferred},
            std::pair{"hero_materials.generated.hlsli", &hero},
            std::pair{"ray_materials.generated.hlsli", &ray}})
            ap::WriteBytes(helpers / name, std::as_bytes(std::span(text->data(), text->size())));
        if (nrd)
            for (const auto *id : {"nrd/NRDConfig.hlsli", "nrd/NRD.hlsli"})
            {
                ap::UpstreamProduct product; fs::path path;
                (void)ap::ReadActionResult(build, "render/shader/nrd", id, product, path);
                const auto copy = ap::CopyAndHashInput(product.logical_id, path, helpers / fs::path(id).filename());
                Require(copy.digest == product.digest, "NRD helper changed during snapshot");
                spec.upstream.push_back(std::move(product));
            }
        // Discover each exact macro/profile closure with retail DXC. Generated
        // helpers are authenticated semantic/upstream inputs above; every other
        // include must reside in the authoritative shader source directory.
        std::set<std::string> shader_paths;
        shader_paths.insert("Content/Shaders/entity_surface.hlsli");
        for (std::size_t i = 0; i < kPrograms.size(); ++i)
        {
            const auto depfile = stage / ("scan" + std::to_string(i) + ".d");
            const auto diagnostics = stage / ("scan" + std::to_string(i) + ".txt");
            Dxc(compiler, source, helpers, kPrograms[i], planes, nrd, depfile, diagnostics, true);
            for (const auto &relative : Dependencies(depfile))
            {
                const auto path = fs::absolute(fs::path(relative).is_absolute() ? fs::path(relative) : source / relative).lexically_normal();
                if (Within(helpers, path)) continue;
                Require(Within(source / "Content/Shaders", path),
                    "shader dependency escapes authenticated source closure: " + relative);
                shader_paths.insert(path.lexically_relative(source).generic_string());
            }
            fs::remove(depfile); fs::remove(diagnostics);
        }
        const auto snapshot = stage / "source-snapshot";
        for (const auto &relative : shader_paths)
        {
            auto input = ap::HashInput(relative, source / relative);
            ap::MakeDirectory((snapshot / relative).parent_path());
            const auto copy = ap::CopyAndHashInput(relative, input.path, snapshot / relative);
            Require(copy.digest == input.digest, "shader source changed during snapshot");
            spec.inputs.push_back(std::move(input));
        }
        const auto tools = stage / "tools";
        ap::MakeDirectory(tools);
        for (const auto &input : spec.inputs)
            if (input.logical_id.starts_with("sdk/dxc69/bin/x64/"))
            {
                const auto copy = ap::CopyAndHashInput(input.logical_id, input.path, tools / input.path.filename());
                Require(copy.digest == input.digest, "retail compiler changed during snapshot");
            }
        std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
        std::ranges::sort(spec.upstream, {}, &ap::UpstreamProduct::logical_id);
        std::vector<ap::Product> products;
        for (const auto &program : kPrograms)
        {
            const auto output = stage / program.filename;
            const auto diagnostics = stage / (std::string(program.filename) + ".txt");
            Dxc(tools / "dxc.exe", snapshot, helpers, program, planes, nrd, output, diagnostics, false);
            Require(ValidDxil(output), "compiler did not produce DXIL");
            products.push_back(ap::HashProduct("render/shader/" + std::string(program.filename), program.filename, output));
            fs::remove(diagnostics);
        }
        for (const auto &input : spec.inputs)
            if (!input.logical_id.ends_with("-ir") && !input.logical_id.ends_with("surface-reflection"))
                Require(ap::Sha256File(input.path) == input.digest, "producer input changed before publication");
        ap::RemoveStage(helpers); ap::RemoveStage(snapshot); ap::RemoveStage(tools); ap::RemoveStage(material_snapshot);
        ap::Publish(build, spec, stage, std::move(products));
        ap::RemoveStage(stage);
        std::cout << spec.action_id << " published programs=" << kPrograms.size() << '\n';
    }
    catch (...) { ap::RemoveStage(stage); throw; }
}
}
int wmain(int argc, wchar_t **argv)
{
    fs::path source, build, dxc, pin; bool nrd{};
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--with-nrd" && !nrd) { nrd = true; continue; }
            const auto read = [&](std::wstring_view prefix, fs::path &path) {
                if (!arg.starts_with(prefix) || !path.empty()) return false;
                path = arg.substr(prefix.size()); return !path.empty(); };
            Require(read(L"--source=", source) || read(L"--build=", build) ||
                read(L"--dxc=", dxc) || read(L"--pin=", pin), "invalid or duplicate producer argument");
        }
        Produce(source.lexically_normal(), build.lexically_normal(), dxc.lexically_normal(), pin.lexically_normal(), nrd);
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_native_ray_shader", error.what(),
                          {{"action_id", "render.shader.ray-native"}, {"with_nrd", nrd},
                           {"source_root", ap::DiagnosticPath(source)}, {"build_root", ap::DiagnosticPath(build)},
                           {"dependency", {{"dxc", ap::DiagnosticPath(dxc)}, {"pin", ap::DiagnosticPath(pin)}}}});
        return 1;
    }
}
