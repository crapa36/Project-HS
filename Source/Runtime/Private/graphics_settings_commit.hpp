#pragma once

#include <hs/core/graphics_settings_request.hpp>
#include <hs/runtime/settings_projection.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace hs
{

struct GraphicsSettingsCommitRequest
{
    std::uint64_t generation{};
    SettingsData effective{};
    // Immutable until the matching acknowledgment. A timeout cannot end this
    // render-thread read lease; Runtime joins that thread before destruction.
    const GraphicsSettings *graphics{};
    GraphicsSettingsRequestCause cause{GraphicsSettingsRequestCause::User};
};
struct GraphicsSettingsAck
{
    std::uint64_t generation{};
    ErrorCode error{ErrorCode::None};
};
static_assert(std::is_trivially_copyable_v<GraphicsSettingsCommitRequest>);
static_assert(std::is_trivially_copyable_v<GraphicsSettingsAck>);

class GraphicsSettingsCommit
{
  public:
    enum class AckOutcome { Ignored, Applied, Rejected, Restored, Recovering };

    GraphicsSettingsCommit(SettingsDocument requested, SettingsData effective, bool mute_audio)
        : requested_(std::move(requested)), committed_(requested_), persisted_(requested_),
          effective_(effective), desired_effective_(effective), persisted_effective_(effective),
          mute_audio_(mute_audio)
    {
        if (mute_audio_) effective_.master_volume = desired_effective_.master_volume = 0;
    }

    [[nodiscard]] SettingsData &Effective() noexcept { return effective_; }
    [[nodiscard]] const SettingsData &UiInput() const noexcept { return desired_effective_; }
    [[nodiscard]] const SettingsDocument &Requested() const noexcept { return requested_; }
    [[nodiscard]] const SettingsDocument &Committed() const noexcept { return committed_; }
    [[nodiscard]] bool HasPending() const noexcept { return pending_generation_ != 0; }
    [[nodiscard]] bool HasUnsentEdits() const noexcept { return graphics_dirty_ || timeout_restore_requested_; }
    [[nodiscard]] bool IsRecovering() const noexcept { return timeout_restore_requested_ || in_flight_timeout_restore_; }
    [[nodiscard]] bool TimedOut() const noexcept { return timed_out_; }
    [[nodiscard]] std::uint64_t CommittedGeneration() const noexcept { return committed_generation_; }
    [[nodiscard]] const GraphicsSettings &ApplyTargetGraphics() const noexcept
    { return timeout_restore_requested_ ? persisted_.graphics : requested_.graphics; }
    [[nodiscard]] const SettingsData &ApplyTargetEffective() const noexcept
    { return timeout_restore_requested_ ? persisted_effective_ : desired_effective_; }

    void SetDesiredOutputExtent(std::uint32_t width, std::uint32_t height) noexcept
    {
        desired_effective_.width = width;
        desired_effective_.height = height;
        if (next_generation_ == 0 && !graphics_dirty_ && !HasPending())
        {
            persisted_effective_.width = width;
            persisted_effective_.height = height;
        }
    }

    bool EditGraphics(const UiCommand &command)
    {
        auto canonical = CanonicalGraphicsEdit(command);
        if (!canonical) return false;
        auto candidate = requested_;
        if (!ApplySettingsEdit(candidate, *canonical)) return false;
        auto projection = ProjectSettingsDocument(candidate);
        projection.width = desired_effective_.width;
        projection.height = desired_effective_.height;
        if (mute_audio_) projection.master_volume = 0;
        requested_ = std::move(candidate);
        desired_effective_ = projection;
        graphics_dirty_ = true;
        return true;
    }

    static void PreviewUiCommand(SettingsData &preview, const UiCommand &command,
                                 bool mute_audio = false)
    {
        ApplyEffectiveEdit(preview, command);
        if (mute_audio) preview.master_volume = 0;
    }

    bool EditImmediate(const UiCommand &command)
    {
        if (command.kind != UiCommandKind::SetMasterVolumePercent &&
            command.kind != UiCommandKind::SetBgmVolumePercent &&
            command.kind != UiCommandKind::SetSfxVolumePercent &&
            command.kind != UiCommandKind::SetUiVolumePercent) return false;
        auto candidate = requested_;
        if (!ApplySettingsEdit(candidate, command)) return false;
        requested_ = std::move(candidate);
        CopyImmediate(requested_, committed_);
        ApplyEffectiveEdit(effective_, command);
        ApplyEffectiveEdit(desired_effective_, command);
        if (mute_audio_) effective_.master_volume = desired_effective_.master_volume = 0;
        return true;
    }

    void RebindKeys(const std::array<std::uint16_t, 4> &keys)
    {
        requested_.skill_virtual_keys = committed_.skill_virtual_keys = keys;
        effective_.skill_virtual_keys = desired_effective_.skill_virtual_keys = keys;
    }

    [[nodiscard]] std::optional<GraphicsSettingsCommitRequest> NextRequest()
    {
        return NextRequest(requested_.graphics);
    }

    [[nodiscard]] std::optional<GraphicsSettingsCommitRequest> NextRequest(const GraphicsSettings &graphics)
    {
        if (HasPending() || !HasUnsentEdits()) return std::nullopt;
        // All allocating copies precede publication. OOM leaves the previous
        // snapshot and requested intent intact, with no outstanding read lease.
        auto document = timeout_restore_requested_ ? persisted_ : requested_;
        CopyImmediate(requested_, document);
        auto native_graphics = timeout_restore_requested_ ? document.graphics : graphics;
        auto projection = timeout_restore_requested_ ? persisted_effective_ : desired_effective_;
        CopyImmediateProjection(requested_, projection);
        in_flight_document_ = std::move(document);
        in_flight_graphics_ = std::move(native_graphics);
        in_flight_effective_ = projection;
        in_flight_timeout_restore_ = timeout_restore_requested_;
        timeout_restore_requested_ = false;
        timed_out_ = false;
        pending_generation_ = ++next_generation_;
        if (!in_flight_timeout_restore_) graphics_dirty_ = false;
        return GraphicsSettingsCommitRequest{pending_generation_, projection, &in_flight_graphics_,
            in_flight_timeout_restore_ ? GraphicsSettingsRequestCause::Recovery
                                        : GraphicsSettingsRequestCause::User};
    }

    void RejectUnqueuedRequest() noexcept
    {
        if (!HasPending()) return;
        pending_generation_ = 0;
        timeout_restore_requested_ = in_flight_timeout_restore_;
        in_flight_timeout_restore_ = false;
        graphics_dirty_ = true;
    }

    void DiscardUnsentEdits()
    {
        if (HasPending()) return;
        auto restored = committed_;
        requested_ = std::move(restored);
        desired_effective_ = effective_;
        graphics_dirty_ = timeout_restore_requested_ = false;
    }

    // Called only after a successful schema-2 atomic save.
    void ConfirmPersisted()
    {
        ConfirmPersisted(committed_);
    }

    // Runtime prepares this copy before writing. Publication after a successful
    // file replacement cannot allocate or turn a saved document into a failure.
    void ConfirmPersisted(SettingsDocument document) noexcept
    {
        persisted_ = std::move(document);
        persisted_effective_ = effective_;
    }

    // Display confirmation cancel restores the accepted display fields while
    // preserving unrelated graphics edits and the current client extent.
    void RestoreDisplay(const GraphicsSettings &accepted_display)
    {
        if (HasPending()) return;
        auto candidate = requested_;
        candidate.graphics.display_output = accepted_display.display_output;
        candidate.graphics.output_extent = accepted_display.output_extent;
        candidate.graphics.display_mode = accepted_display.display_mode;
        auto projection = ProjectSettingsDocument(candidate);
        projection.width = desired_effective_.width;
        projection.height = desired_effective_.height;
        if (mute_audio_) projection.master_volume = 0;
        requested_ = std::move(candidate);
        desired_effective_ = projection;
        graphics_dirty_ = true;
    }

    // Keep the immutable packet alive while waiting for a late acknowledgment.
    // Successful late application must be undone, then newer edits can proceed.
    void MarkTimedOut() noexcept { if (HasPending()) timed_out_ = true; }

    [[nodiscard]] AckOutcome Acknowledge(const GraphicsSettingsAck &ack)
    {
        if (!HasPending() || ack.generation != pending_generation_) return AckOutcome::Ignored;
        auto next_committed = ack.error == ErrorCode::None ? in_flight_document_ : committed_;
        auto next_effective = ack.error == ErrorCode::None ? in_flight_effective_ : effective_;
        CopyImmediate(requested_, next_committed);
        CopyImmediateProjection(requested_, next_effective);
        const auto was_timeout = timed_out_;
        const auto was_timeout_restore = in_flight_timeout_restore_;
        if (!graphics_dirty_)
        {
            auto next_requested = was_timeout ? persisted_ : next_committed;
            CopyImmediate(requested_, next_requested);
            auto next_desired = was_timeout ? persisted_effective_ : next_effective;
            CopyImmediateProjection(requested_, next_desired);
            requested_ = std::move(next_requested);
            desired_effective_ = next_desired;
        }
        committed_ = std::move(next_committed);
        effective_ = next_effective;
        if (ack.error == ErrorCode::None) committed_generation_ = ack.generation;
        pending_generation_ = 0;
        in_flight_timeout_restore_ = false;
        timed_out_ = false;
        if (was_timeout && ack.error == ErrorCode::None) timeout_restore_requested_ = true;
        if (ack.error != ErrorCode::None) return AckOutcome::Rejected;
        if (was_timeout) return AckOutcome::Recovering;
        return was_timeout_restore ? AckOutcome::Restored : AckOutcome::Applied;
    }

  private:
    static void CopyImmediate(const SettingsDocument &source, SettingsDocument &target) noexcept
    {
        target.master_volume = source.master_volume;
        target.bgm_volume = source.bgm_volume;
        target.sfx_volume = source.sfx_volume;
        target.ui_volume = source.ui_volume;
        target.skill_virtual_keys = source.skill_virtual_keys;
    }

    void CopyImmediateProjection(const SettingsDocument &source, SettingsData &target) const noexcept
    {
        target.master_volume = mute_audio_ ? 0 : source.master_volume;
        target.bgm_volume = source.bgm_volume;
        target.sfx_volume = source.sfx_volume;
        target.ui_volume = source.ui_volume;
        target.skill_virtual_keys = source.skill_virtual_keys;
    }

    static void ApplyEffectiveEdit(SettingsData &effective, const UiCommand &command) noexcept
    {
        switch (command.kind)
        {
        case UiCommandKind::SetBorderless: effective.borderless = command.value != 0; break;
        case UiCommandKind::SetVsync: effective.vsync = command.value != 0; break;
        case UiCommandKind::SetFrameCap: effective.frame_cap = command.value; break;
        case UiCommandKind::SetRenderScale: effective.render_scale_percent = command.value; break;
        case UiCommandKind::SetShadowResolution: effective.shadow_resolution = command.value; break;
        case UiCommandKind::SetParticlePercentage: effective.particle_percentage = command.value; break;
        case UiCommandKind::SetBloom: effective.bloom = command.value != 0; break;
        case UiCommandKind::SetMasterVolumePercent: effective.master_volume = command.value / 100.0f; break;
        case UiCommandKind::SetBgmVolumePercent: effective.bgm_volume = command.value / 100.0f; break;
        case UiCommandKind::SetSfxVolumePercent: effective.sfx_volume = command.value / 100.0f; break;
        case UiCommandKind::SetUiVolumePercent: effective.ui_volume = command.value / 100.0f; break;
        case UiCommandKind::SetVfxScreenOverlayIntensityPercent: effective.screen_overlay_intensity = command.value / 100.0f; break;
        default: break;
        }
    }

    SettingsDocument requested_, committed_, persisted_, in_flight_document_;
    GraphicsSettings in_flight_graphics_;
    SettingsData effective_, desired_effective_, persisted_effective_, in_flight_effective_;
    std::uint64_t next_generation_{}, pending_generation_{}, committed_generation_{};
    bool graphics_dirty_{}, mute_audio_{}, timeout_restore_requested_{}, in_flight_timeout_restore_{};
    bool timed_out_{};
};

} // namespace hs
