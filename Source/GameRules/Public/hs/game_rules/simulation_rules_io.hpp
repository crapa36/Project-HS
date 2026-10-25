#pragma once

#include <hs/core/result.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <filesystem>

namespace hs
{
[[nodiscard]] Result LoadSimulationRules(const std::filesystem::path &path,
                                         SimulationRules &rules,
                                         std::uint64_t *content_hash = nullptr);
}
