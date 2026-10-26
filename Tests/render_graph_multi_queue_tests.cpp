#include "render_graph.hpp"
#include "render_graph_validation.hpp"
#include "renderer_local_probe_draws.hpp"
#include "renderer_local_probe.hpp"
#include "renderer_virtual_shadow.hpp"
#include <hs/renderer/renderer.hpp>

#include <D3D12MemAlloc.h>
#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

void RunBakedProbePreparation(Fixture &fixture)
{
    namespace rd = hs::render_data;
    const auto id = hs::MakeAssetId("render/lighting/local_probes.spkg");
    const std::array<std::string, 4> dependencies{
        "environment.spkg", "layout.spkg", "lights.spkg", "materials.spkg"};
    rd::SceneProbeDescription description;
    description.edge = 16; description.ggx_samples = 64;
    description.revisions = {1, 2, 3, 4};
    description.environment_dependency = 0; description.material_dependency = 3;
    description.light_dependency = 2; description.static_layout_dependency = 1;
    rd::SceneProbeRecord probe;
    probe.stable_id = 1;
    probe.influence_min = probe.projection_min = {-1, -1, -1};
    probe.influence_max = probe.projection_max = {1, 1, 1};
    const std::array probes{probe};
    const std::vector<hs::Float4> raw(6 * 16 * 16, {1, 1, 1, 1});
    const std::vector<hs::Float4> filtered(6 * (256 + 64 + 16 + 4 + 1), {1, 1, 1, 1});
    const std::vector<hs::Float4> sh(9, {1, 1, 1, 0});
    const std::vector<float> distance(raw.size(), probe.capture_distance);
    const auto bytes = rd::BuildSceneProbePackage(id, dependencies, description, probes,
        {raw, filtered, sh, distance});
    std::uint32_t shader_reads{};
    const hs::LocalProbeSystem::ShaderReader unavailable_shader =
        [&](std::string_view, std::vector<std::byte> &) {
            ++shader_reads;
            return hs::Result::Failure(hs::ErrorCode::TaskFailed, "test", "Capture shader intentionally unavailable");
        };
    hs::LocalProbeSystem service;
    hs::LocalProbeCandidate candidate;
    Check(service.Prepare(fixture.device.Get(), fixture.allocator, bytes, unavailable_shader, candidate).Succeeded(),
        "baked probe must prepare without capture shaders");
    Check(shader_reads == 0, "sample-only probe read a capture shader");
    Check(service.Commit(std::move(candidate), {}).Succeeded(), "commit sample-only probe");
    hs::RenderGraphBuilder graph;
    auto scene = std::make_shared<hs::StaticRenderScene>();
    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "probe export pool");
    const hs::LocalProbeInputs inputs{scene, description.revisions, {}, {}, {}, 0};
    hs::LocalProbeOutputs output;
    Check(service.Register(graph, inputs, output).Succeeded(), "register baked probe upload");
    Check(graph.Prepare(pool).Succeeded(), "prepare baked probe upload");
    Check(graph.PassCount() == 1 && graph.CompiledPassName(0) == "LocalProbePackageUpload",
        "compatible baked SPB1 upload must not register native capture or filter passes");
    hs::CompletionSet completion;
    fixture.Execute(graph, 0, hs::BarrierMode::Legacy, completion, "submit baked probe upload");
    Check(service.CompleteSubmission(graph, completion, true, scene, description.revisions).Succeeded(),
        "complete baked probe upload submission");
    Check(fixture.executor.Wait(completion).Succeeded(), "wait baked probe upload");
    std::vector<std::byte> exported;
    Check(service.ExportBakedPackage(fixture.executor, pool, 0, hs::BarrierMode::Legacy, exported).Succeeded(),
        "completed current probe can export before another frame publishes its sample bindings");
    Check(exported == bytes, "native four-payload readback changed the baked package");
    graph.Reset();
    auto published_inputs = inputs;
    published_inputs.completed = fixture.executor.Completed();
    published_inputs.max_cube_views = 6;
    Check(service.Register(graph, published_inputs, output).Succeeded() && output.enabled,
        "completed baked probe was not published");
    Check(graph.Prepare(pool).Succeeded(), "prepare baked probe publication");
    Check(graph.PassCount() == 0,
        "matching baked probe publication must not schedule native capture or filtering");
    for (const auto &[name, resource] : std::array{
            std::pair{"radiance", output.radiance_resource}, std::pair{"prefiltered", output.prefiltered_resource},
            std::pair{"irradiance", output.irradiance_resource}, std::pair{"distance", output.distance_resource}})
    {
        // D3D12MA may add an allocation-only flag without adding render/UAV capabilities.
        // The system SDK predates the bundled Agility enum; this is its USE_TIGHT_ALIGNMENT value.
        constexpr auto allocation_flags = static_cast<D3D12_RESOURCE_FLAGS>(0x400u);
        const bool sample_only = resource && (resource->GetDesc().Flags & ~allocation_flags) == D3D12_RESOURCE_FLAG_NONE;
        if (!sample_only)
            std::fprintf(stderr, "Baked probe %s resource present=%u flags=%u\n", name,
                resource != nullptr, resource ? UINT(resource->GetDesc().Flags) : UINT_MAX);
        Check(sample_only, "baked sample-only resource contains unexpected producer capabilities");
    }
    fixture.Execute(graph, 0, hs::BarrierMode::Legacy, completion, "submit baked probe publication");
    Check(service.CompleteSubmission(graph, completion, true, scene, description.revisions).Succeeded(),
        "complete baked probe publication");
    Check(fixture.executor.Wait(completion).Succeeded(), "wait baked probe readers");
    Check(!service.NativeCaptureRequired(scene, description.revisions, fixture.executor.Completed()),
        "matching baked profile must not request a runtime recapture after upload");
    Check(!service.PrepareCapture(unavailable_shader, {}).Succeeded() && shader_reads == 0,
        "capture upgrade ignored in-flight reader completion");
    Check(!service.PrepareCapture(unavailable_shader, fixture.executor.Completed()).Succeeded() && shader_reads == 1,
        "capture upgrade did not preserve native shader failure");
    Check(service.ResetAfterCompletion(fixture.executor.Completed()).Succeeded(), "reset failed probe capture upgrade");
    struct Compiler
    {
        HMODULE module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        ~Compiler() { if (module) FreeLibrary(module); }
    } compiler;
    Check(compiler.module != nullptr, "load probe fixture shader compiler");
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.module, "D3DCompile"));
    Check(compile != nullptr, "probe fixture D3DCompile");
    constexpr char shader[] = "[numthreads(1,1,1)] void main() {}";
    ComPtr<ID3DBlob> code, diagnostics;
    Hr(compile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "main", "cs_5_1", 0, 0,
        &code, &diagnostics), "compile probe state-only filter fixture");
    const hs::LocalProbeSystem::ShaderReader state_shader =
        [&](std::string_view, std::vector<std::byte> &output) {
            const auto *begin = static_cast<const std::byte *>(code->GetBufferPointer());
            output.assign(begin, begin + code->GetBufferSize());
            return hs::Result::Success();
        };
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options{};
    const bool enhanced = SUCCEEDED(fixture.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,
        &options, sizeof(options))) && options.EnhancedBarriersSupported;
    ComPtr<ID3D12Resource> scene_dependency;
    const auto dependency_heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const auto dependency_description = BufferDescription(256);
    Hr(fixture.device->CreateCommittedResource(&dependency_heap, D3D12_HEAP_FLAG_NONE, &dependency_description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&scene_dependency)), "probe fixture scene dependency");
    for (const auto mode : {hs::BarrierMode::Legacy, hs::BarrierMode::Enhanced})
    {
        if (mode == hs::BarrierMode::Enhanced && !enhanced) continue;
        Check(service.Prepare(fixture.device.Get(), fixture.allocator, bytes, unavailable_shader, candidate).Succeeded(),
            "prepare recapture fixture");
        Check(service.Commit(std::move(candidate), fixture.executor.Completed()).Succeeded(), "commit recapture fixture");
        auto capture_inputs = inputs;
        for (std::uint32_t step = 0; step < 4; ++step)
        {
            graph.Reset();
            capture_inputs.completed = fixture.executor.Completed();
            if (step == 1)
            {
                ++capture_inputs.revisions.light;
                Check(service.NativeCaptureRequired(scene, capture_inputs.revisions, capture_inputs.completed),
                    "stale baked probe did not request capture");
                Check(service.PrepareCapture(state_shader, capture_inputs.completed).Succeeded(), "upgrade baked probe for capture");
            }
            capture_inputs.max_cube_views = step == 1 ? 1 : step == 2 ? 5 : 0;
            if (capture_inputs.max_cube_views)
            {
                const auto dependency = graph.ImportBuffer({scene_dependency.Get()}, hs::Access::AllShaderRead,
                    "ProbeFixture.SceneDependency");
                const std::array reads{hs::LocalProbeSceneRead{graph.Version(dependency), hs::Access::AllShaderRead}};
                capture_inputs.scene_reads = reads;
                capture_inputs.draw_static = [](hs::RenderPassContext &, const hs::LocalProbeCaptureView &) {
                    return hs::Result::Success();
                };
                Check(service.Register(graph, capture_inputs, output).Succeeded(), "register bounded stale capture");
                capture_inputs.scene_reads = {};
            }
            else Check(service.Register(graph, capture_inputs, output).Succeeded(), "register recapture publication");
            Check(graph.Prepare(pool).Succeeded(), "prepare recapture state transitions");
            fixture.Execute(graph, 0, mode, completion, "submit recapture state transitions");
            Check(service.CompleteSubmission(graph, completion, true, scene, capture_inputs.revisions).Succeeded(),
                "complete recapture state transitions");
            Check(fixture.executor.Wait(completion).Succeeded(), "wait recapture fixture");
            fixture.CheckValidation();
        }
        Check(output.enabled && output.captured_cube_views == 6, "bounded stale recapture did not publish");
        Check(service.ResetAfterCompletion(fixture.executor.Completed()).Succeeded(), "reset recapture fixture");
    }
    fixture.CheckValidation();
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

