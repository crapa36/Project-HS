#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../AssetPipeline/action.hpp"
#include "material_graph_parser.hpp"
#include <hs/core/cooked_format.hpp>

#include <hs/core/windows_command_line.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/material_sidecar.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace ap = hs::asset_pipeline;
namespace rd = hs::render_data;
namespace fs = std::filesystem;

struct ProgramOutput
{
    std::string id;
    std::string filename;
    std::string source_relative;
    std::wstring_view entry;
    std::wstring_view profile;
    bool feedback{};
};

void Require(bool condition, std::string_view reason)
{
    if (!condition) throw std::runtime_error("hs_render_material_shader: " + std::string(reason));
}

bool ValidDxil(const fs::path &path)
{
    if (!fs::exists(path)) return false;
    const auto bytes = ap::ReadBytes(path);
    return bytes.size() >= 32 && bytes.substr(0, 4) == "DXBC";
}

ap::Input SemanticInput(std::string logical_id, const fs::path &dependency_path,
                        std::span<const std::byte> bytes)
{
    return ap::Input{std::move(logical_id), dependency_path, ap::Sha256Bytes(bytes)};
}

void AddSharedRendererShaderInputs(ap::ActionSpec &spec,const fs::path &source_root)
{
    // These common includes affect named material, hero, deferred and ray shaders.
    for(const auto *relative:{"Content/Shaders/local_probe_sampling.hlsli",
        "Content/Shaders/shadow_data.hlsli","Content/Shaders/local_shadow.hlsli",
        "Content/Shaders/ray_shadow_data.hlsli","Content/Shaders/volumetric_sampling.hlsli",
        "Content/Shaders/scene_hlod_sampling.hlsli", "Content/Shaders/texture_streaming.hlsli", "Content/Shaders/entity_surface.hlsli"})
        if(std::ranges::none_of(spec.inputs,[&](const auto &input) { return input.logical_id==relative; }))
            spec.inputs.push_back(ap::HashInput(relative,source_root/relative));
}

bool SameMaterialParameters(std::span<const rd::MaterialParameterDefinition> left,
                            std::span<const rd::MaterialParameterDefinition> right)
{
    if (left.size() != right.size()) return false;
    return std::ranges::equal(left, right, [](const auto &a, const auto &b) {
        return a.id == b.id && a.type == b.type && a.default_words == b.default_words &&
               a.uint_min == b.uint_min && a.uint_max == b.uint_max;
    });
}

std::size_t MaterialOrdinal(std::string_view name)
{
    const auto found = std::ranges::find(rd::kAuthoredMaterialNames, name);
    Require(found != rd::kAuthoredMaterialNames.end(), "material ordinal is not authored");
    return static_cast<std::size_t>(found - rd::kAuthoredMaterialNames.begin());
}

#include "material_context_include.hpp"

