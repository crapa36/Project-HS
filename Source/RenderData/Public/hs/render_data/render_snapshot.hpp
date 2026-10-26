#pragma once

#include <hs/core/cooked_format.hpp>
#include <hs/core/types.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <string>
#include <vector>

namespace hs
{

enum class StatusVisual : std::uint32_t
{
    Bleed = 1u << 0,
    Burn = 1u << 1,
    Slow = 1u << 2,
    Mark = 1u << 3,
};

enum class PersistentVfxKind : std::uint8_t
{
    TrapPending,
    TrapArmed,
    FireArea,
    SlowArea,
    ArrowRainArea,
    DamageTrail,
    ChargeGuide,
    RangeIndicator,
    ProjectileTrail,
    ProjectileTrailOuter,
    RicochetProjectileTrail,
    BossAreaActive,
    ProjectileHead,
    BossDashWarning,
    BossVolleyWarning,
    RangedEnemyWarning,
    BossShockwaveWavefront,
    BossShockwaveWarning,
    RicochetLink,
    RicochetReturnLink,
    BurnTransferLink,
    RelicChainLink,
    MiniBombWarning,
    EnemySpawnWarning,
    BossSpawnWarning,
    BossAreaWarning,
    SuicideEnemyWarning,
    EnemyBleedStatus, EnemyBurnStatus, EnemySlowStatus, EnemyMarkStatus,
    UpgradeSlowArea,
    BossDashWake,
    BossPhase2Aura,
    BossPhaseTransition,
    PlayerInvulnerableLoop,
    PlayerLowHealthVignette,
    PlayerBowDraw,
    ChargedFullReady,
    EmpoweredReady,
    PickupXpIdle,
    PickupHealIdle,
    PickupMagnetIdle,
    PickupRelicIdle,
    MultishotRetarget,
    BasicArrowReturn,
    RicochetBleedExtend,
    ChargedOverchargeLoop,
    ScheduledAreaDelivery,
    SuicideCountdownLoop,
    PickupXpMagnetTrail,
    WaveTimer,
};

struct PersistentVfxVisual
{
    Float3 position{};
    float yaw{};
    float radius{};
    float length{};
    PersistentVfxKind kind{};
    std::uint64_t stable_id{};
    std::uint64_t status_episode_generation{};
    Tick active_tick{};
    // Area-owned presentation metadata. Keep these as low-level primitives so
    // Core does not depend on GameDomain's enums or read model types.
    Tick expires{};
    // Draw progression target; it does not end a live owner when expires is 0.
    Tick source_horizon_tick{};
    std::uint64_t cast_id{};
    std::uint8_t skill{};
    std::uint8_t source_upgrade{0xFF};
    // Wavefront visuals carry current annulus boundaries, not travel endpoints.
    float ring_inner_radius{};
    float ring_outer_radius{};
    std::uint8_t gap_count{};
    float gap_half_angle_degrees{};
    float gap_offset_degrees{};
    float cone_half_angle_degrees{};
    // Authoritative charged-shot progression for charge-owned persistent VFX.
    float charge_ratio{};
    // Authoritative projectile ingress. These fields are populated from the
    // live actor so path/head rendering never forecasts lifetime or integrates
    // a cosmetic trail position.
    AssetId effect_asset{};
    Float3 projectile_current_position{};
    Float3 projectile_previous_position{};
    Float3 projectile_velocity{};
    float projectile_hitbox_radius{};
    Tick projectile_spawned_tick{};
    std::uint64_t projectile_owner_id{};
    std::uint32_t projectile_state_flags{};
    // Captured link source uses position; target and full world width remain
    // fixed until expires. active_tick is the emission tick.
    Float3 link_target_position{};
    float link_width{};
    Float3 return_start_position{};
    float entity_health_fraction{1.0f};
    std::uint32_t entity_state_flags{};
    std::uint64_t entity_render_id{};
};

enum class RenderMesh : std::uint8_t
{
    Archer,
    Enemy,
    EnemyRanged,
    EnemySuicide,
    Boss,
    PlayerProjectile,
    EnemyProjectile,
    Area,
    Pickup,
    Ground,
    MonsterMelee,
    MonsterRanged,
    MonsterSuicide,
    BossFiveMinute,
    BossTenMinute,
    BossFinal,
    TreeTrunk,
    TreeCanopy,
    Rock,
    Grass,
    DirtPatch,
    GroundDetail,
    MeadowSurface,
};

inline constexpr std::uint32_t kGroundDetailVariantCount = 19;

[[nodiscard]] constexpr bool HasEnvironmentWind(RenderMesh mesh, std::uint32_t variant) noexcept
{
    return mesh == RenderMesh::Grass ||
        (mesh == RenderMesh::GroundDetail && (variant <= 3u || variant == 7u));
}


struct RenderInstance
{
    Float3 position{};
    float yaw{};
    Float3 scale{1.0f, 1.0f, 1.0f};
    std::uint32_t color_rgba{0xFFFFFFFFu};
    RenderMesh mesh{};
    std::uint64_t stable_id{};
    std::uint32_t status_visual_mask{};
    std::uint32_t environment_seed{};
    std::uint32_t environment_variant{};
    std::uint64_t vfx_owner_id{}; // Shared warning group; never used as interpolation identity.
    // Stable render-content identities. A zero ID means the instance is still
    // backed by a procedural/compatibility path rather than a Generation
    // product. Where material metadata is embedded in the render package,
    // material_id intentionally equals asset_id until that product is split.
    AssetId asset_id{};
    AssetId material_id{};
    struct Revisions
    {
        std::uint64_t transform{};
        std::uint64_t pose{};
        std::uint64_t material{};
        std::uint64_t content{};

