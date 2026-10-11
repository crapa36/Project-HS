#include "render_graph.hpp"

#include <D3D12MemAlloc.h>
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <memory>

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
};
}

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

struct Fixture
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    D3D12MA::Allocator *allocator{};
    hs::RenderGraphQueueExecutor executor;

    explicit Fixture(bool hardware)
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
        Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                             IID_PPV_ARGS(&device)),
           "D3D12 device");

        D3D12MA::ALLOCATOR_DESC allocation{};
        allocation.pDevice = device.Get();
        allocation.pAdapter = adapter.Get();
        Hr(D3D12MA::CreateAllocator(&allocation, &allocator), "D3D12MA allocator");
        Check(executor.Initialize(device.Get(), nullptr, 3).Succeeded(),
              "multi-queue executor initialize");
    }

    ~Fixture()
    {
        executor.Clear();
        if (allocator) allocator->Release();
    }

    void CheckValidation()
    {
        ComPtr<ID3D12InfoQueue> info;
        if (FAILED(device.As(&info))) return;
        for (UINT64 index = 0;
             index < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
        {
            SIZE_T size{};
            Hr(info->GetMessage(index, nullptr, &size), "validation message size");
            std::vector<char> bytes(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
            Hr(info->GetMessage(index, message, &size), "validation message");
            Check(message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
                      message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION,
                  message->pDescription);
        }
        info->ClearStoredMessages();
    }

    void Execute(hs::RenderGraphBuilder &graph, std::uint32_t slot, hs::BarrierMode barriers,
                 hs::CompletionSet &completion, const char *message)
    {
        const auto result = executor.Execute(graph, slot, barriers, completion);
        if (!result)
        {
            std::fprintf(stderr, "%s: %s\n", message, std::string(result.Message()).c_str());
            CheckValidation();
        }
        Check(result.Succeeded(), message);
    }
};

D3D12_RESOURCE_DESC BufferDescription(std::uint64_t size)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

template <class T> std::shared_ptr<void> NativeOwner(T *native)
{
    native->AddRef();
    return std::shared_ptr<void>(native, [](void *value) { static_cast<T *>(value)->Release(); });
}

void RunDeviceIdentity(Fixture &fixture, bool hardware)
{
    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "identity pool initialize");
    hs::RenderGraphBuilder graph;
    const auto heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const auto description = BufferDescription(256);
    const auto prepare = [&](ID3D12Resource *resource) {
        graph.Reset();
        const auto imported = graph.ImportBuffer({resource}, hs::Access::Common, "device identity import");
        auto pass = graph.AddPass(1, "identity read", hs::QueueHint::Direct);
        pass.Read(graph.Version(imported), hs::Access::CopySource);
        pass.SetExecute([](hs::RenderPassContext &) {});
        return graph.Prepare(pool);
    };
    ComPtr<ID3D12Resource> resource, resource_alias;
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)), "identity resource");
    ComPtr<ID3D12Pageable> pageable;
    Hr(resource.As(&pageable), "identity resource pageable interface");
    Hr(pageable.As(&resource_alias), "identity resource interface roundtrip");
    Check(prepare(resource_alias.Get()).Succeeded(), "same COM device identity accepts an interface roundtrip");

    if (!hardware) return; // A hardware fixture and WARP provide genuinely distinct device identities.
    ComPtr<IDXGIAdapter> warp;
    ComPtr<ID3D12Device> foreign_device;
    Hr(fixture.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "identity WARP adapter");
    Hr(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&foreign_device)),
        "identity foreign WARP device");
    ComPtr<IUnknown> expected_identity, foreign_identity;
    Hr(fixture.device.As(&expected_identity), "identity fixture IUnknown");
    Hr(foreign_device.As(&foreign_identity), "identity foreign IUnknown");
    Check(expected_identity.Get() != foreign_identity.Get(), "foreign-device fixture must have distinct COM identities");
    ComPtr<ID3D12Resource> foreign_resource;
    Hr(foreign_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&foreign_resource)), "foreign identity resource");
    const auto rejected = prepare(foreign_resource.Get());
    Check(!rejected.Succeeded() &&
        rejected.Message() == "RenderGraph imported resource belongs to a different device.",
        "canonical identity comparison must still reject a genuinely foreign device");
    fixture.CheckValidation();
}

struct ExternalWork
{
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> gate, fence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;

    explicit ExternalWork(ID3D12Device *device)
    {
        D3D12_COMMAND_QUEUE_DESC description{};
        description.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        Hr(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue)), "external Copy queue");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "external gate fence");
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "external completion fence");
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator)),
           "external allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)), "external command list");
        Hr(commands->Close(), "close external command list");
        Hr(queue->Wait(gate.Get(), 1), "delay independent external queue");
    }
};

struct ExternalGateRelease
{
    ComPtr<ID3D12Fence> fence;
    ~ExternalGateRelease() { if (fence) static_cast<void>(fence->Signal(1)); }
};

void RunRemovedFenceSentinel(Fixture &fixture)
{
    hs::RenderGraphQueueExecutor executor;
    Check(executor.Initialize(fixture.device.Get(), nullptr, 1).Succeeded(),
          "sentinel executor initialize");
    auto *fence = executor.NativeFence(hs::QueueHint::Direct);
    Hr(fence->Signal(UINT64_MAX), "inject device-removed fence sentinel");
    struct RestoreFence
    {
        ID3D12Fence *fence;
        ~RestoreFence() { static_cast<void>(fence->Signal(0)); }
    } restore{fence};
    Check(SUCCEEDED(fixture.device->GetDeviceRemovedReason()), "sentinel does not remove the device");
    Check(executor.Completed().direct == 0, "removed fence cannot retire submitted work");
    hs::CompletionSet pending;
    pending.Include(hs::QueueHint::Direct, 1);
    const auto waited = executor.Wait(pending);
    Check(!waited && waited.Message().find(std::to_string(static_cast<std::int64_t>(DXGI_ERROR_DEVICE_REMOVED))) != std::string_view::npos,
          "removed fence wait reports device removal instead of completion");
    Check(!executor.Drain(), "drain cannot prove completion from the removed fence sentinel");
}

