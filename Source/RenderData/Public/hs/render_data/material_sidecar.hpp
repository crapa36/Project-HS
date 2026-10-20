#pragma once

#include <array>
#include <cstdint>
#include <limits>

namespace hs::render_data
{
// Packed Surface sidecar families, in bit/offset order: Tangent, Coat, Sheen,
// ThinTransmission, Emission. Each plane holds up to four uint32 words.
struct MaterialSidecarLayout
{
    std::uint32_t mask{};
    std::uint32_t word_count{};
    std::uint32_t plane_count{};
    std::array<std::uint32_t, 5> offsets{
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max()};
};

// Family word counts are {2, 1, 2, 2, 1}; only selected families occupy words.
// Rejects unknown bits. No graphics API types or runtime dependencies are used.
[[nodiscard]] MaterialSidecarLayout ComputeMaterialSidecarLayout(std::uint32_t mask);
} // namespace hs::render_data
