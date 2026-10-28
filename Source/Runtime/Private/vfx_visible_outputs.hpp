#pragma once

#include "vfx_session_state.hpp"
#include "vfx_typed_flash_commands.hpp"
#include "vfx_typed_mote_commands.hpp"
#include "vfx_workspace_size.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
struct VfxVisibleMoteAdmission
{
    std::size_t requested_rows{}, admitted_rows{};
    bool budget_limited{};
};

template <typename HeldBytesFn>
VfxVisibleMoteAdmission PrepareVisibleMoteOutputs(const VfxImage &image, VfxSessionState &session,
    VfxPersistentSparkState &persistent_sparks, std::span<const VfxPersistentInput> persistent,
    double display_tick, Tick ground_tick, float interpolation, std::size_t retained_flash_count,
    std::size_t required_suffix, std::vector<VfxFlashSpawnInput> &visible_flashes,
    VfxMoteBurstSelection typed_birth_selection, HeldBytesFn &&measure_held_bytes)
{
    using namespace vfx_retained_size;
    auto &active_flashes = session.decoded.flashes;
    visible_flashes.reserve(Add(active_flashes.size(), required_suffix));
    const auto held = measure_held_bytes();
    const std::array unused_rows{
        active_flashes.capacity() - active_flashes.size(),
        visible_flashes.capacity() - active_flashes.size() - required_suffix,
        persistent_sparks.PendingCapacityRows()};
    const auto growth_rows = held < kVfxWorkspaceSoftBudgetBytes
        ? (kVfxWorkspaceSoftBudgetBytes - held) / sizeof(VfxFlashSpawnInput) : 0;
    const auto maximum_rows = Add(std::ranges::min(unused_rows), growth_rows);
    // Staging, retained births and their visible copies coexist through publication.
    VfxMoteRowQuota quota{AppendRowsWithinBudget<VfxFlashSpawnInput>(
        maximum_rows, held, kVfxWorkspaceSoftBudgetBytes, unused_rows)};
    persistent_sparks.Append(image, persistent, ground_tick, interpolation, active_flashes, &quota);

    // The retained prefix plus every newly retained row bounds the visible projection.
    visible_flashes.reserve(Add(active_flashes.size(), required_suffix));
    session.decoded.AppendVisibleFlashes(display_tick, visible_flashes, retained_flash_count);
    session.active_effects.EraseIf(ActiveEffectStore::Profile::PointRate, [&](const auto &input) {
        return display_tick >= input.event_tick && display_tick - input.event_tick >= 39;
    });
    const auto visible_rows = AvailableVfxFlashAppendRows(
        measure_held_bytes(), kVfxWorkspaceSoftBudgetBytes, visible_flashes);
    quota.remaining_rows = visible_rows > required_suffix ? visible_rows - required_suffix : 0;
    AppendVfxOwnedMoteCommands(image,
        session.active_effects.ProfileEvents(ActiveEffectStore::Profile::PointRate),
        display_tick, visible_flashes, &quota);
    AppendVfxOwnedMoteCommands(image,
        session.active_effects.ProfileEvents(ActiveEffectStore::Profile::Ground),
        display_tick, visible_flashes, &quota);
    AppendVfxPersistentMoteCommands(image, persistent, visible_flashes, &quota);

    return {
        Add(typed_birth_selection.requested_decorative_rows, quota.requested_rows),
        Add(typed_birth_selection.admitted_decorative_rows, quota.admitted_rows),
        quota.requested_rows > quota.admitted_rows ||
            typed_birth_selection.requested_decorative_rows > typed_birth_selection.admitted_decorative_rows};
}

struct VfxVisibleFireAdmission
{
    std::size_t emitted_rows{}, admission_bytes{};
    bool budget_limited{};
};

template <typename HeldBytesFn>
VfxVisibleFireAdmission AppendVisibleFireAndStamps(
    std::vector<VfxPreparedFireSource> &prepared_fire_sources,
    std::size_t decorative_fire, std::size_t required_suffix,
    std::span<const VfxFlashSpawnInput> status_stamp_flashes,
    std::vector<VfxFlashSpawnInput> &visible_flashes, std::size_t budget_bytes,
    HeldBytesFn &&measure_held_bytes)
{
    using namespace vfx_retained_size;
    std::size_t emitted_rows{}, admission_bytes{};
    bool budget_limited{};
    if (!prepared_fire_sources.empty())
    {
        // Required fire and stamps retain their suffix before decorative growth.
        visible_flashes.reserve(Add(visible_flashes.size(), required_suffix));
        admission_bytes = measure_held_bytes();
        const auto available = AvailableVfxFlashAppendRows(admission_bytes, budget_bytes, visible_flashes);
        const auto fire_limit = available > required_suffix ? available - required_suffix : 0;
        const auto admitted = std::min(decorative_fire, fire_limit);
        budget_limited = admitted < decorative_fire;
        const auto previous_capacity = visible_flashes.capacity();
        visible_flashes.reserve(Add(Add(visible_flashes.size(), required_suffix), admitted));
        admission_bytes = Add(admission_bytes,
            Multiply(visible_flashes.capacity() - previous_capacity, sizeof(VfxFlashSpawnInput)));
        const auto before = visible_flashes.size();
        AppendVfxPreparedFireFlameCommands(prepared_fire_sources, admitted, visible_flashes);
        emitted_rows = visible_flashes.size() - before;
    }
    visible_flashes.insert(visible_flashes.end(), status_stamp_flashes.begin(), status_stamp_flashes.end());
    return {emitted_rows, admission_bytes, budget_limited};
}
}
