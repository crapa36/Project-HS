#pragma once

#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hs::runtime_detail
{
enum class VfxGenerationRebindOutcome : std::uint8_t { Retained, Deleted, Incompatible };

struct VfxGenerationRebindCounts
{
    std::uint64_t retained{}, deleted{}, incompatible{};
    void Record(VfxGenerationRebindOutcome outcome) noexcept
    {
        if (outcome == VfxGenerationRebindOutcome::Retained) ++retained;
        else if (outcome == VfxGenerationRebindOutcome::Deleted) ++deleted;
        else ++incompatible;
    }
};

// A retained capture owns the old generation lease. It may survive a catalog
// reload only when the captured asset and every validated product reachable from
// it still has the same digest and direct dependency edges in the candidate.
inline bool SameCapturedAssetClosure(const SelectedGeneration &previous,
    const SelectedGeneration &next, AssetId actual_asset)
{
    if (actual_asset.value == 0) return false;
    std::string root;
    for (const auto &[logical_id, path] : previous.product_paths)
    {
        (void)path;
        if (MakeAssetId(logical_id).value != actual_asset.value) continue;
        if (!root.empty()) return false;
        root = logical_id;
    }
    if (root.empty()) return false;

    struct Visit
    {
        std::string logical_id;
        bool exit{};
    };
    std::vector<Visit> pending{{root, false}};
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    while (!pending.empty())
    {
        auto visit = std::move(pending.back());
        pending.pop_back();
        if (visit.exit)
        {
            visiting.erase(visit.logical_id);
            visited.insert(std::move(visit.logical_id));
            continue;
        }
        if (visited.contains(visit.logical_id)) continue;
        if (!visiting.insert(visit.logical_id).second) return false;

        const auto previous_path = previous.product_paths.find(visit.logical_id);
        const auto previous_digest = previous.product_digests.find(visit.logical_id);
        const auto previous_dependencies = previous.product_dependencies.find(visit.logical_id);
        const auto next_path = next.product_paths.find(visit.logical_id);
        const auto next_digest = next.product_digests.find(visit.logical_id);
        const auto next_dependencies = next.product_dependencies.find(visit.logical_id);
        if (previous_path == previous.product_paths.end() ||
            previous_digest == previous.product_digests.end() ||
            previous_dependencies == previous.product_dependencies.end() ||
            next_path == next.product_paths.end() ||
            next_digest == next.product_digests.end() ||
            next_dependencies == next.product_dependencies.end() ||
            previous_digest->second != next_digest->second ||
            previous_dependencies->second != next_dependencies->second)
            return false;

        pending.push_back({visit.logical_id, true});
        for (auto dependency = previous_dependencies->second.rbegin();
             dependency != previous_dependencies->second.rend(); ++dependency)
        {
            if (!previous.product_paths.contains(*dependency) ||
                !previous.product_digests.contains(*dependency) ||
                !previous.product_dependencies.contains(*dependency) ||
                !next.product_paths.contains(*dependency) ||
                !next.product_digests.contains(*dependency) ||
                !next.product_dependencies.contains(*dependency))
                return false;
            if (visiting.contains(*dependency)) return false;
            if (!visited.contains(*dependency)) pending.push_back({*dependency, false});
        }
    }
    return true;
}

// Numeric handles belong to one cooked table. Only its stable effect identity
// and matching input contract can transfer an existing authoritative payload.
inline VfxGenerationRebindOutcome RebindVfxGenerationHandle(
    const VfxProgramData &previous, const VfxProgramData &next,
    VfxEffectHandle &handle, std::uint32_t input_mode) noexcept
{
    const auto identity = std::find_if(previous.effect_lookup.begin(), previous.effect_lookup.end(),
        [handle](const auto &entry) { return entry.handle == handle; });
    if (!handle || handle > previous.effects.size() || identity == previous.effect_lookup.end())
        return VfxGenerationRebindOutcome::Incompatible;
    const auto replacement = std::find_if(next.effect_lookup.begin(), next.effect_lookup.end(),
        [&](const auto &entry) { return entry.effect_id == identity->effect_id; });
    if (replacement == next.effect_lookup.end()) return VfxGenerationRebindOutcome::Deleted;
    if (!replacement->handle || replacement->handle > next.effects.size())
        return VfxGenerationRebindOutcome::Incompatible;
    const auto &old_effect = previous.effects[handle - 1u];
    const auto &new_effect = next.effects[replacement->handle - 1u];
    if (old_effect.handle != handle || new_effect.handle != replacement->handle ||
        old_effect.input_mode != input_mode || new_effect.input_mode != input_mode ||
        old_effect.payload_kind != new_effect.payload_kind)
        return VfxGenerationRebindOutcome::Incompatible;
    handle = replacement->handle;
    return VfxGenerationRebindOutcome::Retained;
}

inline void RebindVfxGenerationEvents(const VfxProgramData &previous,
    const VfxProgramData &next, std::vector<VfxEventInput> &events,
    VfxGenerationRebindCounts &counts)
{
    std::erase_if(events, [&](auto &event) {
        const auto outcome = RebindVfxGenerationHandle(previous, next, event.effect_handle, 0u);
        counts.Record(outcome);
        return outcome != VfxGenerationRebindOutcome::Retained;
    });
}
} // namespace hs::runtime_detail
