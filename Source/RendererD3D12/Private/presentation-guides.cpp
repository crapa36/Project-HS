#include "presentation-guides.hpp"
#include "d3d12_resources.hpp"
#include <D3D12MemAlloc.h>
#include <array>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t DescriptorCount = 11, SrvCount = 9;
struct Constants
{
    std::uint32_t render_width, render_height, output_width, output_height;
    float paper_white, peak;
    std::uint32_t hdr, motion_contains_sky;
    DirectX::XMFLOAT4X4 inverse_current_view_projection, previous_view_projection;
    DirectX::XMFLOAT4 camera_position;
    float jitter_uv[2];
    std::uint32_t reset_history, transparent_enabled;
};
static_assert(sizeof(Constants) == 192);
Result Failure(ErrorCode code, std::string text)
{
    return Result::Failure(code, "hs_renderer_presentation_guides", std::move(text));
}
Result NativeFailure(std::string_view operation, HRESULT code)
{
    return Failure(ErrorCode::TaskFailed, std::string(operation) + " failed (HRESULT " +
                   std::to_string(static_cast<std::int32_t>(code)) + ").");
}
bool Extent(ViewExtent extent)
{
    return extent.width && extent.height && extent.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        extent.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
bool Fits(PresentationGuideResource input, ViewExtent extent, DXGI_FORMAT format)
{
    if (!input.resource || !input.version.resource || input.version.resource.kind != ResourceKind::Texture) return false;
    const auto desc = input.resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width >= extent.width &&
        desc.Height >= extent.height && desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1 &&
        desc.MipLevels == 1 && desc.Format == format;
}
Result CreateTexture(ID3D12Device *device, D3D12MA::Allocator *allocator, ViewExtent extent,
    DXGI_FORMAT format, bool renderTarget, AllocationResource &output)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    auto native = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));
    if (FAILED(native)) return NativeFailure("Query native presentation guide format", native);
    const auto required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
        (renderTarget ? D3D12_FORMAT_SUPPORT1_RENDER_TARGET : 0);
    if ((support.Support1 & required) != required || (!renderTarget && !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)))
        return Failure(ErrorCode::TaskFailed, "The actual device cannot load/store the selected native presentation guide format.");
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = extent.width; desc.Height = extent.height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Flags = renderTarget ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    native = allocator->CreateResource(&allocation, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
        &output.allocation, IID_PPV_ARGS(output.resource.ReleaseAndGetAddressOf()));
    if (FAILED(native)) return NativeFailure("Create native presentation guide texture", native);
    output.graph_access = Access::Common;
    return Result::Success();
}
} // namespace

struct PresentationGuidesTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> colors, guides;
    ComPtr<ID3D12DescriptorHeap> descriptors, rtvs;
    ViewExtent capacity, output;
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN}, ui_format{DXGI_FORMAT_UNKNOWN};
    DXGI_COLOR_SPACE_TYPE color_space{};
    SdkPresentationOwner provider{SdkPresentationOwner::None};
    std::uint32_t descriptor_stride{}, rtv_stride{};
    std::uint64_t bytes{};
    CompletionSet completion;
    bool usable{true};
    struct Slot
    {
        AllocationResource hudless, ui, composite, depth, motion, devtools;
        CompletionSet completion;
    };
    std::vector<Slot> slots;
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot, std::uint32_t index) const
    {
        auto handle = descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slot * DescriptorCount + index) * descriptor_stride;
        return handle;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t slot, std::uint32_t index) const
    {
        auto handle = descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += UINT64(slot * DescriptorCount + index) * descriptor_stride;
        return handle;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(std::uint32_t slot, std::uint32_t index) const
    {
        auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(slot * 4 + index) * rtv_stride;
        return handle;
    }
    void Srv(std::uint32_t slot, std::uint32_t index, ID3D12Resource *resource, DXGI_FORMAT formatValue)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = formatValue; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &view, Cpu(slot, index));
    }
};
struct PresentationGuides::Impl
{
    std::shared_ptr<PresentationGuidesTargets> targets;
    struct Import { AllocationResource *allocation; ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::shared_ptr<PresentationGuidesTargets> targets;
        std::vector<Import> imports;
        std::uint32_t slot{};
        bool tagged{};
        ViewExtent active;
        float peak{};
    } pending;
};
PresentationGuides::PresentationGuides() = default;
PresentationGuides::~PresentationGuides() = default;
std::uint64_t PresentationGuidesCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->bytes : 0; }
std::uint64_t PresentationGuides::AllocatedBytes() const noexcept { return impl_ && impl_->targets ? impl_->targets->bytes : 0; }

