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

    bool diagnostic_due = false;
    if (accumulator_ >= kDebtThreshold)
    {
        if (!debt_started_at_)
        {
            debt_started_at_ = now;
        }
        else if (!debt_reported_ && now - *debt_started_at_ >= kDebtReportDelay)
        {
            debt_reported_ = true;
            diagnostic_due = true;
        }
    }
    else
    {
        debt_started_at_.reset();
        debt_reported_ = false;
    }

    return {tick_count, accumulator_, diagnostic_due};
}

void FixedStepClock::Realign(Clock::time_point now) noexcept
{
    last_time_ = now;
    debt_started_at_.reset();
    accumulator_ = Duration::zero();
    debt_reported_ = false;
}

} // namespace hs
