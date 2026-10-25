#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hs
{
enum class HeroWorkDiagnosticMode : std::uint8_t { Full, NoEnvironment, CoverageOnly };
inline constexpr std::array<const char *, 29> kHeroWorkShaderCounterNames{
    "shader_entries",
    "input_covered_samples",
    "unique_input_pixels",
    "rejected_front",
    "rejected_zero_identity",
    "rejected_opaque_depth",
    "rejected_previous_layer",
    "rejected_entry_missing",
    "rejected_entry_depth",
    "rejected_identity",
    "rejected_selected_front",
    "rejected_material_binding",
    "rejected_opacity",
    "logical_surface_evaluator_calls",
    "water_surface_constructions",
    "accepted_shader_fragments",
    "accepted_covered_samples",
    "unique_accepted_pixels",
    "transport_surface_calls",
    "environment_indirect_calls",
    "environment_mixture32_entries",
    "environment_sample_iterations",
    "environment_valid_samples",
    "environment_invalid_samples",
    "environment_basic_lobe_entries",
    "environment_lighting_calls",
    "environment_bypassed_calls",
    "coverage_only_lighting_returns",
    "environment_coated_prefiltered_entries"
};
// Development capture only. Raw hardware counts are not unique pixels, rejected
// layers, full-material evaluations, or exclusive critical-path contributions.
struct RendererHeroWorkContributor
{
    std::uint64_t asset_id{};
    std::string product, label; // label is diagnostic, never a semantic asset ID.
    // material == UINT32_MAX means mixed/unknown within the existing draw.
    std::uint32_t lod{}, material{0xFFFFFFFFu}, candidate_instances{}, geometry_indices{};
    std::uint32_t color_format{}, data_format{}, color_mips{}, data_mips{};
};
struct RendererHeroWorkPipeline
{
    std::string vertex_product, pixel_product;
    std::array<std::byte, 32> vertex_sha256{}, pixel_sha256{};
    std::string source_requested_flags;
    // Native PSO values, including all active render-target blend descriptors.
    std::uint32_t cull_mode{}, fill_mode{}, depth_func{}, depth_write_mask{}, sample_count{}, sample_mask{};
    bool front_counter_clockwise{}, depth_clip_enabled{};
    std::int32_t depth_bias{};
    float depth_bias_clamp{}, slope_scaled_depth_bias{};
    bool depth_enabled{}, stencil_enabled{}, independent_blend{}, alpha_to_coverage{};
    std::array<std::uint32_t, 8> rtv_formats{}, blend_enabled{}, src_blend{}, dst_blend{}, blend_op{},
        src_blend_alpha{}, dst_blend_alpha{}, blend_op_alpha{}, write_mask{};
    std::uint32_t rtv_count{}, dsv_format{};
    bool compiler_flags_verified_from_binary{};
};
struct RendererHeroWorkSample
{
    std::uint64_t completed_frame{}, tick{};
    std::uint32_t view_id{}, phase{}, layer{}, contributor{}, width{}, height{};
    RendererHeroWorkContributor source;
    RendererHeroWorkPipeline pipeline;
    std::uint64_t gpu_begin{}, gpu_end{}, gpu_frequency{}, cpu_begin_qpc{}, cpu_end_qpc{}, cpu_qpc_frequency{};
    std::uint64_t calibrated_begin_qpc{}, calibrated_end_qpc{};
    // IA vertices/primitives, VS, GS, GS primitives, clip invocations/primitives,
    // PS, HS, DS, CS; optional AS, MS, MS primitives (D3D12 native order).
    std::array<std::uint64_t, 14> pipeline_statistics{};
    std::uint64_t occlusion_samples{};
    std::array<std::uint32_t, 6> indexed_arguments{}; // root constant + DRAW_INDEXED_ARGUMENTS
    std::uint32_t execute_indirect_calls{}, draw_indexed_calls{}, dispatch_mesh_calls{};
    bool timestamp_valid{}, calibration_valid{}, statistics_valid{}, mesh_statistics_available{},
        occlusion_valid{}, indirect_arguments_valid{}, indirect_arguments_in_range{};
    // Exact instrumented-shader events, distinct from native PS/occlusion counts.
    std::array<std::uint32_t, kHeroWorkShaderCounterNames.size()> shader_counters{};
    std::uint32_t shader_counter_flags{}, shader_bitmap_pixels{};
    bool shader_counters_available{}, shader_counters_valid{};
    HeroWorkDiagnosticMode diagnostic_mode{HeroWorkDiagnosticMode::Full};
    // Logical evaluator call count is not DCE-retained instruction work.
    bool unique_pixels_available{}, full_material_evaluations_available{}, layer_rejections_available{}, sampled_mip_available{};
};
struct RendererHeroWorkStatus
{
    bool enabled{}, overflowed{}, mesh_statistics_supported{};
    // Submission notifications include failed recording/submission attempts;
    // failed frames never publish a row or count as completed_frames.
    std::uint64_t submitted_frames{}, completed_frames{}, failed_frames{}, pending_frames{}, samples{},
        readback_bytes{}, query_heap_count{}, query_count{};
    std::uint64_t shader_gpu_bytes{}, shader_readback_bytes{}, shader_zero_upload_bytes{}, shader_invalid_samples{},
        shader_clear_commands{}, shader_copy_commands{};
    std::uint64_t shader_storage_limit_bytes{512ull * 1024 * 1024};
    bool shader_counters_enabled{};
    // Readback allocator bytes are known; native opaque query-heap bytes are not.
    bool query_heap_bytes_available{};
};
} // namespace hs
