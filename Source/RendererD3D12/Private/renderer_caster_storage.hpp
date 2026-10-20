#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <type_traits>
#include <utility>
#include <vector>

namespace hs::renderer_detail
{
// Addresses, never payloads or iterators. The strong immutable source lease and
// complete ordered leaf mapping must match before any saved address is read.
template <typename Map, typename Lease>
struct ShadowCasterNodeCache
{
    ShadowCasterNodeCache() = default;
    ShadowCasterNodeCache(const ShadowCasterNodeCache &) = delete;
    ShadowCasterNodeCache &operator=(const ShadowCasterNodeCache &) = delete;
    ShadowCasterNodeCache(ShadowCasterNodeCache &&) noexcept = default;
    ShadowCasterNodeCache &operator=(ShadowCasterNodeCache &&) noexcept = default;

    struct Entry
    {
        std::uint32_t source_index{}, instance_generation{};
        typename Map::mapped_type *node{};
    };
    Lease lease{};
    std::uint32_t generation{};
    bool valid{};
    std::vector<Entry> entries;

    void Invalidate() noexcept
    {
        valid = false;
        lease = {};
        generation = 0;
        entries.clear();
    }

    template <typename Refs>
    [[nodiscard]] bool Matches(const Lease &source, std::uint32_t source_generation,
                               const Refs &refs) const noexcept
    {
        if (!valid || lease != source || generation != source_generation || entries.size() != refs.size())
            return false;
        for (std::size_t index = 0; index < refs.size(); ++index)
            if (entries[index].source_index != refs[index].instance_index ||
                entries[index].instance_generation != refs[index].instance_generation) return false;
        return true;
    }

    template <typename Refs>
    void Prepare(const Lease &source, std::uint32_t source_generation, const Refs &refs)
    {
        Invalidate();
        entries.reserve(refs.size());
        for (const auto &ref : refs)
            entries.push_back({ref.instance_index, ref.instance_generation, nullptr});
        lease = source;
        generation = source_generation;
    }
};

template <typename Map, typename Cache>
[[nodiscard]] typename Map::mapped_type *FindPreviousShadowCasterNode(Map &previous,
    const typename Map::key_type &key, Cache *cache, std::size_t ordinal, bool warm)
{
    assert(!cache || ordinal < cache->entries.size());
    if (cache && warm) return cache->entries[ordinal].node; // Null is a cached absence.
    const auto found = previous.find(key);
    auto *node = found != previous.end() ? &found->second : nullptr;
    if (cache) cache->entries[ordinal].node = node;
    return node;
}

template <typename Map, typename Cache>
[[nodiscard]] typename Map::mapped_type *FindCurrentShadowCasterNode(Map &current,
    const typename Map::key_type &key, Cache *cache, std::size_t ordinal)
{
    assert(!cache || ordinal < cache->entries.size());
    if (cache && cache->entries[ordinal].node) return cache->entries[ordinal].node;
    auto *node = &current.try_emplace(key).first->second;
    if (cache) cache->entries[ordinal].node = node;
    return node;
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

template <typename Map, typename Cache>
[[nodiscard]] std::uint64_t BeginShadowCasterPreparation(Map &scratch, std::uint64_t &epoch,
    Cache &scratch_nodes, Cache &attempt_nodes)
{
    attempt_nodes = std::move(scratch_nodes);
    // A failed attempt destroys the moved nodes. No owner may retain their addresses.
    scratch_nodes.Invalidate();
    if (epoch == std::numeric_limits<std::uint64_t>::max()) attempt_nodes.Invalidate();
    return BeginShadowCasterPreparation(scratch, epoch);
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

// Independent Debug membership oracle. Only keys/attempt epochs are retained;
// submitted caster records and cached node addresses never enter this map.
template <typename Map>
void BeginShadowCasterKeyOracle(Map &oracle, std::uint64_t epoch, bool &prepared)
{
    assert(epoch != 0);
    // A failed attempt owns no reusable membership; wrap cannot reuse an ancient epoch 1.
    if (!prepared || epoch == 1) oracle.clear();
    prepared = false;
}

template <typename Map>
[[nodiscard]] bool MarkShadowCasterKeyOracle(Map &oracle,
    const typename Map::key_type &key, std::uint64_t epoch)
{
    const auto [entry, allocated] = oracle.try_emplace(key, epoch);
    const bool first = allocated || entry->second != epoch;
    entry->second = epoch;
    return first;
}

template <typename Map>
void PruneShadowCasterKeyOracle(Map &oracle, std::uint64_t epoch, std::size_t prepared_count,
    bool &prepared)
{
    if (oracle.size() != prepared_count)
        std::erase_if(oracle, [&](const auto &entry) { return entry.second != epoch; });
    prepared = true;
}

template <typename Map>
void CommitShadowCasterStorage(Map &previous, Map &scratch, Map &current)
{
    scratch = std::move(previous);
    previous = std::move(current);
}

template <typename Map, typename Cache>
void CommitShadowCasterStorage(Map &previous, Map &scratch, Map &current,
    Cache &previous_nodes, Cache &scratch_nodes, Cache &current_nodes,
    Cache &prepared_previous_nodes, bool previous_nodes_warm) noexcept
{
    static_assert(std::is_nothrow_move_assignable_v<Map> && std::is_nothrow_move_assignable_v<Cache>);
    CommitShadowCasterStorage(previous, scratch, current);
    scratch_nodes = previous_nodes_warm ? std::move(previous_nodes) : std::move(prepared_previous_nodes);
    previous_nodes = std::move(current_nodes);
}
} // namespace hs::renderer_detail
