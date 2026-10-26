#include "renderer_reconstruction.hpp"
#include "view_state.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
using namespace hs;

void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

constexpr DynamicResolutionSample slow_sample{true, false, true, 30.0, DynamicResolutionMetricKind::ReferenceGraphSpan};

void TestPreparedIntegerDomains()
{
    RendererConfig options;
    options.dynamic_resolution = true;
    options.dynamic_min_percent = options.dynamic_max_percent = 50.0f;
    for (const auto output : {ViewExtent{1920, 1080}, ViewExtent{1921, 1081},
                              ViewExtent{13, 7}, ViewExtent{3, 1}, ViewExtent{1, 1}})
    {
        ReconstructionPlan plan;
        ViewExtent capacity, expected;
        Check(ResolveDynamicResolutionExtent(output, 50.0, expected), "valid integer DRS bound");
        const auto prepared = ResolveNativeReconstruction(options, output, plan, capacity);
        Check(prepared.Succeeded(), prepared.Message());
        Check(capacity == expected && plan.render_active == expected &&
                  plan.input_minimum == expected && plan.input_maximum == expected,
              "fixed odd/tiny DRS preparation uses one inclusive integer domain");
        ViewState view;
        Check(UpdateViewState(view, output, capacity, plan.render_active), "prepared view is valid");
        DynamicResolutionController controller;
        const DynamicResolutionConfig config{true, 60, 50, 50};
        const auto selected = controller.Select(view, config, slow_sample);
        Check(selected.sample_accepted && !selected.changed && selected.active_extent == expected,
              "fixed odd/tiny extent remains valid without an aligned candidate");
    }
    ViewExtent extent{7, 9};
    const auto unchanged = extent;
    Check(!ResolveDynamicResolutionExtent({0, 1080}, 50, extent) && extent == unchanged,
          "zero output does not publish a partial extent");
    for (const auto percent : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()})
        Check(!ResolveDynamicResolutionExtent({1920, 1080}, percent, extent) && extent == unchanged,
              "invalid percentage does not publish a partial extent");
    Check(!ResolveDynamicResolutionExtent({UINT32_MAX, UINT32_MAX}, 200, extent) && extent == unchanged,
          "integer DRS capacity overflow is rejected");
    options.dynamic_min_percent = 75;
    options.dynamic_max_percent = 50;
    ReconstructionPlan plan;
    ViewExtent capacity;
    Check(!ResolveNativeReconstruction(options, {1920, 1080}, plan, capacity),
          "reversed DRS range is rejected before clamping");
    options.dynamic_min_percent = std::numeric_limits<float>::quiet_NaN();
    Check(!ResolveNativeReconstruction(options, {1920, 1080}, plan, capacity),
          "nonfinite DRS preparation is rejected");
}

void TestHardBoundsBeforeAlignmentAndRate()
{
    for (const auto active : {ViewExtent{1280, 720}, ViewExtent{960, 720}, ViewExtent{800, 400}})
    {
        ViewState view;
        Check(UpdateViewState(view, {1920, 1080}, {1920, 1080}, active), "out-of-domain view fixture");
        DynamicResolutionController controller;
        const DynamicResolutionConfig config{true, 60, 50, 50};
        // Target-rate timing must not let the aggregate width/height scale hide
        // an invalid individual dimension behind the hysteresis early return.
        const auto selected = controller.Select(view, config, {true, false, true, 1000.0 / 60.0, DynamicResolutionMetricKind::ReferenceGraphSpan});
        Check(selected.sample_accepted && selected.changed && selected.active_extent == ViewExtent{960, 540},
              "hard bounds beat alignment, per-sample rate and hysteresis");
        const auto epoch = view.resize_epoch;
        Check(UpdateViewState(view, view.output_extent, view.render_capacity, selected.active_extent) &&
                  view.resize_epoch == epoch,
              "DRS selection does not change resource capacity or resize epoch");
        for (int sample = 0; sample < 3; ++sample)
            Check(controller.Select(view, config, slow_sample).active_extent == ViewExtent{960, 540},
                  "repeated samples preserve the repaired fixed bound");
    }

    RendererConfig options;
    options.dynamic_resolution = true;
    options.dynamic_min_percent = 50;
    options.dynamic_max_percent = 50.1f;
    ReconstructionPlan plan;
    ViewExtent capacity;
    Check(ResolveNativeReconstruction(options, {1920, 1080}, plan, capacity).Succeeded(),
          "fractional narrow DRS preparation");
    ViewState narrow;
    Check(UpdateViewState(narrow, {1920, 1080}, capacity, plan.render_active), "fractional narrow view");
    DynamicResolutionController controller;
    const DynamicResolutionConfig config{true, 60, options.dynamic_min_percent, options.dynamic_max_percent};
    const auto selected = controller.Select(narrow, config, slow_sample);
    Check(selected.active_extent.width >= plan.input_minimum.width && selected.active_extent.width <= capacity.width &&
              selected.active_extent.height >= plan.input_minimum.height && selected.active_extent.height <= capacity.height,
          "unaligned narrow selection stays in the same prepared integer range");
}

