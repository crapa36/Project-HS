#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/process_info.hpp>
#include <hs/core/result.hpp>
#include <hs/core/snapshot_exchange.hpp>
#include <hs/core/windows_command_line.hpp>
#include <hs/jobs/task_system.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error(std::string(message));
    }
}

void TestResult()
{
    const auto check_success = [](const hs::Result &result) {
        Check(result.Succeeded() && static_cast<bool>(result) && result.Code() == hs::ErrorCode::None,
              "success status");
        Check(result.Subsystem().empty() && result.Message().empty() && result.Location().line() == 0 &&
                  std::string_view(result.Location().file_name()).empty(),
              "success has empty diagnostics and default location");
    };
    check_success(hs::Result::Success());

    const auto location = std::source_location::current();
    const std::string expected_message(96, 'x');
    std::string subsystem = "foundation-owned-subsystem";
    std::string message = expected_message;
    auto failure = hs::Result::Failure(hs::ErrorCode::InvalidState, subsystem, message, location);
    subsystem.assign("changed");
    message.assign("changed");
    const auto check_failure = [&](const hs::Result &result) {
        Check(!result.Succeeded() && !result && result.Code() == hs::ErrorCode::InvalidState,
              "failure status");
        Check(result.Subsystem() == "foundation-owned-subsystem" && result.Message() == expected_message,
              "failure owns subsystem and message");
        Check(result.Location().line() == location.line() && result.Location().column() == location.column() &&
                  std::string_view(result.Location().file_name()) == location.file_name() &&
                  std::string_view(result.Location().function_name()) == location.function_name(),
              "failure preserves source location");
    };
    check_failure(failure);
    auto copied = failure;
    failure = hs::Result::Success();
    check_success(failure);
    check_failure(copied);
    auto moved = std::move(copied);
    check_failure(moved);
    failure = moved;
    check_failure(failure);
    failure = std::move(moved);
    check_failure(failure);
    failure = hs::Result::Success();
    check_success(failure);
    auto success_copy = failure;
    check_success(success_copy);
    auto success_moved = std::move(success_copy);
    check_success(success_moved);

    const auto caller_line = std::source_location::current().line() + 1;
    const auto automatic = hs::Result::Failure(hs::ErrorCode::InvalidArgument, "caller", "invalid");
    Check(automatic.Location().line() == caller_line &&
              std::string_view(automatic.Location().file_name()) == location.file_name(),
          "default failure location is its call site");
    const auto none = hs::Result::Failure(hs::ErrorCode::None, "diagnostic", "retained", location);
    Check(none.Succeeded() && none.Subsystem() == "diagnostic" && none.Message() == "retained" &&
              none.Location().line() == location.line(),
          "explicit None status preserves supplied diagnostics");
}

