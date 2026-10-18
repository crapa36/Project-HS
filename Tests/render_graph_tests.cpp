#include "render_graph.hpp"
#include "render_graph_membership.hpp"
#include <D3D12MemAlloc.h>
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <vector>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <chrono>
#include <stdexcept>
#include <string_view>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}

using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void Hr(HRESULT result, const char *message) { Check(SUCCEEDED(result), message); }

struct MembershipWork
{
    std::uint64_t hashes{}, equalities{};
    std::uint64_t Total() const { return hashes + equalities; }
};
struct MembershipResource
{
    std::uint32_t value{};
    MembershipWork *work{};
    bool operator==(const MembershipResource &other) const
    {
        ++work->equalities;
        return value == other.value;
    }
};
struct MembershipHash
{
    std::size_t operator()(const MembershipResource &resource) const noexcept
    {
        ++resource.work->hashes;
        return std::hash<std::uint32_t>{}(resource.value);
    }
};

std::uint64_t CheckBatchResourceMembership(std::uint32_t count)
{
    MembershipWork work;
    std::vector<MembershipResource> ordered;
    std::optional<std::unordered_set<MembershipResource, MembershipHash>> membership;
    std::vector<std::uint32_t> expected;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto value = (i * 17u) % count;
        Check(hs::render_graph::AppendUniqueBatchResource(ordered, membership,
            MembershipResource{value, &work}), "first batch resource must be appended");
        expected.push_back(value);
        if (i + 1 < 1024) Check(!membership, "small batches do not allocate a membership index");
    }
    for (std::uint32_t i = count; i > 0; --i)
        Check(!hs::render_graph::AppendUniqueBatchResource(ordered, membership,
            MembershipResource{expected[i - 1], &work}), "duplicate batch resource must be rejected");
    Check(ordered.size() == expected.size(), "batch membership preserves unique resource count");
    for (std::size_t i = 0; i < ordered.size(); ++i)
        Check(ordered[i].value == expected[i], "batch membership preserves first-use order");
    return work.Total();
}

