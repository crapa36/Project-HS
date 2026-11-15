#include <hs/jobs/task_system.hpp>
#include "render_graph.hpp"
#include "render_graph_validation.hpp"
#include <hs/renderer/renderer.hpp>

#include <D3D12MemAlloc.h>
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <memory>

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
        Hr(device.As(&info), "validation message queue");
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

void RunRecordingDispatch(Fixture &fixture, hs::BarrierMode barriers)
{
    for (const auto queue : {hs::QueueHint::Direct, hs::QueueHint::Compute})
    for (const bool timestamps_enabled : {false, true})
    for (const bool query_subset : {false, true})
    {
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "independent pool");
        hs::RenderGraphQueueExecutor executor;
        Check(executor.Initialize(fixture.device.Get(), nullptr, 1).Succeeded(), "independent executor");
        hs::RenderGraphBuilder graph;
        std::array<void *, 6> lists{};
        std::array<std::atomic_uint32_t, 6> calls{};
        ComPtr<ID3D12Resource> source, predicate, output;
        const auto upload_heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const auto output_heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const auto data_desc = BufferDescription(256);
        Hr(fixture.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &data_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&source)), "recording source");
        Hr(fixture.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &data_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&predicate)), "recording predicate");
        Hr(fixture.device->CreateCommittedResource(&output_heap, D3D12_HEAP_FLAG_NONE, &data_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&output)), "recording output");
        const D3D12_RANGE no_read{};
        void *mapped{};
        Hr(source->Map(0, &no_read, &mapped), "map recording source");
        std::memset(mapped, 0, 256);
        for (std::size_t index = 0; index < lists.size(); ++index)
            static_cast<std::uint32_t *>(mapped)[index] = static_cast<std::uint32_t>(17 + index);
        source->Unmap(0, nullptr);
        Hr(predicate->Map(0, &no_read, &mapped), "map recording predicate");
        std::memset(mapped, 0, 256);
        predicate->Unmap(0, nullptr);
        const auto source_buffer = graph.ImportBuffer({source.Get()}, hs::Access::CopySource, "recording source");
        const auto predicate_buffer = graph.ImportBuffer({predicate.Get()}, hs::Access::IndirectArgs, "recording predicate");
        const auto output_buffer = graph.ImportBuffer({output.Get()}, hs::Access::CopyDest, "recording output");
        auto output_version = graph.Version(output_buffer);
        for (std::uint32_t index = 0; index < lists.size(); ++index)
        {
            auto pass = graph.AddPass(index + 1, "independent recording", queue);
            pass.Read(graph.Version(source_buffer), hs::Access::CopySource);
            pass.Read(graph.Version(predicate_buffer), hs::Access::IndirectArgs);
            output_version = pass.Write(output_version, hs::Access::CopyDest);
            pass.SetExecuteChecked([&, index](hs::RenderPassContext &context) {
                    calls[index].fetch_add(1, std::memory_order_relaxed);
                    lists[index] = context.NativeCommandList();
                    auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                    if (index == 0) commands->CopyBufferRegion(output.Get(), 0, source.Get(), 64, (lists.size() + 1) * 4);
                    commands->CopyBufferRegion(output.Get(), index * 4, source.Get(), index * 4, 4);
                    // A later logical pass must not inherit this suppressed-copy state.
                    commands->SetPredication(predicate.Get(), 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                    if (index == 0) commands->CopyBufferRegion(output.Get(), lists.size() * 4, source.Get(), 0, 4);
                    return hs::Result::Success();
                });
        }
        if (auto prepared = graph.Prepare(pool); !prepared)
            throw std::runtime_error("prepare independent graph: " + std::string(prepared.Message()));
        hs::TaskSystem tasks(2);
        const auto query_count = query_subset ? 4u : 12u;
        const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, query_count, 0};
        std::array<ComPtr<ID3D12QueryHeap>, 3> queries;
        std::array<ComPtr<ID3D12Resource>, 3> timestamps;
        const auto heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const auto description = BufferDescription(query_count * sizeof(std::uint64_t));
        const auto queue_index = static_cast<std::size_t>(queue);
        Hr(fixture.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries[queue_index])), "independent timestamp heap");
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestamps[queue_index])), "independent timestamp readback");
        hs::GraphExecutionOptions options;
        if (timestamps_enabled)
        {
            options.timestamp_query_heaps[queue_index] = queries[queue_index].Get();
            options.timestamp_readbacks[queue_index] = timestamps[queue_index].Get();
        }
        std::array<std::uint32_t, 6> query_slots{UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                                 UINT32_MAX, UINT32_MAX, UINT32_MAX};
        if (query_subset)
        {
            query_slots[1] = 0;
            query_slots[4] = 1;
        }
        if (timestamps_enabled && query_subset)
        {
            options.timestamp_query_slots = query_slots.data();
            options.timestamp_query_slot_count = query_slots.size();
        }
        options.task_system = &tasks;
        hs::CompletionSet completion;
        const auto result = executor.Execute(graph, 0, barriers, completion, &options);
        std::vector<void *> distinct_lists;
        for (const auto list : lists)
            if (list && std::find(distinct_lists.begin(), distinct_lists.end(), list) == distinct_lists.end())
                distinct_lists.push_back(list);
        Check(std::all_of(calls.begin(), calls.end(), [](const auto &count) { return count.load() == 1; }),
            "recording workers must execute every callback exactly once");
        Check(executor.LastExecutionStats().record_jobs ==
                std::min(tasks.WorkerCount(), distinct_lists.size() + (timestamps_enabled ? 1u : 0u)),
            "recording job admission is bounded by the available workers");
        Check(barriers == hs::BarrierMode::Enhanced
                ? distinct_lists.size() >= tasks.WorkerCount() && distinct_lists.size() < lists.size()
                : distinct_lists.size() == lists.size(),
            "compatible Enhanced passes share lists without serializing all worker recording; Legacy preserves decay");
        Check(result.Succeeded(), "independent recording succeeds");
        Check(std::all_of(lists.begin(), lists.end(), [](const auto list) { return list != nullptr; }),
            "every logical callback records its work");
        Check(executor.Wait(completion).Succeeded(), "wait independent graph");
        D3D12_RANGE copied_bytes{0, (lists.size() + 1) * 4};
        Hr(output->Map(0, &copied_bytes, &mapped), "map recording output");
        for (std::size_t index = 0; index < lists.size(); ++index)
            Check(static_cast<const std::uint32_t *>(mapped)[index] == 17 + index,
                "shared command lists preserve initial predication and every GPU copy");
        Check(static_cast<const std::uint32_t *>(mapped)[lists.size()] == 0,
            "fixture predication actually suppresses its control copy");
        const D3D12_RANGE copy_no_write{};
        output->Unmap(0, &copy_no_write);
        if (!timestamps_enabled && !query_subset)
            std::printf("Recording %s/%s: %zu callbacks, %zu lists, %u jobs; GPU copies and predication reset passed.\n",
                barriers == hs::BarrierMode::Enhanced ? "Enhanced" : "Legacy",
                queue == hs::QueueHint::Direct ? "Direct" : "Compute", lists.size(), distinct_lists.size(),
                executor.LastExecutionStats().record_jobs);
        if (timestamps_enabled)
        {
            std::uint64_t *values{};
            D3D12_RANGE range{0, query_count * sizeof(std::uint64_t)};
            Hr(timestamps[queue_index]->Map(0, &range, reinterpret_cast<void **>(&values)),
                "map independent timestamps");
            for (std::size_t pair = 0; pair < (query_subset ? 2u : 6u); ++pair)
            {
                const auto index = pair * 2;
                Check(values[index] != 0 && values[index + 1] >= values[index],
                    "independent lists preserve every GPU timestamp pair");
            }
            D3D12_RANGE no_write{};
            timestamps[queue_index]->Unmap(0, &no_write);
        }
    }
    {
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(),
              "recording dispatch pool initialize");
        hs::RenderGraphBuilder graph;
        std::atomic<std::uint32_t> completed{};
        std::uint32_t owner_calls{};
        const auto owner = std::this_thread::get_id();
        for (std::uint32_t index = 0; index < 4; ++index)
        {
            auto pass = graph.AddPass(301 + index, "recording dispatch", hs::QueueHint::Direct);
            if (index % 2 == 0)
                pass.SetExecute([&](hs::RenderPassContext &) {
                    Check(std::this_thread::get_id() != owner, "worker callback ran on owner");
                    completed.fetch_add(1);
                });
            else
                pass.SetExecuteOnOwner([&, index](hs::RenderPassContext &) {
                    Check(std::this_thread::get_id() == owner, "owner callback moved to a worker");
                    Check(index == 1 + owner_calls * 2, "owner callback order changed");
                    ++owner_calls;
                    return hs::Result::Success();
                });
        }
        Check(graph.Prepare(pool).Succeeded(), "prepare recording dispatch graph");
        hs::TaskSystem tasks(2);
        hs::GraphExecutionOptions options;
        options.task_system = &tasks;
        hs::CompletionSet completion;
        const auto result = fixture.executor.Execute(graph, 0, barriers, completion, &options);
        Check(completed == 2, "recording tasks escaped mandatory join");
        Check(owner_calls == 2, "owner callbacks were omitted");
        Check(fixture.executor.LastExecutionStats().record_jobs == 2,
              "recording job count includes rejected or owner work");
        Check(result.Succeeded(), "recording dispatch succeeds");
        Check(fixture.executor.Wait(completion).Succeeded(), "wait recording dispatch graph");
        fixture.CheckValidation();
    }
}

