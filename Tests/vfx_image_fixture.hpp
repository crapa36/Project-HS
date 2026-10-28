#pragma once

#include <hs/runtime/vfx_image.hpp>
#include "vfx_typed_frame_adapter.hpp"

#include <algorithm>
#include <vector>

namespace hs::test
{
// Malformed adapter fixtures deliberately bypass the full content decoder.
struct VfxLookupImage : VfxImage
{
    explicit VfxLookupImage(const VfxProgramView &view)
        : lookup(view.effect_lookup.begin(), view.effect_lookup.end())
    {
        program = view;
        std::stable_sort(lookup.begin(), lookup.end(), [](const auto &a, const auto &b) {
            return a.effect_id < b.effect_id;
        });
        effect_lookup_by_id = lookup;
    }

    VfxLookupImage(const VfxLookupImage &) = delete;
    VfxLookupImage &operator=(const VfxLookupImage &) = delete;

  private:
    std::vector<VfxEffectLookupRecord> lookup;
};

inline runtime_detail::VfxTypedFrameInputs BuildFrameInputs(
    const VfxImage &image, std::span<const PresentationEvent> events,
    std::span<const PersistentVfxVisual> current,
    std::span<const PersistentVfxVisual> previous, Tick current_tick,
    const runtime_detail::PreparedPairFacts *pair_facts = nullptr,
    float interpolation = 1.0f)
{
    runtime_detail::VfxTypedFrameInputs result;
    runtime_detail::BuildVfxTypedFrameInputs(
        image, events, current, previous, current_tick, result, pair_facts, interpolation);
    return result;
}
} // namespace hs::test
