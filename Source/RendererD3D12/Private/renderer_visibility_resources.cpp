#include "renderer_impl.hpp"

namespace hs
{
Result D3D12Renderer::Impl::PrepareVisibilityTargets(OptionTargets &targets,
    std::uint32_t target_width, std::uint32_t target_height)
{
    if (config.surface_frontend != SurfaceFrontend::VisibilityBuffer) return Result::Success();
    const auto pixels = static_cast<std::uint64_t>(target_width) * target_height;
    if (pixels == 0 || pixels >= UINT32_MAX)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Visibility extent exceeds its pixel index ABI.");
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    AllocationResource next_ids, next_front_face, next_pixels, next_counts, next_dispatch_args;
    for (auto [target, format] : {
            std::pair{&next_ids, DXGI_FORMAT_R32G32_UINT},
            std::pair{&next_front_face, DXGI_FORMAT_R8_UINT}})
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = target_width; desc.Height = target_height;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = format; desc.SampleDesc = {1, 0};
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (auto result = CreateAllocation(*target, allocation, desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS); !result) return result;
        target->graph_access = enhanced ? Access::Common : Access::UnorderedWrite;
    }
    for (auto [target, bytes] : {
            std::pair{&next_pixels, pixels * sizeof(std::uint32_t)},
            std::pair{&next_counts, std::uint64_t{kVisibilityVariantCount} * 3 * 4},
            std::pair{&next_dispatch_args, std::uint64_t{kVisibilityVariantCount} * 12}})
    {
        if (auto result = CreateAllocation(*target, allocation,
                BufferDescription(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_COPY_DEST); !result) return result;
        target->graph_access = Access::CopyDest;
    }
    targets.visibility_ids = std::move(next_ids);
    targets.visibility_front_face = std::move(next_front_face);
    targets.visibility_pixels = std::move(next_pixels);
    targets.visibility_counts = std::move(next_counts);
    targets.visibility_dispatch_args = std::move(next_dispatch_args);
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateVisibilityTargets()
{
    if (config.surface_frontend != SurfaceFrontend::VisibilityBuffer) return Result::Success();
    OptionTargets targets;
    if (auto result = PrepareVisibilityTargets(targets, render_width, render_height); !result)
        return result;
    using std::swap;
    swap(visibility_ids, targets.visibility_ids);
    swap(visibility_front_face, targets.visibility_front_face);
    swap(visibility_pixels, targets.visibility_pixels);
    swap(visibility_counts, targets.visibility_counts);
    swap(visibility_dispatch_args, targets.visibility_dispatch_args);
    visibility_targets_initialized = false;
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kVisibilityRtvOffset) * rtv_stride;
    device->CreateRenderTargetView(visibility_ids.resource.Get(), nullptr, rtv);
    rtv.ptr += rtv_stride;
    device->CreateRenderTargetView(visibility_front_face.resource.Get(), nullptr, rtv);
    return Result::Success();
}

Result D3D12Renderer::Impl::WriteVisibilityDescriptors(std::uint32_t frame_slot,
    ID3D12Resource *static_scene, ID3D12Resource *dynamic_scene,
    std::uint64_t static_ref_offset, std::uint32_t static_ref_count,
    std::uint64_t dynamic_ref_offset, std::uint32_t dynamic_ref_count)
{
    if (frame_slot >= kFrameCount || !static_scene || !dynamic_scene)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Visibility descriptors have an invalid frame or scene.");
    auto &frame = frames[frame_slot];
    auto &visibility = visibility_frames[frame_slot];
    auto descriptor = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    descriptor.ptr += static_cast<SIZE_T>(frame_slot * kTextureDescriptorCount +
                                         kVisibilitySrvDescriptorOffset) * srv_stride;
    const auto raw_srv = [&](ID3D12Resource *resource) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / 4);
        view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += srv_stride;
    };
    const auto structured_srv = [&](ID3D12Resource *resource,
            std::uint64_t offset = 0, std::uint32_t count = 0) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.FirstElement = offset / 16;
        view.Buffer.NumElements = count ? count :
            static_cast<UINT>((resource->GetDesc().Width - offset) / 16);
        view.Buffer.StructureByteStride = 16;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += srv_stride;
    };
    const auto texture_srv = [&](ID3D12Resource *resource) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = resource->GetDesc().Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &view, descriptor);
        descriptor.ptr += srv_stride;
    };
    raw_srv(visibility.raster_records.resource.Get());
    structured_srv(visibility.ranges_upload.resource.Get());
    raw_srv(static_scene); raw_srv(dynamic_scene);
    structured_srv(frame.static_visible_refs.resource.Get());
    structured_srv(frame.dynamic_visible_refs.resource.Get());
    structured_srv(frame.static_retest_refs.resource.Get());
    structured_srv(frame.upload.resource.Get(), static_ref_offset, static_ref_count);
    structured_srv(frame.upload.resource.Get(), dynamic_ref_offset, dynamic_ref_count);
    texture_srv(visibility_ids.resource.Get());
    texture_srv(visibility_front_face.resource.Get());
    raw_srv(visibility_pixels.resource.Get()); raw_srv(visibility_counts.resource.Get());
    if (cluster_runtime && cluster_runtime->frames[frame_slot].refs.resource)
        structured_srv(cluster_runtime->frames[frame_slot].refs.resource.Get());
    else
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.NumElements = 1;
        view.Buffer.StructureByteStride = sizeof(GpuStaticDrawRef);
        device->CreateShaderResourceView(nullptr, &view, descriptor);
    }

    descriptor = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    descriptor.ptr += static_cast<SIZE_T>(frame_slot * kTextureDescriptorCount +
                                         kVisibilityUavDescriptorOffset) * srv_stride;
    const auto raw_uav = [&](ID3D12Resource *resource) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / 4);
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(resource, nullptr, &view, descriptor);
        descriptor.ptr += srv_stride;
    };
    raw_uav(visibility.raster_records.resource.Get());
    for (auto *resource : {gbuffer_base.resource.Get(), gbuffer_normal.resource.Get(),
            gbuffer_metadata.resource.Get(), gbuffer_material.resource.Get(),
            gbuffer_motion.resource.Get(), gbuffer_surface_history.resource.Get(),
            visibility_ids.resource.Get(), visibility_front_face.resource.Get()})
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = resource->GetDesc().Format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource, nullptr, &view, descriptor);
        descriptor.ptr += srv_stride;
    }
    raw_uav(visibility_pixels.resource.Get()); raw_uav(visibility_counts.resource.Get());
    raw_uav(visibility_dispatch_args.resource.Get());
    return Result::Success();
}
} // namespace hs
