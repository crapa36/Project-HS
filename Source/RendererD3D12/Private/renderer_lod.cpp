#include "renderer_impl.hpp"

#include "renderer_lod.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace hs
{

Result D3D12Renderer::Impl::PrepareLodStateBuffer(GpuLodStateBuffer &buffer,
                                                   std::size_t requested_capacity)
{
    const auto completed = graph_queue_executor.Completed();
    std::erase_if(buffer.retired, [&](const GpuLodStateBuffer::Retired &retired) {
        return retired.completion.IsSatisfiedBy(completed);
    });

    if (requested_capacity == 0 ||
        requested_capacity <= static_cast<std::size_t>(buffer.capacity))
    {
        return Result::Success();
    }

    const auto capacity = static_cast<std::uint64_t>(requested_capacity);
    if (capacity > std::numeric_limits<std::uint64_t>::max() /
                         kGpuLodStateBytesPerSlot)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "GPU LOD state buffer size overflow.");
    }
    const auto bytes = capacity * kGpuLodStateBytesPerSlot;
    if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "GPU LOD state buffer exceeds addressable size.");
    }

    AllocationResource next_states;
    AllocationResource next_upload;
    D3D12MA::ALLOCATION_DESC default_allocation{};
    default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(
            next_states, default_allocation,
            BufferDescription(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COPY_DEST);
        !created)
    {
        return created;
    }

    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(next_upload, upload_allocation,
                                        BufferDescription(bytes),
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
    {
        return created;
    }

    std::byte *mapped{};
    D3D12_RANGE no_read{};
    const auto map_result = next_upload.resource->Map(
        0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(map_result))
    {
        return HResultFailure("Map GPU LOD state zero upload", map_result);
    }
    std::memset(mapped, 0, static_cast<std::size_t>(bytes));
    next_upload.resource->Unmap(0, nullptr);

    // Preparation before graph execution can leave an unsubmitted replacement
    // in the fields below. It is safe to drop; the committed source remains
    // the only prefix source we ever preserve.
    AllocationResource preserved_prefix;
    const auto preserved_capacity = buffer.capacity;
    if (!buffer.prefix_source.resource && buffer.initialized && buffer.states.resource)
        preserved_prefix = std::move(buffer.states);
    buffer.states.Reset();
    buffer.zero_upload.Reset();

    if (preserved_prefix.resource)
    {
        buffer.prefix_source = std::move(preserved_prefix);
        buffer.prefix_bytes = preserved_capacity * kGpuLodStateBytesPerSlot;
    }

    buffer.states = std::move(next_states);
    buffer.zero_upload = std::move(next_upload);
    buffer.capacity = capacity;
    buffer.initialized = false;
    return Result::Success();
}

void D3D12Renderer::Impl::CommitLodStateBuffer(GpuLodStateBuffer &buffer)
{
    CompletionSet retirement_completion;
    retirement_completion.Merge(buffer.states.graph_completion);
    retirement_completion.Merge(buffer.zero_upload.graph_completion);
    retirement_completion.Merge(buffer.prefix_source.graph_completion);

    if (buffer.zero_upload.resource || buffer.prefix_source.resource)
    {
        GpuLodStateBuffer::Retired retired;
        retired.staging = std::move(buffer.zero_upload);
        retired.source = std::move(buffer.prefix_source);
        retired.completion = retirement_completion;
        buffer.retired.push_back(std::move(retired));
    }

    buffer.prefix_bytes = 0;
    buffer.initialized = true;
}

} // namespace hs
