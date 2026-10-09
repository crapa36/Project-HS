#pragma once

#include "d3d12_resources.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hs
{

inline constexpr std::uint32_t kInvalidRasterId = UINT32_MAX;
inline constexpr std::uint32_t kRasterRecordStride = 32;

// Byte-for-byte counterpart of the future VisibilityBuffer raster record.
struct GpuRasterDrawRecord
{
    std::uint32_t geometry_record_id{};
    std::uint32_t visible_ref_index{};
    std::uint32_t ref_source{};
    std::uint32_t material_word{};
    std::uint32_t instance_index{};
    std::uint32_t instance_generation{};
    std::uint32_t page_generation{};
    std::uint32_t flags{};
};

static_assert(sizeof(GpuRasterDrawRecord) == kRasterRecordStride);
static_assert(offsetof(GpuRasterDrawRecord, geometry_record_id) == 0);
static_assert(offsetof(GpuRasterDrawRecord, visible_ref_index) == 4);
static_assert(offsetof(GpuRasterDrawRecord, ref_source) == 8);
static_assert(offsetof(GpuRasterDrawRecord, material_word) == 12);
static_assert(offsetof(GpuRasterDrawRecord, instance_index) == 16);
static_assert(offsetof(GpuRasterDrawRecord, instance_generation) == 20);
static_assert(offsetof(GpuRasterDrawRecord, page_generation) == 24);
static_assert(offsetof(GpuRasterDrawRecord, flags) == 28);

struct GpuRasterRange
{
    std::uint32_t geometry_record_id{};
    std::uint32_t raster_base{};
    std::uint32_t instance_capacity{};
    std::uint32_t material_word{};
};

static_assert(sizeof(GpuRasterRange) == 16);
static_assert(offsetof(GpuRasterRange, geometry_record_id) == 0);
static_assert(offsetof(GpuRasterRange, raster_base) == 4);
static_assert(offsetof(GpuRasterRange, instance_capacity) == 8);
static_assert(offsetof(GpuRasterRange, material_word) == 12);

struct VisibilityFrameBuffers
{
    AllocationResource raster_records;
    AllocationResource ranges_upload;
    std::vector<GpuRasterRange> ranges_cpu;
    std::uint32_t raster_capacity{1}; // Raster record 0 is reserved.
    std::uint32_t range_capacity{};

    void Clear() noexcept
    {
        ranges_cpu.clear();
    }

    void Reset() noexcept
    {
        raster_records.Reset();
        ranges_upload.Reset();
        ranges_cpu.clear();
        raster_capacity = 1;
        range_capacity = 0;
    }
};

[[nodiscard]] Result EnsureVisibilityBuffers(
    ID3D12Device *device, D3D12MA::Allocator *allocator,
    VisibilityFrameBuffers &buffers);

} // namespace hs
