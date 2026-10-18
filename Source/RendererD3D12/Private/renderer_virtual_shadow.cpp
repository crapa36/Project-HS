#include "renderer_virtual_shadow.hpp"
#include "d3d12_resources.hpp"
#include "renderer_diagnostics.hpp"
#include "renderer_pipeline_cache.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <wrl/client.h>

namespace hs
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr std::uint32_t kComputeSrvs = 5, kComputeUavs = 9, kComputeDescriptors = kComputeSrvs+kComputeUavs;
constexpr std::uint32_t kIndexedStride = 24, kMeshStride = 16, kClearStride = 20;
struct alignas(16) GpuCaster
{
    DirectX::XMFLOAT4 current_bounds, previous_bounds;
    DirectX::XMUINT4 draw, indexed, properties;
};
static_assert(sizeof(GpuCaster) == 80);
static_assert(sizeof(VirtualShadowReceiver) == 32);
Result Invalid(std::string_view message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::string(message));
}
D3D12_RESOURCE_DESC Buffer(std::uint64_t bytes, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = std::max<std::uint64_t>(bytes, 4);
    d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc = {1, 0}; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    return d;
}
D3D12_RESOURCE_DESC BankDescription(ViewExtent extent)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = extent.width;
    d.Height = extent.height; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R32_TYPELESS; d.SampleDesc = {1, 0};
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    return d;
}
bool Add(std::uint64_t &total, std::uint64_t amount)
{
    if (amount > UINT64_MAX - total) return false;
    total += amount; return true;
}
bool Finite(DirectX::XMFLOAT4 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}
bool Same(DirectX::XMFLOAT4 a, DirectX::XMFLOAT4 b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
float Dot(DirectX::XMFLOAT3 a, DirectX::XMFLOAT4 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap *heap, std::uint32_t index, std::uint32_t stride)
{
    auto handle = heap->GetCPUDescriptorHandleForHeapStart(); handle.ptr += SIZE_T(index)*stride; return handle;
}
D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap *heap, std::uint32_t index, std::uint32_t stride)
{
    auto handle = heap->GetGPUDescriptorHandleForHeapStart(); handle.ptr += UINT64(index)*stride; return handle;
}
void RawSrv(ID3D12Device *device, ID3D12Resource *resource, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / 4); d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(resource, &d, handle);
}
void StructuredSrv(ID3D12Device *device, ID3D12Resource *resource, std::uint32_t stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.StructureByteStride = stride; d.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / stride);
    device->CreateShaderResourceView(resource, &d, handle);
}
void TextureSrv(ID3D12Device *device, ID3D12Resource *resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = format; d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; d.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(resource, &d, handle);
}
void Uav(ID3D12Device *device, ID3D12Resource *resource, std::uint32_t stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = static_cast<UINT>(resource->GetDesc().Width / (stride ? stride : 4));
    d.Buffer.StructureByteStride = stride;
    if (!stride) { d.Format = DXGI_FORMAT_R32_TYPELESS; d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW; }
    device->CreateUnorderedAccessView(resource, nullptr, &d, handle);
}
}

struct VirtualShadowTargets
{
    struct FrameSlot
    {
        AllocationResource upload, metadata, casters, receivers, pages, requests, draws;
        AllocationResource indexed, mesh, counts, clear, page_draw_counts;
        CompletionSet completion;
        std::vector<VirtualShadowDrawBatch> batches;
    };
    VirtualShadowProfile profile;
    ViewExtent extent;
    std::uint32_t columns{}, entries{}, words{}, descriptor_stride{}, dsv_stride{};
    std::uint64_t allocated{}, upload_bytes{}, caster_offset{512}, receiver_offset{};
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    VirtualShadowSystem::ShaderReader reader;
    RendererPipelineCache *cache{};
    AllocationResource residents, static_bank, dynamic_bank;
    std::vector<FrameSlot> frames;
    ComPtr<ID3D12DescriptorHeap> compute_heap, srv_heap, dsv_heap;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 5> compute;
    ComPtr<ID3D12PipelineState> clear_pso;
    ComPtr<ID3D12CommandSignature> clear_signature;
    CompletionSet lease;
    bool banks_initialized{};
};

