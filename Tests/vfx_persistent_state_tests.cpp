#include "vfx_persistent_state.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

hs::VfxPersistentInput Record(std::uint64_t id, hs::VfxEffectHandle effect, float age)
{
    hs::VfxPersistentInput input;
    input.stable_id = id;
    input.effect_handle = effect;
    input.normalized_age = age;
    return input;
}

void TestAuthoritativePresence()
{
    hs::renderer_detail::VfxPersistentState state;
    std::string error;
    const std::array inputs{Record(12, 9, .5f), Record(7, 8, 1.0f), Record(7, 3, 0.0f)};
    Check(state.Update(inputs, error), "valid frame rejected");
    const auto active = state.Active();
    Check(active.size() == 3, "same owner must retain different effects");
    Check(active[0].stable_id == 7 && active[0].effect_handle == 3 &&
          active[1].stable_id == 7 && active[1].effect_handle == 8 &&
          active[2].stable_id == 12, "records must sort deterministically by compound key");
    const std::array remaining{Record(7, 8, .2f)};
    Check(state.Update(remaining, error), "replacement frame rejected");
    Check(state.Active().size() == 1 && state.Active()[0].effect_handle == 8,
          "omitted effects must disappear in this frame");
    Check(state.Active()[0].normalized_age == .2f, "authoritative age can rewind");
    Check(state.Update({}, error) && state.Active().empty(), "empty frame must remove every effect immediately");
    Check(state.Update(inputs, error) && state.Active().size() == 3, "omitted identity can reappear");
}

void TestInvalidFrameIsAtomic()
{
    hs::renderer_detail::VfxPersistentState state;
    std::string error;
    const std::array original{Record(4, 2, .4f)};
    Check(state.Update(original, error), "initial frame rejected");
    const auto reject = [&](const hs::VfxPersistentInput &invalid)
    {
        const std::array candidate{Record(1, 0, .1f), invalid};
        Check(!state.Update(candidate, error) && !error.empty(), "invalid frame must report failure");
        Check(state.Active().size() == 1 && state.Active()[0].stable_id == 4 &&
              state.Active()[0].effect_handle == 2 && state.Active()[0].normalized_age == .4f,
              "invalid frame changed prior state");
    };
    reject(Record(1, 0, .8f)); // Duplicate identity, despite different age.
    reject(Record(2, hs::kInvalidVfxEffectHandle, .5f));
    reject(Record(2, 3, -.001f));
    reject(Record(2, 3, 1.001f));
    reject(Record(2, 3, std::numeric_limits<float>::quiet_NaN()));
    reject(Record(2, 3, std::numeric_limits<float>::infinity()));
    reject(Record(2, 3, -std::numeric_limits<float>::infinity()));
    Check(state.Update(original, error) && error.empty(), "successful update must clear prior error");
}

void TestGeometryAndTransformsAreRetained()
{
    hs::renderer_detail::VfxPersistentState state;
    std::string error;
    auto input = Record(42, 6, .625f);
    input.current_transform[12] = 17.0f;
    input.previous_transform[12] = -3.0f;
    input.state_flags = 0x35;
    input.stable_seed = 109;
    input.quality = hs::VfxQuality::Low;
    hs::VfxRingGapsPayload geometry;
    geometry.center = {2.0f, 3.0f, 4.0f};
    geometry.inner_radius = 2.5f;
    geometry.outer_radius = 7.25f;
    geometry.gap_half_width_degrees = 12.0f;
    geometry.lifetime01 = .75f;
    geometry.state_flags = 9;
    geometry.gap_angles_degrees = {10.0f, 70.0f, 205.0f};
    input.payload = geometry;
    std::array frame{input};
    Check(state.Update(frame, error), "geometry frame rejected");
    std::get<hs::VfxRingGapsPayload>(frame[0].payload).gap_angles_degrees.clear();
    const auto &result = state.Active()[0];
    const auto &retained = std::get<hs::VfxRingGapsPayload>(result.payload);
    Check(result.current_transform == input.current_transform && result.previous_transform == input.previous_transform,
          "authoritative transforms must not be reconstructed or integrated");
    Check(result.normalized_age == input.normalized_age && result.state_flags == input.state_flags &&
          result.stable_seed == input.stable_seed && result.quality == input.quality, "envelope metadata changed");
    Check(retained.center.x == 2.0f && retained.center.y == 3.0f && retained.center.z == 4.0f &&
          retained.inner_radius == geometry.inner_radius && retained.outer_radius == geometry.outer_radius &&
          retained.gap_half_width_degrees == geometry.gap_half_width_degrees &&
          retained.lifetime01 == geometry.lifetime01 && retained.state_flags == geometry.state_flags &&
          retained.gap_angles_degrees == geometry.gap_angles_degrees, "geometry must be retained as an owned exact copy");
    // Reusing the current frame must also work when its span aliases internal storage.
    Check(state.Update(state.Active(), error), "self-sourced frame rejected");
    Check(state.Active()[0].normalized_age == .625f, "reconciliation must not advance age");
}

hs::PersistentVfxVisual StatusEpisode(hs::StatusVisual status, hs::Tick first = 1,
                                    hs::Tick expires = 1000)
{
    hs::PersistentVfxVisual episode;
    episode.kind = status == hs::StatusVisual::Burn ? hs::PersistentVfxKind::EnemyBurnStatus
        : hs::PersistentVfxKind::EnemyBleedStatus;
    episode.stable_id = 17;
    episode.entity_render_id = 2;
    episode.status_episode_generation = 1;
    episode.active_tick = first;
    episode.expires = expires;
    return episode;
}

