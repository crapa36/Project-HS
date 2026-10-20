#include "native_input_frame.hpp"
#include <atomic>
#include <iostream>
#include <thread>

int main()
{
    hs::NativeInputFrame handoff;
    std::atomic<bool> done{};
    std::atomic<unsigned> target_ticks{}, processed_ticks{}, batches{};
    std::thread input([&] {
        while (!done.load())
        {
            std::uint64_t token{};
            if (handoff.ShouldPump(token) && token) handoff.Sampled(token);
            std::this_thread::yield();
        }
    });
    std::thread simulation([&] {
        hs::FixedStepClock clock;
        (void)clock.Advance({});
        while (!done.load())
        {
            const auto token = handoff.AwaitSimulation();
            if (!token) { std::this_thread::yield(); continue; }
            const auto now = hs::FixedStepClock::Clock::time_point{} +
                hs::FixedStepClock::kFixedStep * target_ticks.load();
            const auto advanced = clock.Advance(now);
            processed_ticks.fetch_add(advanced.tick_count);
            batches.fetch_add(1);
            handoff.Simulated(token, advanced.debt);
        }
    });
    handoff.SetEnabled(true);
    bool valid = true;
    std::uint64_t token{};
    // One render request after 24 or 60 elapsed ticks must acknowledge all due
    // ticks, while preserving the clock's maximum four ticks per batch.
    for (const unsigned target : {24u, 84u})
    {
        ++token;
        target_ticks.store(target);
        valid &= handoff.RequestInput(token) && handoff.WaitInput(token) &&
            handoff.RequestSimulation(token) && handoff.WaitSimulation(token);
        valid &= processed_ticks.load() == target;
    }
    valid &= batches.load() == 21;
    handoff.SetEnabled(false);
    handoff.SetEnabled(true);
    ++token;
    target_ticks.store(90);
    valid &= handoff.RequestInput(token) && handoff.WaitInput(token) &&
        handoff.RequestSimulation(token) && handoff.WaitSimulation(token);
    valid &= processed_ticks.load() == 90 && batches.load() == 23;
    // A zero-tick request still acknowledges its native token.
    ++token;
    valid &= handoff.RequestInput(token) && handoff.WaitInput(token) &&
        handoff.RequestSimulation(token) && handoff.WaitSimulation(token);
    valid &= processed_ticks.load() == 90 && batches.load() == 24;
    for (unsigned transition = 0; transition < 128; ++transition)
    { handoff.SetEnabled(false); handoff.SetEnabled(true); }
    done.store(true);
    handoff.Cancel();
    input.join();
    simulation.join();
    valid &= !handoff.RequestInput(++token) && !handoff.WaitSimulation(token);
    if (!valid) { std::cerr << "native input catch-up/token lifecycle failed\n"; return 1; }
    std::cout << "Native input: 24/60-tick render gaps, re-enable, zero-tick and cancellation passed.\n";
}