void BatchResourceMembership()
{
    for (const auto count : {0u, 1u, 31u, 32u, 33u, 1023u, 1024u, 1025u})
        (void)CheckBatchResourceMembership(count);
    const auto small_work = CheckBatchResourceMembership(4096);
    const auto large_work = CheckBatchResourceMembership(8192);
    Check(large_work <= 8192u * 16u + 1024u * 1024u && large_work <= small_work * 3u + 1024u,
          "doubling large batch resources must retain linear hash/equality work");

    std::vector<std::uint32_t> first, second;
    std::optional<std::unordered_set<std::uint32_t>> first_membership, second_membership;
    for (std::uint32_t i = 0; i < 1040; ++i)
    {
        Check(hs::render_graph::AppendUniqueBatchResource(first, first_membership, i), "first batch insert");
        Check(hs::render_graph::AppendUniqueBatchResource(second, second_membership, 1039u - i), "second batch insert");
    }
    Check(!hs::render_graph::AppendUniqueBatchResource(first, first_membership, 0u) &&
          !hs::render_graph::AppendUniqueBatchResource(second, second_membership, 1039u),
          "returning to an older batch still rejects duplicates after index promotion");
    Check(hs::render_graph::AppendUniqueBatchResource(first, first_membership, 1040u) &&
          first.front() == 0 && first.back() == 1040 && second.front() == 1039 && second.back() == 0,
          "interleaved batch membership remains independent and ordered");
}
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
    Fixture(bool hardware)
    {
        ComPtr<ID3D12Debug> debug;
        Hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12 debug interface");
        debug->EnableDebugLayer();
        ComPtr<ID3D12Debug1> gpu_debug;
        Hr(debug.As(&gpu_debug), "GPU validation interface");
        gpu_debug->SetEnableGPUBasedValidation(TRUE);
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
        if (hardware) Hr(factory->EnumAdapters(0, &adapter), "Hardware adapter");
        else Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
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
void BufferRanges(Fixture &f, hs::BarrierMode mode, ID3D12GraphicsCommandList7 *enhanced)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = 2048; description.Height = 1;
    description.DepthOrArraySize = description.MipLevels = 1;
    description.SampleDesc.Count = 1; description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload, readback;
    Hr(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "range upload");
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    Hr(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "range readback");
    unsigned char *bytes{};
    D3D12_RANGE no_read{0, 0};
    Hr(upload->Map(0, &no_read, reinterpret_cast<void **>(&bytes)), "map range upload");
    for (unsigned i = 0; i < 1024; ++i) bytes[i] = static_cast<unsigned char>((i / 512) * 97 + i % 251);
    upload->Unmap(0, nullptr);
    hs::TransientResourcePool pool;
    Check(pool.Initialize(f.allocator, f.device.Get()).Succeeded(), "range pool");
    hs::RenderGraphBuilder graph;
    const auto buffer = graph.CreateBuffer({1024, 0}, "partial copies");
    auto first = graph.AddPass(10, "first half", hs::QueueHint::Direct);
    const auto v1 = first.Write(graph.Version(buffer), hs::Access::CopyDest, hs::BufferRange{0, 512});
    first.SetExecute([&](hs::RenderPassContext &context) {
        f.commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(buffer)), 0, upload.Get(), 0, 512);
    });
    auto second = graph.AddPass(0, "second half", hs::QueueHint::Direct);
    const auto v2 = second.Write(v1, hs::Access::CopyDest, hs::BufferRange{512, 512});
    second.SetExecute([&](hs::RenderPassContext &context) {
        f.commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(buffer)), 512, upload.Get(), 512, 512);
    });
    auto old_reader = graph.AddPass(30, "preserve old first half", hs::QueueHint::Direct);
    old_reader.Read(v1, hs::Access::CopySource, hs::BufferRange{0, 512});
    old_reader.SetExecute([&](hs::RenderPassContext &context) {
        f.commands->CopyBufferRegion(readback.Get(), 0, static_cast<ID3D12Resource *>(context.Resolve(buffer)), 0, 512);
    });
    auto overwrite = graph.AddPass(1, "overwrite first half", hs::QueueHint::Direct);
    const auto v3 = overwrite.Write(v2, hs::Access::CopyDest, hs::BufferRange{0, 512});
    overwrite.SetExecute([&](hs::RenderPassContext &context) {
        f.commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(buffer)), 0, upload.Get(), 512, 512);
    });
    auto final = graph.AddPass(40, "read complete buffer", hs::QueueHint::Direct);
    final.Read(v3, hs::Access::CopySource);
    final.SetExecute([&](hs::RenderPassContext &context) {
        f.commands->CopyBufferRegion(readback.Get(), 1024, static_cast<ID3D12Resource *>(context.Resolve(buffer)), 0, 1024);
    });
    Check(graph.Prepare(pool).Succeeded(), "prepare actual partial buffer copies");
    const unsigned expected[]{10, 0, 30, 1, 40};
    for (unsigned i = 0; i < 5; ++i) Check(graph.CompiledPassId(i) == expected[i], "partial copy WAR order");
    Check(graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(), "execute actual partial copies");
    f.Submit();
    D3D12_RANGE read_range{0, 2048};
    Hr(readback->Map(0, &read_range, reinterpret_cast<void **>(&bytes)), "map range readback");
    for (unsigned i = 0; i < 512; ++i)
    {
        Check(bytes[i] == static_cast<unsigned char>(i % 251), "old range contents preserved before overwrite");
        const auto expected_byte = static_cast<unsigned char>(97 + (i + 512) % 251);
        Check(bytes[1024 + i] == expected_byte && bytes[1536 + i] == expected_byte,
              "partial versions retain correct final bytes");
    }
    readback->Unmap(0, &no_read);
    pool.ResetClaims(); graph.Reset();
    const auto uav = graph.CreateBuffer({64, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "partial load");
    auto write = graph.AddPass(0, "initialize partial UAV", hs::QueueHint::Direct);
    const auto initialized = write.Write(graph.Version(uav), hs::Access::UnorderedWrite, hs::BufferRange{0, 32});
    write.SetExecute([](auto &) {});
    auto load = graph.AddPass(1, "load partial UAV", hs::QueueHint::Direct);
    (void)load.ReadWrite(initialized, hs::Access::UnorderedWrite, hs::BufferRange{0, 32});
    load.SetExecute([](auto &) {});
    Check(graph.Prepare(pool).Succeeded() && graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(),
          "buffer ReadWrite range overload");
    f.Submit();
}

