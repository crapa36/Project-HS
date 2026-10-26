#pragma once
#include <hs/core/presentation_event.hpp>
#include <hs/core/result.hpp>
#include <hs/renderer/vfx_program.hpp>
#include <span>
#include <vector>

namespace hs::runtime_detail
{
// Resolve explicit activation stages against membership in the cooked binding.
// Replaces a generic resolve event; never emits an entire binding sequence.
// Optional reusable staging must not alias events or output; failure preserves output.
Result DispatchVfxUpgradeEvents(const VfxProgramView &program,
                                std::span<const PresentationEvent> events,
                                std::vector<PresentationEvent> &output,
                                std::vector<PresentationEvent> *staging = nullptr);
}
