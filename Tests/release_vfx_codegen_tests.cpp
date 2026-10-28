#include "release_vfx_codegen.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#ifdef HS_RELEASE_VFX_CODEGEN_TEST_DIR
#include <filesystem>
#include <fstream>
#endif

namespace
{
void Check(bool condition, std::source_location location = std::source_location::current())
{
    if (!condition) throw std::runtime_error("release VFX codegen regression at line " +
        std::to_string(location.line()));
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
    Check(v.effect_output_profiles.size() == 1 &&
        v.HasOutput(1, VfxOutputProfile::SpriteSmoke6WayOit) &&
        !v.HasOutput(1, VfxOutputProfile::RibbonAdd) && !v.HasOutput(0, VfxOutputProfile::Light));
    Check(v.light_effect_ranges.size()==1 && Range(v.light_effect_ranges[0],0,1) && v.light_recipes.size()==1);
    Check(v.ground_effect_ranges.size()==1 && Range(v.ground_effect_ranges[0],0,2) &&
        v.HasGroundRecipes(1) && v.ground_recipes.size()==2);
    const auto &g=v.ground_recipes[0];
    Check(g.effect_id==43 && g.effect_handle==1 && g.source_index==2 && g.source_stable_id==3 &&
        g.output_index==4 && g.gradient_row==31 && g.payload_kind==5 && g.timing_kind==6 &&
        g.min_quality==2 && g.source_type==VfxSourceType::Direct &&
        g.profile==VfxOutputProfile::GroundSdfSoft &&
        g.motion==VfxMotionKind::StableBoundaryInwardPulse && g.kind==VfxGroundRecipeKind::Ticks &&
        g.knot_count==4 && Same(g.knots[0],-0.0f) && Same(g.knots[1],.25f) &&
        std::bit_cast<std::uint32_t>(g.knots[2])==1u && Same(g.knots[3],1.0f));
    Check(Range(g.pulse_curve_keys,1,2) && g.pulse_curve_interpolation==3 && g.tick_count==9 &&
        Same(g.seconds,2.5f) && Same(g.primary,.15f) && Same(g.secondary,1.25f) &&
        Same(g.rgb,Float3{2,3,4}) && Same(g.hdr,5) && Same(g.alpha,.5f) &&
        Same(g.motion_rate_hz,.8f) && Same(g.motion_amplitude,.22f) &&
        Same(g.motion_inset_fraction,.12f) && Same(g.tertiary,.125f));
    Check(Range(g.mask_variants[0].slices,8,2) && Same(g.mask_variants[0].strength,.35f) &&
        g.mask_variants[0].valid && g.mask_variants[0].present &&
        Range(g.mask_variants[1].slices,4,3) && Same(g.mask_variants[1].strength,.5f) &&
        !g.mask_variants[1].valid && g.mask_variants[1].present &&
        Range(g.mask_variants[2].slices,6,1) && Same(g.mask_variants[2].strength,.625f) &&
        g.mask_variants[2].valid && !g.mask_variants[2].present);
    Check(g.texture_contract_valid && g.source_output_count==1 && g.effect_lookup_count==1 &&
        g.source_parameters_empty &&
        g.event_admission_profiles==(kVfxGroundEventDirectAdd|kVfxGroundEventDirectSoft));
    const auto &g1=v.ground_recipes[1];
    Check(g1.effect_id==43 && g1.effect_handle==1 && g1.source_index==2 && g1.source_stable_id==3 &&
        g1.output_index==5 && g1.gradient_row==31 && g1.payload_kind==5 && g1.timing_kind==6 &&
        g1.min_quality==2 && g1.source_type==VfxSourceType::Direct &&
        g1.profile==VfxOutputProfile::GroundSdfSoft && g1.motion==VfxMotionKind::StableBoundaryInwardPulse &&
        g1.kind==VfxGroundRecipeKind::ExpandingRing && g1.knot_count==4 &&
        Same(g1.knots[0],0) && Same(g1.knots[1],.5f) && Same(g1.knots[2],0) && Same(g1.knots[3],0) &&
        Range(g1.pulse_curve_keys,1,2) && g1.pulse_curve_interpolation==3 && g1.tick_count==9 &&
        Same(g1.seconds,2.5f) && Same(g1.primary,.15f) && Same(g1.secondary,1.25f) &&
        Same(g1.rgb,Float3{2,3,4}) && Same(g1.hdr,5) && Same(g1.alpha,.5f) &&
        Same(g1.motion_rate_hz,.8f) && Same(g1.motion_amplitude,.22f) &&
        Same(g1.motion_inset_fraction,.12f) && Same(g1.tertiary,.07f) &&
        Range(g1.mask_variants[0].slices,8,2) && Same(g1.mask_variants[0].strength,.35f) &&
        g1.mask_variants[0].valid && g1.mask_variants[0].present && g1.texture_contract_valid &&
        g1.source_output_count==1 && g1.effect_lookup_count==1 && g1.source_parameters_empty &&
        g1.event_admission_profiles==0);
    Check(v.flash_effect_ranges.size()==1 && v.HasFlashRecipes(1) && v.flash_recipes.size()==1 &&
        v.flash_recipes[0].effect_id==44 && v.flash_recipes[0].supported_quality_mask==4 &&
        v.flash_recipes[0].spawn.shape==VfxImpactShape::Flame &&
        v.flash_recipes[0].spawn.event_tick==0x1'0000'0005ull &&
        v.flash_recipes[0].mask_strengths[2]==.75f);
    Check(v.persistent_stamp_effect_ranges.size()==1 && v.persistent_stamp_recipes.size()==1 &&
        v.persistent_stamp_recipes[0].effect_id==45 && v.persistent_stamp_recipes[0].source_visual_kind==12 &&
        v.persistent_stamp_recipes[0].spawn.shape==VfxImpactShape::StatusStamp &&
        v.persistent_stamp_recipes[0].mask_slices[2].first==5 &&
        v.persistent_fire_recipes.size()==1 && v.persistent_fire_recipes[0].rate==8 &&
        v.persistent_fire_recipes[0].authoritative &&
        v.persistent_fire_recipes[0].spawn.shape==VfxImpactShape::Flame &&
        v.owned_status_tick_recipes.size()==1 && v.owned_status_tick_recipes[0].tick_effect_id==47 &&
        v.owned_status_tick_recipes[0].mask_slices[2]==5);
    Check(v.mote_source_recipes.size()==1 && v.mote_source_recipes[0].count==7 && v.mote_source_recipes[0].rate==3 &&
        v.mote_source_recipes[0].common_params_valid && v.mote_source_recipes[0].spark_valid &&
        v.mote_output_recipes.size()==1 && v.mote_output_recipes[0].min_quality==2 &&
        v.mote_output_recipes[0].gradient_row==31 &&
        v.mote_output_recipes[0].mask_slices[2].first==5 &&
        v.mote_output_recipes[0].mask_strengths[2]==.625f &&
        v.mote_output_recipes[0].persistent_curve_row==6 &&
        v.mote_output_recipes[0].persistent_size==.075f &&
        Same(v.mote_output_recipes[0].color,Float4{1,2,3,4}) &&
        v.mote_output_recipes[0].mask_present_mask==4 && v.mote_output_recipes[0].output_color_valid &&
        v.mote_output_recipes[0].spark_textures_valid);
    Check(v.screen_effect_recipes.size()==1 && v.screen_recipes.size()==1 &&
        v.screen_effect_recipes[0].effect_id==51 && Same(v.screen_effect_recipes[0].seconds,3.25f) &&
        Range(v.screen_effect_recipes[0].outputs,0,1) && v.screen_effect_recipes[0].route_valid);
    const auto &screen=v.screen_recipes[0];
    Check(screen.source_index==9 && screen.output_index==37 && screen.min_quality==1 && screen.gradient_row==41 &&
        screen.shape==VfxScreenShape::DirectionalArc && Same(screen.knot_start,.125f) && Same(screen.knot_end,.875f) &&
        Same(screen.rgb,Float3{.1f,.2f,.3f}) && Same(screen.alpha,.5f) && Same(screen.hdr,2) &&
        Same(screen.center_clear,.625f) && Same(screen.edge_power,3));
    Check(v.volume_effect_recipes.size()==1 && v.volume_recipes.size()==1 &&
        v.volume_effect_recipes[0].effect_id==52 && Same(v.volume_effect_recipes[0].seconds,4.5f) &&
        Range(v.volume_effect_recipes[0].outputs,0,1) && v.volume_effect_recipes[0].route_valid);
    const auto &volume=v.volume_recipes[0];
    Check(volume.source_index==8 && volume.output_index==27 && volume.source_id==39 && volume.min_quality==2 &&
        volume.gradient_row==43 && volume.shape==VfxVolumeShape::NoisySphere && volume.noise==VfxVolumeNoise::Fbm3D &&
        Same(volume.knot_start,.25f) && Same(volume.knot_end,.75f) && Same(volume.density,1.25f) &&
        Same(volume.alpha,.5f) && Same(volume.anisotropy,.25f) && Same(volume.hdr,2) && volume.steps==12 &&
        volume.half_resolution==1 && Same(volume.rgb,Float3{.4f,.5f,.6f}));
)cpp"
R"cpp(    Check(v.entity_surface_effect_recipes.size()==1 && v.entity_surface_recipes.size()==1 &&
        v.entity_surface_effect_recipes[0].effect_id==53 &&
        v.entity_surface_routes.phase_effect==5 && v.entity_surface_routes.death_effect==6 &&
        v.entity_surface_routes.trap_point_rate_effect==7);
    Check(v.ribbon_effect_recipes.size()==1 && v.ribbon_effect_recipes[0].effect_id==61 &&
        v.ribbon_effect_recipes[0].handle==3 && v.ribbon_effect_recipes[0].valid_handle &&
        v.ribbon_slice_indices.size()==2 && v.ribbon_slice_indices[0]==2 && v.ribbon_slice_indices[1]==5);
    Check(v.ribbon_wake_effect_ranges.size()==1 && Range(v.ribbon_wake_effect_ranges[0],0,1) &&
        v.ribbon_wake_recipes.size()==1 && v.ribbon_wake_recipes[0].effect_handle==3 &&
        v.ribbon_wake_recipes[0].importance==4 &&
        v.ribbon_wake_recipes[0].min_quality==1 && v.ribbon_wake_recipes[0].detail_min_quality==2 &&
        v.ribbon_wake_recipes[0].gradient_row==17 && Range(v.ribbon_wake_recipes[0].detail_slices,0,2) &&
        Same(v.ribbon_wake_recipes[0].width_head,.06f) && Same(v.ribbon_wake_recipes[0].width_tail,.04f) &&
        Same(v.ribbon_wake_recipes[0].hdr,2.5f) && Same(v.ribbon_wake_recipes[0].detail_strength,.35f));
    Check(v.ribbon_fixed_effect_ranges.size()==1 && Range(v.ribbon_fixed_effect_ranges[0],0,1) &&
        v.ribbon_fixed_recipes.size()==1 && v.ribbon_fixed_recipes[0].effect_handle==3 &&
        v.ribbon_fixed_recipes[0].kind==VfxRibbonFixedKind::ExplosivePrePull &&
        v.ribbon_fixed_recipes[0].segments==16 && Same(v.ribbon_fixed_recipes[0].seconds,.45f) &&
        Same(v.ribbon_fixed_recipes[0].taper,.55f) &&
        Same(v.ribbon_fixed_recipes[0].outer_width_multiplier,2.8f) &&
        v.ribbon_fixed_recipes[0].core_output.profile==VfxOutputProfile::RibbonAdd &&
        v.ribbon_fixed_recipes[0].core_output.travel_pulse &&
        v.ribbon_fixed_recipes[0].outer_output.profile==VfxOutputProfile::RibbonOit &&
        Same(v.ribbon_fixed_recipes[0].outer_output.noise_warp,.08f) &&
        Same(v.ribbon_fixed_recipes[0].outer_output.color,Float4{.4f,.3f,.2f,.1f}));
    Check(v.ribbon_projectile_effect_ranges.size()==1 && Range(v.ribbon_projectile_effect_ranges[0],0,1) &&
        v.ribbon_projectile_sources.size()==1 && v.ribbon_projectile_sources[0].effect_handle==3 &&
        v.ribbon_projectile_sources[0].source_id==9 && v.ribbon_projectile_sources[0].knot_count==4 &&
        Same(v.ribbon_projectile_sources[0].history_seconds,.25f) &&
        Same(v.ribbon_projectile_sources[0].knots[2],.8f) &&
        Range(v.ribbon_projectile_sources[0].outputs,0,1) &&
        v.ribbon_projectile_outputs.size()==1 && v.ribbon_projectile_outputs[0].min_quality==2 &&
        v.ribbon_projectile_outputs[0].output.motion==VfxMotionKind::TwoPhaseNarrowWakes &&
        Same(v.ribbon_projectile_outputs[0].output.width_head,.08f) &&
        Same(v.ribbon_projectile_outputs[0].output.phase_offset,.12f) &&
        Range(v.ribbon_projectile_outputs[0].detail_slices,1,2) &&
        Same(v.ribbon_projectile_outputs[0].detail_strength,.35f) &&
        v.ribbon_projectile_outputs[0].detail_present);
    Check(v.ribbon_cancel_effect_ranges.size()==1 && Range(v.ribbon_cancel_effect_ranges[0],0,1) &&
        v.ribbon_cancel_recipes.size()==1 && v.ribbon_cancel_recipes[0].effect_handle==3 &&
        v.ribbon_cancel_recipes[0].source_id==9 && v.ribbon_cancel_recipes[0].min_quality==2 &&
        Same(v.ribbon_cancel_recipes[0].seconds,.9f) &&
        Same(v.ribbon_cancel_recipes[0].history_seconds,.24f) &&
        Same(v.ribbon_cancel_recipes[0].width_head,.08f) &&
        v.ribbon_cancel_recipes[0].motion==VfxMotionKind::ReverseFlowBow &&
        Same(v.ribbon_cancel_recipes[0].color,Float4{.2f,.3f,.4f,.7f}) &&
        v.ribbon_cancel_recipes[0].gradient_row==19);
    Check(v.ribbon_link_effect_recipes.size()==1 &&
        v.ribbon_link_effect_recipes[0].kind==VfxRibbonLinkKind::PickupCollect &&
        Same(v.ribbon_link_effect_recipes[0].window_start,.1f) &&
        Range(v.ribbon_link_effect_recipes[0].outputs,0,1) &&
        v.ribbon_link_output_recipes.size()==1 &&
        v.ribbon_link_output_recipes[0].source_id==9 &&
        v.ribbon_link_output_recipes[0].min_quality==2 &&
        v.ribbon_link_output_recipes[0].output.motion==VfxMotionKind::CurvesTowardCollector &&
        Same(v.ribbon_link_output_recipes[0].details[2].strength,.5f) &&
        v.ribbon_link_output_recipes[0].details[2].present);
    Check(v.ribbon_event_effect_recipes.size()==1 &&
        v.ribbon_event_effect_recipes[0].effect_id==62 &&
        v.ribbon_event_effect_recipes[0].effect_handle==3 &&
        v.ribbon_event_effect_recipes[0].importance==4 &&
        v.ribbon_event_effect_recipes[0].payload_kind==5 &&
        Same(v.ribbon_event_effect_recipes[0].seconds,.6f) &&
        Range(v.ribbon_event_effect_recipes[0].sources,0,1) &&
        v.ribbon_event_sources.size()==1 && v.ribbon_event_sources[0].effect_handle==3 &&
        v.ribbon_event_sources[0].source_id==9 && v.ribbon_event_sources[0].axial_source &&
        v.ribbon_event_sources[0].knot_count==4 && Same(v.ribbon_event_sources[0].knots[2],.8f) &&
        Same(v.ribbon_event_sources[0].history_seconds,.25f) &&
        Same(v.ribbon_event_sources[0].axial_arc_by_quality[2],75) &&
        Range(v.ribbon_event_sources[0].outputs,0,1) && v.ribbon_event_outputs.size()==1 &&
        v.ribbon_event_outputs[0].axial && v.ribbon_event_outputs[0].min_quality==2 &&
        Same(v.ribbon_event_outputs[0].arc_degrees,75) &&
        v.ribbon_event_outputs[0].output.motion==VfxMotionKind::ShortLineIncomingVelocity &&
        Range(v.ribbon_event_outputs[0].detail_slices,1,2) &&
        Same(v.ribbon_event_outputs[0].detail_strength,.35f) && v.ribbon_event_outputs[0].detail_present);
    Check(v.fresnel_effect_recipes.size()==1 && v.fresnel_effect_recipes[0].effect_id==62 &&
        v.fresnel_recipes.size()==1 && v.fresnel_recipes[0].effect_handle==3 &&
        v.fresnel_recipes[0].source_stable_id==9 && v.fresnel_recipes[0].output_index==4 &&
        v.fresnel_recipes[0].kind==VfxFresnelShellKind::BossCrown &&
        v.fresnel_recipes[0].sector_count==8 && Same(v.fresnel_recipes[0].rotation_hz,.25f) &&
        v.fresnel_recipes[0].gradient_row==17 && Same(v.fresnel_recipes[0].motion_inset_fraction,.3f));
    Check(v.mesh_effect_recipes.size()==1 && v.mesh_effect_recipes[0].effect_handle==4 &&
        v.mesh_recipes.size()==1 && v.mesh_recipes[0].effect_handle==4 && v.mesh_recipes[0].output_index==6 &&
        v.mesh_recipes[0].source_stable_id==7 && v.mesh_recipes[0].mesh_index==6 &&
        v.mesh_recipes[0].gradient_row==31 && Same(v.mesh_recipes[0].shell_thickness,.22f));
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
    const std::array lookup_by_id{VfxEffectLookupRecord{1,2},VfxEffectLookupRecord{0xfedcba9876543210ull,1}};
    const std::array resources{VfxTextureResourceRecord{12,{1,7}}};
    const std::array strings{std::byte{0},std::byte{34},std::byte{92},std::byte{237},std::byte{133},
        std::byte{141},std::byte{195},std::byte{169},std::byte{0}};
    const std::array upgrades{VfxUpgradeBindingRecord{0xf123456789abcdefull,17,{1,2}}};
    const std::array<std::uint32_t,3> sequences{4,37,38};
    const std::array light_ranges{VfxRange{0,1}};
    const std::array light_recipes{VfxLightRecipe{0xf123456789abcdefull,11,12,13,4,2.5f,6.5f,{-0.0f,.25f,.75f,1},{2,3,4},5,.5f,{1,2},1,true}};
    const std::array<VfxRange,0> ballistic_ranges{};
    const std::array<std::uint32_t,1> output_profiles{1u << static_cast<unsigned>(VfxOutputProfile::SpriteSmoke6WayOit)};
    const std::array ballistic_recipes{VfxBallisticRecipe{
        123ull,1,2,3,4,5,0,6,2,8,3,5,2,VfxSourceType::BallisticCollision,
        VfxMotionKind::BallisticBurstGravitySpin,{0,.75f,0,0},.5f,.06f,.12f,1.5f,2.6f,
        -9.81f,.2f,.8f,-1,75,5,11,{1,.5f,.25f,.75f},3,true}};
    const std::array distortion_ranges{VfxRange{0,1}};
    const std::array distortion_recipes{VfxDistortionRecipe{123ull,1,2,3,4,0,0,1,2,VfxDistortionShape::RadialRing,
        VfxMotionKind::SingleOutwardRefractivePulse,{0,.5f,1,0},2,.8f,.02f,.75f}};
    const std::array decal_ranges{VfxRange{0,1}};
    const std::array decal_recipes{VfxDecalRecipe{124ull,1,3,4,5,0,0,1,4,VfxDecalKind::Organic,
        {0,.2f,.6f,1},3,2,.8f,.4f,{.1f,.2f,.3f},7,12,4,1}};
    VfxGroundRecipe ground_recipe{}; ground_recipe.effect_id=43; ground_recipe.effect_handle=1;
    ground_recipe.source_index=2; ground_recipe.source_stable_id=3; ground_recipe.output_index=4;
    ground_recipe.gradient_row=31; ground_recipe.payload_kind=5; ground_recipe.timing_kind=6;
    ground_recipe.min_quality=2; ground_recipe.source_type=VfxSourceType::Direct;
    ground_recipe.profile=VfxOutputProfile::GroundSdfSoft;
    ground_recipe.motion=VfxMotionKind::StableBoundaryInwardPulse;
    ground_recipe.kind=VfxGroundRecipeKind::Ticks;
    ground_recipe.knot_count=4; ground_recipe.knots={-0.0f,.25f,std::bit_cast<float>(1u),1.0f};
    ground_recipe.seconds=2.5f; ground_recipe.primary=.15f; ground_recipe.secondary=1.25f;
    ground_recipe.rgb={2,3,4}; ground_recipe.hdr=5; ground_recipe.alpha=.5f;
    ground_recipe.motion_rate_hz=.8f; ground_recipe.motion_amplitude=.22f;
    ground_recipe.motion_inset_fraction=.12f;
    ground_recipe.source_output_count=1; ground_recipe.source_parameters_empty=true;
    ground_recipe.effect_lookup_count=1;
    ground_recipe.event_admission_profiles=kVfxGroundEventDirectAdd|kVfxGroundEventDirectSoft;
    ground_recipe.tick_count=9; ground_recipe.pulse_curve_keys={1,2};
    ground_recipe.pulse_curve_interpolation=3; ground_recipe.tertiary=.125f;
    ground_recipe.mask_variants[0]={{8,2},.35f,true,true};
    ground_recipe.mask_variants[1]={{4,3},.5f,false,true};
    ground_recipe.mask_variants[2]={{6,1},.625f,true,false};
    ground_recipe.texture_contract_valid=true;
    VfxGroundRecipe expanding_recipe=ground_recipe; expanding_recipe.output_index=5;
    expanding_recipe.event_admission_profiles=0;
    expanding_recipe.kind=VfxGroundRecipeKind::ExpandingRing; expanding_recipe.knots={0,.5f,0,0};
    expanding_recipe.primary=.15f; expanding_recipe.secondary=1.25f; expanding_recipe.tertiary=.07f;
    expanding_recipe.mask_variants[0]={{8,2},.35f,true,true};
    const std::array ground_ranges{VfxRange{0,2}};
    const std::array ground_recipes{ground_recipe,expanding_recipe};
    VfxFlashRecipe flash_recipe{}; flash_recipe.effect_id=44; flash_recipe.effect_handle=1;
    flash_recipe.source_index=2; flash_recipe.source_stable_id=3; flash_recipe.output_index=4;
    flash_recipe.payload_kind=5; flash_recipe.min_quality=2; flash_recipe.knots={.1f,.8f};
    flash_recipe.seconds=.9f; flash_recipe.spawn.event_tick=0x1'0000'0005ull;
    flash_recipe.spawn.shape=VfxImpactShape::Flame;
    flash_recipe.spawn.size=.4f; flash_recipe.mask_slice_ranges[2]={3,2};
    flash_recipe.mask_strengths[2]=.75f; flash_recipe.supported_quality_mask=4;
    flash_recipe.mask_present_mask=4; flash_recipe.unique_effect_identity=true;
    const std::array flash_ranges{VfxRange{0,1}};
    const std::array flash_recipes{flash_recipe};
    VfxPersistentStampRecipe stamp_recipe{}; stamp_recipe.effect_id=45; stamp_recipe.effect_handle=1;
    stamp_recipe.source_visual_kind=12; stamp_recipe.payload_kind=6; stamp_recipe.source_stable_id=7;
    stamp_recipe.timing_kind=0; stamp_recipe.fixed_seconds=.6f; stamp_recipe.source_end=.55f;
    stamp_recipe.spawn.shape=VfxImpactShape::StatusStamp; stamp_recipe.spawn.curve_row=4;
    stamp_recipe.mask_slices[2]={5,3}; stamp_recipe.mask_strengths[2]=.38f;
    stamp_recipe.supported_quality_mask=7; stamp_recipe.mask_present_mask=4; stamp_recipe.bleed_extend=true;
    const std::array stamp_ranges{VfxRange{0,1}};
    const std::array stamp_recipes{stamp_recipe};
    VfxPersistentFireRecipe fire_recipe{}; fire_recipe.effect_id=46; fire_recipe.effect_handle=1;
    fire_recipe.source_stable_id=8; fire_recipe.rate=8; fire_recipe.radius_scale=.5f;
    fire_recipe.spawn.shape=VfxImpactShape::Flame; fire_recipe.spawn.size=.7f;
    fire_recipe.mask_slices={2,3}; fire_recipe.mask_strength=.4f;
    fire_recipe.authoritative=true;
    const std::array fire_ranges{VfxRange{0,1}};
    const std::array fire_recipes{fire_recipe};
    VfxOwnedStatusTickRecipe tick_recipe{}; tick_recipe.tick_effect_id=47; tick_recipe.status_effect_id=48;
    tick_recipe.tick_effect_handle=1; tick_recipe.status_effect_handle=2; tick_recipe.min_quality=1;
    tick_recipe.tick_seconds=.2f; tick_recipe.delay_fraction=0; tick_recipe.lifetime_fraction=.5f;
    tick_recipe.spawn.shape=VfxImpactShape::Pulse;
    tick_recipe.mask_slices={3,4,5}; tick_recipe.mask_strength=.38f; tick_recipe.bleed=true;
    const std::array tick_ranges{VfxRange{0,1}};
    const std::array tick_recipes{tick_recipe};
    const std::array mote_source_recipes{VfxMoteSourceRecipe{7,3,.2f,.8f,.45f,42.0f,true,true,true,true}};
    VfxMoteOutputRecipe mote_output_recipe{};
    mote_output_recipe.min_quality=2; mote_output_recipe.gradient_row=31;
    mote_output_recipe.mask_slices={VfxRange{1,2},VfxRange{3,1},VfxRange{5,2}};
    mote_output_recipe.mask_strengths={.25f,.5f,.625f}; mote_output_recipe.persistent_curve_row=6;
    mote_output_recipe.persistent_size=.075f; mote_output_recipe.hdr=2;
    mote_output_recipe.color={1,2,3,4}; mote_output_recipe.mask_present_mask=4;
    mote_output_recipe.mask_quality_mask=7; mote_output_recipe.output_color_valid=true;
    mote_output_recipe.persistent_params_valid=true; mote_output_recipe.spark_textures_valid=true;
    const std::array mote_output_recipes{mote_output_recipe};
    const std::array screen_effect_recipes{VfxScreenEffectRecipe{51,3.25f,{0,1},true}};
    const std::array screen_recipes{VfxScreenRecipe{9,37,1,41,VfxScreenShape::DirectionalArc,
        .125f,.875f,{.1f,.2f,.3f},.5f,2,.625f,3}};
    const std::array volume_effect_recipes{VfxVolumeEffectRecipe{52,4.5f,{0,1},true}};
    const std::array volume_recipes{VfxVolumeRecipe{8,27,39,2,43,VfxVolumeShape::NoisySphere,VfxVolumeNoise::Fbm3D,
        .25f,.75f,1.25f,.5f,.25f,2,12,1,{.4f,.5f,.6f}}};
    const std::array entity_surface_effect_recipes{VfxEntitySurfaceEffectRecipe{53,9,10,1,1,1.5f,{0,1},true}};
    const std::array entity_surface_recipes{VfxEntitySurfaceRecipe{1,2,3,1,4,VfxEntitySurfaceShape::Transition,
        VfxEntitySurfaceDirection::UpAndOut,.08f,.88f,2.4f,.07f,{.2f,.3f,.4f,.72f},4.8f,.72f,false,false}};
    const VfxEntitySurfaceRouteHandles entity_surface_routes{5,6,7};
    VfxRibbonEffectRecipe ribbon_effect{}; ribbon_effect.effect_id=61; ribbon_effect.handle=3;
    ribbon_effect.importance=4; ribbon_effect.payload_kind=7; ribbon_effect.input_mode=1;
    ribbon_effect.timing_kind=1; ribbon_effect.lookup_count=1; ribbon_effect.identity_count=1;
    ribbon_effect.seconds=.75f; ribbon_effect.sources={0,1}; ribbon_effect.valid_handle=true;
    const std::array ribbon_effects{ribbon_effect};
    const std::array<std::uint32_t,2> ribbon_slices{2,5};
    const std::array ribbon_wake_ranges{VfxRange{0,1}};
    const std::array ribbon_wake_recipes{VfxRibbonWakeRecipe{3,4,1,2,17,{0,2},{.2f,.3f,.4f,.5f},.06f,.04f,2.5f,.35f}};
    VfxRibbonFixedRecipe ribbon_fixed{}; ribbon_fixed.effect_handle=3; ribbon_fixed.importance=4;
    ribbon_fixed.core_min_quality=1; ribbon_fixed.outer_min_quality=2; ribbon_fixed.segments=16;
    ribbon_fixed.kind=VfxRibbonFixedKind::ExplosivePrePull; ribbon_fixed.seconds=.45f;
    ribbon_fixed.taper=.55f; ribbon_fixed.outer_width_multiplier=2.8f;
    ribbon_fixed.core_output={VfxOutputProfile::RibbonAdd,VfxMotionKind::CurvedLinkTravelingHead,
        {.1f,.2f,.3f,.4f},.07f,.0385f,0,0,3,0,31,0xffffffffu,false,true};
    ribbon_fixed.outer_output={VfxOutputProfile::RibbonOit,VfxMotionKind::SoftWiderSheath,
        {.4f,.3f,.2f,.1f},.196f,.1078f,.08f,0,1.5f,0,32,0xffffffffu,false,false};
    const std::array ribbon_fixed_ranges{VfxRange{0,1}};
    const std::array ribbon_fixed_recipes{ribbon_fixed};
    const std::array ribbon_projectile_ranges{VfxRange{0,1}};
    const std::array ribbon_projectile_sources{VfxRibbonProjectileSourceRecipe{
        3,9,4,.25f,{.1f,.2f,.8f,.9f},{0,1}}};
    const std::array ribbon_projectile_outputs{VfxRibbonProjectileOutputRecipe{
        2,{VfxOutputProfile::RibbonOit,VfxMotionKind::TwoPhaseNarrowWakes,{.3f,.4f,.5f,.6f},
            .08f,.02f,.11f,.12f,2.2f,.13f,33,0xffffffffu,true,false},
        {1,2},.35f,true}};
    const std::array ribbon_cancel_ranges{VfxRange{0,1}};
    const std::array ribbon_cancel_recipes{VfxRibbonCancelRecipe{3,4,9,2,.9f,.24f,
        .08f,.04f,{.2f,.3f,.4f,.7f},VfxOutputProfile::RibbonAdd,
        VfxMotionKind::ReverseFlowBow,2.5f,19}};
    const std::array ribbon_link_effects{VfxRibbonLinkEffectRecipe{3,4,
        VfxRibbonLinkKind::PickupCollect,.9f,.1f,1.0f,0,12,{0,1}}};
    const std::array ribbon_link_outputs{VfxRibbonLinkOutputRecipe{9,2,false,.04f,
        {VfxOutputProfile::RibbonAdd,VfxMotionKind::CurvesTowardCollector,{.2f,.3f,.4f,.7f},
            .04f,.04f,0,0,2,0,19,0xffffffffu,false,false},
        std::array{VfxRibbonLinkDetailRecipe{{0,2},.35f,true,true},
            VfxRibbonLinkDetailRecipe{{2,2},.4f,true,true},
            VfxRibbonLinkDetailRecipe{{4,2},.5f,true,true}}}};
    const std::array ribbon_event_effects{VfxRibbonEventEffectRecipe{62,3,4,5,.6f,{0,1}}};
    const std::array ribbon_event_sources{VfxRibbonEventSourceRecipe{
        3,9,4,true,{.1f,.2f,.8f,.9f},.25f,{35,45,55},{50,65,75},{0,1}}};
    const std::array ribbon_event_outputs{VfxRibbonEventOutputRecipe{2,true,true,75,
        {VfxOutputProfile::RibbonAdd,VfxMotionKind::ShortLineIncomingVelocity,{.2f,.3f,.4f,.7f},
            .08f,.02f,0,0,2,0,19,0xffffffffu,false,false},
        {1,2},.35f,true}};
    const std::array fresnel_effects{VfxFresnelEffectRecipe{62,3,VfxFresnelShellKind::BossCrown,{0,1},true}};
    const std::array fresnel_recipes{VfxFresnelRecipe{3,2,9,4,1,VfxFresnelShellKind::BossCrown,
        .1f,.9f,8,0,0,.25f,0,{.2f,.3f,.4f,.5f},2,17,.1f,.2f,.3f}};
    const std::array mesh_effects{VfxMeshEffectRecipe{4,{0,1},true}};
    const std::array mesh_recipes{VfxMeshRecipe{4,5,6,7,6,2,31,{.4f,.5f,.6f,.7f},3,.25f,0,.22f}};
    const VfxImage image{definitions,emitters,sprites,meshes,decorative,
        {effects,sources,outputs,parameters,textures,slices,curves,keys,lookup,resources,strings,upgrades,sequences},
        true,65535,0xfedcba0123456789ull,
        {{100,10,20},{101,11,21},{102,12,22},{103,13,23},{104,14,24},{105,15,25}},true,light_ranges,light_recipes,ballistic_ranges,ballistic_recipes,output_profiles,
        distortion_ranges,distortion_recipes,decal_ranges,decal_recipes,
        ground_ranges,ground_recipes,flash_ranges,flash_recipes,
        stamp_ranges,stamp_recipes,fire_ranges,fire_recipes,tick_ranges,tick_recipes,
        mote_source_recipes,mote_output_recipes,screen_effect_recipes,screen_recipes,
        volume_effect_recipes,volume_recipes,entity_surface_effect_recipes,entity_surface_recipes,entity_surface_routes,
        ribbon_effects,ribbon_slices,
        ribbon_wake_ranges,ribbon_wake_recipes,ribbon_fixed_ranges,ribbon_fixed_recipes,
        ribbon_projectile_ranges,ribbon_projectile_sources,ribbon_projectile_outputs,
        ribbon_cancel_ranges,ribbon_cancel_recipes,ribbon_link_effects,ribbon_link_outputs,
        ribbon_event_effects,ribbon_event_sources,ribbon_event_outputs,lookup_by_id,
        fresnel_effects,fresnel_recipes,mesh_effects,mesh_recipes};
    const auto source = hs::content::EmitReleaseVfxCpp(image);
    Check(source == hs::content::EmitReleaseVfxCpp(image));
    for (const std::string_view token : {"constinit const VfxImage kVfx{", "VfxProgramView{", "const VfxImage &Vfx() noexcept",
        "std::bit_cast<float>(std::uint32_t{2147483648u})", "std::bit_cast<float>(std::uint32_t{1u})",
        "18364758544493064720ull", "17375808098319191535ull", "18364757894559524745ull",
        "std::byte{237u}", "std::byte{195u}", "std::byte{0u}", "{1u,7u}", "{1u,2u}",
        "{105u,15u,25u}", "true,65535u,"})
        Check(source.find(token) != std::string::npos);
    Check(source.find("std::array<VfxGroundRecipe,2> kGroundRecipes") != std::string::npos &&
          source.find("std::array<VfxFlashRecipe,1> kFlashRecipes") != std::string::npos &&
          source.find("std::array<VfxPersistentStampRecipe,1> kPersistentStampRecipes") != std::string::npos &&
          source.find("std::array<VfxPersistentFireRecipe,1> kPersistentFireRecipes") != std::string::npos &&
          source.find("std::array<VfxOwnedStatusTickRecipe,1> kOwnedStatusTickRecipes") != std::string::npos &&
          source.find("std::array<VfxMoteSourceRecipe,1> kMoteSourceRecipes") != std::string::npos &&
          source.find("std::array<VfxMoteOutputRecipe,1> kMoteOutputRecipes") != std::string::npos &&
          source.find("std::array<VfxScreenEffectRecipe,1> kScreenEffectRecipes") != std::string::npos &&
          source.find("std::array<VfxScreenRecipe,1> kScreenRecipes") != std::string::npos &&
          source.find("std::array<VfxVolumeEffectRecipe,1> kVolumeEffectRecipes") != std::string::npos &&
          source.find("std::array<VfxVolumeRecipe,1> kVolumeRecipes") != std::string::npos &&
          source.find("std::array<VfxEntitySurfaceEffectRecipe,1> kEntitySurfaceEffectRecipes") != std::string::npos &&
          source.find("std::array<VfxEntitySurfaceRecipe,1> kEntitySurfaceRecipes") != std::string::npos &&
          source.find("std::array<VfxRibbonEffectRecipe,1> kRibbonEffectRecipes") != std::string::npos &&
          source.find("std::array<VfxRange,1> kRibbonWakeEffectRanges") != std::string::npos &&
          source.find("std::array<VfxRibbonWakeRecipe,1> kRibbonWakeRecipes") != std::string::npos &&
          source.find("std::array<VfxRange,1> kRibbonFixedEffectRanges") != std::string::npos &&
          source.find("std::array<VfxRibbonFixedRecipe,1> kRibbonFixedRecipes") != std::string::npos &&
          source.find("std::array<VfxRange,1> kRibbonProjectileEffectRanges") != std::string::npos &&
          source.find("std::array<VfxRibbonProjectileSourceRecipe,1> kRibbonProjectileSources") != std::string::npos &&
          source.find("std::array<VfxRibbonProjectileOutputRecipe,1> kRibbonProjectileOutputs") != std::string::npos &&
          source.find("std::array<VfxRange,1> kRibbonCancelEffectRanges") != std::string::npos &&
          source.find("std::array<VfxRibbonCancelRecipe,1> kRibbonCancelRecipes") != std::string::npos &&
          source.find("std::array<VfxRibbonLinkEffectRecipe,1> kRibbonLinkEffectRecipes") != std::string::npos &&
          source.find("std::array<VfxRibbonLinkOutputRecipe,1> kRibbonLinkOutputRecipes") != std::string::npos &&
          source.find("std::array<VfxRibbonEventEffectRecipe,1> kRibbonEventEffectRecipes") != std::string::npos &&
          source.find("std::array<VfxRibbonEventSourceRecipe,1> kRibbonEventSources") != std::string::npos &&
          source.find("std::array<VfxRibbonEventOutputRecipe,1> kRibbonEventOutputs") != std::string::npos &&
          source.find("std::array<VfxFresnelEffectRecipe,1> kFresnelEffectRecipes") != std::string::npos &&
          source.find("std::array<VfxFresnelRecipe,1> kFresnelRecipes") != std::string::npos &&
          source.find("std::array<VfxMeshEffectRecipe,1> kMeshEffectRecipes") != std::string::npos &&
          source.find("std::array<VfxMeshRecipe,1> kMeshRecipes") != std::string::npos &&
          source.find("std::array<VfxEffectLookupRecord,2> kEffectLookupById") != std::string::npos &&
          source.find("{5u,6u,7u}") != std::string::npos);
    Check(source.find("123ull,1u,2u,3u,4u,5u,0u,6u,2u,8u,3u,5u,2u,VfxSourceType(2u)") != std::string::npos &&
          source.find("std::array<VfxBallisticRecipe,1> kBallisticRecipes") != std::string::npos);
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
