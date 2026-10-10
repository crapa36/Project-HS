#include <hs/presentation/projector.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <ranges>
#include <utility>
#include <charconv>
#include <type_traits>
#include <limits>

namespace hs
{
std::size_t GraphicsSettingsPageCount(std::uint8_t group)
{
    if (group >= 7) return 1;
    const auto count = std::ranges::count_if(GraphicsSettingDescriptors(), [&](const auto &d) {
        return static_cast<std::uint8_t>(d.group) == group;
    });
    return std::max<std::size_t>(1, (static_cast<std::size_t>(count) + kGraphicsUiRowsPerPage - 1) / kGraphicsUiRowsPerPage);
}
std::vector<const GraphicsSettingDescriptor *> GraphicsSettingsVisibleRows(const PresentationUiState &ui)
{
    std::vector<const GraphicsSettingDescriptor *> rows;
    if (ui.settings_group >= 7) return rows;
    const auto page = std::min<std::size_t>(ui.settings_page, GraphicsSettingsPageCount(ui.settings_group) - 1);
    std::size_t index{};
    for (const auto &d : GraphicsSettingDescriptors())
        if (static_cast<std::uint8_t>(d.group) == ui.settings_group)
        {
            if (index / kGraphicsUiRowsPerPage == page) rows.push_back(&d);
            ++index;
        }
    return rows;
}
std::string GraphicsSettingDisplayLabel(const GraphicsSettingDescriptor &descriptor)
{
    static constexpr std::array<std::string_view, kGraphicsSettingCount> labels{
        "그래픽 품질", "화면 모드", "모니터", "출력 해상도", "수직 동기화", "최대 프레임",
        "HDR 모드", "HDR 기준 밝기", "HDR 최대 밝기", "업스케일링 방식", "업스케일링 품질",
        "해상도 설정 방식", "렌더링 해상도 비율", "동적 해상도", "목표 프레임",
        "동적 해상도 최소 비율", "동적 해상도 최대 비율", "선명도", "프레임 생성 방식",
        "프레임 생성 모드", "프레임 생성 배수", "입력 지연 줄이기", "텍스처 품질",
        "텍스처 메모리 한도", "이방성 필터링", "모델 품질", "식생 품질", "재질 품질",
        "표면 입체감 품질", "조명 품질", "그림자 방식", "그림자 품질", "그림자 표시 거리",
        "접촉 그림자", "주변 음영 방식", "주변 음영 품질", "간접 조명 방식", "간접 조명 품질",
        "반사 방식", "반사 품질", "레이 트레이싱", "레이 트레이싱 거리", "신경망 광선 복원",
        "광선 복원 방식", "투명 효과 품질", "굴절 품질", "안개와 광선 품질", "파티클 품질",
        "빛 번짐", "빛 번짐 강도", "모션 블러", "피사계 심도", "외곽선", "외곽선 두께",
        "색 보정", "적응형 셰이딩"};
    const auto index = static_cast<std::size_t>(descriptor.id);
    return index > 0 && index <= labels.size() ? std::string(labels[index - 1]) : std::string(descriptor.key);
}
std::string GraphicsSettingDisplayValue(SettingId id, std::string_view token)
{
    if (token == "0" && id == SettingId::FrameLimitFps) return "제한 없음";
    if (token == "0" && id == SettingId::TextureBudgetMib) return "자동";
    if (token == "internal" && id == SettingId::RayReconstructionProvider) return "기본 광선 노이즈 제거";
    static constexpr std::pair<std::string_view, std::string_view> values[]{
        {"true", "켜짐"}, {"false", "꺼짐"}, {"on", "켜짐"}, {"off", "꺼짐"}, {"auto", "자동"},
        {"low", "낮음"}, {"medium", "보통"}, {"high", "높음"}, {"ultra", "최고"},
        {"ultra_rt", "최고 + 레이 트레이싱"}, {"custom", "사용자 설정"}, {"windowed", "창 모드"},
        {"borderless_fullscreen", "테두리 없는 전체 화면"}, {"manual", "직접 설정"},
        {"provider_quality", "업스케일링 품질에 맞춤"}, {"quality", "품질 우선"},
        {"balanced", "균형"}, {"performance", "성능 우선"}, {"ultra_performance", "최대 성능"},
        {"native", "기본 해상도"}, {"internal", "기본 TAA"}, {"native_taa", "기본 해상도 TAA"},
        {"internal_taau", "기본 TAA 업스케일링"}, {"dlss", "NVIDIA DLSS"}, {"dlss_sr", "NVIDIA DLSS"},
        {"dlss_rr", "NVIDIA DLSS 광선 복원"}, {"fsr", "AMD FSR"}, {"xess", "Intel XeSS"},
        {"fsr_sr", "AMD FSR"}, {"fsr_rr", "AMD FSR 광선 복원"}, {"xess_sr", "Intel XeSS"},
        {"nrd_reblur", "NRD REBLUR 광선 노이즈 제거"}, {"nrd_relax", "NRD RELAX 광선 노이즈 제거"},
        {"directsr", "DirectSR"}, {"nrd", "NRD 광선 노이즈 제거"}, {"fixed", "고정 배수"},
        {"dynamic", "동적 배수"}, {"raster", "기본 그림자"}, {"hybrid_rt", "레이 트레이싱 혼합"},
        {"gtao", "화면 기반 음영 (GTAO)"}, {"rtao", "레이 트레이싱 음영"},
        {"probe", "미리 계산한 조명"}, {"software_hybrid", "소프트웨어 혼합"},
        {"ray_traced", "레이 트레이싱"}, {"screen_space", "화면 기반 반사"}};
    for (const auto &[value, label] : values) if (token == value) return std::string(label);
    std::string display_number;
    const auto descriptors = GraphicsSettingDescriptors();
    const auto descriptor = std::ranges::find(descriptors, id, &GraphicsSettingDescriptor::id);
    if (descriptor != descriptors.end() && (descriptor->type == GraphicsSettingType::Float ||
                                            descriptor->type == GraphicsSettingType::AutoFloat))
    {
        float value{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() && std::isfinite(value))
        {
            std::array<char, 64> buffer{};
            const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                               std::chars_format::fixed, 3);
            if (written.ec == std::errc{})
            {
                auto end = written.ptr;
                while (end > buffer.data() && end[-1] == '0') --end;
                if (end > buffer.data() && end[-1] == '.') --end;
                display_number.assign(buffer.data(), end);
                token = display_number;
            }
        }
    }
    switch (id)
    {
    case SettingId::ManualScalePercent: case SettingId::DynamicMinPercent:
    case SettingId::DynamicMaxPercent: return std::string(token) + "%";
    case SettingId::FrameLimitFps: case SettingId::TargetRenderFps: return std::string(token) + " FPS";
    case SettingId::TextureBudgetMib: return std::string(token) + " MiB";
    case SettingId::ShadowDistanceM: case SettingId::RtTraceDistanceM: return std::string(token) + " m";
    case SettingId::HdrPaperWhiteNits: case SettingId::HdrPeakNits: return std::string(token) + " nit";
    case SettingId::OutlineWidthPx: return std::string(token) + " px";
    case SettingId::FrameGenerationMultiplier: case SettingId::AnisotropicFiltering: return std::string(token) + "배";
    default: return std::string(token);
    }
}
Float2 ReferenceUiCursor(Float2 client_pixels, GraphicsExtent extent) noexcept
{
    const auto width = static_cast<float>(std::max(extent.width, 1u));
    const auto height = static_cast<float>(std::max(extent.height, 1u));
    const auto scale = std::min(width / 1920.0f, height / 1080.0f);
    return {(client_pixels.x - width * 0.5f) / (scale * 960.0f),
            (height * 0.5f - client_pixels.y) / (scale * 540.0f)};
}
namespace
{
template<class T> std::vector<std::string> EnumChoices(const T &)
{
    std::vector<std::string> values;
    if constexpr (std::is_enum_v<T>)
        for (unsigned i = 0; i < 16; ++i)
        {
            const auto token = GraphicsEnumToken(static_cast<T>(i));
            if (!token.empty()) values.emplace_back(token);
        }
    return values;
}
double NumericStep(SettingId id)
{
    switch (id)
    {
    case SettingId::TargetRenderFps: return 5;
    case SettingId::TextureBudgetMib: return 128;
    case SettingId::HdrPaperWhiteNits: return 10;
    case SettingId::HdrPeakNits: return 50;
    case SettingId::ManualScalePercent: case SettingId::DynamicMinPercent:
    case SettingId::DynamicMaxPercent: case SettingId::ShadowDistanceM:
    case SettingId::RtTraceDistanceM: return 5;
    case SettingId::Sharpening: case SettingId::BloomStrength:
    case SettingId::OutlineWidthPx: return .1;
    default: return 1;
    }
}
}
bool GraphicsSettingTokenAvailable(SettingId id, std::string_view token, const GraphicsSettingsUiModel *model)
{
    if (id == SettingId::Preset && token == "ultra_rt")
        return GraphicsSettingTokenAvailable(SettingId::RayTracingEnabled, "true", model) &&
            GraphicsSettingTokenAvailable(SettingId::AoMode, "auto", model) &&
            GraphicsSettingTokenAvailable(SettingId::GiMode, "ray_traced", model) &&
            GraphicsSettingTokenAvailable(SettingId::ReflectionMode, "hybrid_rt", model) &&
            GraphicsSettingTokenAvailable(SettingId::ShadowMode, "hybrid_rt", model);
    if (model)
    {
        const auto choice = std::ranges::find_if(model->choices, [&](const auto &entry) {
            return entry.setting == id && entry.token == token;
        });
        if (choice != model->choices.end()) return choice->available;
        if (std::ranges::any_of(model->choices, [&](const auto &entry) { return entry.setting == id; })) return false;
    }
    switch (id)
    {
    case SettingId::HdrMode: case SettingId::ReconstructionProvider: case SettingId::ReconstructionQuality:
    case SettingId::FrameGenerationProvider: case SettingId::FrameGenerationMode: case SettingId::FrameGenerationMultiplier:
    case SettingId::LowLatency: case SettingId::AoMode: case SettingId::GiMode: case SettingId::ReflectionMode:
    case SettingId::ShadowMode: case SettingId::RayTracingEnabled: case SettingId::NeuralRayReconstruction:
    case SettingId::RayReconstructionProvider: case SettingId::AdaptiveShading:
        return token == "off" || token == "auto" || token == "false";
    default: return true;
    }
}
std::optional<std::string> NextGraphicsSettingToken(const GraphicsSettings &settings, SettingId id,
                                                   int direction, const GraphicsSettingsUiModel *model)
{
    const auto descriptors = GraphicsSettingDescriptors();
    const auto d = std::ranges::find(descriptors, id, &GraphicsSettingDescriptor::id);
    if (d == descriptors.end() || (direction != -1 && direction != 1)) return {};
    const auto current = GraphicsSettingToken(settings, id);
    std::vector<std::string> values;
    if (id == SettingId::FrameGenerationMultiplier)
    {
        if (!model || !model->frame_generation_maximum || *model->frame_generation_maximum < 2)
            return {}; // The pinned FSR API exposes no configurable multiplier.
        const auto maximum = *model->frame_generation_maximum;
        for (auto next = static_cast<std::int64_t>(settings.frame_generation_multiplier) + direction;
             next >= 2 && next <= maximum; next += direction)
            if (auto token = std::to_string(next); GraphicsSettingTokenAvailable(id, token, model)) return token;
        return {};
    }
    if (d->type == GraphicsSettingType::Boolean) values = {"false", "true"};
    else if (d->type == GraphicsSettingType::Enum)
    {
        switch (id)
        {
#define HS_GRAPHICS_SETTING(number, name, field, type, kind, default_value, default_token, minimum, maximum, group, label_key) case SettingId::name: values = EnumChoices(settings.field); break;
#include <hs/core/graphics_settings.def>
#undef HS_GRAPHICS_SETTING
        }
    }
    else if (id == SettingId::FrameLimitFps)
    {
        std::vector<std::uint32_t> limits{0, 30, 60, 90, 120, 144, 165, 180, 240, 360, 480, 1000};
        const auto position = std::ranges::lower_bound(limits, settings.frame_limit_fps);
        if (position == limits.end() || *position != settings.frame_limit_fps)
            limits.insert(position, settings.frame_limit_fps);
        for (const auto limit : limits) values.push_back(std::to_string(limit));
    }
    else if (id == SettingId::AnisotropicFiltering) values = {"1", "2", "4", "8", "16"};
    else if (d->type == GraphicsSettingType::DisplayId || d->type == GraphicsSettingType::ProviderId ||
             d->type == GraphicsSettingType::ModeId || d->type == GraphicsSettingType::Extent)
    {
        if (d->type != GraphicsSettingType::ModeId) values.emplace_back("auto");
        if (model) for (const auto &choice : model->choices)
            if (choice.setting == id && std::ranges::find(values, choice.token) == values.end())
                values.push_back(choice.token);
        if (values.empty()) values.push_back(current);
    }
    else
    {
        double value = d->minimum;
        if (current != "auto")
        {
            const auto parsed = std::from_chars(current.data(), current.data() + current.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != current.data() + current.size() || !std::isfinite(value)) return {};
        }
        if (d->type == GraphicsSettingType::AutoFloat && direction < 0 && value <= d->minimum)
            return current == "auto" ? std::nullopt : std::optional(std::string("auto"));
        value = current == "auto" ? d->minimum : std::clamp(value + direction * NumericStep(id), d->minimum, d->maximum);
        std::array<char, 64> buffer{};
        std::to_chars_result written;
        if (d->type == GraphicsSettingType::Unsigned)
            written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<std::uint32_t>(value));
        else written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<float>(value), std::chars_format::general, 6);
        if (written.ec != std::errc{}) return {};
        const std::string token(buffer.data(), written.ptr);
        if (token == current) return {};
        auto candidate = settings;
        return SetGraphicsSettingToken(candidate, id, token) ? std::optional(token) : std::nullopt;
    }
    std::erase_if(values, [&](const auto &token) { return !GraphicsSettingTokenAvailable(id, token, model); });
    if (values.empty()) return {};
    const auto selected = std::ranges::find(values, current);
    const auto index = selected == values.end() ? (direction > 0 ? 0 : values.size() - 1)
        : static_cast<std::size_t>((static_cast<std::ptrdiff_t>(selected - values.begin()) + direction +
            static_cast<std::ptrdiff_t>(values.size())) % static_cast<std::ptrdiff_t>(values.size()));
    auto candidate = settings;
    return values[index] != current && SetGraphicsSettingToken(candidate, id, values[index]) ? std::optional(values[index]) : std::nullopt;
}

