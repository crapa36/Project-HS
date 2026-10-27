#include <hs/runtime/vfx_image_io.hpp>

#include <hs/core/canonical_bytes.hpp>

namespace hs
{
namespace
{
template <typename T> void Range(CanonicalBytes &b, const T &r) { b.Fields(r.first, r.count); }
template <typename T, std::size_t N> void Array(CanonicalBytes &b, const std::array<T, N> &v)
{
    for (const auto &x : v) b.Scalar(x);
}
void Vec(CanonicalBytes &b, Float3 v) { b.Fields(v.x, v.y, v.z); }
void Vec(CanonicalBytes &b, Float4 v) { b.Fields(v.x, v.y, v.z, v.w); }
template <typename T, typename F> void Seq(CanonicalBytes &b, const T &v, F f)
{
    b.Sequence(v, f);
}
void Spawn(CanonicalBytes &b, const VfxFlashSpawnInput &r)
{
    Vec(b,r.position); Vec(b,r.color); b.Fields(r.size,r.delay,r.lifetime,r.event_tick,r.stable_seed,r.gradient_row,r.hdr,r.normalized_age);
    b.Scalar(r.shape); Vec(b,r.direction); b.Fields(r.aspect,r.curve_row,r.mask_slice,r.mask_strength); Vec(b,r.initial_offset); Vec(b,r.initial_velocity);
    b.Fields(r.orbit_radius,r.orbit_phase,r.orbit_rate,r.drag,r.oit,r.ground_base_anchor,r.fbm_octaves,r.smoke_first_frame,r.smoke_frame_count,
        r.domain_warp,r.smoke_fps,r.opacity_scale,r.motion_strength,r.procedural_flow_speed); Vec(b,r.previous_position); b.Scalar(r.interpolate_owner_position);
}
void Recipe(CanonicalBytes &b, const VfxLightRecipe &r)
{
    b.Fields(r.seed_xor,r.payload_kind,r.importance,r.min_quality,r.knot_count,r.seconds,r.radius); Array(b,r.knots); Vec(b,r.rgb);
    b.Fields(r.hdr,r.alpha); Range(b,r.curve_keys); b.Fields(r.interpolation,r.hero);
}
void Recipe(CanonicalBytes &b, const VfxBallisticRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_index,r.source_stable_id,r.payload_kind,r.importance,r.timing_kind,r.shape,r.min_quality,r.gradient_row,
        r.mesh_index,r.count,r.knot_count,r.source_type,r.motion); Array(b,r.knots);
    b.Fields(r.seconds,r.min_size,r.max_size,r.min_speed,r.max_speed,r.gravity,r.bounce,r.drag,r.speed,r.cone,r.min_spin,r.max_spin);
    Vec(b,r.color); b.Fields(r.hdr,r.persistent_status);
}
void Recipe(CanonicalBytes &b, const VfxDistortionRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_stable_id,r.payload_kind,r.importance,r.input_mode,r.timing_kind,r.min_quality,r.knot_count,r.kind,r.motion);
    Array(b,r.knots); b.Fields(r.seconds,r.radius,r.strength,r.alpha);
}
void Recipe(CanonicalBytes &b, const VfxDecalRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_stable_id,r.payload_kind,r.importance,r.input_mode,r.timing_kind,r.min_quality,r.knot_count,r.kind);
    Array(b,r.knots); b.Fields(r.seconds,r.hdr,r.alpha,r.edge_glow); Vec(b,r.linear_rgb);
    b.Fields(r.gradient_row,r.voronoi_cells,r.texture_slice_base,r.texture_min_quality);
}
void Recipe(CanonicalBytes &b, const VfxGroundRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_index,r.source_stable_id,r.output_index,r.gradient_row,r.payload_kind,r.timing_kind,r.min_quality,
        r.source_type,r.profile,r.motion,r.kind,r.knot_count); Array(b,r.knots); Range(b,r.pulse_curve_keys);
    b.Fields(r.pulse_curve_interpolation,r.tick_count,r.seconds,r.primary,r.secondary); Vec(b,r.rgb);
    b.Fields(r.hdr,r.alpha,r.motion_rate_hz,r.motion_amplitude,r.motion_inset_fraction,r.tertiary);
    Seq(b,r.mask_variants,[](CanonicalBytes &out,const VfxGroundRecipe::MaskVariant &m){ Range(out,m.slices); out.Fields(m.strength,m.valid,m.present); });
    b.Scalar(r.texture_contract_valid);
}
void Recipe(CanonicalBytes &b, const VfxFlashRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_index,r.source_stable_id,r.output_index,r.payload_kind,r.min_quality); Array(b,r.knots);
    b.Fields(r.seconds,r.coverage_quality); Spawn(b,r.spawn); Seq(b,r.mask_slice_ranges,[](CanonicalBytes &out,VfxRange v){Range(out,v);});
    Array(b,r.mask_strengths); b.Fields(r.supported_quality_mask,r.mask_present_mask,r.unique_effect_identity);
}
void Recipe(CanonicalBytes &b, const VfxPersistentStampRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_visual_kind,r.payload_kind,r.source_stable_id,r.timing_kind,r.fixed_seconds,r.source_end); Spawn(b,r.spawn);
    Seq(b,r.mask_slices,[](CanonicalBytes &out,VfxRange v){Range(out,v);}); Array(b,r.mask_strengths); b.Fields(r.supported_quality_mask,r.mask_present_mask,r.bleed_extend);
}
void Recipe(CanonicalBytes &b, const VfxPersistentFireRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_stable_id,r.rate,r.radius_scale); Spawn(b,r.spawn); Range(b,r.mask_slices); b.Scalar(r.mask_strength);
}
void Recipe(CanonicalBytes &b, const VfxOwnedStatusTickRecipe &r)
{
    b.Fields(r.tick_effect_id,r.status_effect_id,r.tick_effect_handle,r.status_effect_handle,r.min_quality,r.tick_seconds,r.delay_fraction,r.lifetime_fraction); Spawn(b,r.spawn);
    Array(b,r.mask_slices); b.Fields(r.mask_strength,r.bleed);
}
void Recipe(CanonicalBytes &b, const VfxMoteSourceRecipe &r)
{
    b.Fields(r.count,r.rate,r.curl,r.drag,r.speed,r.cone_degrees,r.common_params_valid,r.rate_valid,r.residual_valid,r.spark_valid);
}
void Recipe(CanonicalBytes &b, const VfxMoteOutputRecipe &r)
{
    b.Fields(r.min_quality,r.gradient_row); Seq(b,r.mask_slices,[](CanonicalBytes &out,VfxRange v){Range(out,v);}); Array(b,r.mask_strengths);
    b.Fields(r.persistent_curve_row,r.persistent_size,r.hdr); Vec(b,r.color); b.Fields(r.mask_present_mask,r.mask_quality_mask,r.output_color_valid,r.persistent_params_valid,r.spark_textures_valid);
}

