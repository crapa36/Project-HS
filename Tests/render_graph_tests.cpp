#include "render_graph.hpp"
#include "render_graph_membership.hpp"
#include "renderer_hlod.hpp"
#include "renderer_post_subgraphs.hpp"
#include "renderer_adaptive_shading.hpp"
#include "renderer_ambient_occlusion.hpp"
#include "renderer_subsurface.hpp"
#include "renderer_reflections.hpp"
#include "renderer_decal_surface.hpp"
#include "renderer_screen_overlay.hpp"
#include <D3D12MemAlloc.h>
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <array>
#include <memory>
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

namespace hs
{
struct RenderGraphTestAccess
{
    static std::uint64_t Compilations(const RenderGraphBuilder &graph)
    {
        return graph.PlanCompilationCount();
    }
    static std::uint64_t Reuses(const RenderGraphBuilder &graph)
    {
        return graph.PlanReuseCount();
    }
    static std::uint64_t InputBuilds(const RenderGraphBuilder &graph)
    {
        return graph.PlanInputBuildCount();
    }
};
}

namespace
{
void Check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }

void EmptyFeatureShapes()
{
    hs::AdaptiveShadingSystem adaptive;
    hs::AdaptiveShadingLayout adaptive_layout;
    Check(!adaptive.MatchesShape(adaptive_layout), "invalid adaptive shape accepted");
    adaptive_layout.valid = true;
    Check(adaptive.MatchesShape(adaptive_layout) && adaptive.MatchesLayout({}, adaptive_layout),
        "inactive adaptive shape disagrees with full binding contract");
    adaptive_layout.active = true;
    Check(!adaptive.MatchesShape(adaptive_layout) && !adaptive.MatchesLayout({}, adaptive_layout),
        "active adaptive shape accepted without native targets");
    hs::AmbientOcclusionSystem ambient;
    hs::AmbientOcclusionLayout ambient_layout;
    ambient_layout.valid = true;
    Check(!ambient.MatchesShape(ambient_layout, false, hs::QueueHint::Direct) &&
        !ambient.MatchesLayout({}, ambient_layout), "AO accepted a shape without targets");
    hs::SubsurfaceSystem subsurface;
    hs::SubsurfaceLayout subsurface_layout;
    subsurface_layout.valid = true;
    Check(!subsurface.MatchesShape(subsurface_layout) && !subsurface.MatchesLayout({}, subsurface_layout),
        "subsurface accepted a shape without targets");
    hs::ScreenReflectionsSystem reflections;
    hs::ScreenReflectionsLayout reflection_layout;
    reflection_layout.valid = true;
    Check(!reflections.MatchesShape(reflection_layout, false, 0, false, 0) &&
        !reflections.MatchesLayout({}, reflection_layout), "reflections accepted a shape without targets");
    hs::DecalSurfaceSystem decals;
    hs::DecalSurfaceLayout decal_layout;
    decal_layout.valid = true;
    Check(decals.MatchesShape(decal_layout, false) && decals.MatchesLayout({}, decal_layout) &&
        !decals.MatchesShape(decal_layout, true), "inactive decal shape disagrees with actual commands");
    decal_layout.active = true;
    Check(!decals.MatchesShape(decal_layout, true), "active decal shape accepted without native targets");
    hs::PostSubgraphLayout post_layout;
    post_layout.pass_count = 1;
    Check(hs::MatchesPostSubgraphShape(false, false, false, false, false, false, false, post_layout) &&
        !hs::MatchesPostSubgraphShape(false, false, false, false, true, false, false, post_layout),
        "post subgraph shape omitted its hook topology");
    hs::ScreenOverlaySystem overlay;
    hs::ScreenOverlayLayout overlay_layout;
    overlay_layout.valid = true;
    Check(overlay.MatchesShape(overlay_layout, false) && !overlay.MatchesShape(overlay_layout, true),
        "inactive screen overlay shape disagrees with the retained layout contract");
}
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

