#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/process_info.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/graphics_settings.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/render_data/scene_probe.hpp>
#include <hs/render_data/render_frame_input.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/jobs/task_system.hpp>
#include "../Common/world_transport_scene.hpp"
#include "scene_probe_action.hpp"

#include <Windows.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace
{
void Require(const hs::Result &result)
{
    if (!result) throw std::runtime_error(std::string(result.Subsystem()) + ": " + std::string(result.Message()));
}

std::vector<std::byte> ReadDeveloperFont()
{
    hs::ShippedContentOwner file;
    Require(hs::ShippedContentOwner::Open(hs::CurrentExecutableDirectory() / L"Fonts" / L"NotoSansKR.ttf", file));
    if (!file.Size() || file.Size() > UINT32_MAX) throw std::runtime_error("Developer font extent is invalid");
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.Size()));
    Require(file.ReadExact(0, bytes));
    return bytes;
}

struct BakeWindow
{
    HWND handle{};
    HINSTANCE instance{GetModuleHandleW(nullptr)};
    bool registered{};
    static constexpr auto class_name = L"ProjectHSSceneProbeBake";
    void Create()
    {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class)) throw std::runtime_error("RegisterClassW failed");
        registered = true;
        RECT rectangle{0, 0, 1280, 720};
        if (!AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE)) throw std::runtime_error("AdjustWindowRect failed");
        handle = CreateWindowExW(0, class_name, L"Project HS scene probe bake", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, instance, nullptr);
        if (!handle) throw std::runtime_error("CreateWindowExW failed");
    }
    ~BakeWindow()
    {
        if (handle) DestroyWindow(handle);
        if (registered) UnregisterClassW(class_name, instance);
    }
};

}

namespace hs::scene_probe_bake
{
void BakeSceneProbe(const hs::SelectedGeneration &generation, const std::filesystem::path &output,
    const std::filesystem::path &artifacts, std::uint32_t probe_count)
{
    hs::RendererConfig config;
    config.capture_timing_stream = false;
    config.vsync = false;
    config.interpolate = false;
    config.devtools_visible = false;
    config.width = 1280;
    config.height = 720;
    config.artifact_directory = artifacts;
    config.material_quality = hs::GraphicsQuality::Ultra;
    config.pom_quality = hs::GraphicsOptionalQuality::Ultra;
    config.foliage_quality = hs::GraphicsQuality::Ultra;
    config.texture_quality = hs::GraphicsQuality::Ultra;
    config.geometry_quality = hs::GraphicsQuality::Ultra;
    config.content_generation_id = generation.id;
    config.content_generation_lease = std::make_shared<const hs::SelectedGeneration>(generation);
    config.product_paths = generation.product_paths;
    config.product_digests = generation.product_digests;

    std::filesystem::path scene_hlod_path;
    Require(generation.Require(config.scene_hlod_product_id, scene_hlod_path));
    // Match ApplicationConfig and SimulationConfig's default gameplay seed.
    constexpr std::uint64_t kDefaultGameplaySeed = 1;
    const auto world_scene = hs::app::WorldTransportScene(generation, kDefaultGameplaySeed);
    if (!world_scene) throw std::runtime_error("Scene probe world transport scene is empty");
    BakeWindow window;
    window.Create();
    config.window = window.handle;
    hs::TaskSystem recording_workers(4);
    auto recording_group = recording_workers.CreateTaskGroup();
    config.task_executor.submit = [&](std::function<void()> record) {
        const bool accepted = recording_group.Submit(
            [record = std::move(record)](std::stop_token) { record(); });
        return accepted ? hs::Result::Success() :
            hs::Result::Failure(hs::ErrorCode::TaskFailed, "hs_scene_probe_bake", "Could not submit recording task.");
    };
    config.task_executor.join = [&] {
        auto joined = recording_group.Wait();
        recording_group = recording_workers.CreateTaskGroup();
        return joined;
    };
    auto startup_font = ReadDeveloperFont();
    config.font_bytes = startup_font;
    hs::D3D12Renderer renderer;
    const auto bake_start=std::chrono::steady_clock::now();
    std::ofstream progress(artifacts/"bake-progress.txt",std::ios::trunc);
    const auto record_progress=[&](const char *phase,std::uint32_t frame,hs::RendererSceneProbeBakeStatus status) {
        progress<<"phase="<<phase<<" frame="<<frame<<" elapsed_ms="
            <<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-bake_start).count()
            <<" sources_ready="<<status.sources_ready<<" capture_complete="<<status.capture_complete<<'\n';
        if(frame&&!status.sources_ready)
            for(const auto &[id,path]:generation.product_paths)
            {
                if(!id.starts_with("render/texture/"))continue;
                const auto texture=renderer.GetTextureStreamingStatus(id);
                if(texture.registered&&(texture.pending||texture.actual_resident_mip!=texture.effective_desired_mip))
                    progress<<"texture="<<id<<" pending="<<texture.pending<<" actual="<<texture.actual_resident_mip
                        <<" desired="<<texture.effective_desired_mip<<" target="<<texture.target_mip
                        <<" requests="<<texture.requests<<" uploads="<<texture.uploads
                        <<" publications="<<texture.publications<<'\n';
            }
        progress.flush();
        if(!progress)throw std::runtime_error("Cannot write scene-probe bake progress");
    };
    record_progress("initialize",0,{});
    Require(renderer.Initialize(config));
    record_progress("render",0,{});
    config.font_bytes = {};
    std::vector<std::byte>().swap(startup_font);