void RunTimestampResolveIsolation(Fixture &fixture)
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
    const bool copy_timestamp_queries_supported =
        SUCCEEDED(fixture.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3,
                                                       sizeof(options3))) &&
        options3.CopyQueueTimestampQueriesSupported != FALSE;
    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "timestamp isolation pool");
    hs::RenderGraphQueueExecutor executor;
    Check(executor.Initialize(fixture.device.Get(), nullptr, 1,
                              copy_timestamp_queries_supported).Succeeded(), "timestamp isolation executor");
    hs::RenderGraphBuilder graph;
    graph.AddPass(1, "Direct timestamp producer", hs::QueueHint::Direct).SetExecute([](hs::RenderPassContext &) {});
    graph.AddPass(2, "Gated compute timestamp producer", hs::QueueHint::Compute).SetExecute([](hs::RenderPassContext &) {});
    graph.AddPass(3, "Direct timestamp suffix", hs::QueueHint::Direct).SetExecute([](hs::RenderPassContext &) {});
    graph.AddPass(4, copy_timestamp_queries_supported ? "Measured copy" : "Unmeasured copy",
                  hs::QueueHint::Copy).SetExecute([](hs::RenderPassContext &) {});
    graph.AddPass(5, "Direct timestamp tail", hs::QueueHint::Direct).SetExecute([](hs::RenderPassContext &) {});
    Check(graph.Prepare(pool).Succeeded(), "timestamp isolation prepare");

    constexpr std::size_t queue_count = 3;
    constexpr std::uint32_t timestamp_base = 4;
    const auto query_count = timestamp_base + 2 * 4 + 2;
    const D3D12_QUERY_HEAP_DESC query_description{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, query_count, 0};
    const auto heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const auto readback_description = BufferDescription(query_count * sizeof(std::uint64_t));
    std::array<ComPtr<ID3D12QueryHeap>, queue_count> queries;
    std::array<ComPtr<ID3D12Resource>, queue_count> timestamps;
    hs::GraphExecutionOptions options;
    options.timestamp_base = timestamp_base;
    const std::array timestamp_queues{hs::QueueHint::Direct, hs::QueueHint::Compute,
                                      hs::QueueHint::Copy};
    for (const auto queue : timestamp_queues)
    {
        const auto index = static_cast<std::size_t>(queue);
        if (queue == hs::QueueHint::Copy && !copy_timestamp_queries_supported) continue;
        auto queue_query_description = query_description;
        if (queue == hs::QueueHint::Copy)
            queue_query_description.Type = D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP;
        Hr(fixture.device->CreateQueryHeap(&queue_query_description, IID_PPV_ARGS(&queries[index])),
           "create isolated timestamp query heap");
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &readback_description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&timestamps[index])), "create isolated timestamp readback");
        options.timestamp_query_heaps[index] = queries[index].Get();
        options.timestamp_readbacks[index] = timestamps[index].Get();
    }
    std::vector<std::uint32_t> query_slots(graph.PassCount(), UINT32_MAX);
    std::vector<std::uint32_t> direct_slots, compute_slots, copy_slots;
    std::uint32_t direct_count{};
    for (std::uint32_t position = 0; position < graph.PassCount(); ++position)
    {
        const auto queue = graph.CompiledPassQueue(position);
        std::uint32_t slot{};
        if (queue == hs::QueueHint::Direct)
        {
            slot = direct_count < 2 ? direct_count + 1 : 4;
            ++direct_count;
            direct_slots.push_back(slot);
        }
        else if (queue == hs::QueueHint::Compute)
        {
            slot = 1;
            compute_slots.push_back(slot);
        }
        else if (queue == hs::QueueHint::Copy)
        {
            slot = 3;
            copy_slots.push_back(slot);
        }
        if (queue != hs::QueueHint::Copy || copy_timestamp_queries_supported)
            query_slots[position] = slot;
    }
    Check(direct_slots.size() == 3 && compute_slots.size() == 1,
          "timestamp isolation fixture keeps its Direct and Compute producers");
    options.timestamp_query_slots = query_slots.data();
    options.timestamp_query_slot_count = query_slots.size();

    ComPtr<ID3D12Fence> compute_gate, copy_gate;
    Hr(fixture.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&compute_gate)),
       "timestamp compute gate");
    Hr(fixture.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_gate)),
       "timestamp copy gate");
    hs::CompletionSet completion;
    struct QueueCompletionGuard
    {
        hs::RenderGraphQueueExecutor &executor;
        hs::CompletionSet &completion;
        ComPtr<ID3D12Fence> compute_gate, copy_gate;
        bool submitted{}, compute_released{}, copy_released{}, completed{};

        void ReleaseGates() noexcept
        {
            if (!compute_released && SUCCEEDED(compute_gate->Signal(1))) compute_released = true;
            if (!copy_released && SUCCEEDED(copy_gate->Signal(1))) copy_released = true;
        }
        ~QueueCompletionGuard()
        {
            ReleaseGates();
            if (submitted && !completed) static_cast<void>(executor.Wait(completion));
        }
    } queue_guard{executor, completion, compute_gate, copy_gate};
    Hr(executor.NativeQueue(hs::QueueHint::Compute)->Wait(compute_gate.Get(), 1),
       "gate independent timestamp compute");
    Hr(executor.NativeQueue(hs::QueueHint::Copy)->Wait(copy_gate.Get(), 1),
       "gate timestamp copy");

    queue_guard.submitted = true;
    Check(executor.Execute(graph, 0, hs::BarrierMode::Legacy, completion, &options).Succeeded(),
          "timestamp isolation execute");
    HANDLE direct_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    struct CloseEvent { HANDLE value; ~CloseEvent() { if (value) CloseHandle(value); } } close_event{direct_event};
    Check(direct_event != nullptr, "create Direct timestamp completion event");
    Hr(executor.NativeFence(hs::QueueHint::Direct)->SetEventOnCompletion(completion.direct, direct_event),
       "watch Direct timestamp resolve");
    const bool direct_resolved_while_compute_waited =
        WaitForSingleObject(direct_event, 10000) == WAIT_OBJECT_0;
    const bool compute_remained_gated = executor.Completed().compute < completion.compute;
    const bool copy_remained_gated = executor.Completed().copy < completion.copy;
    queue_guard.ReleaseGates();
    const auto waited = executor.Wait(completion);
    queue_guard.completed = waited.Succeeded();
    Check(waited.Succeeded(), "wait both timestamp queues before releasing readbacks");

    std::array<std::vector<std::uint64_t>, 3> values;
    for (const auto queue : timestamp_queues)
    {
        const auto index = static_cast<std::size_t>(queue);
        if (queue == hs::QueueHint::Copy && !copy_timestamp_queries_supported) continue;
        std::uint64_t *mapped{};
        D3D12_RANGE range{0, query_count * sizeof(std::uint64_t)};
        Hr(timestamps[index]->Map(0, &range, reinterpret_cast<void **>(&mapped)),
           "map queue timestamp readback");
        const auto &slots = queue == hs::QueueHint::Direct ? direct_slots :
            queue == hs::QueueHint::Compute ? compute_slots : copy_slots;
        for (const auto slot : slots)
            values[index].insert(values[index].end(),
                {mapped[timestamp_base + slot * 2], mapped[timestamp_base + slot * 2 + 1]});
        D3D12_RANGE no_write{};
        timestamps[index]->Unmap(0, &no_write);
    }
    Check(direct_resolved_while_compute_waited && compute_remained_gated && copy_remained_gated,
          "Direct timestamp resolve must not depend on unrelated gated Compute or Copy work");
    for (const auto queue : timestamp_queues)
    {
        if (queue == hs::QueueHint::Copy && !copy_timestamp_queries_supported) continue;
        const auto &queue_values = values[static_cast<std::size_t>(queue)];
        Check(!queue_values.empty(), "each timestamp queue resolves its own producer pairs");
        for (std::size_t pair = 0; pair < queue_values.size(); pair += 2)
            Check(queue_values[pair] != 0 && queue_values[pair + 1] >= queue_values[pair],
                  "queue-local timestamp pairs resolve after producer completion");
    }
    graph.Reset();
    graph.AddPass(1, "Untimed Direct reuse", hs::QueueHint::Direct).SetExecute([](hs::RenderPassContext &) {});
    Check(graph.Prepare(pool).Succeeded(), "prepare smaller untimed graph on the same executor");
    queue_guard.completed = false;
    Check(executor.Execute(graph, 0, hs::BarrierMode::Legacy, completion).Succeeded(), "execute smaller untimed graph");
    const auto reused_wait = executor.Wait(completion);
    queue_guard.completed = reused_wait.Succeeded();
    Check(reused_wait && completion.direct && !completion.compute && !completion.copy &&
              executor.LastExecutionStats().direct_batches == 1,
          "smaller graph must not submit retained timestamp, Compute or Copy batches");
    fixture.CheckValidation();
}

