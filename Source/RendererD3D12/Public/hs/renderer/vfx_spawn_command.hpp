#pragma once

#include <hs/core/cooked_particle_effects.hpp>
#include <hs/core/types.hpp>

#include <cstdint>

namespace hs
{

struct ParticleSpawnCommand
{
    Sequence sequence{};
    Tick tick{};
    Float3 position{};
    ParticleShape shape{};
    ParticleVelocity velocity_mode{};
    ParticleFacing facing{};
    VfxRenderer renderer{VfxRenderer::Sprite};
    VfxPrimitive primitive{VfxPrimitive::Soft};
    ParticleSprite sprite{};
    std::uint8_t frame_columns{1};
    std::uint8_t frame_rows{1};
    Float3 shape_extent{};
    Float3 direction{0.0f, 1.0f, 0.0f};
    float speed_min{};
    float speed_max{};
    float cone_radians{};
    float lifetime_min{};
    float lifetime_max{};
    Float4 start_color{1.0f, 1.0f, 1.0f, 1.0f};
    Float4 end_color{1.0f, 1.0f, 1.0f, 0.0f};
    float start_size_min{0.1f};
    float start_size_max{0.1f};
    float end_size_min{};
    float end_size_max{};
    float gravity{};
    float rotation_min{};
    float rotation_max{};
    float angular_velocity_min{};
    float angular_velocity_max{};
    float stretch{1.0f};
    std::uint32_t count{1};
    std::uint32_t seed{};
};

struct VfxLineSpawnCommand
{
    Sequence sequence{};
    Tick tick{};
    Float3 start{};
    Float3 end{};
    Float4 color{};
    float width{};
    float lifetime{};
    ParticleSprite sprite{};
    std::uint8_t frame_columns{1};
    std::uint8_t frame_rows{1};
    float uv_repeat{1.0f};
    float scroll_speed{};
    VfxPrimitive primitive{VfxPrimitive::SolidTrail};
};

} // namespace hs
