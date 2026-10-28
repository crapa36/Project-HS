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
    std::array<VfxScreenEffectRecipe, 1> screen_effects{};
    std::array<VfxScreenRecipe, 2> screen_recipes{};
    std::array<VfxRibbonEffectRecipe, 1> ribbon_effects{};
    std::array<VfxRange, 1> distortion_ranges{};
    std::array<VfxDistortionRecipe, 1> distortion_recipes{};
    std::array<VfxRange, 1> decal_ranges{};
    std::array<VfxDecalRecipe, 1> decal_recipes{};
    std::array<VfxVolumeEffectRecipe, 1> volume_effects{};
    std::array<VfxVolumeRecipe, 1> volume_recipes{};

    VfxImage image{};
    image.program.effects = effects;
    image.program.sources = sources;
    image.program.outputs = outputs;
    image.effect_output_profiles = output_profiles;
    image.light_effect_ranges = light_ranges;
    image.light_recipes = light_recipes;
    image.screen_effect_recipes = screen_effects;
    image.screen_recipes = screen_recipes;
    image.ribbon_effect_recipes = ribbon_effects;
    image.distortion_effect_ranges = distortion_ranges;
    image.distortion_recipes = distortion_recipes;
    image.decal_effect_ranges = decal_ranges;
    image.decal_recipes = decal_recipes;
    image.volume_effect_recipes = volume_effects;
    image.volume_recipes = volume_recipes;

    auto initialize = [&]
    {
        effects = {};
        sources = {};
        outputs = {};
        light_ranges = {};
        screen_effects = {};
        ribbon_effects = {};
        distortion_ranges = {};
        decal_ranges = {};
        volume_effects = {};
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
}
