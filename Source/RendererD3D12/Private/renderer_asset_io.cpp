#include "renderer_asset_io.hpp"
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
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace hs
{
AssetReadBuffer::AssetReadBuffer(std::size_t size) : size_(size)
{
    if (size_)
    {
        data_.reset(static_cast<std::byte *>(HeapAlloc(GetProcessHeap(), 0, size_)));
        if (!data_) throw std::bad_alloc();
    }
}
AssetReadBuffer::AssetReadBuffer(AssetReadBuffer &&other) noexcept
    : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0))
{}
AssetReadBuffer &AssetReadBuffer::operator=(AssetReadBuffer &&other) noexcept
{
    if (this != &other)
    {
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}
void AssetReadBuffer::HeapDelete::operator()(std::byte *data) const noexcept
{
    HeapFree(GetProcessHeap(), 0, data);
}
void AssetReadBuffer::Reset() noexcept
{
    data_.reset();
    size_ = 0;
}
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
constexpr std::uint32_t kMaxConcurrentAssetCpuTasks = 32;
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
    std::uint32_t chunk_bytes{};
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
    AssetReadBuffer bytes;
    std::future<AssetReadBuffer> future;
    std::future<void> prepare_future;
    std::uint32_t *cpu_task_count{};
    // The owner joins its tasks before destroying their cancellation flag.
    std::atomic_bool cancelled{};
    EventOwner event;
#if HS_WITH_DIRECTSTORAGE
    NativeTransport *transport{};
    ComPtr<IDStorageFile> file;
    std::uint32_t status{};
    std::uint64_t tag{};
    std::uint64_t transport_generation{};
#endif
    void HoldCpuSlot(std::uint32_t &count) noexcept
    {
        cpu_task_count = &count;
        ++count;
    }
    ~ReadOwner()
    {
        // Release of the queue/file/destination/event is never cancellation.
        // This owner outlives every submitted native marker or async future.
#if HS_WITH_DIRECTSTORAGE
        if (transport_generation)
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
        if (cpu_task_count) --*cpu_task_count;
    }
};
} // namespace
struct RendererAssetIo::Impl
{
    std::uint32_t active_cpu_tasks{};
#if HS_WITH_DIRECTSTORAGE
    std::unique_ptr<NativeTransport> transport;
#endif
    std::unordered_map<std::uint64_t, std::unique_ptr<ReadOwner>> reads;
    bool initialized{};
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
    auto transport = std::make_unique<NativeTransport>();
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
    impl_->transport = std::move(transport);
    impl_->initialized = true;
    return Result::Success();
#else
    static_cast<void>(staging_bytes); static_cast<void>(residency);
    return IoFailure("Native DirectStorage was requested without its pinned SDK build.");
#endif
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
    if (input.ranges.empty() && (input.source.IsPacked() || input.source.HasAdmittedWholeFileProof()))
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
    const bool native_file_read =
#if HS_WITH_DIRECTSTORAGE
        impl_->transport && !input.source.IsPacked();
#else
        false;
#endif
    if (!native_file_read && impl_->active_cpu_tasks >= kMaxConcurrentAssetCpuTasks)
        return Result::Success(); // Requested remains eligible for a later pump.
    auto read = std::make_unique<ReadOwner>();
    read->input = std::move(input);
    read->storage = storage;
    if (!native_file_read) read->HoldCpuSlot(impl_->active_cpu_tasks);
#if HS_WITH_DIRECTSTORAGE
    // DirectStorage's file handle denies atomic replacement; packed input already owns its native Core file.
    if (native_file_read)
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
        auto hr = transport.factory->OpenFile(read->input.source.Loose().path.c_str(), IID_PPV_ARGS(&read->file));
        BY_HANDLE_FILE_INFORMATION file_info{};
        if (SUCCEEDED(hr)) hr = read->file->GetFileInformation(&file_info);
        const auto actual_size = (static_cast<std::uint64_t>(file_info.nFileSizeHigh) << 32u) | file_info.nFileSizeLow;
        if (FAILED(hr) || actual_size != product_bytes)
            return IoFailure("DirectStorage immutable file size/open validation failed.");
        read->bytes = AssetReadBuffer(static_cast<std::size_t>(read->input.expected_bytes));
        read->event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!read->event.value) return IoFailure("DirectStorage read completion event creation failed.");
        read->status = static_cast<std::uint32_t>(free_status - transport.occupied.begin());
        read->tag = transport.next_tag++;
        read->transport = impl_->transport.get();
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
        if (!defer_native_submit) SubmitQueuedReads();
        return Result::Success();
    }
