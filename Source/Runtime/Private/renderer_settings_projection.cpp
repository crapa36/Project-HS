#include "renderer_settings_projection.hpp"
#include "window.hpp"
#include <hs/presentation/projector.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hs
{
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

} // namespace hs
