#pragma once

#include <hs/renderer/renderer.hpp>
#include <Windows.h>
#include <chrono>
#include <utility>

namespace hs::renderer_detail
{
// Only the synchronous Initialize owner installs this pointer. Runtime reload,
// frame allocation/waits and capture=false never read a clock through these scopes.
inline thread_local RendererStartupTimings *active_startup_timings{};

class StartupTimer
{
public:
    explicit StartupTimer(RendererStartupSample *sample) noexcept : sample_(sample)
    {
        if (sample_)
        {
            const auto error = GetLastError();
            begin_ = std::chrono::steady_clock::now();
            SetLastError(error);
        }
    }
    ~StartupTimer() { Stop(); }
    void Stop() noexcept
    {
        if (!sample_) return;
        const auto error = GetLastError();
        sample_->nanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin_).count());
        ++sample_->calls;
        sample_ = nullptr;
        SetLastError(error);
    }
    StartupTimer(const StartupTimer &) = delete;
    StartupTimer &operator=(const StartupTimer &) = delete;
private:
    RendererStartupSample *sample_{};
    std::chrono::steady_clock::time_point begin_{};
};
class StartupStepScope : public StartupTimer
{
public:
    explicit StartupStepScope(RendererStartupStep step) noexcept
        : StartupTimer(active_startup_timings ? &active_startup_timings->steps[static_cast<std::size_t>(step)] : nullptr) {}
};
class StartupOperationScope : public StartupTimer
{
public:
    explicit StartupOperationScope(RendererStartupOperation operation) noexcept
        : StartupTimer(active_startup_timings ? &active_startup_timings->operations[static_cast<std::size_t>(operation)] : nullptr) {}
};
template<class Function>
decltype(auto) MeasureStartupStep(RendererStartupStep step, Function &&function)
{
    StartupStepScope timer(step);
    return std::forward<Function>(function)();
}
template<class Function>
decltype(auto) MeasureStartupOperation(RendererStartupOperation operation, Function &&function)
{
    StartupOperationScope timer(operation);
    return std::forward<Function>(function)();
}
class StartupCapture
{
public:
    StartupCapture(RendererStartupTimings &timings, bool enabled) noexcept
        : timings_(timings), previous_(active_startup_timings)
    {
        timings_ = {};
        timings_.captured = enabled;
        active_startup_timings = enabled ? &timings_ : nullptr;
        if (!enabled) return;
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        timings_.begin_qpc = static_cast<std::uint64_t>(counter.QuadPart);
        timings_.qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
        begin_ = std::chrono::steady_clock::now();
    }
    ~StartupCapture()
    {
        if (timings_.captured)
        {
            timings_.total_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin_).count());
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            timings_.end_qpc = static_cast<std::uint64_t>(counter.QuadPart);
        }
        active_startup_timings = previous_;
    }
    void Complete() noexcept { timings_.completed = true; }
    StartupCapture(const StartupCapture &) = delete;
    StartupCapture &operator=(const StartupCapture &) = delete;
private:
    RendererStartupTimings &timings_;
    RendererStartupTimings *previous_{};
    std::chrono::steady_clock::time_point begin_{};
};
} // namespace hs::renderer_detail
