#pragma once

#include <hs/core/result.hpp>
#include <hs/core/types.hpp>

#include <filesystem>
#include <vector>

namespace hs
{

struct ApplicationTimelineAction
{
    Sequence sequence{};
    Tick target_tick{};
    std::uint8_t kind{};
    std::uint64_t value{};
    std::uint32_t secondary{};
    Float2 position{};
};

struct ApplicationConfig
{
    bool smoke{};
    bool warp{};
    bool visible{true};
    bool validation{};
    bool gpu_validation{};
    bool vsync{true};
    bool bloom{true};
    bool outline{true};
    bool resize_test{};
    bool borderless{};
    bool character_preview{};
    bool mute_audio{};
    std::uint32_t monster_preview_asset{0xFFFFFFFFu};
    std::uint32_t slime_family_preview_count{};
    bool environment_preview{};
    std::uint32_t monster_preview_clip{};
    float monster_preview_time{0.5f};
    bool preview_camera_override{};
    float preview_camera_yaw{180.0f};
    float preview_camera_pitch{10.0f};
    float preview_camera_distance{8.0f};
    bool vfx_showcase{};
    std::uint32_t skill_vfx_capture{0xFFFFFFFFu};
    std::uint32_t skill_vfx_upgrade_mask{0xFFu};
    Float2 skill_vfx_aim{0.0f, 9.0f};
    bool record_playtest{};
    bool replay_compare{};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::uint32_t frame_cap{60};
    std::uint32_t render_scale_percent{100};
    std::uint32_t camera_zoom_percent{100};
    std::uint32_t shadow_resolution{1024};
    std::uint32_t particle_percentage{100};
    std::uint64_t seed{1};
    Tick maximum_ticks{180};
    BarrierMode barrier_mode{BarrierMode::Automatic};
    std::filesystem::path artifact_directory{"Artifacts/runtime"};
    std::filesystem::path heartbeat_path;
    std::filesystem::path playtest_output_directory;
    std::filesystem::path replay_directory;
    std::vector<ApplicationTimelineAction> timeline_actions;
};

struct ApplicationResult
{
    Result result{Result::Success()};
    Tick final_tick{};
    GameplayChecksum checksum{};
    std::uint64_t rendered_frames{};
    std::filesystem::path playtest_directory;
};

[[nodiscard]] ApplicationResult RunApplication(const ApplicationConfig &config);

} // namespace hs
