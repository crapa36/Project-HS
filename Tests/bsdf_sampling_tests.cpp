#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
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

std::vector<std::byte> ReadBinaryFile(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open binary input: " + path.string());
    const auto size = input.tellg();
    if (size <= 0) throw std::runtime_error("binary input is empty: " + path.string());
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char *>(bytes.data()), size);
    Check(input.good(), "binary input read failed");
    return bytes;
}

ShaderBytes ReadShader(const std::filesystem::path &path)
{
    return {ReadBinaryFile(path)};
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
    description.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return description;
}

constexpr UINT kLegacyCaseCount = 7;
constexpr UINT kCoatedCaseCount = 32;
constexpr UINT kFresnelCaseCount = 14;
constexpr UINT kProbeCaseCount = kLegacyCaseCount + kCoatedCaseCount + kFresnelCaseCount;

ComPtr<ID3D12RootSignature> CreateProbeRootSignature(ID3D12Device *device)
{
    D3D12_DESCRIPTOR_RANGE1 uav_range{};
    uav_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors = 2;
    uav_range.BaseShaderRegister = 0;
    uav_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    D3D12_DESCRIPTOR_RANGE1 srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;
    srv_range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    D3D12_ROOT_PARAMETER1 parameters[2]{};
    for (auto &parameter : parameters)
    {
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[0].DescriptorTable = {1, &uav_range};
    parameters[1].DescriptorTable = {1, &srv_range};

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = std::numeric_limits<float>::max();
    sampler.ShaderRegister = 1;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC description{};
    description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    description.Desc_1_1.NumParameters = 2;
    description.Desc_1_1.pParameters = parameters;
    description.Desc_1_1.NumStaticSamplers = 1;
    description.Desc_1_1.pStaticSamplers = &sampler;

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

std::uint32_t ReadLe32(const std::vector<std::byte> &bytes, std::size_t offset)
{
    Check(offset <= bytes.size() && sizeof(std::uint32_t) <= bytes.size() - offset,
          "coated DFG DDS header field is truncated");
    std::uint32_t value{};
    for (std::uint32_t byte = 0; byte < 4; ++byte)
        value |= std::to_integer<std::uint32_t>(bytes[offset + byte]) << (8u * byte);
    return value;
}

void CheckCoatedDfgDds(const std::vector<std::byte> &first)
{
    constexpr std::size_t header_size = 148;
    constexpr std::size_t texel_count = 128u * 128u;
    constexpr std::size_t payload_size = texel_count * 8u;
    Check(first.size() == header_size + payload_size, "coated DFG DDS byte count changed");
    Check(ReadLe32(first, 0) == 0x20534444u && ReadLe32(first, 4) == 124u,
          "coated DFG DDS magic or legacy header size is invalid");
    Check(ReadLe32(first, 8) == 0x100fu && ReadLe32(first, 12) == 128u &&
          ReadLe32(first, 16) == 128u && ReadLe32(first, 20) == 1024u &&
          ReadLe32(first, 24) == 0u && ReadLe32(first, 28) == 1u,
          "coated DFG DDS extent or pitch is invalid");
    Check(ReadLe32(first, 76) == 32u && ReadLe32(first, 80) == 4u &&
          ReadLe32(first, 84) == 0x30315844u && ReadLe32(first, 108) == 0x1000u,
          "coated DFG DDS pixel format or caps is invalid");
    Check(ReadLe32(first, 128) == 10u && ReadLe32(first, 132) == 3u &&
          ReadLe32(first, 136) == 0u && ReadLe32(first, 140) == 1u &&
          ReadLe32(first, 144) == 0u,
          "coated DFG DX10 format, dimension, or array size is invalid");

    float maximum_energy{};
    for (std::size_t pixel = 0; pixel < texel_count; ++pixel)
    {
        std::array<float, 4> coefficients{};
        for (std::size_t channel = 0; channel < coefficients.size(); ++channel)
        {
            const auto offset = header_size + pixel * 8u + channel * 2u;
            const auto low = std::to_integer<std::uint16_t>(first[offset]);
            const auto high = std::to_integer<std::uint16_t>(first[offset + 1]);
            coefficients[channel] = HalfToFloat(static_cast<std::uint16_t>(low | (high << 8u)));
            Check(std::isfinite(coefficients[channel]) && coefficients[channel] >= 0.0f,
                  "coated DFG payload contains an invalid coefficient");
        }
        Check(coefficients[2] <= coefficients[0] + 1e-4f &&
              coefficients[3] <= coefficients[1] + 1e-4f,
              "coated DFG Fresnel moments exceed their base moments");
        maximum_energy = std::max(maximum_energy, coefficients[0] + coefficients[1]);
    }
    Check(maximum_energy <= 1.04f, "coated DFG base coefficients exceed the energy bound");
    std::printf("coated DFG CPU DDS bytes=%zu texels=%zu max AB=%.6f\n",
        first.size(), texel_count, maximum_energy);
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
    ComPtr<ID3D12Resource> preintegrate_upload;
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
           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
           IID_PPV_ARGS(&resources.preintegrate)),
       "create cooked preintegration texture");
    UINT preintegrate_rows{};
    UINT64 preintegrate_row_size{};
    fixture.device->GetCopyableFootprints(
        &preintegrate_description, 0, 1, 0, &resources.preintegrate_footprint,
        &preintegrate_rows, &preintegrate_row_size, &resources.preintegrate_readback_size);
    (void)preintegrate_rows;
    (void)preintegrate_row_size;
    const auto preintegrate_upload_description =
        BufferDescription(resources.preintegrate_readback_size, D3D12_RESOURCE_FLAG_NONE);
    const auto upload_heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    Hr(fixture.device->CreateCommittedResource(
           &upload_heap, D3D12_HEAP_FLAG_NONE, &preintegrate_upload_description,
           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
           IID_PPV_ARGS(&resources.preintegrate_upload)),
       "create cooked preintegration upload buffer");
    const auto preintegrate_readback_description =
        BufferDescription(resources.preintegrate_readback_size, D3D12_RESOURCE_FLAG_NONE);
    Hr(fixture.device->CreateCommittedResource(
           &readback_heap, D3D12_HEAP_FLAG_NONE, &preintegrate_readback_description,
           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
           IID_PPV_ARGS(&resources.preintegrate_readback)),
       "create preintegration readback");

    constexpr UINT64 probe_bytes = kProbeCaseCount * sizeof(float) * 4;
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
         const std::filesystem::path &probe_path,
         const std::filesystem::path &coated_dfg_path)
{
    const auto cooked_dfg = ReadBinaryFile(coated_dfg_path);
    CheckCoatedDfgDds(cooked_dfg);
    const auto preintegrate_shader = ReadShader(preintegrate_path);
    const auto probe_shader = ReadShader(probe_path);
    const auto root_signature = CreateProbeRootSignature(fixture.device.Get());
    [[maybe_unused]] const auto preintegrate_pipeline = CreateComputePipeline(
        fixture.device.Get(), root_signature.Get(), preintegrate_shader,
        "create preintegration pipeline");
    const auto probe_pipeline = CreateComputePipeline(
        fixture.device.Get(), root_signature.Get(), probe_shader,
        "create BSDF probe pipeline");
    const auto resources = CreateResources(fixture);

    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_description.NumDescriptors = 3;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    Hr(fixture.device->CreateDescriptorHeap(&heap_description,
                                            IID_PPV_ARGS(&descriptor_heap)),
       "create BSDF UAV descriptor heap");
    const UINT descriptor_stride = fixture.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC preintegrate_uav{};
    preintegrate_uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    preintegrate_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    fixture.device->CreateUnorderedAccessView(resources.preintegrate.Get(), nullptr,
                                              &preintegrate_uav, cpu_handle);
    cpu_handle.ptr += descriptor_stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC probe_uav{};
    probe_uav.Format = DXGI_FORMAT_UNKNOWN;
    probe_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    probe_uav.Buffer.NumElements = kProbeCaseCount;
    probe_uav.Buffer.StructureByteStride = sizeof(float) * 4;
    fixture.device->CreateUnorderedAccessView(resources.probe.Get(), nullptr,
                                              &probe_uav, cpu_handle);
    cpu_handle.ptr += descriptor_stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC preintegrate_srv{};
    preintegrate_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    preintegrate_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    preintegrate_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    preintegrate_srv.Texture2D.MipLevels = 1;
    fixture.device->CreateShaderResourceView(resources.preintegrate.Get(), &preintegrate_srv, cpu_handle);

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
    D3D12_GPU_DESCRIPTOR_HANDLE preintegrate_srv_gpu = gpu_start;
    preintegrate_srv_gpu.ptr += 2u * descriptor_stride;

    commands->SetComputeRootSignature(root_signature.Get());

    // Upload the exact production cooker payload, expanding its tightly packed DDS
    // rows to D3D12's aligned upload footprint. The GPU preintegration shader is
    // still compiled above as an independent build/PSO oracle, but does not produce
    // the texture exercised by the probes below.
    constexpr std::size_t dds_payload_offset = 148;
    constexpr UINT texture_row_bytes = 128u * 8u;
    Check(cooked_dfg.size() >= dds_payload_offset +
          static_cast<std::size_t>(texture_row_bytes) * 128u,
          "cooked DFG payload is shorter than the texture extent");
    D3D12_RANGE no_upload_reads{0, 0};
    void *upload_mapping{};
    Hr(resources.preintegrate_upload->Map(0, &no_upload_reads, &upload_mapping),
       "map cooked preintegration upload buffer");
    auto *upload_bytes = static_cast<std::byte *>(upload_mapping);
    for (UINT y = 0; y < 128; ++y)
        std::memcpy(upload_bytes + resources.preintegrate_footprint.Offset +
                        static_cast<std::size_t>(y) *
                            resources.preintegrate_footprint.Footprint.RowPitch,
                    cooked_dfg.data() + dds_payload_offset +
                        static_cast<std::size_t>(y) * texture_row_bytes,
                    texture_row_bytes);
    resources.preintegrate_upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION upload_source{};
    upload_source.pResource = resources.preintegrate_upload.Get();
    upload_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    upload_source.PlacedFootprint = resources.preintegrate_footprint;
    D3D12_TEXTURE_COPY_LOCATION texture_destination{};
    texture_destination.pResource = resources.preintegrate.Get();
    texture_destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    texture_destination.SubresourceIndex = 0;
    commands->CopyTextureRegion(&texture_destination, 0, 0, 0, &upload_source, nullptr);

    D3D12_RESOURCE_BARRIER preintegrate_barrier{};
    preintegrate_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preintegrate_barrier.Transition.pResource = resources.preintegrate.Get();
    preintegrate_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    preintegrate_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    preintegrate_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &preintegrate_barrier);
    D3D12_TEXTURE_COPY_LOCATION texture_source{};
    texture_source.pResource = resources.preintegrate.Get();
    texture_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    texture_source.SubresourceIndex = 0;
    texture_destination.pResource = resources.preintegrate_readback.Get();
    texture_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    texture_destination.PlacedFootprint = resources.preintegrate_footprint;
    commands->CopyTextureRegion(&texture_destination, 0, 0, 0, &texture_source, nullptr);
    preintegrate_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    preintegrate_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    commands->ResourceBarrier(1, &preintegrate_barrier);

    commands->SetPipelineState(probe_pipeline.Get());
    commands->SetComputeRootDescriptorTable(0, gpu_start);
    commands->SetComputeRootDescriptorTable(1, preintegrate_srv_gpu);
    commands->Dispatch((kProbeCaseCount + 5u) / 6u, 1, 1);
    D3D12_RESOURCE_BARRIER probe_barrier{};
    probe_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    probe_barrier.Transition.pResource = resources.probe.Get();
    probe_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    probe_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    probe_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commands->ResourceBarrier(1, &probe_barrier);
    commands->CopyBufferRegion(resources.probe_readback.Get(), 0,
                               resources.probe.Get(), 0, kProbeCaseCount * sizeof(float) * 4);
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
            const float a = HalfToFloat(row[x * 4]);
            const float b = HalfToFloat(row[x * 4 + 1]);
            const float c = HalfToFloat(row[x * 4 + 2]);
            const float d = HalfToFloat(row[x * 4 + 3]);
            Check(std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d),
                  "preintegration coefficient is not finite");
            Check(a >= 0.0f && b >= 0.0f && c >= 0.0f && d >= 0.0f,
                  "preintegration coefficient is negative");
            Check(c <= a + 1e-4f && d <= b + 1e-4f,
                  "coated preintegration moment exceeds its matching base moment");
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

    D3D12_RANGE probe_range{0, kProbeCaseCount * sizeof(float) * 4};
    const auto *probe_metrics = static_cast<const ProbeMetric *>(nullptr);
    void *mapped_probe{};
    Hr(resources.probe_readback->Map(0, &probe_range, &mapped_probe),
       "map BSDF probe readback");
    probe_metrics = static_cast<const ProbeMetric *>(mapped_probe);
    for (UINT index = 0; index < kLegacyCaseCount; ++index)
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
        Check(metric.max_error <= 1e-4f, "BSDF or local-probe blend contract mismatch");
    }
    float max_spec_error=0.0f,max_diffuse_error=0.0f,max_kernel_error=0.0f;
    for (UINT index=kLegacyCaseCount;index<kLegacyCaseCount+kCoatedCaseCount;++index)
    {
        const ProbeMetric metric=probe_metrics[index];
        Check(std::isfinite(metric.pdf_integral)&&std::isfinite(metric.valid_frequency)&&
              std::isfinite(metric.energy)&&std::isfinite(metric.max_error),
              "coated IBL reference metric is not finite");
        max_spec_error=std::max(max_spec_error,metric.pdf_integral);
        max_diffuse_error=std::max(max_diffuse_error,metric.valid_frequency);
        max_kernel_error=std::max(max_kernel_error,metric.energy);
        Check(metric.max_error<=1e-4f,"coated IBL eligibility or coat-zero contract mismatch");
    }
    std::printf("coated IBL cases=%u max spec=%.6f diffuse=%.6f SH=%.6g\n",
        kCoatedCaseCount,max_spec_error,max_diffuse_error,max_kernel_error);
    Check(max_spec_error<=0.02f,"coated split-sum specular exceeds the 0.02 reference tolerance");
    Check(max_diffuse_error<=0.002f,"coated split-sum diffuse exceeds the 0.002 reference tolerance");
    Check(max_kernel_error<=5e-5f,"coated diffuse SH kernel exceeds quadrature tolerance");
    constexpr std::array<std::uint32_t,kFresnelCaseCount> cosine_bits{
        0u,0x00800000u,0x38d1b717u,0x3c800000u,
        0x3e7fffffu,0x3e800000u,0x3e800001u,
        0x3effffffu,0x3f000000u,0x3f000001u,
        0x3f7fffffu,0x3f800000u,0xb4000000u,0x3f800001u};
    double max_fresnel_error{};
    // Three rounded multiplies plus the affine Fresnel result/FMA contraction:
    // eight float epsilons in the unit output range conservatively cover these
    // operations. Existing energy/PDF/coated integration tolerances stay intact.
    constexpr double fresnel_tolerance=8.0*std::numeric_limits<float>::epsilon();
    for(UINT index=0;index<kFresnelCaseCount;++index)
    {
        const float cosine=std::bit_cast<float>(cosine_bits[index]);
        const float x=1.0f-cosine;
        const double reference=std::pow(static_cast<double>(x),5.0);
        const float coat_x=1.0f-std::clamp(cosine,0.0f,1.0f);
        const double coat_reference=std::pow(static_cast<double>(coat_x),5.0);
        const auto metric=probe_metrics[kLegacyCaseCount+kCoatedCaseCount+index];
        const std::array<float,4> actual{metric.pdf_integral,metric.valid_frequency,metric.energy,metric.max_error};
        const std::array<double,4> expected{reference,
            double(0.04f)+double(1.0f-0.04f)*reference,1.0,
            double(0.04f)+double(0.96f)*coat_reference};
        for(std::size_t channel=0;channel<actual.size();++channel)
        {
            Check(std::isfinite(actual[channel]),"Fresnel production output is not finite");
            const auto error=std::abs(double(actual[channel])-expected[channel]);
            max_fresnel_error=std::max(max_fresnel_error,error);
            Check(error<=fresnel_tolerance,"Fresnel production output disagrees with independent pow reference");
        }
    }
    std::printf("Fresnel boundary cases=%u max error=%.9g tolerance=%.9g\n",
        kFresnelCaseCount,max_fresnel_error,fresnel_tolerance);
    resources.probe_readback->Unmap(0, nullptr);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr,
            "usage: hs_bsdf_sampling_tests <preintegrate.dxil> <probe.dxil> <coated_ggx_dfg.dds>\n");
        return 2;
    }
    try
    {
        Fixture fixture;
        Run(fixture, argv[1], argv[2], argv[3]);
        std::puts("BSDF hardware sampling and preintegration checks passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
