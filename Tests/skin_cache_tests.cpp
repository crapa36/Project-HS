#include "skin_cache.hpp"

#include <cstdio>
#include <stdexcept>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
} // namespace

int main()
{
    try
    {
        hs::SkinCache cache;
        hs::RenderInstance instance;
        instance.stable_id = 7;
        instance.scale = {2.0f, 1.0f, 1.0f};
        instance.mesh = hs::RenderMesh::MonsterMelee;
        hs::PopulateRenderInstanceRevisions(instance);
        hs::AnimationPoseRef pose;
        pose.clip = hs::CharacterAnimationClip::Run;
        pose.normalized_time = 0.25f;
        pose.revision = hs::DeriveAnimationPoseRevision(pose);

        cache.BeginFrame();
        auto first = cache.Resolve({7, 0}, instance, &pose, 1.5f);
        cache.EndFrame();
        cache.Commit();
        Check(!first.history_valid && first.previous_pose == first.current_pose,
              "new skin cache entry must initialize previous from current");
        Check(first.conservative_radius == 3.0f, "scaled conservative bound");

        cache.BeginFrame();
        pose.normalized_time = 0.5f;
        pose.revision = hs::DeriveAnimationPoseRevision(pose);
        auto second = cache.Resolve({7, 0}, instance, &pose, 1.5f);
        cache.EndFrame();
        cache.Commit();
        Check(second.history_valid && second.previous_time == 0.25f &&
                  second.current_time == 0.5f,
              "skin cache must retain previous deformation state");

        cache.BeginFrame();
        pose.normalized_time = 0.75f;
        pose.revision = hs::DeriveAnimationPoseRevision(pose);
        static_cast<void>(cache.Resolve({7, 0}, instance, &pose, 1.5f));
        cache.EndFrame();
        cache.Abort();
        cache.BeginFrame();
        pose.normalized_time = 0.9f;
        pose.revision = hs::DeriveAnimationPoseRevision(pose);
        const auto after_abort = cache.Resolve({7, 0}, instance, &pose, 1.5f);
        Check(after_abort.history_valid && after_abort.previous_time == 0.5f,
              "aborted pose cannot become previous submitted deformation");
        cache.EndFrame();
        cache.Commit();

        cache.BeginFrame();
        cache.EndFrame();
        cache.BeginFrame();
        const auto after_missing_abort = cache.Resolve({7, 0}, instance, &pose, 1.5f);
        Check(after_missing_abort.history_valid && after_missing_abort.previous_time == 0.9f,
              "unsubmitted empty frame cannot destroy skin history");
        cache.EndFrame();
        cache.Commit();

        cache.BeginFrame(true);
        const auto reset = cache.Resolve({7, 0}, instance, &pose, 1.5f);
        Check(!reset.history_valid, "new session prepares fresh pose history");
        cache.EndFrame();
        cache.Abort();
        cache.BeginFrame();
        const auto after_reset_abort = cache.Resolve({7, 0}, instance, &pose, 1.5f);
        Check(after_reset_abort.history_valid,
              "aborted session reset cannot erase submitted pose history");
        cache.EndFrame();
        cache.Commit();

        cache.BeginFrame();
        instance.environment_variant = 2;
        instance.revisions = {};
        hs::PopulateRenderInstanceRevisions(instance);
        auto content_changed = cache.Resolve({7, 0}, instance, &pose, 1.5f);
        cache.EndFrame();
        cache.Commit();
        Check(!content_changed.history_valid,
              "exact content binding change must invalidate skin history");

        cache.BeginFrame();
        cache.EndFrame();
        cache.Commit();
        Check(cache.ActiveCount() == 0, "empty frame active count");

        std::puts("Skin cache tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
