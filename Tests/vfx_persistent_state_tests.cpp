#include "vfx_persistent_state.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
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
        TestStatusCadenceDoesNotDependOnObservedRemainder();
        TestStatusEpisodeAndSubmissionBoundaries();
        TestStatusScheduleTickLimit();
        std::cout << "Persistent VFX status schedule tests passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
