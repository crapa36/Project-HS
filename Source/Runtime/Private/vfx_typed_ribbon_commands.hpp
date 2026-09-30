#pragma once

#include <hs/renderer/vfx_ribbon_input.hpp>
#include <hs/renderer/vfx_program_loader.hpp>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxRibbonSourceInput> BuildVfxTypedRibbonInputs(
    const VfxProgramData &cooked,
    std::span<const VfxPersistentInput> persistent);

[[nodiscard]] std::vector<VfxRibbonSourceInput> BuildVfxEventRibbonInputs(
    const VfxProgramData &cooked,
    std::span<const VfxEventInput> events,
    Tick current_tick);
}
