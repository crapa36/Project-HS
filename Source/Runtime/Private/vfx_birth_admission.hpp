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
template <typename PreparedParticles, typename RetainedParticles,
          typename PreparedLines, typename RetainedLines,
          typename PreparedFlashes, typename RetainedFlashes, typename VisibleFlashes,
          typename HeldBytesFn>
void ReserveAndSelectBirthRows(std::array<std::size_t, 3> &rows,
    std::span<vfx_retained_size::BirthRequest> requests, std::vector<std::size_t> &order,
    HeldBytesFn &&measure_held_bytes,
    PreparedParticles &prepared_particles, RetainedParticles &retained_particles,
    PreparedLines &prepared_lines, RetainedLines &retained_lines,
    PreparedFlashes &prepared_flashes, RetainedFlashes &retained_flashes,
    VisibleFlashes &visible_flashes, std::size_t required_suffix, std::size_t budget_bytes)
{
    using vfx_retained_size::Add;
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

    if (!requests.empty())
    {
        const std::array row_bytes{
            sizeof(typename std::remove_cvref_t<PreparedParticles>::value_type),
            sizeof(typename std::remove_cvref_t<PreparedLines>::value_type),
            sizeof(typename std::remove_cvref_t<PreparedFlashes>::value_type)};
        auto held = measure_held_bytes();
        for (std::size_t i = 0; i < rows.size(); ++i)
            held = Add(held, Multiply(rows[i], row_bytes[i]));
        held = Add(held, Multiply(required_suffix, row_bytes[2]));
        (void)vfx_retained_size::SelectBirthRows(requests, row_bytes, held, budget_bytes, order);
        for (const auto &request : requests)
            rows[request.storage] = Add(rows[request.storage], *request.admitted);
    }
    reserve_birth_rows(rows);
}
}
