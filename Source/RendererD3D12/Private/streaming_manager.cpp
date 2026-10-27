#include "streaming_manager.hpp"

#include <limits>

namespace hs
{

void StreamingManager::Reset() noexcept
{
    pages_.clear();
}

StreamingRequestId StreamingManager::Request(std::string asset_id, std::string page_id,
                                             std::string content_generation,
                                             std::uint32_t priority)
{
    if (asset_id.empty() || page_id.empty() || content_generation.empty() ||
        next_request_ == std::numeric_limits<std::uint64_t>::max())
        return {};
    const auto request = StreamingRequestId{next_request_++};
    pages_.emplace(request.value,
                   StreamingPageSnapshot{request, std::move(asset_id), std::move(page_id),
                                         std::move(content_generation), priority,
                                         StreamingPageState::Requested, {}, false});
    return request;
}

bool StreamingManager::BeginRead(StreamingRequestId request) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state != StreamingPageState::Requested)
        return false;
    found->second.state = StreamingPageState::Reading;
    return true;
}

bool StreamingManager::UpdatePriority(StreamingRequestId request, std::uint32_t priority) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state == StreamingPageState::Retiring ||
        found->second.state == StreamingPageState::Cancelled) return false;
    found->second.priority = priority;
    return true;
}

bool StreamingManager::SubmitRead(StreamingRequestId request) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state != StreamingPageState::Reading || found->second.read_submitted)
        return false;
    found->second.read_submitted = true;
    return true;
}

bool StreamingManager::FinishRead(StreamingRequestId request) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || !found->second.read_submitted || found->second.read_finished) return false;
    found->second.read_finished = true;
    return true;
}

bool StreamingManager::CompleteRead(StreamingRequestId request,
                                    std::string_view content_generation) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state != StreamingPageState::Reading ||
        (found->second.read_submitted && !found->second.read_finished))
        return false;
    if (found->second.content_generation != content_generation)
    {
        found->second.state = StreamingPageState::Cancelled;
        return false;
    }
    found->second.state = StreamingPageState::Decoded;
    return true;
}

bool StreamingManager::BeginUpload(StreamingRequestId request,
                                   std::string_view content_generation) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state != StreamingPageState::Decoded ||
        found->second.content_generation != content_generation)
        return false;
    found->second.state = StreamingPageState::Uploading;
    return true;
}

bool StreamingManager::BeginMappingOnly(StreamingRequestId request,
                                       std::string_view content_generation) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state != StreamingPageState::Requested ||
        found->second.content_generation != content_generation)
        return false;
    found->second.state = StreamingPageState::Uploading;
    return true;
}

bool StreamingManager::SubmitUpload(StreamingRequestId request,
                                    std::string_view content_generation,
                                    const CompletionSet &completion) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state != StreamingPageState::Uploading ||
        found->second.content_generation != content_generation ||
        (!completion.direct && !completion.compute && !completion.copy))
        return false;
    found->second.completion = completion;
    found->second.upload_submitted = true;
    return true;
}

bool StreamingManager::TryPublish(StreamingRequestId request,
                                  std::string_view content_generation,
                                  const CompletionSet &completed) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state != StreamingPageState::Uploading)
        return false;
    if (found->second.content_generation != content_generation)
    {
        found->second.state = StreamingPageState::Cancelled;
        return false;
    }
    if (!found->second.upload_submitted) return false;
    if (!found->second.completion.IsSatisfiedBy(completed)) return false;
    found->second.state = StreamingPageState::Resident;
    return true;
}

bool StreamingManager::Retire(StreamingRequestId request,
                              const CompletionSet &completion) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state != StreamingPageState::Resident)
        return false;
    found->second.state = StreamingPageState::Retiring;
    found->second.completion.Merge(completion);
    return true;
}

bool StreamingManager::Cancel(StreamingRequestId request) noexcept
{
    const auto found = pages_.find(request.value);
    if (found == pages_.end() || found->second.state == StreamingPageState::Retiring)
        return false;
    found->second.state = StreamingPageState::Cancelled;
    return true;
}

std::uint32_t StreamingManager::Collect(const CompletionSet &completed) noexcept
{
    std::uint32_t count = 0;
    for (auto it = pages_.begin(); it != pages_.end();)
    {
        const auto reclaim =
            (it->second.state == StreamingPageState::Cancelled ||
             it->second.state == StreamingPageState::Retiring) &&
            (!it->second.read_submitted || it->second.read_finished) &&
            it->second.completion.IsSatisfiedBy(completed);
        if (!reclaim)
        {
            ++it;
            continue;
        }
        it = pages_.erase(it);
        ++count;
    }
    return count;
}

const StreamingPageSnapshot *StreamingManager::Find(StreamingRequestId request) const noexcept
{
    const auto found = pages_.find(request.value);
    return found == pages_.end() ? nullptr : &found->second;
}

} // namespace hs
