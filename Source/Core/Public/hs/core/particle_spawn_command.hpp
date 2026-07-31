#pragma once

#include <hs/core/types.hpp>

#include <cstdint>

namespace hs
{

struct ParticleSpawnCommand
{
    Sequence sequence{};
    Tick tick{};
    Float3 position{};
    float lifetime{1.0f};
    Float3 velocity{};
    float velocity_spread{};
    Float4 start_color{1.0f, 1.0f, 1.0f, 1.0f};
    Float4 end_color{1.0f, 1.0f, 1.0f, 0.0f};
    float start_size{0.1f};
    float end_size{};
    float gravity{};
    float rotation{};
    float angular_velocity{};
    std::uint32_t sprite_index{};
    std::uint32_t sprite_count{1};
    std::uint32_t count{1};
};

} // namespace hs
