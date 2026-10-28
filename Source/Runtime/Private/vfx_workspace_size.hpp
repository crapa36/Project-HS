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
struct BirthStorage
{
    std::size_t row_bytes{};
    std::array<std::size_t, 3> unused_rows{};
    std::size_t destinations{};
};

struct BirthRequest
{
    std::size_t count{};
    std::size_t *admitted{};
    std::uint32_t importance{};
    std::size_t storage{};
};

inline std::size_t AppendRowsWithinBudget(std::size_t row_bytes,
    std::size_t requested, std::size_t held_bytes, std::size_t budget_bytes,
    std::span<const std::size_t> unused_rows) noexcept;

// Select optional row prefixes against free slots and a remaining byte budget.
// The caller reserves order.capacity() and accounts for it in held_bytes before
// calling; the all-fit path leaves that scratch vector completely untouched.
inline std::size_t SelectBirthRows(std::span<BirthRequest> requests,
    std::span<BirthStorage> storages, std::size_t held_bytes,
    std::size_t budget_bytes, std::vector<std::size_t> &order)
{
    for (const auto &storage : storages)
        if (storage.row_bytes == 0 || storage.destinations == 0 || storage.destinations > 3)
            throw std::logic_error("invalid VFX birth storage descriptor");
    for (const auto &request : requests)
        if (!request.admitted || request.storage >= storages.size())
            throw std::logic_error("invalid VFX birth request descriptor");

    const auto growth_budget = held_bytes < budget_bytes ? budget_bytes - held_bytes : 0;
    bool all_fit = true;
    std::size_t all_fit_growth{};
    for (std::size_t storage_index = 0; storage_index < storages.size() && all_fit; ++storage_index)
    {
        const auto &storage = storages[storage_index];
        std::size_t requested{};
        for (const auto &request : requests)
        {
            if (request.storage != storage_index) continue;
            if (request.count > std::numeric_limits<std::size_t>::max() - requested)
            {
                all_fit = false;
                break;
            }
            requested += request.count;
        }
        if (!all_fit) break;
        const auto remaining_budget = growth_budget - all_fit_growth;
        const auto max_growth_rows = remaining_budget / storage.row_bytes;
        std::size_t growth_rows{};
        for (std::size_t destination = 0; destination < storage.destinations; ++destination)
        {
            const auto growth = requested > storage.unused_rows[destination]
                ? requested - storage.unused_rows[destination] : 0;
            if (growth > max_growth_rows - growth_rows)
            {
                all_fit = false;
                break;
            }
            growth_rows += growth;
        }
        if (all_fit)
        {
            const auto bytes = growth_rows * storage.row_bytes;
            all_fit_growth += bytes;
        }
    }
    if (all_fit)
    {
        for (auto &request : requests) *request.admitted = request.count;
        for (const auto &request : requests)
        {
            auto &storage = storages[request.storage];
            for (std::size_t destination = 0; destination < storage.destinations; ++destination)
                storage.unused_rows[destination] -=
                    std::min(request.count, storage.unused_rows[destination]);
        }
        return all_fit_growth;
    }

    if (order.capacity() < requests.size())
        throw std::logic_error("VFX birth order scratch was not reserved by caller");
    order.clear();
    for (std::size_t index = 0; index < requests.size(); ++index)
        order.push_back(index);
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        if (requests[left].importance != requests[right].importance)
            return requests[left].importance > requests[right].importance;
        return left < right;
    });
    for (auto &request : requests) *request.admitted = 0;

    auto remaining_bytes = growth_budget;
    std::size_t growth_bytes{};
    for (const auto index : order)
    {
        auto &request = requests[index];
        auto &storage = storages[request.storage];
        const auto unused = std::span<const std::size_t>(
            storage.unused_rows.data(), storage.destinations);
        const auto low = AppendRowsWithinBudget(storage.row_bytes, request.count, 0,
            remaining_bytes, unused);
        *request.admitted = low;
        std::size_t added_growth_rows{};
        for (std::size_t destination = 0; destination < storage.destinations; ++destination)
        {
            const auto reused = std::min(low, storage.unused_rows[destination]);
            storage.unused_rows[destination] -= reused;
            added_growth_rows += low - reused;
        }
        const auto added_bytes = added_growth_rows * storage.row_bytes;
        remaining_bytes -= added_bytes;
        growth_bytes += added_bytes;
    }
    return growth_bytes;
}

inline std::size_t Add(std::size_t a, std::size_t b) noexcept
{ return b > std::numeric_limits<std::size_t>::max() - a ? std::numeric_limits<std::size_t>::max() : a + b; }

inline std::size_t Multiply(std::size_t count, std::size_t size) noexcept
{ return count && size > std::numeric_limits<std::size_t>::max() / count
    ? std::numeric_limits<std::size_t>::max() : count * size; }

template<class Rows>
std::size_t NativeCopyGrowthBytes(const Rows &pending, const Rows &frozen,
    const Rows &prepared) noexcept
{
    const auto reusable = prepared.empty() ? prepared.capacity() : 0;
    const auto capacity = std::max(frozen.capacity(), reusable);
    return Multiply(pending.size() - std::min(pending.size(), capacity), sizeof(typename Rows::value_type));
}

inline std::size_t AppendRowsWithinBudget(std::size_t row_bytes, std::size_t requested,
    std::size_t held_bytes, std::size_t budget_bytes,
    std::span<const std::size_t> unused_rows) noexcept
{
    if (unused_rows.empty() || row_bytes == 0) return 0;
    const auto growth_rows = held_bytes < budget_bytes ? (budget_bytes - held_bytes) / row_bytes : 0;
    const auto fits = [&](std::size_t count) {
        auto remaining = growth_rows;
        for (const auto unused : unused_rows)
        {
            const auto growth = count > unused ? count - unused : 0;
            if (growth > remaining) return false;
            remaining -= growth;
        }
        return true;
    };
    if (fits(requested)) return requested;
    std::size_t low{}, high = requested;
    while (low < high)
    {
        const auto distance = high - low;
        const auto middle = low + distance / 2 + distance % 2;
        if (fits(middle)) low = middle;
        else high = middle - 1;
    }
    return low;
}

template<class T>
std::size_t AppendRowsWithinBudget(std::size_t requested, std::size_t held_bytes,
    std::size_t budget_bytes, std::span<const std::size_t> unused_rows) noexcept
{
    return AppendRowsWithinBudget(sizeof(T), requested, held_bytes, budget_bytes, unused_rows);
}

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

template<class T, class Allocator>
std::size_t Vector(const std::vector<T, Allocator> &items) noexcept
{
    // A valid vector's allocation already bounds capacity * sizeof(T).
    return items.capacity() * sizeof(T);
}

template<class Map>
std::size_t HashTable(const Map &items) noexcept
{
    // MSVC uses two bucket links and a list sentinel. Allocator/debug metadata is excluded.
    return Add(Multiply(items.bucket_count(), 2 * sizeof(void *)),
        Multiply(Add(items.size(), 1), sizeof(typename Map::value_type) + 3 * sizeof(void *)));
}
}
