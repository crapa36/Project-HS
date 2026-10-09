#pragma once

#include <hs/render_data/texture_package.hpp>

namespace hs::render_data
{
inline constexpr std::uint32_t kLtcLutVersion = 1;
inline constexpr std::uint32_t kLtcLutEdge = 64;
inline constexpr std::uint32_t kLtcLutMetadataStride = 64;
inline constexpr float kLtcNdotVEndpoint = 0.00001f;

struct LtcLutDescription
{
    std::uint32_t fit_samples{}; // Deterministic power-of-two budget, 256..4096.
    std::uint32_t fit_iterations{}; // Bounded Nelder-Mead iterations, 32..256.
};
struct LtcLutPackageView
{
    LtcLutDescription description;
    // RGBA32F, 64x64, one array slice/mip. x=perceptual roughness,
    // y=sqrt(1-NdotV), endpoints at texel centers, row-major y then x.
    // The NdotV=0 row is the positive kLtcNdotVEndpoint limit of model v1.
    // Matrix is inverse LTC B=[[x,0,y],[0,1,0],[z,0,w]]. det(B)>0.
    TexturePackageView matrix;
    // x=integral of unit-Fresnel GGX f*cos; y=integral with (1-VdotH)^5.
    // Both are dimensionless hemispherical energies, z/w reserved zero.
    TexturePackageView amplitude;
};

// LTC1 metadata and exactly three required little-endian chunks (29/30/31).
// Fits the shared alpha=max(roughness^2,0.002), uncorrelated Smith GGX model.
// The normalized LTC is a cosine/pi density over solid angle; amplitude restores
// reflected energy. No photometric units, exposure or light dimensions live here.
[[nodiscard]] std::vector<std::byte> BuildLtcLutPackage(
    AssetId asset_id, std::span<const std::string> dependencies,
    LtcLutDescription description, std::span<const Float4> matrix,
    std::span<const Float4> amplitude);
// Texture spans borrow package_bytes. Rejects unsupported fitting conventions,
// nonfinite/singular transforms, invalid energy, reserved fields and extra chunks.
[[nodiscard]] LtcLutPackageView ReadLtcLutPackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id);
} // namespace hs::render_data
