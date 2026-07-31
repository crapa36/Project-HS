#include <hs/renderer/renderer.hpp>

#include <hs/renderer/render_graph.hpp>

#include <D3D12MemAlloc.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace hs
{
namespace
{

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kFrameCount = 3;
constexpr std::uint32_t kParticleCount = 10'000;
constexpr std::uint32_t kMaxParticleSpawnCommands = 256;
constexpr std::uint32_t kInstanceDataOffset = 512;
constexpr std::uint32_t kParticleSpawnDataOffset = 64 * 1024;
constexpr std::uint32_t kTimestampCountPerFrame =
    static_cast<std::uint32_t>(kStage1RenderPassCount * 2);
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

struct Vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
};

struct GpuInstance
{
    DirectX::XMFLOAT4 position_scale;
    std::uint32_t color{};
    std::uint32_t mesh{};
    float yaw{};
    float padding{};
};

struct FrameConstants
{
    DirectX::XMFLOAT4X4 view_projection;
    DirectX::XMFLOAT4 camera_time;
    DirectX::XMFLOAT4 light_direction_intensity;
    DirectX::XMFLOAT4 light_color;
    DirectX::XMFLOAT4 screen_size;
    DirectX::XMFLOAT4X4 shadow_view_projection[3];
    DirectX::XMFLOAT4X4 archer_bones[2];
    DirectX::XMFLOAT4 render_options;
    DirectX::XMUINT4 particle_options;
};

static_assert(sizeof(FrameConstants) <= kInstanceDataOffset);

struct GpuParticle
{
    DirectX::XMFLOAT4 position_life;
    DirectX::XMFLOAT4 initial_position_spawn_time;
    DirectX::XMFLOAT4 initial_velocity_max_life;
    DirectX::XMFLOAT4 start_color_size;
    DirectX::XMFLOAT4 end_color_size;
    DirectX::XMFLOAT4 physics_sprite;
};

struct GpuParticleSpawnCommand
{
    DirectX::XMFLOAT4 position_lifetime;
    DirectX::XMFLOAT4 velocity_spread;
    DirectX::XMFLOAT4 start_color_size;
    DirectX::XMFLOAT4 end_color_size;
    DirectX::XMFLOAT4 physics;
    DirectX::XMUINT4 metadata;
};

static_assert(kParticleSpawnDataOffset +
                  sizeof(GpuParticleSpawnCommand) * kMaxParticleSpawnCommands <=
              128 * 1024);

struct AllocationResource
{
    ComPtr<ID3D12Resource> resource;
    D3D12MA::Allocation *allocation{};

    AllocationResource() = default;
    ~AllocationResource()
    {
        Reset();
    }
    AllocationResource(const AllocationResource &) = delete;
    AllocationResource &operator=(const AllocationResource &) = delete;
    AllocationResource(AllocationResource &&other) noexcept
        : resource(std::move(other.resource)), allocation(std::exchange(other.allocation, nullptr))
    {
    }
    AllocationResource &operator=(AllocationResource &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            resource = std::move(other.resource);
            allocation = std::exchange(other.allocation, nullptr);
        }
        return *this;
    }

    void Reset() noexcept
    {
        resource.Reset();
        if (allocation)
        {
            allocation->Release();
            allocation = nullptr;
        }
    }
};

struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> allocator;
    AllocationResource upload;
    std::byte *mapped{};
    std::uint64_t fence_value{};
    bool timestamps_recorded{};
};

[[nodiscard]] Result HResultFailure(std::string_view operation, HRESULT result)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                           std::format("{} failed: 0x{:08X}", operation,
                                       static_cast<std::uint32_t>(result)));
}

[[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(std::uint64_t size,
                                                    D3D12_RESOURCE_FLAGS flags =
                                                        D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc = {1, 0};
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

[[nodiscard]] std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, 32'768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}

[[nodiscard]] Result ReadBinary(const std::filesystem::path &path,
                                std::vector<std::byte> &bytes)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::format("Missing shader: {}", path.string()));
    }
    const auto size = stream.tellg();
    bytes.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), size);
    return Result::Success();
}

constexpr std::array<Vertex, 36> kCubeVertices = {
    Vertex{{-0.5f, -0.5f, -0.5f}, {0, 0, -1}}, Vertex{{0.5f, 0.5f, -0.5f}, {0, 0, -1}},
    Vertex{{0.5f, -0.5f, -0.5f}, {0, 0, -1}},  Vertex{{-0.5f, -0.5f, -0.5f}, {0, 0, -1}},
    Vertex{{-0.5f, 0.5f, -0.5f}, {0, 0, -1}},  Vertex{{0.5f, 0.5f, -0.5f}, {0, 0, -1}},
    Vertex{{-0.5f, -0.5f, 0.5f}, {0, 0, 1}},   Vertex{{0.5f, -0.5f, 0.5f}, {0, 0, 1}},
    Vertex{{0.5f, 0.5f, 0.5f}, {0, 0, 1}},     Vertex{{-0.5f, -0.5f, 0.5f}, {0, 0, 1}},
    Vertex{{0.5f, 0.5f, 0.5f}, {0, 0, 1}},     Vertex{{-0.5f, 0.5f, 0.5f}, {0, 0, 1}},
    Vertex{{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}}, Vertex{{-0.5f, -0.5f, 0.5f}, {-1, 0, 0}},
    Vertex{{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}},   Vertex{{-0.5f, -0.5f, -0.5f}, {-1, 0, 0}},
    Vertex{{-0.5f, 0.5f, 0.5f}, {-1, 0, 0}},   Vertex{{-0.5f, 0.5f, -0.5f}, {-1, 0, 0}},
    Vertex{{0.5f, -0.5f, -0.5f}, {1, 0, 0}},   Vertex{{0.5f, 0.5f, 0.5f}, {1, 0, 0}},
    Vertex{{0.5f, -0.5f, 0.5f}, {1, 0, 0}},    Vertex{{0.5f, -0.5f, -0.5f}, {1, 0, 0}},
    Vertex{{0.5f, 0.5f, -0.5f}, {1, 0, 0}},    Vertex{{0.5f, 0.5f, 0.5f}, {1, 0, 0}},
    Vertex{{-0.5f, 0.5f, -0.5f}, {0, 1, 0}},   Vertex{{-0.5f, 0.5f, 0.5f}, {0, 1, 0}},
    Vertex{{0.5f, 0.5f, 0.5f}, {0, 1, 0}},     Vertex{{-0.5f, 0.5f, -0.5f}, {0, 1, 0}},
    Vertex{{0.5f, 0.5f, 0.5f}, {0, 1, 0}},     Vertex{{0.5f, 0.5f, -0.5f}, {0, 1, 0}},
    Vertex{{-0.5f, -0.5f, -0.5f}, {0, -1, 0}}, Vertex{{0.5f, -0.5f, 0.5f}, {0, -1, 0}},
    Vertex{{-0.5f, -0.5f, 0.5f}, {0, -1, 0}},  Vertex{{-0.5f, -0.5f, -0.5f}, {0, -1, 0}},
    Vertex{{0.5f, -0.5f, -0.5f}, {0, -1, 0}},  Vertex{{0.5f, -0.5f, 0.5f}, {0, -1, 0}},
};

} // namespace

