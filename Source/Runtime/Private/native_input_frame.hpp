#pragma once

#include <hs/core/fixed_step_clock.hpp>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace hs
{
// Only native latency uses this handoff. The render SDK owner sleeps first;
// the window owner then pumps actual input, and simulation advances its normal
// 60 Hz clock before acknowledging the same token. No renderer SDK is called
// from the window or simulation thread.
class NativeInputFrame
{
    std::mutex mutex_;
    std::condition_variable changed_;
    bool enabled_{}, cancelled_{}, input_parked_{}, simulation_parked_{};
    std::uint64_t input_request_{}, input_sampled_{}, simulation_request_{}, simulation_completed_{};
    void (*wake_main_)(void *) noexcept{};
    void *wake_context_{};
    void WakeMain() noexcept { if (wake_main_) wake_main_(wake_context_); }
  public:
    // Bound before any endpoint starts, and retained through stop/join.
    void BindMainWake(void (*notify)(void *) noexcept, void *context) noexcept
    { wake_main_ = notify; wake_context_ = context; }
    bool MainWorkPending()
    {
        std::lock_guard lock(mutex_);
        return !cancelled_ && (enabled_ ? (!input_parked_ || input_request_ != input_sampled_) : input_parked_);
    }
    bool InputGated()
    {
        std::lock_guard lock(mutex_);
        return enabled_ && !cancelled_ && input_request_ == input_sampled_;
    }
    void SetEnabled(bool enabled)
    {
        std::unique_lock lock(mutex_);
        const bool changed = enabled_ != enabled;
        enabled_ = enabled; changed_.notify_all();
        if (changed || (enabled && !input_parked_)) WakeMain();
        if (enabled)
            changed_.wait(lock, [&] { return cancelled_ || (input_parked_ && simulation_parked_); });
        // Parked flags describe the worker state until each worker actually resumes.
    }
    void Cancel()
    {
        std::lock_guard lock(mutex_); cancelled_ = true; changed_.notify_all(); WakeMain();
    }
    bool RequestInput(std::uint64_t token)
    {
        std::lock_guard lock(mutex_);
        if (!enabled_ || cancelled_ || !token || input_request_ != input_sampled_ ||
            simulation_request_ != simulation_completed_) return false;
        input_request_ = token; changed_.notify_all(); WakeMain(); return true;
    }
    bool WaitInput(std::uint64_t token)
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return cancelled_ || !enabled_ || input_sampled_ == token; });
        return !cancelled_ && enabled_ && input_sampled_ == token;
    }
    bool ShouldPump(std::uint64_t &token)
    {
        std::lock_guard lock(mutex_); token = 0;
        if (cancelled_) return false;
        if (!enabled_) { input_parked_ = false; return true; }
        if (input_request_ == input_sampled_)
        { input_parked_ = true; changed_.notify_all(); return false; }
        input_parked_ = false;
        token = input_request_; return true;
    }
    void Sampled(std::uint64_t token)
    {
        std::lock_guard lock(mutex_);
        if (!cancelled_ && token == input_request_) input_sampled_ = token;
        changed_.notify_all();
    }
    bool RequestSimulation(std::uint64_t token)
    {
        std::lock_guard lock(mutex_);
        if (cancelled_ || !enabled_ || input_sampled_ != token) return false;
        simulation_request_ = token; changed_.notify_all(); return true;
    }
    std::uint64_t AwaitSimulation()
    {
        std::unique_lock lock(mutex_);
        simulation_parked_ = enabled_;
        changed_.notify_all();
        changed_.wait(lock, [&] { return cancelled_ || !enabled_ || simulation_request_ != simulation_completed_; });
        simulation_parked_ = false;
        return !cancelled_ && enabled_ ? simulation_request_ : 0;
    }
    void Simulated(std::uint64_t token, FixedStepClock::Duration debt = {})
    {
        // Catch-up batches share a token so rendering cannot throttle the 60 Hz clock.
        if (debt >= FixedStepClock::kFixedStep) return;
        std::lock_guard lock(mutex_);
        if (!cancelled_ && token == simulation_request_) simulation_completed_ = token;
        changed_.notify_all();
    }
    bool WaitSimulation(std::uint64_t token)
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return cancelled_ || !enabled_ || simulation_completed_ == token; });
        return !cancelled_ && enabled_ && simulation_completed_ == token;
    }
};
} // namespace hs
