#include "../Source/Apps/ProjectHS/command_line.hpp"

#include <hs/runtime/save_store.hpp>

#include <cstdio>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

hs::ApplicationConfig Parse(std::initializer_list<std::string_view> arguments,
                            const hs::SettingsDocument *saved_settings = nullptr)
{
    return hs::ParseProjectHsArguments(
        std::span<const std::string_view>(arguments.begin(), arguments.size()),
        saved_settings);
}

hs::SettingsDocument StronglyDifferentSettings()
{
    hs::SettingsDocument settings;
    settings.graphics.output_extent = hs::GraphicsExtent{3440, 1440};
    settings.graphics.display_mode = hs::GraphicsDisplayMode::BorderlessFullscreen;
    settings.graphics.vsync = false;
    settings.graphics.frame_limit_fps = 47;
    settings.graphics.resolution_mode = hs::GraphicsResolutionMode::Manual;
    settings.graphics.reconstruction_provider.value = "internal_taau";
    settings.graphics.manual_scale_percent = 75.0f;
    settings.graphics.shadow_quality = hs::GraphicsQuality::Ultra;
    settings.graphics.particle_quality = hs::GraphicsQuality::Low;
    settings.graphics.bloom = true;
    settings.graphics.outline = true;
    return settings;
}

void TestSmokeUsesProcessDefaults()
{
    const auto smoke = Parse({"--smoke"});
    Check(smoke.smoke && !smoke.visible && !smoke.vsync && smoke.frame_cap == 0,
          "smoke mode flags");
    Check(smoke.width == 1280 && smoke.height == 720 && !smoke.borderless,
          "smoke resolution defaults");
    Check(smoke.render_scale_percent == 100 && smoke.shadow_resolution == 1024 &&
              smoke.particle_percentage == 100 && smoke.bloom && smoke.outline,
          "smoke quality defaults");

    const auto overridden = Parse({"--smoke", "--resolution=1920x1080", "--render-scale=75",
                                   "--shadow=2048", "--particles=50", "--no-bloom",
                                   "--no-outline", "--frame-cap=31"});
    Check(overridden.smoke && overridden.width == 1920 && overridden.height == 1080 &&
              overridden.render_scale_percent == 75 && overridden.shadow_resolution == 2048 &&
              overridden.particle_percentage == 50 && !overridden.bloom && !overridden.outline &&
              overridden.frame_cap == 31,
          "smoke explicit overrides");
}

void TestInteractiveSettingsAndCliPrecedence()
{
    const auto saved = StronglyDifferentSettings();
    const auto from_saved = Parse({}, &saved);
    Check(from_saved.width == 3440 && from_saved.height == 1440 && from_saved.borderless &&
              !from_saved.vsync && from_saved.frame_cap == 47,
          "saved display settings");
    Check(from_saved.render_scale_percent == 75 && from_saved.shadow_resolution == 2048 &&
              from_saved.particle_percentage == 50 && from_saved.bloom && from_saved.outline &&
              !from_saved.render_scale_explicit && !from_saved.shadow_resolution_explicit &&
              !from_saved.particle_percentage_explicit,
          "saved quality settings");

    const auto overridden = Parse({"--resolution=1920x1080", "--vsync", "--frame-cap=90",
                                   "--render-scale=100", "--shadow=1024", "--particles=100",
                                   "--no-bloom", "--no-outline"},
                                  &saved);
    Check(!overridden.smoke && overridden.width == 1920 && overridden.height == 1080 &&
              overridden.borderless && overridden.vsync && overridden.frame_cap == 90,
          "interactive cli display precedence");
    Check(overridden.render_scale_percent == 100 && overridden.shadow_resolution == 1024 &&
              overridden.particle_percentage == 100 && !overridden.bloom && !overridden.outline &&
              overridden.render_scale_explicit && overridden.shadow_resolution_explicit &&
              overridden.particle_percentage_explicit,
          "interactive cli quality precedence");
}

void TestCaptureModeValidation()
{
    const auto gpu = Parse({"--smoke", "--gpu-validation"});
    Check(gpu.validation && gpu.gpu_validation, "GPU validation enables the debug layer");
    const auto boss_capture = Parse({"--boss-vfx-capture=1"});
    Check(boss_capture.smoke && boss_capture.boss_vfx_capture == 1 && !boss_capture.visible &&
              !boss_capture.vsync && boss_capture.frame_cap == 0,
          "valid boss capture enters smoke mode");

    const auto phase2_capture = Parse({"--boss-phase2-capture"});
    Check(phase2_capture.smoke && phase2_capture.boss_phase2_capture,
          "valid phase2 capture enters smoke mode");

    const auto invalid_capture = Parse({"--boss-vfx-capture=3"});
    Check(!invalid_capture.smoke && invalid_capture.boss_vfx_capture == 0xFFFFFFFFu,
          "invalid boss capture remains interactive");
}

} // namespace

int main()
{
    try
    {
        TestSmokeUsesProcessDefaults();
        TestInteractiveSettingsAndCliPrecedence();
        TestCaptureModeValidation();
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "project_hs_arguments_tests failed: %s\n", exception.what());
        return 1;
    }
}