void ResourceAliasPatterns()
{
    std::array<int, 4> natives{};
    std::vector<hs::render_graph::ResourceAlias> scratch;
    const auto groups = [&](const std::array<const void *, 7> &resources) {
        scratch.clear();
        for (std::uint32_t index = 0; index < resources.size(); ++index)
            scratch.push_back({resources[index], index, 0u});
        hs::render_graph::CanonicalizeResourceAliases(scratch);
        std::array<std::uint32_t, 7> result{};
        for (const auto &alias : scratch) result[alias.index] = alias.group;
        return result;
    };
    const std::array<std::uint32_t, 7> expected{0, 2, 3, 2, 5, 3, 0};
    Check(groups({nullptr, &natives[0], &natives[1], &natives[0], &natives[2], &natives[1], nullptr}) == expected,
        "alias groups retain first-use positions and distinguish absent resources");
    const auto capacity = scratch.capacity();
    Check(groups({nullptr, &natives[3], &natives[0], &natives[3], &natives[1], &natives[0], nullptr}) == expected,
        "rotating native addresses with the same equality pattern preserves the frontend variant");
    Check(scratch.capacity() == capacity, "alias grouping reuses caller scratch");
    const auto distinct = groups({nullptr, &natives[3], &natives[0], &natives[3], &natives[1], &natives[2], nullptr});
    Check(distinct != expected && distinct[5] == 6,
        "splitting a native alias changes the retained graph identity");
    Check(groups({}) == std::array<std::uint32_t, 7>{}, "absent resources have no alias group");
    scratch.clear();
    hs::render_graph::CanonicalizeResourceAliases(scratch);
    Check(scratch.empty(), "an empty graph needs no alias storage");
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

void HlodSlotSetTest()
{
    hs::HlodSlotSet slots;
    slots.Resize(130);
    for (const auto slot : {0u, 63u, 64u, 129u})
        Check(slots.Insert(slot), "first valid HLOD member slot must be inserted");
    for (const auto slot : {0u, 63u, 64u, 129u})
        Check(slots.Contains(slot), "inserted HLOD member slot must be found for resident proxy overlap");
    Check(!slots.Insert(64u), "duplicate HLOD member slot must be rejected");
    Check(!slots.Insert(130u) && !slots.Contains(130u), "out-of-capacity HLOD slot must be rejected");
    slots.Clear();
    for (const auto slot : {0u, 63u, 64u, 129u})
        Check(!slots.Contains(slot), "per-registration clear must remove prior member slots");
    Check(slots.Insert(64u), "cleared HLOD slot may be rebound for a later frame");
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

void IdentityFxaaPostSubgraph()
{
    hs::RenderGraphBuilder graph;
    const hs::TextureDesc description{16, 16, DXGI_FORMAT_R16G16B16A16_FLOAT, 0};
    const auto texture = [&](const char *name) { return graph.CreateTexture(description, name); };
    hs::PostSubgraphInputs inputs{};
    inputs.bloom = false;
    inputs.fxaa_identity = true;
    inputs.scene_hdr = graph.Version(texture("identity scene"));
    inputs.composite_hdr = graph.Version(texture("identity composite"));
    inputs.depth = graph.Version(texture("identity depth"));
    inputs.normal = graph.Version(texture("identity normal"));
    inputs.metadata = graph.Version(texture("identity metadata"));
    inputs.motion = graph.Version(texture("identity motion"));
    inputs.surface_history = graph.Version(texture("identity surface history"));
    inputs.temporal_history = graph.Version(texture("identity temporal history"));
    inputs.ui = graph.Version(texture("identity UI"));

    hs::PostSubgraphTargets targets{};
    targets.temporal = graph.Version(texture("identity temporal output"));
    targets.bloom_half = graph.Version(texture("identity bloom half"));
    targets.bloom_quarter = graph.Version(texture("identity bloom quarter"));
    targets.bloom_half_combined = graph.Version(texture("identity bloom combined"));
    targets.post_a = graph.Version(texture("identity post A"));
    targets.post_b = graph.Version(texture("identity post B"));
    targets.back_buffer = graph.Version(texture("identity back buffer"));
    targets.display_linear = graph.Version(texture("identity display linear"));
    const auto pre_bloom_output = graph.Version(texture("identity pre bloom output"));
    const auto pre_tone_output = graph.Version(texture("identity pre tone output"));
    const auto devtools_output = graph.Version(texture("identity devtools output"));
    const auto same_version = [](hs::ResourceVersion left, hs::ResourceVersion right) {
        return left.resource.value == right.resource.value && left.resource.kind == right.resource.kind &&
            left.ordinal == right.ordinal;
    };
    bool pre_bloom_called{}, pre_tone_map_called{}, devtools_called{};
    inputs.pre_bloom = [&](hs::RenderGraphBuilder &, hs::ResourceVersion source, hs::ResourceVersion &output) {
        pre_bloom_called = same_version(source, hs::ResourceVersion{targets.temporal.resource, targets.temporal.ordinal + 1});
        output = pre_bloom_output;
        return hs::Result::Success();
    };
    inputs.pre_tone_map = [&](hs::RenderGraphBuilder &, hs::ResourceVersion source, hs::ResourceVersion &output) {
        pre_tone_map_called = same_version(source, pre_bloom_output);
        output = pre_tone_output;
        return hs::Result::Success();
    };
    inputs.devtools_ui = [&](hs::RenderGraphBuilder &, hs::ResourceVersion destination, hs::ResourceVersion &output) {
        devtools_called = same_version(destination, targets.post_a);
        output = devtools_output;
        return hs::Result::Success();
    };

    hs::PostPassCallbacks callbacks;
    callbacks[0] = [](hs::RenderPassContext &) {};
    callbacks[5] = [](hs::RenderPassContext &) {};
    callbacks[8] = [](hs::RenderPassContext &) {};
    hs::PostSubgraphOutputs outputs;
    Check(hs::BuildPostSubgraphs(graph, inputs, targets, std::move(callbacks), outputs).Succeeded(),
          "build identity FXAA subgraph without the omitted copy callback");
    Check(graph.PassCount() == 3 && graph.DeclaredPassId(0) == 9 &&
              graph.DeclaredPassId(1) == 14 && graph.DeclaredPassId(2) == 17,
          "identity FXAA build omits pass 16 while retaining UI order");
    Check(pre_bloom_called && pre_tone_map_called && devtools_called &&
              same_version(outputs.display_linear,
                  hs::ResourceVersion{targets.display_linear.resource, targets.display_linear.ordinal + 1}) &&
              same_version(outputs.post_a, devtools_output),
          "identity FXAA build preserves current HDR and DevTools hook inputs/outputs");
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

void CachedLogicalLifetimes(Fixture &f, hs::BarrierMode mode, ID3D12GraphicsCommandList7 *enhanced)
{
    hs::TransientResourcePool pool;
    Check(pool.Initialize(f.allocator, f.device.Get()).Succeeded(), "lifetime cache pool initializes");
    hs::RenderGraphBuilder graph;
    std::array<hs::BufferHandle, 3> resources{};
    hs::RenderGraphBuilder::PlanHandle sequential_plan{};
    std::uint64_t disjoint_bytes{};
    const auto check_binding = [&](bool overlap) {
        auto *first = static_cast<ID3D12Resource *>(graph.Resolve(resources[0]));
        auto *last = static_cast<ID3D12Resource *>(graph.Resolve(resources[2]));
        Check(first && last && !graph.Resolve(resources[1]),
            "cached lifetimes must keep unused logical resources unallocated");
        Check(first->GetDesc().Width == 4096 && last->GetDesc().Width == 4096,
            "cached alias descriptors must recreate the declared buffer views");
        const auto allocated = pool.AllocatedBytes();
        if (!disjoint_bytes) disjoint_bytes = allocated;
        Check(disjoint_bytes && allocated == disjoint_bytes * (overlap ? 2u : 1u),
            "cached plans must preserve one backing for disjoint uses and two for overlapping uses");
    };
    for (unsigned cycle = 0; cycle < 2; ++cycle)
        for (unsigned shape = 0; shape < 2; ++shape)
        {
            pool.ResetClaims(); pool.Clear(); graph.Reset();
            for (auto &resource : resources)
                resource = graph.CreateBuffer({4096, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "lifetime resource");
            auto writer = graph.AddPass(0, "lifetime first", hs::QueueHint::Direct);
            const auto first_version = writer.Write(graph.Version(resources[0]), hs::Access::UnorderedWrite);
            writer.SetExecute([](auto &) {});
            auto last = graph.AddPass(1, "lifetime last", hs::QueueHint::Direct);
            if (shape) last.Read(first_version, hs::Access::UnorderedRead);
            (void)last.Write(graph.Version(resources[2]), hs::Access::UnorderedWrite);
            last.SetExecute([](auto &) {});
            graph.AddOrderingDependency(0, 1);
            Check(graph.Prepare(pool).Succeeded(), "cold/warm logical lifetime plan prepares");
            check_binding(shape != 0);
            if (!cycle && !shape) sequential_plan = graph.PreparedPlan();
            Check(graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(), "lifetime graph executes");
            f.Submit();
        }
    Check(hs::RenderGraphTestAccess::Compilations(graph) == 2 &&
        hs::RenderGraphTestAccess::Reuses(graph) == 2,
        "logical lifetime and descriptor facts must follow each cached topology");
    pool.ResetClaims(); pool.Clear(); graph.Reset();
    Check(graph.RestorePlan(sequential_plan), "restore logical lifetime plan from prior cache slot");
    for (std::uint32_t i = 0; i < 2; ++i)
    {
        auto pass = graph.BindPass(i, i);
        Check(pass.has_value(), "restored lifetime pass binds");
        pass->SetExecute([](auto &) {});
    }
    Check(graph.Prepare(pool).Succeeded(), "restored cached lifetimes prepare");
    check_binding(false);
    Check(graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(), "restored lifetime graph executes");
    f.Submit();
}

void Run(Fixture &f, hs::BarrierMode mode, ID3D12GraphicsCommandList7 *enhanced)
{
    IdentityFxaaPostSubgraph();
    ImportedInitialAccess(f, mode, enhanced);
    BufferRanges(f, mode, enhanced);
    TextureRanges(f, mode, enhanced);
    CachedLogicalLifetimes(f, mode, enhanced);
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
    std::weak_ptr<int> reset_callback_owner;
    {
        auto callback_owner = std::make_shared<int>(1);
        reset_callback_owner = callback_owner;
        std::array<hs::TextureHandle, 8> reset_textures{};
        std::array<hs::ResourceVersion, 8> reset_versions{};
        for (auto &resource : reset_textures) resource = graph.CreateTexture(desc, "reset declaration texture");
        auto reset_writer = graph.AddPass(10, "reset declaration writer", hs::QueueHint::Direct);
        for (std::size_t index = 0; index < reset_textures.size(); ++index)
            reset_versions[index] = reset_writer.Write(graph.Version(reset_textures[index]), hs::Access::UnorderedWrite);
        reset_writer.SetExecute([owner = callback_owner](auto &) { (void)owner; });
        auto reset_reader = graph.AddPass(11, "reset declaration reader", hs::QueueHint::Direct);
        for (const auto version : reset_versions) reset_reader.Read(version, hs::Access::ComputeRead);
        reset_reader.SetExecute([](auto &) {});
        auto reset_empty = graph.AddPass(12, "reset declaration empty pass", hs::QueueHint::Direct);
        reset_empty.SetExecute([](auto &) {});
        Check(graph.PassCount() == 3 && graph.Prepare(first).Succeeded(),
              "prepare many per-pass resource uses before reset");
        first.ResetClaims();
        callback_owner.reset();
        graph.Reset();
        Check(reset_callback_owner.expired(), "reset destroys callbacks while retaining use capacity only");
    }
    auto reset_buffer = graph.CreateBuffer({1024, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "reset declaration buffer");
    auto reset_buffer_writer = graph.AddPass(20, "reset declaration buffer writer", hs::QueueHint::Direct);
    const auto reset_buffer_version = reset_buffer_writer.Write(graph.Version(reset_buffer), hs::Access::UnorderedWrite);
    reset_buffer_writer.SetExecute([](auto &) {});
    auto reset_buffer_reader = graph.AddPass(21, "reset declaration buffer reader", hs::QueueHint::Direct);
    reset_buffer_reader.Read(reset_buffer_version, hs::Access::UnorderedRead);
    reset_buffer_reader.SetExecute([](auto &) {});
    Check(graph.PassCount() == 2 && graph.Prepare(first).Succeeded(),
          "reset clears prior uses when declaration count shrinks");
    first.ResetClaims();
    graph.Reset();

    // Four independently optional declaration shapes must survive the bounded
    // exact-plan rotation. The second cycle supplies new imports and callbacks.
    std::array<ComPtr<ID3D12Resource>, 8> plan_imports{};
    D3D12_RESOURCE_DESC plan_buffer_desc{};
    plan_buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    plan_buffer_desc.Width = 1024;
    plan_buffer_desc.Height = 1;
    plan_buffer_desc.DepthOrArraySize = plan_buffer_desc.MipLevels = 1;
    plan_buffer_desc.SampleDesc.Count = 1;
    plan_buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES plan_heap{};
    plan_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (auto &import_resource : plan_imports)
        Hr(f.device->CreateCommittedResource(&plan_heap, D3D12_HEAP_FLAG_NONE, &plan_buffer_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&import_resource)), "plan-cache import");
    ComPtr<IUnknown> plan_device_identity;
    Hr(f.device->QueryInterface(IID_PPV_ARGS(&plan_device_identity)), "plan-cache device identity");
    std::array<ComPtr<IUnknown>, 8> plan_resource_identities;
    std::array<hs::NativeResourceContract, 8> plan_contracts{};
    for (std::size_t index = 0; index < plan_imports.size(); ++index)
    {
        Hr(plan_imports[index]->QueryInterface(IID_PPV_ARGS(&plan_resource_identities[index])),
            "plan-cache resource identity");
        plan_contracts[index].native_resource = plan_imports[index].Get();
        plan_contracts[index].resource_identity = plan_resource_identities[index].Get();
        plan_contracts[index].device_identity = plan_device_identity.Get();
        plan_contracts[index].description = plan_buffer_desc;
        plan_contracts[index].generation = 1;
        plan_contracts[index].backing = hs::NativeBackingKind::Allocation;
    }
    hs::RenderGraphBuilder plan_graph;
    const auto plan_compilations_before = hs::RenderGraphTestAccess::Compilations(plan_graph);
    const auto plan_reuses_before = hs::RenderGraphTestAccess::Reuses(plan_graph);
    std::uint32_t fresh_callback_calls{};
    for (std::uint32_t cycle = 0; cycle < 2; ++cycle)
    {
        for (std::uint32_t shape = 1; shape <= 4; ++shape)
        {
            if (shape != 1 || cycle != 0)
            {
                first.ResetClaims();
                plan_graph.Reset();
            }
            std::array<hs::BufferHandle, 4> handles{};
            const auto import_base = cycle * 4;
            for (std::uint32_t index = 0; index < shape; ++index)
                handles[index] = plan_graph.ImportBuffer(
                    {plan_imports[import_base + index].Get(), {}, hs::QueueHint::Direct,
                     {}, &plan_contracts[import_base + index]}, hs::Access::Common, "rotating plan import");
            auto pass = plan_graph.AddPass(0, "rotating cached declaration", hs::QueueHint::Direct);
            for (std::uint32_t index = 0; index < shape; ++index)
                pass.Read(plan_graph.Version(handles[index]), hs::Access::CopySource);
            pass.SetExecute([&, handles, shape, import_base](hs::RenderPassContext &context) {
                ++fresh_callback_calls;
                for (std::uint32_t index = 0; index < shape; ++index)
                    Check(context.Resolve(handles[index]) == plan_imports[import_base + index].Get(),
                          "cached plan resolves current frame imports");
            });
            const auto input_builds = hs::RenderGraphTestAccess::InputBuilds(plan_graph);
            Check(plan_graph.Prepare(first).Succeeded(), "prepare rotating exact plan shape");
            Check(hs::RenderGraphTestAccess::InputBuilds(plan_graph) == input_builds + (cycle == 0 ? 1u : 0u),
                  "warm topology skips compiler inputs and immutable use validation");
            Check(plan_graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(),
                  "execute rotating exact plan shape");
            f.Submit();
        }
    }
    Check(hs::RenderGraphTestAccess::Compilations(plan_graph) - plan_compilations_before == 4,
          "four declaration shapes compile once");
    Check(hs::RenderGraphTestAccess::Reuses(plan_graph) - plan_reuses_before == 4,
          "second cycle reuses all four retained exact plans");
    Check(fresh_callback_calls == 8, "cached plans invoke each current callback");
    const auto restored_handle = plan_graph.PreparedPlan();
    const auto restored_input_builds = hs::RenderGraphTestAccess::InputBuilds(plan_graph);
    first.ResetClaims();
    Check(plan_graph.RestorePlan(restored_handle), "restore retained plan before declarations");
    Check(!plan_graph.BindPass(0, 99).has_value(), "restored pass requires its original id");
    for (std::uint32_t index = 0; index < 4; ++index)
        Check(plan_graph.BindBuffer({index + 1, hs::ResourceKind::Buffer},
            {plan_imports[index].Get(), {}, hs::QueueHint::Direct, {}, &plan_contracts[index]},
            hs::Access::Common), "bind current import to restored slot");
    auto restored_pass = plan_graph.BindPass(0, 0);
    Check(restored_pass.has_value(), "bind restored pass by declaration index");
    restored_pass->SetExecute([&](hs::RenderPassContext &context) {
        ++fresh_callback_calls;
        Check(context.Resolve({1, hs::ResourceKind::Buffer}) == plan_imports[0].Get(),
              "restored plan resolves current native import");
    });
    Check(plan_graph.Prepare(first).Succeeded() &&
          hs::RenderGraphTestAccess::InputBuilds(plan_graph) == restored_input_builds,
          "restored plan skips resource and pass declarations and compiler inputs");
    Check(plan_graph.Execute(f.commands.Get(), enhanced, mode).Succeeded(), "execute restored plan");
    f.Submit();
    Check(fresh_callback_calls == 9, "restored plan invokes the current callback");
    first.ResetClaims();
    Check(plan_graph.RestorePlan(restored_handle), "restore plan to reject duplicate physical imports");
    for (std::uint32_t index = 0; index < 4; ++index)
        Check(plan_graph.BindBuffer({index + 1, hs::ResourceKind::Buffer},
            {plan_imports[index == 1 ? 0 : index].Get(), {}, hs::QueueHint::Direct,
             {}, &plan_contracts[index == 1 ? 0 : index]}, hs::Access::Common),
            "bind duplicate physical import candidate");
    auto duplicate_pass = plan_graph.BindPass(0, 0);
    Check(duplicate_pass.has_value(), "bind duplicate candidate pass");
    duplicate_pass->SetExecute([](hs::RenderPassContext &) {});
    Check(!plan_graph.Prepare(first).Succeeded(), "restored plan rejects duplicate native identity");
    ComPtr<ID3D12Resource> changed_import;
    auto changed_desc = plan_buffer_desc;
    changed_desc.Width = 2048;
    Hr(f.device->CreateCommittedResource(&plan_heap, D3D12_HEAP_FLAG_NONE, &changed_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&changed_import)), "changed plan import");
    ComPtr<IUnknown> changed_identity;
    Hr(changed_import->QueryInterface(IID_PPV_ARGS(&changed_identity)), "changed import identity");
    auto changed_contract = plan_contracts[0];
    changed_contract.native_resource = changed_import.Get();
    changed_contract.resource_identity = changed_identity.Get();
    changed_contract.description = changed_desc;
    Check(plan_graph.RestorePlan(restored_handle), "failed binding preserves retained plan");
    for (std::uint32_t index = 0; index < 4; ++index)
        Check(plan_graph.BindBuffer({index + 1, hs::ResourceKind::Buffer},
            {index == 0 ? changed_import.Get() : plan_imports[index].Get(), {}, hs::QueueHint::Direct,
             {}, index == 0 ? &changed_contract : &plan_contracts[index]}, hs::Access::Common),
            "bind changed descriptor candidate");
    auto changed_pass = plan_graph.BindPass(0, 0);
    Check(changed_pass.has_value(), "bind changed descriptor pass");
    changed_pass->SetExecute([](hs::RenderPassContext &) {});
    Check(!plan_graph.Prepare(first).Succeeded(), "restored plan rejects changed native descriptor");
    ComPtr<IUnknown> first_identity, second_identity, device_identity;
    Hr(plan_imports[0]->QueryInterface(IID_PPV_ARGS(&first_identity)), "first import identity");
    Hr(plan_imports[1]->QueryInterface(IID_PPV_ARGS(&second_identity)), "second import identity");
    Hr(f.device->QueryInterface(IID_PPV_ARGS(&device_identity)), "plan device identity");
    hs::NativeResourceContract first_contract;
    first_contract.native_resource = plan_imports[0].Get();
    first_contract.resource_identity = first_identity.Get();
    first_contract.device_identity = device_identity.Get();
    first_contract.heap_identity = reinterpret_cast<void *>(std::uintptr_t{1});
    first_contract.description = plan_buffer_desc;
    first_contract.heap_offset = 0;
    first_contract.heap_size = 1024;
    first_contract.generation = 1;
    first_contract.backing = hs::NativeBackingKind::Allocation;
    auto second_contract = first_contract;
    second_contract.native_resource = plan_imports[1].Get();
    second_contract.resource_identity = second_identity.Get();
    struct PlacedCase
    {
        std::uint64_t first_offset, first_size, second_offset, second_size;
        std::uintptr_t second_heap;
        bool accepted;
    };
    constexpr std::array placed_cases{
        PlacedCase{0, 1024, 512, 1024, 1, false},
        PlacedCase{0, 1024, 1024, 1024, 1, true},
        PlacedCase{1024, 1024, 0, 1024, 1, true},
        PlacedCase{0, 1024, 16, 32, 1, false},
        PlacedCase{0, 1024, 0, 1024, 1, false},
        PlacedCase{0, 1024, 0, 1024, 2, true},
        PlacedCase{UINT64_MAX - 1024, 1024, UINT64_MAX - 512, 512, 1, false},
        PlacedCase{0, 1024, UINT64_MAX - 512, 1024, 1, false},
        PlacedCase{0, 1024, 2048, 0, 1, false}};
    for (std::size_t index = 0; index < placed_cases.size(); ++index)
    {
        const auto &test = placed_cases[index];
        first_contract.heap_offset = test.first_offset;
        first_contract.heap_size = test.first_size;
        second_contract.heap_offset = test.second_offset;
        second_contract.heap_size = test.second_size;
        second_contract.heap_identity = reinterpret_cast<void *>(test.second_heap);
        plan_graph.Reset();
        const auto placed_a = plan_graph.ImportBuffer({plan_imports[0].Get(), {}, hs::QueueHint::Direct,
            {}, &first_contract}, hs::Access::Common, "placed A");
        const auto placed_b = plan_graph.ImportBuffer({plan_imports[1].Get(), {}, hs::QueueHint::Direct,
            {}, &second_contract}, hs::Access::Common, "placed B");
        auto placed_reader = plan_graph.AddPass(0, "placed interval candidate", hs::QueueHint::Direct);
        placed_reader.Read(plan_graph.Version(placed_a), hs::Access::CopySource);
        placed_reader.Read(plan_graph.Version(placed_b), hs::Access::CopySource);
        placed_reader.SetExecute([](hs::RenderPassContext &) {});
        const auto prepared = plan_graph.Prepare(first);
        if (prepared.Succeeded() != test.accepted)
            throw std::runtime_error("placed import case " + std::to_string(index) + ": " +
                std::string(prepared.Message()));
    }
    hs::RenderGraphBuilder contract_graph;
    const auto declare_contract = [&](hs::BufferRange range, hs::Access incoming = hs::Access::Common) {
        contract_graph.Reset();
        const auto imported = contract_graph.ImportBuffer({plan_imports[0].Get()}, incoming, "current binding");
        auto reader = contract_graph.AddPass(0, "retained range", hs::QueueHint::Direct);
        reader.Read(contract_graph.Version(imported), hs::Access::CopySource, range);
        auto record_owner = std::make_shared<int>(0);
        std::weak_ptr<int> record_lease = record_owner;
        reader.SetExecute([record_owner](hs::RenderPassContext &) { ++*record_owner; });
        return record_lease;
    };
    auto old_record = declare_contract({0, 64});
    Check(contract_graph.Prepare(first).Succeeded(), "prepare initial retained range");
    (void)declare_contract({64, 64});
    Check(old_record.expired(), "retained topology does not own an old record callback");
    Check(contract_graph.Prepare(first).Succeeded(), "changed use range builds another topology");
    (void)declare_contract({0, 64});
    Check(contract_graph.Prepare(first).Succeeded() && hs::RenderGraphTestAccess::InputBuilds(contract_graph) == 2,
          "returning range reuses its retained compiler inputs");
    (void)declare_contract({plan_buffer_desc.Width, 1});
    Check(!contract_graph.Prepare(first).Succeeded(), "changed out-of-bounds range is still validated");
    const auto failed_input_builds = hs::RenderGraphTestAccess::InputBuilds(contract_graph);
    (void)declare_contract({0, 64}, hs::Access::UnorderedWrite);
    Check(!contract_graph.Prepare(first).Succeeded() &&
          hs::RenderGraphTestAccess::InputBuilds(contract_graph) == failed_input_builds,
          "warm topology rejects incompatible current incoming access before compiler preparation");
    (void)declare_contract({0, 64});
    Check(contract_graph.Prepare(first).Succeeded() &&
          hs::RenderGraphTestAccess::InputBuilds(contract_graph) == failed_input_builds &&
          hs::RenderGraphTestAccess::Compilations(contract_graph) == 2,
          "failed binding and topology candidates leave retained plans reusable");
    first.ResetClaims();

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
        ResourceAliasPatterns();
        HlodSlotSetTest();
        EmptyFeatureShapes();
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
