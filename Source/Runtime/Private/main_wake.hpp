#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace hs::runtime_detail
{
// One Main wait target; all producers publish their owned state before Notify.
class MainWake
{
  public:
    MainWake() noexcept
        : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    { if (!event_) error_.store(GetLastError(), std::memory_order_relaxed); }
    ~MainWake() { if (event_) CloseHandle(event_); }
    MainWake(const MainWake &) = delete;
    HANDLE Handle() const noexcept { return event_; }
    void Notify() noexcept
    {
        sequence_.fetch_add(1, std::memory_order_release);
        if (event_ && !SetEvent(event_)) RecordError(GetLastError());
    }
    static void NotifyOpaque(void *owner) noexcept { static_cast<MainWake *>(owner)->Notify(); }
    template<class Pending> bool PrepareWait(Pending &&pending) noexcept
    { return PrepareWait(pending, Sequence()); }
    template<class Pending> bool PrepareWait(Pending &&pending, std::uint64_t processed_sequence) noexcept
    {
        if (!ResetEvent(event_)) { RecordError(GetLastError()); return false; }
        if (pending() || sequence_.load(std::memory_order_acquire) != processed_sequence)
        { if (!SetEvent(event_)) RecordError(GetLastError()); return false; }
        return true;
    }
    std::uint64_t Sequence() const noexcept { return sequence_.load(std::memory_order_acquire); }
    DWORD Error() const noexcept { return error_.load(std::memory_order_acquire); }
  private:
    void RecordError(DWORD error) noexcept
    { DWORD empty{}; error_.compare_exchange_strong(empty, error, std::memory_order_release); }
    HANDLE event_{};
    std::atomic<std::uint64_t> sequence_{};
    std::atomic<DWORD> error_{};
};
}
