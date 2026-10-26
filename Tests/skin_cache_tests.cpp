#include "skin_cache.hpp"
#include "renderer_named_animation.hpp"

#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <limits>
#include <cmath>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
void CheckCompiledAnimation()
{
    namespace rd = hs::render_data;
    rd::CharacterAnimationExtensionData owned;
    owned.character_asset_id = hs::MakeAssetId("test/character");
    owned.bone_names = {"root", "child"}; owned.parents = {UINT16_MAX, 0};
    const std::array<float, 16> identity{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    owned.inverse_bind_matrices = {identity, identity};
    owned.clips.push_back({hs::MakeAssetId("test/clip"), "clip", 0, 2, 1.0f, false});
    owned.transforms.resize(4);
    owned.transforms[1].translation = {0,2,0};
    owned.transforms[2].translation = {4,0,0}; owned.transforms[3].translation = {0,2,0};
    std::vector<rd::PackedCharacterTransform> packed;
    for (const auto &row : owned.transforms) packed.push_back({row.translation, row.rotation, row.scale});
    const std::array<std::string_view, 2> names{"root", "child"};
    std::array<rd::AnimationClipDescriptor, 1> clips{{{owned.clips[0].id, "clip", 0, 2, 1.0f, false}}};
    rd::AnimationDescriptor compiled;
    compiled.character_asset_id = owned.character_asset_id; compiled.bone_names = names;
    compiled.parents = owned.parents; compiled.inverse_bind_matrices = owned.inverse_bind_matrices; compiled.clips = clips;
    hs::RenderInstance instance;
    instance.asset_id = owned.character_asset_id; instance.animation_clip_id = owned.clips[0].id;
    std::array<DirectX::XMFLOAT4X4, 2> expected{}, actual{};
    for (const bool loop : {false, true}) for (const float rate : {-1.0f, 0.5f, 1.0f})
        for (const float time : {-1.25f, 0.0f, 0.5f, 1.0f, 2.25f})
        {
            owned.clips[0].looping = loop; clips[0].looping = loop;
            instance.animation_time_seconds = time; instance.animation_playback_rate = rate;
            Check(static_cast<bool>(hs::SampleNamedCharacterAnimation(owned, instance, expected)), "owning named pose failed");
            Check(static_cast<bool>(hs::SampleNamedCharacterAnimation(compiled, packed, instance, actual)), "compiled named pose failed");
            Check(std::memcmp(expected.data(), actual.data(), sizeof(actual)) == 0, "compiled named pose differs");
        }
    clips[0].looping = false; instance.animation_time_seconds = 0.5f; instance.animation_playback_rate = 1.0f;
    Check(static_cast<bool>(hs::SampleNamedCharacterAnimation(compiled, packed, instance, actual)), "reference pose failed");
    Check(std::abs(actual[0]._14 - 2.0f) < 0.00001f && std::abs(actual[1]._14 - 2.0f) < 0.00001f &&
        std::abs(actual[1]._24 - 2.0f) < 0.00001f, "two-bone translated reference pose differs");
    const auto unchanged = actual;
    instance.asset_id = hs::MakeAssetId("wrong/character");
    Check(!hs::SampleNamedCharacterAnimation(compiled, packed, instance, actual), "wrong skeleton accepted");
    Check(std::memcmp(unchanged.data(), actual.data(), sizeof(actual)) == 0, "wrong skeleton changed output");
    instance.asset_id = owned.character_asset_id; instance.animation_time_seconds = std::numeric_limits<float>::infinity();
    Check(!hs::SampleNamedCharacterAnimation(compiled, packed, instance, actual), "nonfinite time accepted");
    Check(std::memcmp(unchanged.data(), actual.data(), sizeof(actual)) == 0, "failed sampling changed output");
}
} // namespace

int main()
{
    try
    {
        CheckCompiledAnimation();
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
