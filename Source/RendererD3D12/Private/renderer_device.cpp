#include "renderer_impl.hpp"
#include "renderer_reconstruction.hpp"

namespace hs
{

Result D3D12Renderer::Impl::CreateDevice(const RendererConfig &configuration)
{
    config = configuration;
    graphics_capabilities = {};
    graphics_capabilities_key.clear();
    width = std::max(configuration.width, 1u);
    height = std::max(configuration.height, 1u);
    ViewExtent capacity;
    if (auto resolved = ResolveNativeReconstruction(configuration, {width, height},
            reconstruction_plan, capacity); !resolved) return resolved;
    config.temporal_reconstruction = reconstruction_plan.temporal_enabled;
    render_width = capacity.width;
    render_height = capacity.height;
    (void)UpdateViewState(view_state, {width, height}, {render_width, render_height},
                          reconstruction_plan.render_active);
    dynamic_resolution_controller.Reset();

    // Bootstrap every runtime-selectable Streamline feature before any DXGI or
    // device creation, independently of the initial requested settings.
    if (auto started = InitializeReconstructionSessionsBeforeDevice(); !started) return started;

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
    if (auto bound = BindReconstructionSessionsToDevice(); !bound) return bound;
    if (streamline_session && streamline_session->Initialized() &&
        streamline_session->Availability(StreamlineFeature::FrameGeneration) == ReconstructionAvailability::Available)
    {
        streamline_device_proxy = device;
        void *proxy = streamline_device_proxy.Detach();
        const auto upgraded = streamline_session->UpgradeDevice(&proxy);
        streamline_device_proxy.Attach(static_cast<ID3D12Device *>(proxy));
        if (!upgraded)
        {
            OutputDebugStringA((std::string(upgraded.Message()) + "\n").c_str());
            streamline_device_proxy.Reset();
        }
    }
    feature_capabilities.hardware = QueryRendererFeatureHardware(device.Get());
    D3D12_FEATURE_DATA_D3D12_OPTIONS texture_options{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 feedback_options{};
    D3D12_FEATURE_DATA_SHADER_MODEL feedback_shader_model{D3D_SHADER_MODEL_6_5};
    ComPtr<ID3D12Device8> feedback_device;
    texture_streaming_capabilities.sampler_feedback =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &feedback_options, sizeof(feedback_options))) &&
        feedback_options.SamplerFeedbackTier != D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED &&
        SUCCEEDED(device.As(&feedback_device)) &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &feedback_shader_model, sizeof(feedback_shader_model))) &&
        feedback_shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
    texture_streaming_capabilities.tiled_resources = texture_streaming_capabilities.sampler_feedback &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &texture_options, sizeof(texture_options))) &&
        texture_options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2 &&
        texture_options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
    texture_streaming_capabilities.packed_mip_arrays = texture_streaming_capabilities.tiled_resources &&
        texture_options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_4;
    feature_capabilities.rtao_consumer = true;
    feature_capabilities.rtgi_consumer = true;
    feature_capabilities.rt_reflection_consumer = true;
    feature_capabilities.rt_shadow_consumer = true;
    feature_capabilities.software_gi_consumer = QueryBrixelizerNativeSupport(device.Get()).supported;
    // Concrete DeferredLight pixel and adaptive GTAO consumers are integrated;
    // selected-generation product and actual native admission remain separate.
    feature_capabilities.adaptive_raster_consumer = true;
    feature_capabilities.adaptive_compute_consumer = true;
    if (auto cached = pipeline_cache.Initialize(device.Get(), adapter.Get(),
            configuration.artifact_directory.empty() ? std::filesystem::path{} :
                configuration.artifact_directory / "pso-cache"); !cached) return cached;
    if (auto prepared = PrepareReconstructionProvider(config.reconstruction_request
            ? &*config.reconstruction_request : nullptr, config.dynamic_resolution,
            config.dynamic_min_percent, config.dynamic_max_percent, {width, height},
            reconstruction_plan, capacity, reconstruction_provider, &config); !prepared) return prepared;
    if (reconstruction_plan.effective_provider.value == "dlss_rr")
        if (auto r = streamline_session->ConfigureReconstruction(reconstruction_plan.effective_quality,
                {width, height}, true); !r) return r;
    if (reconstruction_provider)
        if (auto configured = reconstruction_provider->ConfigureAfterCompletion(capacity); !configured)
        {
            const auto removed = device->GetDeviceRemovedReason();
            if (FAILED(removed)) return CheckDevice(removed, "Configure reconstruction provider");
            const auto unavailable = reconstruction_provider->Availability();
            const auto reason = std::string(configured.Message());
            if (auto released = reconstruction_provider->ResetAfterCompletion(); !released) return released;
            reconstruction_provider.reset();
            // Targets have not been allocated yet, so initial native context
            // failure can resolve the complete InternalTAAU fallback extent.
            if (auto resolved = ResolveNativeReconstruction(configuration, {width, height},
                    reconstruction_plan, capacity); !resolved) return resolved;
            reconstruction_plan.requested_availability = unavailable;
            reconstruction_plan.fallback = ReconstructionFallback::ProviderUnavailable;
            reconstruction_plan.fallback_reason = reason;
        }
    config.temporal_reconstruction = reconstruction_plan.temporal_enabled;
    render_width = capacity.width;
    render_height = capacity.height;
    (void)UpdateViewState(view_state, {width, height}, capacity, reconstruction_plan.render_active);

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

    if (configuration.surface_frontend == SurfaceFrontend::VisibilityBuffer)
    {
        struct FormatRequirement
        {
            DXGI_FORMAT format;
            std::string_view name;
        };
        constexpr FormatRequirement formats[] = {
            {DXGI_FORMAT_R8G8B8A8_UNORM, "DXGI_FORMAT_R8G8B8A8_UNORM"},
            {DXGI_FORMAT_R16G16_SNORM, "DXGI_FORMAT_R16G16_SNORM"},
            {DXGI_FORMAT_R16G16_FLOAT, "DXGI_FORMAT_R16G16_FLOAT"},
            {DXGI_FORMAT_R32G32_UINT, "DXGI_FORMAT_R32G32_UINT"},
            {DXGI_FORMAT_R32G32_FLOAT, "DXGI_FORMAT_R32G32_FLOAT"},
            {DXGI_FORMAT_R8_UINT, "DXGI_FORMAT_R8_UINT"},
        };
        constexpr auto required_support1 = static_cast<D3D12_FORMAT_SUPPORT1>(
            D3D12_FORMAT_SUPPORT1_TEXTURE2D |
            D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
        constexpr auto required_support2 = D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
        for (const auto &requirement : formats)
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{requirement.format};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support,
                                                   sizeof(support))) ||
                (support.Support1 & required_support1) != required_support1 ||
                (support.Support2 & required_support2) != required_support2)
            {
                return Result::Failure(
                    ErrorCode::InvalidState, "hs_renderer_d3d12",
                    std::format("VisibilityBuffer requires {} with TEXTURE2D, "
                                "TYPED_UNORDERED_ACCESS_VIEW, and UAV_TYPED_STORE support.",
                                requirement.name));
            }
        }
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_5};
    ComPtr<ID3D12Device2> mesh_device;
    mesh_shader_supported =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
            &options7, sizeof(options7))) &&
        options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 &&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
            &shader_model, sizeof(shader_model))) &&
        shader_model.HighestShaderModel >= D3D_SHADER_MODEL_6_5 &&
        SUCCEEDED(device.As(&mesh_device));
    if (configuration.geometry_frontend == GeometryFrontend::MeshShader &&
        !mesh_shader_supported)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Mesh Shader frontend requested but the device lacks mesh tier 1/SM 6.5 support.");
    // Automatic keeps the indexed profile until end-to-end frontend costs are measured.
    mesh_shader_enabled = configuration.geometry_frontend == GeometryFrontend::MeshShader;
    std::ofstream(config.artifact_directory / "adapter.txt", std::ios::app)
        << "mesh_shader_supported=" << mesh_shader_supported << '\n'
        << "geometry_frontend=" << (mesh_shader_enabled ? "mesh" : "indexed") << '\n'
        << "surface_frontend="
        << (configuration.surface_frontend == SurfaceFrontend::VisibilityBuffer ? "visibility"
                                                                                 : "compact")
        << '\n';

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (streamline_device_proxy)
    {
        result = streamline_device_proxy->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&streamline_queue_proxy));
        if (SUCCEEDED(result))
        {
            void *native{};
            if (auto unwrapped = streamline_session->GetNativeInterface(streamline_queue_proxy.Get(), &native); unwrapped)
                queue.Attach(static_cast<ID3D12CommandQueue *>(native));
            else
                streamline_session->ReportPresentationBootstrapFailure(unwrapped.Message());
        }
        else streamline_session->ReportPresentationBootstrapFailure(
            HResultFailure("Create Streamline presentation proxy queue", result).Message());
        if (!queue)
        {
            const auto removed = device->GetDeviceRemovedReason();
            if (FAILED(removed)) return CheckDevice(removed, "Create Streamline presentation proxy queue");
            streamline_queue_proxy.Reset();
            result = device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue));
        }
    }
    else result = device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue));
    if (FAILED(result))
    {
        return HResultFailure("CreateCommandQueue", result);
    }
    if (auto queue_result =
            graph_queue_executor.Initialize(device.Get(), queue.Get(), kFrameCount);
        !queue_result)
    {
        return queue_result;
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

    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareFrameTimestamps(FrameContext &frame, std::uint32_t pass_count)
{
    // Render has already waited this slot's complete producer/reader requirements.
    // A topology change grows only this slot's query storage.
    if (pass_count <= frame.timestamp_capacity) return Result::Success();
    if (pass_count > std::numeric_limits<UINT>::max() / 2)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Timestamp query capacity overflow.");
    D3D12_QUERY_HEAP_DESC description{};
    description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    description.Count = pass_count * 2;
    ComPtr<ID3D12QueryHeap> next_heap;
    auto result = device->CreateQueryHeap(&description, IID_PPV_ARGS(&next_heap));
    if (FAILED(result)) return HResultFailure("Create frame timestamp query heap", result);
    AllocationResource next_readback;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto created = CreateAllocation(next_readback, allocation,
            BufferDescription(static_cast<UINT64>(description.Count) * sizeof(std::uint64_t)),
            D3D12_RESOURCE_STATE_COPY_DEST); !created) return created;
    std::uint64_t *next_mapped{};
    D3D12_RANGE range{0, static_cast<SIZE_T>(description.Count) * sizeof(std::uint64_t)};
    result = next_readback.resource->Map(0, &range, reinterpret_cast<void **>(&next_mapped));
    if (FAILED(result)) return HResultFailure("Map frame timestamp readback", result);
    if (frame.mapped_timestamps)
    {
        D3D12_RANGE no_write{};
        frame.timestamp_readback.resource->Unmap(0, &no_write);
    }
    frame.timestamp_heap = std::move(next_heap);
    frame.timestamp_readback = std::move(next_readback);
    frame.mapped_timestamps = next_mapped;
    frame.timestamp_capacity = pass_count;
    return Result::Success();
}

