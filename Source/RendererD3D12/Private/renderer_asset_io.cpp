#include "renderer_asset_io.hpp"
#include "checked_binary_read.hpp"
#include <hs/core/sha256.hpp>

#include <Windows.h>
#include <wrl/client.h>
#if HS_WITH_DIRECTSTORAGE
#include <dstorage.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace hs
{
namespace
{
Result IoFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}
struct EventOwner
{
    HANDLE value{};
    ~EventOwner() { if (value) CloseHandle(value); }
};
#if HS_WITH_DIRECTSTORAGE
using Microsoft::WRL::ComPtr;
struct NativeTransport
{
    ComPtr<IDStorageFactory> factory;
    ComPtr<IDStorageQueue1> queue;
    ComPtr<IDStorageStatusArray> statuses;
    std::array<bool, 256> occupied{};
    std::uint64_t next_tag{1};
    std::uint64_t submit_generation{};
    std::uint32_t chunk_bytes{}, staging_bytes{};
    bool queued_any{};
    HRESULT last_error{S_OK};
    void ReadErrors() noexcept
    {
        if (WaitForSingleObject(queue->GetErrorEvent(), 0) == WAIT_OBJECT_0)
        {
            DSTORAGE_ERROR_RECORD record{};
            queue->RetrieveErrorRecord(&record);
            last_error = record.FirstFailure.HResult;
        }
    }
};
#endif
struct ReadOwner
{
    ImmutableAssetRead input;
    ResidencyTicket storage;
    std::vector<std::byte> bytes;
    std::future<std::vector<std::byte>> future;
    std::future<void> prepare_future;
    std::shared_ptr<std::atomic_bool> cancellation{std::make_shared<std::atomic_bool>()};
    EventOwner event;
    bool cancelled{}, submitted{};
#if HS_WITH_DIRECTSTORAGE
    std::shared_ptr<NativeTransport> transport;
    ComPtr<IDStorageFile> file;
    std::uint32_t status{};
    std::uint64_t tag{};
    std::uint64_t transport_generation{};
#endif
    ~ReadOwner()
    {
        // Release of the queue/file/destination/event is never cancellation.
        // This owner outlives every submitted native marker or async future.
#if HS_WITH_DIRECTSTORAGE
        if (transport && (submitted || transport_generation))
        {
            if (transport_generation > transport->submit_generation)
            {
                transport->queue->Submit();
                transport->submit_generation = transport_generation;
                transport->queued_any = false;
            }
            WaitForSingleObject(event.value, INFINITE);
            transport->occupied[status] = false;
        }
#endif
        if (future.valid()) future.wait();
        if (prepare_future.valid()) prepare_future.wait();
    }
};
} // namespace
struct RendererAssetIo::Impl
{
    std::unordered_map<std::uint64_t, std::unique_ptr<ReadOwner>> reads;
    bool initialized{};
    ResidencyTicket transport_ticket;
#if HS_WITH_DIRECTSTORAGE
    std::shared_ptr<NativeTransport> transport;
#endif
};
RendererAssetIo::RendererAssetIo() : impl_(std::make_unique<Impl>()) {}
RendererAssetIo::~RendererAssetIo() = default;
bool RendererAssetIo::HasPendingReads() const noexcept { return !impl_->reads.empty(); }
void RendererAssetIo::SetWaitForReads(bool wait) noexcept { wait_for_reads_ = wait; }
bool RendererAssetIo::NativeEnabled() const noexcept
{
#if HS_WITH_DIRECTSTORAGE
    return static_cast<bool>(impl_->transport);
#else
    return false;
#endif
}
Result RendererAssetIo::Initialize(bool native_enabled, std::uint32_t staging_bytes,
                                   ResidencyManager &residency)
{
    if (impl_->initialized || !impl_->reads.empty())
        return IoFailure("Asset IO transport was already initialized.");
    if (!native_enabled) { impl_->initialized = true; return Result::Success(); }
#if HS_WITH_DIRECTSTORAGE
    if (!staging_bytes) return IoFailure("DirectStorage requires a bounded staging reservation.");
    const auto ticket = residency.ReserveHost(staging_bytes);
    if (!ticket) return IoFailure("DirectStorage staging cannot fit the shared host budget.");
    auto transport = std::make_shared<NativeTransport>();
    auto hr = DStorageGetFactory(IID_PPV_ARGS(&transport->factory));
    if (SUCCEEDED(hr)) hr = transport->factory->SetStagingBufferSize(staging_bytes);
    DSTORAGE_QUEUE_DESC description{};
    description.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    description.Capacity = 1024;
    description.Priority = DSTORAGE_PRIORITY_NORMAL;
    description.Name = "HS immutable asset reads";
    if (SUCCEEDED(hr)) hr = transport->factory->CreateQueue(&description, IID_PPV_ARGS(&transport->queue));
    if (SUCCEEDED(hr)) hr = transport->factory->CreateStatusArray(256, "HS asset status", IID_PPV_ARGS(&transport->statuses));
    if (FAILED(hr) || !residency.CommitHost(ticket, staging_bytes))
    {
        static_cast<void>(residency.ReleaseNow(ticket));
        return IoFailure("DirectStorage transport initialization failed: " + std::to_string(hr));
    }
    transport->chunk_bytes = std::min(staging_bytes, 8u * 1024u * 1024u);
    transport->staging_bytes = staging_bytes;
    impl_->transport_ticket = ticket;
    impl_->transport = std::move(transport);
    impl_->initialized = true;
    return Result::Success();
#else
    static_cast<void>(staging_bytes); static_cast<void>(residency);
    return IoFailure("Native DirectStorage was requested without its pinned SDK build.");
#endif
}
Result RendererAssetIo::ShareTransportFrom(const RendererAssetIo &other, ResidencyManager &residency)
{
    if (impl_->initialized || !impl_->reads.empty() || !other.impl_->initialized)
        return IoFailure("Resident asset IO candidate was already initialized.");
#if HS_WITH_DIRECTSTORAGE
    if (other.impl_->transport)
    {
        const auto size = other.impl_->transport->staging_bytes;
        const auto ticket = residency.ReserveHost(size);
        if (!ticket || !residency.CommitHost(ticket, size))
        {
            if (ticket) static_cast<void>(residency.ReleaseNow(ticket));
            return IoFailure("Resident asset IO transport accounting exceeds the shared host budget.");
        }
        // The candidate ledger replaces the old ledger at publication. Both
        // refer to one retained native transport, not two staging buffers.
        impl_->transport_ticket = ticket;
    }
    impl_->transport = other.impl_->transport;
#else
    static_cast<void>(residency);
#endif
    impl_->initialized = other.impl_->initialized;
    return Result::Success();
}
Result RendererAssetIo::Enqueue(ImmutableAssetRead input, StreamingManager &streaming,
                                ResidencyManager &residency, ResidencyTicket storage, bool &submitted,
                                bool defer_native_submit)
{
    submitted = false;
    const auto *page = streaming.Find(input.request);
    if (!impl_->initialized || !page || page->state != StreamingPageState::Requested ||
        page->asset_id != input.logical_id || page->content_generation != input.generation ||
        input.logical_id.empty() || input.source.empty() || !input.generation_lease || !input.expected_bytes ||
        input.expected_bytes > std::numeric_limits<std::size_t>::max() ||
        input.expected_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        !storage || !residency.Contains(storage) || impl_->reads.contains(input.request.value))
        return IoFailure("Immutable asset read identity, lease or reservation is invalid.");
    if (input.ranges.empty() && input.source.range.byte_count)
    {
        std::uint64_t bytes{};
        if (auto result = input.source.ByteCount(bytes); !result) return result;
        if (bytes != input.expected_bytes) return IoFailure("Immutable pack range differs from its reservation.");
    }
    if (!input.ranges.empty())
    {
        std::uint64_t product_bytes{}, total{};
        if (auto result = input.source.ByteCount(product_bytes); !result) return result;
        for (const auto &range : input.ranges)
        {
            if (!range.byte_count || range.offset > product_bytes ||
                range.byte_count > product_bytes - range.offset || total > UINT64_MAX - range.byte_count)
                return IoFailure("Immutable asset subresource range exceeds its admitted product.");
            total += range.byte_count;
        }
        if (total != input.expected_bytes)
            return IoFailure("Immutable asset range total differs from its host reservation.");
    }
    auto read = std::make_unique<ReadOwner>();
    read->input = std::move(input);
    read->storage = storage;
#if HS_WITH_DIRECTSTORAGE
    // DirectStorage's file handle denies atomic replacement; packed input already owns its native Core file.
    if (impl_->transport && !read->input.source.pack)
    {
        auto &transport = *impl_->transport;
        std::uint64_t product_bytes{};
        if (auto result = read->input.source.ByteCount(product_bytes); !result) return result;
        if (read->input.ranges.empty() && product_bytes != read->input.expected_bytes)
            return IoFailure("DirectStorage immutable file size differs from its read reservation.");
        std::uint64_t chunks{};
        if (read->input.ranges.empty()) chunks = (product_bytes - 1u) / transport.chunk_bytes + 1u;
        else for (const auto &range : read->input.ranges)
        {
            const auto count = (static_cast<std::uint64_t>(range.byte_count) - 1u) / transport.chunk_bytes + 1u;
            if (chunks > UINT64_MAX - count) return IoFailure("DirectStorage subresource request count overflow.");
            chunks += count;
        }
        DSTORAGE_QUEUE_INFO info{};
        transport.queue->Query(&info);
        if (chunks + 2u >= info.Desc.Capacity)
            return IoFailure("Immutable asset read exceeds the bounded DirectStorage queue capacity.");
        const auto free_status = std::find(transport.occupied.begin(), transport.occupied.end(), false);
        if (free_status == transport.occupied.end() || chunks + 2u > info.EmptySlotCount)
            return Result::Success(); // Requested remains eligible for the next pump.
        if (transport.next_tag == UINT64_MAX) return IoFailure("DirectStorage cancellation tag exhausted.");
        if (transport.submit_generation == UINT64_MAX) return IoFailure("DirectStorage batch generation exhausted.");
        auto hr = transport.factory->OpenFile(read->input.source.path.c_str(), IID_PPV_ARGS(&read->file));
        BY_HANDLE_FILE_INFORMATION file_info{};
        if (SUCCEEDED(hr)) hr = read->file->GetFileInformation(&file_info);
        const auto actual_size = (static_cast<std::uint64_t>(file_info.nFileSizeHigh) << 32u) | file_info.nFileSizeLow;
        if (FAILED(hr) || actual_size != product_bytes)
            return IoFailure("DirectStorage immutable file size/open validation failed.");
        read->bytes.resize(static_cast<std::size_t>(read->input.expected_bytes));
        read->event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!read->event.value) return IoFailure("DirectStorage read completion event creation failed.");
        read->status = static_cast<std::uint32_t>(free_status - transport.occupied.begin());
        read->tag = transport.next_tag++;
        read->transport = impl_->transport;
        const auto key = read->input.request.value;
        auto *retained = read.get();
        // Map insertion/allocation happens before EnqueueRequest can auto-submit.
        impl_->reads.emplace(key, std::move(read));
        if (!streaming.BeginRead({key}) || !streaming.SubmitRead({key}))
        { impl_->reads.erase(key); return IoFailure("DirectStorage request state transition failed."); }
        transport.occupied[retained->status] = true;
        submitted = true;
        const auto enqueue_range = [&](std::uint64_t source_offset, std::size_t destination_offset,
                                       std::uint64_t range_bytes) {
            for (std::uint64_t offset = 0; offset < range_bytes;)
            {
                const auto bytes = static_cast<UINT32>(std::min<std::uint64_t>(transport.chunk_bytes, range_bytes - offset));
                DSTORAGE_REQUEST request{};
                request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
                request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
                request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
                request.Source.File = {retained->file.Get(), source_offset + offset, bytes};
                request.Destination.Memory = {retained->bytes.data() + destination_offset + offset, bytes};
                request.CancellationTag = retained->tag;
                request.Name = retained->input.logical_id.c_str();
                transport.queue->EnqueueRequest(&request);
                offset += bytes;
            }
        };
        if (retained->input.ranges.empty()) enqueue_range(0, 0, product_bytes);
        else
        {
            std::size_t destination{};
            for (const auto &range : retained->input.ranges)
            {
                enqueue_range(range.offset, destination, range.byte_count);
                destination += range.byte_count;
            }
        }
        transport.queue->EnqueueStatus(transport.statuses.Get(), retained->status);
        transport.queue->EnqueueSetEvent(retained->event.value);
        retained->transport_generation = transport.submit_generation + 1u;
        transport.queued_any = true;
        retained->submitted = !defer_native_submit;
        if (!defer_native_submit) SubmitQueuedReads();
        return Result::Success();
    }
