#include <hs/core/fixed_step_clock.hpp>

#include <algorithm>

namespace hs
{

FixedStepClock::AdvanceResult FixedStepClock::Advance(Clock::time_point now)
{
    if (!last_time_)
    {
        last_time_ = now;
        return {};
    }

    if (now < *last_time_)
    {
        Realign(now);
        return {};
    }

    accumulator_ += std::chrono::duration_cast<Duration>(now - *last_time_);
    last_time_ = now;

    const auto available_ticks = static_cast<std::uint64_t>(accumulator_ / kFixedStep);
    const auto tick_count =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(available_ticks, kMaxCatchUpTicks));
    accumulator_ -= kFixedStep * tick_count;

    const auto first_boundary = tick_count
        ? now - accumulator_ - kFixedStep * (tick_count - 1)
        : Clock::time_point{};
    return {tick_count, accumulator_, first_boundary};
}

void FixedStepClock::Realign(Clock::time_point now) noexcept
{
    last_time_ = now;
    accumulator_ = Duration::zero();
}

} // namespace hs
