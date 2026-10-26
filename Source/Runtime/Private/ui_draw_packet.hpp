#pragma once

#include <hs/render_data/render_snapshot.hpp>
#include <hs/render_data/render_view_input.hpp>
#include <hs/core/settings.hpp>
#include <hs/presentation/projector.hpp>

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace hs::runtime_detail
{
// Small copied facts needed to resolve a click against the exact draw packet.
// The snapshot itself remains the owner of the full gameplay projection.
struct UiDrawPacketFacts
{
    std::uint64_t generation{};
    std::uint64_t session_id{};
    std::uint64_t selection_epoch{};
    SessionProbe session{};
    PresentationUiState ui{};
    SettingsData settings{};
    std::shared_ptr<const GraphicsSettingsUiModel> graphics_model;
    std::uint8_t modal_phase{};
};

struct UiDrawPacket final : UiDrawPacketFacts
{
    // Copied only after the renderer acknowledges this generation at Present.
    std::vector<UiModel> draw_models;
    ViewExtent presented_extent{};
};

// The simulation stages facts; rendering publishes only its matching Present receipt.
struct UiDrawPackets
{
    std::array<std::optional<UiDrawPacketFacts>, 3> ui_packet_facts;
    mutable std::mutex ui_packet_mutex;
    std::atomic<std::shared_ptr<const UiDrawPacket>> presented_ui_packet;

    void StageUiPacket(std::size_t slot, UiDrawPacketFacts facts)
    {
        if (slot >= ui_packet_facts.size()) return;
        std::scoped_lock lock(ui_packet_mutex);
        ui_packet_facts[slot] = std::move(facts);
    }

    [[nodiscard]] bool ReadUiPacketFacts(std::uint64_t generation,
                                         UiDrawPacketFacts &facts) const
    {
        std::scoped_lock lock(ui_packet_mutex);
        for (const auto &candidate : ui_packet_facts)
            if (candidate && candidate->generation == generation)
            { facts = *candidate; return true; }
        return false;
    }

    void RetireUiPacketFacts(std::uint64_t generation)
    {
        std::scoped_lock lock(ui_packet_mutex);
        for (auto &candidate : ui_packet_facts)
            if (candidate && candidate->generation == generation) candidate.reset();
    }

    [[nodiscard]] bool AcceptPresentedUiPacket(std::uint64_t receipt_generation,
        std::uint64_t rendered_generation, std::uint64_t session_id,
        std::span<const UiModel> draw_models, ViewExtent extent) noexcept
    {
        if (!receipt_generation || receipt_generation != rendered_generation) return false;
        UiDrawPacketFacts facts;
        if (!ReadUiPacketFacts(receipt_generation, facts) || facts.session_id != session_id) return false;
        try
        {
            auto packet = std::make_shared<UiDrawPacket>();
            static_cast<UiDrawPacketFacts &>(*packet) = std::move(facts);
            packet->draw_models.assign(draw_models.begin(), draw_models.end());
            packet->presented_extent = extent;
            presented_ui_packet.store(std::move(packet), std::memory_order_release);
            RetireUiPacketFacts(receipt_generation);
            return true;
        }
        catch (...)
        {
            return false; // Keep the previous accepted packet on allocation failure.
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
