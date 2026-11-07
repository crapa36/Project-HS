#pragma once
#include "hs/core/types.hpp"
#include <cstdint>
#include <span>
#include <string_view>
#include <optional>

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
struct PreviewStationLabel
{
    Float3 position;
    std::string_view text;
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
    std::span<const PreviewStationLabel> station_labels;
    bool test_room{};
    // Deterministic offline capture controls; ordinary packages leave these unset.
    std::optional<float> delta_seconds;
    bool wait_for_streaming{};
};
} // namespace hs
