#include "renderer_ray_shadow.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <tuple>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT kPacketDescriptors=kRayDescriptorCount+kRayShadowOwnDescriptorCount;
Result Failure(ErrorCode code,std::string message)
{return Result::Failure(code,"hs_renderer_ray_shadow",std::move(message));}
Result Native(std::string_view operation,HRESULT hr)
{return Failure(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+std::to_string(std::int32_t(hr))+").");}
bool Extent(ViewExtent e)
{return e.width&&e.height&&e.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION&&e.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;}
DirectX::XMFLOAT4 Size(ViewExtent e){return {float(e.width),float(e.height),1.0f/e.width,1.0f/e.height};}
ViewExtent TraceExtent(ViewExtent e,const render_data::QualityProfile &p)
{return {std::max(1u,UINT(std::ceil(e.width*p.rt_axis_ratio))),std::max(1u,UINT(std::ceil(e.height*p.rt_axis_ratio))) };}
bool SameLight(const LightView &a,const LightView &b)
{
    return a.stable_id==b.stable_id&&a.generation==b.generation&&a.revision==b.revision&&a.type==b.type&&
        a.position.x==b.position.x&&a.position.y==b.position.y&&a.position.z==b.position.z&&a.range==b.range&&
        a.direction.x==b.direction.x&&a.direction.y==b.direction.y&&a.direction.z==b.direction.z&&
        a.outer_cone_radians==b.outer_cone_radians&&a.inner_cone_radians==b.inner_cone_radians&&a.emitter_radius==b.emitter_radius&&
        a.shape.x==b.shape.x&&a.shape.y==b.shape.y&&a.area_tangent.x==b.area_tangent.x&&
        a.area_tangent.y==b.area_tangent.y&&a.area_tangent.z==b.area_tangent.z&&a.two_sided==b.two_sided;
}
bool Area(const LightView &light)
{return light.type==LightType::Rectangle||light.type==LightType::Disk;}
float EmitterBound(const LightView &light)
{
    if(light.type==LightType::Rectangle)return std::hypot(light.shape.x*0.5f,light.shape.y*0.5f);
    if(light.type==LightType::Disk)return std::max(light.shape.x,light.shape.y);
    return light.emitter_radius;
}
bool Supported(const LightView &light)
{
    const auto &p=light.position;const auto &d=light.direction;
    const bool area=Area(light);
    if(area)
    {
        const auto &t=light.area_tangent;
        const double bx=double(d.y)*t.z-double(d.z)*t.y,by=double(d.z)*t.x-double(d.x)*t.z,
            bz=double(d.x)*t.y-double(d.y)*t.x;
        if(light.unit!=LightUnit::Luminance||!std::isfinite(light.shape.x)||!std::isfinite(light.shape.y)||
            light.shape.x<=0||light.shape.y<=0||!std::isfinite(std::hypot(bx,by,bz))||std::hypot(bx,by,bz)<=0)
            return false;
    }
    return (area||light.type==LightType::Point||light.type==LightType::Spot)&&light.stable_id&&light.generation&&light.revision&&
        std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(light.range)&&light.range>0.02f&&
        std::isfinite(EmitterBound(light))&&EmitterBound(light)>=0.0f&&EmitterBound(light)<light.range&&
        std::isfinite(light.quantity)&&light.quantity>=0&&std::isfinite(light.color.x)&&std::isfinite(light.color.y)&&std::isfinite(light.color.z)&&
        (light.type!=LightType::Spot||(std::isfinite(d.x)&&std::isfinite(d.y)&&std::isfinite(d.z)&&d.x*d.x+d.y*d.y+d.z*d.z>1e-12f&&
            std::isfinite(light.outer_cone_radians)&&light.outer_cone_radians>0&&light.outer_cone_radians<DirectX::XM_PIDIV2));
}
double Priority(const LightView &light,DirectX::XMFLOAT3 camera,bool moved)
{
    const double x=double(light.position.x)-camera.x,y=double(light.position.y)-camera.y,z=double(light.position.z)-camera.z;
    const double distance2=x*x+y*y+z*z;
    const double luminance=std::max(0.0,double(light.color.x)*0.2126+double(light.color.y)*0.7152+double(light.color.z)*0.0722);
    const double contribution=std::min(1.0,double(light.range)*light.range/std::max(distance2,0.0001));
    const double flux=Area(light)?double(light.quantity)*std::numbers::pi_v<double>*
        double(light.shape.x)*light.shape.y*(light.type==LightType::Rectangle?1.0:std::numbers::pi_v<double>)*
        (light.two_sided?2.0:1.0):double(light.quantity);
    const double bound=EmitterBound(light);
    return flux*luminance*contribution*(moved?1.125:1.0)/std::max(distance2,bound*bound+0.01);
}
bool Guide(const RenderGraphBuilder &graph,RayShadowGuide input,ViewExtent active,DXGI_FORMAT format)
{
    if(!input.resource||!input.version.resource||input.version.resource.kind!=ResourceKind::Texture||graph.Resolve(input.version.resource)!=input.resource)return false;
    const auto d=input.resource->GetDesc();return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.DepthOrArraySize==1&&
        d.SampleDesc.Count==1&&d.Width>=active.width&&d.Height>=active.height&&d.Format==format;
}
bool BufferVersion(const RenderGraphBuilder &graph,ResourceVersion version,ID3D12Resource *resource)
{return resource&&version.resource&&version.resource.kind==ResourceKind::Buffer&&graph.Resolve(version.resource)==resource&&resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER;}
bool BlueNoise(const RenderGraphBuilder &graph,RayShadowGuide input)
{
    if(!input.resource||!input.version.resource||graph.Resolve(input.version.resource)!=input.resource)return false;
    const auto d=input.resource->GetDesc();return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==64&&d.Height==64&&
        d.DepthOrArraySize==64&&d.MipLevels==1&&d.SampleDesc.Count==1&&d.Format==DXGI_FORMAT_R8_UNORM;
}
D3D12_RESOURCE_DESC Buffer(std::uint64_t bytes)
{D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;
 d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;}
TextureSubresourceRange Layer(UINT layer){return {0,1,layer,1,0,1};}
} // namespace

