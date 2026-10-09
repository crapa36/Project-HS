#include "renderer_impl.hpp"

#include <format>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace hs
{

Result HResultFailure(std::string_view operation, HRESULT result)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                           std::format("{} failed: 0x{:08X}", operation,
                                       static_cast<std::uint32_t>(result)));
}

namespace
{
RendererDiagnosticsPercentiles Percentiles(std::span<double> values)
{
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    const auto nearest_rank = [&](double quantile) {
        const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(values.size())));
        return values[std::min(values.size() - 1, rank - 1)];
    };
    return {true, static_cast<std::uint32_t>(values.size()),
        nearest_rank(.50), nearest_rank(.95), nearest_rank(.99)};
}

void MergeMemoryPeak(RendererDiagnosticsMemory &peak, const RendererDiagnosticsMemory &sample) noexcept
{
    peak.budget_available |= sample.budget_available;
    peak.nonlocal_budget_available |= sample.nonlocal_budget_available;
    peak.host_budget_available |= sample.host_budget_available;
#define HS_DIAGNOSTICS_PEAK(field) peak.field = std::max(peak.field, sample.field)
    HS_DIAGNOSTICS_PEAK(local_usage_bytes); HS_DIAGNOSTICS_PEAK(local_budget_bytes);
    HS_DIAGNOSTICS_PEAK(nonlocal_usage_bytes); HS_DIAGNOSTICS_PEAK(nonlocal_budget_bytes);
    HS_DIAGNOSTICS_PEAK(tracked_live_bytes); HS_DIAGNOSTICS_PEAK(tracked_retiring_bytes);
    HS_DIAGNOSTICS_PEAK(tracked_reclaimed_bytes); HS_DIAGNOSTICS_PEAK(tracked_gpu_bytes);
    HS_DIAGNOSTICS_PEAK(host_staging_bytes); HS_DIAGNOSTICS_PEAK(host_reserved_bytes);
    HS_DIAGNOSTICS_PEAK(gpu_reserved_bytes); HS_DIAGNOSTICS_PEAK(gpu_growth_since_budget_bytes);
    HS_DIAGNOSTICS_PEAK(host_staging_budget_bytes); HS_DIAGNOSTICS_PEAK(texture_physical_bytes);
    HS_DIAGNOSTICS_PEAK(acceleration_structure_bytes); HS_DIAGNOSTICS_PEAK(presentation_provider_bytes);
    HS_DIAGNOSTICS_PEAK(reconstruction_guide_bytes); HS_DIAGNOSTICS_PEAK(ray_reconstruction_bytes);
    HS_DIAGNOSTICS_PEAK(renderer_view_bytes); HS_DIAGNOSTICS_PEAK(shared_deformation_bytes);
#undef HS_DIAGNOSTICS_PEAK
    for (std::size_t index = 0; index < peak.live_bytes.size(); ++index)
    {
        peak.live_bytes[index] = std::max(peak.live_bytes[index], sample.live_bytes[index]);
        peak.retiring_bytes[index] = std::max(peak.retiring_bytes[index], sample.retiring_bytes[index]);
    }
}

