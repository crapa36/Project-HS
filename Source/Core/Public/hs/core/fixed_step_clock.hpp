#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace hs
{

class FixedStepClock
{
  public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;

    static constexpr Duration kFixedStep{16'666'667};
    static constexpr std::uint32_t kMaxCatchUpTicks = 4;
    static constexpr Duration kDebtThreshold = std::chrono::milliseconds(250);
    static constexpr Duration kDebtReportDelay = std::chrono::seconds(3);

    struct AdvanceResult
    {
        std::uint32_t tick_count{};
        Duration debt{};
        bool debt_diagnostic_due{};
    };

    [[nodiscard]] AdvanceResult Advance(Clock::time_point now);
    void Realign(Clock::time_point now) noexcept;

  private:
    std::optional<Clock::time_point> last_time_;
    std::optional<Clock::time_point> debt_started_at_;
    Duration accumulator_{};
    bool debt_reported_{};
};

} // namespace hs
