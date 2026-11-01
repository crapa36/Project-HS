#pragma once

#include <hs/render_data/render_snapshot_values.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

namespace hs
{
using RenderContentKey = std::array<std::uint64_t, 6>;
inline RenderContentKey MakeRenderContentKey(const RenderInstance &instance) noexcept
{
    return {instance.asset_id.value, static_cast<std::uint64_t>(instance.mesh),
        instance.environment_seed, instance.environment_variant,
        instance.vfx_owner_id, instance.revisions.content};
}

class RenderInstanceLookup
{
  public:
    void Update(std::span<const RenderInstance> instances)
    {
        sources_ = instances;
        if (valid_ && instances.size() == previous_ids_.size() &&
            std::equal(instances.begin(), instances.end(), previous_ids_.begin(),
                [](const auto &instance, auto id) { return instance.stable_id == id; }))
            return;
        valid_ = false;
        ++generation_;
        if (instances.size() > previous_ids_.capacity())
        {
            previous_ids_.reserve(instances.size());
            entries_.reserve(instances.size());
        }
        previous_ids_.clear();
        for (std::size_t index = 0; index < instances.size(); ++index)
        {
            const auto id = instances[index].stable_id;
            if (id != 0)
            {
                const auto [entry, inserted] = entries_.try_emplace(id, Entry{index, generation_});
                if (!inserted && entry->second.generation != generation_)
                    entry->second = {index, generation_};
            }
            previous_ids_.push_back(id);
        }
        // Retained entries all have one generation, including across unsigned wrap.
        std::erase_if(entries_, [this](const auto &entry) {
            return entry.second.generation != generation_;
        });
        valid_ = true;
    }

    [[nodiscard]] const RenderInstance *Find(std::uint64_t id) const
    {
        if (!valid_) return nullptr;
        const auto entry = entries_.find(id);
        return entry == entries_.end() ? nullptr : &sources_[entry->second.index];
    }

    [[nodiscard]] std::size_t RetainedCapacityBytes() const noexcept
    {
        const auto max = std::numeric_limits<std::size_t>::max();
        const auto multiply = [max](std::size_t a, std::size_t b) {
            return a && b > max / a ? max : a * b;
        };
        const auto add = [max](std::size_t a, std::size_t b) {
            return b > max - a ? max : a + b;
        };
        return add(multiply(previous_ids_.capacity(), sizeof(std::uint64_t)),
            add(multiply(entries_.bucket_count(), 2 * sizeof(void *)),
                multiply(add(entries_.size(), 1), sizeof(decltype(entries_)::value_type) + 3 * sizeof(void *))));
    }

  private:
    struct Entry { std::size_t index; std::uint64_t generation; };
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::span<const RenderInstance> sources_;
    std::vector<std::uint64_t> previous_ids_;
    std::uint64_t generation_{};
    bool valid_{true};
};
} // namespace hs