    hs::RenderSnapshotPair pair;
    pair.has_current = true;
    pair.current.static_scene = world_scene;
    pair.current.header.session_id = 1;
    pair.current.camera.target = {0, 1, 0};
    pair.current.camera.yaw_degrees = 0;
    pair.current.camera.pitch_degrees = 22;
    pair.current.camera.distance = 22;
    hs::LightView sun;
    sun.direction = hs::kEnvironmentSunDirection;
    sun.quantity = 3.0f * hs::kLightWhiteLumensPerWatt;
    sun.color = {1.0f, .92f, .78f};
    sun.stable_id = 0x48535f53554e0001ull;
    sun.type = hs::LightType::Directional;
    sun.unit = hs::LightUnit::Lux;
    sun.shadow_flags = hs::kLightShadowRequested;
    pair.current.lights = std::span(&sun, 1);
    hs::RenderInstance player;
    player.mesh = hs::RenderMesh::Archer;
    player.scale = {.97174913f, .97174913f, .97174913f};
    player.stable_id = 1;
    const std::array instances{player};
    hs::RendererFrameResult frame_result;
    (void)probe_count;
    hs::RendererSceneProbeBakeStatus previous_status;
    for (std::uint32_t frame = 1;; ++frame)
    {
        pair.current.header.tick = frame;
        pair.current.header.simulation_time = std::chrono::nanoseconds(
            static_cast<std::int64_t>(frame) * 1000000000ll / 60);
        auto current = instances;
        current[0].position.x = static_cast<float>(frame % 60) / 60.f;
        pair.current.instances = current;
        auto view = renderer.MainViewInput(pair.current.camera);
        const hs::RenderFrameInput render_input{.views = std::span(&view, 1), .snapshots = pair,
            .wait_for_streaming = true};
        Require(renderer.Render(render_input, frame_result));
        if (!frame_result.vfx_world_step_submitted || frame_result.vfx_world_tick != frame ||
            frame_result.vfx_world_session_id != pair.current.header.session_id)
            throw std::runtime_error("Submitted world-step receipt does not match the rendered session/tick");
        const auto status=renderer.GetSceneProbeBakeStatus();
        if((frame&(frame-1))==0||status.sources_ready!=previous_status.sources_ready||status.capture_complete)
            record_progress("render",frame,status);
        previous_status=status;
        if (status.capture_complete) break;
    }
    if (frame_result.views.empty()) throw std::runtime_error("Scene probe bake completed without a rendered main view");
    Require(renderer.Capture({.path = output, .kind = hs::CaptureKind::SceneProbePackage}));
    Require(renderer.Shutdown());
}
}

int main(int argc, char **argv)
{
    return hs::scene_probe_bake::ProduceSceneProbe(argc, argv);
}
