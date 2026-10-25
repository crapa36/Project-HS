#pragma once

#include <hs/renderer/vfx_ribbon_input.hpp>
#include <hs/renderer/vfx_program.hpp>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxRibbonSourceInput> BuildVfxTypedRibbonInputs(
    const VfxProgramView &cooked,
    std::span<const VfxPersistentInput> persistent);

[[nodiscard]] std::vector<VfxRibbonSourceInput> BuildVfxEventRibbonInputs(
    const VfxProgramView &cooked,
    std::span<const VfxEventInput> events,
    Tick current_tick);
}
