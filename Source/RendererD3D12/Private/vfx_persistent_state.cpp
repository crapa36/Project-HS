#include "vfx_persistent_state.hpp"

#include <algorithm>

namespace hs::renderer_detail
{
std::optional<StatusParticleBirthRange> StatusParticleBirths(
    const PersistentVfxVisual *episode, std::uint64_t owner,
    StatusVisual status, Tick current, Tick last_submitted, bool reset,
    Tick maximum_age_ticks)
{
    if (maximum_age_ticks == 0 || (!reset && current <= last_submitted)) return {};
    PersistentVfxKind kind;
    std::uint32_t cadence;
    switch (status)
    {
    case StatusVisual::Bleed: kind = PersistentVfxKind::EnemyBleedStatus; cadence = 6; break;
    case StatusVisual::Burn: kind = PersistentVfxKind::EnemyBurnStatus; cadence = 3; break;
    case StatusVisual::Slow: kind = PersistentVfxKind::EnemySlowStatus; cadence = 1; break;
    case StatusVisual::Mark: kind = PersistentVfxKind::EnemyMarkStatus; cadence = 1; break;
    default: return {};
    }
    Tick first = current;
    if (episode)
    {
        if (episode->kind != kind || episode->entity_render_id != owner || owner == 0 ||
            episode->stable_id == 0 || episode->status_episode_generation == 0 ||
            episode->active_tick > current || episode->expires <= current ||
            episode->expires <= episode->active_tick) return {};
        first = std::max(episode->active_tick,
            current >= maximum_age_ticks ? current - maximum_age_ticks + 1 : Tick{});
        if (!reset) first = std::max(first, last_submitted + 1);
    }
    // Avoid overflow in (birth + owner), including at the Tick boundary.
    const auto phase = (first % cadence + owner % cadence) % cadence;
    const auto advance = (cadence - phase) % cadence;
    if (advance > current - first) return {};
    return StatusParticleBirthRange{first + advance, current, cadence};
}

}
