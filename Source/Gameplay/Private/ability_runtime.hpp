#pragma once

#include <hs/game_domain/game_types.hpp>

#include <cstdint>

namespace hs::gameplay_detail
{

struct CastRuntime
{
    std::uint64_t cast_id{};
    SkillKind skill{SkillKind::Count};
    std::uint8_t upgrade_mask{};
    std::uint16_t normal_hits{};
    std::uint16_t kills{};
    std::uint8_t transfer_count{};
    std::uint8_t spawn_count{};
    bool triggered{};
    std::uint32_t basic_relic_triggers{};
    bool refund_triggered{};
    bool full_charge{};
    float stored_burn_attack{};
    Tick stored_burn_remaining{};
    bool has_stored_burn{};
    std::uint64_t terminal_target{};
};

} // namespace hs::gameplay_detail