        [[nodiscard]] bool operator==(const Revisions &) const noexcept = default;
    } revisions{};
    // Explicit authored clip identity. Zero retains the legacy five semantic
    // AnimationPoseRef slots. Time is local, continuous seconds before rate;
    // the selected authored clip owns looping and duration. No per-instance
    // names, renderer metadata or simulation timing changes are required.
    AssetId animation_clip_id{};
    double animation_time_seconds{};
    float animation_playback_rate{1.0f};
    // Last instantaneous movement signal; independent of content and pose identity.
    Sequence motion_epoch{};
};

struct AnimationPoseRef
{
    std::uint32_t instance_index{};
    CharacterAnimationClip clip{CharacterAnimationClip::Idle};
    float normalized_time{};
    float playback_rate{1.0f};
    CharacterAnimationClip secondary_clip{CharacterAnimationClip::Run};
    float secondary_normalized_time{};
    float secondary_playback_rate{1.0f};
    float secondary_weight{};
    CharacterAnimationClip upper_body_clip{CharacterAnimationClip::Idle};
    float upper_body_normalized_time{};
    float upper_body_playback_rate{1.0f};
    float upper_body_weight{};
    std::uint64_t revision{};
};

struct RenderSceneInstanceKey
{
    std::uint64_t stable_id{};
    std::uint32_t fallback_ordinal{};

    [[nodiscard]] bool operator==(const RenderSceneInstanceKey &) const noexcept = default;
};

// Anonymous identities retain their original source order, even when draw order
// changes. Callers validate the instance count before converting the index.
[[nodiscard]] constexpr RenderSceneInstanceKey MakeRenderSceneInstanceKey(
    std::uint64_t stable_id, std::uint32_t source_index) noexcept
{
    return {stable_id, stable_id == 0 ? source_index + 1u : 0u};
}

struct RenderSceneInstanceKeyHash
{
    [[nodiscard]] std::size_t operator()(RenderSceneInstanceKey key) const noexcept;
};

[[nodiscard]] RenderInstance::Revisions
DeriveRenderInstanceRevisions(const RenderInstance &instance) noexcept;
[[nodiscard]] std::uint64_t
DeriveAnimationPoseRevision(const AnimationPoseRef &pose) noexcept;
[[nodiscard]] std::uint64_t
DeriveNamedAnimationPoseRevision(const RenderInstance &instance) noexcept;
void PopulateRenderInstanceIdentity(RenderInstance &instance) noexcept;
void PopulateRenderInstanceRevisions(RenderInstance &instance) noexcept;

enum class LightType : std::uint32_t
{
    Directional, Point, Spot, Rectangle, Disk,
};

enum class LightUnit : std::uint32_t
{
    Lux,       // Directional illuminance.
    Lumen,     // Total Point/Spot luminous flux.
    Luminance, // Rectangle/Disk emitted luminance in cd/m^2, per emitting side.
};

// The renderer's explicit white-equivalent photometric conversion policy.
inline constexpr float kLightWhiteLumensPerWatt = 683.0f;
inline constexpr std::uint32_t kLightShadowRequested = 1u;

struct LightView
{
    Float3 direction{}; // Propagation direction; renderer normalizes it during upload.
    float quantity{};   // Authored photometric quantity in unit.
    Float3 color{};     // Linear RGB multiplier; photometric conversion happens once on upload.
    std::uint64_t stable_id{};
    std::uint32_t generation{1};
    std::uint64_t revision{1};
    LightType type{LightType::Directional};
    LightUnit unit{LightUnit::Lux};
    Float3 position{};  // Absolute world position, meters.
    float range{};     // Finite local influence radius from emitter center, meters.
    Float2 shape{};    // Rectangle full width/height or Disk ellipse radii, meters.
    float emitter_radius{}; // Positive finite Point/Spot radius for inverse-square clamping.
    float inner_cone_radians{}; // Spot half angles: 0 <= inner < outer < pi/2.
    float outer_cone_radians{};
    std::uint32_t shadow_flags{};
    bool legacy_attenuation{}; // Explicit migration: 1/(1+d^2) instead of inverse square.
    Float3 area_tangent{}; // Authored width/radius-X direction; projected orthogonal to direction.
    bool two_sided{}; // Area only. Each side emits the authored luminance, no flux rescaling.
};

struct UiModel
{
    enum class Kind : std::uint8_t
    {
        Text,
        Panel,
        Button,
        Bar,
    };

