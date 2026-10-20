#include "../Source/Apps/ProjectHS/command_line.hpp"
#include "../Source/Runtime/Private/graphics_settings_commit.hpp"
#include "../Source/Runtime/Private/startup_graphics_settings.hpp"

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

hs::ApplicationConfig Parse(std::initializer_list<std::string_view> arguments)
{
    return hs::ParseProjectHsArguments(
        std::span<const std::string_view>(arguments.begin(), arguments.size()));
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
    auto saved = StronglyDifferentSettings();
    saved.graphics.bloom = false;
    saved.graphics.outline = false;
    saved.opaque_optional_json = R"({"graphics":{"future_key":"keep"}})";
    const auto config = Parse({});
    auto requested = saved;
    const auto from_saved = hs::PrepareStartupGraphicsSettings(requested, config);
    Check(from_saved.width == 3440 && from_saved.height == 1440 && from_saved.borderless &&
              !from_saved.vsync && from_saved.frame_cap == 47 &&
              !config.resolution_explicit && !config.vsync_explicit &&
              !config.frame_cap_explicit && !config.borderless_explicit,
          "saved display settings");
    Check(from_saved.render_scale_percent == 75 && from_saved.shadow_resolution == 2048 &&
              from_saved.particle_percentage == 50 && !from_saved.bloom && !from_saved.outline &&
              !config.render_scale_explicit && !config.shadow_resolution_explicit &&
              !config.particle_percentage_explicit && !config.bloom_explicit && !config.outline_explicit &&
              requested.opaque_optional_json == saved.opaque_optional_json,
          "saved quality settings");

    const auto overrides = Parse({"--resolution=1920x1080", "--vsync", "--frame-cap=90",
                                   "--render-scale=100", "--shadow=1024", "--particles=100",
                                   "--no-bloom", "--no-outline"});
    requested = StronglyDifferentSettings();
    const auto overridden = hs::PrepareStartupGraphicsSettings(requested, overrides);
    Check(!overrides.smoke && overridden.width == 1920 && overridden.height == 1080 &&
              overridden.borderless && overridden.vsync && overridden.frame_cap == 90 &&
              overrides.resolution_explicit && overrides.vsync_explicit &&
              overrides.frame_cap_explicit && !overrides.borderless_explicit,
          "interactive cli display precedence");
    Check(overridden.render_scale_percent == 100 && overridden.shadow_resolution == 1024 &&
              overridden.particle_percentage == 100 && !overridden.bloom && !overridden.outline &&
              overrides.render_scale_explicit && overrides.shadow_resolution_explicit &&
              overrides.particle_percentage_explicit && overrides.bloom_explicit && overrides.outline_explicit,
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

void TestPlaytestRecordingOptIn()
{
    Check(!Parse({}).record_playtest,
          "Debug and Release require explicit playtest recording");
    Check(!Parse({"--smoke"}).record_playtest &&
              !Parse({"--capture-diagnostics"}).record_playtest,
          "smoke and diagnostics do not implicitly record playtests");
    for (const std::string_view option : {"--record-playtest", "--playtest-output=recording",
                                         "--replay=recording", "--replay-compare=recording"})
    {
        Check(Parse({option}).record_playtest, "recording option enables recording");
        Check(!Parse({option, "--no-playtest-recording"}).record_playtest,
              "later recording opt-out wins");
        Check(Parse({"--no-playtest-recording", option}).record_playtest,
              "later recording opt-in wins");
    }
    Check(Parse({"--playtest-output=recording"}).playtest_output_directory == "recording",
          "recording output path is preserved");
    const auto replay = Parse({"--replay=recording", "--no-playtest-recording"});
    Check(replay.replay_directory == "recording" && !replay.replay_compare,
          "recording opt-out preserves replay input");
    const auto compare = Parse({"--replay-compare=recording", "--no-playtest-recording"});
    Check(compare.replay_directory == "recording" && compare.replay_compare,
          "recording opt-out preserves replay comparison");
}

void TestStartupGraphicsAuthority()
{
    auto saved = StronglyDifferentSettings();
    saved.graphics.display_mode = hs::GraphicsDisplayMode::Windowed;
    saved.graphics.vsync = true;
    const auto config = Parse({"--resolution=1920x1080", "--no-vsync", "--frame-cap=0"});
    auto requested = saved;
    const auto effective = hs::PrepareStartupGraphicsSettings(requested, config);
    Check(requested.graphics.output_extent == hs::GraphicsOutputExtent{hs::GraphicsExtent{1920, 1080}} &&
              !requested.graphics.vsync && requested.graphics.frame_limit_fps == 0 &&
              effective.width == 1920 && effective.height == 1080 && !effective.borderless &&
              !effective.vsync && effective.frame_cap == 0,
          "CLI display overrides reach both startup authorities");
    hs::GraphicsSettingsCommit commit(requested, effective, false);
    Check(commit.EditGraphics({hs::UiCommandKind::SetOutline, 0}), "later graphics edit");
    const auto packet = commit.NextRequest();
    Check(packet && packet->graphics->output_extent == requested.graphics.output_extent &&
              !packet->graphics->vsync && packet->graphics->frame_limit_fps == 0 &&
              packet->effective.width == 1920 && packet->effective.height == 1080 &&
              !packet->effective.vsync && packet->effective.frame_cap == 0,
          "later graphics commit retains startup CLI overrides");

    saved.graphics.output_extent.reset();
    saved.graphics.preset = hs::GraphicsPreset::Ultra;
    saved.graphics.vsync = false;
    requested = saved;
    const auto unchanged = hs::PrepareStartupGraphicsSettings(requested, Parse({}));
    Check(!requested.graphics.output_extent && requested.graphics.preset == hs::GraphicsPreset::Ultra &&
              requested.graphics.frame_limit_fps == 47 && !unchanged.vsync &&
              unchanged.width == 1280 && unchanged.height == 720,
          "no CLI preserves saved automatic extent and graphics profile");

    requested = hs::LegacyDefaultSettingsDocument();
    const auto defaults = hs::PrepareStartupGraphicsSettings(requested, Parse({}));
    Check(defaults.width == 1280 && defaults.height == 720 && !defaults.borderless &&
              defaults.vsync && defaults.frame_cap == 60,
          "first-run display defaults remain unchanged");
    const auto borderless = Parse({"--borderless"});
    (void)hs::PrepareStartupGraphicsSettings(requested, borderless);
    Check(borderless.borderless_explicit &&
              requested.graphics.display_mode == hs::GraphicsDisplayMode::BorderlessFullscreen,
          "explicit borderless reaches startup graphics");

    requested = saved;
    requested.graphics.display_mode = hs::GraphicsDisplayMode::BorderlessFullscreen;
    requested.graphics.bloom = requested.graphics.outline = false;
    const auto smoke = hs::PrepareStartupGraphicsSettings(requested, Parse({"--smoke"}));
    Check(smoke.width == 1280 && smoke.height == 720 && !smoke.borderless &&
              !requested.graphics.vsync && requested.graphics.frame_limit_fps == 0 &&
              smoke.render_scale_percent == 100 && smoke.shadow_resolution == 1024 &&
              smoke.particle_percentage == 100 && smoke.bloom && smoke.outline,
          "smoke graphics defaults replace stale profile intent");
    requested = saved;
    const auto smoke_override = hs::PrepareStartupGraphicsSettings(requested,
        Parse({"--smoke", "--resolution=1920x1080", "--borderless", "--render-scale=75",
               "--shadow=2048", "--particles=50", "--no-bloom", "--no-outline"}));
    Check(smoke_override.width == 1920 && smoke_override.height == 1080 && smoke_override.borderless &&
              smoke_override.render_scale_percent == 75 && smoke_override.shadow_resolution == 2048 &&
              smoke_override.particle_percentage == 50 && !smoke_override.bloom && !smoke_override.outline,
          "smoke explicit graphics overrides win");

    const auto invalid = Parse({"--resolution=1920xbroken", "--frame-cap=90junk", "--frame-cap=1001"});
    requested = saved;
    const auto after_invalid = hs::PrepareStartupGraphicsSettings(requested, invalid);
    Check(!invalid.resolution_explicit && !invalid.frame_cap_explicit &&
              after_invalid.width == 1280 && after_invalid.height == 720 && after_invalid.frame_cap == 47,
          "invalid CLI display values cannot partially override saved settings");
}

} // namespace

int main()
{
    try
    {
        Check(!Parse({}).capture_diagnostics, "detailed diagnostics disabled by default");
        Check(Parse({"--capture-diagnostics"}).capture_diagnostics, "explicit capture diagnostics");
        TestSmokeUsesProcessDefaults();
        TestInteractiveSettingsAndCliPrecedence();
        TestCaptureModeValidation();
        TestPlaytestRecordingOptIn();
        TestStartupGraphicsAuthority();
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "project_hs_arguments_tests failed: %s\n", exception.what());
        return 1;
    }
}
