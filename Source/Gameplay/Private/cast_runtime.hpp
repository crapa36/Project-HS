#pragma once

#include <hs/game_domain/game_types.hpp>

#include <cstdint>
#include <algorithm>
#include <span>
#include <vector>

namespace hs::gameplay_detail
{

[[nodiscard]] constexpr bool IsOldCast(std::uint64_t cast_id, std::uint64_t next_cast_id) noexcept
{
    return cast_id < next_cast_id && next_cast_id - cast_id > 256;
}

struct CastRuntime
{
    std::uint64_t cast_id{};
    SkillKind skill{SkillKind::Count};
    std::uint8_t upgrade_mask{};
    std::uint16_t normal_hits{};
    std::uint16_t kills{};
    std::uint8_t transfer_count{};
    std::uint8_t spawn_count{};
    Tick cooldown_ticks_saved{};
    bool triggered{};
    std::uint32_t basic_relic_triggers{};
    bool refund_triggered{};
    bool full_charge{};
    float stored_burn_attack{};
    Tick stored_burn_remaining{};
    bool has_stored_burn{};
    std::uint64_t terminal_target{};
};

struct CastCleanupMembership
{
    struct Entry { std::uint64_t id; bool live; };
    std::vector<Entry> entries;

    void Prepare(std::span<const CastRuntime> runtimes, std::uint64_t next_cast_id)
    {
        entries.clear();
        for (const auto &runtime : runtimes)
            if (IsOldCast(runtime.cast_id, next_cast_id) && runtime.cooldown_ticks_saved != 0)
                entries.push_back({runtime.cast_id, false});
        std::ranges::sort(entries, {}, &Entry::id);
    }
    void Observe(std::uint64_t cast_id)
    {
        const auto found = std::ranges::lower_bound(entries, cast_id, {}, &Entry::id);
        if (found != entries.end() && found->id == cast_id) found->live = true;
    }
    bool Contains(std::uint64_t cast_id) const
    {
        const auto found = std::ranges::lower_bound(entries, cast_id, {}, &Entry::id);
        return found != entries.end() && found->id == cast_id && found->live;
    }
};

} // namespace hs::gameplay_detail
