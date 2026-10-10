#include "generation_reload_watch.hpp"

#include <cstdio>

int main()
{
    using Watch = hs::runtime_detail::GenerationReloadWatch;
    using namespace std::chrono_literals;
    Watch watch;
    auto now = Watch::Clock::time_point{};
    const auto check = [](bool condition) {
        if (!condition) std::fputs("Generation reload retry contract failed.\n", stderr);
        return condition;
    };
    const auto begin = [&](std::string_view pointer, Watch::Clock::time_point time) {
        watch.Observe(pointer, time);
        return watch.TryBegin(time);
    };
    if (!check(begin("first", now))) return 1;
    if (!check(!begin("first", now + 1ms))) return 1;
    watch.Complete(false, true, now);
    if (!check(!begin("first", now + 249ms))) return 1;
    now += 250ms;
    if (!check(begin("first", now))) return 1;
    watch.Complete(true, true, now);
    if (!check(!begin("first", now + 1h))) return 1;
    // A new pointer bypasses the previous pointer's backoff.
    if (!check(begin("second", now))) return 1;
    watch.Complete(false, true, now);
    if (!check(begin("third", now))) return 1;
    for (const auto delay : {250ms, 500ms, 1000ms, 2000ms, 4000ms, 8000ms,
                             16000ms, 30000ms, 30000ms, 30000ms})
    {
        watch.Complete(false, true, now);
        if (!check(!begin("third", now + delay - 1ms))) return 1;
        now += delay;
        if (!check(begin("third", now))) return 1;
    }
    // Recovery of the same pointer is accepted even after the maximum backoff.
    watch.Complete(true, true, now);
    if (!check(!begin("third", now + 1h))) return 1;

    if (!check(begin("fourth", now))) return 1;
    if (!check(!begin("fifth", now) && !watch.IsCurrent("fourth"))) return 1;
    // A completed obsolete worker cannot settle the new pointer or add backoff.
    watch.Complete(true, false, now);
    if (!check(begin("fifth", now) && watch.IsCurrent("fifth"))) return 1;
    // Even an observed A -> B -> A change invalidates the original preparation.
    watch.Observe("sixth", now);
    watch.Observe("fifth", now);
    if (!check(!watch.IsCurrent("fifth") && !watch.TryBegin(now))) return 1;
    watch.Complete(false, false, now);
    if (!check(begin("fifth", now))) return 1;
    watch.Complete(false, true, now);
    if (!check(!begin("fifth", now + 249ms) && begin("fifth", now + 250ms))) return 1;
    watch.Complete(true, true, now + 250ms);
    return check(!begin("fifth", now + 1h)) ? 0 : 1;
}
