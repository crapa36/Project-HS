#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../AssetPipeline/action.hpp"
#include "../AssetPipeline/render_shader_binding_inputs.hpp"

#include <hs/core/windows_command_line.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace ap = hs::asset_pipeline;
namespace fs = std::filesystem;

struct ProgramOutput
{
    const char *filename;
    const wchar_t *entry;
    const wchar_t *profile;
};

struct ShaderFamily
{
    std::string_view name;
    std::string_view source_relative;
    std::span<const ProgramOutput> programs;
};

void Require(bool condition, std::string_view reason)
{
    if (!condition)
        throw std::runtime_error("hs_render_runtime_shader: " + std::string(reason));
}

bool ValidDxil(const fs::path &path)
{
    if (!fs::exists(path)) return false;
    const auto bytes = ap::ReadBytes(path);
    return bytes.size() >= 32 && bytes.substr(0, 4) == "DXBC";
}

constexpr std::array kProjectPrograms{
    ProgramOutput{"scene_vs.dxil", L"SceneVS", L"vs_6_0"},
    ProgramOutput{"scene_ps.dxil", L"ScenePS", L"ps_6_0"},
    ProgramOutput{"slime_ps.dxil", L"SlimePS", L"ps_6_6"},
    ProgramOutput{"shadow_vs.dxil", L"ShadowVS", L"vs_6_0"},
    ProgramOutput{"shadow_ps.dxil", L"ShadowPS", L"ps_6_0"},
    ProgramOutput{"environment_opaque_shadow_ps.dxil", L"EnvironmentOpaqueShadowPS", L"ps_6_0"},
    ProgramOutput{"particle_vs.dxil", L"ParticleVS", L"vs_6_0"},
    ProgramOutput{"ground_ring_vs.dxil", L"GroundRingVS", L"vs_6_0"},
    ProgramOutput{"ground_add_ps.dxil", L"GroundAddPS", L"ps_6_0"},
    ProgramOutput{"owner_mesh_vs.dxil", L"OwnerMeshVS", L"vs_6_0"},
    ProgramOutput{"owner_mesh_ps.dxil", L"OwnerMeshPS", L"ps_6_0"},
    ProgramOutput{"fresnel_shell_vs.dxil", L"FresnelShellVS", L"vs_6_0"},
    ProgramOutput{"fresnel_shell_ps.dxil", L"FresnelShellPS", L"ps_6_0"},
    ProgramOutput{"vfx_distortion_vs.dxil", L"VfxDistortionVS", L"vs_6_0"},
    ProgramOutput{"vfx_distortion_ps.dxil", L"VfxDistortionPS", L"ps_6_0"},
    ProgramOutput{"vfx_flash_vs.dxil", L"VfxFlashVS", L"vs_6_0"},
    ProgramOutput{"vfx_flash_ps.dxil", L"VfxFlashPS", L"ps_6_0"},
    ProgramOutput{"vfx_sprite_oit_ps.dxil", L"VfxSpriteOitPS", L"ps_6_0"},
    ProgramOutput{"particle_ps.dxil", L"ParticlePS", L"ps_6_0"},
    ProgramOutput{"particle_cs.dxil", L"ParticleCS", L"cs_6_0"},
    ProgramOutput{"ribbon_update_cs.dxil", L"RibbonUpdateCS", L"cs_6_0"},
    ProgramOutput{"ribbon_args_cs.dxil", L"RibbonArgsCS", L"cs_6_0"},
    ProgramOutput{"ribbon_vs.dxil", L"RibbonVS", L"vs_6_0"},
    ProgramOutput{"ribbon_add_ps.dxil", L"RibbonAddPS", L"ps_6_0"},
    ProgramOutput{"ribbon_oit_ps.dxil", L"RibbonOitPS", L"ps_6_0"},
    ProgramOutput{"fullscreen_vs.dxil", L"FullScreenVS", L"vs_6_0"},
    ProgramOutput{"temporal_ps.dxil", L"TemporalPS", L"ps_6_0"},
    ProgramOutput{"pre_exposure_ps.dxil", L"PreExposurePS", L"ps_6_0"},
    ProgramOutput{"composite_ps.dxil", L"CompositePS", L"ps_6_0"},
    ProgramOutput{"bloom_extract_ps.dxil", L"BloomExtractPS", L"ps_6_0"},
    ProgramOutput{"bloom_downsample_ps.dxil", L"BloomDownsamplePS", L"ps_6_0"},
    ProgramOutput{"bloom_upsample_ps.dxil", L"BloomUpsamplePS", L"ps_6_0"},
    ProgramOutput{"bloom_ps.dxil", L"BloomPS", L"ps_6_0"},
    ProgramOutput{"tonemap_ps.dxil", L"ToneMapPS", L"ps_6_0"},
    ProgramOutput{"outline_ps.dxil", L"OutlinePS", L"ps_6_0"},
    ProgramOutput{"fxaa_ps.dxil", L"FxaaPS", L"ps_6_0"},
    ProgramOutput{"ui_ps.dxil", L"UiPS", L"ps_6_0"},
};

