#pragma once
#include <hs/renderer/vfx_frame_input.hpp>
namespace hs
{
enum class VfxDecalKind : std::uint32_t { Fracture, Organic };
struct VfxDecalInput
{
 std::uint64_t stable_id{};
 Float3 position{};
 float radius{};
 Float3 direction{0,0,1}, linear_rgb{};
 float hdr{}, alpha{}, normalized_age{};
 std::uint32_t gradient_row{}, texture_slice{};
 VfxDecalKind kind{};
 VfxQuality quality{VfxQuality::High};
 std::uint32_t importance{}, stable_seed{}, voronoi_cells{};
 float edge_glow{};
};
}
