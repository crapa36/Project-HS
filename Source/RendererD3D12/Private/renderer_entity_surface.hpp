#pragma once

#include <hs/core/result.hpp>
#include <hs/render_data/vfx_entity_surface_input.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hs
{
// Sparse material payload appended to the existing split GPU scene buffer.
// Instance.reserved contains relative group byte offset + 1; zero has no effect.
// Relative offsets remain valid when the scene's slot capacity grows.
class EntitySurfaceFrame
{
  public:
    [[nodiscard]] Result Build(VfxEntitySurfaceInputs commands);
    [[nodiscard]] std::uint32_t InstancePayload(std::uint64_t render_id) const noexcept;
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept { return bytes_; }

  private:
    struct Binding { std::uint64_t render_id; std::uint32_t relative_offset_plus_one; };
    std::vector<Binding> bindings_;
    std::vector<std::byte> bytes_;
};
} // namespace hs
