#pragma once

#include <hs/core/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace hs
{

enum class PresentationKind : std::uint8_t
{
    Vfx,
    Audio,
    Ui,
};

struct PresentationEvent
{
    Sequence sequence{};
    Tick tick{};
    PresentationKind kind{};
    Float3 position{};
    AssetId asset{};
    std::array<std::byte, 32> parameters{};
};

} // namespace hs
