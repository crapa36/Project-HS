#include "renderer_screen_overlay.hpp"
#include "renderer_pipeline_cache.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr std::uint32_t kMaximumCommands=1024,kConstantStride=256;
struct alignas(16) GpuScreenCommand
{
    DirectX::XMFLOAT4 color,appearance,coverage;
    DirectX::XMUINT4 identity;
};
struct alignas(16) ScreenConstants
{
    DirectX::XMFLOAT4 render_size,output_size;
    DirectX::XMUINT4 info;
};
static_assert(sizeof(GpuScreenCommand)==64&&sizeof(ScreenConstants)==48);
constexpr std::uint32_t kSlotStride=kConstantStride+kMaximumCommands*sizeof(GpuScreenCommand);
Result Failure(ErrorCode code,const char *message)
{ return Result::Failure(code,"hs_vfx_screen",message); }
Result Native(const char *operation,HRESULT value)
{ return Result::Failure(ErrorCode::TaskFailed,"hs_vfx_screen",std::string(operation)+": "+std::to_string(value)); }
DirectX::XMFLOAT4 Size(ViewExtent v)
{ return {float(v.width),float(v.height),1.0f/v.width,1.0f/v.height}; }
bool Valid(ViewExtent v)
{ return v.width&&v.height&&v.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION&&v.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION; }
}
struct ScreenOverlayTargets
{
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> mask_pipeline,draw_pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors,exports;
    AllocationResource upload,mask;
    std::byte *mapped{};
    ViewExtent capacity;
    std::uint32_t stride{};
    std::vector<CompletionSet> slot_completion;
    std::vector<std::uint32_t> counts;
    std::vector<ViewExtent> output_extents;
    CompletionSet completion;
    std::uint64_t bytes{};
    bool usable{true};
    ~ScreenOverlayTargets(){if(mapped&&upload.resource)upload.resource->Unmap(0,nullptr);}
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(std::uint32_t slot,std::uint32_t offset) const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot*2+offset)*stride;return h;}
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(std::uint32_t slot) const
    {auto h=descriptors->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(slot*2)*stride;return h;}
    void Bind(ID3D12GraphicsCommandList *list,std::uint32_t slot,bool compute) const
    {
        ID3D12DescriptorHeap *heaps[]{descriptors.Get()};list->SetDescriptorHeaps(1,heaps);
        const auto base=upload.resource->GetGPUVirtualAddress()+UINT64(slot)*kSlotStride;
        if(compute)
        {list->SetComputeRootSignature(root.Get());list->SetComputeRootConstantBufferView(0,base);
         list->SetComputeRootShaderResourceView(1,base+kConstantStride);list->SetComputeRootDescriptorTable(2,Gpu(slot));}
        else
        {list->SetGraphicsRootSignature(root.Get());list->SetGraphicsRootConstantBufferView(0,base);
         list->SetGraphicsRootShaderResourceView(1,base+kConstantStride);list->SetGraphicsRootDescriptorTable(2,Gpu(slot));}
    }
};
std::uint64_t ScreenOverlayCandidate::AllocatedBytes() const noexcept{return targets_?targets_->bytes:0;}
std::uint64_t ScreenOverlaySystem::AllocatedBytes() const noexcept{return targets_?targets_->bytes:0;}
Result ScreenOverlaySystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ViewExtent capacity,
    std::uint32_t frames,const ShaderReader &read,bool enabled,RendererPipelineCache &pipelineCache,
    ScreenOverlayCandidate &candidate) const
{
    candidate={};
    if(!enabled){candidate.prepared_=true;return Result::Success();}
    if(!device||!allocator||!Valid(capacity)||!frames||frames>16||!read)
        return Failure(ErrorCode::InvalidArgument,"Screen overlay preparation inputs are invalid.");
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{DXGI_FORMAT_R8_UNORM};
    auto hr=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
    if(FAILED(hr))return Native("Query overlay mask format",hr);
    if(!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        return Failure(ErrorCode::TaskFailed,"Screen overlay mask requires R8 load and typed UAV store.");
    auto next=std::make_shared<ScreenOverlayTargets>();next->device=device;next->capacity=capacity;
    next->slot_completion.resize(frames);next->counts.resize(frames);next->output_extents.resize(frames);
    next->stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::array<D3D12_DESCRIPTOR_RANGE,2> ranges{{
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,1,34,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,34,1}}};
    std::array<D3D12_ROOT_PARAMETER,3> parameters{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;parameters[0].Descriptor={0,34};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor={0,34};
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[2].DescriptorTable={2,ranges.data()};
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.RegisterSpace=34;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    sampler.MaxAnisotropy=1;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC root{UINT(parameters.size()),parameters.data(),1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized,error;hr=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error);
    if(FAILED(hr))return Native("Serialize screen overlay root",hr);
    hr=device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&next->root));
    if(FAILED(hr))return Native("Create screen overlay root",hr);
    if(auto registered=pipelineCache.RegisterRootSignature(next->root.Get(),
        {static_cast<const std::byte *>(serialized->GetBufferPointer()),serialized->GetBufferSize()});
        !registered)return registered;
    std::vector<std::byte> cs,vs,ps;
    if(auto r=read("vfx_screen_overlay_mask_cs.dxil",cs);!r)return r;
    if(auto r=read("vfx_screen_overlay_vs.dxil",vs);!r)return r;
    if(auto r=read("vfx_screen_overlay_ps.dxil",ps);!r)return r;
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};compute.pRootSignature=next->root.Get();compute.CS={cs.data(),cs.size()};
    hr=pipelineCache.CreateCompute(&compute,IID_PPV_ARGS(&next->mask_pipeline));
    if(FAILED(hr))return Native("Create screen overlay mask pipeline",hr);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};graphics.pRootSignature=next->root.Get();
    graphics.VS={vs.data(),vs.size()};graphics.PS={ps.data(),ps.size()};graphics.SampleMask=UINT_MAX;
    graphics.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    graphics.RasterizerState.DepthClipEnable=TRUE;graphics.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    graphics.DepthStencilState.StencilReadMask=D3D12_DEFAULT_STENCIL_READ_MASK;
    graphics.DepthStencilState.StencilWriteMask=D3D12_DEFAULT_STENCIL_WRITE_MASK;
    graphics.DepthStencilState.FrontFace=graphics.DepthStencilState.BackFace=
        {D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;graphics.NumRenderTargets=1;
    graphics.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;graphics.SampleDesc.Count=1;
    auto &blend=graphics.BlendState.RenderTarget[0];blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_ONE;
    blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    hr=pipelineCache.CreateGraphics(&graphics,IID_PPV_ARGS(&next->draw_pipeline));
    if(FAILED(hr))return Native("Create screen overlay draw pipeline",hr);
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=capacity.width;texture.Height=capacity.height;texture.DepthOrArraySize=1;texture.MipLevels=1;
    texture.Format=DXGI_FORMAT_R8_UNORM;texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=D3D12_HEAP_TYPE_DEFAULT;
    hr=allocator->CreateResource(&allocation,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,
        &next->mask.allocation,IID_PPV_ARGS(next->mask.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create screen overlay mask",hr);next->mask.graph_access=Access::Common;
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=UINT64(frames)*kSlotStride;
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
    hr=allocator->CreateResource(&allocation,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,
        &next->upload.allocation,IID_PPV_ARGS(next->upload.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create screen overlay commands",hr);next->upload.graph_access=Access::AllShaderRead;
    D3D12_RANGE no_read{0,0};void *mapped{};hr=next->upload.resource->Map(0,&no_read,&mapped);
    if(FAILED(hr))return Native("Map screen overlay commands",hr);next->mapped=static_cast<std::byte *>(mapped);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors=frames*2;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&next->descriptors));
    if(FAILED(hr))return Native("Create screen overlay descriptors",hr);
    heap.NumDescriptors=1;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&next->exports));
    if(FAILED(hr))return Native("Create screen overlay export",hr);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R8_UNORM;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(next->mask.resource.Get(),&srv,next->exports->GetCPUDescriptorHandleForHeapStart());
    next->bytes=next->mask.allocation->GetSize()+next->upload.allocation->GetSize();
    candidate.targets_=std::move(next);candidate.prepared_=true;return Result::Success();
}
Result ScreenOverlaySystem::Commit(ScreenOverlayCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.prepared_)return Failure(ErrorCode::InvalidArgument,"Screen overlay candidate is unprepared.");
    if(pending_graph_||(targets_&&!targets_->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Drain screen overlay GPU uses before replacing its targets.");
    targets_=std::move(candidate.targets_);candidate.prepared_=false;return Result::Success();
}
Result ScreenOverlaySystem::RegisterMask(RenderGraphBuilder &graph,const ScreenOverlayInputs &input,ScreenOverlayOutputs &output)
{
    output={};if(input.commands.empty()||input.intensity==0)return Result::Success();
    const auto t=targets_;
    if(!t||!t->usable||pending_graph_||input.frame_slot>=t->counts.size()||input.commands.size()>kMaximumCommands||
       !t->slot_completion[input.frame_slot].IsSatisfiedBy(input.completed)||!Valid(input.render_active)||!Valid(input.output_active)||
       input.render_active.width>t->capacity.width||input.render_active.height>t->capacity.height||
       !input.gradient.resource||!input.gradient_resource||!std::isfinite(input.intensity)||input.intensity<0||input.intensity>1)
        return Failure(ErrorCode::InvalidState,"Screen overlay registration requires prepared targets, a waited frame bank and valid inputs.");
    const auto gradient=input.gradient_resource->GetDesc();
    if(gradient.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||gradient.DepthOrArraySize!=1||!gradient.Height)
        return Failure(ErrorCode::InvalidArgument,"Screen overlay requires the authored 2D gradient LUT.");
    auto *commands=reinterpret_cast<GpuScreenCommand *>(t->mapped+SIZE_T(input.frame_slot)*kSlotStride+kConstantStride);
    std::uint32_t count{};
    for(const auto &v:input.commands)
    {
        const float values[]{v.color.x,v.color.y,v.color.z,v.hdr,v.alpha,v.gradient_age,v.envelope,v.center_clear,v.edge_power,
            v.source_direction.x,v.source_direction.y,v.source_direction.z};
        if(!std::all_of(std::begin(values),std::end(values),[](float x){return std::isfinite(x);})||v.color.x<0||v.color.y<0||v.color.z<0||v.hdr<0||v.alpha<0||v.alpha>1||
           v.gradient_age<0||v.gradient_age>1||v.envelope<0||v.envelope>1||v.center_clear<0||v.center_clear>=1||v.edge_power<=0||
           v.gradient_row>=gradient.Height||v.shape>VfxScreenShape::DirectionalArc)
            return Failure(ErrorCode::InvalidArgument,"Screen overlay command violates its analytic opacity contract.");
        if(v.alpha==0)continue;
        const auto dot=[](Float3 a,DirectX::XMFLOAT3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
        commands[count++]={{v.color.x,v.color.y,v.color.z,1},{v.alpha,v.hdr,v.gradient_age,v.envelope},
            {v.center_clear,v.edge_power,dot(v.source_direction,input.camera_right),-dot(v.source_direction,input.camera_up)},
            {std::uint32_t(v.shape),v.gradient_row,0,0}};
    }
    t->counts[input.frame_slot]=count;if(!count)return Result::Success();
    t->output_extents[input.frame_slot]=input.output_active;
    const ScreenConstants constants{Size(input.render_active),Size(input.output_active),
        {count,std::bit_cast<std::uint32_t>(input.intensity),gradient.Height,0}};
    std::memcpy(t->mapped+SIZE_T(input.frame_slot)*kSlotStride,&constants,sizeof(constants));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=gradient.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    t->device->CreateShaderResourceView(input.gradient_resource,&srv,t->Cpu(input.frame_slot,0));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R8_UNORM;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    t->device->CreateUnorderedAccessView(t->mask.resource.Get(),nullptr,&uav,t->Cpu(input.frame_slot,1));
    const auto mask=graph.ImportTexture({t->mask.resource.Get(),t->mask.graph_completion,t->mask.graph_queue},t->mask.graph_access,"VfxScreen.Reactive");
    const auto upload=graph.ImportBuffer({t->upload.resource.Get(),t->upload.graph_completion,t->upload.graph_queue},Access::AllShaderRead,"VfxScreen.Commands");
    auto pass=graph.AddPass(290,"VfxScreenOverlayAlpha",QueueHint::Direct);
    pass.Read(input.gradient,Access::ComputeRead);pass.Read(graph.Version(upload),Access::ComputeRead);
    const auto reactive=pass.Write(graph.Version(mask),Access::UnorderedWrite);
    pass.SetExecute([t,slot=input.frame_slot,extent=input.render_active](RenderPassContext &context){
        auto *list=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());t->Bind(list,slot,true);
        list->SetPipelineState(t->mask_pipeline.Get());list->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);
    });
    pending_graph_=&graph;pending_mask_=mask;pending_upload_=upload;pending_slot_=input.frame_slot;
    output={reactive,graph.Version(upload),t->mask.resource.Get(),t->exports->GetCPUDescriptorHandleForHeapStart(),true};
    return Result::Success();
}
void ScreenOverlaySystem::Draw(ID3D12GraphicsCommandList *list,std::uint32_t slot,D3D12_CPU_DESCRIPTOR_HANDLE rtv) const
{
    const auto t=targets_;if(!t||slot>=t->counts.size()||!t->counts[slot]||!pending_graph_)return;
    t->Bind(list,slot,false);list->SetPipelineState(t->draw_pipeline.Get());list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    const auto size=t->output_extents[slot];const D3D12_VIEWPORT viewport{0,0,float(size.width),float(size.height),0,1};
    const D3D12_RECT scissor{0,0,LONG(size.width),LONG(size.height)};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,t->counts[slot],0,0);
}
Result ScreenOverlaySystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    if(!pending_graph_)return Result::Success();
    if(pending_graph_!=&graph)return Failure(ErrorCode::InvalidArgument,"Screen overlay completed a different graph.");
    const auto t=targets_;t->completion.Merge(completion);t->slot_completion[pending_slot_].Merge(completion);
    for(const auto pair:{std::pair{&t->mask,pending_mask_},std::pair{&t->upload,pending_upload_}})
    {
        const auto used=graph.ResourceCompletion(pair.second);t->completion.Merge(used);t->slot_completion[pending_slot_].Merge(used);
        pair.first->graph_completion.Merge(completion);pair.first->graph_completion.Merge(used);
        if(success){pair.first->graph_access=graph.FinalAccess(pair.second);pair.first->graph_queue=graph.FinalQueue(pair.second);}
    }
    if(!success)t->usable=false;pending_graph_=nullptr;pending_mask_={};pending_upload_={};return Result::Success();
}
Result ScreenOverlaySystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(pending_graph_||(targets_&&!targets_->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Complete and drain screen overlay GPU uses before reset.");
    targets_.reset();return Result::Success();
}
}
