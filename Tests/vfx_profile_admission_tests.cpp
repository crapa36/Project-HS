#include "../Source/Runtime/Private/vfx_profile_admission.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace
{
void Require(bool condition)
{
    if (!condition) throw std::runtime_error("VFX profile admission classification failed");
}

constexpr std::uint16_t Bit(hs::runtime_detail::ActiveEffectStore::Profile profile)
{
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(profile));
}
}

void TestVfxProfileAdmission()
{
    using namespace hs;
    using Profile = runtime_detail::ActiveEffectStore::Profile;

    std::array<VfxEffectRecord, 1> effects{};
    std::array<VfxSourceRecord, 2> sources{};
    std::array<VfxOutputRecord, 3> outputs{};
    std::array<std::uint32_t, 1> output_profiles{};
    std::array<VfxRange, 1> light_ranges{};
    std::array<VfxLightRecipe, 3> light_recipes{};
    std::array<VfxRange, 1> ground_ranges{};
    std::array<VfxGroundRecipe, 2> ground_recipes{};
    std::array<VfxScreenEffectRecipe, 1> screen_effects{};
    std::array<VfxScreenRecipe, 2> screen_recipes{};
    std::array<VfxRibbonEffectRecipe, 1> ribbon_effects{};
    std::array<VfxRange, 1> distortion_ranges{};
    std::array<VfxDistortionRecipe, 3> distortion_recipes{};
    std::array<VfxRange, 1> decal_ranges{};
    std::array<VfxDecalRecipe, 3> decal_recipes{};
    std::array<VfxVolumeEffectRecipe, 1> volume_effects{};
    std::array<VfxVolumeRecipe, 1> volume_recipes{};
    std::array<VfxMeshEffectRecipe, 1> mesh_effects{};
    std::array<VfxMeshRecipe, 3> mesh_recipes{};
    std::array<VfxFresnelEffectRecipe, 1> fresnel_effects{};
    std::array<VfxFresnelRecipe, 3> fresnel_recipes{};
    std::array<VfxEntitySurfaceEffectRecipe, 2> entity_surface_effects{};
    std::array<VfxEntitySurfaceRecipe, 4> entity_surface_recipes{};

    VfxImage image{};
    image.program.effects = effects;
    image.program.sources = sources;
    image.program.outputs = outputs;
    image.effect_output_profiles = output_profiles;
    image.light_effect_ranges = light_ranges;
    image.light_recipes = light_recipes;
    image.ground_effect_ranges = ground_ranges;
    image.ground_recipes = ground_recipes;
    image.screen_effect_recipes = screen_effects;
    image.screen_recipes = screen_recipes;
    image.ribbon_effect_recipes = ribbon_effects;
    image.distortion_effect_ranges = distortion_ranges;
    image.distortion_recipes = distortion_recipes;
    image.decal_effect_ranges = decal_ranges;
    image.decal_recipes = decal_recipes;
    image.volume_effect_recipes = volume_effects;
    image.volume_recipes = volume_recipes;
    image.mesh_effect_recipes = mesh_effects;
    image.mesh_recipes = mesh_recipes;
    image.fresnel_effect_recipes = fresnel_effects;
    image.fresnel_recipes = fresnel_recipes;
    image.entity_surface_effect_recipes = entity_surface_effects;
    image.entity_surface_recipes = entity_surface_recipes;

    auto initialize = [&]
    {
        effects = {};
        sources = {};
        outputs = {};
        light_ranges = {};
        light_recipes = {};
        ground_ranges = {};
        ground_recipes = {};
        screen_effects = {};
        ribbon_effects = {};
        distortion_ranges = {};
        distortion_recipes = {};
        decal_ranges = {};
        decal_recipes = {};
        volume_effects = {};
        mesh_effects = {};
        mesh_recipes = {};
        fresnel_effects = {};
        fresnel_recipes = {};
        entity_surface_effects = {};
        entity_surface_recipes = {};
        output_profiles = {};
        effects[0].handle = 1;
        effects[0].sources = {0, 2};
        sources[0].effect = 1;
        sources[0].type = VfxSourceType::Direct;
        sources[0].outputs = {0, 1};
        sources[1].effect = 1;
        sources[1].type = VfxSourceType::Direct;
        sources[1].outputs = {1, 1};
        outputs[0].source = 0;
        outputs[0].profile = VfxOutputProfile::ScreenOverlay;
        outputs[1].source = 1;
        outputs[1].profile = VfxOutputProfile::Light;
        outputs[2].source = 1;
        outputs[2].profile = VfxOutputProfile::Light;
        light_ranges[0] = {0, 1};
        screen_effects[0].route_valid = true;
        screen_effects[0].outputs = {0, 1};
        ribbon_effects[0].handle = 1;
        ribbon_effects[0].valid_handle = true;
        ribbon_effects[0].sources = {0, 1};
        distortion_ranges[0] = {0, 1};
        decal_ranges[0] = {0, 1};
        volume_effects[0].route_valid = true;
        volume_effects[0].outputs = {0, 1};
        output_profiles[0] = (1u << static_cast<unsigned>(VfxOutputProfile::ScreenOverlay)) |
            (1u << static_cast<unsigned>(VfxOutputProfile::Light));
    };

    auto setOutputProfile = [&](VfxOutputProfile profile)
    {
        outputs[0].profile = profile;
        output_profiles[0] |= 1u << static_cast<unsigned>(profile);
    };

    initialize();
    sources[1].authoritative = 1;
    auto mask = static_cast<std::uint16_t>(Bit(Profile::Screen) | Bit(Profile::Light));
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, mask, false) == Bit(Profile::Screen));

    // Every contributor to a profile must be safe; one authoritative contributor protects Light.
    initialize();
    sources[0].outputs = {0, 2};
    sources[1].outputs = {2, 1};
    outputs[1].source = 0;
    sources[1].authoritative = 1;
    light_ranges[0] = {0, 2};
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Light), false) == 0);

    initialize();
    sources[0].type = VfxSourceType::ProjectileFollow;
    outputs[0].profile = VfxOutputProfile::ScreenOverlay;
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == 0);
    initialize();
    sources[0].type = static_cast<VfxSourceType>(0xffffu);
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == 0);
    initialize();
    outputs[1].profile = static_cast<VfxOutputProfile>(0xffffu);
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == 0);

    // Missing or malformed ranges never turn a requested profile optional.
    initialize();
    effects[0].sources = {0, 3};
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, mask, false) == 0);
    initialize();
    sources[0].outputs = {3, 1};
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == 0);
    initialize();
    screen_effects[0].outputs = {2, 1};
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == 0);

    // Both ends of the authored ownership link must agree.
    initialize();
    sources[0].effect = 2;
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == 0);
    initialize();
    outputs[0].source = 1;
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == 0);

    initialize();
    const auto protected_mask = static_cast<std::uint16_t>(Bit(Profile::Ground) | Bit(Profile::EntitySurface) |
        Bit(Profile::PointRate) | Bit(Profile::Screen));
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, protected_mask, false) == Bit(Profile::Screen));
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), true) == 0);

    initialize();
    effects[0].must_match_logic = 1;
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == Bit(Profile::Screen));

    initialize();
    setOutputProfile(VfxOutputProfile::RibbonAdd);
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Ribbon), false) == Bit(Profile::Ribbon));
    initialize();
    setOutputProfile(VfxOutputProfile::RibbonOit);
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Ribbon), false) == Bit(Profile::Ribbon));
    initialize();
    setOutputProfile(VfxOutputProfile::Distortion);
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Distortion), false) == Bit(Profile::Distortion));
    initialize();
    setOutputProfile(VfxOutputProfile::Decal);
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Decal), false) == Bit(Profile::Decal));
    initialize();
    setOutputProfile(VfxOutputProfile::LocalVolume);
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Volume), false) == Bit(Profile::Volume));

    initialize();
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 0, Bit(Profile::Screen), false) == 0);
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 2, Bit(Profile::Screen), false) == 0);

    initialize();
    outputs[0].profile = VfxOutputProfile::SpriteAdd;
    Require(runtime_detail::ClassifyCosmeticVfxEventProfiles(image, 1, Bit(Profile::Screen), false) == 0);

    initialize();
    light_ranges[0] = {0, 3};
    ground_ranges[0] = {0, 2};
    ground_recipes[1].min_quality = 2;
    distortion_ranges[0] = {0, 3};
    distortion_recipes[1].input_mode = distortion_recipes[1].timing_kind = 1;
    distortion_recipes[1].min_quality = 2;
    distortion_recipes[2].timing_kind = 1;
    decal_ranges[0] = {0, 3};
    decal_recipes[1].input_mode = decal_recipes[1].timing_kind = 1;
    decal_recipes[1].min_quality = 2;
    decal_recipes[2].timing_kind = 1;
    auto plan = runtime_detail::PrepareVfxEventProfileAdmission(image, 1);
    Require(plan.frame_rows[static_cast<std::size_t>(Profile::Light)] == 3 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::Ground)] == 2 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::Ground)] == 2 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::Distortion)] == 1 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::Distortion)] == 1 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::Decal)] == 1 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::Decal)] == 1 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::Screen)] == 1 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::Screen)] == 1 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::Volume)] == 1 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::Volume)] == 1);
    sources[1].authoritative = 1;
    plan = runtime_detail::PrepareVfxEventProfileAdmission(image, 1);
    Require((plan.cosmetic_mask & Bit(Profile::Light)) == 0 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::Light)] == 3);
    decal_ranges[0] = {3, 1};
    ground_ranges[0] = {2, 1};
    distortion_ranges[0] = {3, 1};
    plan = runtime_detail::PrepareVfxEventProfileAdmission(image, 1);
    Require(plan.frame_rows[static_cast<std::size_t>(Profile::Decal)] == 0 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::Ground)] == 0 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::Ground)] == 0 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::Decal)] == 0 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::Distortion)] == 0 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::Distortion)] == 0);

    // Prepared owner recipes provide per-input maximums. Quality and age can
    // only remove rows; Mesh/Fresnel also match the effect handle and shell kind.
    initialize();
    mesh_effects[0] = {1, {0, 3}, true};
    mesh_recipes[0].effect_handle = 1;
    mesh_recipes[0].min_quality = 0;
    mesh_recipes[1].effect_handle = 1;
    mesh_recipes[1].min_quality = 2;
    mesh_recipes[2].effect_handle = 2;
    fresnel_effects[0] = {0, 1, VfxFresnelShellKind::BossCrown, {0, 3}, true};
    fresnel_recipes[0].effect_handle = 1;
    fresnel_recipes[0].kind = VfxFresnelShellKind::BossCrown;
    fresnel_recipes[0].min_quality = 0;
    fresnel_recipes[1].effect_handle = 1;
    fresnel_recipes[1].kind = VfxFresnelShellKind::BossCrown;
    fresnel_recipes[1].min_quality = 2;
    fresnel_recipes[2].effect_handle = 1;
    fresnel_recipes[2].kind = VfxFresnelShellKind::BossCrown;
    fresnel_recipes[2].min_quality = 3;
    entity_surface_effects[0].outputs = {0, 2};
    entity_surface_effects[1].outputs = {2, 2};
    entity_surface_recipes[0].effect_handle = 1;
    entity_surface_recipes[0].min_quality = 1;
    entity_surface_recipes[0].persistent = false;
    entity_surface_recipes[1].effect_handle = 1;
    entity_surface_recipes[1].min_quality = 2;
    entity_surface_recipes[1].persistent = true;
    entity_surface_recipes[2].effect_handle = 2;
    entity_surface_recipes[2].min_quality = 0;
    entity_surface_recipes[2].persistent = true;
    entity_surface_recipes[3].effect_handle = 2;
    entity_surface_recipes[3].min_quality = 3;
    entity_surface_recipes[3].persistent = true;
    // EntitySurface emission trusts the effect's output range and does not
    // recheck each row's handle, so the bound follows that same range contract.
    entity_surface_recipes[1].effect_handle = 99;
    plan = runtime_detail::PrepareVfxEventProfileAdmission(image, 1);
    Require(plan.persistent_mesh_rows == 2 && plan.persistent_fresnel_rows == 2 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::EntitySurface)] == 1 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::EntitySurface)] == 1);
    plan = runtime_detail::PrepareVfxEventProfileAdmission(image, 2);
    Require(plan.persistent_mesh_rows == 0 && plan.persistent_fresnel_rows == 0 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::EntitySurface)] == 0 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::EntitySurface)] == 1);
    entity_surface_effects[0].outputs = {4, 1};
    entity_surface_effects[1].outputs = {4, 1};
    mesh_effects[0].outputs = {3, 1};
    fresnel_effects[0].outputs = {3, 1};
    plan = runtime_detail::PrepareVfxEventProfileAdmission(image, 1);
    Require(plan.persistent_mesh_rows == 0 && plan.persistent_fresnel_rows == 0 &&
        plan.frame_rows[static_cast<std::size_t>(Profile::EntitySurface)] == 0 &&
        plan.persistent_frame_rows[static_cast<std::size_t>(Profile::EntitySurface)] == 0);
}
