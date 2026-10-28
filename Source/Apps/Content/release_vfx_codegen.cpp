#include "release_vfx_codegen.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace hs::content
{
namespace
{
std::string U(std::uint32_t value) { return std::to_string(value) + "u"; }
std::string U64(std::uint64_t value) { return std::to_string(value) + "ull"; }
std::string F(float value)
{
    return "std::bit_cast<float>(std::uint32_t{" + U(std::bit_cast<std::uint32_t>(value)) + "})";
}
template <typename T>
std::string Enum(const char *type, T value)
{
    return std::string(type) + "(" + U(static_cast<std::uint32_t>(value)) + ")";
}
std::string Range(VfxRange value) { return "{" + U(value.first) + "," + U(value.count) + "}"; }
std::string RangeArray(const std::array<VfxRange, 3> &value)
{
    return "{{" + Range(value[0]) + "," + Range(value[1]) + "," + Range(value[2]) + "}}";
}
std::string Vector(Float3 value) { return "{" + F(value.x) + "," + F(value.y) + "," + F(value.z) + "}"; }
std::string Vector(Float4 value)
{
    return "{" + F(value.x) + "," + F(value.y) + "," + F(value.z) + "," + F(value.w) + "}";
}
template <std::size_t N>
std::string Vector(const std::array<float, N> &value)
{
    std::string result="{";
    for(std::size_t i=0;i<N;++i){if(i)result+=",";result+=F(value[i]);}
    return result+"}";
}
std::string GroundCodegenValue(std::uint64_t value) { return U64(value); }
std::string GroundCodegenValue(std::uint32_t value) { return U(value); }
std::string GroundCodegenValue(float value) { return F(value); }
std::string GroundCodegenValue(bool value) { return value ? "true" : "false"; }
std::string GroundCodegenValue(VfxSourceType value) { return Enum("VfxSourceType", value); }
std::string GroundCodegenValue(VfxOutputProfile value) { return Enum("VfxOutputProfile", value); }
std::string GroundCodegenValue(VfxMotionKind value) { return Enum("VfxMotionKind", value); }
std::string GroundCodegenValue(VfxGroundRecipeKind value) { return Enum("VfxGroundRecipeKind", value); }
std::string GroundCodegenValue(VfxRange value) { return Range(value); }
std::string GroundCodegenValue(Float3 value) { return Vector(value); }
template <std::size_t N>
std::string GroundCodegenValue(const std::array<float, N> &value) { return Vector(value); }
std::string GroundMaskCodegenRow(const VfxGroundRecipe::MaskVariant &mask)
{
    std::string row = "{";
    bool first = true;
    VisitVfxGroundMaskVariantFields(mask, [&](const char *, const auto &value) {
        if (!first) row += ",";
        first = false;
        row += GroundCodegenValue(value);
    });
    return row + "}";
}
std::string GroundCodegenValue(const std::array<VfxGroundRecipe::MaskVariant, 3> &masks)
{
    std::string rows = "{{";
    for (std::size_t i = 0; i < masks.size(); ++i) {
        if (i) rows += ",";
        rows += GroundMaskCodegenRow(masks[i]);
    }
    return rows + "}}";
}
template <typename T> std::string GroundCodegenValue(const T &)
{
    static_assert(!std::is_same_v<T, T>, "unsupported VfxGroundRecipe codegen field type");
}
std::string Row(const CookedVfxDefinition &r)
{
    return "{" + U64(r.effect_id) + "," + U(r.first_emitter) + "," + U(r.emitter_count) + "," +
        Enum("VfxDefinitionKind", r.kind) + "," + U(r.reserved) + "," + Vector(r.line_color) + "," +
        F(r.line_width) + "," + F(r.line_lifetime) + "," + U(r.line_sprite) + "," +
        U(r.line_frame_columns) + "," + U(r.line_frame_rows) + "," + F(r.line_uv_repeat) + "," +
        F(r.line_scroll_speed) + "," + Enum("VfxPrimitive", r.line_primitive) + "}";
}
std::string Row(const CookedParticleEmitter &r)
{
    return "{" + U(r.sprite) + "," + U(r.frame_columns) + "," + U(r.frame_rows) + "," +
        Enum("ParticleShape", r.shape) + "," + Enum("ParticleVelocity", r.velocity) + "," +
        Enum("ParticleFacing", r.facing) + "," + Enum("VfxRenderer", r.renderer) + "," +
        Enum("VfxPrimitive", r.primitive) + "," + Vector(r.local_offset) + "," + Vector(r.shape_extent) + "," +
        Vector(r.local_direction) + "," + Vector(r.start_color) + "," + Vector(r.end_color) + "," +
        F(r.lifetime_min) + "," + F(r.lifetime_max) + "," + F(r.speed_min) + "," + F(r.speed_max) + "," +
        F(r.cone_degrees) + "," + F(r.start_size_min) + "," + F(r.start_size_max) + "," +
        F(r.end_size_min) + "," + F(r.end_size_max) + "," + F(r.gravity) + "," + F(r.rotation_min) + "," +
        F(r.rotation_max) + "," + F(r.angular_velocity_min) + "," + F(r.angular_velocity_max) + "," +
        F(r.stretch) + "," + U(r.count) + "}";
}
std::string Row(const CookedParticleSprite &r)
{
    return "{" + U64(r.sprite_id) + "," + U(r.index) + "," + U(r.frame_columns) + "," + U(r.frame_rows) + "}";
}
std::string Row(const VfxEffectRecord &r)
{
    return "{" + U(r.handle) + "," + U(r.input_mode) + "," + U(r.importance) + "," + U(r.role) + "," +
        U(r.style) + "," + U(r.readability) + "," + U(r.payload_kind) + "," + U(r.anchor) + "," +
        U(r.orientation) + "," + U(r.scale) + "," + U(r.binding_timing) + "," + U(r.geometry) + "," +
        U(r.must_match_logic) + "," + U(r.timing_kind) + "," + F(r.seconds) + "," + Range(r.sources) + "}";
}
std::string Row(const VfxSourceRecord &r)
{
    return "{" + U(r.effect) + "," + U(r.stable_id) + "," + Enum("VfxSourceType", r.type) + "," +
        U(r.knot_count) + "," + Vector(r.knots) + "," + Range(r.parameters) + "," + Range(r.outputs) + "," +
        U(r.authoritative) + "}";
}
std::string Row(const VfxOutputRecord &r)
{
    return "{" + U(r.source) + "," + Enum("VfxOutputProfile", r.profile) + "," + U(r.shape) + "," +
        U(r.shape_domain) + "," + U(r.shape_scale_rule) + "," + U(r.shape_component_kind) + "," +
        Vector(r.rgba) + "," + F(r.hdr) + "," + U(r.gradient_row) + "," + U(r.min_quality) + "," +
        U(r.coverage_type) + "," + U(r.coverage_ref) + "," + U(r.surface_target) + "," +
        Range(r.parameters) + "," + Range(r.textures) + "," + Enum("VfxMotionKind", r.motion) + "," +
        F(r.motion_rate_hz) + "," + F(r.motion_amplitude) + "," + F(r.motion_inset_fraction) + "}";
}
std::string Row(const VfxParameterRecord &r)
{
    return "{" + U(r.key) + "," + Enum("VfxParameterType", r.type) + "," + U(r.bits) + "}";
}
std::string Row(const VfxTextureBindingRecord &r)
{
    return "{" + U(r.role) + "," + U(r.catalog_slot) + "," + U(r.selection) + "," + U(r.min_quality) + "," +
        F(r.strength) + "," + Range(r.slices) + "," + U(r.first_frame) + "," + U(r.frame_count) + "," + F(r.fps) + "}";
}
std::string Row(const VfxCurveRecord &r)
{
    return "{" + U(r.interpolation) + "," + U(r.domain) + "," + Range(r.keys) + "}";
}
std::string Row(const VfxCurveKey &r) { return "{" + F(r.time) + "," + F(r.value) + "}"; }
std::string Row(const VfxEffectLookupRecord &r) { return "{" + U64(r.effect_id) + "," + U(r.handle) + "}"; }
std::string Row(const VfxTextureResourceRecord &r) { return "{" + U(r.slot) + "," + Range(r.asset_path_bytes) + "}"; }
std::string Row(const VfxUpgradeBindingRecord &r)
{
    return "{" + U64(r.skill_id) + "," + U(r.ordinal) + "," + Range(r.sequence) + "}";
}
std::string Row(const ParticleSpriteBinding &r)
{
    return "{" + U(r.sprite) + "," + U(r.frame_columns) + "," + U(r.frame_rows) + "}";
}
std::string Row(VfxRange r) { return Range(r); }
std::string Row(const VfxLightRecipe &r)
{
    return "{" + U64(r.seed_xor) + "," + U(r.payload_kind) + "," + U(r.importance) + "," + U(r.min_quality) + "," + U(r.knot_count) + "," +
        F(r.seconds) + "," + F(r.radius) + "," + Vector(r.knots) + "," + Vector(r.rgb) + "," + F(r.hdr) + "," + F(r.alpha) + "," +
        Range(r.curve_keys) + "," + U(r.interpolation) + "," + (r.hero ? "true" : "false") + "}";
}
std::string Row(const VfxBallisticRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + U(r.effect_handle) + "," + U(r.source_index) + "," + U(r.source_stable_id) + "," +
        U(r.payload_kind) + "," + U(r.importance) + "," + U(r.timing_kind) + "," + U(r.shape) + "," + U(r.min_quality) + "," +
        U(r.gradient_row) + "," + U(r.mesh_index) + "," + U(r.count) + "," + U(r.knot_count) + "," +
        Enum("VfxSourceType",r.source_type) + "," + Enum("VfxMotionKind",r.motion) + "," + Vector(r.knots) + "," +
        F(r.seconds) + "," + F(r.min_size) + "," + F(r.max_size) + "," + F(r.min_speed) + "," + F(r.max_speed) + "," +
        F(r.gravity) + "," + F(r.bounce) + "," + F(r.drag) + "," + F(r.speed) + "," + F(r.cone) + "," + F(r.min_spin) + "," +
        F(r.max_spin) + "," + Vector(r.color) + "," + F(r.hdr) + "," + (r.persistent_status ? "true" : "false") + "}";
}
std::string Row(const VfxDistortionRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + U(r.effect_handle) + "," + U(r.source_stable_id) + "," + U(r.payload_kind) + "," + U(r.importance) + "," +
        U(r.input_mode) + "," + U(r.timing_kind) + "," + U(r.min_quality) + "," + U(r.knot_count) + "," + Enum("VfxDistortionShape",r.kind) + "," +
        Enum("VfxMotionKind",r.motion) + "," + Vector(r.knots) + "," + F(r.seconds) + "," + F(r.radius) + "," + F(r.strength) + "," + F(r.alpha) + "}";
}
std::string Row(const VfxDecalRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + U(r.effect_handle) + "," + U(r.source_stable_id) + "," + U(r.payload_kind) + "," + U(r.importance) + "," +
        U(r.input_mode) + "," + U(r.timing_kind) + "," + U(r.min_quality) + "," + U(r.knot_count) + "," + Enum("VfxDecalKind",r.kind) + "," +
        Vector(r.knots) + "," + F(r.seconds) + "," + F(r.hdr) + "," + F(r.alpha) + "," + F(r.edge_glow) + "," + Vector(r.linear_rgb) + "," +
        U(r.gradient_row) + "," + U(r.voronoi_cells) + "," + U(r.texture_slice_base) + "," + U(r.texture_min_quality) + "}";
}
std::string Row(const VfxGroundRecipe &r)
{
    std::string row = "{";
    bool first = true;
    VisitVfxGroundRecipeFields(r, [&](const char *, const auto &value) {
        if (!first) row += ",";
        first = false;
        row += GroundCodegenValue(value);
    });
    return row + "}";
}
std::string Row(const VfxFlashSpawnInput &r)
{
    return "{" + Vector(r.position) + "," + Vector(r.color) + "," + F(r.size) + "," + F(r.delay) + "," + F(r.lifetime) + "," +
        U64(r.event_tick) + "," + U(r.stable_seed) + "," + U(r.gradient_row) + "," + F(r.hdr) + "," + F(r.normalized_age) + "," +
        Enum("VfxImpactShape",r.shape) + "," + Vector(r.direction) + "," + F(r.aspect) + "," + U(r.curve_row) + "," + U(r.mask_slice) + "," +
        F(r.mask_strength) + "," + Vector(r.initial_offset) + "," + Vector(r.initial_velocity) + "," + F(r.orbit_radius) + "," + F(r.orbit_phase) + "," +
        F(r.orbit_rate) + "," + F(r.drag) + "," + (r.oit?"true":"false") + "," + (r.ground_base_anchor?"true":"false") + "," +
        U(r.fbm_octaves) + "," + U(r.smoke_first_frame) + "," + U(r.smoke_frame_count) + "," + F(r.domain_warp) + "," + F(r.smoke_fps) + "," +
        F(r.opacity_scale) + "," + F(r.motion_strength) + "," + F(r.procedural_flow_speed) + "," + Vector(r.previous_position) + "," +
        (r.interpolate_owner_position?"true":"false") + "}";
}
std::string Row(const VfxFlashRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + U(r.effect_handle) + "," + U(r.source_index) + "," + U(r.source_stable_id) + "," + U(r.output_index) + "," +
        U(r.payload_kind) + "," + U(r.min_quality) + "," + Vector(r.knots) + "," + F(r.seconds) + "," + U(r.coverage_quality) + "," + Row(r.spawn) + "," +
        RangeArray(r.mask_slice_ranges) + "," +
        Vector(r.mask_strengths) + "," + U(r.supported_quality_mask) + "," + U(r.mask_present_mask) + "," +
        (r.unique_effect_identity?"true":"false") + "}";
}
std::string Row(const VfxPersistentStampRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + U(r.effect_handle) + "," + U(r.source_visual_kind) + "," +
        U(r.payload_kind) + "," + U(r.source_stable_id) + "," + U(r.timing_kind) + "," +
        F(r.fixed_seconds) + "," + F(r.source_end) + "," + Row(r.spawn) + "," + RangeArray(r.mask_slices) + "," +
        Vector(r.mask_strengths) + "," + U(r.supported_quality_mask) + "," + U(r.mask_present_mask) + "," +
        (r.bleed_extend ? "true" : "false") + "}";
}
std::string Row(const VfxPersistentFireRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + U(r.effect_handle) + "," + U(r.source_stable_id) + "," +
        U(r.rate) + "," + F(r.radius_scale) + "," + Row(r.spawn) + "," + Range(r.mask_slices) + "," +
        F(r.mask_strength) + "," + (r.authoritative ? "true" : "false") + "}";
}
std::string Row(const VfxOwnedStatusTickRecipe &r)
{
    return "{" + U64(r.tick_effect_id) + "," + U64(r.status_effect_id) + "," +
        U(r.tick_effect_handle) + "," + U(r.status_effect_handle) + "," + U(r.min_quality) + "," +
        F(r.tick_seconds) + "," + F(r.delay_fraction) + "," + F(r.lifetime_fraction) + "," +
        Row(r.spawn) + ",{ " + U(r.mask_slices[0]) + "," +
        U(r.mask_slices[1]) + "," + U(r.mask_slices[2]) + "}," + F(r.mask_strength) + "," +
        (r.bleed ? "true" : "false") + "}";
}
std::string Row(const VfxMoteSourceRecipe &r)
{
    return "{" + U(r.count) + "," + std::to_string(r.rate) + "," +
        F(r.curl) + "," + F(r.drag) + "," + F(r.speed) + "," + F(r.cone_degrees) + "," +
        (r.common_params_valid ? "true" : "false") + "," + (r.rate_valid ? "true" : "false") + "," +
        (r.residual_valid ? "true" : "false") + "," + (r.spark_valid ? "true" : "false") + "," + U64(r.effect_id) + "," +
        U(r.effect_handle) + "," + U(r.effect_lookup_count) + "," + U(r.stable_id) + "," + Range(r.output_range) + "," +
        Vector(r.knots) + "," + (r.output_range_valid ? "true" : "false") + "," + (r.common_route_valid ? "true" : "false") + "," +
        (r.residual_route_valid ? "true" : "false") + "," + (r.rate_route_valid ? "true" : "false") + "," +
        (r.tick_route_valid ? "true" : "false") + "," + (r.spark_route_valid ? "true" : "false") + "}";
}
std::string Row(const VfxMoteOutputRecipe &r)
{
    return "{" + U(r.min_quality) + "," + U(r.gradient_row) + "," + RangeArray(r.mask_slices) + "," +
        Vector(r.mask_strengths) + "," + U(r.persistent_curve_row) + "," +
        F(r.persistent_size) + "," + F(r.hdr) + "," + Vector(r.color) + "," + U(r.mask_present_mask) + "," + U(r.mask_quality_mask) + "," +
        (r.output_color_valid ? "true" : "false") + "," +
        (r.persistent_params_valid ? "true" : "false") + "," + (r.spark_textures_valid ? "true" : "false") + "," + U(r.common_recipe_index) + "," +
        (r.empty_parameters_valid ? "true" : "false") + "," + (r.residual_route_valid ? "true" : "false") + "," +
        (r.rate_route_valid ? "true" : "false") + "," + (r.tick_route_valid ? "true" : "false") + "," +
        (r.persistent_route_valid ? "true" : "false") + "," + (r.spark_route_valid ? "true" : "false") + "}";
}
std::string Row(const VfxScreenEffectRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + F(r.seconds) + "," + Range(r.outputs) + "," +
        (r.route_valid ? "true" : "false") + "}";
}
std::string Row(const VfxScreenRecipe &r)
{
    return "{" + U(r.source_index) + "," + U(r.output_index) + "," + U(r.min_quality) + "," +
        U(r.gradient_row) + "," + Enum("VfxScreenShape", r.shape) + "," + F(r.knot_start) + "," +
        F(r.knot_end) + "," + Vector(r.rgb) + "," + F(r.alpha) + "," + F(r.hdr) + "," +
        F(r.center_clear) + "," + F(r.edge_power) + "}";
}
std::string Row(const VfxVolumeEffectRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + F(r.seconds) + "," + Range(r.outputs) + "," +
        (r.route_valid ? "true" : "false") + "}";
}
std::string Row(const VfxVolumeRecipe &r)
{
    return "{" + U(r.source_index) + "," + U(r.output_index) + "," + U(r.source_id) + "," +
        U(r.min_quality) + "," + U(r.gradient_row) + "," + Enum("VfxVolumeShape", r.shape) + "," +
        Enum("VfxVolumeNoise", r.noise) + "," + F(r.knot_start) + "," + F(r.knot_end) + "," +
        F(r.density) + "," + F(r.alpha) + "," + F(r.anisotropy) + "," + F(r.hdr) + "," +
        U(r.steps) + "," + U(r.half_resolution) + "," + Vector(r.rgb) + "}";
}
std::string Row(const VfxEntitySurfaceEffectRecipe &r)
{
    return "{" + U64(r.effect_id) + "," + U(r.importance) + "," + U(r.payload_kind) + "," +
        U(r.input_mode) + "," + U(r.timing_kind) + "," + F(r.seconds) + "," + Range(r.outputs) + "," +
        (r.identity_unique ? "true" : "false") + "}";
}
std::string Row(const VfxEntitySurfaceRecipe &r)
{
    return "{" + U(r.effect_handle) + "," + U(r.source_index) + "," + U(r.output_index) + "," +
        U(r.min_quality) + "," + U(r.gradient_row) + "," + Enum("VfxEntitySurfaceShape", r.shape) + "," +
        Enum("VfxEntitySurfaceDirection", r.direction) + "," + F(r.knot_start) + "," + F(r.knot_end) + "," +
        F(r.noise_scale) + "," + F(r.edge_width) + "," + Vector(r.color) + "," + F(r.hdr) + "," +
        F(r.alpha) + "," + (r.persistent ? "true" : "false") + "," + (r.death ? "true" : "false") + "}";
}
std::string Row(const VfxEntitySurfaceRouteHandles &r)
{ return "{"+U(r.phase_effect)+","+U(r.death_effect)+","+U(r.trap_point_rate_effect)+"}"; }
std::string Row(const VfxRibbonEffectRecipe &r)
{
    return "{"+U64(r.effect_id)+","+U(r.handle)+","+U(r.importance)+","+U(r.payload_kind)+","+
        U(r.input_mode)+","+U(r.timing_kind)+","+U(r.lookup_count)+","+U(r.identity_count)+","+
        F(r.seconds)+","+Range(r.sources)+","+(r.valid_handle?"true":"false")+"}";
}
std::string Row(const VfxRibbonWakeRecipe &r)
{
    return "{"+U(r.effect_handle)+","+U(r.importance)+","+U(r.min_quality)+","+U(r.detail_min_quality)+","+
        U(r.gradient_row)+","+Range(r.detail_slices)+","+Vector(r.color)+","+
        F(r.width_head)+","+F(r.width_tail)+","+F(r.hdr)+","+F(r.detail_strength)+"}";
}
std::string RibbonOutput(const VfxRibbonOutputInput &r)
{
    return "{"+Enum("VfxOutputProfile",r.profile)+","+Enum("VfxMotionKind",r.motion)+","+
        Vector(r.color)+","+F(r.width_head)+","+F(r.width_tail)+","+F(r.noise_warp)+","+
        F(r.phase_offset)+","+F(r.hdr)+","+F(r.detail_strength)+","+U(r.gradient_row)+","+
        U(r.detail_slice)+","+(r.dashed?"true":"false")+","+(r.travel_pulse?"true":"false")+"}";
}
std::string Row(const VfxRibbonFixedRecipe &r)
{
    return "{"+U(r.effect_handle)+","+U(r.importance)+","+U(r.core_min_quality)+","+
        U(r.outer_min_quality)+","+U(r.segments)+","+Enum("VfxRibbonFixedKind",r.kind)+","+
        F(r.seconds)+","+F(r.taper)+","+F(r.outer_width_multiplier)+","+
        RibbonOutput(r.core_output)+","+RibbonOutput(r.outer_output)+"}";
}
std::string Row(const VfxRibbonProjectileSourceRecipe &r)
{
    return "{"+U(r.effect_handle)+","+U(r.source_id)+","+U(r.knot_count)+","+
        F(r.history_seconds)+","+Vector(r.knots)+","+Range(r.outputs)+"}";
}
std::string Row(const VfxRibbonProjectileOutputRecipe &r)
{
    return "{"+U(r.min_quality)+","+RibbonOutput(r.output)+","+Range(r.detail_slices)+","+
        F(r.detail_strength)+","+(r.detail_present?"true":"false")+"}";
}
std::string Row(const VfxRibbonCancelRecipe &r)
{
    return "{"+U(r.effect_handle)+","+U(r.importance)+","+U(r.source_id)+","+
        U(r.min_quality)+","+F(r.seconds)+","+F(r.history_seconds)+","+F(r.width_head)+","+
        F(r.width_tail)+","+Vector(r.color)+","+Enum("VfxOutputProfile",r.profile)+","+
        Enum("VfxMotionKind",r.motion)+","+F(r.hdr)+","+U(r.gradient_row)+"}";
}
std::string Row(const VfxRibbonLinkDetailRecipe &r)
{
    return "{"+Range(r.slices)+","+F(r.strength)+","+(r.valid?"true":"false")+","+
        (r.present?"true":"false")+"}";
}
std::string LinkDetails(const std::array<VfxRibbonLinkDetailRecipe,3> &rows)
{
    std::string value="std::array<VfxRibbonLinkDetailRecipe,3>{{";
    for(std::size_t i=0;i<rows.size();++i){if(i)value+=",";value+=Row(rows[i]);}
    return value+"}}";
}
std::string Row(const VfxRibbonLinkOutputRecipe &r)
{
    return "{"+U(r.source_id)+","+U(r.min_quality)+","+(r.core?"true":"false")+","+
        F(r.width_multiplier)+","+RibbonOutput(r.output)+","+LinkDetails(r.details)+"}";
}
std::string Row(const VfxRibbonLinkEffectRecipe &r)
{
    return "{"+U(r.effect_handle)+","+U(r.importance)+","+Enum("VfxRibbonLinkKind",r.kind)+","+
        F(r.seconds)+","+F(r.window_start)+","+F(r.window_end)+","+F(r.taper)+","+
        U(r.segments)+","+Range(r.outputs)+"}";
}
std::string Row(const VfxRibbonEventSourceRecipe &r)
{
    return "{"+U(r.effect_handle)+","+U(r.source_id)+","+U(r.knot_count)+","+
        (r.axial_source?"true":"false")+","+Vector(r.knots)+","+F(r.history_seconds)+","+
        Vector(r.recoil_arc_by_quality)+","+Vector(r.axial_arc_by_quality)+","+Range(r.outputs)+"}";
}
std::string Row(const VfxRibbonEventEffectRecipe &r)
{
    return "{"+U64(r.effect_id)+","+U(r.effect_handle)+","+U(r.importance)+","+
        U(r.payload_kind)+","+F(r.seconds)+","+Range(r.sources)+"}";
}
std::string Row(const VfxRibbonEventOutputRecipe &r)
{
    return "{"+U(r.min_quality)+","+(r.axial?"true":"false")+","+
        (r.arc_present?"true":"false")+","+F(r.arc_degrees)+","+RibbonOutput(r.output)+","+
        Range(r.detail_slices)+","+F(r.detail_strength)+","+(r.detail_present?"true":"false")+"}";
}
std::string Row(const VfxFresnelEffectRecipe &r)
{
    return "{"+U64(r.effect_id)+","+U(r.effect_handle)+","+Enum("VfxFresnelShellKind",r.kind)+","+
        Range(r.outputs)+","+(r.route_valid?"true":"false")+"}";
}
std::string Row(const VfxFresnelRecipe &r)
{
    return "{"+U(r.effect_handle)+","+U(r.source_index)+","+U(r.source_stable_id)+","+
        U(r.output_index)+","+U(r.min_quality)+","+Enum("VfxFresnelShellKind",r.kind)+","+
        F(r.knot_start)+","+F(r.knot_end)+","+U(r.sector_count)+","+U(r.end_crack)+","+
        F(r.fresnel_power)+","+F(r.rotation_hz)+","+F(r.noise_amount)+","+Vector(r.color)+","+
        F(r.hdr)+","+U(r.gradient_row)+","+F(r.motion_rate_hz)+","+F(r.motion_amplitude)+","+
        F(r.motion_inset_fraction)+"}";
}
std::string Row(const VfxMeshEffectRecipe &r)
{
    return "{"+U(r.effect_handle)+","+Range(r.outputs)+","+(r.route_valid?"true":"false")+"}";
}
std::string Row(const VfxMeshRecipe &r)
{
    return "{"+U(r.effect_handle)+","+U(r.source_index)+","+U(r.output_index)+","+
        U(r.source_stable_id)+","+U(r.mesh_index)+","+U(r.min_quality)+","+U(r.gradient_row)+","+
        Vector(r.color)+","+F(r.hdr)+","+F(r.fresnel)+","+F(r.head_to_tail_ratio)+","+
        F(r.shell_thickness)+"}";
}
std::string Row(std::uint32_t value) { return U(value); }
std::string Row(std::uint8_t value) { return U(value); }
std::string Row(std::byte value) { return "std::byte{" + U(std::to_integer<std::uint32_t>(value)) + "}"; }

template <typename T>
void Array(std::string &source, const char *type, const char *name, std::span<const T> rows)
{
    source += "constinit const std::array<" + std::string(type) + "," + std::to_string(rows.size()) + "> " + name + "{{\n";
    for (const auto &row : rows) source += "    " + Row(row) + ",\n";
    source += "}};\n";
}
}

std::string EmitReleaseVfxCpp(const VfxImage &image)
{
    std::string source =
        "#include <hs/runtime/vfx_image.hpp>\n"
        "#include <array>\n#include <bit>\n#include <cstdint>\n\n"
        "namespace hs::shipping\n{\nnamespace\n{\n";
    Array(source, "CookedVfxDefinition", "kVfxDefinitions", image.definitions);
    Array(source, "CookedParticleEmitter", "kVfxEmitters", image.emitters);
    Array(source, "CookedParticleSprite", "kVfxSprites", image.sprites);
    Array(source, "std::uint8_t", "kVfxAuthoredMeshes", image.authored_mesh_indices);
    Array(source, "std::uint8_t", "kVfxDecorativeDefinitions", image.decorative_definitions);
    Array(source, "VfxEffectRecord", "kVfxEffects", image.program.effects);
    Array(source, "VfxSourceRecord", "kVfxSources", image.program.sources);
    Array(source, "VfxOutputRecord", "kVfxOutputs", image.program.outputs);
    Array(source, "VfxParameterRecord", "kVfxParameters", image.program.parameters);
    Array(source, "VfxTextureBindingRecord", "kVfxTextureBindings", image.program.texture_bindings);
    Array(source, "std::uint32_t", "kVfxSliceIndices", image.program.slice_indices);
    Array(source, "VfxCurveRecord", "kVfxCurves", image.program.curves);
    Array(source, "VfxCurveKey", "kVfxCurveKeys", image.program.curve_keys);
    Array(source, "VfxEffectLookupRecord", "kVfxEffectLookup", image.program.effect_lookup);
    Array(source, "VfxTextureResourceRecord", "kVfxTextureResources", image.program.texture_resources);
    Array(source, "std::byte", "kVfxStrings", image.program.strings);
    Array(source, "VfxUpgradeBindingRecord", "kVfxUpgradeBindings", image.program.upgrade_bindings);
    Array(source, "std::uint32_t", "kVfxUpgradeSequences", image.program.upgrade_sequences);
    Array(source, "VfxRange", "kLightRanges", image.light_effect_ranges);
    Array(source, "VfxLightRecipe", "kLightRecipes", image.light_recipes);
    Array(source, "VfxRange", "kBallisticRanges", image.ballistic_effect_ranges);
    Array(source, "VfxBallisticRecipe", "kBallisticRecipes", image.ballistic_recipes);
    Array(source, "std::uint32_t", "kEffectOutputProfiles", image.effect_output_profiles);
    Array(source, "VfxRange", "kDistortionRanges", image.distortion_effect_ranges);
    Array(source, "VfxDistortionRecipe", "kDistortionRecipes", image.distortion_recipes);
    Array(source, "VfxRange", "kDecalRanges", image.decal_effect_ranges);
    Array(source, "VfxDecalRecipe", "kDecalRecipes", image.decal_recipes);
    Array(source, "VfxRange", "kGroundRanges", image.ground_effect_ranges);
    Array(source, "VfxGroundRecipe", "kGroundRecipes", image.ground_recipes);
    Array(source, "VfxRange", "kFlashRanges", image.flash_effect_ranges);
    Array(source, "VfxFlashRecipe", "kFlashRecipes", image.flash_recipes);
    Array(source, "VfxRange", "kPersistentStampRanges", image.persistent_stamp_effect_ranges);
    Array(source, "VfxPersistentStampRecipe", "kPersistentStampRecipes", image.persistent_stamp_recipes);
    Array(source, "VfxRange", "kPersistentFireRanges", image.persistent_fire_effect_ranges);
    Array(source, "VfxPersistentFireRecipe", "kPersistentFireRecipes", image.persistent_fire_recipes);
    Array(source, "VfxRange", "kOwnedStatusTickRanges", image.owned_status_tick_effect_ranges);
    Array(source, "VfxOwnedStatusTickRecipe", "kOwnedStatusTickRecipes", image.owned_status_tick_recipes);
    Array(source, "VfxMoteSourceRecipe", "kMoteSourceRecipes", image.mote_source_recipes);
    Array(source, "VfxMoteOutputRecipe", "kMoteOutputRecipes", image.mote_output_recipes);
    Array(source, "VfxScreenEffectRecipe", "kScreenEffectRecipes", image.screen_effect_recipes);
    Array(source, "VfxScreenRecipe", "kScreenRecipes", image.screen_recipes);
    Array(source, "VfxVolumeEffectRecipe", "kVolumeEffectRecipes", image.volume_effect_recipes);
    Array(source, "VfxVolumeRecipe", "kVolumeRecipes", image.volume_recipes);
    Array(source, "VfxEntitySurfaceEffectRecipe", "kEntitySurfaceEffectRecipes", image.entity_surface_effect_recipes);
    Array(source, "VfxEntitySurfaceRecipe", "kEntitySurfaceRecipes", image.entity_surface_recipes);
    Array(source, "VfxRibbonEffectRecipe", "kRibbonEffectRecipes", image.ribbon_effect_recipes);
    Array(source, "std::uint32_t", "kRibbonSliceIndices", image.ribbon_slice_indices);
    Array(source, "VfxRange", "kRibbonWakeEffectRanges", image.ribbon_wake_effect_ranges);
    Array(source, "VfxRibbonWakeRecipe", "kRibbonWakeRecipes", image.ribbon_wake_recipes);
    Array(source, "VfxRange", "kRibbonFixedEffectRanges", image.ribbon_fixed_effect_ranges);
    Array(source, "VfxRibbonFixedRecipe", "kRibbonFixedRecipes", image.ribbon_fixed_recipes);
    Array(source, "VfxRange", "kRibbonProjectileEffectRanges", image.ribbon_projectile_effect_ranges);
    Array(source, "VfxRibbonProjectileSourceRecipe", "kRibbonProjectileSources", image.ribbon_projectile_sources);
    Array(source, "VfxRibbonProjectileOutputRecipe", "kRibbonProjectileOutputs", image.ribbon_projectile_outputs);
    Array(source, "VfxRange", "kRibbonCancelEffectRanges", image.ribbon_cancel_effect_ranges);
    Array(source, "VfxRibbonCancelRecipe", "kRibbonCancelRecipes", image.ribbon_cancel_recipes);
    Array(source, "VfxRibbonLinkEffectRecipe", "kRibbonLinkEffectRecipes", image.ribbon_link_effect_recipes);
    Array(source, "VfxRibbonLinkOutputRecipe", "kRibbonLinkOutputRecipes", image.ribbon_link_output_recipes);
    Array(source, "VfxRibbonEventEffectRecipe", "kRibbonEventEffectRecipes", image.ribbon_event_effect_recipes);
    Array(source, "VfxRibbonEventSourceRecipe", "kRibbonEventSources", image.ribbon_event_sources);
    Array(source, "VfxRibbonEventOutputRecipe", "kRibbonEventOutputs", image.ribbon_event_outputs);
    Array(source, "VfxEffectLookupRecord", "kEffectLookupById", image.effect_lookup_by_id);
    Array(source, "VfxFresnelEffectRecipe", "kFresnelEffectRecipes", image.fresnel_effect_recipes);
    Array(source, "VfxFresnelRecipe", "kFresnelRecipes", image.fresnel_recipes);
    Array(source, "VfxMeshEffectRecipe", "kMeshEffectRecipes", image.mesh_effect_recipes);
    Array(source, "VfxMeshRecipe", "kMeshRecipes", image.mesh_recipes);
    const std::string entity_routes=Row(image.entity_surface_routes);
    source +=
        "constinit const VfxImage kVfx{\n"
        "    kVfxDefinitions,kVfxEmitters,kVfxSprites,kVfxAuthoredMeshes,kVfxDecorativeDefinitions,\n"
        "    VfxProgramView{kVfxEffects,kVfxSources,kVfxOutputs,kVfxParameters,kVfxTextureBindings,\n"
        "        kVfxSliceIndices,kVfxCurves,kVfxCurveKeys,kVfxEffectLookup,kVfxTextureResources,\n"
        "        kVfxStrings,kVfxUpgradeBindings,kVfxUpgradeSequences},\n    ";
    source += image.has_typed_wavefront ? "true," : "false,";
    source += U(image.sprite_count) + "," + U64(image.payload_hash) + ",{" +
        Row(image.sprite_bindings.bleed_status_sprite) + "," + Row(image.sprite_bindings.burn_status_sprite) + "," +
        Row(image.sprite_bindings.slow_status_sprite) + "," + Row(image.sprite_bindings.mark_status_sprite) + "," +
        Row(image.sprite_bindings.trap_sprite) + "," + Row(image.sprite_bindings.fire_area_sprite) + "},";
    source += image.has_required_sprite_bindings ? "true,\n" : "false,\n";
    source += "kLightRanges,kLightRecipes,kBallisticRanges,kBallisticRecipes,kEffectOutputProfiles,kDistortionRanges,kDistortionRecipes,kDecalRanges,kDecalRecipes,"
        "kGroundRanges,kGroundRecipes,kFlashRanges,kFlashRecipes,kPersistentStampRanges,kPersistentStampRecipes,"
        "kPersistentFireRanges,kPersistentFireRecipes,kOwnedStatusTickRanges,kOwnedStatusTickRecipes,"
        "kMoteSourceRecipes,kMoteOutputRecipes,kScreenEffectRecipes,kScreenRecipes,"
        "kVolumeEffectRecipes,kVolumeRecipes,kEntitySurfaceEffectRecipes,kEntitySurfaceRecipes,"+entity_routes+","+
        "kRibbonEffectRecipes,kRibbonSliceIndices,"
        "kRibbonWakeEffectRanges,kRibbonWakeRecipes,"
        "kRibbonFixedEffectRanges,kRibbonFixedRecipes,"
        "kRibbonProjectileEffectRanges,kRibbonProjectileSources,kRibbonProjectileOutputs,"
        "kRibbonCancelEffectRanges,kRibbonCancelRecipes,"
        "kRibbonLinkEffectRecipes,kRibbonLinkOutputRecipes,"
        "kRibbonEventEffectRecipes,kRibbonEventSources,kRibbonEventOutputs,"
        "kEffectLookupById,kFresnelEffectRecipes,kFresnelRecipes,kMeshEffectRecipes,kMeshRecipes};\n}\n"
        "const VfxImage &Vfx() noexcept { return kVfx; }\n}\n";
    return source;
}
}
