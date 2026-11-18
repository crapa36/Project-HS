#include "residency_manager.hpp"
#include "renderer_asset_io.hpp"
#include "renderer_content.hpp"
#include "streaming_manager.hpp"
#include "renderer_texture_streaming.hpp"
#include <hs/core/sha256.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void CheckAsyncAssetPreparation()
{
    const std::array<std::uint32_t, 3> replacement_subresources{0, 1, 2};
    std::array<hs::render_data::TextureSubresourceLayout, 3> replacement_layouts{};
    replacement_layouts[0].byte_count = 5; replacement_layouts[1].byte_count = 7;
    replacement_layouts[2].byte_count = 3;
    for (auto &layout : replacement_layouts) layout.has_digest = true;
    const auto first_chunk = hs::TextureReadChunkEnd(replacement_subresources, replacement_layouts, 0, 12);
    const auto final_chunk = hs::TextureReadChunkEnd(replacement_subresources, replacement_layouts, first_chunk, 12);
    Check(first_chunk == 2 && first_chunk != replacement_subresources.size() &&
        final_chunk == replacement_subresources.size() &&
        hs::TextureReadChunkEnd(replacement_subresources, replacement_layouts, 0, 4) == 0,
        "software replacement chunking split an authenticated range or marked a partial candidate complete");
    replacement_layouts[1].has_digest = true;
    std::uint64_t packed_upload_bytes{};
    std::uint64_t beyond_window_bytes{};
    const std::array<std::uint64_t, 3> native_footprints{5, 3, 4};
    Check(hs::TextureUploadPackAppend(5, 3, 4, 12, packed_upload_bytes) &&
        packed_upload_bytes == 11 && !hs::TextureUploadPackAppend(packed_upload_bytes, 4, 4, 12,
            beyond_window_bytes) &&
        hs::TextureReadChunkEndWithinUploadWindow(replacement_subresources, replacement_layouts,
            native_footprints, 0, 100, 4, 12) == 2 &&
        hs::TextureReadChunkEndWithinUploadWindow(replacement_subresources, replacement_layouts,
            native_footprints, 1, 100, 4, 12) == 3 &&
        hs::TextureReadChunkEndWithinUploadWindow(replacement_subresources, replacement_layouts,
            native_footprints, 0, 11, 4, 12) == 1,
        "software replacement chunk ignored the aligned upload-window or host-payload boundary");
    hs::render_data::TextureDescription target_description{8, 4, 1, 3, 98};
    std::array<hs::render_data::TextureSubresourceLayout, 3> target_layouts{};
    target_layouts[0].byte_count = 11; target_layouts[1].byte_count = 5; target_layouts[2].byte_count = 2;
    for (auto &layout : target_layouts) layout.has_digest = true;
    std::uint32_t minimum_host_mip{};
    Check(hs::TextureReadTargetMip(target_description, target_layouts, 0, 2, 8, 18, 2) == 1 &&
        hs::TextureReadTargetMip(target_description, target_layouts, 0, 2, 8, 19, 2) == 0 &&
        hs::TextureReadTargetMip(target_description, target_layouts, 0, 2, 8, 20, 2) == 0 &&
        hs::TextureReadTargetMip(target_description, target_layouts, 0, 2, 8, 9, 2) == 2 &&
        hs::TextureReadMinimumHostMip(target_description, target_layouts, 8, 18, minimum_host_mip) &&
        minimum_host_mip == 1 &&
        hs::TextureReadMinimumHostMip(target_description, target_layouts, 8, 9, minimum_host_mip) &&
        minimum_host_mip == target_description.mip_count,
        "optional mip admission did not clamp below, at, and above the intrinsic authenticated-range cap");
    const auto temporary_coarse_target = hs::TextureReadTargetMip(target_description,
        target_layouts, 0, 2, 8, 18, 2);
    Check(temporary_coarse_target == 1 &&
        hs::TextureHostFootprintFitsBudget(8, target_layouts[0].byte_count,
            hs::kMaximumHostStagingBudgetBytes) &&
        hs::TextureHostFootprintFitsBudget(8, 0, hs::kMaximumHostStagingBudgetBytes),
        "temporary coarse host-fit target was not deferred while its source unit remained possible");
    Check(!hs::TextureHostFootprintFitsBudget(8,
            hs::kMaximumHostStagingBudgetBytes - 8 + 1,
            hs::kMaximumHostStagingBudgetBytes),
        "source unit beyond the maximum host staging ceiling was not classified impossible");
    hs::ResidencyManager shrunken_host_budget;
    shrunken_host_budget.UpdateBudget({true, 0, 0, 0, 0, true, 1024});
    const auto first_metadata = shrunken_host_budget.ReserveHost(400);
    const auto second_metadata = shrunken_host_budget.ReserveHost(400);
    Check(first_metadata && second_metadata &&
        shrunken_host_budget.CommitHost(first_metadata, 400) &&
        shrunken_host_budget.CommitHost(second_metadata, 400),
        "software chunk metadata reservations were not committed");
    shrunken_host_budget.UpdateBudget({true, 0, 0, 0, 0, true, 500});
    const bool shrink_blocked = !shrunken_host_budget.CanStage(1);
    const bool chunk_metadata_released = shrunken_host_budget.ReleaseNow(second_metadata);
    const auto next_chunk = shrunken_host_budget.ReserveHost(100);
    Check(shrink_blocked && chunk_metadata_released && next_chunk &&
        shrunken_host_budget.CanStage(0) && shrunken_host_budget.ReleaseNow(next_chunk) &&
        shrunken_host_budget.ReleaseNow(first_metadata),
        "completed chunk metadata was not releasable to let a retained transaction progress after budget shrink");
    const std::array<std::byte, 4> contents{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const auto path = std::filesystem::temp_directory_path() /
        ("hs_stream_prepare_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    struct RemoveFile { std::filesystem::path path; ~RemoveFile() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(contents.data()),
                    static_cast<std::streamsize>(contents.size()));
        Check(file.good(), "could not create async preparation fixture");
    }

    hs::ResidencyManager residency;
    residency.UpdateBudget({true, 0, 0, 0, 0, true, 1024});
    hs::StreamingManager streaming;
    hs::RendererAssetIo io;
    Check(static_cast<bool>(io.Initialize(false, 0, residency)), "async asset IO initialization failed");
    const auto caller = std::this_thread::get_id();
    const auto enqueue = [&](std::string id, std::function<void(std::span<const std::byte>)> prepare) {
        const auto request = streaming.Request(id, id, "generation", 1);
        const auto storage = residency.ReserveHost(contents.size() * 2);
        Check(request && storage, "async asset request reservation failed");
        bool submitted{};
        const auto result = io.Enqueue({request, "generation", id, hs::RendererAssetSource::Loose(path), contents.size(),
            std::make_shared<int>(1), std::move(prepare)}, streaming, residency, storage, submitted);
        Check(static_cast<bool>(result) && submitted, "async asset read was not submitted");
        return std::pair{request, storage};
    };
    const auto poll = [&](hs::StreamingRequestId request, hs::AssetReadCompletion &output) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto result = io.Poll(request, streaming, residency, output); !result) return result;
            if (output.ready) return hs::Result::Success();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("async asset preparation did not complete");
    };

    std::atomic_bool prepared{};
    std::thread::id worker{};
    const auto [success, success_storage] = enqueue("prepared-success", [&](std::span<const std::byte> bytes) {
        worker = std::this_thread::get_id();
        prepared.store(bytes.size() == contents.size() && std::equal(bytes.begin(), bytes.end(), contents.begin()),
                       std::memory_order_release);
    });
    hs::AssetReadCompletion success_output;
    Check(static_cast<bool>(poll(success, success_output)) && success_output.ready && !success_output.cancelled &&
        prepared.load(std::memory_order_acquire) && worker != caller,
        "asset preparation did not run asynchronously before read publication");
    Check(streaming.CompleteRead(success, "generation"), "prepared read did not enter Decoded");
    Check(residency.ReleaseNow(success_output.storage) && !residency.Contains(success_storage),
        "prepared read storage was not reclaimed");

    const auto ranged = streaming.Request("ranged-subresources", "ranged-subresources", "generation", 1);
    const auto ranged_storage = residency.ReserveHost(3);
    const std::array ranges{
        hs::ImmutableAssetRange{1, 2, hs::Sha256(std::span(contents).subspan(1, 2))},
        hs::ImmutableAssetRange{3, 1, hs::Sha256(std::span(contents).subspan(3, 1))}};
    bool ranged_submitted{};
    Check(ranged && ranged_storage && bool(io.Enqueue({ranged, "generation", "ranged-subresources",
        hs::RendererAssetSource::Loose(path), 3, std::make_shared<int>(1), {},
        std::vector<hs::ImmutableAssetRange>(ranges.begin(), ranges.end())}, streaming, residency,
        ranged_storage, ranged_submitted)) && ranged_submitted,
        "authenticated texture subresource ranges were not submitted");
    hs::AssetReadCompletion ranged_output;
    Check(bool(poll(ranged, ranged_output)) && ranged_output.ready && !ranged_output.cancelled &&
        std::ranges::equal(ranged_output.bytes.Bytes(), std::array{contents[1], contents[2], contents[3]}) &&
        streaming.CompleteRead(ranged, "generation") && residency.ReleaseNow(ranged_output.storage) &&
        !residency.Contains(ranged_storage), "range IO did not verify or compact its selected subresources");
    auto started = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    const auto release_gate = release->get_future().share();
    const auto canceled_done = std::make_shared<std::atomic_bool>();
    const auto [cancelled, cancelled_storage] = enqueue("prepared-cancelled",
        [started, release_gate, canceled_done](std::span<const std::byte>) {
            started->set_value();
            release_gate.wait();
            canceled_done->store(true, std::memory_order_release);
        });
    if (started->get_future().wait_for(std::chrono::seconds(5)) != std::future_status::ready)
    {
        release->set_value();
        throw std::runtime_error("cancellable preparation did not start");
    }
    io.Cancel(cancelled, streaming);
    release->set_value();
    hs::AssetReadCompletion cancelled_output;
    const auto cancelled_result = poll(cancelled, cancelled_output);
    Check(bool(cancelled_result) && cancelled_output.ready && cancelled_output.cancelled &&
        cancelled_output.bytes.empty() && canceled_done->load(std::memory_order_acquire) &&
        !residency.Contains(cancelled_storage) && !io.HasPendingReads(),
        "cancelled preparation published bytes or leaked its owner");
    // CPU work admission is independent of host bytes. A saturated read owner
    // remains Requested and can be retried after accepted futures reach terminal.
    constexpr std::uint32_t cpu_task_limit = 32;
    auto task_release = std::make_shared<std::promise<void>>();
    struct ReleaseTaskGate
    {
        std::shared_ptr<std::promise<void>> promise;
        bool released{};
        void Release() { if (!released) { promise->set_value(); released = true; } }
        ~ReleaseTaskGate() { if (!released) { try { Release(); } catch (...) {} } }
    } release_task_gate{task_release};
    const auto task_gate = task_release->get_future().share();
    auto tasks_started = std::make_shared<std::atomic_uint32_t>();
    auto tasks_finished = std::make_shared<std::atomic_uint32_t>();
    std::vector<hs::StreamingRequestId> task_requests;
    task_requests.reserve(cpu_task_limit);
    for (std::uint32_t i = 0; i < cpu_task_limit; ++i)
    {
        const auto id = "cpu-admission-" + std::to_string(i);
        const auto request = streaming.Request(id, id, "generation", 1);
        const auto storage = residency.ReserveHost(contents.size() * 2);
        bool submitted{};
        const auto queued = io.Enqueue({request, "generation", id, hs::RendererAssetSource::Loose(path), contents.size(),
            std::make_shared<int>(1), [task_gate, tasks_started, tasks_finished](std::span<const std::byte>) {
                tasks_started->fetch_add(1, std::memory_order_release);
                task_gate.wait();
                tasks_finished->fetch_add(1, std::memory_order_release);
            }}, streaming, residency, storage, submitted);
        Check(request && storage && bool(queued) && submitted,
            "asset CPU task admission rejected work below its owner-local ceiling");
        task_requests.push_back(request);
    }
    const auto task_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tasks_started->load(std::memory_order_acquire) != cpu_task_limit &&
           std::chrono::steady_clock::now() < task_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (tasks_started->load(std::memory_order_acquire) != cpu_task_limit)
    {
        release_task_gate.Release();
        throw std::runtime_error("accepted CPU admission tasks did not all enter their preparation callback");
    }
    const auto deferred_id = std::string("cpu-admission-deferred");
    const auto deferred_request = streaming.Request(deferred_id, deferred_id, "generation", 1);
    auto deferred_storage = residency.ReserveHost(contents.size() * 2);
    bool deferred_submitted{};
    const auto deferred_result = io.Enqueue({deferred_request, "generation", deferred_id,
        hs::RendererAssetSource::Loose(path), contents.size(), std::make_shared<int>(1), {}},
        streaming, residency, deferred_storage, deferred_submitted);
    const auto *deferred_page = streaming.Find(deferred_request);
    const bool deferred_cleanly = bool(deferred_result) && !deferred_submitted && deferred_page &&
        deferred_page->state == hs::StreamingPageState::Requested;
    release_task_gate.Release();
    Check(deferred_cleanly, "saturated CPU task admission consumed a request or lost its retry state");
    Check(residency.ReleaseNow(deferred_storage), "deferred CPU task kept its caller reservation");
    const auto finish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tasks_finished->load(std::memory_order_acquire) != cpu_task_limit &&
           std::chrono::steady_clock::now() < finish_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Check(tasks_finished->load(std::memory_order_acquire) == cpu_task_limit,
        "admitted CPU task callables did not reach terminal state");
    auto completed_but_uncollected_storage = residency.ReserveHost(contents.size() * 2);
    bool completed_but_uncollected_submitted{};
    const auto completed_but_uncollected = io.Enqueue({deferred_request, "generation", deferred_id,
        hs::RendererAssetSource::Loose(path), contents.size(), std::make_shared<int>(1), {}},
        streaming, residency, completed_but_uncollected_storage, completed_but_uncollected_submitted);
    Check(bool(completed_but_uncollected) && !completed_but_uncollected_submitted &&
        residency.ReleaseNow(completed_but_uncollected_storage),
        "terminal but uncollected futures released CPU admission before owner collection");
    for (std::uint32_t i = 0; i < cpu_task_limit; ++i)
    {
        hs::AssetReadCompletion task_output;
        Check(bool(poll(task_requests[i], task_output)) && task_output.ready &&
            residency.ReleaseNow(task_output.storage),
            "accepted CPU task did not release its slot and host bytes at completion");
    }
    deferred_storage = residency.ReserveHost(contents.size() * 2);
    Check(static_cast<bool>(deferred_storage), "deferred task could not reacquire host admission");
    Check(bool(io.Enqueue({deferred_request, "generation", deferred_id, hs::RendererAssetSource::Loose(path),
        contents.size(), std::make_shared<int>(1), {}}, streaming, residency, deferred_storage,
        deferred_submitted)) && deferred_submitted,
        "deferred CPU task did not submit after earlier futures reached terminal");
    hs::AssetReadCompletion deferred_output;
    Check(bool(poll(deferred_request, deferred_output)) && deferred_output.ready &&
        residency.ReleaseNow(deferred_output.storage), "retried CPU task did not complete cleanly");
}

