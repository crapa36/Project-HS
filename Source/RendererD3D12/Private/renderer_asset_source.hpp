#pragma once

#include "checked_binary_read.hpp"
#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/sha256.hpp>
#include <hs/render_data/render_bulk.hpp>

#include <algorithm>
#include <array>
#include <span>

namespace hs
{
// A loose development product or one immutable range in the retained shipping file.
struct RendererAssetSource
{
    std::filesystem::path path;
    std::shared_ptr<const ShippedContentOwner> pack;
    render_data::BulkRange range;

    [[nodiscard]] bool empty() const noexcept { return !pack && path.empty(); }
    void clear() noexcept { *this = {}; }
    [[nodiscard]] bool operator==(const RendererAssetSource &other) const noexcept
    {
        return path == other.path && pack == other.pack && range.offset == other.range.offset &&
            range.byte_count == other.range.byte_count && range.format == other.range.format &&
            range.expected_digest == other.range.expected_digest;
    }
    [[nodiscard]] Result ByteCount(std::uint64_t &bytes) const
    {
        if (pack)
        {
            if (!range.byte_count || range.offset > pack->Size() || range.byte_count > pack->Size() - range.offset ||
                range.byte_count > std::numeric_limits<std::size_t>::max())
                return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Bulk exceeds the retained pack.");
            bytes = range.byte_count;
            return Result::Success();
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Could not size immutable asset: " + path.string());
        bytes = size;
        return Result::Success();
    }
    [[nodiscard]] Result Read(std::vector<std::byte> &output,
        std::optional<std::uint64_t> expected_bytes = std::nullopt) const
    {
        if (!pack)
        {
            std::vector<std::byte> temporary;
            if (auto result = ReadCheckedBinary(path, temporary,
                    range.byte_count ? std::optional<std::uint64_t>{range.byte_count} : expected_bytes); !result) return result;
            if ((expected_bytes && temporary.size() != *expected_bytes) ||
                (range.byte_count && Sha256(temporary) != range.expected_digest))
                return Result::Failure(ErrorCode::InvalidState, "RendererDevelopmentCatalog", "Loose product changed after admission.");
            output = std::move(temporary);
            return Result::Success();
        }
        std::uint64_t bytes{};
        if (auto result = ByteCount(bytes); !result) return result;
        if (expected_bytes && bytes != *expected_bytes)
            return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Bulk size differs from its reservation.");
        std::vector<std::byte> temporary(static_cast<std::size_t>(bytes));
        if (auto result = pack->ReadVerified(range.offset, temporary, range.expected_digest); !result) return result;
        output = std::move(temporary);
        return Result::Success();
    }

    [[nodiscard]] Result ReadRange(std::uint64_t offset, std::span<std::byte> output,
        const std::array<std::byte, 32> &digest) const
    {
        std::uint64_t product_bytes{};
        if (auto result = ByteCount(product_bytes); !result) return result;
        const auto begin = pack ? range.offset : 0;
        if (!range.byte_count || product_bytes != range.byte_count || offset < begin ||
            offset - begin > product_bytes || output.size() > product_bytes - (offset - begin))
            return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Sample range exceeds its admitted product.");
        if (pack) return pack->ReadVerified(offset, output, digest);
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream || stream.tellg() < 0 || static_cast<std::uint64_t>(stream.tellg()) != product_bytes ||
            offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
            output.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
            return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Loose sample file changed after admission.");
        std::vector<std::byte> bytes(output.size());
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size()) || Sha256(bytes) != digest)
            return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Loose sample range digest or read failed.");
        std::copy(bytes.begin(), bytes.end(), output.begin());
        return Result::Success();
    }
};
} // namespace hs
