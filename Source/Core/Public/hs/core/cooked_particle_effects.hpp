#pragma once

#include <hs/core/types.hpp>

#include <cstdint>

namespace hs
{

using EffectId = AssetId;

enum class ParticleShape : std::uint8_t { Point, Sphere, Disc, Ring, Line };
enum class ParticleVelocity : std::uint8_t { Direction, Cone, Radial, Inward, Upward };
enum class ParticleFacing : std::uint8_t { Camera, Velocity, Ground };
enum class VfxRenderer : std::uint8_t { Sprite, Ground, Segment, Mesh };
enum class VfxPrimitive : std::uint8_t
{
    Soft,
    Disc,
    Ring,
    Sector,
    Chevron,
    Rune,
    Cracks,
    Arrow,
    Shard,
    Ember,
    Spike,
    ShockShell,
    SolidTrail,
    DashedRicochet,
    FireTransfer,
    RelicChain,
    DashWake,
};
using ParticleSprite = std::uint16_t;
enum class VfxDefinitionKind : std::uint8_t { Particles, Line };

struct CookedParticleEffectsHeader
{
    char magic[8]{'H', 'S', 'P', 'F', 'X', '\0', '\0', '\0'};
    std::uint32_t format_version{3};
    std::uint32_t effect_count{};
    std::uint32_t emitter_count{};
    std::uint32_t sprite_count{};
    std::uint64_t schema_hash{};
    std::uint64_t payload_hash{};
};

struct CookedParticleEmitter
{
    ParticleSprite sprite{};
    std::uint8_t frame_columns{1};
    std::uint8_t frame_rows{1};
    ParticleShape shape{};
    ParticleVelocity velocity{};
    ParticleFacing facing{};
    VfxRenderer renderer{};
    VfxPrimitive primitive{};
    Float3 local_offset{};
    Float3 shape_extent{};
    Float3 local_direction{0.0f, 1.0f, 0.0f};
    Float4 start_color{1.0f, 1.0f, 1.0f, 1.0f};
    Float4 end_color{1.0f, 1.0f, 1.0f, 0.0f};
    float lifetime_min{};
    float lifetime_max{};
    float speed_min{};
    float speed_max{};
    float cone_degrees{};
    float start_size_min{};
    float start_size_max{};
    float end_size_min{};
    float end_size_max{};
    float gravity{};
    float rotation_min{};
    float rotation_max{};
    float angular_velocity_min{};
    float angular_velocity_max{};
    float stretch{1.0f};
    std::uint32_t count{};
};

struct CookedParticleSprite
{
    std::uint64_t sprite_id{};
    ParticleSprite index{};
    std::uint8_t frame_columns{1};
    std::uint8_t frame_rows{1};
};

struct CookedVfxDefinition
{
    std::uint64_t effect_id{};
    std::uint32_t first_emitter{};
    std::uint16_t emitter_count{};
    VfxDefinitionKind kind{};
    std::uint8_t reserved{};
    Float4 line_color{};
    float line_width{};
    float line_lifetime{};
    ParticleSprite line_sprite{};
    std::uint8_t line_frame_columns{1};
    std::uint8_t line_frame_rows{1};
    float line_uv_repeat{1.0f};
    float line_scroll_speed{};
    VfxPrimitive line_primitive{VfxPrimitive::SolidTrail};
};

inline constexpr std::uint64_t kParticleEffectsSchemaHash = 0x4853504658563033ull;

} // namespace hs
