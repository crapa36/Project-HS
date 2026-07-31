#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hs
{

enum class RenderMesh : std::uint8_t
{
    Archer,
    Enemy,
};

struct RenderInstance
{
    Float3 position{};
    float yaw{};
    Float3 scale{1.0f, 1.0f, 1.0f};
    std::uint32_t color_rgba{0xFFFFFFFFu};
    RenderMesh mesh{};
};

struct AnimationPoseRef
{
    std::uint32_t instance_index{};
    float normalized_time{};
};

struct LightView
{
    Float3 direction{};
    float intensity{};
    Float3 color{};
};

struct UiModel
{
    std::array<char, 96> utf8_text{};
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
    [[nodiscard]] RenderSnapshot View() const noexcept;

    RenderSnapshotHeader header{};
    CameraView camera{};

  private:
    std::vector<RenderInstance> instances_;
    std::vector<AnimationPoseRef> poses_;
    std::vector<LightView> lights_;
    std::vector<UiModel> ui_;
};

} // namespace hs
