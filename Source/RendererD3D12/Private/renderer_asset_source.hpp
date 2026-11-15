#pragma once

#include "checked_binary_read.hpp"
#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/sha256.hpp>
#include <hs/render_data/render_bulk.hpp>

#include <atomic>
#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hs
{
struct LooseRendererAsset
{
    struct WholeFileProof
    {
        std::uint64_t byte_count{};
        std::array<std::byte, 32> digest{};
        bool operator==(const WholeFileProof &) const = default;
    };

    std::filesystem::path path;
    std::optional<WholeFileProof> admitted;
};

struct PackedRendererAsset
{
    std::shared_ptr<const ShippedContentOwner> owner;
    render_data::BulkRange range;
};

struct ImmutableAssetRange
{
    std::uint64_t offset{}; // Relative to the product range in the source.
    std::uint32_t byte_count{};
    std::array<std::byte, 32> digest{};
};

// The source kind is selected by content composition. Loose products carry a
// path and optional whole-file admission proof; packed products carry only a
// retained owner and one bounded bulk range.
struct RendererAssetSource
{
    using Storage = std::variant<LooseRendererAsset, PackedRendererAsset>;
    Storage storage{LooseRendererAsset{}};

    [[nodiscard]] static RendererAssetSource Loose(std::filesystem::path path,
        std::optional<LooseRendererAsset::WholeFileProof> proof = std::nullopt)
    {
        RendererAssetSource result;
        result.storage = LooseRendererAsset{std::move(path), std::move(proof)};
        return result;
    }

    [[nodiscard]] static RendererAssetSource Packed(
        std::shared_ptr<const ShippedContentOwner> owner, render_data::BulkRange range)
    {
        RendererAssetSource result;
        result.storage = PackedRendererAsset{std::move(owner), std::move(range)};
        return result;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        if (const auto *loose = std::get_if<LooseRendererAsset>(&storage)) return loose->path.empty();
        const auto &packed = std::get<PackedRendererAsset>(storage);
        return !packed.owner;
    }
    [[nodiscard]] bool IsPacked() const noexcept
    { return std::holds_alternative<PackedRendererAsset>(storage); }
    [[nodiscard]] const LooseRendererAsset &Loose() const noexcept
    { return std::get<LooseRendererAsset>(storage); }
    [[nodiscard]] const PackedRendererAsset &Packed() const noexcept
    { return std::get<PackedRendererAsset>(storage); }
    [[nodiscard]] bool HasAdmittedWholeFileProof() const noexcept
    {
        const auto *loose = std::get_if<LooseRendererAsset>(&storage);
        return loose && loose->admitted.has_value();
    }
    [[nodiscard]] std::optional<LooseRendererAsset::WholeFileProof> AdmittedWholeFileProof() const noexcept
    {
        const auto *loose = std::get_if<LooseRendererAsset>(&storage);
        return loose ? loose->admitted : std::nullopt;
    }
    [[nodiscard]] std::string Describe(std::string_view product_id) const
    {
        std::string result(product_id);
        if (const auto *packed = std::get_if<PackedRendererAsset>(&storage))
            result += " (packed range offset " + std::to_string(packed->range.offset) +
                ", bytes " + std::to_string(packed->range.byte_count) + ")";
        else
            result += " (" + std::get<LooseRendererAsset>(storage).path.string() + ")";
        return result;
    }
    void clear() noexcept { storage = LooseRendererAsset{}; }

    [[nodiscard]] bool operator==(const RendererAssetSource &other) const noexcept
    {
        if (storage.index() != other.storage.index()) return false;
        if (const auto *loose = std::get_if<LooseRendererAsset>(&storage))
        {
            const auto &other_loose = std::get<LooseRendererAsset>(other.storage);
            return loose->path == other_loose.path && loose->admitted == other_loose.admitted;
        }
        const auto &packed = std::get<PackedRendererAsset>(storage);
        const auto &other_packed = std::get<PackedRendererAsset>(other.storage);
        const auto &a = packed.range;
        const auto &b = other_packed.range;
        return packed.owner == other_packed.owner && a.offset == b.offset && a.byte_count == b.byte_count &&
            a.format == b.format && a.expected_digest == b.expected_digest;
    }

    [[nodiscard]] Result ByteCount(std::uint64_t &bytes) const
    {
        if (const auto *packed = std::get_if<PackedRendererAsset>(&storage))
        {
            const auto &range = packed->range;
            if (!packed->owner || !range.byte_count || range.offset > packed->owner->Size() ||
                range.byte_count > packed->owner->Size() - range.offset ||
                range.byte_count > std::numeric_limits<std::size_t>::max())
                return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Bulk exceeds the retained pack.");
            bytes = range.byte_count;
            return Result::Success();
        }
        const auto &loose = std::get<LooseRendererAsset>(storage);
        std::error_code error;
        const auto size = std::filesystem::file_size(loose.path, error);
        if (error)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Could not size immutable asset: " + loose.path.string());
        bytes = size;
        return Result::Success();
    }

    [[nodiscard]] Result BindExpectedDigest(std::string_view digest)
    {
        if (digest.size() != 71 || !digest.starts_with("sha256:"))
            return Result::Failure(ErrorCode::InvalidArgument, "RendererAssetSource", "Expected digest must be sha256 plus 64 lowercase hexadecimal digits.");
        std::array<std::byte, 32> parsed{};
        const auto nibble = [](char value) -> int {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            return -1;
        };
        for (std::size_t index = 0; index < parsed.size(); ++index)
        {
            const auto high = nibble(digest[7 + index * 2]);
            const auto low = nibble(digest[8 + index * 2]);
            if (high < 0 || low < 0)
                return Result::Failure(ErrorCode::InvalidArgument, "RendererAssetSource", "Expected digest contains a non-lowercase hexadecimal digit.");
            parsed[index] = static_cast<std::byte>((high << 4) | low);
        }
        if (auto *packed = std::get_if<PackedRendererAsset>(&storage))
        {
            if (packed->range.expected_digest != parsed)
                return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Selected digest differs from the packed range digest.");
            return Result::Success();
        }
        auto &loose = std::get<LooseRendererAsset>(storage);
        if (loose.admitted)
        {
            if (loose.admitted->digest != parsed)
                return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Selected digest differs from the admitted loose product.");
            return Result::Success();
        }
        std::uint64_t bytes{};
        if (auto result = ByteCount(bytes); !result) return result;
        // Selected metadata is bound before I/O; every full read still verifies actual bytes.
        loose.admitted = LooseRendererAsset::WholeFileProof{bytes, parsed};
        return Result::Success();
    }

    [[nodiscard]] Result Read(std::vector<std::byte> &output,
        std::optional<std::uint64_t> expected_bytes = std::nullopt) const
    {
        if (const auto *packed = std::get_if<PackedRendererAsset>(&storage))
        {
            std::uint64_t bytes{};
            if (auto result = ByteCount(bytes); !result) return result;
            if (expected_bytes && bytes != *expected_bytes)
                return Result::Failure(ErrorCode::InvalidState, "RendererPack", "Bulk size differs from its reservation.");
            std::vector<std::byte> temporary(static_cast<std::size_t>(bytes));
            if (auto result = packed->owner->ReadVerified(packed->range.offset, temporary,
                    packed->range.expected_digest); !result) return result;
            output = std::move(temporary);
            return Result::Success();
        }

        const auto &loose = std::get<LooseRendererAsset>(storage);
        const auto read_size = loose.admitted ? std::optional<std::uint64_t>{loose.admitted->byte_count} : expected_bytes;
        std::vector<std::byte> temporary;
        if (auto result = ReadCheckedBinary(loose.path, temporary, read_size); !result) return result;
        if ((expected_bytes && temporary.size() != *expected_bytes) ||
            (loose.admitted && (temporary.size() != loose.admitted->byte_count ||
                                Sha256(temporary) != loose.admitted->digest)))
            return Result::Failure(ErrorCode::InvalidState, "RendererDevelopmentCatalog", "Loose product changed after admission.");
        output = std::move(temporary);
        return Result::Success();
    }

    // Offsets are relative to this product, independent of its pack placement.
    [[nodiscard]] Result ReadRanges(std::span<const ImmutableAssetRange> ranges,
        std::span<std::byte> output, const std::atomic<bool> *cancelled = nullptr) const
    {
        std::uint64_t product_bytes{};
        std::uint64_t product_base{};
        const ShippedContentOwner *owner{};
        ShippedContentOwner loose_owner;
        if (IsPacked())
        {
            const auto &packed = Packed();
            if (auto result = ByteCount(product_bytes); !result) return result;
            product_base = packed.range.offset;
            owner = packed.owner.get();
        }
        else
        {
            const auto &loose = Loose();
            if (auto result = ShippedContentOwner::Open(loose.path, loose_owner); !result) return result;
            product_bytes = loose_owner.Size();
            owner = &loose_owner;
            if (loose.admitted && product_bytes != loose.admitted->byte_count)
                return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Loose product changed after admission.");
        }

        std::size_t destination{};
        // Cancelled range batches leave partial output for the request owner to discard.
        for (const auto &range : ranges)
        {
            if (cancelled && cancelled->load(std::memory_order_acquire))
                return Result::Success();
            if (!range.byte_count || range.offset > product_bytes ||
                range.byte_count > product_bytes - range.offset || destination > output.size() ||
                range.byte_count > output.size() - destination)
                return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Sample range exceeds its product or output.");
            if (auto result = owner->ReadVerified(product_base + range.offset,
                    output.subspan(destination, range.byte_count), range.digest); !result) return result;
            destination += range.byte_count;
        }
        if (destination != output.size())
            return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Sample ranges do not fill their output.");
        return Result::Success();
    }

    [[nodiscard]] Result ReadRange(std::uint64_t offset, std::span<std::byte> output,
        const std::array<std::byte, 32> &digest) const
    {
        if (output.size() > std::numeric_limits<std::uint32_t>::max())
            return Result::Failure(ErrorCode::InvalidState, "RendererAssetSource", "Sample range exceeds the immutable range size limit.");
        const std::array ranges{ImmutableAssetRange{offset, static_cast<std::uint32_t>(output.size()), digest}};
        return ReadRanges(ranges, output);
    }
};
} // namespace hs
