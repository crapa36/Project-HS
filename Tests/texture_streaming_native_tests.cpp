#include "render_graph.hpp"
#include "renderer_asset_io.hpp"
#include "renderer_texture_streaming.hpp"
#include "residency_manager.hpp"
#include "streaming_manager.hpp"

#include <hs/render_data/texture_package.hpp>

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void Hr(HRESULT result, const char *message)
{
    Check(SUCCEEDED(result), message);
}

std::uint8_t PixelByte(std::uint32_t slice, std::uint32_t mip, std::size_t byte)
{
    return static_cast<std::uint8_t>((slice * 31u + mip * 17u + byte * 13u + 3u) & 0xffu);
}

struct TextureSourceFile
{
    TextureSourceFile()
    {
        static std::atomic<std::uint64_t> next_id{};
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::uint32_t attempt = 0; attempt < 128; ++attempt)
        {
            directory = root / ("ProjectHS-texture-streaming-native-" + std::to_string(stamp) + "-" +
                std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory(directory, error))
            {
                path = directory / "fixture.rtex";
                return;
            }
            Check(!error || error == std::errc::file_exists,
                  "create unique texture-streaming temporary directory");
        }
        throw std::runtime_error("could not reserve a unique texture-streaming temporary directory");
    }

    std::filesystem::path directory;
    std::filesystem::path path;

    void Write(std::span<const std::byte> bytes) const
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        Check(static_cast<bool>(output), "open synthetic RTEX source");
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        Check(static_cast<bool>(output), "write synthetic RTEX source");
    }

    void Remove() const
    {
        std::error_code error;
        std::filesystem::remove(path, error);
        Check(!error && !std::filesystem::exists(path), "remove synthetic RTEX source");
    }

    ~TextureSourceFile()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

D3D12_RESOURCE_DESC TextureDescription(const hs::render_data::TextureDescription &source,
                                       std::uint32_t first_mip)
{
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.Width = std::max(1u, source.width >> first_mip);
    result.Height = std::max(1u, source.height >> first_mip);
    result.DepthOrArraySize = static_cast<UINT16>(source.array_size);
    result.MipLevels = static_cast<UINT16>(source.mip_count - first_mip);
    result.Format = static_cast<DXGI_FORMAT>(source.format);
    result.SampleDesc = {1, 0};
    return result;
}

D3D12_RESOURCE_DESC BufferDescription(std::uint64_t size)
{
    D3D12_RESOURCE_DESC result{};
    result.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    result.Width = size;
    result.Height = 1;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return result;
}

