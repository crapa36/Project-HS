#pragma once
#include <hs/renderer/vfx_light_input.hpp>
#include <hs/renderer/vfx_distortion_input.hpp>
#include <hs/renderer/vfx_decal_input.hpp>
#include <hs/renderer/vfx_owned_mesh.hpp>
#include <hs/renderer/vfx_fresnel_input.hpp>
#include <hs/renderer/vfx_ribbon_input.hpp>

#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/core/snapshot_exchange.hpp>
#include <hs/core/types.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_spawn_command.hpp>

#include <cstdint>
#include <array>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace hs
{

inline constexpr std::size_t kRenderPassCount = 16;
inline constexpr std::array<std::string_view, kRenderPassCount> kRenderPassNames{
    "GPU Particle Spawn/Update", "3-cascade Directional Shadow", "GBuffer+Depth",
    "Deferred Cel Lighting", "VFX Distortion Vectors", "Forward Transparent/OIT", "OIT Composite",
    "Temporal Resolve", "Bloom Half Extract", "Bloom Quarter Downsample", "Bloom Half Upsample", "Bloom",
    "ToneMap", "Screen-space Outline", "FXAA", "Game UI"};
static_assert(kRenderPassNames.size() == kRenderPassCount);

struct ParticleSpriteBinding
{
    ParticleSprite sprite{};
    std::uint8_t frame_columns{1};
    std::uint8_t frame_rows{1};
};

struct VfxGroundSpawnInput
{
    ParticleSpawnCommand command;
    bool additive{};
    std::uint64_t stable_id{};
    VfxEffectHandle effect_handle{kInvalidVfxEffectHandle};
    std::uint8_t source_visual_kind{0xff};
    VfxGroundMotion motion;
    std::uint32_t gradient_row{};
    float hdr{};
    VfxGroundGeometry geometry;
};

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
    bool character_preview{};
    std::uint32_t monster_preview_asset{0xFFFFFFFFu};
    std::uint32_t slime_family_preview_count{};
    bool environment_preview{};
    std::uint32_t monster_preview_clip{};
    float monster_preview_time{0.5f};
    bool preview_camera_override{};
    float preview_camera_yaw{180.0f};
    float preview_camera_pitch{10.0f};
    float preview_camera_distance{8.0f};
    bool devtools_visible{true};
    std::uint32_t render_scale_percent{100};
    std::uint32_t shadow_resolution{1024};
    std::uint32_t particle_percentage{100};
    std::uint32_t ribbon_history_points{65536};
    float distortion_coverage{1.0f}; // Projected screen-area budget, 0..1; medium uses half.
    std::uint32_t vfx_light_count{8}; // Runtime budget, 0..32; medium uses half and low uses none.
    VfxQuality vfx_quality{VfxQuality::High};
    ParticleSpriteBinding bleed_status_sprite{};
    ParticleSpriteBinding burn_status_sprite{};
    ParticleSpriteBinding slow_status_sprite{};
    ParticleSpriteBinding mark_status_sprite{};
    ParticleSpriteBinding trap_sprite{};
    ParticleSpriteBinding fire_area_sprite{};
    BarrierMode barrier_mode{BarrierMode::Automatic};
    std::filesystem::path artifact_directory;
};

struct RendererFrameResult
{
    std::uint64_t frame{};
    Tick rendered_tick{};
    bool capture_mouse{};
    bool capture_keyboard{};
    std::uint8_t debug_command{};
    std::uint64_t debug_value{};
    std::uint32_t debug_secondary{};
    std::uint32_t typed_vfx_event_count{};
    std::uint32_t typed_vfx_persistent_count{};
    std::uint32_t typed_vfx_ground_count{};
    std::uint32_t typed_vfx_flash_count{};
    std::uint32_t typed_vfx_mesh_count{};
    std::uint32_t typed_vfx_fresnel_count{}; // Actual boss mesh draws.
    std::uint32_t typed_vfx_ribbon_count{};
    std::uint32_t typed_vfx_ribbon_dropped{};
    std::uint32_t typed_vfx_light_count{};
    std::uint32_t typed_vfx_distortion_count{};
    std::uint32_t typed_vfx_decal_count{};
};

struct RendererOptions
{
    bool vsync{true};
    bool bloom{true};
    bool outline{true};
    std::uint32_t render_scale_percent{100};
    std::uint32_t shadow_resolution{1024};
    std::uint32_t particle_percentage{100};
    VfxQuality vfx_quality{VfxQuality::High};
};

struct DevToolsFrameData
{
    std::uint32_t worker_count{};
    std::uint32_t input_queue_depth{};
    std::uint32_t presentation_queue_depth{};
    std::uint32_t particle_queue_depth{};
    std::uint32_t resize_queue_depth{};
    std::uint32_t debug_queue_depth{};
    std::uint64_t dropped_input{};
    std::uint64_t dropped_presentation{};
    std::uint64_t dropped_particles{};
};

struct NativeWindowMessage
{
    std::uint32_t message{};
    std::uintptr_t wparam{};
    std::intptr_t lparam{};
};

struct GpuPassTimings
{
    std::array<std::uint64_t, kRenderPassCount> nanoseconds{};
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
    [[nodiscard]] Result ApplyOptions(const RendererOptions &options);
    void HandleWindowMessage(const NativeWindowMessage &message) noexcept;
    [[nodiscard]] Result Render(const RenderSnapshotExchange::ReadPair &snapshots,
                                std::span<const PresentationEvent> events,
                                std::span<const ParticleSpawnCommand> particle_spawns,
                                std::span<const VfxLineSpawnCommand> effect_lines,
                                std::span<const VfxEventInput> typed_vfx_events,
                                std::span<const VfxPersistentInput> typed_vfx_persistent,
                                std::span<const VfxGroundSpawnInput> typed_ground_spawns,
                                std::span<const VfxFlashSpawnInput> typed_flash_spawns,
                                std::span<const VfxOwnedMeshInput> typed_mesh_spawns,
                                std::span<const VfxFresnelInput> typed_fresnels,
                                const VfxRibbonFrameInput &typed_ribbons,
                                std::span<const VfxLightInput> typed_lights,
                                std::span<const VfxDistortionInput> typed_distortions,
                                std::span<const VfxDecalInput> typed_decals,
                                const DevToolsFrameData &devtools,
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
