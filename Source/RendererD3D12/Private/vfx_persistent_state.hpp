#pragma once

#include <hs/renderer/vfx_frame_input.hpp>
#include <span>
#include <string>
#include <vector>

namespace hs::renderer_detail
{
// Each update is a complete authoritative frame, not a list of changes.
class VfxPersistentState
{
public:
    [[nodiscard]] bool Update(std::span<const VfxPersistentInput> inputs, std::string &error);
    [[nodiscard]] std::span<const VfxPersistentInput> Active() const noexcept { return active_; }

private:
    std::vector<VfxPersistentInput> active_;
};
}
