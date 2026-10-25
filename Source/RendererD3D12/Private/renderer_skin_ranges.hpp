#pragma once

#include <hs/core/render_snapshot.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace hs
{
struct SkinDeformedVertex
{
    std::array<float, 3> position, normal, tangent;
};
static_assert(sizeof(SkinDeformedVertex) == 36);
static_assert(offsetof(SkinDeformedVertex, normal) == 12);
static_assert(offsetof(SkinDeformedVertex, tangent) == 24);

struct SkinVertexRange
{
    std::uint32_t offset{}, count{};
    std::uint64_t geometry_identity{};
};

constexpr bool SkinVertexRangeFits(std::uint32_t offset, std::uint32_t count,
                                  std::uint32_t stride, std::uint64_t capacity_bytes) noexcept
{
    if (!count || (stride != sizeof(SkinDeformedVertex) && stride != sizeof(std::array<float, 3>))) return false;
    const auto end = (std::uint64_t(offset) + count) * stride;
    return end <= capacity_bytes && end <= std::numeric_limits<std::uint32_t>::max();
}

// One immutable submitted deformation layout; history retains its own offsets.
class SkinVertexRanges
{
  public:
    static constexpr std::uint32_t kVertexBytes = sizeof(SkinDeformedVertex);
    static constexpr std::uint32_t kPreviousPositionBytes = sizeof(std::array<float, 3>);
    static constexpr std::uint32_t kMaxVertices =
        std::numeric_limits<std::uint32_t>::max() / kVertexBytes;

    bool Resolve(RenderSceneInstanceKey key, std::uint32_t count, SkinVertexRange &range,
                 std::uint64_t geometry_identity = 0)
    {
        if (const auto found = ranges_.find(key); found != ranges_.end())
        {
            range = found->second;
            return count == range.count && geometry_identity == range.geometry_identity;
        }
        if (!count || count > kMaxVertices - vertices_) return false;
        range = {vertices_, count, geometry_identity};
        ranges_.emplace(key, range);
        vertices_ += count;
        return true;
    }
    [[nodiscard]] const SkinVertexRange *Find(RenderSceneInstanceKey key) const noexcept
    {
        const auto found = ranges_.find(key);
        return found == ranges_.end() ? nullptr : &found->second;
    }
    [[nodiscard]] std::uint32_t VertexCount() const noexcept { return vertices_; }

  private:
    std::unordered_map<RenderSceneInstanceKey, SkinVertexRange, RenderSceneInstanceKeyHash> ranges_;
    std::uint32_t vertices_{};
};
} // namespace hs
