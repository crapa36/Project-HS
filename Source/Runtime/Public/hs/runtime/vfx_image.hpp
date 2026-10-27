#pragma once
#include <hs/core/cooked_particle_effects.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>
#include <hs/render_data/vfx_distortion_input.hpp>
#include <hs/render_data/vfx_decal_input.hpp>
#include <hs/render_data/vfx_frame_input.hpp>
#include <array>
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
// Prepared immutable ballistic mesh outputs. One row corresponds to one
// accepted source/output pair, in authored order; runtime builders only bind
// live event or persistent-owner data to these constants.
struct VfxBallisticRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_index{}, source_stable_id{}, payload_kind{}, importance{}, timing_kind{};
    std::uint32_t shape{}, min_quality{}, gradient_row{}, mesh_index{}, count{};
    std::uint32_t knot_count{};
    VfxSourceType source_type{};
    VfxMotionKind motion{};
    std::array<float, 4> knots{};
    float seconds{}, min_size{}, max_size{}, min_speed{}, max_speed{};
    float gravity{}, bounce{}, drag{.8f}, speed{-1}, cone{}, min_spin{}, max_spin{};
    Float4 color{};
    float hdr{};
    bool persistent_status{};
};
struct VfxDistortionRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_stable_id{}, payload_kind{}, importance{};
    std::uint32_t input_mode{}, timing_kind{}, min_quality{}, knot_count{};
    VfxDistortionShape kind{VfxDistortionShape::RadialRing};
    VfxMotionKind motion{};
    std::array<float, 4> knots{};
    float seconds{}, radius{}, strength{}, alpha{};
};
struct VfxDecalRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_stable_id{}, payload_kind{}, importance{};
    std::uint32_t input_mode{}, timing_kind{}, min_quality{}, knot_count{};
    VfxDecalKind kind{VfxDecalKind::Fracture};
    std::array<float, 4> knots{};
    float seconds{}, hdr{}, alpha{}, edge_glow{};
    Float3 linear_rgb{};
    std::uint32_t gradient_row{}, voronoi_cells{}, texture_slice_base{}, texture_min_quality{};
};
enum class VfxGroundRecipeKind : std::uint8_t
{
    Ring, Fill, Border, Hatch, Preview, Ticks, ExpandingRing,
    HexConstellation, BossSignature, CrossRing, BrokenHex, AxialFracture,
    StateRing, PolarRune, BrokenCrown, ClosedCrownRing, RepeatingChevron,
    SafeSector, ChargedBoundary, ChargedInterior
};
struct VfxGroundRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_index{}, source_stable_id{}, output_index{}, gradient_row{};
    std::uint32_t payload_kind{}, timing_kind{}, min_quality{};
    VfxSourceType source_type{};
    VfxOutputProfile profile{VfxOutputProfile::GroundSdfSoft};
    VfxMotionKind motion{};
    VfxGroundRecipeKind kind{VfxGroundRecipeKind::Ring};
    std::uint32_t knot_count{};
    std::array<float,4> knots{};
    VfxRange pulse_curve_keys{};
    std::uint32_t pulse_curve_interpolation{}, tick_count{};
    float seconds{}, primary{}, secondary{};
    Float3 rgb{};
    float hdr{}, alpha{}, motion_rate_hz{}, motion_amplitude{}, motion_inset_fraction{};
    float tertiary{};
    struct MaskVariant
    {
        VfxRange slices{};
        float strength{};
        bool valid{true}, present{};
    };
    std::array<MaskVariant,3> mask_variants{};
    bool texture_contract_valid{};
    std::uint32_t source_output_count{};
    bool source_parameters_empty{};
};
struct VfxFlashRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_index{}, source_stable_id{}, output_index{};
    std::uint32_t payload_kind{}, min_quality{};
    std::array<float,2> knots{};
    float seconds{};
    std::uint32_t coverage_quality{};
    VfxFlashSpawnInput spawn{};
    std::array<VfxRange,3> mask_slice_ranges{};
    std::array<float,3> mask_strengths{};
    std::uint8_t supported_quality_mask{}, mask_present_mask{};
    bool unique_effect_identity{};
};
struct VfxPersistentStampRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_visual_kind{}, payload_kind{}, source_stable_id{};
    std::uint32_t timing_kind{};
    float fixed_seconds{}, source_end{};
    VfxFlashSpawnInput spawn{};
    std::array<VfxRange,3> mask_slices{};
    std::array<float,3> mask_strengths{};
    std::uint8_t supported_quality_mask{}, mask_present_mask{};
    bool bleed_extend{};
};
struct VfxPersistentFireRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_stable_id{}, rate{};
    float radius_scale{};
    VfxFlashSpawnInput spawn{};
    VfxRange mask_slices{};
    float mask_strength{};
};
struct VfxOwnedStatusTickRecipe
{
    std::uint64_t tick_effect_id{}, status_effect_id{};
    std::uint32_t tick_effect_handle{}, status_effect_handle{}, min_quality{};
    float tick_seconds{}, delay_fraction{}, lifetime_fraction{};
    VfxFlashSpawnInput spawn{};
    std::array<std::uint32_t,3> mask_slices{};
    float mask_strength{};
    bool bleed{};
};
// Mote source/output records keep immutable authored parameter and texture
// interpretation out of per-event and per-frame builders. Rows are indexed by
// their source/output table index, so ordinary lookups are bounds checks.
struct VfxMoteSourceRecipe
{
    std::uint32_t count{};
    std::int32_t rate{};
    float curl{.2f}, drag{.8f}, speed{}, cone_degrees{};
    bool common_params_valid{}, rate_valid{}, residual_valid{}, spark_valid{};
};
struct VfxMoteOutputRecipe
{
    std::uint32_t min_quality{}, gradient_row{};
    std::array<VfxRange,3> mask_slices{};
    std::array<float,3> mask_strengths{};
    std::uint32_t persistent_curve_row{0xffffffffu};
    float persistent_size{}, hdr{};
    Float4 color{};
    std::uint8_t mask_present_mask{}, mask_quality_mask{0x7};
    bool output_color_valid{}, persistent_params_valid{}, spark_textures_valid{};
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
    std::span<const VfxRange> ballistic_effect_ranges;
    std::span<const VfxBallisticRecipe> ballistic_recipes;
    std::span<const std::uint32_t> effect_output_profiles;
    std::span<const VfxRange> distortion_effect_ranges;
    std::span<const VfxDistortionRecipe> distortion_recipes;
    std::span<const VfxRange> decal_effect_ranges;
    std::span<const VfxDecalRecipe> decal_recipes;
    std::span<const VfxRange> ground_effect_ranges;
    std::span<const VfxGroundRecipe> ground_recipes;
    std::span<const VfxRange> flash_effect_ranges;
    std::span<const VfxFlashRecipe> flash_recipes;
    std::span<const VfxRange> persistent_stamp_effect_ranges;
    std::span<const VfxPersistentStampRecipe> persistent_stamp_recipes;
    std::span<const VfxRange> persistent_fire_effect_ranges;
    std::span<const VfxPersistentFireRecipe> persistent_fire_recipes;
    std::span<const VfxRange> owned_status_tick_effect_ranges;
    std::span<const VfxOwnedStatusTickRecipe> owned_status_tick_recipes;
    std::span<const VfxMoteSourceRecipe> mote_source_recipes;
    std::span<const VfxMoteOutputRecipe> mote_output_recipes;

