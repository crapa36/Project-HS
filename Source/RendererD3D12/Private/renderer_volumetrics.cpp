#include "renderer_volumetrics.hpp"
#include "renderer_pipeline_cache.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
constexpr UINT kConstantStride=768;
constexpr DXGI_FORMAT kSignal=DXGI_FORMAT_R32G32B32A32_FLOAT,kScalar=DXGI_FORMAT_R32_FLOAT,kHdr=DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr std::string_view kProducts[]{
    "render/shader/volumetric_density_cs.dxil","render/shader/volumetric_lighting_cs.dxil",
    "render/shader/volumetric_temporal_cs.dxil","render/shader/volumetric_spatial_cs.dxil",
    "render/shader/volumetric_integrate_cs.dxil","render/shader/volumetric_resolve_cs.dxil"};
Result Fail(ErrorCode code,std::string message)
{return Result::Failure(code,"hs_renderer_volumetrics",std::move(message));}
Result Native(std::string_view operation,HRESULT hr)
{return Fail(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+std::to_string(std::int32_t(hr))+").");}
bool Extent(ViewExtent extent)
{return extent.width&&extent.height&&extent.width<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION&&extent.height<=D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;}
struct Budget { UINT stride{},slices{},area_samples{}; };
ViewExtent FroxelExtent(ViewExtent render,UINT stride)
{return {(render.width+stride-1)/stride,(render.height+stride-1)/stride};}
DirectX::XMFLOAT4 Size(ViewExtent e)
{return {float(e.width),float(e.height),1.0f/float(e.width),1.0f/float(e.height)};}
bool Finite(const DirectX::XMFLOAT4X4 &m)
{for(const auto &row:m.m)for(float f:row)if(!std::isfinite(f))return false;return true;}
bool Finite(const DirectX::XMFLOAT4 &v)
{return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&std::isfinite(v.w);}
bool Camera(const VolumetricCamera &camera)
{
    if(!Finite(camera.inverse_view_projection)||!Finite(camera.view_projection)||!Finite(camera.world_to_view)||
       !std::isfinite(camera.origin.x)||!std::isfinite(camera.origin.y)||!std::isfinite(camera.origin.z)||
       !std::isfinite(camera.near_plane)||!std::isfinite(camera.far_plane)||camera.near_plane<=0||camera.far_plane<=camera.near_plane)
        return false;
    // The reconstruction uses clip-Z1 and0.5 and supports perspective and
    // orthographic projections. Reject singular/misdeclared camera transforms.
    const auto inverse=DirectX::XMLoadFloat4x4(&camera.inverse_view_projection);
    const auto view=DirectX::XMLoadFloat4x4(&camera.world_to_view);
    for(const auto &uv:std::array<DirectX::XMFLOAT2,5>{{{0,0},{1,0},{0,1},{1,1},{0.5f,0.5f}}})
    {
        DirectX::XMFLOAT4 a{},b{};
        DirectX::XMStoreFloat4(&a,DirectX::XMVector4Transform(DirectX::XMVectorSet(uv.x*2-1,1-uv.y*2,1,1),inverse));
        DirectX::XMStoreFloat4(&b,DirectX::XMVector4Transform(DirectX::XMVectorSet(uv.x*2-1,1-uv.y*2,0.5f,1),inverse));
        if(!Finite(a)||!Finite(b)||a.w==0||b.w==0)return false;
        const auto va=DirectX::XMVector4Transform(DirectX::XMVectorSet(a.x/a.w,a.y/a.w,a.z/a.w,0),view);
        const auto vb=DirectX::XMVector4Transform(DirectX::XMVectorSet(b.x/b.w,b.y/b.w,b.z/b.w,0),view);
        const float za=DirectX::XMVectorGetZ(va),zb=DirectX::XMVectorGetZ(vb);
        if(!std::isfinite(za)||!std::isfinite(zb)||za<=0||zb<=0||za==zb)return false;
    }
    return true;
}
bool SameCamera(const VolumetricCamera &a,const VolumetricCamera &b)
{
    return std::memcmp(&a.inverse_view_projection,&b.inverse_view_projection,sizeof(a.inverse_view_projection))==0&&
        std::memcmp(&a.view_projection,&b.view_projection,sizeof(a.view_projection))==0&&
        std::memcmp(&a.world_to_view,&b.world_to_view,sizeof(a.world_to_view))==0&&
        a.origin.x==b.origin.x&&a.origin.y==b.origin.y&&a.origin.z==b.origin.z&&
        a.near_plane==b.near_plane&&a.far_plane==b.far_plane;
}
bool TextureRead(const RenderGraphBuilder &graph,const VolumetricGuide &input,ViewExtent active,DXGI_FORMAT format)
{
    if(!input.resource||!input.version.resource||input.version.resource.kind!=ResourceKind::Texture||graph.Resolve(input.version.resource)!=input.resource)
        return false;
    const auto d=input.resource->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width>=active.width&&d.Height>=active.height&&
        d.DepthOrArraySize==1&&d.SampleDesc.Count==1&&d.Format==format;
}
bool DepthRead(const RenderGraphBuilder &graph,const VolumetricGuide &input,ViewExtent active)
{return TextureRead(graph,input,active,DXGI_FORMAT_R32_TYPELESS)||TextureRead(graph,input,active,kScalar)||TextureRead(graph,input,active,DXGI_FORMAT_D32_FLOAT);}
bool ResourceRead(const RenderGraphBuilder &graph,ResourceVersion version,ID3D12Resource *resource)
{return resource&&version.resource&&graph.Resolve(version.resource)==resource;}
bool BlueNoiseRead(const RenderGraphBuilder &graph,const VolumetricGuide &input)
{
    if(!ResourceRead(graph,input.version,input.resource)||input.version.resource.kind!=ResourceKind::Texture)return false;
    const auto d=input.resource->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Format==DXGI_FORMAT_R8_UNORM&&
        d.Width==64&&d.Height==64&&d.DepthOrArraySize==64&&d.MipLevels==1&&d.SampleDesc.Count==1;
}
bool HeroNoiseVolumeRead(const RenderGraphBuilder &graph,const VolumetricGuide &input)
{
    if(!ResourceRead(graph,input.version,input.resource)||input.version.resource.kind!=ResourceKind::Texture)return false;
    const auto d=input.resource->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D&&d.Format==DXGI_FORMAT_R8G8B8A8_UNORM&&
        d.Width==64&&d.Height==64&&d.DepthOrArraySize==64&&d.MipLevels==1&&d.SampleDesc.Count==1;
}
bool GradientRead(const RenderGraphBuilder &graph,const VolumetricGuide &input)
{
    if(!ResourceRead(graph,input.version,input.resource)||input.version.resource.kind!=ResourceKind::Texture)return false;
    const auto d=input.resource->GetDesc();
    return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Format==kHdr&&d.Width==256&&d.Height>0&&
        d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1;
}
auto HeroKey(const VfxVolumeIdentity &i)
{return std::tie(i.effect_id,i.source_id,i.source_index,i.output_index,i.effect_handle,i.event_sequence,i.owner_id,i.episode_generation);}
bool Affine(const DirectX::XMFLOAT4X4 &m)
{
    const float determinant=DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&m)));
    return Finite(m)&&std::isfinite(determinant)&&determinant!=0&&m._14==0&&m._24==0&&m._34==0&&m._44==1;
}
struct HeroState
{
    VfxVolumeIdentity identity;
    VolumetricHeroVolume gpu;
    std::array<float,16> local_to_world;
    std::array<float,6> bounds;
    std::array<std::uint64_t,3> revisions;
    bool operator==(const HeroState &other)const
    {
        return identity==other.identity&&std::memcmp(&gpu,&other.gpu,sizeof(gpu))==0&&
            local_to_world==other.local_to_world&&bounds==other.bounds&&revisions==other.revisions;
    }
};
Result HeroRecords(VfxVolumeInputs inputs,std::vector<HeroState> &states)
{
    states.reserve(inputs.size());
    for(const auto &input:inputs)
    {
        const auto &id=input.identity;HeroState state{};state.identity=id;
        std::memcpy(&state.gpu.world_to_local,input.world_to_local.data(),sizeof(state.gpu.world_to_local));
        DirectX::XMFLOAT4X4 localToWorld;std::memcpy(&localToWorld,input.local_to_world.data(),sizeof(localToWorld));
        const auto &albedo=input.scattering_albedo;const auto &emission=input.emission;
        const bool crown=input.shape==VfxVolumeShape::BoundedCrown;
        if(!id.effect_id||!id.effect_handle||!id.source_id||bool(id.owner_id)==bool(id.event_sequence)||
           !Affine(state.gpu.world_to_local)||!Affine(localToWorld)||input.minimum_quality!=2||
           input.steps!=(crown?18u:12u)||std::uint32_t(input.shape)>std::uint32_t(VfxVolumeShape::BoundedCrown)||
           input.noise!=(crown?VfxVolumeNoise::None:VfxVolumeNoise::Fbm3D)||input.half_resolution>1||
           !std::isfinite(input.elapsed_seconds)||input.elapsed_seconds<0||!std::isfinite(input.source_age)||input.source_age<0||input.source_age>1||
           !std::isfinite(input.authored_scale)||input.authored_scale<=0||!std::isfinite(input.hdr)||input.hdr<0||
           !std::isfinite(input.source_envelope)||input.source_envelope<0||input.source_envelope>1||
           !std::isfinite(input.density)||input.density<0||!std::isfinite(input.alpha)||input.alpha<0||input.alpha>1||
           !std::isfinite(input.extinction)||input.extinction<0||!std::isfinite(input.anisotropy)||std::abs(input.anisotropy)>=1||
           !Finite({albedo.x,albedo.y,albedo.z,0})||albedo.x<0||albedo.y<0||albedo.z<0||albedo.x>1||albedo.y>1||albedo.z>1||
           !Finite({emission.x,emission.y,emission.z,0})||emission.x<0||emission.y<0||emission.z<0)
            return Fail(ErrorCode::InvalidArgument,"Hero volume requires its exact event/owner identity, authored shape/sample budget, finite affine bounds and valid optical/appearance coefficients.");
        const float density=input.density*input.alpha*input.source_envelope;
        if(!std::isfinite(density)||!std::isfinite(density*input.extinction)||
           !std::isfinite(density*emission.x)||!std::isfinite(density*emission.y)||!std::isfinite(density*emission.z))
            return Fail(ErrorCode::InvalidArgument,"Hero volume density or raw HDR source overflowed.");
        DirectX::XMFLOAT4X4 product;
        DirectX::XMStoreFloat4x4(&product,DirectX::XMLoadFloat4x4(&localToWorld)*DirectX::XMLoadFloat4x4(&state.gpu.world_to_local));
        for(UINT row=0;row<4;++row)for(UINT col=0;col<4;++col)
            if(!std::isfinite(product.m[row][col])||std::abs(product.m[row][col]-(row==col?1.0f:0.0f))>0.001f)
                return Fail(ErrorCode::InvalidArgument,"Hero volume local/world matrices are not actual mutual inverses.");
        state.bounds={input.bounds_min.x,input.bounds_min.y,input.bounds_min.z,input.bounds_max.x,input.bounds_max.y,input.bounds_max.z};
        for(UINT axis=0;axis<3;++axis)
        {
            const float extent=std::abs(localToWorld.m[0][axis])+std::abs(localToWorld.m[1][axis])+std::abs(localToWorld.m[2][axis]);
            const float minimum=localToWorld.m[3][axis]-extent,maximum=localToWorld.m[3][axis]+extent;
            if(!std::isfinite(state.bounds[axis])||!std::isfinite(state.bounds[axis+3])||state.bounds[axis]>=state.bounds[axis+3]||
               std::abs(state.bounds[axis]-minimum)>0.001f*std::max(1.0f,extent)||
               std::abs(state.bounds[axis+3]-maximum)>0.001f*std::max(1.0f,extent))
                return Fail(ErrorCode::InvalidArgument,"Hero shadow receiver bounds do not match the actual transformed canonical shape.");
        }
        state.gpu.optical={density,input.extinction,input.anisotropy,input.elapsed_seconds};
        state.gpu.albedo={albedo.x,albedo.y,albedo.z,input.source_age};
        state.gpu.emission={emission.x,emission.y,emission.z,float(input.gradient_row)};
        state.gpu.sampling={input.steps,UINT(input.noise),input.stable_seed,UINT(input.shape)};
        state.gpu.effect_source={UINT(id.effect_id),UINT(id.effect_id>>32),id.source_id,id.source_index};
        state.gpu.owner_event={UINT(id.owner_id),UINT(id.owner_id>>32),UINT(id.event_sequence),UINT(id.event_sequence>>32)};
        state.gpu.episode_output={UINT(id.episode_generation),UINT(id.episode_generation>>32),id.output_index,id.effect_handle};
        state.local_to_world=input.local_to_world;state.revisions={input.transform_revision,input.density_revision,input.appearance_revision};
        states.push_back(state);
    }
    std::sort(states.begin(),states.end(),[](const auto &a,const auto &b){return HeroKey(a.identity)<HeroKey(b.identity);});
    for(std::size_t i=1;i<states.size();++i)if(states[i-1].identity==states[i].identity)
        return Fail(ErrorCode::InvalidArgument,"A hero volume output was submitted twice with the same exact identity.");
    return Result::Success();
}
Result Texture(D3D12MA::Allocator *allocator,ViewExtent capacity,UINT depth,DXGI_FORMAT format,AllocationResource &resource)
{
    D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=depth?D3D12_RESOURCE_DIMENSION_TEXTURE3D:D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width=capacity.width;d.Height=capacity.height;d.DepthOrArraySize=UINT16(depth?depth:1u);d.MipLevels=1;
    d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto hr=allocator->CreateResource(&allocation,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,&resource.allocation,
        IID_PPV_ARGS(resource.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create native volumetric texture",hr);
    resource.graph_access=Access::Common;return Result::Success();
}
Result Upload(D3D12MA::Allocator *allocator,std::uint64_t bytes,AllocationResource &resource,std::byte *&mapped)
{
    D3D12MA::ALLOCATION_DESC allocation{};allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;
    d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto hr=allocator->CreateResource(&allocation,&d,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,&resource.allocation,
        IID_PPV_ARGS(resource.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create volumetric upload buffer",hr);
    void *address{};D3D12_RANGE empty{};hr=resource.resource->Map(0,&empty,&address);
    if(FAILED(hr))return Native("Map volumetric upload buffer",hr);
    mapped=static_cast<std::byte *>(address);std::memset(mapped,0,SIZE_T(bytes));resource.graph_access=Access::ComputeRead;
    return Result::Success();
}
} // namespace

struct VolumetricStorage
{
    struct Slot
    {
        CompletionSet completion;
        RayShaderBinder binder;
        std::vector<ComPtr<ID3D12Resource>> reads;
    };
    ComPtr<ID3D12Device> device;
    ComPtr<D3D12MA::Allocator> allocator;
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,6> pipelines;
    ComPtr<ID3D12PipelineState> hero_pipeline;
    ComPtr<ID3D12DescriptorHeap> descriptors,exports;
    AllocationResource constants,volumes,medium,phase,raw,filtered,luminance,transmittance,resolved;
    AllocationResource heroes;
    std::array<AllocationResource,2> history,depth_history;
    std::vector<Slot> slots;
    CompletionSet completion;
    ViewExtent render_capacity,froxel_capacity,previous_active,previous_froxel;
    VolumetricCamera previous_camera;
    VolumetricRevisions previous_revisions;
    std::vector<HeroState> previous_heroes;
    ComPtr<ID3D12Resource> previous_hero_gradient,previous_hero_blue_noise,previous_hero_noise_volume;
    Budget budget;
    std::byte *constant_map{},*volume_map{},*hero_map{};
    std::uint64_t previous_frame{},reset_epoch{};
    UINT max_volumes{},descriptor_stride{},read_index{};
    bool enabled{},hero_enabled{},history_valid{},usable{true};
    ~VolumetricStorage()
    {
        if(constant_map&&constants.resource)constants.resource->Unmap(0,nullptr);
        if(volume_map&&volumes.resource)volumes.resource->Unmap(0,nullptr);
        if(hero_map&&heroes.resource)heroes.resource->Unmap(0,nullptr);
    }
    std::array<AllocationResource *,14> Resources()
    {return {&constants,&volumes,&medium,&phase,&raw,&filtered,&luminance,&transmittance,&resolved,&history[0],&history[1],&depth_history[0],&depth_history[1],&heroes};}
    std::uint64_t Bytes()
    {std::uint64_t bytes{};for(auto *r:Resources())if(r->allocation)bytes+=r->allocation->GetSize();return bytes;}
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT slot,UINT offset)const
    {auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot*kVolumetricDescriptorCount+offset)*descriptor_stride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE Export(UINT slot)const
    {auto h=exports->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(slot)*descriptor_stride;return h;}
    void Srv(UINT slot,UINT offset,ID3D12Resource *resource,DXGI_FORMAT format,bool volume)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.ViewDimension=volume?D3D12_SRV_DIMENSION_TEXTURE3D:D3D12_SRV_DIMENSION_TEXTURE2D;
        if(volume)d.Texture3D.MipLevels=1;else d.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(resource,&d,Cpu(slot,offset));
    }
    void Uav(UINT slot,UINT offset,ID3D12Resource *resource,DXGI_FORMAT format,bool volume)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;d.ViewDimension=volume?D3D12_UAV_DIMENSION_TEXTURE3D:D3D12_UAV_DIMENSION_TEXTURE2D;
        if(volume)d.Texture3D.WSize=resource?resource->GetDesc().DepthOrArraySize:1u;
        device->CreateUnorderedAccessView(resource,nullptr,&d,Cpu(slot,offset));
    }
};
struct VolumetricSystem::Impl
{
    std::shared_ptr<VolumetricStorage> storage;
    struct Import { AllocationResource *allocation{};ResourceHandle handle; };
    struct Pending
    {
        const RenderGraphBuilder *graph{};
        std::vector<Import> imports;
        ResourceVersion constants,volumes,heroes,medium,phase,raw,previous,previous_depth,current,current_depth,filtered,luminance,transmittance,resolved;
        VolumetricCamera camera;
        VolumetricRevisions revisions;
        std::vector<HeroState> heroes_state;
        ComPtr<ID3D12Resource> hero_gradient,hero_blue_noise,hero_noise_volume;
        ViewExtent active,froxel;
        std::uint64_t frame{},reset_epoch{};
        UINT slot{},write{};
        bool history{},registered{};
    } pending;
    ResourceVersion ImportResource(RenderGraphBuilder &graph,AllocationResource &r,std::string_view name)
    {
        const ResourceHandle h=r.resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER?
            ResourceHandle(graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name)):
            ResourceHandle(graph.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue},r.graph_access,name));
        pending.imports.push_back({&r,h});return graph.Version(h);
    }
};
Result ValidateVfxHeroVolumes(VfxVolumeInputs inputs, std::uint32_t gradient_rows)
{
    std::vector<HeroState> states;
    if (auto result = HeroRecords(inputs, states); !result) return result;
    for (const auto &input : inputs)
        if (input.gradient_row >= gradient_rows)
            return Fail(ErrorCode::InvalidArgument,"Hero volume output references a row outside the actual resident authored gradient LUT.");
    return Result::Success();
}

