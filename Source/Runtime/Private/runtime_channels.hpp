#pragma once

#include <hs/core/bounded_spsc_queue.hpp>
#include <hs/core/input.hpp>
#include <hs/core/particle_spawn_command.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/core/snapshot_exchange.hpp>

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
    RuntimeChannels() : snapshots(256, 8, 4, 8)
    {
    }

    std::atomic<bool> stop_requested{};
    std::atomic<bool> simulation_done{};
    std::atomic<bool> render_ready{};
    std::atomic<HeldInputState> held_input{};
    std::atomic<std::uint64_t> focus_epoch{};
    BoundedSpscQueue<ActionEdge, 256> action_edges;
    RenderSnapshotExchange snapshots;
    BoundedSpscQueue<PresentationEvent, 8192> presentation_events;
    BoundedSpscQueue<ParticleSpawnCommand, 256> particle_spawns;
    BoundedSpscQueue<GraphicsCommand, 64> graphics_commands;
    std::atomic<Tick> completed_tick{};
    std::atomic<GameplayChecksum> checksum{};
    std::atomic<std::uint64_t> rendered_frames{};
    std::atomic<std::uint32_t> rendered_particles{};
    std::atomic<std::uint64_t> dropped_input_edges{};
    std::atomic<std::uint64_t> dropped_presentation_events{};
    std::atomic<std::uint64_t> dropped_particle_spawns{};
};

} // namespace hs
