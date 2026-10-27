#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace hs
{
// Metadata identity encoding: little-endian scalars, u64 lengths, no object padding.
class CanonicalBytes
{
  public:
    CanonicalBytes(std::string_view domain, std::uint32_t version)
    {
        String(domain);
        Scalar(version);
    }

    template <typename T> void Scalar(T value)
    {
        if constexpr (std::is_enum_v<T>)
            Scalar(static_cast<std::underlying_type_t<T>>(value));
        else if constexpr (std::is_same_v<T, float>)
            Scalar(std::bit_cast<std::uint32_t>(value));
        else if constexpr (std::is_same_v<T, double>)
            Scalar(std::bit_cast<std::uint64_t>(value));
        else
        {
            static_assert(std::is_integral_v<T>);
            static_assert(sizeof(T) <= 8);
            if constexpr (sizeof(T) <= 4)
                Integer(static_cast<std::uint32_t>(value));
            else
                Integer(static_cast<std::uint64_t>(value));
        }
    }

    template <typename... T> void Fields(T... values) { (Scalar(values), ...); }

    void Blob(std::span<const std::byte> bytes)
    {
        Scalar(static_cast<std::uint64_t>(bytes.size()));
        if (!bytes.empty()) bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    void String(std::string_view value)
    {
        Blob(std::as_bytes(std::span(value.data(), value.size())));
    }

    template <typename Range, typename Encode> void Sequence(const Range &values, Encode encode)
    {
        Scalar(static_cast<std::uint64_t>(values.size()));
        for (const auto &value : values) encode(*this, value);
    }

    [[nodiscard]] std::span<const std::byte> View() const noexcept { return bytes_; }
    [[nodiscard]] std::vector<std::byte> Take() && { return std::move(bytes_); }

  private:
    template <typename T> void Integer(T value)
    {
        for (std::size_t byte = 0; byte < sizeof(T); ++byte)
            bytes_.push_back(std::byte((value >> (byte * 8u)) & 0xffu));
    }
    std::vector<std::byte> bytes_;
};
}
