#pragma once

#include <hs/core/presentation_event.hpp>
#include <hs/core/render_snapshot.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace hs::runtime_detail
{

struct VfxTypedFrameInputs
{
    std::vector<VfxEventInput> events;
    std::vector<VfxPersistentInput> persistent;
    std::vector<std::uint64_t> unsupported_effect_ids;
};

// Convert the presentation contracts into typed v4 renderer inputs. The
// function owns no state and does not create a lifetime or geometry value when
// the source contract does not provide one.
[[nodiscard]] VfxTypedFrameInputs BuildVfxTypedFrameInputs(
    const VfxProgramData &cooked,
    std::span<const PresentationEvent> events,
    std::span<const PersistentVfxVisual> current,
    std::span<const PersistentVfxVisual> previous,
    Tick current_tick);

// Retain only the captured endpoints of a successful second retreat. The
// movement is instantaneous in gameplay; fixed authored seconds own the fade.
// Clear on session reset; catalog adoption rebinds the captured owner payloads.
class VfxUpgradeMovementState
{
  public:
    [[nodiscard]] std::vector<VfxPersistentInput> Emit(
        const VfxProgramData &cooked,
        std::span<const PresentationEvent> dispatched_events,
        Tick current_tick);
    void Clear() noexcept { owners_.clear(); }
    [[nodiscard]] std::uint64_t Rebind(const VfxProgramData &previous,
                                     const VfxProgramData &next) noexcept;
    void Swap(VfxUpgradeMovementState &prepared) noexcept { owners_.swap(prepared.owners_); }

  private:
    struct Owner
    {
        VfxPersistentInput input;
        Tick started{};
        float seconds{};
    };
    std::vector<Owner> owners_;
};

// Refresh retained warning events from their authoritative persistent
// owner. Events without owner metadata are preserved unchanged.
void RefreshVfxGroundEventOwners(std::vector<VfxEventInput> &events,
                                 std::span<const PersistentVfxVisual> owners,
                                 Tick current_tick);

// Validate prepared owner-backed events against exact warning/sector recipes.
// Events without owner metadata retain their already remapped handles.
void RebindVfxGroundEventOwners(const VfxProgramData &cooked,
                                std::vector<VfxEventInput> &events);

} // namespace hs::runtime_detail