RendererDiagnosticsMeasurementSet Measurements(std::span<const RendererDiagnosticsFrame> frames,
                                               bool validation, std::optional<std::uint32_t> view_id = {})
{
    RendererDiagnosticsMeasurementSet output;
    const auto selected = [&](const auto &frame) {
        return view_id ? frame.view_id == *view_id : frame.presented;
    };
    const auto collect = [&](auto value) {
        std::array<double, kRendererDiagnosticsFrameCapacity> values{};
        std::size_t count{};
        for (const auto &frame : frames)
        {
            if (!selected(frame) || (frame.validation_enabled || frame.gpu_validation_enabled) != validation) continue;
            const auto sample = value(frame);
            if (sample && std::isfinite(*sample) && *sample >= 0.0) values[count++] = *sample;
        }
        return Percentiles(std::span(values).first(count));
    };
    output.cpu_render_ms = collect([](const auto &frame) -> std::optional<double> {
        return frame.cpu_recording_available ? std::optional(frame.cpu_recording_ms) : std::nullopt;
    });
    output.gpu_critical_path_ms = collect([](const auto &frame) -> std::optional<double> {
        return frame.gpu_critical_path_available ? std::optional(frame.gpu_critical_path_ms) : std::nullopt;
    });
    const auto duration = [](const auto &frame, std::uint64_t begin, std::uint64_t end) -> std::optional<double> {
        if (!frame.qpc_frequency || !begin || end < begin) return std::nullopt;
        return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frame.qpc_frequency);
    };
    output.present_call_ms = collect([&](const auto &frame) {
        return frame.presented ? duration(frame, frame.present_begin_qpc, frame.present_end_qpc) : std::nullopt;
    });
    output.submit_to_present_return_ms = collect([&](const auto &frame) {
        return frame.presented ? duration(frame, frame.submit_qpc, frame.present_end_qpc) : std::nullopt;
    });
    const auto cadence = [&](bool present) {
        RendererDiagnosticsCadence result;
        result.source = present ? "cpu_qpc_successful_present_call_start" : "cpu_qpc_successful_graph_submission_end";
        std::array<double, kRendererDiagnosticsFrameCapacity> intervals{};
        std::size_t count{};
        const RendererDiagnosticsFrame *previous{};
        double elapsed{};
        for (const auto &frame : frames)
        {
            if (!selected(frame)) continue;
            if ((frame.validation_enabled || frame.gpu_validation_enabled) != validation)
            { previous = nullptr; continue; }
            if (present && !frame.presented) { previous = nullptr; continue; }
            const auto time = present ? frame.present_begin_qpc : frame.submit_qpc;
            if (!time || !frame.qpc_frequency) { previous = nullptr; continue; }
            ++result.frames;
            if (previous && frame.view_id == previous->view_id &&
                frame.view_render_sequence == previous->view_render_sequence + 1 &&
                frame.qpc_frequency == previous->qpc_frequency)
            {
                const auto previous_time = present ? previous->present_begin_qpc : previous->submit_qpc;
                if (time > previous_time)
                {
                    const auto interval = static_cast<double>(time - previous_time) * 1000.0 / static_cast<double>(frame.qpc_frequency);
                    intervals[count++] = interval;
                    elapsed += interval;
                }
            }
            previous = &frame;
        }
        result.pacing_ms = Percentiles(std::span(intervals).first(count));
        result.available = count > 0 && elapsed > 0.0;
        result.elapsed_ms = elapsed;
        if (result.available) result.fps = static_cast<double>(count) * 1000.0 / elapsed;
        else result.unavailable_reason = "At least two consecutive completed application-render frames with QPC observations are required.";
        return result;
    };
    output.rendered_cadence = cadence(false);
    output.present_call_cadence = cadence(true);
    return output;
}
} // namespace

std::uint64_t RendererDiagnosticsCounter() noexcept
{
    LARGE_INTEGER counter{};
    return QueryPerformanceCounter(&counter) && counter.QuadPart > 0
        ? static_cast<std::uint64_t>(counter.QuadPart) : 0;
}

void RendererDiagnosticsRecorder::RecordRenderedFrame(std::uint64_t id, std::uint64_t submit,
    std::uint64_t present_begin, std::uint64_t present_end, const RendererQueueStats &queues,
    const RendererDiagnosticsMemory &memory, bool validation, bool gpu_validation,
    bool presented, std::uint32_t view_id) noexcept
{
    if (!id || id < last_rendered_id_) { ++rejected_; return; }
    if (id == last_rendered_id_) { ++duplicates_; return; }
    std::uint64_t view_sequence{};
    for (const auto &recorded : frames_)
        if (recorded.rendered_frame_id && recorded.view_id == view_id)
            view_sequence = std::max(view_sequence, recorded.view_render_sequence);
    auto &frame = frames_[next_frame_];
    frame = {};
    frame.rendered_frame_id = id;
    frame.submit_qpc = submit;
    frame.present_begin_qpc = presented ? present_begin : 0;
    frame.present_end_qpc = presented ? present_end : 0;
    frame.presented = presented;
    frame.view_id = view_id;
    frame.view_render_sequence = view_id == kMainRenderViewId.value
        ? ++rendered_main_sequence_ : view_sequence + 1;
    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
        frame.qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
    frame.queues = queues;
    frame.memory = memory;
    frame.validation_enabled = validation;
    frame.gpu_validation_enabled = gpu_validation;
    next_frame_ = (next_frame_ + 1) % frames_.size();
    frame_count_ = std::min(frame_count_ + 1, frames_.size());
    last_rendered_id_ = id;
    ++rendered_;
    MergeMemoryPeak(sampled_peak_, memory);
}

