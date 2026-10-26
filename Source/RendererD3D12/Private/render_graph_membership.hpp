#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <vector>

namespace hs::render_graph
{
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
