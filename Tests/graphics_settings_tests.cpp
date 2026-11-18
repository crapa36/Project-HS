#include <hs/runtime/settings_projection.hpp>
#include <hs/core/bounded_spsc_queue.hpp>

#include "../Source/Runtime/Private/graphics_settings_commit.hpp"
#include "../Source/Runtime/Private/renderer_settings_projection.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{

void Check(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

void TestGraphicsApplyAcknowledgments()
{
    auto original = hs::LegacyDefaultSettingsDocument();
    original.graphics.hdr_mode = hs::GraphicsHdrMode::On;
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
    controller.EditGraphics({hs::UiCommandKind::SetVsync, 0});
    const auto second = controller.NextRequest();
    Check(second && requests.TryPush(*second), "queue second graphics request");
    controller.EditImmediate({hs::UiCommandKind::SetMasterVolumePercent, 40});
    Check(controller.Committed().master_volume == 0.4f &&
              controller.Committed().graphics.vsync,
          "immediate audio state commits while graphics request remains pending");
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

}

void TestFailedApplyKeepsLatestDesiredSettings()
{
    auto original = hs::LegacyDefaultSettingsDocument();
    original.graphics.hdr_mode = hs::GraphicsHdrMode::On;
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
              controller.Committed().graphics.hdr_mode == hs::GraphicsHdrMode::On,
          "next request applies the full latest desired settings");
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
    Check(controller.EditGraphics({hs::UiCommandKind::SetBloom, 0}) &&
              controller.EditGraphics(GraphicsCommand(hs::SettingId::Bloom, "true")) &&
              controller.Requested().graphics.bloom &&
              controller.EditGraphics(GraphicsCommand(hs::SettingId::Preset, "ultra")) &&
              controller.Requested().graphics.preset == hs::GraphicsPreset::Ultra &&
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
              controller.Requested().graphics.reflection_quality == hs::GraphicsQuality::High &&
              controller.Requested().master_volume == 0.4f,
          "follow-ups preserve actual serial order and immediate audio");
    const auto preset_followup = controller.NextRequest();
    Check(preset_followup && controller.Acknowledge({preset_followup->generation, hs::ErrorCode::None}) ==
              hs::GraphicsSettingsCommit::AckOutcome::Applied,
          "ordered graphics follow-ups apply after the first acknowledgment");
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
}


} // namespace

void TestGraphicsSettings()
{
    TestGraphicsApplyAcknowledgments();
    TestFailedApplyKeepsLatestDesiredSettings();
    TestDisplayIntentWaitsForConfirmation();
    TestFullGraphicsTransactions();
}
