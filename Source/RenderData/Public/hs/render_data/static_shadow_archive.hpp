#pragma once

#include <hs/render_data/render_snapshot.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hs::render_data
{
inline constexpr std::uint32_t kStaticShadowPageInner = 128;
inline constexpr std::uint32_t kStaticShadowPageGuard = 2;
inline constexpr std::uint32_t kStaticShadowPageTile = 132;
inline constexpr std::uint32_t kStaticShadowPageTexels = 132 * 132;
inline constexpr std::uint32_t kStaticShadowArchiveHeaderBytes = 256;

struct StaticShadowArchiveLevel
{
    float page_world_size{};
    std::int32_t minimum_x{}, minimum_y{};
    std::uint32_t width{}, height{}; // Both zero means this level has no product pages; page_world_size remains valid.
};
struct StaticShadowArchiveDescription
{
    Float3 light_right{}, light_up{}, light_forward{};
    float depth_minimum{}, depth_maximum{};
    std::array<StaticShadowArchiveLevel, 3> levels{};
    std::array<std::byte, 32> layout_digest{};
};
struct StaticShadowArchivePage
{
    std::uint64_t offset{}; // Zero for clear/missing tiles; payload order is independent of directory order.
    std::uint32_t bytes{}; // 0 = valid all-positive-zero tile; UINT32_MAX = missing/not baked.
    std::array<std::byte, 32> digest{}; // SHA-256 of encoded bytes; zero for clear/missing tiles.
};
struct StaticShadowArchiveIndex
{
    StaticShadowArchiveDescription description;
    std::uint64_t payload_bytes{};
    // Level order, then increasing absolute page Y and X. Each tile is top-down.
    std::vector<StaticShadowArchivePage> pages;
};

[[nodiscard]] std::vector<std::byte> BuildStaticShadowArchiveIndex(const StaticShadowArchiveIndex &);
[[nodiscard]] StaticShadowArchiveIndex ReadStaticShadowArchiveIndex(std::span<const std::byte>);
// Lossless four-byte words; no float quantization. Empty encoded input is clear.
// Nonempty pages contain a mode byte: 0 = little-endian R32, 1 = meshoptimizer vertex codec.
// The caller authenticates the index product and each encoded page's SHA-256 before decoding.
[[nodiscard]] std::vector<std::byte> EncodeStaticShadowPage(std::span<const float>);
void DecodeStaticShadowPage(std::span<const std::byte>, std::span<float>);
} // namespace hs::render_data
