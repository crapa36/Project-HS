#include "renderer_impl.hpp"
#include <hs/core/process_info.hpp>

#include <format>
#include <algorithm>
#include <limits>
#include <vector>

namespace hs
{

Result HResultFailure(std::string_view operation, HRESULT result)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                           std::format("{} failed: 0x{:08X}", operation,
                                       static_cast<std::uint32_t>(result)));
}

#if defined(HS_DEVELOPMENT_TOOLS)
namespace
{
std::filesystem::path ErrorLogDirectory(const std::filesystem::path &configured)
{
    auto directory = configured;
    if (directory.empty())
    {
        const auto executable = CurrentExecutablePath();
        directory = executable.parent_path() / "Artifacts" / executable.stem();
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory;
}

} // namespace
#endif

std::uint64_t RendererDiagnosticsCounter() noexcept
{
    LARGE_INTEGER counter{};
    return QueryPerformanceCounter(&counter) && counter.QuadPart > 0
        ? static_cast<std::uint64_t>(counter.QuadPart) : 0;
}

#if defined(HS_DEVELOPMENT_TOOLS)
void RendererDiagnosticsRecorder::RecordRenderedFrame(std::uint64_t id, std::uint64_t submit,
    std::uint64_t present_begin, std::uint64_t present_end, const RendererQueueStats &queues,
    const RendererDiagnosticsMemory &memory, bool validation, bool gpu_validation,
    bool presented, std::uint32_t view_id) noexcept
{
    if (id <= last_rendered_id_) return;
    auto &frame = frames_[next_frame_];
    frame = {};
    frame.rendered_frame_id = id;
    frame.submit_qpc = submit;
    frame.present_begin_qpc = presented ? present_begin : 0;
    frame.present_end_qpc = presented ? present_end : 0;
    frame.presented = presented;
    frame.view_id = view_id;
    frame.view_render_sequence = view_id == kMainRenderViewId.value ? ++rendered_main_sequence_ : 0;
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
}

void RendererDiagnosticsRecorder::RecordCompleted(const GpuPassTimings &timings) noexcept
{
    if (!timings.fence_completed || !timings.valid || !timings.completed_frame_id) return;
    auto entry = std::find_if(frames_.begin(), frames_.end(), [&](const auto &frame) {
        return frame.rendered_frame_id == timings.completed_frame_id &&
               frame.view_id == timings.view_id;
    });
    // No metadata exists for generated frames, fabricated IDs, or evicted samples.
    if (entry != frames_.end()) entry->fence_completed = true;
}

void RendererDiagnosticsRecorder::ObserveTransition(std::uint64_t local_usage_bytes) noexcept
{
    transition_peak_local_usage_bytes_ = std::max(
        transition_peak_local_usage_bytes_.value_or(0), local_usage_bytes);
}

bool RendererDiagnosticsRecorder::HasCompleted(std::uint64_t frame_id, std::uint32_t view_id) const noexcept
{
    return std::any_of(frames_.begin(), frames_.end(), [&](const auto &frame) {
        return frame.rendered_frame_id == frame_id && frame.view_id == view_id &&
               frame.fence_completed;
    });
}

RendererDiagnosticsSnapshot RendererDiagnosticsRecorder::Snapshot() const
{
    RendererDiagnosticsSnapshot output;
    output.total_rendered_frames = rendered_;
    std::array<std::size_t, 2> intervals{};
    std::array<double, 2> elapsed{};
    const RendererDiagnosticsFrame *previous{};
    bool previous_validation{};
    auto index = (next_frame_ + frames_.size() - frame_count_) % frames_.size();
    for (std::size_t count = 0; count < frame_count_; ++count)
    {
        const auto &frame = frames_[index];
        index = (index + 1) % frames_.size();
        if (!frame.fence_completed || !frame.presented) continue;
        output.newest_presented_frame_id = frame.rendered_frame_id;
        const bool validation = frame.validation_enabled || frame.gpu_validation_enabled;
        if (!frame.submit_qpc || !frame.qpc_frequency) { previous = nullptr; continue; }
        const auto validation_index = validation ? 1u : 0u;
        if (previous && previous_validation == validation &&
            frame.view_render_sequence == previous->view_render_sequence + 1 &&
            frame.qpc_frequency == previous->qpc_frequency && frame.submit_qpc > previous->submit_qpc)
        {
            elapsed[validation_index] += static_cast<double>(frame.submit_qpc - previous->submit_qpc) * 1000.0 /
                static_cast<double>(frame.qpc_frequency);
            ++intervals[validation_index];
        }
        previous = &frame;
        previous_validation = validation;
    }
    if (intervals[1] && elapsed[1] > 0.0)
        output.validation_on_fps = static_cast<double>(intervals[1]) * 1000.0 / elapsed[1];
    if (intervals[0] && elapsed[0] > 0.0)
        output.validation_off_fps = static_cast<double>(intervals[0]) * 1000.0 / elapsed[0];
    output.transition_peak_local_usage_bytes = transition_peak_local_usage_bytes_;
    return output;
}
#endif

#if defined(HS_DEVELOPMENT_TOOLS)
RendererDiagnosticsMemory D3D12Renderer::Impl::DiagnosticsMemory() const
{
    RendererDiagnosticsMemory output;
    const auto accounting = residency.Accounting();
    static_assert(static_cast<std::size_t>(RendererDiagnosticsMemoryCategory::Count) == static_cast<std::size_t>(ResidencyCategory::Count));
    output.live_bytes = accounting.live_bytes; output.retiring_bytes = accounting.retiring_bytes;
    output.local_live_bytes = accounting.local_live_bytes; output.local_retiring_bytes = accounting.local_retiring_bytes;
    output.nonlocal_live_bytes = accounting.nonlocal_live_bytes; output.nonlocal_retiring_bytes = accounting.nonlocal_retiring_bytes;
    output.tracked_live_bytes = accounting.live_total; output.tracked_retiring_bytes = accounting.retiring_total;
    output.tracked_reclaimed_bytes = accounting.reclaimed_total;
    output.tracked_gpu_bytes = accounting.live_total + accounting.retiring_total;
    output.host_staging_bytes = accounting.host_staging_total; output.host_reserved_bytes = accounting.host_reserved_total;
    output.gpu_reserved_bytes = accounting.gpu_reserved_total; output.gpu_growth_since_budget_bytes = accounting.gpu_growth_since_snapshot;
    output.local_gpu_reserved_bytes = accounting.gpu_reserved_by_segment[0];
    output.nonlocal_gpu_reserved_bytes = accounting.gpu_reserved_by_segment[1];
    output.local_growth_since_budget_bytes = accounting.local_growth_since_snapshot;
    output.nonlocal_growth_since_budget_bytes = accounting.nonlocal_growth_since_snapshot;
    output.host_budget_available = accounting.budget.host_budget_valid;
    output.host_staging_budget_bytes = accounting.budget.host_staging_budget;
    const auto age_ms = [](auto observed_at) -> std::uint64_t {
        if (observed_at == std::chrono::steady_clock::time_point{}) return 0;
        return static_cast<std::uint64_t>(std::max<std::int64_t>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - observed_at).count()));
    };
    const auto status = [](bool valid, bool supported, HRESULT result, std::uint64_t age) {
        using Status = RendererDiagnosticsMemory::ObservationStatus;
        if (!supported) return Status::Unsupported;
        if (!valid) return FAILED(result) ? Status::Failed : Status::Unavailable;
        if (FAILED(result)) return Status::Failed;
        return age <= 1000 ? Status::Fresh : Status::Cached;
    };
    output.budget_available = local_budget_observation.valid;
    output.local_usage_bytes = local_budget_observation.last_valid.CurrentUsage;
    output.local_budget_bytes = local_budget_observation.last_valid.Budget;
    output.local_success_sequence = local_budget_observation.success_sequence;
    output.local_observed_age_ms = age_ms(local_budget_observation.observed_at);
    output.local_last_error = static_cast<std::int32_t>(local_budget_observation.last_result);
    output.local_status = status(local_budget_observation.valid, local_budget_observation.supported,
        local_budget_observation.last_result, output.local_observed_age_ms);
    output.nonlocal_budget_available = nonlocal_budget_observation.valid;
    output.nonlocal_usage_bytes = nonlocal_budget_observation.last_valid.CurrentUsage;
    output.nonlocal_budget_bytes = nonlocal_budget_observation.last_valid.Budget;
    output.nonlocal_success_sequence = nonlocal_budget_observation.success_sequence;
    output.nonlocal_observed_age_ms = age_ms(nonlocal_budget_observation.observed_at);
    output.nonlocal_last_error = static_cast<std::int32_t>(nonlocal_budget_observation.last_result);
    output.nonlocal_status = status(nonlocal_budget_observation.valid, nonlocal_budget_observation.supported,
        nonlocal_budget_observation.last_result, output.nonlocal_observed_age_ms);
    output.host_budget_available = host_budget_observation.valid;
    output.host_success_sequence = host_budget_observation.success_sequence;
    output.host_observed_age_ms = age_ms(host_budget_observation.observed_at);
    output.host_last_error = static_cast<std::int32_t>(host_budget_observation.last_error);
    output.host_status = !host_budget_observation.last_attempt_succeeded
        ? (host_budget_observation.attempted_at == std::chrono::steady_clock::time_point{}
            ? RendererDiagnosticsMemory::ObservationStatus::Unavailable
            : RendererDiagnosticsMemory::ObservationStatus::Failed)
        : output.host_observed_age_ms <= 1000 ? RendererDiagnosticsMemory::ObservationStatus::Fresh
                                              : RendererDiagnosticsMemory::ObservationStatus::Cached;
    output.allocator_local_block_bytes = accounting.allocator_stats.local_block_bytes;
    output.allocator_nonlocal_block_bytes = accounting.allocator_stats.nonlocal_block_bytes;
    output.allocator_local_usage_estimate_bytes = accounting.allocator_stats.local_usage_bytes;
    output.allocator_nonlocal_usage_estimate_bytes = accounting.allocator_stats.nonlocal_usage_bytes;
    output.allocator_stats_available = accounting.allocator_stats_available;
    output.texture_physical_bytes = texture_streaming.PhysicalBytes();
    output.presentation_provider_bytes = sdk_presentation.Status().allocated_bytes + presentation_guides.AllocatedBytes();
    const auto add_guides = [&](const OwnedView &owner) {
        const auto bytes = owner.reconstruction_guides.AllocatedBytes();
        output.reconstruction_guide_bytes = bytes > UINT64_MAX - output.reconstruction_guide_bytes
            ? UINT64_MAX : output.reconstruction_guide_bytes + bytes;
    };
    for (const auto &entry : owned_views) add_guides(*entry.second);
    for (const auto &owner : unresolved_view_candidates) add_guides(*owner);
    return output;
}
RendererDiagnosticsSnapshot D3D12Renderer::GetDiagnostics()
{
    if (!impl_) return {};
    if (impl_->config.capture_timing_stream)
        for (const auto &frame : impl_->frames)
            if (frame.timestamps_recorded && !impl_->diagnostics.HasCompleted(frame.submitted_frame_id, frame.submitted_view_id) &&
                frame.completion.IsSatisfiedBy(impl_->graph_queue_executor.Completed()))
                impl_->diagnostics.RecordCompleted(impl_->ReadFrameGpuTimings(frame));
    auto output = impl_->diagnostics.Snapshot();
    output.current_memory = impl_->DiagnosticsMemory();
    output.reconstruction = GetReconstructionStatus();
    return output;
}
#endif