void RunQueueJoinReuse(Fixture &fixture)
{
    hs::RenderGraphQueueExecutor executor;
    Check(executor.Initialize(fixture.device.Get(), nullptr, 1).Succeeded(), "join executor initialize");
    Check(executor.Drain().Succeeded(), "signal initial join timelines");
    auto first = executor.Completed();
    first.direct = 0;
    Check(first.compute && first.copy, "join test has owned producer serials");
    Check(executor.Join(first, hs::QueueHint::Direct).Succeeded(), "join both producer timelines");
    Check(executor.LastExecutionStats().cross_queue_waits == 2, "first join waits for both producers");
    Check(executor.Join(first, hs::QueueHint::Direct).Succeeded(), "repeat producer join");
    Check(executor.LastExecutionStats().cross_queue_waits == 2, "repeated join does not enqueue duplicate waits");

    Check(executor.Drain().Succeeded(), "advance join timelines");
    auto next = executor.Completed();
    next.direct = 0;
    Check(next.compute > first.compute && next.copy > first.copy, "join serials advance");
    Check(executor.Join(next, hs::QueueHint::Direct).Succeeded(), "join newer producer serials");
    Check(executor.LastExecutionStats().cross_queue_waits == 4, "newer serials still enqueue waits");
    Check(executor.Join(first, hs::QueueHint::Direct).Succeeded(), "join older producer serials");
    Check(executor.LastExecutionStats().cross_queue_waits == 4, "newer waits cover older serials");
    Check(executor.Join(next, hs::QueueHint::Copy).Succeeded(), "join from another destination");
    Check(executor.LastExecutionStats().cross_queue_waits == 5, "wait reuse is destination-specific");
    auto invalid = next;
    invalid.compute = UINT64_MAX;
    Check(!executor.Join(invalid, hs::QueueHint::Direct), "wait reuse still rejects unowned serials");
    Check(executor.LastExecutionStats().cross_queue_waits == 5, "invalid serial adds no wait");

    executor.Clear();
    Check(executor.Initialize(fixture.device.Get(), nullptr, 1).Succeeded(), "reinitialize join executor");
    Check(executor.Drain().Succeeded(), "signal replacement join timelines");
    auto replacement = executor.Completed();
    replacement.direct = 0;
    Check(executor.Join(replacement, hs::QueueHint::Direct).Succeeded(), "join replacement timelines");
    Check(executor.LastExecutionStats().cross_queue_waits == 2, "new timelines do not inherit old waits");
    Check(executor.Drain().Succeeded(), "complete join test");
    fixture.CheckValidation();
}
void RunRecordingDispatch(Fixture &fixture, hs::BarrierMode barriers)
{
    for (const auto queue : {hs::QueueHint::Direct, hs::QueueHint::Compute})
    for (const bool timestamps_enabled : {false, true})
    for (const bool fail_middle : {false, true})
    {
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "independent pool");
        hs::RenderGraphQueueExecutor executor;
        Check(executor.Initialize(fixture.device.Get(), nullptr, 1).Succeeded(), "independent executor");
        hs::RenderGraphBuilder graph;
        std::array<void *, 6> lists{};
        for (std::uint32_t index = 0; index < lists.size(); ++index)
            graph.AddPass(index + 1, "independent recording", queue).SetExecuteChecked(
                [&, index](hs::RenderPassContext &context) {
                    lists[index] = context.NativeCommandList();
                    return fail_middle && index == 1
                        ? hs::Result::Failure(hs::ErrorCode::TaskFailed, "independent-test", "injected failure")
                        : hs::Result::Success();
                });
        Check(graph.Prepare(pool).Succeeded(), "prepare independent graph");
        hs::RenderTaskExecutor tasks;
        tasks.submit = [](std::function<void()> task) { task(); return hs::Result::Success(); };
        tasks.join = [] { return hs::Result::Success(); };
        std::vector<hs::GpuPassTiming> cpu_passes;
        for (std::uint32_t position = 0; position < graph.PassCount(); ++position)
            cpu_passes.push_back({graph.CompiledPassId(position),
                std::string(graph.CompiledPassName(position)),
                static_cast<hs::RenderQueue>(graph.CompiledPassQueue(position))});
        ComPtr<ID3D12QueryHeap> queries;
        const D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 12, 0};
        Hr(fixture.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)), "independent timestamp heap");
        ComPtr<ID3D12Resource> timestamps;
        const auto heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const auto description = BufferDescription(12 * sizeof(std::uint64_t));
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestamps)), "independent timestamp readback");
        hs::GraphExecutionOptions options;
        options.timestamp_query_heap = timestamps_enabled ? queries.Get() : nullptr;
        options.timestamp_readback = timestamps_enabled ? timestamps.Get() : nullptr;
        options.task_executor = &tasks;
        options.cpu_pass_timings = cpu_passes.data();
        options.cpu_pass_timing_count = cpu_passes.size();
        hs::CompletionSet completion;
        const auto result = executor.Execute(graph, 0, barriers, completion, &options);
        Check(executor.LastExecutionStats().record_jobs ==
                  6u + (timestamps_enabled ? 1u : 0u), "each pass retains its own recording job");
        Check(result.Succeeded() != fail_middle, "independent callback failure preserved");
        if (fail_middle)
            Check(completion.direct == 0 && completion.compute == 0 &&
                  executor.LastExecutionStats().direct_batches == 0 &&
                  executor.LastExecutionStats().compute_batches == 0,
                  "independent recording failure submits no GPU work");
        else
        {
            for (std::size_t i = 0; i < lists.size(); ++i)
                for (std::size_t j = i + 1; j < lists.size(); ++j)
                    Check(lists[i] && lists[i] != lists[j], "pass command list state remains isolated");
            for (const auto &timing : cpu_passes)
                Check(timing.cpu_record_valid && timing.cpu_callback_valid &&
                          timing.cpu_record_begin_qpc <= timing.cpu_callback_begin_qpc &&
                          timing.cpu_callback_begin_qpc <= timing.cpu_callback_end_qpc &&
                          timing.cpu_callback_end_qpc <= timing.cpu_record_end_qpc,
                      "independent recording retains each callback inside its pass recording interval");
            Check(executor.Wait(completion).Succeeded(), "wait independent graph");
            if (timestamps_enabled)
            {
            std::uint64_t *values{};
            D3D12_RANGE range{0, 12 * sizeof(std::uint64_t)};
            Hr(timestamps->Map(0, &range, reinterpret_cast<void **>(&values)),
               "map independent timestamps");
            for (std::size_t index = 0; index < 12; index += 2)
                Check(values[index] != 0 && values[index + 1] >= values[index],
                      "independent lists preserve every GPU timestamp pair");
            D3D12_RANGE no_write{};
            timestamps->Unmap(0, &no_write);
            }
        }
    }
    enum class Outcome { Success, Reject, Throw, JoinFailure, OwnerFailure };
    for (const auto outcome : {Outcome::Success, Outcome::Reject, Outcome::Throw,
                               Outcome::JoinFailure, Outcome::OwnerFailure})
    {
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(),
              "recording dispatch pool initialize");
        hs::RenderGraphBuilder graph;
        std::vector<std::jthread> workers;
        std::atomic<std::uint32_t> completed{};
        std::uint32_t submitted{}, owner_calls{};
        bool joined{};
        const auto owner = std::this_thread::get_id();
        const auto failed = [] {
            return hs::Result::Failure(hs::ErrorCode::TaskFailed, "recording-test", "injected failure");
        };
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
                    Check(submitted == 2, "owner callback blocked later worker dispatch");
                    Check(index == 1 + owner_calls * 2, "owner callback order changed");
                    ++owner_calls;
                    if (outcome == Outcome::Success && index == 1)
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    return outcome == Outcome::OwnerFailure && index == 1
                        ? failed() : hs::Result::Success();
                });
        }
        Check(graph.Prepare(pool).Succeeded(), "prepare recording dispatch graph");
        hs::RenderTaskExecutor tasks;
        tasks.submit = [&](std::function<void()> task) {
            if (submitted == 1)
            {
                if (outcome == Outcome::Reject) return failed();
                if (outcome == Outcome::Throw) throw std::runtime_error("injected submit failure");
            }
            workers.emplace_back(std::move(task));
            ++submitted;
            return hs::Result::Success();
        };
        tasks.join = [&] {
            for (auto &worker : workers) worker.join();
            joined = true;
            return outcome == Outcome::JoinFailure ? failed() : hs::Result::Success();
        };
        hs::GraphExecutionOptions options;
        options.task_executor = &tasks;
        std::vector<hs::GpuPassTiming> cpu_passes;
        for (std::uint32_t position = 0; position < graph.PassCount(); ++position)
            cpu_passes.push_back({graph.CompiledPassId(position),
                std::string(graph.CompiledPassName(position)),
                static_cast<hs::RenderQueue>(graph.CompiledPassQueue(position))});
        options.cpu_pass_timings = cpu_passes.data();
        options.cpu_pass_timing_count = cpu_passes.size();
        hs::CompletionSet completion;
        const auto execute_start = std::chrono::steady_clock::now();
        const auto result = fixture.executor.Execute(graph, 0, barriers, completion, &options);
        const auto execute_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - execute_start).count();
        const bool dispatch_failed = outcome == Outcome::Reject || outcome == Outcome::Throw;
        Check(joined && completed == (dispatch_failed ? 1u : 2u),
              "recording tasks escaped mandatory join");
        Check(owner_calls == (dispatch_failed ? 0u : 2u),
              "rejected dispatch ran owner callbacks or successful dispatch omitted them");
        Check(fixture.executor.LastExecutionStats().record_jobs == submitted,
              "recording job count includes rejected or owner work");
        Check(result.Succeeded() == (outcome == Outcome::Success),
              "recording dispatch failure was lost");
        if (outcome == Outcome::Success)
        {
            const auto timings = fixture.executor.LastExecutionStats();
            Check(timings.cpu_record_us >= 1000, "record timing omitted owner callback wall time");
            LARGE_INTEGER frequency{};
            Check(QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0, "CPU timing QPC frequency");
            for (const auto &pass : cpu_passes)
            {
                Check(pass.cpu_record_valid && pass.cpu_record_end_qpc >= pass.cpu_record_begin_qpc,
                    "recording interval missing or reversed");
                Check(pass.cpu_callback_valid && pass.cpu_record_begin_qpc <= pass.cpu_callback_begin_qpc &&
                    pass.cpu_callback_begin_qpc <= pass.cpu_callback_end_qpc &&
                    pass.cpu_callback_end_qpc <= pass.cpu_record_end_qpc,
                    "callback interval is missing or outside its recording interval");
                Check(!pass.cpu_provider_submit_valid, "ordinary pass fabricated provider submission timing");
                if (pass.pass_id == 302)
                    Check(static_cast<long double>(pass.cpu_callback_end_qpc - pass.cpu_callback_begin_qpc) *
                        1000 / frequency.QuadPart >= 1.0L, "callback timing omitted known owner callback delay");
            }
            Check(timings.cpu_plan_us + timings.cpu_record_us + timings.cpu_submit_us <=
                    static_cast<std::uint64_t>(execute_us), "graph timing stages overlap or exceed execution wall time");
            Check(fixture.executor.Wait(completion).Succeeded(), "wait recording dispatch graph");
        }
        else
            Check(completion.direct == 0 && completion.compute == 0 && completion.copy == 0,
                  "failed recording graph submitted GPU work");
        fixture.CheckValidation();
    }
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
    hs::RenderTaskExecutor tasks;
    tasks.submit = [](std::function<void()> task) { task(); return hs::Result::Success(); };
    tasks.join = [] { return hs::Result::Success(); };
    hs::GraphExecutionOptions options;
    options.task_executor = &tasks;
    hs::CompletionSet completion;
    Check(fixture.executor.Execute(graph, 0, barriers, completion, &options).Succeeded(),
        "execute grouped final exports");
    const auto stats = fixture.executor.LastExecutionStats();
    Check(stats.record_jobs == count / 2 + 1 && stats.direct_batches == 1,
        "independent exports share one destination list without merging source releases");
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

    const auto before_rotation_compilations = hs::RenderGraphTestAccess::Compilations(graph);
    const auto before_rotation_reuses = hs::RenderGraphTestAccess::Reuses(graph);
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
    Check(hs::RenderGraphTestAccess::Compilations(graph) == before_rotation_compilations + 2 &&
        hs::RenderGraphTestAccess::Reuses(graph) == before_rotation_reuses + 7,
        "three rotating range plans reuse exact schedules with current callbacks and imports");

    // Descriptor fields must participate even when allocation size would match.
    const auto before_descriptor_compilations = hs::RenderGraphTestAccess::Compilations(graph);
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
    Check(hs::RenderGraphTestAccess::Compilations(graph) == before_descriptor_compilations + 2,
        "format changes invalidate a plan despite matching allocation classes");
    const auto descriptor_reuses = hs::RenderGraphTestAccess::Reuses(graph);
    texture_frame({16, 16, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS});
    texture_frame({16, 16, DXGI_FORMAT_R16_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS});
    Check(hs::RenderGraphTestAccess::Compilations(graph) == before_descriptor_compilations + 2 &&
        hs::RenderGraphTestAccess::Reuses(graph) == descriptor_reuses + 2,
        "alternating exact descriptors reuse their validated allocation plans");

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
        std::vector<hs::GpuPassTiming> cpu_passes;
        for (std::uint32_t position = 0; position < graph.PassCount(); ++position)
            cpu_passes.push_back({graph.CompiledPassId(position),
                std::string(graph.CompiledPassName(position)),
                static_cast<hs::RenderQueue>(graph.CompiledPassQueue(position))});
        hs::GraphExecutionOptions capture_options;
        capture_options.cpu_pass_timings = cpu_passes.data();
        capture_options.cpu_pass_timing_count = cpu_passes.size();
        const auto result = fixture.executor.Execute(graph, slot, barriers, completion, &capture_options);
        Check(result.Succeeded() == (outcome == 0), "external callback outcome is reported");
        for (const auto &pass : cpu_passes)
        {
            Check(pass.cpu_record_valid, "external graph prefix/ordinary recording interval missing");
            Check(pass.cpu_provider_submit_valid == (pass.pass_id == 110),
                "provider submission interval assigned to wrong pass");
            if (pass.pass_id == 110)
                Check(pass.cpu_provider_submit_end_qpc >= pass.cpu_provider_submit_begin_qpc,
                    "provider callback interval reversed");
        }
        Check(outcome != 2 || result.Message().find("invalid owned fence") != std::string_view::npos,
              "malformed completion is rejected explicitly");
        Check(completion.copy && !completion.IsSatisfiedBy(fixture.executor.Completed()),
              "external bridge remains incomplete behind real independent queue gate");
        Check(graph.ResourceCompletion(destination).copy == completion.copy,
              "resource retirement includes the provider bridge");
        Check(graph.FinalAccess(source) == hs::Access::Common &&
            graph.FinalQueue(source) == hs::QueueHint::Copy,
            "provider outgoing state replaces declared source access");
        Check(graph.FinalAccess(destination) == (outcome == 0 ? hs::Access::CopySource : hs::Access::Common) &&
            graph.FinalQueue(destination) == (outcome == 0 ? hs::QueueHint::Direct : hs::QueueHint::Copy),
            "partial submission publishes provider state without the unsubmitted consumer");
        Check(outcome == 0 || graph.ResourceCompletion(destination).direct == 0,
            "failed provider must not publish the future Direct consumer completion");
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