constexpr std::array kGeometryPrograms{
    ProgramOutput{"cluster_scene_vs.dxil", L"ClusterSceneVS", L"vs_6_0"},
    ProgramOutput{"cluster_shadow_vs.dxil", L"ClusterShadowVS", L"vs_6_0"},
    ProgramOutput{"scene_ms.dxil", L"SceneMS", L"ms_6_5"},
    ProgramOutput{"shadow_ms.dxil", L"ShadowMS", L"ms_6_5"},
};

constexpr std::array kVisibilityPrograms{
    ProgramOutput{"visibility_ps.dxil", L"VisibilityPS", L"ps_6_0"},
    ProgramOutput{"surface_resolve_cs.dxil", L"SurfaceResolveCS", L"cs_6_0"},
    ProgramOutput{"raster_records_cs.dxil", L"BuildRasterRecordsCS", L"cs_6_0"},
    ProgramOutput{"visibility_clear_cs.dxil", L"ClearVisibilityCS", L"cs_6_0"},
    ProgramOutput{"visibility_count_cs.dxil", L"ClassifyCountCS", L"cs_6_0"},
    ProgramOutput{"visibility_prefix_cs.dxil", L"ClassifyPrefixCS", L"cs_6_0"},
    ProgramOutput{"visibility_scatter_cs.dxil", L"ClassifyScatterCS", L"cs_6_0"},
};

constexpr std::array kStaticVisibilityPrograms{
    ProgramOutput{"static_visibility_cs.dxil", L"StaticVisibilityCS", L"cs_6_0"},
    ProgramOutput{"visibility_retest_cs.dxil", L"VisibilityRetestCS", L"cs_6_0"},
};

constexpr std::array kDepthPyramidPrograms{
    ProgramOutput{"depth_pyramid_init_cs.dxil", L"DepthPyramidInitCS", L"cs_6_0"},
    ProgramOutput{"depth_pyramid_reduce_cs.dxil", L"DepthPyramidReduceCS", L"cs_6_0"},
};

constexpr std::array kSkinCachePrograms{
    ProgramOutput{"skin_cache_cs.dxil", L"SkinCacheCS", L"cs_6_0"},
    ProgramOutput{"skin_bounds_cs.dxil", L"SkinBoundsCS", L"cs_6_0"},
};
constexpr std::array kExposurePrograms{
    ProgramOutput{"exposure_clear_cs.dxil", L"ClearHistogram", L"cs_6_0"},
    ProgramOutput{"exposure_build_cs.dxil", L"BuildHistogram", L"cs_6_0"},
    ProgramOutput{"exposure_reduce_cs.dxil", L"ReduceExposure", L"cs_6_0"},
};
constexpr std::array kClusterRefinementPrograms{
    ProgramOutput{"cluster_count_cs.dxil", L"CountCuts", L"cs_6_0"},
    ProgramOutput{"cluster_prefix_cs.dxil", L"PrefixCuts", L"cs_6_0"},
    ProgramOutput{"cluster_emit_cs.dxil", L"EmitCuts", L"cs_6_0"},
    ProgramOutput{"cluster_occlusion_cs.dxil", L"CullClusters", L"cs_6_0"},
    ProgramOutput{"cluster_retest_cs.dxil", L"RetestClusters", L"cs_6_0"},
};

