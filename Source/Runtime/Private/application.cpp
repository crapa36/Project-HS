#include <hs/runtime/application.hpp>
#include <hs/runtime/shipping_content.hpp>
#include <hs/core/shipped_content_owner.hpp>
#ifdef HS_DEVELOPMENT_TOOLS
#include <hs/presentation/presentation_catalog_io.hpp>
#include <hs/game_rules/simulation_rules_io.hpp>
#include <hs/asset_manifest/selected_generation.hpp>
#include "vfx_generation_rebind.hpp"
#include "generation_reload_watch.hpp"
#endif
#include <hs/presentation/projector.hpp>
#include <hs/presentation/meadow_surface.hpp>

#include "runtime_channels.hpp"
#include "native_input_frame.hpp"
#include "ui_command_admission.hpp"
#include "runtime_diagnostics.hpp"
#include "process_scene.hpp"
#include "scene_generation_binding.hpp"
#include "hero_work_log.hpp"
#include "audio_session.hpp"
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
#include "renderer_settings_projection.hpp"
#include "startup_graphics_settings.hpp"
#include "window.hpp"
#include "camera_pose.hpp"

#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/process_info.hpp>
#include <hs/gameplay/game_simulation.hpp>
#include <hs/jobs/task_system.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/render_data/render_frame_input.hpp>
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
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(HS_DEVELOPMENT_TOOLS)
#include <future>
#include <nlohmann/json.hpp>
#endif

namespace hs
{
namespace
{

#if defined(HS_DEVELOPMENT_TOOLS)
using runtime_detail::SceneGenerationBinding;

struct PreparedContentGeneration
{
    SelectedGeneration candidate;
    Result result{Result::Success()};
    const char *stage{"manifest"};
    bool superseded{};
};

PreparedContentGeneration PrepareContentGeneration(std::filesystem::path pointer_path,
    std::string pointer_text, std::string active_id, std::string active_scope)
{
    PreparedContentGeneration prepared;
    try
    {
        std::string current;
        prepared.result = ReadGenerationPointer(pointer_path, current);
        if (!prepared.result) return prepared;
        if (current != pointer_text)
        {
            prepared.superseded = true;
            return prepared;
        }
        const auto expected = nlohmann::json::parse(pointer_text);
        prepared.result = LoadSelectedGeneration(pointer_path, prepared.candidate);
        if (prepared.result)
        {
            // Load reads the pathname again; identity checks also reject an A -> B -> A race.
            const auto &candidate = prepared.candidate;
            if (expected.at("schema_version") != 1 ||
                expected.at("generation_id") != candidate.id ||
                expected.at("scope") != candidate.scope ||
                expected.at("manifest_locator") != "Generations/" + candidate.id + "/generation.json")
                prepared.superseded = true;
            else if (candidate.scope != active_scope)
                prepared.result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Content Generation changes the selected scope.");
            else if (candidate.id != active_id)
            {
                prepared.stage = "integrity";
                prepared.result = VerifySelectedGenerationProducts(candidate);
            }
        }
        if (const auto read = ReadGenerationPointer(pointer_path, current); !read)
        {
            if (prepared.result) prepared.result = read;
        }
        else
            prepared.superseded |= current != pointer_text;
    }
    catch (const std::exception &error)
    {
        prepared.result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
            "Content Generation preparation failed: " + std::string(error.what()));
    }
    return prepared;
}
#endif

#ifdef HS_DEVELOPMENT_TOOLS
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

#endif
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
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST),
                         reinterpret_cast<MONITORINFO *>(&monitor)))
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "Cannot query the current display.");
    std::array<char, CCHDEVICENAME * 4> device_name{};
    const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, monitor.szDevice, -1,
        device_name.data(), static_cast<int>(device_name.size()), nullptr, nullptr);
    if (count <= 1)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                               "Cannot encode the current display device name.");
    const std::string_view device(device_name.data(), static_cast<std::size_t>(count - 1));
    const auto selected = std::ranges::find_if(displays, [&](const auto &display) {
        // Public native ID: dxgi:<16 hex LUID digits>:<UTF-8 GDI DeviceName>.
        // CCD rectangles are physical; GetMonitorInfo rectangles can be DPI-virtualized.
        constexpr std::size_t name_offset = 5 + 16 + 1;
        const std::string_view id(display.id);
        return id.starts_with("dxgi:") && id.size() == name_offset + device.size() &&
            id[name_offset - 1] == ':' && id.substr(name_offset) == device;
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
                                           : std::string("확인 중");
    };
    const auto view = std::ranges::find_if(diagnostics.views, [](const auto &entry) { return entry.presenting; });
    const auto *measurements = view == diagnostics.views.end() ? nullptr
        : validation ? &view->validation_on : &view->validation_off;
    const auto fps = [](const RendererDiagnosticsCadence *cadence) {
        return cadence && cadence->available && std::isfinite(cadence->fps) && cadence->fps > 0.0
            ? std::format("{:.1f}", cadence->fps) : std::string("확인 중");
    };
    model.telemetry_status[1] = std::format("렌더링 {} / 출력 {} · 렌더링 {} FPS · 표시 {} FPS",
        extent(diagnostics.reconstruction.render_extent), extent(diagnostics.reconstruction.output_extent),
        fps(measurements ? &measurements->rendered_cadence : nullptr), fps(&diagnostics.displayed_cadence));
    const auto mib = [](std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
    const auto &memory = diagnostics.current_memory;
    const auto current = memory.budget_available
        ? std::format("{:.1f} / {:.1f} MiB", mib(memory.local_usage_bytes), mib(memory.local_budget_bytes))
        : std::string("확인 중");
    const auto peak = diagnostics.transition_memory_peak_available && diagnostics.transition_memory_peak.budget_available
        ? std::format("{:.1f} MiB", mib(diagnostics.transition_memory_peak.local_usage_bytes))
        : std::string("확인 중");
    model.telemetry_status[2] = std::format("VRAM 사용량 / 한도 {} · 설정 전환 최대 사용량 {}",
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
            ? "복원됨" : canonical.state == SettingsApplyState::Rejected
                ? "적용 거부" : canonical.state == SettingsApplyState::Applied
                    ? "적용됨" : "적용 중";
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
        // Keep detailed native reasons in diagnostics; the settings page needs a short readable summary.
        if (!reason.empty() && static_cast<unsigned char>(reason.front()) < 0x80)
        {
            const auto requested = GraphicsSettingToken(model->requested, id);
            const auto selected = std::ranges::find_if(model->choices, [&](const auto &entry) {
                return entry.setting == id && entry.token == requested;
            });
            reason = effective.empty() ? "적용 결과를 기다리는 중입니다"
                : selected != model->choices.end() && !selected->available ? selected->reason
                : requested == "auto" ? "현재 장치와 설정에 맞춰 자동으로 선택합니다"
                : requested == effective ? "요청한 설정으로 적용했습니다"
                : "현재 장치와 설정에서 가능한 값으로 적용했습니다";
        }
        model->resolved.push_back({id, std::move(effective), std::move(reason)});
    };
    constexpr std::array<std::pair<std::string_view, SettingId>, 12> capability_settings{{
        std::pair<std::string_view, SettingId>{"sr_provider", SettingId::ReconstructionProvider},
        {"reconstruction", SettingId::ReconstructionQuality},
        {"fg_provider", SettingId::FrameGenerationProvider},
        {"frame_generation", SettingId::FrameGenerationMode},
        {"frame_generation_multiplier", SettingId::FrameGenerationMultiplier},
        {"hdr", SettingId::HdrMode}, {"latency", SettingId::LowLatency},
        {"ao", SettingId::AoMode}, {"gi", SettingId::GiMode},
        {"reflection", SettingId::ReflectionMode}, {"shadow", SettingId::ShadowMode},
        {"adaptive_shading", SettingId::AdaptiveShading}}};
    const auto support_reason = [](GraphicsCapabilityReason reason) -> std::string {
        switch (reason)
        {
        case GraphicsCapabilityReason::None: return "현재 장치와 설정에서 지원합니다";
        case GraphicsCapabilityReason::Unsupported: return "현재 장치 또는 방식에서 지원하지 않습니다";
        case GraphicsCapabilityReason::Unavailable: return "필요한 기능이 아직 준비되지 않았습니다";
        case GraphicsCapabilityReason::MissingProvider: return "필요한 기능이 설치되어 있지 않습니다";
        case GraphicsCapabilityReason::MissingDisplay: return "필요한 모니터를 사용할 수 없습니다";
        case GraphicsCapabilityReason::Incompatible: return "현재 설정과 함께 사용할 수 없습니다";
        case GraphicsCapabilityReason::BudgetLimited: return "현재 메모리 한도를 초과합니다";
        case GraphicsCapabilityReason::Rejected: return "장치에서 설정 변경을 거부했습니다";
        }
        return "현재 사용할 수 없습니다";
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
            choice(id, token, false, "저장된 선택은 현재 장치에서 사용할 수 없습니다");
    }
    std::vector<RendererDisplayOutput> displays;
    const auto enumerated = EnumerateRendererDisplays(displays);
    choice(SettingId::DisplayOutput, "auto", static_cast<bool>(enumerated), enumerated ? std::string("현재 창이 있는 모니터") : std::string("모니터 목록을 확인할 수 없습니다"));
    choice(SettingId::OutputExtent, "auto", true, "현재 창 크기에 맞춥니다");
    for (const auto &display : displays)
    {
        choice(SettingId::DisplayOutput, display.id, true, "사용할 수 있는 모니터");
        const auto selected = std::ranges::find_if(model->choices, [&](const auto &entry) {
            return entry.setting == SettingId::DisplayOutput && entry.token == display.id;
        });
        selected->label = std::format("모니터 {} ({}×{})", &display - displays.data() + 1,
                                      display.width, display.height);
        choice(SettingId::OutputExtent, std::format("{}x{}", display.width, display.height), true, "현재 모니터 해상도에 맞춥니다");
    }
    MONITORINFO monitor{sizeof(MONITORINFO)};
    const auto renderer_hdr = canonical.actual_applied_available
        ? canonical.actual_applied.hdr_mode == GraphicsHdrMode::On ? "HDR10" : "SDR"
        : "확인 중";
    model->telemetry_status[0] = std::format("모니터 확인 중 · 주사율 확인 중 · 모니터 HDR 확인 중 · 출력 {}", renderer_hdr);
    if (GetMonitorInfoW(MonitorFromWindow(window_handle, MONITOR_DEFAULTTONEAREST), &monitor))
        for (const auto &display : displays)
            if (display.left == monitor.rcMonitor.left && display.top == monitor.rcMonitor.top &&
                static_cast<std::int64_t>(display.left) + display.width == monitor.rcMonitor.right &&
                static_cast<std::int64_t>(display.top) + display.height == monitor.rcMonitor.bottom)
            {
                status(SettingId::DisplayOutput, display.id, graphics.display_output.value == "auto" || graphics.display_output.value == display.id
                    ? "현재 창이 있는 모니터에 적용했습니다" : "선택한 모니터를 사용할 수 없어 현재 모니터를 유지합니다");
                double refresh_hz{};
                const auto refresh = QueryWindowDisplayRefresh(window_handle, refresh_hz);
                model->telemetry_status[0] = std::format("모니터 {} · 주사율 {} · 모니터 HDR {} · 출력 {}",
                    std::format("{}", &display - displays.data() + 1), refresh ? std::format("{:.2f} Hz", refresh_hz) : std::string("확인 중"),
                    display.hdr_active ? "켜짐" : "꺼짐", renderer_hdr);
                break;
            }
    status(SettingId::HdrMode, canonical.actual_applied_available
        ? GraphicsSettingToken(canonical.actual_applied, SettingId::HdrMode) : std::string{},
        canonical.actual_applied_available
            ? canonical.actual_applied.hdr_mode == GraphicsHdrMode::On
                ? "HDR10 화면으로 출력합니다" : "일반 SDR 화면으로 출력합니다"
            : "화면 설정의 적용 결과를 기다리는 중입니다");
    choice(SettingId::DisplayOutput, graphics.display_output.value, false, "저장된 모니터를 현재 찾을 수 없습니다");
    choice(SettingId::OutputExtent, GraphicsSettingToken(graphics, SettingId::OutputExtent), false, "직접 지정한 창 크기입니다");
    status(SettingId::ReconstructionProvider, reconstruction.effective_provider.value, reconstruction.requested_availability + ": " + reconstruction.fallback_reason);
    status(SettingId::ReconstructionQuality, reconstruction.effective_quality.value, reconstruction.fallback_reason);
    status(SettingId::OutputExtent, std::format("{}x{}", reconstruction.output_extent.width, reconstruction.output_extent.height), "실제로 출력하는 해상도입니다");
    status(SettingId::FrameGenerationProvider, presentation.effective_provider.value, presentation.availability + ": " + presentation.fallback_reason);
    status(SettingId::FrameGenerationMode, std::string(GraphicsEnumToken(presentation.active_mode)), presentation.fallback_reason);
    status(SettingId::FrameGenerationMultiplier, FrameGenerationMultiplierPolicyToken(presentation), presentation.maximum_multiplier
        ? std::format("지원하는 프레임 생성 배수: 2~{}배", presentation.maximum_multiplier)
        : presentation.requested.provider.value == "fsr" ? "이 방식은 배수를 직접 설정할 수 없습니다"
        : "프레임 생성을 켜면 지원하는 배수를 확인합니다");
    status(SettingId::LowLatency, presentation.low_latency_active ? "on" : "off", presentation.fallback_reason);
    status(SettingId::AoMode, std::string(GraphicsEnumToken(lighting.active.ao_mode)), lighting.ao_fallback_reason);
    status(SettingId::GiMode, std::string(GraphicsEnumToken(lighting.active.gi_mode)), lighting.gi_fallback_reason.empty() ? lighting.software_gi.reason : lighting.gi_fallback_reason);
    status(SettingId::ReflectionMode, std::string(GraphicsEnumToken(lighting.active.reflection_mode)), lighting.reflection_fallback_reason);
    status(SettingId::ShadowMode, std::string(GraphicsEnumToken(lighting.active.shadow_mode)), lighting.shadow_fallback_reason + "; " + lighting.local_shadows.reason);
    const auto &adaptive = lighting.adaptive_shading;
    status(SettingId::AdaptiveShading,
        std::format("래스터 {} · 연산 {}", adaptive.raster_applied ? "켜짐" : "꺼짐", adaptive.compute_applied ? "켜짐" : "꺼짐"),
        std::format("Requested {}; resolved raster {} / compute {}; ready raster {} / compute {}; usable {}; VRS tier {} / tile {}; {} bytes. Raster: {} Compute: {}",
            GraphicsEnumToken(adaptive.requested), adaptive.raster_resolved, adaptive.compute_resolved,
            adaptive.raster_ready, adaptive.compute_ready, adaptive.usable, adaptive.vrs_tier,
            adaptive.vrs_tile_size, adaptive.allocated_bytes, adaptive.raster_reason, adaptive.compute_reason));
    const auto streaming_reason = std::format("Ready {}; native asset I/O {}; sampler feedback {}; {} textures / {} physical bytes. {}",
        streaming.ready, streaming.native_asset_io, streaming.sampler_feedback, streaming.texture_count,
        streaming.physical_bytes, streaming.reason);
    status(SettingId::TextureQuality, std::string(GraphicsEnumToken(streaming.applied_quality)), streaming_reason);
    status(SettingId::TextureBudgetMib, streaming.applied_budget_mib ? std::to_string(streaming.applied_budget_mib) : "0", streaming_reason);
    for (const auto &descriptor : GraphicsSettingDescriptors())
        if (std::ranges::find(model->resolved, descriptor.id, &GraphicsUiStatus::setting) == model->resolved.end())
            status(descriptor.id, canonical.actual_applied_available
                ? GraphicsSettingToken(canonical.actual_applied, descriptor.id) : std::string{},
                canonical.actual_applied_available ? "요청한 설정으로 적용했습니다" : "렌더러의 적용 결과를 기다리는 중입니다");
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


Result WriteRendererStartup(const std::filesystem::path &path, double initialize_us,
    std::uint64_t calls, bool succeeded, const RendererStartupTimings &timings,
    const RendererPsoCacheStats &cache)
{
    std::uint64_t step_ns{};
    for (const auto &sample : timings.steps) step_ns += sample.nanoseconds;
    std::string text = std::format(
        "{{\"initialize_us\":{},\"calls\":{},\"succeeded\":{},\"detail_available\":{},"
        "\"completed\":{},\"begin_qpc\":{},\"end_qpc\":{},\"qpc_frequency\":{},"
        "\"initialize_ns\":{},\"step_wall_ns\":{},\"unclassified_wall_ns\":{},\"steps\":{{",
        initialize_us, calls, succeeded, timings.captured, timings.completed,
        timings.begin_qpc, timings.end_qpc, timings.qpc_frequency, timings.total_ns,
        step_ns, timings.total_ns >= step_ns ? timings.total_ns - step_ns : 0);
    const auto append_samples = [&](const auto &names, const auto &samples) {
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            if (index) text += ',';
            text += std::format("\"{}\":{{\"ns\":{},\"calls\":{}}}", names[index],
                samples[index].nanoseconds, samples[index].calls);
        }
    };
    append_samples(kRendererStartupStepNames, timings.steps);
    text += "},\"nested_operations\":{";
    append_samples(kRendererStartupOperationNames, timings.operations);
    text += std::format(
        "}},\"pso_at_initialize_return\":{{\"capture_timings\":{},\"hits\":{},\"misses\":{},"
        "\"native_creates\":{},\"loaded_bytes\":{},\"key_build_ns\":{},\"key_build_max_ns\":{},"
        "\"library_load_ns\":{},\"library_load_max_ns\":{},\"native_create_ns\":{},\"native_create_max_ns\":{},"
        "\"library_store_ns\":{},\"library_store_max_ns\":{},\"serialize_ns\":{},\"cache_write_ns\":{},"
        "\"mutex_wait_ns\":{},\"mutex_wait_max_ns\":{}}},",
        cache.capture_timings, cache.hits, cache.misses, cache.native_creates, cache.loaded_bytes,
        cache.key_build_ns, cache.key_build_max_ns, cache.library_load_ns, cache.library_load_max_ns,
        cache.native_create_ns, cache.native_create_max_ns, cache.library_store_ns, cache.library_store_max_ns,
        cache.serialize_ns, cache.cache_write_ns, cache.mutex_wait_ns, cache.mutex_wait_max_ns);
    text += "\"semantics\":\"wall ns; steps are sequential synchronous call intervals; nested operations and PSO intervals overlap steps and each other; do not sum with steps\","
        "\"read_coverage\":\"ReadBinary checked whole-file read plus local-probe and atlas reads; includes bounds/open/host allocation\","
        "\"hash_coverage\":\"explicit local-probe SHA256 only; package-internal validation/hash remains in decode; Runtime Generation verification precedes this Initialize\","
        "\"leaf_coverage\":\"character/environment decoders and geometry preparation; monster palette adoption and named animation decode; Impl allocation plus probe/LTC allocation; canonical/asset descriptors; host packing; utility submit and completion admission\","
        "\"leaf_limits\":\"other native service allocations/decoders remain inside their named step; async-worker internal work is not propagated into thread-local leaf samples; GPU/driver busy time unavailable\","
        "\"gpu_wait_semantics\":\"completion_admission includes fence setup/queue drain; gpu_fence_wait is only actual WaitForSingleObject calls; neither is GPU elapsed\","
        "\"native_device_internal_ns\":null,\"process_cold\":null,\"disk_cache_state\":\"unobserved\","
        "\"native_pipeline_cache_state\":\"observe loaded_bytes/hits/misses; cold/warm not forced or inferred\"}\n";
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    return stream ? Result::Success() : Result::Failure(ErrorCode::InvalidState, "hs_runtime",
        "Could not write renderer initialization diagnostics.");
}


PresentationEvent MakeAudioEvent(std::string_view cue, Sequence sequence) noexcept
{
    PresentationEvent event;
    event.sequence = sequence;
    event.kind = PresentationKind::Audio;
    event.asset = MakeAssetId(cue);
    event.parameters = EncodeAudioAction(AudioEventAction::Play);
    return event;
}

std::uint64_t ReadDiagnosticQpc() noexcept
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}

constexpr std::array<std::string_view, 8> kMainTaskNames{
    "loop", "input_pump", "ui_click", "ui_hover", "ui_settings", "audio_drain", "audio_state", "audio_capture"};
constexpr std::array<std::string_view, 13> kSimulationTaskNames{
    "iteration", "input_harness", "tick_fixed", "enemy_harness", "borrow_observe", "enemy_animation",
    "signal_fanout", "record_io", "snapshot_wait", "project", "publication", "replay_validate", "heartbeat_io"};
constexpr std::array<std::string_view, 23> kVfxTaskNames{
    "vfx_event_drain", "vfx_snapshot_acquire", "vfx_session_dispatch", "vfx_legacy_expand", "vfx_frame_adapter",
    "vfx_entity_surface", "vfx_movement", "vfx_ballistic", "vfx_status", "vfx_ground", "vfx_local_outputs", "vfx_ground_events",
    "vfx_flashes", "vfx_motes", "vfx_mesh", "vfx_ribbons", "vfx_spawn_sort", "vfx_screen", "vfx_volume", "vfx_light", "vfx_distortion", "vfx_decal", "vfx_fresnel"};

std::string TaskColumns(std::span<const std::string_view> names)
{
    std::string result;
    for (const auto name : names) result += std::format(",{}_us,{}_calls", name, name);
    return result;
}

void AppendTaskValues(std::string &row, std::span<const runtime_detail::DiagnosticSample> samples, double microseconds)
{
    for (const auto &sample : samples)
        runtime_detail::AppendCsvValues(row, sample.ticks * microseconds, sample.calls);
}

Result RefillDiagnosticEnemies(GameSimulation &simulation, std::uint32_t target,
                               std::uint64_t &spawn_index, std::uint32_t &added)
{
    if (!target) return Result::Success();
    const auto probe = simulation.GetSessionProbe();
    if (probe.normal_enemy_count > target || probe.boss_count != 0)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Diagnostic enemy fixture exceeded its population.");
    const auto deficit = target - probe.normal_enemy_count;
    for (std::uint32_t i = 0; i < deficit; ++i, ++spawn_index)
    {
        const auto angle = static_cast<double>(spawn_index) * 2.39996322972865332;
        const auto radius = 6.0 + 4.0 * static_cast<double>(spawn_index % 101) / 100.0;
        const Float2 position{probe.player_position.x + static_cast<float>(std::cos(angle) * radius),
                              probe.player_position.y + static_cast<float>(std::sin(angle) * radius)};
        if (auto result = simulation.ApplyDebugCommand({DebugCommandKind::SpawnEnemy,
                spawn_index % 4 == 3 ? static_cast<std::uint64_t>(EnemyKind::Ranged) :
                                      static_cast<std::uint64_t>(EnemyKind::Melee), 0, position}); !result)
            return result;
    }
    const auto after = simulation.GetSessionProbe();
    if (after.normal_enemy_count != target || after.boss_count != 0)
        return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Diagnostic enemy fixture could not fill its population.");
    added += deficit;
    return Result::Success();
}

} // namespace

