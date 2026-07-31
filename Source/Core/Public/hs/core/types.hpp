#pragma once

#include <cstdint>

namespace hs
{

using Tick = std::uint64_t;
using Sequence = std::uint64_t;
using GameplayChecksum = std::uint64_t;

struct Float2
{
    float x{};
    float y{};
};

struct Float3
{
    float x{};
    float y{};
    float z{};
};

struct Float4
{
    float x{};
    float y{};
    float z{};
    float w{};
};

struct AssetId
{
    std::uint64_t value{};
};

enum class BarrierMode : std::uint8_t
{
    Automatic,
    Enhanced,
    Legacy,
};

} // namespace hs