void RunDxc(const fs::path &dxc, const fs::path &source_root,
            const fs::path &stage, const fs::path &output,
            const fs::path &source, std::wstring_view entry,
            std::wstring_view profile, std::wstring_view define,
            std::uint32_t sidecar_planes = 0, bool generated_deferred = false, bool with_nrd = false,
            bool feedback = false)
{
    auto diagnostics = output;
    diagnostics += ".diagnostics.txt";
    std::vector<std::wstring> args{
        L"-nologo", L"-WX", L"-O3", L"-Ges",
        L"-E", std::wstring(entry), L"-T", std::wstring(profile),
        L"-I", stage.wstring(),
        L"-I", (source_root / "Content/Shaders").wstring(),
        L"-Fo", output.wstring(),
        L"-Fe", diagnostics.wstring(),
        source.wstring()};
    if (!define.empty()) args.emplace_back(define);
    if (generated_deferred) args.emplace_back(L"-DHS_GENERATED_DEFERRED_MATERIALS=1");
    if (with_nrd) args.emplace_back(L"-DHS_WITH_NRD=1");
    if (feedback) args.emplace_back(L"-DHS_SAMPLER_FEEDBACK=1");
    args.emplace_back(L"-DHS_SURFACE_SIDECAR_PLANES=" + std::to_wstring(sidecar_planes));
    std::wstring command = hs::QuoteWindowsCommandLineArgument(dxc.wstring());
    for (const auto &arg : args)
    {
        command += L' ';
        command += hs::QuoteWindowsCommandLineArgument(arg);
    }
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(dxc.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("hs_render_material_shader: DXC launch failed: " +
                                 std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, 120'000);
    if (wait == WAIT_TIMEOUT)
        TerminateProcess(process.hProcess, 1);
    DWORD code = 1;
    const auto got_code = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !got_code || code != 0)
        throw std::runtime_error("hs_render_material_shader: DXC compilation failed for " +
            output.filename().string() + ": " +
            (fs::exists(diagnostics) ? ap::ReadBytes(diagnostics) : "no diagnostic output"));
}

void ProducePreintegration(const fs::path &source_root, const fs::path &build_root,
                           const fs::path &dxc, bool with_nrd, bool with_brixelizer)
{
    Require(source_root.is_absolute() && build_root.is_absolute() && dxc.is_absolute(),
            "all paths must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    ap::RequireRegularFile(dxc);
    ap::ActionSpec spec;
    spec.action_id = "render.shader.bsdf-preintegrate";
    spec.recipe_id = "project-hs.render.bsdf-preintegrate";
    spec.recipe_version = 10;
    spec.namespace_path = "render/shader/bsdf-preintegrate";
    std::vector<std::pair<ap::UpstreamProduct, fs::path>> nrd_inputs;
    if (with_nrd)
    {
        for (const auto *id : {"nrd/NRDConfig.hlsli", "nrd/NRD.hlsli", "nrd/provenance.json"})
        {
            ap::UpstreamProduct upstream; fs::path path;
            (void)ap::ReadActionResult(build_root, "render/shader/nrd", id, upstream, path);
            spec.upstream.push_back(upstream); nrd_inputs.emplace_back(std::move(upstream), std::move(path));
        }
        spec.inputs.push_back(ap::HashInput("Content/Shaders/ray_nrd_conversion.hlsl", source_root / "Content/Shaders/ray_nrd_conversion.hlsl"));
    }
    if (with_brixelizer)
        for (const auto *relative : {"Content/Shaders/brixelizer_common.hlsli", "Content/Shaders/brixelizer_guides.hlsl",
                                   "Content/Shaders/brixelizer_gi_resolve.hlsl",
                                   "Content/Shaders/brixelizer_gi_compose.hlsl",
                                   "Content/Shaders/brixelizer_cache_export.hlsl"})
            spec.inputs.push_back(ap::HashInput(relative, source_root / relative));
    const auto generated = DeferredContextInclude(spec, source_root);
    const auto sidecar_layout = rd::ComputeMaterialSidecarLayout(AddSurfaceReflectionInputs(spec, source_root));
    const auto hero_context = HeroContextInclude(spec, source_root);
    const auto ray_context = RayEnvironmentContextInclude(spec, source_root);
    spec.inputs.push_back(ap::HashInput("Schemas/material_graph.schema.json",
                                      source_root / "Schemas/material_graph.schema.json"));
    for (const auto *relative : {"Content/Shaders/bsdf_preintegrate.hlsl",
                                 "Content/Shaders/bsdf_metallic_roughness.hlsli",
                                 "Content/Shaders/bsdf_layers.hlsli",
                                 "Content/Shaders/material_surface.hlsli",
                                 "Content/Shaders/hero_material.hlsli",
                                 "Content/Shaders/material_sidecar.hlsli",
                                 "Content/Shaders/material_parameters.hlsli",
                                 "Content/Shaders/deferred_surface.hlsli",
                                 "Content/Shaders/subsurface_diffusion.hlsl",
                                 "Content/Shaders/screen_reflections.hlsl",
                                 "Content/Shaders/vfx_screen_overlay.hlsl",
                                 "Content/Shaders/adaptive_shading.hlsl",
                                 "Content/Shaders/adaptive_gtao.hlsl",
                                 "Content/Shaders/ambient_occlusion.hlsl",
                                 "Content/Shaders/project_hs.hlsl",
                                "Content/Shaders/environment.hlsli",
                                "Content/Shaders/probe_ibl.hlsli",
                                "Content/Shaders/light_list.hlsli",
                                "Content/Shaders/ltc_area.hlsli",
                                "Content/Shaders/ray_hit.hlsli",
                                "Content/Shaders/ray_geometry.hlsl",
                                 "Content/Shaders/ray_ambient_occlusion.hlsl",
                                 "Content/Shaders/ray_transport.hlsli",
                                 "Content/Shaders/ray_radiance.hlsli",
                                 "Content/Shaders/ray_native_trace.hlsli",
                                 "Content/Shaders/ray_gi.hlsl",
                                 "Content/Shaders/ray_reflections.hlsl",
                                 "Content/Shaders/ray_shadow.hlsl",
                                 "Content/Shaders/ray_transport_composite.hlsl",
                                 "Content/Shaders/ray_reconstruction_guides.hlsl",
                                 "Content/Shaders/shadow_data.hlsli",
                                 "Content/Shaders/local_shadow.hlsli",
                                 "Content/Shaders/ray_shadow_data.hlsli",
                                 "Content/Shaders/gpu_scene_tables.hlsli",
                                "Content/Shaders/geometry.hlsli",
                                "Content/Shaders/geometry_records.hlsli",
                                "Content/Shaders/surface_layout.hlsli",
                                "Content/Shaders/temporal_reconstruction.hlsli",
                                "Content/Shaders/display_transform.hlsli",
                                "Content/Shaders/vfx_ribbon.hlsli"})
        spec.inputs.push_back(ap::HashInput(relative, source_root / relative));
    AddSharedRendererShaderInputs(spec,source_root);
    spec.inputs.push_back(ap::HashInput("tool/dxc.exe", dxc));
    for (const auto *library : {"dxcompiler.dll", "dxil.dll"})
        spec.inputs.push_back(ap::HashInput(std::string("tool/") + library,
                                          dxc.parent_path() / library));
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    std::vector outputs{
        ProgramOutput{"render/shader/rr_diffuse_pack_cs.dxil", "rr_diffuse_pack_cs.dxil",
                      "Content/Shaders/ray_reconstruction_guides.hlsl", L"RRDiffusePackCS", L"cs_6_6"},
        ProgramOutput{"render/shader/rr_diffuse_resolve_cs.dxil", "rr_diffuse_resolve_cs.dxil",
                      "Content/Shaders/ray_reconstruction_guides.hlsl", L"RRDiffuseResolveCS", L"cs_6_6"},
        ProgramOutput{"render/shader/rr_specular_pack_cs.dxil", "rr_specular_pack_cs.dxil",
                      "Content/Shaders/ray_reconstruction_guides.hlsl", L"RRSpecularPackCS", L"cs_6_6"},
        ProgramOutput{"render/shader/rr_specular_resolve_cs.dxil", "rr_specular_resolve_cs.dxil",
                      "Content/Shaders/ray_reconstruction_guides.hlsl", L"RRSpecularResolveCS", L"cs_6_6"},
        ProgramOutput{"render/shader/dlss_rr_guides_cs.dxil", "dlss_rr_guides_cs.dxil",
                      "Content/Shaders/ray_reconstruction_guides.hlsl", L"DLSSRRGuidesCS", L"cs_6_6"},
        ProgramOutput{"render/shader/rr_pre_exposure_cs.dxil", "rr_pre_exposure_cs.dxil",
                      "Content/Shaders/ray_reconstruction_guides.hlsl", L"RRPreExposureCS", L"cs_6_6"},
        ProgramOutput{"render/shader/rtgi_trace_cs.dxil", "rtgi_trace_cs.dxil",
                      "Content/Shaders/ray_gi.hlsl", L"GITraceCS", L"cs_6_5"},
        ProgramOutput{"render/shader/rtgi_temporal_cs.dxil", "rtgi_temporal_cs.dxil",
                      "Content/Shaders/ray_gi.hlsl", L"GITemporalCS", L"cs_6_5"},
        ProgramOutput{"render/shader/rtgi_spatial_cs.dxil", "rtgi_spatial_cs.dxil",
                      "Content/Shaders/ray_gi.hlsl", L"GISpatialCS", L"cs_6_5"},
        ProgramOutput{"render/shader/hybrid_depth_init_cs.dxil", "hybrid_depth_init_cs.dxil",
                      "Content/Shaders/ray_reflections.hlsl", L"HybridDepthInitCS", L"cs_6_5"},
        ProgramOutput{"render/shader/hybrid_depth_reduce_cs.dxil", "hybrid_depth_reduce_cs.dxil",
                      "Content/Shaders/ray_reflections.hlsl", L"HybridDepthReduceCS", L"cs_6_5"},
        ProgramOutput{"render/shader/hybrid_reflection_trace_cs.dxil", "hybrid_reflection_trace_cs.dxil",
                      "Content/Shaders/ray_reflections.hlsl", L"HybridReflectionTraceCS", L"cs_6_5"},
        ProgramOutput{"render/shader/hybrid_reflection_temporal_cs.dxil", "hybrid_reflection_temporal_cs.dxil",
                      "Content/Shaders/ray_reflections.hlsl", L"HybridReflectionTemporalCS", L"cs_6_5"},
        ProgramOutput{"render/shader/hybrid_reflection_spatial_cs.dxil", "hybrid_reflection_spatial_cs.dxil",
                      "Content/Shaders/ray_reflections.hlsl", L"HybridReflectionSpatialCS", L"cs_6_5"},
        ProgramOutput{"render/shader/ray_transport_compose_cs.dxil", "ray_transport_compose_cs.dxil",
                      "Content/Shaders/ray_transport_composite.hlsl", L"TransportComposeCS", L"cs_6_5"},
        ProgramOutput{"render/shader/rtao_trace_cs.dxil", "rtao_trace_cs.dxil",
                      "Content/Shaders/ray_ambient_occlusion.hlsl", L"RTAOTraceCS", L"cs_6_5"},
        ProgramOutput{"render/shader/rtao_temporal_cs.dxil", "rtao_temporal_cs.dxil",
                      "Content/Shaders/ray_ambient_occlusion.hlsl", L"RTAOTemporalCS", L"cs_6_5"},
        ProgramOutput{"render/shader/rtao_resolve_cs.dxil", "rtao_resolve_cs.dxil",
                      "Content/Shaders/ray_ambient_occlusion.hlsl", L"RTAOResolveCS", L"cs_6_5"},
        ProgramOutput{"render/shader/ray_shadow_trace_cs.dxil", "ray_shadow_trace_cs.dxil",
                      "Content/Shaders/ray_shadow.hlsl", L"RayShadowTraceCS", L"cs_6_5"},
        ProgramOutput{"render/shader/ray_shadow_temporal_cs.dxil", "ray_shadow_temporal_cs.dxil",
                      "Content/Shaders/ray_shadow.hlsl", L"RayShadowTemporalCS", L"cs_6_5"},
        ProgramOutput{"render/shader/ray_shadow_resolve_cs.dxil", "ray_shadow_resolve_cs.dxil",
                      "Content/Shaders/ray_shadow.hlsl", L"RayShadowResolveCS", L"cs_6_5"},
        ProgramOutput{"render/shader/bsdf_preintegrate_cs.dxil", "bsdf_preintegrate_cs.dxil",
                      "Content/Shaders/bsdf_preintegrate.hlsl", L"main", L"cs_6_0"},
        ProgramOutput{"render/shader/deferred_ps.dxil", "deferred_ps.dxil",
                      "Content/Shaders/project_hs.hlsl", L"DeferredPS", L"ps_6_0"},
        ProgramOutput{"render/shader/sss_horizontal_cs.dxil", "sss_horizontal_cs.dxil",
                      "Content/Shaders/subsurface_diffusion.hlsl", L"SubsurfaceHorizontalCS", L"cs_6_0"},
        ProgramOutput{"render/shader/sss_vertical_cs.dxil", "sss_vertical_cs.dxil",
                      "Content/Shaders/subsurface_diffusion.hlsl", L"SubsurfaceVerticalCS", L"cs_6_0"},
        ProgramOutput{"render/shader/ssr_depth_init_cs.dxil", "ssr_depth_init_cs.dxil",
                      "Content/Shaders/screen_reflections.hlsl", L"ScreenReflectionDepthInitCS", L"cs_6_0"},
        ProgramOutput{"render/shader/ssr_depth_reduce_cs.dxil", "ssr_depth_reduce_cs.dxil",
                      "Content/Shaders/screen_reflections.hlsl", L"ScreenReflectionDepthReduceCS", L"cs_6_0"},
        ProgramOutput{"render/shader/ssr_trace_cs.dxil", "ssr_trace_cs.dxil",
                      "Content/Shaders/screen_reflections.hlsl", L"ScreenReflectionTraceCS", L"cs_6_0"},
        ProgramOutput{"render/shader/ssr_temporal_cs.dxil", "ssr_temporal_cs.dxil",
                      "Content/Shaders/screen_reflections.hlsl", L"ScreenReflectionTemporalCS", L"cs_6_0"},
        ProgramOutput{"render/shader/ssr_spatial_cs.dxil", "ssr_spatial_cs.dxil",
                      "Content/Shaders/screen_reflections.hlsl", L"ScreenReflectionSpatialCS", L"cs_6_0"},
        ProgramOutput{"render/shader/ssr_compose_cs.dxil", "ssr_compose_cs.dxil",
                      "Content/Shaders/screen_reflections.hlsl", L"ScreenReflectionComposeCS", L"cs_6_0"},
        ProgramOutput{"render/shader/vfx_screen_overlay_mask_cs.dxil", "vfx_screen_overlay_mask_cs.dxil",
                      "Content/Shaders/vfx_screen_overlay.hlsl", L"ScreenOverlayMaskCS", L"cs_6_0"},
        ProgramOutput{"render/shader/vfx_screen_overlay_vs.dxil", "vfx_screen_overlay_vs.dxil",
                      "Content/Shaders/vfx_screen_overlay.hlsl", L"ScreenOverlayVS", L"vs_6_0"},
        ProgramOutput{"render/shader/vfx_screen_overlay_ps.dxil", "vfx_screen_overlay_ps.dxil",
                      "Content/Shaders/vfx_screen_overlay.hlsl", L"ScreenOverlayPS", L"ps_6_0"},
        ProgramOutput{"render/shader/adaptive_shading_classify_cs.dxil", "adaptive_shading_classify_cs.dxil",
                      "Content/Shaders/adaptive_shading.hlsl", L"AdaptiveShadingClassifyCS", L"cs_6_0"},
        ProgramOutput{"render/shader/adaptive_shading_rate_cs.dxil", "adaptive_shading_rate_cs.dxil",
                      "Content/Shaders/adaptive_shading.hlsl", L"AdaptiveShadingRateCS", L"cs_6_0"},
        ProgramOutput{"render/shader/adaptive_gtao_horizon_cs.dxil", "adaptive_gtao_horizon_cs.dxil",
                      "Content/Shaders/adaptive_gtao.hlsl", L"AdaptiveGtaoHorizonCS", L"cs_6_0"},
        ProgramOutput{"render/shader/adaptive_gtao_resolve_cs.dxil", "adaptive_gtao_resolve_cs.dxil",
                      "Content/Shaders/adaptive_gtao.hlsl", L"AdaptiveGtaoResolveCS", L"cs_6_0"},
        ProgramOutput{"render/shader/hero_transparency_vs.dxil", "hero_transparency_vs.dxil",
                      "Content/Shaders/project_hs.hlsl", L"SceneVS", L"vs_6_0"},
        ProgramOutput{"render/shader/hero_peel_front_ps.dxil", "hero_peel_front_ps.dxil",
                      "Content/Shaders/project_hs.hlsl", L"HeroPeelFrontPS", L"ps_6_0"},
        ProgramOutput{"render/shader/hero_peel_exit_ps.dxil", "hero_peel_exit_ps.dxil",
                      "Content/Shaders/project_hs.hlsl", L"HeroPeelExitPS", L"ps_6_0"},
        ProgramOutput{"render/shader/hero_oit_tail_ps.dxil", "hero_oit_tail_ps.dxil",
                      "Content/Shaders/project_hs.hlsl", L"HeroOitTailPS", L"ps_6_0"},
        ProgramOutput{"render/shader/hero_transparent_guide_ps.dxil", "hero_transparent_guide_ps.dxil",
                      "Content/Shaders/project_hs.hlsl", L"HeroTransparentGuidePS", L"ps_6_0"},
        ProgramOutput{"render/shader/hero_layer_composite_ps.dxil", "hero_layer_composite_ps.dxil",
                      "Content/Shaders/project_hs.hlsl", L"HeroLayerCompositePS", L"ps_6_0"}};
    if (with_nrd)
    {
        outputs.push_back({"render/shader/rtgi_nrd_pack_cs.dxil", "rtgi_nrd_pack_cs.dxil", "Content/Shaders/ray_nrd_conversion.hlsl", L"RtGiNrdPackCS", L"cs_6_5"});
        outputs.push_back({"render/shader/rtgi_nrd_resolve_cs.dxil", "rtgi_nrd_resolve_cs.dxil", "Content/Shaders/ray_nrd_conversion.hlsl", L"RtGiNrdResolveCS", L"cs_6_5"});
        outputs.push_back({"render/shader/reflection_nrd_pack_cs.dxil", "reflection_nrd_pack_cs.dxil", "Content/Shaders/ray_nrd_conversion.hlsl", L"RtSpecularNrdPackCS", L"cs_6_5"});
        outputs.push_back({"render/shader/reflection_nrd_resolve_cs.dxil", "reflection_nrd_resolve_cs.dxil", "Content/Shaders/ray_nrd_conversion.hlsl", L"RtSpecularNrdResolveCS", L"cs_6_5"});
    }
    if (with_brixelizer)
    {
        outputs.push_back({"render/shader/brixelizer_guides_cs.dxil", "brixelizer_guides_cs.dxil", "Content/Shaders/brixelizer_guides.hlsl", L"BrixelizerGuidesCS", L"cs_6_6"});
        outputs.push_back({"render/shader/brixelizer_gi_resolve_cs.dxil", "brixelizer_gi_resolve_cs.dxil", "Content/Shaders/brixelizer_gi_resolve.hlsl", L"BrixelizerGiResolveCS", L"cs_6_6"});
        outputs.push_back({"render/shader/brixelizer_gi_compose_cs.dxil", "brixelizer_gi_compose_cs.dxil", "Content/Shaders/brixelizer_gi_compose.hlsl", L"BrixelizerGiComposeCS", L"cs_6_6"});
        outputs.push_back({"render/shader/brixelizer_cache_export_cs.dxil", "brixelizer_cache_export_cs.dxil", "Content/Shaders/brixelizer_cache_export.hlsl", L"BrixelizerCacheExportCS", L"cs_6_6"});
        outputs.push_back({"render/shader/brixelizer_cache_direct_ps.dxil", "brixelizer_cache_direct_ps.dxil", "Content/Shaders/project_hs.hlsl", L"BrixelizerCacheDirectPS", L"ps_6_6"});
    }
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
    {
        try
        {
            const auto result = ap::Json::parse(ap::ReadBytes(result_path));
            ap::UpstreamProduct ignored;
            fs::path path;
            if (result.at("action_key") == ap::ActionKey(spec) &&
                result.at("action_id") == spec.action_id &&
                result.at("products").size() == outputs.size())
            {
                bool valid = true;
                for (const auto &output : outputs)
                {
                    (void)ap::ReadActionResult(build_root, spec.namespace_path, output.id,
                                               ignored, path);
                    valid &= ValidDxil(path);
                }
                if (valid)
                {
                    ap::TouchActionResult(result_path);
                    std::cout << spec.action_id << " current\n";
                    return;
                }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current result. */ }
    }
    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        for (const auto &[upstream, path] : nrd_inputs)
            if (path.extension() == ".hlsli")
            {
                const auto staged = ap::CopyAndHashInput(upstream.logical_id, path, stage / path.filename());
                Require(staged.digest == upstream.digest, "pinned NRD helper changed while creating the shader snapshot");
            }
        const auto include_path = stage / "deferred_materials.generated.hlsli";
        ap::WriteBytes(include_path, std::as_bytes(std::span(generated.data(), generated.size())));
        const auto hero_include_path = stage / "hero_materials.generated.hlsli";
        ap::WriteBytes(hero_include_path, std::as_bytes(std::span(hero_context.data(), hero_context.size())));
        const auto ray_include_path = stage / "ray_materials.generated.hlsli";
        ap::WriteBytes(ray_include_path, std::as_bytes(std::span(ray_context.data(), ray_context.size())));
        std::vector<ap::Product> products;
        for (const auto &program : outputs)
        {
            const auto output = stage / program.filename;
            const bool native_guides = program.id.starts_with("render/shader/rr_") ||
                program.id.starts_with("render/shader/dlss_rr_") ||
                (program.id.starts_with("render/shader/brixelizer_") && program.profile.starts_with(L"cs_"));
            const auto define = (program.id.starts_with("render/shader/hero_") ||
                                 program.id.starts_with("render/shader/rtao_") ||
                                 program.id.starts_with("render/shader/rtgi_") ||
                                 program.id.starts_with("render/shader/hybrid_") ||
                                 program.id.starts_with("render/shader/ray_shadow_") ||
                                 program.id.starts_with("render/shader/reflection_nrd_") ||
                                 program.id.starts_with("render/shader/ray_transport_") || native_guides)
                ? std::wstring_view{L"-DHS_GENERATED_HERO_MATERIAL=1"}
                : program.entry != L"main" ? std::wstring_view{L"-DHS_GENERATED_DEFERRED_MATERIALS=1"} : std::wstring_view{};
            RunDxc(dxc, source_root, stage, output, source_root / program.source_relative,
                   program.entry, program.profile, define, sidecar_layout.plane_count,
                   program.id.starts_with("render/shader/rtgi_") ||
                   program.id.starts_with("render/shader/hybrid_") ||
                   program.id.starts_with("render/shader/ray_shadow_") ||
                   program.id.starts_with("render/shader/reflection_nrd_") ||
                   program.id.starts_with("render/shader/ray_transport_") || native_guides, with_nrd);
            Require(ValidDxil(output), "DXC did not produce a DXIL container");
            products.push_back(ap::HashProduct(program.id, program.filename, output));
        }
        fs::remove(include_path);
        fs::remove(hero_include_path);
        fs::remove(ray_include_path);
        if (with_nrd) { fs::remove(stage / "NRD.hlsli"); fs::remove(stage / "NRDConfig.hlsli"); }
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}

void ProduceParameters(std::string_view name, const fs::path &source_root,
                       const fs::path &build_root)
{
    const bool decal = hs::material_content::IsDecalMaterialName(name);
    Require(hs::material_content::IsAuthoredMaterialName(name) || decal,
            "unsupported material name");
    Require(source_root.is_absolute() && build_root.is_absolute(),
            "all paths must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);

    const auto material_relative = "ContentSource/Materials/" + std::string(name) + ".json";
    const auto schema_relative = decal ? "Schemas/decal_material_graph.schema.json" : "Schemas/material_graph.schema.json";
    const auto material_path = source_root / material_relative;
    const auto schema_path = source_root / schema_relative;
    ap::RequireRegularFile(material_path);
    ap::RequireRegularFile(schema_path);
    const auto graph = decal ? hs::material_content::ParseDecalMaterialGraph(name, ap::ReadBytes(material_path)) :
        hs::material_content::ParseMaterialDefinition(name, ap::ReadBytes(material_path)).graph;
    const std::span<const rd::MaterialParameterDefinition> parameters =
        graph.parameters;
    rd::ValidateMaterialParameters(parameters);

    const std::string product_id = "render/material/" + std::string(name) +
                                   "/parameters.rpkg";
    ap::ActionSpec spec;
    spec.action_id = "render.parameters." + std::string(name);
    spec.recipe_id = "project-hs.render.material-parameters";
    spec.recipe_version = 1;
    spec.namespace_path = "render/material/" + std::string(name) + "/parameters";
    spec.inputs.push_back(ap::HashInput(material_relative, material_path));
    spec.inputs.push_back(ap::HashInput(schema_relative, schema_path));
    if (decal) spec.inputs.push_back(ap::HashInput("Schemas/material_graph.schema.json", source_root / "Schemas/material_graph.schema.json"));
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);

    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
    {
        try
        {
            const auto result = ap::Json::parse(ap::ReadBytes(result_path));
            if (result.at("action_id") == spec.action_id &&
                result.at("action_key") == ap::ActionKey(spec) &&
                result.at("products").is_array() && result.at("products").size() == 1 &&
                result.at("products").at(0).at("logical_id") == product_id)
            {
                ap::UpstreamProduct ignored;
                fs::path package_path;
                (void)ap::ReadActionResult(build_root, spec.namespace_path, product_id,
                                           ignored, package_path);
                const auto package = ap::ReadBytes(package_path);
                const auto decoded = rd::ReadMaterialParameterPackage(
                    std::as_bytes(std::span(package.data(), package.size())),
                    hs::MakeAssetId(product_id));
                rd::ValidateMaterialParameters(decoded);
                if (rd::MaterialParameterLayoutHash(decoded) ==
                        rd::MaterialParameterLayoutHash(parameters) &&
                    SameMaterialParameters(decoded, parameters))
                {
                    ap::TouchActionResult(result_path);
                    std::cout << spec.action_id << " current\n";
                    return;
                }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current result. */ }
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto package = rd::BuildMaterialParameterPackage(product_id, parameters);
        const auto output = stage / "parameters.rpkg";
        ap::WriteBytes(output, package);
        const auto decoded = rd::ReadMaterialParameterPackage(
            std::as_bytes(std::span(package.data(), package.size())),
            hs::MakeAssetId(product_id));
        rd::ValidateMaterialParameters(decoded);
        Require(rd::MaterialParameterLayoutHash(decoded) ==
                    rd::MaterialParameterLayoutHash(parameters) &&
                    SameMaterialParameters(decoded, parameters),
                "material parameter package layout mismatch");
        std::vector<ap::Product> products;
        products.push_back(ap::HashProduct(product_id, output.filename().string(), output));
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}

void ProduceDecalSurface(const fs::path &source_root, const fs::path &build_root, const fs::path &dxc)
{
    Require(source_root.is_absolute() && build_root.is_absolute() && dxc.is_absolute(), "all paths must be absolute");
    ap::RequireDirectory(source_root); ap::MakeDirectory(build_root); ap::RequireRegularFile(dxc);
    ap::ActionSpec spec;
    spec.action_id = "render.shader.decal-surface";
    spec.recipe_id = "project-hs.render.decal-surface";
    spec.recipe_version = 1;
    spec.namespace_path = "render/shader/decal-surface";
    std::vector<rd::MaterialGraph> graphs;
    std::string generated;
    for (std::size_t ordinal = 0; ordinal < rd::kDecalMaterialNames.size(); ++ordinal)
    {
        const std::string name(rd::kDecalMaterialNames[ordinal]);
        const auto path = source_root / ("ContentSource/Materials/" + name + ".json");
        auto graph = hs::material_content::ParseDecalMaterialGraph(name, ap::ReadBytes(path));
        const std::array layer_counts{ordinal == 0 ? 4u : 8u};
        rd::ValidateMaterialSampleLayers(graph, layer_counts, layer_counts.front() - 1u);
        auto function = rd::EmitDecalMaterialContextHlsl(graph, "HsDecalEvaluate_" + std::to_string(ordinal),
            8u + static_cast<std::uint32_t>(ordinal));
        const std::string texture_name = "HsDecalTexture_" + std::to_string(ordinal);
        std::size_t position{};
        while ((position = function.find("HsMaterialTexture0", position)) != std::string::npos)
        {
            function.replace(position, 18, texture_name); position += texture_name.size();
        }
        generated += function;
        spec.inputs.push_back(SemanticInput("material/" + name + "/decal-ir", path,
            std::as_bytes(std::span(function.data(), function.size()))));
        // Programs depend on reflected layout, bindings and receiver policy.
        // Mutable defaults belong to the separate parameters action. Retain
        // the authored path in the depfile and full source authentication.
        const auto reflection = rd::BuildDecalMaterialPackage(name, graph.parameters,
            graph.textures.front().product_id, "render/shader/decal_surface_cs.dxil", graph.decal_receivers);
        spec.inputs.push_back(SemanticInput("material/" + name + "/decal-package", path, reflection));
        graphs.push_back(std::move(graph));
    }
    generated += "HsSurfaceClosure HsDecalEvaluate(HsDecalContext ctx, uint ordinal)\n{\n    switch (ordinal)\n    {\n";
    for (std::size_t ordinal = 0; ordinal < graphs.size(); ++ordinal)
        generated += "    case " + std::to_string(ordinal) + "u: return HsDecalEvaluate_" + std::to_string(ordinal) + "(ctx);\n";
    generated += "    default: return HsMaterialMakeSurfaceClosure(ctx.sampled_base_color, ctx.normal, ctx.sampled_roughness, 0.0, 1.0, 0.0);\n    }\n}\n";
    for (const auto *relative : {"Schemas/decal_material_graph.schema.json", "Schemas/material_graph.schema.json", "Content/Shaders/decal_surface.hlsl",
        "Content/Shaders/material_surface.hlsli", "Content/Shaders/material_parameters.hlsli"})
        spec.inputs.push_back(ap::HashInput(relative, source_root / relative));
    spec.inputs.push_back(ap::HashInput("tool/dxc.exe", dxc));
    for (const auto *library : {"dxcompiler.dll", "dxil.dll"})
        spec.inputs.push_back(ap::HashInput(std::string("tool/") + library, dxc.parent_path() / library));
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    constexpr std::string_view shader_id = "render/shader/decal_surface_cs.dxil";
    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
    {
        try
        {
            const auto result = ap::Json::parse(ap::ReadBytes(result_path));
            if (result.at("action_id") == spec.action_id && result.at("action_key") == ap::ActionKey(spec) &&
                result.at("products").size() == graphs.size() + 1)
            {
                ap::UpstreamProduct ignored; fs::path path;
                (void)ap::ReadActionResult(build_root, spec.namespace_path, shader_id, ignored, path);
                bool valid = ValidDxil(path);
                for (std::size_t ordinal = 0; ordinal < graphs.size(); ++ordinal)
                {
                    const std::string name(rd::kDecalMaterialNames[ordinal]);
                    (void)ap::ReadActionResult(build_root, spec.namespace_path,
                        "render/material/" + name + "/material.rpkg", ignored, path);
                    const auto bytes = ap::ReadBytes(path);
                    const auto material = rd::ReadDecalMaterialPackage(std::as_bytes(std::span(bytes.data(), bytes.size())), name);
                    valid = valid && material.receivers == graphs[ordinal].decal_receivers &&
                        material.parameter_layout_hash == rd::MaterialParameterLayoutHash(graphs[ordinal].parameters);
                }
                if (valid) { ap::TouchActionResult(result_path); std::cout << spec.action_id << " current\n"; return; }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current output. */ }
    }
    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto include = stage / "decal_materials.generated.hlsli";
        ap::WriteBytes(include, std::as_bytes(std::span(generated.data(), generated.size())));
        const auto shader = stage / "decal_surface_cs.dxil";
        RunDxc(dxc, source_root, stage, shader, source_root / "Content/Shaders/decal_surface.hlsl",
            L"DecalSurfaceCS", L"cs_6_0", L"");
        Require(ValidDxil(shader), "DXC did not produce a decal DXIL container");
        std::vector<ap::Product> products{ap::HashProduct(std::string(shader_id), shader.filename().string(), shader)};
        for (std::size_t ordinal = 0; ordinal < graphs.size(); ++ordinal)
        {
            const std::string name(rd::kDecalMaterialNames[ordinal]);
            const auto &graph = graphs[ordinal];
            const auto package = rd::BuildDecalMaterialPackage(name, graph.parameters,
                graph.textures.front().product_id, shader_id, graph.decal_receivers);
            const auto decoded = rd::ReadDecalMaterialPackage(package, name);
            const auto output = stage / (name + ".rpkg");
            ap::WriteBytes(output, package);
            auto product = ap::HashProduct("render/material/" + name + "/material.rpkg", output.filename().string(), output);
            product.dependencies = decoded.dependencies; products.push_back(std::move(product));
        }
        fs::remove(include); ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (...) { ap::RemoveStage(stage); throw; }
    ap::RemoveStage(stage); std::cout << spec.action_id << " published\n";
}

void ProduceHero(std::string_view name, const fs::path &source_root,
                 const fs::path &build_root, const fs::path &dxc)
{
    Require(source_root.is_absolute() && build_root.is_absolute() && dxc.is_absolute(), "all paths must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    ap::RequireRegularFile(dxc);
    const auto graph = hs::material_content::ParseMaterialDefinition(name,
        ap::ReadBytes(source_root / ("ContentSource/Materials/" + std::string(name) + ".json"))).graph;
    const auto reflection = rd::AnalyzeClosure(graph);
    const auto namespace_path = "render/shader/" + std::string(name);
    const auto material_id = "render/material/" + std::string(name) + "/material.rpkg";
    std::vector<ProgramOutput> outputs;
    std::vector<rd::MaterialProgramKind> kinds;
    if (graph.kind == rd::MaterialGraphKind::Translucent || graph.kind == rd::MaterialGraphKind::HeroRefractive)
    {
        outputs.push_back({namespace_path + "_translucent.dxil", std::string(name) + "_translucent.dxil",
            "Content/Shaders/project_hs.hlsl", L"HeroTranslucentPS", L"ps_6_0"});
        kinds.push_back(rd::MaterialProgramKind::HeroTranslucentPS);
    }
    else
    {
        outputs = {
            {namespace_path + "_scene.dxil", std::string(name) + "_scene.dxil", "Content/Shaders/project_hs.hlsl", L"HeroScenePS", L"ps_6_0"},
            {namespace_path + "_visibility.dxil", std::string(name) + "_visibility.dxil", "Content/Shaders/visibility_buffer.hlsl", L"HeroVisibilityPS", L"ps_6_0"},
            {namespace_path + "_resolve.dxil", std::string(name) + "_resolve.dxil", "Content/Shaders/visibility_buffer.hlsl", L"HeroSurfaceResolveCS", L"cs_6_0"},
            {namespace_path + "_shadow.dxil", std::string(name) + "_shadow.dxil", "Content/Shaders/project_hs.hlsl", L"HeroShadowPS", L"ps_6_0"}};
        kinds = {rd::MaterialProgramKind::HeroScenePS, rd::MaterialProgramKind::HeroVisibilityPS,
                 rd::MaterialProgramKind::HeroSurfaceResolveCS, rd::MaterialProgramKind::HeroShadowPS};
    }
    ap::ActionSpec spec;
    spec.action_id = "render.shader." + std::string(name);
    spec.recipe_id = "project-hs.render.material-shader";
    spec.recipe_version = 10;
    spec.namespace_path = namespace_path;
    const auto generated = HeroContextInclude(spec, source_root);
    const auto sidecar_layout = rd::ComputeMaterialSidecarLayout(AddSurfaceReflectionInputs(spec, source_root));
    const auto layout_bytes = rd::EncodeMaterialParameterLayout(graph.parameters);
    spec.inputs.push_back(SemanticInput("material/" + std::string(name) + "/parameter-layout",
        source_root / ("ContentSource/Materials/" + std::string(name) + ".json"), layout_bytes));
    for (const auto *relative : {"Schemas/material_graph.schema.json", "Content/Shaders/project_hs.hlsl",
        "Content/Shaders/visibility_buffer.hlsl", "Content/Shaders/environment.hlsli", "Content/Shaders/material_surface.hlsli",
        "Content/Shaders/hero_material.hlsli", "Content/Shaders/material_sidecar.hlsli", "Content/Shaders/material_parameters.hlsli",
        "Content/Shaders/probe_ibl.hlsli",
        "Content/Shaders/light_list.hlsli",
        "Content/Shaders/local_shadow.hlsli", "Content/Shaders/ray_shadow_data.hlsli",
        "Content/Shaders/ltc_area.hlsli",
        "Content/Shaders/deferred_surface.hlsli", "Content/Shaders/temporal_reconstruction.hlsli", "Content/Shaders/display_transform.hlsli",
        "Content/Shaders/bsdf_metallic_roughness.hlsli", "Content/Shaders/bsdf_layers.hlsli", "Content/Shaders/gpu_scene_tables.hlsli",
        "Content/Shaders/geometry.hlsli", "Content/Shaders/geometry_records.hlsli", "Content/Shaders/surface_layout.hlsli",
        "Content/Shaders/visibility_records.hlsli", "Content/Shaders/triangle_sample.hlsli", "Content/Shaders/vfx_ribbon.hlsli"})
        spec.inputs.push_back(ap::HashInput(relative, source_root / relative));
    AddSharedRendererShaderInputs(spec,source_root);
    spec.inputs.push_back(ap::HashInput("tool/dxc.exe", dxc));
    for (const auto *library : {"dxcompiler.dll", "dxil.dll"})
        spec.inputs.push_back(ap::HashInput(std::string("tool/") + library, dxc.parent_path() / library));
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);
    const auto result_path = build_root / "Actions" / namespace_path / "result.json";
    if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
    {
        try
        {
            const auto result = ap::Json::parse(ap::ReadBytes(result_path));
            if (result.at("action_id") == spec.action_id && result.at("action_key") == ap::ActionKey(spec) &&
                result.at("products").size() == outputs.size() + 1)
            {
                bool valid = true;
                for (const auto &program : outputs)
                {
                    ap::UpstreamProduct ignored;
                    fs::path path;
                    (void)ap::ReadActionResult(build_root, namespace_path, program.id, ignored, path);
                    valid = valid && ValidDxil(path);
                }
                ap::UpstreamProduct ignored;
                fs::path path;
                (void)ap::ReadActionResult(build_root, namespace_path, material_id, ignored, path);
                const auto contents = ap::ReadBytes(path);
                const auto material = rd::ReadHeroMaterialPackage(std::as_bytes(std::span(contents.data(), contents.size())), name);
                valid = valid && material.surface == rd::MaterialSurfaceLayout{reflection.capability_mask, reflection.varying_mask} &&
                    material.parameter_layout_hash == rd::MaterialParameterLayoutHash(graph.parameters);
                if (valid) { ap::TouchActionResult(result_path); std::cout << spec.action_id << " current\n"; return; }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current output. */ }
    }
    const auto stage = ap::CreateStage(build_root / "Actions" / namespace_path);
    try
    {
        const auto include_path = stage / "hero_materials.generated.hlsli";
        ap::WriteBytes(include_path, std::as_bytes(std::span(generated.data(), generated.size())));
        std::vector<ap::Product> products;
        std::vector<rd::MaterialProgramSource> programs;
        for (std::size_t i = 0; i < outputs.size(); ++i)
        {
            const auto &program = outputs[i];
            const auto output = stage / program.filename;
            RunDxc(dxc, source_root, stage, output, source_root / program.source_relative,
                program.entry, program.profile, L"-DHS_GENERATED_HERO_MATERIAL=1", sidecar_layout.plane_count);
            Require(ValidDxil(output), "DXC did not produce a DXIL container");
            products.push_back(ap::HashProduct(program.id, program.filename, output));
            programs.push_back({program.id, kinds[i]});
        }
        const auto package = rd::BuildHeroMaterialPackage(name, graph.parameters,
            {reflection.capability_mask, reflection.varying_mask}, programs);
        const auto decoded = rd::ReadHeroMaterialPackage(package, name);
        const auto output = stage / "material.rpkg";
        ap::WriteBytes(output, package);
        auto product = ap::HashProduct(material_id, "material.rpkg", output);
        product.dependencies = decoded.dependencies;
        products.push_back(std::move(product));
        fs::remove(include_path);
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (...) { ap::RemoveStage(stage); throw; }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}

void Produce(std::string_view name, const fs::path &source_root,
             const fs::path &build_root, const fs::path &dxc)
{
    Require(hs::material_content::IsAuthoredMaterialName(name),
            "unsupported material name");
    if (hs::material_content::IsHeroMaterialName(name))
    {
        ProduceHero(name, source_root, build_root, dxc);
        return;
    }
    Require(source_root.is_absolute() && build_root.is_absolute() && dxc.is_absolute(),
            "all paths must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    ap::RequireRegularFile(dxc);
    const auto material_relative = "ContentSource/Materials/" + std::string(name) + ".json";
    const auto material_path = source_root / material_relative;
    const auto material_text = ap::ReadBytes(material_path);
    const bool terrain = name == "terrain";
    const bool masked = !terrain && hs::material_content::IsMaskedEnvironmentMaterial(name);
    const auto definition = hs::material_content::ParseMaterialDefinition(name, material_text);
    const auto reflection = rd::AnalyzeClosure(definition.graph);
    (void)rd::EncodeMaterialSurfaceLayout({reflection.capability_mask, reflection.varying_mask});
    const auto ordinal = MaterialOrdinal(name);
    const auto generated = rd::EmitEnvironmentMaterialContextHlsl(definition.graph, ordinal);
    const auto namespace_path = "render/shader/" + std::string(name);
    constexpr std::string_view include_name = "environment_material.generated.hlsli";
    const std::string stem(name);
    std::vector<ProgramOutput> outputs;
    {
        outputs.push_back({namespace_path + "_scene.dxil", stem + "_scene.dxil",
                           "Content/Shaders/project_hs.hlsl",
                           masked ? L"EnvironmentMaskedPS" : L"EnvironmentOpaquePS", L"ps_6_0"});
        if (masked)
            outputs.push_back({namespace_path + "_shadow.dxil", stem + "_shadow.dxil",
                               "Content/Shaders/project_hs.hlsl",
                               L"EnvironmentMaskedShadowPS", L"ps_6_0"});
        outputs.push_back({namespace_path + "_visibility.dxil", stem + "_visibility.dxil",
                           "Content/Shaders/visibility_buffer.hlsl",
                           masked ? L"EnvironmentMaskedVisibilityPS"
                                  : L"EnvironmentOpaqueVisibilityPS",
                           L"ps_6_0"});
        outputs.push_back({namespace_path + "_resolve.dxil", stem + "_resolve.dxil",
                           "Content/Shaders/visibility_buffer.hlsl", L"SurfaceResolveCS",
                           L"cs_6_0"});
        outputs.push_back({namespace_path + "_probe_capture.dxil", stem + "_probe_capture.dxil",
                           "Content/Shaders/local_probe_capture.hlsl",
                           masked ? L"LocalProbeMaskedCapturePS" : L"LocalProbeOpaqueCapturePS", L"ps_6_0"});
        outputs.push_back({namespace_path + "_probe_shadow.dxil", stem + "_probe_shadow.dxil",
                           "Content/Shaders/local_probe_capture.hlsl", L"CaptureStaticShadowPS", L"ps_6_0"});
        if(terrain)
        {
            outputs.push_back({"render/shader/local_probe_capture_vs.dxil", "local_probe_capture_vs.dxil",
                               "Content/Shaders/local_probe_capture.hlsl", L"LocalProbeCaptureVS", L"vs_6_0"});
            outputs.push_back({"render/shader/local_probe_capture_sky_ps.dxil", "local_probe_capture_sky_ps.dxil",
                               "Content/Shaders/local_probe_capture.hlsl", L"LocalProbeSkyCapturePS", L"ps_6_0"});
        }
    }

    // Only programs that actually evaluate material texture samples get an
    // immutable feedback variant. Baseline IDs and shader-model requirements
    // remain unchanged for every adapter.
    const auto baseline_count = outputs.size();
    for (std::size_t index = 0; index < baseline_count; ++index)
    {
        const auto &baseline = outputs[index];
        const bool samples = baseline.id.ends_with("_scene.dxil") || baseline.id.ends_with("_resolve.dxil") ||
            baseline.id.ends_with("_probe_capture.dxil") || (masked &&
            (baseline.id.ends_with("_visibility.dxil") || baseline.id.ends_with("_shadow.dxil")));
        if (!samples) continue;
        auto variant = baseline;
        variant.id.insert(variant.id.size() - 5u, "_feedback");
        variant.filename.insert(variant.filename.size() - 5u, "_feedback");
        variant.profile = baseline.profile.starts_with(L"cs_") ? L"cs_6_5" : L"ps_6_5";
        variant.feedback = true;
        outputs.push_back(std::move(variant));
    }

    ap::ActionSpec spec;
    spec.action_id = "render.shader." + std::string(name);
    spec.recipe_id = "project-hs.render.material-shader";
    spec.recipe_version = 10;
    spec.namespace_path = namespace_path;
    spec.inputs.push_back(SemanticInput(
        "material/" + std::string(name) + "/shader-ir", material_path,
        std::as_bytes(std::span<const char>(generated.data(), generated.size()))));
    const auto sidecar_mask = AddSurfaceReflectionInputs(spec, source_root);
    const auto sidecar_layout = rd::ComputeMaterialSidecarLayout(sidecar_mask);
    for (const auto *relative : {"Schemas/material_graph.schema.json",
                                 "Content/Shaders/project_hs.hlsl",
                                 "Content/Shaders/local_probe_capture.hlsl",
                                  "Content/Shaders/environment.hlsli",
                                  "Content/Shaders/probe_ibl.hlsli",
                                  "Content/Shaders/shadow_data.hlsli",
                                  "Content/Shaders/light_list.hlsli",
                                  "Content/Shaders/local_shadow.hlsli",
                                  "Content/Shaders/ray_shadow_data.hlsli",
                                  "Content/Shaders/ltc_area.hlsli",
                                 "Content/Shaders/material_surface.hlsli",
                                 "Content/Shaders/hero_material.hlsli",
                                 "Content/Shaders/material_sidecar.hlsli",
                                 "Content/Shaders/deferred_surface.hlsli",
                                 "Content/Shaders/temporal_reconstruction.hlsli",
                                 "Content/Shaders/display_transform.hlsli",
                                 "Content/Shaders/bsdf_metallic_roughness.hlsli",
                                 "Content/Shaders/bsdf_layers.hlsli",
                                 "Content/Shaders/material_parameters.hlsli",
                                 "Content/Shaders/gpu_scene_tables.hlsli",
                                 "Content/Shaders/geometry.hlsli",
                                 "Content/Shaders/geometry_records.hlsli",
                                 "Content/Shaders/surface_layout.hlsli",
                                 "Content/Shaders/visibility_buffer.hlsl",
                                 "Content/Shaders/visibility_records.hlsli",
                                  "Content/Shaders/triangle_sample.hlsli",
                                  "Content/Shaders/vfx_ribbon.hlsli"})
        spec.inputs.push_back(ap::HashInput(relative, source_root / relative));
    if (terrain)
        spec.inputs.push_back(ap::HashInput("Content/Shaders/material_terrain.hlsli",
                                            source_root / "Content/Shaders/material_terrain.hlsli"));
    AddSharedRendererShaderInputs(spec,source_root);
    spec.inputs.push_back(ap::HashInput("tool/dxc.exe", dxc));
    for (const auto *library : {"dxcompiler.dll", "dxil.dll"})
        spec.inputs.push_back(ap::HashInput(std::string("tool/") + library,
                                          dxc.parent_path() / library));
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);

    const auto result_path = build_root / "Actions" / namespace_path / "result.json";
    if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
    {
        try
        {
            const auto result = ap::Json::parse(ap::ReadBytes(result_path));
            if (result.at("action_id") == spec.action_id &&
                result.at("action_key") == ap::ActionKey(spec) &&
                result.at("products").size() == outputs.size())
            {
                bool valid = true;
                for (const auto &output : outputs)
                {
                    ap::UpstreamProduct ignored;
                    fs::path product_path;
                    (void)ap::ReadActionResult(build_root, namespace_path,
                                               output.id, ignored, product_path);
                    valid &= ValidDxil(product_path);
                }
                if (valid)
                {
                    ap::TouchActionResult(result_path);
                    std::cout << spec.action_id << " current\n";
                    return;
                }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current result. */ }
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / namespace_path);
    try
    {
        const auto include_path = stage / std::string(include_name);
        ap::WriteBytes(include_path, std::as_bytes(std::span(generated.data(), generated.size())));
        std::vector<ap::Product> products;
        products.reserve(outputs.size());
        for (const auto &program : outputs)
        {
            const auto output = stage / std::string(program.filename);
            RunDxc(dxc, source_root, stage, output,
                   source_root / program.source_relative, program.entry, program.profile,
                   L"-DHS_GENERATED_ENVIRONMENT_MATERIAL=1", sidecar_layout.plane_count, false, false,
                   program.feedback);
            Require(ValidDxil(output), "DXC did not produce a DXIL container");
            products.push_back(ap::HashProduct(std::string(program.id),
                                               std::string(program.filename), output));
        }
        fs::remove(include_path);
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
    ap::RemoveStage(stage);
    std::cout << spec.action_id << " published\n";
}
} // namespace

int wmain(int argc, wchar_t **argv)
{
    fs::path source_root, build_root, dxc;
    std::string name;
    ap::Json action_id = nullptr;
    bool preintegrate = false;
    bool parameters = false;
    bool with_nrd = false;
    bool with_brixelizer = false;
    bool decal_surface = false;
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::wstring_view arg = argv[i];
            if (arg == L"--source-root" && source_root.empty() && ++i < argc)
                source_root = argv[i];
            else if (arg == L"--build-root" && build_root.empty() && ++i < argc)
                build_root = argv[i];
            else if (arg == L"--dxc" && dxc.empty() && ++i < argc)
                dxc = argv[i];
            else if (arg == L"--bsdf-preintegrate" && !preintegrate)
                preintegrate = true;
            else if (arg == L"--decal-surface" && !decal_surface)
                decal_surface = true;
            else if (arg == L"--parameters" && !parameters)
                parameters = true;
            else if (arg == L"--with-nrd" && !with_nrd)
                with_nrd = true;
            else if (arg == L"--with-brixelizer" && !with_brixelizer)
                with_brixelizer = true;
            else if (arg == L"--name" && name.empty() && ++i < argc)
            {
                const std::wstring_view value = argv[i];
                Require(std::ranges::all_of(value, [](wchar_t character) {
                    return (character >= L'a' && character <= L'z') ||
                           (character >= L'0' && character <= L'9') ||
                           character == L'_';
                }), "invalid material name");
                name.assign(value.begin(), value.end());
            }
            else
                throw std::runtime_error("hs_render_material_shader: invalid command line");
        }
        const bool valid_mode =
            (parameters && !preintegrate && !decal_surface && !name.empty() && dxc.empty()) ||
            (preintegrate && !parameters && !decal_surface && name.empty() && !dxc.empty()) ||
            (decal_surface && !parameters && !preintegrate && name.empty() && !dxc.empty()) ||
            (!preintegrate && !decal_surface && !parameters && !name.empty() && !dxc.empty());
        Require(valid_mode && (!with_nrd || preintegrate) && (!with_brixelizer || preintegrate) &&
                !source_root.empty() && !build_root.empty(),
                "usage: hs_render_material_shader (--name <archetype> --dxc <absolute> | --parameters --name <archetype> | --bsdf-preintegrate | --decal-surface --dxc <absolute>) --source-root <absolute> --build-root <absolute>");
        if (parameters)
        {
            if (hs::material_content::IsAuthoredMaterialName(name) ||
                hs::material_content::IsDecalMaterialName(name))
                action_id = "render.parameters." + name;
            ProduceParameters(name, source_root.lexically_normal(),
                              build_root.lexically_normal());
        }
        else if (decal_surface)
        {
            action_id = "render.shader.decal-surface";
            ProduceDecalSurface(source_root.lexically_normal(), build_root.lexically_normal(), dxc.lexically_normal());
        }
        else if (preintegrate)
        {
            action_id = "render.shader.bsdf-preintegrate";
            ProducePreintegration(source_root.lexically_normal(), build_root.lexically_normal(),
                                  dxc.lexically_normal(), with_nrd, with_brixelizer);
        }
        else
        {
            if (hs::material_content::IsAuthoredMaterialName(name))
                action_id = "render.shader." + name;
            Produce(name, source_root.lexically_normal(), build_root.lexically_normal(),
                    dxc.lexically_normal());
        }
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_material_shader", error.what(),
                          {{"action_id", action_id}, {"name", name},
                           {"bsdf_preintegrate", preintegrate},
                           {"parameters", parameters}, {"decal_surface", decal_surface},
                           {"with_nrd", with_nrd}, {"with_brixelizer", with_brixelizer},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)},
                           {"dependency", {{"dxc", ap::DiagnosticPath(dxc)}}}});
        return 1;
    }
}
