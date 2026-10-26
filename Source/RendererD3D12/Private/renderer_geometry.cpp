#include "renderer_impl.hpp"
#include "renderer_geometry.hpp"

namespace hs
{
Result D3D12Renderer::Impl::BeginGeometryDrawRecords(std::uint32_t frame_slot)
{
    if (frame_slot >= kFrameCount)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Geometry records have an invalid frame slot.");
    auto &frame = frames[frame_slot];
    constexpr auto bytes = static_cast<std::uint64_t>(kGeometryDrawRecordCapacity) *
        sizeof(GpuGeometryDrawRecord);
    if (!frame.geometry_draw_records.resource)
    {
        AllocationResource records, upload;
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto result = CreateAllocation(records, allocation,
                BufferDescription(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_COPY_DEST); !result) return result;
        records.graph_access = Access::CopyDest;
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto result = CreateAllocation(upload, allocation, BufferDescription(bytes),
                D3D12_RESOURCE_STATE_GENERIC_READ); !result) return result;
        frame.geometry_draw_records = std::move(records);
        frame.geometry_draw_records_upload = std::move(upload);
    }
    frame.geometry_draw_records_cpu.clear();
    frame.geometry_draw_records_cpu.reserve(kGeometryDrawRecordCapacity);
    frame.geometry_draw_records_cpu.emplace_back();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Buffer.NumElements = static_cast<UINT>(bytes / 4);
    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    auto descriptor = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    descriptor.ptr += static_cast<SIZE_T>(frame_slot * kTextureDescriptorCount +
                                         kGeometryDescriptorOffset) * srv_stride;
    device->CreateShaderResourceView(frame.geometry_draw_records.resource.Get(), &view, descriptor);
    return Result::Success();
}

Result D3D12Renderer::Impl::AddGeometryDrawRecord(
    std::uint32_t frame_slot, const GeometryPageGpu &page, std::uint32_t visible_base,
    std::uint32_t instance_count, std::uint32_t flags, std::uint32_t view,
    std::uint32_t mesh_id, std::uint32_t &packed_pass)
{
    if (frame_slot >= kFrameCount || view > 3 || page.page_slot >= kGeometryPageCapacity ||
        !page.storage.resource || (flags & ~(kGeometryGpuSceneRefs | kGeometryMeshEligible |
            kGeometryStaticInstances | kGeometryPreviousPage | kGeometryBuiltin |
            kGeometryExplicitMaterialBinding)) != 0 ||
        static_cast<std::uint64_t>(visible_base) + instance_count > UINT32_MAX)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Geometry record has an invalid page, view or instance range.");
    auto &records = frames[frame_slot].geometry_draw_records_cpu;
    if (records.empty() || records.size() >= kGeometryDrawRecordCapacity)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry record capacity exceeded or frame was not begun.");
    if ((flags & kGeometryMeshEligible) != 0 &&
        !GeometryMeshDispatchFits(page.meshlet_count, instance_count))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Geometry mesh dispatch exceeds the hardware group limit.");
    packed_pass = (static_cast<std::uint32_t>(records.size()) << 2) | view;
    if (page.raster_index_offset != 0) flags |= kGeometryRasterPrimitiveRemap;
    if (previous_geometry_page_versions[page.page_slot] == page.generation)
        flags |= kGeometryPreviousPage;
    records.push_back({page.page_slot, 0, page.index_count, 0,
        page.meshlet_count, visible_base, instance_count, flags,
        page.triangle_offset, page.remap_offset, page.generation, page.metadata.lod,
        mesh_id, std::bit_cast<std::uint32_t>(page.metadata.geometric_error), 0, 0});
    return Result::Success();
}

