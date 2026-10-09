#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/core/graphics_settings_request.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/presentation/environment_layout.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/renderer/render_frame_input.hpp>
#include "view_fixture.hpp"
#include <hs/jobs/task_system.hpp>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <span>
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
        {"ray_tracing_enabled", modes.ray_tracing_enabled},
        {"ao_mode", std::string(hs::GraphicsEnumToken(modes.ao_mode))},
        {"gi_mode", std::string(hs::GraphicsEnumToken(modes.gi_mode))},
        {"reflection_mode", std::string(hs::GraphicsEnumToken(modes.reflection_mode))},
        {"shadow_mode", std::string(hs::GraphicsEnumToken(modes.shadow_mode))},
    };
}

Json LightingStatusReceipt(const hs::RendererLightingStatus &status)
{
    const auto denoiser = [](const hs::RendererDenoiserStatus &value) {
        return Json{{"requested_provider", value.requested_provider.value}, {"active_method", value.active_method},
                    {"availability", value.availability}, {"reason", value.reason}};
    };
    return {
        {"requested", LightingModesReceipt(status.requested)},
        {"active", LightingModesReceipt(status.active)},
        {"fallback", {
            {"ao", status.ao_fallback_reason},
            {"gi", status.gi_fallback_reason},
            {"reflection", status.reflection_fallback_reason},
            {"shadow", status.shadow_fallback_reason},
        }},
        {"hardware", {
            {"ray_scene_supported", status.ray_scene_supported},
            {"inline_ray_queries_supported", status.inline_ray_queries_supported},
        }},
        {"ray_scene_allocated_bytes", status.ray_scene_allocated_bytes},
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
        {"denoisers", {{"ao", denoiser(status.ao_denoiser)}, {"radiance", denoiser(status.radiance_denoiser)}}},
        {"local_shadows", {{"enabled", status.local_shadows.enabled},
            {"requested_lights", status.local_shadows.requested_lights}, {"selected_lights", status.local_shadows.selected_lights},
            {"unsupported_lights", status.local_shadows.unsupported_lights}, {"budget_lights", status.local_shadows.budget_lights},
            {"overflow_lights", status.local_shadows.overflow_lights}, {"allocated_bytes", status.local_shadows.allocated_bytes},
            {"reason", status.local_shadows.reason}}},
        {"ray_shadows", {{"enabled", status.ray_shadows.enabled}, {"native_sigma", status.ray_shadows.native_sigma},
            {"requested_lights", status.ray_shadows.requested_lights}, {"selected_lights", status.ray_shadows.selected_lights},
            {"unsupported_lights", status.ray_shadows.unsupported_lights}, {"budget_lights", status.ray_shadows.budget_lights},
            {"rays_per_pixel", status.ray_shadows.rays_per_pixel}, {"allocated_bytes", status.ray_shadows.allocated_bytes},
            {"reason", status.ray_shadows.reason}}},
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
    auto scene = std::make_shared<hs::StaticRenderScene>();
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
    return scene;
}

std::shared_ptr<const hs::StaticRenderScene> WorldTransportScene(
    const hs::SelectedGeneration &generation)
{
    std::filesystem::path rules_path;
    Require(generation.Require("game-data/simulation-rules", rules_path));
    hs::SimulationRules rules;
    Require(hs::LoadSimulationRules(rules_path, rules));
    if (rules.arena_obstacle_count > rules.arena_obstacles.size())
        throw std::runtime_error("simulation rules contain an invalid obstacle count");
    auto layout = hs::BuildEnvironmentLayout(rules.arena_half_extent, rules.arena_boundary,
        std::span(rules.arena_obstacles).first(rules.arena_obstacle_count), 0);
    if (layout.eligible_static_members.empty())
        throw std::runtime_error("production arena has no HLOD eligible static members");
    auto scene = std::make_shared<hs::StaticRenderScene>();
    scene->content_generation_id = generation.id;
    scene->scene_asset_id = hs::MakeAssetId("render/hlod/arena.shl1");
    scene->layout_abi = layout.abi;
    scene->eligible_layout_digest = layout.eligible_digest;
    for (const auto &member : layout.eligible_static_members)
        scene->eligible_static_members.push_back(
            {member.stable_id, member.object_id, member.instance_index});
    scene->instances = std::move(layout.instances);
    return scene;
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
    constexpr std::array names{"texture", "geometry", "skin", "ray_tracing", "shadow", "gi",
        "history", "transient", "sdk", "staging"};
    Json categories = Json::object();
    for (std::size_t index = 0; index < names.size(); ++index)
        categories[names[index]] = {{"live_bytes", memory.live_bytes[index]},
            {"retiring_bytes", memory.retiring_bytes[index]}};
    return {
        {"dxgi_local", {{"available", memory.budget_available},
            {"usage_bytes", memory.budget_available ? Json(memory.local_usage_bytes) : Json(nullptr)},
            {"budget_bytes", memory.budget_available ? Json(memory.local_budget_bytes) : Json(nullptr)}}},
        {"dxgi_nonlocal", {{"available", memory.nonlocal_budget_available},
            {"usage_bytes", memory.nonlocal_budget_available ? Json(memory.nonlocal_usage_bytes) : Json(nullptr)},
            {"budget_bytes", memory.nonlocal_budget_available ? Json(memory.nonlocal_budget_bytes) : Json(nullptr)}}},
        {"tracked_partial", {{"categories", std::move(categories)},
            {"live_bytes", memory.tracked_live_bytes}, {"retiring_bytes", memory.tracked_retiring_bytes},
            {"gpu_bytes", memory.tracked_gpu_bytes}, {"reclaimed_bytes", memory.tracked_reclaimed_bytes},
            {"gpu_reserved_bytes", memory.gpu_reserved_bytes},
            {"gpu_growth_since_budget_bytes", memory.gpu_growth_since_budget_bytes},
            {"host_staging_bytes", memory.host_staging_bytes}, {"host_reserved_bytes", memory.host_reserved_bytes},
            {"host_staging_budget_bytes", memory.host_budget_available ? Json(memory.host_staging_budget_bytes) : Json(nullptr)}}},
        {"texture_physical_bytes", memory.texture_physical_bytes},
        {"acceleration_structure_bytes", memory.acceleration_structure_bytes},
        {"known_provider_allocations", {{"presentation_bytes", memory.presentation_provider_bytes},
            {"reconstruction_guide_bytes", memory.reconstruction_guide_bytes},
            {"ray_reconstruction_bytes", memory.ray_reconstruction_bytes},
            {"renderer_view_bytes", memory.renderer_view_bytes},
            {"shared_deformation_bytes", memory.shared_deformation_bytes}}}};
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
            {"name_truncated", pass.name_truncated}, {"acceleration_structure_work", pass.acceleration_structure_work},
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
    auto scene = std::make_shared<hs::StaticRenderScene>();
    scene->content_generation_id = generation;
    hs::RenderInstance ground;
    ground.mesh = hs::RenderMesh::Ground;
    ground.position = {0, -.055f, 0};
    ground.scale = {50, .1f, 50};
    ground.environment_seed = 0x47524944u;
    ground.stable_id = 0x4752494400000001ull;
    scene->instances.push_back(ground);
    return scene;
}

std::shared_ptr<const hs::StaticRenderScene> HzbScene(std::string_view generation)
{
    auto scene = std::make_shared<hs::StaticRenderScene>();
    scene->content_generation_id = generation;
    hs::RenderInstance target;
    target.mesh = hs::RenderMesh::Rock;
    target.position = {0, 0, 4};
    target.scale = {.35f, .35f, .35f};
    target.environment_variant = 0;
    target.environment_seed = 0x485A4254u;
    target.stable_id = 0x485A4201ull;
    scene->instances.push_back(target);
    return scene;
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

} // namespace

int main(int argc, char **argv)
{
    Json receipt{{"schema_version", 1}, {"runner", "render-only"},
        {"fixture", "static-environment-moving-player-v1"}, {"execution_valid", false}};
    std::filesystem::path artifacts;
    std::string failure_code = "HS_RENDER_ARGUMENT_INVALID";
    int exit_code = 1;
    try
    {
        hs::RendererConfig config;
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
        bool lighting_transport_probe{};
        bool world_transport_probe{};
        bool software_gi_probe{};
        std::string ray_reconstruction_probe;
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
            else if (arg == "--lighting-transport-probe") lighting_transport_probe = true;
            else if (arg == "--world-transport-probe") world_transport_probe = true;
            else if (arg == "--software-gi-probe") software_gi_probe = world_transport_probe = true;
            else if (arg.starts_with("--ray-reconstruction="))
            {
                ray_reconstruction_probe = arg.substr(21);
                if (ray_reconstruction_probe != "fsr_rr" && ray_reconstruction_probe != "dlss_rr")
                    throw std::runtime_error("ray reconstruction must be fsr_rr or dlss_rr");
                lighting_transport_probe = true;
            }
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
            surface_motion_resize_probe || (lighting_transport_probe && ray_reconstruction_probe.empty());
        if (lighting_transport_probe)
        {
            config.ray_tracing_enabled = true;
            config.ao_mode = hs::GraphicsAoMode::RTAO;
            config.gi_mode = hs::GraphicsGiMode::RayTraced;
            config.reflection_mode = hs::GraphicsReflectionMode::HybridRT;
            config.ao_quality = hs::GraphicsQuality::Low;
            config.gi_quality = hs::GraphicsQuality::Low;
            config.reflection_quality = hs::GraphicsQuality::Low;
            config.shadow_quality = hs::GraphicsQuality::Low;
            config.shadow_mode = hs::GraphicsShadowMode::HybridRT;
        }
        if (world_transport_probe)
        {
            if (!frames_explicit) frames = 7;
            if (frames < 7) throw std::runtime_error("--world-transport-probe requires at least 7 frames");
            config.ray_tracing_enabled = false;
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
        if (!ray_reconstruction_probe.empty())
        {
            config.ray_reconstruction_provider.value = ray_reconstruction_probe;
            config.neural_ray_reconstruction = hs::GraphicsAutoToggle::On;
            if (ray_reconstruction_probe == "dlss_rr")
            {
                config.temporal_reconstruction = true;
                config.reconstruction_request = hs::RendererReconstructionRequest{};
                config.reconstruction_request->provider.value = "dlss_rr";
            }
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
        if (grid_floor_probe && frames < 25)
            throw std::runtime_error("grid floor probe requires at least 25 frames");
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
        if (skin_cache_monster_probe && frames < 2)
            throw std::runtime_error("--skin-cache-monster-probe requires at least 2 frames");
        if (dynamic_lod_skin_growth_probe && frames < 8)
            throw std::runtime_error("--dynamic-lod-skin-growth-probe requires at least 8 frames");
        if (gpu_lod_probe && frames < 10)
            throw std::runtime_error("--gpu-lod-probe requires at least 10 frames");
        if (surface_motion_resize_fixture && frames < 7)
            throw std::runtime_error(
                "--surface-motion-resize-probe/--lighting-transport-probe require at least 7 frames");
        if (static_cast<unsigned>(persistent_scene_probe) +
                static_cast<unsigned>(skin_cache_monster_probe) +
                static_cast<unsigned>(dynamic_lod_skin_growth_probe) +
                static_cast<unsigned>(gpu_lod_probe) +
                static_cast<unsigned>(residency_streaming_probe) +
                static_cast<unsigned>(surface_motion_resize_fixture) +
                static_cast<unsigned>(world_transport_probe) +
                static_cast<unsigned>(material_reload_probe) +
                static_cast<unsigned>(owned_view_probe) +
                static_cast<unsigned>(provider_switch_probe) +
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
        if (persistent_scene_probe || material_reload_probe)
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
        receipt["fixture"] = provider_switch_probe ? "provider-switch-v1" : owned_view_probe ? "owned-view-history-v1" : persistent_scene_probe
            ? "persistent-gpu-scene-v2"
            : skin_cache_monster_probe ? "skin-cache-monster-v1"
            : dynamic_lod_skin_growth_probe ? "dynamic-lod-skin-growth-v1"
            : gpu_lod_probe ? "gpu-lod-probe-v1"
             : residency_streaming_probe ? "residency-streaming-v3"
              : lighting_transport_probe ? "lighting-transport-v1"
              : software_gi_probe ? "software-gi-world-v1"
              : world_transport_probe ? "world-transport-v1"
              : surface_motion_resize_probe ? "surface-motion-resize-v1"
             : grid_floor_probe ? "grid-floor-probe-v1"
             : material_reload_probe ? "material-parameter-reload-v1"
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
        const auto owned_view_archer_asset = owned_view_probe ? SelectedArcherAsset(generation) : hs::AssetId{};
        config.content_generation_id = generation.id;
        config.content_generation_lease = std::make_shared<const hs::SelectedGeneration>(generation);
        config.product_paths = generation.product_paths;
        config.product_digests = generation.product_digests;
        std::shared_ptr<const hs::StaticRenderScene> world_scene;
        if (world_transport_probe || owned_view_probe)
        {
            failure_code = "HS_RENDER_WORLD_TRANSPORT_SCENE_INVALID";
            std::filesystem::path product_path;
            Require(generation.Require(config.scene_hlod_product_id, product_path));
            Require(generation.Require("render/lighting/local_probes.spkg", product_path));
            measure("world_scene", [&] { world_scene = WorldTransportScene(generation); });
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
            const auto handle = recording_workers.Submit(*recording_group,
                hs::TaskDesc{"Render fixture recording"},
                [record = std::move(record)](std::stop_token) { record(); });
            return handle ? hs::Result::Success() :
                hs::Result::Failure(hs::ErrorCode::TaskFailed, "hs_render_test_runner",
                                    "Could not submit recording task.");
        };
        config.task_executor.join = [&] {
            auto joined = recording_workers.Wait(*recording_group);
            recording_group = std::make_unique<hs::TaskGroup>();
            return joined;
        };
        hs::D3D12Renderer renderer;
        failure_code = "HS_RENDER_INITIALIZE_FAILED";
        measure("initialize", [&] { Require(renderer.Initialize(config)); });
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
        }
        const auto scene = Scene(generation.id);
        const auto grid_floor_scene = GridFloorScene(generation.id);
        const auto hzb_scene = HzbScene(generation.id);
        hs::RenderInstance first;
        first.mesh = hs::RenderMesh::Archer;
        first.scale = {.97174913f, .97174913f, .97174913f};
        first.stable_id = 1;
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
        occluder.mesh = hs::RenderMesh::Area;
        occluder.position = {0.0f, -0.25f, -9.25f};
        occluder.scale = {10.0f, 10.0f, 3.0f};
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
        hs::RenderSnapshotExchange::ReadPair pair;
        pair.has_current = true;
        pair.current.static_scene = world_transport_probe || owned_view_probe ? world_scene
            : grid_floor_probe ? grid_floor_scene : scene;
        pair.current.header.session_id = 1;
        pair.current.camera.target = {0, 1, 0};
        pair.current.camera.yaw_degrees = 0;
        pair.current.camera.pitch_degrees = 22;
        pair.current.camera.distance = dynamic_lod_skin_growth_probe ? 28.0f : 22.0f;
        std::array<hs::LightView, 3> world_lights{};
        if (world_transport_probe || lighting_transport_probe)
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
        failure_code = "HS_RENDER_FRAME_FAILED";
        hs::RendererQueueStats queue_totals;
        std::map<std::uint64_t, hs::GpuPassTimings> world_gpu_history;
        std::map<std::uint64_t, hs::GpuPassTimings> provider_gpu_history;
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
                    else if (frame == 7)
                    {
                        current = {occluder};
                        pair.current.static_scene = hzb_scene;
                    }
                    else if (frame == 8) current[0].position.x = 30.0f;
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
                pair.current.instances = current;
                pair.current.header.tick = material_reload_probe || grid_floor_probe ? 1 : frame;
                pair.current.header.simulation_time = material_reload_probe || grid_floor_probe || persistent_scene_probe
                    ? std::chrono::nanoseconds::zero()
                    : std::chrono::nanoseconds(
                        static_cast<std::int64_t>(frame) * 1000000000ll / 60);
                observe_world_timings();
                observe_provider_timings();
                const auto main_view = renderer.MainViewInput(pair.current.camera);
                const hs::RenderFrameInput render_input{
                    .views = std::span(&main_view, 1), .snapshots = pair,
                    .delta_seconds = persistent_scene_probe || material_reload_probe
                        ? std::optional<float>{0.0f} : std::nullopt};
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
                Require(renderer.Render(render_input, frame_result));
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
                    Require(renderer.CapturePng(artifacts / "hzb_retest.png"));
                if (persistent_scene_probe && frame == 9)
                    Require(renderer.CapturePng(artifacts / "hzb_reference.png"));
                if (surface_motion_resize_fixture && frame == 3)
                {
                    failure_code = "HS_RENDER_SURFACE_DIAGNOSTIC_FAILED";
                    Require(renderer.CaptureSurfaceDiagnostics(artifacts / "surface-motion.json"));
                    ValidateSurfaceDiagnostic(artifacts / "surface-motion.json", config.width,
                                              config.height, true, "motion", receipt);
                }
                if (gpu_lod_probe && (frame == 1 || frame == 3 || frame == 4 ||
                                      frame == 5 || frame == 10))
                {
                    failure_code = "HS_RENDER_GEOMETRY_DIAGNOSTIC_FAILED";
                    const auto name = frame == 1 ? "lod-initial.json" :
                        frame == 3 ? "lod-before-growth.json" :
                        frame == 4 ? "lod-after-growth.json" :
                        frame == 5 ? "lod-away.json" : "lod-return.json";
                    Require(renderer.CaptureGeometryDiagnostics(artifacts / name));
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
                    Require(renderer.CapturePng(artifacts / name));
                    receipt["material_reload"]["captures"][std::to_string(frame)] = name;
                }
                if (grid_floor_probe && capture_grid_frames &&
                    (frame == 1 || frame == 9 || frame == 17 || frame == 25))
                {
                    const auto name = std::format("grid-{0:02}.png", frame);
                    Require(renderer.CapturePng(artifacts / name));
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
                    graphics.ray_tracing_enabled = config.ray_tracing_enabled;
                    graphics.rt_trace_distance_m = config.rt_trace_distance_m;
                    graphics.gi_mode = config.gi_mode;
                    graphics.gi_quality = config.gi_quality;
                    graphics.shadow_mode = config.shadow_mode;
                    graphics.volumetric_quality = config.volumetric_quality;
                    graphics.reconstruction_provider = config.reconstruction_request
                        ? config.reconstruction_request->provider : hs::GraphicsProviderId{"auto"};
                    graphics.reconstruction_quality = config.reconstruction_request
                        ? config.reconstruction_request->quality : hs::GraphicsModeId{"quality"};
                    graphics.ray_reconstruction_provider = config.ray_reconstruction_provider;
                    graphics.neural_ray_reconstruction = config.neural_ray_reconstruction;
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
                    Require(renderer.CaptureSurfaceDiagnostics(artifacts / "surface-resized.json"));
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
        failure_code = "HS_RENDER_CAPTURE_FAILED";
        if (config.surface_frontend == hs::SurfaceFrontend::VisibilityBuffer)
        {
            failure_code = "HS_RENDER_VISIBILITY_DIAGNOSTICS_FAILED";
            measure("visibility_diagnostics", [&] {
                Require(renderer.CaptureVisibilityDiagnostics(artifacts / "visibility.json"));
            });
            receipt["visibility"] = Json::parse(std::ifstream(artifacts / "visibility.json"));
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
        if (world_transport_probe)
        {
            failure_code = "HS_RENDER_WORLD_TRANSPORT_PASSES_FAILED";
            receipt["lighting_status"] = LightingStatusReceipt(renderer.GetLightingStatus());
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
        if (lighting_transport_probe)
        {
            const auto lighting_status = renderer.GetLightingStatus();
            receipt["lighting_status"] = LightingStatusReceipt(lighting_status);
            if (!ray_reconstruction_probe.empty())
            {
                const auto &native = lighting_status.radiance_denoiser;
                receipt["ray_reconstruction_probe"] = {{"requested", ray_reconstruction_probe},
                    {"active", native.active_method}, {"availability", native.availability}, {"reason", native.reason}};
                if (native.active_method == "dlss_rr" && gpu_timings.valid &&
                    (!HasGpuPass(gpu_timings, 256, "DLSS RR Opaque Guides") ||
                     !HasGpuPass(gpu_timings, 257, "DLSS RR Fused Native")))
                    throw std::runtime_error("active DLSS RR did not execute its true guide and sole fused native passes");
                if (native.active_method == "fsr_rr" && gpu_timings.valid &&
                    (!HasGpuPass(gpu_timings, 252, "FSR RR Diffuse Remodulate") ||
                     !HasGpuPass(gpu_timings, 255, "FSR RR Specular Remodulate")))
                    throw std::runtime_error("active FSR RR did not execute both selected material remodulation paths");
            }

            const bool active_rtgi =
                lighting_status.active.gi_mode == hs::GraphicsGiMode::RayTraced;
            const bool active_hybrid_reflection =
                lighting_status.active.reflection_mode == hs::GraphicsReflectionMode::HybridRT;
            const bool active_native_ray_consumer = lighting_status.active.ray_tracing_enabled &&
                lighting_status.ray_scene_supported;
            const auto &active_denoiser = lighting_status.radiance_denoiser.active_method;
            const bool native_rr = active_denoiser == "dlss_rr" || active_denoiser == "fsr_rr";
            const char *rtgi_pass_name = native_rr ? "RRDiffuseTrace" : "RTGITrace";
            const char *reflection_pass_name = native_rr ? "RRSpecularTrace" : "HybridReflectionTrace";
            const bool rtgi_pass = HasGpuPass(gpu_timings, 156, rtgi_pass_name);
            const bool hybrid_reflection_pass =
                HasGpuPass(gpu_timings, 160, reflection_pass_name);
            const bool ray_transport_compose_pass =
                HasGpuPass(gpu_timings, 169, "RayTransportCompose");
            if (active_native_ray_consumer && gpu_timings.valid)
            {
                if (active_rtgi && !rtgi_pass)
                    throw std::runtime_error(std::string("active RTGI consumer did not emit ") + rtgi_pass_name + " pass");
                if (active_hybrid_reflection && !hybrid_reflection_pass)
                    throw std::runtime_error(
                        std::string("active HybridRT reflection consumer did not emit ") + reflection_pass_name + " pass");
            }
            receipt["lighting_transport"] = {
                {"native_ray_consumer_active", active_native_ray_consumer},
                {"ray_transport_hardware", (lighting_status.ray_scene_supported &&
                    lighting_status.inline_ray_queries_supported) ? "supported" : "unsupported"},
                {"gpu_pass_timings_valid", gpu_timings.valid},
                {"rtgi", {
                    {"active", active_rtgi},
                    {"pass_id", 156},
                    {"pass_name", rtgi_pass_name},
                    {"present", gpu_timings.valid ? Json(rtgi_pass) : Json(nullptr)},
                }},
                {"hybrid_reflection", {
                    {"active", active_hybrid_reflection},
                    {"pass_id", 160},
                    {"pass_name", reflection_pass_name},
                    {"present", gpu_timings.valid ? Json(hybrid_reflection_pass) : Json(nullptr)},
                }},
                {"compose", {
                    {"pass_id", 169},
                    {"pass_name", "RayTransportCompose"},
                    {"present", gpu_timings.valid ? Json(ray_transport_compose_pass) : Json(nullptr)},
                }},
            };
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
            if (residency_stats.geometry_streaming_requests < 2 ||
                residency_stats.geometry_streaming_uploads !=
                    residency_stats.geometry_streaming_requests ||
                residency_stats.geometry_streaming_publications !=
                    residency_stats.geometry_streaming_requests ||
                residency_stats.geometry_streaming_reclaims <
                    residency_stats.geometry_streaming_requests)
                throw std::runtime_error("Geometry streaming did not publish and reclaim every requested geometry family.");
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
        }
        if (config.validation && skin_cache_monster_probe)
            ValidateMonsterSkinCacheProbe(
                DynamicUploadRows(artifacts / "dynamic_instance_upload.csv"), frames, receipt);
        if (config.validation && dynamic_lod_skin_growth_probe)
            ValidateDynamicLodSkinGrowthProbe(
                DynamicUploadRows(artifacts / "dynamic_instance_upload.csv"), frames, receipt);
        receipt["validation_errors"] = renderer.ValidationErrorCount();
        receipt["frames"] = frames;
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
    if (!artifacts.empty() && artifacts.is_absolute() && std::filesystem::is_directory(artifacts))
    {
        std::ofstream output(artifacts / "result.json");
        output << receipt.dump(2) << '\n';
        if (!output) exit_code = 1;
    }
    std::cout << receipt.dump() << '\n';
    return exit_code;
}
