#include <hs/runtime/shipping_content.hpp>
#include <hs/core/shipped_content_owner.hpp>

#include <utility>
#include <limits>

namespace hs::shipping
{
Result ReadFont(const ShippedContentOwner &owner, const ShippingImageInfo &info, std::vector<std::byte> &output)
{
    if (!info.font_bytes || info.font_bytes > std::numeric_limits<std::uint32_t>::max() ||
        info.font_offset < 64 || info.font_offset > owner.Size() || info.font_bytes > owner.Size()-info.font_offset)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippingFont", "compiled font range is invalid");
    std::vector<std::byte> bytes(static_cast<std::size_t>(info.font_bytes));
    if (auto result=owner.ReadVerified(info.font_offset, bytes, info.font_digest); !result) return result;
    output=std::move(bytes);
    return Result::Success();
}
Result OpenImage(const std::filesystem::path &directory, const ShippingImageInfo &expected,
                 ShippedContentOwner &output)
{
    std::uint64_t expected_size = 0;
    for (unsigned byte = 0; byte < 8; ++byte)
        expected_size |= std::uint64_t(std::to_integer<unsigned char>(expected.header[48 + byte])) << (byte * 8);
    if (expected_size < expected.header.size() || !expected.relative_path)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippingImage", "invalid compiled image extent or path");

    ShippedContentOwner candidate;
    auto opened = ShippedContentOwner::Open(directory / expected.relative_path, candidate);
    if (!opened.Succeeded()) return opened;
    if (candidate.Size() != expected_size)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippingImage", "installed image length differs from executable");
    std::array<std::byte, 64> actual{};
    auto read = candidate.ReadExact(0, actual);
    if (!read.Succeeded()) return read;
    if (actual != expected.header)
        return Result::Failure(ErrorCode::InvalidArgument, "ShippingImage", "installed image header differs from executable");
    output = std::move(candidate);
    return Result::Success();
}
} // namespace hs::shipping
