#include <hs/core/shipped_content_owner.hpp>
#include <hs/render_data/render_snapshot_storage.hpp>
#include <hs/core/process_info.hpp>
#include <hs/core/sha256.hpp>
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/graphics_settings_request.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/presentation/meadow_surface.hpp>
#include <hs/render_data/scene_probe.hpp>
#include <hs/render_data/surface_layout.hpp>
#include <hs/render_data/geometry_package.hpp>
#include <hs/render_data/character_package.hpp>
#include <hs/render_data/material_ir.hpp>
#include <hs/render_data/quality_profiles.hpp>
#include <hs/render_data/texture_descriptor.hpp>
#include <hs/render_data/material_descriptor.hpp>
#include <hs/render_data/material_schema.hpp>
#include <hs/render_data/character_descriptor.hpp>
#include <hs/render_data/render_revision.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/render_data/render_package.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include "view_fixture.hpp"
#include "static_shadow_bake_probe.hpp"
#include "../Common/world_transport_scene.hpp"
#include "../Common/renderer_geometry_catalog.hpp"
#if defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
#include "../Content/geometry_cooker.hpp"
#endif
#include <hs/jobs/task_system.hpp>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <d3d12.h>
#include <DirectXPackedVector.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
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
constexpr std::array kLightListLocalCounts{4u, 64u, 65u, 128u, 127u, 0u, 4u};
constexpr std::array kLightListDirectionalCounts{0u, 0u, 0u, 0u, 1u, 0u, 0u};
void Require(const hs::Result &result);

#if defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
std::vector<std::byte> ReadFixtureProduct(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > std::streamoff(256 * 1024 * 1024))
        throw std::runtime_error("Invalid selected product extent for the translucency geometry fixture");
    std::vector<std::byte> bytes(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Could not read selected product for the translucency geometry fixture");
    return bytes;
}

std::string FixtureDigest(std::span<const std::byte> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    const auto hash = hs::Sha256(bytes);
    std::string result = "sha256:";
    result.reserve(71);
    for (const auto byte : hash)
    {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

struct OpticalPairFixtureProducts
{
    std::uint16_t outer_component{}, inner_component{};
    std::uint32_t material_slot{};
    std::string source_generation;
    std::vector<std::string> output_digests;
};

hs::render_data::GeometryData MakeOpticalOctahedronPair(
    std::uint32_t material_count, std::uint32_t material_slot, bool nested,
    std::vector<hs::render_data::ExpandedVertex> &expanded)
{
    using Vertex = hs::render_data::ExpandedVertex;
    using Profile = hs::geometry_content::OpticalMaterialProfile;
    if (material_count == 0 || material_count > 65535 || material_slot >= material_count)
        throw std::runtime_error("Selected character has no valid solid-transmission material slot");

    std::vector<Profile> profiles(material_count);
    profiles[material_slot] = {1, true};
    constexpr std::array<std::array<float, 3>, 6> axes{{
        {{0,0,1}}, {{0,0,-1}}, {{1,0,0}}, {{0,1,0}}, {{-1,0,0}}, {{0,-1,0}}}};
    constexpr std::array<std::array<std::uint8_t, 3>, 8> faces{{
        {{0,2,3}}, {{0,3,4}}, {{0,4,5}}, {{0,5,2}},
        {{1,3,2}}, {{1,4,3}}, {{1,5,4}}, {{1,2,5}}}};
    const auto append_shell = [&](float radius) {
        for (auto face : faces)
        {
            std::array<float, 3> a{}, b{}, c{};
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                a[axis] = axes[face[0]][axis] * radius;
                b[axis] = axes[face[1]][axis] * radius;
                c[axis] = axes[face[2]][axis] * radius;
            }
            const std::array ab{b[0]-a[0], b[1]-a[1], b[2]-a[2]};
            const std::array ac{c[0]-a[0], c[1]-a[1], c[2]-a[2]};
            const std::array normal{ab[1]*ac[2]-ab[2]*ac[1], ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]};
            if (normal[0]*(a[0]+b[0]+c[0]) + normal[1]*(a[1]+b[1]+c[1]) +
                    normal[2]*(a[2]+b[2]+c[2]) < 0.0f)
                std::swap(face[1], face[2]);
            for (const auto index : face)
            {
                Vertex vertex{};
                vertex.position = {axes[index][0]*radius, axes[index][1]*radius, axes[index][2]*radius};
                const float inverse_length = 1.0f / std::sqrt(vertex.position[0]*vertex.position[0] +
                    vertex.position[1]*vertex.position[1] + vertex.position[2]*vertex.position[2]);
                vertex.normal = {vertex.position[0]*inverse_length, vertex.position[1]*inverse_length,
                    vertex.position[2]*inverse_length};
                vertex.bone_indices = {0,0,0,0};
                vertex.bone_weights = {1,0,0,0};
                vertex.tangent = {1,0,0,1};
                vertex.material_index = static_cast<std::uint16_t>(material_slot);
                expanded.push_back(vertex);
            }
        }
    };
    append_shell(0.42f);
    if (nested) append_shell(0.19f);
    auto geometry = hs::geometry_content::CookGeometry(expanded,
        hs::render_data::kGeometryFlagSkinned, profiles);
    std::set<std::uint16_t> ids;
    for (const auto &vertex : geometry.vertices)
    {
        if (vertex.optical_component_id == 0)
            throw std::runtime_error("Cooked translucency fixture contains an uncertified shell");
        ids.insert(vertex.optical_component_id);
    }
    if (ids.size() != (nested ? 2u : 1u))
        throw std::runtime_error("Cooked translucency fixture component count differs from the authored shells");
    if (nested)
    {
        const auto contains_radius = [&](float radius) {
            return std::ranges::any_of(geometry.vertices, [radius](const auto &vertex) {
                const float length = std::sqrt(vertex.position[0]*vertex.position[0] +
                    vertex.position[1]*vertex.position[1] + vertex.position[2]*vertex.position[2]);
                return std::abs(length-radius) < 1.0e-5f;
            });
        };
        if (!contains_radius(0.42f) || !contains_radius(0.19f))
            throw std::runtime_error("Cooked translucency fixture lost a shell radius");
    }
    return geometry;
}

OpticalPairFixtureProducts BuildOpticalPairFixture(hs::SelectedGeneration &generation,
    const std::filesystem::path &artifacts)
{
    using namespace hs::render_data;
    OpticalPairFixtureProducts result;
    result.source_generation = generation.id;
    const auto write_product = [&](std::string_view id, std::span<const std::byte> bytes) {
        const auto path = artifacts / (std::string(id.substr(id.rfind('/') + 1)) + ".optical-fixture");
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) throw std::runtime_error("Could not write selected-generation translucency fixture product");
        const auto digest = FixtureDigest(bytes);
        generation.product_paths[std::string(id)] = path;
        generation.product_digests[std::string(id)] = digest;
        result.output_digests.push_back(digest);
    };
    for (const auto &[stem, nested] : {std::pair<std::string_view, bool>{"enemy_melee", true}, {"enemy_ranged", false}})
    {
        const std::string metadata_id = "render/character/" + std::string(stem) + ".rpkg";
        const std::string page_id = "render/geometry/character/" + std::string(stem) + ".rpage";
        std::filesystem::path metadata_path;
        Require(generation.Require(metadata_id, metadata_path));
        const auto source_bytes = ReadFixtureProduct(metadata_path);
        if (generation.product_digests.at(metadata_id) != FixtureDigest(source_bytes))
            throw std::runtime_error("Selected character metadata digest mismatch before translucency override");
        const auto package = ReadPackage(source_bytes, hs::MakeAssetId(metadata_id));
        const auto source = DecodeCharacterMetadataPackage(package, false);
        if (!source.monster || source.monster->lods.empty() ||
            source.geometry.page_dependency_index >= package.dependencies.size() ||
            package.dependencies[source.geometry.page_dependency_index] != page_id)
            throw std::runtime_error("Selected slime metadata does not point to the expected LOD0 page");

        std::uint32_t material_slot = UINT32_MAX;
        for (const auto &binding : source.material_bindings)
        {
            if (binding.domain != CharacterMaterialDomain::Translucent) continue;
            const auto material_name = kCharacterMaterialNames[binding.hero_ordinal];
            const auto material_id = "render/material/" + std::string(material_name) + "/material.rpkg";
            std::filesystem::path material_path;
            Require(generation.Require(material_id, material_path));
            const auto material_bytes = ReadFixtureProduct(material_path);
            if (generation.product_digests.at(material_id) != FixtureDigest(material_bytes))
                throw std::runtime_error("Selected material digest mismatch during translucency fixture admission");
            const auto material = ReadCharacterMaterialPackage(material_bytes, material_name);
            if (material.domain == CharacterMaterialDomain::Translucent &&
                (material.surface.capability_mask & static_cast<std::uint32_t>(MaterialClosureCapability::SolidTransmission)) != 0)
            {
                material_slot = binding.slot;
                break;
            }
        }
        if (material_slot == UINT32_MAX)
            throw std::runtime_error("Selected slime material binding has no admitted solid-transmission domain");
        std::vector<ExpandedVertex> expanded;
        expanded.reserve(nested ? 48u : 24u);
        auto geometry = MakeOpticalOctahedronPair(source.material_count, material_slot, nested, expanded);
        auto metadata = source.geometry;
        metadata.abi = kGeometryAbi;
        metadata.flags = geometry.flags;
        metadata.vertex_count = static_cast<std::uint32_t>(geometry.vertices.size());
        metadata.index_count = static_cast<std::uint32_t>(geometry.indices.size());
        metadata.submesh_count = static_cast<std::uint32_t>(geometry.submeshes.size());
        metadata.meshlet_count = static_cast<std::uint32_t>(geometry.meshlets.size());
        metadata.vertex_ref_count = static_cast<std::uint32_t>(geometry.meshlet_vertex_refs.size());
        metadata.max_material_index = material_slot;
        metadata.bounds = geometry.bounds;
        auto monster = *source.monster;
        monster.lods.front() = metadata;
        // This fixture supplies exactly one synthetic geometry page. Keep its
        // monster directory and per-frame intervals limited to that page;
        // retaining authored lower-detail pages would violate the required
        // strictly decreasing vertex/index counts after replacing LOD0.
        monster.lods.resize(1);
        std::erase_if(monster.interval_bounds, [](const auto &bound) { return bound.lod != 0; });

        CharacterPackageData character;
        character.vertices.reserve(expanded.size());
        for (const auto &vertex : expanded)
            character.vertices.push_back({vertex.position, vertex.normal, vertex.bone_indices,
                vertex.bone_weights, vertex.uv, vertex.tangent, vertex.material_index, 0});
        character.clips = source.clips;
        character.parents = source.parents;
        character.inverse_bind_matrices = source.inverse_bind_matrices;
        character.upper_body_weights = source.upper_body_weights;
        // Production metadata stores its pose count with the monster final
        // palette and intentionally has no local-transform chunk. The builder
        // still validates its source shape against that count before omitting
        // local transforms from the cooked monster package.
        character.transforms.resize(source.monster->final_matrices.size());
        character.bone_count = source.bone_count;
        character.material_count = source.material_count;
        character.ground_offset = 0.0f;
        const auto rebuilt_metadata = BuildCharacterMetadataPackage(hs::MakeAssetId(metadata_id),
            package.dependencies, character, metadata, source.material_bindings, &monster);
        const auto rebuilt_page = BuildGeometryPage(hs::MakeAssetId(page_id), {}, geometry);
        const auto rebuilt_character = DecodeCharacterMetadataPackage(rebuilt_metadata, hs::MakeAssetId(metadata_id), false);
        const auto verified_page = PrepareGeometryPage(rebuilt_page, hs::MakeAssetId(page_id), rebuilt_character.geometry);
        ValidateCharacterGeometry(rebuilt_character, verified_page);
        write_product(metadata_id, rebuilt_metadata);
        write_product(page_id, rebuilt_page);
        if (stem == "enemy_melee")
        {
            result.material_slot = material_slot;
            for (const auto &vertex : geometry.vertices)
            {
                const float length = std::sqrt(vertex.position[0]*vertex.position[0] +
                    vertex.position[1]*vertex.position[1] + vertex.position[2]*vertex.position[2]);
                if (std::abs(length - 0.42f) < 1.0e-5f) result.outer_component = vertex.optical_component_id;
                if (std::abs(length - 0.19f) < 1.0e-5f) result.inner_component = vertex.optical_component_id;
            }
            if (!result.outer_component || !result.inner_component || result.outer_component == result.inner_component)
                throw std::runtime_error("Certified nested shell IDs do not identify distinct outer and inner components");
        }
    }
    std::string identity = generation.id;
    for (const auto &digest : result.output_digests) identity += digest;
    generation.id = FixtureDigest(std::as_bytes(std::span(identity.data(), identity.size()))).substr(7);
    return result;
}
#endif

