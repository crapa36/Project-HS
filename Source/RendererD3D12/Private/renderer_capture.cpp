#include "renderer_impl.hpp"

#include <hs/renderer/capture_request.hpp>

#include <DirectXTex.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace hs
{
namespace
{
// PNG is an SDR preview. HDR readback keeps its native packed representation
// until PQ and the display primaries have been decoded here.
float DecodePqNits(float value)
{
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    const auto powered = std::pow(value, 1.0f / m2);
    return 10000.0f * std::pow(std::max(powered - c1, 0.0f) /
                                  std::max(c2 - c3 * powered, 1.0e-6f),
                              1.0f / m1);
}

std::byte EncodePreviewChannel(float linear)
{
    const auto x = std::max(linear, 0.0f);
    const auto mapped = std::clamp(x * (2.51f * x + 0.03f) /
                                      (x * (2.43f * x + 0.59f) + 0.14f),
                                  0.0f, 1.0f);
    const auto encoded = mapped <= 0.0031308f ? 12.92f * mapped
        : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
    return static_cast<std::byte>(static_cast<unsigned>(std::lround(encoded * 255.0f)));
}

void HdrPreview(const std::byte *source, std::uint32_t source_pitch,
                std::uint32_t width, std::uint32_t height, float paper_white,
                std::vector<std::byte> &rgba)
{
    rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
        {
            std::uint32_t packed{};
            std::memcpy(&packed, source + static_cast<std::size_t>(y) * source_pitch + x * 4, 4);
            const auto r = DecodePqNits(static_cast<float>(packed & 1023) / 1023.0f) / paper_white;
            const auto g = DecodePqNits(static_cast<float>((packed >> 10) & 1023) / 1023.0f) / paper_white;
            const auto b = DecodePqNits(static_cast<float>((packed >> 20) & 1023) / 1023.0f) / paper_white;
            auto *pixel = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            pixel[0] = EncodePreviewChannel(1.660491f * r - 0.587641f * g - 0.072850f * b);
            pixel[1] = EncodePreviewChannel(-0.124550f * r + 1.132900f * g - 0.008349f * b);
            pixel[2] = EncodePreviewChannel(-0.018151f * r - 0.100579f * g + 1.118730f * b);
            pixel[3] = std::byte{255};
        }
}
} // namespace

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
    // SetPixelFormat returns the encoder's supported format, which can be BGRA
    // even though the readback is RGBA. Convert instead of reinterpreting bytes.
    ComPtr<IWICBitmap> bitmap;
    result = imaging_factory->CreateBitmapFromMemory(
        width, height, GUID_WICPixelFormat32bppRGBA, row_pitch, row_pitch * height,
        const_cast<BYTE *>(reinterpret_cast<const BYTE *>(pixels)), &bitmap);
    ComPtr<IWICBitmapSource> converted;
    if (SUCCEEDED(result))
        result = WICConvertBitmapSource(format, bitmap.Get(), &converted);
    if (SUCCEEDED(result))
        result = frame->WriteSource(converted.Get(), nullptr);
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
    auto *png_pixels = static_cast<const std::byte *>(mapped_data);
    auto png_pitch = footprint.Footprint.RowPitch;
    std::vector<std::byte> preview;
    if (description.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        HdrPreview(png_pixels, png_pitch, impl_->width, impl_->height,
                   impl_->display_state.paper_white_nits, preview);
        png_pixels = preview.data();
        png_pitch = impl_->width * 4;
    }
    const auto write = impl_->WritePng(path, png_pixels, png_pitch);
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
    if (const auto *hlod = impl_->ActiveHlodView(); write && hlod)
    {
        const auto &admission = impl_->hlod_runtime->admission_status;
        std::ofstream stats(path.parent_path() / "hlod_stats.json", std::ios::trunc);
        stats << std::format(
            "{{\"matching_scene\":{},\"demand_frame_id\":{},\"fault_bits\":{},"
            "\"selected_proxies\":{},\"selected_groups\":{},\"admitted_groups\":{},"
            "\"admitted_pages\":{},\"missing_slots\":{},\"optional_capacity\":{}}}\n",
            static_cast<bool>(hlod->matching_scene), hlod->demand_frame_id,
            hlod->last_status[0], hlod->last_status[1], hlod->last_status[2],
            admission[0], admission[1], admission[2], admission[3]);
        if (!stats)
            return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                                   "Could not write HLOD capture statistics.");
    }
    return write;
}

namespace
{

struct SurfaceCapture
{
    const char *name{};
    const AllocationResource *resource{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    Access final_access{Access::Undefined};
    D3D12_RESOURCE_STATES final_state{};
    D3D12_BARRIER_SYNC final_sync{};
    D3D12_BARRIER_ACCESS final_native_access{};
    D3D12_BARRIER_LAYOUT final_layout{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{};
    std::filesystem::path raw_path;
};

[[nodiscard]] const char *FormatName(DXGI_FORMAT format) noexcept
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32_UINT: return "DXGI_FORMAT_R32G32_UINT";
    case DXGI_FORMAT_R16G16_SNORM: return "DXGI_FORMAT_R16G16_SNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "DXGI_FORMAT_R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8_UINT: return "DXGI_FORMAT_R8_UINT";
    case DXGI_FORMAT_R16_FLOAT: return "DXGI_FORMAT_R16_FLOAT";
    case DXGI_FORMAT_R16G16_FLOAT: return "DXGI_FORMAT_R16G16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "DXGI_FORMAT_R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT: return "DXGI_FORMAT_R32G32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "DXGI_FORMAT_R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_UINT: return "DXGI_FORMAT_R32G32B32A32_UINT";
    case DXGI_FORMAT_R32_TYPELESS: return "DXGI_FORMAT_R32_TYPELESS";
    default: return "DXGI_FORMAT_UNKNOWN";
    }
}

[[nodiscard]] std::uint32_t ReadU32(const std::byte *bytes) noexcept
{
    std::uint32_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

[[nodiscard]] float ReadFloat(const std::byte *bytes) noexcept
{
    float value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

[[nodiscard]] float ReadHalf(const std::byte *bytes) noexcept
{
    std::uint16_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return DirectX::PackedVector::XMConvertHalfToFloat(value);
}

struct CaptureNativeAccess
{
    D3D12_RESOURCE_STATES state{};
    D3D12_BARRIER_SYNC sync{};
    D3D12_BARRIER_ACCESS access{};
    D3D12_BARRIER_LAYOUT layout{};
};

[[nodiscard]] bool GetCaptureNativeAccess(Access access, bool texture,
                                          CaptureNativeAccess &native) noexcept
{
    switch (access)
    {
    case Access::Common:
        native = {D3D12_RESOURCE_STATE_COMMON, D3D12_BARRIER_SYNC_ALL,
                  D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_LAYOUT_COMMON};
        return true;
    case Access::ShaderRead:
        native = {texture ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                          : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                  D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
        return true;
    case Access::ComputeRead:
        native = {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                  D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
        return true;
    case Access::AllShaderRead:
        native = {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                  D3D12_BARRIER_LAYOUT_SHADER_RESOURCE};
        return true;
    case Access::RenderTarget:
        if (!texture) return false;
        native = {D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET,
                  D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET};
        return true;
    case Access::DepthRead:
        if (!texture) return false;
        native = {D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                  D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ,
                  D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ};
        return true;
    case Access::DepthShaderRead:
        if (!texture) return false;
        native = {D3D12_RESOURCE_STATE_DEPTH_READ |
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_BARRIER_SYNC_DEPTH_STENCIL | D3D12_BARRIER_SYNC_ALL_SHADING,
                  D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ |
                      D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                  D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ};
        return true;
    case Access::DepthWrite:
        if (!texture) return false;
        native = {D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                  D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                  D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE};
        return true;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        native = {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_ALL_SHADING,
                  D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                  D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS};
        return true;
    case Access::CopySource:
        native = {D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_BARRIER_SYNC_COPY,
                  D3D12_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE};
        return true;
    case Access::CopyDest:
        native = {D3D12_RESOURCE_STATE_COPY_DEST, D3D12_BARRIER_SYNC_COPY,
                  D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST};
        return true;
    case Access::VertexBuffer:
        if (texture) return false;
        native = {D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                  D3D12_BARRIER_SYNC_VERTEX_SHADING, D3D12_BARRIER_ACCESS_VERTEX_BUFFER,
                  D3D12_BARRIER_LAYOUT_COMMON};
        return true;
    case Access::IndexBuffer:
        if (texture) return false;
        native = {D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_BARRIER_SYNC_INDEX_INPUT,
                  D3D12_BARRIER_ACCESS_INDEX_BUFFER, D3D12_BARRIER_LAYOUT_COMMON};
        return true;
    case Access::VertexShaderRead:
        if (texture) return false;
        native = {D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_BARRIER_SYNC_VERTEX_SHADING | D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                  D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                  D3D12_BARRIER_LAYOUT_COMMON};
        return true;
    case Access::IndirectArgs:
        if (texture) return false;
        native = {D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                  D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                  D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                  D3D12_BARRIER_LAYOUT_COMMON};
        return true;
    case Access::ShadingRateSource:
        if (!texture) return false;
        native = {D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE,
                  D3D12_BARRIER_SYNC_PIXEL_SHADING,
                  D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE,
                  D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE};
        return true;
    case Access::Present:
        if (!texture) return false;
        native = {D3D12_RESOURCE_STATE_PRESENT, D3D12_BARRIER_SYNC_NONE,
                  D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_PRESENT};
        return true;
    case Access::Undefined:
        return false;
    }
    return false;
}

} // namespace

namespace
{
[[nodiscard]] Result HdrCaptureFailure(std::string message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
}

[[nodiscard]] std::string JsonString(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const auto character : value)
    {
        switch (character)
        {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(character); break;
        }
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] std::string JsonNumber(std::uint64_t value)
{
    return value == 0 ? "null" : std::to_string(value);
}

template <typename Impl>
[[nodiscard]] Result CaptureHdrColor(Impl &impl, const std::filesystem::path &path)
{
    // The source is published by the successful frame transaction. It is the
    // reconstructed scene HDR result before post processing/display encoding.
    // In particular, do not substitute display_linear or the back buffer here:
    // both can already contain display encoding or tone mapping.
    if (!impl.hdr_capture_valid || !impl.hdr_capture_source.resource)
        return HdrCaptureFailure(
            "HDR capture has no successfully published reconstructed scene HDR source for this view.");
    if (!impl.hdr_capture_extent.width || !impl.hdr_capture_extent.height)
        return HdrCaptureFailure("HDR capture source has no published active extent.");

    if (auto result = impl.WaitForGpu(); !result)
        return result;

    auto &source_resource = impl.hdr_capture_source;
    const auto source_description = source_resource.resource->GetDesc();
    if (source_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        source_description.DepthOrArraySize != 1 || source_description.MipLevels != 1 ||
        source_description.SampleDesc.Count != 1 ||
        (source_description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
         source_description.Format != DXGI_FORMAT_R32G32B32A32_FLOAT))
    {
        return HdrCaptureFailure(
            "HDR capture source is not a single-sample RGBA floating-point texture.");
    }

    const auto width = impl.hdr_capture_extent.width;
    const auto height = impl.hdr_capture_extent.height;
    if (source_description.Width < width || source_description.Height < height)
        return HdrCaptureFailure("HDR capture source is smaller than its published active extent.");

    CaptureNativeAccess native{};
    if (!GetCaptureNativeAccess(source_resource.graph_access, true, native))
        return HdrCaptureFailure(
            "HDR capture source has no known native state for a safe readback.");

    // The footprint is for the active view extent. A source allocation may be
    // larger because it is a capacity target; the source box below keeps the
    // export tied to the completed view dimensions.
    auto footprint_description = source_description;
    footprint_description.Width = width;
    footprint_description.Height = height;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{};
    UINT64 total_size{};
    impl.device->GetCopyableFootprints(&footprint_description, 0, 1, 0, &footprint,
                                       &rows, &row_size, &total_size);
    if (rows != height || row_size == 0 || footprint.Footprint.RowPitch < row_size ||
        total_size == 0 || total_size > std::numeric_limits<SIZE_T>::max())
    {
        return HdrCaptureFailure("HDR capture readback footprint is invalid.");
    }

    std::error_code directory_error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error)
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               "Could not create HDR capture output directory.");

    AllocationResource readback;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto result = impl.CreateAllocation(readback, allocation, BufferDescription(total_size),
                                            D3D12_RESOURCE_STATE_COPY_DEST);
        !result)
    {
        return result;
    }

    auto &frame = impl.frames[0];
    auto result = frame.allocator->Reset();
    if (FAILED(result))
        return HResultFailure("Reset HDR capture allocator", result);
    result = impl.command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result))
        return HResultFailure("Reset HDR capture list", result);

