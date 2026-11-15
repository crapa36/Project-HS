#include "vfx_typed_entity_surface_commands.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace hs::runtime_detail
{
namespace
{
constexpr std::uint64_t kEnemyRenderPrefix = 2ull << 60;

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
void Append(const VfxEntitySurfaceEffectRecipe &effect,
            std::span<const VfxEntitySurfaceRecipe> outputs,
            VfxQuality quality, Tick started, Tick until, double current,
            const VfxEntitySurfaceCommand &owner, std::vector<VfxEntitySurfaceCommand> &result)
{
    if (until <= started || current < started || current >= until ||
        static_cast<std::uint32_t>(quality) > 2) return;
    const auto age = static_cast<double>(current - started) / static_cast<double>(until - started);
    for (std::size_t i = effect.outputs.first; i < static_cast<std::size_t>(effect.outputs.first) + effect.outputs.count; ++i)
    {
        const auto &recipe = outputs[i];
        if (static_cast<std::uint32_t>(quality) < recipe.min_quality ||
            age < recipe.knot_start || age >= recipe.knot_end) continue;
        auto command=owner;
        command.owner_started_tick=started; command.owner_until_tick=until; command.current_tick=current;
        command.output_index=recipe.output_index; command.gradient_row=recipe.gradient_row;
        command.shape=recipe.shape; command.direction=recipe.direction;
        command.normalized_phase=static_cast<float>((age-recipe.knot_start)/(recipe.knot_end-recipe.knot_start));
        command.gradient_age=command.normalized_phase; command.hdr=recipe.hdr; command.alpha=recipe.alpha;
        command.color=recipe.color; command.noise_scale=recipe.noise_scale; command.edge_width=recipe.edge_width;
        if(std::isfinite(command.normalized_phase)&&command.normalized_phase>=0&&command.normalized_phase<1)
            result.push_back(command);
    }
}
} // namespace

std::vector<VfxEntitySurfaceCommand> BuildVfxTypedEntitySurfaceCommands(
    const VfxImage &image, std::span<const VfxEntitySurfaceEventCapture> events,
    std::span<const VfxEntitySurfacePersistentCapture> persistent, double current_tick,
    double death_current_tick)
{
    std::vector<VfxEntitySurfaceCommand> result;
    AppendVfxTypedEntitySurfaceCommands(image, events, persistent, current_tick,
                                        death_current_tick, result);
    return result;
}

void AppendVfxTypedEntitySurfaceCommands(
    const VfxImage &image, std::span<const VfxEntitySurfaceEventCapture> events,
    std::span<const VfxEntitySurfacePersistentCapture> persistent, double current_tick,
    double death_current_tick, std::vector<VfxEntitySurfaceCommand> &result)
{
    for (const auto &capture : events)
    {
        const auto &event = capture.input;
        const auto *context = std::get_if<VfxContextPayload>(&event.payload);
        if (!context || context->owner_id == 0 || context->owner_id >= (1ull << 60) || event.sequence == 0 ||
            event.effect_handle == 0 || event.effect_handle > image.entity_surface_effect_recipes.size() || !BossInstance(capture.instance) ||
            capture.instance.stable_id != (kEnemyRenderPrefix | context->owner_id)) continue;
        const auto &effect=image.entity_surface_effect_recipes[event.effect_handle-1];
        if (effect.route != VfxEntitySurfaceRoute::EventPhase &&
            effect.route != VfxEntitySurfaceRoute::EventDeath) continue;
        const bool death = effect.route == VfxEntitySurfaceRoute::EventDeath;
        const float seconds = effect.seconds;
        if (event.event_tick > std::numeric_limits<Tick>::max() - static_cast<Tick>(seconds * 60)) continue;
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
        Append(effect,image.entity_surface_recipes,event.quality,event.event_tick,
               event.event_tick+static_cast<Tick>(seconds*60),event_current_tick,owner,result);
    }
    for (const auto &capture : persistent)
    {
        const auto &input = capture.input;
        const auto *entity = std::get_if<VfxEntityPayload>(&input.payload);
        if (!entity || entity->render_instance_id != capture.instance.stable_id || !BossInstance(capture.instance) ||
            input.stable_id == 0 || input.status_episode_generation == 0 ||
            input.source_visual_kind != static_cast<std::uint8_t>(PersistentVfxKind::BossPhaseTransition) ||
            input.effect_handle == 0 || input.effect_handle > image.entity_surface_effect_recipes.size()) continue;
        const auto &effect=image.entity_surface_effect_recipes[input.effect_handle-1];
        if (effect.route != VfxEntitySurfaceRoute::PersistentTransition) continue;
        if (capture.owner_until_tick <= capture.owner_started_tick || current_tick < capture.owner_started_tick ||
            current_tick >= capture.owner_until_tick) continue;
        VfxEntitySurfaceCommand owner;
        owner.render_instance_id = entity->render_instance_id; owner.owner_id = input.stable_id;
        owner.status_episode_generation = input.status_episode_generation; owner.stable_seed = input.stable_seed;
        Append(effect,image.entity_surface_recipes,input.quality,capture.owner_started_tick,
               capture.owner_until_tick,current_tick,owner,result);
    }
}
} // namespace hs::runtime_detail
