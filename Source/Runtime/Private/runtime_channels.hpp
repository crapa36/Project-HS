#pragma once

#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/presentation/snapshot_exchange.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/game_types.hpp>
#include <hs/renderer/renderer.hpp>
#include <hs/presentation/projector.hpp>

#include "graphics_settings_commit.hpp"
#include "runtime_diagnostics.hpp"
#include "main_wake.hpp"
#include "ui_draw_packet.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace hs
{

struct ResizeCommand
{
    std::uint32_t width{};
    std::uint32_t height{};
};

struct QueuedAudioEvent
{
    PresentationEvent event;
    std::uint64_t enqueued_qpc{};
    enum class Kind : std::uint8_t { Event, Tick } kind{};
    struct TickInfo
    {
        std::uint64_t session_id{};
        Tick tick{};
        SessionPhase phase{SessionPhase::MainMenu};
        Float3 listener_position{};
    } tick_info;
    std::uint64_t dequeued_qpc{}; // Main's owned batch only; producer leaves zero.
};

struct RenderPorts
{
    std::atomic<bool> &stop_requested;
    std::atomic<bool> &simulation_done;
    const std::atomic<Tick> &completed_tick;
    std::atomic<bool> &render_ready;
    BoundedSpscQueue<ActionEdge, 256> &action_edges;
    RenderSnapshotExchange &snapshots;
    BoundedSpscQueue<PresentationEvent, 8192> &presentation_events;
    BoundedSpscQueue<QueuedAudioEvent, 1024> &render_audio_events;
    BoundedSpscQueue<ResizeCommand, 64> &resize_commands;
    BoundedSpscQueue<GraphicsSettingsCommitRequest, 8> &renderer_settings;
    BoundedSpscQueue<GraphicsSettingsAck, 8> &renderer_settings_acks;
    BoundedSpscQueue<NativeWindowMessage, 256> &window_messages;
    BoundedSpscQueue<DebugCommand, 64> &debug_commands;
    std::atomic<std::uint64_t> &rendered_frames;
    std::atomic<std::uint32_t> &rendered_particles;
    std::atomic<std::uint64_t> &dropped_input_edges;
    std::atomic<std::uint64_t> &dropped_presentation_events;
    std::atomic<bool> &devtools_capture_mouse;
    std::atomic<bool> &devtools_capture_keyboard;
    std::atomic<std::shared_ptr<const GraphicsSettingsUiModel>> &renderer_graphics_ui;
    const std::atomic<std::uint8_t> &session_phase;
    const std::atomic<std::uint8_t> &ui_page;
    runtime_detail::UiDrawPackets &ui_packets;
    runtime_detail::MainWake *main_wake{};
    void NotifyMain() noexcept { if (main_wake) main_wake->Notify(); }
};

struct SimulationPorts
{
    std::atomic<bool> &stop_requested;
    std::atomic<bool> &simulation_done;
    std::atomic<HeldInputState> &held_input;
    std::atomic<std::uint64_t> &focus_epoch;
    std::atomic<float> &camera_target_x;
    std::atomic<float> &camera_target_z;
    std::atomic<std::uint32_t> &camera_zoom_percent;
    BoundedSpscQueue<ActionEdge, 256> &action_edges;
    RenderSnapshotExchange &snapshots;
    BoundedSpscQueue<PresentationEvent, 8192> &presentation_events;
    BoundedSpscQueue<QueuedAudioEvent, 1024> &sim_audio_events;
    BoundedSpscQueue<SettingsData, 8> &presentation_settings;
    BoundedSpscQueue<runtime_detail::UiActionEnvelope, 64> &ui_actions;
    BoundedSpscQueue<DebugCommand, 64> &debug_commands;
    std::atomic<Tick> &completed_tick;
    std::atomic<GameplayChecksum> &gameplay_checksum;
    std::atomic<std::uint8_t> &session_phase;
    std::mutex &session_probe_mutex;
    SessionProbe &session_probe;
    std::atomic<std::uint8_t> &ui_page;
    std::atomic<std::uint8_t> &collection_skill;
    std::atomic<std::uint8_t> &character_skill;
    std::atomic<std::uint8_t> &loadout_source;
    std::atomic<std::uint8_t> &pending_rebind_slot;
    std::atomic<std::uint32_t> &best_level;
    std::atomic<std::uint64_t> &completed_run_kills;
    std::atomic<std::uint64_t> &completed_run_wins;
    std::atomic<std::uint64_t> &dropped_presentation_events;
    std::atomic<std::uint8_t> &settings_group;
    std::atomic<std::uint8_t> &settings_page;
    std::atomic<std::uint8_t> &character_details_page;
    std::atomic<std::shared_ptr<const GraphicsSettingsUiModel>> &presentation_graphics_ui;
    std::atomic<bool> &ui_input_rejected;
    std::atomic<std::uint64_t> &current_session_id;
    std::atomic<std::uint64_t> &stale_ui_action_drops;
    runtime_detail::UiDrawPackets &ui_packets;
    runtime_detail::DiagnosticClock capture_clock{};
    runtime_detail::MainWake *main_wake{};
    void NotifyMain() noexcept { if (main_wake) main_wake->Notify(); }

    void PublishAudioTick(const QueuedAudioEvent::TickInfo &tick)
    {
        QueuedAudioEvent packet;
        packet.kind = QueuedAudioEvent::Kind::Tick;
        packet.tick_info = tick;
        packet.enqueued_qpc = capture_clock ? capture_clock() : 0;
        if (!sim_audio_events.TryPush(packet))
            dropped_presentation_events.fetch_add(1, std::memory_order_relaxed);
        else NotifyMain();
    }

    // The same owned values feed recording and one live destination in source order.
    void ProjectDomainSignalsOnce(std::span<const DomainSignal> signals, float arena_half_extent,
                                  bool capture_trace, std::vector<PresentationEvent> *record_events)
    {
        if (record_events) record_events->clear();
        for (const auto &signal : signals)
        {
            std::array<PresentationEvent, 3> projected{};
            const auto count = ProjectDomainSignal(signal, projected, arena_half_extent);
            for (const auto &event : std::span(projected).first(count))
            {
                if (record_events) record_events->push_back(event);
                if (event.kind == PresentationKind::Audio)
                {
                    if (!sim_audio_events.TryPush({event, capture_clock ? capture_clock() : 0}))
                        dropped_presentation_events.fetch_add(1, std::memory_order_relaxed);
                    else NotifyMain();
                    // Explicit capture retains the original ordered render-owned trace.
                    // The renderer logs this copy but never forwards or plays it.
                    if (!capture_trace) continue;
                }
                if (!presentation_events.TryPush(event))
                    dropped_presentation_events.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void PublishSessionProbe(const SessionProbe &probe, std::uint64_t session_id = 0)
    {
        std::scoped_lock lock(session_probe_mutex);
        session_probe = probe;
        current_session_id.store(session_id, std::memory_order_release);
    }
};

struct RuntimeChannels
{
    explicit RuntimeChannels(runtime_detail::MainWake *wake = nullptr)
        : snapshots(10'000, 8, 4, kGraphicsUiSnapshotReserve), main_wake(wake)
    {
    }

    std::atomic<bool> stop_requested{};
    std::atomic<bool> simulation_done{};
    std::atomic<bool> render_ready{};
    std::atomic<HeldInputState> held_input{};
    std::atomic<std::uint64_t> focus_epoch{};
    std::atomic<float> camera_target_x{};
    std::atomic<float> camera_target_z{};
    std::atomic<std::uint32_t> camera_zoom_percent{100};
    BoundedSpscQueue<ActionEdge, 256> action_edges;
    RenderSnapshotExchange snapshots;
    BoundedSpscQueue<PresentationEvent, 8192> presentation_events;
    BoundedSpscQueue<QueuedAudioEvent, 1024> sim_audio_events;
    BoundedSpscQueue<QueuedAudioEvent, 1024> render_audio_events;
    runtime_detail::DiagnosticClock capture_clock{}; // Assigned before worker publication.
    runtime_detail::MainWake *main_wake{};
    void NotifyMain() noexcept { if (main_wake) main_wake->Notify(); }
    BoundedSpscQueue<ResizeCommand, 64> resize_commands;
    // Requests carry a pointer to the commit owner's immutable in-flight graphics
    // snapshot. Release/acquire queue publication and the acknowledgment queue
    // protect it until the render consumer finishes; Runtime joins before teardown.
    BoundedSpscQueue<GraphicsSettingsCommitRequest, 8> renderer_settings;
    BoundedSpscQueue<GraphicsSettingsAck, 8> renderer_settings_acks;
    BoundedSpscQueue<SettingsData, 8> presentation_settings;
    BoundedSpscQueue<runtime_detail::UiActionEnvelope, 64> ui_actions;
    BoundedSpscQueue<UiCommand, 64> ui_commands;
    std::uint64_t rejected_ui_commands{}; // Main-thread admission counter.
    std::atomic<std::uint64_t> stale_ui_action_drops{};
    std::atomic<bool> ui_input_rejected{};
    BoundedSpscQueue<NativeWindowMessage, 256> window_messages;
    BoundedSpscQueue<DebugCommand, 64> debug_commands;
    std::atomic<Tick> completed_tick{};
    std::atomic<GameplayChecksum> gameplay_checksum{};
    std::atomic<std::uint8_t> session_phase{};
    mutable std::mutex session_probe_mutex;
    SessionProbe session_probe{};
    std::atomic<std::uint64_t> current_session_id{};
    std::atomic<std::uint8_t> ui_page{};
    std::atomic<std::uint8_t> collection_skill{};
    std::atomic<std::uint8_t> character_skill{};
    std::atomic<std::uint8_t> loadout_source{0xFF};
    std::atomic<std::uint8_t> pending_rebind_slot{0xFF};
    std::atomic<std::uint32_t> best_level{1};
    std::atomic<std::uint64_t> completed_run_kills{};
    std::atomic<std::uint64_t> completed_run_wins{};
    std::atomic<std::uint64_t> rendered_frames{};
    std::atomic<std::uint32_t> rendered_particles{};
    std::atomic<std::uint64_t> dropped_input_edges{};
    std::atomic<std::uint64_t> dropped_presentation_events{};
    std::atomic<bool> devtools_capture_mouse{};
    std::atomic<bool> devtools_capture_keyboard{};
    std::atomic<std::uint8_t> settings_group{};
    std::atomic<std::uint8_t> settings_page{};
    std::atomic<std::uint8_t> character_details_page{};
    // A renderer receipt is published at initialization/apply/reload boundaries
    // and for completed native telemetry while a settings page is open.
    // Main combines it with validated requested intent; simulation only reads it.
    std::atomic<std::shared_ptr<const GraphicsSettingsUiModel>> renderer_graphics_ui;
    std::atomic<std::shared_ptr<const GraphicsSettingsUiModel>> presentation_graphics_ui;
    runtime_detail::UiDrawPackets ui_packets;

    void PublishSessionProbe(const SessionProbe &probe, std::uint64_t session_id = 0)
    {
        std::scoped_lock lock(session_probe_mutex);
        session_probe = probe;
        current_session_id.store(session_id, std::memory_order_release);
    }

    [[nodiscard]] SessionProbe ReadSessionProbe() const
    {
        std::scoped_lock lock(session_probe_mutex);
        return session_probe;
    }

    [[nodiscard]] RenderPorts ForRender() noexcept
    {
        return {stop_requested, simulation_done, completed_tick, render_ready, action_edges, snapshots,
                presentation_events, render_audio_events, resize_commands, renderer_settings,
                renderer_settings_acks, window_messages, debug_commands, rendered_frames, rendered_particles,
                dropped_input_edges, dropped_presentation_events, devtools_capture_mouse,
                devtools_capture_keyboard, renderer_graphics_ui, session_phase, ui_page, ui_packets, main_wake};
    }

    [[nodiscard]] SimulationPorts ForSimulation() noexcept
    {
        return {stop_requested, simulation_done, held_input, focus_epoch,
                camera_target_x, camera_target_z, camera_zoom_percent, action_edges,
                 snapshots, presentation_events, sim_audio_events,
                presentation_settings, ui_actions,
                debug_commands, completed_tick, gameplay_checksum, session_phase,
                session_probe_mutex, session_probe, ui_page,
                collection_skill, character_skill, loadout_source,
                pending_rebind_slot, best_level, completed_run_kills,
                completed_run_wins, dropped_presentation_events,
                settings_group, settings_page, character_details_page, presentation_graphics_ui,
                ui_input_rejected, current_session_id, stale_ui_action_drops, ui_packets, capture_clock, main_wake};
    }
};

} // namespace hs