    impl.TransitionTexture(source_resource.resource.Get(), native.state,
                           D3D12_RESOURCE_STATE_COPY_SOURCE, native.layout,
                           D3D12_BARRIER_LAYOUT_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = source_resource.resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX source_box{0, 0, 0, width, height, 1};
    impl.command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, &source_box);
    impl.TransitionTexture(source_resource.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                           native.state, D3D12_BARRIER_LAYOUT_COPY_SOURCE, native.layout);
    result = impl.command_list->Close();
    if (FAILED(result))
        return HResultFailure("Close HDR capture list", result);
    ID3D12CommandList *lists[] = {impl.command_list.Get()};
    impl.queue->ExecuteCommandLists(1, lists);
    if (auto wait = impl.WaitForGpu(); !wait)
        return wait;

    void *mapped_data{};
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    result = readback.resource->Map(0, &read_range, &mapped_data);
    if (FAILED(result))
        return HResultFailure("Map HDR capture", result);
    const auto unmap = [&]() {
        const D3D12_RANGE no_write{};
        readback.resource->Unmap(0, &no_write);
    };
    const auto mapped = static_cast<const std::byte *>(mapped_data);

    DirectX::ScratchImage source_image;
    result = source_image.Initialize2D(source_description.Format, width, height, 1, 1);
    if (FAILED(result))
    {
        unmap();
        return HResultFailure("Initialize HDR capture image", result);
    }
    const auto *image = source_image.GetImage(0, 0, 0);
    if (!image || !image->pixels || image->rowPitch < row_size)
    {
        unmap();
        return HdrCaptureFailure("HDR capture image storage disagrees with the readback footprint.");
    }
    for (std::uint32_t row = 0; row < height; ++row)
    {
        const auto *source_row = mapped + footprint.Offset +
            static_cast<std::size_t>(row) * footprint.Footprint.RowPitch;
        std::memcpy(image->pixels + static_cast<std::size_t>(row) * image->rowPitch,
                    source_row, static_cast<std::size_t>(row_size));
    }

    DirectX::ScratchImage float_image;
    result = DirectX::Convert(*image, DXGI_FORMAT_R32G32B32A32_FLOAT,
                              DirectX::TEX_FILTER_DEFAULT, 0.0f, float_image);
    if (FAILED(result) || !float_image.GetImages())
    {
        unmap();
        return HResultFailure("Convert HDR capture to FLOAT RGB", FAILED(result) ? result : E_FAIL);
    }
    result = DirectX::SaveToHDRFile(*float_image.GetImages(), path.c_str());
    unmap();
    if (FAILED(result))
        return HResultFailure("Write Radiance HDR capture", result);

    const auto metadata_path = path.parent_path() /
        (path.stem().string() + ".metadata.json");
    std::ofstream metadata(metadata_path, std::ios::trunc);
    if (!metadata)
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               "Could not open HDR capture metadata output.");
    const auto source_name = impl.hdr_capture_source_name.empty()
        ? std::string{} : impl.hdr_capture_source_name;
    metadata << "{\n"
             << "  \"schema_version\": 1,\n"
             << "  \"capture_kind\": \"hdr_color\",\n"
             << "  \"source_stage\": \"reconstructed_scene_hdr_pre_post_processing\",\n"
             << "  \"transparent_vfx_included\": true,\n"
             << "  \"view_id\": " << impl.active_view_id.value << ",\n"
             << "  \"file_format\": \"Radiance RGBE (.hdr)\",\n"
             << "  \"source_resource\": "
             << (source_name.empty() ? "null" : JsonString(source_name)) << ",\n"
             << "  \"source_native_format\": \"" << FormatName(source_description.Format)
             << "\",\n"
             << "  \"source_graph_access\": "
             << static_cast<unsigned>(source_resource.graph_access) << ",\n"
             << "  \"source_graph_queue\": "
             << static_cast<unsigned>(source_resource.graph_queue) << ",\n"
             << "  \"extent\": {\"width\": " << width << ", \"height\": " << height << "},\n"
             << "  \"color_space\": \"rec709_linear\",\n"
             << "  \"units\": \"pre_exposed_scene_linear\",\n"
             << "  \"pre_exposure_semantics\": \""
             << (impl.hdr_capture_pre_exposed ? "pre_exposed_scene_linear"
                                               : "scene_linear")
             << "\",\n"
             << "  \"display_hdr_active\": " << (impl.display_state.hdr_active ? "true" : "false")
             << ",\n"
             << "  \"frame\": {\n"
             << "    \"submission_id\": " << JsonNumber(impl.hdr_capture_submission_id) << ",\n"
             << "    \"submitted_view_id\": " << JsonNumber(impl.active_view_id.value) << ",\n"
             << "    \"render_frame_id\": " << JsonNumber(impl.frame_number) << ",\n"
             << "    \"render_package_id\": " << JsonNumber(impl.hdr_capture_package_id) << ",\n"
             << "    \"view_session_id\": " << JsonNumber(impl.view_session_id) << ",\n"
             << "    \"snapshot_tick\": " << JsonNumber(impl.hdr_capture_snapshot_tick) << ",\n"
             << "    \"content_generation\": "
             << (impl.resource_generation_id.empty() ? "null"
                                                       : JsonString(impl.resource_generation_id))
             << "\n  }\n}\n";
    if (!metadata)
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               "Could not write HDR capture metadata output.");
    return Result::Success();
}
} // namespace