void RunFinalExports(Fixture &fixture, hs::BarrierMode barriers)
{
    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "export pool");
    hs::RenderGraphBuilder graph;
    constexpr std::size_t count = 8;
    std::array<ComPtr<ID3D12Resource>, count> textures;
    std::array<hs::ResourceHandle, count> handles;
    auto description = BufferDescription(256);
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = description.Height = 4;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const auto heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    auto seed = graph.AddPass(1, "establish export source layout", hs::QueueHint::Compute);
    for (std::size_t index = 0; index < count; ++index)
    {
        // A later resource needs a source-queue release, after the first
        // resource has already established the shared destination export.
        const bool release = index % 2 != 0;
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&textures[index])), "export texture");
        if (release)
        {
            handles[index] = graph.ImportTexture({textures[index].Get(), {}, hs::QueueHint::Compute},
                hs::Access::Common, "seed export texture");
            seed.Read(graph.Version(handles[index]), hs::Access::CopySource);
            graph.SetFinalAccess(graph.Version(handles[index]), hs::Access::CopySource);
        }
    }
    seed.SetExecute([](hs::RenderPassContext &) {});
    Check(graph.Prepare(pool).Succeeded(), "prepare export source layout");
    hs::CompletionSet seeded;
    fixture.Execute(graph, 0, barriers, seeded, "establish native source layout");
    Check(fixture.executor.Wait(seeded).Succeeded(), "wait export source layout");
    for (std::size_t index = 1; index < count; index += 2)
        Check(graph.FinalAccess(handles[index]) == hs::Access::CopySource &&
            graph.FinalQueue(handles[index]) == hs::QueueHint::Compute,
            "seed preserves the explicit source layout and owner");
    fixture.CheckValidation();
    // Establish the layout with the same barrier mode: a legacy COPY_SOURCE
    // creation state cannot be handed directly to Enhanced texture barriers.
    graph.Reset();
    for (std::size_t index = 0; index < count; ++index)
    {
        const bool release = index % 2 != 0;
        handles[index] = graph.ImportTexture({textures[index].Get(), release ? seeded : hs::CompletionSet{},
            release ? hs::QueueHint::Compute : hs::QueueHint::Copy},
            release ? hs::Access::CopySource : hs::Access::Common, "unused texture export");
        graph.SetFinalAccess(graph.Version(handles[index]), hs::Access::ShaderRead);
    }
    Check(graph.Prepare(pool).Succeeded(), "prepare independent final exports");
    hs::TaskSystem tasks(2);
    hs::GraphExecutionOptions options;
    options.task_system = &tasks;
    hs::CompletionSet completion;
    Check(fixture.executor.Execute(graph, 0, barriers, completion, &options).Succeeded(),
        "execute grouped final exports");
    const auto stats = fixture.executor.LastExecutionStats();
    Check(stats.record_jobs == 2 && stats.direct_batches == 1 && stats.compute_batches == 1,
        "independent exports share one source-release list and one destination list");
    Check(fixture.executor.Wait(completion).Succeeded(), "wait grouped final exports");
    std::array<hs::CompletionSet, count> prior;
    for (std::size_t index = 0; index < count; ++index)
    {
        Check(graph.FinalAccess(handles[index]) == hs::Access::ShaderRead &&
            graph.FinalQueue(handles[index]) == hs::QueueHint::Direct,
            "every export publishes final access and queue");
        prior[index] = graph.ResourceCompletion(handles[index]);
        Check(prior[index].direct == completion.direct,
            "every export retains the shared destination fence");
    }
    fixture.CheckValidation();

    graph.Reset();
    for (std::size_t index = 0; index < count; ++index)
        handles[index] = graph.ImportTexture({textures[index].Get(), prior[index], hs::QueueHint::Direct},
            hs::Access::ShaderRead, "untouched shader-readable import");
    Check(graph.Prepare(pool).Succeeded(), "prepare untouched imports");
    completion = {};
    Check(fixture.executor.Execute(graph, 0, barriers, completion, &options).Succeeded(),
        "execute untouched imports");
    Check(fixture.executor.LastExecutionStats().record_jobs == 0 &&
        completion.direct == 0 && completion.compute == 0 && completion.copy == 0,
        "unchanged imports require no recording or submission");
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto retained = graph.ResourceCompletion(handles[index]);
        Check(retained.direct == prior[index].direct && retained.compute == prior[index].compute &&
            retained.copy == prior[index].copy &&
            graph.FinalAccess(handles[index]) == hs::Access::ShaderRead &&
            graph.FinalQueue(handles[index]) == hs::QueueHint::Direct,
            "untouched imports preserve prior completion and ownership");
    }
    graph.Reset();
    for (std::size_t index = 0; index < count; ++index)
    {
        handles[index] = graph.ImportTexture({textures[index].Get(), prior[index], hs::QueueHint::Direct},
            hs::Access::ShaderRead, "completed foreign import");
        auto pass = graph.AddPass(static_cast<std::uint32_t>(index + 1), "read completed import", hs::QueueHint::Direct);
        pass.Read(graph.Version(handles[index]), hs::Access::ShaderRead);
        pass.SetExecute([](hs::RenderPassContext &) {});
    }
    Check(graph.Prepare(pool).Succeeded(), "prepare completed foreign imports");
    Check(fixture.executor.Execute(graph, 0, barriers, completion, &options).Succeeded(),
        "execute completed foreign imports");
    Check(fixture.executor.LastExecutionStats().direct_batches ==
            (barriers == hs::BarrierMode::Enhanced ? 1u : count),
        "satisfied foreign completions do not split Enhanced submissions; Legacy preserves decay");
    Check(fixture.executor.Wait(completion).Succeeded(), "wait completed foreign import readers");
    for (std::size_t index = 0; index < count; ++index)
        Check(graph.ResourceCompletion(handles[index]).direct > prior[index].direct &&
            graph.ResourceCompletion(handles[index]).direct <= completion.direct &&
            (barriers != hs::BarrierMode::Enhanced ||
                graph.ResourceCompletion(handles[index]).direct == completion.direct) &&
            graph.ResourceCompletion(handles[index]).compute >= prior[index].compute,
            "merged readers preserve imported and newly accepted completion");
    fixture.CheckValidation();

    graph.Reset();
    const auto leased = graph.ImportTexture({textures[0].Get(), prior[0], hs::QueueHint::Direct,
        {{}, 0, NativeOwner(textures[0].Get())}}, hs::Access::ShaderRead, "unused external lease");
    Check(graph.Prepare(pool).Succeeded(), "prepare untouched external lease");
    Check(fixture.executor.Execute(graph, 0, barriers, completion, &options).Succeeded(),
        "execute untouched external lease");
    Check(fixture.executor.LastExecutionStats().record_jobs == 1 && completion.direct != 0 &&
        graph.ResourceCompletion(leased).direct == completion.direct,
        "external import retains its submission and retirement bridge");
    Check(fixture.executor.Wait(completion).Succeeded(), "wait untouched external lease");
    fixture.CheckValidation();
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
            bytes[byte] = static_cast<std::byte>((byte * 37 + byte / 256 * 19 + index * 53 + 11) & 0xff);
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
        std::uint64_t read_offset{};
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
            hs::Access::CopySource, hs::BufferRange{layout.read_offset, layout.read_size});
        (void)read.Write(graph.Version(output), hs::Access::CopyDest);
        // Frame-local work changes within fixed declared capacity, like an
        // active rect/dispatch change. It must not rebuild the structural plan.
        const std::uint64_t active_bytes = input_index ? 256 : 512;
        if (!missing_callback)
            read.SetExecute([&, frame, second, output, active_bytes, source_offset = layout.read_offset](hs::RenderPassContext &context) {
                Check(frame == expected_frame, "reused plan records current active extent");
                ++callbacks;
                static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(
                    static_cast<ID3D12Resource *>(context.Resolve(output)), 0,
                    static_cast<ID3D12Resource *>(context.Resolve(second)), source_offset, active_bytes);
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
        {
            const auto source_byte = byte + layout.read_offset;
            Check(bytes[byte] == static_cast<std::byte>((source_byte * 37 + source_byte / 256 * 19 + input_index * 53 + 11) & 0xff),
                "reused plan uses current imports and current active work");
        }
        outputs[input_index]->Unmap(0, &no_read);
    };
    Layout layout;
    execute(0, layout);
    execute(1, layout);

    layout.capacity = max_capacity;
    execute(2, layout);
    layout.queue = hs::QueueHint::Compute;
    execute(3, layout);
    layout.reversed = true;
    execute(4, layout);
    layout.read_size = 512;
    execute(5, layout);

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


    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        for (unsigned variant = 0; variant < 3; ++variant)
        {
            auto rotating = layout;
            rotating.read_offset = variant * 512;
            execute(7 + cycle * 3 + variant, rotating);
        }
        if (cycle == 0)
        {
            (void)declare(0, layout, false, true);
            Check(!graph.Prepare(pool).Succeeded(), "failed candidate preserves all three cached plans");
        }
    }


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

    texture_frame({16, 16, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS});
    texture_frame({16, 16, DXGI_FORMAT_R16_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS});


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


