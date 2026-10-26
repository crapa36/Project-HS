#include "gameplay_test_support.hpp"
#include "../Source/Runtime/Private/process_scene.hpp"
#include <type_traits>
#include <hs/presentation/meadow_surface.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <filesystem>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>
#include <vector>

namespace gameplay_test
{

namespace
{
void CheckSettingsTelemetry(hs::GameSimulation &simulation)
{
    for (const auto &descriptor : hs::GraphicsSettingDescriptors())
    {
        const auto label = hs::GraphicsSettingDisplayLabel(descriptor);
        Check(std::ranges::any_of(label, [](unsigned char byte) { return byte >= 0x80; }),
              "every graphics setting has a Korean display label");
    }
    Check(hs::GraphicsSettingDisplayValue(hs::SettingId::Vsync, "true") == "켜짐" &&
              hs::GraphicsSettingDisplayValue(hs::SettingId::FrameLimitFps, "0") == "제한 없음" &&
              hs::GraphicsSettingDisplayValue(hs::SettingId::ManualScalePercent, "75") == "75%" &&
              hs::GraphicsSettingDisplayValue(hs::SettingId::ReconstructionProvider, "internal_taau") == "기본 TAA 업스케일링",
          "settings localize values and show meaningful units");
    Check(hs::GraphicsSettingDisplayValue(hs::SettingId::Sharpening, "0.200000003") == "0.2" &&
              hs::GraphicsSettingDisplayValue(hs::SettingId::BloomStrength, "0.100000001") == "0.1" &&
              hs::GraphicsSettingDisplayValue(hs::SettingId::OutlineWidthPx, "1.23456789") == "1.235 px",
          "float display removes binary precision noise and retains three decimal places");
    Check(hs::GraphicsSettingDisplayValue(hs::SettingId::HdrPeakNits, "1000.12500001") == "1000.125 nit" &&
              hs::GraphicsSettingDisplayValue(hs::SettingId::HdrPeakNits, "auto") == "자동" &&
              hs::GraphicsSettingDisplayValue(hs::SettingId::FrameLimitFps, "144") == "144 FPS",
          "auto-float display keeps units while automatic and integer values stay intact");
    hs::GraphicsSettings capped;
    capped.frame_limit_fps = 120;
    Check(hs::NextGraphicsSettingToken(capped, hs::SettingId::FrameLimitFps, 1, nullptr) == "144",
          "common high-refresh frame caps are selectable");
    capped.frame_limit_fps = 144;
    Check(hs::NextGraphicsSettingToken(capped, hs::SettingId::FrameLimitFps, -1, nullptr) == "120" &&
              hs::NextGraphicsSettingToken(capped, hs::SettingId::FrameLimitFps, 1, nullptr) == "165",
          "144 FPS has adjacent high-refresh choices");
    capped.frame_limit_fps = 138;
    Check(hs::NextGraphicsSettingToken(capped, hs::SettingId::FrameLimitFps, -1, nullptr) == "120" &&
              hs::NextGraphicsSettingToken(capped, hs::SettingId::FrameLimitFps, 1, nullptr) == "144",
          "custom saved frame caps retain their numeric position");
    for (const auto extent : {hs::GraphicsExtent{1920, 1080}, {2560, 1080}, {1280, 1024}})
    {
        const auto scale = std::min(extent.width / 1920.0f, extent.height / 1080.0f);
        const hs::Float2 pixels{extent.width * 0.5f + (1560 - 960) * scale,
                               extent.height * 0.5f + (270 - 540) * scale};
        const auto cursor = hs::ReferenceUiCursor(pixels, extent);
        Check(std::abs(cursor.x - 0.625f) < 0.0001f &&
                  std::abs(cursor.y - 0.5f) < 0.0001f,
              "current click coordinates match letterboxed UI on every aspect ratio");
    }
    hs::GraphicsSettingsUiModel clicks;
    auto click_ui = test_ui;
    click_ui.settings_group = 1;
    const auto click = hs::ResolveUiInteraction({}, click_ui, {},
        hs::ReferenceUiCursor({1000, 430}, {1920, 1080}), &clicks);
    Check(click.runtime_command && click.runtime_command->graphics_setting == hs::SettingId::Vsync &&
              std::string_view(click.runtime_command->graphics_token.data()) == "false",
          "clicking a setting value changes its canonical setting");
    hs::GraphicsSettingsUiModel receipt;
    receipt.choices = {
        {hs::SettingId::HdrMode, "off", "off", "", true},
        {hs::SettingId::HdrMode, "auto", "auto", "", true},
        {hs::SettingId::HdrMode, "on", "on", "HDR 미지원", false},
        {hs::SettingId::ReconstructionProvider, "auto", "auto", "", true},
        {hs::SettingId::ReconstructionProvider, "dlss", "dlss", "DLSS 미지원", false},
        {hs::SettingId::ReconstructionProvider, "internal_taau", "internal_taau", "", true},
        {hs::SettingId::FrameGenerationMode, "off", "off", "", true},
        {hs::SettingId::FrameGenerationMode, "auto", "auto", "프레임 생성 미지원", false}};
    Check(!hs::GraphicsSettingTokenAvailable(hs::SettingId::HdrMode, "on", &receipt) &&
              !hs::GraphicsSettingTokenAvailable(hs::SettingId::HdrMode, "on", nullptr) &&
              !hs::GraphicsSettingTokenAvailable(hs::SettingId::ReconstructionProvider, "dlss", &receipt),
          "unsupported features cannot be enabled before or after capability discovery");
    receipt.requested.hdr_mode = hs::GraphicsHdrMode::Off;
    receipt.requested.frame_generation_mode = hs::GraphicsFrameGenerationMode::Off;
    Check(hs::NextGraphicsSettingToken(receipt.requested, hs::SettingId::HdrMode, 1, &receipt) == "auto" &&
              hs::NextGraphicsSettingToken(receipt.requested, hs::SettingId::ReconstructionProvider, 1, &receipt) == "internal_taau" &&
              !hs::NextGraphicsSettingToken(receipt.requested, hs::SettingId::FrameGenerationMode, 1, &receipt),
          "settings skip unsupported choices and disable controls with no available alternative");
    for (std::uint8_t group = 0; group < 7; ++group)
        for (std::uint8_t page = 0; page < hs::GraphicsSettingsPageCount(group); ++page)
        {
            auto disabled_ui = test_ui;
            disabled_ui.settings_group = group;
            disabled_ui.settings_page = page;
            const auto rows = hs::GraphicsSettingsVisibleRows(disabled_ui);
            for (std::size_t row = 0; row < rows.size(); ++row)
                if (rows[row]->id == hs::SettingId::FrameGenerationMode)
                    for (const float x : {1000.0f, 1470.0f, 1570.0f})
                    {
                        const auto click = hs::ResolveUiInteraction({}, disabled_ui, {},
                            hs::ReferenceUiCursor({x, hs::kGraphicsUiRowY + row * hs::kGraphicsUiRowStride + 20},
                                                  {1920, 1080}), &receipt);
                        Check(!click.invalid && !click.runtime_command && !click.gameplay_action,
                              "disabled setting labels and arrows ignore clicks without a rejection cue");
                    }
        }
    receipt.requested.hdr_mode = hs::GraphicsHdrMode::On;
    Check(hs::NextGraphicsSettingToken(receipt.requested, hs::SettingId::HdrMode, 1, &receipt) == "off",
          "an unavailable saved setting can still be turned off");
    receipt.choices[2].available = true;
    receipt.choices[4].available = true;
    Check(hs::GraphicsSettingTokenAvailable(hs::SettingId::HdrMode, "on", &receipt) &&
              hs::NextGraphicsSettingToken(receipt.requested, hs::SettingId::ReconstructionProvider, 1, &receipt) == "dlss",
          "capability changes immediately restore supported choices");
    receipt.requested.preset = hs::GraphicsPreset::Ultra;
    Check(hs::NextGraphicsSettingToken(receipt.requested, hs::SettingId::Preset, 1, &receipt) == "custom",
          "preset controls cycle directly from Ultra to Custom");
    receipt.telemetry_status = {
        "Monitor fixture | Refresh 59.94 Hz | Monitor HDR active | Renderer HDR10",
        "Render 1280x720 / output 1920x1080 | Render FPS 61.5 | Display FPS unavailable",
        "DXGI process VRAM usage / budget 128.0 / 512.0 MiB | Observed transition usage peak unavailable"};
    for (const auto &descriptor : hs::GraphicsSettingDescriptors())
        receipt.resolved.push_back({descriptor.id, "fixture applied value", "Fixture native applied status"});
    hs::GameReadModelStorage model;
    hs::CopyReadModelForDevelopment(simulation, model);
    hs::RenderSnapshotStorage snapshot(16, 2, 2, hs::kGraphicsUiSnapshotReserve);
    auto ui = test_ui;
    ui.settings_group = 0;
    ui.settings_page = 0;
    std::size_t maximum_rows{};
    for (std::uint8_t group = 0; group < 7; ++group)
        for (std::uint8_t page = 0; page < hs::GraphicsSettingsPageCount(group); ++page)
        {
            auto candidate = ui;
            candidate.settings_group = group;
            candidate.settings_page = page;
            const auto count = hs::GraphicsSettingsVisibleRows(candidate).size();
            if (count > maximum_rows) { maximum_rows = count; ui = candidate; }
        }
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation, ui,
              hs::SettingsData{}, snapshot, 0xFF, nullptr, nullptr, &receipt),
          "settings telemetry fits the bounded production snapshot reserve");
    Check(snapshot.View().ui.size() <= hs::kGraphicsUiSnapshotReserve,
          "fully annotated settings rows and telemetry retain bounded capacity");
    for (const auto &expected : receipt.telemetry_status)
        Check(std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &entry) {
            const auto end = std::ranges::find(entry.utf8_text, '\0');
            return entry.kind == hs::UiModel::Kind::Text &&
                std::string_view(entry.utf8_text.data(), end - entry.utf8_text.begin()) == expected;
        }), "settings exposes native observations and unavailable values as read-only text");
    Check(std::ranges::none_of(snapshot.View().ui, [](const hs::UiModel &entry) {
        return entry.kind == hs::UiModel::Kind::Button &&
               entry.anchor_pixels.y >= 808.0f && entry.anchor_pixels.y < 900.0f;
    }), "settings telemetry adds no editable hit boxes");
    for (const float x : {1450.0f, 1550.0f})
        Check(std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &entry) {
            return entry.kind == hs::UiModel::Kind::Button && entry.anchor_pixels.x == x &&
                   entry.anchor_pixels.y == hs::kGraphicsUiRowY &&
                   entry.size_pixels.x == 64.0f && entry.size_pixels.y == 48.0f;
        }), "settings edit row keeps its original drawing and hit-test bounds");
    for (const float boundary : {0.0f, 1.0f})
    {
        hs::SettingsData bounded;
        bounded.master_volume = bounded.bgm_volume = bounded.sfx_volume = bounded.ui_volume =
            bounded.screen_overlay_intensity = boundary;
        auto bounded_ui = test_ui;
        bounded_ui.settings_group = 7;
        snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation, bounded_ui,
                  bounded, snapshot), "bounded audio and overlay settings snapshot");
        const float arrow_x = boundary == 0 ? 1450.0f : 1550.0f;
        for (std::size_t row = 0; row < 5; ++row)
        {
            const auto y = hs::kGraphicsUiRowY + row * hs::kGraphicsUiRowStride;
            Check(std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &entry) {
                return entry.kind == hs::UiModel::Kind::Text && entry.anchor_pixels.x == arrow_x &&
                       entry.anchor_pixels.y == y;
            }), "clamped audio and overlay arrows are visibly disabled");
            const auto click = hs::ResolveUiInteraction({}, bounded_ui, bounded,
                hs::ReferenceUiCursor({arrow_x + 20, y + 20}, {1920, 1080}));
            Check(!click.invalid && !click.runtime_command && !click.gameplay_action,
                  "clamped audio and overlay arrows emit no redundant apply or cue");
        }
    }
}
} // namespace

void TestCursorMovement()
{
    auto data = QuietGameData();
    data.arena_half_extent = 10.0f;

    hs::GameSimulation simulation;
    Check(simulation.Initialize({11}, data).Succeeded(), "movement initialize");

    hs::HeldInputState held;
    held.move_held = true;
    held.move_target_world = {10.0f, 0.0f, 0.0f};
    held.aim_world = {10.0f, 0.0f, 0.0f};
    (void)Tick(simulation, held);
    const auto click_position = simulation.GetObservation().player_position;

    held.move_target_world = {0.0f, 0.0f, 10.0f};
    (void)Tick(simulation, held);
    const auto updated_target_position = simulation.GetObservation().player_position;
    Check(updated_target_position.y > click_position.y,
          "held RMB continuously updates the destination");

    held.move_held = false;
    for (std::uint32_t tick = 0; tick < 30; ++tick)
    {
        (void)Tick(simulation, held);
    }
    const auto released = simulation.GetObservation().player_position;
    Check(released.y > updated_target_position.y + 2.0f,
          "one RMB click keeps moving after button release");

    held.move_target_world = {-10.0f, 0.0f, 0.0f};
    held.move_held = true;
    (void)Tick(simulation, held);
    held.move_held = false;
    for (std::uint32_t tick = 0; tick < 240; ++tick)
    {
        (void)Tick(simulation, held);
    }
    const auto negative_edge = simulation.GetObservation().player_position;
    Check(std::abs(negative_edge.x + 10.0f) < 0.0001f &&
              std::abs(negative_edge.y) < 0.0001f,
          "new RMB click replaces destination and reaches it");
    hs::GameReadModelStorage movement_model;
    hs::CopyReadModelForDevelopment(simulation, movement_model);
    Check(!movement_model.View().player.locomotion_moving &&
              movement_model.View().player.movement_stopped_tick != 0,
          "actual movement stop retains its presentation clock");
    hs::RenderSnapshotStorage movement_snapshot(4, 2, 2, 64);
    Check(WriteSnapshot(simulation, movement_snapshot), "stopped movement snapshot");
    const auto stop_clip = hs::MakeAssetId("character/archer/animation/RunForwardStop");
    Check(std::ranges::any_of(movement_snapshot.View().instances,
              [&](const hs::RenderInstance &instance) {
                  return instance.mesh == hs::RenderMesh::Archer &&
                         instance.animation_clip_id.value == stop_clip.value;
              }), "actual movement stop selects the authored non-looping clip");
    held.move_target_world = {0.0f, 0.0f, 0.0f};
    held.move_held = true;
    (void)Tick(simulation, held);
    movement_snapshot.Clear();
    Check(WriteSnapshot(simulation, movement_snapshot), "resumed movement snapshot");
    Check(std::ranges::none_of(movement_snapshot.View().instances,
              [&](const hs::RenderInstance &instance) {
                  return instance.mesh == hs::RenderMesh::Archer &&
                         instance.animation_clip_id.value == stop_clip.value;
              }), "resumed movement releases the stop clip");
    Check(simulation.Shutdown().Succeeded(), "movement shutdown");
}

