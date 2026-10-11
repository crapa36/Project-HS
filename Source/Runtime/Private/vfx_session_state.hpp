#pragma once

#include <hs/core/fixed_step_clock.hpp>
#include <hs/core/presentation_event.hpp>
#include <hs/game_domain/game_types.hpp>
#include "vfx_typed_entity_surface_commands.hpp"
#include "vfx_typed_screen_commands.hpp"
#include <hs/renderer/vfx_frame_input.hpp>
#include <hs/renderer/vfx_spawn_command.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace hs::runtime_detail
{
// Render-thread retention. Events can arrive before their matching snapshot;
// a session ID keeps a reset simulation clock from replaying the previous run.
struct VfxSessionState
{
    struct PersistentBurstKey
    {
        std::uint64_t stable_id{};
        std::uint8_t visual_kind{};
        bool operator==(const PersistentBurstKey &) const = default;
    };
    struct PersistentBurstHash
    {
        std::size_t operator()(const PersistentBurstKey &key) const noexcept
        {
            const auto mixed = key.stable_id ^
                (static_cast<std::uint64_t>(key.visual_kind) << 56);
            return static_cast<std::size_t>(mixed ^ (mixed >> 33));
        }
    };
    std::uint64_t session_id{};
    VfxLowHealthPulseState low_health_pulse;
    std::optional<Tick> terminal_clock_source_tick;
    std::optional<SessionPhase> terminal_clock_phase;
    std::optional<FixedStepClock::Clock::time_point> terminal_clock_started_at;
    std::vector<PresentationEvent> pending_events;
    std::vector<ParticleSpawnCommand> particle_spawns;
    std::vector<VfxLineSpawnCommand> effect_lines;
    std::vector<VfxFlashSpawnInput> flashes;
    std::vector<VfxEventInput> ground_events;
    std::vector<VfxEventInput> ribbon_events;
    std::vector<VfxEventInput> light_events;
    std::vector<VfxEventInput> distortion_events;
    std::vector<VfxEventInput> decal_events;
    std::vector<VfxEventInput> volume_events;
    std::vector<VfxEventInput> screen_events;
    std::vector<VfxEventInput> entity_surface_events;
    std::vector<VfxEntitySurfaceEventCapture> entity_surface_captures;
    std::vector<VfxEventInput> point_rate_events;
    std::unordered_set<PersistentBurstKey, PersistentBurstHash> seen_persistent_bursts;

    bool MarkPersistentBurst(std::uint64_t stable_id, std::uint8_t visual_kind)
    {
        return stable_id != 0 &&
            seen_persistent_bursts.insert({stable_id, visual_kind}).second;
    }

    void ClearDecoded()
    {
        particle_spawns.clear();
        effect_lines.clear();
        flashes.clear();
        ground_events.clear();
        ribbon_events.clear();
        light_events.clear();
        distortion_events.clear();
        decal_events.clear();
        volume_events.clear();
        screen_events.clear();
        entity_surface_events.clear();
        entity_surface_captures.clear();
        point_rate_events.clear();
    }

    // Publish a prepared catalog's decoded retention without changing the
    // session clock, pending stable-identity events, or burst tombstones.
    void SwapDecoded(VfxSessionState &prepared) noexcept
    {
        particle_spawns.swap(prepared.particle_spawns);
        effect_lines.swap(prepared.effect_lines);
        flashes.swap(prepared.flashes);
        ground_events.swap(prepared.ground_events);
        ribbon_events.swap(prepared.ribbon_events);
        light_events.swap(prepared.light_events);
        distortion_events.swap(prepared.distortion_events);
        decal_events.swap(prepared.decal_events);
        volume_events.swap(prepared.volume_events);
        screen_events.swap(prepared.screen_events);
        entity_surface_events.swap(prepared.entity_surface_events);
        entity_surface_captures.swap(prepared.entity_surface_captures);
        point_rate_events.swap(prepared.point_rate_events);
    }

    // Terminal simulation stops advancing after the final boss transition.
    // Keep only this presentation clock on real time; all other VFX clocks
    // remain tied to the authoritative snapshot tick.
    Tick UpdateTerminalClock(Tick snapshot_tick, SessionPhase phase,
                             FixedStepClock::Clock::time_point now) noexcept
    {
        const bool terminal = phase == SessionPhase::Victory || phase == SessionPhase::Defeat;
        if (!terminal)
        {
            terminal_clock_source_tick.reset();
            terminal_clock_phase.reset();
            terminal_clock_started_at.reset();
            return snapshot_tick;
        }
        if (!terminal_clock_source_tick || !terminal_clock_phase || !terminal_clock_started_at ||
            *terminal_clock_source_tick != snapshot_tick || *terminal_clock_phase != phase)
        {
            terminal_clock_source_tick = snapshot_tick;
            terminal_clock_phase = phase;
            terminal_clock_started_at = now;
            return snapshot_tick;
        }
        if (now <= *terminal_clock_started_at) return *terminal_clock_source_tick;
        const auto elapsed_ticks = (now - *terminal_clock_started_at) / FixedStepClock::kFixedStep;
        if (elapsed_ticks <= 0) return *terminal_clock_source_tick;
        const auto advance = static_cast<Tick>(elapsed_ticks);
        const auto remaining = std::numeric_limits<Tick>::max() - *terminal_clock_source_tick;
        return *terminal_clock_source_tick + std::min(advance, remaining);
    }

    bool Synchronize(std::uint64_t current_session)
    {
        if (session_id == current_session) return false;
        session_id = current_session;
        low_health_pulse.Clear();
        terminal_clock_source_tick.reset();
        terminal_clock_phase.reset();
        terminal_clock_started_at.reset();
        ClearDecoded();
        seen_persistent_bursts.clear();
        std::erase_if(pending_events, [&](const PresentationEvent &event) {
            return event.session_id < session_id;
        });
        return true;
    }

    std::vector<PresentationEvent> TakeReady(Tick tick)
    {
        std::vector<PresentationEvent> ready;
        std::erase_if(pending_events, [&](const PresentationEvent &event) {
            if (event.session_id < session_id) return true;
            if (event.session_id != session_id || event.tick > tick) return false;
            ready.push_back(event);
            return true;
        });
        return ready;
    }
};
} // namespace hs::runtime_detail