void RunPlanReuse(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t slot)
{
    constexpr std::size_t max_capacity = 2048;
    const auto description = BufferDescription(max_capacity);
    const D3D12_RANGE no_read{0, 0};
    std::array<ComPtr<ID3D12Resource>, 2> uploads, outputs;
    for (std::size_t index = 0; index < uploads.size(); ++index)
    {
        auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploads[index])), "plan upload");
        std::byte *bytes{};
        Hr(uploads[index]->Map(0, &no_read, reinterpret_cast<void **>(&bytes)), "map plan upload");
        for (std::size_t byte = 0; byte < max_capacity; ++byte)
            bytes[byte] = static_cast<std::byte>((byte * 37 + index * 53 + 11) & 0xff);
        uploads[index]->Unmap(0, nullptr);
        heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outputs[index])), "plan readback");
    }
    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "plan pool");
    hs::RenderGraphBuilder graph;
    hs::CompletionSet previous_completion;
    std::atomic<unsigned> callbacks{};
    unsigned expected_frame{};
    struct Layout
    {
        std::uint64_t capacity{1024};
        hs::QueueHint queue{hs::QueueHint::Direct};
        bool reversed{};
        std::uint64_t read_size{1024};
    };
    const auto declare = [&](unsigned frame, Layout layout, bool duplicate = false,
                             bool invalid_version = false, bool missing_callback = false) {
        graph.Reset();
        pool.ResetCompletedClaims(fixture.executor.Completed());
        const auto input_index = frame % 2;
        const auto source = graph.ImportBuffer({uploads[input_index].Get(), previous_completion},
            input_index ? hs::Access::AllShaderRead : hs::Access::CopySource, "current plan source");
        (void)graph.ImportBuffer({uploads[duplicate ? input_index : 1 - input_index].Get()},
            hs::Access::CopySource, "second import identity");
        const auto output = graph.ImportBuffer({outputs[input_index].Get(), previous_completion,
            input_index ? hs::QueueHint::Copy : hs::QueueHint::Direct},
            hs::Access::CopyDest, "current plan output");
        const auto first = graph.CreateBuffer({layout.capacity, 0}, "first alias");
        const auto second = graph.CreateBuffer({layout.capacity, 0}, "second alias");
        auto write_first = graph.AddPass(10, "plan first write", hs::QueueHint::Direct);
        write_first.Read(graph.Version(source), hs::Access::CopySource);
        (void)write_first.Write(graph.Version(first), hs::Access::CopyDest);
        write_first.SetExecute([&, frame, source, first, layout](hs::RenderPassContext &context) {
            Check(frame == expected_frame, "reused plan records the current callback");
            ++callbacks;
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(
                static_cast<ID3D12Resource *>(context.Resolve(first)), 0,
                static_cast<ID3D12Resource *>(context.Resolve(source)), 0, layout.capacity);
        });
        auto write_second = graph.AddPass(20, "plan second write", layout.queue);
        write_second.Read(graph.Version(source), hs::Access::CopySource);
        const auto version = write_second.Write(graph.Version(second), hs::Access::CopyDest);
        write_second.SetExecute([&, frame, source, second, layout](hs::RenderPassContext &context) {
            Check(frame == expected_frame, "reused plan records the current alias callback");
            ++callbacks;
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(
                static_cast<ID3D12Resource *>(context.Resolve(second)), 0,
                static_cast<ID3D12Resource *>(context.Resolve(source)), 0, layout.capacity);
        });
        graph.AddOrderingDependency(layout.reversed ? 20 : 10, layout.reversed ? 10 : 20);
        auto read = graph.AddPass(30, "plan active readback", hs::QueueHint::Copy);
        read.Read(invalid_version ? hs::ResourceVersion{second, 2} : version,
            hs::Access::CopySource, hs::BufferRange{0, layout.read_size});
        (void)read.Write(graph.Version(output), hs::Access::CopyDest);
        // Frame-local work changes within fixed declared capacity, like an
        // active rect/dispatch change. It must not rebuild the structural plan.
        const std::uint64_t active_bytes = input_index ? 256 : 512;
        if (!missing_callback)
            read.SetExecute([&, frame, second, output, active_bytes](hs::RenderPassContext &context) {
                Check(frame == expected_frame, "reused plan records current active extent");
                ++callbacks;
                static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(
                    static_cast<ID3D12Resource *>(context.Resolve(output)), 0,
                    static_cast<ID3D12Resource *>(context.Resolve(second)), 0, active_bytes);
            });
        return std::array{source, output, first, second};
    };
    const auto execute = [&](unsigned frame, Layout layout) {
        expected_frame = frame;
        callbacks = 0;
        const auto handles = declare(frame, layout);
        Check(graph.Prepare(pool).Succeeded(), "prepare reusable plan frame");
        const auto required = previous_completion;
        fixture.Execute(graph, slot, barriers, previous_completion, "execute reusable plan frame");
        Check(callbacks == 3, "reused plan records all current passes");
        Check(graph.ResourceCompletion(handles[0]).IsSatisfiedBy(previous_completion) &&
            required.IsSatisfiedBy(graph.ResourceCompletion(handles[0])),
            "reused plan merges current imported completion requirements");
        if (frame < 2)
        {
            const auto stats = fixture.executor.LastExecutionStats();
            Check(stats.alias_activation_count == 1 &&
                stats.transient_backing_bytes < stats.transient_bytes_without_aliasing,
                "reused plan preserves safe transient aliasing");
        }
        Check(fixture.executor.Wait(previous_completion).Succeeded(), "wait reusable plan frame");
        const auto input_index = frame % 2;
        const std::size_t active_bytes = input_index ? 256 : 512;
        std::byte *bytes{};
        const D3D12_RANGE range{0, active_bytes};
        Hr(outputs[input_index]->Map(0, &range, reinterpret_cast<void **>(&bytes)), "map reused plan output");
        for (std::size_t byte = 0; byte < active_bytes; ++byte)
            Check(bytes[byte] == static_cast<std::byte>((byte * 37 + input_index * 53 + 11) & 0xff),
                "reused plan uses current imports and current active work");
        outputs[input_index]->Unmap(0, &no_read);
    };
    Layout layout;
    execute(0, layout);
    execute(1, layout);
    Check(hs::RenderGraphTestAccess::Compilations(graph) == 1 &&
        hs::RenderGraphTestAccess::Reuses(graph) == 1,
        "imports, initial access, completion, and active work reuse one compiled plan");
    layout.capacity = max_capacity;
    execute(2, layout);
    layout.queue = hs::QueueHint::Compute;
    execute(3, layout);
    layout.reversed = true;
    execute(4, layout);
    layout.read_size = 512;
    execute(5, layout);
    Check(hs::RenderGraphTestAccess::Compilations(graph) == 5,
        "capacity, queue, ordering, and range changes recompile the plan");
    (void)declare(6, layout, true);
    Check(!graph.Prepare(pool).Succeeded(), "cache hit still rejects duplicate physical imports");
    (void)declare(6, layout, false, true);
    Check(!graph.Prepare(pool).Succeeded(), "invalid version candidate is not cached");
    (void)declare(6, layout, false, false, true);
    Check(!graph.Prepare(pool).Succeeded(), "cache cannot mask a missing current callback");
    for (const auto [producer, consumer] : std::array{std::pair{10u, 15u}, std::pair{15u, 20u}})
    {
        (void)declare(6, layout);
        graph.AddOrderingDependency(producer, consumer);
        Check(!graph.Prepare(pool).Succeeded(),
            "sparse unknown ordering endpoint is rejected without replacing the cached plan");
    }
    execute(6, layout);
    Check(hs::RenderGraphTestAccess::Compilations(graph) == 5 &&
        hs::RenderGraphTestAccess::Reuses(graph) == 2,
        "failed candidates retain the previous successful plan");

    // Descriptor fields must participate even when allocation size would match.
    const auto texture_frame = [&](hs::TextureDesc desc) {
        graph.Reset();
        pool.ResetCompletedClaims(fixture.executor.Completed());
        const auto texture = graph.CreateTexture(desc, "plan descriptor");
        auto pass = graph.AddPass(40, "plan descriptor write", hs::QueueHint::Direct);
        (void)pass.Write(graph.Version(texture), hs::Access::UnorderedWrite);
        pass.SetExecute([](auto &) {});
        Check(graph.Prepare(pool).Succeeded(), "prepare plan descriptor change");
        pool.ResetClaims();
    };
    texture_frame({16, 16, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS});
    texture_frame({16, 16, DXGI_FORMAT_R16_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS});
    Check(hs::RenderGraphTestAccess::Compilations(graph) == 7,
        "format changes invalidate a plan despite matching allocation classes");

    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = texture_desc.Height = 16;
    texture_desc.DepthOrArraySize = texture_desc.MipLevels = 1;
    texture_desc.Format = DXGI_FORMAT_R8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    const auto heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> native_texture;
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&native_texture)), "wrong-kind plan texture");
    graph.Reset();
    pool.ResetCompletedClaims(fixture.executor.Completed());
    (void)graph.ImportBuffer({native_texture.Get()}, hs::Access::Common, "texture imported as buffer");
    const auto wrong_kind = graph.Prepare(pool);
    Check(!wrong_kind.Succeeded() &&
        wrong_kind.Message() == "RenderGraph imported resource kind disagrees with its native descriptor.",
        "native descriptor snapshot rejects a texture imported as a buffer");
    fixture.CheckValidation();
}

