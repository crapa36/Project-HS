#pragma once
#include <hs/render_data/vfx_frame_input.hpp>
#include <cstddef>
#include <limits>
#include <span>
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
    std::uint32_t frame_gap_first{}, frame_gap_count{};

    [[nodiscard]] std::size_t GapCount() const noexcept
    { return uniform_gaps.Active() ? uniform_gaps.count : frame_gap_count ? frame_gap_count : gap_angles_degrees.size(); }
    [[nodiscard]] float GapAngleDegrees(std::size_t index,
                                        std::span<const float> frame_gaps = {}) const noexcept
    {
        if (uniform_gaps.Active()) return uniform_gaps.AngleDegrees(static_cast<std::uint32_t>(index));
        if (frame_gap_count) return frame_gaps[frame_gap_first + index];
        return gap_angles_degrees[index];
    }
    [[nodiscard]] bool GapStorageConsistent(std::size_t frame_gap_size = 0) const noexcept
    {
        if (!uniform_gaps.Valid()) return false;
        if (uniform_gaps.Active())
            return gap_angles_degrees.empty() && frame_gap_first == 0 && frame_gap_count == 0;
        if (frame_gap_count != 0)
            return gap_angles_degrees.empty() &&
                frame_gap_count <= std::numeric_limits<std::uint32_t>::max() - frame_gap_first &&
                static_cast<std::size_t>(frame_gap_first) + frame_gap_count <= frame_gap_size;
        return frame_gap_first == 0;
    }
};
}
