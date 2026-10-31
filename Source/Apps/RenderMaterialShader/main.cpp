#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../AssetPipeline/action.hpp"
#include "../AssetPipeline/dxc_process.hpp"
#include "../AssetPipeline/render_shader_binding_inputs.hpp"
#include "../AssetPipeline/shader_source_snapshot.hpp"
#include "coated_dfg_cooker.hpp"
#include "material_graph_parser.hpp"
#include <hs/core/cooked_format.hpp>

#include <hs/core/windows_command_line.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/material_sidecar.hpp>
#include <hs/render_data/surface_layout.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <map>
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

bool ValidDxilBytes(std::string_view bytes)
{
    return bytes.size() >= 32 && bytes.starts_with("DXBC");
}

bool ValidDxil(const fs::path &path)
{
    return fs::exists(path) && ValidDxilBytes(ap::ReadBytes(path));
}

ap::Input SemanticInput(std::string logical_id, const fs::path &dependency_path,
                        std::span<const std::byte> bytes)
{
    return ap::Input{std::move(logical_id), dependency_path, ap::Sha256Bytes(bytes)};
}

void AddSharedRendererShaderInputs(ap::ActionSpec &spec,const fs::path &source_root)
{
    hs::shader_content::AddRenderBindingInputs(spec.inputs, source_root);
    // These common includes affect named material, hero and deferred shaders.
    for(const auto *relative:{"Content/Shaders/surface_metadata.hlsli", "Content/Shaders/render_binding_contract.hlsli", "Content/Shaders/local_probe_sampling.hlsli",
        "Content/Shaders/shadow_data.hlsli","Content/Shaders/local_shadow.hlsli",
        "Content/Shaders/volumetric_sampling.hlsli",
        "Content/Shaders/scene_hlod_sampling.hlsli", "Content/Shaders/texture_streaming.hlsli", "Content/Shaders/entity_surface.hlsli",
        "Content/Shaders/shadow_filter.hlsli", "Content/Shaders/environment_wind.hlsli",
        "Content/Shaders/meadow_surface.hlsli", "Content/Shaders/grass_deformation.hlsli",
        "Content/Shaders/particle_collision.hlsli"})
        if(std::ranges::none_of(spec.inputs,[&](const auto &input) { return input.logical_id==relative; }))
            spec.inputs.push_back(ap::HashInput(relative,source_root/relative));
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
            std::uint32_t sidecar_planes = 0, bool generated_deferred = false,
            bool feedback = false, bool hero_work = false,
            bool hero_translucent_domain = false)
{
    auto diagnostics = output;
    diagnostics += ".diagnostics.txt";
    std::vector<std::wstring> args{
        L"-nologo", L"-WX", L"-O3", L"-Ges",
        L"-E", std::wstring(entry), L"-T", std::wstring(profile),
        L"-I", L"..",
        L"-I", L"Content/Shaders",
        L"-Fo", output.wstring(),
        L"-Fe", diagnostics.wstring(),
        source.wstring()};
    hs::shader_content::AppendRenderBindingArguments(args);
    if (!define.empty()) args.emplace_back(define);
    if (generated_deferred) args.emplace_back(L"-DHS_GENERATED_DEFERRED_MATERIALS=1");
    if (feedback) args.emplace_back(L"-DHS_SAMPLER_FEEDBACK=1");
    if (hero_work) args.emplace_back(L"-DHS_CAPTURE_HERO_WORK=1");
    if (hero_translucent_domain) args.emplace_back(L"-DHS_HERO_TRANSLUCENT_DOMAIN_ONLY=1");
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
                        CREATE_NO_WINDOW, nullptr, source_root.c_str(), &startup, &process))
        throw std::runtime_error("hs_render_material_shader: DXC launch failed: " +
                                 std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    const auto code = hs::shader_content::WaitForDxcExit(process, stage, diagnostics);
    if (code != 0)
        throw std::runtime_error("hs_render_material_shader: DXC compilation failed for " +
            output.filename().string() + ": " +
            hs::shader_content::ReadDxcDiagnostics(diagnostics));
    fs::remove(diagnostics);
}

