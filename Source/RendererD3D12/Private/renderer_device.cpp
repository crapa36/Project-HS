#include "renderer_impl.hpp"

namespace hs
{

Result D3D12Renderer::Impl::CreateDevice(const RendererConfig &configuration)
{
    config = configuration;
    width = std::max(configuration.width, 1u);
    height = std::max(configuration.height, 1u);
    const auto render_scale = std::clamp(configuration.render_scale_percent, 75u, 100u);
    render_width = std::max(width * render_scale / 100u, 1u);
    render_height = std::max(height * render_scale / 100u, 1u);

    const auto com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_initialized = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE)
    {
        return HResultFailure("CoInitializeEx", com_result);
    }

    if (configuration.validation)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            if (configuration.gpu_validation)
            {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(&debug1)))
                {
                    debug1->SetEnableGPUBasedValidation(TRUE);
                }
            }
        }
    }

    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred_settings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred_settings))))
    {
        dred_settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred_enabled = true;
    }
    std::ofstream(config.artifact_directory / "dred_config.json", std::ios::trunc)
        << std::format(
               "{{\"enabled\":{},\"auto_breadcrumbs\":{},\"page_fault\":{},"
               "\"breadcrumb_context\":{}}}\n",
               dred_enabled ? "true" : "false", dred_enabled ? "true" : "false",
               dred_enabled ? "true" : "false", dred_enabled ? "true" : "false");

    const auto factory_flags = configuration.validation ? DXGI_CREATE_FACTORY_DEBUG : 0u;
    auto result = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory));
    if (FAILED(result))
    {
        return HResultFailure("CreateDXGIFactory2", result);
    }

    if (configuration.warp)
    {
        ComPtr<IDXGIAdapter> warp;
        result = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
        if (SUCCEEDED(result))
        {
            result = warp.As(&adapter);
        }
    }
    else
    {
        for (UINT index = 0;
             factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 IID_PPV_ARGS(&adapter)) !=
             DXGI_ERROR_NOT_FOUND;
             ++index)
        {
            DXGI_ADAPTER_DESC3 description{};
            adapter->GetDesc3(&description);
            if (!(description.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                            __uuidof(ID3D12Device), nullptr)))
            {
                break;
            }
            adapter.Reset();
        }
        result = adapter ? S_OK : DXGI_ERROR_NOT_FOUND;
    }
    if (FAILED(result) || !adapter)
    {
        return HResultFailure("SelectAdapter", result);
    }

    DXGI_ADAPTER_DESC3 adapter_description{};
    adapter->GetDesc3(&adapter_description);
    LARGE_INTEGER driver_version{};
    const auto has_driver = SUCCEEDED(
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version));
    const auto driver = static_cast<std::uint64_t>(driver_version.QuadPart);
    std::ofstream(config.artifact_directory / "adapter.txt", std::ios::trunc)
        << Utf8(adapter_description.Description) << '\n'
        << (has_driver
                ? std::format("{}.{}.{}.{}", (driver >> 48) & 0xFFFF,
                              (driver >> 32) & 0xFFFF, (driver >> 16) & 0xFFFF,
                              driver & 0xFFFF)
                : std::string("unknown"))
        << '\n';

    result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(result))
    {
        return HResultFailure("D3D12CreateDevice FL12_0", result);
    }

    if (configuration.validation)
    {
        ComPtr<ID3D12InfoQueue> info_queue;
        if (SUCCEEDED(device.As(&info_queue)))
        {
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    const auto supports_enhanced =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                              sizeof(options12))) &&
        options12.EnhancedBarriersSupported;
    if (configuration.barrier_mode == BarrierMode::Enhanced && !supports_enhanced)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Enhanced barriers requested but unsupported.");
    }
    enhanced = supports_enhanced && configuration.barrier_mode != BarrierMode::Legacy;

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result = device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue));
    if (FAILED(result))
    {
        return HResultFailure("CreateCommandQueue", result);
    }

    D3D12MA::ALLOCATOR_DESC allocator_description{};
    allocator_description.pDevice = device.Get();
    allocator_description.pAdapter = adapter.Get();
    result = D3D12MA::CreateAllocator(&allocator_description, &allocator);
    if (FAILED(result))
    {
        return HResultFailure("D3D12MA::CreateAllocator", result);
    }

    for (auto &frame : frames)
    {
        result =
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        if (FAILED(result))
        {
            return HResultFailure("CreateCommandAllocator", result);
        }

        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        const auto upload_description = BufferDescription(kFrameUploadSize);
        if (auto created = CreateAllocation(frame.upload, upload_allocation, upload_description,
                                            D3D12_RESOURCE_STATE_GENERIC_READ);
            !created)
        {
            return created;
        }
        D3D12_RANGE no_read{};
        result = frame.upload.resource->Map(0, &no_read, reinterpret_cast<void **>(&frame.mapped));
        if (FAILED(result))
        {
            return HResultFailure("Map frame upload", result);
        }
    }

    result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       frames[0].allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&command_list));
    if (FAILED(result))
    {
        return HResultFailure("CreateCommandList", result);
    }
    if (enhanced && FAILED(command_list.As(&enhanced_command_list)))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Enhanced barrier command list interface unavailable.");
    }
    command_list->Close();

    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result))
    {
        return HResultFailure("CreateFence", result);
    }
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "CreateEventW failed.");
    }

    D3D12_QUERY_HEAP_DESC query_description{};
    query_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_description.Count = kFrameCount * kTimestampCountPerFrame;
    result = device->CreateQueryHeap(&query_description, IID_PPV_ARGS(&timestamp_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create timestamp query heap", result);
    }
    D3D12MA::ALLOCATION_DESC readback_allocation{};
    readback_allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto created = CreateAllocation(
            timestamp_readback, readback_allocation,
            BufferDescription(static_cast<std::uint64_t>(query_description.Count) *
                              sizeof(std::uint64_t)),
            D3D12_RESOURCE_STATE_COPY_DEST);
        !created)
    {
        return created;
    }
    D3D12_RANGE read_range{
        0, static_cast<SIZE_T>(query_description.Count) * sizeof(std::uint64_t)};
    result = timestamp_readback.resource->Map(
        0, &read_range, reinterpret_cast<void **>(&mapped_timestamps));
    if (FAILED(result))
    {
        return HResultFailure("Map timestamp readback", result);
    }
    result = queue->GetTimestampFrequency(&timestamp_frequency);
    if (FAILED(result) || timestamp_frequency == 0)
    {
        return HResultFailure("GetTimestampFrequency", FAILED(result) ? result : E_FAIL);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::WaitForFrame(FrameContext &frame)
{
    if (frame.fence_value && fence->GetCompletedValue() < frame.fence_value)
    {
        auto result = fence->SetEventOnCompletion(frame.fence_value, fence_event);
        if (FAILED(result))
        {
            return HResultFailure("SetEventOnCompletion", result);
        }
        WaitForSingleObject(fence_event, INFINITE);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::WaitForGpu()
{
    const auto value = next_fence++;
    auto result = queue->Signal(fence.Get(), value);
    if (FAILED(result))
    {
        return CheckDevice(result, "Queue signal");
    }
    if (fence->GetCompletedValue() < value)
    {
        result = fence->SetEventOnCompletion(value, fence_event);
        if (FAILED(result))
        {
            return HResultFailure("SetEventOnCompletion", result);
        }
        WaitForSingleObject(fence_event, INFINITE);
    }
    return Result::Success();
}

void D3D12Renderer::Impl::TransitionTexture(ID3D12Resource *resource,
                                            D3D12_RESOURCE_STATES before,
                                            D3D12_RESOURCE_STATES after,
                                            D3D12_BARRIER_LAYOUT before_layout,
                                            D3D12_BARRIER_LAYOUT after_layout)
{
    if (enhanced)
    {
        D3D12_TEXTURE_BARRIER barrier{};
        barrier.SyncBefore = before == D3D12_RESOURCE_STATE_PRESENT ? D3D12_BARRIER_SYNC_NONE
                                                                    : D3D12_BARRIER_SYNC_ALL;
        barrier.SyncAfter = after == D3D12_RESOURCE_STATE_PRESENT ? D3D12_BARRIER_SYNC_NONE
                                                                  : D3D12_BARRIER_SYNC_ALL;
        barrier.AccessBefore =
            before == D3D12_RESOURCE_STATE_PRESENT
                ? D3D12_BARRIER_ACCESS_NO_ACCESS
                : (before == D3D12_RESOURCE_STATE_RENDER_TARGET
                       ? D3D12_BARRIER_ACCESS_RENDER_TARGET
                       : (before == D3D12_RESOURCE_STATE_COPY_SOURCE
                              ? D3D12_BARRIER_ACCESS_COPY_SOURCE
                              : (before == D3D12_RESOURCE_STATE_COPY_DEST
                                     ? D3D12_BARRIER_ACCESS_COPY_DEST
                                     : (before == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                            ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE
                                            : D3D12_BARRIER_ACCESS_COMMON))));
        barrier.AccessAfter =
            after == D3D12_RESOURCE_STATE_PRESENT
                ? D3D12_BARRIER_ACCESS_NO_ACCESS
                : (after == D3D12_RESOURCE_STATE_RENDER_TARGET
                       ? D3D12_BARRIER_ACCESS_RENDER_TARGET
                       : (after == D3D12_RESOURCE_STATE_COPY_SOURCE
                              ? D3D12_BARRIER_ACCESS_COPY_SOURCE
                              : (after == D3D12_RESOURCE_STATE_COPY_DEST
                                     ? D3D12_BARRIER_ACCESS_COPY_DEST
                                     : (after == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                            ? D3D12_BARRIER_ACCESS_SHADER_RESOURCE
                                            : D3D12_BARRIER_ACCESS_COMMON))));
        barrier.LayoutBefore = before_layout;
        barrier.LayoutAfter = after_layout;
        barrier.pResource = resource;
        const auto description = resource->GetDesc();
        barrier.Subresources = {0, description.MipLevels, 0, description.DepthOrArraySize, 0, 1};
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1};
        group.pTextureBarriers = &barrier;
        enhanced_command_list->Barrier(1, &group);
    }
    else
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        command_list->ResourceBarrier(1, &barrier);
    }
}

void D3D12Renderer::Impl::TransitionBuffer(ID3D12Resource *resource,
                                           D3D12_RESOURCE_STATES before,
                                           D3D12_RESOURCE_STATES after,
                                           D3D12_BARRIER_SYNC before_sync,
                                           D3D12_BARRIER_SYNC after_sync,
                                           D3D12_BARRIER_ACCESS before_access,
                                           D3D12_BARRIER_ACCESS after_access)
{
    if (enhanced)
    {
        D3D12_BUFFER_BARRIER barrier{};
        barrier.SyncBefore = before_sync;
        barrier.SyncAfter = after_sync;
        barrier.AccessBefore = before_access;
        barrier.AccessAfter = after_access;
        barrier.pResource = resource;
        barrier.Offset = 0;
        barrier.Size = UINT64_MAX;
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
        group.pBufferBarriers = &barrier;
        enhanced_command_list->Barrier(1, &group);
    }
    else
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        command_list->ResourceBarrier(1, &barrier);
    }
}


} // namespace hs
