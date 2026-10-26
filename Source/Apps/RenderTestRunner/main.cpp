#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/process_info.hpp>
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/graphics_settings_request.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/presentation/meadow_surface.hpp>
#include <hs/render_data/scene_probe.hpp>
#include <hs/render_data/quality_profiles.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/render_data/render_frame_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include "view_fixture.hpp"
#include "static_shadow_bake_probe.hpp"
#include "../Common/world_transport_scene.hpp"
#include "../Common/renderer_geometry_catalog.hpp"
#include <hs/jobs/task_system.hpp>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <d3d12.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <span>
#include <set>
#include <tuple>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

namespace
{
using Clock = std::chrono::steady_clock;
using Json = nlohmann::ordered_json;

const char *HeroWorkModeName(hs::HeroWorkDiagnosticMode mode)
{
    switch (mode)
    {
    case hs::HeroWorkDiagnosticMode::Full: return "full";
    case hs::HeroWorkDiagnosticMode::NoEnvironment: return "no-environment";
    case hs::HeroWorkDiagnosticMode::CoverageOnly: return "coverage-only";
    }
    return "invalid";
}

void ValidateHeroWorkProbe(const std::vector<hs::RendererHeroWorkSample> &samples,
    const hs::RendererHeroWorkStatus &status, hs::HeroWorkDiagnosticMode mode,
    std::uint32_t frames, const hs::Result &drained, Json &receipt, bool require_water = false,
    bool require_layer_transition = false, bool quality_transition_valid = true)
{
    auto &probe = receipt["hero_work"];
    probe["mode"] = HeroWorkModeName(mode);
    probe["functional_only"] = true;
    probe["performance_adoption"] = false;
    probe["timing_interpretation"] = "Intrusive functional probe under uncontrolled external GPU competition; no FPS, speedup or quality adoption claim.";
    probe["coverage_semantics"] = "Atomic per-contributor/phase/layer shader-entry and accepted-predicate pixel unions; not summed global visible pixels. SV_Coverage samples, native occlusion samples and PSInvocations are distinct.";
    probe["material_semantics"] = "Logical evaluator calls and actual environment32 branch/sample events; no DCE-retained full graph instruction claim.";
    probe["sampled_mip"] = nullptr;
    probe["retained_full_evaluator_instructions"] = nullptr;
    probe["drain_succeeded"] = drained.Succeeded();
    probe["drain_error"] = drained.Succeeded() ? "" : std::string(drained.Message());
    probe["status"] = {{"enabled", status.enabled}, {"overflowed", status.overflowed},
        {"submitted_frames", status.submitted_frames}, {"completed_frames", status.completed_frames},
        {"failed_frames", status.failed_frames}, {"pending_frames", status.pending_frames}, {"samples", status.samples},
        {"query_readback_bytes", status.readback_bytes}, {"query_heap_count", status.query_heap_count},
        {"query_count", status.query_count}, {"query_heap_bytes", nullptr},
        {"mesh_statistics_supported", status.mesh_statistics_supported}, {"shader_counters_enabled", status.shader_counters_enabled},
        {"shader_gpu_bytes", status.shader_gpu_bytes}, {"shader_header_readback_bytes", status.shader_readback_bytes},
        {"shader_zero_upload_bytes", status.shader_zero_upload_bytes}, {"shader_storage_limit_bytes", status.shader_storage_limit_bytes},
        {"known_storage_bytes", status.readback_bytes + status.shader_gpu_bytes + status.shader_readback_bytes + status.shader_zero_upload_bytes},
        {"shader_invalid_samples", status.shader_invalid_samples}, {"shader_clear_commands", status.shader_clear_commands},
        {"shader_copy_commands", status.shader_copy_commands}};
    probe["counter_names"] = hs::kHeroWorkShaderCounterNames;
    probe["phase_names"] = {"Front", "Exit", "Tail", "Guide", "Composite"};
    probe["pipeline_statistics_order"] = {"IA vertices", "IA primitives", "VS", "GS", "GS primitives", "clip invocations", "clip primitives", "PS", "HS", "DS", "CS", "AS optional", "MS optional", "MS primitives optional"};
    probe["counter_flags"] = {{"uint32_wrap", 1}, {"pixel_out_of_range", 2}};
    probe["pipelines"] = Json::object();
    probe["samples"] = Json::array();
    Json failures = Json::array();
    const auto check = [&](bool valid, std::string_view reason) { if (!valid) failures.push_back(reason); };
    check(drained.Succeeded(), "native drain failed");
    check(status.enabled && status.shader_counters_enabled && !status.overflowed && status.failed_frames == 0 &&
        status.pending_frames == 0 && status.shader_invalid_samples == 0, "capture status failure/overflow/pending/invalid counters");
    check(status.submitted_frames == frames && status.completed_frames == frames && status.samples == samples.size() && !samples.empty(),
        "completed frame/sample conservation or empty fixture");
    check(status.readback_bytes + status.shader_gpu_bytes + status.shader_readback_bytes + status.shader_zero_upload_bytes <=
        status.shader_storage_limit_bytes, "known diagnostic allocation limit");
    const auto hex = [](const std::array<std::byte, 32> &bytes) {
        constexpr char digits[] = "0123456789abcdef";
        std::string value; value.reserve(64);
        for (const auto b : bytes) { const auto n = std::to_integer<unsigned>(b); value += digits[n >> 4]; value += digits[n & 15]; }
        return value;
    };
    std::array<std::uint64_t, hs::kHeroWorkShaderCounterNames.size()> totals{};
    std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> identities;
    std::uint64_t shaded_accepted{}, water_accepted{}, water_constructions{}, water_rows{}, fused_rows{}, fused_accepted{};
    std::set<std::tuple<std::uint64_t, std::string, std::uint32_t>> water_sources;
    std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>, std::array<std::set<std::uint32_t>, 3>> layer_rows;
    std::array<std::uint64_t, 9> farther_accepted{};
    std::uint64_t uncalibrated{}, mesh_statistics_na{};
    for (const auto &sample : samples)
    {
        const auto &p = sample.pipeline;
        Json pipeline{{"vertex_product", p.vertex_product}, {"pixel_product", p.pixel_product},
            {"vertex_sha256", hex(p.vertex_sha256)}, {"pixel_sha256", hex(p.pixel_sha256)},
            {"source_requested_flags", p.source_requested_flags}, {"compiler_flags_verified_from_binary", p.compiler_flags_verified_from_binary},
            {"cull_mode", p.cull_mode}, {"fill_mode", p.fill_mode}, {"front_counter_clockwise", p.front_counter_clockwise},
            {"depth_clip_enabled", p.depth_clip_enabled}, {"depth_bias", p.depth_bias}, {"depth_bias_clamp", p.depth_bias_clamp},
            {"slope_scaled_depth_bias", p.slope_scaled_depth_bias}, {"depth_enabled", p.depth_enabled},
            {"depth_func", p.depth_func}, {"depth_write_mask", p.depth_write_mask}, {"stencil_enabled", p.stencil_enabled},
            {"sample_count", p.sample_count}, {"sample_mask", p.sample_mask}, {"rtv_count", p.rtv_count}, {"rtv_formats", p.rtv_formats},
            {"dsv_format", p.dsv_format}, {"independent_blend", p.independent_blend}, {"alpha_to_coverage", p.alpha_to_coverage},
            {"blend_enabled", p.blend_enabled}, {"src_blend", p.src_blend}, {"dst_blend", p.dst_blend}, {"blend_op", p.blend_op},
            {"src_blend_alpha", p.src_blend_alpha}, {"dst_blend_alpha", p.dst_blend_alpha}, {"blend_op_alpha", p.blend_op_alpha},
            {"write_mask", p.write_mask}};
        auto &stored = probe["pipelines"][p.pixel_product];
        if (!stored.is_null()) check(stored == pipeline, "pipeline metadata changed within fixed probe");
        else stored = std::move(pipeline);
        Json counters = Json::object();
        for (std::size_t index = 0; index < totals.size(); ++index)
        { counters[hs::kHeroWorkShaderCounterNames[index]] = sample.shader_counters[index]; totals[index] += sample.shader_counters[index]; }
        Json statistics = Json::array();
        for (std::size_t index = 0; index < sample.pipeline_statistics.size(); ++index)
            if (sample.statistics_valid && (index < 11 || sample.mesh_statistics_available)) statistics.push_back(sample.pipeline_statistics[index]);
            else statistics.push_back(nullptr);
        probe["samples"].push_back({{"completed_frame", sample.completed_frame}, {"tick", sample.tick}, {"view_id", sample.view_id},
            {"phase", sample.phase}, {"layer", sample.layer}, {"contributor", sample.contributor}, {"width", sample.width}, {"height", sample.height},
            {"diagnostic_mode", static_cast<unsigned>(sample.diagnostic_mode)}, {"pixel_product", p.pixel_product},
            {"source", {{"asset_id", sample.source.asset_id}, {"product", sample.source.product}, {"label", sample.source.label},
                {"lod", sample.source.lod}, {"material", sample.source.material}, {"candidate_instances", sample.source.candidate_instances},
                {"geometry_indices", sample.source.geometry_indices}, {"color_format", sample.source.color_format},
                {"data_format", sample.source.data_format}, {"color_mips", sample.source.color_mips}, {"data_mips", sample.source.data_mips}}},
            {"gpu_begin", sample.gpu_begin}, {"gpu_end", sample.gpu_end}, {"gpu_frequency", sample.gpu_frequency},
            {"timestamp_valid", sample.timestamp_valid}, {"cpu_begin_qpc", sample.cpu_begin_qpc}, {"cpu_end_qpc", sample.cpu_end_qpc},
            {"cpu_qpc_frequency", sample.cpu_qpc_frequency}, {"calibration_valid", sample.calibration_valid},
            {"calibrated_begin_qpc", sample.calibration_valid ? Json(sample.calibrated_begin_qpc) : Json(nullptr)},
            {"calibrated_end_qpc", sample.calibration_valid ? Json(sample.calibrated_end_qpc) : Json(nullptr)},
            {"pipeline_statistics", std::move(statistics)}, {"statistics_valid", sample.statistics_valid},
            {"mesh_statistics_available", sample.mesh_statistics_available}, {"occlusion_valid", sample.occlusion_valid},
            {"occlusion_samples", sample.occlusion_valid ? Json(sample.occlusion_samples) : Json(nullptr)},
            {"indexed_arguments", sample.indexed_arguments}, {"indirect_arguments_valid", sample.indirect_arguments_valid},
            {"indirect_arguments_in_range", sample.indirect_arguments_in_range}, {"execute_indirect_calls", sample.execute_indirect_calls},
            {"draw_indexed_calls", sample.draw_indexed_calls}, {"dispatch_mesh_calls", sample.dispatch_mesh_calls},
            {"unique_pixels_available", sample.unique_pixels_available}, {"layer_rejections_available", sample.layer_rejections_available},
            {"full_material_evaluations_available", sample.full_material_evaluations_available}, {"sampled_mip_available", sample.sampled_mip_available},
            {"shader_counters_available", sample.shader_counters_available}, {"shader_counters_valid", sample.shader_counters_valid},
            {"shader_counter_flags", sample.shader_counter_flags}, {"shader_bitmap_capacity_pixels", sample.shader_bitmap_pixels},
            {"shader_counters", std::move(counters)}});
        const auto &c = sample.shader_counters;
        const auto pixels = std::uint64_t(sample.width) * sample.height;
        check(identities.emplace(sample.completed_frame, sample.view_id, sample.phase, sample.layer, sample.contributor).second,
            "duplicate completed sample identity");
        check(sample.diagnostic_mode == mode && sample.timestamp_valid && sample.statistics_valid && sample.occlusion_valid &&
            sample.indirect_arguments_valid && sample.indirect_arguments_in_range, "native query/argument/mode validity");
        check(sample.shader_counters_available && sample.shader_counters_valid && sample.shader_counter_flags == 0,
            "shader counter availability/validation/overflow flag");
        check(sample.width && sample.height && pixels <= sample.shader_bitmap_pixels && c[2] <= pixels && c[2] <= c[0] &&
            c[17] <= c[2] && c[17] <= c[15] && c[15] <= c[0], "exact unique pixel/fragment bound");
        check(std::uint64_t(c[20]) * 32 == c[21] && std::uint64_t(c[22]) + c[23] == c[21], "environment32 sample conservation");
        check(p.pixel_product.ends_with("_hero_work.dxil"), "instrumented shader product identity");
        check(sample.phase != 3, "retired standalone Guide draw was submitted");
        if (require_layer_transition && (sample.phase <= 1 || sample.phase == 4))
            layer_rows[{sample.completed_frame, sample.view_id, sample.contributor}][sample.phase == 4 ? 2 : sample.phase].insert(sample.layer);
        if (sample.phase == 4)
        {
            check(p.depth_enabled && p.depth_func == D3D12_COMPARISON_FUNC_EQUAL &&
                p.depth_write_mask == D3D12_DEPTH_WRITE_MASK_ZERO && p.dsv_format == DXGI_FORMAT_D32_FLOAT &&
                p.cull_mode == D3D12_CULL_MODE_BACK && p.sample_count == 1,
                "composite must retain the selected-front raster/depth contract");
            if (sample.layer == 0)
            {
                ++fused_rows; fused_accepted += c[15];
                constexpr std::array<std::uint32_t, 7> formats{DXGI_FORMAT_R16G16B16A16_FLOAT,
                    DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R16G16_FLOAT};
                check(p.pixel_product == "render/shader/hero_transparent_guide_ps_hero_work.dxil" &&
                    p.rtv_count == formats.size() && p.independent_blend,
                    "nearest composite must report the actual fused guide shader and seven-target PSO");
                for (std::size_t target = 0; target < formats.size(); ++target)
                    check(p.rtv_formats[target] == formats[target] && p.blend_enabled[target] == 0 &&
                        p.write_mask[target] == D3D12_COLOR_WRITE_ENABLE_ALL,
                        "fused HDR/guide target format, replace blend or write mask changed");
                check(c[18] <= c[0] && std::uint64_t(c[13]) + c[14] <= c[18] &&
                    c[15] <= std::uint64_t(c[13]) + c[14],
                    "nearest surface must be evaluated at most once per shader entry");
            }
            else check(p.pixel_product == "render/shader/hero_layer_composite_ps_hero_work.dxil" && p.rtv_count == 1,
                "farther composites must retain the original single-target product");
            if (sample.layer > 0 && sample.completed_frame < farther_accepted.size())
                farther_accepted[sample.completed_frame] += c[15];
        }
        if (sample.phase == 2 || sample.phase == 4) shaded_accepted += c[15];
        if (sample.source.label == "water")
        {
            ++water_rows; water_accepted += c[15]; water_constructions += c[14];
            water_sources.emplace(sample.source.asset_id, sample.source.product, sample.source.lod);
            for (const auto &[asset, product, lod] : water_sources)
                if (asset == sample.source.asset_id)
                    check(product == sample.source.product && lod == sample.source.lod, "one water geometry asset was reported under conflicting LOD metadata");
            check(sample.source.asset_id != 0 && sample.source.asset_id == hs::MakeAssetId(sample.source.product).value &&
                sample.source.product.starts_with("render/geometry/") && sample.source.product.ends_with(".rpage") &&
                sample.source.lod <= 2 && sample.source.geometry_indices != 0,
                "water contributor must retain its actual separate geometry asset/product/LOD identity");
            check(c[13] == 0 && c[14] >= c[15], "analytic water construction must be separate from character material evaluation");
        }
        else check(c[14] == 0, "water construction appeared in a non-water contributor");
        uncalibrated += !sample.calibration_valid; mesh_statistics_na += !sample.mesh_statistics_available;
    }
    check(totals[15] > 0 && totals[17] > 0 && shaded_accepted > 0, "fixture produced no actual accepted shaded Hero fragment");
    check(fused_rows > 0 && fused_accepted > 0, "fixture did not execute the fused nearest composite");
    probe["surface_reuse"] = {{"standalone_guide_submitted", false}, {"nearest_composite_rows", fused_rows},
        {"nearest_composite_accepted_fragments", fused_accepted},
        {"contract", "Composite layer0 emits HDR plus six original-surface guides after one transport-surface evaluation; farther layers and tail retain their own surfaces"}};
    if (require_layer_transition)
    {
        check(quality_transition_valid, "actual applied refraction/transparency quality did not match the eight-frame transition");
        auto &transition = probe["quality_transition"];
        transition["layer_rows"] = Json::array();
        std::map<std::uint64_t, std::uint32_t> groups;
        for (const auto &[key, phases] : layer_rows)
        {
            const auto [frame, view, contributor] = key;
            const std::set<std::uint32_t> expected = frame <= 4 ? std::set<std::uint32_t>{0}
                : std::set<std::uint32_t>{0, 1, 2, 3}; // Existing Medium/Ultra quality rows: one/four layers.
            check(frame >= 1 && frame <= frames && phases[0] == expected && phases[1] == expected && phases[2] == expected,
                "front/exit/composite completed layer directory differs from the actual Medium/Ultra fixture contract");
            ++groups[frame];
            transition["layer_rows"].push_back({{"completed_frame", frame}, {"view_id", view}, {"contributor", contributor},
                {"front", phases[0]}, {"exit", phases[1]}, {"composite", phases[2]}});
        }
        transition["farther_accepted_fragments"] = Json::array();
        for (std::uint32_t frame = 1; frame <= frames; ++frame)
        {
            check(groups[frame] != 0 && groups[frame] == groups[1], "transition omitted a completed frame/contributor directory");
            if (frame > 4) check(farther_accepted[frame] > 0, "overlapped Ultra fixture produced no accepted farther composite surface");
            transition["farther_accepted_fragments"].push_back({{"completed_frame", frame}, {"accepted", farther_accepted[frame]}});
        }
    }
    if (mode == hs::HeroWorkDiagnosticMode::Full)
        check(totals[26] == 0 && totals[27] == 0 && totals[19] > 0 && totals[25] > 0 && totals[20] + totals[24] + totals[28] == totals[19],
            "Full environment path or unexpected diagnostic bypass");
    else if (mode == hs::HeroWorkDiagnosticMode::NoEnvironment)
        check(totals[19] > 0 && totals[25] > 0 && totals[26] == totals[19] && totals[20] == 0 && totals[21] == 0 &&
            totals[22] == 0 && totals[23] == 0 && totals[24] == 0 && totals[27] == 0 && totals[28] == 0, "NoEnvironment bypass relation");
    else
        check(totals[27] == shaded_accepted && totals[27] > 0 && totals[19] == 0 && totals[20] == 0 && totals[21] == 0 &&
            totals[22] == 0 && totals[23] == 0 && totals[24] == 0 && totals[25] == 0 && totals[26] == 0 && totals[28] == 0, "CoverageOnly bypass relation");
    probe["counter_totals"] = Json::object();
    for (std::size_t index = 0; index < totals.size(); ++index) probe["counter_totals"][hs::kHeroWorkShaderCounterNames[index]] = totals[index];
    probe["lighting_phase_accepted_fragments"] = shaded_accepted;
    probe["water"] = {{"required", require_water}, {"rows", water_rows}, {"accepted_fragments", water_accepted},
        {"analytic_water_constructions", water_constructions}, {"sources", Json::array()}};
    for (const auto &[asset, product, lod] : water_sources)
        probe["water"]["sources"].push_back({{"asset_id", asset}, {"product", product}, {"lod", lod}});
    if (require_water) check(water_rows > 0 && water_accepted > 0 && water_constructions > 0 && !water_sources.empty(),
        "gameplay fixture did not execute the actual mesh22/material1 water closure");
    probe["coverage"] = {{"mixture32_executed", totals[20] != 0},
        {"coated_prefiltered_executed", totals[28] != 0},
        {"mixture32_reason", totals[20] ? "observed actual branch/sample counters" :
            mode == hs::HeroWorkDiagnosticMode::Full ? "fixture did not cover this authored branch" : "deliberately bypassed by diagnostic mode"},
        {"uncalibrated_samples", uncalibrated}, {"mesh_statistics_unavailable_samples", mesh_statistics_na},
        {"mesh_statistics_na_reason", "AS/MS native statistics require device support and are not implied by indexed Hero draws"}};
    probe["failures"] = failures;
    probe["valid"] = failures.empty();
    if (!failures.empty()) throw std::runtime_error("Hero work functional probe invariant failed; raw samples retained in result.json");
}

bool ValidateHeroWaterShadowProbe(Json &probe, const std::map<std::uint64_t, hs::GpuPassTimings> &history)
{
    Json failures = Json::array();
    const auto check = [&](bool valid, std::string_view reason) { if (!valid) failures.push_back(reason); };
    bool first_positive{}, last_positive{}, mismatch_fallback{}, restored_positive{true};
    const bool require_transition = probe.value("require_positive_transition", false);
    for (auto &row : probe["frames"])
    {
        const auto frame = row.at("submission_id").get<std::uint64_t>();
        const auto found = history.find(frame);
        if (found == history.end()) { failures.push_back("missing exact completed GPU frame"); continue; }
        const auto &gpu = found->second;
        check(gpu.valid && gpu.fence_completed && gpu.view_id == row.at("view_id").get<std::uint32_t>(),
            "shadow receipt requires exact completed frame/view identity");
        const auto seen = [&](std::uint32_t id) {
            return std::ranges::any_of(gpu.passes, [&](const auto &pass) { return pass.pass_id == id && pass.measured; });
        };
        const bool csm = seen(2), bank = seen(152), publish = seen(154), ready = seen(155);
        // Copy queues can complete commands without a timestamp-query capability.
        const auto seed_pass = std::ranges::find_if(gpu.passes, [](const auto &pass) {
            return pass.pass_id == 226 && pass.queue == hs::RenderQueue::Copy;
        });
        const bool seed = gpu.valid && gpu.fence_completed && seed_pass != gpu.passes.end();
        const bool root_commands = std::ranges::any_of(gpu.passes, [](const auto &pass) {
            return pass.pass_id == 228 && (pass.queue == hs::RenderQueue::Compute || pass.queue == hs::RenderQueue::Copy);
        });
        const bool root_draw = seen(229), fixed_root = root_commands && root_draw;
        check(root_commands == root_draw, "fixed-root command generation and depth producer must both complete");
        row["gpu_completed"] = gpu.valid && gpu.fence_completed;
        row["csm_pass2_present"] = csm;
        row["vsm_static_bank_completed"] = bank; row["vsm_publish_completed"] = publish;
        row["vsm_shadow_data_ready_completed"] = ready; row["coarse_seed_upload_completed"] = seed;
        row["coarse_seed_upload_timestamp_measured"] = seed && seed_pass->measured;
        row["fixed_root_commands_completed"] = root_commands;
        row["fixed_root_depth_completed"] = root_draw;
        row["completed_passes"] = Json::array();
        for (const auto &pass : gpu.passes)
            row["completed_passes"].push_back({{"pass_id", pass.pass_id}, {"name", pass.name},
                {"measured", pass.measured}, {"nanoseconds", pass.nanoseconds}, {"queue", static_cast<unsigned>(pass.queue)}});
        const bool prepared = row.at("cpu_plan_prepared").get<bool>(), required = row.at("csm_required").get<bool>();
        const bool positive = prepared && !required;
        if (positive)
        {
            check(!csm, "CPU approved frame still executed the CSM producer");
            check(row.at("guarantee_reason_bits").get<std::uint32_t>() == 1 &&
                row.at("baked_active").get<bool>() &&
                (fixed_root || (row.at("required_coarse_pages").get<std::uint32_t>() != 0 &&
                 row.at("baked_pages").get<std::uint32_t>() >= row.at("required_coarse_pages").get<std::uint32_t>())) &&
                row.at("baked_staging_bytes").get<std::uint64_t>() != 0 && bank && publish && ready,
                "approved coarse path lacks completed resource producer/publication evidence");
        }
        else check(csm, "conservative fallback did not execute the actual CSM producer");
        const auto ordinal = row.at("fixture_frame").get<std::uint32_t>();
        if (ordinal == 1) { first_positive = positive; if (positive && !fixed_root) check(seed, "first positive frame did not complete coarse seed upload"); }
        if (ordinal == 2) { mismatch_fallback = prepared && required && csm; check(!positive && csm, "changed light did not use real fallback"); }
        if (ordinal >= 3 && !positive) restored_positive = false;
        last_positive = positive;
    }
    const bool covered = first_positive && mismatch_fallback && last_positive && restored_positive;
    if (require_transition)
        check(covered && probe["frames"].size() == 4, "dedicated shadow transition requires baseline approval, real light-mismatch fallback and both restored frames approved; inspect per-frame reason bits");
    probe["policy_and_producer_consistent"] = failures.empty();
    probe["positive_fallback_positive_covered"] = covered;
    probe["uncovered_reason"] = covered ? "" :
        "The actual supported-view CPU guarantee was not positive at baseline and restoration; raw reason bits remain visible. No far-plane or quality limit was changed to force approval.";
    probe["descriptor_content_valid"] = nullptr;
    probe["descriptor_limit"] = "No public descriptor-content/readback validator; completed VSM ShadowDataReady/Publish are command/fence evidence, not direct descriptor-byte proof.";
    probe["timing_interpretation"] = "Functional producer/lifetime evidence under GPU competition; no FPS/adoption comparison.";
    probe["failures"] = failures;
    return failures.empty();
}

enum class GridAaMode
{
    Taa,
    Fxaa,
    Off,
};

const char *GridAaModeName(GridAaMode mode)
{
    switch (mode)
    {
    case GridAaMode::Taa: return "taa";
    case GridAaMode::Fxaa: return "fxaa";
    case GridAaMode::Off: return "off";
    }
    return "taa";
}

Json LightingModesReceipt(const hs::RendererLightingModes &modes)
{
    return {
        {"ao_mode", std::string(hs::GraphicsEnumToken(modes.ao_mode))},
        {"gi_mode", std::string(hs::GraphicsEnumToken(modes.gi_mode))},
        {"reflection_mode", std::string(hs::GraphicsEnumToken(modes.reflection_mode))},
        {"shadow_mode", std::string(hs::GraphicsEnumToken(modes.shadow_mode))},
    };
}

Json LightingStatusReceipt(const hs::RendererLightingStatus &status)
{
    return {
        {"requested", LightingModesReceipt(status.requested)},
        {"active", LightingModesReceipt(status.active)},
        {"fallback", {
            {"ao", status.ao_fallback_reason},
            {"gi", status.gi_fallback_reason},
            {"reflection", status.reflection_fallback_reason},
            {"shadow", status.shadow_fallback_reason},
        }},
        {"software_gi", {{"compiled", status.software_gi.compiled}, {"supported", status.software_gi.supported},
            {"enabled", status.software_gi.enabled}, {"native_ready", status.software_gi.native_ready},
            {"faulted", status.software_gi.faulted}, {"static_instances", status.software_gi.static_instances},
            {"dynamic_instances", status.software_gi.dynamic_instances},
            {"unsupported_masked", status.software_gi.unsupported_masked},
            {"unsupported_thin", status.software_gi.unsupported_thin},
            {"excluded_translucent", status.software_gi.excluded_translucent},
            {"physical_cascades", status.software_gi.physical_cascades},
            {"allocated_bytes", status.software_gi.allocated_bytes}, {"scratch_bytes", status.software_gi.scratch_bytes},
            {"reason", status.software_gi.reason}}},
        {"local_shadows", {{"enabled", status.local_shadows.enabled},
            {"requested_lights", status.local_shadows.requested_lights}, {"selected_lights", status.local_shadows.selected_lights},
            {"unsupported_lights", status.local_shadows.unsupported_lights}, {"budget_lights", status.local_shadows.budget_lights},
            {"overflow_lights", status.local_shadows.overflow_lights}, {"allocated_bytes", status.local_shadows.allocated_bytes},
            {"reason", status.local_shadows.reason}}},
        {"shadow_cache", {{"requested_bytes", status.shadow_cache.requested_bytes},
            {"admitted_bytes", status.shadow_cache.admitted_bytes}, {"physical_pages", status.shadow_cache.physical_pages},
            {"baked_active", status.shadow_cache.baked_active}, {"baked_pages", status.shadow_cache.baked_pages},
            {"baked_staging_bytes", status.shadow_cache.baked_staging_bytes}}},
    };
}

Json PipelineCacheReceipt(const hs::RendererPsoCacheStats &stats)
{
    return {
        {"hits", stats.hits},
        {"misses", stats.misses},
        {"native_creates", stats.native_creates},
        {"stores", stats.stores},
        {"store_failures", stats.store_failures},
        {"load_failures", stats.load_failures},
        {"io_failures", stats.io_failures},
        {"discarded_files", stats.discarded_files},
        {"unsupported_libraries", stats.unsupported_libraries},
        {"uncached_mesh_creates", stats.uncached_mesh_creates},
        {"root_registrations", stats.root_registrations},
        {"key_failures", stats.key_failures},
        {"loaded_bytes", stats.loaded_bytes},
        {"serialized_bytes", stats.serialized_bytes},
        {"initialized", stats.initialized},
        {"library_supported", stats.library_supported},
        {"mesh_library_supported", stats.mesh_library_supported},
        {"persistence_enabled", stats.persistence_enabled},
        {"last_cache_hresult", stats.last_cache_hresult},
        {"last_io_error", stats.last_io_error},
    };
}

bool HasGpuPass(const hs::GpuPassTimings &timings, std::uint32_t pass_id,
                std::string_view name)
{
    if (!timings.valid)
        return false;
    return std::any_of(timings.passes.begin(), timings.passes.end(),
        [&](const hs::GpuPassTiming &pass) {
            return pass.pass_id == pass_id && pass.name == name;
        });
}

const char *GraphicsCapabilityReasonName(hs::GraphicsCapabilityReason reason)
{
    switch (reason)
    {
    case hs::GraphicsCapabilityReason::None: return "none";
    case hs::GraphicsCapabilityReason::Unsupported: return "unsupported";
    case hs::GraphicsCapabilityReason::Unavailable: return "unavailable";
    case hs::GraphicsCapabilityReason::MissingProvider: return "missing_provider";
    case hs::GraphicsCapabilityReason::MissingDisplay: return "missing_display";
    case hs::GraphicsCapabilityReason::Incompatible: return "incompatible";
    case hs::GraphicsCapabilityReason::BudgetLimited: return "budget_limited";
    case hs::GraphicsCapabilityReason::Rejected: return "rejected";
    }
    return "unknown";
}

std::string_view NativeReconstructionPassName(std::string_view provider)
{
    if (provider == "dlss") return "dlss Reconstruction";
    if (provider == "xess") return "xess Reconstruction";
    if (provider == "fsr") return "fsr Reconstruction";
    if (provider == "directsr") return "directsr Reconstruction";
    return {};
}

void Require(const hs::Result &result)
{
    if (!result)
        throw std::runtime_error(std::string(result.Subsystem()) + ": " +
                                 std::string(result.Message()));
}

// Explicit developer composition: the backend receives bytes, never a filename.
std::vector<std::byte> ReadDeveloperFont()
{
    hs::ShippedContentOwner file;
    Require(hs::ShippedContentOwner::Open(hs::CurrentExecutableDirectory() / L"Fonts" / L"NotoSansKR.ttf", file));
    if (!file.Size() || file.Size() > UINT32_MAX) throw std::runtime_error("Developer font extent is invalid");
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.Size()));
    Require(file.ReadExact(0, bytes));
    return bytes;
}