void RendererDiagnosticsRecorder::RecordCompleted(const GpuPassTimings &timings) noexcept
{
    if (!timings.fence_completed || !timings.valid || !timings.completed_frame_id)
    { ++rejected_; return; }
    auto entry = std::find_if(frames_.begin(), frames_.end(), [&](const auto &frame) {
        return frame.rendered_frame_id == timings.completed_frame_id &&
               frame.view_id == timings.view_id;
    });
    // No metadata exists for generated frames, fabricated IDs, or evicted samples.
    if (entry == frames_.end()) { ++rejected_; return; }
    if (entry->fence_completed) { ++duplicates_; return; }
    entry->fence_completed = true;
    if (entry->view_id == kMainRenderViewId.value)
    {
        // Independent retention prevents preview/capture submissions from
        // evicting the base-cadence window used by the sole presentation owner.
        main_cadence_[entry->view_render_sequence % main_cadence_.size()] = {
            entry->view_render_sequence, entry->rendered_frame_id, entry->submit_qpc,
            entry->qpc_frequency, entry->presented, entry->validation_enabled,
            entry->gpu_validation_enabled};
        latest_main_cadence_sequence_ = std::max(latest_main_cadence_sequence_, entry->view_render_sequence);
    }
    entry->cpu_recording_available = timings.cpu_recording_valid && std::isfinite(timings.cpu_recording_ms) && timings.cpu_recording_ms >= 0.0;
    entry->cpu_recording_ms = entry->cpu_recording_available ? timings.cpu_recording_ms : 0.0;
    entry->gpu_critical_path_available = timings.critical_path_valid && std::isfinite(timings.critical_path_ms) && timings.critical_path_ms > 0.0;
    entry->gpu_critical_path_ms = entry->gpu_critical_path_available ? timings.critical_path_ms : 0.0;
    ++completed_;
    if (timings.completed_frame_id <= timeline_frame_id_) return;
    timeline_frame_id_ = timings.completed_frame_id;
    timeline_view_id_ = entry->view_id;
    timeline_truncated_ = timings.passes.size() > passes_.size();
    pass_count_ = std::min(timings.passes.size(), passes_.size());
    for (std::size_t index = 0; index < pass_count_; ++index)
    {
        const auto &input = timings.passes[index];
        auto &pass = passes_[index];
        pass = {};
        pass.pass_id = input.pass_id;
        const auto copied = std::min(input.name.size(), pass.name.size() - 1);
        std::copy_n(input.name.data(), copied, pass.name.data());
        pass.name_truncated = copied != input.name.size();
        pass.queue = input.queue;
        pass.measured = input.measured;
        pass.nanoseconds = input.nanoseconds;
        pass.begin_ticks = input.begin_ticks; pass.end_ticks = input.end_ticks;
        pass.timestamp_frequency = input.timestamp_frequency;
        pass.begin_qpc = input.begin_qpc; pass.end_qpc = input.end_qpc;
        pass.qpc_frequency = input.qpc_frequency;
        pass.clock_calibrated = input.clock_calibrated;
    }
}

void RendererDiagnosticsRecorder::ObserveTransition(const RendererDiagnosticsMemory &memory) noexcept
{
    transition_observed_ = true;
    MergeMemoryPeak(transition_peak_, memory);
    MergeMemoryPeak(sampled_peak_, memory);
}

