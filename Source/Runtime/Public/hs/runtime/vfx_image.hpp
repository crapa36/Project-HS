#pragma once
#include <hs/core/cooked_particle_effects.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/renderer/vfx_spawn_command.hpp>
#include <span>

namespace hs
{
// Prepared Light outputs retain authored order and exact evaluation constants.
struct VfxLightRecipe
{
    std::uint64_t seed_xor{};
    std::uint32_t payload_kind{}, importance{}, min_quality{}, knot_count{};
    float seconds{}, radius{};
    std::array<float,4> knots{};
    Float3 rgb{};
    float hdr{}, alpha{};
    VfxRange curve_keys{};
    std::uint32_t interpolation{};
    bool hero{};
};
// Borrowed immutable rows. Generated storage lives for the process; developer
// storage must remain alive until all users of the adopted image have stopped.
struct VfxImage
{
    std::span<const CookedVfxDefinition> definitions;
    std::span<const CookedParticleEmitter> emitters;
    std::span<const CookedParticleSprite> sprites;
    std::span<const std::uint8_t> authored_mesh_indices;
    std::span<const std::uint8_t> decorative_definitions;
    VfxProgramView program;
    bool has_typed_wavefront{};
    std::uint32_t sprite_count{};
    std::uint64_t payload_hash{};
    RendererVfxSpriteBindings sprite_bindings{};
    bool has_required_sprite_bindings{};
    std::span<const VfxRange> light_effect_ranges;
    std::span<const VfxLightRecipe> light_recipes;
};
namespace shipping { [[nodiscard]] const VfxImage &Vfx() noexcept; }
}
