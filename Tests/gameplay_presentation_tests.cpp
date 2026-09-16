#define RunGameplayPresentationTests RunGameplayPresentationTestsBase
#include "gameplay_presentation_tests_base.inl"
#undef RunGameplayPresentationTests

namespace gameplay_test
{
namespace
{

void TestPlayerTransitionAnimations()
{
    hs::SettingsData settings{};
    hs::RenderSnapshotStorage snapshot(64, 8, 4, 128);

    hs::GameReadModelStorage dive_model;
    dive_model.tick = 5;
    dive_model.player.active_animation_start = 1;
    dive_model.player.retreat_until = 12;
    dive_model.player.forced_move_skill = hs::SkillKind::Trap;
    Check(hs::ProjectRenderSnapshot(dive_model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot),
          "trap roll animation projection");
    Check(!snapshot.View().poses.empty() &&
              snapshot.View().poses.front().clip == hs::CharacterAnimationClip::Dive &&
              snapshot.View().poses.front().secondary_weight == 0.0f,
          "trap forward roll uses the authored full-body dive clip");

    hs::EnemyAnimationState stop_state;
    hs::GameReadModelStorage stop_model;
    stop_model.tick = 1;
    stop_model.player.locomotion_blend = 1.0f;
    stop_state.Update(stop_model.View(), {});
    stop_model.tick = 2;
    stop_model.player.locomotion_blend = 0.8f;
    stop_state.Update(stop_model.View(), {});
    snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(stop_model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot, 0xFF, &stop_state),
          "movement stop animation projection");
    Check(!snapshot.View().poses.empty() &&
              snapshot.View().poses.front().clip == hs::CharacterAnimationClip::Stop &&
              snapshot.View().poses.front().secondary_weight == 0.0f,
          "movement deceleration starts the authored stop clip");

    hs::EnemyAnimationState hit_state;
    hs::GameReadModelStorage hit_model;
    hit_model.tick = 10;
    hit_model.player.locomotion_blend = 1.0f;
    hs::DomainSignal hit{};
    hit.tick = 10;
    hit.kind = hs::DomainSignalKind::PlayerDamaged;
    hit_state.Update(hit_model.View(), std::span(&hit, 1));
    snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(hit_model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot, 0xFF, &hit_state),
          "player hit animation projection");
    Check(!snapshot.View().poses.empty() &&
              snapshot.View().poses.front().secondary_weight > 0.0f &&
              snapshot.View().poses.front().upper_body_clip ==
                  hs::CharacterAnimationClip::Hit &&
              snapshot.View().poses.front().upper_body_weight > 0.0f,
          "player damage overlays hit reaction on the upper body while locomotion continues");
}

} // namespace

void RunGameplayPresentationTests()
{
    RunGameplayPresentationTestsBase();
    TestPlayerTransitionAnimations();
}

} // namespace gameplay_test