bool RendererDiagnosticsRecorder::HasCompleted(std::uint64_t frame_id, std::uint32_t view_id) const noexcept
{
    return std::any_of(frames_.begin(), frames_.end(), [&](const auto &frame) {
        return frame.rendered_frame_id == frame_id && frame.view_id == view_id &&
               frame.fence_completed;
    });
}

CompletedRenderCadence RendererDiagnosticsRecorder::RecentCompletedMainCadence(
    std::uint32_t maximum_intervals) const noexcept
{
    CompletedRenderCadence result;
    const MainCadenceSample *newer{};
    double seconds{};
    for (std::uint64_t offset = 0; offset < main_cadence_.size() && offset < latest_main_cadence_sequence_ &&
         result.intervals < maximum_intervals; ++offset)
    {
        const auto sequence = latest_main_cadence_sequence_ - offset;
        const auto &frame = main_cadence_[sequence % main_cadence_.size()];
        if (frame.sequence != sequence) break;
        if (!frame.presented || !frame.submit_qpc || !frame.qpc_frequency) break;
        if (newer)
        {
            if (newer->qpc_frequency != frame.qpc_frequency || newer->submit_qpc <= frame.submit_qpc ||
                newer->validation != frame.validation || newer->gpu_validation != frame.gpu_validation) break;
            seconds += static_cast<double>(newer->submit_qpc - frame.submit_qpc) /
                       static_cast<double>(frame.qpc_frequency);
            ++result.intervals;
        }
        else result.newest_frame_id = frame.frame_id;
        newer = &frame;
    }
    if (seconds > 0.0) result.fps = static_cast<double>(result.intervals) / seconds;
    return result;
}

