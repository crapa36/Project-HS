#pragma once
#include <hs/render_data/vfx_owned_mesh.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxOwnedMeshInput> BuildVfxTypedMeshCommands(
    const VfxProgramView &program, std::span<const VfxPersistentInput> inputs);
}
