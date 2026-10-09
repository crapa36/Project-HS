#pragma once
#include <hs/render_data/opacity_micromap_package.hpp>

namespace hs::render_data
{
struct OpacityMicromapAlphaProof
{
    // The producer proves the exact opacity graph is saturate(alpha*scale),
    // and this outward interval contains EVERY reachable decoded alpha tap
    // across all admitted mips/filter/address/streaming/gradient domains.
    // Missing decode equivalence, mutable words, unsupported graphs or sampler
    // domains leave proven=false. No assumed mip0/local UV footprint is used.
    bool proven{};
    float alpha_min{},alpha_max{1},scale{1},cutoff{0.5f};
};
struct OpacityMicromapBakeInput
{
    std::span<const std::array<float,2>> canonical_uvs;
    std::span<const std::uint32_t> canonical_indices,triangle_materials;
    std::span<const OpacityMicromapAlphaProof> materials;
    std::uint32_t subdivision_level{1}; // bounded first implementation:1..4
    std::uint64_t byte_budget{};        // explicit CPU input budget, not native AS size
};
// Whole sampling-domain proof is deliberately conservative. Each admitted
// triangle's cells share its proved state; unproved cells are native state3.
// Uniform blocks do not depend on microtriangle ordering. This does not claim
// a local footprint proof or speedup. All-unknown output is still native data.
[[nodiscard]] OpacityMicromapData BakeOpacityMicromaps(const OpacityMicromapBakeInput &);
}
