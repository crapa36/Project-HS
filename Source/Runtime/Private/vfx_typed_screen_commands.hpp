#pragma once
#include <hs/core/result.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <hs/renderer/vfx_screen_input.hpp>
#include <vector>
namespace hs::runtime_detail
{
// Simulation-tick clock, independent of render submission/history. Missing
// observations hold the last known HP frequency; they cannot invent HP samples.
struct VfxLowHealthPulseState
{
    std::uint64_t stable_id{}, owner_id{};
    Tick last_tick{}, entered_tick{};
    double phase{};
    float frequency{};
    void Clear() noexcept { *this = {}; }
    void Advance(std::uint64_t stable, std::uint64_t owner, Tick tick, float ratio) noexcept;
    [[nodiscard]] float EntryFade() const noexcept;
};
[[nodiscard]] bool IsVfxTypedScreenEffect(const VfxProgramData &, VfxEffectHandle) noexcept;
[[nodiscard]] Result BuildVfxTypedScreenCommands(const VfxProgramData &,
    std::span<const VfxEventInput>, std::span<const VfxPersistentInput>, Tick,
    VfxQuality, VfxLowHealthPulseState &, std::vector<VfxScreenInput> &);
}
