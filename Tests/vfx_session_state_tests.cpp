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
        state.entity_surface_captures);
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
    effects.EraseIf(Store::Profile::Ground, [&](const hs::VfxEventInput &event) {
        return event.sequence == b.sequence || event.sequence == missing.sequence;
    });
    std::vector<hs::runtime_detail::RetainedVfxEntitySurfaceCapture> captures(1);
    captures[0].row = br;
    effects.Compact(captures);
    Check(effects.Size() == 3 && captures[0].row == br,
          "capture retains an effect after its profile expires");
    captures.clear();
    effects.Compact(captures);
    const auto retained = effects.ProfileEvents(Store::Profile::Ground);
    Check(effects.Size() == 2 && effects.Find(a) == 0 && effects.Find(c) == 1 &&
          retained.size() == 2 && retained[0].sequence == 10 &&
          retained[1].sequence == 30,
          "stable erase and compaction preserve retained profile order");
    effects.EraseIf(Store::Profile::Ground, [](const auto &) { return true; });
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

    effects.EraseIf(Store::Profile::Ground, [](const hs::VfxEventInput &event) {
        return event.geometry_sector_index == 0 || event.geometry_sector_index == 3;
    });
    effects.Compact();
    Check(effects.Size() == 2 && effects.Get(0)->geometry_sector_index == 1 &&
          effects.Get(1)->geometry_sector_index == 2 &&
          effects.Find(sectors[1]) == 0 && effects.Find(sectors[2]) == 1,
          "profile commit and compaction must retain distinct sector rows");
}

void CheckOwnerBindingStorageReuse()
{
    using Store = hs::runtime_detail::ActiveEffectStore;
    Store effects;
    hs::VfxEventInput input{};
    input.sequence = 17;
    input.geometry_owner_id = 99;
    hs::VfxRingGapsPayload gaps;
    gaps.gap_angles_degrees = {10.0f, 20.0f, 30.0f};
    input.payload = gaps;
    effects.Append(Store::Profile::Ground, input);
    effects.Append(Store::Profile::Light, input);

    std::size_t refresh_count{};
    const auto refresh = [&](hs::VfxEventInput &bound) {
        ++refresh_count;
        auto &angles = std::get<hs::VfxRingGapsPayload>(bound.payload).gap_angles_degrees;
        angles[0] = 15.0f;
        return true;
    };
    effects.BeginFrameBindings();
    effects.RefreshProfile(Store::Profile::Ground, refresh);
    effects.RefreshProfile(Store::Profile::Light, refresh);
    auto view = effects.ProfileEvents(Store::Profile::Ground);
    auto *angles = std::get<hs::VfxRingGapsPayload>(view[0].payload).gap_angles_degrees.data();
    const auto capacity = std::get<hs::VfxRingGapsPayload>(view[0].payload).gap_angles_degrees.capacity();
    Check(refresh_count == 1 && angles && capacity >= 3 &&
          std::get<hs::VfxRingGapsPayload>(effects.Get(0)->payload).gap_angles_degrees[0] == 10.0f,
          "owner binding is shared while canonical birth payload stays unchanged");

    effects.BeginFrameBindings();
    effects.RefreshProfile(Store::Profile::Ground, refresh);
    view = effects.ProfileEvents(Store::Profile::Ground);
    const auto &reused = std::get<hs::VfxRingGapsPayload>(view[0].payload).gap_angles_degrees;
    Check(reused.data() == angles && reused.capacity() == capacity && refresh_count == 2,
          "stable ring-gap owner binding reuses nested vector allocation across frames");
}
}

void CheckPendingEmissionOrder()
{
    hs::runtime_detail::DecodedVfxState state;
    const auto append = [&](hs::Tick due, hs::Sequence sequence, hs::Tick birth) {
        hs::ParticleSpawnCommand command;
        command.tick = due; command.sequence = sequence; command.authored_birth_tick = birth;
        state.particle_spawns.push_back(command);
    };
    append(120, 1, 100);
    state.SortNewParticleSpawns(0);
    append(101, 2, 101);
    append(120, 3, 120);
    state.SortNewParticleSpawns(1);
    Check(state.particle_spawns[0].sequence == 2 && state.particle_spawns[1].sequence == 1 &&
        state.particle_spawns[2].sequence == 3 && state.particle_spawns[1].authored_birth_tick == 100,
        "delayed activation reordered equal-due births or changed absolute birth time");
    const auto *retained = state.particle_spawns.data();
    state.SortNewParticleSpawns(state.particle_spawns.size());
    Check(state.particle_spawns.data() == retained, "unchanged pending queue replaced its storage");
    state.particle_spawns.erase(state.particle_spawns.begin());
    append(120, 4, 120);
    append(100, 5, 100);
    state.SortNewParticleSpawns(2);
    Check(state.particle_spawns[0].sequence == 5 && state.particle_spawns[1].sequence == 1 &&
        state.particle_spawns[2].sequence == 3 && state.particle_spawns[3].sequence == 4,
        "accepted-prefix retirement lost stable pending/new due ordering");
}