Result D3D12Renderer::CaptureSurfaceDiagnostics(const std::filesystem::path &path)
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
    if (!path.parent_path().empty())
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                                   "Could not create surface diagnostic directory.");
    }

    const auto contract_failure = [](std::string message) {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
    };
    std::vector<SurfaceCapture> surfaces{
        SurfaceCapture{"gbuffer_metadata", &impl_->gbuffer_metadata,
                       DXGI_FORMAT_R32G32_UINT, Access::ShaderRead},
        SurfaceCapture{"gbuffer_normal", &impl_->gbuffer_normal,
                       DXGI_FORMAT_R16G16_SNORM, Access::ShaderRead},
        SurfaceCapture{"gbuffer_material", &impl_->gbuffer_material,
                       DXGI_FORMAT_R8G8B8A8_UNORM, Access::ShaderRead},
        SurfaceCapture{"gbuffer_motion", &impl_->gbuffer_motion,
                       DXGI_FORMAT_R16G16_FLOAT, Access::ShaderRead},
        SurfaceCapture{"surface_history", &impl_->gbuffer_surface_history,
                       DXGI_FORMAT_R32G32_FLOAT, Access::ShaderRead},
        SurfaceCapture{"depth", &impl_->depth, DXGI_FORMAT_R32_TYPELESS, Access::DepthWrite},
        SurfaceCapture{"gbuffer_base", &impl_->gbuffer_base,
                       DXGI_FORMAT_R8G8B8A8_UNORM, Access::ShaderRead},
        SurfaceCapture{"lighting_hdr", &impl_->hdr_color,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, Access::ShaderRead},
        SurfaceCapture{"composite_hdr", &impl_->composite_hdr,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, Access::ShaderRead},
        SurfaceCapture{"scene_work", &impl_->scene_work,
                       DXGI_FORMAT_R16G16B16A16_FLOAT, Access::ShaderRead},
        SurfaceCapture{"oit_accumulation", &impl_->oit_accumulation,
                       DXGI_FORMAT_R32G32B32A32_FLOAT, Access::ShaderRead},
        SurfaceCapture{"oit_revealage", &impl_->oit_revealage,
                       DXGI_FORMAT_R16_FLOAT, Access::ShaderRead},
        SurfaceCapture{"shadow_atlas", &impl_->shadow,
                       DXGI_FORMAT_R32_TYPELESS, Access::DepthWrite},
    };
    for (std::uint32_t plane = 0; plane < impl_->material_sidecar_layout.plane_count; ++plane)
        surfaces.push_back(SurfaceCapture{plane == 0 ? "material_sidecar_0" : "material_sidecar_1",
            &impl_->material_sidecars[plane], impl_->MaterialSidecarFormat(plane), Access::ShaderRead});
    const auto append_diagnostic = [&](const char *name, const AllocationResource *resource) {
        if (resource && resource->resource && resource->graph_access != Access::Undefined)
            surfaces.push_back(SurfaceCapture{name, resource, resource->resource->GetDesc().Format,
                                               resource->graph_access});
    };
    const auto history_current = impl_->temporal_read_index;
    const auto history_previous = 1u - history_current;
    append_diagnostic("temporal_current", &impl_->temporal_history[history_current]);
    append_diagnostic("temporal_previous", &impl_->temporal_history[history_previous]);
    append_diagnostic("temporal_normal_previous", &impl_->temporal_normal_history[history_previous]);
    append_diagnostic("temporal_material_previous", &impl_->temporal_material_history[history_previous]);
    append_diagnostic("exposure_current", &impl_->exposure_values[impl_->exposure_read_index]);
    append_diagnostic("exposure_previous", &impl_->exposure_values[1u - impl_->exposure_read_index]);
    append_diagnostic("exposure_histogram", &impl_->exposure_histogram);
    constexpr std::array ao_names{"ao_raw", "ao_resolved", "ao_contact", "ao_history", "ao_previous_history"};
    const auto ao_resources = impl_->ambient_occlusion.DiagnosticResources();
    for (std::size_t index = 0; index < ao_resources.size(); ++index)
        append_diagnostic(ao_names[index], ao_resources[index]);
    const auto adaptive_resources = impl_->adaptive_shading.DiagnosticResources();
    append_diagnostic("adaptive_density", adaptive_resources[0]);
    append_diagnostic("adaptive_rate", adaptive_resources[1]);
    constexpr std::array shadow_names{"virtual_shadow_metadata", "virtual_shadow_pages",
        "virtual_shadow_static", "virtual_shadow_dynamic", "virtual_shadow_counts"};
    const auto shadow_resources = impl_->virtual_shadows.DiagnosticResources();
    for (std::size_t index = 0; index < shadow_resources.size(); ++index)
        append_diagnostic(shadow_names[index], shadow_resources[index]);
    constexpr std::array light_names{"light_records", "light_ranges", "light_inline", "light_spill"};
    const auto light_resources = impl_->light_list.DiagnosticResources();
    for (std::size_t index = 0; index < light_resources.resources.size(); ++index)
        append_diagnostic(light_names[index], light_resources.resources[index]);
    for (auto &surface : surfaces)
    {
        if (!surface.resource->resource || surface.resource->graph_access != surface.final_access)
        {
            return contract_failure(std::format(
                "Surface diagnostic resource {} has graph access {}, expected {}.", surface.name,
                static_cast<unsigned>(surface.resource->graph_access),
                static_cast<unsigned>(surface.final_access)));
        }
        const auto description = surface.resource->resource->GetDesc();
        if (description.Format != surface.format || description.Width == 0 ||
            description.Height == 0)
        {
            return contract_failure(std::format(
                "Surface diagnostic resource {} has an unexpected native description.",
                surface.name));
        }
        CaptureNativeAccess native{};
        if (!GetCaptureNativeAccess(surface.final_access,
                description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER, native))
            return contract_failure("Surface diagnostic resource has an unknown native access.");
        surface.final_state = native.state;
        surface.final_sync = native.sync;
        surface.final_native_access = native.access;
        surface.final_layout = native.layout;
        surface.raw_path = path.parent_path() /
            (path.stem().string() + "." + surface.name + ".raw");
    }

    const auto width = static_cast<std::uint32_t>(
        surfaces.front().resource->resource->GetDesc().Width);
    const auto height = surfaces.front().resource->resource->GetDesc().Height;
    for (const auto &surface : std::span(surfaces).first(7))
    {
        const auto description = surface.resource->resource->GetDesc();
        if (description.Width != width || description.Height != height)
            return contract_failure("Surface diagnostic resources do not share an extent.");
    }

    UINT64 readback_size{};
    for (auto &surface : surfaces)
    {
        const auto description = surface.resource->resource->GetDesc();
        if (description.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            if (description.Width > UINT_MAX)
                return contract_failure("Surface diagnostic buffer exceeds the raw capture limit.");
            surface.footprint.Offset = (readback_size + 511u) & ~UINT64(511u);
            surface.footprint.Footprint.RowPitch = static_cast<UINT>(description.Width);
            surface.rows = 1;
            surface.row_size = description.Width;
            readback_size = surface.footprint.Offset + description.Width;
            continue;
        }
        UINT64 total{};
        impl_->device->GetCopyableFootprints(&description, 0, 1, readback_size,
                                             &surface.footprint, &surface.rows,
                                             &surface.row_size, &total);
        (void)total;
        if (surface.rows != description.Height || surface.row_size == 0 ||
            surface.footprint.Footprint.RowPitch < surface.row_size)
            return contract_failure(std::format("Surface diagnostic footprint for {} is invalid.",
                                                surface.name));
        readback_size = surface.footprint.Offset +
            static_cast<UINT64>(surface.footprint.Footprint.RowPitch) * surface.rows;
    }

    AllocationResource readback;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto result = impl_->CreateAllocation(readback, allocation,
                                               BufferDescription(readback_size),
                                               D3D12_RESOURCE_STATE_COPY_DEST);
        !result)
        return result;

    auto &frame = impl_->frames[0];
    auto result = frame.allocator->Reset();
    if (FAILED(result))
        return HResultFailure("Reset surface diagnostic allocator", result);
    result = impl_->command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result))
        return HResultFailure("Reset surface diagnostic list", result);

    for (auto &surface : surfaces)
    {
        if (surface.resource->resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            impl_->TransitionBuffer(surface.resource->resource.Get(), surface.final_state,
                D3D12_RESOURCE_STATE_COPY_SOURCE, surface.final_sync, D3D12_BARRIER_SYNC_COPY,
                surface.final_native_access, D3D12_BARRIER_ACCESS_COPY_SOURCE);
            impl_->command_list->CopyBufferRegion(readback.resource.Get(), surface.footprint.Offset,
                surface.resource->resource.Get(), 0, surface.row_size);
            impl_->TransitionBuffer(surface.resource->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                surface.final_state, D3D12_BARRIER_SYNC_COPY, surface.final_sync,
                D3D12_BARRIER_ACCESS_COPY_SOURCE, surface.final_native_access);
            continue;
        }
        impl_->TransitionTexture(surface.resource->resource.Get(), surface.final_state,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE, surface.final_layout,
                                 D3D12_BARRIER_LAYOUT_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = surface.footprint;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = surface.resource->resource.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        impl_->command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        impl_->TransitionTexture(surface.resource->resource.Get(),
                                 D3D12_RESOURCE_STATE_COPY_SOURCE, surface.final_state,
                                 D3D12_BARRIER_LAYOUT_COPY_SOURCE, surface.final_layout);
    }
    const auto close_result = impl_->command_list->Close();
    if (FAILED(close_result))
        return HResultFailure("Close surface diagnostic list", close_result);
    ID3D12CommandList *lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    if (auto wait = impl_->WaitForGpu(); !wait)
        return wait;

    void *mapped_data{};
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
    result = readback.resource->Map(0, &read_range, &mapped_data);
    if (FAILED(result))
        return HResultFailure("Map surface diagnostics", result);
    const auto *mapped = static_cast<const std::byte *>(mapped_data);
    const auto unmap_failure = [&](std::string message) {
        D3D12_RANGE no_write{};
        readback.resource->Unmap(0, &no_write);
        return contract_failure(std::move(message));
    };
    const auto pixel = [&](const SurfaceCapture &surface, std::uint32_t x,
                           std::uint32_t y) {
        return mapped + surface.footprint.Offset +
            static_cast<std::size_t>(y) * surface.footprint.Footprint.RowPitch +
            static_cast<std::size_t>(x) * (surface.row_size / width);
    };
    const auto &metadata = surfaces[0];
    const auto &motion = surfaces[3];
    const auto &history = surfaces[4];
    const auto &depth = surfaces[5];
    std::uint64_t foreground_depth_pixels{};
    std::uint64_t background_depth_pixels{};
    std::uint64_t foreground_valid_metadata_pixels{};
    std::uint64_t background_invalid_metadata_pixels{};
    std::uint64_t historical_foreground_pixels{};
    std::uint64_t nonzero_motion_pixels{};
    std::uint64_t grass_pixels{}, historical_grass_pixels{}, moving_grass_pixels{};
    float grass_motion_max_pixels{};
    float depth_min = 1.0f;
    float depth_max = 0.0f;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const auto depth_value = ReadFloat(pixel(depth, x, y));
            const auto metadata_material = ReadU32(pixel(metadata, x, y));
            const auto metadata_flags = ReadU32(pixel(metadata, x, y) + sizeof(std::uint32_t));
            const auto motion_x = ReadHalf(pixel(motion, x, y));
            const auto motion_y = ReadHalf(pixel(motion, x, y) + sizeof(std::uint16_t));
            const auto history_depth = ReadFloat(pixel(history, x, y));
            const auto history_confidence =
                ReadFloat(pixel(history, x, y) + sizeof(float));
            if (!std::isfinite(depth_value) || !std::isfinite(motion_x) ||
                !std::isfinite(motion_y) || !std::isfinite(history_depth) ||
                !std::isfinite(history_confidence))
                return unmap_failure("Surface diagnostics contain a non-finite sample.");
            if (history_confidence < 0.0f || history_confidence > 1.0f)
                return unmap_failure("Surface history confidence is outside [0,1].");
            const bool valid_metadata =
                (metadata_flags & render_data::SurfaceLayout::valid_flag) != 0 &&
                metadata_material != render_data::SurfaceLayout::invalid_material;
            if (depth_value > 0.0f)
            {
                if (depth_value > 1.0f)
                    return unmap_failure("Foreground depth is outside [0,1].");
                ++foreground_depth_pixels;
                depth_min = std::min(depth_min, depth_value);
                depth_max = std::max(depth_max, depth_value);
                if (!valid_metadata)
                    return unmap_failure("Foreground metadata is not marked valid.");
                ++foreground_valid_metadata_pixels;
                if ((metadata_flags & (3u << 8u)) == (2u << 8u))
                {
                    ++grass_pixels;
                    if (history_confidence > 0.0f)
                    {
                        ++historical_grass_pixels;
                        if (motion_x != 0.0f || motion_y != 0.0f) ++moving_grass_pixels;
                        grass_motion_max_pixels = std::max(grass_motion_max_pixels,
                            std::hypot(motion_x * width, motion_y * height));
                    }
                }
                if (history_confidence > 0.0f)
                {
                    ++historical_foreground_pixels;
                    if (motion_x != 0.0f || motion_y != 0.0f)
                        ++nonzero_motion_pixels;
                }
            }
            else if (depth_value == 0.0f)
            {
                ++background_depth_pixels;
                if (metadata_material != render_data::SurfaceLayout::invalid_material ||
                    metadata_flags != render_data::SurfaceLayout::invalid_material)
                    return unmap_failure("Background metadata is not cleared.");
                ++background_invalid_metadata_pixels;
            }
            else
            {
                return unmap_failure("Depth contains a negative value.");
            }
        }
    }
    // Viewport coverage is a fixture choice: a valid frame may contain only
    // foreground or only cleared background. Validate every sampled surface.
    if (foreground_valid_metadata_pixels != foreground_depth_pixels ||
        background_invalid_metadata_pixels != background_depth_pixels)
        return unmap_failure("Surface diagnostic contract counters are invalid.");

    for (auto &surface : surfaces)
    {
        std::ofstream raw(surface.raw_path, std::ios::binary | std::ios::trunc);
        if (!raw)
            return unmap_failure("Could not open surface diagnostic raw output.");
        const auto *source = mapped + surface.footprint.Offset;
        for (UINT row = 0; row < surface.rows; ++row)
            raw.write(reinterpret_cast<const char *>(source +
                         static_cast<std::size_t>(row) * surface.footprint.Footprint.RowPitch),
                      static_cast<std::streamsize>(surface.row_size));
        if (!raw)
            return unmap_failure("Could not write surface diagnostic raw output.");
    }

    std::ofstream metadata_output(path, std::ios::trunc);
    if (!metadata_output)
        return unmap_failure("Could not open surface diagnostic metadata output.");
    metadata_output << std::format(
        "{{\n  \"schema_version\": 1,\n  \"width\": {},\n  \"height\": {},\n  "
        "\"extent\": {{\"width\": {}, \"height\": {}}},\n  \"resources\": [\n",
        width, height, width, height);
    for (std::size_t index = 0; index < surfaces.size(); ++index)
    {
        const auto &surface = surfaces[index];
        const auto description = surface.resource->resource->GetDesc();
        metadata_output << std::format(
            "    {{\"name\": \"{}\", \"file\": \"{}\", \"width\": {}, "
            "\"height\": {}, \"format\": \"{}\", \"row_size\": {}, \"allocation_bytes\": {}}}{}\n",
            surface.name, surface.raw_path.filename().string(), description.Width, description.Height,
            FormatName(surface.format), surface.row_size,
            surface.resource->allocation ? surface.resource->allocation->GetSize() : 0,
            index + 1 == surfaces.size() ? "" : ",");
    }
    metadata_output << std::format(
        "  ],\n  \"light_list\": {{\"index_encoding\": \"bitset_u32x4\", "
        "\"allocation_bytes\": {}, \"upload_allocation_bytes\": {}, "
        "\"capacity\": {{\"width\": {}, \"height\": {}}}, "
        "\"frame_slot_count\": {}, \"frame_slot\": {}}},\n",
        light_resources.allocated_bytes, light_resources.upload_allocated_bytes,
        light_resources.capacity.width, light_resources.capacity.height,
        light_resources.frame_slot_count, light_resources.frame_slot);
    metadata_output << "  \"terrain_texture_mips\": [";
    bool first_texture = true;
    for (std::uint32_t slot = 0; slot < render_data::kMaterialTextureSlots; ++slot)
    {
        const auto id = impl_->material_texture_stream_ids[kTerrainMaterialOrdinal * render_data::kMaterialTextureSlots + slot];
        if (!id) continue;
        const auto view = impl_->texture_streaming.View(id);
        metadata_output << std::format(
            "{}{{\"slot\":{},\"width\":{},\"height\":{},\"mip_count\":{},\"physical_first_mip\":{},\"resident_min_mip\":{}}}",
            first_texture ? "" : ",", slot, view.logical_description.width, view.logical_description.height,
            view.logical_description.mip_count, view.first_mip, view.resident_min_mip);
        first_texture = false;
    }
    metadata_output << "],\n";
    constexpr std::array auxiliary_names{"subsurface_diffuse", "environment_specular", "indirect_diffuse"};
    const auto &captured_constants = *reinterpret_cast<const FrameConstants *>(
        impl_->frames[impl_->last_presented_index].mapped);
    metadata_output << std::format("  \"grass\": {{\"pixels\": {}, \"historical_pixels\": {}, \"moving_pixels\": {}, \"max_motion_pixels\": {}}},\n",
        grass_pixels, historical_grass_pixels, moving_grass_pixels, grass_motion_max_pixels);
    if (impl_->frames[impl_->last_presented_index].submitted_view_id == impl_->active_view_id.value)
    {
        metadata_output << "  \"temporal_inputs\": {\n";
        metadata_output << std::format("    \"emission_word\": {},\n", captured_constants.material_sidecar_offsets[1].x);
        const auto write_floats = [&](const char *name, const float *values, std::size_t count) {
            metadata_output << "    \"" << name << "\": [";
            for (std::size_t i = 0; i < count; ++i)
                metadata_output << (i ? "," : "") << values[i];
            metadata_output << "],\n";
        };
        write_floats("jitter_uv", &captured_constants.surface_jitter_uv.x, 4);
        write_floats("options", &captured_constants.temporal_options.x, 4);
        write_floats("camera_time", &captured_constants.camera_time.x, 4);
        write_floats("previous_grass_state", &captured_constants.previous_grass_state.x, 4);
        write_floats("previous_grass_origin_delta", &captured_constants.previous_grass_origin_delta.x, 4);
        write_floats("grass_benders", &captured_constants.grass_benders[0].x,
            std::min(captured_constants.grass_bender_count.x, 32u) * 4u);
        write_floats("previous_grass_benders", &captured_constants.previous_grass_benders[0].x,
            static_cast<std::uint32_t>(std::clamp(captured_constants.previous_grass_state.y, 0.0f, 32.0f)) * 4u);
        write_floats("current_vp", &captured_constants.current_unjittered_view_projection._11, 16);
        write_floats("previous_vp", &captured_constants.previous_unjittered_view_projection._11, 16);
        metadata_output << std::format("    \"view_id\": {}, \"frame\": {}, \"history_current_index\": {}\n  }},\n",
            impl_->active_view_id.value, impl_->frame_number, history_current);
    }
    else metadata_output << "  \"temporal_inputs\": null,\n";
    const std::array auxiliary_resources{&impl_->subsurface_diffuse, &impl_->environment_specular, &impl_->indirect_diffuse};
    metadata_output << "  \"auxiliary_targets\": [\n";
    for (std::size_t index = 0; index < auxiliary_resources.size(); ++index)
    {
        const auto *resource = auxiliary_resources[index];
        metadata_output << std::format(
            "    {{\"name\": \"{}\", \"present\": {}, \"allocation_bytes\": {}}}{}\n",
            auxiliary_names[index], resource->resource ? "true" : "false",
            resource->allocation ? resource->allocation->GetSize() : 0,
            index + 1 == auxiliary_resources.size() ? "" : ",");
    }
    metadata_output << "  ],\n";
    metadata_output << std::format(
        "  \"foreground_depth_pixels\": {},\n  \"background_depth_pixels\": {},\n"
        "  \"foreground_valid_metadata_pixels\": {},\n  \"background_invalid_metadata_pixels\": {},\n"
        "  \"historical_foreground_pixels\": {},\n  \"nonzero_motion_pixels\": {},\n"
        "  \"depth_min\": {},\n  \"depth_max\": {}\n}}\n",
        foreground_depth_pixels, background_depth_pixels, foreground_valid_metadata_pixels,
        background_invalid_metadata_pixels, historical_foreground_pixels, nonzero_motion_pixels,
        depth_min, depth_max);
    if (!metadata_output)
        return unmap_failure("Could not write surface diagnostic metadata output.");
    D3D12_RANGE no_write{};
    readback.resource->Unmap(0, &no_write);
    return Result::Success();
}

