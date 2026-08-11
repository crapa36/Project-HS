#pragma once

#include <hs/core/types.hpp>

#include <cstdint>
#include <span>

namespace hs
{

enum class GameAction : std::uint8_t
{
    BasicAttack,
    SkillQ,
    SkillW,
    SkillE,
    SkillR,
    Pause,
    CharacterPage,
};

enum class EdgeKind : std::uint8_t
{
    Pressed,
    Released,
};

struct ActionEdge
{
    Sequence sequence{};
    GameAction action{};
    EdgeKind kind{};
};

struct HeldInputState
{
    Float3 move_target_world{};
    Float3 aim_world{};
    Float2 cursor_normalized{};
    Float2 ui_cursor_pixels{};
    bool move_held{};
    bool basic_attack_held{};
};

struct InputFrame
{
    Tick target_tick{};
    HeldInputState held{};
    std::span<const ActionEdge> ordered_edges{};
};

} // namespace hs