void RunExternal(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t slot)
{
    constexpr std::size_t size = 4096;
    const auto description = BufferDescription(size);
    auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> upload, readback;
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
       D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "external upload");
    std::byte *bytes{};
    const D3D12_RANGE no_read{0, 0};
    Hr(upload->Map(0, &no_read, reinterpret_cast<void **>(&bytes)), "map external upload");
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<std::byte>((index * 13 + 91) & 0xff);
    upload->Unmap(0, nullptr);
    heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "external readback");
    const hs::BufferDesc buffer_desc{size, D3D12_RESOURCE_FLAG_NONE};

    for (const int outcome : {0, 1, 2})
    {
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "external pool");
        hs::RenderGraphBuilder graph;
        auto work = std::make_shared<ExternalWork>(fixture.device.Get());
        std::weak_ptr<ExternalWork> lifetime = work;
        ComPtr<ID3D12Fence> gate = work->gate;
        ExternalGateRelease release_on_failure{gate};
        auto completion_owner = NativeOwner(work->fence.Get());
        const auto source = graph.CreateBuffer(buffer_desc, "external source");
        const auto destination = graph.CreateBuffer(buffer_desc, "external destination");
        auto producer = graph.AddPass(100, "owned external predecessor", hs::QueueHint::Copy);
        const auto source_version = producer.Write(graph.Version(source), hs::Access::CopyDest);
        producer.SetExecute([&](hs::RenderPassContext &context) {
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(
                static_cast<ID3D12Resource *>(context.Resolve(source)), 0, upload.Get(), 0, size);
        });
        auto provider = graph.AddPass(110, "independently owned external Copy", hs::QueueHint::Copy);
        provider.Read(source_version, hs::Access::CopySource);
        const auto destination_version = provider.Write(graph.Version(destination), hs::Access::CopyDest);
        const std::array boundaries{
            hs::ExternalResourceAccess{source, hs::Access::Common, hs::Access::Common},
            hs::ExternalResourceAccess{destination, hs::Access::Common, hs::Access::Common}};
        provider.SetExternalExecute({NativeOwner(work->queue.Get()), hs::QueueHint::Copy, work}, boundaries,
            [work, source, destination, outcome](hs::RenderExternalPassContext &context,
                                                hs::ExternalQueueCompletion &completion) {
                Check(context.NativeCommandQueue() == work->queue.Get(), "external callback native queue");
                Hr(work->allocator->Reset(), "reset external allocator");
                Hr(work->commands->Reset(work->allocator.Get(), nullptr), "reset external commands");
                work->commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(destination)), 0,
                    static_cast<ID3D12Resource *>(context.Resolve(source)), 0, size);
                Hr(work->commands->Close(), "close submitted external commands");
                ID3D12CommandList *lists[]{work->commands.Get()};
                work->queue->ExecuteCommandLists(1, lists);
                Hr(work->queue->Signal(work->fence.Get(), 5000), "external provider completion Signal");
                completion.resource_lease = work;
                if (outcome != 1) completion.native_fence_owner = NativeOwner(work->fence.Get());
                if (outcome == 0) completion.value = 5000;
                return outcome == 1 ? hs::Result::Failure(hs::ErrorCode::TaskFailed, "external-test",
                    "provider failed after submission without returning a fence") : hs::Result::Success();
            });
        auto consumer = graph.AddPass(120, "external continuation", hs::QueueHint::Direct);
        consumer.Read(destination_version, hs::Access::CopySource);
        consumer.SetExecute([&](hs::RenderPassContext &context) {
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(
                readback.Get(), 0, static_cast<ID3D12Resource *>(context.Resolve(destination)), 0, size);
        });
        Check(graph.Prepare(pool).Succeeded(), "prepare external queue contract");
        ComPtr<ID3D12Resource> destination_native = static_cast<ID3D12Resource *>(graph.Resolve(destination));
        auto *source_native = graph.Resolve(source);
        hs::CompletionSet completion;
        const auto result = fixture.executor.Execute(graph, slot, barriers, completion);
        Check(result.Succeeded() == (outcome == 0), "external callback outcome is reported");
        Check(outcome != 2 || result.Message().find("invalid owned fence") != std::string_view::npos,
              "malformed completion is rejected explicitly");
        Check(completion.copy && !completion.IsSatisfiedBy(fixture.executor.Completed()),
              "external bridge remains incomplete behind real independent queue gate");
        Check(graph.ResourceCompletion(destination).copy == completion.copy,
              "resource retirement includes the provider bridge");
        graph.Reset();
        work.reset();
        Check(!lifetime.expired(), "external provider lease survives graph reset and caller release");
        pool.ResetCompletedClaims(fixture.executor.Completed());
        const auto busy = graph.CreateBuffer(buffer_desc, "external allocation still busy");
        auto busy_pass = graph.AddPass(130, "external reuse probe", hs::QueueHint::Direct);
        (void)busy_pass.Write(graph.Version(busy), hs::Access::CopyDest);
        busy_pass.SetExecute([](auto &) {});
        Check(graph.Prepare(pool).Succeeded() && graph.Resolve(busy) != source_native &&
            graph.Resolve(busy) != destination_native.Get(), "delayed provider protects transient allocation reuse");
        Hr(gate->Signal(1), "release independent provider gate");
        Check(fixture.executor.Wait(completion).Succeeded(), "wait external bridge including failure retirement");
        Check(lifetime.expired(), "completed bridge releases provider operation and resource lease");
        pool.ResetCompletedClaims(fixture.executor.Completed());
        graph.Reset();
        const auto reusable = graph.CreateBuffer(buffer_desc, "external allocation reusable");
        auto reuse_pass = graph.AddPass(140, "external complete reuse", hs::QueueHint::Direct);
        (void)reuse_pass.Write(graph.Version(reusable), hs::Access::CopyDest);
        reuse_pass.SetExecute([](auto &) {});
        Check(graph.Prepare(pool).Succeeded() && graph.Resolve(reusable) == source_native,
              "external completion releases the first transient allocation");
        pool.ResetClaims();
        graph.Reset();

        // This foreign value deliberately exceeds every executor-owned serial.
        // It must be waited on the actual provider fence, never treated as Copy.
        const auto imported = graph.ImportBuffer({destination_native.Get(), {}, hs::QueueHint::Copy,
            {completion_owner, 5000, NativeOwner(destination_native.Get())}}, hs::Access::Common,
            "foreign fence import");
        auto import_read = graph.AddPass(150, "external import continuation", hs::QueueHint::Direct);
        import_read.Read(graph.Version(imported), hs::Access::CopySource);
        import_read.SetExecute([&](hs::RenderPassContext &context) {
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(
                readback.Get(), 0, static_cast<ID3D12Resource *>(context.Resolve(imported)), 0, size);
        });
        Check(graph.Prepare(pool).Succeeded(), "prepare actual foreign fence import");
        hs::CompletionSet imported_completion;
        fixture.Execute(graph, slot, barriers, imported_completion, "execute foreign fence import");
        Check(fixture.executor.Wait(imported_completion).Succeeded(), "wait imported external continuation");
        D3D12_RANGE range{0, size};
        Hr(readback->Map(0, &range, reinterpret_cast<void **>(&bytes)), "map external continuation output");
        for (std::size_t index = 0; index < size; ++index)
            Check(bytes[index] == static_cast<std::byte>((index * 13 + 91) & 0xff),
                  "external queue/import continuation output mismatch");
        readback->Unmap(0, &no_read);
        pool.ResetCompletedClaims(fixture.executor.Completed());
    }

    auto work = std::make_shared<ExternalWork>(fixture.device.Get());
    Hr(work->gate->Signal(1), "release invalid declaration provider gate");
    for (const int invalid : {0, 1, 2, 3, 4, 5})
    {
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "invalid external pool");
        hs::RenderGraphBuilder graph;
        const auto output = graph.CreateBuffer(buffer_desc, "invalid external output");
        auto pass = graph.AddPass(160, "invalid external boundary", hs::QueueHint::Copy);
        if (invalid == 3) (void)pass.Write(graph.Version(output), hs::Access::CopyDest, hs::BufferRange{0, size});
        else (void)pass.Write(graph.Version(output), hs::Access::CopyDest);
        std::vector<hs::ExternalResourceAccess> boundaries;
        if (invalid != 0) boundaries.push_back({output, invalid == 1 ? hs::Access::CopyDest : hs::Access::Common,
                                              hs::Access::Common});
        if (invalid == 2) boundaries.push_back(boundaries.front());
        if (invalid == 5) boundaries.front().resource = graph.CreateBuffer(buffer_desc, "undeclared external boundary");
        bool invoked{};
        pass.SetExternalExecute({NativeOwner(work->queue.Get()),
            invalid == 4 ? hs::QueueHint::Direct : hs::QueueHint::Copy, work}, boundaries,
            [&](auto &, auto &) { invoked = true; return hs::Result::Success(); });
        Check(!graph.Prepare(pool) && !invoked, "invalid external boundary fails before callback/allocation");
    }
    fixture.CheckValidation();
}

