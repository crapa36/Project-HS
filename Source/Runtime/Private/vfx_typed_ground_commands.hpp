#pragma once
#include "vfx_event_view.hpp"

#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>

#include <span>
#include <vector>

namespace hs::runtime_detail
{

struct VfxTypedGroundCommand
{
    ParticleSpawnCommand command;
    bool additive{};
    std::uint64_t stable_id{}; // Identity of the gameplay owner, not a generated particle ID.
    VfxEffectHandle effect_handle{kInvalidVfxEffectHandle};
    std::uint8_t source_visual_kind{0xff};
    float edge_width_world{};
    float normalized_age{};
    float lifetime01{};
    VfxGroundMotion motion;
    std::uint32_t gradient_row{};
    float hdr{};
    VfxGroundGeometry geometry;
};

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

// Converts authored persistent CircleAreaPayload boundaries and interior fills.
// Callers retain the owner identity and world-space edge width alongside the
// legacy spawn command until the renderer's exact-ring path consumes them.
[[nodiscard]] std::vector<VfxTypedGroundCommand> BuildVfxTypedGroundCommands(
    const VfxImage &image,
    std::span<const VfxPersistentInput> inputs,
    Tick current_tick);
void AppendVfxTypedGroundCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, Tick current_tick,
    std::vector<VfxTypedGroundCommand> &result);

[[nodiscard]] std::vector<VfxTypedGroundCommand> BuildVfxTypedEventGroundCommands(
    const VfxImage &image,
    VfxEventView inputs,
    double current_tick);
void AppendVfxTypedEventGroundCommands(const VfxImage &image,
    VfxEventView inputs, double current_tick,
    std::vector<VfxTypedGroundCommand> &result);
void AppendVfxTypedEventGroundBirthSuppression(
    const VfxImage &image, VfxEventView inputs,
    std::vector<VfxGroundBirthSuppressionFacts> &result);

} // namespace hs::runtime_detail
