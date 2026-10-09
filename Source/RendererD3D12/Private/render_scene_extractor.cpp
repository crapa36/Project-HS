#include "render_scene_extractor.hpp"

#include <limits>
#include <unordered_set>

namespace hs
{

std::size_t RenderSceneExtractor::KeyHash::operator()(RenderSceneInstanceKey key) const noexcept
{
    auto value = key.stable_id;
    value ^= static_cast<std::uint64_t>(key.fallback_ordinal) +
             0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    return static_cast<std::size_t>(value ^ (value >> 32u));
}

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
        RenderSceneInstanceKey key{
            source.stable_id,
            source.stable_id == 0 ? static_cast<std::uint32_t>(index + 1) : 0u};
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
            item.revision_mask = RenderSceneTransformChanged | RenderScenePoseChanged |
                                 RenderSceneMaterialChanged | RenderSceneContentChanged;
            ++delta.created;
        }
        else
        {
            const auto &before = previous->second;
            if (before.transform != source.revisions.transform)
                item.revision_mask |= RenderSceneTransformChanged;
            if (before.pose != source.revisions.pose)
                item.revision_mask |= RenderScenePoseChanged;
            if (before.material != source.revisions.material)
                item.revision_mask |= RenderSceneMaterialChanged;
            if (before.content != source.revisions.content)
                item.revision_mask |= RenderSceneContentChanged;
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
        RenderSceneInstanceKey key{
            source.stable_id,
            source.stable_id == 0 ? static_cast<std::uint32_t>(index + 1) : 0u};
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