double StageElapsedMilliseconds(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void LogStageElapsed(std::string_view name, double elapsed_ms)
{
    std::clog << "hs.render.stage name=" << name << " elapsed_ms=" << elapsed_ms << '\n' << std::flush;
}

void LogStageBegin(std::string_view name)
{
    std::clog << "hs.render.stage name=" << name << " phase=start\n" << std::flush;
}

struct StageBoundaryMarker
{
    bool enabled{};
    bool begin{};
    std::string_view name;
    Clock::time_point &start;

    ~StageBoundaryMarker() noexcept
    {
        if (!enabled) return;
        try
        {
            if (begin)
            {
                start = Clock::now();
                LogStageBegin(name);
            }
            else if (start != Clock::time_point{})
                LogStageElapsed(name, StageElapsedMilliseconds(start));
        }
        catch (...) {}
    }
};

const char *TranslucencyDiagnosticsModeName(hs::TranslucencyDiagnosticsMode mode)
{
    switch (mode)
    {
    case hs::TranslucencyDiagnosticsMode::Full: return "full";
    case hs::TranslucencyDiagnosticsMode::NoEnvironment: return "no-environment";
    case hs::TranslucencyDiagnosticsMode::CoverageOnly: return "coverage-only";
    }
    return "invalid";
}

void ValidateTranslucencyDiagnosticsProbe(const std::vector<hs::RendererTranslucencyDiagnosticsSample> &samples,
    const hs::RendererTranslucencyDiagnosticsStatus &status, hs::TranslucencyDiagnosticsMode mode,
    std::uint32_t frames, const hs::Result &drained, Json &receipt, bool require_water = false,
    bool require_layer_transition = false, bool quality_transition_valid = true)
{
    auto &probe = receipt["translucency_diagnostics"];
    probe["mode"] = TranslucencyDiagnosticsModeName(mode);
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
    probe["counter_names"] = hs::kTranslucencyDiagnosticsShaderCounterNames;
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
    check(status.submitted_frames == frames && status.completed_frames == frames &&
        status.samples == samples.size() && !samples.empty(),
        "completed frame/sample conservation or empty fixture");
    check(status.readback_bytes + status.shader_gpu_bytes + status.shader_readback_bytes + status.shader_zero_upload_bytes <=
        status.shader_storage_limit_bytes, "known diagnostic allocation limit");
    const auto hex = [](const std::array<std::byte, 32> &bytes) {
        constexpr char digits[] = "0123456789abcdef";
        std::string value; value.reserve(64);
        for (const auto b : bytes) { const auto n = std::to_integer<unsigned>(b); value += digits[n >> 4]; value += digits[n & 15]; }
        return value;
    };
    std::array<std::uint64_t, hs::kTranslucencyDiagnosticsShaderCounterNames.size()> totals{};
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
        const auto pipeline_key = sample.phase == 1
            ? p.pixel_product + "#rtv=" + std::to_string(p.rtv_formats[0]) : p.pixel_product;
        auto &stored = probe["pipelines"][pipeline_key];
        if (!stored.is_null()) check(stored == pipeline, "pipeline metadata changed within fixed probe");
        else stored = std::move(pipeline);
        Json counters = Json::object();
        for (std::size_t index = 0; index < totals.size(); ++index)
        { counters[hs::kTranslucencyDiagnosticsShaderCounterNames[index]] = sample.shader_counters[index]; totals[index] += sample.shader_counters[index]; }
        Json statistics = Json::array();
        for (std::size_t index = 0; index < sample.pipeline_statistics.size(); ++index)
            if (sample.statistics_valid && (index < 11 || sample.mesh_statistics_available)) statistics.push_back(sample.pipeline_statistics[index]);
            else statistics.push_back(nullptr);
        probe["samples"].push_back({{"completed_frame", sample.completed_frame}, {"tick", sample.tick}, {"view_id", sample.view_id},
            {"phase", sample.phase}, {"layer", sample.layer}, {"contributor", sample.contributor}, {"width", sample.width}, {"height", sample.height},
            {"diagnostic_mode", static_cast<unsigned>(sample.diagnostic_mode)}, {"pixel_product", p.pixel_product},
            {"pipeline_key", pipeline_key},
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
        check(p.pixel_product.ends_with("_translucency_diagnostics.dxil"), "instrumented shader product identity");
        check(sample.phase != 3, "retired standalone Guide draw was submitted");
        if (sample.phase == 0)
            check(p.pixel_product == "render/shader/layered_translucency_peel_front_ps_translucency_diagnostics.dxil" &&
                p.rtv_count == 2 && p.rtv_formats[0] == DXGI_FORMAT_R32G32_UINT &&
                p.rtv_formats[1] == DXGI_FORMAT_R32_UINT,
                "front peel must write instance identity and optical component ID");
        if (require_layer_transition && (sample.phase <= 1 || sample.phase == 4))
            layer_rows[{sample.completed_frame, sample.view_id, sample.contributor}][sample.phase == 4 ? 2 : sample.phase].insert(sample.layer);
        if (sample.phase == 1)
        {
            check(sample.layer == 0 && !sample.layer_rejections_available && !p.depth_enabled &&
                p.rtv_count == 1 && (p.rtv_formats[0] == DXGI_FORMAT_R32_FLOAT ||
                    p.rtv_formats[0] == DXGI_FORMAT_R32G32_FLOAT ||
                    p.rtv_formats[0] == DXGI_FORMAT_R32G32B32A32_FLOAT) &&
                p.blend_enabled[0] && p.blend_op[0] == D3D12_BLEND_OP_MAX,
                "single exit must report a full precision MAX reduction and unavailable per-layer rejections");
            if (require_layer_transition)
                check(sample.completed_frame <= 4
                    ? (p.rtv_formats[0] == DXGI_FORMAT_R32_FLOAT || p.rtv_formats[0] == DXGI_FORMAT_R32G32B32A32_FLOAT)
                    : p.rtv_formats[0] == DXGI_FORMAT_R32G32B32A32_FLOAT,
                    "exit format must fit the actual Medium/Ultra layer count");
        }
        if (sample.phase == 4)
        {
            check(p.depth_enabled && p.depth_func == D3D12_COMPARISON_FUNC_EQUAL &&
                p.depth_write_mask == D3D12_DEPTH_WRITE_MASK_ZERO && p.dsv_format == DXGI_FORMAT_D32_FLOAT &&
                p.cull_mode == D3D12_CULL_MODE_BACK && p.sample_count == 1,
                "composite must retain the selected-front raster/depth contract");
            if (sample.layer == 0)
            {
                ++fused_rows; fused_accepted += c[15];
                constexpr std::array<std::uint32_t, 5> formats{DXGI_FORMAT_R16G16B16A16_FLOAT,
                    DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
                    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
                check(p.pixel_product == "render/shader/layered_translucency_composite_front_ps_translucency_diagnostics.dxil" &&
                    p.rtv_count == formats.size() && p.independent_blend,
                    "nearest composite must report the actual fused guide shader and five-target PSO");
                for (std::size_t target = 0; target < formats.size(); ++target)
                    check(p.rtv_formats[target] == formats[target] && p.blend_enabled[target] == 0 &&
                        p.write_mask[target] == D3D12_COLOR_WRITE_ENABLE_ALL,
                        "fused HDR/guide target format, replace blend or write mask changed");
                check(c[18] <= c[0] && std::uint64_t(c[13]) + c[14] <= c[18] &&
                    c[15] <= std::uint64_t(c[13]) + c[14],
                    "nearest surface must be evaluated at most once per shader entry");
            }
            else check(p.pixel_product == "render/shader/layered_translucency_composite_layer_ps_translucency_diagnostics.dxil" && p.rtv_count == 1,
                "farther composites must retain the original single-target product");
            if (sample.layer > 0 && sample.completed_frame < farther_accepted.size())
                farther_accepted[sample.completed_frame] += c[15];
        }
        if (sample.phase == 2 || sample.phase == 4) shaded_accepted += c[15];
        if (sample.source.label == "water" || sample.source.label == "dynamic_water")
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
            check(frame >= 1 && frame <= frames && phases[0] == expected && phases[1] == std::set<std::uint32_t>{0} && phases[2] == expected,
                "front/composite layer directory or single exit directory differs from the actual Medium/Ultra fixture contract");
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
    if (mode == hs::TranslucencyDiagnosticsMode::Full)
        check(totals[26] == 0 && totals[27] == 0 && totals[19] > 0 && totals[25] > 0 && totals[20] + totals[24] + totals[28] == totals[19],
            "Full environment path or unexpected diagnostic bypass");
    else if (mode == hs::TranslucencyDiagnosticsMode::NoEnvironment)
        check(totals[19] > 0 && totals[25] > 0 && totals[26] == totals[19] && totals[20] == 0 && totals[21] == 0 &&
            totals[22] == 0 && totals[23] == 0 && totals[24] == 0 && totals[27] == 0 && totals[28] == 0, "NoEnvironment bypass relation");
    else
        check(totals[27] == shaded_accepted && totals[27] > 0 && totals[19] == 0 && totals[20] == 0 && totals[21] == 0 &&
            totals[22] == 0 && totals[23] == 0 && totals[24] == 0 && totals[25] == 0 && totals[26] == 0 && totals[28] == 0, "CoverageOnly bypass relation");
    probe["counter_totals"] = Json::object();
    for (std::size_t index = 0; index < totals.size(); ++index) probe["counter_totals"][hs::kTranslucencyDiagnosticsShaderCounterNames[index]] = totals[index];
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
            mode == hs::TranslucencyDiagnosticsMode::Full ? "fixture did not cover this authored branch" : "deliberately bypassed by diagnostic mode"},
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
            {"reflection", status.reflection_fallback_reason},
            {"shadow", status.shadow_fallback_reason},
        }},
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

Json WorldTransportReceipt(const std::map<std::uint64_t, hs::GpuPassTimings> &history,
                           bool sample_ready)
{
    constexpr std::array<std::pair<std::uint32_t, std::string_view>, 16> expected{{
        {170, "VolumetricDensity"}, {171, "VolumetricLighting"},
        {172, "VolumetricTemporal"}, {173, "VolumetricSpatial"},
        {174, "VolumetricIntegrate"}, {175, "VolumetricResolve"},
        {70, "SSRTrace"}, {71, "SSRTemporal"},
        {186, "LocalProbePackageUpload"},
        {190, "HlodUpload"}, {191, "HlodClear"}, {192, "HlodSelect"},
        {193, "HlodEmit"}, {194, "HlodPublish"}, {195, "HlodHistory"},
        {196, "HlodDemandStatusReadback"},
    }};
    Json output{{"completed_frame_history", Json::array()},
                {"required_passes", Json::array()}, {"layout_accepted", false}};
    bool runtime_capture_observed = false;
    for (const auto &[frame_id, timings] : history)
    {
        Json passes = Json::array();
        for (const auto &pass : timings.passes)
        {
            runtime_capture_observed |= (pass.pass_id >= 180 && pass.pass_id <= 185) ||
                pass.pass_id == 187 || pass.pass_id == 188;
            passes.push_back({{"pass_id", pass.pass_id}, {"name", pass.name},
                {"nanoseconds", pass.nanoseconds}, {"measured", pass.measured},
                {"queue", static_cast<unsigned>(pass.queue)}});
        }
        output["completed_frame_history"].push_back(
            {{"completed_frame_id", frame_id}, {"passes", std::move(passes)}});
    }
    bool complete = true;
    bool hlod_complete = true;
    bool package_uploaded = false;
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
        if (pass_id == 186) package_uploaded = present;
        output["required_passes"].push_back({{"pass_id", pass_id},
            {"name", std::string(name)}, {"present", present},
            {"observed", std::move(observed)}});
    }
    output["local_probe"] = {{"sample_ready", sample_ready},
        {"baked_package_uploaded", package_uploaded},
        {"runtime_capture_observed", runtime_capture_observed}};
    complete &= sample_ready && package_uploaded && !runtime_capture_observed;
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
                            std::size_t active_slots, std::size_t slot_count,
                            std::size_t created, std::size_t updated,
                            std::size_t destroyed, std::size_t skin_active,
                            std::size_t skin_history,
                            std::size_t dynamic_visibility_refs,
                            std::size_t hzb_history_input) {
        const auto &row = rows[index];
        if (row.frame != index + 1 || row.instances != instances ||
            row.stable_slots != active_slots || row.slot_count != slot_count ||
            row.anonymous_instances != 0 || row.created != created ||
            row.updated != updated || row.destroyed != destroyed ||
            row.skin_cache_active != skin_active ||
            row.skin_history_valid != skin_history ||
            row.dynamic_visibility_refs != dynamic_visibility_refs ||
            row.hzb_history_input != hzb_history_input)
            throw std::runtime_error("persistent GPU scene probe diagnostics mismatch");
        const auto maximum_payload_upload = row.slot_count * instance_bytes;
        if (row.uploaded_bytes > maximum_payload_upload ||
            (row.uploaded_bytes == 0) != (row.copy_regions == 0) ||
            row.copy_regions > row.slot_count * 6)
            throw std::runtime_error("persistent GPU scene probe upload exceeds its active-slot bound");
    };

    expect(0, 2, 2, 2, 2, 0, 0, 1, 0, 2, 0);
    expect(1, 2, 2, 2, 0, 0, 0, 1, 1, 2, 1);
    expect(2, 2, 2, 2, 0, 1, 0, 1, 1, 2, 1);
    expect(3, 1, 1, 2, 0, 0, 1, 1, 1, 1, 1);
    expect(4, 2, 2, 2, 1, 0, 0, 1, 1, 2, 1);
    expect(5, 2, 2, 2, 0, 0, 0, 1, 1, 2, 1);
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
        {"upload_bytes_policy", "Per-owner backing initialization may upload unchanged active slots; each row is bounded by its active slot payload."},
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
        // Previous-skin bindings belong to each view, not the canonical scene payload.
        if (row.instances != source_count || row.stable_slots != source_count ||
            row.slot_count != source_count || row.anonymous_instances != 0 ||
            row.dynamic_visibility_refs != visibility_refs || row.skin_cache_active != skin_count ||
            row.skin_gpu_dispatches != skin_count || row.skin_history_valid != history_count ||
            row.skin_gpu_history_reads != history_count ||
            row.created != (index == 0 || index == 2 ? 2u : 0u) ||
            row.updated != 0 || row.destroyed != 0)
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
    if (!first || !second) return false;
    const auto expected=first.tellg();
    if (expected < 0 || expected != second.tellg()) return false;
    first.seekg(0);
    second.seekg(0);
    std::array<char, 64 * 1024> a{}, b{};
    std::streamoff total{};
    while (first && second)
    {
        first.read(a.data(), static_cast<std::streamsize>(a.size()));
        second.read(b.data(), static_cast<std::streamsize>(b.size()));
        const auto count = first.gcount();
        if (count != second.gcount() ||
            !std::equal(a.begin(), a.begin() + count, b.begin()))
            return false;
        total += count;
    }
    return !first.bad() && !second.bad() && first.eof() && second.eof() &&
        total == static_cast<std::streamoff>(expected);
}

std::pair<std::uint64_t, std::uint64_t> ValidateMaterialReloadShadowDepth(
    const std::filesystem::path &path, Json &receipt)
{
    const auto metadata = Json::parse(std::ifstream(path));
    const auto &resources = metadata.at("resources");
    const auto find = [&](std::string_view name) {
        return std::ranges::find_if(resources, [&](const auto &entry) {
            return entry.value("name", std::string{}) == name;
        });
    };
    auto selected = find("shadow_atlas");
    if (selected == resources.end()) selected = find("virtual_shadow_static");
    if (selected == resources.end() || selected->value("native_format", std::string{}) != "DXGI_FORMAT_R32_TYPELESS")
        throw std::runtime_error("material reload shadow probe has no captured depth atlas");
    const auto raw_path = path.parent_path() / selected->at("file").get<std::string>();
    std::ifstream raw(raw_path, std::ios::binary | std::ios::ate);
    const auto expected = selected->at("row_size").get<std::uint64_t>() * selected->at("height").get<std::uint64_t>();
    if (!raw || expected == 0 || expected % sizeof(float) || raw.tellg() != std::streamoff(expected))
        throw std::runtime_error("material reload shadow depth readback has invalid size");
    raw.seekg(0);
    std::array<float, 4096> words{};
    std::uint64_t visible{};
    while (raw)
    {
        raw.read(reinterpret_cast<char *>(words.data()), sizeof(words));
        const auto bytes = raw.gcount();
        if (bytes % sizeof(float)) throw std::runtime_error("truncated shadow depth readback");
        for (std::streamsize index = 0; index < bytes / sizeof(float); ++index)
        {
            const auto depth = words[static_cast<std::size_t>(index)];
            if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f)
                throw std::runtime_error("material reload shadow depth is nonfinite or out of range");
            visible += depth > 0.0f;
        }
    }
    if (!raw.eof() || visible == 0)
        throw std::runtime_error("material reload shadow depth contains no rasterized caster");
    const auto &preparation = metadata.at("static_shadow_preparation");
    const auto builds = preparation.at("static_membership_builds").get<std::uint64_t>();
    const auto rebinds = preparation.at("static_binding_rebinds").get<std::uint64_t>();
    receipt.push_back({{"capture", path.filename().string()}, {"resource", selected->at("name")},
        {"covered_depth_samples", visible}, {"static_membership_builds", builds},
        {"static_binding_rebinds", rebinds}});
    return {builds, rebinds};
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

struct WaterSurfaceCapture
{
    std::uint64_t pixels{}, history_pixels{}, nonzero_unpeeled{}, changed_color{};
    std::set<std::pair<std::uint32_t,std::uint32_t>> identities;
};

#if defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
struct TranslucencyDepthCapture
{
    std::uint32_t width{}, height{};
    std::vector<float> front0, front1, exit0, exit1;
    std::vector<std::uint32_t> component0, component1;
    std::vector<std::array<std::uint32_t, 2>> identity0, identity1;
};

