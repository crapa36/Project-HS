#pragma once

#include "d3d12_resources.hpp"

#include <cstdint>
#include <vector>

namespace hs
{

// Four view records of 32 bytes each are kept for every persistent scene slot.
constexpr std::uint64_t kGpuLodStateBytesPerSlot = 32u * 4u;

struct GpuLodStateBuffer
{
    struct Retired
    {
        AllocationResource staging;
        AllocationResource source;
        CompletionSet completion{};
    };

    AllocationResource states;
    AllocationResource zero_upload;
    AllocationResource prefix_source;
    std::uint64_t capacity{};
    std::uint64_t prefix_bytes{};
    bool initialized{};
    std::vector<Retired> retired;
};

} // namespace hs
