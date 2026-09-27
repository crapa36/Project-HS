#pragma once
#include <hs/renderer/vfx_owned_mesh.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxOwnedMeshInput> BuildVfxTypedMeshCommands(
    const VfxProgramData &program, std::span<const VfxPersistentInput> inputs);
}
