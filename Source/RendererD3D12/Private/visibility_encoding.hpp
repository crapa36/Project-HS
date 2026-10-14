#pragma once

namespace hs
{

enum class OcclusionMode
{
    Disabled,
    Enabled,
};

// static_visibility.hlsl consumes the magnitude as grass deformation padding
// and the sign as the existing HZB opt-out. Preserve the root-constant ABI,
// including signed zero; callers supply the nonnegative deformation radius.
[[nodiscard]] constexpr float EncodeGrassDeformationRadius(
    float radius, OcclusionMode mode) noexcept
{
    return mode == OcclusionMode::Enabled ? radius : -radius;
}

inline constexpr unsigned kEnvironmentMeshFlag = 0x10000u;

} // namespace hs