void TestUiHitRegionsMatchAnchors()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({13, false, true}, QuietGameData()).Succeeded(),
          "UI hit initialize");
    hs::HeldInputState held;
    hs::Sequence sequence{};

    SetCursor(held, 960.0f, 466.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::MainMenu &&
              test_ui.page == hs::UiPage::Collection,
          "collection anchor opens collection instead of starting");

    const auto contains_text = [](const hs::RenderSnapshot &snapshot,
                                  std::string_view expected) {
        return std::ranges::any_of(snapshot.ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    hs::RenderSnapshotStorage snapshot(4, 2, 2, 64);
    Check(WriteSnapshot(simulation, snapshot), "collection snapshot");
    Check(contains_text(snapshot.View(), "스킬 도감") &&
              contains_text(snapshot.View(), "연속 추가 화살") &&
              contains_text(snapshot.View(), "액티브 연계 사격"),
          "collection shows all eight basic attack upgrades");

    SetCursor(held, 390.0f, 248.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "selected collection snapshot");
    Check(contains_text(snapshot.View(), "관통 사격") &&
              contains_text(snapshot.View(), "후속 화살") &&
              contains_text(snapshot.View(), "빠른 재사용") &&
              contains_text(snapshot.View(), "관통 사격을 발사한 직후"),
          "collection selection shows the skill description and all upgrades");

    SetCursor(held, 390.0f, 928.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(test_ui.page == hs::UiPage::Root, "page back anchor returns to main menu");

    SetCursor(held, 960.0f, 616.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(test_ui.page == hs::UiPage::MainMenuSettings, "settings anchor opens settings");
    CheckSettingsTelemetry(simulation);

    std::vector<hs::SettingId> visible_ids;
    for (std::uint8_t group = 0; group < 7; ++group)
        for (std::uint8_t page = 0; page < hs::GraphicsSettingsPageCount(group); ++page)
        {
            auto visible_ui = test_ui;
            visible_ui.settings_group = group;
            visible_ui.settings_page = page;
            const auto rows = hs::GraphicsSettingsVisibleRows(visible_ui);
            Check(rows.size() <= hs::kGraphicsUiRowsPerPage, "settings rows remain bounded");
            for (const auto *row : rows) visible_ids.push_back(row->id);
        }
    Check(visible_ids.size() == hs::kGraphicsSettingCount, "all graphics rows have a page");
    for (std::size_t row = 0; row < visible_ids.size(); ++row)
        Check(visible_ids[row] == hs::GraphicsSettingDescriptors()[row].id,
              "drawing and hit testing share canonical definition order");

    SetCursor(held, 450.0f, 195.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 1'470.0f, 430.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(last_ui_command && last_ui_command->kind == hs::UiCommandKind::SetGraphicsSetting &&
              last_ui_command->graphics_setting == hs::SettingId::Vsync &&
              std::string_view(last_ui_command->graphics_token.data()) == "false",
          "settings VSync button emits exact target value");

    SetCursor(held, 1'560.0f, 195.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 1'470.0f, 270.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(last_ui_command && last_ui_command->kind ==
              hs::UiCommandKind::SetMasterVolumePercent &&
              last_ui_command->value == 90,
          "settings volume control emits clamped percent");

    SetCursor(held, 1'100.0f, 550.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(last_ui_command && last_ui_command->kind ==
              hs::UiCommandKind::BeginSkillRebind &&
              last_ui_command->value == 0,
          "settings key button begins the selected slot rebind");

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(test_ui.page == hs::UiPage::Root, "Esc returns from settings");

    SetCursor(held, 960.0f, 316.0f);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing,
          "start anchor begins session");
    Check(simulation.Shutdown().Succeeded(), "UI hit shutdown");
}

void TestCharacterInformationPage()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({18}, QuietGameData()).Succeeded(),
          "character page initialize");
    for (std::uint32_t upgrade = 0; upgrade < hs::kUpgradeCount; ++upgrade)
        Debug(simulation, hs::DebugCommandKind::GrantUpgrade,
              static_cast<std::uint64_t>(hs::SkillKind::BasicAttack), upgrade);
    for (std::uint64_t relic = 0; relic < hs::kRelicCount; ++relic)
        Debug(simulation, hs::DebugCommandKind::GrantRelic, relic);
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::PiercingShot));
    Debug(simulation, hs::DebugCommandKind::GrantSkill,
          static_cast<std::uint64_t>(hs::SkillKind::MultiShot));

    hs::HeldInputState held;
    hs::Sequence sequence{};
    const auto before = simulation.GetObservation().tick;
    const auto opened = TickEdge(simulation, hs::GameAction::CharacterPage,
                                 hs::EdgeKind::Pressed, sequence, held);
    Check(opened.phase == hs::SessionPhase::Paused && opened.tick == before &&
              opened.world_changed &&
              test_ui.page == hs::UiPage::CharacterOverview,
          "Tab opens the character page and pauses simulation");

    const auto contains_text = [](const hs::RenderSnapshot &snapshot,
                                  std::string_view expected) {
        return std::ranges::any_of(snapshot.ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    Check(WriteSnapshot(simulation, snapshot), "character stats snapshot");
    Check(contains_text(snapshot.View(), "상세 능력치") &&
              contains_text(snapshot.View(), "기본 공격 피해") &&
              contains_text(snapshot.View(), "투자 포인트") &&
              contains_text(snapshot.View(), "현재 전투 기록"),
          "character overview shows effective stats and damage totals");

    SetCursor(held, 700, 165);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "character skills snapshot");
    Check(test_ui.page == hs::UiPage::CharacterSkills &&
              contains_text(snapshot.View(), "연속 추가 화살") &&
              contains_text(snapshot.View(), "기본 공격을 유지하면"),
          "skill page shows current upgrades and detailed skill explanation");
    const auto upgrade_cards = std::ranges::count_if(
        snapshot.View().ui, [](const hs::UiModel &model) {
            return model.kind == hs::UiModel::Kind::Button &&
                   model.color_rgba == 0xFF2D4058u;
        });
    const auto upgrades_fit = std::ranges::all_of(
        snapshot.View().ui, [](const hs::UiModel &model) {
            if (model.kind != hs::UiModel::Kind::Button ||
                model.color_rgba != 0xFF2D4058u)
                return true;
            return model.anchor_pixels.x >= 780.0f &&
                   model.anchor_pixels.x + model.size_pixels.x <= 1'560.0f &&
                   model.anchor_pixels.y + model.size_pixels.y <= 940.0f;
        });
    Check(upgrade_cards == hs::kCharacterUpgradesPerPage && upgrades_fit,
          "readable upgrade cards fit the character panel without moving slot controls");
    SetCursor(held, 1530, 968);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed, sequence, held);
    snapshot.Clear();
    Check(test_ui.character_details_page == 1 && WriteSnapshot(simulation, snapshot) &&
              contains_text(snapshot.View(), "강화 5 ·") && contains_text(snapshot.View(), "강화 8 ·") &&
              !contains_text(snapshot.View(), "강화 1 ·"),
          "next page exposes the remaining four upgrades");
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed, sequence, held);
    Check(test_ui.character_details_page == 1, "last upgrade page cannot advance");
    SetCursor(held, 1330, 968);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed, sequence, held);
    Check(test_ui.character_details_page == 0, "previous returns to the first upgrade page");
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed, sequence, held);
    Check(test_ui.character_details_page == 0, "first upgrade page cannot go back");
    SetCursor(held, 1530, 968);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed, sequence, held);
    SetCursor(held, 700, 165);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed, sequence, held);
    Check(test_ui.character_details_page == 0, "switching a character tab resets its detail page");
    const auto hidden = hs::ResolveUiInteraction(simulation.GetObservation(), test_ui, {},
        hs::ReferenceUiCursor({400, 494}, {1920, 1080}));
    Check(!hidden.invalid && !hidden.gameplay_action && !hidden.runtime_command,
          "unlearned skill rows have no invisible interactive target");
    const auto check_remapped_bindings = [&](bool hud, const std::array<std::string_view, 4> &labels) {
        hs::SettingsData remapped;
        remapped.skill_virtual_keys = {'Z', 'X', 0x71, '4'};
        hs::GameReadModelStorage model;
        hs::CopyReadModelForDevelopment(simulation, model);
        snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation, test_ui,
                  remapped, snapshot), "remapped skill labels snapshot");
        for (std::size_t slot = 0; slot < labels.size(); ++slot)
            Check(std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &entry) {
                return entry.kind == hs::UiModel::Kind::Button &&
                       entry.anchor_pixels.x == (hud ? 730.0f + slot * 190.0f : 780.0f + slot * 195.0f) &&
                       entry.anchor_pixels.y == (hud ? 948.0f : 225.0f) &&
                       std::string_view(entry.utf8_text.data()).starts_with(labels[slot]);
            }), "HUD and character slots use the current physical binding, including empty slots and function keys");
    };
    check_remapped_bindings(false, {"Z  관통 사격", "X  다중 사격", "F2  비어 있음", "4  비어 있음"});

    SetCursor(held, 850, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 1'250, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    auto reordered = simulation.GetObservation();
    Check(reordered.skill_loadout[0] == hs::SkillKind::Count &&
              reordered.skill_loadout[2] == hs::SkillKind::PiercingShot,
          "clicking an occupied then empty slot moves the skill");

    SetCursor(held, 1'050, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 1'250, 240);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    reordered = simulation.GetObservation();
    Check(reordered.skill_loadout[1] == hs::SkillKind::PiercingShot &&
              reordered.skill_loadout[2] == hs::SkillKind::MultiShot,
          "clicking two occupied slots swaps their QWER bindings");
    check_remapped_bindings(false, {"Z  비어 있음", "X  관통 사격", "F2  다중 사격", "4  비어 있음"});

    SetCursor(held, 1'000, 165);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "character relic snapshot");
    Check(test_ui.page == hs::UiPage::CharacterStats &&
               contains_text(snapshot.View(), "피의 회복") &&
               contains_text(snapshot.View(), "출혈 중인 적을 처치하면") &&
              contains_text(snapshot.View(), "발동 0 · 피해 0 · 킬 0") &&
               !contains_text(snapshot.View(), "유물 00001"),
           "relic page shows acquired relic effects and contribution metrics");
    const auto has_relic_text = [&](std::uint32_t color, float y) {
        return std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &model) {
            return model.kind == hs::UiModel::Kind::Text &&
                   model.color_rgba == color && model.anchor_pixels.x == 316.0f &&
                   model.anchor_pixels.y == y;
        });
    };
    Check(has_relic_text(0xFFFFD06Au, 247.0f) &&
              has_relic_text(0xFF9CC8FFu, 280.0f) &&
              has_relic_text(0xFFB8C2D0u, 404.0f),
          "relic cards separate name, contribution, and rule hierarchy");
    const auto relic_cards = std::ranges::count_if(
        snapshot.View().ui, [](const hs::UiModel &model) {
            return model.kind == hs::UiModel::Kind::Button &&
                   model.color_rgba == 0xFF2D4058u;
        });
    const auto relics_fit = std::ranges::all_of(
        snapshot.View().ui, [](const hs::UiModel &model) {
            if (model.kind != hs::UiModel::Kind::Button ||
                model.color_rgba != 0xFF2D4058u)
                return true;
            return model.anchor_pixels.x >= 300.0f &&
                   model.anchor_pixels.x + model.size_pixels.x <= 1'620.0f &&
                   model.anchor_pixels.y + model.size_pixels.y <= 935.0f;
        });
    Check(relic_cards == hs::kCharacterRelicsPerPage && relics_fit,
          "readable relic cards fit the character panel");
    for (std::size_t page = 0; page < hs::CharacterDetailsPageCount(simulation.GetObservation(), test_ui); ++page)
    {
        snapshot.Clear();
        Check(WriteSnapshot(simulation, snapshot), "paged relic snapshot");
        for (std::size_t relic = page * hs::kCharacterRelicsPerPage;
             relic < std::min(hs::kRelicCount, (page + 1) * hs::kCharacterRelicsPerPage); ++relic)
            Check(contains_text(snapshot.View(), DefaultContent().presentation.relic_rules[relic].data()),
                  "every acquired relic retains its complete authored rule on a reachable page");
        SetCursor(held, 1530, 968);
        (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed, sequence, held);
    }
    Check(test_ui.character_details_page == 4, "last relic page cannot advance");
    auto empty = simulation.GetObservation();
    empty.relic_mask = 0;
    (void)hs::ResolveUiInteraction(empty, test_ui, {}, hs::ReferenceUiCursor({300, 950}, {1920, 1080}));
    Check(test_ui.character_details_page == 0 && hs::CharacterDetailsPageCount(empty, test_ui) == 1,
          "empty or reduced detail content clamps a stale page safely");

    const auto closed = TickEdge(simulation, hs::GameAction::CharacterPage,
                                 hs::EdgeKind::Pressed, sequence, held);
    Check(closed.phase == hs::SessionPhase::Playing && test_ui.page == hs::UiPage::Root,
          "Tab closes the character page");
    check_remapped_bindings(true, {"Z\n-", "X\n관통 사격", "F2\n다중 사격", "4\n-"});
    held.aim_world = {20.0f, 0.0f, 0.0f};
    (void)TickEdge(simulation, hs::GameAction::SkillW, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().cooldown_ticks[0] > 0,
          "the swapped W binding immediately casts piercing shot");
    Check(simulation.Shutdown().Succeeded(), "character page shutdown");
}

void TestPauseMenuActions()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({19}, QuietGameData()).Succeeded(),
          "pause menu initialize");
    hs::HeldInputState held;
    hs::Sequence sequence{};
    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);

    hs::RenderSnapshotStorage snapshot(16, 2, 2, 32);
    Check(WriteSnapshot(simulation, snapshot), "pause menu snapshot");
    const auto contains_text = [&](std::string_view expected) {
        return std::ranges::any_of(snapshot.View().ui, [&](const hs::UiModel &model) {
            const auto end = std::ranges::find(model.utf8_text, '\0');
            return std::string_view(model.utf8_text.data(),
                                    end - model.utf8_text.begin()).contains(expected);
        });
    };
    Check(contains_text("계속") && contains_text("설정") &&
              contains_text("게임 종료"),
          "Esc menu shows continue, settings, and quit actions");

    SetCursor(held, 960, 556);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Paused &&
              test_ui.page == hs::UiPage::PauseSettings,
          "pause settings keeps the game paused");
    CheckSettingsTelemetry(simulation);
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "in-game settings snapshot");
    Check(contains_text("품질") && contains_text("화면") &&
              contains_text("돌아가기") && !contains_text("상세 능력치") &&
              !contains_text("캐릭터 정보") && !contains_text("스킬·강화"),
          "in-game settings does not render the character information page");

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Paused &&
              test_ui.page == hs::UiPage::Root,
          "Esc returns from settings to the pause menu");

    SetCursor(held, 960, 456);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::Playing,
          "continue resumes gameplay");

    (void)TickEdge(simulation, hs::GameAction::Pause, hs::EdgeKind::Pressed,
                   sequence, held);
    SetCursor(held, 960, 656);
    (void)TickEdge(simulation, hs::GameAction::BasicAttack, hs::EdgeKind::Pressed,
                   sequence, held);
    Check(simulation.GetObservation().phase == hs::SessionPhase::QuitRequested,
          "pause menu quit requests application exit");
    Check(simulation.Shutdown().Succeeded(), "pause menu shutdown");
}

