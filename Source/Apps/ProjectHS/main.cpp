#include <hs/runtime/application.hpp>

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace
{

hs::ApplicationConfig ParseArguments(int argc, char **argv)
{
    hs::ApplicationConfig config;
#if defined(HS_ENABLE_VALIDATION)
    config.validation = true;
#endif
#if defined(HS_ENABLE_GPU_VALIDATION)
    config.gpu_validation = true;
#endif
    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        if (argument == "--smoke")
        {
            config.smoke = true;
            config.visible = false;
            config.vsync = false;
            config.frame_cap = 0;
        }
        else if (argument == "--warp")
        {
            config.warp = true;
        }
        else if (argument == "--no-vsync")
        {
            config.vsync = false;
        }
        else if (argument == "--vsync")
        {
            config.vsync = true;
        }
        else if (argument == "--borderless")
        {
            config.borderless = true;
        }
        else if (argument == "--no-bloom")
        {
            config.bloom = false;
        }
        else if (argument == "--no-outline")
        {
            config.outline = false;
        }
        else if (argument == "--resize-test")
        {
            config.resize_test = true;
        }
        else if (argument == "--barriers=legacy")
        {
            config.barrier_mode = hs::BarrierMode::Legacy;
        }
        else if (argument == "--barriers=enhanced")
        {
            config.barrier_mode = hs::BarrierMode::Enhanced;
        }
        else if (argument.starts_with("--ticks="))
        {
            const auto value = argument.substr(8);
            std::from_chars(value.data(), value.data() + value.size(), config.maximum_ticks);
        }
        else if (argument.starts_with("--frame-cap="))
        {
            const auto value = argument.substr(12);
            std::from_chars(value.data(), value.data() + value.size(), config.frame_cap);
        }
        else if (argument.starts_with("--render-scale="))
        {
            const auto value = argument.substr(15);
            std::from_chars(value.data(), value.data() + value.size(),
                            config.render_scale_percent);
            config.render_scale_percent =
                config.render_scale_percent <= 75 ? 75u : 100u;
        }
        else if (argument.starts_with("--shadow="))
        {
            const auto value = argument.substr(9);
            std::from_chars(value.data(), value.data() + value.size(),
                            config.shadow_resolution);
            config.shadow_resolution = config.shadow_resolution <= 1024 ? 1024u : 2048u;
        }
        else if (argument.starts_with("--particles="))
        {
            const auto value = argument.substr(12);
            std::from_chars(value.data(), value.data() + value.size(),
                            config.particle_percentage);
            config.particle_percentage = config.particle_percentage <= 50 ? 50u : 100u;
        }
        else if (argument.starts_with("--artifacts="))
        {
            config.artifact_directory = argument.substr(12);
        }
        else if (argument.starts_with("--resolution="))
        {
            const auto value = argument.substr(13);
            if (const auto separator = value.find('x'); separator != std::string_view::npos)
            {
                std::from_chars(value.data(), value.data() + separator, config.width);
                std::from_chars(value.data() + separator + 1, value.data() + value.size(),
                                config.height);
                config.width = std::max(config.width, 640u);
                config.height = std::max(config.height, 360u);
            }
        }
    }
    return config;
}

} // namespace

int main(int argc, char **argv)
{
    const auto run = hs::RunApplication(ParseArguments(argc, argv));
    if (!run.result)
    {
        std::cerr << run.result.Subsystem() << ": " << run.result.Message() << '\n';
        return 1;
    }
    std::cout << "stage1 tick=" << run.final_tick << " checksum=" << run.checksum
              << " frames=" << run.rendered_frames << '\n';
    return 0;
}
