#include <hs/core/render_snapshot.hpp>

#include <bit>
#include <array>
#include <type_traits>

namespace hs
{
namespace
{
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void HashByte(std::uint64_t &hash, std::uint8_t value) noexcept
{
    hash ^= value;
    hash *= kFnvPrime;
}

template <typename T>
void HashUnsigned(std::uint64_t &hash, T value) noexcept
{
    for (std::size_t byte = 0; byte < sizeof(T); ++byte)
        HashByte(hash, static_cast<std::uint8_t>(
                           (static_cast<std::make_unsigned_t<T>>(value) >> (byte * 8u)) & 0xffu));
}

void HashFloat(std::uint64_t &hash, float value) noexcept
{
    HashUnsigned(hash, std::bit_cast<std::uint32_t>(value));
}

AssetId PackagedRenderAsset(RenderMesh mesh, std::uint32_t variant) noexcept
{
    constexpr std::array<std::string_view, 3> trunks{
        "render/trunk_0.rpkg", "render/trunk_1.rpkg", "render/trunk_2.rpkg"};
    constexpr std::array<std::string_view, 3> leaves{
        "render/leaf_0.rpkg", "render/leaf_1.rpkg", "render/leaf_2.rpkg"};
    constexpr std::array<std::string_view, 4> rocks{
        "render/rock_0.rpkg", "render/rock_1.rpkg", "render/rock_2.rpkg",
        "render/rock_3.rpkg"};
    constexpr std::array<std::string_view, 4> grasses{
        "render/grass_0.rpkg", "render/grass_1.rpkg", "render/grass_2.rpkg",
        "render/grass_3.rpkg"};
    switch (mesh)
    {
    case RenderMesh::Archer:
        return MakeAssetId("render/character/archer.rpkg");
    case RenderMesh::MonsterMelee:
        return MakeAssetId("render/character/enemy_melee.rpkg");
    case RenderMesh::MonsterRanged:
        return MakeAssetId("render/character/enemy_ranged.rpkg");
    case RenderMesh::MonsterSuicide:
        return MakeAssetId("render/character/enemy_suicide.rpkg");
    case RenderMesh::BossFiveMinute:
        return MakeAssetId("render/character/boss_5m.rpkg");
    case RenderMesh::BossTenMinute:
        return MakeAssetId("render/character/boss_10m.rpkg");
    case RenderMesh::BossFinal:
        return MakeAssetId("render/character/boss_final.rpkg");
    case RenderMesh::EnemyProjectile:
        return MakeAssetId("render/character/enemy_gel_projectile.rpkg");
    case RenderMesh::TreeTrunk:
        return MakeAssetId(trunks[variant % trunks.size()]);
    case RenderMesh::TreeCanopy:
        return MakeAssetId(leaves[variant % leaves.size()]);
    case RenderMesh::Rock:
        return MakeAssetId(rocks[variant % rocks.size()]);
    case RenderMesh::Grass:
        return MakeAssetId(grasses[variant % grasses.size()]);
    default:
        return {};
    }
}
} // namespace

void PopulateRenderInstanceIdentity(RenderInstance &instance) noexcept
{
    if (instance.asset_id.value == 0)
        instance.asset_id = PackagedRenderAsset(instance.mesh, instance.environment_variant);
    // Current character/environment packages own the vertex material binding.
    // Keep that real product identity instead of inventing a separate material
    // asset before Generation publishes one.
    if (instance.material_id.value == 0 && instance.asset_id.value != 0)
        instance.material_id = instance.asset_id;
}

RenderInstance::Revisions DeriveRenderInstanceRevisions(
    const RenderInstance &instance) noexcept
{
    RenderInstance::Revisions revisions;
    revisions.pose = DeriveNamedAnimationPoseRevision(instance);
    revisions.transform = kFnvOffset;
    for (const auto value : {instance.position.x, instance.position.y, instance.position.z,
                             instance.yaw, instance.scale.x, instance.scale.y,
                             instance.scale.z})
        HashFloat(revisions.transform, value);

    revisions.material = kFnvOffset;
    HashUnsigned(revisions.material, instance.material_id.value);
    HashUnsigned(revisions.material, instance.color_rgba);
    HashUnsigned(revisions.material, instance.status_visual_mask);

    revisions.content = kFnvOffset;
    HashUnsigned(revisions.content, instance.asset_id.value);
    HashUnsigned(revisions.content, static_cast<std::uint8_t>(instance.mesh));
    HashUnsigned(revisions.content, instance.environment_seed);
    HashUnsigned(revisions.content, instance.environment_variant);
    HashUnsigned(revisions.content, instance.vfx_owner_id);
    return revisions;
}

std::uint64_t DeriveAnimationPoseRevision(const AnimationPoseRef &pose) noexcept
{
    auto revision = kFnvOffset;
    HashUnsigned(revision, static_cast<std::uint8_t>(pose.clip));
    HashFloat(revision, pose.normalized_time);
    HashFloat(revision, pose.playback_rate);
    HashUnsigned(revision, static_cast<std::uint8_t>(pose.secondary_clip));
    HashFloat(revision, pose.secondary_normalized_time);
    HashFloat(revision, pose.secondary_playback_rate);
    HashFloat(revision, pose.secondary_weight);
    HashUnsigned(revision, static_cast<std::uint8_t>(pose.upper_body_clip));
    HashFloat(revision, pose.upper_body_normalized_time);
    HashFloat(revision, pose.upper_body_playback_rate);
    HashFloat(revision, pose.upper_body_weight);
    return revision;
}

std::uint64_t DeriveNamedAnimationPoseRevision(const RenderInstance &instance) noexcept
{
    if (instance.animation_clip_id.value == 0) return 0;
    auto revision = kFnvOffset;
    HashUnsigned(revision, instance.animation_clip_id.value);
    HashUnsigned(revision, std::bit_cast<std::uint64_t>(instance.animation_time_seconds));
    HashFloat(revision, instance.animation_playback_rate);
    return revision;
}

void PopulateRenderInstanceRevisions(RenderInstance &instance) noexcept
{
    PopulateRenderInstanceIdentity(instance);
    const auto derived = DeriveRenderInstanceRevisions(instance);
    if (instance.animation_clip_id.value != 0) instance.revisions.pose = derived.pose;
    if (instance.revisions.transform == 0) instance.revisions.transform = derived.transform;
    if (instance.revisions.material == 0) instance.revisions.material = derived.material;
    if (instance.revisions.content == 0) instance.revisions.content = derived.content;
}

RenderSnapshotStorage::RenderSnapshotStorage(std::size_t instance_capacity,
                                             std::size_t pose_capacity,
                                             std::size_t light_capacity,
                                             std::size_t ui_capacity)
{
    instances_.reserve(instance_capacity);
    poses_.reserve(pose_capacity);
    lights_.reserve(light_capacity);
    ui_.reserve(ui_capacity);
    persistent_vfx_.reserve(instance_capacity);
}

void RenderSnapshotStorage::Clear() noexcept
{
    instances_.clear();
    poses_.clear();
    captured_instances_.clear();
    lights_.clear();
    ui_.clear();
    persistent_vfx_.clear();
    static_scene_.reset();
    ClearSceneDelta();
}

bool RenderSnapshotStorage::AddInstance(const RenderInstance &instance)
{
    auto stored = instance;
    PopulateRenderInstanceRevisions(stored);
    instances_.push_back(stored);
    return true;
}

bool RenderSnapshotStorage::AddPose(const AnimationPoseRef &pose)
{
    auto stored = pose;
    const auto named_revision = stored.instance_index < instances_.size()
        ? DeriveNamedAnimationPoseRevision(instances_[stored.instance_index]) : 0;
    if (named_revision != 0) stored.revision = named_revision;
    else if (stored.revision == 0) stored.revision = DeriveAnimationPoseRevision(stored);
    poses_.push_back(stored);
    if (stored.instance_index < instances_.size())
        instances_[stored.instance_index].revisions.pose = stored.revision;
    return true;
}

bool RenderSnapshotStorage::AddCapturedInstance(const CapturedRenderInstance &capture)
{
    auto stored = capture;
    PopulateRenderInstanceRevisions(stored.instance);
    stored.animation_pose.revision = DeriveAnimationPoseRevision(stored.animation_pose);
    stored.instance.revisions.pose = stored.animation_pose.revision;
    captured_instances_.push_back(stored);
    return true;
}

bool RenderSnapshotStorage::AddLight(const LightView &light)
{
    lights_.push_back(light);
    return true;
}

bool RenderSnapshotStorage::AddUi(const UiModel &ui)
{
    ui_.push_back(ui);
    return true;
}

bool RenderSnapshotStorage::AddPersistentVfx(const PersistentVfxVisual &visual)
{
    persistent_vfx_.push_back(visual);
    return true;
}

std::size_t RenderSnapshotStorage::InstanceCount() const noexcept
{
    return instances_.size();
}

RenderSnapshot RenderSnapshotStorage::View() const noexcept
{
    RenderSceneDeltaView scene_delta;
    if (scene_delta_published_)
    {
        scene_delta = {scene_delta_.instances,
                       scene_delta_.destroyed,
                       scene_delta_.base_generation,
                       scene_delta_.generation,
                       scene_delta_.created,
                       scene_delta_.updated,
                       scene_delta_.unchanged,
                       true};
    }
    return {header, instances_, poses_, lights_, ui_, persistent_vfx_, camera,
            static_scene_, scene_delta, captured_instances_};
}

void RenderSnapshotStorage::SetStaticScene(
    std::shared_ptr<const StaticRenderScene> scene) noexcept
{
    static_scene_ = std::move(scene);
}

void RenderSnapshotStorage::SetSceneDelta(RenderSceneDelta delta)
{
    scene_delta_ = std::move(delta);
    scene_delta_published_ = true;
}

void RenderSnapshotStorage::ClearSceneDelta() noexcept
{
    scene_delta_ = {};
    scene_delta_published_ = false;
}

} // namespace hs
