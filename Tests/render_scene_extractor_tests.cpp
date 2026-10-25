#include "renderer_scene_sample.hpp"
#include "visibility_encoding.hpp"
#include <bit>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void CheckDevelopmentAdapter()
{
    hs::RenderSnapshotStorage packet(4, 4, 0, 0);
    hs::RenderInstance instance;
    instance.mesh = hs::RenderMesh::Archer;
    instance.revisions = {11, 22, 33, 44};
    Check(packet.AddInstance(instance), "development instance append");
    Check(packet.View().instances[0].asset_id.value != 0 &&
              packet.View().instances[0].revisions == instance.revisions,
          "development explicit revisions retain identity population");
    hs::AnimationPoseRef pose;
    pose.revision = 77;
    Check(packet.AddPose(pose), "development pose append");
    pose.revision = 88;
    Check(packet.AddPose(pose) && packet.View().poses.size() == 2 &&
              packet.View().instances[0].revisions.pose == 88 &&
              hs::FindLastRenderPose(packet.View().poses, 0) == &packet.View().poses.back(),
          "last development pose owns the supplied pose revision");
    pose.instance_index = 3;
    Check(packet.AddPose(pose) && packet.View().poses.size() == 3,
          "development unassociated pose append remains accepted");
    instance.animation_clip_id.value = 123;
    instance.animation_time_seconds = 0.5;
    packet.Clear();
    Check(packet.AddInstance(instance), "named development instance append");
    pose.instance_index = 0;
    Check(packet.AddPose(pose) && packet.View().instances[0].revisions.pose ==
              hs::DeriveNamedAnimationPoseRevision(instance),
          "named animation takes precedence over supplied legacy pose revision");
    hs::CapturedRenderInstance capture;
    capture.instance = instance;
    capture.animation_pose = pose;
    Check(packet.AddCapturedInstance(capture) && packet.View().captured_instances[0].instance.revisions.pose ==
              hs::DeriveAnimationPoseRevision(pose), "manual capture retains its legacy pose-revision rule");
    packet.Clear();
    instance.revisions = {};
    instance.animation_clip_id = {};
    pose.revision = 0;
    Check(packet.AppendInstance(instance) && packet.AppendPose(pose) &&
              packet.AppendCapturedInstance({}), "complete producer operands append");
    Check(packet.View().instances[0].revisions == hs::RenderInstance::Revisions{} &&
              packet.View().poses[0].revision == 0 &&
              packet.View().captured_instances[0].instance.revisions == hs::RenderInstance::Revisions{},
          "complete producer append cannot calculate dynamic revision hashes");
    packet.Clear();
    Check(packet.View().instances.empty() && packet.View().poses.empty() &&
              packet.View().captured_instances.empty(), "packet reuse clears all logical operands");
}
void CheckExactFinalPoseInputs()
{
    hs::RenderInstance instance;
    instance.mesh = hs::RenderMesh::Archer;
    instance.revisions.pose = 99;
    hs::AnimationPoseRef pose;
    pose.revision = 99;
    const auto original = hs::MakeRenderPoseKey(instance, &pose);
    const auto changed = [&](auto edit) {
        auto candidate = pose;
        edit(candidate);
        Check(hs::MakeRenderPoseKey(instance, &candidate) != original,
              "supplied revision concealed an actual final pose operand");
    };
    changed([](auto &p) { p.clip = hs::CharacterAnimationClip::Run; });
    changed([](auto &p) { p.normalized_time = 0.5f; });
    changed([](auto &p) { p.playback_rate = 2; });
    changed([](auto &p) { p.secondary_clip = hs::CharacterAnimationClip::Death; });
    changed([](auto &p) { p.secondary_normalized_time = 0.25f; });
    changed([](auto &p) { p.secondary_playback_rate = 2; });
    changed([](auto &p) { p.secondary_weight = 0.25f; });
    changed([](auto &p) { p.upper_body_clip = hs::CharacterAnimationClip::Run; });
    changed([](auto &p) { p.upper_body_normalized_time = 0.25f; });
    changed([](auto &p) { p.upper_body_playback_rate = 2; });
    changed([](auto &p) { p.upper_body_weight = 0.25f; });
    changed([](auto &p) { p.normalized_time = -0.0f; });
    instance.animation_clip_id.value = 123;
    instance.animation_time_seconds = 1;
    const auto named = hs::MakeRenderPoseKey(instance, &pose);
    instance.animation_time_seconds = std::bit_cast<double>(std::bit_cast<std::uint64_t>(1.0) + 1);
    Check(hs::MakeRenderPoseKey(instance, &pose) != named, "named animation key retains all double time bits");
    Check(hs::MakeRenderSceneInstanceKey(0, UINT32_MAX - 1).fallback_ordinal == UINT32_MAX &&
              hs::MakeRenderSceneInstanceKey(42, 5) == hs::RenderSceneInstanceKey{42, 0},
          "anonymous ordinals and stable identities preserve their original contract");
    for (float radius : {0.0f, 0.035f, 1.0f, 128.5f})
        Check(std::bit_cast<std::uint32_t>(hs::EncodeGrassDeformationRadius(radius, hs::OcclusionMode::Disabled)) ==
                  std::bit_cast<std::uint32_t>(-radius), "occlusion constant retains signed-zero bits");
}
}
int main()
{
    try
    {
        CheckDevelopmentAdapter();
        CheckExactFinalPoseInputs();
        std::puts("Render scene sample tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
