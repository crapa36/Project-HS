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
};

struct PersistentVfxVisual
{
    Float3 position{};
    float yaw{};
    float radius{};
    float length{};
    PersistentVfxKind kind{};
    std::uint64_t stable_id{};
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