UiInteraction ResolveUiInteraction(const SessionProbe &session,
                                   PresentationUiState &ui,
                                   const SettingsData &settings,
                                   Float2 cursor_normalized,
                                   const GraphicsSettingsUiModel *graphics_ui)
{
    const auto cursor = Float2{(cursor_normalized.x + 1.0f) * 960.0f,
                               (1.0f - cursor_normalized.y) * 540.0f};
    const auto clicked = [&](float x, float y, float width, float height) {
        return cursor.x >= x && cursor.x <= x + width &&
               cursor.y >= y && cursor.y <= y + height;
    };
    const auto action = [](UiActionKind kind, std::uint8_t value = 0,
                           std::uint8_t secondary = 0) {
        return UiInteraction{UiAction{kind, value, secondary}, std::nullopt};
    };
    const auto command = [](UiCommandKind kind, std::uint32_t value) {
        return UiInteraction{std::nullopt, UiCommand{kind, value}};
    };
    const auto &probe = session;

    if (ui.page == UiPage::MainMenuSettings || ui.page == UiPage::PauseSettings)
    {
        for (std::uint8_t group = 0; group < 8; ++group)
            if (clicked(210.0f + group * 190.0f, 175, 180, 48))
            { ui.settings_group = group; ui.settings_page = 0; return {}; }
        if (clicked(1250, 900, 160, 56))
        { if (ui.settings_page != 0) --ui.settings_page; return {}; }
        if (clicked(1450, 900, 160, 56))
        { if (ui.settings_page + 1 < GraphicsSettingsPageCount(ui.settings_group)) ++ui.settings_page; return {}; }
        if (clicked(210, 900, 300, 56))
        { ui.page = UiPage::Root; return {std::nullopt, UiCommand{UiCommandKind::CancelSkillRebind, 0}}; }
        if (ui.settings_group < 7)
        {
            const GraphicsSettings defaults;
            const auto &requested = graphics_ui ? graphics_ui->requested : defaults;
            const auto rows = GraphicsSettingsVisibleRows(ui);
            for (std::size_t index = 0; index < rows.size(); ++index)
            {
                const auto y = kGraphicsUiRowY + index * kGraphicsUiRowStride;
                const int direction = clicked(1450, y, 64, 48) ? -1
                    : (clicked(1550, y, 64, 48) || clicked(900, y, 530, 48)) ? 1 : 0;
                if (!direction) continue;
                const auto token = NextGraphicsSettingToken(requested, rows[index]->id, direction, graphics_ui);
                if (!token || token->size() >= UiCommand{}.graphics_token.size()) return {std::nullopt, std::nullopt, true};
                UiCommand edit{UiCommandKind::SetGraphicsSetting};
                edit.graphics_setting = rows[index]->id;
                std::copy(token->begin(), token->end(), edit.graphics_token.begin());
                return {std::nullopt, edit};
            }
            return {};
        }
        constexpr std::array volume_kinds{
            UiCommandKind::SetMasterVolumePercent, UiCommandKind::SetBgmVolumePercent,
            UiCommandKind::SetSfxVolumePercent, UiCommandKind::SetUiVolumePercent};
        const std::array volumes{settings.master_volume, settings.bgm_volume,
                                 settings.sfx_volume, settings.ui_volume};
        for (std::size_t index = 0; index < volumes.size(); ++index)
        {
            const auto y = kGraphicsUiRowY + index * kGraphicsUiRowStride;
            if (!clicked(1450, y, 64, 48) && !clicked(1550, y, 64, 48) &&
                !clicked(900, y, 530, 48)) continue;
            const auto delta = clicked(1450, y, 64, 48) ? -0.1f : 0.1f;
            const auto value = std::round(std::clamp(volumes[index] + delta,
                                                     0.0f, 1.0f) * 10.0f) / 10.0f;
            return command(volume_kinds[index],
                           static_cast<std::uint32_t>(std::lround(value * 100.0f)));
        }
        if (clicked(1450, kGraphicsUiRowY + 4 * kGraphicsUiRowStride, 64, 48) ||
            clicked(1550, kGraphicsUiRowY + 4 * kGraphicsUiRowStride, 64, 48) ||
            clicked(900, kGraphicsUiRowY + 4 * kGraphicsUiRowStride, 530, 48))
        {
            const auto delta = clicked(1450, kGraphicsUiRowY + 4 * kGraphicsUiRowStride, 64, 48) ? -10 : 10;
            return command(UiCommandKind::SetVfxScreenOverlayIntensityPercent,
                static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::lround(settings.screen_overlay_intensity * 100)) + delta, 0, 100)));
        }
        for (std::uint8_t slot = 0; slot < 4; ++slot)
            if (clicked(950, kGraphicsUiRowY + (slot + 5) * kGraphicsUiRowStride, 664, 48))
                return command(UiCommandKind::BeginSkillRebind, slot);
        return {};
    }

    if (probe.phase == SessionPhase::MainMenu)
    {
        if (ui.page == UiPage::Collection)
        {
            for (std::uint8_t skill = 0; skill < kCombatSkillCount; ++skill)
                if (clicked(210, 150.0f + skill * 70.0f, 360, 56))
                {
                    ui.selected_collection_skill = skill;
                    return {};
                }
            if (clicked(210, 900, 360, 56)) { ui.page = UiPage::Root; return {}; }
        }
        else
        {
            if (clicked(760, 270, 400, 92)) return action(UiActionKind::StartSession);
            if (clicked(760, 420, 400, 92)) { ui.page = UiPage::Collection; return {}; }
            if (clicked(760, 570, 400, 92)) { ui.page = UiPage::MainMenuSettings; return {}; }
            if (clicked(760, 720, 400, 92)) return action(UiActionKind::Quit);
        }
    }
    if (probe.phase == SessionPhase::CardSelection ||
        probe.phase == SessionPhase::RelicSelection)
    {
        if (clicked(760, 790, 400, 72)) return action(UiActionKind::Reroll);
        for (std::uint8_t index = 0; index < 3; ++index)
            if (clicked(360.0f + index * 420.0f, 300, 360, 420))
                return action(UiActionKind::SelectCard, index);
    }
    if (probe.phase == SessionPhase::StatAllocation)
    {
        for (std::uint8_t index = 0; index < kStatCount; ++index)
            if (clicked(390.0f + (index % 3) * 400.0f,
                        310.0f + (index / 3) * 260.0f, 340, 180))
                return action(UiActionKind::AssignStat, index);
    }
    if (probe.phase == SessionPhase::Paused &&
        (ui.page == UiPage::CharacterOverview ||
         ui.page == UiPage::CharacterSkills ||
         ui.page == UiPage::CharacterStats))
    {
        if (clicked(350, 140, 280, 58)) { ui.page = UiPage::CharacterOverview; return {}; }
        if (clicked(650, 140, 280, 58)) { ui.page = UiPage::CharacterSkills; return {}; }
        if (clicked(950, 140, 280, 58)) { ui.page = UiPage::CharacterStats; return {}; }
        if (clicked(1520, 140, 120, 58))
        {
            ui = {};
            return action(UiActionKind::Resume);
        }
        if (ui.page == UiPage::CharacterSkills)
        {
            for (std::uint8_t slot = 0; slot < 4; ++slot)
                if (clicked(780.0f + slot * 195.0f, 225, 180, 54))
                {
                    if (ui.loadout_source_slot == 0xFF)
                    {
                        if (probe.skill_loadout[slot] != SkillKind::Count)
                            ui.loadout_source_slot = slot;
                        return {};
                    }
                    if (ui.loadout_source_slot == slot)
                    {
                        ui.loadout_source_slot = 0xFF;
                        return {};
                    }
                    const auto source = std::exchange(ui.loadout_source_slot,
                                                      std::uint8_t{0xFF});
                    return action(UiActionKind::SwapLoadoutSlots, source, slot);
                }
            for (std::uint8_t skill = 0; skill < kCombatSkillCount; ++skill)
                if (clicked(350, 240.0f + skill * 78.0f, 360, 64))
                {
                    if (probe.skill_levels[skill] == 0)
                        return {std::nullopt, std::nullopt, true};
                    ui.selected_character_skill = skill;
                    return {};
                }
        }
    }
    if (probe.phase == SessionPhase::Paused && ui.page == UiPage::Root)
    {
        if (clicked(760, 420, 400, 72)) return action(UiActionKind::Resume);
        if (clicked(760, 520, 400, 72)) { ui.page = UiPage::PauseSettings; return {}; }
        if (clicked(760, 620, 400, 72)) return action(UiActionKind::Quit);
    }
    if (probe.phase == SessionPhase::Victory || probe.phase == SessionPhase::Defeat)
        return action(UiActionKind::ReturnToMainMenu);
    return {};
}

} // namespace hs
