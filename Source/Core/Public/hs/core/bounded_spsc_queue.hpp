#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hs
{

template <typename T, std::size_t Capacity> class BoundedSpscQueue
{
    static_assert(Capacity > 0);
    static_assert(std::is_trivially_copyable_v<T>);

  public:
    [[nodiscard]] bool TryPush(const T &value) noexcept
    {
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) == Capacity)
        {
            return false;
        }

        values_[write % Capacity] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool TryPop(T &value) noexcept
    {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire))
        {
            return false;
        }

        value = values_[read % Capacity];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    void ClearConsumer() noexcept
    {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }

    [[nodiscard]] std::size_t Size() const noexcept
    {
        const auto write = write_.load(std::memory_order_acquire);
        const auto read = read_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(write - read);
    }

  private:
    alignas(64) std::atomic<std::uint64_t> write_{};
    alignas(64) std::atomic<std::uint64_t> read_{};
    std::array<T, Capacity> values_{};
};

} // namespace hs
