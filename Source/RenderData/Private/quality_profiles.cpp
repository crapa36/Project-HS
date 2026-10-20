#include <hs/render_data/quality_profiles.hpp>
#include <hs/core/cooked_format.hpp>

#include <bit>
#include <cmath>
#include <stdexcept>

namespace hs::render_data
{
namespace
{
void Require(bool value)
{ if (!value) throw std::runtime_error("Invalid quality profiles package."); }
void Word(std::vector<std::byte> &bytes, std::uint32_t word)
{ for (unsigned i = 0; i != 4; ++i) bytes.push_back(std::byte((word >> (i * 8)) & 255)); }
std::uint32_t Word(std::span<const std::byte> bytes, std::size_t &offset)
{
    Require(offset + 4 <= bytes.size()); std::uint32_t word{};
    for (unsigned i = 0; i != 4; ++i) word |= std::uint32_t(bytes[offset++]) << (i * 8);
    return word;
}
bool Ratio(float value) { return std::isfinite(value) && value > 0 && value <= 1; }
// Portable positive binary16 round-to-nearest-even used to validate the packed
// shader lane without introducing DirectX into the CPU package module.
std::uint32_t PositiveHalf(float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const int exponent = int((bits >> 23) & 255) - 127;
    if (exponent < -25) return 0;
    const auto mantissa = (bits & 0x7fffffu) | (exponent < -14 ? 0x800000u : 0u);
    const auto shift = exponent < -14 ? static_cast<unsigned>(-exponent - 1) : 13u;
    auto rounded = mantissa >> shift;
    const auto remainder = mantissa & ((1u << shift) - 1u), halfway = 1u << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (rounded & 1u))) ++rounded;
    return exponent < -14 ? rounded : (static_cast<std::uint32_t>(exponent + 15) << 10) + rounded;
}
}
void ValidateQualityProfiles(const QualityProfiles &table)
{
    for (const auto &p : table.profiles)
    {
        Require(std::isfinite(p.geometry_screen_error_px) && p.geometry_screen_error_px > 0 && p.geometry_screen_error_px <= 64);
        Require(Ratio(p.foliage_density) && Ratio(p.decorative_particle_fraction));
        Require(p.texture_finest_mip <= 16 && p.shadow_cache_mib >= 1 && p.shadow_cache_mib <= 4096 &&
            p.shadow_filter_samples >= 1 && p.shadow_filter_samples <= 16 &&
            (p.shadow_resolution == 1024 || p.shadow_resolution == 2048));
        Require(Ratio(p.ao_axis_ratio) && p.ao_directions >= 1 && p.ao_directions <= 16 &&
            p.ao_steps >= 1 && p.ao_steps <= 32 && p.contact_shadow_steps >= 1 && p.contact_shadow_steps <= 64 &&
            p.ao_spatial_radius >= 1 && p.ao_spatial_radius <= 8);
        Require(Ratio(p.ssr_axis_ratio) && p.ssr_samples >= 1 && p.ssr_samples <= 8 &&
            p.ssr_max_steps >= 1 && p.ssr_max_steps <= 128 && p.ssr_refine_steps >= 1 && p.ssr_refine_steps <= 8);
        Require(p.froxel_stride >= 4 && p.froxel_stride <= 32 && p.froxel_slices >= 8 && p.froxel_slices <= 128 &&
            p.volumetric_area_samples >= 1 && p.volumetric_area_samples <= 16 && p.hero_refraction_layers <= 4 &&
            p.pom_max_steps >= 1 && p.pom_max_steps <= 64 && p.pom_refinement_steps >= 1 && p.pom_refinement_steps <= 8);
        Require(p.local_shadow_max_lights >= 1 && p.local_shadow_max_lights <= 64 &&
            p.local_shadow_resolution >= 64 && p.local_shadow_resolution <= 1024 &&
            (p.local_shadow_area_samples == 1 || p.local_shadow_area_samples == 2 || p.local_shadow_area_samples == 4) &&
            p.local_shadow_budget_mib >= 1 && p.local_shadow_budget_mib <= 4096);
        Require(p.lighting_cluster_capacity >= 1 && p.lighting_cluster_capacity <= 128);
        Require(p.gi_logical_cascades >= 1 && p.gi_logical_cascades <= 8 && (p.gi_internal_resolution_percent == 25 ||
            p.gi_internal_resolution_percent == 50 || p.gi_internal_resolution_percent == 75 || p.gi_internal_resolution_percent == 100));
        for (float f : {p.vfx_light_fraction, p.vfx_distortion_fraction, p.vfx_decal_fraction}) Require(std::isfinite(f) && f >= 0 && f <= 1);
        Require(std::isfinite(p.terrain_detail_fade_start_m) && std::isfinite(p.terrain_detail_fade_end_m) &&
            p.terrain_detail_fade_start_m > 0 && p.terrain_detail_fade_end_m > p.terrain_detail_fade_start_m && p.terrain_detail_fade_end_m <= 65504 && PositiveHalf(p.terrain_detail_fade_start_m) > 0 &&
            PositiveHalf(p.terrain_detail_fade_start_m) < PositiveHalf(p.terrain_detail_fade_end_m));
        Require(p.subsurface_half_samples >= 1 && p.subsurface_half_samples <= 16);
    }
    const auto &c = table.auto_fg_cadence;
    Require(std::isfinite(c.disable_below_fps) && std::isfinite(c.reenable_above_fps) &&
        c.disable_below_fps > 0 && c.reenable_above_fps > c.disable_below_fps && c.reenable_above_fps <= 1000 &&
        c.minimum_samples >= 1 && c.minimum_samples <= 239);
}
std::vector<std::byte> EncodeQualityProfiles(const QualityProfiles &table)
{
    ValidateQualityProfiles(table); std::vector<std::byte> rows, cadence;
    for (const auto &p : table.profiles)
    {
#define F(name) Word(rows, std::bit_cast<std::uint32_t>(p.name))
#define U(name) Word(rows, p.name)
        F(geometry_screen_error_px); F(foliage_density); U(texture_finest_mip); F(decorative_particle_fraction);
        U(shadow_cache_mib); U(shadow_filter_samples); U(shadow_resolution); F(ao_axis_ratio);
        U(ao_directions); U(ao_steps); U(contact_shadow_steps); U(ao_spatial_radius); F(ssr_axis_ratio);
        U(ssr_samples); U(ssr_max_steps); U(ssr_refine_steps);
        U(froxel_stride); U(froxel_slices);
        U(volumetric_area_samples); U(hero_refraction_layers); U(pom_max_steps); U(pom_refinement_steps);
        U(local_shadow_max_lights); U(local_shadow_resolution); U(local_shadow_area_samples); U(local_shadow_budget_mib); U(lighting_cluster_capacity); U(gi_logical_cascades); U(gi_internal_resolution_percent); F(vfx_light_fraction); F(vfx_distortion_fraction); F(vfx_decal_fraction); F(terrain_detail_fade_start_m); F(terrain_detail_fade_end_m); U(subsurface_half_samples);
#undef F
#undef U
    }
    Word(cadence, std::bit_cast<std::uint32_t>(table.auto_fg_cadence.disable_below_fps));
    Word(cadence, std::bit_cast<std::uint32_t>(table.auto_fg_cadence.reenable_above_fps));
    Word(cadence, table.auto_fg_cadence.minimum_samples);
    const std::array chunks{
        ChunkSource{static_cast<std::uint32_t>(ChunkType::QualityProfiles), 2, true, rows, 4, kQualityProfileStride},
        ChunkSource{static_cast<std::uint32_t>(ChunkType::AutoFgCadence), 1, true, cadence, 1, kAutoFgCadenceStride}};
    return BuildPackage(MakeAssetId(kQualityProfilesProduct), {}, chunks);
}
QualityProfiles DecodeQualityProfiles(std::span<const std::byte> bytes)
{
    const auto package = ReadPackage(bytes, MakeAssetId(kQualityProfilesProduct));
    Require(package.dependencies.empty() && package.chunks.size() == 2);
    const ChunkView *rows{}, *cadence{};
    for (const auto &chunk : package.chunks)
    {
        if (chunk.type == ChunkType::QualityProfiles) rows = &chunk;
        if (chunk.type == ChunkType::AutoFgCadence) cadence = &chunk;
    }
    Require(rows && cadence && rows->version == 2 && cadence->version == 1 && rows->required && cadence->required && rows->count == 4 && cadence->count == 1);
    QualityProfiles table; std::size_t offset{};
    for (auto &p : table.profiles)
    {
#define F(name) p.name = std::bit_cast<float>(Word(rows->bytes, offset))
#define U(name) p.name = Word(rows->bytes, offset)
        F(geometry_screen_error_px); F(foliage_density); U(texture_finest_mip); F(decorative_particle_fraction);
        U(shadow_cache_mib); U(shadow_filter_samples); U(shadow_resolution); F(ao_axis_ratio);
        U(ao_directions); U(ao_steps); U(contact_shadow_steps); U(ao_spatial_radius); F(ssr_axis_ratio);
        U(ssr_samples); U(ssr_max_steps); U(ssr_refine_steps);
        U(froxel_stride); U(froxel_slices);
        U(volumetric_area_samples); U(hero_refraction_layers); U(pom_max_steps); U(pom_refinement_steps);
        U(local_shadow_max_lights); U(local_shadow_resolution); U(local_shadow_area_samples); U(local_shadow_budget_mib); U(lighting_cluster_capacity); U(gi_logical_cascades); U(gi_internal_resolution_percent); F(vfx_light_fraction); F(vfx_distortion_fraction); F(vfx_decal_fraction); F(terrain_detail_fade_start_m); F(terrain_detail_fade_end_m); U(subsurface_half_samples);
#undef F
#undef U
    }
    offset = 0;
    table.auto_fg_cadence.disable_below_fps = std::bit_cast<float>(Word(cadence->bytes, offset));
    table.auto_fg_cadence.reenable_above_fps = std::bit_cast<float>(Word(cadence->bytes, offset));
    table.auto_fg_cadence.minimum_samples = Word(cadence->bytes, offset);
    ValidateQualityProfiles(table); return table;
}
} // namespace hs::render_data
