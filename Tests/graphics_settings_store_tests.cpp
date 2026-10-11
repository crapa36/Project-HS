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

void TestLoadSourceAndProjection()
{
    const auto root = NewRoot("source");
    hs::SaveStore store(root);
    hs::SettingsDocument document;
    bool loaded_from_file = true;
    Check(store.LoadSettingsV2(document, &loaded_from_file).Succeeded() &&
              !loaded_from_file,
          "missing settings use in-memory defaults");
    const auto legacy = hs::ProjectSettingsDocument(hs::LegacyDefaultSettingsDocument());
    Check(!legacy.borderless && legacy.frame_cap == 60 &&
              legacy.shadow_resolution == 1024 && legacy.particle_percentage == 100,
          "first run retains v1 effective defaults");

    document.graphics.vsync = false;
    Check(store.SaveSettingsV2(document).Succeeded(), "write requested v2 settings");
    Check(store.LoadSettingsV2(document, &loaded_from_file).Succeeded() &&
              loaded_from_file && !document.graphics.vsync,
          "valid primary reports loaded settings");
    const auto primary = root / "settings.json";
    const auto backup = root / "settings.json.bak";
    std::filesystem::copy_file(primary, backup,
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(primary);
    loaded_from_file = false;
    Check(store.LoadSettingsV2(document, &loaded_from_file).Succeeded() &&
              loaded_from_file && !document.graphics.vsync,
          "valid backup reports loaded settings");

    Write(primary, "invalid");
    Write(backup, "invalid");
    loaded_from_file = true;
    Check(store.LoadSettingsV2(document, &loaded_from_file).Succeeded() &&
              !loaded_from_file,
          "corrupt reset does not report stored settings");
    Check(store.LoadSettingsV2(document, &loaded_from_file).Succeeded() &&
              !loaded_from_file && !std::filesystem::exists(primary),
          "corrupt reset keeps legacy first-run defaults on next launch");
    std::filesystem::remove_all(root);
}

void TestRequestedEditPreservesUnimplementedFields()
{
    hs::SettingsDocument document;
    document.graphics.preset = hs::GraphicsPreset::UltraRT;
    document.graphics.hdr_mode = hs::GraphicsHdrMode::On;
    document.graphics.reconstruction_provider.value = "future_provider";
    document.opaque_optional_json = R"({"graphics":{"future_key":"keep"}})";
    hs::ApplySettingsEdit(document, {hs::UiCommandKind::SetMasterVolumePercent, 50});
    Check(document.master_volume == 0.5f &&
              document.graphics.preset == hs::GraphicsPreset::UltraRT,
          "audio edit preserves requested graphics preset");
    hs::ApplySettingsEdit(document, {hs::UiCommandKind::SetBloom, 0});
    Check(!document.graphics.bloom &&
              document.graphics.preset == hs::GraphicsPreset::Custom &&
              document.graphics.hdr_mode == hs::GraphicsHdrMode::On &&
              document.graphics.reconstruction_provider.value == "future_provider" &&
              document.opaque_optional_json == R"({"graphics":{"future_key":"keep"}})",
          "UI edit changes requested field without dropping other requests");
    hs::ApplySettingsEdit(document, {hs::UiCommandKind::SetRenderScale, 75});
    Check(document.graphics.resolution_mode == hs::GraphicsResolutionMode::Manual &&
              document.graphics.reconstruction_provider.value == "internal_taau" &&
              hs::ProjectSettingsDocument(document).render_scale_percent == 75,
          "render scale selects implemented manual reconstruction");
    document.graphics.shadow_quality = hs::GraphicsQuality::Ultra;
    Check(hs::ProjectSettingsDocument(document).shadow_resolution == 2048,
          "Ultra shadow request projects to the highest supported resolution");
    document.graphics.frame_limit_fps = 240;
    Check(hs::ProjectSettingsDocument(document).frame_cap == 240 &&
              document.graphics.frame_limit_fps == 240,
          "effective frame cap matches render pacing without changing the request");
}

void TestStartupGraphicsRendererProjection()
{
    auto requested = hs::LegacyDefaultSettingsDocument();
    requested.graphics.vsync = true;
    requested.graphics.frame_limit_fps = 47;
    requested.graphics.output_extent = hs::GraphicsExtent{3440, 1440};
    hs::ApplicationConfig config;
    config.width = 1920;
    config.height = 1080;
    config.resolution_explicit = config.vsync_explicit = config.frame_cap_explicit = true;
    config.vsync = false;
    config.frame_cap = 0;
    const auto effective = hs::PrepareStartupGraphicsSettings(requested, config);
    const auto renderer = hs::ProjectRendererConfig(effective, requested.graphics);
    Check(renderer.width == 1920 && renderer.height == 1080 && !renderer.vsync &&
              renderer.presentation_request.frame_limit_fps == 0 &&
              renderer.requested_graphics &&
              renderer.requested_graphics->output_extent == requested.graphics.output_extent &&
              renderer.requested_graphics->display_mode == hs::GraphicsDisplayMode::Windowed,
          "startup CLI intent reaches renderer and window display snapshot");

    hs::GraphicsSettingsCommit controller(requested, effective, false);
    Check(controller.EditGraphics({hs::UiCommandKind::SetBloom, 0}), "later startup settings edit");
    const auto packet = controller.NextRequest();
    Check(packet.has_value(), "later startup settings packet");
    Check(controller.Acknowledge({packet->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Applied,
          "later startup settings acknowledgment");
    const auto after = hs::ProjectRendererConfig(controller.Effective(), controller.Committed().graphics);
    Check(after.width == 1920 && after.height == 1080 && !after.vsync && !after.bloom &&
              after.presentation_request.frame_limit_fps == 0 &&
              after.requested_graphics &&
              after.requested_graphics->output_extent == requested.graphics.output_extent,
          "unrelated settings commit does not restore saved display overrides");
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

void TestFailedApplyReplaysOnlyLaterEdits()
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
              controller.Requested().graphics.bloom &&
              !controller.Requested().graphics.vsync &&
              controller.UiInput().bloom && !controller.UiInput().vsync,
          "failed bloom edit is dropped while later vsync edit survives");
    const auto next = controller.NextRequest();
    Check(next && next->effective.bloom && !next->effective.vsync &&
              controller.Acknowledge({next->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              controller.Committed().graphics.bloom &&
              !controller.Committed().graphics.vsync &&
              controller.Committed().graphics.hdr_mode == hs::GraphicsHdrMode::On &&
              controller.Committed().opaque_optional_json ==
                  R"({"graphics":{"future_key":"keep"}})",
          "next request applies only later edit and retains unrelated requests");
}

void TestClickPreviewWithinOnePump()
{
    const auto original = hs::LegacyDefaultSettingsDocument();
    hs::GraphicsSettingsCommit controller(
        original, hs::ProjectSettingsDocument(original), false);
    hs::BoundedSpscQueue<hs::UiCommand, 8> commands;
    auto preview = controller.UiInput();
    const hs::UiCommand first{hs::UiCommandKind::SetBloom,
                              static_cast<std::uint32_t>(!preview.bloom)};
    Check(commands.TryPush(first), "queue first click");
    hs::GraphicsSettingsCommit::PreviewUiCommand(preview, first);
    const hs::UiCommand second{hs::UiCommandKind::SetBloom,
                               static_cast<std::uint32_t>(!preview.bloom)};
    Check(commands.TryPush(second) && first.value != second.value,
          "second click in one pump uses first queued command");
    hs::GraphicsSettingsCommit::PreviewUiCommand(preview, second);
    hs::UiCommand command;
    while (commands.TryPop(command)) controller.EditGraphics(command);
    Check(preview.bloom && controller.UiInput().bloom &&
              controller.Effective().bloom,
          "two queued toggles return requested value to the starting value");

    controller.EditGraphics({hs::UiCommandKind::SetBloom, 0});
    const auto pending = controller.NextRequest();
    Check(pending && !controller.UiInput().bloom && controller.Effective().bloom,
          "pending request changes click input without changing applied display");
    preview = controller.UiInput();
    const hs::UiCommand later{hs::UiCommandKind::SetBloom,
                              static_cast<std::uint32_t>(!preview.bloom)};
    hs::GraphicsSettingsCommit::PreviewUiCommand(preview, later);
    controller.EditGraphics(later);
    Check(controller.Acknowledge({pending->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Applied,
          "first pending toggle applies");
    const auto opposite = controller.NextRequest();
    Check(opposite && opposite->effective.bloom,
          "toggle before acknowledgment queues opposite value after first apply");
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
    controller.EditGraphics({hs::UiCommandKind::SetBorderless, 0});
    controller.SetDesiredOutputExtent(1280, 720);
    const auto rollback = controller.NextRequest();
    Check(rollback && !rollback->effective.borderless &&
              rollback->effective.width == 1280 &&
              controller.Acknowledge({rollback->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              controller.Committed().graphics.display_mode ==
                  hs::GraphicsDisplayMode::Windowed,
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
              controller.UiInput().bloom && followup &&
              followup->effective.borderless && queued_followups.TryPush(*followup),
          "deferred display click survives an earlier failed graphics request");
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

void TestMigration()
{
    const auto root = NewRoot("migration");
    hs::SaveStore store(root);
    hs::SettingsData old;
    old.width = 1600;
    old.height = 900;
    old.borderless = true;
    old.vsync = false;
    old.frame_cap = 120;
    old.render_scale_percent = 75;
    old.shadow_resolution = 2048;
    old.particle_percentage = 50;
    old.bloom = false;
    old.outline = false;
    old.master_volume = 0.3f;
    old.skill_virtual_keys = {'W', 'Q', 'E', 'R'};
    Check(store.SaveSettings(old).Succeeded(), "write valid v1");
    hs::SettingsData old_roundtrip;
    Check(store.LoadSettings(old_roundtrip).Succeeded() &&
              old_roundtrip.width == old.width && old_roundtrip.frame_cap == 120,
          "legacy API still reads valid v1");

    hs::SettingsDocument migrated;
    Check(store.LoadSettingsV2(migrated).Succeeded(), "load v1 into v2");
    Check(migrated.graphics.preset == hs::GraphicsPreset::Custom &&
              migrated.graphics.display_mode ==
                  hs::GraphicsDisplayMode::BorderlessFullscreen &&
              migrated.graphics.output_extent == hs::GraphicsExtent{1600, 900} &&
              !migrated.graphics.vsync && migrated.graphics.frame_limit_fps == 120 &&
              migrated.graphics.resolution_mode == hs::GraphicsResolutionMode::Manual &&
              migrated.graphics.reconstruction_provider.value == "internal_taau" &&
              migrated.graphics.manual_scale_percent == 75.0f &&
              migrated.graphics.frame_generation_mode ==
                  hs::GraphicsFrameGenerationMode::Off &&
              migrated.graphics.shadow_quality == hs::GraphicsQuality::High &&
              migrated.graphics.particle_quality == hs::GraphicsQuality::Low &&
              !migrated.graphics.bloom && !migrated.graphics.outline &&
              migrated.master_volume == old.master_volume &&
              migrated.skill_virtual_keys == old.skill_virtual_keys,
          "exact v1 migration mappings");
    Check(Read(root / "settings.json").find("\"schema_version\": 1") !=
              std::string::npos,
          "load migration leaves v1 on disk until explicit save");
    hs::ApplySettingsEdit(migrated, {hs::UiCommandKind::SetBloom, 1});
    Check(store.SaveSettingsV2(migrated).Succeeded(), "persist migrated v2");
    hs::SettingsDocument restarted;
    Check(store.LoadSettingsV2(restarted).Succeeded() &&
              restarted.graphics.bloom &&
              restarted.graphics.output_extent == hs::GraphicsExtent{1600, 900} &&
              restarted.graphics.manual_scale_percent == 75.0f &&
              restarted.master_volume == old.master_volume &&
              restarted.skill_virtual_keys == old.skill_virtual_keys,
          "migrated v1 UI edit survives schema-2 restart");
    const auto bytes = Read(root / "settings.json");
    const auto backup = Read(root / "settings.json.bak");
    hs::SettingsData legacy;
    Check(!store.LoadSettings(legacy) && !store.SaveSettings(old),
          "legacy API rejects v2 without destructive recovery");
    Check(Read(root / "settings.json") == bytes &&
              Read(root / "settings.json.bak") == backup &&
              CorruptCount(root) == 0,
          "legacy API preserves v2 primary and backup");
    std::filesystem::remove_all(root);
}

void TestV2RoundTrip()
{
    const auto root = NewRoot("roundtrip");
    hs::SaveStore store(root);
    hs::SettingsDocument settings;
    settings.graphics.display_output.value = "monitor_2";
    settings.graphics.output_extent = hs::GraphicsExtent{1920, 1080};
    settings.graphics.hdr_peak_nits = 1000.0f;
    settings.graphics.reconstruction_provider.value = "future_provider";
    settings.graphics.reconstruction_quality.value = "future_quality_mode";
    settings.graphics.frame_generation_provider.value = "future_fg";
    settings.graphics.frame_generation_mode = hs::GraphicsFrameGenerationMode::Dynamic;
    settings.graphics.ao_mode = hs::GraphicsAoMode::RTAO;
    settings.graphics.dynamic_resolution = true;
    settings.graphics.dynamic_min_percent = 40.0f;
    settings.graphics.dynamic_max_percent = 90.0f;
    settings.graphics.bloom_strength = 1.5f;
    settings.bgm_volume = 0.25f;
    settings.screen_overlay_intensity = 0.35f;
    settings.skill_virtual_keys = {'R', 'W', 'E', 'Q'};
    settings.opaque_optional_json =
        R"({"top":{"future_field":{"token":"future_value"}},"graphics":{"future_quality":"novel"}})";
    Check(store.SaveSettingsV2(settings).Succeeded(), "save complete v2");
    const auto text = Read(root / "settings.json");
    for (const auto &descriptor : hs::GraphicsSettingDescriptors())
        Check(text.find("\"" + std::string(descriptor.key) + "\"") !=
                  std::string::npos,
              "every graphics definition serialized");
    hs::SettingsDocument loaded;
    Check(store.LoadSettingsV2(loaded).Succeeded() &&
              loaded.graphics.display_output.value == "monitor_2" &&
              loaded.graphics.output_extent == hs::GraphicsExtent{1920, 1080} &&
              loaded.graphics.hdr_peak_nits == 1000.0f &&
              loaded.graphics.reconstruction_provider.value == "future_provider" &&
              loaded.graphics.reconstruction_quality.value == "future_quality_mode" &&
              loaded.graphics.frame_generation_mode ==
                  hs::GraphicsFrameGenerationMode::Dynamic &&
              loaded.graphics.ao_mode == hs::GraphicsAoMode::RTAO &&
              loaded.graphics.dynamic_resolution &&
              loaded.graphics.dynamic_min_percent == 40.0f &&
              loaded.graphics.dynamic_max_percent == 90.0f &&
              loaded.bgm_volume == 0.25f &&
              loaded.screen_overlay_intensity == 0.35f &&
              loaded.skill_virtual_keys == settings.skill_virtual_keys,
          "typed v2 roundtrip including symbolic provider and mode");
    Check(loaded.opaque_optional_json.find("future_quality") != std::string::npos &&
              loaded.opaque_optional_json.find("future_field") != std::string::npos,
          "unknown optional fields retained");
    hs::ApplySettingsEdit(loaded, {hs::UiCommandKind::SetVsync, 0});
    Check(store.SaveSettingsV2(loaded).Succeeded(), "save edited v2");
    hs::SettingsDocument edited;
    Check(store.LoadSettingsV2(edited).Succeeded() &&
              !edited.graphics.vsync &&
              edited.graphics.frame_generation_mode ==
                  hs::GraphicsFrameGenerationMode::Dynamic &&
              edited.graphics.reconstruction_provider.value == "future_provider" &&
              edited.graphics.ao_mode == hs::GraphicsAoMode::RTAO &&
              edited.opaque_optional_json.find("future_quality") != std::string::npos,
          "UI edit preserves unsupported requests and unknown fields across restart");
    edited.opaque_optional_json = R"({"graphics":{"future_quality":"latest"}})";
    Check(store.SaveSettingsV2(edited).Succeeded(), "update optional value and retain older backup");
    Check(store.SaveSettingsV2(hs::SettingsDocument{}).Succeeded() &&
              Read(root / "settings.json").find("future_quality") != std::string::npos,
          "save merges unknown optional fields from current v2");
    Check(store.LoadSettingsV2(edited).Succeeded() &&
              edited.opaque_optional_json.find("latest") != std::string::npos,
          "primary optional value takes precedence over an older backup");
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
    auto original = hs::LegacyDefaultSettingsDocument();
    original.graphics.reconstruction_provider.value = "saved_unavailable_provider";
    original.graphics.display_output.value = "saved_monitor";
    original.graphics.frame_limit_fps = 120;
    hs::GraphicsSettingsCommit controller(original, hs::ProjectSettingsDocument(original), false);
    Check(controller.EditGraphics(GraphicsCommand(hs::SettingId::Preset, "ultra_rt")) &&
              controller.Requested().graphics.preset == hs::GraphicsPreset::UltraRT &&
              controller.Requested().graphics.gi_mode == hs::GraphicsGiMode::RayTraced &&
              controller.Requested().graphics.shadow_mode == hs::GraphicsShadowMode::HybridRT &&
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
              controller.EditGraphics(GraphicsCommand(hs::SettingId::GiQuality, "high")),
          "coalesced preset and later field edit");
    Check(controller.EditImmediate({hs::UiCommandKind::SetMasterVolumePercent, 40}) &&
              first->graphics->reconstruction_provider.value == immutable_provider &&
              first->effective.master_volume == 1,
          "in-flight native and effective snapshots stay immutable during audio edits");
    Check(controller.Acknowledge({first->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              controller.Requested().graphics.preset == hs::GraphicsPreset::Custom &&
              controller.Requested().graphics.gi_mode == hs::GraphicsGiMode::Probe &&
              controller.Requested().graphics.gi_quality == hs::GraphicsQuality::High &&
              controller.Requested().master_volume == 0.4f,
          "follow-ups preserve actual serial order and immediate audio");
    controller.RollbackUnpersisted();
    const auto recovery = controller.NextRequest();
    Check(recovery && recovery->graphics->gi_mode == original.graphics.gi_mode &&
              recovery->effective.master_volume == 0.4f &&
              controller.Acknowledge({recovery->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Restored &&
              controller.Committed().graphics.gi_mode == original.graphics.gi_mode &&
              controller.Requested().graphics.gi_quality == hs::GraphicsQuality::High,
          "save recovery restores acknowledged baseline and retains newer intent/audio");
    const auto later = controller.NextRequest();
    Check(later.has_value(), "newer intent follows recovery");
    const auto *packet = later->graphics;
    controller.MarkTimedOut();
    Check(controller.HasPending() && controller.TimedOut() && packet == later->graphics &&
              !controller.NextRequest(), "timeout retains native read lease and blocks replacement");
    Check(controller.Acknowledge({later->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Recovering,
          "late applied acknowledgment schedules a real baseline restore");
    const auto timed_recovery = controller.NextRequest();
    Check(timed_recovery && timed_recovery->generation > later->generation &&
              controller.Acknowledge({later->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Ignored,
          "stale receipt cannot replace recovery generation");
    Check(controller.Acknowledge({timed_recovery->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Restored,
          "timeout restore is acknowledged");
    Check(controller.EditGraphics({hs::UiCommandKind::SetVfxScreenOverlayIntensityPercent, 30}),
          "accessibility option uses renderer transaction");
    const auto overlay = controller.NextRequest();
    Check(overlay && overlay->effective.screen_overlay_intensity == 0.3f &&
              controller.Acknowledge({overlay->generation, hs::ErrorCode::None}) ==
                  hs::GraphicsSettingsCommit::AckOutcome::Applied &&
              hs::ProjectRendererConfig(controller.Effective(), controller.Committed().graphics).screen_overlay_intensity == 0.3f,
          "overlay intensity reaches native options only through matching apply");
}

void TestOptionalAccessibilitySchema()
{
    const auto root = NewRoot("accessibility");
    hs::SaveStore store(root);
    Write(root / "settings.json", R"({"schema_version":2,"graphics":{},"master_volume":1,"bgm_volume":1,"sfx_volume":1,"ui_volume":1,"skill_virtual_keys":[81,87,69,82]})");
    hs::SettingsDocument loaded;
    Check(store.LoadSettingsV2(loaded) && loaded.screen_overlay_intensity == 1,
          "missing schema-2 accessibility field defaults to one");
    loaded.screen_overlay_intensity = 1.1f;
    Check(!store.SaveSettingsV2(loaded), "out of range overlay intensity is rejected");
    std::filesystem::remove_all(root);
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

    const std::string malformed =
        R"({"schema_version":2,"graphics":{"vsync":true,"vsync":false},"master_volume":1,"bgm_volume":1,"sfx_volume":1,"ui_volume":1,"skill_virtual_keys":[81,87,69,82]})";
    Write(root / "settings.json", malformed);
    hs::SettingsDocument recovered;
    Check(store.LoadSettingsV2(recovered).Succeeded() &&
              recovered.graphics.vsync && CorruptCount(root) == 1,
          "duplicate field rejected and preserved as corrupt");
    Write(root / "settings.json",
          R"({"schema_version":2,"graphics":{"vsync":"on"},"master_volume":1,"bgm_volume":1,"sfx_volume":1,"ui_volume":1,"skill_virtual_keys":[81,87,69,82]})");
    Check(store.LoadSettingsV2(recovered).Succeeded() && CorruptCount(root) == 2,
          "known graphics field with wrong type rejected");
    Write(root / "settings.json",
          R"({"schema_version":2,"graphics":{},"master_volume":1,"bgm_volume":1,"sfx_volume":1,"ui_volume":1,"skill_virtual_keys":[81,87,69,82]})");
    Check(store.LoadSettingsV2(recovered).Succeeded() &&
              recovered.graphics.vsync &&
              recovered.graphics.shadow_quality == hs::GraphicsQuality::Medium &&
              CorruptCount(root) == 2,
          "missing optional graphics keys use registry defaults");
    const std::string oversized(1024 * 1024 + 1, 'x');
    Write(root / "settings.json", oversized);
    Check(!store.LoadSettingsV2(recovered) &&
              Read(root / "settings.json") == oversized && CorruptCount(root) == 2,
          "oversized settings rejected without modifying the file");
    Write(root / "settings.json", R"({"schema_version":"3","graphics":{}})");
    Check(store.LoadSettingsV2(recovered).Succeeded() && CorruptCount(root) == 3,
          "malformed schema type is corrupt rather than future");

    const std::string future =
        R"({"schema_version":3,"graphics":{},"unknown":"keep","unknown":"keep2"})";
    const auto primary = root / "settings.json";
    const auto backup = root / "settings.json.bak";
    Write(primary, future);
    const auto backup_before = std::filesystem::exists(backup) ? Read(backup) : "";
    hs::SettingsData old_reader;
    Check(!store.LoadSettingsV2(recovered) &&
              !store.SaveSettingsV2(hs::SettingsDocument{}) &&
              !store.LoadSettings(old_reader),
          "future primary rejected by both APIs");
    Check(Read(primary) == future &&
              (std::filesystem::exists(backup) ? Read(backup) : "") == backup_before &&
              CorruptCount(root) == 3,
          "future primary and backup untouched");
    const std::string large_future =
        R"({"schema_version":3,"payload":")" + std::string(1024 * 1024, 'x') + "\"}";
    Write(primary, large_future);
    Check(!store.LoadSettingsV2(recovered) &&
              !store.SaveSettingsV2(hs::SettingsDocument{}) &&
              Read(primary) == large_future && CorruptCount(root) == 3,
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
        TestLoadSourceAndProjection();
        TestRequestedEditPreservesUnimplementedFields();
        TestStartupGraphicsRendererProjection();
        TestGraphicsApplyAcknowledgments();
        TestFailedApplyReplaysOnlyLaterEdits();
        TestClickPreviewWithinOnePump();
        TestDisplayIntentWaitsForConfirmation();
        TestDeferredDisplaySaveBoundary();
        TestMigration();
        TestV2RoundTrip();
        TestFullGraphicsTransactions();
        TestOptionalAccessibilitySchema();
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
