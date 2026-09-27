#pragma once
#include <hs/renderer/vfx_frame_input.hpp>
namespace hs
{
struct VfxLightInput
{
    std::uint64_t stable_id{};
    Float3 position{};
    float radius{};
    Float3 linear_rgb{};
    float intensity{};
    std::uint32_t importance{};
    VfxQuality quality{VfxQuality::High};
};
}
