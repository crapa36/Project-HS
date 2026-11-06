#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace hs
{
// Counterpart of CanonicalBytes: explicit LE scalars and bounded u64 lengths.
class CanonicalReader
{
  public:
    CanonicalReader(std::span<const std::byte> bytes, std::string_view domain,
                    std::uint32_t version) : bytes_(bytes)
    {
        if (String() != domain || Scalar<std::uint32_t>() != version)
            throw std::runtime_error("Unsupported content wire domain or version.");
    }

    template <typename T> T Scalar()
    {
        if constexpr (std::is_enum_v<T>)
            return static_cast<T>(Scalar<std::underlying_type_t<T>>());
        else if constexpr (std::is_same_v<T, float>)
            return std::bit_cast<float>(Scalar<std::uint32_t>());
        else if constexpr (std::is_same_v<T, double>)
            return std::bit_cast<double>(Scalar<std::uint64_t>());
        else
        {
            static_assert(std::is_integral_v<T> && sizeof(T) <= 8);
            constexpr std::size_t width = sizeof(T) <= 4 ? 4 : 8;
            if (width > Remaining()) throw std::runtime_error("Truncated content scalar.");
            std::uint64_t value{};
            for (std::size_t i = 0; i < width; ++i)
                value |= std::uint64_t(std::to_integer<unsigned>(bytes_[cursor_++])) << (i * 8);
            if constexpr (std::is_signed_v<T>)
            {
                const auto signed_value = width == 4
                    ? static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value)))
                    : std::bit_cast<std::int64_t>(value);
                if (signed_value < std::numeric_limits<T>::min() || signed_value > std::numeric_limits<T>::max())
                    throw std::runtime_error("Content signed scalar exceeds its field width.");
                return static_cast<T>(signed_value);
            }
            else
            {
                if (value > std::numeric_limits<T>::max())
                    throw std::runtime_error("Content scalar exceeds its field width.");
                return static_cast<T>(value);
            }
        }
    }

    template <typename... T> void Fields(T &...values) { ((values = Scalar<T>()), ...); }

    [[nodiscard]] std::size_t Count(std::size_t minimum_row_bytes = 1)
    {
        const auto count = Scalar<std::uint64_t>();
        if (!minimum_row_bytes || count > Remaining() / minimum_row_bytes)
            throw std::runtime_error("Content row count exceeds the remaining wire range.");
        return static_cast<std::size_t>(count);
    }

    [[nodiscard]] std::span<const std::byte> Blob()
    {
        const auto size = Count();
        const auto result = bytes_.subspan(cursor_, size);
        cursor_ += size;
        return result;
    }

    [[nodiscard]] std::string_view String()
    {
        const auto bytes = Blob();
        return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    }

    [[nodiscard]] std::size_t Remaining() const noexcept { return bytes_.size() - cursor_; }
    void Finish() const
    {
        if (Remaining()) throw std::runtime_error("Trailing bytes in content wire section.");
    }

  private:
    std::span<const std::byte> bytes_;
    std::size_t cursor_{};
};
}
