#include "renderer_impl.hpp"

namespace hs
{
Result D3D12Renderer::Impl::PrepareHeroBindings(ID3D12GraphicsCommandList *list,
    std::vector<AllocationResource> &uploads)
{
    std::vector<DirectX::XMUINT2> slots;
    for (std::size_t asset = 0; asset < hero_binding_ranges.size(); ++asset)
    {
        const auto count = asset == 0 ? archer_material_count : monster_assets[asset - 1].material_count;
        const auto &bindings = asset == 0 ? archer_material_bindings : monster_assets[asset - 1].material_bindings;
        const auto first = static_cast<std::uint32_t>(slots.size());
        hero_binding_ranges[asset] = {first, count, 0, 0};
        slots.resize(slots.size() + count, {UINT32_MAX, UINT32_MAX});
        for (const auto &binding : bindings)
        {
            if (binding.slot >= count || binding.hero_ordinal >= kHeroMaterialNames.size())
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid native hero material binding.");
            auto &slot = slots[first + binding.slot];
            (binding.domain == render_data::HeroMaterialDomain::Opaque ? slot.x : slot.y) = binding.hero_ordinal;
        }
        for (std::uint32_t slot = first; slot < slots.size(); ++slot)
            if (slots[slot].x == UINT32_MAX)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Character slot has no opaque hero material.");
    }
    if (slots.empty())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Native hero material table is empty.");
    const auto bytes = slots.size() * sizeof(DirectX::XMUINT2);
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(hero_material_bindings_gpu, allocation, BufferDescription(bytes),
            D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
    uploads.emplace_back();
    allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(uploads.back(), allocation, BufferDescription(bytes),
            D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
    void *mapped{};
    D3D12_RANGE no_read{};
    const auto mapped_result = uploads.back().resource->Map(0, &no_read, &mapped);
    if (FAILED(mapped_result)) return HResultFailure("Map hero material binding upload", mapped_result);
    std::memcpy(mapped, slots.data(), bytes);
    uploads.back().resource->Unmap(0, nullptr);
    list->CopyBufferRegion(hero_material_bindings_gpu.resource.Get(), 0, uploads.back().resource.Get(), 0, bytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {hero_material_bindings_gpu.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &barrier);
    hero_material_bindings_gpu.graph_access = enhanced ? Access::AllShaderRead : Access::Common;
    hero_material_bindings_gpu.graph_queue = QueueHint::Direct;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = static_cast<UINT>(slots.size());
    view.Buffer.StructureByteStride = sizeof(DirectX::XMUINT2);
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + kHeroBindingDescriptorOffset) * srv_stride;
        device->CreateShaderResourceView(hero_material_bindings_gpu.resource.Get(), &view, handle);
    }
    return Result::Success();
}
} // namespace hs
