#pragma once

#include <hs/core/cooked_format.hpp>
#include <hs/core/types.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
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
};

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
};

struct LightView
{
    Float3 direction{};
    float intensity{};
    Float3 color{};
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
};

struct RenderSnapshotHeader
{
    Tick tick{};
    std::chrono::nanoseconds simulation_time{};
    GameplayChecksum checksum{};
    std::uint64_t session_id{};
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
};

class RenderSnapshotStorage
{
  public:
    RenderSnapshotStorage(std::size_t instance_capacity, std::size_t pose_capacity,
                          std::size_t light_capacity, std::size_t ui_capacity);

    void Clear() noexcept;
    [[nodiscard]] bool AddInstance(const RenderInstance &instance);
    [[nodiscard]] bool AddPose(const AnimationPoseRef &pose);
    [[nodiscard]] bool AddLight(const LightView &light);
    [[nodiscard]] bool AddUi(const UiModel &ui);
    [[nodiscard]] bool AddPersistentVfx(const PersistentVfxVisual &visual);
    [[nodiscard]] std::size_t InstanceCount() const noexcept;
    [[nodiscard]] RenderSnapshot View() const noexcept;

    RenderSnapshotHeader header{};
    CameraView camera{};

  private:
    std::vector<RenderInstance> instances_;
    std::vector<AnimationPoseRef> poses_;
    std::vector<LightView> lights_;
    std::vector<UiModel> ui_;
    std::vector<PersistentVfxVisual> persistent_vfx_;
};

} // namespace hs
