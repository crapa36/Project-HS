#pragma once

#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/cooked_format.hpp>
#include <hs/render_data/render_bulk.hpp>

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

namespace hs
{
inline Result ValidatePackedRendererAssets(const ShippedContentOwner &owner,
    std::span<const render_data::RendererBulkDescriptor> entries)
{
    if (!owner.Identity() || entries.empty())
        return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Pack owner or catalog is absent.");
    std::uint64_t previous_id{};
    for (const auto &entry : entries)
    {
        const auto &range = entry.range;
        if (!entry.asset_id.value || entry.asset_id.value <= previous_id ||
            range.format > render_data::RenderBulkFormat::SurfaceLayoutAbi ||
            !range.byte_count || range.byte_count > std::numeric_limits<std::size_t>::max() ||
            range.offset % 64 || range.offset < 64 || range.offset > owner.Size() ||
            range.byte_count > owner.Size() - range.offset)
            return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Compiled bulk catalog has invalid IDs or ranges.");
        previous_id = entry.asset_id.value;
    }
    return Result::Success();
}

// The immutable directory is validated at initialization, not for every read.
inline Result ReadPackedRendererAsset(const ShippedContentOwner &owner,
    std::span<const render_data::RendererBulkDescriptor> entries, AssetId id,
    render_data::RenderBulkFormat format, std::vector<std::byte> &output)
{
    const auto found = std::lower_bound(entries.begin(), entries.end(), id.value,
        [](const auto &entry, std::uint64_t value) { return entry.asset_id.value < value; });
    if (found == entries.end() || found->asset_id.value != id.value || found->range.format != format)
        return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Required typed bulk is absent or has the wrong format.");
    const auto &range = found->range;
    if (range.offset > owner.Size() || range.byte_count > owner.Size() - range.offset ||
        range.byte_count > std::numeric_limits<std::size_t>::max())
        return Result::Failure(ErrorCode::InvalidArgument, "RendererPack", "Bulk exceeds the opened pack.");
    std::vector<std::byte> bytes(static_cast<std::size_t>(range.byte_count));
    if (auto read = owner.ReadVerified(range.offset, bytes, range.expected_digest); !read) return read;
    output = std::move(bytes);
    return Result::Success();
}
} // namespace hs