void CheckPackAssetReads(bool native)
{
    const auto path = std::filesystem::temp_directory_path() /
        ("hs_stream_pack_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    auto old_path = path;
    old_path += ".old";
    struct RemoveFiles
    {
        std::filesystem::path path, old_path;
        ~RemoveFiles()
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(old_path, ignored);
        }
    } cleanup{path, old_path};
    const std::array<std::byte, 4> payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    std::array<std::byte, 68> pack_bytes{};
    std::fill_n(pack_bytes.begin(), 64, std::byte{0x7a});
    std::copy(payload.begin(), payload.end(), pack_bytes.begin() + 64);
    const auto write = [&] {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(pack_bytes.data()), static_cast<std::streamsize>(pack_bytes.size()));
        file.close();
        Check(bool(file), "could not write pack fixture");
    };
    write();
    auto owner = std::make_shared<hs::ShippedContentOwner>();
    auto unbound_owner = std::make_shared<hs::ShippedContentOwner>();
    Check(bool(hs::ShippedContentOwner::Open(path, *owner)) &&
        bool(hs::ShippedContentOwner::Open(path, *unbound_owner)), "could not open pack fixture");
    const auto identity = owner->Identity();
    auto source = hs::RendererAssetSource::Packed(owner,
        {64, payload.size(), {}, hs::Sha256(payload)});
    auto unbound = hs::RendererAssetSource::Packed(unbound_owner, source.Packed().range);
    std::vector<std::byte> output;
    Check(bool(source.Read(output, payload.size())) && std::equal(output.begin(), output.end(), payload.begin(), payload.end()),
        "pack source did not read its nonzero-offset range");
    const auto previous = output;
    const std::string shader_id = "render/shader/test.dxil";
    const std::string feedback_id = "render/shader/test_feedback.dxil";
    std::array shader_bulks{
        hs::render_data::RendererBulkDescriptor{hs::MakeAssetId(shader_id),
            {64, payload.size(), hs::render_data::RenderBulkFormat::ShaderDxil, hs::Sha256(payload)}},
        hs::render_data::RendererBulkDescriptor{hs::MakeAssetId(feedback_id),
            {0, 4, hs::render_data::RenderBulkFormat::ShaderDxil, hs::Sha256(std::span(pack_bytes).first(4))}}};
    std::ranges::sort(shader_bulks, {}, [](const auto &bulk) { return bulk.asset_id.value; });
    const auto base_index = static_cast<std::uint32_t>(std::ranges::find_if(shader_bulks,
        [&](const auto &bulk) { return bulk.asset_id.value == hs::MakeAssetId(shader_id).value; }) - shader_bulks.begin());
    hs::RendererConfig shader_config;
    shader_config.packed_content = owner; shader_config.packed_bulks = shader_bulks;
    hs::render_data::MaterialProgramDescriptor program{hs::render_data::MaterialProgramKind::OpaqueScenePS,
        base_index, hs::MakeAssetId(shader_id), shader_id};
    Check(bool(hs::ReadRendererMaterialProgram(shader_config, program, output)) && output == previous,
        "dense material program did not read base shader");
    shader_config.selected_texture_feedback = true;
    Check(bool(hs::ReadRendererMaterialProgram(shader_config, program, output)) &&
        output == std::vector<std::byte>(pack_bytes.begin(), pack_bytes.begin() + 4), "dense shader feedback selection differs");
    {
        const auto effective_digest = hs::RendererProductDigest(shader_config, shader_id);
        const auto canonical_digest = hs::RendererProductDigest(shader_config, shader_id, false);
        hs::RendererAssetSource effective, canonical;
        Check(effective_digest != canonical_digest &&
            bool(hs::ResolveRendererAsset(shader_config, shader_id, effective)) &&
            bool(hs::ResolveRendererAsset(shader_config, shader_id, canonical, false)) &&
            bool(effective.Read(output)) && hs::Sha256(output) == shader_bulks[1u-base_index].range.expected_digest &&
            bool(canonical.Read(output)) && hs::Sha256(output) == shader_bulks[base_index].range.expected_digest,
            "packed feedback selection changed canonical shader identity or selected the wrong bytes");
        hs::RendererConfig loose;
        loose.selected_texture_feedback = true;
        loose.product_paths[shader_id] = path;
        loose.product_paths[feedback_id] = old_path;
        loose.product_digests[shader_id] = canonical_digest;
        loose.product_digests[feedback_id] = effective_digest;
        Check(bool(hs::ResolveRendererAsset(loose, shader_id, effective)) && effective.Loose().path == old_path &&
            bool(hs::ResolveRendererAsset(loose, shader_id, canonical, false)) && canonical.Loose().path == path &&
            hs::RendererProductDigest(loose, shader_id) == effective_digest &&
            hs::RendererProductDigest(loose, shader_id, false) == canonical_digest &&
            loose.product_paths.at(shader_id) == path && loose.product_digests.at(shader_id) == canonical_digest,
            "loose feedback selection changed canonical shader identity or selected the wrong path");
    }
    const std::array base_only{shader_bulks[base_index]};
    shader_config.packed_bulks = base_only;
    auto fallback_program = program; fallback_program.bulk_index = 0;
    Check(bool(hs::ReadRendererMaterialProgram(shader_config, fallback_program, output)) && output == previous,
        "absent feedback variant must fall back to base shader");
    shader_config.packed_bulks = shader_bulks;
    shader_config.selected_texture_feedback = false;
    Check(bool(hs::ReadRendererMaterialProgram(shader_config, program, output)) && output == previous,
        "disabled shader feedback must select base");

    hs::ResidencyManager residency;
    constexpr std::uint32_t staging = 32u * 1024u * 1024u;
    residency.UpdateBudget({true, 0, 0, 0, 0, true, staging + 1024u});
    hs::StreamingManager streaming;
    hs::RendererAssetIo io;
    const auto initialized = io.Initialize(native, native ? staging : 0, residency);
    if (native && !initialized)
    {
        std::cout << "SKIP native pack IO: " << initialized.Message() << '\n';
        return;
    }
    Check(bool(initialized) && io.NativeEnabled() == native, "pack IO initialization mode mismatch");
    io.SetWaitForReads(true);
    if (native)
    {
        // Loose immutable products retain FILE->MEMORY transport for compact
        // authenticated ranges; texture Pump defers these requests so one
        // queue submission covers every texture admitted in that pump.
        auto loose = hs::RendererAssetSource::Loose(path);
        const std::array ranges{hs::ImmutableAssetRange{64, static_cast<std::uint32_t>(payload.size()), hs::Sha256(payload)}};
        const auto range_request = streaming.Request("native-range", "native-range", "generation", 1);
        const auto range_storage = residency.ReserveHost(payload.size() * 2);
        Check(range_request && range_storage, "native range reservation failed");
        bool range_submitted{};
        Check(bool(io.Enqueue({range_request, "generation", "native-range", loose, payload.size(), owner, {},
                               std::vector<hs::ImmutableAssetRange>(ranges.begin(), ranges.end())}, streaming, residency, range_storage,
                             range_submitted, true)) && range_submitted && io.HasPendingReads(),
              "native range was not retained for deferred batch submission");
        io.SubmitQueuedReads();
        hs::AssetReadCompletion range_output;
        Check(bool(io.Poll(range_request, streaming, residency, range_output)) && range_output.ready &&
              !range_output.cancelled && std::ranges::equal(range_output.bytes.Bytes(), payload),
              "native loose range returned incorrect compact bytes");
        Check(streaming.CompleteRead(range_request, "generation") && residency.ReleaseNow(range_output.storage),
              "native range completion did not reclaim its reservation");

        const auto cancelled_range = streaming.Request("native-range-cancel", "native-range-cancel", "generation", 1);
        const auto cancelled_range_storage = residency.ReserveHost(payload.size() * 2);
        Check(cancelled_range && cancelled_range_storage, "native cancellation range reservation failed");
        bool cancel_submitted{};
        Check(bool(io.Enqueue({cancelled_range, "generation", "native-range-cancel", loose, payload.size(), owner, {},
                               std::vector<hs::ImmutableAssetRange>(ranges.begin(), ranges.end())}, streaming, residency, cancelled_range_storage,
                             cancel_submitted, true)) && cancel_submitted,
              "native cancellation range was not queued");
        io.Cancel(cancelled_range, streaming); // Must submit and retain queued storage through its terminal marker.
        hs::AssetReadCompletion cancelled_range_output;
        const auto cancelled_range_result = io.Poll(cancelled_range, streaming, residency, cancelled_range_output);
        Check(bool(cancelled_range_result), std::string(cancelled_range_result.Message()).c_str());
        Check(cancelled_range_output.ready && cancelled_range_output.cancelled &&
              cancelled_range_output.bytes.empty() && !residency.Contains(cancelled_range_storage),
              "queued native cancellation published bytes or released storage before terminal status");

        io.SetWaitForReads(false);
        for (const int mode : {0, 1, 3})
        {
            struct ReleasePrepareGate
            {
                std::shared_ptr<std::promise<void>> promise;
                bool released{};
                void ReleaseOnce()
                {
                    if (!released) { promise->set_value(); released = true; }
                }
                ~ReleasePrepareGate() { ReleaseOnce(); }
            };
            const auto id = "native-prepare-" + std::to_string(mode);
            const auto generation = mode == 3 ? "generation-old" : "generation";
            const auto request = streaming.Request(id, id, generation, 1);
            const auto storage = residency.ReserveHost(payload.size() * 2);
            auto started = std::make_shared<std::promise<void>>();
            const auto started_future = started->get_future();
            auto release = std::make_shared<std::promise<void>>();
            const auto release_future = release->get_future().share();
            ReleasePrepareGate release_gate{release};
            const auto prepare_done = std::make_shared<std::atomic_bool>(false);
            auto generation_lease = std::make_shared<int>(mode);
            std::weak_ptr<int> generation_weak = generation_lease;
            bool submitted{};
            Check(request && storage && bool(io.Enqueue({request, generation, id, loose, payload.size(),
                generation_lease, [started, release_future, prepare_done](std::span<const std::byte>) {
                    started->set_value();
                    release_future.wait();
                    prepare_done->store(true, std::memory_order_release);
                }, std::vector<hs::ImmutableAssetRange>(ranges.begin(), ranges.end())},
                streaming, residency, storage, submitted, true)) && submitted,
                "native gated preparation was not queued");
            generation_lease.reset();
            io.SubmitQueuedReads();

            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            bool prepare_started{};
            hs::AssetReadCompletion pending_output;
            while (std::chrono::steady_clock::now() < deadline)
            {
                Check(bool(io.Poll(request, streaming, residency, pending_output)) &&
                    !pending_output.ready, "native gated preparation completed before release");
                if (started_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                { prepare_started = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Check(prepare_started, "native gated preparation callback did not start");
            hs::StreamingRequestId replacement{};
            if (mode == 1) io.Cancel(request, streaming);
            if (mode == 3)
            {
                streaming.Reset();
                replacement = streaming.Request(id, id, "generation-new", 1);
                Check(replacement && replacement.value != request.value,
                    "native reset reused a pending request identity");
            }
            Check(bool(io.Poll(request, streaming, residency, pending_output)) &&
                !pending_output.ready && residency.Contains(storage) && !generation_weak.expired(),
                "native pending prepare released its host or generation owner before terminal completion");
            release_gate.ReleaseOnce();

            bool terminal{};
            bool poll_succeeded{};
            hs::AssetReadCompletion prepare_output;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto result = io.Poll(request, streaming, residency, prepare_output);
                if (!result) { terminal = true; break; }
                if (prepare_output.ready) { terminal = true; poll_succeeded = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Check(terminal && prepare_done->load(std::memory_order_acquire) &&
                !io.HasPendingReads() && generation_weak.expired(),
                "native prepare terminal path retained work, storage or its generation owner");
            if (mode == 0)
                Check(poll_succeeded && prepare_output.ready && !prepare_output.cancelled &&
                    std::ranges::equal(prepare_output.bytes.Bytes(), payload) &&
                    residency.Contains(storage) && streaming.CompleteRead(request, generation) &&
                    residency.ReleaseNow(prepare_output.storage) && !residency.Contains(storage),
                    "native successful preparation did not publish and reclaim its bytes");
            else
                Check(poll_succeeded && prepare_output.ready && prepare_output.cancelled && prepare_output.bytes.empty() &&
                    !residency.Contains(storage),
                    "native cancelled or superseded preparation published bytes");
            if (mode == 3)
                Check(streaming.Find(replacement) &&
                    streaming.Find(replacement)->state == hs::StreamingPageState::Requested &&
                    streaming.Cancel(replacement) && streaming.Collect({}) == 1,
                    "native replacement request was not retained for retry or cleaned up");
        }
        io.SetWaitForReads(true);
    }
    unsigned sequence{};
    const auto enqueue = [&](const hs::RendererAssetSource &input, std::function<void(std::span<const std::byte>)> prepare = {}) {
        const auto id = "pack-" + std::to_string(++sequence);
        const auto request = streaming.Request(id, id, "generation", 1);
        const auto storage = residency.ReserveHost(payload.size() * 2);
        Check(request && storage, "pack read reservation failed");
        bool submitted{};
        Check(bool(io.Enqueue({request, "generation", id, input, payload.size(), owner, std::move(prepare)},
            streaming, residency, storage, submitted)) && submitted, "pack read was not submitted");
        return std::pair{request, storage};
    };
    const auto read = [&](const hs::RendererAssetSource &input) {
        const auto [request, storage] = enqueue(input);
        hs::AssetReadCompletion completion;
        Check(bool(io.Poll(request, streaming, residency, completion)) && completion.ready && !completion.cancelled &&
            std::ranges::equal(completion.bytes.Bytes(), previous), "pack IO returned incorrect range bytes");
        Check(streaming.CompleteRead(request, "generation") && residency.ReleaseNow(completion.storage) &&
            !residency.Contains(storage), "pack IO did not reclaim its completed reservation");
    };
    read(source); // Establish the native binding before replacing its pathname.
    {
        const std::array ranges{
            hs::ImmutableAssetRange{0, 1, hs::Sha256(std::span(payload).first(1))},
            hs::ImmutableAssetRange{2, 2, hs::Sha256(std::span(payload).subspan(2))}};
        const auto request = streaming.Request("packed-relative-ranges", "packed-relative-ranges", "generation", 1);
        const auto storage = residency.ReserveHost(6);
        bool submitted{};
        Check(request && storage && bool(io.Enqueue({request, "generation", "packed-relative-ranges", source,
            3, owner, {}, std::vector<hs::ImmutableAssetRange>(ranges.begin(), ranges.end())},
            streaming, residency, storage, submitted)) && submitted,
            "product-relative packed ranges were not submitted");
        hs::AssetReadCompletion completion;
        Check(bool(io.Poll(request, streaming, residency, completion)) && completion.ready && !completion.cancelled &&
            std::ranges::equal(completion.bytes.Bytes(), std::array{payload[0], payload[2], payload[3]}) &&
            streaming.CompleteRead(request, "generation") && residency.ReleaseNow(completion.storage),
            "packed subresource ranges did not apply the product base exactly once");
    }
    std::filesystem::rename(path, old_path);
    pack_bytes[64] ^= std::byte{1};
    write();
    Check(owner->Identity() == identity && bool(source.Read(output)) && output == previous,
        "retained CPU pack changed after pathname replacement");
    read(source);
    read(unbound); // A new native binding must fall back to this retained CPU file.

    const auto [cancelled, cancelled_storage] = enqueue(source);
    io.Cancel(cancelled, streaming);
    hs::AssetReadCompletion cancelled_output;
    Check(bool(io.Poll(cancelled, streaming, residency, cancelled_output)) && cancelled_output.ready &&
        cancelled_output.cancelled && cancelled_output.bytes.empty() && !residency.Contains(cancelled_storage),
        "cancelled pack read published data or leaked storage");
    const auto [drained, drained_storage] = enqueue(source);
    Check(bool(io.DrainSubmitted(streaming, residency)) && !io.HasPendingReads() &&
        !residency.Contains(drained_storage) && streaming.Find(drained)->state == hs::StreamingPageState::Cancelled,
        "pack drain did not cancel and reclaim its pending read");
    std::cout << (native ? "Core pack with optional DirectStorage transport" : "CPU pack")
              << " range, replacement, digest and cancellation checks passed\n";
}
}

int main()
{
    try
    {
        hs::RendererTextureStreaming texture_working_set;
        Check(texture_working_set.SetPreferredWorkingSet({}).Succeeded(),
            "empty preferred texture set was rejected");
        const std::array invalid_texture_owners{hs::TextureStreamId{1}};
        Check(!texture_working_set.SetPreferredWorkingSet(invalid_texture_owners),
            "preferred texture set accepted an unregistered owner");
        hs::EssentialBootstrapAdmission bootstrap;
        Check(bootstrap.Configure(1'000, 400, false), "known bootstrap capacity setup failed");
        Check(bootstrap.HasCapacity(hs::ResidencySegment::Local) &&
            bootstrap.HasCapacity(hs::ResidencySegment::NonLocal) &&
            bootstrap.CanAdmit(1'000, hs::ResidencySegment::Local) &&
            bootstrap.Commit(600, hs::ResidencySegment::Local) &&
            bootstrap.Used(hs::ResidencySegment::Local) == 600 &&
            bootstrap.CanAdmit(400, hs::ResidencySegment::Local) &&
            !bootstrap.CanAdmit(401, hs::ResidencySegment::Local),
            "essential bootstrap did not enforce the exact local physical-capacity boundary");
        Check(bootstrap.CanAdmit(400, hs::ResidencySegment::NonLocal) &&
            bootstrap.Commit(300, hs::ResidencySegment::NonLocal) &&
            bootstrap.CanAdmit(100, hs::ResidencySegment::NonLocal) &&
            !bootstrap.CanAdmit(101, hs::ResidencySegment::NonLocal) &&
            bootstrap.Used(hs::ResidencySegment::Local) == 600,
            "essential bootstrap mixed local and non-local segment accounting");
        Check(bootstrap.CanAdmit(100, hs::ResidencySegment::Local) &&
            bootstrap.Used(hs::ResidencySegment::Local) == 600 &&
            !bootstrap.Commit(401, hs::ResidencySegment::Local) &&
            bootstrap.Used(hs::ResidencySegment::Local) == 600 &&
            bootstrap.Commit(100, hs::ResidencySegment::Local) &&
            bootstrap.CanAdmit(300, hs::ResidencySegment::Local) &&
            !bootstrap.CanAdmit(301, hs::ResidencySegment::Local),
            "failed or successful actual allocation charging changed bootstrap allowance incorrectly");
        Check(!bootstrap.Configure(2'000, 400, false) &&
            bootstrap.Used(hs::ResidencySegment::Local) == 700,
            "bootstrap reconfiguration discarded allocations still owned by the device");
        bootstrap.Reset();
        Check(bootstrap.Configure(0, 0, false), "unknown bootstrap capacity setup failed");
        Check(!bootstrap.HasCapacity(hs::ResidencySegment::Local) &&
            !bootstrap.HasCapacity(hs::ResidencySegment::NonLocal) &&
            !bootstrap.CanAdmit(1, hs::ResidencySegment::Local) &&
            !bootstrap.Commit(1, hs::ResidencySegment::Local),
            "unknown physical capacity became an unlimited essential bootstrap allowance");
        bootstrap.Reset();
        Check(bootstrap.Configure(512, 0, true), "UMA bootstrap capacity setup failed");
        Check(bootstrap.CanAdmit(512, hs::ResidencySegment::NonLocal) &&
            bootstrap.Commit(512, hs::ResidencySegment::NonLocal) &&
            bootstrap.Used(hs::ResidencySegment::Local) == 512 &&
            bootstrap.HasCapacity(hs::ResidencySegment::NonLocal) &&
            !bootstrap.CanAdmit(1, hs::ResidencySegment::NonLocal),
            "UMA essential bootstrap did not combine native memory into its single LOCAL segment");

        hs::ResidencyManager residency;
        residency.UpdateBudget({true, 1'000, 600, 2'000, 100});
        Check(residency.CanAdmit(250), "safe local-budget admission rejected");
        Check(!residency.CanAdmit(350), "unsafe local-budget admission accepted");
        const auto fallback = residency.Register(hs::ResidencyCategory::Texture, 80, true);
        const auto streamed = residency.Register(hs::ResidencyCategory::Texture, 200, false);
        const auto staging = residency.Register(hs::ResidencyCategory::Staging, 40, false);
        const auto nonlocal_skin = residency.Register(hs::ResidencyCategory::Skin, 64, false,
            hs::ResidencyStorage::GpuAllocation, hs::ResidencySegment::NonLocal);
        Check(fallback && streamed && staging && nonlocal_skin, "residency tickets were not allocated");
        auto accounting = residency.Accounting();
        const auto texture_category = static_cast<std::size_t>(hs::ResidencyCategory::Texture);
        const auto skin_category = static_cast<std::size_t>(hs::ResidencyCategory::Skin);
        Check(accounting.live_total == 384 && accounting.local_live_bytes[texture_category] == 280 &&
            accounting.nonlocal_live_bytes[skin_category] == 64,
            "native allocation categories or memory segments were not accounted");
        Check(!residency.Retire(fallback, {1, 0, 0}), "pinned fallback was retired");
        Check(residency.Retire(nonlocal_skin, {2, 0, 0}) && residency.Retire(nonlocal_skin, {0, 0, 4}),
            "nonlocal retirement did not merge queue completions");
        accounting = residency.Accounting();
        Check(accounting.live_total == 320 && accounting.retiring_total == 64 &&
            accounting.nonlocal_live_bytes[skin_category] == 0 &&
            accounting.nonlocal_retiring_bytes[skin_category] == 64,
            "repeated retirement moved native bytes more than once");
        Check(residency.Retire(staging, {0, 0, 3}), "staging retirement failed");
        Check(residency.Collect({0, 0, 2}) == 0, "staging reclaimed before copy completion");
        Check(residency.Collect({0, 0, 3}) == 1, "staging was not reclaimed at completion");
        accounting = residency.Accounting();
        Check(accounting.live_total == 280 && accounting.retiring_total == 64,
              "residency live accounting mismatch");
        Check(accounting.reclaimed_total == 40, "residency reclaimed accounting mismatch");
        Check(residency.ReleaseNow(streamed), "explicit resident release failed");
        accounting = residency.Accounting();
        Check(accounting.live_total == 80 && accounting.reclaimed_total == 240,
              "residency explicit release accounting mismatch");
        Check(residency.Collect({2, 0, 4}) == 1, "nonlocal native bytes were not reclaimed at completion");
        accounting = residency.Accounting();
        Check(accounting.retiring_total == 0 && accounting.nonlocal_retiring_bytes[skin_category] == 0 &&
            accounting.reclaimed_total == 304, "native collection left retired category bytes");
        residency.Reset();
        accounting = residency.Accounting();
        Check(accounting.live_total == 0 && accounting.retiring_total == 0 && accounting.reclaimed_total == 0 &&
            accounting.live_bytes[texture_category] == 0 && accounting.local_live_bytes[texture_category] == 0 &&
            !accounting.budget.valid, "reset retained native bytes or budget observations");

        // Admission uses cached domain totals on the normal reservation lifecycle.
        hs::ResidencyManager admission;
        Check(!admission.CanAdmit(1) && !admission.CanStage(1),
            "optional native or host work was admitted without an initial observation");
        admission.UpdateBudget({true, 1000, 0, 0, 0, true, 1000});
        const auto gpu_reservation = admission.ReserveGpu(300);
        const auto host_reservation = admission.ReserveHost(700);
        accounting = admission.Accounting();
        Check(accounting.gpu_reserved_total == 300 && accounting.gpu_reserved_by_segment[0] == 300 &&
            accounting.host_reserved_total == 700 && accounting.host_staging_total == 0 && accounting.live_total == 0,
            "reservation bytes were mixed with native allocations");
        auto host_facts = admission.HostAdmissionFacts();
        Check(gpu_reservation && host_reservation && admission.CanAdmit(600) && !admission.CanAdmit(601) &&
            admission.CanStage(300) && !admission.CanStage(301) &&
            host_facts.budget.host_staging_budget == 1000 && host_facts.held_bytes == 700 &&
            !host_facts.held_overflow, "reservation admission boundaries or live host facts changed");
        Check(!admission.CommitHost(host_reservation, 701) && !admission.CommitHost(host_reservation, 0) &&
            !admission.CanStage(301), "failed host commit changed admission");
        Check(admission.CommitHost(host_reservation, 400) && admission.CanStage(600) && !admission.CanStage(601),
            "host commit did not shrink held admission");
        accounting = admission.Accounting();
        Check(accounting.host_reserved_total == 0 && accounting.host_staging_total == 400 &&
            accounting.gpu_reserved_total == 300, "host commit did not transfer and shrink its storage accounting");
        const auto second_host_reservation = admission.ReserveHost(600);
        host_facts = admission.HostAdmissionFacts();
        Check(second_host_reservation && host_facts.held_bytes == 1000 && !admission.ReserveHost(1),
            "successive host reservations exceeded the live shared cap");
        Check(admission.ReleaseNow(second_host_reservation), "second host reservation release failed");
        host_facts = admission.HostAdmissionFacts();
        Check(host_facts.held_bytes == 400 && !host_facts.held_overflow &&
            admission.Retire(gpu_reservation, {1, 0, 0}) &&
            admission.Retire(gpu_reservation, {2, 0, 0}) && admission.Collect({}) == 0 &&
            !admission.CanAdmit(601) && admission.Collect({1, 0, 0}) == 0 &&
            admission.Collect({2, 0, 0}) == 1 && admission.CanAdmit(900),
            "retiring reservation admission changed before completion");
        Check(admission.ReleaseNow(host_reservation) && admission.CanStage(1000), "host abort leaked admission");
        accounting = admission.Accounting();
        Check(accounting.host_reserved_total == 0 && accounting.host_staging_total == 0 &&
            accounting.gpu_reserved_total == 0, "completed reservations retained storage accounting");
        admission.Reset();
        admission.UpdateBudget({true, 1000, 0, 0, 0, true, 1000});
        admission.RecordNativeGrowth(hs::ResidencySegment::Local, 100);
        const auto native = admission.Register(hs::ResidencyCategory::Texture, 100, false);
        Check(native && admission.CanAdmit(800) && !admission.CanAdmit(801) && admission.ReleaseNow(native) &&
            !admission.CanAdmit(801), "release incorrectly credited an old DXGI snapshot");
        admission.UpdateBudget({true, 1000, 0, 0, 0, true, 1000});
        Check(admission.CanAdmit(900) && admission.CanStage(1000), "fresh snapshot/reset admission mismatch");

        hs::ResidencyManager observations;
        hs::ResidencyBudgetSnapshot domains{true, 1'000, 100, 1'000, 200, true, 4'000};
        domains.nonlocal_valid = true;
        observations.UpdateBudget(domains);
        const auto local_growth = observations.Register(hs::ResidencyCategory::Geometry, 100, false,
            hs::ResidencyStorage::GpuAllocation, hs::ResidencySegment::Local);
        const auto nonlocal_growth = observations.Register(hs::ResidencyCategory::Staging, 120, false,
            hs::ResidencyStorage::GpuAllocation, hs::ResidencySegment::NonLocal);
        Check(observations.Accounting().gpu_growth_since_snapshot == 0,
            "logical allocation bytes were treated as observed native growth");
        hs::ResidencyManager local_pressure;
        hs::ResidencyBudgetSnapshot split_budget{true, 1'000, 0, 1'000, 0};
        split_budget.nonlocal_valid = true;
        local_pressure.UpdateBudget(split_budget);
        local_pressure.RecordNativeGrowth(hs::ResidencySegment::NonLocal, 120);
        const auto split_pressure = local_pressure.GpuPressureFacts();
        Check(split_pressure.growth_since_snapshot == 120 && split_pressure.local_growth_since_snapshot == 0 &&
            split_pressure.nonlocal_growth_since_snapshot == 120 && local_pressure.CanAdmit(900) &&
            !local_pressure.CanAdmit(901),
            "non-local native growth consumed local-only texture pressure headroom");
        observations.RecordNativeGrowth(hs::ResidencySegment::Local, 100);
        observations.RecordNativeGrowth(hs::ResidencySegment::NonLocal, 120);
        observations.UpdateBudget({});
        auto domain_accounting = observations.Accounting();
        Check(local_growth && nonlocal_growth && domain_accounting.budget.valid &&
            domain_accounting.budget.nonlocal_valid && domain_accounting.local_growth_since_snapshot == 100 &&
            domain_accounting.nonlocal_growth_since_snapshot == 120 &&
            !observations.CanAdmit(1, 10, hs::ResidencySegment::Local) &&
            !observations.CanStage(1), "failed refresh erased last-valid data or left stale admission open");
        hs::ResidencyBudgetSnapshot local_only{true, 1'000, 220, 0, 0, false, 0};
        observations.UpdateBudget(local_only);
        domain_accounting = observations.Accounting();
        Check(domain_accounting.local_growth_since_snapshot == 0 &&
            domain_accounting.nonlocal_growth_since_snapshot == 120 &&
            domain_accounting.budget.local_usage == 220 && domain_accounting.budget.nonlocal_usage == 200 &&
            observations.CanAdmit(1, 10, hs::ResidencySegment::Local) &&
            !observations.CanAdmit(1, 10, hs::ResidencySegment::NonLocal),
            "partial refresh absorbed or replaced another segment's growth/last-valid observation");
        hs::ResidencyBudgetSnapshot nonlocal_only{};
        nonlocal_only.nonlocal_valid = true;
        nonlocal_only.nonlocal_budget = 1'000;
        nonlocal_only.nonlocal_usage = 320;
        observations.UpdateBudget(nonlocal_only);
        domain_accounting = observations.Accounting();
        Check(domain_accounting.nonlocal_growth_since_snapshot == 0 &&
            domain_accounting.budget.local_usage == 220 && domain_accounting.budget.nonlocal_usage == 320,
            "successful non-local refresh did not independently absorb only its own growth");
        hs::ResidencyManager unified;
        hs::ResidencyBudgetSnapshot unified_budget{true, 1'000, 0, 0, 0, true, 1'000};
        unified_budget.unified_memory = true;
        unified.UpdateBudget(unified_budget);
        Check(unified.GpuPressureFacts().budget.unified_memory &&
              unified.HostAdmissionFacts().budget.unified_memory &&
              unified.Accounting().budget.unified_memory,
            "UMA budget topology was not published consistently to consumers");
        {
            hs::ScopedGpuAdmission admission(unified, 500, 400);
            Check(admission && unified.Accounting().gpu_reserved_by_segment[0] == 900 &&
                unified.Accounting().gpu_reserved_by_segment[1] == 0,
                "UMA upload reservation was not combined with LOCAL capacity");
        }
        unified_budget.unified_memory = false;
        unified.UpdateBudget(unified_budget);
        Check(!unified.GpuPressureFacts().budget.unified_memory &&
              !unified.HostAdmissionFacts().budget.unified_memory &&
              !unified.Accounting().budget.unified_memory,
            "updated non-UMA budget topology left stale UMA consumer facts");

        hs::StreamingManager streaming;
        const auto request = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
                                               "mips:0-3", "generation-a", 100);
        Check(request && streaming.BeginRead(request), "stream request did not enter Reading");
        Check(!streaming.CompleteRead(request, "generation-b"),
              "stale generation read completion was accepted");
        const auto stale = streaming.Find(request);
        Check(stale && stale->state == hs::StreamingPageState::Cancelled,
              "stale completion did not cancel request");
        Check(streaming.Collect({}) == 1, "cancelled request was not collected");

        const auto live = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
                                            "mips:0-3", "generation-c", 100);
        Check(!streaming.BeginUpload(live, "generation-c") && streaming.BeginRead(live),
              "payload upload bypassed the read/decode gate");
        Check(streaming.CompleteRead(live, "generation-c"), "live read completion failed");
        Check(streaming.BeginUpload(live, "generation-c"), "live upload did not begin");
        Check(!streaming.TryPublish(live, "generation-c", {}), "unsubmitted upload was published");
        Check(streaming.SubmitUpload(live, "generation-c", {0, 0, 7}),
              "live upload completion was not recorded");
        Check(!streaming.TryPublish(live, "generation-c", {0, 0, 6}),
              "stream page published before copy completion");
        Check(streaming.TryPublish(live, "generation-c", {0, 0, 7}),
              "stream page did not publish after copy completion");
        Check(streaming.Retire(live, {4, 2, 0}), "resident page did not enter Retiring");
        Check(streaming.Collect({4, 2, 6}) == 0, "retirement discarded the original upload completion");
        Check(streaming.Collect({3, 2, 7}) == 0,
              "retiring page reclaimed before all queue consumers completed");
        Check(streaming.Collect({4, 2, 7}) == 1,
              "retiring page was not reclaimed after CompletionSet");

        const auto mapping_only = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
            "mips:3-4-unmap", "generation-map", 100);
        Check(mapping_only &&
              !streaming.BeginResidentTransfer(mapping_only, "stale-generation"),
              "mapping-only admission accepted a stale request generation");
        const auto mapping_requested = streaming.Find(mapping_only);
        Check(mapping_requested && mapping_requested->state == hs::StreamingPageState::Requested &&
              !mapping_requested->read_submitted && !mapping_requested->read_finished &&
              !streaming.BeginUpload(mapping_only, "generation-map"),
              "failed mapping-only evidence changed the request or opened the payload upload transition");
        Check(streaming.BeginResidentTransfer(mapping_only, "generation-map"),
              "valid reserved-resource coarsening did not enter its explicit mapping-only state");
        Check(streaming.SubmitUpload(mapping_only, "generation-map", {0, 0, 11}) &&
              !streaming.TryPublish(mapping_only, "generation-map", {0, 0, 10}) &&
              streaming.TryPublish(mapping_only, "generation-map", {0, 0, 11}) &&
              streaming.Retire(mapping_only, {0, 0, 11}) &&
              streaming.Collect({0, 0, 11}) == 1,
              "mapping-only transition skipped its queue completion/publication lifecycle");
        const auto cancelled_mapping = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
            "mips:4-5-unmap", "generation-map-cancel", 100);
        Check(cancelled_mapping && streaming.BeginResidentTransfer(cancelled_mapping,
              "generation-map-cancel") &&
              streaming.Cancel(cancelled_mapping) && streaming.Collect({}) == 1,
              "unsubmitted mapping-only request could not be cancelled and collected");

        const auto gpu_copy = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
            "software-mip-copy:2", "generation-copy", 100);
        Check(gpu_copy && !streaming.BeginResidentTransfer(gpu_copy, "stale-generation"),
              "resident GPU copy accepted a stale generation");
        const auto copy_requested = streaming.Find(gpu_copy);
        Check(copy_requested && copy_requested->state == hs::StreamingPageState::Requested &&
              !copy_requested->read_submitted && !copy_requested->read_finished &&
              !streaming.BeginUpload(gpu_copy, "generation-copy"),
              "failed GPU-copy admission changed the request or bypassed the explicit transition");
        Check(streaming.BeginResidentTransfer(gpu_copy, "generation-copy"),
              "valid resident GPU copy did not enter its payloadless queue state");
        const auto copy_uploading = streaming.Find(gpu_copy);
        Check(copy_uploading && copy_uploading->state == hs::StreamingPageState::Uploading &&
              !copy_uploading->read_submitted && !copy_uploading->read_finished,
              "resident GPU copy fabricated a read/decode completion");
        Check(streaming.SubmitUpload(gpu_copy, "generation-copy", {0, 0, 13}) &&
              !streaming.TryPublish(gpu_copy, "generation-copy", {0, 0, 12}) &&
              streaming.TryPublish(gpu_copy, "generation-copy", {0, 0, 13}) &&
              streaming.Retire(gpu_copy, {0, 0, 13}) &&
              streaming.Collect({0, 0, 13}) == 1,
              "resident GPU copy skipped its queue completion/publication lifecycle");
        const auto cancelled_copy = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
            "software-mip-copy:3", "generation-copy-cancel", 100);
        Check(cancelled_copy && streaming.BeginResidentTransfer(cancelled_copy, "generation-copy-cancel") &&
              streaming.Cancel(cancelled_copy) && streaming.Collect({}) == 1,
              "unsubmitted resident GPU copy could not be cancelled and collected");

        const auto texture = streaming.Request("render/texture/environment_terrain_gaussian.rpkg",
                                               "mips:0-3", "generation-d", 100);
        const auto geometry = streaming.Request("render/trunk_0.rpkg",
                                                "vertices:lod0", "generation-d", 90);
        Check(texture && geometry && texture != geometry,
              "concurrent texture/geometry stream requests did not receive unique identities");
        Check(streaming.BeginRead(texture) && streaming.BeginRead(geometry),
              "concurrent stream requests did not enter Reading");
        Check(streaming.CompleteRead(texture, "generation-d") &&
                  streaming.CompleteRead(geometry, "generation-d"),
              "concurrent stream reads did not complete");
        Check(streaming.BeginUpload(texture, "generation-d") &&
                  streaming.BeginUpload(geometry, "generation-d"),
              "concurrent stream uploads did not begin");
        Check(streaming.SubmitUpload(texture, "generation-d", {0, 0, 8}) &&
                  streaming.SubmitUpload(geometry, "generation-d", {0, 0, 9}),
              "concurrent stream upload completions were not recorded");
        Check(streaming.TryPublish(texture, "generation-d", {0, 0, 8}),
              "texture page did not publish at its completion");
        Check(!streaming.TryPublish(geometry, "generation-d", {0, 0, 8}) &&
                  streaming.TryPublish(geometry, "generation-d", {0, 0, 9}),
              "geometry page publication ignored its independent completion");

        const auto cancelled = streaming.Request("render/geometry/trunk_0.rpage", "page", "generation-e", 100);
        Check(streaming.BeginRead(cancelled) && streaming.CompleteRead(cancelled, "generation-e") &&
            streaming.BeginUpload(cancelled, "generation-e") &&
            streaming.SubmitUpload(cancelled, "generation-e", {5, 3, 10}) && streaming.Cancel(cancelled),
            "submitted upload cancellation failed");
        Check(streaming.Collect({4, 3, 10}) == 0 && streaming.Find(cancelled),
            "cancelled upload lost its record before direct completion");
        Check(streaming.Collect({5, 2, 10}) == 0 && streaming.Collect({5, 3, 9}) == 0,
            "cancelled upload ignored compute or copy completion");
        Check(streaming.Collect({5, 3, 10}) == 1, "completed cancelled upload was not collected");
        streaming.Reset();
        const auto after_reset = streaming.Request("render/geometry/trunk_0.rpage", "page", "generation-f", 100);
        Check(after_reset.value > cancelled.value && !streaming.BeginRead(geometry),
            "reset aliased an old asynchronous request identity");
        CheckAsyncAssetPreparation();
        CheckPackAssetReads(false);
        CheckPackAssetReads(true);
        std::cout << "residency budget, generation-aware streaming, async preparation and CompletionSet retirement passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
