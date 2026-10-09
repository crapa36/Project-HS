#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

using Microsoft::WRL::ComPtr;

namespace
{
void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void Hr(HRESULT result, const char *message)
{
    Check(SUCCEEDED(result), message);
}

struct ShaderBytes
{
    std::vector<std::byte> bytes;

    D3D12_SHADER_BYTECODE Bytecode() const
    {
        return {bytes.data(), bytes.size()};
    }
};

ShaderBytes ReadShader(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open shader DXIL: " + path.string());
    const auto size = input.tellg();
    if (size <= 0) throw std::runtime_error("shader DXIL is empty: " + path.string());
    ShaderBytes shader;
    shader.bytes.resize(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char *>(shader.bytes.data()), size);
    Check(input.good(), "shader DXIL read failed");
    return shader;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferDescription(UINT64 size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

D3D12_RESOURCE_DESC PreintegrateDescription()
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = 128;
    description.Height = 128;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R16G16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return description;
}

ComPtr<ID3D12RootSignature> CreateUavRootSignature(ID3D12Device *device)
{
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;

    D3D12_ROOT_PARAMETER1 parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC description{};
    description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    description.Desc_1_1.NumParameters = 1;
    description.Desc_1_1.pParameters = &parameter;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    Hr(D3D12SerializeVersionedRootSignature(&description, &serialized, &errors),
       "serialize BSDF test root signature");

    ComPtr<ID3D12RootSignature> signature;
    Hr(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                   IID_PPV_ARGS(&signature)),
       "create BSDF test root signature");
    return signature;
}

ComPtr<ID3D12PipelineState> CreateComputePipeline(ID3D12Device *device,
                                                  ID3D12RootSignature *signature,
                                                  const ShaderBytes &shader,
                                                  const char *message)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
    description.pRootSignature = signature;
    description.CS = shader.Bytecode();
    ComPtr<ID3D12PipelineState> pipeline;
    Hr(device->CreateComputePipelineState(&description, IID_PPV_ARGS(&pipeline)), message);
    return pipeline;
}

struct ProbeMetric
{
    float pdf_integral{};
    float valid_frequency{};
    float energy{};
    float max_error{};
};

float HalfToFloat(std::uint16_t value)
{
    const std::uint32_t sign = (value >> 15u) & 1u;
    const std::uint32_t exponent = (value >> 10u) & 0x1fu;
    const std::uint32_t mantissa = value & 0x3ffu;
    std::uint32_t bits{};
    if (exponent == 0)
    {
        if (mantissa == 0) bits = sign << 31u;
        else
        {
            std::uint32_t normalized = mantissa;
            std::uint32_t shift = 0;
            while ((normalized & 0x400u) == 0)
            {
                normalized <<= 1u;
                ++shift;
            }
            normalized &= 0x3ffu;
            bits = (sign << 31u) | ((127u - 14u - shift) << 23u) | (normalized << 13u);
        }
    }
    else if (exponent == 0x1fu)
    {
        bits = (sign << 31u) | 0x7f800000u | (mantissa << 13u);
    }
    else
    {
        bits = (sign << 31u) | ((exponent + 127u - 15u) << 23u) | (mantissa << 13u);
    }
    float result{};
    static_assert(sizeof(result) == sizeof(bits));
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

struct Fixture
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};
    UINT64 next_fence_value{};

    Fixture()
    {
        ComPtr<ID3D12Debug> debug;
        Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),
           "D3D12 debug layer unavailable");
        debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> gpu_debug;
        Hr(debug.As(&gpu_debug), "D3D12 GPU validation interface unavailable");
        gpu_debug->SetEnableGPUBasedValidation(TRUE);

        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "create DXGI factory");
        for (UINT index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT result = factory->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            Hr(result, "enumerate hardware adapter");
            DXGI_ADAPTER_DESC1 description{};
            Hr(candidate->GetDesc1(&description), "query hardware adapter");
            if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

            ComPtr<ID3D12Device> probe_device;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&probe_device))))
            {
                adapter = candidate;
                break;
            }
        }
        Check(adapter != nullptr, "hardware D3D12 adapter unavailable");
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device)),
           "create hardware D3D12 device");

        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&queue)),
           "create direct command queue");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
           "create completion fence");
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Check(fence_event != nullptr, "create completion event");
        CheckInfoQueue();
    }

    ~Fixture()
    {
        if (fence_event) CloseHandle(fence_event);
    }

    void CheckInfoQueue()
    {
        ComPtr<ID3D12InfoQueue> info;
        if (FAILED(device.As(&info))) return;
        const UINT64 count = info->GetNumStoredMessages();
        for (UINT64 index = 0; index < count; ++index)
        {
            SIZE_T size{};
            Hr(info->GetMessage(index, nullptr, &size), "query D3D12 info message size");
            std::vector<std::byte> bytes(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
            Hr(info->GetMessage(index, message, &size), "query D3D12 info message");
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
            {
                throw std::runtime_error(message->pDescription);
            }
        }
        info->ClearStoredMessages();
    }

    void ExecuteAndWait(ID3D12GraphicsCommandList *commands)
    {
        Hr(commands->Close(), "close BSDF test command list");
        ID3D12CommandList *lists[] = {commands};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++next_fence_value;
        Hr(queue->Signal(fence.Get(), value), "signal BSDF test fence");
        Hr(fence->SetEventOnCompletion(value, fence_event), "set BSDF test fence event");
        Check(WaitForSingleObject(fence_event, INFINITE) == WAIT_OBJECT_0,
              "wait for BSDF test GPU work");
        CheckInfoQueue();
    }
};

