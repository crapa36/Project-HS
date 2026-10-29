#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/canonical_bytes.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/process_info.hpp>
#include <hs/core/result.hpp>
#include <hs/core/windows_command_line.hpp>
#include <hs/jobs/task_system.hpp>

#include <algorithm>
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
    Check(catch_up.tick_count == 4 &&
              catch_up.first_tick_boundary == start + hs::FixedStepClock::kFixedStep,
          "catch-up boundaries retain the scheduled tick positions");
    Check(catch_up.debt == hs::FixedStepClock::kFixedStep * 2, "debt preserved");
    const auto drained = clock.Advance(start + hs::FixedStepClock::kFixedStep * 6);
    Check(drained.tick_count == 2 &&
              drained.first_tick_boundary == start + hs::FixedStepClock::kFixedStep * 5,
          "debt drains from the remaining scheduled boundaries");

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
        std::uint32_t expected{};
        bool rejected_current = false;
        while (!producer_done.load(std::memory_order_acquire) || queue.Size() != 0)
        {
            if (!queue.TryPopIf(value, [&](const std::uint32_t &candidate) {
                    if (candidate != expected)
                    {
                        failed.store(true, std::memory_order_relaxed);
                        return true;
                    }
                    if (!rejected_current && candidate != 0 && candidate % 997 == 0)
                    {
                        rejected_current = true;
                        return false;
                    }
                    rejected_current = false;
                    return true;
                }))
                continue;

            if (value != expected)
                failed.store(true, std::memory_order_relaxed);
            ++expected;
        }
        if (expected != 100'000)
            failed.store(true, std::memory_order_relaxed);
    });
    while (!producer_done.load(std::memory_order_acquire))
    {
        if (queue.Size() > 64) failed.store(true, std::memory_order_relaxed);
    }
    producer.join();
    consumer.join();
    Check(!failed.load(std::memory_order_relaxed), "SPSC observed size stays bounded");
}

void TestSpscQueueConditionalPop()
{
    hs::BoundedSpscQueue<std::uint32_t, 3> queue;
    std::uint32_t output = 99;
    bool called = false;
    Check(!queue.TryPopIf(output, [&](const std::uint32_t &) {
              called = true;
              return true;
          }) &&
              !called && output == 99 && queue.Size() == 0,
          "conditional pop does not call the predicate for an empty queue");

    Check(queue.TryPush(1) && queue.TryPush(2) && queue.TryPush(3) &&
              !queue.TryPush(4),
          "conditional pop fixture fills its bounded queue");
    called = false;
    Check(!queue.TryPopIf(output, [&](const std::uint32_t &candidate) {
              called = true;
              return candidate != 1;
          }) &&
              called && output == 99 && queue.Size() == 3,
          "rejected conditional pop preserves output and FIFO head");

    try
    {
        (void)queue.TryPopIf(output, [](const std::uint32_t &) -> bool {
            throw std::runtime_error("predicate failure");
        });
        throw std::runtime_error("conditional pop swallowed a predicate exception");
    }
    catch (const std::runtime_error &exception)
    {
        Check(std::string_view(exception.what()) == "predicate failure" && output == 99 &&
                  queue.Size() == 3,
              "throwing conditional pop preserves output and FIFO head");
    }

    Check(queue.TryPopIf(output, [](const std::uint32_t &candidate) {
              return candidate == 1;
          }) &&
              output == 1 && queue.TryPush(4),
          "conditional pop can retry the same head and wrap the producer index");
    for (const auto expected : {2u, 3u, 4u})
    {
        Check(queue.TryPopIf(output, [expected](const std::uint32_t &candidate) {
                  return candidate == expected;
              }) &&
                  output == expected,
              "conditional pop preserves FIFO order across wrap");
    }
    Check(!queue.TryPopIf(output, [](const std::uint32_t &) { return true; }) &&
              queue.Size() == 0,
          "conditional pop drains wrapped queue");
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

void TestCanonicalBytes()
{
    hs::CanonicalBytes bytes("x", 1);
    bytes.Fields(std::int32_t{-2}, std::uint64_t{0x0102030405060708ull}, -0.0f);
    bytes.String("a");
    bytes.String("bc");
    const std::vector<std::byte> expected{
        std::byte{1}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{'x'},
        std::byte{1}, std::byte{}, std::byte{}, std::byte{},
        std::byte{0xfe}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{8}, std::byte{7}, std::byte{6}, std::byte{5}, std::byte{4}, std::byte{3}, std::byte{2}, std::byte{1},
        std::byte{}, std::byte{}, std::byte{}, std::byte{0x80},
        std::byte{1}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{'a'},
        std::byte{2}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{'b'}, std::byte{'c'}};
    Check(std::ranges::equal(bytes.View(), expected), "canonical bytes use length-prefixed LE fields and preserve signed zero");
    hs::CanonicalBytes negative_zero("x", 1), positive_zero("x", 1);
    negative_zero.Scalar(-0.0f);
    positive_zero.Scalar(0.0f);
    Check(!std::ranges::equal(negative_zero.View(), positive_zero.View()), "canonical float bytes preserve signed zero");
    hs::CanonicalBytes other_domain("y", 1), other_version("x", 2);
    Check(!std::ranges::equal(other_domain.View(), positive_zero.View()) &&
              !std::ranges::equal(other_version.View(), positive_zero.View()),
          "canonical bytes include domain and version");
    hs::CanonicalBytes field_order("x", 1), swapped_order("x", 1);
    field_order.Fields(std::uint32_t{1}, std::uint32_t{2});
    swapped_order.Fields(std::uint32_t{2}, std::uint32_t{1});
    Check(!std::ranges::equal(field_order.View(), swapped_order.View()), "canonical bytes preserve field order");
    hs::CanonicalBytes short_string("x", 1), long_string("x", 1);
    short_string.String("ab");
    long_string.String("a");
    long_string.String("b");
    Check(!std::ranges::equal(short_string.View(), long_string.View()), "canonical strings retain length boundaries");
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
            TestSpscQueueConditionalPop();
            TestVfxEventParameters();
            TestCookedHeaderValidation();
            TestCanonicalBytes();
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
