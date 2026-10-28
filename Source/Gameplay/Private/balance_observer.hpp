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
    std::array<bool, kEnemyArchetypeCount> enemy_seen_zero{};
    std::vector<std::uint64_t> active_player_casts;
    std::array<std::vector<std::uint64_t>, kEnemyArchetypeCount> active_enemy_casts;

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
        enemy_seen_zero = {};
        for (auto &casts : enemy_hit_casts) casts.clear();
        player_hit_casts.clear();
        active_player_casts.clear();
        for (auto &casts : active_enemy_casts) casts.clear();
    }

    void Reset(bool capture_detailed, bool capture_hit_rates = true)
    {
        Reset();
        metrics.detailed_balance_captured = capture_detailed;
        metrics.hit_rates_captured = capture_hit_rates;
    }

    static void AddCause(std::vector<std::uint64_t> &causes, std::uint64_t cast)
    {
        if (cast != 0) causes.push_back(cast);
    }

    static void SortCauses(std::vector<std::uint64_t> &causes)
    {
        std::ranges::sort(causes);
        causes.erase(std::unique(causes.begin(), causes.end()), causes.end());
    }

    static void RetireInactive(std::vector<std::uint64_t> &seen,
                               const std::vector<std::uint64_t> &active)
    {
        std::erase_if(seen, [&](std::uint64_t cast) {
            return !std::ranges::binary_search(active, cast);
        });
    }

    bool RecordEnemyHit(std::size_t source, std::uint64_t masked_cast)
    {
        if (source >= kEnemyArchetypeCount) return false;
        if (masked_cast == 0)
        {
            const bool first = !enemy_seen_zero[source];
            enemy_seen_zero[source] = true;
            return first;
        }
        return RecordHit(enemy_hit_casts[source], masked_cast);
    }

    void RetireInactiveCauses()
    {
        SortCauses(active_player_casts);
        RetireInactive(player_hit_casts, active_player_casts);
        for (std::size_t source = 0; source < kEnemyArchetypeCount; ++source)
        {
            SortCauses(active_enemy_casts[source]);
            RetireInactive(enemy_hit_casts[source], active_enemy_casts[source]);
        }
    }
};
}