struct ExternalGateRelease
{
    ComPtr<ID3D12Fence> fence;
    ~ExternalGateRelease() { if (fence) static_cast<void>(fence->Signal(1)); }
};

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
            [work, source, destination](hs::RenderExternalPassContext &context,
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
                completion.native_fence_owner = NativeOwner(work->fence.Get());
                completion.value = 5000;
                return hs::Result::Success();
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
        std::vector<hs::GpuPassTiming> cpu_passes;
        for (std::uint32_t position = 0; position < graph.PassCount(); ++position)
            cpu_passes.push_back({graph.CompiledPassId(position),
                std::string(graph.CompiledPassName(position)),
                static_cast<hs::RenderQueue>(graph.CompiledPassQueue(position))});
        hs::GraphExecutionOptions capture_options;
        capture_options.cpu_pass_timings = cpu_passes.data();
        capture_options.cpu_pass_timing_count = cpu_passes.size();
        const auto result = fixture.executor.Execute(graph, slot, barriers, completion, &capture_options);
        Check(result.Succeeded(), "external callback succeeds");
        for (const auto &pass : cpu_passes)
        {
            Check(pass.cpu_record_valid, "external graph prefix/ordinary recording interval missing");
            Check(pass.cpu_provider_submit_valid == (pass.pass_id == 110),
                "provider submission interval assigned to wrong pass");
            if (pass.pass_id == 110)
                Check(pass.cpu_provider_submit_end_qpc >= pass.cpu_provider_submit_begin_qpc,
                    "provider callback interval reversed");
        }
        Check(completion.copy && !completion.IsSatisfiedBy(fixture.executor.Completed()),
              "external bridge remains incomplete behind real independent queue gate");
        Check(graph.ResourceCompletion(destination).copy == completion.copy,
              "resource retirement includes the provider bridge");
        Check(graph.FinalAccess(source) == hs::Access::Common &&
            graph.FinalQueue(source) == hs::QueueHint::Copy,
            "provider outgoing state replaces declared source access");
        Check(graph.FinalAccess(destination) == hs::Access::CopySource &&
            graph.FinalQueue(destination) == hs::QueueHint::Direct,
            "provider completion publishes the consumer state");
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
        Check(fixture.executor.Wait(completion).Succeeded(), "wait external provider bridge");
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
    parameters[1].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
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
        // A prior scope leaves a static SRV bound even when the next shader only uses UAVs.
        auto primed = graph.Version(output);
        if (!interpass)
        {
            auto prime = graph.AddPass(299, "Prime static SRV binding", queue);
            prime.Read(poisoned, hs::Access::ComputeRead);
            primed = prime.Write(primed, hs::Access::UnorderedWrite);
            prime.SetExecute([&](hs::RenderPassContext &context) {
                auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                list->SetComputeRootSignature(root.Get());
                list->SetPipelineState(pipelines[1].Get());
                list->SetComputeRootShaderResourceView(1,
                    static_cast<ID3D12Resource *>(context.Resolve(source))->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(2,
                    static_cast<ID3D12Resource *>(context.Resolve(output))->GetGPUVirtualAddress());
                list->Dispatch(1, 1, 1);
            });
        }
        auto clear = graph.AddPass(301, "Clear oracle UAV clear", queue);
        const auto cleared = clear.Write(poisoned, hs::Access::UnorderedWrite);
        hs::ResourceVersion result;
        if (!interpass) result = clear.Write(primed, hs::Access::UnorderedWrite);
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

void RunSameStateRepeatedRead(Fixture &fixture)
{
    constexpr std::size_t size = 512;
    const auto description = BufferDescription(size);
    auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> upload;
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
        "create repeated-read upload");
    std::array<std::byte, size> expected{};
    for (std::size_t index = 0; index < expected.size(); ++index)
        expected[index] = static_cast<std::byte>((index * 29u + 7u) & 0xffu);
    void *mapped{};
    const D3D12_RANGE no_read{0, 0};
    Hr(upload->Map(0, &no_read, &mapped), "map repeated-read upload");
    std::memcpy(mapped, expected.data(), expected.size());
    upload->Unmap(0, nullptr);

    heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    std::array<ComPtr<ID3D12Resource>, 2> readbacks;
    for (auto &readback : readbacks)
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
            "create repeated-read output");

    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(),
        "initialize repeated-read pool");
    hs::RenderGraphBuilder graph;
    std::array<hs::ResourceHandle, 2> sources{
        graph.CreateBuffer({size, D3D12_RESOURCE_FLAG_NONE}, "first repeated-read source"),
        graph.CreateBuffer({size, D3D12_RESOURCE_FLAG_NONE}, "second repeated-read source")};
    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = texture_description.Height = 1;
    texture_description.DepthOrArraySize = texture_description.MipLevels = 1;
    texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_description.SampleDesc.Count = 1;
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    std::array<ComPtr<ID3D12Resource>, 2> textures;
    heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    for (auto &texture : textures)
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &texture_description, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&texture)), "create repeated-read texture");
    std::array<hs::ResourceHandle, 2> sampled_textures{
        graph.ImportTexture({textures[0].Get()}, hs::Access::AllShaderRead, "first repeated-read texture"),
        graph.ImportTexture({textures[1].Get()}, hs::Access::AllShaderRead, "second repeated-read texture")};
    auto seed = graph.AddPass(10, "seed repeated-read source", hs::QueueHint::Copy);
    std::array<hs::ResourceVersion, 2> seeded;
    for (std::size_t index = 0; index < sources.size(); ++index)
        seeded[index] = seed.Write(graph.Version(sources[index]), hs::Access::CopyDest);
    seed.SetExecute([&](hs::RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (const auto source : sources)
            commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(source)),
                0, upload.Get(), 0, size);
    });
    std::array<hs::ResourceHandle, 2> destinations;
    for (std::size_t index = 0; index < readbacks.size(); ++index)
        destinations[index] = graph.ImportBuffer({readbacks[index].Get()},
            hs::Access::CopyDest, index == 0 ? "first repeated-read output" : "second repeated-read output");
    auto read = graph.AddPass(20, "same-state repeated source read", hs::QueueHint::Direct);
    read.Read(seeded[0], hs::Access::CopySource);
    // These distinct same-state texture reads used to split the real buffer
    // transitions into separate enhanced-barrier groups.
    read.Read(graph.Version(sampled_textures[0]), hs::Access::AllShaderRead);
    read.Read(seeded[1], hs::Access::CopySource);
    read.Read(graph.Version(sampled_textures[1]), hs::Access::AllShaderRead);
    for (const auto destination : destinations)
        (void)read.Write(graph.Version(destination), hs::Access::CopyDest);
    read.SetExecute([&](hs::RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (std::size_t index = 0; index < sources.size(); ++index)
            commands->CopyBufferRegion(readbacks[index].Get(), 0,
                static_cast<ID3D12Resource *>(context.Resolve(sources[index])), 0, size);
    });
    const auto prepared = graph.Prepare(pool);
    if (!prepared)
        throw std::runtime_error("prepare same-state repeated-read graph: " + std::string(prepared.Message()));
    hs::CompletionSet completion;
    fixture.Execute(graph, 0, hs::BarrierMode::Enhanced, completion,
        "execute same-state repeated-read graph");
    Check(fixture.executor.Wait(completion).Succeeded(), "wait same-state repeated-read graph");
    for (auto &readback : readbacks)
    {
        D3D12_RANGE read_range{0, size};
        Hr(readback->Map(0, &read_range, &mapped), "map repeated-read output");
        Check(std::memcmp(mapped, expected.data(), expected.size()) == 0,
            "same-state repeated read preserves copied bytes");
        const D3D12_RANGE no_write{0, 0};
        readback->Unmap(0, &no_write);
    }
    fixture.CheckValidation();
    pool.ResetCompletedClaims(fixture.executor.Completed());
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
        std::array<void *, 3> alias_lists{};
        const auto alias_first = graph.CreateBuffer(buffer_desc, "alias first");
        const auto alias_second = graph.CreateBuffer({kSize * 2, D3D12_RESOURCE_FLAG_NONE}, "alias second");
        auto alias_pass_first = graph.AddPass(60, "alias first write", hs::QueueHint::Direct);
        (void)alias_pass_first.Write(graph.Version(alias_first), hs::Access::CopyDest);
        alias_pass_first.SetExecute([&](hs::RenderPassContext &context) {
            alias_lists[0] = context.NativeCommandList();
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(alias_first)),
                                       0, upload.Get(), 0, kSize);
        });
        auto alias_pass_second = graph.AddPass(70, "alias second write", hs::QueueHint::Direct);
        const auto alias_second_v1 = alias_pass_second.Write(graph.Version(alias_second),
                                                           hs::Access::CopyDest);
        alias_pass_second.SetExecute([&](hs::RenderPassContext &context) {
            alias_lists[1] = context.NativeCommandList();
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(alias_second)),
                                       0, upload.Get(), 0, kSize);
        });
        auto alias_read = graph.AddPass(75, "alias readback", hs::QueueHint::Direct);
        alias_read.Read(alias_second_v1, hs::Access::CopySource, hs::BufferRange{0, kSize});
        alias_read.SetExecute([&](hs::RenderPassContext &context) {
            alias_lists[2] = context.NativeCommandList();
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
        Check(graph.FinalAccess(alias_first) == hs::Access::CopyDest &&
            graph.FinalAccess(alias_second) == hs::Access::CopySource &&
            graph.FinalQueue(alias_first) == hs::QueueHint::Direct &&
            graph.FinalQueue(alias_second) == hs::QueueHint::Direct,
            "alias release preserves logical access while physical views return to COMMON");
        Check(alias_lists[0] && alias_lists[1] && alias_lists[2], "alias callbacks recorded");
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
    Check(graph.FinalAccess(copied_texture) == (exported ? hs::Access::ShaderRead : hs::Access::CopySource) &&
        graph.FinalQueue(copied_texture) == hs::QueueHint::Direct,
        "after-pass export overrides the use access and preserves the consumer queue");
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

        RunRecordingDispatch(fixture, hs::BarrierMode::Legacy);
        RunTimestampResolveIsolation(fixture);
        RunFinalExports(fixture, hs::BarrierMode::Legacy);
        Run(fixture, hs::BarrierMode::Legacy, 0);
        RunExternal(fixture, hs::BarrierMode::Legacy, 0);
        RunPlanReuse(fixture, hs::BarrierMode::Legacy, 0);
        RunUavClear(fixture, hs::BarrierMode::Legacy, 0);

        RunSharedTextureReaders(fixture, hs::BarrierMode::Legacy, 0);

        fixture.CheckValidation();
        std::puts("Legacy Copy -> Compute -> Direct queue chain passed.");

        D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
        if (SUCCEEDED(fixture.device->CheckFeatureSupport(
                D3D12_FEATURE_D3D12_OPTIONS12, &options, sizeof(options))) &&
            options.EnhancedBarriersSupported)
        {
            RunRecordingDispatch(fixture, hs::BarrierMode::Enhanced);
            RunFinalExports(fixture, hs::BarrierMode::Enhanced);
            Run(fixture, hs::BarrierMode::Enhanced, 1);
            RunExternal(fixture, hs::BarrierMode::Enhanced, 1);
            RunPlanReuse(fixture, hs::BarrierMode::Enhanced, 1);
            RunUavClear(fixture, hs::BarrierMode::Enhanced, 1);
            RunSameStateRepeatedRead(fixture);

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