void TextureRanges(Fixture &f, hs::BarrierMode mode, ID3D12GraphicsCommandList7 *enhanced)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 16; desc.DepthOrArraySize = 2; desc.MipLevels = 3;
    desc.Format = DXGI_FORMAT_R8_UNORM; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> native;
    Hr(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&native)), "mipped array texture");
    hs::TransientResourcePool pool;
    Check(pool.Initialize(f.allocator, f.device.Get()).Succeeded(), "texture range pool");
    hs::RenderGraphBuilder graph;
    auto texture = graph.ImportTexture({native.Get()}, hs::Access::Common, "mipped array");
    const hs::TextureSubresourceRange last{2, 1, 1, 1, 0, 1};
    auto write = graph.AddPass(0, "write last subresource", hs::QueueHint::Direct);
    auto v1 = write.Write(graph.Version(texture), hs::Access::UnorderedWrite, last);
    write.SetExecute([](auto &) {});
    auto load = graph.AddPass(1, "load last subresource", hs::QueueHint::Direct);
    auto v2 = load.ReadWrite(v1, hs::Access::UnorderedWrite, last);
    load.SetExecute([](auto &) {});
    auto read = graph.AddPass(2, "read last subresource", hs::QueueHint::Direct);
    read.Read(v2, hs::Access::ComputeRead, last); read.SetExecute([](auto &) {});
    Check(graph.Prepare(pool).Succeeded() && graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(),
          "native mip array range overloads");
    f.Submit();
    graph.Reset();
    texture = graph.ImportTexture({native.Get()}, hs::Access::Common, "invalid mipped array");
    auto invalid = graph.AddPass(0, "out of array bounds", hs::QueueHint::Direct);
    invalid.Read(graph.Version(texture), hs::Access::ComputeRead, hs::TextureSubresourceRange{0, 1, 2, 1, 0, 1});
    invalid.SetExecute([](auto &) {});
    Check(!graph.Prepare(pool).Succeeded(), "native array extent validated");
    graph.Reset();
    (void)graph.ImportTexture({native.Get()}, hs::Access::Common, "first identity");
    (void)graph.ImportTexture({native.Get()}, hs::Access::Common, "duplicate identity");
    Check(!graph.Prepare(pool).Succeeded(), "same native resource cannot be imported twice");
    // Depth/stencil has two format planes even in a one-mip transient texture.
    graph.Reset();
    const hs::TextureDesc depth_desc{16, 16, DXGI_FORMAT_D24_UNORM_S8_UINT,
                                     D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL};
    texture = graph.CreateTexture(depth_desc, "transient depth stencil");
    const hs::TextureSubresourceRange stencil{0, 1, 0, 1, 1, 1};
    auto stencil_write = graph.AddPass(0, "write stencil plane", hs::QueueHint::Direct);
    const auto stencil_v1 = stencil_write.Write(graph.Version(texture), hs::Access::DepthWrite, stencil);
    stencil_write.SetExecute([](auto &) {});
    auto stencil_read = graph.AddPass(1, "read stencil plane", hs::QueueHint::Direct);
    stencil_read.Read(stencil_v1, hs::Access::DepthRead, stencil); stencil_read.SetExecute([](auto &) {});
    Check(graph.Prepare(pool).Succeeded() && graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(),
          "actual transient format plane count and full-plane barriers");
    f.Submit();
    graph.Reset();
    texture = graph.CreateTexture(depth_desc, "invalid stencil plane");
    auto invalid_plane = graph.AddPass(0, "invalid plane", hs::QueueHint::Direct);
    (void)invalid_plane.Write(graph.Version(texture), hs::Access::DepthWrite,
                             hs::TextureSubresourceRange{0, 1, 0, 1, 2, 1});
    invalid_plane.SetExecute([](auto &) {});
    Check(!graph.Prepare(pool).Succeeded(), "transient plane bounds use actual format");

    // A volume mip contains all its depth slices; they are not array slices.
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.DepthOrArraySize = 4;
    ComPtr<ID3D12Resource> volume;
    Hr(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&volume)), "volume texture");
    graph.Reset();
    texture = graph.ImportTexture({volume.Get()}, hs::Access::Common, "volume");
    auto volume_read = graph.AddPass(0, "read volume mip", hs::QueueHint::Direct);
    volume_read.Read(graph.Version(texture), hs::Access::ComputeRead,
                     hs::TextureSubresourceRange{2, 1, 0, 1, 0, 1});
    volume_read.SetExecute([](auto &) {});
    Check(graph.Prepare(pool).Succeeded() && graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(),
          "volume whole-resource barriers use subresources rather than depth slices");
    f.Submit();
    graph.Reset();
    texture = graph.ImportTexture({volume.Get()}, hs::Access::Common, "invalid volume slice");
    auto volume_slice = graph.AddPass(0, "invalid volume slice", hs::QueueHint::Direct);
    volume_slice.Read(graph.Version(texture), hs::Access::ComputeRead,
                      hs::TextureSubresourceRange{0, 1, 1, 1, 0, 1});
    volume_slice.SetExecute([](auto &) {});
    Check(!graph.Prepare(pool).Succeeded(), "volume depth cannot be declared as an array slice");

}