void TestSkillUpgradeCardsHaveDescriptions()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({73}, QuietGameData()).Succeeded(),
          "upgrade description initialize");
    Debug(simulation, hs::DebugCommandKind::GrantExperience, 20);
    (void)Tick(simulation);
    const auto probe = simulation.GetObservation();
    Check(probe.phase == hs::SessionPhase::CardSelection,
          "upgrade description card selection");
    for (const auto phase : {hs::SessionPhase::CardSelection, hs::SessionPhase::RelicSelection})
    {
        auto bounded = probe;
        bounded.phase = phase;
        bounded.card_count = 1;
        bounded.level_rerolls_remaining = bounded.relic_rerolls_remaining = 0;
        hs::PresentationUiState ui;
        const auto reroll = hs::ResolveUiInteraction(bounded, ui, {},
            hs::ReferenceUiCursor({960, 826}, {1920, 1080}));
        Check(!reroll.invalid && !reroll.gameplay_action, "exhausted reroll emits no accepted-action cue");
        const auto empty_card = hs::ResolveUiInteraction(bounded, ui, {},
            hs::ReferenceUiCursor({960, 510}, {1920, 1080}));
        Check(!empty_card.invalid && !empty_card.gameplay_action, "empty card positions are not interactive");
        bounded.level_rerolls_remaining = bounded.relic_rerolls_remaining = 1;
        const auto available = hs::ResolveUiInteraction(bounded, ui, {},
            hs::ReferenceUiCursor({960, 826}, {1920, 1080}));
        Check(available.gameplay_action && available.gameplay_action->kind == hs::UiActionKind::Reroll,
              "available reroll keeps its existing action and hit rectangle");
    }

    hs::RenderSnapshotStorage snapshot(16, 2, 2, 128);
    hs::GameReadModelStorage exhausted;
    hs::CopyReadModelForDevelopment(simulation, exhausted);
    exhausted.session.level_rerolls_remaining = 0;
    Check(hs::ProjectRenderSnapshot(exhausted.View(), DefaultContent().presentation, {}, {}, snapshot),
          "exhausted reroll snapshot");
    Check(std::ranges::any_of(snapshot.View().ui, [](const hs::UiModel &entry) {
        return entry.kind == hs::UiModel::Kind::Text && entry.anchor_pixels.x == 760 &&
               entry.anchor_pixels.y == 790 && std::string_view(entry.utf8_text.data()) == "재추첨 0";
    }), "exhausted reroll is visibly disabled at its existing position");
    snapshot.Clear();
    Check(WriteSnapshot(simulation, snapshot), "upgrade description snapshot");
    bool checked_upgrade{};
    for (std::size_t card_index = 0; card_index < probe.card_count; ++card_index)
    {
        const auto card = probe.cards[card_index];
        if (card.kind != hs::CardKind::BasicUpgrade &&
            card.kind != hs::CardKind::SkillUpgrade)
            continue;
        checked_upgrade = true;
        const auto heading = std::format("강화 {}",
                                         static_cast<unsigned>(card.upgrade) + 1);
        const auto expected_x = 384.0f + static_cast<float>(card_index) * 420.0f;
        const auto has_heading = std::ranges::any_of(
            snapshot.View().ui, [&](const hs::UiModel &model) {
                const auto end = std::ranges::find(model.utf8_text, '\0');
                const std::string_view text(model.utf8_text.data(),
                                            end - model.utf8_text.begin());
                return model.kind == hs::UiModel::Kind::Text &&
                       model.color_rgba == 0xFF78B8FFu &&
                       model.anchor_pixels.x == expected_x && text.contains(heading);
            });
        const auto has_description = std::ranges::any_of(
            snapshot.View().ui, [&](const hs::UiModel &model) {
                const auto end = std::ranges::find(model.utf8_text, '\0');
                const std::string_view text(model.utf8_text.data(),
                                            end - model.utf8_text.begin());
                return model.kind == hs::UiModel::Kind::Text &&
                       model.color_rgba == 0xFFD7E0ECu &&
                       model.anchor_pixels.x == expected_x &&
                       model.anchor_pixels.y == 420.0f && text.size() > 30;
            });
        Check(has_heading && has_description,
              "skill upgrade card separates heading and readable description");
    }
    Check(checked_upgrade, "upgrade description test has an upgrade card");
    Check(simulation.Shutdown().Succeeded(), "upgrade description shutdown");
}

void TestBossAnimationReachesReleaseFrame()
{
    hs::GameReadModelStorage model_storage;
    hs::EnemyView enemy{};
    enemy.id = {1};
    enemy.boss = hs::BossKind::FiveMinute;
    enemy.max_health = enemy.health = 100;
    enemy.boss_action_started = 0;
    enemy.boss_action_until = 10;
    model_storage.AddEnemy(enemy);
    hs::BossActionView pending_action{};
    pending_action.boss_id = enemy.id.value;
    pending_action.animation_started = 0;
    pending_action.execute_tick = 10;
    model_storage.AddBossAction(pending_action);
    hs::RenderSnapshotStorage snapshot(8, 1, 1, 16);
    hs::SettingsData settings{};
    const auto project = [&](hs::Tick tick) {
        model_storage.tick = tick;
        snapshot.Clear();
        return hs::ProjectRenderSnapshot(model_storage.View(), DefaultContent().presentation,
                                         test_ui, settings, snapshot);
    };
    Check(project(9), "boss animation pre-release projection");
    Check(snapshot.View().poses.size() == 2 && snapshot.View().poses.back().normalized_time > 0.89f,
          "boss animation progresses monotonically before release");
    Check(project(10), "boss animation release projection");
    Check(snapshot.View().poses.size() == 2 && snapshot.View().poses.back().normalized_time == 1.0f,
          "boss animation reaches release frame");

    enemy.boss_action_started = enemy.boss_action_until = 0;
    const auto project_pending = [&](hs::Tick start, hs::Tick execute, hs::Tick tick) {
        model_storage.Clear();
        model_storage.AddEnemy(enemy);
        hs::BossActionView action{};
        action.boss_id = enemy.id.value;
        action.animation_started = start;
        action.execute_tick = execute;
        model_storage.AddBossAction(action);
        return project(tick);
    };
    Check(project_pending(0, 10, 9) && snapshot.View().poses.back().normalized_time > 0.89f,
          "first boss action approaches its release frame");
    Check(project_pending(10, 20, 10) && snapshot.View().poses.back().normalized_time == 0.0f,
          "chained boss action restarts its animation");
}

void TestMonsterVisualScalesAndAttackFacing()
{
    hs::GameReadModelStorage model_storage;
    const auto add_enemy = [&](hs::EntityId id, hs::EnemyKind kind) {
        hs::EnemyView enemy{};
        enemy.id = id;
        enemy.kind = kind;
        enemy.health = enemy.max_health = 100;
        model_storage.AddEnemy(enemy);
    };
    add_enemy({1}, hs::EnemyKind::Melee);
    add_enemy({2}, hs::EnemyKind::Ranged);
    add_enemy({3}, hs::EnemyKind::Suicide);

    hs::RenderSnapshotStorage snapshot(64, 4, 4, 16);
    hs::SettingsData settings{};
    Check(hs::ProjectRenderSnapshot(model_storage.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot),
          "monster scale projection");
    const auto instances = snapshot.View().instances;
    const auto check_scale = [&](hs::RenderMesh mesh, float expected) {
        const auto instance = std::ranges::find_if(instances, [mesh](const auto &candidate) {
            return candidate.mesh == mesh;
        });
        Check(instance != instances.end(), "monster mesh is projected");
        Check(instance->scale.x == expected && instance->scale.y == expected &&
                  instance->scale.z == expected,
              "monster visual scale is uniform and mesh-specific");
    };
    check_scale(hs::RenderMesh::MonsterMelee, 1.25f);
    check_scale(hs::RenderMesh::MonsterRanged, 1.40625f);
    check_scale(hs::RenderMesh::MonsterSuicide, 1.5625f);
    hs::GameReadModelStorage facing_model;
    hs::EnemyView ranged{};
    ranged.id = {4};
    ranged.kind = hs::EnemyKind::Ranged;
    ranged.health = ranged.max_health = 100;
    ranged.attacking = true;
    ranged.locked_aim = {1.0f, 0.0f};
    facing_model.AddEnemy(ranged);
    snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(facing_model.View(), DefaultContent().presentation,
                                    test_ui, settings, snapshot),
          "ranged attack facing projection");
    const auto projected_ranged = std::ranges::find_if(
        snapshot.View().instances, [](const auto &instance) {
            return instance.mesh == hs::RenderMesh::MonsterRanged;
        });
    Check(projected_ranged != snapshot.View().instances.end() &&
              std::abs(projected_ranged->yaw - std::numbers::pi_v<float> * 0.5f) < 0.001f,
          "stationary ranged enemy faces its locked attack direction");
}

void TestRelicTriggerProjection()
{
    constexpr std::array<std::string_view, 8> assets{
        "particle.relic.projectile_cadence_reward",
        "particle.relic.pre_damage_guard",
        "particle.relic.slow_synergy",
        "particle.relic.area_resonance",
        "particle.relic.boss_pressure",
        "particle.relic.hit_streak_reward",
        "particle.relic.pickup_reward",
        "particle.relic.low_health_survival"};
    for (std::size_t index = 0; index < assets.size(); ++index)
    {
        hs::DomainSignal signal;
        signal.kind = hs::DomainSignalKind::RelicTriggered;
        signal.context = static_cast<std::uint8_t>(
            static_cast<std::size_t>(hs::RelicKind::ProjectileCadenceReward) + index);
        std::array<hs::PresentationEvent, 1> output{};
        const auto count = hs::ProjectDomainSignal(signal, output);
        Check(count == 1 && output[0].kind == hs::PresentationKind::Vfx &&
                  output[0].asset.value == hs::MakeAssetId(assets[index]).value,
              "new relic activation projects its distinct VFX");
    }
}

void TestEnemyAnimationStateSignals()
{
    hs::GameReadModelStorage storage;
    hs::EnemyView enemy{};
    enemy.id = {7}; enemy.kind = hs::EnemyKind::Ranged;
    enemy.health = enemy.max_health = 100;
    storage.AddEnemy(enemy);
    hs::EnemyAnimationState state;
    state.Update(storage.View(), {});
    enemy.health = 90; storage.Clear(); storage.AddEnemy(enemy);
    storage.tick = 1; state.Update(storage.View(), {});
    Check(state.RecoilStart(7).has_value(), "nonattacking health drop starts recoil");
    enemy.attacking = true;
    storage.Clear(); storage.AddEnemy(enemy); storage.tick = 2;
    state.Update(storage.View(), {});
    Check(!state.RecoilStart(7).has_value(), "attack draw takes priority over hit recoil");
    hs::DomainSignal release{}; release.kind = hs::DomainSignalKind::RangedEnemyReleased;
    release.tick = 1; release.source_entity_id = 7; release.direction = {1, 0, 0};
    state.Update(storage.View(), std::span(&release, 1));
    Check(state.ReleaseTick(7) == 1, "ranged release keeps source entity id");
    hs::DomainSignal death{}; death.kind = hs::DomainSignalKind::EnemyDied;
    death.tick = 1; death.source_entity_id = 7; death.context = static_cast<std::uint8_t>(hs::EnemyKind::Ranged);
    state.Update(storage.View(), std::span(&death, 1));
    Check(state.DeathPoses().size() == 1, "normal death creates corpse pose");
    hs::EnemyAnimationState suicide_state;
    hs::GameReadModelStorage suicide_storage;
    suicide_storage.tick = 1;
    suicide_state.Update(suicide_storage.View(), {});
    Check(suicide_state.DeathPoses().empty(), "suicide explosion without death signal has no corpse");
    hs::EnemyView cancelling{};
    cancelling.id = {8}; cancelling.kind = hs::EnemyKind::Suicide;
    cancelling.health = cancelling.max_health = 100;
    cancelling.attacking = true; cancelling.attack_started = 2; cancelling.attack_resolve = 12;
    suicide_storage.Clear(); suicide_storage.AddEnemy(cancelling);
    suicide_state.Update(suicide_storage.View(), {});
    cancelling.attacking = false;
    suicide_storage.Clear(); suicide_storage.AddEnemy(cancelling); suicide_storage.tick = 2;
    suicide_state.Update(suicide_storage.View(), {});
    const auto cancel_sample = suicide_state.AnimationSample(8);
    Check(cancel_sample.cancel_progress.has_value() && *cancel_sample.cancel_progress == 0.0f,
          "zero-progress cancel stays engaged in the per-actor animation sample");
    storage.tick = 49; state.Update(storage.View(), {});
    Check(state.DeathPoses().empty(), "corpse expires after 48 ticks");
}

void TestPlayerFootstepTimeline()
{
    hs::GameReadModelStorage storage;
    storage.session_id = 23;
    hs::PlayerFootstepTimeline timeline;
    {
        const auto sample = storage.View();
        const auto view = hs::development::BorrowOwnedReadModel(sample);
        timeline.Reset(view);
    }
    for (hs::Tick tick = 1; tick <= 22; ++tick)
    {
        storage.tick = tick;
        storage.player.locomotion_blend = 0.0f;
        const auto sample = storage.View();
        const auto view = hs::development::BorrowOwnedReadModel(sample);
        Check(!timeline.Advance(view),
              "stopped secondary timeline advances without cue");
    }
    storage.tick = 23;
    storage.player.position = {2.0f, -3.0f};
    storage.player.locomotion_blend = 0.75f;
    {
        const auto sample = storage.View();
        const auto view = hs::development::BorrowOwnedReadModel(sample);
        const auto first = timeline.Advance(view);
        Check(first && first->tick == 23 && first->sequence == 23 && first->session_id == 23 &&
                  first->asset.value == hs::MakeAssetId("audio.player.footstep_run").value &&
                  first->position.x == 2.0f && first->position.z == -3.0f,
              "tick 22 stopped to tick 23 moving emits the current run sample");
        Check(!timeline.Advance(view),
              "same-tick retry does not duplicate a footstep");
    }

    std::vector<hs::PresentationEvent> gap_events;
    for (hs::Tick tick = 24; tick <= 70; ++tick)
    {
        storage.tick = tick;
        storage.player.position = {static_cast<float>(tick), 1.0f};
        storage.player.locomotion_blend = tick == 45 ? 0.25f : tick == 68 ? 0.75f : 0.0f;
        const auto sample = storage.View();
        const auto view = hs::development::BorrowOwnedReadModel(sample);
        if (auto event = timeline.Advance(view))
            gap_events.push_back(*event);
    }
    Check(gap_events.size() == 2 && gap_events[0].tick == 45 && gap_events[0].sequence == 45 &&
              gap_events[0].asset.value == hs::MakeAssetId("audio.player.footstep_walk").value &&
              gap_events[1].tick == 68 && gap_events[1].sequence == 68 &&
              gap_events[1].asset.value == hs::MakeAssetId("audio.player.footstep_run").value,
          "crossings remain on tick 45 and 68 across a 40-to-70 render-consumption gap");
    storage.session_id = 24;
    storage.tick = 71;
    storage.player.locomotion_blend = 1.0f;
    {
        const auto sample = storage.View();
        const auto view = hs::development::BorrowOwnedReadModel(sample);
        Check(!timeline.Advance(view),
              "new session resets the secondary timeline");
    }
    storage.tick = 72;
    {
        const auto sample = storage.View();
        const auto view = hs::development::BorrowOwnedReadModel(sample);
        Check(!timeline.Advance(view, 5),
              "instant motion epoch resets the secondary timeline");
    }
}

void TestPlayerInstantMotionSignal()
{
    hs::GameReadModelStorage storage;
    storage.session_id = 23;
    hs::EnemyAnimationState state;
    state.Update(storage.View(), {});
    hs::DomainSignal signal{};
    signal.kind = hs::DomainSignalKind::RetreatMoved;
    signal.sequence = 4;
    signal.tick = storage.tick = 1;
    signal.session_id = storage.session_id;
    state.Update(storage.View(), std::span(&signal, 1));
    Check(state.PlayerMotionEpoch() == 0, "ordinary retreat movement remains continuous");
    signal.kind = hs::DomainSignalKind::UpgradeVisual;
    signal.sequence = 5;
    signal.upgrade_skill = static_cast<std::uint8_t>(hs::SkillKind::RetreatShot);
    signal.upgrade_index = 7;
    signal.upgrade_stage = hs::UpgradeVisualStage::Spawn;
    signal.upgrade_owner_id = signal.geometry.source_id = 1ull << 60;
    signal.geometry.kind = hs::DomainSignalGeometryKind::Line;
    signal.geometry.start_tick = signal.geometry.end_tick = signal.tick;
    state.Update(storage.View(), std::span(&signal, 1));
    state.Update(storage.View(), std::span(&signal, 1));
    Check(state.PlayerMotionEpoch() == 5, "repeated same-tick signal keeps one movement epoch");
    storage.tick = 3;
    state.Update(storage.View(), {});
    Check(state.PlayerMotionEpoch() == 5, "skipped publications retain the last movement epoch");
    hs::RenderSnapshotStorage snapshot(30000, 2048, 8, 2048);
    const auto owned = storage.View();
    const auto read = hs::development::BorrowOwnedReadModel(owned);
    Check(hs::ProjectDynamicTick(read, snapshot,
              hs::ProcessSceneView(hs::BuildProcessSceneAtLaunch(read, {})), &state) &&
              snapshot.View().camera.target_motion_epoch == 5 &&
              snapshot.View().instances.front().motion_epoch == 5,
          "player and camera publish the same movement boundary");
    storage.session_id = 24;
    state.Update(storage.View(), {});
    Check(state.PlayerMotionEpoch() == 0, "new session clears movement history");
}

