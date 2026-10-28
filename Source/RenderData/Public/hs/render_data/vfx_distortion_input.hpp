#pragma once
#include <hs/render_data/vfx_frame_input.hpp>
#include <cstddef>
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
    VfxUniformRingGaps uniform_gaps{};

    [[nodiscard]] std::size_t GapCount() const noexcept
    { return uniform_gaps.Active() ? uniform_gaps.count : gap_angles_degrees.size(); }
    [[nodiscard]] float GapAngleDegrees(std::size_t index) const noexcept
    { return uniform_gaps.Active() ? uniform_gaps.AngleDegrees(static_cast<std::uint32_t>(index)) : gap_angles_degrees[index]; }
    [[nodiscard]] bool GapStorageConsistent() const noexcept
    { return uniform_gaps.Valid() && (!uniform_gaps.Active() || gap_angles_degrees.empty()); }
};
}
