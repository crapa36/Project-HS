#include "renderer_impl.hpp"
#include "renderer_resident_assets.hpp"


#include <algorithm>

namespace hs
{
namespace
{
Result ResidentFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}

bool SubmissionFinished(const ResidentUtilitySubmission &submission) noexcept
{
    // GetCompletedValue returns UINT64_MAX for device removal, not a timeline
    // completion. Query the device explicitly to prove that terminal condition.
    if (submission.device && FAILED(submission.device->GetDeviceRemovedReason())) return true;
    if (!submission.signaled || !submission.fence) return false;
    const auto completed = submission.fence->GetCompletedValue();
    return completed != UINT64_MAX && completed >= submission.fence_value;
}
} // namespace

Result D3D12Renderer::Impl::SubmitResidentUtilityUpload(ID3D12GraphicsCommandList *list,
    ID3D12CommandAllocator *command_allocator, std::vector<AllocationResource> &uploads)
{
    CollectResidentUtilityUploads();
    if (!list || !command_allocator || !queue || !fence || !fence_event ||
        HasUnresolvedResidentUtilityUploads() || next_fence == UINT64_MAX)
        return ResidentFailure("Utility upload requires an available owned command context and fence timeline.");
    ResidentUtilitySubmission submission;
    submission.allocator = command_allocator;
    submission.list = list;
    submission.device = device;
    submission.queue = queue;
    submission.fence = fence;
    submission.fence_value = next_fence++;
    // Allocate the lease slot before moving the uploads or submitting work.
    resident_utility_submissions.emplace_back(std::move(submission));
    auto &retained = resident_utility_submissions.back();
    retained.uploads = std::move(uploads);
    ID3D12CommandList *lists[]{list};
    StartupOperationScope submit_timing(RendererStartupOperation::UploadSubmit);
    queue->ExecuteCommandLists(1, lists);
    auto hr = queue->Signal(fence.Get(), retained.fence_value);
    submit_timing.Stop();
    if (FAILED(hr))
    {
        frame_submission_faulted = true;
        return CheckDevice(hr, "Signal resident utility upload completion");
    }
    retained.signaled = true;
    StartupOperationScope wait_timing(RendererStartupOperation::GpuCompletionAdmission);
    if (fence->GetCompletedValue() < retained.fence_value)
    {
        hr = fence->SetEventOnCompletion(retained.fence_value, fence_event);
        if (FAILED(hr))
        {
            frame_submission_faulted = true;
            return HResultFailure("Set resident utility upload completion event", hr);
        }
        const auto wait = MeasureStartupOperation(RendererStartupOperation::GpuFenceWait, [&] { return WaitForSingleObject(fence_event, INFINITE); });
        if (wait != WAIT_OBJECT_0)
        {
            frame_submission_faulted = true;
            return HResultFailure("Wait for resident utility upload completion",
                wait == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL);
        }
    }
    wait_timing.Stop();
    if (FAILED(device->GetDeviceRemovedReason()))
    {
        frame_submission_faulted = true;
        return CheckDevice(device->GetDeviceRemovedReason(), "Complete resident utility upload");
    }
    if (!SubmissionFinished(retained))
    {
        frame_submission_faulted = true;
        return ResidentFailure("Utility upload event returned without its post-work fence completion.");
    }
    CollectResidentUtilityUploads();
    return Result::Success();
}

void D3D12Renderer::Impl::CollectResidentUtilityUploads() noexcept
{
    std::erase_if(resident_utility_submissions, [&](const auto &submission) {
        if (!SubmissionFinished(submission)) return false;
        return true;
    });
}

bool D3D12Renderer::Impl::HasUnresolvedResidentUtilityUploads() const noexcept
{
    return std::ranges::any_of(resident_utility_submissions,
        [](const auto &submission) { return !SubmissionFinished(submission); });
}

} // namespace hs
