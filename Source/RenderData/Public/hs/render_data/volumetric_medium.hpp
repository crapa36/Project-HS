#pragma once
#include <hs/render_data/render_asset_format.hpp>
namespace hs::render_data
{
inline constexpr std::uint32_t kVolumetricMediumStride = 112;
struct VolumetricMedium
{
    std::array<float, 16> world_to_local{};
    std::array<float, 4> density_extinction{}, scattering{};
    std::uint64_t stable_id{};
    std::uint32_t generation{}, shape{}; //0global,1box[-1,1],2unit sphere.
};
void ValidateVolumetricMedia(std::span<const VolumetricMedium>);
[[nodiscard]] std::vector<std::byte> EncodeVolumetricMedia(std::span<const VolumetricMedium>);
[[nodiscard]] std::vector<VolumetricMedium> DecodeVolumetricMedia(std::span<const std::byte>);
}