struct TestWindow
{
    HWND handle{};
    HINSTANCE instance{GetModuleHandleW(nullptr)};
    bool registered{};
    static constexpr auto class_name = L"ProjectHSRenderTest";

    void Create(std::uint32_t width, std::uint32_t height)
    {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = instance;
        window_class.lpszClassName = class_name;
        if (!RegisterClassW(&window_class))
            throw std::runtime_error("RegisterClassW failed");
        registered = true;
        RECT rectangle{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        if (!AdjustWindowRect(&rectangle, WS_OVERLAPPEDWINDOW, FALSE))
            throw std::runtime_error("AdjustWindowRect failed");
        handle = CreateWindowExW(0, class_name, L"Project HS render validation",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
            nullptr, nullptr, instance, nullptr);
        if (!handle)
            throw std::runtime_error("CreateWindowExW failed");
    }

    ~TestWindow()
    {
        if (handle) DestroyWindow(handle);
        if (registered) UnregisterClassW(class_name, instance);
    }
};

std::uint32_t Number(std::string_view value, std::uint32_t minimum,
                     std::uint32_t maximum)
{
    std::uint32_t number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        number < minimum || number > maximum)
        throw std::runtime_error("invalid numeric argument: " + std::string(value));
    return number;
}

std::shared_ptr<const hs::StaticRenderScene> Scene(std::string_view generation)
{
    auto scene = std::make_unique<hs::StaticRenderSceneStorage>();
    scene->content_generation_id = generation;
    std::uint64_t identity = 100;
    const auto add = [&](hs::RenderMesh mesh, hs::Float3 position,
                         hs::Float3 scale, std::uint32_t variant) {
        hs::RenderInstance instance;
        instance.mesh = mesh;
        instance.position = position;
        instance.scale = scale;
        instance.environment_variant = variant;
        instance.environment_seed = 0x48535254u;
        instance.stable_id = identity++;
        scene->instances.push_back(instance);
    };
    add(hs::RenderMesh::Ground, {0, -.055f, 0}, {50, .1f, 50}, 0);
    for (std::uint32_t variant = 0; variant < 3; ++variant)
    {
        const auto x = static_cast<float>(variant) * 5.f - 5.f;
        add(hs::RenderMesh::TreeTrunk, {x, 0, 3}, {1, 1, 1}, variant);
        add(hs::RenderMesh::TreeCanopy, {x, 0, 3}, {1, 1, 1}, variant);
    }
    for (std::uint32_t variant = 0; variant < 4; ++variant)
    {
        const auto x = static_cast<float>(variant) * 3.f - 4.5f;
        add(hs::RenderMesh::Rock, {x, 0, -3}, {.8f, .8f, .8f}, variant);
        add(hs::RenderMesh::Grass, {x, 0, -6}, {1, 1, 1}, variant);
    }
    return hs::FreezeStaticRenderScene(std::move(scene));
}

hs::AssetId SelectedArcherAsset(const hs::SelectedGeneration &generation)
{
    constexpr std::string_view logical_id = "render/character/archer.rpkg";
    std::filesystem::path path;
    Require(generation.Require(logical_id, path));
    const auto digest = generation.product_digests.find(std::string(logical_id));
    if (digest == generation.product_digests.end() || digest->second.empty())
        throw std::runtime_error("selected Generation lacks the authenticated Archer character digest");
    return hs::MakeAssetId(logical_id);
}

Json DiagnosticsPercentilesReceipt(const hs::RendererDiagnosticsPercentiles &values)
{
    return {{"available", values.available}, {"samples", values.samples},
        {"method", "nearest_rank_bounded_completed_frame_window"},
        {"p50", values.available ? Json(values.p50) : Json(nullptr)},
        {"p95", values.available ? Json(values.p95) : Json(nullptr)},
        {"p99", values.available ? Json(values.p99) : Json(nullptr)}};
}

Json DiagnosticsCadenceReceipt(const hs::RendererDiagnosticsCadence &cadence)
{
    return {{"available", cadence.available}, {"source", cadence.source},
        {"unavailable_reason", cadence.unavailable_reason}, {"frames", cadence.frames},
        {"fps", cadence.available ? Json(cadence.fps) : Json(nullptr)},
        {"observed_elapsed_ms", cadence.available ? Json(cadence.elapsed_ms) : Json(nullptr)},
        {"pacing_ms", DiagnosticsPercentilesReceipt(cadence.pacing_ms)}};
}

Json DiagnosticsMemoryReceipt(const hs::RendererDiagnosticsMemory &memory)
{
    constexpr std::array names{"texture", "geometry", "skin", "shadow", "gi",
        "history", "transient", "sdk", "staging"};
    Json categories = Json::object();
    for (std::size_t index = 0; index < names.size(); ++index)
        categories[names[index]] = {{"live_bytes", memory.live_bytes[index]},
            {"retiring_bytes", memory.retiring_bytes[index]},
            {"local_live_bytes", memory.local_live_bytes[index]},
            {"local_retiring_bytes", memory.local_retiring_bytes[index]},
            {"nonlocal_live_bytes", memory.nonlocal_live_bytes[index]},
            {"nonlocal_retiring_bytes", memory.nonlocal_retiring_bytes[index]}};
    const auto observation_status = [](auto status) -> std::string_view {
        using Status = hs::RendererDiagnosticsMemory::ObservationStatus;
        switch (status)
        {
        case Status::Fresh: return "fresh";
        case Status::Cached: return "cached";
        case Status::Failed: return "failed";
        case Status::Unsupported: return "unsupported";
        default: return "unavailable";
        }
    };
    return {
        {"dxgi_local", {{"available", memory.budget_available},
            {"status", observation_status(memory.local_status)},
            {"success_sequence", memory.local_success_sequence}, {"age_ms", memory.local_observed_age_ms},
            {"last_error_hresult", memory.local_last_error},
            {"usage_bytes", memory.budget_available ? Json(memory.local_usage_bytes) : Json(nullptr)},
            {"budget_bytes", memory.budget_available ? Json(memory.local_budget_bytes) : Json(nullptr)}}},
        {"dxgi_nonlocal", {{"available", memory.nonlocal_budget_available},
            {"status", observation_status(memory.nonlocal_status)},
            {"success_sequence", memory.nonlocal_success_sequence}, {"age_ms", memory.nonlocal_observed_age_ms},
            {"last_error_hresult", memory.nonlocal_last_error},
            {"usage_bytes", memory.nonlocal_budget_available ? Json(memory.nonlocal_usage_bytes) : Json(nullptr)},
            {"budget_bytes", memory.nonlocal_budget_available ? Json(memory.nonlocal_budget_bytes) : Json(nullptr)}}},
        {"host_memory", {{"available", memory.host_budget_available},
            {"status", observation_status(memory.host_status)},
            {"success_sequence", memory.host_success_sequence}, {"age_ms", memory.host_observed_age_ms},
            {"last_error", memory.host_last_error},
            {"staging_budget_bytes", memory.host_budget_available ? Json(memory.host_staging_budget_bytes) : Json(nullptr)}}},
        {"tracked_partial", {{"categories", std::move(categories)},
            {"live_bytes", memory.tracked_live_bytes}, {"retiring_bytes", memory.tracked_retiring_bytes},
            {"gpu_bytes", memory.tracked_gpu_bytes}, {"reclaimed_bytes", memory.tracked_reclaimed_bytes},
            {"gpu_reserved_bytes", memory.gpu_reserved_bytes},
            {"gpu_growth_since_budget_bytes", memory.gpu_growth_since_budget_bytes},
            {"local_gpu_reserved_bytes", memory.local_gpu_reserved_bytes},
            {"nonlocal_gpu_reserved_bytes", memory.nonlocal_gpu_reserved_bytes},
            {"local_growth_since_budget_bytes", memory.local_growth_since_budget_bytes},
            {"nonlocal_growth_since_budget_bytes", memory.nonlocal_growth_since_budget_bytes},
            {"host_staging_bytes", memory.host_staging_bytes}, {"host_reserved_bytes", memory.host_reserved_bytes},
            {"host_staging_budget_bytes", memory.host_budget_available ? Json(memory.host_staging_budget_bytes) : Json(nullptr)}}},
        {"allocator_stats", {{"available", memory.allocator_stats_available},
            {"local_block_bytes", memory.allocator_stats_available ? Json(memory.allocator_local_block_bytes) : Json(nullptr)},
            {"nonlocal_block_bytes", memory.allocator_stats_available ? Json(memory.allocator_nonlocal_block_bytes) : Json(nullptr)},
            {"local_usage_estimate_bytes", memory.allocator_stats_available ? Json(memory.allocator_local_usage_estimate_bytes) : Json(nullptr)},
            {"nonlocal_usage_estimate_bytes", memory.allocator_stats_available ? Json(memory.allocator_nonlocal_usage_estimate_bytes) : Json(nullptr)},
            {"sums_with_dxgi", false}}},
        {"texture_physical_bytes", memory.texture_physical_bytes},
        {"known_provider_allocations", {{"presentation_bytes", memory.presentation_provider_bytes},
            {"reconstruction_guide_bytes", memory.reconstruction_guide_bytes},
            {"renderer_view_bytes", memory.renderer_view_bytes_available ? Json(memory.renderer_view_bytes) : Json(nullptr)},
            {"renderer_view_bytes_available", memory.renderer_view_bytes_available},
            {"shared_deformation_bytes", memory.shared_deformation_bytes_available ? Json(memory.shared_deformation_bytes) : Json(nullptr)},
            {"shared_deformation_bytes_available", memory.shared_deformation_bytes_available}}}};
}

Json DiagnosticsReceipt(const hs::RendererDiagnosticsSnapshot &diagnostics)
{
    const auto measurements = [](const hs::RendererDiagnosticsMeasurementSet &values) {
        return Json{{"cpu_render_ms", DiagnosticsPercentilesReceipt(values.cpu_render_ms)},
            {"gpu_critical_path_ms", DiagnosticsPercentilesReceipt(values.gpu_critical_path_ms)},
            {"present_call_ms", DiagnosticsPercentilesReceipt(values.present_call_ms)},
            {"submit_to_present_return_ms", DiagnosticsPercentilesReceipt(values.submit_to_present_return_ms)},
            {"rendered_cadence", DiagnosticsCadenceReceipt(values.rendered_cadence)},
            {"present_call_cadence", DiagnosticsCadenceReceipt(values.present_call_cadence)}};
    };
    Json frames = Json::array(), passes = Json::array(), views = Json::array();
    for (const auto &view : diagnostics.views)
        views.push_back({{"view_id", view.view_id}, {"presenting", view.presenting},
            {"validation_on", measurements(view.validation_on)},
            {"validation_off", measurements(view.validation_off)}});
    for (const auto &frame : diagnostics.completed_frames)
        frames.push_back({{"rendered_frame_id", frame.rendered_frame_id},
            {"frame_kind", "application_render"}, {"fence_completed", frame.fence_completed},
            {"view_id", frame.view_id}, {"view_render_sequence", frame.view_render_sequence},
            {"presented", frame.presented},
            {"validation_enabled", frame.validation_enabled}, {"gpu_validation_enabled", frame.gpu_validation_enabled},
            {"cpu_render_ms", frame.cpu_recording_available ? Json(frame.cpu_recording_ms) : Json(nullptr)},
            {"gpu_critical_path_ms", frame.gpu_critical_path_available ? Json(frame.gpu_critical_path_ms) : Json(nullptr)},
            {"submit_qpc", frame.submit_qpc}, {"present_begin_qpc", frame.present_begin_qpc},
            {"present_end_qpc", frame.present_end_qpc}, {"qpc_frequency", frame.qpc_frequency},
            {"queue_execution", {{"direct_batches", frame.queues.direct_batches},
                {"compute_batches", frame.queues.compute_batches}, {"copy_batches", frame.queues.copy_batches},
                {"cross_queue_waits", frame.queues.cross_queue_waits}, {"record_jobs", frame.queues.record_jobs}}},
            {"transient_memory", {{"backing_bytes", frame.queues.transient_backing_bytes},
                {"unaliased_bytes", frame.queues.transient_bytes_without_aliasing}}},
            {"memory", DiagnosticsMemoryReceipt(frame.memory)}});
    for (const auto &pass : diagnostics.passes)
        passes.push_back({{"pass_id", pass.pass_id}, {"name", pass.name},
            {"queue", static_cast<unsigned>(pass.queue)}, {"measured", pass.measured},
            {"clock_domain", pass.clock_domain}, {"clock_calibrated", pass.clock_calibrated},
            {"name_truncated", pass.name_truncated},
            {"nanoseconds", pass.measured ? Json(pass.nanoseconds) : Json(nullptr)},
            {"begin_ticks", pass.measured ? Json(pass.begin_ticks) : Json(nullptr)},
            {"end_ticks", pass.measured ? Json(pass.end_ticks) : Json(nullptr)},
            {"timestamp_frequency", pass.measured ? Json(pass.timestamp_frequency) : Json(nullptr)},
            {"begin_ms", pass.measured ? Json(pass.begin_ms) : Json(nullptr)},
            {"end_ms", pass.measured ? Json(pass.end_ms) : Json(nullptr)}});
    return {{"schema_version", 1}, {"frame_capacity", diagnostics.frame_capacity},
        {"pass_capacity", diagnostics.pass_capacity}, {"total_rendered_frames", diagnostics.total_rendered_frames},
        {"total_completed_frames", diagnostics.total_completed_frames},
        {"duplicate_samples", diagnostics.duplicate_samples}, {"rejected_samples", diagnostics.rejected_samples},
        {"sources", {{"cpu_render", diagnostics.cpu_render_source},
            {"gpu_critical_path", diagnostics.gpu_critical_path_source}, {"latency", diagnostics.latency_source},
            {"memory", diagnostics.memory_source}}},
        {"validation_on", measurements(diagnostics.validation_on)},
        {"validation_off", measurements(diagnostics.validation_off)},
        {"views", std::move(views)},
        {"displayed_cadence", DiagnosticsCadenceReceipt(diagnostics.displayed_cadence)},
        {"input_to_display_latency", {{"available", diagnostics.input_to_display_latency_available},
            {"ms", nullptr}, {"unavailable_reason", diagnostics.input_to_display_latency_reason}}},
        {"completed_frames", std::move(frames)},
        {"timeline", {{"completed_frame_id", diagnostics.timeline_completed_frame_id},
            {"view_id", diagnostics.timeline_view_id},
            {"truncated", diagnostics.timeline_truncated}, {"passes", std::move(passes)}}},
        {"memory", {{"current", DiagnosticsMemoryReceipt(diagnostics.current_memory)},
            {"sampled_peak", {{"source", "componentwise_maximum_observed_at_frame_and_transition_boundaries"},
                {"values", DiagnosticsMemoryReceipt(diagnostics.sampled_memory_peak)}}},
            {"transition_peak", {{"available", diagnostics.transition_memory_peak_available},
                {"source", diagnostics.transition_memory_peak_source},
                {"unavailable_reason", diagnostics.transition_memory_peak_reason},
                {"values", diagnostics.transition_memory_peak_available ? DiagnosticsMemoryReceipt(diagnostics.transition_memory_peak) : Json(nullptr)}}},
            {"complete_provider_memory_available", diagnostics.complete_provider_memory_available},
            {"provider_memory_reason", diagnostics.provider_memory_reason}}},
        {"streaming", {{"misses_available", diagnostics.stream_misses_available},
            {"misses", nullptr}, {"misses_reason", diagnostics.stream_misses_reason},
            {"texture_requests", diagnostics.streaming.streaming_requests},
            {"texture_uploads", diagnostics.streaming.streaming_uploads},
            {"texture_publications", diagnostics.streaming.streaming_publications},
            {"geometry_requests", diagnostics.streaming.geometry_streaming_requests},
            {"geometry_uploads", diagnostics.streaming.geometry_streaming_uploads},
            {"geometry_publications", diagnostics.streaming.geometry_streaming_publications}}},
        {"providers", {{"reconstruction", diagnostics.reconstruction.effective_provider.value},
            {"presentation", diagnostics.presentation.effective_provider.value},
            {"observed_multiplier", diagnostics.presentation.observed_multiplier},
            {"multiplier_source", "native_presentation_status_not_displayed_cadence"}}}};
}

Json WorldTransportReceipt(const std::map<std::uint64_t, hs::GpuPassTimings> &history)
{
    constexpr std::array<std::pair<std::uint32_t, std::string_view>, 22> expected{{
        {170, "VolumetricDensity"}, {171, "VolumetricLighting"},
        {172, "VolumetricTemporal"}, {173, "VolumetricSpatial"},
        {174, "VolumetricIntegrate"}, {175, "VolumetricResolve"},
        {180, "LocalProbeStaticCubeCapture"}, {181, "LocalProbeStaticCubeCapture"},
        {182, "LocalProbeStaticCubeCapture"}, {183, "LocalProbeStaticCubeCapture"},
        {184, "LocalProbeStaticCubeCapture"}, {185, "LocalProbeStaticCubeCapture"},
        {186, "LocalProbePackageUpload"}, {187, "LocalProbeCosineShIntegration"},
        {188, "LocalProbeGgxPrefilter"},
        {190, "HlodUpload"}, {191, "HlodClear"}, {192, "HlodSelect"},
        {193, "HlodEmit"}, {194, "HlodPublish"}, {195, "HlodHistory"},
        {196, "HlodDemandStatusReadback"},
    }};
    Json output{{"completed_frame_history", Json::array()},
                {"required_passes", Json::array()}, {"layout_accepted", false}};
    for (const auto &[frame_id, timings] : history)
    {
        Json passes = Json::array();
        for (const auto &pass : timings.passes)
            passes.push_back({{"pass_id", pass.pass_id}, {"name", pass.name},
                {"nanoseconds", pass.nanoseconds}, {"measured", pass.measured},
                {"queue", static_cast<unsigned>(pass.queue)}});
        output["completed_frame_history"].push_back(
            {{"completed_frame_id", frame_id}, {"passes", std::move(passes)}});
    }
    bool complete = true;
    bool hlod_complete = true;
    for (const auto &[pass_id, name] : expected)
    {
        Json observed = Json::array();
        bool present = false;
        for (const auto &[frame_id, timings] : history)
            for (const auto &pass : timings.passes)
                if (pass.pass_id == pass_id && pass.name == name)
                {
                    // The shared copy queue does not support timestamps. Its
                    // entries still identify actual completed graph passes.
                    present |= pass.measured || pass.queue == hs::RenderQueue::Copy;
                    observed.push_back({{"completed_frame_id", frame_id},
                        {"nanoseconds", pass.nanoseconds}, {"measured", pass.measured}});
                }
        complete &= present;
        if (pass_id >= 190) hlod_complete &= present;
        output["required_passes"].push_back({{"pass_id", pass_id},
            {"name", std::string(name)}, {"present", present},
            {"observed", std::move(observed)}});
    }
    output["layout_accepted"] = hlod_complete;
    output["required_passes_complete"] = complete;
    return output;
}

std::shared_ptr<const hs::StaticRenderScene> GridFloorScene(std::string_view generation)
{
    auto scene = std::make_unique<hs::StaticRenderSceneStorage>();
    scene->content_generation_id = generation;
    hs::RenderInstance ground;
    ground.mesh = hs::RenderMesh::Ground;
    ground.position = {0, -.055f, 0};
    ground.scale = {50, .1f, 50};
    ground.environment_seed = 0x47524944u;
    ground.stable_id = 0x4752494400000001ull;
    scene->instances.push_back(ground);
    return hs::FreezeStaticRenderScene(std::move(scene));
}

std::shared_ptr<const hs::StaticRenderScene> HzbScene(std::string_view generation)
{
    auto scene = std::make_unique<hs::StaticRenderSceneStorage>();
    scene->content_generation_id = generation;
    hs::RenderInstance target;
    target.mesh = hs::RenderMesh::Rock;
    target.position = {0, 0, 4};
    target.scale = {.35f, .35f, .35f};
    target.environment_variant = 0;
    target.environment_seed = 0x485A4254u;
    target.stable_id = 0x485A4201ull;
    scene->instances.push_back(target);
    return hs::FreezeStaticRenderScene(std::move(scene));
}

struct DynamicUploadRow
{
    std::uint32_t frame{};
    std::size_t instances{};
    std::size_t uploaded_bytes{};
    std::size_t copy_regions{};
    std::size_t capacity_bytes{};
    std::size_t stable_slots{};
    std::size_t slot_count{};
    std::size_t anonymous_instances{};
    std::size_t created{};
    std::size_t updated{};
    std::size_t destroyed{};
    std::size_t skin_cache_active{};
    std::size_t skin_history_valid{};
    std::size_t skin_gpu_dispatches{};
    std::size_t skin_gpu_history_reads{};
    std::size_t dynamic_visibility_refs{};
    std::size_t hzb_history_input{};
};

std::uint64_t CsvNumber(std::string_view value)
{
    std::uint64_t number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid dynamic instance upload CSV value");
    return number;
}

std::vector<DynamicUploadRow> DynamicUploadRows(const std::filesystem::path &path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("dynamic instance upload diagnostics are missing");
    std::string line;
    if (!std::getline(input, line) ||
        line != "frame,instances,uploaded_bytes,copy_regions,capacity_bytes,stable_slots,slot_count,anonymous_instances,created,updated,destroyed,skin_cache_active,skin_history_valid,skin_gpu_dispatches,skin_gpu_history_reads,dynamic_visibility_refs,hzb_history_input")
        throw std::runtime_error("dynamic instance upload diagnostics have an unexpected schema");

    std::vector<DynamicUploadRow> rows;
    while (std::getline(input, line))
    {
        if (line.empty()) continue;
        std::array<std::string_view, 17> fields{};
        std::size_t field{};
        std::size_t begin{};
        for (; field + 1 < fields.size(); ++field)
        {
            const auto separator = line.find(',', begin);
            if (separator == std::string::npos) break;
            fields[field] = std::string_view(line).substr(begin, separator - begin);
            begin = separator + 1;
        }
        if (field != fields.size() - 1 || line.find(',', begin) != std::string::npos)
            throw std::runtime_error("dynamic instance upload diagnostics row is malformed");
        fields.back() = std::string_view(line).substr(begin);
        const auto value = [&](std::size_t index) { return CsvNumber(fields[index]); };
        rows.push_back({
            static_cast<std::uint32_t>(value(0)),
            static_cast<std::size_t>(value(1)),
            static_cast<std::size_t>(value(2)),
            static_cast<std::size_t>(value(3)),
            static_cast<std::size_t>(value(4)),
            static_cast<std::size_t>(value(5)),
            static_cast<std::size_t>(value(6)),
            static_cast<std::size_t>(value(7)),
            static_cast<std::size_t>(value(8)),
            static_cast<std::size_t>(value(9)),
            static_cast<std::size_t>(value(10)),
            static_cast<std::size_t>(value(11)),
            static_cast<std::size_t>(value(12)),
            static_cast<std::size_t>(value(13)),
            static_cast<std::size_t>(value(14)),
            static_cast<std::size_t>(value(15)),
            static_cast<std::size_t>(value(16)),
        });
    }
    return rows;
}