TranslucencyDepthCapture ReadTranslucencyDepthCapture(const std::filesystem::path &path)
{
    const auto diagnostic = Json::parse(std::ifstream(path));
    struct Image { std::uint32_t width{}, height{}; std::size_t row{}; std::vector<std::byte> bytes; };
    const auto find = [&](std::string_view name) -> const Json * {
        for (const auto &resource : diagnostic.at("resources"))
            if (resource.at("name").get<std::string>() == name) return &resource;
        return nullptr;
    };
    const auto load = [&](std::string_view name, std::string_view format, std::size_t pixel_bytes) {
        const auto *entry = find(name);
        if (!entry || entry->at("format").get<std::string>() != format)
            throw std::runtime_error("missing or invalid optical-pair capture resource: " + std::string(name));
        Image image;
        image.width = entry->at("width").get<std::uint32_t>();
        image.height = entry->at("height").get<std::uint32_t>();
        image.row = entry->at("row_size").get<std::size_t>();
        if (!image.width || !image.height || image.row != std::size_t(image.width) * pixel_bytes)
            throw std::runtime_error("invalid optical-pair capture extent: " + std::string(name));
        std::ifstream input(path.parent_path() / entry->at("file").get<std::string>(), std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != std::streamoff(image.row * image.height))
            throw std::runtime_error("invalid optical-pair capture bytes: " + std::string(name));
        image.bytes.resize(image.row * image.height);
        input.seekg(0);
        if (!input.read(reinterpret_cast<char *>(image.bytes.data()), static_cast<std::streamsize>(image.bytes.size())))
            throw std::runtime_error("could not read optical-pair capture: " + std::string(name));
        return image;
    };
    const auto front0 = load("translucency_depth", "DXGI_FORMAT_R32_TYPELESS", 4);
    const auto front1 = load("translucency_depth_layer1", "DXGI_FORMAT_R32_TYPELESS", 4);
    const auto component0 = load("translucency_optical_component_0", "DXGI_FORMAT_R32_UINT", 4);
    const auto component1 = load("translucency_optical_component_1", "DXGI_FORMAT_R32_UINT", 4);
    const auto identity0 = load("translucency_metadata", "DXGI_FORMAT_R32G32_UINT", 8);
    const auto identity1 = load("translucency_metadata_layer1", "DXGI_FORMAT_R32G32_UINT", 8);
    const auto *exit_format = find("translucency_exit_depth");
    if (!exit_format) throw std::runtime_error("gathered exit depth is absent from translucency capture");
    const auto format = exit_format->at("format").get<std::string>();
    const std::size_t exit_pixel_bytes = format == "DXGI_FORMAT_R32_FLOAT" ? 4u :
        format == "DXGI_FORMAT_R32G32_FLOAT" ? 8u :
        format == "DXGI_FORMAT_R32G32B32A32_FLOAT" ? 16u : 0u;
    if (!exit_pixel_bytes) throw std::runtime_error("gathered exit depth has an unsupported native format");
    const auto exit = load("translucency_exit_depth", format, exit_pixel_bytes);
    if (front0.width != front1.width || front0.height != front1.height || front0.width != component0.width ||
        front0.height != component0.height || front0.width != component1.width || front0.height != component1.height ||
        front0.width != identity0.width || front0.height != identity0.height || front0.width != identity1.width ||
        front0.height != identity1.height || front0.width != exit.width || front0.height != exit.height || exit_pixel_bytes < 8)
        throw std::runtime_error("optical-pair capture surfaces have different extents or fewer than two exit channels");
    const auto read_u32 = [](const std::byte *bytes) { std::uint32_t value{}; std::memcpy(&value, bytes, 4); return value; };
    const auto read_f32 = [](const std::byte *bytes) { float value{}; std::memcpy(&value, bytes, 4); return value; };
    const auto pixel = [](const Image &image, std::uint32_t x, std::uint32_t y, std::size_t bytes) {
        return image.bytes.data() + std::size_t(y) * image.row + std::size_t(x) * bytes;
    };
    TranslucencyDepthCapture result;
    result.width = front0.width; result.height = front0.height;
    const std::size_t count = std::size_t(result.width) * result.height;
    result.front0.resize(count); result.front1.resize(count); result.component0.resize(count); result.component1.resize(count);
    result.identity0.resize(count); result.identity1.resize(count);
    result.exit0.resize(count); result.exit1.resize(count);
    for (std::uint32_t y = 0; y < result.height; ++y)
        for (std::uint32_t x = 0; x < result.width; ++x)
        {
            const auto index = std::size_t(y) * result.width + x;
            result.front0[index] = read_f32(pixel(front0, x, y, 4));
            result.front1[index] = read_f32(pixel(front1, x, y, 4));
            result.component0[index] = read_u32(pixel(component0, x, y, 4));
            result.component1[index] = read_u32(pixel(component1, x, y, 4));
            const auto *key0 = pixel(identity0, x, y, 8);
            const auto *key1 = pixel(identity1, x, y, 8);
            result.identity0[index] = {read_u32(key0), read_u32(key0 + 4)};
            result.identity1[index] = {read_u32(key1), read_u32(key1 + 4)};
            const auto *exit_sample = pixel(exit, x, y, exit_pixel_bytes);
            result.exit0[index] = read_f32(exit_sample);
            result.exit1[index] = read_f32(exit_sample + 4);
            if (!std::isfinite(result.front0[index]) || !std::isfinite(result.front1[index]) ||
                !std::isfinite(result.exit0[index]) || !std::isfinite(result.exit1[index]))
                throw std::runtime_error("optical-pair capture contains a non-finite depth");
        }
    return result;
}

std::uint64_t ValidateNestedOpticalPair(const TranslucencyDepthCapture &nested,
    const TranslucencyDepthCapture &outer_only, std::uint16_t outer_id, std::uint16_t inner_id)
{
    if (nested.width != outer_only.width || nested.height != outer_only.height || !outer_id || !inner_id || outer_id == inner_id)
        throw std::runtime_error("optical-pair fixture identities or capture extents are invalid");
    std::uint64_t paired_pixels{};
    for (std::size_t index = 0; index < nested.front0.size(); ++index)
    {
        if (nested.component0[index] != outer_id || nested.component1[index] != inner_id ||
            nested.front0[index] <= 0.0f || nested.front1[index] <= 0.0f)
            continue;
        const auto nested_outer_exit = nested.exit0[index];
        const auto nested_inner_exit = nested.exit1[index];
        const auto reference_outer_exit = outer_only.exit0[index];
        const auto &outer_key = nested.identity0[index];
        const auto &inner_key = nested.identity1[index];
        if (nested_outer_exit <= 0.0f || nested_inner_exit <= 0.0f || reference_outer_exit <= 0.0f ||
            outer_only.component0[index] != outer_id ||
            outer_key != inner_key || (outer_key[0] == 0 && outer_key[1] == 0) ||
            !(nested_outer_exit < nested_inner_exit) ||
            std::abs(nested_outer_exit - reference_outer_exit) > 2.0e-6f)
            throw std::runtime_error("nested-shell exit pairing differs from its outer-only reference");
        ++paired_pixels;
    }
    if (paired_pixels < 8)
        throw std::runtime_error("nested-shell GPU oracle did not find enough overlapping component pixels");
    return paired_pixels;
}
#endif

