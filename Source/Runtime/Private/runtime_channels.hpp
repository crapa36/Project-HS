#pragma once

#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/input.hpp>
#include <hs/core/particle_spawn_command.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/snapshot_exchange.hpp>
#include <hs/core/settings.hpp>
#include <hs/gameplay/gameplay_types.hpp>
#include <hs/gameplay/game_data.hpp>
#include <hs/renderer/renderer.hpp>

#include <atomic>
#include <cstdint>

namespace hs
{

enum class GraphicsCommandKind : std::uint8_t
{
    Resize,
};

struct GraphicsCommand
{
    GraphicsCommandKind kind{};
    std::uint32_t width{};
    std::uint32_t height{};
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
    BoundedSpscQueue<ParticleSpawnCommand, 256> particle_spawns;
    BoundedSpscQueue<GraphicsCommand, 64> graphics_commands;
    BoundedSpscQueue<SettingsData, 8> renderer_settings;
    BoundedSpscQueue<SettingsData, 8> simulation_settings;
    BoundedSpscQueue<GameData, 2> gameplay_data_updates;
    BoundedSpscQueue<UiCommand, 64> ui_commands;
    BoundedSpscQueue<NativeWindowMessage, 256> window_messages;
    BoundedSpscQueue<DebugCommand, 64> debug_commands;
    std::atomic<Tick> completed_tick{};
    std::atomic<GameplayChecksum> checksum{};
    std::atomic<std::uint8_t> session_phase{};
    std::atomic<std::uint32_t> best_level{1};
    std::atomic<std::uint64_t> completed_run_kills{};
    std::atomic<std::uint64_t> completed_run_wins{};
    std::atomic<std::uint64_t> rendered_frames{};
    std::atomic<std::uint32_t> rendered_particles{};
    std::atomic<std::uint64_t> dropped_input_edges{};
    std::atomic<std::uint64_t> dropped_presentation_events{};
    std::atomic<std::uint64_t> dropped_particle_spawns{};
    std::atomic<bool> devtools_capture_mouse{};
    std::atomic<bool> devtools_capture_keyboard{};
};

} // namespace hs