    [[nodiscard]] bool HasOutput(std::uint32_t handle, VfxOutputProfile profile) const noexcept
    {
        const auto bit = static_cast<unsigned>(profile);
        return handle != 0 && handle <= effect_output_profiles.size() && bit < 32 &&
            (effect_output_profiles[handle - 1] & (1u << bit)) != 0;
    }
    [[nodiscard]] bool HasDistortionRecipes(std::uint32_t handle) const noexcept
    {
        if (!handle || handle > distortion_effect_ranges.size()) return false;
        const auto range = distortion_effect_ranges[handle - 1];
        return range.count && range.first <= distortion_recipes.size() &&
            range.count <= distortion_recipes.size() - range.first;
    }
    [[nodiscard]] bool HasDecalRecipes(std::uint32_t handle) const noexcept
    {
        if (!handle || handle > decal_effect_ranges.size()) return false;
        const auto range = decal_effect_ranges[handle - 1];
        return range.count && range.first <= decal_recipes.size() &&
            range.count <= decal_recipes.size() - range.first;
    }
    [[nodiscard]] bool HasGroundRecipes(std::uint32_t handle) const noexcept
    {
        if (!handle || handle > ground_effect_ranges.size()) return false;
        const auto range = ground_effect_ranges[handle - 1];
        return range.count && range.first <= ground_recipes.size() &&
            range.count <= ground_recipes.size() - range.first;
    }
    [[nodiscard]] bool HasFlashRecipes(std::uint32_t handle) const noexcept
    {
        if (!handle || handle > flash_effect_ranges.size()) return false;
        const auto range = flash_effect_ranges[handle - 1];
        return range.count && range.first <= flash_recipes.size() &&
            range.count <= flash_recipes.size() - range.first;
    }
};
namespace shipping { [[nodiscard]] const VfxImage &Vfx() noexcept; }
}
