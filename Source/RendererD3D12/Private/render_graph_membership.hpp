#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_set>
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

// The ordered list remains the authority for publication order. Most batches
// have few resources; only large batches allocate a membership index. Both
// contents belong to one Execute attempt; their owner may retain capacity.
template <typename Resource, typename Hash, typename Equal>
bool AppendUniqueBatchResource(std::vector<Resource> &ordered,
    std::optional<std::unordered_set<Resource, Hash, Equal>> &membership, const Resource &resource)
{
    constexpr std::size_t indexed_threshold = 1024;
    if (ordered.size() < indexed_threshold)
    {
        if (std::find(ordered.begin(), ordered.end(), resource) != ordered.end()) return false;
        ordered.push_back(resource);
        if (ordered.size() == indexed_threshold)
            membership.emplace(ordered.begin(), ordered.end());
        return true;
    }
    if (!membership->insert(resource).second) return false;
    ordered.push_back(resource);
    return true;
}
}
