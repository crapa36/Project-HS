#pragma once

#include <cmath>
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

inline float CheckedSpawnRate(double value)
{
    const auto rate = CheckedPositiveFloat(value);
    // Previous residual is below 60; every spawn subtraction must make progress.
    const auto peak = CheckedFloat(static_cast<double>(rate) + 60.0);
    if (!(peak - 60.0f < peak))
        throw std::out_of_range("spawn rate prevents accumulator progress");
    return rate;
}

} // namespace hs::content::detail
