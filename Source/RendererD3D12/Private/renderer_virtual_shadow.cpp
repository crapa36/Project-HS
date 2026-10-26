#include "renderer_virtual_shadow.hpp"
#include "d3d12_resources.hpp"
#include "renderer_diagnostics.hpp"
#include "renderer_pipeline_cache.hpp"
#include <hs/renderer/renderer.hpp>
#include <hs/core/shipped_content_owner.hpp>
#include <hs/core/sha256.hpp>
#include <hs/render_data/static_shadow.hpp>
#include <hs/render_data/static_shadow_archive.hpp>
#include <fstream>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
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
constexpr std::uint32_t kSegmentMetadataOffset=512; // Preserve append-only root metadata before command segments.
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
ShadowGuaranteeFacts ShadowDemandFacts(const VirtualShadowInputs &in,const VirtualShadowConstants &c)
{
    ShadowGuaranteeFacts facts;
    const auto copy=[](DirectX::XMFLOAT4 v) { return Float4{v.x,v.y,v.z,v.w}; };
    facts.right=copy(c.light_right); facts.up=copy(c.light_up); facts.forward=copy(c.light_forward);
    facts.depth=copy(c.depth);
    facts.minimum_x=c.windows[2].x; facts.minimum_y=c.windows[2].y;
    facts.page_world_size=c.clipmaps[2].x; facts.world_texel=c.clipmaps[2].y;
    facts.draw_group_count=static_cast<std::uint32_t>(in.draw_groups.size());
    facts.compatible_bake=in.baked_static && c.baked_depth.z!=0 && c.options.y!=0;
    return facts;
}
}

VirtualShadowGuarantee CheckVirtualShadowGuarantee(const VirtualShadowInputs &in,
    const VirtualShadowConstants &c, std::uint32_t physical_pages, bool seed_available,
    std::span<ShadowCommandSegment> command_segments,std::uint32_t command_capacity,bool fixed_root_eligible,
    const ShadowCommandDemand &commands)
{
    auto facts=ShadowDemandFacts(in,c);
    for (unsigned i=0;i<3;++i) facts.window_sides[i] = static_cast<std::uint32_t>(c.windows[i].z);
    facts.physical_pages = physical_pages; facts.filter_samples = c.options.w;
    facts.segment_capacity = c.options.z; facts.command_capacity=command_capacity;
    facts.seed_available = seed_available;
    facts.required_receiver_domains=in.required_receiver_domains;facts.closed_receiver_domains=in.closed_receiver_domains;
    facts.frustum_receiver_index=in.frustum_receiver_index;
    const auto &matrix=in.receiver_view_projection;
    for(unsigned face=0;face<6;++face) for(unsigned row=0;row<4;++row)
    {
        const unsigned axis=face/2;
        facts.receiver_frustum[face][row]=axis==2 && face%2==0 ? matrix.m[row][2]
            : double(matrix.m[row][3])+(face%2==0?1.0:-1.0)*matrix.m[row][axis];
    }
    facts.camera_valid = true;
    const auto inverse = DirectX::XMLoadFloat4x4(&in.inverse_view_projection);
    for (const float z : {0.0f,1.0f}) for (const float y : {-1.0f,1.0f}) for (const float x : {-1.0f,1.0f})
    {
        const auto homogeneous = DirectX::XMVector4Transform(DirectX::XMVectorSet(x,y,z,1),inverse);
        const float w = DirectX::XMVectorGetW(homogeneous);
        if (!std::isfinite(w) || !(w > 0)) { facts.camera_valid = false; break; }
    }
    // The retained stable prefix plus current dynamic suffix supplies both
    // the partition and proof, including normal fallback page admission.
    const auto count=std::size_t(facts.draw_group_count)*2;
    if(command_segments.size()!=count)return {true,VirtualShadowCsmReason::CommandGroupLimit};
    if(count<=commands.coarse.size())
        static_cast<void>(PlanShadowCommandSegments(command_capacity,std::span(commands.coarse).first(count),
            std::span(commands.weights).first(count),command_segments));
    else static_cast<void>(PlanShadowCommandSegments(command_capacity,{},{},command_segments));
    facts.command_segments=command_segments;
    if(fixed_root_eligible)
    {
        VirtualShadowGuarantee result;
        result.csm_required=false; result.reason=VirtualShadowCsmReason::Guaranteed;
        result.maximum_coarse_commands=commands.maximum;
        result.command_capacity=command_capacity;
        for(const auto value:commands.coarse)result.coarse_command_total+=value;
        return result;
    }
    auto csm_matrices = in.main_csm_view_projection;
    const bool csm_matrices_valid = in.prepare_main_csm
        ? in.prepare_main_csm(csm_matrices) : in.main_csm_view_projection_valid;
    facts.csm_query_frusta_valid = csm_matrices_valid &&
        BuildShadowCsmQueryFrusta(csm_matrices, facts.csm_query_frusta);
    auto result=CheckVirtualShadowGuaranteeFacts(facts,in.casters,in.receivers,in.surface_receivers,&commands);
    result.command_capacity=command_capacity;
    result.coarse_command_total=0;
    for(const auto value:commands.coarse)result.coarse_command_total+=value;
    if(result.csm_required)
    {
        // A rejected no-CSM plan must not broaden the fallback frame's VSM
        // publication set. Preserve its exact prior equal floor partition,
        // including the unused remainder; retain the completed demand above.
        const auto per_segment=count?command_capacity/static_cast<std::uint32_t>(count):0u;
        for(std::size_t segment=0;segment<count;++segment)
            command_segments[segment]={static_cast<std::uint32_t>(segment)*per_segment,per_segment};
    }
    return result;
}

struct VirtualShadowBakeData
{
    render_data::StaticShadowArchiveDescription description;
    std::string generation,index_digest,pages_digest;
    std::filesystem::path index_path,pages_path;
    std::vector<std::byte> staging; // Coarse directory followed by decoded R32 tiles only.
    std::vector<float> root_static_pixels; // Core texels flattened in world light-space order.
    ViewExtent root_static_extent;
    float root_depth_minimum{}, root_depth_span{};
};

struct VirtualShadowTargets
{
    struct FrameSlot
    {
        AllocationResource upload, metadata, casters, receivers, pages, requests, draws;
        AllocationResource indexed, mesh, counts, clear, page_draw_counts;
        CompletionSet completion;
        std::uint64_t stable_revision{}, pending_stable_revision{};
        std::vector<VirtualShadowDrawBatch> batches;
        std::vector<VirtualShadowDrawBatch> root_batches;
        struct RootGroup { std::uint32_t count{},command_first{}; };
        std::vector<RootGroup> root_groups;
        std::array<std::size_t,2> batch_begins{},batch_counts{};
    };
    VirtualShadowProfile profile;
    ViewExtent extent;
    std::uint32_t columns{}, entries{}, words{}, descriptor_stride{}, dsv_stride{};
    std::uint64_t allocated{}, upload_bytes{}, caster_offset{512}, receiver_offset{},segment_offset{};
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    VirtualShadowSystem::ShaderReader reader;
    RendererPipelineCache *cache{};
    AllocationResource residents, static_bank, dynamic_bank, baked_staging, baked_upload;
    AllocationResource root_static, root_static_upload, root_dynamic;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT root_static_footprint{};
    CompletionSet baked_upload_completion, root_static_upload_completion;
    bool baked_initialized{}, root_static_initialized{};
    std::vector<FrameSlot> frames;
    ComPtr<ID3D12DescriptorHeap> compute_heap, srv_heap, dsv_heap;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 5> compute;
    ComPtr<ID3D12PipelineState> clear_pso, seed_pso;
    ComPtr<ID3D12CommandSignature> clear_signature;
    CompletionSet lease;
    bool banks_initialized{};
};

