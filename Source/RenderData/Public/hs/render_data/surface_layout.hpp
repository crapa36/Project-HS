#pragma once

#include <cstdint>

namespace hs::render_data
{
// GPU SurfaceLayout v1, paired with Content/Shaders/surface_layout.hlsli.
// Logical material identity is distinct from a texture-array/atlas entry.
struct SurfaceLayout
{
    static constexpr std::uint32_t version = 1;
    static constexpr std::uint32_t render_target_count = 6;
    static constexpr std::uint32_t invalid_material = 0;
    static constexpr std::uint32_t valid_flag = 1u;
    static constexpr std::uint32_t model_shift = 1u;
    static constexpr std::uint32_t model_mask = 0xfeu;
    static constexpr std::uint32_t outline_shift = 16u;
};
} // namespace hs::render_data
