#include "renderer_transparency.hpp"
#include "d3d12_resources.hpp"
#include "renderer_pipeline_cache.hpp"
#if defined(HS_DEVELOPMENT_TOOLS)
#include "renderer_hero_work.hpp"
#endif

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kConstantStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr std::uint32_t kTailPhase = 8, kCompositePhase = 10;
constexpr std::uint32_t kUavOffset = 22;
constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr std::array<DXGI_FORMAT, 6> kGuideFormats{
    DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
    kColor, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R16G16_FLOAT};
constexpr std::array<std::uint32_t, 4> kCompositeGuideIndices{1, 2, 3, 5};
static_assert(1 + kCompositeGuideIndices.size() <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
constexpr std::array<std::string_view, 5> kPixelProducts{
    "render/shader/hero_peel_front_ps.dxil", "render/shader/hero_peel_exit_ps.dxil",
    "render/shader/hero_oit_tail_ps.dxil", "render/shader/hero_transparent_guide_ps.dxil",
    "render/shader/hero_layer_composite_ps.dxil"};
#if defined(HS_DEVELOPMENT_TOOLS)
constexpr std::array<std::string_view, 5> kWorkPixelProducts{
    "render/shader/hero_peel_front_ps_hero_work.dxil", "render/shader/hero_peel_exit_ps_hero_work.dxil",
    "render/shader/hero_oit_tail_ps_hero_work.dxil", "render/shader/hero_transparent_guide_ps_hero_work.dxil",
    "render/shader/hero_layer_composite_ps_hero_work.dxil"};
#endif

Result Failure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_transparency", std::move(message));
}
Result NativeFailure(std::string_view operation, HRESULT error)
{
    return Failure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool Fits(const HeroTransparencyGuide &guide, ViewExtent active, DXGI_FORMAT format)
{
    if (!guide.resource || !guide.version.resource || guide.version.resource.kind != ResourceKind::Texture)
        return false;
    const auto desc = guide.resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.Width >= active.width && desc.Height >= active.height && desc.Format == format;
}
bool FitsDepth(const HeroTransparencyGuide &guide, ViewExtent active)
{
    return Fits(guide, active, DXGI_FORMAT_R32_TYPELESS) || Fits(guide, active, DXGI_FORMAT_D32_FLOAT) ||
        Fits(guide, active, DXGI_FORMAT_R32_FLOAT);
}
Result CreateTexture(D3D12MA::Allocator *allocator, ViewExtent extent, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags, AllocationResource &texture)
{
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = extent.width; desc.Height = extent.height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = format;
    desc.SampleDesc.Count = 1; desc.Flags = flags;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ? DXGI_FORMAT_D32_FLOAT : format;
    const auto native = allocator->CreateResource(&allocation, &desc, D3D12_RESOURCE_STATE_COMMON,
        flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS ? nullptr : &clear,
        &texture.allocation, IID_PPV_ARGS(texture.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create hero transparency texture", native);
    texture.graph_access = Access::Common;
    return Result::Success();
}
Result CheckFormat(ID3D12Device *device, DXGI_FORMAT format, bool depth, bool counter)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    const auto native = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(native)) return NativeFailure("Check hero transparency format", native);
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
        (!depth && !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD)) ||
        (depth && !(support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) ||
        (!depth && !counter && !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)) ||
        (counter && (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) ||
                     !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD))))
        return Failure(ErrorCode::TaskFailed, "Hero transparency texture format lacks required native access support.");
    return Result::Success();
}
} // namespace

struct HeroTransparencyTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 5> pipelines;
#if defined(HS_DEVELOPMENT_TOOLS)
    std::unique_ptr<HeroWorkCapture> work;
