#include "renderer_local_probe_runtime.hpp"
#include "renderer_impl.hpp"
#include "renderer_pipeline_cache.hpp"
#include "renderer_quality.hpp"

#include <hs/core/sha256.hpp>
#include <hs/render_data/lighting_probe.hpp>

#include <bit>
#include <fstream>
#include <map>
#include <limits>
#include <stdexcept>

namespace hs
{
namespace
{
using Microsoft::WRL::ComPtr;
using renderer_detail::FrameConstants;
namespace rd=render_data;
constexpr std::string_view kProduct="render/lighting/local_probes.spkg";
constexpr std::string_view kEnvironment="render/lighting/default_probe.rpkg";
constexpr UINT kFaceCount=6;
constexpr UINT kConstantBankCount=kFaceCount+1, kShadowEdge=512;
constexpr UINT kCaptureDescriptorCount=renderer_detail::kHeroTransparencyDescriptorOffset+kHeroTransparencyTableSize;
constexpr UINT kConstantsStride=(sizeof(FrameConstants)+255u)&~255u;
Result Failure(std::string message)
{ return Result::Failure(ErrorCode::InvalidState,"hs_renderer_local_probe_runtime",std::move(message)); }
void Require(bool valid,std::string_view message)
{ if(!valid) throw std::runtime_error(std::string(message)); }
void Native(HRESULT result,std::string_view operation)
{ Require(SUCCEEDED(result),std::string(operation)+" failed (HRESULT "+std::to_string(std::int32_t(result))+")."); }
std::string Hex(std::span<const std::byte> bytes)
{
    constexpr char digits[]="0123456789abcdef"; std::string result; result.reserve(bytes.size()*2);
    for(auto b:bytes) { const auto value=std::to_integer<unsigned>(b); result+=digits[value>>4]; result+=digits[value&15]; }
    return result;
}
std::string Hash(std::span<const std::byte> bytes) { return "sha256:" + Hex(hs::Sha256(bytes)); }
std::vector<std::byte> ReadProduct(const RendererConfig &config,std::string_view id)
{
    std::filesystem::path path; const auto resolved=config.ResolveProduct(id,path);
    Require(static_cast<bool>(resolved),resolved.Message());
    const auto found=config.product_digests.find(std::string(id));
    Require(found!=config.product_digests.end(),"Generation omits required product digest: "+std::string(id));
    std::ifstream stream(path,std::ios::binary|std::ios::ate);
    Require(static_cast<bool>(stream),"Cannot open Generation probe/shader product: "+std::string(id));
    const auto count=stream.tellg(); Require(count>0 && count<=256ll*1024*1024,"Invalid bounded probe/shader product size");
    std::vector<std::byte> bytes(static_cast<std::size_t>(count)); stream.seekg(0);
    Require(static_cast<bool>(stream.read(reinterpret_cast<char *>(bytes.data()),static_cast<std::streamsize>(bytes.size()))),
        "Cannot read exact Generation probe/shader product bytes");
    Require(Hash(bytes)==found->second,"Generation probe/shader digest mismatch: "+std::string(id)); return bytes;
}
class Identity
{
  public:
    void Integer(std::uint64_t value,std::size_t count=8)
    {
        std::array<std::byte,8> encoded{};
        for(std::size_t i=0;i<count;++i) encoded[i]=std::byte((value>>(8*i))&255);
        hash_.Add(std::span(encoded).first(count));
    }
    void Float(float value)
    { Require(std::isfinite(value),"Nonfinite actual native capture input"); Integer(std::bit_cast<std::uint32_t>(value),4); }
    void Vector(Float3 value) { Float(value.x); Float(value.y); Float(value.z); }
    void Vector(const DirectX::XMFLOAT4 &v) { Float(v.x); Float(v.y); Float(v.z); Float(v.w); }
    void Text(std::string_view value)
    { Integer(value.size()); hash_.Add(std::as_bytes(std::span(value.data(),value.size()))); }
    std::string Finish() { return "sha256:" + Hex(hash_.Finish()); }
  private:
    hs::Sha256Stream hash_;
};
using Identities=std::map<std::string,std::string>;
std::uint64_t Revision(rd::SceneProbeRevisionDomain domain,const Identities &values)
{
    std::vector<rd::SceneProbeRevisionInput> inputs; inputs.reserve(values.size());
    for(const auto &[id,digest]:values)
    {
        Require(digest.starts_with("sha256:") && digest.size()==71,"Invalid scene revision SHA256: "+id);
        inputs.push_back({id,std::string_view(digest).substr(7)});
    }
    return rd::DeriveSceneProbeRevision(domain,inputs);
}
std::uint32_t MaterialOrdinal(const RenderInstance &instance)
{
    switch(instance.mesh)
    {
    case RenderMesh::TreeTrunk: Require(instance.environment_variant<3,"Invalid static trunk variant"); return instance.environment_variant;
    case RenderMesh::TreeCanopy: Require(instance.environment_variant<3,"Invalid static canopy variant"); return 3+instance.environment_variant;
    case RenderMesh::Rock: Require(instance.environment_variant<4,"Invalid static rock variant"); return 6+instance.environment_variant;
    case RenderMesh::Grass: Require(instance.environment_variant<4,"Invalid static grass variant"); return 10+instance.environment_variant;
    case RenderMesh::Ground: case RenderMesh::DirtPatch: return 14;
    default: throw std::runtime_error("Dynamic/unsupported mesh in immutable local-probe static scene");
    }
}
bool Masked(std::size_t ordinal) { return (ordinal>=3 && ordinal<6) || (ordinal>=10 && ordinal<14); }
void ValidateBounds(const rd::GeometryBounds &bounds)
{
    for(std::size_t axis=0;axis<3;++axis)
        Require(std::isfinite(bounds.minimum[axis]) && std::isfinite(bounds.maximum[axis]) &&
            bounds.minimum[axis]<=bounds.maximum[axis],"Complete capture page has invalid local geometry bounds");
    for(float value:bounds.sphere) Require(std::isfinite(value),"Complete capture page has nonfinite bounding sphere");
    Require(bounds.sphere[3]>=0,"Complete capture page has a negative bounding radius");
}
struct StaticShadowView
{
    LocalProbeCaptureView view;
    std::uint64_t light_id{};
    std::uint32_t light_generation{};
    bool enabled{};
};
StaticShadowView MakeStaticShadowView(const std::shared_ptr<const StaticRenderScene> &scene,
    std::span<const LocalProbeCapturePage> pages,std::span<const LightView> lights)
{
    using namespace DirectX;
    StaticShadowView result;
    const LightView *primary=nullptr;
    for(const auto &light:lights)
        if(light.type==LightType::Directional && (light.shadow_flags&kLightShadowRequested) &&
            (!primary || light.stable_id<primary->stable_id)) primary=&light;
    if(!primary || scene->instances.empty()) return result;
    Require(primary->stable_id && primary->generation && primary->revision,"Static capture shadow owner has invalid identity");
    const auto direction=XMVectorSet(primary->direction.x,primary->direction.y,primary->direction.z,0);
    const float length=XMVectorGetX(XMVector3Length(direction));
    Require(std::isfinite(length) && length>0,"Static capture shadow owner has invalid propagation direction");
    const auto forward=XMVectorScale(direction,1.0f/length);
    const auto up=std::abs(XMVectorGetY(forward))>.95f ? XMVectorSet(0,0,1,0) : XMVectorSet(0,1,0,0);
    // Rotation-only light coordinates support arbitrary absolute world positions.
    // Bounds and orthographic near/far below supply all scene translation.
    const auto light_view=XMMatrixLookToLH(XMVectorZero(),forward,up);
    XMFLOAT3 minimum{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
    XMFLOAT3 maximum{-minimum.x,-minimum.y,-minimum.z};
    for(const auto &instance:scene->instances)
    {
        const auto &bounds=pages[MaterialOrdinal(instance)].bounds; ValidateBounds(bounds);
        Require(std::isfinite(instance.yaw) && std::isfinite(instance.scale.x) && std::isfinite(instance.scale.y) &&
            std::isfinite(instance.scale.z) && instance.scale.x>0 && instance.scale.y>0 && instance.scale.z>0 &&
            std::isfinite(instance.position.x) && std::isfinite(instance.position.y) && std::isfinite(instance.position.z),
            "Static capture shadow transform is not finite with positive scale");
        const float sine=std::sin(instance.yaw),cosine=std::cos(instance.yaw);
        for(UINT corner=0;corner<8;++corner)
        {
            const float x=((corner&1)?bounds.maximum[0]:bounds.minimum[0])*instance.scale.x;
            const float y=((corner&2)?bounds.maximum[1]:bounds.minimum[1])*instance.scale.y;
            const float z=((corner&4)?bounds.maximum[2]:bounds.minimum[2])*instance.scale.z;
            const float world_x=instance.position.x+x*cosine+z*sine;
            const float world_y=instance.position.y+y;
            const float world_z=instance.position.z-x*sine+z*cosine;
            // Capture zeroes grass benders/time. BendGrass still adds its fixed
            // world-space wind envelope: 0.035 X and 0.035*0.7 Z, at most.
            const float wind=instance.mesh==RenderMesh::Grass?.035f:0.0f;
            for(UINT wind_corner=0;wind_corner<(wind>0?4u:1u);++wind_corner)
            {
                const auto world=XMVectorSet(world_x+((wind_corner&1)?wind:-wind),world_y,
                    world_z+((wind_corner&2)?wind*.7f:-wind*.7f),1);
                XMFLOAT3 point; XMStoreFloat3(&point,XMVector3TransformCoord(world,light_view));
                Require(std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z),"Static shadow world bounds exceed finite coordinates");
                minimum.x=std::min(minimum.x,point.x); minimum.y=std::min(minimum.y,point.y); minimum.z=std::min(minimum.z,point.z);
                maximum.x=std::max(maximum.x,point.x); maximum.y=std::max(maximum.y,point.y); maximum.z=std::max(maximum.z,point.z);
            }
        }
    }
    const double dx=double(maximum.x)-minimum.x,dy=double(maximum.y)-minimum.y,dz=double(maximum.z)-minimum.z;
    const double diagonal=std::hypot(dx,dy,dz);
    const double coordinate=std::max({1.0,std::abs(double(minimum.x)),std::abs(double(minimum.y)),std::abs(double(minimum.z)),
        std::abs(double(maximum.x)),std::abs(double(maximum.y)),std::abs(double(maximum.z))});
    // Two geometry-derived texels plus float transform rounding contain flat
    // axes, raster edges and first-hit PCF taps without camera-dependent fitting.
    const float padding=static_cast<float>(std::max(diagonal*2.0/kShadowEdge,
        coordinate*std::numeric_limits<float>::epsilon()*64.0));
    Require(diagonal>0 && std::isfinite(padding) && padding>0,"Static shadow geometry has no finite projection extent");
    minimum.x-=padding; minimum.y-=padding; minimum.z-=padding;
    maximum.x+=padding; maximum.y+=padding; maximum.z+=padding;
    Require(std::isfinite(minimum.x) && std::isfinite(minimum.y) && std::isfinite(minimum.z) &&
        std::isfinite(maximum.x) && std::isfinite(maximum.y) && std::isfinite(maximum.z) &&
        maximum.x>minimum.x && maximum.y>minimum.y && maximum.z>minimum.z,"Static shadow padded projection is not representable");
    const auto projection=XMMatrixOrthographicOffCenterLH(minimum.x,maximum.x,minimum.y,maximum.y,maximum.z,minimum.z);
    result.enabled=true; result.light_id=primary->stable_id; result.light_generation=primary->generation;
    result.view.scene=scene; result.view.edge=kShadowEdge;
    result.view.near_distance=0; result.view.far_distance=maximum.z-minimum.z;
    XMStoreFloat4x4(&result.view.world_to_view,light_view); XMStoreFloat4x4(&result.view.projection,projection);
    Require(result.view.projection._11>0 && result.view.projection._22>0 && result.view.projection._33<0,
        "Static shadow projection collapses its finite geometry interval");
    XMStoreFloat4x4(&result.view.view_projection,light_view*projection);
    for(const auto &row:result.view.view_projection.m)
        for(float value:row) Require(std::isfinite(value),"Static shadow projection contains nonfinite coefficients");
    const auto eye=XMVector3TransformCoord(XMVectorSet(float((double(minimum.x)+maximum.x)*.5),
        float((double(minimum.y)+maximum.y)*.5),minimum.z,1),XMMatrixInverse(nullptr,light_view));
    XMFLOAT3 position; XMStoreFloat3(&position,eye);
    Require(std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z),"Static shadow camera exceeds finite coordinates");
    result.view.position={position.x,position.y,position.z}; return result;
}
bool MaterialIdentity(std::string_view id)
{
    if(id.starts_with("render/material/") || id.starts_with("render/texture/") ||
        id.starts_with("render/shader/") || id.starts_with("render/geometry/") || id.starts_with("environment/")) return true;
    for(auto name:rd::kEnvironmentMaterialNames)
        if(id.starts_with("render/"+std::string(name))) return true;
    return false;
}
void FillFaceConstants(FrameConstants &c,const LocalProbeCaptureView &view,std::uint32_t generation)
{
    using namespace DirectX;
    const auto vp=XMLoadFloat4x4(&view.view_projection);
    XMStoreFloat4x4(&c.view_projection,XMMatrixTranspose(vp)); c.previous_view_projection=c.view_projection;
    c.current_unjittered_view_projection=view.view_projection;
    c.previous_unjittered_view_projection=view.view_projection;
    XMStoreFloat4x4(&c.inverse_view_projection,XMMatrixInverse(nullptr,vp));
    c.inverse_unjittered_view_projection=c.inverse_view_projection;
    c.camera_time={view.position.x,view.position.y,view.position.z,0};
    c.camera_forward_softness={view.world_to_view._13,view.world_to_view._23,view.world_to_view._33,0};
    c.screen_size={float(view.edge),float(view.edge),1.0f/view.edge,1.0f/view.edge}; c.output_size=c.screen_size;
    c.surface_view_origin={0,0,0,0}; c.surface_jitter_uv={0,0,0,0};
    c.temporal_options={0,0,view.far_distance,view.near_distance};
    c.grass_bender_count={0,0,0,0}; c.previous_grass_state={};
    c.vfx_light_count={0,0,0,0}; c.vfx_decal_count={0,0,0,0};
    c.particle_options={0,0,0,0}; c.authored_particle_clock={0,0,0,0}; c.lod_clock={0,0,0,0};
    c.skin_cache_options={0,0,0,generation}; c.lighting_options={0,0,1,1};
    // Captured transport is independent of exposure, screen AO/contact, camera
    // history, gameplay grass benders, VFX/decal lights and post/display settings.
    c.display_options={0,0,0,0}; c.post_options={0,0,0,0}; c.render_options={0,0,0,0};
}
} // namespace