void TestOrdinaryRateAndIgnoredSamples()
{
    ViewState view;
    Check(UpdateViewState(view, {1920, 1080}, {1920, 1080}, {1920, 1080}), "ordinary DRS view");
    DynamicResolutionConfig config{true, 60, 50, 100};
    DynamicResolutionController controller;
    const auto selected = controller.Select(view, config, slow_sample);
    Check(selected.changed && selected.active_extent.width < view.render_active.width &&
              selected.active_extent.height < view.render_active.height &&
              view.render_active.width - selected.active_extent.width <= view.output_extent.width * 0.02 &&
              view.render_active.height - selected.active_extent.height <= view.output_extent.height * 0.02 &&
              selected.active_extent.width % 8 == 0 && selected.active_extent.height % 8 == 0,
          "ordinary DRS prefers alignment within its rate bound");

    Check(UpdateViewState(view, {1920, 1080}, {1920, 1080}, {1001, 563}), "unaligned rate fixture");
    config.max_scale_change_percent = 0.01;
    controller.Reset();
    Check(controller.Select(view, config, slow_sample).active_extent == view.render_active,
          "a subpixel rate bound does not force an aligned step");
    for (const auto sample : std::array{
             DynamicResolutionSample{true, false, true, 30},
             DynamicResolutionSample{false, false, true, 30, DynamicResolutionMetricKind::ReferenceGraphSpan},
             DynamicResolutionSample{true, true, true, 30, DynamicResolutionMetricKind::ReferenceGraphSpan},
             DynamicResolutionSample{true, false, false, 30, DynamicResolutionMetricKind::ReferenceGraphSpan},
             DynamicResolutionSample{true, false, true, 0, DynamicResolutionMetricKind::ReferenceGraphSpan},
             DynamicResolutionSample{true, false, true, std::numeric_limits<double>::quiet_NaN(), DynamicResolutionMetricKind::ReferenceGraphSpan},
             DynamicResolutionSample{true, false, true, std::numeric_limits<double>::infinity(), DynamicResolutionMetricKind::ReferenceGraphSpan}})
    {
        controller.Reset();
        const auto ignored = controller.Select(view, config, sample);
        Check(!ignored.sample_accepted && !ignored.changed && ignored.active_extent == view.render_active,
              "unqualified-metric/uncompleted/generated/CPU-limited/invalid samples do not select an extent");
    }
    config.enabled = false;
    Check(!controller.Select(view, config, slow_sample).sample_accepted, "disabled DRS ignores timing");
}

DynamicResolutionSample SourceSample(const ViewState &view, std::uint64_t frame)
{
    DynamicResolutionSample sample;
    sample.completed = true;
    sample.metric_kind = DynamicResolutionMetricKind::SourceOutputReady;
    sample.view_id = view.view_id;
    sample.source_frame_id = frame;
    sample.metric_generation = view.metric_generation;
    sample.quality_generation = view.quality_generation;
    sample.provider_generation = view.provider_generation;
    sample.render_active = view.render_active;
    sample.calibration_valid = true;
    return sample;
}

