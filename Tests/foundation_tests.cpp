#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/result.hpp>
#include <hs/jobs/task_system.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
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
    const auto delayed = debt_clock.Advance(start + std::chrono::milliseconds(500));
    Check(delayed.tick_count == hs::FixedStepClock::kMaxCatchUpTicks &&
              delayed.debt == std::chrono::milliseconds(500) -
                                  hs::FixedStepClock::kFixedStep * delayed.tick_count,
          "long stalls retain debt without advancing extra simulation ticks");

    debt_clock.Realign(start + std::chrono::seconds(4));
    const auto realigned = debt_clock.Advance(start + std::chrono::seconds(4));
    Check(realigned.tick_count == 0, "realign tick");
    Check(realigned.debt == hs::FixedStepClock::Duration::zero(), "realign debt");
}

std::uint64_t RunDeterministicJobs(std::size_t worker_count)
{
    hs::TaskSystem tasks(worker_count);
    auto group = tasks.CreateTaskGroup();
    std::vector<std::uint64_t> values(10'000);

    group.ParallelFor(values.size(), 127,
                      [&values](std::size_t begin, std::size_t end) {
                          for (auto index = begin; index < end; ++index)
                          {
                              values[index] = index * 7 + 11;
                          }
                      });
    Check(group.Wait().Succeeded(), "parallel wait");

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
    auto outer = tasks.CreateTaskGroup();
    std::atomic<std::uint32_t> nested_count{};

    (void)outer.Submit([&nested_count, &tasks](std::stop_token) {
        auto inner = tasks.CreateTaskGroup();
        for (std::uint32_t index = 0; index < 64; ++index)
        {
            (void)inner.Submit([&nested_count](std::stop_token) {
                nested_count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        const auto result = inner.Wait();
        if (!result)
        {
            throw std::runtime_error(result.Message().data());
        }
    });

    Check(outer.Wait().Succeeded(), "nested cooperative wait");
    Check(nested_count.load() == 64, "nested completion");

    auto cancelled = tasks.CreateTaskGroup();
    std::atomic<bool> ran{};
    cancelled.RequestCancel();
    (void)cancelled.Submit([&ran](std::stop_token) { ran.store(true); });
    Check(cancelled.Wait().Code() == hs::ErrorCode::Cancelled, "cancellation result");
    Check(!ran.load(), "cancelled task skipped");
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

} // namespace

int main()
{
    try
    {
        TestFixedStepClock();
        TestTaskSystem();
        TestSpscQueueSize();
        std::cout << "foundation_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "foundation_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
