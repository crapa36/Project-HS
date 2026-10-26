#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/process_info.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/renderer/renderer.hpp>
#include "../Common/world_transport_scene.hpp"
#include "../RenderTestRunner/static_shadow_action.hpp"

#include <Windows.h>

#include <memory>
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
    static constexpr auto class_name = L"ProjectHSStaticShadowBake";
    void Create()
    {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class)) throw std::runtime_error("RegisterClassW failed");
        registered = true;
        RECT rectangle{0, 0, 320, 180};
        if (!AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE)) throw std::runtime_error("AdjustWindowRect failed");
        handle = CreateWindowExW(0, class_name, L"Project HS static shadow bake", WS_OVERLAPPEDWINDOW,
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

void BakeStaticShadowGeneration(const hs::SelectedGeneration &generation,
    const std::shared_ptr<const hs::StaticRenderScene> &scene, const std::filesystem::path &artifacts,
    const hs::render_test::BeginStaticShadowArchive &begin,
    const hs::render_test::WriteStaticShadowPage &write_page)
{
    hs::RendererConfig config;
    auto startup_font = ReadDeveloperFont();
    config.font_bytes = startup_font;
    config.width = 320; config.height = 180;
    config.artifact_directory = artifacts;
    config.vsync = false; config.devtools_visible = false;
    config.temporal_reconstruction = false;
    config.vfx_quality = hs::VfxQuality::Low;
    config.ao_mode = hs::GraphicsAoMode::Off;
    config.contact_shadows = false;
    config.reflection_mode = hs::GraphicsReflectionMode::Off;
    config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
    config.content_generation_id = generation.id;
    config.content_generation_lease = std::make_shared<const hs::SelectedGeneration>(generation);
    config.product_paths = generation.product_paths;
    config.product_digests = generation.product_digests;
    BakeWindow window;
    window.Create();
    config.window = window.handle;
    hs::D3D12Renderer renderer;
    Require(renderer.Initialize(config));
    config.font_bytes = {};
    std::vector<std::byte>().swap(startup_font);
    hs::LightView sun;
    sun.direction = hs::kEnvironmentSunDirection;
    sun.quantity = 50000.0f; sun.color = {1.0f, .94f, .86f};
    sun.stable_id = 0x535342414b450001ull;
    sun.shadow_flags = hs::kLightShadowRequested;
    Require(renderer.BakeStaticShadowPages(scene, sun, begin, write_page));
    Require(renderer.Shutdown());
    if (renderer.ValidationErrorCount()) throw std::runtime_error("Static shadow producer reported native validation errors");
}
}

int main(int argc, char **argv)
{
    return hs::render_test::ProduceStaticShadow(argc, argv, hs::app::WorldTransportScene,
        BakeStaticShadowGeneration);
}
