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
}

DynamicResolutionSelection DynamicResolutionController::Select(
    const ViewState &view, const DynamicResolutionConfig &config,
    DynamicResolutionSample sample) noexcept
{
    DynamicResolutionSelection selection{view.render_active, false, false, filtered_ms_};
    if (!ValidView(view) || !ValidConfig(config)) return selection;
    if (resize_epoch_ != view.resize_epoch)
    {
        Reset();
        resize_epoch_ = view.resize_epoch;
        selection.filtered_critical_path_ms = 0.0;
    }
    if (!config.enabled)
    {
        has_sample_ = false;
        filtered_ms_ = 0.0;
        selection.filtered_critical_path_ms = 0.0;
        return selection;
    }
    if (sample.metric_kind != DynamicResolutionMetricKind::ReferenceGraphSpan ||
        !sample.completed || sample.generated || !sample.gpu_limited ||
        !std::isfinite(sample.render_critical_path_ms) || sample.render_critical_path_ms <= 0.0)
        return selection;

    filtered_ms_ = has_sample_
        ? std::lerp(filtered_ms_, sample.render_critical_path_ms, config.smoothing)
        : sample.render_critical_path_ms;
    has_sample_ = true;
    selection.sample_accepted = true;
    selection.filtered_critical_path_ms = filtered_ms_;

    const double target_ms = 1000.0 / config.target_render_fps;
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
        std::abs(filtered_ms_ - target_ms) <= target_ms * config.hysteresis_fraction)
        return selection;

    // Pixel work grows approximately with scale squared. Bound each accepted
    // sample's change before choosing the quantized extent inside fixed capacity.
    const double rate = config.max_scale_change_percent * 0.01;
    const double bounded_current = std::clamp(current_scale, minimum_scale, maximum_scale);
    const double lower_scale = std::max(minimum_scale, bounded_current - rate);
    const double upper_scale = std::min(maximum_scale, bounded_current + rate);
    const double desired_scale = std::clamp(current_scale * std::sqrt(target_ms / filtered_ms_),
                                            lower_scale, upper_scale);
    selection.active_extent = {
        SelectDimension(view.output_extent.width, view.render_active.width, desired_scale,
            minimum.width, maximum.width, rate, config.extent_quantum),
        SelectDimension(view.output_extent.height, view.render_active.height, desired_scale,
            minimum.height, maximum.height, rate, config.extent_quantum)};
    selection.changed = selection.active_extent != view.render_active;
    return selection;
}
} // namespace hs
