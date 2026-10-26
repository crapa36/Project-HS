#pragma once
#include <hs/core/types.hpp>
#include <cstdint>
#include <span>
#include <type_traits>

namespace hs
{
enum class VfxScreenShape : std::uint32_t { Vignette, DirectionalArc };
// Cooked color, opacity and gradient remain separate until GPU composition.
// Source direction points toward the incoming attack's origin in world space.
struct VfxScreenInput
{
    std::uint64_t event_sequence{}, owner_id{};
    std::uint32_t output_index{}, gradient_row{};
    VfxScreenShape shape{};
    Float4 color{};
    Float3 source_direction{};
    float hdr{}, alpha{}, gradient_age{}, envelope{1.0f};
    float center_clear{0.62f}, edge_power{2.8f};
};
static_assert(std::is_trivially_copyable_v<VfxScreenInput>);
using VfxScreenInputs = std::span<const VfxScreenInput>;
}