void TestSlimeAttackPoseTransitions()
{
    hs::GameReadModelStorage model;
    hs::EnemyAnimationState state;
    hs::EnemyView enemy{};
    enemy.id = {91}; enemy.kind = hs::EnemyKind::Suicide;
    enemy.health = enemy.max_health = 100;
    enemy.attacking = true; enemy.attack_started = 10; enemy.attack_resolve = 70;
    enemy.locked_aim = {1, 0};
    float projected_yaw{};
    const auto project = [&](hs::Tick tick, std::span<const hs::DomainSignal> signals = {}) {
        model.Clear(); model.tick = tick; model.AddEnemy(enemy);
        state.Update(model.View(), signals);
        hs::RenderSnapshotStorage snapshot(30000, 2048, 8, 2048);
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
              test_ui, hs::SettingsData{}, snapshot, 0xFF, &state), "slime state snapshot");
        projected_yaw = snapshot.View().instances[snapshot.View().poses.back().instance_index].yaw;
        if (state.CancelProgress(enemy.id.value))
        {
            const auto &pose = snapshot.View().poses.back();
            const auto &instance = snapshot.View().instances[pose.instance_index];
            Check(std::abs(instance.yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
                  "cancelled charge keeps its original attack facing");
        }
        return snapshot.View().poses.back();
    };
    const auto charging = project(39);
    enemy.attacking = false; enemy.velocity = {0, 1};
    const auto cancelled = project(40);
    Check(cancelled.clip == hs::CharacterAnimationClip::Draw &&
          std::abs(cancelled.normalized_time - 0.5f) < 0.0001f &&
          cancelled.normalized_time >= charging.normalized_time,
          "suicide cancellation starts at exact current charge progress");
    const auto reversing = project(58);
    Check(reversing.clip == hs::CharacterAnimationClip::Draw &&
          std::abs(reversing.normalized_time - 0.25f) < 0.0001f,
          "suicide cancellation reverses the original draw slowly");
    Check(project(76).clip == hs::CharacterAnimationClip::Run,
          "suicide cancellation returns to locomotion after 36 ticks");
    enemy.attacking = true; enemy.attack_started = 80; enemy.attack_resolve = 140;
    (void)project(90); enemy.attacking = false; (void)project(91);
    enemy.attacking = true; enemy.attack_started = 92; enemy.attack_resolve = 152;
    Check(project(92).normalized_time == 0.0f && !state.CancelProgress(91),
          "new attack clears the old reversed charge");
    enemy.kind = hs::EnemyKind::Ranged; enemy.attacking = false; enemy.velocity = {};
    hs::DomainSignal release{}; release.kind = hs::DomainSignalKind::RangedEnemyReleased;
    release.tick = 100; release.source_entity_id = 91; release.direction = {1, 0, 0};
    const auto spit = project(100, std::span(&release, 1));
    Check(spit.clip == hs::CharacterAnimationClip::Draw && spit.normalized_time == 1.0f,
          "ranged release preserves fully inflated draw endpoint");
    Check(project(101).clip == hs::CharacterAnimationClip::Recoil,
          "ranged release proceeds into spit recovery");
    Check(std::abs(projected_yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "stationary ranged spit retains release facing on the next tick");
    Check(project(118).clip == hs::CharacterAnimationClip::Recoil &&
              std::abs(projected_yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "ranged spit retains release facing through its final recovery tick");
    Check(project(119).clip == hs::CharacterAnimationClip::Idle &&
              !state.IsAttackRecoil(91) && std::abs(projected_yaw) < 0.0001f,
          "ranged facing lock expires with attack recovery");
    enemy.kind = hs::EnemyKind::Melee; enemy.attacking = true; enemy.velocity = {-1, 0};
    enemy.attack_started = 120; enemy.attack_resolve = 130;
    (void)project(129); enemy.attacking = false;
    Check(project(130).normalized_time == 1.0f,
          "melee resolve preserves headbutt endpoint");
    Check(project(131).clip == hs::CharacterAnimationClip::Recoil,
          "melee strike enters recovery after contact");
    Check(std::abs(projected_yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "melee headbutt recovery retains attack facing against navigation velocity");
    Check(project(148).clip == hs::CharacterAnimationClip::Recoil &&
              std::abs(projected_yaw - std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "melee headbutt retains facing through its final recovery tick");
    Check(project(149).clip == hs::CharacterAnimationClip::Run,
          "melee recovery expires after 18 ticks");
    Check(!state.IsAttackRecoil(91) && std::abs(projected_yaw + std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "melee resumes navigation facing after recovery");
    enemy.attacking = true; enemy.attack_started = 150; enemy.attack_resolve = 153;
    (void)project(152); enemy.attacking = false; (void)project(153); (void)project(154);
    enemy.health = 90;
    Check(project(155).clip == hs::CharacterAnimationClip::Recoil && !state.IsAttackRecoil(91) &&
              std::abs(projected_yaw + std::numbers::pi_v<float> * 0.5f) < 0.0001f,
          "damage recoil supersedes the attack facing lock");
    enemy.attacking = true; enemy.attack_started = 160; enemy.attack_resolve = 190;
    (void)project(170); enemy.attacking = false;
    Check(project(171).clip == hs::CharacterAnimationClip::Run,
          "interrupted melee windup does not invent a strike recovery");
}

void TestAuthoredEnvironmentProjection()
{
    auto rules = QuietGameData();
    hs::GameSimulation simulation;
    constexpr std::uint64_t seed = 0x1234567887654321ull;
    Check(simulation.Initialize({seed}, rules).Succeeded(), "environment initialize");
    hs::RenderSnapshotStorage snapshot(30000, 2048, 8, 2048);
    Check(WriteSnapshot(simulation, snapshot), "environment snapshot capacity");
    const auto &instances = snapshot.View().static_scene->instances;
    std::size_t trees{}, grass{}, ground{};
    for (std::size_t i = 0; i < instances.size(); ++i)
    {
        const auto &item = instances[i];
        Check(item.mesh != hs::RenderMesh::DirtPatch, "dirt path blends in terrain without coplanar rectangle overlays");
        if (item.mesh == hs::RenderMesh::Ground)
        {
            ++ground;
            Check(item.environment_seed == static_cast<std::uint32_t>(seed ^ (seed >> 32)),
                  "all continuous ground tiles share the map seed");
        }
        if (item.mesh == hs::RenderMesh::TreeTrunk)
        {
            ++trees;
            Check(i + 1 < instances.size(), "tree has a canopy");
            const auto &canopy = instances[i + 1];
            Check(canopy.mesh == hs::RenderMesh::TreeCanopy && canopy.environment_variant == item.environment_variant &&
                      item.environment_variant < 3, "trunk and canopy share one authored tree species");
            Check(canopy.position.x == item.position.x && canopy.position.y == item.position.y &&
                      canopy.position.z == item.position.z && canopy.yaw == item.yaw &&
                      canopy.scale.x == item.scale.x && canopy.scale.y == item.scale.y &&
                      canopy.scale.z == item.scale.z && item.scale.x == item.scale.y && item.scale.y == item.scale.z,
                  "authored tree parts share ground origin and uniform metre scale");
        }
        if (item.mesh == hs::RenderMesh::Grass)
        {
            ++grass;
            const bool meadow = hs::MeadowContains({item.position.x, item.position.z});
            Check(item.environment_variant < 4 && item.scale.x == item.scale.z &&
                      item.scale.x >= (meadow ? (hs::MeadowRoadCoverage({item.position.x, item.position.z}) > 0 ? .35f : .45f) : .55f) && item.scale.x <= 1.2f &&
                      item.scale.y >= (meadow ? .07f : (hs::MeadowRoadCoverage({item.position.x, item.position.z}, -.25f) > .02f ? .09f : .18f)) && item.scale.y <= .65f,
                  "grass retains low ground coverage and bounded long meadow clumps");
        }
    }
    Check(trees > 0 && grass > 0 && ground > 0, "authored environment is present in normal gameplay");
    Check(ground == 4, "exterior ground must leave the authored meadow interior uncovered");
    for (const auto &floor : instances) {
        if (floor.mesh != hs::RenderMesh::Ground) continue;
        Check(std::abs(floor.position.x) - floor.scale.x * .5f >= 12.0f - 1e-5f ||
              std::abs(floor.position.z) - floor.scale.z * .5f >= 12.0f - 1e-5f,
              "flat exterior slab overlaps a meadow depression");
        Check(floor.scale.x >= 52.0f || floor.scale.z >= 24.0f,
              "exterior slab no longer covers border trees");
    }
    for (std::uint32_t variant = 0; variant < 2; ++variant)
        Check(std::ranges::count_if(instances, [=](const auto &item) {
            return item.mesh == hs::RenderMesh::MeadowSurface && item.environment_variant == variant;
        }) == 1, "representative meadow needs exactly one ground and one water surface");
}

void TestDenseGrassProjection()
{
    hs::GameReadModelStorage model;
    model.seed = 0x1234567887654321ull;
    model.arena_half_extent = 16.0f;
    model.arena_boundary.count = 4;
    model.arena_boundary.points[0] = {-16, -16};
    model.arena_boundary.points[1] = {-16, 16};
    model.arena_boundary.points[2] = {16, 16};
    model.arena_boundary.points[3] = {16, -16};
    model.arena_obstacle_count = 1;
    model.arena_obstacles[0].center = {9, 0};
    model.arena_obstacles[0].radius = 2.0f;
    const auto project_grass = [&] {
        hs::RenderSnapshotStorage snapshot(30000, 16, 8, 2048);
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                       test_ui, hs::SettingsData{}, snapshot), "dense grass snapshot");
        std::vector<hs::RenderInstance> result;
        for (const auto &instance : snapshot.View().static_scene->instances)
            if (instance.mesh == hs::RenderMesh::Grass) result.push_back(instance);
        return result;
    };
    const auto grass = project_grass();
    const auto repeated = project_grass();
    Check(grass.size() > 600 && grass.size() == repeated.size(), "grass forms dense patches within allowed terrain");
    std::array<bool, 4> variants{};
    std::size_t grouped{}, central_grass{}, pressed_grass{}, tall_grass{}, low_grass{}, meadow_fill{}, long_fill{}, short_fill{}, dry_grass{};
    std::array<bool, 31> central_segments{};
    std::unordered_set<std::uint32_t> grass_colors;
    for (std::size_t i = 0; i < grass.size(); ++i)
    {
        const auto &item = grass[i];
        Check(item.position.x == repeated[i].position.x && item.position.z == repeated[i].position.z &&
                  item.yaw == repeated[i].yaw && item.scale.x == repeated[i].scale.x &&
                  item.scale.y == repeated[i].scale.y && item.color_rgba == repeated[i].color_rgba &&
                  item.environment_variant == repeated[i].environment_variant,
              "grass placement is deterministic for the map seed");
        const hs::Float2 position{item.position.x, item.position.z};
        const bool meadow = hs::MeadowContains(position);
        const auto across = std::abs(item.position.x - hs::MeadowCartCenter(item.position.z));
        Check(std::abs(item.position.x) <= 14 && std::abs(item.position.z) <= 14 &&
                  (hs::MeadowRoadCoverage(position) == 0.0f || across <= .35f) &&
                  (!meadow || (hs::MeadowPressed(position) <= .2f && !std::isfinite(hs::MeadowWaterLevel(position)))),
              "grass preserves arena inset, worn wheel/foot paths and actual water");
        if (meadow && hs::MeadowRoadCoverage(position, -.25f) == 0.0f && hs::MeadowPressed(position) <= .02f) {
            tall_grass += item.scale.y > .27f;
            low_grass += item.scale.y < .21f;
            grass_colors.insert(item.color_rgba);
        }
        if ((item.stable_id >> 56u) == 0x6Au) {
            ++meadow_fill;
            short_fill += item.scale.y <= .18f;
            long_fill += item.scale.y >= .5f;
            Check(meadow && item.scale.y <= .65f, "bounded meadow fill escaped its visible area or height contract");
        }
        dry_grass += item.color_rgba == 0xFF2869E0u;
        if (meadow && hs::MeadowRoadCoverage(position) > 0.0f) {
            Check(across <= .12f && item.scale.x == .35f && item.scale.y <= .11f,
                  "central weeds must leave both wheel tracks clear and stay below the path edge");
            central_segments[static_cast<std::size_t>((position.y + 12.0f) * 1.25f)] = true;
        }
        if (meadow && across < .4f) ++central_grass;
        if (meadow && hs::MeadowPressed(position) > .02f) {
            ++pressed_grass;
            Check(item.scale.y >= .07f && item.scale.y <= .11f,
                  "worn path edge must retain shorter pressed grass");
        }
        const auto dx = item.position.x - 9.0f;
        Check(dx * dx + item.position.z * item.position.z >= 2.45f * 2.45f,
              "dense grass preserves obstacle clearance");
        variants[item.environment_variant] = true;
        std::size_t neighbours{};
        for (std::size_t j = 0; j < grass.size(); ++j)
        {
            if (i == j) continue;
            const auto x = item.position.x - grass[j].position.x;
            const auto z = item.position.z - grass[j].position.z;
            if (x * x + z * z < 0.65f * 0.65f) ++neighbours;
        }
        if (neighbours >= 3) ++grouped;
    }
    Check(meadow_fill > 1000 && meadow_fill <= 3072 && short_fill > 800 && long_fill > 50 && dry_grass > 10,
          "visible meadow needs a bounded dense short floor, longer clumps and dry colonies");
    const auto covered_segments = std::ranges::count(central_segments, true);
    Check(covered_segments > 5 && covered_segments < 25, "central weed strip must retain visible breaks");
    Check(tall_grass > 20 && low_grass > 20 && grass_colors.size() >= 3,
          "meadow needs taller stands, low openings and broad foliage color variation");
    Check(pressed_grass > 0, "representative route omits all pressed-grass transitions");
    Check(central_grass > 0, "cart route must retain grass between its worn wheel tracks");
    Check(grouped * 4 > grass.size() * 3, "most grass has several nearby neighbours instead of isolated tufts");
    Check(std::ranges::all_of(variants, [](bool present) { return present; }), "all four grass variants populate patches");
    model.seed ^= 1ull << 40;
    const auto changed = project_grass();
    Check(!changed.empty() && (changed.front().position.x != grass.front().position.x ||
                              changed.front().position.z != grass.front().position.z),
          "high seed bits change grass placement");
}

void TestProjectileProjectionCarriesAuthoritativeOwner()
{
    hs::GameReadModelStorage storage;
    hs::ProjectileView projectile{};
    projectile.id = {42};
    projectile.player_owned = true;
    projectile.position = {8.0f, 2.0f};
    projectile.previous_position = {7.0f, 2.0f};
    projectile.velocity = {60.0f, 0.0f};
    projectile.spawned_tick = 10;
    projectile.skill = hs::SkillKind::ChargedShot;
    projectile.radius = 0.15f;
    projectile.cast_id = 99;
    storage.AddProjectile(projectile);
    // One actor may project several persistent outputs. Reservation is not a cap.
    hs::RenderSnapshotStorage snapshot(1, 1, 1, 16);
    Check(hs::ProjectRenderSnapshot(storage.View(), DefaultContent().presentation,
                                    test_ui, hs::SettingsData{}, snapshot),
          "projectile owner projection");
    const auto head = std::ranges::find_if(
        snapshot.View().persistent_vfx, [](const auto &visual) {
            return visual.kind == hs::PersistentVfxKind::ProjectileHead;
        });
    Check(head != snapshot.View().persistent_vfx.end() &&
              head->projectile_owner_id == 42 &&
              head->projectile_previous_position.x == 7.0f &&
              head->projectile_velocity.x == 60.0f &&
              head->projectile_spawned_tick == 10 &&
              head->effect_asset.value == hs::MakeAssetId("particle.skill.charged_shot").value,
          "projectile head dropped authoritative owner fields");
}

void TestMeadowPresentationGrounding()
{
    for (int step = -48; step <= 48; ++step) {
        const float along = float(step) * .25f;
        for (const hs::Float2 point : {hs::Float2{-12,along}, {12,along}, {along,-12}, {along,12}}) {
            Check(hs::MeadowGroundHeight(point) == 0.0f, "meadow perimeter must meet the flat outer slab");
            constexpr float e=.002f;
            const float dx=(hs::MeadowGroundHeight({point.x+e,point.y})-hs::MeadowGroundHeight({point.x-e,point.y}))/(2*e);
            const float dz=(hs::MeadowGroundHeight({point.x,point.y+e})-hs::MeadowGroundHeight({point.x,point.y-e}))/(2*e);
            Check(std::hypot(dx,dz)<1e-4f, "meadow edge slope must meet the horizontal slab normal");
        }
    }
    hs::Float2 low{};
    for (int z = -44; z <= 44; ++z)
        for (int x = -44; x <= 44; ++x) {
            const hs::Float2 point{float(x) * .25f, float(z) * .25f};
            if (hs::MeadowGroundHeight(point) < hs::MeadowGroundHeight(low)) low = point;
        }
    Check(hs::MeadowGroundHeight(low) < -.005f, "meadow grounding test requires a real depression");
    hs::DomainSignal signal{};
    signal.kind = hs::DomainSignalKind::ExperienceCollected;
    signal.position = {low.x, .3f, low.y};
    signal.target = {low.x + .5f, .55f, low.y};
    signal.flags = static_cast<std::uint8_t>(hs::DomainSignalFlag::HasTarget);
    for (const float extent : {0.0f, 8.0f, 16.0f}) {
        std::array<hs::PresentationEvent, 3> events{};
        const auto count = extent == 0 ? hs::ProjectDomainSignal(signal, events) :
            hs::ProjectDomainSignal(signal, events, extent);
        const auto h = extent >= 14 ? hs::MeadowGroundHeight(low) : 0.0f;
        const auto target_h = extent >= 14 ? hs::MeadowGroundHeight({signal.target.x, signal.target.z}) : 0.0f;
        const auto parameters = hs::DecodeVfxParameters(events[0].parameters);
        Check(count == 2 && std::abs(events[0].position.y - (.3f + h)) < 1e-6f &&
              events[1].position.y == events[0].position.y &&
              std::abs(parameters.target.y - (.55f + target_h)) < 1e-6f,
              "one-shot VFX/audio/target grounding must be explicit and independently sampled");
        auto line = signal;
        line.kind = hs::DomainSignalKind::RangedEnemyTelegraphed;
        line.geometry.kind = hs::DomainSignalGeometryKind::Line;
        line.geometry.end_position = signal.target;
        Check(hs::ProjectDomainSignal(line, events, extent) == 1 &&
              std::abs(events[0].geometry.end_position.y - (.55f + target_h)) < 1e-6f,
              "line endpoint grounding must match its own world location");
    }
    Check(signal.position.y == .3f && signal.target.y == .55f, "one-shot projection mutated its source signal");

    hs::GameReadModelStorage model;
    model.arena_half_extent = 16;
    model.arena_boundary.count = 4;
    model.arena_boundary.points[0] = {-16,-16}; model.arena_boundary.points[1] = {-16,16};
    model.arena_boundary.points[2] = {16,16}; model.arena_boundary.points[3] = {16,-16};
    model.player.position = low;
    hs::ProjectileView projectile{};
    projectile.id = {42}; projectile.player_owned = true; projectile.radius = .15f;
    projectile.position = low; projectile.previous_position = {low.x + .5f, low.y};
    model.AddProjectile(projectile);
    const auto project = [&] {
        hs::RenderSnapshotStorage snapshot(16, 4, 4, 16);
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
            test_ui, hs::SettingsData{}, snapshot), "meadow grounded projection");
        const auto view = snapshot.View();
        const auto actor = std::ranges::find_if(view.instances, [](const auto &v) { return v.mesh == hs::RenderMesh::Archer; });
        const auto head = std::ranges::find_if(view.persistent_vfx, [](const auto &v) { return v.kind == hs::PersistentVfxKind::ProjectileHead; });
        const auto height = model.arena_half_extent >= 14 ? hs::MeadowGroundHeight(low) : 0.0f;
        Check(actor != view.instances.end() && std::abs(actor->position.y - height) < 1e-6f &&
              std::abs(snapshot.camera.target.y - height) < 1e-6f, "actor/camera lost terrain contact or double-offset");
        Check(head != view.persistent_vfx.end() && std::abs(head->position.y - (1.05f + height)) < 1e-6f &&
              head->projectile_current_position.y == head->position.y &&
              std::abs(head->projectile_previous_position.y - (1.05f + (model.arena_half_extent >= 14 ?
                  hs::MeadowGroundHeight(projectile.previous_position) : 0.0f))) < 1e-6f,
              "projectile endpoints must independently follow their sampled ground heights");
        Check(model.player.position.x == low.x && model.player.position.y == low.y,
              "terrain presentation mutated 2D simulation coordinates");
        if (model.arena_half_extent >= 14) {
            std::size_t slabs=0;
            for (const auto &ground : view.static_scene->instances) {
                if (ground.mesh != hs::RenderMesh::Ground) continue;
                ++slabs;
                const float raster_top=ground.position.y + ground.scale.y * .5f + .5f * .1f * ground.scale.y;
                Check(std::abs(raster_top)<1e-7f, "outer ground raster surface must meet meadow at height zero");
            }
            Check(slabs==4, "meadow perimeter requires exactly four non-overlapping outer slabs");
            for (const auto &grass : view.static_scene->instances) {
                if (grass.mesh != hs::RenderMesh::Grass) continue;
                for (const auto &prop : view.static_scene->instances) {
                    if (prop.mesh != hs::RenderMesh::GroundDetail || prop.environment_variant < 7) continue;
                    constexpr std::array<float, 12> radii{1,1.3f,.75f,.75f,.65f,.65f,2,1.35f,1.4f,.5f,1.75f,1.05f};
                    const auto required = radii[prop.environment_variant - 7] * prop.scale.x + .45f;
                    Check(std::hypot(grass.position.x-prop.position.x, grass.position.z-prop.position.z) >= required - 1e-5f,
                          "grass intersects an authored prop footprint");
                }
            }
        }
    };
    project();
    model.arena_half_extent = 8;
    project();
}