bool IsHeroTranslucentProgram(const ProgramOutput &program)
{
    constexpr std::array<std::wstring_view, 6> entries{
        L"HeroPeelFrontPS", L"HeroPeelExitPS", L"HeroOitTailPS",
        L"HeroTransparentGuidePS", L"HeroLayerCompositePS", L"HeroTranslucentPS"};
    return std::ranges::find(entries, program.entry) != entries.end();
}

void ProducePreintegration(const fs::path &source_root, const fs::path &build_root,
                           const fs::path &dxc, bool with_brixelizer, bool hero_work = false)
{
    Require(source_root.is_absolute() && build_root.is_absolute() && dxc.is_absolute(),
            "all paths must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    ap::RequireRegularFile(dxc);
    ap::ActionSpec spec;
    spec.action_id = "render.shader.bsdf-preintegrate";
    spec.recipe_id = "project-hs.render.bsdf-preintegrate";
    spec.recipe_version = 12;
    spec.namespace_path = "render/shader/bsdf-preintegrate";
    if (hero_work)
    {
        spec.action_id = "render.shader.hero-work";
        spec.recipe_id = "project-hs.render.hero-work-ps";
        spec.recipe_version = 1;
        spec.namespace_path = "render/shader/hero-work";
    }
    else
    {
        spec.inputs.push_back(ap::HashInput("Source/Apps/RenderMaterialShader/coated_dfg_cooker.cpp",
            source_root / "Source/Apps/RenderMaterialShader/coated_dfg_cooker.cpp"));
        spec.inputs.push_back(ap::HashInput("Source/Apps/RenderMaterialShader/coated_dfg_cooker.hpp",
            source_root / "Source/Apps/RenderMaterialShader/coated_dfg_cooker.hpp"));
    }
    if (with_brixelizer)
        for (const auto *relative : {"Content/Shaders/brixelizer_common.hlsli", "Content/Shaders/brixelizer_guides.hlsl",
                                   "Content/Shaders/brixelizer_gi_resolve.hlsl",
                                   "Content/Shaders/brixelizer_gi_compose.hlsl",
                                   "Content/Shaders/brixelizer_cache_export.hlsl"})
            spec.inputs.push_back(ap::HashInput(relative, source_root / relative));
    PreparedMaterialInputs materials(source_root);
    const auto generated = DeferredContextInclude(spec, source_root, materials);
    const auto sidecar_layout = rd::ComputeMaterialSidecarLayout(AddSurfaceReflectionInputs(spec, source_root, materials));
    const auto hero_context = HeroContextInclude(spec, source_root, materials);
    spec.inputs.push_back(ap::HashInput("Schemas/material_graph.schema.json",
                                      source_root / "Schemas/material_graph.schema.json"));
    for (const auto *relative : {"Content/Shaders/bsdf_preintegrate.hlsl",
                                 "Content/Shaders/bsdf_metallic_roughness.hlsli",
                                 "Content/Shaders/bsdf_layers.hlsli",
                                 "Content/Shaders/material_surface.hlsli",
                                 "Content/Shaders/material_terrain.hlsli",
                                 "Content/Shaders/hero_material.hlsli",
                                 "Content/Shaders/visibility_buffer.hlsl",
                                 "Content/Shaders/visibility_records.hlsli",
                                 "Content/Shaders/visibility_classification.hlsli",
                                 "Content/Shaders/triangle_sample.hlsli",
                                 "Content/Shaders/material_sidecar.hlsli",
                                 "Content/Shaders/material_parameters.hlsli",
                                 "Content/Shaders/deferred_surface.hlsli",
                                 "Content/Shaders/subsurface_diffusion.hlsl",
                                 "Content/Shaders/screen_reflections.hlsl",
                                 "Content/Shaders/vfx_screen_overlay.hlsl",
                                 "Content/Shaders/adaptive_shading.hlsl",
                                 "Content/Shaders/project_hs.hlsl",
                                "Content/Shaders/environment.hlsli",
                                "Content/Shaders/probe_ibl.hlsli",
                                "Content/Shaders/light_list.hlsli",
                                "Content/Shaders/ltc_area.hlsli",
                                 "Content/Shaders/shadow_data.hlsli",
                                 "Content/Shaders/local_shadow.hlsli",

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
        ProgramOutput{"render/shader/hero_transparency_vs.dxil", "hero_transparency_vs.dxil",
                      "Content/Shaders/project_hs.hlsl", L"SceneVS", L"vs_6_0"},
        ProgramOutput{"render/shader/hero_transparency_current_vs.dxil", "hero_transparency_current_vs.dxil",
                      "Content/Shaders/project_hs.hlsl", L"HeroCurrentVS", L"vs_6_0"},
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
    if (hero_work)
    {
        std::erase_if(outputs, [](const auto &program) {
            return !program.id.starts_with("render/shader/hero_") || !program.profile.starts_with(L"ps_");
        });
        Require(outputs.size() == 5, "hero work recipe must contain exactly the five existing Hero PS entries");
        for (auto &program : outputs)
        {
            program.id.insert(program.id.size() - 5, "_hero_work");
            program.filename.insert(program.filename.size() - 5, "_hero_work");
        }
    }
    if (!hero_work)
    {
        for (const auto &program : std::array{
            ProgramOutput{"render/shader/hero_scene_ps.dxil", "hero_scene_ps.dxil", "Content/Shaders/project_hs.hlsl", L"HeroScenePS", L"ps_6_0"},
            ProgramOutput{"render/shader/hero_visibility_ps.dxil", "hero_visibility_ps.dxil", "Content/Shaders/visibility_buffer.hlsl", L"HeroVisibilityPS", L"ps_6_0"},
            ProgramOutput{"render/shader/hero_resolve_cs.dxil", "hero_resolve_cs.dxil", "Content/Shaders/visibility_buffer.hlsl", L"HeroSurfaceResolveCS", L"cs_6_0"},
            ProgramOutput{"render/shader/hero_shadow_ps.dxil", "hero_shadow_ps.dxil", "Content/Shaders/project_hs.hlsl", L"HeroShadowPS", L"ps_6_0"},
            ProgramOutput{"render/shader/hero_translucent_ps.dxil", "hero_translucent_ps.dxil", "Content/Shaders/project_hs.hlsl", L"HeroTranslucentPS", L"ps_6_0"}})
            outputs.push_back(program);
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
            const ap::ActionResultSnapshot result(build_root, spec.namespace_path);
            ap::UpstreamProduct ignored;
            if (result.ActionKeyValue() == ap::ActionKey(spec) &&
                result.ActionId() == spec.action_id &&
                result.ProductCount() == outputs.size() + 2u * static_cast<std::size_t>(!hero_work))
            {
                bool valid = true;
                for (const auto &output : outputs)
                {
                    valid &= ValidDxilBytes(result.ReadProductBytes(output.id, ignored));
                }
                if (!hero_work)
                {
                    valid &= result.ReadProductBytes("render/lighting/coated_ggx_dfg.dds", ignored).size() ==
                        148u + 256u * 256u * 8u;
                    const auto abi = result.ReadProductBytes(rd::SurfaceLayout::abi_product, ignored);
                    valid &= abi.size() == 4 && static_cast<unsigned char>(abi[0]) == rd::SurfaceLayout::version &&
                        abi[1] == 0 && abi[2] == 0 && abi[3] == 0;
                }
                if (valid)
                {
                    ap::TouchActionResult(result_path);
                    return;
                }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current result. */ }
    }
    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto source_snapshot = hs::shader_content::CreateShaderSourceSnapshot(spec.inputs, stage);
        const auto include_path = stage / "deferred_materials.generated.hlsli";
        ap::WriteBytes(include_path, std::as_bytes(std::span(generated.data(), generated.size())));
        const auto hero_include_path = stage / "hero_materials.generated.hlsli";
        ap::WriteBytes(hero_include_path, std::as_bytes(std::span(hero_context.data(), hero_context.size())));
        std::vector<ap::Product> products;
        for (const auto &program : outputs)
        {
            const auto output = stage / program.filename;
            const bool brixelizer_guides = program.id.starts_with("render/shader/brixelizer_") && program.profile.starts_with(L"cs_");
            const auto define = (program.id.starts_with("render/shader/hero_") || brixelizer_guides)
                ? std::wstring_view{L"-DHS_GENERATED_HERO_MATERIAL=1"}
                : program.entry != L"main" ? std::wstring_view{L"-DHS_GENERATED_DEFERRED_MATERIALS=1"} : std::wstring_view{};
            RunDxc(dxc, source_snapshot, stage, output, program.source_relative,
                   program.entry, program.profile, define, sidecar_layout.plane_count, brixelizer_guides,
                   false, hero_work, IsHeroTranslucentProgram(program));
            Require(ValidDxil(output), "DXC did not produce a DXIL container");
            products.push_back(ap::HashProduct(program.id, program.filename, output));
        }
        if (!hero_work)
        {
            const auto dfg = hs::material_content::CookCoatedDfgDds();
            const auto output = stage / "coated_ggx_dfg.dds";
            ap::WriteBytes(output, std::span<const std::byte>(dfg.data(), dfg.size()));
            products.push_back(ap::HashProduct("render/lighting/coated_ggx_dfg.dds",
                output.filename().string(), output));
            const std::array<std::byte, 4> abi{
                std::byte(rd::SurfaceLayout::version & 0xffu), std::byte((rd::SurfaceLayout::version >> 8u) & 0xffu),
                std::byte((rd::SurfaceLayout::version >> 16u) & 0xffu), std::byte((rd::SurfaceLayout::version >> 24u) & 0xffu)};
            const auto abi_path = stage / "surface_layout.abi";
            ap::WriteBytes(abi_path, abi);
            products.push_back(ap::HashProduct(std::string(rd::SurfaceLayout::abi_product),
                abi_path.filename().string(), abi_path));
        }
        fs::remove(include_path);
        fs::remove(hero_include_path);
        ap::RemoveStage(source_snapshot);
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (const hs::shader_content::DxcStageInUseError &) { throw; }
    catch (...)
    {
        ap::RemoveStageAfterFailure(stage);
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
    const auto material_text = ap::ReadBytes(material_path);
    const auto prepared = decal ? hs::material_content::ParsePreparedDecalMaterial(name, material_text) :
        hs::material_content::ParseMaterialDefinition(name, material_text).prepared;
    const auto &graph = prepared.Graph();
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
    spec.inputs.push_back({material_relative, material_path,
        ap::Sha256Bytes(std::as_bytes(std::span(material_text.data(), material_text.size())))});
    spec.inputs.push_back(ap::HashInput(schema_relative, schema_path));
    if (decal) spec.inputs.push_back(ap::HashInput("Schemas/material_graph.schema.json", source_root / "Schemas/material_graph.schema.json"));
    std::ranges::sort(spec.inputs, {}, &ap::Input::logical_id);

    const auto result_path = build_root / "Actions" / spec.namespace_path / "result.json";
    if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
    {
        try
        {
            const ap::ActionResultSnapshot result(build_root, spec.namespace_path);
            if (result.ActionId() == spec.action_id &&
                result.ActionKeyValue() == ap::ActionKey(spec) &&
                result.ProductCount() == 1 &&
                result.HasProduct(product_id))
            {
                ap::UpstreamProduct ignored;
                const auto package = result.ReadProductBytes(product_id, ignored);
                const auto decoded = rd::ReadMaterialParameterPackage(
                    std::as_bytes(std::span(package.data(), package.size())),
                    hs::MakeAssetId(product_id));
                rd::ValidateMaterialParameters(decoded);
                if (rd::MaterialParameterLayoutHash(decoded) ==
                        rd::MaterialParameterLayoutHash(parameters) &&
                    std::ranges::equal(decoded, parameters))
                {
                    ap::TouchActionResult(result_path);
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
                    std::ranges::equal(decoded, parameters),
                "material parameter package layout mismatch");
        std::vector<ap::Product> products;
        products.push_back(ap::HashProduct(product_id, output.filename().string(), output));
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

void ProduceDecalSurface(const fs::path &source_root, const fs::path &build_root, const fs::path &dxc)
{
    Require(source_root.is_absolute() && build_root.is_absolute() && dxc.is_absolute(), "all paths must be absolute");
    ap::RequireDirectory(source_root); ap::MakeDirectory(build_root); ap::RequireRegularFile(dxc);
    ap::ActionSpec spec;
    spec.action_id = "render.shader.decal-surface";
    spec.recipe_id = "project-hs.render.decal-surface";
    spec.recipe_version = 1;
    spec.namespace_path = "render/shader/decal-surface";
    hs::shader_content::AddRenderBindingInputs(spec.inputs, source_root);
    std::vector<rd::MaterialGraph> graphs;
    std::string generated;
    for (std::size_t ordinal = 0; ordinal < rd::kDecalMaterialNames.size(); ++ordinal)
    {
        const std::string name(rd::kDecalMaterialNames[ordinal]);
        const auto path = source_root / ("ContentSource/Materials/" + name + ".json");
        const auto prepared = hs::material_content::ParsePreparedDecalMaterial(name, ap::ReadBytes(path));
        const auto &graph = prepared.Graph();
        const std::array layer_counts{ordinal == 0 ? 4u : 8u};
        rd::ValidateMaterialSampleLayers(prepared, layer_counts, layer_counts.front() - 1u);
        auto function = rd::EmitDecalMaterialContextHlsl(prepared, "HsDecalEvaluate_" + std::to_string(ordinal),
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
        "Content/Shaders/material_surface.hlsli", "Content/Shaders/material_parameters.hlsli", "Content/Shaders/surface_metadata.hlsli",
        "Content/Shaders/render_binding_contract.hlsli"})
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
            const ap::ActionResultSnapshot result(build_root, spec.namespace_path);
            if (result.ActionId() == spec.action_id && result.ActionKeyValue() == ap::ActionKey(spec) &&
                result.ProductCount() == graphs.size() + 1)
            {
                ap::UpstreamProduct ignored;
                bool valid = ValidDxilBytes(result.ReadProductBytes(shader_id, ignored));
                for (std::size_t ordinal = 0; ordinal < graphs.size(); ++ordinal)
                {
                    const std::string name(rd::kDecalMaterialNames[ordinal]);
                    const auto bytes = result.ReadProductBytes("render/material/" + name + "/material.rpkg", ignored);
                    const auto material = rd::ReadDecalMaterialPackage(std::as_bytes(std::span(bytes.data(), bytes.size())), name);
                    valid = valid && material.receivers == graphs[ordinal].decal_receivers &&
                        material.parameter_layout_hash == rd::MaterialParameterLayoutHash(graphs[ordinal].parameters);
                }
                if (valid) { ap::TouchActionResult(result_path); return; }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current output. */ }
    }
    const auto stage = ap::CreateStage(build_root / "Actions" / spec.namespace_path);
    try
    {
        const auto source_snapshot = hs::shader_content::CreateShaderSourceSnapshot(spec.inputs, stage);
        const auto include = stage / "decal_materials.generated.hlsli";
        ap::WriteBytes(include, std::as_bytes(std::span(generated.data(), generated.size())));
        const auto shader = stage / "decal_surface_cs.dxil";
        RunDxc(dxc, source_snapshot, stage, shader, "Content/Shaders/decal_surface.hlsl",
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
        fs::remove(include);
        ap::RemoveStage(source_snapshot);
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (const hs::shader_content::DxcStageInUseError &) { throw; }
    catch (...) { ap::RemoveStageAfterFailure(stage); throw; }
    ap::RemoveStage(stage); std::cout << spec.action_id << " published\n";
}

void ProduceHero(std::string_view name, const fs::path &source_root,
                 const fs::path &build_root)
{
    Require(source_root.is_absolute() && build_root.is_absolute(), "all paths must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    PreparedMaterialInputs materials(source_root);
    const auto &prepared = materials.Get(name).prepared;
    const auto &graph = prepared.Graph();
    const auto reflection = rd::AnalyzeClosure(prepared);
    const auto namespace_path = "render/shader/" + std::string(name);
    const auto material_id = "render/material/" + std::string(name) + "/material.rpkg";
    const bool translucent = graph.kind == rd::MaterialGraphKind::Translucent ||
        graph.kind == rd::MaterialGraphKind::HeroRefractive;
    ap::ActionSpec spec;
    spec.action_id = "render.shader." + std::string(name);
    spec.recipe_id = "project-hs.render.hero-material-interface";
    spec.recipe_version = 1;
    spec.namespace_path = namespace_path;
    const ap::ActionResultSnapshot shaders(build_root, "render/shader/bsdf-preintegrate");
    std::vector<rd::MaterialProgramSource> programs;
    for (const auto &[id, kind] : std::array{
        std::pair{"render/shader/hero_scene_ps.dxil", rd::MaterialProgramKind::HeroScenePS},
        std::pair{"render/shader/hero_visibility_ps.dxil", rd::MaterialProgramKind::HeroVisibilityPS},
        std::pair{"render/shader/hero_resolve_cs.dxil", rd::MaterialProgramKind::HeroSurfaceResolveCS},
        std::pair{"render/shader/hero_shadow_ps.dxil", rd::MaterialProgramKind::HeroShadowPS},
        std::pair{"render/shader/hero_translucent_ps.dxil", rd::MaterialProgramKind::HeroTranslucentPS}})
    {
        if ((kind == rd::MaterialProgramKind::HeroTranslucentPS) != translucent) continue;
        ap::UpstreamProduct upstream;
        Require(ValidDxilBytes(shaders.ReadProductBytes(id, upstream)), "Shared Hero program is not DXIL");
        spec.upstream.push_back(std::move(upstream));
        programs.push_back({id, kind});
    }
    const auto package = rd::BuildHeroMaterialPackage(name, graph.parameters,
        {reflection.capability_mask, reflection.varying_mask}, programs);
    const auto decoded = rd::ReadHeroMaterialPackage(package, name);
    spec.inputs.push_back(SemanticInput("material/" + std::string(name) + "/package-interface",
        source_root / ("ContentSource/Materials/" + std::string(name) + ".json"), package));
    const auto result_path = build_root / "Actions" / namespace_path / "result.json";
    if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
    {
        try
        {
            const ap::ActionResultSnapshot result(build_root, namespace_path);
            if (result.ActionId() == spec.action_id && result.ActionKeyValue() == ap::ActionKey(spec) &&
                result.ProductCount() == 1)
            {
                ap::UpstreamProduct ignored;
                const auto bytes = result.ReadProductBytes(material_id, ignored);
                if (std::ranges::equal(std::as_bytes(std::span(bytes.data(), bytes.size())), package))
                { ap::TouchActionResult(result_path); return; }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current output. */ }
    }
    const auto stage = ap::CreateStage(build_root / "Actions" / namespace_path);
    try
    {
        const auto output = stage / "material.rpkg";
        ap::WriteBytes(output, package);
        auto product = ap::HashProduct(material_id, "material.rpkg", output);
        product.dependencies = decoded.dependencies;
        ap::Publish(build_root, spec, stage, {std::move(product)});
    }
    catch (...) { ap::RemoveStageAfterFailure(stage); throw; }
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
        ProduceHero(name, source_root, build_root);
        return;
    }
    Require(source_root.is_absolute() && build_root.is_absolute() && dxc.is_absolute(),
            "all paths must be absolute");
    ap::RequireDirectory(source_root);
    ap::MakeDirectory(build_root);
    ap::RequireRegularFile(dxc);
    const auto material_relative = "ContentSource/Materials/" + std::string(name) + ".json";
    const auto material_path = source_root / material_relative;
    const bool terrain = name == "terrain";
    const bool masked = !terrain && hs::material_content::IsMaskedEnvironmentMaterial(name);
    PreparedMaterialInputs materials(source_root);
    const auto &definition = materials.Get(name);
    const auto reflection = rd::AnalyzeClosure(definition.prepared);
    (void)rd::EncodeMaterialSurfaceLayout({reflection.capability_mask, reflection.varying_mask});
    const auto ordinal = MaterialOrdinal(name);
    const auto generated = rd::EmitEnvironmentMaterialContextHlsl(definition.prepared, ordinal);
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
            outputs.push_back({"render/shader/local_probe_capture_source_base_vs.dxil", "local_probe_capture_source_base_vs.dxil",
                               "Content/Shaders/local_probe_capture.hlsl", L"LocalProbeCaptureSourceBaseVS", L"vs_6_0"});
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
    const auto sidecar_mask = AddSurfaceReflectionInputs(spec, source_root, materials);
    const auto sidecar_layout = rd::ComputeMaterialSidecarLayout(sidecar_mask);
    for (const auto *relative : {"Schemas/material_graph.schema.json",
                                 "Content/Shaders/project_hs.hlsl",
                                 "Content/Shaders/local_probe_capture.hlsl",
                                 "Content/Shaders/local_probe_draw.hlsli",
                                  "Content/Shaders/environment.hlsli",
                                  "Content/Shaders/probe_ibl.hlsli",
                                  "Content/Shaders/shadow_data.hlsli",
                                  "Content/Shaders/light_list.hlsli",
                                  "Content/Shaders/local_shadow.hlsli",

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
                                 "Content/Shaders/visibility_classification.hlsli",
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
            const ap::ActionResultSnapshot result(build_root, spec.namespace_path);
            if (result.ActionId() == spec.action_id &&
                result.ActionKeyValue() == ap::ActionKey(spec) &&
                result.ProductCount() == outputs.size())
            {
                bool valid = true;
                for (const auto &output : outputs)
                {
                    ap::UpstreamProduct ignored;
                    valid &= ValidDxilBytes(result.ReadProductBytes(output.id, ignored));
                }
                if (valid)
                {
                    ap::TouchActionResult(result_path);
                    return;
                }
            }
        }
        catch (const std::exception &) { /* Rebuild invalid current result. */ }
    }

    const auto stage = ap::CreateStage(build_root / "Actions" / namespace_path);
    try
    {
        const auto source_snapshot = hs::shader_content::CreateShaderSourceSnapshot(spec.inputs, stage);
        const auto include_path = stage / std::string(include_name);
        ap::WriteBytes(include_path, std::as_bytes(std::span(generated.data(), generated.size())));
        std::vector<ap::Product> products;
        products.reserve(outputs.size());
        for (const auto &program : outputs)
        {
            const auto output = stage / std::string(program.filename);
            RunDxc(dxc, source_snapshot, stage, output,
                   program.source_relative, program.entry, program.profile,
                   L"-DHS_GENERATED_ENVIRONMENT_MATERIAL=1", sidecar_layout.plane_count, false,
                   program.feedback);
            Require(ValidDxil(output), "DXC did not produce a DXIL container");
            products.push_back(ap::HashProduct(std::string(program.id),
                                               std::string(program.filename), output));
        }
        fs::remove(include_path);
        ap::RemoveStage(source_snapshot);
        ap::Publish(build_root, spec, stage, std::move(products));
    }
    catch (const hs::shader_content::DxcStageInUseError &) { throw; }
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
    fs::path source_root, build_root, dxc;
    std::string name;
    ap::Json action_id = nullptr;
    bool preintegrate = false;
    bool hero_work = false;
    bool parameters = false;
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
            else if (arg == L"--hero-work" && !hero_work)
                hero_work = true;
            else if (arg == L"--decal-surface" && !decal_surface)
                decal_surface = true;
            else if (arg == L"--parameters" && !parameters)
                parameters = true;
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
            (!hero_work && parameters && !preintegrate && !decal_surface && !name.empty() && dxc.empty()) ||
            (!hero_work && preintegrate && !parameters && !decal_surface && name.empty() && !dxc.empty()) ||
            (!hero_work && decal_surface && !parameters && !preintegrate && name.empty() && !dxc.empty()) ||
            (!hero_work && !preintegrate && !decal_surface && !parameters && !name.empty() && !dxc.empty()) ||
            (hero_work && !parameters && !preintegrate && !decal_surface && name.empty() && !dxc.empty());
        Require(valid_mode && (!with_brixelizer || preintegrate) &&
                !source_root.empty() && !build_root.empty(),
                "usage: hs_render_material_shader (--name <archetype> --dxc <absolute> | --parameters --name <archetype> | --bsdf-preintegrate | --hero-work | --decal-surface --dxc <absolute>) --source-root <absolute> --build-root <absolute>");
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
        else if (hero_work)
        {
            action_id = "render.shader.hero-work";
            ProducePreintegration(source_root.lexically_normal(), build_root.lexically_normal(),
                                  dxc.lexically_normal(), false, true);
        }
        else if (preintegrate)
        {
            action_id = "render.shader.bsdf-preintegrate";
            ProducePreintegration(source_root.lexically_normal(), build_root.lexically_normal(),
                                  dxc.lexically_normal(), with_brixelizer);
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
                           {"bsdf_preintegrate", preintegrate}, {"hero_work", hero_work},
                           {"parameters", parameters}, {"decal_surface", decal_surface},
                           {"with_brixelizer", with_brixelizer},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)},
                           {"dependency", {{"dxc", ap::DiagnosticPath(dxc)}}}});
        return 1;
    }
}