ApplicationResult RunApplication(const ApplicationConfig &config)
{
    // Visible performance experiments use the same clock and interpolation as interactive play.
    const bool realtime = !config.smoke || config.visible;
    // Smoke, experiment, recording and replay consumers require complete diagnostic artifacts.
    const bool capture_diagnostics = config.capture_diagnostics || config.smoke ||
        config.record_playtest || config.replay_compare || !config.replay_directory.empty();
    if (config.smoke_render_lockstep && !config.smoke)
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Smoke render lockstep requires --smoke.")};
    if (config.diagnostic_enemy_count &&
        (!config.smoke || !config.capture_diagnostics || config.diagnostic_enemy_count > 10'000 ||
         config.record_playtest || config.replay_compare || !config.replay_directory.empty() ||
         config.skill_vfx_capture < kCombatSkillCount || config.monster_preview_asset < 6 ||
         config.boss_vfx_capture < 3 || config.boss_phase2_capture))
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
            "Diagnostic enemy fixture requires smoke + diagnostics, at most 10000 enemies, and no replay, recording or preview.")};
    if (config.hero_work_mode > HeroWorkDiagnosticMode::CoverageOnly ||
        (!config.capture_hero_work && config.hero_work_mode != HeroWorkDiagnosticMode::Full))
        return {Result::Failure(ErrorCode::InvalidArgument, "hs_runtime", "Hero work diagnostic mode requires its explicit capture flag.")};
    if (config.capture_hero_work && !config.capture_diagnostics)
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Hero work capture requires explicit diagnostics.")};
#if !defined(HS_DEVELOPMENT_TOOLS)
    if (config.capture_hero_work)
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Hero work capture requires a development executable.")};
#endif
    const runtime_detail::DiagnosticClock capture_clock = config.capture_diagnostics ? ReadDiagnosticQpc : nullptr;
    LARGE_INTEGER capture_frequency{};
    if (capture_clock && (!QueryPerformanceFrequency(&capture_frequency) || capture_frequency.QuadPart <= 0))
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Diagnostic clock calibration failed.")};
    const auto task_microseconds = capture_clock ? 1'000'000.0 / static_cast<double>(capture_frequency.QuadPart) : 0.0;
    std::array<runtime_detail::DiagnosticSample, kMainTaskNames.size()> main_work{};
    const auto executable_directory = CurrentExecutableDirectory();
    if (executable_directory.empty())
    {
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Cannot resolve the executable directory.")};
    }
    std::uint64_t content_hash{};
#ifdef HS_DEVELOPMENT_TOOLS
    const auto generation_pointer = config.generation_pointer.empty()
        ? executable_directory / "GenerationState/runtime-full.json"
        : config.generation_pointer;
    SelectedGeneration selected_generation;
    if (auto loaded = LoadSelectedGeneration(generation_pointer, selected_generation);
        !loaded)
        return {loaded};
    const std::string deployment_id = selected_generation.id;
    std::filesystem::path cooked_particle_effects_path;
    std::filesystem::path cooked_vfx_program_path;
    for (const auto [id, path] : {
             std::pair<std::string_view, std::filesystem::path *>(
                 "vfx/particle_effects.hsbin", &cooked_particle_effects_path),
             {"vfx/vfx_program.hsbin", &cooked_vfx_program_path}})
        if (auto resolved = selected_generation.Require(id, *path); !resolved)
            return {resolved};
    SimulationRules simulation_rules;
    std::filesystem::path cooked_simulation_rules_path;
    if (auto resolved = selected_generation.Require("game-data/simulation-rules", cooked_simulation_rules_path); !resolved)
        return {resolved};
    if (auto loaded = LoadSimulationRules(cooked_simulation_rules_path,
                                          simulation_rules, &content_hash);
        !loaded)
        return {loaded};
    PresentationCatalog presentation_catalog;
    std::filesystem::path cooked_presentation_path;
    if (auto resolved = selected_generation.Require("game-data/presentation-catalog", cooked_presentation_path); !resolved)
        return {resolved};
    if (auto loaded = LoadPresentationCatalog(cooked_presentation_path,
                                              presentation_catalog, &content_hash);
        !loaded)
    {
        return {loaded};
    }
#else
    auto shipped_content = std::make_shared<ShippedContentOwner>();
    if (auto opened = shipping::OpenImage(executable_directory, shipping::ImageInfo(), *shipped_content); !opened)
        return {opened};
    const auto &presentation_catalog = shipping::Presentation();
    const auto &simulation_rules = shipping::Simulation();
    content_hash = shipping::ImageInfo().content_hash;
    std::string deployment_id;
    deployment_id.reserve(64);
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 16; i != 48; ++i)
    {
        const auto byte = std::to_integer<unsigned>(shipping::ImageInfo().header[i]);
        deployment_id += hex[byte >> 4]; deployment_id += hex[byte & 15];
    }
#endif
    const auto simulation_rules_hash = SimulationRulesHash(simulation_rules);
    VfxCatalog vfx_catalog;
#ifdef HS_DEVELOPMENT_TOOLS
    if (auto loaded = VfxCatalog::Load(
            cooked_particle_effects_path, cooked_vfx_program_path, vfx_catalog);
        !loaded)
        return {loaded};
#else
    vfx_catalog.Bind(shipping::Vfx());
