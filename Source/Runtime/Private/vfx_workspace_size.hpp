#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace hs::runtime_detail::vfx_retained_size
{
struct BirthRequest
{
    std::size_t count{};
    std::size_t *admitted{};
    std::uint32_t importance{};
    std::size_t storage{};
};

// The all-fit path leaves the scratch vector untouched.
inline std::size_t SelectBirthRows(std::span<BirthRequest> requests,
    std::span<const std::size_t> row_bytes, std::size_t held_bytes,
    std::size_t budget_bytes, std::vector<std::size_t> &order)
{
    const auto available = held_bytes < budget_bytes ? budget_bytes - held_bytes : 0;
    auto remaining = available;
    bool all_fit = true;
    for (const auto &request : requests)
    {
        const auto bytes = row_bytes[request.storage];
        if (request.count > remaining / bytes) { all_fit = false; break; }
        remaining -= request.count * bytes;
    }
    if (all_fit)
    {
        for (auto &request : requests) *request.admitted = request.count;
        return available - remaining;
    }
    order.clear();
    order.reserve(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) order.push_back(index);
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        if (requests[left].importance != requests[right].importance)
            return requests[left].importance > requests[right].importance;
        return left < right;
    });
    remaining = available;
    for (const auto index : order)
    {
        auto &request = requests[index];
        const auto bytes = row_bytes[request.storage];
        const auto admitted = std::min(request.count, remaining / bytes);
        *request.admitted = admitted;
        remaining -= admitted * bytes;
    }
    return available - remaining;
}

inline std::size_t Add(std::size_t a, std::size_t b) noexcept
{ return b > std::numeric_limits<std::size_t>::max() - a ? std::numeric_limits<std::size_t>::max() : a + b; }

inline std::size_t Multiply(std::size_t count, std::size_t size) noexcept
{ return count && size > std::numeric_limits<std::size_t>::max() / count
    ? std::numeric_limits<std::size_t>::max() : count * size; }

template<class T, class Allocator>
void ReserveAppend(std::vector<T, Allocator> &items, std::size_t count)
{
    const auto maximum = items.max_size();
    if (count > maximum - items.size())
        throw std::length_error("VFX append exceeds vector capacity");
    const auto needed = items.size() + count;
    const auto capacity = items.capacity();
    if (needed <= capacity) return;
    const auto grown = capacity + std::min(maximum - capacity, capacity / 2 + 1);
    items.reserve(std::max(needed, grown));
}
}
