#pragma once

#include <hs/core/render_snapshot.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

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

// Each update is a complete authoritative frame, not a list of changes.
class VfxPersistentState
{
public:
    [[nodiscard]] bool Update(std::span<const VfxPersistentInput> inputs, std::string &error);
    [[nodiscard]] std::span<const VfxPersistentInput> Active() const noexcept { return active_; }

private:
    std::vector<VfxPersistentInput> active_;
};
}
