#pragma once

#include <hs/core/render_view_input.hpp>
#include <cstdint>

namespace hs
{
// Allocation capacity is independent of the active render rectangle. A change
// within that capacity does not replace resources or advance the resize epoch.
struct ViewState
{
    ViewExtent output_extent;
    ViewExtent render_capacity;
    ViewExtent render_active;
    std::uint64_t resize_epoch{};
};

[[nodiscard]] bool UpdateViewState(ViewState &state, ViewExtent output,
                                   ViewExtent capacity, ViewExtent active) noexcept;

// Percent bounds include the partially covered edge pixel (ceil, minimum 1).
// Preparation, provider domains and frame selection must use the same rule.
[[nodiscard]] bool ResolveDynamicResolutionExtent(ViewExtent output, double percent,
                                                  ViewExtent &extent) noexcept;

struct DynamicResolutionConfig
{
    bool enabled{};
    double target_render_fps{60.0};
    double min_scale_percent{50.0};
    double max_scale_percent{100.0};
    std::uint32_t extent_quantum{8}; // Preferred alignment; hard bounds take priority.
    double hysteresis_fraction{0.05};
    double smoothing{0.2};
    double max_scale_change_percent{2.0};
};

struct DynamicResolutionSample
{
    bool completed{};
    bool generated{};
    bool gpu_limited{};
    double render_critical_path_ms{};
};

struct DynamicResolutionSelection
{
    ViewExtent active_extent;
    bool changed{};
    bool sample_accepted{};
    double filtered_critical_path_ms{};
};

class DynamicResolutionController
{
  public:
    // The caller supplies a completed, ungenerated render critical-path sample,
    // once per newly completed render frame, excluding present/limiter/idle waits.
    // Selection never allocates resources.
    [[nodiscard]] DynamicResolutionSelection Select(
        const ViewState &view, const DynamicResolutionConfig &config,
        DynamicResolutionSample sample) noexcept;
    void Reset() noexcept;

  private:
    double filtered_ms_{};
    std::uint64_t resize_epoch_{};
    bool has_sample_{};
};
} // namespace hs