void ImportedInitialAccess(Fixture &f, hs::BarrierMode mode, ID3D12GraphicsCommandList7 *enhanced)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer, texture, denied_srv;
    Hr(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer)), "initial access buffer");
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 16; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    Hr(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)), "initial access texture");
    desc.Format = DXGI_FORMAT_D32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    Hr(f.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&denied_srv)), "initial access denied SRV");
    hs::TransientResourcePool pool;
    Check(pool.Initialize(f.allocator, f.device.Get()).Succeeded(), "initial access pool");
    bool all_rejected = true;
    const auto reject = [&](ID3D12Resource *native, bool is_buffer, hs::Access access, const char *name) {
        hs::RenderGraphBuilder graph;
        const auto resource = is_buffer ? graph.ImportBuffer({native}, access, name)
                                        : graph.ImportTexture({native}, access, name);
        bool called{};
        auto read = graph.AddPass(0, "valid copy consumer", hs::QueueHint::Direct);
        read.Read(graph.Version(resource), hs::Access::CopySource);
        read.SetExecute([&](auto &) { called = true; });
        const auto result = graph.Prepare(pool);
        if (result.Succeeded())
        {
            // Baseline negative control must never record an invalid native barrier.
            std::fprintf(stderr, "Unexpectedly accepted imported initial access: %s\n", name);
            all_rejected = false;
            return;
        }
        Check(result.Message() == ((!native || access == hs::Access::Undefined)
                  ? "RenderGraph import requires a native resource and known initial access."
                  : "RenderGraph initial access is incompatible with its imported resource."),
              "reject import for its initial access rather than its valid consumer");
        Check(!graph.Resolve(resource), "failed import must not resolve");
        Check(!graph.Execute(f.commands.Get(), enhanced, mode).Succeeded() && !called,
              "failed import must reject execution before callbacks");
    };
    reject(buffer.Get(), true, static_cast<hs::Access>(255), "buffer invalid enum");
    reject(texture.Get(), false, static_cast<hs::Access>(255), "texture invalid enum");
    reject(buffer.Get(), true, hs::Access::RenderTarget, "buffer RenderTarget");
    reject(buffer.Get(), true, hs::Access::Present, "buffer Present");
    reject(texture.Get(), false, hs::Access::VertexBuffer, "texture VertexBuffer");
    reject(texture.Get(), false, hs::Access::IndexBuffer, "texture IndexBuffer");
    reject(texture.Get(), false, hs::Access::VertexShaderRead, "texture VertexShaderRead");
    reject(texture.Get(), false, hs::Access::IndirectArgs, "texture IndirectArgs");
    reject(buffer.Get(), true, hs::Access::UnorderedWrite, "buffer missing UAV flag");
    reject(texture.Get(), false, hs::Access::UnorderedWrite, "texture missing UAV flag");
    reject(texture.Get(), false, hs::Access::RenderTarget, "texture missing RT flag");
    reject(denied_srv.Get(), false, hs::Access::ShaderRead, "denied ShaderRead");
    reject(denied_srv.Get(), false, hs::Access::ComputeRead, "denied ComputeRead");
    reject(buffer.Get(), true, hs::Access::Undefined, "buffer Undefined");
    reject(texture.Get(), false, hs::Access::Undefined, "texture Undefined");
    reject(nullptr, true, hs::Access::Common, "null buffer");
    reject(nullptr, false, hs::Access::Common, "null texture");
    Check(all_rejected, "incompatible imported initial accesses must be rejected");

    hs::RenderGraphBuilder graph;
    const auto common_buffer = graph.ImportBuffer({buffer.Get()}, hs::Access::Common, "valid Common buffer");
    const auto common_texture = graph.ImportTexture({texture.Get()}, hs::Access::Common, "valid Common texture");
    bool called{};
    auto read = graph.AddPass(0, "Common to CopySource", hs::QueueHint::Direct);
    read.Read(graph.Version(common_buffer), hs::Access::CopySource);
    read.Read(graph.Version(common_texture), hs::Access::CopySource);
    read.SetExecute([&](auto &context) {
        called = true;
        Check(context.Resolve(common_buffer) == buffer.Get() && context.Resolve(common_texture) == texture.Get(),
              "valid Common imports resolve native resources");
    });
    Check(graph.Prepare(pool).Succeeded(), "prepare valid Common imports");
    Check(graph.Execute(f.commands.Get(), enhanced, mode).Succeeded() && called, "execute valid Common imports");
    f.Submit();
}