void TestStaticEnvironmentSceneLease()
{
    hs::GameReadModelStorage model;
    model.seed = 19;
    model.session_id = 3;
    model.arena_half_extent = 16.0f;
    model.arena_boundary.count = 4;
    model.arena_boundary.points[0] = {-16, -16};
    model.arena_boundary.points[1] = {-16, 16};
    model.arena_boundary.points[2] = {16, 16};
    model.arena_boundary.points[3] = {16, -16};
    hs::runtime_detail::ProcessSceneFactory cache("test-generation");
    hs::RenderSnapshotStorage snapshot(4, 4, 2, 128);
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, hs::SettingsData{}, snapshot, 0xFF,
                                    nullptr, cache.Resolve(model.View())), "static scene projection");
    const auto first = snapshot.View().static_scene;
    Check(first && first->content_generation_id == "test-generation" &&
              first->instances.size() > 1 && snapshot.View().instances.size() == 1 &&
              snapshot.View().poses.size() == 1 &&
              snapshot.View().poses[0].instance_index == 0,
          "static scene lease preserves dynamic pose indices and content identity");
    std::array<bool, hs::kGroundDetailVariantCount> detail_variants{};
    std::unordered_set<std::uint64_t> identities;
    std::size_t details{}, pebbles{}, short_tufts{}, clover_links{}, stacked_crates{}, roadside_details{}, packing_tufts{};
    for (const auto &instance : first->instances)
    {
        Check(instance.stable_id && identities.insert(instance.stable_id).second,
              "appended ground details collide with existing placement provenance");
        if (instance.mesh == hs::RenderMesh::GroundDetail)
        {
            ++details;
            if ((instance.stable_id & 0xFFF00000ull) == 0x400000ull) {
                ++clover_links;
                Check(instance.environment_variant == 3u, "colony links must retain low clover geometry");
            }
            const auto detail_key = instance.stable_id & 0xFFFFFFull;
            if ((detail_key & 0xFFF00000ull) == 0x500000ull || (detail_key & 0xFFF00000ull) == 0x600000ull) {
                ++roadside_details;
                if (detail_key == 0x500012ull) {
                    ++stacked_crates;
                    const auto base = std::ranges::find(first->instances,
                        (instance.stable_id & ~0xFFFFFFull) | 0x500010ull, &hs::RenderInstance::stable_id);
                    Check(base != first->instances.end() && base->environment_variant == 9u &&
                              instance.environment_variant == 9u && instance.position.x == base->position.x &&
                              instance.position.z == base->position.z && instance.yaw == base->yaw &&
                              std::abs(instance.position.y - base->position.y - .658665597f) < 1e-6f,
                          "stacked crate must rest on the actual authored base height with aligned supports");
                    const auto member = std::ranges::find(first->eligible_static_members,
                        instance.stable_id, &hs::StaticRenderScene::EligibleStaticMember::stable_id);
                    Check(member != first->eligible_static_members.end() && member->object_id == base->stable_id,
                          "a crate stack must remain one HLOD object");
                } else {
                    Check(std::abs(instance.position.y - hs::MeadowGroundHeight({instance.position.x, instance.position.z})) < 1e-6f,
                          "roadside debris and props must retain terrain contact");
                }
            }
            Check(instance.environment_variant < detail_variants.size() &&
                  instance.asset_id.value && instance.material_id.value == hs::MakeAssetId("render/material/ground_detail.rpkg").value,
                  "detail geometry is not bound to the actual shared material product");
            detail_variants[instance.environment_variant] = true;
            const auto variant = instance.environment_variant;
            constexpr std::array<float, hs::kGroundDetailVariantCount> minimum_scale{1.2f, 1.2f, 1.6f, .8f, .8f, 1.2f, 1.35f, 1,1,1,1,1,1,1,1,1,1,1,1};
            constexpr std::array<float, hs::kGroundDetailVariantCount> maximum_scale{1.725f, 1.725f, 2.3f, 1.15f, 1.1f, 1.65f, 1.575f, 1,1,1,1,1,1,1,1,1,1,1,1};
            Check(instance.scale.x >= minimum_scale[variant] - 1e-5f &&
                  instance.scale.x <= maximum_scale[variant] + 1e-5f,
                  "ground-detail species lost its player-relative placement scale");
            constexpr std::array<float, hs::kGroundDetailVariantCount> footprint{.58f,.58f,.58f,.58f,.58f,.58f,1.05f,1,1.3f,.75f,.75f,.65f,.65f,2,1.35f,1.4f,.5f,1.75f,1.05f};
            const auto radius = footprint[variant] * instance.scale.x;
            Check((variant == 13u || hs::MeadowRoadCoverage({instance.position.x, instance.position.z}, -radius) == 0.0f) &&
                  instance.scale.x == instance.scale.y && instance.scale.y == instance.scale.z,
                  "scaled static detail enters the clear path or loses uniform transforms");
        }
        if (instance.mesh == hs::RenderMesh::Grass && (instance.stable_id & 0xFFFFFCull) == 0x500200ull) {
            ++packing_tufts;
            const hs::Float2 point{instance.position.x, instance.position.z};
            Check(instance.scale.y == .07f && instance.scale.x == instance.scale.z && instance.scale.x == 1.05f &&
                      instance.color_rgba == 0xFF1865F0u && hs::MeadowWetness(point) <= .05f &&
                      !std::isfinite(hs::MeadowWaterLevel(point)) && hs::MeadowRoadCoverage(point, -.55f) == 0.0f,
                  "packing litter must remain low, dry and clear of the road");
        }
        if (instance.mesh == hs::RenderMesh::Rock && instance.scale.x <= .1f) ++pebbles;
        if (instance.mesh == hs::RenderMesh::Grass && instance.scale.y == .18f) ++short_tufts;
    }
    Check(details > 0 && details <= 320 && pebbles > 0 && pebbles <= 64 && short_tufts > 0 && short_tufts <= 64,
          "new detail, small-rock, or short-grass population escaped its explicit budget");
    Check(stacked_crates == 1 && packing_tufts == 3 && roadside_details >= 14 && roadside_details <= 18,
          "bounded roadside stories need their supply stack and broken-edge debris");
    Check(clover_links > 0 && clover_links <= 16,
          "large colonies need bounded clover links instead of a uniform flower border");
    for (std::size_t variant = 0; variant < detail_variants.size(); ++variant)
        Check(detail_variants[variant], std::format("normal play scene omits ground-detail variant {}", variant));
    auto cook_model = model;
    cook_model.seed = 0;
    hs::runtime_detail::ProcessSceneFactory cook_cache("test-generation");
    const auto cook_scene = cook_cache.Resolve(cook_model.View());
    Check(cook_scene->eligible_layout_digest == first->eligible_layout_digest &&
          cook_scene->eligible_static_members.size() == first->eligible_static_members.size(),
          "seed-zero scene cooking differs from runtime detail/HLOD placement");
    snapshot.Clear();
    Check(!snapshot.View().static_scene && first->instances.size() > 1,
          "clearing a slot releases only its own scene lease");
    Check(cache.Resolve(model.View()) == first, "unchanged layout reuses static scene");
    model.arena_half_extent = 17.0f;
    const auto extent = cache.Resolve(model.View());
    Check(extent != first, "arena extent invalidates static scene");
    model.arena_boundary.points[0].x = -17.0f;
    const auto boundary = cache.Resolve(model.View());
    Check(boundary != extent, "boundary invalidates static scene");
    model.arena_obstacle_count = 1;
    model.arena_obstacles[0].center = {8.0f, 0.0f};
    model.arena_obstacles[0].radius = 1.0f;
    const auto obstacles = cache.Resolve(model.View());
    Check(obstacles != boundary, "obstacle list invalidates static scene");
    model.session_id = 4;
    Check(cache.Resolve(model.View()) == obstacles,
          "session restart preserves the process scene and its exact static identities");
    const auto prior_instances = obstacles->instances.data();
    const auto prior_members = obstacles->eligible_static_members.data();
    model.seed = 20;
    const auto reseeded = cache.Resolve(model.View());
    Check(reseeded != obstacles && obstacles->instances.data() == prior_instances &&
              obstacles->eligible_static_members.data() == prior_members,
          "new launch seed replaces only the owner while old packet spans stay leased");
    Check(!reseeded->static_keys.empty() && reseeded->static_keys.size() == reseeded->instances.size() &&
              std::ranges::is_sorted(reseeded->static_keys, {}, &hs::RenderSceneInstanceKey::stable_id),
          "static keys are frozen in the process owner before publication");
    for (const auto &instance : reseeded->instances)
        Check(instance.stable_id && instance.revisions == hs::DeriveRenderInstanceRevisions(instance),
              "static revisions must reflect actual seeded rows");
    static_assert(!std::is_move_constructible_v<hs::ProcessSceneLayout>);
    static_assert(!std::is_copy_constructible_v<hs::ProcessSceneLayout>);
}

