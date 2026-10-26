#pragma once

#include <hs/render_data/render_snapshot.hpp>
#include <optional>

namespace hs::renderer_detail
{
struct StatusParticleBirthRange
{
    Tick first{}, last{};
    std::uint32_t cadence{};
};

// Called for a currently active status bit. Without episode provenance only
// the current tick is known; never infer an unobserved status interval.
[[nodiscard]] std::optional<StatusParticleBirthRange> StatusParticleBirths(
    const PersistentVfxVisual *episode, std::uint64_t owner,
    StatusVisual status, Tick current, Tick last_submitted, bool reset,
    Tick maximum_age_ticks);

}