class TextureStreamFixture
{
  public:
    TextureStreamFixture(ID3D12Device *device, D3D12MA::Allocator *allocator,
                         hs::RenderGraphQueueExecutor &executor, bool reserved_candidate = false)
        : device_(device), allocator_(allocator), executor_(executor)
    {
        description_ = reserved_candidate
            ? hs::render_data::TextureDescription{256, 256, 1, std::bit_width(256u), hs::render_data::kTextureFormatRgba8}
            : hs::render_data::TextureDescription{4, 4096, 4, std::bit_width(4096u), hs::render_data::kTextureFormatRgba8};
        const auto subresources = static_cast<std::size_t>(description_.array_size) * description_.mip_count;
        payload_.reserve(static_cast<std::size_t>(description_.array_size) * description_.width *
            description_.height * 4u * 2u);
        for (std::uint32_t slice = 0; slice < description_.array_size; ++slice)
            for (std::uint32_t mip = 0; mip < description_.mip_count; ++mip)
            {
                const auto width = std::max(1u, description_.width >> mip);
                const auto height = std::max(1u, description_.height >> mip);
                const auto bytes = static_cast<std::size_t>(width) * height * 4u;
                for (std::size_t index = 0; index < bytes; ++index)
                    payload_.push_back(static_cast<std::byte>(PixelByte(slice, mip, index)));
            }
        if (!reserved_candidate)
            Check(payload_.size() > 256u * 1024u && subresources >= 48u,
                  "synthetic streaming package must cover multiple software chunks");
        const std::vector<std::string> dependencies;
        package_ = hs::render_data::BuildTexturePackage(
            hs::MakeAssetId(logical_id_), dependencies, description_, payload_);
        descriptor_ = hs::render_data::CompileTextureDescriptor(
            package_, hs::MakeAssetId(logical_id_), 0);
        source_.Write(package_);

        hs::ResidencyBudgetSnapshot budget{};
        budget.valid = true;
        budget.local_budget = 2ull * 1024u * 1024u * 1024u;
        budget.local_fresh = true;
        budget.nonlocal_valid = true;
        budget.nonlocal_budget = 2ull * 1024u * 1024u * 1024u;
        budget.nonlocal_fresh = true;
        budget.host_budget_valid = true;
        budget.host_fresh = true;
        budget.host_staging_budget = 32ull * 1024u * 1024u;
        residency_.UpdateBudget(budget);

        Check(io_.Initialize(false, 0, residency_).Succeeded(), "initialize CPU asset IO");
        io_.SetWaitForReads(true);
        Check(streaming_.SupplyDescriptor(logical_id_, std::move(descriptor_)).Succeeded(),
              "supply synthetic RTEX descriptor");
        hs::TextureStreamingCapabilities capabilities{};
        Check(streaming_.Initialize(device_, allocator_, hs::GraphicsQuality::Low, 0, 64,
            capabilities, 3, {}).Succeeded(), "initialize texture streaming");

        const auto fallback_mip = std::min(4u, description_.mip_count - 1u);
        const auto fallback_desc = TextureDescription(description_, fallback_mip);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &fallback_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&fallback_)),
            "create synthetic fallback texture");
        const auto allocation_info = device_->GetResourceAllocationInfo(0, 1, &fallback_desc);
        Check(allocation_info.SizeInBytes && allocation_info.SizeInBytes != UINT64_MAX,
              "query fallback allocation bytes");
        hs::TextureStreamRegistration registration{};
        registration.generation = "native-fixture-generation";
        registration.logical_id = logical_id_;
        registration.source.path = source_.path;
        registration.generation_lease = std::make_shared<int>(1);
        registration.description = description_;
        registration.fallback = fallback_;
        registration.fallback_first_mip = fallback_mip;
        registration.fallback_allocation_bytes = allocation_info.SizeInBytes;
        registration.fallback_graph_access = hs::Access::Common;
        Check(streaming_.Register(std::move(registration), residency_, id_).Succeeded(),
              "register synthetic streaming texture");

        Check(pool_.Initialize(allocator_, device_).Succeeded(), "initialize texture graph pool");
        // Declared last so emergency drain runs before graph/streaming owners unwind.
        drain_.executor = &executor_;
    }

    ~TextureStreamFixture()
    {
        if (drain_.executor && !drain_.drained)
            static_cast<void>(drain_.executor->Drain());
    }

    hs::TextureStreamId Id() const noexcept { return id_; }
    hs::RendererTextureStreaming &Streaming() noexcept { return streaming_; }
    hs::RendererAssetIo &Io() noexcept { return io_; }
    hs::StreamingManager &Requests() noexcept { return requests_; }
    hs::ResidencyManager &Residency() noexcept { return residency_; }
    hs::RenderGraphBuilder &Graph() noexcept { return graph_; }
    hs::TransientResourcePool &Pool() noexcept { return pool_; }
    TextureSourceFile &Source() noexcept { return source_; }
    hs::RenderGraphQueueExecutor &Executor() noexcept { return executor_; }
    void NextFrame() noexcept { ++frame_; }
    const std::vector<std::byte> &Package() const noexcept { return package_; }
    std::uint32_t TailMip() const noexcept { return std::min(4u, description_.mip_count - 1u); }
    bool HardwareBacked() const noexcept { return streaming_.HardwareBacked(id_); }
    std::string TimeoutDetails(std::uint32_t target_mip, std::uint32_t attempts) const
    {
        const auto status = Status();
        const auto host = residency_.HostAdmissionFacts();
        const auto accounting = residency_.Accounting();
        std::ostringstream detail;
        detail << "target=" << target_mip << " attempts=" << attempts << " frame=" << frame_
            << " actual=" << status.actual_resident_mip << " desired=" << status.effective_desired_mip
            << " selected=" << status.target_mip << " pending=" << status.pending
            << " publication_complete=" << status.publication_complete << " requests=" << status.requests
            << " uploads=" << status.uploads << " publications=" << status.publications
            << " io_reads=" << io_.HasPendingReads()
            << " host_valid=" << host.budget.host_budget_valid << " host_fresh=" << host.budget.host_fresh
            << " host_budget=" << host.budget.host_staging_budget << " host_held=" << host.held_bytes
            << " host_reserved=" << accounting.host_reserved_total << " host_staging=" << accounting.host_staging_total
            << " local_valid=" << host.budget.valid << " local_fresh=" << host.budget.local_fresh
            << " local_budget=" << host.budget.local_budget << " local_usage=" << host.budget.local_usage
            << " gpu_reserved=" << accounting.gpu_reserved_total
            << " gpu_growth=" << accounting.gpu_growth_since_snapshot;
        if (status.requests)
        {
            const auto *latest = requests_.Find({status.requests});
            if (latest)
                detail << " latest_request=" << status.requests << " page=" << latest->page_id
                    << " state=" << static_cast<unsigned>(latest->state)
                    << " submitted=" << latest->upload_submitted;
        }
        return detail.str();
    }

    bool Pump(std::uint32_t target_mip)
    {
        streaming_.Demand(id_, target_mip, 100, frame_);
        const auto result = streaming_.Pump(frame_, executor_.Completed(), io_, requests_, residency_);
        if (!result) throw std::runtime_error("texture streaming pump failed: " + std::string(result.Message()));
        return true;
    }

    bool PumpWithCompletion(std::uint32_t target_mip, hs::RendererAssetIo &io,
                            const hs::CompletionSet &completed)
    {
        streaming_.Demand(id_, target_mip, 100, frame_);
        const auto result = streaming_.Pump(frame_, completed, io, requests_, residency_);
        if (!result) throw std::runtime_error("texture streaming pump failed: " + std::string(result.Message()));
        return true;
    }

    bool AddPendingGraph(hs::TextureStreamingLayout &layout,
                         std::vector<hs::TextureStreamingGraphBinding> &bindings)
    {
        graph_.Reset();
        auto owner = std::make_shared<ComPtr<ID3D12CommandQueue>>(
            executor_.NativeQueue(hs::QueueHint::Copy));
        hs::ExternalQueueBinding queue{std::shared_ptr<void>(owner, owner->Get()), hs::QueueHint::Copy, {}};
        if (auto result = streaming_.AddPendingPasses(graph_, std::move(queue), 4000,
                bindings, &layout); !result)
            throw std::runtime_error(std::string(result.Message()));
        return !layout.entries.empty() && layout.entries[0].pending;
    }

    hs::CompletionSet SubmitPending()
    {
        if (auto prepared = graph_.Prepare(pool_); !prepared)
            throw std::runtime_error(std::string(prepared.Message()));
        hs::CompletionSet completion;
        if (auto executed = executor_.Execute(graph_, 0, hs::BarrierMode::Legacy, completion); !executed)
            throw std::runtime_error(std::string(executed.Message()));
        streaming_.MarkSubmitted(completion, requests_);
        streaming_.MarkUse(id_, completion);
        return completion;
    }

    void Wait(hs::CompletionSet completion)
    {
        if (auto waited = executor_.Wait(completion); !waited)
            throw std::runtime_error(std::string(waited.Message()));
        pool_.ResetCompletedClaims(executor_.Completed());
        CheckNativeValidation("streaming batch completion");
    }

    hs::TextureStreamingProductStatus Status() const
    {
        return streaming_.ProductStatus(logical_id_, requests_, executor_.Completed());
    }

    void ReadbackPublished(std::uint32_t logical_mip)
    {
        const auto view = streaming_.View(id_);
        Check(view.resource && logical_mip >= view.first_mip &&
            logical_mip < view.logical_description.mip_count,
            "published texture exposes requested logical mip");
        ComPtr<ID3D12Resource> texture = view.resource;
        const auto description = texture->GetDesc();
        const auto local_mip = logical_mip - view.first_mip;
        Check(local_mip < description.MipLevels && view.logical_description.array_size == description.DepthOrArraySize,
              "logical mip and array slices map to native subresources");

        struct SliceReadback
        {
            UINT subresource{};
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT rows{};
            UINT64 row_bytes{}, total_bytes{};
        };
        std::vector<SliceReadback> slices(description.DepthOrArraySize);
        UINT64 total_bytes{};
        for (UINT slice = 0; slice < description.DepthOrArraySize; ++slice)
        {
            auto &entry = slices[slice];
            entry.subresource = slice * description.MipLevels + local_mip;
            Check(entry.subresource < static_cast<UINT>(description.MipLevels) * description.DepthOrArraySize,
                  "array slice and mip map to a native subresource");
            const auto base = (total_bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u) &
                ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1u);
            Check(base >= total_bytes, "align readback footprint base without overflow");
            device_->GetCopyableFootprints(&description, entry.subresource, 1, base,
                &entry.footprint, &entry.rows, &entry.row_bytes, &entry.total_bytes);
            Check(entry.total_bytes && entry.total_bytes != UINT64_MAX &&
                entry.total_bytes <= UINT64_MAX - base,
                  "query texture array-slice readback footprint");
            total_bytes = base + entry.total_bytes;
        }

        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        const auto readback_desc = BufferDescription(total_bytes);
        ComPtr<ID3D12Resource> readback;
        Hr(device_->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
            "create texture-streaming readback");

        graph_.Reset();
        Check(!Status().pending, "readback starts from a settled texture owner");
        const auto source = graph_.ImportTexture(
            {texture.Get(), view.completion, hs::QueueHint::Copy, {}}, view.access,
            "published texture readback source");
        const auto destination = graph_.ImportBuffer(
            {readback.Get(), {}, hs::QueueHint::Direct, {}}, hs::Access::CopyDest, "texture streaming readback");
        auto pass = graph_.AddPass(5000, "read back published mip", hs::QueueHint::Direct);
        pass.Read(graph_.Version(source), hs::Access::CopySource);
        (void)pass.Write(graph_.Version(destination), hs::Access::CopyDest);
        pass.SetExecute([texture, readback, slices](hs::RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            for (const auto &slice : slices)
            {
                D3D12_TEXTURE_COPY_LOCATION source_location{};
                source_location.pResource = texture.Get();
                source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source_location.SubresourceIndex = slice.subresource;
                D3D12_TEXTURE_COPY_LOCATION target_location{};
                target_location.pResource = readback.Get();
                target_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                target_location.PlacedFootprint = slice.footprint;
                commands->CopyTextureRegion(&target_location, 0, 0, 0, &source_location, nullptr);
            }
        });
        graph_.SetFinalAccess(graph_.Version(source), hs::Access::AllShaderRead);
        if (auto prepared = graph_.Prepare(pool_); !prepared)
            throw std::runtime_error(std::string(prepared.Message()));
        hs::CompletionSet completion;
        if (auto executed = executor_.Execute(graph_, 0, hs::BarrierMode::Legacy, completion); !executed)
            throw std::runtime_error(std::string(executed.Message()));
        streaming_.MarkUse(id_, completion);
        if (auto waited = executor_.Wait(completion); !waited)
            throw std::runtime_error(std::string(waited.Message()));
        pool_.ResetCompletedClaims(executor_.Completed());
        CheckNativeValidation("published texture readback completion");

        std::byte *mapped{};
        D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_bytes)};
        Hr(readback->Map(0, &read_range, reinterpret_cast<void **>(&mapped)), "map texture-streaming readback");
        const auto width = std::max(1u, description_.width >> logical_mip);
        const auto height = std::max(1u, description_.height >> logical_mip);
        for (std::size_t slice_index = 0; slice_index < slices.size(); ++slice_index)
        {
            const auto &entry = slices[slice_index];
            Check(entry.row_bytes == static_cast<UINT64>(width) * 4u && entry.rows == height,
                  "readback footprint matches the logical array mip");
            for (std::size_t index = 0; index < static_cast<std::size_t>(entry.row_bytes) * entry.rows; ++index)
                Check(mapped[entry.footprint.Offset +
                        (index / entry.row_bytes) * entry.footprint.Footprint.RowPitch + index % entry.row_bytes] ==
                    static_cast<std::byte>(PixelByte(static_cast<std::uint32_t>(slice_index), logical_mip, index)),
                    "published array mip bytes match the synthetic RTEX source");
        }
        D3D12_RANGE no_write{0, 0};
        readback->Unmap(0, &no_write);
    }

    void Drain()
    {
        if (auto drained = executor_.Drain(); !drained)
            throw std::runtime_error(std::string(drained.Message()));
        drain_.drained = true;
    }

  private:
    void CheckNativeValidation(const char *stage)
    {
        ComPtr<ID3D12InfoQueue> info;
        if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&info)))) return;
        const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 index = 0; index < count; ++index)
        {
            SIZE_T size{};
            Hr(info->GetMessage(index, nullptr, &size), "query texture fixture validation message size");
            std::vector<char> bytes(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
            Hr(info->GetMessage(index, message, &size), "query texture fixture validation message");
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
                throw std::runtime_error(std::string(stage) + ": " + message->pDescription);
        }
        info->ClearStoredMessages();
    }

    struct DrainGuard
    {
        hs::RenderGraphQueueExecutor *executor{};
        bool drained{};
    } drain_;
    ID3D12Device *device_{};
    D3D12MA::Allocator *allocator_{};
    hs::RenderGraphQueueExecutor &executor_;
    TextureSourceFile source_;
    const std::string logical_id_{"test/native-texture-streaming"};
    hs::render_data::TextureDescription description_{};
    hs::render_data::TextureDescriptorStorage descriptor_;
    std::vector<std::byte> payload_, package_;
    hs::ResidencyManager residency_;
    hs::RendererAssetIo io_;
    hs::StreamingManager requests_;
    hs::RendererTextureStreaming streaming_;
    ComPtr<ID3D12Resource> fallback_;
    hs::TextureStreamId id_{};
    hs::TransientResourcePool pool_;
    hs::RenderGraphBuilder graph_;
    std::uint64_t frame_{1};
};