    Kind kind{Kind::Text};
    Float2 anchor_pixels{32.0f, 32.0f};
    Float2 size_pixels{};
    std::uint32_t color_rgba{0xFFFFFFFFu};
    float value{1.0f};
    std::uint16_t font_pixels{28};
    std::array<char, 512> utf8_text{};
};

struct CameraView
{
    Float3 target{};
    float yaw_degrees{45.0f};
    float pitch_degrees{55.0f};
    float vertical_fov_degrees{45.0f};
    float distance{28.0f};
    // Included in target.y; keep user zoom offset out of dynamic follow interpolation.
    float target_height_offset{};
    Sequence target_motion_epoch{};
};

enum class RenderClockMode : std::uint8_t { Scripted, RealtimeRunning, Frozen };

struct RenderClockStamp
{
    std::uint64_t epoch{};
    RenderClockMode mode{RenderClockMode::Scripted};
    std::chrono::steady_clock::time_point current_boundary{};
};

struct RenderSnapshotHeader
{
    Tick tick{};
    std::chrono::nanoseconds simulation_time{};
    GameplayChecksum checksum{};
    std::uint64_t session_id{};
    RenderClockStamp clock;
    std::uint64_t dynamic_pair_revision{};
};

// Immutable environment data shared by all snapshots for one arena/content layout.
// A snapshot retains its own lease when the producer replaces the cached scene.
struct StaticRenderScene
{
    struct EligibleStaticMember
    {
        std::uint64_t stable_id{}, object_id{};
        std::uint32_t instance_index{};
    };
    std::span<const RenderInstance> instances;
    AssetId scene_asset_id{};
    std::string content_generation_id;
    std::uint32_t layout_abi{};
    std::array<std::byte, 32> eligible_layout_digest{};
    std::span<const EligibleStaticMember> eligible_static_members;
    std::span<const RenderSceneInstanceKey> static_keys;
};

// Mutable preparation only. Bind after the final allocation and all vector writes.
struct StaticRenderSceneStorage final
{
    using EligibleStaticMember = StaticRenderScene::EligibleStaticMember;
    std::vector<RenderInstance> instances;
    AssetId scene_asset_id{};
    std::string content_generation_id;
    std::uint32_t layout_abi{};
    std::array<std::byte, 32> eligible_layout_digest{};
    std::vector<EligibleStaticMember> eligible_static_members;
    std::vector<RenderSceneInstanceKey> static_keys;
    StaticRenderScene exported_view;

    StaticRenderSceneStorage() = default;
    explicit StaticRenderSceneStorage(const StaticRenderScene &view)
        : instances(view.instances.begin(), view.instances.end()), scene_asset_id(view.scene_asset_id),
          content_generation_id(view.content_generation_id), layout_abi(view.layout_abi),
          eligible_layout_digest(view.eligible_layout_digest),
          eligible_static_members(view.eligible_static_members.begin(), view.eligible_static_members.end()),
          static_keys(view.static_keys.begin(), view.static_keys.end()) {}
    void BindExportedView()
    {
        exported_view = {instances, scene_asset_id, content_generation_id, layout_abi,
                         eligible_layout_digest, eligible_static_members, static_keys};
    }
};

// Development/native fixtures use the same stable-address, immutable lease contract.
[[nodiscard]] inline std::shared_ptr<const StaticRenderScene> FreezeStaticRenderScene(
    std::unique_ptr<StaticRenderSceneStorage> candidate)
{
    candidate->static_keys.clear();
    candidate->static_keys.reserve(candidate->instances.size());
    for (std::size_t index = 0; index < candidate->instances.size(); ++index)
        candidate->static_keys.push_back(MakeRenderSceneInstanceKey(
            candidate->instances[index].stable_id, static_cast<std::uint32_t>(index)));
    std::ranges::sort(candidate->static_keys, [](const auto &left, const auto &right) {
        return std::pair{left.stable_id, left.fallback_ordinal} <
               std::pair{right.stable_id, right.fallback_ordinal};
    });
    candidate->BindExportedView();
    const auto *view = &candidate->exported_view;
    std::shared_ptr<const StaticRenderSceneStorage> owner(std::move(candidate));
    return {std::move(owner), view};
}

// Actual projected instance captured before its source actor leaves the scene.
// The snapshot carries provenance only; Runtime owns recipe-dependent retention.
struct CapturedRenderInstance
{
    std::uint64_t session_id{}, source_entity_id{};
    Sequence sequence{};
    Tick tick{};
    RenderInstance instance;
    AnimationPoseRef animation_pose;
};

struct RenderSnapshot
{
    RenderSnapshotHeader header{};
    std::span<const RenderInstance> instances;
    std::span<const AnimationPoseRef> poses;
    std::span<const LightView> lights;
    std::span<const UiModel> ui;
    std::span<const PersistentVfxVisual> persistent_vfx;
    CameraView camera{};
    std::shared_ptr<const StaticRenderScene> static_scene;
    std::span<const CapturedRenderInstance> captured_instances;
};

class RenderSnapshotStorage
{
  public:
    RenderSnapshotStorage(std::size_t instance_capacity, std::size_t pose_capacity,
                          std::size_t light_capacity, std::size_t ui_capacity);

