#pragma once

#include <hs/render_data/render_snapshot_values.hpp>
#include <hs/render_data/render_view_input.hpp>
#include <hs/core/settings.hpp>
#include <hs/presentation/projector.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace hs::runtime_detail
{
struct PresentedUiFrame
{
    std::shared_ptr<const UiFrame> frame;
    ViewExtent presented_extent{};
};

struct UiDrawPackets
{
    std::atomic<std::shared_ptr<const PresentedUiFrame>> presented_ui_packet;

    [[nodiscard]] bool AcceptPresentedUiPacket(std::uint64_t receipt_generation,
        std::shared_ptr<const UiFrame> frame, std::uint64_t session_id,
        ViewExtent extent) noexcept
    {
        if (!frame || !receipt_generation || receipt_generation != frame->generation ||
            frame->session_id != session_id ||
            !extent.width || !extent.height) return false;
        const auto accepted = presented_ui_packet.load(std::memory_order_acquire);
        if (accepted)
        {
            if (accepted->frame->generation > receipt_generation) return false;
            if (accepted->frame->generation == receipt_generation)
            {
                if (accepted->frame != frame) return false;
                if (accepted->presented_extent == extent) return true;
            }
        }
        try
        {
            auto receipt = std::make_shared<const PresentedUiFrame>(
                PresentedUiFrame{std::move(frame), extent});
            presented_ui_packet.store(std::move(receipt), std::memory_order_release);
            return true;
        }
        catch (...)
        {
            return false; // Keep the accepted owner if receipt allocation fails.
        }
    }

};

struct UiActionEnvelope
{
    UiAction action{};
    std::uint64_t packet_generation{};
    std::uint64_t session_id{};
    std::uint64_t selection_epoch{};
    SessionProbe displayed_session{};
};
static_assert(std::is_trivially_copyable_v<UiActionEnvelope>);

[[nodiscard]] inline std::uint64_t UiSelectionEpoch(const SessionProbe &session) noexcept
{
    std::uint64_t value = 0x9E3779B97F4A7C15ull ^ static_cast<std::uint64_t>(session.phase);
    auto mix = [&](std::uint64_t item) {
        value ^= item + 0x9E3779B97F4A7C15ull + (value << 6) + (value >> 2);
    };
    mix(session.card_count);
    mix(session.pending_stat_points);
    mix(session.level_rerolls_remaining);
    mix(session.relic_rerolls_remaining);
    for (std::size_t i = 0; i < session.card_count && i < session.cards.size(); ++i)
    {
        mix(static_cast<std::uint64_t>(session.cards[i].kind));
        mix(session.cards[i].subject);
        mix(session.cards[i].upgrade);
    }
    for (const auto points : session.stat_points) mix(points);
    return value ? value : 1;
}

[[nodiscard]] inline bool SameUiSelection(const SessionProbe &left,
                                         const SessionProbe &right) noexcept
{
    if (left.phase != right.phase || left.card_count != right.card_count ||
        left.level_rerolls_remaining != right.level_rerolls_remaining ||
        left.relic_rerolls_remaining != right.relic_rerolls_remaining ||
        left.pending_stat_points != right.pending_stat_points ||
        left.stat_points != right.stat_points) return false;
    for (std::size_t i = 0; i < left.card_count && i < left.cards.size(); ++i)
        if (left.cards[i].kind != right.cards[i].kind ||
            left.cards[i].subject != right.cards[i].subject ||
            left.cards[i].upgrade != right.cards[i].upgrade) return false;
    return true;
}

[[nodiscard]] inline bool UiSelectionPhase(SessionPhase phase) noexcept
{
    return phase == SessionPhase::CardSelection || phase == SessionPhase::RelicSelection ||
        phase == SessionPhase::StatAllocation;
}

[[nodiscard]] inline bool UiActionCurrent(const UiActionEnvelope &action,
    std::uint64_t current_session_id, const SessionProbe &current) noexcept
{
    if (!action.packet_generation || action.session_id != current_session_id ||
        action.selection_epoch != UiSelectionEpoch(action.displayed_session) ||
        action.displayed_session.phase != current.phase) return false;
    if (UiSelectionPhase(current.phase) && !SameUiSelection(current, action.displayed_session))
        return false;
    if (action.action.kind == UiActionKind::SelectCard)
    {
        const auto index = action.action.value;
        if (index >= current.card_count || index >= action.displayed_session.card_count) return false;
        const auto &now = current.cards[index];
        const auto &then = action.displayed_session.cards[index];
        if (now.kind != then.kind || now.subject != then.subject || now.upgrade != then.upgrade)
            return false;
    }
    if (action.action.kind == UiActionKind::SwapLoadoutSlots &&
        current.skill_loadout != action.displayed_session.skill_loadout) return false;
    return true;
}

[[nodiscard]] inline bool SameUiSettings(const SettingsData &a, const SettingsData &b) noexcept
{
    return a.schema_version == b.schema_version && a.width == b.width && a.height == b.height &&
        a.borderless == b.borderless && a.vsync == b.vsync && a.frame_cap == b.frame_cap &&
        a.render_scale_percent == b.render_scale_percent && a.shadow_resolution == b.shadow_resolution &&
        a.particle_percentage == b.particle_percentage && a.bloom == b.bloom && a.outline == b.outline &&
        a.master_volume == b.master_volume && a.bgm_volume == b.bgm_volume &&
        a.sfx_volume == b.sfx_volume && a.ui_volume == b.ui_volume &&
        a.skill_virtual_keys == b.skill_virtual_keys &&
        a.screen_overlay_intensity == b.screen_overlay_intensity;
}

[[nodiscard]] inline bool SameUiState(const PresentationUiState &a,
                                      const PresentationUiState &b) noexcept
{
    return a.page == b.page && a.selected_collection_skill == b.selected_collection_skill &&
        a.selected_character_skill == b.selected_character_skill &&
        a.loadout_source_slot == b.loadout_source_slot && a.settings_group == b.settings_group &&
        a.settings_page == b.settings_page && a.character_details_page == b.character_details_page;
}

[[nodiscard]] inline bool SameUiDrawInputs(const UiFrame &frame,
    std::span<const std::uint64_t> facts,
    const std::shared_ptr<const GraphicsSettingsUiModel> &graphics_model) noexcept
{
    const bool graphics_visible = frame.ui.page == UiPage::MainMenuSettings ||
        frame.ui.page == UiPage::PauseSettings;
    return frame.draw_content && frame.draw_facts &&
        (!graphics_visible || frame.graphics_model == graphics_model) &&
        std::ranges::equal(*frame.draw_facts, facts);
}

[[nodiscard]] inline bool PointHitsUiDrawButton(std::span<const UiModel> models,
                                               Float2 cursor_normalized) noexcept
{
    const auto x = (cursor_normalized.x + 1.0f) * 960.0f;
    const auto y = (1.0f - cursor_normalized.y) * 540.0f;
    for (auto it = models.rbegin(); it != models.rend(); ++it)
        if (it->kind == UiModel::Kind::Button &&
            x >= it->anchor_pixels.x && x <= it->anchor_pixels.x + it->size_pixels.x &&
            y >= it->anchor_pixels.y && y <= it->anchor_pixels.y + it->size_pixels.y)
            return true;
    return false;
}

[[nodiscard]] inline bool UiFrameExtentMatches(ViewExtent presented,
                                               ViewExtent current) noexcept
{
    return presented.width != 0 && presented.height != 0 && presented == current;
}
}