void SettleTarget(TextureStreamFixture &fixture, std::uint32_t target_mip,
                  std::optional<bool> expect_software_copy = std::nullopt,
                  ID3D12Resource *expected_old_owner = nullptr,
                  hs::RendererAssetIo *io_override = nullptr,
                  bool expect_reserved_unmap = false)
{
    bool saw_copy{};
    bool saw_reserved_unmap{};
    for (std::uint32_t attempt = 0; attempt < 128; ++attempt)
    {
        Check(io_override
                ? fixture.PumpWithCompletion(target_mip, *io_override, fixture.Executor().Completed())
                : fixture.Pump(target_mip),
              "pump texture streaming target");
        auto status = fixture.Status();
        if (status.publication_complete && !status.pending && status.actual_resident_mip == target_mip)
        {
            if (expect_software_copy) Check(saw_copy == *expect_software_copy, "streaming path mode observed");
            if (expect_reserved_unmap)
                Check(saw_reserved_unmap, "reserved coarsening emits an actual unmap batch");
            return;
        }
        hs::TextureStreamingLayout layout;
        std::vector<hs::TextureStreamingGraphBinding> bindings;
        if (fixture.AddPendingGraph(layout, bindings))
        {
            if (layout.entries[0].software_copy) saw_copy = true;
            if (layout.entries[0].reserved && layout.entries[0].unmap_count)
                saw_reserved_unmap = true;
            if (layout.entries[0].software_copy)
            {
                Check(expected_old_owner && io_override,
                      "software-copy lifetime fixture has its prior owner and fresh IO service");
                Check(fixture.Streaming().View(fixture.Id()).resource == expected_old_owner,
                      "software-copy candidate stays hidden before submission");
            }
            const auto completion = fixture.SubmitPending();
            if (layout.entries[0].software_copy)
            {
                hs::CompletionSet none;
                Check(fixture.PumpWithCompletion(target_mip, *io_override, none),
                      "pump before software-copy completion");
                Check(fixture.Status().pending &&
                    fixture.Streaming().View(fixture.Id()).resource == expected_old_owner,
                    "software-copy candidate remains hidden before completion");
            }
            fixture.Wait(completion);
            if (layout.entries[0].software_copy)
            {
                Check(fixture.PumpWithCompletion(target_mip, *io_override, completion),
                      "publish software copy after real queue completion");
                Check(fixture.Status().actual_resident_mip == target_mip &&
                    fixture.Streaming().View(fixture.Id()).resource != expected_old_owner,
                    "software-copy candidate publishes after completion");
            }
        }
        fixture.NextFrame();
    }
    throw std::runtime_error("texture streaming did not settle within bounded native fixture iterations: " +
        fixture.TimeoutDetails(target_mip, 128));
}
} // namespace

