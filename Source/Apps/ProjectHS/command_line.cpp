#include "command_line.hpp"

#include <hs/renderer/renderer.hpp>
#if defined(HS_DEVELOPMENT_TOOLS)
#include <hs/game_domain/game_types.hpp>
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace hs
{

ApplicationConfig ParseProjectHsArguments(
    std::span<const std::string_view> args)
{
    ApplicationConfig config;
    config.surface_frontend = SurfaceFrontend::VisibilityBuffer;
#if defined(HS_DEVELOPMENT_TOOLS)
    const auto parse_number = [](std::string_view text, auto &destination) {
        auto candidate = destination;
        const auto end = text.data() + text.size();
        const auto parsed = std::from_chars(text.data(), end, candidate);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return false;
        destination = candidate;
        return true;
    };
#endif
    for (const std::string_view argument : args)
    {
#if defined(HS_DEVELOPMENT_TOOLS)
        if (argument == "--test-room")
        {
            config.test_room = true;
        }
        else if (argument.starts_with("--test-room-capture="))
        {
            const auto value = argument.substr(std::string_view{"--test-room-capture="}.size());
            std::uint32_t station{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), station);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && station > 0)
                config.test_room_capture = station;
        }
        else if (argument.starts_with("--test-room-time="))
        {
            const auto value = argument.substr(std::string_view{"--test-room-time="}.size());
            float seconds{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                std::isfinite(seconds) && seconds >= 0.0f && seconds <= 30.0f)
                config.test_room_time = seconds;
        }
        else if (argument == "--smoke")
        {
            config.smoke = true;
            config.visible = false;
            config.vsync = false;
            config.frame_cap = 0;
        }
        else if (argument == "--smoke-render-lockstep")
        {
            config.smoke_render_lockstep = true;
        }
#endif
#if defined(HS_DEVELOPMENT_TOOLS)
        if (argument == "--warp")
        {
            config.warp = true;
        }
        else if (argument == "--validation")
        {
            config.validation = true;
        }
        else if (argument == "--gpu-validation")
        {
            config.validation = true;
            config.gpu_validation = true;
        }
#endif
        if (argument == "--no-vsync")
        {
            config.vsync_explicit = true;
            config.vsync = false;
        }
        else if (argument == "--vsync")
        {
            config.vsync_explicit = true;
            config.vsync = true;
        }
        else if (argument == "--borderless")
        {
            config.borderless_explicit = true;
            config.borderless = true;
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        else if (argument == "--environment-preview")
        {
            config.environment_preview = true;
        }
        else if (argument == "--character-preview")
        {
            config.character_preview = true;
        }
#endif
        else if (argument == "--mute")
        {
            config.mute_audio = true;
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        else if (argument.starts_with("--monster-preview="))
        {
            const auto value = argument.substr(18);
            const auto first = value.find(',');
            const auto second = first == std::string_view::npos
                                    ? first : value.find(',', first + 1);
            if (first != std::string_view::npos && second != std::string_view::npos)
            {
                std::uint32_t asset{}, clip{};
                float time{};
                const auto asset_result = std::from_chars(
                    value.data(), value.data() + first, asset);
                const auto clip_result = std::from_chars(
                    value.data() + first + 1, value.data() + second, clip);
                const auto time_result = std::from_chars(
                    value.data() + second + 1, value.data() + value.size(), time);
                if (asset_result.ec == std::errc{} && asset_result.ptr == value.data() + first &&
                    clip_result.ec == std::errc{} && clip_result.ptr == value.data() + second &&
                    time_result.ec == std::errc{} && time_result.ptr == value.data() + value.size() &&
                    asset < 6 && clip < 5 && time >= 0.0f && time <= 1.0f)
                {
                    config.monster_preview_asset = asset;
                    config.monster_preview_clip = clip;
                    config.monster_preview_time = time;
                    config.character_preview = true;
                    config.mute_audio = true;
                    config.smoke = true;
                    config.visible = false;
                    config.vsync = false;
                    config.frame_cap = 0;
                }
            }
        }
        else if (argument.starts_with("--boss-vfx-capture="))
        {
            const auto value = argument.substr(std::string_view{"--boss-vfx-capture="}.size());
            std::uint32_t boss{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), boss);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && boss < 3)
            {
                config.boss_vfx_capture = boss;
                config.smoke = true;
                config.visible = false;
                config.vsync = false;
                config.frame_cap = 0;
            }
        }
        else if (argument == "--boss-phase2-capture")
        {
            config.boss_phase2_capture = true;
            config.smoke = true;
            config.visible = false;
            config.vsync = false;
            config.frame_cap = 0;
        }
        else if (argument.starts_with("--slime-family-preview="))
        {
            const auto value = argument.substr(std::string_view{"--slime-family-preview="}.size());
            std::uint32_t count{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                count >= 1 && count <= 300)
            {
                config.slime_family_preview_count = count;
                config.smoke = true;
                config.visible = false;
                config.vsync = false;
                config.frame_cap = 0;
            }
        }
        else if (argument.starts_with("--preview-camera="))
        {
            const auto value = argument.substr(17);
            const auto first = value.find(',');
            const auto second = first == std::string_view::npos
                                    ? first : value.find(',', first + 1);
            if (first != std::string_view::npos && second != std::string_view::npos)
            {
                float yaw{}, pitch{}, distance{};
                const auto yaw_result = std::from_chars(value.data(), value.data() + first, yaw);
                const auto pitch_result = std::from_chars(value.data() + first + 1,
                                                          value.data() + second, pitch);
                const auto distance_result = std::from_chars(value.data() + second + 1,
                                                              value.data() + value.size(), distance);
                if (yaw_result.ec == std::errc{} && yaw_result.ptr == value.data() + first &&
                    pitch_result.ec == std::errc{} && pitch_result.ptr == value.data() + second &&
                    distance_result.ec == std::errc{} &&
                    distance_result.ptr == value.data() + value.size() &&
                    std::isfinite(yaw) && std::isfinite(pitch) && std::isfinite(distance) &&
                    yaw >= -360.0f && yaw <= 720.0f && pitch >= -80.0f && pitch <= 80.0f &&
                    distance >= 1.0f && distance <= 100.0f)
                {
                    config.preview_camera_override = true;
                    config.preview_camera_yaw = yaw;
                    config.preview_camera_pitch = pitch;
                    config.preview_camera_distance = distance;
                }
            }
        }
        else if (argument == "--vfx-showcase")
        {
            config.vfx_showcase = true;
        }
        else if (argument.starts_with("--skill-vfx-capture="))
        {
            std::uint32_t skill{};
            if (!parse_number(argument.substr(std::string_view{"--skill-vfx-capture="}.size()), skill) ||
                skill >= kCombatSkillCount) continue;
            config.skill_vfx_capture = skill;
            config.smoke = true;
            config.visible = false;
            config.vsync = false;
            config.frame_cap = 0;
        }
        else if (argument.starts_with("--skill-vfx-upgrade-mask="))
        {
            std::uint32_t mask{};
            if (parse_number(argument.substr(std::string_view{"--skill-vfx-upgrade-mask="}.size()), mask))
                config.skill_vfx_upgrade_mask = mask & 0xFFu;
        }
        else if (argument.starts_with("--skill-vfx-aim="))
        {
            const auto value = argument.substr(16);
            if (const auto comma = value.find(','); comma != std::string_view::npos)
            {
                Float2 aim{};
                if (parse_number(value.substr(0, comma), aim.x) && parse_number(value.substr(comma + 1), aim.y) &&
                    std::isfinite(aim.x) && std::isfinite(aim.y))
                    config.skill_vfx_aim = aim;
            }
        }
#endif
#if defined(HS_DEVELOPMENT_TOOLS)
        if (argument == "--record-playtest")
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
#endif
        else if (argument == "--no-bloom")
        {
            config.bloom_explicit = true;
            config.bloom = false;
        }
        else if (argument.starts_with("--generation="))
        {
            config.generation_pointer =
                std::filesystem::path(argument.substr(std::string_view{"--generation="}.size()));
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        else if (argument == "--resize-test")
        {
            config.resize_test = true;
        }
#endif
#if defined(HS_DEVELOPMENT_TOOLS)
        else if (argument == "--barriers=legacy")
        {
            config.barrier_mode = BarrierMode::Legacy;
        }
        else if (argument == "--barriers=enhanced")
        {
            config.barrier_mode = BarrierMode::Enhanced;
        }
#endif
        else if (argument.starts_with("--camera-zoom="))
        {
            const auto value = argument.substr(std::string_view{"--camera-zoom="}.size());
            std::uint32_t zoom{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), zoom);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                zoom >= 15 && zoom <= 120)
                config.camera_zoom_percent = zoom;
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        else if (argument.starts_with("--ticks="))
        {
            const auto value = argument.substr(8);
            parse_number(value, config.maximum_ticks);
        }
        else if (argument.starts_with("--seed="))
        {
            const auto value = argument.substr(7);
            parse_number(value, config.seed);
        }
#endif
        else if (argument.starts_with("--frame-cap="))
        {
            const auto value = argument.substr(12);
            std::uint32_t frame_cap{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frame_cap);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && frame_cap <= 1000)
            {
                config.frame_cap = frame_cap;
                config.frame_cap_explicit = true;
            }
        }
        else if (argument.starts_with("--render-scale="))
        {
            const auto value = argument.substr(15);
            std::uint32_t parsed_value{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
            { config.render_scale_percent = parsed_value <= 75 ? 75u : 100u; config.render_scale_explicit = true; }
        }
        else if (argument.starts_with("--shadow="))
        {
            const auto value = argument.substr(9);
            std::uint32_t parsed_value{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
            { config.shadow_resolution = parsed_value <= 1024 ? 1024u : 2048u; config.shadow_resolution_explicit = true; }
        }
        else if (argument.starts_with("--particles="))
        {
            const auto value = argument.substr(12);
            std::uint32_t parsed_value{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
            { config.particle_percentage = parsed_value <= 50 ? 50u : 100u; config.particle_percentage_explicit = true; }
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        else if (argument == "--capture-diagnostics")
        {
            config.capture_diagnostics = true;
        }
        else if (argument.starts_with("--artifacts="))
        {
            config.artifact_directory = argument.substr(12);
        }
#endif
        else if (argument.starts_with("--resolution="))
        {
            const auto value = argument.substr(13);
            if (const auto separator = value.find('x'); separator != std::string_view::npos)
            {
                std::uint32_t width{}, height{};
                const auto parsed_width = std::from_chars(value.data(), value.data() + separator, width);
                const auto parsed_height = std::from_chars(value.data() + separator + 1,
                                                          value.data() + value.size(), height);
                if (parsed_width.ec == std::errc{} && parsed_width.ptr == value.data() + separator &&
                    parsed_height.ec == std::errc{} && parsed_height.ptr == value.data() + value.size())
                {
                    config.width = std::max(width, 640u);
                    config.height = std::max(height, 360u);
                    config.resolution_explicit = true;
                }
            }
        }
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    if (config.test_room_capture)
    {
        config.test_room = true;
        config.smoke = true;
        config.visible = false;
        config.smoke_render_lockstep = true;
        config.mute_audio = true;
        config.vsync = false;
        config.frame_cap = 0;
        config.maximum_ticks = 30 + static_cast<Tick>(std::ceil(config.test_room_time * 60.0f));
    }
#endif
    return config;
}

} // namespace hs
