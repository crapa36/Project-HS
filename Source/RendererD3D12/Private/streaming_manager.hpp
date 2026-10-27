#pragma once

#include "checked_binary_read.hpp"
#include "render_graph.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace hs
{

enum class StreamingPageState : std::uint8_t
{
    Requested,
    Reading,
    Decoded,
    Uploading,
    Resident,
    Retiring,
    Cancelled,
};

struct StreamingRequestId
{
    std::uint64_t value{};
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    friend bool operator==(StreamingRequestId, StreamingRequestId) = default;
};

struct StreamingPageSnapshot
{
    StreamingRequestId request;
    std::string asset_id; // Immutable product read by RendererAssetIo.
    std::string page_id;
    std::string content_generation;
    std::uint32_t priority{};
    StreamingPageState state{StreamingPageState::Cancelled};
    CompletionSet completion{};
    bool upload_submitted{};
    bool read_submitted{}, read_finished{};
};

class StreamingManager
{
  public:
    void Reset() noexcept;
    // page_id names a page or subresource within the immutable asset product.
    [[nodiscard]] StreamingRequestId Request(std::string asset_id, std::string page_id,
                                             std::string content_generation,
                                             std::uint32_t priority);
    [[nodiscard]] bool BeginRead(StreamingRequestId request) noexcept;
    [[nodiscard]] bool SubmitRead(StreamingRequestId request) noexcept;
    [[nodiscard]] bool FinishRead(StreamingRequestId request) noexcept;
    [[nodiscard]] bool UpdatePriority(StreamingRequestId request, std::uint32_t priority) noexcept;
    [[nodiscard]] bool CompleteRead(StreamingRequestId request,
                                    std::string_view content_generation) noexcept;
    [[nodiscard]] bool BeginUpload(StreamingRequestId request,
                                   std::string_view content_generation) noexcept;
    [[nodiscard]] bool BeginMappingOnly(StreamingRequestId request,
                                        std::string_view content_generation) noexcept;
    [[nodiscard]] bool SubmitUpload(StreamingRequestId request,
                                    std::string_view content_generation,
                                    const CompletionSet &completion) noexcept;
    [[nodiscard]] bool TryPublish(StreamingRequestId request,
                                  std::string_view content_generation,
                                  const CompletionSet &completed) noexcept;
    [[nodiscard]] bool Retire(StreamingRequestId request,
                              const CompletionSet &completion) noexcept;
    [[nodiscard]] bool Cancel(StreamingRequestId request) noexcept;
    std::uint32_t Collect(const CompletionSet &completed) noexcept;
    [[nodiscard]] const StreamingPageSnapshot *Find(StreamingRequestId request) const noexcept;

  private:
    std::unordered_map<std::uint64_t, StreamingPageSnapshot> pages_;
    std::uint64_t next_request_{1};
};

} // namespace hs