std::vector<hs::Tick> Births(const hs::PersistentVfxVisual &episode, hs::StatusVisual status,
                           hs::Tick now, hs::Tick last, bool reset, hs::Tick maximum_age = 30)
{
    const auto range = hs::renderer_detail::StatusParticleBirths(&episode,
        episode.entity_render_id, status, now, last, reset, maximum_age);
    std::vector<hs::Tick> result;
    if (range)
        for (hs::Tick birth = range->first;; birth += range->cadence)
        {
            result.push_back(birth);
            if (range->last - birth < range->cadence) break;
        }
    return result;
}

void TestStatusCadenceDoesNotDependOnObservedRemainder()
{
    for (const auto status : {hs::StatusVisual::Bleed, hs::StatusVisual::Burn})
    {
        const auto episode = StatusEpisode(status);
        const auto consume = [&](hs::Tick step) {
            std::vector<hs::Tick> result;
            hs::Tick last{};
            for (hs::Tick now = step; now <= 60; now += step)
            {
                const auto scheduled = Births(episode, status, now, last, last == 0);
                result.insert(result.end(), scheduled.begin(), scheduled.end());
                last = now;
            }
            return result;
        };
        const auto every_tick = consume(1);
        Check(every_tick == consume(3) && every_tick == consume(4),
              "skipped observations changed known status birth ticks");
        Check(every_tick.size() == (status == hs::StatusVisual::Burn ? 20 : 10),
              "status cadence lost the original 60 Hz schedule");
    }
}

void TestStatusEpisodeAndSubmissionBoundaries()
{
    auto episode = StatusEpisode(hs::StatusVisual::Burn);
    const auto prepared = Births(episode, hs::StatusVisual::Burn, 30, 24, false, 27);
    Check(prepared == std::vector<hs::Tick>{25, 28} &&
          prepared == Births(episode, hs::StatusVisual::Burn, 30, 24, false, 27),
          "unsubmitted retry changed or consumed the birth range");
    Check(Births(episode, hs::StatusVisual::Burn, 30, 30, false).empty(),
          "paused/same-tick or another shared view spawned duplicate particles");
    const auto catch_up = Births(episode, hs::StatusVisual::Burn, 100, 1, false, 27);
    Check(catch_up == std::vector<hs::Tick>({76, 79, 82, 85, 88, 91, 94, 97, 100}),
          "catch-up revived expired births or omitted live ones");
    episode.active_tick = 55;
    ++episode.status_episode_generation;
    Check(Births(episode, hs::StatusVisual::Burn, 60, 1, false) ==
              std::vector<hs::Tick>({55, 58}),
          "reapplied status inherited births from the prior episode");
    Check(Births(episode, hs::StatusVisual::Burn, 60, 60, true) ==
              std::vector<hs::Tick>({55, 58}),
          "fresh session/pool did not reconstruct its known live episode");
    episode.expires = 60;
    Check(Births(episode, hs::StatusVisual::Burn, 60, 1, false).empty(),
          "authoritative expiry was extended by the renderer");
    episode = StatusEpisode(hs::StatusVisual::Burn);
    Check(Births(episode, hs::StatusVisual::Burn, 10, 60, true) ==
              std::vector<hs::Tick>({1, 4, 7, 10}),
          "rewound fresh particle pool retained the future schedule");
    const auto unknown = hs::renderer_detail::StatusParticleBirths(nullptr, 2,
        hs::StatusVisual::Burn, 61, 1, false, 27);
    Check(unknown && unknown->first == 61 && unknown->last == 61,
          "missing episode metadata invented past active duration");
    Check(!hs::renderer_detail::StatusParticleBirths(nullptr, 2,
        hs::StatusVisual::Burn, 60, 1, false, 27),
          "compatibility fallback shifted the original cadence phase");
}

void TestStatusScheduleTickLimit()
{
    const auto limit = std::numeric_limits<hs::Tick>::max();
    auto episode = StatusEpisode(hs::StatusVisual::Bleed, limit - 40, limit);
    episode.entity_render_id = limit;
    const auto scheduled = Births(episode, hs::StatusVisual::Bleed, limit - 1, limit - 35, false);
    Check(!scheduled.empty() && std::ranges::all_of(scheduled, [&](hs::Tick birth) {
        return birth <= limit - 1 && limit - 1 - birth < 30 &&
               (birth % 6 + episode.entity_render_id % 6) % 6 == 0;
    }), "status cadence overflowed at the Tick limit");
    Check(!hs::renderer_detail::StatusParticleBirths(nullptr, limit, hs::StatusVisual::Bleed,
        limit, limit, false, 30), "submitted maximum Tick was replayed");
}
}

int main()
{
    try
    {
        TestAuthoritativePresence();
        TestInvalidFrameIsAtomic();
        TestGeometryAndTransformsAreRetained();
        TestStatusCadenceDoesNotDependOnObservedRemainder();
        TestStatusEpisodeAndSubmissionBoundaries();
        TestStatusScheduleTickLimit();
        std::cout << "Persistent VFX reconciliation tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
