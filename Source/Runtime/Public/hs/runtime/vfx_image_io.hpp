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
    std::vector<VfxScreenEffectRecipe> screen_effect_recipes;
    std::vector<VfxScreenRecipe> screen_recipes;
    std::vector<VfxVolumeEffectRecipe> volume_effect_recipes;
    std::vector<VfxVolumeRecipe> volume_recipes;
    std::vector<VfxEntitySurfaceEffectRecipe> entity_surface_effect_recipes;
    std::vector<VfxEntitySurfaceRecipe> entity_surface_recipes;
    VfxEntitySurfaceRouteHandles entity_surface_routes{};
    std::vector<VfxRibbonEffectRecipe> ribbon_effect_recipes;
    std::vector<VfxRibbonSourceRecipe> ribbon_source_recipes;
    std::vector<VfxRibbonOutputRecipe> ribbon_output_recipes;
    std::vector<VfxRibbonTextureRecipe> ribbon_texture_recipes;
    std::vector<std::uint32_t> ribbon_slice_indices;
    std::vector<VfxRange> ribbon_wake_effect_ranges;
    std::vector<VfxRibbonWakeRecipe> ribbon_wake_recipes;
    std::vector<VfxRange> ribbon_fixed_effect_ranges;
    std::vector<VfxRibbonFixedRecipe> ribbon_fixed_recipes;
    std::vector<VfxRange> ribbon_projectile_effect_ranges;
    std::vector<VfxRibbonProjectileSourceRecipe> ribbon_projectile_sources;
    std::vector<VfxRibbonProjectileOutputRecipe> ribbon_projectile_outputs;
    std::vector<VfxRange> ribbon_cancel_effect_ranges;
    std::vector<VfxRibbonCancelRecipe> ribbon_cancel_recipes;
    std::vector<VfxRibbonLinkEffectRecipe> ribbon_link_effect_recipes;
    std::vector<VfxRibbonLinkOutputRecipe> ribbon_link_output_recipes;
    std::vector<VfxRibbonEventEffectRecipe> ribbon_event_effect_recipes;
    std::vector<VfxRibbonEventSourceRecipe> ribbon_event_sources;
    std::vector<VfxRibbonEventOutputRecipe> ribbon_event_outputs;
    std::vector<VfxEffectLookupRecord> effect_lookup_by_id;
    std::vector<VfxFresnelEffectRecipe> fresnel_effect_recipes;
    std::vector<VfxFresnelRecipe> fresnel_recipes;
    std::vector<VfxMeshEffectRecipe> mesh_effect_recipes;
    std::vector<VfxMeshRecipe> mesh_recipes;
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
                mote_source_recipes, mote_output_recipes,
                screen_effect_recipes, screen_recipes,
                volume_effect_recipes, volume_recipes,
                entity_surface_effect_recipes, entity_surface_recipes, entity_surface_routes,
                ribbon_effect_recipes, ribbon_slice_indices, ribbon_wake_effect_ranges,
                ribbon_wake_recipes, ribbon_fixed_effect_ranges, ribbon_fixed_recipes,
                ribbon_projectile_effect_ranges, ribbon_projectile_sources, ribbon_projectile_outputs,
                ribbon_cancel_effect_ranges, ribbon_cancel_recipes,
                ribbon_link_effect_recipes, ribbon_link_output_recipes,
                ribbon_event_effect_recipes, ribbon_event_sources, ribbon_event_outputs,
                effect_lookup_by_id,
                fresnel_effect_recipes, fresnel_recipes, mesh_effect_recipes, mesh_recipes};
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
[[nodiscard]] Result PrepareVfxScreenRecipes(VfxProgramView program,
    std::vector<VfxScreenEffectRecipe> &effects, std::vector<VfxScreenRecipe> &outputs);
[[nodiscard]] Result PrepareVfxVolumeRecipes(VfxProgramView program,
    std::vector<VfxVolumeEffectRecipe> &effects, std::vector<VfxVolumeRecipe> &outputs);
void PrepareVfxEntitySurfaceRecipes(VfxProgramView program,
    std::vector<VfxEntitySurfaceEffectRecipe> &effects, std::vector<VfxEntitySurfaceRecipe> &outputs,
    VfxEntitySurfaceRouteHandles &routes);
void PrepareVfxRibbonRecipes(VfxProgramView program,
    std::vector<VfxRibbonEffectRecipe> &effects, std::vector<VfxRibbonSourceRecipe> &sources,
    std::vector<VfxRibbonOutputRecipe> &outputs, std::vector<VfxRibbonTextureRecipe> &textures,
    std::vector<std::uint32_t> &slice_indices);
void PrepareVfxRibbonWakeRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRange> &ranges, std::vector<VfxRibbonWakeRecipe> &recipes);
void PrepareVfxRibbonFixedRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRange> &ranges, std::vector<VfxRibbonFixedRecipe> &recipes);
void PrepareVfxRibbonProjectileRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRange> &effect_ranges,
    std::vector<VfxRibbonProjectileSourceRecipe> &source_recipes,
    std::vector<VfxRibbonProjectileOutputRecipe> &output_recipes);
void PrepareVfxRibbonCancelRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::vector<VfxRange> &ranges, std::vector<VfxRibbonCancelRecipe> &recipes);
void PrepareVfxRibbonLinkRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRibbonLinkEffectRecipe> &effect_recipes,
    std::vector<VfxRibbonLinkOutputRecipe> &output_recipes);
void PrepareVfxRibbonEventRecipes(std::span<const VfxRibbonEffectRecipe> effects,
    std::span<const VfxRibbonSourceRecipe> sources,
    std::span<const VfxRibbonOutputRecipe> outputs,
    std::span<const VfxRibbonTextureRecipe> textures,
    std::span<const std::uint32_t> slice_indices,
    std::vector<VfxRibbonEventEffectRecipe> &effect_recipes,
    std::vector<VfxRibbonEventSourceRecipe> &source_recipes,
    std::vector<VfxRibbonEventOutputRecipe> &output_recipes);
void PrepareVfxEffectLookupById(VfxProgramView program,
    std::vector<VfxEffectLookupRecord> &lookup_by_id);
void PrepareVfxFresnelRecipes(VfxProgramView program,
    std::vector<VfxFresnelEffectRecipe> &effects, std::vector<VfxFresnelRecipe> &outputs);
void PrepareVfxMeshRecipes(VfxProgramView program,
    std::vector<VfxMeshEffectRecipe> &effects, std::vector<VfxMeshRecipe> &outputs);
void PrepareVfxOutputProfiles(VfxProgramView program, std::vector<std::uint32_t> &profiles);
[[nodiscard]] std::vector<std::byte> CanonicalVfxImageBytes(const VfxImage &image);
// Failure leaves output unchanged; successful decoding owns all view storage.
[[nodiscard]] Result DecodeVfxImage(std::span<const std::byte> legacy,
                                   std::span<const std::byte> program, VfxImageData &output);
[[nodiscard]] Result LoadVfxImage(const std::filesystem::path &legacy_path,
                                 const std::filesystem::path &program_path, VfxImageData &output);
}
