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
    std::vector<VfxRange> ballistic_effect_ranges;
    std::vector<VfxBallisticRecipe> ballistic_recipes;
    std::vector<std::uint32_t> effect_output_profiles;
    std::vector<VfxRange> distortion_effect_ranges;
    std::vector<VfxDistortionRecipe> distortion_recipes;
    std::vector<VfxRange> decal_effect_ranges;
    std::vector<VfxDecalRecipe> decal_recipes;
    std::vector<VfxRange> ground_effect_ranges;
    std::vector<VfxGroundRecipe> ground_recipes;
    std::vector<VfxRange> flash_effect_ranges;
    std::vector<VfxFlashRecipe> flash_recipes;
    std::vector<VfxRange> persistent_stamp_effect_ranges;
    std::vector<VfxPersistentStampRecipe> persistent_stamp_recipes;
    std::vector<VfxRange> persistent_fire_effect_ranges;
    std::vector<VfxPersistentFireRecipe> persistent_fire_recipes;
    std::vector<VfxRange> owned_status_tick_effect_ranges;
    std::vector<VfxOwnedStatusTickRecipe> owned_status_tick_recipes;
    std::vector<VfxMoteSourceRecipe> mote_source_recipes;
    std::vector<VfxMoteOutputRecipe> mote_output_recipes;
    VfxImage View() const && = delete;
    [[nodiscard]] VfxImage View() const & noexcept
    {
        return {definitions, emitters, sprites, authored_mesh_indices, decorative_definitions,
                program.View(), has_typed_wavefront, sprite_count, payload_hash,
                sprite_bindings, has_required_sprite_bindings, light_effect_ranges, light_recipes,
                ballistic_effect_ranges, ballistic_recipes, effect_output_profiles,
                distortion_effect_ranges, distortion_recipes, decal_effect_ranges, decal_recipes,
                ground_effect_ranges, ground_recipes, flash_effect_ranges, flash_recipes,
                persistent_stamp_effect_ranges, persistent_stamp_recipes,
                persistent_fire_effect_ranges, persistent_fire_recipes,
                owned_status_tick_effect_ranges, owned_status_tick_recipes,
                mote_source_recipes, mote_output_recipes};
    }
};
// Rebuild after developer fixture mutation; unsupported Light rows are skipped.
void PrepareVfxLightRecipes(VfxProgramView program, std::vector<VfxRange> &ranges,
                            std::vector<VfxLightRecipe> &recipes);
void PrepareVfxBallisticRecipes(VfxProgramView program, std::vector<VfxRange> &ranges,
                                std::vector<VfxBallisticRecipe> &recipes);
void PrepareVfxDistortionRecipes(VfxProgramView program, std::vector<VfxRange> &ranges,
                                  std::vector<VfxDistortionRecipe> &recipes);
void PrepareVfxDecalRecipes(VfxProgramView program, std::vector<VfxRange> &ranges,
                            std::vector<VfxDecalRecipe> &recipes);
void PrepareVfxGroundRecipes(VfxProgramView program, std::vector<VfxRange> &ranges,
                             std::vector<VfxGroundRecipe> &recipes);
void PrepareVfxFlashRecipes(VfxProgramView program, std::vector<VfxRange> &ranges,
                            std::vector<VfxFlashRecipe> &recipes);
void PrepareVfxPersistentFlashRecipes(VfxProgramView program,
    std::vector<VfxRange> &stamp_ranges, std::vector<VfxPersistentStampRecipe> &stamps,
    std::vector<VfxRange> &fire_ranges, std::vector<VfxPersistentFireRecipe> &fire,
    std::vector<VfxRange> &tick_ranges, std::vector<VfxOwnedStatusTickRecipe> &ticks);
void PrepareVfxMoteRecipes(VfxProgramView program,
    std::vector<VfxMoteSourceRecipe> &sources,
    std::vector<VfxMoteOutputRecipe> &outputs);
void PrepareVfxOutputProfiles(VfxProgramView program, std::vector<std::uint32_t> &profiles);
[[nodiscard]] std::vector<std::byte> CanonicalVfxImageBytes(const VfxImage &image);
// Failure leaves output unchanged; successful decoding owns all view storage.
[[nodiscard]] Result DecodeVfxImage(std::span<const std::byte> legacy,
                                   std::span<const std::byte> program, VfxImageData &output);
[[nodiscard]] Result LoadVfxImage(const std::filesystem::path &legacy_path,
                                 const std::filesystem::path &program_path, VfxImageData &output);
}
