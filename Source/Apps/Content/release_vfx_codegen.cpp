#include "release_vfx_codegen.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

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
std::string Vector(Float3 value) { return "{" + F(value.x) + "," + F(value.y) + "," + F(value.z) + "}"; }
std::string Vector(Float4 value)
{
    return "{" + F(value.x) + "," + F(value.y) + "," + F(value.z) + "," + F(value.w) + "}";
}
std::string Vector(const std::array<float, 4> &value)
{
    return "{" + F(value[0]) + "," + F(value[1]) + "," + F(value[2]) + "," + F(value[3]) + "}";
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
    source += "kLightRanges,kLightRecipes};\n}\nconst VfxImage &Vfx() noexcept { return kVfx; }\n}\n";
    return source;
}
}
