#include "release_vfx_codegen.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef HS_RELEASE_VFX_CODEGEN_TEST_DIR
#include <filesystem>
#include <fstream>
#endif

namespace
{
void Check(bool condition)
{
    if (!condition) throw std::runtime_error("release VFX codegen regression");
}

// Compiled separately with the emitted translation unit; expected values do not use emitter helpers.
constexpr std::string_view kRoundtripCheck = R"cpp(#include <hs/runtime/vfx_image.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>

void Check(bool condition) { if (!condition) throw std::runtime_error("compiled release VFX mismatch"); }
bool Same(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
bool Same(hs::Float3 a, hs::Float3 b) { return Same(a.x,b.x) && Same(a.y,b.y) && Same(a.z,b.z); }
bool Same(hs::Float4 a, hs::Float4 b) { return Same(a.x,b.x) && Same(a.y,b.y) && Same(a.z,b.z) && Same(a.w,b.w); }
bool Range(hs::VfxRange r, std::uint32_t first, std::uint32_t count) { return r.first == first && r.count == count; }
int main()
{
    using namespace hs;
    const auto &v = shipping::Vfx();
    Check(&v == &shipping::Vfx());
    Check(v.light_effect_ranges.size()==1 && Range(v.light_effect_ranges[0],0,1) && v.light_recipes.size()==1);
    const auto &lr=v.light_recipes[0];
    Check(lr.seed_xor==0xf123456789abcdefull && lr.payload_kind==11 && lr.importance==12 && lr.min_quality==13 && lr.knot_count==4 &&
        Same(lr.seconds,2.5f) && Same(lr.radius,6.5f) && Same(lr.knots[0],-0.0f) && Same(lr.knots[1],.25f) && Same(lr.knots[2],.75f) && Same(lr.knots[3],1) &&
        Same(lr.rgb,Float3{2,3,4}) && Same(lr.hdr,5) && Same(lr.alpha,.5f) && Range(lr.curve_keys,1,2) && lr.interpolation==1 && lr.hero);

    Check(v.definitions.size() == 2 && v.emitters.size() == 2 && v.sprites.size() == 2);
    const auto &d = v.definitions[0];
    Check(d.effect_id == 0xfedcba9876543210ull && d.first_emitter == 1 && d.emitter_count == 1 &&
        d.kind == VfxDefinitionKind::Line && d.reserved == 213 && Same(d.line_color, Float4{-0.0f,1,2,3}) &&
        Same(d.line_width,4) && Same(d.line_lifetime,5) && d.line_sprite == 42 &&
        d.line_frame_columns == 6 && d.line_frame_rows == 7 && Same(d.line_uv_repeat,8) &&
        Same(d.line_scroll_speed,9) && d.line_primitive == VfxPrimitive::DashWake);
    Check(v.definitions[1].effect_id == 1 && v.definitions[1].first_emitter == 0 &&
        v.definitions[1].emitter_count == 1 && v.definitions[1].kind == VfxDefinitionKind::Particles &&
        v.definitions[1].reserved == 19 && Same(v.definitions[1].line_color, Float4{11,12,13,14}) &&
        Same(v.definitions[1].line_width,15) && Same(v.definitions[1].line_lifetime,16) &&
        v.definitions[1].line_sprite == 17 && v.definitions[1].line_frame_columns == 18 &&
        v.definitions[1].line_frame_rows == 19 && Same(v.definitions[1].line_uv_repeat,20) &&
        Same(v.definitions[1].line_scroll_speed,21) && v.definitions[1].line_primitive == VfxPrimitive::ExactRing);
    const auto &e = v.emitters[0];
    Check(e.sprite == 43 && e.frame_columns == 8 && e.frame_rows == 9 && e.shape == ParticleShape::Ring &&
        e.velocity == ParticleVelocity::Inward && e.facing == ParticleFacing::Velocity &&
        e.renderer == VfxRenderer::Mesh && e.primitive == VfxPrimitive::LowFrequencyFill &&
        Same(e.local_offset,Float3{1,2,3}) && Same(e.shape_extent,Float3{4,5,6}) &&
        Same(e.local_direction,Float3{7,8,9}) && Same(e.start_color,Float4{10,11,12,13}) &&
        Same(e.end_color,Float4{14,15,16,-0.0f}) && e.count == 4000000000u);
    const std::array emitter_floats{e.lifetime_min,e.lifetime_max,e.speed_min,e.speed_max,e.cone_degrees,
        e.start_size_min,e.start_size_max,e.end_size_min,e.end_size_max,e.gravity,e.rotation_min,
        e.rotation_max,e.angular_velocity_min,e.angular_velocity_max,e.stretch};
    for (std::size_t i = 0; i < emitter_floats.size(); ++i) Check(Same(emitter_floats[i],float(20 + i)));
    Check(v.emitters[1].sprite == 44 && v.emitters[1].count == 655u);
    Check(v.sprites[0].sprite_id == 0xffffffffffffffffull && v.sprites[0].index == 43 &&
        v.sprites[0].frame_columns == 8 && v.sprites[0].frame_rows == 9 &&
        v.sprites[1].sprite_id == 1 && v.sprites[1].index == 44 &&
        v.sprites[1].frame_columns == 2 && v.sprites[1].frame_rows == 3);
    Check(v.authored_mesh_indices.size() == 2 && v.authored_mesh_indices[0] == 7 && v.authored_mesh_indices[1] == 0);
    Check(v.decorative_definitions.size() == 2 && v.decorative_definitions[0] == 1 && v.decorative_definitions[1] == 0);
    const auto &p = v.program;
    Check(p.effects.size() == 1 && p.sources.size() == 1 && p.outputs.size() == 1 && p.parameters.size() == 2 &&
        p.texture_bindings.size() == 1 && p.slice_indices.size() == 3 && p.curves.size() == 1 &&
        p.curve_keys.size() == 2 && p.effect_lookup.size() == 2 && p.texture_resources.size() == 1 &&
        p.strings.size() == 9 && p.upgrade_bindings.size() == 1 && p.upgrade_sequences.size() == 3);
    const auto &effect = p.effects[0];
    Check(effect.handle == 1 && effect.input_mode == 2 && effect.importance == 3 && effect.role == 4 &&
        effect.style == 5 && effect.readability == 6 && effect.payload_kind == 7 && effect.anchor == 8 &&
        effect.orientation == 9 && effect.scale == 10 && effect.binding_timing == 11 && effect.geometry == 12 &&
        effect.must_match_logic == 13 && effect.timing_kind == 14 && Same(effect.seconds,-0.0f) && Range(effect.sources,0,1));
    const auto &s = p.sources[0];
    Check(s.effect == 1 && s.stable_id == 2 && s.type == VfxSourceType::HistoryRibbon && s.knot_count == 4 &&
        Same(s.knots[0],1) && Same(s.knots[1],-0.0f) && std::bit_cast<std::uint32_t>(s.knots[2]) == 1u &&
        Same(s.knots[3],4) && Range(s.parameters,1,1) && Range(s.outputs,0,1) && s.authoritative == 1);
    const auto &o = p.outputs[0];
    Check(o.source == 0 && o.profile == VfxOutputProfile::SpriteSmoke6WayOit && o.shape == 3 && o.shape_domain == 4 &&
        o.shape_scale_rule == 5 && o.shape_component_kind == 6 && Same(o.rgba[0],7) && Same(o.rgba[1],8) &&
        Same(o.rgba[2],9) && Same(o.rgba[3],10) && Same(o.hdr,11) && o.gradient_row == 12 && o.min_quality == 13 &&
        o.coverage_type == 14 && o.coverage_ref == 15 && o.surface_target == 16 && Range(o.parameters,0,1) &&
        Range(o.textures,0,1) && o.motion == VfxMotionKind::VelocityAlignedSharpHead &&
        Same(o.motion_rate_hz,17) && Same(o.motion_amplitude,18) && Same(o.motion_inset_fraction,-0.0f));
    Check(p.parameters[0].key == 0xffffffffu && p.parameters[0].type == VfxParameterType::Int &&
        p.parameters[0].bits == 0x80000001u && p.parameters[1].key == 25 &&
        p.parameters[1].type == VfxParameterType::CurveRow && p.parameters[1].bits == 1);
    const auto &t = p.texture_bindings[0];
    Check(t.role == 1 && t.catalog_slot == 12 && t.selection == 3 && t.min_quality == 4 &&
        Same(t.strength,-0.0f) && Range(t.slices,1,2) && t.first_frame == 7 && t.frame_count == 8 && Same(t.fps,9));
    Check(p.slice_indices[0] == 4000000000u && p.slice_indices[1] == 1 && p.slice_indices[2] == 0xffffffffu);
    Check(p.curves[0].interpolation == 3 && p.curves[0].domain == 4 && Range(p.curves[0].keys,1,1));
    Check(Same(p.curve_keys[0].time,-0.0f) && std::bit_cast<std::uint32_t>(p.curve_keys[0].value) == 1u &&
        Same(p.curve_keys[1].time,0.5f) && Same(p.curve_keys[1].value,1));
    Check(p.effect_lookup[0].effect_id == 0xfedcba9876543210ull && p.effect_lookup[0].handle == 1 &&
        p.effect_lookup[1].effect_id == 1 && p.effect_lookup[1].handle == 1);
    Check(p.texture_resources[0].slot == 12 && Range(p.texture_resources[0].asset_path_bytes,1,7));
    constexpr std::array<unsigned,9> bytes{0,34,92,237,133,141,195,169,0};
    for (std::size_t i = 0; i < bytes.size(); ++i) Check(std::to_integer<unsigned>(p.strings[i]) == bytes[i]);
    Check(p.upgrade_bindings[0].skill_id == 0xf123456789abcdefull && p.upgrade_bindings[0].ordinal == 17 &&
        Range(p.upgrade_bindings[0].sequence,1,2) && p.upgrade_sequences[0] == 4 &&
        p.upgrade_sequences[1] == 37 && p.upgrade_sequences[2] == 38);
    const auto &b = v.sprite_bindings;
    const std::array bindings{b.bleed_status_sprite,b.burn_status_sprite,b.slow_status_sprite,
        b.mark_status_sprite,b.trap_sprite,b.fire_area_sprite};
    for (std::size_t i = 0; i < bindings.size(); ++i)
        Check(bindings[i].sprite == static_cast<ParticleSprite>(100 + i) &&
            bindings[i].frame_columns == static_cast<std::uint8_t>(10 + i) &&
            bindings[i].frame_rows == static_cast<std::uint8_t>(20 + i));
    Check(v.has_typed_wavefront && v.has_required_sprite_bindings && v.sprite_count == 65535 &&
        v.payload_hash == 0xfedcba0123456789ull);
}
)cpp";
}

