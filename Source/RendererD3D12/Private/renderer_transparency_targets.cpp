#include "renderer_transparency_targets.hpp"
#include "renderer_pipeline_cache.hpp"

#include <cstring>
#include <iterator>
#include <utility>

namespace hs
{
using namespace translucency_detail;

namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kConstantStride = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr DXGI_FORMAT kOpticalComponentFormat = DXGI_FORMAT_R16_UINT;
constexpr std::array<DXGI_FORMAT, 6> kGuideFormats{
    DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
    kColor, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_R16G16_FLOAT};
constexpr std::array<std::string_view, 5> kPixelProducts{
    "render/shader/layered_translucency_peel_front_ps.dxil", "render/shader/layered_translucency_gather_exit_depth_ps.dxil",
    "render/shader/layered_translucency_accumulate_unpeeled_ps.dxil", "render/shader/layered_translucency_composite_front_ps.dxil",
    "render/shader/layered_translucency_composite_layer_ps.dxil"};
#if defined(HS_DEVELOPMENT_TOOLS)
constexpr std::array<std::string_view, 5> kWorkPixelProducts{
    "diagnostics/shader/layered_translucency_peel_front_ps_translucency_diagnostics.dxil", "diagnostics/shader/layered_translucency_gather_exit_depth_ps_translucency_diagnostics.dxil",
    "diagnostics/shader/layered_translucency_accumulate_unpeeled_ps_translucency_diagnostics.dxil", "diagnostics/shader/layered_translucency_composite_front_ps_translucency_diagnostics.dxil",
    "diagnostics/shader/layered_translucency_composite_layer_ps_translucency_diagnostics.dxil"};
#endif
Result NativeFailure(std::string_view operation, HRESULT error)
{
    return Failure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
        std::to_string(static_cast<std::int32_t>(error)) + ").");
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
    if (FAILED(native)) return NativeFailure("Create translucency transparency texture", native);
    texture.graph_access = Access::Common;
    return Result::Success();
}
Result CheckFormat(ID3D12Device *device, DXGI_FORMAT format, bool depth)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    const auto native = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(native)) return NativeFailure("Check translucency transparency format", native);
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
        (!depth && !(support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD)) ||
        (depth && !(support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL)) ||
        (!depth && !(support.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET)))
        return Failure(ErrorCode::TaskFailed, "Translucency transparency texture format lacks required native access support.");
    return Result::Success();
}
} // namespace

namespace translucency_detail
{
Result Failure(ErrorCode code, std::string message)
{
    return Result::Failure(code, "hs_renderer_transparency", std::move(message));
}
bool ValidExtent(ViewExtent extent)
{
    return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
void WriteTextureSrv(ID3D12Device *device, ID3D12Resource *resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = format;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource, &view, handle);
}
} // namespace translucency_detail

void LayeredTranslucencyTargets::InitializeTable(std::byte *mapped, std::uint32_t phase, std::uint32_t layer, LayeredTranslucencyPhase semantic)
{
    const std::array<std::uint32_t, 4> values{layer, layer_count, static_cast<std::uint32_t>(semantic), 0};
    std::memcpy(mapped + SIZE_T(phase) * kConstantStride, values.data(), sizeof(values));
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{constants.resource->GetGPUVirtualAddress() + UINT64(phase) * kConstantStride,
                                      kConstantStride};
    device->CreateConstantBufferView(&cbv, Cpu(phase, 0));
}