void CheckActivationBeforeNativeEmission()
{
    hs::runtime_detail::VfxSessionState state;
    state.Synchronize(7);
    hs::PresentationEvent first;
    first.session_id = 7; first.tick = 100; first.sequence = 1;
    Check(state.raw_events.Enqueue(first), "first raw activation enqueue");
    const auto &activation = state.raw_events.Prepare(100, 7);
    Check(activation.entries.size() == 1, "raw event did not reach CPU activation");
    hs::ParticleSpawnCommand delayed;
    delayed.sequence = activation.entries[0].event.sequence;
    delayed.tick = 120; delayed.authored_birth_tick = 120;
    state.decoded.particle_spawns.push_back(delayed);
    state.raw_events.AcknowledgeActivation();
    Check(state.raw_events.Prepare(100, 7).entries.empty() &&
        state.decoded.particle_spawns.size() == 1 && state.decoded.particle_spawns[0].tick == 120,
        "native delay replayed raw activation or lost its owned emission");
    auto second = first;
    second.tick = 101; second.sequence = 2;
    Check(state.raw_events.Enqueue(second) && state.raw_events.Prepare(101, 7).entries.size() == 1,
        "pending native emission blocked the next CPU activation");
    auto immediate = delayed;
    immediate.tick = 101; immediate.authored_birth_tick = 101; immediate.sequence = 2;
    state.decoded.particle_spawns.push_back(immediate);
    state.decoded.SortNewParticleSpawns(1);
    state.raw_events.AcknowledgeActivation();
    Check(state.decoded.particle_spawns[0].sequence == 2 &&
        state.decoded.particle_spawns[1].sequence == 1 && state.raw_events.Prepare(101, 7).entries.empty(),
        "CPU activation order replaced native due order or activated twice");
}

void CheckFrozenNativeBatches()
{
    hs::runtime_detail::VfxSessionState state;
    state.Synchronize(7);
    const auto particle = [&](hs::Tick due, hs::Sequence sequence, hs::Tick birth) {
        hs::ParticleSpawnCommand command;
        command.tick = due; command.sequence = sequence; command.authored_birth_tick = birth;
        state.decoded.particle_spawns.push_back(command);
    };
    particle(120, 1, 100); // A: activated at 100, authored delay 20.
    particle(101, 2, 101); // B: activated at 101, immediate.
    hs::VfxLineSpawnCommand line;
    line.tick = 101; line.sequence = 4;
    state.decoded.effect_lines.push_back(line);
    state.decoded.SortNewParticleSpawns(0);
    Check(state.decoded.particle_spawns[0].sequence == 2 &&
          state.decoded.particle_spawns[1].sequence == 1,
          "delayed source A must remain due after immediate source B");

    auto *batch = state.PrepareNativeBatch(101, true);
    Check(batch && batch->identity.session == 7 && batch->identity.stateful_target == 101 &&
          batch->particle_spawns.size() == 1 && batch->particle_spawns[0].sequence == 2 &&
          batch->particle_spawns[0].authored_birth_tick == 101 && batch->effect_lines.size() == 1 &&
          batch->effect_lines[0].sequence == 4,
          "batch freezes only the due prefix and preserves absolute birth");
    const auto identity = batch->identity;
    const auto particle_storage = batch->particle_spawns.data();
    const auto particle_capacity = batch->particle_spawns.capacity();
    particle(102, 3, 102);
    auto *retry = state.PrepareNativeBatch(102, true);
    Check(retry == batch && retry->identity.sequence == identity.sequence &&
          retry->identity.stateful_target == 101 && retry->particle_spawns.size() == 1 &&
          state.decoded.particle_spawns.size() == 2,
          "retry holds the batch while newer emissions remain pending");
    auto wrong = identity;
    ++wrong.sequence;
    auto wrong_target = identity;
    ++wrong_target.stateful_target;
    auto wrong_session = identity;
    ++wrong_session.session;
    Check(!state.AcknowledgeNativeBatch(wrong, true) &&
          !state.AcknowledgeNativeBatch(wrong_target, true) &&
          !state.AcknowledgeNativeBatch(wrong_session, true) &&
          !state.AcknowledgeNativeBatch(identity, false) && state.native_batch_active,
          "only a submitted exact identity acknowledges the batch");
    Check(state.AcknowledgeNativeBatch(identity, true) &&
          state.last_committed_stateful_target == 101 && !state.native_batch_active &&
          state.native_batch.particle_spawns.capacity() == particle_capacity,
          "matching submission advances the committed target and retires the batch");
    batch = state.PrepareNativeBatch(102, true);
    Check(batch && batch->particle_spawns.size() == 1 && batch->particle_spawns[0].sequence == 3,
          "next batch consumes the newly due pending emission");
    const auto second_identity = batch->identity;
    Check(state.AcknowledgeNativeBatch(second_identity, true), "second batch submitted");

    Check(state.PrepareNativeBatch(102, false) == nullptr,
          "no emission and no world step creates no batch");
    batch = state.PrepareNativeBatch(103, true);
    Check(batch && batch->particle_spawns.empty() && batch->effect_lines.empty(),
          "world step can be submitted with empty emission channels");
    const auto step_identity = batch->identity;
    Check(state.AcknowledgeNativeBatch(step_identity, true), "empty world-step batch submission");

    batch = state.PrepareNativeBatch(104, true);
    const auto before_epoch_change = batch->identity;
    state.UpdateTerminalClock(104, hs::SessionPhase::Victory, hs::FixedStepClock::Clock::time_point{});
    Check(state.PrepareNativeBatch(104, true)->identity.sequence == before_epoch_change.sequence,
          "display clock changes do not cancel a frozen batch");
    batch = state.PrepareNativeBatch(90, true);
    Check(batch && batch->identity.stateful_target == 90 && batch->identity.reset_particle_state &&
          state.native_batch_active,
          "same-session rewind cancels old batch and requests particle reset");
    const auto rewind_identity = batch->identity;
    batch = state.PrepareNativeBatch(90, true);
    Check(batch->identity.sequence == rewind_identity.sequence &&
          batch->identity.stateful_target == rewind_identity.stateful_target &&
          batch->particle_spawns.data() == particle_storage &&
          batch->particle_spawns.capacity() == particle_capacity,
          "rewind retry preserves identity and retained batch storage");
    batch = state.PrepareNativeBatch(91, true);
    Check(batch->identity.sequence == rewind_identity.sequence &&
          batch->identity.stateful_target == rewind_identity.stateful_target,
          "newer target cannot replace an unsubmitted rewind retry");
    Check(state.AcknowledgeNativeBatch(rewind_identity, true) && !state.particle_reset_pending,
          "submitted rewind reset clears its pending reset request");

    batch = state.PrepareNativeBatch(91, true);
    const auto discarded_identity = batch->identity;
    state.DiscardNativeBatch();
    Check(!state.native_batch_active && state.last_committed_stateful_target == 90 &&
          !state.AcknowledgeNativeBatch(discarded_identity, true),
          "discarded catalog output must not advance the committed target or accept a late receipt");
    state.PrepareNativeBatch(91, true);
    Check(state.Synchronize(8) && !state.native_batch_active && !state.has_committed_stateful_target &&
          !state.particle_reset_pending,
          "session synchronization discards batch and target state");
}

