#include "renderer_transparency.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kConstantStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr std::uint32_t kTailPhase = 8, kGuidePhase = 9, kCompositePhase = 10;
constexpr std::uint32_t kUavOffset = 22;
constexpr DXGI_FORMAT kColor = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr std::array<DXGI_FORMAT, 6> kGuideFormats{
    DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
    kColor, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R16G16_FLOAT};
constexpr std::array<std::string_view, 5> kPixelProducts{
    "render/shader/hero_peel_front_ps.dxil", "render/shader/hero_peel_exit_ps.dxil",
    "render/shader/hero_oit_tail_ps.dxil", "render/shader/hero_transparent_guide_ps.dxil",
    "render/shader/hero_layer_composite_ps.dxil"};

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
    ComPtr<ID3D12DescriptorHeap> tables, rtv_heap, dsv_heap, exports;
    std::array<AllocationResource, 4> front, identity, exit;
    std::array<AllocationResource, 6> guides;
    std::array<AllocationResource, 2> color;
    AllocationResource overflow, constants;
    std::byte *mapped{};
    ViewExtent capacity;
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
    void FillTable(const HeroTransparencyInputs &inputs, std::uint32_t phase, std::uint32_t layer,
        HeroTransparencyPhase semantic, ID3D12Resource *background)
    {
        const auto constantSlot = inputs.frame_slot * kHeroTransparencyPhaseCount + phase;
        const std::array<std::uint32_t, 4> values{layer, layer_count, static_cast<std::uint32_t>(semantic), 0};
        std::memcpy(mapped + SIZE_T(constantSlot) * kConstantStride, values.data(), sizeof(values));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{constants.resource->GetGPUVirtualAddress() + UINT64(constantSlot) * kConstantStride,
                                          kConstantStride};
        device->CreateConstantBufferView(&cbv, Cpu(inputs.frame_slot, phase, 0));
        for (std::uint32_t index = 0; index < kHeroTransparencyMaxLayers; ++index)
        {
            Srv(front[index].resource.Get(), DXGI_FORMAT_R32_FLOAT, Cpu(inputs.frame_slot, phase, 1 + index));
            Srv(identity[index].resource.Get(), DXGI_FORMAT_R32G32_UINT, Cpu(inputs.frame_slot, phase, 5 + index));
            Srv(exit[index].resource.Get(), DXGI_FORMAT_R32_FLOAT, Cpu(inputs.frame_slot, phase, 9 + index));
        }
        Srv(background, kColor, Cpu(inputs.frame_slot, phase, 13));
        Srv(overflow.resource.Get(), DXGI_FORMAT_R32_UINT, Cpu(inputs.frame_slot, phase, 14));
        for (std::uint32_t index = 0; index < 5; ++index)
            Srv(guides[index].resource.Get(), kGuideFormats[index], Cpu(inputs.frame_slot, phase, 15 + index));
        Srv(overflow.resource.Get(), DXGI_FORMAT_R32_UINT, Cpu(inputs.frame_slot, phase, 20));
        Srv(guides[5].resource.Get(), kGuideFormats[5], Cpu(inputs.frame_slot, phase, 21));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_UINT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(overflow.resource.Get(), nullptr, &uav, Cpu(inputs.frame_slot, phase, kUavOffset));
        auto destination = inputs.table_cpu;
        destination.ptr += SIZE_T(phase * kHeroTransparencyTableSize) * inputs.descriptor_stride;
        device->CopyDescriptorsSimple(kHeroTransparencyTableSize, destination,
            Cpu(inputs.frame_slot, phase, 0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
};

struct HeroTransparencySystem::Impl
{
    std::shared_ptr<HeroTransparencyTargets> targets;
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
        std::array<ResourceVersion, 2> color;
        bool composed{};
    } pending;
};

HeroTransparencySystem::HeroTransparencySystem() : impl_(std::make_unique<Impl>()) {}
HeroTransparencySystem::~HeroTransparencySystem() = default;
std::uint64_t HeroTransparencyCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t HeroTransparencySystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }

