#pragma once

#include <hs/gameplay/game_simulation.hpp>

namespace hs
{

// Copies the scoped view only when a development caller needs owned storage.
void CopyReadModelForDevelopment(const GameSimulation &simulation, GameReadModelStorage &model);

} // namespace hs
