#pragma once

#include <hs/core/render_view_input.hpp>
#include <hs/core/types.hpp>

#include <cstdint>

namespace hs
{

// Frame identity keeps simulation, render, presentation, and provider cadence
// distinct. The provider token is opaque to Core; known says whether value was
// supplied by the provider for this frame.
struct FrameIdentity
{
    Tick actual_tick{};
    std::uint64_t render_frame_id{};
    ViewId view_id{};
    std::uint64_t presentation_id{};
    std::uint64_t provider_token{};
    bool provider_token_known{};
};

} // namespace hs
