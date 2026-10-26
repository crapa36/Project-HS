#include "vfx_typed_entity_surface_commands.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint32_t Hash32(std::string_view value) noexcept
{
    std::uint32_t result = 2166136261u;
    for (const unsigned char c : value) result = (result ^ c) * 16777619u;
    return result;
}
constexpr std::uint64_t Hash64(std::string_view value) noexcept
{
    std::uint64_t result = 14695981039346656037ull;
    for (const unsigned char c : value) result = (result ^ c) * 1099511628211ull;
    return result;
}
constexpr auto kPhase = Hash64("particle.boss.phase_change");
constexpr auto kDeath = Hash64("particle.boss.death");
constexpr auto kPersistent = Hash64("persistent.boss.phase_transition_invulnerable");
constexpr std::uint64_t kEnemyRenderPrefix = 2ull << 60;

template<class T> bool Range(VfxRange r, std::span<const T> v) noexcept
{ return r.first <= v.size() && r.count <= v.size() - r.first; }
bool Finite(Float3 v) noexcept
{ return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool BossInstance(const RenderInstance &instance) noexcept
{
    return instance.stable_id != 0 &&
        (instance.stable_id >> 60) == 2 &&
        instance.mesh >= RenderMesh::BossFiveMinute && instance.mesh <= RenderMesh::BossFinal &&
        instance.asset_id.value != 0 && instance.material_id.value != 0 &&
        Finite(instance.position) && Finite(instance.scale) &&
        instance.scale.x > 0 && instance.scale.y > 0 && instance.scale.z > 0 &&
        std::isfinite(instance.yaw) && std::isfinite(instance.animation_time_seconds) &&
        std::isfinite(instance.animation_playback_rate);
}
bool FinitePose(const AnimationPoseRef &pose) noexcept
{
    return pose.clip < CharacterAnimationClip::Count && pose.secondary_clip < CharacterAnimationClip::Count &&
        pose.upper_body_clip < CharacterAnimationClip::Count &&
        std::isfinite(pose.normalized_time) && std::isfinite(pose.playback_rate) &&
        std::isfinite(pose.secondary_normalized_time) && std::isfinite(pose.secondary_playback_rate) &&
        std::isfinite(pose.secondary_weight) && pose.secondary_weight >= 0 && pose.secondary_weight <= 1 &&
        std::isfinite(pose.upper_body_normalized_time) && std::isfinite(pose.upper_body_playback_rate) &&
        std::isfinite(pose.upper_body_weight) && pose.upper_body_weight >= 0 && pose.upper_body_weight <= 1;
}
std::uint64_t Identity(const VfxProgramView &program, VfxEffectHandle handle) noexcept
{
    std::uint64_t id{};
    for (const auto &lookup : program.effect_lookup)
        if (lookup.handle == handle)
        {
            if (id != 0 || lookup.effect_id == 0) return 0;
            id = lookup.effect_id;
        }
    return id;
}
bool Textures(const VfxProgramView &program, const VfxOutputRecord &output) noexcept
{
    if (!Range(output.textures, program.texture_bindings) || output.textures.count != 3) return false;
    std::uint32_t seen{};
    for (const auto &binding : std::span(program.texture_bindings).subspan(output.textures.first, output.textures.count))
    {
        const auto resource = std::find_if(program.texture_resources.begin(), program.texture_resources.end(),
            [&](const auto &v) { return v.slot == binding.catalog_slot; });
        if (resource == program.texture_resources.end() || !Range(resource->asset_path_bytes, program.strings) ||
            resource->asset_path_bytes.count == 0 || binding.min_quality > 2 || binding.selection != 0 ||
            binding.slices.count != 0 || binding.strength != 1.0f) return false;
        const std::string_view path(reinterpret_cast<const char *>(program.strings.data()) +
            resource->asset_path_bytes.first, resource->asset_path_bytes.count);
        std::uint32_t bit{};
        if (binding.role == Hash32("profile.global") && path == "Content/Textures/VFX/vfx_gradient_lut.dds") bit = 1;
        if (binding.role == Hash32("profile.global") && path == "Content/Textures/VFX/vfx_stbn_scalar.dds") bit = 2;
        if (binding.role == Hash32("profile.authored") && path == "Content/Textures/VFX/vfx_noise_volume_64.dds") bit = 4;
        if (bit == 0 || (seen & bit)) return false;
        seen |= bit;
    }
    return seen == 7;
}
bool Parameters(const VfxProgramView &program, const VfxOutputRecord &output,
                bool persistent, VfxEntitySurfaceCommand &command) noexcept
{
    if (!Range(output.parameters, program.parameters) || output.parameters.count != 3) return false;
    std::uint32_t seen{};
    for (const auto &p : std::span(program.parameters).subspan(output.parameters.first, output.parameters.count))
    {
        std::uint32_t bit{};
        if (p.key == Hash32("noise_scale") && p.type == VfxParameterType::Float)
        { bit = 1; command.noise_scale = std::bit_cast<float>(p.bits); }
        else if (p.key == Hash32("edge_width") && p.type == VfxParameterType::Float)
        { bit = 2; command.edge_width = std::bit_cast<float>(p.bits); }
        else if (p.key == Hash32("direction") && p.type == VfxParameterType::Enum &&
                 p.bits == Hash32(persistent ? "up" : "up_and_out"))
        { bit = 4; command.direction = persistent ? VfxEntitySurfaceDirection::Up : VfxEntitySurfaceDirection::UpAndOut; }
        if (bit == 0 || (seen & bit)) return false;
        seen |= bit;
    }
    return seen == 7 && std::isfinite(command.noise_scale) && command.noise_scale == (persistent ? 2.6f : 2.4f) &&
        std::isfinite(command.edge_width) && command.edge_width == (persistent ? .08f : .07f);
}
void Append(const VfxProgramView &program, const VfxEffectRecord &effect, bool persistent,
            bool death, VfxQuality quality, Tick started, Tick until, double current,
            const VfxEntitySurfaceCommand &owner, std::vector<VfxEntitySurfaceCommand> &result)
{
    if (!Range(effect.sources, program.sources) || until <= started || current < started || current >= until ||
        static_cast<std::uint32_t>(quality) > 2) return;
    const auto age = static_cast<double>(current - started) / static_cast<double>(until - started);
    for (std::size_t si = effect.sources.first; si < static_cast<std::size_t>(effect.sources.first) + effect.sources.count; ++si)
    {
        const auto &source = program.sources[si];
        const float begin = persistent ? .65f : .08f;
        const float end = persistent || death ? 1.0f : .88f;
        if (source.effect != effect.handle || source.stable_id != Hash32(persistent ? "surface_dissolve_edge" : "entity_surface_transition") ||
            source.type != VfxSourceType::Direct || source.knot_count != 2 || source.knots[0] != begin || source.knots[1] != end ||
            source.parameters.count != 0 || !Range(source.parameters, program.parameters) || !Range(source.outputs, program.outputs) ||
            age < static_cast<double>(source.knots[0]) || age >= static_cast<double>(source.knots[1])) continue;
        for (std::size_t oi = source.outputs.first; oi < static_cast<std::size_t>(source.outputs.first) + source.outputs.count; ++oi)
        {
            const auto &output = program.outputs[oi];
            if (output.source != si || output.profile != VfxOutputProfile::EntitySurface || output.shape != Hash32("") ||
                output.coverage_type != Hash32("mesh_surface") || output.surface_target != Hash32(persistent ? "boss_material_hook" : "boss_surface") ||
                output.motion != (persistent ? VfxMotionKind::NoiseEdgeFlowsUpward : VfxMotionKind::SurfaceNoiseThroughBoss) ||
                output.min_quality > static_cast<std::uint32_t>(quality) || output.min_quality > 2 ||
                output.min_quality != static_cast<std::uint32_t>(persistent ? VfxQuality::Low : VfxQuality::Medium) ||
                !std::isfinite(output.hdr) || output.hdr != (persistent ? 3.8f : death ? 5.5f : 4.8f) ||
                output.rgba[3] != (persistent ? .5f : .72f) ||
                std::any_of(output.rgba.begin(), output.rgba.end(), [](float v) { return !std::isfinite(v) || v < 0; }) ||
                output.rgba[3] > 1 || !Textures(program, output)) continue;
            auto command = owner;
            if (!Parameters(program, output, persistent, command)) continue;
            command.owner_started_tick = started; command.owner_until_tick = until; command.current_tick = current;
            command.output_index = static_cast<std::uint32_t>(oi); command.gradient_row = output.gradient_row;
            command.shape = persistent ? VfxEntitySurfaceShape::DissolveEdge : VfxEntitySurfaceShape::Transition;
            command.normalized_phase = static_cast<float>((age - source.knots[0]) / (source.knots[1] - source.knots[0]));
            command.gradient_age = command.normalized_phase; command.hdr = output.hdr; command.alpha = output.rgba[3];
            command.color = {output.rgba[0], output.rgba[1], output.rgba[2], output.rgba[3]};
            if (std::isfinite(command.normalized_phase) && command.normalized_phase >= 0 && command.normalized_phase < 1)
                result.push_back(command);
        }
    }
}
} // namespace

