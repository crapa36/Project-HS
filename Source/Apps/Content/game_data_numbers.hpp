#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace hs::content::detail
{

inline float CheckedFloat(double value)
{
    if (!std::isfinite(value))
        throw std::invalid_argument("number must be finite");
    if (value > static_cast<double>(std::numeric_limits<float>::max()) ||
        value < static_cast<double>(std::numeric_limits<float>::lowest()))
        throw std::out_of_range("number exceeds finite float range");
    // Generic coefficients may round to zero; producer speeds use CheckedPositiveFloat.
    return static_cast<float>(value);
}

inline float CheckedPositiveFloat(double value)
{
    const auto converted = CheckedFloat(value);
    if (!(converted > 0.0f))
        throw std::invalid_argument("value must remain positive as a float");
    return converted;
}

// shortcut: necessary axis check only; complete direction/homing admission before deriving a lifetime bound.
inline bool MakesFiniteProjectileRangeProgress(float speed, float max_leg_range) noexcept
{
    if (!(max_leg_range > 0.0f))
        return true;

    constexpr float tick_seconds = 1.0f / 60.0f;
    const auto speed_squared = speed * speed;
    if (!std::isfinite(speed_squared))
        return false;

    const auto step = speed * tick_seconds;
    const auto step_length = std::sqrt(step * step);
    return step_length > 0.0f &&
           max_leg_range - step_length < max_leg_range;
}

inline float CheckedSpawnRate(double value)
{
    const auto rate = CheckedPositiveFloat(value);
    // Previous residual is below 60; every spawn subtraction must make progress.
    const auto peak = CheckedFloat(static_cast<double>(rate) + 60.0);
    if (!(peak - 60.0f < peak))
        throw std::out_of_range("spawn rate prevents accumulator progress");
    return rate;
}

inline std::uint64_t CheckedPositiveTickInterval(std::uint64_t ticks)
{
    if (ticks == 0)
        throw std::invalid_argument("recurring interval must round to at least one tick");
    return ticks;
}

} // namespace hs::content::detail
