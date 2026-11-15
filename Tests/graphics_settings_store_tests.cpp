#include <hs/runtime/save_store.hpp>
#include <hs/runtime/settings_projection.hpp>
#include <hs/core/bounded_spsc_queue.hpp>

#include "../Source/Runtime/Private/graphics_settings_commit.hpp"
#include "../Source/Runtime/Private/renderer_settings_projection.hpp"
#include "../Source/Runtime/Private/startup_graphics_settings.hpp"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

std::filesystem::path NewRoot(std::string_view name)
{
    const auto root = std::filesystem::temp_directory_path() /
        ("ProjectHS_settings_v2_" + std::to_string(GetCurrentProcessId()) + "_" +
         std::to_string(GetTickCount64()) + "_" + std::string(name));
    std::filesystem::create_directories(root);
    return root;
}

void Write(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    Check(stream.good(), "write fixture");
}

std::string Read(const std::filesystem::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}

std::size_t CorruptCount(const std::filesystem::path &root)
{
    std::size_t count{};
    for (const auto &entry : std::filesystem::directory_iterator(root))
        if (entry.path().extension() == ".corrupt") ++count;
    return count;
}

void TestGraphicsApplyAcknowledgments()
{
    const auto root = NewRoot("ack");
    hs::SaveStore store(root);
    auto original = hs::LegacyDefaultSettingsDocument();
    original.graphics.hdr_mode = hs::GraphicsHdrMode::On;
    original.opaque_optional_json = R"({"graphics":{"future_key":"keep"}})";
    Check(store.SaveSettingsV2(original).Succeeded(), "write initial requested settings");
    hs::GraphicsSettingsCommit controller(
        original, hs::ProjectSettingsDocument(original), false);
    hs::BoundedSpscQueue<hs::GraphicsSettingsCommitRequest, 8> requests;
    hs::BoundedSpscQueue<hs::GraphicsSettingsAck, 8> acknowledgments;

    controller.EditGraphics({hs::UiCommandKind::SetBloom, 0});
    const auto first = controller.NextRequest();
    Check(first && requests.TryPush(*first) && controller.HasPending() &&
              controller.Committed().graphics.bloom &&
              controller.Effective().bloom,
          "graphics edit is pending and not committed");
    hs::GraphicsSettingsCommitRequest applied_request;
    Check(requests.TryPop(applied_request) &&
              acknowledgments.TryPush({applied_request.generation,
                                       hs::ErrorCode::InvalidState}),
          "fake failed renderer apply");
    hs::GraphicsSettingsAck ack;
    Check(acknowledgments.TryPop(ack) &&
              controller.Acknowledge(ack) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Rejected &&
              controller.Requested().graphics.bloom &&
              controller.Effective().bloom && !controller.HasPending(),
          "failed apply restores requested and effective graphics");
    Check(Read(root / "settings.json").find("\"bloom\": true") !=
              std::string::npos,
          "failed apply never writes requested graphics");

    controller.EditGraphics({hs::UiCommandKind::SetVsync, 0});
    const auto second = controller.NextRequest();
    Check(second && requests.TryPush(*second), "queue second graphics request");
    controller.EditImmediate({hs::UiCommandKind::SetMasterVolumePercent, 40});
    Check(store.SaveSettingsV2(controller.Committed()).Succeeded() &&
              controller.Committed().master_volume == 0.4f &&
              controller.Committed().graphics.vsync,
          "audio save excludes pending graphics");
    controller.EditGraphics({hs::UiCommandKind::SetBloom, 0});
    Check(requests.TryPop(applied_request) &&
              acknowledgments.TryPush({applied_request.generation,
                                       hs::ErrorCode::None}) &&
              acknowledgments.TryPop(ack) &&
              controller.Acknowledge(ack) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Applied,
          "fake successful renderer apply");
    Check(!controller.Committed().graphics.vsync &&
              controller.Committed().graphics.bloom &&
              !controller.Requested().graphics.bloom &&
              controller.Effective().bloom,
          "success commits applied generation while later edit remains pending");
    Check(store.SaveSettingsV2(controller.Committed()).Succeeded(),
          "save successful generation");
    const auto third = controller.NextRequest();
    Check(third && third->generation > second->generation &&
              requests.TryPush(*third), "queue superseding generation");
    Check(controller.Acknowledge({second->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Ignored &&
              controller.Committed().graphics.bloom,
          "stale acknowledgment cannot commit a newer request");
    Check(requests.TryPop(applied_request) &&
              controller.Acknowledge({applied_request.generation,
                                      hs::ErrorCode::InvalidState}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Rejected &&
              controller.Requested().graphics.bloom &&
              controller.Effective().bloom,
          "latest failure restores last committed graphics");

    controller.EditGraphics({hs::UiCommandKind::SetBloom, 0});
    const auto unsent = controller.NextRequest();
    Check(unsent && controller.HasPending(), "prepare request for full queue");
    controller.RejectUnqueuedRequest();
    Check(!controller.HasPending() &&
              !controller.Requested().graphics.bloom &&
              controller.Committed().graphics.bloom &&
              controller.NextRequest().has_value(),
          "full queue leaves requested edit retryable without committing it");

    hs::SettingsDocument restarted;
    Check(store.LoadSettingsV2(restarted).Succeeded() &&
              !restarted.graphics.vsync && restarted.graphics.bloom &&
              restarted.master_volume == 0.4f &&
              restarted.graphics.hdr_mode == hs::GraphicsHdrMode::On &&
              restarted.opaque_optional_json.find("future_key") != std::string::npos,
          "restart retains only successful graphics and immediate audio edits");
    std::filesystem::remove_all(root);
}

void TestFailedApplyKeepsLatestDesiredSettings()
{
    auto original = hs::LegacyDefaultSettingsDocument();
    original.graphics.hdr_mode = hs::GraphicsHdrMode::On;
    original.opaque_optional_json = R"({"graphics":{"future_key":"keep"}})";
    hs::GraphicsSettingsCommit controller(
        original, hs::ProjectSettingsDocument(original), false);
    controller.EditGraphics({hs::UiCommandKind::SetBloom, 0});
    const auto failed = controller.NextRequest();
    Check(failed && !failed->effective.bloom, "queue bloom request");
    controller.EditGraphics({hs::UiCommandKind::SetVsync, 0});
    Check(controller.Acknowledge({failed->generation, hs::ErrorCode::InvalidState}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Rejected &&
              !controller.Requested().graphics.bloom &&
              !controller.Requested().graphics.vsync &&
              !controller.UiInput().bloom && !controller.UiInput().vsync &&
              controller.Committed().graphics.bloom &&
              controller.Committed().graphics.vsync && controller.Effective().bloom &&
              controller.Effective().vsync,
          "failed apply keeps the full latest desired settings while applied state stays unchanged");
    const auto next = controller.NextRequest();
    Check(next && !next->effective.bloom && !next->effective.vsync &&
              controller.Acknowledge({next->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              !controller.Committed().graphics.bloom &&
              !controller.Committed().graphics.vsync &&
              controller.Committed().graphics.hdr_mode == hs::GraphicsHdrMode::On &&
              controller.Committed().opaque_optional_json ==
                  R"({"graphics":{"future_key":"keep"}})",
          "next request applies the full latest desired settings and retains unknown fields");
}

void TestDisplayIntentWaitsForConfirmation()
{
    const auto original = hs::LegacyDefaultSettingsDocument();
    hs::GraphicsSettingsCommit controller(
        original, hs::ProjectSettingsDocument(original), false);
    controller.EditGraphics({hs::UiCommandKind::SetBorderless, 1});
    controller.SetDesiredOutputExtent(1920, 1080);
    const auto change = controller.NextRequest();
    Check(change && change->effective.borderless &&
              change->effective.width == 1920 &&
              controller.Committed().graphics.display_mode ==
                  hs::GraphicsDisplayMode::Windowed &&
              controller.Committed().graphics.output_extent ==
                  original.graphics.output_extent,
          "display change queues actual client extent while saved intent stays previous");
    Check(controller.Acknowledge({change->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              controller.Committed().graphics.display_mode ==
                  hs::GraphicsDisplayMode::BorderlessFullscreen &&
              controller.Effective().width == 1920,
          "renderer acknowledgment exposes unconfirmed applied display");
    controller.EditGraphics({hs::UiCommandKind::SetVsync,
                              original.graphics.vsync ? 0u : 1u});
    controller.RestoreDisplay(original.graphics);
    controller.SetDesiredOutputExtent(1280, 720);
    const auto rollback = controller.NextRequest();
    Check(rollback && !rollback->effective.borderless &&
              rollback->effective.width == 1280 &&
              rollback->effective.vsync == !original.graphics.vsync &&
              controller.Acknowledge({rollback->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              controller.Committed().graphics.display_mode ==
                  original.graphics.display_mode &&
              controller.Requested().graphics.display_output == original.graphics.display_output &&
              controller.Requested().graphics.output_extent == original.graphics.output_extent &&
              controller.Committed().graphics.display_output == original.graphics.display_output &&
              controller.Committed().graphics.output_extent == original.graphics.output_extent &&
              controller.Committed().graphics.vsync == !original.graphics.vsync,
          "cancellation restores previous display through a second acknowledged request");

    controller.EditGraphics({hs::UiCommandKind::SetBorderless, 1});
    const auto failed = controller.NextRequest();
    Check(failed && controller.Acknowledge(
              {failed->generation, hs::ErrorCode::InvalidState}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Rejected &&
              !controller.Effective().borderless &&
              !controller.UiInput().borderless,
          "failed display apply does not mark target mode applied");

    controller.EditGraphics({hs::UiCommandKind::SetBloom, 0});
    const auto earlier = controller.NextRequest();
    controller.EditGraphics({hs::UiCommandKind::SetBorderless, 1});
    const auto earlier_rejected = earlier && controller.Acknowledge(
              {earlier->generation, hs::ErrorCode::InvalidState}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Rejected;
    const auto followup = controller.NextRequest();
    hs::BoundedSpscQueue<hs::GraphicsSettingsCommitRequest, 8> queued_followups;
    Check(earlier_rejected && controller.UiInput().borderless &&
              !controller.UiInput().bloom && controller.Effective().bloom && followup &&
              followup->effective.borderless && !followup->effective.bloom && queued_followups.TryPush(*followup),
          "deferred display request carries latest desired settings while applied state stays unchanged");
}

void TestDeferredDisplaySaveBoundary()
{
    const auto root = NewRoot("deferred-display");
    hs::SaveStore store(root);
    const auto original = hs::LegacyDefaultSettingsDocument();
    Check(store.SaveSettingsV2(original).Succeeded(), "save original display mode");
    hs::GraphicsSettingsCommit controller(
        original, hs::ProjectSettingsDocument(original), false);
    controller.EditGraphics({hs::UiCommandKind::SetBloom, 0});
    const auto earlier = controller.NextRequest();
    controller.EditGraphics({hs::UiCommandKind::SetBorderless, 1});
    controller.EditImmediate({hs::UiCommandKind::SetMasterVolumePercent, 40});
    Check(earlier && controller.Acknowledge({earlier->generation,
                                            hs::ErrorCode::None}) ==
                         hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              store.SaveSettingsV2(controller.Committed()).Succeeded(),
          "earlier applied generation remains saveable while display is deferred");
    hs::SettingsDocument saved;
    Check(store.LoadSettingsV2(saved).Succeeded() &&
              !saved.graphics.bloom &&
              saved.graphics.display_mode == hs::GraphicsDisplayMode::Windowed &&
              saved.master_volume == 0.4f,
          "deferred save includes confirmed graphics and audio, excludes display intent");

    controller.DiscardUnsentEdits();
    Check(store.SaveSettingsV2(controller.Committed()).Succeeded() &&
              store.LoadSettingsV2(saved).Succeeded() &&
              saved.graphics.display_mode == hs::GraphicsDisplayMode::Windowed &&
              !saved.graphics.bloom && saved.master_volume == 0.4f,
          "window style failure keeps the confirmed document persisted");

    controller.EditGraphics({hs::UiCommandKind::SetBorderless, 1});
    const auto display = controller.NextRequest();
    Check(display && controller.Acknowledge({display->generation,
                                             hs::ErrorCode::None}) ==
                         hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              store.LoadSettingsV2(saved).Succeeded() &&
              saved.graphics.display_mode == hs::GraphicsDisplayMode::Windowed,
          "unconfirmed display acknowledgment does not change saved intent");
    std::filesystem::remove_all(root);
}

hs::UiCommand GraphicsCommand(hs::SettingId id, std::string_view token)
{
    hs::UiCommand command{hs::UiCommandKind::SetGraphicsSetting};
    command.graphics_setting = id;
    Check(token.size() < command.graphics_token.size(), "complete UI token fits");
    std::copy(token.begin(), token.end(), command.graphics_token.begin());
    return command;
}

void TestFullGraphicsTransactions()
{
    {
        auto invalid_document = hs::LegacyDefaultSettingsDocument();
        invalid_document.graphics.dynamic_min_percent = 95.0f;
        invalid_document.graphics.dynamic_max_percent = 70.0f;
        const auto bloom_before = invalid_document.graphics.bloom;
        const auto minimum_before = invalid_document.graphics.dynamic_min_percent;
        const auto maximum_before = invalid_document.graphics.dynamic_max_percent;
        const auto volume_before = invalid_document.master_volume;

        for (const auto &command : {GraphicsCommand(hs::SettingId::Bloom, "false"),
                 hs::UiCommand{hs::UiCommandKind::SetBloom, 0},
                 hs::UiCommand{hs::UiCommandKind::SetMasterVolumePercent, 50}})
            Check(!hs::ApplySettingsEdit(invalid_document, command) &&
                      invalid_document.graphics.bloom == bloom_before &&
                      invalid_document.graphics.dynamic_min_percent == minimum_before &&
                      invalid_document.graphics.dynamic_max_percent == maximum_before &&
                      invalid_document.master_volume == volume_before,
                  "settings edits reject invalid existing graphics without mutation");
    }

    auto original = hs::LegacyDefaultSettingsDocument();
    original.graphics.reconstruction_provider.value = "saved_unavailable_provider";
    original.graphics.display_output.value = "saved_monitor";
    original.graphics.frame_limit_fps = 120;
    hs::GraphicsSettingsCommit controller(original, hs::ProjectSettingsDocument(original), false);
    Check(controller.EditGraphics({hs::UiCommandKind::SetBloom, 0}) &&
              controller.EditGraphics(GraphicsCommand(hs::SettingId::Bloom, "true")) &&
              controller.Requested().graphics.bloom &&
              controller.EditGraphics(GraphicsCommand(hs::SettingId::Preset, "ultra")) &&
              controller.Requested().graphics.preset == hs::GraphicsPreset::Ultra &&
              controller.Requested().graphics.gi_mode == hs::GraphicsGiMode::Probe &&
              controller.Requested().graphics.shadow_mode == hs::GraphicsShadowMode::Raster &&
              controller.Requested().graphics.pom_quality == hs::GraphicsOptionalQuality::Ultra &&
              controller.Requested().graphics.frame_limit_fps == 120 &&
              controller.Requested().graphics.display_output.value == "saved_monitor" &&
              controller.Requested().graphics.reconstruction_provider.value == "saved_unavailable_provider",
          "preset applies owned quality/modes while preserving user/provider intent");
    const auto first = controller.NextRequest();
    Check(first.has_value(), "full preset request");
    const auto immutable_provider = first->graphics->reconstruction_provider.value;
    auto invalid = GraphicsCommand(hs::SettingId::DynamicMinPercent, "101");
    Check(!controller.EditGraphics(invalid), "invalid follow-up rejected before mutation");
    invalid.graphics_token.fill('x');
    Check(!controller.EditGraphics(invalid), "unterminated token never truncated");
    Check(controller.EditGraphics(GraphicsCommand(hs::SettingId::Preset, "low")) &&
              controller.EditGraphics(GraphicsCommand(hs::SettingId::ReflectionQuality, "high")),
          "coalesced preset and later field edit");
    Check(controller.EditImmediate({hs::UiCommandKind::SetMasterVolumePercent, 40}) &&
              first->graphics->reconstruction_provider.value == immutable_provider &&
              first->effective.master_volume == 1,
          "in-flight native and effective snapshots stay immutable during audio edits");
    Check(controller.Acknowledge({first->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              controller.Requested().graphics.preset == hs::GraphicsPreset::Custom &&
              controller.Requested().graphics.gi_mode == hs::GraphicsGiMode::Probe &&
              controller.Requested().graphics.reflection_quality == hs::GraphicsQuality::High &&
              controller.Requested().master_volume == 0.4f,
          "follow-ups preserve actual serial order and immediate audio");
    const auto preset_followup = controller.NextRequest();
    Check(preset_followup && controller.Acknowledge({preset_followup->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Applied,
          "ordered graphics follow-ups apply after the first acknowledgment");
    Check(!controller.NextRequest(), "saving does not enqueue a native recovery request");
    controller.EditGraphics(GraphicsCommand(hs::SettingId::HdrMode, "on"));
    const auto later = controller.NextRequest();
    Check(later.has_value(), "newer intent creates a native apply request");
    const auto *packet = later->graphics;
    controller.EditGraphics(GraphicsCommand(hs::SettingId::DynamicMaxPercent, "90"));
    controller.MarkTimedOut();
    Check(controller.HasPending() && controller.TimedOut() && packet == later->graphics &&
              !controller.NextRequest(), "timeout retains native read lease and blocks replacement");
    Check(controller.Acknowledge({later->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Recovering &&
              controller.Requested().graphics.dynamic_max_percent == 90.0f,
          "late successful acknowledgment schedules timeout restore and retains newer edits");
    const auto timeout_restore = controller.NextRequest();
    Check(timeout_restore && timeout_restore->generation > later->generation &&
              controller.Acknowledge({later->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Ignored,
          "stale receipt cannot replace timeout restoration generation");
    Check(controller.Acknowledge({timeout_restore->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Restored,
          "timeout restore is acknowledged before newer intent is sent");
    const auto followup = controller.NextRequest();
    Check(followup && followup->graphics->dynamic_max_percent == 90.0f &&
              controller.Acknowledge({followup->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Applied,
          "newer edit is applied after the timeout restore");
    Check(controller.EditGraphics({hs::UiCommandKind::SetVfxScreenOverlayIntensityPercent, 30}),
          "accessibility option uses renderer transaction");
    const auto overlay = controller.NextRequest();
    Check(overlay && overlay->effective.screen_overlay_intensity == 0.3f &&
              controller.Acknowledge({overlay->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              hs::ProjectRendererConfig(controller.Effective(), controller.Committed().graphics).screen_overlay_intensity == 0.3f,
          "overlay intensity reaches native options only through matching apply");
}

void TestInvalidAndFutureSchemas()
{
    const auto root = NewRoot("invalid");
    hs::SaveStore store(root);
    auto invalid = hs::SettingsDocument{};
    invalid.graphics.dynamic_min_percent = 101.0f;
    Check(!store.SaveSettingsV2(invalid) &&
              !std::filesystem::exists(root / "settings.json"),
          "range failure does not write");
    invalid = hs::SettingsDocument{};
    invalid.graphics.output_extent = hs::GraphicsExtent{639, 1080};
    Check(!store.SaveSettingsV2(invalid) &&
              !std::filesystem::exists(root / "settings.json"),
          "legacy minimum output extent is preserved");
    invalid = hs::SettingsDocument{};
    invalid.skill_virtual_keys = {'Q', 'Q', 'E', 'R'};
    Check(!store.SaveSettingsV2(invalid) &&
              !std::filesystem::exists(root / "settings.json"),
          "duplicate action keys do not write");

    const std::string malformed =
        R"({"schema_version":2,"graphics":{"vsync":true,"vsync":false},"master_volume":1,"bgm_volume":1,"sfx_volume":1,"ui_volume":1,"skill_virtual_keys":[81,87,69,82]})";
    Write(root / "settings.json", malformed);
    hs::SettingsDocument recovered;
    Check(store.LoadSettingsV2(recovered).Succeeded() &&
              !recovered.graphics.vsync && Read(root / "settings.json") == malformed &&
              CorruptCount(root) == 0,
          "duplicate graphics key uses the last value without rewriting the file");
    Write(root / "settings.json",
          R"({"schema_version":2,"graphics":{"vsync":"on"},"master_volume":1,"bgm_volume":1,"sfx_volume":1,"ui_volume":1,"skill_virtual_keys":[81,87,69,82]})");
    Check(store.LoadSettingsV2(recovered).Succeeded() && CorruptCount(root) == 1,
          "known graphics field with wrong type rejected");
    Write(root / "settings.json",
          R"({"schema_version":2,"graphics":{},"master_volume":1,"bgm_volume":1,"sfx_volume":1,"ui_volume":1,"skill_virtual_keys":[81,87,69,82]})");
    Check(store.LoadSettingsV2(recovered).Succeeded() &&
              recovered.graphics.vsync &&
              recovered.graphics.shadow_quality == hs::GraphicsQuality::Medium &&
              CorruptCount(root) == 1,
          "missing optional graphics keys use registry defaults");
    const std::string oversized(1024 * 1024 + 1, 'x');
    Write(root / "settings.json", oversized);
    Check(!store.LoadSettingsV2(recovered) &&
              Read(root / "settings.json") == oversized && CorruptCount(root) == 1,
          "oversized settings rejected without modifying the file");
    Write(root / "settings.json", R"({"schema_version":"3","graphics":{}})");
    Check(store.LoadSettingsV2(recovered).Succeeded() && CorruptCount(root) == 2,
          "malformed schema type is corrupt rather than future");

    const std::string future =
        R"({"schema_version":3,"graphics":{},"unknown":"keep","unknown":"keep2"})";
    const auto primary = root / "settings.json";
    const auto backup = root / "settings.json.bak";
    Write(primary, future);
    const auto backup_before = std::filesystem::exists(backup) ? Read(backup) : "";
    Check(!store.LoadSettingsV2(recovered) &&
              !store.SaveSettingsV2(hs::SettingsDocument{}),
          "future primary rejected by V2 load and save");
    Check(Read(primary) == future &&
              (std::filesystem::exists(backup) ? Read(backup) : "") == backup_before &&
              CorruptCount(root) == 2,
          "future primary and backup untouched");
    const std::string large_future =
        R"({"schema_version":3,"payload":")" + std::string(1024 * 1024, 'x') + "\"}";
    Write(primary, large_future);
    Check(!store.LoadSettingsV2(recovered) &&
              !store.SaveSettingsV2(hs::SettingsDocument{}) &&
              Read(primary) == large_future && CorruptCount(root) == 2,
          "oversized future schema remains untouched");
    Write(primary, R"({"schema_version":2,"graphics":{}})");
    Write(backup, future);
    const auto primary_before = Read(primary);
    Check(!store.LoadSettingsV2(recovered) &&
              !store.SaveSettingsV2(hs::SettingsDocument{}),
          "future backup rejected during recovery and save");
    Check(Read(primary) == primary_before && Read(backup) == future,
          "future backup and primary untouched");
    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    try
    {
        TestGraphicsApplyAcknowledgments();
        TestFailedApplyKeepsLatestDesiredSettings();
        TestDisplayIntentWaitsForConfirmation();
        TestDeferredDisplaySaveBoundary();
        TestFullGraphicsTransactions();
        TestInvalidAndFutureSchemas();
        std::cout << "graphics settings store: ok\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
