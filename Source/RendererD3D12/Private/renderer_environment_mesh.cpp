#include "renderer_impl.hpp"

namespace hs
{
Result D3D12Renderer::Impl::CreateEnvironmentMeshes()
{
    constexpr std::array names{"trunk_0", "trunk_1", "trunk_2", "leaf_0", "leaf_1",
        "leaf_2", "rock_0", "rock_1", "rock_2", "rock_3", "grass_0", "grass_1", "grass_2", "grass_3"};
    auto result = frames[0].allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset mesh upload allocator", result);
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result)) return HResultFailure("Reset mesh upload list", result);
    std::vector<AllocationResource> uploads;
    for (std::size_t index = 0; index < environment_meshes.size(); ++index)
    {
        const auto path = CurrentExecutableDirectory() / "Cooked" /
                          (std::string("environment_") + names[index/3] +
                           (index%3 == 0 ? std::string{} : "_lod" + std::to_string(index%3)) + ".meshbin");
        std::vector<std::byte> bytes;
        if (auto read = ReadBinary(path, bytes); !read) return read;
        // HSEM, version, expanded vertex count, SkinnedVertex stride.
        std::array<std::uint32_t,4> header{};
        if (bytes.size() >= sizeof(header)) std::memcpy(header.data(), bytes.data(), sizeof(header));
        if (header[0] != 0x4d455348 || header[1] != 1 || header[2] == 0 ||
            header[2] > 3'000'000 || header[2] % 3 != 0 || header[3] != sizeof(Vertex) ||
            bytes.size() != sizeof(header) + static_cast<std::size_t>(header[2])*sizeof(Vertex))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid environment mesh: " + path.string());
        auto &mesh = environment_meshes[index];
        const auto size = static_cast<UINT>(bytes.size()-sizeof(header));
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(mesh.vertices, allocation, BufferDescription(size),
                                            D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
        uploads.emplace_back();
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), allocation, BufferDescription(size),
                                            D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
        void *mapped{}; D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(0, &no_read, &mapped);
        if (FAILED(result)) return HResultFailure("Map environment mesh", result);
        std::memcpy(mapped, bytes.data()+sizeof(header), size);
        uploads.back().resource->Unmap(0, nullptr);
        command_list->CopyBufferRegion(mesh.vertices.resource.Get(), 0, uploads.back().resource.Get(), 0, size);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = mesh.vertices.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
        mesh.vertex_view = {mesh.vertices.resource->GetGPUVirtualAddress(), size, sizeof(Vertex)};
        mesh.vertex_count = header[2];
    }
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close mesh upload", result);
    ID3D12CommandList *lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    return WaitForGpu();
}
}