WaterSurfaceCapture ValidateWaterSurface(const std::filesystem::path &path, bool empty)
{
    const auto diagnostic = Json::parse(std::ifstream(path));
    const auto active_width = diagnostic.at("width").get<std::uint32_t>();
    const auto active_height = diagnostic.at("height").get<std::uint32_t>();
    struct Image { std::uint32_t width{}, height{}; std::size_t row{}; std::vector<std::byte> bytes; };
    const auto find = [&](std::string_view name) -> const Json * {
        for (const auto &resource : diagnostic.at("resources"))
            if (resource.at("name").get<std::string>() == name) return &resource;
        return nullptr;
    };
    if (empty)
    {
        for (const auto *name : {"translucency_depth", "translucency_motion", "translucency_history",
                "translucency_classification", "translucency_metadata", "translucency_unpeeled",
                "translucency_transport", "translucency_color", "translucency_depth_layer1",
                "translucency_optical_component_0", "translucency_optical_component_1",
                "translucency_optical_component_2", "translucency_optical_component_3",
                "translucency_exit_depth", "translucency_metadata_layer1"})
            if (find(name)) throw std::runtime_error("empty water frame retained transparent capture resources");
        return {};
    }
    const auto load = [&](std::string_view name, std::string_view format, std::size_t pixel_bytes) {
        const auto *entry = find(name);
        if (!entry || entry->at("format").get<std::string>() != format)
            throw std::runtime_error("missing or invalid transparent capture format: " + std::string(name));
        Image result; result.width = entry->at("width").get<std::uint32_t>();
        result.height = entry->at("height").get<std::uint32_t>();
        result.row = entry->at("row_size").get<std::size_t>();
        if (result.width < active_width || result.height < active_height ||
            result.row != std::size_t(result.width) * pixel_bytes)
            throw std::runtime_error("invalid transparent capture extent");
        std::ifstream file(path.parent_path() / entry->at("file").get<std::string>(), std::ios::binary | std::ios::ate);
        if (!file || file.tellg() != std::streamoff(result.row * result.height))
            throw std::runtime_error("invalid transparent capture byte count");
        result.bytes.resize(result.row * result.height); file.seekg(0);
        file.read(reinterpret_cast<char *>(result.bytes.data()), static_cast<std::streamsize>(result.bytes.size()));
        if (!file) throw std::runtime_error("could not read transparent capture");
        return result;
    };
    const auto depth = load("translucency_depth", "DXGI_FORMAT_R32_TYPELESS", 4);
    const auto opaque = load("depth", "DXGI_FORMAT_R32_TYPELESS", 4);
    const auto color = load("translucency_color", "DXGI_FORMAT_R16G16B16A16_FLOAT", 8);
    const auto background = load("composite_hdr", "DXGI_FORMAT_R16G16B16A16_FLOAT", 8);
    const auto motion = load("translucency_motion", "DXGI_FORMAT_R16G16_FLOAT", 4);
    const auto history = load("translucency_history", "DXGI_FORMAT_R32G32_FLOAT", 8);
    const auto classification = load("translucency_classification", "DXGI_FORMAT_R16G16B16A16_FLOAT", 8);
    const auto metadata = load("translucency_metadata", "DXGI_FORMAT_R32G32_UINT", 8);
    const auto unpeeled = load("translucency_unpeeled", "DXGI_FORMAT_R32_UINT", 4);
    const auto transport = load("translucency_transport", "DXGI_FORMAT_R16G16_FLOAT", 4);
    const auto ptr = [](const Image &image, std::uint32_t x, std::uint32_t y, std::size_t pixel_bytes) {
        return image.bytes.data() + std::size_t(y) * image.row + std::size_t(x) * pixel_bytes;
    };
    const auto u32 = [](const std::byte *value) { std::uint32_t result; std::memcpy(&result,value,4); return result; };
    const auto f32 = [](const std::byte *value) { float result; std::memcpy(&result,value,4); return result; };
    const auto half = [](const std::byte *value) { std::uint16_t bits; std::memcpy(&bits,value,2);
        return DirectX::PackedVector::XMConvertHalfToFloat(bits); };
    WaterSurfaceCapture result;
    for (std::uint32_t y=0; y<active_height; ++y) for (std::uint32_t x=0; x<active_width; ++x)
    {
        const auto *id=ptr(metadata,x,y,8); const auto key=u32(id), generation=u32(id+4);
        const auto count=u32(ptr(unpeeled,x,y,4));
        if (count != 0) ++result.nonzero_unpeeled;
        if (!key) continue;
        ++result.pixels; result.identities.emplace(key,generation);
        const auto front=f32(ptr(depth,x,y,4)), behind=f32(ptr(opaque,x,y,4));
        if (!std::isfinite(front) || !std::isfinite(behind) || !(front > behind) || front > 1.0f)
            throw std::runtime_error("transparent front is not in front of opaque depth");
        const auto *m=ptr(motion,x,y,4), *h=ptr(history,x,y,8), *c=ptr(classification,x,y,8), *t=ptr(transport,x,y,4);
        for (unsigned i=0;i<2;++i)
            if (!std::isfinite(half(m+2*i)) || !std::isfinite(f32(h+4*i)) || !std::isfinite(half(t+2*i)))
                throw std::runtime_error("transparent motion/history/transport is nonfinite");
        for (unsigned i=0;i<4;++i) if (!std::isfinite(half(c+2*i)))
            throw std::runtime_error("transparent classification is nonfinite");
        const auto *hdr=ptr(color,x,y,8), *prior=ptr(background,x,y,8);
        bool changed=false;
        for (unsigned i=0;i<4;++i)
        {
            const auto value=half(hdr+2*i), before=half(prior+2*i);
            if (!std::isfinite(value) || !std::isfinite(before)) throw std::runtime_error("water HDR is nonfinite");
            if (i<3 && value != before) changed=true;
        }
        if (changed) ++result.changed_color;
        const auto opacity=half(c+4);
        if (!(opacity >= 0.0f && opacity <= 1.0f)) throw std::runtime_error("transparent opacity outside range");
        if (f32(h+4)>0.0f) ++result.history_pixels;
    }
    if (!result.pixels) throw std::runtime_error("water capture has no visible transparent pixels");
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

Json ValidateAoVisibilityCapture(const std::filesystem::path &path,
                                 std::uint32_t active_width, std::uint32_t active_height)
{
    const auto diagnostic = Json::parse(std::ifstream(path));
    struct Output
    {
        const char *name;
        const char *format;
        std::size_t pixel_bytes;
        bool half_float;
    };
    constexpr std::array outputs{
        Output{"ao_raw", "DXGI_FORMAT_R32G32B32A32_FLOAT", 16, false},
        Output{"ao_resolved", "DXGI_FORMAT_R16_FLOAT", 2, true},
        Output{"ao_history", "DXGI_FORMAT_R32G32B32A32_FLOAT", 16, false},
    };
    Json result = Json::object();
    for (const auto &output : outputs)
    {
        const Json *resource{};
        for (const auto &candidate : diagnostic.at("resources"))
            if (candidate.at("name").get<std::string>() == output.name)
            {
                resource = &candidate;
                break;
            }
        if (!resource || resource->at("format").get<std::string>() != output.format)
            throw std::runtime_error(std::format("AO capture is missing {} in its expected format", output.name));
        const auto width = resource->at("width").get<std::uint32_t>();
        const auto height = resource->at("height").get<std::uint32_t>();
        const auto row_size = resource->at("row_size").get<std::size_t>();
        if (width < active_width || height < active_height ||
            row_size != std::size_t(width) * output.pixel_bytes)
            throw std::runtime_error(std::format("AO capture {} has an invalid extent or row size", output.name));
        std::ifstream input(path.parent_path() / resource->at("file").get<std::string>(),
            std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != std::streamoff(row_size * height))
            throw std::runtime_error(std::format("AO capture {} has an invalid byte count", output.name));
        std::vector<std::byte> row(row_size);
        float minimum = 1.0f;
        float maximum = 0.0f;
        std::uint64_t occluded_pixels{};
        input.seekg(0);
        for (std::uint32_t y = 0; y < active_height; ++y)
        {
            input.read(reinterpret_cast<char *>(row.data()), static_cast<std::streamsize>(row.size()));
            if (!input) throw std::runtime_error(std::format("Could not read AO capture {}", output.name));
            for (std::uint32_t x = 0; x < active_width; ++x)
            {
                float visibility{};
                const auto *pixel = row.data() + std::size_t(x) * output.pixel_bytes;
                if (output.half_float)
                {
                    std::uint16_t bits{};
                    std::memcpy(&bits, pixel, sizeof(bits));
                    visibility = DirectX::PackedVector::XMConvertHalfToFloat(bits);
                }
                else
                    std::memcpy(&visibility, pixel, sizeof(visibility));
                if (!std::isfinite(visibility) || visibility < 0.0f || visibility > 1.0f)
                    throw std::runtime_error(std::format("AO capture {} has a nonfinite or out-of-range visibility", output.name));
                minimum = std::min(minimum, visibility);
                maximum = std::max(maximum, visibility);
                occluded_pixels += visibility < 0.999f;
            }
        }
        result[output.name] = {{"minimum_visibility", minimum}, {"maximum_visibility", maximum},
            {"occluded_pixels_below_0_999", occluded_pixels}};
        if (std::string_view(output.name) == "ao_raw" && occluded_pixels == 0)
            throw std::runtime_error("AO grass fixture produced no nontrivial raw visibility despite its ground/grass occluders");
    }
    return result;
}

void ValidateVisibilityCommandCapture(const std::filesystem::path &path,
    std::span<const hs::RenderInstance> static_instances,
    std::span<const hs::RenderInstance> dynamic_instances, std::uint32_t frame, Json &receipt)
{
    const auto diagnostic = Json::parse(std::ifstream(path));
    const auto &metadata = diagnostic.at("visibility_commands");
    const auto &masks = metadata.at("range_flag_masks");
    const auto mesh_eligible = masks.at("mesh_eligible").get<std::uint32_t>();
    const auto lod_family = masks.at("lod_family").get<std::uint32_t>();
    const auto empty_source = masks.at("empty_source").get<std::uint32_t>();
    const auto static_source = masks.at("static_source").get<std::uint32_t>();
    if (!mesh_eligible || !lod_family || !empty_source || !static_source ||
        (mesh_eligible & (lod_family | empty_source | static_source)) ||
        (lod_family & (empty_source | static_source)) || (empty_source & static_source))
        throw std::runtime_error("visibility range flag masks are invalid");
    const auto buckets = metadata.at("bucket_count").get<std::size_t>();
    const auto views = metadata.at("active_views").get<std::size_t>();
    const auto expected_views = dynamic_instances.empty() ? 4u : 1u;
    if (views != expected_views || metadata.at("command_stride") != 24 ||
        metadata.at("range_stride") != 32 || metadata.at("range_count_per_set") != buckets + 1)
        throw std::runtime_error("visibility command capture layout mismatch");
    const auto read = [&](const char *key, std::size_t words) {
        std::ifstream file(path.parent_path() / metadata.at(key).get<std::string>(), std::ios::binary | std::ios::ate);
        if (!file || file.tellg() != std::streamoff(words * sizeof(std::uint32_t)))
            throw std::runtime_error("visibility command capture byte count mismatch");
        std::vector<std::uint32_t> values(words);
        file.seekg(0);
        file.read(reinterpret_cast<char *>(values.data()), static_cast<std::streamsize>(words * sizeof(std::uint32_t)));
        if (!file) throw std::runtime_error("visibility command capture read failed");
        return values;
    };
    const auto ranges = read("visibility_ranges", 3 * (buckets + 1) * 8);
    const auto static_draw = read("static_draw_args", buckets * views * 6);
    const auto dynamic_draw = read("dynamic_draw_args", buckets * views * 6);
    const auto rejected = read("static_rejected_args", buckets * 6);
    const auto retest = read("static_retest_args", buckets * 6);
    const auto &records = diagnostic.at("records");
    const std::size_t environment_buckets = hs::render_data::kEnvironmentMaterialNames.size() *
        hs::render_data::kEnvironmentGeometryLodCount;
    if (environment_buckets >= buckets) throw std::runtime_error("visibility bucket catalog is incomplete");
    const auto present = [&](std::span<const hs::RenderInstance> instances, std::size_t bucket) {
        return std::ranges::any_of(instances, [&](const auto &instance) {
            if (hs::render_data::IsEnvironmentMesh(instance.mesh))
                return bucket / 3 == hs::render_data::EnvironmentGeometryOrdinal(instance.mesh, instance.environment_variant) &&
                    bucket < environment_buckets;
            return instance.mesh == hs::RenderMesh::Archer && bucket == environment_buckets;
        });
    };
    const auto verify = [&](const std::vector<std::uint32_t> &commands, std::size_t set,
                            std::size_t view_count, bool is_static) {
        for (std::size_t bucket = 0; bucket < buckets; ++bucket)
        {
            const auto *range = ranges.data() + (set * (buckets + 1) + bucket) * 8;
            const bool expected = present(is_static ? static_instances : dynamic_instances, bucket);
            if (((range[6] & empty_source) == 0) != expected || range[3] != buckets ||
                ((range[6] & static_source) != 0) != (is_static && !expected) ||
                ((range[6] & lod_family) != 0) != (set != 2 && bucket < environment_buckets))
                throw std::runtime_error("visibility source range disagrees with fixture input");
            for (std::size_t view = 0; view < view_count; ++view)
            {
                const auto *command = commands.data() + (view * buckets + bucket) * 6;
                if (!expected)
                {
                    if (std::any_of(command, command + 6, [](auto value) { return value != 0; }))
                        throw std::runtime_error("empty source left a nonzero indirect command");
                    continue;
                }
                const auto record_id = command[0] >> 2;
                if ((command[0] & 3u) != view || record_id != range[4] + view || record_id >= records.size())
                    throw std::runtime_error("indirect command record/view encoding mismatch");
                const auto &record = records.at(record_id);
                if (record.at("id") != record_id || record.at("refsource") != (set == 0 ? 1 : set == 1 ? 2 : 3))
                    throw std::runtime_error("indirect command refers to the wrong geometry source");
                const auto mesh = bucket < environment_buckets
                    ? hs::render_data::EnvironmentMeshForOrdinal(static_cast<std::uint32_t>(bucket / 3))
                    : hs::RenderMesh::Archer;
                if (record.at("mesh").get<std::uint32_t>() != static_cast<std::uint32_t>(mesh))
                    throw std::runtime_error("indirect command geometry mesh does not match the input bucket");
                if (record.at("index_count").get<std::uint32_t>() != range[2] ||
                    record.at("meshlet_count").get<std::uint32_t>() != range[5] ||
                    (record.at("flags").get<std::uint32_t>() & mesh_eligible) != (range[6] & mesh_eligible))
                    throw std::runtime_error("indirect range does not match its geometry record");
                const auto count = record.at("count").get<std::uint32_t>();
                if (count > range[1] || command[4] || command[5])
                    throw std::runtime_error("indirect command count/reserved fields invalid");
                if ((range[6] & mesh_eligible) != 0)
                {
                    const auto groups = std::uint64_t(range[5]) * count;
                    if (command[1] != std::min<std::uint64_t>(groups, 65535u) ||
                        command[2] != (groups ? (groups + 65534u) / 65535u : 1u) || command[3] != 1)
                        throw std::runtime_error("mesh indirect command encoding mismatch");
                }
                else if (command[1] != range[2] || command[2] != count ||
                         command[3] != record.at("first_index").get<std::uint32_t>())
                    throw std::runtime_error("indexed indirect command encoding mismatch");
            }
        }
    };
    verify(static_draw, 0, views, true);
    verify(dynamic_draw, 1, views, false);
    verify(retest, 2, 1, true);
    std::uint64_t meadow_rejected{};
    const auto meadow_begin = hs::render_data::EnvironmentGeometryOrdinal(hs::RenderMesh::MeadowSurface, 0) * 3u;
    for (std::size_t bucket = 0; bucket < buckets; ++bucket)
    {
        const auto *command = rejected.data() + bucket * 6;
        const auto *range = ranges.data() + bucket * 8;
        if (!present(static_instances, bucket))
        {
            if (std::any_of(command, command + 6, [](auto value) { return value != 0; }))
                throw std::runtime_error("empty static source left a rejected command");
        }
        else if (command[0] || command[1] != range[2] || command[2] > range[1] ||
                 command[3] || command[4] || command[5])
            throw std::runtime_error("static rejected command encoding mismatch");
        if (bucket >= meadow_begin && bucket < meadow_begin + 3u) meadow_rejected += command[2];
    }
    if ((frame == 16 || frame == 17 || frame >= 22) && meadow_rejected == 0)
        throw std::runtime_error("empty dynamic source erased or failed to produce the occluded static meadow command");
    receipt.push_back({{"capture", path.filename().string()}, {"active_views", views},
        {"static_instances", static_instances.size()}, {"dynamic_instances", dynamic_instances.size()},
        {"meadow_rejected", meadow_rejected}});
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
        if (observations.size() > 2)
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
    const auto runner_entry = Clock::now();
    Json receipt{{"schema_version", 1}, {"runner", "render-only"},
        {"fixture", "static-environment-moving-player-v1"}, {"execution_valid", false}};
    std::filesystem::path artifacts;
    std::filesystem::path temporary_pack;
    std::string failure_code = "HS_RENDER_ARGUMENT_INVALID";
    int exit_code = 1;
    const auto write_receipt = [&] {
        if (!artifacts.empty() && artifacts.is_absolute() && std::filesystem::is_directory(artifacts))
        {
            std::ofstream output(artifacts / "result.json");
            output << receipt.dump(2) << '\n';
            output.close();
            if (!output)
            {
                std::cerr << "Could not write render result: " << artifacts / "result.json" << '\n';
                return 1;
            }
        }
        if (exit_code != 0)
            std::cerr << receipt.value("error_code", failure_code) << ": "
                      << receipt.value("error", std::string{}) << '\n';
        return exit_code;
    };
    try
    {
        hs::RendererConfig config;
        bool environment_preview = false;
        bool slime_preview = false;
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
        bool translucency_diagnostics_probe{}, translucency_diagnostics_mode_explicit{}, shadow_transition_probe{}, hero_layer_probe{};
        bool translucency_optical_pair_probe{};
        bool world_transport_probe{};
        bool single_layer_water_probe{};
        bool gameplay_scene{};
        bool static_shadow_bake_probe{};
        bool static_shadow_bake_only{};
        bool local_probe_revision_probe{};
        bool scene_probe_bake_only{};
        bool oit_overdraw_probe{};
        std::string vfx_composition_probe;
        bool entity_surface_probe{};
        bool camera_basis_probe{};
        bool light_list_probe{};
        bool lighting_auxiliary_probe{};
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
            else if (arg == "--environment-preview") environment_preview = true;
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
            else if (arg == "--translucency-diagnostics-probe") translucency_diagnostics_probe = true;
            else if (arg == "--translucency-optical-pair-probe") translucency_optical_pair_probe = true;
            else if (arg == "--shadow-transition-probe") shadow_transition_probe = true;
            else if (arg.starts_with("--translucency-diagnostics-mode="))
            {
                if (translucency_diagnostics_mode_explicit) throw std::runtime_error("duplicate hero work mode");
                translucency_diagnostics_mode_explicit = true;
                const auto mode = arg.substr(std::string_view("--translucency-diagnostics-mode=").size());
                if (mode == "full") config.translucency_diagnostics_mode = hs::TranslucencyDiagnosticsMode::Full;
                else if (mode == "no-environment") config.translucency_diagnostics_mode = hs::TranslucencyDiagnosticsMode::NoEnvironment;
                else if (mode == "coverage-only") config.translucency_diagnostics_mode = hs::TranslucencyDiagnosticsMode::CoverageOnly;
                else throw std::runtime_error("hero work mode must be full/no-environment/coverage-only");
            }
            else if (arg == "--world-transport-probe") world_transport_probe = true;
            else if (arg == "--single-layer-water-probe") single_layer_water_probe = true;
            else if (arg == "--gameplay-scene") gameplay_scene = true;
            else if (arg == "--static-shadow-bake-probe") gameplay_scene = static_shadow_bake_probe = true;
            else if (arg == "--static-shadow-bake-only") static_shadow_bake_only = true;
            else if (arg == "--local-probe-revision-probe") gameplay_scene = local_probe_revision_probe = true;
            else if (arg == "--scene-probe-bake-only") scene_probe_bake_only = true;
            else if (arg == "--oit-overdraw-probe") oit_overdraw_probe = true;
            else if (arg.starts_with("--vfx-composition-probe=")) vfx_composition_probe = arg.substr(24);
            else if (arg == "--entity-surface-probe") entity_surface_probe = true;
            else if (arg == "--camera-basis-probe") camera_basis_probe = true;
            else if (arg == "--light-list-probe") light_list_probe = true;
            else if (arg == "--lighting-auxiliary-probe") lighting_auxiliary_probe = true;
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
        if (entity_surface_probe)
        {
            if ((frames_explicit && frames != 8) || !vfx_composition_probe.empty() || !grass_history_probe.empty())
                throw std::runtime_error("Entity surface probe requires eight isolated ordinary frames");
            frames = 8;
            config.interpolate = config.dynamic_resolution = config.temporal_reconstruction = false;
            config.grid_floor_probe = config.grid_floor_disable_aa = true;
            config.bloom = false;
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
                grass_history_probe[0] > 'F' || frames < 16 || gameplay_scene || owned_view_probe ||
                surface_motion_resize_probe || persistent_scene_probe || gpu_lod_probe)
                throw std::runtime_error("grass history probe requires A..F and at least 16 ordinary frames");
            config.dynamic_resolution = false;
            config.temporal_reconstruction = grass_history_probe[0] != 'F';
            config.sharpening = 0;
            config.bloom = false;
            if (grass_history_probe[0] == 'F')
            {
                if (frames != 24)
                    throw std::runtime_error("AO grass history probe F requires exactly 24 frames");
                config.ao_mode = hs::GraphicsAoMode::GTAO;
                config.ao_quality = hs::GraphicsQuality::Ultra;
            }
            else
                config.ao_mode = hs::GraphicsAoMode::Off;
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
            receipt["grass_history"] = {{"case", grass_history_probe},
                {"logical_render_hz", grass_cadence}, {"diagnostic_options", true},
                {"frames", Json::array()}};
            if (grass_history_probe[0] == 'F')
            {
                receipt["grass_history"]["ao_mode"] = "gtao";
                receipt["grass_history"]["ao_quality"] = "ultra";
                receipt["grass_history"]["temporal_reconstruction"] = false;
                receipt["grass_history"]["ao_capture_frames"] = {4, 5, 24};
            }
        }
        if (light_list_probe || lighting_auxiliary_probe)
        {
            const auto required_frames = light_list_probe ? 7u : 5u;
            if (!frames_explicit) frames = required_frames;
            if (frames != required_frames) throw std::runtime_error("lighting probe frame count is fixed");
            config.lighting_quality = hs::GraphicsQuality::Ultra;
            config.dynamic_resolution = config.temporal_reconstruction = false;
            config.bloom = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
            if (light_list_probe)
                receipt["light_list"] = {{"local_counts", kLightListLocalCounts},
                    {"directional_counts", kLightListDirectionalCounts}, {"frames", Json::array()}};
            else receipt["lighting_auxiliaries"]["frames"] = Json::array();
        }
        if (!vfx_composition_probe.empty())
        {
            if ((vfx_composition_probe != "fixed" && vfx_composition_probe != "auto" &&
                 vfx_composition_probe != "control" && vfx_composition_probe != "switch") ||
                (frames_explicit && frames != 32) || oit_overdraw_probe || camera_basis_probe ||
                gameplay_scene || owned_view_probe || surface_motion_resize_probe ||
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
            config.bloom = false;
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
                surface_motion_resize_probe || world_transport_probe || provider_switch_probe)
                throw std::runtime_error("OIT/camera-basis probes require their own four/six ordinary render frames");
            config.temporal_reconstruction = false;
            config.bloom = false;
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
        if (single_layer_water_probe)
        {
            if (!frames_explicit) frames = 15;
            if (frames != 15) throw std::runtime_error("single-layer water probe requires exactly 15 frames");
            config.transparency_quality = hs::GraphicsQuality::Ultra;
            config.refraction_quality = hs::GraphicsOptionalQuality::Ultra;
            config.temporal_reconstruction = true;
            config.capture_timing_stream = true;
            config.motion_blur = hs::GraphicsMotionBlur::Low;
            config.depth_of_field = true;
            receipt["single_layer_water"]["frames"] = Json::array();
        }
        if (world_transport_probe)
        {
            if (!frames_explicit) frames = 32;
            if (frames < 7) throw std::runtime_error("--world-transport-probe requires at least 7 frames");
            config.ao_mode = hs::GraphicsAoMode::GTAO;
            config.gi_mode = hs::GraphicsGiMode::Probe;
            config.reflection_mode = hs::GraphicsReflectionMode::ScreenSpace;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Medium;
        }
        if (static_shadow_bake_only && !static_shadow_bake_probe)
            throw std::runtime_error("--static-shadow-bake-only requires --static-shadow-bake-probe");
        if (scene_probe_bake_only && !local_probe_revision_probe)
            throw std::runtime_error("--scene-probe-bake-only requires --local-probe-revision-probe");
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
        if (translucency_diagnostics_mode_explicit && !translucency_diagnostics_probe)
            throw std::runtime_error("--translucency-diagnostics-mode requires --translucency-diagnostics-probe");
        if (shadow_transition_probe && (!translucency_diagnostics_probe || !gameplay_scene))
            throw std::runtime_error("--shadow-transition-probe requires --translucency-diagnostics-probe --gameplay-scene");
        if (translucency_optical_pair_probe && (translucency_diagnostics_probe || gameplay_scene || timing_stream_probe ||
                !grass_history_probe.empty() || !vfx_composition_probe.empty()))
            throw std::runtime_error("--translucency-optical-pair-probe requires its own isolated preview run");
#if !defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
        if (translucency_optical_pair_probe)
            throw std::runtime_error("This runner was built without its optical geometry fixture.");
#endif
        if (translucency_optical_pair_probe)
        {
            if (!frames_explicit) frames = 2;
            if (frames != 2) throw std::runtime_error("--translucency-optical-pair-probe requires exactly two frames");
            config.capture_translucency_diagnostics = true;
            config.transparency_quality = hs::GraphicsQuality::Ultra;
            config.refraction_quality = hs::GraphicsOptionalQuality::Ultra;
            config.temporal_reconstruction = false;
            config.bloom = false;
            slime_preview = true;
            receipt["translucency_optical_pair"] = {{"fixture", "certified-nested-slime-outer-reference"},
                {"requested_frames", frames}, {"functional_only", true}};
        }
        if (translucency_diagnostics_probe)
        {
            if (!frames_explicit) frames = 4;
            if (frames > 8 || timing_stream_probe || !grass_history_probe.empty() || !vfx_composition_probe.empty())
                throw std::runtime_error("Hero work probe requires at most eight isolated ordinary frames");
            if (gameplay_scene && frames != 4) throw std::runtime_error("Hero gameplay/water shadow probe requires exactly four frames");
            config.capture_translucency_diagnostics = config.capture_timing_stream = true;
            hero_layer_probe = !gameplay_scene && frames == 8;
            slime_preview = !gameplay_scene; // Preview replaces environment; gameplay must retain authored water.
            if (shadow_transition_probe)
            {
                // Dedicated supported-domain functional fixture, never a production quality default.
                config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
                config.gi_mode = hs::GraphicsGiMode::Probe;
            }
            config.temporal_reconstruction = false;
            config.bloom = false;
            receipt["translucency_diagnostics"] = {{"fixture", gameplay_scene ? "actual-gameplay-meadow-water" : "existing-slime-family-preview-count1"}, {"requested_frames", frames},
                {"mode", TranslucencyDiagnosticsModeName(config.translucency_diagnostics_mode)}, {"exposure_delta_seconds", 1.0f / 60.0f},
                {"functional_only", true}, {"performance_adoption", false}};
            if (hero_layer_probe)
                receipt["translucency_diagnostics"]["quality_transition"] = {{"request_frame", 5},
                    {"frames1_4", "existing Medium refraction/transparency, one layer and original camera"},
                    {"frames5_8", "Ultra refraction/transparency, four layers, camera yaw90/pitch0 for actual transparent overlap"},
                    {"camera_source", "fixture camera yaw/pitch through MainViewInput"},
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
        if (local_probe_revision_probe && !scene_probe_bake_only && frames < 96)
            throw std::runtime_error("--local-probe-revision-probe requires at least 96 frames");
        if (dynamic_lod_skin_growth_probe && frames < 8)
            throw std::runtime_error("--dynamic-lod-skin-growth-probe requires at least 8 frames");
        if (gpu_lod_probe && frames < 10)
            throw std::runtime_error("--gpu-lod-probe requires at least 10 frames");
        if (surface_motion_resize_probe && frames < 7)
            throw std::runtime_error("--surface-motion-resize-probe requires at least 7 frames");
        if (static_cast<unsigned>(translucency_diagnostics_probe) + static_cast<unsigned>(persistent_scene_probe) +
                static_cast<unsigned>(skin_cache_monster_probe) +
                static_cast<unsigned>(dynamic_lod_skin_growth_probe) +
                static_cast<unsigned>(gpu_lod_probe) +
                static_cast<unsigned>(residency_streaming_probe) +
                static_cast<unsigned>(surface_motion_resize_probe) +
                static_cast<unsigned>(world_transport_probe) +
                static_cast<unsigned>(single_layer_water_probe) +
                static_cast<unsigned>(gameplay_scene && !translucency_diagnostics_probe) +
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
            if (grid_aa_mode == GridAaMode::Fxaa)
                config.vfx_quality = hs::VfxQuality::Low;
            else if (grid_aa_mode == GridAaMode::Taa)
                config.vfx_quality = hs::VfxQuality::High;
            config.grid_floor_disable_aa = grid_aa_mode == GridAaMode::Off;
        }
        if (persistent_scene_probe || material_reload_probe || static_shadow_bake_probe ||
            residency_streaming_probe || dynamic_lod_skin_growth_probe)
        {
            // These probes compare scene/material/skin publications. Temporal
            // reconstruction and unrelated high-quality effects add costly driver
            // instrumentation without exercising those publication contracts.
            config.vfx_quality = hs::VfxQuality::Low;
            config.temporal_reconstruction = false;
            config.ao_mode = hs::GraphicsAoMode::Off;
            config.contact_shadows = false;
            config.reflection_mode = hs::GraphicsReflectionMode::Off;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Off;
        }
        if (residency_streaming_probe)
        {
            config.gi_mode = hs::GraphicsGiMode::Off;
            config.bloom = false;
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
            slime_preview = true;
            config.vfx_quality = hs::VfxQuality::Low;
        }
        if (owned_view_probe)
        {
            config.vfx_quality = hs::VfxQuality::High;
            config.volumetric_quality = hs::GraphicsOptionalQuality::Medium;
        }
        receipt["fixture"] = translucency_diagnostics_probe ? (gameplay_scene ? "translucency-diagnostics-functional-water-world-v1" : "translucency-diagnostics-functional-slime-v1") : provider_switch_probe ? "provider-switch-v1" : owned_view_probe ? "owned-view-history-v1" : persistent_scene_probe
            ? "persistent-gpu-scene-v2"
            : skin_cache_monster_probe ? "skin-cache-monster-v1"
            : dynamic_lod_skin_growth_probe ? "dynamic-lod-skin-growth-v1"
            : gpu_lod_probe ? "gpu-lod-probe-v1"
             : residency_streaming_probe ? "residency-streaming-v3"
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
            if (owned_view_probe &&
                (std::string_view(name) == "capture" || std::string_view(name) == "shutdown"))
                LogStageBegin(name);
            const auto start = Clock::now();
            operation();
            const auto elapsed_ms = StageElapsedMilliseconds(start);
            timings[name] = elapsed_ms;
            LogStageElapsed(name, elapsed_ms);
        };
        hs::SelectedGeneration generation;
        hs::SelectedGeneration material_reload_generation;
        hs::SelectedGeneration material_reload_invalid_generation;
        hs::render_data::SceneProbeDescription probe_fixture_description;
        std::vector<hs::render_data::SceneProbeRecord> probe_fixture_records;
#if !defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
        static_cast<void>(translucency_optical_pair_probe);
#else
        std::optional<OpticalPairFixtureProducts> optical_pair_fixture;
#endif
        failure_code = "HS_GENERATION_INVALID";
        timings["runner_setup_before_generation"] = StageElapsedMilliseconds(runner_entry);
        LogStageElapsed("runner_setup_before_generation", timings["runner_setup_before_generation"]);
        measure("generation", [&] { Require(hs::LoadSelectedGeneration(pointer, generation)); });
#if defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
        if (translucency_optical_pair_probe)
        {
            optical_pair_fixture = BuildOpticalPairFixture(generation, artifacts);
            receipt["translucency_optical_pair"]["fixture_products"] = {
                {"source_generation", optical_pair_fixture->source_generation},
                {"nested_outer_component", optical_pair_fixture->outer_component},
                {"nested_inner_component", optical_pair_fixture->inner_component},
                {"material_slot", optical_pair_fixture->material_slot},
                {"synthetic_product_digests", optical_pair_fixture->output_digests}};
        }
#endif
        if (local_probe_revision_probe)
        {
            constexpr std::string_view id = "render/lighting/local_probes.spkg";
            std::filesystem::path source;
            Require(generation.Require(id, source));
            std::ifstream input(source, std::ios::binary | std::ios::ate);
            if (!input || input.tellg() <= 0 || input.tellg() > 256 * 1024 * 1024)
                throw std::runtime_error("Invalid scene-probe revision fixture source extent");
            std::vector<std::byte> bytes(static_cast<std::size_t>(input.tellg()));
            input.seekg(0);
            if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("Could not read scene-probe revision fixture source");
            const auto hex = [](std::span<const std::byte> value) {
                constexpr char digits[] = "0123456789abcdef";
                std::string result;
                for (const auto byte : value)
                {
                    const auto n = std::to_integer<unsigned>(byte);
                    result.push_back(digits[n >> 4]); result.push_back(digits[n & 15]);
                }
                return result;
            };
            const auto source_digest = "sha256:" + hex(hs::Sha256(bytes));
            if (source_digest != generation.product_digests.at(std::string(id)))
                throw std::runtime_error("Scene-probe revision fixture source digest differs from its Generation");
            const auto package = hs::render_data::ReadSceneProbePackage(bytes, hs::MakeAssetId(id));
            if ((package.payload != hs::render_data::SceneProbePayload::Layout &&
                    package.payload != hs::render_data::SceneProbePayload::Baked) || package.probes.empty())
                throw std::runtime_error("Scene-probe revision fixture requires a validated baked or layout package with a reflection probe");
            auto description = package.description;
            description.grid.dimensions = {2, 2, 2};
            const auto fixture = hs::render_data::BuildSceneProbePackage(hs::MakeAssetId(id),
                package.dependencies, description, std::span(package.probes).first(1));
            const auto verified = hs::render_data::ReadSceneProbePackage(fixture, hs::MakeAssetId(id));
            if (verified.payload != hs::render_data::SceneProbePayload::Layout || verified.probes.size() != 1 ||
                verified.description.grid.NodeCount() != 8 || verified.dependencies != package.dependencies)
                throw std::runtime_error("Scene-probe revision fixture failed package validation");
            probe_fixture_description = verified.description;
            probe_fixture_records = verified.probes;
            const auto path = artifacts / "local-probe-revision-layout.spkg";
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char *>(fixture.data()), static_cast<std::streamsize>(fixture.size()));
            output.close();
            if (!output) throw std::runtime_error("Could not write scene-probe revision fixture");
            const auto digest = "sha256:" + hex(hs::Sha256(fixture));
            const auto source_generation_id = generation.id;
            const auto identity = generation.id + digest;
            generation.id = hex(hs::Sha256(std::as_bytes(std::span(identity.data(), identity.size()))));
            generation.product_paths[std::string(id)] = path;
            generation.product_digests[std::string(id)] = digest;
            receipt["local_probe_fixture"] = {{"reflection_probes", 1}, {"grid_nodes", description.grid.NodeCount()},
                {"total_cube_views", (1 + description.grid.NodeCount()) * 6}, {"maximum_cube_views_per_frame", 6},
                {"source_generation_id", source_generation_id}, {"source_digest", source_digest}, {"fixture_digest", digest}};
        }
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
        const bool material_reload_terrain_archive = material_reload_probe &&
            generation.product_paths.contains("render/terrain/arena_surface.rpkg");
        std::shared_ptr<const hs::StaticRenderScene> world_scene;
        if (world_transport_probe || owned_view_probe || gameplay_scene || single_layer_water_probe || material_reload_terrain_archive)
        {
            failure_code = "HS_RENDER_WORLD_TRANSPORT_SCENE_INVALID";
            std::filesystem::path product_path;
            if (!scene_probe_bake_only)
                Require(generation.Require(config.scene_hlod_product_id, product_path));
            Require(generation.Require("render/lighting/local_probes.spkg", product_path));
            measure("world_scene", [&] { world_scene = hs::app::WorldTransportScene(generation,
                world_transport_probe || material_reload_terrain_archive || scene_probe_bake_only ? 1 : 0); });
            if (!material_reload_terrain_archive) receipt[world_transport_probe ? "world_transport" : "owned_view_scene"] = {
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
        hs::LightView static_shadow_sun;
        if (static_shadow_bake_probe)
        {
            static_shadow_sun.direction = hs::kEnvironmentSunDirection;
            static_shadow_sun.quantity = 50000.0f;
            static_shadow_sun.color = {1.0f, .94f, .86f};
            static_shadow_sun.stable_id = 0x535342414b450001ull;
            static_shadow_sun.shadow_flags = hs::kLightShadowRequested;
        }
        if (static_shadow_bake_only)
        {
            receipt["validation_requested"] = {{"debug_layer", config.validation},
                {"gpu_based_validation", config.gpu_validation}};
            failure_code = "HS_RENDER_STATIC_SHADOW_BAKE_FAILED";
            measure("static_shadow_bake", [&] {
                receipt["static_shadow_bake"] = StaticShadowArchiveProbe(
                    [&](const auto &scene, const auto &light, const auto &begin, const auto &page_sink) {
                        return hs::D3D12Renderer::BakeStaticShadowPages(config, scene, light, begin, page_sink);
                    }, world_scene, static_shadow_sun);
            });
            receipt["static_shadow_bake"]["bake_only"] = true;
            receipt["static_shadow_bake"]["subsequent_render_frames"] = 0;
            receipt["frames"] = 0;
            // The one-shot API returns success only after owner shutdown and a
            // zero native validation error count. Keep that separate from whether
            // validation was requested or GPU-based validation was enabled.
            receipt["validation_errors"] = 0;
            receipt["timings_ms"] = std::move(timings);
            receipt["execution_valid"] = true;
            receipt["error_code"] = "";
            exit_code = 0;
            return write_receipt();
        }
        if (scene_probe_bake_only)
        {
            receipt["validation_requested"] = {{"debug_layer", config.validation},
                {"gpu_based_validation", config.gpu_validation}};
            failure_code = "HS_RENDER_SCENE_PROBE_BAKE_FAILED";
            hs::TaskSystem recording_workers(4);
            config.task_system = &recording_workers;
            std::vector<std::byte> baked;
            measure("scene_probe_bake", [&] {
                Require(hs::D3D12Renderer::BakeSceneProbePackage(config, world_scene, hs::EnvironmentSun(), baked));
            });
            std::vector<std::byte> partitioned_bake;
            measure("scene_probe_partitioned_bake", [&] {
                Require(hs::D3D12Renderer::BakeSceneProbePackage(config, world_scene, hs::EnvironmentSun(), partitioned_bake, 3));
            });
            if (baked != partitioned_bake)
                throw std::runtime_error("Scene-probe Jacobi result depends on capture graph partitioning");
            const auto package = hs::render_data::ReadSceneProbePackage(
                baked, hs::MakeAssetId("render/lighting/local_probes.spkg"));
            const auto &grid = package.description.grid;
            const auto &expected = probe_fixture_description.grid;
            const auto equal = [](const auto &a, const auto &b) {
                return a.x == b.x && a.y == b.y && a.z == b.z;
            };
            if (package.payload != hs::render_data::SceneProbePayload::Baked || package.probes.size() != 1 ||
                probe_fixture_records.size() != 1)
                throw std::runtime_error("Scene-probe bake did not produce the fixture payload");
            const auto &probe = package.probes.front();
            const auto &source = probe_fixture_records.front();
            if (package.description.edge != probe_fixture_description.edge ||
                package.description.ggx_samples != probe_fixture_description.ggx_samples ||
                grid.dimensions != expected.dimensions || !equal(grid.origin, expected.origin) ||
                !equal(grid.spacing, expected.spacing) || probe.stable_id != source.stable_id ||
                !equal(probe.position, source.position) || !equal(probe.influence_min, source.influence_min) ||
                !equal(probe.influence_max, source.influence_max) || !equal(probe.projection_min, source.projection_min) ||
                !equal(probe.projection_max, source.projection_max) || probe.weight != source.weight ||
                probe.influence_fade != source.influence_fade || probe.visibility_bias != source.visibility_bias ||
                probe.visibility_fade != source.visibility_fade || probe.capture_distance != source.capture_distance)
                throw std::runtime_error("Scene-probe bake did not preserve the fixture shape");
            receipt["scene_probe_bake"] = {{"bake_only", true}, {"reflection_probes", package.probes.size()},
                {"grid_nodes", grid.NodeCount()}, {"package_bytes", baked.size()}, {"subsequent_render_frames", 0}};
            receipt["frames"] = 0;
            receipt["validation_errors"] = 0;
            receipt["timings_ms"] = std::move(timings);
            receipt["execution_valid"] = true;
            receipt["error_code"] = "";
            exit_code = 0;
            return write_receipt();
        }
        TestWindow window;
        failure_code = "HS_RENDER_WINDOW_FAILED";
        measure("window", [&] { window.Create(config.width, config.height); });
        config.window = window.handle;
        hs::TaskSystem recording_workers(4);
        config.task_system = &recording_workers;
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
        Clock::time_point renderer_destruction_start{};
        StageBoundaryMarker renderer_destruction_complete{
            owned_view_probe, false, "renderer_destruction", renderer_destruction_start};
        hs::D3D12Renderer renderer;
        StageBoundaryMarker renderer_destruction_begin{
            owned_view_probe, true, "renderer_destruction", renderer_destruction_start};
        failure_code = "HS_RENDER_INITIALIZE_FAILED";
        auto startup_font = ReadDeveloperFont();
        config.font_bytes = startup_font;
        const auto residency_pack_prepare_start = Clock::now();
        if (residency_streaming_probe || world_transport_probe)
        {
            const auto pack_path = artifacts / "renderer-products-pack.hsp";
            std::ofstream pack(pack_path, std::ios::binary | std::ios::trunc);
            if (!pack) throw std::runtime_error("Could not create renderer fixture pack.");
            temporary_pack = pack_path;
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
                    id.ends_with(".hsbin") ? F::VfxMeshAtlas :
                    id == hs::render_data::SurfaceLayout::abi_product ? F::SurfaceLayoutAbi : F::ShaderDxil;
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
                if ((id.starts_with("render/texture/") && id.ends_with(".rpkg")) ||
                    hs::render_data::IsTerrainSurfaceSectionProduct(id))
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
        if (residency_streaming_probe)
        {
            timings["residency_pack_prepare"] = StageElapsedMilliseconds(residency_pack_prepare_start);
            LogStageElapsed("residency_pack_prepare", timings["residency_pack_prepare"]);
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
        if (static_shadow_bake_probe)
        {
            failure_code = "HS_RENDER_STATIC_SHADOW_BAKE_FAILED";
            measure("static_shadow_bake", [&] {
                receipt["static_shadow_bake"] = StaticShadowArchiveProbe(
                    [&](const auto &scene, const auto &light, const auto &begin, const auto &page_sink) {
                        return renderer.BakeStaticShadowPages(scene, light, begin, page_sink);
                    }, world_scene, static_shadow_sun);
            });
            receipt["static_shadow_bake"]["bake_only"] = static_shadow_bake_only;
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
        std::shared_ptr<const hs::StaticRenderScene> visibility_empty_scene, visibility_filled_scene;
        if (persistent_scene_probe && frames >= 24)
        {
            auto visibility_empty = std::make_unique<hs::StaticRenderSceneStorage>();
            visibility_empty->content_generation_id = generation.id;
            auto visibility_filled = std::make_unique<hs::StaticRenderSceneStorage>(*visibility_empty);
            auto static_occluder = second;
            static_occluder.stable_id = 0x56495301u;
            static_occluder.position = {0, 0, 3};
            static_occluder.scale = {6, 6, 6};
            auto static_meadow = second;
            static_meadow.stable_id = 0x56495302u;
            static_meadow.mesh = hs::RenderMesh::MeadowSurface;
            static_meadow.environment_variant = 0;
            static_meadow.position = {0, 0, 10};
            static_meadow.scale = {.04f, .04f, .04f};
            visibility_filled->instances = {static_occluder, static_meadow};
            visibility_empty_scene = hs::FreezeStaticRenderScene(std::move(visibility_empty));
            visibility_filled_scene = hs::FreezeStaticRenderScene(std::move(visibility_filled));
        }
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
        if (material_reload_terrain_archive)
            for (auto instance : scene->instances)
                if (instance.mesh != hs::RenderMesh::Ground)
                {
                    instance.stable_id += 0x4d41540000000000ull;
                    current.push_back(instance);
                }
        hs::RenderSnapshotPair pair;
        pair.has_current = true;
        const auto fixture_sun = hs::EnvironmentSun();
        pair.current.lights = std::span(&fixture_sun, 1);
        pair.current.static_scene = world_transport_probe || owned_view_probe || gameplay_scene || single_layer_water_probe || material_reload_terrain_archive ? world_scene
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
        std::shared_ptr<const hs::StaticRenderScene> waterless_scene;
        if ((translucency_diagnostics_probe && gameplay_scene) || single_layer_water_probe)
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
            if (single_layer_water_probe)
            {
                auto waterless = std::make_unique<hs::StaticRenderSceneStorage>(*world_scene);
                const auto index = static_cast<std::uint32_t>(water - world_scene->instances.begin());
                if (std::ranges::any_of(waterless->eligible_static_members, [index](const auto &member) { return member.instance_index == index; }))
                    throw std::runtime_error("Water-only fixture cannot remove an HLOD-eligible instance");
                waterless->instances.erase(waterless->instances.begin() + index);
                for (auto &member : waterless->eligible_static_members)
                    if (member.instance_index > index) --member.instance_index;
                waterless_scene = hs::FreezeStaticRenderScene(std::move(waterless));
            }
            receipt["translucency_diagnostics"]["water_camera"] = {{"water_instance_stable_id", water->stable_id},
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
        if (translucency_diagnostics_probe && gameplay_scene)
        {
            world_lights[0].stable_id = 0x485753554e000001ull;
            world_lights[0].direction = hs::kEnvironmentSunDirection;
            world_lights[0].quantity = 50000.0f; world_lights[0].color = {1.0f, .94f, .86f};
            world_lights[0].shadow_flags = hs::kLightShadowRequested;
            pair.current.lights = std::span(world_lights).first(1);
        }
        if (world_transport_probe || local_probe_revision_probe || single_layer_water_probe)
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
                        sample.metric_kind != hs::GpuFrameTimingKind::OutputDependencyInterval ||
                        !sample.critical_path_valid || !sample.cpu_recording_valid ||
                        sample.pass_timings_valid || sample.passes.size() != 2 ||
                        begin == sample.passes.end() || end == sample.passes.end() ||
                        begin->queue != hs::RenderQueue::Direct || end->queue != hs::RenderQueue::Direct ||
                        !begin->measured || !end->measured || !begin->name.empty() || !end->name.empty() ||
                        begin->cpu_record_valid || end->cpu_record_valid)
                        throw std::runtime_error("Output interval stream did not retain only Direct endpoints and frame-level CPU timing");
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
        std::array<std::uint64_t, 15> water_submissions{};
        std::optional<std::pair<std::uint32_t,std::uint32_t>> water_identity;
        std::map<std::uint64_t, hs::GpuPassTimings> provider_gpu_history;
        std::array<std::uint32_t, 3> probe_capture_frames{};
        std::array<std::uint64_t, 3> probe_last_capture_frame{};
        std::array<std::uint32_t, 3> probe_idle_frames{};
        std::uint32_t probe_light_change_frame{}, probe_scene_change_frame{};
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
            const auto phase = probe_scene_change_frame && observed_probe_frame >= probe_scene_change_frame ? 2u :
                probe_light_change_frame && observed_probe_frame >= probe_light_change_frame ? 1u : 0u;
            if (capture)
            {
                ++probe_capture_frames[phase];
                probe_last_capture_frame[phase] = observed_probe_frame;
                probe_idle_frames[phase] = 0;
            }
            else if (probe_last_capture_frame[phase] &&
                observed_probe_frame > probe_last_capture_frame[phase] &&
                renderer.GetSceneProbeBakeStatus().capture_complete)
                ++probe_idle_frames[phase];
            if (phase == 0) settled_probe_frames = probe_idle_frames[0];
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
            receipt["translucency_diagnostics"]["quality_transition"]["baseline_request"] = {
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
        std::optional<std::pair<std::uint64_t, std::uint64_t>> material_shadow_baseline, material_shadow_previous;
        std::optional<std::uint64_t> material_terrain_archive_baseline;
        bool material_reload_changes_terrain{};
        if (material_reload_probe)
        {
            const auto &baseline = generation.product_digests.at("render/material/terrain/parameters.rpkg");
            material_reload_changes_terrain = baseline != material_reload_generation.product_digests.at("render/material/terrain/parameters.rpkg");
            receipt["material_reload"] = {
                {"baseline_generation", generation.id},
                {"valid_generation", material_reload_generation.id},
                {"invalid_generation", material_reload_invalid_generation.id},
                {"initial_material_parameter_revision", initial_material_parameter_revision},
                {"initial_pipeline_revision", initial_pipeline_revision},
                {"captures", Json::object()},
                {"terrain_coverage", material_reload_terrain_archive ? "world_archive" : "archive_unavailable"},
                {"terrain_parameters_changed", material_reload_changes_terrain},
            };
        }
        constexpr std::array residency_texture_qualities{
            hs::GraphicsQuality::Ultra, hs::GraphicsQuality::Low, hs::GraphicsQuality::Ultra};
        std::array<std::uint64_t, residency_texture_qualities.size()> residency_quality_generations{};
        std::array<std::uint32_t, residency_texture_qualities.size()> residency_quality_frames{};
        std::array<std::uint32_t, residency_texture_qualities.size()> residency_quality_mips{};
        std::array<std::uint64_t, residency_texture_qualities.size()> residency_quality_publication_baselines{};
        std::array<bool, residency_texture_qualities.size()> residency_quality_applied{};
        std::array<bool, residency_texture_qualities.size()> residency_quality_settled{};
        std::size_t residency_quality_phase{};
        Json residency_quality_samples = Json::array();
        std::uint32_t rendered_frame_count{};
#if defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
        std::optional<TranslucencyDepthCapture> nested_optical_capture;
        std::optional<TranslucencyDepthCapture> reference_optical_capture;
#endif
        const auto residency_streaming_complete = [&] {
            if (!residency_streaming_probe ||
                residency_quality_phase != residency_texture_qualities.size() ||
                std::ranges::any_of(residency_quality_settled, [](bool settled) { return !settled; }))
                return false;
            const auto stats = renderer.ResidencyStats();
            const auto texture = renderer.GetTextureStreamingStatus(
                "render/texture/environment_terrain_gaussian.rpkg");
            return stats.budget_valid && stats.local_budget_bytes != 0 &&
                stats.tracked_live_bytes != 0 && stats.tracked_reclaimed_bytes != 0 &&
                stats.streaming_publications != 0 &&
                stats.streaming_uploads >= stats.streaming_publications &&
                stats.streaming_requests >= stats.streaming_uploads &&
                texture.registered && texture.publications != 0 && !texture.pending &&
                texture.publication_complete &&
                texture.actual_resident_mip == texture.effective_desired_mip &&
                texture.actual_resident_mip == texture.target_mip && texture.reclaims != 0 &&
                stats.geometry_streaming_requests != 0 &&
                stats.geometry_streaming_uploads == stats.geometry_streaming_requests &&
                stats.geometry_streaming_publications == stats.geometry_streaming_requests &&
                stats.geometry_streaming_reclaims >= stats.geometry_streaming_requests;
        };
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
            const auto request_residency_texture_phase = [&](std::size_t phase, std::uint32_t request_frame) {
                auto status = renderer.GetGraphicsSettingsStatus();
                status.requested.texture_quality = residency_texture_qualities[phase];
                residency_quality_generations[phase] = status.generation + 1u;
                residency_quality_frames[phase] = request_frame;
                const auto texture = renderer.GetTextureStreamingStatus(
                    "render/texture/environment_terrain_gaussian.rpkg");
                residency_quality_publication_baselines[phase] = texture.publications;
                Require(renderer.RequestGraphicsSettings({residency_quality_generations[phase],
                    hs::GraphicsSettingsRequestCause::User, status.requested}));
            };
            if (residency_streaming_probe) request_residency_texture_phase(0, 0);
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
                    receipt["translucency_diagnostics"]["quality_transition"]["request"] = {{"generation", hero_quality_generation},
                        {"succeeded", requested.Succeeded()}, {"error", requested.Succeeded() ? "" : std::string(requested.Message())},
                        {"observed_generation", observed.generation}, {"state", static_cast<unsigned>(observed.state)},
                        {"status_error", static_cast<unsigned>(observed.error)}};
                    Require(requested);
                }
                if (translucency_diagnostics_probe && gameplay_scene)
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
                    else if (frames >= 24 && frame >= 15)
                    {
                        const bool dynamic_filled = frame >= 18 && frame <= 20;
                        const bool static_filled = !dynamic_filled;
                        pair.current.static_scene = static_filled ? visibility_filled_scene : visibility_empty_scene;
                        current = dynamic_filled ? std::vector<hs::RenderInstance>{second} : std::vector<hs::RenderInstance>{};
                        world_lights[0] = hs::LightView{};
                        world_lights[0].stable_id = 1;
                        world_lights[0].direction = hs::kEnvironmentSunDirection;
                        world_lights[0].quantity = 50000.0f;
                        world_lights[0].color = {1, 1, 1};
                        world_lights[0].shadow_flags = dynamic_filled ? 0 : hs::kLightShadowRequested;
                        pair.current.lights = std::span(world_lights).first(1);
                    }
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
                            // Keep growth identities disjoint from Scene's 100..114 static owners.
                            added.stable_id = 0x4c4f440000000000ull + index;
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
                else if (surface_motion_resize_probe)
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
                else if (!world_transport_probe && !single_layer_water_probe)
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
                const auto probe_status = local_probe_revision_probe ? renderer.GetSceneProbeBakeStatus()
                    : hs::RendererSceneProbeBakeStatus{};
                if (local_probe_revision_probe && !probe_light_change_frame && probe_status.sources_ready &&
                    probe_status.capture_complete && probe_idle_frames[0])
                {
                    probe_light_change_frame = frame;
                    world_lights[0].direction.x += 0.2f;
                }
                if (local_probe_revision_probe && probe_light_change_frame && !probe_scene_change_frame &&
                    probe_status.sources_ready && probe_status.capture_complete && probe_idle_frames[1])
                {
                    probe_scene_change_frame = frame;
                    auto changed = std::make_unique<hs::StaticRenderSceneStorage>(*pair.current.static_scene);
                    changed->layout_abi = 0;
                    changed->eligible_layout_digest = {};
                    changed->eligible_static_members.clear();
                    changed->instances.resize(world_scene->instances.size());
                    changed->instances.back().position.x += 1.0f;
                    pair.current.static_scene = hs::FreezeStaticRenderScene(std::move(changed));
                    current.push_back(shadow_history_caster);
                }
                if (local_probe_revision_probe && probe_scene_change_frame && frame == probe_scene_change_frame + 15)
                    std::erase_if(current, [](const auto &instance) { return instance.stable_id == 0; });
                if (local_probe_revision_probe && probe_scene_change_frame && frame == probe_scene_change_frame + 16)
                    std::erase_if(current, [](const auto &instance) { return instance.stable_id == kShadowHistoryCasterId; });
                if (single_layer_water_probe)
                {
                    const bool mixed = frame >= 4 && frame <= 6;
                    const bool empty = frame >= 10 && frame <= 12;
                    pair.current.static_scene = empty ? waterless_scene : world_scene;
                    current.clear();
                    if (mixed)
                    {
                        hs::RenderInstance slime{};
                        slime.mesh = hs::RenderMesh::MonsterMelee; slime.stable_id = 0x5741544552534cull;
                        slime.position = {pair.current.camera.target.x, pair.current.camera.target.y, pair.current.camera.target.z};
                        slime.scale = {1, 1, 1};
                        hs::PopulateRenderInstanceIdentity(slime);
                        current.push_back(slime);
                    }
                }
                pair.current.instances = current;
                if (!grass_history_probe.empty() && (frame == 13 || frame == 14))
                    Require(renderer.Resize(config.width + (frame == 13 ? 64 : 0),
                                            config.height + (frame == 13 ? 32 : 0)));
                std::vector<hs::RenderInstance> preview_instances;
                std::vector<hs::AnimationPoseRef> preview_poses;
                if (slime_preview)
                {
                    for (std::uint32_t i = 0; i < 3; ++i)
                    {
                        hs::RenderInstance slime{};
                        slime.position = {(static_cast<float>(i) - 1) * 2.5f, 0, 0};
                        const float scale = i == 0 ? 1.25f : i == 1 ? 1.40625f : 1.5625f;
                        slime.scale = {scale, scale, scale}; slime.color_rgba = 0xffffffffu;
                        slime.mesh = static_cast<hs::RenderMesh>(10 + i);
                        slime.stable_id = 0x53504c4d00000000ull | (i + 1);
                        hs::PopulateRenderInstanceIdentity(slime);
                        hs::AnimationPoseRef pose{};
                        pose.instance_index = i; pose.normalized_time = .5f;
                        preview_poses.push_back(pose); preview_instances.push_back(slime);
                    }
                    hs::RenderInstance projectile{};
                    projectile.position = {1.25f, .45f, -1.5f}; projectile.scale = {.24f, .24f, .8f};
                    projectile.color_rgba = 0xff60d060u; projectile.mesh = hs::RenderMesh::EnemyProjectile;
                    projectile.stable_id = 0x53504c4d00100003ull;
                    hs::PopulateRenderInstanceIdentity(projectile);
                    preview_instances.push_back(projectile);
                }
                const bool optical_pair_nested_frame = translucency_optical_pair_probe && frame == 1;
                const bool optical_pair_reference_frame = translucency_optical_pair_probe && frame == 2;
                if (optical_pair_nested_frame || optical_pair_reference_frame)
                {
                    preview_instances.clear();
                    preview_poses.clear();
                    hs::RenderInstance actor{};
                    actor.mesh = optical_pair_nested_frame ? hs::RenderMesh::MonsterMelee : hs::RenderMesh::MonsterRanged;
                    actor.position = {};
                    actor.scale = {1, 1, 1};
                    actor.stable_id = 0x4f50544943414c01ull;
                    hs::PopulateRenderInstanceIdentity(actor);
                    hs::AnimationPoseRef pose{};
                    pose.instance_index = 0;
                    pose.clip = hs::CharacterAnimationClip::Idle;
                    pose.normalized_time = 0.5f;
                    preview_instances.push_back(actor);
                    preview_poses.push_back(pose);
                }
                if (environment_preview)
                {
                    preview_instances.clear(); preview_poses.clear();
                    const auto add = [&](hs::RenderMesh mesh, hs::Float3 position, hs::Float3 scale, std::uint32_t variant) {
                        hs::RenderInstance instance{}; instance.mesh = mesh; instance.position = position; instance.scale = scale;
                        instance.environment_variant = variant; instance.environment_seed = 0x4853454eu;
                        hs::PopulateRenderInstanceIdentity(instance); preview_instances.push_back(instance);
                    };
                    for (std::uint32_t i = 0; i < 3; ++i)
                    {
                        const float x = static_cast<float>(i) * 5 - 5;
                        add(hs::RenderMesh::TreeTrunk, {x,0,0}, {1,1,1}, i);
                        add(hs::RenderMesh::TreeCanopy, {x,0,0}, {1,1,1}, i);
                    }
                    for (std::uint32_t i = 0; i < 4; ++i)
                    {
                        const float x = static_cast<float>(i) * 3 - 4.5f;
                        add(hs::RenderMesh::Rock, {x,0,-5}, {.8f,.8f,.8f}, i);
                        add(hs::RenderMesh::Grass, {x,0,-8}, {1,1,1}, i);
                    }
                    for (int z = -5; z <= 2; ++z)
                        for (int x = -5; x <= 5; ++x)
                            add(hs::RenderMesh::Ground, {static_cast<float>(x)*2.5f,-.055f,static_cast<float>(z)*2.5f}, {2.5f,.1f,2.5f}, 0);
                }

                auto preview_camera = pair.current.camera;
                if (slime_preview || environment_preview)
                {
                    if (environment_preview) { preview_camera.target = {0,1,0}; preview_camera.yaw_degrees = 0; preview_camera.pitch_degrees = 22; preview_camera.distance = 22; }
                    if (slime_preview) { preview_camera.target = {0,1,0}; preview_camera.distance = 10; }
                }
                if (residency_streaming_probe)
                {
                    // Keep the terrain close enough to request fine mips even at the minimum test resolution.
                    preview_camera.target = {0, 0, 0};
                    preview_camera.pitch_degrees = 60;
                    preview_camera.distance = 0.8f;
                }
                auto main_view = renderer.MainViewInput(preview_camera);
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
                    const auto local_count = kLightListLocalCounts[frame - 1];
                    const auto directional_count = kLightListDirectionalCounts[frame - 1];
                    light_list_lights.assign(local_count + directional_count, {});
                    for (std::uint32_t index = 0; index < light_list_lights.size(); ++index)
                    {
                        auto &light = light_list_lights[index];
                        light.stable_id = 0x4c4c000000000000ull + light_list_lights.size() - index;
                        light.type = index < directional_count ? hs::LightType::Directional
                            : static_cast<hs::LightType>(1u + index % 4u);
                        // Frame 5 requests shadows for a radiating sun whose
                        // direction differs from the authored baked sun.
                        if (frame == 5 && light.type == hs::LightType::Directional)
                            light.shadow_flags = hs::kLightShadowRequested;
                        light.unit = light.type == hs::LightType::Directional ? hs::LightUnit::Lux
                            : index % 4u >= 2u ? hs::LightUnit::Luminance : hs::LightUnit::Lumen;
                        light.position = {0, 3, 0}; light.direction = {0, -1, 0};
                        light.color = {.7f, .8f, 1};
                        light.quantity = local_count ? 100.0f / local_count : 0.0f;
                        light.range = 10000; light.emitter_radius = .15f;
                        light.inner_cone_radians = .2f; light.outer_cone_radians = .6f;
                        light.shape = {2, 2}; light.area_tangent = {1, 0, 0};
                        if (frame == 7 && light.type == hs::LightType::Point)
                        {
                            light.unit = hs::LightUnit::RadiantIntensity;
                            light.quantity = static_cast<float>(double(light.quantity) /
                                hs::kRgbReferenceLumensPerWatt / (4.0 * std::acos(-1.0)));
                        }
                    }
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
                auto preview_pair = pair;
                if (slime_preview || environment_preview)
                {
                    preview_pair.current.instances = preview_instances;
                    preview_pair.current.poses = preview_poses;
                    preview_pair.current.static_scene.reset();
                    if (preview_pair.has_previous)
                    {
                        preview_pair.previous.instances = preview_instances;
                        preview_pair.previous.poses = preview_poses;
                        preview_pair.previous.static_scene.reset();
                    }
                }
                if (slime_preview && !environment_preview && !optical_pair_nested_frame && !optical_pair_reference_frame &&
                    (preview_pair.current.instances.size() != 4 || preview_pair.current.poses.size() != 3 ||
                     preview_pair.current.static_scene || preview_pair.current.instances[2].stable_id == 0 ||
                     preview_pair.current.poses[2].instance_index != 2))
                    throw std::runtime_error("slime preview package lost its scene or animation identity");
                if ((optical_pair_nested_frame || optical_pair_reference_frame) &&
                    (preview_pair.current.instances.size() != 1 || preview_pair.current.poses.size() != 1 ||
                     preview_pair.current.static_scene || preview_pair.current.instances.front().stable_id != 0x4f50544943414c01ull))
                    throw std::runtime_error("certified optical-pair preview lost its single-instance identity");
#if defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
                if (translucency_optical_pair_probe)
                {
                    // Both captures use the same camera and fixed Idle sample; only the
                    // selected certified geometry package changes between frames.
                    main_view.camera.position = {0, 1.6, -3.0};
                    main_view.camera.target = {0, 0.45, 0};
                    main_view.camera.up = {0, 1, 0};
                    main_view.view_origin = main_view.camera.position;
                    main_view.camera_cut = true;
                }
#endif
                const hs::RenderPackage render_input{
                    .views = std::span(&main_view, 1), .snapshots = preview_pair,
                    .vfx = {.flashes = oit_flashes, .entity_surfaces = entity_surfaces,
                        .world_batch = {pair.current.header.session_id, pair.current.header.tick, frame}},
                    .devtools = {
                        .delta_seconds = entity_surface_probe ? std::optional<float>{0.0f}
                        : local_probe_revision_probe || translucency_diagnostics_probe || translucency_optical_pair_probe ? std::optional<float>{1.0f / 60.0f}
                        : !vfx_composition_probe.empty()
                        ? std::optional<float>{vfx_composition_probe == "auto" || vfx_composition_probe == "switch"
                            ? 1.0f / 60.0f : 0.0f}
                        : persistent_scene_probe || material_reload_probe
                        ? std::optional<float>{0.0f} : std::nullopt,
                        .wait_for_streaming = entity_surface_probe || world_transport_probe || local_probe_revision_probe || single_layer_water_probe ||
                            !vfx_composition_probe.empty()},
                    .display_sample = hs::RenderDisplaySample{std::chrono::steady_clock::now(), 1.0f}};
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
                rendered_frame_count = frame;
#if defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
                if (optical_pair_nested_frame || optical_pair_reference_frame)
                {
                    const auto filename = optical_pair_nested_frame ? "translucency-optical-nested.json" :
                        "translucency-optical-outer-only.json";
                    const auto path = artifacts / filename;
                    Require(renderer.Capture({.view_id = hs::kMainRenderViewId, .path = path,
                        .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    auto capture = ReadTranslucencyDepthCapture(path);
                    if (optical_pair_nested_frame) nested_optical_capture = std::move(capture);
                    else reference_optical_capture = std::move(capture);
                    receipt["translucency_optical_pair"][optical_pair_nested_frame ? "nested_capture" : "outer_only_capture"] = filename;
                }
#endif
                if (world_transport_probe && frame == 1)
                {
                    const auto path = artifacts / "world-transport-surface.json";
                    Require(renderer.Capture({.path = path, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    const auto diagnostic = Json::parse(std::ifstream(path));
                    const bool pages = diagnostic.at("terrain_surface_pages").get<bool>();
                    receipt["world_transport"]["terrain_surface_pages"] = pages;
                    if (!pages) throw std::runtime_error("Packed world transport did not admit terrain surface pages");
                }
                if (single_layer_water_probe)
                {
                    if (frame_result.views.size() != 1) throw std::runtime_error("water probe requires one accepted view");
                    water_submissions[frame - 1] = frame_result.views[0].submission_id;
                    if (frame == 1 || frame == 3 || frame == 6 || frame == 9 || frame == 12 || frame == 15)
                    {
                        const auto name = std::format("single-layer-water-{:02}.json", frame);
                        Require(renderer.Capture({.path = artifacts / name, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                        const bool mixed = frame == 6, empty = frame == 12;
                        const auto observed = ValidateWaterSurface(artifacts / name, empty);
                        if (!mixed && !empty && (observed.identities.size() != 1 || observed.nonzero_unpeeled || !observed.changed_color))
                            throw std::runtime_error("water-only frame has foreign identity or unpeeled fragments");
                        if (frame == 1)
                        {
                            water_identity = *observed.identities.begin();
                            if (observed.history_pixels != 0) throw std::runtime_error("first water frame has unexpected history");
                        }
                        if (frame == 3 && !observed.history_pixels)
                            throw std::runtime_error("settled water has no valid history");
                        if (mixed && (!water_identity || !observed.identities.contains(*water_identity) || observed.identities.size() < 2))
                            throw std::runtime_error("mixed water and slime did not both reach transparent output");
                        receipt["single_layer_water"]["capture_" + std::to_string(frame)] = {
                            {"file", name}, {"pixels", observed.pixels}, {"historical_pixels", observed.history_pixels},
                            {"identities", observed.identities.size()}, {"nonzero_unpeeled", observed.nonzero_unpeeled},
                            {"changed_hdr_pixels", observed.changed_color}};
                    }
                }
                if (residency_streaming_probe && residency_quality_phase < residency_texture_qualities.size())
                {
                    const auto settings = renderer.GetGraphicsSettingsStatus();
                    const auto texture = renderer.GetTextureStreamingStatus(
                        "render/texture/environment_terrain_gaussian.rpkg");
                    const auto phase = residency_quality_phase;
                    const bool applied = settings.actual_applied_available &&
                        settings.generation == residency_quality_generations[phase] &&
                        settings.error == hs::ErrorCode::None &&
                        (settings.state == hs::SettingsApplyState::Applied ||
                         settings.state == hs::SettingsApplyState::Confirmed) &&
                        settings.actual_applied.texture_quality == residency_texture_qualities[phase];
                    residency_quality_applied[phase] = residency_quality_applied[phase] || applied;
                    const bool settled = applied && texture.registered && texture.publication_complete &&
                        !texture.pending && texture.publications > residency_quality_publication_baselines[phase] &&
                        texture.actual_resident_mip == texture.effective_desired_mip &&
                        texture.actual_resident_mip == texture.target_mip &&
                        (phase == 1
                            ? texture.actual_resident_mip >= 2 && texture.actual_resident_mip > residency_quality_mips[0]
                            : texture.actual_resident_mip <= 1 &&
                                (phase != 2 || texture.actual_resident_mip < residency_quality_mips[1]));
                    residency_quality_settled[phase] = settled;
                    residency_quality_samples.push_back({
                        {"frame", frame}, {"phase", phase},
                        {"request_frame", residency_quality_frames[phase]},
                        {"request_generation", residency_quality_generations[phase]},
                        {"settings_generation", settings.generation},
                        {"settings_state", static_cast<unsigned>(settings.state)},
                        {"actual_quality_available", settings.actual_applied_available},
                        {"actual_texture_quality", settings.actual_applied_available
                            ? Json(static_cast<unsigned>(settings.actual_applied.texture_quality)) : Json(nullptr)},
                        {"phase_applied", applied}, {"phase_settled", settled},
                        {"required_mip_direction_observed", settled}, {"registered", texture.registered},
                        {"pending", texture.pending}, {"resident_mip", texture.actual_resident_mip},
                        {"effective_desired_mip", texture.effective_desired_mip},
                        {"target_mip", texture.target_mip}, {"revision", texture.revision},
                        {"requests", texture.requests}, {"uploads", texture.uploads},
                        {"publications", texture.publications}, {"reclaims", texture.reclaims}});
                    if (settled)
                    {
                        residency_quality_mips[phase] = texture.actual_resident_mip;
                        residency_quality_phase = phase + 1u;
                        if (residency_quality_phase < residency_texture_qualities.size())
                        {
                            request_residency_texture_phase(residency_quality_phase, frame + 1u);
                        }
                    }
                }
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
                    receipt["translucency_diagnostics"]["quality_transition"]["frames"].push_back({{"fixture_frame", frame},
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
                if (translucency_diagnostics_probe && gameplay_scene)
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
                if (!timing_stream_probe && !(translucency_diagnostics_probe && gameplay_scene))
                {
                    // Keep the completed stream for world transport's one-time passes.
                    if (world_transport_probe || single_layer_water_probe)
                        collect_timing_stream(false);
                    else
                    {
                        std::vector<hs::GpuPassTimings> unused_timings;
                        Require(renderer.ConsumeCompletedGpuPassTimings(unused_timings));
                    }
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
                    if (targets.size() != 2 ||
                        targets.at(1).at("present").get<bool>() != (frame % 2 == 0) ||
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
                if (persistent_scene_probe && frames >= 24 && frame >= 15 &&
                    config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
                {
                    const auto path = artifacts / std::format("visibility-commands-{:02}.json", frame);
                    Require(renderer.Capture({.path = path, .kind = hs::CaptureKind::GeometryDiagnostics}));
                    ValidateVisibilityCommandCapture(path, pair.current.static_scene->instances, current, frame,
                        receipt["visibility_command_cases"]);
                }
                if (persistent_scene_probe && frames >= 24 && frame == 14 &&
                    config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
                {
                    const auto path = artifacts / "hzb-empty-slot-reuse.json";
                    Require(renderer.Capture({.path = path, .kind = hs::CaptureKind::VisibilityDiagnostics}));
                    const auto diagnostic = Json::parse(std::ifstream(path));
                    for (const auto &catalog : diagnostic.at("cluster_commands"))
                        for (const auto &view : catalog.at("views"))
                            if (view.at("view").get<std::uint32_t>() == 4 &&
                                (view.at("published_count").get<std::uint64_t>() != 0 ||
                                 view.at("nonzero_count").get<std::uint64_t>() != 0 ||
                                 view.at("nonzero_outside_published_prefix").get<std::uint64_t>() != 0))
                                throw std::runtime_error("Cluster Retest count survived frame 14 slot reuse");
                    receipt["hzb_cluster_retest"]["empty_after_slot_reuse"] = true;
                }
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
                    std::uint64_t committed{}, published{}, live{}, vertices{}, mesh_groups{};
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
                            const auto draw_work = view.at("draw_vertex_count_sum").get<std::uint64_t>();
                            const auto mesh_work = view.at("mesh_group_count_sum").get<std::uint64_t>();
                            const bool mesh = catalog.at("geometry_frontend_flags").get<std::uint32_t>() != 0u;
                            if ((mesh ? draw_work : mesh_work) != 0 ||
                                (view.at("nonzero_count").get<std::uint64_t>() != 0 && !(mesh ? mesh_work : draw_work)))
                                throw std::runtime_error("HZB Cluster Retest workload does not match its frontend");
                            published += count;
                            live += view.at("nonzero_count").get<std::uint64_t>();
                            vertices += draw_work;
                            mesh_groups += mesh_work;
                        }
                    }
                    if (!committed || !live || !(vertices || mesh_groups))
                        throw std::runtime_error("HZB reveal did not exercise live Cluster Retest draws");
                    receipt["hzb_cluster_retest"] = {
                        {"frame", frame}, {"diagnostic", path.filename().string()},
                        {"committed", committed}, {"published", published},
                        {"nonzero", live}, {"draw_vertex_count_sum", vertices},
                        {"mesh_group_count_sum", mesh_groups},
                        {"nonzero_outside_published_prefix", 0},
                    };
                }
                if (surface_motion_resize_probe && frame == 3)
                {
                    failure_code = "HS_RENDER_SURFACE_DIAGNOSTIC_FAILED";
                    Require(renderer.Capture({.path = artifacts / "surface-motion.json", .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    ValidateSurfaceDiagnostic(artifacts / "surface-motion.json", config.width,
                                              config.height, true, "motion", receipt);
                }
                const bool capture_grass_diagnostics = grass_history_probe == "F"
                    ? (frame == 4 || frame == 5 || frame == frames)
                    : !grass_history_probe.empty() &&
                        (frame == 1 || frame == 5 || frame == 6 || frame > frames - 6);
                if (capture_grass_diagnostics)
                {
                    const auto name = std::format("grass-{:03}.json", frame);
                    Require(renderer.Capture({.path = artifacts / name, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    const auto diagnostic = Json::parse(std::ifstream(artifacts / name));
                    const auto ao_visibility = grass_history_probe == "F"
                        ? ValidateAoVisibilityCapture(artifacts / name,
                            diagnostic.at("width").get<std::uint32_t>(),
                            diagnostic.at("height").get<std::uint32_t>())
                        : Json{};
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
                    Json frame_capture{{"frame", frame},
                        {"capture", name}, {"grass", grass},
                        {"gpu_frame", gpu.completed_frame_id}, {"gpu_passes", passes}};
                    if (grass_history_probe == "F") frame_capture["ao_visibility"] = ao_visibility;
                    receipt["grass_history"]["frames"].push_back(std::move(frame_capture));
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
                    auto auxiliary = preview;
                    auxiliary.id = hs::ViewId{0x47524154u};
                    auxiliary.output_extent = auxiliary.render_capacity = auxiliary.render_active = {320, 180};
                    auxiliary.camera.position.z -= 2.0;
                    auxiliary.view_origin = auxiliary.camera.position;
                    std::array preview_views{preview, auxiliary};
                    if (frame == 9) std::swap(preview_views[0], preview_views[1]);
                    preview_pair.has_current = true;
                    preview_pair.current.header.clock.mode = hs::RenderClockMode::Scripted;
                    preview_pair.current.header.simulation_time =
                        std::chrono::nanoseconds(10000000000ll + frame * 1000000000ll / 30);
                    const hs::RenderPackage preview_input{.views = preview_views, .snapshots = preview_pair,
                        .vfx = {.world_batch = {preview_pair.current.header.session_id, preview_pair.current.header.tick}},
                        .display_sample = hs::RenderDisplaySample{std::chrono::steady_clock::now(), 1.0f}};
                    hs::RendererFrameResult preview_result;
                    Require(renderer.Render(preview_input, preview_result));
                    if (preview_result.views.size() != preview_views.size())
                        throw std::runtime_error("shared scene package lost a preview view");
                    bool distinct_slot_and_output = false;
                    for (std::size_t index = 0; index < preview_views.size(); ++index)
                    {
                        if (preview_result.views[index].identity.view_id != preview_views[index].id ||
                            preview_result.views[index].rendered_view.output_extent != preview_views[index].output_extent)
                            throw std::runtime_error("shared scene package mixed preview identity or extent");
                        const auto &view = preview_result.views[index];
                        distinct_slot_and_output |= (view.submission_id - 1) % 3 != (view.rendered_frame - 1) % 3;
                    }
                    if (!distinct_slot_and_output)
                        throw std::runtime_error("preview fixture did not separate output index and submission slot");
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
                    const auto main_timing = renderer.LastGpuPassTimings();
                    if (frame_result.views.size() != 1 || !main_timing.valid ||
                        main_timing.view_id != main_view.id.value ||
                        main_timing.completed_frame_id != frame_result.views.front().submission_id)
                        throw std::runtime_error("preview submissions changed the main view timing owner");
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
                    if (frame != 5)
                    {
                        const auto depth = artifacts / std::format("material-shadow-{:02}.json", frame);
                        Require(renderer.Capture({.path = depth, .kind = hs::CaptureKind::SurfaceDiagnostics}));
                        if (material_reload_terrain_archive)
                        {
                            const auto diagnostic = Json::parse(std::ifstream(depth));
                            const bool active = diagnostic.at("terrain_surface_pages").get<bool>();
                            const auto reads = diagnostic.at("terrain_archive_reads").get<std::uint64_t>();
                            if (!material_terrain_archive_baseline)
                            {
                                if (!active || !reads) throw std::runtime_error("material reload world did not admit terrain pages");
                                material_terrain_archive_baseline = reads;
                            }
                            const bool changed = material_reload_changes_terrain && frame == 4;
                            const auto expected_reads = *material_terrain_archive_baseline +
                                (material_reload_changes_terrain && frame == 6 ? 1 : 0);
                            if (active == changed || reads != expected_reads)
                                throw std::runtime_error("terrain page admission or archive reads changed unexpectedly during material reload");
                            receipt["material_reload"]["terrain_observations"].push_back({
                                {"frame", frame}, {"active", active}, {"archive_reads", reads}});
                        }
                        const auto shadow = ValidateMaterialReloadShadowDepth(depth, receipt["material_reload"]["shadow_depth_readbacks"]);
                        if (!material_shadow_baseline)
                        {
                            if (!shadow.first) throw std::runtime_error("material reload shadow membership was not built");
                            material_shadow_baseline = shadow;
                        }
                        else
                        {
                            if (shadow.first != material_shadow_baseline->first)
                                throw std::runtime_error("material reload rebuilt unchanged static shadow membership");
                            if ((frame == 4 || frame == 6) &&
                                (!material_shadow_previous || shadow.second <= material_shadow_previous->second))
                                throw std::runtime_error("material reload did not rebind static shadow records");
                        }
                        material_shadow_previous = shadow;
                    }
                }
                if (grid_floor_probe && capture_grid_frames &&
                    (frame == 1 || frame == 9 || frame == 17 || frame == 25))
                {
                    const auto name = std::format("grid-{0:02}.png", frame);
                    Require(renderer.Capture({.path = artifacts / name}));
                    receipt["grid_floor"]["captures"][std::to_string(frame)] = name;
                }
                if (surface_motion_resize_probe && frame == 4)
                {
                    // The fixture changes only controls it actually owns. The
                    // remaining fields retain GraphicsSettings defaults rather
                    // than being fabricated from unrelated renderer state.
                    hs::GraphicsSettings graphics;
                    graphics.vsync = config.vsync;
                    graphics.bloom = config.bloom;
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
                if (surface_motion_resize_probe && frame == 5)
                    Require(renderer.Resize(960, 540));
                if (surface_motion_resize_probe && frame == 7)
                {
                    failure_code = "HS_RENDER_SURFACE_DIAGNOSTIC_FAILED";
                    Require(renderer.Capture({.path = artifacts / "surface-resized.json", .kind = hs::CaptureKind::SurfaceDiagnostics}));
                    ValidateSurfaceDiagnostic(artifacts / "surface-resized.json", 720, 405,
                                              true, "resized", receipt);
                }
                previous = current;
                // Stop once every texture quality transition and every demanded
                // geometry request has published and reclaimed its old backing.
                // The larger frame count remains only the failure bound.
                if (residency_streaming_complete() ||
                    (world_transport_probe && frame >= 7 &&
                        renderer.GetSceneProbeBakeStatus().capture_complete)) break;
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
                {"mode", timing_stream_critical_probe ? "OutputDependencyInterval" : "Full"},
                {"output_dependency_endpoints_only", timing_stream_critical_probe},
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
            if (persistent_scene_probe && frames >= 14 && frames < 24)
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
        const auto post_render_receipt_start = Clock::now();
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
                {"rendered_frames", rendered_frame_count},
                {"frame_bound", frames},
                {"terminated_after_completion", rendered_frame_count < frames},
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
                {"texture_quality_phases", {
                    {"requested_qualities", {static_cast<unsigned>(residency_texture_qualities[0]),
                        static_cast<unsigned>(residency_texture_qualities[1]),
                        static_cast<unsigned>(residency_texture_qualities[2])}},
                    {"request_frames", residency_quality_frames},
                    {"applied", residency_quality_applied},
                    {"settled", residency_quality_settled},
                    {"settled_mips", residency_quality_mips},
                    {"samples", std::move(residency_quality_samples)}}},
            };
            if (std::ranges::any_of(residency_quality_settled, [](bool settled) { return !settled; }))
                throw std::runtime_error("Texture streaming did not publish a settled fine→coarse→fine mip sequence within the probe's frame bound.");
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
                    {"light_change_frame", probe_light_change_frame},
                    {"static_removal_and_dynamic_transfer_frame", probe_scene_change_frame},
                    {"anonymous_dynamic_removal_frame", probe_scene_change_frame ? probe_scene_change_frame + 15 : 0},
                    {"named_dynamic_removal_frame", probe_scene_change_frame ? probe_scene_change_frame + 16 : 0}}}};
            if (settled_probe_frames == 0 || !probe_scene_change_frame || rendered_frame_count < probe_scene_change_frame + 16 ||
                !renderer.GetSceneProbeBakeStatus().capture_complete || !probe_idle_frames[2] ||
                std::ranges::any_of(probe_capture_frames, [](auto count) { return count == 0; }))
                throw std::runtime_error("Local probes must settle, then recapture changed lights and a replacement static scene.");
            if (frame_result.static_scene_upload_count != 2)
                throw std::runtime_error("Shadow history fixture must upload exactly the initial and replacement static scenes.");
            if (renderer.LastParticleCount() != 0)
                throw std::runtime_error("Authored WaveTimer/pickup/bow owners leaked into legacy GPU particles.");
        }
        if (single_layer_water_probe)
        {
            collect_timing_stream(true);
            for (std::size_t i = 0; i < water_submissions.size(); ++i)
            {
                const auto found = timing_stream_history.find(water_submissions[i]);
                if (found == timing_stream_history.end()) throw std::runtime_error("water probe lost submitted GPU pass timings");
                const auto &sample = found->second;
                const auto measured = [&](std::string_view name) {
                    return std::ranges::any_of(sample.passes, [&](const auto &pass) { return pass.name == name && pass.measured; });
                };
                const bool mixed = i >= 3 && i < 6, empty = i >= 9 && i < 12;
                const bool water = measured("Water.CopyBackground") && measured("Water.SingleLayer");
                const bool peel = measured("Translucency.PeelFront");
                const bool exit = measured("Translucency.GatherExitDepth");
                const bool tail = measured("Translucency.AccumulateUnpeeled");
                if (!measured("HdrPostNearestGuides") || !measured("HdrPostMotionBlur") ||
                    !measured("HdrPostDepthOfField"))
                    throw std::runtime_error("water probe requires active HDR geometry post passes");
                if (mixed ? (water || !peel || !exit || !tail) : empty ? (water || peel || exit || tail)
                    : (!water || peel || exit || tail))
                    throw std::runtime_error("water-only/mixed/empty pass transition mismatch");
                receipt["single_layer_water"]["frames"].push_back({{"frame",i+1}, {"submission",water_submissions[i]},
                    {"state",mixed ? "mixed" : empty ? "empty" : "water"}, {"water",water}, {"peel",peel}, {"exit",exit}, {"tail",tail}});
            }
        }
        if (world_transport_probe)
        {
            failure_code = "HS_RENDER_WORLD_TRANSPORT_PASSES_FAILED";
            if (!timing_stream_probe) collect_timing_stream(true);
            for (const auto &[frame_id, sample] : timing_stream_history)
                world_gpu_history.insert_or_assign(frame_id, sample);
            const auto observed = WorldTransportReceipt(world_gpu_history,
                renderer.GetSceneProbeBakeStatus().capture_complete);
            receipt["world_transport"].update(observed);
            receipt["world_transport"]["immutable_scene_frames"] = rendered_frame_count;
            receipt["world_transport"]["frame_bound"] = frames;
            if (!observed["local_probe"]["sample_ready"].get<bool>())
            {
                Json capture_textures = Json::array();
                for (const auto &[logical_id, path] : generation.product_paths)
                {
                    static_cast<void>(path);
                    if (!logical_id.starts_with("render/texture/environment_")) continue;
                    const auto status = renderer.GetTextureStreamingStatus(logical_id);
                    if (!status.registered) continue;
                    capture_textures.push_back({{"logical_id", logical_id},
                        {"pending", status.pending}, {"publication_complete", status.publication_complete},
                        {"requests", status.requests}, {"uploads", status.uploads},
                        {"publications", status.publications}, {"actual_resident_mip", status.actual_resident_mip},
                        {"effective_desired_mip", status.effective_desired_mip},
                        {"target_mip", status.target_mip}});
                }
                receipt["world_transport"]["capture_texture_streaming"] = std::move(capture_textures);
            }
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
                    "world transport did not complete volumetrics, SSR, baked local-probe readiness without runtime capture, and accepted-layout HLOD");
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
#if defined(HS_RENDER_TEST_OPTICAL_GEOMETRY_FIXTURE)
        if (optical_pair_fixture)
        {
            if (!nested_optical_capture || !reference_optical_capture)
                throw std::runtime_error("certified nested-shell GPU capture pair is incomplete");
            const auto paired_pixels = ValidateNestedOpticalPair(*nested_optical_capture, *reference_optical_capture,
                optical_pair_fixture->outer_component, optical_pair_fixture->inner_component);
            receipt["translucency_optical_pair"]["nested_shell_oracle"] = {
                {"paired_pixels", paired_pixels}, {"same_instance_key", true},
                {"outer_exit_matches_outer_only_reference", true},
                {"outer_exit_precedes_inner_exit", true}};
        }
#endif
        if (translucency_diagnostics_probe)
        {
            failure_code = "HS_RENDER_TRANSLUCENCY_DIAGNOSTICS_PROBE_FAILED";
            std::vector<hs::RendererTranslucencyDiagnosticsSample> hero_samples;
            const auto drained = renderer.DrainTranslucencyDiagnostics(hero_samples);
            bool shadow_valid = true;
            if (gameplay_scene)
            {
                collect_timing_stream(true);
                shadow_valid = ValidateHeroWaterShadowProbe(receipt["hero_water_shadow"], timing_stream_history);
            }
            ValidateTranslucencyDiagnosticsProbe(hero_samples, renderer.TranslucencyDiagnosticsStatus(), config.translucency_diagnostics_mode, frames, drained, receipt, gameplay_scene,
                hero_layer_probe, hero_quality_valid);
            if (!shadow_valid) throw std::runtime_error("Hero water shadow policy/producer invariant failed; raw frame receipts retained");
        }
        receipt["pipeline_cache"] = PipelineCacheReceipt(renderer.GetPipelineCacheStats());
        timings["post_render_receipt_validation"] = StageElapsedMilliseconds(post_render_receipt_start);
        LogStageElapsed("post_render_receipt_validation", timings["post_render_receipt_validation"]);
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
        {
            const auto rows = DynamicUploadRows(artifacts / "dynamic_instance_upload.csv");
            ValidateMonsterSkinCacheProbe(rows, frames, receipt);
        }
        if (config.validation && dynamic_lod_skin_growth_probe)
        {
            const auto rows = DynamicUploadRows(artifacts / "dynamic_instance_upload.csv");
            ValidateDynamicLodSkinGrowthProbe(rows, frames, receipt);
        }
        const auto validation_errors = renderer.ValidationErrorCount();
        const auto validation_status = ValidationStatusReceipt(renderer.ValidationStatus());
        receipt["validation_errors"] = validation_errors;
        receipt["validation_status"] = validation_status;
        receipt["frames"] = residency_streaming_probe || world_transport_probe ? rendered_frame_count : frames;
        if (static_shadow_bake_probe) receipt["static_shadow_bake"]["subsequent_render_frames"] = frames;
        receipt["static_scene_upload_count"] = frame_result.static_scene_upload_count;
        receipt["enhanced_barriers"] = renderer.UsesEnhancedBarriers();
        receipt["timings_ms"] = std::move(timings);
        receipt["execution_valid"] = validation_errors == 0;
        receipt["error_code"] = validation_errors == 0
            ? "" : "HS_RENDER_VALIDATION_FAILED";
        exit_code = validation_errors == 0 ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        receipt["error_code"] = failure_code;
        receipt["error"] = error.what();
    }
    if (!temporary_pack.empty())
    {
        std::error_code error;
        std::filesystem::remove(temporary_pack, error);
        if (error)
            std::cerr << "Could not remove renderer fixture pack: " << error.message() << '\n';
    }
    return write_receipt();
}
