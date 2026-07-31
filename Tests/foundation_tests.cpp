#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/snapshot_exchange.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/jobs/task_system.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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

hs::GameplayChecksum RunSimulation()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({0x12345678u}).Succeeded(), "simulation initialize");
    hs::GameplayChecksum checksum{};
    for (hs::Tick tick = 1; tick <= 240; ++tick)
    {
        hs::InputFrame input;
        input.target_tick = tick;
        input.held.normalized_move = tick <= 120 ? hs::Float2{1.0f, 1.0f}
                                                 : hs::Float2{-1.0f, 0.0f};
        checksum = simulation.TickFixed(input, hs::FixedStepClock::kFixedStep).checksum;
    }
    Check(simulation.Shutdown().Succeeded(), "simulation shutdown");
    return checksum;
}

void TestGameplayDeterminism()
{
    Check(RunSimulation() == RunSimulation(), "repeated gameplay checksum");

    hs::GameSimulation particle_simulation;
    Check(particle_simulation.Initialize({1}).Succeeded(), "particle simulation initialize");
    std::uint32_t particle_count{};
    for (hs::Tick tick = 1; tick <= 180; ++tick)
    {
        hs::InputFrame particle_input;
        particle_input.target_tick = tick;
        (void)particle_simulation.TickFixed(particle_input, hs::FixedStepClock::kFixedStep);
        for (const auto &spawn : particle_simulation.PendingParticleSpawns())
        {
            Check(spawn.tick == tick && spawn.sprite_count == 4,
                  "particle spawn command contract");
            particle_count += spawn.count;
        }
        particle_simulation.ClearParticleSpawns();
        particle_simulation.ClearPresentationEvents();
    }
    Check(particle_count == 10'000, "particle demo count");
    Check(particle_simulation.Shutdown().Succeeded(), "particle simulation shutdown");

    hs::GameSimulation simulation;
    Check(simulation.Initialize({1}).Succeeded(), "diagonal simulation initialize");
    hs::InputFrame input;
    input.held.normalized_move = {1.0f, 1.0f};
    const auto first = simulation.TickFixed(input, hs::FixedStepClock::kFixedStep);
    Check(first.tick == 1 && first.checksum != 0, "diagonal simulation tick");
    Check(simulation.Shutdown().Succeeded(), "diagonal simulation shutdown");
}

} // namespace

int main()
{
    try
    {
        TestFixedStepClock();
        TestTaskSystem();
        TestSnapshotExchange();
        TestGameplayDeterminism();
        std::cout << "foundation_tests passed\n";
        return 0;
    }
    catch (const std::exception &exception)
    {
        std::cerr << "foundation_tests failed: " << exception.what() << '\n';
        return 1;
    }
}
