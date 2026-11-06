#pragma once
#include <cstdint>

namespace hs
{
struct PreviewEditValues
{
    bool enabled{};
    bool pose_override{true};
    int base_clip{}, secondary_clip{1}, upper_clip{3};
    float base_time{.37f}, secondary_time{.61f}, upper_time{.5f};
    float secondary_weight{.5f}, upper_weight{.5f};
    float distance{4.0f}, yaw{180.0f}, pitch{10.0f};
};
struct DevToolsFrameData
{
    std::uint32_t worker_count{};
    std::uint32_t input_queue_depth{};
    std::uint32_t presentation_queue_depth{};
    std::uint32_t particle_queue_depth{};
    std::uint32_t resize_queue_depth{};
    std::uint32_t debug_queue_depth{};
    std::uint64_t dropped_input{};
    std::uint64_t dropped_presentation{};
    std::uint64_t dropped_particles{};
    PreviewEditValues preview;
};
} // namespace hs