// All phase tables share these texture bindings. Only their constants and composite backgrounds differ.
static void WriteTranslucencySrvs(ID3D12Device *device, D3D12_CPU_DESCRIPTOR_HANDLE table,
    UINT stride, const LayeredTranslucencyTargets *targets)
{
    const auto write = [&](UINT slot, DXGI_FORMAT format, ID3D12Resource *resource) {
        WriteTextureSrv(device, resource, format, {table.ptr + SIZE_T(1 + slot) * stride});
    };
    for (UINT layer = 0; layer < kLayeredTranslucencyMaxLayers; ++layer)
    {
        write(HS_TRANSLUCENCY_SRV_FRONT_DEPTH + layer, DXGI_FORMAT_R32_FLOAT,
            targets ? targets->front[layer].resource.Get() : nullptr);
        write(HS_TRANSLUCENCY_SRV_FRONT_IDENTITY + layer, DXGI_FORMAT_R32G32_UINT,
            targets ? targets->identity[layer].resource.Get() : nullptr);
        write(HS_TRANSLUCENCY_SRV_FRONT_OPTICAL_COMPONENT + layer, kOpticalComponentFormat,
            targets ? targets->optical_component[layer].resource.Get() : nullptr);
    }
    write(HS_TRANSLUCENCY_SRV_EXIT, targets ? targets->exit_format : DXGI_FORMAT_R32G32B32A32_FLOAT,
        targets ? targets->exit.resource.Get() : nullptr);
    write(HS_TRANSLUCENCY_SRV_BACKGROUND, kColor, nullptr);
    write(HS_TRANSLUCENCY_SRV_GUIDE_DEPTH, kGuideFormats[0], targets ? targets->front[0].resource.Get() : nullptr);
    for (UINT index : kCompositeGuideIndices)
        write(HS_TRANSLUCENCY_SRV_GUIDE_DEPTH + index, kGuideFormats[index],
            targets ? targets->guides[index].resource.Get() : nullptr);
    write(HS_TRANSLUCENCY_SRV_GUIDE_METADATA, kGuideFormats[4], targets ? targets->identity[0].resource.Get() : nullptr);
}

