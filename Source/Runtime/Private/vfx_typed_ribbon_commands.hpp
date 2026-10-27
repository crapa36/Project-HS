#pragma once
#include "vfx_event_view.hpp"

#include <hs/render_data/vfx_ribbon_input.hpp>
#include <hs/renderer/vfx_program.hpp>

namespace hs::runtime_detail
{
[[nodiscard]] std::vector<VfxRibbonSourceInput> BuildVfxTypedRibbonInputs(
    const VfxProgramView &cooked,
    std::span<const VfxPersistentInput> persistent);

[[nodiscard]] std::vector<VfxRibbonSourceInput> BuildVfxEventRibbonInputs(
    const VfxProgramView &cooked,
    VfxEventView events,
    double current_tick);
}
