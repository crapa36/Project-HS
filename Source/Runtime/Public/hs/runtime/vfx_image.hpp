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
#include <hs/render_data/vfx_owned_mesh.hpp>
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
    float intensity_scale{};
    VfxRange curve_keys{};
    std::uint32_t interpolation{};
    bool hero{};
};
// Prepared immutable ballistic mesh outputs. One row corresponds to one
// accepted source/output pair, in authored order; runtime builders only bind
// live event or persistent-owner data to these constants.
enum class VfxBallisticDirection : std::uint8_t { Up, EventNormal, ProjectileVelocity };
struct VfxBallisticRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t source_seed_salt{}, payload_kind{}, importance{}, min_quality{}, gradient_row{}, mesh_index{}, count{};
    VfxBallisticDirection direction{};
    bool persistent_status{}, cosmetic{};
    double birth_floor_ticks{}, birth_ceil_ticks{};
    float birth_fraction{}, event_lifetime{}, end_knot{}, seconds{};
    float min_size{}, max_size{}, min_speed{}, max_speed{};
    float gravity{}, bounce{}, drag{.8f}, speed{-1}, cone{}, min_spin{}, max_spin{};
    Float4 color{};
    float hdr{};
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
inline constexpr std::uint32_t kVfxGroundEventFixedDuration = 4u;
inline constexpr std::uint32_t kVfxGroundEventOwnerDuration = 8u;
inline constexpr std::uint32_t kVfxGroundEventChargedPulse = 16u;
inline constexpr std::uint32_t kVfxGroundEventWaveStart = 32u;
inline constexpr std::uint32_t kVfxGroundEventWaveWarning = 64u;
inline constexpr std::uint32_t kVfxGroundEntityPolar = 1u << 7;
inline constexpr std::uint32_t kVfxGroundEntityState = 1u << 8;
inline constexpr std::uint32_t kVfxGroundEntityCrown = 1u << 9;
inline constexpr std::uint32_t kVfxGroundEntityPickup = 1u << 10;
inline constexpr std::uint32_t kVfxGroundEntityRelic = 1u << 11;
inline constexpr std::uint32_t kVfxGroundEntityCharged = 1u << 12;
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
    // Aggregated once per effect; only the first row in its range carries these bits.
    std::uint32_t admission_profiles{};
};
enum class VfxFlashEventKind : std::uint8_t
{
    Unsupported,
    Point,
    Context,
    ProjectileContact,
    ConeMuzzle,
    ConeBossEnergy,
    RingBossEnergy,
    Circle,
    PickupCollect
};

struct VfxFlashEffectRecipe
{
    VfxRange outputs{};
    VfxFlashEventKind kind{VfxFlashEventKind::Unsupported};
    std::uint32_t importance{};
};

