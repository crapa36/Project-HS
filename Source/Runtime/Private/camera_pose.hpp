#pragma once

#include <algorithm>

namespace hs
{
struct RuntimeCameraPose
{
    float pitch_degrees{};
    float distance_m{};
    float target_height_m{};
};

inline RuntimeCameraPose ComputeRuntimeCameraPose(float base_distance_m,
                                                 float base_pitch_degrees,
                                                 float zoom_percent) noexcept
{
    const auto progress = std::clamp((80.0f - zoom_percent) / 65.0f, 0.0f, 1.0f);
    const auto blend = progress * progress * (3.0f - 2.0f * progress);
    return {base_pitch_degrees + (18.0f - base_pitch_degrees) * blend,
            base_distance_m * zoom_percent / 100.0f, blend};
}
} // namespace hs
