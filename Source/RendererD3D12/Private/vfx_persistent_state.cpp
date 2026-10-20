#include "vfx_persistent_state.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

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

bool VfxPersistentState::Update(std::span<const VfxPersistentInput> inputs, std::string &error)
{
    for (const auto &input : inputs)
    {
        if (input.effect_handle == kInvalidVfxEffectHandle)
        {
            error = "Persistent VFX has an invalid effect handle";
            return false;
        }
        if (!std::isfinite(input.normalized_age) || input.normalized_age < 0.0f || input.normalized_age > 1.0f)
        {
            error = "Persistent VFX age must be finite and within [0, 1]";
            return false;
        }
    }
    std::vector<VfxPersistentInput> next(inputs.begin(), inputs.end());
    const auto key = [](const VfxPersistentInput &input) { return std::tie(input.stable_id, input.effect_handle); };
    std::sort(next.begin(), next.end(), [&](const auto &a, const auto &b) { return key(a) < key(b); });
    for (std::size_t i = 1; i < next.size(); ++i)
    {
        if (key(next[i - 1]) == key(next[i]))
        {
            error = "Persistent VFX contains a duplicate stable ID and effect handle";
            return false;
        }
    }
    active_.swap(next);
    error.clear();
    return true;
}
}
