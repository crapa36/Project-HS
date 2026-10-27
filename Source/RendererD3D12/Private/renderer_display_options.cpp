#include "renderer_impl.hpp"

namespace hs
{

Result D3D12Renderer::Impl::PrepareDisplayPipeline(
    const RendererConfig &candidate_config, DXGI_FORMAT format,
    ComPtr<ID3D12PipelineState> &output)
{
    if (!device || !root_signature ||
        (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R10G10B10A2_UNORM))
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Display pipeline requires the active device/root and an SDR or HDR10 format.");

    std::vector<std::byte> vertex, pixel;
    for (auto [name, bytes] : {std::pair{"fullscreen_vs.dxil", &vertex},
                               std::pair{"ui_ps.dxil", &pixel}})
    {
        RendererAssetSource path;
        if (auto resolved = ResolveRendererAsset(candidate_config, "render/shader/" + std::string(name), path);
            !resolved) return resolved;
        if (auto read = ReadBinary(path, *bytes); !read) return read;
        if (bytes->empty())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Empty selected display shader: " + path.path.string());
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = root_signature.Get();
    description.VS = {vertex.data(), vertex.size()};
    description.PS = {pixel.data(), pixel.size()};
    auto &blend = description.BlendState.RenderTarget[0];
    blend.BlendEnable = FALSE;
    blend.LogicOpEnable = FALSE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ZERO;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    description.SampleMask = UINT_MAX;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthEnable = FALSE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.StencilEnable = FALSE;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets = 1;
    description.RTVFormats[0] = format;
    description.SampleDesc = {1, 0};
    ComPtr<ID3D12PipelineState> prepared;
    const auto created = pipeline_cache.CreateGraphics(&description, IID_PPV_ARGS(&prepared));
    if (FAILED(created)) return CheckDevice(created, "Create display UI pipeline");
    output = std::move(prepared);
    return Result::Success();
}

Result D3D12Renderer::Impl::ApplyDisplayOptions(const RendererOptions &options)
{
    auto &owner = *owned_views.at(kMainRenderViewId.value);
    if (!initialized || !device || !adapter || !swap_chain || !rtv_heap || !ui_pipeline)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Display options require an initialized renderer.");

    auto candidate_config = config;
    candidate_config.hdr_mode = options.hdr_mode;
    candidate_config.hdr_paper_white_nits = options.hdr_paper_white_nits;
    candidate_config.hdr_peak_nits = options.hdr_peak_nits;
    candidate_config.display_output = options.display_output;
    const auto preparation_failure = [&](const Result &failure) -> Result {
        if (FAILED(device->GetDeviceRemovedReason()))
        {
            CountValidationErrors();
            initialized = false;
        }
        return failure;
    };
    DisplayState candidate;
    if (auto queried = QueryDisplayState(adapter.Get(), static_cast<HWND>(config.window),
                                         candidate_config, candidate); !queried)
        return preparation_failure(queried);
    if (candidate_config.display_output.value != "auto" &&
        MonitorFromWindow(static_cast<HWND>(config.window), MONITOR_DEFAULTTONEAREST) != candidate.monitor)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Runtime must place the window on the selected display before applying its options.");

    const auto previous = owner.display_state;
    const auto previous_presented_index = owner.last_presented_index;
    DXGI_SWAP_CHAIN_DESC1 previous_description{};
    const auto described = swap_chain->GetDesc1(&previous_description);
    if (FAILED(described))
        return preparation_failure(CheckDevice(described, "Query current display swap chain"));
    if (previous_description.Format != previous.format || previous_description.Width != owner.width ||
        previous_description.Height != owner.height || previous_description.BufferCount != kFrameCount)
    {
        initialized = false;
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Current display state does not match its swap-chain buffers; renderer is unusable.");
    }

    // Only the UI pipeline depends on the swap-chain format. Reuse a matching
    // pipeline; nits/metadata changes never rebuild the other renderer PSOs.
    ComPtr<ID3D12PipelineState> sdr_pipeline, hdr_pipeline;
    if (previous.format == DXGI_FORMAT_R8G8B8A8_UNORM) sdr_pipeline = ui_pipeline;
    else hdr_pipeline = ui_pipeline;
    if (candidate.format == DXGI_FORMAT_R10G10B10A2_UNORM && !hdr_pipeline)
        if (auto prepared = PrepareDisplayPipeline(candidate_config, candidate.format, hdr_pipeline); !prepared)
            return preparation_failure(prepared);
    // ApplyDisplayState can negotiate a PQ candidate down to SDR. Prepare that
    // format before touching any color-space, metadata, or back-buffer state.
    if (!sdr_pipeline)
        if (auto prepared = PrepareDisplayPipeline(candidate_config, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                   sdr_pipeline); !prepared)
            return preparation_failure(prepared);

    const auto write_rtvs = [&] {
        auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (const auto &buffer : back_buffers)
        {
            device->CreateRenderTargetView(buffer.Get(), nullptr, handle);
            handle.ptr += rtv_stride;
        }
    };
    const auto acquire_buffers = [&](DXGI_FORMAT format,
                                     std::array<ComPtr<ID3D12Resource>, kFrameCount> &buffers,
                                     std::string_view operation) -> Result {
        for (std::uint32_t index = 0; index < kFrameCount; ++index)
        {
            const auto acquired = swap_chain->GetBuffer(index, IID_PPV_ARGS(&buffers[index]));
            if (FAILED(acquired)) return CheckDevice(acquired, operation);
            const auto description = buffers[index]->GetDesc();
            if (description.Format != format || description.Width != owner.width || description.Height != owner.height)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       std::format("{} returned an incompatible back buffer {}.", operation, index));
        }
        return Result::Success();
    };
    const auto unusable = [&](const Result &failure, std::string_view recovery) -> Result {
        CountValidationErrors();
        initialized = false;
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::format("{}; {}; renderer is unusable.", failure.Message(), recovery));
    };
    bool buffers_released = false;
    const auto recover = [&](const Result &failure) -> Result {
        const auto device_reason = device->GetDeviceRemovedReason();
        if (FAILED(device_reason))
        {
            (void)CheckDevice(device_reason, "Recover display options");
            return unusable(failure, "device recovery is unavailable");
        }
        if (previous.monitor &&
            MonitorFromWindow(static_cast<HWND>(config.window), MONITOR_DEFAULTTONEAREST) != previous.monitor)
            return unusable(failure, "original display monitor no longer contains the window");
        if (buffers_released)
        {
            DXGI_SWAP_CHAIN_DESC1 current{};
            const auto queried = swap_chain->GetDesc1(&current);
            if (FAILED(queried))
                return unusable(failure, CheckDevice(queried, "Query display rollback swap chain").Message());
            if (current.Format != previous.format || current.Width != owner.width || current.Height != owner.height ||
                current.BufferCount != kFrameCount)
            {
                const auto resized = swap_chain->ResizeBuffers(kFrameCount, owner.width, owner.height,
                    previous.format, previous_description.Flags);
                if (FAILED(resized))
                    return unusable(failure, CheckDevice(resized, "Restore original display buffers").Message());
            }
        }
        auto restored = previous;
        if (auto applied = ApplyDisplayState(swap_chain.Get(), restored); !applied)
            return unusable(failure, std::format("original display state could not be restored: {}", applied.Message()));
        if (restored.format != previous.format || restored.color_space != previous.color_space ||
            restored.hdr_active != previous.hdr_active || restored.availability != previous.availability ||
            restored.reason != previous.reason)
            return unusable(failure, "original display state is no longer supported on the actual monitor");
        if (buffers_released)
        {
            std::array<ComPtr<ID3D12Resource>, kFrameCount> buffers;
            if (auto acquired = acquire_buffers(previous.format, buffers,
                                                 "Reacquire original display buffers"); !acquired)
                return unusable(failure, acquired.Message());
            back_buffers.swap(buffers);
            back_buffer_contracts = {};
            back_buffer_contract_devices = {};
            write_rtvs();
            back_buffer_completion = {};
            owner.last_presented_index = previous_presented_index;
        }
        // Configuration, effective state and UI PSO were never published.
        return failure;
    };
    const auto recover_native_failure = [&](HRESULT result, std::string_view operation) -> Result {
        const auto failure = CheckDevice(result, operation);
        if (result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
            result == DXGI_ERROR_DEVICE_HUNG)
            return unusable(failure, "device failure prevents display recovery");
        return recover(failure);
    };
    const auto publish = [&] {
        config.hdr_mode = candidate_config.hdr_mode;
        config.hdr_paper_white_nits = candidate_config.hdr_paper_white_nits;
        config.hdr_peak_nits = candidate_config.hdr_peak_nits;
        config.display_output = std::move(candidate_config.display_output);
        ui_pipeline = candidate.format == DXGI_FORMAT_R10G10B10A2_UNORM
            ? std::move(hdr_pipeline) : std::move(sdr_pipeline);
        owner.display_state = std::move(candidate);
    };

