#pragma once

#include <hs/core/result.hpp>
#include <hs/game_rules/simulation_rules.hpp>
#include <filesystem>
#include <span>
#include <vector>

namespace hs
{
[[nodiscard]] std::vector<std::byte> EncodeSimulationRules(const SimulationRules &rules);
[[nodiscard]] Result DecodeSimulationRules(std::span<const std::byte> bytes, SimulationRules &rules);
[[nodiscard]] Result LoadSimulationRules(const std::filesystem::path &path,
                                         SimulationRules &rules,
                                         std::uint64_t *content_hash = nullptr);
}