RendererDiagnosticsSnapshot RendererDiagnosticsRecorder::Snapshot() const
{
    RendererDiagnosticsSnapshot output;
    output.total_rendered_frames = rendered_; output.total_completed_frames = completed_;
    output.duplicate_samples = duplicates_; output.rejected_samples = rejected_;
    output.timeline_completed_frame_id = timeline_frame_id_;
    output.timeline_view_id = timeline_view_id_;
    output.timeline_truncated = timeline_truncated_;
    output.completed_frames.reserve(frame_count_);
    for (const auto &frame : frames_)
        if (frame.fence_completed) output.completed_frames.push_back(frame);
    std::sort(output.completed_frames.begin(), output.completed_frames.end(),
        [](const auto &a, const auto &b) { return a.rendered_frame_id < b.rendered_frame_id; });
    output.validation_on = Measurements(output.completed_frames, true);
    output.validation_off = Measurements(output.completed_frames, false);
    for (const auto &frame : output.completed_frames)
        if (std::none_of(output.views.begin(), output.views.end(), [&](const auto &view) { return view.view_id == frame.view_id; }))
        {
            output.views.push_back({frame.view_id, frame.presented,
                Measurements(output.completed_frames, true, frame.view_id),
                Measurements(output.completed_frames, false, frame.view_id)});
        }
    output.displayed_cadence.source = "unavailable";
    output.displayed_cadence.unavailable_reason = "Native displayed-frame timestamps/counts are not exposed by the presentation owner; Present calls and FG multipliers do not establish displayed cadence.";
    output.sampled_memory_peak = sampled_peak_;
    output.transition_memory_peak = transition_peak_;
    output.transition_memory_peak_available = transition_observed_;
    output.transition_memory_peak_source = "componentwise_maximum_at_observed_live_and_candidate_allocation_boundaries";
    if (!transition_observed_) output.transition_memory_peak_reason = "No live/candidate transition allocation boundary has been observed.";
    std::uint64_t qpc_origin = std::numeric_limits<std::uint64_t>::max();
    std::array<std::uint64_t, 3> queue_origins{};
    queue_origins.fill(std::numeric_limits<std::uint64_t>::max());
    for (std::size_t index = 0; index < pass_count_; ++index)
    {
        const auto &pass = passes_[index];
        if (!pass.measured) continue;
        if (pass.clock_calibrated) qpc_origin = std::min(qpc_origin, pass.begin_qpc);
        const auto queue_index = static_cast<std::size_t>(pass.queue);
        if (queue_index < queue_origins.size()) queue_origins[queue_index] = std::min(queue_origins[queue_index], pass.begin_ticks);
    }
    output.passes.reserve(pass_count_);
    for (std::size_t index = 0; index < pass_count_; ++index)
    {
        const auto &pass = passes_[index];
        RendererDiagnosticsPass result;
        result.pass_id = pass.pass_id; result.name = pass.name.data(); result.queue = pass.queue;
        result.measured = pass.measured; result.clock_calibrated = pass.clock_calibrated;
        result.name_truncated = pass.name_truncated;
        result.acceleration_structure_work = pass.pass_id >= 120 && pass.pass_id <= 125;
        result.nanoseconds = pass.nanoseconds; result.begin_ticks = pass.begin_ticks;
        result.end_ticks = pass.end_ticks; result.timestamp_frequency = pass.timestamp_frequency;
        result.clock_domain = "unavailable";
        if (pass.measured && pass.clock_calibrated && pass.qpc_frequency)
        {
            result.clock_domain = "cpu_qpc_calibrated";
            result.begin_ms = static_cast<double>(pass.begin_qpc - qpc_origin) * 1000.0 / static_cast<double>(pass.qpc_frequency);
            result.end_ms = static_cast<double>(pass.end_qpc - qpc_origin) * 1000.0 / static_cast<double>(pass.qpc_frequency);
        }
        else if (pass.measured && pass.timestamp_frequency)
        {
            const auto queue_index = static_cast<std::size_t>(pass.queue);
            if (queue_index < queue_origins.size())
            {
                result.clock_domain = "queue_local_" + std::to_string(queue_index);
                result.begin_ms = static_cast<double>(pass.begin_ticks - queue_origins[queue_index]) * 1000.0 / static_cast<double>(pass.timestamp_frequency);
                result.end_ms = static_cast<double>(pass.end_ticks - queue_origins[queue_index]) * 1000.0 / static_cast<double>(pass.timestamp_frequency);
            }
        }
        output.passes.push_back(std::move(result));
    }
    return output;
}

