#include "renderer_local_shadow.hpp"
#include "d3d12_resources.hpp"

#include <D3D12MemAlloc.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <tuple>
#include <utility>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
Result Failure(ErrorCode code,std::string message)
{return Result::Failure(code,"hs_renderer_local_shadow",std::move(message));}
Result Native(std::string_view operation,HRESULT hr)
{return Failure(ErrorCode::TaskFailed,std::string(operation)+" failed (HRESULT "+std::to_string(std::int32_t(hr))+").");}
D3D12_RESOURCE_DESC Buffer(std::uint64_t bytes)
{
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;
    d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
}
bool Sphere(DirectX::XMFLOAT4 p)
{return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.w)&&p.w>=0.0f;}
bool SameBounds(DirectX::XMFLOAT4 a,DirectX::XMFLOAT4 b)
{return a.x==b.x&&a.y==b.y&&a.z==b.z&&a.w==b.w;}
bool StaticCaster(const VirtualShadowCaster &c,bool previous=false)
{return previous?c.previous_present&&c.previous_immutable_source&&!c.previous_skin&&!c.previous_wind&&!c.previous_animated_alpha:
    c.present&&c.immutable_source&&!c.skin&&!c.wind&&!c.animated_alpha;}
bool Changed(const VirtualShadowCaster &c)
{
    return c.present!=c.previous_present||c.instance_generation!=c.previous_generation||
        c.current_revisions!=c.previous_revisions||c.current_cut_revision!=c.previous_cut_revision||
        !SameBounds(c.current_bounds,c.previous_bounds)||StaticCaster(c)!=StaticCaster(c,true);
}
bool SameLight(const LightView &a,const LightView &b)
{
    return a.stable_id==b.stable_id&&a.generation==b.generation&&a.revision==b.revision&&a.type==b.type&&
        a.position.x==b.position.x&&a.position.y==b.position.y&&a.position.z==b.position.z&&a.range==b.range&&
        a.direction.x==b.direction.x&&a.direction.y==b.direction.y&&a.direction.z==b.direction.z&&
        a.inner_cone_radians==b.inner_cone_radians&&a.outer_cone_radians==b.outer_cone_radians&&a.emitter_radius==b.emitter_radius&&
        a.shape.x==b.shape.x&&a.shape.y==b.shape.y&&a.area_tangent.x==b.area_tangent.x&&
        a.area_tangent.y==b.area_tangent.y&&a.area_tangent.z==b.area_tangent.z&&a.two_sided==b.two_sided;
}
bool Area(const LightView &light)
{return light.type==LightType::Rectangle||light.type==LightType::Disk;}
double EmitterBound(const LightView &light)
{
    if(light.type==LightType::Rectangle)return std::hypot(double(light.shape.x)*0.5,double(light.shape.y)*0.5);
    if(light.type==LightType::Disk)return std::max(double(light.shape.x),double(light.shape.y));
    return light.emitter_radius;
}
UINT SampleCount(const LightView &light,const LocalShadowProfile &profile)
{return Area(light)?profile.area_samples:1u;}
UINT FaceCount(const LightView &light,const LocalShadowProfile &profile)
{return light.type==LightType::Spot?1u:6u*SampleCount(light,profile);}
bool Supported(const LightView &light)
{
    const auto &p=light.position;const auto &d=light.direction;
    const bool area=Area(light),spot=light.type==LightType::Spot;
    if((!area&&!spot&&light.type!=LightType::Point)||!light.stable_id||!light.generation||!light.revision||
        !std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)||!std::isfinite(light.range)||light.range<=0.02f||
        !std::isfinite(light.quantity)||light.quantity<0||!std::isfinite(light.color.x)||light.color.x<0||
        !std::isfinite(light.color.y)||light.color.y<0||!std::isfinite(light.color.z)||light.color.z<0||
        light.unit!=(area?LightUnit::Luminance:LightUnit::Lumen))return false;
    if((area||spot)&&(!std::isfinite(d.x)||!std::isfinite(d.y)||!std::isfinite(d.z)||
        !(std::hypot(double(d.x),double(d.y),double(d.z))>0)))return false;
    if(spot&&(!std::isfinite(light.inner_cone_radians)||!std::isfinite(light.outer_cone_radians)||
        light.inner_cone_radians<0||light.inner_cone_radians>=light.outer_cone_radians||
        light.outer_cone_radians>=DirectX::XM_PIDIV2))return false;
    if(area)
    {
        const auto &t=light.area_tangent;
        if(!std::isfinite(t.x)||!std::isfinite(t.y)||!std::isfinite(t.z)||
            !std::isfinite(light.shape.x)||light.shape.x<=0||!std::isfinite(light.shape.y)||light.shape.y<=0)return false;
        // Match the light-list emitter basis: cross original inputs in double,
        // so a parallel authored tangent cannot become a fabricated orientation.
        const double bx=double(d.y)*t.z-double(d.z)*t.y,by=double(d.z)*t.x-double(d.x)*t.z,
            bz=double(d.x)*t.y-double(d.y)*t.x;
        if(!(std::hypot(bx,by,bz)>0)||!std::isfinite(std::hypot(bx,by,bz)))return false;
        const double area_m2=double(light.shape.x)*light.shape.y*(light.type==LightType::Rectangle?1.0:std::numbers::pi_v<double>);
        if(area_m2<std::numeric_limits<float>::min()||area_m2>std::numeric_limits<float>::max())return false;
    }
    else if(!std::isfinite(light.emitter_radius)||light.emitter_radius<0)return false;
    const double influence=double(light.range)+(area?EmitterBound(light):0.0);
    return std::isfinite(influence)&&influence*influence<=std::numeric_limits<float>::max();
}
double Priority(const LightView &light,DirectX::XMFLOAT3 camera,bool moved)
{
    const double x=double(light.position.x)-camera.x,y=double(light.position.y)-camera.y,z=double(light.position.z)-camera.z;
    const double distance2=x*x+y*y+z*z;
    const double luminance=std::max(0.0,double(light.color.x)*0.2126+double(light.color.y)*0.7152+double(light.color.z)*0.0722);
    const double flux=Area(light)?double(light.quantity)*std::numbers::pi_v<double>*double(light.shape.x)*light.shape.y*
        (light.type==LightType::Rectangle?1.0:std::numbers::pi_v<double>)*(light.two_sided?2.0:1.0):double(light.quantity);
    const double importance=flux*luminance,bound=EmitterBound(light),influence=double(light.range)+(Area(light)?bound:0.0);
    // Projected influence area and the emitter's photometric importance are
    // evaluated separately; revision/movement modestly prioritizes stale caches.
    const double contribution=std::min(1.0,influence*influence/std::max(distance2,0.0001));
    return importance*contribution*(moved?1.125:1.0)/std::max(distance2,bound*bound+0.01);
}
bool Overlap(const DirectX::XMFLOAT4 &sphere,const DirectX::XMFLOAT4X4 &matrix,float guard)
{
    // DirectX row-vector clip planes are matrix columns. Expanded XY includes
    // the actual guard rendered by the projection; Z remains the native [0,W].
    const float *m=&matrix._11;const float center[4]{sphere.x,sphere.y,sphere.z,1.0f};
    const auto outside=[&](int axis,float sign,float w)
    {
        float p[4]{};for(int row=0;row<4;++row)p[row]=sign*m[row*4+axis]+w*m[row*4+3];
        float distance{};for(int row=0;row<4;++row)distance+=center[row]*p[row];
        return distance < -sphere.w*std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
    };
    return !outside(0,1,guard)&&!outside(0,-1,guard)&&!outside(1,1,guard)&&!outside(1,-1,guard)&&
        !outside(2,1,0)&&!outside(2,-1,1);
}
struct IndexedCommand{std::uint32_t pass;D3D12_DRAW_INDEXED_ARGUMENTS draw;};
struct MeshCommand{std::uint32_t pass;D3D12_DISPATCH_MESH_ARGUMENTS draw;};
static_assert(sizeof(IndexedCommand)==24);static_assert(sizeof(MeshCommand)==16);
} // namespace

