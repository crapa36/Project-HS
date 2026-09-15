#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace hs
{

inline constexpr std::size_t kArenaObstacleMaxCount = 32;

enum class ArenaObstacleKind : std::uint8_t
{
    Tree,
    Rock,
};

struct ArenaObstacle2D
{
    ArenaObstacleKind kind{ArenaObstacleKind::Tree};
    Float2 center{};
    float radius{};
};

}
