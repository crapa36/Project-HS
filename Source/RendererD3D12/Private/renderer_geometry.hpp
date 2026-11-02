#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace hs
{
// The package boundary validates the remap permutation and material subranges.
// Byte spans avoid alignment assumptions about the portable package storage.
inline bool WriteGeometryRasterIndexSuffix(std::span<std::byte> destination,
    std::span<const std::byte> canonical, std::span<const std::byte> remap) noexcept
{
    if (canonical.empty() || canonical.size() % 12 || destination.size() != canonical.size() ||
        remap.size() != canonical.size() / 3) return false;
    for (std::size_t triangle = 0; triangle < remap.size() / 4; ++triangle)
    {
        std::uint32_t primitive{};
        std::memcpy(&primitive, remap.data() + triangle * 4, 4);
        if (primitive >= canonical.size() / 12) return false;
        std::memcpy(destination.data() + triangle * 12, canonical.data() + primitive * 12, 12);
    }
    return true;
}
// Four existing raw descriptors address aligned slices of one immutable page.
constexpr bool GeometryPageLayout(const std::array<std::uint64_t, 4> &sizes,
    std::array<std::uint64_t, 4> &offsets, std::uint64_t &total) noexcept
{
    total = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i)
    {
        if (!sizes[i] || sizes[i] % 4 || sizes[i] > UINT32_MAX || total > UINT32_MAX - 15u)
            return false;
        total = (total + 15u) & ~std::uint64_t(15u);
        offsets[i] = total;
        if (sizes[i] > UINT32_MAX - total) return false;
        total += sizes[i];
    }
    return true;
}

inline constexpr std::uint32_t kGeometryDrawRecordCapacity = 4096;
inline constexpr std::uint32_t kGeometryGpuSceneRefs = 1u;
inline constexpr std::uint32_t kGeometryMeshEligible = 2u;
inline constexpr std::uint32_t kGeometryStaticInstances = 4u;
inline constexpr std::uint32_t kGeometryPreviousPage = 8u;
inline constexpr std::uint32_t kGeometryBuiltin = 16u;
inline constexpr std::uint32_t kGeometryExplicitMaterialBinding = 32u;
inline constexpr std::uint32_t kGeometryRasterPrimitiveRemap = 64u;
inline constexpr std::uint32_t kGeometryArgumentStride = 24;
inline constexpr std::uint32_t kGeometryMeshDispatchDimension = 65535;
inline constexpr std::uint32_t kGeometryMeshDispatchGroupLimit = 1u << 22;

// Byte-for-byte counterpart of GeometryDrawRecord in geometry.hlsli. Record 0
// is invalid so packed geometry pass values never overlap legacy cascades 0..2.
namespace renderer_detail
{
struct GpuGeometryDrawRecord
{
    std::uint32_t page_slot{}, first_index{}, index_count{}, first_meshlet{};
    std::uint32_t meshlet_count{}, visible_base{}, instance_count{}, flags{};
    std::uint32_t triangle_offset{}, remap_offset{}, geometry_generation{}, lod{};
    std::uint32_t mesh_id{}, reserved0{}, reserved1{}, reserved2{};
};
static_assert(sizeof(GpuGeometryDrawRecord) == 64);
static_assert(offsetof(GpuGeometryDrawRecord, instance_count) == 24);
} // namespace renderer_detail

// The grid is padded to full rows. Its PRODUCT, including padded groups,
// must satisfy DispatchMesh's limit, not only the number of real meshlets.
[[nodiscard]] constexpr bool GeometryMeshDispatchFits(
    std::uint32_t meshlet_count, std::uint32_t instance_capacity) noexcept
{
    const auto groups = static_cast<std::uint64_t>(meshlet_count) * instance_capacity;
    if (meshlet_count == 0 || groups > kGeometryMeshDispatchGroupLimit) return false;
    const auto x = groups < kGeometryMeshDispatchDimension
        ? groups : kGeometryMeshDispatchDimension;
    const auto y = groups == 0 ? 1 :
        (groups + kGeometryMeshDispatchDimension - 1) / kGeometryMeshDispatchDimension;
    return x * y <= kGeometryMeshDispatchGroupLimit;
}
} // namespace hs