void RunUavClear(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t frame_slot)
{
    struct Compiler
    {
        HMODULE module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        ~Compiler() { if (module) FreeLibrary(module); }
    } compiler;
    if (!compiler.module)
        throw std::runtime_error("Load System32 d3dcompiler_47.dll failed: " + std::to_string(GetLastError()));
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.module, "D3DCompile"));
    Check(compile != nullptr, "System32 d3dcompiler_47.dll lacks D3DCompile");
    constexpr std::string_view shader = R"(
RWByteAddressBuffer UavSource : register(u0);
ByteAddressBuffer SrvSource : register(t0);
RWByteAddressBuffer Output : register(u1);
[numthreads(64,1,1)] void ReadUav(uint3 p : SV_DispatchThreadID)
{ Output.Store(p.x * 4, UavSource.Load(p.x * 4) ^ (0xa5a55a5au + p.x)); }
[numthreads(64,1,1)] void ReadSrv(uint3 p : SV_DispatchThreadID)
{ Output.Store(p.x * 4, SrvSource.Load(p.x * 4) ^ (0xa5a55a5au + p.x)); }
)";
    std::array<D3D12_ROOT_PARAMETER1, 3> parameters{};
    for (auto &parameter : parameters)
    {
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[2].Descriptor.ShaderRegister = 1;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    root_desc.Desc_1_1.pParameters = parameters.data();
    ComPtr<ID3DBlob> serialized, errors;
    Hr(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, &errors), "serialize clear oracle root");
    ComPtr<ID3D12RootSignature> root;
    Hr(fixture.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&root)), "create clear oracle root");
    std::array<ComPtr<ID3D12PipelineState>, 2> pipelines;
    for (std::size_t entry = 0; entry < pipelines.size(); ++entry)
    {
        ComPtr<ID3DBlob> code, diagnostics;
        const auto result = compile(shader.data(), shader.size(), "UavClearOracle", nullptr, nullptr,
            entry ? "ReadSrv" : "ReadUav", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS |
            D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &diagnostics);
        if (FAILED(result) && diagnostics)
            std::fprintf(stderr, "%.*s\n", static_cast<int>(diagnostics->GetBufferSize()),
                static_cast<const char *>(diagnostics->GetBufferPointer()));
        Hr(result, "compile actual ClearUAV consumer shader");
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = root.Get();
        description.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        Hr(fixture.device->CreateComputePipelineState(&description, IID_PPV_ARGS(&pipelines[entry])),
            "create clear oracle pipeline");
    }
    constexpr UINT elements = 64, bytes = elements * sizeof(UINT), clear_value = 0x13579bdfu;
    for (const auto queue : {hs::QueueHint::Direct, hs::QueueHint::Compute})
    for (const bool interpass : {false, true})
    {
        const auto description = BufferDescription(bytes);
        auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> upload, readback;
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "create clear poison upload");
        heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create clear oracle readback");
        std::array<UINT, elements> poison;
        poison.fill(0xdeadbeefu);
        void *mapped{};
        const D3D12_RANGE no_read{0, 0};
        Hr(upload->Map(0, &no_read, &mapped), "map clear poison upload");
        std::memcpy(mapped, poison.data(), bytes);
        upload->Unmap(0, nullptr);
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "clear oracle pool");
        hs::RenderGraphBuilder graph;
        const hs::BufferDesc uav_desc{bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        const auto source = graph.CreateBuffer(uav_desc, "clear oracle source");
        const auto output = graph.CreateBuffer(uav_desc, "clear oracle output");
        auto seed = graph.AddPass(300, "Clear oracle poison", hs::QueueHint::Copy);
        const auto poisoned = seed.Write(graph.Version(source), hs::Access::CopyDest);
        seed.SetExecute([&](hs::RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            list->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(source)), 0, upload.Get(), 0, bytes);
        });
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 1;
        ComPtr<ID3D12DescriptorHeap> clear_cpu, clear_gpu;
        Hr(fixture.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&clear_cpu)), "clear oracle CPU heap");
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        Hr(fixture.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&clear_gpu)), "clear oracle GPU heap");
        const auto dispatch = [&](hs::RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            list->SetComputeRootSignature(root.Get());
            list->SetPipelineState(pipelines[interpass ? 1 : 0].Get());
            auto *input = static_cast<ID3D12Resource *>(context.Resolve(source));
            if (interpass) list->SetComputeRootShaderResourceView(1, input->GetGPUVirtualAddress());
            else list->SetComputeRootUnorderedAccessView(0, input->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(2,
                static_cast<ID3D12Resource *>(context.Resolve(output))->GetGPUVirtualAddress());
            list->Dispatch(1, 1, 1);
        };
        auto clear = graph.AddPass(301, "Clear oracle UAV clear", queue);
        const auto cleared = clear.Write(poisoned, hs::Access::UnorderedWrite);
        hs::ResourceVersion result;
        if (!interpass) result = clear.Write(graph.Version(output), hs::Access::UnorderedWrite);
        clear.SetExecute([&](hs::RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            ID3D12DescriptorHeap *heaps[]{clear_gpu.Get()};
            list->SetDescriptorHeaps(1, heaps);
            const UINT values[]{clear_value, clear_value, clear_value, clear_value};
            list->ClearUnorderedAccessViewUint(clear_gpu->GetGPUDescriptorHandleForHeapStart(),
                clear_cpu->GetCPUDescriptorHandleForHeapStart(),
                static_cast<ID3D12Resource *>(context.Resolve(source)), values, 0, nullptr);
            if (!interpass) { context.UavBarrier(source); dispatch(context); }
        });
        if (interpass)
        {
            auto consumer = graph.AddPass(302, "Clear oracle SRV consumer", queue);
            consumer.Read(cleared, hs::Access::ComputeRead);
            result = consumer.Write(graph.Version(output), hs::Access::UnorderedWrite);
            consumer.SetExecute(dispatch);
        }
        auto copy = graph.AddPass(303, "Clear oracle readback", hs::QueueHint::Copy);
        copy.Read(result, hs::Access::CopySource);
        copy.SetExecute([&](hs::RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            list->CopyBufferRegion(readback.Get(), 0, static_cast<ID3D12Resource *>(context.Resolve(output)), 0, bytes);
        });
        Check(graph.Prepare(pool).Succeeded(), "prepare clear oracle graph");
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = elements;
        view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        auto *input = static_cast<ID3D12Resource *>(graph.Resolve(source));
        fixture.device->CreateUnorderedAccessView(input, nullptr, &view, clear_cpu->GetCPUDescriptorHandleForHeapStart());
        fixture.device->CreateUnorderedAccessView(input, nullptr, &view, clear_gpu->GetCPUDescriptorHandleForHeapStart());
        hs::CompletionSet completion;
        fixture.Execute(graph, frame_slot, barriers, completion, "execute ClearUAV consumer oracle");
        Check(fixture.executor.Wait(completion).Succeeded(), "wait clear oracle completion");
        const D3D12_RANGE read{0, bytes};
        Hr(readback->Map(0, &read, &mapped), "map clear oracle result");
        const auto *actual = static_cast<const UINT *>(mapped);
        for (UINT index = 0; index < elements; ++index)
            Check(actual[index] == (clear_value ^ (0xa5a55a5au + index)), "ClearUAV actual shader result mismatch");
        readback->Unmap(0, &no_read);
        fixture.CheckValidation();
        pool.ResetCompletedClaims(fixture.executor.Completed());
        std::printf("ClearUAV %s/%s/%s passed (64 shader outputs).\n",
            barriers == hs::BarrierMode::Enhanced ? "Enhanced" : "Legacy",
            queue == hs::QueueHint::Compute ? "Compute" : "Direct", interpass ? "SRV interpass" : "UAV callback");
    }
}