#endif
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
    const auto initial_settings = PrepareStartupGraphicsSettings(requested_settings, config);
    GraphicsSettingsCommit settings_commit(std::move(requested_settings),
                                           initial_settings, config.mute_audio);
    SettingsData &settings = settings_commit.Effective();
    ProfileData profile;
    if (auto loaded = save_store.LoadProfile(profile); !loaded)
    {
        return {loaded};
    }
    runtime_detail::DiagnosticSample audio_startup{};
    runtime_detail::MainWake main_wake(capture_clock);
    if (!main_wake.Handle())
        return {Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Cannot create Main wake event.")};
    runtime_detail::DiagnosticScope audio_startup_scope(audio_startup, capture_clock);
    AudioEngine audio;
    audio.SetCaptureTimings(config.capture_diagnostics);
    audio.SetWakeCallback(runtime_detail::MainWake::NotifyOpaque, &main_wake);
#ifdef HS_DEVELOPMENT_TOOLS
    if (auto initialized = audio.Initialize(settings, selected_generation.product_paths); !initialized)
#else
    if (auto initialized = audio.Initialize(settings, shipping::Audio(), *shipped_content); !initialized)
#endif
    {
        return {initialized};
    }

    audio_startup_scope.Stop();
    // No window owns the foreground yet; suppress even the initial menu cue.
    audio.SetBackgroundMuted(true);

    // Bounded event queues carry full gameplay geometry and exceed the default
    // Windows stack reserve; their lifetime still spans all joined workers.
    auto channel_storage = std::make_unique<RuntimeChannels>(&main_wake);
    auto &channels = *channel_storage;
    channels.capture_clock = capture_clock;
    channels.camera_zoom_percent.store(std::clamp(config.camera_zoom_percent, 15u, 120u));
    channels.best_level.store(profile.best_level, std::memory_order_relaxed);
    TaskSystem task_system;
    Window window(channels, settings.skill_virtual_keys, presentation_catalog.camera);
    Sequence ui_audio_sequence{};
    auto main_audio = std::make_unique<runtime_detail::MainAudioSession>(audio, ui_audio_sequence, capture_clock);
    SettingsData ui_click_settings = settings_commit.UiInput();
    SettingsDocument ui_click_document = settings_commit.Requested();
    GraphicsSettingsUiModel ui_click_model;
    ui_click_model.requested = ui_click_document.graphics;
    ui_click_model.transaction_state = "렌더러를 준비하는 중입니다";
    channels.presentation_graphics_ui.store(std::make_shared<const GraphicsSettingsUiModel>(ui_click_model), std::memory_order_release);
    bool ui_command_queue_full{};
    bool ui_click_allocation_failed{};
    std::uint64_t consumed_selection_packet_generation{};
    std::atomic graphics_ui_phase{GraphicsUiPhase::Applied};
    std::atomic settings_save_failed{false};
    std::atomic display_modal_phase{GraphicsUiPhase::Applied};
    bool display_confirm_clicked{};
    bool display_cancel_clicked{};
    window.SetUiClickHandler([&](Float2 client_pixels, ViewExtent client_extent) {
        runtime_detail::DiagnosticScope scope(main_work[2], capture_clock);
        try
        {
            const auto receipt = channels.ui_packets.presented_ui_packet.load(std::memory_order_acquire);
            if (!receipt) return;
            const auto &displayed = receipt->frame;
            if (!runtime_detail::UiFrameExtentMatches(receipt->presented_extent, client_extent))
            {
                channels.ui_input_rejected.store(true, std::memory_order_release);
                return; // Wait until the resized target has an accepted UI frame.
            }
            const auto cursor = ReferenceUiCursor(client_pixels,
                {receipt->presented_extent.width, receipt->presented_extent.height});
            if (!displayed->draw_content ||
                !runtime_detail::PointHitsUiDrawButton(displayed->draw_content->models, cursor)) return;
            const auto modal = static_cast<GraphicsUiPhase>(displayed->modal_phase);
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
            const auto live_session = channels.ReadSessionProbe();
            const auto live_page = static_cast<UiPage>(channels.ui_page.load(std::memory_order_acquire));
            const PresentationUiState live_ui{
                live_page,
                channels.collection_skill.load(std::memory_order_acquire),
                channels.character_skill.load(std::memory_order_acquire),
                channels.loadout_source.load(std::memory_order_acquire),
                channels.settings_group.load(std::memory_order_acquire),
                channels.settings_page.load(std::memory_order_acquire),
                channels.character_details_page.load(std::memory_order_acquire)};
            const bool selection_phase = displayed->session.phase == SessionPhase::CardSelection ||
                displayed->session.phase == SessionPhase::RelicSelection ||
                displayed->session.phase == SessionPhase::StatAllocation;
            if (channels.current_session_id.load(std::memory_order_acquire) != displayed->session_id ||
                live_session.phase != displayed->session.phase || !runtime_detail::SameUiState(live_ui, displayed->ui) ||
                (selection_phase && (!runtime_detail::SameUiSelection(live_session, displayed->session) ||
                    consumed_selection_packet_generation == displayed->generation)))
            {
                channels.ui_input_rejected.store(true, std::memory_order_release);
                return;
            }
            auto ui = displayed->ui;
            const auto previous_ui = ui;
            const auto current_graphics_ui = channels.presentation_graphics_ui.load(std::memory_order_acquire);
            if ((displayed->graphics_model && current_graphics_ui &&
                 displayed->graphics_model->generation != current_graphics_ui->generation) ||
                ((displayed->ui.page == UiPage::MainMenuSettings ||
                  displayed->ui.page == UiPage::PauseSettings) &&
                 !runtime_detail::SameUiSettings(displayed->settings, ui_click_settings)))
            {
                channels.ui_input_rejected.store(true, std::memory_order_release);
                return;
            }
            auto interaction = ResolveUiInteraction(displayed->session, ui, displayed->settings,
                cursor, displayed->graphics_model.get());
            const bool admitted = runtime_detail::AdmitPresentedUiInteraction(interaction, ui,
                previous_ui, *displayed, channels.ui_actions, channels.ui_commands,
                channels.rejected_ui_commands);
            if (admitted && selection_phase && interaction.gameplay_action)
                consumed_selection_packet_generation = displayed->generation;
            channels.ui_input_rejected.store(!admitted, std::memory_order_release);
            if (!admitted) ui_command_queue_full = true;
            channels.ui_page.store(static_cast<std::uint8_t>(ui.page),
                                   std::memory_order_release);
            channels.collection_skill.store(ui.selected_collection_skill, std::memory_order_release);
            channels.character_skill.store(ui.selected_character_skill, std::memory_order_release);
            channels.loadout_source.store(ui.loadout_source_slot, std::memory_order_release);
            channels.settings_group.store(ui.settings_group, std::memory_order_release);
            channels.settings_page.store(ui.settings_page, std::memory_order_release);
            channels.character_details_page.store(ui.character_details_page, std::memory_order_release);
            if (interaction.runtime_command &&
                ApplySettingsEdit(ui_click_document, *interaction.runtime_command))
            {
                ui_click_settings = ProjectSettingsDocument(ui_click_document);
                if (config.mute_audio) ui_click_settings.master_volume = 0;
                ui_click_model.requested = ui_click_document.graphics;
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
        runtime_detail::DiagnosticScope scope(main_work[3], capture_clock);
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
        window.Create(settings.width, settings.height, config.visible, settings.borderless);
    if (!result)
    {
        return {result};
    }
    GraphicsSettings window_display_graphics = settings_commit.Requested().graphics;
    GraphicsExtent windowed_client_extent = window_display_graphics.output_extent.value_or(
        GraphicsExtent{settings.width, settings.height});
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
    std::uint64_t sleep_probe_begin{}, sleep_probe_end{};
    if (capture_clock)
    {
        sleep_probe_begin = capture_clock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        sleep_probe_end = capture_clock();
    }
    if (capture_clock)
        WriteText(config.artifact_directory / "runtime_capture.json", std::format(
            "{{\"qpc_frequency\":{},\"audio_startup_us\":{},\"audio_startup_calls\":{},\"audio_startup_detail\":\"aggregate including preload and native initialization\",\"diagnostic_enemy_count\":{},\"scripted_input\":{},\"automatic_spawning\":{},\"capture_checksums\":{},\"cpu_scopes\":\"wall elapsed; nested inclusive\",\"audio_latency\":\"enqueue to API drain; worker request to verified Ready; XAudio2 latency is SDK estimate\",\"device_kernel_internal_time\":null,\"sleep_probe_requested_ms\":1,\"sleep_probe_begin_qpc\":{},\"sleep_probe_end_qpc\":{},\"sleep_probe_us\":{},\"os_thread_ready_qpc\":null}}\n",
            capture_frequency.QuadPart, audio_startup.ticks * task_microseconds, audio_startup.calls,
            config.diagnostic_enemy_count, config.diagnostic_enemy_count != 0, config.diagnostic_enemy_count == 0,
            config.diagnostic_enemy_count == 0 && (config.smoke || config.record_playtest || !config.replay_directory.empty()),
            sleep_probe_begin, sleep_probe_end, (sleep_probe_end - sleep_probe_begin) * task_microseconds));

    std::mutex result_mutex;
    Result thread_result = Result::Success();
    std::atomic<Tick> rendered_tick{};
    NativeInputFrame native_input_frame;
    native_input_frame.BindMainWake(runtime_detail::MainWake::NotifyOpaque, &main_wake);
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
#ifdef HS_DEVELOPMENT_TOOLS
        auto render_generation = std::make_shared<const SelectedGeneration>(selected_generation);
#else
        std::shared_ptr<const void> render_generation = shipped_content;
#endif
        D3D12Renderer renderer;
        RendererConfig renderer_config = ProjectRendererConfig(initial_renderer_settings, startup_graphics);
        std::vector<std::byte> startup_font;
        const auto font_result = [&]() -> Result {
#if defined(HS_DEVELOPMENT_TOOLS)
            ShippedContentOwner font_file;
            if (auto opened = ShippedContentOwner::Open(executable_directory / L"Fonts" / L"NotoSansKR.ttf", font_file); !opened) return opened;
            if (!font_file.Size() || font_file.Size() > UINT32_MAX)
                return Result::Failure(ErrorCode::InvalidArgument, "hs_runtime", "Developer font extent is invalid.");
            startup_font.resize(static_cast<std::size_t>(font_file.Size()));
            return font_file.ReadExact(0, startup_font);
#else
            return shipping::ReadFont(*shipped_content, shipping::ImageInfo(), startup_font);
#endif
        }();
        if (!font_result)
        {
            record_thread_failure(font_result);
            channels.render_ready.store(true, std::memory_order_release);
            channels.render_ready.notify_all();
            channels.NotifyMain();
            return;
        }
        renderer_config.font_bytes = startup_font;
        renderer_config.display_output = initial_native_output;
        renderer_config.task_executor.submit = [&](std::function<void()> record) {
            const bool accepted = task_system.Submit(*render_tasks,
                "Render graph recording",
                [record = std::move(record)](std::stop_token) { record(); });
            return accepted ? Result::Success() :
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
        if (config.surface_frontend) renderer_config.surface_frontend = *config.surface_frontend;
        if (config.geometry_frontend) renderer_config.geometry_frontend = *config.geometry_frontend;
        renderer_config.capture_timing_stream = config.capture_diagnostics;
        renderer_config.capture_gpu_timings = config.capture_gpu_timings;
        renderer_config.capture_hero_work = config.capture_hero_work;
        renderer_config.hero_work_mode = config.hero_work_mode;
        renderer_config.frame_generation_allowed = false; // Startup/loading/menu are not gameplay frames.
        renderer_config.interpolate = realtime;
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
        // Scripted runs must not accept developer-UI input or vary their captures with its state.
        renderer_config.devtools_visible = !config.smoke && config.skill_vfx_capture >= kCombatSkillCount;
#ifdef HS_DEVELOPMENT_TOOLS
        RendererVfxSpriteBindings initial_sprite_bindings;
        auto initialized = PrepareVfxSpriteBindings(vfx_catalog, initial_sprite_bindings);
#else
        const auto &initial_sprite_bindings = shipping::Vfx().sprite_bindings;
        auto initialized = Result::Success();
#endif
        renderer_config.bleed_status_sprite = initial_sprite_bindings.bleed_status_sprite;
        renderer_config.burn_status_sprite = initial_sprite_bindings.burn_status_sprite;
        renderer_config.slow_status_sprite = initial_sprite_bindings.slow_status_sprite;
        renderer_config.mark_status_sprite = initial_sprite_bindings.mark_status_sprite;
        renderer_config.trap_sprite = initial_sprite_bindings.trap_sprite;
        renderer_config.fire_area_sprite = initial_sprite_bindings.fire_area_sprite;
        renderer_config.barrier_mode = config.barrier_mode;
        renderer_config.artifact_directory = config.artifact_directory;
#ifdef HS_DEVELOPMENT_TOOLS
        renderer_config.content_generation_id = render_generation->id;
        renderer_config.content_generation_lease = render_generation;
        renderer_config.product_paths = render_generation->product_paths;
        renderer_config.product_digests = render_generation->product_digests;
#else
        renderer_config.content_generation_id = deployment_id;
        renderer_config.content_generation_lease = shipped_content;
        renderer_config.packed_content = shipped_content;
        renderer_config.packed_bulks = shipping::RendererBulks();
        renderer_config.packed_textures = shipping::RendererTextures();
        renderer_config.packed_geometry = shipping::RendererGeometry();
        renderer_config.packed_materials = shipping::RendererMaterials();
        renderer_config.packed_characters = shipping::RendererCharacters();
        renderer_config.packed_animations = shipping::RendererAnimations();
        renderer_config.packed_vfx_mesh_atlas = &shipping::RendererVfxMeshAtlas();
        renderer_config.packed_source_revisions = shipping::RendererSourceRevisions();
        renderer_config.packed_revision_bulks = shipping::RendererRevisionBulks();
#endif

        runtime_detail::DiagnosticSample renderer_startup{};
        runtime_detail::DiagnosticScope renderer_startup_scope(renderer_startup, capture_clock);
        if (initialized) initialized = renderer.Initialize(renderer_config);
        renderer_startup_scope.Stop();
        if (capture_clock)
            if (auto written = WriteRendererStartup(config.artifact_directory / "renderer_startup.json",
                    renderer_startup.ticks * task_microseconds, renderer_startup.calls, initialized.Succeeded(),
                    renderer.GetStartupTimings(), renderer.GetPipelineCacheStats()); !written)
                record_thread_failure(written);
        renderer_config.font_bytes = {};
        std::vector<std::byte>().swap(startup_font);
        if (!initialized)
        {
            record_thread_failure(initialized);
            channels.render_ready.store(true, std::memory_order_release);
            channels.render_ready.notify_all();
            channels.NotifyMain();
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
        std::ofstream graphics_settings_file;
        std::optional<runtime_detail::DiagnosticLog> graphics_settings_lines;
        std::uint64_t graphics_config_revision{};
        std::string graphics_config_signature;
        bool graphics_capture_complete{true};
        if (config.capture_diagnostics)
        {
            graphics_settings_file.open(config.artifact_directory / "graphics_settings.csv", std::ios::binary);
            if (!graphics_settings_file)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Could not initialize graphics settings capture."));
            graphics_settings_lines.emplace(graphics_settings_file, true,
                "config_revision,settings_generation,pipeline_revision,setting_id,key,requested,effective,requested_available,native_effective,native_reason\n");
        }
        bool execution_receipt_pending{true};
        const auto record_renderer_features = [&](std::uint64_t settings_generation) {
            execution_receipt_pending = true;
            try
            {
                const auto receipt = RendererGraphicsReceipt(renderer, active_graphics, active_settings_generation,
                    static_cast<HWND>(window.Handle()), config.validation || config.gpu_validation);
                channels.renderer_graphics_ui.store(receipt, std::memory_order_release);
                channels.NotifyMain();
                if (graphics_settings_lines)
                {
                    const auto canonical = renderer.GetGraphicsSettingsStatus();
                    std::vector<std::string> rows;
                    std::string signature = std::format("{},{}\n", active_settings_generation, renderer.PipelineRevision());
                    for (const auto &descriptor : GraphicsSettingDescriptors())
                    {
                        const auto requested = GraphicsSettingToken(active_graphics, descriptor.id);
                        const auto effective = canonical.actual_applied_available
                            ? GraphicsSettingToken(canonical.actual_applied, descriptor.id) : std::string{};
                        const auto choice = std::ranges::find_if(receipt->choices, [&](const auto &entry) {
                            return entry.setting == descriptor.id && entry.token == requested;
                        });
                        // UI display/extent choices are suggestions, not native capability admissions.
                        constexpr std::array capability_settings{SettingId::ReconstructionProvider,
                            SettingId::ReconstructionQuality, SettingId::FrameGenerationProvider,
                            SettingId::FrameGenerationMode, SettingId::FrameGenerationMultiplier,
                            SettingId::HdrMode, SettingId::LowLatency,
                            SettingId::AoMode, SettingId::GiMode, SettingId::ReflectionMode, SettingId::ShadowMode,
                            SettingId::AdaptiveShading};
                        const bool capability_reported = std::ranges::find(capability_settings, descriptor.id) != capability_settings.end();
                        const auto native = std::ranges::find(receipt->resolved, descriptor.id, &GraphicsUiStatus::setting);
                        rows.push_back(std::format("{},{},{},{},{},{},{}\n",
                            static_cast<unsigned>(descriptor.id), runtime_detail::CsvField(descriptor.key),
                            runtime_detail::CsvField(requested), runtime_detail::CsvField(effective),
                            !capability_reported || choice == receipt->choices.end() ? -1 : choice->available ? 1 : 0,
                            runtime_detail::CsvField(native == receipt->resolved.end() ? std::string_view{} : native->effective),
                            runtime_detail::CsvField(native == receipt->resolved.end() ? std::string_view{} : native->reason)));
                        signature += rows.back();
                    }
                    if (signature != graphics_config_signature)
                    {
                        ++graphics_config_revision;
                        graphics_config_signature = std::move(signature);
                        for (const auto &row : rows)
                            graphics_settings_lines->Append(std::format("{},{},{},{}", graphics_config_revision,
                                active_settings_generation, renderer.PipelineRevision(), row));
                    }
                }
                if (!capture_diagnostics) return;
                const auto lighting = renderer.GetLightingStatus();
                const auto reconstruction = renderer.GetReconstructionStatus();
                const auto cache = renderer.GetPipelineCacheStats();
                std::ofstream log(config.artifact_directory / "renderer_features.jsonl", std::ios::app);
                log << std::format(
                    "{{\"settings_generation\":{},\"pipeline_revision\":{},"
                    "\"requested\":{{\"ao\":\"{}\",\"gi\":\"{}\",\"reflection\":\"{}\",\"shadow\":\"{}\"}},"
                    "\"active\":{{\"ao\":\"{}\",\"gi\":\"{}\",\"reflection\":\"{}\",\"shadow\":\"{}\"}},"
                    "\"fallback\":{{\"ao\":\"{}\",\"gi\":\"{}\",\"reflection\":\"{}\",\"shadow\":\"{}\"}},"
                    "\"reconstruction\":{{\"requested\":\"{}\",\"active\":\"{}\",\"availability\":\"{}\"}},"
                    "\"pso_cache\":{{\"library\":{},\"mesh_library\":{},\"hits\":{},\"misses\":{},\"native_creates\":{},"
                    "\"store_failures\":{},\"load_failures\":{},\"io_failures\":{},\"discarded_files\":{}}}}}\n",
                    settings_generation, renderer.PipelineRevision(),
                    GraphicsEnumToken(lighting.requested.ao_mode), GraphicsEnumToken(lighting.requested.gi_mode),
                    GraphicsEnumToken(lighting.requested.reflection_mode), GraphicsEnumToken(lighting.requested.shadow_mode),
                    GraphicsEnumToken(lighting.active.ao_mode), GraphicsEnumToken(lighting.active.gi_mode),
                    GraphicsEnumToken(lighting.active.reflection_mode), GraphicsEnumToken(lighting.active.shadow_mode),
                    lighting.ao_fallback_reason, lighting.gi_fallback_reason, lighting.reflection_fallback_reason,
                    lighting.shadow_fallback_reason, reconstruction.requested_provider.value,
                    reconstruction.effective_provider.value, reconstruction.requested_availability,
                    cache.library_supported, cache.mesh_library_supported, cache.hits, cache.misses, cache.native_creates,
                    cache.store_failures, cache.load_failures, cache.io_failures, cache.discarded_files);
            }
            catch (const std::bad_alloc &)
            {
                OutputDebugStringA("Graphics status receipt allocation failed; previous immutable receipt retained.\n");
                if (graphics_settings_lines)
                {
                    graphics_capture_complete = false;
                    record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime",
                        "Graphics settings capture allocation failed."));
                }
            }
        };
        record_renderer_features(0);
        auto previous_ui_presentation = renderer.GetPresentationStatus();
        auto previous_receipt_phase = channels.session_phase.load(std::memory_order_acquire);
        std::uint64_t telemetry_completed_frame{};
        bool telemetry_settings_open{};
        channels.render_ready.store(true, std::memory_order_release);
        channels.render_ready.notify_all();
        channels.NotifyMain();

        auto active_frame_cap = initial_renderer_settings.frame_cap;
        auto active_decorative_particle_fraction = renderer.DecorativeParticleFraction();
        auto active_borderless = initial_renderer_settings.borderless;
        auto active_vfx_quality = active_graphics.particle_quality == GraphicsQuality::Low
            ? VfxQuality::Low
            : active_graphics.particle_quality == GraphicsQuality::Medium
                ? VfxQuality::Medium : VfxQuality::High;
        bool renderer_settings_suspended{};
#if defined(HS_DEVELOPMENT_TOOLS)
        runtime_detail::GenerationReloadWatch generation_reload_watch;
        // The future joins on teardown; the worker owns only file inputs and its read lease.
        std::future<PreparedContentGeneration> generation_preparation;
        std::shared_ptr<const SelectedGeneration> generation_awaiting_frame;
        SceneGenerationBinding scene_generation{selected_generation.id, selected_generation.id, {}};
        auto next_generation_check = std::chrono::steady_clock::now();
        std::ofstream(config.artifact_directory / "shader_hot_reload.log", std::ios::app)
            << "ready active=" << render_generation->id << '\n';
#endif

        RenderSnapshotExchange::Consumer snapshot_consumer(channels.snapshots);
        runtime_detail::VfxSessionState vfx_session;
        runtime_detail::PreparedPairFacts vfx_pair_facts;
        runtime_detail::VfxTypedFrameInputs typed_vfx;
        std::vector<PresentationEvent> ready_events, frame_events, dispatched_event_scratch;
        std::vector<VfxScreenInput> typed_screen;
        std::vector<VfxVolumeInput> typed_volumes;
        std::vector<VfxLightInput> typed_lights;
        std::vector<VfxDistortionInput> typed_distortions;
        std::vector<VfxDecalInput> typed_decals;
        std::vector<VfxEntitySurfaceCommand> typed_surfaces;
        std::vector<VfxOwnedMeshInput> typed_mesh_spawns;
        std::vector<VfxFresnelInput> typed_fresnels;
        std::vector<runtime_detail::VfxTypedGroundCommand> ground_commands;
        std::vector<VfxGroundSpawnInput> typed_ground_spawns;
        std::vector<VfxFlashSpawnInput> visible_flashes;
        std::vector<VfxFlashSpawnInput> status_stamp_flashes;
        std::vector<runtime_detail::VfxPreparedFireSource> prepared_fire_sources;
        runtime_detail::VfxRibbonInputScratch ribbon_inputs;
        runtime_detail::DecodedVfxState prepared_births;
        std::vector<runtime_detail::VfxEntitySurfaceEventCapture> surface_capture_inputs;
        std::vector<runtime_detail::VfxEntitySurfacePersistentCapture> surface_persistent;
        std::vector<runtime_detail::VfxGroundBirthSuppressionFacts> ground_birth_suppressions;
        std::vector<Sequence> replaced_particle_sequences, superseded_pickup_sequences, owned_status_sequences;
        std::vector<Sequence> owned_status_mote_sequences;
        std::vector<Sequence> replaced_legacy_mesh_sequences, replaced_burst_shell_sequences;
        std::vector<std::size_t> vfx_row_indices;
        std::vector<runtime_detail::VfxSessionState::PersistentBurstKey> status_burst_keys;
        std::vector<ParticleSpawnCommand> status_shard_commands;
        const auto explosive_arrow_main = MakeAssetId("particle.skill.explosive_arrow.main").value;
        auto &active_effects = vfx_session.active_effects;
        using VfxEventStore = runtime_detail::ActiveEffectStore;
        using VfxEventProfile = VfxEventStore::Profile;
        std::vector<VfxEventStore::AppendRequest> profile_additions;
        VfxEventStore::PreparedAppendBatch prepared_profiles;
        const auto profile_bit = [](VfxEventProfile profile) -> std::uint16_t {
            return static_cast<std::uint16_t>(1u << static_cast<unsigned>(profile));
        };
        runtime_detail::VfxPersistentSparkState persistent_sparks;
        runtime_detail::VfxUpgradeMovementState vfx_upgrade_movements;
        const auto vfx_workspace_bytes = [&]() noexcept {
            using namespace runtime_detail::vfx_retained_size;
            auto bytes = vfx_session.TrackedLogicalCapacityBytes();
            const auto charge = [&](std::size_t owned) { bytes = Add(bytes, owned); };
            const auto vectors = [&](const auto &...items) { (charge(Vector(items)), ...); };
            charge(vfx_pair_facts.RetainedCapacityBytes());
            charge(prepared_births.LogicalCapacityBytes());
            charge(prepared_profiles.LogicalCapacityBytes());
            charge(persistent_sparks.RetainedCapacityBytes());
            charge(vfx_upgrade_movements.RetainedCapacityBytes());
            vectors(typed_vfx.events, typed_vfx.persistent, typed_vfx.unsupported_effect_ids,
                ready_events, frame_events, dispatched_event_scratch, typed_screen, typed_volumes,
                typed_lights, typed_distortions, typed_decals, typed_surfaces, typed_mesh_spawns,
                typed_fresnels, ground_commands, typed_ground_spawns, visible_flashes,
                status_stamp_flashes, prepared_fire_sources, surface_capture_inputs,
                surface_persistent, ground_birth_suppressions, replaced_particle_sequences,
                superseded_pickup_sequences, owned_status_sequences, owned_status_mote_sequences,
                replaced_legacy_mesh_sequences, replaced_burst_shell_sequences, vfx_row_indices,
                status_burst_keys, status_shard_commands, profile_additions);
            for (const auto &input : typed_vfx.events) charge(RingGapPayload(input.payload));
            for (const auto &input : typed_vfx.persistent) charge(RingGapPayload(input.payload));
            for (const auto &capture : surface_capture_inputs) charge(RingGapPayload(capture.input.payload));
            for (const auto &capture : surface_persistent) charge(RingGapPayload(capture.input.payload));
            for (const auto &command : ground_commands) charge(Vector(command.geometry.gap_angles_radians));
            for (const auto &command : typed_ground_spawns) charge(Vector(command.geometry.gap_angles_radians));
            for (const auto &command : typed_distortions) charge(Vector(command.gap_angles_degrees));
            charge(Multiply(ribbon_inputs.StorageCapacity(), sizeof(VfxRibbonSourceInput)));
            for (const auto &source : ribbon_inputs.Storage()) charge(Vector(source.outputs));
            return bytes;
        };
        auto &pending_particle_spawns = vfx_session.decoded.particle_spawns;
        auto &pending_effect_lines = vfx_session.decoded.effect_lines;
        auto &active_vfx_flashes = vfx_session.decoded.flashes;
        const auto write_diagnostics_status = [&](std::string_view text) {
            std::ofstream status(config.artifact_directory / "diagnostics.json", std::ios::binary);
            status.write(text.data(), static_cast<std::streamsize>(text.size()));
            status.close();
            return !status.fail();
        };
        const bool status_started = !capture_diagnostics || write_diagnostics_status(
            "{\"complete\":false,\"reason\":\"running\"}\n");
        if (!status_started)
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                "Could not initialize diagnostic status."));
        std::ofstream event_file, timeline_file, gpu_file;
        if (capture_diagnostics)
        {
            event_file.open(config.artifact_directory / "events.ndjson", std::ios::binary);
            timeline_file.open(config.artifact_directory / "timeline.csv", std::ios::binary);
            gpu_file.open(config.artifact_directory / "gpu_passes.csv", std::ios::binary);
        }
        runtime_detail::DiagnosticLog event_lines(event_file, capture_diagnostics, {}, capture_clock);
        runtime_detail::DiagnosticLog timeline_lines(timeline_file, capture_diagnostics,
            "frame,tick,cpu_us,memory_bytes,ecs_count,queue_depth\n", capture_clock);
        runtime_detail::DiagnosticLog gpu_lines(gpu_file, capture_diagnostics,
            "frame,tick,pass,gpu_ns,queue,measured,cpu_callback_begin_qpc,cpu_callback_end_qpc,cpu_callback_valid,cpu_qpc_frequency,completed_frame,view_id\n", capture_clock);
        std::ofstream performance_file, gpu_live_file;
        std::optional<runtime_detail::DiagnosticLog> performance_lines, gpu_live_lines;
        runtime_detail::PerformanceLogState performance_state;
        bool performance_ready_published{};
        std::uint64_t performance_frequency{};
        if (config.capture_diagnostics)
        {
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Could not calibrate performance capture clock."));
            else
                performance_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
            performance_file.open(config.artifact_directory / "performance.csv", std::ios::binary);
            gpu_live_file.open(config.artifact_directory / "gpu_live.csv", std::ios::binary);
            if (!performance_file || !gpu_live_file)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Could not initialize performance capture files."));
            performance_lines.emplace(performance_file, true,
                "frame,tick,frame_qpc,qpc_frequency,interval_us,adapter_us,render_us,wait_us,streaming_us,scene_prepare_us,ui_us,constants_upload_us,graph_build_us,graph_prepare_us,descriptors_us,graph_plan_us,graph_frontend_reused,graph_record_us,graph_submit_us,post_submit_us,present_us,diagnostics_us,wrapper_us,gpu_frame,gpu_valid,gpu_critical_path_ms,gpu_critical_path_valid,foreground,dynamic_instances,static_instances,session_phase,frame_cap,config_revision,settings_generation,pipeline_revision,view_id,output_width,output_height,render_active_width,render_active_height,render_capacity_width,render_capacity_height,diagnostic_frame,renderer_diagnostics_valid,submit_qpc,present_begin_qpc,present_end_qpc,renderer_qpc_frequency,memory_budget_available,local_usage_bytes,local_budget_bytes,tracked_live_bytes,tracked_retiring_bytes,texture_physical_bytes,host_staging_bytes,direct_batches,compute_batches,copy_batches,cross_queue_waits,record_jobs,transient_bytes_without_aliasing,transient_backing_bytes,validation_enabled,gpu_validation_enabled,frame_slot_wait_begin_qpc,frame_slot_wait_end_qpc,graph_prepare_validation_us,graph_prepare_info_key_us,graph_prepare_compile_us,graph_prepare_transient_us,streamline_actual_present_count,streamline_actual_present_count_valid,streamline_actual_present_begin_qpc,streamline_actual_present_end_qpc,streamline_actual_present_qpc_frequency,native_latency_active,latency_begin_us,input_wait_us,simulation_wait_us,retained_vfx_us,ribbon_prepare_us,particle_spawn_prepare_us,typed_vfx_upload_prepare_us,particle_upload_prepare_us,local_vfx_prepare_us,palette_prepare_us,skin_prepare_us,scene_reconcile_us,scene_gpu_prepare_us,session_id,event_inputs,typed_event_inputs,persistent_inputs,particle_spawn_commands,effect_line_commands,ground_inputs,flash_inputs,mesh_inputs,fresnel_inputs,ribbon_inputs,light_inputs,distortion_inputs,decal_inputs,volume_inputs,screen_inputs,entity_surface_inputs,previous_capture_log_us,capture_log_io_us_total,capture_log_io_calls_total,capture_log_bytes_total" + TaskColumns(kVfxTaskNames) + ",ribbon_state_us,ribbon_wait_us,ribbon_allocation_us,ribbon_replacement_count,ribbon_allocation_bytes,ribbon_prepare_thread_cpu_us,ribbon_prepare_thread_cpu_valid,graph_resources_us,graph_base_passes_us,graph_shadows_us,graph_light_probes_us,graph_scene_selection_us,graph_ao_us,graph_opaque_us,graph_hero_us,graph_post_us,scene_skin_prepare_us,scene_shadow_prepare_us,scene_cluster_prepare_us,scene_geometry_demand_us,scene_texture_prepare_us,scene_upload_plan_us,target_render_fps,vfx_fire_admission_bytes,vfx_fire_requested,vfx_fire_admitted,vfx_fire_budget_limited\n", capture_clock);
            gpu_live_lines.emplace(gpu_live_file, true,
                "observed_frame,completed_frame,view_id,critical_path_ms,critical_path_valid,cpu_recording_ms,cpu_recording_valid,gpu_limited,fence_completed,pass_id,queue,nanoseconds,measured,begin_ticks,end_ticks,timestamp_frequency,begin_qpc,end_qpc,qpc_frequency,clock_calibrated,pass_name,gpu_timings_valid,drained,cpu_qpc_frequency,cpu_record_begin_qpc,cpu_record_end_qpc,cpu_record_valid,cpu_provider_submit_begin_qpc,cpu_provider_submit_end_qpc,cpu_provider_submit_valid,cpu_callback_begin_qpc,cpu_callback_end_qpc,cpu_callback_valid,frame_metric_kind,metric_generation,timing_resize_epoch,timing_render_width,timing_render_height,timing_main_view,timing_frame_generation_enabled,output_ready_valid,output_ready_ticks,output_ready_frequency,output_ready_qpc,output_ready_qpc_frequency,output_ready_clock_calibrated,source_frame_id,source_frame_sequence,quality_generation,provider_generation,is_generated_frame,clock_gpu_before,clock_cpu_before,clock_gpu_after,clock_cpu_after,clock_gpu_frequency,clock_cpu_frequency,clock_anchors_valid,source_ready_cadence_ms,output_dependency_span_ms,calibration_valid,calibration_drift,cadence_valid,span_valid,gpu_backlog_proven,cadence_gap,closure_span_gap,cadence_exclusions_valid,cpu_supply_gap,pacing_gap,closure_clock_valid,closure_gap_checked\n", capture_clock);
        }
        bool gpu_drain_succeeded{true};
        std::string gpu_live_row;
#if defined(HS_DEVELOPMENT_TOOLS)
        bool hero_drain_succeeded{true};
        std::unique_ptr<runtime_detail::HeroWorkLog> hero_lines;
        if (config.capture_hero_work)
        {
            hero_lines = std::make_unique<runtime_detail::HeroWorkLog>(config.artifact_directory, capture_clock);
            if (hero_lines->Failed()) record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Could not initialize isolated Hero work capture."));
        }
#endif
        std::uint64_t invalid_gpu_frames{};
        const auto append_gpu_capture = [&](const GpuPassTimings &gpu, std::uint64_t observed_frame, bool drained) {
            if (!gpu_live_lines || !config.capture_gpu_timings) return;
            if (!performance_state.AcceptGpuFrame(gpu.completed_frame_id, !gpu.passes.empty(), gpu.fence_completed)) return;
            if (!gpu.valid) ++invalid_gpu_frames;
            for (const auto &pass : gpu.passes)
            {
                gpu_live_row.clear();
                runtime_detail::AppendCsvValues(gpu_live_row,
                    observed_frame,
                    gpu.completed_frame_id,
                    gpu.view_id,
                    gpu.critical_path_ms,
                    static_cast<unsigned>(gpu.critical_path_valid),
                    gpu.cpu_recording_ms,
                    static_cast<unsigned>(gpu.cpu_recording_valid),
                    static_cast<unsigned>(gpu.gpu_limited),
                    static_cast<unsigned>(gpu.fence_completed),
                    pass.pass_id,
                    static_cast<unsigned>(pass.queue),
                    pass.nanoseconds,
                    static_cast<unsigned>(pass.measured),
                    pass.begin_ticks,
                    pass.end_ticks,
                    pass.timestamp_frequency,
                    pass.begin_qpc,
                    pass.end_qpc,
                    pass.qpc_frequency,
                    static_cast<unsigned>(pass.clock_calibrated));
                runtime_detail::AppendCsvText(gpu_live_row, pass.name);
                runtime_detail::AppendCsvValues(gpu_live_row,
                    static_cast<unsigned>(gpu.valid),
                    static_cast<unsigned>(drained),
                    gpu.cpu_qpc_frequency,
                    pass.cpu_record_begin_qpc,
                    pass.cpu_record_end_qpc,
                    static_cast<unsigned>(pass.cpu_record_valid),
                    pass.cpu_provider_submit_begin_qpc,
                    pass.cpu_provider_submit_end_qpc,
                    static_cast<unsigned>(pass.cpu_provider_submit_valid),
                    pass.cpu_callback_begin_qpc, pass.cpu_callback_end_qpc, static_cast<unsigned>(pass.cpu_callback_valid));
                runtime_detail::AppendCsvText(gpu_live_row,
                    gpu.metric_kind == GpuFrameTimingKind::ReferenceGraphSpan ? "reference_graph_span" :
                        gpu.metric_kind == GpuFrameTimingKind::SourceOutputReady ? "source_output_ready" : "unavailable");
                runtime_detail::AppendCsvValues(gpu_live_row,
                    gpu.metric_generation, gpu.resize_epoch, gpu.render_active.width, gpu.render_active.height,
                    static_cast<unsigned>(gpu.main_view), static_cast<unsigned>(gpu.frame_generation_enabled),
                    static_cast<unsigned>(gpu.output_ready_valid), gpu.output_ready_ticks, gpu.output_ready_frequency,
                    gpu.output_ready_qpc, gpu.output_ready_qpc_frequency, static_cast<unsigned>(gpu.output_ready_clock_calibrated),
                    gpu.source_frame_id, gpu.source_frame_sequence, gpu.quality_generation,
                    gpu.provider_generation, static_cast<unsigned>(gpu.generated_frame),
                    gpu.clock_gpu_before, gpu.clock_cpu_before, gpu.clock_gpu_after, gpu.clock_cpu_after,
                    gpu.clock_gpu_frequency, gpu.clock_cpu_frequency, static_cast<unsigned>(gpu.clock_anchors_valid),
                    gpu.source_ready_cadence_ms, gpu.output_dependency_span_ms,
                    static_cast<unsigned>(gpu.calibration_valid), static_cast<unsigned>(gpu.calibration_drift),
                    static_cast<unsigned>(gpu.cadence_valid), static_cast<unsigned>(gpu.span_valid),
                    static_cast<unsigned>(gpu.gpu_backlog_proven), static_cast<unsigned>(gpu.cadence_gap),
                    static_cast<unsigned>(gpu.closure_span_gap),
                    static_cast<unsigned>(gpu.cadence_exclusions_valid),
                    static_cast<unsigned>(gpu.cpu_supply_gap), static_cast<unsigned>(gpu.pacing_gap),
                    static_cast<unsigned>(gpu.closure_clock_valid), static_cast<unsigned>(gpu.closure_gap_checked));
                gpu_live_row += '\n';
                gpu_live_lines->Append(gpu_live_row);
            }
        };
        const auto collect_gpu_capture = [&](std::uint64_t observed_frame, bool drain) -> Result {
            if (!gpu_live_lines) return Result::Success();
            std::vector<GpuPassTimings> completed;
            const auto collected = drain ? renderer.DrainGpuPassTimings(completed)
                                         : renderer.ConsumeCompletedGpuPassTimings(completed);
            if (!collected)
            {
                gpu_drain_succeeded = false;
                return collected;
            }
            for (const auto &gpu : completed) append_gpu_capture(gpu, observed_frame, drain);
#if defined(HS_DEVELOPMENT_TOOLS)
            if (hero_lines)
            {
                try
                {
                    std::vector<RendererHeroWorkSample> samples;
                    const auto captured = drain ? renderer.DrainHeroWork(samples) : renderer.ConsumeCompletedHeroWork(samples);
                    if (!captured) { hero_drain_succeeded = false; return captured; }
                    for (const auto &sample : samples) hero_lines->Append(sample);
                    if (hero_lines->Failed()) return Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Hero work capture write failed.");
                }
                catch (const std::exception &error)
                {
                    hero_drain_succeeded = false;
                    return Result::Failure(ErrorCode::TaskFailed, "hs_runtime", "Hero work capture failed: " + std::string(error.what()));
                }
            }
#endif
            return Result::Success();
        };
        const auto drain_gpu_capture = [&]() -> Result {
            return collect_gpu_capture(channels.rendered_frames.load(std::memory_order_acquire), true);
        };
        bool frame_generation_allowed{};
        const auto set_frame_generation_allowed = [&](bool allowed) -> Result {
            if (frame_generation_allowed == allowed) return Result::Success();
            if (renderer.GetPresentationStatus().requested.mode != GraphicsFrameGenerationMode::Off)
                if (auto drained = drain_gpu_capture(); !drained) return drained;
            if (auto changed = renderer.SetFrameGenerationAllowed(allowed); !changed) return changed;
            frame_generation_allowed = allowed;
            record_renderer_features(0);
            return Result::Success();
        };
        const auto read_performance_qpc = [] {
            LARGE_INTEGER qpc{};
            QueryPerformanceCounter(&qpc);
            return static_cast<std::uint64_t>(qpc.QuadPart);
        };
        pending_particle_spawns.reserve(256);
        pending_effect_lines.reserve(64);

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
        std::uint64_t vfx_fire_budget_limited_frames{};
        std::size_t vfx_fire_admission_peak_bytes{};
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
        runtime_detail::DiagnosticSample previous_capture_log{};
        std::string performance_row;
        for (;;)
        {
            std::array<runtime_detail::DiagnosticSample, kVfxTaskNames.size()> vfx_work{};
            const auto loop_start_qpc = performance_lines ? read_performance_qpc() : 0;
#if defined(HS_DEVELOPMENT_TOOLS)
            const auto generation_now = std::chrono::steady_clock::now();
            const auto generation_ready = generation_preparation.valid() &&
                generation_preparation.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            std::string pointer_text;
            bool pointer_read{};
            if (generation_now >= next_generation_check || generation_ready)
            {
                next_generation_check = generation_now + std::chrono::milliseconds(250);
                pointer_read = static_cast<bool>(ReadGenerationPointer(generation_pointer, pointer_text));
                if (pointer_read) generation_reload_watch.Observe(pointer_text, generation_now);
            }
            if (generation_ready)
            {
                PreparedContentGeneration prepared;
                try
                {
                    prepared = generation_preparation.get();
                }
                catch (const std::exception &error)
                {
                    prepared.result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Content Generation worker failed: " + std::string(error.what()));
                }
                auto &candidate = prepared.candidate;
                auto reloaded = std::move(prepared.result);
                bool current = pointer_read && !prepared.superseded &&
                    generation_reload_watch.IsCurrent(pointer_text);
                {
                    std::string candidate_id = "unresolved";
                    const char *stage = prepared.stage;
                    const auto pipeline_revision = renderer.PipelineRevision();
                    const auto parameter_revision = renderer.MaterialParameterRevision();
                    bool applied{}, catalog_changed{};
                    runtime_detail::VfxGenerationRebindCounts rebound;
                    std::uint64_t retired_legacy{}, retired_movements{}, retired_sparks{};
                    try
                    {
                        if (!candidate.id.empty()) candidate_id = candidate.id;
                        if (current && reloaded && candidate.scope != render_generation->scope)
                            reloaded = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                                       "Content Generation changes the selected scope.");
                        if (current && reloaded && candidate.id != render_generation->id)
                        {
                            std::filesystem::path next_particle_path, next_program_path;
                            VfxCatalog replacement;
                            RendererVfxSpriteBindings sprite_bindings;
                            runtime_detail::DecodedVfxState prepared_decoded;
                            runtime_detail::ActiveEffectStore prepared_active_effects = active_effects;
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
                                prepared_decoded.entity_surface_captures = vfx_session.decoded.entity_surface_captures;
                                std::erase_if(prepared_decoded.entity_surface_captures, [&](auto &capture) {
                                    const auto captured_generation = std::static_pointer_cast<const SelectedGeneration>(
                                        capture.generation_lease);
                                    if (!captured_generation || !runtime_detail::SameCapturedAssetClosure(
                                            *captured_generation, candidate, capture.instance.asset_id))
                                    {
                                        rebound.Record(runtime_detail::VfxGenerationRebindOutcome::Incompatible);
                                        return true;
                                    }
                                    return false;
                                });
                                if (catalog_changed)
                                {
                                    prepared_active_effects.RebindRows([&](VfxEventInput &input) {
                                        std::vector<VfxEventInput> one{input};
                                        runtime_detail::RebindVfxGenerationEvents(previous, next, one, rebound);
                                        if (one.empty()) return false;
                                        input = std::move(one.front());
                                        return true;
                                    }, prepared_decoded.entity_surface_captures);
                                    std::vector<VfxEventInput> rebound_ground_events;
                                    prepared_active_effects.CopyProfileForRebind(
                                        runtime_detail::ActiveEffectStore::Profile::Ground,
                                        rebound_ground_events);
                                    const auto ground_count = rebound_ground_events.size();
                                    const auto &next_image = catalog_changed ? replacement.Image() : vfx_catalog.Image();
                                    runtime_detail::RebindVfxGroundEventOwners(next_image, rebound_ground_events);
                                    const auto retired_ground = ground_count - rebound_ground_events.size();
                                    rebound.retained -= retired_ground;
                                    rebound.incompatible += retired_ground;
                                    prepared_active_effects.RebindProfile(
                                        runtime_detail::ActiveEffectStore::Profile::Ground,
                                        rebound_ground_events);
                                    prepared_movements = vfx_upgrade_movements;
                                    retired_movements = prepared_movements.Rebind(previous, next);
                                    prepared_sparks = persistent_sparks;
                                    retired_sparks = prepared_sparks.Rebind(previous, next);
                                    // Expanded legacy commands have no stable effect identity.
                                    // Empty prepared queues retire them at the same commit.
                                    retired_legacy = vfx_session.decoded.particle_spawns.size() +
                                        vfx_session.decoded.effect_lines.size() + vfx_session.decoded.flashes.size() +
                                        vfx_session.native_batch.particle_spawns.size() + vfx_session.native_batch.effect_lines.size();
                                }
                            }
                            if (reloaded)
                            {
                                stage = "renderer_prepare";
                                auto next_generation = std::make_shared<const SelectedGeneration>(candidate);
                                // The closure includes shader/parameter leaves: the renderer alone
                                // classifies narrow reloads, which keep the existing scene association.
                                bool scene_compatible = true;
                                for (const std::string_view id : std::array<std::string_view, 4>{"game-data/simulation-rules",
                                         "game-data/presentation-catalog", "game-data/arena-layout",
                                         std::string_view(renderer_config.scene_hlod_product_id)})
                                    scene_compatible &= runtime_detail::SameCapturedAssetClosure(
                                        *render_generation, candidate, MakeAssetId(id));
                                SceneGenerationBinding next_scene_generation{selected_generation.id, candidate.id, {}};
                                const auto scene_snapshots = snapshot_consumer.AcquireLatest();
                                if (scene_snapshots.has_previous)
                                    (void)next_scene_generation.Resolve(scene_snapshots.previous.static_scene);
                                if (scene_snapshots.has_current)
                                    (void)next_scene_generation.Resolve(scene_snapshots.current.static_scene);
                                bool resident_replaced{};
                                pointer_read = static_cast<bool>(ReadGenerationPointer(generation_pointer, pointer_text));
                                if (pointer_read)
                                    generation_reload_watch.Observe(pointer_text, std::chrono::steady_clock::now());
                                current = pointer_read && generation_reload_watch.IsCurrent(pointer_text);
                                if (current)
                                {
                                    if (!scene_compatible) stage = "scene_closure";
                                    reloaded = drain_gpu_capture();
                                    if (!reloaded) record_thread_failure(reloaded);
                                    if (reloaded)
                                        reloaded = renderer.ReloadContentGeneration(candidate.id, candidate.product_paths,
                                            candidate.product_digests, next_generation, catalog_changed ? &sprite_bindings : nullptr,
                                            &resident_replaced, scene_compatible);
                                }
                                if (current && reloaded)
                                {
                                    // All allocation, decoding and rebinding preceded the
                                    // renderer commit. Publish only prepared owned state.
                                    if (catalog_changed)
                                    {
                                        vfx_catalog.Swap(replacement);
                                        active_effects.Swap(prepared_active_effects);
                                        vfx_session.SwapDecoded(prepared_decoded);
                                        vfx_session.DiscardNativeBatch();
                                        vfx_session.particle_reset_pending = true;
                                        vfx_upgrade_movements.Swap(prepared_movements);
                                        persistent_sparks.Swap(prepared_sparks);
                                        ++vfx_catalog_generation;
                                    }
                                    else
                                        vfx_session.decoded.entity_surface_captures.swap(
                                            prepared_decoded.entity_surface_captures);
                                    cooked_particle_effects_path.swap(next_particle_path);
                                    cooked_vfx_program_path.swap(next_program_path);
                                    render_generation.swap(next_generation);
                                    if (resident_replaced) scene_generation.Swap(next_scene_generation);
                                    active_decorative_particle_fraction = renderer.DecorativeParticleFraction();
                                    generation_awaiting_frame = render_generation;
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
                    generation_reload_watch.Complete(static_cast<bool>(reloaded), current,
                        std::chrono::steady_clock::now());
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
                    if (current && !reloaded)
                        std::ofstream(config.artifact_directory / "shader_hot_reload.log", std::ios::app)
                            << "rejected candidate=" << candidate_id << " active=" << render_generation->id
                            << " stage=" << stage << ' ' << reloaded.Message() << "; previous content retained\n";
                }
            }
            if (pointer_read && generation_reload_watch.TryBegin(std::chrono::steady_clock::now()))
            {
                try
                {
                    generation_preparation = std::async(std::launch::async, PrepareContentGeneration,
                        generation_pointer, pointer_text, render_generation->id, render_generation->scope);
                }
                catch (const std::exception &error)
                {
                    generation_reload_watch.Complete(false, true, std::chrono::steady_clock::now());
                    std::ofstream(config.artifact_directory / "shader_hot_reload.log", std::ios::app)
                        << "rejected candidate=unresolved active=" << render_generation->id
                        << " stage=worker " << error.what() << "; previous content retained\n";
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
            if (apply_renderer_settings || (resize_width && resize_height))
                if (auto drained = drain_gpu_capture(); !drained)
                {
                    record_thread_failure(drained);
                    break;
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
                channels.NotifyMain();
            }

            const bool active_gameplay = AllowGameplayFrameGeneration(static_cast<SessionPhase>(
                channels.session_phase.load(std::memory_order_acquire)));
            if (auto policy = set_frame_generation_allowed(active_gameplay); !policy)
            { record_thread_failure(policy); break; }
            const bool has_current_snapshot = !renderer_settings_suspended && snapshot_consumer.AcquireLatest().has_current;
            std::uint64_t input_token{};
            const auto latency_begin_qpc = performance_lines ? read_performance_qpc() : 0;
            std::uint64_t input_wait_ticks{}, simulation_wait_ticks{};
            // Evaluate Auto cadence even while its native latency/FG owner is Off,
            // so a recovered base cadence can re-enable the existing request.
            if (has_current_snapshot)
                if (auto begun = renderer.BeginInputFrame(input_token); !begun)
                { record_thread_failure(begun); break; }
            const auto latency_begin_ticks = performance_lines
                ? runtime_detail::PerformanceLogState::ElapsedTicks(latency_begin_qpc, read_performance_qpc()) : 0;
            auto current_presentation = renderer.GetPresentationStatus();
            const bool native_latency = current_presentation.low_latency_active;
            if (current_presentation.effective_provider != previous_ui_presentation.effective_provider ||
                current_presentation.active_mode != previous_ui_presentation.active_mode ||
                current_presentation.maximum_multiplier != previous_ui_presentation.maximum_multiplier ||
                current_presentation.low_latency_active != previous_ui_presentation.low_latency_active ||
                current_presentation.availability != previous_ui_presentation.availability ||
                current_presentation.fallback_reason != previous_ui_presentation.fallback_reason)
            {
                previous_ui_presentation = std::move(current_presentation);
                record_renderer_features(0);
            }
            const bool has_native_snapshot = native_latency && has_current_snapshot;
            if (native_latency && has_native_snapshot && config.smoke)
            {
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Native input latency requires actual window input; smoke playback has stopped that input source."));
                break;
            }
            native_input_frame.SetEnabled(native_latency && has_native_snapshot);
            if (native_latency && has_native_snapshot)
            {
                const auto token = input_token;
                if (!native_input_frame.RequestInput(token))
                {
                    if (!channels.stop_requested.load(std::memory_order_acquire))
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Native input frame request violated its acknowledged token order."));
                    break;
                }
                const auto input_wait_begin = performance_lines ? read_performance_qpc() : 0;
                if (!native_input_frame.WaitInput(token)) break;
                if (performance_lines) input_wait_ticks = runtime_detail::PerformanceLogState::ElapsedTicks(input_wait_begin, read_performance_qpc());
                if (auto sampled = renderer.InputSampled(token); !sampled) { record_thread_failure(sampled); break; }
                if (!native_input_frame.RequestSimulation(token))
                {
                    if (!channels.stop_requested.load(std::memory_order_acquire))
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Native simulation request did not match acknowledged input."));
                    break;
                }
                const auto simulation_wait_begin = performance_lines ? read_performance_qpc() : 0;
                if (!native_input_frame.WaitSimulation(token)) break;
                if (performance_lines) simulation_wait_ticks = runtime_detail::PerformanceLogState::ElapsedTicks(simulation_wait_begin, read_performance_qpc());
                if (auto simulated = renderer.SimulationCompleted(token); !simulated) { record_thread_failure(simulated); break; }
            }
            // AcquireLatest below observes the simulation's completed publication.
            runtime_detail::DiagnosticScope event_drain_scope(vfx_work[0], capture_clock);
            std::size_t event_count{};
            bool vfx_event_overflow{};
            PresentationEvent event;
            while (event_count < 256 && vfx_session.raw_events.HasCapacity() &&
                   channels.presentation_events.TryPop(event))
            {
                ++event_count;
                if (event.kind == PresentationKind::Vfx)
                    if (!vfx_session.raw_events.Enqueue(event))
                    {
                        vfx_event_overflow = true;
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                            "Bounded VFX raw-event scheduler is full."));
                        break;
                    }
                if (capture_diagnostics) event_lines.Append(
                    std::format("{{\"sequence\":{},\"tick\":{},\"kind\":{},"
                                "\"asset\":{},\"position\":[{},{},{}]}}\n",
                                event.sequence, event.tick, static_cast<unsigned>(event.kind),
                                event.asset.value, event.position.x, event.position.y,
                                event.position.z));
            }
            event_drain_scope.Stop();
            if (vfx_event_overflow) break;
            runtime_detail::DiagnosticScope snapshot_scope(vfx_work[1], capture_clock);
            auto snapshots = snapshot_consumer.AcquireLatest();
#if defined(HS_DEVELOPMENT_TOOLS)
            if (snapshots.has_previous)
                snapshots.previous.static_scene = scene_generation.Resolve(snapshots.previous.static_scene);
            if (snapshots.has_current)
                snapshots.current.static_scene = scene_generation.Resolve(snapshots.current.static_scene);
#endif
            snapshot_scope.Stop();
            if (renderer_settings_suspended)
            {
                std::this_thread::yield();
                continue;
            }
            if (snapshots.has_current &&
                (!config.smoke_render_lockstep ||
                 snapshots.current.header.tick > rendered_tick.load(std::memory_order_acquire)))
            {
                runtime_detail::DiagnosticScope session_scope(vfx_work[2], capture_clock);
                const auto &current_snapshot = snapshots.current;
                const auto sample_time = std::chrono::steady_clock::now();
                const auto sample_alpha = RenderSnapshotInterpolation(snapshots, sample_time, realtime);
                const double display_tick = static_cast<double>(current_snapshot.header.tick) -
                    (snapshots.has_previous && sample_alpha < 1.0f ? 1.0 - sample_alpha : 0.0);
                if (vfx_session.Synchronize(current_snapshot.header.session_id))
                {
                    persistent_sparks.Clear();
                    vfx_upgrade_movements.Clear();
                    vfx_showcase_injected = false;
                }
                prepared_births.Clear();
                const auto retained_particle_count = pending_particle_spawns.size();
                const auto &ready_batch = vfx_session.raw_events.Prepare(
                    current_snapshot.header.tick, vfx_session.session_id);
                ready_events.clear();
                ready_events.reserve(ready_batch.entries.size());
                for (const auto &entry : ready_batch.entries) ready_events.push_back(entry.event);
                if (auto dispatched = runtime_detail::DispatchVfxUpgradeEvents(
                        vfx_catalog.Program(), ready_events, frame_events, &dispatched_event_scratch); !dispatched)
                {
                    record_thread_failure(dispatched);
                    break;
                }
                const bool inject_pickup_showcase = config.vfx_showcase && !vfx_showcase_injected;
                if (inject_pickup_showcase)
                {
                    constexpr std::array pickup_effects{
                        "particle.pickup.xp_collect", "particle.pickup.heal_collect",
                        "particle.pickup.magnet_collect", "particle.pickup.relic_collect"};
                    for (std::size_t index = 0; index < pickup_effects.size(); ++index)
                    {
                        PresentationEvent event;
                        event.sequence = 0xF100u + index;
                        event.tick = current_snapshot.header.tick;
                        event.session_id = current_snapshot.header.session_id;
                        event.kind = PresentationKind::Vfx;
                        event.asset = MakeAssetId(pickup_effects[index]);
                        event.position = {(static_cast<float>(index % 2) - .5f) * 4.0f,
                                          .35f, (static_cast<float>(index / 2) - .5f) * 4.0f};
                        event.parameters = EncodeVfxParameters({{0, 0, 1}, 1,
                            {event.position.x + .9f, .6f, event.position.z + .6f},
                            static_cast<std::uint32_t>(VfxEventFlag::HasTarget)});
                        frame_events.push_back(event);
                    }
                }
                session_scope.Stop();
                if (config.resize_test && !resized &&
                    snapshots.current.header.tick >= config.maximum_ticks / 2)
                {
                    if (auto drained = drain_gpu_capture(); !drained)
                    {
                        record_thread_failure(drained);
                        break;
                    }
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
                const bool new_vfx_pair = vfx_pair_facts.Prepare({
                    .session_id = snapshots.current.header.session_id,
                    .previous_publication = snapshots.has_previous
                        ? (snapshots.previous.header.dynamic_pair_revision
                            ? snapshots.previous.header.dynamic_pair_revision : snapshots.previous_publication_sequence)
                        : Sequence{},
                    .current_publication = snapshots.current.header.dynamic_pair_revision
                        ? snapshots.current.header.dynamic_pair_revision : snapshots.current_publication_sequence},
                    previous_vfx, current_vfx, snapshots.current.instances,
                    snapshots.current.captured_instances);
                if (snapshots.has_current && (new_vfx_pair ||
                    (vfx_session.has_status_burst_tick &&
                     snapshots.current.header.tick < vfx_session.last_status_burst_tick)))
                {
                    vfx_session.RetireUnownedPersistentBursts(true,
                        snapshots.current.header.tick, [&](const auto &key) noexcept {
                            const auto kind = static_cast<PersistentVfxKind>(key.visual_kind);
                            return vfx_pair_facts.Current(key.stable_id, kind, current_vfx) ||
                                vfx_pair_facts.Previous(key.stable_id, kind, previous_vfx);
                        });
                }
                active_effects.BeginFrameBindings();
                const auto refresh_owner_event = [&](VfxEventInput &input) {
                    return runtime_detail::RefreshVfxGroundEventOwner(input,
                        current_vfx, snapshots.current.header.tick,
                        &vfx_pair_facts, previous_vfx, sample_alpha);
                };
                runtime_detail::DiagnosticScope frame_adapter_scope(vfx_work[4], capture_clock);
                runtime_detail::BuildVfxTypedFrameInputs(
                    vfx_catalog.Image(), frame_events,
                    current_vfx, previous_vfx,
                    snapshots.has_current ? snapshots.current.header.tick : Tick{},
                    typed_vfx, &vfx_pair_facts, sample_alpha);
                frame_adapter_scope.Stop();
                runtime_detail::DiagnosticScope ground_birth_scope(vfx_work[11], capture_clock);
                ground_birth_suppressions.clear();
                runtime_detail::AppendVfxTypedEventGroundBirthSuppression(
                    vfx_catalog.Image(), typed_vfx.events, ground_birth_suppressions);
                std::ranges::sort(ground_birth_suppressions, {},
                    &runtime_detail::VfxGroundBirthSuppressionFacts::sequence);
                auto unique_ground = ground_birth_suppressions.begin();
                for (auto it = ground_birth_suppressions.begin(); it != ground_birth_suppressions.end();)
                {
                    auto merged = *it++;
                    while (it != ground_birth_suppressions.end() && it->sequence == merged.sequence)
                    {
                        merged.all_particles |= it->all_particles;
                        merged.ground_ring |= it->ground_ring;
                        merged.ground_disc |= it->ground_disc;
                        merged.segment_solid_trail |= it->segment_solid_trail;
                        ++it;
                    }
                    *unique_ground++ = merged;
                }
                ground_birth_suppressions.erase(unique_ground, ground_birth_suppressions.end());
                ground_birth_scope.Stop();
                replaced_particle_sequences.clear();
                superseded_pickup_sequences.clear();
                owned_status_sequences.clear();
                {
                    runtime_detail::DiagnosticScope flash_scope(vfx_work[12], capture_clock);
                    owned_status_sequences.reserve(typed_vfx.events.size());
                    runtime_detail::AppendVfxOwnedStatusTickFlashes(
                        vfx_catalog.Image(), typed_vfx.events, typed_vfx.persistent,
                        prepared_births.flashes, owned_status_sequences);
                    std::ranges::sort(owned_status_sequences);
                    runtime_detail::AppendVfxTypedFlashCommands(
                        vfx_catalog.Image(), typed_vfx.events, owned_status_sequences,
                        prepared_births.flashes, &superseded_pickup_sequences);
                    replaced_particle_sequences.reserve(owned_status_sequences.size() + superseded_pickup_sequences.size());
                    replaced_particle_sequences.insert(replaced_particle_sequences.end(),
                        owned_status_sequences.begin(), owned_status_sequences.end());
                    replaced_particle_sequences.insert(replaced_particle_sequences.end(),
                        superseded_pickup_sequences.begin(), superseded_pickup_sequences.end());
                    std::ranges::sort(replaced_particle_sequences);
                }
                const auto ballistic_first = prepared_births.particle_spawns.size();
                replaced_legacy_mesh_sequences.clear();
                replaced_burst_shell_sequences.clear();
                {
                    runtime_detail::DiagnosticScope ballistic_prepare_scope(vfx_work[7], capture_clock);
                    const auto &lookup = vfx_catalog.Program().effect_lookup;
                    vfx_row_indices.clear();
                    runtime_detail::AppendVfxTypedBallisticCommands(
                        vfx_catalog.Image(), typed_vfx.events, prepared_births.particle_spawns,
                        &vfx_row_indices);
                    replaced_legacy_mesh_sequences.reserve(vfx_row_indices.size());
                    replaced_burst_shell_sequences.reserve(vfx_row_indices.size());
                    for (const auto index : vfx_row_indices)
                    {
                        const auto &input = typed_vfx.events[index];
                        replaced_legacy_mesh_sequences.push_back(input.sequence);
                        if (input.effect_handle != 0 && input.effect_handle <= lookup.size() &&
                            lookup[input.effect_handle - 1].effect_id == explosive_arrow_main)
                            replaced_burst_shell_sequences.push_back(input.sequence);
                    }
                    std::ranges::sort(replaced_legacy_mesh_sequences);
                    std::ranges::sort(replaced_burst_shell_sequences);
                }
                const auto ballistic_end = prepared_births.particle_spawns.size();
                runtime_detail::DiagnosticScope legacy_scope(vfx_work[3], capture_clock);
                for (const auto &ready_event : frame_events)
                {
                    const auto fact = std::ranges::lower_bound(ground_birth_suppressions,
                        ready_event.sequence, {},
                        &runtime_detail::VfxGroundBirthSuppressionFacts::sequence);
                    const VfxLegacyGroundSuppression ground_suppression =
                        fact != ground_birth_suppressions.end() && fact->sequence == ready_event.sequence
                            ? VfxLegacyGroundSuppression{fact->all_particles, fact->ground_ring,
                                  fact->ground_disc, fact->segment_solid_trail}
                            : VfxLegacyGroundSuppression{};
                    const auto prefix = prepared_births.MarkBirths();
                    if (auto expanded = vfx_catalog.ExpandEvent(
                            ready_event, active_decorative_particle_fraction,
                            prepared_births.particle_spawns, prepared_births.effect_lines,
                            std::ranges::binary_search(replaced_particle_sequences, ready_event.sequence),
                            std::ranges::binary_search(replaced_legacy_mesh_sequences, ready_event.sequence),
                            ground_suppression); !expanded)
                    {
                        prepared_births.RollbackBirths(prefix);
#if defined(HS_DEVELOPMENT_TOOLS)
                        record_thread_failure(expanded);
                        break;
#endif
                    }
                }
                legacy_scope.Stop();
                if (channels.stop_requested.load(std::memory_order_acquire)) break;
                const bool append_vfx_showcase = config.vfx_showcase && !vfx_showcase_injected;
                if (append_vfx_showcase)
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
                        prepared_births.particle_spawns.push_back(command);
                    }
                }

                {
                    runtime_detail::DiagnosticScope mote_birth_scope(vfx_work[13], capture_clock);
                    owned_status_mote_sequences.clear();
                    runtime_detail::AppendVfxOwnedStatusTickMoteFlashes(
                        vfx_catalog.Image(), typed_vfx.events, typed_vfx.persistent,
                        prepared_births.flashes, owned_status_mote_sequences);
                    std::ranges::sort(owned_status_mote_sequences);
                    owned_status_mote_sequences.erase(std::unique(owned_status_mote_sequences.begin(),
                        owned_status_mote_sequences.end()), owned_status_mote_sequences.end());
                    runtime_detail::AppendVfxTypedMoteCommands(
                        vfx_catalog.Image(), typed_vfx.events, owned_status_mote_sequences,
                        prepared_births.flashes);
                }
                const auto phase_effect = vfx_catalog.Image().entity_surface_routes.phase_effect;
                const auto death_effect = vfx_catalog.Image().entity_surface_routes.death_effect;
                const auto trap_effect = vfx_catalog.Image().entity_surface_routes.trap_point_rate_effect;
                const auto is_effect = [](const VfxEventInput &input, std::uint32_t handle) {
                    return handle != 0 && input.effect_handle == handle;
                };
                {
                    runtime_detail::DiagnosticScope profile_prepare_scope(vfx_work[10], capture_clock);
                    profile_additions.clear();
                    const auto &image = vfx_catalog.Image();
                    for (const auto &input : typed_vfx.events)
                    {
                        std::uint16_t mask{};
                        const auto ground_admission = runtime_detail::ClassifyVfxTypedEventGroundAdmission(image, input);
                        if (ground_admission.accepted) mask |= profile_bit(VfxEventProfile::Ground);
                        if (is_effect(input, phase_effect) || is_effect(input, death_effect))
                            mask |= profile_bit(VfxEventProfile::EntitySurface);
                        if (is_effect(input, trap_effect)) mask |= profile_bit(VfxEventProfile::PointRate);
                        if (input.effect_handle != 0 && input.effect_handle <= image.program.effects.size())
                        {
                            if (input.effect_handle <= image.light_effect_ranges.size() &&
                                image.light_effect_ranges[input.effect_handle - 1].count != 0)
                                mask |= profile_bit(VfxEventProfile::Light);
                            if (image.HasDistortionRecipes(input.effect_handle))
                                mask |= profile_bit(VfxEventProfile::Distortion);
                            if (image.HasDecalRecipes(input.effect_handle))
                                mask |= profile_bit(VfxEventProfile::Decal);
                            if (runtime_detail::IsVfxTypedVolumeEffect(image, input.effect_handle))
                                mask |= profile_bit(VfxEventProfile::Volume);
                            if (runtime_detail::IsVfxTypedScreenEffect(image, input.effect_handle))
                                mask |= profile_bit(VfxEventProfile::Screen);
                        }
                        if (input.effect_handle != 0 && input.effect_handle <= image.ribbon_effect_recipes.size() &&
                            (image.HasOutput(input.effect_handle, VfxOutputProfile::RibbonAdd) ||
                             image.HasOutput(input.effect_handle, VfxOutputProfile::RibbonOit)))
                        {
                            const auto &effect = image.ribbon_effect_recipes[input.effect_handle - 1];
                            if (effect.input_mode == 0 && effect.timing_kind == 0 &&
                                std::isfinite(effect.seconds) && effect.seconds > 0.0f &&
                                (std::holds_alternative<VfxPointPayload>(input.payload) ||
                                 std::holds_alternative<VfxProjectilePayload>(input.payload) ||
                                 std::holds_alternative<VfxConePayload>(input.payload) ||
                                 std::holds_alternative<VfxCirclePayload>(input.payload) ||
                                 std::holds_alternative<VfxEntityPayload>(input.payload) ||
                                 std::holds_alternative<VfxContextPayload>(input.payload) ||
                                 std::holds_alternative<VfxLinkPayload>(input.payload)))
                                mask |= profile_bit(VfxEventProfile::Ribbon);
                        }
                        if (mask) profile_additions.push_back({&input, mask, ground_admission.replace_context_owner});
                    }
                    if (!profile_additions.empty() &&
                        !active_effects.PrepareAppendBatch(profile_additions, prepared_profiles))
                    {
                        record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                            "VFX event profiles could not be prepared."));
                        break;
                    }
                }
                runtime_detail::DiagnosticScope birth_commit_scope(vfx_work[3], capture_clock);
                // Keep legacy and showcase births before typed ballistic births
                // while leaving the already-retained particle prefix untouched.
                if (ballistic_first != ballistic_end && ballistic_end != prepared_births.particle_spawns.size())
                    std::rotate(prepared_births.particle_spawns.begin() + ballistic_first,
                        prepared_births.particle_spawns.begin() + ballistic_end,
                        prepared_births.particle_spawns.end());
                if (!vfx_session.AppendPreparedBirths(prepared_births))
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Prepared VFX birth batch aliases retained output."));
                    break;
                }
                // Birth publication only changes decoded queues; the prepared store is untouched.
                if (!profile_additions.empty() && !active_effects.CommitAppendBatch(prepared_profiles))
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Prepared VFX event profiles changed before publication."));
                    break;
                }
                if (append_vfx_showcase) vfx_showcase_injected = true;
                birth_commit_scope.Stop();

                runtime_detail::DiagnosticScope surface_scope(vfx_work[5], capture_clock);
                const Tick surface_tick = snapshots.current.header.tick;
                const auto surface_instance = [&](std::uint64_t id) -> const RenderInstance * {
                    if (const auto *instance = vfx_pair_facts.Instance(id, snapshots.current.instances))
                        return instance;
                    if (sample_alpha < 1.0f && snapshots.has_previous)
                    {
                        const auto found = std::find_if(snapshots.previous.instances.begin(), snapshots.previous.instances.end(),
                            [id](const auto &instance) { return instance.stable_id == id; });
                        if (found != snapshots.previous.instances.end()) return &*found;
                    }
                    return nullptr;
                };
                const auto terminal_phase = static_cast<SessionPhase>(
                    channels.session_phase.load(std::memory_order_acquire));
                const auto terminal_tick = vfx_session.UpdateTerminalClock(
                    surface_tick, terminal_phase, sample_time);
                const double death_current_tick = terminal_phase == SessionPhase::Victory || terminal_phase == SessionPhase::Defeat
                    ? static_cast<double>(terminal_tick) : display_tick;
                std::erase_if(vfx_session.decoded.entity_surface_captures, [&](const auto &capture) {
                    const auto *input = active_effects.Get(capture.row);
                    if (!input || input->effect_handle == 0 || input->effect_handle > vfx_catalog.Image().entity_surface_effect_recipes.size())
                        return true;
                    const auto &effect = vfx_catalog.Image().entity_surface_effect_recipes[input->effect_handle - 1];
                    const double current_tick = is_effect(*input, death_effect)
                        ? death_current_tick : display_tick;
                    return current_tick >= input->event_tick &&
                        static_cast<double>(current_tick - input->event_tick) >= static_cast<double>(effect.seconds) * 60.0;
                });
                active_effects.EraseIf(VfxEventProfile::EntitySurface, [&](const auto &input) {
                    const auto &effect = vfx_catalog.Image().entity_surface_effect_recipes[input.effect_handle - 1];
                    const double current_tick = is_effect(input, death_effect)
                        ? death_current_tick : display_tick;
                    if (current_tick >= input.event_tick &&
                        static_cast<double>(current_tick - input.event_tick) >= static_cast<double>(effect.seconds) * 60.0) return true;
                    const auto *context = std::get_if<VfxContextPayload>(&input.payload);
                    if (!context || context->owner_id == 0) return true;
                    runtime_detail::RetainedVfxEntitySurfaceCapture capture;
                    capture.row = active_effects.Find(input);
                    if (capture.row == std::numeric_limits<runtime_detail::ActiveEffectStore::Row>::max())
                        return false;
                    capture.generation_lease = render_generation;
                    if (is_effect(input, death_effect))
                    {
                        const auto *actual = vfx_pair_facts.Capture(
                            snapshots.current.header.session_id, input.sequence,
                            input.event_tick, context->owner_id,
                            snapshots.current.captured_instances);
                        if (!actual) return false;
                        capture.instance = actual->instance;
                        capture.animation_pose = actual->animation_pose;
                        capture.retained_instance = capture.has_animation_pose = true;
                    }
                    else
                    {
                        const auto *actual = surface_instance((2ull << 60) | context->owner_id);
                        if (!actual) return false;
                        capture.instance = *actual;
                    }
                    vfx_session.decoded.entity_surface_captures.push_back(std::move(capture));
                    return true;
                });
                surface_capture_inputs.clear();
                surface_capture_inputs.reserve(vfx_session.decoded.entity_surface_captures.size());
                for (const auto &capture : vfx_session.decoded.entity_surface_captures)
                    if (const auto *input = active_effects.Get(capture.row))
                        surface_capture_inputs.push_back({*input, capture.instance,
                            capture.retained_instance, capture.has_animation_pose,
                            capture.animation_pose, capture.generation_lease});
                surface_persistent.clear();
                for (const auto &input : typed_vfx.persistent)
                {
                    if (input.source_visual_kind != static_cast<std::uint8_t>(PersistentVfxKind::BossPhaseTransition)) continue;
                    const auto *entity = std::get_if<VfxEntityPayload>(&input.payload);
                    if (!entity) continue;
                    const auto *actual = surface_instance(entity->render_instance_id);
                    const auto *visual = vfx_pair_facts.Current(
                        input.stable_id, PersistentVfxKind::BossPhaseTransition, current_vfx);
                    if (!visual || visual->status_episode_generation != input.status_episode_generation)
                        visual = vfx_pair_facts.Previous(
                            input.stable_id, PersistentVfxKind::BossPhaseTransition, previous_vfx);
                    if (visual && visual->status_episode_generation != input.status_episode_generation)
                        visual = nullptr;
                    if (actual && visual)
                        surface_persistent.push_back({input, *actual, visual->active_tick, visual->expires});
                }
                typed_surfaces.clear();
                runtime_detail::AppendVfxTypedEntitySurfaceCommands(vfx_catalog.Image(),
                    surface_capture_inputs, surface_persistent, display_tick,
                    death_current_tick, typed_surfaces);
                surface_scope.Stop();
                runtime_detail::DiagnosticScope movement_scope(vfx_work[6], capture_clock);
                vfx_upgrade_movements.Append(vfx_catalog.Image(), frame_events,
                    snapshots.has_current ? snapshots.current.header.tick : Tick{},
                    typed_vfx.persistent, sample_alpha);
                movement_scope.Stop();
                runtime_detail::DiagnosticScope ballistic_scope(vfx_work[7], capture_clock);
                typed_vfx_unsupported_inputs += typed_vfx.unsupported_effect_ids.size();
                // Compatibility cancellation is a post-commit mutation: the staged
                // replacement births are already in the retained queue at this point.
                if (!replaced_particle_sequences.empty())
                    for (auto &legacy : pending_particle_spawns)
                        if (std::ranges::binary_search(replaced_particle_sequences, legacy.sequence) &&
                            (!legacy.authored_ballistic ||
                             std::ranges::binary_search(owned_status_sequences, legacy.sequence)))
                            legacy.count = 0;
                if (!replaced_legacy_mesh_sequences.empty())
                    for (auto &legacy : pending_particle_spawns)
                        if (!legacy.authored_ballistic && legacy.renderer == VfxRenderer::Mesh &&
                            std::ranges::binary_search(replaced_legacy_mesh_sequences, legacy.sequence) &&
                            (legacy.primitive != VfxPrimitive::ShockShell ||
                             std::ranges::binary_search(replaced_burst_shell_sequences, legacy.sequence)))
                            legacy.count = 0;
                ballistic_scope.Stop();
                runtime_detail::DiagnosticScope status_scope(vfx_work[8], capture_clock);
                status_burst_keys.clear();
                status_shard_commands.clear();
                // Ballistic provenance is consumed above; this stage reuses its unborrowed row scratch.
                vfx_session.SelectUnseenPersistentBurstOwners(typed_vfx.persistent, vfx_row_indices);
                status_burst_keys.reserve(vfx_row_indices.size());
                for (const auto index : vfx_row_indices)
                {
                    const auto &owner = typed_vfx.persistent[index];
                    status_burst_keys.push_back({owner.stable_id, owner.source_visual_kind});
                }
                runtime_detail::AppendVfxSelectedPersistentStatusShardCommands(
                    vfx_catalog.Image(), typed_vfx.persistent, vfx_row_indices,
                    snapshots.current.header.tick, status_shard_commands);
                (void)vfx_session.CommitPersistentBursts(status_burst_keys,
                    status_shard_commands, pending_particle_spawns);
                status_scope.Stop();
                runtime_detail::DiagnosticScope ground_scope(vfx_work[9], capture_clock);
                ground_commands.clear();
                runtime_detail::AppendVfxTypedGroundCommands(
                    vfx_catalog.Image(), typed_vfx.persistent,
                    snapshots.has_current ? snapshots.current.header.tick : Tick{}, ground_commands);
                ground_scope.Stop();
                runtime_detail::DiagnosticScope local_scope(vfx_work[10], capture_clock);
                const auto light_tick = display_tick;
                active_effects.EraseIf(VfxEventProfile::Screen, [&](const VfxEventInput &input) {
                    const auto &effect = vfx_catalog.Image().screen_effect_recipes[input.effect_handle - 1];
                    return light_tick >= input.event_tick &&
                        static_cast<double>(light_tick - input.event_tick) / 60.0 >= effect.seconds;
                });
                runtime_detail::DiagnosticScope screen_scope(vfx_work[17], capture_clock);
                if (auto converted = runtime_detail::BuildVfxTypedScreenCommands(vfx_catalog.Image(),
                        active_effects.ProfileEvents(VfxEventProfile::Screen), typed_vfx.persistent, current_snapshot.header.tick, display_tick,
                        active_vfx_quality, vfx_session.low_health_pulse, typed_screen); !converted)
                {
                    record_thread_failure(converted);
                    break;
                }
                screen_scope.Stop();
                active_effects.EraseIf(VfxEventProfile::Volume, [&](const VfxEventInput &input) {
                    const auto &effect = vfx_catalog.Image().volume_effect_recipes[input.effect_handle - 1];
                    return light_tick >= input.event_tick &&
                        static_cast<double>(light_tick - input.event_tick) / 60.0 >= effect.seconds;
                });
                runtime_detail::DiagnosticScope volume_scope(vfx_work[18], capture_clock);
                if (auto converted = runtime_detail::BuildVfxTypedVolumeCommands(vfx_catalog.Image(),
                        active_effects.ProfileEvents(VfxEventProfile::Volume), typed_vfx.persistent, light_tick,
                        active_vfx_quality, typed_volumes); !converted)
                {
                    record_thread_failure(converted);
                    break;
                }
                volume_scope.Stop();
                const auto owner_event_expired = [&](const VfxEventInput &input) {
                    if (input.geometry_owner_id != 0) return light_tick >= input.geometry_end_tick;
                    const auto &effect = vfx_catalog.Program().effects[input.effect_handle-1];
                    return effect.timing_kind != 0 || !std::isfinite(effect.seconds) || effect.seconds <= 0 ||
                        (light_tick >= input.event_tick && static_cast<double>(light_tick-input.event_tick)/60.0 >= effect.seconds);
                };
                active_effects.RefreshProfile(VfxEventProfile::Light, refresh_owner_event);
                active_effects.EraseIf(VfxEventProfile::Light, owner_event_expired);
                runtime_detail::DiagnosticScope light_scope(vfx_work[19], capture_clock);
                typed_lights.clear();
                runtime_detail::AppendVfxTypedLightCommands(vfx_catalog.Image(),
                    active_effects.ProfileEvents(VfxEventProfile::Light), light_tick, typed_lights);
                light_scope.Stop();
                active_effects.RefreshProfile(VfxEventProfile::Distortion, refresh_owner_event);
                active_effects.EraseIf(VfxEventProfile::Distortion, owner_event_expired);
                runtime_detail::DiagnosticScope distortion_scope(vfx_work[20], capture_clock);
                typed_distortions.clear();
                runtime_detail::AppendVfxTypedDistortionCommands(vfx_catalog.Image(),
                    active_effects.ProfileEvents(VfxEventProfile::Distortion),
                    typed_vfx.persistent, light_tick, typed_distortions);
                distortion_scope.Stop();
                active_effects.RefreshProfile(VfxEventProfile::Decal, refresh_owner_event);
                active_effects.EraseIf(VfxEventProfile::Decal, owner_event_expired);
                runtime_detail::DiagnosticScope decal_scope(vfx_work[21], capture_clock);
                typed_decals.clear();
                runtime_detail::AppendVfxTypedDecalCommands(vfx_catalog.Image(),
                    active_effects.ProfileEvents(VfxEventProfile::Decal),
                    typed_vfx.persistent, light_tick, typed_decals);
                decal_scope.Stop();
                local_scope.Stop();
                runtime_detail::DiagnosticScope ground_event_scope(vfx_work[11], capture_clock);
                // Retire only preexisting compatibility rows for accepted birth
                // layers; the current frame's emitter already skipped these rows.
                for (auto &legacy : pending_particle_spawns)
                {
                    const auto fact = std::ranges::lower_bound(ground_birth_suppressions,
                        legacy.sequence, {},
                        &runtime_detail::VfxGroundBirthSuppressionFacts::sequence);
                    if (fact == ground_birth_suppressions.end() || fact->sequence != legacy.sequence)
                        continue;
                    if (fact->all_particles ||
                        (fact->ground_ring && legacy.renderer == VfxRenderer::Ground &&
                         legacy.primitive == VfxPrimitive::Ring) ||
                        (fact->ground_disc && legacy.renderer == VfxRenderer::Ground &&
                         legacy.primitive == VfxPrimitive::Disc) ||
                        (fact->segment_solid_trail && legacy.renderer == VfxRenderer::Segment &&
                         legacy.primitive == VfxPrimitive::SolidTrail))
                        legacy.count = 0;
                }
                const Tick ground_tick = snapshots.current.header.tick;
                active_effects.RefreshProfile(VfxEventProfile::Ground, refresh_owner_event);
                active_effects.EraseIf(VfxEventProfile::Ground, [&](const VfxEventInput &input) {
                    if (input.geometry_owner_id != 0)
                        return display_tick >= input.geometry_end_tick;
                    const auto seconds = vfx_catalog.Program().effects[input.effect_handle - 1].seconds;
                    return display_tick >= input.event_tick &&
                        (display_tick - input.event_tick) / 60.0 >= seconds;
                });
                runtime_detail::AppendVfxTypedEventGroundCommands(
                    vfx_catalog.Image(), active_effects.ProfileEvents(VfxEventProfile::Ground),
                    display_tick, ground_commands);
                ground_event_scope.Stop();
                if (inject_pickup_showcase && std::ranges::count_if(superseded_pickup_sequences,
                        [](Sequence sequence) { return sequence >= 0xF100u && sequence < 0xF104u; }) != 4)
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Pickup showcase did not convert all four collection bursts."));
                    break;
                }
                runtime_detail::DiagnosticScope mote_scope(vfx_work[13], capture_clock);
                persistent_sparks.Append(
                    vfx_catalog.Image(), typed_vfx.persistent, ground_tick, sample_alpha,
                    active_vfx_flashes);
                const double flash_tick = display_tick;
                visible_flashes.clear();
                visible_flashes.reserve(active_vfx_flashes.size());
                std::erase_if(active_vfx_flashes, [&](const VfxFlashSpawnInput &flash) {
                    if (flash_tick < flash.event_tick) return false;
                    const auto elapsed = static_cast<float>((flash_tick - flash.event_tick) / 60.0);
                    if (elapsed >= flash.delay + flash.lifetime) return true;
                    if (elapsed < flash.delay) return false;
                    auto visible = flash;
                    visible.normalized_age =
                        (elapsed - flash.delay) / flash.lifetime;
                    visible_flashes.push_back(visible);
                    return false;
                });
                active_effects.EraseIf(VfxEventProfile::PointRate, [&](const auto &input) {
                    return display_tick >= input.event_tick && display_tick - input.event_tick >= 39;
                });
                runtime_detail::AppendVfxOwnedMoteCommands(
                    vfx_catalog.Image(), active_effects.ProfileEvents(VfxEventProfile::PointRate),
                    display_tick, visible_flashes);
                runtime_detail::AppendVfxOwnedMoteCommands(
                    vfx_catalog.Image(), active_effects.ProfileEvents(VfxEventProfile::Ground),
                    display_tick, visible_flashes);
                runtime_detail::AppendVfxPersistentMoteCommands(
                    vfx_catalog.Image(), typed_vfx.persistent, visible_flashes);
                status_stamp_flashes.clear();
                runtime_detail::AppendVfxPersistentStatusStampCommands(
                    vfx_catalog.Image(), typed_vfx.persistent, status_stamp_flashes);
                typed_ground_spawns.clear();
                typed_ground_spawns.reserve(ground_commands.size());
                for (auto &source : ground_commands)
                    typed_ground_spawns.push_back(
                        {source.command, source.additive, source.stable_id, source.effect_handle,
                         source.source_visual_kind, source.motion, source.gradient_row, source.hdr,
                         std::move(source.geometry)});
                mote_scope.Stop();
                runtime_detail::DiagnosticScope mesh_scope(vfx_work[14], capture_clock);
                typed_mesh_spawns.clear();
                runtime_detail::AppendVfxTypedMeshCommands(
                    vfx_catalog.Image(), typed_vfx.persistent, typed_mesh_spawns);
                mesh_scope.Stop();
                runtime_detail::DiagnosticScope fresnel_scope(vfx_work[22], capture_clock);
                typed_fresnels.clear();
                runtime_detail::AppendVfxTypedFresnelCommands(
                    vfx_catalog.Image(), typed_vfx.persistent, typed_fresnels);
                fresnel_scope.Stop();
                runtime_detail::DiagnosticScope ribbon_scope(vfx_work[15], capture_clock);
                active_effects.EraseIf(VfxEventProfile::Ribbon, [&](const VfxEventInput &input) {
                    const auto seconds = vfx_catalog.Image().ribbon_effect_recipes[input.effect_handle - 1].seconds;
                    return display_tick >= input.event_tick &&
                        (display_tick - input.event_tick) / 60.0 >= seconds;
                });
                ribbon_inputs.Reset();
                runtime_detail::AppendVfxTypedRibbonInputs(
                    vfx_catalog.Image(), typed_vfx.persistent, ribbon_inputs);
                runtime_detail::AppendVfxEventRibbonInputs(
                    vfx_catalog.Image(), active_effects.ProfileEvents(VfxEventProfile::Ribbon),
                    display_tick, ribbon_inputs);
                const VfxRibbonFrameInput typed_ribbons{ribbon_inputs.Sources(), vfx_catalog_generation};
                ribbon_scope.Stop();
                runtime_detail::DiagnosticScope sort_scope(vfx_work[16], capture_clock);
                // Authored source delays can cross later event deliveries. Keep the ready prefix chronological.
                vfx_session.decoded.SortNewParticleSpawns(retained_particle_count);
                const Tick stateful_target = current_snapshot.header.tick -
                    (snapshots.has_previous && sample_alpha < 1.0f && current_snapshot.header.tick != 0 ? 1u : 0u);
                auto *native_batch = vfx_session.PrepareNativeBatch(stateful_target,
                    !vfx_session.has_committed_stateful_target ||
                    stateful_target != vfx_session.last_committed_stateful_target);
                const RenderVfxWorldBatch native_world = native_batch ? native_batch->identity :
                    RenderVfxWorldBatch{.session = current_snapshot.header.session_id,
                        .stateful_target = stateful_target};
                const auto native_particles = native_batch
                    ? std::span<const ParticleSpawnCommand>(native_batch->particle_spawns)
                    : std::span<const ParticleSpawnCommand>{};
                const auto native_lines = native_batch
                    ? std::span<const VfxLineSpawnCommand>(native_batch->effect_lines)
                    : std::span<const VfxLineSpawnCommand>{};
                const auto particle_spawn_count = native_particles.size();
                const auto effect_line_count = native_lines.size();
                sort_scope.Stop();
                std::size_t fire_requested{}, fire_admitted{}, fire_admission_bytes{};
                bool fire_budget_limited{};
                {
                    runtime_detail::DiagnosticScope fire_scope(vfx_work[13], capture_clock);
                    using namespace runtime_detail::vfx_retained_size;
                    runtime_detail::PrepareVfxPersistentFireSources(vfx_catalog.Image(),
                        typed_vfx.persistent, display_tick, prepared_fire_sources);
                    if (!prepared_fire_sources.empty())
                    {
                        std::size_t required_fire{}, decorative_fire{};
                        for (const auto &source : prepared_fire_sources)
                        {
                            fire_requested = Add(fire_requested, source.eligible_ordinals);
                            auto &count = source.authoritative ? required_fire : decorative_fire;
                            count = Add(count, source.eligible_ordinals);
                        }
                        // Reserve essential rows before admitting decorative fire; preserve the
                        // old flash/mote/fire/stamp output order without moving the fire suffix.
                        const auto required_suffix = Add(status_stamp_flashes.size(), required_fire);
                        visible_flashes.reserve(Add(visible_flashes.size(), required_suffix));
                        fire_admission_bytes = vfx_workspace_bytes();
                        const auto available = runtime_detail::AvailableVfxFlashAppendRows(
                            fire_admission_bytes, runtime_detail::kVfxWorkspaceSoftBudgetBytes, visible_flashes);
                        const auto fire_limit = available > required_suffix ? available - required_suffix : 0;
                        const auto admitted = std::min(decorative_fire, fire_limit);
                        fire_budget_limited = admitted < decorative_fire;
                        const auto previous_capacity = visible_flashes.capacity();
                        visible_flashes.reserve(Add(Add(visible_flashes.size(), required_suffix), admitted));
                        fire_admission_bytes = Add(fire_admission_bytes,
                            Multiply(visible_flashes.capacity() - previous_capacity, sizeof(VfxFlashSpawnInput)));
                        const auto before = visible_flashes.size();
                        runtime_detail::AppendVfxPreparedFireFlameCommands(
                            prepared_fire_sources, admitted, visible_flashes);
                        fire_admitted = visible_flashes.size() - before;
                        vfx_fire_admission_peak_bytes = std::max(vfx_fire_admission_peak_bytes, fire_admission_bytes);
                    }
                    visible_flashes.insert(visible_flashes.end(),
                        status_stamp_flashes.begin(), status_stamp_flashes.end());
                }
                vfx_fire_budget_limited_frames += fire_budget_limited;
                active_effects.Compact(vfx_session.decoded.entity_surface_captures);
                // Activation owns CPU fanout; native emission keeps its separate pending queue.
                vfx_session.raw_events.AcknowledgeActivation();
                const auto frame_start = std::chrono::steady_clock::now();
                const auto render_start_qpc = performance_lines ? read_performance_qpc() : 0;
                const std::array views{renderer.MainViewInput(InterpolateRenderCamera(snapshots, sample_alpha))};
                const RenderFrameInput render_input{
                    .views = views,
                    .snapshots = snapshots,
                    .events = frame_events,
                    .vfx = {
                        .particle_spawns = native_particles,
                        .effect_lines = native_lines,
                        .event_count = static_cast<std::uint32_t>(typed_vfx.events.size()),
                        .persistent_count = static_cast<std::uint32_t>(typed_vfx.persistent.size()),
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
                        .world_batch = native_world,
                    },
                    .devtools = devtools,
                    .ui_draw_content = snapshots.ui_frame
                        ? snapshots.ui_frame->draw_content : nullptr,
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
                    .ui_packet_generation = snapshots.ui_frame ? snapshots.ui_frame->generation : 0,
                    .sample_time = sample_time,
                };
                // GPU completion is delayed; offline attribution joins this revision through its own frame ID.
                // Simulation/input can enter Pause or a menu after this loop's
                // input boundary. Disable before that first non-gameplay Present.
                if (!AllowGameplayFrameGeneration(static_cast<SessionPhase>(channels.session_phase.load(std::memory_order_acquire))))
                    if (auto policy = set_frame_generation_allowed(false); !policy)
                    { record_thread_failure(policy); break; }
                const auto frame_config_revision = graphics_config_revision;
                const auto frame_settings_generation = active_settings_generation;
                const auto frame_pipeline_revision = renderer.PipelineRevision();
                const auto rendered = renderer.Render(render_input, frame);
                if (frame.presented_ui_packet_generation != 0)
                {
                    if (!channels.ui_packets.AcceptPresentedUiPacket(frame.presented_ui_packet_generation,
                        snapshots.ui_frame, snapshots.current.header.session_id,
                        frame.rendered_view.output_extent))
                        OutputDebugStringA("Presented UI receipt did not match the retained packet; prior accepted frame retained.\n");
                }
                const bool vfx_world_step_accepted = vfx_session.AcknowledgeNativeBatch(
                    {.session = frame.vfx_world_session_id, .stateful_target = frame.vfx_world_tick,
                        .sequence = frame.vfx_world_batch_sequence}, frame.vfx_world_step_submitted);
                if (!rendered)
                {
                    record_thread_failure(rendered);
                    break;
                }
                if (native_world.sequence != 0 && !vfx_world_step_accepted)
                {
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        "Renderer completed VFX inputs without acknowledging the matching world step."));
                    break;
                }
                if (performance_lines)
                {
                    const auto render_end_qpc = read_performance_qpc();
                    const auto qpc_microseconds = performance_frequency
                        ? 1'000'000.0 / static_cast<double>(performance_frequency) : 0.0;
                    const auto &cpu = frame.cpu_timings;
                    const auto &gpu = renderer.LastCompletedGpuPassTimings();
                    const auto &diagnostic = renderer.LastRenderedFrameDiagnostics();
                    const auto &memory = diagnostic.memory;
                    const auto &queues = diagnostic.queues;
                    runtime_detail::DiagnosticSample capture_log{};
                    runtime_detail::DiagnosticScope capture_log_scope(capture_log, capture_clock);
                    performance_row.clear();
                    runtime_detail::AppendCsvValues(performance_row,
                        frame.frame, frame.rendered_tick, render_start_qpc, performance_frequency,
                        performance_state.FrameInterval(render_start_qpc) * qpc_microseconds,
                        runtime_detail::PerformanceLogState::ElapsedTicks(loop_start_qpc, render_start_qpc) * qpc_microseconds,
                        runtime_detail::PerformanceLogState::ElapsedTicks(render_start_qpc, render_end_qpc) * qpc_microseconds,
                        cpu.wait_us, cpu.streaming_us, cpu.scene_prepare_us, cpu.ui_us, cpu.constants_upload_us,
                        cpu.graph_build_us, cpu.graph_prepare_us, cpu.descriptors_us, cpu.graph_plan_us,
                        static_cast<unsigned>(cpu.graph_frontend_reused), cpu.graph_record_us, cpu.graph_submit_us,
                        cpu.post_submit_us, cpu.present_us, cpu.diagnostics_us, cpu.wrapper_us,
                        gpu.completed_frame_id, static_cast<unsigned>(gpu.valid && gpu.fence_completed),
                        gpu.critical_path_ms, static_cast<unsigned>(gpu.critical_path_valid),
                        static_cast<unsigned>(GetForegroundWindow() == window.Handle()), snapshots.current.instances.size(),
                        snapshots.current.static_scene ? snapshots.current.static_scene->instances.size() : 0,
                        static_cast<unsigned>(channels.session_phase.load(std::memory_order_acquire)), active_frame_cap,
                        frame_config_revision, frame_settings_generation, frame_pipeline_revision,
                        frame.rendered_view.id.value, frame.rendered_view.output_extent.width, frame.rendered_view.output_extent.height,
                        frame.rendered_view.render_active.width, frame.rendered_view.render_active.height,
                        frame.rendered_view.render_capacity.width, frame.rendered_view.render_capacity.height,
                        diagnostic.rendered_frame_id,
                        static_cast<unsigned>(diagnostic.rendered_frame_id == frame.frame && diagnostic.view_id == frame.rendered_view.id.value),
                        diagnostic.submit_qpc, diagnostic.present_begin_qpc, diagnostic.present_end_qpc,
                        diagnostic.qpc_frequency, static_cast<unsigned>(memory.budget_available),
                        memory.local_usage_bytes, memory.local_budget_bytes, memory.tracked_live_bytes,
                        memory.tracked_retiring_bytes, memory.texture_physical_bytes, memory.host_staging_bytes,
                        queues.direct_batches, queues.compute_batches, queues.copy_batches, queues.cross_queue_waits,
                        queues.record_jobs, queues.transient_bytes_without_aliasing, queues.transient_backing_bytes,
                        static_cast<unsigned>(diagnostic.validation_enabled), static_cast<unsigned>(diagnostic.gpu_validation_enabled),
                        cpu.frame_slot_wait_begin_qpc, cpu.frame_slot_wait_end_qpc, cpu.graph_prepare_validation_us,
                        cpu.graph_prepare_info_key_us, cpu.graph_prepare_compile_us, cpu.graph_prepare_transient_us,
                        frame.streamline_present.frames, static_cast<unsigned>(frame.streamline_present.valid),
                        frame.streamline_present.begin_qpc, frame.streamline_present.end_qpc,
                        frame.streamline_present.qpc_frequency, static_cast<unsigned>(native_latency),
                        latency_begin_ticks * qpc_microseconds, input_wait_ticks * qpc_microseconds,
                        simulation_wait_ticks * qpc_microseconds, cpu.retained_vfx_us, cpu.ribbon_prepare_us,
                        cpu.particle_spawn_prepare_us, cpu.typed_vfx_upload_prepare_us, cpu.particle_upload_prepare_us,
                        cpu.local_vfx_prepare_us, cpu.palette_prepare_us, cpu.skin_prepare_us, cpu.scene_reconcile_us,
                        cpu.scene_gpu_prepare_us);
                    const auto io = performance_lines->Io();
                    runtime_detail::AppendCsvValues(performance_row,
                        snapshots.current.header.session_id, frame_events.size(), typed_vfx.events.size(), typed_vfx.persistent.size(),
                        particle_spawn_count, effect_line_count, typed_ground_spawns.size(), visible_flashes.size(),
                        typed_mesh_spawns.size(), typed_fresnels.size(), typed_ribbons.sources.size(), typed_lights.size(),
                        typed_distortions.size(), typed_decals.size(), typed_volumes.size(), typed_screen.size(), typed_surfaces.size(),
                        previous_capture_log.ticks * qpc_microseconds, io.ticks * qpc_microseconds, io.calls, performance_lines->Bytes());
                    AppendTaskValues(performance_row, vfx_work, qpc_microseconds);
                    runtime_detail::AppendCsvValues(performance_row,
                        cpu.ribbon_state_us, cpu.ribbon_wait_us, cpu.ribbon_allocation_us, cpu.ribbon_replacement_count,
                        cpu.ribbon_allocation_bytes, cpu.ribbon_prepare_thread_cpu_us,
                        static_cast<unsigned>(cpu.ribbon_prepare_thread_cpu_valid), cpu.graph_resources_us,
                        cpu.graph_base_passes_us, cpu.graph_shadows_us, cpu.graph_light_probes_us,
                        cpu.graph_scene_selection_us, cpu.graph_ao_us, cpu.graph_opaque_us, cpu.graph_hero_us,
                        cpu.graph_post_us, cpu.scene_skin_prepare_us, cpu.scene_shadow_prepare_us,
                        cpu.scene_cluster_prepare_us, cpu.scene_geometry_demand_us, cpu.scene_texture_prepare_us,
                        cpu.scene_upload_plan_us, active_graphics.target_render_fps,
                        fire_admission_bytes, fire_requested, fire_admitted,
                        static_cast<unsigned>(fire_budget_limited));
                    performance_row += '\n';
                    performance_lines->Append(performance_row);
                    // The sampler waits for nonempty performance.csv before opening GPU counters.
                    if (!performance_ready_published && (!config.capture_gpu_timings || (gpu.valid && gpu.fence_completed)))
                    {
                        performance_ready_published = true;
                        if (!performance_lines->Flush())
                        {
                            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                "Could not publish GPU-ready performance capture."));
                            break;
                        }
                    }
                    if (auto collected = collect_gpu_capture(frame.frame, false); !collected)
                    {
                        record_thread_failure(collected);
                        break;
                    }
                    capture_log_scope.Stop();
                    previous_capture_log = capture_log;
                }