std::span<const ShaderFamily> ShaderFamilies()
{
    static constexpr std::array ambient_programs{
        ProgramOutput{"gtao_horizon_cs.dxil", L"GTAOHorizonCS", L"cs_6_0"},
        ProgramOutput{"gtao_temporal_cs.dxil", L"GTAOTemporalCS", L"cs_6_0"},
        ProgramOutput{"gtao_bilateral_cs.dxil", L"GTAOBilateralCS", L"cs_6_0"},
        ProgramOutput{"contact_shadow_cs.dxil", L"ContactShadowCS", L"cs_6_0"}};
    static constexpr std::array reconstruction_programs{
        ProgramOutput{"reconstruction_guides_cs.dxil", L"ReconstructionGuidesCS", L"cs_6_0"},
        ProgramOutput{"reconstruction_pre_exposure_cs.dxil", L"PreExposeReconstructionCS", L"cs_6_0"}};
    static constexpr std::array presentation_programs{
        ProgramOutput{"sdk_presentation_vs.dxil", L"PresentationVS", L"vs_6_0"},
        ProgramOutput{"sdk_presentation_ps.dxil", L"PresentationPS", L"ps_6_0"},
        ProgramOutput{"sdk_presentation_guides_cs.dxil", L"PresentationGuidesCS", L"cs_6_0"}};
    static constexpr std::array hdr_post_programs{
        ProgramOutput{"hdr_post_guides_cs.dxil", L"HdrPostGuidesCS", L"cs_6_0"},
        ProgramOutput{"hdr_post_motion_blur_cs.dxil", L"HdrPostMotionBlurCS", L"cs_6_0"},
        ProgramOutput{"hdr_post_dilate_cs.dxil", L"HdrPostDilateCS", L"cs_6_0"},
        ProgramOutput{"hdr_post_dof_cs.dxil", L"HdrPostDoFCS", L"cs_6_0"},
        ProgramOutput{"hdr_post_lut_init_cs.dxil", L"HdrPostLutInitCS", L"cs_6_0"},
        ProgramOutput{"hdr_post_grade_cs.dxil", L"HdrPostGradeCS", L"cs_6_0"}};
    static constexpr std::array light_list_programs{
        ProgramOutput{"light_list_cs.dxil", L"BuildLightLists", L"cs_6_0"}};
    static constexpr std::array scene_geometry_programs{
        ProgramOutput{"scene_geometry_cs.dxil", L"SceneGeometryCS", L"cs_6_0"}};
    static constexpr std::array virtual_shadow_programs{
        ProgramOutput{"virtual_shadow_init_cs.dxil", L"InitCS", L"cs_6_0"},
        ProgramOutput{"virtual_shadow_feedback_cs.dxil", L"FeedbackCS", L"cs_6_0"},
        ProgramOutput{"virtual_shadow_resident_cs.dxil", L"ResidentCS", L"cs_6_0"},
        ProgramOutput{"virtual_shadow_cull_cs.dxil", L"CullCS", L"cs_6_0"},
        ProgramOutput{"virtual_shadow_publish_cs.dxil", L"PublishCS", L"cs_6_0"},
        ProgramOutput{"virtual_shadow_clear_vs.dxil", L"ClearVS", L"vs_6_0"},
        ProgramOutput{"virtual_shadow_clear_ps.dxil", L"ClearPS", L"ps_6_0"}};
    static constexpr std::array volumetric_programs{
        ProgramOutput{"volumetric_density_cs.dxil", L"VolumetricDensityCS", L"cs_6_0"},
        ProgramOutput{"volumetric_hero_density_cs.dxil", L"VolumetricHeroDensityCS", L"cs_6_0"},
        ProgramOutput{"volumetric_lighting_cs.dxil", L"VolumetricLightingCS", L"cs_6_0"},
        ProgramOutput{"volumetric_temporal_cs.dxil", L"VolumetricTemporalCS", L"cs_6_0"},
        ProgramOutput{"volumetric_spatial_cs.dxil", L"VolumetricSpatialCS", L"cs_6_0"},
        ProgramOutput{"volumetric_integrate_cs.dxil", L"VolumetricIntegrateCS", L"cs_6_0"},
        ProgramOutput{"volumetric_resolve_cs.dxil", L"VolumetricResolveCS", L"cs_6_0"}};
    static constexpr std::array hlod_programs{
        ProgramOutput{"scene_hlod_clear_cs.dxil", L"ClearCS", L"cs_6_0"},
        ProgramOutput{"scene_hlod_select_cs.dxil", L"SelectCS", L"cs_6_0"},
        ProgramOutput{"scene_hlod_emit_cs.dxil", L"EmitCS", L"cs_6_0"},
        ProgramOutput{"scene_hlod_publish_cs.dxil", L"PublishCS", L"cs_6_0"},
        ProgramOutput{"scene_hlod_history_cs.dxil", L"HistoryCS", L"cs_6_0"}};
    static constexpr std::array local_probe_programs{
        ProgramOutput{"local_probe_sh_cs.dxil", L"CSSh", L"cs_6_0"},
        ProgramOutput{"local_probe_ggx_cs.dxil", L"CSGgx", L"cs_6_0"}};
    static constexpr std::array families{
        ShaderFamily{"project", "Content/Shaders/project_hs.hlsl", kProjectPrograms},
        ShaderFamily{"geometry", "Content/Shaders/geometry.hlsl", kGeometryPrograms},
        ShaderFamily{"visibility", "Content/Shaders/visibility_buffer.hlsl", kVisibilityPrograms},
        ShaderFamily{"static-visibility", "Content/Shaders/static_visibility.hlsl", kStaticVisibilityPrograms},
        ShaderFamily{"depth-pyramid", "Content/Shaders/depth_pyramid.hlsl", kDepthPyramidPrograms},
        ShaderFamily{"skin-cache", "Content/Shaders/skin_cache.hlsl", kSkinCachePrograms},
        ShaderFamily{"exposure", "Content/Shaders/exposure.hlsl", kExposurePrograms},
        ShaderFamily{"cluster-refinement", "Content/Shaders/cluster_refinement.hlsl", kClusterRefinementPrograms},
        ShaderFamily{"ambient-occlusion", "Content/Shaders/ambient_occlusion.hlsl", ambient_programs},
        ShaderFamily{"reconstruction-guides", "Content/Shaders/reconstruction_guides.hlsl", reconstruction_programs},
        ShaderFamily{"sdk-presentation", "Content/Shaders/sdk_presentation.hlsl", presentation_programs},
        ShaderFamily{"hdr-post", "Content/Shaders/hdr_post.hlsl", hdr_post_programs},
        ShaderFamily{"light-list", "Content/Shaders/light_list.hlsl", light_list_programs},
        ShaderFamily{"scene-geometry", "Content/Shaders/scene_geometry.hlsl", scene_geometry_programs},
        ShaderFamily{"virtual-shadow", "Content/Shaders/virtual_shadow.hlsl", virtual_shadow_programs},
        ShaderFamily{"volumetrics", "Content/Shaders/volumetrics.hlsl", volumetric_programs},
        ShaderFamily{"scene-hlod", "Content/Shaders/scene_hlod.hlsl", hlod_programs},
        ShaderFamily{"local-probe-bake", "Content/Shaders/local_probe_bake.hlsl", local_probe_programs},
    };
    return families;
}

