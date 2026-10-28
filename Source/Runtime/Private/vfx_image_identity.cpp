#include <hs/runtime/vfx_image_io.hpp>

#include <hs/core/canonical_bytes.hpp>

#include <type_traits>

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
void GroundValue(CanonicalBytes &b, std::uint64_t v) { b.Scalar(v); }
void GroundValue(CanonicalBytes &b, std::uint32_t v) { b.Scalar(v); }
void GroundValue(CanonicalBytes &b, float v) { b.Scalar(v); }
void GroundValue(CanonicalBytes &b, bool v) { b.Scalar(v); }
void GroundValue(CanonicalBytes &b, VfxSourceType v) { b.Scalar(v); }
void GroundValue(CanonicalBytes &b, VfxOutputProfile v) { b.Scalar(v); }
void GroundValue(CanonicalBytes &b, VfxMotionKind v) { b.Scalar(v); }
void GroundValue(CanonicalBytes &b, VfxGroundRecipeKind v) { b.Scalar(v); }
void GroundValue(CanonicalBytes &b, VfxRange v) { Range(b, v); }
void GroundValue(CanonicalBytes &b, Float3 v) { Vec(b, v); }
template <std::size_t N> void GroundValue(CanonicalBytes &b, const std::array<float, N> &v) { Array(b, v); }
void GroundMask(CanonicalBytes &b, const VfxGroundRecipe::MaskVariant &mask)
{
    VisitVfxGroundMaskVariantFields(mask, [&](const char *, const auto &value) { GroundValue(b, value); });
}
void GroundValue(CanonicalBytes &b, const std::array<VfxGroundRecipe::MaskVariant, 3> &masks)
{
    b.Sequence(masks, [](CanonicalBytes &out, const VfxGroundRecipe::MaskVariant &mask) { GroundMask(out, mask); });
}
template <typename T> void GroundValue(CanonicalBytes &, const T &)
{
    static_assert(!std::is_same_v<T, T>, "unsupported VfxGroundRecipe canonical field type");
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
    VisitVfxGroundRecipeFields(r, [&](const char *, const auto &value) { GroundValue(b, value); });
}
void Recipe(CanonicalBytes &b, const VfxFlashRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_index,r.source_stable_id,r.output_index,r.payload_kind,r.min_quality); Array(b,r.knots);
    b.Fields(r.seconds,r.coverage_quality); Spawn(b,r.spawn); b.Sequence(r.mask_slice_ranges,[](CanonicalBytes &out,VfxRange v){Range(out,v);});
    Array(b,r.mask_strengths); b.Fields(r.supported_quality_mask,r.mask_present_mask,r.unique_effect_identity);
}
void Recipe(CanonicalBytes &b, const VfxPersistentStampRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_visual_kind,r.payload_kind,r.source_stable_id,r.timing_kind,r.fixed_seconds,r.source_end); Spawn(b,r.spawn);
    b.Sequence(r.mask_slices,[](CanonicalBytes &out,VfxRange v){Range(out,v);}); Array(b,r.mask_strengths); b.Fields(r.supported_quality_mask,r.mask_present_mask,r.bleed_extend);
}
void Recipe(CanonicalBytes &b, const VfxPersistentFireRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.source_stable_id,r.rate,r.radius_scale); Spawn(b,r.spawn); Range(b,r.mask_slices); b.Fields(r.mask_strength,r.authoritative);
}
void Recipe(CanonicalBytes &b, const VfxOwnedStatusTickRecipe &r)
{
    b.Fields(r.tick_effect_id,r.status_effect_id,r.tick_effect_handle,r.status_effect_handle,r.min_quality,r.tick_seconds,r.delay_fraction,r.lifetime_fraction); Spawn(b,r.spawn);
    Array(b,r.mask_slices); b.Fields(r.mask_strength,r.bleed);
}
void Recipe(CanonicalBytes &b, const VfxMoteSourceRecipe &r)
{
    b.Fields(r.count,r.rate,r.curl,r.drag,r.speed,r.cone_degrees,r.common_params_valid,r.rate_valid,r.residual_valid,r.spark_valid);
    b.Fields(r.effect_id,r.effect_handle,r.effect_lookup_count,r.stable_id); Range(b,r.output_range); Array(b,r.knots);
    b.Fields(r.output_range_valid,r.common_route_valid,r.residual_route_valid,r.rate_route_valid,r.tick_route_valid,r.spark_route_valid);
}
void Recipe(CanonicalBytes &b, const VfxMoteOutputRecipe &r)
{
    b.Fields(r.min_quality,r.gradient_row); b.Sequence(r.mask_slices,[](CanonicalBytes &out,VfxRange v){Range(out,v);}); Array(b,r.mask_strengths);
    b.Fields(r.persistent_curve_row,r.persistent_size,r.hdr); Vec(b,r.color); b.Fields(r.mask_present_mask,r.mask_quality_mask,r.output_color_valid,r.persistent_params_valid,r.spark_textures_valid);
    b.Fields(r.common_recipe_index,r.empty_parameters_valid,r.residual_route_valid,r.rate_route_valid,r.tick_route_valid,r.persistent_route_valid,r.spark_route_valid);
}
void Recipe(CanonicalBytes &b, const VfxScreenEffectRecipe &r)
{
    b.Fields(r.effect_id,r.seconds); Range(b,r.outputs); b.Scalar(r.route_valid);
}
void Recipe(CanonicalBytes &b, const VfxScreenRecipe &r)
{
    b.Fields(r.source_index,r.output_index,r.min_quality,r.gradient_row,r.shape,r.knot_start,r.knot_end);
    Vec(b,r.rgb); b.Fields(r.alpha,r.hdr,r.center_clear,r.edge_power);
}
void Recipe(CanonicalBytes &b, const VfxVolumeEffectRecipe &r)
{
    b.Fields(r.effect_id,r.seconds); Range(b,r.outputs); b.Scalar(r.route_valid);
}
void Recipe(CanonicalBytes &b, const VfxVolumeRecipe &r)
{
    b.Fields(r.source_index,r.output_index,r.source_id,r.min_quality,r.gradient_row,r.shape,r.noise,
        r.knot_start,r.knot_end,r.density,r.alpha,r.anisotropy,r.hdr,r.steps,r.half_resolution);
    Vec(b,r.rgb);
}
void Recipe(CanonicalBytes &b, const VfxEntitySurfaceEffectRecipe &r)
{
    b.Fields(r.effect_id,r.importance,r.payload_kind,r.input_mode,r.timing_kind,r.seconds);
    Range(b,r.outputs); b.Scalar(r.identity_unique);
}
void Recipe(CanonicalBytes &b, const VfxEntitySurfaceRecipe &r)
{
    b.Fields(r.effect_handle,r.source_index,r.output_index,r.min_quality,r.gradient_row,r.shape,r.direction,
        r.knot_start,r.knot_end,r.noise_scale,r.edge_width);
    Vec(b,r.color); b.Fields(r.hdr,r.alpha,r.persistent,r.death);
}
void Recipe(CanonicalBytes &b,const VfxRibbonEffectRecipe &r)
{
    b.Fields(r.effect_id,r.handle,r.importance,r.payload_kind,r.input_mode,r.timing_kind,r.lookup_count,
        r.identity_count,r.seconds); Range(b,r.sources); b.Scalar(r.valid_handle);
}
void Recipe(CanonicalBytes &b,const VfxRibbonWakeRecipe &r)
{
    b.Fields(r.effect_handle,r.importance,r.min_quality,r.detail_min_quality,r.gradient_row);
    Range(b,r.detail_slices); Vec(b,r.color);
    b.Fields(r.width_head,r.width_tail,r.hdr,r.detail_strength);
}
void RibbonOutput(CanonicalBytes &b,const VfxRibbonOutputInput &r)
{
    b.Fields(r.profile,r.motion); Vec(b,r.color);
    b.Fields(r.width_head,r.width_tail,r.noise_warp,r.phase_offset,r.hdr,r.detail_strength,
        r.gradient_row,r.detail_slice,r.dashed,r.travel_pulse);
}
void Recipe(CanonicalBytes &b,const VfxRibbonFixedRecipe &r)
{
    b.Fields(r.effect_handle,r.importance,r.core_min_quality,r.outer_min_quality,r.segments,r.kind,
        r.seconds,r.taper,r.outer_width_multiplier);
    RibbonOutput(b,r.core_output); RibbonOutput(b,r.outer_output);
}
void Recipe(CanonicalBytes &b,const VfxRibbonProjectileSourceRecipe &r)
{
    b.Fields(r.effect_handle,r.source_id,r.knot_count,r.history_seconds);
    Array(b,r.knots); Range(b,r.outputs);
}
void Recipe(CanonicalBytes &b,const VfxRibbonProjectileOutputRecipe &r)
{
    b.Scalar(r.min_quality); RibbonOutput(b,r.output); Range(b,r.detail_slices);
    b.Fields(r.detail_strength,r.detail_present);
}
void Recipe(CanonicalBytes &b,const VfxRibbonCancelRecipe &r)
{
    b.Fields(r.effect_handle,r.importance,r.source_id,r.min_quality,r.seconds,r.history_seconds,
        r.width_head,r.width_tail); Vec(b,r.color); b.Fields(r.profile,r.motion,r.hdr,r.gradient_row);
}
void Recipe(CanonicalBytes &b,const VfxRibbonLinkDetailRecipe &r)
{
    Range(b,r.slices); b.Fields(r.strength,r.valid,r.present);
}
void Recipe(CanonicalBytes &b,const VfxRibbonLinkOutputRecipe &r)
{
    b.Fields(r.source_id,r.min_quality,r.core,r.width_multiplier); RibbonOutput(b,r.output);
    b.Sequence(r.details,[](CanonicalBytes &out,const VfxRibbonLinkDetailRecipe &detail) { Recipe(out,detail); });
}
void Recipe(CanonicalBytes &b,const VfxRibbonLinkEffectRecipe &r)
{
    b.Fields(r.effect_handle,r.importance,r.kind,r.seconds,r.window_start,r.window_end,
        r.taper,r.segments); Range(b,r.outputs);
}
void Recipe(CanonicalBytes &b,const VfxRibbonEventSourceRecipe &r)
{
    b.Fields(r.effect_handle,r.source_id,r.knot_count,r.axial_source); Array(b,r.knots);
    b.Scalar(r.history_seconds); Array(b,r.recoil_arc_by_quality); Array(b,r.axial_arc_by_quality);
    Range(b,r.outputs);
}
void Recipe(CanonicalBytes &b,const VfxRibbonEventEffectRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.importance,r.payload_kind,r.seconds); Range(b,r.sources);
}
void Recipe(CanonicalBytes &b,const VfxRibbonEventOutputRecipe &r)
{
    b.Fields(r.min_quality,r.axial,r.arc_present,r.arc_degrees); RibbonOutput(b,r.output);
    Range(b,r.detail_slices); b.Fields(r.detail_strength,r.detail_present);
}
void Recipe(CanonicalBytes &b,const VfxFresnelEffectRecipe &r)
{
    b.Fields(r.effect_id,r.effect_handle,r.kind); Range(b,r.outputs); b.Scalar(r.route_valid);
}
void Recipe(CanonicalBytes &b,const VfxFresnelRecipe &r)
{
    b.Fields(r.effect_handle,r.source_index,r.source_stable_id,r.output_index,r.min_quality,r.kind,
        r.knot_start,r.knot_end,r.sector_count,r.end_crack,r.fresnel_power,r.rotation_hz,r.noise_amount);
    Vec(b,r.color); b.Fields(r.hdr,r.gradient_row,r.motion_rate_hz,r.motion_amplitude,r.motion_inset_fraction);
}
void Recipe(CanonicalBytes &b,const VfxMeshEffectRecipe &r)
{
    b.Fields(r.effect_handle); Range(b,r.outputs); b.Scalar(r.route_valid);
}
void Recipe(CanonicalBytes &b,const VfxMeshRecipe &r)
{
    b.Fields(r.effect_handle,r.source_index,r.output_index,r.source_stable_id,r.mesh_index,r.min_quality,
        r.gradient_row); Vec(b,r.color); b.Fields(r.hdr,r.fresnel,r.head_to_tail_ratio,r.shell_thickness);
}