Result D3D12Renderer::Impl::UploadGeometryDrawRecords(
    ID3D12GraphicsCommandList *list, std::uint32_t frame_slot)
{
    if (!list || frame_slot >= kFrameCount)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Geometry upload has an invalid command list or frame.");
    auto &frame = frames[frame_slot];
    if (!frame.geometry_draw_records.resource || !frame.geometry_draw_records_upload.resource ||
        frame.geometry_draw_records_cpu.empty())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry records were not prepared before upload.");
    void *mapped{};
    const D3D12_RANGE read_range{0, 0};
    const auto result = frame.geometry_draw_records_upload.resource->Map(0, &read_range, &mapped);
    if (FAILED(result)) return HResultFailure("Map geometry draw records", result);
    const auto bytes = frame.geometry_draw_records_cpu.size() * sizeof(GpuGeometryDrawRecord);
    std::memcpy(mapped, frame.geometry_draw_records_cpu.data(), bytes);
    const D3D12_RANGE written_range{0, bytes};
    frame.geometry_draw_records_upload.resource->Unmap(0, &written_range);
    list->CopyBufferRegion(frame.geometry_draw_records.resource.Get(), 0,
                          frame.geometry_draw_records_upload.resource.Get(), 0, bytes);
    return Result::Success();
}

bool D3D12Renderer::Impl::GeometryUsesMesh(ID3D12PipelineState *indexed_pipeline,
    const GeometryPageGpu &page, std::uint32_t instance_capacity) const noexcept
{
    if (!mesh_shader_enabled || !GeometryMeshDispatchFits(page.meshlet_count, instance_capacity))
        return false;
    for (const auto &pair : mesh_pipeline_pairs)
        if (pair.indexed == indexed_pipeline) return pair.mesh != nullptr;
    return false;
}

Result D3D12Renderer::Impl::DrawGeometry(ID3D12GraphicsCommandList *list,
    std::uint32_t frame_slot, ID3D12PipelineState *indexed_pipeline,
    const GeometryPageGpu &page, std::uint32_t packed_pass,
    ID3D12Resource *arguments, std::uint64_t argument_offset)
{
    const auto id = packed_pass >> 2;
    if (!list || !indexed_pipeline || frame_slot >= kFrameCount || id == 0 ||
        id >= frames[frame_slot].geometry_draw_records_cpu.size())
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Geometry draw has an invalid record or argument range.");
    if (arguments)
    {
        const auto bytes = arguments->GetDesc().Width;
        if (argument_offset > bytes || bytes - argument_offset < kGeometryArgumentStride)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "Geometry draw has an invalid argument range.");
    }
    const auto &record = frames[frame_slot].geometry_draw_records_cpu[id];
    if (record.page_slot != page.page_slot || record.geometry_generation != page.generation)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Geometry draw record does not match its resident page.");
    list->SetGraphicsRoot32BitConstant(6, packed_pass, 0);
    if ((record.flags & kGeometryMeshEligible) != 0)
    {
        for (const auto &pair : mesh_pipeline_pairs)
            if (pair.indexed == indexed_pipeline && pair.mesh)
            {
                list->SetPipelineState(pair.mesh.Get());
                if (arguments)
                    list->ExecuteIndirect(mesh_geometry_signature.Get(), 1, arguments,
                                          argument_offset, nullptr, 0);
                else
                {
                    ComPtr<ID3D12GraphicsCommandList6> mesh_list;
                    const auto queried = list->QueryInterface(IID_PPV_ARGS(&mesh_list));
                    if (FAILED(queried)) return HResultFailure("Query mesh draw command list", queried);
                    const auto groups = record.meshlet_count * record.instance_count;
                    mesh_list->DispatchMesh(std::min(groups, kGeometryMeshDispatchDimension),
                        groups == 0 ? 1 : (groups + kGeometryMeshDispatchDimension - 1) /
                            kGeometryMeshDispatchDimension, 1);
                }
                return Result::Success();
            }
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Mesh geometry record has no compatible mesh pipeline.");
    }
    list->SetPipelineState(indexed_pipeline);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->IASetVertexBuffers(0, 1, &page.vertex_view);
    list->IASetIndexBuffer(&page.index_view);
    if (arguments)
        list->ExecuteIndirect(indexed_geometry_signature.Get(), 1, arguments,
                              argument_offset, nullptr, 0);
    else
        list->DrawIndexedInstanced(record.index_count, record.instance_count,
                                  record.first_index, 0, 0);
    return Result::Success();
}
} // namespace hs
