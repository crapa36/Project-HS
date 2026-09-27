#pragma once
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <cstdint>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
// Emits one immutable record per burst particle. Runtime owns absolute event age.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxTypedMoteCommands(
    const VfxProgramData &program, std::span<const VfxEventInput> inputs);
// Reconstruct only live indexed emissions from an authoritative owner clock.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxOwnedMoteCommands(
    const VfxProgramData &program, std::span<const VfxEventInput> inputs, Tick current_tick);

// Reconstruct the currently visible indexed births for gameplay-owned entity
// status effects. This is intentionally stateless: callers submit the result
// directly for the current frame instead of retaining the records as flashes.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxPersistentMoteCommands(
    const VfxProgramData &program, std::span<const VfxPersistentInput> owners);

// One cursor per live projectile/source. The six supported micro_sparks sources
// each have one velocity_billboard output. Past births are skipped when snapshots
// jump; only the owner present at a scheduled birth can create its spark.
class VfxPersistentSparkState
{
  public:
    [[nodiscard]] std::vector<VfxFlashSpawnInput> Emit(
        const VfxProgramData &program,
        std::span<const VfxPersistentInput> owners,
        Tick current_tick);
    void Clear() noexcept { cursors_.clear(); }

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
};
}
