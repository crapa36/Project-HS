#include "renderer_impl.hpp"

namespace hs
{

Result D3D12Renderer::Impl::WritePng(const std::filesystem::path &path,
                                     const std::byte *pixels, std::uint32_t row_pitch) const
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    ComPtr<IWICImagingFactory> imaging_factory;
    auto result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&imaging_factory));
    if (FAILED(result))
    {
        return HResultFailure("Create WIC factory", result);
    }
    ComPtr<IWICStream> stream;
    result = imaging_factory->CreateStream(&stream);
    if (FAILED(result))
    {
        return HResultFailure("Create WIC stream", result);
    }
    result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(result))
    {
        return HResultFailure("Open PNG output", result);
    }
    ComPtr<IWICBitmapEncoder> encoder;
    result = imaging_factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(result))
    {
        return HResultFailure("Create PNG encoder", result);
    }
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result))
    {
        return HResultFailure("Initialize PNG encoder", result);
    }
    ComPtr<IWICBitmapFrameEncode> frame;
    result = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(result) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height)))
    {
        return HResultFailure("Initialize PNG frame", FAILED(result) ? result : E_FAIL);
    }
    auto format = GUID_WICPixelFormat32bppRGBA;
    result = frame->SetPixelFormat(&format);
    if (FAILED(result))
    {
        return HResultFailure("Set PNG format", result);
    }
    result = frame->WritePixels(height, row_pitch, row_pitch * height,
                                const_cast<BYTE *>(reinterpret_cast<const BYTE *>(pixels)));
    if (FAILED(result) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
    {
        return HResultFailure("Write PNG", FAILED(result) ? result : E_FAIL);
    }
    return Result::Success();
}

Result D3D12Renderer::CapturePng(const std::filesystem::path &path)
{
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }
    if (auto result = impl_->WaitForGpu(); !result)
    {
        return result;
    }

    const auto description = impl_->back_buffers[impl_->last_presented_index]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{};
    UINT64 total_size{};
    impl_->device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, &rows, &row_size,
                                         &total_size);

    AllocationResource readback;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto result = impl_->CreateAllocation(
            readback, allocation,
            BufferDescription(total_size + sizeof(D3D12_DRAW_ARGUMENTS)),
            D3D12_RESOURCE_STATE_COPY_DEST);
        !result)
    {
        return result;
    }

    auto &frame = impl_->frames[0];
    auto result = frame.allocator->Reset();
    if (FAILED(result))
    {
        return HResultFailure("Reset capture allocator", result);
    }
    result = impl_->command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return HResultFailure("Reset capture list", result);
    }
    impl_->TransitionTexture(impl_->back_buffers[impl_->last_presented_index].Get(),
                             D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_BARRIER_LAYOUT_PRESENT,
                             D3D12_BARRIER_LAYOUT_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = impl_->back_buffers[impl_->last_presented_index].Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    impl_->command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    impl_->TransitionBuffer(
        impl_->indirect_arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
        D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
        D3D12_BARRIER_ACCESS_COPY_SOURCE);
    impl_->command_list->CopyBufferRegion(readback.resource.Get(), total_size,
                                          impl_->indirect_arguments.resource.Get(), 0,
                                          sizeof(D3D12_DRAW_ARGUMENTS));
    impl_->TransitionBuffer(
        impl_->indirect_arguments.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COPY,
        D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_COPY_SOURCE,
        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    impl_->TransitionTexture(impl_->back_buffers[impl_->last_presented_index].Get(),
                             D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT,
                             D3D12_BARRIER_LAYOUT_COPY_SOURCE,
                             D3D12_BARRIER_LAYOUT_PRESENT);
    impl_->command_list->Close();
    ID3D12CommandList *lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    if (auto wait = impl_->WaitForGpu(); !wait)
    {
        return wait;
    }

    void *mapped_data{};
    D3D12_RANGE read_range{
        0, static_cast<SIZE_T>(total_size + sizeof(D3D12_DRAW_ARGUMENTS))};
    result = readback.resource->Map(0, &read_range, &mapped_data);
    if (FAILED(result))
    {
        return HResultFailure("Map capture", result);
    }
    const auto write = impl_->WritePng(path, static_cast<const std::byte *>(mapped_data),
                                       footprint.Footprint.RowPitch);
    const auto *draw_arguments = reinterpret_cast<const D3D12_DRAW_ARGUMENTS *>(
        static_cast<const std::byte *>(mapped_data) + total_size);
    impl_->last_particle_count = draw_arguments->InstanceCount;
    std::ofstream(path.parent_path() / "particle_stats.json", std::ios::trunc)
        << std::format("{{\"alive\":{},\"capacity\":{}}}\n", impl_->last_particle_count,
                       kParticleCount * std::clamp(impl_->config.particle_percentage, 50u,
                                                   100u) /
                           100u);
    D3D12_RANGE no_write{};
    readback.resource->Unmap(0, &no_write);
    return write;
}


} // namespace hs
