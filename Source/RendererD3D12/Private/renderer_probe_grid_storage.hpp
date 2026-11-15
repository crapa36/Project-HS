#pragma once

#include <hs/render_data/scene_probe.hpp>

#include <array>
#include <span>

namespace hs::renderer_detail
{
inline constexpr std::uint32_t kProbeGridPackedCount = (render_data::kSceneProbeShCount * 3u + 3u) / 4u;

// Sampled GPU storage keeps all 27 RGB coefficients; wire SH9 retains its float4 rows.
[[nodiscard]] constexpr std::array<Float4, kProbeGridPackedCount> PackProbeGridSh(
    std::span<const Float4, render_data::kSceneProbeShCount> sh) noexcept
{
    return {{{sh[0].x, sh[0].y, sh[0].z, sh[1].x},
             {sh[1].y, sh[1].z, sh[2].x, sh[2].y},
             {sh[2].z, sh[3].x, sh[3].y, sh[3].z},
             {sh[4].x, sh[4].y, sh[4].z, sh[5].x},
             {sh[5].y, sh[5].z, sh[6].x, sh[6].y},
             {sh[6].z, sh[7].x, sh[7].y, sh[7].z},
             {sh[8].x, sh[8].y, sh[8].z, 0}}};
}
}