struct Resources
{
    ComPtr<ID3D12Resource> preintegrate;
    ComPtr<ID3D12Resource> preintegrate_readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT preintegrate_footprint{};
    UINT64 preintegrate_readback_size{};
    ComPtr<ID3D12Resource> probe;
    ComPtr<ID3D12Resource> probe_readback;
};

Resources CreateResources(Fixture &fixture)
{
    Resources resources;
    const auto default_heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const auto readback_heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);

    const auto preintegrate_description = PreintegrateDescription();
    Hr(fixture.device->CreateCommittedResource(
           &default_heap, D3D12_HEAP_FLAG_NONE, &preintegrate_description,
           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
           IID_PPV_ARGS(&resources.preintegrate)),
       "create preintegration UAV");
    UINT preintegrate_rows{};
    UINT64 preintegrate_row_size{};
    fixture.device->GetCopyableFootprints(
        &preintegrate_description, 0, 1, 0, &resources.preintegrate_footprint,
        &preintegrate_rows, &preintegrate_row_size, &resources.preintegrate_readback_size);
    (void)preintegrate_rows;
    (void)preintegrate_row_size;
    const auto preintegrate_readback_description =
        BufferDescription(resources.preintegrate_readback_size, D3D12_RESOURCE_FLAG_NONE);
    Hr(fixture.device->CreateCommittedResource(
           &readback_heap, D3D12_HEAP_FLAG_NONE, &preintegrate_readback_description,
           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
           IID_PPV_ARGS(&resources.preintegrate_readback)),
       "create preintegration readback");

    constexpr UINT64 probe_bytes = 7 * sizeof(float) * 4;
    const auto probe_description = BufferDescription(
        probe_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Hr(fixture.device->CreateCommittedResource(
           &default_heap, D3D12_HEAP_FLAG_NONE, &probe_description,
           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
           IID_PPV_ARGS(&resources.probe)),
       "create BSDF probe UAV");
    const auto probe_readback_description = BufferDescription(probe_bytes, D3D12_RESOURCE_FLAG_NONE);
    Hr(fixture.device->CreateCommittedResource(
           &readback_heap, D3D12_HEAP_FLAG_NONE, &probe_readback_description,
           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
           IID_PPV_ARGS(&resources.probe_readback)),
       "create BSDF probe readback");
    return resources;
}

