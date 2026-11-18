#pragma once
#include <hs/runtime/vfx_image.hpp>
#include <hs/core/result.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <vector>

namespace hs
{
struct VfxImageData
{
    std::vector<CookedVfxDefinition> definitions;
    std::vector<CookedParticleEmitter> emitters;
    std::vector<CookedParticleSprite> sprites;
    std::vector<std::uint8_t> authored_mesh_indices;
    std::vector<VfxLegacyEffectPolicy> legacy_effect_policies;
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
    std::vector<VfxFlashEffectRecipe> flash_effect_recipes;
    std::vector<VfxFlashRecipe> flash_recipes;
    std::vector<VfxRange> persistent_stamp_effect_ranges;
    std::vector<VfxPersistentStampRecipe> persistent_stamp_recipes;
    std::vector<VfxRange> persistent_fire_effect_ranges;
    std::vector<VfxPersistentFireRecipe> persistent_fire_recipes;
    std::vector<VfxRange> owned_status_tick_effect_ranges;
    std::vector<VfxOwnedStatusTickRecipe> owned_status_tick_recipes;
    std::vector<VfxMoteSourceRecipe> mote_source_recipes;
    std::vector<VfxMoteOutputRecipe> mote_output_recipes;
    std::vector<VfxSparkSchedule> spark_schedules;
    std::vector<VfxScreenEffectRecipe> screen_effect_recipes;
    std::vector<VfxScreenRecipe> screen_recipes;
    std::vector<VfxVolumeEffectRecipe> volume_effect_recipes;
    std::vector<VfxVolumeRecipe> volume_recipes;
    std::vector<VfxEntitySurfaceEffectRecipe> entity_surface_effect_recipes;
    std::vector<VfxEntitySurfaceRecipe> entity_surface_recipes;
    VfxEntitySurfaceRouteHandles entity_surface_routes{};
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
        return {definitions, emitters, sprites, authored_mesh_indices, legacy_effect_policies,
                program.View(), has_typed_wavefront, sprite_count, payload_hash,
                sprite_bindings, has_required_sprite_bindings, light_effect_ranges, light_recipes,
                ballistic_effect_ranges, ballistic_recipes, effect_output_profiles,
                distortion_effect_ranges, distortion_recipes, decal_effect_ranges, decal_recipes,
                ground_effect_ranges, ground_recipes, flash_effect_recipes, flash_recipes,
                persistent_stamp_effect_ranges, persistent_stamp_recipes,
                persistent_fire_effect_ranges, persistent_fire_recipes,
                owned_status_tick_effect_ranges, owned_status_tick_recipes,
                mote_source_recipes, mote_output_recipes, spark_schedules,
                screen_effect_recipes, screen_recipes,
                volume_effect_recipes, volume_recipes,
                entity_surface_effect_recipes, entity_surface_recipes, entity_surface_routes,
                ribbon_wake_effect_ranges,
                ribbon_wake_recipes, ribbon_fixed_effect_ranges, ribbon_fixed_recipes,
                ribbon_projectile_effect_ranges, ribbon_projectile_sources, ribbon_projectile_outputs,
                ribbon_cancel_effect_ranges, ribbon_cancel_recipes,
                ribbon_link_effect_recipes, ribbon_link_output_recipes,
                ribbon_event_effect_recipes, ribbon_event_sources, ribbon_event_outputs,
                effect_lookup_by_id,
                fresnel_effect_recipes, fresnel_recipes, mesh_effect_recipes, mesh_recipes};
    }
};
// The program must satisfy the table and range contracts checked by LoadVfxProgram.
void PrepareVfxMoteRecipes(VfxImageData &image);
[[nodiscard]] std::vector<std::byte> CanonicalVfxImageBytes(const VfxImage &image);
// Both input spans have been verified against their content digests.
// Failure leaves output unchanged; successful decoding owns all view storage.
[[nodiscard]] Result DecodeVfxImage(std::span<const std::byte> legacy,
                                   std::span<const std::byte> program, VfxImageData &output);
}
