#pragma once

#include <hs/render_data/vfx_decal_input.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace hs
{
struct VfxDecalCandidate
{
    const VfxDecalInput *source;
    float area;
};

inline bool VfxDecalCandidateBefore(const VfxDecalCandidate &a, const VfxDecalCandidate &b) noexcept
{
    if (a.source->importance != b.source->importance) return a.source->importance > b.source->importance;
    if (a.area != b.area) return a.area > b.area;
    if (a.source->stable_id != b.source->stable_id) return a.source->stable_id < b.source->stable_id;
    if (a.source->stable_seed != b.source->stable_seed) return a.source->stable_seed < b.source->stable_seed;
    // All candidates borrow one contiguous input span, preserving stable-sort ties.
    return std::less<const VfxDecalInput *>{}(a.source, b.source);
}

class VfxDecalCandidates
{
  public:
    static constexpr std::size_t kCapacity = 64;

    explicit VfxDecalCandidates(std::size_t limit) : limit_(limit)
    {
        assert(limit <= kCapacity);
        worst_.fill(kNoCandidate);
    }

    void Add(VfxDecalCandidate candidate)
    {
        auto *const candidates = candidates_.data();
        auto *const worst = worst_.data();
        std::size_t slot;
        if (count_ < limit_) slot = count_++;
        else
        {
            if (count_ == 0 || !VfxDecalCandidateBefore(candidate, candidates[worst[1]])) return;
            slot = worst[1];
        }
        candidates[slot] = candidate;
        auto node = kCapacity + slot;
        worst[node] = static_cast<std::uint8_t>(slot);
        // One comparison per tree level avoids a second heap walk per replacement.
        while ((node /= 2) != 0)
        {
            const auto left = worst[node * 2], right = worst[node * 2 + 1];
            worst[node] = left == kNoCandidate ? right : right == kNoCandidate ? left :
                VfxDecalCandidateBefore(candidates[left], candidates[right]) ? right : left;
        }
    }

    // Finishes selection; add all inputs before borrowing this span.
    [[nodiscard]] std::span<const VfxDecalCandidate> Sorted() &
    {
        auto *const first = candidates_.data();
        std::sort(first, first + count_, VfxDecalCandidateBefore);
        return {first, count_};
    }

  private:
    static constexpr std::uint8_t kNoCandidate = static_cast<std::uint8_t>(kCapacity);
    std::array<VfxDecalCandidate, kCapacity> candidates_;
    std::array<std::uint8_t, kCapacity * 2> worst_;
    std::size_t limit_, count_{};
};
} // namespace hs
