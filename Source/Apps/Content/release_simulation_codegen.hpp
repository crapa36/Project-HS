#pragma once

#include <hs/game_rules/simulation_rules.hpp>
#include <string>

namespace hs::content
{
// Input is the checked BuildGameData result; output belongs to the shipping link transaction.
[[nodiscard]] std::string EmitReleaseSimulationCpp(const SimulationRules &rules);
}
