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
    bool vfx_showcase{};
    std::uint32_t skill_vfx_capture{0xFFFFFFFFu};
    std::uint32_t skill_vfx_upgrade_mask{0xFFu};
    bool record_playtest{};
    bool replay_compare{};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::uint32_t frame_cap{60};
    std::uint32_t render_scale_percent{100};
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
