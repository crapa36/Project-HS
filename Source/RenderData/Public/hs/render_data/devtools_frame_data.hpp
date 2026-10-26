#pragma once
#include <cstdint>

namespace hs
{
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
};
} // namespace hs
