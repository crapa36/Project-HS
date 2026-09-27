#pragma once
#include <hs/renderer/vfx_frame_input.hpp>
#include <vector>
namespace hs
{
enum class VfxDistortionShape : std::uint32_t { RadialRing, HeatShock, ShortShock, HeadEnvelope, TightDisc, ConeSector, GappedAnnulus };
struct VfxDistortionInput
{
    std::uint64_t stable_id{};
    Float3 position{};
    float radius{};
    Float3 direction{0,0,1};
    float strength{}, normalized_age{}, alpha{};
    VfxDistortionShape shape{};
    VfxQuality quality{VfxQuality::High};
    std::uint32_t importance{}, stable_seed{};
    float inner_radius{}, half_angle_degrees{}, gap_half_width_degrees{};
    std::vector<float> gap_angles_degrees;
};
}
