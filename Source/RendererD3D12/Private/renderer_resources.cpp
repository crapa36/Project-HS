#include "renderer_impl.hpp"

#include <hs/render_data/texture_package.hpp>

#include <exception>
#include <stdexcept>

namespace hs
{
namespace
{
constexpr D3D12_RESOURCE_STATES kAllShaderRead =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

Result CheckOitFormats(ID3D12Device *device)
{
    for (const auto format : {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16_FLOAT})
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        const auto queried = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                                                         &support, sizeof(support));
        if (FAILED(queried)) return HResultFailure("Query OIT target format support", queried);
        const auto required = D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                              D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
                              D3D12_FORMAT_SUPPORT1_BLENDABLE |
                              D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE;
        if ((support.Support1 & required) != required)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                std::format("OIT target format {} lacks texture, render, blend or sample support.",
                    static_cast<unsigned>(format)));
    }
    return Result::Success();
}
} // namespace

Result D3D12Renderer::Impl::CreateAllocation(AllocationResource &output,
                                            const D3D12MA::ALLOCATION_DESC &allocation,
                                            const D3D12_RESOURCE_DESC &resource,
                                            D3D12_RESOURCE_STATES initial_state,
                                            const D3D12_CLEAR_VALUE *clear)
{
    if (auto admitted = AdmitResidentAllocation(resource, allocation.HeapType); !admitted)
        return admitted;
    const auto effective_state =
        enhanced && resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
            ? D3D12_RESOURCE_STATE_COMMON
            : initial_state;
    const auto result = allocator->CreateResource(&allocation, &resource, effective_state, clear,
                                                  &output.allocation,
                                                  IID_PPV_ARGS(output.resource.ReleaseAndGetAddressOf()));
    if (FAILED(result)) return CheckDevice(result, std::format(
        "D3D12MA::CreateResource dimension={} extent={}x{} mips={} format={} flags={} heap={}",
        static_cast<unsigned>(resource.Dimension), resource.Width, resource.Height, resource.MipLevels,
        static_cast<unsigned>(resource.Format), static_cast<unsigned>(resource.Flags),
        static_cast<unsigned>(allocation.HeapType)));
    RecordResidentAllocation(output, allocation.HeapType);
    return Result::Success();
}

Result D3D12Renderer::Impl::ResolvePresentation(const RendererConfig &candidate, ViewExtent output,
    ViewExtent capacity, const DisplayState &display, std::optional<bool> cadence_suppression)
{
    auto request = candidate.presentation_request;
    const bool suppressed = request.mode == GraphicsFrameGenerationMode::Auto &&
        cadence_suppression.value_or(auto_fg_cadence_suppressed);
    if (suppressed || !candidate.frame_generation_allowed) request.mode = GraphicsFrameGenerationMode::Off;
    if (request.mode != GraphicsFrameGenerationMode::Off &&
        (candidate.presentation_request.provider.value == "auto" || candidate.presentation_request.provider.value == "fsr") &&
        !fsr_session)
    {
        fsr_session = std::make_unique<FsrSession>();
#if HS_WITH_FSR
        if (auto bound = fsr_session->Initialize(device.Get()); !bound) return bound;
#endif
    }
    auto result = sdk_presentation.Resolve(device.Get(), request, streamline_session.get(),
        fsr_session.get(), {capacity, output, display.format, display.color_space, candidate.vsync,
            candidate.dynamic_resolution, static_cast<float>(candidate.target_render_fps)}, true);
    if (result) sdk_presentation.ReportFrameGenerationPolicy(candidate.presentation_request, suppressed, candidate.frame_generation_allowed);
    return result;
}

Result D3D12Renderer::Impl::CreatePresentationChain(const RendererConfig &candidate, ViewExtent output,
    DisplayState &display)
{
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = output.width; desc.Height = output.height; desc.Format = display.format;
    desc.SampleDesc = {1, 0}; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kFrameCount; desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (sdk_presentation.PresentationOwner() == SdkPresentationOwner::Streamline)
    {
        if (!streamline_queue_proxy)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "DLSS-G presentation device/queue proxy bootstrap is unavailable.");
        if (auto hr = factory.As(&streamline_factory_proxy); FAILED(hr))
            return HResultFailure("Query presentation factory", hr);
        void *proxy = streamline_factory_proxy.Detach();
        const auto upgraded = sdk_presentation.UpgradeCreationFactory(&proxy);
        streamline_factory_proxy.Attach(static_cast<IDXGIFactory2 *>(proxy));
        if (!upgraded) return upgraded;
        ComPtr<IDXGISwapChain1> chain;
        auto hr = streamline_factory_proxy->CreateSwapChainForHwnd(streamline_queue_proxy.Get(),
            static_cast<HWND>(candidate.window), &desc, nullptr, nullptr, &chain);
        if (FAILED(hr)) return HResultFailure("Create Streamline swap-chain proxy", hr);
        hr = chain.As(&swap_chain);
        if (FAILED(hr)) return HResultFailure("Query Streamline swap chain", hr);
        if (auto attached = sdk_presentation.AttachSLChain(swap_chain.Get(), queue.Get()); !attached) return attached;
    }
    else if (sdk_presentation.PresentationOwner() != SdkPresentationOwner::None)
    {
        if (auto created = sdk_presentation.CreateSwapChain(queue.Get(), factory.Get(),
                static_cast<HWND>(candidate.window), desc, display.color_space, swap_chain); !created) return created;
    }
    else
    {
        ComPtr<IDXGISwapChain1> chain;
        auto hr = factory->CreateSwapChainForHwnd(queue.Get(), static_cast<HWND>(candidate.window),
            &desc, nullptr, nullptr, &chain);
        if (FAILED(hr)) return HResultFailure("Create native swap chain", hr);
        hr = chain.As(&swap_chain);
        if (FAILED(hr)) return HResultFailure("Query native swap chain", hr);
    }
    return ApplyDisplayState(swap_chain.Get(), display);
}

Result D3D12Renderer::Impl::RecreatePresentation(const RendererConfig &candidate, ViewExtent output,
    ViewExtent capacity, DisplayState &display, std::optional<bool> cadence_suppression)
{
    const auto old_config = config;
    const auto old_display = display_state;
    const ViewExtent old_output{width, height};
    const auto old_capacity = view_state.render_capacity;
    ComPtr<ID3D12PipelineState> sdr_pipeline, hdr_pipeline;
    if (old_display.format == DXGI_FORMAT_R8G8B8A8_UNORM) sdr_pipeline = ui_pipeline;
    else hdr_pipeline = ui_pipeline;
    if (!sdr_pipeline)
        if (auto prepared = PrepareDisplayPipeline(candidate, DXGI_FORMAT_R8G8B8A8_UNORM, sdr_pipeline); !prepared) return prepared;
    if (display.format == DXGI_FORMAT_R10G10B10A2_UNORM && !hdr_pipeline)
        if (auto prepared = PrepareDisplayPipeline(candidate, display.format, hdr_pipeline); !prepared) return prepared;
    const auto release_chain = [&]() -> Result {
        if (auto drained = sdk_presentation.DrainBeforeSwapChainMutation(); !drained) return drained;
        if (auto drained = WaitForGpu(); !drained) return drained;
        if (auto released = graph_queue_executor.ReleaseRecordersAfterCompletion(); !released) return released;
        graph.Reset(); // Release imported back buffers and unsubmitted callbacks.
        for (auto &buffer : back_buffers) buffer.Reset();
        swap_chain.Reset();
        return sdk_presentation.DestroyAfterGpuCompletion(true);
    };
    const auto acquire = [&]() -> Result {
        auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (auto &buffer : back_buffers)
        {
            const auto index = static_cast<UINT>(&buffer - back_buffers.data());
            if (auto hr = swap_chain->GetBuffer(index, IID_PPV_ARGS(&buffer)); FAILED(hr))
                return HResultFailure("Acquire recreated presentation buffer", hr);
            device->CreateRenderTargetView(buffer.Get(), nullptr, handle); handle.ptr += rtv_stride;
        }
        return Result::Success();
    };
    const auto create = [&](const RendererConfig &settings, ViewExtent extent, ViewExtent render,
                            DisplayState &state, std::optional<bool> suppression = std::nullopt) -> Result {
        if (auto resolved = ResolvePresentation(settings, extent, render, state, suppression); !resolved) return resolved;
        const auto requested_format = state.format;
        if (auto created = CreatePresentationChain(settings, extent, state); !created) return created;
        if (state.format != requested_format)
        {
            // DXGI negotiated SDR. The SDK context must use that exact format.
            if (auto released = release_chain(); !released) return released;
            if (auto resolved = ResolvePresentation(settings, extent, render, state, suppression); !resolved) return resolved;
            if (auto created = CreatePresentationChain(settings, extent, state); !created) return created;
        }
        return acquire();
    };
    if (auto released = release_chain(); !released)
    {
        initialized = false;
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            std::format("Presentation retirement failed; the native chain cannot be mutated: {}", released.Message()));
    }
    auto attempted = create(candidate, output, capacity, display, cadence_suppression);
    PresentationGuidesCandidate guides;
    if (attempted) attempted = PreparePresentationGuides(candidate, output, capacity, display, guides);
    if (attempted) attempted = sdk_presentation.IsEnabled()
        ? presentation_guides.Commit(std::move(guides), graph_queue_executor.Completed())
        : presentation_guides.ResetAfterCompletion(graph_queue_executor.Completed());
    if (attempted)
    {
        display_state = display;
        ui_pipeline = display.format == DXGI_FORMAT_R10G10B10A2_UNORM ? std::move(hdr_pipeline) : std::move(sdr_pipeline);
        back_buffer_completion = {};
        return attempted;
    }
    const auto failure = std::string(attempted.Message());
    auto restored_display = old_display;
    auto restored = release_chain();
    if (restored) restored = create(old_config, old_output, old_capacity, restored_display);
    if (!restored)
    {
        initialized = false;
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            std::format("{}; restoring the previous presentation failed: {}", failure, restored.Message()));
    }
    display_state = restored_display;
    return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12", failure);
}

