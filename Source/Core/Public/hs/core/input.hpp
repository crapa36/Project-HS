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
    bool move_held{};
    bool basic_attack_held{};
};

enum class UiActionKind : std::uint8_t
{
    StartSession, Quit, Reroll, SelectCard, AssignStat,
    Resume, ReturnToMainMenu, SwapLoadoutSlots,
};

struct UiAction
{
    UiActionKind kind{};
    std::uint8_t value{};
    std::uint8_t secondary{};

    bool operator==(const UiAction &) const = default;
};

struct InputFrame
{
    Tick target_tick{};
    HeldInputState held{};
    std::span<const ActionEdge> ordered_edges{};
    std::span<const UiAction> ui_actions{};
};

} // namespace hs
