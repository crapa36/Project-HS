#pragma once

#include <hs/runtime/vfx_image.hpp>

#include <string>

namespace hs::content
{
// Input is the checked VFX image; output belongs to the release link transaction.
[[nodiscard]] std::string EmitReleaseVfxCpp(const VfxImage &image);
}
