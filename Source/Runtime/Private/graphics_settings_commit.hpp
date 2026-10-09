#pragma once

#include <hs/core/graphics_settings_request.hpp>
#include <hs/runtime/settings_projection.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
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
          mute_audio_(mute_audio) {}

    [[nodiscard]] SettingsData &Effective() noexcept { return effective_; }
    [[nodiscard]] const SettingsData &UiInput() const noexcept { return desired_effective_; }
    [[nodiscard]] const SettingsDocument &Requested() const noexcept { return requested_; }
    [[nodiscard]] const SettingsDocument &Committed() const noexcept { return committed_; }
    [[nodiscard]] bool HasPending() const noexcept { return pending_generation_ != 0; }
    [[nodiscard]] bool HasUnsentEdits() const noexcept { return graphics_dirty_ || recovery_requested_; }
    [[nodiscard]] bool IsRecovering() const noexcept { return recovery_requested_ || in_flight_recovery_; }
    [[nodiscard]] bool TimedOut() const noexcept { return timed_out_; }
    [[nodiscard]] bool PreparationFailed() const noexcept { return preparation_failed_; }
    void SuspendUnsentAfterAllocationFailure() noexcept { preparation_failed_ = true; }
    [[nodiscard]] bool FollowupRejected() const noexcept { return followup_rejected_; }
    [[nodiscard]] std::uint64_t CommittedGeneration() const noexcept { return committed_generation_; }
    [[nodiscard]] const GraphicsSettings &ApplyTargetGraphics() const noexcept
    { return recovery_requested_ ? persisted_.graphics : requested_.graphics; }
    [[nodiscard]] const SettingsData &ApplyTargetEffective() const noexcept
    { return recovery_requested_ ? persisted_effective_ : desired_effective_; }

    void EditDisplayOutput(GraphicsDisplayId output)
    {
        auto candidate = requested_;
        candidate.graphics.display_output = output;
        candidate.graphics.preset = GraphicsPreset::Custom;
        if (!ValidateGraphicsSettings(candidate.graphics).empty()) return;
        requested_ = std::move(candidate);
        followup_display_output_ = std::move(output);
        graphics_dirty_ = true;
        preparation_failed_ = false;
    }

    void EditDisplayIntent(const GraphicsSettings &graphics)
    {
        EditDisplayOutput(graphics.display_output);
        requested_.graphics.output_extent = graphics.output_extent;
        followup_display_extent_ = graphics.output_extent;
        (void)EditGraphics({UiCommandKind::SetBorderless,
            graphics.display_mode == GraphicsDisplayMode::BorderlessFullscreen ? 1u : 0u});
    }

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

    // Validate before changing either intent or a coalesced slot. The slot key is
    // the actual setting ID, and serial order preserves preset/field ordering.
    bool EditGraphics(const UiCommand &command)
    {
        const auto index = GraphicsEditIndex(command);
        if (!index) return false;
        auto candidate = requested_;
        if (!ApplySettingsEdit(candidate, command)) return false;
        auto projection = ProjectSettingsDocument(candidate);
        projection.width = desired_effective_.width;
        projection.height = desired_effective_.height;
        if (mute_audio_) projection.master_volume = 0;
        requested_ = std::move(candidate);
        desired_effective_ = projection;
        followup_edits_[*index] = PendingEdit{command, ++edit_serial_};
        graphics_dirty_ = true;
        preparation_failed_ = false;
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
        if (HasPending() || !HasUnsentEdits() || preparation_failed_) return std::nullopt;
        // All allocating copies precede publication. OOM leaves the previous
        // snapshot and requested intent intact, with no outstanding read lease.
        auto document = recovery_requested_ ? persisted_ : requested_;
        CopyImmediate(requested_, document);
        auto native_graphics = recovery_requested_ ? document.graphics : graphics;
        auto projection = recovery_requested_ ? persisted_effective_ : desired_effective_;
        CopyImmediateProjection(requested_, projection);
        in_flight_document_ = std::move(document);
        in_flight_graphics_ = std::move(native_graphics);
        in_flight_effective_ = projection;
        in_flight_recovery_ = recovery_requested_;
        recovery_requested_ = false;
        timed_out_ = false;
        pending_generation_ = ++next_generation_;
        graphics_dirty_ = false;
        if (!in_flight_recovery_) ClearFollowups();
        const auto cause = in_flight_recovery_ ? GraphicsSettingsRequestCause::Recovery
                                               : GraphicsSettingsRequestCause::User;
        return GraphicsSettingsCommitRequest{pending_generation_, projection, &in_flight_graphics_, cause};
    }

    void RejectUnqueuedRequest() noexcept
    {
        if (!HasPending()) return;
        pending_generation_ = 0;
        recovery_requested_ = in_flight_recovery_;
        in_flight_recovery_ = false;
        graphics_dirty_ = true;
    }

    void DiscardUnsentEdits()
    {
        if (HasPending()) return;
        auto restored = committed_;
        requested_ = std::move(restored);
        desired_effective_ = effective_;
        graphics_dirty_ = recovery_requested_ = false;
        ClearFollowups();
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

    // A failed save needs a real acknowledged renderer recovery. Subsequent
    // edits remain coalesced and immediate audio/keys remain current.
    void RollbackUnpersisted() noexcept
    {
        if (HasPending()) return;
        // Recording recovery cannot allocate after a failed save. Its snapshot
        // is prepared by NextRequest; follow-ups replay only when the actual
        // restoration is acknowledged, within Acknowledge's strong transaction.
        recovery_requested_ = true;
        preparation_failed_ = false;
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
        const auto was_recovery = in_flight_recovery_;
        const auto was_timeout = timed_out_;
        // Replay allocates only before completing the main-thread commit. A
        // caller can retain and retry this ack if its UI/document allocation fails.
        ReplayFollowupEdits(was_timeout ? persisted_ : next_committed,
                            was_timeout ? persisted_effective_ : next_effective);
        committed_ = std::move(next_committed);
        effective_ = next_effective;
        if (ack.error == ErrorCode::None) committed_generation_ = ack.generation;
        pending_generation_ = 0;
        in_flight_recovery_ = false;
        timed_out_ = false;
        if (was_timeout && ack.error == ErrorCode::None) recovery_requested_ = true;
        if (ack.error != ErrorCode::None) return AckOutcome::Rejected;
        if (was_timeout) return AckOutcome::Recovering;
        return was_recovery ? AckOutcome::Restored : AckOutcome::Applied;
    }

  private:
    struct PendingEdit { UiCommand command; std::uint64_t serial; };
    static constexpr std::size_t kEditSlots = 8 + kGraphicsSettingCount + 1;

    [[nodiscard]] static std::optional<std::size_t> GraphicsEditIndex(const UiCommand &command) noexcept
    {
        switch (command.kind)
        {
        case UiCommandKind::SetBorderless: return 0;
        case UiCommandKind::SetVsync: return 1;
        case UiCommandKind::SetFrameCap: return 2;
        case UiCommandKind::SetRenderScale: return 3;
        case UiCommandKind::SetShadowResolution: return 4;
        case UiCommandKind::SetParticlePercentage: return 5;
        case UiCommandKind::SetBloom: return 6;
        case UiCommandKind::SetOutline: return 7;
        case UiCommandKind::SetVfxScreenOverlayIntensityPercent: return kEditSlots - 1;
        case UiCommandKind::SetGraphicsSetting:
            for (std::size_t i = 0; i < GraphicsSettingDescriptors().size(); ++i)
                if (GraphicsSettingDescriptors()[i].id == command.graphics_setting) return 8 + i;
            return std::nullopt;
        default: return std::nullopt;
        }
    }

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

    void ClearFollowups() noexcept
    {
        for (auto &edit : followup_edits_) edit.reset();
        followup_display_output_.reset();
        followup_display_extent_.reset();
    }

    void ReplayFollowupEdits(const SettingsDocument &base, SettingsData projection)
    {
        auto candidate = base;
        CopyImmediate(requested_, candidate);
        if (followup_display_output_) candidate.graphics.display_output = *followup_display_output_;
        if (followup_display_extent_) candidate.graphics.output_extent = *followup_display_extent_;
        std::array<std::size_t, kEditSlots> order{};
        std::size_t count{};
        for (std::size_t i = 0; i < kEditSlots; ++i) if (followup_edits_[i]) order[count++] = i;
        std::sort(order.begin(), order.begin() + count, [&](auto a, auto b) {
            return followup_edits_[a]->serial < followup_edits_[b]->serial;
        });
        bool rejected{};
        for (std::size_t i = 0; i < count; ++i)
            if (!ApplySettingsEdit(candidate, followup_edits_[order[i]]->command)) rejected = true;
        auto projected = ProjectSettingsDocument(candidate);
        projected.width = projection.width;
        projected.height = projection.height;
        CopyImmediateProjection(candidate, projected);
        requested_ = std::move(candidate);
        desired_effective_ = projected;
        followup_rejected_ = rejected;
        graphics_dirty_ = count != 0 || followup_display_output_.has_value() || followup_display_extent_.has_value();
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
        case UiCommandKind::SetOutline: effective.outline = command.value != 0; break;
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
    std::array<std::optional<PendingEdit>, kEditSlots> followup_edits_{};
    std::optional<GraphicsDisplayId> followup_display_output_;
    std::optional<GraphicsOutputExtent> followup_display_extent_;
    std::uint64_t edit_serial_{}, next_generation_{}, pending_generation_{}, committed_generation_{};
    bool graphics_dirty_{}, mute_audio_{}, recovery_requested_{}, in_flight_recovery_{};
    bool timed_out_{}, followup_rejected_{}, preparation_failed_{};
};

} // namespace hs
