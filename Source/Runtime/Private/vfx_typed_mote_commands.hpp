#pragma once
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>
namespace hs::runtime_detail
{
// Bounded session-local cache of deterministic mote birth coordinates. Current
// owner geometry, envelope, quality and age are still evaluated every frame.
class VfxPersistentMoteBirthWindow
{
  public:
    struct UnitBirth
    {
        float angle01{};
        float radius01{};
    };
    void Prune(Tick current_tick);
    [[nodiscard]] UnitBirth GetOrCreate(std::uint64_t owner_id,
        std::uint64_t episode_generation, std::uint64_t effect_id,
        std::uint32_t source_id, std::uint32_t ordinal,
        Tick birth_tick, std::uint32_t seed);
    void Clear() noexcept { births_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return births_.size(); }

  private:
    struct Key
    {
        std::uint64_t owner_id{}, episode_generation{}, effect_id{};
        std::uint32_t source_id{}, ordinal{}, seed{};
        bool operator==(const Key &) const = default;
    };
    struct KeyHash
    {
        std::size_t operator()(const Key &key) const noexcept
        {
            auto hash = std::hash<std::uint64_t>{}(key.owner_id);
            const auto mix = [&](std::uint64_t value) {
                hash ^= std::hash<std::uint64_t>{}(value) + 0x9e3779b97f4a7c15ull +
                    (hash << 6) + (hash >> 2);
            };
            mix(key.episode_generation); mix(key.effect_id);
            mix(key.source_id); mix(key.ordinal); mix(key.seed);
            return hash;
        }
    };
    struct Entry { Tick birth_tick{}; UnitBirth unit; };
    std::unordered_map<Key, Entry, KeyHash> births_;
};

// Emits one immutable record per burst particle. Runtime owns absolute event age.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxTypedMoteCommands(
    const VfxProgramView &program, std::span<const VfxEventInput> inputs);
// Reconstruct only live indexed emissions from an authoritative owner clock.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxOwnedMoteCommands(
    const VfxProgramView &program, std::span<const VfxEventInput> inputs, double current_tick);

struct VfxOwnedStatusTickMoteCommand
{
    Sequence event_sequence{};
    VfxFlashSpawnInput flash;
};
// A real status tick supplies the burst clock; the matching entity episode
// supplies the persistent recipe and surface anchor.
[[nodiscard]] std::vector<VfxOwnedStatusTickMoteCommand> BuildVfxOwnedStatusTickMoteCommands(
    const VfxProgramView &program, std::span<const VfxEventInput> events,
    std::span<const VfxPersistentInput> owners);

// Reconstruct the currently visible indexed births for gameplay-owned entity
// status effects. This is intentionally stateless: callers submit the result
// directly for the current frame instead of retaining the records as flashes.
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxPersistentMoteCommands(
    const VfxProgramView &program, std::span<const VfxPersistentInput> owners);
[[nodiscard]] std::vector<VfxFlashSpawnInput> BuildVfxPersistentMoteCommands(
    const VfxProgramView &program, std::span<const VfxPersistentInput> owners,
    VfxPersistentMoteBirthWindow &births, Tick current_tick, float interpolation = 1.0f);

// One cursor per live projectile/source. The six supported micro_sparks sources
// each have one velocity_billboard output. Past births are skipped when snapshots
// jump; only the owner present at a scheduled birth can create its spark.
class VfxPersistentSparkState
{
  public:
    [[nodiscard]] std::vector<VfxFlashSpawnInput> Emit(
        const VfxProgramView &program,
        std::span<const VfxPersistentInput> owners,
        Tick current_tick, float interpolation = 1.0f);
    void Clear() noexcept { cursors_.clear(); }
    [[nodiscard]] std::uint64_t Rebind(const VfxProgramView &previous,
                                     const VfxProgramView &next) noexcept;
    void Swap(VfxPersistentSparkState &prepared) noexcept { cursors_.swap(prepared.cursors_); }

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