void ValidatePersistentSceneProbe(const std::vector<DynamicUploadRow> &rows,
                                  std::uint32_t frames, Json &receipt)
{
    if (frames < 9 || rows.size() != frames)
        throw std::runtime_error("persistent GPU scene probe requires one diagnostics row per frame");
    if (rows[0].uploaded_bytes == 0 || rows[0].uploaded_bytes % 2 != 0)
        throw std::runtime_error("persistent GPU scene probe initial upload is invalid");
    const auto instance_bytes = rows[0].uploaded_bytes / 2;
    const auto expect = [&](std::size_t index, std::size_t instances,
                            std::size_t uploaded_bytes, std::size_t copy_regions,
                            std::size_t active_slots, std::size_t slot_count,
                            std::size_t created, std::size_t updated,
                            std::size_t destroyed, std::size_t skin_active,
                            std::size_t skin_history,
                            std::size_t dynamic_visibility_refs,
                            std::size_t hzb_history_input) {
        const auto &row = rows[index];
        if (row.frame != index + 1 || row.instances != instances ||
            row.uploaded_bytes != uploaded_bytes || row.copy_regions != copy_regions ||
            row.stable_slots != active_slots || row.slot_count != slot_count ||
            row.anonymous_instances != 0 || row.created != created ||
            row.updated != updated || row.destroyed != destroyed ||
            row.skin_cache_active != skin_active ||
            row.skin_history_valid != skin_history ||
            row.dynamic_visibility_refs != dynamic_visibility_refs ||
            row.hzb_history_input != hzb_history_input)
            throw std::runtime_error("persistent GPU scene probe diagnostics mismatch");
    };

    expect(0, 2, instance_bytes * 2, 1, 2, 2, 2, 0, 0, 1, 0, 2, 0);
    expect(1, 2, 0, 0, 2, 2, 0, 0, 0, 1, 1, 2, 1);
    expect(2, 2, instance_bytes, 1, 2, 2, 0, 1, 0, 1, 1, 2, 1);
    expect(3, 1, 0, 0, 1, 2, 0, 0, 1, 1, 1, 1, 1);
    expect(4, 2, instance_bytes, 1, 2, 2, 1, 0, 0, 1, 1, 2, 1);
    expect(5, 2, 0, 0, 2, 2, 0, 0, 0, 1, 1, 2, 1);
    if (rows[0].skin_gpu_dispatches != 1 || rows[0].skin_gpu_history_reads != 0)
        throw std::runtime_error("skin cache first frame GPU history contract is invalid");
    for (std::size_t index = 1; index < 6; ++index)
        if (rows[index].skin_gpu_dispatches != 1 || rows[index].skin_gpu_history_reads != 1)
            throw std::runtime_error("skin cache current/previous GPU deformation was not reused");

    receipt["persistent_gpu_scene"] = {
        {"initial_upload_bytes", rows[0].uploaded_bytes},
        {"unchanged_upload_bytes", rows[1].uploaded_bytes},
        {"single_update_upload_bytes", rows[2].uploaded_bytes},
        {"destroy_upload_bytes", rows[3].uploaded_bytes},
        {"reused_slot_upload_bytes", rows[4].uploaded_bytes},
        {"slot_count_after_reuse", rows[4].slot_count},
        {"anonymous_instances", rows[4].anonymous_instances},
        {"created_after_reuse", rows[4].created},
        {"destroyed_on_remove", rows[3].destroyed},
        {"skin_cache_active", rows[5].skin_cache_active},
        {"skin_history_valid", rows[5].skin_history_valid},
        {"skin_gpu_dispatches", rows[5].skin_gpu_dispatches},
        {"skin_gpu_history_reads", rows[5].skin_gpu_history_reads},
        {"dynamic_visibility_refs", rows[5].dynamic_visibility_refs},
        {"hzb_history_reset_on_scene_change", rows[6].hzb_history_input == 0},
        {"hzb_history_input_frame8", rows[7].hzb_history_input},
    };
    if (rows[6].hzb_history_input != 0 || rows[7].hzb_history_input != 1 ||
        rows[8].hzb_history_input != 1)
        throw std::runtime_error("HZB history reset/use sequence is invalid");
}

void ValidateMonsterSkinCacheProbe(const std::vector<DynamicUploadRow> &rows,
                                   std::uint32_t frames, Json &receipt)
{
    if (frames < 2 || rows.size() != frames)
        throw std::runtime_error("monster skin cache probe requires one diagnostics row per frame");
    if (rows[0].skin_cache_active != 3 || rows[0].skin_gpu_dispatches != 3 ||
        rows[0].skin_history_valid != 0 || rows[0].skin_gpu_history_reads != 0 ||
        rows[0].dynamic_visibility_refs != 3)
        throw std::runtime_error("monster skin cache first-frame GPU contract is invalid");
    if (rows[1].skin_cache_active != 3 || rows[1].skin_gpu_dispatches != 3 ||
        rows[1].skin_history_valid != 3 || rows[1].skin_gpu_history_reads != 3 ||
        rows[1].dynamic_visibility_refs != 3)
        throw std::runtime_error("monster skin cache current/previous GPU reuse is invalid");
    receipt["skin_cache_gpu"] = {
        {"monster_instances", rows[1].skin_cache_active},
        {"current_deformation_dispatches", rows[1].skin_gpu_dispatches},
        {"previous_deformation_reads", rows[1].skin_gpu_history_reads},
        {"history_valid", rows[1].skin_history_valid},
        {"gpu_visibility_refs", rows[1].dynamic_visibility_refs},
    };
}

void ValidateDynamicLodSkinGrowthProbe(const std::vector<DynamicUploadRow> &rows,
                                     std::uint32_t frames, Json &receipt)
{
    if (frames < 8 || rows.size() != frames)
        throw std::runtime_error("dynamic LOD/skin growth probe requires at least eight frames");
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const auto &row = rows[index];
        const auto skin_count = index < 2 ? 1u : 3u;
        const auto source_count = skin_count + 1;
        const auto history_count = index == 0 ? 0u : index == 2 ? 1u : skin_count;
        const auto visibility_refs = 1u + skin_count;
        // The two added slots acquire previous-skin bindings on their first
        // retained frame, then remain unchanged.
        const auto updated_count = index == 3 ? 2u : 0u;
        if (row.instances != source_count || row.stable_slots != source_count ||
            row.slot_count != source_count || row.anonymous_instances != 0 ||
            row.dynamic_visibility_refs != visibility_refs || row.skin_cache_active != skin_count ||
            row.skin_gpu_dispatches != skin_count || row.skin_history_valid != history_count ||
            row.skin_gpu_history_reads != history_count ||
            row.created != (index == 0 || index == 2 ? 2u : 0u) ||
            row.updated != updated_count || row.destroyed != 0)
            throw std::runtime_error("dynamic LOD/skin growth diagnostics mismatch");
    }
    receipt["dynamic_lod_skin_growth"] = {
        {"initial_gpu_visibility_refs", rows[0].dynamic_visibility_refs},
        {"grown_gpu_visibility_refs", rows[2].dynamic_visibility_refs},
        {"initial_canonical_instances", rows[0].stable_slots},
        {"initial_draw_rows", rows[0].instances},
        {"grown_skin_instances", rows[2].skin_cache_active},
        {"history_reads_after_growth", rows[3].skin_gpu_history_reads},
        {"frames", rows.size()},
    };
}

bool FilesEqual(const std::filesystem::path &left, const std::filesystem::path &right)
{
    std::ifstream first(left, std::ios::binary | std::ios::ate);
    std::ifstream second(right, std::ios::binary | std::ios::ate);
    if (!first || !second || first.tellg() != second.tellg()) return false;
    first.seekg(0);
    second.seekg(0);
    std::array<char, 64 * 1024> a{}, b{};
    while (first && second)
    {
        first.read(a.data(), static_cast<std::streamsize>(a.size()));
        second.read(b.data(), static_cast<std::streamsize>(b.size()));
        const auto count = first.gcount();
        if (count != second.gcount() ||
            !std::equal(a.begin(), a.begin() + count, b.begin()))
            return false;
    }
    return true;
}

struct HzbReferencePixels
{
    std::uint32_t width{}, height{};
    std::uint64_t nonzero_rgb_pixels{};
    bool pixel_variation{};
};

class ScopedComApartment
{
  public:
    ScopedComApartment()
    {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result_) && result_ != RPC_E_CHANGED_MODE)
            throw std::runtime_error("initialize COM for HZB reference decode failed");
    }
    ~ScopedComApartment()
    {
        if (SUCCEEDED(result_)) CoUninitialize();
    }

  private:
    HRESULT result_{E_FAIL};
};

HzbReferencePixels DecodeHzbReferencePixels(const std::filesystem::path &path)
{
    ScopedComApartment com;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        throw std::runtime_error("create WIC factory for HZB reference decode failed");
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnLoad, &decoder)))
        throw std::runtime_error("open HZB reference PNG failed");
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        throw std::runtime_error("read HZB reference PNG frame failed");
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        throw std::runtime_error("convert HZB reference PNG to RGBA failed");

    HzbReferencePixels result;
    if (FAILED(converter->GetSize(&result.width, &result.height)) ||
        !result.width || !result.height || result.width > UINT_MAX / 4u)
        throw std::runtime_error("HZB reference PNG has an invalid extent");
    if (static_cast<std::size_t>(result.height) >
        std::numeric_limits<std::size_t>::max() / result.width)
        throw std::runtime_error("HZB reference PNG pixel count overflows");
    const auto pixel_count = static_cast<std::size_t>(result.width) * result.height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4u ||
        pixel_count * 4u > std::numeric_limits<UINT>::max())
        throw std::runtime_error("HZB reference PNG is too large to decode");
    std::vector<BYTE> pixels(pixel_count * 4u);
    if (FAILED(converter->CopyPixels(nullptr, result.width * 4u,
                                     static_cast<UINT>(pixels.size()), pixels.data())))
        throw std::runtime_error("read HZB reference PNG pixels failed");

    std::uint32_t first_rgb{};
    bool have_first_rgb{};
    for (std::size_t index = 0; index < pixel_count; ++index)
    {
        const auto *pixel = pixels.data() + index * 4u;
        const auto rgb = static_cast<std::uint32_t>(pixel[0]) |
            (static_cast<std::uint32_t>(pixel[1]) << 8u) |
            (static_cast<std::uint32_t>(pixel[2]) << 16u);
        if (rgb) ++result.nonzero_rgb_pixels;
        if (!have_first_rgb) first_rgb = rgb, have_first_rgb = true;
        else if (rgb != first_rgb) result.pixel_variation = true;
    }
    return result;
}

void ValidateSurfaceDiagnostic(const std::filesystem::path &path,
                               std::uint32_t expected_width, std::uint32_t expected_height,
                               bool require_motion, const char *receipt_name, Json &receipt)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("surface diagnostic metadata is missing");
    Json diagnostic;
    input >> diagnostic;
    if (diagnostic.value("width", 0u) != expected_width ||
        diagnostic.value("height", 0u) != expected_height)
        throw std::runtime_error("surface diagnostic extent does not match the renderer target");
    const auto &extent = diagnostic.at("extent");
    if (extent.value("width", 0u) != expected_width ||
        extent.value("height", 0u) != expected_height)
        throw std::runtime_error("surface diagnostic extent metadata is inconsistent");
    if (diagnostic.value("foreground_depth_pixels", std::uint64_t{}) == 0 ||
        diagnostic.value("background_depth_pixels", std::uint64_t{}) == 0 ||
        diagnostic.value("foreground_valid_metadata_pixels", std::uint64_t{}) == 0 ||
        diagnostic.value("background_invalid_metadata_pixels", std::uint64_t{}) == 0)
        throw std::runtime_error("surface diagnostic foreground/background counters are invalid");
    if (require_motion && diagnostic.value("nonzero_motion_pixels", std::uint64_t{}) == 0)
        throw std::runtime_error("surface diagnostic has no nonzero historical motion pixels");
    diagnostic["path"] = path.filename().string();
    receipt["surface_diagnostics"][receipt_name] = std::move(diagnostic);
}

void ValidateGeometryDiagnostic(const std::filesystem::path &path, const char *receipt_name,
                               Json &receipt, bool require_static_dynamic = false)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("geometry diagnostic metadata is missing");
    Json diagnostic;
    input >> diagnostic;
    const auto used_records = diagnostic.value("used_record_count", 0u);
    if (used_records == 0 || !diagnostic.contains("records") ||
        diagnostic.at("records").size() != used_records)
        throw std::runtime_error("geometry diagnostic record count is invalid");
    const auto &counts = diagnostic.at("visible_ref_counts");
    const auto static_count = counts.value("static", std::uint64_t{});
    const auto dynamic_count = counts.value("dynamic", std::uint64_t{});
    const auto retest_count = counts.value("static_retest", std::uint64_t{});
    const auto static_input_count = counts.value("static_input", std::uint64_t{});
    const auto dynamic_input_count = counts.value("dynamic_input", std::uint64_t{});
    if (require_static_dynamic && (static_count == 0 || dynamic_count == 0))
        throw std::runtime_error("geometry diagnostic did not capture static and dynamic refs");

    struct Observation
    {
        std::uint32_t page{};
        float threshold{};
        bool complement{};
        bool history_invalid{};
    };
    std::map<std::string, std::vector<Observation>> groups;
    std::map<std::string, bool> aliases;
    std::uint64_t observed_counts[6]{};
    for (const auto &record : diagnostic.at("records"))
    {
        const auto source = record.value("refsource", 0u);
        const auto base = record.value("base", std::numeric_limits<std::uint64_t>::max());
        const auto count = record.value("count", std::numeric_limits<std::uint64_t>::max());
        const auto &visible_refs = record.at("visible_refs");
        constexpr auto max_index = std::numeric_limits<std::uint32_t>::max();
        if (source > 5 || base > max_index || count > max_index ||
            base > max_index - count || (source != 0 && count != visible_refs.size()))
            throw std::runtime_error("geometry diagnostic record ref source/count is invalid");
        const auto page = record.value("page", 0u);
        const auto view = record.value("view", 0u);
        for (const auto &ref : visible_refs)
        {
            const auto instance = ref.value("InstanceIndex", 0u);
            const auto generation = record.value("gen", 0u);
            const auto mesh_flags = ref.value("MeshFlags", 0u);
            const auto complement = ref.value("complement", false);
            const auto history_invalid = ref.value("history_invalid", false);
            if (((mesh_flags & 0x20000u) != 0) != complement ||
                ((mesh_flags & 0x100000u) != 0) != history_invalid)
                throw std::runtime_error("geometry diagnostic LOD flag metadata disagrees");
            const auto key = std::format("{}:{}:{}:{}:{}", instance, view, source, page,
                                         generation);
            if (aliases.contains(key))
                throw std::runtime_error("geometry diagnostic contains an aliased visible ref");
            aliases.emplace(key, true);
            const auto group = std::format("{}:{}:{}", instance, view, source);
            groups[group].push_back({page, ref.value("LodThreshold", -1.0f),
                complement, history_invalid});
            ++observed_counts[source];
        }
    }
    if (observed_counts[1] != static_count || observed_counts[2] != dynamic_count ||
        observed_counts[3] != retest_count || observed_counts[4] != static_input_count ||
        observed_counts[5] != dynamic_input_count)
        throw std::runtime_error("geometry diagnostic ref counters do not match its records");
    for (const auto &[key, observations] : groups)
    {
        if (observations.empty() || observations.size() > 2)
            throw std::runtime_error("geometry diagnostic has more than two pages per LOD family");
        if (observations.size() == 1)
        {
            if (observations.front().threshold != 1.0f || observations.front().complement)
                throw std::runtime_error("steady geometry LOD ref is not the selected endpoint");
        }
        else
        {
            if (observations[0].page == observations[1].page ||
                observations[0].threshold != observations[1].threshold ||
                observations[0].complement == observations[1].complement ||
                observations[0].history_invalid != observations[1].history_invalid)
                throw std::runtime_error("geometry LOD fade pair is inconsistent");
        }
    }
    diagnostic["path"] = path.filename().string();
    receipt["geometry_diagnostics"][receipt_name] = std::move(diagnostic);
}

void ValidateGpuLodGrowthSteady(const std::filesystem::path &path, Json &receipt)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("post-growth geometry diagnostic metadata is missing");
    Json diagnostic;
    input >> diagnostic;
    bool found_tree = false;
    for (const auto &record : diagnostic.at("records"))
    {
        if (record.value("refsource", 0u) != 2) continue;
        for (const auto &ref : record.at("visible_refs"))
        {
            if (ref.value("InstanceIndex", 0xffffffffu) != 1u) continue;
            found_tree = true;
            if (ref.value("history_invalid", true))
                throw std::runtime_error(
                    "post-growth original dynamic tree ref invalidated its steady LOD history");
        }
    }
    if (!found_tree)
        throw std::runtime_error("post-growth geometry diagnostic did not retain the original tree");
    receipt["gpu_lod_growth"] = {
        {"post_growth", path.filename().string()},
        {"original_tree_instance", 1},
        {"history_valid_after_growth", true},
    };
}

Json ValidationStatusReceipt(const hs::RendererValidationStatus &status)
{
    return {{"requested", {{"debug_layer", status.debug_layer_requested},
                {"gpu_based_validation", status.gpu_validation_requested}}},
        {"d3d12_debug_interface", {{"acquisition_attempted", status.debug_interface_acquisition_attempted},
            {"acquired", status.debug_interface_acquired}}},
        {"gpu_validation_interface_queried", status.gpu_validation_interface_queried},
        {"debug_layer_enable_call_before_device", status.debug_layer_enable_called_before_device},
        {"gpu_validation_enable_call_before_device", status.gpu_validation_enable_called_before_device},
        {"device_creation", {{"attempted", status.device_creation_attempted},
            {"succeeded", status.device_creation_succeeded}, {"hresult", status.device_creation_hresult},
            {"created_after_requested_enable_calls", status.device_created_after_requested_enable_calls}}},
        {"activation_state_getter_available", false}};
}

} // namespace

