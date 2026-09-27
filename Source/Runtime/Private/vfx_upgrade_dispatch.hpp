#pragma once
#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/renderer/vfx_program_loader.hpp>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
// Resolve explicit activation stages against membership in the cooked binding.
// Replaces a generic resolve event; never emits an entire binding sequence.
Result DispatchVfxUpgradeEvents(const VfxProgramData &program,
                                std::span<const PresentationEvent> events,
                                std::vector<PresentationEvent> &output);
}