void InitializeNullTranslucencyDescriptors(ID3D12Device *device, D3D12_CPU_DESCRIPTOR_HANDLE table, UINT stride)
{
    device->CreateConstantBufferView(nullptr, table);
    WriteTranslucencySrvs(device, table, stride, nullptr);
    for (UINT phase = 1; phase < kLayeredTranslucencyPhaseCount; ++phase)
        device->CopyDescriptorsSimple(kLayeredTranslucencyTableSize,
            {table.ptr + SIZE_T(phase * kLayeredTranslucencyTableSize) * stride}, table,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

TranslucencySystem::TranslucencySystem() : impl_(std::make_unique<Impl>()) {}
TranslucencySystem::~TranslucencySystem() = default;
std::uint64_t LayeredTranslucencyCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t TranslucencySystem::AllocatedBytes() const noexcept { return impl_->targets ? impl_->targets->bytes : 0; }
#if defined(HS_DEVELOPMENT_TOOLS)
std::array<const AllocationResource *, 14> TranslucencySystem::DiagnosticResources() const noexcept
{
    if (!impl_->targets || !impl_->targets->enabled || !impl_->targets->usable ||
        !impl_->diagnostic_submission_valid) return {};
    const auto &targets = *impl_->targets;
    std::array<const AllocationResource *, 14> resources{
        &targets.front[0], &targets.guides[1], &targets.guides[2],
        &targets.guides[3], &targets.identity[0], &targets.guides[5],
        &targets.color[impl_->diagnostic_color_index]};
    if (targets.layer_count > 1)
    {
        resources[7] = &targets.front[1];
        resources[13] = &targets.identity[1];
    }
    for (std::uint32_t layer = 0; layer < targets.layer_count; ++layer)
        resources[8 + layer] = &targets.optical_component[layer];
    resources[12] = &targets.exit;
    return resources;
}
#endif

Result TranslucencySystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ID3D12RootSignature *root, ViewExtent capacity, std::uint32_t layerCount, bool enabled,
    std::uint32_t frameSlots, const ShaderReader &readShader, RendererPipelineCache &pipelineCache,
    LayeredTranslucencyCandidate &candidate, bool captureTranslucencyDiagnostics, TranslucencyDiagnosticsMode mode) const
{
    if (!device || !allocator || !root || !ValidExtent(capacity) || layerCount > 4 || (enabled && !layerCount) || !frameSlots ||
        frameSlots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / kLayeredTranslucencyDescriptorsPerFrame ||
        (enabled && !readShader))
        return Failure(ErrorCode::InvalidArgument, "Invalid translucency transparency device, root, extent, quality, frame slots or shader reader.");
    auto next = std::make_shared<LayeredTranslucencyTargets>();
    next->device = device; next->allocator = allocator; next->root = root;
    next->capacity = capacity; next->enabled = enabled; next->layer_count = layerCount;
    next->frame_slot_count = frameSlots; next->slot_completion.resize(frameSlots);
    if (!enabled) { candidate.targets_ = std::move(next); return Result::Success(); }
#if defined(HS_DEVELOPMENT_TOOLS)
    if (captureTranslucencyDiagnostics)
    {
        next->work = std::make_unique<TranslucencyDiagnosticsCapture>();
        if (auto prepared = next->work->Initialize(device, allocator, frameSlots, capacity, mode); !prepared) return prepared;
    }
    else
#else
    static_cast<void>(captureTranslucencyDiagnostics);
    static_cast<void>(mode);
#endif
    {
        D3D12_QUERY_HEAP_DESC queries{};
        queries.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
        queries.Count = frameSlots * kLayeredTranslucencyMaxLayers;
        const auto native = device->CreateQueryHeap(&queries, IID_PPV_ARGS(&next->occupancy_queries));
        if (FAILED(native)) return NativeFailure("Create translucency occupancy queries", native);
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
                if (FAILED(created)) return NativeFailure("Create translucency occupancy result", created);
                result.graph_access = Access::CopyDest;
                next->bytes += result.allocation->GetSize();
            }
    }
    if (auto checked = CheckFormat(device, DXGI_FORMAT_D32_FLOAT, true); !checked) return checked;
    if (auto checked = CheckFormat(device, DXGI_FORMAT_R32G32_UINT, false); !checked) return checked;
    if (auto checked = CheckFormat(device, kOpticalComponentFormat, false); !checked) return checked;
    if (auto checked = CheckFormat(device, kColor, false); !checked) return checked;
    for (const auto format : kGuideFormats)
        if (auto checked = CheckFormat(device, format, false); !checked) return checked;
    for (std::uint32_t layer = 0; layer < next->layer_count; ++layer)
    {
        if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R32_TYPELESS,
                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, next->front[layer]); !created) return created;
        if (auto created = CreateTexture(allocator, capacity, DXGI_FORMAT_R32G32_UINT,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, next->identity[layer]); !created) return created;
        if (auto created = CreateTexture(allocator, capacity, kOpticalComponentFormat,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, next->optical_component[layer]); !created) return created;
    }
    next->exit_format = layerCount == 1 ? DXGI_FORMAT_R32_FLOAT :
        layerCount == 2 ? DXGI_FORMAT_R32G32_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT;
    constexpr auto exitRequired = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
        D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT exitSupport{next->exit_format};
    auto exitQuery = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &exitSupport, sizeof(exitSupport));
    if (FAILED(exitQuery)) return NativeFailure("Check translucency exit format", exitQuery);
    if ((exitSupport.Support1 & exitRequired) != exitRequired && next->exit_format != DXGI_FORMAT_R32G32B32A32_FLOAT)
    {
        next->exit_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        exitSupport = {next->exit_format};
        exitQuery = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &exitSupport, sizeof(exitSupport));
        if (FAILED(exitQuery)) return NativeFailure("Check translucency exit format", exitQuery);
    }
    if ((exitSupport.Support1 & exitRequired) != exitRequired)
        return Failure(ErrorCode::TaskFailed, "Translucency exit depth format lacks required texture, load, render or blend support.");
    if (auto created = CreateTexture(allocator, capacity, next->exit_format,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, next->exit); !created) return created;
    for (std::uint32_t index = 1; index < next->guides.size(); ++index)
    {
        if (index == 4) continue;
        if (auto created = CreateTexture(allocator, capacity, kGuideFormats[index],
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, next->guides[index]); !created) return created;
    }
    for (auto &color : next->color)
        if (auto created = CreateTexture(allocator, capacity, kColor,
                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, color); !created) return created;

    std::vector<std::byte> vertex;
    if (auto read = readShader("render/shader/scene_vs.dxil", vertex); !read) return read;
    if (vertex.empty()) return Failure(ErrorCode::TaskFailed, "Translucency transparency indexed vertex shader is empty.");
    std::vector<std::byte> current_vertex;
    if (auto read = readShader("render/shader/scene_current_vs.dxil", current_vertex); !read) return read;
    if (current_vertex.empty()) return Failure(ErrorCode::TaskFailed, "Translucency current-pose vertex shader is empty.");
    constexpr D3D12_INPUT_ELEMENT_DESC layout[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"MATERIAL", 0, DXGI_FORMAT_R16_UINT, 0, 72, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"OPTICAL_COMPONENT", 0, DXGI_FORMAT_R16_UINT, 0, 74, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    for (std::uint32_t index = 0; index < next->pipelines.size(); ++index)
    {
#if defined(HS_DEVELOPMENT_TOOLS)
        const auto product = captureTranslucencyDiagnostics ? kWorkPixelProducts[index] : kPixelProducts[index];
#else
        const auto product = kPixelProducts[index];
#endif
        std::vector<std::byte> pixel;
        if (auto read = readShader(product, pixel); !read) return read;
        if (pixel.empty()) return Failure(ErrorCode::TaskFailed, "Translucency transparency pixel product is empty: " + std::string(product));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        const auto &phase_vertex = index == 3 ? vertex : current_vertex;
        pipeline.pRootSignature = root; pipeline.VS = {phase_vertex.data(), phase_vertex.size()}; pipeline.PS = {pixel.data(), pixel.size()};
        pipeline.SampleMask = UINT_MAX; pipeline.SampleDesc.Count = 1;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
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
        if (index == 0)
        {
            pipeline.DepthStencilState.DepthEnable = TRUE;
            pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
            pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            pipeline.NumRenderTargets = 2;
            pipeline.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
            pipeline.RTVFormats[1] = kOpticalComponentFormat;
        }
        else if (index == 1)
        {
            pipeline.NumRenderTargets = 1; pipeline.RTVFormats[0] = next->exit_format;
            auto &blend = pipeline.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = blend.DestBlend = blend.SrcBlendAlpha = blend.DestBlendAlpha = D3D12_BLEND_ONE;
            blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_MAX;
        }
        else if (index == 2)
        {
            // Reverse-Z: exactly the tail's former z > opaque-depth predicate.
            // Read-only depth allows early rejection before material work.
            pipeline.DepthStencilState.DepthEnable = TRUE;
            pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
            pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            pipeline.NumRenderTargets = 2; pipeline.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
            pipeline.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
            pipeline.BlendState.IndependentBlendEnable = TRUE;
            auto &accumulation = pipeline.BlendState.RenderTarget[0];
            accumulation.BlendEnable = TRUE; accumulation.SrcBlend = accumulation.DestBlend = D3D12_BLEND_ONE;
            accumulation.BlendOp = accumulation.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            accumulation.SrcBlendAlpha = accumulation.DestBlendAlpha = D3D12_BLEND_ONE;
            auto &reveal = pipeline.BlendState.RenderTarget[1];
            reveal.BlendEnable = TRUE; reveal.SrcBlend = D3D12_BLEND_ONE; reveal.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            reveal.BlendOp = reveal.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            reveal.SrcBlendAlpha = D3D12_BLEND_ZERO; reveal.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            reveal.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
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
        if (index == 3 || index == 4)
        {
            // Shared current-position evaluation with the front peel. Keep the
            // identity check in the PS to reject coincident other instances.
            pipeline.DepthStencilState.DepthEnable = TRUE;
            pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
            pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        }
        const auto native = pipelineCache.CreateGraphics(&pipeline, IID_PPV_ARGS(&next->pipelines[index]));
        if (FAILED(native)) return NativeFailure("Create translucency transparency pipeline " + std::string(product), native);
#if defined(HS_DEVELOPMENT_TOOLS)
        if (next->work && index < kPixelProducts.size()) next->work->Pipeline(index, pipeline, phase_vertex, pixel, product);
#endif
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kLayeredTranslucencyDescriptorsPerFrame;
    auto native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->tables));
    if (FAILED(native)) return NativeFailure("Create translucency transparency source tables", native);
    next->srv_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors = 2;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->exports));
    if (FAILED(native)) return NativeFailure("Create translucency transparency exports", native);
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 15;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->rtv_heap));
    if (FAILED(native)) return NativeFailure("Create translucency transparency RTVs", native);
    next->rtv_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors = 12;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->dsv_heap));
    if (FAILED(native)) return NativeFailure("Create translucency transparency DSVs", native);
    next->dsv_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format = DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    for (std::uint32_t layer = 0; layer < next->layer_count; ++layer)
    {
        device->CreateDepthStencilView(next->front[layer].resource.Get(), &dsv, next->Dsv(layer));
        auto read_only = dsv;
        read_only.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        device->CreateDepthStencilView(next->front[layer].resource.Get(), &read_only, next->Dsv(8 + layer));
        device->CreateRenderTargetView(next->identity[layer].resource.Get(), nullptr, next->Rtv(layer));
        device->CreateRenderTargetView(next->optical_component[layer].resource.Get(), nullptr, next->Rtv(4 + layer));
    }
    device->CreateRenderTargetView(next->exit.resource.Get(), nullptr, next->Rtv(8));
    for (std::uint32_t index = 1; index < next->guides.size(); ++index)
    {
        if (index == 4) continue;
        device->CreateRenderTargetView(next->guides[index].resource.Get(), nullptr,
            next->Rtv(index == 5 ? 12 : 8 + index));
    }
    for (std::uint32_t index = 0; index < next->color.size(); ++index)
    {
        device->CreateRenderTargetView(next->color[index].resource.Get(), nullptr, next->Rtv(13 + index));
        WriteTextureSrv(device, next->color[index].resource.Get(), kColor, next->Export(index));
    }
    D3D12MA::ALLOCATION_DESC uploadAllocation{}; uploadAllocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload{};
    upload.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload.Width = UINT64(kLayeredTranslucencyPhaseCount) * kConstantStride;
    upload.Height = 1; upload.DepthOrArraySize = 1; upload.MipLevels = 1; upload.SampleDesc.Count = 1;
    upload.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    native = allocator->CreateResource(&uploadAllocation, &upload, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        &next->constants.allocation, IID_PPV_ARGS(next->constants.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create translucency transparency constants", native);
    void *mapped{}; D3D12_RANGE noRead{0, 0};
    native = next->constants.resource->Map(0, &noRead, &mapped);
    if (FAILED(native)) return NativeFailure("Map translucency transparency constants", native);
    auto *phase_constants = static_cast<std::byte *>(mapped);
    WriteTranslucencySrvs(device, next->Cpu(0, 0), next->srv_stride, next.get());
    for (UINT phase = 1; phase < kLayeredTranslucencyPhaseCount; ++phase)
        device->CopyDescriptorsSimple(kLayeredTranslucencySrvCount, next->Cpu(phase, 1), next->Cpu(0, 1),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (std::uint32_t layer = 0; layer < kLayeredTranslucencyMaxLayers; ++layer)
    {
        next->InitializeTable(phase_constants, layer, layer, LayeredTranslucencyPhase::Front);
        next->InitializeTable(phase_constants, kCompositePhase + layer, layer, LayeredTranslucencyPhase::Composite);
        // All but the first far-to-near step read the preceding ping-pong target.
        if (layer + 1 < layerCount)
            device->CopyDescriptorsSimple(1, next->Cpu(kCompositePhase + layer, 1 + HS_TRANSLUCENCY_SRV_BACKGROUND),
                next->Export((layerCount - layer - 2) % 2), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    next->InitializeTable(phase_constants, 4, 0, LayeredTranslucencyPhase::Exit);
    next->InitializeTable(phase_constants, kTailPhase, 0, LayeredTranslucencyPhase::Tail);
    next->constants.resource->Unmap(0, nullptr);
    auto include = [&](const AllocationResource &resource) { if (resource.allocation) next->bytes += resource.allocation->GetSize(); };
    include(next->constants);
    for (const auto &resource : next->front) include(resource);
    for (const auto &resource : next->identity) include(resource);
    for (const auto &resource : next->optical_component) include(resource);
    include(next->exit);
    for (const auto &resource : next->guides) include(resource);
    for (const auto &resource : next->color) include(resource);
#if defined(HS_DEVELOPMENT_TOOLS)
    if (next->work) next->bytes += next->work->Status().readback_bytes;
#endif
    candidate.targets_ = std::move(next);
    return Result::Success();
}

Result TranslucencySystem::Commit(LayeredTranslucencyCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete pending translucency transparency before replacing targets.");
    if (!candidate.targets_) return Failure(ErrorCode::InvalidArgument, "Translucency transparency candidate was not prepared.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "Old translucency transparency targets are still in GPU use.");
#if defined(HS_DEVELOPMENT_TOOLS)
    if (auto collected = ConsumeCompletedWork(completed, impl_->completed_work); !collected) return collected;
    if (impl_->targets && impl_->targets->work && impl_->targets->work->Status().pending_frames)
        return Failure(ErrorCode::InvalidState, "Old translucency contributor queries are still in GPU use.");
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
        return Failure(ErrorCode::InvalidState, "Translucency transparency target generation is exhausted.");
    candidate.targets_->generation = ++impl_->next_generation;
    impl_->targets = std::move(candidate.targets_);
#if defined(HS_DEVELOPMENT_TOOLS)
    impl_->diagnostic_color_index = 0;
    impl_->diagnostic_submission_valid = false;
#endif
    return Result::Success();
}

Result TranslucencySystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending.graph) return Failure(ErrorCode::InvalidState, "Complete pending translucency transparency before reset.");
    if (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))
        return Failure(ErrorCode::InvalidState, "Translucency transparency reset requires completed GPU uses.");
#if defined(HS_DEVELOPMENT_TOOLS)
    if (auto collected = ConsumeCompletedWork(completed, impl_->completed_work); !collected) return collected;
    if (impl_->targets && impl_->targets->work && impl_->targets->work->Status().pending_frames)
        return Failure(ErrorCode::InvalidState, "Translucency transparency reset requires completed native contributor queries.");
    impl_->retired_work = {};
#endif
    impl_->targets.reset();
#if defined(HS_DEVELOPMENT_TOOLS)
    impl_->diagnostic_submission_valid = false;
    impl_->diagnostic_color_index = 0;
#endif
    return Result::Success();
}
#if defined(HS_DEVELOPMENT_TOOLS)
Result TranslucencySystem::ConsumeCompletedWork(const CompletionSet &completed, std::vector<RendererTranslucencyDiagnosticsSample> &output)
{
    if (impl_->targets && impl_->targets->work)
        if (auto result = impl_->targets->work->Collect(completed, impl_->completed_work); !result) return result;
    if (&output != &impl_->completed_work)
    {
        if (output.size() + impl_->completed_work.size() >
            240 * TranslucencyDiagnosticsRetainedLayout::kMaxContributors * kLayeredTranslucencyPhaseCount)
        {
            impl_->retired_work.overflowed = true;
            return Failure(ErrorCode::InvalidState, "Translucency contributor output exceeded its bounded pending rows; preserved rows require capture drain.");
        }
        output.insert(output.end(), std::make_move_iterator(impl_->completed_work.begin()), std::make_move_iterator(impl_->completed_work.end()));
        impl_->completed_work.clear();
    }
    return Result::Success();
}
RendererTranslucencyDiagnosticsStatus TranslucencySystem::WorkStatus() const
{
    auto status = impl_->targets && impl_->targets->work ? impl_->targets->work->Status() : RendererTranslucencyDiagnosticsStatus{};
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
Result TranslucencySystem::ConsumeCompletedWork(const CompletionSet &, std::vector<RendererTranslucencyDiagnosticsSample> &output)
{ output.clear(); return Result::Success(); }
RendererTranslucencyDiagnosticsStatus TranslucencySystem::WorkStatus() const { return {}; }
#endif
} // namespace hs
