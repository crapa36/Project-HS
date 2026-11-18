#pragma once

#include <cmath>
#include <cstdint>

namespace hs::render_data
{
inline std::uint32_t SeededNoiseHash(std::uint32_t value) noexcept
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

inline float SeededNoiseRandom(std::uint32_t value) noexcept
{
    return static_cast<float>(SeededNoiseHash(value) & 0xffffffu) / 16777216.0f;
}

inline float SeededNoiseValue(float x, float y, std::uint32_t seed) noexcept
{
    const float ix = std::floor(x);
    const float iy = std::floor(y);
    const float fx = x - ix;
    const float fy = y - iy;
    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const auto cell = [&](std::int32_t cell_x, std::int32_t cell_y) {
        const auto ux = static_cast<std::uint32_t>(cell_x);
        const auto uy = static_cast<std::uint32_t>(cell_y);
        return SeededNoiseHash(ux ^ SeededNoiseHash(uy) ^ seed);
    };
    const float x0y0 = SeededNoiseRandom(cell(static_cast<std::int32_t>(ix),
                                               static_cast<std::int32_t>(iy)));
    const float x1y0 = SeededNoiseRandom(cell(static_cast<std::int32_t>(ix + 1.0f),
                                               static_cast<std::int32_t>(iy)));
    const float x0y1 = SeededNoiseRandom(cell(static_cast<std::int32_t>(ix),
                                               static_cast<std::int32_t>(iy + 1.0f)));
    const float x1y1 = SeededNoiseRandom(cell(static_cast<std::int32_t>(ix + 1.0f),
                                               static_cast<std::int32_t>(iy + 1.0f)));
    const float x0 = x0y0 + (x1y0 - x0y0) * ux;
    const float x1 = x0y1 + (x1y1 - x0y1) * ux;
    return (x0 + (x1 - x0) * uy) * 2.0f - 1.0f;
}
} // namespace hs::render_data