void RunTextureStreamingNative(ID3D12Device *device, D3D12MA::Allocator *allocator,
                               hs::RenderGraphQueueExecutor &executor)
{
    TextureStreamFixture fixture(device, allocator, executor);
    const auto fine_mip = 0u;
    const auto coarse_mip = fixture.TailMip() - 1u;
    SettleTarget(fixture, fine_mip, false);
    const auto first_publication = fixture.Status();
    Check(first_publication.publications > 0 && first_publication.actual_resident_mip == fine_mip,
          "fine software texture generation publishes");
    Check(first_publication.requests >= 2 && first_publication.uploads >= 2,
          "tall software texture exercises multiple authenticated chunks");
    fixture.ReadbackPublished(fine_mip);

    const auto old_owner = fixture.Streaming().View(fixture.Id()).resource;
    fixture.Source().Remove();
    const auto requests_before_coarsen = fixture.Status().requests;
    hs::RendererAssetIo coarsen_io;
    Check(coarsen_io.Initialize(false, 0, fixture.Residency()).Succeeded(),
          "initialize fresh source-IO owner for resident coarsen");
    coarsen_io.SetWaitForReads(true);
    SettleTarget(fixture, coarse_mip, true, old_owner, &coarsen_io);
    Check(!coarsen_io.HasPendingReads() && fixture.Status().requests == requests_before_coarsen + 1u,
          "resident coarsen owns one GPU transaction without payload reads");
    Check(fixture.Streaming().View(fixture.Id()).resource != old_owner,
          "coarse software candidate publishes after completion");
    fixture.ReadbackPublished(coarse_mip);

    fixture.Source().Write(fixture.Package());
    const auto published_coarse_owner = fixture.Streaming().View(fixture.Id()).resource;
    const auto requests_before_cancel = fixture.Status().requests;
    Check(fixture.Pump(fine_mip), "begin a finer source-backed request");
    Check(fixture.Status().requests == requests_before_cancel + 1u && fixture.Io().HasPendingReads(),
          "fine re-request owns an outstanding source read");
    const hs::StreamingRequestId cancelled_request{requests_before_cancel + 1u};
    const auto *request = fixture.Requests().Find(cancelled_request);
    Check(request && request->state == hs::StreamingPageState::Reading &&
        request->content_generation == "native-fixture-generation",
        "cancellable read belongs to the registered content generation");
    fixture.Streaming().CancelReads(fixture.Io(), fixture.Requests());
    Check(fixture.Requests().Find(cancelled_request)->state == hs::StreamingPageState::Cancelled,
          "asset IO cancellation marks its generation request cancelled");
    Check(fixture.Io().CollectCancelled(fixture.Requests(), fixture.Residency()).Succeeded() &&
        !fixture.Io().HasPendingReads(), "collect the cancelled source-read owner");
    fixture.NextFrame();
    Check(fixture.Pump(coarse_mip), "collect cancelled texture read");
    Check(fixture.Streaming().View(fixture.Id()).resource == published_coarse_owner,
          "cancelled finer request preserves the published coarse owner");
    SettleTarget(fixture, fine_mip, false);
    Check(fixture.Status().actual_resident_mip == fine_mip && fixture.Status().publications >= 3,
          "refinement after cancelled request publishes a new software generation");
    fixture.ReadbackPublished(fine_mip);
    fixture.Drain();
}

void RunReservedTextureStreamingNative(ID3D12Device *device, D3D12MA::Allocator *allocator,
                                       hs::RenderGraphQueueExecutor &executor)
{
    TextureStreamFixture fixture(device, allocator, executor, true);
    if (!fixture.HardwareBacked())
    {
        fixture.Drain();
        return;
    }
    const auto tail = fixture.TailMip();
    const auto fine = 0u;
    const auto coarse = std::min(1u, tail);
    Check(coarse > fine, "reserved fixture has a nonzero coarsening target");
    SettleTarget(fixture, fine, false);
    fixture.ReadbackPublished(fine);
    SettleTarget(fixture, coarse, false, nullptr, nullptr, true);
    fixture.ReadbackPublished(coarse);
    SettleTarget(fixture, fine, false);
    fixture.ReadbackPublished(fine);
    fixture.Drain();
}