void Run(Fixture &f, hs::BarrierMode mode, ID3D12GraphicsCommandList7 *enhanced)
{
    ImportedInitialAccess(f, mode, enhanced);
    BufferRanges(f, mode, enhanced);
    TextureRanges(f, mode, enhanced);
    hs::TransientResourcePool first,second;
    Check(first.Initialize(f.allocator, f.device.Get()).Succeeded() && second.Initialize(f.allocator, f.device.Get()).Succeeded(),"pool initialize");
    hs::RenderGraphBuilder graph;
    const hs::TextureDesc desc{32,32,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
    auto texture=graph.CreateTexture(desc,"first"); auto other=graph.CreateTexture(desc,"second");
    auto unused=graph.CreateBuffer({256,0},"unused");
    auto storage=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"storage");
    auto write=graph.AddPass(0,"write",hs::QueueHint::Direct);
    auto texture_v1=write.Write(graph.Version(texture),hs::Access::UnorderedWrite);
    (void)write.Write(graph.Version(other),hs::Access::UnorderedWrite);
    (void)write.Write(graph.Version(storage),hs::Access::UnorderedWrite);
    bool called{}; write.SetExecute([&](hs::RenderPassContext &context){called=true;Check(context.Resolve(texture)!=nullptr,"callback resolve");context.UavBarrier(texture);});
    auto read=graph.AddPass(1,"read",hs::QueueHint::Direct); read.Read(texture_v1,hs::Access::ComputeRead); read.SetExecute([](auto &){});
    hs::GraphExecutionStats prepare_cpu;
    prepare_cpu.cpu_plan_us = 123;
    prepare_cpu.cpu_prepare_validation_us = prepare_cpu.cpu_prepare_info_key_us = UINT64_MAX;
    prepare_cpu.cpu_prepare_compile_us = prepare_cpu.cpu_prepare_transient_us = UINT64_MAX;
    const auto prepare_begin = std::chrono::steady_clock::now();
    Check(graph.Prepare(first, &prepare_cpu).Succeeded(),"prepare valid graph with optional CPU attribution");
    const auto prepare_wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - prepare_begin).count();
    const auto attributed_us = prepare_cpu.cpu_prepare_validation_us + prepare_cpu.cpu_prepare_info_key_us +
        prepare_cpu.cpu_prepare_compile_us + prepare_cpu.cpu_prepare_transient_us;
    Check(prepare_cpu.cpu_plan_us == 0 && attributed_us <= static_cast<std::uint64_t>(prepare_wall_us),
          "Prepare clears stale output and reports only nested completed wall intervals");
    Check(!called && graph.CompiledPassId(0) == 0 && graph.CompiledPassId(1) == 1,
          "CPU attribution preserves validation-before-recording and compiled order");
    Check(graph.Resolve(storage)!=nullptr,"buffer allocation");
    auto *native=graph.Resolve(texture); Check(native && native!=graph.Resolve(other) && !graph.Resolve(unused),"distinct claims and unused allocation");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded()&&called,"execute valid graph"); f.Submit();
    Check(graph.FinalAccess(texture)==hs::Access::ComputeRead,"compute final state");
    first.ResetClaims(); graph.Reset();
    auto dependency=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"uav dependency");
    int dependency_callbacks{};
    auto producer=graph.AddPass(0,"uav producer",hs::QueueHint::Direct);
    auto dependency_v1=producer.Write(graph.Version(dependency),hs::Access::UnorderedWrite);
    producer.SetExecute([&](auto &){++dependency_callbacks;});
    auto second_producer=graph.AddPass(1,"uav second producer",hs::QueueHint::Direct);
    auto dependency_v2=second_producer.Write(dependency_v1,hs::Access::UnorderedWrite);
    second_producer.SetExecute([&](auto &){++dependency_callbacks;});
    auto consumer=graph.AddPass(2,"uav consumer",hs::QueueHint::Direct);
    consumer.Read(dependency_v2,hs::Access::UnorderedRead);
    consumer.SetExecute([&](auto &){++dependency_callbacks;});
    Check(graph.Prepare(first).Succeeded()&&graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),
          "consecutive UAV passes");
    f.Submit();
    Check(dependency_callbacks==3,"all consecutive UAV callbacks executed");
    auto reused_buffer=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"buffer reuse");
    auto buffer_write=graph.AddPass(3,"buffer reuse",hs::QueueHint::Direct);auto reused_v1=buffer_write.Write(graph.Version(reused_buffer),hs::Access::UnorderedWrite);buffer_write.SetExecute([](auto &){});
    graph.SetFinalAccess(reused_v1,hs::Access::ComputeRead);
    Check(graph.Prepare(first).Succeeded()&&graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"submitted buffer reuse");f.Submit();
    first.ResetClaims();graph.Reset();
    reused_buffer=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"buffer second reuse");
    auto buffer_again=graph.AddPass(0,"buffer second reuse",hs::QueueHint::Direct);(void)buffer_again.Write(graph.Version(reused_buffer),hs::Access::UnorderedWrite);buffer_again.SetExecute([](auto &){});
    Check(graph.Prepare(first).Succeeded()&&graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"submitted buffer final-state reuse");f.Submit();
    first.ResetClaims(); graph.Reset(); texture=graph.CreateTexture(desc,"reuse");
    auto reuse=graph.AddPass(0,"reuse",hs::QueueHint::Direct);(void)reuse.Write(graph.Version(texture),hs::Access::UnorderedWrite);reuse.SetExecute([](auto &){});
    Check(graph.Prepare(first).Succeeded()&&graph.Resolve(texture)==native,"descriptor keyed reuse");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"reuse previous physical state");f.Submit();
    graph.Reset();texture=graph.CreateTexture(desc,"other frame");auto frame=graph.AddPass(0,"other frame",hs::QueueHint::Direct);
    (void)frame.Write(graph.Version(texture),hs::Access::UnorderedWrite);frame.SetExecute([](auto &){});
    Check(graph.Prepare(second).Succeeded()&&graph.Resolve(texture)!=native,"separate in-flight slots");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"other frame execute");f.Submit();
    first.Clear();second.Clear();
    graph.Reset();texture=graph.CreateTexture(desc,"cleared pool"); auto cleared=graph.AddPass(0,"cleared pool",hs::QueueHint::Direct);
    (void)cleared.Write(graph.Version(texture),hs::Access::UnorderedWrite);cleared.SetExecute([](auto &){});
    Check(graph.Prepare(first).Succeeded(),"clear retains allocator");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"cleared pool execution");f.Submit();
    first.ResetClaims();
    graph.Reset();texture=graph.CreateTexture(desc,"uninitialized");auto invalid=graph.AddPass(0,"invalid",hs::QueueHint::Direct);
    invalid.Read(graph.Version(texture),hs::Access::ComputeRead);invalid.SetExecute([&](auto &){throw std::runtime_error("invalid callback executed");});
    prepare_cpu.cpu_plan_us = 123;
    prepare_cpu.cpu_prepare_validation_us = prepare_cpu.cpu_prepare_info_key_us = UINT64_MAX;
    prepare_cpu.cpu_prepare_compile_us = prepare_cpu.cpu_prepare_transient_us = UINT64_MAX;
    Check(!graph.Prepare(first, &prepare_cpu).Succeeded()&&!graph.Resolve(texture),"reject first read");
    Check(prepare_cpu.cpu_plan_us == 0 && prepare_cpu.cpu_prepare_validation_us == 0 &&
          prepare_cpu.cpu_prepare_info_key_us == 0 && prepare_cpu.cpu_prepare_compile_us == 0 &&
          prepare_cpu.cpu_prepare_transient_us == 0,
          "failed preparation leaves no stale or partially published CPU timing output");
    Check(!graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"execute requires prepared graph");
    graph.Reset();texture=graph.CreateTexture(desc,"uninitialized read write");auto read_write=graph.AddPass(0,"read write",hs::QueueHint::Direct);
    (void)read_write.ReadWrite(graph.Version(texture),hs::Access::UnorderedWrite);read_write.SetExecute([](auto &){});
    Check(!graph.Prepare(first).Succeeded(),"reject first ReadWrite");
    graph.Reset();texture=graph.CreateTexture({0,32,DXGI_FORMAT_R8_UNORM,0},"invalid dimensions");
    Check(!graph.Prepare(first).Succeeded(),"reject invalid descriptor even when unused");
    graph.Reset();texture=graph.CreateTexture(desc,"missing callback");auto missing=graph.AddPass(0,"missing callback",hs::QueueHint::Direct);(void)missing.Write(graph.Version(texture),hs::Access::UnorderedWrite);
    Check(!graph.Prepare(first).Succeeded(),"reject missing callback before allocation");
    graph.Reset();texture=graph.CreateTexture(desc,"compute queue");auto compute_queue=graph.AddPass(20,"compute queue",hs::QueueHint::Compute);
    const auto compute_v1=compute_queue.Write(graph.Version(texture),hs::Access::UnorderedWrite);compute_queue.SetExecute([](auto &){});
    auto direct_consumer=graph.AddPass(30,"direct consumer",hs::QueueHint::Direct);
    direct_consumer.Read(compute_v1,hs::Access::ShaderRead);direct_consumer.SetExecute([](auto &){});
    Check(graph.Prepare(first).Succeeded(),"prepare multi-queue graph before native queue submission is connected");
    Check(graph.PassCount()==2 && graph.QueuePassCount(hs::QueueHint::Compute)==1 &&
          graph.QueuePassCount(hs::QueueHint::Direct)==1 &&
          graph.CompiledQueuePassId(hs::QueueHint::Compute,0)==20 &&
          graph.CompiledQueuePassId(hs::QueueHint::Direct,0)==30,
          "prepared graph exposes queue-local pass schedules");
    Check(graph.CrossQueueDependencyCount()==1,"prepared graph exposes one compute-to-direct dependency");
    const auto cross=graph.CrossQueueDependency(0);
    Check(cross.producer_pass_id==20 && cross.consumer_pass_id==30 &&
          cross.producer_queue==hs::QueueHint::Compute && cross.consumer_queue==hs::QueueHint::Direct,
          "prepared graph exposes stable cross-queue pass IDs");
    Check(!graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),
          "single-list execute rejects prepared multi-queue schedule instead of silently using Direct");

    // A future multi-queue frame may complete Direct while Compute or Copy is
    // still using the same transient allocation. CompletionSet must keep that
    // claim unavailable until every required queue fence has completed.
    hs::TransientResourcePool completion_pool;
    Check(completion_pool.Initialize(f.allocator, f.device.Get()).Succeeded(),
          "completion pool initialize");
    hs::RenderGraphBuilder completion_graph;
    auto completion_texture=completion_graph.CreateTexture(desc,"completion protected");
    auto completion_write=completion_graph.AddPass(0,"completion write",hs::QueueHint::Direct);
    (void)completion_write.Write(completion_graph.Version(completion_texture),hs::Access::UnorderedWrite);
    completion_write.SetExecute([](auto &){});
    Check(completion_graph.Prepare(completion_pool).Succeeded(),"prepare completion protected resource");
    auto *completion_native=completion_graph.Resolve(completion_texture);
    Check(completion_native!=nullptr,"completion protected allocation exists");
    hs::CompletionSet required_completion;
    required_completion.Include(hs::QueueHint::Direct,5);
    required_completion.Include(hs::QueueHint::Compute,7);
    completion_pool.SetClaimCompletion(required_completion);
    completion_graph.Reset();
    hs::CompletionSet partial_completion;
    partial_completion.Include(hs::QueueHint::Direct,5);
    partial_completion.Include(hs::QueueHint::Compute,6);
    completion_pool.ResetCompletedClaims(partial_completion);
    completion_texture=completion_graph.CreateTexture(desc,"completion still busy");
    auto busy_write=completion_graph.AddPass(0,"completion still busy",hs::QueueHint::Direct);
    (void)busy_write.Write(completion_graph.Version(completion_texture),hs::Access::UnorderedWrite);
    busy_write.SetExecute([](auto &){});
    Check(completion_graph.Prepare(completion_pool).Succeeded()&&
          completion_graph.Resolve(completion_texture)!=completion_native,
          "incomplete CompletionSet prevents transient allocation reuse");
    completion_pool.Clear(); completion_graph.Reset();

    Check(completion_pool.Initialize(f.allocator, f.device.Get()).Succeeded(),
          "reinitialize completion pool");
    completion_texture=completion_graph.CreateTexture(desc,"completion reusable");
    auto first_completion_write=completion_graph.AddPass(0,"completion reusable",hs::QueueHint::Direct);
    (void)first_completion_write.Write(completion_graph.Version(completion_texture),hs::Access::UnorderedWrite);
    first_completion_write.SetExecute([](auto &){});
    Check(completion_graph.Prepare(completion_pool).Succeeded(),"prepare reusable completion resource");
    completion_native=completion_graph.Resolve(completion_texture);
    completion_pool.SetClaimCompletion(required_completion);
    completion_graph.Reset();
    hs::CompletionSet full_completion;
    full_completion.Include(hs::QueueHint::Direct,5);
    full_completion.Include(hs::QueueHint::Compute,7);
    completion_pool.ResetCompletedClaims(full_completion);
    completion_texture=completion_graph.CreateTexture(desc,"completion released");
    auto released_write=completion_graph.AddPass(0,"completion released",hs::QueueHint::Direct);
    (void)released_write.Write(completion_graph.Version(completion_texture),hs::Access::UnorderedWrite);
    released_write.SetExecute([](auto &){});
    Check(completion_graph.Prepare(completion_pool).Succeeded()&&
          completion_graph.Resolve(completion_texture)==completion_native,
          "satisfied CompletionSet releases transient allocation for reuse");
    completion_pool.ResetClaims(); completion_graph.Reset(); completion_pool.Clear();
    graph.Reset(); auto buffer=graph.CreateBuffer({256,0},"no UAV flag");auto flags=graph.AddPass(0,"flags",hs::QueueHint::Direct);
    (void)flags.Write(graph.Version(buffer),hs::Access::UnorderedWrite);flags.SetExecute([](auto &){});Check(!graph.Prepare(first).Succeeded(),"reject access flags");

    first.ResetClaims(); graph.Reset();
    buffer=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"reverse registration");
    const auto version0=graph.Version(buffer);
    const hs::ResourceVersion version1{buffer,1}, version2{buffer,2};
    std::vector<std::uint32_t> callback_order;
    auto future_consumer=graph.AddPass(2,"future consumer",hs::QueueHint::Direct);
    future_consumer.Read(version2,hs::Access::UnorderedRead);
    future_consumer.SetExecute([&](auto &){callback_order.push_back(2);});
    auto overwrite=graph.AddPass(1,"overwrite",hs::QueueHint::Direct);
    Check(overwrite.Write(version1,hs::Access::UnorderedWrite).ordinal==2,"successor ordinal two");
    overwrite.SetExecute([&](auto &){callback_order.push_back(1);});
    auto initial=graph.AddPass(0,"initial",hs::QueueHint::Direct);
    Check(initial.Write(version0,hs::Access::UnorderedWrite).ordinal==1,"successor ordinal one");
    initial.SetExecute([&](auto &){callback_order.push_back(0);});
    auto old_reader=graph.AddPass(3,"old reader",hs::QueueHint::Direct);
    old_reader.Read(version1,hs::Access::UnorderedRead);
    old_reader.SetExecute([&](auto &){callback_order.push_back(3);});
    graph.SetFinalAccess(version2,hs::Access::ComputeRead);
    Check(graph.Prepare(first).Succeeded(),"compile reverse registration and WAR");
    const std::vector<std::uint32_t> expected_order{0,3,1,2};
    for (std::uint32_t i=0;i<expected_order.size();++i)
        Check(graph.CompiledPassId(i)==expected_order[i],"compiled dependency order");
    Check(graph.Execute(f.commands.Get(),enhanced,mode).Succeeded(),"execute compiled dependency order");
    f.Submit(); Check(callback_order==expected_order,"producer RAW and old-version WAR callback order");

    first.ResetClaims(); graph.Reset();
    buffer=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"missing version");
    auto nonexistent=graph.AddPass(0,"missing version",hs::QueueHint::Direct);
    nonexistent.Read({buffer,1},hs::Access::UnorderedRead);
    nonexistent.SetExecute([](auto &){});
    Check(!graph.Prepare(first).Succeeded()&&!graph.Resolve(buffer),"reject nonexistent version before allocation");

    graph.Reset(); buffer=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"branch");
    auto branch_a=graph.AddPass(0,"branch a",hs::QueueHint::Direct);
    (void)branch_a.Write(graph.Version(buffer),hs::Access::UnorderedWrite);
    branch_a.SetExecute([](auto &){});
    auto branch_b=graph.AddPass(1,"branch b",hs::QueueHint::Direct);
    (void)branch_b.Write(graph.Version(buffer),hs::Access::UnorderedWrite);
    branch_b.SetExecute([](auto &){});
    Check(!graph.Prepare(first).Succeeded()&&!graph.Resolve(buffer),"reject branch writes before allocation");

    graph.Reset(); buffer=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"stale final");
    auto step_a=graph.AddPass(0,"step a",hs::QueueHint::Direct);
    auto old=step_a.Write(graph.Version(buffer),hs::Access::UnorderedWrite);
    step_a.SetExecute([](auto &){});
    auto step_b=graph.AddPass(1,"step b",hs::QueueHint::Direct);
    (void)step_b.Write(old,hs::Access::UnorderedWrite);
    step_b.SetExecute([](auto &){});
    graph.SetFinalAccess(old,hs::Access::UnorderedWrite);
    Check(!graph.Prepare(first).Succeeded(),"reject stale final version");

    graph.Reset();
    auto cycle_a=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"cycle a");
    auto cycle_b=graph.CreateBuffer({1024,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},"cycle b");
    auto cycle_first=graph.AddPass(0,"cycle first",hs::QueueHint::Direct);
    cycle_first.Read({cycle_b,1},hs::Access::UnorderedRead);
    (void)cycle_first.Write(graph.Version(cycle_a),hs::Access::UnorderedWrite);
    cycle_first.SetExecute([](auto &){});
    auto cycle_second=graph.AddPass(1,"cycle second",hs::QueueHint::Direct);
    cycle_second.Read({cycle_a,1},hs::Access::UnorderedRead);
    (void)cycle_second.Write(graph.Version(cycle_b),hs::Access::UnorderedWrite);
    cycle_second.SetExecute([](auto &){});
    Check(!graph.Prepare(first).Succeeded()&&!graph.Resolve(cycle_a),"reject dependency cycle before allocation");
}
}
int main(int argc, char **argv)
{
    try
    {
        BatchResourceMembership();
        const bool hardware = argc == 2 && std::string_view(argv[1]) == "--hardware";
        Fixture fixture(hardware); Run(fixture,hs::BarrierMode::Legacy,nullptr);
        std::puts("Legacy range/readback checks passed.");
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{}; ComPtr<ID3D12GraphicsCommandList7> enhanced;
        if(SUCCEEDED(fixture.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&options,sizeof(options)))&&options.EnhancedBarriersSupported&&SUCCEEDED(fixture.commands.As(&enhanced)))
            Run(fixture,hs::BarrierMode::Enhanced,enhanced.Get());
        else if (hardware) throw std::runtime_error("Enhanced barriers unavailable on hardware");
        else std::puts("Enhanced barriers unavailable on WARP; legacy checks completed.");
        std::puts("Render graph transient tests passed."); return 0;
    }
    catch(const std::exception &error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