Result HeroTransparencySystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ID3D12RootSignature *root, ViewExtent capacity, std::uint32_t layerCount, bool enabled,
    std::uint32_t frameSlots, const ShaderReader &readShader, HeroTransparencyCandidate &candidate) const
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
    for (std::uint32_t index = 0; index < next->guides.size(); ++index)
        if (auto created = CreateTexture(allocator, capacity, kGuideFormats[index],
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, next->guides[index]); !created) return created;
    for (auto &color : next->color)
        if (auto created = CreateTexture(allocator, capacity, kColor,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, color); !created) return created;
    if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R32_UINT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, next->overflow); !created) return created;

    std::vector<std::byte> vertex;
    if (auto read = readShader("render/shader/hero_transparency_vs.dxil", vertex); !read) return read;
    if (vertex.empty()) return Failure(ErrorCode::TaskFailed, "Hero transparency indexed vertex shader is empty.");
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
        std::vector<std::byte> pixel;
        if (auto read = readShader(kPixelProducts[index], pixel); !read) return read;
        if (pixel.empty()) return Failure(ErrorCode::TaskFailed, "Hero transparency pixel product is empty: " + std::string(kPixelProducts[index]));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root; pipeline.VS = {vertex.data(), vertex.size()}; pipeline.PS = {pixel.data(), pixel.size()};
        pipeline.SampleMask = UINT_MAX; pipeline.SampleDesc.Count = 1;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pipeline.RasterizerState.DepthClipEnable = TRUE;
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
            pipeline.NumRenderTargets = static_cast<UINT>(kGuideFormats.size());
            for (std::uint32_t target = 0; target < kGuideFormats.size(); ++target)
            {
                pipeline.RTVFormats[target] = kGuideFormats[target];
                pipeline.BlendState.RenderTarget[target].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }
            pipeline.BlendState.IndependentBlendEnable = TRUE;
        }
        else { pipeline.NumRenderTargets = 1; pipeline.RTVFormats[0] = kColor; }
        const auto native = device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&next->pipelines[index]));
        if (FAILED(native)) return NativeFailure("Create hero transparency pipeline " + std::string(kPixelProducts[index]), native);
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
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors = 8;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->dsv_heap));
    if (FAILED(native)) return NativeFailure("Create hero transparency DSVs", native);
    next->dsv_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format = DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    for (std::uint32_t layer = 0; layer < next->layer_count; ++layer)
    {
        device->CreateDepthStencilView(next->front[layer].resource.Get(), &dsv, next->Dsv(layer));
        device->CreateDepthStencilView(next->exit[layer].resource.Get(), &dsv, next->Dsv(4 + layer));
        device->CreateRenderTargetView(next->identity[layer].resource.Get(), nullptr, next->Rtv(layer));
    }
    for (std::uint32_t index = 0; index < next->guides.size(); ++index)
    {
        device->CreateRenderTargetView(next->guides[index].resource.Get(), nullptr, next->Rtv(4 + index));
        next->Srv(next->guides[index].resource.Get(), kGuideFormats[index], next->Export(index));
    }
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
    auto include = [&](const AllocationResource &resource) { if (resource.allocation) next->bytes += resource.allocation->GetSize(); };
    include(next->constants); include(next->overflow);
    for (const auto &resource : next->front) include(resource);
    for (const auto &resource : next->identity) include(resource);
    for (const auto &resource : next->exit) include(resource);
    for (const auto &resource : next->guides) include(resource);
    for (const auto &resource : next->color) include(resource);
    candidate.targets_ = std::move(next);
    return Result::Success();
}