GpuPassTimings D3D12Renderer::Impl::ReadFrameGpuTimings(const FrameContext &frame) const
{
    GpuPassTimings timings;
    if (!frame.mapped_timestamps || !frame.timestamps_recorded ||
        !frame.completion.IsSatisfiedBy(graph_queue_executor.Completed()))
        return timings;

    timings.fence_completed = true;
    timings.completed_frame_id = frame.submitted_frame_id;
    timings.view_id = frame.submitted_view_id;
    timings.cpu_recording_ms = frame.cpu_recording_ms;
    timings.cpu_recording_valid = frame.cpu_recording_ms_valid &&
        std::isfinite(frame.cpu_recording_ms) && frame.cpu_recording_ms >= 0.0f;
    timings.passes = frame.timestamp_passes;
    if (timings.passes.empty() || timings.passes.size() > frame.timestamp_capacity ||
        frame.timestamp_frequencies.size() != timings.passes.size())
    {
        for (auto &pass : timings.passes) pass.measured = false;
        return timings;
    }

    constexpr std::uint32_t critical_begin_pass = 37;
    constexpr std::uint32_t critical_end_pass = 38;
    std::size_t begin_slot = timings.passes.size(), end_slot = timings.passes.size();
    bool duplicate_endpoint = false;
    bool pass_timings_valid = true;
    struct ClockCalibration
    {
        UINT64 gpu{}, cpu{};
        bool available{};
    };
    std::array<ClockCalibration, 3> calibrations{};
    LARGE_INTEGER qpc_frequency{};
    const bool qpc_available = QueryPerformanceFrequency(&qpc_frequency) && qpc_frequency.QuadPart > 0;
    // GetClockCalibration samples CPU/GPU counters together. No timestamps from
    // separate queue clocks are aligned without this actual native calibration.
    if (qpc_available)
        for (std::size_t index = 0; index < calibrations.size(); ++index)
        {
            auto &clock = calibrations[index];
            auto *native = graph_queue_executor.NativeQueue(static_cast<QueueHint>(index));
            clock.available = native && SUCCEEDED(native->GetClockCalibration(&clock.gpu, &clock.cpu));
        }
    for (std::size_t slot = 0; slot < timings.passes.size(); ++slot)
    {
        auto &pass = timings.passes[slot];
        if (pass.pass_id == critical_begin_pass)
        {
            duplicate_endpoint |= begin_slot != timings.passes.size();
            begin_slot = slot;
        }
        if (pass.pass_id == critical_end_pass)
        {
            duplicate_endpoint |= end_slot != timings.passes.size();
            end_slot = slot;
        }
        if (!pass.measured) continue; // Copy queue timestamps are unsupported.
        const auto begin = frame.mapped_timestamps[slot * 2];
        const auto end = frame.mapped_timestamps[slot * 2 + 1];
        const auto frequency = frame.timestamp_frequencies[slot];
        pass.begin_ticks = begin;
        pass.end_ticks = end;
        pass.timestamp_frequency = frequency;
        if (end < begin || frequency == 0)
        {
            pass.measured = false;
            pass_timings_valid = false;
            continue;
        }
        const auto nanoseconds = static_cast<long double>(end - begin) *
            1'000'000'000.0L / frequency;
        if (!std::isfinite(nanoseconds) ||
            nanoseconds > static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        {
            pass.measured = false;
            pass_timings_valid = false;
            continue;
        }
        pass.nanoseconds = static_cast<std::uint64_t>(nanoseconds);
        const auto queue_index = static_cast<std::size_t>(pass.queue);
        if (queue_index < calibrations.size() && calibrations[queue_index].available)
        {
            const auto &clock = calibrations[queue_index];
            const auto aligned = [&](UINT64 tick) {
                return static_cast<long double>(clock.cpu) +
                    (static_cast<long double>(tick) - static_cast<long double>(clock.gpu)) *
                    static_cast<long double>(qpc_frequency.QuadPart) / frequency;
            };
            const auto aligned_begin = aligned(begin), aligned_end = aligned(end);
            if (std::isfinite(aligned_begin) && std::isfinite(aligned_end) &&
                aligned_begin >= 0 && aligned_end >= aligned_begin &&
                aligned_end <= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            {
                pass.begin_qpc = static_cast<std::uint64_t>(aligned_begin);
                pass.end_qpc = static_cast<std::uint64_t>(aligned_end);
                pass.qpc_frequency = static_cast<std::uint64_t>(qpc_frequency.QuadPart);
                pass.clock_calibrated = true;
            }
        }
    }
    // Preserve independent pass diagnostics even when the critical-path sample
    // is unavailable. DRS requires critical_path_valid as well as completion.
    timings.valid = pass_timings_valid;
    if (!timings.valid || duplicate_endpoint || begin_slot == timings.passes.size() ||
        end_slot == timings.passes.size())
        return timings;

    const auto &begin_pass = timings.passes[begin_slot];
    const auto &end_pass = timings.passes[end_slot];
    if (begin_pass.queue != RenderQueue::Direct || end_pass.queue != RenderQueue::Direct ||
        !begin_pass.measured || !end_pass.measured)
        return timings;
    UINT64 direct_frequency{};
    if (!queue || FAILED(queue->GetTimestampFrequency(&direct_frequency)) || direct_frequency == 0 ||
        frame.timestamp_frequencies[begin_slot] != direct_frequency ||
        frame.timestamp_frequencies[end_slot] != direct_frequency)
        return timings;

    // These endpoints share the Direct queue clock. Explicit graph edges bracket
    // every render queue, so this interval includes cross-queue overlap and waits.
    const auto begin = frame.mapped_timestamps[begin_slot * 2];
    const auto end = frame.mapped_timestamps[end_slot * 2 + 1];
    if (end <= begin) return timings;
    timings.critical_path_ms = static_cast<double>(
        static_cast<long double>(end - begin) * 1000.0L / direct_frequency);
    timings.critical_path_valid = std::isfinite(timings.critical_path_ms) &&
        timings.critical_path_ms > 0.0;
    timings.gpu_limited = timings.critical_path_valid && timings.cpu_recording_valid &&
        timings.critical_path_ms >= timings.cpu_recording_ms;
    return timings;
}

Result D3D12Renderer::Impl::WaitForFrame(FrameContext &frame)
{
    if (static_scene_fence_unreliable)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "A submitted frame has no reliable completion fence.");
    auto completion = frame.completion;
    completion.Merge(frame.skin_cache_use_completion);
    return graph_queue_executor.Wait(completion);
}

void D3D12Renderer::Impl::InitializeCanonicalDescriptors(ID3D12DescriptorHeap *heap) const noexcept
{
    D3D12_SHADER_RESOURCE_VIEW_DESC empty{};
    empty.Format = DXGI_FORMAT_R32_UINT;
    empty.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    empty.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    empty.Buffer.NumElements = 1;
    auto destination = heap->GetCPUDescriptorHandleForHeapStart();
    const auto count = heap->GetDesc().NumDescriptors;
    const auto stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // SRV is the default for texture/geometry slots. Root tables also declare
    // CBVs and UAVs even when their feature is inactive; initialize those types
    // below before a complete-bank copy can publish or clone the table.
    for (UINT index = 0; index < count; ++index)
    {
        device->CreateShaderResourceView(nullptr, &empty, destination);
        destination.ptr += stride;
    }

    // These are the non-SRV slots of root table32 in CreatePipelines. Active
    // services replace the nulls with their native resource views, retaining
    // the existing waited frame-bank leases and immutable pass suffixes.
    constexpr std::array cbv_offsets{
        kViewDescriptorOffset + 9,
        kRayDescriptorOffset, kRayTransportDescriptorOffset,
        kVolumetricDescriptorOffset, kRayShadowDescriptorOffset,
        kRayReconstructionDescriptorOffset, kBrixelizerDescriptorOffset,
        kDecalSurfaceDescriptorOffset + kDecalSurfaceCbvOffset};
    struct UavRange { UINT offset, count; };
    constexpr std::array uav_ranges{
        UavRange{kVisibilityUavDescriptorOffset, kVisibilityUavDescriptorCount},
        UavRange{kViewDescriptorOffset + 7, 2},
        UavRange{kRayDescriptorOffset + 16, 5},
        UavRange{kRayTransportDescriptorOffset + 27, 11},
        UavRange{kRayTransportDescriptorOffset + 40, 5},
        UavRange{kVolumetricDescriptorOffset + 15, 9},
        UavRange{kRayShadowDescriptorOffset + 15, 9},
        UavRange{kRayReconstructionDescriptorOffset + 8, 15},
        UavRange{kBrixelizerDescriptorOffset + 16, 9},
        UavRange{kTextureFeedbackDescriptorOffset + 1, kTextureFeedbackBindingCount},
        UavRange{kDecalSurfaceDescriptorOffset + kDecalSurfaceUavOffset, kDecalSurfaceUavCount}};
    D3D12_UNORDERED_ACCESS_VIEW_DESC empty_uav{};
    empty_uav.Format = DXGI_FORMAT_R32_UINT;
    empty_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    empty_uav.Buffer.NumElements = 1;
    const auto heap_start = heap->GetCPUDescriptorHandleForHeapStart();
    static_assert((kTextureDescriptorCount - kRayGeometryTableOffset) % kRayFrameTableCount == 0);
    for (UINT frame = 0; frame < count / kTextureDescriptorCount; ++frame)
    {
        const auto frame_offset = static_cast<SIZE_T>(frame) * kTextureDescriptorCount;
        const auto cpu = [&](UINT offset) {
            auto handle = heap_start;
            handle.ptr += (frame_offset + offset) * stride;
            return handle;
        };
        for (const auto offset : cbv_offsets)
            device->CreateConstantBufferView(nullptr, cpu(offset));
        auto empty_skin = empty;
        empty_skin.Format = DXGI_FORMAT_UNKNOWN;
        empty_skin.Buffer.StructureByteStride = sizeof(DirectX::XMFLOAT4X4);
        device->CreateShaderResourceView(nullptr, &empty_skin, cpu(kViewDescriptorOffset + 10));
        for (const auto range : uav_ranges)
            for (UINT index = 0; index < range.count; ++index)
                device->CreateUnorderedAccessView(nullptr, nullptr, &empty_uav, cpu(range.offset + index));

        // Every private ray/GI/volume/RR bank uses the same root32 layout. Seed
        // it from the typed canonical table; builders later copy current
        // canonical values and overwrite their active service suffix only.
        for (UINT offset = kRayGeometryTableOffset; offset < kTextureDescriptorCount;
             offset += kRayFrameTableCount)
            device->CopyDescriptorsSimple(kRayFrameTableCount, cpu(offset),
                cpu(kTemporalTextureDescriptorOffset), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Root table36 has an independent immutable CBV/SRV/UAV table per hero
        // transparency phase, including phases not selected by this frame.
        for (UINT phase = 0; phase < kHeroTransparencyPhaseCount; ++phase)
        {
            const auto offset = kHeroTransparencyDescriptorOffset + phase * kHeroTransparencyTableSize;
            device->CreateConstantBufferView(nullptr, cpu(offset));
            device->CreateUnorderedAccessView(nullptr, nullptr, &empty_uav, cpu(offset + 22));
        }
    }
}
void D3D12Renderer::Impl::PublishFrameDescriptors(std::uint32_t slot) const noexcept
{
    auto source = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    auto destination = srv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto offset = static_cast<SIZE_T>(slot) * kTextureDescriptorCount * srv_stride;
    source.ptr += offset; destination.ptr += offset;
    device->CopyDescriptorsSimple(kTextureDescriptorCount, destination, source,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

Result D3D12Renderer::Impl::WaitForGpu()
{
    // Utility upload/capture work uses a separate Direct fence; destruction
    // additionally requires the graph's Compute and Copy timelines to drain.
    if (graph_queue_executor.NativeQueue(QueueHint::Direct))
        if (auto result = graph_queue_executor.Drain(); !result) return result;
    // Initialization may fail after creating a provider but before utility
    // queue/fence creation. No utility submission can exist in that state.
    if (!queue || !fence) return Result::Success();
    const auto value = next_fence++;
    auto result = queue->Signal(fence.Get(), value);
    if (FAILED(result))
    {
        return CheckDevice(result, "Queue signal");
    }
    auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        return CheckDevice(DXGI_ERROR_DEVICE_REMOVED, "Read utility completion fence");
    if (completed < value)
    {
        result = fence->SetEventOnCompletion(value, fence_event);
        if (FAILED(result))
        {
            return HResultFailure("SetEventOnCompletion", result);
        }
        const auto waited = WaitForSingleObject(fence_event, INFINITE);
        if (waited != WAIT_OBJECT_0)
            return HResultFailure("Wait for utility completion fence",
                waited == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL);
    }
    if (device && FAILED(device->GetDeviceRemovedReason()))
        return CheckDevice(device->GetDeviceRemovedReason(), "Complete utility queue drain");
    completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX || completed < value)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Utility queue drain has no reliable post-work fence completion.");
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
        const auto texture_access = [](D3D12_RESOURCE_STATES state) {
            if (state == D3D12_RESOURCE_STATE_PRESENT)
                return D3D12_BARRIER_ACCESS_NO_ACCESS;
            auto access = D3D12_BARRIER_ACCESS_COMMON;
            if ((state & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0)
                access |= D3D12_BARRIER_ACCESS_RENDER_TARGET;
            if ((state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0)
                access |= D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            if ((state & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0)
                access |= D3D12_BARRIER_ACCESS_COPY_SOURCE;
            if ((state & D3D12_RESOURCE_STATE_COPY_DEST) != 0)
                access |= D3D12_BARRIER_ACCESS_COPY_DEST;
            if ((state & D3D12_RESOURCE_STATE_DEPTH_READ) != 0)
                access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
            if ((state & D3D12_RESOURCE_STATE_DEPTH_WRITE) != 0)
                access |= D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
            if ((state & D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE) != 0)
                access |= D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE;
            if ((state & (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) != 0)
                access |= D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
            return access;
        };
        D3D12_TEXTURE_BARRIER barrier{};
        barrier.SyncBefore = before == D3D12_RESOURCE_STATE_PRESENT ? D3D12_BARRIER_SYNC_NONE
                                                                    : D3D12_BARRIER_SYNC_ALL;
        barrier.SyncAfter = after == D3D12_RESOURCE_STATE_PRESENT ? D3D12_BARRIER_SYNC_NONE
                                                                  : D3D12_BARRIER_SYNC_ALL;
        barrier.AccessBefore = texture_access(before);
        barrier.AccessAfter = texture_access(after);
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
