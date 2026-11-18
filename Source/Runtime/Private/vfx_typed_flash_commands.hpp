#pragma once
#include "vfx_event_view.hpp"

#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/runtime/vfx_image.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hs::runtime_detail
{

// These builders require an immutable image produced by the VFX image loading
// and recipe preparation path. Its backing owner must outlive retained commands.
// Raw event and persistent-owner inputs remain mutable runtime data.
// Borrowed recipe/event references are valid only through the immediate
// prepare-to-append window; the image and event backing storage must outlive it.
struct VfxPreparedFlashOutput
{
    const VfxFlashRecipe *recipe{};
    const VfxEventInput *input{};
    std::size_t event_index{};
    static constexpr std::size_t count = 1;
    std::size_t admitted_count{1};
    std::uint32_t importance{};
    Sequence pickup_sequence{};
    Float3 position{}, direction{};
    float scale{1.0f}, mask_strength{};
    std::uint32_t mask_slice{0xffffffffu};
    bool cosmetic{}, pickup_collect{}, circle_geometry{};
};

void PrepareVfxTypedFlashOutputs(const VfxImage &image, VfxEventView inputs,
    std::span<const Sequence> excluded_sorted_sequences,
    std::vector<VfxPreparedFlashOutput> &prepared);
void AppendVfxPreparedFlashOutputs(std::span<const VfxPreparedFlashOutput> prepared,
    std::vector<VfxFlashSpawnInput> &result,
    std::vector<Sequence> *superseded_pickup_sequences = nullptr);

// Rebuild these attachment sprites from the live owner every frame. The returned
// age is local to status_stamp's authored knot interval; callers do not retain it.
void AppendVfxPersistentStatusStampCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, std::vector<VfxFlashSpawnInput> &result);

// Real area presence owns the source interval. Rate births use the 60 Hz
// schedule; each tongue ends at that interval's end, with no invented TTL.
// Prepared rows retain dynamic owner facts and borrow the pinned image appearance.
struct VfxPreparedFireSource
{
    VfxFlashSpawnInput base;
    double elapsed_ticks{};
    std::uint64_t duration_ticks{}, started_tick{}, eligible_ordinals{};
    std::uint64_t admitted_ordinals{};
    std::uint32_t source_seed{}, rate{};
    float circle_radius{}, radius_scale{};
    bool authoritative{};
};
void PrepareVfxPersistentFireSources(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, double current_tick,
    std::vector<VfxPreparedFireSource> &prepared);
// Caps decorative commands only. Authoritative rows remain complete; capped
// decorative rows share a fair quota and retain each source's newest ordinals.
void AppendVfxPreparedFireFlameCommands(
    std::vector<VfxPreparedFireSource> &prepared, std::size_t max_decorative_commands,
    std::vector<VfxFlashSpawnInput> &result);

} // namespace hs::runtime_detail