std::vector<VfxEntitySurfaceCommand> BuildVfxTypedEntitySurfaceCommands(
    const VfxProgramView &program, std::span<const VfxEntitySurfaceEventCapture> events,
    std::span<const VfxEntitySurfacePersistentCapture> persistent, double current_tick,
    double death_current_tick)
{
    std::vector<VfxEntitySurfaceCommand> result;
    for (const auto &capture : events)
    {
        const auto &event = capture.input;
        const auto *context = std::get_if<VfxContextPayload>(&event.payload);
        if (!context || context->owner_id == 0 || context->owner_id >= (1ull << 60) || event.sequence == 0 ||
            event.effect_handle == 0 || event.effect_handle > program.effects.size() || !BossInstance(capture.instance) ||
            capture.instance.stable_id != (kEnemyRenderPrefix | context->owner_id)) continue;
        const auto id = Identity(program, event.effect_handle);
        const bool death = id == kDeath;
        const auto &effect = program.effects[event.effect_handle - 1];
        const float seconds = death ? 2.0f : 1.5f;
        if ((id != kPhase && !death) || effect.handle != event.effect_handle || effect.input_mode != 0 || effect.timing_kind != 0 ||
            effect.payload_kind != Hash32("PresentationContextPayload") || effect.seconds != seconds ||
            event.event_tick > std::numeric_limits<Tick>::max() - static_cast<Tick>(seconds * 60)) continue;
        // Named clips are self-contained in the actual RenderInstance; legacy
        // boss poses require the captured snapshot pose, including blends.
        if (death && (!capture.retained_instance ||
            (capture.instance.animation_clip_id.value == 0 && !capture.has_animation_pose) ||
            (capture.has_animation_pose && !FinitePose(capture.animation_pose)))) continue;
        VfxEntitySurfaceCommand owner;
        owner.render_instance_id = capture.instance.stable_id; owner.owner_id = context->owner_id;
        owner.event_sequence = event.sequence; owner.status_episode_generation = event.status_episode_generation;
        owner.stable_seed = event.stable_seed; owner.has_retained_instance = death;
        if (death)
        {
            owner.retained_instance = capture.instance;
            owner.has_retained_animation_pose = capture.has_animation_pose;
            if (capture.has_animation_pose) owner.retained_animation_pose = capture.animation_pose;
        }
        const double event_current_tick = death
            ? std::max(current_tick, death_current_tick)
            : current_tick;
        Append(program, effect, false, death, event.quality, event.event_tick,
               event.event_tick + static_cast<Tick>(seconds * 60), event_current_tick, owner, result);
    }
    for (const auto &capture : persistent)
    {
        const auto &input = capture.input;
        const auto *entity = std::get_if<VfxEntityPayload>(&input.payload);
        if (!entity || entity->render_instance_id != capture.instance.stable_id || !BossInstance(capture.instance) ||
            input.stable_id == 0 || input.status_episode_generation == 0 ||
            input.source_visual_kind != static_cast<std::uint8_t>(PersistentVfxKind::BossPhaseTransition) ||
            input.effect_handle == 0 || input.effect_handle > program.effects.size() ||
            Identity(program, input.effect_handle) != kPersistent) continue;
        if (capture.owner_until_tick <= capture.owner_started_tick || current_tick < capture.owner_started_tick ||
            current_tick >= capture.owner_until_tick) continue;
        const auto &effect = program.effects[input.effect_handle - 1];
        if (effect.handle != input.effect_handle || effect.input_mode != 1 || effect.timing_kind != 1 ||
            effect.payload_kind != Hash32("EntityAttachmentPayload")) continue;
        VfxEntitySurfaceCommand owner;
        owner.render_instance_id = entity->render_instance_id; owner.owner_id = input.stable_id;
        owner.status_episode_generation = input.status_episode_generation; owner.stable_seed = input.stable_seed;
        Append(program, effect, true, false, input.quality, capture.owner_started_tick,
               capture.owner_until_tick, current_tick, owner, result);
    }
    return result;
}
} // namespace hs::runtime_detail
