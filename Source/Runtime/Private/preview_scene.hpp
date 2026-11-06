#pragma once

#include <hs/runtime/application.hpp>
#include <hs/render_data/render_snapshot_values.hpp>
#include <hs/render_data/devtools_frame_data.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace hs::runtime_detail
{
// Owns the scene spans submitted by one preview package.
struct PreviewScene
{
    std::vector<RenderInstance> instances;
    std::vector<AnimationPoseRef> poses;
    std::vector<AnimationPoseRef> previous_poses;

    static CameraView Camera(const ApplicationConfig &config, const PreviewEditValues &edit, CameraView camera)
    {
        const bool family = config.slime_family_preview_count != 0;
        const bool monster = config.monster_preview_asset < 6 || family;
        if (config.environment_preview)
        {
            camera.target = {0, 1, 0};
            if (!config.preview_camera_override) { camera.yaw_degrees = 0; camera.pitch_degrees = 22; camera.distance = 22; }
        }
        if (monster) camera.target.y = !family && config.monster_preview_asset >= 3 ? 3.0f : 1.0f;
        else if (edit.enabled) camera.target.y = 1.0f;
        if (family) { camera.target.x = 0; camera.target.z = 0; }
        if (config.preview_camera_override)
        {
            camera.yaw_degrees = config.preview_camera_yaw; camera.pitch_degrees = config.preview_camera_pitch;
            camera.distance = config.preview_camera_distance;
        }
        else
        {
            if (edit.enabled) { camera.yaw_degrees = edit.yaw; camera.pitch_degrees = edit.pitch; }
            if (family) camera.distance = std::max(10.0f, std::sqrt(config.slime_family_preview_count * 3.0f) * 1.9f);
            else if (monster) camera.distance = config.monster_preview_asset >= 3 ? 18.0f : 8.0f;
            else if (edit.enabled) camera.distance = edit.distance;
        }
        return camera;
    }

    void Prepare(const ApplicationConfig &config, const PreviewEditValues &edit, RenderSnapshotPair &pair)
    {
        const auto count = config.slime_family_preview_count;
        if (!count && config.monster_preview_asset >= 6 && !config.environment_preview)
        {
            if (edit.enabled && edit.pose_override)
            {
                const auto override_poses = [&](RenderSnapshot &snapshot, std::vector<AnimationPoseRef> &output) {
                    output.assign(snapshot.poses.begin(), snapshot.poses.end());
                    for (std::size_t index = 0; index < snapshot.instances.size(); ++index)
                    {
                        if (snapshot.instances[index].mesh != RenderMesh::Archer) continue;
                        std::erase_if(output, [index](const auto &pose) { return pose.instance_index == index; });
                        AnimationPoseRef pose{};
                        pose.instance_index = static_cast<std::uint32_t>(index);
                        pose.clip = static_cast<CharacterAnimationClip>(edit.base_clip);
                        pose.normalized_time = edit.base_time;
                        pose.secondary_clip = static_cast<CharacterAnimationClip>(edit.secondary_clip);
                        pose.secondary_normalized_time = edit.secondary_time;
                        pose.secondary_weight = edit.secondary_weight;
                        pose.upper_body_clip = static_cast<CharacterAnimationClip>(edit.upper_clip);
                        pose.upper_body_normalized_time = edit.upper_time;
                        pose.upper_body_weight = edit.upper_weight;
                        output.push_back(pose);
                    }
                    snapshot.poses = output;
                };
                override_poses(pair.current, poses);
                if (pair.has_previous) override_poses(pair.previous, previous_poses);
            }
            return;
        }
        if (count)
        {
            const auto columns = count == 1 ? 3u : static_cast<std::uint32_t>(std::ceil(std::sqrt(count * 3.0f)));
            const auto rows = (count * 3 + columns - 1) / columns;
            for (std::uint32_t i = 0; i < count * 3; ++i)
            {
                const auto kind = i % 3;
                const float x = count == 1 ? (static_cast<float>(i) - 1.0f) * 2.5f :
                    (static_cast<float>(i % columns) - (columns - 1) * .5f) * 1.1f;
                const float z = count == 1 ? 0.0f :
                    (static_cast<float>(i / columns) - (rows - 1) * .5f) * 1.1f;
                const float scale = kind == 0 ? 1.25f : kind == 1 ? 1.40625f : 1.5625f;
                RenderInstance instance{};
                instance.position = {x, 0, z}; instance.scale = {scale, scale, scale};
                instance.color_rgba = 0xffffffffu; instance.mesh = static_cast<RenderMesh>(10 + kind);
                instance.stable_id = 0x53504c4d00000000ull | static_cast<std::uint64_t>(i + 1);
                PopulateRenderInstanceIdentity(instance);
                AnimationPoseRef pose{};
                pose.instance_index = static_cast<std::uint32_t>(instances.size());
                pose.clip = static_cast<CharacterAnimationClip>(std::min(config.monster_preview_clip, 4u));
                pose.normalized_time = config.monster_preview_time;
                poses.push_back(pose);
                instances.push_back(instance);
                if (kind == 2)
                {
                    RenderInstance projectile{};
                    projectile.position = {x - 1.25f, .45f, z - 1.5f};
                    projectile.scale = {.24f, .24f, .8f}; projectile.color_rgba = 0xff60d060u;
                    projectile.mesh = RenderMesh::EnemyProjectile;
                    projectile.stable_id = 0x53504c4d00100000ull | static_cast<std::uint64_t>(i + 1);
                    PopulateRenderInstanceIdentity(projectile);
                    instances.push_back(projectile);
                }
            }
        }
        else if (config.monster_preview_asset < 6)
        {
            const auto mesh = static_cast<RenderMesh>(static_cast<std::uint32_t>(RenderMesh::MonsterMelee) + config.monster_preview_asset);
            const auto found = std::ranges::find(pair.current.instances, mesh, &RenderInstance::mesh);
            if (found != pair.current.instances.end())
            {
                auto instance = *found;
                instance.position = {}; instance.yaw = 0;
                if (!instance.stable_id) instance.stable_id = 0x505245564d4f4e01ull;
                PopulateRenderInstanceIdentity(instance);
                instances.push_back(instance);
                AnimationPoseRef pose{};
                pose.clip = static_cast<CharacterAnimationClip>(std::min(config.monster_preview_clip, 4u));
                pose.normalized_time = config.monster_preview_time;
                poses.push_back(pose);
            }
        }
        if (config.environment_preview)
        {
            instances.clear(); poses.clear();
            const auto add = [&](RenderMesh mesh, Float3 position, Float3 scale, std::uint32_t variant) {
                RenderInstance instance{};
                instance.mesh = mesh; instance.position = position; instance.scale = scale;
                instance.environment_variant = variant; instance.environment_seed = 0x4853454eu;
                PopulateRenderInstanceIdentity(instance);
                instances.push_back(instance);
            };
            for (std::uint32_t variant = 0; variant < 3; ++variant)
            {
                const float x = static_cast<float>(variant) * 5.0f - 5.0f;
                add(RenderMesh::TreeTrunk, {x, 0, 0}, {1, 1, 1}, variant);
                add(RenderMesh::TreeCanopy, {x, 0, 0}, {1, 1, 1}, variant);
            }
            for (std::uint32_t variant = 0; variant < 4; ++variant)
            {
                const float x = static_cast<float>(variant) * 3.0f - 4.5f;
                add(RenderMesh::Rock, {x, 0, -5}, {.8f, .8f, .8f}, variant);
                add(RenderMesh::Grass, {x, 0, -8}, {1, 1, 1}, variant);
            }
            for (int z = -5; z <= 2; ++z)
                for (int x = -5; x <= 5; ++x)
                    add(RenderMesh::Ground, {static_cast<float>(x) * 2.5f, -.055f, static_cast<float>(z) * 2.5f},
                        {2.5f, .1f, 2.5f}, 0);
        }
        pair.current.instances = instances;
        pair.current.poses = poses;
        pair.current.static_scene.reset();
        if (pair.has_previous)
        {
            pair.previous.instances = instances;
            pair.previous.poses = poses;
            pair.previous.static_scene.reset();
        }
    }
};
} // namespace hs::runtime_detail
