#pragma once

#include <cstddef>
#include <vector>

namespace hs::material_content
{
// Returns the fixed 256x256, one-mip RGBA16F DX10 DDS used by coated IBL.
[[nodiscard]] std::vector<std::byte> CookCoatedDfgDds();
}
