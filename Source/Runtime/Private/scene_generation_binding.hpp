#pragma once
#include <hs/core/render_snapshot.hpp>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace hs::runtime_detail
{
// Development-only generation relabeling retains the source span owner even
// after the small lookup cache or the original packet releases its entry.
struct SceneGenerationBinding
{
    std::string source_generation_id, resident_generation_id;
    struct Entry { std::shared_ptr<const StaticRenderScene> source, bound; };
    std::array<Entry, 2> scenes;
    std::shared_ptr<const StaticRenderScene> Resolve(std::shared_ptr<const StaticRenderScene> source)
    {
        if (!source) return {};
        if (source->content_generation_id != source_generation_id)
            throw std::runtime_error("Snapshot scene does not belong to the simulation generation.");
        if (source_generation_id == resident_generation_id) return source;
        for (const auto &entry : scenes) if (entry.source == source) return entry.bound;
        struct ReboundScene { std::shared_ptr<const StaticRenderScene> source; StaticRenderScene view; };
        auto owner = std::make_shared<ReboundScene>();
        owner->source = source; owner->view = *source;
        owner->view.content_generation_id = resident_generation_id;
        std::shared_ptr<const StaticRenderScene> bound(owner, &owner->view);
        scenes[1] = std::move(scenes[0]);
        scenes[0] = {std::move(source), std::move(bound)};
        return scenes[0].bound;
    }
    void Swap(SceneGenerationBinding &other) noexcept
    {
        source_generation_id.swap(other.source_generation_id);
        resident_generation_id.swap(other.resident_generation_id);
        scenes.swap(other.scenes);
    }
};
}
