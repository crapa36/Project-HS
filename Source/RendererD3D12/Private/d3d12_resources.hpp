#pragma once

#include <D3D12MemAlloc.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace hs
{

constexpr std::uint32_t kFrameUploadSize = 576 * 1024;

struct AllocationResource
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12MA::Allocation *allocation{};

    AllocationResource() = default;
    ~AllocationResource() { Reset(); }
    AllocationResource(const AllocationResource &) = delete;
    AllocationResource &operator=(const AllocationResource &) = delete;
    AllocationResource(AllocationResource &&other) noexcept
        : resource(std::move(other.resource)),
          allocation(std::exchange(other.allocation, nullptr))
    {
    }
    AllocationResource &operator=(AllocationResource &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            resource = std::move(other.resource);
            allocation = std::exchange(other.allocation, nullptr);
        }
        return *this;
    }

    void Reset() noexcept
    {
        resource.Reset();
        if (allocation)
        {
            allocation->Release();
            allocation = nullptr;
        }
    }
};

struct FrameContext
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    AllocationResource upload;
    AllocationResource ui_upload;
    std::byte *mapped{};
    std::size_t upload_size{kFrameUploadSize};
    std::byte *ui_mapped{};
    std::uint64_t fence_value{};
    bool timestamps_recorded{};
    bool ui_initialized{};
};

} // namespace hs
