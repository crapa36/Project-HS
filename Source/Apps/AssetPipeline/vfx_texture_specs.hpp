#pragma once

#include <hs/render_data/texture_package.hpp>
#include <string_view>

namespace hs::asset_pipeline
{
struct VfxTextureSpec
{
    std::string_view name;
    render_data::TextureDescription description;
    bool masks{};
    bool volume{};
};

// Zero height/array count is supplied by the cooked upstream DDS.
inline constexpr VfxTextureSpec kVfxTextureSpecs[] = {
    {"vfx_authored_mask_array", {256, 256, 12, 9, render_data::kTextureFormatR8}},
    {"vfx_curve_lut", {256, 7, 1, 1, render_data::kTextureFormatR16Float}},
    {"vfx_flow_curl_2d", {256, 256, 1, 9, render_data::kTextureFormatRg8Snorm}},
    {"vfx_fracture_decal_array", {512, 512, 4, 10, render_data::kTextureFormatRgba8}},
    {"vfx_gradient_lut", {256, 0, 1, 1, render_data::kTextureFormatRgba16Float}},
    {"vfx_noise_basis_2d", {256, 256, 1, 9, render_data::kTextureFormatRgba8}},
    {"vfx_noise_volume_64", {64, 64, 1, 1, render_data::kTextureFormatRgba8}, false, true},
    {"vfx_organic_decal_array", {512, 512, 8, 10, render_data::kTextureFormatR8}},
    {"vfx_ribbon_detail_array", {256, 64, 8, 9, render_data::kTextureFormatR8}},
    {"vfx_smoke6way_neg", {128, 128, 64, 8, render_data::kTextureFormatRgba8}},
    {"vfx_smoke6way_pos", {128, 128, 64, 8, render_data::kTextureFormatRgba8}},
    {"vfx_smoke_motion_vectors", {128, 128, 64, 8, render_data::kTextureFormatRg8Snorm}},
    {"vfx_stbn_scalar", {64, 64, 64, 1, render_data::kTextureFormatR8}},
    {"vfx_masks", {512, 512, 0, 10, render_data::kTextureFormatBc4}, true},
};
} // namespace hs::asset_pipeline