void RunBatchedBufferBarriers(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t frame_slot)
{
    // Cross the 64-entry flush boundary twice and exercise a nonempty tail.
    constexpr UINT count = 137, words = 64, bytes = words * sizeof(UINT);
    const auto description = BufferDescription(count * bytes);
    auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    ComPtr<ID3D12Resource> upload, readback;
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "create batched barrier upload");
    heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create batched barrier readback");
    const D3D12_RANGE no_read{0, 0};
    void *mapped{};
    Hr(upload->Map(0, &no_read, &mapped), "map batched barrier upload");
    for (UINT index = 0; index < count * words; ++index)
        static_cast<UINT *>(mapped)[index] = 0x13579bdfu ^ (index * 251u);
    upload->Unmap(0, nullptr);
    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "batched barrier pool");
    hs::RenderGraphBuilder graph;
    std::array<hs::ResourceHandle, count> sources, outputs;
    std::array<hs::ResourceVersion, count> initialized, copied;
    for (UINT index = 0; index < count; ++index)
    {
        sources[index] = graph.CreateBuffer({bytes, D3D12_RESOURCE_FLAG_NONE}, "batched source");
        outputs[index] = graph.CreateBuffer({bytes, D3D12_RESOURCE_FLAG_NONE}, "batched output");
    }
    auto seed = graph.AddPass(360, "Batched barrier upload", hs::QueueHint::Direct);
    for (UINT index = 0; index < count; ++index)
        initialized[index] = seed.Write(graph.Version(sources[index]), hs::Access::CopyDest);
    seed.SetExecute([&](hs::RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (UINT index = 0; index < count; ++index)
            commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(sources[index])), 0,
                upload.Get(), index * bytes, bytes);
    });
    auto transfer = graph.AddPass(361, "Batched barrier compute copy", hs::QueueHint::Compute);
    for (UINT index = 0; index < count; ++index)
    {
        transfer.Read(initialized[index], hs::Access::CopySource);
        copied[index] = transfer.Write(graph.Version(outputs[index]), hs::Access::CopyDest);
    }
    transfer.SetExecute([&](hs::RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (UINT index = 0; index < count; ++index)
            commands->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(outputs[index])), 0,
                static_cast<ID3D12Resource *>(context.Resolve(sources[index])), 0, bytes);
    });
    auto finish = graph.AddPass(362, "Batched barrier readback", hs::QueueHint::Copy);
    for (const auto version : copied) finish.Read(version, hs::Access::CopySource);
    finish.SetExecute([&](hs::RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        for (UINT index = 0; index < count; ++index)
            commands->CopyBufferRegion(readback.Get(), index * bytes,
                static_cast<ID3D12Resource *>(context.Resolve(outputs[index])), 0, bytes);
    });
    Check(graph.Prepare(pool).Succeeded(), "prepare batched buffer barriers");
    hs::CompletionSet completion;
    fixture.Execute(graph, frame_slot, barriers, completion, "execute batched buffer barriers");
    Check(fixture.executor.Wait(completion).Succeeded(), "wait batched buffer barriers");
    const D3D12_RANGE read{0, count * bytes};
    Hr(readback->Map(0, &read, &mapped), "map batched barrier result");
    for (UINT index = 0; index < count * words; ++index)
        Check(static_cast<const UINT *>(mapped)[index] == (0x13579bdfu ^ (index * 251u)),
            "batched buffer barrier lost data or tail");
    readback->Unmap(0, &no_read);
    fixture.CheckValidation();
    pool.ResetCompletedClaims(fixture.executor.Completed());
    std::printf("Batched buffer barriers %s passed (137 buffers, three queues, all 8768 words).\n",
        barriers == hs::BarrierMode::Enhanced ? "Enhanced" : "Legacy");
}