Result HeroTransparencySystem::Commit(HeroTransparencyCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete pending hero transparency before replacing targets.");
    if (!candidate.targets_) return Failure(ErrorCode::InvalidArgument, "Hero transparency candidate was not prepared.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "Old hero transparency targets are still in GPU use.");
    impl_->targets = std::move(candidate.targets_);
    return Result::Success();
}

Result HeroTransparencySystem::RegisterLayers(RenderGraphBuilder &graph, const HeroTransparencyInputs &inputs,
    HeroTransparencyOutputs &outputs)
{
    outputs = {};
    auto storage = impl_->targets;
    if (!storage) return Failure(ErrorCode::InvalidState, "Prepare and commit hero transparency before registration.");
    if (!storage->enabled) return Result::Success();
    if (impl_->pending.graph || !storage->usable)
        return Failure(ErrorCode::InvalidState, "Hero transparency is pending or its previous graph failed.");
    const auto active = inputs.render_active;
    if (!ValidExtent(active) || active.width > storage->capacity.width || active.height > storage->capacity.height ||
        inputs.frame_slot >= storage->frame_slot_count || !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed) ||
        !FitsDepth(inputs.opaque_depth, active) || !Fits(inputs.hdr, active, kColor) ||
        !Fits(inputs.accumulation, active, DXGI_FORMAT_R32G32B32A32_FLOAT) ||
        !Fits(inputs.revealage, active, DXGI_FORMAT_R16_FLOAT) ||
        !inputs.accumulation_rtv.ptr || !inputs.revealage_rtv.ptr || !inputs.shader_heap ||
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
    pending.scene_reads.assign(inputs.scene_reads.begin(), inputs.scene_reads.end());
    auto import = [&](AllocationResource &resource, std::string_view name) {
        const auto handle = graph.ImportTexture({resource.resource.Get(), resource.graph_completion, resource.graph_queue}, resource.graph_access, name);
        pending.imports.push_back({&resource, handle}); return graph.Version(handle);
    };
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
    {
        pending.front[layer] = import(storage->front[layer], "Hero.FrontDepth");
        pending.identity[layer] = import(storage->identity[layer], "Hero.InstanceIdentity");
        pending.exit[layer] = import(storage->exit[layer], "Hero.ExitDepth");
        storage->FillTable(inputs, layer, layer, HeroTransparencyPhase::Front, inputs.hdr.resource);
        storage->FillTable(inputs, 4 + layer, layer, HeroTransparencyPhase::Exit, inputs.hdr.resource);
    }
    for (std::uint32_t index = 0; index < storage->color.size(); ++index)
        pending.color[index] = import(storage->color[index], "Hero.CompositeHDR");
    std::array<ResourceVersion, 6> guides;
    for (std::uint32_t index = 0; index < guides.size(); ++index) guides[index] = import(storage->guides[index], "Hero.NearestFrontGuide");
    auto overflow = import(storage->overflow, "Hero.LayerOverflow");
    storage->FillTable(inputs, kTailPhase, 0, HeroTransparencyPhase::Tail, inputs.hdr.resource);
    storage->FillTable(inputs, kGuidePhase, 0, HeroTransparencyPhase::Guide, inputs.hdr.resource);
    auto readScene = [&](PassBuilder &pass) {
        pass.Read(inputs.opaque_depth.version, Access::ShaderRead);
        for (const auto &read : pending.scene_reads) pass.Read(read.version, read.access);
    };
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
    {
        auto front = graph.AddPass(90 + layer * 2, "HeroPeelFront", QueueHint::Direct);
        readScene(front);
        if (layer) front.Read(pending.front[layer - 1], Access::ShaderRead);
        pending.front[layer] = front.Write(pending.front[layer], Access::DepthWrite);
        pending.identity[layer] = front.Write(pending.identity[layer], Access::RenderTarget);
        front.SetExecute([storage, inputs, layer](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            constexpr float clear[]{0, 0, 0, 0};
            const auto target = storage->Rtv(layer), depth = storage->Dsv(layer);
            commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            commands->ClearRenderTargetView(target, clear, 0, nullptr);
            commands->OMSetRenderTargets(1, &target, FALSE, &depth);
            inputs.draw(commands, storage->pipelines[0].Get(), HeroTransparencyPhase::Front, layer, storage->Gpu(inputs, layer));
        });
        auto exit = graph.AddPass(91 + layer * 2, "HeroPeelExit", QueueHint::Direct);
        readScene(exit); exit.Read(pending.front[layer], Access::ShaderRead); exit.Read(pending.identity[layer], Access::ShaderRead);
        pending.exit[layer] = exit.Write(pending.exit[layer], Access::DepthWrite);
        exit.SetExecute([storage, inputs, layer](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            const auto depth = storage->Dsv(4 + layer);
            commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            commands->OMSetRenderTargets(0, nullptr, FALSE, &depth);
            inputs.draw(commands, storage->pipelines[1].Get(), HeroTransparencyPhase::Exit, layer, storage->Gpu(inputs, 4 + layer));
        });
    }
    auto clear = graph.AddPass(98, "HeroClearOverflow", QueueHint::Direct);
    overflow = clear.Write(overflow, Access::UnorderedWrite);
    clear.SetExecute([storage, inputs](RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ID3D12DescriptorHeap *heaps[]{inputs.shader_heap}; commands->SetDescriptorHeaps(1, heaps);
        constexpr UINT zero[]{0, 0, 0, 0};
        commands->ClearUnorderedAccessViewUint(storage->Gpu(inputs, kTailPhase, kUavOffset),
            storage->Cpu(inputs.frame_slot, kTailPhase, kUavOffset), storage->overflow.resource.Get(), zero, 0, nullptr);
    });
    auto tail = graph.AddPass(99, "HeroOitTail", QueueHint::Direct);
    readScene(tail); tail.Read(inputs.hdr.version, Access::ShaderRead);
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
    {
        tail.Read(pending.front[layer], Access::ShaderRead); tail.Read(pending.identity[layer], Access::ShaderRead);
    }
    overflow = tail.Write(overflow, Access::UnorderedWrite);
    outputs.accumulation = tail.ReadWrite(inputs.accumulation.version, Access::RenderTarget);
    outputs.revealage = tail.ReadWrite(inputs.revealage.version, Access::RenderTarget);
    tail.SetExecute([storage, inputs](RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{inputs.accumulation_rtv, inputs.revealage_rtv};
        commands->OMSetRenderTargets(2, targets, FALSE, nullptr);
        inputs.draw(commands, storage->pipelines[2].Get(), HeroTransparencyPhase::Tail, 0, storage->Gpu(inputs, kTailPhase));
    });
    auto guide = graph.AddPass(100, "HeroNearestFrontGuides", QueueHint::Direct);
    readScene(guide); guide.Read(pending.front[0], Access::ShaderRead); guide.Read(pending.identity[0], Access::ShaderRead);
    guide.Read(overflow, Access::ShaderRead);
    for (auto &version : guides) version = guide.Write(version, Access::RenderTarget);
    guide.SetExecute([storage, inputs](RenderPassContext &context) {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6> targets;
        constexpr float zero[]{0, 0, 0, 0};
        for (std::uint32_t index = 0; index < targets.size(); ++index)
        {
            targets[index] = storage->Rtv(4 + index); commands->ClearRenderTargetView(targets[index], zero, 0, nullptr);
        }
        commands->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), FALSE, nullptr);
        inputs.draw(commands, storage->pipelines[3].Get(), HeroTransparencyPhase::Guide, 0, storage->Gpu(inputs, kGuidePhase));
    });
    outputs.depth = guides[0]; outputs.motion = guides[1]; outputs.history = guides[2];
    outputs.classification = guides[3]; outputs.metadata = guides[4]; outputs.transport = guides[5]; outputs.overflow = overflow;
    outputs.depth_resource = storage->guides[0].resource.Get(); outputs.motion_resource = storage->guides[1].resource.Get();
    outputs.history_resource = storage->guides[2].resource.Get(); outputs.classification_resource = storage->guides[3].resource.Get();
    outputs.metadata_resource = storage->guides[4].resource.Get(); outputs.transport_resource = storage->guides[5].resource.Get();
    outputs.overflow_resource = storage->overflow.resource.Get();
    outputs.depth_srv = storage->Export(0); outputs.motion_srv = storage->Export(1); outputs.history_srv = storage->Export(2);
    outputs.classification_srv = storage->Export(3); outputs.metadata_srv = storage->Export(4);
    outputs.transport_srv = storage->Export(5); outputs.overflow_srv = storage->Export(6);
    outputs.guides_table_gpu = storage->Gpu(inputs, kGuidePhase);
    impl_->pending = std::move(pending);
    return Result::Success();
}

