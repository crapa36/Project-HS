#include <hs/runtime/application.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include <hs/presentation/projector.hpp>

#include "runtime_channels.hpp"
#include "vfx_catalog.hpp"
#include "vfx_typed_frame_adapter.hpp"
#include "vfx_typed_ground_commands.hpp"
#include "vfx_upgrade_dispatch.hpp"
#include "vfx_typed_light_commands.hpp"
#include "vfx_typed_distortion_commands.hpp"
#include "vfx_typed_decal_commands.hpp"
#include "vfx_typed_volume_commands.hpp"
#include "vfx_typed_screen_commands.hpp"
#include "vfx_typed_mesh_commands.hpp"
#include "vfx_typed_fresnel_commands.hpp"
#include "vfx_typed_ribbon_commands.hpp"
#include "vfx_typed_flash_commands.hpp"
#include "vfx_typed_mote_commands.hpp"
#include "vfx_typed_ballistic_commands.hpp"
#include "vfx_session_state.hpp"
#include "vfx_generation_rebind.hpp"
#include "renderer_settings_projection.hpp"
#include "window.hpp"
#include "camera_pose.hpp"

#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/process_info.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/jobs/task_system.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/renderer/render_frame_input.hpp>
#include <hs/runtime/audio_engine.hpp>
#include <hs/runtime/playtest_recording.hpp>
#include <hs/runtime/save_store.hpp>
#include <hs/runtime/settings_projection.hpp>

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hs
{
namespace
{

Result PrepareVfxSpriteBindings(const VfxCatalog &catalog, RendererVfxSpriteBindings &bindings)
{
    for (const auto &[id, target] : {
             std::pair<std::string_view, ParticleSpriteBinding *>("particle_sprite.blood", &bindings.bleed_status_sprite),
             {"particle_sprite.fire", &bindings.burn_status_sprite},
             {"particle_sprite.slow_rune", &bindings.slow_status_sprite},
             {"particle_sprite.mark_target", &bindings.mark_status_sprite},
             {"particle_sprite.trap_rune", &bindings.trap_sprite},
             {"particle_sprite.fire_ground", &bindings.fire_area_sprite}})
    {
        const auto *sprite = catalog.FindSprite(MakeAssetId(id));
        if (!sprite)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "VFX catalog omits required sprite: " + std::string(id));
        *target = {sprite->index, sprite->frame_columns, sprite->frame_rows};
    }
    return Result::Success();
}

enum class GraphicsUiPhase : std::uint8_t
{
    Applied,
    Requested,
    Pending,
    AwaitingDisplayConfirmation,
    RestoringDisplay,
    ApplyFailed,
};

// Receipts contain only public renderer support queries and applied state.
Result ResolveGraphicsDisplayOutput(HWND window, const GraphicsDisplayId &requested,
                                    GraphicsDisplayId &effective)
{
    std::vector<RendererDisplayOutput> displays;
    if (auto result = EnumerateWindowDisplayPlacements(displays); !result) return result;
    if (requested.value != "auto")
    {
        const auto selected = std::ranges::find(displays, requested.value,
                                                &RendererDisplayOutput::id);
        if (selected != displays.end())
        {
            effective.value = selected->id;
            return Result::Success();
        }
    }
    if (!window)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime",
                               "Cannot resolve a display without a window.");
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "Cannot query the current display.");
    const auto selected = std::ranges::find_if(displays, [&](const auto &display) {
        return display.left == monitor.rcMonitor.left && display.top == monitor.rcMonitor.top &&
            static_cast<std::int64_t>(display.left) + display.width == monitor.rcMonitor.right &&
            static_cast<std::int64_t>(display.top) + display.height == monitor.rcMonitor.bottom;
    });
    if (selected == displays.end())
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "The current window display is not exposed by Win32 display enumeration.");
    effective.value = selected->id;
    return Result::Success();
}

std::uint64_t CompletedPresentingFrame(const RendererDiagnosticsSnapshot &diagnostics)
{
    const auto view = std::ranges::find_if(diagnostics.views, [](const auto &entry) { return entry.presenting; });
    if (view == diagnostics.views.end()) return 0;
    std::uint64_t latest{};
    for (const auto &frame : diagnostics.completed_frames)
        if (frame.fence_completed && frame.view_id == view->view_id)
            latest = std::max(latest, frame.rendered_frame_id);
    return latest;
}

void UpdateGraphicsTelemetry(GraphicsSettingsUiModel &model,
                             const RendererDiagnosticsSnapshot &diagnostics, bool validation)
{
    const auto extent = [](GraphicsExtent value) {
        return value.width && value.height ? std::format("{}x{}", value.width, value.height)
                                           : std::string("unavailable");
    };
    const auto view = std::ranges::find_if(diagnostics.views, [](const auto &entry) { return entry.presenting; });
    const auto *measurements = view == diagnostics.views.end() ? nullptr
        : validation ? &view->validation_on : &view->validation_off;
    const auto fps = [](const RendererDiagnosticsCadence *cadence) {
        return cadence && cadence->available && std::isfinite(cadence->fps) && cadence->fps > 0.0
            ? std::format("{:.1f}", cadence->fps) : std::string("unavailable");
    };
    model.telemetry_status[1] = std::format("Render {} / output {} | Render FPS {} | Display FPS {}",
        extent(diagnostics.reconstruction.render_extent), extent(diagnostics.reconstruction.output_extent),
        fps(measurements ? &measurements->rendered_cadence : nullptr), fps(&diagnostics.displayed_cadence));
    const auto mib = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
    const auto &memory = diagnostics.current_memory;
    const auto current = memory.budget_available
        ? std::format("{:.1f} / {:.1f} MiB", mib(memory.local_usage_bytes), mib(memory.local_budget_bytes))
        : std::string("unavailable");
    const auto peak = diagnostics.transition_memory_peak_available && diagnostics.transition_memory_peak.budget_available
        ? std::format("{:.1f} MiB", mib(diagnostics.transition_memory_peak.local_usage_bytes))
        : std::string("unavailable");
    model.telemetry_status[2] = std::format("DXGI process VRAM usage / budget {} | Observed transition usage peak {}",
                                          current, peak);
}

std::shared_ptr<const GraphicsSettingsUiModel> RendererGraphicsReceipt(
    const D3D12Renderer &renderer, const GraphicsSettings &graphics, std::uint64_t generation,
    HWND window_handle, bool validation)
{
    auto model = std::make_shared<GraphicsSettingsUiModel>();
    model->requested = graphics;
    model->generation = generation;
    const auto canonical = renderer.GetGraphicsSettingsStatus();
    if (canonical.actual_applied_available)
    {
        model->requested = canonical.requested;
        model->generation = canonical.generation;
        model->transaction_state = canonical.state == SettingsApplyState::Restored
            ? "Restored" : canonical.state == SettingsApplyState::Rejected
                ? "Rejected" : canonical.state == SettingsApplyState::Applied
                    ? "Applied" : "Pending";
    }
    const auto reconstruction = renderer.GetReconstructionStatus();
    const auto presentation = renderer.GetPresentationStatus();
    const auto lighting = renderer.GetLightingStatus();
    const auto streaming = renderer.GetTextureStreamingStatus();
    const auto capabilities = renderer.GetGraphicsCapabilities();
    if (presentation.availability == "available" && presentation.maximum_multiplier >= 2)
        model->frame_generation_maximum = presentation.maximum_multiplier;
    const auto choice = [&](SettingId id, std::string token, bool available, std::string reason) {
        const auto found = std::ranges::find_if(model->choices, [&](const auto &c) {
            return c.setting == id && c.token == token;
        });
        if (found == model->choices.end()) model->choices.push_back(
            {id, token, token, std::move(reason), available});
        else if (available || !found->available) { found->available = available; found->reason = std::move(reason); }
    };
    const auto status = [&](SettingId id, std::string effective, std::string reason) {
        model->resolved.push_back({id, std::move(effective), std::move(reason)});
    };
    constexpr std::array<std::pair<std::string_view, SettingId>, 14> capability_settings{{
        std::pair<std::string_view, SettingId>{"sr_provider", SettingId::ReconstructionProvider},
        {"reconstruction", SettingId::ReconstructionQuality},
        {"fg_provider", SettingId::FrameGenerationProvider},
        {"frame_generation", SettingId::FrameGenerationMode},
        {"frame_generation_multiplier", SettingId::FrameGenerationMultiplier},
        {"rr_provider", SettingId::RayReconstructionProvider},
        {"hdr", SettingId::HdrMode}, {"latency", SettingId::LowLatency},
        {"ao", SettingId::AoMode}, {"gi", SettingId::GiMode},
        {"reflection", SettingId::ReflectionMode}, {"shadow", SettingId::ShadowMode},
        {"adaptive_shading", SettingId::AdaptiveShading},
        {"neural_rr", SettingId::NeuralRayReconstruction}}};
    const auto support_reason = [](GraphicsCapabilityReason reason) -> std::string {
        switch (reason)
        {
        case GraphicsCapabilityReason::None: return "Supported by the current device and configuration";
        case GraphicsCapabilityReason::Unsupported: return "Unsupported by the current device or provider";
        case GraphicsCapabilityReason::Unavailable: return "Native provider is not ready";
        case GraphicsCapabilityReason::MissingProvider: return "Required provider is missing or excluded";
        case GraphicsCapabilityReason::MissingDisplay: return "Required display is unavailable";
        case GraphicsCapabilityReason::Incompatible: return "Incompatible with the current configuration";
        case GraphicsCapabilityReason::BudgetLimited: return "Unavailable within the current resource budget";
        case GraphicsCapabilityReason::Rejected: return "Native request was rejected";
        }
        return "Native support is unavailable";
    };
    for (const auto &entry : capabilities.options)
    {
        const std::string_view qualified = entry.option.value;
        const auto separator = qualified.find(':');
        if (separator == std::string_view::npos) continue;
        const auto setting = std::ranges::find_if(capability_settings, [&](const auto &candidate) {
            return candidate.first == qualified.substr(0, separator);
        });
        if (setting == capability_settings.end()) continue;
        const auto token = qualified.substr(separator + 1);
        choice(setting->second, std::string(token), entry.available, support_reason(entry.reason));
        if (setting->second == SettingId::FrameGenerationMultiplier && entry.available)
        {
            std::uint32_t multiplier{};
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), multiplier);
            if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size())
                model->frame_generation_maximum = std::max(model->frame_generation_maximum.value_or(0), multiplier);
        }
    }
    for (const auto &[category, id] : capability_settings)
    {
        static_cast<void>(category);
        const auto token = GraphicsSettingToken(model->requested, id);
        if (std::ranges::none_of(model->choices, [&](const auto &entry) {
                return entry.setting == id && entry.token == token;
            }))
            choice(id, token, false, "Saved selection is unavailable in the current capability catalog");
    }
    std::vector<RendererDisplayOutput> displays;
    const auto enumerated = EnumerateRendererDisplays(displays);
    choice(SettingId::DisplayOutput, "auto", static_cast<bool>(enumerated), enumerated ? std::string("Current window monitor") : std::string(enumerated.Message()));
    choice(SettingId::OutputExtent, "auto", true, "Actual window client extent");
    for (const auto &display : displays)
    {
        choice(SettingId::DisplayOutput, display.id, true, "Enumerated DXGI output");
        choice(SettingId::OutputExtent, std::format("{}x{}", display.width, display.height), true, "Enumerated desktop extent; no OS mode switch");
    }
    MONITORINFO monitor{sizeof(MONITORINFO)};
    const auto renderer_hdr = canonical.actual_applied_available
        ? canonical.actual_applied.hdr_mode == GraphicsHdrMode::On ? "HDR10" : "SDR"
        : "unavailable";
    model->telemetry_status[0] = std::format("Monitor unavailable | Refresh unavailable | Monitor HDR unavailable | Renderer {}", renderer_hdr);
    if (GetMonitorInfoW(MonitorFromWindow(window_handle, MONITOR_DEFAULTTONEAREST), &monitor))
        for (const auto &display : displays)
            if (display.left == monitor.rcMonitor.left && display.top == monitor.rcMonitor.top &&
                static_cast<std::int64_t>(display.left) + display.width == monitor.rcMonitor.right &&
                static_cast<std::int64_t>(display.top) + display.height == monitor.rcMonitor.bottom)
            {
                status(SettingId::DisplayOutput, display.id, graphics.display_output.value == "auto" || graphics.display_output.value == display.id
                    ? "Actual window monitor" : "Requested output is unavailable; current window monitor retained");
                double refresh_hz{};
                const auto refresh = QueryWindowDisplayRefresh(window_handle, refresh_hz);
                model->telemetry_status[0] = std::format("Monitor {} | Refresh {} | Monitor HDR {} | Renderer {}",
                    display.id, refresh ? std::format("{:.2f} Hz", refresh_hz) : std::string("unavailable"),
                    display.hdr_active ? "active" : "inactive", renderer_hdr);
                break;
            }
    status(SettingId::HdrMode, canonical.actual_applied_available
        ? GraphicsSettingToken(canonical.actual_applied, SettingId::HdrMode) : std::string{},
        canonical.actual_applied_available
            ? canonical.actual_applied.hdr_mode == GraphicsHdrMode::On
                ? "Actual HDR10 output (PQ / Rec.2020)" : "Actual SDR output (Rec.709)"
            : "Waiting for the native display apply receipt");
    choice(SettingId::DisplayOutput, graphics.display_output.value, false, "Saved output is not currently enumerated");
    choice(SettingId::OutputExtent, GraphicsSettingToken(graphics, SettingId::OutputExtent), false, "Saved window size request");
    status(SettingId::ReconstructionProvider, reconstruction.effective_provider.value, reconstruction.requested_availability + ": " + reconstruction.fallback_reason);
    status(SettingId::ReconstructionQuality, reconstruction.effective_quality.value, reconstruction.fallback_reason);
    status(SettingId::OutputExtent, std::format("{}x{}", reconstruction.output_extent.width, reconstruction.output_extent.height), "Actual render output extent");
    status(SettingId::FrameGenerationProvider, presentation.effective_provider.value, presentation.availability + ": " + presentation.fallback_reason);
    status(SettingId::FrameGenerationMode, std::string(GraphicsEnumToken(presentation.active_mode)), presentation.fallback_reason);
    status(SettingId::FrameGenerationMultiplier, std::to_string(presentation.observed_multiplier), presentation.maximum_multiplier
        ? std::format("Native supported range 2..{}", presentation.maximum_multiplier)
        : presentation.requested.provider.value == "fsr" ? "Provider exposes no configurable multiplier range"
        : "Native multiplier capability has not been queried for this profile");
    status(SettingId::LowLatency, presentation.low_latency_active ? "on" : "off", presentation.fallback_reason);
    status(SettingId::RayTracingEnabled, lighting.active.ray_tracing_enabled ? "true" : "false", lighting.ray_scene_supported ? "Native ray scene supported" : "Native ray scene unavailable");
    status(SettingId::AoMode, std::string(GraphicsEnumToken(lighting.active.ao_mode)), lighting.ao_fallback_reason);
    status(SettingId::GiMode, std::string(GraphicsEnumToken(lighting.active.gi_mode)), lighting.gi_fallback_reason.empty() ? lighting.software_gi.reason : lighting.gi_fallback_reason);
    status(SettingId::ReflectionMode, std::string(GraphicsEnumToken(lighting.active.reflection_mode)), lighting.reflection_fallback_reason);
    status(SettingId::ShadowMode, std::string(GraphicsEnumToken(lighting.active.shadow_mode)), lighting.shadow_fallback_reason + "; " + lighting.local_shadows.reason + "; " + lighting.ray_shadows.reason);
    status(SettingId::RayReconstructionProvider, canonical.actual_applied_available
        ? canonical.actual_applied.ray_reconstruction_provider.value : std::string{},
        lighting.radiance_denoiser.availability + ": " + lighting.radiance_denoiser.reason +
            (lighting.ray_shadows.enabled && lighting.ray_shadows.native_sigma ? "; Native NRD SIGMA shadows active" : ""));
    const auto &adaptive = lighting.adaptive_shading;
    status(SettingId::AdaptiveShading,
        std::format("Raster {}; compute {}", adaptive.raster_applied ? "on" : "off", adaptive.compute_applied ? "on" : "off"),
        std::format("Requested {}; resolved raster {} / compute {}; ready raster {} / compute {}; usable {}; VRS tier {} / tile {}; {} bytes. Raster: {} Compute: {}",
            GraphicsEnumToken(adaptive.requested), adaptive.raster_resolved, adaptive.compute_resolved,
            adaptive.raster_ready, adaptive.compute_ready, adaptive.usable, adaptive.vrs_tier,
            adaptive.vrs_tile_size, adaptive.allocated_bytes, adaptive.raster_reason, adaptive.compute_reason));
    const auto streaming_reason = std::format("Ready {}; native asset I/O {}; sampler feedback {}; {} textures / {} physical bytes. {}",
        streaming.ready, streaming.native_asset_io, streaming.sampler_feedback, streaming.texture_count,
        streaming.physical_bytes, streaming.reason);
    status(SettingId::TextureQuality, std::string(GraphicsEnumToken(streaming.applied_quality)), streaming_reason);
    status(SettingId::TextureBudgetMib, streaming.applied_budget_mib ? std::to_string(streaming.applied_budget_mib) : "Auto (0)", streaming_reason);
    for (const auto &descriptor : GraphicsSettingDescriptors())
        if (std::ranges::find(model->resolved, descriptor.id, &GraphicsUiStatus::setting) == model->resolved.end())
            status(descriptor.id, canonical.actual_applied_available
                ? GraphicsSettingToken(canonical.actual_applied, descriptor.id) : std::string{},
                canonical.actual_applied_available ? "Applied renderer receipt" : "Waiting for the renderer apply receipt");
    UpdateGraphicsTelemetry(*model, renderer.GetDiagnostics(), validation);
    return model;
}

UiModel SettingsStatusText(std::string_view label, Float2 anchor,
                           std::uint32_t color = 0xFFFFFFFFu)
{
    UiModel model;
    model.kind = UiModel::Kind::Text;
    model.anchor_pixels = anchor;
    model.size_pixels = {920, 27};
    model.color_rgba = color;
    model.font_pixels = 18;
    std::copy_n(label.data(), std::min(label.size(), model.utf8_text.size() - 1),
                model.utf8_text.data());
    return model;
}

UiModel SettingsConfirmButton(std::string_view label, Float2 anchor)
{
    auto model = SettingsStatusText(label, anchor, 0xFFFFFFFFu);
    model.kind = UiModel::Kind::Button;
    model.size_pixels = {260, 64};
    model.color_rgba = 0xFF34495Eu;
    model.font_pixels = 25;
    return model;
}

void SetThreadName(const wchar_t *name) noexcept
{
    SetThreadDescription(GetCurrentThread(), name);
}

void WriteText(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream stream(path, std::ios::trunc);
    stream << text;
}