void RunVirtualShadowFeedbackOracle(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t frame_slot)
{
    constexpr std::array<UINT, 9> order_counts{0, 1, 31, 32, 33, 65, 1152, 1153, 1152};
    UINT random = 0x56534d31u;
    const auto next_random = [&] { random = random * 1664525u + 1013904223u; return random; };
    for (UINT test = 0; test < 309; ++test)
    {
        const UINT count = test < order_counts.size() ? order_counts[test] : next_random() % 1200u;
        std::vector<UINT> masks((count + 31u) / 32u);
        for (auto &mask : masks) mask = test == 0 ? 0u : test < 9 ? 0xffffffffu : next_random();
        std::vector<UINT> reference, walked;
        for (UINT entry = 0; entry < count; ++entry)
            if ((masks[entry >> 5u] & (1u << (entry & 31u))) != 0) reference.push_back(entry);
        for (UINT word = 0; word < masks.size(); ++word)
        {
            UINT mask = masks[word];
            while (mask)
            {
                const UINT entry = word * 32u + std::countr_zero(mask);
                mask &= mask - 1u;
                if (entry < count) walked.push_back(entry);
            }
        }
        Check(walked == reference, "VSM requested-entry order or tail-bit filtering changed");
        // Dense/sparse masks, partial final words, and a partial caster group
        // must enumerate every requested page/caster pair exactly once.
        const UINT caster_count = test % 4u == 0u ? 0u : test % 4u == 1u ? 1u : test % 4u == 2u ? 64u : 65u;
        std::vector<UINT> expected_counts(count), actual_counts(count);
        const auto candidate = [](UINT entry, UINT caster) { return (entry * 17u + caster * 13u) % 7u < 3u; };
        for (UINT entry : reference)
            for (UINT caster = 0u; caster < caster_count; ++caster)
                expected_counts[entry] += candidate(entry, caster);
        for (UINT word = 0u; word < masks.size(); ++word)
            for (UINT caster = 0u; caster < ((caster_count + 63u) / 64u) * 64u; ++caster)
            {
                if (caster >= caster_count) continue;
                UINT mask = masks[word];
                while (mask)
                {
                    const UINT entry = word * 32u + std::countr_zero(mask);
                    mask &= mask - 1u;
                    if (entry < count) actual_counts[entry] += candidate(entry, caster);
                }
            }
        Check(actual_counts == expected_counts, "VSM word dispatch duplicated or lost page/caster candidates");
        for (const UINT capacity : {0u, 1u, 31u, 4096u})
        {
            std::vector<UINT> expected_rejected(count), actual_rejected(count);
            UINT expected_remaining = capacity, actual_remaining = capacity;
            for (UINT entry = 0u; entry < count; ++entry)
                if (expected_counts[entry] > expected_remaining) expected_rejected[entry] = 1u;
                else expected_remaining -= expected_counts[entry];
            for (UINT entry : walked)
                if (actual_counts[entry] > actual_remaining) actual_rejected[entry] = 1u;
                else actual_remaining -= actual_counts[entry];
            Check(actual_rejected == expected_rejected && actual_remaining == expected_remaining,
                "VSM requested-only admission changed ascending capacity decisions");
        }

    }
    struct Compiler
    {
        HMODULE module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        ~Compiler() { if (module) FreeLibrary(module); }
    } compiler;
    Check(compiler.module != nullptr, "load virtual shadow oracle compiler");
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.module, "D3DCompile"));
    Check(compile != nullptr, "find virtual shadow oracle compiler");
    const auto source_path = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "Content/Shaders/virtual_shadow.hlsl";
    std::ifstream source(source_path, std::ios::binary);
    Check(source.good(), "open actual virtual shadow shader");
    std::string shader{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
    // Compile the production helpers, not a CPU mirror of their shared-memory algorithm.
    shader += R"(
StructuredBuffer<uint> OracleEntries : register(t5, space21);
cbuffer OracleConstants : register(b0, space22) { uint OracleCount, OracleWidth, OracleHeight, OracleBudget; };
[numthreads(8,8,1)] void OptimizedOracle(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    BeginFeedbackRequests(lane);
    const uint first = (group.x * 64u + lane) * 3u;
    for (uint index = first; index < min(first + 3u, OracleCount); ++index) Request(OracleEntries[index]);
    FlushFeedbackRequests(lane);
}
[numthreads(8,8,1)] void ReferenceOracle(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint first = (group.x * 64u + lane) * 3u;
    for (uint index = first; index < min(first + 3u, OracleCount); ++index)
    {
        const uint entry = OracleEntries[index];
        if (entry != 0xffffffffu)
        {
            uint ignored;
            Requests.InterlockedOr((entry >> 5u) * 4u, 1u << (entry & 31u), ignored);
        }
    }
}

[numthreads(1,1,1)] void OptimizedOrderOracle(uint3 unused : SV_DispatchThreadID)
{
    uint count = 0u;
    for (uint word = 0u; word < (OracleCount + 31u) / 32u; ++word)
    {
        uint mask = OracleEntries[word];
        while (mask != 0u)
        {
            const uint entry = PopRequestedEntry(word, mask);
            if (entry < OracleCount) Requests.Store((++count) * 4u, entry);
        }
    }
    Requests.Store(0u, count);
}
[numthreads(1,1,1)] void ReferenceOrderOracle(uint3 unused : SV_DispatchThreadID)
{
    uint count = 0u;
    for (uint entry = 0u; entry < OracleCount; ++entry)
        if ((OracleEntries[entry >> 5u] & (1u << (entry & 31u))) != 0u)
            Requests.Store((++count) * 4u, entry);
    Requests.Store(0u, count);
}
)";
    // Execute the production dilation body with buffer-backed Loads/Stores, comparing every output bit.
    std::ifstream post_source(source_path.parent_path() / "hdr_post.hlsl", std::ios::binary);
    const std::string post{std::istreambuf_iterator<char>(post_source), std::istreambuf_iterator<char>()};
    const auto function = post.find("void HdrPostDilateCS");
    const auto begin = post.rfind("[numthreads", function);
    const auto end = post.find("[numthreads", function);
    Check(begin != std::string::npos && end != std::string::npos, "find production DOF dilation");
    std::string dilation = post.substr(begin, end - begin);
    const auto replace = [&](std::string_view from, std::string_view to) {
        std::size_t offset{}; bool found{};
        while ((offset = dilation.find(from, offset)) != std::string::npos)
        { dilation.replace(offset, from.size(), to); offset += to.size(); found = true; }
        Check(found, "DOF oracle source binding changed");
    };
    replace("EffectParameters.x", "float(OracleBudget)");
    replace("OutputSize.xy", "float2(OracleWidth, OracleHeight)");
    replace("SignedCoC.Load(int3(pixel,0))", "asfloat(OracleEntries[uint(clamp(pixel.y, 0, int(OracleHeight)-1)) * OracleWidth + uint(clamp(pixel.x, 0, int(OracleWidth)-1))])");
    replace("ForegroundOutput[dispatch.xy] = radius;", "Requests.Store((dispatch.y * OracleWidth + dispatch.x) * 4u, asuint(radius));");
    shader += "\ngroupshared float CoCTile[72 * 72]; groupshared uint CoCTileMaximum;\n" + dilation;
    std::array<D3D12_ROOT_PARAMETER1, 3> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 22, 4};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor = {5, 21, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor = {2, 21, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_desc.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    root_desc.Desc_1_1.pParameters = parameters.data();
    ComPtr<ID3DBlob> serialized, errors;
    Hr(D3D12SerializeVersionedRootSignature(&root_desc, &serialized, &errors), "serialize virtual shadow oracle root");
    ComPtr<ID3D12RootSignature> root;
    Hr(fixture.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&root)), "create virtual shadow oracle root");
    std::array<ComPtr<ID3D12PipelineState>, 5> pipelines;
    constexpr std::array entry_points{"ReferenceOracle", "OptimizedOracle",
        "ReferenceOrderOracle", "OptimizedOrderOracle", "HdrPostDilateCS"};
    for (std::size_t entry = 0; entry < pipelines.size(); ++entry)
    {
        ComPtr<ID3DBlob> code, diagnostics;
        const auto result = compile(shader.data(), shader.size(), "VirtualShadowFeedbackOracle", nullptr, nullptr,
            entry_points[entry], "cs_5_1", D3DCOMPILE_ENABLE_STRICTNESS |
            D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &diagnostics);
        if (FAILED(result) && diagnostics)
            std::fprintf(stderr, "%.*s\n", static_cast<int>(diagnostics->GetBufferSize()),
                static_cast<const char *>(diagnostics->GetBufferPointer()));
        Hr(result, "compile actual virtual shadow feedback oracle");
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = root.Get();
        description.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        Hr(fixture.device->CreateComputePipelineState(&description, IID_PPV_ARGS(&pipelines[entry])),
            "create virtual shadow feedback oracle pipeline");
    }
    constexpr UINT words = 2048, bytes = words * sizeof(UINT);
    constexpr std::array<UINT, 6> counts{1, 191, 192, 193, 1541, 1541};
    constexpr std::array<UINT, 12> boundary_entries{
        0, 31, 32, 2047, 2048, 65534, 0xffffffffu, 0, 0, 31, 2047, 2048};
    for (std::size_t test = 0; test < counts.size() + order_counts.size() + 6; ++test)
    {
        const bool dof_test = test >= counts.size() + order_counts.size();
        const UINT dof_case = dof_test ? UINT(test - counts.size() - order_counts.size()) : 0;
        constexpr UINT width = 37, height = 29;
        const UINT budget = dof_case == 5 ? 32u : 12u;
        const bool order_test = !dof_test && test >= counts.size();
        const auto order_index = order_test ? test - counts.size() : 0;
        const UINT count = dof_test ? width * height : order_test ? order_counts[order_index] : counts[test];
        const UINT input_count = order_test ? std::max(1u, (count + 31u) / 32u) : count;
        std::vector<UINT> entries(input_count);
        for (UINT index = 0; index < input_count; ++index)
            entries[index] = order_test ? (order_index == 0 ? 0u :
                order_index < 5 || order_index == 8 ? 0xffffffffu : next_random()) :
                index % 7 == 6 ? (index * 4051u) % 65535u : boundary_entries[index % boundary_entries.size()];
        std::array<UINT, words> initial{}, expected{};
        if (order_test) initial.fill(0xffffffffu);
        else if (test == counts.size() - 1)
            for (UINT word = 0; word < words; ++word) initial[word] = word * 0x01010101u;
        expected = initial;
        if (dof_test)
        {
            for (UINT i = 0; i < count; ++i)
            {
                const float radius = dof_case == 0 ? 0.0f : dof_case == 1 ? (i == 0 ? 12.0f : 0.0f) :
                    dof_case == 2 ? (i == count / 2 ? 3.25f : 0.0f) :
                    dof_case == 3 ? float(i % 9) * 0.125f :
                    dof_case == 4 ? float(budget) : float(next_random() % 129u) * 0.25f;
                const float coc = i % 7 == 6 ? radius : -radius;
                std::memcpy(&entries[i], &coc, sizeof(coc));
            }
            for (int y = 0; y < int(height); ++y) for (int x = 0; x < int(width); ++x)
            {
                float maximum{};
                for (int dy = -int(budget); dy <= int(budget); ++dy)
                    for (int dx = -int(budget); dx <= int(budget); ++dx)
                    {
                        const int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= int(width) || sy >= int(height)) continue;
                        float coc; std::memcpy(&coc, &entries[sy * width + sx], sizeof(coc));
                        const float radius = std::max(-coc, 0.0f);
                        if (float(dx * dx + dy * dy) <= radius * radius) maximum = std::max(maximum, radius);
                    }
                std::memcpy(&expected[y * width + x], &maximum, sizeof(maximum));
            }
        }
        else if (order_test)
        {
            UINT output_count{};
            for (UINT entry = 0; entry < count; ++entry)
                if ((entries[entry >> 5u] & (1u << (entry & 31u))) != 0u)
                    expected[++output_count] = entry;
            expected[0] = output_count;
        }
        else for (const auto entry : entries)
            if (entry != 0xffffffffu) expected[entry >> 5u] |= 1u << (entry & 31u);
        const auto upload_desc = BufferDescription(bytes + input_count * sizeof(UINT));
        const auto readback_desc = BufferDescription(bytes * 2u);
        auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> upload, readback;
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "create virtual shadow oracle upload");
        heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create virtual shadow oracle readback");
        void *mapped{};
        const D3D12_RANGE no_read{0, 0};
        Hr(upload->Map(0, &no_read, &mapped), "map virtual shadow oracle upload");
        std::memcpy(mapped, initial.data(), bytes);
        std::memcpy(static_cast<std::byte *>(mapped) + bytes, entries.data(), input_count * sizeof(UINT));
        upload->Unmap(0, nullptr);
        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "virtual shadow oracle pool");
        hs::RenderGraphBuilder graph;
        const hs::BufferDesc description{bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
        std::array<hs::ResourceHandle, 2> outputs;
        std::array<hs::ResourceVersion, 2> initialized, results;
        auto seed = graph.AddPass(320, "Virtual shadow oracle initialize", hs::QueueHint::Copy);
        for (std::size_t entry = 0; entry < outputs.size(); ++entry)
        {
            outputs[entry] = graph.CreateBuffer(description, "virtual shadow request bits");
            initialized[entry] = seed.Write(graph.Version(outputs[entry]), hs::Access::CopyDest);
        }
        seed.SetExecute([&](hs::RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            for (const auto output : outputs)
                list->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(output)), 0, upload.Get(), 0, bytes);
        });
        for (std::size_t entry = 0; entry < outputs.size(); ++entry)
        {
            auto pass = graph.AddPass(321 + static_cast<UINT>(entry), "Virtual shadow request OR", hs::QueueHint::Compute);
            results[entry] = pass.ReadWrite(initialized[entry], hs::Access::UnorderedWrite);
            pass.SetExecute([&, entry](hs::RenderPassContext &context) {
                auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                list->SetComputeRootSignature(root.Get());
                list->SetPipelineState(pipelines[dof_test ? 4 : (order_test ? 2 : 0) + entry].Get());
                const UINT constants[]{count, width, height, budget};
                list->SetComputeRoot32BitConstants(0, 4, constants, 0);
                list->SetComputeRootShaderResourceView(1, upload->GetGPUVirtualAddress() + bytes);
                list->SetComputeRootUnorderedAccessView(2,
                    static_cast<ID3D12Resource *>(context.Resolve(outputs[entry]))->GetGPUVirtualAddress());
                list->Dispatch(dof_test ? (width + 7) / 8 : order_test ? 1u : (count + 191) / 192, dof_test ? (height + 7) / 8 : 1u, 1);
            });
        }
        auto copy = graph.AddPass(323, "Virtual shadow oracle readback", hs::QueueHint::Copy);
        for (const auto result : results) copy.Read(result, hs::Access::CopySource);
        copy.SetExecute([&](hs::RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            for (std::size_t entry = 0; entry < outputs.size(); ++entry)
                list->CopyBufferRegion(readback.Get(), entry * bytes,
                    static_cast<ID3D12Resource *>(context.Resolve(outputs[entry])), 0, bytes);
        });
        Check(graph.Prepare(pool).Succeeded(), "prepare virtual shadow feedback oracle");
        hs::CompletionSet completion;
        fixture.Execute(graph, frame_slot, barriers, completion, "execute virtual shadow feedback oracle");
        Check(fixture.executor.Wait(completion).Succeeded(), "wait virtual shadow feedback oracle");
        const D3D12_RANGE read{0, bytes * 2u};
        const auto mapped_result = readback->Map(0, &read, &mapped);
        if (FAILED(mapped_result))
        {
            std::fprintf(stderr, "Image oracle case %zu: Map HRESULT=%08lx, removed=%08lx\n",
                test, static_cast<unsigned long>(mapped_result), static_cast<unsigned long>(fixture.device->GetDeviceRemovedReason()));
            fixture.CheckValidation();
        }
        Hr(mapped_result, "map virtual shadow request bits");
        const auto *actual = static_cast<const UINT *>(mapped);
        for (std::size_t entry = 0; entry < outputs.size(); ++entry)
            for (UINT word = 0; word < words; ++word)
                Check(actual[entry * words + word] == expected[word], "GPU VSM or production DOF dilation output mismatch");
        readback->Unmap(0, &no_read);
        fixture.CheckValidation();
        pool.ResetCompletedClaims(fixture.executor.Completed());
    }
    std::printf("Virtual shadow feedback %s passed (6 GPU OR bitsets, 9 requested-entry lists, 6 DOF dilation patterns including partial groups and max radius).\n",
        barriers == hs::BarrierMode::Enhanced ? "Enhanced" : "Legacy");
}