void Run(Fixture &fixture, const std::filesystem::path &preintegrate_path,
         const std::filesystem::path &probe_path)
{
    const auto preintegrate_shader = ReadShader(preintegrate_path);
    const auto probe_shader = ReadShader(probe_path);
    const auto root_signature = CreateUavRootSignature(fixture.device.Get());
    const auto preintegrate_pipeline = CreateComputePipeline(
        fixture.device.Get(), root_signature.Get(), preintegrate_shader,
        "create preintegration pipeline");
    const auto probe_pipeline = CreateComputePipeline(
        fixture.device.Get(), root_signature.Get(), probe_shader,
        "create BSDF probe pipeline");
    const auto resources = CreateResources(fixture);

    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 2;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Hr(fixture.device->CreateDescriptorHeap(&heap_description,
                                            IID_PPV_ARGS(&descriptor_heap)),
       "create BSDF UAV descriptor heap");
    const UINT descriptor_stride = fixture.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC preintegrate_uav{};
    preintegrate_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    preintegrate_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    fixture.device->CreateUnorderedAccessView(resources.preintegrate.Get(), nullptr,
                                              &preintegrate_uav, cpu_handle);
    cpu_handle.ptr += descriptor_stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC probe_uav{};
    probe_uav.Format = DXGI_FORMAT_UNKNOWN;
    probe_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    probe_uav.Buffer.NumElements = 7;
    probe_uav.Buffer.StructureByteStride = sizeof(float) * 4;
    fixture.device->CreateUnorderedAccessView(resources.probe.Get(), nullptr,
                                              &probe_uav, cpu_handle);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    Hr(fixture.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&allocator)),
       "create BSDF test command allocator");
    Hr(fixture.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&commands)),
       "create BSDF test command list");
    ID3D12DescriptorHeap *descriptor_heaps[] = {descriptor_heap.Get()};
    commands->SetDescriptorHeaps(1, descriptor_heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE probe_gpu = gpu_start;
    probe_gpu.ptr += descriptor_stride;

    commands->SetComputeRootSignature(root_signature.Get());
    commands->SetPipelineState(preintegrate_pipeline.Get());
    commands->SetComputeRootDescriptorTable(0, gpu_start);
    commands->Dispatch(16, 16, 1);

    D3D12_RESOURCE_BARRIER preintegrate_barrier{};
    preintegrate_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preintegrate_barrier.Transition.pResource = resources.preintegrate.Get();
    preintegrate_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    preintegrate_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    preintegrate_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &preintegrate_barrier);
    D3D12_TEXTURE_COPY_LOCATION texture_source{};
    texture_source.pResource = resources.preintegrate.Get();
    texture_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    texture_source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION texture_destination{};
    texture_destination.pResource = resources.preintegrate_readback.Get();
    texture_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    texture_destination.PlacedFootprint = resources.preintegrate_footprint;
    commands->CopyTextureRegion(&texture_destination, 0, 0, 0, &texture_source, nullptr);

    commands->SetPipelineState(probe_pipeline.Get());
    commands->SetComputeRootDescriptorTable(0, probe_gpu);
    commands->Dispatch(2, 1, 1);
    D3D12_RESOURCE_BARRIER probe_barrier{};
    probe_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    probe_barrier.Transition.pResource = resources.probe.Get();
    probe_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    probe_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    probe_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &probe_barrier);
    commands->CopyBufferRegion(resources.probe_readback.Get(), 0,
                               resources.probe.Get(), 0, 7 * sizeof(float) * 4);
    fixture.ExecuteAndWait(commands.Get());

    D3D12_RANGE preintegrate_range{0, resources.preintegrate_readback_size};
    void *preintegrate_mapping{};
    Hr(resources.preintegrate_readback->Map(
           0, &preintegrate_range, &preintegrate_mapping),
       "map preintegration readback");
    const auto *preintegrate_bytes = static_cast<const std::byte *>(preintegrate_mapping);
    float smooth_normal = 0.0f;
    float rough_normal = 0.0f;
    for (UINT y = 0; y < 128; ++y)
    {
        const auto *row = reinterpret_cast<const std::uint16_t *>(
            preintegrate_bytes + resources.preintegrate_footprint.Offset +
            y * resources.preintegrate_footprint.Footprint.RowPitch);
        for (UINT x = 0; x < 128; ++x)
        {
            const float a = HalfToFloat(row[x * 2]);
            const float b = HalfToFloat(row[x * 2 + 1]);
            Check(std::isfinite(a) && std::isfinite(b), "preintegration coefficient is not finite");
            Check(a >= 0.0f && b >= 0.0f, "preintegration coefficient is negative");
            Check(a + b <= 1.04f, "preintegration coefficient exceeds energy bound");
            if (x == 127 && y == 0) smooth_normal = a + b;
            if (x == 127 && y == 127) rough_normal = a + b;
        }
    }
    resources.preintegrate_readback->Unmap(0, nullptr);

    Check(smooth_normal > 0.75f && smooth_normal < 1.25f,
          "smooth normal preintegration is outside the expected range");
    Check(rough_normal > 0.25f && rough_normal < 0.4f,
          "rough normal preintegration is outside the physical bound");
    std::printf("preintegrate normal smooth sum=%.6f rough sum=%.6f\n",
                smooth_normal, rough_normal);

    D3D12_RANGE probe_range{0, 7 * sizeof(float) * 4};
    const auto *probe_metrics = static_cast<const ProbeMetric *>(nullptr);
    void *mapped_probe{};
    Hr(resources.probe_readback->Map(0, &probe_range, &mapped_probe),
       "map BSDF probe readback");
    probe_metrics = static_cast<const ProbeMetric *>(mapped_probe);
    for (UINT index = 0; index < 7; ++index)
    {
        const ProbeMetric metric = probe_metrics[index];
        std::printf("probe[%u] pdf=%.6f valid=%.6f energy=%.6f error=%.6g\n",
                    index, metric.pdf_integral, metric.valid_frequency,
                    metric.energy, metric.max_error);
        Check(std::isfinite(metric.pdf_integral) &&
                  std::isfinite(metric.valid_frequency) &&
                  std::isfinite(metric.energy) && std::isfinite(metric.max_error),
              "BSDF probe metric is not finite");
        Check(metric.pdf_integral >= 0.0f && metric.valid_frequency >= 0.0f &&
                  metric.energy >= 0.0f,
              "BSDF probe metric is negative");
        Check(std::abs(metric.pdf_integral - metric.valid_frequency) <= 0.035f,
              "BSDF PDF integral disagrees with valid sample frequency");
        Check(metric.energy <= 1.04f, "BSDF sampled energy exceeds physical bound");
        Check(metric.max_error <= 1e-4f, "BSDF sample/PDF/evaluate contract mismatch");
    }
    resources.probe_readback->Unmap(0, nullptr);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: hs_bsdf_sampling_tests <preintegrate.dxil> <probe.dxil>\n");
        return 2;
    }
    try
    {
        Fixture fixture;
        Run(fixture, argv[1], argv[2]);
        std::puts("BSDF hardware sampling and preintegration checks passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