#endif
    ComPtr<ID3D12DescriptorHeap> tables, rtv_heap, dsv_heap, exports;
    std::array<AllocationResource, 4> front, identity, exit;
    std::array<AllocationResource, 6> guides;
    std::array<AllocationResource, 2> color;
    // One result resource per slot/layer keeps a front query's CopyDest write
    // separate from the preceding layer's predication read in the same pass.
    ComPtr<ID3D12QueryHeap> occupancy_queries;
    std::vector<std::array<AllocationResource, kHeroTransparencyMaxLayers>> occupancy_results;
    AllocationResource overflow, constants;
    std::byte *mapped{};
    ViewExtent capacity;
    std::uint64_t generation{};
    bool enabled{}, usable{true};
    std::uint32_t layer_count{}, frame_slot_count{}, srv_stride{}, rtv_stride{}, dsv_stride{};
    std::vector<CompletionSet> slot_completion;
    CompletionSet completion;
    std::uint64_t bytes{};

    ~HeroTransparencyTargets() { if (mapped && constants.resource) constants.resource->Unmap(0, nullptr); }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot, std::uint32_t phase, std::uint32_t offset) const
    {
        auto handle = tables->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T((slot * kHeroTransparencyPhaseCount + phase) * kHeroTransparencyTableSize + offset) * srv_stride;
        return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(std::uint32_t offset) const
    {
        auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(offset) * rtv_stride; return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(std::uint32_t offset) const
    {
        auto handle = dsv_heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(offset) * dsv_stride; return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Export(std::uint32_t offset) const
    {
        auto handle = exports->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(offset) * srv_stride; return handle;
    }
    void Srv(ID3D12Resource *resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &view, handle);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(const HeroTransparencyInputs &inputs, std::uint32_t phase,
        std::uint32_t offset = 0) const
    {
        auto handle = inputs.table_gpu;
        handle.ptr += UINT64(phase * kHeroTransparencyTableSize + offset) * inputs.descriptor_stride;
        return handle;
    }
    void InitializeTable(std::uint32_t slot, std::uint32_t phase)
    {
        const auto constantSlot = slot * kHeroTransparencyPhaseCount + phase;
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{constants.resource->GetGPUVirtualAddress() + UINT64(constantSlot) * kConstantStride,
                                          kConstantStride};
        device->CreateConstantBufferView(&cbv, Cpu(slot, phase, 0));
        for (std::uint32_t index = 0; index < kHeroTransparencyMaxLayers; ++index)
        {
            Srv(front[index].resource.Get(), DXGI_FORMAT_R32_FLOAT, Cpu(slot, phase, 1 + index));
            Srv(identity[index].resource.Get(), DXGI_FORMAT_R32G32_UINT, Cpu(slot, phase, 5 + index));
            Srv(exit[index].resource.Get(), DXGI_FORMAT_R32_FLOAT, Cpu(slot, phase, 9 + index));
        }
        Srv(overflow.resource.Get(), DXGI_FORMAT_R32_UINT, Cpu(slot, phase, 14));
        Srv(front[0].resource.Get(), DXGI_FORMAT_R32_FLOAT, Cpu(slot, phase, 15));
        for (std::uint32_t index = 1; index < 4; ++index)
            Srv(guides[index].resource.Get(), kGuideFormats[index], Cpu(slot, phase, 15 + index));
        Srv(identity[0].resource.Get(), DXGI_FORMAT_R32G32_UINT, Cpu(slot, phase, 19));
        Srv(overflow.resource.Get(), DXGI_FORMAT_R32_UINT, Cpu(slot, phase, 20));
        Srv(guides[5].resource.Get(), kGuideFormats[5], Cpu(slot, phase, 21));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_UINT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(overflow.resource.Get(), nullptr, &uav, Cpu(slot, phase, kUavOffset));
    }
    void FillTable(const HeroTransparencyInputs &inputs, std::uint32_t phase, std::uint32_t layer,
        HeroTransparencyPhase semantic, ID3D12Resource *background)
    {
        const auto constantSlot = inputs.frame_slot * kHeroTransparencyPhaseCount + phase;
        const std::array<std::uint32_t, 4> values{layer, layer_count, static_cast<std::uint32_t>(semantic), 0};
        std::memcpy(mapped + SIZE_T(constantSlot) * kConstantStride, values.data(), sizeof(values));
        Srv(background, kColor, Cpu(inputs.frame_slot, phase, 13));
        auto destination = inputs.table_cpu;
        destination.ptr += SIZE_T(phase * kHeroTransparencyTableSize) * inputs.descriptor_stride;
        device->CopyDescriptorsSimple(kHeroTransparencyTableSize, destination,
            Cpu(inputs.frame_slot, phase, 0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
};

struct HeroTransparencySystem::Impl
{
    std::uint64_t next_generation{};
    std::shared_ptr<HeroTransparencyTargets> targets;
#if defined(HS_DEVELOPMENT_TOOLS)
    std::vector<RendererHeroWorkSample> completed_work;
    RendererHeroWorkStatus retired_work;
#endif
    struct Import { AllocationResource *allocation{}; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<HeroTransparencyTargets> targets;
        HeroTransparencyInputs inputs;
        ComPtr<ID3D12DescriptorHeap> shader_heap;
        std::vector<HeroTransparencySceneRead> scene_reads;
        std::vector<Import> imports;
        std::array<ResourceVersion, 4> front, identity, exit;
        std::array<ResourceVersion, kHeroTransparencyMaxLayers> occupancy;
        std::array<ResourceVersion, 2> color;
        std::array<ResourceVersion, 6> guides;
        ResourceVersion overflow;
        bool composed{};
    } pending;
};

HeroTransparencySystem::HeroTransparencySystem() : impl_(std::make_unique<Impl>()) {}
HeroTransparencySystem::~HeroTransparencySystem() = default;
std::uint64_t HeroTransparencyCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t HeroTransparencySystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }

Result HeroTransparencySystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ID3D12RootSignature *root, ViewExtent capacity, std::uint32_t layerCount, bool enabled,
    std::uint32_t frameSlots, const ShaderReader &readShader, RendererPipelineCache &pipelineCache,
    HeroTransparencyCandidate &candidate, bool captureHeroWork, HeroWorkDiagnosticMode mode) const
{
    if (!device || !allocator || !root || !ValidExtent(capacity) || layerCount > 4 || (enabled && !layerCount) || !frameSlots ||
        frameSlots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / kHeroTransparencyDescriptorsPerFrame ||
        (enabled && !readShader))
        return Failure(ErrorCode::InvalidArgument, "Invalid hero transparency device, root, extent, quality, frame slots or shader reader.");
    auto next = std::make_shared<HeroTransparencyTargets>();
    next->device = device; next->allocator = allocator; next->root = root;
    next->capacity = capacity; next->enabled = enabled; next->layer_count = layerCount;
    next->frame_slot_count = frameSlots; next->slot_completion.resize(frameSlots);
    if (!enabled) { candidate.targets_ = std::move(next); return Result::Success(); }
#if defined(HS_DEVELOPMENT_TOOLS)
    if (captureHeroWork)
    {
        next->work = std::make_unique<HeroWorkCapture>();
        if (auto prepared = next->work->Initialize(device, allocator, frameSlots, capacity, mode); !prepared) return prepared;
    }
    else
#else
    static_cast<void>(captureHeroWork);
    static_cast<void>(mode);
#endif
    {
        D3D12_QUERY_HEAP_DESC queries{};
        queries.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        queries.Count = frameSlots * kHeroTransparencyMaxLayers;
        const auto native = device->CreateQueryHeap(&queries, IID_PPV_ARGS(&next->occupancy_queries));
        if (FAILED(native)) return NativeFailure("Create hero occupancy queries", native);
        next->occupancy_results.resize(frameSlots);
        for (auto &slot : next->occupancy_results)
            for (std::uint32_t layer = 0; layer < layerCount; ++layer)
            {
                D3D12MA::ALLOCATION_DESC allocation{};
                allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC buffer{};
                buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buffer.Width = sizeof(std::uint64_t);
                buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
                buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                auto &result = slot[layer];
                const auto created = allocator->CreateResource(&allocation, &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, &result.allocation,
                    IID_PPV_ARGS(result.resource.ReleaseAndGetAddressOf()));
                if (FAILED(created)) return NativeFailure("Create hero occupancy result", created);
                result.graph_access = Access::CopyDest;
                next->bytes += result.allocation->GetSize();
            }
    }
    if (auto checked = CheckFormat(device, DXGI_FORMAT_D32_FLOAT, true, false); !checked) return checked;
    if (auto checked = CheckFormat(device, DXGI_FORMAT_R32G32_UINT, false, false); !checked) return checked;
    if (auto checked = CheckFormat(device, DXGI_FORMAT_R32_UINT, false, true); !checked) return checked;
    if (auto checked = CheckFormat(device, kColor, false, false); !checked) return checked;
    for (const auto format : kGuideFormats)
        if (auto checked = CheckFormat(device, format, false, false); !checked) return checked;
    for (std::uint32_t layer = 0; layer < next->layer_count; ++layer)
    {
        if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R32_TYPELESS,
                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, next->front[layer]); !created) return created;
        if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R32G32_UINT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, next->identity[layer]); !created) return created;
        if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R32_TYPELESS,
                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, next->exit[layer]); !created) return created;
    }
    for (std::uint32_t index = 1; index < next->guides.size(); ++index)
    {
        if (index == 4) continue;
        if (auto created = CreateTexture(allocator, capacity, kGuideFormats[index],
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, next->guides[index]); !created) return created;
    }
    for (auto &color : next->color)
        if (auto created = CreateTexture(allocator, capacity, kColor,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, color); !created) return created;
    if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R32_UINT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, next->overflow); !created) return created;

    std::vector<std::byte> vertex;
    if (auto read = readShader("render/shader/hero_transparency_vs.dxil", vertex); !read) return read;
    if (vertex.empty()) return Failure(ErrorCode::TaskFailed, "Hero transparency indexed vertex shader is empty.");
    std::vector<std::byte> current_vertex;
    if (auto read = readShader("render/shader/hero_transparency_current_vs.dxil", current_vertex); !read) return read;
    if (current_vertex.empty()) return Failure(ErrorCode::TaskFailed, "Hero current-pose vertex shader is empty.");
    constexpr D3D12_INPUT_ELEMENT_DESC layout[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"MATERIAL", 0, DXGI_FORMAT_R16_UINT, 0, 72, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    for (std::uint32_t index = 0; index < kPixelProducts.size(); ++index)
    {
#if defined(HS_DEVELOPMENT_TOOLS)
        const auto product = captureHeroWork ? kWorkPixelProducts[index] : kPixelProducts[index];
#else
        const auto product = kPixelProducts[index];
#endif
        std::vector<std::byte> pixel;
        if (auto read = readShader(product, pixel); !read) return read;
        if (pixel.empty()) return Failure(ErrorCode::TaskFailed, "Hero transparency pixel product is empty: " + std::string(product));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        const auto &phase_vertex = index == 3 ? vertex : current_vertex;
        pipeline.pRootSignature = root; pipeline.VS = {phase_vertex.data(), phase_vertex.size()}; pipeline.PS = {pixel.data(), pixel.size()};
        pipeline.SampleMask = UINT_MAX; pipeline.SampleDesc.Count = 1;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // Exit selects back faces; every other phase selects front faces.
        pipeline.RasterizerState.CullMode = index == 1 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.InputLayout = {layout, static_cast<UINT>(std::size(layout))};
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        for (auto &blend : pipeline.BlendState.RenderTarget)
        {
            blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
            blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        if (index < 2)
        {
            pipeline.DepthStencilState.DepthEnable = TRUE;
            pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
            pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            if (index == 0) { pipeline.NumRenderTargets = 1; pipeline.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT; }
        }
        else if (index == 2)
        {
            // Reverse-Z: exactly the tail's former z > opaque-depth predicate.
            // Read-only depth allows early rejection before material/UAV work.
            pipeline.DepthStencilState.DepthEnable = TRUE;
            pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
            pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            pipeline.NumRenderTargets = 2; pipeline.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            pipeline.RTVFormats[1] = DXGI_FORMAT_R16_FLOAT;
            pipeline.BlendState.IndependentBlendEnable = TRUE;
            auto &accumulation = pipeline.BlendState.RenderTarget[0];
            accumulation.BlendEnable = TRUE; accumulation.SrcBlend = accumulation.DestBlend = D3D12_BLEND_ONE;
            accumulation.BlendOp = accumulation.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            accumulation.SrcBlendAlpha = accumulation.DestBlendAlpha = D3D12_BLEND_ONE;
            auto &reveal = pipeline.BlendState.RenderTarget[1];
            reveal.BlendEnable = TRUE; reveal.SrcBlend = D3D12_BLEND_ZERO; reveal.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            reveal.BlendOp = reveal.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            reveal.SrcBlendAlpha = D3D12_BLEND_ZERO; reveal.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            reveal.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
        }
        else if (index == 3)
        {
            // Reuse peeled depth/identity; emit only motion, history, class and transport.
            pipeline.NumRenderTargets = 1 + static_cast<UINT>(kCompositeGuideIndices.size());
            pipeline.RTVFormats[0] = kColor;
            for (std::uint32_t target = 0; target < kCompositeGuideIndices.size(); ++target)
            {
                pipeline.RTVFormats[1 + target] = kGuideFormats[kCompositeGuideIndices[target]];
                pipeline.BlendState.RenderTarget[1 + target].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }
            pipeline.BlendState.IndependentBlendEnable = TRUE;
        }
        else { pipeline.NumRenderTargets = 1; pipeline.RTVFormats[0] = kColor; }
        if (index >= 3)
        {
            // Shared current-position evaluation with the front peel. Keep the
            // identity check in the PS to reject coincident other instances.
            pipeline.DepthStencilState.DepthEnable = TRUE;
            pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
            pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        }
        const auto native = pipelineCache.CreateGraphics(&pipeline, IID_PPV_ARGS(&next->pipelines[index]));
        if (FAILED(native)) return NativeFailure("Create hero transparency pipeline " + std::string(product), native);
#if defined(HS_DEVELOPMENT_TOOLS)
        if (next->work) next->work->Pipeline(index, pipeline, phase_vertex, pixel, product);
#endif
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = frameSlots * kHeroTransparencyDescriptorsPerFrame;
    auto native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->tables));
    if (FAILED(native)) return NativeFailure("Create hero transparency source tables", native);
    next->srv_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors = 9;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->exports));
    if (FAILED(native)) return NativeFailure("Create hero transparency exports", native);
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 12;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->rtv_heap));
    if (FAILED(native)) return NativeFailure("Create hero transparency RTVs", native);
    next->rtv_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors = 12;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->dsv_heap));
    if (FAILED(native)) return NativeFailure("Create hero transparency DSVs", native);
    next->dsv_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format = DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    for (std::uint32_t layer = 0; layer < next->layer_count; ++layer)
    {
        device->CreateDepthStencilView(next->front[layer].resource.Get(), &dsv, next->Dsv(layer));
        device->CreateDepthStencilView(next->exit[layer].resource.Get(), &dsv, next->Dsv(4 + layer));
        auto read_only = dsv;
        read_only.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device->CreateDepthStencilView(next->front[layer].resource.Get(), &read_only, next->Dsv(8 + layer));
        device->CreateRenderTargetView(next->identity[layer].resource.Get(), nullptr, next->Rtv(layer));
    }
    next->Srv(next->front[0].resource.Get(), DXGI_FORMAT_R32_FLOAT, next->Export(0));
    for (std::uint32_t index = 1; index < next->guides.size(); ++index)
    {
        if (index == 4) continue;
        device->CreateRenderTargetView(next->guides[index].resource.Get(), nullptr, next->Rtv(4 + index));
        next->Srv(next->guides[index].resource.Get(), kGuideFormats[index], next->Export(index));
    }
    next->Srv(next->identity[0].resource.Get(), DXGI_FORMAT_R32G32_UINT, next->Export(4));
    next->Srv(next->overflow.resource.Get(), DXGI_FORMAT_R32_UINT, next->Export(6));
    for (std::uint32_t index = 0; index < next->color.size(); ++index)
    {
        device->CreateRenderTargetView(next->color[index].resource.Get(), nullptr, next->Rtv(10 + index));
        next->Srv(next->color[index].resource.Get(), kColor, next->Export(7 + index));
    }
    D3D12MA::ALLOCATION_DESC uploadAllocation{}; uploadAllocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload{};
    upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload.Width = UINT64(frameSlots) * kHeroTransparencyPhaseCount * kConstantStride;
    upload.Height = 1; upload.DepthOrArraySize = 1; upload.MipLevels = 1; upload.SampleDesc.Count = 1;
    upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    native = allocator->CreateResource(&uploadAllocation, &upload, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        &next->constants.allocation, IID_PPV_ARGS(next->constants.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create hero transparency constants", native);
    void *mapped{}; D3D12_RANGE noRead{0, 0};
    native = next->constants.resource->Map(0, &noRead, &mapped);
    if (FAILED(native)) return NativeFailure("Map hero transparency constants", native);
    next->mapped = static_cast<std::byte *>(mapped);
    // Resource identity, formats and CBV addresses are fixed for this target generation.
    for (std::uint32_t slot = 0; slot < frameSlots; ++slot)
        for (std::uint32_t phase = 0; phase < kHeroTransparencyPhaseCount; ++phase)
            next->InitializeTable(slot, phase);
    auto include = [&](const AllocationResource &resource) { if (resource.allocation) next->bytes += resource.allocation->GetSize(); };
    include(next->constants); include(next->overflow);
    for (const auto &resource : next->front) include(resource);
    for (const auto &resource : next->identity) include(resource);
    for (const auto &resource : next->exit) include(resource);
    for (const auto &resource : next->guides) include(resource);
    for (const auto &resource : next->color) include(resource);
#if defined(HS_DEVELOPMENT_TOOLS)
    if (next->work) next->bytes += next->work->Status().readback_bytes;
#endif
    candidate.targets_ = std::move(next);
    return Result::Success();
}

Result HeroTransparencySystem::Commit(HeroTransparencyCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete pending hero transparency before replacing targets.");
    if (!candidate.targets_) return Failure(ErrorCode::InvalidArgument, "Hero transparency candidate was not prepared.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "Old hero transparency targets are still in GPU use.");
#if defined(HS_DEVELOPMENT_TOOLS)
    if (auto collected = ConsumeCompletedWork(completed, impl_->completed_work); !collected) return collected;
    if (impl_->targets && impl_->targets->work && impl_->targets->work->Status().pending_frames)
        return Failure(ErrorCode::InvalidState, "Old hero contributor queries are still in GPU use.");
    if (impl_->targets && impl_->targets->work)
    {
        const auto previous = impl_->targets->work->Status();
        impl_->retired_work.mesh_statistics_supported |= previous.mesh_statistics_supported;
        impl_->retired_work.shader_counters_enabled |= previous.shader_counters_enabled;
        impl_->retired_work.submitted_frames += previous.submitted_frames;
        impl_->retired_work.completed_frames += previous.completed_frames;
        impl_->retired_work.failed_frames += previous.failed_frames;
        impl_->retired_work.samples += previous.samples;
        impl_->retired_work.overflowed |= previous.overflowed;
        impl_->retired_work.shader_invalid_samples += previous.shader_invalid_samples;
        impl_->retired_work.shader_clear_commands += previous.shader_clear_commands;
        impl_->retired_work.shader_copy_commands += previous.shader_copy_commands;
    }
#endif
    if (impl_->next_generation == UINT64_MAX)
        return Failure(ErrorCode::InvalidState, "Hero transparency target generation is exhausted.");
    candidate.targets_->generation = ++impl_->next_generation;
    impl_->targets = std::move(candidate.targets_);
    return Result::Success();
}

Result HeroTransparencySystem::RegisterLayers(RenderGraphBuilder &graph,
    const HeroTransparencyInputs &inputs, HeroTransparencyOutputs &outputs,
    HeroTransparencyLayout *layout)
{
    return RegisterLayersImpl(graph, inputs, outputs, layout, nullptr);
}
Result HeroTransparencySystem::BindRetainedLayers(RenderGraphBuilder &graph,
    const HeroTransparencyInputs &inputs, const HeroTransparencyLayout &layout,
    HeroTransparencyOutputs &outputs)
{
    return RegisterLayersImpl(graph, inputs, outputs, nullptr, &layout);
}
Result HeroTransparencySystem::RegisterLayersImpl(RenderGraphBuilder &graph,
    const HeroTransparencyInputs &inputs, HeroTransparencyOutputs &outputs,
    HeroTransparencyLayout *layout, const HeroTransparencyLayout *retained)
{
    outputs = {};
    auto storage = impl_->targets;
    if (!storage) return Failure(ErrorCode::InvalidState, "Prepare and commit hero transparency before registration.");
    if (!storage->enabled) return Result::Success();
    if (impl_->pending.graph || !storage->usable)
        return Failure(ErrorCode::InvalidState, "Hero transparency is pending or its previous graph failed.");
    const auto active = inputs.render_active;
    const auto rect = inputs.composition_rect;
    const bool rect_empty = rect.left == rect.right && rect.top == rect.bottom;
    if (!rect_empty && (rect.left < 0 || rect.top < 0 || rect.right <= rect.left ||
            rect.bottom <= rect.top || static_cast<std::uint32_t>(rect.right) > active.width ||
            static_cast<std::uint32_t>(rect.bottom) > active.height))
        return Failure(ErrorCode::InvalidArgument, "Hero composition rectangle exceeds its active extent.");
    const bool use_occupancy = inputs.use_occupancy_queries && storage->occupancy_queries.Get();
#if defined(HS_DEVELOPMENT_TOOLS)
    const bool capture_work = static_cast<bool>(storage->work);
#else
    constexpr bool capture_work = false;
#endif
    const auto same_version = [](ResourceVersion a, ResourceVersion b) {
        return a.resource.value == b.resource.value && a.resource.kind == b.resource.kind &&
            a.ordinal == b.ordinal;
    };
    if (retained)
    {
        const std::array current{inputs.opaque_depth.version, inputs.hdr.version,
            inputs.accumulation.version, inputs.revealage.version};
        const auto same_reads = [&](std::span<const HeroTransparencySceneRead> now,
                const std::vector<HeroTransparencySceneRead> &saved) {
            if (now.size() != saved.size()) return false;
            for (std::size_t index = 0; index < now.size(); ++index)
                if (now[index].access != saved[index].access ||
                    !same_version(now[index].version, saved[index].version)) return false;
            return true;
        };
        if (retained->target_generation != storage->generation ||
            retained->layer_count != storage->layer_count ||
            retained->capture_work != capture_work ||
            retained->occupancy != use_occupancy ||
            !same_reads(inputs.peel_reads, retained->peel_reads) ||
            !same_reads(inputs.scene_reads, retained->scene_reads))
            return Failure(ErrorCode::InvalidState, "Hero retained topology variant changed.");
        for (std::size_t index = 0; index < current.size(); ++index)
            if (!same_version(current[index], retained->inputs[index]))
                return Failure(ErrorCode::InvalidState, "Hero retained input version changed.");
        const auto pass_matches = [&](std::uint32_t index, std::uint32_t id) {
            return index < graph.PassCount() && graph.DeclaredPassId(index) == id &&
                graph.BindPass(index, id).has_value();
        };
        for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
            if (!pass_matches(retained->front_pass[layer], 90 + layer * 2) ||
                !pass_matches(retained->exit_pass[layer], 91 + layer * 2))
                return Failure(ErrorCode::InvalidState, "Hero retained peel pass changed.");
        if (!pass_matches(retained->overflow_clear_pass, 98) ||
            !pass_matches(retained->tail_pass, 99))
            return Failure(ErrorCode::InvalidState, "Hero retained overflow pass changed.");
    }
    if (!ValidExtent(active) || active.width > storage->capacity.width || active.height > storage->capacity.height ||
        inputs.frame_slot >= storage->frame_slot_count || !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed) ||
        !FitsDepth(inputs.opaque_depth, active) || !Fits(inputs.hdr, active, kColor) ||
        !Fits(inputs.accumulation, active, DXGI_FORMAT_R32G32B32A32_FLOAT) ||
        !Fits(inputs.revealage, active, DXGI_FORMAT_R16_FLOAT) ||
        !inputs.accumulation_rtv.ptr || !inputs.revealage_rtv.ptr || !inputs.opaque_readonly_dsv.ptr || !inputs.shader_heap ||
        !inputs.table_cpu.ptr || !inputs.table_gpu.ptr || inputs.descriptor_stride != storage->srv_stride || !inputs.draw)
        return Failure(ErrorCode::InvalidArgument, "Hero transparency frame, guides, native table or draw callback is invalid.");
    const auto heap = inputs.shader_heap->GetDesc();
    auto *sourceHeap = inputs.descriptor_source_heap ? inputs.descriptor_source_heap : inputs.shader_heap;
    const auto sourceDescription = sourceHeap->GetDesc();
    Microsoft::WRL::ComPtr<ID3D12Device> sourceDevice;
    Microsoft::WRL::ComPtr<ID3D12Device> shaderDevice;
    if (FAILED(sourceHeap->GetDevice(IID_PPV_ARGS(&sourceDevice))) || sourceDevice.Get() != storage->device.Get() ||
        FAILED(inputs.shader_heap->GetDevice(IID_PPV_ARGS(&shaderDevice))) || shaderDevice.Get() != storage->device.Get() ||
        sourceDescription.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
        sourceDescription.NumDescriptors != heap.NumDescriptors ||
        (inputs.descriptor_source_heap && (sourceDescription.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)))
        return Failure(ErrorCode::InvalidArgument, "Hero transparency CPU descriptor source is incompatible with its native shader heap.");
    const auto heapCpu = sourceHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    const auto heapGpu = inputs.shader_heap->GetGPUDescriptorHandleForHeapStart().ptr;
    const auto heapBytes = UINT64(heap.NumDescriptors) * inputs.descriptor_stride;
    if (heap.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(heap.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ||
        inputs.table_cpu.ptr < heapCpu || inputs.table_gpu.ptr < heapGpu ||
        UINT64(inputs.table_cpu.ptr - heapCpu) != inputs.table_gpu.ptr - heapGpu ||
        UINT64(inputs.table_cpu.ptr - heapCpu) % inputs.descriptor_stride != 0 ||
        UINT64(inputs.table_cpu.ptr - heapCpu) + UINT64(kHeroTransparencyDescriptorsPerFrame) * inputs.descriptor_stride > heapBytes)
        return Failure(ErrorCode::InvalidArgument, "Hero transparency descriptor suffix does not fit its native shader heap.");
    Impl::Pending pending;
    pending.graph = &graph; pending.targets = storage; pending.inputs = inputs; pending.shader_heap = inputs.shader_heap;
    if (layout)
    {
        *layout = {};
        layout->target_generation = storage->generation;
        layout->layer_count = storage->layer_count;
        layout->capture_work = capture_work;
        layout->inputs = {inputs.opaque_depth.version, inputs.hdr.version,
            inputs.accumulation.version, inputs.revealage.version};
        layout->peel_reads.assign(inputs.peel_reads.begin(), inputs.peel_reads.end());
        layout->scene_reads.assign(inputs.scene_reads.begin(), inputs.scene_reads.end());
    }
    if (layout) layout->occupancy = use_occupancy;
    if (!retained) pending.scene_reads.assign(inputs.scene_reads.begin(), inputs.scene_reads.end());
    const auto certify = [&](AllocationResource &resource) {
        return resource.EnsureNativeContract(storage->device.Get());
    };
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
        for (auto *resource : {&storage->front[layer], &storage->identity[layer],
                &storage->exit[layer]})
            if (auto certified = certify(*resource); !certified) return certified;
    for (std::uint32_t index = 0; index < storage->guides.size(); ++index)
    {
        if (index == 0 || index == 4) continue;
        if (auto certified = certify(storage->guides[index]); !certified) return certified;
    }
    for (auto &resource : storage->color)
        if (auto certified = certify(resource); !certified) return certified;
    if (auto certified = certify(storage->overflow); !certified) return certified;
    if (use_occupancy)
        for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
            if (auto certified = certify(storage->occupancy_results[inputs.frame_slot][layer]);
                !certified) return certified;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (storage->work)
    {
        if (auto collected = storage->work->Collect(inputs.completed, impl_->completed_work); !collected) return collected;
        const auto begun = retained
            ? storage->work->BindRetainedBegin(graph, inputs, retained->work)
            : storage->work->Begin(graph, inputs, layout ? &layout->work : nullptr);
        if (!begun) return begun;
    }
#endif
    bool imports_valid = true;
    auto import = [&](AllocationResource &resource, std::string_view name, TextureHandle saved = {}) {
        const auto external = ExternalTexture{resource.resource.Get(), resource.graph_completion,
            resource.graph_queue, {}, resource.NativeContract()};
        const auto handle = retained ? saved : graph.ImportTexture(external, resource.graph_access, name);
        if (retained && !graph.BindTexture(handle, external, resource.graph_access))
            imports_valid = false;
        pending.imports.push_back({&resource, handle}); return graph.Version(handle);
    };
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
    {
        pending.front[layer] = import(storage->front[layer], "Hero.FrontDepth",
            retained ? retained->front[layer] : TextureHandle{});
        pending.identity[layer] = import(storage->identity[layer], "Hero.InstanceIdentity",
            retained ? retained->identity[layer] : TextureHandle{});
        pending.exit[layer] = import(storage->exit[layer], "Hero.ExitDepth",
            retained ? retained->exit[layer] : TextureHandle{});
        if (layout)
        {
            layout->front[layer] = pending.front[layer].resource;
            layout->identity[layer] = pending.identity[layer].resource;
            layout->exit[layer] = pending.exit[layer].resource;
        }
        storage->FillTable(inputs, layer, layer, HeroTransparencyPhase::Front, inputs.hdr.resource);
        storage->FillTable(inputs, 4 + layer, layer, HeroTransparencyPhase::Exit, inputs.hdr.resource);
    }
    for (std::uint32_t index = 0; index < storage->color.size(); ++index)
    {
        pending.color[index] = import(storage->color[index], "Hero.CompositeHDR",
            retained ? retained->color[index] : TextureHandle{});
        if (layout) layout->color[index] = pending.color[index].resource;
    }
    auto &guides = pending.guides;
    for (std::uint32_t index = 0; index < guides.size(); ++index)
    {
        if (index == 0) guides[index] = pending.front[0];
        else if (index == 4) guides[index] = pending.identity[0];
        else guides[index] = import(storage->guides[index], "Hero.NearestFrontGuide",
            retained ? retained->guides[index] : TextureHandle{});
        if (layout) layout->guides[index] = guides[index].resource;
    }
    auto overflow = import(storage->overflow, "Hero.LayerOverflow",
        retained ? retained->overflow : TextureHandle{});
    if (layout) layout->overflow = overflow.resource;
    std::array<ResourceVersion, kHeroTransparencyMaxLayers> occupancy{};
    if (use_occupancy)
        for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
        {
            auto &result = storage->occupancy_results[inputs.frame_slot][layer];
            const auto external = ExternalBuffer{result.resource.Get(), result.graph_completion,
                result.graph_queue, {}, result.NativeContract()};
            const auto handle = retained ? retained->occupancy_results[layer]
                : graph.ImportBuffer(external, result.graph_access, "Hero.LayerOccupancy");
            if (retained && !graph.BindBuffer(handle, external, result.graph_access))
                imports_valid = false;
            pending.imports.push_back({&result, handle});
            occupancy[layer] = graph.Version(handle);
            if (layout) layout->occupancy_results[layer] = handle;
        }
    if (!imports_valid)
        return Failure(ErrorCode::InvalidState, "Hero retained import bindings changed.");
    storage->FillTable(inputs, kTailPhase, 0, HeroTransparencyPhase::Tail, inputs.hdr.resource);
    auto readScene = [&](PassBuilder &pass, bool shading, Access depthAccess = Access::ShaderRead) {
        pass.Read(inputs.opaque_depth.version, depthAccess);
        for (const auto &read : shading ? std::span<const HeroTransparencySceneRead>(pending.scene_reads) : inputs.peel_reads)
            pass.Read(read.version, read.access);
    };
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
    {
        if (layout) layout->front_pass[layer] = graph.PassCount();
        auto front = retained
            ? *graph.BindPass(retained->front_pass[layer], 90 + layer * 2)
            : graph.AddPass(90 + layer * 2, "HeroPeelFront", QueueHint::Direct);
        if (!retained)
        {
            readScene(front, false);
            if (layer) front.Read(pending.front[layer - 1], Access::ShaderRead);
        }
        if (use_occupancy)
        {
            if (!retained)
            {
                if (layer) front.Read(occupancy[layer - 1], Access::IndirectArgs);
                occupancy[layer] = front.Write(occupancy[layer], Access::CopyDest);
            }
            else occupancy[layer] = {occupancy[layer].resource, 1};
            pending.occupancy[layer] = occupancy[layer];
        }
        pending.front[layer] = retained ? ResourceVersion{pending.front[layer].resource, 1}
            : front.Write(pending.front[layer], Access::DepthWrite);
        pending.identity[layer] = retained ? ResourceVersion{pending.identity[layer].resource, 1}
            : front.Write(pending.identity[layer], Access::RenderTarget);
#if defined(HS_DEVELOPMENT_TOOLS)
        if (storage->work)
        {
            const auto bound = retained
                ? storage->work->BindRetainedPhase(graph, inputs.frame_slot, layer, retained->work)
                : Result::Success();
            if (!bound) return bound;
            if (!retained) storage->work->DeclarePhase(graph, front, inputs.frame_slot, layer,
                layout ? &layout->work : nullptr);
        }
#endif
        front.SetExecute([storage, inputs, layer, use_occupancy](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            constexpr float clear[]{0, 0, 0, 0};
            const auto target = storage->Rtv(layer), depth = storage->Dsv(layer);
            commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            commands->ClearRenderTargetView(target, clear, 0, nullptr);
            commands->OMSetRenderTargets(1, &target, FALSE, &depth);
            if (use_occupancy)
            {
                commands->BeginQuery(storage->occupancy_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                    inputs.frame_slot * kHeroTransparencyMaxLayers + layer);
                if (layer)
                    commands->SetPredication(storage->occupancy_results[inputs.frame_slot][layer - 1].resource.Get(),
                        0, D3D12_PREDICATION_OP_EQUAL_ZERO);
            }
#if defined(HS_DEVELOPMENT_TOOLS)
            if (storage->work) storage->work->Record(commands, storage->pipelines[0].Get(), inputs, HeroTransparencyPhase::Front, layer, layer, storage->Gpu(inputs, layer));
            else
#endif
                inputs.draw(commands, storage->pipelines[0].Get(), HeroTransparencyPhase::Front, layer, storage->Gpu(inputs, layer));
            if (use_occupancy)
            {
                if (layer) commands->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                const auto query = inputs.frame_slot * kHeroTransparencyMaxLayers + layer;
                commands->EndQuery(storage->occupancy_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
                commands->ResolveQueryData(storage->occupancy_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                    query, 1, storage->occupancy_results[inputs.frame_slot][layer].resource.Get(), 0);
            }
        });
        if (layout) layout->exit_pass[layer] = graph.PassCount();
        auto exit = retained
            ? *graph.BindPass(retained->exit_pass[layer], 91 + layer * 2)
            : graph.AddPass(91 + layer * 2, "HeroPeelExit", QueueHint::Direct);
        if (!retained)
        {
            readScene(exit, false);
            exit.Read(pending.front[layer], Access::ShaderRead);
            exit.Read(pending.identity[layer], Access::ShaderRead);
            if (use_occupancy) exit.Read(occupancy[layer], Access::IndirectArgs);
        }
        pending.exit[layer] = retained ? ResourceVersion{pending.exit[layer].resource, 1}
            : exit.Write(pending.exit[layer], Access::DepthWrite);
#if defined(HS_DEVELOPMENT_TOOLS)
        if (storage->work)
        {
            const auto bound = retained
                ? storage->work->BindRetainedPhase(graph, inputs.frame_slot, 4 + layer, retained->work)
                : Result::Success();
            if (!bound) return bound;
            if (!retained) storage->work->DeclarePhase(graph, exit, inputs.frame_slot,
                4 + layer, layout ? &layout->work : nullptr);
        }
#endif
        exit.SetExecute([storage, inputs, layer, use_occupancy](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            const auto depth = storage->Dsv(4 + layer);
            commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            commands->OMSetRenderTargets(0, nullptr, FALSE, &depth);
            if (use_occupancy)
                commands->SetPredication(storage->occupancy_results[inputs.frame_slot][layer].resource.Get(),
                    0, D3D12_PREDICATION_OP_EQUAL_ZERO);
#if defined(HS_DEVELOPMENT_TOOLS)
            if (storage->work) storage->work->Record(commands, storage->pipelines[1].Get(), inputs, HeroTransparencyPhase::Exit, layer, 4 + layer, storage->Gpu(inputs, 4 + layer));
            else
#endif
                inputs.draw(commands, storage->pipelines[1].Get(), HeroTransparencyPhase::Exit, layer, storage->Gpu(inputs, 4 + layer));
            if (use_occupancy)
                commands->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        });
    }
    if (layout) layout->overflow_clear_pass = graph.PassCount();
    auto clear = retained ? *graph.BindPass(retained->overflow_clear_pass, 98)
        : graph.AddPass(98, "HeroClearOverflow", QueueHint::Direct);
    overflow = retained ? ResourceVersion{overflow.resource, 1}
        : clear.Write(overflow, Access::UnorderedWrite);
    clear.SetExecute([storage, inputs](RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ID3D12DescriptorHeap *heaps[]{inputs.shader_heap}; commands->SetDescriptorHeaps(1, heaps);
        constexpr UINT zero[]{0, 0, 0, 0};
        commands->ClearUnorderedAccessViewUint(storage->Gpu(inputs, kTailPhase, kUavOffset),
            storage->Cpu(inputs.frame_slot, kTailPhase, kUavOffset), storage->overflow.resource.Get(), zero, 0, nullptr);
    });
    if (layout) layout->tail_pass = graph.PassCount();
    auto tail = retained ? *graph.BindPass(retained->tail_pass, 99)
        : graph.AddPass(99, "HeroOitTail", QueueHint::Direct);
    if (!retained)
    {
        readScene(tail, true, Access::DepthShaderRead);
        tail.Read(inputs.hdr.version, Access::ShaderRead);
    }
    // Layer 0 and the tail use the same front-face, reserved-key, opaque-depth
    // and transport-opacity admission. If layer 0 has no accepted sample, no
    // tail sample can exist. Deeper emptiness alone is insufficient because
    // equal-depth, different-identity fragments may still reach the tail.
    if (use_occupancy && !retained) tail.Read(occupancy[0], Access::IndirectArgs);
    if (!retained) for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
    {
        tail.Read(pending.front[layer], Access::ShaderRead); tail.Read(pending.identity[layer], Access::ShaderRead);
    }
    overflow = retained ? ResourceVersion{overflow.resource, 2}
        : tail.Write(overflow, Access::UnorderedWrite);
    outputs.accumulation = retained ? retained->accumulation_version
        : tail.ReadWrite(inputs.accumulation.version, Access::RenderTarget);
    outputs.revealage = retained ? retained->revealage_version
        : tail.ReadWrite(inputs.revealage.version, Access::RenderTarget);
#if defined(HS_DEVELOPMENT_TOOLS)
    if (storage->work)
    {
        const auto bound = retained
            ? storage->work->BindRetainedPhase(graph, inputs.frame_slot, kTailPhase, retained->work)
            : Result::Success();
        if (!bound) return bound;
        if (!retained) storage->work->DeclarePhase(graph, tail, inputs.frame_slot,
            kTailPhase, layout ? &layout->work : nullptr);
    }
#endif
    tail.SetExecute([storage, inputs, use_occupancy](RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{inputs.accumulation_rtv, inputs.revealage_rtv};
        commands->OMSetRenderTargets(2, targets, FALSE, &inputs.opaque_readonly_dsv);
        if (use_occupancy)
            commands->SetPredication(storage->occupancy_results[inputs.frame_slot][0].resource.Get(),
                0, D3D12_PREDICATION_OP_EQUAL_ZERO);
#if defined(HS_DEVELOPMENT_TOOLS)
        if (storage->work) storage->work->Record(commands, storage->pipelines[2].Get(), inputs, HeroTransparencyPhase::Tail, 0, kTailPhase, storage->Gpu(inputs, kTailPhase));
        else
#endif
            inputs.draw(commands, storage->pipelines[2].Get(), HeroTransparencyPhase::Tail, 0, storage->Gpu(inputs, kTailPhase));
        if (use_occupancy)
            commands->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    });
    pending.overflow = overflow;
    outputs.depth = pending.front[0]; outputs.motion = guides[1]; outputs.history = guides[2];
    outputs.classification = guides[3]; outputs.metadata = pending.identity[0]; outputs.transport = guides[5]; outputs.overflow = overflow;
    outputs.depth_resource = storage->front[0].resource.Get(); outputs.motion_resource = storage->guides[1].resource.Get();
    outputs.history_resource = storage->guides[2].resource.Get(); outputs.classification_resource = storage->guides[3].resource.Get();
    outputs.metadata_resource = storage->identity[0].resource.Get(); outputs.transport_resource = storage->guides[5].resource.Get();
    outputs.overflow_resource = storage->overflow.resource.Get();
    outputs.depth_srv = storage->Export(0); outputs.motion_srv = storage->Export(1); outputs.history_srv = storage->Export(2);
    outputs.classification_srv = storage->Export(3); outputs.metadata_srv = storage->Export(4);
    outputs.transport_srv = storage->Export(5); outputs.overflow_srv = storage->Export(6);
    if (layout)
    {
        layout->overflow_version = overflow;
        layout->accumulation_version = outputs.accumulation;
        layout->revealage_version = outputs.revealage;
    }
    impl_->pending = std::move(pending);
    return Result::Success();
}

Result HeroTransparencySystem::RegisterComposition(RenderGraphBuilder &graph,
    HeroTransparencyGuide background, HeroTransparencyOutputs &outputs,
    HeroTransparencyLayout *layout)
{
    return RegisterCompositionImpl(graph, background, outputs, layout, nullptr);
}
Result HeroTransparencySystem::BindRetainedComposition(RenderGraphBuilder &graph,
    HeroTransparencyGuide background, const HeroTransparencyLayout &layout,
    HeroTransparencyOutputs &outputs)
{
    return RegisterCompositionImpl(graph, background, outputs, nullptr, &layout);
}
bool HeroTransparencySystem::MatchesLayout(const HeroTransparencyInputs &inputs,
    HeroTransparencyGuide background, const HeroTransparencyLayout &layout) const noexcept
{
    const auto storage = impl_->targets;
#if defined(HS_DEVELOPMENT_TOOLS)
    const bool capture_work = storage && static_cast<bool>(storage->work);
#else
    constexpr bool capture_work = false;
#endif
    if (!storage || !storage->enabled || !storage->usable ||
        !layout.target_generation || layout.target_generation != storage->generation ||
        layout.layer_count != storage->layer_count ||
        layout.capture_work != capture_work ||
        layout.occupancy != (inputs.use_occupancy_queries && storage->occupancy_queries.Get()))
        return false;
    const auto same = [](ResourceVersion a, ResourceVersion b) {
        return a.resource.value == b.resource.value && a.resource.kind == b.resource.kind &&
            a.ordinal == b.ordinal;
    };
    const std::array current{inputs.opaque_depth.version, inputs.hdr.version,
        inputs.accumulation.version, inputs.revealage.version};
    for (std::size_t index = 0; index < current.size(); ++index)
        if (!same(current[index], layout.inputs[index])) return false;
    if (!same(background.version, layout.background)) return false;
    const auto same_reads = [&](std::span<const HeroTransparencySceneRead> now,
            const std::vector<HeroTransparencySceneRead> &saved) {
        if (now.size() != saved.size()) return false;
        for (std::size_t index = 0; index < now.size(); ++index)
            if (now[index].access != saved[index].access ||
                !same(now[index].version, saved[index].version)) return false;
        return true;
    };
    if (!same_reads(inputs.peel_reads, layout.peel_reads) ||
        !same_reads(inputs.scene_reads, layout.scene_reads)) return false;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (storage->work)
    {
        if (inputs.work_contributors.size() != layout.work.contributor_count ||
            inputs.work_contributors.size() > layout.work.argument_versions.size()) return false;
        for (std::size_t index = 0; index < inputs.work_contributors.size(); ++index)
            if (!same(inputs.work_contributors[index].arguments,
                    layout.work.argument_versions[index])) return false;
    }
#endif
    return true;
}
Result HeroTransparencySystem::RegisterCompositionImpl(RenderGraphBuilder &graph,
    HeroTransparencyGuide background, HeroTransparencyOutputs &outputs,
    HeroTransparencyLayout *layout, const HeroTransparencyLayout *retained)
{
    if (impl_->targets && !impl_->targets->enabled) return Result::Success();
    auto &pending = impl_->pending;
    if (pending.graph != &graph || pending.composed)
        return Failure(ErrorCode::InvalidState, "Register hero layers once before their composition on the same graph.");
    auto storage = pending.targets;
    const auto inputs = pending.inputs;
    const bool use_occupancy = inputs.use_occupancy_queries && storage->occupancy_queries.Get();
#if defined(HS_DEVELOPMENT_TOOLS)
    const bool capture_work = static_cast<bool>(storage->work);
#else
    constexpr bool capture_work = false;
#endif
    if (layout && (layout->target_generation != storage->generation ||
            layout->layer_count != storage->layer_count || layout->occupancy != use_occupancy ||
            layout->capture_work != capture_work))
        return Failure(ErrorCode::InvalidState, "Hero composition layout variant changed.");
    if (retained)
    {
        const auto expected = retained->background;
        if (retained->target_generation != storage->generation ||
            retained->layer_count != storage->layer_count ||
            retained->occupancy != use_occupancy ||
            retained->capture_work != capture_work ||
            expected.resource.value != background.version.resource.value ||
            expected.resource.kind != background.version.resource.kind ||
            expected.ordinal != background.version.ordinal)
            return Failure(ErrorCode::InvalidState, "Hero retained background or variant changed.");
        for (std::uint32_t step = 0; step < storage->layer_count; ++step)
            if (!graph.BindPass(retained->copy_pass[step], 101 + step * 2) ||
                !graph.BindPass(retained->composite_pass[step], 102 + step * 2))
                return Failure(ErrorCode::InvalidState, "Hero retained composition pass changed.");
    }
    if (!Fits(background, inputs.render_active, kColor))
        return Failure(ErrorCode::InvalidArgument, "Hero composition requires an initialized RGBA16F background at the active extent.");
    if (layout) layout->background = background.version;
    auto source = background.version;
    auto *sourceResource = background.resource;
    // Occupancy is available from this frame's front peel only. A missing
    // front cannot write its corresponding composite, while the tail retains
    // its independent front-face/opacity path.
    for (std::uint32_t step = 0; step < storage->layer_count; ++step)
    {
        const auto layer = storage->layer_count - 1 - step, target = step % 2;
        const auto phase = kCompositePhase + layer;
        storage->FillTable(inputs, phase, layer, HeroTransparencyPhase::Composite, sourceResource);
        if (layout) layout->copy_pass[step] = graph.PassCount();
        auto copy = retained ? *graph.BindPass(retained->copy_pass[step], 101 + step * 2)
            : graph.AddPass(101 + step * 2, "HeroCopyBackground", QueueHint::Direct);
        if (!retained) copy.Read(source, Access::CopySource);
        pending.color[target] = retained
            ? ResourceVersion{pending.color[target].resource, (step / 2) * 2 + 1}
            : copy.Write(pending.color[target], Access::CopyDest);
        copy.SetExecute([storage, sourceResource, target, step, active = inputs.render_active,
                            rect = inputs.composition_rect](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            D3D12_TEXTURE_COPY_LOCATION destination{}, sourceLocation{};
            destination.pResource = storage->color[target].resource.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sourceLocation.pResource = sourceResource; sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            // The first two full copies initialize both ping-pong targets this
            // frame. Thereafter their pixels outside the proven write rect are
            // the same background, including refracted source samples.
            const bool partial = step >= 2 && rect.right > rect.left && rect.bottom > rect.top;
            const D3D12_BOX box{partial ? static_cast<UINT>(rect.left) : 0u,
                partial ? static_cast<UINT>(rect.top) : 0u, 0u,
                partial ? static_cast<UINT>(rect.right) : active.width,
                partial ? static_cast<UINT>(rect.bottom) : active.height, 1u};
            commands->CopyTextureRegion(&destination, box.left, box.top, 0, &sourceLocation, &box);
        });
        if (layout) layout->composite_pass[step] = graph.PassCount();
        auto compose = retained ? *graph.BindPass(retained->composite_pass[step], 102 + step * 2)
            : graph.AddPass(102 + step * 2, "HeroLayerComposite", QueueHint::Direct);
        if (!retained)
        {
            compose.Read(source, Access::ShaderRead);
            compose.Read(inputs.opaque_depth.version, Access::ShaderRead);
            compose.Read(pending.front[layer], Access::DepthShaderRead);
            compose.Read(pending.identity[layer], Access::ShaderRead);
            compose.Read(pending.exit[layer], Access::ShaderRead);
            if (use_occupancy) compose.Read(pending.occupancy[layer], Access::IndirectArgs);
            if (layer + 1 < storage->layer_count)
                compose.Read(pending.front[layer + 1], Access::ShaderRead);
            for (const auto &read : pending.scene_reads) compose.Read(read.version, read.access);
        }
        pending.color[target] = retained
            ? ResourceVersion{pending.color[target].resource, (step / 2) * 2 + 2}
            : compose.ReadWrite(pending.color[target], Access::RenderTarget);
        if (layer == 0)
        {
            if (!retained) compose.Read(pending.overflow, Access::ShaderRead);
            for (const auto index : kCompositeGuideIndices)
                pending.guides[index] = retained ? ResourceVersion{pending.guides[index].resource, 1}
                    : compose.Write(pending.guides[index], Access::RenderTarget);
            outputs.depth = pending.front[0]; outputs.motion = pending.guides[1]; outputs.history = pending.guides[2];
            outputs.classification = pending.guides[3]; outputs.metadata = pending.identity[0]; outputs.transport = pending.guides[5];
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        if (storage->work)
        {
            const auto bound = retained
                ? storage->work->BindRetainedPhase(graph, inputs.frame_slot, phase, retained->work)
                : Result::Success();
            if (!bound) return bound;
            if (!retained) storage->work->DeclarePhase(graph, compose, inputs.frame_slot,
                phase, layout ? &layout->work : nullptr);
        }
#endif
        compose.SetExecute([storage, inputs, layer, phase, target, use_occupancy](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            const auto rtv = storage->Rtv(10 + target);
            const auto depth = storage->Dsv(8 + layer);
            if (layer == 0)
            {
                std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1 + kCompositeGuideIndices.size()> targets{rtv};
                constexpr float zero[]{0, 0, 0, 0};
                for (std::uint32_t index = 0; index < kCompositeGuideIndices.size(); ++index)
                {
                    const auto guideIndex = kCompositeGuideIndices[index];
                    targets[1 + index] = storage->Rtv(4 + guideIndex);
                    commands->ClearRenderTargetView(targets[1 + index], zero, 0, nullptr);
                }
                commands->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), FALSE, &depth);
            }
            else commands->OMSetRenderTargets(1, &rtv, FALSE, &depth);
            auto *pipeline = storage->pipelines[layer == 0 ? 3 : 4].Get();
            if (use_occupancy)
                commands->SetPredication(storage->occupancy_results[inputs.frame_slot][layer].resource.Get(),
                    0, D3D12_PREDICATION_OP_EQUAL_ZERO);
#if defined(HS_DEVELOPMENT_TOOLS)
            if (storage->work) storage->work->Record(commands, pipeline, inputs, HeroTransparencyPhase::Composite, layer, phase, storage->Gpu(inputs, phase));
            else
#endif
                inputs.draw(commands, pipeline, HeroTransparencyPhase::Composite, layer, storage->Gpu(inputs, phase));
            if (use_occupancy)
                commands->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        });
        source = pending.color[target]; sourceResource = storage->color[target].resource.Get();
        outputs.corrected = source; outputs.corrected_resource = sourceResource; outputs.corrected_srv = storage->Export(7 + target);
    }
    if (layout)
    {
        layout->corrected_version = outputs.corrected;
    }
    pending.composed = true;
    return Result::Success();
}

Result HeroTransparencySystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph)
    {
        if (impl_->targets && !impl_->targets->enabled) return Result::Success();
        return Failure(ErrorCode::InvalidState, "Hero transparency has no registered submission.");
    }
    if (pending.graph != &graph) return Failure(ErrorCode::InvalidArgument, "Hero transparency completion refers to another graph.");
    auto storage = pending.targets;
    storage->completion.Merge(completion); storage->slot_completion[pending.inputs.frame_slot].Merge(completion);
    for (const auto &imported : pending.imports)
    {
        const auto resourceCompletion = graph.ResourceCompletion(imported.handle);
        storage->completion.Merge(resourceCompletion); storage->slot_completion[pending.inputs.frame_slot].Merge(resourceCompletion);
        imported.allocation->graph_completion.Merge(completion); imported.allocation->graph_completion.Merge(resourceCompletion);
        if (success)
        {
            imported.allocation->graph_access = graph.FinalAccess(imported.handle);
            imported.allocation->graph_queue = graph.FinalQueue(imported.handle);
        }
    }
    if (!success) storage->usable = false;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (storage->work) storage->work->Submitted(graph, pending.inputs.frame_slot, completion, success);
#endif
    pending = {};
    return Result::Success();
}
Result HeroTransparencySystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete pending hero transparency before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "Hero transparency reset requires completed GPU uses.");
#if defined(HS_DEVELOPMENT_TOOLS)
    if (auto collected = ConsumeCompletedWork(completed, impl_->completed_work); !collected) return collected;
    if (impl_->targets && impl_->targets->work && impl_->targets->work->Status().pending_frames)
        return Failure(ErrorCode::InvalidState, "Hero transparency reset requires completed native contributor queries.");
    impl_->retired_work = {};
