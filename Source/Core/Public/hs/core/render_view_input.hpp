#pragma once

#include <hs/core/render_snapshot.hpp>

namespace hs
{
struct ViewId
{
    std::uint32_t value{};
    bool operator==(const ViewId &) const = default;
};
inline constexpr ViewId kMainRenderViewId{1};

struct ViewExtent
{
    std::uint32_t width{}, height{};
    bool operator==(const ViewExtent &) const = default;
};
struct RenderWorldPosition
{
    double x{}, y{}, z{};
    bool operator==(const RenderWorldPosition &) const = default;
};
enum class RenderViewRole : std::uint8_t { Main, Preview, Capture };

// Position and target are global doubles. Snapshot, static scene, VFX, lights,
// probes and ray geometry use the common scene-relative coordinate convention.
// Conversion to float happens only after subtracting scene_origin in double.
struct RenderCameraInput
{
    RenderWorldPosition position, target;
    Float3 up{0, 1, 0};
    float vertical_fov_degrees{45};
};
struct RenderViewInput
{
    ViewId id;
    RenderCameraInput camera;
    float near_plane{0.1f}, far_plane{500};
    RenderWorldPosition scene_origin, view_origin;
    ViewExtent output_extent, render_capacity, render_active;
    bool camera_cut{};
    RenderViewRole role{RenderViewRole::Main};
};
} // namespace hs