struct RayShadowStorage
{
    struct ProducerOwner{std::vector<ComPtr<D3D12MA::Allocation>> allocations;std::vector<ComPtr<ID3D12Resource>> resources;};
    struct Context
    {
        LightView light;bool occupied{},history_valid{},selected{};ViewExtent previous_active;
        std::unique_ptr<NrdSystem> nrd;
        AllocationResource motion,normal,view_z,penumbra;
        std::shared_ptr<ProducerOwner> owner;
    };
    struct Frame
    {
        AllocationResource upload,metadata,records;std::byte *mapped{};CompletionSet completion;
        std::vector<RayShaderBinder> binders;std::vector<ComPtr<ID3D12Resource>> reads;
        std::vector<std::shared_ptr<const void>> owners;
        ~Frame(){if(mapped&&upload.resource)upload.resource->Unmap(0,nullptr);}
        Frame()=default;Frame(const Frame &)=delete;Frame &operator=(const Frame &)=delete;
        Frame(Frame &&other)noexcept:upload(std::move(other.upload)),metadata(std::move(other.metadata)),records(std::move(other.records)),
            mapped(std::exchange(other.mapped,nullptr)),completion(other.completion),binders(std::move(other.binders)),
            reads(std::move(other.reads)),owners(std::move(other.owners)){}
    };
    ComPtr<ID3D12Device> device;ComPtr<D3D12MA::Allocator> allocator;ComPtr<ID3D12RootSignature> root;
    RayShadowProfile profile;ViewExtent trace_capacity;UINT maximum_lights{},samples{},read_index{},stride{};
    bool native{},initialized{},usable{true};std::uint64_t allocated{};CompletionSet completion;
    AllocationResource constants,raw,visibility,confidence;
    std::array<AllocationResource,2> history,validation;
    std::byte *mapped{};
    std::array<ComPtr<ID3D12PipelineState>,3> pipelines;
    ComPtr<ID3D12DescriptorHeap> descriptors,exports,clear_cpu_heap,clear_heap;
    std::vector<Context> contexts;std::vector<Frame> frames;
    ~RayShadowStorage(){if(mapped&&constants.resource)constants.resource->Unmap(0,nullptr);}
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT frame,UINT context,UINT index)const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T((frame*maximum_lights+context)*kPacketDescriptors+index)*stride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE Own(UINT frame,UINT context,UINT index)const{return Cpu(frame,context,kRayDescriptorCount+index);}
    D3D12_CPU_DESCRIPTOR_HANDLE Export(UINT frame,UINT index)const
    {auto h=exports->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(frame*kRayShadowExportCount+index)*stride;return h;}
    void TextureSrv(ID3D12Resource *resource,DXGI_FORMAT format,D3D12_CPU_DESCRIPTOR_HANDLE h,bool array=false)
    {D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.ViewDimension=array?D3D12_SRV_DIMENSION_TEXTURE2DARRAY:D3D12_SRV_DIMENSION_TEXTURE2D;
     d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;if(array){d.Texture2DArray.MipLevels=1;d.Texture2DArray.ArraySize=maximum_lights;}
     else d.Texture2D.MipLevels=1;device->CreateShaderResourceView(resource,&d,h);}
    void TextureUav(ID3D12Resource *resource,DXGI_FORMAT format,D3D12_CPU_DESCRIPTOR_HANDLE h,bool array=false)
    {D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;d.ViewDimension=array?D3D12_UAV_DIMENSION_TEXTURE2DARRAY:D3D12_UAV_DIMENSION_TEXTURE2D;
     if(array)d.Texture2DArray.ArraySize=maximum_lights;device->CreateUnorderedAccessView(resource,nullptr,&d,h);}
    void Structured(ID3D12Resource *resource,UINT count,UINT size,D3D12_CPU_DESCRIPTOR_HANDLE h)
    {D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
     d.Buffer.NumElements=count;d.Buffer.StructureByteStride=size;device->CreateShaderResourceView(resource,&d,h);}
    void Raw(ID3D12Resource *resource,D3D12_CPU_DESCRIPTOR_HANDLE h)
    {D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
     d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Buffer.NumElements=resource?UINT(resource->GetDesc().Width/4):1;
     d.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;device->CreateShaderResourceView(resource,&d,h);}
};
namespace
{
Result Allocate(RayShadowStorage &s,AllocationResource &resource,D3D12_RESOURCE_DESC desc,bool upload=false,Access access=Access::CopySource)
{
    D3D12MA::ALLOCATION_DESC ad{};ad.HeapType=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
    const auto hr=s.allocator->CreateResource(&ad,&desc,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COMMON,nullptr,
        &resource.allocation,IID_PPV_ARGS(resource.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create ray shadow resource",hr);resource.graph_access=upload?access:Access::Common;
    s.allocated+=resource.allocation->GetSize();
    if(s.allocated>s.profile.byte_budget)return Failure(ErrorCode::InvalidArgument,"Ray shadow native allocations exceed byte_budget.");
    return Result::Success();
}
Result Texture(RayShadowStorage &s,AllocationResource &resource,ViewExtent extent,DXGI_FORMAT format,UINT layers=1)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=extent.width;d.Height=extent.height;
    d.DepthOrArraySize=static_cast<UINT16>(layers);d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return Allocate(s,resource,d);
}
Result ResetNative(RayShadowStorage &s,const CompletionSet &completed)
{for(auto &c:s.contexts)if(c.nrd)if(auto r=c.nrd->ResetAfterCompletion(completed);!r)return r;return Result::Success();}
} // namespace
struct RayShadowSystem::Impl
{
    struct Import{AllocationResource *resource;ResourceHandle handle;};
    struct PendingContext{UINT slot{};LightView light;bool native_registered{};};
    struct Pending{const RenderGraphBuilder *graph{};UINT frame{},write_index{};ViewExtent active;
        std::vector<Import> imports;std::vector<PendingContext> contexts;RayShadowStatus status;}pending;
    std::shared_ptr<RayShadowStorage> storage;RayShadowStatus status;
};
RayShadowSystem::RayShadowSystem():impl_(std::make_unique<Impl>()){}
RayShadowSystem::~RayShadowSystem()=default;
std::uint64_t RayShadowCandidate::AllocatedBytes()const noexcept{return storage_?storage_->allocated:0;}
std::uint64_t RayShadowSystem::AllocatedBytes()const noexcept{return impl_->storage?impl_->storage->allocated:0;}
RayShadowStatus RayShadowSystem::Status()const{return impl_->status;}
Result RayShadowSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    const RayShadowProfile &profile,const RayShaderReader &reader,RayShadowCandidate &candidate)const
{
    candidate={};if(!profile.enabled){candidate.prepared_=true;return Result::Success();}
    if(!device||!allocator||!root||!reader||!Extent(profile.render_capacity)||!profile.byte_budget||!profile.frame_slots||profile.frame_slots>16||
        !profile.maximum_lights||profile.maximum_lights>kRayShadowMaximumLights||
        !std::isfinite(profile.maximum_trace_distance_m)||profile.maximum_trace_distance_m<5.0f||profile.maximum_trace_distance_m>500.0f||
        (profile.quality!=GraphicsQuality::Low&&profile.quality!=GraphicsQuality::Medium&&profile.quality!=GraphicsQuality::High&&profile.quality!=GraphicsQuality::Ultra))
        return Failure(ErrorCode::InvalidArgument,"Invalid ray shadow profile/native inputs.");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options{};D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options,sizeof(options)))||options.RaytracingTier<D3D12_RAYTRACING_TIER_1_1||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)))||model.HighestShaderModel<D3D_SHADER_MODEL_6_5)
        return Failure(ErrorCode::TaskFailed,"Selective ray shadows require actual DXR1.1 and Shader Model6.5.");
    auto s=std::make_shared<RayShadowStorage>();s->device=device;s->allocator=allocator;s->root=root;s->profile=profile;
    const UINT light_limit=profile.quality_profile.ray_shadow_max_lights;
    s->maximum_lights=std::min(profile.maximum_lights,light_limit);s->trace_capacity=TraceExtent(profile.render_capacity,profile.quality_profile);
    s->native=ResolveRayDenoiser(profile.denoiser)!=NrdRadianceMethod::Off;
    // SIGMA consumes one distance sample per pixel. Internal filtering can use
    // the bounded higher-rpp quality estimator without inventing a SIGMA input.
    s->samples=s->native?1:profile.quality_profile.ray_shadow_samples;
    s->contexts.resize(s->maximum_lights);s->frames.resize(profile.frame_slots);
    for(auto format:{DXGI_FORMAT_R16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_UINT,
        DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R32_FLOAT})
    {
        if(s->native&&format==DXGI_FORMAT_R32G32B32A32_UINT)continue;
        if(s->native==false&&(format==DXGI_FORMAT_R16G16_FLOAT||format==DXGI_FORMAT_R10G10B10A2_UNORM||format==DXGI_FORMAT_R32_FLOAT))continue;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};auto hr=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
        if(FAILED(hr))return Native("Check ray shadow format",hr);
        if(!(support.Support1&D3D12_FORMAT_SUPPORT1_TEXTURE2D)||!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||
            !(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))return Failure(ErrorCode::TaskFailed,"Ray shadow format lacks native shader-load/UAV-store support.");
    }
    if(auto r=Texture(*s,s->raw,s->trace_capacity,DXGI_FORMAT_R16G16B16A16_FLOAT,s->maximum_lights);!r)return r;
    if(auto r=Texture(*s,s->visibility,profile.render_capacity,DXGI_FORMAT_R16_FLOAT,s->maximum_lights);!r)return r;
    if(auto r=Texture(*s,s->confidence,profile.render_capacity,DXGI_FORMAT_R16_FLOAT,s->maximum_lights);!r)return r;
    if(s->native)for(auto &context:s->contexts)
    {
        context.nrd=std::make_unique<NrdSystem>();NrdCandidate native;
        if(auto r=context.nrd->Prepare(device,allocator,s->trace_capacity,profile.frame_slots,reader,native,{NrdSignal::Shadow,NrdRadianceMethod::Reblur});!r)return r;
        s->allocated+=native.AllocatedBytes();if(s->allocated>profile.byte_budget)return Failure(ErrorCode::InvalidArgument,"Selected per-light SIGMA pools exceed byte_budget.");
        if(auto r=context.nrd->Commit(std::move(native),{});!r)return r;
        context.owner=std::make_shared<RayShadowStorage::ProducerOwner>();
        const std::pair<AllocationResource *,DXGI_FORMAT> outputs[]{{&context.motion,DXGI_FORMAT_R16G16_FLOAT},
            {&context.normal,DXGI_FORMAT_R10G10B10A2_UNORM},{&context.view_z,DXGI_FORMAT_R32_FLOAT},{&context.penumbra,DXGI_FORMAT_R16_FLOAT}};
        for(auto [resource,format]:outputs)
        {if(auto r=Texture(*s,*resource,s->trace_capacity,format);!r)return r;context.owner->allocations.push_back(resource->allocation);context.owner->resources.push_back(resource->resource);}
    }
    else for(UINT i=0;i<2;++i)
    {
        if(auto r=Texture(*s,s->history[i],s->trace_capacity,DXGI_FORMAT_R16G16B16A16_FLOAT,s->maximum_lights);!r)return r;
        if(auto r=Texture(*s,s->validation[i],s->trace_capacity,DXGI_FORMAT_R32G32B32A32_UINT,s->maximum_lights);!r)return r;
    }
    constexpr std::string_view products[]{"render/shader/ray_shadow_trace_cs.dxil","render/shader/ray_shadow_temporal_cs.dxil","render/shader/ray_shadow_resolve_cs.dxil"};
    for(UINT i=0;i<3;++i)
    {
        if(s->native&&i==1)continue;std::vector<std::byte> code;if(auto r=reader(products[i],code);!r)return r;
        if(code.empty())return Failure(ErrorCode::TaskFailed,"Empty ray shadow product: "+std::string(products[i]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root;desc.CS={code.data(),code.size()};
        auto hr=device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&s->pipelines[i]));if(FAILED(hr))return Native("Create ray shadow PSO",hr);
    }
    if(auto r=Allocate(*s,s->constants,Buffer(UINT64(profile.frame_slots)*s->maximum_lights*512),true,Access::ComputeRead);!r)return r;
    D3D12_RANGE empty{};void *mapped{};auto hr=s->constants.resource->Map(0,&empty,&mapped);if(FAILED(hr))return Native("Map ray shadow constants",hr);
    s->mapped=static_cast<std::byte *>(mapped);
    for(auto &frame:s->frames)
    {
        if(auto r=Allocate(*s,frame.upload,Buffer(sizeof(RayShadowMetadata)+UINT64(s->maximum_lights)*sizeof(GpuRayShadowRecord)),true);!r)return r;
        if(auto r=Allocate(*s,frame.metadata,Buffer(sizeof(RayShadowMetadata)));!r)return r;
        if(auto r=Allocate(*s,frame.records,Buffer(UINT64(s->maximum_lights)*sizeof(GpuRayShadowRecord)));!r)return r;
        hr=frame.upload.resource->Map(0,&empty,&mapped);if(FAILED(hr))return Native("Map ray shadow metadata",hr);frame.mapped=static_cast<std::byte *>(mapped);
        frame.binders.resize(s->maximum_lights);
    }
    s->stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s->allocated+=UINT64(profile.frame_slots)*(s->maximum_lights*kPacketDescriptors+kRayShadowExportCount)*s->stride+4*s->stride;
    if(s->allocated>profile.byte_budget)return Failure(ErrorCode::InvalidArgument,"Ray shadow native descriptor backing exceeds byte_budget.");
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=profile.frame_slots*s->maximum_lights*kPacketDescriptors;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->descriptors));if(FAILED(hr))return Native("Create ray shadow descriptors",hr);
    heap.NumDescriptors=profile.frame_slots*kRayShadowExportCount;hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->exports));
    if(FAILED(hr))return Native("Create ray shadow exports",hr);heap.NumDescriptors=2;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->clear_cpu_heap));if(FAILED(hr))return Native("Create ray shadow canonical clear descriptors",hr);
    auto clear=s->clear_cpu_heap->GetCPUDescriptorHandleForHeapStart();s->TextureUav(s->visibility.resource.Get(),DXGI_FORMAT_R16_FLOAT,clear,true);
    clear.ptr+=s->stride;s->TextureUav(s->confidence.resource.Get(),DXGI_FORMAT_R16_FLOAT,clear,true);
    heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->clear_heap));if(FAILED(hr))return Native("Create ray shadow clear descriptors",hr);
    device->CopyDescriptorsSimple(2,s->clear_heap->GetCPUDescriptorHandleForHeapStart(),
        s->clear_cpu_heap->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    candidate.storage_=std::move(s);candidate.prepared_=true;return Result::Success();
}
Result RayShadowSystem::Commit(RayShadowCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.prepared_||impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Ray shadow replacement requires a prepared candidate and every queue use completed.");
    if(impl_->storage)if(auto r=ResetNative(*impl_->storage,completed);!r)return r;
    impl_->storage=std::move(candidate.storage_);candidate.prepared_=false;impl_->status={};
    if(auto s=impl_->storage){impl_->status.enabled=true;impl_->status.native_sigma=s->native;impl_->status.allocated_bytes=s->allocated;
        impl_->status.rays_per_pixel=s->samples;impl_->status.availability=ReconstructionAvailability::Available;
        impl_->status.reason=s->native?"Per-light SIGMA shadow contexts prepared.":"Internal temporal/spatial shadow filter prepared; native SIGMA is disabled or compiled out.";}
    return Result::Success();
}
Result RayShadowSystem::Register(RenderGraphBuilder &graph,const RayShadowInputs &in,RayShadowOutputs &out)
{
    out={};auto s=impl_->storage;if(!s)return Result::Success();
    if(!s->usable||impl_->pending.graph)return Failure(ErrorCode::InvalidState,"Ray shadow service requires submission accounting/reset.");
    if(!Extent(in.render_active)||in.render_active.width>s->profile.render_capacity.width||in.render_active.height>s->profile.render_capacity.height||
        in.frame_slot>=s->frames.size()||!s->frames[in.frame_slot].completion.IsSatisfiedBy(in.completed)||
        !(Guide(graph,in.depth,in.render_active,DXGI_FORMAT_R32_TYPELESS)||Guide(graph,in.depth,in.render_active,DXGI_FORMAT_R32_FLOAT)||Guide(graph,in.depth,in.render_active,DXGI_FORMAT_D32_FLOAT))||
        !Guide(graph,in.normal,in.render_active,DXGI_FORMAT_R16G16_SNORM)||!Guide(graph,in.metadata,in.render_active,DXGI_FORMAT_R32G32_UINT)||
        !Guide(graph,in.motion,in.render_active,DXGI_FORMAT_R16G16_FLOAT)||!Guide(graph,in.surface_history,in.render_active,DXGI_FORMAT_R32G32_FLOAT)||
        in.camera.projection._34!=1.0f||in.camera.projection._44!=0.0f)
        return Failure(ErrorCode::InvalidArgument,"Ray shadow guides, perspective camera or completed frame slot are invalid.");
    RayShadowStatus status;status.enabled=true;status.native_sigma=s->native;status.allocated_bytes=s->allocated;status.rays_per_pixel=s->samples;
    status.availability=ReconstructionAvailability::Available;
    struct Ranked{const LightView *light;double priority;};std::vector<Ranked> ranked;
    for(const auto &light:in.lights)
    {
        if(light.type==LightType::Directional||(light.shadow_flags&kLightShadowRequested)==0)continue;
        ++status.requested_lights;if(!Supported(light)){++status.unsupported_lights;continue;}
        if(std::ranges::any_of(ranked,[&](const auto &r){return r.light->stable_id==light.stable_id&&r.light->generation==light.generation;}))
            return Failure(ErrorCode::InvalidArgument,"Duplicate full ray shadow light identity.");
        const auto old=std::ranges::find_if(s->contexts,[&](const auto &c){return c.occupied&&c.light.stable_id==light.stable_id&&c.light.generation==light.generation;});
        ranked.push_back({&light,Priority(light,in.camera.position,old==s->contexts.end()||!SameLight(old->light,light))});
    }
    std::ranges::sort(ranked,[](const auto &a,const auto &b){return a.priority!=b.priority?a.priority>b.priority:
        std::tie(a.light->stable_id,a.light->generation)<std::tie(b.light->stable_id,b.light->generation);});
    if(ranked.size()>s->maximum_lights){status.budget_lights=UINT(ranked.size()-s->maximum_lights);ranked.resize(s->maximum_lights);}
    status.selected_lights=UINT(ranked.size());
    if(!ranked.empty())
    {
        if(!in.make_bindings||!BlueNoise(graph,in.blue_noise)||!BufferVersion(graph,in.scene.tlas,in.scene.tlas_resource)||
            !BufferVersion(graph,in.scene.metadata,in.scene.metadata_resource)||!in.scene.instance_count||
            in.scene.tlas_address!=in.scene.tlas_resource->GetGPUVirtualAddress()||
            !BufferVersion(graph,in.geometry.records,in.geometry.record_resource)||!BufferVersion(graph,in.geometry.instances,in.geometry.instance_resource)||
            !in.geometry.geometry_count||in.geometry.geometry_count!=in.geometry.geometries.size()||
            in.scene.metadata_resource->GetDesc().Width<UINT64(in.scene.instance_count)*32||
            in.geometry.record_resource->GetDesc().Width<UINT64(in.geometry.geometry_count)*80||
            in.geometry.instance_resource->GetDesc().Width<UINT64(in.geometry.geometry_count)*48)
            return Failure(ErrorCode::InvalidArgument,"Selective shadows require the full shared TLAS, exact ray geometry, native bindings and resident64x64x64 STBN.");
        for(const auto &r:in.reads)if(!r.resource||!r.version.resource||graph.Resolve(r.version.resource)!=r.resource||
            (r.access!=Access::ComputeRead&&r.access!=Access::ShaderRead&&r.access!=Access::AllShaderRead))
            return Failure(ErrorCode::InvalidArgument,"Ray shadow canonical geometry/material graph read invalid.");
    }
    std::vector<UINT> assignments(ranked.size(),UINT_MAX);std::vector<bool> assigned(s->maximum_lights);
    for(UINT i=0;i<ranked.size();++i)for(UINT slot=0;slot<s->maximum_lights;++slot)
        if(s->contexts[slot].occupied&&s->contexts[slot].light.stable_id==ranked[i].light->stable_id&&s->contexts[slot].light.generation==ranked[i].light->generation)
        {assignments[i]=slot;assigned[slot]=true;break;}
    for(UINT i=0;i<ranked.size();++i)if(assignments[i]==UINT_MAX)
        for(UINT slot=0;slot<s->maximum_lights;++slot)if(!assigned[slot]){assignments[i]=slot;assigned[slot]=true;break;}
    const auto active=TraceExtent(in.render_active,s->profile.quality_profile);const UINT write=1-s->read_index;
    auto &frame=s->frames[in.frame_slot];frame.reads.clear();frame.owners=in.owners;
    for(auto &binder:frame.binders)binder={};
    std::vector<GpuRayShadowRecord> records(s->maximum_lights);
    RayShadowMetadata metadata;metadata.world_to_clip=in.camera.current_unjittered_view_projection;metadata.render_size=Size(in.render_active);
    metadata.jitter_uv={in.camera.jitter_pixels[0]/float(in.render_active.width),in.camera.jitter_pixels[1]/float(in.render_active.height),0,0};
    metadata.counts={UINT(ranked.size()),1,std::bit_cast<UINT>(in.camera.projection._33),std::bit_cast<UINT>(in.camera.projection._43)};
    auto &pending=impl_->pending;pending={};pending.graph=&graph;pending.frame=in.frame_slot;pending.write_index=write;pending.active=active;
    pending.status=status;pending.contexts.reserve(ranked.size());
    auto import=[&](AllocationResource &r,std::string_view name,bool texture=true)
    {const auto handle=texture?graph.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name):
        graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name);pending.imports.push_back({&r,handle});return graph.Version(handle);};
    auto raw=import(s->raw,"RayShadow.Raw"),visibility=import(s->visibility,"RayShadow.Visibility"),confidence=import(s->confidence,"RayShadow.Confidence");
    auto upload=import(frame.upload,"RayShadow.MetadataUpload",false),meta=import(frame.metadata,"RayShadow.Metadata",false),record=import(frame.records,"RayShadow.Records",false);
    auto constants_version=import(s->constants,"RayShadow.Constants",false);
    ResourceVersion previous,previous_validation,history,validation;
    if(!s->native){previous=import(s->history[s->read_index],"RayShadow.Previous");previous_validation=import(s->validation[s->read_index],"RayShadow.PreviousValidation");
        history=import(s->history[write],"RayShadow.Current");validation=import(s->validation[write],"RayShadow.CurrentValidation");}
    auto clear=graph.AddPass(242,"RayShadow.InitializeOpaqueOutputs",QueueHint::Direct);
    clear.Read(upload,Access::CopySource);meta=clear.Write(meta,Access::CopyDest);record=clear.Write(record,Access::CopyDest);
    visibility=clear.Write(visibility,Access::UnorderedWrite);confidence=clear.Write(confidence,Access::UnorderedWrite);
    clear.SetExecute([s,slot=in.frame_slot,v=visibility.resource,c=confidence.resource](RenderPassContext &context)
    {
        auto *cmd=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());auto &f=s->frames[slot];
        cmd->CopyBufferRegion(f.metadata.resource.Get(),0,f.upload.resource.Get(),0,sizeof(RayShadowMetadata));
        cmd->CopyBufferRegion(f.records.resource.Get(),0,f.upload.resource.Get(),sizeof(RayShadowMetadata),f.records.resource->GetDesc().Width);
        ID3D12DescriptorHeap *heaps[]{s->clear_heap.Get()};cmd->SetDescriptorHeaps(1,heaps);
        auto cpu=s->clear_cpu_heap->GetCPUDescriptorHandleForHeapStart();auto gpu=s->clear_heap->GetGPUDescriptorHandleForHeapStart();
        const float visible[4]{1,1,1,1},zero[4]{};cmd->ClearUnorderedAccessViewFloat(gpu,cpu,s->visibility.resource.Get(),visible,0,nullptr);
        cpu.ptr+=s->stride;gpu.ptr+=s->stride;cmd->ClearUnorderedAccessViewFloat(gpu,cpu,s->confidence.resource.Get(),zero,0,nullptr);
        context.UavBarrier(v);context.UavBarrier(c);
    });
    for(UINT selected=0;selected<ranked.size();++selected)
    {
        const auto slot=assignments[selected];auto &context=s->contexts[slot];const auto &light=*ranked[selected].light;
        const bool valid=context.history_valid&&context.occupied&&SameLight(context.light,light)&&!in.reset_history&&!in.camera.reset_history&&
            context.previous_active.width==active.width&&context.previous_active.height==active.height;
        RayShadowConstants constants;constants.render_size=Size(in.render_active);constants.trace_size=Size(active);
        constants.previous_trace_size=Size(valid?context.previous_active:active);constants.world_to_view=in.camera.view;constants.projection=in.camera.projection;
        using namespace DirectX;
        const auto relative_view=XMMatrixTranslation(in.camera.position.x,in.camera.position.y,in.camera.position.z)*XMLoadFloat4x4(&in.camera.view);
        auto jittered_projection=XMLoadFloat4x4(&in.camera.projection);XMFLOAT4X4 jittered;XMStoreFloat4x4(&jittered,jittered_projection);
        jittered._31+=2.0f*metadata.jitter_uv.x;jittered._32-=2.0f*metadata.jitter_uv.y;
        XMStoreFloat4x4(&constants.inverse_view_projection,XMMatrixInverse(nullptr,relative_view*XMLoadFloat4x4(&jittered)));
        const auto &prior=valid?in.camera.previous_projection:in.camera.projection;
        constants.previous_depth_parameters={prior._33,prior._34,prior._43,prior._44};constants.jitter_uv=metadata.jitter_uv;
        constants.world_origin={in.camera.position.x,in.camera.position.y,in.camera.position.z,0};
        constants.light_position_range={light.position.x,light.position.y,light.position.z,light.range};
        const auto toward=light.type==LightType::Spot||Area(light)?XMVector3Normalize(XMVectorSet(light.direction.x,light.direction.y,light.direction.z,0)):XMVectorSet(0,0,1,0);
        XMStoreFloat4(&constants.light_direction_cos,toward);
        constants.light_direction_cos.w=Area(light)?(light.two_sided?1.0f:0.0f):std::cos(light.outer_cone_radians);
        if(Area(light))
        {
            // Use the authored plane, including a non-unit tangent, just as
            // LightList does. Double precision preserves exact degeneracy.
            const auto &d=light.direction;const auto &t=light.area_tangent;
            const double bx=double(d.y)*t.z-double(d.z)*t.y,by=double(d.z)*t.x-double(d.x)*t.z,
                bz=double(d.x)*t.y-double(d.y)*t.x,length=std::hypot(bx,by,bz);
            const double ux=bx/length,uy=by/length,uz=bz/length;
            const double nl=std::hypot(double(d.x),double(d.y),double(d.z));
            const double nx=d.x/nl,ny=d.y/nl,nz=d.z/nl;
            const float scale=light.type==LightType::Rectangle?0.5f:1.0f;
            constants.area_tangent_half_width={float(uy*nz-uz*ny),float(uz*nx-ux*nz),float(ux*ny-uy*nx),light.shape.x*scale};
            constants.area_bitangent_half_height={float(ux),float(uy),float(uz),light.shape.y*scale};
        }
        constants.emitter={EmitterBound(light),0.002f,2.0f/(float(in.render_active.height)*std::max(std::abs(in.camera.projection._22),1e-6f)),1000.0f};
        constants.identity={UINT(light.stable_id),UINT(light.stable_id>>32),light.generation,UINT(light.type)};
        constants.frame={UINT(in.identity.render_id),UINT(in.identity.render_id>>32),UINT(s->native),UINT(!valid)};
        constants.control={UINT(valid),slot,in.scene.instance_count,in.geometry.geometry_count};
        constants.budget={s->samples,s->profile.quality_profile.ray_shadow_spatial_radius,5,
            std::bit_cast<UINT>(s->profile.maximum_trace_distance_m)};
        const UINT64 constant_offset=UINT64(in.frame_slot*s->maximum_lights+slot)*512;
        std::memcpy(s->mapped+constant_offset,&constants,sizeof(constants));
        GpuRayShadowRecord gpu_record;gpu_record.identity=constants.identity;
        gpu_record.revision={UINT(light.revision),UINT(light.revision>>32),0,0};gpu_record.position_range=constants.light_position_range;
        gpu_record.direction_cos_outer=constants.light_direction_cos;gpu_record.emitter=constants.emitter;gpu_record.state={slot,1,UINT(s->native),0};records[selected]=gpu_record;
        pending.contexts.push_back({slot,light,false});
        auto &native_pending=pending.contexts.back();
        s->device->CreateConstantBufferView(nullptr,s->Cpu(in.frame_slot,slot,0));
        D3D12_SHADER_RESOURCE_VIEW_DESC as{};as.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        as.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;as.RaytracingAccelerationStructure.Location=in.scene.tlas_address;
        s->device->CreateShaderResourceView(nullptr,&as,s->Cpu(in.frame_slot,slot,1));
        s->Structured(in.scene.metadata_resource,in.scene.instance_count,32,s->Cpu(in.frame_slot,slot,2));
        s->Structured(in.geometry.record_resource,in.geometry.geometry_count,80,s->Cpu(in.frame_slot,slot,3));
        s->Structured(in.geometry.record_resource,in.geometry.geometry_count,80,s->Cpu(in.frame_slot,slot,4));
        s->Raw(in.geometry.position_resource,s->Cpu(in.frame_slot,slot,5));
        for(UINT i=6;i<15;++i)s->TextureSrv(nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT,s->Cpu(in.frame_slot,slot,i));
        s->Structured(in.geometry.instance_resource,in.geometry.geometry_count,48,s->Cpu(in.frame_slot,slot,15));
        D3D12_UNORDERED_ACCESS_VIEW_DESC null{};null.Format=DXGI_FORMAT_R32_TYPELESS;null.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        null.Buffer.NumElements=1;null.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        for(UINT i=16;i<21;++i)s->device->CreateUnorderedAccessView(nullptr,nullptr,&null,s->Cpu(in.frame_slot,slot,i));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{s->constants.resource->GetGPUVirtualAddress()+constant_offset,512};
        s->device->CreateConstantBufferView(&cbv,s->Own(in.frame_slot,slot,0));
        s->Raw(frame.metadata.resource.Get(),s->Own(in.frame_slot,slot,1));s->Structured(frame.records.resource.Get(),s->maximum_lights,96,s->Own(in.frame_slot,slot,2));
        s->TextureSrv(s->visibility.resource.Get(),DXGI_FORMAT_R16_FLOAT,s->Own(in.frame_slot,slot,3),true);
        s->TextureSrv(s->confidence.resource.Get(),DXGI_FORMAT_R16_FLOAT,s->Own(in.frame_slot,slot,4),true);
        s->TextureSrv(in.depth.resource,DXGI_FORMAT_R32_FLOAT,s->Own(in.frame_slot,slot,5));s->TextureSrv(in.normal.resource,DXGI_FORMAT_R16G16_SNORM,s->Own(in.frame_slot,slot,6));
        s->TextureSrv(in.motion.resource,DXGI_FORMAT_R16G16_FLOAT,s->Own(in.frame_slot,slot,7));s->TextureSrv(in.metadata.resource,DXGI_FORMAT_R32G32_UINT,s->Own(in.frame_slot,slot,8));
        s->TextureSrv(in.surface_history.resource,DXGI_FORMAT_R32G32_FLOAT,s->Own(in.frame_slot,slot,9));s->TextureSrv(s->raw.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,s->Own(in.frame_slot,slot,10),true);
        s->TextureSrv(s->native?nullptr:s->history[s->read_index].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,s->Own(in.frame_slot,slot,11),true);
        s->TextureSrv(s->native?nullptr:s->validation[s->read_index].resource.Get(),DXGI_FORMAT_R32G32B32A32_UINT,s->Own(in.frame_slot,slot,12),true);
        s->TextureSrv(s->native?context.nrd->PreparedOutput(NrdSignal::Shadow):nullptr,DXGI_FORMAT_R16_FLOAT,s->Own(in.frame_slot,slot,13));
        s->TextureSrv(s->native?nullptr:s->history[write].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,s->Own(in.frame_slot,slot,14),true);
        s->TextureUav(s->raw.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,s->Own(in.frame_slot,slot,15),true);
        s->TextureUav(context.motion.resource.Get(),DXGI_FORMAT_R16G16_FLOAT,s->Own(in.frame_slot,slot,16));
        s->TextureUav(context.normal.resource.Get(),s->native?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT,s->Own(in.frame_slot,slot,17));
        s->TextureUav(context.view_z.resource.Get(),DXGI_FORMAT_R32_FLOAT,s->Own(in.frame_slot,slot,18));
        s->TextureUav(context.penumbra.resource.Get(),DXGI_FORMAT_R16_FLOAT,s->Own(in.frame_slot,slot,19));
        s->TextureUav(s->native?nullptr:s->history[write].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,s->Own(in.frame_slot,slot,20),true);
        s->TextureUav(s->native?nullptr:s->validation[write].resource.Get(),DXGI_FORMAT_R32G32B32A32_UINT,s->Own(in.frame_slot,slot,21),true);
        s->TextureUav(s->visibility.resource.Get(),DXGI_FORMAT_R16_FLOAT,s->Own(in.frame_slot,slot,22),true);
        s->TextureUav(s->confidence.resource.Get(),DXGI_FORMAT_R16_FLOAT,s->Own(in.frame_slot,slot,23),true);
        D3D12_SHADER_RESOURCE_VIEW_DESC noise{};noise.Format=DXGI_FORMAT_R8_UNORM;noise.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        noise.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;noise.Texture2DArray.MipLevels=1;noise.Texture2DArray.ArraySize=64;
        s->device->CreateShaderResourceView(in.blue_noise.resource,&noise,s->Own(in.frame_slot,slot,24));
        if(auto r=in.make_bindings(s->Cpu(in.frame_slot,slot,0),s->Own(in.frame_slot,slot,0),frame.binders[slot]);!r)return r;
        if(!frame.binders[slot])return Failure(ErrorCode::InvalidArgument,"Ray shadow frame-table builder returned no native compute binder.");
        const auto binder=frame.binders[slot];
        const auto execute=[s,binder](UINT pipeline,ViewExtent extent)
        {return [s,binder,pipeline,extent](RenderPassContext &context)->Result
            {auto *cmd=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[pipeline].Get());
             if(auto r=binder(cmd);!r)return r;const UINT pass[2]{};cmd->SetComputeRoot32BitConstants(6,2,pass,0);
             cmd->Dispatch((extent.width+7)/8,(extent.height+7)/8,1);return Result::Success();};};
        const auto guides=[&](PassBuilder &pass){for(auto v:{in.depth.version,in.normal.version,in.metadata.version,constants_version})pass.Read(v,Access::ComputeRead);};
        auto trace=graph.AddPass(230+slot*3,"RayShadow.TraceFiniteEmitter",QueueHint::Direct);guides(trace);
        trace.Read(in.scene.tlas,Access::ASRead);trace.Read(in.scene.metadata,Access::ComputeRead);trace.Read(in.geometry.records,Access::ComputeRead);
        trace.Read(in.geometry.instances,Access::ComputeRead);trace.Read(in.blue_noise.version,Access::ComputeRead);
        for(const auto &r:in.reads)trace.Read(r.version,r.access);
        raw=trace.Write(raw,Access::UnorderedWrite,Layer(slot));
        ResourceVersion native_clean;
        if(s->native)
        {
            trace.Read(in.motion.version,Access::ComputeRead);
            auto motion=import(context.motion,"RayShadow.NRD.Motion"),normal=import(context.normal,"RayShadow.NRD.NormalRoughness"),
                view_z=import(context.view_z,"RayShadow.NRD.LinearViewZ"),penumbra=import(context.penumbra,"RayShadow.NRD.Penumbra");
            motion=trace.Write(motion,Access::UnorderedWrite);normal=trace.Write(normal,Access::UnorderedWrite);
            view_z=trace.Write(view_z,Access::UnorderedWrite);penumbra=trace.Write(penumbra,Access::UnorderedWrite);
            NrdNativeInputs native;native.motion={motion,context.motion.resource.Get()};native.normal_roughness={normal,context.normal.resource.Get()};
            native.view_z={view_z,context.view_z.resource.Get()};native.shadow_penumbra={penumbra,context.penumbra.resource.Get()};
            native.native_geometry_packed=true;native.sigma_penumbra_packed=true;native.camera=in.camera;
            native.camera.jitter_pixels[0]*=float(active.width)/in.render_active.width;native.camera.jitter_pixels[1]*=float(active.height)/in.render_active.height;
            native.camera.reset_history|=!valid;native.identity=in.identity;native.active=active;native.frame_slot=in.frame_slot;
            native.first_pass_id=10000+slot*1000;native.completed=in.completed;native.owners.push_back(context.owner);
            NrdSettings settings;settings.shadow=true;settings.shadow_history_frames=5;settings.denoising_range_m=constants.emitter.w;
            NrdNativeOutputs output;if(auto r=context.nrd->Register(graph,native,settings,output);!r)return r;
            native_pending.native_registered=true;native_clean=output.shadow.version;
        }
        trace.SetExecuteChecked(execute(0,active));
        if(!s->native)
        {
            auto temporal=graph.AddPass(231+slot*3,"RayShadow.InternalTemporal",QueueHint::Direct);guides(temporal);
            temporal.Read(raw,Access::ComputeRead,Layer(slot));temporal.Read(in.motion.version,Access::ComputeRead);temporal.Read(in.surface_history.version,Access::ComputeRead);
            if(valid){temporal.Read(previous,Access::ComputeRead,Layer(slot));temporal.Read(previous_validation,Access::ComputeRead,Layer(slot));}
            history=temporal.Write(history,Access::UnorderedWrite,Layer(slot));validation=temporal.Write(validation,Access::UnorderedWrite,Layer(slot));temporal.SetExecuteChecked(execute(1,active));
        }
        auto resolve=graph.AddPass(232+slot*3,"RayShadow.OpaqueVisibilityResolve",QueueHint::Direct);guides(resolve);resolve.Read(raw,Access::ComputeRead,Layer(slot));
        if(s->native)resolve.Read(native_clean,Access::ComputeRead);else resolve.Read(history,Access::ComputeRead,Layer(slot));
        visibility=resolve.ReadWrite(visibility,Access::UnorderedWrite,Layer(slot));confidence=resolve.ReadWrite(confidence,Access::UnorderedWrite,Layer(slot));
        resolve.SetExecuteChecked(execute(2,in.render_active));
    }
    std::memcpy(frame.mapped,&metadata,sizeof(metadata));std::memcpy(frame.mapped+sizeof(metadata),records.data(),records.size()*sizeof(records[0]));
    auto ready=graph.AddPass(243,"RayShadow.OpaqueVisibilityReady",QueueHint::Direct);
    for(auto v:{meta,record,visibility,confidence,in.depth.version,in.normal.version})ready.Read(v,Access::AllShaderRead);
    ready.SetExecute([](RenderPassContext &){});
    s->Raw(frame.metadata.resource.Get(),s->Export(in.frame_slot,0));s->Structured(frame.records.resource.Get(),s->maximum_lights,96,s->Export(in.frame_slot,1));
    s->TextureSrv(s->visibility.resource.Get(),DXGI_FORMAT_R16_FLOAT,s->Export(in.frame_slot,2),true);s->TextureSrv(s->confidence.resource.Get(),DXGI_FORMAT_R16_FLOAT,s->Export(in.frame_slot,3),true);
    s->TextureSrv(in.depth.resource,DXGI_FORMAT_R32_FLOAT,s->Export(in.frame_slot,4));s->TextureSrv(in.normal.resource,DXGI_FORMAT_R16G16_SNORM,s->Export(in.frame_slot,5));
    out.metadata=meta;out.records=record;out.visibility=visibility;out.confidence=confidence;out.depth=in.depth.version;out.normal=in.normal.version;
    out.resources={frame.metadata.resource.Get(),frame.records.resource.Get(),s->visibility.resource.Get(),s->confidence.resource.Get(),in.depth.resource,in.normal.resource};
    for(UINT i=0;i<kRayShadowExportCount;++i)out.srvs[i]=s->Export(in.frame_slot,i);out.descriptor_count=kRayShadowExportCount;
    for(const auto &r:in.reads)frame.reads.emplace_back(r.resource);
    for(auto *r:{in.scene.tlas_resource,in.scene.metadata_resource,in.geometry.record_resource,in.geometry.instance_resource,
        in.geometry.position_resource,in.depth.resource,in.normal.resource,in.metadata.resource,in.motion.resource,in.surface_history.resource,in.blue_noise.resource})
        if(r)frame.reads.emplace_back(r);
    pending.status.reason=s->native?"Per-light SIGMA denoises actual finite-emitter occluder-distance penumbra.":
        "Internal temporal/spatial shadow filter; native SIGMA disabled or compiled out.";
    if(status.unsupported_lights)pending.status.reason+=" Invalid or unsupported light domains retain their original world-shadow path.";
    if(status.budget_lights)pending.status.reason+=" Unselected lights retain their raster shadow alternative.";
    return Result::Success();
}
Result RayShadowSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;if(!p.graph)return Result::Success();if(p.graph!=&graph)return Failure(ErrorCode::InvalidArgument,"Wrong ray shadow completion graph.");
    auto s=impl_->storage;auto &frame=s->frames[p.frame];s->completion.Merge(completion);frame.completion.Merge(completion);
    Result native_result=Result::Success();
    for(auto &c:p.contexts)if(c.native_registered)
    {auto r=s->contexts[c.slot].nrd->CompleteSubmission(graph,completion,success);if(!r){native_result=r;s->usable=false;}}
    for(auto &i:p.imports)
    {const auto lease=graph.ResourceCompletion(i.handle);s->completion.Merge(lease);frame.completion.Merge(lease);
     i.resource->graph_completion.Merge(completion);i.resource->graph_completion.Merge(lease);
     if(success){i.resource->graph_access=graph.FinalAccess(i.handle);i.resource->graph_queue=graph.FinalQueue(i.handle);}}
    for(auto &c:s->contexts)c.selected=false;
    if(success&&native_result)
    {
        for(const auto &pending:p.contexts){auto &c=s->contexts[pending.slot];c.light=pending.light;c.occupied=true;c.selected=true;c.history_valid=true;c.previous_active=p.active;}
        for(auto &c:s->contexts)if(!c.selected)c.history_valid=false;
        s->read_index=p.write_index;s->initialized=true;impl_->status=std::move(p.status);
    }
    else{s->usable=false;impl_->status.availability=ReconstructionAvailability::Unavailable;
        impl_->status.reason="Ray shadow partial/native submission failure requires all queues drained and fresh preparation.";}
    p={};return native_result;
}
void RayShadowSystem::ResetHistory()noexcept{if(auto s=impl_->storage)for(auto &c:s->contexts)c.history_valid=false;}
Result RayShadowSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Ray shadow reset requires every Direct/Compute/Copy use completed.");
    if(impl_->storage)if(auto r=ResetNative(*impl_->storage,completed);!r)return r;
    impl_->storage.reset();impl_->status={};return Result::Success();
}
} // namespace hs