RendererDiagnosticsMemory D3D12Renderer::Impl::DiagnosticsMemory() const
{
    RendererDiagnosticsMemory output;
    const auto accounting = residency.Accounting();
    static_assert(static_cast<std::size_t>(RendererDiagnosticsMemoryCategory::Count) == static_cast<std::size_t>(ResidencyCategory::Count));
    output.live_bytes = accounting.live_bytes; output.retiring_bytes = accounting.retiring_bytes;
    output.tracked_live_bytes = accounting.live_total; output.tracked_retiring_bytes = accounting.retiring_total;
    output.tracked_reclaimed_bytes = accounting.reclaimed_total;
    output.tracked_gpu_bytes = accounting.live_total + accounting.retiring_total;
    output.host_staging_bytes = accounting.host_staging_total; output.host_reserved_bytes = accounting.host_reserved_total;
    output.gpu_reserved_bytes = accounting.gpu_reserved_total; output.gpu_growth_since_budget_bytes = accounting.gpu_growth_since_snapshot;
    output.host_budget_available = accounting.budget.host_budget_valid;
    output.host_staging_budget_bytes = accounting.budget.host_staging_budget;
    // Query here as well as at normal frame admission: a transition observation
    // must see current process usage while old and candidate resources coexist.
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
    {
        output.budget_available = true;
        output.local_usage_bytes = local.CurrentUsage; output.local_budget_bytes = local.Budget;
    }
    if (adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal)))
    {
        output.nonlocal_budget_available = true;
        output.nonlocal_usage_bytes = nonlocal.CurrentUsage; output.nonlocal_budget_bytes = nonlocal.Budget;
    }
    output.texture_physical_bytes = texture_streaming.PhysicalBytes();
    output.acceleration_structure_bytes = ray_scene.AllocatedBytes();
    output.presentation_provider_bytes = sdk_presentation.Status().allocated_bytes + presentation_guides.AllocatedBytes();
    output.reconstruction_guide_bytes = reconstruction_guides.AllocatedBytes();
    output.ray_reconstruction_bytes = ray_reconstruction.AllocatedBytes();
    // Known renderer allocations are independent of ResidencyManager's partial
    // tracking and of opaque native SDK allocations. Shared deformation leases
    // retain allocation identities, so count those allocations only once.
    std::unordered_set<const D3D12MA::Allocation *> allocations;
    const auto bytes = [&](const AllocationResource &resource) -> std::uint64_t {
        return resource.allocation && allocations.insert(resource.allocation).second
            ? resource.allocation->GetSize() : 0;
    };
    const auto target_bytes = [&](const auto &targets) {
        std::uint64_t total{};
#define HS_VIEW_ALLOCATION(field) total += bytes(targets.field)
        HS_VIEW_ALLOCATION(depth); HS_VIEW_ALLOCATION(shadow);
        HS_VIEW_ALLOCATION(gbuffer_base); HS_VIEW_ALLOCATION(gbuffer_normal);
        HS_VIEW_ALLOCATION(gbuffer_metadata); HS_VIEW_ALLOCATION(gbuffer_material);
        HS_VIEW_ALLOCATION(gbuffer_motion); HS_VIEW_ALLOCATION(gbuffer_surface_history);
        HS_VIEW_ALLOCATION(visibility_ids); HS_VIEW_ALLOCATION(visibility_front_face);
        HS_VIEW_ALLOCATION(visibility_pixels); HS_VIEW_ALLOCATION(visibility_counts);
        HS_VIEW_ALLOCATION(visibility_dispatch_args); HS_VIEW_ALLOCATION(hdr_color);
        HS_VIEW_ALLOCATION(oit_accumulation); HS_VIEW_ALLOCATION(oit_revealage);
        HS_VIEW_ALLOCATION(subsurface_diffuse); HS_VIEW_ALLOCATION(environment_specular);
        HS_VIEW_ALLOCATION(indirect_diffuse); HS_VIEW_ALLOCATION(post_a); HS_VIEW_ALLOCATION(post_b);
        HS_VIEW_ALLOCATION(scene_work); HS_VIEW_ALLOCATION(display_linear); HS_VIEW_ALLOCATION(composite_hdr);
#undef HS_VIEW_ALLOCATION
        for (const auto *bank : {&targets.material_sidecars, &targets.temporal_history,
                &targets.temporal_normal_history, &targets.temporal_material_history, &targets.hzb_history})
            for (const auto &resource : *bank) total += bytes(resource);
        for (const auto &pool : targets.transient_pools) total += pool.AllocatedBytes();
        return total;
    };
    const auto auxiliary_bytes = [&](const auto &view) {
        std::uint64_t total{};
        for (const auto &resource : view.exposure_values) total += bytes(resource);
        total += bytes(view.exposure_histogram) + bytes(view.initial_exposure);
#define HS_VIEW_SERVICE(field) total += view.field.AllocatedBytes()
        HS_VIEW_SERVICE(ambient_occlusion); HS_VIEW_SERVICE(adaptive_shading);
        HS_VIEW_SERVICE(subsurface); HS_VIEW_SERVICE(screen_reflections);
        HS_VIEW_SERVICE(hero_transparency); HS_VIEW_SERVICE(hdr_post); HS_VIEW_SERVICE(screen_overlay);
        HS_VIEW_SERVICE(ray_ambient_occlusion); HS_VIEW_SERVICE(ray_transport);
        HS_VIEW_SERVICE(ray_reconstruction); HS_VIEW_SERVICE(brixelizer_gi);
        HS_VIEW_SERVICE(virtual_shadows); HS_VIEW_SERVICE(local_shadows); HS_VIEW_SERVICE(ray_shadows);
        HS_VIEW_SERVICE(volumetrics); HS_VIEW_SERVICE(light_list); HS_VIEW_SERVICE(decal_surface);
        HS_VIEW_SERVICE(reconstruction_guides);
#undef HS_VIEW_SERVICE
        const auto lod_bytes = [&](const GpuLodStateBuffer &lod) {
            total += bytes(lod.states) + bytes(lod.zero_upload) + bytes(lod.prefix_source);
            for (const auto &retired : lod.retired) total += bytes(retired.staging) + bytes(retired.source);
        };
        lod_bytes(view.dynamic_lod_states);
        return total;
    };
    output.renderer_view_bytes = target_bytes(*this) + auxiliary_bytes(*this);
    if (static_gpu_scene)
    {
        const auto &lod = static_gpu_scene->lod_states;
        output.renderer_view_bytes += bytes(lod.states) + bytes(lod.zero_upload) + bytes(lod.prefix_source);
        for (const auto &retired : lod.retired)
            output.renderer_view_bytes += bytes(retired.staging) + bytes(retired.source);
    }
    for (const auto &resource : ui_textures) output.renderer_view_bytes += bytes(resource);
    for (const auto &frame : frames) output.renderer_view_bytes += bytes(frame.ui_upload);
    for (const auto &[id, view] : owned_views)
    {
        for (const auto &resource : view->offscreen_allocations) output.renderer_view_bytes += bytes(resource);
        if (id == active_view_id.value) continue;
        for (const auto &resource : view->ui.textures) output.renderer_view_bytes += bytes(resource);
        for (const auto &resource : view->ui.uploads) output.renderer_view_bytes += bytes(resource);
        output.reconstruction_guide_bytes += view->reconstruction_guides.AllocatedBytes();
        output.ray_reconstruction_bytes += view->ray_reconstruction.AllocatedBytes();
        output.renderer_view_bytes += target_bytes(view->targets) + auxiliary_bytes(*view);
        output.renderer_view_bytes += bytes(view->static_lod_states.states) + bytes(view->static_lod_states.zero_upload)
            + bytes(view->static_lod_states.prefix_source);
        for (const auto &retired : view->static_lod_states.retired)
            output.renderer_view_bytes += bytes(retired.staging) + bytes(retired.source);
    }
    for (const auto &frame : frames)
        output.shared_deformation_bytes += bytes(frame.skin_cache_vertices) + bytes(frame.skin_cache_bounds);
    const auto skin_bytes = [&](const auto &lease) {
        if (lease) output.shared_deformation_bytes += bytes(lease->skin_cache_vertices) + bytes(lease->skin_cache_bounds);
    };
    skin_bytes(canonical_skin_current); skin_bytes(previous_skin_deformation);
    for (const auto &lease : canonical_skin_frames) skin_bytes(lease);
    for (const auto &[id, view] : owned_views) skin_bytes(view->previous_skin_deformation);
    return output;
}