#endif
    const auto expected = read->input.expected_bytes;
    const auto key = read->input.request.value;
    auto *retained = read.get();
    impl_->reads.emplace(key, std::move(read));
    if (!streaming.BeginRead({key}) || !streaming.SubmitRead({key}))
    { impl_->reads.erase(key); return IoFailure("Async asset request state transition failed."); }
    try
    {
        auto source = std::move(retained->input.source);
        auto prepare = std::move(retained->input.prepare);
        auto ranges = std::move(retained->input.ranges);
        auto *cancellation = &retained->cancelled;
        retained->future = std::async(std::launch::async,
            [source = std::move(source), expected, prepare = std::move(prepare),
             ranges = std::move(ranges), cancellation] {
            AssetReadBuffer bytes(static_cast<std::size_t>(expected));
            if (ranges.empty())
            {
                if (auto result = source.ReadInto(bytes.Bytes()); !result)
                    throw std::runtime_error(std::string(result.Message()));
            }
            else
            {
                if (!cancellation->load(std::memory_order_acquire))
                {
                    if (auto result = source.ReadRanges(ranges, bytes.Bytes(), cancellation); !result)
                        throw std::runtime_error(std::string(result.Message()));
                }
            }
            if (prepare && !cancellation->load(std::memory_order_acquire)) prepare(bytes.Bytes());
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
    return CompleteRead(request, streaming, residency, output, wait_for_reads_);
}

Result RendererAssetIo::CompleteRead(StreamingRequestId request, StreamingManager &streaming,
                                     ResidencyManager &residency, AssetReadCompletion &output, bool wait)
{
    output.ready = output.cancelled = false;
    output.storage = {};
    // Release the previous payload without allocating another Debug iterator proxy.
    output.bytes.Reset();
    const auto found = impl_->reads.find(request.value);
    if (found == impl_->reads.end()) return IoFailure("Asset read owner is missing.");
    auto &read = *found->second;
    const auto *page = streaming.Find(request);
    const bool cancellation_requested = read.cancelled.load(std::memory_order_relaxed);
    const bool ledger_cancelled = !page || page->state == StreamingPageState::Cancelled ||
        page->content_generation != read.input.generation;
    const bool cancelled = cancellation_requested || ledger_cancelled;
    if (!cancellation_requested && ledger_cancelled)
        read.cancelled.store(true, std::memory_order_release);
    const auto fail_completed_read = [&](std::string message) {
        static_cast<void>(streaming.FinishRead(request));
        static_cast<void>(streaming.Cancel(request));
        static_cast<void>(residency.ReleaseNow(read.storage));
        impl_->reads.erase(found);
        return IoFailure(std::move(message));
    };
    HRESULT hr = S_OK;
#if HS_WITH_DIRECTSTORAGE
    if (read.transport)
    {
        if (wait)
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
        if (!wait)
        {
            const auto event_result = WaitForSingleObject(read.event.value, 0);
            if (event_result == WAIT_TIMEOUT) return Result::Success();
            if (event_result != WAIT_OBJECT_0) return IoFailure("DirectStorage completion event wait failed.");
        }
        // Error details borrow the completed request's file; drain them before its owner retires.
        read.transport->ReadErrors();
        if (SUCCEEDED(hr) && !cancelled)
        {
            if (!read.prepare_future.valid())
            {
                if (impl_->active_cpu_tasks >= kMaxConcurrentAssetCpuTasks)
                    return Result::Success(); // Keep bytes and marker owner until verification can be admitted.
                read.HoldCpuSlot(impl_->active_cpu_tasks);
                const auto source_proof = read.input.source.AdmittedWholeFileProof();
                auto *cancellation = &read.cancelled;
                // ReadOwner joins this future before destroying its input or bytes.
                const auto *prepare = &read.input.prepare;
                const std::span<const ImmutableAssetRange> ranges{read.input.ranges};
                auto *bytes = &read.bytes;
                try
                {
                    read.prepare_future = std::async(std::launch::async,
                        [prepare, ranges, source_proof, cancellation, bytes] {
                        if (cancellation->load(std::memory_order_acquire)) return;
                        if (ranges.empty() && source_proof && Sha256(bytes->Bytes()) != source_proof->digest)
                            throw std::runtime_error("Immutable file digest mismatch.");
                        if (!ranges.empty())
                        {
                            std::size_t offset{};
                            for (const auto &range : ranges)
                            {
                                if (range.byte_count > bytes->size() - offset ||
                                    Sha256(bytes->Bytes().subspan(offset, range.byte_count)) != range.digest)
                                    throw std::runtime_error("Immutable subresource range digest mismatch.");
                                offset += range.byte_count;
                            }
                            if (offset != bytes->size()) throw std::runtime_error("Immutable subresource ranges are incomplete.");
                        }
                        if (!cancellation->load(std::memory_order_acquire) && *prepare) (*prepare)(bytes->Bytes());
                    });
                }
                catch (const std::exception &error)
                {
                    return fail_completed_read("Could not submit immutable asset verification: " +
                                               std::string(error.what()));
                }
            }
        }
        if (read.prepare_future.valid())
        {
            if (!wait && read.prepare_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return Result::Success();
            try { read.prepare_future.get(); }
            catch (const std::exception &error)
            {
                // Cancellation does not turn a completed integrity/prepare failure into success.
                return fail_completed_read("Immutable asset verification or preparation failed: " +
                                           std::string(error.what()));
            }
        }
    }
    else
#endif
    {
        if (!read.future.valid()) return Result::Success();
        if (!wait && read.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return Result::Success();
        try { read.bytes = read.future.get(); }
        catch (const std::exception &error)
        {
            return fail_completed_read("Immutable asset read failed: " + std::string(error.what()));
        }
    }
    if (SUCCEEDED(hr) && !cancelled &&
        (read.bytes.size() != read.input.expected_bytes ||
         !residency.CommitHost(read.storage, read.bytes.size())))
        return fail_completed_read("Completed immutable read size or shared host accounting is invalid.");
    static_cast<void>(streaming.FinishRead(request));
    output.ready = true;
    output.cancelled = cancelled;
    if (FAILED(hr) || cancelled)
    {
        static_cast<void>(streaming.Cancel(request));
        static_cast<void>(residency.ReleaseNow(read.storage));
    }
    else
    {
        output.bytes = std::move(read.bytes);
        output.storage = read.storage;
    }
    const bool failed = FAILED(hr) && !(cancelled && hr == HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED));
    impl_->reads.erase(found);
    return failed ? IoFailure("DirectStorage read failed: " + std::to_string(hr)) : Result::Success();
}
void RendererAssetIo::Cancel(StreamingRequestId request, StreamingManager &streaming) noexcept
{
    static_cast<void>(streaming.Cancel(request));
    const auto found = impl_->reads.find(request.value);
    if (found == impl_->reads.end()) return;
    found->second->cancelled.store(true, std::memory_order_release);
#if HS_WITH_DIRECTSTORAGE
    if (found->second->transport)
    {
        found->second->transport->queue->CancelRequestsWithTag(UINT64_MAX, found->second->tag);
        SubmitQueuedReads(); // Submit completion markers after cancelling deferred reads.
    }
#endif
}
Result RendererAssetIo::CollectCancelled(StreamingManager &streaming, ResidencyManager &residency)
{
    for (auto iterator = impl_->reads.begin(); iterator != impl_->reads.end();)
    {
        const auto request = iterator->second->input.request;
        const auto *page = streaming.Find(request);
        const bool cancelled = iterator->second->cancelled.load(std::memory_order_relaxed) || !page ||
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
    for (const auto &entry : impl_->reads) Cancel({entry.first}, streaming);
    SubmitQueuedReads();
    while (!impl_->reads.empty())
    {
        AssetReadCompletion output;
        if (auto result = CompleteRead({impl_->reads.begin()->first}, streaming, residency, output, true); !result)
            return result;
    }
    return Result::Success();
}
void RendererAssetIo::SwapState(RendererAssetIo &other) noexcept { impl_.swap(other.impl_); }
} // namespace hs
