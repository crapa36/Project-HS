#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>

namespace hs::renderer_detail
{
// Advance only after successful submission. Also used before marking a deleted
// dense record absent, so its last submitted bounds still invalidate old pages.
template <typename Caster>
void AdvanceShadowCasterHistory(Caster &caster) noexcept
{
    caster.previous_bounds=caster.current_bounds;
    caster.previous_revisions=caster.current_revisions;
    caster.previous_cut_revision=caster.current_cut_revision;
    caster.previous_generation=caster.instance_generation;
    caster.previous_present=caster.present;
    caster.previous_immutable_source=caster.immutable_source;
    caster.previous_skin=caster.skin;
    caster.previous_wind=caster.wind;
    caster.previous_animated_alpha=caster.animated_alpha;
}

[[nodiscard]] constexpr std::size_t ShadowCasterGroupSelector(std::uint32_t bucket,
    std::size_t bucket_count, bool terrain) noexcept
{
    return bucket < bucket_count ? bucket : bucket_count + (terrain ? 0u : 1u);
}

// The caller's selector must identify an immutable binding within this attempt.
// Different selectors still use the original first-binding-key-wins search.
template <typename Groups, typename Append>
[[nodiscard]] std::uint32_t ResolveShadowCasterGroup(std::uint32_t &cached_index,
    Groups &groups, std::uint64_t binding_key, Append &&append)
{
    if (cached_index != std::numeric_limits<std::uint32_t>::max()) return cached_index;
    const auto found = std::find_if(groups.begin(), groups.end(),
        [&](const auto &group) { return group.binding_key == binding_key; });
    const auto index = static_cast<std::uint32_t>(found - groups.begin());
    if (found == groups.end()) std::forward<Append>(append)();
    // Failed append leaves the caller's cache unpublished.
    cached_index = index;
    return index;
}

template <typename Map>
[[nodiscard]] std::uint64_t BeginShadowCasterPreparation(Map &scratch, std::uint64_t &epoch)
{
    if (epoch == std::numeric_limits<std::uint64_t>::max())
    {
        scratch.clear();
        epoch = 0;
    }
    return ++epoch;
}

[[nodiscard]] inline bool MarkShadowCasterPrepared(std::uint64_t &record_epoch,
                                                   std::uint64_t preparation_epoch) noexcept
{
    if (record_epoch == preparation_epoch) return false;
    record_epoch = preparation_epoch;
    return true;
}

template <typename Map>
void PruneShadowCasterStorage(Map &current, std::uint64_t epoch, std::size_t prepared_count)
{
    if (prepared_count != current.size())
        std::erase_if(current, [&](const auto &entry) { return entry.second.prepared_epoch != epoch; });
}

template <typename Map>
void CommitShadowCasterStorage(Map &previous, Map &scratch, Map &current)
{
    scratch = std::move(previous);
    previous = std::move(current);
}

} // namespace hs::renderer_detail
