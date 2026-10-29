#pragma once

#include <hs/render_data/render_view_input.hpp>
#include <cstdint>
#include <unordered_map>

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
    // The render caller supplies these identities for completed timing samples.
    std::uint32_t view_id{};
    std::uint64_t metric_generation{}, quality_generation{}, provider_generation{};
};

[[nodiscard]] bool UpdateViewState(ViewState &state, ViewExtent output,
                                   ViewExtent capacity, ViewExtent active) noexcept;

// Percent bounds include the partially covered edge pixel (ceil, minimum 1).
// Preparation, provider domains and frame selection must use the same rule.
[[nodiscard]] bool ResolveDynamicResolutionExtent(ViewExtent output, double percent,
                                                  ViewExtent &extent) noexcept;

struct GpuClockCalibrationEvidence
{
    std::uint64_t gpu_before{}, cpu_before{}, gpu_after{}, cpu_after{};
    std::uint64_t gpu_frequency{}, cpu_frequency{};
};

// Calibration anchors must bracket the measured GPU interval, with monotonic
// clocks and bounded rate drift. The caller must obtain both native anchors.
[[nodiscard]] bool QualifiedGpuClockInterval(const GpuClockCalibrationEvidence &clock,
                                             std::uint64_t first_tick,
                                             std::uint64_t last_tick) noexcept;

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

enum class DynamicResolutionMetricKind : std::uint8_t
{
    Unavailable,
    ReferenceGraphSpan,
    SourceOutputReady,
    SourceRenderReadyCadence,
    DisplayDependencySpan,
    OutputDependencyInterval,
};

struct DynamicResolutionSample
{
    bool completed{};
    bool generated{};
    bool gpu_limited{};
    double render_critical_path_ms{};
    DynamicResolutionMetricKind metric_kind{DynamicResolutionMetricKind::Unavailable};
    std::uint32_t view_id{};
    std::uint64_t source_frame_id{}, metric_generation{}, quality_generation{}, provider_generation{};
    ViewExtent render_active;
    bool calibration_valid{}, calibration_drift{};
    bool cadence_valid{}, span_valid{}, gpu_backlog_proven{};
    bool cadence_gap{}, closure_span_gap{};
    double source_ready_cadence_ms{}, output_dependency_span_ms{};
};

struct SourceTimingEndpoint
{
    std::uint32_t view_id{};
    std::uint64_t source_frame_id{}, source_frame_sequence{};
    std::uint64_t metric_generation{}, quality_generation{}, provider_generation{};
    ViewExtent render_active;
    bool completed{}, main_view{}, generated{};
    std::uint64_t ready_tick{}, ready_frequency{}, ready_qpc{}, qpc_frequency{};
    bool ready_valid{}, clock_anchors_valid{};
    GpuClockCalibrationEvidence clock;
    // These are native queue/submit and complete closure facts, not timing heuristics.
    bool gpu_backlog_proven{}, cadence_exclusions_valid{}, cpu_supply_gap{}, pacing_gap{};
    bool closure_span_valid{}, closure_clock_valid{}, closure_gap_checked{}, closure_span_gap{};
    double closure_span_ms{};
};

[[nodiscard]] DynamicResolutionSample QualifySourceTiming(
    const SourceTimingEndpoint &current, const SourceTimingEndpoint *previous) noexcept;

struct DynamicResolutionSelection
{
    ViewExtent active_extent;
    bool changed{};
    bool sample_accepted{};
    double filtered_critical_path_ms{};
    DynamicResolutionMetricKind metric_kind{DynamicResolutionMetricKind::Unavailable};
};

class DynamicResolutionController
{
  public:
    // SourceOutputReady samples require proven cadence or independently qualified
    // closure span. OutputDependencyInterval is a same-Direct-queue begin/end
    // interval ending at the rendered output; ReferenceGraphSpan is legacy.
    [[nodiscard]] DynamicResolutionSelection Select(
        const ViewState &view, const DynamicResolutionConfig &config,
        DynamicResolutionSample sample);
    void Reset() noexcept;

  private:
    double filtered_ms_{};
    std::uint64_t resize_epoch_{};
    bool has_sample_{};
    struct ConsumedFrame
    {
        std::uint64_t metric_generation{}, resize_epoch{}, quality_generation{}, provider_generation{}, frame_id{};
        ViewExtent render_active;
        double filtered_ms{};
        bool has_sample{};
    };
    std::unordered_map<std::uint32_t, ConsumedFrame> consumed_frames_;
};
} // namespace hs