#if defined(HS_DEVELOPMENT_TOOLS)
                if (generation_awaiting_frame)
                {
                    std::ofstream(config.artifact_directory / "shader_hot_reload.log", std::ios::app)
                        << "rendered active=" << generation_awaiting_frame->id << " frame=" << frame.frame
                        << " pipelines=" << renderer.PipelineRevision()
                        << " scene_generation=" << scene_generation.resident_generation_id << '\n';
                    generation_awaiting_frame.reset();
                }
#endif
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
                            channels.NotifyMain();
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
                if (capture_diagnostics)
                {
                    gpu_live_row.clear();
                    runtime_detail::AppendCsvValues(gpu_live_row, frame.frame, last_tick,
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - frame_start).count(),
                        CurrentProcessWorkingSetBytes(),
                        snapshots.current.instances.size() + (snapshots.current.static_scene
                            ? snapshots.current.static_scene->instances.size() : 0),
                        devtools.input_queue_depth + devtools.presentation_queue_depth + devtools.particle_queue_depth +
                            devtools.resize_queue_depth + devtools.debug_queue_depth);
                    gpu_live_row += '\n';
                    timeline_lines.Append(gpu_live_row);
                }
                if (active_frame_cap != 0 && realtime && !renderer.UsesNativeFrameLimiter())
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
                        gpu_live_row.clear();
                        runtime_detail::AppendCsvValues(gpu_live_row,
                            channels.rendered_frames.load(std::memory_order_acquire), last_tick);
                        runtime_detail::AppendCsvText(gpu_live_row, pass.name);
                        runtime_detail::AppendCsvValues(gpu_live_row, pass.nanoseconds,
                            static_cast<unsigned>(pass.queue), pass.measured ? 1 : 0,
                            pass.cpu_callback_begin_qpc, pass.cpu_callback_end_qpc, pass.cpu_callback_valid ? 1 : 0,
                            gpu_timings.cpu_qpc_frequency, gpu_timings.completed_frame_id, gpu_timings.view_id);
                        gpu_live_row += '\n';
                        gpu_lines.Append(gpu_live_row);
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
        // Shutdown releases query readbacks; retain the last in-flight frames first.
        if (auto drained = drain_gpu_capture(); !drained) record_thread_failure(drained);
