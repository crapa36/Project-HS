#pragma once
#include "vfx_event_view.hpp"

#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{

// Birth-time compatibility layers that the typed builder actually accepted.
// These facts filter matching legacy emitters before DrawCommand construction.
struct VfxGroundBirthSuppressionFacts
{
    Sequence sequence{};
    bool all_particles{};
    bool ground_ring{};
    bool ground_disc{};
    bool segment_solid_trail{};
};

struct VfxGroundEventAdmission
{
    bool accepted{};
    bool replace_context_owner{};
};

// Applies the event/profile eligibility used before Ground profile retention.
[[nodiscard]] VfxGroundEventAdmission ClassifyVfxTypedEventGroundAdmission(
    const VfxImage &image, const VfxEventInput &input) noexcept;

// Builders require an immutable prepared and admitted image; live events and owners remain mutable.
void AppendVfxTypedGroundCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, Tick current_tick,
    std::vector<VfxGroundSpawnInput> &result, std::vector<float> &frame_gap_angles);
void AppendVfxTypedEventGroundCommands(const VfxImage &image,
    VfxEventView inputs, double current_tick,
    std::vector<VfxGroundSpawnInput> &result, std::vector<float> &frame_gap_angles);
void AppendVfxTypedEventGroundBirthSuppression(
    const VfxImage &image, VfxEventView inputs,
    std::vector<VfxGroundBirthSuppressionFacts> &result);

} // namespace hs::runtime_detail