int main(int argc, char **argv)
{
    Json receipt{{"schema_version", 1}, {"runner", "render-only"},
        {"fixture", "static-environment-moving-player-v1"}, {"execution_valid", false}};
    std::filesystem::path artifacts;
    std::string failure_code = "HS_RENDER_ARGUMENT_INVALID";
    int exit_code = 1;
    const auto write_receipt = [&] {
        if (!artifacts.empty() && artifacts.is_absolute() && std::filesystem::is_directory(artifacts))
        {
            std::ofstream output(artifacts / "result.json");
            output << receipt.dump(2) << '\n';
            if (!output) exit_code = 1;
        }
        std::cout << receipt.dump() << '\n';
        return exit_code;
    };
    try
    {
        hs::RendererConfig config;
        config.capture_timing_stream = true;
        config.vsync = false;
        config.interpolate = false;
        config.devtools_visible = false;
        std::filesystem::path pointer;
        std::filesystem::path material_reload_pointer;
        std::filesystem::path material_reload_invalid_pointer;
        std::uint32_t frames = 60;
        bool frames_explicit{};
        bool persistent_scene_probe{};
        bool skin_cache_monster_probe{};
        bool dynamic_lod_skin_growth_probe{};
        bool gpu_lod_probe{};
        bool residency_streaming_probe{};
        bool surface_motion_resize_probe{};
        std::string grass_history_probe;
        std::uint32_t grass_cadence = 60;
        bool timing_stream_probe{};
        bool timing_stream_critical_probe{};
        bool hero_work_probe{}, hero_work_mode_explicit{}, shadow_transition_probe{}, hero_layer_probe{};
        bool world_transport_probe{};
        bool gameplay_scene{};
        bool static_shadow_bake_probe{};
        bool static_shadow_bake_only{};
        bool local_probe_revision_probe{};
        bool oit_overdraw_probe{};
        std::string vfx_composition_probe;
        bool entity_surface_probe{};
        bool camera_basis_probe{};
        bool light_list_probe{};
        bool lighting_auxiliary_probe{};
        bool software_gi_probe{};
        bool require_multi_queue{};
        bool require_transient_aliasing{};
        bool grid_floor_probe{};
        bool owned_view_probe{};
        bool provider_switch_probe{};
        bool all_providers_probe{};
        bool camera_motion{};
        bool capture_grid_frames{};
        bool aa_mode_explicit{};
        GridAaMode grid_aa_mode = GridAaMode::Taa;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view arg = argv[index];
            if (arg.starts_with("--generation=")) pointer = arg.substr(13);
            else if (arg.starts_with("--material-reload="))
                material_reload_pointer = arg.substr(18);
            else if (arg.starts_with("--material-reload-invalid="))
                material_reload_invalid_pointer = arg.substr(26);
            else if (arg.starts_with("--artifacts=")) artifacts = arg.substr(12);
            else if (arg.starts_with("--frames="))
            {
                frames = Number(arg.substr(9), 1, 600);
                frames_explicit = true;
            }
            else if (arg == "--warp") config.warp = true;
            else if (arg == "--validation") config.validation = true;
            else if (arg == "--gpu-validation") config.validation = config.gpu_validation = true;
            else if (arg == "--barriers=legacy") config.barrier_mode = hs::BarrierMode::Legacy;
            else if (arg == "--barriers=enhanced") config.barrier_mode = hs::BarrierMode::Enhanced;
            else if (arg.starts_with("--geometry="))
            {
                const auto value = arg.substr(11);
                if (value == "auto") config.geometry_frontend = hs::GeometryFrontend::Automatic;
                else if (value == "indexed") config.geometry_frontend = hs::GeometryFrontend::Indexed;
                else if (value == "mesh") config.geometry_frontend = hs::GeometryFrontend::MeshShader;
                else throw std::runtime_error("geometry must be auto, indexed, or mesh: " +
                                              std::string(value));
            }
            else if (arg == "--surface=compact")
                config.surface_frontend = hs::SurfaceFrontend::CompactDeferred;
            else if (arg == "--surface=visibility")
                config.surface_frontend = hs::SurfaceFrontend::VisibilityBuffer;
            else if (arg == "--environment-preview") config.environment_preview = true;
            else if (arg == "--persistent-scene-probe") persistent_scene_probe = true;
            else if (arg == "--skin-cache-monster-probe") skin_cache_monster_probe = true;
            else if (arg == "--require-multi-queue") require_multi_queue = true;
            else if (arg == "--require-transient-aliasing") require_transient_aliasing = true;
            else if (arg == "--dynamic-lod-skin-growth-probe") dynamic_lod_skin_growth_probe = true;
            else if (arg == "--gpu-lod-probe") gpu_lod_probe = true;
            else if (arg == "--residency-streaming-probe") residency_streaming_probe = true;
            else if (arg == "--surface-motion-resize-probe") surface_motion_resize_probe = true;
            else if (arg.starts_with("--grass-history-probe="))
                grass_history_probe = arg.substr(22);
            else if (arg.starts_with("--grass-cadence="))
                grass_cadence = Number(arg.substr(16), 30, 240);
            else if (arg == "--timing-stream-probe")
            {
                if (timing_stream_critical_probe)
                    throw std::runtime_error("timing stream probes are mutually exclusive");
                timing_stream_probe = config.capture_timing_stream = true;
                config.width = 320; config.height = 180;
            }
            else if (arg == "--timing-stream-critical-probe")
            {
                if (timing_stream_probe)
                    throw std::runtime_error("timing stream probes are mutually exclusive");
                timing_stream_probe = timing_stream_critical_probe = config.capture_timing_stream = true;
                config.capture_gpu_timings = false;
                config.dynamic_resolution = true;
                config.width = 320; config.height = 180;
            }
            else if (arg == "--hero-work-probe") hero_work_probe = true;
            else if (arg == "--shadow-transition-probe") shadow_transition_probe = true;
            else if (arg.starts_with("--hero-work-mode="))
            {
                if (hero_work_mode_explicit) throw std::runtime_error("duplicate hero work mode");
                hero_work_mode_explicit = true;
                const auto mode = arg.substr(std::string_view("--hero-work-mode=").size());
                if (mode == "full") config.hero_work_mode = hs::HeroWorkDiagnosticMode::Full;
                else if (mode == "no-environment") config.hero_work_mode = hs::HeroWorkDiagnosticMode::NoEnvironment;
                else if (mode == "coverage-only") config.hero_work_mode = hs::HeroWorkDiagnosticMode::CoverageOnly;
                else throw std::runtime_error("hero work mode must be full/no-environment/coverage-only");
            }
            else if (arg == "--world-transport-probe") world_transport_probe = true;
            else if (arg == "--gameplay-scene") gameplay_scene = true;
            else if (arg == "--static-shadow-bake-probe") gameplay_scene = static_shadow_bake_probe = true;
            else if (arg == "--static-shadow-bake-only") static_shadow_bake_only = true;
            else if (arg == "--local-probe-revision-probe") gameplay_scene = local_probe_revision_probe = true;
            else if (arg == "--oit-overdraw-probe") oit_overdraw_probe = true;
            else if (arg.starts_with("--vfx-composition-probe=")) vfx_composition_probe = arg.substr(24);
            else if (arg == "--entity-surface-probe") entity_surface_probe = true;
            else if (arg == "--camera-basis-probe") camera_basis_probe = true;
            else if (arg == "--light-list-probe") light_list_probe = true;
            else if (arg == "--lighting-auxiliary-probe") lighting_auxiliary_probe = true;
            else if (arg == "--software-gi-probe") software_gi_probe = world_transport_probe = true;
            else if (arg == "--grid-floor-probe") grid_floor_probe = true;
            else if (arg == "--owned-view-probe") owned_view_probe = true;
            else if (arg == "--provider-switch-probe") provider_switch_probe = true;
            else if (arg == "--all-providers-probe") all_providers_probe = true;
            else if (arg == "--camera-motion") camera_motion = true;
            else if (arg == "--capture-grid-frames") capture_grid_frames = true;
            else if (arg == "--taa")
            {
                grid_aa_mode = GridAaMode::Taa;
                aa_mode_explicit = true;
            }
            else if (arg == "--fxaa")
            {
                grid_aa_mode = GridAaMode::Fxaa;
                aa_mode_explicit = true;
            }
            else if (arg == "--aa=off")
            {
                grid_aa_mode = GridAaMode::Off;
                aa_mode_explicit = true;
            }
            else if (arg == "--aa=taa")
            {
                grid_aa_mode = GridAaMode::Taa;
                aa_mode_explicit = true;
            }
            else if (arg == "--aa=fxaa")
            {
                grid_aa_mode = GridAaMode::Fxaa;
                aa_mode_explicit = true;
            }
            else if (arg == "--no-bloom") config.bloom = false;
            else if (arg == "--no-outline") config.outline = false;
            else if (arg.starts_with("--resolution="))
            {
                const auto value = arg.substr(13);
                const auto separator = value.find('x');
                if (separator == std::string_view::npos)
                    throw std::runtime_error("resolution must be WIDTHxHEIGHT");
                config.width = Number(value.substr(0, separator), 640, 7680);
                config.height = Number(value.substr(separator + 1), 360, 4320);
            }
            else throw std::runtime_error("unknown argument: " + std::string(arg));
        }
        receipt["geometry_frontend_requested"] = config.geometry_frontend == hs::GeometryFrontend::Indexed
            ? "indexed" : config.geometry_frontend == hs::GeometryFrontend::MeshShader ? "mesh" : "auto";
        receipt["surface_frontend"] = config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer
            ? "visibility" : "compact";
        const bool material_reload_probe = !material_reload_pointer.empty() ||
                                           !material_reload_invalid_pointer.empty();
        const bool surface_motion_resize_fixture =
            surface_motion_resize_probe;
        if (entity_surface_probe)
        {
            if ((frames_explicit && frames != 8) || !vfx_composition_probe.empty() || !grass_history_probe.empty())
                throw std::runtime_error("Entity surface probe requires eight isolated ordinary frames");
            frames = 8;
            config.interpolate = config.dynamic_resolution = config.temporal_reconstruction = false;
            config.grid_floor_probe = config.grid_floor_disable_aa = true;
            config.bloom = config.outline = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
            receipt["entity_surface"] = {{"frames", Json::array()}, {"diagnostic_options", true},
                {"aa", "off"}, {"exposure_delta_seconds", 0}, {"simulation_time_ns", 0},
                {"negative_generation_scope", "source guard only; no GPU fault injection"}};
        }
        if (!grass_history_probe.empty())
        {
            if (grass_history_probe.size() != 1 || grass_history_probe[0] < 'A' ||
                grass_history_probe[0] > 'E' || frames < 16 || gameplay_scene || owned_view_probe ||
                surface_motion_resize_fixture || persistent_scene_probe || gpu_lod_probe)
                throw std::runtime_error("grass history probe requires A..E and at least 16 ordinary frames");
            config.dynamic_resolution = false;
            config.temporal_reconstruction = true;
            config.sharpening = 0;
            config.bloom = config.outline = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
            receipt["grass_history"] = {{"case", grass_history_probe},
                {"logical_render_hz", grass_cadence}, {"diagnostic_options", true},
                {"frames", Json::array()}};
        }
        if (light_list_probe || lighting_auxiliary_probe)
        {
            const auto required_frames = light_list_probe ? 6u : 5u;
            if (!frames_explicit) frames = required_frames;
            if (frames != required_frames) throw std::runtime_error("lighting probe frame count is fixed");
            config.lighting_quality = hs::GraphicsQuality::Ultra;
            config.dynamic_resolution = config.temporal_reconstruction = false;
            config.bloom = config.outline = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
            if (light_list_probe)
                receipt["light_list"] = {{"local_counts", {4, 64, 65, 128, 127, 0}},
                    {"directional_counts", {0, 0, 0, 0, 1, 1}}, {"frames", Json::array()}};
            else receipt["lighting_auxiliaries"]["frames"] = Json::array();
        }
        if (!vfx_composition_probe.empty())
        {
            if ((vfx_composition_probe != "fixed" && vfx_composition_probe != "auto" &&
                 vfx_composition_probe != "control" && vfx_composition_probe != "switch") ||
                (frames_explicit && frames != 32) || oit_overdraw_probe || camera_basis_probe ||
                gameplay_scene || owned_view_probe || surface_motion_resize_fixture ||
                persistent_scene_probe || grid_floor_probe || world_transport_probe ||
                provider_switch_probe || material_reload_probe || !grass_history_probe.empty() ||
                light_list_probe || lighting_auxiliary_probe || skin_cache_monster_probe ||
                dynamic_lod_skin_growth_probe || gpu_lod_probe || residency_streaming_probe ||
                all_providers_probe)
                throw std::runtime_error("VFX composition probe requires fixed/auto/control/switch and 32 isolated frames");
            frames = 32;
            config.dynamic_resolution = false;
            config.temporal_reconstruction = true;
            config.sharpening = 0;
            config.bloom = config.outline = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
            receipt["vfx_composition"] = {{"exposure", vfx_composition_probe},
                {"logical_hz", 60}, {"diagnostic_options", true}, {"frames", Json::array()}};
            if (vfx_composition_probe == "switch")
            {
                // Unlike native-options fixtures, a settings transition needs
                // an acknowledged startup request. Preserve this fixture's
                // native extent, TAA and disabled effects in that projection.
                hs::GraphicsSettings initial;
                initial.vsync = config.vsync;
                initial.resolution_mode = hs::GraphicsResolutionMode::Manual;
                initial.manual_scale_percent = static_cast<float>(config.render_scale_percent);
                initial.dynamic_resolution = config.dynamic_resolution;
                initial.reconstruction_provider.value = "internal_taau";
                initial.reconstruction_quality.value = "native";
                initial.sharpening = config.sharpening;
                initial.bloom = config.bloom;
                initial.outline = config.outline;
                initial.ao_mode = config.ao_mode;
                initial.gi_mode = config.gi_mode;
                initial.reflection_mode = config.reflection_mode;
                initial.volumetric_quality = config.volumetric_quality;
                initial.particle_quality = config.vfx_quality == hs::VfxQuality::Low
                    ? hs::GraphicsQuality::Low : config.vfx_quality == hs::VfxQuality::Medium
                        ? hs::GraphicsQuality::Medium : hs::GraphicsQuality::High;
                config.requested_graphics = std::move(initial);
            }
        }
        if (oit_overdraw_probe || camera_basis_probe)
        {
            const auto required_frames = camera_basis_probe ? 6u : 4u;
            if (!frames_explicit) frames = required_frames;
            if (frames != required_frames || (oit_overdraw_probe && camera_basis_probe) ||
                owned_view_probe || grid_floor_probe || gameplay_scene ||
                surface_motion_resize_fixture || world_transport_probe || provider_switch_probe)
                throw std::runtime_error("OIT/camera-basis probes require their own four/six ordinary render frames");
            config.temporal_reconstruction = false;
            config.bloom = config.outline = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
            if (oit_overdraw_probe)
                receipt["oit_overdraw"] = {{"writer", "VfxSpriteOitPS/RadialEnergy"},
                    {"layers", {1, 22, 11, 64}}, {"hdr", {1, 1, 2, 5.5}},
                    {"opacity_scale", 1000}, {"frames", Json::array()}};
            else receipt["camera_basis"] = {{"cameras", {"front", "roll90", "vertical_down"}},
                {"writers", {"RadialEnergy", "Smoke6Way"}}, {"frames", Json::array()}};
        }
        if (world_transport_probe)
        {
            if (!frames_explicit) frames = 7;
            if (frames < 7) throw std::runtime_error("--world-transport-probe requires at least 7 frames");
            config.ao_mode = hs::GraphicsAoMode::GTAO;
            config.gi_mode = hs::GraphicsGiMode::Probe;
            config.reflection_mode = hs::GraphicsReflectionMode::ScreenSpace;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Medium;
        }
        if (software_gi_probe)
        {
            config.gi_mode = hs::GraphicsGiMode::SoftwareHybrid;
            config.gi_quality = hs::GraphicsQuality::Low;
            config.dynamic_resolution = false;
        }
        if (static_shadow_bake_only && !static_shadow_bake_probe)
            throw std::runtime_error("--static-shadow-bake-only requires --static-shadow-bake-probe");
        if (static_shadow_bake_probe)
        {
            if (!frames_explicit) frames = 3;
            if (!static_shadow_bake_only && frames < 2)
                throw std::runtime_error("--static-shadow-bake-probe requires at least 2 subsequent frames");
            if (local_probe_revision_probe)
                throw std::runtime_error("Static shadow bake and local probe revision probes are mutually exclusive");
        }
        if ((camera_motion || capture_grid_frames || aa_mode_explicit) && !grid_floor_probe)
            throw std::runtime_error(
                "--camera-motion, --capture-grid-frames, and AA selection require --grid-floor-probe");
        if (pointer.empty() || !pointer.is_absolute() || artifacts.empty() || !artifacts.is_absolute())
            throw std::runtime_error("--generation and --artifacts require absolute paths");
        if (material_reload_probe &&
            (material_reload_pointer.empty() || material_reload_invalid_pointer.empty() ||
             !material_reload_pointer.is_absolute() ||
             !material_reload_invalid_pointer.is_absolute()))
            throw std::runtime_error(
                "--material-reload and --material-reload-invalid require absolute paths together");
        if (material_reload_probe && material_reload_pointer == material_reload_invalid_pointer)
            throw std::runtime_error("material reload candidates must be different pointers");
        if (material_reload_probe && frames < 6)
            throw std::runtime_error("material reload probe requires at least 6 frames");
        if (hero_work_mode_explicit && !hero_work_probe)
            throw std::runtime_error("--hero-work-mode requires --hero-work-probe");
        if (shadow_transition_probe && (!hero_work_probe || !gameplay_scene))
            throw std::runtime_error("--shadow-transition-probe requires --hero-work-probe --gameplay-scene");
        if (hero_work_probe)
        {
            if (!frames_explicit) frames = 4;
            if (frames > 8 || timing_stream_probe || !grass_history_probe.empty() || !vfx_composition_probe.empty())
                throw std::runtime_error("Hero work probe requires at most eight isolated ordinary frames");
            if (gameplay_scene && frames != 4) throw std::runtime_error("Hero gameplay/water shadow probe requires exactly four frames");
            config.capture_hero_work = config.capture_timing_stream = true;
            hero_layer_probe = !gameplay_scene && frames == 8;
            if (hero_layer_probe) config.preview_camera_override = false; // Use the fixture snapshot's actual camera yaw.
            config.slime_family_preview_count = gameplay_scene ? 0u : 1u; // Preview replaces environment; gameplay must retain authored water.
            if (shadow_transition_probe)
            {
                // Dedicated supported-domain functional fixture, never a production quality default.
                config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
                config.gi_mode = hs::GraphicsGiMode::Probe;
            }
            config.temporal_reconstruction = false;
            config.bloom = config.outline = false;
            receipt["hero_work"] = {{"fixture", gameplay_scene ? "actual-gameplay-meadow-water" : "existing-slime-family-preview-count1"}, {"requested_frames", frames},
                {"mode", HeroWorkModeName(config.hero_work_mode)}, {"functional_only", true}, {"performance_adoption", false}};
            if (hero_layer_probe)
                receipt["hero_work"]["quality_transition"] = {{"request_frame", 5},
                    {"frames1_4", "existing Medium refraction/transparency, one layer and original camera"},
                    {"frames5_8", "Ultra refraction/transparency, four layers, camera yaw90/pitch0 for actual transparent overlap"},
                    {"camera_source", "snapshot camera yaw/pitch through MainViewInput; renderer preview_camera_override=false"},
                    {"frames", Json::array()}};
        }
        if (grid_floor_probe && frames < 25)
            throw std::runtime_error("grid floor probe requires at least 25 frames");
        if (timing_stream_probe && (frames < 8 || owned_view_probe))
            throw std::runtime_error("--timing-stream-probe requires at least 8 single-view frames");
        if (provider_switch_probe && frames < 7)
            throw std::runtime_error("provider switch probe requires at least seven frames");
        if (all_providers_probe && !provider_switch_probe)
            throw std::runtime_error("--all-providers-probe requires --provider-switch-probe");
        if (all_providers_probe && frames < 38)
            throw std::runtime_error("--all-providers-probe requires at least 38 frames");
        if (owned_view_probe && frames != 3)
            throw std::runtime_error("--owned-view-probe requires exactly 3 frame packages");
        if (persistent_scene_probe && frames < 9)
            throw std::runtime_error("--persistent-scene-probe requires at least 9 frames");
        if (persistent_scene_probe && config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer && frames < 11)
            throw std::runtime_error("Visibility --persistent-scene-probe requires at least 11 frames for live Cluster Retest");
        if (skin_cache_monster_probe && frames < 2)
            throw std::runtime_error("--skin-cache-monster-probe requires at least 2 frames");
        if (local_probe_revision_probe && frames < 96)
            throw std::runtime_error("--local-probe-revision-probe requires at least 96 frames");
        if (dynamic_lod_skin_growth_probe && frames < 8)
            throw std::runtime_error("--dynamic-lod-skin-growth-probe requires at least 8 frames");
        if (gpu_lod_probe && frames < 10)
            throw std::runtime_error("--gpu-lod-probe requires at least 10 frames");
        if (surface_motion_resize_fixture && frames < 7)
            throw std::runtime_error("--surface-motion-resize-probe requires at least 7 frames");
        if (static_cast<unsigned>(hero_work_probe) + static_cast<unsigned>(persistent_scene_probe) +
                static_cast<unsigned>(skin_cache_monster_probe) +
                static_cast<unsigned>(dynamic_lod_skin_growth_probe) +
                static_cast<unsigned>(gpu_lod_probe) +
                static_cast<unsigned>(residency_streaming_probe) +
                static_cast<unsigned>(surface_motion_resize_fixture) +
                static_cast<unsigned>(world_transport_probe) +
                static_cast<unsigned>(gameplay_scene && !hero_work_probe) +
                static_cast<unsigned>(material_reload_probe) +
                static_cast<unsigned>(owned_view_probe) +
                static_cast<unsigned>(provider_switch_probe) +
                static_cast<unsigned>(oit_overdraw_probe) +
                static_cast<unsigned>(camera_basis_probe) +
                static_cast<unsigned>(light_list_probe) +
                static_cast<unsigned>(lighting_auxiliary_probe) +
                static_cast<unsigned>(entity_surface_probe) +
                static_cast<unsigned>(grid_floor_probe) > 1)
            throw std::runtime_error("render-only probes are mutually exclusive");
        if (grid_floor_probe)
        {
            config.grid_floor_probe = true;
            config.bloom = false;
            config.outline = false;
            if (grid_aa_mode == GridAaMode::Fxaa)
                config.vfx_quality = hs::VfxQuality::Low;
            else if (grid_aa_mode == GridAaMode::Taa)
                config.vfx_quality = hs::VfxQuality::High;
            config.grid_floor_disable_aa = grid_aa_mode == GridAaMode::Off;
        }
        if (persistent_scene_probe || material_reload_probe || static_shadow_bake_probe)
        {
            // These probes compare scene/material publications. Unrelated
            // temporal and high-quality effects add history and costly driver
            // instrumentation without exercising that publication contract.
            config.vfx_quality = hs::VfxQuality::Low;
            config.temporal_reconstruction = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.contact_shadows = false;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
        }
        if (provider_switch_probe)
        {
            hs::GraphicsSettings initial;
            initial.vsync = false;
            initial.reconstruction_provider.value = "internal_taau";
            initial.frame_generation_provider.value = "off";
            initial.frame_generation_mode = hs::GraphicsFrameGenerationMode::Off;
            initial.low_latency = hs::GraphicsAutoToggle::Off;
            config.requested_graphics = initial;
        }
        if (skin_cache_monster_probe)
        {
            config.slime_family_preview_count = 1;
            config.vfx_quality = hs::VfxQuality::Low;
        }
        receipt["fixture"] = hero_work_probe ? (gameplay_scene ? "hero-work-functional-water-world-v1" : "hero-work-functional-slime-v1") : provider_switch_probe ? "provider-switch-v1" : owned_view_probe ? "owned-view-history-v1" : persistent_scene_probe
            ? "persistent-gpu-scene-v2"
            : skin_cache_monster_probe ? "skin-cache-monster-v1"
            : dynamic_lod_skin_growth_probe ? "dynamic-lod-skin-growth-v1"
            : gpu_lod_probe ? "gpu-lod-probe-v1"
             : residency_streaming_probe ? "residency-streaming-v3"
              : software_gi_probe ? "software-gi-world-v1"
            : world_transport_probe ? "world-transport-v1"
            : static_shadow_bake_probe ? "static-shadow-bake-v1"
            : gameplay_scene ? "gameplay-arena-v1"
              : surface_motion_resize_probe ? "surface-motion-resize-v1"
             : grid_floor_probe ? "grid-floor-probe-v1"
             : material_reload_probe ? "material-parameter-reload-v1"
             : light_list_probe ? "light-list-capacity-v1"
             : lighting_auxiliary_probe ? "lighting-auxiliary-liveness-v1"
             : !vfx_composition_probe.empty() ? "vfx-composition-v1"
             : entity_surface_probe ? "entity-surface-identity-v1"
                                        : "static-environment-moving-player-v1";
        if (grid_floor_probe)
        {
            receipt["grid_floor"] = {
                {"enabled", true},
                {"aa", GridAaModeName(grid_aa_mode)},
                {"aa_disabled", config.grid_floor_disable_aa},
                {"bloom", config.bloom},
                {"outline", config.outline},
                {"camera_motion", camera_motion},
                {"frames", frames},
                {"capture_frames", Json::array({1, 9, 17, 25})},
                {"captures", Json::object()},
                {"camera_targets", Json::object()},
            };
        }
        std::filesystem::create_directories(artifacts);
        config.artifact_directory = artifacts;
        Json timings = Json::object();
        const auto measure = [&](const char *name, auto &&operation) {
            const auto start = Clock::now();
            operation();
            const auto elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            timings[name] = elapsed_ms;
            std::clog << "hs.render.stage name=" << name << " elapsed_ms=" << elapsed_ms << '\n';
        };
        hs::SelectedGeneration generation;
        hs::SelectedGeneration material_reload_generation;
        hs::SelectedGeneration material_reload_invalid_generation;
        failure_code = "HS_GENERATION_INVALID";
        measure("generation", [&] { Require(hs::LoadSelectedGeneration(pointer, generation)); });
        receipt["generation_id"] = generation.id;
        receipt["generation_scope"] = generation.scope;
        hs::VfxEntitySurfaceCommand entity_surface_template;
        if (entity_surface_probe)
        {
            std::filesystem::path path;
            Require(generation.Require("vfx/vfx_program.hsbin", path));
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input || input.tellg() <= 0) throw std::runtime_error("Entity surface fixture requires its cooked VFX program");
            std::vector<std::byte> bytes(static_cast<std::size_t>(input.tellg()));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("Could not read entity surface fixture VFX program");
            hs::VfxProgramData program;
            std::string error;
            if (!hs::LoadVfxProgram(bytes, program, error)) throw std::runtime_error(error);
            const auto hash64 = [](std::string_view value) {
                std::uint64_t hash = 14695981039346656037ull;
                for (const unsigned char c : value) hash = (hash ^ c) * 1099511628211ull;
                return hash;
            };
            const auto hash32 = [](std::string_view value) {
                std::uint32_t hash = 2166136261u;
                for (const unsigned char c : value) hash = (hash ^ c) * 16777619u;
                return hash;
            };
            const auto effect = std::ranges::find(program.effect_lookup,
                hash64("particle.boss.phase_change"), &hs::VfxEffectLookupRecord::effect_id);
            if (effect == program.effect_lookup.end()) throw std::runtime_error("Cooked boss phase effect is missing");
            if (program.effects.at(effect->handle - 1).seconds != 1.5f)
                throw std::runtime_error("Cooked boss phase fixture requires the actual 90-tick interval");
            unsigned matched{};
            for (std::size_t i = 0; i < program.outputs.size(); ++i)
            {
                const auto &output = program.outputs[i];
                const auto &source = program.sources.at(output.source);
                if (source.effect != effect->handle || output.profile != hs::VfxOutputProfile::EntitySurface) continue;
                if (source.stable_id != hash32("entity_surface_transition") || source.type != hs::VfxSourceType::Direct ||
                    source.knot_count != 2 || source.knots[0] != .08f || source.knots[1] != .88f ||
                    output.motion != hs::VfxMotionKind::SurfaceNoiseThroughBoss || output.hdr != 4.8f || output.rgba[3] != .72f)
                    throw std::runtime_error("Cooked entity surface fixture interval/output contract changed");
                entity_surface_template.output_index = static_cast<std::uint32_t>(i);
                entity_surface_template.gradient_row = output.gradient_row;
                entity_surface_template.color = {output.rgba[0], output.rgba[1], output.rgba[2], output.rgba[3]};
                entity_surface_template.hdr = output.hdr; entity_surface_template.alpha = output.rgba[3];
                unsigned parameters{};
                for (const auto &parameter : std::span(program.parameters).subspan(output.parameters.first, output.parameters.count))
                {
                    if (parameter.key == hash32("noise_scale") && parameter.type == hs::VfxParameterType::Float)
                    { entity_surface_template.noise_scale = std::bit_cast<float>(parameter.bits); parameters |= 1; }
                    else if (parameter.key == hash32("edge_width") && parameter.type == hs::VfxParameterType::Float)
                    { entity_surface_template.edge_width = std::bit_cast<float>(parameter.bits); parameters |= 2; }
                    else if (parameter.key == hash32("direction") && parameter.type == hs::VfxParameterType::Enum && parameter.bits == hash32("up_and_out"))
                    { entity_surface_template.direction = hs::VfxEntitySurfaceDirection::UpAndOut; parameters |= 4; }
                }
                if (output.parameters.count != 3 || parameters != 7 || entity_surface_template.noise_scale != 2.4f || entity_surface_template.edge_width != .07f)
                    throw std::runtime_error("Cooked entity surface fixture parameter contract changed");
                ++matched;
            }
            if (matched != 1) throw std::runtime_error("Entity surface fixture requires one actual phase output");
            receipt["entity_surface"]["cooked_output"] = {{"index", entity_surface_template.output_index},
                {"gradient_row", entity_surface_template.gradient_row}, {"hdr", entity_surface_template.hdr},
                {"alpha", entity_surface_template.alpha}, {"noise_scale", entity_surface_template.noise_scale},
                {"edge_width", entity_surface_template.edge_width}};
        }
        const auto owned_view_archer_asset = owned_view_probe ? SelectedArcherAsset(generation) : hs::AssetId{};
        config.content_generation_id = generation.id;
        config.content_generation_lease = std::make_shared<const hs::SelectedGeneration>(generation);
        config.product_paths = generation.product_paths;
        config.product_digests = generation.product_digests;
        std::shared_ptr<const hs::StaticRenderScene> world_scene;
        if (world_transport_probe || owned_view_probe || gameplay_scene)
        {
            failure_code = "HS_RENDER_WORLD_TRANSPORT_SCENE_INVALID";
            std::filesystem::path product_path;
            Require(generation.Require(config.scene_hlod_product_id, product_path));
            Require(generation.Require("render/lighting/local_probes.spkg", product_path));
            measure("world_scene", [&] { world_scene = hs::app::WorldTransportScene(generation); });
            receipt[world_transport_probe ? "world_transport" : "owned_view_scene"] = {
                {"layout_abi", world_scene->layout_abi},
                {"eligible_static_member_count", world_scene->eligible_static_members.size()},
                {"static_instance_count", world_scene->instances.size()},
                {"immutable_scene_frames", frames},
            };
        }
        if (material_reload_probe)
        {
            failure_code = "HS_MATERIAL_RELOAD_GENERATION_INVALID";
            measure("material_reload_generation", [&] {
                Require(hs::LoadSelectedGeneration(material_reload_invalid_pointer,
                                                    material_reload_invalid_generation));
                Require(hs::VerifySelectedGenerationProducts(
                    material_reload_invalid_generation));
                Require(hs::LoadSelectedGeneration(material_reload_pointer,
                                                   material_reload_generation));
                Require(hs::VerifySelectedGenerationProducts(material_reload_generation));
            });
        }
        TestWindow window;
        failure_code = "HS_RENDER_WINDOW_FAILED";
        measure("window", [&] { window.Create(config.width, config.height); });
        config.window = window.handle;
        hs::TaskSystem recording_workers(4);
        auto recording_group = std::make_unique<hs::TaskGroup>();
        config.task_executor.submit = [&](std::function<void()> record) {
            const bool accepted = recording_workers.Submit(*recording_group,
                "Render fixture recording",
                [record = std::move(record)](std::stop_token) { record(); });
            return accepted ? hs::Result::Success() :
                hs::Result::Failure(hs::ErrorCode::TaskFailed, "hs_render_test_runner",
                                    "Could not submit recording task.");
        };
        config.task_executor.join = [&] {
            auto joined = recording_workers.Wait(*recording_group);
            recording_group = std::make_unique<hs::TaskGroup>();
            return joined;
        };
        std::vector<hs::render_data::RendererBulkDescriptor> packed_directory;
        std::vector<hs::render_data::TextureDescriptorStorage> packed_texture_storage;
        std::vector<hs::render_data::TextureDescriptor> packed_textures;
        std::vector<hs::render_data::GeometryDescriptorStorage> packed_geometry_storage;
        std::vector<hs::render_data::GeometryDescriptor> packed_geometry;
        std::vector<hs::render_data::MaterialDescriptorStorage> packed_material_storage;
        std::vector<hs::render_data::MaterialDescriptor> packed_materials;
        std::vector<hs::render_data::CharacterDescriptorStorage> packed_character_storage;
        std::vector<hs::render_data::CharacterDescriptor> packed_characters;
        std::vector<hs::render_data::AnimationDescriptorStorage> packed_animation_storage;
        std::vector<hs::render_data::AnimationDescriptor> packed_animations;
        hs::render_data::VfxMeshAtlasDescriptor packed_vfx_mesh_atlas;
        std::vector<hs::render_data::RendererSourceRevision> packed_source_revisions;
        std::vector<hs::render_data::RendererRevisionBulk> packed_revision_bulks;
        hs::D3D12Renderer renderer;
        failure_code = "HS_RENDER_INITIALIZE_FAILED";
        auto startup_font = ReadDeveloperFont();
        config.font_bytes = startup_font;
        if (residency_streaming_probe)
        {
            const auto pack_path = artifacts / "renderer-products-pack.hsp";
            std::ofstream pack(pack_path, std::ios::binary | std::ios::trunc);
            const std::array<char, 64> zeros{};
            pack.write(zeros.data(), zeros.size());
            std::vector<std::pair<hs::AssetId, std::string>> products;
            for (const auto &[id, path] : generation.product_paths)
                if (id.starts_with("render/") || id == "vfx/vfx_mesh_atlas.hsbin")
                    products.emplace_back(hs::MakeAssetId(id), id);
            std::ranges::sort(products, {}, [](const auto &entry) { return entry.first.value; });
            const auto digest_bytes = [&](const std::string &id) {
                const auto &digest = generation.product_digests.at(id);
                std::array<std::byte, 32> expected{};
                if (digest.size() != 71 || !digest.starts_with("sha256:"))
                    throw std::runtime_error("Invalid renderer fixture digest: " + id);
                for (std::size_t i = 0; i < expected.size(); ++i)
                {
                    unsigned byte{};
                    const auto first = digest.data() + 7 + i * 2;
                    const auto [last, error] = std::from_chars(first, first + 2, byte, 16);
                    if (error != std::errc{} || last != first + 2) throw std::runtime_error("Invalid renderer digest byte");
                    expected[i] = static_cast<std::byte>(byte);
                }
                return expected;
            };
            const auto verified_product = [&](const std::string &id) {
                hs::ShippedContentOwner input;
                Require(hs::ShippedContentOwner::Open(generation.product_paths.at(id), input));
                if (input.Size() > std::numeric_limits<std::size_t>::max())
                    throw std::runtime_error("Fixture product exceeds addressing: " + id);
                std::vector<std::byte> bytes(static_cast<std::size_t>(input.Size()));
                Require(input.ReadVerified(0, bytes, digest_bytes(id)));
                return bytes;
            };
            std::vector<std::string> closure;
            closure.reserve(generation.product_paths.size());
            for (const auto &[id, path] : generation.product_paths) closure.push_back(id);
            hs::apps::RendererGeometryCatalog geometry_catalog(closure);
            std::map<std::string, std::vector<std::byte>> metadata_buffers;
            for (const auto &[asset, id] : products)
                if (hs::apps::RendererGeometryCatalog::NeedsMetadata(id))
                {
                    auto bytes = verified_product(id);
                    geometry_catalog.AddMetadata(id, bytes);
                    metadata_buffers.emplace(id, std::move(bytes));
                }
            std::vector<char> buffer(1024 * 1024);
            std::uint64_t offset = 64;
            for (const auto &[asset, id] : products)
            {
                const auto expected = digest_bytes(id);
                using F = hs::render_data::RenderBulkFormat;
                const auto format = id.ends_with(".rpkg") ? F::Package : id.ends_with(".rpage") ? F::GeometryPage :
                    id.ends_with(".ghbin") ? F::GeometryHierarchy : id.ends_with(".shl1") ? F::SceneHlod :
                    id.ends_with(".index") ? F::StaticShadowIndex : id.ends_with(".pages") ? F::StaticShadowPages :
                    id.ends_with(".dds") ? F::DdsTexture : id.ends_with(".spkg") ? F::SceneProbe :
                    id.ends_with(".hsbin") ? F::VfxMeshAtlas : F::ShaderDxil;
                if (format == F::ShaderDxil && !id.ends_with(".dxil"))
                    throw std::runtime_error("Unsupported renderer fixture bulk format: " + id);
                const auto bytes = std::filesystem::file_size(generation.product_paths.at(id));
                const auto padding = (64 - offset % 64) % 64;
                pack.write(zeros.data(), static_cast<std::streamsize>(padding));
                offset += padding;
                packed_directory.push_back({asset, {offset, bytes, format, expected}});
                if (const auto metadata = metadata_buffers.find(id); metadata != metadata_buffers.end())
                {
                    if (metadata->second.size() != bytes ||
                        !pack.write(reinterpret_cast<const char *>(metadata->second.data()), static_cast<std::streamsize>(bytes)))
                        throw std::runtime_error("Could not write verified geometry metadata: " + id);
                    offset += bytes;
                    continue;
                }
                if (id.ends_with(".rpage"))
                {
                    const auto page_bytes = verified_product(id);
                    packed_geometry_storage.push_back(geometry_catalog.CompilePage(id, page_bytes,
                        static_cast<std::uint32_t>(packed_directory.size() - 1)));
                    if (page_bytes.size() != bytes ||
                        !pack.write(reinterpret_cast<const char *>(page_bytes.data()), static_cast<std::streamsize>(bytes)))
                        throw std::runtime_error("Could not write verified geometry page: " + id);
                    offset += bytes;
                    continue;
                }
                if (id.starts_with("render/texture/") && id.ends_with(".rpkg"))
                {
                    hs::ShippedContentOwner input;
                    Require(hs::ShippedContentOwner::Open(generation.product_paths.at(id), input));
                    if (bytes > std::numeric_limits<std::size_t>::max() ||
                        bytes > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                        throw std::runtime_error("Renderer fixture texture exceeds host addressing");
                    std::vector<std::byte> texture_bytes(static_cast<std::size_t>(bytes));
                    Require(input.ReadVerified(0, texture_bytes, expected));
                    packed_texture_storage.push_back(hs::render_data::CompileTextureDescriptor(texture_bytes, asset,
                        static_cast<std::uint32_t>(packed_directory.size() - 1)));
                    if (!pack.write(reinterpret_cast<const char *>(texture_bytes.data()), static_cast<std::streamsize>(bytes)))
                        throw std::runtime_error("Could not write renderer fixture texture: " + id);
                    offset += bytes;
                    continue;
                }
                if (id == "vfx/vfx_mesh_atlas.hsbin")
                {
                    const auto atlas_bytes = verified_product(id);
                    packed_vfx_mesh_atlas = hs::render_data::CompileVfxMeshAtlasDescriptor(atlas_bytes, asset);
                    if (atlas_bytes.size() != bytes ||
                        !pack.write(reinterpret_cast<const char *>(atlas_bytes.data()), static_cast<std::streamsize>(bytes)))
                        throw std::runtime_error("Could not write verified VFX mesh atlas");
                    offset += bytes;
                    continue;
                }
                std::ifstream input(generation.product_paths.at(id), std::ios::binary);
                for (std::uint64_t remaining = bytes; remaining;)
                {
                    const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
                    if (!input.read(buffer.data(), count) || !pack.write(buffer.data(), count))
                        throw std::runtime_error("Could not copy renderer fixture bulk: " + id);
                    remaining -= count;
                }
                offset += bytes;
            }
            pack.close();
            if (!pack) throw std::runtime_error("Could not write renderer fixture pack");
            auto owner = std::make_shared<hs::ShippedContentOwner>();
            Require(hs::ShippedContentOwner::Open(pack_path, *owner));
            config.packed_content = std::move(owner);
            config.packed_bulks = packed_directory;
            for (const auto &texture : packed_texture_storage) packed_textures.push_back(texture.View());
            config.packed_textures = packed_textures;
            for (const auto &geometry : packed_geometry_storage) packed_geometry.push_back(geometry.View());
            config.packed_geometry = packed_geometry;
            std::vector<hs::AssetId> bulk_assets;
            for (const auto &bulk : packed_directory) bulk_assets.push_back(bulk.asset_id);
            for (std::size_t index = 0; index < products.size(); ++index)
            {
                const auto &id = products[index].second;
                if (!hs::render_data::IsMaterialDescriptorProduct(id)) continue;
                const auto parameters = hs::render_data::MaterialParametersProduct(id);
                packed_material_storage.push_back(hs::render_data::CompileMaterialDescriptor(id,
                    metadata_buffers.at(id), metadata_buffers.at(parameters), static_cast<std::uint32_t>(index),
                    bulk_assets, packed_textures));
            }
            for (const auto &material : packed_material_storage) packed_materials.push_back(material.View());
            config.packed_materials = packed_materials;
            const auto character_bulk = [&](hs::AssetId id) {
                const auto found = std::ranges::find(bulk_assets, id.value, &hs::AssetId::value);
                if (found == bulk_assets.end()) throw std::runtime_error("Character fixture bulk missing");
                return static_cast<std::uint32_t>(found - bulk_assets.begin());
            };
            for (auto &prepared : geometry_catalog.TakeCharacters())
                packed_character_storage.push_back(hs::render_data::CompileCharacterDescriptor(prepared.bytes,
                    prepared.package, std::move(prepared.data), character_bulk(prepared.package.asset_id),
                    packed_directory, packed_geometry, packed_materials));
            for (const auto &character : packed_character_storage) packed_characters.push_back(character.View());
            for (auto &prepared : geometry_catalog.TakeAnimations())
            {
                const auto found = std::ranges::find(packed_characters, prepared.data.character_asset_id.value,
                    [](const auto &character) { return character.asset_id.value; });
                if (found == packed_characters.end()) throw std::runtime_error("Animation fixture character missing");
                const auto index = static_cast<std::uint32_t>(found - packed_characters.begin());
                packed_animation_storage.push_back(hs::render_data::CompileAnimationDescriptor(prepared.bytes,
                    prepared.package, std::move(prepared.data), character_bulk(prepared.package.asset_id),
                    packed_directory, hs::render_data::CharacterHandle{index}, packed_character_storage[index]));
            }
            for (const auto &animation : packed_animation_storage) packed_animations.push_back(animation.View());
            config.packed_characters = packed_characters;
            config.packed_animations = packed_animations;
            config.packed_vfx_mesh_atlas = &packed_vfx_mesh_atlas;
            // Canonical names borrow the selected Generation, which outlives the renderer.
            for (const auto &[id, path] : generation.product_paths)
                if (!id.starts_with("render/") && id != "vfx/vfx_mesh_atlas.hsbin")
                    packed_source_revisions.push_back({hs::MakeAssetId(id), id, digest_bytes(id)});
            std::ranges::sort(packed_source_revisions, {}, [](const auto &revision) { return revision.asset_id.value; });
            for (std::size_t i = 0; i < packed_source_revisions.size(); ++i)
            {
                const auto &revision = packed_source_revisions[i];
                if (i && packed_source_revisions[i-1].asset_id.value == revision.asset_id.value)
                    throw std::runtime_error("Fixture provenance AssetId collision");
                if (std::ranges::find(bulk_assets, revision.asset_id.value, &hs::AssetId::value) != bulk_assets.end())
                    throw std::runtime_error("Fixture provenance collides with bulk AssetId");
            }
            for (std::size_t index = 0; index < products.size(); ++index)
                if (hs::render_data::IsRendererMaterialRevisionProduct(products[index].second))
                {
                    // products is local to this block; borrow the persistent map key.
                    const auto found = generation.product_paths.find(products[index].second);
                    packed_revision_bulks.push_back({found->first, static_cast<std::uint32_t>(index)});
                }
            config.packed_source_revisions = packed_source_revisions;
            config.packed_revision_bulks = packed_revision_bulks;
            config.product_paths.clear();
            config.product_digests.clear();
            receipt["packed_generation_maps_empty"] = config.product_paths.empty() && config.product_digests.empty();
            receipt["packed_quality_profiles"] = true;
            receipt["packed_renderer_products"] = packed_directory.size();
            receipt["packed_renderer_textures"] = packed_textures.size();
            receipt["packed_renderer_geometry"] = packed_geometry.size();
            receipt["packed_renderer_materials"] = packed_materials.size();
            receipt["packed_renderer_characters"] = packed_characters.size();
            receipt["packed_renderer_animations"] = packed_animations.size();
            receipt["packed_renderer_source_revisions"] = packed_source_revisions.size();
            receipt["packed_renderer_revision_bulks"] = packed_revision_bulks.size();
            receipt["packed_renderer_volumes"] = std::ranges::count(packed_textures,
                hs::render_data::TextureDimension::Texture3D, &hs::render_data::TextureDescriptor::dimension);
            receipt["packed_renderer_bytes"] = offset;
        }
        receipt["validation_requested"] = {{"debug_layer", config.validation},
            {"gpu_based_validation", config.gpu_validation}};
        measure("initialize", [&] {
            const auto initialized = renderer.Initialize(config);
            const auto status = renderer.ValidationStatus();
            receipt["validation_status"] = ValidationStatusReceipt(status);
            if (!initialized && status.debug_interface_acquisition_attempted &&
                ((status.debug_layer_requested && !status.debug_interface_acquired) ||
                 (status.gpu_validation_requested &&
                    (!status.debug_interface_acquired || !status.gpu_validation_interface_queried))))
                failure_code = status.gpu_validation_requested
                    ? "HS_RENDER_GBV_PREREQUISITE_FAILED" : "HS_RENDER_DEBUG_LAYER_PREREQUISITE_FAILED";
            Require(initialized);
        });
        config.font_bytes = {};
        std::vector<std::byte>().swap(startup_font);
        hs::LightView static_shadow_sun;
        if (static_shadow_bake_probe)
        {
            static_shadow_sun.direction = hs::kEnvironmentSunDirection;
            static_shadow_sun.quantity = 50000.0f;
            static_shadow_sun.color = {1.0f, .94f, .86f};
            static_shadow_sun.stable_id = 0x535342414b450001ull;
            static_shadow_sun.shadow_flags = hs::kLightShadowRequested;
            failure_code = "HS_RENDER_STATIC_SHADOW_BAKE_FAILED";
            measure("static_shadow_bake", [&] {
                receipt["static_shadow_bake"] = StaticShadowArchiveProbe(renderer, world_scene, static_shadow_sun);
            });
            receipt["static_shadow_bake"]["bake_only"] = static_shadow_bake_only;
            if (static_shadow_bake_only)
            {
                receipt["static_shadow_bake"]["subsequent_render_frames"] = 0;
                receipt["frames"] = 0;
                failure_code = "HS_RENDER_SHUTDOWN_FAILED";
                measure("shutdown", [&] { Require(renderer.Shutdown()); });
                const auto errors = renderer.ValidationErrorCount();
                receipt["validation_errors"] = errors;
                receipt["timings_ms"] = std::move(timings);
                receipt["execution_valid"] = errors == 0;
                receipt["error_code"] = errors == 0 ? "" : "HS_RENDER_VALIDATION_FAILED";
                exit_code = errors == 0 ? 0 : 1;
                return write_receipt();
            }
        }
        hs::GraphicsSettings switch_settings;
        bool switch_sr{}, switch_fg{};
        const bool switch_auto_probe = provider_switch_probe && frames >= 15;
        const auto switch_restore_frame = all_providers_probe ? 19u :
            switch_auto_probe ? frames - 1 : 6u;
        if (provider_switch_probe)
        {
            const auto capabilities = renderer.GetGraphicsCapabilities();
            const auto capability = [&](std::string_view option) {
                const auto found = std::ranges::find_if(capabilities.options, [&](const auto &entry) {
                    return entry.option.value == option;
                });
                if (found == capabilities.options.end())
                    throw std::runtime_error("GetGraphicsCapabilities omitted required option " +
                                             std::string(option));
                return *found;
            };
            const auto dlss_sr_capability = capability("sr_provider:dlss");
            const auto dlss_fg_capability = capability("fg_provider:dlss");
            switch_sr = dlss_sr_capability.available;
            switch_fg = dlss_fg_capability.available;
            switch_settings = renderer.GetGraphicsSettingsStatus().requested;
            receipt["provider_switch"] = {{"startup_sr", "internal_taau"}, {"startup_fg", "off"},
                {"dlss_sr_supported", switch_sr}, {"dlss_fg_supported", switch_fg},
                {"auto_cadence", {{"exercised", switch_auto_probe && switch_fg},
                    {"policy_source", "selected_generation/render/config/quality_profiles.rpkg"},
                    {"suppression_observed", false}, {"first_suppression_frame", nullptr},
                    {"recovery_exercised", false}}},
                {"frames", Json::array()}};
            if (all_providers_probe)
            {
                receipt["provider_switch"]["all_providers"] = true;
                receipt["provider_switch"]["first_restore_frame"] = 19;
            }
            for (const auto &entry : capabilities.options)
                receipt["provider_switch"]["capabilities"].push_back({{"option", entry.option.value},
                    {"available", entry.available}, {"reason", static_cast<unsigned>(entry.reason)}});
            if (all_providers_probe)
            {
                const auto add_capability = [&](std::string_view category, std::string_view provider) {
                    const auto option = std::string(category) + ":" + std::string(provider);
                    const auto entry = capability(option);
                    receipt["provider_switch"]["capability_map"][std::string(category)][std::string(provider)] = {
                        {"option", option}, {"available", entry.available},
                        {"reason", static_cast<unsigned>(entry.reason)},
                        {"reason_name", GraphicsCapabilityReasonName(entry.reason)}};
                    if (entry.reason == hs::GraphicsCapabilityReason::MissingProvider)
                        throw std::runtime_error("Required provider capability is missing: " + option);
                    return entry.available;
                };
                for (const auto provider : {"dlss", "xess", "fsr", "directsr"})
                    add_capability("sr_provider", provider);
                for (const auto provider : {"dlss", "xess", "fsr"})
                    add_capability("fg_provider", provider);
            }
        }
        if (residency_streaming_probe)
        {
            failure_code = "HS_RENDER_RESIDENCY_STREAMING_FAILED";
            const auto initial_residency = renderer.ResidencyStats();
            if (initial_residency.geometry_streaming_requests != 0 ||
                initial_residency.geometry_streaming_uploads != 0 ||
                initial_residency.geometry_streaming_publications != 0)
                throw std::runtime_error("Geometry streamed before any scene demand.");
            receipt["geometry_initial_requests"] = initial_residency.geometry_streaming_requests;
            auto missing_paths = generation.product_paths;
            auto changed_digests = generation.product_digests;
            const std::string probe_id = "render/lighting/default_probe.rpkg";
            missing_paths.at(probe_id) = artifacts / "absent-developer-probe.rpkg";
            auto &probe_digest = changed_digests.at(probe_id);
            probe_digest.back() = probe_digest.back() == '0' ? '1' : '0';
            bool resident_replaced = false;
            const auto rejected = renderer.ReloadContentGeneration(generation.id + "-missing-probe", missing_paths,
                changed_digests, std::make_shared<const hs::SelectedGeneration>(generation), nullptr, &resident_replaced);
            const std::string rejection_message(rejected.Message());
            if (rejected || resident_replaced ||
                (rejection_message.find("default_probe") == std::string::npos &&
                 rejection_message.find("absent-developer-probe") == std::string::npos))
                throw std::runtime_error("Developer reload did not reject the actual missing probe while preserving the packed resident.");
            receipt["packed_developer_reload_missing_product_rejected"] = true;
            receipt["packed_developer_reload_rejection"] = rejection_message;
            receipt["packed_developer_reload_resident_preserved"] = !resident_replaced;
        }
        const auto scene = Scene(generation.id);
        const auto grid_floor_scene = GridFloorScene(generation.id);
        const auto hzb_scene = HzbScene(generation.id);
        hs::RenderInstance first;
        first.mesh = hs::RenderMesh::Archer;
        first.scale = {.97174913f, .97174913f, .97174913f};
        first.stable_id = 1;
        if (entity_surface_probe)
        {
            first.mesh = hs::RenderMesh::BossFiveMinute;
            first.scale = {2, 2, 2};
        }
        if (grid_floor_probe)
        {
            first.position = {0, 0, 3};
            first.environment_seed = 0x47524944u;
        }
        hs::RenderInstance second = first;
        second.stable_id = 2;
        second.mesh = hs::RenderMesh::Rock;
        second.environment_variant = 0;
        second.position.x = 2.0f;
        hs::RenderInstance gpu_tree = first;
        gpu_tree.stable_id = 2;
        gpu_tree.mesh = hs::RenderMesh::TreeTrunk;
        gpu_tree.position = {-1.5f, 0.0f, 2.5f};
        gpu_tree.scale = {.02f, .02f, .02f};
        hs::RenderInstance gpu_rock = second;
        gpu_rock.stable_id = 3;
        gpu_rock.position = {2.0f, 0.0f, 1.5f};
        gpu_rock.scale = {.8f, .8f, .8f};
        hs::RenderInstance replacement = first;
        replacement.stable_id = 3;
        replacement.mesh = hs::RenderMesh::Rock;
        replacement.environment_variant = 0;
        replacement.position.x = -2.0f;
        hs::RenderInstance occluder;
        occluder.stable_id = 99;
        // Area flattens cube height by 0.1 and sits below the target's sightline.
        // Use the opaque builtin box across the orbit camera's target projection.
        occluder.mesh = hs::RenderMesh::Enemy;
        occluder.position = {0.0f, 5.0f, -9.25f};
        occluder.scale = {12.0f, 16.0f, 3.0f};
        occluder.color_rgba = 0xff303030u;
        std::vector<hs::RenderInstance> previous;
        std::vector<hs::RenderInstance> current =
            grid_floor_probe ? std::vector<hs::RenderInstance>{first}
            : persistent_scene_probe || dynamic_lod_skin_growth_probe
                                   ? std::vector<hs::RenderInstance>{first, second}
            : gpu_lod_probe ? std::vector<hs::RenderInstance>{first, gpu_tree, gpu_rock}
                            : std::vector<hs::RenderInstance>{first};
        if (dynamic_lod_skin_growth_probe)
        {
            current[1].mesh = hs::RenderMesh::TreeTrunk;
            current[1].position = {};
            current[1].scale = {1, 1, 1};
        }
        if (persistent_scene_probe)
            for (auto &source : current) source.revisions = {11, 22, 33, 44};
        hs::RenderSnapshotPair pair;
        pair.has_current = true;
        pair.current.static_scene = world_transport_probe || owned_view_probe || gameplay_scene ? world_scene
            : grid_floor_probe ? grid_floor_scene : scene;
        // The camera-basis oracle measures an unoccluded silhouette. Ordinary
        // fixture terrain/rocks otherwise occlude different pixels after roll.
        if (camera_basis_probe) pair.current.static_scene.reset();
        if (entity_surface_probe) pair.current.static_scene.reset();
        if (!grass_history_probe.empty())
        {
            auto grass_scene = std::make_unique<hs::StaticRenderSceneStorage>(*scene);
            std::erase_if(grass_scene->instances, [](const auto &instance) {
                return instance.mesh != hs::RenderMesh::Ground && instance.mesh != hs::RenderMesh::Grass;
            });
            for (auto &instance : grass_scene->instances)
                if (instance.mesh == hs::RenderMesh::Grass)
                {
                    instance.position.x *= 0.35f;
                    instance.scale = {2, 2, 2};
                }
            pair.current.static_scene = hs::FreezeStaticRenderScene(std::move(grass_scene));
            auto dynamic_grass = pair.current.static_scene->instances.back();
            dynamic_grass.stable_id = 0x4752415353ull;
            dynamic_grass.position = {0, 0, -5};
            current.push_back(dynamic_grass);
        }
        pair.current.header.session_id = 1;
        constexpr std::uint64_t kShadowHistoryCasterId = 0x5348434153540001ull;
        hs::RenderInstance shadow_history_caster;
        std::array<hs::PersistentVfxVisual, 3> authored_owner_fixture{};
        if (local_probe_revision_probe)
        {
            constexpr std::array kinds{hs::PersistentVfxKind::WaveTimer,
                hs::PersistentVfxKind::PickupXpIdle, hs::PersistentVfxKind::PlayerBowDraw};
            for (std::size_t index = 0; index < kinds.size(); ++index)
            {
                auto &owner = authored_owner_fixture[index];
                owner.kind = kinds[index];
                owner.stable_id = 0x415554484f520001ull + index;
                owner.position = {0.0f, 0.025f, 0.0f};
                owner.radius = index == 0 ? 40.0f : 0.34f;
                owner.source_horizon_tick = 10800;
            }
            // Authored snapshot owners without typed outputs must not become legacy white quads.
            pair.current.persistent_vfx = authored_owner_fixture;
            auto fixture = std::make_unique<hs::StaticRenderSceneStorage>(*world_scene);
            const auto rock = std::ranges::find(fixture->instances, hs::RenderMesh::Rock,
                &hs::RenderInstance::mesh);
            if (rock == fixture->instances.end() || std::ranges::any_of(fixture->instances,
                    [](const auto &instance) { return instance.stable_id == kShadowHistoryCasterId; }))
                throw std::runtime_error("Shadow history fixture requires a rock and an unused stable ID.");
            shadow_history_caster = *rock;
            shadow_history_caster.position = {3.0f, 0.0f, 3.0f};
            shadow_history_caster.stable_id = kShadowHistoryCasterId;
            ++shadow_history_caster.revisions.transform;
            fixture->instances.push_back(shadow_history_caster);
            // Appended leaf casters leave the authenticated HLOD members and digest unchanged.
            auto anonymous = shadow_history_caster;
            anonymous.stable_id = 0;
            anonymous.position.x = -3.0f;
            fixture->instances.push_back(anonymous);
            anonymous.position.z = -3.0f;
            fixture->instances.push_back(anonymous);
            anonymous.position.x = 3.0f;
            current.push_back(anonymous);
            pair.current.static_scene = hs::FreezeStaticRenderScene(std::move(fixture));
        }
        pair.current.camera.target = {0, 1, 0};
        pair.current.camera.yaw_degrees = 0;
        pair.current.camera.pitch_degrees = 22;
        pair.current.camera.distance = dynamic_lod_skin_growth_probe ? 28.0f : 22.0f;
        if (hero_work_probe && gameplay_scene)
        {
            const auto water = std::ranges::find_if(world_scene->instances, [](const auto &instance) {
                return instance.mesh == hs::RenderMesh::MeadowSurface && instance.environment_variant == 1;
            });
            if (water == world_scene->instances.end() || water->yaw != 0 || water->scale.x != 1 || water->scale.y != 1 || water->scale.z != 1)
                throw std::runtime_error("Hero water fixture requires the actual identity-transformed authored MeadowSurface water instance");
            const hs::MeadowDepression *pool{};
            for (const auto &candidate : hs::kMeadowDepressions)
                if (std::isfinite(hs::MeadowWaterLevel({candidate.x, candidate.z})) &&
                    (!pool || candidate.rx * candidate.rz > pool->rx * pool->rz)) pool = &candidate;
            if (!pool) throw std::runtime_error("Gameplay layout has no finite authored water depression");
            const auto level = hs::MeadowWaterLevel({pool->x, pool->z});
            pair.current.camera.target = {water->position.x + pool->x, water->position.y + level, water->position.z + pool->z};
            pair.current.camera.pitch_degrees = 55;
            pair.current.camera.distance = 7;
            receipt["hero_work"]["water_camera"] = {{"water_instance_stable_id", water->stable_id},
                {"mesh", static_cast<unsigned>(water->mesh)}, {"variant", water->environment_variant},
                {"local_pool_center", {pool->x, level, pool->z}}, {"radii", {pool->rx, pool->rz}},
                {"target", {pair.current.camera.target.x, pair.current.camera.target.y, pair.current.camera.target.z}},
                {"pitch_degrees", pair.current.camera.pitch_degrees}, {"distance", pair.current.camera.distance}};
            receipt["hero_water_shadow"] = {{"frames", Json::array()},
                {"baked_products_selected", generation.product_paths.contains("render/lighting/arena_static_shadow.index") &&
                    generation.product_paths.contains("render/lighting/arena_static_shadow.pages")},
                {"require_positive_transition", shadow_transition_probe},
                {"fixture_settings_reason", shadow_transition_probe ? "dedicated S01 supported-surface fixture; VM Off and GI Probe; production/default water settings unchanged" : "standard water settings unchanged; positive transition may remain uncovered"},
                {"configured_volumetric_quality", static_cast<unsigned>(config.volumetric_quality)},
                {"configured_volumetric_quality_token", std::string(hs::GraphicsEnumToken(config.volumetric_quality))},
                {"configured_gi_mode", std::string(hs::GraphicsEnumToken(config.gi_mode))},
                {"policy_label", "current per-submission CPU plan joined to completed GPU frame/view"}};
        }
        std::array<hs::LightView, 3> world_lights{};
        std::vector<hs::LightView> light_list_lights;
        if (static_shadow_bake_probe) pair.current.lights = std::span(&static_shadow_sun, 1);
        if (hero_work_probe && gameplay_scene)
        {
            world_lights[0].stable_id = 0x485753554e000001ull;
            world_lights[0].direction = hs::kEnvironmentSunDirection;
            world_lights[0].quantity = 50000.0f; world_lights[0].color = {1.0f, .94f, .86f};
            world_lights[0].shadow_flags = hs::kLightShadowRequested;
            pair.current.lights = std::span(world_lights).first(1);
        }
        if (world_transport_probe || local_probe_revision_probe)
        {
            auto &sun = world_lights[0];
            sun.stable_id = 0x57544c0000000001ull;
            sun.direction = {.3f, -1.0f, .2f};
            sun.quantity = 50000.0f;
            sun.color = {1.0f, .94f, .86f};
            sun.shadow_flags = hs::kLightShadowRequested;
            auto &point = world_lights[1];
            point.stable_id = 0x57544c0000000002ull;
            point.type = hs::LightType::Point;
            point.unit = hs::LightUnit::Lumen;
            point.position = {0.0f, 3.0f, 2.0f};
            point.quantity = 3000.0f;
            point.color = {.45f, .7f, 1.0f};
            point.range = 12.0f;
            point.emitter_radius = .15f;
            point.shadow_flags = hs::kLightShadowRequested;
            auto &spot = world_lights[2];
            spot.stable_id = 0x57544c0000000003ull;
            spot.type = hs::LightType::Spot; spot.unit = hs::LightUnit::Lumen;
            spot.position = {-4.0f, 5.0f, 0.0f}; spot.direction = {.45f, -.8f, .2f};
            spot.quantity = 2000.0f; spot.color = {1.0f, .55f, .25f}; spot.range = 15.0f;
            spot.inner_cone_radians = .3f; spot.outer_cone_radians = .65f;
            spot.emitter_radius = .08f; spot.shadow_flags = hs::kLightShadowRequested;
            pair.current.lights = world_lights;
        }
        hs::RendererFrameResult frame_result;
        std::map<std::uint64_t, hs::GpuPassTimings> timing_stream_history;
        std::uint32_t timing_stream_live{}, timing_stream_drained{};
        const auto collect_timing_stream = [&](bool drain) {
            std::vector<hs::GpuPassTimings> completed;
            Require(drain ? renderer.DrainGpuPassTimings(completed)
                          : renderer.ConsumeCompletedGpuPassTimings(completed));
            if (drain) timing_stream_drained += static_cast<std::uint32_t>(completed.size());
            else timing_stream_live += static_cast<std::uint32_t>(completed.size());
            std::uint64_t previous{};
            for (auto &sample : completed)
            {
                if (!sample.valid || !sample.fence_completed || !sample.cpu_qpc_frequency ||
                    sample.view_id != hs::kMainRenderViewId.value || !sample.completed_frame_id ||
                    sample.completed_frame_id <= previous)
                    throw std::runtime_error("Timing stream emitted incomplete or unsorted frame identities");
                previous = sample.completed_frame_id;
                if (timing_stream_critical_probe)
                {
                    const auto begin = std::ranges::find(sample.passes, 37u, &hs::GpuPassTiming::pass_id);
                    const auto end = std::ranges::find(sample.passes, 38u, &hs::GpuPassTiming::pass_id);
                    if (sample.mode != hs::GpuTimingMode::Critical ||
                        sample.metric_kind != hs::GpuFrameTimingKind::ReferenceGraphSpan ||
                        !sample.critical_path_valid || !sample.cpu_recording_valid ||
                        sample.pass_timings_valid || sample.passes.size() != 2 ||
                        begin == sample.passes.end() || end == sample.passes.end() ||
                        begin->queue != hs::RenderQueue::Direct || end->queue != hs::RenderQueue::Direct ||
                        !begin->measured || !end->measured || !begin->name.empty() || !end->name.empty() ||
                        begin->cpu_record_valid || end->cpu_record_valid)
                        throw std::runtime_error("Critical timing stream did not retain only Direct endpoints and frame-level CPU timing");
                }
                else if (timing_stream_probe && !config.dynamic_resolution)
                {
                    const auto ready = std::ranges::find(sample.passes, 38u, &hs::GpuPassTiming::pass_id);
                    if (sample.metric_kind != hs::GpuFrameTimingKind::SourceOutputReady ||
                        sample.critical_path_valid || !sample.output_ready_valid || !sample.main_view ||
                        !sample.metric_generation || !sample.resize_epoch || !sample.output_ready_ticks ||
                        !sample.output_ready_frequency || frame_result.views.size() != 1 ||
                        sample.render_active != frame_result.views.front().rendered_view.render_active ||
                        std::ranges::any_of(sample.passes, [](const auto &pass) { return pass.pass_id == 37; }) ||
                        ready == sample.passes.end() || ready->name != "SourceRenderOutputReady" ||
                        ready->queue != hs::RenderQueue::Direct || !ready->measured ||
                        sample.output_ready_ticks != ready->end_ticks ||
                        sample.output_ready_frequency != ready->timestamp_frequency)
                        throw std::runtime_error("Timing stream lost the natural source-output-ready contract");
                    if (sample.output_ready_clock_calibrated &&
                        (!ready->clock_calibrated || !sample.output_ready_qpc_frequency ||
                         sample.output_ready_qpc != ready->end_qpc ||
                         sample.output_ready_qpc_frequency != ready->qpc_frequency))
                        throw std::runtime_error("Timing stream ready endpoint has inconsistent clock calibration");
                }
                if (!timing_stream_critical_probe)
                    for (const auto &pass : sample.passes)
                        if (!pass.cpu_record_valid || pass.cpu_record_begin_qpc == 0 ||
                            pass.cpu_record_end_qpc < pass.cpu_record_begin_qpc)
                            throw std::runtime_error("Timing stream lost per-pass CPU recording interval");
                if (!timing_stream_history.emplace(sample.completed_frame_id, std::move(sample)).second)
                    throw std::runtime_error("Timing stream emitted a duplicate completed frame");
            }
        };
        failure_code = "HS_RENDER_FRAME_FAILED";
        hs::RendererQueueStats queue_totals;
        std::map<std::uint64_t, hs::GpuPassTimings> world_gpu_history;
        std::map<std::uint64_t, hs::GpuPassTimings> provider_gpu_history;
        std::array<std::uint32_t, 3> probe_capture_frames{};
        std::uint64_t observed_probe_frame{};
        std::uint32_t settled_probe_frames{};
        const auto observe_probe_revisions = [&] {
            if (!local_probe_revision_probe) return;
            const auto timings = renderer.LastGpuPassTimings();
            if (!timings.valid || timings.completed_frame_id <= observed_probe_frame) return;
            observed_probe_frame = timings.completed_frame_id;
            const auto capture = std::ranges::any_of(timings.passes, [](const auto &pass) {
                return pass.name == "LocalProbeStaticCubeCapture" && pass.measured;
            });
            const auto phase = std::min<std::uint64_t>((observed_probe_frame - 1) / 32, 2);
            if (capture) ++probe_capture_frames[phase];
            if (observed_probe_frame >= 24 && observed_probe_frame <= 32 && !capture) ++settled_probe_frames;
        };
        std::uint32_t fsr_fg_suppressed{}, fsr_fg_insufficient{}, fsr_fg_generated_evidence_count{};
        bool fsr_fg_callback_observed{}, fsr_fg_generated_evidence{};
        const auto observe_world_timings = [&] {
            if (!world_transport_probe) return;
            auto completed = renderer.LastGpuPassTimings();
            if (completed.valid && completed.completed_frame_id != 0)
                world_gpu_history.insert_or_assign(completed.completed_frame_id, std::move(completed));
        };
        const auto observe_provider_timings = [&] {
            if (!provider_switch_probe) return;
            auto completed = renderer.LastGpuPassTimings();
            if (completed.valid && completed.completed_frame_id != 0)
                provider_gpu_history.insert_or_assign(completed.completed_frame_id, std::move(completed));
        };
        const auto initial_material_parameter_revision = renderer.MaterialParameterRevision();
        const auto initial_pipeline_revision = renderer.PipelineRevision();
        std::uint64_t hero_quality_generation{};
        bool hero_quality_valid = true;
        if (hero_layer_probe)
        {
            auto status = renderer.GetGraphicsSettingsStatus();
            const auto before = status;
            status.requested.transparency_quality = hs::GraphicsQuality::Medium;
            status.requested.refraction_quality = hs::GraphicsOptionalQuality::Medium;
            hero_quality_generation = status.generation + 1;
            const auto requested = renderer.RequestGraphicsSettings({hero_quality_generation,
                hs::GraphicsSettingsRequestCause::User, status.requested});
            const auto observed = renderer.GetGraphicsSettingsStatus();
            receipt["hero_work"]["quality_transition"]["baseline_request"] = {
                {"before_generation", before.generation}, {"before_state", static_cast<unsigned>(before.state)},
                {"before_actual_available", before.actual_applied_available}, {"generation", hero_quality_generation},
                {"succeeded", requested.Succeeded()}, {"error", requested.Succeeded() ? "" : std::string(requested.Message())},
                {"requested_transparency_quality", static_cast<unsigned>(status.requested.transparency_quality)},
                {"requested_refraction_quality", static_cast<unsigned>(status.requested.refraction_quality)},
                {"observed_generation", observed.generation}, {"state", static_cast<unsigned>(observed.state)},
                {"status_error", static_cast<unsigned>(observed.error)}, {"actual_applied_available", observed.actual_applied_available},
                {"actual_transparency_quality", observed.actual_applied_available ? Json(static_cast<unsigned>(observed.actual_applied.transparency_quality)) : Json(nullptr)},
                {"actual_refraction_quality", observed.actual_applied_available ? Json(static_cast<unsigned>(observed.actual_applied.refraction_quality)) : Json(nullptr)}};
            Require(requested);
        }
        if (material_reload_probe)
        {
            receipt["material_reload"] = {
                {"baseline_generation", generation.id},
                {"valid_generation", material_reload_generation.id},
                {"invalid_generation", material_reload_invalid_generation.id},
                {"initial_material_parameter_revision", initial_material_parameter_revision},
                {"initial_pipeline_revision", initial_pipeline_revision},
                {"captures", Json::object()},
            };
        }
        measure("render", [&] {
            if (owned_view_probe)
            {
                pair.current.instances = current;
                pair.current.header.tick = 1;
                hs::render_test::OwnedViewProbeEvidence evidence;
                Require(hs::render_test::RunOwnedViewProbe(renderer, pair, artifacts,
                    owned_view_archer_asset, evidence));
                frame_result = evidence.final_frame;
                receipt["owned_view_probe"] = hs::render_test::OwnedViewProbeReceipt(evidence);
                receipt["queue_execution_scope"] = "last_owned_view_submission";
                queue_totals = renderer.LastQueueStats();
                return;
            }
            for (std::uint32_t frame = 1; frame <= frames; ++frame)
            {
                if (hero_layer_probe && frame == 5)
                {
                    auto status = renderer.GetGraphicsSettingsStatus();
                    status.requested.transparency_quality = hs::GraphicsQuality::Ultra;
                    status.requested.refraction_quality = hs::GraphicsOptionalQuality::Ultra;
                    hero_quality_generation = status.generation + 1;
                    const auto requested = renderer.RequestGraphicsSettings({hero_quality_generation,
                        hs::GraphicsSettingsRequestCause::User, status.requested});
                    const auto observed = renderer.GetGraphicsSettingsStatus();
                    receipt["hero_work"]["quality_transition"]["request"] = {{"generation", hero_quality_generation},
                        {"succeeded", requested.Succeeded()}, {"error", requested.Succeeded() ? "" : std::string(requested.Message())},
                        {"observed_generation", observed.generation}, {"state", static_cast<unsigned>(observed.state)},
                        {"status_error", static_cast<unsigned>(observed.error)}};
                    Require(requested);
                }
                if (hero_work_probe && gameplay_scene)
                {
                    world_lights[0].direction = hs::kEnvironmentSunDirection;
                    if (frame == 2) world_lights[0].direction.x += .125f;
                }
                if (vfx_composition_probe == "switch" && (frame == 13 || frame == 21))
                {
                    auto status = renderer.GetGraphicsSettingsStatus();
                    status.requested.volumetric_quality = frame == 13
                        ? hs::GraphicsOptionalQuality::Medium : hs::GraphicsOptionalQuality::Off;
                    Require(renderer.RequestGraphicsSettings({status.generation + 1,
                        hs::GraphicsSettingsRequestCause::User, status.requested}));
                }
                if (lighting_auxiliary_probe && frame > 1)
                {
                    auto status = renderer.GetGraphicsSettingsStatus();
                    status.requested.reflection_mode = frame % 2 == 0
                        ? hs::GraphicsReflectionMode::ScreenSpace : hs::GraphicsReflectionMode::Off;
                    measure(std::format("auxiliary_settings_{}", frame).c_str(), [&] {
                        Require(renderer.RequestGraphicsSettings({status.generation + 1,
                            hs::GraphicsSettingsRequestCause::User, status.requested}));
                    });
                    if (frame == 3 || frame == 4)
                        measure(std::format("auxiliary_resize_{}", frame).c_str(), [&] {
                            Require(renderer.Resize(frame == 3 ? 1279 : config.width,
                                frame == 3 ? 719 : config.height));
                        });
                }
                const bool provider_switch_frame = provider_switch_probe && (all_providers_probe
                    ? (frame == 2 || frame == 4 || frame == 6 || frame == 19 ||
                       frame == 21 || frame == 23 || frame == 25 || frame == 27 ||
                       frame == 29 || frame == 32 || frame == 34 || frame == 37)
                    : (frame == 2 || frame == 4 || frame == 6 || frame == switch_restore_frame));
                if (provider_switch_frame)
                {
                    if (all_providers_probe)
                    {
                        if (frame == 2)
                            switch_settings.reconstruction_provider.value = "dlss";
                        else if (frame == 4)
                        {
                            switch_settings.frame_generation_provider.value = "dlss";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Fixed;
                            switch_settings.frame_generation_multiplier = 2;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::On;
                        }
                        else if (frame == 6)
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Auto;
                        else if (frame == 19 || frame == 27 || frame == 32 || frame == 37)
                        {
                            switch_settings.reconstruction_provider.value = "internal_taau";
                            switch_settings.frame_generation_provider.value = "off";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Off;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::Off;
                        }
                        else if (frame == 21)
                        {
                            switch_settings.reconstruction_provider.value = "xess";
                            switch_settings.frame_generation_provider.value = "off";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Off;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::Off;
                        }
                        else if (frame == 23)
                        {
                            switch_settings.reconstruction_provider.value = "fsr";
                            switch_settings.frame_generation_provider.value = "off";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Off;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::Off;
                        }
                        else if (frame == 25)
                        {
                            switch_settings.reconstruction_provider.value = "directsr";
                            switch_settings.frame_generation_provider.value = "off";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Off;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::Off;
                        }
                        else if (frame == 29)
                        {
                            switch_settings.reconstruction_provider.value = "internal_taau";
                            switch_settings.frame_generation_provider.value = "xess";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Fixed;
                            switch_settings.frame_generation_multiplier = 2;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::On;
                        }
                        else if (frame == 34)
                        {
                            switch_settings.reconstruction_provider.value = "internal_taau";
                            switch_settings.frame_generation_provider.value = "fsr";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Auto;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::Auto;
                        }
                    }
                    else
                    {
                        if (frame == 2 && switch_sr) switch_settings.reconstruction_provider.value = "dlss";
                        if (frame == 4 && switch_fg)
                        {
                            switch_settings.frame_generation_provider.value = "dlss";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Fixed;
                            switch_settings.frame_generation_multiplier = 2;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::On;
                        }
                        if (frame == 6 && switch_auto_probe && switch_fg)
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Auto;
                        if (frame == switch_restore_frame)
                        {
                            switch_settings.reconstruction_provider.value = "internal_taau";
                            switch_settings.frame_generation_provider.value = "off";
                            switch_settings.frame_generation_mode = hs::GraphicsFrameGenerationMode::Off;
                            switch_settings.low_latency = hs::GraphicsAutoToggle::Off;
                        }
                    }
                    Require(renderer.RequestGraphicsSettings({frame,
                        hs::GraphicsSettingsRequestCause::User, switch_settings}));
                }
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                {
                    if (message.message == WM_QUIT) throw std::runtime_error("window closed");
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                pair.previous = pair.current;
                pair.previous.instances = previous;
                if (hero_layer_probe && frame >= 5)
                {
                    pair.current.camera.yaw_degrees = 90.0f;
                    pair.current.camera.pitch_degrees = 0.0f;
                }
                pair.has_previous = frame > 1;
                if (grid_floor_probe)
                {
                    if (camera_motion && frame >= 10 && frame <= 17)
                        pair.current.camera.target.x += .03f;
                    receipt["grid_floor"]["camera_targets"][std::to_string(frame)] = {
                        pair.current.camera.target.x,
                        pair.current.camera.target.y,
                        pair.current.camera.target.z,
                    };
                }
                else if (persistent_scene_probe)
                {
                    if (frame == 3) current[0].position.x = 1.0f;
                    else if (frame == 4) current.resize(1);
                    else if (frame == 5) current.push_back(replacement);
                    else if (frame == 6) std::ranges::reverse(current);
                    else if (frame == 7)
                    {
                        current = {occluder};
                        pair.current.static_scene = hzb_scene;
                    }
                    else if (frame == 8) current[0].position.x = 30.0f;
                    // Scene publication at frame7 resets Cluster membership. Let
                    // selection/page history settle before a second HZB reveal.
                    else if (config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer && frame == 10)
                        current[0].position = occluder.position;
                    else if (config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer && frame == 11)
                        current[0].position.x = 30.0f;
                }
                else if (dynamic_lod_skin_growth_probe)
                {
                    if (frame == 3)
                    {
                        auto added = first;
                        added.stable_id = 3;
                        added.position.x = -2.0f;
                        current.push_back(added);
                        added.stable_id = 4;
                        added.position.x = 2.0f;
                        current.push_back(added);
                    }
                }
                else if (gpu_lod_probe)
                {
                    if (frame == 5)
                        pair.current.camera.distance += 0.25f;
                    else if (frame >= 6)
                        pair.current.camera.distance -= 0.25f;
                    if (frame == 4)
                    {
                        for (std::uint32_t index = 0; index < 260; ++index)
                        {
                            auto added = gpu_rock;
                            added.stable_id = 100u + index;
                            added.position = {
                                static_cast<float>(index % 20u) * .25f - 2.5f,
                                0.0f,
                                static_cast<float>(index / 20u) * .25f - 2.0f};
                            added.scale = {.01f, .01f, .01f};
                            current.push_back(added);
                        }
                    }
                    else if (frame == 5)
                    {
                        current[1].scale = {.002f, .002f, .002f};
                    }
                    else if (frame == 8)
                    {
                        current.resize(3);
                    }
                }
                else if (surface_motion_resize_fixture)
                {
                    current[0].position.x = static_cast<float>(frame % 60) / 60.f;
                    if (frame == 2)
                        pair.current.camera.yaw_degrees += 6.0f;
                    if (frame == 3)
                        pair.current.camera.target.x += 0.5f;
                }
                else if (material_reload_probe)
                {
                    if (frame == 3)
                    {
                        const auto material_revision = renderer.MaterialParameterRevision();
                        const auto pipeline_revision = renderer.PipelineRevision();
                        const auto rejected = renderer.ReloadShaderGeneration(
                            material_reload_invalid_generation.id,
                            material_reload_invalid_generation.product_paths,
                            material_reload_invalid_generation.product_digests);
                        if (rejected)
                            throw std::runtime_error(
                                "incompatible material parameter layout was accepted");
                        if (renderer.MaterialParameterRevision() != material_revision ||
                            renderer.PipelineRevision() != pipeline_revision)
                            throw std::runtime_error(
                                "incompatible material parameter reload changed renderer revisions");
                        const auto rejection_message = std::string(rejected.Message());
                        if (rejection_message.find("layout") == std::string::npos)
                            throw std::runtime_error(
                                "incompatible material parameter reload failed for an unexpected reason: " +
                                rejection_message);
                        receipt["material_reload"]["rejected"] = {
                            {"succeeded", false},
                            {"subsystem", std::string(rejected.Subsystem())},
                            {"message", rejection_message},
                            {"material_parameter_revision", material_revision},
                            {"pipeline_revision", pipeline_revision},
                        };
                    }
                    else if (frame == 4)
                    {
                        Require(renderer.ReloadShaderGeneration(
                            material_reload_generation.id,
                            material_reload_generation.product_paths,
                            material_reload_generation.product_digests));
                        const auto material_revision = renderer.MaterialParameterRevision();
                        const auto pipeline_revision = renderer.PipelineRevision();
                        if (material_revision != initial_material_parameter_revision + 1 ||
                            pipeline_revision != initial_pipeline_revision)
                            throw std::runtime_error(
                                "valid material parameter reload did not publish a table without PSO rebuild");
                        receipt["material_reload"]["applied"] = {
                            {"material_parameter_revision", material_revision},
                            {"pipeline_revision", pipeline_revision},
                        };
                    }
                    else if (frame == 5)
                    {
                        Require(renderer.Resize(960, 540));
                        const auto material_revision = renderer.MaterialParameterRevision();
                        const auto pipeline_revision = renderer.PipelineRevision();
                        if (material_revision != initial_material_parameter_revision + 1 ||
                            pipeline_revision != initial_pipeline_revision)
                            throw std::runtime_error(
                                "material parameter resize changed published revisions");
                        receipt["material_reload"]["resized"] = {
                            {"width", 960},
                            {"height", 540},
                            {"material_parameter_revision", material_revision},
                            {"pipeline_revision", pipeline_revision},
                        };
                    }
                    else if (frame == 6)
                    {
                        Require(renderer.ReloadShaderGeneration(
                            generation.id, generation.product_paths, generation.product_digests));
                        const auto material_revision = renderer.MaterialParameterRevision();
                        const auto pipeline_revision = renderer.PipelineRevision();
                        if (material_revision != initial_material_parameter_revision + 2 ||
                            pipeline_revision != initial_pipeline_revision)
                            throw std::runtime_error(
                                "baseline material parameter reload did not restore the table without PSO rebuild");
                        receipt["material_reload"]["restored"] = {
                            {"material_parameter_revision", material_revision},
                            {"pipeline_revision", pipeline_revision},
                        };
                    }
                }
                else if (!world_transport_probe)
                {
                    current[0].position.x = static_cast<float>(frame % 60) / 60.f;
                }
                if (!vfx_composition_probe.empty()) current[0].position.x = 0;
                pair.current.header.tick = material_reload_probe || grid_floor_probe ? 1 : frame;
                pair.current.header.simulation_time = material_reload_probe || grid_floor_probe || persistent_scene_probe
                    ? std::chrono::nanoseconds::zero()
                    : std::chrono::nanoseconds(
                        static_cast<std::int64_t>(frame) * 1000000000ll / 60);
                if (!grass_history_probe.empty())
                {
                    const bool wind = grass_history_probe != "A" && grass_history_probe != "C";
                    pair.current.header.simulation_time = std::chrono::nanoseconds(
                        1000000000ll + (wind ? static_cast<std::int64_t>(frame) * 1000000000ll / grass_cadence : 0));
                    current[0].position = grass_history_probe == "D"
                        ? hs::Float3{-2.0f + frame * 0.12f, 0, -6.0f} : hs::Float3{20, 0, 0};
                    current.back().position.x = grass_history_probe == "D" ? frame * 0.015f : 0;
                    pair.current.instances = current;
                }
                observe_world_timings();
                observe_provider_timings();
                if (local_probe_revision_probe && frame == 33) world_lights[0].direction.x += 0.2f;
                if (local_probe_revision_probe && frame == 65)
                {
                    auto changed = std::make_unique<hs::StaticRenderSceneStorage>(*pair.current.static_scene);
                    changed->layout_abi = 0;
                    changed->eligible_layout_digest = {};
                    changed->eligible_static_members.clear();
                    changed->instances.resize(world_scene->instances.size());
                    changed->instances.back().position.x += 1.0f;
                    pair.current.static_scene = hs::FreezeStaticRenderScene(std::move(changed));
                    current.push_back(shadow_history_caster);
                }
                if (local_probe_revision_probe && frame == 80)
                    std::erase_if(current, [](const auto &instance) { return instance.stable_id == 0; });
                if (local_probe_revision_probe && frame == 81)
                    std::erase_if(current, [](const auto &instance) { return instance.stable_id == kShadowHistoryCasterId; });
                pair.current.instances = current;
                if (!grass_history_probe.empty() && (frame == 13 || frame == 14))
                    Require(renderer.Resize(config.width + (frame == 13 ? 64 : 0),
                                            config.height + (frame == 13 ? 32 : 0)));
                auto main_view = renderer.MainViewInput(pair.current.camera);
                std::vector<hs::VfxEntitySurfaceCommand> entity_surfaces;
                if (entity_surface_probe)
                {
                    current[0].position = {};
                    if (frame >= 7) current[0].stable_id = 2;
                    pair.current.instances = frame == 6 ? std::span<const hs::RenderInstance>{}
                        : std::span<const hs::RenderInstance>{current};
                    pair.current.header.simulation_time = std::chrono::nanoseconds::zero();
                    main_view.camera.position = {0, 2, -8};
                    main_view.camera.target = {0, 1, 0};
                    main_view.camera.up = {0, 1, 0};
                    main_view.view_origin = main_view.camera.position;
                    main_view.camera_cut = frame == 1 || frame == 6 || frame == 7;
                    if (frame == 2 || frame == 3 || frame == 8)
                    {
                        auto command = entity_surface_template;
                        command.render_instance_id = command.owner_id = command.event_sequence = 1;
                        command.owner_started_tick = 0; command.current_tick = frame == 2 ? 8 : 43; command.owner_until_tick = 90;
                        command.normalized_phase = static_cast<float>((static_cast<double>(command.current_tick) / 90.0 - .08f) /
                            (static_cast<double>(.88f) - .08f));
                        command.gradient_age = command.normalized_phase;
                        entity_surfaces.push_back(command);
                    }
                }
                if (!grass_history_probe.empty())
                {
                    const double pan = grass_history_probe == "C" || grass_history_probe == "D"
                        ? frame * 0.012 : 0;
                    const double distance = grass_history_probe == "E"
                        ? 3.0 + 0.5 * (frame <= frames / 2 ? frame : frames - frame) : 3.0;
                    main_view.camera.position = {pan, 1.6, -6.0 - distance};
                    main_view.camera.target = {pan, 0.45, -6};
                    main_view.view_origin = main_view.camera.position;
                    main_view.camera_cut = frame == 5;
                }
                if (light_list_probe)
                {
                    constexpr std::array counts{4u, 64u, 65u, 128u, 127u, 0u};
                    const auto local_count = counts[frame - 1];
                    const auto directional_count = frame >= 5 ? 1u : 0u;
                    light_list_lights.assign(local_count + directional_count, {});
                    for (std::uint32_t index = 0; index < light_list_lights.size(); ++index)
                    {
                        auto &light = light_list_lights[index];
                        light.stable_id = 0x4c4c000000000000ull + light_list_lights.size() - index;
                        light.type = index < directional_count ? hs::LightType::Directional
                            : static_cast<hs::LightType>(1u + index % 4u);
                        light.unit = light.type == hs::LightType::Directional ? hs::LightUnit::Lux
                            : index % 4u >= 2u ? hs::LightUnit::Luminance : hs::LightUnit::Lumen;
                        light.position = {0, 3, 0}; light.direction = {0, -1, 0};
                        light.color = {.7f, .8f, 1};
                        light.quantity = local_count ? 100.0f / local_count : 0.0f;
                        light.range = 10000; light.emitter_radius = .15f;
                        light.inner_cone_radians = .2f; light.outer_cone_radians = .6f;
                        light.shape = {2, 2}; light.area_tangent = {1, 0, 0};
                    }
                    // An explicit zero-intensity sun avoids Runtime's empty-table default sun.
                    pair.current.lights = light_list_lights;
                    main_view.camera_cut = true;
                }
                if (camera_basis_probe)
                {
                    pair.current.instances = {};
                    pair.previous.instances = {};
                    const auto camera_case = (frame - 1) % 3;
                    main_view.camera.position = camera_case == 2
                        ? hs::RenderWorldPosition{0, 6, 0} : hs::RenderWorldPosition{0, 2, -6};
                    main_view.camera.target = camera_case == 2
                        ? hs::RenderWorldPosition{0, 0, 0} : hs::RenderWorldPosition{0, 2, 0};
                    main_view.camera.up = camera_case == 0 ? hs::Float3{0, 1, 0}
                        : camera_case == 1 ? hs::Float3{1, 0, 0} : hs::Float3{0, 0, 1};
                    main_view.view_origin = main_view.camera.position;
                    main_view.camera_cut = true;
                }
                std::vector<hs::VfxFlashSpawnInput> oit_flashes;
                if (oit_overdraw_probe || camera_basis_probe)
                {
                    constexpr std::array layers{1u, 22u, 11u, 64u};
                    constexpr std::array hdr{1.0f, 1.0f, 2.0f, 5.5f};
                    hs::VfxFlashSpawnInput flash;
                    const auto &camera = main_view.camera;
                    const double dx = camera.target.x - camera.position.x;
                    const double dy = camera.target.y - camera.position.y;
                    const double dz = camera.target.z - camera.position.z;
                    const double distance = std::hypot(dx, dy, dz);
                    flash.position = {static_cast<float>(camera.position.x + 3.0 * dx / distance),
                        static_cast<float>(camera.position.y + 3.0 * dy / distance),
                        static_cast<float>(camera.position.z + 3.0 * dz / distance)};
                    flash.color = {1, 1, 1, 1}; flash.size = .8f; flash.lifetime = 1;
                    flash.event_tick = pair.current.header.tick; flash.stable_seed = 17;
                    flash.hdr = oit_overdraw_probe ? hdr[frame - 1] : 1.0f;
                    flash.normalized_age = .2f;
                    flash.shape = hs::VfxImpactShape::RadialEnergy;
                    flash.oit = true; flash.opacity_scale = 1000;
                    if (camera_basis_probe)
                    {
                        // Keep the full aspect3 silhouette inside the viewport.
                        flash.size = .5f;
                        flash.aspect = 3;
                        if (frame > 3)
                        {
                            flash.shape = hs::VfxImpactShape::Smoke6Way;
                            flash.smoke_frame_count = 64; flash.smoke_fps = 30;
                        }
                    }
                    oit_flashes.assign(oit_overdraw_probe ? layers[frame - 1] : 1u, flash);
                }
                if (!vfx_composition_probe.empty() && vfx_composition_probe != "control" && frame >= 9 && frame <= 24)
                {
                    hs::VfxFlashSpawnInput flash;
                    const auto &camera = main_view.camera;
                    const double dx = camera.target.x - camera.position.x;
                    const double dy = camera.target.y - camera.position.y;
                    const double dz = camera.target.z - camera.position.z;
                    const double distance = std::hypot(dx, dy, dz);
                    flash.position = {static_cast<float>(camera.position.x + 3.0 * dx / distance),
                        static_cast<float>(camera.position.y + 3.0 * dy / distance),
                        static_cast<float>(camera.position.z + 3.0 * dz / distance)};
                    if (frame >= 13 && frame <= 16) flash.position.x += .35f;
                    flash.color = {1, .7f, .3f, 1};
                    flash.size = frame >= 17 ? 3.0f : .6f;
                    flash.hdr = frame >= 17 ? 8.0f : .1f;
                    flash.opacity_scale = frame >= 17 ? 1.0f : .25f;
                    flash.lifetime = 1; flash.normalized_age = .2f;
                    flash.event_tick = pair.current.header.tick; flash.stable_seed = 17;
                    flash.shape = hs::VfxImpactShape::RadialEnergy; flash.oit = true;
                    oit_flashes.push_back(flash);
                }
                const hs::RenderFrameInput render_input{
                    .views = std::span(&main_view, 1), .snapshots = pair,
                    .vfx = {.flashes = oit_flashes, .entity_surfaces = entity_surfaces},
                    .delta_seconds = entity_surface_probe ? std::optional<float>{0.0f}
                        : local_probe_revision_probe ? std::optional<float>{1.0f / 60.0f}
                        : !vfx_composition_probe.empty()
                        ? std::optional<float>{vfx_composition_probe == "auto" || vfx_composition_probe == "switch"
                            ? 1.0f / 60.0f : 0.0f}
                        : persistent_scene_probe || material_reload_probe
                        ? std::optional<float>{0.0f} : std::nullopt,
                    .wait_for_streaming = entity_surface_probe || !vfx_composition_probe.empty()};
                if (provider_switch_probe)
                {
                    std::uint64_t input_token{};
                    Require(renderer.BeginInputFrame(input_token));
                    if (input_token)
                    {
                        Require(renderer.InputSampled(input_token));
                        Require(renderer.SimulationCompleted(input_token));
                    }
                }
                if (lighting_auxiliary_probe)
                    measure(std::format("auxiliary_render_{}", frame).c_str(), [&] {
                        Require(renderer.Render(render_input, frame_result));
                    });
                else Require(renderer.Render(render_input, frame_result));
                if (!frame_result.vfx_world_step_submitted ||
                    frame_result.vfx_world_tick != pair.current.header.tick ||
                    frame_result.vfx_world_session_id != pair.current.header.session_id)
                    throw std::runtime_error("Submitted world-step receipt does not match the rendered session/tick");
                if (hero_layer_probe)
                {
                    const auto status = renderer.GetGraphicsSettingsStatus();
                    const bool committed = status.actual_applied_available && status.error == hs::ErrorCode::None &&
                        (status.state == hs::SettingsApplyState::Applied || status.state == hs::SettingsApplyState::Confirmed);
                    const bool actual = committed && status.generation == hero_quality_generation && (frame <= 4
                        ? status.actual_applied.transparency_quality == hs::GraphicsQuality::Medium &&
                            status.actual_applied.refraction_quality == hs::GraphicsOptionalQuality::Medium
                        : status.actual_applied.transparency_quality == hs::GraphicsQuality::Ultra &&
                            status.actual_applied.refraction_quality == hs::GraphicsOptionalQuality::Ultra);
                    hero_quality_valid &= actual && frame_result.views.size() == 1;
                    Json views = Json::array();
                    for (const auto &view : frame_result.views)
                        views.push_back({{"submission_id", view.submission_id}, {"view_id", view.rendered_view.id.value},
                            {"camera_position", {view.rendered_view.camera.position.x, view.rendered_view.camera.position.y, view.rendered_view.camera.position.z}},
                            {"camera_target", {view.rendered_view.camera.target.x, view.rendered_view.camera.target.y, view.rendered_view.camera.target.z}}});
                    receipt["hero_work"]["quality_transition"]["frames"].push_back({{"fixture_frame", frame},
                        {"views", std::move(views)}, {"camera_yaw_degrees", pair.current.camera.yaw_degrees},
                        {"camera_pitch_degrees", pair.current.camera.pitch_degrees},
                        {"request_generation", hero_quality_generation}, {"status_generation", status.generation},
                        {"status_state", static_cast<unsigned>(status.state)}, {"status_error", static_cast<unsigned>(status.error)},
                        {"actual_applied_available", status.actual_applied_available}, {"expected_actual_applied", actual},
                        {"requested_transparency_quality", static_cast<unsigned>(status.requested.transparency_quality)},
                        {"requested_refraction_quality", static_cast<unsigned>(status.requested.refraction_quality)},
                        {"actual_transparency_quality", status.actual_applied_available ? Json(static_cast<unsigned>(status.actual_applied.transparency_quality)) : Json(nullptr)},
                        {"actual_refraction_quality", status.actual_applied_available ? Json(static_cast<unsigned>(status.actual_applied.refraction_quality)) : Json(nullptr)}});
                }
                if (hero_work_probe && gameplay_scene)
                {
                    const auto shadow = renderer.GetLightingStatus().shadow_cache;
                    if (frame_result.views.size() != 1) throw std::runtime_error("Hero water probe needs one submitted view");
                    const auto &view = frame_result.views.front();
                    receipt["hero_water_shadow"]["frames"].push_back({{"fixture_frame", frame},
                        {"submission_id", view.submission_id}, {"view_id", view.rendered_view.id.value},
                        {"light_direction", {world_lights[0].direction.x, world_lights[0].direction.y, world_lights[0].direction.z}},
                        {"cpu_plan_prepared", shadow.cpu_plan_prepared}, {"csm_required", shadow.csm_required},
                        {"guarantee_reason_bits", shadow.guarantee_reason_bits}, {"required_coarse_pages", shadow.required_coarse_pages},
                        {"maximum_coarse_commands", shadow.maximum_coarse_commands}, {"baked_active", shadow.baked_active},
                        {"coarse_command_total", shadow.coarse_command_total}, {"command_capacity", shadow.command_capacity},
                        {"baked_pages", shadow.baked_pages}, {"baked_staging_bytes", shadow.baked_staging_bytes}});
                    collect_timing_stream(false);
                }

                if (timing_stream_probe)
                {
                    collect_timing_stream(false);
                    const auto &cached = renderer.LastRenderedFrameDiagnostics();
                    if (frame_result.views.size() != 1 ||
                        cached.rendered_frame_id != frame_result.views.front().submission_id ||
                        cached.view_id != hs::kMainRenderViewId.value || !cached.submit_qpc ||
                        frame_result.cpu_timings.frame_slot_wait_begin_qpc == 0 ||
                        frame_result.cpu_timings.frame_slot_wait_end_qpc <
                            frame_result.cpu_timings.frame_slot_wait_begin_qpc)
                        throw std::runtime_error("Cached rendered frame/wait metadata has wrong identity");
                    if (frame == 4)
                    {
                        collect_timing_stream(true);
                        const auto before_duplicate_read = timing_stream_history.size();
                        static_cast<void>(renderer.LastGpuPassTimings());
                        collect_timing_stream(false);
                        if (timing_stream_history.size() != before_duplicate_read)
                            throw std::runtime_error("Repeated timing read republished a completed frame");
                    }
                }
                if (!timing_stream_probe && !(hero_work_probe && gameplay_scene))
                {
                    // Timing consumers above retain the latest sample; only the
                    // explicit stream probes need the consume-once history.
                    std::vector<hs::GpuPassTimings> unused_timings;
                    Require(renderer.ConsumeCompletedGpuPassTimings(unused_timings));
                }
                if (vfx_composition_probe == "switch")
                {
                    const auto status = renderer.GetGraphicsSettingsStatus();
                    const auto expected = frame >= 13 && frame < 21
                        ? hs::GraphicsOptionalQuality::Medium : hs::GraphicsOptionalQuality::Off;
                    const auto actual = status.actual_applied_available
                        ? hs::GraphicsEnumToken(status.actual_applied.volumetric_quality)
                        : std::string_view{"unavailable"};
                    if (frame == 1 || frame == 13 || frame == 21)
                        receipt["vfx_composition"]["settings"].push_back({{"frame", frame},
                            {"generation", status.generation}, {"state", static_cast<unsigned>(status.state)},
                            {"actual_applied_available", status.actual_applied_available},
                            {"requested_volumetric_quality", std::string(hs::GraphicsEnumToken(status.requested.volumetric_quality))},
                            {"active_volumetric_quality", std::string(actual)}});
                    if (!status.actual_applied_available ||
                        status.actual_applied.volumetric_quality != expected ||
                        status.requested.volumetric_quality != expected)
                        throw std::runtime_error(std::format(
                            "Volumetric switch mismatch at frame {}: generation={} state={} actual_available={} requested={} actual={} expected={}",
                            frame, status.generation, static_cast<unsigned>(status.state),
                            status.actual_applied_available,
                            hs::GraphicsEnumToken(status.requested.volumetric_quality), actual,
                            hs::GraphicsEnumToken(expected)));
                }
                if (lighting_auxiliary_probe)
                {
                    const auto capture = std::format("lighting-auxiliary-{}.json", frame);
                    measure(std::format("auxiliary_capture_{}", frame).c_str(), [&] {
                        Require(renderer.Capture({.path = artifacts / capture, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    });
                    std::ifstream input(artifacts / capture);
                    const auto diagnostic = Json::parse(input);
                    const auto &targets = diagnostic.at("auxiliary_targets");
                    if (targets.at(1).at("present").get<bool>() != (frame % 2 == 0) ||
                        targets.at(2).at("present").get<bool>() ||
                        (frame > 1 && targets.at(0).at("present") != receipt["lighting_auxiliaries"]
                            ["frames"].front().at("targets").at(0).at("present")) ||
                        (frame > 1 && renderer.GetGraphicsSettingsStatus().state != hs::SettingsApplyState::Applied &&
                         renderer.GetGraphicsSettingsStatus().state != hs::SettingsApplyState::Confirmed))
                        throw std::runtime_error("Auxiliary liveness did not follow applied reflection settings");
                    for (const auto &target : targets)
                        if (target.at("present").get<bool>() !=
                            (target.at("allocation_bytes").get<std::uint64_t>() != 0))
                            throw std::runtime_error("Auxiliary allocation disagrees with resource liveness");
                    receipt["lighting_auxiliaries"]["frames"].push_back({{"frame", frame},
                        {"targets", targets}, {"extent", diagnostic.at("extent")}});
                }
                if (light_list_probe)
                {
                    const auto capture = std::format("light-list-{}.json", frame);
                    Require(renderer.Capture({.path = artifacts / capture, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    receipt["light_list"]["frames"].push_back({{"frame", frame}, {"surface", capture}});
                }
                if (oit_overdraw_probe || camera_basis_probe)
                {
                    if (frame_result.typed_vfx_flash_count != oit_flashes.size())
                        throw std::runtime_error("OIT overdraw fixture did not admit every actual writer");
                    const auto capture = std::format("{}-{}.json",
                        oit_overdraw_probe ? "oit-overdraw" : "camera-basis", frame);
                    Require(renderer.Capture({.path = artifacts / capture, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    receipt[oit_overdraw_probe ? "oit_overdraw" : "camera_basis"]["frames"].push_back({{"frame", frame},
                        {"flashes", frame_result.typed_vfx_flash_count}, {"surface", capture}});
                }
                observe_probe_revisions();
                if (entity_surface_probe)
                {
                    const auto capture = std::format("entity-surface-{}.json", frame);
                    Require(renderer.Capture({.path = artifacts / capture, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    const auto geometry = std::format("entity-surface-geometry-{}.json", frame);
                    if (frame != 6) Require(renderer.Capture({.path = artifacts / geometry, .kind = hs::CaptureKind::GeometryDiagnostics}));
                    receipt["entity_surface"]["frames"].push_back({{"frame", frame}, {"surface", capture},
                        {"geometry", frame == 6 ? Json(nullptr) : Json(geometry)},
                        {"owner", frame >= 7 ? 2 : 1}, {"submitted_commands", entity_surfaces.size()},
                        {"source_tick", frame == 4 ? Json(80) : entity_surfaces.empty() ? Json(nullptr) : Json(entity_surfaces[0].current_tick)},
                        {"phase", entity_surfaces.empty() ? Json(nullptr) : Json(entity_surfaces[0].normalized_phase)}});
                }
                if (!vfx_composition_probe.empty() && (vfx_composition_probe != "control" || frame == 9) &&
                    (frame == 8 || frame == 9 || frame == 12 ||
                    frame == 13 || frame == 16 || frame == 17 || frame == 24 || frame == 25 || frame == 32))
                {
                    if (frame_result.typed_vfx_flash_count != oit_flashes.size())
                        throw std::runtime_error("VFX composition probe did not admit its actual writer");
                    const auto capture = std::format("vfx-composition-{:02}.json", frame);
                    Require(renderer.Capture({.path = artifacts / capture, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    receipt["vfx_composition"]["frames"].push_back({{"frame", frame},
                        {"flashes", frame_result.typed_vfx_flash_count}, {"surface", capture}});
                }
                observe_provider_timings();
                if (provider_switch_probe)
                {
                    const auto applied = renderer.GetGraphicsSettingsStatus();
                    const auto sr = renderer.GetReconstructionStatus();
                    const auto fg = renderer.GetPresentationStatus();
                    Json frame_receipt{{"frame", frame},
                        {"generation", applied.generation}, {"state", static_cast<unsigned>(applied.state)},
                        {"sr", sr.effective_provider.value}, {"fg", fg.effective_provider.value},
                        {"fg_mode", std::string(hs::GraphicsEnumToken(fg.active_mode))},
                        {"requested_fg_mode", std::string(hs::GraphicsEnumToken(applied.requested.frame_generation_mode))},
                        {"native_requested_fg_mode", std::string(hs::GraphicsEnumToken(fg.requested.mode))},
                        {"latency", fg.low_latency_active}, {"sr_reason", sr.fallback_reason},
                        {"fg_reason", fg.fallback_reason}};
                    const auto native_sr_pass_observed = [&](std::string_view provider) {
                        const auto pass_name = NativeReconstructionPassName(provider);
                        return !pass_name.empty() && std::ranges::any_of(provider_gpu_history,
                            [&](const auto &sample) {
                                return sample.second.valid && std::ranges::any_of(sample.second.passes,
                                    [&](const auto &pass) {
                                        return pass.pass_id == 9 && pass.name == pass_name && pass.measured;
                                    });
                            });
                    };
                    if (all_providers_probe)
                    {
                        const auto &requested_sr = applied.requested.reconstruction_provider.value;
                        const auto &requested_fg = applied.requested.frame_generation_provider.value;
                        const auto requested_fg_mode = applied.requested.frame_generation_mode;
                        const auto current_capabilities = renderer.GetGraphicsCapabilities();
                        const auto record_requested_capability = [&](std::string_view option) {
                            const auto found = std::ranges::find_if(current_capabilities.options,
                                [&](const auto &entry) { return entry.option.value == option; });
                            if (found == current_capabilities.options.end()) return;
                            frame_receipt["requested_capabilities"][std::string(option)] = {
                                {"available", found->available},
                                {"reason", static_cast<unsigned>(found->reason)},
                                {"reason_name", GraphicsCapabilityReasonName(found->reason)}};
                        };
                        record_requested_capability("frame_generation:fixed");
                        record_requested_capability("frame_generation:auto");
                        record_requested_capability("frame_generation_multiplier:2");
                        frame_receipt["sr_requested_provider"] = sr.requested_provider.value;
                        frame_receipt["sr_effective_provider"] = sr.effective_provider.value;
                        frame_receipt["sr_availability"] = sr.requested_availability;
                        frame_receipt["sr_reason"] = sr.fallback_reason;
                        frame_receipt["fg_requested_provider"] = fg.requested.provider.value;
                        frame_receipt["fg_requested_mode"] = std::string(hs::GraphicsEnumToken(fg.requested.mode));
                        frame_receipt["fg_effective_provider"] = fg.effective_provider.value;
                        frame_receipt["fg_effective_mode"] = std::string(hs::GraphicsEnumToken(fg.active_mode));
                        frame_receipt["fg_availability"] = fg.availability;
                        frame_receipt["fg_reason"] = fg.fallback_reason;
                        frame_receipt["fg_observed_multiplier"] = fg.observed_multiplier;
                        frame_receipt["observed_multiplier"] = fg.observed_multiplier;
                        frame_receipt["fg_native_callback_observed"] = fg.observed_multiplier != 0;
                        frame_receipt["fg_generated_frame_evidence"] = fg.observed_multiplier >= 2;
                        frame_receipt["sr_native_pass_observed"] = native_sr_pass_observed(sr.effective_provider.value);
                        auto &published_frames = receipt["provider_switch"]["frames"];
                        published_frames.push_back(std::move(frame_receipt));
                        auto &published_frame_receipt = published_frames.back();
                        if (!applied.actual_applied_available)
                            throw std::runtime_error("All-provider settings did not publish an actual applied status");
                        if (sr.requested_availability == "unavailable" || fg.availability == "unavailable")
                            throw std::runtime_error("All-provider request has unresolved unavailable SDK state");
                        const bool sr_rejected = sr.requested_availability == "unsupported" ||
                            sr.requested_availability == "compiled_out";
                        published_frame_receipt["sr_unexecuted"] = sr_rejected;
                        if (sr_rejected && (sr.effective_provider.value == requested_sr || sr.fallback_reason.empty()))
                            throw std::runtime_error("Rejected SR provider did not retain its requested identity and reason");
                        if (!sr_rejected && requested_sr != "internal_taau" &&
                            requested_sr != "auto" && sr.effective_provider.value != requested_sr)
                            throw std::runtime_error("Available SR provider did not activate natively");
                        const bool fg_rejected = fg.availability == "unsupported" || fg.availability == "compiled_out";
                        published_frame_receipt["fg_unexecuted"] = fg_rejected;
                        if (fg_rejected && (fg.effective_provider.value == requested_fg || fg.fallback_reason.empty()))
                            throw std::runtime_error("Rejected FG provider did not retain its requested identity and reason");
                        const bool fg_suppression = !fg_rejected && fg.availability == "available" &&
                            requested_fg_mode == hs::GraphicsFrameGenerationMode::Auto &&
                            fg.active_mode == hs::GraphicsFrameGenerationMode::Off;
                        if (!fg_rejected && requested_fg_mode == hs::GraphicsFrameGenerationMode::Off &&
                            (fg.effective_provider.value != "off" || fg.active_mode != hs::GraphicsFrameGenerationMode::Off))
                            throw std::runtime_error("FG Off request did not restore the native presentation owner");
                        if (!fg_rejected && requested_fg_mode != hs::GraphicsFrameGenerationMode::Off &&
                            !fg_suppression && (fg.effective_provider.value != requested_fg ||
                                fg.active_mode != requested_fg_mode))
                            throw std::runtime_error("Available FG provider did not activate its requested native mode");
                        if (fg_suppression && fg.fallback_reason !=
                            "Auto frame generation is Off because completed base render cadence is below the authored quality profile threshold.")
                            throw std::runtime_error("Auto FG suppression did not report its actual cadence resolution");
                        if (requested_fg == "dlss" && requested_fg_mode == hs::GraphicsFrameGenerationMode::Auto &&
                            fg_suppression)
                        {
                            auto &cadence = receipt["provider_switch"]["auto_cadence"];
                            if (!cadence["suppression_observed"].get<bool>())
                            {
                                cadence["suppression_observed"] = true;
                                cadence["first_suppression_frame"] = frame;
                            }
                        }
                        if (!fg_rejected && requested_fg == "fsr" &&
                            requested_fg_mode == hs::GraphicsFrameGenerationMode::Auto)
                        {
                            if (fg_suppression) ++fsr_fg_suppressed;
                            else
                            {
                                if (fg.observed_multiplier != 0) fsr_fg_callback_observed = true;
                                if (fg.observed_multiplier >= 2)
                                {
                                    fsr_fg_generated_evidence = true;
                                    ++fsr_fg_generated_evidence_count;
                                }
                                else ++fsr_fg_insufficient;
                            }
                        }
                    }
                    else if (!applied.actual_applied_available ||
                        (frame >= 2 && frame < switch_restore_frame && switch_sr && sr.effective_provider.value != "dlss") ||
                        (frame >= 4 && frame < 6 && switch_fg &&
                            (fg.effective_provider.value != "dlss" || fg.active_mode != hs::GraphicsFrameGenerationMode::Fixed)) ||
                        (frame >= switch_restore_frame && (sr.effective_provider.value != "internal_taau" ||
                            fg.active_mode != hs::GraphicsFrameGenerationMode::Off)))
                        throw std::runtime_error("Available native provider did not activate after startup with internal SR / FG Off, or restore failed");
                    if (!all_providers_probe && switch_auto_probe && switch_fg && frame >= 6 && frame < switch_restore_frame)
                    {
                        const bool suppressed = fg.active_mode == hs::GraphicsFrameGenerationMode::Off;
                        if (applied.requested.frame_generation_mode != hs::GraphicsFrameGenerationMode::Auto ||
                            applied.requested.frame_generation_provider.value != "dlss" ||
                            fg.requested.mode != hs::GraphicsFrameGenerationMode::Auto ||
                            fg.requested.provider.value != "dlss" ||
                            (!suppressed && (fg.active_mode != hs::GraphicsFrameGenerationMode::Auto ||
                                fg.effective_provider.value != "dlss")) ||
                            (suppressed && (fg.effective_provider.value != "off" ||
                                fg.fallback_reason != "Auto frame generation is Off because completed base render cadence is below the authored quality profile threshold.")))
                            throw std::runtime_error("Auto FG did not preserve its request or report its actual cadence resolution");
                        auto &cadence = receipt["provider_switch"]["auto_cadence"];
                        if (suppressed && !cadence["suppression_observed"].get<bool>())
                        {
                            cadence["suppression_observed"] = true;
                            cadence["first_suppression_frame"] = frame;
                        }
                    }
                    if (!all_providers_probe)
                        receipt["provider_switch"]["frames"].push_back(std::move(frame_receipt));
                }
                observe_world_timings();
                const auto queue_stats = renderer.LastQueueStats();
                queue_totals.direct_batches += queue_stats.direct_batches;
                queue_totals.compute_batches += queue_stats.compute_batches;
                queue_totals.copy_batches += queue_stats.copy_batches;
                queue_totals.cross_queue_waits += queue_stats.cross_queue_waits;
                queue_totals.record_jobs += queue_stats.record_jobs;
                queue_totals.alias_activation_count += queue_stats.alias_activation_count;
                queue_totals.transient_resource_count = std::max(queue_totals.transient_resource_count, queue_stats.transient_resource_count);
                queue_totals.transient_block_count = std::max(queue_totals.transient_block_count, queue_stats.transient_block_count);
                queue_totals.transient_bytes_without_aliasing = std::max(queue_totals.transient_bytes_without_aliasing, queue_stats.transient_bytes_without_aliasing);
                queue_totals.transient_backing_bytes = std::max(queue_totals.transient_backing_bytes, queue_stats.transient_backing_bytes);
                if (persistent_scene_probe && frame == 8)
                    Require(renderer.Capture({.path = artifacts / "hzb_retest.png"}));
                if (persistent_scene_probe && frame == 9)
                    Require(renderer.Capture({.path = artifacts / "hzb_reference.png"}));
                if (persistent_scene_probe && frame == 11 &&
                    config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
                {
                    failure_code = "HS_RENDER_HZB_CLUSTER_RETEST_FAILED";
                    Require(renderer.Capture({.path = artifacts / "hzb-cluster-retest.png"}));
                    const auto path = artifacts / "hzb-retest-commands.json";
                    Require(renderer.Capture({.path = path, .kind = hs::CaptureKind::VisibilityDiagnostics}));
                    const auto diagnostic = Json::parse(std::ifstream(path));
                    if (!diagnostic.at("execution_valid").get<bool>())
                        throw std::runtime_error("HZB Cluster Retest diagnostic is invalid");
                    std::uint64_t committed{}, published{}, live{}, vertices{};
                    for (const auto &catalog : diagnostic.at("cluster_commands"))
                    {
                        if (catalog.at("frame").get<std::uint64_t>() != frame ||
                            catalog.at("failure_flags").get<std::uint32_t>() != 0)
                            throw std::runtime_error("HZB Cluster Retest has stale or failed status");
                        committed += catalog.at("committed_records").get<std::uint64_t>();
                        for (const auto &view : catalog.at("views"))
                        {
                            if (view.at("nonzero_outside_published_prefix").get<std::uint64_t>() != 0)
                                throw std::runtime_error("HZB Cluster draw prefix omits live commands");
                            if (view.at("view").get<std::uint32_t>() != 4) continue;
                            const auto count = view.at("published_count").get<std::uint64_t>();
                            if (count < view.at("last_nonzero_exclusive").get<std::uint64_t>())
                                throw std::runtime_error("HZB Cluster Retest suffix excludes a live command");
                            published += count;
                            live += view.at("nonzero_count").get<std::uint64_t>();
                            vertices += view.at("draw_vertex_count_sum").get<std::uint64_t>();
                        }
                    }
                    if (!committed || !live || !vertices)
                        throw std::runtime_error("HZB reveal did not exercise live Cluster Retest draws");
                    receipt["hzb_cluster_retest"] = {
                        {"frame", frame}, {"diagnostic", path.filename().string()},
                        {"committed", committed}, {"published", published},
                        {"nonzero", live}, {"draw_vertex_count_sum", vertices},
                        {"nonzero_outside_published_prefix", 0},
                    };
                }
                if (surface_motion_resize_fixture && frame == 3)
                {
                    failure_code = "HS_RENDER_SURFACE_DIAGNOSTIC_FAILED";
                    Require(renderer.Capture({.path = artifacts / "surface-motion.json", .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    ValidateSurfaceDiagnostic(artifacts / "surface-motion.json", config.width,
                                              config.height, true, "motion", receipt);
                }
                if (!grass_history_probe.empty() && (frame == 1 || frame == 5 || frame == 6 || frame > frames - 6))
                {
                    const auto name = std::format("grass-{:03}.json", frame);
                    Require(renderer.Capture({.path = artifacts / name, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    const auto diagnostic = Json::parse(std::ifstream(artifacts / name));
                    const auto grass = diagnostic.at("grass");
                    const auto pixels = grass.at("pixels").get<std::uint64_t>();
                    const auto historical = grass.at("historical_pixels").get<std::uint64_t>();
                    if (pixels < 100 || ((frame == 1 || frame == 5 || frame == 13 || frame == 14) && historical != 0) ||
                        (frame == frames && grass_history_probe != "E" && historical < pixels / 2))
                        throw std::runtime_error("grass history reset/recovery regression");
                    if (frame == frames && grass_history_probe != "A" && grass_history_probe != "E" &&
                        grass.at("moving_pixels").get<std::uint64_t>() == 0)
                        throw std::runtime_error("moving grass has no motion vectors");
                    const auto previous_time = diagnostic.at("temporal_inputs").at("previous_grass_state").at(0).get<double>();
                    const double expected_time = grass_history_probe == "A" || grass_history_probe == "C"
                        ? 1.0 : 1.0 + double(frame - 1) / grass_cadence;
                    if (frame > 1 && std::abs(previous_time - expected_time) > 0.0001)
                        throw std::runtime_error("grass history used another view or a simulation tick instead of its previous render");
                    const auto &gpu = renderer.LastGpuPassTimings();
                    Json passes = Json::array();
                    for (const auto &pass : gpu.passes)
                        if (gpu.valid && pass.measured)
                            passes.push_back({{"name", pass.name}, {"nanoseconds", pass.nanoseconds}});
                    receipt["grass_history"]["frames"].push_back({{"frame", frame},
                        {"capture", name}, {"grass", grass},
                        {"gpu_frame", gpu.completed_frame_id}, {"gpu_passes", passes}});
                }
                if (!grass_history_probe.empty() && (frame == 3 || frame == 9))
                {
                    auto preview = main_view;
                    preview.id = hs::ViewId{0x47524153u};
                    preview.role = hs::RenderViewRole::Preview;
                    preview.output_extent = preview.render_capacity = preview.render_active = {640, 360};
                    preview.camera_cut = false;
                    preview.scene_origin.x = frame == 9 ? 0.25 : 0.0;
                    preview.camera.position.x = preview.camera.target.x = preview.scene_origin.x;
                    preview.view_origin = preview.camera.position;
                    auto preview_instances = current;
                    preview_instances.front().position = {20, 0, 0};
                    preview_instances.back().position = {0, 0, -5};
                    auto preview_pair = pair;
                    preview_pair.has_previous = false;
                    preview_pair.current.instances = preview_instances;
                    const hs::RenderFrameInput preview_input{
                        .views = std::span(&preview, 1), .snapshots = preview_pair,
                        .simulation_time = std::chrono::nanoseconds(10000000000ll + frame * 1000000000ll / 30)};
                    hs::RendererFrameResult preview_result;
                    Require(renderer.Render(preview_input, preview_result));
                    const auto path = artifacts / std::format("grass-preview-{:03}.json", frame);
                    Require(renderer.Capture({preview.id, path, hs::CaptureKind::SurfaceDiagnostics}));
                    const auto captured = Json::parse(std::ifstream(path));
                    const auto &state = captured.at("temporal_inputs").at("previous_grass_state");
                    if ((frame == 3 && state.at(2).get<double>() != 0.0) ||
                        (frame == 9 && (std::abs(state.at(0).get<double>() - 10.1) > 0.0001 ||
                                       state.at(2).get<double>() != 1.0)))
                        throw std::runtime_error("preview grass history lost its last successful render state");
                    if (frame == 9 && captured.at("grass").at("max_motion_pixels").get<double>() > 10.0)
                        throw std::runtime_error("grass origin shift produced false motion");
                    receipt["grass_history"]["preview"][std::to_string(frame)] = state;
                }
                if (gpu_lod_probe && (frame == 1 || frame == 3 || frame == 4 ||
                                      frame == 5 || frame == 10))
                {
                    failure_code = "HS_RENDER_GEOMETRY_DIAGNOSTIC_FAILED";
                    const auto name = frame == 1 ? "lod-initial.json" :
                        frame == 3 ? "lod-before-growth.json" :
                        frame == 4 ? "lod-after-growth.json" :
                        frame == 5 ? "lod-away.json" : "lod-return.json";
                    Require(renderer.Capture({.path = artifacts / name, .kind = hs::CaptureKind::GeometryDiagnostics}));
                    ValidateGeometryDiagnostic(artifacts / name, name, receipt, true);
                    if (frame == 4)
                        ValidateGpuLodGrowthSteady(artifacts / name, receipt);
                }
                if (material_reload_probe && frame >= 2 && frame <= 6)
                {
                    const auto name = frame == 2 ? "material-reload-before.png" :
                        frame == 3 ? "material-reload-rejected.png" :
                        frame == 4 ? "material-reload-applied.png" :
                        frame == 5 ? "material-reload-resized.png" :
                                      "material-reload-restored.png";
                    Require(renderer.Capture({.path = artifacts / name}));
                    receipt["material_reload"]["captures"][std::to_string(frame)] = name;
                }
                if (grid_floor_probe && capture_grid_frames &&
                    (frame == 1 || frame == 9 || frame == 17 || frame == 25))
                {
                    const auto name = std::format("grid-{0:02}.png", frame);
                    Require(renderer.Capture({.path = artifacts / name}));
                    receipt["grid_floor"]["captures"][std::to_string(frame)] = name;
                }
                if (surface_motion_resize_fixture && frame == 4)
                {
                    // The fixture changes only controls it actually owns. The
                    // remaining fields retain GraphicsSettings defaults rather
                    // than being fabricated from unrelated renderer state.
                    hs::GraphicsSettings graphics;
                    graphics.vsync = config.vsync;
                    graphics.bloom = config.bloom;
                    graphics.outline = config.outline;
                    graphics.resolution_mode = hs::GraphicsResolutionMode::Manual;
                    graphics.manual_scale_percent = 75.0f;
                    graphics.dynamic_resolution = config.dynamic_resolution;
                    graphics.target_render_fps = config.target_render_fps;
                    graphics.dynamic_min_percent = config.dynamic_min_percent;
                    graphics.dynamic_max_percent = config.dynamic_max_percent;
                    graphics.shadow_quality = config.shadow_quality;
                    graphics.shadow_distance_m = config.shadow_distance_m;
                    graphics.particle_quality = config.vfx_quality == hs::VfxQuality::Low
                        ? hs::GraphicsQuality::Low
                        : config.vfx_quality == hs::VfxQuality::Medium
                            ? hs::GraphicsQuality::Medium : hs::GraphicsQuality::High;
                    graphics.ao_mode = config.ao_mode;
                    graphics.ao_quality = config.ao_quality;
                    graphics.contact_shadows = config.contact_shadows;
                    graphics.reflection_mode = config.reflection_mode;
                    graphics.reflection_quality = config.reflection_quality;
                    graphics.gi_mode = config.gi_mode;
                    graphics.gi_quality = config.gi_quality;
                    graphics.shadow_mode = config.shadow_mode;
                    graphics.volumetric_quality = config.volumetric_quality;
                    graphics.reconstruction_provider = config.reconstruction_request
                        ? config.reconstruction_request->provider : hs::GraphicsProviderId{"auto"};
                    graphics.reconstruction_quality = config.reconstruction_request
                        ? config.reconstruction_request->quality : hs::GraphicsModeId{"quality"};
                    graphics.sharpening = config.sharpening;
                    graphics.hdr_mode = config.hdr_mode;
                    graphics.hdr_paper_white_nits = config.hdr_paper_white_nits;
                    graphics.hdr_peak_nits = config.hdr_peak_nits;
                    graphics.display_output = config.display_output;
                    const auto next_settings_generation =
                        renderer.GetGraphicsSettingsStatus().generation + 1;
                    Require(renderer.RequestGraphicsSettings(
                        hs::GraphicsSettingsRequest{next_settings_generation, hs::GraphicsSettingsRequestCause::User,
                                                    std::move(graphics)}));
                }
                if (surface_motion_resize_fixture && frame == 5)
                    Require(renderer.Resize(960, 540));
                if (surface_motion_resize_fixture && frame == 7)
                {
                    failure_code = "HS_RENDER_SURFACE_DIAGNOSTIC_FAILED";
                    Require(renderer.Capture({.path = artifacts / "surface-resized.json", .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    ValidateSurfaceDiagnostic(artifacts / "surface-resized.json", 720, 405,
                                              true, "resized", receipt);
                }
                previous = current;
            }
        });
        if (grid_floor_probe)
        {
            receipt["grid_floor"]["current_target"] = {
                pair.current.camera.target.x,
                pair.current.camera.target.y,
                pair.current.camera.target.z,
            };
        }
        if (timing_stream_probe)
        {
            collect_timing_stream(true);
            if (!timing_stream_live || timing_stream_drained < 2 || timing_stream_history.size() != frames)
                throw std::runtime_error("Timing stream did not cover live slot reuse and every drained tail frame");
            for (std::uint64_t frame = 1; frame <= frames; ++frame)
                if (!timing_stream_history.contains(frame))
                    throw std::runtime_error("Timing stream omitted an intermediate completed frame");
            if (!config.dynamic_resolution)
            {
                const auto &expected = timing_stream_history.begin()->second;
                std::uint64_t previous_ready{};
                for (const auto &entry : timing_stream_history)
                {
                    const auto &sample = entry.second;
                    // This fixture never resizes or switches providers/quality.
                    if (sample.metric_generation != expected.metric_generation || sample.resize_epoch != expected.resize_epoch ||
                        sample.render_active != expected.render_active || sample.view_id != expected.view_id ||
                        sample.output_ready_frequency != expected.output_ready_frequency ||
                        sample.output_ready_ticks <= previous_ready)
                        throw std::runtime_error("Timing stream changed its fixed epoch or regressed the Direct ready endpoint");
                    previous_ready = sample.output_ready_ticks;
                }
            }
            std::vector<hs::GpuPassTimings> empty;
            Require(renderer.ConsumeCompletedGpuPassTimings(empty));
            if (!empty.empty()) throw std::runtime_error("Timing stream consume repeated drained frames");
            receipt["timing_stream"] = {{"completed_frames", timing_stream_history.size()},
                {"live_frames", timing_stream_live}, {"drained_frames", timing_stream_drained},
                {"all_frames_covered", true}, {"slot_wrap_and_duplicate_read", true},
                {"mode", timing_stream_critical_probe ? "Critical" : "Full"},
                {"critical_endpoints_only", timing_stream_critical_probe},
                {"source_output_ready_contract", !config.dynamic_resolution},
                {"fixed_epoch_ready_monotonic", !config.dynamic_resolution}};
        }
        failure_code = "HS_RENDER_CAPTURE_FAILED";
        if (config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
        {
            failure_code = "HS_RENDER_VISIBILITY_DIAGNOSTICS_FAILED";
            const auto diagnostic_view = frame_result.views.empty() ? hs::kMainRenderViewId :
                frame_result.views.back().identity.view_id;
            measure("visibility_diagnostics", [&] {
                Require(renderer.Capture({.view_id = diagnostic_view, .path = artifacts / "visibility.json",
                    .kind = hs::CaptureKind::VisibilityDiagnostics}));
            });
            receipt["visibility"] = Json::parse(std::ifstream(artifacts / "visibility.json"));
            receipt["visibility"]["view_id"] = diagnostic_view.value;
            if (persistent_scene_probe && frames >= 14)
            {
                failure_code = "HS_RENDER_HZB_CLUSTER_RETEST_FAILED";
                for (const auto &catalog : receipt["visibility"].at("cluster_commands"))
                    for (const auto &view : catalog.at("views"))
                        if (view.at("view").get<std::uint32_t>() == 4 &&
                            (view.at("published_count").get<std::uint64_t>() != 0 ||
                             view.at("nonzero_count").get<std::uint64_t>() != 0 ||
                             view.at("nonzero_outside_published_prefix").get<std::uint64_t>() != 0))
                            throw std::runtime_error("Cluster Retest count survived the live frame's slot reuse");
                receipt["hzb_cluster_retest"]["empty_after_slot_reuse"] = true;
            }
        }
        measure("capture", [&] { Require(renderer.Capture({.path = artifacts / "capture.png"})); });
        observe_world_timings();
        observe_provider_timings();
        const auto gpu_timings = renderer.LastGpuPassTimings();
        if (all_providers_probe)
        {
            for (const auto provider : {"dlss", "xess", "fsr", "directsr"})
            {
                const auto pass_name = NativeReconstructionPassName(provider);
                Json completed_frames = Json::array();
                Json passes = Json::array();
                bool observed = false;
                for (const auto &[completed_frame_id, timings] : provider_gpu_history)
                    for (const auto &pass : timings.passes)
                        if (pass.pass_id == 9 && pass.name == pass_name && pass.measured)
                        {
                            observed = true;
                            completed_frames.push_back(completed_frame_id);
                            passes.push_back({{"pass_id", pass.pass_id}, {"name", pass.name},
                                {"nanoseconds", pass.nanoseconds}, {"measured", pass.measured},
                                {"completed_frame_id", completed_frame_id}});
                        }
                const bool capability_available = receipt["provider_switch"]["capability_map"]["sr_provider"]
                    [provider]["available"].get<bool>();
                receipt["provider_switch"]["native_sr_gpu_history"][provider] = {
                    {"pass_id", 9}, {"pass_name", std::string(pass_name)}, {"observed", observed},
                    {"completed_frames", std::move(completed_frames)}, {"passes", std::move(passes)}};
                if (capability_available && !observed)
                    throw std::runtime_error("Available native SR provider has no completed GPU reconstruction pass: " +
                                             std::string(provider));
            }
            const bool fsr_fg_available = receipt["provider_switch"]["capability_map"]["fg_provider"]["fsr"]
                ["available"].get<bool>();
            const std::string fsr_fg_unresolved_reason = fsr_fg_available
                ? (fsr_fg_suppressed != 0
                    ? "FSR FG Auto was cadence-suppressed; no successful native generated-frame dispatch was observed."
                    : "FSR FG was available but no successful native generated-frame dispatch count >= 2 was observed.")
                : "FSR FG capability was unsupported or compiled out; native coverage was unexecuted.";
            receipt["provider_switch"]["fsr_fg_coverage"] = {
                {"available", fsr_fg_available}, {"executed", fsr_fg_generated_evidence},
                {"native_callback_observed", fsr_fg_callback_observed},
                {"generated_evidence_frames", fsr_fg_generated_evidence_count},
                {"suppressed_frames", fsr_fg_suppressed},
                {"insufficient_observation_count", fsr_fg_insufficient},
                {"coverage_claimed", fsr_fg_generated_evidence},
                {"unresolved_reason", fsr_fg_generated_evidence ? "" : fsr_fg_unresolved_reason},
                {"metric_source", "RendererPresentationStatus.observed_multiplier (successful native generation dispatch count; numGeneratedFrames+1)"}};
        }
        const auto residency_stats = renderer.ResidencyStats();
        const auto representative_texture = renderer.GetTextureStreamingStatus(
            "render/texture/environment_terrain_gaussian.rpkg");
        if (residency_streaming_probe)
        {
            receipt["residency_streaming"] = {
                {"budget_valid", residency_stats.budget_valid},
                {"local_budget_bytes", residency_stats.local_budget_bytes},
                {"local_usage_bytes", residency_stats.local_usage_bytes},
                {"tracked_live_bytes", residency_stats.tracked_live_bytes},
                {"tracked_retiring_bytes", residency_stats.tracked_retiring_bytes},
                {"tracked_reclaimed_bytes", residency_stats.tracked_reclaimed_bytes},
                {"requests", residency_stats.streaming_requests},
                {"uploads", residency_stats.streaming_uploads},
                {"publications", residency_stats.streaming_publications},
                {"reclaims", residency_stats.streaming_reclaims},
                {"generation", residency_stats.streaming_generation},
                {"state", residency_stats.streaming_state},
                {"texture", {
                    {"logical_id", "render/texture/environment_terrain_gaussian.rpkg"},
                    {"registered", representative_texture.registered},
                    {"pending", representative_texture.pending},
                    {"publication_complete", representative_texture.publication_complete},
                    {"requests", representative_texture.requests},
                    {"uploads", representative_texture.uploads},
                    {"publications", representative_texture.publications},
                    {"reclaims", representative_texture.reclaims},
                    {"revision", representative_texture.revision},
                    {"actual_resident_mip", representative_texture.actual_resident_mip},
                    {"effective_desired_mip", representative_texture.effective_desired_mip},
                    {"target_mip", representative_texture.target_mip},
                }},
                {"geometry_requests", residency_stats.geometry_streaming_requests},
                {"geometry_uploads", residency_stats.geometry_streaming_uploads},
                {"geometry_publications", residency_stats.geometry_streaming_publications},
                {"geometry_reclaims", residency_stats.geometry_streaming_reclaims},
                {"geometry_generation", residency_stats.geometry_streaming_generation},
                {"geometry_state", residency_stats.geometry_streaming_state},
            };
        }
        receipt["diagnostics"] = DiagnosticsReceipt(renderer.GetDiagnostics());
        receipt["lighting_status"] = LightingStatusReceipt(renderer.GetLightingStatus());
        if (local_probe_revision_probe)
        {
            observe_probe_revisions();
            receipt["local_probe_revisions"] = {{"capture_frames", probe_capture_frames},
                {"exposure_delta_seconds", 1.0f / 60.0f},
                {"settled_frames_before_change", settled_probe_frames},
                {"authored_owner_legacy_particles", renderer.LastParticleCount()},
                {"shadow_history", {{"anonymous_static_casters", 2}, {"anonymous_dynamic_casters", 1},
                    {"static_removal_and_dynamic_transfer_frame", 65},
                    {"anonymous_dynamic_removal_frame", 80}, {"named_dynamic_removal_frame", 81}}}};
            if (settled_probe_frames == 0 || std::ranges::any_of(probe_capture_frames, [](auto count) { return count == 0; }))
                throw std::runtime_error("Local probes must settle, then recapture changed lights and a replacement static scene.");
            if (frame_result.static_scene_upload_count != 2)
                throw std::runtime_error("Shadow history fixture must upload exactly the initial and replacement static scenes.");
            if (renderer.LastParticleCount() != 0)
                throw std::runtime_error("Authored WaveTimer/pickup/bow owners leaked into legacy GPU particles.");
        }
        if (world_transport_probe)
        {
            failure_code = "HS_RENDER_WORLD_TRANSPORT_PASSES_FAILED";
            const auto observed = WorldTransportReceipt(world_gpu_history);
            receipt["world_transport"].update(observed);
            const auto hlod_stats = Json::parse(std::ifstream(artifacts / "hlod_stats.json"));
            receipt["world_transport"]["hlod_admission"] = hlod_stats;
            if (!hlod_stats.at("matching_scene").get<bool>() ||
                hlod_stats.at("fault_bits").get<std::uint32_t>() != 0u ||
                hlod_stats.at("admitted_pages").get<std::uint32_t>() >
                    hlod_stats.at("optional_capacity").get<std::uint32_t>() ||
                hlod_stats.at("missing_slots").get<std::uint32_t>() >
                    hlod_stats.at("admitted_pages").get<std::uint32_t>())
                throw std::runtime_error("HLOD shared-pool admission or complete-leaf fallback contract failed");
            if (!observed["required_passes_complete"].get<bool>())
                throw std::runtime_error(
                    "world transport did not execute every volumetric, native local-cube/filter and accepted-layout HLOD pass in completed GPU history");
        }
        if (software_gi_probe)
        {
            const auto status = renderer.GetLightingStatus();
            const auto seen = [&](std::uint32_t id, std::string_view name) {
                return std::ranges::any_of(world_gpu_history, [&](const auto &sample) {
                    return sample.second.valid && HasGpuPass(sample.second, id, name);
                });
            };
            const bool executed = seen(261, "BrixelizerSdfUpdate") && seen(262, "BrixelizerNativeGi") &&
                seen(263, "BrixelizerMaterialResolve") && seen(266, "BrixelizerCacheDirectDiffuseEmission");
            receipt["software_gi_probe"] = {{"native_passes_present", executed},
                {"active", status.active.gi_mode == hs::GraphicsGiMode::SoftwareHybrid}};
            if (status.software_gi.compiled && status.software_gi.supported &&
                (!status.software_gi.enabled || !status.software_gi.native_ready || status.software_gi.faulted ||
                 !status.software_gi.allocated_bytes ||
                 !(status.software_gi.static_instances + status.software_gi.dynamic_instances) || !executed))
                throw std::runtime_error("supported software GI did not register real SDF geometry and execute native cache/GI/material passes");
        }
        receipt["gpu_passes"] = Json::array();
        for (const auto &pass : gpu_timings.passes)
            if (gpu_timings.valid)
                receipt["gpu_passes"].push_back({{"pass_id", pass.pass_id},
                    {"name", pass.name}, {"nanoseconds", pass.nanoseconds},
                    {"queue", static_cast<unsigned>(pass.queue)}, {"measured", pass.measured}});
        receipt["queue_execution"] = {
            {"direct_batches", queue_totals.direct_batches},
            {"compute_batches", queue_totals.compute_batches},
            {"copy_batches", queue_totals.copy_batches},
            {"cross_queue_waits", queue_totals.cross_queue_waits},
            {"record_jobs", queue_totals.record_jobs}};
        receipt["transient_memory"] = {
            {"peak_resources", queue_totals.transient_resource_count},
            {"peak_blocks", queue_totals.transient_block_count},
            {"alias_activations", queue_totals.alias_activation_count},
            {"peak_unaliased_bytes", queue_totals.transient_bytes_without_aliasing},
            {"peak_backing_bytes", queue_totals.transient_backing_bytes}};
        if (require_transient_aliasing && (queue_totals.alias_activation_count == 0 ||
            queue_totals.transient_resource_count <= queue_totals.transient_block_count ||
            queue_totals.transient_bytes_without_aliasing <= queue_totals.transient_backing_bytes))
            throw std::runtime_error("Production fixture did not save transient backing memory through aliasing");
        if (require_multi_queue && (queue_totals.direct_batches == 0 ||
            queue_totals.compute_batches == 0 || queue_totals.copy_batches == 0 ||
            queue_totals.cross_queue_waits == 0 || queue_totals.record_jobs == 0))
            throw std::runtime_error("Production fixture did not execute three queues and worker recording");
        if (residency_streaming_probe)
        {
            failure_code = "HS_RENDER_RESIDENCY_STREAMING_FAILED";
            if (!residency_stats.budget_valid ||
                residency_stats.local_budget_bytes == 0 ||
                residency_stats.tracked_live_bytes == 0 ||
                residency_stats.tracked_reclaimed_bytes == 0)
                throw std::runtime_error("Residency budget accounting did not report a valid live/reclaimed transaction.");
            if (residency_stats.streaming_publications == 0 ||
                residency_stats.streaming_uploads < residency_stats.streaming_publications ||
                residency_stats.streaming_requests < residency_stats.streaming_uploads)
                throw std::runtime_error("Texture streaming transaction conservation failed: requests >= uploads >= publications is false.");
            if (!representative_texture.registered || representative_texture.publications == 0 ||
                representative_texture.pending || !representative_texture.publication_complete ||
                representative_texture.actual_resident_mip != representative_texture.effective_desired_mip ||
                representative_texture.actual_resident_mip != representative_texture.target_mip ||
                representative_texture.reclaims == 0)
                throw std::runtime_error("Texture streaming representative did not complete at its effective mip target and reclaim staging/backing.");
            // View demand selects the families; require completion of actual requests.
            if (residency_stats.geometry_streaming_requests == 0 ||
                residency_stats.geometry_streaming_uploads !=
                    residency_stats.geometry_streaming_requests ||
                residency_stats.geometry_streaming_publications !=
                    residency_stats.geometry_streaming_requests ||
                residency_stats.geometry_streaming_reclaims <
                    residency_stats.geometry_streaming_requests)
                throw std::runtime_error("Geometry streaming did not publish and reclaim every requested geometry family.");
        }
        if (hero_work_probe)
        {
            failure_code = "HS_RENDER_HERO_WORK_PROBE_FAILED";
            std::vector<hs::RendererHeroWorkSample> hero_samples;
            const auto drained = renderer.DrainHeroWork(hero_samples);
            bool shadow_valid = true;
            if (gameplay_scene)
            {
                collect_timing_stream(true);
                shadow_valid = ValidateHeroWaterShadowProbe(receipt["hero_water_shadow"], timing_stream_history);
            }
            ValidateHeroWorkProbe(hero_samples, renderer.HeroWorkStatus(), config.hero_work_mode, frames, drained, receipt, gameplay_scene,
                hero_layer_probe, hero_quality_valid);
            if (!shadow_valid) throw std::runtime_error("Hero water shadow policy/producer invariant failed; raw frame receipts retained");
        }
        receipt["pipeline_cache"] = PipelineCacheReceipt(renderer.GetPipelineCacheStats());
        failure_code = "HS_RENDER_SHUTDOWN_FAILED";
        measure("shutdown", [&] { Require(renderer.Shutdown()); });
        failure_code = "HS_RENDER_PERSISTENT_SCENE_FAILED";
        if (config.validation && persistent_scene_probe)
        {
            ValidatePersistentSceneProbe(
                DynamicUploadRows(artifacts / "dynamic_instance_upload.csv"), frames, receipt);
            const auto hzb_reference = DecodeHzbReferencePixels(artifacts / "hzb_reference.png");
            receipt["hzb_retest"] = {
                {"previous_history_active", true},
                {"matching_reference_capture", nullptr},
                {"reference_width", hzb_reference.width},
                {"reference_height", hzb_reference.height},
                {"reference_nonzero_rgb_pixels", hzb_reference.nonzero_rgb_pixels},
                {"reference_pixel_variation", hzb_reference.pixel_variation},
                {"retest_frame", 8},
                {"reference_frame", 9},
            };
            if (!hzb_reference.nonzero_rgb_pixels)
                throw std::runtime_error("HZB reference capture is entirely black");
            if (!hzb_reference.pixel_variation)
                throw std::runtime_error("HZB reference capture has no RGB variation");
            const bool matching_hzb =
                FilesEqual(artifacts / "hzb_retest.png", artifacts / "hzb_reference.png");
            receipt["hzb_retest"]["matching_reference_capture"] = matching_hzb;
            if (!matching_hzb)
                throw std::runtime_error(
                    "current-depth visibility retest does not match the next stable frame");
            if (config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
            {
                const bool matching_cluster = FilesEqual(artifacts / "hzb-cluster-retest.png",
                                                         artifacts / "hzb_reference.png");
                receipt["hzb_cluster_retest"]["matching_reference_capture"] = matching_cluster;
                if (!matching_cluster)
                    throw std::runtime_error("live Cluster Retest does not match the stable HZB reference");
            }
        }
        if (config.validation && skin_cache_monster_probe)
            ValidateMonsterSkinCacheProbe(
                DynamicUploadRows(artifacts / "dynamic_instance_upload.csv"), frames, receipt);
        if (config.validation && dynamic_lod_skin_growth_probe)
            ValidateDynamicLodSkinGrowthProbe(
                DynamicUploadRows(artifacts / "dynamic_instance_upload.csv"), frames, receipt);
        receipt["validation_errors"] = renderer.ValidationErrorCount();
        receipt["validation_status"] = ValidationStatusReceipt(renderer.ValidationStatus());
        receipt["frames"] = frames;
        if (static_shadow_bake_probe) receipt["static_shadow_bake"]["subsequent_render_frames"] = frames;
        receipt["static_scene_upload_count"] = frame_result.static_scene_upload_count;
        receipt["enhanced_barriers"] = renderer.UsesEnhancedBarriers();
        receipt["timings_ms"] = std::move(timings);
        receipt["execution_valid"] = renderer.ValidationErrorCount() == 0;
        receipt["error_code"] = renderer.ValidationErrorCount() == 0
            ? "" : "HS_RENDER_VALIDATION_FAILED";
        exit_code = renderer.ValidationErrorCount() == 0 ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        receipt["error_code"] = failure_code;
        receipt["error"] = error.what();
    }
    return write_receipt();
}
