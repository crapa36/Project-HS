#pragma once
#include <hs/render_data/vfx_decal_input.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <span>
#include <vector>
namespace hs::runtime_detail
{
std::vector<VfxDecalInput> BuildVfxTypedDecalCommands(const VfxProgramView &program,
 std::span<const VfxEventInput> events, std::span<const VfxPersistentInput> persistent, double current_tick);
}
