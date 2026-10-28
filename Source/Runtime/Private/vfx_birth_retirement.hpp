#pragma once

#include "vfx_typed_ground_commands.hpp"

#include <algorithm>
#include <cassert>
#include <span>
#include <utility>
#include <vector>

namespace hs::runtime_detail::vfx_birth_retirement
{
template <typename ShouldRetire>
inline std::size_t RemoveRetiredRows(std::vector<ParticleSpawnCommand> &rows,
    std::size_t retained_prefix, ShouldRetire &&should_retire) noexcept
{
    assert(retained_prefix <= rows.size());
    auto write = rows.begin();
    std::size_t removed_from_prefix{};
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        auto &row = rows[index];
        if (row.count == 0 || should_retire(row))
        {
            removed_from_prefix += index < retained_prefix;
            continue;
        }
        if (write != rows.begin() + index) *write = std::move(row);
        ++write;
    }
    rows.erase(write, rows.end());
    return retained_prefix - removed_from_prefix;
}

inline std::size_t RetireCompatibilityBirthRows(std::vector<ParticleSpawnCommand> &rows,
    std::size_t retained_prefix,
    std::span<const Sequence> replaced_particle_sequences,
    std::span<const Sequence> owned_status_sequences,
    std::span<const Sequence> replaced_legacy_mesh_sequences,
    std::span<const Sequence> replaced_burst_shell_sequences) noexcept
{
    if (replaced_particle_sequences.empty() && replaced_legacy_mesh_sequences.empty()) return retained_prefix;
    return RemoveRetiredRows(rows, retained_prefix, [&](const auto &row) noexcept {
        const bool replaced_particle =
            std::ranges::binary_search(replaced_particle_sequences, row.sequence) &&
            (!row.authored_ballistic || std::ranges::binary_search(owned_status_sequences, row.sequence));
        const bool replaced_mesh = !row.authored_ballistic && row.renderer == VfxRenderer::Mesh &&
            std::ranges::binary_search(replaced_legacy_mesh_sequences, row.sequence) &&
            (row.primitive != VfxPrimitive::ShockShell ||
             std::ranges::binary_search(replaced_burst_shell_sequences, row.sequence));
        return replaced_particle || replaced_mesh;
    });
}

inline std::size_t RetireGroundSuppressedBirthRows(std::vector<ParticleSpawnCommand> &rows,
    std::size_t retained_prefix,
    std::span<const VfxGroundBirthSuppressionFacts> facts) noexcept
{
    if (facts.empty()) return retained_prefix;
    return RemoveRetiredRows(rows, retained_prefix, [&](const auto &row) noexcept {
        const auto fact = std::ranges::lower_bound(facts, row.sequence, {},
            &VfxGroundBirthSuppressionFacts::sequence);
        if (fact == facts.end() || fact->sequence != row.sequence) return false;
        return fact->all_particles ||
            (fact->ground_ring && row.renderer == VfxRenderer::Ground && row.primitive == VfxPrimitive::Ring) ||
            (fact->ground_disc && row.renderer == VfxRenderer::Ground && row.primitive == VfxPrimitive::Disc) ||
            (fact->segment_solid_trail && row.renderer == VfxRenderer::Segment &&
             row.primitive == VfxPrimitive::SolidTrail);
    });
}
}