Result D3D12Renderer::CaptureGeometryDiagnostics(const std::filesystem::path &path)
{
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }
    if (auto result = impl_->WaitForGpu(); !result)
        return result;
    if (!path.parent_path().empty())
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                                   "Could not create geometry diagnostic directory.");
    }

    const auto contract_failure = [](std::string message) {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
    };
    auto &frame = impl_->frames[impl_->last_presented_index];
    const auto used_records = frame.geometry_draw_records_cpu.size();
    if (used_records == 0)
        return contract_failure("Geometry diagnostics have no CPU draw records.");

    struct BufferCapture
    {
        const char *name{};
        AllocationResource *resource{};
        UINT64 offset{};
        UINT64 size{};
        D3D12_RESOURCE_STATES native_state{};
        D3D12_BARRIER_SYNC native_sync{};
        D3D12_BARRIER_ACCESS native_access{};
    };
    std::array buffers{
        BufferCapture{"geometry_records", &frame.geometry_draw_records},
        BufferCapture{"static_visible_refs", &frame.static_visible_refs},
        BufferCapture{"dynamic_visible_refs", &frame.dynamic_visible_refs},
        BufferCapture{"static_retest_refs", &frame.static_retest_refs},
    };
    constexpr auto copy_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr auto copy_sync = D3D12_BARRIER_SYNC_COPY;
    constexpr auto copy_access = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    UINT64 readback_size{};
    for (std::size_t index = 0; index < buffers.size(); ++index)
    {
        auto &buffer = buffers[index];
        if (!buffer.resource->resource)
            return contract_failure(std::format(
                "Geometry diagnostic resource {} has no native resource.", buffer.name));
        switch (buffer.resource->graph_access)
        {
        case Access::CopyDest:
            buffer.native_state = D3D12_RESOURCE_STATE_COPY_DEST;
            buffer.native_sync = D3D12_BARRIER_SYNC_COPY;
            buffer.native_access = D3D12_BARRIER_ACCESS_COPY_DEST;
            break;
        case Access::UnorderedWrite:
            buffer.native_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            buffer.native_sync = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            buffer.native_access = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            break;
        case Access::ShaderRead:
            buffer.native_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            buffer.native_sync = D3D12_BARRIER_SYNC_ALL_SHADING;
            buffer.native_access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
            break;
        case Access::ComputeRead:
            buffer.native_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            buffer.native_sync = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            buffer.native_access = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
            break;
        default:
            return contract_failure(std::format(
                "Geometry diagnostic resource {} has unsupported graph access {}.", buffer.name,
                static_cast<unsigned>(buffer.resource->graph_access)));
        }
        buffer.size = buffer.resource->resource->GetDesc().Width;
        if (buffer.size == 0 || (index != 0 && buffer.size % sizeof(GpuStaticDrawRef) != 0))
            return contract_failure(std::format(
                "Geometry diagnostic resource {} has an invalid byte capacity.", buffer.name));
        if (index == 0 && buffer.size < used_records * sizeof(renderer_detail::GpuGeometryDrawRecord))
            return contract_failure("Geometry records are smaller than their CPU record vector.");
        if (readback_size > std::numeric_limits<UINT64>::max() - buffer.size)
            return contract_failure("Geometry diagnostic readback size overflowed.");
        buffer.offset = readback_size;
        readback_size += buffer.size;
    }

    AllocationResource readback;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto result = impl_->CreateAllocation(readback, allocation,
                                               BufferDescription(readback_size),
                                               D3D12_RESOURCE_STATE_COPY_DEST);
        !result)
        return result;

    auto &capture_frame = impl_->frames[0];
    auto result = capture_frame.allocator->Reset();
    if (FAILED(result))
        return HResultFailure("Reset geometry diagnostic allocator", result);
    result = impl_->command_list->Reset(capture_frame.allocator.Get(), nullptr);
    if (FAILED(result))
        return HResultFailure("Reset geometry diagnostic list", result);
    for (const auto &buffer : buffers)
    {
        impl_->TransitionBuffer(buffer.resource->resource.Get(), buffer.native_state, copy_state,
                                buffer.native_sync, copy_sync, buffer.native_access, copy_access);
        impl_->command_list->CopyBufferRegion(readback.resource.Get(), buffer.offset,
                                               buffer.resource->resource.Get(), 0, buffer.size);
        impl_->TransitionBuffer(buffer.resource->resource.Get(), copy_state, buffer.native_state,
                                copy_sync, buffer.native_sync, copy_access, buffer.native_access);
    }
    result = impl_->command_list->Close();
    if (FAILED(result))
        return HResultFailure("Close geometry diagnostic list", result);
    ID3D12CommandList *lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    if (auto wait = impl_->WaitForGpu(); !wait)
        return wait;

    void *mapped_data{};
    D3D12_RANGE read_range{0, static_cast<SIZE_T>(readback_size)};
    result = readback.resource->Map(0, &read_range, &mapped_data);
    if (FAILED(result))
        return HResultFailure("Map geometry diagnostics", result);
    const auto *mapped = static_cast<const std::byte *>(mapped_data);
    const auto fail_mapped = [&](std::string message) {
        D3D12_RANGE no_write{};
        readback.resource->Unmap(0, &no_write);
        return contract_failure(std::move(message));
    };
    const auto *record_bytes = mapped + buffers[0].offset;
    const auto *static_ref_bytes = mapped + buffers[1].offset;
    const auto *dynamic_ref_bytes = mapped + buffers[2].offset;
    const auto *retest_ref_bytes = mapped + buffers[3].offset;
    const auto upload_bytes = frame.upload.resource
        ? frame.upload.resource->GetDesc().Width : UINT64{};
    const auto input_ref_range_valid = [&](std::uint64_t offset,
                                           std::uint32_t count) {
        if (count == 0)
            return true;
        if (!frame.mapped || !frame.upload.resource ||
            offset > upload_bytes ||
            count > (upload_bytes - offset) / sizeof(GpuStaticDrawRef))
            return false;
        return true;
    };
    if (!input_ref_range_valid(frame.static_input_ref_offset,
                               frame.static_input_ref_count) ||
        !input_ref_range_valid(frame.dynamic_input_ref_offset,
                               frame.dynamic_input_ref_count))
        return fail_mapped("Built-in geometry ref upload range is invalid.");
    const auto *static_input_ref_bytes = frame.static_input_ref_count == 0
        ? nullptr : frame.mapped + frame.static_input_ref_offset;
    const auto *dynamic_input_ref_bytes = frame.dynamic_input_ref_count == 0
        ? nullptr : frame.mapped + frame.dynamic_input_ref_offset;
    const auto read_u32_at = [](const std::byte *bytes, std::size_t offset) {
        return ReadU32(bytes + offset);
    };
    struct RefDiagnostic
    {
        std::uint32_t instance_index{};
        std::uint32_t mesh_flags{};
        float lod_threshold{};
        std::uint32_t instance_generation{};
    };
    struct RecordDiagnostic
    {
        std::uint32_t id{};
        std::uint32_t view{};
        std::uint32_t mesh{};
        std::uint32_t page{};
        std::uint32_t generation{};
        std::uint32_t physical_lod{};
        std::uint32_t base{};
        std::uint32_t count{};
        float local_error{};
        std::uint32_t ref_source{};
        std::vector<RefDiagnostic> refs;
    };
    std::vector<RecordDiagnostic> records;
    records.reserve(used_records);
    std::array<std::uint32_t, 6> source_views{};
    std::uint64_t source_counts[6]{};
    const auto ref_base = [&](std::uint32_t source) -> const std::byte * {
        switch (source)
        {
        case 1: return static_ref_bytes;
        case 2: return dynamic_ref_bytes;
        case 3: return retest_ref_bytes;
        case 4: return static_input_ref_bytes;
        case 5: return dynamic_input_ref_bytes;
        default: return nullptr;
        }
    };
    const auto ref_capacity = [&](std::uint32_t source) -> UINT64 {
        switch (source)
        {
        case 1:
        case 2:
        case 3: return buffers[source].size / sizeof(GpuStaticDrawRef);
        case 4: return frame.static_input_ref_count;
        case 5: return frame.dynamic_input_ref_count;
        default: return 0;
        }
    };
    for (std::size_t index = 0; index < used_records; ++index)
    {
        const auto *bytes = record_bytes + index * sizeof(renderer_detail::GpuGeometryDrawRecord);
        RecordDiagnostic record;
        record.id = static_cast<std::uint32_t>(index);
        record.page = read_u32_at(bytes, 0);
        record.base = read_u32_at(bytes, 20);
        record.count = read_u32_at(bytes, 24);
        record.mesh = read_u32_at(bytes, 48);
        record.generation = read_u32_at(bytes, 40);
        record.physical_lod = read_u32_at(bytes, 44);
        record.local_error = ReadFloat(bytes + 52);
        record.ref_source = read_u32_at(bytes, 56);
        if (!std::isfinite(record.local_error) || record.ref_source > 5)
            return fail_mapped(std::format(
                "Geometry record {} has a non-finite error or invalid ref source.", index));
        record.view = record.ref_source == 3 || record.ref_source == 0
            ? 0 : source_views[record.ref_source]++ % 4;
        const auto cpu_instance_capacity = frame.geometry_draw_records_cpu[index].instance_count;
        if (record.count > cpu_instance_capacity)
            return fail_mapped(std::format("Geometry record {} count exceeds CPU capacity.", index));
        if (record.ref_source != 0)
        {
            const auto capacity = ref_capacity(record.ref_source);
            if (record.base > capacity || record.count > capacity - record.base)
                return fail_mapped(std::format(
                    "Geometry record {} visible range exceeds its ref buffer.", index));
            const auto *refs = ref_base(record.ref_source);
            record.refs.reserve(record.count);
            for (std::uint32_t ref_index = 0; ref_index < record.count; ++ref_index)
            {
                const auto *ref = refs +
                    static_cast<std::size_t>(record.base + ref_index) * sizeof(GpuStaticDrawRef);
                RefDiagnostic value;
                value.instance_index = ReadU32(ref);
                value.mesh_flags = ReadU32(ref + 4);
                value.lod_threshold = ReadFloat(ref + 8);
                value.instance_generation = ReadU32(ref + 12);
                if (!std::isfinite(value.lod_threshold) || value.lod_threshold < 0.0f ||
                    value.lod_threshold > 1.0f)
                    return fail_mapped(std::format(
                        "Geometry record {} has a visible ref threshold outside [0,1].", index));
                record.refs.push_back(value);
            }
            source_counts[record.ref_source] += record.count;
        }
        records.push_back(std::move(record));
    }
    D3D12_RANGE no_write{};
    readback.resource->Unmap(0, &no_write);

    std::ofstream output(path, std::ios::trunc);
    if (!output)
        return contract_failure("Could not open geometry diagnostic metadata output.");
    output << std::format(
        "{{\n  \"schema_version\": 1,\n  \"used_record_count\": {},\n"
        "  \"record_bytes\": {},\n  \"visible_ref_stride\": {},\n"
        "  \"visible_ref_counts\": {{\"static\": {}, \"dynamic\": {}, "
        "\"static_retest\": {}, \"static_input\": {}, "
        "\"dynamic_input\": {}}},\n  \"records\": [\n",
        used_records, sizeof(renderer_detail::GpuGeometryDrawRecord), sizeof(GpuStaticDrawRef),
        source_counts[1], source_counts[2], source_counts[3], source_counts[4], source_counts[5]);
    for (std::size_t index = 0; index < records.size(); ++index)
    {
        const auto &record = records[index];
        output << std::format(
            "    {{\"id\": {}, \"view\": {}, \"mesh\": {}, \"page\": {}, "
            "\"gen\": {}, \"physicalLOD\": {}, \"base\": {}, \"count\": {}, "
            "\"localerror\": {}, \"refsource\": {}, \"visible_refs\": [",
            record.id, record.view, record.mesh, record.page, record.generation,
            record.physical_lod, record.base, record.count, record.local_error,
            record.ref_source);
        for (std::size_t ref_index = 0; ref_index < record.refs.size(); ++ref_index)
        {
            const auto &ref = record.refs[ref_index];
            output << std::format(
                "{{\"InstanceIndex\": {}, \"MeshFlags\": {}, "
                "\"LodThreshold\": {}, \"InstanceGeneration\": {}, "
                "\"complement\": {}, \"history_invalid\": {}}}{}",
                ref.instance_index, ref.mesh_flags, ref.lod_threshold,
                ref.instance_generation, (ref.mesh_flags & 0x20000u) != 0,
                (ref.mesh_flags & 0x100000u) != 0,
                ref_index + 1 == record.refs.size() ? "" : ", ");
        }
        output << std::format(" ]}}{}\n", index + 1 == records.size() ? "" : ",");
    }
    output << "  ]\n}\n";
    if (!output)
        return contract_failure("Could not write geometry diagnostic metadata output.");
    return Result::Success();
}

