#include "render_scene_extractor.hpp"

#include <limits>
#include <unordered_set>

namespace hs
{

bool RenderSceneExtractor::Build(std::span<const RenderInstance> instances,
                                 RenderSceneDelta &delta, bool reset_history)
{
    Abort();
    delta = {};
    if (instances.size() > std::numeric_limits<std::uint32_t>::max()) return false;

    pending_.reserve(instances.size());
    delta.instances.reserve(instances.size());
    for (std::size_t index = 0; index < instances.size(); ++index)
    {
        auto source = instances[index];
        PopulateRenderInstanceRevisions(source);
        const auto key = MakeRenderSceneInstanceKey(
            source.stable_id, static_cast<std::uint32_t>(index));
        if (key.fallback_ordinal == 0 && key.stable_id == 0) return false;
        if (!pending_.emplace(key, source.revisions).second) return false;

        RenderSceneDeltaItem item;
        item.key = key;
        item.source_index = static_cast<std::uint32_t>(index);
        item.revisions = source.revisions;
        const auto previous = reset_history ? previous_.end() : previous_.find(key);
        if (previous == previous_.end())
        {
            item.kind = RenderSceneDeltaKind::Created;
            item.revision_mask = kRenderSceneAllRevisions;
            ++delta.created;
        }
        else
        {
            item.revision_mask = CompareRenderInstanceRevisions(
                previous->second, source.revisions);
            item.kind = item.revision_mask == 0 ? RenderSceneDeltaKind::Unchanged
                                                : RenderSceneDeltaKind::Updated;
            if (item.kind == RenderSceneDeltaKind::Updated) ++delta.updated;
            else ++delta.unchanged;
        }
        delta.instances.push_back(item);
    }

    delta.destroyed.reserve(previous_.size());
    for (const auto &[key, revisions] : previous_)
    {
        static_cast<void>(revisions);
        if (!reset_history && !pending_.contains(key)) delta.destroyed.push_back(key);
    }
    pending_valid_ = true;
    return true;
}

bool RenderSceneExtractor::Adopt(std::span<const RenderInstance> instances,
                                 bool reset_history)
{
    static_cast<void>(reset_history);
    Abort();
    if (instances.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    pending_.reserve(instances.size());
    for (std::size_t index = 0; index < instances.size(); ++index)
    {
        auto source = instances[index];
        PopulateRenderInstanceRevisions(source);
        const auto key = MakeRenderSceneInstanceKey(
            source.stable_id, static_cast<std::uint32_t>(index));
        if (key.fallback_ordinal == 0 && key.stable_id == 0) return false;
        if (!pending_.emplace(key, source.revisions).second) return false;
    }
    pending_valid_ = true;
    return true;
}

void RenderSceneExtractor::Commit() noexcept
{
    if (!pending_valid_) return;
    previous_.swap(pending_);
    Abort();
}

void RenderSceneExtractor::Abort() noexcept
{
    pending_.clear();
    pending_valid_ = false;
}

void RenderSceneExtractor::Clear() noexcept
{
    previous_.clear();
    Abort();
}

} // namespace hs
