#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

namespace hs::runtime_detail
{
class GenerationReloadWatch
{
  public:
    using Clock = std::chrono::steady_clock;

    void Observe(std::string_view pointer, Clock::time_point now)
    {
        if (pointer != observed_)
        {
            superseded_ |= preparing_;
            observed_ = pointer;
            settled_ = false;
            failures_ = 0;
            next_attempt_ = now;
        }
    }

    [[nodiscard]] bool TryBegin(Clock::time_point now) noexcept
    {
        if (preparing_ || settled_ || now < next_attempt_) return false;
        preparing_ = true;
        superseded_ = false;
        return true;
    }

    [[nodiscard]] bool IsCurrent(std::string_view pointer) const noexcept
    {
        return preparing_ && !superseded_ && pointer == observed_;
    }

    void Complete(bool success, bool current, Clock::time_point now) noexcept
    {
        preparing_ = false;
        if (!current || superseded_)
        {
            settled_ = false;
            failures_ = 0;
            next_attempt_ = now;
            return;
        }
        settled_ = success;
        if (!success)
        {
            // shortcut: undifferentiated failures retry after capped backoff; classify them if costly rejections become common.
            const auto delay = std::min(30000u, 250u << std::min(failures_, 7u));
            failures_ = std::min(failures_ + 1, 7u);
            next_attempt_ = now + std::chrono::milliseconds(delay);
        }
    }

  private:
    std::string observed_;
    Clock::time_point next_attempt_{};
    unsigned failures_{};
    bool settled_{};
    bool preparing_{};
    bool superseded_{};
};
} // namespace hs::runtime_detail