#endif
    const auto source = read->input.source;
    const auto expected = read->input.expected_bytes;
    const auto key = read->input.request.value;
    auto *retained = read.get();
    impl_->reads.emplace(key, std::move(read));
    if (!streaming.BeginRead({key}) || !streaming.SubmitRead({key}))
    { impl_->reads.erase(key); return IoFailure("Async asset request state transition failed."); }
    try
    {
        const auto prepare = retained->input.prepare;
        const auto ranges = retained->input.ranges;
        const auto cancellation = retained->cancellation;
        retained->future = std::async(std::launch::async, [source, expected, prepare, ranges, cancellation] {
            std::vector<std::byte> bytes;
            if (ranges.empty())
            {
                if (auto result = source.Read(bytes, expected); !result)
                    throw std::runtime_error(std::string(result.Message()));
            }
            else
            {
                // shortcut: authenticated packed ranges are read serially per owner; upgrade when the retained pack transport can batch discontiguous verified reads.
                bytes.resize(static_cast<std::size_t>(expected));
                const auto base = source.pack ? source.range.offset : 0;
                std::size_t destination{};
                for (const auto &range : ranges)
                {
                    if (cancellation->load(std::memory_order_acquire)) break;
                    if (base > UINT64_MAX - range.offset)
                        throw std::runtime_error("Immutable asset range offset overflow.");
                    auto output = std::span<std::byte>(bytes).subspan(destination, range.byte_count);
                    if (auto result = source.ReadRange(base + range.offset, output, range.digest); !result)
                        throw std::runtime_error(std::string(result.Message()));
                    destination += range.byte_count;
                }
                if (!cancellation->load(std::memory_order_acquire) && destination != bytes.size())
                    throw std::runtime_error("Immutable asset ranges did not fill their admitted payload.");
            }
            if (prepare && !cancellation->load(std::memory_order_acquire)) prepare(bytes);
            return bytes;
        });
    }
    catch (const std::exception &error)
    {
        static_cast<void>(streaming.FinishRead({key}));
        static_cast<void>(streaming.Cancel({key}));
        impl_->reads.erase(key);
        return IoFailure("Could not submit immutable async asset read: " + std::string(error.what()));
    }
    retained->submitted = true;
    submitted = true;
    return Result::Success();
}
void RendererAssetIo::SubmitQueuedReads() noexcept
{
#if HS_WITH_DIRECTSTORAGE
    const auto &transport = impl_->transport;
    if (transport && transport->queued_any)
    {
        transport->queue->Submit();
        ++transport->submit_generation;
        transport->queued_any = false;
    }
#endif
}
Result RendererAssetIo::Poll(StreamingRequestId request, StreamingManager &streaming,
                             ResidencyManager &residency, AssetReadCompletion &output)
{
    output = {};
    const auto found = impl_->reads.find(request.value);
    if (found == impl_->reads.end()) return IoFailure("Asset read owner is missing.");
    auto &read = *found->second;
    const auto *page = streaming.Find(request);
    read.cancelled = read.cancelled || !page || page->state == StreamingPageState::Cancelled ||
        page->content_generation != read.input.generation;
    if (read.cancelled) read.cancellation->store(true, std::memory_order_release);
    HRESULT hr = S_OK;
#if HS_WITH_DIRECTSTORAGE
    if (read.transport)
    {
        read.transport->ReadErrors();
        if (wait_for_reads_)
        {
            const auto event_result = WaitForSingleObject(read.event.value, INFINITE);
            if (event_result != WAIT_OBJECT_0)
                return IoFailure("DirectStorage completion event wait failed.");
            if (!read.transport->statuses->IsComplete(read.status))
                return IoFailure("DirectStorage completion event preceded its status marker.");
        }
        else if (!read.transport->statuses->IsComplete(read.status)) return Result::Success();
        hr = read.transport->statuses->GetHResult(read.status);
        // Status completion precedes the event marker. The event must also
        // complete before its handle/queue slot can be reused or destroyed.
        if (!wait_for_reads_)
        {
            const auto event_result = WaitForSingleObject(read.event.value, 0);
            if (event_result == WAIT_TIMEOUT) return Result::Success();
            if (event_result != WAIT_OBJECT_0) return IoFailure("DirectStorage completion event wait failed.");
        }
        if (SUCCEEDED(hr) && !read.cancelled)
        {
            if (!read.prepare_future.valid())
            {
                const auto prepare = read.input.prepare;
                const auto ranges = read.input.ranges;
                const auto source_range = read.input.source.range;
                const auto cancellation = read.cancellation;
                auto *bytes = &read.bytes;
                read.prepare_future = std::async(std::launch::async, [prepare, ranges, source_range, cancellation, bytes] {
                    if (cancellation->load(std::memory_order_acquire)) return;
                    if (ranges.empty() && source_range.byte_count && Sha256(*bytes) != source_range.expected_digest)
                        throw std::runtime_error("Immutable file digest mismatch.");
                    if (!ranges.empty())
                    {
                        std::size_t offset{};
                        for (const auto &range : ranges)
                        {
                            if (range.byte_count > bytes->size() - offset ||
                                Sha256(std::span<const std::byte>(*bytes).subspan(offset, range.byte_count)) != range.digest)
                                throw std::runtime_error("Immutable subresource range digest mismatch.");
                            offset += range.byte_count;
                        }
                        if (offset != bytes->size()) throw std::runtime_error("Immutable subresource ranges are incomplete.");
                    }
                    if (!cancellation->load(std::memory_order_acquire) && prepare) prepare(*bytes);
                });
            }
        }
        if (read.prepare_future.valid())
        {
            if (wait_for_reads_) read.prepare_future.wait();
            else if (read.prepare_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return Result::Success();
            try { read.prepare_future.get(); }
            catch (const std::exception &error)
            {
                if (!read.cancelled)
                {
                    static_cast<void>(streaming.FinishRead(request));
                    static_cast<void>(streaming.Cancel(request));
                    static_cast<void>(residency.ReleaseNow(read.storage));
                    impl_->reads.erase(found);
                    return IoFailure("Immutable asset verification or preparation failed: " + std::string(error.what()));
                }
            }
        }
    }
    else
#endif
    {
        if (!read.future.valid()) return Result::Success();
        if (wait_for_reads_) read.future.wait();
        else if (read.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return Result::Success();
        try { read.bytes = read.future.get(); }
        catch (const std::exception &error)
        {
            static_cast<void>(streaming.FinishRead(request));
            static_cast<void>(streaming.Cancel(request));
            static_cast<void>(residency.ReleaseNow(read.storage));
            impl_->reads.erase(found);
            return IoFailure("Immutable asset read failed: " + std::string(error.what()));
        }
    }
    static_cast<void>(streaming.FinishRead(request));
    output.ready = true;
    output.cancelled = read.cancelled;
    if (FAILED(hr) || read.cancelled)
    {
        static_cast<void>(streaming.Cancel(request));
        static_cast<void>(residency.ReleaseNow(read.storage));
    }
    else
    {
        if (read.bytes.size() != read.input.expected_bytes || !residency.CommitHost(read.storage, read.bytes.capacity()))
        {
            static_cast<void>(streaming.Cancel(request));
            static_cast<void>(residency.ReleaseNow(read.storage));
            impl_->reads.erase(found);
            return IoFailure("Completed immutable read size or shared host accounting is invalid.");
        }
        output.bytes = std::move(read.bytes);
        output.storage = read.storage;
    }
    const bool failed = FAILED(hr) && !(read.cancelled && hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
    impl_->reads.erase(found);
    return failed ? IoFailure("DirectStorage read failed: " + std::to_string(hr)) : Result::Success();
}
void RendererAssetIo::Cancel(StreamingRequestId request, StreamingManager &streaming) noexcept
{
    static_cast<void>(streaming.Cancel(request));
    const auto found = impl_->reads.find(request.value);
    if (found == impl_->reads.end()) return;
    found->second->cancelled = true;
    found->second->cancellation->store(true, std::memory_order_release);
#if HS_WITH_DIRECTSTORAGE
    if (found->second->transport)
    {
        SubmitQueuedReads(); // Make a deferred owner visible to native cancellation/status tracking.
        found->second->transport->queue->CancelRequestsWithTag(UINT64_MAX, found->second->tag);
    }
#endif
}
void RendererAssetIo::CancelAll(StreamingManager &streaming) noexcept
{
    for (const auto &[id, read] : impl_->reads) { static_cast<void>(read); Cancel({id}, streaming); }
}
Result RendererAssetIo::CollectCancelled(StreamingManager &streaming, ResidencyManager &residency)
{
    for (auto iterator = impl_->reads.begin(); iterator != impl_->reads.end();)
    {
        const auto request = iterator->second->input.request;
        const auto *page = streaming.Find(request);
        const bool cancelled = iterator->second->cancelled || !page ||
            page->state == StreamingPageState::Cancelled ||
            page->content_generation != iterator->second->input.generation;
        ++iterator; // Poll may erase the retained owner, but never inserts one.
        if (!cancelled) continue;
        Cancel(request, streaming);
        AssetReadCompletion output;
        if (auto result = Poll(request, streaming, residency, output); !result) return result;
    }
    return Result::Success();
}
Result RendererAssetIo::DrainSubmitted(StreamingManager &streaming, ResidencyManager &residency)
{
    CancelAll(streaming);
    SubmitQueuedReads();
    while (!impl_->reads.empty())
    {
        auto &read = *impl_->reads.begin()->second;
#if HS_WITH_DIRECTSTORAGE
        if (read.transport && WaitForSingleObject(read.event.value, INFINITE) != WAIT_OBJECT_0)
            return IoFailure("DirectStorage drain did not establish terminal completion.");
#endif
        if (read.future.valid()) read.future.wait();
        AssetReadCompletion output;
        if (auto result = Poll(read.input.request, streaming, residency, output); !result) return result;
    }
    return Result::Success();
}
void RendererAssetIo::SwapState(RendererAssetIo &other) noexcept { impl_.swap(other.impl_); }
} // namespace hs
