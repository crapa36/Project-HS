#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <cstddef>
#include <fstream>
#include <string_view>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

struct HzbProbeInput
{
    uint width, height, levels, capacity, min_x, min_y, max_x, max_y;
};
struct HzbProbeOutput { uint valid; HsHzbRegion region; };
static_assert(sizeof(HzbProbeInput)==32 && sizeof(HzbProbeOutput)==36);

// Test-only D3D12 dispatch. The existing CPU test independently checks tile
// ownership, level minimality, overflow rejection and the reduced sphere hole;
// every tested input is then compared against this compiled production helper.
inline void VerifyHzbReadback(const char* shader_path, bool warp,
    const std::vector<HzbProbeInput>& inputs,
    const std::vector<HzbProbeOutput>& expected)
{
    using Microsoft::WRL::ComPtr;
    auto require=[](bool ok, const char* message) {
        if (!ok) throw std::runtime_error(message);
    };
    auto hr=[&](HRESULT result, const char* message) { require(SUCCEEDED(result),message); };
    ComPtr<ID3D12Debug> debug;
    hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"HZB debug layer unavailable");
    debug->EnableDebugLayer();
    ComPtr<ID3D12Debug1> gpu_debug;
    hr(debug.As(&gpu_debug),"HZB GPU validation unavailable");
    gpu_debug->SetEnableGPUBasedValidation(TRUE);

    ComPtr<IDXGIFactory4> factory;
    hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"HZB DXGI factory");
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    if (warp)
        hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"HZB WARP adapter unavailable");
    else
    {
        for (UINT i=0;;++i)
        {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result=factory->EnumAdapters1(i,&candidate);
            if (result==DXGI_ERROR_NOT_FOUND) break;
            hr(result,"HZB adapter enumeration");
            DXGI_ADAPTER_DESC1 desc{};
            hr(candidate->GetDesc1(&desc),"HZB adapter description");
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(),D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&device)))) { adapter=candidate; break; }
        }
        require(adapter!=nullptr,"HZB hardware adapter unavailable (no WARP fallback)");
    }
    if (!device)
        hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&device)),"HZB device creation");
    DXGI_ADAPTER_DESC1 adapter_desc{};
    hr(adapter->GetDesc1(&adapter_desc),"HZB selected adapter description");
    std::wcout << L"HZB adapter: " << adapter_desc.Description
               << (warp ? L" (explicit WARP)" : L" (hardware)") << L"; GBV enabled\n";

    std::ifstream file(shader_path,std::ios::binary|std::ios::ate);
    require(static_cast<bool>(file),"HZB probe DXIL missing");
    const auto shader_size=file.tellg();
    require(shader_size>0,"HZB probe DXIL empty");
    std::vector<char> shader(static_cast<std::size_t>(shader_size));
    file.seekg(0); file.read(shader.data(),shader_size);
    require(file.good(),"HZB probe DXIL read failed");
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues=1;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC signature_desc{};
    signature_desc.NumParameters=3; signature_desc.pParameters=parameters;
    ComPtr<ID3DBlob> serialized, errors;
    hr(D3D12SerializeRootSignature(&signature_desc,D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,&errors),"HZB root signature serialization");
    ComPtr<ID3D12RootSignature> signature;
    hr(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
        IID_PPV_ARGS(&signature)),"HZB root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature=signature.Get();
    pipeline_desc.CS={shader.data(),shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    hr(device->CreateComputePipelineState(&pipeline_desc,IID_PPV_ARGS(&pipeline)),"HZB compute pipeline");

    auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,
        D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type=heap; properties.CreationNodeMask=properties.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes; desc.Height=1;
        desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags=flags;
        ComPtr<ID3D12Resource> resource;
        hr(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,
            nullptr,IID_PPV_ARGS(&resource)),"HZB buffer creation");
        return resource;
    };
    const auto input_bytes=inputs.size()*sizeof(HzbProbeInput);
    const auto output_bytes=expected.size()*sizeof(HzbProbeOutput);
    auto upload=buffer(input_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto output=buffer(output_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback=buffer(output_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped{};
    const D3D12_RANGE no_read{0,0};
    hr(upload->Map(0,&no_read,&mapped),"HZB upload map");
    std::memcpy(mapped,inputs.data(),input_bytes); upload->Unmap(0,nullptr);
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue;
    hr(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)),"HZB queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"HZB allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),
        IID_PPV_ARGS(&commands)),"HZB command list");
    commands->SetComputeRootSignature(signature.Get());
    commands->SetComputeRoot32BitConstant(0,static_cast<UINT>(inputs.size()),0);
    commands->SetComputeRootShaderResourceView(1,upload->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(2,output->GetGPUVirtualAddress());
    commands->Dispatch(static_cast<UINT>((inputs.size()+63)/64),1,1);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};
    commands->ResourceBarrier(1,&barrier);
    commands->CopyResource(readback.Get(),output.Get());
    hr(commands->Close(),"HZB close command list");
    ComPtr<ID3D12Fence> fence;
    hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"HZB fence");
    struct Event { HANDLE handle=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        ~Event() { if(handle) CloseHandle(handle); } } event;
    require(event.handle!=nullptr,"HZB fence event");
    ID3D12CommandList* lists[]={commands.Get()};
    queue->ExecuteCommandLists(1,lists);
    hr(queue->Signal(fence.Get(),1),"HZB signal fence");
    hr(fence->SetEventOnCompletion(1,event.handle),"HZB completion event");
    require(WaitForSingleObject(event.handle,60000)==WAIT_OBJECT_0,"HZB dispatch timeout");
    hr(device->GetDeviceRemovedReason(),"HZB device removed");

    ComPtr<ID3D12InfoQueue> info;
    hr(device.As(&info),"HZB validation info queue");
    for (UINT64 i=0;i<info->GetNumStoredMessages();++i)
    {
        SIZE_T size{};
        hr(info->GetMessage(i,nullptr,&size),"HZB validation message size");
        std::vector<std::byte> bytes(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        hr(info->GetMessage(i,message,&size),"HZB validation message");
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
    const D3D12_RANGE read_range{0,output_bytes};
    hr(readback->Map(0,&read_range,&mapped),"HZB readback map");
    const auto* actual=static_cast<const HzbProbeOutput*>(mapped);
    std::size_t mismatch=expected.size();
    for (std::size_t i=0;i<expected.size();++i)
        if(actual[i].valid!=expected[i].valid || (actual[i].valid &&
            std::memcmp(&actual[i].region,&expected[i].region,sizeof(HsHzbRegion))!=0))
            { mismatch=i; break; }
    readback->Unmap(0,&no_read);
    if(mismatch!=expected.size())
        throw std::runtime_error("HZB compiled helper mismatch at case "+std::to_string(mismatch));
    std::cout << "PASS compiled HZB readback: " << inputs.size() << " cases\n";
}