#endif
    impl_->targets.reset();
    return Result::Success();
}
#if defined(HS_DEVELOPMENT_TOOLS)
Result HeroTransparencySystem::ConsumeCompletedWork(const CompletionSet &completed, std::vector<RendererHeroWorkSample> &output)
{
    if (impl_->targets && impl_->targets->work)
        if (auto result = impl_->targets->work->Collect(completed, impl_->completed_work); !result) return result;
    if (&output != &impl_->completed_work)
    {
        if (output.size() + impl_->completed_work.size() >
            240 * HeroWorkRetainedLayout::kMaxContributors * kHeroTransparencyPhaseCount)
        {
            impl_->retired_work.overflowed = true;
            return Failure(ErrorCode::InvalidState, "Hero contributor output exceeded its bounded pending rows; preserved rows require capture drain.");
        }
        output.insert(output.end(), std::make_move_iterator(impl_->completed_work.begin()), std::make_move_iterator(impl_->completed_work.end()));
        impl_->completed_work.clear();
    }
    return Result::Success();
}
RendererHeroWorkStatus HeroTransparencySystem::WorkStatus() const
{
    auto status = impl_->targets && impl_->targets->work ? impl_->targets->work->Status() : RendererHeroWorkStatus{};
    status.submitted_frames += impl_->retired_work.submitted_frames;
    status.completed_frames += impl_->retired_work.completed_frames;
    status.failed_frames += impl_->retired_work.failed_frames;
    status.samples += impl_->retired_work.samples;
    status.mesh_statistics_supported |= impl_->retired_work.mesh_statistics_supported;
    status.shader_counters_enabled |= impl_->retired_work.shader_counters_enabled;
    status.overflowed |= impl_->retired_work.overflowed;
    status.shader_invalid_samples += impl_->retired_work.shader_invalid_samples;
    status.shader_clear_commands += impl_->retired_work.shader_clear_commands;
    status.shader_copy_commands += impl_->retired_work.shader_copy_commands;
    return status;
}
#else
Result HeroTransparencySystem::ConsumeCompletedWork(const CompletionSet &, std::vector<RendererHeroWorkSample> &output)
{ output.clear(); return Result::Success(); }
RendererHeroWorkStatus HeroTransparencySystem::WorkStatus() const { return {}; }
#endif
} // namespace hs