template <typename T> void Definitions(CanonicalBytes &b,const T &r)
{
    b.Fields(r.effect_id,r.first_emitter,r.emitter_count,r.kind,r.reserved); Vec(b,r.line_color);
    b.Fields(r.line_width,r.line_lifetime,r.line_sprite,r.line_frame_columns,r.line_frame_rows,r.line_uv_repeat,r.line_scroll_speed,r.line_primitive);
}
void Emitter(CanonicalBytes &b,const CookedParticleEmitter &r)
{
    b.Fields(r.sprite,r.frame_columns,r.frame_rows,r.shape,r.velocity,r.facing,r.renderer,r.primitive); Vec(b,r.local_offset); Vec(b,r.shape_extent); Vec(b,r.local_direction); Vec(b,r.start_color); Vec(b,r.end_color);
    b.Fields(r.lifetime_min,r.lifetime_max,r.speed_min,r.speed_max,r.cone_degrees,r.start_size_min,r.start_size_max,r.end_size_min,r.end_size_max,r.gravity,
        r.rotation_min,r.rotation_max,r.angular_velocity_min,r.angular_velocity_max,r.stretch,r.count);
}
void Sprite(CanonicalBytes &b,const CookedParticleSprite &r) { b.Fields(r.sprite_id,r.index,r.frame_columns,r.frame_rows); }
void Effect(CanonicalBytes &b,const VfxEffectRecord &r)
{
    b.Fields(r.handle,r.input_mode,r.importance,r.role,r.style,r.readability,r.payload_kind,r.anchor,r.orientation,r.scale,r.binding_timing,r.geometry,r.must_match_logic,r.timing_kind,r.seconds); Range(b,r.sources);
}
void Source(CanonicalBytes &b,const VfxSourceRecord &r)
{
    b.Fields(r.effect,r.stable_id,r.type,r.knot_count); Array(b,r.knots); Range(b,r.parameters); Range(b,r.outputs); b.Scalar(r.authoritative);
}
void Output(CanonicalBytes &b,const VfxOutputRecord &r)
{
    b.Fields(r.source,r.profile,r.shape,r.shape_domain,r.shape_scale_rule,r.shape_component_kind); Array(b,r.rgba);
    b.Fields(r.hdr,r.gradient_row,r.min_quality,r.coverage_type,r.coverage_ref,r.surface_target); Range(b,r.parameters); Range(b,r.textures);
    b.Fields(r.motion,r.motion_rate_hz,r.motion_amplitude,r.motion_inset_fraction);
}
void Parameter(CanonicalBytes &b,const VfxParameterRecord &r) { b.Fields(r.key,r.type,r.bits); }
void Texture(CanonicalBytes &b,const VfxTextureBindingRecord &r)
{
    b.Fields(r.role,r.catalog_slot,r.selection,r.min_quality,r.strength); Range(b,r.slices); b.Fields(r.first_frame,r.frame_count,r.fps);
}
void Curve(CanonicalBytes &b,const VfxCurveRecord &r) { b.Fields(r.interpolation,r.domain); Range(b,r.keys); }
void CurveKey(CanonicalBytes &b,const VfxCurveKey &r) { b.Fields(r.time,r.value); }
void Lookup(CanonicalBytes &b,const VfxEffectLookupRecord &r) { b.Fields(r.effect_id,r.handle); }
void Resource(CanonicalBytes &b,const VfxTextureResourceRecord &r) { b.Scalar(r.slot); Range(b,r.asset_path_bytes); }
void Upgrade(CanonicalBytes &b,const VfxUpgradeBindingRecord &r) { b.Fields(r.skill_id,r.ordinal); Range(b,r.sequence); }
void Binding(CanonicalBytes &b,const ParticleSpriteBinding &r) { b.Fields(r.sprite,r.frame_columns,r.frame_rows); }
}