    bool display_applied = false;
    if (candidate.format == previous.format)
    {
        if (auto applied = ApplyDisplayState(swap_chain.Get(), candidate); !applied)
            return recover(applied);
        display_applied = true;
        if (candidate.format == previous.format)
        {
            publish();
            return Result::Success();
        }
    }

    // This is the sole GPU drain for a format transaction, including rollback.
    // Graph/provider leases retire in WaitForGpu; the CPU graph owns no live work.
    if (auto drained = WaitForGpu(); !drained)
    {
        if (display_applied)
        {
            const auto recovery = recover(drained);
            if (!initialized) return recovery;
        }
        return unusable(drained, "GPU completion could not be established for a display format change");
    }
    graph.Reset();
    for (auto &buffer : back_buffers) buffer.Reset();
    buffers_released = true;

    const auto requested_format = candidate.format;
    const auto resized = swap_chain->ResizeBuffers(kFrameCount, owner.width, owner.height,
        requested_format, previous_description.Flags);
    if (FAILED(resized)) return recover_native_failure(resized, "Resize display buffers");
    if (auto applied = ApplyDisplayState(swap_chain.Get(), candidate); !applied)
        return recover(applied);
    if (candidate.format != requested_format)
    {
        if (requested_format != DXGI_FORMAT_R10G10B10A2_UNORM ||
            candidate.format != DXGI_FORMAT_R8G8B8A8_UNORM || candidate.hdr_active)
            return recover(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                           "Display negotiation returned an unexpected buffer format."));
        // Negotiate exactly one SDR resize before acquiring any buffers or RTVs.
        const auto sdr_resized = swap_chain->ResizeBuffers(kFrameCount, owner.width, owner.height,
            candidate.format, previous_description.Flags);
        if (FAILED(sdr_resized))
            return recover_native_failure(sdr_resized, "Resize negotiated SDR display buffers");
        if (auto applied = ApplyDisplayState(swap_chain.Get(), candidate); !applied)
            return recover(applied);
    }
    {
        std::array<ComPtr<ID3D12Resource>, kFrameCount> buffers;
        if (auto acquired = acquire_buffers(candidate.format, buffers, "Acquire selected display buffers"); !acquired)
        {
            // A partial GetBuffer result is another direct DXGI buffer reference.
            for (auto &buffer : buffers) buffer.Reset();
            return recover(acquired);
        }
        back_buffers.swap(buffers);
        back_buffer_contracts = {};
        back_buffer_contract_devices = {};
    }
    write_rtvs();
    back_buffer_completion = {};
    owner.last_presented_index = swap_chain->GetCurrentBackBufferIndex();
    publish();
    return Result::Success();
}

} // namespace hs
