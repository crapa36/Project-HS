#pragma once

#include <hs/core/canonical_bytes.hpp>
#include <hs/core/sha256.hpp>
#include <hs/runtime/shipping_content.hpp>

#include <algorithm>
#include <array>

namespace hs::shipping
{
// This short tag binds the compiled header to the physical VFX ranges. Full
// per-range SHA256 verification remains the integrity check for their bytes.
inline std::array<std::byte, 8> VfxStorageTag(const std::array<ShippingRange, 2> &ranges)
{
    CanonicalBytes bytes("project-hs-shipping-vfx-storage", 1);
    bytes.Sequence(ranges, [](CanonicalBytes &out, const ShippingRange &range) {
        out.Fields(range.offset, range.size);
        out.Blob(range.digest);
    });
    const auto digest = Sha256(bytes.View());
    std::array<std::byte, 8> tag{};
    std::copy_n(digest.begin(), tag.size(), tag.begin());
    return tag;
}
} // namespace hs::shipping
