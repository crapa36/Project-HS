#pragma once

#include <hs/core/types.hpp>

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
enum class RenderViewRole : std::uint8_t { Main, Preview, Capture };

// Camera positions use arena-local metre coordinates.
struct RenderCameraInput
{
    Float3 position{}, target{};
    Float3 up{0, 1, 0};
    float vertical_fov_degrees{45};
};
struct RenderViewConfiguration
{
    ViewId id;
    RenderViewRole role{RenderViewRole::Main};
    ViewExtent output_extent, render_capacity;
};

struct RenderViewSample
{
    ViewId id;
    RenderCameraInput camera;
    float near_plane{0.1f}, far_plane{500};
    ViewExtent render_active;
    bool camera_cut{};
};

// Resolved CPU projection/output facts; frame producers submit only the sample.
struct RenderViewInput : RenderViewSample
{
    ViewExtent output_extent, render_capacity;
    RenderViewRole role{RenderViewRole::Main};

    [[nodiscard]] RenderViewConfiguration Configuration() const noexcept
    { return {id, role, output_extent, render_capacity}; }
    [[nodiscard]] const RenderViewSample &Sample() const noexcept { return *this; }
};
} // namespace hs
