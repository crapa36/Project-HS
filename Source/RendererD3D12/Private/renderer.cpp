#include "renderer_impl.hpp"

namespace hs
{

D3D12Renderer::D3D12Renderer() : impl_(std::make_unique<Impl>())
{
}

D3D12Renderer::~D3D12Renderer()
{
    (void)Shutdown();
}

Result D3D12Renderer::Initialize(const RendererConfig &config)
{
    if (impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer already initialized.");
    }
    if (!config.window)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Renderer requires HWND.");
    }
    if (auto result = impl_->CreateDevice(config); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateSwapChainAndTargets(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateDepthAndShadow(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateUiTexture(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreatePostProcessTargets(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreatePipeline(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateGpuData(); !result)
    {
        return result;
    }
    if (auto result = impl_->CreateCharacterTextures(); !result)
    {
        return result;
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    D3D12_DESCRIPTOR_HEAP_DESC description{};
    description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    description.NumDescriptors = 1;
    description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(impl_->device->CreateDescriptorHeap(&description,
                                                    IID_PPV_ARGS(&impl_->imgui_heap))))
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot create Dear ImGui descriptor heap.");
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    const auto imgui_font_path =
        (CurrentExecutableDirectory() / L"Fonts" / L"NotoSansKR.ttf").string();
    ImGui::GetIO().Fonts->AddFontFromFileTTF(
        imgui_font_path.c_str(), 16.0f, nullptr,
        ImGui::GetIO().Fonts->GetGlyphRangesKorean());
    ImGui::StyleColorsDark();
    if (!ImGui_ImplWin32_Init(static_cast<HWND>(config.window)))
    {
        ImGui::DestroyContext();
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot initialize Dear ImGui Win32 backend.");
    }
    ImGui_ImplDX12_InitInfo info{};
    info.Device = impl_->device.Get();
    info.CommandQueue = impl_->queue.Get();
    info.NumFramesInFlight = kFrameCount;
    info.RTVFormat = kBackBufferFormat;
    info.DSVFormat = kDepthFormat;
    info.SrvDescriptorHeap = impl_->imgui_heap.Get();
    info.LegacySingleSrvCpuDescriptor =
        impl_->imgui_heap->GetCPUDescriptorHandleForHeapStart();
    info.LegacySingleSrvGpuDescriptor =
        impl_->imgui_heap->GetGPUDescriptorHandleForHeapStart();
    if (!ImGui_ImplDX12_Init(&info))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return Result::Failure(ErrorCode::InvalidState, "hs_devtools",
                               "Cannot initialize Dear ImGui D3D12 backend.");
    }
    impl_->imgui_initialized = true;
    impl_->shader_write = LatestShaderWrite();
    impl_->next_shader_check = std::chrono::steady_clock::now();
#endif
    impl_->initialized = true;
    return Result::Success();
}

void D3D12Renderer::HandleWindowMessage(const NativeWindowMessage &message) noexcept
{
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->imgui_initialized)
    {
        ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(impl_->config.window),
                                       message.message,
                                       static_cast<WPARAM>(message.wparam),
                                       static_cast<LPARAM>(message.lparam));
    }
#else
    (void)message;
#endif
}

Result D3D12Renderer::Resize(std::uint32_t width, std::uint32_t height)
{
    if (!impl_->initialized || width == 0 || height == 0 ||
        (width == impl_->width && height == impl_->height))
    {
        return Result::Success();
    }
    if (auto result = impl_->WaitForGpu(); !result)
    {
        return result;
    }

    impl_->depth.Reset();
    impl_->shadow.Reset();
    for (auto &back_buffer : impl_->back_buffers)
    {
        back_buffer.Reset();
    }
    const auto result = impl_->swap_chain->ResizeBuffers(kFrameCount, width, height,
                                                         kBackBufferFormat, 0);
    if (FAILED(result))
    {
        return impl_->CheckDevice(result, "ResizeBuffers");
    }
    impl_->width = width;
    impl_->height = height;
    const auto render_scale = std::clamp(impl_->config.render_scale_percent, 75u, 100u);
    impl_->render_width = std::max(width * render_scale / 100u, 1u);
    impl_->render_height = std::max(height * render_scale / 100u, 1u);

    auto handle = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        const auto get_result =
            impl_->swap_chain->GetBuffer(index, IID_PPV_ARGS(&impl_->back_buffers[index]));
        if (FAILED(get_result))
        {
            return HResultFailure("Get resized swap-chain buffer", get_result);
        }
        impl_->device->CreateRenderTargetView(impl_->back_buffers[index].Get(), nullptr, handle);
        handle.ptr += impl_->rtv_stride;
    }
    if (auto depth_result = impl_->CreateDepthAndShadow(); !depth_result)
    {
        return depth_result;
    }
    return impl_->CreatePostProcessTargets();
}

Result D3D12Renderer::ApplyOptions(const RendererOptions &options)
{
    if (!impl_->initialized)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");

    const auto render_scale = std::clamp(options.render_scale_percent, 75u, 100u);
    const auto shadow_resolution = std::clamp(options.shadow_resolution, 1024u, 2048u);
    const auto recreate_targets = render_scale != impl_->config.render_scale_percent ||
                                  shadow_resolution != impl_->config.shadow_resolution;
    impl_->config.vsync = options.vsync;
    impl_->config.bloom = options.bloom;
    impl_->config.outline = options.outline;
    impl_->config.render_scale_percent = render_scale;
    impl_->config.shadow_resolution = shadow_resolution;
    impl_->config.particle_percentage =
        std::clamp(options.particle_percentage, 50u, 100u);
    if (!recreate_targets)
        return Result::Success();
    if (auto result = impl_->WaitForGpu(); !result)
        return result;

    impl_->render_width = std::max(impl_->width * render_scale / 100u, 1u);
    impl_->render_height = std::max(impl_->height * render_scale / 100u, 1u);
    impl_->depth.Reset();
    impl_->shadow.Reset();
    if (auto result = impl_->CreateDepthAndShadow(); !result)
        return result;
    return impl_->CreatePostProcessTargets();
}

Result D3D12Renderer::Shutdown()
{
    if (!impl_)
    {
        return Result::Success();
    }
    Result result = Result::Success();
    if (impl_->initialized)
    {
        result = impl_->WaitForGpu();
        impl_->CountValidationErrors();
    }
#if defined(HS_DEVELOPMENT_TOOLS)
    if (impl_->imgui_initialized)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        impl_->imgui_initialized = false;
    }
    impl_->imgui_heap.Reset();
#endif
    for (auto &frame : impl_->frames)
    {
        if (frame.upload.resource && frame.mapped)
        {
            frame.upload.resource->Unmap(0, nullptr);
            frame.mapped = nullptr;
        }
        if (frame.ui_upload.resource && frame.ui_mapped)
        {
            frame.ui_upload.resource->Unmap(0, nullptr);
            frame.ui_mapped = nullptr;
        }
        frame.upload.Reset();
        frame.ui_upload.Reset();
    }
    impl_->depth.Reset();
    impl_->shadow.Reset();
    impl_->vertices.Reset();
    impl_->archer_vertices.Reset();
    impl_->archer_diffuse.Reset();
    impl_->archer_normal.Reset();
    impl_->vfx_masks.Reset();
    impl_->particles.Reset();
    for (auto &alive : impl_->particle_alive)
    {
        alive.Reset();
    }
    impl_->particle_dead.Reset();
    impl_->particle_counters.Reset();
    impl_->indirect_arguments.Reset();
    impl_->gbuffer_base.Reset();
    impl_->gbuffer_normal.Reset();
    impl_->gbuffer_position.Reset();
    impl_->hdr_color.Reset();
    impl_->oit_accumulation.Reset();
    impl_->oit_revealage.Reset();
    impl_->post_a.Reset();
    impl_->post_b.Reset();
    for (auto &texture : impl_->ui_textures)
    {
        texture.Reset();
    }
    for (auto &surface : impl_->ui_surfaces)
    {
        surface.brush.Reset();
        surface.target.Reset();
        surface.bitmap.Reset();
    }
    impl_->ui_font_collection.Reset();
    impl_->ui_dwrite_factory.Reset();
    impl_->ui_d2d_factory.Reset();
    impl_->ui_wic_factory.Reset();
    if (impl_->timestamp_readback.resource && impl_->mapped_timestamps)
    {
        D3D12_RANGE no_write{};
        impl_->timestamp_readback.resource->Unmap(0, &no_write);
        impl_->mapped_timestamps = nullptr;
    }
    impl_->timestamp_readback.Reset();
    impl_->timestamp_heap.Reset();
    for (auto &buffer : impl_->back_buffers)
    {
        buffer.Reset();
    }
    if (impl_->allocator)
    {
        impl_->allocator->Release();
        impl_->allocator = nullptr;
    }
    if (impl_->fence_event)
    {
        CloseHandle(impl_->fence_event);
        impl_->fence_event = nullptr;
    }
    impl_->initialized = false;
    if (impl_->com_initialized)
    {
        CoUninitialize();
        impl_->com_initialized = false;
    }
    return result;
}

bool D3D12Renderer::UsesEnhancedBarriers() const noexcept
{
    return impl_->enhanced;
}

std::uint64_t D3D12Renderer::ValidationErrorCount() const noexcept
{
    return impl_->validation_errors;
}

std::uint32_t D3D12Renderer::LastParticleCount() const noexcept
{
    return impl_->last_particle_count;
}

GpuPassTimings D3D12Renderer::LastGpuPassTimings() const noexcept
{
    GpuPassTimings timings;
    if (!impl_->mapped_timestamps || impl_->timestamp_frequency == 0 ||
        !impl_->frames[impl_->last_presented_index].timestamps_recorded)
    {
        return timings;
    }
    const auto base = impl_->last_presented_index * kTimestampCountPerFrame;
    for (std::size_t pass = 0; pass < timings.nanoseconds.size(); ++pass)
    {
        const auto begin = impl_->mapped_timestamps[base + pass * 2];
        const auto end = impl_->mapped_timestamps[base + pass * 2 + 1];
        if (end < begin)
        {
            return {};
        }
        timings.nanoseconds[pass] =
            (end - begin) * 1'000'000'000ull / impl_->timestamp_frequency;
    }
    timings.valid = true;
    return timings;
}


} // namespace hs