const ShaderFamily *FindFamily(std::string_view name)
{
    const auto families = ShaderFamilies();
    const auto found = std::ranges::find(families, name, &ShaderFamily::name);
    return found == families.end() ? nullptr : &*found;
}

std::wstring Quote(const fs::path &path)
{
    return hs::QuoteWindowsCommandLineArgument(path.wstring());
}

void RunDxc(const fs::path &dxc, const fs::path &source_root, const fs::path &stage,
            const fs::path &output, std::string_view source_relative,
            std::wstring_view entry, std::wstring_view profile)
{
    const auto diagnostics = stage / "diagnostics" /
        (output.filename().string() + ".txt");
    const auto source = fs::path(std::string(source_relative));
    std::vector<std::wstring> args{
        L"-nologo", L"-WX", L"-O3", L"-Ges",
        L"-E", std::wstring(entry), L"-T", std::wstring(profile),
        L"-I", L"Content/Shaders", L"-Fo", output.wstring(),
        L"-Fe", fs::path(diagnostics).wstring(), source.wstring()};
    hs::shader_content::AppendRenderBindingArguments(args);
    std::wstring command = Quote(dxc);
    for (const auto &arg : args)
    {
        command += L' ';
        command += hs::QuoteWindowsCommandLineArgument(arg);
    }
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(dxc.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, source_root.c_str(), &startup, &process))
        throw std::runtime_error("DXC launch failed: " + std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    auto wait = WaitForSingleObject(process.hProcess, 120'000);
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(process.hProcess, 1);
        // TerminateProcess is asynchronous. Keep the handle open until the
        // child is actually gone so stage cleanup cannot race its file IO.
        wait = WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD code = 1;
    const auto got_code = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !got_code || code != 0)
        throw std::runtime_error("DXC compilation failed for " + output.filename().string() + ": " +
            (fs::exists(diagnostics) ? ap::ReadBytes(diagnostics) : "no diagnostic output"));
}

