#pragma once

#include "d3d12_resources.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace hs
{
// Owned before ExecuteCommandLists. A failed post-work Signal/Wait leaves this
// lease intact; only a real fence completion or device removal releases it.
struct ResidentUtilitySubmission
{
    std::vector<AllocationResource> uploads;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::uint64_t fence_value{};
    bool signaled{};
};

// Exact native allocation sizes, rather than package file sizes, describe GPU
// overlap. The DXGI observation used for admission includes the live generation.
struct ResidentPreparationAccounting
{
    bool enabled{};
    bool unified_memory{};
    std::uint64_t host_staging_budget_bytes{};
    std::uint64_t default_bytes{}, upload_bytes{}, readback_bytes{};
    std::uint64_t live_native_bytes{}, peak_native_bytes{};
    std::uint64_t closure_payload_bytes{}, largest_payload_bytes{};
    std::uint64_t retained_host_bytes{};
};
} // namespace hs
