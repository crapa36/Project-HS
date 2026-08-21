#pragma once

#include <array>
#include <cstdint>

namespace hs
{

inline constexpr std::uint32_t kDdsMagic = 0x20534444;

struct DdsPixelFormat
{
    std::uint32_t size{32};
    std::uint32_t flags{0x41};
    std::uint32_t four_cc{};
    std::uint32_t rgb_bit_count{32};
    std::uint32_t red_mask{0x000000ff};
    std::uint32_t green_mask{0x0000ff00};
    std::uint32_t blue_mask{0x00ff0000};
    std::uint32_t alpha_mask{0xff000000};
};

static_assert(sizeof(DdsPixelFormat) == 32);

struct DdsHeader
{
    std::uint32_t size{124};
    std::uint32_t flags{0x100F};
    std::uint32_t height{1};
    std::uint32_t width{1};
    std::uint32_t pitch{4};
    std::uint32_t depth{};
    std::uint32_t mip_count{1};
    std::array<std::uint32_t, 11> reserved{};
    DdsPixelFormat pixel_format;
    std::uint32_t caps{0x1000};
    std::array<std::uint32_t, 4> remaining_caps{};
};

static_assert(sizeof(DdsHeader) == 124);

} // namespace hs