Result HeroTransparencySystem::RegisterComposition(RenderGraphBuilder &graph, HeroTransparencyGuide background,
    HeroTransparencyOutputs &outputs)
{
    if (impl_->targets && !impl_->targets->enabled) return Result::Success();
    auto &pending = impl_->pending;
    if (pending.graph != &graph || pending.composed)
        return Failure(ErrorCode::InvalidState, "Register hero layers once before their composition on the same graph.");
    auto storage = pending.targets;
    const auto inputs = pending.inputs;
    if (!Fits(background, inputs.render_active, kColor))
        return Failure(ErrorCode::InvalidArgument, "Hero composition requires an initialized RGBA16F background at the active extent.");
    auto source = background.version;
    auto *sourceResource = background.resource;
    for (std::uint32_t step = 0; step < storage->layer_count; ++step)
    {
        const auto layer = storage->layer_count - 1 - step, target = step % 2;
        const auto phase = kCompositePhase + layer;
        storage->FillTable(inputs, phase, layer, HeroTransparencyPhase::Composite, sourceResource);
        auto copy = graph.AddPass(101 + step * 2, "HeroCopyBackground", QueueHint::Direct);
        copy.Read(source, Access::CopySource);
        pending.color[target] = copy.Write(pending.color[target], Access::CopyDest);
        copy.SetExecute([storage, sourceResource, target, active = inputs.render_active](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            D3D12_TEXTURE_COPY_LOCATION destination{}, sourceLocation{};
            destination.pResource = storage->color[target].resource.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sourceLocation.pResource = sourceResource; sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, 0, 0, active.width, active.height, 1};
            commands->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, &box);
        });
        auto compose = graph.AddPass(102 + step * 2, "HeroLayerComposite", QueueHint::Direct);
        compose.Read(source, Access::ShaderRead); compose.Read(inputs.opaque_depth.version, Access::ShaderRead);
        compose.Read(pending.front[layer], Access::ShaderRead); compose.Read(pending.identity[layer], Access::ShaderRead);
        compose.Read(pending.exit[layer], Access::ShaderRead);
        if (layer + 1 < storage->layer_count) compose.Read(pending.front[layer + 1], Access::ShaderRead);
        for (const auto &read : pending.scene_reads) compose.Read(read.version, read.access);
        pending.color[target] = compose.ReadWrite(pending.color[target], Access::RenderTarget);
        compose.SetExecute([storage, inputs, layer, phase, target](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            const auto rtv = storage->Rtv(10 + target);
            commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            inputs.draw(commands, storage->pipelines[4].Get(), HeroTransparencyPhase::Composite, layer, storage->Gpu(inputs, phase));
        });
        source = pending.color[target]; sourceResource = storage->color[target].resource.Get();
        outputs.corrected = source; outputs.corrected_resource = sourceResource; outputs.corrected_srv = storage->Export(7 + target);
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
    pending = {};
    return Result::Success();
}
Result HeroTransparencySystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete pending hero transparency before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "Hero transparency reset requires completed GPU uses.");
    impl_->targets.reset(); return Result::Success();
}
} // namespace hs