std::vector<std::byte> CanonicalVfxImageBytes(const VfxImage &image)
{
    CanonicalBytes b("project-hs-vfx-image",1);
    Seq(b,image.definitions,Definitions<CookedVfxDefinition>); Seq(b,image.emitters,Emitter); Seq(b,image.sprites,Sprite);
    Seq(b,image.authored_mesh_indices,[](CanonicalBytes &out,std::uint8_t x){out.Scalar(x);});
    Seq(b,image.decorative_definitions,[](CanonicalBytes &out,std::uint8_t x){out.Scalar(x);});
    const auto &p=image.program;
    Seq(b,p.effects,Effect); Seq(b,p.sources,Source); Seq(b,p.outputs,Output); Seq(b,p.parameters,Parameter); Seq(b,p.texture_bindings,Texture);
    Seq(b,p.slice_indices,[](CanonicalBytes &out,std::uint32_t x){out.Scalar(x);}); Seq(b,p.curves,Curve); Seq(b,p.curve_keys,CurveKey);
    Seq(b,p.effect_lookup,Lookup); Seq(b,p.texture_resources,Resource); b.Blob(p.strings); Seq(b,p.upgrade_bindings,Upgrade);
    Seq(b,p.upgrade_sequences,[](CanonicalBytes &out,std::uint32_t x){out.Scalar(x);});
    b.Fields(image.has_typed_wavefront,image.sprite_count,image.payload_hash);
    Seq(b,std::array{image.sprite_bindings.bleed_status_sprite,image.sprite_bindings.burn_status_sprite,image.sprite_bindings.slow_status_sprite,
        image.sprite_bindings.mark_status_sprite,image.sprite_bindings.trap_sprite,image.sprite_bindings.fire_area_sprite},Binding);
    b.Scalar(image.has_required_sprite_bindings);
    Seq(b,image.light_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.light_recipes,Recipe<VfxLightRecipe>);
    Seq(b,image.ballistic_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.ballistic_recipes,Recipe<VfxBallisticRecipe>);
    Seq(b,image.effect_output_profiles,[](CanonicalBytes &out,std::uint32_t x){out.Scalar(x);});
    Seq(b,image.distortion_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.distortion_recipes,Recipe<VfxDistortionRecipe>);
    Seq(b,image.decal_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.decal_recipes,Recipe<VfxDecalRecipe>);
    Seq(b,image.ground_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.ground_recipes,Recipe<VfxGroundRecipe>);
    Seq(b,image.flash_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.flash_recipes,Recipe<VfxFlashRecipe>);
    Seq(b,image.persistent_stamp_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.persistent_stamp_recipes,Recipe<VfxPersistentStampRecipe>);
    Seq(b,image.persistent_fire_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.persistent_fire_recipes,Recipe<VfxPersistentFireRecipe>);
    Seq(b,image.owned_status_tick_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Seq(b,image.owned_status_tick_recipes,Recipe<VfxOwnedStatusTickRecipe>);
    Seq(b,image.mote_source_recipes,Recipe<VfxMoteSourceRecipe>); Seq(b,image.mote_output_recipes,Recipe<VfxMoteOutputRecipe>);
    return std::move(b).Take();
}
}
