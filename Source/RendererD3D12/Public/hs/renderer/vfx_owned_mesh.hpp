#pragma once
#include <hs/renderer/vfx_frame_input.hpp>

namespace hs
{
struct VfxOwnedMeshInput
{
    std::uint64_t owner_id{};
    Float3 position{}, previous_position{}, velocity{};
    Float4 color{};
    float radius{}, hdr{}, fresnel{};
    std::uint32_t mesh_index{}, gradient_row{};
    // Zero preserves an older command without this authored parameter.
    float head_to_tail_ratio{};
    float shell_thickness{}; // Actual world-space metres.
};
}