void TestProjectileUpgradeAttachmentEpisodes()
{
    hs::GameReadModelStorage model;
    model.tick = 20;
    model.session_id = 3;
    hs::ProjectileView retarget{};
    retarget.id = {42};
    retarget.player_owned = true;
    retarget.skill = hs::SkillKind::MultiShot;
    retarget.origin = hs::EffectOrigin::Derived;
    retarget.source_upgrade = 7;
    retarget.homing = true;
    retarget.spawned_tick = 10;
    retarget.position = {3.0f, 4.0f};
    retarget.previous_position = {2.0f, 4.0f};
    retarget.velocity = {12.0f, 0.0f};
    retarget.radius = 0.18f;
    hs::ProjectileView ricochet{};
    ricochet.id = {43};
    ricochet.player_owned = true;
    ricochet.skill = hs::SkillKind::RicochetArrow;
    ricochet.spawned_tick = 5;
    ricochet.position = {-3.0f, 6.0f};
    ricochet.previous_position = {-4.0f, 6.0f};
    ricochet.velocity = {18.0f, 0.0f};
    ricochet.radius = 0.25f;
    ricochet.bleed_extend_ticks = {19, 20};
    const auto add_live = [&] {
        model.AddProjectile(retarget);
        model.AddProjectile(ricochet);
    };
    add_live();
    hs::RenderSnapshotStorage snapshot(16, 2, 2, 8);
    const auto project = [&] {
        snapshot.Clear();
        const auto owned = model.View();
        const auto borrowed = hs::development::BorrowOwnedReadModel(owned);
        if (owned.projectiles.size() > 1)
        {
            auto projectile=borrowed.projectiles.begin(); ++projectile;
            Check(projectile!=borrowed.projectiles.end()&&
                      projectile->bleed_extend_ticks.data()==owned.projectiles[1].bleed_extend_ticks.data(),
                  "projection borrows complete original episode history without copying or renumbering");
        }
        Check(hs::ProjectTick(borrowed, DefaultContent().presentation,
            test_ui, hs::SettingsData{}, snapshot,
            hs::ProcessSceneView(hs::BuildProcessSceneAtLaunch(borrowed, {}))), "projectile upgrade owner snapshot");
    };
    const auto find = [&](hs::PersistentVfxKind kind, hs::Tick started)
        -> const hs::PersistentVfxVisual * {
        const auto owners = snapshot.View().persistent_vfx;
        const auto found = std::ranges::find_if(owners,
            [=](const auto &visual) {
                return visual.kind == kind && visual.active_tick == started;
            });
        return found == owners.end() ? nullptr : &*found;
    };
    project();
    const auto *lock = find(hs::PersistentVfxKind::MultishotRetarget, 10);
    const auto *first = find(hs::PersistentVfxKind::RicochetBleedExtend, 19);
    const auto *second = find(hs::PersistentVfxKind::RicochetBleedExtend, 20);
    Check(lock && lock->effect_asset.value == hs::MakeAssetId("particle.upgrade.retarget").value &&
        lock->skill == static_cast<std::uint8_t>(hs::SkillKind::MultiShot) &&
        lock->source_upgrade == 7 && lock->expires == 28 &&
        lock->projectile_owner_id == 42 && lock->projectile_spawned_tick == 10 &&
        lock->projectile_previous_position.x == 2.0f &&
        lock->projectile_current_position.x == 3.0f &&
        lock->projectile_hitbox_radius == retarget.radius,
        "retarget child lost its real path and fixed episode clock");
    Check(first && second && first->stable_id != second->stable_id &&
        first->effect_asset.value == hs::MakeAssetId("particle.upgrade.ricochet.bleed_extend").value &&
        first->skill == static_cast<std::uint8_t>(hs::SkillKind::RicochetArrow) &&
        first->source_upgrade == 2 && first->expires == 40 && second->expires == 41 &&
        first->projectile_owner_id == 43 && first->entity_render_id == ((3ull << 60) | 43) &&
        first->radius == ricochet.radius && first->entity_health_fraction == 1.0f,
        "overlapping ricochet procs lost separate projectile attachment episodes");
    const auto lock_id = lock->stable_id;
    const auto first_id = first->stable_id;
    const auto second_id = second->stable_id;
    model.Clear();
    model.tick = 21;
    retarget.position = {5.0f, 4.0f};
    ricochet.position = {-1.0f, 6.0f};
    add_live();
    project();
    lock = find(hs::PersistentVfxKind::MultishotRetarget, 10);
    first = find(hs::PersistentVfxKind::RicochetBleedExtend, 19);
    second = find(hs::PersistentVfxKind::RicochetBleedExtend, 20);
    Check(lock && first && second && lock->stable_id == lock_id &&
        first->stable_id == first_id && second->stable_id == second_id &&
        lock->position.x == 5.0f && first->position.x == -1.0f &&
        second->position.x == -1.0f,
        "moving projectiles restarted or detached upgrade owners");
    model.session_id = 4;
    project();
    Check(find(hs::PersistentVfxKind::MultishotRetarget, 10)->stable_id != lock_id &&
        find(hs::PersistentVfxKind::RicochetBleedExtend, 19)->stable_id != first_id,
        "new session reused projectile upgrade owner identity");
    model.tick = 28;
    project();
    Check(!find(hs::PersistentVfxKind::MultishotRetarget, 10) &&
        find(hs::PersistentVfxKind::RicochetBleedExtend, 19) &&
        find(hs::PersistentVfxKind::RicochetBleedExtend, 20),
        "retarget owner outlived its 18 ticks or truncated ricochet episodes");
    model.tick = 40;
    project();
    Check(!find(hs::PersistentVfxKind::RicochetBleedExtend, 19) &&
        find(hs::PersistentVfxKind::RicochetBleedExtend, 20),
        "ricochet proc owners do not expire independently after 21 ticks");
    model.Clear();
    model.tick = 21;
    project();
    Check(std::ranges::none_of(snapshot.View().persistent_vfx, [](const auto &visual) {
        return visual.kind == hs::PersistentVfxKind::MultishotRetarget ||
               visual.kind == hs::PersistentVfxKind::RicochetBleedExtend;
    }), "dead projectile retained an upgrade attachment");
    retarget.homing = false;
    model.AddProjectile(retarget);
    project();
    Check(!find(hs::PersistentVfxKind::MultishotRetarget, 10),
        "retarget owner appeared without the successful homing child");
}

void TestPresentationGeometryTransport()
{
    hs::DomainSignal signal{};
    signal.kind = hs::DomainSignalKind::FireAreaPulse;
    signal.sequence = 77;
    signal.position = {2.0f, 0.12f, -3.0f};
    signal.geometry.kind = hs::DomainSignalGeometryKind::Circle;
    signal.geometry.radius = 4.5f;
    signal.geometry.start_tick = 10;
    signal.geometry.end_tick = 70;
    signal.geometry.source_id = 9001;
    std::array<hs::PresentationEvent, 3> output{};
    output[0].geometry.kind = hs::PresentationGeometryKind::Cone;
    const auto count = hs::ProjectDomainSignal(signal, output);
    Check(count >= 1 && output[0].geometry.kind == hs::PresentationGeometryKind::Circle &&
              output[0].geometry.radius == 4.5f &&
              output[0].geometry.start_tick == 10 &&
              output[0].geometry.end_tick == 70 &&
              output[0].geometry.source_id == 9001,
          "presentation projection dropped gameplay geometry");
    signal.kind = hs::DomainSignalKind::TrapDamaged;
    signal.geometry.radius = 1.8f;
    const auto damaged_count = hs::ProjectDomainSignal(signal, output);
    Check(damaged_count == 2 &&
              output[0].asset.value == hs::MakeAssetId("particle.skill.trap.damaged").value &&
              output[0].geometry.kind == hs::PresentationGeometryKind::Circle &&
              output[0].geometry.radius == 1.8f &&
              output[1].kind == hs::PresentationKind::Audio,
          "trap damage visual lost its real area radius or audio");
}

void TestProjectileImpactGeometryTransport()
{
    hs::DomainSignal signal{};
    signal.kind = hs::DomainSignalKind::EnemyRangedProjectileImpact;
    signal.sequence = 712;
    signal.position = {4.0f, 0.45f, -2.0f};
    signal.geometry.kind = hs::DomainSignalGeometryKind::Projectile;
    signal.geometry.velocity = {0.0f, 0.0f, 24.0f};
    signal.geometry.radius = 0.18f;
    signal.geometry.source_id = 712;
    std::array<hs::PresentationEvent, 2> output{};
    const auto count = hs::ProjectDomainSignal(signal, output);
    Check(count == 1 && output[0].kind == hs::PresentationKind::Vfx &&
              output[0].asset.value == hs::MakeAssetId("particle.enemy.ranged.impact").value &&
              output[0].position.y == 0.45f &&
              output[0].geometry.kind == hs::PresentationGeometryKind::Projectile &&
              output[0].geometry.velocity.z == 24.0f &&
              output[0].geometry.radius == 0.18f &&
              output[0].geometry.source_id == 712,
          "projectile impact projection dropped authoritative contact geometry");
}

void TestBossTelegraphProjectsGeometryAndAudio()
{
    hs::DomainSignal ranged{};
    ranged.kind = hs::DomainSignalKind::RangedEnemyTelegraphed;
    ranged.sequence = 711;
    ranged.geometry.kind = hs::DomainSignalGeometryKind::Line;
    ranged.geometry.width = 0.74f; ranged.geometry.range = 31.0f;
    ranged.geometry.source_id = 711; ranged.geometry.start_tick = 20; ranged.geometry.end_tick = 67;
    ranged.geometry.end_position = {-21.0f, 0.025f, 0.0f};
    std::array<hs::PresentationEvent, 3> ranged_output{};
    const auto ranged_count = hs::ProjectDomainSignal(ranged, ranged_output);
    Check(ranged_count == 1 && ranged_output[0].kind == hs::PresentationKind::Vfx &&
          ranged_output[0].asset.value == hs::MakeAssetId("particle.enemy.ranged.telegraph_line").value &&
          ranged_output[0].geometry.kind == hs::PresentationGeometryKind::Line &&
          ranged_output[0].geometry.source_id == 711 && ranged_output[0].geometry.end_tick == 67 &&
          ranged_output[0].geometry.width == 0.74f && ranged_output[0].geometry.end_position.x == -21.0f,
          "ranged warning projects exact line geometry without an added audio cue");

    hs::DomainSignal shockwave{};
    shockwave.kind = hs::DomainSignalKind::BossShockwaveTelegraphed;
    shockwave.geometry.kind = hs::DomainSignalGeometryKind::RingGaps;
    shockwave.geometry.inner_radius = 1.7f; shockwave.geometry.outer_radius = 13.0f;
    shockwave.geometry.gap_count = 3; shockwave.geometry.gap_half_width_degrees = 20;
    shockwave.geometry.source_id = 831; shockwave.geometry.start_tick = 10; shockwave.geometry.end_tick = 83;
    std::array<hs::PresentationEvent, 3> shockwave_output{};
    const auto shockwave_count = hs::ProjectDomainSignal(shockwave, shockwave_output);
    Check(shockwave_count == 2 && std::ranges::any_of(shockwave_output, [](const auto &e) {
        return e.kind == hs::PresentationKind::Vfx && e.asset.value == hs::MakeAssetId("particle.boss.shockwave.telegraph").value &&
            e.geometry.kind == hs::PresentationGeometryKind::RingGaps && e.geometry.inner_radius == 1.7f &&
            e.geometry.outer_radius == 13.0f && e.geometry.source_id == 831;
    }), "shockwave telegraph preserves ring geometry alongside its existing audio cue");
    const auto verify = [](hs::DomainSignalKind kind, hs::DomainSignalGeometry geometry,
                           std::string_view vfx_asset, std::string_view audio_asset) {
        hs::DomainSignal signal{};
        signal.kind = kind;
        signal.sequence = 803;
        signal.position = {2.0f, 0.025f, -3.0f};
        signal.direction = {0.6f, 0.0f, 0.8f};
        signal.geometry = geometry;
        std::array<hs::PresentationEvent, 4> output{};
        const auto count = hs::ProjectDomainSignal(signal, output);
        const auto vfx = std::ranges::find_if(
            output.begin(), output.begin() + count,
            [vfx_asset](const hs::PresentationEvent &event) {
                return event.kind == hs::PresentationKind::Vfx &&
                       event.asset.value == hs::MakeAssetId(vfx_asset).value;
            });
        const auto audio = std::ranges::find_if(
            output.begin(), output.begin() + count,
            [audio_asset](const hs::PresentationEvent &event) {
                return event.kind == hs::PresentationKind::Audio &&
                       event.asset.value == hs::MakeAssetId(audio_asset).value;
            });
        Check(vfx != output.begin() + count && audio != output.begin() + count &&
                  vfx->geometry.kind ==
                      (geometry.kind == hs::DomainSignalGeometryKind::Line
                           ? hs::PresentationGeometryKind::Line
                           : hs::PresentationGeometryKind::Cone) &&
                  vfx->geometry.width == geometry.width &&
                  vfx->geometry.range == geometry.range &&
                  vfx->geometry.end_position.x == geometry.end_position.x &&
                  vfx->geometry.end_position.z == geometry.end_position.z,
              "boss telegraph keeps geometry on VFX while retaining its audio cue");
    };

    hs::DomainSignalGeometry dash{};
    dash.kind = hs::DomainSignalGeometryKind::Line;
    dash.width = 3.25f;
    dash.range = 9.5f;
    dash.start_tick = 20;
    dash.end_tick = 55;
    dash.source_id = 12;
    dash.end_position = {11.5f, 0.025f, -3.0f};
    verify(hs::DomainSignalKind::BossDashTelegraphed, dash,
           "particle.boss.dash.telegraph", "audio.boss.dash.telegraph");

    hs::DomainSignalGeometry volley{};
    volley.kind = hs::DomainSignalGeometryKind::Cone;
    volley.range = 31.0f;
    volley.half_angle_degrees = 17.5f;
    volley.start_tick = 30;
    volley.end_tick = 80;
    volley.source_id = 13;
    verify(hs::DomainSignalKind::BossVolleyTelegraphed, volley,
           "particle.boss.volley.telegraph", "audio.boss.volley.telegraph");
}

void TestArrowReleaseProjectsVfxAndAudio()
{
    hs::DomainSignal signal{};
    signal.kind = hs::DomainSignalKind::ArrowReleased;
    signal.sequence = 12;
    signal.position = {1.0f, 1.05f, 2.0f};
    signal.direction = {0.0f, 0.0f, 1.0f};
    signal.context = static_cast<std::uint8_t>(hs::SkillKind::BasicAttack);
    std::array<hs::PresentationEvent, 3> output{};
    const auto count = hs::ProjectDomainSignal(signal, output);
    const auto parameters = hs::DecodeVfxParameters(output[0].parameters);
    Check(count == 3 && output[0].kind == hs::PresentationKind::Vfx &&
              output[0].asset.value == hs::MakeAssetId("particle.player.arrow_release").value &&
              parameters.direction.z == 1.0f && output[0].position.y == 1.05f &&
              output[1].kind == hs::PresentationKind::Audio &&
              output[2].kind == hs::PresentationKind::Audio,
          "arrow release projects directional VFX without changing its two audio cues");
}

void TestSessionIdentityAcrossPresentationReset()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({91, false, true}, QuietGameData()).Succeeded(),
          "session identity initialize");

    hs::GameReadModelStorage model;
    hs::RenderSnapshotStorage snapshot(8, 1, 1, 8);
    std::array<hs::PresentationEvent, 1> events{};

    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::StartSession}).Succeeded(),
          "first session starts");
    hs::CopyReadModelForDevelopment(simulation, model);
    const auto first_id = model.View().session_id;
    Check(first_id != 0, "first session receives a nonzero identity");
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, hs::SettingsData{}, snapshot),
          "first session snapshot projection");
    Check(snapshot.View().header.session_id == first_id,
          "snapshot keeps first session identity");

    hs::DomainSignal signal{};
    signal.session_id = first_id;
    signal.kind = hs::DomainSignalKind::BasicAttackImpact;
    Check(hs::ProjectDomainSignal(signal, events) == 1 &&
              events[0].session_id == first_id,
          "presentation event keeps first session identity");

    Check(simulation.ApplyDebugCommand({hs::DebugCommandKind::StartSession}).Succeeded(),
          "second session starts");
    hs::CopyReadModelForDevelopment(simulation, model);
    const auto second_id = model.View().session_id;
    Check(second_id > first_id, "session identity increases across reset");
    snapshot.Clear();
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
                                    test_ui, hs::SettingsData{}, snapshot),
          "second session snapshot projection");
    Check(snapshot.View().header.session_id == second_id,
          "snapshot keeps second session identity");
    signal.session_id = second_id;
    Check(hs::ProjectDomainSignal(signal, events) == 1 &&
              events[0].session_id == second_id,
          "presentation event keeps second session identity");
    Check(simulation.Shutdown().Succeeded(), "session identity shutdown");
}