int main()
{
    try
    {
        CheckActiveEffectRetention();
        CheckGeometrySectorIdentity();
        CheckOwnerBindingStorageReuse();
        CheckPendingEmissionOrder();
        CheckActivationBeforeNativeEmission();
        CheckFrozenNativeBatches();
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
        state.raw_events.AcknowledgeActivation();
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
            ordered.tick = i <= scheduler.kCapacity / 2 ? 4 : 5;
            ordered.sequence = (i + 1) / 2;
            ordered.asset = hs::AssetId{i};
            Check(scheduler.Enqueue(ordered), "bounded scheduler rejected in-capacity event");
        }
        hs::PresentationEvent overflow;
        overflow.session_id = 1;
        Check(!scheduler.Enqueue(overflow), "bounded scheduler exceeded capacity");
        const auto &ordered = scheduler.Prepare(5, 1);
        Check(ordered.entries.size() == scheduler.kCapacity, "raw FIFO missed ready entries");
        for (std::size_t i = 0; i < ordered.entries.size(); ++i)
            Check(ordered.entries[i].event.asset.value == i + 1,
                  "raw FIFO reordered same-tick projector fanout");
        scheduler.AcknowledgeActivation();
        hs::PresentationEvent future;
        future.session_id = 3; future.tick = 0; future.sequence = 3;
        hs::PresentationEvent current = future;
        current.session_id = 2; current.tick = 120; current.sequence = 2;
        Check(scheduler.Enqueue(current) && scheduler.Enqueue(future), "cross-session enqueue");
        const auto &current_batch = scheduler.Prepare(120, 2);
        Check(current_batch.entries.size() == 1 && current_batch.entries[0].event.sequence == 2 &&
              scheduler.pending.size() == 1 && scheduler.pending.front().event.session_id == 3,
              "future session must neither block nor activate in the current session");
        scheduler.AcknowledgeActivation();
        Check(scheduler.Prepare(200, 2).entries.empty() && scheduler.pending.size() == 1,
              "future session must retain ownership while the current session advances");
        scheduler.DiscardBeforeSession(3);
        Check(scheduler.Prepare(0, 3).entries.size() == 1,
              "retained future event must activate when its session begins");
        scheduler.AcknowledgeActivation();
        std::puts("VFX session state tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
