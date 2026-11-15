#include "renderer_impl.hpp"

namespace hs
{
Result D3D12Renderer::Impl::PrepareMeshMaterialBindings(ID3D12GraphicsCommandList *list,
    std::vector<AllocationResource> &uploads)
{
    std::vector<DirectX::XMUINT2> slots{{0, 0}};
    for (std::size_t asset = 0; asset < monster_assets.size() + 1; ++asset)
    {
        const auto count = asset == 0 ? archer_material_count : monster_assets[asset - 1].material_count;
        const auto &bindings = asset == 0 ? archer_material_bindings : monster_assets[asset - 1].material_bindings;
        const auto binding_set = static_cast<std::uint32_t>(slots.size());
        slots.push_back({binding_set + 1u, count});
        const auto first = slots.size();
        slots.resize(slots.size() + count, {UINT32_MAX, UINT32_MAX});
        std::uint32_t opaque_binding_count{};
        std::uint32_t translucent_binding_count{};
        for (const auto &binding : bindings)
        {
            if (binding.slot >= count || binding.hero_ordinal >= kCharacterMaterialNames.size())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid native character material binding.");
            auto &slot = slots[first + binding.slot];
            if (binding.domain == render_data::CharacterMaterialDomain::Opaque)
            {
                slot.x = binding.hero_ordinal;
                ++opaque_binding_count;
            }
            else
            {
                slot.y = binding.hero_ordinal;
                ++translucent_binding_count;
            }
        }
        for (std::size_t slot = first; slot < slots.size(); ++slot)
            if (slots[slot].x == UINT32_MAX && slots[slot].y == UINT32_MAX)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Character slot has no character material binding.");
        const GeometryMaterialDomains domains{opaque_binding_count != 0, translucent_binding_count != 0,
            binding_set};
        if (asset == 0) geometry_material_domains[kArcherGeometryPage] = domains;
        else
        {
            const auto family = asset - 1;
            geometry_material_domains[kMonsterGeometryPage + family] = domains;
            geometry_material_domains[kMonsterGeometryPage + 6 + family * 2] = domains;
            geometry_material_domains[kMonsterGeometryPage + 7 + family * 2] = domains;
        }
    }
    procedural_water_material = {false, true, static_cast<std::uint32_t>(slots.size())};
    slots.push_back({static_cast<std::uint32_t>(slots.size()) + 1u, 1u});
    slots.push_back({UINT32_MAX, HS_MATERIAL_PROGRAM_WATER});
    if (slots.size() > (UINT32_MAX >> HS_GEOMETRY_MATERIAL_SET_SHIFT))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Native mesh material binding set index capacity exceeded.");
    const auto bytes = slots.size() * sizeof(DirectX::XMUINT2);
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(mesh_material_bindings_gpu, allocation, BufferDescription(bytes),
            D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
    uploads.emplace_back();
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(uploads.back(), allocation, BufferDescription(bytes),
            D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
    void *mapped{};
    D3D12_RANGE no_read{};
    const auto mapped_result = uploads.back().resource->Map(0, &no_read, &mapped);
    if (FAILED(mapped_result)) return HResultFailure("Map character material binding upload", mapped_result);
    std::memcpy(mapped, slots.data(), bytes);
    uploads.back().resource->Unmap(0, nullptr);
    list->CopyBufferRegion(mesh_material_bindings_gpu.resource.Get(), 0, uploads.back().resource.Get(), 0, bytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {mesh_material_bindings_gpu.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &barrier);
    mesh_material_bindings_gpu.graph_access = enhanced ? Access::AllShaderRead : Access::Common;
    mesh_material_bindings_gpu.graph_queue = QueueHint::Direct;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = static_cast<UINT>(slots.size());
    view.Buffer.StructureByteStride = sizeof(DirectX::XMUINT2);
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + kCharacterBindingDescriptorOffset) * srv_stride;
        device->CreateShaderResourceView(mesh_material_bindings_gpu.resource.Get(), &view, handle);
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    mesh_material_bindings_cpu = std::move(slots);
#endif
    return Result::Success();
}
} // namespace hs
