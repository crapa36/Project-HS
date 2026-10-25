#pragma once
#include <hs/runtime/vfx_image.hpp>
#include <hs/core/result.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <filesystem>
#include <vector>

namespace hs
{
struct VfxImageData
{
    std::vector<CookedVfxDefinition> definitions;
    std::vector<CookedParticleEmitter> emitters;
    std::vector<CookedParticleSprite> sprites;
    std::vector<std::uint8_t> authored_mesh_indices;
    std::vector<std::uint8_t> decorative_definitions;
    VfxProgramData program;
    bool has_typed_wavefront{};
    std::uint32_t sprite_count{};
    std::uint64_t payload_hash{};
    RendererVfxSpriteBindings sprite_bindings{};
    bool has_required_sprite_bindings{};
    std::vector<VfxRange> light_effect_ranges;
    std::vector<VfxLightRecipe> light_recipes;
    VfxImage View() const && = delete;
    [[nodiscard]] VfxImage View() const & noexcept
    {
        return {definitions, emitters, sprites, authored_mesh_indices, decorative_definitions,
                program.View(), has_typed_wavefront, sprite_count, payload_hash,
                sprite_bindings, has_required_sprite_bindings, light_effect_ranges, light_recipes};
    }
};
// Rebuild after developer fixture mutation; unsupported Light rows are skipped.
void PrepareVfxLightRecipes(VfxProgramView program, std::vector<VfxRange> &ranges,
                            std::vector<VfxLightRecipe> &recipes);
// Failure leaves output unchanged; successful decoding owns all view storage.
[[nodiscard]] Result DecodeVfxImage(std::span<const std::byte> legacy,
                                   std::span<const std::byte> program, VfxImageData &output);
[[nodiscard]] Result LoadVfxImage(const std::filesystem::path &legacy_path,
                                 const std::filesystem::path &program_path, VfxImageData &output);
}