// Only native latency uses this handoff. The render SDK owner sleeps first;
// the window owner then pumps actual input, and simulation advances its normal
// 60 Hz clock before acknowledging the same token. No renderer SDK is called
// from the window or simulation thread.
class NativeInputFrame
{
    std::mutex mutex_;
    std::condition_variable changed_;
    bool enabled_{}, cancelled_{}, input_parked_{}, simulation_parked_{};
    std::uint64_t input_request_{}, input_sampled_{}, simulation_request_{}, simulation_completed_{};
  public:
    void SetEnabled(bool enabled)
    {
        std::unique_lock lock(mutex_); enabled_ = enabled; changed_.notify_all();
        if (enabled)
            changed_.wait(lock, [&] { return cancelled_ || (input_parked_ && simulation_parked_); });
        else input_parked_ = simulation_parked_ = false;
    }
    void Cancel()
    {
        std::lock_guard lock(mutex_); cancelled_ = true; changed_.notify_all();
    }
    bool RequestInput(std::uint64_t token)
    {
        std::lock_guard lock(mutex_);
        if (!enabled_ || cancelled_ || !token || input_request_ != input_sampled_ ||
            simulation_request_ != simulation_completed_) return false;
        input_request_ = token; changed_.notify_all(); return true;
    }
    bool WaitInput(std::uint64_t token)
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return cancelled_ || !enabled_ || input_sampled_ == token; });
        return !cancelled_ && enabled_ && input_sampled_ == token;
    }
    bool ShouldPump(std::uint64_t &token)
    {
        std::lock_guard lock(mutex_); token = 0;
        if (cancelled_) return false;
        if (!enabled_) return true;
        if (input_request_ == input_sampled_)
        { input_parked_ = true; changed_.notify_all(); return false; }
        input_parked_ = false;
        token = input_request_; return true;
    }
    void Sampled(std::uint64_t token)
    {
        std::lock_guard lock(mutex_);
        if (!cancelled_ && token == input_request_) input_sampled_ = token;
        changed_.notify_all();
    }
    bool RequestSimulation(std::uint64_t token)
    {
        std::lock_guard lock(mutex_);
        if (cancelled_ || !enabled_ || input_sampled_ != token) return false;
        simulation_request_ = token; changed_.notify_all(); return true;
    }
    std::uint64_t AwaitSimulation()
    {
        std::unique_lock lock(mutex_);
        simulation_parked_ = enabled_;
        changed_.notify_all();
        changed_.wait(lock, [&] { return cancelled_ || !enabled_ || simulation_request_ != simulation_completed_; });
        simulation_parked_ = false;
        return !cancelled_ && enabled_ ? simulation_request_ : 0;
    }
    void Simulated(std::uint64_t token)
    {
        std::lock_guard lock(mutex_);
        if (!cancelled_ && token == simulation_request_) simulation_completed_ = token;
        changed_.notify_all();
    }
    bool WaitSimulation(std::uint64_t token)
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return cancelled_ || !enabled_ || simulation_completed_ == token; });
        return !cancelled_ && enabled_ && simulation_completed_ == token;
    }
};

PresentationEvent MakeAudioEvent(std::string_view cue, Sequence sequence) noexcept
{
    PresentationEvent event;
    event.sequence = sequence;
    event.kind = PresentationKind::Audio;
    event.asset = MakeAssetId(cue);
    event.parameters = EncodeAudioAction(AudioEventAction::Play);
    return event;
}

} // namespace

