#pragma once
#include <hs/core/cooked_particle_effects.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <hs/render_data/vfx_spawn_command.hpp>
#include <hs/render_data/vfx_distortion_input.hpp>
#include <hs/render_data/vfx_decal_input.hpp>
#include <hs/render_data/vfx_frame_input.hpp>
#include <hs/render_data/vfx_screen_input.hpp>
#include <hs/render_data/vfx_volume_input.hpp>
#include <hs/render_data/vfx_entity_surface_input.hpp>
#include <hs/render_data/vfx_fresnel_input.hpp>
#include <hs/render_data/vfx_ribbon_input.hpp>
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
inline constexpr std::uint32_t kVfxGroundEventDirectAdd = 1u;
inline constexpr std::uint32_t kVfxGroundEventDirectSoft = 2u;
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
    std::uint32_t source_output_count{}, effect_lookup_count{};
    bool source_parameters_empty{};
    // Aggregated once per effect; only the first row in its range carries these bits.
    std::uint32_t event_admission_profiles{};
};
// Ground is consumed by both the canonical identity writer and release C++
// codegen. Keep its semantic field inventory in one typed list so a new field
// cannot silently be omitted by either sink.
template <typename Visit>
constexpr void VisitVfxGroundMaskVariantFields(const VfxGroundRecipe::MaskVariant &mask, Visit &&visit)
{
    visit("slices", mask.slices);
    visit("strength", mask.strength);
    visit("valid", mask.valid);
    visit("present", mask.present);
}
template <typename Visit>
constexpr void VisitVfxGroundRecipeFields(const VfxGroundRecipe &r, Visit &&visit)
{
    visit("effect_id", r.effect_id);
    visit("effect_handle", r.effect_handle);
    visit("source_index", r.source_index);
    visit("source_stable_id", r.source_stable_id);
    visit("output_index", r.output_index);
    visit("gradient_row", r.gradient_row);
    visit("payload_kind", r.payload_kind);
    visit("timing_kind", r.timing_kind);
    visit("min_quality", r.min_quality);
    visit("source_type", r.source_type);
    visit("profile", r.profile);
    visit("motion", r.motion);
    visit("kind", r.kind);
    visit("knot_count", r.knot_count);
    visit("knots", r.knots);
    visit("pulse_curve_keys", r.pulse_curve_keys);
    visit("pulse_curve_interpolation", r.pulse_curve_interpolation);
    visit("tick_count", r.tick_count);
    visit("seconds", r.seconds);
    visit("primary", r.primary);
    visit("secondary", r.secondary);
    visit("rgb", r.rgb);
    visit("hdr", r.hdr);
    visit("alpha", r.alpha);
    visit("motion_rate_hz", r.motion_rate_hz);
    visit("motion_amplitude", r.motion_amplitude);
    visit("motion_inset_fraction", r.motion_inset_fraction);
    visit("tertiary", r.tertiary);
    visit("mask_variants", r.mask_variants);
    visit("texture_contract_valid", r.texture_contract_valid);
    visit("source_output_count", r.source_output_count);
    visit("effect_lookup_count", r.effect_lookup_count);
    visit("source_parameters_empty", r.source_parameters_empty);
    visit("event_admission_profiles", r.event_admission_profiles);
}
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
    bool authoritative{};
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
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, effect_lookup_count{}, stable_id{};
    VfxRange output_range{};
    std::array<float,2> knots{};
    bool output_range_valid{}, common_route_valid{}, residual_route_valid{}, rate_route_valid{}, tick_route_valid{}, spark_route_valid{};
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
    std::uint32_t common_recipe_index{0xffffffffu};
    bool empty_parameters_valid{}, residual_route_valid{}, rate_route_valid{}, tick_route_valid{}, persistent_route_valid{}, spark_route_valid{};
};
struct VfxScreenEffectRecipe
{
    std::uint64_t effect_id{};
    float seconds{};
    VfxRange outputs{};
    bool route_valid{};
};
struct VfxScreenRecipe
{
    std::uint32_t source_index{}, output_index{}, min_quality{}, gradient_row{};
    VfxScreenShape shape{};
    float knot_start{}, knot_end{};
    Float3 rgb{};
    float alpha{}, hdr{}, center_clear{0.62f}, edge_power{2.8f};
};
struct VfxVolumeEffectRecipe
{
    std::uint64_t effect_id{};
    float seconds{};
    VfxRange outputs{};
    bool route_valid{};
};
struct VfxVolumeRecipe
{
    std::uint32_t source_index{}, output_index{}, source_id{}, min_quality{}, gradient_row{};
    VfxVolumeShape shape{};
    VfxVolumeNoise noise{};
    float knot_start{}, knot_end{}, density{1.0f}, alpha{}, anisotropy{}, hdr{};
    std::uint32_t steps{}, half_resolution{};
    Float3 rgb{};
};
struct VfxEntitySurfaceEffectRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t importance{}, payload_kind{}, input_mode{}, timing_kind{};
    float seconds{};
    VfxRange outputs{};
    bool identity_unique{};
};
struct VfxEntitySurfaceRecipe
{
    std::uint32_t effect_handle{}, source_index{}, output_index{}, min_quality{}, gradient_row{};
    VfxEntitySurfaceShape shape{};
    VfxEntitySurfaceDirection direction{};
    float knot_start{}, knot_end{}, noise_scale{}, edge_width{};
    Float4 color{};
    float hdr{}, alpha{};
    bool persistent{}, death{};
};
struct VfxEntitySurfaceRouteHandles
{
    std::uint32_t phase_effect{}, death_effect{}, trap_point_rate_effect{};
};
enum class VfxRibbonTextureAsset : std::uint8_t
{
    Other, Gradient, Curve, FlowCurl, OptionalDetail, DetailArray, Stbn, NoiseVolume
};
struct VfxRibbonEffectRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t handle{}, importance{}, payload_kind{}, input_mode{}, timing_kind{}, lookup_count{}, identity_count{};
    float seconds{};
    VfxRange sources{};
    bool valid_handle{};
};
struct VfxRibbonSourceRecipe
{
    std::uint32_t effect{}, source_index{}, stable_id{}, parameter_count{};
    VfxSourceType type{};
    std::uint32_t knot_count{};
    std::array<float,4> knots{};
    VfxRange outputs{};
    std::uint32_t authoritative{};
    float history_seconds{};
    std::uint32_t ground_lock_bits{};
      bool history_present{}, history_valid{}, ground_lock_present{}, ground_lock_valid{}, knots_valid{};
};
struct VfxRibbonOutputRecipe
{
    std::uint32_t source{}, shape{}, shape_domain{}, shape_scale_rule{}, shape_component_kind{};
    VfxOutputProfile profile{};
    VfxMotionKind motion{};
    std::uint32_t coverage_type{}, coverage_ref{}, min_quality{}, surface_target{}, gradient_row{}, parameter_count{};
    VfxRange textures{};
    Float4 rgba{};
    float hdr{}, motion_rate_hz{}, motion_amplitude{}, motion_inset_fraction{};
    std::uint32_t first_parameter_key{}, second_parameter_key{};
    float arc_degrees{}, width_head{}, width_tail{}, noise_warp{}, phase_offset{}, width{}, taper{}, width_multiplier{}, reverse_flow{};
    std::uint32_t segments{};
    bool arc_present{}, arc_valid{}, width_head_present{}, width_head_valid{}, width_tail_present{}, width_tail_valid{};
    bool noise_warp_present{}, noise_warp_valid{}, phase_offset_present{}, phase_offset_valid{};
    bool width_present{}, width_valid{}, taper_present{}, taper_valid{}, width_multiplier_present{}, width_multiplier_valid{};
    bool reverse_flow_present{}, reverse_flow_valid{}, segments_present{}, segments_valid{};
    bool travel_pulse_present{}, travel_pulse_valid{};
      bool color_contract_valid{};
};
struct VfxRibbonTextureRecipe
{
    std::uint32_t role{}, selection{}, min_quality{};
    VfxRibbonTextureAsset asset{};
    float strength{};
    VfxRange slices{};
    std::uint32_t first_frame{}, frame_count{};
    float fps{};
    bool valid_path{};
};
// Accepted paired ground wake output. Authored route, source envelope,
// parameter and detail texture contracts are resolved at image admission.
struct VfxRibbonWakeRecipe
{
    std::uint32_t effect_handle{}, importance{}, min_quality{}, detail_min_quality{}, gradient_row{};
    VfxRange detail_slices{};
    Float4 color{};
    float width_head{}, width_tail{}, hdr{}, detail_strength{};
};
enum class VfxRibbonFixedKind : std::uint8_t { Unsupported, CommonPull, RetreatMove, ExplosivePrePull };
struct VfxRibbonFixedRecipe
{
    std::uint32_t effect_handle{}, importance{}, core_min_quality{}, outer_min_quality{}, segments{};
    VfxRibbonFixedKind kind{};
    float seconds{}, taper{}, outer_width_multiplier{};
    VfxRibbonOutputInput core_output{}, outer_output{};
};
struct VfxRibbonProjectileSourceRecipe
{
    std::uint32_t effect_handle{}, source_id{}, knot_count{};
    float history_seconds{};
    std::array<float,4> knots{};
    VfxRange outputs{};
};
struct VfxRibbonProjectileOutputRecipe
{
    std::uint32_t min_quality{};
    VfxRibbonOutputInput output{};
    VfxRange detail_slices{};
    float detail_strength{};
    bool detail_present{};
};
struct VfxRibbonCancelRecipe
{
    std::uint32_t effect_handle{}, importance{}, source_id{}, min_quality{};
    float seconds{}, history_seconds{}, width_head{}, width_tail{};
    Float4 color{};
    VfxOutputProfile profile{};
    VfxMotionKind motion{};
    float hdr{};
    std::uint32_t gradient_row{};
};
enum class VfxRibbonLinkKind : std::uint8_t
{
    Unsupported, SourceTarget, CapturedPath, CapturedLine, CapturedContext,
    PickupMotion, PickupCollect, ArrowRain
};
struct VfxRibbonLinkDetailRecipe
{
    VfxRange slices{};
    float strength{};
    bool valid{true}, present{};
};
struct VfxRibbonLinkOutputRecipe
{
    std::uint32_t source_id{}, min_quality{};
    bool core{};
    float width_multiplier{1.0f};
    VfxRibbonOutputInput output{};
    std::array<VfxRibbonLinkDetailRecipe,3> details{};
};
struct VfxRibbonLinkEffectRecipe
{
    std::uint32_t effect_handle{}, importance{};
    VfxRibbonLinkKind kind{};
    float seconds{}, window_start{}, window_end{}, taper{};
    std::uint32_t segments{};
    VfxRange outputs{};
};
struct VfxRibbonEventSourceRecipe
{
    std::uint32_t effect_handle{}, source_id{}, knot_count{};
    bool axial_source{};
    std::array<float,4> knots{};
    float history_seconds{};
    std::array<float,3> recoil_arc_by_quality{35.0f,35.0f,35.0f};
    std::array<float,3> axial_arc_by_quality{35.0f,35.0f,35.0f};
    VfxRange outputs{};
};
struct VfxRibbonEventEffectRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, importance{}, payload_kind{};
    float seconds{};
    VfxRange sources{};
};
struct VfxRibbonEventOutputRecipe
{
    std::uint32_t min_quality{};
    bool axial{}, arc_present{};
    float arc_degrees{};
    VfxRibbonOutputInput output{};
    VfxRange detail_slices{};
    float detail_strength{};
    bool detail_present{};
};
struct VfxFresnelEffectRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{};
    VfxFresnelShellKind kind{};
    VfxRange outputs{};
    bool route_valid{};
};
struct VfxFresnelRecipe
{
    std::uint32_t effect_handle{}, source_index{}, source_stable_id{}, output_index{}, min_quality{};
    VfxFresnelShellKind kind{};
    float knot_start{}, knot_end{};
    std::uint32_t sector_count{}, end_crack{};
    float fresnel_power{}, rotation_hz{}, noise_amount{};
    Float4 color{};
    float hdr{};
    std::uint32_t gradient_row{};
    float motion_rate_hz{}, motion_amplitude{}, motion_inset_fraction{};
};
struct VfxMeshEffectRecipe
{
    std::uint32_t effect_handle{};
    VfxRange outputs{};
    bool route_valid{};
};
struct VfxMeshRecipe
{
    std::uint32_t effect_handle{}, source_index{}, output_index{}, source_stable_id{};
    std::uint32_t mesh_index{}, min_quality{}, gradient_row{};
    Float4 color{};
    float hdr{}, fresnel{}, head_to_tail_ratio{}, shell_thickness{};
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
    std::span<const VfxScreenEffectRecipe> screen_effect_recipes;
    std::span<const VfxScreenRecipe> screen_recipes;
    std::span<const VfxVolumeEffectRecipe> volume_effect_recipes;
    std::span<const VfxVolumeRecipe> volume_recipes;
    std::span<const VfxEntitySurfaceEffectRecipe> entity_surface_effect_recipes;
    std::span<const VfxEntitySurfaceRecipe> entity_surface_recipes;
    VfxEntitySurfaceRouteHandles entity_surface_routes{};
    std::span<const VfxRibbonEffectRecipe> ribbon_effect_recipes;
    std::span<const std::uint32_t> ribbon_slice_indices;
    std::span<const VfxRange> ribbon_wake_effect_ranges;
    std::span<const VfxRibbonWakeRecipe> ribbon_wake_recipes;
    std::span<const VfxRange> ribbon_fixed_effect_ranges;
    std::span<const VfxRibbonFixedRecipe> ribbon_fixed_recipes;
    std::span<const VfxRange> ribbon_projectile_effect_ranges;
    std::span<const VfxRibbonProjectileSourceRecipe> ribbon_projectile_sources;
    std::span<const VfxRibbonProjectileOutputRecipe> ribbon_projectile_outputs;
    std::span<const VfxRange> ribbon_cancel_effect_ranges;
    std::span<const VfxRibbonCancelRecipe> ribbon_cancel_recipes;
    std::span<const VfxRibbonLinkEffectRecipe> ribbon_link_effect_recipes;
    std::span<const VfxRibbonLinkOutputRecipe> ribbon_link_output_recipes;
    std::span<const VfxRibbonEventEffectRecipe> ribbon_event_effect_recipes;
    std::span<const VfxRibbonEventSourceRecipe> ribbon_event_sources;
    std::span<const VfxRibbonEventOutputRecipe> ribbon_event_outputs;
    std::span<const VfxEffectLookupRecord> effect_lookup_by_id;
    std::span<const VfxFresnelEffectRecipe> fresnel_effect_recipes;
    std::span<const VfxFresnelRecipe> fresnel_recipes;
    std::span<const VfxMeshEffectRecipe> mesh_effect_recipes;
    std::span<const VfxMeshRecipe> mesh_recipes;

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
