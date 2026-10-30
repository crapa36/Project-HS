#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/renderer/renderer.hpp>
#include "../Common/world_transport_scene.hpp"
#include "../RenderTestRunner/static_shadow_action.hpp"

#include <Windows.h>

#include <memory>
#include <stdexcept>
#include <string>

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

void BakeStaticShadowGeneration(const hs::SelectedGeneration &generation,
    const std::shared_ptr<const hs::StaticRenderScene> &scene, const std::filesystem::path &artifacts,
    const hs::render_test::BeginStaticShadowArchive &begin,
    const hs::render_test::WriteStaticShadowPage &write_page)
{
    hs::RendererConfig config;
    config.width = 320; config.height = 180;
    config.artifact_directory = artifacts;
    config.temporal_reconstruction = false;
    config.content_generation_id = generation.id;
    config.content_generation_lease = std::make_shared<const hs::SelectedGeneration>(generation);
    config.product_paths = generation.product_paths;
    config.product_digests = generation.product_digests;
    hs::LightView sun;
    sun.direction = hs::kEnvironmentSunDirection;
    sun.quantity = 50000.0f; sun.color = {1.0f, .94f, .86f};
    sun.stable_id = 0x535342414b450001ull;
    sun.shadow_flags = hs::kLightShadowRequested;
    Require(hs::D3D12Renderer::BakeStaticShadowPages(config, scene, sun, begin, write_page));
}
}

int main(int argc, char **argv)
{
    return hs::render_test::ProduceStaticShadow(argc, argv, hs::app::WorldTransportScene,
        BakeStaticShadowGeneration);
}
