#include <hs/presentation/projector.hpp>

#define Update UpdateLegacy
#define ProjectRenderSnapshot ProjectRenderSnapshotBase
#include "snapshot_projector_base.inl"
#undef ProjectRenderSnapshot
#undef Update

namespace hs
{

bool ProjectRenderSnapshot(const GameReadModel &model,
                           const PresentationCatalog &presentation,
                           const PresentationUiState &ui,
                           const SettingsData &settings,
                           RenderSnapshotStorage &snapshot,
                           std::uint8_t pending_rebind_slot,
                           const EnemyAnimationState *enemy_animations)
{
    const auto complete = ProjectRenderSnapshotBase(
        model, presentation, ui, settings, snapshot, pending_rebind_slot,
        enemy_animations);
    if (!complete || model.session.phase == SessionPhase::Defeat)
        return complete;

    auto *pose = snapshot.MutablePose(0);
    if (!pose)
        return complete;

    constexpr Tick kStopTicks = 24;
    constexpr Tick kHitTicks = 18;
    const bool diving = model.player.forced_move_skill == SkillKind::Trap &&
                        model.tick < model.player.retreat_until;
    if (diving)
    {
        const auto duration = std::max<Tick>(
            1, model.player.retreat_until - model.player.active_animation_start - 1);
        pose->clip = CharacterAnimationClip::Dive;
        pose->normalized_time = std::clamp(
            static_cast<float>(model.tick - model.player.active_animation_start) /
                static_cast<float>(duration),
            0.0f, 1.0f);
        pose->playback_rate = 1.0f;
        pose->secondary_weight = 0.0f;
        pose->upper_body_weight = 0.0f;
    }
    else if (enemy_animations)
    {
        if (const auto stopped = enemy_animations->PlayerStopStart();
            stopped && model.tick >= *stopped && model.tick - *stopped < kStopTicks)
        {
            pose->clip = CharacterAnimationClip::Stop;
            pose->normalized_time = std::clamp(
                static_cast<float>(model.tick - *stopped) /
                    static_cast<float>(kStopTicks),
                0.0f, 1.0f);
            pose->playback_rate = 1.0f;
            pose->secondary_weight = 0.0f;
        }
    }

    if (enemy_animations)
    {
        if (const auto hit = enemy_animations->PlayerHitStart();
            hit && model.tick >= *hit && model.tick - *hit < kHitTicks)
        {
            const auto elapsed = model.tick - *hit;
            const auto fade_in = std::min(1.0f,
                static_cast<float>(elapsed + 1) / 3.0f);
            const auto fade_out = std::min(1.0f,
                static_cast<float>(kHitTicks - elapsed) / 5.0f);
            pose->upper_body_clip = CharacterAnimationClip::Hit;
            pose->upper_body_normalized_time = std::clamp(
                static_cast<float>(elapsed) / static_cast<float>(kHitTicks),
                0.0f, 1.0f);
            pose->upper_body_playback_rate = 1.0f;
            pose->upper_body_weight = std::min(fade_in, fade_out);
        }
    }
    return complete;
}

} // namespace hs
