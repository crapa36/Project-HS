#include "render_graph.hpp"

#include <D3D12MemAlloc.h>
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <array>
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
        RunRemovedFenceSentinel(fixture);
        Run(fixture, hs::BarrierMode::Legacy, 0);
        RunExternal(fixture, hs::BarrierMode::Legacy, 0);
        RunPlanReuse(fixture, hs::BarrierMode::Legacy, 0);
        std::puts("Legacy Copy -> Compute -> Direct queue chain passed.");

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
        if (SUCCEEDED(fixture.device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) &&
            options.EnhancedBarriersSupported)
        {
            Run(fixture, hs::BarrierMode::Enhanced, 1);
            RunExternal(fixture, hs::BarrierMode::Enhanced, 1);
            RunPlanReuse(fixture, hs::BarrierMode::Enhanced, 1);
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
