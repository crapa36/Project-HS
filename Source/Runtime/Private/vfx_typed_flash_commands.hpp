#pragma once
#include "vfx_event_view.hpp"

#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/runtime/vfx_image.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{

// Decoded additive impact sprites. Runtime owns age/retention after decode;
// this record carries the authored event tick so repeated render frames cannot
// restart the flash. Successful pickup bursts identify the legacy collection
// layers they replace; rejected recipes retain their compatibility visuals.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxTypedFlashCommands(
    const VfxImage &image,
    VfxEventView inputs,
    std::vector<Sequence> *superseded_pickup_sequences = nullptr);
// Appends in event/recipe order. Exclusions are sorted and suppress every event
// sharing a sequence, matching legacy paired-status behavior.
void AppendVfxTypedFlashCommands(const VfxImage &image, VfxEventView inputs,
    std::span<const Sequence> excluded_sorted_sequences,
    std::vector<VfxFlashSpawnInput> &result,
    std::vector<Sequence> *superseded_pickup_sequences = nullptr);

// Rebuild these attachment sprites from the live owner every frame. The returned
// age is local to status_stamp's authored knot interval; callers do not retain it.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxPersistentStatusStampCommands(
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs);
void AppendVfxPersistentStatusStampCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, std::vector<VfxFlashSpawnInput> &result);

// Real area presence owns the source interval. Rate births use the 60 Hz
// schedule; each tongue ends at that interval's end, with no invented TTL.
// Prepared rows own copied recipe/owner facts and borrow no image or input data.
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
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxPersistentFireFlameCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> inputs,
    double current_tick);
void AppendVfxPersistentFireFlameCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, double current_tick,
    std::vector<VfxFlashSpawnInput> &result);

struct VfxOwnedStatusTickFlashCommand
{
    Sequence event_sequence{};
    VfxFlashSpawnInput flash;
};

// A fixed status tick is converted only while its matching persistent entity
// owner and status episode are live. The sequence identifies the event whose
// legacy visuals may be suppressed after this conversion succeeds.
[[nodiscard]] std::vector<VfxOwnedStatusTickFlashCommand> BuildVfxOwnedStatusTickFlashCommands(
    const VfxImage &image,
    VfxEventView events,
    std::span<const VfxPersistentInput> persistent);
// Appends one flash and its sequence fact only for each successfully paired tick.
// Duplicate input sequences remain duplicated so callers can suppress all matches.
void AppendVfxOwnedStatusTickFlashes(const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> persistent,
    std::vector<VfxFlashSpawnInput> &flashes,
    std::vector<Sequence> &emitted_sequences);

} // namespace hs::runtime_detail
