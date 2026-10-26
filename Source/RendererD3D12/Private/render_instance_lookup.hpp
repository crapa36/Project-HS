#pragma once

#include <hs/render_data/render_snapshot.hpp>
#include <algorithm>
#include <span>
#include <unordered_map>
#include <vector>

namespace hs
{
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

  private:
    struct Entry { std::size_t index; std::uint64_t generation; };
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::span<const RenderInstance> sources_;
    std::vector<std::uint64_t> previous_ids_;
    std::uint64_t generation_{};
    bool valid_{true};
};
} // namespace hs
