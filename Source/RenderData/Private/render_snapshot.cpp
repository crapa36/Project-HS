#include <hs/render_data/render_snapshot.hpp>

#include <bit>
#include <array>
#include <string_view>
#include <type_traits>

namespace hs
{

std::size_t RenderSceneInstanceKeyHash::operator()(
    RenderSceneInstanceKey key) const noexcept
{
    auto value = key.stable_id;
    value ^= static_cast<std::uint64_t>(key.fallback_ordinal) +
             0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    return static_cast<std::size_t>(value ^ (value >> 32u));
}

namespace
{
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

constexpr AssetId ProductAssetId(std::string_view path) noexcept
{
    auto hash = kFnvOffset;
    for (const auto character : path)
    {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= kFnvPrime;
    }
    return {hash};
}

constexpr auto kArcherAsset = ProductAssetId("render/character/archer.rpkg");
constexpr auto kMonsterMeleeAsset = ProductAssetId("render/character/enemy_melee.rpkg");
constexpr auto kMonsterRangedAsset = ProductAssetId("render/character/enemy_ranged.rpkg");
constexpr auto kMonsterSuicideAsset = ProductAssetId("render/character/enemy_suicide.rpkg");
constexpr auto kBossFiveMinuteAsset = ProductAssetId("render/character/boss_5m.rpkg");
constexpr auto kBossTenMinuteAsset = ProductAssetId("render/character/boss_10m.rpkg");
constexpr auto kBossFinalAsset = ProductAssetId("render/character/boss_final.rpkg");
constexpr auto kEnemyProjectileAsset = ProductAssetId("render/character/enemy_gel_projectile.rpkg");
constexpr auto kMeadowGroundAsset = ProductAssetId("render/meadow_ground.rpkg");
constexpr auto kMeadowWaterAsset = ProductAssetId("render/meadow_water.rpkg");
constexpr auto kGroundDetailMaterial = ProductAssetId("render/material/ground_detail.rpkg");
constexpr auto kTerrainMaterial = ProductAssetId("render/material/terrain.rpkg");

constexpr std::array kTreeTrunks{
    ProductAssetId("render/trunk_0.rpkg"), ProductAssetId("render/trunk_1.rpkg"),
    ProductAssetId("render/trunk_2.rpkg")};
constexpr std::array kTreeCanopies{
    ProductAssetId("render/leaf_0.rpkg"), ProductAssetId("render/leaf_1.rpkg"),
    ProductAssetId("render/leaf_2.rpkg")};
constexpr std::array kRocks{
    ProductAssetId("render/rock_0.rpkg"), ProductAssetId("render/rock_1.rpkg"),
    ProductAssetId("render/rock_2.rpkg"), ProductAssetId("render/rock_3.rpkg")};
constexpr std::array kGrasses{
    ProductAssetId("render/grass_0.rpkg"), ProductAssetId("render/grass_1.rpkg"),
    ProductAssetId("render/grass_2.rpkg"), ProductAssetId("render/grass_3.rpkg")};
constexpr std::array<AssetId, kGroundDetailVariantCount> kGroundDetails{
    ProductAssetId("render/flower_daisy.rpkg"), ProductAssetId("render/flower_buttercup.rpkg"),
    ProductAssetId("render/flower_violet.rpkg"), ProductAssetId("render/clover_patch.rpkg"),
    ProductAssetId("render/ground_leaves.rpkg"), ProductAssetId("render/twig_cluster.rpkg"),
    ProductAssetId("render/fence_short.rpkg"), ProductAssetId("render/shrub_low.rpkg"),
    ProductAssetId("render/stump_roots.rpkg"), ProductAssetId("render/crate_intact.rpkg"),
    ProductAssetId("render/crate_broken.rpkg"), ProductAssetId("render/barrel_intact.rpkg"),
    ProductAssetId("render/barrel_broken.rpkg"), ProductAssetId("render/cart_broken.rpkg"),
    ProductAssetId("render/fence_broken.rpkg"), ProductAssetId("render/fence_splitrail.rpkg"),
    ProductAssetId("render/sack_tied.rpkg"), ProductAssetId("render/fallen_log.rpkg"),
    ProductAssetId("render/rock_mossy.rpkg")};

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
    switch (mesh)
    {
    case RenderMesh::Archer:
        return kArcherAsset;
    case RenderMesh::MonsterMelee:
        return kMonsterMeleeAsset;
    case RenderMesh::MonsterRanged:
        return kMonsterRangedAsset;
    case RenderMesh::MonsterSuicide:
        return kMonsterSuicideAsset;
    case RenderMesh::BossFiveMinute:
        return kBossFiveMinuteAsset;
    case RenderMesh::BossTenMinute:
        return kBossTenMinuteAsset;
    case RenderMesh::BossFinal:
        return kBossFinalAsset;
    case RenderMesh::EnemyProjectile:
        return kEnemyProjectileAsset;
    case RenderMesh::TreeTrunk:
        return kTreeTrunks[variant % kTreeTrunks.size()];
    case RenderMesh::TreeCanopy:
        return kTreeCanopies[variant % kTreeCanopies.size()];
    case RenderMesh::Rock:
        return kRocks[variant % kRocks.size()];
    case RenderMesh::Grass:
        return kGrasses[variant % kGrasses.size()];
    case RenderMesh::GroundDetail:
        return kGroundDetails[variant % kGroundDetails.size()];
    case RenderMesh::MeadowSurface:
        return variant == 0 ? kMeadowGroundAsset : kMeadowWaterAsset;
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
        instance.material_id = instance.mesh == RenderMesh::GroundDetail
            ? kGroundDetailMaterial : instance.mesh == RenderMesh::MeadowSurface
            ? (instance.environment_variant == 0 ? kTerrainMaterial : kGroundDetailMaterial)
            : instance.asset_id;
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
    if (instance.animation_clip_id.value == 0 && instance.revisions.transform != 0 &&
        instance.revisions.material != 0 && instance.revisions.content != 0)
        return;
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
}

void RenderSnapshotStorage::CopyDynamicFrom(const RenderSnapshot &sample)
{
    header = sample.header;
    camera = sample.camera;
    instances_.assign(sample.instances.begin(), sample.instances.end());
    poses_.assign(sample.poses.begin(), sample.poses.end());
    captured_instances_.assign(sample.captured_instances.begin(), sample.captured_instances.end());
    lights_.assign(sample.lights.begin(), sample.lights.end());
    persistent_vfx_.assign(sample.persistent_vfx.begin(), sample.persistent_vfx.end());
    static_scene_ = sample.static_scene;
    ui_.clear();
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

bool RenderSnapshotStorage::AppendInstance(const RenderInstance &instance)
{
    instances_.push_back(instance);
    return true;
}

bool RenderSnapshotStorage::AppendPose(const AnimationPoseRef &pose)
{
    poses_.push_back(pose);
    return true;
}

bool RenderSnapshotStorage::AppendCapturedInstance(const CapturedRenderInstance &capture)
{
    captured_instances_.push_back(capture);
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
    return {header, instances_, poses_, lights_, ui_, persistent_vfx_, camera,
            static_scene_, captured_instances_};
}

void RenderSnapshotStorage::SetStaticScene(
    std::shared_ptr<const StaticRenderScene> scene) noexcept
{
    static_scene_ = std::move(scene);
}

} // namespace hs
