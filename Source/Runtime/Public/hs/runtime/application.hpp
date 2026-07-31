#pragma once

#include <hs/core/result.hpp>
#include <hs/core/types.hpp>

#include <filesystem>

namespace hs
{

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
    std::uint32_t width{1280};
    std::uint32_t height{720};
    std::uint32_t frame_cap{60};
    std::uint32_t render_scale_percent{100};
    std::uint32_t shadow_resolution{1024};
    std::uint32_t particle_percentage{100};
    Tick maximum_ticks{180};
    BarrierMode barrier_mode{BarrierMode::Automatic};
    std::filesystem::path artifact_directory{"Artifacts/stage1"};
};

struct ApplicationResult
{
    Result result{Result::Success()};
    Tick final_tick{};
    GameplayChecksum checksum{};
    std::uint64_t rendered_frames{};
};

[[nodiscard]] ApplicationResult RunApplication(const ApplicationConfig &config);

} // namespace hs