struct LocalShadowStorage
{
    struct CachedLight
    {
        LightView light;std::uint32_t valid_faces{},face_count{};bool occupied{},selected{};
        std::array<UINT,kLocalShadowMaximumFaces> face_tiles{};
    };
    struct Frame
    {
        AllocationResource upload,metadata,records,draws,indexed,mesh,counts;
        std::byte *mapped{};CompletionSet completion;
        std::vector<std::byte> prepared_upload;
        std::uint64_t record_generation{};
        std::vector<VirtualShadowDrawBatch> batches;
        std::array<std::vector<D3D12_RECT>,2> clears;
        std::vector<std::shared_ptr<const void>> owners;
        ~Frame(){if(mapped&&upload.resource)upload.resource->Unmap(0,nullptr);}
        Frame()=default;Frame(const Frame &)=delete;Frame &operator=(const Frame &)=delete;
        Frame(Frame &&other)noexcept:upload(std::move(other.upload)),metadata(std::move(other.metadata)),records(std::move(other.records)),
            draws(std::move(other.draws)),indexed(std::move(other.indexed)),mesh(std::move(other.mesh)),counts(std::move(other.counts)),
            mapped(std::exchange(other.mapped,nullptr)),completion(other.completion),
            prepared_upload(std::move(other.prepared_upload)),record_generation(other.record_generation),
            batches(std::move(other.batches)),
            clears(std::move(other.clears)),owners(std::move(other.owners)){}
    };
    ComPtr<ID3D12Device> device;ComPtr<D3D12MA::Allocator> allocator;
    LocalShadowProfile profile;ViewExtent extent;UINT columns{},stride{},dsv_stride{};
    std::uint64_t allocated{},record_offset{64},draw_offset{},indexed_offset{},mesh_offset{},count_offset{},upload_bytes{};
    AllocationResource static_bank,dynamic_bank;
    ComPtr<ID3D12DescriptorHeap> srvs,dsvs;
    std::vector<Frame> frames;std::vector<CachedLight> cache;
    CompletionSet completion;std::uint64_t generation{};bool initialized{},usable{true};
    D3D12_CPU_DESCRIPTOR_HANDLE Srv(UINT frame,UINT offset)const
    {auto h=srvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(frame*5+offset)*stride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT index)const
    {auto h=dsvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*dsv_stride;return h;}
};
struct LocalShadowFramePlanData
{
    struct Scratch
    {
        std::byte *mapped{};
        std::vector<VirtualShadowDrawBatch> batches;
        std::array<std::vector<D3D12_RECT>,2> clears;
    } frame;
    const LocalShadowInputs *source{};
    std::shared_ptr<LocalShadowStorage> storage;
    const LightView *lights{};
    const VirtualShadowCaster *stable_casters{}, *dynamic_casters{};
    const VirtualShadowDrawGroup *groups{};
    std::size_t light_count{}, stable_count{}, dynamic_count{}, group_count{};
    DirectX::XMFLOAT3 camera_position{};
    std::uint64_t rendered_frame_id{};
    std::uint32_t frame_slot{};
    std::uint64_t record_generation{};
    bool reset_cache{};
    std::vector<LocalShadowStorage::CachedLight> cache;
    LocalShadowStatus status;
    std::vector<LocalShadowLightReference> light_records;
    std::array<bool,2> has_batches{};
    bool consumed{};
};
namespace
{
bool PreparedInputsMatch(const LocalShadowFramePlanData &data,
    const LocalShadowInputs &in) noexcept
{
    return data.source==&in&&data.lights==in.lights.data()&&
        data.light_count==in.lights.size()&&
        data.stable_casters==in.casters.stable.data()&&
        data.stable_count==in.casters.stable.size()&&
        data.dynamic_casters==in.casters.dynamic.data()&&
        data.dynamic_count==in.casters.dynamic.size()&&
        data.groups==in.draw_groups.data()&&data.group_count==in.draw_groups.size()&&
        data.rendered_frame_id==in.rendered_frame_id&&data.frame_slot==in.frame_slot&&
        data.reset_cache==in.reset_cache&&
        std::memcmp(&data.camera_position,&in.camera_position,sizeof(in.camera_position))==0;
}
Result Allocate(LocalShadowStorage &s,AllocationResource &resource,D3D12_RESOURCE_DESC desc,bool upload=false)
{
    D3D12MA::ALLOCATION_DESC ad{};ad.HeapType=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_D32_FLOAT;clear.DepthStencil.Depth=0;
    const auto hr=s.allocator->CreateResource(&ad,&desc,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COMMON,
        desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL?&clear:nullptr,&resource.allocation,
        IID_PPV_ARGS(resource.resource.ReleaseAndGetAddressOf()));
    if(FAILED(hr))return Native("Create local shadow resource",hr);
    resource.graph_access=upload?Access::CopySource:Access::Common;
    s.allocated+=resource.allocation->GetSize();
    if(s.allocated>s.profile.byte_budget)return Failure(ErrorCode::InvalidArgument,"Local shadow native allocations exceed byte_budget.");
    return Result::Success();
}
void RawSrv(LocalShadowStorage &s,ID3D12Resource *resource,D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Buffer.NumElements=UINT(resource->GetDesc().Width/4);
    d.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;s.device->CreateShaderResourceView(resource,&d,h);
}
void StructuredSrv(LocalShadowStorage &s,ID3D12Resource *resource,UINT stride,D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements=UINT(resource->GetDesc().Width/stride);d.Buffer.StructureByteStride=stride;s.device->CreateShaderResourceView(resource,&d,h);
}
GpuLocalShadowRecord Record(const LocalShadowStorage &s,const LightView &light)
{
    using namespace DirectX;GpuLocalShadowRecord record;
    record.identity={UINT(light.stable_id),UINT(light.stable_id>>32),light.generation,UINT(light.type)};
    record.revision={UINT(light.revision),UINT(light.revision>>32),SampleCount(light,s.profile),FaceCount(light,s.profile)};
    record.position_range={light.position.x,light.position.y,light.position.z,light.range};
    const bool area=Area(light),spot=light.type==LightType::Spot;
    const double length=area||spot?std::hypot(double(light.direction.x),double(light.direction.y),double(light.direction.z)):1.0;
    const auto direction=area||spot?XMVectorSet(float(light.direction.x/length),float(light.direction.y/length),float(light.direction.z/length),0):XMVectorSet(0,0,1,0);
    XMStoreFloat4(&record.direction_cos_outer,direction);record.direction_cos_outer.w=std::cos(light.outer_cone_radians);
    const float near_plane=std::max(0.005f,std::min(0.05f,light.range*0.001f));
    record.parameters={near_plane,s.profile.depth_bias,s.profile.normal_bias_m,float(s.profile.filter_samples)};
    for(UINT sample=0;sample<record.revision.z;++sample)
        record.sample_positions[sample]={light.position.x,light.position.y,light.position.z,0};
    if(area)
    {
        const auto &d=light.direction;const auto &t=light.area_tangent;
        const double bx=double(d.y)*t.z-double(d.z)*t.y,by=double(d.z)*t.x-double(d.x)*t.z,bz=double(d.x)*t.y-double(d.y)*t.x;
        const double cross_length=std::hypot(bx,by,bz),ux=bx/cross_length,uy=by/cross_length,uz=bz/cross_length;
        const double nx=d.x/length,ny=d.y/length,nz=d.z/length;
        const double tx=uy*nz-uz*ny,ty=uz*nx-ux*nz,tz=ux*ny-uy*nx;
        const double scale=light.type==LightType::Rectangle?0.5:1.0,width=light.shape.x*scale,height=light.shape.y*scale;
        record.emitter_normal_two_sided={float(nx),float(ny),float(nz),light.two_sided?1.0f:0.0f};
        // Fixed plane quadrature: the centroid, two opposite equal-area half
        // centroids, or four symmetric quadrant points. These are bounded
        // visibility samples, not a claim of exact area-light integration.
        for(UINT sample=0;sample<record.revision.z;++sample)
        {
            double x{},y{};
            if(record.revision.z==2)
            {
                const double offset=light.type==LightType::Rectangle?0.5:4.0/(3.0*std::numbers::pi_v<double>);
                if(width>=height)x=sample==0?-offset:offset;else y=sample==0?-offset:offset;
            }
            else if(record.revision.z==4)
            {
                const double offset=light.type==LightType::Rectangle?0.5:4.0/(3.0*std::numbers::pi_v<double>);
                x=(sample&1u)?offset:-offset;y=(sample&2u)?offset:-offset;
            }
            record.sample_positions[sample]={float(double(light.position.x)+tx*x*width+ux*y*height),
                float(double(light.position.y)+ty*x*width+uy*y*height),
                float(double(light.position.z)+tz*x*width+uz*y*height),0};
        }
    }
    const XMFLOAT3 directions[6]{{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    const XMFLOAT3 ups[6]{{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
    const float fov=spot?light.outer_cone_radians*2:XM_PIDIV2;
    // Swapping the native near/far arguments creates the established reversed Z.
    const float far_plane=float(double(light.range)+(area?EmitterBound(light):0.0));
    const auto projection=XMMatrixPerspectiveFovLH(fov,1,far_plane,near_plane);
    for(UINT face=0;face<record.revision.w;++face)
    {
        const UINT cube_face=face%6,sample=face/6;
        const auto position=XMLoadFloat4(&record.sample_positions[sample]);
        auto toward=spot?direction:XMLoadFloat3(&directions[cube_face]);
        auto up=!spot?XMLoadFloat3(&ups[cube_face]):
            (std::abs(XMVectorGetY(toward))>0.99f?XMVectorSet(0,0,1,0):XMVectorSet(0,1,0,0));
        XMStoreFloat4x4(&record.world_to_clip[face],XMMatrixLookToLH(position,toward,up)*projection);
    }
    record.state.y=1;return record;
}
UINT DirtyFaces(const LocalShadowStorage &s,const LocalShadowInputs &in,const GpuLocalShadowRecord &record,
    const LocalShadowStorage::CachedLight *cached,const LightView &light,float expanded)
{
    const UINT all=(1u<<record.revision.w)-1;
    UINT dirty=in.reset_cache||!cached||!SameLight(cached->light,light)||cached->face_count!=record.revision.w||!s.initialized?
        all:all&~cached->valid_faces;
    for(UINT face=0;face<record.revision.w;++face)
        if((dirty&(1u<<face))==0)for(const auto &c:in.casters)if(Changed(c)&&
            ((StaticCaster(c)&&Overlap(c.current_bounds,record.world_to_clip[face],expanded))||
             (StaticCaster(c,true)&&Overlap(c.previous_bounds,record.world_to_clip[face],expanded))))
            {dirty|=1u<<face;break;}
    return dirty;
}
bool CommandsFit(const LocalShadowInputs &in,const GpuLocalShadowRecord &record,UINT dirty,float expanded,
    UINT remaining,UINT &commands)
{
    commands=0;
    // Every present caster belongs to exactly one validated draw group and
    // one bank. Count its exact face commands before reserving any tile/slot.
    for(UINT face=0;face<record.revision.w;++face)for(const auto &c:in.casters)
        if(c.present&&(!StaticCaster(c)||(dirty&(1u<<face)))&&Overlap(c.current_bounds,record.world_to_clip[face],expanded))
        {if(commands==remaining)return false;++commands;}
    return true;
}
} // namespace

struct LocalShadowSystem::Impl
{
    struct Import{AllocationResource *resource;ResourceHandle handle;};
    std::shared_ptr<LocalShadowStorage> storage;
    std::uint64_t next_generation{};
    struct Pending{const RenderGraphBuilder *graph{};UINT slot{};std::vector<Import> imports;
        std::vector<LocalShadowStorage::CachedLight> cache;LocalShadowStatus status;
        std::vector<LocalShadowLightReference> light_records;}pending;
    LocalShadowStatus status;
};
LocalShadowSystem::LocalShadowSystem():impl_(std::make_unique<Impl>()){}
LocalShadowSystem::~LocalShadowSystem()=default;
std::uint64_t LocalShadowCandidate::AllocatedBytes()const noexcept{return storage_?storage_->allocated:0;}
std::uint64_t LocalShadowSystem::AllocatedBytes()const noexcept{return impl_->storage?impl_->storage->allocated:0;}
LocalShadowStatus LocalShadowSystem::Status()const{return impl_->status;}
Result LocalShadowSystem::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,const LocalShadowProfile &profile,
    const ShaderReader &,LocalShadowCandidate &candidate,RendererPipelineCache *)const
{
    candidate={};
    if(!profile.enabled){candidate.prepared_=true;return Result::Success();}
    if(!profile.filter_samples||profile.filter_samples>16)return Failure(ErrorCode::InvalidArgument,"Invalid local shadow filter sample count.");
    if(!device||!allocator||!profile.byte_budget||!profile.maximum_lights||profile.maximum_lights>64||
        profile.face_resolution<16||profile.face_resolution>4096||!profile.max_casters||!profile.max_draw_groups||
        !profile.command_capacity||profile.command_capacity>=kLocalShadowPassBit||!profile.frame_slots||profile.frame_slots>16||
        (profile.area_samples!=1&&profile.area_samples!=2&&profile.area_samples!=4)||
        !std::isfinite(profile.normal_bias_m)||profile.normal_bias_m<0||!std::isfinite(profile.depth_bias)||profile.depth_bias<0)
        return Failure(ErrorCode::InvalidArgument,"Invalid local shadow capacities/budget/bias.");
    auto s=std::make_shared<LocalShadowStorage>();s->device=device;s->allocator=allocator;s->profile=profile;
    // Raw HLSL loads use UINT byte addresses. Include the last Load4 range in
    // the proof instead of relying on the native structured-stride limit.
    const UINT64 record_bytes=UINT64(profile.maximum_lights)*sizeof(GpuLocalShadowRecord);
    if(record_bytes>UINT_MAX||record_bytes%4!=0)
        return Failure(ErrorCode::InvalidArgument,"Local shadow raw record addresses exceed the shader ABI.");
    const UINT tiles=profile.maximum_lights*6,tile_extent=profile.face_resolution+2*kLocalShadowGuard;
    s->columns=UINT(std::ceil(std::sqrt(double(tiles))));const UINT rows=(tiles+s->columns-1)/s->columns;
    if(UINT64(s->columns)*tile_extent>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION||UINT64(rows)*tile_extent>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        return Failure(ErrorCode::InvalidArgument,"Local shadow banks exceed the native texture limit.");
    s->extent={s->columns*tile_extent,rows*tile_extent};s->frames.resize(profile.frame_slots);s->cache.resize(profile.maximum_lights);
    s->draw_offset=s->record_offset+UINT64(profile.maximum_lights)*sizeof(GpuLocalShadowRecord);
    s->indexed_offset=s->draw_offset+UINT64(profile.command_capacity)*sizeof(GpuShadowDrawRecord);
    s->mesh_offset=s->indexed_offset+UINT64(profile.command_capacity)*sizeof(IndexedCommand);
    s->count_offset=s->mesh_offset+UINT64(profile.command_capacity)*sizeof(MeshCommand);
    s->upload_bytes=s->count_offset+UINT64(profile.command_capacity)*sizeof(UINT);
    D3D12_RESOURCE_DESC bank{};bank.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;bank.Width=s->extent.width;bank.Height=s->extent.height;
    bank.DepthOrArraySize=1;bank.MipLevels=1;bank.Format=DXGI_FORMAT_R32_TYPELESS;bank.SampleDesc.Count=1;bank.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if(auto r=Allocate(*s,s->static_bank,bank);!r)return r;if(auto r=Allocate(*s,s->dynamic_bank,bank);!r)return r;
    HS_RENDERER_SET_DEBUG_NAME(s->static_bank.resource.Get(), L"LocalShadow.StaticBank"); HS_RENDERER_SET_DEBUG_NAME(s->dynamic_bank.resource.Get(), L"LocalShadow.DynamicBank");
    for(auto &f:s->frames)
    {
        if(auto r=Allocate(*s,f.upload,Buffer(s->upload_bytes),true);!r)return r;
        const std::pair<AllocationResource *,UINT64> buffers[]{{&f.metadata,sizeof(LocalShadowMetadata)},
            {&f.records,UINT64(profile.maximum_lights)*sizeof(GpuLocalShadowRecord)},
            {&f.draws,UINT64(profile.command_capacity)*sizeof(GpuShadowDrawRecord)},
            {&f.indexed,UINT64(profile.command_capacity)*sizeof(IndexedCommand)},
            {&f.mesh,UINT64(profile.command_capacity)*sizeof(MeshCommand)},
            {&f.counts,UINT64(profile.command_capacity)*sizeof(UINT)}};
        for(auto [r,bytes]:buffers)if(auto result=Allocate(*s,*r,Buffer(bytes));!result)return result;
        void *mapped{};D3D12_RANGE empty{};auto hr=f.upload.resource->Map(0,&empty,&mapped);
        if(FAILED(hr))return Native("Map local shadow upload",hr);f.mapped=static_cast<std::byte *>(mapped);
        f.prepared_upload.resize(static_cast<std::size_t>(s->upload_bytes));
    }
    s->stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s->dsv_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    s->allocated+=UINT64(profile.frame_slots)*5*s->stride+2*s->dsv_stride;
    if(s->allocated>profile.byte_budget)return Failure(ErrorCode::InvalidArgument,"Local shadow descriptor backing exceeds byte_budget.");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=profile.frame_slots*5;
    auto hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s->srvs));if(FAILED(hr))return Native("Create local shadow SRVs",hr);
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=2;hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&s->dsvs));
    if(FAILED(hr))return Native("Create local shadow DSVs",hr);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(s->static_bank.resource.Get(),&dsv,s->Dsv(0));device->CreateDepthStencilView(s->dynamic_bank.resource.Get(),&dsv,s->Dsv(1));
    D3D12_SHADER_RESOURCE_VIEW_DESC texture{};texture.Format=DXGI_FORMAT_R32_FLOAT;texture.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    texture.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;texture.Texture2D.MipLevels=1;
    for(UINT slot=0;slot<profile.frame_slots;++slot)
    {
        auto &f=s->frames[slot];RawSrv(*s,f.metadata.resource.Get(),s->Srv(slot,0));RawSrv(*s,f.records.resource.Get(),s->Srv(slot,1));
        device->CreateShaderResourceView(s->static_bank.resource.Get(),&texture,s->Srv(slot,2));
        device->CreateShaderResourceView(s->dynamic_bank.resource.Get(),&texture,s->Srv(slot,3));
        StructuredSrv(*s,f.draws.resource.Get(),32,s->Srv(slot,4));
    }
    candidate.storage_=std::move(s);candidate.prepared_=true;return Result::Success();
}
Result LocalShadowSystem::Commit(LocalShadowCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.prepared_||impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Local shadow replacement requires a prepared candidate and all queue readers completed.");
    if(candidate.storage_&&impl_->next_generation==UINT64_MAX)
        return Failure(ErrorCode::InvalidState,"Local shadow storage generation is exhausted.");
    if(candidate.storage_)candidate.storage_->generation=++impl_->next_generation;
    impl_->storage=std::move(candidate.storage_);impl_->status={};impl_->status.enabled=bool(impl_->storage);
    impl_->status.allocated_bytes=AllocatedBytes();candidate.prepared_=false;return Result::Success();
}
Result LocalShadowSystem::PrepareFrameRecordData(const LocalShadowInputs &in,
    LocalShadowFramePlan &plan)
{
    plan={};auto s=impl_->storage;if(!s)return Result::Success();
    if(!s->usable||impl_->pending.graph)return Failure(ErrorCode::InvalidState,"Local shadows require completion and reset after a failed graph.");
    if(in.frame_slot>=s->frames.size()||!s->frames[in.frame_slot].completion.IsSatisfiedBy(in.completed)||!in.draw||
        in.casters.size()>s->profile.max_casters||in.draw_groups.size()>s->profile.max_draw_groups)
        return Failure(ErrorCode::InvalidArgument,"Local shadow caster/draw capacity or completed frame slot is invalid.");
    for(const auto &c:in.casters)if((c.present&&(!Sphere(c.current_bounds)||c.draw_group>=in.draw_groups.size()))||
        (c.previous_present&&!Sphere(c.previous_bounds)))return Failure(ErrorCode::InvalidArgument,"Local shadow caster bounds/group invalid.");
    auto data=std::make_shared<LocalShadowFramePlanData>();
    data->source=&in;data->storage=s;
    auto &native_frame=s->frames[in.frame_slot];
    if(native_frame.record_generation==UINT64_MAX)
        return Failure(ErrorCode::InvalidState,"Local shadow frame record generation is exhausted.");
    data->record_generation=++native_frame.record_generation;
    data->lights=in.lights.data();data->light_count=in.lights.size();
    data->stable_casters=in.casters.stable.data();data->stable_count=in.casters.stable.size();
    data->dynamic_casters=in.casters.dynamic.data();data->dynamic_count=in.casters.dynamic.size();
    data->groups=in.draw_groups.data();data->group_count=in.draw_groups.size();
    data->camera_position=in.camera_position;data->rendered_frame_id=in.rendered_frame_id;
    data->frame_slot=in.frame_slot;data->reset_cache=in.reset_cache;
    data->frame.mapped=native_frame.prepared_upload.data();
    LocalShadowStatus status;status.enabled=true;status.allocated_bytes=s->allocated;
    struct Ranked{const LightView *light;double priority;};std::vector<Ranked> ranked;
    for(const auto &light:in.lights)
    {
        if(light.type==LightType::Directional||(light.shadow_flags&kLightShadowRequested)==0)continue;
        ++status.requested_lights;if(!Supported(light)){++status.unsupported_lights;continue;}
        if(std::ranges::any_of(ranked,[&](const auto &r){return r.light->stable_id==light.stable_id&&r.light->generation==light.generation;}))
            return Failure(ErrorCode::InvalidArgument,"Duplicate full local light identity.");
        const auto old=std::ranges::find_if(s->cache,[&](const auto &cached){return cached.occupied&&cached.light.stable_id==light.stable_id&&cached.light.generation==light.generation;});
        ranked.push_back({&light,Priority(light,in.camera_position,old==s->cache.end()||!SameLight(old->light,light))});
    }
    std::ranges::sort(ranked,[](const auto &a,const auto &b){return a.priority!=b.priority?a.priority>b.priority:
        std::tie(a.light->stable_id,a.light->generation)<std::tie(b.light->stable_id,b.light->generation);});
    const float expanded=1.0f+2.0f*kLocalShadowGuard/s->profile.face_resolution;
    const UINT physical_tiles=s->profile.maximum_lights*6;
    struct Selected{const LightView *light;GpuLocalShadowRecord record;UINT old_slot,slot,dirty,commands;};
    std::vector<Selected> selected;UINT reserved_tiles{},reserved_commands{};
    for(const auto &rank:ranked)
    {
        const auto &light=*rank.light;const UINT faces=FaceCount(light,s->profile);
        if(selected.size()==s->cache.size()||faces>physical_tiles-reserved_tiles){++status.budget_lights;continue;}
        const auto old=std::ranges::find_if(s->cache,[&](const auto &c){return c.occupied&&
            c.light.stable_id==light.stable_id&&c.light.generation==light.generation;});
        const UINT old_slot=old==s->cache.end()?UINT_MAX:UINT(old-s->cache.begin());
        auto record=Record(*s,light);const UINT dirty=DirtyFaces(*s,in,record,old==s->cache.end()?nullptr:&*old,light,expanded);
        UINT commands{};
        if(!CommandsFit(in,record,dirty,expanded,s->profile.command_capacity-reserved_commands,commands))
        {++status.overflow_lights;continue;}
        selected.push_back({&light,record,old_slot,UINT_MAX,dirty,commands});reserved_tiles+=faces;reserved_commands+=commands;
    }
    // First reserve stable record slots and the exact tiles of every surviving
    // owner. New higher-priority lights can evict unselected owners, but cannot
    // displace a surviving light merely because it appears later in the ranking.
    auto cache=s->cache;for(auto &c:cache)c.selected=false;
    for(auto &choice:selected)if(choice.old_slot!=UINT_MAX)
    {choice.slot=choice.old_slot;cache[choice.slot].selected=true;}
    for(auto &choice:selected)if(choice.slot==UINT_MAX)
        for(UINT slot=0;slot<cache.size();++slot)if(!cache[slot].selected)
        {choice.slot=slot;cache[slot]={};cache[slot].selected=true;break;}
    std::vector<bool> used_tiles(physical_tiles);
    for(auto &choice:selected)
    {
        auto &c=cache[choice.slot];
        if(choice.old_slot==UINT_MAX||c.face_count!=choice.record.revision.w)
        {c.face_count=0;c.face_tiles.fill(UINT_MAX);continue;}
        for(UINT face=0;face<c.face_count;++face)
        {
            const UINT tile=c.face_tiles[face];
            if(tile>=physical_tiles||used_tiles[tile])
                return Failure(ErrorCode::InvalidState,"Local shadow cached physical tile ownership is invalid.");
            used_tiles[tile]=true;
        }
    }
    for(auto &choice:selected)
    {
        auto &c=cache[choice.slot];
        if(c.face_count==0)
        {
            UINT tile{};
            for(UINT face=0;face<choice.record.revision.w;++face)
            {
                while(tile<physical_tiles&&used_tiles[tile])++tile;
                if(tile==physical_tiles)return Failure(ErrorCode::InvalidState,"Local shadow preflight did not reserve the entire face bundle.");
                c.face_tiles[face]=tile;used_tiles[tile]=true;
            }
            c.face_count=choice.record.revision.w;
        }
        const UINT tile_extent=s->profile.face_resolution+2*kLocalShadowGuard;
        for(UINT face=0;face<c.face_count;++face)
        {
            const UINT tile=c.face_tiles[face];
            choice.record.atlas_tiles[face]={float((tile%s->columns)*tile_extent),float((tile/s->columns)*tile_extent),
                float(s->profile.face_resolution),float(kLocalShadowGuard)};
        }
    }
    std::vector<GpuLocalShadowRecord> records(cache.size());std::vector<GpuShadowDrawRecord> draws;
    std::vector<IndexedCommand> indexed;std::vector<MeshCommand> mesh;std::vector<UINT> counts;
    auto &frame=data->frame;frame.batches.clear();for(auto &clears:frame.clears)clears.clear();
    for(const auto &choice:selected)
    {
        const auto slot=choice.slot;const auto &light=*choice.light;auto &cached=cache[slot];
        auto record=choice.record;const UINT dirty=choice.dirty;
        const UINT all=(1u<<record.revision.w)-1;
        const auto first_draw=draws.size();
        for(UINT layer=0;layer<2;++layer)for(UINT face=0;face<record.revision.w;++face)
        {
            if(layer==0&&(dirty&(1u<<face))==0)continue;
            for(UINT group=0;group<in.draw_groups.size();++group)
            {
                const auto begin=draws.size();
                for(const auto &c:in.casters)if(c.present&&c.draw_group==group&&UINT(!StaticCaster(c))==layer&&Overlap(c.current_bounds,record.world_to_clip[face],expanded))
                {
                    if(draws.size()>=s->profile.command_capacity)
                        return Failure(ErrorCode::InvalidState,"Local shadow emitted commands exceed their transactional preflight.");
                    const auto index=UINT(draws.size());
                    draws.push_back({c.geometry_record,c.source_ref,c.instance_index,c.instance_generation,slot,layer,group,face});
                    const auto &group_data=in.draw_groups[group];
                    indexed.push_back({kLocalShadowPassBit|index,{group_data.index_count,1,
                        group_data.first_index,group_data.base_vertex,0}});
                    mesh.push_back({kLocalShadowPassBit|index,{group_data.meshlet_count,1,1}});
                }
                if(draws.size()!=begin)
                {
                    const auto command_count=UINT(draws.size()-begin);const bool mesh_group=in.draw_groups[group].frontend==VirtualShadowFrontend::Mesh;
                    frame.batches.push_back({in.draw_groups[group],group,layer,command_count,
                        UINT64(begin)*(mesh_group?sizeof(MeshCommand):sizeof(IndexedCommand)),UINT64(counts.size())*4});counts.push_back(command_count);
                }
            }
        }
        if(draws.size()-first_draw!=choice.commands)
            return Failure(ErrorCode::InvalidState,"Local shadow command emission differs from its admitted face bundle.");
        cached.light=light;cached.occupied=true;
        for(UINT face=0;face<record.revision.w;++face)
        {
            const auto tile=record.atlas_tiles[face];const D3D12_RECT rect{LONG(tile.x),LONG(tile.y),
                LONG(tile.x+tile.z+2*tile.w),LONG(tile.y+tile.z+2*tile.w)};
            if(dirty&(1u<<face))frame.clears[0].push_back(rect);frame.clears[1].push_back(rect);
        }
        cached.valid_faces=all;record.state.x=all;++status.selected_lights;
        records[slot]=record;
    }
    for(auto &c:cache)if(!c.selected){c.valid_faces=0;c.face_count=0;c.face_tiles.fill(UINT_MAX);} // No stale cache or tile ownership after an unobserved frame.
    if(status.unsupported_lights)status.reason="Requested local raster shadow input has unsupported type or invalid authored emitter geometry.";
    if(status.budget_lights||status.overflow_lights)status.reason+=(status.reason.empty()?"":" ")+std::string("Unselected or command-overflow lights keep unshadowed lighting within the explicit budget.");
    LocalShadowMetadata metadata;metadata.counts={UINT(records.size()),status.selected_lights,s->profile.maximum_lights*6,1};
    metadata.bank={float(s->extent.width),float(s->extent.height),float(s->columns),float(s->profile.face_resolution+2*kLocalShadowGuard)};
    metadata.options={float(s->profile.face_resolution),float(kLocalShadowGuard),s->profile.depth_bias,s->profile.normal_bias_m};
    metadata.frame={UINT(in.rendered_frame_id),UINT(in.rendered_frame_id>>32),status.overflow_lights,status.unsupported_lights};
    std::memcpy(frame.mapped,&metadata,sizeof(metadata));std::memcpy(frame.mapped+s->record_offset,records.data(),records.size()*sizeof(records[0]));
    if(!draws.empty()){std::memcpy(frame.mapped+s->draw_offset,draws.data(),draws.size()*sizeof(draws[0]));
        std::memcpy(frame.mapped+s->indexed_offset,indexed.data(),indexed.size()*sizeof(indexed[0]));
        std::memcpy(frame.mapped+s->mesh_offset,mesh.data(),mesh.size()*sizeof(mesh[0]));}
    if(!counts.empty())std::memcpy(frame.mapped+s->count_offset,counts.data(),counts.size()*sizeof(counts[0]));
    data->cache=std::move(cache);data->status=status;
    data->light_records.reserve(selected.size());
    for(const auto &choice:selected)
        data->light_records.push_back({choice.light->stable_id,choice.light->revision,
            choice.light->generation,choice.slot,choice.light->type});
    // Complete the unused portions in the prepared value before publishing it
    // to a waited native frame bank.
    std::memset(frame.mapped+s->draw_offset+draws.size()*32,0,std::size_t(s->indexed_offset-s->draw_offset-draws.size()*32));
    std::memset(frame.mapped+s->indexed_offset+indexed.size()*24,0,std::size_t(s->mesh_offset-s->indexed_offset-indexed.size()*24));
    std::memset(frame.mapped+s->mesh_offset+mesh.size()*16,0,std::size_t(s->count_offset-s->mesh_offset-mesh.size()*16));
    std::memset(frame.mapped+s->count_offset+counts.size()*4,0,std::size_t(s->upload_bytes-s->count_offset-counts.size()*4));
    for(const auto &batch:frame.batches)data->has_batches[batch.layer]=true;
    plan.has_batches=data->has_batches;plan.data=std::move(data);
    return Result::Success();
}
Result LocalShadowSystem::Register(RenderGraphBuilder &graph,const LocalShadowInputs &in,
    LocalShadowOutputs &out,LocalShadowLayout *layout)
{
    LocalShadowFramePlan plan;
    if(auto prepared=PrepareFrameRecordData(in,plan);!prepared)return prepared;
    return RegisterImpl(graph,in,plan,out,layout,nullptr);
}
Result LocalShadowSystem::Register(RenderGraphBuilder &graph,const LocalShadowInputs &in,
    LocalShadowFramePlan &plan,LocalShadowOutputs &out,LocalShadowLayout *layout)
{return RegisterImpl(graph,in,plan,out,layout,nullptr);}
Result LocalShadowSystem::BindRetained(RenderGraphBuilder &graph,const LocalShadowInputs &in,
    LocalShadowFramePlan &plan,const LocalShadowLayout &layout,LocalShadowOutputs &out)
{return RegisterImpl(graph,in,plan,out,nullptr,&layout);}
bool LocalShadowSystem::MatchesLayout(const LocalShadowInputs &in,
    const LocalShadowFramePlan &plan,const LocalShadowLayout &layout) const noexcept
{
    const auto s=impl_->storage;
    if(!s||!s->usable||!plan.data||!PreparedInputsMatch(*plan.data,in)||
        plan.data->storage!=s||plan.data->consumed||
        in.frame_slot>=s->frames.size()||
        plan.data->record_generation!=s->frames[in.frame_slot].record_generation||
        layout.storage_generation!=s->generation||
        layout.initialized!=s->initialized||
        layout.material_sampling!=bool(in.declare_material_sampling)||
        layout.native_reads.size()!=in.native_reads.size())return false;
    for(std::size_t index=0;index<in.native_reads.size();++index)
        if(layout.native_reads[index].version!=in.native_reads[index].version||
            layout.native_reads[index].access!=in.native_reads[index].access)return false;
    for(UINT layer=0;layer<2;++layer)
        if(layout.sampled_layers[layer]!=(plan.has_batches[layer]&&bool(in.draw)&&
            bool(in.declare_material_sampling)))return false;
    return true;
}
Result LocalShadowSystem::RegisterImpl(RenderGraphBuilder &graph,const LocalShadowInputs &in,
    LocalShadowFramePlan &plan,LocalShadowOutputs &out,LocalShadowLayout *layout,
    const LocalShadowLayout *retained)
{
    out={};auto s=impl_->storage;if(!s)return Result::Success();
    auto data=plan.data;
    if(!data||!PreparedInputsMatch(*data,in)||data->storage!=s||data->consumed||
        in.frame_slot>=s->frames.size()||!s->frames[in.frame_slot].completion.IsSatisfiedBy(in.completed)||
        data->record_generation!=s->frames[in.frame_slot].record_generation||
        !s->usable||impl_->pending.graph)
        return Failure(ErrorCode::InvalidState,"Local shadow frame record plan changed or is already used.");
    if(retained)
    {
        if(!MatchesLayout(in,plan,*retained))
            return Failure(ErrorCode::InvalidState,"Local shadow retained topology changed.");
        for(UINT index=0;index<4;++index)
            if(!graph.BindPass(retained->pass_indices[index],220+index))
                return Failure(ErrorCode::InvalidState,"Local shadow retained pass changed.");
        for(UINT index=0;index<retained->imports.size();++index)
            if(!retained->imports[index]||
                retained->imports[index].value>graph.ResourceCount()||
                retained->imports[index].kind!=(index<7?ResourceKind::Buffer:ResourceKind::Texture))
                return Failure(ErrorCode::InvalidState,"Local shadow retained import slot changed.");
    }
    for(const auto &read:in.native_reads)if(!read.version.resource||!graph.Resolve(read.version.resource))
        return Failure(ErrorCode::InvalidArgument,"Local shadow canonical geometry/material graph read is missing.");
    for(auto *resource:{&s->frames[in.frame_slot].upload,&s->frames[in.frame_slot].metadata,
            &s->frames[in.frame_slot].records,&s->frames[in.frame_slot].draws,
            &s->frames[in.frame_slot].indexed,&s->frames[in.frame_slot].mesh,
            &s->frames[in.frame_slot].counts,&s->static_bank,&s->dynamic_bank})
        if(auto certified=resource->EnsureNativeContract(s->device.Get());!certified)return certified;
    if(layout)
    {
        *layout={};layout->storage_generation=s->generation;
        layout->initialized=s->initialized;
        layout->material_sampling=bool(in.declare_material_sampling);
        layout->native_reads.assign(in.native_reads.begin(),in.native_reads.end());
        for(UINT layer=0;layer<2;++layer)
            layout->sampled_layers[layer]=plan.has_batches[layer]&&bool(in.draw)&&
                bool(in.declare_material_sampling);
    }
    auto &frame=s->frames[in.frame_slot];
    std::memcpy(frame.mapped,data->frame.mapped,static_cast<std::size_t>(s->upload_bytes));
    frame.batches=std::move(data->frame.batches);frame.clears=std::move(data->frame.clears);
    auto &pending=impl_->pending;pending={};pending.graph=&graph;pending.slot=in.frame_slot;
    pending.cache=std::move(data->cache);pending.status=std::move(data->status);
    pending.light_records=std::move(data->light_records);out.light_records=pending.light_records;
    data->consumed=true;
    bool imports_valid=true;UINT import_index{};
    auto import=[&](AllocationResource &r,std::string_view name,bool texture=false)
    {
        const UINT index=import_index++;
        const auto handle=retained?retained->imports[index]:
            (texture?graph.ImportTexture({r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access,name):
                graph.ImportBuffer({r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access,name));
        if(retained)
            imports_valid&=texture?graph.BindTexture(handle,{r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access):
                graph.BindBuffer(handle,{r.resource.Get(),r.graph_completion,r.graph_queue,{},r.NativeContract()},r.graph_access);
        if(layout)layout->imports[index]=handle;
        pending.imports.push_back({&r,handle});return graph.Version(handle);
    };
    auto upload=import(frame.upload,"LocalShadow.Upload"),meta=import(frame.metadata,"LocalShadow.Metadata"),record=import(frame.records,"LocalShadow.Records"),
        draw=import(frame.draws,"LocalShadow.DrawRecords"),index=import(frame.indexed,"LocalShadow.Indexed"),mesh_args=import(frame.mesh,"LocalShadow.Mesh"),
        count=import(frame.counts,"LocalShadow.Counts"),statics=import(s->static_bank,"LocalShadow.StaticBank",true),dynamics=import(s->dynamic_bank,"LocalShadow.DynamicBank",true);
    if(!imports_valid)return Failure(ErrorCode::InvalidState,"Local shadow retained resource binding changed.");
    if(layout)layout->pass_indices[0]=graph.PassCount();
    auto copy=retained?*graph.BindPass(retained->pass_indices[0],220):
        graph.AddPass(220,"LocalShadow.UploadCommands",QueueHint::Copy);
    if(!retained)
    {
        copy.Read(upload,Access::CopySource);
        meta=copy.Write(meta,Access::CopyDest);record=copy.Write(record,Access::CopyDest);
        draw=copy.Write(draw,Access::CopyDest);index=copy.Write(index,Access::CopyDest);
        mesh_args=copy.Write(mesh_args,Access::CopyDest);count=copy.Write(count,Access::CopyDest);
    }
    else
    {
        meta=retained->outputs[0];record=retained->outputs[1];draw=retained->outputs[2];
        index=retained->outputs[3];mesh_args=retained->outputs[4];count=retained->outputs[5];
    }
    copy.SetExecute([s,slot=in.frame_slot](RenderPassContext &context)
    {auto *cmd=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());auto &f=s->frames[slot];
        const std::pair<AllocationResource *,UINT64> copies[]{{&f.metadata,0},{&f.records,s->record_offset},{&f.draws,s->draw_offset},
            {&f.indexed,s->indexed_offset},{&f.mesh,s->mesh_offset},{&f.counts,s->count_offset}};
        for(auto [r,offset]:copies)cmd->CopyBufferRegion(r->resource.Get(),0,f.upload.resource.Get(),offset,r->resource->GetDesc().Width);});
    out.metadata=meta;out.records=record;out.draw_records=draw;out.indexed_commands=index;out.mesh_commands=mesh_args;out.command_counts=count;
    out.resources={frame.metadata.resource.Get(),frame.records.resource.Get(),s->static_bank.resource.Get(),s->dynamic_bank.resource.Get(),frame.draws.resource.Get()};
    for(UINT i=0;i<5;++i)out.srvs[i]=s->Srv(in.frame_slot,i);out.depth_views={s->Dsv(0),s->Dsv(1)};
    out.indexed_arguments=frame.indexed.resource.Get();out.mesh_arguments=frame.mesh.resource.Get();out.counts_resource=frame.counts.resource.Get();
    out.bank_extent=s->extent;out.descriptor_count=5;
    for(UINT layer=0;layer<2;++layer)
    {
        if(layout)layout->pass_indices[1+layer]=graph.PassCount();
        auto pass=retained?*graph.BindPass(retained->pass_indices[1+layer],221+layer):
            graph.AddPass(221+layer,layer?"LocalShadow.DynamicBank":"LocalShadow.StaticBank",QueueHint::Direct);
        if(!retained)
        {
            for(auto version:{meta,record,draw})pass.Read(version,Access::ShaderRead);
            for(auto version:{index,mesh_args,count})pass.Read(version,Access::IndirectArgs);
            for(const auto &r:in.native_reads)pass.Read(r.version,r.access);
            if(plan.has_batches[layer]&&in.declare_material_sampling)
                in.declare_material_sampling(pass);
        }
        auto &bank=layer?dynamics:statics;
        bank=retained?retained->outputs[6+layer]:
            (s->initialized?pass.ReadWrite(bank,Access::DepthWrite):pass.Write(bank,Access::DepthWrite));
        pass.SetExecuteChecked([s,slot=in.frame_slot,layer,callback=in.draw,outputs=out,initialize=!s->initialized](RenderPassContext &context)->Result
        {
            auto *cmd=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());const auto &f=s->frames[slot];
            // Rectangle clears cannot initialize a new placed depth subresource.
            if(initialize)cmd->ClearDepthStencilView(outputs.depth_views[layer],D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
            else for(const auto &rect:f.clears[layer])cmd->ClearDepthStencilView(outputs.depth_views[layer],D3D12_CLEAR_FLAG_DEPTH,0,0,1,&rect);
            std::vector<VirtualShadowDrawBatch> batches;for(const auto &b:f.batches)if(b.layer==layer)batches.push_back(b);
            if(batches.empty())return Result::Success();
            const auto dsv=outputs.depth_views[layer];cmd->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
            const D3D12_VIEWPORT viewport{0,0,float(s->extent.width),float(s->extent.height),0,1};
            const D3D12_RECT rect{0,0,LONG(s->extent.width),LONG(s->extent.height)};
            cmd->RSSetViewports(1,&viewport);cmd->RSSetScissorRects(1,&rect);return callback(context,outputs,batches);
        });
    }
    if(layout)layout->pass_indices[3]=graph.PassCount();
    auto ready=retained?*graph.BindPass(retained->pass_indices[3],223):
        graph.AddPass(223,"LocalShadow.ShadowDataReady",QueueHint::Direct);
    if(!retained)for(auto version:{meta,record,statics,dynamics,draw})
        ready.Read(version,Access::AllShaderRead);
    ready.SetExecute([](RenderPassContext &){});out.static_bank=statics;out.dynamic_bank=dynamics;
    if(layout)layout->outputs={meta,record,draw,index,mesh_args,count,statics,dynamics};
    frame.owners=in.owners;return Result::Success();
}
Result LocalShadowSystem::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success)
{
    auto &p=impl_->pending;if(!p.graph)return Result::Success();
    if(p.graph!=&graph)return Failure(ErrorCode::InvalidArgument,"Wrong local shadow completion graph.");
    auto s=impl_->storage;auto &frame=s->frames[p.slot];s->completion.Merge(completion);frame.completion.Merge(completion);
    for(auto &i:p.imports)
    {
        const auto lease=graph.ResourceCompletion(i.handle);s->completion.Merge(lease);frame.completion.Merge(lease);
        i.resource->graph_completion.Merge(completion);i.resource->graph_completion.Merge(lease);
        if(success){i.resource->graph_access=graph.FinalAccess(i.handle);i.resource->graph_queue=graph.FinalQueue(i.handle);}
    }
    if(success){s->cache=std::move(p.cache);s->initialized=true;impl_->status=std::move(p.status);}
    else{s->usable=false;impl_->status.reason="Local shadow partial submission requires drain and fresh preparation.";}
    p={};return Result::Success();
}
Result LocalShadowSystem::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending.graph||(impl_->storage&&!impl_->storage->completion.IsSatisfiedBy(completed)))
        return Failure(ErrorCode::InvalidState,"Local shadow reset requires every Direct/Compute/Copy reader completed.");
    impl_->storage.reset();impl_->status={};return Result::Success();
}
} // namespace hs
