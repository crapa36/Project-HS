#pragma once

#include <hs/core/result.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace hs
{
class ShippedContentOwner;
namespace shipping
{
struct ShippingRange
{
    std::uint64_t offset{}, size{};
    std::array<std::byte,32> digest{};
};
enum class ShippingTable : std::size_t { Simulation, Presentation, Audio, Renderer, Count };
// Emitted together with the immutable shipping image. The compiled header is
// trusted; startup matches it before admitting the bounded binary table sections.
struct ShippingImageInfo
{
    std::array<std::byte, 64> header;
    const wchar_t *relative_path;
    std::uint64_t content_hash;
    std::uint64_t font_offset{}, font_bytes{};
    std::array<std::byte,32> font_digest{};
    std::array<ShippingRange,2> vfx{};
    std::array<ShippingRange,static_cast<std::size_t>(ShippingTable::Count)> tables{};
};
[[nodiscard]] const ShippingImageInfo &ImageInfo() noexcept;
// Bounded startup read; failure preserves the caller buffer.
[[nodiscard]] Result ReadFont(const ShippedContentOwner &, const ShippingImageInfo &, std::vector<std::byte> &);
[[nodiscard]] Result ReadRange(const ShippedContentOwner &, const ShippingRange &, std::vector<std::byte> &);
// Failure leaves output attached to its previously opened file.
[[nodiscard]] Result OpenImage(const std::filesystem::path &directory,
                               const ShippingImageInfo &expected, ShippedContentOwner &output);
} // namespace shipping
} // namespace hs
