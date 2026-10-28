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
// Shared across one frame's owned/persistent mote builders. Only
// non-authoritative CurlMotes output rows spend this budget.
struct VfxMoteRowQuota
{
    std::size_t remaining_rows{};
    // Saturating counts of eligible decorative output rows considered and
    // allowed before dynamic row math; they are not counts of emitted rows.
    std::size_t requested_rows{};
    std::size_t admitted_rows{};
};

enum class VfxPreparedMoteSampler : std::uint8_t { Common, Residual, StatusTick };
struct VfxPreparedMoteBurst
{
    VfxFlashSpawnInput base;
    std::size_t count{}, admitted_count{};
    std::uint32_t seed_base{}, importance{};
    float scale{1.0f}, radius{}, speed{}, residual_reach{};
    VfxPreparedMoteSampler sampler{VfxPreparedMoteSampler::Common};
    bool cosmetic{};
    Sequence event_sequence{};
};
struct VfxMoteBurstSelection
{
    std::size_t required_rows{}, requested_decorative_rows{}, admitted_decorative_rows{};
};

// Split route preparation from row generation so admission can reserve all
// supported raw-event mote fanout before sampling any ordinal geometry.
void PrepareVfxTypedMoteBursts(const VfxImage &image, VfxEventView inputs,
    std::span<const Sequence> consumed_event_sequences,
    std::vector<VfxPreparedMoteBurst> &prepared);
[[nodiscard]] VfxMoteBurstSelection SelectVfxPreparedMoteBursts(
    std::vector<VfxPreparedMoteBurst> &prepared, std::size_t max_decorative_rows,
    std::vector<std::size_t> &order_scratch);
void AppendVfxPreparedMoteBursts(std::span<const VfxPreparedMoteBurst> prepared,
    std::vector<VfxFlashSpawnInput> &result,
    // Emits one sequence fact for each successfully appended status-tick row.
    std::vector<Sequence> *emitted_status_sequences = nullptr);

// Emits one immutable record per burst particle. Runtime owns absolute event age.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxTypedMoteCommands(
    const VfxImage &image, VfxEventView inputs);
// consumed_event_sequences must be sorted; membership preserves duplicate events
// while suppressing every event whose sequence was actually converted.
void AppendVfxTypedMoteCommands(const VfxImage &image, VfxEventView inputs,
    std::span<const Sequence> consumed_event_sequences,
    std::vector<VfxFlashSpawnInput> &result);
// Reconstruct only live indexed emissions from an authoritative owner clock.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxOwnedMoteCommands(
    const VfxImage &image, VfxEventView inputs, double current_tick);
void AppendVfxOwnedMoteCommands(const VfxImage &image, VfxEventView inputs,
    double current_tick, std::vector<VfxFlashSpawnInput> &result,
    VfxMoteRowQuota *quota = nullptr);

struct VfxOwnedStatusTickMoteCommand
{
    Sequence event_sequence{};
    VfxFlashSpawnInput flash;
};
void PrepareVfxOwnedStatusTickMoteBursts(const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> owners,
    std::vector<VfxPreparedMoteBurst> &prepared);
// A real status tick supplies the burst clock; the matching entity episode
// supplies the persistent recipe and surface anchor.
[[nodiscard]] std::vector<VfxOwnedStatusTickMoteCommand> BuildVfxOwnedStatusTickMoteCommands(
    const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> owners);
void AppendVfxOwnedStatusTickMoteFlashes(const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> owners,
    std::vector<VfxFlashSpawnInput> &result,
    std::vector<Sequence> &consumed_event_sequences);

// Reconstruct the currently visible indexed births for gameplay-owned entity
// status effects. This is intentionally stateless: callers submit the result
// directly for the current frame instead of retaining the records as flashes.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxPersistentMoteCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> owners);
void AppendVfxPersistentMoteCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> owners, std::vector<VfxFlashSpawnInput> &result,
    VfxMoteRowQuota *quota = nullptr);

// One cursor per live projectile/source. The six supported micro_sparks sources
// each have one velocity_billboard output. Past births are skipped when snapshots
// jump; only the owner present at a scheduled birth can create its spark.
class VfxPersistentSparkState
{
  public:
    void Append(const VfxImage &image, std::span<const VfxPersistentInput> owners,
        Tick current_tick, float interpolation, std::vector<VfxFlashSpawnInput> &result,
        VfxMoteRowQuota *quota = nullptr);
    [[nodiscard]] std::vector<VfxFlashSpawnInput> Emit(
        const VfxImage &image,
        std::span<const VfxPersistentInput> owners,
        Tick current_tick, float interpolation = 1.0f,
        VfxMoteRowQuota *quota = nullptr);
    void Clear() noexcept { cursors_.clear(); changed_.clear(); pending_.clear(); change_slots_.clear(); }
    [[nodiscard]] std::size_t RetainedCapacityBytes() const noexcept;
    [[nodiscard]] std::size_t PendingCapacityRows() const noexcept { return pending_.capacity(); }
    [[nodiscard]] std::uint64_t Rebind(const VfxProgramView &previous,
                                     const VfxProgramView &next) noexcept;
    void Swap(VfxPersistentSparkState &prepared) noexcept
    {
        cursors_.swap(prepared.cursors_);
        changed_.swap(prepared.changed_);
        pending_.swap(prepared.pending_);
        change_slots_.swap(prepared.change_slots_);
    }

  private:
    struct Cursor
    {
        std::uint64_t owner_id{};
        VfxEffectHandle effect_handle{};
        std::uint32_t source_index{};
        std::uint32_t next_ordinal{};
        Tick expires_tick{};
    };
    struct CursorChange { std::size_t index; Cursor cursor; bool is_new; };
    std::vector<Cursor> cursors_;
    std::vector<CursorChange> changed_;
    std::vector<VfxFlashSpawnInput> pending_;
    std::vector<std::size_t> change_slots_;
};
}