    void Clear() noexcept;
    // Copies a different retained sample; UI belongs only to the current packet.
    void CopyDynamicFrom(const RenderSnapshot &sample);
    [[nodiscard]] bool AddInstance(const RenderInstance &instance);
    [[nodiscard]] bool AddPose(const AnimationPoseRef &pose);
    [[nodiscard]] bool AddCapturedInstance(const CapturedRenderInstance &capture);
    // Complete-packet producers append final operands without deriving a
    // producer revision history. Add* retains the development adapter rules.
    [[nodiscard]] bool AppendInstance(const RenderInstance &instance);
    [[nodiscard]] bool AppendPose(const AnimationPoseRef &pose);
    [[nodiscard]] bool AppendCapturedInstance(const CapturedRenderInstance &capture);
    [[nodiscard]] bool AddLight(const LightView &light);
    [[nodiscard]] bool AddUi(const UiModel &ui);
    [[nodiscard]] bool AddPersistentVfx(const PersistentVfxVisual &visual);
    [[nodiscard]] std::size_t InstanceCount() const noexcept;
    [[nodiscard]] RenderSnapshot View() const noexcept;
    void SetStaticScene(std::shared_ptr<const StaticRenderScene> scene) noexcept;

    RenderSnapshotHeader header{};
    CameraView camera{};

  private:
    std::vector<RenderInstance> instances_;
    std::vector<AnimationPoseRef> poses_;
    std::vector<CapturedRenderInstance> captured_instances_;
    std::vector<LightView> lights_;
    std::vector<UiModel> ui_;
    std::vector<PersistentVfxVisual> persistent_vfx_;
    std::shared_ptr<const StaticRenderScene> static_scene_;
};

struct RenderSnapshotPair
{
    RenderSnapshot previous{}, current{};
    Sequence previous_publication_sequence{}, current_publication_sequence{};
    bool has_previous{}, has_current{};
};

[[nodiscard]] inline CameraView InterpolateRenderCamera(const RenderSnapshotPair &pair, float alpha) noexcept
{
    auto camera = pair.current.camera;
    if (!pair.has_previous || alpha >= 1.0f) return camera;
    const auto &previous = pair.previous.camera;
    if (previous.target_motion_epoch != camera.target_motion_epoch)
    {
        camera.target = {previous.target.x,
            previous.target.y - previous.target_height_offset + camera.target_height_offset,
            previous.target.z};
        return camera;
    }
    camera.target = {std::lerp(previous.target.x, camera.target.x, alpha),
        std::lerp(previous.target.y - previous.target_height_offset,
                  camera.target.y - camera.target_height_offset, alpha) + camera.target_height_offset,
        std::lerp(previous.target.z, camera.target.z, alpha)};
    return camera;
}

[[nodiscard]] inline float RenderSnapshotInterpolation(const RenderSnapshotPair &pair,
    std::chrono::steady_clock::time_point now, bool enabled) noexcept
{
    const auto &current = pair.current.header;
    const auto &previous = pair.previous.header;
    if (!enabled || !pair.has_previous || current.session_id != previous.session_id ||
        current.clock.mode != RenderClockMode::RealtimeRunning ||
        current.tick <= previous.tick ||
        current.tick - previous.tick != 1 || current.simulation_time <= previous.simulation_time)
        return 1.0f;
    const auto step = std::chrono::duration<float>(current.simulation_time - previous.simulation_time).count();
    return std::clamp(std::chrono::duration<float>(now - current.clock.current_boundary).count() / step,
                      0.0f, 1.0f);
}

} // namespace hs
