#pragma once

#include "vfx_workspace_size.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <type_traits>
#include <vector>

namespace hs::runtime_detail::vfx_birth_admission
{
template <typename PreparedParticles, typename RetainedParticles, typename FrozenParticles,
          typename PreparedLines, typename RetainedLines, typename FrozenLines,
          typename PreparedFlashes, typename RetainedFlashes, typename VisibleFlashes,
          typename HeldBytesFn>
void ReserveAndSelectBirthRows(std::array<std::size_t, 3> &rows,
    std::span<vfx_retained_size::BirthRequest> requests, std::vector<std::size_t> &order,
    HeldBytesFn &&measure_held_bytes,
    PreparedParticles &prepared_particles, RetainedParticles &retained_particles,
    const FrozenParticles &frozen_particles,
    PreparedLines &prepared_lines, RetainedLines &retained_lines, const FrozenLines &frozen_lines,
    PreparedFlashes &prepared_flashes, RetainedFlashes &retained_flashes,
    VisibleFlashes &visible_flashes, std::size_t required_suffix, std::size_t budget_bytes)
{
    using vfx_retained_size::Add;
    using vfx_retained_size::BirthStorage;
    using vfx_retained_size::Multiply;
    const auto reserve_pair = [](auto &prepared, auto &retained, std::size_t count) {
        prepared.reserve(count);
        if (!retained.empty()) retained.reserve(Add(retained.size(), count));
    };
    const auto reserve_birth_rows = [&](const std::array<std::size_t, 3> &counts) {
        reserve_pair(prepared_particles, retained_particles, counts[0]);
        reserve_pair(prepared_lines, retained_lines, counts[1]);
        reserve_pair(prepared_flashes, retained_flashes, counts[2]);
        visible_flashes.reserve(Add(Add(retained_flashes.size(), counts[2]), required_suffix));
    };

    reserve_birth_rows(rows);
    if (requests.empty()) return;

    // Scratch growth must be visible in the byte snapshot used by selection.
    order.reserve(requests.size());
    auto held = measure_held_bytes();
    std::array<BirthStorage, 3> storage{};
    const auto native_storage = [&](const auto &prepared, const auto &retained,
        const auto &frozen, std::size_t count) {
        using Row = typename std::remove_cvref_t<decltype(prepared)>::value_type;
        const auto native_rows = Add(retained.size(), count);
        // When retained output is nonempty, AppendPreparedBirths clears (rather
        // than swaps) the prepared vector, so its capacity can be swapped into
        // the next mixed-prefix native freeze. Empty retained output takes the
        // swap path and transfers that capacity into retained storage instead.
        const auto native_capacity = std::max(frozen.capacity(),
            retained.empty() ? std::size_t{} : prepared.capacity());
        if (native_rows > native_capacity)
            held = Add(held, Multiply(native_rows - native_capacity, sizeof(Row)));
        return BirthStorage{sizeof(Row), {prepared.capacity() - count,
            native_capacity > native_rows ? native_capacity - native_rows : 0,
            retained.empty() ? 0 : retained.capacity() - retained.size() - count},
            retained.empty() ? std::size_t{2} : std::size_t{3}};
    };
    storage[0] = native_storage(prepared_particles, retained_particles, frozen_particles, rows[0]);
    storage[1] = native_storage(prepared_lines, retained_lines, frozen_lines, rows[1]);
    storage[2] = {sizeof(typename std::remove_cvref_t<PreparedFlashes>::value_type),
        {prepared_flashes.capacity() - rows[2],
         visible_flashes.capacity() - retained_flashes.size() - rows[2] - required_suffix,
         retained_flashes.empty() ? 0 : retained_flashes.capacity() - retained_flashes.size() - rows[2]},
        retained_flashes.empty() ? std::size_t{2} : std::size_t{3}};

    (void)vfx_retained_size::SelectBirthRows(requests, storage, held, budget_bytes, order);
    for (const auto &request : requests)
        rows[request.storage] = Add(rows[request.storage], *request.admitted);
    reserve_birth_rows(rows);
}
}