void RunSharedTextureReaders(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t frame_slot)
{
    struct Compiler
    {
        HMODULE module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        ~Compiler() { if (module) FreeLibrary(module); }
    } compiler;
    Check(compiler.module != nullptr, "load shared-reader shader compiler");
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.module, "D3DCompile"));
    Check(compile != nullptr, "find shared-reader shader compiler");
    constexpr std::string_view shader = R"(
Texture2D<uint> Source : register(t0);
RWByteAddressBuffer Output : register(u0);
[numthreads(64,1,1)] void Main(uint3 p : SV_DispatchThreadID)
{ Output.Store(p.x * 4, Source.Load(int3(p.x,0,0))); }
)";
    ComPtr<ID3DBlob> code, diagnostics, serialized;
    const auto compiled = compile(shader.data(), shader.size(), "SharedTextureReaders", nullptr, nullptr,
        "Main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &diagnostics);
    if (FAILED(compiled) && diagnostics)
        std::fprintf(stderr, "%.*s\n", static_cast<int>(diagnostics->GetBufferSize()),
            static_cast<const char *>(diagnostics->GetBufferPointer()));
    Hr(compiled, "compile shared-reader shader");
    D3D12_DESCRIPTOR_RANGE1 range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0,
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE, 0};
    std::array<D3D12_ROOT_PARAMETER1, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    root_desc.Desc_1_1.pParameters = parameters.data();
    Hr(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, &diagnostics), "serialize shared-reader root");
    ComPtr<ID3D12RootSignature> root;
    Hr(fixture.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&root)), "create shared-reader root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;
    Hr(fixture.device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pipeline)), "create shared-reader pipeline");
    constexpr UINT elements = 64, bytes = elements * sizeof(UINT), texture_offset = 1024;
    constexpr UINT original = 0x5a17a55au, replacement = 0x16c4e21bu;
    for (const auto final_access : {hs::Access::ComputeRead, hs::Access::CopySource})
    for (const auto writer_queue : {hs::QueueHint::Direct, hs::QueueHint::Compute})
    for (const bool legacy_bridge : {false, true})
    {
        auto heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC texture_desc{};
        texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture_desc.Width = elements; texture_desc.Height = 1;
        texture_desc.DepthOrArraySize = texture_desc.MipLevels = 1;
        texture_desc.Format = DXGI_FORMAT_R32_UINT;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> texture, readback;
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)), "create shared texture");
        auto output_desc = BufferDescription(bytes);
        output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        std::array<ComPtr<ID3D12Resource>, 3> outputs;
        for (auto &output : outputs)
            Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &output_desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output)), "create shared-reader output");
        heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const auto readback_desc = BufferDescription(texture_offset + bytes);
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create shared-reader readback");
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = 2;
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> gpu_heap, cpu_heap;
        Hr(fixture.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&gpu_heap)), "create shared-reader GPU heap");
        heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; heap_desc.NumDescriptors = 1;
        Hr(fixture.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&cpu_heap)), "create shared-reader CPU heap");
        const auto stride = fixture.device->GetDescriptorHandleIncrementSize(heap_desc.Type);
        auto srv_cpu = gpu_heap->GetCPUDescriptorHandleForHeapStart();
        auto uav_cpu = srv_cpu; uav_cpu.ptr += stride;
        auto srv_gpu = gpu_heap->GetGPUDescriptorHandleForHeapStart();
        auto uav_gpu = srv_gpu; uav_gpu.ptr += stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R32_UINT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        fixture.device->CreateShaderResourceView(texture.Get(), &srv, srv_cpu);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_UINT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        fixture.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, uav_cpu);
        fixture.device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav, cpu_heap->GetCPUDescriptorHandleForHeapStart());
        const auto clear = [&](hs::RenderPassContext &context, UINT value) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            ID3D12DescriptorHeap *heaps[]{gpu_heap.Get()}; commands->SetDescriptorHeaps(1, heaps);
            const UINT values[]{value, value, value, value};
            commands->ClearUnorderedAccessViewUint(uav_gpu, cpu_heap->GetCPUDescriptorHandleForHeapStart(),
                texture.Get(), values, 0, nullptr);
        };
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "initialize shared-reader pool");
        hs::RenderGraphBuilder graph;
        const auto add_reader = [&](UINT id, hs::QueueHint queue, hs::ResourceVersion source,
                                    hs::ResourceHandle output) {
            auto pass = graph.AddPass(id, "Shared texture reader", queue);
            pass.Read(source, hs::Access::ComputeRead);
            const auto written = pass.Write(graph.Version(output), hs::Access::UnorderedWrite);
            pass.SetExecute([&, output](hs::RenderPassContext &context) {
                auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                ID3D12DescriptorHeap *heaps[]{gpu_heap.Get()}; commands->SetDescriptorHeaps(1, heaps);
                commands->SetComputeRootSignature(root.Get()); commands->SetPipelineState(pipeline.Get());
                commands->SetComputeRootDescriptorTable(0, srv_gpu);
                commands->SetComputeRootUnorderedAccessView(1,
                    static_cast<ID3D12Resource *>(context.Resolve(output))->GetGPUVirtualAddress());
                commands->Dispatch(1, 1, 1);
            });
            return written;
        };
        auto source = graph.ImportTexture({texture.Get()}, hs::Access::Common, "shared texture");
        std::array<hs::ResourceHandle, 3> destination;
        for (UINT i = 0; i < 2; ++i)
            destination[i] = graph.ImportBuffer({outputs[i].Get()}, hs::Access::Common, "reader output");
        auto seed = graph.AddPass(10, "Shared texture producer", hs::QueueHint::Direct);
        const auto seeded = seed.Write(graph.Version(source), hs::Access::UnorderedWrite);
        seed.SetExecute([&](auto &context) { clear(context, original); });
        auto entry = graph.AddPass(20, "Shared read layout transition", hs::QueueHint::Direct);
        entry.Read(seeded, hs::Access::ComputeRead);
        if (legacy_bridge)
            entry.SetExecuteOnOwner([source](hs::RenderPassContext &context) {
                const hs::ExternalResourceAccess access{source, hs::Access::ComputeRead, hs::Access::ComputeRead};
                return context.RecordLegacyResourceCommands({&access, 1}, [] { return hs::Result::Success(); });
            });
        else entry.SetExecute([](auto &) {});
        add_reader(30, hs::QueueHint::Direct, seeded, destination[0]);
        add_reader(40, hs::QueueHint::Compute, seeded, destination[1]);
        graph.SetFinalAccess(seeded, final_access);
        Check(graph.Prepare(pool).Succeeded(), "prepare shared-reader fanout");
        hs::CompletionSet first;
        fixture.Execute(graph, frame_slot, barriers, first, "execute shared-reader fanout");
        const auto source_completion = graph.ResourceCompletion(source);
        Check(source_completion.direct && source_completion.compute,
            "shared texture retains both reader queue fences even without a final layout change");
        auto partial = source_completion; --partial.compute;
        Check(!source_completion.IsSatisfiedBy(partial), "one completed reader cannot retire the shared texture");
        std::array<hs::CompletionSet, 2> output_completion;
        std::array<hs::Access, 2> output_access;
        std::array<hs::QueueHint, 2> output_queue;
        for (UINT i = 0; i < 2; ++i)
        {
            output_completion[i] = graph.ResourceCompletion(destination[i]);
            output_access[i] = graph.FinalAccess(destination[i]); output_queue[i] = graph.FinalQueue(destination[i]);
        }
        const auto source_queue = graph.FinalQueue(source);
        graph.Reset();
        // No CPU wait: the next import must join both previous-frame readers.
        source = graph.ImportTexture({texture.Get(), source_completion, source_queue}, final_access, "shared reimport");
        for (UINT i = 0; i < 2; ++i)
            destination[i] = graph.ImportBuffer({outputs[i].Get(), output_completion[i], output_queue[i]},
                output_access[i], "reimported reader output");
        destination[2] = graph.ImportBuffer({outputs[2].Get()}, hs::Access::Common, "replacement reader output");
        std::array<hs::ResourceVersion, 3> written_outputs;
        written_outputs[0] = add_reader(10, hs::QueueHint::Direct, graph.Version(source), destination[0]);
        written_outputs[1] = add_reader(20, hs::QueueHint::Compute, graph.Version(source), destination[1]);
        auto overwrite = graph.AddPass(30, "Overwrite after all texture readers", writer_queue);
        const auto changed = overwrite.Write(graph.Version(source), hs::Access::UnorderedWrite);
        overwrite.SetExecute([&](auto &context) { clear(context, replacement); });
        written_outputs[2] = add_reader(40, writer_queue == hs::QueueHint::Direct ? hs::QueueHint::Compute : hs::QueueHint::Direct,
            changed, destination[2]);
        const auto readback_handle = graph.ImportBuffer({readback.Get()}, hs::Access::CopyDest, "shared-reader readback");
        auto copy = graph.AddPass(50, "Readback after shared-reader overwrite", hs::QueueHint::Direct);
        copy.Read(changed, hs::Access::CopySource);
        for (const auto output : written_outputs) copy.Read(output, hs::Access::CopySource);
        (void)copy.Write(graph.Version(readback_handle), hs::Access::CopyDest);
        copy.SetExecute([&](hs::RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            for (UINT i = 0; i < outputs.size(); ++i)
                commands->CopyBufferRegion(readback.Get(), UINT64(i) * bytes, outputs[i].Get(), 0, bytes);
            D3D12_TEXTURE_COPY_LOCATION target{}; target.pResource = readback.Get();
            target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            target.PlacedFootprint = {texture_offset, {DXGI_FORMAT_R32_UINT, elements, 1, 1, bytes}};
            D3D12_TEXTURE_COPY_LOCATION input{}; input.pResource = texture.Get();
            input.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            commands->CopyTextureRegion(&target, 0, 0, 0, &input, nullptr);
        });
        Check(graph.Prepare(pool).Succeeded(), "prepare reimport/reader fanout/overwrite");
        hs::CompletionSet second;
        fixture.Execute(graph, 1u - frame_slot, barriers, second, "execute reimport/reader fanout/overwrite");
        second.Merge(first);
        Check(fixture.executor.Wait(second).Succeeded(), "wait every shared-reader frame");
        void *mapped{}; const D3D12_RANGE read{0, texture_offset + bytes}, no_read{0, 0};
        Hr(readback->Map(0, &read, &mapped), "map shared-reader results");
        const auto *actual = static_cast<const UINT *>(mapped);
        for (UINT i = 0; i < elements; ++i)
            Check(actual[i] == original && actual[elements + i] == original &&
                actual[2 * elements + i] == replacement && actual[texture_offset / sizeof(UINT) + i] == replacement,
                "both readers precede overwrite and next frame observes the same imported texture");
        readback->Unmap(0, &no_read);
        fixture.CheckValidation();
    }
}

