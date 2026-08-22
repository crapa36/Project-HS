#pragma once

#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/input.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/snapshot_exchange.hpp>
#include <hs/core/settings.hpp>
#include <hs/game_domain/game_types.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <hs/renderer/renderer.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace hs
{

struct ResizeCommand
{
    std::uint32_t width{};
    std::uint32_t height{};
};

struct RenderPorts
{
    std::atomic<bool> &stop_requested;
    std::atomic<bool> &simulation_done;
    std::atomic<bool> &render_ready;
    BoundedSpscQueue<ActionEdge, 256> &action_edges;
    RenderSnapshotExchange &snapshots;
    BoundedSpscQueue<PresentationEvent, 8192> &presentation_events;
    BoundedSpscQueue<PresentationEvent, 1024> &audio_events;
    BoundedSpscQueue<ResizeCommand, 64> &resize_commands;
    BoundedSpscQueue<SettingsData, 8> &renderer_settings;
    BoundedSpscQueue<NativeWindowMessage, 256> &window_messages;
    BoundedSpscQueue<DebugCommand, 64> &debug_commands;
    std::atomic<std::uint64_t> &rendered_frames;
    std::atomic<std::uint32_t> &rendered_particles;
    std::atomic<std::uint64_t> &dropped_input_edges;
    std::atomic<std::uint64_t> &dropped_presentation_events;
    std::atomic<bool> &devtools_capture_mouse;
    std::atomic<bool> &devtools_capture_keyboard;
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
    BoundedSpscQueue<SimulationRules, 2> &simulation_rules_updates;
    BoundedSpscQueue<SettingsData, 8> &presentation_settings;
    BoundedSpscQueue<UiAction, 64> &ui_actions;
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

    void PublishSessionProbe(const SessionProbe &probe)
    {
        std::scoped_lock lock(session_probe_mutex);
        session_probe = probe;
    }
};

struct RuntimeChannels
{
    RuntimeChannels() : snapshots(10'000, 8, 4, 32)
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
    BoundedSpscQueue<PresentationEvent, 1024> audio_events;
    BoundedSpscQueue<ResizeCommand, 64> resize_commands;
    BoundedSpscQueue<SettingsData, 8> renderer_settings;
    BoundedSpscQueue<SimulationRules, 2> simulation_rules_updates;
    BoundedSpscQueue<SettingsData, 8> presentation_settings;
    BoundedSpscQueue<UiAction, 64> ui_actions;
    BoundedSpscQueue<UiCommand, 64> ui_commands;
    BoundedSpscQueue<NativeWindowMessage, 256> window_messages;
    BoundedSpscQueue<DebugCommand, 64> debug_commands;
    std::atomic<Tick> completed_tick{};
    std::atomic<GameplayChecksum> gameplay_checksum{};
    std::atomic<std::uint8_t> session_phase{};
    mutable std::mutex session_probe_mutex;
    SessionProbe session_probe{};
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

    void PublishSessionProbe(const SessionProbe &probe)
    {
        std::scoped_lock lock(session_probe_mutex);
        session_probe = probe;
    }

    [[nodiscard]] SessionProbe ReadSessionProbe() const
    {
        std::scoped_lock lock(session_probe_mutex);
        return session_probe;
    }

    [[nodiscard]] RenderPorts ForRender() noexcept
    {
        return {stop_requested, simulation_done, render_ready, action_edges, snapshots,
                presentation_events, audio_events, resize_commands, renderer_settings,
                window_messages, debug_commands, rendered_frames, rendered_particles,
                dropped_input_edges, dropped_presentation_events, devtools_capture_mouse,
                devtools_capture_keyboard};
    }

    [[nodiscard]] SimulationPorts ForSimulation() noexcept
    {
        return {stop_requested, simulation_done, held_input, focus_epoch,
                camera_target_x, camera_target_z, camera_zoom_percent, action_edges,
                 snapshots, presentation_events, simulation_rules_updates,
                presentation_settings, ui_actions,
                debug_commands, completed_tick, gameplay_checksum, session_phase,
                session_probe_mutex, session_probe, ui_page,
                collection_skill, character_skill, loadout_source,
                pending_rebind_slot, best_level, completed_run_kills,
                completed_run_wins, dropped_presentation_events};
    }
};

} // namespace hs