Result D3D12Renderer::CaptureVisibilityDiagnostics(const std::filesystem::path &path)
{
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }
    if (impl_->config.surface_frontend != SurfaceFrontend::VisibilityBuffer)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Visibility diagnostics require the VisibilityBuffer frontend.");
    }
    if (auto result = impl_->WaitForGpu(); !result)
        return result;
    if (!path.parent_path().empty())
    {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                                   "Could not create visibility diagnostic directory.");
    }

    const auto json_escape = [](std::string_view text) {
        std::string escaped;
        escaped.reserve(text.size());
        for (const char value : text)
        {
            switch (value)
            {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += value; break;
            }
        }
        return escaped;
    };
    const auto invalid = [&](std::string message) {
        std::ofstream output(path, std::ios::trunc);
        if (!output)
            return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                                   "Could not write invalid visibility diagnostic JSON.");
        output << std::format(
            "{{\n  \"schema_version\": 1,\n  \"execution_valid\": false,\n"
            "  \"declared_formats\": {{\"visibility_ids\": \"DXGI_FORMAT_R32G32_UINT\", "
            "\"visibility_front_face\": \"DXGI_FORMAT_R8_UINT\", "
            "\"gbuffer_metadata\": \"DXGI_FORMAT_R32G32_UINT\"}},\n"
            "  \"sentinel\": {{\"visibility_ids\": [4294967295, 4294967295], "
            "\"front_face_clear\": 0}},\n  \"error\": \"{}\"\n}}\n",
            json_escape(message));
        if (!output)
            return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                                   "Could not write invalid visibility diagnostic JSON.");
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
    };

    if (impl_->last_presented_index >= kFrameCount)
        return invalid("Last presented frame index is outside the frame ring.");
    auto &frame = impl_->frames[impl_->last_presented_index];
    auto &visibility_frame = impl_->visibility_frames[impl_->last_presented_index];
    auto *cluster_frame = impl_->cluster_runtime
        ? &impl_->cluster_runtime->frames[impl_->last_presented_index] : nullptr;

    struct CommandDiagnostic
    {
        std::uint32_t published{}, nonzero{}, last_nonzero_exclusive{}, outside_prefix_nonzero{};
        std::uint64_t draw_vertices{};
        std::uint64_t argument_offset{};
    };
    static_assert(kGeometryArgumentStride == 24); // Root constant + DRAW words and padding.
    struct CutCapture
    {
        std::uint32_t ordinal{};
        ClusterCutBuffers *cut{};
        GpuClusterRefinementConstants constants{};
        GpuClusterCutStatus status{};
        std::size_t status_buffer_index{std::numeric_limits<std::size_t>::max()};
        std::array<std::size_t, 6> command_buffer_indices{};
        std::array<CommandDiagnostic, 5> commands{};
    };
    std::vector<CutCapture> cuts;
    if (cluster_frame)
    {
        cuts.reserve(kClusterRuntimeCatalogs);
        for (std::uint32_t ordinal = 0; ordinal < kClusterRuntimeCatalogs; ++ordinal)
        {
            auto &cut = cluster_frame->cuts[ordinal];
            const bool active = cut.candidate_count != 0 || cut.actual_frame_id != 0;
            if (!active)
                continue;
            if (!cut.candidate_count || !cut.actual_frame_id || !cut.generation ||
                !cut.constants.resource || !cut.status.resource)
                return invalid(std::format(
                    "Active cluster cut {} has incomplete constants/status identity.", ordinal));
            if (cut.constants.resource->GetDesc().Width < sizeof(GpuClusterRefinementConstants))
                return invalid(std::format(
                    "Active cluster cut {} constants are smaller than their ABI.", ordinal));

            void *mapped_constants{};
            const D3D12_RANGE read_range{0, sizeof(GpuClusterRefinementConstants)};
            const auto hr = cut.constants.resource->Map(0, &read_range, &mapped_constants);
            if (FAILED(hr))
                return HResultFailure("Map cluster visibility diagnostic constants", hr);
            CutCapture capture;
            capture.ordinal = ordinal;
            capture.cut = &cut;
            std::memcpy(&capture.constants, mapped_constants,
                        sizeof(capture.constants));
            const D3D12_RANGE no_write{};
            cut.constants.resource->Unmap(0, &no_write);
            // The normal graph already copied this frame's status after Emit.
            // Reuse that completed readback to bound argument copies, with no
            // additional submission/wait or dependence on an older last_status.
            const auto status_offset = impl_->cluster_runtime->cpu.pages.size() *
                sizeof(std::uint32_t);
            if (!cut.readback.resource || cut.readback.resource->GetDesc().Width <
                    status_offset + sizeof(capture.status))
                return invalid("Cluster command diagnostic has no complete status readback.");
            void *mapped_status{};
            const D3D12_RANGE status_range{status_offset, status_offset + sizeof(capture.status)};
            const auto status_hr = cut.readback.resource->Map(0, &status_range, &mapped_status);
            if (FAILED(status_hr)) return HResultFailure("Map cluster command diagnostic status", status_hr);
            std::memcpy(&capture.status, static_cast<const std::byte *>(mapped_status) +
                status_offset, sizeof(capture.status));
            cut.readback.resource->Unmap(0, &no_write);
            const auto &status = capture.status;
            const auto status_frame = (static_cast<std::uint64_t>(status.frame_id_high) << 32u) |
                status.frame_id_low;
            if (status.reserved || status_frame != cut.actual_frame_id ||
                status.catalog_index != ordinal || status.content_generation != cut.generation ||
                cut.generation != impl_->cluster_runtime->generation ||
                status.candidate_count != cut.candidate_count ||
                status.committed_records > cut.output_capacity ||
                (status.failure_flags & ~kClusterCutFailureCapacity) ||
                (status.failure_flags && status.committed_records) ||
                status.required_records_high || status.required_commands_high ||
                status.required_records_low > UINT32_MAX / 96u ||
                status.required_commands_low > UINT32_MAX / 24u)
                return invalid("Cluster command diagnostic status is stale or violates its transaction.");
            cuts.push_back(std::move(capture));
        }
    }

    struct TextureCapture
    {
        const char *name{};
        AllocationResource *resource{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        std::uint32_t bytes_per_pixel{};
        UINT64 offset{};
        UINT width{};
        UINT height{};
        UINT rows{};
        UINT64 row_size{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        CaptureNativeAccess native{};
    };
    std::array textures{
        TextureCapture{"visibility_ids", &impl_->visibility_ids, DXGI_FORMAT_R32G32_UINT, 8},
        TextureCapture{"visibility_front_face", &impl_->visibility_front_face,
                       DXGI_FORMAT_R8_UINT, 1},
        TextureCapture{"gbuffer_metadata", &impl_->gbuffer_metadata,
                       DXGI_FORMAT_R32G32_UINT, 8},
    };
    struct BufferCapture
    {
        std::string name;
        AllocationResource *resource{};
        UINT64 offset{};
        UINT64 size{};
        CaptureNativeAccess native{};
        std::uint32_t cut_ordinal{UINT32_MAX};
        UINT64 source_offset{};
    };
    std::vector<BufferCapture> buffers;
    buffers.reserve(3 + 2 + 7 * cuts.size());
    buffers.push_back({"visibility_pixels", &impl_->visibility_pixels});
    buffers.push_back({"visibility_counts", &impl_->visibility_counts});
    buffers.push_back({"raster_records", &visibility_frame.raster_records});
    const auto geometry_buffer_index = buffers.size();
    buffers.push_back({"geometry_draw_records", &frame.geometry_draw_records});
    std::size_t cluster_ref_buffer_index = std::numeric_limits<std::size_t>::max();
    if (!cuts.empty())
    {
        if (!cluster_frame->refs.resource)
            return invalid("Active cluster cuts have no current-frame reference buffer.");
        cluster_ref_buffer_index = buffers.size();
        buffers.push_back({"cluster_refs", &cluster_frame->refs});
    }
    UINT64 command_readback_bytes{};
    for (auto &cut : cuts)
    {
        cut.status_buffer_index = buffers.size();
        buffers.push_back({std::format("cluster_status_{}", cut.ordinal),
                           &cut.cut->status, 0, 0, {}, cut.ordinal});
        const auto argument_bytes = static_cast<UINT64>(cut.status.committed_records) *
            kGeometryArgumentStride;
        command_readback_bytes += 5 * argument_bytes + 5 * sizeof(std::uint32_t);
        if (command_readback_bytes > 512ull * 1024 * 1024)
            return invalid("Cluster command diagnostic prefixes exceed the 512 MiB capture budget.");
        cut.command_buffer_indices[0] = buffers.size();
        buffers.push_back({std::format("cluster_command_counts_{}", cut.ordinal),
                           &cut.cut->command_counts, 0, 5 * sizeof(std::uint32_t), {}, cut.ordinal});
        if (!argument_bytes) continue;
        for (std::size_t view = 0; view < cut.commands.size(); ++view)
        {
            cut.command_buffer_indices[view + 1] = buffers.size();
            const auto source_offset = view == 4 ? 0 : view *
                static_cast<UINT64>(cut.cut->output_capacity) * kGeometryArgumentStride;
            buffers.push_back({std::format("cluster_commands_{}_{}", cut.ordinal, view),
                               view == 4 ? &cut.cut->retest_draw_commands : &cut.cut->draw_commands,
                               0, argument_bytes, {}, cut.ordinal, source_offset});
        }
    }

    UINT64 readback_size{};
    for (auto &texture : textures)
    {
        if (!texture.resource->resource ||
            !GetCaptureNativeAccess(texture.resource->graph_access, true, texture.native))
            return invalid(std::format(
                "Visibility diagnostic texture {} has no resource or unsupported graph access.",
                texture.name));
        const auto description = texture.resource->resource->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.Format != texture.format || description.Width != impl_->render_width ||
            description.Height != impl_->render_height || description.DepthOrArraySize != 1 ||
            description.MipLevels != 1)
            return invalid(std::format(
                "Visibility diagnostic texture {} has an unexpected native description.",
                texture.name));
        texture.width = static_cast<UINT>(description.Width);
        texture.height = static_cast<UINT>(description.Height);
        UINT64 total_size{};
        // Query a local footprint. pTotalBytes is a size, not the absolute
        // end of the readback heap when BaseOffset is nonzero.
        impl_->device->GetCopyableFootprints(&description, 0, 1, 0,
                                             &texture.footprint, &texture.rows,
                                             &texture.row_size, &total_size);
        const auto row_pitch = static_cast<UINT64>(texture.footprint.Footprint.RowPitch);
        const auto last_row = texture.rows == 0 ? 0u : texture.rows - 1u;
        const auto max_uint64 = std::numeric_limits<UINT64>::max();
        const bool row_offset_overflow = static_cast<UINT64>(last_row) != 0 &&
            row_pitch > max_uint64 / static_cast<UINT64>(last_row);
        const auto last_row_offset = row_offset_overflow
            ? 0 : row_pitch * static_cast<UINT64>(last_row);
        const bool tail_offset_overflow = texture.footprint.Offset >
            max_uint64 - last_row_offset;
        const auto tail_offset = tail_offset_overflow
            ? 0 : texture.footprint.Offset + last_row_offset;
        const bool tail_size_overflow = texture.row_size > max_uint64 - tail_offset;
        const auto touched_end = tail_size_overflow ? 0 : tail_offset + texture.row_size;
        if (texture.rows == 0 || texture.rows != texture.height || texture.row_size !=
                static_cast<UINT64>(texture.width) * texture.bytes_per_pixel ||
            texture.footprint.Footprint.RowPitch < texture.row_size ||
            total_size == max_uint64 || row_offset_overflow || tail_offset_overflow ||
            tail_size_overflow || touched_end > total_size)
            return invalid(std::format(
                "Visibility diagnostic texture {} has an invalid copy footprint.", texture.name));
        constexpr UINT64 alignment = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        if (readback_size > max_uint64 - (alignment - 1))
            return invalid("Visibility diagnostic readback alignment overflowed.");
        const auto base_offset = (readback_size + alignment - 1) & ~(alignment - 1);
        if (total_size > max_uint64 - base_offset)
            return invalid("Visibility diagnostic readback size overflowed.");
        texture.footprint.Offset += base_offset;
        texture.offset = texture.footprint.Offset;
        readback_size = base_offset + total_size;
    }
    for (auto &buffer : buffers)
    {
        if (!buffer.resource->resource ||
            !GetCaptureNativeAccess(buffer.resource->graph_access, false, buffer.native))
            return invalid(std::format(
                "Visibility diagnostic buffer {} has no resource or unsupported graph access.",
                buffer.name));
        const auto description = buffer.resource->resource->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || description.Width == 0)
            return invalid(std::format(
                "Visibility diagnostic buffer {} has an invalid native description.",
                buffer.name));
        buffer.offset = readback_size;
        if (buffer.source_offset > description.Width ||
            buffer.size > description.Width - buffer.source_offset)
            return invalid(std::format("Visibility diagnostic buffer {} copy exceeds its allocation.",
                                       buffer.name));
        if (!buffer.size) buffer.size = description.Width - buffer.source_offset;
        if (buffer.size > std::numeric_limits<UINT64>::max() - readback_size)
            return invalid("Visibility diagnostic readback size overflowed.");
        readback_size += buffer.size;
    }
    if (buffers[0].size < static_cast<UINT64>(impl_->render_width) * impl_->render_height * 4u ||
        buffers[1].size < kVisibilityVariantCount * 3u * sizeof(std::uint32_t) || buffers[1].size % sizeof(std::uint32_t) ||
        buffers[2].size < kRasterRecordStride || buffers[2].size % kRasterRecordStride)
        return invalid("Visibility diagnostic buffers are smaller than their declared ABI.");
    if (buffers[geometry_buffer_index].size <
            frame.geometry_draw_records_cpu.size() * sizeof(renderer_detail::GpuGeometryDrawRecord) ||
        buffers[geometry_buffer_index].size % sizeof(renderer_detail::GpuGeometryDrawRecord))
        return invalid("GPU geometry records are smaller than their declared ABI or CPU prefix.");
    if (cluster_ref_buffer_index != std::numeric_limits<std::size_t>::max() &&
        (buffers[cluster_ref_buffer_index].size < sizeof(GpuStaticDrawRef) ||
         buffers[cluster_ref_buffer_index].size % sizeof(GpuStaticDrawRef)))
        return invalid("Cluster reference buffer is smaller than its declared ABI.");
    for (const auto &cut : cuts)
        if (buffers[cut.status_buffer_index].size != sizeof(GpuClusterCutStatus))
            return invalid(std::format("Cluster cut {} status buffer has an unexpected ABI size.",
                                       cut.ordinal));

    AllocationResource readback;
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (auto result = impl_->CreateAllocation(readback, allocation,
                                               BufferDescription(readback_size),
                                               D3D12_RESOURCE_STATE_COPY_DEST);
        !result)
        return result;

    auto &capture_frame = impl_->frames[0];
    auto result = capture_frame.allocator->Reset();
    if (FAILED(result))
        return HResultFailure("Reset visibility diagnostic allocator", result);
    result = impl_->command_list->Reset(capture_frame.allocator.Get(), nullptr);
    if (FAILED(result))
        return HResultFailure("Reset visibility diagnostic list", result);
    constexpr auto copy_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    constexpr auto copy_sync = D3D12_BARRIER_SYNC_COPY;
    constexpr auto copy_access = D3D12_BARRIER_ACCESS_COPY_SOURCE;
    for (const auto &texture : textures)
    {
        impl_->TransitionTexture(texture.resource->resource.Get(), texture.native.state, copy_state,
                                 texture.native.layout, D3D12_BARRIER_LAYOUT_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = texture.footprint;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = texture.resource->resource.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        impl_->command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        impl_->TransitionTexture(texture.resource->resource.Get(), copy_state,
                                 texture.native.state, D3D12_BARRIER_LAYOUT_COPY_SOURCE,
                                 texture.native.layout);
    }
    for (const auto &buffer : buffers)
    {
        impl_->TransitionBuffer(buffer.resource->resource.Get(), buffer.native.state, copy_state,
                                buffer.native.sync, copy_sync, buffer.native.access, copy_access);
        impl_->command_list->CopyBufferRegion(readback.resource.Get(), buffer.offset,
                                               buffer.resource->resource.Get(), buffer.source_offset, buffer.size);
        impl_->TransitionBuffer(buffer.resource->resource.Get(), copy_state, buffer.native.state,
                                copy_sync, buffer.native.sync, copy_access, buffer.native.access);
    }
    result = impl_->command_list->Close();
    if (FAILED(result))
        return HResultFailure("Close visibility diagnostic list", result);
    ID3D12CommandList *lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    if (auto wait = impl_->WaitForGpu(); !wait)
        return wait;

    void *mapped_data{};
    result = readback.resource->Map(0, nullptr, &mapped_data);
    if (FAILED(result))
        return HResultFailure("Map visibility diagnostics", result);
    const auto *mapped = static_cast<const std::byte *>(mapped_data);
    const auto fail_mapped = [&](std::string message) {
        D3D12_RANGE no_write{};
        readback.resource->Unmap(0, &no_write);
        return invalid(std::move(message));
    };

    const auto pixel_count = static_cast<std::size_t>(impl_->render_width) * impl_->render_height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 8u)
        return fail_mapped("Visibility diagnostic pixel count overflowed.");
    std::vector<std::uint32_t> visibility_ids(pixel_count * 2);
    std::vector<std::uint8_t> front_faces(pixel_count);
    std::vector<std::uint32_t> metadata(pixel_count * 2);
    const auto copy_texture_rows = [&](const TextureCapture &texture, std::byte *destination) {
        const auto tight_row_size = static_cast<std::size_t>(texture.width) *
            texture.bytes_per_pixel;
        const auto *source = mapped + texture.offset;
        for (UINT row = 0; row < texture.rows; ++row)
            std::memcpy(destination + static_cast<std::size_t>(row) * tight_row_size,
                        source + static_cast<std::size_t>(row) *
                            texture.footprint.Footprint.RowPitch,
                        tight_row_size);
    };
    copy_texture_rows(textures[0], reinterpret_cast<std::byte *>(visibility_ids.data()));
    copy_texture_rows(textures[1], reinterpret_cast<std::byte *>(front_faces.data()));
    copy_texture_rows(textures[2], reinterpret_cast<std::byte *>(metadata.data()));

    const auto pixel_bytes = static_cast<std::size_t>(buffers[0].size);
    const auto count_bytes = kVisibilityVariantCount * 3u * sizeof(std::uint32_t);
    const auto record_bytes = static_cast<std::size_t>(buffers[2].size);
    const auto geometry_record_bytes = static_cast<std::size_t>(
        buffers[geometry_buffer_index].size);
    std::vector<std::byte> visibility_pixels_raw(pixel_bytes);
    std::vector<std::byte> visibility_counts_raw(count_bytes);
    std::vector<std::byte> raster_records_raw(record_bytes);
    std::vector<std::byte> geometry_records_raw(geometry_record_bytes);
    std::vector<std::byte> cluster_refs_raw;
    if (cluster_ref_buffer_index != std::numeric_limits<std::size_t>::max())
        cluster_refs_raw.resize(static_cast<std::size_t>(
            buffers[cluster_ref_buffer_index].size));
    std::vector<std::byte> cluster_status_raw(cuts.size() * sizeof(GpuClusterCutStatus));
    std::vector<std::byte> cluster_command_counts_raw(cuts.size() * 5 * sizeof(std::uint32_t));
    std::vector<std::byte> cluster_commands_raw;
    cluster_commands_raw.reserve(static_cast<std::size_t>(command_readback_bytes));
    std::memcpy(visibility_pixels_raw.data(), mapped + buffers[0].offset, pixel_bytes);
    std::memcpy(visibility_counts_raw.data(), mapped + buffers[1].offset, count_bytes);
    std::memcpy(raster_records_raw.data(), mapped + buffers[2].offset, record_bytes);
    std::memcpy(geometry_records_raw.data(), mapped + buffers[geometry_buffer_index].offset,
                geometry_record_bytes);
    if (!cluster_refs_raw.empty())
        std::memcpy(cluster_refs_raw.data(), mapped + buffers[cluster_ref_buffer_index].offset,
                    cluster_refs_raw.size());
    for (std::size_t index = 0; index < cuts.size(); ++index)
    {
        GpuClusterCutStatus copied_status{};
        std::memcpy(&copied_status,
                    mapped + buffers[cuts[index].status_buffer_index].offset,
                    sizeof(copied_status));
        if (std::memcmp(&copied_status, &cuts[index].status, sizeof(copied_status)) != 0)
            return fail_mapped("Cluster command diagnostic status changed after prefix sizing.");
        std::memcpy(cluster_status_raw.data() + index * sizeof(cuts[index].status),
                    &cuts[index].status, sizeof(cuts[index].status));
        auto &cut = cuts[index];
        if (cut.status.committed_records > cut.cut->output_capacity)
            return fail_mapped("Cluster command diagnostic committed prefix exceeds capacity.");
        const auto *command_counts = mapped + buffers[cut.command_buffer_indices[0]].offset;
        std::memcpy(cluster_command_counts_raw.data() + index * 5 * sizeof(std::uint32_t),
                    command_counts, 5 * sizeof(std::uint32_t));
        for (std::size_t view = 0; view < cut.commands.size(); ++view)
        {
            auto &command = cut.commands[view];
            command.argument_offset = cluster_commands_raw.size();
            command.published = ReadU32(command_counts + view * sizeof(std::uint32_t));
            if (command.published > cut.status.committed_records)
                return fail_mapped("Cluster command diagnostic publishes outside its committed prefix.");
            if (!cut.status.committed_records) continue;
            const auto &arguments = buffers[cut.command_buffer_indices[view + 1]];
            cluster_commands_raw.insert(cluster_commands_raw.end(), mapped + arguments.offset,
                                        mapped + arguments.offset + arguments.size);
            // Only same-frame committed prefixes were copied; unpublished
            // allocation tails are neither copied nor interpreted as commands.
            for (std::uint32_t selection = 0; selection < cut.status.committed_records; ++selection)
            {
                const auto *argument = mapped + arguments.offset +
                    static_cast<UINT64>(selection) * kGeometryArgumentStride;
                const auto vertices = ReadU32(argument + 4);
                const auto instances = ReadU32(argument + 8);
                if (!vertices || !instances) continue;
                if (selection >= command.published)
                {
                    ++command.outside_prefix_nonzero;
                    continue;
                }
                ++command.nonzero;
                command.last_nonzero_exclusive = selection + 1;
                command.draw_vertices += static_cast<std::uint64_t>(vertices) * instances;
            }
        }
    }
    std::array<std::uint32_t, kVisibilityVariantCount * 3> counts{};
    std::memcpy(counts.data(), visibility_counts_raw.data(), count_bytes);
    const auto *classified_pixels = reinterpret_cast<const std::uint32_t *>(
        visibility_pixels_raw.data());
    const auto raster_capacity = record_bytes / kRasterRecordStride;
    const auto geometry_capacity = geometry_record_bytes /
        sizeof(renderer_detail::GpuGeometryDrawRecord);
    const auto cluster_ref_capacity = cluster_refs_raw.size() / sizeof(GpuStaticDrawRef);

    std::vector<std::byte> ids_raw(pixel_count * 8u);
    std::vector<std::byte> front_face_raw(pixel_count);
    std::vector<std::byte> metadata_raw(pixel_count * 8u);
    std::memcpy(ids_raw.data(), visibility_ids.data(), ids_raw.size());
    std::memcpy(front_face_raw.data(), front_faces.data(), front_face_raw.size());
    std::memcpy(metadata_raw.data(), metadata.data(), metadata_raw.size());
    D3D12_RANGE no_write{};
    readback.resource->Unmap(0, &no_write);

    for (const auto &cut : cuts)
    {
        const auto &constants = cut.constants;
        const auto &status = cut.status;
        const auto actual_frame_id = (static_cast<std::uint64_t>(status.frame_id_high) << 32u) |
            status.frame_id_low;
        const auto constants_frame_id = (static_cast<std::uint64_t>(constants.frame_id_high) << 32u) |
            constants.frame_id_low;
        if (constants.reserved || constants.candidate_count != cut.cut->candidate_count ||
            constants.generation != cut.cut->generation ||
            constants.generation != impl_->cluster_runtime->generation ||
            constants.output_capacity != cut.cut->output_capacity ||
            constants.geometry_draw_base != cut.cut->geometry_draw_base ||
            constants_frame_id != cut.cut->actual_frame_id ||
            constants.cluster_ref_base > cluster_ref_capacity ||
            constants.output_capacity > cluster_ref_capacity - constants.cluster_ref_base ||
            constants.geometry_draw_base == 0u ||
            constants.geometry_draw_base > 0x3fffffffu ||
            constants.output_capacity > 0x40000000u - constants.geometry_draw_base ||
            constants.geometry_draw_base > geometry_capacity ||
            constants.output_capacity > geometry_capacity - constants.geometry_draw_base)
            return invalid(std::format(
                "Cluster cut {} constants do not match its captured ABI ranges.", cut.ordinal));
        if (constants.raster_record_base != UINT32_MAX &&
            (constants.raster_record_base == 0u ||
             constants.raster_record_base > UINT32_MAX - constants.output_capacity ||
             constants.raster_record_base > raster_capacity ||
             constants.output_capacity > raster_capacity - constants.raster_record_base))
            return invalid(std::format(
                "Cluster cut {} raster interval exceeds the captured raster buffer.", cut.ordinal));
        if (status.reserved || status.candidate_count != cut.cut->candidate_count ||
            status.content_generation != cut.cut->generation ||
            status.content_generation != impl_->cluster_runtime->generation ||
            status.catalog_index != cut.ordinal || actual_frame_id != cut.cut->actual_frame_id ||
            (status.failure_flags & ~kClusterCutFailureCapacity) != 0u ||
            status.required_records_high || status.required_commands_high ||
            status.required_records_low > UINT32_MAX / 96u ||
            status.required_commands_low > UINT32_MAX / 24u ||
            status.committed_records > constants.output_capacity ||
            (status.failure_flags != 0u && status.committed_records != 0u))
            return invalid(std::format(
                "Cluster cut {} status violates its captured transaction contract.", cut.ordinal));
    }

    const auto raw_path = [&](std::string_view name) {
        return path.parent_path() / (path.stem().string() + "." + std::string(name) + ".raw");
    };
    const auto write_raw = [&](std::string_view name, const std::vector<std::byte> &bytes) {
        std::ofstream output(raw_path(name), std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(output);
    };
    if (!write_raw("visibility_ids", ids_raw) || !write_raw("visibility_front_face", front_face_raw) ||
        !write_raw("visibility_pixels", visibility_pixels_raw) ||
        !write_raw("visibility_counts", visibility_counts_raw) ||
        !write_raw("raster_records", raster_records_raw) ||
        !write_raw("gbuffer_metadata", metadata_raw) ||
        !write_raw("geometry_draw_records", geometry_records_raw) ||
        (!cluster_refs_raw.empty() && !write_raw("cluster_refs", cluster_refs_raw)) ||
        (!cluster_status_raw.empty() && !write_raw("cluster_status", cluster_status_raw)) ||
        !write_raw("cluster_command_counts", cluster_command_counts_raw) ||
        !write_raw("cluster_commands", cluster_commands_raw))
        return invalid("Could not write visibility diagnostic raw output.");

    std::vector<std::uint8_t> seen(pixel_count);
    std::vector<std::uint8_t> used_raster_records(raster_capacity);
    std::array<std::uint32_t, kVisibilityVariantCount> variant_counts{};
    std::uint64_t valid_pixel_count{};
    std::uint64_t invalid_pixel_count{};
    const auto validate_pixel = [&](std::size_t pixel, bool classified) -> std::string {
        const auto id = visibility_ids[pixel * 2];
        const auto primitive = visibility_ids[pixel * 2 + 1];
        const bool invalid_id = id == UINT32_MAX;
        const bool invalid_primitive = primitive == UINT32_MAX;
        if (invalid_id != invalid_primitive)
            return std::format("Visibility pixel {} has a partial invalid sentinel.", pixel);
        const auto metadata_id = metadata[pixel * 2];
        const auto metadata_flags = metadata[pixel * 2 + 1];
        if (invalid_id)
        {
            if (metadata_id != 0 || metadata_flags != 0)
                return std::format("Invalid visibility pixel {} has nonzero surface metadata.",
                                   pixel);
            ++invalid_pixel_count;
            return {};
        }
        if (id == 0 || id >= raster_capacity)
            return std::format("Visibility pixel {} references raster record {} outside capacity {}.",
                               pixel, id, raster_capacity);
        if (metadata_id == 0 || (metadata_flags & 1u) == 0)
            return std::format("Valid visibility pixel {} has invalid surface metadata.", pixel);
        used_raster_records[id] = 1;
        const auto *raster_bytes = raster_records_raw.data() +
            static_cast<std::size_t>(id) * kRasterRecordStride;
        GpuRasterDrawRecord record{};
        std::memcpy(&record, raster_bytes, sizeof(record));
        if (record.geometry_record_id == 0 || record.geometry_record_id >= geometry_capacity)
            return std::format("Visibility pixel {} references GPU geometry record {} outside capacity {}.",
                               pixel, record.geometry_record_id, geometry_capacity);
        if (record.ref_source < 1 || record.ref_source > 6)
            return std::format("Visibility raster record {} has ref source {} outside [1,6].", id,
                               record.ref_source);
        if (record.ref_source != 6 &&
            record.geometry_record_id >= frame.geometry_draw_records_cpu.size())
            return std::format("Visibility pixel {} references non-cluster geometry record {} outside CPU prefix.",
                               pixel, record.geometry_record_id);

        const auto *geometry_bytes = geometry_records_raw.data() +
            static_cast<std::size_t>(record.geometry_record_id) *
                sizeof(renderer_detail::GpuGeometryDrawRecord);
        renderer_detail::GpuGeometryDrawRecord geometry{};
        std::memcpy(&geometry, geometry_bytes, sizeof(geometry));
        const auto variant = record.flags & 0xffu;
        if (variant >= kVisibilityVariantCount)
            return std::format("Visibility raster record {} has variant {} outside [0,{}].", id,
                               variant, kVisibilityVariantCount - 1u);
        if (variant == kTerrainMaterialOrdinal + 1u &&
            metadata_id != 0x10000000u + kTerrainMaterialOrdinal)
            return std::format("Terrain visibility pixel {} has material identity {}.", pixel,
                               metadata_id);
        if (record.page_generation != geometry.geometry_generation)
            return std::format("Visibility raster record {} page generation {} mismatches GPU geometry {}.",
                               id, record.page_generation, geometry.geometry_generation);
        const auto first_primitive = geometry.first_index / 3u;
        const auto primitive_count = geometry.index_count / 3u;
        if (primitive < first_primitive || primitive - first_primitive >= primitive_count)
            return std::format("Visibility pixel {} canonical primitive {} is outside geometry [{},{}).",
                               pixel, primitive, first_primitive, first_primitive + primitive_count);
        if (geometry.reserved1 != record.ref_source || geometry.instance_count == 0u ||
            geometry.visible_base > UINT32_MAX - geometry.instance_count ||
            geometry.reserved2 > UINT32_MAX - geometry.instance_count ||
            id < geometry.reserved2 || id - geometry.reserved2 >= geometry.instance_count)
            return std::format("Visibility raster record {} disagrees with geometry visible/ref range.", id);
        const auto local = id - geometry.reserved2;
        if (record.visible_ref_index != geometry.visible_base + local)
            return std::format("Visibility raster record {} ref index {} disagrees with geometry base {}.",
                               id, record.visible_ref_index, geometry.visible_base + local);

        const CutCapture *owning_cut{};
        std::uint32_t selection{};
        if (record.ref_source == 6)
        {
            std::size_t matches{};
            for (const auto &cut : cuts)
            {
                if (cut.status.failure_flags != 0u || cut.status.committed_records == 0u ||
                    cut.constants.raster_record_base == UINT32_MAX)
                    continue;
                const auto base = static_cast<std::uint64_t>(cut.constants.cluster_ref_base);
                const auto end = base + cut.status.committed_records;
                if (record.visible_ref_index >= base && record.visible_ref_index < end)
                {
                    ++matches;
                    owning_cut = &cut;
                    selection = record.visible_ref_index - cut.constants.cluster_ref_base;
                }
            }
            if (matches != 1 || !owning_cut)
                return std::format("Visibility raster record {} source6 ref {} is not owned by exactly one successful cut.",
                                   id, record.visible_ref_index);
            const auto expected_raster = static_cast<std::uint64_t>(
                owning_cut->constants.raster_record_base) + selection;
            const auto expected_geometry = static_cast<std::uint64_t>(
                owning_cut->constants.geometry_draw_base) + selection;
            if (expected_raster > UINT32_MAX || expected_geometry > UINT32_MAX ||
                id != expected_raster || record.geometry_record_id != expected_geometry ||
                geometry.visible_base != record.visible_ref_index || geometry.reserved1 != 6u ||
                geometry.reserved2 != id || geometry.instance_count != 1u ||
                record.material_word != owning_cut->constants.material_word)
                return std::format("Visibility raster record {} source6 selection {} disagrees with its cut bases.",
                                   id, selection);
            if (geometry.page_slot >= kGeometryPageCapacity)
                return std::format("Visibility raster record {} source6 page slot {} is outside capacity.",
                                   id, geometry.page_slot);
            const auto &catalog = impl_->cluster_runtime->catalogs[owning_cut->ordinal];
            const auto &snapshot = cluster_frame->snapshot;
            if (catalog.page_base > snapshot.size() ||
                catalog.page_count > snapshot.size() - catalog.page_base)
                return std::format("Visibility raster record {} source6 catalog {} page range is outside the current snapshot.",
                                   id, owning_cut->ordinal);
            bool resident_page{};
            for (std::size_t logical = catalog.page_base;
                 logical < static_cast<std::size_t>(catalog.page_base) + catalog.page_count;
                 ++logical)
            {
                const auto &page = snapshot[logical];
                if (page.generation == owning_cut->cut->generation && page.resident &&
                    page.physical_page_slot == geometry.page_slot &&
                    page.page_version == geometry.geometry_generation)
                {
                    resident_page = true;
                    break;
                }
            }
            if (!resident_page)
                return std::format("Visibility raster record {} source6 page {} is not resident in catalog {}.",
                                   id, geometry.page_slot, owning_cut->ordinal);
            if ((geometry.flags & kGeometryExplicitMaterialBinding) == 0u ||
                variant != 1u + (geometry.lod & 0xffffu) ||
                variant != owning_cut->ordinal + 1u)
                return std::format("Visibility raster record {} source6 explicit material variant {} disagrees with catalog {}.",
                                   id, variant, owning_cut->ordinal);
            if (cluster_ref_buffer_index == std::numeric_limits<std::size_t>::max() ||
                record.visible_ref_index >= cluster_ref_capacity)
                return std::format("Visibility raster record {} source6 ref {} is outside the captured cluster refs.",
                                   id, record.visible_ref_index);
            GpuStaticDrawRef reference{};
            std::memcpy(&reference,
                        cluster_refs_raw.data() + static_cast<std::size_t>(
                            record.visible_ref_index) * sizeof(reference), sizeof(reference));
            if (reference.instance_index != record.instance_index ||
                reference.instance_generation != record.instance_generation)
                return std::format("Visibility raster record {} source6 ref identity disagrees with raster identity.", id);
        }
        std::uint32_t expected_variant{};
        if ((geometry.flags & kGeometryBuiltin) != 0u && geometry.mesh_id == 9u)
            expected_variant = 15u;
        else if ((geometry.flags & kGeometryExplicitMaterialBinding) != 0u)
            expected_variant = 1u + (geometry.lod & 0xffffu);
        else if (geometry.page_slot < kArcherGeometryPage &&
                 (geometry.flags & kGeometryBuiltin) == 0u)
            expected_variant = 1u + geometry.page_slot / 3u;
        if (variant != expected_variant)
            return std::format("Visibility raster record {} variant {} mismatches geometry ABI {}.",
                               id, variant, expected_variant);
        if (classified && seen[pixel] != 0)
            return std::format("Classified visibility pixel {} appears more than once.", pixel);
        ++valid_pixel_count;
        return {};
    };
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        if (front_faces[pixel] > 1)
            return invalid(std::format("Visibility pixel {} has front-face value {}.", pixel,
                                       front_faces[pixel]));
        if (auto reason = validate_pixel(pixel, false); !reason.empty())
            return invalid(std::move(reason));
    }
    for (std::size_t variant = 0; variant < variant_counts.size(); ++variant)
    {
        variant_counts[variant] = counts[variant];
        if (counts[2 * kVisibilityVariantCount + variant] != counts[variant])
            return invalid(std::format(
                "Visibility variant {} cursor {} does not equal count {}.", variant,
                counts[2 * kVisibilityVariantCount + variant], counts[variant]));
        const auto expected_offset = std::accumulate(counts.begin(), counts.begin() + variant,
                                                      std::uint64_t{});
        if (counts[kVisibilityVariantCount + variant] != expected_offset)
            return invalid(std::format(
                "Visibility variant {} offset {} does not match prefix {}.", variant,
                counts[kVisibilityVariantCount + variant], expected_offset));
    }
    const auto classified_capacity = visibility_pixels_raw.size() / sizeof(std::uint32_t);
    std::uint64_t classified_total{};
    const auto hero_asset_index = [](std::uint32_t mesh) -> std::uint32_t {
        if (mesh == static_cast<std::uint32_t>(RenderMesh::Archer))
            return 0u;
        const auto first_monster = static_cast<std::uint32_t>(RenderMesh::MonsterMelee);
        const auto last_monster = static_cast<std::uint32_t>(RenderMesh::BossFinal);
        if (mesh >= first_monster && mesh <= last_monster)
            return 1u + mesh - first_monster;
        return UINT32_MAX;
    };
    for (std::size_t variant = 0; variant < variant_counts.size(); ++variant)
    {
        const auto begin = static_cast<std::uint64_t>(counts[kVisibilityVariantCount + variant]);
        const auto end = begin + counts[variant];
        if (end > classified_capacity)
            return invalid(std::format(
                "Visibility variant {} classified range exceeds pixel buffer.", variant));
        classified_total += counts[variant];
        for (std::uint64_t index = begin; index < end; ++index)
        {
            const auto pixel = classified_pixels[index];
            if (pixel >= pixel_count)
                return invalid(std::format(
                    "Visibility variant {} contains out-of-range pixel {}.", variant, pixel));
            if (seen[pixel] != 0)
                return invalid(std::format(
                    "Classified visibility pixel {} appears more than once.", pixel));
            const auto id = visibility_ids[static_cast<std::size_t>(pixel) * 2];
            const auto primitive = visibility_ids[static_cast<std::size_t>(pixel) * 2 + 1];
            if (id == UINT32_MAX)
                return invalid(std::format(
                    "Visibility variant {} contains an invalid visibility pixel {}.", variant,
                    pixel));
            const auto *record_bytes = raster_records_raw.data() +
                static_cast<std::size_t>(id) * kRasterRecordStride;
            GpuRasterDrawRecord record{};
            std::memcpy(&record, record_bytes, sizeof(record));
            const auto *geometry_bytes = geometry_records_raw.data() +
                static_cast<std::size_t>(record.geometry_record_id) *
                    sizeof(renderer_detail::GpuGeometryDrawRecord);
            renderer_detail::GpuGeometryDrawRecord geometry{};
            std::memcpy(&geometry, geometry_bytes, sizeof(geometry));
            const auto hero_asset = hero_asset_index(geometry.mesh_id);
            if ((geometry.flags & kGeometryBuiltin) != 0u || hero_asset == UINT32_MAX)
            {
                if ((record.flags & 0xffu) != variant)
                    return invalid(std::format(
                        "Classified pixel {} variant {} mismatches raster record variant {}.", pixel,
                        variant, record.flags & 0xffu));
            }
            else
            {
                const GeometryPageGpu *page{};
                std::span<const render_data::CharacterMaterialBinding> bindings;
                const auto logical_page = hero_asset == 0u
                    ? kArcherGeometryPage : kMonsterGeometryPage + hero_asset - 1u;
                if (hero_asset == 0u)
                    bindings = impl_->archer_material_bindings;
                else
                    bindings = impl_->monster_assets[hero_asset - 1u].material_bindings;
                page = impl_->FindGeometryPage(logical_page);
                if (!page)
                    return invalid(std::format(
                        "Classified hero pixel {} mesh {} has no resident geometry page.",
                        pixel, geometry.mesh_id));
                if (page->page_slot != geometry.page_slot)
                    return invalid(std::format(
                        "Classified hero pixel {} page slot {} mismatches mesh {} resident page {}.",
                        pixel, geometry.page_slot, geometry.mesh_id, page->page_slot));
                if (page->generation != geometry.geometry_generation)
                    return invalid(std::format(
                        "Classified hero pixel {} generation {} mismatches mesh {} resident page {}.",
                        pixel, geometry.geometry_generation, geometry.mesh_id, page->generation));
                if (primitive > UINT32_MAX / 3u)
                    return invalid(std::format(
                        "Classified hero pixel {} primitive {} overflows its canonical index offset.",
                        pixel, primitive));
                const auto primitive_index = primitive * 3u;
                const render_data::GeometrySubmesh *submesh{};
                std::size_t submesh_matches{};
                for (const auto &candidate : page->submeshes)
                {
                    if (candidate.first_index > UINT32_MAX - candidate.index_count)
                        return invalid(std::format(
                            "Classified hero pixel {} has an overflowing resident submesh range.",
                            pixel));
                    if (primitive_index >= candidate.first_index &&
                        primitive_index < candidate.first_index + candidate.index_count)
                    {
                        submesh = &candidate;
                        ++submesh_matches;
                    }
                }
                if (submesh_matches != 1u || !submesh)
                    return invalid(std::format(
                        "Classified hero pixel {} primitive {} does not resolve to exactly one resident submesh.",
                        pixel, primitive));
                const render_data::CharacterMaterialBinding *binding{};
                std::size_t binding_matches{};
                for (const auto &candidate : bindings)
                    if (candidate.slot == submesh->material_index &&
                        candidate.domain == render_data::HeroMaterialDomain::Opaque)
                    {
                        binding = &candidate;
                        ++binding_matches;
                    }
                if (binding_matches != 1u || !binding)
                    return invalid(std::format(
                        "Classified hero pixel {} material slot {} does not resolve to exactly one opaque binding.",
                        pixel, submesh->material_index));
                if (binding->hero_ordinal >= kHeroMaterialNames.size() ||
                    binding->hero_ordinal == 5u)
                    return invalid(std::format(
                        "Classified hero pixel {} has invalid opaque hero ordinal {}.",
                        pixel, binding->hero_ordinal));
                if (variant != 16u + binding->hero_ordinal)
                    return invalid(std::format(
                        "Classified hero pixel {} variant {} mismatches opaque ordinal {}.",
                        pixel, variant, binding->hero_ordinal));
                if (metadata[pixel * 2] != 0x20000000u + binding->hero_ordinal)
                    return invalid(std::format(
                        "Classified hero pixel {} metadata {} mismatches opaque ordinal {}.",
                        pixel, metadata[pixel * 2], binding->hero_ordinal));
            }
            seen[pixel] = 1;
        }
    }
    if (classified_total != valid_pixel_count)
        return invalid(std::format(
            "Visibility count sum {} does not equal valid pixel count {}.", classified_total,
            valid_pixel_count));
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
    {
        const bool valid = visibility_ids[pixel * 2] != UINT32_MAX;
        if (valid != (seen[pixel] != 0))
            return invalid(std::format(
                "Visibility classified-pixel coverage disagrees at pixel {}.", pixel));
    }
    const auto used_record_count = static_cast<std::uint64_t>(std::count(
        used_raster_records.begin(), used_raster_records.end(), std::uint8_t{1}));

    std::ofstream output(path, std::ios::trunc);
    if (!output)
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               "Could not open visibility diagnostic metadata output.");
    output << "{\n  \"schema_version\": 1,\n  \"execution_valid\": true,\n"
              "  \"width\": "
           << impl_->render_width << ",\n  \"height\": " << impl_->render_height
            << ",\n  \"frame_slot\": " << impl_->last_presented_index
           << ",\n  \"valid_pixel_count\": " << valid_pixel_count
           << ",\n  \"invalid_pixel_count\": " << invalid_pixel_count
           << ",\n  \"variant_counts\": [";
    for (std::size_t index = 0; index < variant_counts.size(); ++index)
        output << (index == 0 ? "" : ", ") << variant_counts[index];
    output << "],\n  \"cluster_command_readback_bytes\": " << command_readback_bytes
           << ",\n  \"cluster_command_layout\": {\"indexed\": {\"file\": \""
           << json_escape(raw_path("cluster_commands").filename().string())
           << "\", \"stride\": " << kGeometryArgumentStride << ", \"size\": " << cluster_commands_raw.size()
           << "}, \"counts\": {\"file\": \""
           << json_escape(raw_path("cluster_command_counts").filename().string())
           << "\", \"stride\": 20, \"size\": " << cluster_command_counts_raw.size()
           << "}, \"mesh_arguments_captured\": false}"
           << ",\n  \"cluster_commands\": [";
    for (std::size_t index = 0; index < cuts.size(); ++index)
    {
        const auto &cut = cuts[index];
        const auto used_cluster_records = cut.constants.raster_record_base == UINT32_MAX ? 0 :
            std::count(used_raster_records.begin() + cut.constants.raster_record_base,
                       used_raster_records.begin() + cut.constants.raster_record_base +
                           cut.status.committed_records, std::uint8_t{1});
        output << (index ? "," : "") << "\n    {\"catalog\": " << cut.ordinal
               << ", \"frame\": " << cut.cut->actual_frame_id
               << ", \"candidate_count\": " << cut.cut->candidate_count
               << ", \"output_capacity\": " << cut.cut->output_capacity
               << ", \"committed_records\": " << cut.status.committed_records
               << ", \"failure_flags\": " << cut.status.failure_flags
               << ", \"pixel_contributing_raster_records\": " << used_cluster_records
               << ", \"geometry_frontend_flags\": " << cut.cut->geometry_frontend_flags
               << ", \"command_counts_offset\": " << index * 5 * sizeof(std::uint32_t)
               << ", \"views\": [";
        for (std::size_t view = 0; view < cut.commands.size(); ++view)
        {
            const auto &command = cut.commands[view];
            output << (view ? ", " : "") << "{\"view\": " << view
                   << ", \"argument_offset\": " << command.argument_offset
                   << ", \"argument_size\": " << static_cast<std::uint64_t>(cut.status.committed_records) * kGeometryArgumentStride
                   << ", \"published_count\": " << command.published
                   << ", \"nonzero_count\": " << command.nonzero
                   << ", \"zero_count\": " << command.published - command.nonzero
                   << ", \"last_nonzero_exclusive\": " << command.last_nonzero_exclusive
                   << ", \"trailing_zero_count\": " << command.published - command.last_nonzero_exclusive
                   << ", \"committed_prefix_not_submitted\": " << cut.status.committed_records - command.published
                   << ", \"nonzero_outside_published_prefix\": " << command.outside_prefix_nonzero
                   << ", \"unused_capacity_records_not_inspected\": " << cut.cut->output_capacity - cut.status.committed_records
                   << ", \"draw_vertex_count_sum\": " << command.draw_vertices << "}";
        }
        output << "]}";
    }
    output << "\n  ],\n  \"cluster_command_notes\": \"Views 0=main after previous-HZB cull, 1..3=CSM, 4=current-depth retest. "
              "24-byte vertex-pull arguments are written for both geometry frontends; draw_vertex_count_sum is the vertex-pull argument workload, "
              "not measured VS/MS invocations. Uncommitted allocation tails are neither copied nor inspected.\",\n  \"num_records\": " << used_record_count
           << ",\n  \"record_capacity\": " << raster_capacity
           << ",\n  \"declared_formats\": {\"visibility_ids\": \"DXGI_FORMAT_R32G32_UINT\", "
              "\"visibility_front_face\": \"DXGI_FORMAT_R8_UINT\", "
              "\"gbuffer_metadata\": \"DXGI_FORMAT_R32G32_UINT\"},\n"
              "  \"sentinel\": {\"visibility_ids\": [4294967295, 4294967295], "
              "\"front_face_clear\": 0}\n}\n";
    if (!output)
        return Result::Failure(ErrorCode::TaskFailed, "hs_renderer_d3d12",
                               "Could not write visibility diagnostic metadata output.");
    return Result::Success();
}

