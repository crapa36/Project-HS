#pragma once

#include <hs/render_data/texture_package.hpp>

#include <array>

namespace hs::render_data
{
inline constexpr std::uint32_t kLightingProbeVersion = 1;
inline constexpr std::uint32_t kLightingProbeMetadataStride = 64;
inline constexpr std::uint32_t kLightingProbeShCount = 9;

struct LightingProbeDescription
{
    std::uint32_t edge{}; // Power of two, 16..256. D3D face order +X,-X,+Y,-Y,+Z,-Z.
    float orientation_y_radians{}; // Local probe -> world, positive rotation around +Y.
};
using LightingProbeIrradiance = std::array<Float4, kLightingProbeShCount>;

struct LightingProbePackageView
{
    LightingProbeDescription description;
    TexturePackageView radiance; // RGBA32F, six faces, one mip, alpha=1.
    TexturePackageView prefiltered; // RGBA32F, six faces, full GGX mip chain.
    // Cosine-convolved real SH in local probe space. Evaluate yields irradiance E;
    // Lambertian reflected radiance is albedo * E / pi. w is reserved zero.
    // Basis order: 1,y,z,x,xy,yz,3z*z-1,xz,x*x-y*y (orthonormal constants applied).
    LightingProbeIrradiance irradiance{};
};

// Lighting probe v1 has exactly three required, portable little-endian chunks:
// LPB1 metadata64, raw+GGX RGBA32F radiance16, cosine SH9 float4. The cube payload
// is raw face-major first, then prefiltered face-major/mip-minor. Rec709 linear
// radiance has no exposure/display transfer; the sun is a separate direct light.
[[nodiscard]] std::vector<std::byte> BuildLightingProbePackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    LightingProbeDescription description, std::span<const Float4> radiance,
    std::span<const Float4> prefiltered, const LightingProbeIrradiance &irradiance);

// Texture spans borrow package_bytes. Keep the bytes alive while using the view.
// Rejects other render package kinds, unsupported conventions, nonfinite/negative
// radiance, nonunit alpha, invalid SH/reserved fields and any layout ambiguity.
[[nodiscard]] LightingProbePackageView ReadLightingProbePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id);
} // namespace hs::render_data