Result D3D12Renderer::Impl::CheckDevice(HRESULT result, std::string_view operation)
{
    if (SUCCEEDED(result))
    {
        return Result::Success();
    }
    (void)CountValidationErrors();
    const auto reason = device ? device->GetDeviceRemovedReason() : result;
    if (FAILED(reason) ||
        result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        WriteDred(reason);
    }
    return HResultFailure(operation, result);
}


Result D3D12Renderer::Impl::CountValidationErrors()
{
#if defined(HS_DEVELOPMENT_TOOLS)
    if (!config.validation && !config.gpu_validation) return Result::Success();
    validation_status.message_collection_attempted = true;
    const auto failed = [&](std::string_view operation, HRESULT result) {
        validation_status.message_collection_hresult = result;
        return HResultFailure(operation, result);
    };
    if (FAILED(validation_status.message_collection_hresult))
        return HResultFailure("D3D12 validation collection is incomplete",
                              validation_status.message_collection_hresult);
    auto *info_queue = validation_info_queue.Get();
    if (!info_queue) return failed("D3D12 validation message queue unavailable", E_NOINTERFACE);
    if (info_queue->GetNumMessagesDiscardedByMessageCountLimit() != 0)
        return failed("D3D12 validation messages exceeded the queue limit", E_FAIL);
    const auto count = info_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
    if (count == 0) return Result::Success();
    std::vector<std::byte> storage;
    std::ofstream log;
    for (std::uint64_t index = 0; index < count; ++index)
    {
        SIZE_T size{};
        if (const auto result = info_queue->GetMessage(index, nullptr, &size); FAILED(result))
            return failed("Read D3D12 validation message size", result);
        if (size < sizeof(D3D12_MESSAGE))
            return failed("D3D12 validation message is incomplete", E_FAIL);
        storage.resize(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (const auto result = info_queue->GetMessage(index, message, &size); FAILED(result))
            return failed("Read D3D12 validation message", result);
        if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
            message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        {
            ++validation_errors;
            if (!log.is_open())
                log.open(ErrorLogDirectory(config.artifact_directory) / "d3d12_validation.log", std::ios::app);
            log << "id=" << message->ID << " severity=" << message->Severity << ' '
                << message->pDescription << '\n';
        }
    }
    info_queue->ClearStoredMessages();
#endif
    return Result::Success();
}

void D3D12Renderer::Impl::WriteDred(HRESULT reason) const
{
#if defined(HS_DEVELOPMENT_TOOLS)
    std::ofstream stream(ErrorLogDirectory(config.artifact_directory) / "dred.txt", std::ios::trunc);
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
            // reporting only that a breadcrumb list exists. Prioritize active work
            // while keeping the number of detailed nodes bounded.
            std::size_t in_progress_count{}, pending_count{}, completed_count{};
            std::size_t total_nodes{};
            for (auto *node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext)
            {
                const auto completed = node->pLastBreadcrumbValue
                    ? std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount) : 0u;
                if (completed > 0 && completed < node->BreadcrumbCount) ++in_progress_count;
                else if (completed == 0 && completed < node->BreadcrumbCount) ++pending_count;
                else ++completed_count;
                ++total_nodes;
            }
            constexpr std::size_t max_emitted_nodes = 128;
            stream << "breadcrumb_nodes_total=" << total_nodes
                   << " in_progress=" << in_progress_count
                   << " pending=" << pending_count
                   << " completed=" << completed_count
                   << " emitted=" << std::min(total_nodes, max_emitted_nodes) << '\n';
            const auto emit_node = [&](const auto *node, std::size_t node_index,
                                       std::uint32_t completed) {
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
            };
            std::size_t emitted_nodes{};
            for (std::uint32_t priority = 0; priority < 3 && emitted_nodes < max_emitted_nodes; ++priority)
            {
                std::size_t node_index{};
                for (auto *node = breadcrumbs.pHeadAutoBreadcrumbNode;
                     node && emitted_nodes < max_emitted_nodes;
                     node = node->pNext, ++node_index)
                {
                    const auto completed = node->pLastBreadcrumbValue
                        ? std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount) : 0u;
                    const auto node_priority = completed > 0 && completed < node->BreadcrumbCount ? 0u
                        : completed == 0 && completed < node->BreadcrumbCount ? 1u : 2u;
                    if (node_priority != priority) continue;
                    emit_node(node, node_index, completed);
                    ++emitted_nodes;
                }
            }
            if (emitted_nodes < total_nodes) stream << "breadcrumb_nodes_truncated=1\n";
        }
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&page_fault)))
        {
            stream << "page_fault_va=" << page_fault.PageFaultVA << '\n';
        }
    }
#else
    static_cast<void>(reason);
#endif
}


} // namespace hs
