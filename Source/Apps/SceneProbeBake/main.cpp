#include <hs/core/graphics_settings.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/jobs/task_system.hpp>
#include "../Common/world_transport_scene.hpp"
#include "scene_probe_action.hpp"
#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace hs::scene_probe_bake
{
void BakeSceneProbe(const SelectedGeneration &generation, const std::filesystem::path &output,
    const std::filesystem::path &artifacts)
{
    RendererConfig config;
    config.capture_timing_stream = false;
    config.width = 1280; config.height = 720;
    config.artifact_directory = artifacts;
    config.material_quality = GraphicsQuality::Ultra;
    config.pom_quality = GraphicsOptionalQuality::Ultra;
    config.foliage_quality = GraphicsQuality::Ultra;
    config.texture_quality = GraphicsQuality::Ultra;
    config.geometry_quality = GraphicsQuality::Ultra;
    config.content_generation_id = generation.id;
    config.content_generation_lease = std::make_shared<const SelectedGeneration>(generation);
    config.product_paths = generation.product_paths;
    config.product_digests = generation.product_digests;
    constexpr std::uint64_t kDefaultGameplaySeed = 1;
    const auto scene = app::WorldTransportScene(generation, kDefaultGameplaySeed);
    if (!scene) throw std::runtime_error("Scene probe world transport scene is empty");
    TaskSystem workers(4);
    config.task_system = &workers;
    const auto sun = EnvironmentSun();
    const auto start = std::chrono::steady_clock::now();
    std::ofstream progress(artifacts / "bake-progress.txt", std::ios::trunc);
    progress << "phase=bake\n" << std::flush;
    if (!progress) throw std::runtime_error("Cannot write scene-probe bake progress");
    std::vector<std::byte> bytes;
    if (auto baked = D3D12Renderer::BakeSceneProbePackage(config, scene, sun, bytes); !baked)
        throw std::runtime_error(std::string(baked.Subsystem()) + ": " + std::string(baked.Message()));
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("Cannot write scene-probe package");
    progress << "phase=complete elapsed_ms="
        << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
        << " bytes=" << bytes.size() << '\n';
    if (!progress) throw std::runtime_error("Cannot write scene-probe bake progress");
}
}

int main(int argc, char **argv)
{
    return hs::scene_probe_bake::ProduceSceneProbe(argc, argv);
}
