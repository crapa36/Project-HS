#pragma once
#include <hs/core/types.hpp>
#include <array>

namespace hs::renderer_detail
{
// Fixed slot stride bounds GPU indexing. The global point budget is configurable;
// 64 samples support all supplied histories (maximum 0.24s at 60 Hz), with room
// for a clipped tail endpoint. Unsupported longer histories fail validation.
inline constexpr std::uint32_t kRibbonHistorySamples = 64;
inline constexpr std::uint32_t kRibbonHistoryStride = 32 + 32 * kRibbonHistorySamples;
inline constexpr std::uint32_t kRibbonArgumentStride = 20; // output root constant + D3D12_DRAW_ARGUMENTS
struct alignas(16) GpuRibbonSourceUpdate
{
    Float4 position_duration;
    Float4 previous_interpolation;
    // slot, tick low32, generation, flags: bit0 alive, bit1 reset, bit2 analytic, bits8..15 segment count
    std::array<std::uint32_t,4> identity{};
    Float4 rendered_head;
    Float4 control_age;
};
struct alignas(16) GpuRibbonOutput
{
    // slot, gradient row, motion (0 tapered,1 turbulent,2 dual,3 recoil,4 analytic link,5 analytic sheath), flags: bit0 OIT, bit1 dashed
    std::array<std::uint32_t,4> metadata{};
    Float4 widths; // head, tail, noise warp, phase offset
    Float4 color;
    Float4 material; // HDR, history seconds, detail strength, miter limit
    // xyz previous camera position; w stores the exact current tick bits (asuint in shader), never a float time.
    Float4 previous_eye;
    Float4 detail; // slice (-1 absent), UV repeat per metre, scroll speed, source alpha
    Float4 analytic; // owner normalized age, travel pulse enabled, analytic mode, reserved
};
static_assert(sizeof(GpuRibbonSourceUpdate)==80);
static_assert(sizeof(GpuRibbonOutput)==112);
}