void RunVirtualShadowRootScatterOracle(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t frame_slot)
{
    struct CasterInput
    {
        std::array<float, 4> current_bounds{}, previous_bounds{};
        std::array<UINT, 4> draw{}, indexed{}, properties{};
    };
    static_assert(sizeof(CasterInput) == 80);
    struct Segment { UINT first{}, capacity{}; };
    struct Case
    {
        const char *name{};
        UINT group_count{}, max_groups{}, fine_entries{}, fine_capacity{};
        std::vector<CasterInput> casters;
        std::vector<Segment> segments;
        std::vector<bool> mesh_frontends;
    };
    const auto make_caster = [](UINT id, UINT group, bool mesh, UINT flags) {
        CasterInput value{};
        value.current_bounds = {float(id), float(id + 1), float(id + 2), 1.0f};
        value.previous_bounds = value.current_bounds;
        value.draw = {100u + id, 200u + id, 300u + id, 400u + id};
        value.indexed = {group, 20u + id, 3u + id, UINT(-2)};
        value.properties = {flags, 5u + id, mesh ? 1u : 0u, 0u};
        return value;
    };
    Case complete{"complete-mixed-boundary", 2, 3, 5, 4, {
        make_caster(0, 0, false, 69u), // present + immutable + baked: archive owns it
        make_caster(1, 0, false, 5u),  // unbaked immutable caster remains fresh
        make_caster(2, 0, false, 1u),  // skin
        make_caster(3, 1, true, 1u),    // wind
        make_caster(4, 1, true, 1u),    // animated alpha
        make_caster(5, 1, true, 0u),    // absent
        make_caster(6, 0, false, 1u)},  // dynamic
        {{1, 3}, {0, 2}}, {false, true}};
    Case overflow{"capacity-and-group-overflow", 1, 3, 1, 2, {
        make_caster(0, 0, false, 1u), make_caster(1, 1, true, 1u)}, {{0, 0}}, {false}};
    Case empty{"empty-caster-set", 0, 3, 2, 4, {}, {}, {}};
    std::array<Case, 3> cases{std::move(complete), std::move(overflow), std::move(empty)};
    constexpr UINT sentinel = 0xdedededeu;

    struct CompilerModule
    {
        HMODULE module{};
        ~CompilerModule() { if (module) FreeLibrary(module); }
    } compiler_module;
    const auto compiler_path = std::filesystem::path(HS_TEST_DXC_PATH);
    Check(!compiler_path.empty(), "production shadow oracle requires configured DXC");
    const auto compiler_dll = compiler_path.parent_path() / L"dxcompiler.dll";
    compiler_module.module = LoadLibraryExW(compiler_dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Check(compiler_module.module != nullptr, "load production DXC compiler");
    const auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(compiler_module.module, "DxcCreateInstance"));
    Check(create != nullptr, "find production DxcCreateInstance");
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    Hr(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "create DXC utilities");
    Hr(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "create DXC compiler");
    const auto shader_path = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "Content/Shaders/virtual_shadow.hlsl";
    ComPtr<IDxcBlobEncoding> source;
    Hr(utils->LoadFile(shader_path.c_str(), nullptr, &source), "load production virtual shadow source");
    const DxcBuffer input{source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_UTF8};
    const wchar_t *arguments[]{shader_path.c_str(), L"-E", L"CullCS", L"-T", L"cs_6_0", L"-O3", L"-Ges", L"-WX"};
    ComPtr<IDxcResult> compiled;
    Hr(compiler->Compile(&input, arguments, static_cast<UINT32>(std::size(arguments)), nullptr,
        IID_PPV_ARGS(&compiled)), "invoke production shadow DXC");
    ComPtr<IDxcBlobUtf8> diagnostics;
    Hr(compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&diagnostics), nullptr), "read shadow DXC diagnostics");
    if (diagnostics && diagnostics->GetStringLength()) std::fputs(diagnostics->GetStringPointer(), stderr);
    HRESULT compile_status{};
    Hr(compiled->GetStatus(&compile_status), "read shadow DXC status");
    Hr(compile_status, "compile production virtual shadow CullCS");
    ComPtr<IDxcBlob> code;
    Hr(compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&code), nullptr), "read production shadow DXIL");

    constexpr std::array<UINT, 7> uav_registers{0u, 2u, 3u, 4u, 5u, 6u, 8u};
    std::array<D3D12_ROOT_PARAMETER1, 12> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor = {0u, 21u, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {1u, 21u, 1u};
    constexpr std::array<UINT, 3> srv_registers{2u, 5u, 6u};
    for (UINT index = 0; index < srv_registers.size(); ++index)
    {
        parameters[2u + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameters[2u + index].Descriptor = {srv_registers[index], 21u,
            D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    }
    for (UINT index = 0; index < uav_registers.size(); ++index)
    {
        parameters[5u + index].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[5u + index].Descriptor = {uav_registers[index], 21u,
            D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    }
    for (auto &parameter : parameters) parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_description{};
    root_description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_description.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    root_description.Desc_1_1.pParameters = parameters.data();
    ComPtr<ID3DBlob> serialized, errors;
    Hr(D3D12SerializeVersionedRootSignature(&root_description, &serialized, &errors),
        "serialize production virtual shadow CullCS root");
    ComPtr<ID3D12RootSignature> root;
    Hr(fixture.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&root)), "create production virtual shadow CullCS root");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_description{};
    pipeline_description.pRootSignature = root.Get();
    pipeline_description.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;
    Hr(fixture.device->CreateComputePipelineState(&pipeline_description, IID_PPV_ARGS(&pipeline)),
        "create production virtual shadow CullCS pipeline");

    const auto create_upload = [&](const void *data, UINT64 bytes, const char *label) {
        auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const auto description = BufferDescription(std::max<UINT64>(bytes, 4u));
        ComPtr<ID3D12Resource> resource;
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource)), label);
        if (bytes)
        {
            void *mapped{}; const D3D12_RANGE no_read{0, 0};
            Hr(resource->Map(0, &no_read, &mapped), "map production CullCS upload");
            std::memcpy(mapped, data, static_cast<std::size_t>(bytes));
            resource->Unmap(0, nullptr);
        }
        return resource;
    };
    for (const auto &test : cases)
    {
        Check(test.segments.size() == test.group_count, "root segment fixture matches actual groups");
        const UINT command_words = test.fine_capacity + static_cast<UINT>(test.casters.size());
        const UINT root_counter_base = test.max_groups * 2u + 2u;
        hs::VirtualShadowConstants constants{};
        constants.counts = {static_cast<UINT>(test.casters.size()), 0u, test.fine_entries, 0u};
        constants.identity.w = 91u;
        constants.frame.w = test.group_count * 2u;
        constants.options.z = test.fine_capacity;
        constants.root_options = {1u, 1u, 1u, root_counter_base};

        std::array<std::byte, 512> constants_bytes{};
        std::memcpy(constants_bytes.data(), &constants, sizeof(constants));
        auto constant_upload = create_upload(constants_bytes.data(), constants_bytes.size(), "create CullCS constants");
        const UINT caster_bytes = static_cast<UINT>(test.casters.size() * sizeof(CasterInput));
        auto caster_upload = create_upload(test.casters.empty() ? nullptr : test.casters.data(), caster_bytes,
            "create CullCS caster input");
        const UINT segment_count = (constants.frame.w + test.group_count) * 2u;
        std::vector<UINT> segments(segment_count, sentinel);
        for (UINT group = 0; group < test.group_count; ++group)
        {
            const UINT word = (constants.frame.w + group) * 2u;
            segments[word] = test.segments[group].first;
            segments[word + 1u] = test.segments[group].capacity;
        }
        auto segment_upload = create_upload(segments.empty() ? nullptr : segments.data(),
            static_cast<UINT>(segments.size() * sizeof(UINT)), "create CullCS segment input");

        const UINT pages_bytes = (test.fine_entries + 1u) * static_cast<UINT>(sizeof(hs::GpuShadowPageEntry));
        const UINT counter_words = root_counter_base + test.max_groups;
        const UINT counters_bytes = counter_words * sizeof(UINT);
        const UINT records_bytes = std::max<UINT>(static_cast<UINT>(sizeof(hs::GpuShadowDrawRecord)),
            command_words * static_cast<UINT>(sizeof(hs::GpuShadowDrawRecord)));
        const UINT indexed_bytes = std::max<UINT>(24u, command_words * 24u);
        const UINT mesh_bytes = std::max<UINT>(16u, command_words * 16u);
        std::vector<std::byte> seed;
        const auto append_poison = [&](UINT bytes) {
            const auto offset = static_cast<UINT>(seed.size());
            const auto old_size = seed.size(); seed.resize(old_size + bytes);
            std::memset(seed.data() + old_size, 0xde, bytes);
            return offset;
        };
        const UINT pages_seed = append_poison(pages_bytes);
        const UINT counters_seed = append_poison(counters_bytes);
        const UINT records_seed = append_poison(records_bytes);
        const UINT indexed_seed = append_poison(indexed_bytes);
        const UINT mesh_seed = append_poison(mesh_bytes);
        auto seed_upload = create_upload(seed.data(), seed.size(), "create CullCS output seed");

        auto heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
        const UINT pages_read = 0u, counters_read = pages_read + pages_bytes;
        const UINT records_read = counters_read + counters_bytes, indexed_read = records_read + records_bytes;
        const UINT mesh_read = indexed_read + indexed_bytes, readback_bytes = mesh_read + mesh_bytes;
        const auto readback_description = BufferDescription(readback_bytes);
        ComPtr<ID3D12Resource> readback;
        Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create CullCS readback");

        hs::TransientResourcePool pool;
        Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "initialize CullCS transient pool");
        hs::RenderGraphBuilder graph;
        const auto caster_handle = graph.ImportBuffer({caster_upload.Get()}, hs::Access::ComputeRead, "CullCS.Casters");
        const auto segment_handle = graph.ImportBuffer({segment_upload.Get()}, hs::Access::ComputeRead, "CullCS.Segments");
        const auto constant_handle = graph.ImportBuffer({constant_upload.Get()}, hs::Access::ComputeRead, "CullCS.Constants");
        const auto seed_handle = graph.ImportBuffer({seed_upload.Get()}, hs::Access::CopySource, "CullCS.Seed");
        const auto pages_handle = graph.CreateBuffer({pages_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "CullCS.Pages");
        const auto counters_handle = graph.CreateBuffer({counters_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "CullCS.Counts");
        const auto records_handle = graph.CreateBuffer({records_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "CullCS.DrawRecords");
        const auto indexed_handle = graph.CreateBuffer({indexed_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "CullCS.Indexed");
        const auto mesh_handle = graph.CreateBuffer({mesh_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "CullCS.Mesh");
        auto seed_pass = graph.AddPass(360u, "CullCS initialize oracle outputs", hs::QueueHint::Copy);
        seed_pass.Read(graph.Version(seed_handle), hs::Access::CopySource);
        auto pages = seed_pass.Write(graph.Version(pages_handle), hs::Access::CopyDest);
        auto counters = seed_pass.Write(graph.Version(counters_handle), hs::Access::CopyDest);
        auto records = seed_pass.Write(graph.Version(records_handle), hs::Access::CopyDest);
        auto indexed = seed_pass.Write(graph.Version(indexed_handle), hs::Access::CopyDest);
        auto mesh = seed_pass.Write(graph.Version(mesh_handle), hs::Access::CopyDest);
        seed_pass.SetExecute([&](hs::RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            auto *source = static_cast<ID3D12Resource *>(context.Resolve(seed_handle));
            list->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(pages_handle)), 0, source, pages_seed, pages_bytes);
            list->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(counters_handle)), 0, source, counters_seed, counters_bytes);
            list->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(records_handle)), 0, source, records_seed, records_bytes);
            list->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(indexed_handle)), 0, source, indexed_seed, indexed_bytes);
            list->CopyBufferRegion(static_cast<ID3D12Resource *>(context.Resolve(mesh_handle)), 0, source, mesh_seed, mesh_bytes);
        });
        const auto bind = [&](hs::RenderPassContext &context, UINT phase) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            list->SetComputeRootSignature(root.Get()); list->SetPipelineState(pipeline.Get());
            list->SetComputeRootConstantBufferView(0u, constant_upload->GetGPUVirtualAddress());
            list->SetComputeRoot32BitConstant(1u, phase, 0u);
            list->SetComputeRootShaderResourceView(2u, caster_upload->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3u, segment_upload->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(4u, segment_upload->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(5u, static_cast<ID3D12Resource *>(context.Resolve(pages_handle))->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(6u, static_cast<ID3D12Resource *>(context.Resolve(counters_handle))->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(7u, static_cast<ID3D12Resource *>(context.Resolve(records_handle))->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(8u, static_cast<ID3D12Resource *>(context.Resolve(indexed_handle))->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(9u, static_cast<ID3D12Resource *>(context.Resolve(mesh_handle))->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(10u, static_cast<ID3D12Resource *>(context.Resolve(counters_handle))->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(11u, static_cast<ID3D12Resource *>(context.Resolve(counters_handle))->GetGPUVirtualAddress());
        };
        auto initialize = graph.AddPass(361u, "CullCS phase 3 root initialize", hs::QueueHint::Compute);
        initialize.Read(graph.Version(caster_handle), hs::Access::ComputeRead);
        initialize.Read(graph.Version(segment_handle), hs::Access::ComputeRead);
        initialize.Read(graph.Version(constant_handle), hs::Access::ComputeRead);
        pages = initialize.Write(pages, hs::Access::UnorderedWrite);
        counters = initialize.Write(counters, hs::Access::UnorderedWrite);
        records = initialize.Write(records, hs::Access::UnorderedWrite);
        indexed = initialize.Write(indexed, hs::Access::UnorderedWrite);
        mesh = initialize.Write(mesh, hs::Access::UnorderedWrite);
        initialize.SetExecute([&](hs::RenderPassContext &context) {
            bind(context, 3u);
            static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->Dispatch(1u, 1u, 1u);
        });
        if (!test.casters.empty())
        {
            auto emit = graph.AddPass(362u, "CullCS phase 4 root emit", hs::QueueHint::Compute);
            emit.Read(graph.Version(caster_handle), hs::Access::ComputeRead);
            emit.Read(graph.Version(segment_handle), hs::Access::ComputeRead);
            emit.Read(graph.Version(constant_handle), hs::Access::ComputeRead);
            pages = emit.ReadWrite(pages, hs::Access::UnorderedWrite);
            counters = emit.ReadWrite(counters, hs::Access::UnorderedWrite);
            records = emit.Write(records, hs::Access::UnorderedWrite);
            indexed = emit.Write(indexed, hs::Access::UnorderedWrite);
            mesh = emit.Write(mesh, hs::Access::UnorderedWrite);
            emit.SetExecute([&](hs::RenderPassContext &context) {
                auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                context.UavBarrier(pages_handle); context.UavBarrier(counters_handle);
                bind(context, 4u);
                list->Dispatch((static_cast<UINT>(test.casters.size()) + 63u) / 64u, 1u, 1u);
            });
        }
        auto copy = graph.AddPass(363u, "CullCS readback", hs::QueueHint::Copy);
        copy.Read(pages, hs::Access::CopySource); copy.Read(counters, hs::Access::CopySource);
        copy.Read(records, hs::Access::CopySource); copy.Read(indexed, hs::Access::CopySource); copy.Read(mesh, hs::Access::CopySource);
        copy.SetExecute([&](hs::RenderPassContext &context) {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            const std::array<std::pair<hs::ResourceHandle, UINT>, 5> sources{{
                {pages_handle, pages_read}, {counters_handle, counters_read}, {records_handle, records_read},
                {indexed_handle, indexed_read}, {mesh_handle, mesh_read}}};
            const std::array<UINT, 5> sizes{pages_bytes, counters_bytes, records_bytes, indexed_bytes, mesh_bytes};
            for (std::size_t index = 0; index < sources.size(); ++index)
                list->CopyBufferRegion(readback.Get(), sources[index].second,
                    static_cast<ID3D12Resource *>(context.Resolve(sources[index].first)), 0, sizes[index]);
        });
        Check(graph.Prepare(pool).Succeeded(), "prepare production CullCS root oracle");
        hs::CompletionSet completion;
        fixture.Execute(graph, frame_slot, barriers, completion, "execute production CullCS root oracle");
        Check(fixture.executor.Wait(completion).Succeeded(), "wait production CullCS root oracle");

        const UINT page_index = test.fine_entries;
        const UINT root_flags = 16u;
        std::vector<UINT> expected_counts(counter_words, sentinel);
        std::vector<std::array<UINT, 8>> expected_records(command_words);
        for (auto &record : expected_records) record.fill(sentinel);
        std::vector<std::vector<std::array<UINT, 6>>> expected_indexed(test.group_count);
        std::vector<std::vector<std::array<UINT, 4>>> expected_mesh(test.group_count);
        std::vector<UINT> group_attempts(test.group_count);
        bool expected_overflow{};
        for (UINT caster_id = 0; caster_id < test.casters.size(); ++caster_id)
        {
            const auto &caster = test.casters[caster_id];
            const UINT flags = caster.properties[0];
            if ((flags & 1u) == 0u || (flags & 68u) == 68u) continue;
            const UINT group = caster.indexed[0];
            if (group >= test.group_count) { expected_overflow = true; continue; }
            const UINT slot = group_attempts[group]++;
            const auto layout = test.segments[group];
            if (slot >= layout.capacity || layout.first >= test.casters.size() ||
                slot >= test.casters.size() - layout.first)
            { expected_overflow = true; continue; }
            const UINT record_index = test.fine_capacity + caster_id;
            expected_records[record_index] = {caster.draw[3], caster.draw[2], caster.draw[0], caster.draw[1],
                page_index, 2u, group, 0u};
            const UINT command_pass = 0x80000000u | record_index;
            if (caster.properties[2] == 0u)
                expected_indexed[group].push_back({command_pass, caster.indexed[1], 1u, caster.indexed[2], caster.indexed[3], 0u});
            else
                expected_mesh[group].push_back({command_pass, caster.properties[1], 1u, 1u});
        }
        for (UINT group = 0; group < test.group_count; ++group)
            expected_counts[root_counter_base + group] = group_attempts[group];

        void *mapped{}; const D3D12_RANGE read{0, readback_bytes};
        Hr(readback->Map(0, &read, &mapped), "map production CullCS root results");
        const auto *result = static_cast<const std::byte *>(mapped);
        const auto compare_words = [&](UINT offset, const std::vector<UINT> &expected, const char *label) {
            const auto *actual = reinterpret_cast<const UINT *>(result + offset);
            Check(std::equal(expected.begin(), expected.end(), actual), label);
        };
        std::vector<UINT> expected_pages((test.fine_entries + 1u) * 8u, sentinel);
        expected_pages[page_index * 8u + 0u] = 0u; expected_pages[page_index * 8u + 1u] = 0u;
        expected_pages[page_index * 8u + 2u] = 3u; expected_pages[page_index * 8u + 3u] = UINT_MAX;
        expected_pages[page_index * 8u + 4u] = constants.identity.w;
        expected_pages[page_index * 8u + 5u] = root_flags | (expected_overflow ? 8u : 0u);
        expected_pages[page_index * 8u + 6u] = 0u; expected_pages[page_index * 8u + 7u] = 0u;
        compare_words(pages_read, expected_pages, "production CullCS root page initialization/overflow mismatch");
        const auto actual_root_flags = reinterpret_cast<const UINT *>(result + pages_read)[page_index * 8u + 5u];
        Check(((actual_root_flags & 16u) != 0u && (actual_root_flags & 8u) == 0u) == !expected_overflow,
            "production CullCS root lookup must accept only published non-overflow pages");
        compare_words(counters_read, expected_counts, "production CullCS root counters or fine-prefix sentinel mismatch");

        std::vector<UINT> expected_record_words(command_words * 8u, sentinel);
        for (UINT index = 0; index < command_words; ++index)
            std::copy(expected_records[index].begin(), expected_records[index].end(), expected_record_words.begin() + index * 8u);
        compare_words(records_read, expected_record_words, "production CullCS record-address/data mismatch");

        std::vector<UINT> expected_indexed_words(indexed_bytes / 4u, sentinel);
        std::vector<UINT> expected_mesh_words(mesh_bytes / 4u, sentinel);
        for (UINT group = 0; group < test.group_count; ++group)
        {
            const auto layout = test.segments[group];
            const bool mesh_frontend = test.mesh_frontends[group];
            if (mesh_frontend)
            {
                auto &expected = expected_mesh[group];
                const UINT actual_count = static_cast<UINT>(expected.size());
                std::vector<std::array<UINT, 4>> actual;
                for (UINT slot = 0; slot < actual_count; ++slot)
                {
                    std::array<UINT, 4> command{};
                    std::memcpy(command.data(), result + mesh_read + (test.fine_capacity + layout.first + slot) * 16u, 16u);
                    actual.push_back(command);
                }
                auto sorted_actual = actual; auto sorted_expected = expected;
                std::ranges::sort(sorted_actual); std::ranges::sort(sorted_expected);
                Check(sorted_actual == sorted_expected, "production CullCS mesh command set differs from CPU reference");
                for (UINT slot = actual_count; slot < layout.capacity; ++slot)
                {
                    const UINT offset = (test.fine_capacity + layout.first + slot) * 4u;
                    for (UINT word = 0; word < 4u; ++word)
                        Check(reinterpret_cast<const UINT *>(result + mesh_read)[offset + word] == sentinel,
                            "production CullCS mesh overflow wrote beyond its segment");
                }
                for (UINT slot = 0; slot < actual_count; ++slot)
                    std::copy(actual[slot].begin(), actual[slot].end(), expected_mesh_words.begin() +
                        (test.fine_capacity + layout.first + slot) * 4u);
            }
            else
            {
                auto &expected = expected_indexed[group];
                const UINT actual_count = static_cast<UINT>(expected.size());
                std::vector<std::array<UINT, 6>> actual;
                for (UINT slot = 0; slot < actual_count; ++slot)
                {
                    std::array<UINT, 6> command{};
                    std::memcpy(command.data(), result + indexed_read + (test.fine_capacity + layout.first + slot) * 24u, 24u);
                    actual.push_back(command);
                }
                auto sorted_actual = actual; auto sorted_expected = expected;
                std::ranges::sort(sorted_actual); std::ranges::sort(sorted_expected);
                Check(sorted_actual == sorted_expected, "production CullCS indexed command set differs from CPU reference");
                for (UINT slot = actual_count; slot < layout.capacity; ++slot)
                {
                    const UINT offset = (test.fine_capacity + layout.first + slot) * 4u;
                    for (UINT word = 0; word < 6u; ++word)
                        Check(reinterpret_cast<const UINT *>(result + indexed_read)[offset + word] == sentinel,
                            "production CullCS indexed overflow wrote beyond its segment");
                }
                for (UINT slot = 0; slot < actual_count; ++slot)
                    std::copy(actual[slot].begin(), actual[slot].end(), expected_indexed_words.begin() +
                        (test.fine_capacity + layout.first + slot) * 6u);
            }
        }
        compare_words(indexed_read, expected_indexed_words, "production CullCS changed indexed fine prefix or bytes outside root segments");
        compare_words(mesh_read, expected_mesh_words, "production CullCS changed mesh fine prefix or bytes outside root segments");
        readback->Unmap(0, nullptr);
        fixture.CheckValidation();
        pool.ResetCompletedClaims(fixture.executor.Completed());
        std::printf("Production VSM CullCS root scatter %s/%s passed (%s, %u casters, %u groups).\n",
            barriers == hs::BarrierMode::Enhanced ? "Enhanced" : "Legacy", test.name,
            test.casters.empty() ? "empty" : expected_overflow ? "overflow guarded" : "capacity exact",
            static_cast<UINT>(test.casters.size()), test.group_count);
    }
}


void RunInstanceIdOracle(Fixture &fixture, hs::BarrierMode barriers, std::uint32_t frame_slot)
{
    struct Compiler
    {
        HMODULE module{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        ~Compiler() { if (module) FreeLibrary(module); }
    } compiler;
    Check(compiler.module != nullptr, "load instance ID oracle compiler");
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.module, "D3DCompile"));
    Check(compile != nullptr, "find instance ID oracle compiler");
    const auto source_path = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "Content/Shaders/local_probe_draw.hlsli";
    std::ifstream source(source_path, std::ios::binary);
    Check(source.good(), "open production local-probe source decoder");
    std::string shader{std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>()};
    shader += R"(
RWByteAddressBuffer Output : register(u0);
cbuffer Case : register(b0) { uint CaseIndex, PassValue, Mode; };
float4 Main(uint instance : SV_InstanceID) : SV_Position
{
    if(Mode == 0u)
    {
        uint ignored;
        Output.InterlockedOr(CaseIndex * 8u, 1u << instance, ignored);
        Output.InterlockedOr(CaseIndex * 8u + 4u, 1u << (PassValue + instance), ignored);
    }
    else
        Output.Store3(16u + (CaseIndex + instance) * 12u, uint3(instance,
            HsLocalProbeSourceSlot(PassValue,instance), 0x10000000u + HsLocalProbeMaterialOrdinal(PassValue)));
    return float4(0,0,0,1);
}
)";
    ComPtr<ID3DBlob> code_blob, diagnostics, serialized, errors;
    const auto compiled = compile(shader.data(), shader.size(), "NativeInstanceIdOracle", nullptr, nullptr,
        "Main", "vs_5_1", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS |
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code_blob, &diagnostics);
    if (FAILED(compiled) && diagnostics)
        std::fprintf(stderr, "%.*s\n", static_cast<int>(diagnostics->GetBufferSize()),
            static_cast<const char *>(diagnostics->GetBufferPointer()));
    Hr(compiled, "compile instance ID oracle");
    std::array<D3D12_ROOT_PARAMETER1, 2> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 3};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_description{};
    root_description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_description.Desc_1_1.NumParameters = static_cast<UINT>(parameters.size());
    root_description.Desc_1_1.pParameters = parameters.data();
    root_description.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    Hr(D3D12SerializeVersionedRootSignature(&root_description, &serialized, &errors),
        "serialize instance ID root signature");
    ComPtr<ID3D12RootSignature> root;
    Hr(fixture.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&root)), "create instance ID root signature");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature = root.Get();
    description.VS = {code_blob->GetBufferPointer(), code_blob->GetBufferSize()};
    description.SampleMask = UINT_MAX;
    description.SampleDesc.Count = 1;
    description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable = TRUE;
    description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    description.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    description.DepthStencilState.BackFace = description.DepthStencilState.FrontFace;
    for (auto &blend : description.BlendState.RenderTarget)
    {
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    ComPtr<ID3D12PipelineState> pipeline;
    Hr(fixture.device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline)),
        "create instance ID graphics pipeline");
    constexpr std::array<hs::LocalProbeDrawRun, 5> runs{{
        {11,2,3}, {14,1,3}, {17,2,6}, {21,2,3}, {hs::kLocalProbeSourceCapacity-3u,3,14}}};
    constexpr UINT record_count = 10;
    constexpr UINT value_count = 4 + record_count * 3;
    constexpr UINT bytes = value_count * sizeof(UINT);
    auto heap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const auto upload_description = BufferDescription(bytes + sizeof(UINT));
    ComPtr<ID3D12Resource> upload, readback;
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &upload_description,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "create instance ID upload");
    heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const auto readback_description = BufferDescription(bytes);
    Hr(fixture.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_description,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "create instance ID readback");
    void *mapped{};
    const D3D12_RANGE no_read{0, 0};
    Hr(upload->Map(0, &no_read, &mapped), "map instance ID initialization");
    std::memset(mapped, 0, bytes + sizeof(UINT));
    upload->Unmap(0, nullptr);
    hs::TransientResourcePool pool;
    Check(pool.Initialize(fixture.allocator, fixture.device.Get()).Succeeded(), "instance ID pool");
    hs::RenderGraphBuilder graph;
    const auto output = graph.CreateBuffer({bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS}, "native instance IDs");
    auto seed = graph.AddPass(340, "Instance ID initialize", hs::QueueHint::Copy);
    const auto initialized = seed.Write(graph.Version(output), hs::Access::CopyDest);
    seed.SetExecute([&](hs::RenderPassContext &context) {
        static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(
            static_cast<ID3D12Resource *>(context.Resolve(output)), 0, upload.Get(), 0, bytes);
    });
    auto draw = graph.AddPass(341, "Instance ID graphics oracle", hs::QueueHint::Direct);
    const auto written = draw.ReadWrite(initialized, hs::Access::UnorderedWrite);
    draw.SetExecute([&](hs::RenderPassContext &context) {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        list->SetGraphicsRootSignature(root.Get());
        list->SetPipelineState(pipeline.Get());
        list->SetGraphicsRootUnorderedAccessView(0,
            static_cast<ID3D12Resource *>(context.Resolve(output))->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        const D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        const D3D12_RECT scissor{0, 0, 1, 1};
        list->RSSetViewports(1, &viewport);list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        const UINT first[]{0, 11, 0};
        list->SetGraphicsRoot32BitConstants(1, 3, first, 0);
        list->DrawInstanced(1, 3, 0, 5);
        const UINT second[]{1, 14, 0};
        list->SetGraphicsRoot32BitConstants(1, 3, second, 0);
        const D3D12_INDEX_BUFFER_VIEW indices{upload->GetGPUVirtualAddress() + bytes, sizeof(UINT), DXGI_FORMAT_R32_UINT};
        list->IASetIndexBuffer(&indices);
        list->DrawIndexedInstanced(1, 3, 0, 0, 9);
        UINT offset=0;
        for(const auto &run:runs)
        {
            Check(hs::LocalProbeDrawRunValid(run,hs::kLocalProbeSourceCapacity),"native packed run bounds");
            const UINT constants[]{offset,hs::LocalProbePassValue(run),1};
            list->SetGraphicsRoot32BitConstants(1,3,constants,0);
            list->DrawIndexedInstanced(1,run.instance_count,0,0,0);
            offset+=run.instance_count;
        }
    });
    auto copy = graph.AddPass(342, "Instance ID readback", hs::QueueHint::Copy);
    copy.Read(written, hs::Access::CopySource);
    copy.SetExecute([&](hs::RenderPassContext &context) {
        static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList())->CopyBufferRegion(readback.Get(), 0,
            static_cast<ID3D12Resource *>(context.Resolve(output)), 0, bytes);
    });
    Check(graph.Prepare(pool).Succeeded(), "prepare instance ID oracle");
    hs::CompletionSet completion;
    fixture.Execute(graph, frame_slot, barriers, completion, "execute instance ID oracle");
    Check(fixture.executor.Wait(completion).Succeeded(), "wait instance ID oracle");
    const D3D12_RANGE read{0, bytes};
    Hr(readback->Map(0, &read, &mapped), "map instance ID results");
    std::array<UINT, value_count> values{};
    std::memcpy(values.data(), mapped, bytes);
    readback->Unmap(0, &no_read);
    std::printf("Native SV_InstanceID %s: DrawInstanced(start=5) raw=%08x explicit11=%08x; "
        "DrawIndexedInstanced(start=9) raw=%08x explicit14=%08x.\n",
        barriers == hs::BarrierMode::Enhanced ? "Enhanced" : "Legacy",
        values[0], values[1], values[2], values[3]);
    Check(values[0] == 7u && values[2] == 7u, "SV_InstanceID draw-local native contract differs from expected IDs 0,1,2");
    Check(values[1] == (7u << 11u) && values[3] == (7u << 14u),
        "explicit base plus draw-local instance ID did not preserve source slots");
    UINT offset=4;
    for(const auto &run:runs)
        for(UINT local=0;local<run.instance_count;++local)
        {
            Check(values[offset++]==local,"native packed source helper used a nonlocal instance ID");
            Check(values[offset++]==run.first_instance+local,"native packed source helper lost a source gap or base");
            Check(values[offset++]==0x10000000u+run.material_ordinal,"native packed source helper changed material ordinal");
        }
    Check(offset==value_count,"native packed source helper record count");
    std::printf("Local probe production source decoder %s passed (10 exact source/material records, gaps, ordinal changes and maximum base).\n",
        barriers == hs::BarrierMode::Enhanced ? "Enhanced" : "Legacy");
    fixture.CheckValidation();
    pool.ResetCompletedClaims(fixture.executor.Completed());
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
        Check(alias_lists[0] != alias_lists[1] && alias_lists[1] != alias_lists[2] &&
                  alias_lists[0] != alias_lists[2],
              "Direct scopes retain independent command lists in both barrier modes");
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
        RunBakedProbePreparation(fixture);
        RunDeviceIdentity(fixture, hardware);
        RunRemovedFenceSentinel(fixture);
        RunQueueJoinReuse(fixture);
        RunRecordingDispatch(fixture, hs::BarrierMode::Legacy);
        RunFinalExports(fixture, hs::BarrierMode::Legacy);
        Run(fixture, hs::BarrierMode::Legacy, 0);
        RunExternal(fixture, hs::BarrierMode::Legacy, 0);
        RunPlanReuse(fixture, hs::BarrierMode::Legacy, 0);
        RunUavClear(fixture, hs::BarrierMode::Legacy, 0);
        RunBatchedBufferBarriers(fixture, hs::BarrierMode::Legacy, 0);
        RunVirtualShadowFeedbackOracle(fixture, hs::BarrierMode::Legacy, 0);
        RunVirtualShadowRootScatterOracle(fixture, hs::BarrierMode::Legacy, 0);
        RunInstanceIdOracle(fixture, hs::BarrierMode::Legacy, 0);
        RunSharedTextureReaders(fixture, hs::BarrierMode::Legacy, 0);
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
            RunBatchedBufferBarriers(fixture, hs::BarrierMode::Enhanced, 1);
            RunVirtualShadowFeedbackOracle(fixture, hs::BarrierMode::Enhanced, 1);
            RunVirtualShadowRootScatterOracle(fixture, hs::BarrierMode::Enhanced, 1);
            RunInstanceIdOracle(fixture, hs::BarrierMode::Enhanced, 1);
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