void RemoveTransientDirectory(const fs::path &path)
{
    if (!fs::exists(path)) return;
    std::error_code error;
    fs::remove_all(path, error);
    Require(!error, "cannot remove transient producer directory: " + path.string());
}

void RunDependencyScan(const fs::path &dxc, const fs::path &source_root,
                       const fs::path &dependency_file, const fs::path &diagnostics,
                       std::string_view source_relative, std::wstring_view entry,
                       std::wstring_view profile)
{
    const auto source = fs::path(std::string(source_relative));
    std::vector<std::wstring> args{
        L"-nologo", L"-WX", L"-O3", L"-Ges",
        L"-E", std::wstring(entry), L"-T", std::wstring(profile),
        L"-M", L"-MF", dependency_file.wstring(),
        L"-I", L"Content/Shaders", L"-Fe", diagnostics.wstring(), source.wstring()};
    hs::shader_content::AppendRenderBindingArguments(args);
    std::wstring command = Quote(dxc);
    for (const auto &arg : args)
    {
        command += L' ';
        command += hs::QuoteWindowsCommandLineArgument(arg);
    }
    STARTUPINFOW startup{.cb = sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(dxc.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, source_root.c_str(), &startup, &process))
        throw std::runtime_error("DXC dependency scan launch failed: " +
                                 std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    auto wait = WaitForSingleObject(process.hProcess, 120'000);
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(process.hProcess, 1);
        // TerminateProcess is asynchronous. Keep the handle open until the
        // child is actually gone so stage cleanup cannot race its file IO.
        wait = WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD code = 1;
    const auto got_code = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || !got_code || code != 0)
        throw std::runtime_error("DXC dependency scan failed for " + std::string(source_relative) + ": " +
            (fs::exists(diagnostics) ? ap::ReadBytes(diagnostics) : "no diagnostic output"));
}

std::size_t MakeTargetSeparator(std::string_view text)
{
    for (std::size_t index = 0; index + 1 < text.size(); ++index)
    {
        if (text[index] == ':' &&
            (text[index + 1] == ' ' || text[index + 1] == '\t'))
            return index;
    }

    return std::string_view::npos;
}

std::set<std::string> ParseMakePrerequisites(std::string_view text)
{
    std::set<std::string> paths;
    std::string token;
    bool has_token{};
    const auto flush = [&] {
        if (has_token)
        {
            paths.insert(token);
            token.clear();
            has_token = false;
        }
    };
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const auto character = text[index];
        if (character == '\\')
        {
            if (index + 1 < text.size() && text[index + 1] == '\r' &&
                index + 2 < text.size() && text[index + 2] == '\n')
            {
                index += 2;
                continue;
            }
            if (index + 1 < text.size() && text[index + 1] == '\n')
            {
                ++index;
                continue;
            }
            if (index + 1 < text.size() &&
                (text[index + 1] == ' ' || text[index + 1] == '\t' ||
                 text[index + 1] == '#' || text[index + 1] == ':' ||
                 text[index + 1] == '\\'))
            {
                token += text[++index];
                has_token = true;
            }
            else
            {
                // DXC may emit native Windows separators without Make escaping.
                token += character;
                has_token = true;
            }
            continue;
        }
        if (character == ' ' || character == '\t' || character == '\r' ||
            character == '\n')
        {
            flush();
            continue;
        }
        token += character;
        has_token = true;
    }
    flush();
    Require(!paths.empty(), "DXC dependency output is empty");
    return paths;
}

std::set<std::string> ParseDependencyFile(const fs::path &path)
{
    const auto text = ap::ReadBytes(path);
    const auto separator = MakeTargetSeparator(text);
    Require(separator != std::string_view::npos,
            "DXC dependency output has no target separator");
    return ParseMakePrerequisites(std::string_view(text).substr(separator + 1));
}

std::string LogicalShaderPath(const fs::path &source_root, const fs::path &path)
{
    const auto absolute = fs::absolute(path.is_absolute() ? path : source_root / path)
                              .lexically_normal();
    const auto relative = absolute.lexically_relative(source_root);
    Require(!relative.empty() && relative != "." && !relative.has_root_path(),
            "shader include escapes source root");
    for (const auto &part : relative)
        Require(part != "..", "shader include escapes source root");
    const auto logical = relative.generic_string();
    Require(logical.starts_with("Content/Shaders/"),
            "shader dependency is outside Content/Shaders");
    return logical;
}

