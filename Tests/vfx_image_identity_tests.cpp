#include <hs/runtime/vfx_image_io.hpp>
#include <hs/core/cooked_format.hpp>
#include "release_vfx_codegen.hpp"

#include <stdexcept>

namespace
{
void Check(bool value)
{
    if (!value) throw std::runtime_error("canonical VFX image identity regression");
}
}

void TestVfxImageIdentity()
{
    using namespace hs;
    VfxProgramData lookup_program;
    lookup_program.effect_lookup = {{9,1},{3,2},{3,4}};
    std::vector<VfxEffectLookupRecord> prepared_lookup;
    PrepareVfxEffectLookupById(lookup_program.View(), prepared_lookup);
    Check(prepared_lookup.size() == 3 && prepared_lookup[0].effect_id == 3 &&
        prepared_lookup[0].handle == 2 && prepared_lookup[1].effect_id == 3 &&
        prepared_lookup[1].handle == 4 && prepared_lookup[2].effect_id == 9);
    {
        VfxProgramData unsupported;
        unsupported.effects.resize(1);
        unsupported.effects[0].handle = 1;
        unsupported.effect_lookup = {{MakeAssetId("particle.player.hit_direction").value, 1}};
        std::vector<VfxScreenEffectRecipe> screen_effects;
        std::vector<VfxScreenRecipe> screens;
        const auto screen_result = PrepareVfxScreenRecipes(unsupported.View(), screen_effects, screens);
        Check(!screen_result && screen_result.Subsystem() == "hs_vfx_screen" &&
            screen_result.Message().find(std::to_string(unsupported.effect_lookup[0].effect_id)) != std::string_view::npos);
        unsupported.effect_lookup[0].effect_id = MakeAssetId("particle.boss.phase_change").value;
        std::vector<VfxVolumeEffectRecipe> volume_effects;
        std::vector<VfxVolumeRecipe> volumes;
        Check(!PrepareVfxVolumeRecipes(unsupported.View(), volume_effects, volumes));
        unsupported.effect_lookup[0].effect_id = MakeAssetId("unrelated.effect").value;
        Check(PrepareVfxScreenRecipes(unsupported.View(), screen_effects, screens).Succeeded() &&
            PrepareVfxVolumeRecipes(unsupported.View(), volume_effects, volumes).Succeeded());
    }
    VfxImageData data;
    data.definitions.push_back({});
    data.definitions[0].reserved = 1;
    data.definitions[0].line_width = -0.0f;
    data.payload_hash = 11;
    data.program.strings = {std::byte{0xc3}, std::byte{0xa9}};
    data.effect_output_profiles = {1};
    data.effect_lookup_by_id = {{0x11u, 2u}, {0x22u, 1u}};
    data.ground_effect_ranges = {{0,1}};
    VfxGroundRecipe ground{};
    ground.tertiary = .125f;
    ground.mask_variants[1] = {{3,2}, .5f, false, true};
    data.ground_recipes = {ground};
    data.persistent_fire_effect_ranges = {{0,1}};
    data.persistent_fire_recipes = {VfxPersistentFireRecipe{}};
    data.screen_effect_recipes = {{17, 1.0f, {0,1}, true}};
    data.screen_recipes = {{2,3,1,4,VfxScreenShape::DirectionalArc,.1f,.9f,{.2f,.3f,.4f},.5f,2,.6f,3}};
    data.volume_effect_recipes = {{18, 2.0f, {0,1}, true}};
    data.volume_recipes = {{5,6,7,2,8,VfxVolumeShape::BoundedCrown,VfxVolumeNoise::Fbm3D,
        .2f,.8f,1.2f,.7f,.1f,2.0f,18,1,{.4f,.5f,.6f}}};
    data.entity_surface_effect_recipes = {{19,2,3,1,0,1.5f,{0,1},true}};
    data.entity_surface_recipes = {{1,2,3,1,4,VfxEntitySurfaceShape::Transition,
        VfxEntitySurfaceDirection::UpAndOut,.08f,.88f,2.4f,.07f,{.2f,.3f,.4f,.72f},4.8f,.72f,false,true}};
    data.entity_surface_routes = {1,2,3};
    VfxRibbonEffectRecipe ribbon_effect{}; ribbon_effect.effect_id=21; ribbon_effect.handle=1;
    ribbon_effect.lookup_count=1; ribbon_effect.identity_count=1; ribbon_effect.sources={0,1};
    ribbon_effect.valid_handle=true; data.ribbon_effect_recipes={ribbon_effect};
    data.ribbon_slice_indices={1,4};
    data.ribbon_wake_effect_ranges={{0,1}};
    data.ribbon_wake_recipes={{1,0,1,2,17,{0,2},{.2f,.3f,.4f,.5f},.06f,.04f,2.5f,.35f}};
    VfxRibbonFixedRecipe ribbon_fixed{}; ribbon_fixed.effect_handle=1; ribbon_fixed.importance=2;
    ribbon_fixed.core_min_quality=0; ribbon_fixed.outer_min_quality=1; ribbon_fixed.segments=16;
    ribbon_fixed.kind=VfxRibbonFixedKind::CommonPull; ribbon_fixed.seconds=.45f;
    ribbon_fixed.taper=.55f; ribbon_fixed.outer_width_multiplier=2.8f;
    ribbon_fixed.core_output={VfxOutputProfile::RibbonAdd,VfxMotionKind::CurvedLinkTravelingHead,
        {.1f,.2f,.3f,.4f},.07f,.0385f,0,0,3,0,31,0xffffffffu,false,true};
    ribbon_fixed.outer_output={VfxOutputProfile::RibbonOit,VfxMotionKind::SoftWiderSheath,
        {.4f,.3f,.2f,.1f},.196f,.1078f,.08f,0,1.5f,0,32,0xffffffffu,false,false};
    data.ribbon_fixed_effect_ranges={{0,1}};
    data.ribbon_fixed_recipes={ribbon_fixed};
    data.ribbon_projectile_effect_ranges={{0,1}};
    data.ribbon_projectile_sources={{1,9,2,.25f,{0,1,0,0},{0,1}}};
    data.ribbon_projectile_outputs={{2,
        {VfxOutputProfile::RibbonOit,VfxMotionKind::ShortTurbulentWake,{.4f,.3f,.2f,.1f},
            .08f,.02f,.1f,.15f,2,0,12,0xffffffffu,true,false},
        {0,2},.35f,true}};
    data.ribbon_cancel_effect_ranges={{0,1}};
    data.ribbon_cancel_recipes={{1,2,9,1,.9f,.24f,.08f,.04f,{.2f,.3f,.4f,.7f},
        VfxOutputProfile::RibbonAdd,VfxMotionKind::ReverseFlowBow,2.5f,19}};
    data.ribbon_link_effect_recipes={{1,2,VfxRibbonLinkKind::PickupCollect,.9f,.1f,1.0f,0,12,{0,1}}};
    data.ribbon_link_output_recipes={{9,1,false,.04f,
        {VfxOutputProfile::RibbonAdd,VfxMotionKind::CurvesTowardCollector,{.2f,.3f,.4f,.7f},
            .04f,.04f,0,0,2,0,19,0xffffffffu,false,false},
        std::array{VfxRibbonLinkDetailRecipe{{0,2},.35f,true,true},
            VfxRibbonLinkDetailRecipe{{2,2},.4f,true,true},
            VfxRibbonLinkDetailRecipe{{4,2},.5f,true,true}}}};
    data.ribbon_event_effect_recipes={{22,1,3,4,.6f,{0,1}}};
    data.ribbon_event_sources={{1,9,4,true,{.1f,.2f,.8f,.9f},.25f,{35,45,55},{50,65,75},{0,1}}};
    data.ribbon_event_outputs={{2,true,true,75,
        {VfxOutputProfile::RibbonAdd,VfxMotionKind::ShortLineIncomingVelocity,{.2f,.3f,.4f,.7f},
            .08f,.02f,0,0,2,0,19,0xffffffffu,false,false},
        {0,2},.35f,true}};
    data.fresnel_effect_recipes={{MakeAssetId("persistent.boss.phase2_aura").value,1,
        VfxFresnelShellKind::BossCrown,{0,1},true}};
    data.fresnel_recipes={{1,0,9,0,1,VfxFresnelShellKind::BossCrown,.1f,.9f,8,0,0,.2f,0,
        {.2f,.3f,.4f,.5f},2,17,.1f,.2f,.3f}};
    data.mesh_effect_recipes={{1,{0,1},true}};
    data.mesh_recipes={{1,2,3,4,1,2,5,{.1f,.2f,.3f,.4f},3,.35f,.42f,0}};
    const auto generated = content::EmitReleaseVfxCpp(data.View());
    Check(generated.find("kScreenEffectRecipes") != std::string::npos &&
          generated.find("kScreenRecipes") != std::string::npos &&
          generated.find("kVolumeEffectRecipes") != std::string::npos &&
          generated.find("kVolumeRecipes") != std::string::npos &&
          generated.find("kEntitySurfaceEffectRecipes") != std::string::npos &&
          generated.find("kEntitySurfaceRecipes") != std::string::npos &&
          generated.find("kEffectLookupById") != std::string::npos &&
          generated.find("kRibbonWakeRecipes") != std::string::npos &&
          generated.find("kRibbonFixedRecipes") != std::string::npos &&
          generated.find("kRibbonProjectileSources") != std::string::npos &&
          generated.find("kRibbonCancelRecipes") != std::string::npos &&
          generated.find("kRibbonLinkOutputRecipes") != std::string::npos &&
          generated.find("kRibbonEventEffectRecipes") != std::string::npos &&
          generated.find("kRibbonEventSources") != std::string::npos &&
          generated.find("kRibbonEventOutputs") != std::string::npos &&
          generated.find("kFresnelRecipes") != std::string::npos);
    Check(generated.find("kMeshRecipes") != std::string::npos);

    const auto baseline = CanonicalVfxImageBytes(data.View());
    Check(baseline.size() > 31 && baseline[28] == std::byte{4} &&
        baseline[29] == std::byte{0} && baseline[30] == std::byte{0} && baseline[31] == std::byte{0});
    data.payload_hash = 999;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.payload_hash = 11;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.definitions[0].line_width = 0.0f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline); // preserve exact IEEE-754 bits.
    data.definitions[0].line_width = -0.0f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.definitions[0].reserved = 0;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.definitions[0].reserved = 1;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.persistent_fire_recipes[0].authoritative = true;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.persistent_fire_recipes[0].authoritative = false;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    const auto check_ground_field = [&](auto mutate) {
        data.ground_recipes[0] = ground;
        mutate(data.ground_recipes[0]);
        Check(CanonicalVfxImageBytes(data.View()) != baseline);
    };
    check_ground_field([](auto &r) { ++r.effect_id; });
    check_ground_field([](auto &r) { ++r.effect_handle; });
    check_ground_field([](auto &r) { ++r.source_index; });
    check_ground_field([](auto &r) { ++r.source_stable_id; });
    check_ground_field([](auto &r) { ++r.output_index; });
    check_ground_field([](auto &r) { ++r.gradient_row; });
    check_ground_field([](auto &r) { ++r.payload_kind; });
    check_ground_field([](auto &r) { ++r.timing_kind; });
    check_ground_field([](auto &r) { ++r.min_quality; });
    check_ground_field([](auto &r) { r.source_type = VfxSourceType::ProjectileFollow; });
    check_ground_field([](auto &r) { r.profile = VfxOutputProfile::GroundSdfAdd; });
    check_ground_field([](auto &r) { r.motion = VfxMotionKind::SlowCounterRotationStableFootprint; });
    check_ground_field([](auto &r) { r.kind = VfxGroundRecipeKind::Ticks; });
    check_ground_field([](auto &r) { ++r.knot_count; });
    check_ground_field([](auto &r) { r.knots[0] = -0.0f; });
    check_ground_field([](auto &r) { r.knots[1] = .25f; });
    check_ground_field([](auto &r) { r.knots[2] = .5f; });
    check_ground_field([](auto &r) { r.knots[3] = 1.0f; });
    check_ground_field([](auto &r) { ++r.pulse_curve_keys.first; });
    check_ground_field([](auto &r) { ++r.pulse_curve_keys.count; });
    check_ground_field([](auto &r) { ++r.pulse_curve_interpolation; });
    check_ground_field([](auto &r) { ++r.tick_count; });
    check_ground_field([](auto &r) { r.seconds = 1.0f; });
    check_ground_field([](auto &r) { r.primary = 1.0f; });
    check_ground_field([](auto &r) { r.secondary = 1.0f; });
    check_ground_field([](auto &r) { r.rgb.x = 1.0f; });
    check_ground_field([](auto &r) { r.rgb.y = 1.0f; });
    check_ground_field([](auto &r) { r.rgb.z = 1.0f; });
    check_ground_field([](auto &r) { r.hdr = 1.0f; });
    check_ground_field([](auto &r) { r.alpha = 1.0f; });
    check_ground_field([](auto &r) { r.motion_rate_hz = 1.0f; });
    check_ground_field([](auto &r) { r.motion_amplitude = 1.0f; });
    check_ground_field([](auto &r) { r.motion_inset_fraction = 1.0f; });
    check_ground_field([](auto &r) { r.tertiary = .25f; });
    check_ground_field([](auto &r) { ++r.mask_variants[1].slices.first; });
    check_ground_field([](auto &r) { ++r.mask_variants[1].slices.count; });
    check_ground_field([](auto &r) { r.mask_variants[1].strength = .75f; });
    check_ground_field([](auto &r) { r.mask_variants[1].valid = true; });
    check_ground_field([](auto &r) { r.mask_variants[1].present = false; });
    check_ground_field([](auto &r) { r.mask_variants[0].slices.first = 1; });
    check_ground_field([](auto &r) { r.mask_variants[0].slices.count = 1; });
    check_ground_field([](auto &r) { r.mask_variants[0].strength = .25f; });
    check_ground_field([](auto &r) { r.mask_variants[0].valid = false; });
    check_ground_field([](auto &r) { r.mask_variants[0].present = true; });
    check_ground_field([](auto &r) { r.mask_variants[2].slices.first = 1; });
    check_ground_field([](auto &r) { r.mask_variants[2].slices.count = 1; });
    check_ground_field([](auto &r) { r.mask_variants[2].strength = .25f; });
    check_ground_field([](auto &r) { r.mask_variants[2].valid = false; });
    check_ground_field([](auto &r) { r.mask_variants[2].present = true; });
    check_ground_field([](auto &r) { r.texture_contract_valid = true; });
    check_ground_field([](auto &r) { ++r.source_output_count; });
    check_ground_field([](auto &r) { ++r.effect_lookup_count; });
    check_ground_field([](auto &r) { r.source_parameters_empty = true; });
    check_ground_field([](auto &r) { r.event_admission_profiles = kVfxGroundEventDirectAdd; });
    data.ground_recipes[0] = ground;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.screen_recipes[0].center_clear += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.screen_recipes[0].center_clear -= .01f;
    data.screen_effect_recipes[0].route_valid = false;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.screen_effect_recipes[0].route_valid = true;
    data.volume_recipes[0].steps++;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.volume_recipes[0].steps--;
    data.volume_effect_recipes[0].outputs.first++;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.volume_effect_recipes[0].outputs.first--;
    data.entity_surface_routes.death_effect++;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.entity_surface_routes.death_effect--;
    data.ribbon_wake_recipes[0].detail_strength += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ribbon_wake_recipes[0].detail_strength -= .01f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);
    data.ribbon_fixed_recipes[0].outer_output.noise_warp += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ribbon_fixed_recipes[0].outer_output.noise_warp -= .01f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);
    data.ribbon_projectile_outputs[0].output.phase_offset += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ribbon_projectile_outputs[0].output.phase_offset -= .01f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);
    data.ribbon_cancel_recipes[0].history_seconds += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ribbon_cancel_recipes[0].history_seconds -= .01f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);
    data.ribbon_link_output_recipes[0].details[1].strength += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ribbon_link_output_recipes[0].details[1].strength -= .01f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);
    data.ribbon_event_effect_recipes[0].seconds += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ribbon_event_effect_recipes[0].seconds -= .01f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);
    data.ribbon_event_sources[0].axial_arc_by_quality[2] += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ribbon_event_sources[0].axial_arc_by_quality[2] -= .01f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);
    data.fresnel_recipes[0].rotation_hz += .01f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.fresnel_recipes[0].rotation_hz = .2f;
    data.mesh_recipes[0].shell_thickness = .3f;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.mesh_recipes[0].shell_thickness = 0.0f;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.ground_recipes[0].mask_variants[1].present = false;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ground_recipes[0].mask_variants[1].present = true;

    data.program.strings[1] = std::byte{0xa8};
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.program.strings[1] = std::byte{0xa9};
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.effect_output_profiles[0] = 2;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.effect_output_profiles[0] = 1;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.effect_lookup_by_id[0].handle++;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.effect_lookup_by_id[0].handle--;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.ground_effect_ranges[0].count = 0;
    Check(CanonicalVfxImageBytes(data.View()) != baseline);
    data.ground_effect_ranges[0].count = 1;
    Check(CanonicalVfxImageBytes(data.View()) == baseline);

    data.ground_recipes.push_back(ground);
    data.ground_recipes[1].tertiary = .25f;
    const auto ordered = CanonicalVfxImageBytes(data.View());
    std::swap(data.ground_recipes[0], data.ground_recipes[1]);
    Check(CanonicalVfxImageBytes(data.View()) != ordered); // authored row order is identity.
}
