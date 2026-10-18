#pragma once

#include <hs/core/render_view_input.hpp>
#include <hs/core/frame_identity.hpp>
#include <hs/core/graphics_settings_request.hpp>
#include <hs/renderer/capture_request.hpp>
#include <hs/renderer/vfx_light_input.hpp>
#include <hs/renderer/vfx_distortion_input.hpp>
#include <hs/renderer/vfx_decal_input.hpp>
#include <hs/renderer/vfx_owned_mesh.hpp>
#include <hs/renderer/vfx_fresnel_input.hpp>
#include <hs/renderer/vfx_ribbon_input.hpp>

#include <hs/core/presentation_event.hpp>
#include <hs/core/graphics_settings.hpp>
#include <hs/core/result.hpp>
#include <hs/core/snapshot_exchange.hpp>
#include <hs/core/types.hpp>
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_volume_input.hpp>
#include <hs/renderer/vfx_screen_input.hpp>
#include <hs/renderer/vfx_entity_surface_input.hpp>
#include <hs/renderer/vfx_spawn_command.hpp>

#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hs
{
struct RenderFrameInput;
struct ViewId;


enum class RenderQueue : std::uint8_t { Direct, Compute, Copy };
enum class GeometryFrontend : std::uint8_t { Automatic, Indexed, MeshShader };
enum class SurfaceFrontend : std::uint8_t { CompactDeferred, VisibilityBuffer };

struct RendererDisplayOutput
{
    std::string id;
    std::int32_t left{}, top{};
    std::uint32_t width{}, height{};
    bool hdr_active{};
    float peak_nits{};
};

// Native output IDs match the display query: dxgi:<adapter LUID high/low as
// 16 hex digits>:<UTF-8 DXGI DeviceName>. No OS display/HDR mode is changed.
[[nodiscard]] Result EnumerateRendererDisplays(std::vector<RendererDisplayOutput> &outputs);

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

// Runtime owns worker scheduling. Each submission group is joined before graph
// submission or return, so frame captures and snapshot leases stay valid.
struct RenderTaskExecutor
{
    std::function<Result(std::function<void()>)> submit;
    std::function<Result()> join;
};

struct RendererReconstructionRequest
{
    GraphicsProviderId provider{"auto"};
    GraphicsModeId quality{"quality"};
    GraphicsResolutionMode resolution_mode{GraphicsResolutionMode::ProviderQuality};
    float manual_scale_percent{100.0f};
    friend bool operator==(const RendererReconstructionRequest &, const RendererReconstructionRequest &) = default;
};

struct RendererReconstructionStatus
{
    GraphicsProviderId requested_provider, effective_provider;
    GraphicsModeId requested_quality, effective_quality;
    GraphicsExtent render_extent, output_extent;
    std::string requested_availability, fallback_reason;
    bool temporal_enabled{};
};

struct RendererPresentationRequest
{
    GraphicsProviderId provider{"auto"};
    GraphicsFrameGenerationMode mode{GraphicsFrameGenerationMode::Off};
    std::uint32_t multiplier{2};
    GraphicsAutoToggle low_latency{GraphicsAutoToggle::Auto};
    std::uint32_t frame_limit_fps{};
    friend bool operator==(const RendererPresentationRequest &, const RendererPresentationRequest &) = default;
};

struct RendererPresentationStatus
{
    RendererPresentationRequest requested;
    GraphicsProviderId effective_provider{"off"};
    GraphicsFrameGenerationMode active_mode{GraphicsFrameGenerationMode::Off};
    std::uint32_t maximum_multiplier{}, observed_multiplier{};
    std::string availability, fallback_reason;
    bool low_latency_active{};
    std::uint64_t allocated_bytes{};
};

// Streamline's actual-present count since the preceding native GetState query.
// This is SDK-reported presentation throughput, not physical display cadence.
// FSR generated frames and XeSS queued frames are not interchangeable with it.
struct RendererStreamlinePresentCount
{
    std::uint64_t begin_qpc{}, end_qpc{}, qpc_frequency{};
    std::uint32_t frames{};
    bool valid{};
};

struct RendererLightingModes
{
    GraphicsAoMode ao_mode{GraphicsAoMode::Off};
    GraphicsGiMode gi_mode{GraphicsGiMode::Off};
    GraphicsReflectionMode reflection_mode{GraphicsReflectionMode::Off};
    GraphicsShadowMode shadow_mode{GraphicsShadowMode::Raster};
};
struct RendererLocalShadowStatus
{
    bool enabled{};
    std::uint32_t requested_lights{}, selected_lights{}, unsupported_lights{}, budget_lights{}, overflow_lights{};
    std::uint64_t allocated_bytes{};
    std::string reason;
};
struct RendererCharacterAnimationClip
{
    AssetId id;
    std::string name;
    float duration_seconds{};
    std::uint32_t frame_count{};
    bool looping{};
};
struct RendererLightingStatus
{
    RendererLightingModes requested, active;
    std::string ao_fallback_reason, gi_fallback_reason, reflection_fallback_reason, shadow_fallback_reason;
    RendererLocalShadowStatus local_shadows;
    struct SoftwareGiStatus
    {
        bool compiled{}, supported{}, enabled{}, native_ready{}, faulted{};
        std::uint32_t static_instances{}, dynamic_instances{}, unsupported_masked{}, unsupported_thin{}, excluded_translucent{};
        std::uint32_t physical_cascades{};
        std::uint64_t allocated_bytes{}, scratch_bytes{};
        std::string reason;
    } software_gi;
    struct AdaptiveStatus
    {
        GraphicsAutoToggle requested{GraphicsAutoToggle::Auto};
        bool raster_resolved{}, compute_resolved{}, raster_ready{}, compute_ready{};
        bool raster_applied{}, compute_applied{}, usable{true};
        std::uint32_t vrs_tier{}, vrs_tile_size{};
        std::uint64_t allocated_bytes{};
        std::string raster_reason, compute_reason;
    } adaptive_shading;
    struct ShadowCacheStatus
    {
        std::uint64_t requested_bytes{}, admitted_bytes{};
        std::uint32_t physical_pages{};
    } shadow_cache;
};

struct RendererTextureStreamingStatus
{
    GraphicsQuality applied_quality{GraphicsQuality::Medium};
    std::uint32_t applied_budget_mib{}; // Zero uses actual DXGI/shared-budget headroom.
    bool ready{}, native_asset_io{}, sampler_feedback{};
    std::uint32_t texture_count{};
    std::uint64_t physical_bytes{};
    std::string reason;
};

struct RendererTextureProductStatus
{
    bool registered{}, pending{}, publication_complete{};
    std::uint64_t requests{}, uploads{}, publications{}, reclaims{}, revision{};
    std::uint32_t actual_resident_mip{}, effective_desired_mip{}, target_mip{};
};

struct RendererPsoCacheStats
{
    std::uint64_t hits{}, misses{}, native_creates{}, stores{}, store_failures{}, load_failures{};
    std::uint64_t io_failures{}, discarded_files{}, unsupported_libraries{}, uncached_mesh_creates{};
    std::uint64_t root_registrations{}, key_failures{}, loaded_bytes{}, serialized_bytes{};
    bool initialized{}, library_supported{}, mesh_library_supported{}, persistence_enabled{};
    std::int32_t last_cache_hresult{}, last_io_error{};
};

struct RendererVfxSpriteBindings
{
    ParticleSpriteBinding bleed_status_sprite, burn_status_sprite, slow_status_sprite,
        mark_status_sprite, trap_sprite, fire_area_sprite;
};

struct RendererConfig
{
    void *window{};
    std::uint32_t width{1280};
    std::uint32_t height{720};
    bool warp{};
    bool validation{};
    bool gpu_validation{};
    // Capture-only completed timing stream; ordinary rendering retains only the latest sample.
    bool capture_timing_stream{};
    bool vsync{true};
    bool bloom{true};
    bool outline{true};
    bool interpolate{true};
    bool character_preview{};
    std::uint32_t monster_preview_asset{0xFFFFFFFFu};
    std::uint32_t slime_family_preview_count{};
    bool environment_preview{};
    bool grid_floor_probe{};
    bool grid_floor_disable_aa{};
    std::uint32_t monster_preview_clip{};
    float monster_preview_time{0.5f};
    bool preview_camera_override{};
    float preview_camera_yaw{180.0f};
    float preview_camera_pitch{10.0f};
    float preview_camera_distance{8.0f};
    bool devtools_visible{true};
    std::uint32_t render_scale_percent{100};
    bool dynamic_resolution{};
    std::uint32_t target_render_fps{60};
    float dynamic_min_percent{50.0f};
    float dynamic_max_percent{100.0f};
    std::uint32_t shadow_resolution{1024};
    std::uint32_t particle_percentage{100};
    std::uint32_t ribbon_history_points{65536};
    float distortion_coverage{1.0f}; // Projected screen-area budget, 0..1; medium uses half.
    float screen_overlay_intensity{1.0f};
    std::uint32_t vfx_light_count{8}; // Runtime budget, 0..32; medium uses half and low uses none.
    VfxQuality vfx_quality{VfxQuality::High};
    std::uint32_t anisotropic_filtering{16};
    float bloom_strength{1.0f};
    float outline_width_px{1.0f};
    bool color_grading{true};
    GraphicsMotionBlur motion_blur{GraphicsMotionBlur::Off};
    bool depth_of_field{};
    float shadow_distance_m{160.0f};
    GraphicsQuality texture_quality{GraphicsQuality::Medium};
    std::uint32_t texture_budget_mib{}; // Zero uses the observed global/OS budget.
    GraphicsQuality geometry_quality{GraphicsQuality::Medium};
    GraphicsQuality foliage_quality{GraphicsQuality::Medium};
    GraphicsQuality material_quality{GraphicsQuality::Medium};
    GraphicsOptionalQuality pom_quality{GraphicsOptionalQuality::Medium};
    GraphicsQuality lighting_quality{GraphicsQuality::Medium};
    GraphicsAutoToggle adaptive_shading{GraphicsAutoToggle::Auto};
    GraphicsAoMode ao_mode{GraphicsAoMode::GTAO};
    GraphicsQuality ao_quality{GraphicsQuality::Medium};
    bool contact_shadows{true};
    GraphicsReflectionMode reflection_mode{GraphicsReflectionMode::ScreenSpace};
    GraphicsQuality reflection_quality{GraphicsQuality::Medium};
    GraphicsQuality transparency_quality{GraphicsQuality::Medium};
    GraphicsOptionalQuality refraction_quality{GraphicsOptionalQuality::Medium};
    GraphicsOptionalQuality volumetric_quality{GraphicsOptionalQuality::Medium};
    GraphicsGiMode gi_mode{GraphicsGiMode::Probe};
    GraphicsQuality gi_quality{GraphicsQuality::Medium};
    GraphicsShadowMode shadow_mode{GraphicsShadowMode::Raster};
    GraphicsQuality shadow_quality{GraphicsQuality::Medium};
    bool temporal_reconstruction{true};
    std::optional<RendererReconstructionRequest> reconstruction_request;
    RendererPresentationRequest presentation_request;
    // Composition may suppress FG outside gameplay while retaining the requested setting.
    bool frame_generation_allowed{true};
    float sharpening{0.2f};
    GraphicsHdrMode hdr_mode{GraphicsHdrMode::Auto};
    float hdr_paper_white_nits{200.0f};
    GraphicsAutoFloat hdr_peak_nits;
    GraphicsDisplayId display_output{"auto"};
    // Optional canonical startup request. When present, Initialize projects
    // this full snapshot before native device and pipeline preparation.
    std::optional<GraphicsSettings> requested_graphics;
    ParticleSpriteBinding bleed_status_sprite{};
    ParticleSpriteBinding burn_status_sprite{};
    ParticleSpriteBinding slow_status_sprite{};
    ParticleSpriteBinding mark_status_sprite{};
    ParticleSpriteBinding trap_sprite{};
    ParticleSpriteBinding fire_area_sprite{};
    BarrierMode barrier_mode{BarrierMode::Automatic};
    GeometryFrontend geometry_frontend{GeometryFrontend::Automatic};
    SurfaceFrontend surface_frontend{SurfaceFrontend::CompactDeferred};
    std::filesystem::path artifact_directory;
    std::string content_generation_id;
    std::shared_ptr<const void> content_generation_lease;
    std::string scene_hlod_product_id{"render/hlod/arena.shl1"};
    std::unordered_map<std::string, std::filesystem::path> product_paths;
    std::unordered_map<std::string, std::string> product_digests;
    RenderTaskExecutor task_executor;

    [[nodiscard]] Result ResolveProduct(std::string_view logical_id,
                                        std::filesystem::path &path) const
    {
        const auto found = product_paths.find(std::string(logical_id));
        if (found == product_paths.end())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Missing Generation product: " + std::string(logical_id));
        path = found->second;
        return Result::Success();
    }
};

struct RenderViewFrameResult
{
    FrameIdentity identity;
    std::uint64_t submission_id{}, rendered_frame{}, history_generation{};
    RenderViewInput rendered_view;
    std::array<float, 16> unjittered_view_projection{};
};

// Sequential wall-clock stages in microseconds, not CPU busy time. Per-view stages
// exclude wrapper_us, which is package overhead outside all view Render calls.
struct RendererCpuTimings
{
    std::uint64_t wait_us{}, streaming_us{}, scene_prepare_us{}, ui_us{};
    std::uint64_t constants_upload_us{}, graph_build_us{}, graph_prepare_us{}, descriptors_us{};
    std::uint64_t graph_plan_us{}, graph_record_us{}, graph_submit_us{};
    std::uint64_t post_submit_us{}, present_us{}, diagnostics_us{}, wrapper_us{};
    // Capture-only QPC interval around frame-slot completion admission.
    std::uint64_t frame_slot_wait_begin_qpc{}, frame_slot_wait_end_qpc{};
    // Capture-only nested Prepare scopes are included in graph_prepare_us, with cleanup left as residual.
    // Do not add these values to the outer stage totals.
    std::uint64_t graph_prepare_validation_us{}, graph_prepare_info_key_us{};
    std::uint64_t graph_prepare_compile_us{}, graph_prepare_transient_us{};
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
    std::uint64_t static_scene_upload_count{};
    std::uint32_t typed_vfx_volume_count{};
    FrameIdentity identity;
    RenderViewInput rendered_view;
    std::uint64_t view_history_generation{};
    std::array<float, 16> unjittered_view_projection{};
    std::vector<RenderViewFrameResult> views;
    RendererCpuTimings cpu_timings;
    RendererStreamlinePresentCount streamline_present;
};

struct RendererOptions;

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

struct GpuPassTiming
{
    std::uint32_t pass_id{};
    std::string name;
    RenderQueue queue{};
    std::uint64_t nanoseconds{};
    bool measured{};
    // Raw queue-domain timestamps are retained only after frame completion.
    std::uint64_t begin_ticks{}, end_ticks{}, timestamp_frequency{};
    std::uint64_t begin_qpc{}, end_qpc{}, qpc_frequency{};
    bool clock_calibrated{};
    // Capture-only host wall intervals; parallel recording intervals may overlap.
    // Recording includes command-list reset/barriers/close; provider submission is its callback only.
    std::uint64_t cpu_record_begin_qpc{}, cpu_record_end_qpc{};
    std::uint64_t cpu_provider_submit_begin_qpc{}, cpu_provider_submit_end_qpc{};
    bool cpu_record_valid{}, cpu_provider_submit_valid{};
};

struct GpuPassTimings
{
    std::vector<GpuPassTiming> passes;
    bool valid{};
    std::uint64_t completed_frame_id{};
    std::uint32_t view_id{};
    double critical_path_ms{};
    bool critical_path_valid{};
    double cpu_recording_ms{};
    bool cpu_recording_valid{};
    bool gpu_limited{};
    bool fence_completed{};
    std::uint64_t cpu_qpc_frequency{};
};

struct RendererQueueStats
{
    std::uint32_t direct_batches{};
    std::uint32_t compute_batches{};
    std::uint32_t copy_batches{};
    std::uint32_t cross_queue_waits{};
    std::uint32_t record_jobs{};
    std::uint32_t transient_resource_count{}, transient_block_count{}, alias_activation_count{};
    std::uint64_t transient_bytes_without_aliasing{}, transient_backing_bytes{};
};

struct RendererResidencyStats
{
    bool budget_valid{};
    std::uint64_t local_budget_bytes{};
    std::uint64_t local_usage_bytes{};
    std::uint64_t tracked_live_bytes{};
    std::uint64_t tracked_retiring_bytes{};
    std::uint64_t tracked_reclaimed_bytes{};
    std::uint64_t streaming_requests{};
    std::uint64_t streaming_uploads{};
    std::uint64_t streaming_publications{};
    std::uint64_t streaming_reclaims{};
    std::uint64_t streaming_generation{};
    std::uint8_t streaming_state{};
    std::uint64_t geometry_streaming_requests{};
    std::uint64_t geometry_streaming_uploads{};
    std::uint64_t geometry_streaming_publications{};
    std::uint64_t geometry_streaming_reclaims{};
    std::uint64_t geometry_streaming_generation{};
    std::uint8_t geometry_streaming_state{};
};

inline constexpr std::size_t kRendererDiagnosticsFrameCapacity = 240;
inline constexpr std::size_t kRendererDiagnosticsPassCapacity = 512;
// Matches the renderer's tracked categories; these are partial accounting,
// independently reported from the process-wide DXGI usage/budget observation.
enum class RendererDiagnosticsMemoryCategory : std::uint8_t
{
    Texture, Geometry, Skin, Shadow, Gi, History, Transient, Sdk, Staging, Count
};
struct RendererDiagnosticsMemory
{
    bool budget_available{}, nonlocal_budget_available{}, host_budget_available{};
    std::uint64_t local_usage_bytes{}, local_budget_bytes{}, nonlocal_usage_bytes{}, nonlocal_budget_bytes{};
    std::array<std::uint64_t, static_cast<std::size_t>(RendererDiagnosticsMemoryCategory::Count)> live_bytes{}, retiring_bytes{};
    std::uint64_t tracked_live_bytes{}, tracked_retiring_bytes{}, tracked_reclaimed_bytes{}, tracked_gpu_bytes{};
    std::uint64_t host_staging_bytes{}, host_reserved_bytes{}, gpu_reserved_bytes{}, gpu_growth_since_budget_bytes{};
    std::uint64_t host_staging_budget_bytes{}, texture_physical_bytes{};
    // Known provider allocations and renderer-owned guide resources are separate;
    // opaque native SR/SDK allocations are not claimed to be fully accounted.
    std::uint64_t presentation_provider_bytes{}, reconstruction_guide_bytes{};
    std::uint64_t renderer_view_bytes{}, shared_deformation_bytes{};
};
struct RendererDiagnosticsFrame
{
    std::uint64_t rendered_frame_id{}, submit_qpc{}, present_begin_qpc{}, present_end_qpc{}, qpc_frequency{};
    std::uint32_t view_id{};
    std::uint64_t view_render_sequence{};
    bool presented{};
    bool fence_completed{}, cpu_recording_available{}, gpu_critical_path_available{};
    bool validation_enabled{}, gpu_validation_enabled{};
    double cpu_recording_ms{}, gpu_critical_path_ms{};
    RendererQueueStats queues;
    RendererDiagnosticsMemory memory;
};
struct RendererDiagnosticsPercentiles
{
    bool available{};
    std::uint32_t samples{};
    double p50{}, p95{}, p99{};
};
struct RendererDiagnosticsCadence
{
    bool available{};
    std::uint32_t frames{};
    double fps{}, elapsed_ms{};
    RendererDiagnosticsPercentiles pacing_ms;
    std::string source, unavailable_reason;
};
struct RendererDiagnosticsPass
{
    std::uint32_t pass_id{};
    std::string name;
    RenderQueue queue{};
    bool measured{}, clock_calibrated{}, name_truncated{};
    std::uint64_t nanoseconds{}, begin_ticks{}, end_ticks{}, timestamp_frequency{};
    // Aligned passes use CPU QPC and a shared origin. Otherwise offsets are
    // relative to the first timestamp in this queue, with no cross-queue claim.
    double begin_ms{}, end_ms{};
    std::string clock_domain;
};
struct RendererDiagnosticsMeasurementSet
{
    RendererDiagnosticsPercentiles cpu_render_ms, gpu_critical_path_ms, present_call_ms, submit_to_present_return_ms;
    RendererDiagnosticsCadence rendered_cadence, present_call_cadence;
};
struct RendererDiagnosticsViewMeasurements
{
    std::uint32_t view_id{};
    bool presenting{};
    RendererDiagnosticsMeasurementSet validation_on, validation_off;
};
struct RendererDiagnosticsSnapshot
{
    std::uint32_t frame_capacity{kRendererDiagnosticsFrameCapacity}, pass_capacity{kRendererDiagnosticsPassCapacity};
    std::uint64_t total_rendered_frames{}, total_completed_frames{}, duplicate_samples{}, rejected_samples{};
    std::uint64_t timeline_completed_frame_id{};
    std::uint32_t timeline_view_id{};
    bool timeline_truncated{};
    std::vector<RendererDiagnosticsFrame> completed_frames;
    std::vector<RendererDiagnosticsPass> passes;
    RendererDiagnosticsMeasurementSet validation_on, validation_off;
    std::vector<RendererDiagnosticsViewMeasurements> views;
    RendererDiagnosticsCadence displayed_cadence;
    std::string cpu_render_source{"renderer_recording_and_graph_submission"};
    std::string gpu_critical_path_source{"direct_queue_graph_bracketing_timestamps"};
    std::string latency_source{"cpu_qpc_graph_submit_to_present_return"};
    bool input_to_display_latency_available{};
    std::string input_to_display_latency_reason{"No completed native input-to-display latency report is exposed."};
    RendererDiagnosticsMemory current_memory, sampled_memory_peak, transition_memory_peak;
    bool transition_memory_peak_available{};
    std::string transition_memory_peak_source, transition_memory_peak_reason;
    std::string memory_source{"dxgi_process_budget_and_partial_renderer_residency_accounting"};
    bool complete_provider_memory_available{}, stream_misses_available{};
    std::string provider_memory_reason{"Only reported presentation allocations and reconstruction guides are known."};
    std::string stream_misses_reason{"Demand miss counters are not exposed; request/publication counters are reported separately."};
    RendererResidencyStats streaming;
    RendererPresentationStatus presentation;
    RendererReconstructionStatus reconstruction;
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
    [[nodiscard]] Result Resize(ViewExtent extent) { return Resize(extent.width, extent.height); }
    [[nodiscard]] Result RequestGraphicsSettings(const GraphicsSettingsRequest &);
    [[nodiscard]] SettingsApplyResult GetGraphicsSettingsStatus() const;
    [[nodiscard]] GraphicsCapabilitiesView GetGraphicsCapabilities() const;
    // Render-thread transaction. The caller validates payload digests and keeps
    // both Generation reader leases alive until this operation returns.
    // Reflected parameter defaults may change with an identical layout; such
    // adoption publishes a new GPU table without replacing any PSO.
    [[nodiscard]] Result ReloadShaderGeneration(
        const std::string &generation_id,
        const std::unordered_map<std::string, std::filesystem::path> &product_paths,
        const std::unordered_map<std::string, std::string> &product_digests);
    // Caller validates the selected immutable manifest/products. Preparation
    // keeps its actual reader lease; publication occurs after all queue uses drain.
    // resident_replaced is false on failure/no-op/narrow and true after a full commit;
    // allow_resident_replacement=false rejects only the full path before preparation.
    [[nodiscard]] Result ReloadContentGeneration(
        const std::string &generation_id,
        const std::unordered_map<std::string, std::filesystem::path> &product_paths,
        const std::unordered_map<std::string, std::string> &product_digests,
        std::shared_ptr<const void> reader_lease,
        const RendererVfxSpriteBindings *sprite_bindings = nullptr,
        bool *resident_replaced = nullptr,
        bool allow_resident_replacement = true);
    [[nodiscard]] std::uint64_t MaterialParameterRevision() const noexcept;
    [[nodiscard]] std::uint64_t PipelineRevision() const noexcept;
    void HandleWindowMessage(const NativeWindowMessage &message) noexcept;
    [[nodiscard]] RenderViewInput MainViewInput(const CameraView &) const;
    [[nodiscard]] Result Render(const RenderFrameInput &, RendererFrameResult &);
    [[nodiscard]] Result Capture(const CaptureRequest &);
    [[nodiscard]] Result CaptureViewPng(ViewId, const std::filesystem::path &);
    [[nodiscard]] Result ReleaseView(ViewId);

    [[nodiscard]] Result CapturePng(const std::filesystem::path &path);
    [[nodiscard]] Result CaptureSurfaceDiagnostics(const std::filesystem::path &path);
    [[nodiscard]] Result CaptureGeometryDiagnostics(const std::filesystem::path &path);
    [[nodiscard]] Result CaptureVisibilityDiagnostics(const std::filesystem::path &path);
    [[nodiscard]] Result Shutdown();
    [[nodiscard]] bool UsesEnhancedBarriers() const noexcept;
    [[nodiscard]] std::uint64_t ValidationErrorCount() const noexcept;
    [[nodiscard]] std::uint32_t LastParticleCount() const noexcept;
    [[nodiscard]] float DecorativeParticleFraction() const noexcept;
    [[nodiscard]] GpuPassTimings LastGpuPassTimings() const;
    // Owner thread only; cached data remains valid until the next renderer mutation.
    // No query readback, clock calibration, allocation or copy occurs here.
    [[nodiscard]] const GpuPassTimings &LastCompletedGpuPassTimings() const noexcept;
    // Owner-thread capture stream. Consume performs no native query or GPU wait.
    // Drain waits existing work and collects every remaining timestamp-bearing frame slot.
    // Both replace output, sort by completed frame/view, and explicitly fail on bounded-stream overflow.
    [[nodiscard]] Result ConsumeCompletedGpuPassTimings(std::vector<GpuPassTimings> &output);
    [[nodiscard]] Result DrainGpuPassTimings(std::vector<GpuPassTimings> &output);
    // Cached latest rendered view; reference remains valid until the next renderer mutation.
    [[nodiscard]] const RendererDiagnosticsFrame &LastRenderedFrameDiagnostics() const noexcept;
    [[nodiscard]] RendererQueueStats LastQueueStats() const noexcept;
    [[nodiscard]] RendererResidencyStats ResidencyStats() const noexcept;
    [[nodiscard]] RendererReconstructionStatus GetReconstructionStatus() const;
    [[nodiscard]] RendererPresentationStatus GetPresentationStatus() const;
    // Call on the renderer owner thread before input begins. A completed
    // simulation token may also disable FG before its first paused/menu frame.
    // Changes drain/recreate the native presentation chain; requested settings survive.
    [[nodiscard]] Result SetFrameGenerationAllowed(bool allowed);
    [[nodiscard]] Result BeginInputFrame(std::uint64_t &identity);
    [[nodiscard]] Result InputSampled(std::uint64_t identity);
    [[nodiscard]] Result SimulationCompleted(std::uint64_t identity);
    [[nodiscard]] bool UsesNativeFrameLimiter() const noexcept;
    [[nodiscard]] RendererLightingStatus GetLightingStatus() const;
    [[nodiscard]] RendererTextureStreamingStatus GetTextureStreamingStatus() const;
    [[nodiscard]] RendererTextureProductStatus GetTextureStreamingStatus(std::string_view logical_id) const noexcept;
    [[nodiscard]] std::vector<RendererCharacterAnimationClip> GetCharacterAnimationClips() const;
    [[nodiscard]] RendererPsoCacheStats GetPipelineCacheStats() const;
    // Call on the renderer owner thread; the returned bounded value owns its data.
    [[nodiscard]] RendererDiagnosticsSnapshot GetDiagnostics() const;

  private:
    [[nodiscard]] Result ApplyOptions(const RendererOptions &options);
    [[nodiscard]] Result InitializeGraphicsSettings(const GraphicsSettings &);
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
                                RendererFrameResult &frame_result,
                                VfxVolumeInputs typed_volumes = {}, VfxScreenInputs typed_screen = {},
                                VfxEntitySurfaceInputs typed_entity_surfaces = {});
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hs