void TestVisualLinkSnapshotTransport()
{
    hs::GameReadModelStorage model;model.tick=105;
    const hs::VisualLinkView ricochet{91,hs::VisualLinkKind::Ricochet,{1,.4f,2},{7,.8f,-4},.09f,100,108};
    const hs::VisualLinkView burn{92,hs::VisualLinkKind::BurnTransfer,{-3,.4f,9},{-2,.4f,5},.12f,100,111};
    const hs::VisualLinkView relic{93,hs::VisualLinkKind::RelicChain,{4,.4f,2},{8,.4f,3},.10f,100,109};
    model.AddVisualLink(ricochet);model.AddVisualLink(burn);model.AddVisualLink(relic);
    model.AddVisualLink({94,hs::VisualLinkKind::Ricochet,{},{1,0,1},.09f,105,105});
    model.AddVisualLink({95,hs::VisualLinkKind::Ricochet,{},{1,0,1},.09f,106,114});
    hs::RenderSnapshotStorage snapshot(8, 1, 1, 16);
    Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot),"link snapshot projection");
    for(const auto &link:std::array{ricochet,burn,relic})
    {
        const auto found=std::ranges::find_if(snapshot.View().persistent_vfx,[&](const auto &visual){return visual.stable_id==link.owner_id;});
        const auto expected=link.kind==hs::VisualLinkKind::Ricochet?hs::PersistentVfxKind::RicochetLink:
            link.kind==hs::VisualLinkKind::BurnTransfer?hs::PersistentVfxKind::BurnTransferLink:hs::PersistentVfxKind::RelicChainLink;
        Check(found!=snapshot.View().persistent_vfx.end() && found->kind==expected && found->position.x==link.source_position.x &&
                  found->position.y==link.source_position.y && found->position.z==link.source_position.z &&
                  found->link_target_position.x==link.target_position.x && found->link_target_position.y==link.target_position.y &&
                  found->link_target_position.z==link.target_position.z && found->link_width==link.width &&
                  found->active_tick==link.started && found->expires==link.expires,"snapshot preserves fixed link geometry and owner clocks");
    }
    Check(std::ranges::none_of(snapshot.View().persistent_vfx,[](const auto &visual){return visual.stable_id==94 || visual.stable_id==95;}),
          "expired and future links are not published");
    model.Clear();Check(model.View().visual_links.empty(),"read model Clear removes prior links");
}

void TestUpgradeActivationProjection()
{
    hs::DomainSignal signal;signal.kind=hs::DomainSignalKind::UpgradeVisual;signal.sequence=91;signal.tick=100;signal.session_id=3;
    signal.position={4,.15f,-7};signal.upgrade_skill=static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow);
    signal.upgrade_index=1;signal.upgrade_stage=hs::UpgradeVisualStage::Telegraph;signal.upgrade_cast_id=19;signal.upgrade_owner_id=71;
    signal.geometry.kind=hs::DomainSignalGeometryKind::Circle;signal.geometry.radius=2.25f;
    signal.geometry.source_id=71;signal.geometry.start_tick=100;signal.geometry.end_tick=160;
    std::array<hs::PresentationEvent,4> events{};
    Check(hs::ProjectDomainSignal(signal,events)==1 && events[0].kind==hs::PresentationKind::Vfx &&
        events[0].upgrade_skill==static_cast<std::uint8_t>(hs::SkillKind::ExplosiveArrow)&&events[0].upgrade_index==1&&events[0].upgrade_stage==2&&
        events[0].upgrade_cast_id==19&&events[0].upgrade_owner_id==71&&events[0].geometry.end_tick==160&&events[0].geometry.radius==2.25f,
        "upgrade activation preserves stage and authoritative schedule geometry");
    signal.kind=hs::DomainSignalKind::SmallExplosion;signal.upgrade_stage=hs::UpgradeVisualStage::Resolve;
    for(auto &event:events){event.upgrade_stage=99;event.upgrade_skill=4;event.upgrade_index=1;event.upgrade_cast_id=19;event.upgrade_owner_id=71;}
    Check(hs::ProjectDomainSignal(signal,events)==2&&events[0].upgrade_stage==3&&
        events[1].kind==hs::PresentationKind::Audio&&events[1].upgrade_stage==0&&events[1].upgrade_skill==0xFF&&events[1].upgrade_owner_id==0,
        "resolve projects one annotated generic visual and unannotated existing audio");
    hs::GameReadModelStorage model;model.tick=120;model.AddMiniBomb({71,19,{4,-7},2.25f,100,160});
    model.AddMiniBomb({72,19,{1,1},2,100,120});
    hs::RenderSnapshotStorage snapshot(8,1,1,16);
    Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot),"mini bomb snapshot projection");
    const auto warning=std::ranges::find_if(snapshot.View().persistent_vfx,[](const auto &v){return v.kind==hs::PersistentVfxKind::MiniBombWarning;});
    Check(warning!=snapshot.View().persistent_vfx.end()&&warning->stable_id==71&&warning->cast_id==19&&warning->position.x==4&&
        warning->position.z==-7&&warning->radius==2.25f&&warning->active_tick==100&&warning->expires==160&&warning->source_upgrade==1,
        "mini bomb owner clocks and actual footprint lost in projection");
    Check(std::ranges::count_if(snapshot.View().persistent_vfx,[](const auto &v){return v.kind==hs::PersistentVfxKind::MiniBombWarning;})==1,
        "expired mini bomb warning was published");
    model.Clear();Check(model.View().mini_bombs.empty(),"read model Clear retains mini bombs");
}

void TestCircleWarningProjection()
{
    const std::array kinds{hs::DomainSignalKind::EnemySpawnWarning, hs::DomainSignalKind::BossSpawnWarning,
        hs::DomainSignalKind::BossAreaTelegraphed, hs::DomainSignalKind::SuicideEnemyCharging};
    const std::array ids{"particle.enemy.spawn_warning", "particle.boss.spawn_warning",
        "particle.boss.area.telegraph", "particle.enemy.suicide.telegraph_radius"};
    for (std::size_t i=0; i<kinds.size(); ++i)
    {
        hs::DomainSignal signal{}; signal.kind=kinds[i]; signal.sequence=44; signal.tick=10;
        signal.geometry.kind=hs::DomainSignalGeometryKind::Circle; signal.geometry.radius=3.75f;
        signal.geometry.source_id=44; signal.geometry.start_tick=10; signal.geometry.end_tick=83;
        std::array<hs::PresentationEvent,8> events{};
        const auto count=hs::ProjectDomainSignal(signal,events);
        const auto span=std::span(events).first(count);
        const auto warning=std::ranges::find_if(span,[&](const auto &e){return e.asset.value==hs::MakeAssetId(ids[i]).value;});
        Check(warning!=span.end() && warning->geometry.radius==3.75f && warning->geometry.source_id==44 &&
            warning->geometry.start_tick==10 && warning->geometry.end_tick==83,"Circle warning projection preserves exact schedule");
        if(i==1) Check(std::ranges::none_of(span,[](const auto &e){return e.asset.value==hs::MakeAssetId("particle.boss.spawn").value;}),
            "boss warning must not emit actual spawn burst");
        if(i==3) Check(std::ranges::count_if(span,[](const auto &e){return e.asset.value==hs::MakeAssetId("particle.enemy.suicide.charge").value;})==1,
            "suicide telegraph preserves charge visual");
    }
    hs::GameReadModelStorage model; model.tick=20;
    model.AddSpawnWarning({44,{3,4},.79f,10,83,false});
    model.AddSpawnWarning({45,{-3,4},1.1f,10,83,true});
    model.AddSpawnWarning({46,{0,0},1,10,20,false});
    hs::EnemyView enemy{}; enemy.id={11}; enemy.kind=hs::EnemyKind::Suicide; enemy.attacking=true;
    enemy.attack_started=10; enemy.attack_resolve=83; enemy.warning_sequence=47; enemy.warning_extent=3.75f;
    model.AddEnemy(enemy);
    hs::EnemyView boss{}; boss.id={12}; boss.boss=hs::BossKind::FiveMinute; model.AddEnemy(boss);
    hs::BossActionView area{}; area.boss_id=12; area.kind=hs::BossActionViewKind::Area;
    area.position={6,7}; area.radius=4.25f; area.warning_sequence=48; area.warning_started=10; area.execute_tick=83;
    model.AddBossAction(area);
    hs::RenderSnapshotStorage snapshot(256,4,4,32);
    Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot),"Circle warning owner snapshot");
    for(auto id:{44u,45u,47u,48u})
    {
        const auto owner=std::ranges::find_if(snapshot.View().persistent_vfx,[&](const auto &v){return v.stable_id==id;});
        Check(owner!=snapshot.View().persistent_vfx.end()&&owner->active_tick==10&&owner->expires==83,"Circle warning owner clocks lost");
    }
    Check(std::ranges::none_of(snapshot.View().persistent_vfx,[](const auto &v){return v.stable_id==46;}),"expired spawn warning published");
    Check(std::ranges::any_of(snapshot.View().instances,[](const auto &v){return v.mesh==hs::RenderMesh::Area&&v.vfx_owner_id==47;})&&
        std::ranges::any_of(snapshot.View().instances,[](const auto &v){return v.mesh==hs::RenderMesh::Area&&v.vfx_owner_id==48;}),"Circle legacy markers lack replacement owners");
    model.Clear(); snapshot.Clear();
    Check(model.View().spawn_warnings.empty(),"spawn warning storage clear");
    Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot)&&snapshot.View().persistent_vfx.empty(),
        "removed warning owners linger in next snapshot");
}

void TestStatusEpisodeSnapshot()
{
 hs::GameReadModelStorage model;model.tick=20;model.session_id=7;hs::EnemyView enemy;enemy.id={12};enemy.position={3,4};enemy.health=30;enemy.max_health=100;enemy.footprint_radius=.81f;enemy.status_episodes[0]={10,40,1};enemy.status_episodes[3]={10,21,2};model.AddEnemy(enemy);
 hs::RenderSnapshotStorage snapshot(256,4,4,32);
 const auto project=[&](){snapshot.Clear();Check(hs::ProjectRenderSnapshot(model.View(),DefaultContent().presentation,test_ui,hs::SettingsData{},snapshot),"status snapshot projection");};
 project();auto owners=snapshot.View().persistent_vfx;Check(owners.size()==2&&owners[0].radius==.81f&&owners[0].entity_health_fraction==.3f,"status actual footprint/health lost");const auto identity=owners[0].stable_id;
 model.tick=21;project();Check(snapshot.View().persistent_vfx.size()==1&&snapshot.View().persistent_vfx[0].stable_id==identity,"mark inclusive expiry lost or bleed identity changed");
 model.session_id=8;project();Check(snapshot.View().persistent_vfx[0].stable_id!=identity,"new session reused status owner");model.tick=40;project();Check(snapshot.View().persistent_vfx.empty(),"expired status owner retained");
}

void TestPickupCollectionEndpointProjection()
{
    struct PickupCase
    {
        hs::DomainSignalKind kind;
        std::string_view asset;
        std::string_view audio;
    };
    constexpr std::array cases{
        PickupCase{hs::DomainSignalKind::ExperienceCollected,
                   "particle.pickup.xp_collect", "audio.pickup.xp_collect"},
        PickupCase{hs::DomainSignalKind::HealCollected,
                   "particle.pickup.heal_collect", "audio.pickup.heal"},
        PickupCase{hs::DomainSignalKind::MagnetCollected,
                   "particle.pickup.magnet_collect", "audio.pickup.magnet"},
        PickupCase{hs::DomainSignalKind::RelicCollected,
                   "particle.pickup.relic_collect", "audio.pickup.relic_chest"}};
    for (const auto &pickup : cases)
    {
        hs::DomainSignal signal{};
        signal.kind = pickup.kind;
        signal.sequence = 73;
        signal.position = {4.0f, 0.3f, -2.0f};
        signal.target = {-1.0f, 0.3f, 6.0f};
        signal.flags = static_cast<std::uint8_t>(hs::DomainSignalFlag::HasTarget);
        std::array<hs::PresentationEvent, 3> output{};
        const auto count = hs::ProjectDomainSignal(signal, output);
        const auto parameters = hs::DecodeVfxParameters(output[0].parameters);
        const bool opens_chest = pickup.kind == hs::DomainSignalKind::RelicCollected;
        const auto audio_index = opens_chest ? 2u : 1u;
        Check(count == audio_index + 1 && output[0].kind == hs::PresentationKind::Vfx &&
                  output[0].asset.value == hs::MakeAssetId(pickup.asset).value &&
                  output[0].position.x == signal.position.x &&
                  output[0].position.y == signal.position.y &&
                  output[0].position.z == signal.position.z &&
                  parameters.flags == static_cast<std::uint32_t>(hs::VfxEventFlag::HasTarget) &&
                  parameters.target.x == signal.target.x &&
                  parameters.target.y == signal.target.y &&
                  parameters.target.z == signal.target.z &&
                  (!opens_chest || (output[1].kind == hs::PresentationKind::Vfx &&
                      output[1].asset.value == hs::MakeAssetId("particle.pickup.relic_chest.open").value)) &&
                  output[audio_index].kind == hs::PresentationKind::Audio &&
                  output[audio_index].asset.value == hs::MakeAssetId(pickup.audio).value,
              "pickup projection preserves source, player endpoint, and existing audio");
    }
}

void TestPickupIdleAttachmentOwners()
{
    struct PickupCase
    {
        hs::PickupKind kind;
        hs::PersistentVfxKind visual_kind;
        std::string_view asset;
    };
    constexpr std::array cases{
        PickupCase{hs::PickupKind::Experience, hs::PersistentVfxKind::PickupXpIdle,
                   "particle.pickup.xp.idle"},
        PickupCase{hs::PickupKind::Heal, hs::PersistentVfxKind::PickupHealIdle,
                   "particle.pickup.heal.idle"},
        PickupCase{hs::PickupKind::Magnet, hs::PersistentVfxKind::PickupMagnetIdle,
                   "particle.pickup.magnet.idle"},
        PickupCase{hs::PickupKind::RelicChest, hs::PersistentVfxKind::PickupRelicIdle,
                   "particle.pickup.relic_chest.idle"}};
    hs::GameReadModelStorage model;
    model.tick = 40;
    model.session_id = 7;
    hs::RenderSnapshotStorage snapshot(32, 2, 2, 8);
    const auto project = [&] {
        snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
            test_ui, hs::SettingsData{}, snapshot), "pickup idle snapshot projection");
    };
    const auto find_owner = [&](hs::PersistentVfxKind kind) -> const hs::PersistentVfxVisual * {
        const auto owners = snapshot.View().persistent_vfx;
        const auto found = std::ranges::find_if(owners,
            [kind](const auto &visual) { return visual.kind == kind; });
        return found == owners.end() ? nullptr : &*found;
    };
    for (std::size_t i = 0; i < cases.size(); ++i)
        model.AddPickup({{100 + i}, cases[i].kind,
            {static_cast<float>(i), static_cast<float>(i + 2)}, false,
            static_cast<hs::Tick>(10 + i)});
    project();
    std::array<std::uint64_t, cases.size()> identities{};
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        const auto *owner = find_owner(cases[i].visual_kind);
        const auto instances = snapshot.View().instances;
        const auto render_id = (5ull << 60) | (100 + i);
        const auto instance = std::ranges::find_if(instances,
            [render_id](const auto &candidate) {
                return candidate.mesh == hs::RenderMesh::Pickup &&
                       candidate.stable_id == render_id;
            });
        Check(owner && instance != instances.end() &&
            owner->effect_asset.value == hs::MakeAssetId(cases[i].asset).value &&
            owner->entity_render_id == instance->stable_id &&
            owner->position.x == instance->position.x &&
            owner->position.y == instance->position.y &&
            owner->position.z == instance->position.z &&
            owner->radius == std::max(instance->scale.x, instance->scale.z) &&
            owner->active_tick == 10 + i && owner->expires == 0 &&
            owner->stable_id != 0, "pickup idle owner loses actor geometry, asset, or spawn clock");
        Check(std::ranges::count_if(snapshot.View().persistent_vfx,
            [kind = cases[i].visual_kind](const auto &visual) {
                return visual.kind == kind;
            }) == 1, "live pickup projects exactly one idle owner");
        identities[i] = owner->stable_id;
    }
    model.Clear();
    model.tick = 41;
    for (std::size_t i = 0; i < cases.size(); ++i)
        model.AddPickup({{100 + i}, cases[i].kind,
            {static_cast<float>(i + 6), static_cast<float>(i + 8)}, false,
            static_cast<hs::Tick>(10 + i)});
    project();
    for (std::size_t i = 0; i < cases.size(); ++i)
    {
        const auto *owner = find_owner(cases[i].visual_kind);
        Check(owner && owner->stable_id == identities[i] &&
            owner->active_tick == 10 + i &&
            owner->position.x == static_cast<float>(i + 6) &&
            owner->position.z == static_cast<float>(i + 8),
            "pickup idle owner must follow its actor without restarting");
    }
    model.Clear();
    for (std::size_t i = 1; i < cases.size(); ++i)
        model.AddPickup({{100 + i}, cases[i].kind,
            {static_cast<float>(i + 6), static_cast<float>(i + 8)}, false,
            static_cast<hs::Tick>(10 + i)});
    project();
    Check(!find_owner(hs::PersistentVfxKind::PickupXpIdle),
        "collected pickup retained its idle owner");
    model.session_id = 8;
    project();
    const auto *next_session_heal = find_owner(hs::PersistentVfxKind::PickupHealIdle);
    Check(next_session_heal && next_session_heal->stable_id != identities[1],
        "new session reused a pickup owner identity");
}