Result D3D12Renderer::Impl::CreateSwapChainAndTargets()
{
    DisplayState candidate_display;
    if (auto queried = QueryDisplayState(adapter.Get(), static_cast<HWND>(config.window),
                                         config, candidate_display); !queried)
        return queried;
    if (auto resolved = ResolvePresentation(config, {width, height}, view_state.render_capacity, candidate_display); !resolved) return resolved;
    const auto requested_format = candidate_display.format;
    if (auto created = CreatePresentationChain(config, {width, height}, candidate_display); !created) return created;
    if (candidate_display.format != requested_format)
    {
        // PRESENT support is known only after creating a swap chain. Negotiate
        // its SDR buffer format before any back buffer, RTV, or PSO is published.
        if (auto drained = sdk_presentation.DrainBeforeSwapChainMutation(); !drained) return drained;
        swap_chain.Reset();
        if (auto destroyed = sdk_presentation.DestroyAfterGpuCompletion(true); !destroyed) return destroyed;
        if (auto resolved = ResolvePresentation(config, {width, height}, view_state.render_capacity, candidate_display); !resolved) return resolved;
        if (auto created = CreatePresentationChain(config, {width, height}, candidate_display); !created) return created;
    }
    display_state = std::move(candidate_display);
    factory->MakeWindowAssociation(static_cast<HWND>(config.window), DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
    rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_description.NumDescriptors = kLightingAuxRtvOffset + kLightingAuxRtvCount;
    auto result = device->CreateDescriptorHeap(&rtv_description, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create RTV heap", result);
    }
    rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        result = swap_chain->GetBuffer(index, IID_PPV_ARGS(&back_buffers[index]));
        if (FAILED(result))
        {
            return HResultFailure("Get swap-chain buffer", result);
        }
        device->CreateRenderTargetView(back_buffers[index].Get(), nullptr, handle);
        handle.ptr += rtv_stride;
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareLightingAuxiliaries(std::array<AllocationResource *, 3> targets,
    ViewExtent capacity, std::array<bool, 3> required)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = capacity.width; texture.Height = capacity.height;
    texture.DepthOrArraySize = texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = texture.Format;
    for (std::size_t index = 0; index < targets.size(); ++index)
        if (required[index])
        {
            if (auto created = CreateAllocation(*targets[index], allocation, texture,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear); !created) return created;
            targets[index]->graph_access = enhanced ? Access::Common : Access::ShaderRead;
        }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreatePostProcessTargets()
{
    if (auto checked = CheckOitFormats(device.Get()); !checked) return checked;
    transient_textures_common = enhanced;
    gbuffer_base.Reset();
    gbuffer_normal.Reset();
    gbuffer_metadata.Reset();
    gbuffer_material.Reset();
    gbuffer_motion.Reset();
    gbuffer_surface_history.Reset();
    for (auto &sidecar : material_sidecars) sidecar.Reset();
    visibility_ids.Reset(); visibility_front_face.Reset();
    visibility_pixels.Reset(); visibility_counts.Reset(); visibility_dispatch_args.Reset();
    hdr_color.Reset();
    subsurface_diffuse.Reset();
    environment_specular.Reset();
    indirect_diffuse.Reset();
    oit_accumulation.Reset();
    oit_revealage.Reset();
    post_a.Reset();
    post_b.Reset();
    for (auto &history : temporal_history) history.Reset();
    for (auto &history : temporal_normal_history) history.Reset();
    for (auto &history : temporal_material_history) history.Reset();
    scene_work.Reset();
    display_linear.Reset();
    composite_hdr.Reset();
    for (auto &history : hzb_history) history.Reset();
    temporal_history_valid = false;
    temporal_targets_initialized = false;
    view_targets_initialized = false;
    material_sidecars_initialized = false;
    temporal_read_index = 0;
    hzb_history_valid = false;
    hzb_read_index = 0;
    hzb_layout = MakeHzbLayout(render_width, render_height);

    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    auto create_target = [&](AllocationResource &output, DXGI_FORMAT format,
                             const float *clear_color,
                             std::uint32_t target_width = 0,
                             std::uint32_t target_height = 0) -> Result {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = target_width ? target_width : render_width;
        description.Height = target_height ? target_height : render_height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc = {1, 0};
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer &&
            (&output == &gbuffer_base || &output == &gbuffer_normal ||
             &output == &gbuffer_metadata || &output == &gbuffer_material ||
             &output == &gbuffer_motion || &output == &gbuffer_surface_history))
            description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        std::copy_n(clear_color, 4, clear.Color);
        return CreateAllocation(output, allocation, description,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear);
    };

    constexpr float black[4] = {0, 0, 0, 0};
    constexpr float normal_clear[4] = {0, 0, 0, 0};
    constexpr float metadata_clear[4] = {0, 0, 0, 0};
    constexpr float reveal_clear[4] = {1, 1, 1, 1};
    for (auto [resource, format, clear] : {
             std::tuple{&gbuffer_base, DXGI_FORMAT_R8G8B8A8_UNORM, black},
             std::tuple{&gbuffer_normal, DXGI_FORMAT_R16G16_SNORM, normal_clear},
             std::tuple{&gbuffer_metadata, DXGI_FORMAT_R32G32_UINT, metadata_clear},
             std::tuple{&hdr_color, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&oit_accumulation, DXGI_FORMAT_R32G32B32A32_FLOAT, black},
             std::tuple{&oit_revealage, DXGI_FORMAT_R16_FLOAT, reveal_clear},
             std::tuple{&gbuffer_material, DXGI_FORMAT_R8G8B8A8_UNORM, black},
             std::tuple{&gbuffer_motion, DXGI_FORMAT_R16G16_FLOAT, black},
             std::tuple{&gbuffer_surface_history, DXGI_FORMAT_R32G32_FLOAT, black},
         })
    {
        if (auto created = create_target(*resource, format, clear); !created)
        {
            return created;
        }
    }
    if (auto created = PrepareLightingAuxiliaries({&subsurface_diffuse, &environment_specular, &indirect_diffuse},
            {render_width, render_height}, RequiredLightingAuxiliaries(config)); !created) return created;
    if (auto created = create_target(post_a, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                     black, width, height); !created) return created;
    if (config.bloom || config.outline)
        if (auto created = create_target(post_b, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                         black, width, height); !created) return created;
    const bool native_resolve = UsesInternalResolveTargets(reconstruction_plan);
    if (native_resolve)
    for (auto *resource : {&temporal_history[0], &temporal_history[1]})
        if (auto created = create_target(*resource, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                         black, width, height); !created)
            return created;
    OptionTargets view_targets;
    if (auto created = PrepareViewTargets(view_targets, render_width, render_height,
                                          width, height, native_resolve); !created)
        return created;
    temporal_normal_history.swap(view_targets.temporal_normal_history);
    temporal_material_history.swap(view_targets.temporal_material_history);
    using std::swap;
    swap(scene_work, view_targets.scene_work);
    swap(display_linear, view_targets.display_linear);
    swap(composite_hdr, view_targets.composite_hdr);
    if (!exposure_values[0].resource)
        if (auto created = PrepareExposureResources(); !created) return created;
    if (auto created = PrepareMaterialSidecars(material_sidecars, render_width, render_height); !created)
        return created;
    if (hzb_layout.elements >
        std::numeric_limits<std::uint64_t>::max() / sizeof(float))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "HZB buffer size overflow.");
    for (auto &history : hzb_history)
        if (auto created = CreateAllocation(
                history, allocation,
                BufferDescription(hzb_layout.elements * sizeof(float),
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            !created)
            return created;
    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * rtv_stride;
    for (auto [resource, format] : {
             std::pair{gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM},
             std::pair{gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16_SNORM},
             std::pair{gbuffer_metadata.resource.Get(), DXGI_FORMAT_R32G32_UINT},
             std::pair{hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{oit_accumulation.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT},
             std::pair{oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT},
             std::pair{post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{gbuffer_material.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM},
             std::pair{gbuffer_motion.resource.Get(), DXGI_FORMAT_R16G16_FLOAT},
         })
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource, &view, rtv);
        rtv.ptr += rtv_stride;
    }
    rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kLightingAuxRtvOffset) * rtv_stride;
    for (auto *resource : {subsurface_diffuse.resource.Get(), environment_specular.resource.Get(), indirect_diffuse.resource.Get()})
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        // An unused fixed MRT slot uses a null RTV, which discards shader writes.
        device->CreateRenderTargetView(resource, &view, rtv);
        rtv.ptr += rtv_stride;
    }
    rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kTemporalRtvOffset) * rtv_stride;
    for (auto &history : temporal_history)
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(history.resource.Get(), &view, rtv);
        rtv.ptr += rtv_stride;
    }
    rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kSurfaceHistoryRtvOffset) * rtv_stride;
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32G32_FLOAT;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(gbuffer_surface_history.resource.Get(), &view, rtv);
    }

    if (!srv_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = kTextureDescriptorCount * kFrameCount;
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const auto result = device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&srv_heap));
        if (FAILED(result))
        {
            return HResultFailure("Create SRV heap", result);
        }
        srv_stride =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!srv_cpu_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = kTextureDescriptorCount * kFrameCount;
        const auto result = device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&srv_cpu_heap));
        if (FAILED(result)) return HResultFailure("Create canonical CPU descriptor heap", result);
        InitializeCanonicalDescriptors(srv_cpu_heap.Get());
    }

    if (auto created = CreateVisibilityTargets(); !created) return created;
    for (std::uint32_t frame_index = 0; frame_index < kFrameCount; ++frame_index)
    {
        const auto frame_offset = static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount;
        const auto srv_start = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        const auto write_post_table = [&](std::uint32_t offset) {
            auto srv = srv_start;
            srv.ptr += (frame_offset + offset) * srv_stride;
            const auto create_srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(resource, &view, srv);
                srv.ptr += srv_stride;
            };
             create_srv(gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            create_srv(gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16_SNORM);
            create_srv(gbuffer_metadata.resource.Get(), DXGI_FORMAT_R32G32_UINT);
            create_srv(shadow.resource.Get(), DXGI_FORMAT_R32_FLOAT);
            create_srv(hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            create_srv(oit_accumulation.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
            create_srv(oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT);
            create_srv(post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            create_srv(post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            create_srv(ui_textures[frame_index].resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
        };
        write_post_table(0);
        write_post_table(kToneMapInputDescriptorOffset);
        write_post_table(kFxaaInputDescriptorOffset);
        auto srv = srv_start;
        srv.ptr += (frame_offset +
                    kPostTextureDescriptorCount + kCharacterDescriptorCount +
                    kMonsterPbrDescriptorCount + kEnvironmentDescriptorCount - 2) * srv_stride;
        const auto create_srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resource, &view, srv);
            srv.ptr += srv_stride;
        };
        create_srv(gbuffer_material.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        create_srv(gbuffer_motion.resource.Get(), DXGI_FORMAT_R16G16_FLOAT);
        srv = srv_start;
        srv.ptr += (frame_offset +
                    kBloomTextureDescriptorOffset) * srv_stride;
        // The frame graph resolves these slots after Prepare. Keep valid null
        // descriptors while bloom is disabled and before the first bloom frame.
        create_srv(nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
        srv = srv_start;
        srv.ptr += (frame_offset +
                    kTemporalTextureDescriptorOffset) * srv_stride;
        create_srv(temporal_history[0].resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        create_srv(temporal_history[1].resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        srv = srv_start;
        srv.ptr += (frame_offset + kSurfaceDescriptorOffset) * srv_stride;
        create_srv(depth.resource.Get(), DXGI_FORMAT_R32_FLOAT);
        create_srv(gbuffer_surface_history.resource.Get(), DXGI_FORMAT_R32G32_FLOAT);
    }
    WriteMaterialSidecarDescriptors();
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
        WriteViewDescriptors(frame, temporal_read_index, 1u - temporal_read_index,
                             exposure_read_index, 1u - exposure_read_index, 0);
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateDepthAndShadow()
{
    transient_textures_common = enhanced;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_description{};
    dsv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_description.NumDescriptors = 5;
    auto result = device->CreateDescriptorHeap(&dsv_description, IID_PPV_ARGS(&dsv_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create DSV heap", result);
    }
    dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil = {0.0f, 0};
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depth_description{};
    depth_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_description.Width = render_width;
    depth_description.Height = render_height;
    depth_description.DepthOrArraySize = 1;
    depth_description.MipLevels = 1;
    depth_description.Format = DXGI_FORMAT_R32_TYPELESS;
    depth_description.SampleDesc = {1, 0};
    depth_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (auto created = CreateAllocation(depth, allocation, depth_description,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        !created)
    {
        return created;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
    depth_view.Format = DXGI_FORMAT_D32_FLOAT;
    depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    depth_view.Texture2D.MipSlice = 0;
    auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depth.resource.Get(), &depth_view, dsv);

    auto shadow_description = depth_description;
    shadow_description.Format = DXGI_FORMAT_R32_TYPELESS;
    const auto base_shadow = std::clamp(config.shadow_resolution, 1024u, 2048u);
    shadow_description.Width = base_shadow * 2u;
    shadow_description.Height = base_shadow * 2u;
    shadow_description.DepthOrArraySize = 1;
    if (auto created = CreateAllocation(shadow, allocation, shadow_description,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        !created)
    {
        return created;
    }
    auto shadow_handle = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    shadow_handle.ptr += dsv_stride;
    for (std::uint32_t cascade = 0; cascade < 3; ++cascade)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = kDepthFormat;
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;

        device->CreateDepthStencilView(shadow.resource.Get(), &view, shadow_handle);
        shadow_handle.ptr += dsv_stride;
    }
    depth_view.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    dsv.ptr += static_cast<SIZE_T>(4) * dsv_stride;
    device->CreateDepthStencilView(depth.resource.Get(), &depth_view, dsv);
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareOptionTargets(OptionTargets &targets,
                                                 std::uint32_t target_width,
                                                 std::uint32_t target_height,
                                                 std::uint32_t shadow_resolution,
                                                 bool bloom,
                                                 std::uint32_t output_width,
                                                 std::uint32_t output_height,
                                                 bool native_resolve, bool outline,
                                                 std::array<bool, 3> lighting_auxiliaries)
{
    if (auto checked = CheckOitFormats(device.Get()); !checked) return checked;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = target_width;
    description.Height = target_height;
    description.DepthOrArraySize = description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
    depth_clear.DepthStencil = {0.0f, 0};
    if (auto result = CreateAllocation(targets.depth, allocation, description,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear); !result)
        return result;
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.Width = shadow_resolution * 2u;
    description.Height = shadow_resolution * 2u;
    if (auto result = CreateAllocation(targets.shadow, allocation, description,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear); !result)
        return result;

    constexpr float black[4] = {0, 0, 0, 0};
    constexpr float normal_clear[4] = {0, 0, 0, 0};
    constexpr float metadata_clear[4] = {0, 0, 0, 0};
    constexpr float reveal_clear[4] = {1, 1, 1, 1};
    const auto color_target = [&](AllocationResource &output, DXGI_FORMAT format,
                                  const float *color,
                                  std::uint32_t resource_width = 0,
                                  std::uint32_t resource_height = 0) -> Result {
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = resource_width ? resource_width : target_width;
        texture.Height = resource_height ? resource_height : target_height;
        texture.DepthOrArraySize = texture.MipLevels = 1;
        texture.Format = format;
        texture.SampleDesc.Count = 1;
        texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer &&
            (&output == &targets.gbuffer_base || &output == &targets.gbuffer_normal ||
             &output == &targets.gbuffer_metadata || &output == &targets.gbuffer_material ||
             &output == &targets.gbuffer_motion || &output == &targets.gbuffer_surface_history))
            texture.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        std::copy_n(color, 4, clear.Color);
        return CreateAllocation(output, allocation, texture,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear);
    };
    for (auto [resource, format, clear] : {
             std::tuple{&targets.gbuffer_base, DXGI_FORMAT_R8G8B8A8_UNORM, black},
             std::tuple{&targets.gbuffer_normal, DXGI_FORMAT_R16G16_SNORM, normal_clear},
             std::tuple{&targets.gbuffer_metadata, DXGI_FORMAT_R32G32_UINT, metadata_clear},
             std::tuple{&targets.hdr_color, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&targets.oit_accumulation, DXGI_FORMAT_R32G32B32A32_FLOAT, black},
             std::tuple{&targets.oit_revealage, DXGI_FORMAT_R16_FLOAT, reveal_clear},
             std::tuple{&targets.gbuffer_material, DXGI_FORMAT_R8G8B8A8_UNORM, black},
             std::tuple{&targets.gbuffer_motion, DXGI_FORMAT_R16G16_FLOAT, black},
             std::tuple{&targets.gbuffer_surface_history, DXGI_FORMAT_R32G32_FLOAT, black},
         })
         if (auto result = color_target(*resource, format, clear); !result) return result;

    if (auto result = PrepareLightingAuxiliaries({&targets.subsurface_diffuse, &targets.environment_specular,
            &targets.indirect_diffuse}, {target_width, target_height}, lighting_auxiliaries); !result) return result;
    if (auto result = color_target(targets.post_a, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   black, output_width, output_height); !result) return result;
    if (bloom || outline)
        if (auto result = color_target(targets.post_b, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       black, output_width, output_height); !result) return result;
    if (native_resolve)
    for (auto *resource : {&targets.temporal_history[0], &targets.temporal_history[1]})
        if (auto result = color_target(*resource, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      black, output_width, output_height); !result)
            return result;
    if (auto result = PrepareViewTargets(targets, target_width, target_height,
                                         output_width, output_height, native_resolve); !result)
        return result;

    if (auto result = PrepareMaterialSidecars(targets.material_sidecars, target_width, target_height); !result)
        return result;
    if (auto result = PrepareVisibilityTargets(targets, target_width, target_height); !result)
        return result;

    targets.hzb_layout = MakeHzbLayout(target_width, target_height);
    if (targets.hzb_layout.elements >
        std::numeric_limits<std::uint64_t>::max() / sizeof(float))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "HZB buffer size overflow.");
    for (auto &history : targets.hzb_history)
        if (auto result = CreateAllocation(
                history, allocation,
                BufferDescription(targets.hzb_layout.elements * sizeof(float),
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            !result)
            return result;

    return PrepareOptionPools(targets.transient_pools, target_width, target_height, bloom,
                              output_width, output_height);
}

Result D3D12Renderer::Impl::PrepareOptionPools(
    std::array<TransientResourcePool, kFrameCount> &pools,
    std::uint32_t target_width, std::uint32_t target_height, bool bloom,
    std::uint32_t output_width, std::uint32_t output_height)
{
    const auto half_width = output_width / 2 + output_width % 2;
    const auto half_height = output_height / 2 + output_height % 2;
    const auto quarter_width = half_width / 2 + half_width % 2;
    const auto quarter_height = half_height / 2 + half_height % 2;
    constexpr auto transient_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    constexpr auto transient_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (auto &pool : pools)
    {
        if (auto result = pool.Initialize(allocator, device.Get()); !result) return result;
        if (auto result = pool.ReserveTexture(
                {target_width, target_height, transient_format, transient_flags}); !result)
            return result; // Distortion is present in every frame.
        if (bloom)
            for (const auto [width, height] : {
                     // Output-sized bloom follows the completed render-sized distortion.
                     std::pair{quarter_width, quarter_height},
                     std::pair{half_width, half_height}})
                if (auto result = pool.ReserveTexture(
                        {width, height, transient_format, transient_flags}); !result)
                    return result;
    }
    return Result::Success();
}

void D3D12Renderer::Impl::CommitOptionTargets(OptionTargets &targets) noexcept
{
    hdr_capture_source.Reset();
    hdr_capture_valid = false;
    using std::swap;
    swap(depth, targets.depth);
    swap(shadow, targets.shadow);
    swap(gbuffer_base, targets.gbuffer_base);
    swap(gbuffer_normal, targets.gbuffer_normal);
    swap(gbuffer_metadata, targets.gbuffer_metadata);
    swap(gbuffer_material, targets.gbuffer_material);
    swap(gbuffer_motion, targets.gbuffer_motion);
    swap(gbuffer_surface_history, targets.gbuffer_surface_history);
    swap(material_sidecars, targets.material_sidecars);
    swap(hdr_color, targets.hdr_color);
    swap(subsurface_diffuse, targets.subsurface_diffuse);
    swap(environment_specular, targets.environment_specular);
    swap(indirect_diffuse, targets.indirect_diffuse);
    swap(oit_accumulation, targets.oit_accumulation);
    swap(oit_revealage, targets.oit_revealage);
    swap(post_a, targets.post_a);
    swap(post_b, targets.post_b);
    swap(scene_work, targets.scene_work);
    swap(display_linear, targets.display_linear);
    swap(composite_hdr, targets.composite_hdr);
    for (std::size_t i = 0; i < temporal_history.size(); ++i)
        swap(temporal_history[i], targets.temporal_history[i]);
    swap(temporal_normal_history, targets.temporal_normal_history);
    swap(temporal_material_history, targets.temporal_material_history);
    for (std::size_t i = 0; i < hzb_history.size(); ++i)
        swap(hzb_history[i], targets.hzb_history[i]);
    if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
    {
        swap(visibility_ids, targets.visibility_ids);
        swap(visibility_front_face, targets.visibility_front_face);
        swap(visibility_pixels, targets.visibility_pixels);
        swap(visibility_counts, targets.visibility_counts);
        swap(visibility_dispatch_args, targets.visibility_dispatch_args);
        visibility_targets_initialized = false;
    }
    hzb_layout = targets.hzb_layout;
    hzb_history_valid = false;
    hzb_read_index = 0;
    temporal_history_valid = false;
    temporal_read_index = 0;
    temporal_targets_initialized = false;
    view_targets_initialized = false;
    material_sidecars_initialized = false;
    SwapOptionPools(targets.transient_pools);
    WriteOptionTargetDescriptors();
}

void D3D12Renderer::Impl::SwapOptionPools(
    std::array<TransientResourcePool, kFrameCount> &pools) noexcept
{
    for (std::size_t i = 0; i < transient_pools.size(); ++i)
        transient_pools[i].Swap(pools[i]);
}

void D3D12Renderer::Impl::WriteOptionTargetDescriptors() noexcept
{
    auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
    depth_view.Format = DXGI_FORMAT_D32_FLOAT;
    depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    depth_view.Texture2D.MipSlice = 0;
    device->CreateDepthStencilView(depth.resource.Get(), &depth_view, dsv);
    dsv.ptr += dsv_stride;
    D3D12_DEPTH_STENCIL_VIEW_DESC shadow_view{};
    shadow_view.Format = kDepthFormat;
    shadow_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    for (std::uint32_t cascade = 0; cascade < 3; ++cascade)
    {
        device->CreateDepthStencilView(shadow.resource.Get(), &shadow_view, dsv);
        dsv.ptr += dsv_stride;
    }
    depth_view.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    dsv.ptr += static_cast<SIZE_T>(4) * dsv_stride;
    device->CreateDepthStencilView(depth.resource.Get(), &depth_view, dsv);

    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * rtv_stride;
    for (auto [resource, format] : {
             std::pair{gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM},
             std::pair{gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16_SNORM},
             std::pair{gbuffer_metadata.resource.Get(), DXGI_FORMAT_R32G32_UINT},
             std::pair{hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{oit_accumulation.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT},
             std::pair{oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT},
             std::pair{post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{gbuffer_material.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM},
             std::pair{gbuffer_motion.resource.Get(), DXGI_FORMAT_R16G16_FLOAT},
         })
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource, &view, rtv);
        rtv.ptr += rtv_stride;
    }
    rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kLightingAuxRtvOffset) * rtv_stride;
    for (auto *resource : {subsurface_diffuse.resource.Get(), environment_specular.resource.Get(), indirect_diffuse.resource.Get()})
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource, &view, rtv);
        rtv.ptr += rtv_stride;
    }
    rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kTemporalRtvOffset) * rtv_stride;
    for (auto &history : temporal_history)
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(history.resource.Get(), &view, rtv);
        rtv.ptr += rtv_stride;
    }
    rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kSurfaceHistoryRtvOffset) * rtv_stride;
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32G32_FLOAT;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(gbuffer_surface_history.resource.Get(), &view, rtv);
    }
    if (config.surface_frontend == SurfaceFrontend::VisibilityBuffer)
    {
        rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(kVisibilityRtvOffset) * rtv_stride;
        device->CreateRenderTargetView(visibility_ids.resource.Get(), nullptr, rtv);
        rtv.ptr += rtv_stride;
        device->CreateRenderTargetView(visibility_front_face.resource.Get(), nullptr, rtv);
    }

    for (std::uint32_t frame_index = 0; frame_index < kFrameCount; ++frame_index)
    {
        const auto frame_offset = static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount;
        const auto srv_start = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        const auto write_post_table = [&](std::uint32_t offset) {
            auto srv = srv_start;
            srv.ptr += (frame_offset + offset) * srv_stride;
            const auto create_srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(resource, &view, srv);
                srv.ptr += srv_stride;
            };
            create_srv(gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
            create_srv(gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16_SNORM);
            create_srv(gbuffer_metadata.resource.Get(), DXGI_FORMAT_R32G32_UINT);
            create_srv(shadow.resource.Get(), DXGI_FORMAT_R32_FLOAT);
            create_srv(hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            create_srv(oit_accumulation.resource.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
            create_srv(oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT);
            create_srv(post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            create_srv(post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
            create_srv(ui_textures[frame_index].resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
        };
        write_post_table(0);
        write_post_table(kToneMapInputDescriptorOffset);
        write_post_table(kFxaaInputDescriptorOffset);
        auto srv = srv_start;
        srv.ptr += (frame_offset + kPostTextureDescriptorCount +
                    kCharacterDescriptorCount + kMonsterPbrDescriptorCount +
                    kEnvironmentDescriptorCount - 2) * srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(gbuffer_material.resource.Get(), &view, srv);
        srv.ptr += srv_stride;
        view.Format = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateShaderResourceView(gbuffer_motion.resource.Get(), &view, srv);
        srv = srv_start;
        srv.ptr += (frame_offset + kTemporalTextureDescriptorOffset) * srv_stride;
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateShaderResourceView(temporal_history[0].resource.Get(), &view, srv);
        srv.ptr += srv_stride;
        device->CreateShaderResourceView(temporal_history[1].resource.Get(), &view, srv);
        srv = srv_start;
        srv.ptr += (frame_offset + kSurfaceDescriptorOffset) * srv_stride;
        view.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(depth.resource.Get(), &view, srv);
        srv.ptr += srv_stride;
        view.Format = DXGI_FORMAT_R32G32_FLOAT;
        device->CreateShaderResourceView(gbuffer_surface_history.resource.Get(), &view, srv);
    }
    ClearTransientDescriptors();
    WriteMaterialSidecarDescriptors();
    for (std::uint32_t frame = 0; frame < kFrameCount; ++frame)
        WriteViewDescriptors(frame, temporal_read_index, 1u - temporal_read_index,
                             exposure_read_index, 1u - exposure_read_index, 0);
}

void D3D12Renderer::Impl::ClearTransientDescriptors() noexcept
{
    D3D12_RENDER_TARGET_VIEW_DESC rtv_view{};
    rtv_view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rtv_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_view{};
    srv_view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_view.Texture2D.MipLevels = 1;
    const auto rtv_start = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto srv_start = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t frame_index = 0; frame_index < kFrameCount; ++frame_index)
    {
        auto rtv = rtv_start;
        rtv.ptr += static_cast<SIZE_T>(kDistortionRtvOffset + frame_index) * rtv_stride;
        device->CreateRenderTargetView(nullptr, &rtv_view, rtv);
        for (std::uint32_t slot = 0; slot < 3; ++slot)
        {
            rtv = rtv_start;
            rtv.ptr += static_cast<SIZE_T>(kBloomRtvOffset + 3 * frame_index + slot) * rtv_stride;
            device->CreateRenderTargetView(nullptr, &rtv_view, rtv);
        }
        auto srv = srv_start;
        srv.ptr += (static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount +
                    kVfxDistortionDescriptorOffset) * srv_stride;
        device->CreateShaderResourceView(nullptr, &srv_view, srv);
        srv = srv_start;
        srv.ptr += (static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount +
                    kBloomTextureDescriptorOffset) * srv_stride;
        for (std::uint32_t slot = 0; slot < 3; ++slot)
        {
            device->CreateShaderResourceView(nullptr, &srv_view, srv);
            srv.ptr += srv_stride;
        }
    }
}

Result D3D12Renderer::Impl::CreateGpuData()
{
    residency.Reset();
    if (resident_preparation_accounting.enabled)
        if (auto budget = UpdateResidencyBudget(); !budget) return budget;
    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    std::byte *mapped{};
    D3D12_RANGE no_read{};
    auto result = frames[0].allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset character geometry upload allocator", result);
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result)) return HResultFailure("Reset character geometry upload list", result);
    std::vector<AllocationResource> geometry_uploads;
    if (auto created = CreateProceduralGeometry(command_list.Get(), geometry_uploads); !created)
        return created;
    const auto load_character_geometry = [&](const std::string &logical_id,
        bool validate_upper_body_mask, render_data::CharacterMetadataPackageData &character,
        GeometryPageGpu &geometry_page, std::uint32_t page_slot,
        std::vector<render_data::ExpandedVertex> *support_vertices = nullptr) -> Result {
        std::filesystem::path metadata_path;
        if (auto resolved = config.ResolveProduct(logical_id, metadata_path); !resolved)
            return resolved;
        std::vector<std::byte> metadata_bytes;
        if (auto read = ReadBinary(metadata_path, metadata_bytes); !read) return read;
        try
        {
            character = render_data::DecodeCharacterMetadataPackage(
                metadata_bytes, MakeAssetId(logical_id), validate_upper_body_mask);
            const auto package = render_data::ReadPackage(metadata_bytes, MakeAssetId(logical_id));
            if (logical_id == "render/character/archer.rpkg")
            {
                constexpr std::string_view extension_id = "render/animation/archer.rpkg";
                if (std::ranges::find(package.dependencies, extension_id) == package.dependencies.end())
                    throw std::runtime_error("Archer metadata is missing its named-animation dependency; recook the character package.");
                std::filesystem::path extension_path;
                if (auto resolved = config.ResolveProduct(extension_id, extension_path); !resolved) return resolved;
                std::vector<std::byte> extension_bytes;
                if (auto read = ReadBinary(extension_path, extension_bytes); !read) return read;
                auto extension = render_data::DecodeCharacterAnimationExtension(extension_bytes, MakeAssetId(extension_id));
                render_data::ValidateCharacterAnimationBinding(extension, character, MakeAssetId(logical_id));
                archer_named_animations = std::move(extension);
            }
            for (const auto &binding : character.material_bindings)
            {
                const auto name = render_data::kHeroMaterialNames[binding.hero_ordinal];
                const auto &material_id = package.dependencies[binding.dependency_index];
                std::filesystem::path material_path;
                if (auto resolved = config.ResolveProduct(material_id, material_path); !resolved) return resolved;
                std::vector<std::byte> material_bytes;
                if (auto read = ReadBinary(material_path, material_bytes); !read) return read;
                const auto material = render_data::ReadHeroMaterialPackage(material_bytes, name);
                if (material.domain != binding.domain)
                    throw std::runtime_error("character FBX slot binding disagrees with its material closure domain");
            }
            const auto &page_id = package.dependencies[character.geometry.page_dependency_index];
            std::filesystem::path page_path;
            if (auto resolved = config.ResolveProduct(page_id, page_path); !resolved) return resolved;
            std::vector<std::byte> page_bytes;
            if (auto read = ReadBinary(page_path, page_bytes); !read) return read;
            const auto character_stem = std::filesystem::path(logical_id).stem().string();
            const auto mesh_source_id = "character/" + character_stem + ".meshbin";
            const auto page_package = render_data::ReadPackage(page_bytes, MakeAssetId(page_id));
            if (page_package.dependencies != std::vector<std::string>{mesh_source_id})
                throw std::runtime_error(
                    "character geometry page has an unexpected source dependency; expected only " +
                    mesh_source_id);
            auto geometry = render_data::DecodeGeometryPage(
                page_bytes, MakeAssetId(page_id), character.geometry);
            render_data::ValidateCharacterGeometry(character, geometry);
            if (auto created = CreateGeometryPage(geometry_page, character.geometry, page_bytes,
                    MakeAssetId(page_id), page_slot, command_list.Get(), geometry_uploads); !created)
                return created;
            if (support_vertices) *support_vertices = std::move(geometry.vertices);
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Invalid character geometry " + metadata_path.string() + ": " + error.what());
        }
        return Result::Success();
    };

    constexpr std::array<const char *, 6> monster_names{
        "enemy_melee", "enemy_ranged", "enemy_suicide", "boss_5m", "boss_10m", "boss_final"};
    std::vector<DirectX::XMFLOAT4X4> skin_matrices;
    for (std::size_t asset_index = 0; asset_index < monster_names.size(); ++asset_index)
    {
        auto &asset = monster_assets[asset_index];
        const std::string logical_id =
            std::string("render/character/") + monster_names[asset_index] + ".rpkg";
        render_data::CharacterMetadataPackageData character;
        if (auto loaded = load_character_geometry(logical_id, false, character, asset,
                kMonsterGeometryPage + static_cast<std::uint32_t>(asset_index)); !loaded)
            return loaded;
        asset.bone_count = character.bone_count;
        asset.ground_offset = character.ground_offset;
        asset.material_count = character.material_count;
        asset.material_bindings = std::move(character.material_bindings);
        asset.skin_offset = static_cast<std::uint32_t>(skin_matrices.size());
        for (const auto &clip : character.clips)
        {
            asset.clip_meta[static_cast<std::size_t>(clip.clip)] = {
                static_cast<std::uint32_t>(skin_matrices.size() - asset.skin_offset),
                clip.frame_count, clip.looping, asset.bone_count};
            for (std::uint32_t frame = 0; frame < clip.frame_count; ++frame)
            {
                std::array<DirectX::XMFLOAT4X4, kMaxCharacterBones> globals{};
                for (std::uint32_t bone = 0; bone < asset.bone_count; ++bone)
                {
                    const auto &local = character.transforms[clip.first_transform +
                        frame * asset.bone_count + bone];
                    const auto local_matrix = DirectX::XMMatrixScaling(
                        local.scale[0], local.scale[1], local.scale[2]) *
                        DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(
                            local.rotation[0], local.rotation[1], local.rotation[2], local.rotation[3])) *
                        DirectX::XMMatrixTranslation(local.translation[0], local.translation[1],
                                                     local.translation[2]);
                    const auto parent = character.parents[bone];
                    const auto global = parent == std::numeric_limits<std::uint16_t>::max()
                                            ? local_matrix
                                            : local_matrix * DirectX::XMLoadFloat4x4(&globals[parent]);
                    DirectX::XMStoreFloat4x4(&globals[bone], global);
                    DirectX::XMFLOAT4X4 inverse_bind{};
                    std::memcpy(&inverse_bind, character.inverse_bind_matrices[bone].data(),
                                sizeof(inverse_bind));
                    // Cooked FBX inverse-bind matrices use column-vector layout;
                    // DirectX runtime transforms use row-vector layout.
                    const auto skin = DirectX::XMMatrixTranspose(
                                          DirectX::XMLoadFloat4x4(&inverse_bind)) *
                                      global;
                    const auto determinant = DirectX::XMVectorGetX(
                        DirectX::XMMatrixDeterminant(skin));
                    if (!std::isfinite(determinant) || determinant <= 0.0001f ||
                        DirectX::XMMatrixIsNaN(skin) || DirectX::XMMatrixIsInfinite(skin))
                    {
                        return Result::Failure(
                            ErrorCode::InvalidState, "hs_renderer_d3d12",
                            "Monster animation produced an invalid skin pose.");
                    }
                    auto &stored_skin = skin_matrices.emplace_back();
                    DirectX::XMStoreFloat4x4(&stored_skin,
                                             DirectX::XMMatrixTranspose(skin));
                }
            }
        }
    }
    const auto skin_bytes = skin_matrices.size() * sizeof(skin_matrices.front());
    if (auto created = CreateAllocation(monster_skin_matrices, upload_allocation,
                                        BufferDescription(skin_bytes),
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
        return created;
    result = monster_skin_matrices.resource->Map(0, &no_read,
                                                 reinterpret_cast<void **>(&mapped));
    if (FAILED(result)) return HResultFailure("Map monster skin buffer", result);
    std::memcpy(mapped, skin_matrices.data(), skin_bytes);
    monster_skin_matrices.resource->Unmap(0, nullptr);

    // The atlas is cooked from Blender-authored triangle-expanded meshes.
    std::filesystem::path atlas_path;
    if (auto resolved = config.ResolveProduct("vfx/vfx_mesh_atlas.hsbin", atlas_path);
        !resolved)
        return resolved;
    std::ifstream atlas_stream(atlas_path, std::ios::binary | std::ios::ate);
    const auto atlas_failure = [] {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", "Invalid or missing VFX mesh atlas");
    };
    if (!atlas_stream) return atlas_failure();
    const auto atlas_size = atlas_stream.tellg();
    if (atlas_size < 100 || atlas_size > 64 * 1024 * 1024) return atlas_failure();
    std::vector<std::uint32_t> atlas_words(static_cast<std::size_t>(atlas_size) / 4);
    if (static_cast<std::size_t>(atlas_size) % 4 != 0) return atlas_failure();
    atlas_stream.seekg(0);
    if (!atlas_stream.read(reinterpret_cast<char *>(atlas_words.data()), atlas_size)) return atlas_failure();
    if (atlas_words[0] != 0x4d565348u || atlas_words[1] != 1 || atlas_words[2] != 7 ||
        atlas_words.size() != 25ull + 12ull * atlas_words[3]) return atlas_failure();
    constexpr std::array<std::string_view, 7> mesh_names{
        "arrowhead_mesh", "shard_mesh", "debris_shard_mesh", "small_shard_mesh",
        "enemy_thorn_mesh", "boss_crest_lance_mesh", "needle_shard_mesh"};
    std::uint32_t next_vertex = 0;
    for (std::size_t mesh = 0; mesh < 7; ++mesh)
    {
        std::uint32_t expected_hash = 2166136261u;
        for (const auto character : mesh_names[mesh])
            expected_hash = (expected_hash ^ static_cast<unsigned char>(character)) * 16777619u;
        if (atlas_words[4 + mesh * 3] != expected_hash) return atlas_failure();
        const auto first = atlas_words[5 + mesh * 3];
        const auto count = atlas_words[6 + mesh * 3];
        if (first != next_vertex || count == 0 || count % 3 != 0 ||
            first > atlas_words[3] || count > atlas_words[3] - first) return atlas_failure();
        vfx_mesh_vertex_counts[mesh] = count;
        float transverse_radius{};
        float minimum_z = std::numeric_limits<float>::infinity();
        float maximum_z = -minimum_z, widest_z{};
        for (std::uint32_t vertex = first; vertex < first + count; ++vertex)
        {
            float x{}, y{}, z{};
            std::memcpy(&x, &atlas_words[25 + vertex * 12], sizeof(x));
            std::memcpy(&y, &atlas_words[26 + vertex * 12], sizeof(y));
            std::memcpy(&z, &atlas_words[27 + vertex * 12], sizeof(z));
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return atlas_failure();
            minimum_z = std::min(minimum_z, z);
            maximum_z = std::max(maximum_z, z);
            const auto radius = std::hypot(x, y);
            if (radius > transverse_radius) { transverse_radius = radius; widest_z = z; }
        }
        if (transverse_radius <= 0) return atlas_failure();
        vfx_mesh_transverse_radii[mesh] = transverse_radius;
        if (minimum_z >= maximum_z) return atlas_failure();
        vfx_mesh_longitudinal_bounds[mesh] = {minimum_z, widest_z, maximum_z};
        next_vertex += count;
    }
    if (next_vertex != atlas_words[3]) return atlas_failure();
    for (std::size_t word = 25; word < atlas_words.size(); ++word)
    {
        float value{};
        std::memcpy(&value, &atlas_words[word], sizeof(value));
        if (!std::isfinite(value)) return atlas_failure();
    }
    if (auto created = CreateAllocation(vfx_mesh_atlas, upload_allocation,
            BufferDescription(static_cast<std::size_t>(atlas_size)), D3D12_RESOURCE_STATE_GENERIC_READ);
        !created) return created;
    result = vfx_mesh_atlas.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(result)) return HResultFailure("Map VFX mesh atlas", result);
    std::memcpy(mapped, atlas_words.data(), static_cast<std::size_t>(atlas_size));
    vfx_mesh_atlas.resource->Unmap(0, nullptr);

    const std::string archer_logical_id = "render/character/archer.rpkg";
    render_data::CharacterMetadataPackageData archer_package;
    std::vector<render_data::ExpandedVertex> cooked_vertices;
    if (auto loaded = load_character_geometry(archer_logical_id, true, archer_package,
            archer_geometry, kArcherGeometryPage, &cooked_vertices); !loaded)
        return loaded;
    archer_clips = std::move(archer_package.clips);
    archer_parents = std::move(archer_package.parents);
    archer_inverse_bind_matrices = std::move(archer_package.inverse_bind_matrices);
    archer_upper_body_weights = std::move(archer_package.upper_body_weights);
    archer_transforms = std::move(archer_package.transforms);
    archer_bone_count = archer_package.bone_count;
    archer_ground_offset = archer_package.ground_offset;
    archer_material_count = archer_package.material_count;
    archer_material_bindings = std::move(archer_package.material_bindings);
    archer_support_vertices.clear();
    for (const auto &vertex : cooked_vertices)
    {
        if (vertex.position[1] > -archer_ground_offset + 0.12f) continue;
        const auto duplicate = std::ranges::any_of(archer_support_vertices, [&](const auto &existing) {
            return existing.position == vertex.position &&
                   existing.bone_indices == vertex.bone_indices &&
                   existing.bone_weights == vertex.bone_weights;
        });
        if (!duplicate) archer_support_vertices.push_back({vertex.position, vertex.normal,
            vertex.bone_indices, vertex.bone_weights, vertex.uv, vertex.tangent,
            vertex.material_index, vertex.reserved});
    }

    const std::string projectile_logical_id = "render/character/enemy_gel_projectile.rpkg";
    render_data::CharacterMetadataPackageData projectile_package;
    if (auto loaded = load_character_geometry(projectile_logical_id, false, projectile_package,
            gel_projectile_geometry, kProjectileGeometryPage); !loaded)
        return loaded;

    // Particle execution state belongs to the live renderer, independently of
    // the immutable resident asset generation.
    if (!resident_preparation_accounting.enabled)
    {
        D3D12MA::ALLOCATION_DESC default_allocation{};
        default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        const auto particle_description =
            BufferDescription(kParticleCount * sizeof(GpuParticle),
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (auto created = CreateAllocation(particles, default_allocation, particle_description,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            !created)
        {
            return created;
        }
        const auto particle_index_description =
            BufferDescription(kParticleCount * sizeof(std::uint32_t),
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        for (auto &alive : particle_alive)
        {
            if (auto created = CreateAllocation(alive, default_allocation,
                                                BufferDescription(3 * kParticleCount * sizeof(std::uint32_t),
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                !created)
            {
                return created;
            }
        }
        if (auto created = CreateAllocation(particle_dead, default_allocation,
                                            particle_index_description,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            !created)
        {
            return created;
        }
        if (auto created = CreateAllocation(
                particle_counters, default_allocation,
                BufferDescription(20, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            !created)
        {
            return created;
        }
        const auto indirect_description =
            BufferDescription(2 * sizeof(D3D12_DRAW_ARGUMENTS),
                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (auto created = CreateAllocation(indirect_arguments, default_allocation, indirect_description,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS); !created)
            return created;
    }
    if (auto prepared = PrepareHeroBindings(command_list.Get(), geometry_uploads); !prepared) return prepared;
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close character geometry upload", result);
    return SubmitResidentUtilityUpload(command_list.Get(), frames[0].allocator.Get(), geometry_uploads);
}

Result D3D12Renderer::Impl::CreateCharacterTextures()
{
    auto result = frames[0].allocator->Reset();
    if (FAILED(result))
    {
        return HResultFailure("Reset character texture allocator", result);
    }
    result = command_list->Reset(frames[0].allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return HResultFailure("Reset character texture command list", result);
    }

    std::vector<AllocationResource> uploads;
    uploads.reserve(7);
    struct PackageSource
    {
        std::vector<std::byte> storage;
        render_data::TexturePackageView view;
    };
    auto upload_packaged = [&](AllocationResource &texture,
                               const std::vector<std::string> &names,
                               std::uint32_t width, std::uint32_t height,
                               std::uint32_t mips) -> Result {
        std::vector<PackageSource> sources(names.size());
        for (std::size_t slice = 0; slice < names.size(); ++slice)
        {
            const auto source_id = "character/" + names[slice] + ".dds";
            const auto package_id = "render/texture/" + names[slice] + ".rpkg";
            std::filesystem::path path;
            if (auto resolved = config.ResolveProduct(package_id, path); !resolved)
                return resolved;
            if (auto loaded = ReadBinary(path, sources[slice].storage); !loaded)
                return loaded;
            try
            {
                sources[slice].view = render_data::ReadTexturePackage(
                    sources[slice].storage, MakeAssetId(package_id));
            }
            catch (const std::exception &error)
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Invalid character texture package " + path.string() + ": " +
                    error.what());
            }
            const auto &view = sources[slice].view;
            const bool normal = names[slice].find("_normal_") != std::string::npos ||
                                names[slice] == "monster_ram";
            const bool placeholder = names[slice] == "archer_normal_0";
            const auto source_width = placeholder ? 1u : width;
            const auto source_height = placeholder ? 1u : height;
            const auto source_mips = placeholder ? 1u : mips;
            if (view.description.width != source_width ||
                view.description.height != source_height ||
                view.description.array_size != 1 ||
                view.description.mip_count != source_mips ||
                view.description.format != (normal ?
                    render_data::kTextureFormatRgba8 :
                    render_data::kTextureFormatRgba8Srgb) ||
                view.dependencies != std::vector<std::string>{source_id} ||
                view.subresources.size() != source_mips)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "Character texture package shape or dependency changed: " +
                    path.string());
        }

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = static_cast<UINT16>(names.size());
        description.MipLevels = static_cast<UINT16>(mips);
        description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        description.SampleDesc = {1, 0};
        D3D12MA::ALLOCATION_DESC allocation{};
        allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(texture, allocation, description,
                                             D3D12_RESOURCE_STATE_COPY_DEST); !created)
            return created;
        const auto count = static_cast<UINT>(names.size() * mips);
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> row_sizes(count);
        UINT64 upload_size{};
        device->GetCopyableFootprints(&description, 0, count, 0, footprints.data(),
                                      rows.data(), row_sizes.data(), &upload_size);
        uploads.emplace_back();
        allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), allocation,
                                             BufferDescription(upload_size),
                                             D3D12_RESOURCE_STATE_GENERIC_READ); !created)
            return created;
        std::byte *mapped{};
        D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(0, &no_read,
                                               reinterpret_cast<void **>(&mapped));
        if (FAILED(result))
            return HResultFailure("Map character texture upload", result);
        for (std::size_t slice = 0; slice < names.size(); ++slice)
            for (std::uint32_t mip = 0; mip < mips; ++mip)
            {
                const auto index = static_cast<UINT>(slice * mips + mip);
                const auto target_width = std::max(1u, width >> mip);
                const auto target_height = std::max(1u, height >> mip);
                const auto &source = sources[slice].view.subresources[mip];
                if (source.width == target_width && source.height == target_height)
                {
                    for (std::uint32_t row = 0; row < target_height; ++row)
                        std::memcpy(mapped + footprints[index].Offset +
                                        static_cast<std::size_t>(row) *
                                            footprints[index].Footprint.RowPitch,
                                    source.bytes.data() + static_cast<std::size_t>(row) *
                                        source.row_pitch,
                                    source.row_pitch);
                }
                else
                {
                    for (std::uint32_t row = 0; row < target_height; ++row)
                    {
                        auto *destination = mapped + footprints[index].Offset +
                            static_cast<std::size_t>(row) *
                                footprints[index].Footprint.RowPitch;
                        const auto source_row = std::uint64_t(row) * source.height /
                                                target_height;
                        for (std::uint32_t column = 0; column < target_width; ++column)
                        {
                            const auto source_column = std::uint64_t(column) *
                                                       source.width / target_width;
                            std::memcpy(destination + static_cast<std::size_t>(column) * 4,
                                        source.bytes.data() +
                                            source_row * source.row_pitch +
                                            source_column * 4, 4);
                        }
                    }
                }
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = texture.resource.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = index;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = uploads.back().resource.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = footprints[index];
                command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
        uploads.back().resource->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = kAllShaderRead;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
        return Result::Success();
    };

    std::vector<std::string> archer_diffuse_names, archer_normal_names;
    for (std::uint32_t index = 0; index < archer_material_count; ++index)
    {
        archer_diffuse_names.push_back("archer_diffuse_" + std::to_string(index));
        archer_normal_names.push_back("archer_normal_" + std::to_string(index));
    }
    if (auto uploaded = upload_packaged(archer_diffuse, archer_diffuse_names,
                                         kCharacterTextureSize, kCharacterTextureSize, 1);
        !uploaded) return uploaded;
    if (auto uploaded = upload_packaged(monster_basecolor, {"monster_basecolor"},
                                         512, 512, 1); !uploaded) return uploaded;
    if (auto uploaded = upload_packaged(monster_emissive, {"monster_emissive"},
                                         512, 512, 1); !uploaded) return uploaded;
    if (auto uploaded = upload_packaged(monster_ram, {"monster_ram"},
                                         512, 512, 1); !uploaded) return uploaded;
    if (auto uploaded = upload_packaged(family_diffuse,
                                         {"enemy_melee_diffuse_0",
                                          "enemy_ranged_diffuse_0",
                                          "enemy_suicide_diffuse_0"},
                                         1024, 1024, 11); !uploaded) return uploaded;
    if (auto uploaded = upload_packaged(family_normal,
                                         {"enemy_melee_normal_0",
                                          "enemy_ranged_normal_0",
                                          "enemy_suicide_normal_0"},
                                         1024, 1024, 11); !uploaded) return uploaded;
    if (auto uploaded = upload_packaged(archer_normal, archer_normal_names,
                                         kCharacterTextureSize, kCharacterTextureSize, 1);
        !uploaded) return uploaded;
    const auto upload_vfx_package = [&](AllocationResource &texture, const char *filename,
                                        std::uint32_t expected_width,
                                        std::uint32_t expected_height,
                                        std::uint32_t expected_mips,
                                        std::uint32_t expected_array_size,
                                        DXGI_FORMAT format,
                                        D3D12_RESOURCE_STATES final_state,
                                        std::uint32_t *actual_array_size = nullptr,
                                        std::uint32_t *actual_height = nullptr) -> Result
    {
        const auto name = std::filesystem::path(filename).stem().string();
        const auto package_id = "render/texture/" + name + ".rpkg";
        const auto source_id = name == "vfx_masks"
            ? "vfx/vfx_masks.dds"
            : "vfx/Content/Textures/VFX/" + std::string(filename);
        std::filesystem::path path;
        if (auto resolved = config.ResolveProduct(package_id, path); !resolved)
            return resolved;
        std::vector<std::byte> storage;
        if (auto loaded = ReadBinary(path, storage); !loaded)
            return loaded;
        render_data::TexturePackageView view;
        try
        {
            view = render_data::ReadTexturePackage(storage, MakeAssetId(package_id));
        }
        catch (const std::exception &error)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "Invalid VFX texture package " + path.string() + ": " + error.what());
        }
        const auto &shape = view.description;
        if (shape.width != expected_width ||
            (expected_height != 0 && shape.height != expected_height) ||
            (expected_array_size != 0 && shape.array_size != expected_array_size) ||
            shape.height == 0 || shape.array_size == 0 ||
            shape.array_size > std::numeric_limits<UINT16>::max() ||
            shape.mip_count != expected_mips ||
            shape.format != static_cast<std::uint32_t>(format) ||
            view.dependencies != std::vector<std::string>{source_id} ||
            view.subresources.size() !=
                static_cast<std::size_t>(shape.array_size) * shape.mip_count)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                "VFX texture package shape or dependency changed: " + path.string());

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = shape.width;
        description.Height = shape.height;
        description.DepthOrArraySize = static_cast<UINT16>(shape.array_size);
        description.MipLevels = static_cast<UINT16>(shape.mip_count);
        description.Format = format;
        description.SampleDesc = {1, 0};
        D3D12MA::ALLOCATION_DESC default_allocation{};
        default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
        if (auto created = CreateAllocation(texture, default_allocation, description,
                                             D3D12_RESOURCE_STATE_COPY_DEST); !created)
            return created;

        const auto subresource_count =
            static_cast<UINT>(shape.array_size * shape.mip_count);
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresource_count);
        std::vector<UINT> rows(subresource_count);
        std::vector<UINT64> row_sizes(subresource_count);
        UINT64 upload_size{};
        device->GetCopyableFootprints(&description, 0, subresource_count, 0,
                                      footprints.data(), rows.data(), row_sizes.data(),
                                      &upload_size);
        for (UINT index = 0; index < subresource_count; ++index)
        {
            const auto &source = view.subresources[index];
            if (row_sizes[index] != source.row_pitch ||
                static_cast<std::uint64_t>(rows[index]) * source.row_pitch !=
                    source.slice_pitch)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                    "VFX texture package subresource pitch differs from the GPU footprint: " +
                    path.string());
        }

        uploads.emplace_back();
        D3D12MA::ALLOCATION_DESC upload_allocation{};
        upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = CreateAllocation(uploads.back(), upload_allocation,
                                             BufferDescription(upload_size),
                                             D3D12_RESOURCE_STATE_GENERIC_READ); !created)
            return created;
        std::byte *mapped{};
        D3D12_RANGE no_read{};
        result = uploads.back().resource->Map(0, &no_read,
                                               reinterpret_cast<void **>(&mapped));
        if (FAILED(result)) return HResultFailure("Map VFX texture upload", result);
        for (UINT index = 0; index < subresource_count; ++index)
        {
            const auto &source = view.subresources[index];
            for (UINT row = 0; row < rows[index]; ++row)
                std::memcpy(mapped + footprints[index].Offset +
                                static_cast<std::size_t>(row) *
                                    footprints[index].Footprint.RowPitch,
                            source.bytes.data() +
                                static_cast<std::size_t>(row) * source.row_pitch,
                            static_cast<std::size_t>(row_sizes[index]));
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.resource.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = index;
            D3D12_TEXTURE_COPY_LOCATION source_location{};
            source_location.pResource = uploads.back().resource.Get();
            source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source_location.PlacedFootprint = footprints[index];
            command_list->CopyTextureRegion(&destination, 0, 0, 0,
                                            &source_location, nullptr);
        }
        uploads.back().resource->Unmap(0, nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = final_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list->ResourceBarrier(1, &barrier);
        if (actual_array_size) *actual_array_size = shape.array_size;
        if (actual_height) *actual_height = shape.height;
        return Result::Success();
    };
    const auto graph_owned_vfx_upload_state = enhanced
        ? D3D12_RESOURCE_STATE_COMMON
        : kAllShaderRead;
    if (auto uploaded = upload_vfx_package(vfx_masks, "vfx_masks.dds",
            512, 512, 10, 0, DXGI_FORMAT_BC4_UNORM,
            kAllShaderRead, &vfx_sprite_count);
        !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_gradient, "vfx_gradient_lut.dds",
            256, 0, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
            graph_owned_vfx_upload_state, nullptr, &vfx_gradient_rows);
        !uploaded) return uploaded;
    vfx_gradient.graph_access = enhanced ? Access::Common : Access::AllShaderRead;
    if (auto uploaded = upload_vfx_package(ribbon_detail, "vfx_ribbon_detail_array.dds",
            256, 64, 9, 8, DXGI_FORMAT_R8_UNORM,
            kAllShaderRead); !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_authored_mask, "vfx_authored_mask_array.dds",
            256, 256, 9, 12, DXGI_FORMAT_R8_UNORM,
            kAllShaderRead); !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_noise_basis, "vfx_noise_basis_2d.dds",
            256, 256, 9, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
            kAllShaderRead); !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_curve_lut, "vfx_curve_lut.dds",
            256, 7, 1, 1, DXGI_FORMAT_R16_FLOAT,
            kAllShaderRead); !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_smoke_pos, "vfx_smoke6way_pos.dds",
            128, 128, 8, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
            kAllShaderRead); !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_smoke_neg, "vfx_smoke6way_neg.dds",
            128, 128, 8, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
            kAllShaderRead); !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_smoke_motion, "vfx_smoke_motion_vectors.dds",
            128, 128, 8, 64, DXGI_FORMAT_R8G8_SNORM,
            kAllShaderRead); !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_flow_curl, "vfx_flow_curl_2d.dds",
            256, 256, 9, 1, DXGI_FORMAT_R8G8_SNORM,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); !uploaded) return uploaded;
    if (auto uploaded = upload_vfx_package(vfx_stbn_scalar, "vfx_stbn_scalar.dds",
            64, 64, 1, 64, DXGI_FORMAT_R8_UNORM,
            graph_owned_vfx_upload_state); !uploaded) return uploaded;
    vfx_stbn_scalar.graph_access = enhanced ? Access::Common : Access::AllShaderRead;
    if (auto uploaded = upload_vfx_package(vfx_fracture_decals, "vfx_fracture_decal_array.dds",
            512, 512, 10, 4, DXGI_FORMAT_R8G8B8A8_UNORM,
            graph_owned_vfx_upload_state); !uploaded) return uploaded;
    vfx_fracture_decals.graph_access = enhanced ? Access::Common : Access::AllShaderRead;
    if (auto uploaded = upload_vfx_package(vfx_organic_decals, "vfx_organic_decal_array.dds",
            512, 512, 10, 8, DXGI_FORMAT_R8_UNORM,
            graph_owned_vfx_upload_state); !uploaded) return uploaded;
    vfx_organic_decals.graph_access = enhanced ? Access::Common : Access::AllShaderRead;
    if (auto uploaded = UploadVfxNoiseVolume(uploads); !uploaded) return uploaded;
    result = command_list->Close();
    if (FAILED(result))
    {
        return HResultFailure("Close character texture upload", result);
    }
    if (auto waited = SubmitResidentUtilityUpload(command_list.Get(), frames[0].allocator.Get(), uploads); !waited)
    {
        return waited;
    }

    for (std::uint32_t frame_index = 0; frame_index < kFrameCount; ++frame_index)
    {
        auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount +
                       kPostTextureDescriptorCount) * srv_stride;
        auto create_view = [&](ID3D12Resource *texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray.MipLevels = 1;
            view.Texture2DArray.ArraySize = archer_material_count;
            device->CreateShaderResourceView(texture, &view, handle);
            handle.ptr += srv_stride;
        };
        create_view(archer_diffuse.resource.Get(),
                    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_view(archer_normal.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D12_SHADER_RESOURCE_VIEW_DESC vfx_view{};
        vfx_view.Format = DXGI_FORMAT_BC4_UNORM;
        vfx_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        vfx_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vfx_view.Texture2DArray.MipLevels = 10;
        vfx_view.Texture2DArray.ArraySize = vfx_sprite_count;
        device->CreateShaderResourceView(vfx_masks.resource.Get(), &vfx_view, handle);
        handle.ptr += srv_stride;
        auto create_shared_view = [&](ID3D12Resource *texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(texture, &view, handle);
            handle.ptr += srv_stride;
        };
        create_shared_view(monster_basecolor.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_shared_view(monster_emissive.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_shared_view(monster_ram.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        auto create_family_view = [&](ID3D12Resource *texture, DXGI_FORMAT format) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2DArray.MipLevels = 11; view.Texture2DArray.ArraySize = 3;
            device->CreateShaderResourceView(texture, &view, handle); handle.ptr += srv_stride;
        };
        create_family_view(family_diffuse.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        create_family_view(family_normal.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        auto gradient_handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
        gradient_handle.ptr +=
            (static_cast<SIZE_T>(frame_index) * kTextureDescriptorCount +
             kPostTextureDescriptorCount + kCharacterDescriptorCount +
             kMonsterPbrDescriptorCount + kEnvironmentDescriptorCount) * srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC gradient_view{};
        gradient_view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        gradient_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        gradient_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        gradient_view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(vfx_gradient.resource.Get(), &gradient_view,
                                          gradient_handle);
        auto ribbon_detail_handle = gradient_handle;
        ribbon_detail_handle.ptr += srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC ribbon_detail_view{};
        ribbon_detail_view.Format = DXGI_FORMAT_R8_UNORM;
        ribbon_detail_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        ribbon_detail_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ribbon_detail_view.Texture2DArray.MipLevels = 9;
        ribbon_detail_view.Texture2DArray.ArraySize = 8;
        device->CreateShaderResourceView(ribbon_detail.resource.Get(), &ribbon_detail_view,
                                          ribbon_detail_handle);
        auto impact_handle = ribbon_detail_handle;
        impact_handle.ptr += srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC mask_view = ribbon_detail_view;
        mask_view.Texture2DArray.ArraySize = 12;
        device->CreateShaderResourceView(vfx_authored_mask.resource.Get(), &mask_view, impact_handle);
        impact_handle.ptr += srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC noise_view{};
        noise_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        noise_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        noise_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        noise_view.Texture2D.MipLevels = 9;
        device->CreateShaderResourceView(vfx_noise_basis.resource.Get(), &noise_view, impact_handle);
        impact_handle.ptr += srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC curve_view = noise_view;
        curve_view.Format = DXGI_FORMAT_R16_FLOAT;
        curve_view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(vfx_curve_lut.resource.Get(), &curve_view, impact_handle);
        impact_handle.ptr += srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC smoke_view{};
        smoke_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        smoke_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        smoke_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        smoke_view.Texture2DArray.MipLevels = 8;
        smoke_view.Texture2DArray.ArraySize = 64;
        device->CreateShaderResourceView(vfx_smoke_pos.resource.Get(), &smoke_view, impact_handle);
        impact_handle.ptr += srv_stride;
        device->CreateShaderResourceView(vfx_smoke_neg.resource.Get(), &smoke_view, impact_handle);
        impact_handle.ptr += srv_stride;
        smoke_view.Format = DXGI_FORMAT_R8G8_SNORM;
        device->CreateShaderResourceView(vfx_smoke_motion.resource.Get(), &smoke_view, impact_handle);
        impact_handle.ptr += 2 * srv_stride; // frame transient distortion target occupies the skipped slot.
        D3D12_SHADER_RESOURCE_VIEW_DESC flow_view = noise_view;
        flow_view.Format = DXGI_FORMAT_R8G8_SNORM;
        device->CreateShaderResourceView(vfx_flow_curl.resource.Get(), &flow_view, impact_handle);
        impact_handle.ptr += srv_stride;
        smoke_view.Format = DXGI_FORMAT_R8_UNORM;
        smoke_view.Texture2DArray.MipLevels = 1;
        device->CreateShaderResourceView(vfx_stbn_scalar.resource.Get(), &smoke_view, impact_handle);
        impact_handle.ptr += srv_stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC decal_view = smoke_view;
        decal_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        decal_view.Texture2DArray.MipLevels = 10;
        decal_view.Texture2DArray.ArraySize = 4;
        device->CreateShaderResourceView(vfx_fracture_decals.resource.Get(), &decal_view, impact_handle);
        impact_handle.ptr += srv_stride;
        decal_view.Format = DXGI_FORMAT_R8_UNORM;
        decal_view.Texture2DArray.ArraySize = 8;
        device->CreateShaderResourceView(vfx_organic_decals.resource.Get(), &decal_view, impact_handle);
    }
    return Result::Success();
}


} // namespace hs