void TestQualifiedSourceTiming()
{
    const GpuClockCalibrationEvidence clock{1000, 2000, 17000, 18000, 1'000'000, 1'000'000};
    Check(QualifiedGpuClockInterval(clock, 2000, 16000),
          "monotonic bracketing calibration qualifies a GPU interval");
    Check(!QualifiedGpuClockInterval(clock, 500, 16000) &&
          !QualifiedGpuClockInterval({1000, 2000, 17000, 19000, 1'000'000, 1'000'000}, 2000, 16000),
          "unbracketed or drifting calibration is rejected");
    SourceTimingEndpoint previous;
    previous.view_id = 1;
    previous.source_frame_id = previous.source_frame_sequence = 1;
    previous.metric_generation = 7;
    previous.quality_generation = 3;
    previous.provider_generation = 2;
    previous.render_active = {1536, 864};
    previous.completed = previous.main_view = previous.ready_valid = previous.clock_anchors_valid = true;
    previous.ready_tick = 16000;
    previous.ready_frequency = previous.qpc_frequency = 1'000'000;
    previous.ready_qpc = 18000;
    previous.clock = clock;
    auto current = previous;
    current.source_frame_id = current.source_frame_sequence = 2;
    current.ready_tick = 32000;
    current.ready_qpc = 34000;
    current.clock = {17000, 18000, 33000, 34000, 1'000'000, 1'000'000};
    current.gpu_backlog_proven = true;
    current.cadence_exclusions_valid = true;
    current.closure_span_valid = current.closure_clock_valid = true;
    current.closure_gap_checked = true;
    current.closure_span_ms = 10;
    const auto qualified = QualifySourceTiming(current, &previous);
    Check(qualified.cadence_valid && qualified.span_valid && qualified.gpu_backlog_proven &&
          std::abs(qualified.source_ready_cadence_ms - 16.0) < 1e-6,
          "consecutive same-view endpoints produce an independent cadence and span");
    previous.render_active.width -= 8;
    Check(!QualifySourceTiming(current, &previous).cadence_valid &&
          QualifySourceTiming(current, &previous).span_valid,
          "mixed render extents invalidate cadence without discarding current closure");
    previous.render_active.width += 8;
    current.closure_span_valid = false;
    Check(QualifySourceTiming(current, &previous).cadence_valid &&
          !QualifySourceTiming(current, &previous).span_valid,
          "missing closure coverage cannot be inferred from two ready endpoints");
    current.pacing_gap = true;
    const auto paced = QualifySourceTiming(current, &previous);
    Check(paced.cadence_valid && paced.cadence_gap && !paced.span_valid,
          "pacing invalidates cadence eligibility without inventing closure headroom");
    current.pacing_gap = false;
    current.generated = true;
    Check(QualifySourceTiming(current, &previous).generated,
          "generated output retains its rejection identity");
    ViewState view;
    Check(UpdateViewState(view, {1920, 1080}, {1920, 1080}, {1536, 864}), "source view");
    view.view_id = 1;
    view.metric_generation = 7;
    view.quality_generation = 3;
    view.provider_generation = 2;
    const DynamicResolutionConfig config{true, 60, 50, 100};
    DynamicResolutionController controller;

    auto slow = SourceSample(view, 1);
    slow.cadence_valid = slow.gpu_backlog_proven = true;
    slow.source_ready_cadence_ms = 30;
    const auto down = controller.Select(view, config, slow);
    Check(down.sample_accepted && down.changed && down.active_extent.width < view.render_active.width &&
          down.metric_kind == DynamicResolutionMetricKind::SourceRenderReadyCadence,
          "qualified GPU backlog cadence lowers scale");
    Check(!controller.Select(view, config, slow).sample_accepted,
          "one source frame is consumed only once");

    auto other_view = view;
    other_view.view_id = 2;
    auto other = SourceSample(other_view, 100);
    other.cadence_valid = other.gpu_backlog_proven = true;
    other.source_ready_cadence_ms = 30;
    Check(controller.Select(other_view, config, other).sample_accepted, "other view has its own consumer");
    slow.source_frame_id = 2;
    Check(controller.Select(view, config, slow).sample_accepted,
          "other view's larger frame ID cannot discard a main-view sample");

    auto stale = SourceSample(view, 3);
    stale.cadence_valid = stale.gpu_backlog_proven = true;
    stale.source_ready_cadence_ms = 30;
    Check(UpdateViewState(view, view.output_extent, view.render_capacity, down.active_extent),
          "apply selected source extent");
    ++view.quality_generation;
    Check(!controller.Select(view, config, stale).sample_accepted,
          "old extent and quality cannot drive a new extent");

    auto first = SourceSample(view, 3);
    first.cadence_valid = first.gpu_backlog_proven = true;
    first.source_ready_cadence_ms = 40;
    Check(controller.Select(view, config, first).filtered_critical_path_ms == 40,
          "new quality starts an independent cadence filter");
    ++view.provider_generation;
    first = SourceSample(view, 4);
    first.cadence_valid = first.gpu_backlog_proven = true;
    first.source_ready_cadence_ms = 45;
    Check(controller.Select(view, config, first).filtered_critical_path_ms == 45,
          "new provider starts an independent cadence filter");
    Check(UpdateViewState(view, view.output_extent, view.render_capacity,
                          {view.render_active.width - 8, view.render_active.height - 8}),
          "change active extent without reallocating capacity");
    first = SourceSample(view, 5);
    first.cadence_valid = first.gpu_backlog_proven = true;
    first.source_ready_cadence_ms = 50;
    Check(controller.Select(view, config, first).filtered_critical_path_ms == 50,
          "new active extent starts an independent cadence filter");

    auto short_cadence = SourceSample(view, 6);
    short_cadence.cadence_valid = short_cadence.gpu_backlog_proven = true;
    short_cadence.source_ready_cadence_ms = 8;
    Check(!controller.Select(view, config, short_cadence).changed,
          "short cadence alone cannot claim upscale headroom");
    short_cadence.source_frame_id = 7;
    short_cadence.span_valid = true;
    short_cadence.output_dependency_span_ms = 10;
    const auto both_short = controller.Select(view, config, short_cadence);
    Check(both_short.changed && both_short.active_extent.width > view.render_active.width &&
          both_short.metric_kind == DynamicResolutionMetricKind::DisplayDependencySpan,
          "independent short closure permits upscale when cadence is also short");
    short_cadence.source_frame_id = 8;
    short_cadence.closure_span_gap = true;
    Check(!controller.Select(view, config, short_cadence).changed,
          "short cadence with a closure gap cannot upscale");

    auto recovery = SourceSample(view, 9);
    recovery.span_valid = true;
    recovery.output_dependency_span_ms = 10;
    recovery.cadence_gap = true; // A supply gap between frames does not extend this closure.
    const auto up = controller.Select(view, config, recovery);
    Check(up.sample_accepted && up.changed && up.active_extent.width > view.render_active.width &&
          up.metric_kind == DynamicResolutionMetricKind::DisplayDependencySpan,
          "independently valid short closure recovers scale without cadence");
    recovery.source_frame_id = 10;
    recovery.closure_span_gap = true;
    Check(!controller.Select(view, config, recovery).sample_accepted,
          "a gap inside the closure cannot establish recovery headroom");
    recovery.source_frame_id = 11;
    recovery.closure_span_gap = false;
    recovery.calibration_drift = true;
    Check(!controller.Select(view, config, recovery).sample_accepted,
          "clock drift invalidates both measurements");
    recovery.source_frame_id = 12;
    recovery.calibration_drift = false;
    recovery.output_dependency_span_ms = 16;
    Check(!controller.Select(view, config, recovery).sample_accepted,
          "near-budget closure is not recovery headroom");
}
} // namespace

int main()
{
    try
    {
        TestPreparedIntegerDomains();
        TestHardBoundsBeforeAlignmentAndRate();
        TestOrdinaryRateAndIgnoredSamples();
        TestQualifiedSourceTiming();
        std::cout << "dynamic_resolution_tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "dynamic_resolution_tests failed: " << error.what() << '\n';
        return 1;
    }
}
