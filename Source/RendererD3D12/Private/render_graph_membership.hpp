#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace hs::render_graph
{
struct ResourceAlias
{
    const void *native{};
    std::uint32_t index{}, group{};
};

// First occurrence labels preserve alias patterns when native allocations rotate.
inline void CanonicalizeResourceAliases(std::span<ResourceAlias> aliases)
{
    std::sort(aliases.begin(), aliases.end(), [](const ResourceAlias &a, const ResourceAlias &b) {
        if (a.native != b.native) return std::less<const void *>{}(a.native, b.native);
        return a.index < b.index;
    });
    const void *previous{};
    std::uint32_t group{};
    for (auto &alias : aliases)
    {
        if (!alias.native) { alias.group = 0; continue; }
        if (alias.native != previous)
        {
            previous = alias.native;
            group = alias.index + 1;
        }
        alias.group = group;
    }
}

// Resource indices are dense in [0, resource_count), matching the graph's
// logical-resource vector. The ordered list remains authoritative for order;
// the bitset is only a per-batch membership index and its owner retains storage.
inline bool AppendUniqueBatchResource(std::vector<std::uint32_t> &ordered,
    std::vector<std::uint64_t> &membership, std::uint32_t resource,
    [[maybe_unused]] std::size_t resource_count)
{
    const auto word_index = static_cast<std::size_t>(resource / 64u);
    assert(resource < resource_count && word_index < membership.size());
    const auto mask = std::uint64_t{1} << (resource % 64u);
    auto &word = membership[word_index];
    if ((word & mask) != 0) return false;
    ordered.push_back(resource);
    word |= mask;
    return true;
}

inline void EnsureBatchResourceMembershipDomain(std::vector<std::uint64_t> &membership,
    std::size_t resource_count)
{
    const auto word_count = (resource_count + 63u) / 64u;
    if (membership.size() < word_count) membership.resize(word_count, 0);
}

inline void ResetBatchResourceMembership(std::vector<std::uint32_t> &ordered,
    std::vector<std::uint64_t> &membership)
{
    std::fill(membership.begin(), membership.end(), std::uint64_t{});
    ordered.clear();
}
}
