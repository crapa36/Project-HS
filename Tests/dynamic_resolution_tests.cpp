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

constexpr DynamicResolutionSample slow_sample{true, false, true, 30.0};

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
        const auto selected = controller.Select(view, config, {true, false, true, 1000.0 / 60.0});
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
             DynamicResolutionSample{false, false, true, 30},
             DynamicResolutionSample{true, true, true, 30},
             DynamicResolutionSample{true, false, false, 30},
             DynamicResolutionSample{true, false, true, 0},
             DynamicResolutionSample{true, false, true, std::numeric_limits<double>::quiet_NaN()},
             DynamicResolutionSample{true, false, true, std::numeric_limits<double>::infinity()}})
    {
        controller.Reset();
        const auto ignored = controller.Select(view, config, sample);
        Check(!ignored.sample_accepted && !ignored.changed && ignored.active_extent == view.render_active,
              "uncompleted/generated/CPU-limited/invalid samples do not select an extent");
    }
    config.enabled = false;
    Check(!controller.Select(view, config, slow_sample).sample_accepted, "disabled DRS ignores timing");
}
} // namespace

int main()
{
    try
    {
        TestPreparedIntegerDomains();
        TestHardBoundsBeforeAlignmentAndRate();
        TestOrdinaryRateAndIgnoredSamples();
        std::cout << "dynamic_resolution_tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "dynamic_resolution_tests failed: " << error.what() << '\n';
        return 1;
    }
}
