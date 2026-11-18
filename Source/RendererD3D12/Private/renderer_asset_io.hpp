#pragma once

#include "residency_manager.hpp"
#include "streaming_manager.hpp"
#include "renderer_asset_source.hpp"

#include <filesystem>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hs
{
class AssetReadBuffer
{
  public:
    struct HeapDelete
    {
        void operator()(std::byte *) const noexcept;
    };

    AssetReadBuffer() = default;
    explicit AssetReadBuffer(std::size_t);
    AssetReadBuffer(const AssetReadBuffer &) = delete;
    AssetReadBuffer &operator=(const AssetReadBuffer &) = delete;
    AssetReadBuffer(AssetReadBuffer &&) noexcept;
    AssetReadBuffer &operator=(AssetReadBuffer &&) noexcept;
    ~AssetReadBuffer() = default;

    [[nodiscard]] std::byte *data() noexcept { return data_.get(); }
    [[nodiscard]] const std::byte *data() const noexcept { return data_.get(); }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    void Reset() noexcept;
    [[nodiscard]] std::span<std::byte> Bytes() noexcept { return {data(), size_}; }
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return {data(), size_}; }

  private:
    std::unique_ptr<std::byte[], HeapDelete> data_;
    std::size_t size_{};
};

struct ImmutableAssetRead
{
    StreamingRequestId request;
    std::string generation, logical_id;
    RendererAssetSource source;
    std::uint64_t expected_bytes{};
    std::shared_ptr<const void> generation_lease;
    std::function<void(std::span<const std::byte>)> prepare;
    std::vector<ImmutableAssetRange> ranges;
};
struct AssetReadCompletion
{
    bool ready{}, cancelled{};
    AssetReadBuffer bytes;
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
    [[nodiscard]] Result Enqueue(ImmutableAssetRead, StreamingManager &, ResidencyManager &,
                                 ResidencyTicket existing_storage_reservation, bool &submitted,
                                 bool defer_native_submit = false);
    void SubmitQueuedReads() noexcept;
    [[nodiscard]] Result Poll(StreamingRequestId, StreamingManager &, ResidencyManager &,
                              AssetReadCompletion &);
    void Cancel(StreamingRequestId, StreamingManager &) noexcept;
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
    [[nodiscard]] Result CompleteRead(StreamingRequestId, StreamingManager &, ResidencyManager &,
                                      AssetReadCompletion &, bool wait);
    bool wait_for_reads_{};
};
} // namespace hs
