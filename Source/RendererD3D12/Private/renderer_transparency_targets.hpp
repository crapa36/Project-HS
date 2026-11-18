#pragma once

#include "renderer_transparency.hpp"
#include "d3d12_resources.hpp"
#if defined(HS_DEVELOPMENT_TOOLS)
#include "renderer_translucency_diagnostics.hpp"
#endif

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

namespace hs
{
namespace translucency_detail
{
inline constexpr std::uint32_t kTailPhase = 5, kCompositePhase = 6;
inline constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr std::array<std::uint32_t, 4> kCompositeGuideIndices{1, 2, 3, 5};
static_assert(1 + kCompositeGuideIndices.size() <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);

Result Failure(ErrorCode code, std::string message);
bool ValidExtent(ViewExtent extent);
void WriteTextureSrv(ID3D12Device *device, ID3D12Resource *resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle);
} // namespace translucency_detail

struct LayeredTranslucencyTargets
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<D3D12MA::Allocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 5> pipelines;
#if defined(HS_DEVELOPMENT_TOOLS)
    std::unique_ptr<TranslucencyDiagnosticsCapture> work;
#endif
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> tables, rtv_heap, dsv_heap, exports;
    std::array<AllocationResource, 4> front, identity, optical_component;
    AllocationResource exit;
    DXGI_FORMAT exit_format{};
    std::array<AllocationResource, 6> guides;
    std::array<AllocationResource, 2> color;
    // One result resource per slot/layer keeps a front query's CopyDest write
    // separate from the preceding layer's predication read in the same pass.
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> occupancy_queries;
    std::vector<std::array<AllocationResource, kLayeredTranslucencyMaxLayers>> occupancy_results;
    AllocationResource constants;
    ViewExtent capacity;
    std::uint64_t generation{};
    bool enabled{}, usable{true};
    std::uint32_t layer_count{}, frame_slot_count{}, srv_stride{}, rtv_stride{}, dsv_stride{};
    std::vector<CompletionSet> slot_completion;
    CompletionSet completion;
    std::uint64_t bytes{};

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t phase, std::uint32_t offset) const
    {
        auto handle = tables->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(phase * kLayeredTranslucencyTableSize + offset) * srv_stride;
        return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(std::uint32_t offset) const
    {
        auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(offset) * rtv_stride; return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE GuideRtv(std::uint32_t index) const
    {
        return Rtv(index == 5 ? 12 : 8 + index);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(std::uint32_t offset) const
    {
        auto handle = dsv_heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(offset) * dsv_stride; return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Export(std::uint32_t offset) const
    {
        auto handle = exports->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(offset) * srv_stride; return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(const LayeredTranslucencyInputs &inputs, std::uint32_t phase,
        std::uint32_t offset = 0) const
    {
        auto handle = inputs.table_gpu;
        handle.ptr += UINT64(phase * kLayeredTranslucencyTableSize + offset) * inputs.descriptor_stride;
        return handle;
    }
    void InitializeTable(std::byte *mapped, std::uint32_t phase, std::uint32_t layer, LayeredTranslucencyPhase semantic);
};

struct TranslucencySystem::Impl
{
    std::uint64_t next_generation{};
    std::shared_ptr<LayeredTranslucencyTargets> targets;
#if defined(HS_DEVELOPMENT_TOOLS)
    std::vector<RendererTranslucencyDiagnosticsSample> completed_work;
    RendererTranslucencyDiagnosticsStatus retired_work;
    std::uint32_t diagnostic_color_index{};
    bool diagnostic_submission_valid{};
#endif
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<LayeredTranslucencyTargets> targets;
        LayeredTranslucencyInputs inputs;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> shader_heap;
        std::vector<LayeredTranslucencySceneRead> scene_reads;
        std::vector<Import> imports;
        std::array<ResourceVersion, 4> front, identity, optical_component;
        ResourceVersion exit;
        std::array<ResourceVersion, kLayeredTranslucencyMaxLayers> occupancy;
        std::array<ResourceVersion, 2> color;
        std::array<ResourceVersion, 6> guides;
        ResourceVersion revealage;
        bool composed{};
    } pending;
};
} // namespace hs