struct D3D12Renderer::Impl
{
    [[nodiscard]] Result CreateDevice(const RendererConfig &configuration);
    [[nodiscard]] Result CreateSwapChainAndTargets();
    [[nodiscard]] Result CreateDepthAndShadow();
    [[nodiscard]] Result CreatePostProcessTargets();
    [[nodiscard]] Result CreatePipeline();
    [[nodiscard]] Result CreateGpuData();
    [[nodiscard]] Result CreateUiTexture();
    [[nodiscard]] Result CreateAllocation(AllocationResource &output,
                                          const D3D12MA::ALLOCATION_DESC &allocation,
                                          const D3D12_RESOURCE_DESC &resource,
                                          D3D12_RESOURCE_STATES initial_state,
                                          const D3D12_CLEAR_VALUE *clear = nullptr);
    [[nodiscard]] Result WaitForFrame(FrameContext &frame);
    [[nodiscard]] Result WaitForGpu();
    [[nodiscard]] Result CheckDevice(HRESULT result, std::string_view operation);
    void TransitionTexture(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after, D3D12_BARRIER_LAYOUT before_layout,
                           D3D12_BARRIER_LAYOUT after_layout);
    void TransitionBuffer(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after, D3D12_BARRIER_SYNC before_sync,
                          D3D12_BARRIER_SYNC after_sync, D3D12_BARRIER_ACCESS before_access,
                          D3D12_BARRIER_ACCESS after_access);
    void CountValidationErrors();
    void WriteDred(HRESULT reason) const;
    [[nodiscard]] Result WritePng(const std::filesystem::path &path, const std::byte *pixels,
                                  std::uint32_t row_pitch) const;

    RendererConfig config;
    ComPtr<IDXGIFactory7> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain4> swap_chain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12GraphicsCommandList7> enhanced_command_list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};
    std::uint64_t next_fence{1};
    D3D12MA::Allocator *allocator{};
    std::array<FrameContext, kFrameCount> frames;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> back_buffers;
    AllocationResource depth;
    AllocationResource shadow;
    AllocationResource vertices;
    AllocationResource particles;
    std::array<AllocationResource, 2> particle_alive;
    AllocationResource particle_dead;
    AllocationResource particle_counters;
    AllocationResource indirect_arguments;
    AllocationResource gbuffer_base;
    AllocationResource gbuffer_normal;
    AllocationResource gbuffer_position;
    AllocationResource hdr_color;
    AllocationResource oit_accumulation;
    AllocationResource oit_revealage;
    AllocationResource post_a;
    AllocationResource post_b;
    AllocationResource ui_texture;
    AllocationResource timestamp_readback;
    std::uint64_t *mapped_timestamps{};
    ComPtr<ID3D12QueryHeap> timestamp_heap;
    std::uint64_t timestamp_frequency{};
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> scene_pipeline;
    ComPtr<ID3D12PipelineState> shadow_pipeline;
    ComPtr<ID3D12PipelineState> particle_pipeline;
    ComPtr<ID3D12PipelineState> particle_compute_pipeline;
    ComPtr<ID3D12PipelineState> deferred_pipeline;
    ComPtr<ID3D12PipelineState> composite_pipeline;
    ComPtr<ID3D12PipelineState> bloom_pipeline;
    ComPtr<ID3D12PipelineState> tone_map_pipeline;
    ComPtr<ID3D12PipelineState> outline_pipeline;
    ComPtr<ID3D12PipelineState> fxaa_pipeline;
    ComPtr<ID3D12PipelineState> ui_pipeline;
    ComPtr<ID3D12CommandSignature> draw_signature;
    D3D12_VERTEX_BUFFER_VIEW vertex_view{};
    RenderGraphBuilder graph;
    UINT rtv_stride{};
    UINT dsv_stride{};
    UINT srv_stride{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t render_width{};
    std::uint32_t render_height{};
    std::uint32_t last_presented_index{};
    std::uint64_t frame_number{};
    std::uint64_t validation_errors{};
    std::uint32_t last_particle_count{};
    Tick observed_snapshot_tick{};
    std::chrono::steady_clock::time_point snapshot_arrival{};
    bool enhanced{};
    bool dred_enabled{};
    bool transient_textures_common{};
    bool particles_initialized{};
    bool particle_input_is_a{true};
    bool initialized{};
    bool com_initialized{};
    Tick last_particle_tick{};
};

Result D3D12Renderer::Impl::CreateAllocation(AllocationResource &output,
                                            const D3D12MA::ALLOCATION_DESC &allocation,
                                            const D3D12_RESOURCE_DESC &resource,
                                            D3D12_RESOURCE_STATES initial_state,
                                            const D3D12_CLEAR_VALUE *clear)
{
    const auto effective_state =
        enhanced && resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
            ? D3D12_RESOURCE_STATE_COMMON
            : initial_state;
    const auto result = allocator->CreateResource(&allocation, &resource, effective_state, clear,
                                                  &output.allocation,
                                                  IID_PPV_ARGS(output.resource.ReleaseAndGetAddressOf()));
    return SUCCEEDED(result) ? Result::Success() : HResultFailure("D3D12MA::CreateResource", result);
}

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
        const auto upload_description = BufferDescription(128 * 1024);
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