Result PresentationGuides::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator,
    ViewExtent capacity, ViewExtent output, DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE colorSpace,
    SdkPresentationOwner provider, std::uint32_t frameSlots, const ShaderReader &reader, PresentationGuidesCandidate &candidate) const
{
    const bool hdr = format == DXGI_FORMAT_R10G10B10A2_UNORM && colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const bool sdr = format == DXGI_FORMAT_R8G8B8A8_UNORM && colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (!device || !allocator || !Extent(capacity) || !Extent(output) || (!hdr && !sdr) || !reader ||
        provider == SdkPresentationOwner::None || !frameSlots || frameSlots > D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1 / DescriptorCount)
        return Failure(ErrorCode::InvalidArgument, "Presentation guides require active native FG, valid SDR/HDR10 output, device, allocator and shader products.");
    auto next = std::make_shared<PresentationGuidesTargets>();
    next->device = device; next->allocator = allocator; next->capacity = capacity; next->output = output;
    next->format = format; next->color_space = colorSpace; next->provider = provider;
    // DLSS-G explicitly excludes UINT10 alpha for UI; preserve its native
    // supported floating point alpha instead. Intel/AMD require matching format.
    next->ui_format = provider == SdkPresentationOwner::Streamline ? DXGI_FORMAT_R16G16B16A16_FLOAT : format;
    next->slots.resize(frameSlots);
    D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, SrvCount, 0, 23, 0},
                                   {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 23, 0}};
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[0].Constants = {0, 23, sizeof(Constants) / 4};
    parameters[1].ParameterType = parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &ranges[0]}; parameters[2].DescriptorTable = {1, &ranges[1]};
    D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters = 3; root.pParameters = parameters;
    ComPtr<ID3DBlob> signature, diagnostics;
    auto native = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &diagnostics);
    if (FAILED(native)) return NativeFailure("Serialize presentation guide root", native);
    native = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&next->root));
    if (FAILED(native)) return NativeFailure("Create presentation guide root", native);
    std::array<std::vector<std::byte>, 3> bytecode;
    constexpr std::array<std::string_view, 3> products{"render/shader/sdk_presentation_vs.dxil",
        "render/shader/sdk_presentation_ps.dxil", "render/shader/sdk_presentation_guides_cs.dxil"};
    for (std::size_t i = 0; i < products.size(); ++i)
    {
        if (auto result = reader(products[i], bytecode[i]); !result) return result;
        if (bytecode[i].empty()) return Failure(ErrorCode::TaskFailed, "Empty native presentation shader: " + std::string(products[i]));
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC colors{};
    colors.pRootSignature = next->root.Get();
    colors.VS = {bytecode[0].data(), bytecode[0].size()}; colors.PS = {bytecode[1].data(), bytecode[1].size()};
    colors.SampleMask = UINT_MAX; colors.SampleDesc.Count = 1;
    colors.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; colors.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    colors.RasterizerState.DepthClipEnable = TRUE;
    colors.DepthStencilState.DepthEnable = FALSE; colors.DepthStencilState.StencilEnable = FALSE;
    colors.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    colors.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    colors.NumRenderTargets = 3; colors.RTVFormats[0] = colors.RTVFormats[2] = format; colors.RTVFormats[1] = next->ui_format;
    colors.BlendState.IndependentBlendEnable = TRUE;
    for (auto &blend : colors.BlendState.RenderTarget)
    {
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    native = device->CreateGraphicsPipelineState(&colors, IID_PPV_ARGS(&next->colors));
    if (FAILED(native)) return NativeFailure("Create display-domain presentation conversion", native);
    D3D12_COMPUTE_PIPELINE_STATE_DESC guides{}; guides.pRootSignature = next->root.Get();
    guides.CS = {bytecode[2].data(), bytecode[2].size()};
    native = device->CreateComputePipelineState(&guides, IID_PPV_ARGS(&next->guides));
    if (FAILED(native)) return NativeFailure("Create undilated presentation guides", native);
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = frameSlots * DescriptorCount; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->descriptors));
    if (FAILED(native)) return NativeFailure("Create presentation descriptors", native);
    next->descriptor_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = frameSlots * 4; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    native = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&next->rtvs));
    if (FAILED(native)) return NativeFailure("Create presentation RTV descriptors", native);
    next->rtv_stride = device->GetDescriptorHandleIncrementSize(heap.Type);
    for (std::uint32_t i = 0; i < frameSlots; ++i)
    {
        auto &slot = next->slots[i];
        for (auto [resource, extent, resourceFormat, renderTarget] : {
            std::tuple{&slot.hudless, output, format, true}, std::tuple{&slot.ui, output, next->ui_format, true},
            std::tuple{&slot.composite, output, format, true}, std::tuple{&slot.depth, capacity, DXGI_FORMAT_R32_FLOAT, false},
            std::tuple{&slot.motion, capacity, DXGI_FORMAT_R16G16_FLOAT, false},
            std::tuple{&slot.devtools, output, DXGI_FORMAT_R16G16B16A16_FLOAT, true}})
        {
            if (auto result = CreateTexture(device, allocator, extent, resourceFormat, renderTarget, *resource); !result) return result;
            next->bytes += resource->allocation->GetSize();
        }
        std::uint32_t colorIndex{};
        for (auto *color : {&slot.hudless, &slot.ui, &slot.composite, &slot.devtools})
            device->CreateRenderTargetView(color->resource.Get(), nullptr, next->Rtv(i, colorIndex++));
        std::uint32_t uavIndex{};
        for (auto *guide : {&slot.depth, &slot.motion})
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = guide->resource->GetDesc().Format; view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(guide->resource.Get(), nullptr, &view, next->Cpu(i, SrvCount + uavIndex++));
        }
    }
    candidate.targets_ = std::move(next);
    return Result::Success();
}
Result PresentationGuides::Commit(PresentationGuidesCandidate &&candidate, const CompletionSet &completed)
{
    if (!candidate.targets_) return Failure(ErrorCode::InvalidArgument, "No prepared presentation guides.");
    if (impl_ && (impl_->pending.graph || (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed))))
        return Failure(ErrorCode::InvalidState, "Complete all old graph uses and SDK presentation before replacing guides.");
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->targets = std::move(candidate.targets_);
    return Result::Success();
}
Result PresentationGuides::Register(RenderGraphBuilder &graph, const PresentationGuidesInputs &input,
    PresentationGuidesOutputs &output, std::uint32_t colorPassId, std::uint32_t guidePassId,
    std::uint32_t devtoolsPassId)
{
    output = {};
    if (!impl_ || !impl_->targets || !impl_->targets->usable || impl_->pending.graph)
        return Failure(ErrorCode::InvalidState, "Presentation guides require completed reusable target storage.");
    auto storage = impl_->targets;
    if (!Extent(input.render_active) || input.render_active.width > storage->capacity.width ||
        input.render_active.height > storage->capacity.height || input.frame_slot >= storage->slots.size() ||
        !storage->slots[input.frame_slot].completion.IsSatisfiedBy(input.completed) || !colorPassId || !guidePassId || !devtoolsPassId ||
        colorPassId == guidePassId || colorPassId == devtoolsPassId || guidePassId == devtoolsPassId ||
        !std::isfinite(input.paper_white_nits) || !std::isfinite(input.peak_nits) || input.paper_white_nits <= 0 || input.peak_nits < input.paper_white_nits)
        return Failure(ErrorCode::InvalidArgument, "Invalid presentation extent, completed slot, pass IDs or luminance parameters.");
    if (!Fits(input.display_linear, storage->output, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        !Fits(input.encoded_ui, storage->output, DXGI_FORMAT_B8G8R8A8_UNORM) ||
        !Fits(input.motion, input.render_active, DXGI_FORMAT_R16G16_FLOAT) ||
        !(Fits(input.depth, input.render_active, DXGI_FORMAT_R32_FLOAT) ||
          Fits(input.depth, input.render_active, DXGI_FORMAT_R32_TYPELESS) || Fits(input.depth, input.render_active, DXGI_FORMAT_D32_FLOAT)))
        return Failure(ErrorCode::InvalidArgument, "Native presentation needs actual HUDless display-linear RGBA16F, premultiplied encoded BGRA8 UI, undilated UV motion and reversed device Z.");
    const auto slotIndex = input.frame_slot;
    if (input.transparent_guides_enabled &&
        (!Fits(input.transparent_depth, input.render_active, DXGI_FORMAT_R32_FLOAT) ||
         !Fits(input.transparent_motion, input.render_active, DXGI_FORMAT_R16G16_FLOAT) ||
         !Fits(input.transparent_classification, input.render_active, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
         !Fits(input.transparent_metadata, input.render_active, DXGI_FORMAT_R32G32_UINT)))
        return Failure(ErrorCode::InvalidArgument, "Native transparent depth/motion requires the actual nearest surface classification and identity guides.");
    auto &slot = storage->slots[slotIndex];
    storage->Srv(slotIndex, 0, input.display_linear.resource, DXGI_FORMAT_R16G16B16A16_FLOAT);
    storage->Srv(slotIndex, 1, input.encoded_ui.resource, DXGI_FORMAT_B8G8R8A8_UNORM);
    storage->Srv(slotIndex, 2, input.depth.resource, DXGI_FORMAT_R32_FLOAT);
    storage->Srv(slotIndex, 3, input.motion.resource, DXGI_FORMAT_R16G16_FLOAT);
    storage->Srv(slotIndex, 4, slot.devtools.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    storage->Srv(slotIndex, 5, input.transparent_guides_enabled ? input.transparent_depth.resource : nullptr, DXGI_FORMAT_R32_FLOAT);
    storage->Srv(slotIndex, 6, input.transparent_guides_enabled ? input.transparent_motion.resource : nullptr, DXGI_FORMAT_R16G16_FLOAT);
    storage->Srv(slotIndex, 7, input.transparent_guides_enabled ? input.transparent_classification.resource : nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
    storage->Srv(slotIndex, 8, input.transparent_guides_enabled ? input.transparent_metadata.resource : nullptr, DXGI_FORMAT_R32G32_UINT);
    Impl::Pending pending;
    pending.graph = &graph; pending.targets = storage; pending.slot = slotIndex;
    pending.active = input.render_active; pending.peak = input.peak_nits;
    const auto import = [&](AllocationResource &resource, std::string_view name) {
        const auto handle = graph.ImportTexture({resource.resource.Get(), resource.graph_completion, resource.graph_queue}, resource.graph_access, name);
        pending.imports.push_back({&resource, handle});
        return PresentationGuideResource{graph.Version(handle), resource.resource.Get()};
    };
    auto hudless = import(slot.hudless, "SdkHudlessDisplay"), ui = import(slot.ui, "SdkUiDisplay"),
        composite = import(slot.composite, "SdkCompositeDisplay"), depth = import(slot.depth, "SdkUndilatedDepth"),
        motion = import(slot.motion, "SdkUndilatedMotionUv"), devtools = import(slot.devtools, "SdkDevToolsUi");
    Constants constants{input.render_active.width, input.render_active.height, storage->output.width, storage->output.height,
        input.paper_white_nits, input.peak_nits, storage->color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? 1U : 0U,
        input.motion_contains_sky ? 1U : 0U};
    DirectX::XMStoreFloat4x4(&constants.inverse_current_view_projection,
        DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&input.camera.current_unjittered_view_projection)));
    constants.previous_view_projection = input.camera.previous_unjittered_view_projection;
    constants.camera_position = {input.camera.position.x, input.camera.position.y, input.camera.position.z, 1};
    constants.jitter_uv[0] = input.camera.jitter_pixels[0] / input.render_active.width;
    constants.jitter_uv[1] = input.camera.jitter_pixels[1] / input.render_active.height;
    constants.reset_history = input.camera.reset_history ? 1U : 0U;
    constants.transparent_enabled = input.transparent_guides_enabled ? 1U : 0U;
    auto devtoolsPass = graph.AddPass(devtoolsPassId, "SDK DevTools UI", QueueHint::Direct);
    devtools.version = devtoolsPass.Write(devtools.version, Access::RenderTarget);
    devtoolsPass.SetExecuteOnOwner([storage, slotIndex, draw = input.draw_devtools](RenderPassContext &context) -> Result {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        const auto rtv = storage->Rtv(slotIndex, 3);
        constexpr float transparent[4]{};
        commands->ClearRenderTargetView(rtv, transparent, 0, nullptr);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        return draw ? draw(commands) : Result::Success();
    });
    auto color = graph.AddPass(colorPassId, "SDK Display Guides", QueueHint::Direct);
    const bool freshSlot = slot.completion.direct == 0 && slot.completion.compute == 0 && slot.completion.copy == 0;
    color.Read(input.display_linear.version, Access::ShaderRead); color.Read(input.encoded_ui.version, Access::ShaderRead);
    color.Read(devtools.version, Access::ShaderRead);
    hudless.version = color.Write(hudless.version, Access::RenderTarget);
    ui.version = color.Write(ui.version, Access::RenderTarget); composite.version = color.Write(composite.version, Access::RenderTarget);
    color.SetExecuteChecked([storage, slotIndex, constants, freshSlot](RenderPassContext &context) -> Result {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        if (freshSlot)
        {
            commands->DiscardResource(storage->slots[slotIndex].hudless.resource.Get(), nullptr);
            commands->DiscardResource(storage->slots[slotIndex].ui.resource.Get(), nullptr);
            commands->DiscardResource(storage->slots[slotIndex].composite.resource.Get(), nullptr);
        }
        ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()}; commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(storage->root.Get()); commands->SetPipelineState(storage->colors.Get());
        commands->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
        commands->SetGraphicsRootDescriptorTable(1, storage->Gpu(slotIndex, 0));
        commands->SetGraphicsRootDescriptorTable(2, storage->Gpu(slotIndex, SrvCount));
        const std::array rtvs{storage->Rtv(slotIndex, 0), storage->Rtv(slotIndex, 1), storage->Rtv(slotIndex, 2)};
        commands->OMSetRenderTargets(3, rtvs.data(), FALSE, nullptr);
        D3D12_VIEWPORT viewport{0, 0, static_cast<float>(storage->output.width), static_cast<float>(storage->output.height), 0, 1};
        D3D12_RECT scissor{0, 0, static_cast<LONG>(storage->output.width), static_cast<LONG>(storage->output.height)};
        commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); commands->DrawInstanced(3, 1, 0, 0);
        return Result::Success();
    });
    auto guides = graph.AddPass(guidePassId, "SDK Undilated Guides", QueueHint::Direct);
    guides.Read(input.depth.version, Access::ComputeRead); guides.Read(input.motion.version, Access::ComputeRead);
    if (input.transparent_guides_enabled)
        for (const auto &resource : {input.transparent_depth, input.transparent_motion, input.transparent_classification, input.transparent_metadata})
            guides.Read(resource.version, Access::ComputeRead);
    depth.version = guides.Write(depth.version, Access::UnorderedWrite); motion.version = guides.Write(motion.version, Access::UnorderedWrite);
    guides.SetExecuteChecked([storage, slotIndex, constants](RenderPassContext &context) -> Result {
        auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        ID3D12DescriptorHeap *heaps[]{storage->descriptors.Get()}; commands->SetDescriptorHeaps(1, heaps);
        commands->SetComputeRootSignature(storage->root.Get()); commands->SetPipelineState(storage->guides.Get());
        commands->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
        commands->SetComputeRootDescriptorTable(1, storage->Gpu(slotIndex, 0));
        commands->SetComputeRootDescriptorTable(2, storage->Gpu(slotIndex, SrvCount));
        commands->Dispatch((constants.render_width + 7) / 8, (constants.render_height + 7) / 8, 1);
        return Result::Success();
    });
    output = {hudless, ui, composite, depth, motion, storage};
    impl_->pending = std::move(pending);
    return Result::Success();
}
Result PresentationGuides::RegisterTagPass(RenderGraphBuilder &graph, const PresentationGuidesOutputs &output,
    PresentationGuideResource backBuffer, SdkPresentationFrame frame, SdkPresentation &presentation, std::uint32_t passId)
{
    if (!impl_ || !impl_->pending.graph || impl_->pending.graph != &graph || impl_->pending.tagged ||
        !output.owner || output.owner.get() != impl_->targets.get() || !passId || !backBuffer.resource || !backBuffer.version.resource ||
        presentation.PresentationOwner() != impl_->targets->provider)
        return Failure(ErrorCode::InvalidState, "SDK tag pass must match one registered guide graph and its sole selected presentation owner.");
    auto storage = impl_->targets;
    if (!Fits(backBuffer, storage->output, storage->format))
        return Failure(ErrorCode::InvalidArgument, "Presentation backbuffer version/format/extent does not match the resolved native chain.");
    auto tag = graph.AddPass(passId, "SDK Frame Generation Prepare", QueueHint::Direct);
    const bool intel = storage->provider == SdkPresentationOwner::Xess;
    const auto access = intel ? Access::CopySource : Access::ComputeRead;
    for (const auto &resource : {output.hudless, output.ui, output.depth, output.motion}) tag.Read(resource.version, access);
    tag.Read(backBuffer.version, Access::Present);
    frame.hudless = output.hudless.resource; frame.ui = output.ui.resource;
    frame.depth = output.depth.resource; frame.motion = output.motion.resource; frame.back_buffer = backBuffer.resource;
    frame.output_format = storage->format; frame.color_space = storage->color_space;
    frame.output_rect = {0, 0, storage->output.width, storage->output.height};
    frame.render_rect = {0, 0, impl_->pending.active.width, impl_->pending.active.height};
    frame.maximum_luminance = impl_->pending.peak;
    frame.hudless_state = frame.ui_state = frame.depth_state = frame.motion_state = intel ?
        D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    frame.back_buffer_state = D3D12_RESOURCE_STATE_PRESENT; frame.owners.push_back(storage);
    tag.SetExecuteOnOwner([frame = std::move(frame), &presentation](RenderPassContext &context) -> Result {
        return presentation.RecordDirect(static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList()), frame);
    });
    impl_->pending.tagged = true;
    return Result::Success();
}
Result PresentationGuides::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    if (!impl_ || !impl_->pending.graph || impl_->pending.graph != &graph)
        return Failure(ErrorCode::InvalidState, "Presentation completion does not match its registered graph.");
    auto &pending = impl_->pending;
    auto storage = pending.targets;
    storage->completion.Merge(completion); storage->slots[pending.slot].completion.Merge(completion);
    for (const auto &item : pending.imports)
    {
        const auto resourceCompletion = graph.ResourceCompletion(item.handle);
        storage->completion.Merge(resourceCompletion); storage->slots[pending.slot].completion.Merge(resourceCompletion);
        item.allocation->graph_completion.Merge(completion); item.allocation->graph_completion.Merge(resourceCompletion);
        if (success)
        {
            item.allocation->graph_access = graph.FinalAccess(item.handle); item.allocation->graph_queue = graph.FinalQueue(item.handle);
        }
    }
    if (!success) storage->usable = false;
    pending = {};
    return Result::Success();
}
Result PresentationGuides::ResetAfterCompletion(const CompletionSet &completed)
{
    if (!impl_) return Result::Success();
    if (impl_->pending.graph || (impl_->targets && !impl_->targets->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState, "Complete presentation guide graph uses and native SDK presentation before reset.");
    impl_.reset();
    return Result::Success();
}
} // namespace hs