void TestPlayerAttachmentOwnerWindows()
{
    hs::GameReadModelStorage model;
    hs::RenderSnapshotStorage snapshot(30000, 2048, 8, 2048);
    const auto project = [&] {
        snapshot.Clear();
        Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
            test_ui, hs::SettingsData{}, snapshot), "player attachment snapshot");
    };
    const auto find = [&](hs::PersistentVfxKind kind) -> const hs::PersistentVfxVisual * {
        const auto owners = snapshot.View().persistent_vfx;
        const auto found = std::ranges::find_if(owners,
            [kind](const auto &visual) { return visual.kind == kind; });
        return found == owners.end() ? nullptr : &*found;
    };

    model.tick = 10;
    model.player.position = {2.0f, 3.0f};
    model.player.basic_attack_animation_start = 10;
    model.player.basic_attack_release_tick = 14;
    model.player.basic_attack_cast_id = 77;
    project();
    const auto *basic = find(hs::PersistentVfxKind::PlayerBowDraw);
    Check(basic && basic->effect_asset.value == hs::MakeAssetId("particle.player.bow_draw").value &&
        basic->active_tick == 10 && basic->expires == 14 &&
        basic->source_horizon_tick == 14 && basic->entity_render_id == (1ull << 60) &&
        basic->radius > 0 && basic->position.x == 2.0f && basic->position.z == 3.0f,
        "basic draw lost actual release or player attachment");
    const auto basic_id = basic->stable_id;
    model.tick = 13; model.player.position = {4.0f, 5.0f}; project();
    basic = find(hs::PersistentVfxKind::PlayerBowDraw);
    Check(basic && basic->stable_id == basic_id && basic->position.x == 4.0f,
        "basic draw owner changed while moving");
    model.tick = 14; project();
    Check(!find(hs::PersistentVfxKind::PlayerBowDraw),
        "basic draw persisted through release tick");

    model.player.charging = true;
    model.player.charging_skill = hs::SkillKind::ChargedShot;
    model.player.charge_start = 20;
    model.player.charge_full_ready_tick = 59;
    model.tick = 58; project();
    const auto *charge = find(hs::PersistentVfxKind::PlayerBowDraw);
    Check(charge && charge->active_tick == 20 && charge->expires == 0 &&
        charge->source_horizon_tick == 59 &&
        charge->stable_id != basic_id && !find(hs::PersistentVfxKind::ChargedFullReady),
        "live charged draw needs its own owner and no forecast deadline");
    model.tick = 59; project();
    const auto *ready = find(hs::PersistentVfxKind::ChargedFullReady);
    Check(ready && ready->effect_asset.value ==
        hs::MakeAssetId("particle.upgrade.charged.full_ready").value &&
        ready->active_tick == 59 && ready->expires == 92 &&
        ready->stable_id != find(hs::PersistentVfxKind::PlayerBowDraw)->stable_id &&
        ready->entity_render_id == (1ull << 60) && ready->radius > 0,
        "full charge ready lost actual threshold or fixed window");
    model.tick = 91; project();
    Check(find(hs::PersistentVfxKind::ChargedFullReady),
        "full charge ready ended before its last live tick");
    model.tick = 92; project();
    Check(!find(hs::PersistentVfxKind::ChargedFullReady),
        "full charge ready exceeded 0.55 seconds");
    model.player.charging = false; model.tick = 60; project();
    Check(!find(hs::PersistentVfxKind::PlayerBowDraw) &&
        !find(hs::PersistentVfxKind::ChargedFullReady),
        "released charge retained player effects");

    model.player.charging = true;
    model.player.charge_start = 20;
    model.player.upgrades[static_cast<std::size_t>(hs::SkillKind::ChargedShot)] = 1;
    model.player.charge_full_ready_tick = 104;
    model.charge_normal_ready_tick = 59;
    model.charge_ratio = .8f;
    model.charge_radius = .72f;
    model.tick = 60; project();
    const auto *overcharge = find(hs::PersistentVfxKind::ChargedOverchargeLoop);
    Check(overcharge && overcharge->active_tick == 20 &&
        overcharge->expires == 104 && overcharge->radius == .72f &&
        overcharge->charge_ratio == .8f && overcharge->source_upgrade == 0,
        "charged ordinal1 overcharge owner lost gameplay threshold, footprint, or ratio");
    model.tick = 104; project();
    Check(!find(hs::PersistentVfxKind::ChargedOverchargeLoop),
        "charged ordinal1 overcharge owner survived its gameplay end");
    model.player.charging = false;

    model.player.active_basic_empower_started = 100;
    model.player.active_basic_empower_until = 103;
    model.tick = 102; project();
    Check(!find(hs::PersistentVfxKind::EmpoweredReady),
        "empower effect appeared without basic upgrade eight");
    model.player.upgrades[static_cast<std::size_t>(hs::SkillKind::BasicAttack)] = 0x80;
    model.tick = 103; project();
    const auto *empowered = find(hs::PersistentVfxKind::EmpoweredReady);
    Check(empowered && empowered->effect_asset.value ==
        hs::MakeAssetId("particle.upgrade.empowered_ready").value &&
        empowered->active_tick == 100 && empowered->expires == 104 &&
        empowered->entity_render_id == (1ull << 60) && empowered->radius > 0,
        "empower activation should include its final gameplay tick");
    const auto empower_id = empowered->stable_id;
    model.tick = 104; project();
    Check(!find(hs::PersistentVfxKind::EmpoweredReady),
        "expired empower activation retained its owner");
    model.player.active_basic_empower_started = 120;
    model.player.active_basic_empower_until = 123;
    model.tick = 120; project();
    empowered = find(hs::PersistentVfxKind::EmpoweredReady);
    Check(empowered && empowered->stable_id != empower_id,
        "a new empower activation reused its previous owner");
    const auto next_activation_id = empowered->stable_id;
    model.session_id = 2; project();
    empowered = find(hs::PersistentVfxKind::EmpoweredReady);
    Check(empowered && empowered->stable_id != next_activation_id,
        "a new session reused the previous player owner");
}

void TestPresentationCompletePacket()
{
    hs::GameReadModelStorage model;
    model.session_id = 41;
    model.tick = 1;
    hs::RenderSnapshotStorage snapshot(64, 8, 4, 16);
    const auto project = [&] {
        snapshot.Clear();
        return hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation,
            test_ui, hs::SettingsData{}, snapshot);
    };
    Check(project(), "initial complete packet projection");
    const auto count = snapshot.View().instances.size();
    Check(count != 0 && !snapshot.View().poses.empty(), "complete packet retains instance and pose operands");
    const auto check_complete = [&] {
        const auto packet = snapshot.View();
        Check(packet.instances.size() == count && packet.header.tick == model.tick &&
                  packet.header.session_id == model.session_id,
              "packet reuse or skipped publication cannot omit unchanged rows");
        for (const auto &instance : packet.instances)
            Check(instance.revisions == hs::RenderInstance::Revisions{},
                  "dynamic projection must not hash source revisions");
        for (const auto &pose : packet.poses)
            Check(pose.revision == 0, "dynamic projection must publish actual pose operands without hashes");
        const auto player = std::ranges::find(packet.instances, hs::RenderMesh::Archer, &hs::RenderInstance::mesh);
        Check(player != packet.instances.end() && player->position.x == model.player.position.x &&
                  player->position.z == model.player.position.y && player->asset_id.value != 0,
              "complete packet lost the final player transform or binding");
    };
    check_complete();
    model.tick += 4; // Renderer may skip any number of complete publications.
    model.player.position = {3, -2};
    Check(project(), "complete packet after skipped publication");
    check_complete();
    Check(project(), "reused complete packet");
    check_complete();
    ++model.session_id;
    ++model.tick;
    Check(project(), "new-session complete packet");
    check_complete();
}
void TestBossDeathCaptureWithoutPriorSnapshot()
{
    hs::GameReadModelStorage model;
    model.session_id = 9; model.tick = 37;
    hs::BossDeathVisualView death;
    death.sequence = 71; death.tick = 37;
    death.enemy.id = {55}; death.enemy.boss = hs::BossKind::Final;
    death.enemy.position = {8, -3}; death.enemy.velocity = {2, 1};
    death.enemy.max_health = 100; death.enemy.health = 0;
    death.has_action = true; death.action.boss_id = 55;
    death.action.animation_started = 30; death.action.execute_tick = 50;
    death.action.kind = hs::BossActionViewKind::Area;
    model.AddEnemy(death.enemy); model.AddBossAction(death.action);
    hs::RenderSnapshotStorage live(4, 4, 1, 16);
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation, test_ui, hs::SettingsData{}, live),
          "live boss reference projection");
    const auto reference = live.View();
    const auto boss = std::ranges::find(reference.instances, hs::RenderMesh::BossFinal,
                                       &hs::RenderInstance::mesh);
    Check(boss != reference.instances.end(), "live boss reference instance missing");
    const auto boss_index = static_cast<std::uint32_t>(boss - reference.instances.begin());
    const auto boss_pose = std::ranges::find(reference.poses, boss_index,
                                            &hs::AnimationPoseRef::instance_index);
    Check(boss_pose != reference.poses.end(), "live boss reference pose missing");
    const auto actual = *boss;
    const auto pose = *boss_pose;
    model.Clear(); model.tick = 900; model.AddBossDeathVisual(death);
    hs::RenderSnapshotStorage captured(4, 4, 1, 16);
    Check(hs::ProjectRenderSnapshot(model.View(), DefaultContent().presentation, test_ui, hs::SettingsData{}, captured),
          "boss capture projection without live snapshot history");
    Check(captured.View().captured_instances.size() == 1, "boss capture missing after actor removal");
    const auto &record = captured.View().captured_instances.front();
    Check(record.session_id == 9 && record.source_entity_id == 55 && record.sequence == 71 && record.tick == 37 &&
          record.instance.stable_id == actual.stable_id && record.instance.asset_id.value == actual.asset_id.value &&
          record.instance.material_id.value == actual.material_id.value && record.instance.mesh == hs::RenderMesh::BossFinal &&
          record.instance.position.x == actual.position.x && record.instance.scale.x == actual.scale.x &&
          record.instance.yaw == actual.yaw && record.animation_pose.clip == pose.clip &&
          record.animation_pose.normalized_time == pose.normalized_time,
          "death capture substituted mesh, transform, provenance or death-tick pose");
    Check(std::ranges::none_of(captured.View().instances, [&](const auto &value) { return value.stable_id == actual.stable_id; }),
          "capture inserted a dead actor into the ordinary snapshot scene");
}

void TestBorrowedProjectionOwnsPublication()
{
    hs::GameSimulation simulation;
    Check(simulation.Initialize({319}, QuietGameData()).Succeeded(), "borrowed projection initialize");
    Debug(simulation, hs::DebugCommandKind::SpawnEnemy, 0, 0, {6, 0});
    hs::HeldInputState held{}; held.basic_attack_held = true; held.aim_world = {20, 0, 0};
    for (int frame = 0; frame < 18; ++frame) (void)Tick(simulation, held);
    hs::GameReadModelStorage legacy;
    hs::CopyReadModelForDevelopment(simulation, legacy);
    hs::RenderSnapshotStorage projected(64, 64, 8, 64), fixture(64, 64, 8, 64);
    {
        auto read = simulation.BorrowTickReadView();
        Check(hs::ProjectTick(read.View(), DefaultContent().presentation, {}, {}, projected,
                  hs::ProcessSceneView(hs::BuildProcessSceneAtLaunch(read.View(), {}))) &&
                  hs::ProjectRenderSnapshot(legacy.View(), DefaultContent().presentation, {}, {}, fixture),
              "borrowed and explicit owned fixture projection succeed");
    }
    const auto a = projected.View(), b = fixture.View();
    Check(a.header.tick == b.header.tick && a.header.session_id == b.header.session_id &&
              a.instances.size() == b.instances.size() && a.poses.size() == b.poses.size() &&
              a.persistent_vfx.size() == b.persistent_vfx.size() && a.ui.size() == b.ui.size(),
          "borrowed projection preserves emitted row counts and publication identity");
    for (std::size_t row = 0; row < a.instances.size(); ++row)
        Check(a.instances[row].stable_id == b.instances[row].stable_id &&
                  a.instances[row].revisions == b.instances[row].revisions,
              "borrowed projection preserves exact transform, content, material and pose signatures");
    for (std::size_t row = 0; row < a.poses.size(); ++row)
        Check(a.poses[row].revision == b.poses[row].revision, "borrowed animation operands match owned format");
    for (std::size_t row = 0; row < a.persistent_vfx.size(); ++row)
        Check(a.persistent_vfx[row].stable_id == b.persistent_vfx[row].stable_id &&
                  a.persistent_vfx[row].kind == b.persistent_vfx[row].kind &&
                  a.persistent_vfx[row].active_tick == b.persistent_vfx[row].active_tick &&
                  a.persistent_vfx[row].expires == b.persistent_vfx[row].expires,
              "borrowed VFX retains emission order, activation and episode identities");
    Check(!a.instances.empty(), "borrowed packet has live payload");
    const auto saved_tick = a.header.tick;
    const auto saved_id = a.instances.front().stable_id;
    const auto saved_revision = a.instances.front().revisions;
    Debug(simulation, hs::DebugCommandKind::StartSession);
    Check(projected.View().header.tick == saved_tick &&
              projected.View().instances.front().stable_id == saved_id &&
              projected.View().instances.front().revisions == saved_revision,
          "published payload remains owned after borrow ends and authoritative actors reset");
    Check(simulation.Shutdown().Succeeded(), "borrowed projection shutdown");
}

void RunGameplayPresentationTests()
{
    TestBorrowedProjectionOwnsPublication();
    TestBossDeathCaptureWithoutPriorSnapshot();
    TestPresentationCompletePacket();
    TestPlayerAttachmentOwnerWindows();
    TestStatusEpisodeSnapshot();
    TestCircleWarningProjection();
    TestUpgradeActivationProjection();
    TestVisualLinkSnapshotTransport();
    TestCursorMovement();
    TestUiHitRegionsMatchAnchors();
    TestCharacterInformationPage();
    TestPauseMenuActions();
    TestSkillUpgradeCardsHaveDescriptions();
    TestBossAnimationReachesReleaseFrame();
    TestMonsterVisualScalesAndAttackFacing();
    TestRelicTriggerProjection();
    TestProjectileProjectionCarriesAuthoritativeOwner();
    TestProjectileUpgradeAttachmentEpisodes();
    TestEnemyAnimationStateSignals();
    TestPlayerFootstepTimeline();
    TestPlayerInstantMotionSignal();
    TestSlimeAttackPoseTransitions();
    TestAuthoredEnvironmentProjection();
    TestStaticEnvironmentSceneLease();
    TestMeadowPresentationGrounding();
    TestArrowReleaseProjectsVfxAndAudio();
    TestBossTelegraphProjectsGeometryAndAudio();
    TestDenseGrassProjection();
    TestPresentationGeometryTransport();
    TestProjectileImpactGeometryTransport();
    TestPickupCollectionEndpointProjection();
    TestPickupIdleAttachmentOwners();
    TestSessionIdentityAcrossPresentationReset();
}

} // namespace gameplay_test
