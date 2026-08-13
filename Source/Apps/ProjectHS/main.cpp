#include <hs/runtime/application.hpp>
#include <hs/runtime/save_store.hpp>

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
#if defined(_DEBUG)
    config.record_playtest = true;
#endif
    hs::SettingsData settings;
    if (hs::SaveStore store; store.LoadSettings(settings))
    {
        config.width = settings.width;
        config.height = settings.height;
        config.borderless = settings.borderless;
        config.vsync = settings.vsync;
        config.frame_cap = settings.frame_cap;
        config.render_scale_percent = settings.render_scale_percent;
        config.shadow_resolution = settings.shadow_resolution;
        config.particle_percentage = settings.particle_percentage;
        config.bloom = settings.bloom;
        config.outline = settings.outline;
    }
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
        else if (argument == "--character-preview")
        {
            config.character_preview = true;
        }
        else if (argument == "--vfx-showcase")
        {
            config.vfx_showcase = true;
        }
        else if (argument.starts_with("--skill-vfx-capture="))
        {
            const auto value = argument.substr(20);
            std::from_chars(value.data(), value.data() + value.size(),
                            config.skill_vfx_capture);
            config.smoke = true;
            config.visible = false;
            config.vsync = false;
            config.frame_cap = 0;
        }
        else if (argument.starts_with("--skill-vfx-upgrade-mask="))
        {
            const auto value = argument.substr(25);
            std::from_chars(value.data(), value.data() + value.size(),
                            config.skill_vfx_upgrade_mask);
            config.skill_vfx_upgrade_mask &= 0xFFu;
        }
        else if (argument.starts_with("--skill-vfx-aim="))
        {
            const auto value = argument.substr(16);
            if (const auto comma = value.find(','); comma != std::string_view::npos)
            {
                std::from_chars(value.data(), value.data() + comma,
                                config.skill_vfx_aim.x);
                std::from_chars(value.data() + comma + 1, value.data() + value.size(),
                                config.skill_vfx_aim.y);
            }
        }
        else if (argument == "--record-playtest")
        {
            config.record_playtest = true;
        }
        else if (argument == "--no-playtest-recording")
        {
            config.record_playtest = false;
        }
        else if (argument.starts_with("--playtest-output="))
        {
            config.playtest_output_directory = argument.substr(18);
            config.record_playtest = true;
        }
        else if (argument.starts_with("--replay="))
        {
            config.replay_directory = argument.substr(9);
            config.record_playtest = true;
        }
        else if (argument.starts_with("--replay-compare="))
        {
            config.replay_directory = argument.substr(17);
            config.replay_compare = true;
            config.record_playtest = true;
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
        else if (argument.starts_with("--seed="))
        {
            const auto value = argument.substr(7);
            std::from_chars(value.data(), value.data() + value.size(), config.seed);
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
    std::cout << "runtime tick=" << run.final_tick << " checksum=" << run.checksum
              << " frames=" << run.rendered_frames << '\n';
    if (!run.playtest_directory.empty())
        std::cout << "playtest=" << run.playtest_directory.string() << '\n';
    return 0;
}
