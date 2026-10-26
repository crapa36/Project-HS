#pragma once

#include "residency_manager.hpp"
#include "streaming_manager.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hs
{
struct ImmutableAssetRead
{
    StreamingRequestId request;
    std::string generation, logical_id;
    std::filesystem::path path;
    std::uint64_t expected_bytes{};
    std::shared_ptr<const void> generation_lease;
    std::function<void(std::span<const std::byte>)> prepare;
};
struct AssetReadCompletion
{
    bool ready{}, cancelled{};
    std::vector<std::byte> bytes;
    ResidencyTicket storage;
};

// One owner for the existing async reads and optional native file->memory IO.
// No decoded package or GPU resource is published by this service.
class RendererAssetIo
{
  public:
    RendererAssetIo();
    ~RendererAssetIo();
    RendererAssetIo(const RendererAssetIo &) = delete;
    RendererAssetIo &operator=(const RendererAssetIo &) = delete;
    [[nodiscard]] Result Initialize(bool native_enabled, std::uint32_t staging_bytes,
                                    ResidencyManager &);
    // A resident candidate shares only the native transport. Its read owners,
    // request IDs, tickets and Generation leases remain independent.
    [[nodiscard]] Result ShareTransportFrom(const RendererAssetIo &, ResidencyManager &);
    [[nodiscard]] Result Enqueue(ImmutableAssetRead, StreamingManager &, ResidencyManager &,
                                 ResidencyTicket existing_storage_reservation, bool &submitted);
    [[nodiscard]] Result Poll(StreamingRequestId, StreamingManager &, ResidencyManager &,
                              AssetReadCompletion &);
    void Cancel(StreamingRequestId, StreamingManager &) noexcept;
    void CancelAll(StreamingManager &) noexcept;
    // Reclaim terminal cancelled owners without waiting or decoding them.
    [[nodiscard]] Result CollectCancelled(StreamingManager &, ResidencyManager &);
    [[nodiscard]] Result DrainSubmitted(StreamingManager &, ResidencyManager &);
    void SwapState(RendererAssetIo &) noexcept;
    void SetWaitForReads(bool) noexcept;
    [[nodiscard]] bool NativeEnabled() const noexcept;
    [[nodiscard]] bool HasPendingReads() const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool wait_for_reads_{};
};
} // namespace hs