void TestFixedStepClock()
{
    hs::FixedStepClock clock;
    const auto start = hs::FixedStepClock::Clock::time_point{};

    Check(clock.Advance(start).tick_count == 0, "first advance");

    const auto catch_up = clock.Advance(start + hs::FixedStepClock::kFixedStep * 6);
    Check(catch_up.tick_count == 4, "catch-up cap");
    Check(catch_up.debt == hs::FixedStepClock::kFixedStep * 2, "debt preserved");
    Check(clock.Advance(start + hs::FixedStepClock::kFixedStep * 6).tick_count == 2,
          "debt drained");

    hs::FixedStepClock debt_clock;
    (void)debt_clock.Advance(start);
    Check(!debt_clock.Advance(start + std::chrono::milliseconds(500)).debt_diagnostic_due,
          "debt diagnostic delayed");
    Check(debt_clock.Advance(start + std::chrono::milliseconds(3'500)).debt_diagnostic_due,
          "debt diagnostic emitted");

    debt_clock.Realign(start + std::chrono::seconds(4));
    const auto realigned = debt_clock.Advance(start + std::chrono::seconds(4));
    Check(realigned.tick_count == 0, "realign tick");
    Check(realigned.debt == hs::FixedStepClock::Duration::zero(), "realign debt");
}

std::uint64_t RunDeterministicJobs(std::size_t worker_count)
{
    hs::TaskSystem tasks(worker_count);
    hs::TaskGroup group;
    std::vector<std::uint64_t> values(10'000);

    tasks.ParallelFor(group, {"determinism"}, values.size(), 127,
                      [&values](std::size_t begin, std::size_t end) {
                          for (auto index = begin; index < end; ++index)
                          {
                              values[index] = index * 7 + 11;
                          }
                      });
    Check(tasks.Wait(group).Succeeded(), "parallel wait");

    std::uint64_t checksum = 14695981039346656037ull;
    for (const auto value : values)
    {
        checksum ^= value;
        checksum *= 1099511628211ull;
    }
    return checksum;
}

void TestTaskSystem()
{
    const auto inline_checksum = RunDeterministicJobs(0);
    Check(inline_checksum == RunDeterministicJobs(1), "worker 0/1 checksum");
    Check(inline_checksum == RunDeterministicJobs(4), "worker 0/4 checksum");

    hs::TaskSystem tasks(2);
    hs::TaskGroup outer;
    std::atomic<std::uint32_t> nested_count{};

    (void)tasks.Submit(outer, {"nested.outer"}, [&tasks, &nested_count](std::stop_token) {
        hs::TaskGroup inner;
        for (std::uint32_t index = 0; index < 64; ++index)
        {
            (void)tasks.Submit(inner, {"nested.inner"}, [&nested_count](std::stop_token) {
                nested_count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        const auto result = tasks.Wait(inner);
        if (!result)
        {
            throw std::runtime_error(result.Message().data());
        }
    });

    Check(tasks.Wait(outer).Succeeded(), "nested cooperative wait");
    Check(nested_count.load() == 64, "nested completion");

    hs::TaskGroup failing;
    (void)tasks.Submit(failing, {"expected.failure"},
                       [](std::stop_token) { throw std::runtime_error("expected"); });
    Check(tasks.Wait(failing).Code() == hs::ErrorCode::TaskFailed, "exception propagation");

    hs::TaskGroup cancelled;
    std::atomic<bool> ran{};
    cancelled.RequestCancel();
    (void)tasks.Submit(cancelled, {"cancelled"}, [&ran](std::stop_token) { ran.store(true); });
    Check(tasks.Wait(cancelled).Code() == hs::ErrorCode::Cancelled, "cancellation result");
    Check(!ran.load(), "cancelled task skipped");

    for (const auto task_count : {100u, 1'000u, 10'000u})
    {
        hs::TaskGroup submissions;
        std::atomic<std::uint32_t> completed{};
        for (std::uint32_t index = 0; index < task_count; ++index)
        {
            (void)tasks.Submit(submissions, {"submission-cost"},
                               [&completed](std::stop_token) {
                                   completed.fetch_add(1, std::memory_order_relaxed);
                               });
        }
        Check(tasks.Wait(submissions).Succeeded(), "submission-size wait");
        Check(completed.load() == task_count, "submission-size completion");
    }

    for (std::uint32_t round = 0; round < 64; ++round)
    {
        hs::TaskSystem lifecycle(2);
        hs::TaskGroup group;
        std::atomic<std::uint32_t> completed{};
        for (std::uint32_t index = 0; index < 128; ++index)
        {
            (void)lifecycle.Submit(group, {"lifecycle"}, [&completed](std::stop_token) {
                completed.fetch_add(1, std::memory_order_relaxed);
            });
        }
        Check(lifecycle.Wait(group).Succeeded(), "lifecycle wait");
        Check(completed.load() == 128, "lifecycle completion");
    }
}

void TestSnapshotExchange()
{
    hs::RenderSnapshotExchange exchange(4, 1, 1, 1);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);

    for (hs::Sequence sequence = 1; sequence <= 3; ++sequence)
    {
        auto slot = exchange.TryBeginWrite();
        Check(slot.has_value(), "snapshot write slot");
        slot->storage->header.tick = sequence;
        Check(slot->storage->AddInstance(
                  {{static_cast<float>(sequence), 0, 0}, 0, {}, 0xFFFFFFFFu,
                   hs::RenderMesh::Enemy}),
              "snapshot instance");
        Check(slot->storage->AddInstance(
                  {{static_cast<float>(sequence), 0, 1}, 0, {}, 0xFFFFFFFFu,
                   hs::RenderMesh::Pickup}),
              "snapshot grows beyond its initial reserve");
        exchange.Publish(*slot, sequence);
        const auto pair = consumer.AcquireLatest();
        Check(pair.has_current && pair.current.header.tick == sequence &&
                  pair.current_publication_sequence == sequence,
              "snapshot newest publication identity");
        if (sequence > 1)
        {
            Check(pair.has_previous && pair.previous.header.tick == sequence - 1 &&
                      pair.previous_publication_sequence == sequence - 1,
                  "snapshot previous publication identity");
        }
    }
    Check(exchange.DroppedPublishes() == 0, "snapshot no drops");
}

void TestSnapshotExchangeQueuedPublications()
{
    hs::RenderSnapshotExchange exchange(1, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    const auto publish = [&](hs::Sequence sequence, hs::Tick tick) {
        auto slot = exchange.TryBeginWrite();
        Check(slot.has_value(), "queued snapshot write slot");
        slot->storage->header.tick = tick;
        exchange.Publish(*slot, sequence);
    };
    for (hs::Sequence sequence = 1; sequence <= 3; ++sequence) publish(sequence, sequence);
    for (int read = 0; read < 4; ++read)
    {
        const auto pair = consumer.AcquireLatest();
        Check(pair.has_current && pair.current.header.tick == 3 && !pair.has_previous,
              "queued snapshots must not reverse or replace the retained pair");
    }
    publish(4, 0); // A new session can restart ticks without restarting publication order.
    auto pair = consumer.AcquireLatest();
    Check(pair.current.header.tick == 0 && pair.has_previous && pair.previous.header.tick == 3,
          "publication order is independent of session ticks");
    Check(pair.current_publication_sequence == 4 && pair.previous_publication_sequence == 3,
          "snapshot pair exposes retained publication identities");
    for (const hs::Sequence stale : {hs::Sequence{4}, hs::Sequence{2}})
    {
        publish(stale, 99);
        pair = consumer.AcquireLatest();
        Check(pair.current.header.tick == 0 && pair.previous.header.tick == 3,
              "equal and stale publications preserve both retained snapshots");
    }
    const auto abandoned = exchange.TryBeginWrite();
    Check(abandoned.has_value(), "abandoned snapshot slot");
    abandoned->storage->header.tick = 99;
    exchange.Abandon(*abandoned);
    publish(5, 1);
    pair = consumer.AcquireLatest();
    Check(pair.current.header.tick == 1 && pair.previous.header.tick == 0,
          "abandon and stale rejection release their slots");

    hs::RenderSnapshotExchange overwritten(0, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer overwrite_consumer(overwritten);
    const auto maximum = std::numeric_limits<hs::Sequence>::max();
    for (hs::Sequence sequence = maximum - 3;; ++sequence)
    {
        const auto slot = overwritten.TryBeginWrite();
        Check(slot.has_value(), "oldest ready snapshot can be overwritten");
        slot->storage->header.tick = sequence;
        overwritten.Publish(*slot, sequence);
        if (sequence == maximum) break;
    }
    for (int read = 0; read < 4; ++read)
        Check(overwrite_consumer.AcquireLatest().current.header.tick == maximum,
              "maximum publication sequence is retained without wrapping");

    hs::RenderSnapshotExchange maximum_ready(0, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer maximum_consumer(maximum_ready);
    for (hs::Sequence sequence = maximum - 2;; ++sequence)
    {
        const auto slot = maximum_ready.TryBeginWrite();
        Check(slot.has_value(), "maximum ready sequence fixture");
        maximum_ready.Publish(*slot, sequence);
        if (sequence == maximum) break;
        (void)maximum_consumer.AcquireLatest();
    }
    const auto replace_maximum = maximum_ready.TryBeginWrite();
    Check(replace_maximum.has_value(), "UINT64_MAX ready slot is still an overwrite candidate");
    maximum_ready.Abandon(*replace_maximum);
}

void TestSnapshotExchangeConcurrentOwnership()
{
    hs::RenderSnapshotExchange exchange(1, 0, 0, 0);
    hs::RenderSnapshotExchange::Consumer consumer(exchange);
    constexpr hs::Sequence publication_count = 20'000;
    std::atomic<bool> finished{}, producer_failed{};
    std::jthread producer([&](std::stop_token stop) {
        for (hs::Sequence sequence = 1; sequence <= publication_count; ++sequence)
        {
            auto slot = exchange.TryBeginWrite();
            while (!slot && !stop.stop_requested())
            {
                std::this_thread::yield();
                slot = exchange.TryBeginWrite();
            }
            if (!slot) return;
            slot->storage->header.tick = sequence;
            if (!slot->storage->AddInstance({{static_cast<float>(sequence), 0, 0}, 0, {},
                                             0xFFFFFFFFu, hs::RenderMesh::Enemy}))
                producer_failed.store(true, std::memory_order_relaxed);
            exchange.Publish(*slot, sequence);
            if ((sequence & 7u) == 0) std::this_thread::yield();
        }
        finished.store(true, std::memory_order_release);
    });
    hs::Tick current_tick{};
    const auto inspect = [&] {
        const auto pair = consumer.AcquireLatest();
        if (!pair.has_current) return;
        Check(pair.current.header.tick >= current_tick, "concurrent snapshot order is monotonic");
        current_tick = pair.current.header.tick;
        const auto previous_tick = pair.has_previous ? pair.previous.header.tick : 0;
        std::this_thread::yield(); // The producer must not rewrite either returned span.
        Check(pair.current.header.tick == current_tick && pair.current.instances.size() == 1 &&
                  pair.current.instances[0].position.x == static_cast<float>(current_tick),
              "producer preserves current snapshot storage");
        if (pair.has_previous)
            Check(pair.previous.header.tick == previous_tick && previous_tick < current_tick &&
                      pair.previous.instances.size() == 1 &&
                      pair.previous.instances[0].position.x == static_cast<float>(previous_tick),
                  "producer preserves previous snapshot storage");
    };
    do { inspect(); } while (!finished.load(std::memory_order_acquire));
    producer.join();
    for (int read = 0; read < 4; ++read) inspect();
    Check(!producer_failed.load() && current_tick == publication_count,
          "concurrent snapshot consumer receives the final publication");
}

void TestSpscQueueSize()
{
    hs::BoundedSpscQueue<std::uint32_t, 64> queue;
    std::atomic<bool> producer_done{};
    std::atomic<bool> failed{};
    std::jthread producer([&] {
        for (std::uint32_t value = 0; value < 100'000;)
        {
            if (queue.TryPush(value)) ++value;
        }
        producer_done.store(true, std::memory_order_release);
    });
    std::jthread consumer([&] {
        std::uint32_t value{};
        while (!producer_done.load(std::memory_order_acquire) || queue.Size() != 0)
        {
            (void)queue.TryPop(value);
        }
    });
    while (!producer_done.load(std::memory_order_acquire))
    {
        if (queue.Size() > 64) failed.store(true, std::memory_order_relaxed);
    }
    producer.join();
    consumer.join();
    Check(!failed.load(std::memory_order_relaxed), "SPSC observed size stays bounded");
}

void TestStaticSceneViewLifetime()
{
    auto storage = std::make_unique<hs::StaticRenderSceneStorage>();
    storage->content_generation_id = "first";
    storage->instances.resize(2);
    storage->instances[0].stable_id = 9;
    storage->instances[1].stable_id = 3;
    storage->eligible_static_members.push_back({9, 7, 0});
    const auto *instances = storage->instances.data();
    const auto first = hs::FreezeStaticRenderScene(std::move(storage));
    Check(!storage && first->instances.data() == instances && first->instances.size() == 2 &&
          first->eligible_static_members.front().stable_id == 9 &&
          first->static_keys[0].stable_id == 3 && first->static_keys[1].stable_id == 9,
          "frozen static view must borrow its stable backing and final prepared keys");
    auto replacement = std::make_unique<hs::StaticRenderSceneStorage>(*first);
    replacement->content_generation_id = "second";
    replacement->instances[0].position.x = 42;
    replacement->instances[0].stable_id = 10;
    replacement->eligible_static_members[0].stable_id = 10;
    const auto second = hs::FreezeStaticRenderScene(std::move(replacement));
    Check(second->instances.data() != first->instances.data() && first->instances[0].position.x == 0 &&
          second->instances[0].position.x == 42 && second->static_keys.back().stable_id == 10 &&
          first->content_generation_id == "first" && second->content_generation_id == "second",
          "development scene replacement must own new storage without rebinding an old leased view");
}

void TestVfxEventParameters()
{
    const hs::VfxEventParameters expected{{0.6f, 0.0f, 0.8f}, 1.75f,
                                          {4.0f, 0.3f, -2.0f},
                                          static_cast<std::uint32_t>(
                                              hs::VfxEventFlag::HasTarget)};
    const auto decoded = hs::DecodeVfxParameters(hs::EncodeVfxParameters(expected));
    Check(decoded.direction.x == expected.direction.x &&
              decoded.direction.z == expected.direction.z &&
              decoded.scale == expected.scale && decoded.target.x == expected.target.x &&
              decoded.target.z == expected.target.z && decoded.flags == expected.flags,
          "VFX event parameter round-trip");
}

void TestCookedHeaderValidation()
{
    const auto path = std::filesystem::temp_directory_path() /
                      ("hs_cooked_header_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                       ".hsbin");
    hs::CookedHeader expected;
    expected.schema_hash = 0x1234;
    expected.payload_size = 0;
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(&expected), sizeof(expected));
    }

    hs::CookedHeader actual;
    Check(static_cast<bool>(hs::ReadCookedHeader(path, actual, expected.schema_hash)),
          "cooked header read");
    Check(actual.schema_hash == expected.schema_hash, "cooked header fields");
    std::vector<std::byte> payload;
    Check(static_cast<bool>(hs::ReadCookedPayload(path, expected.schema_hash, actual, payload)),
          "cooked payload reuses header validation");
    Check(payload.empty(), "empty cooked payload");
    Check(!hs::ReadCookedHeader(path, actual, expected.schema_hash + 1),
          "cooked schema mismatch");

    expected.table_offset = 0;
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(&expected), sizeof(expected));
    }
    Check(!hs::ReadCookedHeader(path, actual), "cooked structural mismatch");
    std::filesystem::remove(path);
}

void TestProcessInfo()
{
    const auto executable = hs::CurrentExecutablePath();
    Check(executable.is_absolute() && std::filesystem::is_regular_file(executable),
          "current executable path");
    Check(hs::CurrentExecutableDirectory() == executable.parent_path(),
          "current executable directory");
    Check(hs::CurrentProcessWorkingSetBytes() > 0, "current process working set");
}

void TestWindowsCommandLineQuoting()
{
    Check(hs::QuoteWindowsCommandLineArgument(L"") == L"\"\"", "empty argument quote");
    Check(hs::QuoteWindowsCommandLineArgument(L"plain") == L"\"plain\"",
          "plain argument quote");
    Check(hs::QuoteWindowsCommandLineArgument(L"has space") == L"\"has space\"",
          "space argument quote");
    Check(hs::QuoteWindowsCommandLineArgument(L"has\"quote") == L"\"has\\\"quote\"",
          "quote argument quote");
    Check(hs::QuoteWindowsCommandLineArgument(L"slash\\\"quote") ==
              L"\"slash\\\\\\\"quote\"",
          "backslash before quote");
    Check(hs::QuoteWindowsCommandLineArgument(L"trailing\\") == L"\"trailing\\\\\"",
          "trailing backslash");
}

} // namespace

void TestShippedContentOwner();

int main(int argc, char **argv)
{
    try
    {
        const std::string_view group = argc > 1 ? argv[1] : "all";
        if (group == "all" || group == "foundation")
        {
            TestResult();
            TestFixedStepClock();
            TestSpscQueueSize();
            TestSnapshotExchange();
            TestSnapshotExchangeQueuedPublications();
            TestSnapshotExchangeConcurrentOwnership();
            TestStaticSceneViewLifetime();
            TestVfxEventParameters();
            TestCookedHeaderValidation();
            TestWindowsCommandLineQuoting();
            TestProcessInfo();
            TestShippedContentOwner();
        }
        if (group == "all" || group == "jobs") TestTaskSystem();
        Check(group == "all" || group == "foundation" || group == "jobs",
              "unknown test group");
        std::cout << "foundation_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "foundation_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