void Run(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t frame_slot)
{
    constexpr std::size_t kSize = 4096;
    const auto description = BufferDescription(kSize);

    ComPtr<ID3D12Resource> upload;
    auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    Hr(fixture.device->CreateCommittedResource(
           &heap, D3D12_HEAP_FLAG_NONE, &description,
           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
       "create upload");
    std::byte *upload_bytes{};
    D3D12_RANGE no_read{0, 0};
    Hr(upload->Map(0, &no_read, reinterpret_cast<void **>(&upload_bytes)), "map upload");
    for (std::size_t index = 0; index < kSize; ++index)
        upload_bytes[index] = static_cast<std::byte>((index * 37 + 11) & 0xff);
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12Resource> readback;
    heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    Hr(fixture.device->CreateCommittedResource(
           &heap, D3D12_HEAP_FLAG_NONE, &description,
           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
       "create readback");

    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(),
          "initialize transient pool");
    hs::RenderGraphBuilder graph;
    const hs::BufferDesc buffer_desc{kSize, D3D12_RESOURCE_FLAG_NONE};
    const auto copy_buffer = graph.CreateBuffer(buffer_desc, "copy output");
    const auto compute_buffer = graph.CreateBuffer(buffer_desc, "compute output");

    auto copy = graph.AddPass(10, "Copy producer", hs::QueueHint::Copy);
    const auto copy_v1 =
        copy.Write(graph.Version(copy_buffer), hs::Access::CopyDest);
    copy.SetExecute([&](hs::RenderPassContext &context) {
        auto *commands =
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        commands->CopyBufferRegion(
            static_cast<ID3D12Resource *>(context.Resolve(copy_buffer)), 0,
            upload.Get(), 0, kSize);
    });

    auto compute = graph.AddPass(20, "Compute transfer", hs::QueueHint::Compute);
    compute.Read(copy_v1, hs::Access::CopySource);
    const auto compute_v1 =
        compute.Write(graph.Version(compute_buffer), hs::Access::CopyDest);
    compute.SetExecute([&](hs::RenderPassContext &context) {
        auto *commands =
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        commands->CopyBufferRegion(
            static_cast<ID3D12Resource *>(context.Resolve(compute_buffer)), 0,
            static_cast<ID3D12Resource *>(context.Resolve(copy_buffer)), 0, kSize);
    });

    auto direct = graph.AddPass(30, "Direct readback", hs::QueueHint::Direct);
    direct.Read(compute_v1, hs::Access::CopySource);
    direct.SetExecute([&](hs::RenderPassContext &context) {
        auto *commands =
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        commands->CopyBufferRegion(
            readback.Get(), 0,
            static_cast<ID3D12Resource *>(context.Resolve(compute_buffer)), 0, kSize);
    });

    Check(graph.Prepare(pool).Succeeded(), "prepare Copy -> Compute -> Direct graph");
    Check(graph.QueuePassCount(hs::QueueHint::Copy) == 1 &&
              graph.QueuePassCount(hs::QueueHint::Compute) == 1 &&
              graph.QueuePassCount(hs::QueueHint::Direct) == 1,
          "one pass scheduled on every native queue");
    Check(graph.CrossQueueDependencyCount() == 2,
          "Copy -> Compute -> Direct has two cross-queue dependencies");
    const auto first_dependency = graph.CrossQueueDependency(0);
    const auto second_dependency = graph.CrossQueueDependency(1);
    Check(first_dependency.producer_pass_id == 10 &&
              first_dependency.consumer_pass_id == 20 &&
              first_dependency.producer_queue == hs::QueueHint::Copy &&
              first_dependency.consumer_queue == hs::QueueHint::Compute,
          "Copy -> Compute dependency");
    Check(second_dependency.producer_pass_id == 20 &&
              second_dependency.consumer_pass_id == 30 &&
              second_dependency.producer_queue == hs::QueueHint::Compute &&
              second_dependency.consumer_queue == hs::QueueHint::Direct,
          "Compute -> Direct dependency");

    auto *first_native = graph.Resolve(copy_buffer);
    auto *second_native = graph.Resolve(compute_buffer);
    Check(first_native && second_native && first_native != second_native,
          "multi-queue transient buffers allocated");

    hs::CompletionSet submitted;
    fixture.Execute(graph, frame_slot, barriers, submitted, "execute native multi-queue graph");
    Check(submitted.copy != 0 && submitted.compute != 0 && submitted.direct != 0,
          "native queue submission returns all fence timelines");

    // Execute attaches the real submitted CompletionSet to the claims before
    // queue submission. A partial completion derived from those actual serials
    // must not retire either allocation.
    auto partial = submitted;
    --partial.compute;
    pool.ResetCompletedClaims(partial);
    graph.Reset();
    const auto busy = graph.CreateBuffer(buffer_desc, "busy while queues are in flight");
    auto busy_write = graph.AddPass(40, "busy allocation", hs::QueueHint::Direct);
    (void)busy_write.Write(graph.Version(busy), hs::Access::CopyDest);
    busy_write.SetExecute([](auto &) {});
    Check(graph.Prepare(pool).Succeeded(), "prepare while submitted claims remain");
    auto *busy_native = graph.Resolve(busy);
    Check(busy_native != first_native && busy_native != second_native,
          "submitted CompletionSet keeps transient claims unavailable");

    Check(fixture.executor.Wait(submitted).Succeeded(), "wait submitted multi-queue work");
    const auto completed = fixture.executor.Completed();
    Check(submitted.IsSatisfiedBy(completed),
          "completed native fence values satisfy submitted CompletionSet");

    std::byte *readback_bytes{};
    D3D12_RANGE read_range{0, kSize};
    Hr(readback->Map(0, &read_range, reinterpret_cast<void **>(&readback_bytes)),
       "map readback");
    for (std::size_t index = 0; index < kSize; ++index)
    {
        const auto expected = static_cast<std::byte>((index * 37 + 11) & 0xff);
        Check(readback_bytes[index] == expected,
              "Copy -> Compute -> Direct data mismatch");
    }
    readback->Unmap(0, &no_read);

    pool.ResetCompletedClaims(completed);
    graph.Reset();
    const auto reusable = graph.CreateBuffer(buffer_desc, "released after all queues complete");
    auto reusable_write =
        graph.AddPass(50, "released allocation", hs::QueueHint::Direct);
    (void)reusable_write.Write(graph.Version(reusable), hs::Access::CopyDest);
    reusable_write.SetExecute([](auto &) {});
    Check(graph.Prepare(pool).Succeeded() && graph.Resolve(reusable) == first_native,
          "actual queue completion releases transient allocation for reuse");
    pool.ResetClaims();

    // Two Direct passes with disjoint lifetimes exercise real placed-resource
    // aliasing. The views must remain distinct even though their backing block
    // is shared, and both barrier modes must activate the second view safely.
    for (const bool timing_boundaries : {false, true})
    {
        graph.Reset();
        const auto alias_first = graph.CreateBuffer(buffer_desc, "alias first");
        const auto alias_second = graph.CreateBuffer({kSize * 2, D3D12_RESOURCE_FLAG_NONE}, "alias second");
        auto alias_pass_first = graph.AddPass(60, "alias first write", hs::QueueHint::Direct);
        (void)alias_pass_first.Write(graph.Version(alias_first), hs::Access::CopyDest);
        alias_pass_first.SetExecute([&](hs::RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(alias_first)),
                                       0, upload.Get(), 0, kSize);
        });
        auto alias_pass_second = graph.AddPass(70, "alias second write", hs::QueueHint::Direct);
        const auto alias_second_v1 = alias_pass_second.Write(graph.Version(alias_second),
                                                           hs::Access::CopyDest);
        alias_pass_second.SetExecute([&](hs::RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(alias_second)),
                                       0, upload.Get(), 0, kSize);
        });
        auto alias_read = graph.AddPass(75, "alias readback", hs::QueueHint::Direct);
        alias_read.Read(alias_second_v1, hs::Access::CopySource, hs::BufferRange{0, kSize});
        alias_read.SetExecute([&](hs::RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            commands->CopyBufferRegion(readback.Get(), 0,
                static_cast<ID3D12Resource *>(context.Resolve(alias_second)), 0, kSize);
        });
        if (timing_boundaries)
        {
            graph.AddPass(37, "critical begin", hs::QueueHint::Direct).SetExecute([](auto &) {});
            graph.AddPass(38, "critical end", hs::QueueHint::Direct).SetExecute([](auto &) {});
            for (const auto id : {60u, 70u, 75u})
            {
                graph.AddOrderingDependency(37, id);
                graph.AddOrderingDependency(id, 38);
            }
        }
        Check(graph.Prepare(pool).Succeeded(), "prepare serial alias graph");
        Check(graph.Resolve(alias_first) && graph.Resolve(alias_second) &&
                  graph.Resolve(alias_first) != graph.Resolve(alias_second),
              "serial aliases expose distinct native views");
        hs::CompletionSet alias_completion;
        fixture.Execute(graph, frame_slot, barriers, alias_completion, "execute serial alias graph");
        const auto alias_stats = fixture.executor.LastExecutionStats();
        const auto expected_batches = (barriers == hs::BarrierMode::Enhanced ? 1u : 3u) +
            (timing_boundaries ? 2u : 0u);
        Check(alias_stats.direct_batches == expected_batches,
              "Enhanced batches Direct lists and preserves explicit timing/Legacy decay boundaries");
        Check(alias_stats.alias_activation_count == 1 &&
                  alias_stats.transient_backing_bytes < alias_stats.transient_bytes_without_aliasing,
              "serial alias graph reports reduced transient backing");
        Check(fixture.executor.Wait(alias_completion).Succeeded(), "wait serial alias graph");
        std::byte *alias_bytes{};
        D3D12_RANGE alias_range{0, kSize};
        Hr(readback->Map(0, &alias_range, reinterpret_cast<void **>(&alias_bytes)), "map alias readback");
        for (std::size_t index = 0; index < kSize; ++index)
            Check(alias_bytes[index] == static_cast<std::byte>((index * 37 + 11) & 0xff),
                  "alias successor preserves uploaded bytes");
        readback->Unmap(0, &no_read);
        pool.ResetCompletedClaims(fixture.executor.Completed());
    }

    // A texture whose first use is on Copy must use COMMON layout on the copy
    // queue. The Direct consumer then exercises the cross-queue acquire in both
    // Legacy and Enhanced modes. Reusing an exported shader-readable view as a
    // Copy-first transient also verifies the previous owning queue is respected.
    for (const bool exported : {true, false})
    {
    graph.Reset();
    const auto copied_texture = graph.CreateTexture(
        {4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE}, "copy texture activation");
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{
        0, {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT}};
    auto texture_copy = graph.AddPass(80, "copy texture", hs::QueueHint::Copy);
    const auto copied_texture_v1 = texture_copy.Write(
        graph.Version(copied_texture), hs::Access::CopyDest);
    texture_copy.SetExecute([&](hs::RenderPassContext &context) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = static_cast<ID3D12Resource *>(context.Resolve(copied_texture));
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    });
    auto texture_read = graph.AddPass(90, "direct texture read", hs::QueueHint::Direct);
    texture_read.Read(copied_texture_v1, hs::Access::CopySource);
    texture_read.SetExecute([&](hs::RenderPassContext &context) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = static_cast<ID3D12Resource *>(context.Resolve(copied_texture));
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    });
    if (exported) graph.SetFinalAccess(copied_texture_v1, hs::Access::ShaderRead);
    Check(graph.Prepare(pool).Succeeded() && graph.Resolve(copied_texture),
          "prepare copy-first texture activation");
    hs::CompletionSet texture_completion;
    fixture.Execute(graph, frame_slot, barriers, texture_completion, "execute copy-first texture activation");
    Check(fixture.executor.Wait(texture_completion).Succeeded(),
          "wait copy-first texture activation");
    std::byte *texture_bytes{};
    D3D12_RANGE texture_range{0, 4 * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
    Hr(readback->Map(0, &texture_range, reinterpret_cast<void **>(&texture_bytes)),
       "map copy-first texture readback");
    for (std::size_t row = 0; row < 4; ++row)
        for (std::size_t column = 0; column < 4 * 4; ++column)
        {
            const auto index = row * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT + column;
            Check(texture_bytes[index] == static_cast<std::byte>((index * 37 + 11) & 0xff),
                  "copy-first texture preserves uploaded bytes");
        }
    readback->Unmap(0, &no_read);
    pool.ResetCompletedClaims(fixture.executor.Completed());
    }
    fixture.CheckValidation();
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const bool hardware =
            argc == 2 && std::string_view(argv[1]) == "--hardware";
        Fixture fixture(hardware);
        RunDeviceIdentity(fixture, hardware);
        RunRemovedFenceSentinel(fixture);
        Run(fixture, hs::BarrierMode::Legacy, 0);
        RunExternal(fixture, hs::BarrierMode::Legacy, 0);
        RunPlanReuse(fixture, hs::BarrierMode::Legacy, 0);
        RunUavClear(fixture, hs::BarrierMode::Legacy, 0);
        RunSharedTextureReaders(fixture, hs::BarrierMode::Legacy, 0);
        std::puts("Legacy Copy -> Compute -> Direct queue chain passed.");

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
        if (SUCCEEDED(fixture.device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) &&
            options.EnhancedBarriersSupported)
        {
            Run(fixture, hs::BarrierMode::Enhanced, 1);
            RunExternal(fixture, hs::BarrierMode::Enhanced, 1);
            RunPlanReuse(fixture, hs::BarrierMode::Enhanced, 1);
            RunUavClear(fixture, hs::BarrierMode::Enhanced, 1);
            RunSharedTextureReaders(fixture, hs::BarrierMode::Enhanced, 1);
            std::puts("Enhanced Copy -> Compute -> Direct queue chain passed.");
        }
        else if (hardware)
        {
            throw std::runtime_error("Enhanced barriers unavailable on hardware");
        }
        else
        {
            std::puts("Enhanced barriers unavailable on WARP; legacy multi-queue checks completed.");
        }

        std::puts("Render graph native multi-queue tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
