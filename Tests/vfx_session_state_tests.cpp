#include "vfx_session_state.hpp"

#include <cstdio>
#include <stdexcept>
#include <tuple>
#include <utility>

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

void CheckActiveEffectRetention()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    Store effects;
    hs::VfxEventInput a{}, b{}, c{}, missing{};
    a.sequence = 10; b.sequence = 20; c.sequence = 30; missing.sequence = 40;
    const auto ar = effects.Intern(a), br = effects.Intern(b), cr = effects.Intern(c);
    Check(effects.Add(Store::Profile::Ground, ar) &&
          effects.Add(Store::Profile::Ground, br) &&
          effects.Add(Store::Profile::Ground, cr), "initial effect membership");
    const auto *unchanged = effects.Get(ar);
    effects.Compact();
    Check(effects.Get(ar) == unchanged && effects.Find(c) == cr,
          "unchanged membership must preserve rows and their indices");
    const std::array survivors{c, a, c, missing};
    effects.Commit(Store::Profile::Ground, survivors);
    std::vector<hs::runtime_detail::RetainedVfxEntitySurfaceCapture> captures(1);
    captures[0].row = br;
    effects.Compact(captures);
    Check(effects.Size() == 3 && captures[0].row == br,
          "capture retains an effect after its profile expires");
    captures.clear();
    effects.Compact(captures);
    std::vector<hs::VfxEventInput> materialized;
    effects.Materialize(Store::Profile::Ground, materialized);
    Check(effects.Size() == 2 && effects.Find(a) == 0 && effects.Find(c) == 1 &&
          materialized.size() == 2 && materialized[0].sequence == 30 &&
          materialized[1].sequence == 10,
          "stable compaction remaps indices and preserves unique scratch order");
    effects.Commit(Store::Profile::Ground, {});
    effects.Compact();
    Check(effects.Size() == 0 && !effects.Get(0), "last effect retirement clears rows");
    Check(effects.Intern(b) == 0, "row storage is reusable after retirement");
}

void CheckGeometrySectorIdentity()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    Store effects;
    std::array<hs::VfxEventInput, 4> sectors{};
    for (std::uint32_t i = 0; i < sectors.size(); ++i)
    {
        sectors[i].sequence = 10;
        sectors[i].event_tick = 20;
        sectors[i].geometry_owner_id = 30;
        sectors[i].geometry_effect_id = 40;
        sectors[i].status_episode_generation = 50;
        sectors[i].geometry_sector_index = i;
        const auto row = effects.Intern(sectors[i]);
        Check(row == i && effects.Add(Store::Profile::Ground, row),
              "safe sectors with shared episode identity need distinct rows");
    }

    auto rebound = sectors;
    for (auto &sector : rebound) sector.stable_seed = 99;
    effects.RebindProfile(Store::Profile::Ground, rebound);
    for (std::uint32_t i = 0; i < rebound.size(); ++i)
        Check(effects.Get(i)->stable_seed == 99 && effects.Find(sectors[i]) == i,
              "profile rebind must preserve each sector's row identity");

    effects.Commit(Store::Profile::Ground, std::span<const hs::VfxEventInput>(rebound).subspan(1, 2));
    effects.Compact();
    Check(effects.Size() == 2 && effects.Get(0)->geometry_sector_index == 1 &&
          effects.Get(1)->geometry_sector_index == 2 &&
          effects.Find(sectors[1]) == 0 && effects.Find(sectors[2]) == 1,
          "profile commit and compaction must retain distinct sector rows");
}
}

int main()
{
    try
    {
        CheckActiveEffectRetention();
        CheckGeometrySectorIdentity();
        static_assert(noexcept(std::declval<hs::runtime_detail::VfxSessionState &>().SwapDecoded(
            std::declval<hs::runtime_detail::DecodedVfxState &>())));
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
        Check(state.raw_events.Enqueue(pending), "event enqueue");
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
        Check(state.session_id == 10 && state.raw_events.pending.size() == 1 &&
                  state.low_health_pulse.stable_id == 77 && !state.MarkPersistentBurst(88, 3),
              "generation publication must preserve semantic session state");
        Check(state.UpdateTerminalClock(120, SessionPhase::Victory,
                  start + Clock::kFixedStep * 5) == 125,
              "generation publication must not restart terminal time");
        state.ClearDecoded();
        CheckSize(state.decoded, 0);
        Check(state.terminal_clock.has_value() && state.raw_events.pending.size() == 1 &&
                  !state.MarkPersistentBurst(88, 3),
              "decoded clear must preserve pending events, clock and burst tombstones");
        Check(state.raw_events.Prepare(139, 10).entries.empty(), "future event dispatched early");
        const auto &batch = state.raw_events.Prepare(140, 10);
        Check(batch.entries.size() == 1 && batch.entries[0].event.sequence == 9,
              "pending event becomes ready at its authoritative tick");
        Check(state.raw_events.Prepare(140, 10).entries.size() == 1,
              "unacknowledged ready batch remains owned for retry");
        state.raw_events.Acknowledge();
        Check(state.raw_events.Prepare(140, 10).entries.empty(),
              "acknowledged event consumes exactly once");
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
        hs::runtime_detail::VfxSessionState::RawEventScheduler scheduler;
        for (std::uint64_t i = 1; i <= scheduler.kCapacity; ++i)
        {
            hs::PresentationEvent ordered;
            ordered.session_id = 1;
            ordered.tick = i % 2 ? 5 : 4;
            ordered.sequence = i;
            Check(scheduler.Enqueue(ordered), "bounded scheduler rejected in-capacity event");
        }
        hs::PresentationEvent overflow;
        overflow.session_id = 1;
        Check(!scheduler.Enqueue(overflow), "bounded scheduler exceeded capacity");
        const auto &ordered = scheduler.Prepare(5, 1);
        Check(ordered.entries.size() == scheduler.kCapacity, "due scheduler missed ready entries");
        for (std::size_t i = 1; i < ordered.entries.size(); ++i)
            Check(ordered.entries[i-1].event.tick < ordered.entries[i].event.tick ||
                  (ordered.entries[i-1].event.tick == ordered.entries[i].event.tick &&
                   ordered.entries[i-1].ordinal < ordered.entries[i].ordinal),
                  "due scheduler did not preserve due/insertion ordering");
        scheduler.Acknowledge();
        hs::PresentationEvent future;
        future.session_id = 3; future.tick = 0; future.sequence = 3;
        hs::PresentationEvent current = future;
        current.session_id = 2; current.tick = 120; current.sequence = 2;
        Check(scheduler.Enqueue(future) && scheduler.Enqueue(current), "cross-session enqueue");
        const auto &current_batch = scheduler.Prepare(120, 2);
        Check(current_batch.entries.size() == 1 && current_batch.entries[0].event.sequence == 2 &&
              scheduler.pending.size() == 1 && scheduler.pending.front().event.session_id == 3,
              "future session must neither block nor activate in the current session");
        scheduler.Acknowledge();
        Check(scheduler.Prepare(200, 2).entries.empty() && scheduler.pending.size() == 1,
              "future session must retain ownership while the current session advances");
        scheduler.DiscardBeforeSession(3);
        Check(scheduler.Prepare(0, 3).entries.size() == 1,
              "retained future event must activate when its session begins");
        scheduler.Acknowledge();
        std::puts("VFX session state tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