template <typename T> void Recipes(CanonicalBytes &b, const T &rows)
{
    b.Sequence(rows, [](CanonicalBytes &out, const auto &row) { Recipe(out, row); });
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
    // v4 includes the admitted Ribbon route tables in this source batch.
    // The cooked wire pack version and ActionKey identity remain independent.
    CanonicalBytes b("project-hs-vfx-image",4);
    b.Sequence(image.definitions,Definitions<CookedVfxDefinition>); b.Sequence(image.emitters,Emitter); b.Sequence(image.sprites,Sprite);
    b.Sequence(image.authored_mesh_indices,[](CanonicalBytes &out,std::uint8_t x){out.Scalar(x);});
    b.Sequence(image.decorative_definitions,[](CanonicalBytes &out,std::uint8_t x){out.Scalar(x);});
    const auto &p=image.program;
    b.Sequence(p.effects,Effect); b.Sequence(p.sources,Source); b.Sequence(p.outputs,Output); b.Sequence(p.parameters,Parameter); b.Sequence(p.texture_bindings,Texture);
    b.Sequence(p.slice_indices,[](CanonicalBytes &out,std::uint32_t x){out.Scalar(x);}); b.Sequence(p.curves,Curve); b.Sequence(p.curve_keys,CurveKey);
    b.Sequence(p.effect_lookup,Lookup); b.Sequence(p.texture_resources,Resource); b.Blob(p.strings); b.Sequence(p.upgrade_bindings,Upgrade);
    b.Sequence(p.upgrade_sequences,[](CanonicalBytes &out,std::uint32_t x){out.Scalar(x);});
    b.Fields(image.has_typed_wavefront,image.sprite_count,image.payload_hash);
    b.Sequence(std::array{image.sprite_bindings.bleed_status_sprite,image.sprite_bindings.burn_status_sprite,image.sprite_bindings.slow_status_sprite,
        image.sprite_bindings.mark_status_sprite,image.sprite_bindings.trap_sprite,image.sprite_bindings.fire_area_sprite},Binding);
    b.Scalar(image.has_required_sprite_bindings);
    b.Sequence(image.light_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.light_recipes);
    b.Sequence(image.ballistic_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.ballistic_recipes);
    b.Sequence(image.effect_output_profiles,[](CanonicalBytes &out,std::uint32_t x){out.Scalar(x);});
    b.Sequence(image.distortion_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.distortion_recipes);
    b.Sequence(image.decal_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.decal_recipes);
    b.Sequence(image.ground_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.ground_recipes);
    b.Sequence(image.flash_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.flash_recipes);
    b.Sequence(image.persistent_stamp_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.persistent_stamp_recipes);
    b.Sequence(image.persistent_fire_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.persistent_fire_recipes);
    b.Sequence(image.owned_status_tick_effect_ranges,[](CanonicalBytes &out,VfxRange x){Range(out,x);}); Recipes(b,image.owned_status_tick_recipes);
    Recipes(b,image.mote_source_recipes); Recipes(b,image.mote_output_recipes);
    Recipes(b,image.screen_effect_recipes); Recipes(b,image.screen_recipes);
    Recipes(b,image.volume_effect_recipes); Recipes(b,image.volume_recipes);
    Recipes(b,image.entity_surface_effect_recipes); Recipes(b,image.entity_surface_recipes);
    b.Fields(image.entity_surface_routes.phase_effect,image.entity_surface_routes.death_effect,
        image.entity_surface_routes.trap_point_rate_effect);
    Recipes(b,image.ribbon_effect_recipes);
    b.Sequence(image.ribbon_slice_indices,[](CanonicalBytes &out,std::uint32_t slice){out.Scalar(slice);});
    b.Sequence(image.ribbon_wake_effect_ranges,[](CanonicalBytes &out,VfxRange range){Range(out,range);});
    Recipes(b,image.ribbon_wake_recipes);
    b.Sequence(image.ribbon_fixed_effect_ranges,[](CanonicalBytes &out,VfxRange range){Range(out,range);});
    Recipes(b,image.ribbon_fixed_recipes);
    b.Sequence(image.ribbon_projectile_effect_ranges,[](CanonicalBytes &out,VfxRange range){Range(out,range);});
    Recipes(b,image.ribbon_projectile_sources); Recipes(b,image.ribbon_projectile_outputs);
    b.Sequence(image.ribbon_cancel_effect_ranges,[](CanonicalBytes &out,VfxRange range){Range(out,range);});
    Recipes(b,image.ribbon_cancel_recipes);
    Recipes(b,image.ribbon_link_effect_recipes); Recipes(b,image.ribbon_link_output_recipes);
    Recipes(b,image.ribbon_event_effect_recipes); Recipes(b,image.ribbon_event_sources);
    Recipes(b,image.ribbon_event_outputs);
    b.Sequence(image.effect_lookup_by_id,[](CanonicalBytes &out,const VfxEffectLookupRecord &row){
        out.Fields(row.effect_id,row.handle);
    });
    Recipes(b,image.fresnel_effect_recipes); Recipes(b,image.fresnel_recipes);
    Recipes(b,image.mesh_effect_recipes); Recipes(b,image.mesh_recipes);
    return std::move(b).Take();
}
}