namespace
{
Result Allocate(VirtualShadowTargets &t, AllocationResource &r, const D3D12_RESOURCE_DESC &description, bool upload = false)
{
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{}; clear.Format = DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth = 0.0f;
    const auto hr = t.allocator->CreateResource(&allocation, &description,
        upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON,
        description.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ? &clear : nullptr,
        &r.allocation, IID_PPV_ARGS(r.resource.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return HResultFailure("Create virtual shadow allocation", hr);
    r.graph_access = Access::Common;
    if (!Add(t.allocated, r.allocation->GetSize())) return Invalid("Virtual shadow allocation accounting overflow.");
    if (t.allocated > t.profile.gpu_budget_bytes) return Invalid("Virtual shadow native allocations exceed the service GPU budget.");
    return Result::Success();
}
Result Heap(VirtualShadowTargets &t, D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint32_t count, bool visible, ComPtr<ID3D12DescriptorHeap> &heap)
{
    D3D12_DESCRIPTOR_HEAP_DESC d{}; d.Type = type; d.NumDescriptors = count;
    d.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    const auto hr = t.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(heap.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return HResultFailure("Create virtual shadow descriptor heap", hr);
    // Native descriptor storage has no D3D12 allocation-size query. Account its
    // device descriptor stride as well as every D3D12MA allocation above.
    if (!Add(t.allocated, UINT64(count) * t.device->GetDescriptorHandleIncrementSize(type)) ||
        t.allocated > t.profile.gpu_budget_bytes) return Invalid("Virtual shadow descriptors exceed the service GPU budget.");
    return Result::Success();
}
Result Pipelines(VirtualShadowTargets &t)
{
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kComputeSrvs, 0, 21, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kComputeUavs, 0, 21, 0};
    std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[0].Descriptor = {0, 21};
    for (std::uint32_t i = 0; i < 2; ++i)
    {
        parameters[i+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i+1].DescriptorTable = {1, &ranges[i]};
    }
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants = {1, 21, 1};
    D3D12_ROOT_SIGNATURE_DESC description{};
    description.NumParameters = static_cast<UINT>(parameters.size()); description.pParameters = parameters.data();
    ComPtr<ID3DBlob> serialized, errors;
    auto hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
        serialized.ReleaseAndGetAddressOf(), errors.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return HResultFailure("Serialize virtual shadow root", hr);
    hr = t.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(t.root.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return HResultFailure("Create virtual shadow root", hr);
    if (t.cache)
        if (auto result = t.cache->RegisterRootSignature(t.root.Get(),
            {static_cast<const std::byte *>(serialized->GetBufferPointer()), serialized->GetBufferSize()}); !result) return result;
    constexpr std::array<std::string_view, 5> ids{
        "render/shader/virtual_shadow_init_cs.dxil", "render/shader/virtual_shadow_feedback_cs.dxil",
        "render/shader/virtual_shadow_resident_cs.dxil", "render/shader/virtual_shadow_cull_cs.dxil",
        "render/shader/virtual_shadow_publish_cs.dxil"};
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        std::vector<std::byte> code;
        if (auto result = t.reader(ids[i], code); !result) return result;
        if (code.empty()) return Invalid("Virtual shadow compute product is empty.");
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{}; d.pRootSignature = t.root.Get(); d.CS = {code.data(), code.size()};
        hr = t.cache ? t.cache->CreateCompute(&d, IID_PPV_ARGS(t.compute[i].ReleaseAndGetAddressOf()))
                     : t.device->CreateComputePipelineState(&d, IID_PPV_ARGS(t.compute[i].ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return HResultFailure("Create virtual shadow compute PSO", hr);
    }
    std::vector<std::byte> vs, ps;
    if (auto result = t.reader("render/shader/virtual_shadow_clear_vs.dxil", vs); !result) return result;
    if (auto result = t.reader("render/shader/virtual_shadow_clear_ps.dxil", ps); !result) return result;
    if (vs.empty() || ps.empty()) return Invalid("Virtual shadow clear product is empty.");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};
    graphics.pRootSignature = t.root.Get(); graphics.VS = {vs.data(), vs.size()}; graphics.PS = {ps.data(), ps.size()};
    for (auto &target : graphics.BlendState.RenderTarget)
    {
        target.SrcBlend = D3D12_BLEND_ONE; target.DestBlend = D3D12_BLEND_ZERO;
        target.BlendOp = D3D12_BLEND_OP_ADD; target.SrcBlendAlpha = D3D12_BLEND_ONE;
        target.DestBlendAlpha = D3D12_BLEND_ZERO; target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP; target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    graphics.SampleMask = UINT_MAX;
    graphics.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; graphics.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable = TRUE;
    graphics.DepthStencilState.DepthEnable = TRUE; graphics.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    graphics.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    graphics.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    graphics.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    graphics.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    graphics.DepthStencilState.BackFace = graphics.DepthStencilState.FrontFace;
    graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.DSVFormat = DXGI_FORMAT_D32_FLOAT; graphics.SampleDesc = {1, 0};
    hr = t.cache ? t.cache->CreateGraphics(&graphics, IID_PPV_ARGS(t.clear_pso.ReleaseAndGetAddressOf()))
                 : t.device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(t.clear_pso.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return HResultFailure("Create virtual shadow tile clear PSO", hr);
    D3D12_INDIRECT_ARGUMENT_DESC arguments[2]{};
    arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    arguments[0].Constant = {3, 0, 1}; arguments[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = kClearStride; signature.NumArgumentDescs = 2; signature.pArgumentDescs = arguments;
    hr = t.device->CreateCommandSignature(&signature, t.root.Get(), IID_PPV_ARGS(t.clear_signature.ReleaseAndGetAddressOf()));
    return FAILED(hr) ? HResultFailure("Create virtual shadow tile clear signature", hr) : Result::Success();
}
void BindCompute(ID3D12GraphicsCommandList *list, const VirtualShadowTargets &t, const VirtualShadowTargets::FrameSlot &f,
    std::uint32_t slot, std::uint32_t pipeline)
{
    ID3D12DescriptorHeap *heaps[]{t.compute_heap.Get()}; list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(t.root.Get()); list->SetPipelineState(t.compute[pipeline].Get());
    list->SetComputeRootConstantBufferView(0, f.upload.resource->GetGPUVirtualAddress());
    list->SetComputeRootDescriptorTable(1, Gpu(t.compute_heap.Get(), slot*kComputeDescriptors, t.descriptor_stride));
    list->SetComputeRootDescriptorTable(2, Gpu(t.compute_heap.Get(), slot*kComputeDescriptors+kComputeSrvs, t.descriptor_stride));
    list->SetComputeRoot32BitConstant(3, 0, 0);
}
ResourceVersion Import(RenderGraphBuilder &graph, AllocationResource &r, std::string_view name, bool texture = false)
{
    return texture ? graph.Version(graph.ImportTexture({r.resource.Get(), r.graph_completion, r.graph_queue}, r.graph_access, name))
        : graph.Version(graph.ImportBuffer({r.resource.Get(), r.graph_completion, r.graph_queue}, r.graph_access, name));
}
}

struct VirtualShadowSystem::Impl
{
    struct Tracked { AllocationResource *resource; ResourceHandle handle; };
    std::shared_ptr<VirtualShadowTargets> active;
    std::vector<std::shared_ptr<VirtualShadowTargets>> retired;
    std::vector<Tracked> tracked;
    const RenderGraphBuilder *pending_graph{};
    std::uint32_t pending_slot{};
    std::uint32_t last_successful_slot{};
    VirtualShadowConstants successful{}, pending{};
    bool has_success{}, faulted{};
    void Collect(const CompletionSet &completed)
    {
        std::erase_if(retired, [&](const auto &t) { return t->lease.IsSatisfiedBy(completed); });
    }
};
VirtualShadowSystem::VirtualShadowSystem() : impl_(std::make_unique<Impl>()) {}
VirtualShadowSystem::~VirtualShadowSystem() = default;
std::uint64_t VirtualShadowCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->allocated : 0; }
std::uint64_t VirtualShadowCandidate::HostStagingBytes() const noexcept
{
    if (!targets_) return 0;
    std::uint64_t bytes=sizeof(VirtualShadowTargets)+targets_->frames.capacity()*sizeof(VirtualShadowTargets::FrameSlot);
    for (const auto &f : targets_->frames) bytes += f.batches.capacity()*sizeof(VirtualShadowDrawBatch);
    return bytes;
}
Result VirtualShadowSystem::Prepare(ID3D12Device *device, D3D12MA::Allocator *allocator, const VirtualShadowProfile &profile,
    const ShaderReader &reader, VirtualShadowCandidate &candidate, RendererPipelineCache *cache) const
{
    candidate.targets_.reset();
    if (!profile.enabled) return Result::Success();
    if (!device || !allocator || !reader || !profile.gpu_budget_bytes || !profile.physical_pages || !profile.filter_samples || profile.filter_samples > 16 ||
        !profile.max_casters || !profile.max_receivers || !profile.max_draw_groups || !profile.frame_slots ||
        profile.frame_slots > 16 || profile.max_draw_groups > 32767 ||
        profile.command_capacity < UINT64(profile.max_draw_groups)*2 || profile.command_capacity >= kVirtualShadowPassBit ||
        !profile.render_capacity.width || !profile.render_capacity.height)
        return Invalid("Virtual shadow profile has an invalid native capacity.");
    auto t = std::make_shared<VirtualShadowTargets>();
    t->profile = profile; t->device = device; t->allocator = allocator; t->reader = reader; t->cache = cache;
    for (const auto side : profile.virtual_sides)
    {
        if (!side || side > 256 || UINT64(t->entries)+UINT64(side)*side > 65535)
            return Invalid("Virtual shadow clipmap table exceeds native dispatch/address capacity.");
        t->entries += side*side;
    }
    if (profile.max_casters > 65535u*64u || UINT64(profile.max_casters)*t->entries > UINT32_MAX ||
        UINT64(profile.max_casters)*sizeof(GpuCaster) > UINT32_MAX ||
        UINT64(profile.max_receivers)*sizeof(VirtualShadowReceiver) > UINT32_MAX ||
        UINT64(profile.command_capacity)*sizeof(GpuShadowDrawRecord) > UINT32_MAX ||
        UINT64(t->entries)*profile.max_draw_groups*2*sizeof(std::uint32_t) > UINT32_MAX)
        return Invalid("Virtual shadow raw-buffer/dispatch capacity exceeds the shader ABI.");
    t->columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(double(profile.physical_pages))));
    const auto rows = (profile.physical_pages+t->columns-1)/t->columns;
    if (UINT64(t->columns)*kVirtualShadowTile > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        UINT64(rows)*kVirtualShadowTile > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return Invalid("Virtual shadow physical bank exceeds the native texture extent.");
    t->extent = {t->columns*kVirtualShadowTile, rows*kVirtualShadowTile};
    t->words = (t->entries+31)/32;
    t->receiver_offset = t->caster_offset+UINT64(profile.max_casters)*sizeof(GpuCaster);
    t->upload_bytes = t->receiver_offset+UINT64(profile.max_receivers)*sizeof(VirtualShadowReceiver);
    t->descriptor_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    t->dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    t->frames.resize(profile.frame_slots);
    if (auto result = Allocate(*t, t->residents, Buffer(UINT64(profile.physical_pages)*32, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)); !result) return result;
    if (auto result = Allocate(*t, t->static_bank, BankDescription(t->extent)); !result) return result;
    if (auto result = Allocate(*t, t->dynamic_bank, BankDescription(t->extent)); !result) return result;
    t->static_bank.resource->SetName(L"VirtualShadow.StaticDepth");
    t->dynamic_bank.resource->SetName(L"VirtualShadow.DynamicDepth");
    if (t->static_bank.allocation->GetSize() > profile.cache_budget_bytes / 2 ||
        t->dynamic_bank.allocation->GetSize() > profile.cache_budget_bytes - t->static_bank.allocation->GetSize())
        return Invalid("Actual native shadow depth banks exceed the authored cache capacity.");
    for (auto &f : t->frames)
    {
        if (auto result = Allocate(*t, f.upload, Buffer(t->upload_bytes), true); !result) return result;
        if (auto result = Allocate(*t, f.metadata, Buffer(sizeof(VirtualShadowConstants))); !result) return result;
        if (auto result = Allocate(*t, f.casters, Buffer(UINT64(profile.max_casters)*sizeof(GpuCaster))); !result) return result;
        if (auto result = Allocate(*t, f.receivers, Buffer(UINT64(profile.max_receivers)*sizeof(VirtualShadowReceiver))); !result) return result;
        const std::array<std::pair<AllocationResource *, std::uint64_t>, 8> buffers{{
            {&f.pages, UINT64(t->entries)*32}, {&f.requests, UINT64(t->words)*4},
            {&f.draws, UINT64(profile.command_capacity)*32}, {&f.indexed, UINT64(profile.command_capacity)*kIndexedStride},
            {&f.mesh, UINT64(profile.command_capacity)*kMeshStride}, {&f.counts, UINT64(profile.max_draw_groups*2+2)*4},
            {&f.clear, UINT64(profile.physical_pages)*2*kClearStride},
            {&f.page_draw_counts, UINT64(t->entries)*profile.max_draw_groups*2*sizeof(std::uint32_t)}}};
        for (auto [resource, bytes] : buffers)
            if (auto result = Allocate(*t, *resource, Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)); !result) return result;
        f.batches.reserve(profile.max_draw_groups*2);
    }
    if (auto result = Heap(*t, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, profile.frame_slots*kComputeDescriptors, true, t->compute_heap); !result) return result;
    if (auto result = Heap(*t, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, profile.frame_slots*5, false, t->srv_heap); !result) return result;
    if (auto result = Heap(*t, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, false, t->dsv_heap); !result) return result;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format = DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(t->static_bank.resource.Get(), &dsv, Cpu(t->dsv_heap.Get(), 0, t->dsv_stride));
    device->CreateDepthStencilView(t->dynamic_bank.resource.Get(), &dsv, Cpu(t->dsv_heap.Get(), 1, t->dsv_stride));
    for (std::uint32_t slot = 0; slot < profile.frame_slots; ++slot)
    {
        auto &f = t->frames[slot]; const auto compute_base = slot*kComputeDescriptors, main_base = slot*5;
        StructuredSrv(device, f.casters.resource.Get(), sizeof(GpuCaster), Cpu(t->compute_heap.Get(), compute_base+2, t->descriptor_stride));
        StructuredSrv(device, f.receivers.resource.Get(), sizeof(VirtualShadowReceiver), Cpu(t->compute_heap.Get(), compute_base+3, t->descriptor_stride));
        StructuredSrv(device, f.pages.resource.Get(), sizeof(GpuShadowPageEntry), Cpu(t->compute_heap.Get(), compute_base+4, t->descriptor_stride));
        const std::array<AllocationResource *, kComputeUavs> uavs{&f.pages, &t->residents, &f.requests, &f.draws, &f.indexed, &f.mesh, &f.counts, &f.clear, &f.page_draw_counts};
        constexpr std::array<std::uint32_t, kComputeUavs> strides{32,32,0,32,0,0,0,0,0};
        for (std::uint32_t i = 0; i < kComputeUavs; ++i) Uav(device, uavs[i]->resource.Get(), strides[i], Cpu(t->compute_heap.Get(), compute_base+kComputeSrvs+i, t->descriptor_stride));
        RawSrv(device, f.metadata.resource.Get(), Cpu(t->srv_heap.Get(), main_base, t->descriptor_stride));
        StructuredSrv(device, f.pages.resource.Get(), 32, Cpu(t->srv_heap.Get(), main_base+1, t->descriptor_stride));
        TextureSrv(device, t->static_bank.resource.Get(), DXGI_FORMAT_R32_FLOAT, Cpu(t->srv_heap.Get(), main_base+2, t->descriptor_stride));
        TextureSrv(device, t->dynamic_bank.resource.Get(), DXGI_FORMAT_R32_FLOAT, Cpu(t->srv_heap.Get(), main_base+3, t->descriptor_stride));
        StructuredSrv(device, f.draws.resource.Get(), 32, Cpu(t->srv_heap.Get(), main_base+4, t->descriptor_stride));
    }
    if (auto result = Pipelines(*t); !result) return result;
    candidate.targets_ = std::move(t); return Result::Success();
}
Result VirtualShadowSystem::Commit(VirtualShadowCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending_graph) return Invalid("Cannot replace virtual shadows while a graph is awaiting submission.");
    impl_->Collect(completed);
    if (impl_->active && !impl_->active->lease.IsSatisfiedBy(completed)) impl_->retired.push_back(impl_->active);
    impl_->active = std::move(candidate.targets_); impl_->has_success = false; impl_->faulted = false;
    impl_->successful = {}; impl_->pending = {}; return Result::Success();
}

Result VirtualShadowSystem::Register(RenderGraphBuilder &graph, const VirtualShadowInputs &in, VirtualShadowOutputs &out)
{
    out = {};
    impl_->Collect(in.completed);
    if (!impl_->active) { out.descriptor_count = 0; return Result::Success(); }
    if (impl_->pending_graph || impl_->faulted)
        return Invalid("Virtual shadows require completion/reset of the preceding graph before registration.");
    const auto t = impl_->active;
    if (in.frame_slot >= t->frames.size() || !in.render_active.width || !in.render_active.height ||
        in.render_active.width > t->profile.render_capacity.width || in.render_active.height > t->profile.render_capacity.height ||
        in.casters.size() > t->profile.max_casters || in.receivers.size() > t->profile.max_receivers ||
        in.draw_groups.size() > t->profile.max_draw_groups || !in.depth.resource || !in.normal.resource ||
        !in.depth_resource || !in.normal_resource || !in.draw || !std::isfinite(in.depth_span) || in.depth_span <= 0)
        return Invalid("Virtual shadow frame exceeds its capacity or lacks required native bindings.");
    auto &f = t->frames[in.frame_slot];
    if (!f.completion.IsSatisfiedBy(in.completed)) return Invalid("Virtual shadow frame slot still has GPU readers.");
    const auto rounded_width = UINT64((in.render_active.width+7u)&~7u);
    const auto feedback_rows = std::max<UINT64>(in.render_active.height, (in.receivers.size()+rounded_width-1)/rounded_width);
    if ((in.render_active.width+7u)/8u > 65535u || (feedback_rows+7u)/8u > 65535u)
        return Invalid("Virtual shadow receiver dispatch exceeds native dimensions.");
    VirtualShadowConstants constants{};
    constants.inverse_view_projection = in.inverse_view_projection;
    constants.view_origin = {in.view_origin.x, in.view_origin.y, in.view_origin.z, 0};
    constants.bank = {float(t->extent.width), float(t->extent.height), float(t->columns), float(kVirtualShadowTile)};
    const bool requested = in.light.type == LightType::Directional && in.light.stable_id && in.light.generation &&
        (in.light.shadow_flags & kLightShadowRequested) != 0;
    const auto vector = DirectX::XMVectorSet(in.light.direction.x, in.light.direction.y, in.light.direction.z, 0);
    const float length = DirectX::XMVectorGetX(DirectX::XMVector3Length(vector));
    if (requested && (!std::isfinite(length) || length <= 0.0f)) return Invalid("Directional virtual shadow has an invalid propagation direction.");
    const auto forward = requested ? DirectX::XMVector3Normalize(vector) : DirectX::XMVectorSet(0, -1, 0, 0);
    const auto seed = std::abs(DirectX::XMVectorGetY(forward)) > 0.95f
        ? DirectX::XMVectorSet(0, 0, 1, 0) : DirectX::XMVectorSet(0, 1, 0, 0);
    const auto right = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(seed, forward));
    const auto up = DirectX::XMVector3Cross(forward, right);
    DirectX::XMStoreFloat4(&constants.light_right, right);
    DirectX::XMStoreFloat4(&constants.light_up, up);
    DirectX::XMStoreFloat4(&constants.light_forward, forward);
    bool reset = !impl_->has_success ||
        impl_->successful.identity.x != std::uint32_t(in.light.stable_id) ||
        impl_->successful.identity.y != std::uint32_t(in.light.stable_id >> 32u) ||
        impl_->successful.identity.z != in.light.generation ||
        !Same(impl_->successful.light_right, constants.light_right) ||
        !Same(impl_->successful.light_up, constants.light_up) ||
        !Same(impl_->successful.light_forward, constants.light_forward);
    std::uint32_t table_base{};
    for (std::uint32_t level = 0; level < 3; ++level)
    {
        const float extent = in.coverage_extents[level];
        if (!std::isfinite(extent) || extent <= 0 || (level && extent <= in.coverage_extents[level-1]))
            return Invalid("Virtual shadow clipmap coverage must have increasing positive extents.");
        const auto side = t->profile.virtual_sides[level];
        const float page_side = extent*2.0f/float(side);
        if (!std::isfinite(page_side) || page_side <= 0 || page_side/float(kVirtualShadowInner) <= 0)
            return Invalid("Virtual shadow world-page scale exceeds finite shader precision.");
        const double key_x = std::floor(double(Dot(in.clipmap_center, constants.light_right))/page_side);
        const double key_y = std::floor(double(Dot(in.clipmap_center, constants.light_up))/page_side);
        if (!std::isfinite(key_x) || !std::isfinite(key_y) || key_x < double(INT32_MIN)+side ||
            key_y < double(INT32_MIN)+side || key_x > double(INT32_MAX)-side || key_y > double(INT32_MAX)-side)
            return Invalid("Virtual shadow absolute clipmap key exceeds its signed ABI.");
        constants.clipmaps[level] = {page_side, page_side/float(kVirtualShadowInner), extent, 0};
        constants.windows[level] = {std::int32_t(key_x)-std::int32_t(side/2), std::int32_t(key_y)-std::int32_t(side/2),
            std::int32_t(side), std::int32_t(table_base)};
        table_base += side*side;
        reset |= impl_->has_success && impl_->successful.clipmaps[level].x != page_side;
    }
    // Z never follows camera near/far or jitter. Retain an epoch while all full
    // world caster/receiver bounds fit; grow a quantized interval when necessary.
    const double center_z = Dot(in.clipmap_center, constants.light_forward);
    double minimum = center_z-in.depth_span*0.25, maximum = center_z+in.depth_span*0.25;
    for (const auto &caster : in.casters)
    {
        if ((caster.present && (!Finite(caster.current_bounds) || caster.current_bounds.w < 0)) ||
            (caster.previous_present && (!Finite(caster.previous_bounds) || caster.previous_bounds.w < 0)))
            return Invalid("Virtual shadow snapshot has nonconservative/nonfinite caster bounds.");
        for (const auto bounds : {std::pair{caster.present, caster.current_bounds}, std::pair{caster.previous_present, caster.previous_bounds}})
            if (bounds.first)
            {
                const auto sphere = bounds.second;
                const double z = Dot({sphere.x, sphere.y, sphere.z}, constants.light_forward);
                minimum = std::min(minimum, z-sphere.w); maximum = std::max(maximum, z+sphere.w);
            }
        if (caster.present)
        {
            if (caster.draw_group >= in.draw_groups.size() ||
                (in.draw_groups[caster.draw_group].frontend == VirtualShadowFrontend::Indexed && !caster.index_count) ||
                (in.draw_groups[caster.draw_group].frontend == VirtualShadowFrontend::Mesh && (!caster.meshlet_count || caster.meshlet_count > 65535u)))
                return Invalid("Virtual shadow caster lacks a complete native geometry/material draw group.");
        }
    }
    for (const auto &receiver : in.receivers)
    {
        if (!Finite(receiver.minimum) || !Finite(receiver.maximum) ||
            (receiver.minimum.w != 0.0f && receiver.minimum.w != 1.0f) || receiver.minimum.x > receiver.maximum.x ||
            receiver.minimum.y > receiver.maximum.y || receiver.minimum.z > receiver.maximum.z)
            return Invalid("Virtual shadow receiver has invalid world bounds.");
        for (std::uint32_t corner = 0; corner < 8; ++corner)
        {
            const double z = Dot({corner&1u ? receiver.maximum.x : receiver.minimum.x,
                corner&2u ? receiver.maximum.y : receiver.minimum.y,
                corner&4u ? receiver.maximum.z : receiver.minimum.z}, constants.light_forward);
            minimum = std::min(minimum, z); maximum = std::max(maximum, z);
        }
    }
    minimum -= 1.0; maximum += 1.0;
    if (!reset && minimum >= impl_->successful.depth.x && maximum <=
        double(impl_->successful.depth.x)+impl_->successful.depth.w)
        constants.depth = impl_->successful.depth;
    else
    {
        reset = true;
        double span = std::exp2(std::ceil(std::log2(std::max<double>(in.depth_span, 2.0*(maximum-minimum)))));
        const double midpoint = (minimum+maximum)*0.5;
        double start = std::round(midpoint/(span*0.25))*(span*0.25)-span*0.5;
        if (!std::isfinite(span) || !std::isfinite(start) || span > std::numeric_limits<float>::max()/2)
            return Invalid("Virtual shadow stable depth interval exceeds finite shader precision.");
        constants.depth = {float(start), float(1.0/span), 0, float(span)};
        // Expand for final float rounding rather than accepting clipped casters.
        if (minimum < constants.depth.x || maximum > double(constants.depth.x)+constants.depth.w)
        {
            span *= 2; start = std::round(midpoint/(span*0.25))*(span*0.25)-span*0.5;
            constants.depth = {float(start), float(1.0/span), 0, float(span)};
        }
    }
    constants.depth.z = std::max(2.0e-7f, constants.clipmaps[0].y*0.35f*constants.depth.y);
    const auto previous_epoch = impl_->has_success ? impl_->successful.identity.w : 0u;
    if (reset && previous_epoch == UINT32_MAX) return Invalid("Virtual shadow depth epoch is exhausted.");
    constants.identity = {std::uint32_t(in.light.stable_id), std::uint32_t(in.light.stable_id>>32u), in.light.generation,
        reset ? previous_epoch+1u : previous_epoch};
    constants.counts = {std::uint32_t(in.casters.size()), std::uint32_t(in.receivers.size()), t->entries, t->profile.physical_pages};
    constants.limits = {UINT32_MAX, std::uint32_t(in.draw_groups.size()), in.render_active.width, in.render_active.height};
    const auto group_count = static_cast<std::uint32_t>(in.draw_groups.size());
    const auto segment_count = group_count*2;
    // Reuse the fixed command allocation across active groups, rather than reserving it for absent groups.
    const auto segment_capacity = segment_count ? t->profile.command_capacity/segment_count : 0u;
    assert(UINT64(segment_count)*segment_capacity <= t->profile.command_capacity);
    constants.frame = {std::uint32_t(in.rendered_frame_id), std::uint32_t(in.rendered_frame_id>>32u), t->words, segment_count};
    constants.options = {reset ? 1u : 0u, requested ? 1u : 0u, segment_capacity, t->profile.filter_samples};
    void *mapped{}; const D3D12_RANGE no_read{};
    const auto mapped_hr = f.upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(mapped_hr)) return HResultFailure("Map immutable virtual shadow frame snapshot", mapped_hr);
    auto *bytes = static_cast<std::byte *>(mapped);
    [[maybe_unused]] std::uint32_t changed_static_count{};
    for (std::size_t i = 0; i < in.casters.size(); ++i)
    {
        const auto &c = in.casters[i];
        const bool is_static = c.immutable_source && !c.skin && !c.wind && !c.animated_alpha;
        const bool was_static = c.previous_immutable_source && !c.previous_skin && !c.previous_wind && !c.previous_animated_alpha;
        const bool changed = c.present != c.previous_present || is_static != was_static ||
            c.instance_generation != c.previous_generation || c.current_revisions != c.previous_revisions ||
            c.current_cut_revision != c.previous_cut_revision || !Same(c.current_bounds, c.previous_bounds);
        const std::uint32_t flags = (c.present ? 1u : 0u) | (c.previous_present ? 2u : 0u) |
            (is_static ? 4u : 0u) | (was_static ? 8u : 0u) | (changed ? 16u : 0u);
        GpuCaster gpu{c.current_bounds, c.previous_bounds,
            {c.instance_index, c.instance_generation, c.source_ref, c.geometry_record},
            {c.draw_group, c.index_count, c.first_index, std::uint32_t(c.base_vertex)},
            {flags, c.meshlet_count, c.present ? std::uint32_t(in.draw_groups[c.draw_group].frontend) : 0u, constants.limits.x}};
        std::memcpy(bytes+t->caster_offset+i*sizeof(GpuCaster), &gpu, sizeof(gpu));
        if (changed && (is_static || was_static))
        {
            constants.limits.x = static_cast<std::uint32_t>(i);
            ++changed_static_count;
        }
    }
    // Link only cache-invalidating changes; preserve caster order and draw identities.
#ifndef NDEBUG
    std::uint32_t linked_count{};
    for (auto index = constants.limits.x; index != UINT32_MAX; )
    {
        assert(index < in.casters.size());
        GpuCaster caster;
        std::memcpy(&caster, bytes+t->caster_offset+index*sizeof(GpuCaster), sizeof(caster));
        assert((caster.properties.x & 16u) != 0 && (caster.properties.x & 12u) != 0);
        assert(caster.properties.w == UINT32_MAX || caster.properties.w < index);
        index = caster.properties.w;
        ++linked_count;
    }
    assert(linked_count == changed_static_count);
#endif
    std::memcpy(bytes, &constants, sizeof(constants));
    if (!in.receivers.empty()) std::memcpy(bytes+t->receiver_offset, in.receivers.data(), in.receivers.size_bytes());
    const D3D12_RANGE written{0, static_cast<SIZE_T>(t->upload_bytes)}; f.upload.resource->Unmap(0, &written);
    f.batches.clear();
    for (std::uint32_t layer = 0; layer < 2; ++layer)
        for (std::uint32_t group = 0; group < in.draw_groups.size(); ++group)
        {
            const auto segment = layer*group_count+group;
            const auto stride = in.draw_groups[group].frontend == VirtualShadowFrontend::Indexed ? kIndexedStride : kMeshStride;
            f.batches.push_back({in.draw_groups[group], group, layer, segment_capacity,
                UINT64(segment)*segment_capacity*stride, UINT64(segment)*4});
        }
    TextureSrv(t->device.Get(), in.depth_resource, DXGI_FORMAT_R32_FLOAT,
        Cpu(t->compute_heap.Get(), in.frame_slot*kComputeDescriptors, t->descriptor_stride));
    TextureSrv(t->device.Get(), in.normal_resource, in.normal_resource->GetDesc().Format,
        Cpu(t->compute_heap.Get(), in.frame_slot*kComputeDescriptors+1, t->descriptor_stride));
    impl_->tracked.clear();
    const auto import = [&](AllocationResource &resource, std::string_view name, bool texture = false)
    {
        const auto version = Import(graph, resource, name, texture);
        impl_->tracked.push_back({&resource, version.resource}); return version;
    };
    auto metadata = import(f.metadata, "VirtualShadow.Metadata");
    auto casters = import(f.casters, "VirtualShadow.CasterSnapshot");
    auto receivers = import(f.receivers, "VirtualShadow.ReceiverBounds");
    auto pages = import(f.pages, "VirtualShadow.FramePageTable");
    auto requests = import(f.requests, "VirtualShadow.RequestBitset");
    auto residents = import(t->residents, "VirtualShadow.PersistentResidents");
    auto draws = import(f.draws, "VirtualShadow.DrawRecords");
    auto indexed = import(f.indexed, "VirtualShadow.IndexedCommands");
    auto mesh = import(f.mesh, "VirtualShadow.MeshCommands");
    auto counts = import(f.counts, "VirtualShadow.CommandCounts");
    auto clear = import(f.clear, "VirtualShadow.ClearCommands");
    auto page_draw_counts = import(f.page_draw_counts, "VirtualShadow.PageDrawCounts");
    auto static_bank = import(t->static_bank, "VirtualShadow.StaticDepth", true);
    auto dynamic_bank = import(t->dynamic_bank, "VirtualShadow.DynamicDepth", true);
    {
        auto pass = graph.AddPass(147, "VirtualShadow.SnapshotUpload", QueueHint::Copy);
        metadata = pass.Write(metadata, Access::CopyDest);
        casters = pass.Write(casters, Access::CopyDest);
        receivers = pass.Write(receivers, Access::CopyDest);
        pass.SetExecute([t, slot=in.frame_slot, caster_bytes=in.casters.size()*sizeof(GpuCaster),
            receiver_bytes=in.receivers.size_bytes()](RenderPassContext &ctx)
        {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList()); auto &frame=t->frames[slot];
            list->CopyBufferRegion(frame.metadata.resource.Get(), 0, frame.upload.resource.Get(), 0, sizeof(VirtualShadowConstants));
            if (caster_bytes) list->CopyBufferRegion(frame.casters.resource.Get(), 0, frame.upload.resource.Get(), t->caster_offset, caster_bytes);
            if (receiver_bytes) list->CopyBufferRegion(frame.receivers.resource.Get(), 0, frame.upload.resource.Get(), t->receiver_offset, receiver_bytes);
        });
    }
    {
        auto pass = graph.AddPass(148, "VirtualShadow.InitializeFeedback", QueueHint::Compute);
        pages = pass.Write(pages, Access::UnorderedWrite); requests = pass.Write(requests, Access::UnorderedWrite);
        counts = pass.Write(counts, Access::UnorderedWrite);
        page_draw_counts = pass.Write(page_draw_counts, Access::UnorderedWrite);
        residents = reset ? pass.Write(residents, Access::UnorderedWrite) : pass.ReadWrite(residents, Access::UnorderedWrite);
        pass.SetExecute([t, slot=in.frame_slot](RenderPassContext &ctx)
        {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            BindCompute(list, *t, t->frames[slot], slot, 0);
            const auto work = std::max({t->entries, t->profile.physical_pages, t->words, t->profile.max_draw_groups*2+2});
            list->Dispatch((work+63)/64, 1, 1);
        });
    }
    {
        auto pass = graph.AddPass(149, "VirtualShadow.ReceiverFeedback", QueueHint::Compute);
        pass.Read(in.depth, Access::ComputeRead); pass.Read(in.normal, Access::ComputeRead); pass.Read(receivers, Access::ComputeRead);
        requests = pass.ReadWrite(requests, Access::UnorderedWrite);
        pass.SetExecute([t, slot=in.frame_slot, width=in.render_active.width, rows=std::uint32_t(feedback_rows)](RenderPassContext &ctx)
        {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            BindCompute(list, *t, t->frames[slot], slot, 1); list->Dispatch((width+7)/8, (rows+7)/8, 1);
        });
    }
    {
        auto pass = graph.AddPass(150, "VirtualShadow.ResidentAndDirty", QueueHint::Compute);
        pass.Read(casters, Access::ComputeRead); pass.Read(requests, Access::UnorderedRead);
        residents = pass.ReadWrite(residents, Access::UnorderedWrite); pages = pass.ReadWrite(pages, Access::UnorderedWrite);
        counts = pass.ReadWrite(counts, Access::UnorderedWrite); clear = pass.Write(clear, Access::UnorderedWrite);
        pass.SetExecute([t, slot=in.frame_slot, resident_handle=residents.resource, page_handle=pages.resource](RenderPassContext &ctx)
        {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            BindCompute(list, *t, t->frames[slot], slot, 2); list->Dispatch((t->profile.physical_pages+63)/64, 1, 1);
            ctx.UavBarrier(resident_handle); ctx.UavBarrier(page_handle);
            list->SetComputeRoot32BitConstant(3, 1, 0); list->Dispatch(1, 1, 1);
        });
    }
    {
        auto pass = graph.AddPass(224, "VirtualShadow.CountPageDraws", QueueHint::Compute);
        pass.Read(requests, Access::UnorderedRead);
        pass.Read(casters, Access::ComputeRead); pass.Read(pages, Access::UnorderedRead);
        page_draw_counts = pass.ReadWrite(page_draw_counts, Access::UnorderedWrite);
        pass.SetExecute([t, slot=in.frame_slot, caster_count=std::uint32_t(in.casters.size())](RenderPassContext &ctx)
        {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            BindCompute(list, *t, t->frames[slot], slot, 3);
            if (caster_count) list->Dispatch((caster_count+63)/64, t->words, 1);
        });
    }
    {
        auto pass = graph.AddPass(225, "VirtualShadow.AdmitCompletePages", QueueHint::Compute);
        pass.Read(requests, Access::UnorderedRead);
        pass.Read(page_draw_counts, Access::UnorderedRead);
        pages = pass.ReadWrite(pages, Access::UnorderedWrite);
        pass.SetExecute([t, slot=in.frame_slot, segment_count](RenderPassContext &ctx)
        {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            BindCompute(list, *t, t->frames[slot], slot, 3);
            list->SetComputeRoot32BitConstant(3, 1, 0);
            if (segment_count) list->Dispatch((segment_count+63)/64, 1, 1);
        });
    }
    {
        auto pass = graph.AddPass(151, "VirtualShadow.CullAndCommands", QueueHint::Compute);
        pass.Read(requests, Access::UnorderedRead);
        pass.Read(casters, Access::ComputeRead); pages = pass.ReadWrite(pages, Access::UnorderedWrite);
        counts = pass.ReadWrite(counts, Access::UnorderedWrite); draws = pass.Write(draws, Access::UnorderedWrite);
        indexed = pass.Write(indexed, Access::UnorderedWrite); mesh = pass.Write(mesh, Access::UnorderedWrite);
        pass.SetExecute([t, slot=in.frame_slot, caster_count=std::uint32_t(in.casters.size())](RenderPassContext &ctx)
        {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            BindCompute(list, *t, t->frames[slot], slot, 3);
            list->SetComputeRoot32BitConstant(3, 2, 0);
            if (caster_count) list->Dispatch((caster_count+63)/64, t->words, 1);
        });
    }
    out.metadata = metadata; out.page_table = pages; out.draw_records = draws;
    out.indexed_commands = indexed; out.mesh_commands = mesh; out.command_counts = counts;
    out.resources = {f.metadata.resource.Get(), f.pages.resource.Get(), t->static_bank.resource.Get(), t->dynamic_bank.resource.Get(), f.draws.resource.Get()};
    for (std::uint32_t i=0; i<5; ++i) out.srvs[i] = Cpu(t->srv_heap.Get(), in.frame_slot*5+i, t->descriptor_stride);
    out.depth_views = {Cpu(t->dsv_heap.Get(), 0, t->dsv_stride), Cpu(t->dsv_heap.Get(), 1, t->dsv_stride)};
    out.indexed_arguments = f.indexed.resource.Get(); out.mesh_arguments = f.mesh.resource.Get(); out.counts_resource = f.counts.resource.Get();
    out.bank_extent = t->extent;
    for (std::uint32_t layer=0; layer<2; ++layer)
    {
        auto pass = graph.AddPass(152+layer, layer ? "VirtualShadow.DynamicBank" : "VirtualShadow.StaticBank", QueueHint::Direct);
        pass.Read(metadata, Access::ShaderRead); pass.Read(pages, Access::ShaderRead); pass.Read(draws, Access::ShaderRead);
        pass.Read(indexed, Access::IndirectArgs); pass.Read(mesh, Access::IndirectArgs);
        pass.Read(counts, Access::IndirectArgs); pass.Read(clear, Access::IndirectArgs);
        for (const auto &read : in.native_reads) if (read.version.resource) pass.Read(read.version, read.access);
        if (in.draw && !in.draw_groups.empty() && in.declare_material_sampling)
            in.declare_material_sampling(pass);
        if (layer) dynamic_bank = pass.Write(dynamic_bank, Access::DepthWrite);
        else static_bank = pass.ReadWrite(static_bank, Access::DepthWrite);
        pass.SetExecuteChecked([t, slot=in.frame_slot, layer, callback=in.draw, outputs=out,
            group_count=std::uint32_t(in.draw_groups.size()), initialize=!t->banks_initialized](RenderPassContext &ctx) -> Result
        {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList()); auto &frame=t->frames[slot];
            const auto dsv = outputs.depth_views[layer];
            // Sparse shader clears do not initialize placed depth metadata.
            // Initialize the complete new bank before its first indirect draw.
            if (initialize) list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
            list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            const D3D12_VIEWPORT viewport{0,0,float(t->extent.width),float(t->extent.height),0,1};
            const D3D12_RECT scissor{0,0,LONG(t->extent.width),LONG(t->extent.height)};
            list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
            ID3D12DescriptorHeap *heaps[]{t->compute_heap.Get()}; list->SetDescriptorHeaps(1, heaps);
            list->SetGraphicsRootSignature(t->root.Get()); list->SetPipelineState(t->clear_pso.Get());
            list->SetGraphicsRootConstantBufferView(0, frame.upload.resource->GetGPUVirtualAddress());
            list->SetGraphicsRootDescriptorTable(1, Gpu(t->compute_heap.Get(), slot*kComputeDescriptors, t->descriptor_stride));
            list->SetGraphicsRootDescriptorTable(2, Gpu(t->compute_heap.Get(), slot*kComputeDescriptors+kComputeSrvs, t->descriptor_stride));
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->ExecuteIndirect(t->clear_signature.Get(), t->profile.physical_pages, frame.clear.resource.Get(),
                UINT64(layer)*t->profile.physical_pages*kClearStride, frame.counts.resource.Get(),
                UINT64(group_count*2+layer)*4);
            return callback(ctx, outputs, std::span<const VirtualShadowDrawBatch>{frame.batches}.subspan(layer*group_count, group_count));
        });
    }
    {
        auto pass = graph.AddPass(154, "VirtualShadow.Publish", QueueHint::Compute);
        graph.AddOrderingDependency(152, 154); graph.AddOrderingDependency(153, 154);
        pages = pass.ReadWrite(pages, Access::UnorderedWrite); residents = pass.ReadWrite(residents, Access::UnorderedWrite);
        pass.SetExecute([t, slot=in.frame_slot](RenderPassContext &ctx)
        {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            BindCompute(list, *t, t->frames[slot], slot, 4); list->Dispatch((t->entries+63)/64,1,1);
        });
    }
    {
        auto pass = graph.AddPass(155, "VirtualShadow.ShadowDataReady", QueueHint::Direct);
        for (const auto version : {metadata, pages, static_bank, dynamic_bank, draws}) pass.Read(version, Access::ShaderRead);
        pass.SetExecute([](RenderPassContext &) {});
    }
    out.page_table = pages; out.static_bank = static_bank; out.dynamic_bank = dynamic_bank;
    impl_->pending_graph = &graph; impl_->pending_slot = in.frame_slot; impl_->pending = constants;
    return Result::Success();
}
Result VirtualShadowSystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    if (impl_->pending_graph != &graph || !impl_->active) return Invalid("Virtual shadow submission does not match its registered graph.");
    for (auto &tracked : impl_->tracked)
    {
        tracked.resource->graph_access = graph.FinalAccess(tracked.handle);
        tracked.resource->graph_queue = graph.FinalQueue(tracked.handle);
        tracked.resource->graph_completion = graph.ResourceCompletion(tracked.handle);
        // On partial submission, retain every possible use through the executor's
        // completion set. Resource states are discarded during fault recovery.
        tracked.resource->graph_completion.Merge(completion);
    }
    impl_->active->lease.Merge(completion);
    impl_->active->frames[impl_->pending_slot].completion = completion;
    // Register always retains both bank passes, even with no requested pages.
    // Whole-graph success therefore includes both first-use API clears.
    if (success)
    {
        impl_->successful = impl_->pending;
        impl_->last_successful_slot = impl_->pending_slot;
        impl_->has_success = true;
        impl_->active->banks_initialized = true;
    }
    else impl_->faulted = true;
    impl_->pending_graph = nullptr; impl_->tracked.clear(); return Result::Success();
}
Result VirtualShadowSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending_graph) return Invalid("Virtual shadow reset requires submission accounting first.");
    impl_->Collect(completed);
    if (!impl_->active) { impl_->faulted=false; impl_->has_success=false; return Result::Success(); }
    if (!impl_->active->lease.IsSatisfiedBy(completed)) return Invalid("Virtual shadow reset requires all bank/page-table readers to complete.");
    if (impl_->faulted)
    {
        // A failed multi-queue submission can leave arbitrary intermediate
        // states. Recreate resources after their lease rather than guessing a
        // barrier state or treating the proposed depth epoch as successful.
        const auto old = impl_->active;
        VirtualShadowCandidate replacement;
        if (auto result=Prepare(old->device.Get(), old->allocator.Get(), old->profile, old->reader, replacement, old->cache); !result) return result;
        return Commit(std::move(replacement), completed);
    }
    impl_->has_success=false; impl_->successful={}; impl_->pending={}; return Result::Success();
}
VirtualShadowCacheStatus VirtualShadowSystem::CacheStatus() const noexcept
{
    const auto &t = impl_->active;
    if (!t) return {};
    return {t->profile.cache_budget_bytes,
        (t->static_bank.allocation ? t->static_bank.allocation->GetSize() : 0) +
        (t->dynamic_bank.allocation ? t->dynamic_bank.allocation->GetSize() : 0), t->profile.physical_pages};
}
std::uint64_t VirtualShadowSystem::AllocatedBytes() const noexcept
{
    std::uint64_t bytes=impl_->active ? impl_->active->allocated : 0;
    for (const auto &t : impl_->retired) bytes += t->allocated;
    return bytes;
}
std::uint64_t VirtualShadowSystem::HostStagingBytes() const noexcept
{
    std::uint64_t bytes=sizeof(Impl)+impl_->retired.capacity()*sizeof(std::shared_ptr<VirtualShadowTargets>)+
        impl_->tracked.capacity()*sizeof(Impl::Tracked);
    const auto add=[&](const auto &t)
    {
        if (t)
        {
            bytes += sizeof(VirtualShadowTargets)+t->frames.capacity()*sizeof(VirtualShadowTargets::FrameSlot);
            for (const auto &f : t->frames) bytes += f.batches.capacity()*sizeof(VirtualShadowDrawBatch);
        }
    };
    add(impl_->active); for (const auto &t : impl_->retired) add(t); return bytes;
}
std::array<const AllocationResource *, 5> VirtualShadowSystem::DiagnosticResources() const noexcept
{
    if (!impl_->active || !impl_->has_success || impl_->last_successful_slot >= impl_->active->frames.size()) return {};
    const auto &storage = *impl_->active;
    const auto &frame = storage.frames[impl_->last_successful_slot];
    return {&frame.metadata, &frame.pages, &storage.static_bank, &storage.dynamic_bank, &frame.counts};
}
} // namespace hs