void TestReleaseVfxCodegen()
{
    using namespace hs;
    const float negative_zero = std::bit_cast<float>(0x80000000u);
    const float subnormal = std::bit_cast<float>(1u);
    const std::array definitions{
        CookedVfxDefinition{0xfedcba9876543210ull,1,1,VfxDefinitionKind::Line,213,
            {negative_zero,1,2,3},4,5,42,6,7,8,9,VfxPrimitive::DashWake},
        CookedVfxDefinition{1,0,1,VfxDefinitionKind::Particles,19,
            {11,12,13,14},15,16,17,18,19,20,21,VfxPrimitive::ExactRing}};
    std::array emitters{
        CookedParticleEmitter{43,8,9,ParticleShape::Ring,ParticleVelocity::Inward,ParticleFacing::Velocity,
            VfxRenderer::Mesh,VfxPrimitive::LowFrequencyFill,{1,2,3},{4,5,6},{7,8,9},{10,11,12,13},
            {14,15,16,negative_zero},20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,4000000000u},
        CookedParticleEmitter{}};
    emitters[1] = emitters[0];
    emitters[1].sprite = 44;
    emitters[1].count = 655;
    const std::array sprites{CookedParticleSprite{0xffffffffffffffffull,43,8,9},CookedParticleSprite{1,44,2,3}};
    const std::array<std::uint8_t,2> meshes{7,0}, decorative{1,0};
    const std::array effects{VfxEffectRecord{1,2,3,4,5,6,7,8,9,10,11,12,13,14,negative_zero,{0,1}}};
    const std::array sources{VfxSourceRecord{1,2,VfxSourceType::HistoryRibbon,4,
        {1,negative_zero,subnormal,4},{1,1},{0,1},1}};
    const std::array outputs{VfxOutputRecord{0,VfxOutputProfile::SpriteSmoke6WayOit,3,4,5,6,
        {7,8,9,10},11,12,13,14,15,16,{0,1},{0,1},VfxMotionKind::VelocityAlignedSharpHead,17,18,negative_zero}};
    const std::array parameters{VfxParameterRecord{0xffffffffu,VfxParameterType::Int,0x80000001u},
        VfxParameterRecord{25,VfxParameterType::CurveRow,1}};
    const std::array textures{VfxTextureBindingRecord{1,12,3,4,negative_zero,{1,2},7,8,9}};
    const std::array<std::uint32_t,3> slices{4000000000u,1,0xffffffffu};
    const std::array curves{VfxCurveRecord{3,4,{1,1}}};
    const std::array keys{VfxCurveKey{negative_zero,subnormal},VfxCurveKey{0.5f,1}};
    const std::array lookup{VfxEffectLookupRecord{0xfedcba9876543210ull,1},VfxEffectLookupRecord{1,1}};
    const std::array resources{VfxTextureResourceRecord{12,{1,7}}};
    const std::array strings{std::byte{0},std::byte{34},std::byte{92},std::byte{237},std::byte{133},
        std::byte{141},std::byte{195},std::byte{169},std::byte{0}};
    const std::array upgrades{VfxUpgradeBindingRecord{0xf123456789abcdefull,17,{1,2}}};
    const std::array<std::uint32_t,3> sequences{4,37,38};
    const std::array light_ranges{VfxRange{0,1}};
    const std::array light_recipes{VfxLightRecipe{0xf123456789abcdefull,11,12,13,4,2.5f,6.5f,{-0.0f,.25f,.75f,1},{2,3,4},5,.5f,{1,2},1,true}};
    const VfxImage image{definitions,emitters,sprites,meshes,decorative,
        {effects,sources,outputs,parameters,textures,slices,curves,keys,lookup,resources,strings,upgrades,sequences},
        true,65535,0xfedcba0123456789ull,
        {{100,10,20},{101,11,21},{102,12,22},{103,13,23},{104,14,24},{105,15,25}},true,light_ranges,light_recipes};
    const auto source = hs::content::EmitReleaseVfxCpp(image);
    Check(source == hs::content::EmitReleaseVfxCpp(image));
    for (const std::string_view token : {"constinit const VfxImage kVfx{", "VfxProgramView{", "const VfxImage &Vfx() noexcept",
        "std::bit_cast<float>(std::uint32_t{2147483648u})", "std::bit_cast<float>(std::uint32_t{1u})",
        "18364758544493064720ull", "17375808098319191535ull", "18364757894559524745ull",
        "std::byte{237u}", "std::byte{195u}", "std::byte{0u}", "{1u,7u}", "{1u,2u}",
        "{105u,15u,25u}", "true,65535u,"})
        Check(source.find(token) != std::string::npos);
    std::size_t arrays = 0, offset = 0;
    while ((offset = source.find("constinit const std::array<", offset)) != std::string::npos) { ++arrays; ++offset; }
    Check(arrays == 20);
    for (const std::string_view token : {"std::vector", "memcpy", "DecodeVfx", "LoadVfx", "new ", "[]"})
        Check(source.find(token) == std::string::npos);
    Check(source.find("18364758544493064720ull") < source.find("{1ull,0u,1u,VfxDefinitionKind"));
    const auto empty = hs::content::EmitReleaseVfxCpp(VfxImage{});
    Check(empty.find("std::array<CookedVfxDefinition,0>") != std::string::npos &&
        empty.find("std::array<std::byte,0>") != std::string::npos && empty.find("false,0u,0ull,") != std::string::npos &&
        empty.find("},false,\n") != std::string::npos);
#ifdef HS_RELEASE_VFX_CODEGEN_TEST_DIR
    const std::filesystem::path directory{HS_RELEASE_VFX_CODEGEN_TEST_DIR};
    std::filesystem::create_directories(directory);
    const auto write = [&](const char *name, std::string_view text) {
        std::ofstream file(directory / name, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        Check(bool(file));
    };
    write("release_vfx.fixture.cpp", source);
    write("release_vfx.check.cpp", kRoundtripCheck);
#else
    (void)kRoundtripCheck;
#endif
}
