#include "view_state.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hs
{
namespace
{
bool ValidExtent(ViewExtent extent) noexcept
{
    return extent.width != 0 && extent.height != 0;
}

bool ValidView(const ViewState &view) noexcept
{
    return ValidExtent(view.output_extent) && ValidExtent(view.render_capacity) &&
           ValidExtent(view.render_active) &&
           view.render_active.width <= view.render_capacity.width &&
           view.render_active.height <= view.render_capacity.height;
}

bool ValidConfig(const DynamicResolutionConfig &config) noexcept
{
    return std::isfinite(config.target_render_fps) && config.target_render_fps >= 1.0 &&
           std::isfinite(config.min_scale_percent) && config.min_scale_percent > 0.0 &&
           std::isfinite(config.max_scale_percent) && config.max_scale_percent >= config.min_scale_percent &&
           config.extent_quantum != 0 &&
           std::isfinite(config.hysteresis_fraction) && config.hysteresis_fraction >= 0.0 && config.hysteresis_fraction < 1.0 &&
           std::isfinite(config.smoothing) && config.smoothing > 0.0 && config.smoothing <= 1.0 &&
           std::isfinite(config.max_scale_change_percent) && config.max_scale_change_percent > 0.0;
}

// Alignment is a preference, never a reason to keep an out-of-domain extent.
std::uint32_t SelectDimension(std::uint32_t output, std::uint32_t current,
                              double desired_scale, double global_minimum, double global_maximum,
                              double rate, std::uint32_t quantum) noexcept
{
    // Configuration changes first establish a feasible interval. Ordinary DRS
    // samples additionally remain within the configured change from this extent.
    const bool current_in_range = current >= global_minimum && current <= global_maximum;
    const double minimum = current_in_range
        ? std::max(global_minimum, std::ceil(current - output * rate)) : global_minimum;
    const double maximum = current_in_range
        ? std::min(global_maximum, std::floor(current + output * rate)) : global_maximum;
    const double aligned_minimum = std::ceil(minimum / quantum) * quantum;
    const double aligned_maximum = std::floor(maximum / quantum) * quantum;
    if (aligned_minimum <= aligned_maximum)
    {
        const double desired = std::round(output * desired_scale / quantum) * quantum;
        return static_cast<std::uint32_t>(std::clamp(desired, aligned_minimum, aligned_maximum));
    }
    return static_cast<std::uint32_t>(std::clamp(std::round(output * desired_scale), minimum, maximum));
}
} // namespace

bool ResolveDynamicResolutionExtent(ViewExtent output, double percent, ViewExtent &extent) noexcept
{
    if (!ValidExtent(output) || !std::isfinite(percent) || percent <= 0.0) return false;
    const auto scaled = [percent](std::uint32_t pixels) {
        return std::max(1.0, std::ceil(static_cast<double>(pixels) * percent / 100.0));
    };
    const auto width = scaled(output.width), height = scaled(output.height);
    if (!std::isfinite(width) || !std::isfinite(height) ||
        width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max()) return false;
    extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    return true;
}

bool QualifiedGpuClockInterval(const GpuClockCalibrationEvidence &clock,
                               std::uint64_t first_tick,
                               std::uint64_t last_tick) noexcept
{
    if (!clock.gpu_frequency || !clock.cpu_frequency ||
        clock.gpu_before >= first_tick || first_tick > last_tick ||
        last_tick >= clock.gpu_after || clock.cpu_before >= clock.cpu_after)
        return false;
    const long double gpu_delta = static_cast<long double>(clock.gpu_after - clock.gpu_before);
    const long double cpu_delta = static_cast<long double>(clock.cpu_after - clock.cpu_before);
    const long double expected_cpu_delta = gpu_delta * clock.cpu_frequency / clock.gpu_frequency;
    const long double tolerance = std::max(cpu_delta * 0.005L,
                                           static_cast<long double>(clock.cpu_frequency) * 0.00005L);
    return std::isfinite(expected_cpu_delta) &&
           std::abs(expected_cpu_delta - cpu_delta) <= tolerance;
}

DynamicResolutionSample QualifySourceTiming(
    const SourceTimingEndpoint &current, const SourceTimingEndpoint *previous) noexcept
{
    DynamicResolutionSample sample;
    sample.metric_kind = DynamicResolutionMetricKind::SourceOutputReady;
    sample.completed = current.completed && current.main_view;
    sample.generated = current.generated;
    sample.view_id = current.view_id;
    sample.source_frame_id = current.source_frame_id;
    sample.metric_generation = current.metric_generation;
    sample.quality_generation = current.quality_generation;
    sample.provider_generation = current.provider_generation;
    sample.render_active = current.render_active;
    sample.calibration_valid = current.clock_anchors_valid && current.ready_valid &&
        QualifiedGpuClockInterval(current.clock, current.ready_tick, current.ready_tick);
    sample.calibration_drift = current.clock_anchors_valid && !sample.calibration_valid;
    sample.gpu_backlog_proven = current.gpu_backlog_proven;
    sample.cadence_gap = current.cpu_supply_gap || current.pacing_gap;
    sample.closure_span_gap = current.closure_span_gap;
    sample.span_valid = sample.calibration_valid && current.closure_clock_valid &&
        current.closure_gap_checked &&
        current.closure_span_valid && std::isfinite(current.closure_span_ms) &&
        current.closure_span_ms > 0.0;
    if (sample.span_valid) sample.output_dependency_span_ms = current.closure_span_ms;

    if (!sample.completed || sample.generated || !sample.calibration_valid || !previous ||
        !previous->completed || !previous->main_view || previous->generated ||
        !previous->ready_valid || !previous->clock_anchors_valid ||
        !QualifiedGpuClockInterval(previous->clock, previous->ready_tick, previous->ready_tick) ||
        previous->view_id != current.view_id ||
        previous->metric_generation != current.metric_generation ||
        previous->quality_generation != current.quality_generation ||
        previous->provider_generation != current.provider_generation ||
        previous->render_active != current.render_active ||
        previous->source_frame_sequence == std::numeric_limits<std::uint64_t>::max() ||
        previous->source_frame_sequence + 1 != current.source_frame_sequence ||
        previous->source_frame_id >= current.source_frame_id ||
        !current.ready_frequency || previous->ready_frequency != current.ready_frequency ||
        !current.qpc_frequency || previous->qpc_frequency != current.qpc_frequency ||
        previous->ready_qpc >= current.ready_qpc)
        return sample;

    sample.source_ready_cadence_ms = static_cast<double>(
        static_cast<long double>(current.ready_qpc - previous->ready_qpc) *
        1000.0L / current.qpc_frequency);
    sample.cadence_valid = current.cadence_exclusions_valid &&
        std::isfinite(sample.source_ready_cadence_ms) &&
        sample.source_ready_cadence_ms > 0.0;
    return sample;
}

bool UpdateViewState(ViewState &state, ViewExtent output,
                     ViewExtent capacity, ViewExtent active) noexcept
{
    if (!ValidExtent(output) || !ValidExtent(capacity) || !ValidExtent(active) ||
        active.width > capacity.width || active.height > capacity.height)
        return false;
    const bool resize = state.resize_epoch == 0 || state.output_extent != output ||
                        state.render_capacity != capacity;
    if (resize && state.resize_epoch == std::numeric_limits<std::uint64_t>::max())
        return false;
    state.output_extent = output;
    state.render_capacity = capacity;
    state.render_active = active;
    if (resize) ++state.resize_epoch;
    return true;
}

void DynamicResolutionController::Reset() noexcept
{
    filtered_ms_ = 0.0;
    resize_epoch_ = 0;
    has_sample_ = false;
    consumed_frames_.clear();
}

DynamicResolutionSelection DynamicResolutionController::Select(
    const ViewState &view, const DynamicResolutionConfig &config,
    DynamicResolutionSample sample)
{
    DynamicResolutionSelection selection{view.render_active, false, false, filtered_ms_};
    if (!ValidView(view) || !ValidConfig(config)) return selection;
    const bool source_ready = sample.metric_kind == DynamicResolutionMetricKind::SourceOutputReady;
    const bool output_dependency_interval =
        sample.metric_kind == DynamicResolutionMetricKind::OutputDependencyInterval;
    if (!source_ready && resize_epoch_ != view.resize_epoch)
    {
        filtered_ms_ = 0.0;
        has_sample_ = false;
        resize_epoch_ = view.resize_epoch;
        selection.filtered_critical_path_ms = 0.0;
    }
    if (!config.enabled)
    {
        has_sample_ = false;
        filtered_ms_ = 0.0;
        consumed_frames_.clear();
        selection.filtered_critical_path_ms = 0.0;
        return selection;
    }
    if (!sample.completed || sample.generated)
        return selection;

    ConsumedFrame *consumer = nullptr;
    if (source_ready)
    {
        if (sample.view_id != view.view_id || !sample.source_frame_id ||
            sample.metric_generation != view.metric_generation ||
            sample.quality_generation != view.quality_generation ||
            sample.provider_generation != view.provider_generation ||
            sample.render_active != view.render_active)
            return selection;
        auto &consumed = consumed_frames_[sample.view_id];
        if (consumed.metric_generation != sample.metric_generation ||
            consumed.resize_epoch != view.resize_epoch ||
            consumed.quality_generation != sample.quality_generation ||
            consumed.provider_generation != sample.provider_generation ||
            consumed.render_active != sample.render_active)
            consumed = {sample.metric_generation, view.resize_epoch, sample.quality_generation,
                        sample.provider_generation, 0, sample.render_active};
        if (sample.source_frame_id <= consumed.frame_id) return selection;
        consumed.frame_id = sample.source_frame_id;
        consumer = &consumed;
        selection.filtered_critical_path_ms = consumed.filtered_ms;
        if (!sample.calibration_valid || sample.calibration_drift) return selection;
    }
    else if ((!output_dependency_interval &&
              sample.metric_kind != DynamicResolutionMetricKind::ReferenceGraphSpan) ||
             !sample.gpu_limited || !std::isfinite(sample.render_critical_path_ms) ||
             sample.render_critical_path_ms <= 0.0)
        return selection;

    const double target_ms = 1000.0 / config.target_render_fps;
    const bool cadence = source_ready && sample.cadence_valid && sample.gpu_backlog_proven &&
        !sample.cadence_gap && std::isfinite(sample.source_ready_cadence_ms) &&
        sample.source_ready_cadence_ms > 0.0;
    const bool cadence_over_budget = cadence &&
        sample.source_ready_cadence_ms > target_ms * (1.0 + config.hysteresis_fraction);
    const bool recovery = source_ready && !cadence_over_budget && sample.span_valid && !sample.closure_span_gap &&
        std::isfinite(sample.output_dependency_span_ms) && sample.output_dependency_span_ms > 0.0 &&
        sample.output_dependency_span_ms < target_ms *
            (1.0 - std::max(0.1, config.hysteresis_fraction * 2.0));
    if (source_ready && !cadence_over_budget && !recovery) return selection;

    if (!recovery)
    {
        const double measured_ms = source_ready ? sample.source_ready_cadence_ms : sample.render_critical_path_ms;
        auto &filtered = source_ready ? consumer->filtered_ms : filtered_ms_;
        auto &has_sample = source_ready ? consumer->has_sample : has_sample_;
        filtered = has_sample ? std::lerp(filtered, measured_ms, config.smoothing) : measured_ms;
        has_sample = true;
    }
    else
        consumer->has_sample = false; // A recovery span must not seed the cadence filter.
    const double filtered = source_ready ? consumer->filtered_ms : filtered_ms_;
    selection.sample_accepted = true;
    selection.filtered_critical_path_ms = filtered;
    selection.metric_kind = source_ready
        ? (recovery ? DynamicResolutionMetricKind::DisplayDependencySpan
                    : DynamicResolutionMetricKind::SourceRenderReadyCadence)
        : output_dependency_interval ? DynamicResolutionMetricKind::OutputDependencyInterval
                                     : DynamicResolutionMetricKind::ReferenceGraphSpan;

    ViewExtent minimum, maximum;
    if (!ResolveDynamicResolutionExtent(view.output_extent, config.min_scale_percent, minimum) ||
        !ResolveDynamicResolutionExtent(view.output_extent, config.max_scale_percent, maximum)) return selection;
    maximum.width = std::min(maximum.width, view.render_capacity.width);
    maximum.height = std::min(maximum.height, view.render_capacity.height);
    if (minimum.width > maximum.width || minimum.height > maximum.height) return selection;
    const double minimum_scale = config.min_scale_percent / 100.0;
    const double maximum_scale = std::min({config.max_scale_percent / 100.0,
        static_cast<double>(view.render_capacity.width) / view.output_extent.width,
        static_cast<double>(view.render_capacity.height) / view.output_extent.height});
    if (minimum_scale > maximum_scale) return selection;
    const double current_scale = std::min(
        static_cast<double>(view.render_active.width) / view.output_extent.width,
        static_cast<double>(view.render_active.height) / view.output_extent.height);
    if (view.render_active.width >= minimum.width && view.render_active.width <= maximum.width &&
        view.render_active.height >= minimum.height && view.render_active.height <= maximum.height &&
        !recovery && std::abs(filtered - target_ms) <= target_ms * config.hysteresis_fraction)
        return selection;

    // Pixel work grows approximately with scale squared. Bound each accepted
    // sample's change before choosing the quantized extent inside fixed capacity.
    const double rate = config.max_scale_change_percent * 0.01;
    const double bounded_current = std::clamp(current_scale, minimum_scale, maximum_scale);
    const double lower_scale = std::max(minimum_scale, bounded_current - rate);
    const double upper_scale = std::min(maximum_scale, bounded_current + rate);
    const double desired_scale = recovery ? upper_scale :
        std::clamp(current_scale * std::sqrt(target_ms / filtered), lower_scale,
                   source_ready ? std::max(lower_scale, std::min(upper_scale, current_scale)) : upper_scale);
    selection.active_extent = {
        SelectDimension(view.output_extent.width, view.render_active.width, desired_scale,
            minimum.width, maximum.width, rate, config.extent_quantum),
        SelectDimension(view.output_extent.height, view.render_active.height, desired_scale,
            minimum.height, maximum.height, rate, config.extent_quantum)};
    selection.changed = selection.active_extent != view.render_active;
    return selection;
}
} // namespace hs