std::vector<ap::Input> DiscoverInputs(const ShaderFamily &family, const fs::path &source_root,
                                      const fs::path &stage, const fs::path &dxc)
{
    const auto dependency_directory = stage / "dependencies";
    ap::MakeDirectory(dependency_directory);
    std::set<std::string> logical_paths;
    for (std::size_t index = 0; index < family.programs.size(); ++index)
    {
        const auto &program = family.programs[index];
        const auto dependency_file = dependency_directory /
            (std::to_string(index) + ".d");
        const auto diagnostics = dependency_directory /
            (std::to_string(index) + ".diagnostics.txt");
        RunDependencyScan(dxc, source_root, dependency_file, diagnostics,
                          family.source_relative, program.entry, program.profile);
        for (const auto &dependency : ParseDependencyFile(dependency_file))
            logical_paths.insert(LogicalShaderPath(source_root, fs::path(dependency)));
    }
    const bool uses_shadow_closure = std::ranges::any_of(logical_paths, [](const std::string &logical) {
        return logical == "Content/Shaders/project_hs.hlsl" ||
               logical == "Content/Shaders/shadow_data.hlsli" ||
               logical == "Content/Shaders/light_list.hlsli" ||
               logical == "Content/Shaders/geometry.hlsli";
    });
    if (uses_shadow_closure)
    {
        logical_paths.insert("Content/Shaders/local_shadow.hlsli");
    }
    std::vector<ap::Input> inputs;
    inputs.reserve(logical_paths.size() + 3);
    for (const auto &logical : logical_paths)
    {
        const auto path = source_root / fs::path(logical);
        ap::RequireRegularFile(path);
        inputs.push_back(ap::HashInput(logical, path));
    }
    hs::shader_content::AddRenderBindingInputs(inputs, source_root);
    inputs.push_back(ap::HashInput("tool/dxc.exe", dxc));
    for (const auto *library : {"dxcompiler.dll", "dxil.dll"})
        inputs.push_back(ap::HashInput(std::string("tool/") + library,
                                       dxc.parent_path() / library));
    std::ranges::sort(inputs, {}, &ap::Input::logical_id);
    return inputs;
}

void RequireStableToolInputs(const std::vector<ap::Input> &inputs)
{
    for (const auto &input : inputs)
    {
        if (!input.logical_id.starts_with("tool/"))
            continue;
        Require(ap::Sha256File(input.path) == input.digest,
                "DXC tool changed during production: " + input.path.string());
    }
}

fs::path CreateSourceSnapshot(const std::vector<ap::Input> &inputs, const fs::path &stage)
{
    const auto snapshot = stage / "source-snapshot";
    std::size_t copied = 0;
    for (const auto &input : inputs)
    {
        if (!input.logical_id.starts_with("Content/Shaders/"))
            continue;
        const auto target = snapshot / fs::path(input.logical_id);
        ap::MakeDirectory(target.parent_path());
        const auto staged = ap::CopyAndHashInput(input.logical_id, input.path, target);
        Require(staged.digest == input.digest,
                "shader source changed while creating snapshot: " + input.logical_id);
        ++copied;
    }
    Require(copied != 0, "shader source dependency snapshot is empty");
    return snapshot;
}