namespace
{
ShadowRootCoverageDomain RootCoverageDomain(const VirtualShadowProfile &profile,const VirtualShadowConstants &constants)
{
    const auto &archive=profile.baked->description;
    const auto &coarse=archive.levels[2];
    const double page=coarse.page_world_size, texel=page/render_data::kStaticShadowPageInner;
    ShadowRootCoverageDomain domain;
    domain.right={constants.light_right.x,constants.light_right.y,constants.light_right.z,constants.light_right.w};
    domain.up={constants.light_up.x,constants.light_up.y,constants.light_up.z,constants.light_up.w};
    domain.forward={constants.light_forward.x,constants.light_forward.y,constants.light_forward.z,constants.light_forward.w};
    domain.minimum_x=double(coarse.minimum_x)*page+render_data::kStaticShadowPageGuard*texel;
    domain.minimum_y=double(coarse.minimum_y)*page+render_data::kStaticShadowPageGuard*texel;
    domain.maximum_x=(double(coarse.minimum_x)+coarse.width)*page-render_data::kStaticShadowPageGuard*texel;
    domain.maximum_y=(double(coarse.minimum_y)+coarse.height)*page-render_data::kStaticShadowPageGuard*texel;
    const double z_min=profile.baked->root_depth_minimum;
    const double z_max=z_min+profile.baked->root_depth_span;
    domain.minimum_z=z_min;domain.maximum_z=z_max;
    return domain;
}
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
    std::array<D3D12_ROOT_PARAMETER, 6> parameters{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; parameters[0].Descriptor = {0, 21};
    for (std::uint32_t i = 0; i < 2; ++i)
    {
        parameters[i+1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i+1].DescriptorTable = {1, &ranges[i]};
    }
    parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[3].Constants = {1, 21, 1};
    parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[4].Descriptor={5,21};
    parameters[5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; parameters[5].Descriptor={6,21};
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
    if (t.profile.baked)
    {
        if (auto result=t.reader("render/shader/virtual_shadow_seed_ps.dxil",ps); !result) return result;
        graphics.PS={ps.data(),ps.size()};
        hr=t.cache ? t.cache->CreateGraphics(&graphics,IID_PPV_ARGS(t.seed_pso.ReleaseAndGetAddressOf()))
            : t.device->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(t.seed_pso.ReleaseAndGetAddressOf()));
        if(FAILED(hr)) return HResultFailure("Create virtual shadow static seed PSO",hr);
    }
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
    list->SetComputeRootShaderResourceView(4,t.baked_staging.resource ? t.baked_staging.resource->GetGPUVirtualAddress() : 0);
    list->SetComputeRootShaderResourceView(5,f.metadata.resource->GetGPUVirtualAddress()+kSegmentMetadataOffset);
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
    VirtualShadowGuarantee last_guarantee;
    bool cpu_plan_prepared{};
    bool has_success{}, faulted{};
    std::shared_ptr<const StaticRenderScene> qualified_scene;
    bool qualified_layout{};
    std::uint64_t stable_revision{}, stable_bounds_revision{};
    std::vector<GpuCaster> stable_words;
    std::uint64_t stable_demand_revision{};
    std::size_t stable_demand_size{};
    std::array<double,15> stable_demand_spatial{};
    std::uint32_t stable_demand_groups{};
    bool stable_demand_bake{},stable_demand_valid{};
    ShadowCommandDemand stable_demand;
    std::uint64_t stable_root_revision{};
    std::size_t stable_root_size{};
    std::array<double,18> stable_root_domain{};
    std::uint32_t stable_root_capacity{},stable_root_group_capacity{},stable_root_required_groups{};
    std::vector<std::uint32_t> stable_root_counts;
    std::uint32_t stable_root_count{};
    bool stable_root_valid{},stable_root_eligible{};
    std::uint32_t stable_head{UINT32_MAX}, stable_changes{};
    DirectX::XMFLOAT4 stable_forward{};
    double stable_minimum{}, stable_maximum{};
    ShadowCommandDemand PrepareCommandDemand(const VirtualShadowInputs &in,const ShadowGuaranteeFacts &facts)
    {
        const std::array<double,15> spatial{facts.right.x,facts.right.y,facts.right.z,
            facts.up.x,facts.up.y,facts.up.z,facts.forward.x,facts.forward.y,facts.forward.z,
            facts.depth.x,facts.depth.w,double(facts.minimum_x),double(facts.minimum_y),
            facts.page_world_size,facts.world_texel};
        if(!stable_demand_valid || !in.stable_caster_revision || stable_demand_revision!=in.stable_caster_revision ||
            stable_demand_size!=in.casters.stable.size() || stable_demand_groups!=facts.draw_group_count ||
            stable_demand_bake!=facts.compatible_bake ||
            (stable_demand.uses_spatial_facts && stable_demand_spatial!=spatial))
        {
            stable_demand=ComputeShadowCommandDemand(facts,in.casters.stable);
            stable_demand_revision=in.stable_caster_revision; stable_demand_size=in.casters.stable.size();
            stable_demand_groups=facts.draw_group_count; stable_demand_bake=facts.compatible_bake;
            stable_demand_spatial=spatial; stable_demand_valid=true;
        }
        // Seeded static geometry contributes fine-page weights even when it needs
        // no coarse commands. Dynamic/wind/alpha contributions retain exact order.
        return ComputeShadowCommandDemand(facts,in.casters.dynamic,stable_demand);
    }
    bool PrepareStableRoot(const VirtualShadowInputs &in,const ShadowRootCoverageDomain &domain,
                           std::uint32_t capacity,std::uint32_t group_capacity)
    {
        const std::array<double,18> key{domain.right.x,domain.right.y,domain.right.z,domain.right.w,
            domain.up.x,domain.up.y,domain.up.z,domain.up.w,
            domain.forward.x,domain.forward.y,domain.forward.z,domain.forward.w,
            domain.minimum_x,domain.minimum_y,domain.maximum_x,domain.maximum_y,domain.minimum_z,domain.maximum_z};
        if(!stable_root_valid || !in.stable_caster_revision || stable_root_revision!=in.stable_caster_revision ||
            stable_root_size!=in.casters.stable.size() || stable_root_domain!=key ||
            stable_root_capacity!=capacity || stable_root_group_capacity!=group_capacity)
        {
            stable_root_count=0; stable_root_counts.assign(group_capacity,0);
            stable_root_required_groups=0; stable_root_eligible=true;
            // The same immutable revision already owns stable_words and bounds.
            // Wind/animated-alpha entries stay fresh; only their conservative classification is retained.
            for(std::size_t index=0;index<in.casters.stable.size();++index)
            {
                const auto &caster=in.casters.stable[index];
                if(!caster.present || IsCompleteBakedStaticCaster(caster.present,caster.immutable_source,
                    caster.baked_source,caster.skin,caster.wind,caster.animated_alpha))continue;
                if(caster.draw_group>=group_capacity || stable_root_count>=capacity ||
                    !ShadowRootContainsCaster({caster.current_bounds.x,caster.current_bounds.y,
                        caster.current_bounds.z,caster.current_bounds.w},domain))
                { stable_root_eligible=false;break; }
                ++stable_root_count;
                ++stable_root_counts[caster.draw_group];
                stable_root_required_groups=std::max(stable_root_required_groups,caster.draw_group+1);
            }
            stable_root_revision=in.stable_caster_revision; stable_root_size=in.casters.stable.size();
            stable_root_domain=key; stable_root_capacity=capacity; stable_root_group_capacity=group_capacity;
            stable_root_valid=true;
        }
        return stable_root_eligible && stable_root_required_groups<=in.draw_groups.size();
    }
    void Collect(const CompletionSet &completed)
    {
        std::erase_if(retired, [&](const auto &t) { return t->lease.IsSatisfiedBy(completed); });
    }
};
VirtualShadowSystem::VirtualShadowSystem() : impl_(std::make_unique<Impl>()) {}
VirtualShadowSystem::~VirtualShadowSystem() = default;
std::uint64_t VirtualShadowCandidate::AllocatedBytes() const noexcept { return targets_ ? targets_->allocated : 0; }
Result VirtualShadowSystem::LoadBake(const RendererConfig &config,VirtualShadowProfile &profile,
    std::shared_ptr<const VirtualShadowBakeData> &shared_source)
{
    constexpr std::string_view index_id="render/lighting/arena_static_shadow.index";
    constexpr std::string_view pages_id="render/lighting/arena_static_shadow.pages";
    if(!profile.enabled || profile.virtual_sides!=std::array<std::uint32_t,3>{32,8,8} ||
        !config.product_paths.contains(std::string(index_id)) ||
        !config.product_paths.contains(std::string(pages_id))) return Result::Success();
    try
    {
        std::filesystem::path index_path,pages_path;
        if(auto r=config.ResolveProduct(index_id,index_path);!r) return r;
        if(auto r=config.ResolveProduct(pages_id,pages_path);!r) return r;
        const auto digest=config.product_digests.find(std::string(index_id));
        const auto pages_digest=config.product_digests.find(std::string(pages_id));
        if(digest==config.product_digests.end() || pages_digest==config.product_digests.end())
            return Invalid("Static shadow products lack selected Generation identities.");
        const auto requested_root_span=profile.fixed_root_depth_span;
        if(shared_source && shared_source->generation==config.content_generation_id &&
            shared_source->index_digest==digest->second && shared_source->pages_digest==pages_digest->second &&
            shared_source->index_path==index_path && shared_source->pages_path==pages_path &&
            shared_source->root_depth_span==requested_root_span)
        { profile.baked=shared_source; return Result::Success(); }
        ShippedContentOwner index_file,pages_file;
        if(auto r=ShippedContentOwner::Open(index_path,index_file);!r) return r;
        if(auto r=ShippedContentOwner::Open(pages_path,pages_file);!r) return r;
        if(index_file.Size()>64ull*1024*1024) return Invalid("Static shadow index exceeds bounded metadata size.");
        std::vector<std::byte> bytes(static_cast<std::size_t>(index_file.Size()));
        if(auto r=index_file.ReadExact(0,bytes);!r) return r;
        std::string index_hash="sha256:"; index_hash.reserve(71);
        constexpr char hex[]="0123456789abcdef";
        for(const auto byte:Sha256(bytes))
        { const auto value=std::to_integer<unsigned>(byte); index_hash+=hex[value>>4]; index_hash+=hex[value&15]; }
        if(index_hash!=digest->second)
            return Invalid("Static shadow index does not match the selected Generation.");
        const auto index=render_data::ReadStaticShadowArchiveIndex(bytes);
        if(index.payload_bytes!=pages_file.Size()) return Invalid("Static shadow archive payload size differs from index.");
        auto data=std::make_shared<VirtualShadowBakeData>(); data->description=index.description;
        data->generation=config.content_generation_id; data->index_digest=digest->second; data->pages_digest=pages_digest->second;
        data->index_path=index_path; data->pages_path=pages_path;
        const auto &level=index.description.levels[2];
        const std::uint64_t count=std::uint64_t(level.width)*level.height;
        if(!count || count>UINT32_MAX/4) return Invalid("Static shadow coarse directory is empty or exceeds native addressing.");
        const std::uint64_t root_width=std::uint64_t(level.width)*render_data::kStaticShadowPageInner;
        const std::uint64_t root_height=std::uint64_t(level.height)*render_data::kStaticShadowPageInner;
        if(root_width>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || root_height>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            root_width*root_height>std::numeric_limits<std::size_t>::max()/sizeof(float) ||
            !std::isfinite(requested_root_span) || requested_root_span<=0)
            return Invalid("Static shadow root dimensions or fixed depth interval are invalid.");
        const double archive_span=double(index.description.depth_maximum)-index.description.depth_minimum;
        if(requested_root_span<archive_span)
            return Invalid("Configured fixed shadow root depth interval cannot contain the baked static archive.");
        data->root_static_extent={static_cast<std::uint32_t>(root_width),static_cast<std::uint32_t>(root_height)};
        data->root_static_pixels.assign(static_cast<std::size_t>(root_width*root_height),0.0f);
        data->root_depth_span=requested_root_span;
        data->root_depth_minimum=float((double(index.description.depth_minimum)+index.description.depth_maximum-requested_root_span)*0.5);
        data->staging.resize(static_cast<std::size_t>(count)*4);
        std::size_t page_base{};
        for(unsigned i=0;i<2;++i) page_base+=std::size_t(index.description.levels[i].width)*index.description.levels[i].height;
        std::vector<float> tile(render_data::kStaticShadowPageTexels);
        for(std::size_t page=0;page<count;++page)
        {
            const auto &entry=index.pages[page_base+page];
            if(entry.bytes==UINT32_MAX) return Invalid("Static shadow coarse coverage contains an unbaked page.");
            std::uint32_t offset{};
            if(entry.bytes && entry.bytes!=UINT32_MAX)
            {
                bytes.resize(entry.bytes);
                if(auto r=pages_file.ReadExact(entry.offset,bytes);!r) return r;
                if(Sha256(bytes)!=entry.digest) return Invalid("Static shadow encoded page digest mismatch.");
                render_data::DecodeStaticShadowPage(bytes,tile);
                if(data->staging.size()>UINT32_MAX-tile.size()*sizeof(float)) return Invalid("Coarse shadow staging exceeds native address range.");
                offset=static_cast<std::uint32_t>(data->staging.size());
                const auto begin=data->staging.size(); data->staging.resize(begin+tile.size()*sizeof(float));
                std::memcpy(data->staging.data()+begin,tile.data(),tile.size()*sizeof(float));
                const double archive_min=index.description.depth_minimum;
                const double archive_span=double(index.description.depth_maximum)-archive_min;
                const double root_min=data->root_depth_minimum;
                const double root_span=data->root_depth_span;
                const std::size_t page_x=page%level.width, page_y=page/level.width;
                const std::size_t root_x=page_x*render_data::kStaticShadowPageInner;
                const std::size_t root_y=(std::size_t(level.height)-1-page_y)*render_data::kStaticShadowPageInner;
                for(std::size_t row=0;row<render_data::kStaticShadowPageInner;++row)
                {
                    const auto source=tile.begin()+std::ptrdiff_t((row+render_data::kStaticShadowPageGuard)*render_data::kStaticShadowPageTile+
                        render_data::kStaticShadowPageGuard);
                    auto destination=data->root_static_pixels.begin()+std::ptrdiff_t((root_y+row)*root_width+root_x);
                    for(std::size_t column=0;column<render_data::kStaticShadowPageInner;++column)
                    {
                        const float depth=source[column];
                        if(depth==0.0f) { destination[column]=0.0f; continue; }
                        const double world_z=archive_min+(1.0-double(depth))*archive_span;
                        const double root_depth=1.0-(world_z-root_min)/root_span;
                        if(!std::isfinite(root_depth)||root_depth<=0.0||root_depth>1.0)
                            return Invalid("Baked static shadow lies outside the fixed root depth interval.");
                        destination[column]=static_cast<float>(root_depth);
                    }
                }
            }
            std::memcpy(data->staging.data()+page*4,&offset,4);
        }
        shared_source=std::move(data); profile.baked=shared_source; return Result::Success();
    }
    catch(const std::exception &error) { return Invalid(error.what()); }
}
bool VirtualShadowSystem::MatchesBake(const std::shared_ptr<const StaticRenderScene> &scene,
    const LightView &light,const std::array<float,3> &extents)
{
    if(!impl_->active || !impl_->active->profile.baked || !scene) return false;
    const auto &data=*impl_->active->profile.baked;
    const auto &description=data.description;
    for(unsigned level=0;level<3;++level)
        if(extents[level]*2.0f/impl_->active->profile.virtual_sides[level]!=description.levels[level].page_world_size) return false;
    const float length=std::hypot(light.direction.x,light.direction.y,light.direction.z);
    if(!(length>0) || !std::isfinite(length)) return false;
    const auto f=description.light_forward;
    if(std::abs(light.direction.x/length-f.x)>1.e-6f || std::abs(light.direction.y/length-f.y)>1.e-6f ||
        std::abs(light.direction.z/length-f.z)>1.e-6f) return false;
    const auto forward=DirectX::XMVector3Normalize(DirectX::XMVectorSet(light.direction.x,light.direction.y,light.direction.z,0));
    const auto up_seed=std::abs(DirectX::XMVectorGetY(forward))>.95f ? DirectX::XMVectorSet(0,0,1,0) : DirectX::XMVectorSet(0,1,0,0);
    const auto right=DirectX::XMVector3Normalize(DirectX::XMVector3Cross(up_seed,forward));
    DirectX::XMFLOAT3 actual_right,actual_up;
    DirectX::XMStoreFloat3(&actual_right,right); DirectX::XMStoreFloat3(&actual_up,DirectX::XMVector3Cross(forward,right));
    const auto aligned=[](DirectX::XMFLOAT3 a,Float3 b) {
        return std::abs(a.x-b.x)<=1.e-6f && std::abs(a.y-b.y)<=1.e-6f && std::abs(a.z-b.z)<=1.e-6f;
    };
    if(!aligned(actual_right,description.light_right) || !aligned(actual_up,description.light_up)) return false;
    if(impl_->qualified_scene!=scene)
    {
        impl_->qualified_scene=scene;
        impl_->qualified_layout=render_data::StaticShadowLayoutDigest(*scene)==description.layout_digest;
        if(impl_->qualified_layout) for(const auto &source:scene->instances)
        {
            if(!render_data::IsStaticShadowCaster(source)) continue;
            auto canonical=source; canonical.asset_id={}; canonical.material_id={}; PopulateRenderInstanceIdentity(canonical);
            if((source.asset_id.value && source.asset_id.value!=canonical.asset_id.value) ||
                (source.material_id.value && source.material_id.value!=canonical.material_id.value))
            { impl_->qualified_layout=false; break; }
        }
    }
    return impl_->qualified_layout;
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
    t->segment_offset=t->receiver_offset+UINT64(profile.max_receivers)*sizeof(VirtualShadowReceiver);
    const bool has_fixed_root=profile.baked && !profile.baked->root_static_pixels.empty();
    const auto segment_bytes=UINT64(profile.max_draw_groups)*(has_fixed_root?3u:2u)*sizeof(ShadowCommandSegment);
    t->upload_bytes=t->segment_offset+segment_bytes;
    // Root draw-record slots use caster IDs, including absent/baked holes.
    // Check the combined allocation, not only the number of emitted commands.
    if(has_fixed_root && (UINT64(profile.command_capacity)+profile.max_casters)*sizeof(GpuShadowDrawRecord)>UINT32_MAX)
        return Invalid("Fixed-root command and draw-record addressing exceeds the shader ABI.");
    if(t->upload_bytes>UINT32_MAX) return Invalid("Virtual shadow upload exceeds the shader address range.");
    t->descriptor_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    t->dsv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    t->frames.resize(profile.frame_slots);
    if(profile.baked)
    {
        if(auto result=Allocate(*t,t->baked_staging,Buffer(profile.baked->staging.size()));!result) return result;
        if(auto result=Allocate(*t,t->baked_upload,Buffer(profile.baked->staging.size()),true);!result) return result;
        void *mapped{}; D3D12_RANGE no_read{};
        const auto hr=t->baked_upload.resource->Map(0,&no_read,&mapped);
        if(FAILED(hr)) return HResultFailure("Map coarse shadow staging",hr);
        std::memcpy(mapped,profile.baked->staging.data(),profile.baked->staging.size());
        t->baked_upload.resource->Unmap(0,nullptr);
    }
    if(profile.baked && !profile.baked->root_static_pixels.empty())
    {
        if(auto result=Allocate(*t,t->root_static,BankDescription(profile.baked->root_static_extent));!result)return result;
        if(auto result=Allocate(*t,t->root_dynamic,BankDescription(profile.baked->root_static_extent));!result)return result;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows{}; UINT64 row_size{},upload_bytes{};
        const auto description=t->root_static.resource->GetDesc();
        device->GetCopyableFootprints(&description,0,1,0,&footprint,&rows,&row_size,&upload_bytes);
        if(rows!=profile.baked->root_static_extent.height || row_size!=UINT64(profile.baked->root_static_extent.width)*sizeof(float) ||
            !upload_bytes || upload_bytes>UINT32_MAX)
            return Invalid("Flattened static shadow root upload footprint is invalid.");
        t->root_static_footprint=footprint;
        if(auto result=Allocate(*t,t->root_static_upload,Buffer(upload_bytes),true);!result)return result;
        void *mapped{}; D3D12_RANGE no_read{};
        const auto hr=t->root_static_upload.resource->Map(0,&no_read,&mapped);
        if(FAILED(hr))return HResultFailure("Map flattened static shadow root upload",hr);
        auto *destination=static_cast<std::byte *>(mapped)+footprint.Offset;
        const auto row_bytes=static_cast<std::size_t>(row_size);
        for(UINT row=0;row<rows;++row)
            std::memcpy(destination+std::size_t(row)*footprint.Footprint.RowPitch,
                profile.baked->root_static_pixels.data()+std::size_t(row)*profile.baked->root_static_extent.width,row_bytes);
        const D3D12_RANGE written{static_cast<SIZE_T>(footprint.Offset),static_cast<SIZE_T>(upload_bytes)};
        t->root_static_upload.resource->Unmap(0,&written);
    }
    // Mandatory root storage above is allocated before the fine cache.
    if (auto result = Allocate(*t, t->residents, Buffer(UINT64(profile.physical_pages)*32, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)); !result) return result;
    if (auto result = Allocate(*t, t->static_bank, BankDescription(t->extent)); !result) return result;
    if (auto result = Allocate(*t, t->dynamic_bank, BankDescription(t->extent)); !result) return result;
    t->static_bank.resource->SetName(L"VirtualShadow.StaticDepth");
    t->dynamic_bank.resource->SetName(L"VirtualShadow.DynamicDepth");
    if(t->root_static.resource)t->root_static.resource->SetName(L"VirtualShadow.FixedRootStaticDepth");
    if(t->root_dynamic.resource)t->root_dynamic.resource->SetName(L"VirtualShadow.FixedRootDynamicDepth");
    if (t->static_bank.allocation->GetSize() > profile.cache_budget_bytes / 2 ||
        t->dynamic_bank.allocation->GetSize() > profile.cache_budget_bytes - t->static_bank.allocation->GetSize())
        return Invalid("Actual native shadow depth banks exceed the authored cache capacity.");
    for (auto &f : t->frames)
    {
        if (auto result = Allocate(*t, f.upload, Buffer(t->upload_bytes), true); !result) return result;
        if (auto result = Allocate(*t, f.metadata, Buffer(kSegmentMetadataOffset+segment_bytes)); !result) return result;
        if (auto result = Allocate(*t, f.casters, Buffer(UINT64(profile.max_casters)*sizeof(GpuCaster))); !result) return result;
        if (auto result = Allocate(*t, f.receivers, Buffer(UINT64(profile.max_receivers)*sizeof(VirtualShadowReceiver))); !result) return result;
        const auto root_casters=has_fixed_root ? UINT64(profile.max_casters):0u;
        const auto root_pages=has_fixed_root ? 1u:0u;
        const auto root_groups=has_fixed_root ? UINT64(profile.max_draw_groups):0u;
        const std::array<std::pair<AllocationResource *, std::uint64_t>, 8> buffers{{
            {&f.pages, UINT64(t->entries+root_pages)*32}, {&f.requests, UINT64(t->words)*4},
            {&f.draws, (UINT64(profile.command_capacity)+root_casters)*32},
            {&f.indexed, (UINT64(profile.command_capacity)+root_casters)*kIndexedStride},
            {&f.mesh, (UINT64(profile.command_capacity)+root_casters)*kMeshStride},
            {&f.counts, (UINT64(profile.max_draw_groups)*2+2+root_groups)*4},
            {&f.clear, UINT64(profile.physical_pages)*2*kClearStride},
            {&f.page_draw_counts, UINT64(t->entries)*profile.max_draw_groups*2*sizeof(std::uint32_t)}}};
        for (auto [resource, bytes] : buffers)
            if (auto result = Allocate(*t, *resource, Buffer(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)); !result) return result;
        f.batches.reserve(profile.max_draw_groups*2);
        f.root_batches.reserve(profile.max_draw_groups);
    }
    if (auto result = Heap(*t, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, profile.frame_slots*kComputeDescriptors, true, t->compute_heap); !result) return result;
    if (auto result = Heap(*t, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            profile.frame_slots*kVirtualShadowDescriptorCount, false, t->srv_heap); !result) return result;
    if (auto result = Heap(*t, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, t->root_dynamic.resource ? 3u : 2u, false, t->dsv_heap); !result) return result;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{}; dsv.Format = DXGI_FORMAT_D32_FLOAT; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(t->static_bank.resource.Get(), &dsv, Cpu(t->dsv_heap.Get(), 0, t->dsv_stride));
    device->CreateDepthStencilView(t->dynamic_bank.resource.Get(), &dsv, Cpu(t->dsv_heap.Get(), 1, t->dsv_stride));
    if(t->root_dynamic.resource)
        device->CreateDepthStencilView(t->root_dynamic.resource.Get(),&dsv,Cpu(t->dsv_heap.Get(),2,t->dsv_stride));
    for (std::uint32_t slot = 0; slot < profile.frame_slots; ++slot)
    {
        auto &f = t->frames[slot]; const auto compute_base = slot*kComputeDescriptors, main_base = slot*kVirtualShadowDescriptorCount;
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
        TextureSrv(device,t->root_static.resource.Get(),DXGI_FORMAT_R32_FLOAT,Cpu(t->srv_heap.Get(),main_base+5,t->descriptor_stride));
        TextureSrv(device,t->root_dynamic.resource.Get(),DXGI_FORMAT_R32_FLOAT,Cpu(t->srv_heap.Get(),main_base+6,t->descriptor_stride));
    }
    if (auto result = Pipelines(*t); !result) return result;
    candidate.targets_ = std::move(t); return Result::Success();
}
Result VirtualShadowSystem::Commit(VirtualShadowCandidate &&candidate, const CompletionSet &completed)
{
    if (impl_->pending_graph) return Invalid("Cannot replace virtual shadows while a graph is awaiting submission.");
    impl_->Collect(completed);
    if (impl_->active && !impl_->active->lease.IsSatisfiedBy(completed)) impl_->retired.push_back(impl_->active);
    impl_->active = std::move(candidate.targets_);
    impl_->stable_root_valid=false; impl_->stable_demand_valid=false;
    BeginFramePolicy();
    impl_->qualified_scene.reset(); impl_->qualified_layout=false; impl_->has_success = false; impl_->faulted = false;
    impl_->successful = {}; impl_->pending = {}; impl_->last_guarantee = {}; return Result::Success();
}

void VirtualShadowSystem::BeginFramePolicy() noexcept
{
    impl_->last_guarantee = {};
    impl_->cpu_plan_prepared = false;
}
Result VirtualShadowSystem::PrepareFrame(const VirtualShadowInputs &in, VirtualShadowFramePlan &plan)
{
    plan = {};
    BeginFramePolicy();
    impl_->Collect(in.completed);
    if (!impl_->active) return Result::Success();
    if (impl_->pending_graph || impl_->faulted)
        return Invalid("Virtual shadows require completion/reset of the preceding graph before registration.");
    const auto t = impl_->active;
    if(t->baked_initialized && t->baked_upload.resource && t->baked_upload_completion.IsSatisfiedBy(in.completed))
    { t->allocated-=t->baked_upload.allocation->GetSize(); t->baked_upload.Reset(); }
    if(t->root_static_initialized && t->root_static_upload.resource && t->root_static_upload_completion.IsSatisfiedBy(in.completed))
    { t->allocated-=t->root_static_upload.allocation->GetSize(); t->root_static_upload.Reset(); }
    if (in.receivers.size() > t->profile.max_receivers)
        return Invalid(std::format("Virtual shadow receiver capacity exceeded: {} > {}.",
            in.receivers.size(), t->profile.max_receivers));
    if (in.frame_slot >= t->frames.size() || !in.render_active.width || !in.render_active.height ||
        in.render_active.width > t->profile.render_capacity.width || in.render_active.height > t->profile.render_capacity.height ||
        in.casters.size() > t->profile.max_casters ||
        in.draw_groups.size() > t->profile.max_draw_groups || !std::isfinite(in.depth_span) || in.depth_span <= 0)
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
    const auto qualify = [&](std::span<const VirtualShadowCaster> casters,double &minimum,double &maximum) -> Result {
    for (const auto &caster : casters)
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
    return Result::Success();
    };
    if(!in.stable_caster_revision || impl_->stable_bounds_revision!=in.stable_caster_revision ||
        !Same(impl_->stable_forward,constants.light_forward))
    {
        double low=std::numeric_limits<double>::infinity(),high=-low;
        if(auto result=qualify(in.casters.stable,low,high);!result) return result;
        impl_->stable_minimum=low; impl_->stable_maximum=high;
        impl_->stable_forward=constants.light_forward;
        impl_->stable_bounds_revision=in.stable_caster_revision;
    }
    minimum=std::min(minimum,impl_->stable_minimum); maximum=std::max(maximum,impl_->stable_maximum);
    if(auto result=qualify(in.casters.dynamic,minimum,maximum);!result) return result;
    bool root_receivers_valid=true;
    for (const auto &receiver : in.receivers)
    {
        if (!Finite(receiver.minimum) || !Finite(receiver.maximum) ||
            (receiver.minimum.w != 0.0f && receiver.minimum.w != 1.0f) || receiver.minimum.x > receiver.maximum.x ||
            receiver.minimum.y > receiver.maximum.y || receiver.minimum.z > receiver.maximum.z)
            return Invalid("Virtual shadow receiver has invalid world bounds.");
        // XYZ/finite/minimum.w are already validated above. maximum.w is a
        // root-only qualification; failure must preserve the ordinary fallback.
        root_receivers_valid &= receiver.maximum.w==0.0f || receiver.maximum.w==1.0f;
        for (std::uint32_t corner = 0; corner < 8; ++corner)
        {
            const double z = Dot({corner&1u ? receiver.maximum.x : receiver.minimum.x,
                corner&2u ? receiver.maximum.y : receiver.minimum.y,
                corner&4u ? receiver.maximum.z : receiver.minimum.z}, constants.light_forward);
            minimum = std::min(minimum, z); maximum = std::max(maximum, z);
        }
    }
    const auto surface_depth=[&](std::span<const ShadowSurfaceReceiver> bounds) -> Result {
        for(const auto &r:bounds)
        {
            if(!std::isfinite(r.minimum.x)||!std::isfinite(r.minimum.y)||!std::isfinite(r.minimum.z)||
               !std::isfinite(r.maximum.x)||!std::isfinite(r.maximum.y)||!std::isfinite(r.maximum.z)||
               r.minimum.x>r.maximum.x||r.minimum.y>r.maximum.y||r.minimum.z>r.maximum.z)
                return Invalid("Camera shadow surface has invalid conservative bounds.");
            root_receivers_valid &= r.minimum.w==0.0f && r.maximum.w==1.0f;
            for(unsigned corner=0;corner<8;++corner)
            {
                const double z=Dot({corner&1?r.maximum.x:r.minimum.x,corner&2?r.maximum.y:r.minimum.y,
                    corner&4?r.maximum.z:r.minimum.z},constants.light_forward);
                minimum=std::min(minimum,z);maximum=std::max(maximum,z);
            }
        }
        return Result::Success();
    };
    if(auto r=surface_depth(in.surface_receivers.stable_aggregate.empty()?in.surface_receivers.stable:in.surface_receivers.stable_aggregate);!r)return r;
    if(auto r=surface_depth(in.surface_receivers.dynamic);!r)return r;
    const bool baked=in.baked_static && bool(t->profile.baked);
    reset |= impl_->has_success && (impl_->successful.baked_depth.z!=0)!=baked;
    if(baked)
    {
        const auto &archive=t->profile.baked->description;
        minimum=std::min(minimum,double(archive.depth_minimum)); maximum=std::max(maximum,double(archive.depth_maximum));
        constants.baked_depth={archive.depth_minimum,archive.depth_maximum,1,1};
        const auto &level=archive.levels[2];
        constants.baked_directory={level.minimum_x,level.minimum_y,static_cast<int>(level.width),static_cast<int>(level.height)};
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
    plan.command_segments.resize(segment_count);
    constants.frame = {std::uint32_t(in.rendered_frame_id), std::uint32_t(in.rendered_frame_id>>32u), t->words, segment_count};
    constants.options = {reset ? 1u : 0u, requested ? 1u : 0u, t->profile.command_capacity, t->profile.filter_samples};
    if(t->profile.baked && t->root_static.resource)
    {
        const auto &archive=t->profile.baked->description;
        const auto &coarse=archive.levels[2];
        const float page_size=coarse.page_world_size;
        const float texel=page_size/float(render_data::kStaticShadowPageInner);
        constants.root_bounds={float(coarse.minimum_x*page_size),float(coarse.minimum_y*page_size),
            float((std::int64_t(coarse.minimum_x)+coarse.width)*page_size),
            float((std::int64_t(coarse.minimum_y)+coarse.height)*page_size)};
        constants.root_extent={float(t->profile.baked->root_static_extent.width),
            float(t->profile.baked->root_static_extent.height),texel,float(render_data::kStaticShadowPageGuard)};
        constants.root_depth={t->profile.baked->root_depth_minimum,
            1.0f/t->profile.baked->root_depth_span,t->profile.baked->root_depth_span,0};
        const bool root_ready=t->root_static_initialized &&
            t->root_static_upload_completion.IsSatisfiedBy(in.completed);
        constants.root_options={1u,root_ready?1u:0u,in.baked_static?1u:0u,0u};
    }
    const auto root_coverage=[&]() {
        constexpr auto known_domains=ShadowReceiverCamera|ShadowReceiverWorld|ShadowReceiverSoftwareGi;
        if(!t->root_static.resource || !t->root_dynamic.resource || !in.baked_static ||
            constants.root_options.y==0u || (in.required_receiver_domains&ShadowReceiverSoftwareGi) ||
            !(in.required_receiver_domains&ShadowReceiverCamera) ||
            (in.required_receiver_domains&~known_domains) ||
            (in.required_receiver_domains&~in.closed_receiver_domains) ||
            in.draw_groups.size()>t->profile.max_draw_groups ||
            t->profile.max_casters>kVirtualShadowPassBit-t->profile.command_capacity ||
            t->profile.fixed_root_depth_span<in.depth_span)
            return false;
        const auto domain=RootCoverageDomain(t->profile,constants);
        if(!impl_->PrepareStableRoot(in,domain,t->profile.max_casters,t->profile.max_draw_groups))return false;
        auto root_caster_count=impl_->stable_root_count;
        const auto caster_range=[&](std::span<const VirtualShadowCaster> casters) {
            for(const auto &caster:casters)
            {
                if(!caster.present || IsCompleteBakedStaticCaster(caster.present,caster.immutable_source,
                    caster.baked_source,caster.skin,caster.wind,caster.animated_alpha))continue;
                if(caster.draw_group>=in.draw_groups.size() || root_caster_count>=t->profile.max_casters)return false;
                ++root_caster_count;
                if(!ShadowRootContainsCaster({caster.current_bounds.x,caster.current_bounds.y,
                    caster.current_bounds.z,caster.current_bounds.w},domain))return false;
            }
            return true;
        };
        if(!caster_range(in.casters.dynamic))return false;
        return root_receivers_valid;
    };
    plan.fixed_root=root_coverage();
    const auto commands=impl_->PrepareCommandDemand(in,ShadowDemandFacts(in,constants));
    plan.guarantee = CheckVirtualShadowGuarantee(in, constants, t->profile.physical_pages,
        t->profile.baked && t->baked_staging.resource && t->seed_pso.Get() &&
        (t->baked_initialized || t->baked_upload.resource),plan.command_segments,t->profile.command_capacity,plan.fixed_root,commands);
    constants.root_options.w=plan.fixed_root?t->profile.max_draw_groups*2+2:0u;
    constants.baked_depth.w = plan.guarantee.csm_required ? 1.0f : 0.0f;
    plan.constants = constants;
    plan.targets = t;
    plan.casters = in.casters;
    plan.receivers = in.receivers;
    plan.groups = in.draw_groups;
    plan.light = in.light;
    plan.view_id = in.view_id;
    plan.rendered_frame_id = in.rendered_frame_id;
    plan.frame_slot = in.frame_slot;
    plan.stable_caster_revision = in.stable_caster_revision;
    plan.baked_static = in.baked_static;
    plan.required_receiver_domains=in.required_receiver_domains;plan.closed_receiver_domains=in.closed_receiver_domains;
    plan.receiver_view_projection=in.receiver_view_projection;plan.surface_receivers=in.surface_receivers;
    plan.clipmap_center = in.clipmap_center;
    plan.coverage_extents = in.coverage_extents;
    plan.depth_span = in.depth_span;
    plan.prepared = true;
    impl_->last_guarantee = plan.guarantee;
    impl_->cpu_plan_prepared = true;
    return Result::Success();
}
Result VirtualShadowSystem::Register(RenderGraphBuilder &graph, const VirtualShadowInputs &in,
    const VirtualShadowFramePlan &plan, VirtualShadowOutputs &out)
{
    out = {};
    if (!impl_->active && !plan.targets) { out.descriptor_count = 0; return Result::Success(); }
    const auto same_span = [](auto left, auto right) {
        return left.data() == right.data() && left.size() == right.size();
    };
    if (!plan.prepared || plan.targets != impl_->active || impl_->pending_graph || impl_->faulted ||
        plan.view_id != in.view_id || plan.frame_slot != in.frame_slot ||
        plan.rendered_frame_id != in.rendered_frame_id || plan.stable_caster_revision != in.stable_caster_revision ||
        plan.baked_static != in.baked_static || plan.required_receiver_domains!=in.required_receiver_domains ||
        plan.closed_receiver_domains!=in.closed_receiver_domains ||
        std::memcmp(&plan.receiver_view_projection,&in.receiver_view_projection,sizeof(in.receiver_view_projection))!=0 ||
        !same_span(plan.surface_receivers.stable,in.surface_receivers.stable)||
        !same_span(plan.surface_receivers.dynamic,in.surface_receivers.dynamic)||
        !same_span(plan.surface_receivers.stable_aggregate,in.surface_receivers.stable_aggregate)||
        plan.light.stable_id != in.light.stable_id || plan.light.generation != in.light.generation ||
        plan.light.type != in.light.type || plan.light.shadow_flags != in.light.shadow_flags ||
        std::memcmp(&plan.light.direction, &in.light.direction, sizeof(in.light.direction)) != 0 ||
        std::memcmp(&plan.clipmap_center, &in.clipmap_center, sizeof(in.clipmap_center)) != 0 ||
        plan.coverage_extents != in.coverage_extents || plan.depth_span != in.depth_span ||
        std::memcmp(&plan.constants.inverse_view_projection, &in.inverse_view_projection, sizeof(in.inverse_view_projection)) != 0 ||
        std::memcmp(&plan.constants.view_origin, &in.view_origin, sizeof(in.view_origin)) != 0 ||
        plan.constants.limits.z != in.render_active.width || plan.constants.limits.w != in.render_active.height ||
        !same_span(plan.casters.stable, in.casters.stable) || !same_span(plan.casters.dynamic, in.casters.dynamic) ||
        !same_span(plan.receivers, in.receivers) || !same_span(plan.groups, in.draw_groups))
        return Invalid("Virtual shadow registration does not match its prepared view/frame/slot snapshot.");
    if (!in.depth.resource || !in.normal.resource || !in.depth_resource || !in.normal_resource || !in.draw)
        return Invalid("Virtual shadow registration lacks current graph/native bindings.");
    const auto t = plan.targets;
    auto &f = t->frames[in.frame_slot];
    if (!f.completion.IsSatisfiedBy(in.completed)) return Invalid("Virtual shadow frame slot still has GPU readers.");
    auto constants = plan.constants;
    const auto group_count = static_cast<std::uint32_t>(in.draw_groups.size());
    if(plan.fixed_root && !impl_->PrepareStableRoot(in,RootCoverageDomain(t->profile,constants),
            t->profile.max_casters,t->profile.max_draw_groups))
        return Invalid("Fixed-root stable casters no longer satisfy the prepared root domain.");
    const auto segment_count = group_count*2;
    // PrepareFrame owns this private partition. Its bounded planner (or equal
    // fallback partition) already establishes the layout; the snapshot checks
    // above keep the same target capacity and group span through registration.
    const bool reset = constants.options.x != 0;
    const auto rounded_width = UINT64((in.render_active.width+7u)&~7u);
    const auto feedback_rows = std::max<UINT64>(in.render_active.height, (in.receivers.size()+rounded_width-1)/rounded_width);
    void *mapped{}; const D3D12_RANGE no_read{};
    const auto mapped_hr = f.upload.resource->Map(0, &no_read, &mapped);
    if (FAILED(mapped_hr)) return HResultFailure("Map immutable virtual shadow frame snapshot", mapped_hr);
    auto *bytes = static_cast<std::byte *>(mapped);
    const auto pack = [&](const VirtualShadowCaster &c,std::size_t i,std::uint32_t &head,std::uint32_t &changes) {
        const bool is_static = c.immutable_source && !c.skin && !c.wind && !c.animated_alpha;
        const bool was_static = c.previous_immutable_source && !c.previous_skin && !c.previous_wind && !c.previous_animated_alpha;
        const bool changed = c.present != c.previous_present || is_static != was_static ||
            c.instance_generation != c.previous_generation || c.current_revisions != c.previous_revisions ||
            c.current_cut_revision != c.previous_cut_revision || !Same(c.current_bounds, c.previous_bounds);
        const std::uint32_t flags = (c.present ? 1u : 0u) | (c.previous_present ? 2u : 0u) |
            (is_static ? 4u : 0u) | (was_static ? 8u : 0u) | (changed ? 16u : 0u) | (c.baked_source ? 64u : 0u);
        GpuCaster gpu{c.current_bounds, c.previous_bounds,
            {c.instance_index, c.instance_generation, c.source_ref, c.geometry_record},
            {c.draw_group, c.index_count, c.first_index, std::uint32_t(c.base_vertex)},
            {flags, c.meshlet_count, c.present ? std::uint32_t(in.draw_groups[c.draw_group].frontend) : 0u, head}};
        if (changed && (is_static || was_static))
        {
            head = static_cast<std::uint32_t>(i);
            ++changes;
        }
        return gpu;
    };
    if(!in.stable_caster_revision || impl_->stable_revision!=in.stable_caster_revision)
    {
        impl_->stable_words.resize(in.casters.stable.size());
        impl_->stable_head=UINT32_MAX; impl_->stable_changes=0;
        for(std::size_t i=0;i<in.casters.stable.size();++i)
            impl_->stable_words[i]=pack(in.casters.stable[i],i,impl_->stable_head,impl_->stable_changes);
        impl_->stable_revision=in.stable_caster_revision;
    }
    const bool upload_stable=!in.stable_caster_revision || f.stable_revision!=in.stable_caster_revision;
    const auto stable_bytes=in.casters.stable.size()*sizeof(GpuCaster);
    if(upload_stable && stable_bytes) std::memcpy(bytes+t->caster_offset,impl_->stable_words.data(),stable_bytes);
    constants.limits.x=impl_->stable_head;
    [[maybe_unused]] auto changed_static_count=impl_->stable_changes;
    for(std::size_t i=0;i<in.casters.dynamic.size();++i)
    {
        const auto index=in.casters.stable.size()+i;
        const auto gpu=pack(in.casters.dynamic[i],index,constants.limits.x,changed_static_count);
        std::memcpy(bytes+t->caster_offset+index*sizeof(GpuCaster),&gpu,sizeof(gpu));
    }
    f.pending_stable_revision=in.stable_caster_revision;
    const auto caster_copy_offset=upload_stable ? 0 : stable_bytes;
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
    if(segment_count)std::memcpy(bytes+t->segment_offset,plan.command_segments.data(),segment_count*sizeof(ShadowCommandSegment));
    f.root_batches.clear();
    if(plan.fixed_root)
    {
        auto *root_segments=reinterpret_cast<ShadowCommandSegment *>(bytes+t->segment_offset)+segment_count;
        f.root_groups.assign(in.draw_groups.size(),{});
        const auto fresh=[](const VirtualShadowCaster &caster) {
            return caster.present && !IsCompleteBakedStaticCaster(caster.present,caster.immutable_source,
                caster.baked_source,caster.skin,caster.wind,caster.animated_alpha);
        };
        // Stable counts are revision-owned; only the dynamic suffix is counted here.
        // Current GPU caster records supply IDs and native command payloads.
        auto draw_count=impl_->stable_root_count;
        for(std::size_t group=0;group<f.root_groups.size();++group)
            f.root_groups[group].count=impl_->stable_root_counts[group];
        for(const auto &caster:in.casters.dynamic)
            if(fresh(caster))
            {
                if(caster.draw_group>=f.root_groups.size() || draw_count>=t->profile.max_casters)
                    return Invalid("Fixed-root caster commands exceed their preflight reservation.");
                ++f.root_groups[caster.draw_group].count;
                ++draw_count;
            }
        std::uint32_t draw_cursor{},indexed_cursor{},mesh_cursor{};
        for(std::uint32_t group=0;group<in.draw_groups.size();++group)
        {
            auto &layout=f.root_groups[group];
            const auto frontend=in.draw_groups[group].frontend;
            auto &command_cursor=frontend==VirtualShadowFrontend::Indexed?indexed_cursor:mesh_cursor;
            layout.command_first=command_cursor;
            draw_cursor+=layout.count; command_cursor+=layout.count;
            root_segments[group]={layout.command_first,layout.count};
            if(!layout.count)continue;
            const auto stride=frontend==VirtualShadowFrontend::Indexed?kIndexedStride:kMeshStride;
            f.root_batches.push_back({in.draw_groups[group],group,2u,layout.count,
                (UINT64(t->profile.command_capacity)+layout.command_first)*stride,
                (UINT64(t->profile.max_draw_groups)*2+2+group)*sizeof(std::uint32_t)});
        }
        assert(draw_cursor==draw_count);
    }
    const D3D12_RANGE written{0, static_cast<SIZE_T>(t->upload_bytes)}; f.upload.resource->Unmap(0, &written);
    f.batches.clear();
    for (std::uint32_t layer = 0; layer < 2; ++layer)
    {
        f.batch_begins[layer]=f.batches.size();
        for (std::uint32_t group = 0; group < in.draw_groups.size(); ++group)
        {
            const auto segment = layer*group_count+group;
            const auto layout=plan.command_segments[segment];
            if(!layout.capacity)continue;
            const auto stride = in.draw_groups[group].frontend == VirtualShadowFrontend::Indexed ? kIndexedStride : kMeshStride;
            f.batches.push_back({in.draw_groups[group], group, layer, layout.capacity,
                UINT64(layout.first)*stride, UINT64(segment)*4});
        }
        f.batch_counts[layer]=f.batches.size()-f.batch_begins[layer];
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
    auto baked_staging=t->baked_staging.resource ? import(t->baked_staging,"VirtualShadow.CoarseSeed") : ResourceVersion{};
    auto root_static=t->root_static.resource ? import(t->root_static,"VirtualShadow.FixedRootStatic",true) : ResourceVersion{};
    auto root_dynamic=plan.fixed_root&&t->root_dynamic.resource ? import(t->root_dynamic,"VirtualShadow.FixedRootDynamic",true) : ResourceVersion{};
    if(baked_staging.resource && !t->baked_initialized)
    {
        auto seed_upload=graph.AddPass(226,"VirtualShadow.CoarseSeedUpload",QueueHint::Copy);
        baked_staging=seed_upload.Write(baked_staging,Access::CopyDest);
        seed_upload.SetExecute([t](RenderPassContext &ctx) {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            list->CopyBufferRegion(t->baked_staging.resource.Get(),0,t->baked_upload.resource.Get(),0,t->profile.baked->staging.size());
        });
    }
    if(root_static.resource && t->root_static_upload.resource && !t->root_static_initialized)
    {
        auto pass=graph.AddPass(227,"VirtualShadow.FixedRootStaticUpload",QueueHint::Copy);
        root_static=pass.Write(root_static,Access::CopyDest);
        const auto footprint=t->root_static_footprint;
        pass.SetExecute([t,footprint](RenderPassContext &ctx) {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource=t->root_static_upload.resource.Get();
            source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint=footprint;
            D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=t->root_static.resource.Get();
            destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; destination.SubresourceIndex=0;
            list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        });
    }
    auto static_bank = import(t->static_bank, "VirtualShadow.StaticDepth", true);
    auto dynamic_bank = import(t->dynamic_bank, "VirtualShadow.DynamicDepth", true);
    {
        auto pass = graph.AddPass(147, "VirtualShadow.SnapshotUpload", QueueHint::Copy);
        metadata = pass.Write(metadata, Access::CopyDest);
        casters = pass.Write(casters, Access::CopyDest);
        receivers = pass.Write(receivers, Access::CopyDest);
        pass.SetExecute([t, slot=in.frame_slot, caster_bytes=in.casters.size()*sizeof(GpuCaster)-caster_copy_offset, caster_copy_offset,
            receiver_bytes=in.receivers.size_bytes(),segment_bytes=UINT64(segment_count+(plan.fixed_root?group_count:0))*sizeof(ShadowCommandSegment)](RenderPassContext &ctx)
        {
            auto *list = static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList()); auto &frame=t->frames[slot];
            list->CopyBufferRegion(frame.metadata.resource.Get(), 0, frame.upload.resource.Get(), 0, sizeof(VirtualShadowConstants));
            if (caster_bytes) list->CopyBufferRegion(frame.casters.resource.Get(), caster_copy_offset, frame.upload.resource.Get(), t->caster_offset+caster_copy_offset, caster_bytes);
            if (receiver_bytes) list->CopyBufferRegion(frame.receivers.resource.Get(), 0, frame.upload.resource.Get(), t->receiver_offset, receiver_bytes);
            if(segment_bytes)list->CopyBufferRegion(frame.metadata.resource.Get(),kSegmentMetadataOffset,
                frame.upload.resource.Get(),t->segment_offset,segment_bytes);
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
        pass.Read(metadata,Access::ComputeRead);
        if(baked_staging.resource) pass.Read(baked_staging,Access::ComputeRead);
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
        pass.Read(metadata,Access::ComputeRead);
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
        pass.Read(metadata,Access::ComputeRead);
        if(baked_staging.resource) pass.Read(baked_staging,Access::ComputeRead);
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
    if(plan.fixed_root)
    {
        auto pass=graph.AddPass(228,"VirtualShadow.FixedRootCurrentCommands",QueueHint::Compute);
        pass.Read(metadata,Access::ComputeRead); pass.Read(casters,Access::ComputeRead);
        draws=pass.ReadWrite(draws,Access::UnorderedWrite); indexed=pass.ReadWrite(indexed,Access::UnorderedWrite);
        mesh=pass.ReadWrite(mesh,Access::UnorderedWrite); counts=pass.ReadWrite(counts,Access::UnorderedWrite);
        pages=pass.ReadWrite(pages,Access::UnorderedWrite);
        pass.SetExecute([t,slot=in.frame_slot,group_count,caster_count=std::uint32_t(in.casters.size()),
            count_handle=counts.resource,page_handle=pages.resource](RenderPassContext &ctx) {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            BindCompute(list,*t,t->frames[slot],slot,3);
            list->SetComputeRoot32BitConstant(3,3,0);
            list->Dispatch((std::max(group_count,1u)+63)/64,1,1);
            ctx.UavBarrier(count_handle); ctx.UavBarrier(page_handle);
            list->SetComputeRoot32BitConstant(3,4,0);
            if(caster_count)list->Dispatch((caster_count+63)/64,1,1);
        });
    }
    out.metadata = metadata; out.page_table = pages; out.draw_records = draws;
    out.indexed_commands = indexed; out.mesh_commands = mesh; out.command_counts = counts;
    out.resources = {f.metadata.resource.Get(), f.pages.resource.Get(), t->static_bank.resource.Get(),
        t->dynamic_bank.resource.Get(), f.draws.resource.Get(),t->root_static.resource.Get(),
        plan.fixed_root?t->root_dynamic.resource.Get():nullptr};
    for (std::uint32_t i=0; i<kVirtualShadowDescriptorCount; ++i)
        out.srvs[i] = Cpu(t->srv_heap.Get(), in.frame_slot*kVirtualShadowDescriptorCount+i, t->descriptor_stride);
    out.depth_views = {Cpu(t->dsv_heap.Get(), 0, t->dsv_stride), Cpu(t->dsv_heap.Get(), 1, t->dsv_stride)};
    if(t->root_dynamic.resource)out.depth_views[2]=Cpu(t->dsv_heap.Get(),2,t->dsv_stride);
    out.indexed_arguments = f.indexed.resource.Get(); out.mesh_arguments = f.mesh.resource.Get(); out.counts_resource = f.counts.resource.Get();
    out.bank_extent = t->extent;
    if(t->root_static.resource)out.root_extent=t->profile.baked->root_static_extent;
    for (std::uint32_t layer=0; layer<2; ++layer)
    {
        auto pass = graph.AddPass(152+layer, layer ? "VirtualShadow.DynamicBank" : "VirtualShadow.StaticBank", QueueHint::Direct);
        if(!layer && baked_staging.resource) pass.Read(baked_staging,Access::ShaderRead);
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
            list->SetGraphicsRootSignature(t->root.Get()); list->SetPipelineState(!layer && t->profile.baked ? t->seed_pso.Get() : t->clear_pso.Get());
            list->SetGraphicsRootConstantBufferView(0, frame.upload.resource->GetGPUVirtualAddress());
            list->SetGraphicsRootShaderResourceView(4,t->baked_staging.resource ? t->baked_staging.resource->GetGPUVirtualAddress() : 0);
            list->SetGraphicsRootShaderResourceView(5,frame.metadata.resource->GetGPUVirtualAddress()+kSegmentMetadataOffset);
            list->SetGraphicsRootDescriptorTable(1, Gpu(t->compute_heap.Get(), slot*kComputeDescriptors, t->descriptor_stride));
            list->SetGraphicsRootDescriptorTable(2, Gpu(t->compute_heap.Get(), slot*kComputeDescriptors+kComputeSrvs, t->descriptor_stride));
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->ExecuteIndirect(t->clear_signature.Get(), t->profile.physical_pages, frame.clear.resource.Get(),
                UINT64(layer)*t->profile.physical_pages*kClearStride, frame.counts.resource.Get(),
                UINT64(group_count*2+layer)*4);
            return callback(ctx,outputs,std::span<const VirtualShadowDrawBatch>{frame.batches}.subspan(frame.batch_begins[layer],frame.batch_counts[layer]));
        });
    }
    if(plan.fixed_root)
    {
        auto pass=graph.AddPass(229,"VirtualShadow.FixedRootCurrent",QueueHint::Direct);
        pass.Read(metadata,Access::ShaderRead); pass.Read(pages,Access::ShaderRead); pass.Read(draws,Access::ShaderRead);
        pass.Read(indexed,Access::IndirectArgs); pass.Read(mesh,Access::IndirectArgs); pass.Read(counts,Access::IndirectArgs);
        for(const auto &read:in.native_reads)if(read.version.resource)pass.Read(read.version,read.access);
        if(in.draw&&!in.draw_groups.empty()&&in.declare_material_sampling)in.declare_material_sampling(pass);
        root_dynamic=pass.Write(root_dynamic,Access::DepthWrite);
        graph.AddOrderingDependency(152,229); graph.AddOrderingDependency(153,229);
        pass.SetExecuteChecked([t,slot=in.frame_slot,callback=in.draw,outputs=out](RenderPassContext &ctx) -> Result {
            auto *list=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            const auto dsv=outputs.depth_views[2];
            list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0.0f,0,0,nullptr);
            list->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
            const D3D12_VIEWPORT viewport{0,0,float(outputs.root_extent.width),float(outputs.root_extent.height),0,1};
            const D3D12_RECT scissor{0,0,LONG(outputs.root_extent.width),LONG(outputs.root_extent.height)};
            list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
            // Like the fine-page batches, the waited frame owns these current
            // bindings until Execute has joined all recording callbacks.
            return callback(ctx,outputs,t->frames[slot].root_batches);
        });
    }
    {
        auto pass = graph.AddPass(154, "VirtualShadow.Publish", QueueHint::Compute);
        graph.AddOrderingDependency(152, 154); graph.AddOrderingDependency(153, 154);
        if(plan.fixed_root)graph.AddOrderingDependency(229,154);
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
        if(root_static.resource)pass.Read(root_static,Access::ShaderRead);
        if(root_dynamic.resource)pass.Read(root_dynamic,Access::ShaderRead);
        pass.SetExecute([](RenderPassContext &) {});
    }
    out.page_table = pages; out.static_bank = static_bank; out.dynamic_bank = dynamic_bank;
    out.metadata=metadata; out.page_table=pages; out.draw_records=draws;
    out.indexed_commands=indexed; out.mesh_commands=mesh; out.command_counts=counts;
    out.root_static=root_static; out.root_dynamic=root_dynamic;
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
        impl_->active->frames[impl_->pending_slot].stable_revision=impl_->active->frames[impl_->pending_slot].pending_stable_revision;
        impl_->successful = impl_->pending;
        impl_->last_successful_slot = impl_->pending_slot;
        impl_->has_success = true;
        impl_->active->banks_initialized = true;
        if(impl_->active->baked_staging.resource && !impl_->active->baked_initialized)
        { impl_->active->baked_initialized=true; impl_->active->baked_upload_completion=completion; }
        if(impl_->active->root_static.resource && !impl_->active->root_static_initialized)
        { impl_->active->root_static_initialized=true; impl_->active->root_static_upload_completion=completion; }
    }
    else impl_->faulted = true;
    impl_->pending_graph = nullptr; impl_->tracked.clear(); return Result::Success();
}
Result VirtualShadowSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if (impl_->pending_graph) return Invalid("Virtual shadow reset requires submission accounting first.");
    BeginFramePolicy();
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
        (t->dynamic_bank.allocation ? t->dynamic_bank.allocation->GetSize() : 0), t->profile.physical_pages,
        impl_->has_success && impl_->successful.baked_depth.z!=0,
        t->profile.baked ? t->profile.baked->description.levels[2].width*t->profile.baked->description.levels[2].height : 0,
        t->baked_staging.allocation ? t->baked_staging.allocation->GetSize() : 0,
        impl_->cpu_plan_prepared, impl_->last_guarantee};
}
std::uint64_t VirtualShadowSystem::AllocatedBytes() const noexcept
{
    std::uint64_t bytes=impl_->active ? impl_->active->allocated : 0;
    for (const auto &t : impl_->retired) bytes += t->allocated;
    return bytes;
}
std::array<const AllocationResource *, 5> VirtualShadowSystem::DiagnosticResources() const noexcept
{
    if (!impl_->active || !impl_->has_success || impl_->last_successful_slot >= impl_->active->frames.size()) return {};
    const auto &storage = *impl_->active;
    const auto &frame = storage.frames[impl_->last_successful_slot];
    return {&frame.metadata, &frame.pages, &storage.static_bank, &storage.dynamic_bank, &frame.counts};
}
} // namespace hs
