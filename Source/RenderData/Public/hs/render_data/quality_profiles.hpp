#pragma once

#include <hs/render_data/render_asset_format.hpp>
#include <string_view>

namespace hs::render_data
{
inline constexpr std::uint32_t kQualityProfileStride = 176;
inline constexpr std::uint32_t kAutoFgCadenceStride = 12;
inline constexpr std::string_view kQualityProfilesProduct = "render/config/quality_profiles.rpkg";

// Rows are Low, Medium, High, Ultra. Values are requested limits; native
// preparation still checks resource admission and supported capacity.
struct QualityProfile
{
    float geometry_screen_error_px{}, foliage_density{};
    std::uint32_t texture_finest_mip{};
    float decorative_particle_fraction{};
    std::uint32_t shadow_cache_mib{}, shadow_filter_samples{}, shadow_resolution{};
    float ao_axis_ratio{};
    std::uint32_t ao_directions{}, ao_steps{}, contact_shadow_steps{}, ao_spatial_radius{};
    float ssr_axis_ratio{};
    std::uint32_t ssr_samples{}, ssr_max_steps{}, ssr_refine_steps{};
    float rt_axis_ratio{};
    std::uint32_t rt_diffuse_samples{}, rt_specular_samples{}, rt_spatial_radius{}, rt_diffuse_bounces{};
    std::uint32_t froxel_stride{}, froxel_slices{}, volumetric_area_samples{};
    std::uint32_t hero_refraction_layers{}, pom_max_steps{}, pom_refinement_steps{};
    std::uint32_t local_shadow_max_lights{}, local_shadow_resolution{}, local_shadow_area_samples{}, local_shadow_budget_mib{};
    std::uint32_t rtao_samples{}, ray_shadow_max_lights{}, ray_shadow_samples{}, ray_shadow_spatial_radius{};
    std::uint32_t lighting_cluster_capacity{}, gi_logical_cascades{}, gi_internal_resolution_percent{};
    float vfx_light_fraction{}, vfx_distortion_fraction{}, vfx_decal_fraction{};
    float terrain_detail_fade_start_m{}, terrain_detail_fade_end_m{};
    std::uint32_t subsurface_half_samples{};
};
struct AutoFgCadencePolicy
{
    float disable_below_fps{}, reenable_above_fps{};
    std::uint32_t minimum_samples{};
};
struct QualityProfiles
{
    std::array<QualityProfile, 4> profiles{};
    AutoFgCadencePolicy auto_fg_cadence{};
};
void ValidateQualityProfiles(const QualityProfiles &);
[[nodiscard]] std::vector<std::byte> EncodeQualityProfiles(const QualityProfiles &);
[[nodiscard]] QualityProfiles DecodeQualityProfiles(std::span<const std::byte>);
} // namespace hs::render_data
