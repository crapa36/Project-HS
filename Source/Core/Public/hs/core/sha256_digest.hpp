#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <system_error>
#include <string_view>

namespace hs
{
[[nodiscard]] inline std::optional<std::array<std::byte, 32>> ParseSha256Digest(std::string_view text) noexcept
{
    if (!text.starts_with("sha256:") || text.size() != 71)
        return std::nullopt;
    std::array<std::byte, 32> digest{};
    for (std::size_t i = 0; i < digest.size(); ++i)
    {
        unsigned value{};
        const auto first = text.data() + 7 + i * 2;
        const auto [end, error] = std::from_chars(first, first + 2, value, 16);
        if (error != std::errc{} || end != first + 2)
            return std::nullopt;
        digest[i] = static_cast<std::byte>(value);
    }
    return digest;
}
} // namespace hs
