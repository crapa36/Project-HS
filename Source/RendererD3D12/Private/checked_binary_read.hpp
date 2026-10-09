#pragma once

#include <hs/core/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace hs
{

[[nodiscard]] inline Result ReadCheckedBinary(
    const std::filesystem::path &path, std::vector<std::byte> &bytes,
    std::optional<std::uint64_t> expected_bytes = std::nullopt)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Could not open binary: " + path.string());
    }

    const auto end = stream.tellg();
    if (end < std::streampos{0})
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Could not determine binary size: " + path.string());
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Binary is too large to read: " + path.string());
    }
    if (expected_bytes && size != *expected_bytes)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Binary size changed while reading: " + path.string());
    }

    std::vector<std::byte> temporary(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!stream)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Could not seek binary: " + path.string());
    }
    if (size != 0)
    {
        const auto requested = static_cast<std::streamsize>(size);
        stream.read(reinterpret_cast<char *>(temporary.data()), requested);
        if (!stream || stream.gcount() != requested)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Short binary read: " + path.string());
        }
    }

    bytes = std::move(temporary);
    return Result::Success();
}

} // namespace hs
