#pragma once

#include <hs/core/particle_spawn_command.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/core/snapshot_exchange.hpp>
#include <hs/core/types.hpp>

#include <cstdint>
#include <array>
#include <filesystem>
#include <memory>
#include <span>

namespace hs
{

inline constexpr std::size_t kStage1RenderPassCount = 11;

struct RendererConfig
{
    void *window{};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    bool warp{};
    bool validation{};
    bool gpu_validation{};
    bool vsync{true};
    bool bloom{true};
    bool outline{true};
    bool interpolate{true};
    std::uint32_t render_scale_percent{100};
    std::uint32_t shadow_resolution{1024};
    std::uint32_t particle_percentage{100};
    BarrierMode barrier_mode{BarrierMode::Automatic};
    std::filesystem::path artifact_directory;
};

struct RendererFrameResult
{
    std::uint64_t frame{};
    Tick rendered_tick{};
};

struct GpuPassTimings
{
    std::array<std::uint64_t, kStage1RenderPassCount> nanoseconds{};
    bool valid{};
};

class D3D12Renderer
{
  public:
    D3D12Renderer();
    ~D3D12Renderer();

    D3D12Renderer(const D3D12Renderer &) = delete;
    D3D12Renderer &operator=(const D3D12Renderer &) = delete;

    [[nodiscard]] Result Initialize(const RendererConfig &config);
    [[nodiscard]] Result Resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] Result Render(const RenderSnapshotExchange::ReadPair &snapshots,
                                std::span<const PresentationEvent> events,
                                std::span<const ParticleSpawnCommand> particle_spawns,
                                RendererFrameResult &frame_result);
    [[nodiscard]] Result CapturePng(const std::filesystem::path &path);
    [[nodiscard]] Result Shutdown();
    [[nodiscard]] bool UsesEnhancedBarriers() const noexcept;
    [[nodiscard]] std::uint64_t ValidationErrorCount() const noexcept;
    [[nodiscard]] std::uint32_t LastParticleCount() const noexcept;
    [[nodiscard]] GpuPassTimings LastGpuPassTimings() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
