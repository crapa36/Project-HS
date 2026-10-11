#pragma once

#include <hs/core/result.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace hs::renderer_detail
{
inline std::array<bool, 3> RibbonBufferReplacements(
    const std::array<std::uint64_t, 3> &widths, std::uint64_t history_bytes, std::uint64_t argument_bytes)
{
    const bool history = widths[0] != history_bytes;
    return {history, history || widths[1] != history_bytes, widths[2] < argument_bytes};
}

template <typename Resource, typename Allocate>
Result ReplaceRibbonBuffers(const std::array<Resource *, 3> &resources,
    const std::array<bool, 3> &replace, std::uint64_t history_bytes, std::uint64_t argument_bytes,
    Allocate allocate)
{
    std::array<Resource, 3> candidates;
    for (std::size_t index = 0; index < resources.size(); ++index)
        if (replace[index])
            if (auto result = allocate(candidates[index], index == 2 ? argument_bytes : history_bytes); !result)
                return result;
    for (std::size_t index = 0; index < resources.size(); ++index)
        if (replace[index]) *resources[index] = std::move(candidates[index]);
    return Result::Success();
}
}