void Produce(const ShaderFamily &family, const fs::path &source_root,
             const fs::path &build_root, const fs::path &dxc)
{
    Require(source_root.is_absolute() && build_root.is_absolute() && dxc.is_absolute(),
            "source, build, and dxc paths must be absolute");
    ap::RequireDirectory(source_root);
    ap::RequireDirectory(source_root / "Content/Shaders");
    ap::MakeDirectory(build_root);
    ap::RequireRegularFile(dxc);
    for (const auto *library : {"dxcompiler.dll", "dxil.dll"})
        ap::RequireRegularFile(dxc.parent_path() / library);

    const auto namespace_path = "render/shader/runtime/" + std::string(family.name);
    ap::ActionSpec spec;
    spec.action_id = "render.shader.runtime." + std::string(family.name);
    spec.recipe_id = "project-hs.render.runtime-shader/" + std::string(family.name);
    spec.recipe_version = 1;
    spec.namespace_path = namespace_path;

    const auto stage = ap::CreateStage(build_root / "Actions" / namespace_path);
    try
    {
        spec.inputs = DiscoverInputs(family, source_root, stage, dxc);
        const auto result_path = build_root / "Actions" / namespace_path / "result.json";
        if (fs::exists(result_path) && fs::exists(result_path.parent_path() / "result.d"))
        {
            try
            {
                const auto result = ap::Json::parse(ap::ReadBytes(result_path));
                if (result.at("action_id") == spec.action_id &&
                    result.at("action_key") == ap::ActionKey(spec) &&
                    result.at("products").size() == family.programs.size())
                {
                    bool valid = true;
                    for (const auto &program : family.programs)
                    {
                        ap::UpstreamProduct ignored;
                        fs::path product_path;
                        (void)ap::ReadActionResult(build_root, namespace_path,
                                                   "render/shader/" + std::string(program.filename),
                                                   ignored, product_path);
                        valid &= ValidDxil(product_path);
                    }
                    if (valid)
                    {
                        ap::TouchActionResult(result_path);
                        std::cout << spec.action_id << " current\n";
                        ap::RemoveStage(stage);
                        return;
                    }
                }
            }
            catch (const std::exception &) { /* Rebuild invalid current result. */ }
        }

        const auto source_snapshot = CreateSourceSnapshot(spec.inputs, stage);
        RequireStableToolInputs(spec.inputs);

        std::vector<ap::Product> products;
        products.reserve(family.programs.size());
        ap::MakeDirectory(stage / "diagnostics");
        for (const auto &program : family.programs)
        {
            const auto output = stage / program.filename;
            RunDxc(dxc, source_snapshot, stage, output, family.source_relative,
                   program.entry, program.profile);
            Require(ValidDxil(output), "DXC did not produce a DXIL container");
            products.push_back(ap::HashProduct(
                "render/shader/" + std::string(program.filename),
                program.filename, output));
        }
        RequireStableToolInputs(spec.inputs);
        RemoveTransientDirectory(stage / "source-snapshot");
        RemoveTransientDirectory(stage / "dependencies");
        RemoveTransientDirectory(stage / "diagnostics");
        ap::Publish(build_root, spec, stage, std::move(products));
        ap::RemoveStage(stage);
        std::cout << spec.action_id << " published programs=" << family.programs.size() << '\n';
    }
    catch (...)
    {
        ap::RemoveStage(stage);
        throw;
    }
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    std::string family_name;
    fs::path source_root, build_root, dxc;
    ap::Json action_id = nullptr;
    constexpr auto usage = "usage: hs_render_runtime_shader --family=<name> "
        "--source=<absolute> --build=<absolute> --dxc=<absolute>";
    try
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::wstring_view argument = argv[index];
            const auto read_path = [&](std::wstring_view prefix, fs::path &destination) {
                if (!argument.starts_with(prefix) || !destination.empty()) return false;
                destination = fs::path(argument.substr(prefix.size()));
                return !destination.empty();
            };
            if (argument.starts_with(L"--family=") && family_name.empty())
            {
                const auto value = argument.substr(9);
                Require(!value.empty(), "family is required");
                family_name.assign(value.begin(), value.end());
            }
            else if (read_path(L"--source=", source_root) ||
                     read_path(L"--build=", build_root) ||
                     read_path(L"--dxc=", dxc))
            {
            }
            else
                throw std::runtime_error(usage);
        }
        Require(!family_name.empty() && !source_root.empty() &&
                    !build_root.empty() && !dxc.empty(), usage);
        const auto *family = FindFamily(family_name);
        Require(family != nullptr, "unsupported shader family");
        action_id = "render.shader.runtime." + std::string(family->name);
        Produce(*family, fs::absolute(source_root).lexically_normal(),
                fs::absolute(build_root).lexically_normal(),
                fs::absolute(dxc).lexically_normal());
        return 0;
    }
    catch (const std::exception &error)
    {
        ap::ReportFailure("hs_render_runtime_shader", error.what(),
                          {{"action_id", action_id}, {"family", family_name},
                           {"source_root", ap::DiagnosticPath(source_root)},
                           {"build_root", ap::DiagnosticPath(build_root)},
                           {"dependency", {{"dxc", ap::DiagnosticPath(dxc)}}}});
        return 1;
    }
}