VolumetricSystem::VolumetricSystem():impl_(std::make_unique<Impl>()){}
VolumetricSystem::~VolumetricSystem()=default;
std::uint64_t VolumetricCandidate::AllocatedBytes()const noexcept
{return storage_?storage_->Bytes():0;}
std::uint64_t VolumetricSystem::AllocatedBytes()const noexcept
{return impl_->storage?impl_->storage->Bytes():0;}
Result VolumetricSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    GraphicsOptionalQuality quality,const render_data::QualityProfile &profile,ViewExtent capacity,UINT frames,UINT maxVolumes,const RayShaderReader &reader,
    VolumetricCandidate &candidate,RendererPipelineCache *cache,bool heroEnabled)const
{
    const bool enabled=quality!=GraphicsOptionalQuality::Off;const Budget budget=enabled?Budget{profile.froxel_stride,profile.froxel_slices,profile.volumetric_area_samples}:Budget{};
    if(!Extent(capacity)||!frames||frames>UINT_MAX/kVolumetricDescriptorCount||
       (enabled&&UINT64(maxVolumes)>UINT64(SIZE_MAX)/(UINT64(frames)*sizeof(VolumetricVolume)))||
       (enabled&&heroEnabled&&UINT64(maxVolumes)>UINT64(SIZE_MAX)/(UINT64(frames)*sizeof(VolumetricHeroVolume)))||
       (enabled&&(!budget.stride||!device||!allocator||!root||!reader||!maxVolumes)))
        return Fail(ErrorCode::InvalidArgument,"Invalid volumetric quality, capacity, completed frame-slot count, volume capacity or native preparation inputs.");
    auto s=std::make_shared<VolumetricStorage>();s->enabled=enabled;s->render_capacity=capacity;s->slots.resize(frames);
    s->budget=budget;s->max_volumes=maxVolumes;s->hero_enabled=enabled&&heroEnabled;
    if(!enabled){candidate.storage_=std::move(s);return Result::Success();}
    s->device=device;s->allocator=allocator;s->root=root;s->froxel_capacity=FroxelExtent(capacity,budget.stride);
    if(s->froxel_capacity.width>D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION||s->froxel_capacity.height>D3D12_REQ_TEXTURE3D_U_V_OR_W_DIMENSION)
        return Fail(ErrorCode::InvalidArgument,"Volumetric froxel capacity exceeds native Texture3D dimensions.");
    for(const auto format:{kSignal,kScalar,kHdr})
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};const auto hr=device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support));
        if(FAILED(hr))return Native("Check volumetric format support",hr);
        const auto dimension=format==kHdr?D3D12_FORMAT_SUPPORT1_TEXTURE2D:format==kScalar?
            D3D12_FORMAT_SUPPORT1_TEXTURE2D|D3D12_FORMAT_SUPPORT1_TEXTURE3D:D3D12_FORMAT_SUPPORT1_TEXTURE3D;
        if((support.Support1&dimension)!=dimension||!(support.Support1&D3D12_FORMAT_SUPPORT1_SHADER_LOAD)||!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            return Fail(ErrorCode::TaskFailed,"Volumetric format lacks native texture, shader-load or typed UAV-store support.");
        if(s->hero_enabled&&format!=kHdr&&!(support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
            return Fail(ErrorCode::TaskFailed,"Hero medium/emission accumulation requires actual typed UAV-load support.");
    }
    for(auto *r:{&s->medium,&s->raw,&s->filtered,&s->history[0],&s->history[1]})
        if(auto result=Texture(allocator,s->froxel_capacity,budget.slices,kSignal,*r);!result)return result;
    if(auto result=Texture(allocator,s->froxel_capacity,budget.slices,kScalar,s->phase);!result)return result;
    if(auto result=Texture(allocator,s->froxel_capacity,budget.slices+1,kSignal,s->luminance);!result)return result;
    if(auto result=Texture(allocator,s->froxel_capacity,budget.slices+1,kScalar,s->transmittance);!result)return result;
    for(auto &r:s->depth_history)if(auto result=Texture(allocator,s->froxel_capacity,0,kScalar,r);!result)return result;
    if(auto result=Texture(allocator,capacity,0,kHdr,s->resolved);!result)return result;
    if(auto result=Upload(allocator,UINT64(frames)*kConstantStride,s->constants,s->constant_map);!result)return result;
    if(auto result=Upload(allocator,UINT64(frames)*maxVolumes*sizeof(VolumetricVolume),s->volumes,s->volume_map);!result)return result;
    if(s->hero_enabled)
        if(auto result=Upload(allocator,UINT64(frames)*maxVolumes*sizeof(VolumetricHeroVolume),s->heroes,s->hero_map);!result)return result;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=frames*kVolumetricDescriptorCount;
    auto hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->descriptors));if(FAILED(hr))return Native("Create volumetric CPU descriptors",hr);
    s->descriptor_stride=device->GetDescriptorHandleIncrementSize(heap.Type);
    heap.NumDescriptors=frames;
    hr=device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&s->exports));if(FAILED(hr))return Native("Create volumetric HDR export descriptors",hr);
    D3D12_SHADER_RESOURCE_VIEW_DESC hdrView{};hdrView.Format=kHdr;hdrView.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    hdrView.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;hdrView.Texture2D.MipLevels=1;
    for(UINT slot=0;slot<frames;++slot)device->CreateShaderResourceView(s->resolved.resource.Get(),&hdrView,s->Export(slot));
    for(UINT i=0;i<std::size(kProducts);++i)
    {
        std::vector<std::byte> code;if(auto result=reader(kProducts[i],code);!result)return result;
        if(code.empty())return Fail(ErrorCode::TaskFailed,"Volumetric shader product is empty: "+std::string(kProducts[i]));
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root;p.CS={code.data(),code.size()};
        hr=cache?cache->CreateCompute(&p,IID_PPV_ARGS(&s->pipelines[i])):device->CreateComputePipelineState(&p,IID_PPV_ARGS(&s->pipelines[i]));
        if(FAILED(hr))return Native("Create volumetric PSO "+std::string(kProducts[i]),hr);
    }
    if(s->hero_enabled)
    {
        std::vector<std::byte> code;if(auto result=reader("render/shader/volumetric_hero_density_cs.dxil",code);!result)return result;
        if(code.empty())return Fail(ErrorCode::InvalidArgument,"Volumetric hero density shader product is empty.");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root;pso.CS={code.data(),code.size()};
        const auto heroHr=cache?cache->CreateCompute(&pso,IID_PPV_ARGS(&s->hero_pipeline)):
            device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&s->hero_pipeline));
        if(FAILED(heroHr))return Native("Create native volumetric hero density PSO",heroHr);
    }
    candidate.storage_=std::move(s);return Result::Success();
}
Result VolumetricSystem::Commit(VolumetricCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.storage_||impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Volumetric candidate absent or a submission is pending.");
    if(impl_->storage&&(!impl_->storage->usable||!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Fail(ErrorCode::InvalidState,"Volumetric replacement requires all old uses completed and usable storage.");
    impl_->storage=std::move(candidate.storage_);return Result::Success();
}
Result VolumetricSystem::Register(RenderGraphBuilder &graph,const VolumetricInputs &in,VolumetricOutputs &out)
{
    out={};auto s=impl_->storage;
    if(!s)return Fail(ErrorCode::InvalidState,"Volumetric service has not been committed.");
    if(!s->enabled)return Result::Success();
    if(!s->usable||impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Volumetric storage faulted or its previous submission remains pending.");
    if(!Extent(in.render_active)||in.render_active.width>s->render_capacity.width||in.render_active.height>s->render_capacity.height||
       in.frame_slot>=s->slots.size()||!s->slots[in.frame_slot].completion.IsSatisfiedBy(in.completed)||!in.make_bindings||
       !DepthRead(graph,in.depth,in.render_active)||!TextureRead(graph,in.hdr,in.render_active,kHdr)||
       !Camera(in.camera)||in.volumes.size()>s->max_volumes||in.hero_volumes.size()>s->max_volumes-in.volumes.size())
        return Fail(ErrorCode::InvalidArgument,"Invalid volumetric depth/HDR, camera, active extent, volume capacity or completed frame slot.");
    if(!in.hero_volumes.empty()&&(!s->hero_enabled||!s->hero_pipeline||!s->heroes.resource||
        !BlueNoiseRead(graph,in.hero_blue_noise)||!GradientRead(graph,in.hero_gradient)||
        !HeroNoiseVolumeRead(graph,in.hero_noise_volume)))
        return Fail(ErrorCode::InvalidArgument,"Active hero volumes require High VFX admission, prepared native metadata/PSO, actual resident 64x64x64 R8 STBN, authored RGBA16F gradient LUT and RGBA8 noise volume.");
    if(!in.hero_volumes.empty())for(const auto &hero:in.hero_volumes)
        if(hero.gradient_row>=in.hero_gradient.resource->GetDesc().Height)
            return Fail(ErrorCode::InvalidArgument,"Hero volume output references a row outside the actual resident authored gradient LUT.");
    std::vector<HeroState> heroes;if(auto result=HeroRecords(in.hero_volumes,heroes);!result)return result;
    for(const auto &volume:in.volumes)
    {
        const auto d=volume.density_extinction;const auto scatter=volume.scattering;
        if(!Finite(volume.world_to_local)||!Finite(d)||!Finite(scatter)||d.x<0||d.y<0||!std::isfinite(d.x*d.y)||std::abs(d.z)>=1||
           scatter.x<0||scatter.y<0||scatter.z<0||scatter.x>d.y||scatter.y>d.y||scatter.z>d.y||
           volume.identity.w>UINT(VolumetricShape::Sphere)||(!volume.identity.x&&!volume.identity.y)||!volume.identity.z)
            return Fail(ErrorCode::InvalidArgument,"Volumetric records require stable identity, finite transforms, nonnegative density/extinction, conservative scattering and |g|<1.");
        if(volume.identity.w!=UINT(VolumetricShape::Global))
        {
            const float determinant=DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(DirectX::XMLoadFloat4x4(&volume.world_to_local)));
            if(!std::isfinite(determinant)||determinant==0||volume.world_to_local._14!=0||volume.world_to_local._24!=0||
               volume.world_to_local._34!=0||volume.world_to_local._44!=1)
                return Fail(ErrorCode::InvalidArgument,"Local volume world-to-local transform must be nonsingular affine.");
        }
    }
    const ResourceVersion lightVersions[]{in.lights.lights,in.lights.ranges,in.lights.inline_indices,in.lights.spill_indices};
    for(UINT i=0;i<4;++i)if(!ResourceRead(graph,lightVersions[i],in.lights.resources[i]))
        return Fail(ErrorCode::InvalidArgument,"Volumetric world-light query requires all four authoritative LightList resources.");
    const ResourceVersion shadowVersions[]{in.shadows.metadata,in.shadows.page_table,in.shadows.static_bank,in.shadows.dynamic_bank,in.shadows.draw_records};
    const bool shadows=in.shadows.resources[0]!=nullptr;
    for(UINT i=0;i<5;++i)if(shadows?!ResourceRead(graph,shadowVersions[i],in.shadows.resources[i]):
        (in.shadows.resources[i]!=nullptr||bool(shadowVersions[i].resource)))
        return Fail(ErrorCode::InvalidArgument,"Volumetric ShadowData must contain all five current resources or be absent.");
    const ResourceVersion localShadowVersions[]{in.local_shadows.metadata,in.local_shadows.records,
        in.local_shadows.static_bank,in.local_shadows.dynamic_bank,in.local_shadows.draw_records};
    const bool localShadows=in.local_shadows.resources[0]!=nullptr;
    for(UINT i=0;i<5;++i)if(localShadows?!ResourceRead(graph,localShadowVersions[i],in.local_shadows.resources[i]):
        (in.local_shadows.resources[i]!=nullptr||bool(localShadowVersions[i].resource)))
        return Fail(ErrorCode::InvalidArgument,"Volumetric local ShadowData must contain all five current world-query resources or be absent.");
    if(localShadows&&in.local_shadows.descriptor_count!=kLocalShadowDescriptorCount)
        return Fail(ErrorCode::InvalidArgument,"Volumetric local shadow table does not expose its exact native five-SRV contract.");
    if(in.compact_shadow.identity.w)
    {
        if(!in.compact_shadow.identity.z||(!in.compact_shadow.identity.x&&!in.compact_shadow.identity.y)||
           !DepthRead(graph,in.compact_shadow.atlas,{1,1})||!std::isfinite(in.compact_shadow.receiver_bias)||in.compact_shadow.receiver_bias<0)
            return Fail(ErrorCode::InvalidArgument,"Volumetric compact CSM fallback requires its actual atlas and exact directional identity.");
        for(UINT i=0;i<3;++i)
        {
            const auto tile=in.compact_shadow.atlas_scale_offset[i];
            if(!Finite(in.compact_shadow.view_projection[i])||!Finite(tile)||tile.x<=0||tile.y<=0||tile.z<0||tile.w<0||tile.x+tile.z>1||tile.y+tile.w>1)
                return Fail(ErrorCode::InvalidArgument,"Volumetric compact shadow projection or atlas tile is invalid.");
        }
    }
    Impl::Pending next;next.graph=&graph;next.slot=in.frame_slot;next.frame=in.frame_index;next.camera=in.camera;
    next.revisions=in.revisions;next.active=in.render_active;next.froxel=FroxelExtent(next.active,s->budget.stride);
    next.heroes_state=std::move(heroes);
    if(!next.heroes_state.empty())
    {next.hero_gradient=in.hero_gradient.resource;next.hero_blue_noise=in.hero_blue_noise.resource;
     next.hero_noise_volume=in.hero_noise_volume.resource;}
    next.write=1-s->read_index;next.reset_epoch=s->reset_epoch;
    next.history=s->history_valid&&!in.reset_history&&in.frame_index==s->previous_frame+1&&in.revisions==s->previous_revisions&&
        next.heroes_state==s->previous_heroes&&next.hero_gradient.Get()==s->previous_hero_gradient.Get()&&
        next.hero_blue_noise.Get()==s->previous_hero_blue_noise.Get()&&
        next.hero_noise_volume.Get()==s->previous_hero_noise_volume.Get()&&
        SameCamera(in.previous_camera,s->previous_camera);
    auto &lease=s->slots[next.slot];lease.binder={};lease.reads.clear();impl_->pending=std::move(next);auto &p=impl_->pending;
    p.constants=impl_->ImportResource(graph,s->constants,"Volumetric.Constants");
    p.volumes=impl_->ImportResource(graph,s->volumes,"Volumetric.VolumeRecords");
    if(!p.heroes_state.empty())p.heroes=impl_->ImportResource(graph,s->heroes,"Volumetric.HeroVolumeMetadata");
    p.medium=impl_->ImportResource(graph,s->medium,"Volumetric.Medium");p.phase=impl_->ImportResource(graph,s->phase,"Volumetric.Anisotropy");
    p.raw=impl_->ImportResource(graph,s->raw,"Volumetric.RawScattering");p.current=impl_->ImportResource(graph,s->history[p.write],"Volumetric.CurrentScattering");
    p.current_depth=impl_->ImportResource(graph,s->depth_history[p.write],"Volumetric.CurrentColumnDepth");
    p.filtered=impl_->ImportResource(graph,s->filtered,"Volumetric.FilteredScattering");
    p.luminance=impl_->ImportResource(graph,s->luminance,"Volumetric.CumulativeL");p.transmittance=impl_->ImportResource(graph,s->transmittance,"Volumetric.CumulativeT");
    p.resolved=impl_->ImportResource(graph,s->resolved,"Volumetric.ResolvedOpaqueHDR");
    if(p.history)
    {
        p.previous=impl_->ImportResource(graph,s->history[s->read_index],"Volumetric.PreviousScattering");
        p.previous_depth=impl_->ImportResource(graph,s->depth_history[s->read_index],"Volumetric.PreviousColumnDepth");
    }
    const auto &previous=p.history?s->previous_camera:in.camera;VolumetricConstants c;
    c.inverse_view_projection=in.camera.inverse_view_projection;c.world_to_view=in.camera.world_to_view;
    c.previous_view_projection=previous.view_projection;c.previous_world_to_view=previous.world_to_view;
    c.shadow_view_projection=in.compact_shadow.view_projection;c.shadow_atlas_scale_offset=in.compact_shadow.atlas_scale_offset;
    c.render_size=Size(p.active);c.froxel_size=Size(p.froxel);c.capacity={float(s->froxel_capacity.width),float(s->froxel_capacity.height),float(s->budget.slices),float(s->budget.stride)};
    c.previous_render_size=Size(p.history?s->previous_active:p.active);c.previous_froxel_size=Size(p.history?s->previous_froxel:p.froxel);
    c.origin_near={in.camera.origin.x,in.camera.origin.y,in.camera.origin.z,in.camera.near_plane};
    c.previous_origin_near={previous.origin.x,previous.origin.y,previous.origin.z,previous.near_plane};
    c.depth_range={in.camera.far_plane,std::log2(in.camera.far_plane/in.camera.near_plane),previous.far_plane,std::log2(previous.far_plane/previous.near_plane)};
    c.shadow_identity=in.compact_shadow.identity;c.frame_info={UINT(p.frame),UINT(p.frame>>32),p.history?1u:0u,s->budget.area_samples};
    c.counts={UINT(in.volumes.size()),s->budget.slices,shadows?1u:0u,0u};
    c.hero_info={UINT(p.heroes_state.size()),s->hero_enabled?1u:0u,localShadows?1u:0u,0u};
    if(in.compact_shadow.identity.w)
    {
        const auto desc=in.compact_shadow.atlas.resource->GetDesc();
        c.shadow_parameters={1.0f/float(desc.Width),1.0f/float(desc.Height),in.compact_shadow.receiver_bias,0};
    }
    std::memcpy(s->constant_map+UINT64(p.slot)*kConstantStride,&c,sizeof(c));
    if(!in.volumes.empty())std::memcpy(s->volume_map+UINT64(p.slot)*s->max_volumes*sizeof(VolumetricVolume),in.volumes.data(),in.volumes.size_bytes());
    for(UINT i=0;i<p.heroes_state.size();++i)
        std::memcpy(s->hero_map+(UINT64(p.slot)*s->max_volumes+i)*sizeof(VolumetricHeroVolume),&p.heroes_state[i].gpu,sizeof(VolumetricHeroVolume));
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{s->constants.resource->GetGPUVirtualAddress()+UINT64(p.slot)*kConstantStride,kConstantStride};
    s->device->CreateConstantBufferView(&cbv,s->Cpu(p.slot,0));
    s->Srv(p.slot,1,in.depth.resource,kScalar,false);s->Srv(p.slot,2,in.hdr.resource,kHdr,false);
    D3D12_SHADER_RESOURCE_VIEW_DESC buffer{};buffer.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;buffer.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    buffer.Buffer.FirstElement=UINT64(p.slot)*s->max_volumes;buffer.Buffer.NumElements=std::max(UINT(in.volumes.size()),1u);buffer.Buffer.StructureByteStride=sizeof(VolumetricVolume);
    s->device->CreateShaderResourceView(s->volumes.resource.Get(),&buffer,s->Cpu(p.slot,3));
    s->Srv(p.slot,4,s->medium.resource.Get(),kSignal,true);s->Srv(p.slot,5,s->phase.resource.Get(),kScalar,true);
    s->Srv(p.slot,6,s->raw.resource.Get(),kSignal,true);s->Srv(p.slot,7,p.history?s->history[s->read_index].resource.Get():nullptr,kSignal,true);
    s->Srv(p.slot,8,p.history?s->depth_history[s->read_index].resource.Get():nullptr,kScalar,false);
    s->Srv(p.slot,9,s->history[p.write].resource.Get(),kSignal,true);s->Srv(p.slot,10,s->filtered.resource.Get(),kSignal,true);
    s->Srv(p.slot,11,s->luminance.resource.Get(),kSignal,true);s->Srv(p.slot,12,s->transmittance.resource.Get(),kScalar,true);
    s->Srv(p.slot,13,in.compact_shadow.identity.w?in.compact_shadow.atlas.resource:nullptr,kScalar,false);
    s->Srv(p.slot,14,s->depth_history[p.write].resource.Get(),kScalar,false);
    s->Uav(p.slot,15,s->medium.resource.Get(),kSignal,true);s->Uav(p.slot,16,s->phase.resource.Get(),kScalar,true);
    s->Uav(p.slot,17,s->raw.resource.Get(),kSignal,true);s->Uav(p.slot,18,s->history[p.write].resource.Get(),kSignal,true);
    s->Uav(p.slot,19,s->depth_history[p.write].resource.Get(),kScalar,false);s->Uav(p.slot,20,s->filtered.resource.Get(),kSignal,true);
    s->Uav(p.slot,21,s->luminance.resource.Get(),kSignal,true);s->Uav(p.slot,22,s->transmittance.resource.Get(),kScalar,true);
    s->Uav(p.slot,23,s->resolved.resource.Get(),kHdr,false);
    buffer.Buffer.FirstElement=s->heroes.resource?UINT64(p.slot)*s->max_volumes:0;
    buffer.Buffer.NumElements=std::max(UINT(p.heroes_state.size()),1u);buffer.Buffer.StructureByteStride=sizeof(VolumetricHeroVolume);
    s->device->CreateShaderResourceView(s->heroes.resource.Get(),&buffer,s->Cpu(p.slot,24));
    D3D12_SHADER_RESOURCE_VIEW_DESC blueNoise{};blueNoise.Format=DXGI_FORMAT_R8_UNORM;
    blueNoise.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;blueNoise.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    blueNoise.Texture2DArray.MipLevels=1;blueNoise.Texture2DArray.ArraySize=64;
    s->device->CreateShaderResourceView(p.heroes_state.empty()?nullptr:in.hero_blue_noise.resource,&blueNoise,s->Cpu(p.slot,25));
    D3D12_SHADER_RESOURCE_VIEW_DESC noiseVolume{};noiseVolume.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    noiseVolume.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;
    noiseVolume.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    noiseVolume.Texture3D.MipLevels=1;
    s->device->CreateShaderResourceView(p.heroes_state.empty()?nullptr:in.hero_noise_volume.resource,
                                        &noiseVolume,s->Cpu(p.slot,26));
    RayShaderBinder binder;if(auto result=in.make_bindings(s->Cpu(p.slot,0),kVolumetricDescriptorCount,binder);!result)return result;
    if(!binder)return Fail(ErrorCode::InvalidArgument,"Volumetric frame table builder returned no compute binder.");
    lease.binder=binder;
    auto retain=[&](ID3D12Resource *resource){if(resource&&std::none_of(lease.reads.begin(),lease.reads.end(),[&](const auto &r){return r.Get()==resource;}))lease.reads.emplace_back(resource);};
    retain(in.depth.resource);retain(in.hdr.resource);if(in.compact_shadow.identity.w)retain(in.compact_shadow.atlas.resource);
    for(auto *resource:in.lights.resources)retain(resource);for(auto *resource:in.shadows.resources)retain(resource);
    for(auto *resource:in.local_shadows.resources)retain(resource);
    if(!p.heroes_state.empty()){retain(in.hero_blue_noise.resource);retain(in.hero_gradient.resource);
        retain(in.hero_noise_volume.resource);}
    const BufferRange constantsRange{UINT64(p.slot)*kConstantStride,kConstantStride};
    const BufferRange volumesRange{UINT64(p.slot)*s->max_volumes*sizeof(VolumetricVolume),std::max(UINT64(in.volumes.size_bytes()),UINT64(sizeof(VolumetricVolume)))};
    const BufferRange heroesRange{UINT64(p.slot)*s->max_volumes*sizeof(VolumetricHeroVolume),UINT64(p.heroes_state.size())*sizeof(VolumetricHeroVolume)};
    auto execute=[s,binder,froxel=p.froxel,active=p.active](UINT stage)
    {
        return [s,binder,froxel,active,stage](RenderPassContext &ctx)->Result
        {
            auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->pipelines[stage].Get());
            if(auto result=binder(cmd);!result)return result;
            if(stage<4)cmd->Dispatch((froxel.width+3)/4,(froxel.height+3)/4,(s->budget.slices+3)/4);
            else if(stage==4)cmd->Dispatch((froxel.width+7)/8,(froxel.height+7)/8,1);
            else cmd->Dispatch((active.width+7)/8,(active.height+7)/8,1);
            return Result::Success();
        };
    };
    auto constants=[&](PassBuilder &pass){pass.Read(p.constants,Access::ComputeRead,constantsRange);};
    auto density=graph.AddPass(170,"VolumetricDensity",QueueHint::Direct);constants(density);
    density.Read(in.depth.version,Access::ComputeRead);density.Read(p.volumes,Access::ComputeRead,volumesRange);
    p.medium=density.Write(p.medium,Access::UnorderedWrite);p.phase=density.Write(p.phase,Access::UnorderedWrite);
    p.current_depth=density.Write(p.current_depth,Access::UnorderedWrite);density.SetExecuteChecked(execute(0));
    if(!p.heroes_state.empty())
    {
        p.raw=density.Write(p.raw,Access::UnorderedWrite);
        auto hero=graph.AddPass(176,"VolumetricHeroDensity",QueueHint::Direct);constants(hero);
        hero.Read(p.heroes,Access::ComputeRead,heroesRange);hero.Read(p.current_depth,Access::ComputeRead);
        hero.Read(in.hero_blue_noise.version,Access::ComputeRead);
        hero.Read(in.hero_gradient.version,Access::ComputeRead);
        hero.Read(in.hero_noise_volume.version,Access::ComputeRead);
        p.medium=hero.ReadWrite(p.medium,Access::UnorderedWrite);p.phase=hero.ReadWrite(p.phase,Access::UnorderedWrite);
        p.raw=hero.ReadWrite(p.raw,Access::UnorderedWrite);
        hero.SetExecuteChecked([s,binder,froxel=p.froxel](RenderPassContext &ctx)->Result
        {
            auto *cmd=static_cast<ID3D12GraphicsCommandList *>(ctx.NativeCommandList());
            cmd->SetComputeRootSignature(s->root.Get());cmd->SetPipelineState(s->hero_pipeline.Get());
            if(auto result=binder(cmd);!result)return result;
            cmd->Dispatch((froxel.width+3)/4,(froxel.height+3)/4,(s->budget.slices+3)/4);
            return Result::Success();
        });
    }
    auto lighting=graph.AddPass(171,"VolumetricLighting",QueueHint::Direct);constants(lighting);
    lighting.Read(p.medium,Access::ComputeRead);lighting.Read(p.phase,Access::ComputeRead);
    for(auto version:lightVersions)lighting.Read(version,Access::ComputeRead);
    if(shadows)for(auto version:shadowVersions)lighting.Read(version,Access::AllShaderRead);
    if(localShadows)for(auto version:localShadowVersions)lighting.Read(version,Access::AllShaderRead);
    if(in.compact_shadow.identity.w)lighting.Read(in.compact_shadow.atlas.version,Access::ComputeRead);
    p.raw=p.heroes_state.empty()?lighting.Write(p.raw,Access::UnorderedWrite):lighting.ReadWrite(p.raw,Access::UnorderedWrite);
    lighting.SetExecuteChecked(execute(1));
    auto temporal=graph.AddPass(172,"VolumetricTemporal",QueueHint::Direct);constants(temporal);
    temporal.Read(p.raw,Access::ComputeRead);temporal.Read(p.current_depth,Access::ComputeRead);
    if(p.history){temporal.Read(p.previous,Access::ComputeRead);temporal.Read(p.previous_depth,Access::ComputeRead);}
    p.current=temporal.Write(p.current,Access::UnorderedWrite);temporal.SetExecuteChecked(execute(2));
    auto spatial=graph.AddPass(173,"VolumetricSpatial",QueueHint::Direct);constants(spatial);
    spatial.Read(p.current,Access::ComputeRead);spatial.Read(p.current_depth,Access::ComputeRead);
    p.filtered=spatial.Write(p.filtered,Access::UnorderedWrite);spatial.SetExecuteChecked(execute(3));
    auto integrate=graph.AddPass(174,"VolumetricIntegrate",QueueHint::Direct);constants(integrate);
    integrate.Read(p.filtered,Access::ComputeRead);p.luminance=integrate.Write(p.luminance,Access::UnorderedWrite);
    p.transmittance=integrate.Write(p.transmittance,Access::UnorderedWrite);integrate.SetExecuteChecked(execute(4));
    auto resolve=graph.AddPass(175,"VolumetricResolve",QueueHint::Direct);constants(resolve);
    resolve.Read(in.depth.version,Access::ComputeRead);resolve.Read(in.hdr.version,Access::ComputeRead);
    for(auto version:{p.filtered,p.current_depth,p.luminance,p.transmittance})resolve.Read(version,Access::ComputeRead);
    p.resolved=resolve.Write(p.resolved,Access::UnorderedWrite);resolve.SetExecuteChecked(execute(5));
    for(auto version:{p.filtered,p.current_depth,p.luminance,p.transmittance,p.constants})graph.SetFinalAccess(version,Access::AllShaderRead);
    graph.SetFinalAccess(p.current,Access::ComputeRead);graph.SetFinalAccess(p.resolved,Access::ComputeRead);
    out.scattering={p.luminance,s->luminance.resource.Get(),s->Cpu(p.slot,11)};
    out.transmittance={p.transmittance,s->transmittance.resource.Get(),s->Cpu(p.slot,12)};
    out.resolved={p.resolved,s->resolved.resource.Get(),s->Export(p.slot)};
    out.filtered_scattering={p.filtered,s->filtered.resource.Get(),s->Cpu(p.slot,10)};
    out.column_depth={p.current_depth,s->depth_history[p.write].resource.Get(),s->Cpu(p.slot,14)};
    out.constants=p.constants;out.constant_resource=s->constants.resource.Get();out.descriptor_suffix=s->Cpu(p.slot,0);
    out.froxel_active=p.froxel;out.froxel_capacity=s->froxel_capacity;out.slices=s->budget.slices;p.registered=true;
    return Result::Success();
}
Result VolumetricSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;auto s=impl_->storage;if(!p.graph)return Result::Success();
    if(p.graph!=&graph)return Fail(ErrorCode::InvalidArgument,"Volumetric completion refers to a different graph.");
    auto &slot=s->slots[p.slot];s->completion.Merge(completion);slot.completion.Merge(completion);
    for(const auto &i:p.imports)
    {
        const auto use=graph.ResourceCompletion(i.handle);s->completion.Merge(use);slot.completion.Merge(use);
        i.allocation->graph_completion.Merge(completion);i.allocation->graph_completion.Merge(use);
        if(success){i.allocation->graph_access=graph.FinalAccess(i.handle);i.allocation->graph_queue=graph.FinalQueue(i.handle);}
    }
    if(success&&p.registered)
    {
        s->read_index=p.write;s->previous_frame=p.frame;s->previous_camera=p.camera;s->previous_revisions=p.revisions;
        s->previous_active=p.active;s->previous_froxel=p.froxel;s->history_valid=s->reset_epoch==p.reset_epoch;
        s->previous_heroes=std::move(p.heroes_state);
        s->previous_hero_gradient=std::move(p.hero_gradient);s->previous_hero_blue_noise=std::move(p.hero_blue_noise);
        s->previous_hero_noise_volume=std::move(p.hero_noise_volume);
    }
    else {s->history_valid=false;if(!success)s->usable=false;}
    p={};return Result::Success();
}
void VolumetricSystem::ResetHistory()noexcept
{if(auto s=impl_->storage){s->history_valid=false;++s->reset_epoch;}}
Result VolumetricSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph)return Fail(ErrorCode::InvalidState,"Complete volumetric pending submission before reset.");
    if(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed))
        return Fail(ErrorCode::InvalidState,"Volumetric reset requires all GPU uses completed.");
    impl_->storage.reset();return Result::Success();
}
} // namespace hs