ApplicationResult RunApplication(const ApplicationConfig &config)
{
    if (config.smoke_render_lockstep && !config.smoke)
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Smoke render lockstep requires --smoke.")};
    const auto executable_directory = CurrentExecutableDirectory();
    if (executable_directory.empty())
    {
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Cannot resolve the executable directory.")};
    }
    const auto generation_pointer = config.generation_pointer.empty()
        ? executable_directory / "GenerationState/runtime-full.json"
        : config.generation_pointer;
    SelectedGeneration selected_generation;
    if (auto loaded = LoadSelectedGeneration(generation_pointer, selected_generation);
        !loaded)
        return {loaded};
    SimulationRules simulation_rules;
    PresentationCatalog presentation_catalog;
    std::uint64_t content_hash{};
    std::filesystem::path cooked_simulation_rules_path;
    std::filesystem::path cooked_presentation_path;
    std::filesystem::path cooked_particle_effects_path;
    std::filesystem::path cooked_vfx_program_path;
    for (const auto [id, path] : {
             std::pair<std::string_view, std::filesystem::path *>(
                 "game-data/simulation-rules", &cooked_simulation_rules_path),
             {"game-data/presentation-catalog", &cooked_presentation_path},
             {"vfx/particle_effects.hsbin", &cooked_particle_effects_path},
             {"vfx/vfx_program.hsbin", &cooked_vfx_program_path}})
        if (auto resolved = selected_generation.Require(id, *path); !resolved)
            return {resolved};
    if (auto loaded = LoadSimulationRules(cooked_simulation_rules_path,
                                          simulation_rules, &content_hash);
        !loaded)
        return {loaded};
    if (auto loaded = LoadPresentationCatalog(cooked_presentation_path,
                                              presentation_catalog, &content_hash);
        !loaded)
    {
        return {loaded};
    }
    auto simulation_rules_hash = SimulationRulesHash(simulation_rules);
    VfxCatalog vfx_catalog;
    if (auto loaded = VfxCatalog::Load(
            cooked_particle_effects_path, cooked_vfx_program_path, vfx_catalog);
        !loaded)
        return {loaded};
    PlaytestReplay replay;
    const auto replaying = !config.replay_directory.empty();
    if (replaying)
    {
        if (auto loaded = LoadPlaytestReplay(config.replay_directory, replay); !loaded)
            return {loaded};
        if (!config.replay_compare &&
            replay.header.simulation_rules_hash != simulation_rules_hash)
            return {Result::Failure(ErrorCode::InvalidState, "hs_playtest",
                                    "Replay content hash differs from the current content.")};
    }
    const auto effective_seed = replaying ? replay.header.seed : config.seed;
    PlaytestRecorder playtest_recorder;
    if (config.record_playtest)
    {
        if (auto started = playtest_recorder.Start(
                {config.playtest_output_directory, effective_seed,
                 simulation_rules_hash, content_hash}); !started)
            return {started};
    }
    const auto settings_root = config.smoke
        ? std::optional<std::filesystem::path>(config.artifact_directory / "Profile")
        : std::nullopt;
    SaveStore save_store(settings_root);
    SettingsDocument requested_settings;
    bool loaded_from_file{};
    if (auto loaded = save_store.LoadSettingsV2(requested_settings, &loaded_from_file); !loaded)
    {
        return {loaded};
    }
    if (!loaded_from_file)
        requested_settings = LegacyDefaultSettingsDocument();
    if (config.render_scale_explicit)
        (void)ApplySettingsEdit(requested_settings,
                                {UiCommandKind::SetRenderScale, config.render_scale_percent});
    if (config.shadow_resolution_explicit)
        (void)ApplySettingsEdit(requested_settings,
                                {UiCommandKind::SetShadowResolution, config.shadow_resolution});
    if (config.particle_percentage_explicit)
        (void)ApplySettingsEdit(requested_settings,
                                {UiCommandKind::SetParticlePercentage, config.particle_percentage});
    requested_settings.graphics.bloom = config.bloom;
    requested_settings.graphics.outline = config.outline;
    SettingsData initial_settings = ProjectSettingsDocument(requested_settings);
    initial_settings.width = config.width;
    initial_settings.height = config.height;
    initial_settings.borderless = config.borderless;
    initial_settings.vsync = config.vsync;
    initial_settings.frame_cap = config.frame_cap;
    if (config.mute_audio)
        initial_settings.master_volume = 0.0f;
    GraphicsSettingsCommit settings_commit(std::move(requested_settings),
                                           initial_settings, config.mute_audio);
    SettingsData &settings = settings_commit.Effective();
    ProfileData profile;
    if (auto loaded = save_store.LoadProfile(profile); !loaded)
    {
        return {loaded};
    }
    AudioEngine audio;
    if (auto initialized = audio.Initialize(settings, selected_generation.product_paths); !initialized)
    {
        return {initialized};
    }

    // No window owns the foreground yet; suppress even the initial menu cue.
    audio.SetBackgroundMuted(true);

    // Bounded event queues carry full gameplay geometry and exceed the default
    // Windows stack reserve; their lifetime still spans all joined workers.
    auto channel_storage = std::make_unique<RuntimeChannels>();
    auto &channels = *channel_storage;
    channels.camera_zoom_percent.store(std::clamp(config.camera_zoom_percent, 15u, 120u));
    channels.best_level.store(profile.best_level, std::memory_order_relaxed);
    TaskSystem task_system;
    Window window(channels, settings.skill_virtual_keys, presentation_catalog.camera);
    Sequence ui_audio_sequence{};
    AssetId active_ambience{};
    AssetId active_bgm = MakeAssetId("audio.bgm.main_menu");
    // MainMenu is the initial phase, so it never crosses a phase transition
    // that would otherwise trigger the normal BGM selection below.
    audio.Play(MakeAudioEvent("audio.bgm.main_menu", ++ui_audio_sequence));
    SettingsData ui_click_settings = settings_commit.UiInput();
    SettingsDocument ui_click_document = settings_commit.Requested();
    GraphicsSettingsUiModel ui_click_model;
    ui_click_model.requested = ui_click_document.graphics;
    ui_click_model.transaction_state = "Renderer initialization pending";
    channels.presentation_graphics_ui.store(std::make_shared<const GraphicsSettingsUiModel>(ui_click_model), std::memory_order_release);
    bool ui_command_queue_full{};
    bool ui_click_allocation_failed{};
    std::atomic graphics_ui_phase{GraphicsUiPhase::Applied};
    std::atomic settings_save_failed{false};
    std::atomic display_modal_phase{GraphicsUiPhase::Applied};
    bool display_confirm_clicked{};
    bool display_cancel_clicked{};
    window.SetUiClickHandler([&](Float2 cursor) {
        try
        {
            const auto modal = display_modal_phase.load(std::memory_order_acquire);
            if (modal != GraphicsUiPhase::Applied)
            {
                if (modal == GraphicsUiPhase::AwaitingDisplayConfirmation)
                {
                    const auto x = (cursor.x + 1.0f) * 960.0f;
                    const auto y = (1.0f - cursor.y) * 540.0f;
                    if (x >= 650 && x <= 910 && y >= 600 && y <= 664)
                        display_confirm_clicked = true;
                    else if (x >= 1010 && x <= 1270 && y >= 600 && y <= 664)
                        display_cancel_clicked = true;
                }
                return;
            }
            const auto session = channels.ReadSessionProbe();
            PresentationUiState ui{
                static_cast<UiPage>(channels.ui_page.load(std::memory_order_acquire)),
                channels.collection_skill.load(std::memory_order_acquire),
                channels.character_skill.load(std::memory_order_acquire),
                channels.loadout_source.load(std::memory_order_acquire),
                channels.settings_group.load(std::memory_order_acquire),
                channels.settings_page.load(std::memory_order_acquire)};
            const auto interaction = ResolveUiInteraction(session, ui, ui_click_settings, cursor, &ui_click_model);
            channels.ui_page.store(static_cast<std::uint8_t>(ui.page),
                                   std::memory_order_release);
            channels.collection_skill.store(ui.selected_collection_skill, std::memory_order_release);
            channels.character_skill.store(ui.selected_character_skill, std::memory_order_release);
            channels.loadout_source.store(ui.loadout_source_slot, std::memory_order_release);
            channels.settings_group.store(ui.settings_group, std::memory_order_release);
            channels.settings_page.store(ui.settings_page, std::memory_order_release);
            if (interaction.gameplay_action) (void)channels.ui_actions.TryPush(*interaction.gameplay_action);
            if (interaction.runtime_command)
            {
                if (channels.ui_commands.TryPush(*interaction.runtime_command))
                {
                    if (ApplySettingsEdit(ui_click_document, *interaction.runtime_command))
                    {
                        ui_click_settings = ProjectSettingsDocument(ui_click_document);
                        if (config.mute_audio) ui_click_settings.master_volume = 0;
                        ui_click_model.requested = ui_click_document.graphics;
                    }
                }
                else
                    ui_command_queue_full = true;
            }
            std::string_view cue;
            if (interaction.invalid)
                cue = "audio.ui.invalid";
            if (interaction.gameplay_action)
            {
                switch (interaction.gameplay_action->kind)
                {
                case UiActionKind::StartSession: cue = "audio.ui.confirm"; break;
                case UiActionKind::Quit:
                case UiActionKind::ReturnToMainMenu: cue = "audio.ui.cancel"; break;
                case UiActionKind::Reroll: cue = "audio.ui.reroll"; break;
                case UiActionKind::SelectCard: cue = "audio.ui.card_select"; break;
                case UiActionKind::AssignStat: cue = "audio.ui.stat_allocate"; break;
                case UiActionKind::Resume: cue = "audio.ui.pause_close"; break;
                case UiActionKind::SwapLoadoutSlots: cue = "audio.ui.tab"; break;
                }
            }
            if (interaction.runtime_command)
            {
                switch (interaction.runtime_command->kind)
                {
                case UiCommandKind::BeginSkillRebind: cue = "audio.ui.rebind_start"; break;
                case UiCommandKind::CancelSkillRebind: cue = "audio.ui.cancel"; break;
                case UiCommandKind::SetMasterVolumePercent:
                case UiCommandKind::SetBgmVolumePercent:
                case UiCommandKind::SetSfxVolumePercent:
                case UiCommandKind::SetUiVolumePercent: cue = "audio.ui.slider_tick"; break;
                default: if (cue.empty()) cue = "audio.ui.confirm"; break;
                }
            }
            if (!cue.empty())
                audio.Play(MakeAudioEvent(cue, ++ui_audio_sequence));
        }
        catch (const std::bad_alloc &)
        {
            ui_click_allocation_failed = true;
            OutputDebugStringA("Settings click preview allocation failed; queued command and committed settings retained.\n");
        }
    });
    std::uint32_t hover_target = 0;
    std::uint32_t previous_hover_target = 0;
    window.SetUiHoverHandler([&](Float2 cursor) {
        const auto phase = static_cast<SessionPhase>(
            channels.session_phase.load(std::memory_order_acquire));
        const auto page = static_cast<UiPage>(
            channels.ui_page.load(std::memory_order_acquire));
        const auto x = (cursor.x + 1.0f) * 960.0f;
        const auto y = (1.0f - cursor.y) * 540.0f;
        const auto inside = [&](float left, float top, float width, float height) {
            return x >= left && x <= left + width && y >= top && y <= top + height;
        };
        if (phase == SessionPhase::MainMenu && page == UiPage::Root)
        {
            for (std::uint32_t i = 0; i < 4; ++i)
                if (inside(760.0f, 270.0f + i * 150.0f, 400.0f, 92.0f)) { hover_target = i + 1; goto hover_done; }
        }
        if ((phase == SessionPhase::CardSelection || phase == SessionPhase::RelicSelection) &&
            inside(760.0f, 790.0f, 400.0f, 72.0f)) { hover_target = 20; goto hover_done; }
        if (phase == SessionPhase::Paused && page == UiPage::Root)
        {
            for (std::uint32_t i = 0; i < 3; ++i)
                if (inside(760.0f, 420.0f + i * 100.0f, 400.0f, 72.0f)) { hover_target = 30 + i; goto hover_done; }
        }
        hover_target = 0;
    hover_done:
        if (hover_target != previous_hover_target)
        {
            previous_hover_target = hover_target;
            if (hover_target != 0)
                audio.Play(MakeAudioEvent("audio.ui.hover", ++ui_audio_sequence));
        }
    });
    auto result =
        window.Create(config.width, config.height, config.visible, config.borderless);
    if (!result)
    {
        return {result};
    }
    GraphicsSettings window_display_graphics = settings_commit.Requested().graphics;
    GraphicsExtent windowed_client_extent = window_display_graphics.output_extent.value_or(
        GraphicsExtent{config.width, config.height});
    bool window_borderless = settings.borderless;
    GraphicsDisplayId initial_native_output;
    if (auto resolved = ResolveGraphicsDisplayOutput(window.Handle(),
                                                     window_display_graphics.display_output,
                                                     initial_native_output); !resolved)
        return {resolved};
    if (auto placed = window.ApplyDisplayPlacement(initial_native_output,
            window_borderless, windowed_client_extent.width, windowed_client_extent.height); !placed)
        return {placed};
    GraphicsDisplayId window_native_output = initial_native_output;
    RECT initial_client{};
    if (!GetClientRect(window.Handle(), &initial_client) ||
        initial_client.right <= initial_client.left || initial_client.bottom <= initial_client.top)
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Cannot query the placed window client extent.")};
    settings.width = static_cast<std::uint32_t>(initial_client.right - initial_client.left);
    settings.height = static_cast<std::uint32_t>(initial_client.bottom - initial_client.top);
    settings_commit.SetDesiredOutputExtent(settings.width, settings.height);
    if (config.smoke)
    {
        window.StopInput();
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(config.artifact_directory, filesystem_error);

    std::mutex result_mutex;
    Result thread_result = Result::Success();
    std::atomic<Tick> rendered_tick{};
    NativeInputFrame native_input_frame;
    auto record_thread_failure = [&](Result failure) {
        std::lock_guard lock(result_mutex);
        if (thread_result)
        {
            thread_result = std::move(failure);
        }
        channels.stop_requested.store(true, std::memory_order_release);
        native_input_frame.Cancel();
    };
    const auto log_settings_save_failure = [&](const Result &failure) {
        settings_save_failed.store(true, std::memory_order_release);
        const auto message = std::format("Settings save failed (code {}): {}\n",
                                         static_cast<unsigned>(failure.Code()),
                                         failure.Message());
        OutputDebugStringA(message.c_str());
        std::ofstream log(config.artifact_directory / "settings_save_errors.log",
                          std::ios::app);
        log << message;
    };
    bool settings_persistence_rollback_requested{};
    const auto save_committed_settings = [&] {
        try
        {
            auto confirmation = settings_commit.Committed();
            if (auto saved = save_store.SaveSettingsV2(confirmation); !saved)
            {
                log_settings_save_failure(saved);
                settings_persistence_rollback_requested = true;
            }
            else
            {
                settings_commit.ConfirmPersisted(std::move(confirmation));
                settings_persistence_rollback_requested = false;
                settings_save_failed.store(false, std::memory_order_release);
            }
        }
        catch (const std::bad_alloc &)
        {
            settings_save_failed.store(true, std::memory_order_release);
            settings_persistence_rollback_requested = true;
            OutputDebugStringA("Settings save allocation failed; last persisted profile retained.\n");
        }
    };

    auto render_ports = channels.ForRender();
    const SettingsData initial_renderer_settings = settings;
    const GraphicsSettings startup_graphics = settings_commit.Requested().graphics;
    std::jthread render_thread([&, render_ports, initial_renderer_settings, startup_graphics](std::stop_token) mutable {
        auto &channels = render_ports;
        SetThreadName(L"HS Render");
        auto render_tasks = std::make_unique<TaskGroup>();
        auto render_generation = std::make_shared<const SelectedGeneration>(selected_generation);
        D3D12Renderer renderer;
        RendererConfig renderer_config = ProjectRendererConfig(initial_renderer_settings, startup_graphics);
        renderer_config.display_output = initial_native_output;
        renderer_config.task_executor.submit = [&](std::function<void()> record) {
            const auto handle = task_system.Submit(*render_tasks,
                TaskDesc{"Render graph recording"},
                [record = std::move(record)](std::stop_token) { record(); });
            return handle ? Result::Success() :
                Result::Failure(ErrorCode::TaskFailed, "hs_runtime",
                                "Could not submit render graph recording task.");
        };
        renderer_config.task_executor.join = [&] {
            auto joined = task_system.Wait(*render_tasks);
            render_tasks = std::make_unique<TaskGroup>();
            return joined;
        };
        renderer_config.window = window.Handle();
        renderer_config.warp = config.warp;
        renderer_config.validation = config.validation;
        renderer_config.gpu_validation = config.gpu_validation;
        renderer_config.interpolate = !config.smoke;
        renderer_config.character_preview = config.character_preview;
        renderer_config.monster_preview_asset = config.monster_preview_asset;
        renderer_config.slime_family_preview_count = config.slime_family_preview_count;
        renderer_config.environment_preview = config.environment_preview;
        renderer_config.monster_preview_clip = config.monster_preview_clip;
        renderer_config.monster_preview_time = config.monster_preview_time;
        renderer_config.preview_camera_override = config.preview_camera_override;
        renderer_config.preview_camera_yaw = config.preview_camera_yaw;
        renderer_config.preview_camera_pitch = config.preview_camera_pitch;
        renderer_config.preview_camera_distance = config.preview_camera_distance;
        renderer_config.devtools_visible = config.skill_vfx_capture >= kCombatSkillCount;
        RendererVfxSpriteBindings initial_sprite_bindings;
        auto initialized = PrepareVfxSpriteBindings(vfx_catalog, initial_sprite_bindings);
        renderer_config.bleed_status_sprite = initial_sprite_bindings.bleed_status_sprite;
        renderer_config.burn_status_sprite = initial_sprite_bindings.burn_status_sprite;
        renderer_config.slow_status_sprite = initial_sprite_bindings.slow_status_sprite;
        renderer_config.mark_status_sprite = initial_sprite_bindings.mark_status_sprite;
        renderer_config.trap_sprite = initial_sprite_bindings.trap_sprite;
        renderer_config.fire_area_sprite = initial_sprite_bindings.fire_area_sprite;
        renderer_config.barrier_mode = config.barrier_mode;
        renderer_config.artifact_directory = config.artifact_directory;
        renderer_config.content_generation_id = render_generation->id;
        renderer_config.content_generation_lease = render_generation;
        renderer_config.product_paths = render_generation->product_paths;
        renderer_config.product_digests = render_generation->product_digests;

        if (initialized) initialized = renderer.Initialize(renderer_config);
        if (!initialized)
        {
            record_thread_failure(initialized);
            channels.render_ready.store(true, std::memory_order_release);
            channels.render_ready.notify_all();
            return;
        }
        // Render-thread-owned copies: record resolved execution state only at
        // successful commit boundaries, without changing persisted user intent.
        GraphicsSettings active_graphics = startup_graphics;
        SettingsData active_renderer_settings = initial_renderer_settings;
        GraphicsDisplayId active_native_output = initial_native_output;
        std::uint64_t active_settings_generation{};
        const auto restore_renderer_settings = [&] {
            try
            {
                GraphicsDisplayId restored_output;
                if (auto resolved = ResolveGraphicsDisplayOutput(window.Handle(),
                                                                 active_native_output,
                                                                 restored_output); !resolved)
                    return resolved;
                auto restored = renderer.Resize(active_renderer_settings.width, active_renderer_settings.height);
                if (restored) restored = renderer.RequestGraphicsSettings(
                    GraphicsSettingsRequest{active_settings_generation,
                        GraphicsSettingsRequestCause::Recovery, active_graphics, restored_output});
                if (restored) active_native_output = std::move(restored_output);
                return restored;
            }
            catch (const std::bad_alloc &)
            {
                return Result::Failure(ErrorCode::TaskFailed, "hs_runtime", "Graphics recovery allocation failed.");
            }
        };
        bool execution_receipt_pending{true};
        const auto record_renderer_features = [&](std::uint64_t settings_generation) {
            execution_receipt_pending = true;
            try
            {
                channels.renderer_graphics_ui.store(RendererGraphicsReceipt(renderer, active_graphics, active_settings_generation,
                    static_cast<HWND>(window.Handle()), config.validation || config.gpu_validation), std::memory_order_release);
                const auto lighting = renderer.GetLightingStatus();
                const auto reconstruction = renderer.GetReconstructionStatus();
                const auto cache = renderer.GetPipelineCacheStats();
                std::ofstream log(config.artifact_directory / "renderer_features.jsonl", std::ios::app);
                log << std::format(
                    "{{\"settings_generation\":{},\"pipeline_revision\":{},"
                    "\"requested\":{{\"ray_master\":{},\"ao\":\"{}\",\"gi\":\"{}\",\"reflection\":\"{}\",\"shadow\":\"{}\"}},"
                    "\"active\":{{\"ray_scene\":{},\"ao\":\"{}\",\"gi\":\"{}\",\"reflection\":\"{}\",\"shadow\":\"{}\"}},"
                    "\"fallback\":{{\"ao\":\"{}\",\"gi\":\"{}\",\"reflection\":\"{}\",\"shadow\":\"{}\"}},"
                    "\"hardware\":{{\"ray_scene\":{},\"inline_queries\":{}}},\"ray_scene_bytes\":{},"
                    "\"reconstruction\":{{\"requested\":\"{}\",\"active\":\"{}\",\"availability\":\"{}\"}},"
                    "\"pso_cache\":{{\"library\":{},\"mesh_library\":{},\"hits\":{},\"misses\":{},\"native_creates\":{},"
                    "\"store_failures\":{},\"load_failures\":{},\"io_failures\":{},\"discarded_files\":{}}}}}\n",
                    settings_generation, renderer.PipelineRevision(),
                    lighting.requested.ray_tracing_enabled, GraphicsEnumToken(lighting.requested.ao_mode),
                    GraphicsEnumToken(lighting.requested.gi_mode), GraphicsEnumToken(lighting.requested.reflection_mode),
                    GraphicsEnumToken(lighting.requested.shadow_mode),
                    lighting.active.ray_tracing_enabled, GraphicsEnumToken(lighting.active.ao_mode),
                    GraphicsEnumToken(lighting.active.gi_mode), GraphicsEnumToken(lighting.active.reflection_mode),
                    GraphicsEnumToken(lighting.active.shadow_mode),
                    lighting.ao_fallback_reason, lighting.gi_fallback_reason, lighting.reflection_fallback_reason,
                    lighting.shadow_fallback_reason, lighting.ray_scene_supported, lighting.inline_ray_queries_supported,
                    lighting.ray_scene_allocated_bytes, reconstruction.requested_provider.value,
                    reconstruction.effective_provider.value, reconstruction.requested_availability,
                    cache.library_supported, cache.mesh_library_supported, cache.hits, cache.misses, cache.native_creates,
                    cache.store_failures, cache.load_failures, cache.io_failures, cache.discarded_files);
            }
            catch (const std::bad_alloc &)
            {
                OutputDebugStringA("Graphics status receipt allocation failed; previous immutable receipt retained.\n");
            }
        };
        record_renderer_features(0);
        auto previous_ui_presentation = renderer.GetPresentationStatus();
        auto previous_receipt_phase = channels.session_phase.load(std::memory_order_acquire);
        std::uint64_t telemetry_completed_frame{};
        bool telemetry_settings_open{};
        channels.render_ready.store(true, std::memory_order_release);
        channels.render_ready.notify_all();

        auto active_frame_cap = initial_renderer_settings.frame_cap;
        auto active_decorative_particle_fraction = renderer.DecorativeParticleFraction();
        auto active_borderless = initial_renderer_settings.borderless;
        auto active_vfx_quality = renderer_config.vfx_quality;
        bool renderer_settings_suspended{};
#if defined(HS_DEVELOPMENT_TOOLS)
        std::string watched_generation_pointer;
        auto next_generation_check = std::chrono::steady_clock::now();
        std::ofstream(config.artifact_directory / "shader_hot_reload.log", std::ios::app)
            << "ready active=" << render_generation->id << '\n';
#endif

        RenderSnapshotExchange::Consumer snapshot_consumer(channels.snapshots);
        runtime_detail::VfxSessionState vfx_session;
        runtime_detail::VfxPersistentSparkState persistent_sparks;
        runtime_detail::VfxUpgradeMovementState vfx_upgrade_movements;
        auto &pending_particle_spawns = vfx_session.particle_spawns;
        auto &pending_effect_lines = vfx_session.effect_lines;
        auto &active_vfx_flashes = vfx_session.flashes;
        auto &active_vfx_ground_events = vfx_session.ground_events;
        auto &active_vfx_ribbon_events = vfx_session.ribbon_events;
        std::vector<std::string> event_lines;
        std::vector<std::string> timeline_lines;
        std::vector<std::string> gpu_lines;
        float previous_foot_phase = -1.0f;
        Tick previous_foot_tick{};
        event_lines.reserve(32);
        pending_particle_spawns.reserve(256);
        pending_effect_lines.reserve(64);
        timeline_lines.reserve(static_cast<std::size_t>(config.maximum_ticks) + 16);

        Tick last_tick{};
        std::uint64_t typed_vfx_events_received{};
        std::uint32_t typed_vfx_persistent_peak{};
        std::uint32_t typed_vfx_ground_peak{};
        std::uint64_t typed_vfx_ground_submitted_total{};
        std::uint32_t typed_vfx_flash_peak{};
        std::uint32_t typed_vfx_mesh_peak{};
        std::uint32_t typed_vfx_fresnel_peak{};
        std::uint32_t typed_vfx_ribbon_peak{};
        std::uint32_t typed_vfx_ribbon_dropped{};
        std::uint32_t typed_vfx_light_peak{};
        std::uint32_t typed_vfx_distortion_peak{};
        std::uint32_t typed_vfx_decal_peak{};
        std::uint32_t typed_vfx_volume_peak{};
        std::uint64_t static_scene_upload_count{};
        std::uint64_t vfx_catalog_generation{1};
        std::uint64_t typed_vfx_flash_submitted_total{};
        std::uint64_t typed_vfx_unsupported_inputs{};
        bool captured{};
        bool resized{};
        bool vfx_showcase_injected{};
        const auto log_resize_failure = [&](const Result &failure) {
            const auto message = std::format("Recoverable display resize failed: {}\n",
                                             failure.Message());
            OutputDebugStringA(message.c_str());
            std::ofstream(config.artifact_directory / "resize_errors.log", std::ios::app)
                << message;
        };
        auto next_frame = std::chrono::steady_clock::now();
        for (;;)
        {
#if defined(HS_DEVELOPMENT_TOOLS)
            if (std::chrono::steady_clock::now() >= next_generation_check)
            {
                next_generation_check = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                std::string pointer_text;
                if (ReadGenerationPointer(generation_pointer, pointer_text) &&
                    pointer_text != watched_generation_pointer)
                {
                    watched_generation_pointer = pointer_text;
                    SelectedGeneration candidate;
                    auto reloaded = Result::Success();
                    std::string candidate_id = "unresolved";
                    const char *stage = "manifest";
                    const auto pipeline_revision = renderer.PipelineRevision();
                    const auto parameter_revision = renderer.MaterialParameterRevision();
                    bool applied{}, catalog_changed{};
                    runtime_detail::VfxGenerationRebindCounts rebound;
                    std::uint64_t retired_legacy{}, retired_movements{}, retired_sparks{};
                    try
                    {
                        reloaded = LoadSelectedGeneration(generation_pointer, candidate);
                        if (!candidate.id.empty()) candidate_id = candidate.id;
                        if (reloaded && candidate.scope != render_generation->scope)
                            reloaded = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                                       "Content Generation changes the selected scope.");
                        if (reloaded && candidate.id != render_generation->id)
                        {
                            stage = "integrity";
                            reloaded = VerifySelectedGenerationProducts(candidate);
                            std::filesystem::path next_particle_path, next_program_path;
                            VfxCatalog replacement;
                            RendererVfxSpriteBindings sprite_bindings;
                            runtime_detail::VfxSessionState prepared_session;
                            runtime_detail::VfxUpgradeMovementState prepared_movements;
                            runtime_detail::VfxPersistentSparkState prepared_sparks;
                            if (reloaded)
                            {
                                stage = "vfx_paths";
                                reloaded = candidate.Require("vfx/particle_effects.hsbin", next_particle_path);
                                if (reloaded) reloaded = candidate.Require("vfx/vfx_program.hsbin", next_program_path);
                            }
                            if (reloaded)
                            {
                                for (const std::string id : {"vfx/particle_effects.hsbin", "vfx/vfx_program.hsbin"})
                                {
                                    const auto previous = render_generation->product_digests.find(id);
                                    const auto next = candidate.product_digests.find(id);
                                    if (previous == render_generation->product_digests.end() ||
                                        next == candidate.product_digests.end())
                                    {
                                        reloaded = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                                                   "Generation omits required VFX digest: " + id);
                                        break;
                                    }
                                    catalog_changed |= previous->second != next->second;
                                }
                            }
                            if (reloaded && catalog_changed)
                            {
                                stage = "vfx_catalog";
                                reloaded = VfxCatalog::Load(next_particle_path, next_program_path, replacement);
                                if (reloaded) reloaded = PrepareVfxSpriteBindings(replacement, sprite_bindings);
                                if (reloaded && vfx_catalog_generation == UINT64_MAX)
                                    reloaded = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                                               "VFX catalog generation exhausted.");
                            }
                            if (reloaded)
                            {
                                stage = "vfx_retention";
                                const auto &previous = vfx_catalog.Program();
                                const auto &next = catalog_changed ? replacement.Program() : previous;
                                prepared_session.entity_surface_captures = vfx_session.entity_surface_captures;
                                std::erase_if(prepared_session.entity_surface_captures, [&](auto &capture) {
                                    const auto captured_generation = std::static_pointer_cast<const SelectedGeneration>(
                                        capture.generation_lease);
                                    if (!captured_generation || !runtime_detail::SameCapturedAssetClosure(
                                            *captured_generation, candidate, capture.instance.asset_id))
                                    {
                                        rebound.Record(runtime_detail::VfxGenerationRebindOutcome::Incompatible);
                                        return true;
                                    }
                                    if (!catalog_changed) return false;
                                    const auto outcome = runtime_detail::RebindVfxGenerationHandle(
                                        previous, next, capture.input.effect_handle, 0u);
                                    rebound.Record(outcome);
                                    return outcome != runtime_detail::VfxGenerationRebindOutcome::Retained;
                                });
                                if (catalog_changed)
                                {
                                    prepared_session.ground_events = vfx_session.ground_events;
                                    prepared_session.ribbon_events = vfx_session.ribbon_events;
                                    prepared_session.light_events = vfx_session.light_events;
                                    prepared_session.distortion_events = vfx_session.distortion_events;
                                    prepared_session.decal_events = vfx_session.decal_events;
                                    prepared_session.volume_events = vfx_session.volume_events;
                                    prepared_session.screen_events = vfx_session.screen_events;
                                    prepared_session.entity_surface_events = vfx_session.entity_surface_events;
                                    prepared_session.point_rate_events = vfx_session.point_rate_events;
                                    for (auto *events : {&prepared_session.ground_events, &prepared_session.ribbon_events,
                                            &prepared_session.light_events, &prepared_session.distortion_events,
                                            &prepared_session.decal_events, &prepared_session.volume_events,
                                            &prepared_session.screen_events, &prepared_session.entity_surface_events,
                                            &prepared_session.point_rate_events})
                                        runtime_detail::RebindVfxGenerationEvents(previous, next, *events, rebound);
                                    const auto ground_count = prepared_session.ground_events.size();
                                    runtime_detail::RebindVfxGroundEventOwners(next, prepared_session.ground_events);
                                    const auto retired_ground = ground_count - prepared_session.ground_events.size();
                                    rebound.retained -= retired_ground;
                                    rebound.incompatible += retired_ground;
                                    prepared_movements = vfx_upgrade_movements;
                                    retired_movements = prepared_movements.Rebind(previous, next);
                                    prepared_sparks = persistent_sparks;
                                    retired_sparks = prepared_sparks.Rebind(previous, next);
                                    // Expanded legacy commands have no stable effect identity.
                                    // Empty prepared queues retire them at the same commit.
                                    retired_legacy = vfx_session.particle_spawns.size() +
                                        vfx_session.effect_lines.size() + vfx_session.flashes.size();
                                }
                            }
                            if (reloaded)
                            {
                                stage = "renderer_prepare";
                                auto next_generation = std::make_shared<const SelectedGeneration>(candidate);
                                reloaded = renderer.ReloadContentGeneration(candidate.id, candidate.product_paths,
                                    candidate.product_digests, next_generation, catalog_changed ? &sprite_bindings : nullptr);
                                if (reloaded)
                                {
                                    // All allocation, decoding and rebinding preceded the
                                    // renderer commit. Publish only prepared owned state.
                                    if (catalog_changed)
                                    {
                                        vfx_catalog.Swap(replacement);
                                        vfx_session.SwapDecoded(prepared_session);
                                        vfx_upgrade_movements.Swap(prepared_movements);
                                        persistent_sparks.Swap(prepared_sparks);
                                        ++vfx_catalog_generation;
                                    }
                                    else
                                        vfx_session.entity_surface_captures.swap(
                                            prepared_session.entity_surface_captures);
                                    cooked_particle_effects_path.swap(next_particle_path);
                                    cooked_vfx_program_path.swap(next_program_path);
                                    render_generation.swap(next_generation);
                                    active_decorative_particle_fraction = renderer.DecorativeParticleFraction();
                                    applied = true;
                                }
                            }
                        }
                    }
                    catch (const std::exception &error)
                    {
                        reloaded = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                                   "Content Generation preparation failed: " + std::string(error.what()));
                    }
                    if (applied)
                    {
                        record_renderer_features(0);
                        std::ofstream(config.artifact_directory / "shader_hot_reload.log", std::ios::app)
                            << "applied candidate=" << candidate_id << " active=" << render_generation->id
                            << " pipelines=" << pipeline_revision << "->" << renderer.PipelineRevision()
                            << " parameters=" << parameter_revision << "->" << renderer.MaterialParameterRevision()
                            << " vfx_changed=" << catalog_changed << " typed_retained=" << rebound.retained
                            << " typed_deleted=" << rebound.deleted << " typed_incompatible=" << rebound.incompatible
                            << " movements_retired=" << retired_movements << " sparks_retired=" << retired_sparks
                            << " expanded_legacy_retired=" << retired_legacy
                            << "; temporal/HZB history invalidated\n";
                    }
                    if (!reloaded)
                        std::ofstream(config.artifact_directory / "shader_hot_reload.log", std::ios::app)
                            << "rejected candidate=" << candidate_id << " active=" << render_generation->id
                            << " stage=" << stage << ' ' << reloaded.Message() << "; previous content retained\n";
                }
            }
#endif
            GraphicsSettingsCommitRequest renderer_settings;
            bool apply_renderer_settings{};
            while (channels.renderer_settings.TryPop(renderer_settings))
                apply_renderer_settings = true;
            NativeWindowMessage window_message;
            bool display_receipt_changed{};
            while (channels.window_messages.TryPop(window_message))
            {
                renderer.HandleWindowMessage(window_message);
                display_receipt_changed |= window_message.message == WM_DISPLAYCHANGE;
            }
            if (display_receipt_changed) record_renderer_features(0);
            ResizeCommand resize_command;
            std::uint32_t resize_width{};
            std::uint32_t resize_height{};
            while (channels.resize_commands.TryPop(resize_command))
            {
                resize_width = resize_command.width;
                resize_height = resize_command.height;
            }
            auto resize_error = ErrorCode::None;
            if (resize_width && resize_height)
            {
                if (auto resize_result = renderer.Resize(resize_width, resize_height);
                    !resize_result)
                {
                    if (resize_result.Code() != ErrorCode::TaskFailed)
                    {
                        record_thread_failure(resize_result);
                        break;
                    }
                    log_resize_failure(resize_result);
                    resize_error = resize_result.Code();
                }
                else if (!apply_renderer_settings)
                {
                    active_renderer_settings.width = resize_width;
                    active_renderer_settings.height = resize_height;
                    record_renderer_features(0);
                }
            }
            // WM_SIZE is enqueued synchronously by the window-thread mode change.
            // ApplyOptions may only acknowledge that change after Resize succeeds.
            if (apply_renderer_settings)
            {
                const auto &effective = renderer_settings.effective;
                bool display_target_changed{};
                if (resize_error == ErrorCode::None &&
                    effective.borderless != active_borderless)
                {
                    if (auto resize_result = renderer.Resize(effective.width,
                                                              effective.height);
                        !resize_result)
                    {
                        if (resize_result.Code() != ErrorCode::TaskFailed)
                        {
                            record_thread_failure(resize_result);
                            break;
                        }
                        log_resize_failure(resize_result);
                        resize_error = resize_result.Code();
                    }
                    else
                        active_borderless = effective.borderless;
                }
                auto settings_error = resize_error;
                if (settings_error == ErrorCode::None)
                {
                    try
                    {
                        auto next_graphics = renderer_settings.graphics ? *renderer_settings.graphics : active_graphics;
                        GraphicsDisplayId next_native_output;
                        display_target_changed = next_graphics.display_output.value != "auto" &&
                            next_graphics.display_output != active_native_output;
                        auto applied = ResolveGraphicsDisplayOutput(window.Handle(),
                                                                    next_graphics.display_output,
                                                                    next_native_output);
                        if (applied)
                        {
                            display_target_changed = next_native_output != active_native_output;
                            applied = renderer.RequestGraphicsSettings(
                                GraphicsSettingsRequest{renderer_settings.generation,
                                    renderer_settings.cause, next_graphics, next_native_output});
                        }
                        settings_error = applied.Code();
                        if (applied)
                        {
                            active_graphics = std::move(next_graphics);
                            active_native_output = std::move(next_native_output);
                            active_settings_generation = renderer_settings.generation;
                            active_renderer_settings = effective;
                            if (renderer_settings.cause == GraphicsSettingsRequestCause::Recovery)
                                renderer_settings_suspended = false;
                            active_frame_cap = effective.frame_cap;
                            active_decorative_particle_fraction = renderer.DecorativeParticleFraction();
                            active_vfx_quality = active_graphics.particle_quality == GraphicsQuality::Low
                                ? VfxQuality::Low
                                : active_graphics.particle_quality == GraphicsQuality::Medium
                                    ? VfxQuality::Medium : VfxQuality::High;
                            record_renderer_features(renderer_settings.generation);
                        }
                        else
                        {
                            // Resize and options are separate native transactions.
                            // A cross-monitor failure is acknowledged while the
                            // window thread rolls placement back. Recovery is
                            // submitted only after that placement completes.
                            if (display_target_changed)
                                renderer_settings_suspended = true;
                            else
                            {
                                auto restored = restore_renderer_settings();
                                if (!restored) { record_thread_failure(restored); break; }
                                active_borderless = active_renderer_settings.borderless;
                                record_renderer_features(renderer_settings.generation);
                            }
                        }
                    }
                    catch (const std::bad_alloc &)
                    {
                        settings_error = ErrorCode::TaskFailed;
                        if (display_target_changed)
                            renderer_settings_suspended = true;
                        else
                        {
                            auto restored = restore_renderer_settings();
                            if (!restored) { record_thread_failure(restored); break; }
                            active_borderless = active_renderer_settings.borderless;
                            record_renderer_features(renderer_settings.generation);
                        }
                    }
                }
                if (!channels.renderer_settings_acks.TryPush(
                        {renderer_settings.generation, settings_error}))
                {
                    record_thread_failure(Result::Failure(
                        ErrorCode::InvalidState, "hs_runtime",
                        "Renderer settings acknowledgment queue capacity exceeded."));
                    break;
                }
            }

            auto current_presentation = renderer.GetPresentationStatus();
            const bool native_latency = current_presentation.low_latency_active;
            if (current_presentation.effective_provider != previous_ui_presentation.effective_provider ||
                current_presentation.active_mode != previous_ui_presentation.active_mode ||
                current_presentation.maximum_multiplier != previous_ui_presentation.maximum_multiplier ||
                current_presentation.observed_multiplier != previous_ui_presentation.observed_multiplier ||
                current_presentation.low_latency_active != previous_ui_presentation.low_latency_active ||
                current_presentation.availability != previous_ui_presentation.availability ||
                current_presentation.fallback_reason != previous_ui_presentation.fallback_reason)
            {
                previous_ui_presentation = std::move(current_presentation);
                record_renderer_features(0);
            }
            const bool has_native_snapshot = native_latency && snapshot_consumer.AcquireLatest().has_current;
            if (native_latency && has_native_snapshot && config.smoke)
            {
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Native input latency requires actual window input; smoke playback has stopped that input source."));
                break;
            }
            native_input_frame.SetEnabled(native_latency && has_native_snapshot);
            if (native_latency && has_native_snapshot)
            {
                std::uint64_t token{};
                if (auto begun = renderer.BeginInputFrame(token); !begun) { record_thread_failure(begun); break; }
                if (!native_input_frame.RequestInput(token))
                {
                    if (!channels.stop_requested.load(std::memory_order_acquire))
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Native input frame request violated its acknowledged token order."));
                    break;
                }
                if (!native_input_frame.WaitInput(token)) break;
                if (auto sampled = renderer.InputSampled(token); !sampled) { record_thread_failure(sampled); break; }
                if (!native_input_frame.RequestSimulation(token))
                {
                    if (!channels.stop_requested.load(std::memory_order_acquire))
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Native simulation request did not match acknowledged input."));
                    break;
                }
                if (!native_input_frame.WaitSimulation(token)) break;
                if (auto simulated = renderer.SimulationCompleted(token); !simulated) { record_thread_failure(simulated); break; }
            }
            // AcquireLatest below observes the simulation's completed publication.
            std::size_t event_count{};
            PresentationEvent event;
            while (event_count < 256 &&
                   channels.presentation_events.TryPop(event))
            {
                ++event_count;
                if (event.kind == PresentationKind::Vfx)
                    vfx_session.pending_events.push_back(event);
                if (event.kind == PresentationKind::Audio &&
                    !channels.audio_events.TryPush(event))
                {
                    channels.dropped_presentation_events.fetch_add(1,
                                                                   std::memory_order_relaxed);
                }
                event_lines.push_back(
                    std::format("{{\"sequence\":{},\"tick\":{},\"kind\":{},"
                                "\"asset\":{},\"position\":[{},{},{}]}}\n",
                                event.sequence, event.tick, static_cast<unsigned>(event.kind),
                                event.asset.value, event.position.x, event.position.y,
                                event.position.z));
            }
            const auto snapshots = snapshot_consumer.AcquireLatest();
            if (snapshots.has_current &&
                (!config.smoke_render_lockstep ||
                 snapshots.current.header.tick > rendered_tick.load(std::memory_order_acquire)))
            {
                const auto &current_snapshot = snapshots.current;
                if (vfx_session.Synchronize(current_snapshot.header.session_id))
                {
                    persistent_sparks.Clear();
                    vfx_upgrade_movements.Clear();
                    previous_foot_phase = -1.0f;
                    previous_foot_tick = 0;
                    vfx_showcase_injected = false;
                }
                const auto ready_events = vfx_session.TakeReady(current_snapshot.header.tick);
                std::vector<PresentationEvent> frame_events;
                if (auto dispatched = runtime_detail::DispatchVfxUpgradeEvents(
                        vfx_catalog.Program(), ready_events, frame_events); !dispatched)
                {
                    record_thread_failure(dispatched);
                    break;
                }
                for (const auto &ready_event : frame_events)
                    if (auto expanded = vfx_catalog.ExpandEvent(
                            ready_event, active_decorative_particle_fraction,
                            pending_particle_spawns, pending_effect_lines); !expanded)
                    {
#if defined(HS_DEVELOPMENT_TOOLS)
                        record_thread_failure(expanded);
                        break;
#endif
                    }
                if (!current_snapshot.poses.empty() && !current_snapshot.instances.empty() &&
                    current_snapshot.header.tick != previous_foot_tick)
                {
                    const auto &pose = current_snapshot.poses.front();
                    const auto phase = pose.secondary_normalized_time -
                                       std::floor(pose.secondary_normalized_time);
                    const auto crossed = [&](float marker) {
                        return previous_foot_phase >= 0.0f &&
                               (previous_foot_phase < marker && phase >= marker ||
                                previous_foot_phase > phase &&
                                    (previous_foot_phase < marker || phase >= marker));
                    };
                    if (pose.secondary_weight > 0.01f &&
                        (crossed(0.0f) || crossed(0.5f)))
                    {
                        const auto cue = pose.secondary_weight >= 0.5f
                                             ? "audio.player.footstep_run"
                                             : "audio.player.footstep_walk";
                        auto event = MakeAudioEvent(cue, current_snapshot.header.tick);
                        event.tick = current_snapshot.header.tick;
                        event.position = current_snapshot.instances.front().position;
                        (void)channels.audio_events.TryPush(event);
                    }
                    previous_foot_phase = phase;
                    previous_foot_tick = current_snapshot.header.tick;
                }
                if (config.vfx_showcase && !vfx_showcase_injected)
                {
                    constexpr std::array colors{
                        Float4{0.35f, 0.75f, 2.5f, 0.9f},
                        Float4{1.8f, 0.25f, 2.2f, 0.9f},
                        Float4{3.0f, 2.4f, 0.5f, 1.0f},
                        Float4{0.4f, 2.2f, 3.0f, 0.9f},
                        Float4{0.65f, 0.72f, 0.8f, 0.8f},
                        Float4{3.2f, 0.55f, 0.08f, 0.95f},
                        Float4{1.8f, 0.02f, 0.03f, 0.95f},
                        Float4{0.4f, 1.6f, 2.8f, 0.95f},
                        Float4{2.8f, 2.8f, 3.0f, 1.0f},
                        Float4{0.3f, 1.2f, 3.0f, 0.9f},
                        Float4{0.7f, 1.8f, 3.0f, 0.9f}};
                    constexpr std::array primitives{
                        VfxPrimitive::Disc, VfxPrimitive::Ring, VfxPrimitive::Sector,
                        VfxPrimitive::Chevron, VfxPrimitive::Rune, VfxPrimitive::Cracks,
                        VfxPrimitive::Arrow, VfxPrimitive::Shard, VfxPrimitive::Ember,
                        VfxPrimitive::Spike, VfxPrimitive::ShockShell,
                        VfxPrimitive::SolidTrail, VfxPrimitive::DashedRicochet,
                        VfxPrimitive::FireTransfer, VfxPrimitive::RelicChain,
                        VfxPrimitive::DashWake};
                    for (std::size_t index = 0; index < primitives.size(); ++index)
                    {
                        ParticleSpawnCommand command;
                        command.sequence = 0xF000u + index;
                        command.tick = 0;
                        command.position = {
                            (static_cast<float>(index % 6) - 2.5f) * 3.2f,
                            index < 6 || index >= 11 ? 0.04f : 1.0f,
                            (static_cast<float>(index / 6) - 1.0f) * 4.0f};
                        command.direction = index >= 6 && index < 11
                                                ? Float3{0.5f, 0.35f, 1.0f}
                                                : Float3{0.0f, 1.0f, 0.0f};
                        command.shape = ParticleShape::Point;
                        command.velocity_mode = ParticleVelocity::Direction;
                        command.speed_min = command.speed_max =
                            index >= 6 && index < 11 ? 0.001f : 0.0f;
                        command.facing = index < 6 || index >= 11
                                             ? ParticleFacing::Ground
                                             : ParticleFacing::Velocity;
                        command.renderer = index < 6 ? VfxRenderer::Ground
                                           : index < 11 ? VfxRenderer::Mesh
                                                        : VfxRenderer::Segment;
                        command.primitive = primitives[index];
                        command.count = 1;
                        command.lifetime_min = command.lifetime_max = 10.0f;
                        command.start_color = command.end_color = colors[index % colors.size()];
                        command.start_size_min = command.start_size_max =
                            command.renderer == VfxRenderer::Segment ? 0.18f : 1.15f;
                        command.end_size_min = command.end_size_max =
                            command.start_size_min;
                        command.stretch = command.renderer == VfxRenderer::Segment ? 7.0f : 1.0f;
                        command.seed = static_cast<std::uint32_t>(index + 1);
                        pending_particle_spawns.push_back(command);
                    }
                    vfx_showcase_injected = true;
                }
                std::size_t effect_line_count{};
                while (effect_line_count < pending_effect_lines.size() &&
                       pending_effect_lines[effect_line_count].tick <=
                           snapshots.current.header.tick)
                    ++effect_line_count;
                if (config.resize_test && !resized &&
                    snapshots.current.header.tick >= config.maximum_ticks / 2)
                {
                    if (auto resize_result =
                            renderer.Resize(config.width + 64, config.height + 36);
                        !resize_result)
                    {
                        record_thread_failure(resize_result);
                        break;
                    }
                    resized = true;
                }
                RendererFrameResult frame;
                const DevToolsFrameData devtools{
                    static_cast<std::uint32_t>(task_system.WorkerCount()),
                    static_cast<std::uint32_t>(channels.action_edges.Size()),
                    static_cast<std::uint32_t>(channels.presentation_events.Size()),
                    0,
                    static_cast<std::uint32_t>(channels.resize_commands.Size()),
                    static_cast<std::uint32_t>(channels.debug_commands.Size()),
                    channels.dropped_input_edges.load(std::memory_order_relaxed),
                    channels.dropped_presentation_events.load(std::memory_order_relaxed),
                    0};
                const auto current_vfx = snapshots.has_current
                    ? snapshots.current.persistent_vfx
                    : std::span<const PersistentVfxVisual>{};
                const auto previous_vfx = snapshots.has_previous &&
                    snapshots.previous.header.session_id == snapshots.current.header.session_id
                    ? snapshots.previous.persistent_vfx
                    : std::span<const PersistentVfxVisual>{};
                auto typed_vfx = runtime_detail::BuildVfxTypedFrameInputs(
                    vfx_catalog.Program(), frame_events,
                    current_vfx, previous_vfx,
                    snapshots.has_current ? snapshots.current.header.tick : Tick{});
                const Tick surface_tick = snapshots.current.header.tick;
                const auto &surface_program = vfx_catalog.Program();
                const auto is_effect = [&](const VfxEventInput &input, std::string_view id) {
                    return std::ranges::any_of(surface_program.effect_lookup, [&](const auto &entry) {
                        return entry.handle == input.effect_handle && entry.effect_id == MakeAssetId(id).value;
                    });
                };
                for (const auto &input : typed_vfx.events)
                {
                    if (is_effect(input, "particle.boss.phase_change") || is_effect(input, "particle.boss.death"))
                        vfx_session.entity_surface_events.push_back(input);
                    if (is_effect(input, "particle.skill.trap")) vfx_session.point_rate_events.push_back(input);
                }
                const auto terminal_phase = static_cast<SessionPhase>(
                    channels.session_phase.load(std::memory_order_acquire));
                const Tick death_current_tick = vfx_session.UpdateTerminalClock(
                    surface_tick, terminal_phase, FixedStepClock::Clock::now());
                std::erase_if(vfx_session.entity_surface_captures, [&](const auto &capture) {
                    const auto &effect = surface_program.effects[capture.input.effect_handle - 1];
                    const Tick current_tick = is_effect(capture.input, "particle.boss.death")
                        ? death_current_tick : surface_tick;
                    return current_tick >= capture.input.event_tick &&
                        static_cast<double>(current_tick - capture.input.event_tick) >= static_cast<double>(effect.seconds) * 60.0;
                });
                std::erase_if(vfx_session.entity_surface_events, [&](const auto &input) {
                    const auto &effect = surface_program.effects[input.effect_handle - 1];
                    const Tick current_tick = is_effect(input, "particle.boss.death")
                        ? death_current_tick : surface_tick;
                    if (current_tick >= input.event_tick &&
                        static_cast<double>(current_tick - input.event_tick) >= static_cast<double>(effect.seconds) * 60.0) return true;
                    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
                    if (!context || context->owner_id == 0) return true;
                    runtime_detail::VfxEntitySurfaceEventCapture capture;
                    capture.input = input;
                    capture.generation_lease = render_generation;
                    if (is_effect(input, "particle.boss.death"))
                    {
                        const auto actual = std::ranges::find_if(snapshots.current.captured_instances, [&](const auto &value) {
                            return value.session_id == snapshots.current.header.session_id && value.sequence == input.sequence &&
                                value.tick == input.event_tick && value.source_entity_id == context->owner_id;
                        });
                        if (actual == snapshots.current.captured_instances.end()) return false;
                        capture.instance = actual->instance;
                        capture.animation_pose = actual->animation_pose;
                        capture.retained_instance = capture.has_animation_pose = true;
                    }
                    else
                    {
                        const auto actual = std::ranges::find_if(snapshots.current.instances, [&](const auto &value) {
                            return value.stable_id == ((2ull << 60) | context->owner_id);
                        });
                        if (actual == snapshots.current.instances.end()) return false;
                        capture.instance = *actual;
                    }
                    vfx_session.entity_surface_captures.push_back(std::move(capture));
                    return true;
                });
                std::vector<runtime_detail::VfxEntitySurfacePersistentCapture> surface_persistent;
                for (const auto &input : typed_vfx.persistent)
                {
                    if (input.source_visual_kind != static_cast<std::uint8_t>(PersistentVfxKind::BossPhaseTransition)) continue;
                    const auto *entity = std::get_if<VfxEntityPayload>(&input.payload);
                    if (!entity) continue;
                    const auto actual = std::ranges::find_if(snapshots.current.instances,
                        [&](const auto &value) { return value.stable_id == entity->render_instance_id; });
                    const auto visual = std::ranges::find_if(current_vfx,
                        [&](const auto &value) { return value.stable_id == input.stable_id && value.kind == PersistentVfxKind::BossPhaseTransition; });
                    if (actual != snapshots.current.instances.end() && visual != current_vfx.end())
                        surface_persistent.push_back({input, *actual, visual->active_tick, visual->expires});
                }
                const auto typed_surfaces = runtime_detail::BuildVfxTypedEntitySurfaceCommands(surface_program,
                    vfx_session.entity_surface_captures, surface_persistent, surface_tick,
                    death_current_tick);
                const auto movement_owners = vfx_upgrade_movements.Emit(vfx_catalog.Program(), frame_events,
                    snapshots.has_current ? snapshots.current.header.tick : Tick{});
                typed_vfx.persistent.insert(typed_vfx.persistent.end(), movement_owners.begin(), movement_owners.end());
                typed_vfx_unsupported_inputs += typed_vfx.unsupported_effect_ids.size();
                for (const auto &input : typed_vfx.events)
                {
                    const auto ballistic = runtime_detail::BuildVfxTypedBallisticCommands(
                        vfx_catalog.Program(), std::span(&input,1));
                    if (ballistic.empty()) continue;
                    const auto &lookup = vfx_catalog.Program().effect_lookup;
                    const bool replaces_legacy_burst_shell = std::ranges::any_of(lookup, [&](const auto &entry) {
                        return entry.handle == input.effect_handle &&
                            entry.effect_id == MakeAssetId("particle.skill.explosive_arrow.main").value;
                    });
                    for (auto &legacy : pending_particle_spawns)
                        if (!legacy.authored_ballistic && legacy.sequence == input.sequence && legacy.renderer == VfxRenderer::Mesh &&
                            (legacy.primitive != VfxPrimitive::ShockShell || replaces_legacy_burst_shell)) legacy.count = 0;
                    pending_particle_spawns.insert(pending_particle_spawns.end(), ballistic.begin(), ballistic.end());
                }
                std::vector<VfxPersistentInput> new_status_owners;
                for (const auto &owner : typed_vfx.persistent)
                {
                    const auto kind = static_cast<PersistentVfxKind>(owner.source_visual_kind);
                    if (kind != PersistentVfxKind::PlayerBowDraw &&
                        kind != PersistentVfxKind::ChargedFullReady &&
                        kind != PersistentVfxKind::EmpoweredReady &&
                        kind != PersistentVfxKind::MultishotRetarget &&
                        kind != PersistentVfxKind::RicochetBleedExtend)
                        continue;
                    if (vfx_session.MarkPersistentBurst(owner.stable_id,
                                                        owner.source_visual_kind))
                        new_status_owners.push_back(owner);
                }
                const auto status_shards = runtime_detail::BuildVfxPersistentStatusShardCommands(
                    vfx_catalog.Program(), new_status_owners,
                    snapshots.current.header.tick);
                pending_particle_spawns.insert(pending_particle_spawns.end(),
                                               status_shards.begin(), status_shards.end());
                auto ground_commands = runtime_detail::BuildVfxTypedGroundCommands(
                    vfx_catalog.Program(), typed_vfx.persistent,
                    snapshots.has_current ? snapshots.current.header.tick : Tick{});
                for (const auto &input : typed_vfx.events)
                {
                    const auto &program = vfx_catalog.Program();
                    if (input.effect_handle == 0 || input.effect_handle > program.effects.size()) continue;
                    const auto &effect = program.effects[input.effect_handle-1];
                    bool has_light = false;
                    bool has_distortion = false;
                    bool has_decal = false;
                    for (const auto &source : std::span(program.sources).subspan(effect.sources.first,effect.sources.count))
                        for (const auto &output : std::span(program.outputs).subspan(source.outputs.first,source.outputs.count))
                        {
                            has_light |= output.profile == VfxOutputProfile::Light;
                            has_distortion |= output.profile == VfxOutputProfile::Distortion;
                            has_decal |= output.profile == VfxOutputProfile::Decal;
                        }
                    if (has_light) vfx_session.light_events.push_back(input);
                    if (has_distortion) vfx_session.distortion_events.push_back(input);
                    if (has_decal) vfx_session.decal_events.push_back(input);
                    if (runtime_detail::IsVfxTypedVolumeEffect(program, input.effect_handle))
                        vfx_session.volume_events.push_back(input);
                    if (runtime_detail::IsVfxTypedScreenEffect(program, input.effect_handle))
                        vfx_session.screen_events.push_back(input);
                }
                const auto light_tick = current_snapshot.header.tick;
                std::erase_if(vfx_session.screen_events, [&](const VfxEventInput &input) {
                    const auto &effect = vfx_catalog.Program().effects[input.effect_handle - 1];
                    return light_tick >= input.event_tick &&
                        static_cast<double>(light_tick - input.event_tick) / 60.0 >= effect.seconds;
                });
                std::vector<VfxScreenInput> typed_screen;
                if (auto converted = runtime_detail::BuildVfxTypedScreenCommands(vfx_catalog.Program(),
                        vfx_session.screen_events, typed_vfx.persistent, light_tick,
                        active_vfx_quality, typed_screen); !converted)
                {
                    record_thread_failure(converted);
                    break;
                }
                std::erase_if(vfx_session.volume_events, [&](const VfxEventInput &input) {
                    const auto &effect = vfx_catalog.Program().effects[input.effect_handle - 1];
                    return light_tick >= input.event_tick &&
                        static_cast<double>(light_tick - input.event_tick) / 60.0 >= effect.seconds;
                });
                std::vector<VfxVolumeInput> typed_volumes;
                if (auto converted = runtime_detail::BuildVfxTypedVolumeCommands(vfx_catalog.Program(),
                        vfx_session.volume_events, typed_vfx.persistent, light_tick,
                        active_vfx_quality, typed_volumes); !converted)
                {
                    record_thread_failure(converted);
                    break;
                }
                runtime_detail::RefreshVfxGroundEventOwners(vfx_session.light_events,current_snapshot.persistent_vfx,light_tick);
                std::erase_if(vfx_session.light_events,[&](const VfxEventInput &input) {
                    if (input.geometry_owner_id != 0) return light_tick >= input.geometry_end_tick;
                    const auto &effect = vfx_catalog.Program().effects[input.effect_handle-1];
                    return effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0 ||
                        (light_tick >= input.event_tick && static_cast<double>(light_tick-input.event_tick)/60.0 >= effect.seconds);
                });
                const auto typed_lights=runtime_detail::BuildVfxTypedLightCommands(vfx_catalog.Program(),vfx_session.light_events,light_tick);
                runtime_detail::RefreshVfxGroundEventOwners(vfx_session.distortion_events,current_snapshot.persistent_vfx,light_tick);
                std::erase_if(vfx_session.distortion_events,[&](const VfxEventInput &input) {
                    if (input.geometry_owner_id != 0) return light_tick >= input.geometry_end_tick;
                    const auto &effect = vfx_catalog.Program().effects[input.effect_handle-1];
                    return effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0 ||
                        (light_tick >= input.event_tick && static_cast<double>(light_tick-input.event_tick)/60.0 >= effect.seconds);
                });
                const auto typed_distortions=runtime_detail::BuildVfxTypedDistortionCommands(
                    vfx_catalog.Program(),vfx_session.distortion_events,typed_vfx.persistent,light_tick);
                runtime_detail::RefreshVfxGroundEventOwners(vfx_session.decal_events,current_snapshot.persistent_vfx,light_tick);
                std::erase_if(vfx_session.decal_events,[&](const VfxEventInput &input) {
                    if (input.geometry_owner_id != 0) return light_tick >= input.geometry_end_tick;
                    const auto &effect = vfx_catalog.Program().effects[input.effect_handle-1];
                    return effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0 ||
                        (light_tick >= input.event_tick && static_cast<double>(light_tick-input.event_tick)/60.0 >= effect.seconds);
                });
                const auto typed_decals=runtime_detail::BuildVfxTypedDecalCommands(
                    vfx_catalog.Program(),vfx_session.decal_events,typed_vfx.persistent,light_tick);
                for (const auto &input : typed_vfx.events)
                {
                    const auto &program = vfx_catalog.Program();
                    if ((!std::holds_alternative<VfxPointPayload>(input.payload) &&
                         !std::holds_alternative<VfxContextPayload>(input.payload) &&
                         !std::holds_alternative<VfxProjectilePayload>(input.payload) &&
                         !std::holds_alternative<VfxCirclePayload>(input.payload) &&
                         !std::holds_alternative<VfxLinePayload>(input.payload) &&
                         !std::holds_alternative<VfxConePayload>(input.payload) &&
                         !std::holds_alternative<VfxRingGapsPayload>(input.payload) &&
                         !std::holds_alternative<VfxSafeSectorPayload>(input.payload)) ||
                        input.effect_handle == 0 || input.effect_handle > program.effects.size())
                        continue;
                    const auto &effect = program.effects[input.effect_handle - 1];
                    const bool charged_pulse =
                        std::holds_alternative<VfxContextPayload>(input.payload) &&
                        std::ranges::any_of(program.effect_lookup, [&](const auto &lookup) {
                            return lookup.handle == input.effect_handle &&
                                lookup.effect_id == MakeAssetId(
                                    "particle.skill.charged_shot.pulse").value;
                        });
                    const bool wave_start =
                        std::holds_alternative<VfxContextPayload>(input.payload) &&
                        std::ranges::any_of(program.effect_lookup, [&](const auto &lookup) {
                            return lookup.handle == input.effect_handle &&
                                lookup.effect_id == MakeAssetId("particle.session.wave_start").value;
                        });
                    const bool owned = input.geometry_owner_id != 0 &&
                        input.geometry_end_tick > input.geometry_start_tick;
                    if (effect.input_mode != 0 ||
                        (effect.timing_kind == 1 ? !owned :
                            (effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0.0f)))
                        continue;
                    if (std::holds_alternative<VfxPointPayload>(input.payload) ||
                        std::holds_alternative<VfxContextPayload>(input.payload) ||
                        std::holds_alternative<VfxProjectilePayload>(input.payload))
                    {
                        const auto source_first = static_cast<std::size_t>(effect.sources.first);
                        const auto source_count = static_cast<std::size_t>(effect.sources.count);
                        if (source_first > program.sources.size() ||
                            source_count > program.sources.size() - source_first) continue;
                        bool has_ground_ring = false;
                        for (std::size_t source_index = source_first;
                             source_index < source_first + source_count && !has_ground_ring; ++source_index)
                        {
                            const auto &source = program.sources[source_index];
                            const auto first = static_cast<std::size_t>(source.outputs.first);
                            const auto count = static_cast<std::size_t>(source.outputs.count);
                            if (source.type != VfxSourceType::Direct || first > program.outputs.size() ||
                                count > program.outputs.size() - first) continue;
                            for (std::size_t output_index = first; output_index < first + count; ++output_index)
                            {
                                const auto &output = program.outputs[output_index];
                                has_ground_ring |= output.profile == VfxOutputProfile::GroundSdfAdd ||
                                    ((charged_pulse || wave_start) && output.profile == VfxOutputProfile::GroundSdfSoft);
                            }
                        }
                        if (!has_ground_ring) continue;
                    }
                    if (charged_pulse)
                        std::erase_if(active_vfx_ground_events, [&](const VfxEventInput &older) {
                            return older.effect_handle == input.effect_handle &&
                                std::holds_alternative<VfxContextPayload>(older.payload) &&
                                std::get<VfxContextPayload>(older.payload).owner_id ==
                                    std::get<VfxContextPayload>(input.payload).owner_id;
                        });
                    active_vfx_ground_events.push_back(input);
                    const auto initial = runtime_detail::BuildVfxTypedEventGroundCommands(
                        program, std::span(&input, 1), input.event_tick);
                    if (charged_pulse && std::ranges::any_of(initial, [](const auto &command) {
                            return command.geometry.shape == VfxGroundShape::StateRing &&
                                command.geometry.rings == 1;
                        }))
                        for (auto &legacy : pending_particle_spawns)
                            if (legacy.sequence == input.sequence) legacy.count = 0;
                    // Replace only the corresponding legacy geometric layer.
                    // Other sources in this event retain their compatibility path.
                    for (const auto &authored : initial)
                        for (auto &legacy : pending_particle_spawns)
                            if (legacy.sequence == input.sequence &&
                                (((authored.geometry.shape == VfxGroundShape::Circle || authored.geometry.shape == VfxGroundShape::CirclePreviewBorder) &&
                                  legacy.renderer == VfxRenderer::Ground &&
                                  ((authored.command.primitive == VfxPrimitive::ExactRing &&
                                    legacy.primitive == VfxPrimitive::Ring) ||
                                   (authored.command.primitive == VfxPrimitive::LowFrequencyFill &&
                                    legacy.primitive == VfxPrimitive::Disc))) ||
                                 (authored.geometry.shape == VfxGroundShape::LineBorder &&
                                  legacy.renderer == VfxRenderer::Segment &&
                                  legacy.primitive == VfxPrimitive::SolidTrail)))
                                legacy.count = 0;
                }
                const Tick ground_tick = snapshots.current.header.tick;
                runtime_detail::RefreshVfxGroundEventOwners(active_vfx_ground_events,
                    snapshots.current.persistent_vfx, ground_tick);
                std::erase_if(active_vfx_ground_events, [&](const VfxEventInput &input) {
                    if (input.geometry_owner_id != 0)
                        return ground_tick >= input.geometry_end_tick;
                    const auto seconds = vfx_catalog.Program().effects[input.effect_handle - 1].seconds;
                    return ground_tick >= input.event_tick &&
                        static_cast<double>(ground_tick - input.event_tick) / 60.0 >= seconds;
                });
                auto event_ground = runtime_detail::BuildVfxTypedEventGroundCommands(
                    vfx_catalog.Program(), active_vfx_ground_events, ground_tick);
                ground_commands.insert(ground_commands.end(), event_ground.begin(), event_ground.end());
                const auto owned_status_ticks =
                    runtime_detail::BuildVfxOwnedStatusTickFlashCommands(
                        vfx_catalog.Program(), typed_vfx.events, typed_vfx.persistent);
                std::vector<VfxEventInput> unpaired_flash_events;
                unpaired_flash_events.reserve(typed_vfx.events.size());
                for (const auto &event : typed_vfx.events)
                {
                    const bool paired = std::ranges::any_of(owned_status_ticks,
                        [&](const auto &tick) { return tick.event_sequence == event.sequence; });
                    if (!paired) unpaired_flash_events.push_back(event);
                }
                for (const auto &tick : owned_status_ticks)
                {
                    for (auto &legacy : pending_particle_spawns)
                        if (legacy.sequence == tick.event_sequence) legacy.count = 0;
                    active_vfx_flashes.push_back(tick.flash);
                }
                const auto new_flashes = runtime_detail::BuildVfxTypedFlashCommands(
                    vfx_catalog.Program(), unpaired_flash_events);
                active_vfx_flashes.insert(active_vfx_flashes.end(),
                                          new_flashes.begin(), new_flashes.end());
                const auto owned_status_motes = runtime_detail::BuildVfxOwnedStatusTickMoteCommands(
                    vfx_catalog.Program(), typed_vfx.events, typed_vfx.persistent);
                std::vector<VfxEventInput> unpaired_mote_events;
                unpaired_mote_events.reserve(typed_vfx.events.size());
                for (const auto &event : typed_vfx.events)
                    if (!std::ranges::any_of(owned_status_motes,
                        [&](const auto &mote) { return mote.event_sequence == event.sequence; }))
                        unpaired_mote_events.push_back(event);
                for (const auto &mote : owned_status_motes) active_vfx_flashes.push_back(mote.flash);
                const auto new_motes = runtime_detail::BuildVfxTypedMoteCommands(
                    vfx_catalog.Program(), unpaired_mote_events);
                active_vfx_flashes.insert(active_vfx_flashes.end(), new_motes.begin(), new_motes.end());
                const auto born_sparks = persistent_sparks.Emit(
                    vfx_catalog.Program(), typed_vfx.persistent, ground_tick);
                active_vfx_flashes.insert(active_vfx_flashes.end(),
                                          born_sparks.begin(), born_sparks.end());
                const Tick flash_tick = snapshots.has_current
                    ? snapshots.current.header.tick : Tick{};
                std::vector<VfxFlashSpawnInput> visible_flashes;
                visible_flashes.reserve(active_vfx_flashes.size());
                std::erase_if(active_vfx_flashes, [&](const VfxFlashSpawnInput &flash) {
                    if (flash_tick < flash.event_tick) return false;
                    const auto elapsed = flash_tick >= flash.event_tick
                        ? std::chrono::duration<float>(FixedStepClock::kFixedStep).count() *
                              static_cast<float>(flash_tick - flash.event_tick)
                        : 0.0f;
                    if (elapsed >= flash.delay + flash.lifetime) return true;
                    if (elapsed < flash.delay) return false;
                    auto visible = flash;
                    visible.normalized_age =
                        (elapsed - flash.delay) / flash.lifetime;
                    visible_flashes.push_back(visible);
                    return false;
                });
                std::erase_if(vfx_session.point_rate_events, [&](const auto &input) {
                    return ground_tick >= input.event_tick && ground_tick - input.event_tick >= 39;
                });
                const auto point_motes = runtime_detail::BuildVfxOwnedMoteCommands(
                    vfx_catalog.Program(), vfx_session.point_rate_events, ground_tick);
                visible_flashes.insert(visible_flashes.end(), point_motes.begin(), point_motes.end());
                const auto owned_motes = runtime_detail::BuildVfxOwnedMoteCommands(
                    vfx_catalog.Program(), active_vfx_ground_events, ground_tick);
                visible_flashes.insert(visible_flashes.end(), owned_motes.begin(), owned_motes.end());
                const auto persistent_motes = runtime_detail::BuildVfxPersistentMoteCommands(
                    vfx_catalog.Program(), typed_vfx.persistent);
                visible_flashes.insert(visible_flashes.end(), persistent_motes.begin(),
                                       persistent_motes.end());
                const auto fire_flames = runtime_detail::BuildVfxPersistentFireFlameCommands(
                    vfx_catalog.Program(), typed_vfx.persistent, ground_tick);
                visible_flashes.insert(visible_flashes.end(), fire_flames.begin(), fire_flames.end());
                const auto status_stamps = runtime_detail::BuildVfxPersistentStatusStampCommands(
                    vfx_catalog.Program(), typed_vfx.persistent);
                visible_flashes.insert(visible_flashes.end(), status_stamps.begin(),
                                       status_stamps.end());
                std::vector<VfxGroundSpawnInput> typed_ground_spawns;
                typed_ground_spawns.reserve(ground_commands.size());
                for (const auto &source : ground_commands)
                    typed_ground_spawns.push_back(
                        {source.command, source.additive, source.stable_id, source.effect_handle,
                         source.source_visual_kind, source.motion, source.gradient_row, source.hdr, source.geometry});
                const auto typed_mesh_spawns = runtime_detail::BuildVfxTypedMeshCommands(
                    vfx_catalog.Program(), typed_vfx.persistent);
                const auto typed_fresnels = runtime_detail::BuildVfxTypedFresnelCommands(
                    vfx_catalog.Program(), typed_vfx.persistent);
                for (const auto &input : typed_vfx.events)
                {
                    const auto &program = vfx_catalog.Program();
                    if (input.effect_handle == 0 || input.effect_handle > program.effects.size()) continue;
                    const auto &effect = program.effects[input.effect_handle - 1];
                    if (effect.input_mode == 0 && effect.timing_kind == 0 &&
                        std::isfinite(effect.seconds) && effect.seconds > 0.0f &&
                        (std::holds_alternative<VfxPointPayload>(input.payload) ||
                         std::holds_alternative<VfxProjectilePayload>(input.payload) ||
                         std::holds_alternative<VfxConePayload>(input.payload) ||
                         std::holds_alternative<VfxCirclePayload>(input.payload) ||
                         std::holds_alternative<VfxEntityPayload>(input.payload) ||
                         std::holds_alternative<VfxContextPayload>(input.payload) ||
                         std::holds_alternative<VfxLinkPayload>(input.payload)))
                        active_vfx_ribbon_events.push_back(input);
                }
                std::erase_if(active_vfx_ribbon_events, [&](const VfxEventInput &input) {
                    const auto seconds = vfx_catalog.Program().effects[input.effect_handle - 1].seconds;
                    return ground_tick >= input.event_tick &&
                        static_cast<double>(ground_tick - input.event_tick) / 60.0 >= seconds;
                });
                auto ribbon_sources = runtime_detail::BuildVfxTypedRibbonInputs(
                    vfx_catalog.Program(), typed_vfx.persistent);
                auto event_ribbons = runtime_detail::BuildVfxEventRibbonInputs(
                    vfx_catalog.Program(), active_vfx_ribbon_events, ground_tick);
                ribbon_sources.insert(ribbon_sources.end(), std::make_move_iterator(event_ribbons.begin()),
                                      std::make_move_iterator(event_ribbons.end()));
                const VfxRibbonFrameInput typed_ribbons{ribbon_sources, vfx_catalog_generation};
                // Authored source delays can cross later event deliveries. Keep the ready prefix chronological.
                std::stable_sort(pending_particle_spawns.begin(), pending_particle_spawns.end(),
                    [](const auto &a, const auto &b) { return a.tick < b.tick; });
                std::size_t particle_spawn_count{};
                while (particle_spawn_count < pending_particle_spawns.size() &&
                       pending_particle_spawns[particle_spawn_count].tick <= snapshots.current.header.tick)
                    ++particle_spawn_count;
                if (renderer_settings_suspended)
                {
                    std::this_thread::yield();
                    continue;
                }
                const auto frame_start = std::chrono::steady_clock::now();
                const std::array views{renderer.MainViewInput(snapshots.current.camera)};
                const RenderFrameInput render_input{
                    .views = views,
                    .snapshots = snapshots,
                    .events = frame_events,
                    .vfx = {
                        .particle_spawns = std::span(pending_particle_spawns.data(), particle_spawn_count),
                        .effect_lines = std::span(pending_effect_lines.data(), effect_line_count),
                        .events = typed_vfx.events,
                        .persistent = typed_vfx.persistent,
                        .ground = typed_ground_spawns,
                        .flashes = visible_flashes,
                        .meshes = typed_mesh_spawns,
                        .fresnels = typed_fresnels,
                        .ribbons = typed_ribbons,
                        .lights = typed_lights,
                        .distortions = typed_distortions,
                        .decals = typed_decals,
                        .volumes = typed_volumes,
                        .screen = typed_screen,
                        .entity_surfaces = typed_surfaces,
                    },
                    .devtools = devtools,
                    // Lockstep captures advance presentation by the same fixed
                    // step as simulation, independent of GPU/driver wall time.
                    .delta_seconds = config.smoke_render_lockstep
                        ? std::optional<float>{
                            std::chrono::duration<float>(FixedStepClock::kFixedStep).count()}
                        : std::nullopt,
                    .scene = snapshots.current.static_scene
                        ? std::optional<RenderSceneRef>{RenderSceneRef{
                            snapshots.current.static_scene->scene_asset_id,
                            snapshots.current.static_scene->content_generation_id,
                            snapshots.current.static_scene}}
                        : std::nullopt,
                    .screen_overlay_intensity = active_renderer_settings.screen_overlay_intensity,
                    .wait_for_streaming = config.smoke_render_lockstep,
                };
                if (auto rendered = renderer.Render(render_input, frame);
                    !rendered)
                {
                    record_thread_failure(rendered);
                    break;
                }
                if (config.smoke_render_lockstep)
                    rendered_tick.store(snapshots.current.header.tick, std::memory_order_release);
                const auto receipt_phase = channels.session_phase.load(std::memory_order_acquire);
                if (execution_receipt_pending || receipt_phase != previous_receipt_phase)
                {
                    record_renderer_features(0);
                    execution_receipt_pending = false;
                    previous_receipt_phase = receipt_phase;
                }
                const auto settings_page = static_cast<UiPage>(channels.ui_page.load(std::memory_order_acquire));
                const auto settings_phase = static_cast<SessionPhase>(receipt_phase);
                const bool settings_open = (settings_phase == SessionPhase::MainMenu && settings_page == UiPage::MainMenuSettings) ||
                                           (settings_phase == SessionPhase::Paused && settings_page == UiPage::PauseSettings);
                if (settings_open && !telemetry_settings_open) record_renderer_features(0);
                telemetry_settings_open = settings_open;
                if (settings_open)
                {
                    try
                    {
                        const auto diagnostics = renderer.GetDiagnostics();
                        const auto completed = CompletedPresentingFrame(diagnostics);
                        const auto receipt = channels.renderer_graphics_ui.load(std::memory_order_acquire);
                        if (receipt && completed > telemetry_completed_frame)
                        {
                            auto updated = std::make_shared<GraphicsSettingsUiModel>(*receipt);
                            UpdateGraphicsTelemetry(*updated, diagnostics, config.validation || config.gpu_validation);
                            channels.renderer_graphics_ui.store(std::move(updated), std::memory_order_release);
                            telemetry_completed_frame = completed;
                        }
                    }
                    catch (const std::bad_alloc &)
                    {
                        OutputDebugStringA("Settings telemetry allocation failed; previous immutable receipt retained.\n");
                    }
                }
                typed_vfx_events_received += frame.typed_vfx_event_count;
                typed_vfx_persistent_peak = std::max(
                    typed_vfx_persistent_peak, frame.typed_vfx_persistent_count);
                typed_vfx_ground_peak = std::max(
                    typed_vfx_ground_peak, frame.typed_vfx_ground_count);
                typed_vfx_ground_submitted_total += frame.typed_vfx_ground_count;
                typed_vfx_flash_peak = std::max(
                    typed_vfx_flash_peak, frame.typed_vfx_flash_count);
                typed_vfx_flash_submitted_total += frame.typed_vfx_flash_count;
                typed_vfx_mesh_peak = std::max(typed_vfx_mesh_peak, frame.typed_vfx_mesh_count);
                typed_vfx_fresnel_peak = std::max(typed_vfx_fresnel_peak, frame.typed_vfx_fresnel_count);
                typed_vfx_ribbon_peak = std::max(typed_vfx_ribbon_peak, frame.typed_vfx_ribbon_count);
                typed_vfx_ribbon_dropped += frame.typed_vfx_ribbon_dropped;
                typed_vfx_light_peak = std::max(typed_vfx_light_peak,frame.typed_vfx_light_count);
                typed_vfx_distortion_peak = std::max(typed_vfx_distortion_peak,frame.typed_vfx_distortion_count);
                typed_vfx_decal_peak = std::max(typed_vfx_decal_peak,frame.typed_vfx_decal_count);
                typed_vfx_volume_peak = std::max(typed_vfx_volume_peak,frame.typed_vfx_volume_count);
                static_scene_upload_count = frame.static_scene_upload_count;
                pending_particle_spawns.erase(
                    pending_particle_spawns.begin(),
                    pending_particle_spawns.begin() + particle_spawn_count);
                pending_effect_lines.erase(pending_effect_lines.begin(),
                                           pending_effect_lines.begin() + effect_line_count);
                const auto frame_microseconds =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - frame_start)
                        .count();
                channels.rendered_frames.store(frame.frame, std::memory_order_release);
                channels.devtools_capture_mouse.store(frame.capture_mouse,
                                                       std::memory_order_release);
                channels.devtools_capture_keyboard.store(frame.capture_keyboard,
                                                          std::memory_order_release);
                if (frame.debug_command != 0)
                {
                    (void)channels.debug_commands.TryPush(
                        {static_cast<DebugCommandKind>(frame.debug_command - 1),
                         frame.debug_value, frame.debug_secondary, {}});
                }
                last_tick = frame.rendered_tick;
                timeline_lines.push_back(
                    std::format("{},{},{},{},{},{}\n", frame.frame, last_tick,
                                frame_microseconds, CurrentProcessWorkingSetBytes(),
                                snapshots.current.instances.size() +
                                    (snapshots.current.static_scene
                                         ? snapshots.current.static_scene->instances.size() : 0),
                                devtools.input_queue_depth +
                                    devtools.presentation_queue_depth +
                                    devtools.particle_queue_depth +
                                    devtools.resize_queue_depth +
                                    devtools.debug_queue_depth));
                if (active_frame_cap != 0 && !config.smoke && !renderer.UsesNativeFrameLimiter())
                {
                    next_frame += std::chrono::nanoseconds(
                        1'000'000'000ull / std::clamp(active_frame_cap, 1u, 1000u));
                    std::this_thread::sleep_until(next_frame);
                    if (std::chrono::steady_clock::now() > next_frame +
                                                                   std::chrono::milliseconds(100))
                    {
                        next_frame = std::chrono::steady_clock::now();
                    }
                }

            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            // Completion can arrive after the terminal Render call. Capture
            // that frame without rendering the same snapshot a second time.
            if (config.smoke && snapshots.has_current && !captured &&
                channels.simulation_done.load(std::memory_order_acquire) &&
                last_tick >= channels.completed_tick.load(std::memory_order_acquire))
            {
                if (auto capture = renderer.Capture({.path = config.artifact_directory /
                        std::format("capture_tick_{}.png", last_tick)});
                    !capture)
                {
                    record_thread_failure(capture);
                    break;
                }
                if (config.smoke_render_lockstep)
                {
                    if (auto surface = renderer.Capture({
                            .path = config.artifact_directory / "surface.json",
                            .kind = CaptureKind::SurfaceDiagnostics}); !surface)
                    {
                        record_thread_failure(surface);
                        break;
                    }
                    if (auto hdr = renderer.Capture({
                            .path = config.artifact_directory / "scene_hdr.dds",
                            .kind = CaptureKind::HdrColor}); !hdr)
                    {
                        record_thread_failure(hdr);
                        break;
                    }
                }
                channels.rendered_particles.store(renderer.LastParticleCount(),
                                                  std::memory_order_release);
                const auto gpu_timings = renderer.LastGpuPassTimings();
                if (gpu_timings.valid)
                {
                    for (const auto &pass : gpu_timings.passes)
                    {
                        gpu_lines.push_back(std::format(
                            "{},{},{},{},{},{}\n",
                            channels.rendered_frames.load(std::memory_order_acquire),
                            last_tick, pass.name, pass.nanoseconds,
                            static_cast<unsigned>(pass.queue), pass.measured ? 1 : 0));
                    }
                }
                captured = true;
            }

            if (config.smoke && captured)
            {
                channels.stop_requested.store(true, std::memory_order_release);
                native_input_frame.Cancel();
            }
            if (channels.stop_requested.load(std::memory_order_acquire) &&
                channels.simulation_done.load(std::memory_order_acquire) &&
                channels.presentation_events.Size() == 0)
            {
                break;
            }
        }

        native_input_frame.Cancel();
        const auto enhanced = renderer.UsesEnhancedBarriers();
        if (auto shutdown = renderer.Shutdown(); !shutdown)
        {
            record_thread_failure(shutdown);
        }
        const auto cache_stats = renderer.GetPipelineCacheStats();
        WriteText(config.artifact_directory / "pipeline_cache.json", std::format(
            "{{\"hits\":{},\"misses\":{},\"native_creates\":{},\"stores\":{},\"store_failures\":{},"
            "\"load_failures\":{},\"io_failures\":{},\"discarded_files\":{},\"unsupported_libraries\":{},"
            "\"uncached_mesh_creates\":{},\"root_registrations\":{},\"loaded_bytes\":{},\"serialized_bytes\":{},"
            "\"last_cache_hresult\":{},\"last_io_error\":{}}}\n",
            cache_stats.hits, cache_stats.misses, cache_stats.native_creates, cache_stats.stores,
            cache_stats.store_failures, cache_stats.load_failures, cache_stats.io_failures, cache_stats.discarded_files,
            cache_stats.unsupported_libraries, cache_stats.uncached_mesh_creates, cache_stats.root_registrations,
            cache_stats.loaded_bytes, cache_stats.serialized_bytes, cache_stats.last_cache_hresult, cache_stats.last_io_error));
        const auto validation_errors = renderer.ValidationErrorCount();
        if (validation_errors != 0)
        {
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                        std::format("{} validation errors.", validation_errors)));
        }

        std::string timeline = "frame,tick,cpu_us,memory_bytes,ecs_count,queue_depth\n";
        for (const auto &line : timeline_lines)
        {
            timeline += line;
        }
        WriteText(config.artifact_directory / "timeline.csv", timeline);
        std::string gpu_timeline = "frame,tick,pass,gpu_ns,queue,measured\n";
        for (const auto &line : gpu_lines)
        {
            gpu_timeline += line;
        }
        WriteText(config.artifact_directory / "gpu_passes.csv", gpu_timeline);

        std::string events;
        for (const auto &line : event_lines)
        {
            events += line;
        }
        WriteText(config.artifact_directory / "events.ndjson", events);
        WriteText(config.artifact_directory / "render.json",
                  std::format("{{\"enhanced_barriers\":{},\"validation_errors\":{},"
                              "\"last_tick\":{},\"typed_vfx_events_received\":{},"
                              "\"typed_vfx_persistent_peak\":{},"
                              "\"typed_vfx_ground_peak\":{},"
                              "\"typed_vfx_ground_submitted_total\":{},"
                              "\"typed_vfx_flash_peak\":{},"
                              "\"typed_vfx_flash_submitted_total\":{},"
                              "\"typed_vfx_mesh_peak\":{},"
                              "\"typed_vfx_fresnel_peak\":{},"
                              "\"typed_vfx_ribbon_peak\":{},"
                              "\"typed_vfx_ribbon_dropped\":{},"
                              "\"typed_vfx_light_peak\":{},"
                              "\"typed_vfx_distortion_peak\":{},"
                              "\"typed_vfx_decal_peak\":{},"
                              "\"typed_vfx_volume_peak\":{},"
                              "\"static_scene_upload_count\":{},"
                              "\"typed_vfx_unsupported_inputs\":{}}}\n",
                              enhanced ? "true" : "false", validation_errors, last_tick,
                              typed_vfx_events_received, typed_vfx_persistent_peak,
                              typed_vfx_ground_peak, typed_vfx_ground_submitted_total,
                              typed_vfx_flash_peak, typed_vfx_flash_submitted_total,
                              typed_vfx_mesh_peak, typed_vfx_fresnel_peak,
                              typed_vfx_ribbon_peak, typed_vfx_ribbon_dropped,
                              typed_vfx_light_peak, typed_vfx_distortion_peak, typed_vfx_decal_peak,
                              typed_vfx_volume_peak,
                              static_scene_upload_count,
                              typed_vfx_unsupported_inputs));
    });

    channels.render_ready.wait(false, std::memory_order_acquire);
    {
        std::lock_guard lock(result_mutex);
        if (!thread_result)
        {
            render_thread.join();
            return {thread_result};
        }
    }

    auto simulation_ports = channels.ForSimulation();
    const SettingsData initial_projection_settings = settings;
    std::jthread simulation_thread([&, simulation_ports, initial_projection_settings](std::stop_token) mutable {
        auto &channels = simulation_ports;
        SetThreadName(L"HS Simulation");
        GameSimulation simulation;
        SettingsData projection_settings = initial_projection_settings;
        if (auto initialized = simulation.Initialize(
                {effective_seed, config.smoke, !config.smoke},
                simulation_rules);
            !initialized)
        {
            record_thread_failure(initialized);
            channels.simulation_done.store(true, std::memory_order_release);
            return;
        }

        const auto captured_skill =
            config.skill_vfx_capture < kCombatSkillCount
                ? static_cast<SkillKind>(config.skill_vfx_capture)
                : SkillKind::Count;
        if (captured_skill != SkillKind::Count)
        {
            if (captured_skill != SkillKind::BasicAttack)
            {
                if (auto granted = simulation.ApplyDebugCommand(
                        {DebugCommandKind::GrantSkill,
                         static_cast<std::uint64_t>(captured_skill)});
                    !granted)
                {
                    record_thread_failure(granted);
                    channels.simulation_done.store(true, std::memory_order_release);
                    return;
                }
            }
            for (std::uint32_t upgrade = 0; upgrade < kUpgradeCount; ++upgrade)
            {
                if ((config.skill_vfx_upgrade_mask & (1u << upgrade)) == 0) continue;
                if (auto granted = simulation.ApplyDebugCommand(
                        {DebugCommandKind::GrantUpgrade,
                         static_cast<std::uint64_t>(captured_skill), upgrade});
                    !granted)
                {
                    record_thread_failure(granted);
                    channels.simulation_done.store(true, std::memory_order_release);
                    return;
                }
            }
            constexpr std::array enemy_positions{
                Float2{-2.0f, 5.0f}, Float2{0.0f, 5.0f}, Float2{2.0f, 5.0f},
                Float2{-3.0f, 7.0f}, Float2{-1.0f, 7.0f}, Float2{1.0f, 7.0f},
                Float2{3.0f, 7.0f},  Float2{-2.0f, 9.0f}, Float2{0.0f, 9.0f},
                Float2{2.0f, 9.0f},  Float2{-1.0f, 11.0f}, Float2{1.0f, 11.0f}};
            for (const auto position : enemy_positions)
            {
                if (auto spawned = simulation.ApplyDebugCommand(
                        {DebugCommandKind::SpawnEnemy, 0, 0, position});
                    !spawned)
                {
                    record_thread_failure(spawned);
                    channels.simulation_done.store(true, std::memory_order_release);
                    return;
                }
            }
        }
        if (config.monster_preview_asset < 6)
        {
            const auto boss = config.monster_preview_asset >= 3;
            if (auto spawned = simulation.ApplyDebugCommand(
                    {boss ? DebugCommandKind::SpawnBoss : DebugCommandKind::SpawnEnemy,
                     boss ? config.monster_preview_asset - 3 : config.monster_preview_asset,
                     0, {0.0f, 0.0f}});
                !spawned)
            {
                record_thread_failure(spawned);
                channels.simulation_done.store(true, std::memory_order_release);
                return;
            }
        }
        if (config.boss_phase2_capture || config.boss_vfx_capture < 3)
        {
            if (auto spawned = simulation.ApplyDebugCommand(
                    {DebugCommandKind::SpawnBoss,
                     config.boss_phase2_capture ? 2u : config.boss_vfx_capture});
                !spawned)
            {
                record_thread_failure(spawned);
                channels.simulation_done.store(true, std::memory_order_release);
                return;
            }
        }

        FixedStepClock clock;
        auto now = FixedStepClock::Clock::now();
        (void)clock.Advance(now);
        std::uint64_t observed_focus_epoch{};
        Sequence publish_sequence{};
        std::array<ActionEdge, 256> action_storage{};
        std::array<UiAction, 64> ui_action_storage{};
        auto previous_phase = config.smoke ? SessionPhase::Playing : SessionPhase::MainMenu;
        GameReadModelStorage read_model;
        EnemyAnimationState enemy_animations;
        StaticEnvironmentSceneCache environment_cache(selected_generation.id);
        RenderSceneProjectionState scene_projection;
        std::size_t next_timeline_action{};
        std::size_t replay_frame_index{};
        bool smoke_finished{};
        if (!config.heartbeat_path.empty())
        {
            WriteText(config.heartbeat_path, "{\"tick\":0,\"state\":\"running\"}\n");
        }

        while (!channels.stop_requested.load(std::memory_order_acquire))
        {
            const auto native_token = native_input_frame.AwaitSimulation();
            if (channels.stop_requested.load(std::memory_order_acquire)) break;
            std::uint32_t tick_count = 1;
            if (!config.smoke)
            {
                now = FixedStepClock::Clock::now();
                tick_count = clock.Advance(now).tick_count;
                if (tick_count == 0)
                {
                    if (native_token) { native_input_frame.Simulated(native_token); continue; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }

            for (std::uint32_t local_tick = 0;
                 local_tick < tick_count &&
                 !smoke_finished;
                 ++local_tick)
            {
                SimulationRules updated_game_data;
                while (channels.simulation_rules_updates.TryPop(updated_game_data))
                {
                    if (auto applied = simulation.ApplySimulationRules(updated_game_data); !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
                SettingsData updated_presentation_settings;
                while (channels.presentation_settings.TryPop(
                    updated_presentation_settings))
                    projection_settings = updated_presentation_settings;
                std::size_t ui_action_count{};
                UiAction ui_action;
                while (ui_action_count < ui_action_storage.size() &&
                       channels.ui_actions.TryPop(ui_action))
                    ui_action_storage[ui_action_count++] = ui_action;
                const auto focus_epoch = channels.focus_epoch.load(std::memory_order_acquire);
                if (focus_epoch != observed_focus_epoch)
                {
                    channels.action_edges.ClearConsumer();
                    observed_focus_epoch = focus_epoch;
                }

                std::size_t action_count{};
                ActionEdge action;
                while (action_count < action_storage.size() &&
                       channels.action_edges.TryPop(action))
                {
                    const auto current_phase = static_cast<SessionPhase>(
                        channels.session_phase.load(std::memory_order_acquire));
                    const auto ui_page = static_cast<UiPage>(
                        channels.ui_page.load(std::memory_order_acquire));
                    if (action.kind == EdgeKind::Pressed &&
                        action.action == GameAction::Pause &&
                        ((current_phase == SessionPhase::Paused &&
                          ui_page == UiPage::PauseSettings) ||
                         (current_phase == SessionPhase::MainMenu &&
                          ui_page != UiPage::Root)))
                    {
                        channels.ui_page.store(static_cast<std::uint8_t>(UiPage::Root),
                                               std::memory_order_release);
                        continue;
                    }
                    if (action.kind == EdgeKind::Pressed &&
                        action.action == GameAction::CharacterPage)
                    {
                        channels.ui_page.store(
                            static_cast<std::uint8_t>(
                                current_phase == SessionPhase::Playing
                                    ? UiPage::CharacterOverview
                                    : UiPage::Root),
                            std::memory_order_release);
                        channels.loadout_source.store(0xFF, std::memory_order_release);
                    }
                    action_storage[action_count++] = action;
                }

                InputFrame input;
                if (replaying)
                {
                    if (replay_frame_index >= replay.frames.size())
                    {
                        channels.stop_requested.store(true, std::memory_order_release);
                        native_input_frame.Cancel();
                        break;
                    }
                    input = replay.frames[replay_frame_index].View();
                }
                else
                {
                    input.target_tick =
                        channels.completed_tick.load(std::memory_order_relaxed) + 1;
                    input.held = channels.held_input.load(std::memory_order_acquire);
                    input.ordered_edges = std::span(action_storage.data(), action_count);
                    input.ui_actions =
                        std::span(ui_action_storage.data(), ui_action_count);
                }
                if (captured_skill != SkillKind::Count && !replaying)
                {
                    input.held.aim_world = {config.skill_vfx_aim.x, 0.0f,
                                            config.skill_vfx_aim.y};
                    input.held.basic_attack_held =
                        captured_skill == SkillKind::BasicAttack && input.target_tick >= 5;
                    const auto add_edge = [&](GameAction action, EdgeKind kind) {
                        action_storage[action_count++] =
                            {static_cast<Sequence>(0xF000u + input.target_tick), action, kind};
                        input.ordered_edges =
                            std::span(action_storage.data(), action_count);
                    };
                    if (captured_skill != SkillKind::BasicAttack && input.target_tick == 5)
                        add_edge(GameAction::SkillQ, EdgeKind::Pressed);
                    if (captured_skill == SkillKind::ChargedShot && input.target_tick == 65)
                        add_edge(GameAction::SkillQ, EdgeKind::Released);
                }
                if (next_timeline_action < config.timeline_actions.size() &&
                    config.timeline_actions[next_timeline_action].target_tick < input.target_tick)
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                                "Timeline action missed its target tick."));
                    break;
                }
                while (next_timeline_action < config.timeline_actions.size() &&
                       config.timeline_actions[next_timeline_action].target_tick ==
                           input.target_tick)
                {
                    const auto &scheduled = config.timeline_actions[next_timeline_action++];
                    if (auto applied = simulation.ApplyDebugCommand(
                            {static_cast<DebugCommandKind>(scheduled.kind), scheduled.value,
                             scheduled.secondary, scheduled.position});
                        !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
                if (config.boss_phase2_capture && config.smoke && !replaying &&
                    input.target_tick == 10)
                {
                    if (auto damaged = simulation.ApplyDebugCommand(
                            {DebugCommandKind::DamageFinalBoss, 2250}); !damaged)
                    {
                        record_thread_failure(damaged);
                        break;
                    }
                }
                if (channels.stop_requested.load(std::memory_order_acquire))
                    break;
                DebugCommand debug_command;
                while (channels.debug_commands.TryPop(debug_command))
                {
                    if (auto applied = simulation.ApplyDebugCommand(debug_command); !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
                const auto tick = simulation.TickFixed(input, FixedStepClock::kFixedStep);
                simulation.WriteReadModel(read_model);
                const auto model = read_model.View();
                enemy_animations.Update(model, simulation.PendingDomainSignals());
                const auto &probe = model.session;
                channels.PublishSessionProbe(probe);
                if (replaying && !config.replay_compare && tick.checksum !=
                                     replay.frames[replay_frame_index].expected_checksum)
                {
                    record_thread_failure(Result::Failure(
                        ErrorCode::InvalidState, "hs_playtest",
                        std::format("Replay checksum mismatch at tick {}.", tick.tick)));
                    break;
                }
                if (playtest_recorder.Active())
                {
                    const auto diagnostics = simulation.GetDiagnostics();
                    SimulationObservation recorded_probe;
                    static_cast<SessionProbe &>(recorded_probe) = probe;
                    recorded_probe.balance = diagnostics.balance;
                    if (auto recorded = playtest_recorder.Record(
                            input, tick.checksum, recorded_probe,
                            [&] {
                                std::vector<PresentationEvent> events;
                                events.reserve(simulation.PendingDomainSignals().size() * 2);
                                for (const auto &signal : simulation.PendingDomainSignals())
                                {
                                    std::array<PresentationEvent, 3> projected{};
                                    const auto count = ProjectDomainSignal(signal, projected);
                                    events.insert(events.end(), projected.begin(),
                                                  projected.begin() + count);
                                }
                                return events;
                            }()); !recorded)
                    {
                        record_thread_failure(recorded);
                        break;
                    }
                }
                if (replaying) ++replay_frame_index;
                channels.camera_target_x.store(probe.player_position.x,
                                               std::memory_order_release);
                channels.camera_target_z.store(probe.player_position.y,
                                               std::memory_order_release);
                channels.session_phase.store(static_cast<std::uint8_t>(tick.phase),
                                             std::memory_order_release);
                if ((tick.phase == SessionPhase::Victory ||
                     tick.phase == SessionPhase::Defeat) &&
                    tick.phase != previous_phase)
                {
                    channels.completed_run_kills.fetch_add(probe.kills,
                                                           std::memory_order_relaxed);
                    if (tick.phase == SessionPhase::Victory)
                    {
                        channels.completed_run_wins.fetch_add(1,
                                                              std::memory_order_relaxed);
                    }
                    auto best = channels.best_level.load(std::memory_order_relaxed);
                    while (best < probe.level &&
                           !channels.best_level.compare_exchange_weak(
                               best, probe.level, std::memory_order_relaxed))
                    {
                    }
                }
                previous_phase = tick.phase;

                for (const auto &signal : simulation.PendingDomainSignals())
                {
                    std::array<PresentationEvent, 3> projected{};
                    const auto count = ProjectDomainSignal(signal, projected);
                    for (const auto &presentation :
                         std::span(projected).first(count))
                        if (!channels.presentation_events.TryPush(presentation))
                            channels.dropped_presentation_events.fetch_add(
                                1, std::memory_order_relaxed);
                }
                simulation.ClearDomainSignals();
                const bool final_smoke_frame =
                    config.smoke &&
                    (tick.tick >= config.maximum_ticks ||
                     tick.phase == SessionPhase::Victory ||
                     tick.phase == SessionPhase::Defeat);
                auto slot = channels.snapshots.TryBeginWrite();
                // The terminal snapshot must reach the renderer.  Waiting
                // here only applies to exchange backpressure for that final
                // frame; it never advances the simulation another tick.
                while (!slot && final_smoke_frame &&
                       !channels.stop_requested.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                    slot = channels.snapshots.TryBeginWrite();
                }
                if (slot)
                {
                    const auto graphics_ui = channels.presentation_graphics_ui.load(std::memory_order_acquire);
                    slot->storage->camera.yaw_degrees = presentation_catalog.camera.yaw_degrees;
                    const auto zoom_percent = static_cast<float>(channels.camera_zoom_percent.load(std::memory_order_acquire));
                    const auto pose = ComputeRuntimeCameraPose(presentation_catalog.camera.distance_m, presentation_catalog.camera.pitch_degrees, zoom_percent);
                    slot->storage->camera.pitch_degrees = pose.pitch_degrees;
                    slot->storage->camera.vertical_fov_degrees =
                        presentation_catalog.camera.vertical_fov_degrees;
                    slot->storage->camera.distance = pose.distance_m;
                    if (ProjectRenderSnapshot(read_model.View(), presentation_catalog,
                                              {static_cast<UiPage>(channels.ui_page.load(
                                                   std::memory_order_acquire)),
                                               channels.collection_skill.load(std::memory_order_acquire),
                                               channels.character_skill.load(std::memory_order_acquire),
                                               channels.loadout_source.load(std::memory_order_acquire),
                                               channels.settings_group.load(std::memory_order_acquire),
                                               channels.settings_page.load(std::memory_order_acquire)},
                                              projection_settings,
                                              *slot->storage,
                                               channels.pending_rebind_slot.load(
                                                   std::memory_order_acquire),
                                               &enemy_animations,
                                               &environment_cache,
                                               &scene_projection, graphics_ui.get()))
                    {
                        const auto page = static_cast<UiPage>(channels.ui_page.load(
                            std::memory_order_acquire));
                        if (page == UiPage::MainMenuSettings ||
                            page == UiPage::PauseSettings)
                        {
                            const auto modal = display_modal_phase.load(
                                std::memory_order_acquire);
                            if (modal != GraphicsUiPhase::Applied)
                            {
                                UiModel backdrop;
                                backdrop.kind = UiModel::Kind::Panel;
                                backdrop.anchor_pixels = {550, 360};
                                backdrop.size_pixels = {820, 360};
                                backdrop.color_rgba = 0xF0202430u;
                                const auto title = SettingsStatusText(
                                    modal == GraphicsUiPhase::AwaitingDisplayConfirmation
                                        ? "화면 모드를 유지하시겠습니까? (15초 안에 확인)"
                                        : modal == GraphicsUiPhase::RestoringDisplay
                                            ? "이전 화면 모드로 복원 중..."
                                            : "화면 모드 적용 중...", {620, 440});
                                bool modal_complete = slot->storage->AddUi(backdrop) &&
                                    slot->storage->AddUi(title);
                                if (modal == GraphicsUiPhase::AwaitingDisplayConfirmation)
                                    modal_complete = modal_complete &&
                                        slot->storage->AddUi(SettingsConfirmButton("유지", {650, 600})) &&
                                        slot->storage->AddUi(SettingsConfirmButton("취소", {1010, 600}));
                                if (!modal_complete)
                                {
                                    scene_projection.Abort();
                                    channels.snapshots.Abandon(*slot);
                                    record_thread_failure(Result::Failure(
                                        ErrorCode::InvalidState, "hs_runtime",
                                        "Display confirmation UI capacity exceeded."));
                                    break;
                                }
                            }
                        }
                        slot->storage->camera.target.y = pose.target_height_m;
                        channels.snapshots.Publish(*slot, ++publish_sequence);
                        scene_projection.Commit();
                    }
                    else
                    {
                        channels.snapshots.Abandon(*slot);
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_gameplay",
                                                    "Render snapshot capacity exceeded."));
                        break;
                    }
                }
                else if (final_smoke_frame || config.smoke_render_lockstep)
                {
                    record_thread_failure(Result::Failure(
                        ErrorCode::InvalidState, "hs_gameplay",
                        "Smoke render snapshot could not be published."));
                    break;
                }
                channels.completed_tick.store(tick.tick, std::memory_order_release);
                channels.gameplay_checksum.store(tick.checksum, std::memory_order_release);
                if (config.smoke_render_lockstep && !final_smoke_frame)
                {
                    while (rendered_tick.load(std::memory_order_acquire) < tick.tick &&
                           !channels.stop_requested.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    if (channels.stop_requested.load(std::memory_order_acquire))
                        break;
                }
                if (!config.heartbeat_path.empty() && tick.tick % 60 == 0)
                {
                    WriteText(config.heartbeat_path,
                              std::format("{{\"tick\":{},\"state\":\"running\"}}\n",
                                          tick.tick));
                }

                if (tick.phase == SessionPhase::QuitRequested)
                {
                    channels.stop_requested.store(true, std::memory_order_release);
                    native_input_frame.Cancel();
                    break;
                }

                // TickFixed does not advance a terminal gameplay clock.
                if (final_smoke_frame)
                {
                    smoke_finished = true;
                    break;
                }
            }

            if (native_token) native_input_frame.Simulated(native_token);
            if (smoke_finished)
            {
                break;
            }
        }
        if (auto shutdown = simulation.Shutdown(); !shutdown)
        {
            record_thread_failure(shutdown);
        }
        if (!config.heartbeat_path.empty())
        {
            WriteText(config.heartbeat_path,
                      std::format("{{\"tick\":{},\"state\":\"finished\"}}\n",
                                  channels.completed_tick.load(std::memory_order_acquire)));
        }
        native_input_frame.Cancel();
        channels.simulation_done.store(true, std::memory_order_release);
    });

#if defined(HS_DEVELOPMENT_TOOLS)
    std::error_code hot_reload_error;
    auto observed_cooked_write =
        std::filesystem::last_write_time(cooked_simulation_rules_path, hot_reload_error);
    auto next_hot_reload_check = std::chrono::steady_clock::now();
#endif

    SessionPhase previous_audio_phase = SessionPhase::MainMenu;
    bool display_start_deferred{};
    bool display_previous_borderless{};
    GraphicsSettings display_previous_graphics = window_display_graphics;
    GraphicsExtent display_previous_windowed_extent = windowed_client_extent;
    GraphicsDisplayId display_previous_native_output;
    bool display_restoring{};
    std::uint64_t display_generation{};
    auto display_confirmation_deadline = std::chrono::steady_clock::time_point{};
    const auto capture_display_placement = [&] {
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromWindow(window.Handle(), MONITOR_DEFAULTTONEAREST), &monitor))
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Cannot capture the current display for rollback.");
        std::vector<RendererDisplayOutput> displays;
        if (auto enumerated = EnumerateRendererDisplays(displays); !enumerated) return enumerated;
        const auto selected = std::ranges::find_if(displays, [&](const auto &output) {
            return output.left == monitor.rcMonitor.left && output.top == monitor.rcMonitor.top &&
                static_cast<std::int64_t>(output.left) + output.width == monitor.rcMonitor.right &&
                static_cast<std::int64_t>(output.top) + output.height == monitor.rcMonitor.bottom;
        });
        if (selected == displays.end())
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "The current window display is not exposed by DXGI.");
        display_previous_native_output.value = selected->id;
        display_previous_borderless = window_borderless;
        display_previous_graphics = window_display_graphics;
        display_previous_graphics.display_mode = window_borderless
            ? GraphicsDisplayMode::BorderlessFullscreen : GraphicsDisplayMode::Windowed;
        display_previous_windowed_extent = windowed_client_extent;
        return Result::Success();
    };
    const auto set_desired_client_extent = [&] {
        RECT client{};
        if (!GetClientRect(window.Handle(), &client))
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Cannot query display confirmation client extent.");
        if (client.right <= client.left || client.bottom <= client.top)
            return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                   "Display confirmation client extent is empty.");
        settings_commit.SetDesiredOutputExtent(
            static_cast<std::uint32_t>(client.right - client.left),
            static_cast<std::uint32_t>(client.bottom - client.top));
        return Result::Success();
    };
    const auto rollback_window_placement = [&] {
        try
        {
            // Requested Auto remains Auto in the document, but rollback must restore
            // the monitor captured before the move, not the new current monitor.
            GraphicsDisplayId restored_output;
            if (auto resolved = ResolveGraphicsDisplayOutput(window.Handle(),
                                                             display_previous_native_output,
                                                             restored_output); !resolved)
            {
                record_thread_failure(resolved);
                return false;
            }
            auto restored_graphics = display_previous_graphics;
            auto restored_tracking = restored_output;
            if (auto restored = window.ApplyDisplayPlacement(restored_output,
                    display_previous_borderless, display_previous_windowed_extent.width,
                    display_previous_windowed_extent.height); !restored)
            {
                record_thread_failure(restored);
                return false;
            }
            window_display_graphics = std::move(restored_graphics);
            window_native_output = std::move(restored_tracking);
            windowed_client_extent = display_previous_windowed_extent;
            window_borderless = display_previous_borderless;
            if (auto extent = set_desired_client_extent(); !extent)
            {
                record_thread_failure(extent);
                return false;
            }
            return true;
        }
        catch (const std::bad_alloc &)
        {
            record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime",
                "Display recovery allocation failed."));
            return false;
        }
    };
    const auto restore_display_mode = [&] {
        if (!rollback_window_placement()) return false;
        settings_commit.RollbackUnpersisted();
        display_restoring = true;
        display_generation = 0;
        display_modal_phase.store(GraphicsUiPhase::RestoringDisplay,
                                  std::memory_order_release);
        graphics_ui_phase.store(GraphicsUiPhase::RestoringDisplay,
                                 std::memory_order_release);
        return true;
    };
    std::shared_ptr<const GraphicsSettingsUiModel> last_renderer_receipt;
    auto last_published_phase = GraphicsUiPhase::Applied;
    bool last_published_pending{}, last_published_save_failure{};
    std::optional<GraphicsSettingsAck> deferred_settings_ack;
    auto settings_request_deadline = std::chrono::steady_clock::time_point{};
    while (!channels.stop_requested.load(std::memory_order_acquire))
    {
        ui_click_settings = settings_commit.UiInput();
        std::uint64_t sampled_token{};
        if (native_input_frame.ShouldPump(sampled_token) && !window.PumpMessages())
        {
            channels.stop_requested.store(true, std::memory_order_release);
            native_input_frame.Cancel();
            break;
        }
        if (sampled_token) native_input_frame.Sampled(sampled_token);
        if (ui_command_queue_full)
        {
            record_thread_failure(Result::Failure(
                ErrorCode::InvalidState, "hs_runtime",
                "UI settings command queue capacity exceeded."));
            ui_command_queue_full = false;
        }
        audio.SetBackgroundMuted(GetForegroundWindow() != window.Handle());
#if defined(HS_DEVELOPMENT_TOOLS)
        if (std::chrono::steady_clock::now() >= next_hot_reload_check)
        {
            next_hot_reload_check = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(250);
            hot_reload_error.clear();
            const auto write =
                std::filesystem::last_write_time(cooked_simulation_rules_path, hot_reload_error);
            if (!hot_reload_error && write != observed_cooked_write)
            {
                observed_cooked_write = write;
                std::ofstream log(config.artifact_directory / "hot_reload.log",
                                  std::ios::app);
                if (static_cast<SessionPhase>(channels.session_phase.load(
                        std::memory_order_acquire)) != SessionPhase::MainMenu)
                {
                    log << "rejected active_gameplay\n";
                }
                else
                {
                    SimulationRules replacement;
                    std::uint64_t replacement_hash{};
                    if (auto loaded = LoadSimulationRules(cooked_simulation_rules_path,
                                                          replacement,
                                                          &replacement_hash);
                        !loaded)
                    {
                        log << "rejected validation " << loaded.Message() << '\n';
                    }
                    else if (!channels.simulation_rules_updates.TryPush(replacement))
                    {
                        log << "rejected queue_full\n";
                    }
                    else
                    {
                        content_hash = replacement_hash;
                        simulation_rules_hash = SimulationRulesHash(replacement);
                        log << "applied content_hash=" << replacement_hash << '\n';
                    }
                }
            }
        }
#endif
        bool presentation_settings_changed = std::exchange(ui_click_allocation_failed, false);
        GraphicsSettingsAck settings_ack;
        while (deferred_settings_ack || channels.renderer_settings_acks.TryPop(settings_ack))
        {
            if (deferred_settings_ack) settings_ack = *deferred_settings_ack;
            const auto display_ack = display_generation != 0 &&
                                     settings_ack.generation == display_generation;
            const auto recovery_ack = settings_commit.IsRecovering();
            GraphicsSettingsCommit::AckOutcome outcome;
            try { outcome = settings_commit.Acknowledge(settings_ack); }
            catch (const std::bad_alloc &)
            {
                // The renderer already finished reading this packet. Retain its
                // receipt and ownership until a complete document can be restored.
                deferred_settings_ack = settings_ack;
                settings_commit.MarkTimedOut();
                graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                break;
            }
            deferred_settings_ack.reset();
            if (outcome == GraphicsSettingsCommit::AckOutcome::Ignored)
                continue;
            presentation_settings_changed = true;
            if (outcome == GraphicsSettingsCommit::AckOutcome::Recovering)
            {
                if (display_ack) { display_generation = 0; (void)rollback_window_placement(); }
                graphics_ui_phase.store(GraphicsUiPhase::RestoringDisplay, std::memory_order_release);
                continue;
            }
            if (outcome == GraphicsSettingsCommit::AckOutcome::Restored)
            {
                display_generation = 0;
                display_restoring = false;
                display_modal_phase.store(GraphicsUiPhase::Applied, std::memory_order_release);
                graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                continue;
            }
            if (outcome == GraphicsSettingsCommit::AckOutcome::Rejected && recovery_ack)
            {
                record_thread_failure(Result::Failure(settings_ack.error, "hs_runtime",
                    "Renderer could not restore the last persisted graphics profile."));
                break;
            }
            if (outcome == GraphicsSettingsCommit::AckOutcome::Applied)
            {
                if (display_ack)
                {
                    display_generation = 0;
                    if (display_restoring)
                    {
                        display_restoring = false;
                        display_modal_phase.store(GraphicsUiPhase::Applied,
                                                  std::memory_order_release);
                        graphics_ui_phase.store(GraphicsUiPhase::Applied,
                                                std::memory_order_release);
                        save_committed_settings();
                    }
                    else
                    {
                        display_confirmation_deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(15);
                        display_modal_phase.store(
                            GraphicsUiPhase::AwaitingDisplayConfirmation,
                            std::memory_order_release);
                        graphics_ui_phase.store(
                            GraphicsUiPhase::AwaitingDisplayConfirmation,
                            std::memory_order_release);
                    }
                }
                else if (const auto modal = display_modal_phase.load(
                             std::memory_order_acquire);
                         modal == GraphicsUiPhase::Applied ||
                         (modal == GraphicsUiPhase::Pending && display_start_deferred))
                {
                    graphics_ui_phase.store(GraphicsUiPhase::Applied,
                                            std::memory_order_release);
                    // This ack belongs to the earlier graphics request. The
                    // deferred display switch has not touched the window yet.
                    save_committed_settings();
                }
            }
            else if (display_ack)
            {
                display_generation = 0;
                if (display_restoring)
                {
                    display_restoring = false;
                    display_modal_phase.store(GraphicsUiPhase::Applied,
                                              std::memory_order_release);
                    graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed,
                                            std::memory_order_release);
                }
                else
                    (void)restore_display_mode();
            }
            else if (display_modal_phase.load(std::memory_order_acquire) ==
                     GraphicsUiPhase::Applied)
                graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed,
                                        std::memory_order_release);
        }
        UiCommand ui_command;
        bool immediate_settings_changed{};
        bool audio_settings_changed{};
        while (channels.ui_commands.TryPop(ui_command))
        {
            try
            {
                switch (ui_command.kind)
                {
                case UiCommandKind::SetBorderless:
                    if (display_modal_phase.load(std::memory_order_acquire) ==
                        GraphicsUiPhase::Applied)
                    {
                        if (auto captured = capture_display_placement(); !captured)
                        {
                            OutputDebugStringA(std::format("Display placement capture failed: {}\n",
                                                          captured.Message()).c_str());
                            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                            presentation_settings_changed = true;
                            break;
                        }
                        display_start_deferred = true;
                        display_modal_phase.store(GraphicsUiPhase::Pending,
                                                  std::memory_order_release);
                        settings_commit.EditGraphics(ui_command);
                        graphics_ui_phase.store(GraphicsUiPhase::Requested,
                                                std::memory_order_release);
                        presentation_settings_changed = true;
                    }
                    break;
                case UiCommandKind::SetVsync:
                case UiCommandKind::SetFrameCap:
                case UiCommandKind::SetRenderScale:
                case UiCommandKind::SetShadowResolution:
                case UiCommandKind::SetParticlePercentage:
                case UiCommandKind::SetBloom:
                case UiCommandKind::SetOutline:
                case UiCommandKind::SetGraphicsSetting:
                case UiCommandKind::SetVfxScreenOverlayIntensityPercent:
                    graphics_ui_phase.store(settings_commit.EditGraphics(ui_command)
                        ? GraphicsUiPhase::Requested : GraphicsUiPhase::ApplyFailed,
                                            std::memory_order_release);
                    presentation_settings_changed = true;
                    break;
                case UiCommandKind::SetMasterVolumePercent:
                case UiCommandKind::SetBgmVolumePercent:
                case UiCommandKind::SetSfxVolumePercent:
                case UiCommandKind::SetUiVolumePercent:
                    if (settings_commit.EditImmediate(ui_command))
                    {
                        immediate_settings_changed = true;
                        audio_settings_changed = true;
                        presentation_settings_changed = true;
                    }
                    break;
                case UiCommandKind::BeginSkillRebind:
                    window.BeginSkillRebind(ui_command.value);
                    channels.pending_rebind_slot.store(
                        static_cast<std::uint8_t>(ui_command.value),
                        std::memory_order_release);
                    break;
                case UiCommandKind::CancelSkillRebind:
                    window.CancelSkillRebind();
                    channels.pending_rebind_slot.store(0xFF, std::memory_order_release);
                    break;
                }
            }
            catch (const std::bad_alloc &)
            {
                graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                presentation_settings_changed = true;
            }
        }
        if (immediate_settings_changed)
        {
            if (audio_settings_changed)
                audio.ApplySettings(settings);
            if (display_modal_phase.load(std::memory_order_acquire) ==
                GraphicsUiPhase::Applied)
                save_committed_settings();
        }
        std::array<std::uint16_t, 4> rebound_keys;
        if (window.ConsumeReboundSkillKeys(rebound_keys))
        {
            audio.Play(MakeAudioEvent("audio.ui.rebind_success", ++ui_audio_sequence));
            channels.pending_rebind_slot.store(0xFF, std::memory_order_release);
            settings_commit.RebindKeys(rebound_keys);
            presentation_settings_changed = true;
            if (display_modal_phase.load(std::memory_order_acquire) ==
                GraphicsUiPhase::Applied)
                save_committed_settings();
        }
        if (display_modal_phase.load(std::memory_order_acquire) ==
            GraphicsUiPhase::AwaitingDisplayConfirmation)
        {
            const auto expired = std::chrono::steady_clock::now() >=
                                 display_confirmation_deadline;
            if (display_confirm_clicked && !expired)
            {
                display_modal_phase.store(GraphicsUiPhase::Applied,
                                          std::memory_order_release);
                graphics_ui_phase.store(GraphicsUiPhase::Applied,
                                        std::memory_order_release);
                save_committed_settings();
            }
            else if (display_cancel_clicked || expired)
            {
                (void)restore_display_mode();
                presentation_settings_changed = true;
            }
        }
        display_confirm_clicked = false;
        display_cancel_clicked = false;
        if (settings_persistence_rollback_requested && !settings_commit.HasPending())
        {
            settings_commit.RollbackUnpersisted();
            settings_persistence_rollback_requested = false;
            if (window_display_graphics.display_output != settings_commit.ApplyTargetGraphics().display_output ||
                window_display_graphics.output_extent != settings_commit.ApplyTargetGraphics().output_extent ||
                window_borderless != settings_commit.ApplyTargetEffective().borderless)
                (void)rollback_window_placement();
            graphics_ui_phase.store(GraphicsUiPhase::RestoringDisplay, std::memory_order_release);
            presentation_settings_changed = true;
        }
        if (settings_commit.HasPending() && !settings_commit.TimedOut() &&
            std::chrono::steady_clock::now() >= settings_request_deadline)
        {
            settings_commit.MarkTimedOut();
            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
            presentation_settings_changed = true;
        }
        if (display_start_deferred && !settings_commit.HasPending())
        {
            const auto target = settings_commit.UiInput().borderless;
            display_start_deferred = false;
            if (target == display_previous_borderless)
                display_modal_phase.store(GraphicsUiPhase::Applied,
                                          std::memory_order_release);
            presentation_settings_changed = true;
        }
        if (!settings_commit.HasPending() && settings_commit.HasUnsentEdits() && !settings_commit.PreparationFailed())
        {
            try
            {
                const auto &requested = settings_commit.ApplyTargetGraphics();
                const auto target_borderless = settings_commit.ApplyTargetEffective().borderless;
                GraphicsDisplayId native_output;
                const auto resolved_output = ResolveGraphicsDisplayOutput(window.Handle(),
                                                                          requested.display_output,
                                                                          native_output);
                if (!resolved_output)
                {
                    settings_commit.DiscardUnsentEdits();
                    graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                    presentation_settings_changed = true;
                }
                if (requested.display_output != window_display_graphics.display_output ||
                    requested.output_extent != window_display_graphics.output_extent ||
                    target_borderless != window_borderless || (resolved_output && native_output != window_native_output))
                {
                    if (display_modal_phase.load(std::memory_order_acquire) == GraphicsUiPhase::Applied)
                    {
                        if (auto captured = capture_display_placement(); !captured)
                        {
                            OutputDebugStringA(std::format("Display placement capture failed: {}\n",
                                                          captured.Message()).c_str());
                            settings_commit.DiscardUnsentEdits();
                            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                            presentation_settings_changed = true;
                        }
                        else
                            display_modal_phase.store(GraphicsUiPhase::Pending, std::memory_order_release);
                    }
                    if (settings_commit.HasUnsentEdits())
                    {
                        auto next_window_graphics = requested;
                        auto next_native_output = native_output;
                        const auto target_extent = requested.output_extent.value_or(windowed_client_extent);
                        auto placed = resolved_output;
                        if (placed) placed = window.ApplyDisplayPlacement(native_output, target_borderless,
                                                                         target_extent.width, target_extent.height);
                        if (placed) placed = set_desired_client_extent();
                        if (!placed)
                        {
                            OutputDebugStringA(std::format("Display placement failed: {}\n", placed.Message()).c_str());
                            (void)rollback_window_placement();
                            settings_commit.DiscardUnsentEdits();
                            display_modal_phase.store(GraphicsUiPhase::Applied, std::memory_order_release);
                            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                        }
                        else
                        {
                            window_display_graphics = std::move(next_window_graphics);
                            window_native_output = std::move(next_native_output);
                            windowed_client_extent = target_extent;
                            window_borderless = target_borderless;
                        }
                    }
                    presentation_settings_changed = true;
                }
            }
            catch (const std::bad_alloc &)
            {
                settings_commit.SuspendUnsentAfterAllocationFailure();
                const auto modal = display_modal_phase.load(std::memory_order_acquire);
                if (modal == GraphicsUiPhase::Pending || modal == GraphicsUiPhase::RestoringDisplay)
                {
                    (void)rollback_window_placement();
                    display_modal_phase.store(GraphicsUiPhase::Applied, std::memory_order_release);
                }
                graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
                presentation_settings_changed = true;
            }
        }
        if (presentation_settings_changed &&
            !channels.presentation_settings.TryPush(settings_commit.UiInput()))
            record_thread_failure(Result::Failure(
                ErrorCode::InvalidState, "hs_runtime",
                "Presentation settings queue capacity exceeded."));
        std::optional<GraphicsSettingsCommitRequest> next_settings_request;
        try { next_settings_request = settings_commit.NextRequest(); }
        catch (const std::bad_alloc &)
        {
            settings_commit.SuspendUnsentAfterAllocationFailure();
            const auto modal = display_modal_phase.load(std::memory_order_acquire);
            if (modal == GraphicsUiPhase::Pending || modal == GraphicsUiPhase::RestoringDisplay)
            {
                (void)rollback_window_placement();
                display_modal_phase.store(GraphicsUiPhase::Applied, std::memory_order_release);
            }
            graphics_ui_phase.store(GraphicsUiPhase::ApplyFailed, std::memory_order_release);
            presentation_settings_changed = true;
        }
        if (const auto &request = next_settings_request)
        {
            if (!channels.renderer_settings.TryPush(*request))
            {
                settings_commit.RejectUnqueuedRequest();
                const auto modal = display_modal_phase.load(std::memory_order_acquire);
                if ((modal == GraphicsUiPhase::Pending && !display_start_deferred) ||
                    modal == GraphicsUiPhase::RestoringDisplay)
                    (void)rollback_window_placement();
                record_thread_failure(Result::Failure(
                    ErrorCode::InvalidState, "hs_runtime",
                    "Renderer settings request queue capacity exceeded."));
            }
            else
            {
                settings_request_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                const auto modal = display_modal_phase.load(std::memory_order_acquire);
                if ((modal == GraphicsUiPhase::Pending && !display_start_deferred) ||
                    modal == GraphicsUiPhase::RestoringDisplay)
                    display_generation = request->generation;
                graphics_ui_phase.store(
                    display_restoring ? GraphicsUiPhase::RestoringDisplay
                                      : GraphicsUiPhase::Pending,
                    std::memory_order_release);
            }
        }
        const auto receipt = channels.renderer_graphics_ui.load(std::memory_order_acquire);
        const auto phase = graphics_ui_phase.load(std::memory_order_acquire);
        const auto pending = settings_commit.HasPending();
        const auto save_failure = settings_save_failed.load(std::memory_order_acquire);
        if (presentation_settings_changed || receipt != last_renderer_receipt ||
            phase != last_published_phase || pending != last_published_pending ||
            save_failure != last_published_save_failure)
        {
            try
            {
                auto model = receipt ? std::make_shared<GraphicsSettingsUiModel>(*receipt)
                                     : std::make_shared<GraphicsSettingsUiModel>();
                model->requested = settings_commit.Requested().graphics;
                model->pending = pending || settings_commit.HasUnsentEdits();
                const auto state = settings_commit.TimedOut() ? "Apply timed out; waiting for native completion before restore"
                    : settings_commit.PreparationFailed() ? "Settings snapshot allocation failed; renderer and confirmed document retained"
                    : settings_commit.IsRecovering() ? "Restoring last persisted graphics profile"
                    : phase == GraphicsUiPhase::Requested ? "Requested"
                    : phase == GraphicsUiPhase::Pending ? "Applying"
                    : phase == GraphicsUiPhase::AwaitingDisplayConfirmation ? "Awaiting display confirmation (15 seconds)"
                    : phase == GraphicsUiPhase::RestoringDisplay ? "Restoring display"
                    : phase == GraphicsUiPhase::ApplyFailed ? "Apply rejected; previous confirmed profile retained"
                    : receipt ? "Renderer acknowledged" : "Renderer status has not arrived";
                model->transaction_state = std::string(state) + (save_failure ? " / Save failed" : " / Confirmed settings save unchanged");
                if (receipt && receipt->generation != settings_commit.CommittedGeneration())
                    model->transaction_state += " / Detailed renderer receipt is from a different acknowledgement generation";
                if (settings_commit.FollowupRejected()) model->transaction_state += " / Dependent follow-up edit rejected by complete validation";
                ui_click_document = settings_commit.Requested();
                ui_click_settings = settings_commit.UiInput();
                ui_click_model = *model;
                channels.presentation_graphics_ui.store(std::move(model), std::memory_order_release);
            }
            catch (const std::bad_alloc &)
            {
                OutputDebugStringA("Settings UI snapshot allocation failed; previous immutable model retained.\n");
            }
            last_renderer_receipt = receipt;
            last_published_phase = phase;
            last_published_pending = pending;
            last_published_save_failure = save_failure;
        }
        PresentationEvent audio_event;
        while (channels.audio_events.TryPop(audio_event))
        {
            if (DecodeAudioAction(audio_event.parameters) == AudioEventAction::Stop)
                audio.Stop(audio_event.asset);
            else
            {
                if (audio_event.asset.value == MakeAssetId("audio.bgm.final_boss").value)
                {
                    if (active_bgm.value != 0) audio.Stop(active_bgm);
                    active_bgm = MakeAssetId("audio.bgm.final_boss");
                }
                audio.Play(audio_event);
            }
        }
        audio.UpdateListener(
            {channels.camera_target_x.load(std::memory_order_acquire), 0.0f,
             channels.camera_target_z.load(std::memory_order_acquire)},
            {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
        const auto session_phase = static_cast<SessionPhase>(
            channels.session_phase.load(std::memory_order_acquire));
        if (session_phase != previous_audio_phase)
        {
            const auto old_phase = previous_audio_phase;
            const auto target = session_phase == SessionPhase::MainMenu
                                    ? MakeAssetId("audio.bgm.main_menu")
                                    : (session_phase == SessionPhase::Victory
                                           ? MakeAssetId("audio.stinger.victory")
                                           : (session_phase == SessionPhase::Defeat
                                                  ? MakeAssetId("audio.stinger.defeat")
                                                  : MakeAssetId("audio.bgm.session")));
            if (session_phase == SessionPhase::Victory || session_phase == SessionPhase::Defeat)
            {
                if (active_bgm.value != 0) audio.Stop(active_bgm);
                if (active_ambience.value != 0) audio.Stop(active_ambience);
                active_ambience = {};
                audio.Play(MakeAudioEvent(target.value == MakeAssetId("audio.stinger.victory").value
                                              ? "audio.stinger.victory" : "audio.stinger.defeat",
                                          ++ui_audio_sequence));
                active_bgm = {};
            }
            else if (target.value != active_bgm.value)
            {
                if (active_bgm.value != 0) audio.Stop(active_bgm);
                active_bgm = target;
                audio.Play(MakeAudioEvent(session_phase == SessionPhase::MainMenu
                                              ? "audio.bgm.main_menu" : "audio.bgm.session",
                                          ++ui_audio_sequence));
            }
            if (session_phase == SessionPhase::Playing && active_ambience.value == 0)
            {
                active_ambience = MakeAssetId("audio.ambience.arena");
                audio.Play(MakeAudioEvent("audio.ambience.arena", ++ui_audio_sequence));
            }
            else if (session_phase != SessionPhase::Playing && active_ambience.value != 0)
            {
                audio.Stop(active_ambience);
                active_ambience = {};
            }
            if (session_phase == SessionPhase::Paused)
                audio.Play(MakeAudioEvent("audio.ui.pause_open", ++ui_audio_sequence));
            else if (old_phase == SessionPhase::Paused)
                audio.Play(MakeAudioEvent("audio.ui.pause_close", ++ui_audio_sequence));
            else if (session_phase == SessionPhase::CardSelection ||
                     session_phase == SessionPhase::StatAllocation)
                audio.Play(MakeAudioEvent("audio.ui.level_up", ++ui_audio_sequence));
            else if (session_phase == SessionPhase::RelicSelection)
                audio.Play(MakeAudioEvent("audio.ui.relic_select", ++ui_audio_sequence));
            previous_audio_phase = session_phase;
        }
        if (session_phase == SessionPhase::Paused ||
            session_phase == SessionPhase::CardSelection ||
            session_phase == SessionPhase::StatAllocation ||
            session_phase == SessionPhase::RelicSelection)
        {
            audio.PauseCombat();
        }
        else
        {
            audio.ResumeCombat();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    window.StopInput();
    channels.stop_requested.store(true, std::memory_order_release);
    native_input_frame.Cancel();
    simulation_thread.join();
    // GraphicsSettingsCommit owns any in-flight request snapshot. Join before
    // that owner can be destroyed, including shutdown with an unconsumed ack.
    render_thread.join();
    audio.Shutdown();

    if (!config.smoke)
    {
        profile.best_level = channels.best_level.load(std::memory_order_relaxed);
        profile.total_wins += channels.completed_run_wins.load(std::memory_order_relaxed);
        profile.total_kills += channels.completed_run_kills.load(std::memory_order_relaxed);
        if (auto saved = save_store.SaveProfile(profile); !saved)
        {
            record_thread_failure(saved);
        }
    }

    {
        std::lock_guard lock(result_mutex);
        result = thread_result;
    }
    const auto final_tick = channels.completed_tick.load(std::memory_order_acquire);
    const auto checksum = channels.gameplay_checksum.load(std::memory_order_acquire);
    const auto rendered_frames = channels.rendered_frames.load(std::memory_order_acquire);
    const auto rendered_particles =
        channels.rendered_particles.load(std::memory_order_acquire);
    const auto valid =
        result && final_tick >= (config.smoke ? config.maximum_ticks : 0) && rendered_frames > 0 &&
        rendered_particles <= 10'000u &&
        channels.dropped_input_edges.load() == 0 &&
        channels.dropped_presentation_events.load() == 0;
    if (playtest_recorder.Active())
    {
        if (auto finished = playtest_recorder.Finish(valid); !finished)
        {
            result = finished;
        }
    }

    WriteText(config.artifact_directory / "spec.json",
              std::format("{{\"scenario_id\":\"runtime-smoke\",\"seed\":{},\"maximum_ticks\":{},"
                          "\"content_hash\":{},\"mode\":\"offscreen_render\"}}\n",
                          config.seed, config.maximum_ticks, content_hash));
    WriteText(config.artifact_directory / "result.json",
              std::format("{{\"schema_version\":1,\"scenario_id\":\"runtime-smoke\","
                          "\"mode\":\"offscreen_render\",\"execution_valid\":{},"
                          "\"assertions_passed\":{},"
                          "\"tick\":{},\"checksum\":{},\"rendered_frames\":{},"
                          "\"rendered_particles\":{},"
                          "\"content_hash\":{},"
                          "\"worker_count\":{},"
                          "\"dropped_input_edges\":{},\"dropped_presentation_events\":{},"
                          "\"particle_effects_hash\":{},"
                          "\"user_review\":\"awaiting\"}}\n",
                          valid ? "true" : "false", valid ? "true" : "false", final_tick,
                          checksum, rendered_frames, rendered_particles, content_hash,
                          task_system.WorkerCount(),
                          channels.dropped_input_edges.load(),
                          channels.dropped_presentation_events.load(),
                          vfx_catalog.PayloadHash()));

    if (!valid && result)
    {
        result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                 "Runtime execution assertions failed.");
    }
    return {result, final_tick, checksum, rendered_frames, playtest_recorder.Directory()};
}

} // namespace hs
