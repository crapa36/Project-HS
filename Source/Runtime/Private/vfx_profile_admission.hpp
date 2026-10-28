#pragma once

#include "vfx_session_state.hpp"
#include <hs/runtime/vfx_image.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace hs::runtime_detail
{
[[nodiscard]] inline bool ValidVfxAdmissionRange(VfxRange range, std::size_t size) noexcept
{
    return range.first <= size && range.count <= size - range.first;
}

[[nodiscard]] inline bool KnownVfxAdmissionSourceType(VfxSourceType type) noexcept
{
    switch (type)
    {
    case VfxSourceType::Direct:
    case VfxSourceType::ProjectileFollow:
    case VfxSourceType::BallisticCollision:
    case VfxSourceType::CurlMotes:
    case VfxSourceType::ImpactSprite:
    case VfxSourceType::HistoryRibbon:
    case VfxSourceType::VolumeLocal:
        return true;
    }
    return false;
}

[[nodiscard]] inline bool KnownVfxAdmissionOutputProfile(VfxOutputProfile profile) noexcept
{
    switch (profile)
    {
    case VfxOutputProfile::SpriteSdfAdd:
    case VfxOutputProfile::MeshEmissiveOit:
    case VfxOutputProfile::SpriteAdd:
    case VfxOutputProfile::RibbonAdd:
    case VfxOutputProfile::GroundSdfSoft:
    case VfxOutputProfile::GroundSdfAdd:
    case VfxOutputProfile::Distortion:
    case VfxOutputProfile::GroundSdfOit:
    case VfxOutputProfile::SpriteFlameOit:
    case VfxOutputProfile::Light:
    case VfxOutputProfile::RibbonOit:
    case VfxOutputProfile::LocalVolume:
    case VfxOutputProfile::Decal:
    case VfxOutputProfile::SpriteNoiseOit:
    case VfxOutputProfile::ScreenOverlay:
    case VfxOutputProfile::EntitySurface:
    case VfxOutputProfile::FresnelShellOit:
    case VfxOutputProfile::FresnelShellEmissive:
    case VfxOutputProfile::SpriteSmoke6WayOit:
        return true;
    }
    return false;
}

[[nodiscard]] inline int VfxAdmissionProfileBit(VfxOutputProfile profile) noexcept
{
    using Profile = ActiveEffectStore::Profile;
    switch (profile)
    {
    case VfxOutputProfile::RibbonAdd:
    case VfxOutputProfile::RibbonOit:
        return static_cast<int>(Profile::Ribbon);
    case VfxOutputProfile::Light:
        return static_cast<int>(Profile::Light);
    case VfxOutputProfile::Distortion:
        return static_cast<int>(Profile::Distortion);
    case VfxOutputProfile::Decal:
        return static_cast<int>(Profile::Decal);
    case VfxOutputProfile::LocalVolume:
        return static_cast<int>(Profile::Volume);
    case VfxOutputProfile::ScreenOverlay:
        return static_cast<int>(Profile::Screen);
    default:
        return -1;
    }
}

[[nodiscard]] inline bool HasPreparedVfxAdmissionProfile(const VfxImage &image,
    VfxEffectHandle handle, ActiveEffectStore::Profile profile) noexcept
{
    const auto h = static_cast<std::size_t>(handle);
    switch (profile)
    {
    case ActiveEffectStore::Profile::Ribbon:
        return h != 0 && h <= image.ribbon_effect_recipes.size() &&
            image.ribbon_effect_recipes[h - 1].handle == handle &&
            image.ribbon_effect_recipes[h - 1].valid_handle &&
            image.ribbon_effect_recipes[h - 1].sources.count != 0;
    case ActiveEffectStore::Profile::Light:
        return h != 0 && h <= image.light_effect_ranges.size() &&
            image.light_effect_ranges[h - 1].count != 0 &&
            ValidVfxAdmissionRange(image.light_effect_ranges[h - 1], image.light_recipes.size());
    case ActiveEffectStore::Profile::Distortion:
        return image.HasDistortionRecipes(handle);
    case ActiveEffectStore::Profile::Decal:
        return image.HasDecalRecipes(handle);
    case ActiveEffectStore::Profile::Volume:
        return h != 0 && h <= image.volume_effect_recipes.size() &&
            image.volume_effect_recipes[h - 1].route_valid &&
            image.volume_effect_recipes[h - 1].outputs.count != 0 &&
            ValidVfxAdmissionRange(image.volume_effect_recipes[h - 1].outputs, image.volume_recipes.size());
    case ActiveEffectStore::Profile::Screen:
        return h != 0 && h <= image.screen_effect_recipes.size() &&
            image.screen_effect_recipes[h - 1].route_valid &&
            image.screen_effect_recipes[h - 1].outputs.count != 0 &&
            ValidVfxAdmissionRange(image.screen_effect_recipes[h - 1].outputs, image.screen_recipes.size());
    default:
        return false;
    }
}

[[nodiscard]] inline std::uint16_t ClassifyCosmeticVfxEventProfiles(const VfxImage &image,
    VfxEffectHandle handle, std::uint16_t requested_mask, bool replace_ground_context_owner)
{
    using Profile = ActiveEffectStore::Profile;
    if (replace_ground_context_owner || handle == 0 || handle > image.program.effects.size())
        return 0;

    constexpr std::array candidates{
        Profile::Ribbon, Profile::Light, Profile::Distortion,
        Profile::Decal, Profile::Volume, Profile::Screen
    };
    std::uint16_t prepared_mask = 0;
    for (const auto candidate : candidates)
    {
        const auto bit_index = static_cast<unsigned>(candidate);
        if (bit_index >= 16 || (requested_mask & (std::uint16_t{1} << bit_index)) == 0 ||
            !HasPreparedVfxAdmissionProfile(image, handle, candidate)) continue;
        if (image.effect_output_profiles.size() < handle) continue;
        const auto authored_profiles = image.effect_output_profiles[handle - 1];
        const auto has_authored_profile = [&](VfxOutputProfile profile)
        {
            const auto output_bit = static_cast<unsigned>(profile);
            return output_bit < 32 && (authored_profiles & (1u << output_bit)) != 0;
        };
        if (candidate == Profile::Ribbon
            ? (!has_authored_profile(VfxOutputProfile::RibbonAdd) &&
                !has_authored_profile(VfxOutputProfile::RibbonOit))
            : !has_authored_profile(candidate == Profile::Light ? VfxOutputProfile::Light :
                candidate == Profile::Distortion ? VfxOutputProfile::Distortion :
                candidate == Profile::Decal ? VfxOutputProfile::Decal :
                candidate == Profile::Volume ? VfxOutputProfile::LocalVolume : VfxOutputProfile::ScreenOverlay))
            continue;
        prepared_mask = static_cast<std::uint16_t>(prepared_mask | (std::uint16_t{1} << bit_index));
    }
    if (prepared_mask == 0) return 0;

    const auto &effect = image.program.effects[handle - 1];
    if (effect.handle != handle || effect.sources.count == 0 ||
        !ValidVfxAdmissionRange(effect.sources, image.program.sources.size())) return 0;

    std::uint16_t found_mask = 0;
    std::uint16_t unsafe_mask = 0;
    for (std::size_t source_index = effect.sources.first;
         source_index < static_cast<std::size_t>(effect.sources.first) + effect.sources.count; ++source_index)
    {
        const auto &source = image.program.sources[source_index];
        if (!KnownVfxAdmissionSourceType(source.type) ||
            !ValidVfxAdmissionRange(source.outputs, image.program.outputs.size())) return 0;
        for (std::size_t output_index = source.outputs.first;
             output_index < static_cast<std::size_t>(source.outputs.first) + source.outputs.count; ++output_index)
        {
            const auto &output = image.program.outputs[output_index];
            if (!KnownVfxAdmissionOutputProfile(output.profile)) return 0;
            const auto profile_index = VfxAdmissionProfileBit(output.profile);
            if (profile_index < 0 || profile_index >= 16) continue;
            const auto profile_bit = static_cast<std::uint16_t>(1u << profile_index);
            if ((prepared_mask & profile_bit) == 0) continue;
            found_mask = static_cast<std::uint16_t>(found_mask | profile_bit);
            if (source.effect != handle || output.source != source_index || source.authoritative != 0 ||
                source.type == VfxSourceType::ProjectileFollow)
                unsafe_mask = static_cast<std::uint16_t>(unsafe_mask | profile_bit);
        }
    }
    return static_cast<std::uint16_t>(found_mask & prepared_mask & ~unsafe_mask);
}
} // namespace hs::runtime_detail