RendererDiagnosticsSnapshot D3D12Renderer::GetDiagnostics() const
{
    if (!impl_) return {};
    // Refresh completed slots on a copy so capture can include the final frames
    // after a GPU wait without mutating the recorder through this const API.
    auto recorder = impl_->diagnostics;
    for (const auto &frame : impl_->frames)
        if (frame.timestamps_recorded && !recorder.HasCompleted(frame.submitted_frame_id, frame.submitted_view_id) &&
            frame.completion.IsSatisfiedBy(impl_->graph_queue_executor.Completed()))
            recorder.RecordCompleted(impl_->ReadFrameGpuTimings(frame));
    auto output = recorder.Snapshot();
    output.current_memory = impl_->DiagnosticsMemory();
    output.streaming = ResidencyStats();
    output.presentation = GetPresentationStatus();
    output.reconstruction = GetReconstructionStatus();
    return output;
}

Result D3D12Renderer::Impl::CheckDevice(HRESULT result, std::string_view operation)
{
    if (SUCCEEDED(result))
    {
        return Result::Success();
    }
    CountValidationErrors();
    const auto reason = device ? device->GetDeviceRemovedReason() : result;
    if (reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        WriteDred(reason);
    }
    return HResultFailure(operation, result);
}


void D3D12Renderer::Impl::CountValidationErrors()
{
    if (!config.validation || !device)
    {
        return;
    }
    ComPtr<ID3D12InfoQueue> info_queue;
    if (FAILED(device.As(&info_queue)))
    {
        return;
    }
    const auto count = info_queue->GetNumStoredMessages();
    std::ofstream log(config.artifact_directory / "d3d12_validation.log", std::ios::app);
    for (std::uint64_t index = 0; index < count; ++index)
    {
        SIZE_T size{};
        info_queue->GetMessage(index, nullptr, &size);
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (SUCCEEDED(info_queue->GetMessage(index, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
             message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION))
        {
            ++validation_errors;
            log << "id=" << message->ID << " severity=" << message->Severity << ' '
                << message->pDescription << '\n';
        }
    }
    info_queue->ClearStoredMessages();
}

void D3D12Renderer::Impl::WriteDred(HRESULT reason) const
{
    std::error_code error;
    std::filesystem::create_directories(config.artifact_directory, error);
    std::ofstream stream(config.artifact_directory / "dred.txt", std::ios::trunc);
    stream << std::format("device_removed_reason=0x{:08X}\n",
                          static_cast<std::uint32_t>(reason));
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (device && SUCCEEDED(device.As(&dred)))
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            stream << "breadcrumbs=" << (breadcrumbs.pHeadAutoBreadcrumbNode ? "present" : "none")
                   << '\n';
            const auto debug_name = [](const char *ansi, const wchar_t *wide) {
                if (ansi) return std::string(ansi);
                if (!wide) return std::string{};
                const auto count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0,
                                                       nullptr, nullptr);
                if (count <= 1) return std::string{};
                std::string name(static_cast<std::size_t>(count), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), count, nullptr, nullptr);
                name.pop_back();
                return name;
            };
            // Retain the failure position and its nearby PIX contexts instead of
            // reporting only that a breadcrumb list exists. Bound failure output.
            const auto *node = breadcrumbs.pHeadAutoBreadcrumbNode;
            std::uint32_t node_index{};
            for (; node && node_index < 128; node = node->pNext, ++node_index)
            {
                const auto completed = node->pLastBreadcrumbValue
                    ? std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount) : 0u;
                stream << "node=" << node_index << " list=" << node->pCommandList
                       << " queue=" << node->pCommandQueue << " list_name="
                       << debug_name(node->pCommandListDebugNameA, node->pCommandListDebugNameW)
                       << " queue_name="
                       << debug_name(node->pCommandQueueDebugNameA, node->pCommandQueueDebugNameW)
                       << " completed=" << completed << " count=" << node->BreadcrumbCount << '\n';
                const auto first = completed > 4 ? completed - 4 : 0u;
                const auto last = completed + std::min(12u, node->BreadcrumbCount - completed);
                if (node->pCommandHistory)
                    for (auto operation = first; operation < last; ++operation)
                        stream << "op=" << operation << " kind="
                               << static_cast<std::uint32_t>(node->pCommandHistory[operation]) << '\n';
                if (node->pBreadcrumbContexts)
                    for (std::uint32_t context = 0, emitted = 0;
                         context < node->BreadcrumbContextsCount && emitted < 64; ++context)
                    {
                        const auto &entry = node->pBreadcrumbContexts[context];
                        if (entry.BreadcrumbIndex < first || entry.BreadcrumbIndex >= last) continue;
                        stream << "context=" << entry.BreadcrumbIndex << ' '
                               << debug_name(nullptr, entry.pContextString) << '\n';
                        ++emitted;
                    }
            }
            if (node) stream << "breadcrumb_nodes_truncated=1\n";
        }
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&page_fault)))
        {
            stream << "page_fault_va=" << page_fault.PageFaultVA << '\n';
        }
    }
}


} // namespace hs
