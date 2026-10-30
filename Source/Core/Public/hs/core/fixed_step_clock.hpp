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

    struct AdvanceResult
    {
        std::uint32_t tick_count{};
        Duration debt{};
        Clock::time_point first_tick_boundary{};
    };

    [[nodiscard]] AdvanceResult Advance(Clock::time_point now);
    void Realign(Clock::time_point now) noexcept;

  private:
    std::optional<Clock::time_point> last_time_;
    Duration accumulator_{};
};

} // namespace hs
