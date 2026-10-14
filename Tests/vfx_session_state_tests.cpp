#include "vfx_session_state.hpp"

#include <cstdio>
#include <stdexcept>
#include <tuple>

namespace
{
void Check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

auto Retained(hs::runtime_detail::DecodedVfxState &state)
{
    return std::tie(state.particle_spawns, state.effect_lines, state.flashes,
        state.ground_events, state.ribbon_events, state.light_events,
        state.distortion_events, state.decal_events, state.volume_events,
        state.screen_events, state.entity_surface_events,
        state.entity_surface_captures, state.point_rate_events);
}

void Resize(hs::runtime_detail::DecodedVfxState &state, std::size_t count)
{
    std::apply([&](auto &...retained) { (retained.resize(count), ...); }, Retained(state));
}

void CheckSize(hs::runtime_detail::DecodedVfxState &state, std::size_t count)
{
    Check(std::apply([&](const auto &...retained) {
        return ((retained.size() == count) && ...);
    }, Retained(state)), "every decoded retention category must transfer/reset");
}
}

int main()
{
    try
    {
        using hs::SessionPhase;
        using Clock = hs::FixedStepClock;
        hs::runtime_detail::VfxSessionState state;
        Check(state.Synchronize(10), "initial session");
        const auto start = Clock::Clock::time_point{};
        Check(state.UpdateTerminalClock(120, SessionPhase::Victory, start) == 120,
              "terminal episode starts on snapshot tick");
        state.low_health_pulse.stable_id = 77;
        Check(state.MarkPersistentBurst(88, 3), "first burst admitted");
        hs::PresentationEvent pending;
        pending.session_id = 10; pending.tick = 140; pending.sequence = 9;
        state.pending_events.push_back(pending);
        Resize(state.decoded, 1);
        const auto lease = std::make_shared<int>(1);
        state.decoded.entity_surface_captures[0].generation_lease = lease;

        // A rejected reload destroys only its prepared copy.
        {
            auto failed = state.decoded;
            Resize(failed, 2);
            failed.entity_surface_captures[0].generation_lease.reset();
        }
        CheckSize(state.decoded, 1);
        Check(state.decoded.entity_surface_captures[0].generation_lease == lease,
              "failed preparation must preserve captured generation lease");

        auto candidate = state.decoded;
        Resize(candidate, 2);
        state.SwapDecoded(candidate);
        CheckSize(state.decoded, 2);
        CheckSize(candidate, 1);
        Check(state.decoded.entity_surface_captures[0].generation_lease == lease,
              "successful publication preserves the prepared capture lease");
        Check(state.session_id == 10 && state.pending_events.size() == 1 &&
                  state.low_health_pulse.stable_id == 77 && !state.MarkPersistentBurst(88, 3),
              "generation publication must preserve semantic session state");
        Check(state.UpdateTerminalClock(120, SessionPhase::Victory,
                  start + Clock::kFixedStep * 5) == 125,
              "generation publication must not restart terminal time");
        state.ClearDecoded();
        CheckSize(state.decoded, 0);
        Check(state.terminal_clock.has_value() && state.pending_events.size() == 1 &&
                  !state.MarkPersistentBurst(88, 3),
              "decoded clear must preserve pending events, clock and burst tombstones");
        Check(state.TakeReady(139).empty() && state.TakeReady(140).size() == 1,
              "pending event consumes exactly once at its authoritative tick");
        Resize(state.decoded, 1);
        Check(state.Synchronize(11), "new session");
        CheckSize(state.decoded, 0);
        Check(!state.terminal_clock && state.low_health_pulse.stable_id == 0 &&
                  state.MarkPersistentBurst(88, 3), "session restart clears episode state");
        Check(state.UpdateTerminalClock(240, SessionPhase::Defeat, start) == 240 &&
                  state.UpdateTerminalClock(240, SessionPhase::Defeat,
                      start - Clock::kFixedStep) == 240,
              "backwards wall time cannot advance terminal time");
        Check(state.UpdateTerminalClock(240, SessionPhase::Victory,
                  start + Clock::kFixedStep * 8) == 240,
              "terminal phase change starts a new clock");
        Check(state.UpdateTerminalClock(241, SessionPhase::Victory,
                  start + Clock::kFixedStep * 9) == 241,
              "new authoritative terminal tick restarts presentation clock");
        Check(state.UpdateTerminalClock(241, SessionPhase::Paused, start) == 241 &&
                  !state.terminal_clock, "nonterminal phases stay on authoritative time");
        const auto maximum = std::numeric_limits<hs::Tick>::max();
        state.UpdateTerminalClock(maximum - 1, SessionPhase::Victory, start);
        Check(state.UpdateTerminalClock(maximum - 1, SessionPhase::Victory,
                  start + Clock::kFixedStep * 4) == maximum,
              "terminal clock saturates without overflow");
        std::puts("VFX session state tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
