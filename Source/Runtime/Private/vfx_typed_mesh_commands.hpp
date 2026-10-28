#pragma once
#include <hs/render_data/vfx_owned_mesh.hpp>
#include <hs/runtime/vfx_image.hpp>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxOwnedMeshInput> BuildVfxTypedMeshCommands(
    const VfxImage &image, std::span<const VfxPersistentInput> inputs);
void AppendVfxTypedMeshCommands(const VfxImage &image,
    std::span<const VfxPersistentInput> inputs, std::vector<VfxOwnedMeshInput> &result);
}