#if defined(HS_DEVELOPMENT_TOOLS)
        RendererHeroWorkStatus hero_status;
        if (config.capture_hero_work) hero_status = renderer.HeroWorkStatus();
#endif
        RendererLightingStatus::ShadowCacheStatus shadow_cache;
        if (capture_diagnostics) shadow_cache = renderer.GetLightingStatus().shadow_cache;
        if (auto shutdown = renderer.Shutdown(); !shutdown)
        {
            record_thread_failure(shutdown);
        }
        if (capture_diagnostics)
        {
            const auto cache_stats = renderer.GetPipelineCacheStats();
            WriteText(config.artifact_directory / "pipeline_cache.json", std::format(
                "{{\"hits\":{},\"misses\":{},\"native_creates\":{},\"stores\":{},\"store_failures\":{},"
                "\"load_failures\":{},\"io_failures\":{},\"discarded_files\":{},\"unsupported_libraries\":{},"
                "\"uncached_mesh_creates\":{},\"root_registrations\":{},\"loaded_bytes\":{},\"serialized_bytes\":{},"
                "\"last_cache_hresult\":{},\"last_io_error\":{},"
                "\"key_build_ns\":{},\"key_build_max_ns\":{},\"library_load_ns\":{},\"library_load_max_ns\":{},"
                "\"native_create_ns\":{},\"native_create_max_ns\":{},\"library_store_ns\":{},\"library_store_max_ns\":{},"
                "\"serialize_ns\":{},\"cache_write_ns\":{},\"mutex_wait_ns\":{},\"mutex_wait_max_ns\":{},"
                "\"capture_timings\":{}}}\n",
                cache_stats.hits, cache_stats.misses, cache_stats.native_creates, cache_stats.stores,
                cache_stats.store_failures, cache_stats.load_failures, cache_stats.io_failures, cache_stats.discarded_files,
                cache_stats.unsupported_libraries, cache_stats.uncached_mesh_creates, cache_stats.root_registrations,
                cache_stats.loaded_bytes, cache_stats.serialized_bytes, cache_stats.last_cache_hresult, cache_stats.last_io_error,
                cache_stats.key_build_ns, cache_stats.key_build_max_ns, cache_stats.library_load_ns, cache_stats.library_load_max_ns,
                cache_stats.native_create_ns, cache_stats.native_create_max_ns, cache_stats.library_store_ns, cache_stats.library_store_max_ns,
                cache_stats.serialize_ns, cache_stats.cache_write_ns, cache_stats.mutex_wait_ns, cache_stats.mutex_wait_max_ns, cache_stats.capture_timings));
        }
        const auto validation_errors = renderer.ValidationErrorCount();
        if (validation_errors != 0)
        {
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                        std::format("{} validation errors.", validation_errors)));
        }

        if (capture_diagnostics)
        {
            const bool events_written = event_lines.Finish();
            const bool timeline_written = timeline_lines.Finish();
            const bool gpu_written = gpu_lines.Finish();
            const bool performance_written = !performance_lines || performance_lines->Finish();
            const bool gpu_live_written = !gpu_live_lines || gpu_live_lines->Finish();
            const bool graphics_settings_written = !graphics_settings_lines || graphics_settings_lines->Finish();
            bool hero_written = true;
#if defined(HS_DEVELOPMENT_TOOLS)
            if (hero_lines)
            {
                try { hero_written = hero_lines->Finish(hero_status, hero_drain_succeeded); }
                catch (const std::exception &) { hero_written = false; }
            }
#endif
            if (performance_lines)
            {
                const auto io = performance_lines->Io();
                const auto live_io = gpu_live_lines->Io();
                WriteText(config.artifact_directory / "render_tasks_status.json", std::format(
                    "{{\"performance_io_ticks\":{},\"performance_io_calls\":{},\"performance_bytes\":{},\"gpu_live_io_ticks\":{},\"gpu_live_io_calls\":{},\"gpu_live_bytes\":{},\"last_row_log_ticks\":{},\"qpc_frequency\":{}}}\n",
                    io.ticks, io.calls, performance_lines->Bytes(), live_io.ticks, live_io.calls, gpu_live_lines->Bytes(),
                    previous_capture_log.ticks, capture_frequency.QuadPart));
            }
            event_file.close(); timeline_file.close(); gpu_file.close();
            if (performance_lines) { performance_file.close(); gpu_live_file.close(); }
            if (graphics_settings_lines) graphics_settings_file.close();
            const bool io_ok = events_written && timeline_written && gpu_written &&
                performance_written && gpu_live_written && graphics_settings_written && hero_written &&
                !event_file.fail() && !timeline_file.fail() && !gpu_file.fail() &&
                (!performance_lines || (!performance_file.fail() && !gpu_live_file.fail())) &&
                (!graphics_settings_lines || !graphics_settings_file.fail());
            const auto expected_gpu_frames = performance_lines && config.capture_gpu_timings ? performance_lines->Records() : 0;
            const bool gpu_frame_coverage_complete = !performance_lines ||
                performance_state.GpuFrameCoverageComplete(expected_gpu_frames);
            const bool complete = io_ok && gpu_drain_succeeded && gpu_frame_coverage_complete &&
                graphics_capture_complete && event_lines.Complete() &&
                timeline_lines.Complete() && gpu_lines.Complete() &&
                (!performance_lines || (performance_lines->Complete() && gpu_live_lines->Complete())) &&
                (!graphics_settings_lines || graphics_settings_lines->Complete());
            const bool status_written = write_diagnostics_status(std::format(
                "{{\"schema\":1,\"mode\":\"{}\",\"complete\":{},\"io_failed\":{},"
                "\"event_records\":{},\"event_dropped\":{},\"timeline_records\":{},"
                "\"timeline_dropped\":{},\"gpu_records\":{},\"gpu_dropped\":{},"
                "\"performance_records\":{},\"gpu_live_records\":{},\"graphics_settings_records\":{},"
                "\"gpu_frames_written\":{},\"gpu_expected_frames\":{},\"gpu_last_frame\":{},\"gpu_timing_requested\":{},"
                "\"gpu_frame_coverage_complete\":{},\"gpu_drain_succeeded\":{},\"gpu_invalid_frames\":{},"
                "\"graphics_capture_complete\":{}}}\n",
                "capture", complete ? "true" : "false",
                io_ok ? "false" : "true", event_lines.Records(), event_lines.Dropped(),
                timeline_lines.Records(), timeline_lines.Dropped(), gpu_lines.Records(), gpu_lines.Dropped(),
                performance_lines ? performance_lines->Records() : 0,
                gpu_live_lines ? gpu_live_lines->Records() : 0,
                graphics_settings_lines ? graphics_settings_lines->Records() : 0,
                performance_state.GpuFrames(), expected_gpu_frames, performance_state.LastGpuFrame(),
                config.capture_diagnostics && config.capture_gpu_timings ? "true" : "false",
                gpu_frame_coverage_complete ? "true" : "false", gpu_drain_succeeded ? "true" : "false", invalid_gpu_frames,
                graphics_capture_complete ? "true" : "false"));
            if (!status_started || !status_written || !complete)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    "Diagnostic output incomplete: GPU frame coverage, metadata, write, flush or close failed."));
            WriteText(config.artifact_directory / "render.json",
                      std::format("{{\"enhanced_barriers\":{},\"validation_errors\":{},"
                                  "\"last_tick\":{},\"typed_vfx_events_received\":{},"
                                  "\"typed_vfx_persistent_peak\":{},"
                                  "\"typed_vfx_ground_peak\":{},"
                                  "\"typed_vfx_ground_submitted_total\":{},"
                                  "\"typed_vfx_flash_peak\":{},"
                                  "\"typed_vfx_flash_submitted_total\":{},"
                                  "\"vfx_workspace_soft_budget_bytes\":{},"
                                  "\"vfx_fire_admission_peak_bytes\":{},"
                                  "\"vfx_fire_budget_limited_frames\":{},"
                                  "\"typed_vfx_mesh_peak\":{},"
                                  "\"typed_vfx_fresnel_peak\":{},"
                                  "\"typed_vfx_ribbon_peak\":{},"
                                  "\"typed_vfx_ribbon_dropped\":{},"
                                  "\"typed_vfx_light_peak\":{},"
                                  "\"typed_vfx_distortion_peak\":{},"
                                  "\"typed_vfx_decal_peak\":{},"
                                  "\"typed_vfx_volume_peak\":{},"
                                  "\"static_scene_upload_count\":{},"
                                  "\"baked_static_shadow_active\":{},"
                                  "\"baked_static_shadow_pages\":{},"
                                  "\"baked_static_shadow_staging_bytes\":{},"
                                  "\"baked_static_shadow_cpu_plan_prepared\":{},"
                                  "\"baked_static_shadow_csm_required\":{},"
                                  "\"baked_static_shadow_guarantee_reason_bits\":{},"
                                  "\"baked_static_shadow_required_coarse_pages\":{},"
                                  "\"baked_static_shadow_maximum_coarse_commands\":{},"
                                  "\"baked_static_shadow_coarse_command_total\":{},"
                                  "\"baked_static_shadow_command_capacity\":{},"
                                  "\"typed_vfx_unsupported_inputs\":{}}}\n",
                                  enhanced ? "true" : "false", validation_errors, last_tick,
                                  typed_vfx_events_received, typed_vfx_persistent_peak,
                                  typed_vfx_ground_peak, typed_vfx_ground_submitted_total,
                                  typed_vfx_flash_peak, typed_vfx_flash_submitted_total,
                                  runtime_detail::kVfxWorkspaceSoftBudgetBytes,
                                  vfx_fire_admission_peak_bytes, vfx_fire_budget_limited_frames,
                                  typed_vfx_mesh_peak, typed_vfx_fresnel_peak,
                                  typed_vfx_ribbon_peak, typed_vfx_ribbon_dropped,
                                  typed_vfx_light_peak, typed_vfx_distortion_peak, typed_vfx_decal_peak,
                                  typed_vfx_volume_peak,
                                  static_scene_upload_count,
                                  shadow_cache.baked_active, shadow_cache.baked_pages, shadow_cache.baked_staging_bytes,
                                  shadow_cache.cpu_plan_prepared, shadow_cache.csm_required, shadow_cache.guarantee_reason_bits,
                                  shadow_cache.required_coarse_pages, shadow_cache.maximum_coarse_commands,
                                  shadow_cache.coarse_command_total, shadow_cache.command_capacity,
                                  typed_vfx_unsupported_inputs));
        }
    });

    // Renderer initialization may call DXGI with this HWND from the render
    // thread. Keep its owner thread dispatching window/system messages until
    // the renderer publishes readiness instead of blocking in atomic::wait.
    while (!channels.render_ready.load(std::memory_order_acquire))
    {
        const auto processed_wake_sequence = main_wake.Sequence();
        Result window_failure = Result::Success();
        if (!window.PumpMessages(&window_failure, false))
        {
            if (!window_failure) record_thread_failure(std::move(window_failure));
            channels.stop_requested.store(true, std::memory_order_release);
            native_input_frame.Cancel();
        }
        if (channels.render_ready.load(std::memory_order_acquire)) break;

        const auto pending_startup = [&]() noexcept {
            MSG pending{};
            return channels.render_ready.load(std::memory_order_acquire) ||
                runtime_detail::PeekMainMessage(pending, false, PM_NOREMOVE);
        };
        if (main_wake.PrepareWait(pending_startup, processed_wake_sequence) &&
            !pending_startup())
        {
            const auto handle = main_wake.Handle();
            const auto wait_result = MsgWaitForMultipleObjectsEx(1, &handle, INFINITE,
                runtime_detail::MainMessageMask(false),
                runtime_detail::MainMessageFlags(false));
            if (wait_result == WAIT_FAILED)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                    std::format("Main startup message/event wait failed: {}.", GetLastError())));
        }
        if (const auto error = main_wake.Error())
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                std::format("Main startup wake event failed: {}.", error)));
    }
    {
        std::lock_guard lock(result_mutex);
        result = thread_result;
    }
    if (!result)
    {
        // Simulation was never started; let Render finish its normal drain.
        channels.simulation_done.store(true, std::memory_order_release);
        // Teardown can publish another failure through result_mutex.
        render_thread.join();
        return {result};
    }

    auto simulation_ports = channels.ForSimulation();
    const SettingsData initial_projection_settings = settings;
    std::jthread simulation_thread([&, simulation_ports, initial_projection_settings](std::stop_token) mutable {
        auto &channels = simulation_ports;
        SetThreadName(L"HS Simulation");
        GameSimulation simulation;
        SettingsData projection_settings = initial_projection_settings;
        const bool capture_balance = config.record_playtest || config.capture_diagnostics || config.smoke || replaying;
        if (auto initialized = simulation.Initialize(
                {.seed = effective_seed, .automatic_choices = config.smoke,
                 .start_in_main_menu = !config.smoke,
                 .scenario = {.player_invulnerable = config.diagnostic_enemy_count != 0,
                              .progression_enabled = config.diagnostic_enemy_count == 0,
                              .automatic_spawning = config.diagnostic_enemy_count == 0},
                 .capture_checksums = config.diagnostic_enemy_count == 0 &&
                                      (config.smoke || config.record_playtest || replaying),
                 .capture_timings = config.capture_diagnostics,
                 .capture_detailed_balance = capture_balance,
                 .capture_hit_rates = capture_balance},
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

        std::ofstream simulation_tasks_file;
        std::optional<runtime_detail::DiagnosticLog> simulation_tasks;
        std::string simulation_task_row;
        runtime_detail::DiagnosticSample previous_simulation_log{}, initial_harness{};
        std::uint64_t diagnostic_spawn_index{};
        std::uint32_t initial_added{};
        if (capture_clock)
        {
            simulation_tasks_file.open(config.artifact_directory / "simulation_tasks.csv", std::ios::binary);
            if (!simulation_tasks_file)
                record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Could not initialize simulation task capture."));
            std::string header = "tick,qpc,qpc_frequency,session_id,enemy_count,player_projectiles,enemy_projectiles,domain_signals,snapshot_published,harness_added,scripted_input,checksum_capture";
            header += TaskColumns(kSimulationTaskNames);
            for (const auto name : kSimulationTimingNames) header += std::format(",game_{}_ns,game_{}_calls", name, name);
            header += ",game_captured,game_completed,empty_scope_ns,empty_scope_calls,previous_log_us,previous_log_calls,log_io_us_total,log_io_calls_total,log_bytes_total\n";
            simulation_tasks.emplace(simulation_tasks_file, true, header, capture_clock);
        }
        {
            runtime_detail::DiagnosticScope scope(initial_harness, config.diagnostic_enemy_count ? capture_clock : nullptr);
            if (auto filled = RefillDiagnosticEnemies(simulation, config.diagnostic_enemy_count, diagnostic_spawn_index, initial_added); !filled)
            {
                record_thread_failure(filled);
                channels.simulation_done.store(true, std::memory_order_release);
                return;
            }
        }
        FixedStepClock clock;
        auto now = FixedStepClock::Clock::now();
        (void)clock.Advance(now);
        std::uint64_t observed_focus_epoch{};
        Sequence publish_sequence{};
        RenderSnapshotStorage dynamic_previous(1'024, 256, 4, 0);
        RenderSnapshotStorage dynamic_current(1'024, 256, 4, 0);
        bool has_dynamic_sample{};
        bool has_previous_dynamic_sample{};
        std::uint64_t display_clock_epoch{};
        RenderClockMode display_clock_mode{RenderClockMode::Scripted};
        std::uint64_t dynamic_pair_revision{};
        std::shared_ptr<const UiFrame> prepared_ui;
        std::vector<std::uint64_t> ui_projection_facts;
        std::array<ActionEdge, 256> action_storage{};
        std::array<UiAction, 64> ui_action_storage{};
        auto previous_phase = config.smoke ? SessionPhase::Playing : SessionPhase::MainMenu;
        EnemyAnimationState enemy_animations;
        PlayerFootstepTimeline footstep_timeline;
        runtime_detail::ProcessSceneFactory environment_cache(deployment_id);
        const auto configure_dynamic_camera = [&](const TickReadView &model) {
            const auto zoom_percent = static_cast<float>(channels.camera_zoom_percent.load(std::memory_order_acquire));
            const auto pose = ComputeRuntimeCameraPose(presentation_catalog.camera.distance_m,
                presentation_catalog.camera.pitch_degrees, zoom_percent);
            dynamic_current.camera.yaw_degrees = presentation_catalog.camera.yaw_degrees;
            dynamic_current.camera.pitch_degrees = pose.pitch_degrees;
            dynamic_current.camera.vertical_fov_degrees = presentation_catalog.camera.vertical_fov_degrees;
            dynamic_current.camera.distance = pose.distance_m;
            dynamic_current.camera.target_height_offset = pose.target_height_m;
            dynamic_current.camera.target.y = pose.target_height_m +
                (model.arena_half_extent >= 14.0f ? MeadowGroundHeight({dynamic_current.camera.target.x,
                    dynamic_current.camera.target.z}) : 0.0f);
        };
        {
            auto read_scope = simulation.BorrowTickReadView();
            const auto &model = read_scope.View();
            // Seed the preceding endpoint without consuming or replaying pending events.
            enemy_animations.Update(model, {});
            footstep_timeline.Reset(model, enemy_animations.PlayerMotionEpoch());
            if (auto projected = ProjectDynamicTick(model, dynamic_current,
                    environment_cache.Resolve(model), &enemy_animations); !projected)
            {
                record_thread_failure(projected);
                channels.simulation_done.store(true, std::memory_order_release);
                return;
            }
            configure_dynamic_camera(model);
            now = FixedStepClock::Clock::now();
            clock.Realign(now);
            display_clock_epoch = 1;
            display_clock_mode = !realtime ? RenderClockMode::Scripted
                : model.session.phase == SessionPhase::Playing ? RenderClockMode::RealtimeRunning
                                                               : RenderClockMode::Frozen;
            dynamic_current.header.clock = {display_clock_epoch, display_clock_mode,
                realtime ? now : FixedStepClock::Clock::time_point{}};
            dynamic_current.header.dynamic_pair_revision = ++dynamic_pair_revision;
            has_dynamic_sample = true;
        }
        std::vector<PresentationEvent> record_events;
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
            FixedStepClock::Duration simulation_debt{};
            FixedStepClock::Clock::time_point first_tick_boundary{};
            if (realtime)
            {
                now = FixedStepClock::Clock::now();
                const auto advanced = clock.Advance(now);
                tick_count = advanced.tick_count;
                simulation_debt = advanced.debt;
                first_tick_boundary = advanced.first_tick_boundary;
                if (tick_count == 0)
                {
                    if (native_token) { native_input_frame.Simulated(native_token); continue; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }

            bool stop_remaining_admitted_steps{};
            for (std::uint32_t local_tick = 0;
                 local_tick < tick_count &&
                 !smoke_finished && !stop_remaining_admitted_steps;
                 ++local_tick)
            {
                std::array<runtime_detail::DiagnosticSample, kSimulationTaskNames.size()> simulation_work{};
                simulation_work[3] = std::exchange(initial_harness, {});
                auto harness_added = std::exchange(initial_added, 0u);
                runtime_detail::DiagnosticScope iteration_scope(simulation_work[0], capture_clock);
                runtime_detail::DiagnosticScope input_scope(simulation_work[1], capture_clock);
                SessionProbe captured_probe{};
                std::uint64_t captured_session{}, captured_signals{};
                bool snapshot_published{};
                bool forced_dynamic_refresh{};
                SettingsData updated_presentation_settings;
                while (channels.presentation_settings.TryPop(
                    updated_presentation_settings))
                    projection_settings = updated_presentation_settings;
                std::size_t ui_action_count{};
                runtime_detail::UiActionEnvelope queued_ui_action;
                if (channels.ui_actions.TryPop(queued_ui_action))
                {
                    const auto authoritative_session = simulation.GetSessionProbe();
                    const auto authoritative_session_id = channels.current_session_id.load(
                        std::memory_order_acquire);
                    if (runtime_detail::UiActionCurrent(queued_ui_action, authoritative_session_id,
                                                        authoritative_session))
                        ui_action_storage[ui_action_count++] = queued_ui_action.action;
                    else
                    {
                        channels.stale_ui_action_drops.fetch_add(1, std::memory_order_relaxed);
                        channels.ui_input_rejected.store(true, std::memory_order_release);
                    }
                }
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
                        channels.character_details_page.store(0, std::memory_order_release);
                        continue;
                    }
                    if (action.kind == EdgeKind::Pressed && action.action == GameAction::Pause &&
                        current_phase == SessionPhase::Paused &&
                        (ui_page == UiPage::CharacterOverview || ui_page == UiPage::CharacterSkills ||
                         ui_page == UiPage::CharacterStats))
                    {
                        channels.ui_page.store(static_cast<std::uint8_t>(UiPage::Root),
                                               std::memory_order_release);
                        channels.character_details_page.store(0, std::memory_order_release);
                        channels.loadout_source.store(0xFF, std::memory_order_release);
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
                        channels.character_details_page.store(0, std::memory_order_release);
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
                if (config.diagnostic_enemy_count)
                {
                    const auto player = simulation.GetSessionProbe().player_position;
                    constexpr std::array corners{Float3{-4, 0, -4}, Float3{4, 0, -4},
                                                  Float3{4, 0, 4}, Float3{-4, 0, 4}};
                    input.held.move_held = true;
                    input.held.move_target_world = corners[((input.target_tick - 1) / 300) % corners.size()];
                    input.held.basic_attack_held = true;
                    input.held.aim_world = {player.x, 0, player.y + 8};
                    if (input.target_tick >= 180 && (input.target_tick - 180) % 90 == 0)
                    {
                        if (action_count == action_storage.size())
                        {
                            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Diagnostic input edge capacity exceeded."));
                            break;
                        }
                        constexpr std::array skills{GameAction::SkillQ, GameAction::SkillW, GameAction::SkillE, GameAction::SkillR};
                        action_storage[action_count++] = {static_cast<Sequence>(0xF0000000ull + input.target_tick),
                            skills[((input.target_tick - 180) / 90) % skills.size()], EdgeKind::Pressed};
                        input.ordered_edges = std::span(action_storage.data(), action_count);
                    }
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
                    forced_dynamic_refresh = true;
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
                    forced_dynamic_refresh = true;
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
                    forced_dynamic_refresh = true;
                    if (auto applied = simulation.ApplyDebugCommand(debug_command); !applied)
                    {
                        record_thread_failure(applied);
                        break;
                    }
                }
                input_scope.Stop();
                runtime_detail::DiagnosticScope tick_scope(simulation_work[2], capture_clock);
                const auto tick = simulation.TickFixed(input, FixedStepClock::kFixedStep);
                tick_scope.Stop();
                if (config.diagnostic_enemy_count)
                {
                    runtime_detail::DiagnosticScope harness_scope(simulation_work[3], capture_clock);
                    if (auto filled = RefillDiagnosticEnemies(simulation, config.diagnostic_enemy_count, diagnostic_spawn_index, harness_added); !filled)
                    {
                        record_thread_failure(filled);
                        break;
                    }
                }
                const bool final_smoke_frame =
                    config.smoke &&
                    (tick.tick >= config.maximum_ticks ||
                     tick.phase == SessionPhase::Victory ||
                     tick.phase == SessionPhase::Defeat);
                {
                    runtime_detail::DiagnosticScope observe_scope(simulation_work[4], capture_clock);
                    auto read_scope = simulation.BorrowTickReadView();
                    const auto &model = read_scope.View();
                    runtime_detail::DiagnosticScope animation_scope(simulation_work[5], capture_clock);
                    enemy_animations.Update(model, model.domain_signals);
                    animation_scope.Stop();
                    const auto &probe = model.session;
                    if (capture_clock)
                    {
                        captured_probe = probe;
                        captured_session = model.session_id;
                        captured_signals = model.domain_signals.size();
                    }
                    runtime_detail::DiagnosticScope replay_scope(simulation_work[11], replaying ? capture_clock : nullptr);
                    channels.PublishSessionProbe(probe, model.session_id);
                    if (replaying && !config.replay_compare && tick.checksum !=
                                         replay.frames[replay_frame_index].expected_checksum)
                    {
                        record_thread_failure(Result::Failure(
                            ErrorCode::InvalidState, "hs_playtest",
                            std::format("Replay checksum mismatch at tick {}.", tick.tick)));
                        break;
                    }
                    if (replaying) ++replay_frame_index;
                    replay_scope.Stop();
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
                    observe_scope.Stop();

                    try
                    {
                        const bool recording = playtest_recorder.Active();
                        runtime_detail::DiagnosticScope fanout_scope(simulation_work[6], capture_clock);
                        channels.PublishAudioTick({model.session_id, probe.tick, tick.phase,
                            {probe.player_position.x, 0.0f, probe.player_position.y}});
                        channels.ProjectDomainSignalsOnce(model.domain_signals, model.arena_half_extent,
                            capture_diagnostics, recording ? &record_events : nullptr);
                        if (const auto footstep = footstep_timeline.Advance(
                                model, enemy_animations.PlayerMotionEpoch()))
                            channels.PublishAudioEvent(*footstep);
                        fanout_scope.Stop();
                        if (recording)
                        {
                            runtime_detail::DiagnosticScope recording_scope(simulation_work[7], capture_clock);
                            const auto diagnostics = simulation.GetDiagnostics();
                            SimulationObservation recorded_probe;
                            static_cast<SessionProbe &>(recorded_probe) = probe;
                            recorded_probe.balance = diagnostics.balance;
                            if (auto recorded = playtest_recorder.Record(
                                    input, tick.checksum, recorded_probe, record_events); !recorded)
                            {
                                record_thread_failure(recorded);
                                break;
                            }
                        }
                    }
                    catch (const std::exception &failure)
                    {
                        // Fanout may already be partial. Stop instead of replaying this tick.
                        record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime", failure.what()));
                        break;
                    }
                    catch (...)
                    {
                        record_thread_failure(Result::Failure(ErrorCode::TaskFailed, "hs_runtime",
                            "Simulation tick presentation failed."));
                        break;
                    }

                    const auto next_clock_mode = !realtime ? RenderClockMode::Scripted
                        : tick.phase == SessionPhase::Playing ? RenderClockMode::RealtimeRunning
                                                            : RenderClockMode::Frozen;
                    const bool new_session = !has_dynamic_sample ||
                        dynamic_current.header.session_id != model.session_id;
                    const bool tick_changed = !has_dynamic_sample ||
                        dynamic_current.header.tick != model.tick;
                    auto current_boundary = first_tick_boundary + FixedStepClock::kFixedStep * local_tick;
                    if (realtime && has_dynamic_sample &&
                        (new_session || model.tick < dynamic_current.header.tick ||
                         (display_clock_mode != RenderClockMode::RealtimeRunning &&
                          next_clock_mode == RenderClockMode::RealtimeRunning)))
                    {
                        current_boundary = FixedStepClock::Clock::now();
                        clock.Realign(current_boundary);
                        simulation_debt = {};
                        stop_remaining_admitted_steps = true;
                    }
                    if (new_session || next_clock_mode != display_clock_mode ||
                        (has_dynamic_sample && model.tick < dynamic_current.header.tick))
                        ++display_clock_epoch;
                    display_clock_mode = next_clock_mode;
                    if (new_session || tick_changed)
                    {
                        has_previous_dynamic_sample = has_dynamic_sample && !new_session &&
                            model.tick > dynamic_current.header.tick &&
                            model.tick - dynamic_current.header.tick == 1;
                        std::swap(dynamic_previous, dynamic_current);
                    }
                    const bool project_dynamic = new_session || tick_changed || tick.world_changed ||
                        forced_dynamic_refresh || harness_added != 0;
                    if (project_dynamic) dynamic_current.Clear();
                    if (project_dynamic)
                    {
                        runtime_detail::DiagnosticScope project_scope(simulation_work[9], capture_clock);
                        if (auto projected = ProjectDynamicTick(model, dynamic_current,
                                environment_cache.Resolve(model), &enemy_animations); !projected)
                        {
                            record_thread_failure(projected);
                            break;
                        }
                    }
                    if (project_dynamic) ++dynamic_pair_revision;
                    dynamic_current.header.checksum = model.checksum;
                    dynamic_current.header.dynamic_pair_revision = dynamic_pair_revision;
                    dynamic_current.header.clock = {display_clock_epoch, display_clock_mode,
                        realtime ? current_boundary
                                 : FixedStepClock::Clock::time_point{}};
                    configure_dynamic_camera(model);
                    has_dynamic_sample = true;

                    // Retain every actual dynamic sample, but publish UI only once after catch-up.
                    if (local_tick + 1 == tick_count || stop_remaining_admitted_steps ||
                        final_smoke_frame || config.smoke_render_lockstep)
                    {
                        runtime_detail::DiagnosticScope wait_scope(simulation_work[8], capture_clock);
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
                        wait_scope.Stop();
                        if (slot)
                        {
                            runtime_detail::DiagnosticScope project_scope(simulation_work[9], capture_clock);
                            const auto graphics_ui = channels.presentation_graphics_ui.load(std::memory_order_acquire);
                            const PresentationUiState ui_draw_state{
                                static_cast<UiPage>(channels.ui_page.load(std::memory_order_acquire)),
                                channels.collection_skill.load(std::memory_order_acquire),
                                channels.character_skill.load(std::memory_order_acquire),
                                channels.loadout_source.load(std::memory_order_acquire),
                                channels.settings_group.load(std::memory_order_acquire),
                                channels.settings_page.load(std::memory_order_acquire),
                                channels.character_details_page.load(std::memory_order_acquire)};
                            const auto modal_phase = display_modal_phase.load(std::memory_order_acquire);
                            const auto pending_rebind = channels.pending_rebind_slot.load(std::memory_order_acquire);
                            const auto input_rejected = channels.ui_input_rejected.load(std::memory_order_acquire);
                            try
                            {
                                slot->storage->CopyDynamicFrom(dynamic_current.View());
                                CaptureUiProjectionFacts(model, presentation_catalog, ui_draw_state,
                                    projection_settings, static_cast<std::uint8_t>(modal_phase),
                                    pending_rebind, input_rejected, ui_projection_facts);
                                const bool reuse_draw = prepared_ui &&
                                    runtime_detail::SameUiDrawInputs(*prepared_ui,
                                        ui_projection_facts, graphics_ui);
                                std::shared_ptr<const UiDrawContent> draw_content = reuse_draw
                                    ? prepared_ui->draw_content : nullptr;
                                std::shared_ptr<const std::vector<std::uint64_t>> draw_facts = reuse_draw
                                    ? prepared_ui->draw_facts : nullptr;
                                if (!reuse_draw)
                                {
                                    ProjectTickUi(model, presentation_catalog, ui_draw_state,
                                        projection_settings, *slot->storage, pending_rebind, graphics_ui.get());
                                    const auto page = ui_draw_state.page;
                                    if ((page == UiPage::MainMenuSettings || page == UiPage::PauseSettings) &&
                                        modal_phase != GraphicsUiPhase::Applied)
                                    {
                                        UiModel backdrop;
                                        backdrop.kind = UiModel::Kind::Panel;
                                        backdrop.anchor_pixels = {550, 360};
                                        backdrop.size_pixels = {820, 360};
                                        backdrop.color_rgba = 0xF0202430u;
                                        const auto title = SettingsStatusText(
                                            modal_phase == GraphicsUiPhase::AwaitingDisplayConfirmation
                                                ? "화면 모드를 유지하시겠습니까? (15초 안에 확인)"
                                                : modal_phase == GraphicsUiPhase::RestoringDisplay
                                                    ? "이전 화면 모드로 복원 중..."
                                                    : "화면 모드 적용 중...", {620, 440});
                                        slot->storage->AddUi(backdrop);
                                        slot->storage->AddUi(title);
                                        if (modal_phase == GraphicsUiPhase::AwaitingDisplayConfirmation)
                                        {
                                            slot->storage->AddUi(SettingsConfirmButton("유지", {650, 600}));
                                            slot->storage->AddUi(SettingsConfirmButton("취소", {1010, 600}));
                                        }
                                    }
                                    if (input_rejected)
                                        slot->storage->AddUi(SettingsStatusText(
                                            "입력을 처리하지 못했습니다. 잠시 후 다시 눌러 주세요.",
                                            {500, 1000}, 0xFF80C0FFu));
                                    auto content = std::make_shared<UiDrawContent>();
                                    content->models = slot->storage->TakeUiModels();
                                    draw_content = std::move(content);
                                    draw_facts = std::make_shared<const std::vector<std::uint64_t>>(
                                        ui_projection_facts);
                                }
                                auto ui_frame = std::make_shared<UiFrame>();
                                ui_frame->generation = publish_sequence + 1;
                                ui_frame->session_id = model.session_id;
                                ui_frame->selection_epoch = runtime_detail::UiSelectionEpoch(model.session);
                                ui_frame->session = model.session;
                                ui_frame->ui = ui_draw_state;
                                ui_frame->settings = projection_settings;
                                ui_frame->graphics_model = graphics_ui;
                                ui_frame->modal_phase = static_cast<std::uint8_t>(modal_phase);
                                ui_frame->pending_rebind_slot = pending_rebind;
                                ui_frame->input_rejected = input_rejected;
                                ui_frame->draw_facts = std::move(draw_facts);
                                ui_frame->draw_content = std::move(draw_content);
                                project_scope.Stop();
                                runtime_detail::DiagnosticScope publication_scope(simulation_work[10], capture_clock);
                                const auto previous_sample = dynamic_previous.View();
                                channels.snapshots.Publish(*slot, publish_sequence + 1,
                                    has_previous_dynamic_sample ? &previous_sample : nullptr, ui_frame);
                                ++publish_sequence;
                                prepared_ui = std::move(ui_frame);
                                snapshot_published = true;
                            }
                            catch (const std::bad_alloc &)
                            {
                                channels.snapshots.Abandon(*slot);
                                if (final_smoke_frame || config.smoke_render_lockstep)
                                {
                                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                        "Smoke UI frame could not be prepared."));
                                    break;
                                }
                            }
                            catch (...)
                            {
                                channels.snapshots.Abandon(*slot);
                                throw;
                            }
                        }
                        else if (final_smoke_frame || config.smoke_render_lockstep)
                        {
                            record_thread_failure(Result::Failure(
                                ErrorCode::InvalidState, "hs_gameplay",
                                "Smoke render snapshot could not be published."));
                            break;
                        }
                    }
                } // Read borrows end before clearing signals or the next mutation.
                simulation.ClearDomainSignals();
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
                    runtime_detail::DiagnosticScope heartbeat_scope(simulation_work[12], capture_clock);
                    WriteText(config.heartbeat_path,
                              std::format("{{\"tick\":{},\"state\":\"running\"}}\n",
                                          tick.tick));
                }

                iteration_scope.Stop();
                if (simulation_tasks)
                {
                    const auto gameplay = simulation.GetTimingDiagnostics();
                    runtime_detail::DiagnosticSample current_log{};
                    runtime_detail::DiagnosticScope log_scope(current_log, capture_clock);
                    simulation_task_row.clear();
                    runtime_detail::AppendCsvValues(simulation_task_row, tick.tick, ReadDiagnosticQpc(),
                        capture_frequency.QuadPart, captured_session, captured_probe.normal_enemy_count,
                        captured_probe.player_projectile_count, captured_probe.enemy_projectile_count, captured_signals,
                        snapshot_published ? 1 : 0, harness_added, config.diagnostic_enemy_count ? 1 : 0,
                        config.diagnostic_enemy_count == 0 && (config.smoke || config.record_playtest || replaying) ? 1 : 0);
                    AppendTaskValues(simulation_task_row, simulation_work, task_microseconds);
                    for (const auto &sample : gameplay.scopes)
                        runtime_detail::AppendCsvValues(simulation_task_row, sample.nanoseconds, sample.calls);
                    const auto io = simulation_tasks->Io();
                    runtime_detail::AppendCsvValues(simulation_task_row, gameplay.captured ? 1 : 0, gameplay.completed ? 1 : 0,
                        gameplay.empty_scope_calibration.nanoseconds, gameplay.empty_scope_calibration.calls,
                        previous_simulation_log.ticks * task_microseconds, previous_simulation_log.calls,
                        io.ticks * task_microseconds, io.calls, simulation_tasks->Bytes());
                    simulation_task_row += '\n';
                    simulation_tasks->Append(simulation_task_row);
                    log_scope.Stop();
                    previous_simulation_log = current_log;
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

            if (native_token) native_input_frame.Simulated(native_token, simulation_debt);
            if (smoke_finished)
            {
                break;
            }
        }
        if (simulation_tasks)
        {
            const auto finished = simulation_tasks->Finish();
            simulation_tasks_file.close();
            const auto complete = finished && simulation_tasks->Complete() && !simulation_tasks_file.fail();
            const auto io = simulation_tasks->Io();
            WriteText(config.artifact_directory / "simulation_tasks_status.json", std::format(
                "{{\"complete\":{},\"records\":{},\"dropped\":{},\"log_io_ticks\":{},\"log_io_calls\":{},\"log_bytes\":{},\"last_row_log_ticks\":{},\"qpc_frequency\":{}}}\n",
                complete, simulation_tasks->Records(), simulation_tasks->Dropped(), io.ticks, io.calls,
                simulation_tasks->Bytes(), previous_simulation_log.ticks, capture_frequency.QuadPart));
            if (!complete) record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Simulation task capture failed."));
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
    std::ofstream main_tasks_file;
    std::optional<runtime_detail::DiagnosticLog> main_tasks;
    std::string main_task_row;
    runtime_detail::DiagnosticSample previous_main_log{};
    std::uint64_t main_iteration{}, previous_wait_begin{}, previous_wait_end{};
    DWORD previous_wait_result{WAIT_TIMEOUT}, previous_wait_timeout{INFINITE};
    bool previous_wait_input_gated{};
    if (capture_clock)
    {
        main_tasks_file.open(config.artifact_directory / "main_tasks.csv", std::ios::binary);
        if (!main_tasks_file)
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Could not initialize main task capture."));
        std::string header = "iteration,completed_tick,qpc,qpc_frequency,foreground,session_phase,audio_depth,audio_consumed,audio_latency_us_sum,audio_latency_us_max,audio_latency_calls,played_total,active_voices,skipped_load_total,skipped_limit_total,audio_device_state,audio_native_error,skipped_missing_total";
        header += TaskColumns(kMainTaskNames);
        for (const auto name : kAudioWorkNames) header += std::format(",audio_{}_us_total,audio_{}_calls_total", name, name);
        header += ",reader_jobs_total,reader_failures_total,reader_bytes_total,reader_read_us_total,reader_queue_us_total,reader_request_ready_us_total,native_available,native_sample_qpc,native_samples_total,native_audio_cycles,native_total_cycles,native_min_cycles_quantum,native_max_cycles_quantum,native_active_voices,native_total_voices,native_glitches_total,native_latency_samples,native_output_sample_rate,previous_log_us,previous_log_calls,log_io_us_total,log_io_calls_total,log_bytes_total,previous_wait_begin_qpc,previous_wait_end_qpc,previous_wait_result,previous_wait_timeout_ms,previous_wait_input_gated,latest_notify_qpc,wake_sequence,audio_drain_submit_us_total,audio_drain_submit_us_max,audio_drain_submit_calls_total\n";
        main_tasks.emplace(main_tasks_file, true, header, capture_clock);
    }
    while (!channels.stop_requested.load(std::memory_order_acquire))
    {
        const auto processed_wake_sequence = main_wake.Sequence();
        main_work = {};
        runtime_detail::DiagnosticScope main_loop_scope(main_work[0], capture_clock);
        runtime_detail::DiagnosticScope input_scope(main_work[1], capture_clock);
        const auto iteration_qpc = capture_clock ? capture_clock() : 0;
        ui_click_settings = settings_commit.UiInput();
        std::uint64_t sampled_token{};
        Result window_failure = Result::Success();
        const bool sample_input = native_input_frame.ShouldPump(sampled_token);
        if (!window.PumpMessages(&window_failure, sample_input))
        {
            if (!window_failure) record_thread_failure(std::move(window_failure));
            channels.stop_requested.store(true, std::memory_order_release);
            native_input_frame.Cancel();
            break;
        }
        if (sampled_token) native_input_frame.Sampled(sampled_token);
        input_scope.Stop();
        runtime_detail::DiagnosticScope ui_scope(main_work[4], capture_clock);
        if (ui_command_queue_full)
        {
            OutputDebugStringA("UI command rejected: queue full. Retry with a new input.\n");
            ui_command_queue_full = false;
        }
        audio.SetBackgroundMuted(GetForegroundWindow() != window.Handle());
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
                    graphics_ui_phase.store((ui_command.kind != UiCommandKind::SetGraphicsSetting ||
                        GraphicsSettingTokenAvailable(ui_command.graphics_setting,
                            std::string_view(ui_command.graphics_token.data(),
                                std::ranges::find(ui_command.graphics_token, '\0') - ui_command.graphics_token.begin()), &ui_click_model)) &&
                        settings_commit.EditGraphics(ui_command)
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
                const auto state = settings_commit.TimedOut() ? "적용 시간이 초과되어 장치 작업 완료 후 복원합니다"
                    : settings_commit.PreparationFailed() ? "메모리가 부족해 이전 설정을 유지합니다"
                    : settings_commit.IsRecovering() ? "마지막으로 저장한 설정을 복원하는 중입니다"
                    : phase == GraphicsUiPhase::Requested ? "설정 변경을 요청했습니다"
                    : phase == GraphicsUiPhase::Pending ? "설정을 적용하는 중입니다"
                    : phase == GraphicsUiPhase::AwaitingDisplayConfirmation ? "화면 변경을 15초 안에 확인해 주세요"
                    : phase == GraphicsUiPhase::RestoringDisplay ? "이전 화면 설정을 복원하는 중입니다"
                    : phase == GraphicsUiPhase::ApplyFailed ? "적용할 수 없어 이전 설정을 유지합니다"
                    : receipt ? "적용 및 저장 완료 · 값이나 − / + 버튼으로 변경하세요" : "렌더러 상태를 확인하는 중입니다";
                model->transaction_state = std::string(state) + (save_failure ? " · 저장 실패: 이전 설정을 복원합니다" : "");
                if (receipt && receipt->generation != settings_commit.CommittedGeneration())
                    model->transaction_state += " · 새 적용 결과를 기다리는 중입니다";
                if (settings_commit.FollowupRejected()) model->transaction_state += " · 함께 사용할 수 없는 변경은 취소했습니다";
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
        ui_scope.Stop();
        runtime_detail::DiagnosticScope audio_drain_scope(main_work[5], capture_clock);
        const auto audio_depth = capture_clock ? channels.sim_audio_events.Size() : 0;
        std::uint64_t audio_consumed{}, audio_latency_ticks{}, audio_latency_max{}, audio_latency_calls{};
        main_audio->Drain(channels.sim_audio_events,
            [&](const QueuedAudioEvent &packet) {
                if (capture_clock && packet.kind == QueuedAudioEvent::Kind::Event)
                {
                    ++audio_consumed;
                    if (packet.enqueued_qpc)
                    {
                        const auto elapsed = runtime_detail::PerformanceLogState::ElapsedTicks(packet.enqueued_qpc, packet.dequeued_qpc);
                        audio_latency_ticks += elapsed;
                        audio_latency_max = std::max(audio_latency_max, elapsed);
                        ++audio_latency_calls;
                    }
                }
            });
        audio_drain_scope.Stop();
        runtime_detail::DiagnosticScope audio_state_scope(main_work[6], capture_clock);
        main_audio->Update();
        const auto session_phase = main_audio->Phase();
        if (main_audio->Failure() != runtime_detail::MainAudioFailure::None)
        {
            const auto failure = audio.Status();
            const auto &detail = main_audio->FailureDetails();
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_audio",
                std::format("Audio delivery failed (boundary={}, cue_id={}, readiness={}, play_result={}, session={}, tick={}, phase={}, stage={}, loaded_files={}, load_failures={}, native_operation={}, HRESULT={}, cleanup_operation={}, cleanup_HRESULT={}).",
                    static_cast<unsigned>(main_audio->Failure()), detail.cue.value, static_cast<unsigned>(detail.readiness),
                    static_cast<unsigned>(detail.play_result), detail.session_id, detail.tick, static_cast<unsigned>(detail.phase), detail.stage,
                    failure.loaded_files, failure.skipped_load_failures, static_cast<unsigned>(failure.native_operation),
                    failure.native_error, static_cast<unsigned>(failure.native_cleanup_operation), failure.native_cleanup_error)));
        }
        audio_state_scope.Stop();
        main_loop_scope.Stop();
        if (main_tasks)
        {
            runtime_detail::DiagnosticScope capture_scope(main_work[7], capture_clock);
            const auto work = audio.CaptureTimings();
            const auto status = audio.Status();
            capture_scope.Stop();
            runtime_detail::DiagnosticSample current_log{};
            runtime_detail::DiagnosticScope log_scope(current_log, capture_clock);
            main_task_row.clear();
            runtime_detail::AppendCsvValues(main_task_row,
                ++main_iteration, channels.completed_tick.load(std::memory_order_acquire), iteration_qpc, capture_frequency.QuadPart,
                GetForegroundWindow() == window.Handle() ? 1 : 0, static_cast<unsigned>(session_phase),
                audio_depth, audio_consumed,
                audio_latency_ticks * task_microseconds, audio_latency_max * task_microseconds, audio_latency_calls,
                status.played_count, status.active_source_voices, status.skipped_load_failures, status.skipped_voice_limits,
                static_cast<unsigned>(status.device_state), status.native_error, status.skipped_missing_cues);
            AppendTaskValues(main_task_row, main_work, task_microseconds);
            for (const auto &sample : work.scopes)
                runtime_detail::AppendCsvValues(main_task_row, sample.ticks * task_microseconds, sample.calls);
            runtime_detail::AppendCsvValues(main_task_row,
                work.reader_jobs, work.reader_failures, work.reader_bytes, work.reader_read_ticks * task_microseconds,
                work.reader_queue_ticks * task_microseconds, work.reader_request_ready_ticks * task_microseconds,
                work.native_available ? 1 : 0, work.native_sample_qpc, work.native_samples, work.audio_cycles, work.total_cycles,
                work.minimum_cycles_per_quantum, work.maximum_cycles_per_quantum, work.native_active_voices,
                work.native_total_voices, work.glitches, work.latency_samples, work.output_sample_rate);
            const auto io = main_tasks->Io();
            runtime_detail::AppendCsvValues(main_task_row, previous_main_log.ticks * task_microseconds, previous_main_log.calls,
                io.ticks * task_microseconds, io.calls, main_tasks->Bytes(), previous_wait_begin, previous_wait_end,
                previous_wait_result, previous_wait_timeout, previous_wait_input_gated ? 1 : 0,
                main_wake.LatestNotifyQpc(), main_wake.Sequence(), work.drain_submit_ticks * task_microseconds,
                work.drain_submit_max_ticks * task_microseconds, work.drain_submit_calls);
            main_task_row += '\n';
            main_tasks->Append(main_task_row);
            log_scope.Stop();
            previous_main_log = current_log;
        }
        DWORD timeout = INFINITE;
        const auto include_deadline = [&](std::chrono::steady_clock::time_point deadline) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
            timeout = std::min(timeout, milliseconds <= 0 ? 0u : static_cast<DWORD>(std::min<std::int64_t>(milliseconds, INFINITE - 1ull)));
        };
        if (display_modal_phase.load(std::memory_order_acquire) == GraphicsUiPhase::AwaitingDisplayConfirmation)
            include_deadline(display_confirmation_deadline);
        if (settings_commit.HasPending() && !settings_commit.TimedOut()) include_deadline(settings_request_deadline);
        const auto pending_main_work = [&]() noexcept {
            MSG pending{};
            const auto input_gated = native_input_frame.InputGated();
            const bool messages = runtime_detail::PeekMainMessage(pending, !input_gated, PM_NOREMOVE);
            return messages || channels.stop_requested.load(std::memory_order_acquire) ||
                channels.sim_audio_events.Size() != 0 ||
                (!deferred_settings_ack && channels.renderer_settings_acks.Size() != 0) ||
                channels.ui_commands.Size() != 0 || native_input_frame.MainWorkPending() ||
                channels.renderer_graphics_ui.load(std::memory_order_acquire) != last_renderer_receipt;
        };
        previous_wait_begin = previous_wait_end = 0;
        previous_wait_input_gated = native_input_frame.InputGated();
        previous_wait_timeout = timeout;
        previous_wait_result = WAIT_OBJECT_0;
        if (main_wake.PrepareWait(pending_main_work, processed_wake_sequence))
        {
            const auto generation = main_wake.Sequence();
            // Recheck immediately before entering the OS wait. A later successful
            // producer push sets the manual event and cannot be lost by this wait.
            if (!pending_main_work() && main_wake.Sequence() == generation)
            {
                const auto handle = main_wake.Handle();
                previous_wait_input_gated = native_input_frame.InputGated();
                previous_wait_begin = capture_clock ? capture_clock() : 0;
                previous_wait_result = MsgWaitForMultipleObjectsEx(1, &handle, timeout,
                    runtime_detail::MainMessageMask(previous_wait_input_gated),
                    runtime_detail::MainMessageFlags(previous_wait_input_gated));
                const auto wait_error = previous_wait_result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
                previous_wait_end = capture_clock ? capture_clock() : 0;
                if (previous_wait_result == WAIT_FAILED)
                    record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                        std::format("Main message/event wait failed: {}.", wait_error)));
            }
        }
        if (const auto error = main_wake.Error())
            record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                std::format("Main wake event failed: {}.", error)));
    }
    if (main_tasks)
    {
        const auto finished = main_tasks->Finish();
        main_tasks_file.close();
        const auto complete = finished && main_tasks->Complete() && !main_tasks_file.fail();
        const auto io = main_tasks->Io();
        WriteText(config.artifact_directory / "main_tasks_status.json", std::format(
            "{{\"complete\":{},\"records\":{},\"dropped\":{},\"log_io_ticks\":{},\"log_io_calls\":{},\"log_bytes\":{},\"last_row_log_ticks\":{},\"qpc_frequency\":{}}}\n",
            complete, main_tasks->Records(), main_tasks->Dropped(), io.ticks, io.calls, main_tasks->Bytes(),
            previous_main_log.ticks, capture_frequency.QuadPart));
        if (!complete) record_thread_failure(Result::Failure(ErrorCode::InvalidState, "hs_runtime", "Main task capture failed."));
    }

    if (capture_clock)
    {
        const auto &delivery = main_audio->Stats();
        const auto &failure = main_audio->FailureDetails();
        WriteText(config.artifact_directory / "audio_delivery.json", std::format(
            "{{\"delivered\":{},\"discarded_old_session\":{},\"tick_boundaries\":{},\"event_age_ticks_total\":{},\"maximum_event_age_ticks\":{},\"event_age_samples\":{},\"merge_policy\":\"bounded sim FIFO batch, latest observed session, then render FIFO; refresh preceding sim boundary on session mismatch\",\"age_policy\":\"measure same-session tick age; no arbitrary time cutoff\",\"failure_boundary\":{},\"failure_cue_id\":{},\"failure_readiness\":{},\"failure_play_result\":{},\"failure_session_id\":{},\"failure_tick\":{},\"failure_phase\":{},\"failure_stage\":\"{}\",\"pending_music\":{},\"pending_ambience\":{},\"active_music\":{},\"active_ambience\":{}}}\n",
            delivery.delivered, delivery.discarded_old_session, delivery.tick_boundaries, delivery.event_age_ticks,
            delivery.maximum_event_age_ticks, delivery.event_age_samples,
            static_cast<unsigned>(main_audio->Failure()), failure.cue.value, static_cast<unsigned>(failure.readiness),
            static_cast<unsigned>(failure.play_result), failure.session_id, failure.tick, static_cast<unsigned>(failure.phase), failure.stage,
            main_audio->HasPendingMusic(), main_audio->HasPendingAmbience(), main_audio->ActiveMusic().value, main_audio->ActiveAmbience().value));
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
    auto valid =
        result && final_tick >= (config.smoke ? config.maximum_ticks : 0) && rendered_frames > 0 &&
        rendered_particles <= 10'000u &&
        channels.dropped_input_edges.load() == 0 &&
        channels.dropped_presentation_events.load() == 0;
    if (playtest_recorder.Active())
    {
        if (auto finished = playtest_recorder.Finish(valid); !finished)
        {
            result = finished;
            valid = false;
        }
    }

    if (capture_diagnostics)
    {
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
    }

    if (!valid && result)
    {
        result = Result::Failure(ErrorCode::InvalidState, "hs_runtime",
                                 "Runtime execution assertions failed.");
    }
    return {result, final_tick, checksum, rendered_frames, playtest_recorder.Directory()};
}

} // namespace hs
