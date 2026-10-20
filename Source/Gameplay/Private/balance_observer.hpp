#pragma once

#include <hs/game_domain/game_types.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace hs::gameplay_detail
{
struct BalanceObserver
{
    BalanceTelemetry metrics{};
    std::array<std::vector<std::uint64_t>, kEnemyArchetypeCount> enemy_hit_casts;
    std::vector<std::uint64_t> player_hit_casts;

    static bool RecordHit(std::vector<std::uint64_t> &casts, std::uint64_t cast)
    {
        if (casts.empty() || casts.back() < cast)
        {
            casts.push_back(cast);
            return true;
        }
        if (casts.back() == cast) return false;
        const auto position = std::lower_bound(casts.begin(), casts.end(), cast);
        if (*position == cast) return false;
        casts.insert(position, cast);
        return true;
    }

    void Reset()
    {
        metrics = {};
        for (auto &casts : enemy_hit_casts) casts.clear();
        player_hit_casts.clear();
    }
};
}
