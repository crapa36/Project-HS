#pragma once
#if defined(HS_DEVELOPMENT_TOOLS)
#include "runtime_diagnostics.hpp"
#include <hs/renderer/hero_work.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <format>
#include <bit>
#include <tuple>
#include <vector>

namespace hs::runtime_detail
{
// Isolated developer capture. It never feeds normal frame policy or asset identity.
class HeroWorkLog
{
  public:
    HeroWorkLog(const std::filesystem::path &directory, DiagnosticClock clock)
        : directory_(directory), output_(directory / "hero_work.csv", std::ios::binary),
          lines_(output_, true, Header(), clock) {}
    bool Failed() const noexcept { return !output_ || lines_.Failed(); }
    void Append(const RendererHeroWorkSample &sample)
    {
        auto found = std::ranges::find_if(pipelines_, [&](const auto &pipeline) { return Same(pipeline, sample.pipeline); });
        const auto pipeline = static_cast<std::size_t>(found - pipelines_.begin());
        if (found == pipelines_.end()) pipelines_.push_back(sample.pipeline);
        const auto &source = sample.source;
        std::string row = std::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}",
            sample.completed_frame, sample.tick, sample.view_id, sample.phase, sample.layer, sample.contributor, pipeline,
            source.asset_id, CsvField(source.product), CsvField(source.label), source.lod, source.material,
            source.candidate_instances, source.geometry_indices, source.color_format, source.data_format,
            source.color_mips, source.data_mips, sample.width, sample.height,
            sample.execute_indirect_calls, sample.draw_indexed_calls);
        row += std::format(",{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}", sample.dispatch_mesh_calls,
            sample.gpu_begin, sample.gpu_end, sample.gpu_frequency, sample.cpu_begin_qpc, sample.cpu_end_qpc,
            sample.cpu_qpc_frequency, sample.calibrated_begin_qpc, sample.calibrated_end_qpc,
            sample.timestamp_valid ? 1 : 0, sample.calibration_valid ? 1 : 0, sample.statistics_valid ? 1 : 0,
            sample.mesh_statistics_available ? 1 : 0, sample.occlusion_valid ? 1 : 0, sample.indirect_arguments_valid ? 1 : 0);
        for (std::size_t i = 0; i < sample.pipeline_statistics.size(); ++i)
            row += sample.statistics_valid && (i < 11 || sample.mesh_statistics_available) ?
                std::format(",{}", sample.pipeline_statistics[i]) : ",";
        row += sample.occlusion_valid ? std::format(",{}", sample.occlusion_samples) : ",";
        for (const auto argument : sample.indexed_arguments)
            row += sample.indirect_arguments_valid ? std::format(",{}", argument) : ",";
        if ((sample.indirect_arguments_valid && !sample.indirect_arguments_in_range) ||
            (sample.execute_indirect_calls != 0 && !sample.indirect_arguments_valid)) ++invalid_argument_samples_;
        if (!sample.timestamp_valid) ++invalid_timestamp_samples_;
        if (!sample.calibration_valid) ++uncalibrated_samples_;
        row += std::format(",{},{},{},{},{}", sample.unique_pixels_available ? 1 : 0,
            sample.full_material_evaluations_available ? 1 : 0, sample.layer_rejections_available ? 1 : 0,
            sample.sampled_mip_available ? 1 : 0, sample.indirect_arguments_in_range ? 1 : 0);
        row += std::format(",{},{},{},{},{}", sample.shader_counters_available ? 1 : 0,
            sample.shader_counters_valid ? 1 : 0, sample.shader_counter_flags, sample.shader_bitmap_pixels,
            static_cast<unsigned>(sample.diagnostic_mode));
        for (const auto counter : sample.shader_counters)
            row += sample.shader_counters_available ? std::format(",{}", counter) : ",";
        if (!sample.shader_counters_available || !sample.shader_counters_valid) ++invalid_shader_samples_;
        row += '\n';
        lines_.Append(row);
    }
    bool Finish(const RendererHeroWorkStatus &status, bool drained)
    {
        const auto written = lines_.Finish(); output_.close();
        nlohmann::json metadata = nlohmann::json::array();
        for (const auto &p : pipelines_)
            metadata.push_back({{"vertex_product", p.vertex_product}, {"pixel_product", p.pixel_product},
                {"vertex_sha256", Hex(p.vertex_sha256)}, {"pixel_sha256", Hex(p.pixel_sha256)},
                {"source_requested_flags", p.source_requested_flags}, {"compiler_flags_verified_from_binary", p.compiler_flags_verified_from_binary},
                {"cull_mode", p.cull_mode}, {"fill_mode", p.fill_mode}, {"depth_func", p.depth_func},
                {"front_counter_clockwise", p.front_counter_clockwise}, {"depth_clip_enabled", p.depth_clip_enabled},
                {"depth_bias", p.depth_bias}, {"depth_bias_clamp", p.depth_bias_clamp}, {"slope_scaled_depth_bias", p.slope_scaled_depth_bias},
                {"depth_write_mask", p.depth_write_mask}, {"depth_enabled", p.depth_enabled}, {"stencil_enabled", p.stencil_enabled},
                {"sample_count", p.sample_count}, {"sample_mask", p.sample_mask}, {"independent_blend", p.independent_blend},
                {"alpha_to_coverage", p.alpha_to_coverage}, {"rtv_count", p.rtv_count}, {"rtv_formats", p.rtv_formats},
                {"dsv_format", p.dsv_format}, {"blend_enabled", p.blend_enabled}, {"src_blend", p.src_blend},
                {"dst_blend", p.dst_blend}, {"blend_op", p.blend_op}, {"src_blend_alpha", p.src_blend_alpha},
                {"dst_blend_alpha", p.dst_blend_alpha}, {"blend_op_alpha", p.blend_op_alpha}, {"write_mask", p.write_mask}});
        std::ofstream pipelines(directory_ / "hero_work_pipelines.json", std::ios::binary);
        pipelines << metadata.dump(2) << '\n'; pipelines.close();
        const bool io = written && lines_.Complete() && !output_.fail() && !pipelines.fail();
        const bool native_complete = io && drained && status.enabled && !status.overflowed && status.failed_frames == 0 &&
            status.pending_frames == 0 && status.submitted_frames == status.completed_frames && status.samples == lines_.Records() &&
            invalid_argument_samples_ == 0 && invalid_timestamp_samples_ == 0;
        const bool shader_complete = status.shader_counters_enabled && status.shader_invalid_samples == 0 && invalid_shader_samples_ == 0;
        const bool complete = native_complete && shader_complete;
        nlohmann::json summary{{"complete", complete}, {"io_complete", io}, {"drained", drained},
            {"queue", "direct"},
            {"phase_names", {"Front", "Exit", "Tail", "Guide", "Composite"}},
            {"surface_reuse", "Guide phase3 is reserved and not submitted; Composite layer0 uses the existing guide pixel product to emit HDR plus six original-surface guides after one transport-surface evaluation; other layers and tail retain their own surfaces"},
            {"cpu_callback_scope", "actual work_draw API body only; query/mapping/CSV overhead excluded; outer frame clocks include diagnostic overhead"},
            {"native_stream_complete", native_complete}, {"shader_counter_stream_complete", shader_complete}, {"t01_complete", false},
            {"t01_incomplete_reason", "actual sampled mip and DCE-retained full-evaluator instruction work remain unavailable; logical calls and exact shader-predicate coverage are separate measurements"},
            {"coverage_available", status.samples != 0 && shader_complete},
            {"shader_counter_names", kHeroWorkShaderCounterNames},
            {"shader_counter_flags", {{"counter_wrap", 1}, {"pixel_out_of_range", 2}}},
            {"shader_counter_semantics", "uint32 atomic events; all raw values retained on invalid; unique bitmaps use atomicOr previous-bit tests per contributor/phase/layer; coverage is shader-entry or accepted-shader-predicate union, not post-depth visible pixels"},
            {"covered_sample_semantics", "countbits(SV_Coverage) at shader entry and accepted predicates; distinct from native occlusion passed samples and raw PSInvocations; actual PSO sample_count is in pipeline metadata"},
            {"material_semantics", "logical_surface_evaluator_calls counts HsHeroEvaluateSurface calls; water construction is separate; optimizer-retained graph operations are not inferred"},
            {"environment_semantics", "actual indirect function entry, mixture32 branch entry, sample iterations and valid/invalid samples; basic-lobe and bypass branches separate"},
            {"diagnostic_mode_names", {"full", "no-environment", "coverage-only"}},
            {"diagnostic_mode_semantics", "same instrumented products/geometry/gradients/layer/alpha/depth; no-environment zeros indirect ambient; coverage-only skips transport color lighting; intrusive sensitivity experiments, never production quality or exclusive cost attribution"},
            {"intrusion", "PS UAV atomics and bitmap updates may change early/late depth behavior and scheduling; extra copy/zero/map/query commands and allocations are included in diagnostic capture only; normal A/B capture flag must be false"},
            {"sampled_mip", nullptr}, {"retained_full_evaluator_instructions", nullptr},
            {"shader_gpu_bytes", status.shader_gpu_bytes}, {"shader_readback_bytes", status.shader_readback_bytes},
            {"shader_zero_upload_bytes", status.shader_zero_upload_bytes},
            {"known_diagnostic_storage_bytes", status.readback_bytes + status.shader_gpu_bytes + status.shader_readback_bytes + status.shader_zero_upload_bytes},
            {"known_storage_limit_bytes", status.shader_storage_limit_bytes},
            {"shader_clear_commands", status.shader_clear_commands}, {"shader_copy_commands", status.shader_copy_commands},
            {"shader_invalid_samples", status.shader_invalid_samples}, {"invalid_shader_counter_rows", invalid_shader_samples_},
            {"enabled", status.enabled}, {"overflowed", status.overflowed}, {"submitted_frames", status.submitted_frames},
            {"completed_frames", status.completed_frames}, {"failed_frames", status.failed_frames},
            {"pending_frames", status.pending_frames}, {"samples", status.samples}, {"written_samples", lines_.Records()},
            {"invalid_indirect_argument_samples", invalid_argument_samples_},
            {"invalid_timestamp_samples", invalid_timestamp_samples_}, {"uncalibrated_samples", uncalibrated_samples_},
            {"readback_bytes", status.readback_bytes}, {"query_heap_count", status.query_heap_count}, {"query_count", status.query_count},
            {"query_heap_bytes_available", status.query_heap_bytes_available}, {"mesh_statistics_supported", status.mesh_statistics_supported},
            {"interpretation", "isolated diagnostic; native PS/occlusion counts are not unique pixels, full evaluations or exclusive GPU contribution"}};
        std::ofstream manifest(directory_ / "hero_work_status.json", std::ios::binary);
        manifest << summary.dump(2) << '\n'; manifest.close();
        return complete && !manifest.fail();
    }
  private:
    static std::string Header()
    {
        std::string header = "completed_frame,tick,view_id,phase,layer,contributor,pipeline,asset_id,product,label,lod,material,candidate_instances,geometry_indices,color_format,data_format,color_mips,data_mips,width,height,execute_indirect_calls,draw_indexed_calls,dispatch_mesh_calls,gpu_begin,gpu_end,gpu_frequency,cpu_begin_qpc,cpu_end_qpc,cpu_qpc_frequency,calibrated_begin_qpc,calibrated_end_qpc,timestamp_valid,calibration_valid,statistics_valid,mesh_statistics_available,occlusion_valid,indirect_arguments_valid,ia_vertices,ia_primitives,vs_invocations,gs_invocations,gs_primitives,clip_invocations,clip_primitives,ps_invocations,hs_invocations,ds_invocations,cs_invocations,as_invocations,ms_invocations,ms_primitives,occlusion_samples,root_constant,index_count_per_instance,instance_count,start_index_location,base_vertex_location_bits,start_instance_location,unique_pixels_available,full_material_evaluations_available,layer_rejections_available,sampled_mip_available,indirect_arguments_in_range,shader_counters_available,shader_counters_valid,shader_counter_flags,shader_bitmap_capacity_pixels,diagnostic_mode";
        for (const auto *name : kHeroWorkShaderCounterNames) { header += ','; header += name; }
        return header + '\n';
    }
    static std::string Hex(const std::array<std::byte, 32> &bytes)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string result; result.reserve(64);
        for (const auto b : bytes) { const auto value = std::to_integer<unsigned>(b); result += digits[value >> 4]; result += digits[value & 15]; }
        return result;
    }
    static bool Same(const RendererHeroWorkPipeline &a, const RendererHeroWorkPipeline &b)
    {
        const auto fields = [](const auto &p) { return std::tie(p.vertex_product, p.pixel_product, p.vertex_sha256, p.pixel_sha256,
            p.source_requested_flags, p.cull_mode, p.fill_mode, p.depth_func, p.depth_write_mask, p.sample_count, p.sample_mask,
            p.front_counter_clockwise, p.depth_clip_enabled, p.depth_bias,
            p.depth_enabled, p.stencil_enabled, p.independent_blend, p.alpha_to_coverage, p.rtv_formats, p.blend_enabled,
            p.src_blend, p.dst_blend, p.blend_op, p.src_blend_alpha, p.dst_blend_alpha, p.blend_op_alpha, p.write_mask,
            p.rtv_count, p.dsv_format, p.compiler_flags_verified_from_binary); };
        return fields(a) == fields(b) &&
            std::bit_cast<std::uint32_t>(a.depth_bias_clamp) == std::bit_cast<std::uint32_t>(b.depth_bias_clamp) &&
            std::bit_cast<std::uint32_t>(a.slope_scaled_depth_bias) == std::bit_cast<std::uint32_t>(b.slope_scaled_depth_bias);
    }
    std::filesystem::path directory_;
    std::ofstream output_;
    DiagnosticLog lines_;
    std::vector<RendererHeroWorkPipeline> pipelines_;
    std::uint64_t invalid_argument_samples_{}, invalid_timestamp_samples_{}, uncalibrated_samples_{}, invalid_shader_samples_{};
};
}
#endif