Result D3D12Renderer::Capture(const CaptureRequest &request)
{
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }
    if (request.path.empty())
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Capture output path is empty.");
    }
    switch (request.kind)
    {
    case CaptureKind::Color:
    case CaptureKind::SurfaceDiagnostics:
    case CaptureKind::GeometryDiagnostics:
    case CaptureKind::VisibilityDiagnostics:
    case CaptureKind::HdrColor:
        break;
    default:
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Capture kind is invalid.");
    }
    if (request.view_id != kMainRenderViewId &&
        !impl_->owned_views.contains(request.view_id.value))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Capture view ID is not keyed.");
    }

    const auto original_view = impl_->active_view_id;
    if (auto result = impl_->ActivateView(request.view_id); !result)
    {
        return result;
    }
    const auto restore = [&](Result result) {
        if (auto restored = impl_->ActivateView(original_view); !restored)
        {
            return restored;
        }
        return result;
    };
    if (impl_->frame_number == 0)
    {
        return restore(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "Capture view has not produced a successful rendered output."));
    }
    if ((request.kind == CaptureKind::GeometryDiagnostics || request.kind == CaptureKind::VisibilityDiagnostics) &&
        impl_->frames[impl_->last_presented_index].submitted_view_id != request.view_id.value)
    {
        return restore(Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "This view has no completed submitted frame available for capture."));
    }

    Result result = Result::Success();
    switch (request.kind)
    {
    case CaptureKind::Color:
        result = CapturePng(request.path);
        break;
    case CaptureKind::HdrColor:
        result = CaptureHdrColor(*impl_, request.path);
        break;
    case CaptureKind::SurfaceDiagnostics:
        result = CaptureSurfaceDiagnostics(request.path);
        break;
    case CaptureKind::GeometryDiagnostics:
        result = CaptureGeometryDiagnostics(request.path);
        break;
    case CaptureKind::VisibilityDiagnostics:
        result = CaptureVisibilityDiagnostics(request.path);
        break;
    default:
        // The request kind was checked above; keep the switch exhaustive if the
        // enum gains another value without dispatching an invalid readback.
        result = Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                 "Capture kind is invalid.");
        break;
    }
    return restore(std::move(result));
}


} // namespace hs