struct VfxFlashRecipe
{
    std::uint32_t source_index{}, source_stable_id{}, output_index{};
    VfxFlashTemplate spawn{};
    std::array<VfxRange,3> mask_slice_ranges{};
    std::array<float,3> mask_strengths{};
    std::uint8_t supported_quality_mask{}, mask_present_mask{};
    bool cosmetic{}, suppress_smoke_motion{};
};
struct VfxPersistentStampRecipe
{
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, source_visual_kind{}, payload_kind{}, source_stable_id{};
    std::uint32_t timing_kind{};
    float fixed_seconds{}, source_end{};
    VfxFlashTemplate spawn{};
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
    VfxFlashTemplate spawn{};
    VfxRange mask_slices{};
    float mask_strength{};
    bool authoritative{};
};
struct VfxOwnedStatusTickRecipe
{
    std::uint64_t tick_effect_id{}, status_effect_id{};
    std::uint32_t tick_effect_handle{}, status_effect_handle{}, min_quality{};
    VfxFlashTemplate spawn{};
    std::array<std::uint32_t,3> mask_slices{};
    float mask_strength{};
    bool bleed{};
};
// Mote source/output records keep immutable authored parameter and texture
// interpretation out of per-event and per-frame builders. Rows are indexed by
// their source/output table index, so ordinary lookups are bounds checks.
inline constexpr std::uint32_t kVfxMoteBurstPoint = 1u << 0;
inline constexpr std::uint32_t kVfxMoteBurstContext = 1u << 1;
inline constexpr std::uint32_t kVfxMoteResidualEntity = 1u << 2;
inline constexpr std::uint32_t kVfxMoteTrapPoint = 1u << 3;
inline constexpr std::uint32_t kVfxMoteSuicideCircle = 1u << 4;
inline constexpr std::uint32_t kVfxMoteTrapPending = 1u << 5;
inline constexpr std::uint32_t kVfxMoteTrapArmed = 1u << 6;
inline constexpr std::uint32_t kVfxMoteTrapRetreat = 1u << 7;
inline constexpr std::uint32_t kVfxMoteBleedTick = 1u << 8;
inline constexpr std::uint32_t kVfxMoteBurnTick = 1u << 9;
inline constexpr std::uint32_t kVfxMoteBleedOwner = 1u << 10;
inline constexpr std::uint32_t kVfxMoteBurnOwner = 1u << 11;
inline constexpr std::uint32_t kVfxMoteStatusPresence = 1u << 12;
inline constexpr std::uint32_t kVfxMotePickupPresence = 1u << 13;
inline constexpr std::uint32_t kVfxMoteChargedPresence = 1u << 14;
inline constexpr std::uint32_t kVfxMoteSuicidePresence = 1u << 15;
inline constexpr std::uint32_t kVfxMoteProjectileSpark = 1u << 16;
inline constexpr std::uint32_t kVfxMoteSafeSector = 1u << 17;
struct VfxMoteSourceRecipe
{
    std::uint32_t count{};
    std::int32_t rate{};
    float curl{.2f}, drag{.8f}, speed{}, cone_degrees{};
    bool common_params_valid{}, rate_valid{}, residual_valid{}, spark_valid{};
    std::uint64_t effect_id{};
    std::uint32_t effect_handle{}, effect_routes{}, stable_id{};
    VfxRange output_range{};
    std::array<float,2> knots{};
    bool output_range_valid{}, common_route_valid{}, residual_route_valid{}, rate_route_valid{}, tick_route_valid{}, spark_route_valid{};
    bool cosmetic{};
};
struct VfxMoteOutputRecipe
{
    std::uint32_t min_quality{};
    std::array<VfxRange,3> mask_slices{};
    std::array<float,3> mask_strengths{};
    std::uint32_t persistent_curve_row{0xffffffffu};
    float persistent_size{};
    VfxFlashAppearance appearance;
    std::uint8_t mask_present_mask{}, mask_quality_mask{0x7};
    bool output_color_valid{}, persistent_params_valid{}, spark_textures_valid{};
    std::uint32_t common_recipe_index{0xffffffffu};
    bool empty_parameters_valid{}, residual_route_valid{}, rate_route_valid{}, tick_route_valid{}, persistent_route_valid{}, spark_route_valid{};
};
enum class VfxScreenRoute : std::uint8_t { Unsupported=0, EventHit=1, PersistentLowHealth=2 };
struct VfxScreenEffectRecipe
{
    std::uint64_t effect_id{};
    float seconds{};
    VfxRange outputs{};
    VfxScreenRoute route{VfxScreenRoute::Unsupported};
};
struct VfxScreenRecipe
{
    std::uint32_t source_index{}, output_index{}, min_quality{}, gradient_row{};
    VfxScreenShape shape{};
    float knot_start{}, knot_end{};
    Float3 rgb{};
    float alpha{}, hdr{}, center_clear{0.62f}, edge_power{2.8f};
};
enum class VfxVolumeRoute : std::uint8_t { Unsupported=0, EventContext=1, EventPoint=2, PersistentEntity=3 };
struct VfxVolumeEffectRecipe
{
    std::uint64_t effect_id{};
    float seconds{};
    VfxRange outputs{};
    VfxVolumeRoute route{VfxVolumeRoute::Unsupported};
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
enum class VfxEntitySurfaceRoute : std::uint8_t { Unsupported=0, EventPhase=1, EventDeath=2, PersistentTransition=3 };
struct VfxEntitySurfaceEffectRecipe
{
    float seconds{};
    VfxRange outputs{};
    VfxEntitySurfaceRoute route{VfxEntitySurfaceRoute::Unsupported};
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
    std::uint32_t source_id{}, knot_count{};
    std::array<float,4> knots{};
    float history_seconds{}, start_seconds{}, end_seconds{};
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
struct VfxMeshEffectRecipe
{
    std::uint32_t effect_handle{};
    VfxRange outputs{};
    bool route_valid{};
};
struct VfxLegacyEffectPolicy
{
    std::uint32_t importance{};
    bool decorative{};
};

// Borrowed immutable rows. Generated storage lives for the process; developer
// storage must remain alive until all users of the adopted image have stopped.
struct VfxImage
{
    std::span<const CookedVfxDefinition> definitions;
    std::span<const CookedParticleEmitter> emitters;
    std::span<const CookedParticleSprite> sprites;
    std::span<const std::uint8_t> authored_mesh_indices;
    std::span<const VfxLegacyEffectPolicy> legacy_effect_policies;
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
    std::span<const VfxFlashEffectRecipe> flash_effect_recipes;
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
        if (!handle || handle > flash_effect_recipes.size()) return false;
        const auto range = flash_effect_recipes[handle - 1].outputs;
        return range.count && range.first <= flash_recipes.size() &&
            range.count <= flash_recipes.size() - range.first;
    }
};
}
