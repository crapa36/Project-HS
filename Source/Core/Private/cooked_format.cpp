#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>

namespace hs
{

std::uint64_t Fnv1a64(std::span<const std::byte> bytes) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : bytes)
    {
        hash ^= std::to_integer<std::uint8_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t Fnv1a64(std::string_view text) noexcept
{
    return Fnv1a64(std::as_bytes(std::span(text.data(), text.size())));
}

std::uint32_t Crc32(std::span<const std::byte> bytes) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes)
    {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (std::uint32_t bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

Result NormalizeAssetPath(std::string_view source, std::string &normalized)
{
    normalized.clear();
    std::vector<std::string> segments;
    std::string segment;
    const auto flush_segment = [&]() -> bool {
        if (segment.empty() || segment == ".")
        {
            segment.clear();
            return true;
        }
        if (segment == "..")
        {
            segment.clear();
            if (segments.empty())
            {
                return false;
            }
            segments.pop_back();
            return true;
        }
        segments.push_back(std::move(segment));
        segment.clear();
        return true;
    };

    for (const auto character : source)
    {
        const auto value = character == '\\' ? '/' : character;
        if (value == '/')
        {
            if (!flush_segment())
            {
                return Result::Failure(ErrorCode::InvalidArgument, "hs_core",
                                       "Asset path escapes its content root.");
            }
            continue;
        }
        const auto lower = static_cast<char>(
            std::tolower(static_cast<unsigned char>(value)));
        if (!(std::isalnum(static_cast<unsigned char>(lower)) || lower == '_' ||
              lower == '-' || lower == '.'))
        {
            return Result::Failure(ErrorCode::InvalidArgument, "hs_core",
                                   "Asset path contains an unsupported character.");
        }
        segment.push_back(lower);
    }
    if (!flush_segment())
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_core",
                               "Asset path escapes its content root.");
    }
    for (const auto &part : segments)
    {
        if (!normalized.empty())
        {
            normalized.push_back('/');
        }
        normalized += part;
    }
    if (normalized.empty())
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_core",
                               "Asset path is empty.");
    }
    return Result::Success();
}

AssetId MakeAssetId(std::string_view normalized) noexcept
{
    return {Fnv1a64(normalized)};
}

Result ReadCookedHeader(const std::filesystem::path &path, CookedHeader &header,
                        std::uint64_t expected_schema_hash)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_core",
                               "Cooked file header is missing.");
    }
    if (header.magic != std::array<char, 4>{'H', 'S', 'B', 'N'} ||
        header.format_version != kCookedFormatVersion ||
        header.endian_marker != kLittleEndianMarker ||
        header.table_offset != sizeof(CookedHeader) ||
        (expected_schema_hash != 0 && header.schema_hash != expected_schema_hash))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_core",
                               "Cooked file header is incompatible.");
    }
    return Result::Success();
}

Result ReadCookedPayload(const std::filesystem::path &path,
                         std::uint64_t expected_schema_hash, CookedHeader &header,
                         std::vector<std::byte> &payload)
{
    std::ifstream stream(path, std::ios::binary);
    if (auto result = ReadCookedHeader(path, header, expected_schema_hash); !result)
    {
        return result;
    }
    stream.seekg(sizeof(header));
    payload.resize(header.payload_size);
    if (!stream.read(reinterpret_cast<char *>(payload.data()),
                     static_cast<std::streamsize>(payload.size())) ||
        Crc32(payload) != header.payload_crc32)
    {
        payload.clear();
        return Result::Failure(ErrorCode::InvalidArgument, "hs_core",
                               "Cooked file payload is truncated or corrupt.");
    }
    return Result::Success();
}

} // namespace hs
