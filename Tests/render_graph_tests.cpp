#include "render_graph.hpp"
#include <D3D12MemAlloc.h>
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <vector>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void Hr(HRESULT result, const char *message) { Check(SUCCEEDED(result), message); }
struct Fixture
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> commands_allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    D3D12MA::Allocator *allocator{};
    HANDLE completed{};
    UINT64 serial{};
    Fixture()
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP device");
        D3D12MA::ALLOCATOR_DESC allocation{}; allocation.pDevice=device.Get(); allocation.pAdapter=adapter.Get();
        Hr(D3D12MA::CreateAllocator(&allocation,&allocator), "D3D12MA allocator");
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)), "queue");
        Hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&commands_allocator)), "command allocator");
        Hr(device->CreateCommandList(0,q.Type,commands_allocator.Get(),nullptr,IID_PPV_ARGS(&commands)), "command list");
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)), "fence");
        completed=CreateEventW(nullptr,FALSE,FALSE,nullptr); Check(completed!=nullptr,"fence event");
    }
    ~Fixture() { if(completed) CloseHandle(completed); if(allocator) allocator->Release(); }
    void Submit()
    {
        Hr(commands->Close(),"close"); ID3D12CommandList *lists[]{commands.Get()}; queue->ExecuteCommandLists(1,lists);
        Hr(queue->Signal(fence.Get(),++serial),"signal"); Hr(fence->SetEventOnCompletion(serial,completed),"completion");
        Check(WaitForSingleObject(completed,10000)==WAIT_OBJECT_0,"GPU timeout");
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device.As(&info)))
        {
            for (UINT64 i=0; i<info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
            {
                SIZE_T size{}; Hr(info->GetMessage(i,nullptr,&size),"validation message size");
                std::vector<char> bytes(size); auto *message=reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
                Hr(info->GetMessage(i,message,&size),"validation message");
                Check(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR&&message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,message->pDescription);
            }
            info->ClearStoredMessages();
        }
        Hr(commands_allocator->Reset(),"allocator reset"); Hr(commands->Reset(commands_allocator.Get(),nullptr),"list reset");
    }
};
void Run(Fixture &f, hs::BarrierMode mode, ID3D12GraphicsCommandList7 *enhanced)
{
    hs::TransientResourcePool first,second;
    Check(first.Initialize(f.allocator).Succeeded() && second.Initialize(f.allocator).Succeeded(),"pool initialize");
    hs::RenderGraphBuilder graph;
    const hs::TextureDesc desc{32,32,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    auto texture=graph.CreateTexture(desc,"first"); auto other=graph.CreateTexture(desc,"second");
    auto unused=graph.CreateBuffer({256,0},"unused");
    auto storage=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"storage");
    auto write=graph.AddPass("write",hs::QueueHint::Direct); write.Write(texture,hs::Access::UnorderedWrite); write.Write(other,hs::Access::UnorderedWrite);write.Write(storage,hs::Access::UnorderedWrite);
    bool called{}; write.SetExecute([&](hs::RenderPassContext &context){called=true;Check(context.Resolve(texture)!=nullptr,"callback resolve");context.UavBarrier(texture);});
    auto read=graph.AddPass("read",hs::QueueHint::Direct); read.Read(texture,hs::Access::ComputeRead); read.SetExecute([](auto &){});
    Check(graph.Prepare(first).Succeeded(),"prepare valid graph");
    Check(graph.Resolve(storage)!=nullptr,"buffer allocation");
    auto *native=graph.Resolve(texture); Check(native && native!=graph.Resolve(other) && !graph.Resolve(unused),"distinct claims and unused allocation");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded()&&called,"execute valid graph"); f.Submit();
    Check(graph.FinalAccess(texture)==hs::Access::ComputeRead,"compute final state");
    first.ResetClaims(); graph.Reset();
    auto reused_buffer=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"buffer reuse");
    auto buffer_write=graph.AddPass("buffer reuse",hs::QueueHint::Direct);buffer_write.Write(reused_buffer,hs::Access::UnorderedWrite);buffer_write.SetExecute([](auto &){});
    graph.SetFinalAccess(reused_buffer,hs::Access::ComputeRead);
    Check(graph.Prepare(first).Succeeded()&&graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"submitted buffer reuse");f.Submit();
    first.ResetClaims();graph.Reset();
    reused_buffer=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"buffer second reuse");
    auto buffer_again=graph.AddPass("buffer second reuse",hs::QueueHint::Direct);buffer_again.Write(reused_buffer,hs::Access::UnorderedWrite);buffer_again.SetExecute([](auto &){});
    Check(graph.Prepare(first).Succeeded()&&graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"submitted buffer final-state reuse");f.Submit();
    first.ResetClaims(); graph.Reset(); texture=graph.CreateTexture(desc,"reuse");
    auto reuse=graph.AddPass("reuse",hs::QueueHint::Direct);reuse.Write(texture,hs::Access::UnorderedWrite);reuse.SetExecute([](auto &){});
    Check(graph.Prepare(first).Succeeded()&&graph.Resolve(texture)==native,"descriptor keyed reuse");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"reuse previous physical state");f.Submit();
    graph.Reset();texture=graph.CreateTexture(desc,"other frame");auto frame=graph.AddPass("other frame",hs::QueueHint::Direct);
    frame.Write(texture,hs::Access::UnorderedWrite);frame.SetExecute([](auto &){});
    Check(graph.Prepare(second).Succeeded()&&graph.Resolve(texture)!=native,"separate in-flight slots");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"other frame execute");f.Submit();
    first.Clear();second.Clear();
    graph.Reset();texture=graph.CreateTexture(desc,"cleared pool"); auto cleared=graph.AddPass("cleared pool",hs::QueueHint::Direct);
    cleared.Write(texture,hs::Access::UnorderedWrite);cleared.SetExecute([](auto &){});
    Check(graph.Prepare(first).Succeeded(),"clear retains allocator");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"cleared pool execution");f.Submit();
    first.ResetClaims();
    graph.Reset();texture=graph.CreateTexture(desc,"uninitialized");auto invalid=graph.AddPass("invalid",hs::QueueHint::Direct);
    invalid.Read(texture,hs::Access::ComputeRead);invalid.SetExecute([&](auto &){throw std::runtime_error("invalid callback executed");});
    Check(!graph.Prepare(first).Succeeded()&&!graph.Resolve(texture),"reject first read");
    Check(!graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"execute requires prepared graph");
    graph.Reset();texture=graph.CreateTexture(desc,"uninitialized read write");auto read_write=graph.AddPass("read write",hs::QueueHint::Direct);
    read_write.ReadWrite(texture,hs::Access::UnorderedWrite);read_write.SetExecute([](auto &){});
    Check(!graph.Prepare(first).Succeeded(),"reject first ReadWrite");
    graph.Reset();texture=graph.CreateTexture({0,32,DXGI_FORMAT_R8_UNORM,0},"invalid dimensions");
    Check(!graph.Prepare(first).Succeeded(),"reject invalid descriptor even when unused");
    graph.Reset();texture=graph.CreateTexture(desc,"missing callback");auto missing=graph.AddPass("missing callback",hs::QueueHint::Direct);missing.Write(texture,hs::Access::UnorderedWrite);
    Check(!graph.Prepare(first).Succeeded(),"reject missing callback before allocation");
    graph.Reset();texture=graph.CreateTexture(desc,"wrong queue");auto wrong_queue=graph.AddPass("compute queue",hs::QueueHint::Compute);
    wrong_queue.Write(texture,hs::Access::UnorderedWrite);wrong_queue.SetExecute([](auto &){});
    Check(!graph.Prepare(first).Succeeded(),"reject unsupported queue before allocation");
    graph.Reset(); auto buffer=graph.CreateBuffer({256,0},"no UAV flag");auto flags=graph.AddPass("flags",hs::QueueHint::Direct);
    flags.Write(buffer,hs::Access::UnorderedWrite);flags.SetExecute([](auto &){});Check(!graph.Prepare(first).Succeeded(),"reject access flags");
}
}
int main()
{
    try
    {
        Fixture fixture; Run(fixture,hs::BarrierMode::Legacy,nullptr);
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{}; ComPtr<ID3D12GraphicsCommandList7> enhanced;
        if(SUCCEEDED(fixture.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&options,sizeof(options)))&&options.EnhancedBarriersSupported&&SUCCEEDED(fixture.commands.As(&enhanced)))
            Run(fixture,hs::BarrierMode::Enhanced,enhanced.Get());
        else std::puts("Enhanced barriers unavailable on WARP; legacy checks completed.");
        std::puts("Render graph transient tests passed."); return 0;
    }
    catch(const std::exception &error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
