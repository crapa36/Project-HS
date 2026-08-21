#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/snapshot_exchange.hpp>
#include <hs/core/windows_command_line.hpp>
#include <hs/jobs/task_system.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
        Check(pair.has_current && pair.current.header.tick == sequence, "snapshot newest");
        if (sequence > 1)
        {
            Check(pair.has_previous && pair.previous.header.tick == sequence - 1,
                  "snapshot previous");
        }
    }
    Check(exchange.DroppedPublishes() == 0, "snapshot no drops");
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

int main(int argc, char **argv)
{
    try
    {
        const std::string_view group = argc > 1 ? argv[1] : "all";
        if (group == "all" || group == "foundation")
        {
            TestFixedStepClock();
            TestSpscQueueSize();
            TestSnapshotExchange();
            TestVfxEventParameters();
            TestCookedHeaderValidation();
            TestWindowsCommandLineQuoting();
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