struct LocalProbeCaptureState
{
    ComPtr<ID3D12Device> device; ComPtr<D3D12MA::Allocator> allocator; ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,rd::kAuthoredMaterialNames.size()> materials,shadows;
    ComPtr<ID3D12PipelineState> sky;
    rd::SceneProbeRevisions baseline;
    std::vector<rd::SceneProbeRecord> probes;
    std::string scene_generation,probe_digest;
    Identities environment,material_products;
    std::shared_ptr<const StaticRenderScene> revision_scene;
    std::uint32_t revision_instance_generation{};
    float revision_foliage_density{};
    std::string layout_identity,shadow_identity;
    StaticShadowView revision_shadow;
};
struct LocalProbeCaptureFrame
{
    std::shared_ptr<LocalProbeCaptureState> capture;
    std::shared_ptr<const StaticRenderScene> scene;
    std::vector<LocalProbeCapturePage> pages;
    std::vector<std::uint32_t> ordinals;
    float foliage_density{};
    std::vector<std::shared_ptr<const void>> binding_owners;
    ComPtr<ID3D12Resource> static_instances; ComPtr<D3D12MA::Allocation> static_allocation;
    ComPtr<ID3D12DescriptorHeap> descriptors,samplers,source_descriptors;
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table{};
    AllocationResource constants,shadow_map;
    ComPtr<ID3D12DescriptorHeap> shadow_dsv;
    StaticShadowView shadow;
    std::byte *mapped{};
    FrameConstants base{};
    std::uint32_t generation{},stride{};
    CompletionSet completion;
    ResourceHandle constants_handle,shadow_handle;
    std::function<void(ID3D12GraphicsCommandList *)> bind,restore;
    ~LocalProbeCaptureFrame() { if(mapped) constants.resource->Unmap(0,nullptr); }
    void BindPrivate(ID3D12GraphicsCommandList *list,UINT bank)
    {
        bind(list);
        ID3D12DescriptorHeap *heaps[]{descriptors.Get(),samplers.Get()}; list->SetDescriptorHeaps(2,heaps);
        list->SetGraphicsRootSignature(capture->root.Get());
        list->SetGraphicsRootConstantBufferView(15,
            constants.resource->GetGPUVirtualAddress()+UINT64(bank)*kConstantsStride);
        const auto table=[&](UINT offset) {
            auto handle=descriptors->GetGPUDescriptorHandleForHeapStart();
            handle.ptr+=(UINT64(bank)*kCaptureDescriptorCount+offset)*stride; return handle;
        };
        list->SetGraphicsRootDescriptorTable(0,sampler_table);
        list->SetGraphicsRootDescriptorTable(5,table(0));
        list->SetGraphicsRootDescriptorTable(13,table(renderer_detail::kPostTextureDescriptorCount));
        list->SetGraphicsRootDescriptorTable(24,table(renderer_detail::kPostTextureDescriptorCount+
            renderer_detail::kCharacterDescriptorCount+renderer_detail::kMonsterPbrDescriptorCount+
            renderer_detail::kEnvironmentDescriptorCount+renderer_detail::kVfxGradientDescriptorCount+3));
        list->SetGraphicsRootDescriptorTable(26,table(renderer_detail::kVfxDistortionDescriptorOffset));
        list->SetGraphicsRootDescriptorTable(29,table(renderer_detail::kVfxDistortionDescriptorOffset+3));
        list->SetGraphicsRootDescriptorTable(30,table(renderer_detail::kBloomTextureDescriptorOffset));
        list->SetGraphicsRootDescriptorTable(32,table(renderer_detail::kTemporalTextureDescriptorOffset));
        list->SetGraphicsRootDescriptorTable(33,table(renderer_detail::kTemporalTextureDescriptorOffset+1));
        list->SetGraphicsRootDescriptorTable(36,table(renderer_detail::kHeroTransparencyDescriptorOffset));
        list->SetGraphicsRootShaderResourceView(34,static_instances->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    void DrawInstances(ID3D12GraphicsCommandList *list,bool shadow_pass)
    {
        for(std::size_t instance=0;instance<ordinals.size();++instance)
        {
            if(!RetainDecorativeFoliage(scene->instances[instance],instance,foliage_density)) continue;
            const auto ordinal=ordinals[instance]; const auto &page=pages[ordinal];
            const auto &bases=base.material_parameter_bases[ordinal/4];
            const UINT parameter_base=std::array<UINT,4>{bases.x,bases.y,bases.z,bases.w}[ordinal%4];
            const UINT material[2]{ordinal,(parameter_base<<1)|1u};
            list->SetGraphicsRoot32BitConstants(6,2,material,0);
            list->SetPipelineState((shadow_pass?capture->shadows[ordinal]:capture->materials[ordinal]).Get());
            list->IASetVertexBuffers(0,1,&page.vertex_view); list->IASetIndexBuffer(&page.index_view);
            list->DrawIndexedInstanced(page.index_count,1,page.first_index,page.base_vertex,static_cast<UINT>(instance));
        }
    }
    Result DrawShadow(RenderPassContext &context)
    {
        auto *list=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        if(!list || !shadow.enabled) return Failure("Static capture shadow pass has no native commands or owner");
        struct Restore { ID3D12GraphicsCommandList *list; std::function<void(ID3D12GraphicsCommandList *)> &callback; ~Restore() { callback(list); } } restore_scope{list,restore};
        BindPrivate(list,kFaceCount);
        const auto depth=shadow_dsv->GetCPUDescriptorHandleForHeapStart();
        list->ClearDepthStencilView(depth,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
        list->OMSetRenderTargets(0,nullptr,FALSE,&depth);
        const D3D12_VIEWPORT viewport{0,0,float(kShadowEdge),float(kShadowEdge),0,1};
        const D3D12_RECT scissor{0,0,LONG(kShadowEdge),LONG(kShadowEdge)};
        list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
        DrawInstances(list,true); return Result::Success();
    }
    Result Draw(RenderPassContext &context,const LocalProbeCaptureView &view)
    {
        if(view.scene!=scene || view.face>=kFaceCount) return Failure("Capture callback received a stale scene or invalid face");
        auto *list=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        if(!list) return Failure("Capture callback requires native commands");
        struct Restore { ID3D12GraphicsCommandList *list; std::function<void(ID3D12GraphicsCommandList *)> &callback; ~Restore() { callback(list); } } restore_scope{list,restore};
        auto face=base; FillFaceConstants(face,view,generation);
        std::memcpy(mapped+std::size_t(view.face)*kConstantsStride,&face,sizeof(face));
        BindPrivate(list,view.face);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{view.radiance_rtv,view.distance_rtv};
        list->OMSetRenderTargets(2,targets,FALSE,&view.depth_dsv);
        const D3D12_VIEWPORT viewport{0,0,float(view.edge),float(view.edge),0,1};
        const D3D12_RECT scissor{0,0,LONG(view.edge),LONG(view.edge)};
        list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT defaults[2]{}; list->SetGraphicsRoot32BitConstants(6,2,defaults,0);
        // Background is one global environment contribution. Depth is reversed;
        // geometry replaces it and writes the radial first hit to MRT1.
        list->SetPipelineState(capture->sky.Get()); list->DrawInstanced(3,1,0,0);
        DrawInstances(list,false);
        return Result::Success();
    }
};
struct LocalProbeRuntime::Impl
{
    LocalProbeSystem service;
    std::shared_ptr<LocalProbeCaptureState> capture;
    const RenderGraphBuilder *pending_graph{};
    std::shared_ptr<LocalProbeCaptureFrame> pending_frame;
    std::vector<std::shared_ptr<LocalProbeCaptureFrame>> retired_frames;
};
LocalProbeRuntime::LocalProbeRuntime():impl_(std::make_unique<Impl>()) {}
LocalProbeRuntime::~LocalProbeRuntime()=default;

Result LocalProbeRuntime::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    const RendererConfig &config,std::string_view scene_generation,
    LocalProbeRuntimeCandidate &candidate,RendererPipelineCache *pipeline_cache) const
{
    candidate=LocalProbeRuntimeCandidate{};
    if(config.gi_mode==GraphicsGiMode::Off && config.reflection_mode==GraphicsReflectionMode::Off)
    { candidate.prepared_=true; return Result::Success(); }
    try
    {
        Require(device && allocator && root && pipeline_cache && !config.content_generation_id.empty() && !scene_generation.empty(),
            "Active local probes need native owners, pipeline cache and selected/resident scene Generations");
        const auto bytes=ReadProduct(config,kProduct); const auto package=rd::ReadSceneProbePackage(bytes,MakeAssetId(kProduct));
        auto capture=std::make_shared<LocalProbeCaptureState>(); capture->device=device; capture->allocator=allocator; capture->root=root;
        capture->baseline=package.description.revisions; capture->probes=package.probes;
        capture->scene_generation=scene_generation; capture->probe_digest=config.product_digests.at(std::string(kProduct));
        Require(package.dependencies[package.description.environment_dependency]==kEnvironment,"Local package references a different global environment");
        const auto environment=ReadProduct(config,kEnvironment);
        (void)rd::ReadLightingProbePackage(environment,MakeAssetId(kEnvironment));
        capture->environment.emplace(kEnvironment,config.product_digests.at(std::string(kEnvironment)));
        Require(Revision(rd::SceneProbeRevisionDomain::Environment,capture->environment)==capture->baseline.environment,
            "Local package baseline environment revision is stale for the selected Generation");
        for(const auto &id:package.dependencies)
        {
            std::filesystem::path path; const auto found=config.product_digests.find(id);
            Require(found!=config.product_digests.end() && static_cast<bool>(config.ResolveProduct(id,path)),"Local package dependency is absent from Generation: "+id);
        }
        // Source-file light/layout identities are unavailable in RendererConfig.
        // Keep the four validated package baselines; native revisions below fold
        // real retained scene/lights/parameters/products rather than forging them.
        for(const auto &[id,digest]:config.product_digests)
            if(MaterialIdentity(id)) capture->material_products.emplace(id,digest);
        capture->material_products[std::string(kProduct)]=capture->probe_digest;
        const auto vertex=ReadProduct(config,"render/shader/local_probe_capture_vs.dxil");
        const auto fullscreen=ReadProduct(config,"render/shader/fullscreen_vs.dxil");
        const auto sky=ReadProduct(config,"render/shader/local_probe_capture_sky_ps.dxil");
        constexpr D3D12_INPUT_ELEMENT_DESC layout[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BLENDINDICES",0,DXGI_FORMAT_R16G16B16A16_UINT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,56,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"MATERIAL",0,DXGI_FORMAT_R16_UINT,0,72,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature=root; p.VS={vertex.data(),vertex.size()};
        p.InputLayout={layout,static_cast<UINT>(std::size(layout))}; p.SampleMask=UINT_MAX;
        p.BlendState.IndependentBlendEnable=TRUE;
        for(auto &blend:p.BlendState.RenderTarget)
        {
            blend.SrcBlend=D3D12_BLEND_ONE; blend.DestBlend=D3D12_BLEND_ZERO; blend.BlendOp=D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlendAlpha=D3D12_BLEND_ZERO; blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
            blend.LogicOp=D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.DepthClipEnable=TRUE;
        p.DepthStencilState.DepthEnable=TRUE; p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        p.DepthStencilState.StencilReadMask=D3D12_DEFAULT_STENCIL_READ_MASK;
        p.DepthStencilState.StencilWriteMask=D3D12_DEFAULT_STENCIL_WRITE_MASK;
        p.DepthStencilState.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
        p.DepthStencilState.BackFace=p.DepthStencilState.FrontFace;
        p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.NumRenderTargets=2;
        p.RTVFormats[0]=DXGI_FORMAT_R32G32B32A32_FLOAT; p.RTVFormats[1]=DXGI_FORMAT_R32_FLOAT;
        p.DSVFormat=DXGI_FORMAT_D32_FLOAT; p.SampleDesc={1,0};
        for(std::size_t i=0;i<rd::kAuthoredMaterialNames.size();++i)
        {
            const auto pixel=ReadProduct(config,"render/shader/"+std::string(rd::kAuthoredMaterialNames[i])+"_probe_capture.dxil");
            p.PS={pixel.data(),pixel.size()}; p.RasterizerState.CullMode=Masked(i)?D3D12_CULL_MODE_NONE:D3D12_CULL_MODE_BACK;
            Native(pipeline_cache->CreateGraphics(&p,IID_PPV_ARGS(&capture->materials[i])),"Create static local-probe material capture PSO");
            const auto shadow_pixel=ReadProduct(config,"render/shader/"+std::string(rd::kAuthoredMaterialNames[i])+"_probe_shadow.dxil");
            auto depth=p; depth.PS={shadow_pixel.data(),shadow_pixel.size()}; depth.NumRenderTargets=0;
            for(auto &format:depth.RTVFormats) format=DXGI_FORMAT_UNKNOWN;
            // Match the renderer's established reversed-depth directional bias.
            depth.RasterizerState.DepthBias=-800; depth.RasterizerState.SlopeScaledDepthBias=-1.5f;
            Native(pipeline_cache->CreateGraphics(&depth,IID_PPV_ARGS(&capture->shadows[i])),"Create pure-static capture shadow PSO");
        }
        p.VS={fullscreen.data(),fullscreen.size()}; p.PS={sky.data(),sky.size()}; p.InputLayout={};
        p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; p.DepthStencilState.DepthEnable=FALSE;
        p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        Native(pipeline_cache->CreateGraphics(&p,IID_PPV_ARGS(&capture->sky)),"Create local-probe global sky capture PSO");
        const auto reader=[&](std::string_view id,std::vector<std::byte> &output)->Result {
            try { output=ReadProduct(config,id); return Result::Success(); }
            catch(const std::exception &error) { return Failure(error.what()); }
        };
        if(auto prepared=impl_->service.Prepare(device,allocator,bytes,reader,candidate.service_);!prepared) return prepared;
        candidate.capture_=std::move(capture); candidate.prepared_=true; return Result::Success();
    }
    catch(const std::exception &error) { return Failure(error.what()); }
}
Result LocalProbeRuntime::Commit(LocalProbeRuntimeCandidate &&candidate,const CompletionSet &completed)
{
    if(!candidate.prepared_ || impl_->pending_graph) return Failure("Local-probe candidate is unprepared or has pending graph work");
    for(const auto &frame:impl_->retired_frames) if(!frame->completion.IsSatisfiedBy(completed)) return Failure("Capture leases are still in flight");
    if(candidate.capture_)
    { if(auto committed=impl_->service.Commit(std::move(candidate.service_),completed);!committed) return committed; }
    else if(auto reset=impl_->service.ResetAfterCompletion(completed);!reset) return reset;
    impl_->capture=std::move(candidate.capture_); impl_->retired_frames.clear(); candidate.prepared_=false;
    return Result::Success();
}
Result LocalProbeRuntime::CurrentRevisions(const LocalProbeRuntimeFrameInputs &in,rd::SceneProbeRevisions &out) const
{
    out={}; const auto capture=impl_->capture; if(!capture || !in.scene) return Result::Success();
    try
    {
        Require(in.scene && in.frame_constants && in.scene->content_generation_id==capture->scene_generation && in.static_instance_generation &&
            in.pages.size()==rd::kAuthoredMaterialNames.size(),"Native local-probe scene/page generation does not match adoption");
        // The retained scene is immutable; revalidate its layout only when its adoption or density changes.
        if(capture->revision_scene!=in.scene || capture->revision_instance_generation!=in.static_instance_generation ||
            capture->revision_foliage_density!=in.foliage_density)
        {
            Identity layout; layout.Text("local-probe.actual-static-scene.v1"); layout.Text(in.scene->content_generation_id);
            layout.Integer(in.static_instance_generation,4); layout.Integer(in.scene->layout_abi,4);
            layout.Integer(in.scene->instances.size()); layout.Float(in.foliage_density);
            for(const auto &instance:in.scene->instances)
            {
                (void)MaterialOrdinal(instance); layout.Integer(instance.stable_id); layout.Integer(static_cast<std::uint32_t>(instance.mesh),4);
                layout.Vector(instance.position); layout.Vector(instance.scale); layout.Float(instance.yaw);
                layout.Integer(instance.color_rgba,4); layout.Integer(instance.environment_seed,4); layout.Integer(instance.environment_variant,4);
                layout.Integer(instance.asset_id.value); layout.Integer(instance.material_id.value);
                layout.Integer(instance.revisions.transform); layout.Integer(instance.revisions.material); layout.Integer(instance.revisions.content);
            }
            capture->layout_identity=layout.Finish();
            capture->revision_scene=in.scene;
            capture->revision_instance_generation=in.static_instance_generation;
            capture->revision_foliage_density=in.foliage_density;
        }
        Identity material; material.Text("local-probe.actual-material-state.v1");
        material.Integer(in.material_parameter_revision); material.Integer(in.pipeline_revision);
        for(const auto &page:in.pages)
        {
            Require(page.complete && page.generation && page.index_count,"Capture geometry is not an actual complete page");
            material.Integer(page.generation,4); material.Integer(page.page_slot,4); material.Integer(page.first_index,4); material.Integer(page.index_count,4);
            material.Integer(std::bit_cast<std::uint32_t>(page.base_vertex),4);
            material.Integer(page.vertex_view.StrideInBytes,4); material.Integer(page.vertex_view.SizeInBytes,4);
            material.Integer(static_cast<std::uint32_t>(page.index_view.Format),4); material.Integer(page.index_view.SizeInBytes,4);
            ValidateBounds(page.bounds);
            for(float value:page.bounds.minimum) material.Float(value);
            for(float value:page.bounds.maximum) material.Float(value);
            for(float value:page.bounds.sphere) material.Float(value);
        }
        Identity lights; lights.Text("local-probe.actual-light-view.v1"); lights.Integer(in.lights.size());
        for(const auto &light:in.lights)
        {
            lights.Integer(light.stable_id); lights.Integer(light.generation,4); lights.Integer(light.revision);
            lights.Integer(static_cast<std::uint32_t>(light.type),4); lights.Integer(static_cast<std::uint32_t>(light.unit),4);
            lights.Vector(light.direction); lights.Vector(light.color); lights.Vector(light.position); lights.Float(light.quantity);
            lights.Float(light.range); lights.Float(light.shape.x); lights.Float(light.shape.y); lights.Float(light.emitter_radius);
            lights.Float(light.inner_cone_radians); lights.Float(light.outer_cone_radians); lights.Vector(light.area_tangent);
            lights.Integer(light.shadow_flags,4); lights.Integer(light.legacy_attenuation,1); lights.Integer(light.two_sided,1);
        }
        if(in.frame_constants)
        {
            material.Vector(in.frame_constants->material_quality_options);
            lights.Vector(in.frame_constants->light_direction_intensity); lights.Vector(in.frame_constants->light_color);
        }
        const auto material_identity=material.Finish();
        auto shadow_key=lights;
        shadow_key.Text(capture->layout_identity); shadow_key.Text(material_identity);
        const auto shadow_identity=shadow_key.Finish();
        if(capture->shadow_identity!=shadow_identity)
        {
            const auto shadow=MakeStaticShadowView(in.scene,in.pages,in.lights);
            capture->revision_shadow=shadow;
            capture->shadow_identity=shadow_identity;
        }
        const auto &shadow=capture->revision_shadow;
        lights.Text("local-probe.pure-static-directional-shadow.v1"); lights.Integer(kShadowEdge,4);
        lights.Integer(shadow.enabled,1); lights.Integer(shadow.light_id); lights.Integer(shadow.light_generation,4);
        lights.Integer(std::bit_cast<std::uint32_t>(std::int32_t(-800)),4); lights.Float(-1.5f);
        if(shadow.enabled)
            for(const auto &row:shadow.view.view_projection.m) for(float value:row) lights.Float(value);
        auto materials=capture->material_products; materials["native/material-and-complete-page-state"]=material_identity;
        const Identities light_inputs{{std::string(kProduct),capture->probe_digest},{"native/authoritative-light-view",lights.Finish()}};
        const Identities layout_inputs{{std::string(kProduct),capture->probe_digest},{"native/immutable-static-scene",capture->layout_identity}};
        out={Revision(rd::SceneProbeRevisionDomain::Environment,capture->environment),Revision(rd::SceneProbeRevisionDomain::Material,materials),
            Revision(rd::SceneProbeRevisionDomain::Light,light_inputs),Revision(rd::SceneProbeRevisionDomain::StaticLayout,layout_inputs)};
        return Result::Success();
    }
    catch(const std::exception &error) { return Failure(error.what()); }
}
Result LocalProbeRuntime::Register(RenderGraphBuilder &graph,const LocalProbeRuntimeFrameInputs &in,LocalProbeOutputs &out)
{
    out={}; if(!impl_->capture) return Result::Success();
    if(impl_->pending_graph) return Failure("Local-probe runtime already has pending graph work");
    std::erase_if(impl_->retired_frames,[&](const auto &frame) { return frame->completion.IsSatisfiedBy(in.completed); });
    if(!in.scene)
    {
        // No substitute scene or revisions. The service uploads/exports its
        // real zero-count metadata and rejects prior scene publication.
        const LocalProbeInputs absent{nullptr,{}, {},{},in.completed,0};
        if(auto registered=impl_->service.Register(graph,absent,out);!registered) return registered;
        impl_->pending_graph=&graph; return Result::Success();
    }
    rd::SceneProbeRevisions revisions; if(auto qualified=CurrentRevisions(in,revisions);!qualified) return qualified;
    if(in.max_cube_views>6) return Failure("Capture cube-view budget exceeds six");
    if(!in.max_cube_views || !impl_->service.NativeCaptureRequired(in.scene,revisions,in.completed))
    {
        const LocalProbeInputs publication{in.scene,revisions,{}, {},in.completed,0};
        if(auto registered=impl_->service.Register(graph,publication,out);!registered) return registered;
        impl_->pending_graph=&graph; return Result::Success();
    }
    try
    {
        Require(in.frame_constants && in.static_instances && in.static_version.resource && in.frame_srv_heap && in.sampler_heap &&
            in.frame_srv_base.ptr && in.sampler_table.ptr && in.bind_graphics && in.restore_graphics && !in.scene_reads.empty(),
            "Capture requires actual static/shared bindings, graph reads and parent restore callback");
        Require(in.scene->instances.size()<=UINT_MAX && in.max_cube_views<=6,"Capture instance/view budget overflow");
        for(const auto &read:in.scene_reads)
            Require(read.version.resource && read.access!=Access::Undefined && read.access!=Access::Common &&
                read.access!=Access::RenderTarget && read.access!=Access::DepthWrite && read.access!=Access::UnorderedWrite && read.access!=Access::CopyDest,
                "Shared capture binding is not a valid actual graph read");
        auto frame=std::make_shared<LocalProbeCaptureFrame>(); frame->capture=impl_->capture; frame->scene=in.scene;
        frame->pages.assign(in.pages.begin(),in.pages.end()); frame->binding_owners=in.binding_owners;
        frame->static_instances=in.static_instances; frame->static_allocation=in.static_allocation;
        frame->generation=in.static_instance_generation; frame->base=*in.frame_constants; frame->foliage_density=in.foliage_density;
        frame->shadow=MakeStaticShadowView(in.scene,in.pages,in.lights);
        for(UINT cascade=0;cascade<3;++cascade)
        {
            const auto matrix=frame->shadow.enabled?DirectX::XMLoadFloat4x4(&frame->shadow.view.view_projection):DirectX::XMMatrixIdentity();
            DirectX::XMStoreFloat4x4(&frame->base.shadow_view_projection[cascade],DirectX::XMMatrixTranspose(matrix));
            frame->base.shadow_atlas_scale_offset[cascade]={1,1,0,0};
        }
        frame->base.shadow_atlas_texel_size=frame->shadow.enabled?
            DirectX::XMFLOAT4{1.0f/kShadowEdge,1.0f/kShadowEdge,0,in.frame_constants->shadow_atlas_texel_size.w}:DirectX::XMFLOAT4{};
        frame->bind=in.bind_graphics; frame->restore=in.restore_graphics; frame->samplers=in.sampler_heap;
        frame->sampler_table=in.sampler_table; frame->source_descriptors=in.frame_srv_heap;
        for(const auto &instance:in.scene->instances) frame->ordinals.push_back(MaterialOrdinal(instance));
        for(const auto &page:frame->pages)
        {
            Require(page.vertices && page.indices && page.vertex_version.resource && page.index_version.resource &&
                page.vertex_view.StrideInBytes==76 && page.vertex_view.SizeInBytes && page.index_view.Format==DXGI_FORMAT_R32_UINT &&
                UINT64(page.first_index)+page.index_count<=page.index_view.SizeInBytes/4,
                "Capture page native geometry/views/graph versions are invalid");
            Require(page.vertex_view.BufferLocation>=page.vertices->GetGPUVirtualAddress() &&
                page.vertex_view.BufferLocation-page.vertices->GetGPUVirtualAddress()+page.vertex_view.SizeInBytes<=page.vertices->GetDesc().Width &&
                page.index_view.BufferLocation>=page.indices->GetGPUVirtualAddress() &&
                page.index_view.BufferLocation-page.indices->GetGPUVirtualAddress()+page.index_view.SizeInBytes<=page.indices->GetDesc().Width,
                "Capture page native views escape their retained resources");
        }
        const auto device=frame->capture->device.Get(); frame->stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto source_start=in.frame_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
        ComPtr<ID3D12Device> source_device;
        Native(in.frame_srv_heap->GetDevice(IID_PPV_ARGS(&source_device)),"Query capture descriptor source device");
        Require(source_device.Get()==device,"Capture descriptor source belongs to another native device");
        Require(in.frame_srv_heap->GetDesc().Type==D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
            !(in.frame_srv_heap->GetDesc().Flags&D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE),
            "Local probe capture requires the original nonvisible CPU descriptor source");
        Require(in.frame_srv_base.ptr>=source_start && (in.frame_srv_base.ptr-source_start)%frame->stride==0 &&
            (in.frame_srv_base.ptr-source_start)/frame->stride+kCaptureDescriptorCount<=in.frame_srv_heap->GetDesc().NumDescriptors,
            "Capture descriptor prefix is outside the actual frame heap");
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; heap.NumDescriptors=kConstantBankCount*kCaptureDescriptorCount;
        Native(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&frame->descriptors)),"Create immutable capture face tables");
        if(frame->shadow.enabled)
        {
            D3D12_RESOURCE_DESC depth{}; depth.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            depth.Width=depth.Height=kShadowEdge; depth.DepthOrArraySize=depth.MipLevels=1;
            depth.Format=DXGI_FORMAT_R32_TYPELESS; depth.SampleDesc.Count=1; depth.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            D3D12_CLEAR_VALUE clear{}; clear.Format=DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth=0;
            D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=D3D12_HEAP_TYPE_DEFAULT;
            Native(frame->capture->allocator->CreateResource(&allocation,&depth,D3D12_RESOURCE_STATE_COMMON,&clear,
                &frame->shadow_map.allocation,IID_PPV_ARGS(&frame->shadow_map.resource)),"Allocate private pure-static capture shadow map");
            D3D12_DESCRIPTOR_HEAP_DESC dsv{}; dsv.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dsv.NumDescriptors=1;
            Native(device->CreateDescriptorHeap(&dsv,IID_PPV_ARGS(&frame->shadow_dsv)),"Create private static capture shadow DSV heap");
            D3D12_DEPTH_STENCIL_VIEW_DESC view{}; view.Format=DXGI_FORMAT_D32_FLOAT; view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(frame->shadow_map.resource.Get(),&view,frame->shadow_dsv->GetCPUDescriptorHandleForHeapStart());
            frame->shadow_map.resource->SetName(L"LocalProbe.PureStaticDirectionalCaptureShadow");
        }
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=UINT64(kConstantBankCount)*kConstantsStride;
        buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
        Native(frame->capture->allocator->CreateResource(&allocation,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,&frame->constants.allocation,
            IID_PPV_ARGS(&frame->constants.resource)),"Allocate immutable capture face constants");
        D3D12_RANGE no_read{}; Native(frame->constants.resource->Map(0,&no_read,reinterpret_cast<void **>(&frame->mapped)),"Map capture face constants");
        for(UINT bank=0;bank<kConstantBankCount;++bank)
        {
            auto constants=frame->base;
            if(bank==kFaceCount && frame->shadow.enabled) FillFaceConstants(constants,frame->shadow.view,frame->generation);
            std::memcpy(frame->mapped+std::size_t(bank)*kConstantsStride,&constants,sizeof(constants));
            auto destination=frame->descriptors->GetCPUDescriptorHandleForHeapStart(); destination.ptr+=SIZE_T(bank)*kCaptureDescriptorCount*frame->stride;
            device->CopyDescriptorsSimple(kCaptureDescriptorCount,destination,in.frame_srv_base,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto shadow_descriptor=destination; shadow_descriptor.ptr+=3u*frame->stride; // Root5 begins at t2; ShadowMap is t5.
            D3D12_SHADER_RESOURCE_VIEW_DESC shadow_srv{}; shadow_srv.Format=DXGI_FORMAT_R32_FLOAT;
            shadow_srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; shadow_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            shadow_srv.Texture2D.MipLevels=1;
            // The opacity-only depth shader never reads ShadowMap. Its bank uses
            // a typed null to avoid an SRV/DSV alias while the map is written.
            device->CreateShaderResourceView(bank<kFaceCount?frame->shadow_map.resource.Get():nullptr,&shadow_srv,shadow_descriptor);
        }
        frame->constants_handle=graph.ImportBuffer({frame->constants.resource.Get(),{},QueueHint::Direct},Access::AllShaderRead,"LocalProbe.PrivateFaceConstants");
        std::vector<LocalProbeSceneRead> reads(in.scene_reads.begin(),in.scene_reads.end());
        reads.push_back({graph.Version(frame->constants_handle),Access::AllShaderRead}); reads.push_back({in.static_version,Access::VertexShaderRead});
        for(const auto &page:frame->pages) { reads.push_back({page.vertex_version,Access::VertexBuffer}); reads.push_back({page.index_version,Access::IndexBuffer}); }
        if(frame->shadow.enabled)
        {
            frame->shadow_handle=graph.ImportTexture({frame->shadow_map.resource.Get(),{},QueueHint::Direct},Access::Common,"LocalProbe.PrivateStaticShadow");
            auto shadow_pass=graph.AddPass(177,"LocalProbeStaticDirectionalShadow",QueueHint::Direct);
            for(const auto &read:reads) shadow_pass.Read(read.version,read.access);
            if(in.declare_shadow_sampling) in.declare_shadow_sampling(shadow_pass);
            const auto shadow_version=shadow_pass.Write(graph.Version(frame->shadow_handle),Access::DepthWrite);
            shadow_pass.SetExecuteChecked([frame](RenderPassContext &context) { return frame->DrawShadow(context); });
            reads.push_back({shadow_version,Access::ShaderRead});
            graph.SetFinalAccess(shadow_version,Access::ShaderRead);
        }
        const LocalProbeInputs input{in.scene,revisions,reads,[frame](RenderPassContext &context,const LocalProbeCaptureView &view) {
            return frame->Draw(context,view);
        },in.completed,in.max_cube_views,in.declare_material_sampling};
        if(auto registered=impl_->service.Register(graph,input,out);!registered) return registered;
        impl_->pending_graph=&graph; impl_->pending_frame=std::move(frame); return Result::Success();
    }
    catch(const std::exception &error) { return Failure(error.what()); }
}
Result LocalProbeRuntime::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success,
    const std::shared_ptr<const StaticRenderScene> &scene,rd::SceneProbeRevisions revisions)
{
    if(!impl_->capture) return Result::Success();
    if(impl_->pending_graph!=&graph) return Failure("Local-probe completion does not match its registered graph");
    auto frame=std::move(impl_->pending_frame); impl_->pending_graph=nullptr;
    if(frame)
    {
        frame->completion=completion; frame->completion.Merge(graph.ResourceCompletion(frame->constants_handle));
        if(frame->shadow_handle) frame->completion.Merge(graph.ResourceCompletion(frame->shadow_handle));
        impl_->retired_frames.push_back(std::move(frame));
    }
    return impl_->service.CompleteSubmission(graph,completion,success,scene,revisions);
}
Result LocalProbeRuntime::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending_graph) return Failure("Cannot reset local-probe runtime with pending graph work");
    for(const auto &frame:impl_->retired_frames) if(!frame->completion.IsSatisfiedBy(completed)) return Failure("Cannot reset in-flight capture bindings");
    if(auto reset=impl_->service.ResetAfterCompletion(completed);!reset) return reset;
    impl_->capture.reset(); impl_->retired_frames.clear(); return Result::Success();
}
std::span<const rd::SceneProbeRecord> LocalProbeRuntime::Probes() const noexcept
{ return impl_->capture?std::span<const rd::SceneProbeRecord>(impl_->capture->probes):std::span<const rd::SceneProbeRecord>{}; }
rd::SceneProbeRevisions LocalProbeRuntime::BaselineRevisions() const noexcept
{ return impl_->capture?impl_->capture->baseline:rd::SceneProbeRevisions{}; }
std::uint64_t LocalProbeRuntime::AllocatedBytes() const noexcept
{
    auto bytes=impl_->service.AllocatedBytes();
    const auto native_bytes=[](const std::shared_ptr<LocalProbeCaptureFrame> &frame) {
        return (frame->constants.allocation?frame->constants.allocation->GetSize():0) +
            (frame->shadow_map.allocation?frame->shadow_map.allocation->GetSize():0);
    };
    if(impl_->pending_frame) bytes+=native_bytes(impl_->pending_frame);
    for(const auto &frame:impl_->retired_frames) bytes+=native_bytes(frame);
    return bytes;
}
} // namespace hs