Result D3D12Renderer::Impl::CreateSwapChainAndTargets()
{
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = kBackBufferFormat;
    description.SampleDesc = {1, 0};
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = kFrameCount;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> swap_chain1;
    auto result = factory->CreateSwapChainForHwnd(
        queue.Get(), static_cast<HWND>(config.window), &description, nullptr, nullptr, &swap_chain1);
    if (FAILED(result))
    {
        return HResultFailure("CreateSwapChainForHwnd", result);
    }
    result = swap_chain1.As(&swap_chain);
    if (FAILED(result))
    {
        return HResultFailure("Query IDXGISwapChain4", result);
    }
    factory->MakeWindowAssociation(static_cast<HWND>(config.window), DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_description{};
    rtv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_description.NumDescriptors = 11;
    result = device->CreateDescriptorHeap(&rtv_description, IID_PPV_ARGS(&rtv_heap));
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

Result D3D12Renderer::Impl::CreatePostProcessTargets()
{
    transient_textures_common = enhanced;
    gbuffer_base.Reset();
    gbuffer_normal.Reset();
    gbuffer_position.Reset();
    hdr_color.Reset();
    oit_accumulation.Reset();
    oit_revealage.Reset();
    post_a.Reset();
    post_b.Reset();

    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    auto create_target = [&](AllocationResource &output, DXGI_FORMAT format,
                             const float *clear_color) -> Result {
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = render_width;
        description.Height = render_height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc = {1, 0};
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = format;
        std::copy_n(clear_color, 4, clear.Color);
        return CreateAllocation(output, allocation, description,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear);
    };

    constexpr float black[4] = {0, 0, 0, 0};
    constexpr float normal_clear[4] = {0.5f, 1.0f, 0.5f, 0};
    constexpr float reveal_clear[4] = {1, 1, 1, 1};
    for (auto [resource, format, clear] : {
             std::tuple{&gbuffer_base, DXGI_FORMAT_R8G8B8A8_UNORM, black},
             std::tuple{&gbuffer_normal, DXGI_FORMAT_R16G16B16A16_FLOAT, normal_clear},
             std::tuple{&gbuffer_position, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&hdr_color, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&oit_accumulation, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&oit_revealage, DXGI_FORMAT_R16_FLOAT, reveal_clear},
             std::tuple{&post_a, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
             std::tuple{&post_b, DXGI_FORMAT_R16G16B16A16_FLOAT, black},
         })
    {
        if (auto created = create_target(*resource, format, clear); !created)
        {
            return created;
        }
    }

    auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(kFrameCount) * rtv_stride;
    for (auto [resource, format] : {
             std::pair{gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM},
             std::pair{gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{gbuffer_position.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{oit_accumulation.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT},
             std::pair{post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
             std::pair{post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT},
         })
    {
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(resource, &view, rtv);
        rtv.ptr += rtv_stride;
    }

    if (!srv_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        description.NumDescriptors = 10;
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        const auto result = device->CreateDescriptorHeap(&description, IID_PPV_ARGS(&srv_heap));
        if (FAILED(result))
        {
            return HResultFailure("Create SRV heap", result);
        }
        srv_stride =
            device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    auto srv = srv_heap->GetCPUDescriptorHandleForHeapStart();
    auto create_srv = [&](ID3D12Resource *resource, DXGI_FORMAT format) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &view, srv);
        srv.ptr += srv_stride;
    };
    create_srv(gbuffer_base.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
    create_srv(gbuffer_normal.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_srv(gbuffer_position.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_SHADER_RESOURCE_VIEW_DESC shadow_view{};
    shadow_view.Format = DXGI_FORMAT_R32_FLOAT;
    shadow_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    shadow_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadow_view.Texture2DArray.MipLevels = 1;
    shadow_view.Texture2DArray.ArraySize = 3;
    device->CreateShaderResourceView(shadow.resource.Get(), &shadow_view, srv);
    srv.ptr += srv_stride;
    create_srv(hdr_color.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_srv(oit_accumulation.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_srv(oit_revealage.resource.Get(), DXGI_FORMAT_R16_FLOAT);
    create_srv(post_a.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    create_srv(post_b.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (ui_texture.resource)
    {
        create_srv(ui_texture.resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreateDepthAndShadow()
{
    transient_textures_common = enhanced;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_description{};
    dsv_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_description.NumDescriptors = 4;
    auto result = device->CreateDescriptorHeap(&dsv_description, IID_PPV_ARGS(&dsv_heap));
    if (FAILED(result))
    {
        return HResultFailure("Create DSV heap", result);
    }
    dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_CLEAR_VALUE clear{};
    clear.Format = kDepthFormat;
    clear.DepthStencil = {0.0f, 0};
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depth_description{};
    depth_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_description.Width = render_width;
    depth_description.Height = render_height;
    depth_description.DepthOrArraySize = 1;
    depth_description.MipLevels = 1;
    depth_description.Format = kDepthFormat;
    depth_description.SampleDesc = {1, 0};
    depth_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (auto created = CreateAllocation(depth, allocation, depth_description,
                                        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        !created)
    {
        return created;
    }
    device->CreateDepthStencilView(depth.resource.Get(), nullptr,
                                   dsv_heap->GetCPUDescriptorHandleForHeapStart());

    auto shadow_description = depth_description;
    shadow_description.Format = DXGI_FORMAT_R32_TYPELESS;
    shadow_description.Width = std::clamp(config.shadow_resolution, 1024u, 2048u);
    shadow_description.Height = static_cast<UINT>(shadow_description.Width);
    shadow_description.DepthOrArraySize = 3;
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
        view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.ArraySize = 1;
        view.Texture2DArray.FirstArraySlice = cascade;
        device->CreateDepthStencilView(shadow.resource.Get(), &view, shadow_handle);
        shadow_handle.ptr += dsv_stride;
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::CreatePipeline()
{
    D3D12_DESCRIPTOR_RANGE1 texture_range{};
    texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    texture_range.NumDescriptors = 10;
    texture_range.BaseShaderRegister = 2;
    texture_range.RegisterSpace = 0;
    texture_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
                          D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    std::array<D3D12_ROOT_PARAMETER1, 13> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[3].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[4].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[5].DescriptorTable = {1, &texture_range};
    parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[6].Constants = {1, 0, 1};
    parameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    for (std::uint32_t index = 0; index < 4; ++index)
    {
        auto &parameter = parameters[7 + index];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor = {2 + index, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[11].Descriptor = {12, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[12].Descriptor = {13, 0,
                                 D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    std::array<D3D12_STATIC_SAMPLER_DESC, 2> samplers{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_description{};
    root_description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_description.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    root_description.Desc_1_1.pParameters = parameters.data();
    root_description.Desc_1_1.NumStaticSamplers = static_cast<UINT>(samplers.size());
    root_description.Desc_1_1.pStaticSamplers = samplers.data();
    root_description.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> error_blob;
    auto result = D3D12SerializeVersionedRootSignature(&root_description, &root_blob, &error_blob);
    if (FAILED(result))
    {
        const auto message = error_blob
                                 ? std::string(static_cast<const char *>(error_blob->GetBufferPointer()),
                                               error_blob->GetBufferSize())
                                 : "unknown root signature error";
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", message);
    }
    result = device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature));
    if (FAILED(result))
    {
        return HResultFailure("CreateRootSignature", result);
    }

    const auto shader_directory = ExecutableDirectory() / "Shaders";
    std::vector<std::byte> scene_vertex;
    std::vector<std::byte> scene_pixel;
    std::vector<std::byte> shadow_vertex;
    std::vector<std::byte> particle_vertex;
    std::vector<std::byte> particle_pixel;
    std::vector<std::byte> particle_compute;
    std::vector<std::byte> full_screen_vertex;
    std::vector<std::byte> deferred_pixel;
    std::vector<std::byte> composite_pixel;
    std::vector<std::byte> bloom_pixel;
    std::vector<std::byte> tone_map_pixel;
    std::vector<std::byte> outline_pixel;
    std::vector<std::byte> fxaa_pixel;
    std::vector<std::byte> ui_pixel;
    for (auto [name, bytes] : {
             std::pair{"scene_vs.dxil", &scene_vertex},
             std::pair{"scene_ps.dxil", &scene_pixel},
             std::pair{"shadow_vs.dxil", &shadow_vertex},
             std::pair{"particle_vs.dxil", &particle_vertex},
             std::pair{"particle_ps.dxil", &particle_pixel},
             std::pair{"particle_cs.dxil", &particle_compute},
             std::pair{"fullscreen_vs.dxil", &full_screen_vertex},
             std::pair{"deferred_ps.dxil", &deferred_pixel},
             std::pair{"composite_ps.dxil", &composite_pixel},
             std::pair{"bloom_ps.dxil", &bloom_pixel},
             std::pair{"tonemap_ps.dxil", &tone_map_pixel},
             std::pair{"outline_ps.dxil", &outline_pixel},
             std::pair{"fxaa_ps.dxil", &fxaa_pixel},
             std::pair{"ui_ps.dxil", &ui_pixel},
         })
    {
        if (auto read = ReadBinary(shader_directory / name, *bytes); !read)
        {
            return read;
        }
    }

    constexpr D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC scene{};
    scene.pRootSignature = root_signature.Get();
    scene.VS = {scene_vertex.data(), scene_vertex.size()};
    scene.PS = {scene_pixel.data(), scene_pixel.size()};
    scene.BlendState.AlphaToCoverageEnable = FALSE;
    scene.BlendState.IndependentBlendEnable = FALSE;
    scene.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    scene.SampleMask = UINT_MAX;
    scene.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    scene.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    scene.RasterizerState.FrontCounterClockwise = FALSE;
    scene.RasterizerState.DepthClipEnable = TRUE;
    scene.DepthStencilState.DepthEnable = TRUE;
    scene.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    scene.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    scene.InputLayout = {input_layout, static_cast<UINT>(std::size(input_layout))};
    scene.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    scene.NumRenderTargets = 3;
    scene.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    scene.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    scene.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    scene.DSVFormat = kDepthFormat;
    scene.SampleDesc = {1, 0};
    result = device->CreateGraphicsPipelineState(&scene, IID_PPV_ARGS(&scene_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create scene PSO", result);
    }

    auto shadow_state = scene;
    shadow_state.VS = {shadow_vertex.data(), shadow_vertex.size()};
    shadow_state.PS = {};
    shadow_state.NumRenderTargets = 0;
    std::fill(std::begin(shadow_state.RTVFormats), std::end(shadow_state.RTVFormats),
              DXGI_FORMAT_UNKNOWN);
    shadow_state.RasterizerState.DepthBias = 800;
    shadow_state.RasterizerState.SlopeScaledDepthBias = 1.5f;
    result = device->CreateGraphicsPipelineState(&shadow_state, IID_PPV_ARGS(&shadow_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create shadow PSO", result);
    }

    auto particle = scene;
    particle.VS = {particle_vertex.data(), particle_vertex.size()};
    particle.PS = {particle_pixel.data(), particle_pixel.size()};
    particle.InputLayout = {};
    particle.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    particle.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    particle.BlendState.RenderTarget[0].BlendEnable = TRUE;
    particle.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    particle.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    particle.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    particle.BlendState.IndependentBlendEnable = TRUE;
    particle.BlendState.RenderTarget[1].BlendEnable = TRUE;
    particle.BlendState.RenderTarget[1].SrcBlend = D3D12_BLEND_ZERO;
    particle.BlendState.RenderTarget[1].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    particle.BlendState.RenderTarget[1].BlendOp = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[1].SrcBlendAlpha = D3D12_BLEND_ZERO;
    particle.BlendState.RenderTarget[1].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    particle.BlendState.RenderTarget[1].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    particle.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    particle.NumRenderTargets = 2;
    particle.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    particle.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
    particle.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    result = device->CreateGraphicsPipelineState(&particle, IID_PPV_ARGS(&particle_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle PSO", result);
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = root_signature.Get();
    compute.CS = {particle_compute.data(), particle_compute.size()};
    result =
        device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&particle_compute_pipeline));
    if (FAILED(result))
    {
        return HResultFailure("Create particle compute PSO", result);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC post{};
    post.pRootSignature = root_signature.Get();
    post.VS = {full_screen_vertex.data(), full_screen_vertex.size()};
    post.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    post.SampleMask = UINT_MAX;
    post.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    post.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    post.RasterizerState.DepthClipEnable = TRUE;
    post.DepthStencilState.DepthEnable = FALSE;
    post.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    post.NumRenderTargets = 1;
    post.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    post.SampleDesc = {1, 0};
    auto create_post_pipeline = [&](std::span<const std::byte> pixel_shader,
                                    ID3D12PipelineState **pipeline) -> Result {
        post.PS = {pixel_shader.data(), pixel_shader.size()};
        const auto create_result = device->CreateGraphicsPipelineState(
            &post, IID_PPV_ARGS(pipeline));
        return SUCCEEDED(create_result) ? Result::Success()
                                        : HResultFailure("Create post-process PSO", create_result);
    };
    for (auto [shader, pipeline] :
         std::array{
             std::pair{std::span<const std::byte>(deferred_pixel),
                       deferred_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(composite_pixel),
                       composite_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(bloom_pixel),
                       bloom_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(tone_map_pixel),
                       tone_map_pipeline.ReleaseAndGetAddressOf()},
             std::pair{std::span<const std::byte>(outline_pixel),
                       outline_pipeline.ReleaseAndGetAddressOf()},
         })
    {
        if (auto pipeline_result = create_post_pipeline(shader, pipeline); !pipeline_result)
        {
            return pipeline_result;
        }
    }

    post.RTVFormats[0] = kBackBufferFormat;
    if (auto pipeline_result =
            create_post_pipeline(fxaa_pixel, fxaa_pipeline.ReleaseAndGetAddressOf());
        !pipeline_result)
    {
        return pipeline_result;
    }
    post.BlendState.RenderTarget[0].BlendEnable = TRUE;
    post.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    post.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    post.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    post.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    post.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    post.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    if (auto pipeline_result =
            create_post_pipeline(ui_pixel, ui_pipeline.ReleaseAndGetAddressOf());
        !pipeline_result)
    {
        return pipeline_result;
    }

    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    result = device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&draw_signature));
    return SUCCEEDED(result) ? Result::Success()
                             : HResultFailure("CreateCommandSignature", result);
}

Result D3D12Renderer::Impl::CreateGpuData()
{
    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    const auto vertex_description = BufferDescription(sizeof(kCubeVertices));
    if (auto created = CreateAllocation(vertices, upload_allocation, vertex_description,
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
    {
        return created;
    }
    std::byte *mapped{};
    D3D12_RANGE no_read{};
    auto result = vertices.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(result))
    {
        return HResultFailure("Map vertex buffer", result);
    }
    std::memcpy(mapped, kCubeVertices.data(), sizeof(kCubeVertices));
    vertices.resource->Unmap(0, nullptr);
    vertex_view = {vertices.resource->GetGPUVirtualAddress(), sizeof(kCubeVertices),
                   sizeof(Vertex)};

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
                                            particle_index_description,
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
            BufferDescription(16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        !created)
    {
        return created;
    }
    const auto indirect_description =
        BufferDescription(sizeof(D3D12_DRAW_ARGUMENTS),
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    return CreateAllocation(indirect_arguments, default_allocation, indirect_description,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

Result D3D12Renderer::Impl::CreateUiTexture()
{
    constexpr std::uint32_t texture_width = 512;
    constexpr std::uint32_t texture_height = 96;
    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = texture_width;
    bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(texture_height);
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void *bitmap_pixels{};
    const auto screen = GetDC(nullptr);
    const auto bitmap =
        CreateDIBSection(screen, &bitmap_info, DIB_RGB_COLORS, &bitmap_pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bitmap || !bitmap_pixels)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "CreateDIBSection for UI failed.");
    }

    std::array<wchar_t, 32'768> module_path{};
    const auto module_length =
        GetModuleFileNameW(nullptr, module_path.data(), static_cast<DWORD>(module_path.size()));
    if (module_length == 0 || module_length == module_path.size())
    {
        DeleteObject(bitmap);
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Cannot resolve the executable directory.");
    }
    const auto font_path =
        std::filesystem::path(module_path.data()).parent_path() / L"Fonts" / L"NotoSansKR.ttf";
    if (AddFontResourceExW(font_path.c_str(), FR_PRIVATE, nullptr) == 0)
    {
        DeleteObject(bitmap);
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Bundled Noto Sans KR font is missing or invalid.");
    }

    const auto device_context = CreateCompatibleDC(nullptr);
    if (!device_context)
    {
        RemoveFontResourceExW(font_path.c_str(), FR_PRIVATE, nullptr);
        DeleteObject(bitmap);
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Cannot create the Korean UI device context.");
    }
    const auto old_bitmap = SelectObject(device_context, bitmap);
    const auto font = CreateFontW(-42, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, HANGUL_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Noto Sans KR");
    if (!old_bitmap || !font)
    {
        DeleteDC(device_context);
        RemoveFontResourceExW(font_path.c_str(), FR_PRIVATE, nullptr);
        DeleteObject(bitmap);
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Cannot create the Korean UI font surface.");
    }
    const auto old_font = SelectObject(device_context, font);
    SetBkMode(device_context, TRANSPARENT);
    SetTextColor(device_context, RGB(245, 248, 255));
    constexpr wchar_t text[] = L"프로젝트 HS  |  STAGE 1";
    TextOutW(device_context, 8, 18, text, static_cast<int>(std::size(text) - 1));
    SelectObject(device_context, old_font);
    SelectObject(device_context, old_bitmap);
    DeleteObject(font);
    DeleteDC(device_context);
    RemoveFontResourceExW(font_path.c_str(), FR_PRIVATE, nullptr);

    auto *pixel_bytes = static_cast<std::uint8_t *>(bitmap_pixels);
    for (std::size_t index = 0; index < texture_width * texture_height; ++index)
    {
        const auto blue = pixel_bytes[index * 4 + 0];
        const auto green = pixel_bytes[index * 4 + 1];
        const auto red = pixel_bytes[index * 4 + 2];
        pixel_bytes[index * 4 + 3] = std::max({red, green, blue});
    }

    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = texture_width;
    texture_description.Height = texture_height;
    texture_description.DepthOrArraySize = 1;
    texture_description.MipLevels = 1;
    texture_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_description.SampleDesc = {1, 0};
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12MA::ALLOCATION_DESC default_allocation{};
    default_allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    if (auto created = CreateAllocation(ui_texture, default_allocation, texture_description,
                                        D3D12_RESOURCE_STATE_COPY_DEST);
        !created)
    {
        DeleteObject(bitmap);
        return created;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{};
    UINT64 total_size{};
    device->GetCopyableFootprints(&texture_description, 0, 1, 0, &footprint, &rows, &row_size,
                                  &total_size);
    AllocationResource upload;
    D3D12MA::ALLOCATION_DESC upload_allocation{};
    upload_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(upload, upload_allocation, BufferDescription(total_size),
                                        D3D12_RESOURCE_STATE_GENERIC_READ);
        !created)
    {
        DeleteObject(bitmap);
        return created;
    }
    std::byte *mapped{};
    D3D12_RANGE no_read{};
    auto result = upload.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(result))
    {
        DeleteObject(bitmap);
        return HResultFailure("Map UI upload", result);
    }
    for (std::uint32_t row = 0; row < texture_height; ++row)
    {
        std::memcpy(mapped + footprint.Offset + row * footprint.Footprint.RowPitch,
                    pixel_bytes + row * texture_width * 4, texture_width * 4);
    }
    upload.resource->Unmap(0, nullptr);
    DeleteObject(bitmap);

    result = frames[0].allocator->Reset();
    if (FAILED(result) ||
        FAILED(command_list->Reset(frames[0].allocator.Get(), nullptr)))
    {
        return HResultFailure("Reset UI upload commands", FAILED(result) ? result : E_FAIL);
    }
    if (enhanced)
    {
        TransitionTexture(ui_texture.resource.Get(), D3D12_RESOURCE_STATE_COMMON,
                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_BARRIER_LAYOUT_COMMON,
                          D3D12_BARRIER_LAYOUT_COPY_DEST);
    }
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = ui_texture.resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.resource.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = footprint;
    command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    TransitionTexture(ui_texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                      D3D12_BARRIER_LAYOUT_COPY_DEST,
                      D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    result = command_list->Close();
    if (FAILED(result))
    {
        return HResultFailure("Close UI upload commands", result);
    }
    ID3D12CommandList *lists[] = {command_list.Get()};
    queue->ExecuteCommandLists(1, lists);
    return WaitForGpu();
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

Result D3D12Renderer::Impl::CheckDevice(HRESULT result, std::string_view operation)
{
    if (SUCCEEDED(result))
    {
        return Result::Success();
    }
    const auto reason = device ? device->GetDeviceRemovedReason() : result;
    if (reason == DXGI_ERROR_DEVICE_REMOVED || reason == DXGI_ERROR_DEVICE_RESET ||
        result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET)
    {
        WriteDred(reason);
    }
    return HResultFailure(operation, result);
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

void D3D12Renderer::Impl::CountValidationErrors()
{
    if (!config.validation || !device)
    {
        return;
    }
    ComPtr<ID3D12InfoQueue> info_queue;
    if (FAILED(device.As(&info_queue)))
    {
        return;
    }
    const auto count = info_queue->GetNumStoredMessages();
    std::ofstream log(config.artifact_directory / "d3d12_validation.log", std::ios::app);
    for (std::uint64_t index = 0; index < count; ++index)
    {
        SIZE_T size{};
        info_queue->GetMessage(index, nullptr, &size);
        std::vector<std::byte> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        if (SUCCEEDED(info_queue->GetMessage(index, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
             message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION))
        {
            ++validation_errors;
            log << "id=" << message->ID << " severity=" << message->Severity << ' '
                << message->pDescription << '\n';
        }
    }
    info_queue->ClearStoredMessages();
}

void D3D12Renderer::Impl::WriteDred(HRESULT reason) const
{
    std::error_code error;
    std::filesystem::create_directories(config.artifact_directory, error);
    std::ofstream stream(config.artifact_directory / "dred.txt", std::ios::trunc);
    stream << std::format("device_removed_reason=0x{:08X}\n",
                          static_cast<std::uint32_t>(reason));
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (device && SUCCEEDED(device.As(&dred)))
    {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs)))
        {
            stream << "breadcrumbs=" << (breadcrumbs.pHeadAutoBreadcrumbNode ? "present" : "none")
                   << '\n';
        }
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&page_fault)))
        {
            stream << "page_fault_va=" << page_fault.PageFaultVA << '\n';
        }
    }
}

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
    impl_->initialized = true;
    return Result::Success();
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

Result D3D12Renderer::Render(const RenderSnapshotExchange::ReadPair &snapshots,
                             std::span<const PresentationEvent> events,
                             std::span<const ParticleSpawnCommand> particle_spawns,
                             RendererFrameResult &frame_result)
{
    if (!impl_->initialized)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Renderer not initialized.");
    }

    const auto back_buffer_index = impl_->swap_chain->GetCurrentBackBufferIndex();
    auto &frame = impl_->frames[back_buffer_index];
    if (auto result = impl_->WaitForFrame(frame); !result)
    {
        return result;
    }

    auto snapshot = snapshots.has_current ? snapshots.current : RenderSnapshot{};
    const auto instance_count =
        std::min<std::size_t>(snapshot.instances.size(), static_cast<std::size_t>(256));

    auto *constants = reinterpret_cast<FrameConstants *>(frame.mapped);
    auto *instances = reinterpret_cast<GpuInstance *>(frame.mapped + kInstanceDataOffset);
    auto *gpu_particle_spawns =
        reinterpret_cast<GpuParticleSpawnCommand *>(frame.mapped + kParticleSpawnDataOffset);
    const auto particle_capacity =
        kParticleCount * std::clamp(impl_->config.particle_percentage, 50u, 100u) / 100u;
    std::uint32_t gpu_particle_spawn_count{};
    std::uint32_t total_particles_to_spawn{};
    for (const auto &source : particle_spawns)
    {
        if (gpu_particle_spawn_count == kMaxParticleSpawnCommands ||
            total_particles_to_spawn == particle_capacity)
        {
            break;
        }
        const auto age_ticks =
            snapshot.header.tick > source.tick ? snapshot.header.tick - source.tick : 0;
        const auto age = static_cast<float>(age_ticks) / 60.0f;
        if (age >= source.lifetime || source.count == 0)
        {
            continue;
        }
        const auto count = std::min(source.count, particle_capacity - total_particles_to_spawn);
        auto &target_spawn = gpu_particle_spawns[gpu_particle_spawn_count++];
        target_spawn.position_lifetime = {source.position.x, source.position.y,
                                          source.position.z, source.lifetime};
        target_spawn.velocity_spread = {source.velocity.x, source.velocity.y,
                                        source.velocity.z, source.velocity_spread};
        target_spawn.start_color_size = {source.start_color.x, source.start_color.y,
                                         source.start_color.z, source.start_size};
        target_spawn.end_color_size = {source.end_color.x, source.end_color.y,
                                       source.end_color.z, source.end_size};
        target_spawn.physics = {source.gravity, source.rotation, source.angular_velocity, age};
        target_spawn.metadata = {
            source.sprite_index, std::max(source.sprite_count, 1u), count,
            static_cast<std::uint32_t>(source.sequence ^ (source.sequence >> 32))};
        total_particles_to_spawn += count;
    }
    const auto particle_delta_ticks =
        impl_->particles_initialized && snapshot.header.tick > impl_->last_particle_tick
            ? snapshot.header.tick - impl_->last_particle_tick
            : 0;
    const auto now = std::chrono::steady_clock::now();
    if (snapshot.header.tick != impl_->observed_snapshot_tick)
    {
        impl_->observed_snapshot_tick = snapshot.header.tick;
        impl_->snapshot_arrival = now;
    }
    const auto interpolation =
        impl_->config.interpolate && snapshots.has_previous
            ? std::clamp(
                  std::chrono::duration<float>(now - impl_->snapshot_arrival).count() * 60.0f,
                  0.0f, 1.0f)
            : 1.0f;
    const auto target = DirectX::XMVectorSet(snapshot.camera.target.x, snapshot.camera.target.y,
                                             snapshot.camera.target.z, 1.0f);
    const auto yaw = DirectX::XMConvertToRadians(snapshot.camera.yaw_degrees);
    const auto pitch = DirectX::XMConvertToRadians(snapshot.camera.pitch_degrees);
    const auto direction = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(std::cos(pitch) * std::sin(yaw), -std::sin(pitch),
                             std::cos(pitch) * std::cos(yaw), 0.0f));
    const auto eye =
        DirectX::XMVectorSubtract(target, DirectX::XMVectorScale(direction, snapshot.camera.distance));
    const auto view =
        DirectX::XMMatrixLookAtLH(eye, target, DirectX::XMVectorSet(0, 1, 0, 0));
    const auto projection = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(snapshot.camera.vertical_fov_degrees),
        static_cast<float>(impl_->render_width) / static_cast<float>(impl_->render_height), 500.0f,
        0.1f);
    DirectX::XMStoreFloat4x4(&constants->view_projection,
                             DirectX::XMMatrixTranspose(view * projection));
    if (impl_->frame_number == 0)
    {
        DirectX::XMFLOAT3 projected_origin{};
        DirectX::XMFLOAT3 projected_forward{};
        DirectX::XMStoreFloat3(
            &projected_origin,
            DirectX::XMVector3TransformCoord(DirectX::XMVectorZero(), view * projection));
        DirectX::XMStoreFloat3(
            &projected_forward,
            DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(0, 0, 20, 1),
                                             view * projection));
        std::ofstream camera_log(impl_->config.artifact_directory / "camera.json",
                                 std::ios::trunc);
        camera_log << std::format(
            "{{\"origin\":[{},{},{}],\"forward\":[{},{},{}],\"yaw\":{},\"pitch\":{}}}\n",
            projected_origin.x, projected_origin.y, projected_origin.z, projected_forward.x,
            projected_forward.y, projected_forward.z, snapshot.camera.yaw_degrees,
            snapshot.camera.pitch_degrees);
    }
    DirectX::XMStoreFloat4(&constants->camera_time, eye);
    constants->camera_time.w =
        static_cast<float>(snapshot.header.simulation_time.count()) / 1'000'000'000.0f;
    const auto light = snapshot.lights.empty() ? LightView{{-0.4f, -0.8f, 0.3f}, 3.0f, {1, 1, 1}}
                                                : snapshot.lights.front();
    constants->light_direction_intensity = {light.direction.x, light.direction.y,
                                             light.direction.z, light.intensity};
    constants->light_color = {light.color.x, light.color.y, light.color.z, 1.0f};
    constants->screen_size = {
        static_cast<float>(impl_->render_width), static_cast<float>(impl_->render_height),
        1.0f / static_cast<float>(impl_->render_width),
        1.0f / static_cast<float>(impl_->render_height)};
    constexpr float cascade_extent[] = {18.0f, 36.0f, 72.0f};
    const auto light_direction = DirectX::XMVector3Normalize(
        DirectX::XMVectorSet(light.direction.x, light.direction.y, light.direction.z, 0.0f));
    const auto light_eye =
        DirectX::XMVectorSubtract(target, DirectX::XMVectorScale(light_direction, 60.0f));
    const auto light_view =
        DirectX::XMMatrixLookAtLH(light_eye, target, DirectX::XMVectorSet(0, 1, 0, 0));
    for (std::size_t cascade = 0; cascade < std::size(cascade_extent); ++cascade)
    {
        const auto light_projection = DirectX::XMMatrixOrthographicLH(
            cascade_extent[cascade], cascade_extent[cascade], 120.0f, 0.1f);
        DirectX::XMStoreFloat4x4(
            &constants->shadow_view_projection[cascade],
            DirectX::XMMatrixTranspose(light_view * light_projection));
    }
    DirectX::XMStoreFloat4x4(&constants->archer_bones[0],
                             DirectX::XMMatrixTranspose(DirectX::XMMatrixIdentity()));
    const auto pose_time =
        snapshot.poses.empty() ? 0.0f : snapshot.poses.front().normalized_time;
    const auto bone_rotation =
        std::sin(pose_time * std::numbers::pi_v<float> * 2.0f) * 0.22f;
    const auto animated_bone =
        DirectX::XMMatrixTranslation(0.0f, 0.5f, 0.0f) *
        DirectX::XMMatrixRotationZ(bone_rotation) *
        DirectX::XMMatrixTranslation(0.0f, -0.5f, 0.0f);
    DirectX::XMStoreFloat4x4(&constants->archer_bones[1],
                             DirectX::XMMatrixTranspose(animated_bone));
    constants->render_options = {
        static_cast<float>(particle_capacity), impl_->config.bloom ? 1.0f : 0.0f,
        impl_->config.outline ? 1.0f : 0.0f,
        static_cast<float>(particle_delta_ticks) / 60.0f};
    constants->particle_options = {particle_capacity, gpu_particle_spawn_count,
                                   total_particles_to_spawn, 0};
    for (std::size_t index = 0; index < instance_count; ++index)
    {
        const auto &source = snapshot.instances[index];
        auto position = source.position;
        auto yaw_value = source.yaw;
        if (snapshots.has_previous && snapshots.previous.instances.size() == instance_count)
        {
            const auto &previous = snapshots.previous.instances[index];
            position.x = std::lerp(previous.position.x, source.position.x, interpolation);
            position.y = std::lerp(previous.position.y, source.position.y, interpolation);
            position.z = std::lerp(previous.position.z, source.position.z, interpolation);
            yaw_value = std::lerp(previous.yaw, source.yaw, interpolation);
        }
        instances[index] = {{position.x, position.y + source.scale.y * 0.5f, position.z, 1.0f},
                            source.color_rgba,
                            static_cast<std::uint32_t>(source.mesh),
                            yaw_value,
                            0.0f};
        instances[index].position_scale.w = 1.0f;
    }

    auto result = frame.allocator->Reset();
    if (FAILED(result))
    {
        return HResultFailure("Reset command allocator", result);
    }
    result = impl_->command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result))
    {
        return HResultFailure("Reset command list", result);
    }

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(impl_->render_width),
                                  static_cast<float>(impl_->render_height), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(impl_->render_width),
                             static_cast<LONG>(impl_->render_height)};
    const D3D12_VIEWPORT output_viewport{0.0f, 0.0f, static_cast<float>(impl_->width),
                                         static_cast<float>(impl_->height), 0.0f, 1.0f};
    const D3D12_RECT output_scissor{0, 0, static_cast<LONG>(impl_->width),
                                    static_cast<LONG>(impl_->height)};
    impl_->command_list->RSSetViewports(1, &viewport);
    impl_->command_list->RSSetScissorRects(1, &scissor);
    impl_->command_list->SetGraphicsRootSignature(impl_->root_signature.Get());
    impl_->command_list->SetComputeRootSignature(impl_->root_signature.Get());
    impl_->command_list->SetGraphicsRootConstantBufferView(
        0, frame.upload.resource->GetGPUVirtualAddress());
    impl_->command_list->SetComputeRootConstantBufferView(
        0, frame.upload.resource->GetGPUVirtualAddress());

    const auto rtv_start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const auto rtv_at = [&](std::uint32_t index) {
        return D3D12_CPU_DESCRIPTOR_HANDLE{
            rtv_start.ptr + static_cast<SIZE_T>(index) * impl_->rtv_stride};
    };
    const auto rtv = rtv_at(back_buffer_index);
    const auto gbuffer_base_rtv = rtv_at(kFrameCount);
    const auto gbuffer_normal_rtv = rtv_at(kFrameCount + 1);
    const auto gbuffer_position_rtv = rtv_at(kFrameCount + 2);
    const auto hdr_rtv = rtv_at(kFrameCount + 3);
    const auto oit_accumulation_rtv = rtv_at(kFrameCount + 4);
    const auto oit_revealage_rtv = rtv_at(kFrameCount + 5);
    const auto post_a_rtv = rtv_at(kFrameCount + 6);
    const auto post_b_rtv = rtv_at(kFrameCount + 7);
    const auto dsv = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap *descriptor_heaps[] = {impl_->srv_heap.Get()};
    impl_->command_list->SetDescriptorHeaps(1, descriptor_heaps);
    const auto draw_fullscreen =
        [&](ID3D12PipelineState *pipeline, D3D12_CPU_DESCRIPTOR_HANDLE target) {
            impl_->command_list->OMSetRenderTargets(1, &target, FALSE, nullptr);
            impl_->command_list->SetPipelineState(pipeline);
            impl_->command_list->SetGraphicsRootDescriptorTable(
                5, impl_->srv_heap->GetGPUDescriptorHandleForHeapStart());
            impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->command_list->DrawInstanced(3, 1, 0, 0);
        };

    impl_->graph.Reset();
    const auto particles = impl_->graph.ImportBuffer(
        {impl_->particles.resource.Get()}, Access::UnorderedWrite, "Particles");
    const auto particle_input_index = impl_->particle_input_is_a ? 0u : 1u;
    const auto particle_output_index = particle_input_index ^ 1u;
    const auto particle_alive_input = impl_->graph.ImportBuffer(
        {impl_->particle_alive[particle_input_index].resource.Get()}, Access::UnorderedWrite,
        "ParticleAliveInput");
    const auto particle_alive_output = impl_->graph.ImportBuffer(
        {impl_->particle_alive[particle_output_index].resource.Get()}, Access::UnorderedWrite,
        "ParticleAliveOutput");
    const auto particle_dead = impl_->graph.ImportBuffer(
        {impl_->particle_dead.resource.Get()}, Access::UnorderedWrite, "ParticleDead");
    const auto particle_counters = impl_->graph.ImportBuffer(
        {impl_->particle_counters.resource.Get()}, Access::UnorderedWrite, "ParticleCounters");
    const auto indirect_arguments = impl_->graph.ImportBuffer(
        {impl_->indirect_arguments.resource.Get()}, Access::UnorderedWrite, "IndirectArguments");
    const auto initial_depth_access =
        impl_->transient_textures_common ? Access::Common : Access::DepthWrite;
    const auto initial_color_access =
        impl_->transient_textures_common ? Access::Common : Access::ShaderRead;
    const auto shadow =
        impl_->graph.ImportTexture({impl_->shadow.resource.Get()}, initial_depth_access, "Shadow");
    const auto depth =
        impl_->graph.ImportTexture({impl_->depth.resource.Get()}, initial_depth_access, "Depth");
    const auto gbuffer_base = impl_->graph.ImportTexture(
        {impl_->gbuffer_base.resource.Get()}, initial_color_access, "GBufferBase");
    const auto gbuffer_normal = impl_->graph.ImportTexture(
        {impl_->gbuffer_normal.resource.Get()}, initial_color_access, "GBufferNormal");
    const auto gbuffer_position = impl_->graph.ImportTexture(
        {impl_->gbuffer_position.resource.Get()}, initial_color_access, "GBufferPosition");
    const auto hdr = impl_->graph.ImportTexture(
        {impl_->hdr_color.resource.Get()}, initial_color_access, "HdrColor");
    const auto oit_accumulation = impl_->graph.ImportTexture(
        {impl_->oit_accumulation.resource.Get()}, initial_color_access, "OitAccumulation");
    const auto oit_revealage = impl_->graph.ImportTexture(
        {impl_->oit_revealage.resource.Get()}, initial_color_access, "OitRevealage");
    const auto post_a = impl_->graph.ImportTexture(
        {impl_->post_a.resource.Get()}, initial_color_access, "PostA");
    const auto post_b = impl_->graph.ImportTexture(
        {impl_->post_b.resource.Get()}, initial_color_access, "PostB");
    const auto ui = impl_->graph.ImportTexture(
        {impl_->ui_texture.resource.Get()}, Access::ShaderRead, "UiTexture");
    const auto back_buffer = impl_->graph.ImportTexture(
        {impl_->back_buffers[back_buffer_index].Get()}, Access::Present, "BackBuffer");

    auto particle_pass = impl_->graph.AddPass("GPU Particle Spawn/Update", QueueHint::Direct);
    particle_pass.ReadWrite(particles, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_alive_input, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_alive_output, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_dead, Access::UnorderedWrite);
    particle_pass.ReadWrite(particle_counters, Access::UnorderedWrite);
    particle_pass.ReadWrite(indirect_arguments, Access::UnorderedWrite);
    const auto initialize_particles = !impl_->particles_initialized;
    particle_pass.SetExecute([&](RenderPassContext &context) {
        impl_->command_list->SetPipelineState(impl_->particle_compute_pipeline.Get());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            3, impl_->particles.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            4, impl_->indirect_arguments.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            7, impl_->particle_alive[particle_input_index].resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            8, impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            9, impl_->particle_dead.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootUnorderedAccessView(
            10, impl_->particle_counters.resource->GetGPUVirtualAddress());
        impl_->command_list->SetComputeRootShaderResourceView(
            11, frame.upload.resource->GetGPUVirtualAddress() + kParticleSpawnDataOffset);

        const auto dispatch_phase = [&](std::uint32_t phase, std::uint32_t item_count) {
            impl_->command_list->SetComputeRoot32BitConstant(6, phase, 0);
            impl_->command_list->Dispatch((std::max(item_count, 1u) + 255) / 256, 1, 1);
        };
        const auto synchronize_particle_state = [&] {
            context.UavBarrier(particles);
            context.UavBarrier(particle_alive_output);
            context.UavBarrier(particle_dead);
            context.UavBarrier(particle_counters);
        };

        if (initialize_particles)
        {
            dispatch_phase(0, particle_capacity);
            context.UavBarrier(particle_dead);
            context.UavBarrier(particle_counters);
            context.UavBarrier(indirect_arguments);
        }
        dispatch_phase(1, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
        dispatch_phase(2, particle_capacity);
        synchronize_particle_state();
        if (total_particles_to_spawn != 0)
        {
            dispatch_phase(3, total_particles_to_spawn);
            synchronize_particle_state();
        }
        dispatch_phase(4, 1);
        context.UavBarrier(particle_counters);
        context.UavBarrier(indirect_arguments);
    });

    auto shadow_pass =
        impl_->graph.AddPass("3-cascade Directional Shadow", QueueHint::Direct);
    shadow_pass.Write(shadow, Access::DepthWrite);
    shadow_pass.SetExecute([&](RenderPassContext &) {
        const auto shadow_size =
            static_cast<float>(std::clamp(impl_->config.shadow_resolution, 1024u, 2048u));
        const D3D12_VIEWPORT shadow_viewport{0, 0, shadow_size, shadow_size, 0, 1};
        const D3D12_RECT shadow_scissor{0, 0, static_cast<LONG>(shadow_size),
                                        static_cast<LONG>(shadow_size)};
        impl_->command_list->RSSetViewports(1, &shadow_viewport);
        impl_->command_list->RSSetScissorRects(1, &shadow_scissor);
        impl_->command_list->SetPipelineState(impl_->shadow_pipeline.Get());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            1, frame.upload.resource->GetGPUVirtualAddress() + kInstanceDataOffset);
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->command_list->IASetVertexBuffers(0, 1, &impl_->vertex_view);
        auto handle = impl_->dsv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += impl_->dsv_stride;
        for (std::uint32_t cascade = 0; cascade < 3; ++cascade)
        {
            impl_->command_list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0,
                                                        0, nullptr);
            impl_->command_list->OMSetRenderTargets(0, nullptr, FALSE, &handle);
            impl_->command_list->SetGraphicsRoot32BitConstant(6, cascade, 0);
            impl_->command_list->DrawInstanced(static_cast<UINT>(kCubeVertices.size()),
                                               static_cast<UINT>(instance_count), 0, 0);
            handle.ptr += impl_->dsv_stride;
        }
        impl_->command_list->RSSetViewports(1, &viewport);
        impl_->command_list->RSSetScissorRects(1, &scissor);
    });

    auto gbuffer_pass = impl_->graph.AddPass("GBuffer+Depth", QueueHint::Direct);
    gbuffer_pass.Write(gbuffer_base, Access::RenderTarget);
    gbuffer_pass.Write(gbuffer_normal, Access::RenderTarget);
    gbuffer_pass.Write(gbuffer_position, Access::RenderTarget);
    gbuffer_pass.Write(depth, Access::DepthWrite);
    gbuffer_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear_base[] = {0, 0, 0, 0};
        constexpr float clear_normal[] = {0.5f, 1.0f, 0.5f, 0};
        constexpr float clear_position[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(gbuffer_base_rtv, clear_base, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(gbuffer_normal_rtv, clear_normal, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(gbuffer_position_rtv, clear_position, 0,
                                                   nullptr);
        impl_->command_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0,
                                                   nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
            gbuffer_base_rtv, gbuffer_normal_rtv, gbuffer_position_rtv};
        impl_->command_list->OMSetRenderTargets(3, targets, FALSE, &dsv);
        impl_->command_list->SetPipelineState(impl_->scene_pipeline.Get());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            1, frame.upload.resource->GetGPUVirtualAddress() + kInstanceDataOffset);
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->command_list->IASetVertexBuffers(0, 1, &impl_->vertex_view);
        impl_->command_list->DrawInstanced(static_cast<UINT>(kCubeVertices.size()),
                                           static_cast<UINT>(instance_count), 0, 0);
    });

    auto lighting_pass = impl_->graph.AddPass("Deferred Cel Lighting", QueueHint::Direct);
    lighting_pass.Read(gbuffer_base, Access::ShaderRead);
    lighting_pass.Read(gbuffer_normal, Access::ShaderRead);
    lighting_pass.Read(gbuffer_position, Access::ShaderRead);
    lighting_pass.Read(shadow, Access::ShaderRead);
    lighting_pass.Write(hdr, Access::RenderTarget);
    lighting_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(hdr_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->deferred_pipeline.Get(), hdr_rtv);
    });

    auto transparent_pass =
        impl_->graph.AddPass("Forward Transparent/OIT", QueueHint::Direct);
    transparent_pass.Read(depth, Access::DepthRead);
    transparent_pass.Read(particles, Access::ShaderRead);
    transparent_pass.Read(particle_alive_output, Access::ShaderRead);
    transparent_pass.Read(indirect_arguments, Access::IndirectArgs);
    transparent_pass.Write(oit_accumulation, Access::RenderTarget);
    transparent_pass.Write(oit_revealage, Access::RenderTarget);
    transparent_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear_accumulation[] = {0, 0, 0, 0};
        constexpr float clear_revealage[] = {1, 1, 1, 1};
        impl_->command_list->ClearRenderTargetView(
            oit_accumulation_rtv, clear_accumulation, 0, nullptr);
        impl_->command_list->ClearRenderTargetView(
            oit_revealage_rtv, clear_revealage, 0, nullptr);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {
            oit_accumulation_rtv, oit_revealage_rtv};
        impl_->command_list->OMSetRenderTargets(2, targets, FALSE, &dsv);
        impl_->command_list->SetPipelineState(impl_->particle_pipeline.Get());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            2, impl_->particles.resource->GetGPUVirtualAddress());
        impl_->command_list->SetGraphicsRootShaderResourceView(
            12,
            impl_->particle_alive[particle_output_index].resource->GetGPUVirtualAddress());
        impl_->command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        impl_->command_list->ExecuteIndirect(
            impl_->draw_signature.Get(), 1, impl_->indirect_arguments.resource.Get(), 0, nullptr,
            0);
    });

    auto composite_pass = impl_->graph.AddPass("OIT Composite", QueueHint::Direct);
    composite_pass.Read(hdr, Access::ShaderRead);
    composite_pass.Read(oit_accumulation, Access::ShaderRead);
    composite_pass.Read(oit_revealage, Access::ShaderRead);
    composite_pass.Write(post_a, Access::RenderTarget);
    composite_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(post_a_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->composite_pipeline.Get(), post_a_rtv);
    });

    auto bloom_pass = impl_->graph.AddPass("Bloom", QueueHint::Direct);
    bloom_pass.Read(post_a, Access::ShaderRead);
    bloom_pass.Write(post_b, Access::RenderTarget);
    bloom_pass.SetExecute([&](RenderPassContext &) {
        constexpr float clear[] = {0, 0, 0, 0};
        impl_->command_list->ClearRenderTargetView(post_b_rtv, clear, 0, nullptr);
        draw_fullscreen(impl_->bloom_pipeline.Get(), post_b_rtv);
    });

    auto tone_map_pass = impl_->graph.AddPass("ToneMap", QueueHint::Direct);
    tone_map_pass.Read(post_b, Access::ShaderRead);
    tone_map_pass.Write(post_a, Access::RenderTarget);
    tone_map_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->tone_map_pipeline.Get(), post_a_rtv);
    });

    auto outline_pass =
        impl_->graph.AddPass("Screen-space Outline", QueueHint::Direct);
    outline_pass.Read(post_a, Access::ShaderRead);
    outline_pass.Read(gbuffer_normal, Access::ShaderRead);
    outline_pass.Write(post_b, Access::RenderTarget);
    outline_pass.SetExecute([&](RenderPassContext &) {
        draw_fullscreen(impl_->outline_pipeline.Get(), post_b_rtv);
    });

    auto fxaa_pass = impl_->graph.AddPass("FXAA", QueueHint::Direct);
    fxaa_pass.Read(post_b, Access::ShaderRead);
    fxaa_pass.Write(back_buffer, Access::RenderTarget);
    fxaa_pass.SetExecute([&](RenderPassContext &) {
        impl_->command_list->RSSetViewports(1, &output_viewport);
        impl_->command_list->RSSetScissorRects(1, &output_scissor);
        draw_fullscreen(impl_->fxaa_pipeline.Get(), rtv);
    });

    auto ui_pass = impl_->graph.AddPass("Game UI", QueueHint::Direct);
    ui_pass.Read(ui, Access::ShaderRead);
    ui_pass.ReadWrite(back_buffer, Access::RenderTarget);
    ui_pass.SetExecute([&](RenderPassContext &) {
        (void)events;
        draw_fullscreen(impl_->ui_pipeline.Get(), rtv);
    });

    impl_->graph.SetFinalAccess(particles, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_input, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_alive_output, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_dead, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(particle_counters, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(indirect_arguments, Access::UnorderedWrite);
    impl_->graph.SetFinalAccess(shadow, Access::DepthWrite);
    impl_->graph.SetFinalAccess(depth, Access::DepthWrite);
    impl_->graph.SetFinalAccess(gbuffer_base, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_normal, Access::ShaderRead);
    impl_->graph.SetFinalAccess(gbuffer_position, Access::ShaderRead);
    impl_->graph.SetFinalAccess(hdr, Access::ShaderRead);
    impl_->graph.SetFinalAccess(oit_accumulation, Access::ShaderRead);
    impl_->graph.SetFinalAccess(oit_revealage, Access::ShaderRead);
    impl_->graph.SetFinalAccess(post_a, Access::ShaderRead);
    impl_->graph.SetFinalAccess(post_b, Access::ShaderRead);
    impl_->graph.SetFinalAccess(back_buffer, Access::Present);

    if (auto graph_result = impl_->graph.Execute(
            impl_->command_list.Get(), impl_->enhanced_command_list.Get(),
            impl_->enhanced ? BarrierMode::Enhanced : BarrierMode::Legacy,
            impl_->timestamp_heap.Get(), impl_->timestamp_readback.resource.Get(),
            back_buffer_index * kTimestampCountPerFrame);
        !graph_result)
    {
        return graph_result;
    }
    impl_->transient_textures_common = false;
    impl_->particles_initialized = true;
    impl_->particle_input_is_a = !impl_->particle_input_is_a;
    impl_->last_particle_tick = snapshot.header.tick;
    frame.timestamps_recorded = true;

    result = impl_->command_list->Close();
    if (FAILED(result))
    {
        return HResultFailure("Close command list", result);
    }
    ID3D12CommandList *lists[] = {impl_->command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, lists);
    impl_->last_presented_index = back_buffer_index;
    result = impl_->swap_chain->Present(impl_->config.vsync ? 1 : 0, 0);
    if (FAILED(result))
    {
        return impl_->CheckDevice(result, "Present");
    }

    frame.fence_value = impl_->next_fence++;
    result = impl_->queue->Signal(impl_->fence.Get(), frame.fence_value);
    if (FAILED(result))
    {
        return impl_->CheckDevice(result, "Signal frame fence");
    }

    ++impl_->frame_number;
    frame_result = {impl_->frame_number, snapshot.header.tick};
    impl_->CountValidationErrors();
    return Result::Success();
}

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
    for (auto &frame : impl_->frames)
    {
        if (frame.upload.resource && frame.mapped)
        {
            frame.upload.resource->Unmap(0, nullptr);
            frame.mapped = nullptr;
        }
        frame.upload.Reset();
    }
    impl_->depth.Reset();
    impl_->shadow.Reset();
    impl_->vertices.Reset();
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
    impl_->ui_texture.Reset();
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
