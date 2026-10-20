#include "renderer_visibility.hpp"

#include "renderer_diagnostics.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace hs
{
namespace
{
constexpr std::uint32_t kRangeRecordStride = sizeof(GpuRasterRange);

[[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(std::uint64_t bytes,
                                                     D3D12_RESOURCE_FLAGS flags =
                                                         D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = bytes;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc = {1, 0};
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

[[nodiscard]] Result InvalidArgument(std::string_view message)
{
    return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                           std::string(message));
}

[[nodiscard]] Result InvalidState(std::string_view message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                           std::string(message));
}

[[nodiscard]] Result GrowPowerOfTwo(std::uint64_t required, std::uint32_t &capacity)
{
    if (required == 0)
    {
        capacity = 0;
        return Result::Success();
    }

    constexpr auto maximum = static_cast<std::uint64_t>(UINT32_MAX);
    std::uint64_t grown = 1;
    while (grown < required)
    {
        if (grown > maximum / 2)
            return InvalidArgument("Visibility buffer capacity exceeds uint32 power-of-two range.");
        grown *= 2;
    }
    capacity = static_cast<std::uint32_t>(grown);
    return Result::Success();
}

[[nodiscard]] Result ExistingCapacity(const AllocationResource &resource,
                                      std::uint32_t stride,
                                      std::uint32_t &capacity)
{
    if (!resource.resource)
    {
        capacity = 0;
        return Result::Success();
    }

    const auto bytes = resource.resource->GetDesc().Width;
    if (bytes == 0 || bytes % stride != 0 || bytes / stride > UINT32_MAX)
        return InvalidState("Visibility buffer has an invalid existing allocation size.");
    capacity = static_cast<std::uint32_t>(bytes / stride);
    return Result::Success();
}

[[nodiscard]] Result AllocateBuffer(AllocationResource &output,
                                    D3D12MA::Allocator *allocator,
                                    std::uint64_t bytes,
                                    D3D12_RESOURCE_FLAGS flags,
                                    D3D12_RESOURCE_STATES initial_state)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (initial_state == D3D12_RESOURCE_STATE_GENERIC_READ)
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    const auto description = BufferDescription(bytes, flags);
    const auto result = allocator->CreateResource(
        &allocation, &description, initial_state, nullptr, &output.allocation,
        IID_PPV_ARGS(output.resource.ReleaseAndGetAddressOf()));
    if (FAILED(result)) return HResultFailure("Create visibility buffer", result);
    return Result::Success();
}

[[nodiscard]] Result UploadRanges(AllocationResource &upload,
                                  const std::vector<GpuRasterRange> &ranges)
{
    if (ranges.empty()) return Result::Success();

    void *mapped{};
    D3D12_RANGE no_read{};
    const auto map_result = upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(map_result)) return HResultFailure("Map visibility ranges upload", map_result);

    const auto bytes = ranges.size() * sizeof(GpuRasterRange);
    std::memcpy(mapped, ranges.data(), bytes);
    const D3D12_RANGE written{0, bytes};
    upload.resource->Unmap(0, &written);
    return Result::Success();
}
} // namespace

Result EnsureVisibilityBuffers(ID3D12Device *device, D3D12MA::Allocator *allocator,
                              VisibilityFrameBuffers &buffers)
{
    if (!device || !allocator)
        return InvalidArgument("Visibility buffers require a device and allocator.");
    if (buffers.raster_capacity == 0)
        return InvalidArgument("Visibility raster capacity must reserve record 0.");
    if (buffers.ranges_cpu.size() > UINT32_MAX ||
        buffers.ranges_cpu.size() > std::numeric_limits<std::size_t>::max() /
            sizeof(GpuRasterRange))
        return InvalidArgument("Visibility range count exceeds uint32 capacity.");

    for (const auto &range : buffers.ranges_cpu)
    {
        if (range.raster_base > UINT32_MAX - range.instance_capacity)
            return InvalidArgument("Visibility range span overflows uint32 capacity.");
    }

    std::uint32_t existing_raster_capacity{};
    if (auto valid = ExistingCapacity(buffers.raster_records, kRasterRecordStride,
                                      existing_raster_capacity); !valid)
        return valid;
    std::uint32_t existing_range_capacity{};
    if (auto valid = ExistingCapacity(buffers.ranges_upload, kRangeRecordStride,
                                      existing_range_capacity); !valid)
        return valid;

    const auto requested_raster_capacity = std::max<std::uint64_t>(
        buffers.raster_capacity, 1u);
    const auto requested_range_capacity = std::max<std::uint64_t>(
        buffers.range_capacity, buffers.ranges_cpu.size());

    std::uint32_t next_raster_capacity = existing_raster_capacity;
    if (next_raster_capacity < requested_raster_capacity)
    {
        if (auto grown = GrowPowerOfTwo(requested_raster_capacity, next_raster_capacity);
            !grown)
            return grown;
    }
    std::uint32_t next_range_capacity = existing_range_capacity;
    if (next_range_capacity < requested_range_capacity)
    {
        if (auto grown = GrowPowerOfTwo(requested_range_capacity, next_range_capacity);
            !grown)
            return grown;
    }

    const bool replace_raster = !buffers.raster_records.resource ||
        next_raster_capacity != existing_raster_capacity;
    const bool replace_ranges = next_range_capacity != existing_range_capacity;
    AllocationResource next_raster;
    AllocationResource next_ranges;

    if (replace_raster)
    {
        const auto bytes = static_cast<std::uint64_t>(next_raster_capacity) *
            kRasterRecordStride;
        if (auto allocated = AllocateBuffer(next_raster, allocator, bytes,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST); !allocated)
            return allocated;
        next_raster.graph_access = Access::CopyDest;
    }
    if (replace_ranges)
    {
        if (next_range_capacity == 0)
            return InvalidState("Visibility range allocation has zero capacity.");
        const auto bytes = static_cast<std::uint64_t>(next_range_capacity) *
            kRangeRecordStride;
        if (auto allocated = AllocateBuffer(next_ranges, allocator, bytes,
                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ); !allocated)
            return allocated;
        next_ranges.graph_access = Access::CopySource;
        if (auto uploaded = UploadRanges(next_ranges, buffers.ranges_cpu); !uploaded)
            return uploaded;
    }
    else if (!buffers.ranges_cpu.empty())
    {
        if (auto uploaded = UploadRanges(buffers.ranges_upload, buffers.ranges_cpu);
            !uploaded)
            return uploaded;
    }

    if (replace_raster) buffers.raster_records = std::move(next_raster);
    if (replace_ranges) buffers.ranges_upload = std::move(next_ranges);
    buffers.raster_capacity = next_raster_capacity;
    buffers.range_capacity = next_range_capacity;
    return Result::Success();
}
} // namespace hs
