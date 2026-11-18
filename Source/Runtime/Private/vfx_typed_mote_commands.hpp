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
    bool budget_limited{};
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
// Split route preparation from row generation so admission can reserve all
// supported raw-event mote fanout before sampling any ordinal geometry.
void PrepareVfxTypedMoteBursts(const VfxImage &image, VfxEventView inputs,
    std::span<const Sequence> consumed_event_sequences,
    std::vector<VfxPreparedMoteBurst> &prepared);
void AppendVfxPreparedMoteBursts(std::span<const VfxPreparedMoteBurst> prepared,
    std::vector<VfxFlashSpawnInput> &result);

// Reconstruct only live indexed emissions from an authoritative owner clock.
void AppendVfxOwnedMoteCommands(const VfxImage &image, VfxEventView inputs,
    double current_tick, std::vector<VfxFlashSpawnInput> &result,
    VfxMoteRowQuota *quota = nullptr);

void PrepareVfxOwnedStatusTickOutputs(const VfxImage &image, VfxEventView events,
    std::span<const VfxPersistentInput> owners,
    std::vector<VfxFlashSpawnInput> &flashes,
    std::vector<Sequence> &emitted_flash_sequences,
    std::vector<VfxPreparedMoteBurst> &prepared,
    std::vector<std::size_t> &owner_indices);

// Reconstruct the currently visible indexed births for gameplay-owned entity
// status effects. This is intentionally stateless: callers submit the result
// directly for the current frame instead of retaining the records as flashes.
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
    void Clear() noexcept { cursors_.clear(); cursor_slots_.clear(); }

  private:
    struct Cursor
    {
        std::uint64_t owner_id{};
        VfxEffectHandle effect_handle{};
        std::uint32_t source_index{};
        std::uint32_t next_ordinal{};
        Tick expires_tick{};
    };
    std::vector<Cursor> cursors_;
    std::vector<std::size_t> cursor_slots_;
};
}
