#pragma once
#include <hs/render_data/vfx_frame_input.hpp>
namespace hs
{
struct VfxLightInput
{
    std::uint64_t stable_id{};
    Float3 position{};
    float radius{};
    Float3 linear_rgb{};
    float intensity{}; // W/sr, using the legacy local-light attenuation curve.
    std::uint32_t importance{};
    VfxQuality quality{VfxQuality::High};
};
}
