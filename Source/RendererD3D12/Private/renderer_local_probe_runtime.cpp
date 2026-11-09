#include <hs/jobs/task_system.hpp>
#include "renderer_local_probe_runtime.hpp"
#include "renderer_local_probe_draws.hpp"
#include "renderer_impl.hpp"
#include "renderer_pipeline_cache.hpp"
#include "renderer_quality.hpp"
#include "visibility_encoding.hpp"

#include <hs/core/sha256.hpp>
#include <hs/render_data/lighting_probe.hpp>
#include <hs/render_data/scene_probe_revision.hpp>
#include <hs/render_data/scene_probe_grid_bake.hpp>
#include <hs/render_data/static_shadow.hpp>
#include <hs/render_data/static_shadow_archive.hpp>
#include <hs/render_data/texture_package.hpp>

#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <stdexcept>

namespace hs
{
#if defined(HS_OFFLINE_PROBE_BAKER)
Result D3D12Renderer::Impl::CreateStaticShadowBakeGeometry()
{
    auto &frame = frames[0];
    auto result = frame.allocator->Reset();
    if (FAILED(result)) return HResultFailure("Reset static shadow geometry allocator", result);
    result = command_list->Reset(frame.allocator.Get(), nullptr);
    if (FAILED(result)) return HResultFailure("Reset static shadow geometry commands", result);
    std::vector<AllocationResource> uploads;
    if (auto created = CreateProceduralGeometry(command_list.Get(), uploads); !created) return created;
    result = command_list->Close();
    if (FAILED(result)) return HResultFailure("Close static shadow geometry upload", result);
    return SubmitResidentUtilityUpload(command_list.Get(), frame.allocator.Get(), uploads);
}

void D3D12Renderer::Impl::PrepareLocalProbeCaptureInputs(LocalProbeRuntimeFrameInputs &inputs,
    std::array<LocalProbeCapturePage, kEnvironmentMaterialNames.size() + 1> &pages,
    std::vector<LocalProbeSceneRead> &reads, bool restored_graph,
    std::span<const GeometryPageGpu *const> bucket_pages,
    ResourceVersion static_instances_version,
    ResourceVersion material_parameter_version, ResourceVersion hero_binding_version,
    const LightingProbeOutputs &probe_outputs, const LightListOutputs &light_outputs,
    const LtcLutOutputs &ltc_outputs, const VirtualShadowOutputs &virtual_shadow_outputs,
    const LocalShadowOutputs &local_shadow_outputs,
    const std::function<ResourceHandle(AllocationResource &, Access, std::string_view)> &import_buffer,
    const std::function<ResourceHandle(AllocationResource &, Access, std::string_view)> &import_texture)
{
    inputs.static_instances = static_gpu_scene->instances.resource;
    inputs.static_allocation = static_gpu_scene->instances.allocation;
    if (!restored_graph) inputs.static_version = static_instances_version;
    inputs.binding_owners.push_back(static_gpu_scene);
    struct CaptureNativeLease { Microsoft::WRL::ComPtr<D3D12MA::Allocation> allocation; Microsoft::WRL::ComPtr<ID3D12Resource> resource; };
    const auto capture_read = [&](AllocationResource &resource, bool texture, std::string_view name,
                                  ResourceVersion produced = {}) {
        if (!resource.resource) return;
        const auto version = produced.resource ? produced : graph.Version(texture
            ? import_texture(resource, Access::AllShaderRead, name)
            : import_buffer(resource, Access::AllShaderRead, name));
        if (!restored_graph && std::ranges::none_of(reads, [&](const auto &read) {
                return read.version.resource.value == version.resource.value &&
                       read.version.resource.kind == version.resource.kind;
            }))
            reads.push_back({version, Access::AllShaderRead});
        inputs.binding_owners.push_back(std::make_shared<CaptureNativeLease>(CaptureNativeLease{resource.allocation, resource.resource}));
    };
    for (std::size_t material = 0; material < pages.size(); ++material)
    {
        const auto &page = material == kEnvironmentMaterialNames.size() ? procedural_geometry : *bucket_pages[material * 3];
        auto &capture = pages[material];
        capture.vertices = page.storage.resource; capture.indices = page.storage.resource;
        capture.vertex_allocation = page.storage.allocation; capture.index_allocation = page.storage.allocation;
        if (!restored_graph)
            capture.vertex_version = graph.Version(import_buffer(
                const_cast<AllocationResource &>(page.storage), Access::VertexShaderRead, "ProbeCompleteGeometry"));
        capture.index_version = capture.vertex_version;
        capture.vertex_view = page.vertex_view; capture.index_view = page.index_view;
        capture.index_count = page.index_count; capture.generation = page.generation; capture.page_slot = page.page_slot;
        capture.source_digest = page.source_digest;
        capture.bounds = page.metadata.bounds;
        capture.complete = page.storage.resource && page.index_count != 0;
    }
    capture_read(material_parameters, false, "ProbeMaterialParameters", material_parameter_version);
    capture_read(mesh_material_bindings_gpu, false, "ProbeHeroBindings", hero_binding_version);
    capture_read(environment_brdf_lut, true, "ProbeBrdfLut");
    if (!restored_graph)
    {
    for (const auto version : {probe_outputs.radiance, probe_outputs.prefiltered, probe_outputs.irradiance,
            light_outputs.lights, light_outputs.ranges, light_outputs.inline_indices, light_outputs.spill_indices,
            ltc_outputs.matrix, ltc_outputs.amplitude}) reads.push_back({version, Access::AllShaderRead});
    if (virtual_shadow_outputs.resources[0])
        for (const auto version : {virtual_shadow_outputs.metadata, virtual_shadow_outputs.page_table,
                virtual_shadow_outputs.static_bank, virtual_shadow_outputs.dynamic_bank, virtual_shadow_outputs.draw_records})
            reads.push_back({version, Access::AllShaderRead});
    if(virtual_shadow_outputs.root_static.resource)
        reads.push_back({virtual_shadow_outputs.root_static,Access::AllShaderRead});
    if(virtual_shadow_outputs.root_dynamic.resource)
        reads.push_back({virtual_shadow_outputs.root_dynamic,Access::AllShaderRead});
    if (local_shadow_outputs.resources[0])
        for (const auto version : {local_shadow_outputs.metadata, local_shadow_outputs.records,
                local_shadow_outputs.static_bank, local_shadow_outputs.dynamic_bank, local_shadow_outputs.draw_records})
            reads.push_back({version, Access::AllShaderRead});
    }
    inputs.pages = pages;
    inputs.scene_reads = reads;
}

#endif
namespace
{
using Microsoft::WRL::ComPtr;
using renderer_detail::FrameConstants;
namespace rd=render_data;
constexpr std::string_view kProduct="render/lighting/local_probes.spkg";
constexpr std::string_view kEnvironment="render/lighting/default_probe.rpkg";
constexpr UINT kFaceCount=6;
constexpr UINT kConstantBankCount=kFaceCount+1, kShadowEdge=512;
constexpr UINT kCaptureSkinDescriptorOffset=renderer_detail::kLayeredTranslucencyDescriptorOffset+kLayeredTranslucencyTableSize;
constexpr UINT kCaptureDescriptorCount=kCaptureSkinDescriptorOffset+2;
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
std::string Hash(std::span<const std::byte> bytes)
{
    renderer_detail::StartupOperationScope timing(RendererStartupOperation::Sha256);
    return "sha256:" + Hex(hs::Sha256(bytes));
}
std::vector<std::byte> ReadProduct(const RendererConfig &config,std::string_view id)
{
    RendererAssetSource source; const auto resolved=ResolveRendererAsset(config,id,source);
    Require(static_cast<bool>(resolved),resolved.Message());
    const auto digest=RendererProductDigest(config,id);
    Require(!digest.empty(),"Missing required product digest: "+std::string(id));
    renderer_detail::StartupOperationScope read_timing(RendererStartupOperation::CheckedBinaryRead);
    std::uint64_t count{}; const auto sized=source.ByteCount(count);
    Require(static_cast<bool>(sized),sized.Message());
    Require(count>0 && count<=256ull*1024*1024,"Invalid bounded probe/shader product size");
    std::vector<std::byte> bytes; const auto read=source.Read(bytes,count);
    Require(static_cast<bool>(read),read.Message());
    read_timing.Stop();
    if(!source.IsPacked() && !source.HasAdmittedWholeFileProof())
        Require(Hash(bytes)==digest,"Probe/shader digest mismatch: "+std::string(id));
    return bytes;
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
    void Bytes(std::span<const std::byte> value)
    { Integer(value.size()); hash_.Add(value); }
    std::string Finish() { return "sha256:" + Hex(hash_.Finish()); }
  private:
    hs::Sha256Stream hash_;
};
#if defined(HS_OFFLINE_PROBE_BAKER)
std::array<std::byte,32> CaptureBindingIdentity(std::span<const LocalProbeTextureFact> facts,
    std::span<const LocalProbePageRevision> page_revisions,
    std::span<const LocalProbeCapturePage> pages)
{
    std::vector<const LocalProbeTextureFact *> sorted; sorted.reserve(facts.size());
    for(const auto &fact:facts) sorted.push_back(&fact);
    std::ranges::sort(sorted,{},[](const auto *fact){return fact->logical_id;});
    Identity identity; identity.Text("local-probe.native-capture-bindings.v2");
    for(const auto *fact:sorted) { identity.Text(fact->logical_id); identity.Integer(fact->sampled_mip,4); }
    const auto page_count=page_revisions.empty()?pages.size():page_revisions.size();
    identity.Integer(page_count);
    const auto append_page=[&](std::size_t index,std::uint32_t page_slot,
        const std::array<std::byte,32> &source_digest,std::uint32_t index_count,
        const render_data::GeometryBounds &bounds)
    {
        identity.Integer(index,4); identity.Integer(page_slot,4); identity.Bytes(source_digest);
        identity.Integer(index_count,4);
        for(float value:bounds.minimum) identity.Float(value);
        for(float value:bounds.maximum) identity.Float(value);
        for(float value:bounds.sphere) identity.Float(value);
    };
    if(!page_revisions.empty())
        for(std::size_t index=0;index<page_revisions.size();++index)
        {
            const auto &page=page_revisions[index];
            append_page(index,page.page_slot,page.source_digest,page.index_count,page.bounds);
        }
    else for(std::size_t index=0;index<pages.size();++index)
        {
            const auto &page=pages[index];
            append_page(index,page.page_slot,page.source_digest,page.index_count,page.bounds);
        }
    const auto digest=identity.Finish(); std::array<std::byte,32> result{};
    const auto digit=[](char c)->unsigned { return c<='9'?unsigned(c-'0'):unsigned(c-'a'+10); };
    for(std::size_t i=0;i<result.size();++i)
        result[i]=std::byte((digit(digest[7+i*2])<<4)|digit(digest[8+i*2]));
    return result;
}
bool CaptureTexturesReady(std::span<const LocalProbeTextureFact> facts)
{ return std::ranges::all_of(facts,[](const auto &fact){return !fact.logical_id.empty()&&fact.ready;}); }
#endif
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
    case RenderMesh::MeadowSurface: Require(instance.environment_variant<2,"Invalid meadow variant"); return instance.environment_variant==0 ? rd::kTerrainMaterialOrdinal : rd::kGroundDetailMaterialOrdinal;
    case RenderMesh::GroundDetail: Require(instance.environment_variant<kGroundDetailVariantCount,"Invalid static ground-detail variant"); return rd::kGroundDetailMaterialOrdinal;
    case RenderMesh::Ground: case RenderMesh::DirtPatch: return rd::kTerrainMaterialOrdinal;
    default: throw std::runtime_error("Dynamic/unsupported mesh in immutable local-probe static scene");
    }
}
#if defined(HS_OFFLINE_PROBE_BAKER)
std::uint32_t PageOrdinal(const RenderInstance &instance)
{
    // All seven detail geometries share material15, but retain distinct complete pages.
    (void)MaterialOrdinal(instance);
    return rd::IsEnvironmentMesh(instance.mesh)
        ? rd::EnvironmentGeometryOrdinal(instance.mesh,instance.environment_variant)
        : static_cast<std::uint32_t>(rd::kEnvironmentMaterialNames.size());
}
bool Masked(std::size_t ordinal) { return (ordinal>=3 && ordinal<6) || (ordinal>=10 && ordinal<14); }
#endif
void ValidateBounds(const rd::GeometryBounds &bounds)
{
    for(std::size_t axis=0;axis<3;++axis)
        Require(std::isfinite(bounds.minimum[axis]) && std::isfinite(bounds.maximum[axis]) &&
            bounds.minimum[axis]<=bounds.maximum[axis],"Complete capture page has invalid local geometry bounds");
    for(float value:bounds.sphere) Require(std::isfinite(value),"Complete capture page has nonfinite bounding sphere");
    Require(bounds.sphere[3]>=0,"Complete capture page has a negative bounding radius");
}
#if defined(HS_OFFLINE_PROBE_BAKER)
struct StaticShadowView
{
    LocalProbeCaptureView view;
    std::uint64_t light_id{};
    std::uint32_t light_generation{};
    bool enabled{};
};
struct StaticShadowBakeBounds
{
    DirectX::XMFLOAT3 minimum{}, maximum{};
};
template<class Page>
StaticShadowView MakeStaticShadowView(const std::shared_ptr<const StaticRenderScene> &scene,
    std::span<const Page> pages,std::span<const LightView> lights,
    UINT edge=kShadowEdge, StaticShadowBakeBounds *bake_bounds=nullptr)
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
        if(bake_bounds && !rd::IsStaticShadowCaster(instance)) continue;
        const auto &bounds=pages[PageOrdinal(instance)].bounds; ValidateBounds(bounds);
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
            const bool terrain=instance.mesh==RenderMesh::Ground || instance.mesh==RenderMesh::DirtPatch;
            const float world_y=instance.position.y+y+(terrain ? instance.scale.y*.5f : 0.0f);
            const float world_z=instance.position.z-x*sine+z*cosine;
            // Capture zeroes grass benders/time. BendGrass still adds its fixed
            // world-space wind envelope: 0.035 X and 0.035*0.7 Z, at most.
            const float wind=HasEnvironmentWind(instance.mesh,instance.environment_variant)?.035f:0.0f;
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
    const float padding=static_cast<float>(std::max(diagonal*2.0/edge,
        coordinate*std::numeric_limits<float>::epsilon()*64.0));
    Require(diagonal>0 && std::isfinite(padding) && padding>0,"Static shadow geometry has no finite projection extent");
    minimum.x-=padding; minimum.y-=padding; minimum.z-=padding;
    maximum.x+=padding; maximum.y+=padding; maximum.z+=padding;
    Require(std::isfinite(minimum.x) && std::isfinite(minimum.y) && std::isfinite(minimum.z) &&
        std::isfinite(maximum.x) && std::isfinite(maximum.y) && std::isfinite(maximum.z) &&
        maximum.x>minimum.x && maximum.y>minimum.y && maximum.z>minimum.z,"Static shadow padded projection is not representable");
    if(bake_bounds) *bake_bounds={minimum,maximum};
    const auto projection=XMMatrixOrthographicOffCenterLH(minimum.x,maximum.x,minimum.y,maximum.y,maximum.z,minimum.z);
    result.enabled=true; result.light_id=primary->stable_id; result.light_generation=primary->generation;
    result.view.scene=scene; result.view.edge=edge;
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
void FillFaceConstants(FrameConstants &c,const LocalProbeCaptureView &view,std::uint32_t generation)
{
    using namespace DirectX;
    const float offline_previous_grid=c.render_options.w;
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
    c.temporal_options={view.irradiance_node?1.f:0.f,0,view.far_distance,view.near_distance};
    c.grass_bender_count={0,0,0,0}; c.previous_grass_state={};
    c.vfx_decal_count={0,0,0,0};
    c.particle_options={0,0,0,0}; c.authored_particle_clock={0,0,0,0}; c.lod_clock={0,0,0,0};
    c.skin_cache_options={0,0,0,generation}; c.lighting_options={0,0,1,1};
    // Captured transport is independent of exposure, screen AO/contact, camera
    // history, gameplay grass benders, VFX/decal lights and post/display settings.
    c.display_options={0,0,0,0}; c.post_options={0,0,0,0}; c.render_options={0,0,0,0};
    c.render_options.w=offline_previous_grid;
}
#endif
} // namespace

#if defined(HS_OFFLINE_PROBE_BAKER)
struct LocalProbeNativeCapture
{
    ComPtr<ID3D12Device> device; ComPtr<D3D12MA::Allocator> allocator; ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>,rd::kAuthoredMaterialNames.size()> materials,shadows;
    ComPtr<ID3D12PipelineState> procedural_shadow;
    ComPtr<ID3D12PipelineState> sky;
};

#endif
struct LocalProbeCaptureState
{
#if defined(HS_OFFLINE_PROBE_BAKER)
    std::unique_ptr<LocalProbeNativeCapture> native;
#endif
    rd::SceneProbeRevisions baseline;
    rd::SceneProbePayload payload;
    std::vector<rd::SceneProbeRecord> probes;
    std::string scene_generation,input_identity;
    Identities environment,material_products;
    std::shared_ptr<const StaticRenderScene> revision_scene;
    std::uint32_t revision_instance_generation{};
    float revision_foliage_density{};
    std::string layout_identity, compatibility_layout_identity;
#if defined(HS_OFFLINE_PROBE_BAKER)
    std::string shadow_identity;
    StaticShadowView revision_shadow;
    std::string light_revision_identity,derived_light_identity;
    Identity light_identity_prefix;
    std::vector<LightView> revision_lights;
    std::array<std::uint32_t,8> revision_frame_lighting{};
    bool light_revision_facts_valid{};
#endif
    std::string material_revision_identity,layout_revision_identity,compatibility_material_identity;
    std::string derived_material_identity;
    rd::SceneProbeRevisions derived_revisions;
    rd::SceneProbeRevisions compatibility_revisions;
    std::vector<LocalProbePageRevision> revision_pages;
    std::uint64_t revision_material_parameter{}, revision_pipeline{};
    std::array<std::uint32_t,3> revision_material_quality{};
    bool material_revision_facts_valid{};
};

namespace
{
bool SameFloat(float a,float b) noexcept
{ return std::bit_cast<std::uint32_t>(a)==std::bit_cast<std::uint32_t>(b); }
bool SamePageRevision(const LocalProbePageRevision &a,const LocalProbePageRevision &b) noexcept
{
    if(a.page_slot!=b.page_slot||a.generation!=b.generation||a.index_count!=b.index_count||
        a.source_digest!=b.source_digest)return false;
    for(std::size_t i=0;i<a.bounds.minimum.size();++i)
        if(!SameFloat(a.bounds.minimum[i],b.bounds.minimum[i])||!SameFloat(a.bounds.maximum[i],b.bounds.maximum[i]))return false;
    for(std::size_t i=0;i<a.bounds.sphere.size();++i)
        if(!SameFloat(a.bounds.sphere[i],b.bounds.sphere[i]))return false;
    return true;
}
bool SameLightRevision(const LightView &a,const LightView &b) noexcept
{
    return a.stable_id==b.stable_id&&a.generation==b.generation&&a.revision==b.revision&&
        a.type==b.type&&a.unit==b.unit&&a.shadow_flags==b.shadow_flags&&
        a.legacy_attenuation==b.legacy_attenuation&&a.two_sided==b.two_sided&&
        SameFloat(a.direction.x,b.direction.x)&&SameFloat(a.direction.y,b.direction.y)&&SameFloat(a.direction.z,b.direction.z)&&
        SameFloat(a.color.x,b.color.x)&&SameFloat(a.color.y,b.color.y)&&SameFloat(a.color.z,b.color.z)&&
        SameFloat(a.position.x,b.position.x)&&SameFloat(a.position.y,b.position.y)&&SameFloat(a.position.z,b.position.z)&&
        SameFloat(a.quantity,b.quantity)&&SameFloat(a.range,b.range)&&SameFloat(a.shape.x,b.shape.x)&&SameFloat(a.shape.y,b.shape.y)&&
        SameFloat(a.emitter_radius,b.emitter_radius)&&SameFloat(a.inner_cone_radians,b.inner_cone_radians)&&
        SameFloat(a.outer_cone_radians,b.outer_cone_radians)&&SameFloat(a.area_tangent.x,b.area_tangent.x)&&
        SameFloat(a.area_tangent.y,b.area_tangent.y)&&SameFloat(a.area_tangent.z,b.area_tangent.z);
}
bool SamePageRevisions(std::span<const LocalProbePageRevision> a,
    std::span<const LocalProbePageRevision> b) noexcept
{
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),SamePageRevision);
}
bool SameLightRevisions(std::span<const LightView> a,std::span<const LightView> b) noexcept
{
    return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),SameLightRevision);
}
}

#if defined(HS_OFFLINE_PROBE_BAKER)
struct LocalProbeCaptureFrame
{
    std::shared_ptr<LocalProbeCaptureState> capture;
    std::shared_ptr<const StaticRenderScene> scene;
    std::vector<LocalProbeCapturePage> pages;
    std::vector<LocalProbeDraw> draws;
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
    ~LocalProbeCaptureFrame() { if(mapped) constants.resource->Unmap(0,nullptr); }
    void BindPrivate(ID3D12GraphicsCommandList *list,UINT bank)
    {
        ID3D12DescriptorHeap *heaps[]{descriptors.Get(),samplers.Get()}; list->SetDescriptorHeaps(2,heaps);
        list->SetGraphicsRootSignature(capture->native->root.Get());
        constexpr std::array<UINT, 5> roots{18, 20, 25, 28, 35};
        // Source-base capture bypasses skin, VFX and instance-reference indirection.
        for (const auto root : roots) list->SetGraphicsRootShaderResourceView(root, 0);
        list->SetGraphicsRootConstantBufferView(15,
            constants.resource->GetGPUVirtualAddress()+UINT64(bank)*kConstantsStride);
        const auto table=[&](UINT offset) {
            auto handle=descriptors->GetGPUDescriptorHandleForHeapStart();
            handle.ptr+=(UINT64(bank)*kCaptureDescriptorCount+offset)*stride; return handle;
        };
        list->SetGraphicsRootDescriptorTable(0,sampler_table);
        list->SetGraphicsRootDescriptorTable(5,table(0));
        list->SetGraphicsRootDescriptorTable(13,table(renderer_detail::kPostTextureDescriptorCount));
        list->SetGraphicsRootDescriptorTable(19,table(kCaptureSkinDescriptorOffset));
        list->SetGraphicsRootDescriptorTable(24,table(renderer_detail::kPostTextureDescriptorCount+
            renderer_detail::kCharacterDescriptorCount+renderer_detail::kMonsterPbrDescriptorCount+
            renderer_detail::kEnvironmentDescriptorCount+renderer_detail::kVfxGradientDescriptorCount+3));
        list->SetGraphicsRootDescriptorTable(26,table(renderer_detail::kVfxDistortionDescriptorOffset));
        list->SetGraphicsRootDescriptorTable(29,table(renderer_detail::kVfxDistortionDescriptorOffset+3));
        list->SetGraphicsRootDescriptorTable(30,table(renderer_detail::kBloomTextureDescriptorOffset));
        list->SetGraphicsRootDescriptorTable(32,table(renderer_detail::kTemporalTextureDescriptorOffset));
        list->SetGraphicsRootDescriptorTable(33,table(renderer_detail::kTemporalTextureDescriptorOffset+1));
        list->SetGraphicsRootDescriptorTable(36,table(renderer_detail::kLayeredTranslucencyDescriptorOffset));
        list->SetGraphicsRootShaderResourceView(34,static_instances->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    void DrawInstances(ID3D12GraphicsCommandList *list,bool shadow_pass,const LocalProbeFrustum *frustum=nullptr)
    {
        std::uint32_t previous_ordinal=UINT32_MAX, previous_page=UINT32_MAX;
        ID3D12PipelineState *previous_pipeline=nullptr;
        ForEachLocalProbeDrawRun(draws,shadow_pass?nullptr:frustum,[&](const LocalProbeDrawRun &run)
        {
            Require(LocalProbeDrawRunValid(run,scene->instances.size()),"Capture source run exceeds packed source/material bounds");
            const auto ordinal=run.material_ordinal; const auto &page=pages[run.page_ordinal];
            if(ordinal!=previous_ordinal)
            {
                const auto &bases=base.material_parameter_bases[ordinal/4];
                const UINT parameter_base=std::array<UINT,4>{bases.x,bases.y,bases.z,bases.w}[ordinal%4];
                list->SetGraphicsRoot32BitConstant(6,(parameter_base<<1)|1u,1);
                previous_ordinal=ordinal;
            }
            auto *pipeline=(shadow_pass?capture->native->shadows[ordinal]:capture->native->materials[ordinal]).Get();
            if(shadow_pass && capture->native->procedural_shadow && run.page_ordinal==rd::kEnvironmentMaterialNames.size())
                pipeline=capture->native->procedural_shadow.Get();
            if(pipeline!=previous_pipeline) { list->SetPipelineState(pipeline); previous_pipeline=pipeline; }
            if(run.page_ordinal!=previous_page)
            {
                list->IASetVertexBuffers(0,1,&page.vertex_view); list->IASetIndexBuffer(&page.index_view);
                previous_page=run.page_ordinal;
            }
            // SV_InstanceID is draw-local; every run carries its exact source base.
            list->SetGraphicsRoot32BitConstant(6,LocalProbePassValue(run),0);
            list->DrawIndexedInstanced(page.index_count,run.instance_count,page.first_index,page.base_vertex,0);
        });
    }
    Result DrawShadow(RenderPassContext &context)
    {
        auto *list=static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        if(!list || !shadow.enabled) return Failure("Static capture shadow pass has no native commands or owner");
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
        list->SetPipelineState(capture->native->sky.Get()); list->DrawInstanced(3,1,0,0);
        std::array<float,16> matrix{}; std::memcpy(matrix.data(),&view.view_projection,sizeof(matrix));
        const auto frustum=MakeLocalProbeFrustum(matrix);
        DrawInstances(list,false,&frustum);
        return Result::Success();
    }
};
#endif
struct LocalProbeRuntime::Impl
{
    LocalProbeSystem service;
    std::shared_ptr<LocalProbeCaptureState> capture;
    const RenderGraphBuilder *pending_graph{};
#if defined(HS_OFFLINE_PROBE_BAKER)
    std::shared_ptr<LocalProbeCaptureFrame> pending_frame;
    std::vector<std::shared_ptr<LocalProbeCaptureFrame>> retired_frames;
    std::weak_ptr<const StaticRenderScene> material_scene;
    std::uint32_t material_foliage_density_bits{};
    std::vector<std::uint32_t> material_ordinals;
#endif
};
LocalProbeRuntime::LocalProbeRuntime():impl_(std::make_unique<Impl>()) {}
LocalProbeRuntime::~LocalProbeRuntime()=default;
#if defined(HS_OFFLINE_PROBE_BAKER)
Result LocalProbeRuntime::PrepareCaptureMaterialOrdinals(const std::shared_ptr<const StaticRenderScene> &scene,
    float foliage_density)
{
    const auto density_bits=std::bit_cast<std::uint32_t>(foliage_density);
    if(impl_->material_scene.lock()==scene&&impl_->material_foliage_density_bits==density_bits)return Result::Success();
    if(!scene)
    { impl_->material_scene.reset();impl_->material_foliage_density_bits=density_bits;impl_->material_ordinals.clear();return Result::Success(); }
    try
    {
        std::vector<bool> used(rd::kAuthoredMaterialNames.size());
        std::vector<std::uint32_t> ordinals;
        for(std::size_t index=0;index<scene->instances.size();++index)
        {
            const auto &instance=scene->instances[index];
            if(RetainDecorativeFoliage(instance,index,foliage_density))used[MaterialOrdinal(instance)]=true;
        }
        for(std::uint32_t ordinal=0;ordinal<used.size();++ordinal)if(used[ordinal])ordinals.push_back(ordinal);
        impl_->material_scene=scene;impl_->material_foliage_density_bits=density_bits;
        impl_->material_ordinals=std::move(ordinals);
        return Result::Success();
    }
    catch(const std::exception &error){return Failure(error.what());}
}
std::span<const std::uint32_t> LocalProbeRuntime::CachedCaptureMaterialOrdinals(
    const std::shared_ptr<const StaticRenderScene> &scene,float foliage_density) const noexcept
{
    return impl_->material_scene.lock()==scene&&impl_->material_foliage_density_bits==std::bit_cast<std::uint32_t>(foliage_density)
        ?std::span<const std::uint32_t>(impl_->material_ordinals):std::span<const std::uint32_t>{};
}
bool LocalProbeRuntime::CaptureIdentityPending(const CompletionSet &completed) const noexcept
{ return impl_->service.CaptureIdentityPending(completed); }

namespace
{
// Both capture paths clone the canonical mixed CBV/SRV/UAV table. Its layout
// is initialized centrally by InitializeCanonicalDescriptors, including inactive features.
void CopyCaptureDescriptors(ID3D12Device *device,D3D12_CPU_DESCRIPTOR_HANDLE destination,
    D3D12_CPU_DESCRIPTOR_HANDLE source)
{
    device->CopyDescriptorsSimple(kCaptureDescriptorCount,destination,source,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Static capture owns no dynamic history or view skin input.
    const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    destination.ptr+=SIZE_T(kCaptureSkinDescriptorOffset)*stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC empty{};
    empty.Format=DXGI_FORMAT_R32_TYPELESS; empty.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
    empty.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    empty.Buffer.NumElements=sizeof(GpuSceneTableHeader)/sizeof(UINT);
    empty.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(nullptr,&empty,destination);
    destination.ptr+=stride;
    empty.Buffer.NumElements=sizeof(GpuSceneSkinEntry)/sizeof(UINT);
    device->CreateShaderResourceView(nullptr,&empty,destination);
}
Result PrepareCapturePipelines(const std::shared_ptr<LocalProbeCaptureState> &capture,
    const RendererConfig &config, RendererPipelineCache *pipeline_cache, bool color,
    std::span<const bool> selected={}, INT depth_bias=-800, bool procedural_two_sided=false)
{
    if(!capture || !capture->native) return Failure("Capture pipelines require a layout capture owner.");
    auto *root=capture->native->root.Get();
    assert(selected.empty() || selected.size()==rd::kAuthoredMaterialNames.size());
    const auto selected_material=[&](std::size_t i) { return selected.empty() || selected[i]; };
    // Each capture owner retains one root/bias variant for its lifetime.
    bool needs_material{},needs_shadow{};
    for(std::size_t i=0;i<rd::kAuthoredMaterialNames.size();++i)
    {
        if(!selected_material(i))continue;
        needs_material|=color&&!capture->native->materials[i];
        needs_shadow|=!capture->native->shadows[i] ||
            (procedural_two_sided && i==rd::kTerrainMaterialOrdinal && !capture->native->procedural_shadow);
    }
    const bool needs_sky=color&&!capture->native->sky;
    if(!needs_material&&!needs_shadow&&!needs_sky)return Result::Success();
    const auto vertex=needs_material||needs_shadow
        ?ReadProduct(config,"render/shader/local_probe_capture_source_base_vs.dxil"):std::vector<std::byte>{};
    constexpr D3D12_INPUT_ELEMENT_DESC layout[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDINDICES",0,DXGI_FORMAT_R16G16B16A16_UINT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"BLENDWEIGHT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,48,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TANGENT",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,56,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"MATERIAL",0,DXGI_FORMAT_R16_UINT,0,72,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"OPTICAL_COMPONENT",0,DXGI_FORMAT_R16_UINT,0,74,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
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
    std::vector<std::size_t> work;
    work.reserve(rd::kAuthoredMaterialNames.size());
    for(std::size_t i=0;i<rd::kAuthoredMaterialNames.size();++i)
    {
        if(!selected_material(i)) continue;
        const bool create_material=color&&!capture->native->materials[i];
        const bool create_shadow=!capture->native->shadows[i];
        const bool create_procedural_shadow=procedural_two_sided && i==rd::kTerrainMaterialOrdinal && !capture->native->procedural_shadow;
        if(create_material||create_shadow||create_procedural_shadow) work.push_back(i);
    }
    const auto prepare_material=[&](std::size_t i)->Result
    {
        auto material=p;
        material.RasterizerState.CullMode=Masked(i)?D3D12_CULL_MODE_NONE:D3D12_CULL_MODE_BACK;
        const bool create_material=color&&!capture->native->materials[i];
        const bool create_shadow=!capture->native->shadows[i];
        const bool create_procedural_shadow=procedural_two_sided && i==rd::kTerrainMaterialOrdinal && !capture->native->procedural_shadow;
        if(create_material)
        {
            try
            {
                const auto pixel=ReadProduct(config,"render/shader/"+std::string(rd::kAuthoredMaterialNames[i])+"_probe_capture.dxil");
                material.PS={pixel.data(),pixel.size()};
                material.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
                const auto result=pipeline_cache->CreateGraphics(&material,IID_PPV_ARGS(&capture->native->materials[i]));
                if(FAILED(result)) return Failure("Create static local-probe material capture PSO failed (HRESULT "+std::to_string(std::int32_t(result))+").");
            }
            catch(const std::exception &error) { return Failure(error.what()); }
            catch(...) { return Failure("Create static local-probe material capture PSO failed with an unknown exception."); }
        }
        if(create_shadow||create_procedural_shadow)
        {
            try
            {
                const auto shadow_pixel=ReadProduct(config,"render/shader/"+std::string(rd::kAuthoredMaterialNames[i])+"_probe_shadow.dxil");
                auto depth=material; depth.RasterizerState.CullMode=Masked(i)?D3D12_CULL_MODE_NONE:D3D12_CULL_MODE_BACK; depth.PS={shadow_pixel.data(),shadow_pixel.size()}; depth.NumRenderTargets=0;
                for(auto &format:depth.RTVFormats) format=DXGI_FORMAT_UNKNOWN;
                // Virtual pages use zero constant bias; probes retain their directional bias.
                depth.RasterizerState.DepthBias=depth_bias; depth.RasterizerState.SlopeScaledDepthBias=-1.5f;
                if(create_shadow)
                {
                    const auto result=pipeline_cache->CreateGraphics(&depth,IID_PPV_ARGS(&capture->native->shadows[i]));
                    if(FAILED(result)) return Failure("Create pure-static capture shadow PSO failed (HRESULT "+std::to_string(std::int32_t(result))+").");
                }
                if(create_procedural_shadow)
                {
                    depth.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
                    const auto result=pipeline_cache->CreateGraphics(&depth,IID_PPV_ARGS(&capture->native->procedural_shadow));
                    if(FAILED(result)) return Failure("Create virtual procedural shadow PSO failed (HRESULT "+std::to_string(std::int32_t(result))+").");
                }
            }
            catch(const std::exception &error) { return Failure(error.what()); }
            catch(...) { return Failure("Create local-probe shadow PSO failed with an unknown exception."); }
        }
        return Result::Success();
    };
    std::vector<Result> work_results(work.size(),Result::Success());
    auto *tasks=config.task_system;
    if(work.size()>1 && tasks)
    {
        auto group=tasks->CreateTaskGroup();
        std::size_t submitted{};
        Result dispatch=Result::Success();
        for(;submitted<work.size();++submitted)
        {
            try
            {
                const auto index=submitted;
                if(!group.Submit([&,index](std::stop_token) { work_results[index]=prepare_material(work[index]); }))
                { dispatch=Failure("Submit local-probe PSO preparation task rejected."); break; }
            }
            catch(const std::exception &error)
            {
                dispatch=Failure(std::string("Submit local-probe PSO preparation task: ")+error.what());
                break;
            }
            catch(...) { dispatch=Failure("Submit local-probe PSO preparation task failed with an unknown exception."); break; }
        }
        Result joined=Result::Success();
        if(submitted)
        {
            try { joined=group.Wait(); }
            catch(const std::exception &error) { joined=Failure(std::string("Join local-probe PSO preparation tasks: ")+error.what()); }
            catch(...) { joined=Failure("Join local-probe PSO preparation tasks failed with an unknown exception."); }
        }
        for(std::size_t i=0;i<submitted;++i) if(!work_results[i]) return work_results[i];
        if(!dispatch) return dispatch;
        if(!joined) return joined;
    }
    else
    {
        for(std::size_t i=0;i<work.size();++i)
        {
            work_results[i]=prepare_material(work[i]);
            if(!work_results[i]) return work_results[i];
        }
    }
    if(needs_sky)
    {
        try
        {
            const auto fullscreen=ReadProduct(config,"render/shader/fullscreen_vs.dxil");
            const auto sky=ReadProduct(config,"render/shader/local_probe_capture_sky_ps.dxil");
            p.VS={fullscreen.data(),fullscreen.size()}; p.PS={sky.data(),sky.size()}; p.InputLayout={};
            p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE; p.DepthStencilState.DepthEnable=FALSE;
            p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
            const auto result=pipeline_cache->CreateGraphics(&p,IID_PPV_ARGS(&capture->native->sky));
            if(FAILED(result)) return Failure("Create local-probe global sky capture PSO failed (HRESULT "+std::to_string(std::int32_t(result))+").");
        }
        catch(const std::exception &error) { return Failure(error.what()); }
        catch(...) { return Failure("Create local-probe global sky capture PSO failed with an unknown exception."); }
    }
#ifndef NDEBUG
    if(color)assert(capture->native->sky);
    for(std::size_t i=0;i<rd::kAuthoredMaterialNames.size();++i)
        if(selected_material(i))assert((!color||capture->native->materials[i])&&capture->native->shadows[i]);
#endif
    return Result::Success();
}
} // namespace

#endif
// All retained sources belong to the offline renderer until its queue is drained.
#if defined(HS_OFFLINE_PROBE_BAKER)
Result D3D12Renderer::Impl::PrepareSceneProbeBakeSources(SceneProbeBakeSources &sources)
{
    try
    {
        Require(sources.textures.empty(), "Scene-probe sources are already prepared");
        std::map<std::string, std::size_t> textures;
        std::uint64_t total_bytes{};
        for (const auto &material : environment_material_bindings)
            for (const auto &id : material.product_ids)
            {
                if (id.empty() || textures.contains(id)) continue;
                const auto bytes = ReadProduct(config, id);
                const auto package = ReadRendererTexturePackage(config, id, bytes);
                const auto &source = package.description;
                Require(source.array_size && source.array_size <= UINT16_MAX && source.mip_count &&
                    source.mip_count <= UINT16_MAX && package.subresources.size() == source.array_size * source.mip_count,
                    "Scene-probe texture has an invalid array or mip count");
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                desc.Width = source.width; desc.Height = source.height;
                desc.DepthOrArraySize = static_cast<UINT16>(source.array_size);
                desc.MipLevels = static_cast<UINT16>(source.mip_count);
                desc.Format = static_cast<DXGI_FORMAT>(source.format); desc.SampleDesc.Count = 1;
                sources.textures.emplace_back();
                auto &target = sources.textures.back(); target.id = id;
                D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
                if (auto created = CreateAllocation(target.resource, allocation, desc, D3D12_RESOURCE_STATE_COPY_DEST); !created)
                    return created;
                const auto size = target.resource.allocation->GetSize();
                Require(size <= UINT64_MAX - total_bytes, "Scene-probe texture allocation sum overflows");
                total_bytes += size;
                Require(!config.texture_budget_mib || total_bytes <= std::uint64_t(config.texture_budget_mib) * 1024 * 1024,
                    "Scene-probe texture set exceeds the configured texture residency budget");
                const auto count = source.array_size * source.mip_count;
                std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
                std::vector<UINT> rows(count); std::vector<UINT64> row_bytes(count);
                UINT64 upload_size{};
                device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), row_bytes.data(), &upload_size);
                Require(upload_size && upload_size <= 512ull * 1024 * 1024,
                    "Scene-probe texture exceeds the host staging budget");
                std::vector<AllocationResource> uploads(1);
                allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
                if (auto created = CreateAllocation(uploads.front(), allocation, BufferDescription(upload_size),
                        D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
                std::byte *mapped{}; D3D12_RANGE no_read{};
                Native(uploads.front().resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped)),
                    "Map scene-probe texture upload");
                for (UINT sub = 0; sub < count; ++sub)
                {
                    const auto &part = package.subresources[sub];
                    const auto &footprint = footprints[sub];
                    Require(rows[sub] && part.row_pitch == row_bytes[sub] &&
                        UINT64(rows[sub]) * part.row_pitch == part.bytes.size() &&
                        footprint.Offset <= upload_size && row_bytes[sub] <= footprint.Footprint.RowPitch &&
                        UINT64(rows[sub] - 1) * footprint.Footprint.RowPitch + row_bytes[sub] <= upload_size - footprint.Offset,
                        "Scene-probe texture upload footprint differs from its admitted package");
                    for (UINT row = 0; row < rows[sub]; ++row)
                        std::memcpy(mapped + footprint.Offset + SIZE_T(row) * footprint.Footprint.RowPitch,
                            part.bytes.data() + SIZE_T(row) * part.row_pitch, static_cast<std::size_t>(row_bytes[sub]));
                }
                uploads.front().resource->Unmap(0, nullptr);
                Native(frames[0].allocator->Reset(), "Reset scene-probe texture allocator");
                Native(command_list->Reset(frames[0].allocator.Get(), nullptr), "Reset scene-probe texture list");
                for (UINT sub = 0; sub < count; ++sub)
                {
                    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = target.resource.resource.Get();
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = sub;
                    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = uploads.front().resource.Get();
                    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprints[sub];
                    command_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                }
                D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = {target.resource.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
                command_list->ResourceBarrier(1, &barrier);
                Native(command_list->Close(), "Close scene-probe texture list");
                if (auto submitted = SubmitResidentUtilityUpload(command_list.Get(), frames[0].allocator.Get(), uploads); !submitted)
                    return submitted;
                target.resource.graph_access = Access::AllShaderRead;
                target.resource.graph_queue = QueueHint::Direct;
                textures.emplace(id, sources.textures.size() - 1);
            }
        constexpr std::array legacy_names{"terrain_gaussian", "terrain_surface", "terrain_normal", "terrain_detail",
            "terrain_height", "rock_basecolor", "rock_surface", "rock_normal", "rock_detail", "bark_basecolor",
            "bark_surface", "bark_normal", "bark_detail", "grass_basecolor", "grass_normal", "grass_roughness",
            "leaf_basecolor", "leaf_normal", "leaf_roughness", "terrain_histogram"};
        static_assert(legacy_names.size() == kEnvironmentTextureCount);
        for (UINT frame = 0; frame < kFrameCount; ++frame)
        {
            for (UINT material = 0; material < environment_material_bindings.size(); ++material)
                for (UINT slot = 0; slot < rd::kMaterialTextureSlots; ++slot)
                {
                    const auto &id = environment_material_bindings[material].product_ids[slot];
                    const auto found = textures.find(id);
                    WriteEnvironmentMaterialTextureDescriptor(frame, material, slot,
                        found == textures.end() ? nullptr : sources.textures[found->second].resource.resource.Get());
                }
            for (UINT index = 0; index < legacy_names.size(); ++index)
            {
                const auto id = std::string("render/texture/environment_") + legacy_names[index] + ".rpkg";
                const auto found = textures.find(id);
                Require(found != textures.end(), "Scene-probe legacy environment texture is missing: " + id);
                auto *resource = sources.textures[found->second].resource.resource.Get();
                const auto desc = resource->GetDesc();
                WriteEnvironmentTextureDescriptor(frame, index, resource, desc.Format, desc.MipLevels, desc.DepthOrArraySize);
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC metadata{};
            metadata.Format = DXGI_FORMAT_UNKNOWN; metadata.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            metadata.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            metadata.Buffer.NumElements = kTextureFeedbackBindingCount;
            metadata.Buffer.StructureByteStride = sizeof(UINT) * 4;
            auto handle = srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + kTextureFeedbackDescriptorOffset) * srv_stride;
            device->CreateShaderResourceView(nullptr, &metadata, handle);
        }
        if (auto brdf = CreateEnvironmentBrdfLut(config, environment_brdf_lut); !brdf) return brdf;
        WriteEnvironmentBrdfLutDescriptors();
        LightingProbeCandidate probe;
        if (auto prepared = PrepareLightingProbe(config, probe); !prepared) return prepared;
        if (auto committed = lighting_probe.Commit(std::move(probe), graph_queue_executor.Completed()); !committed) return committed;
        LtcLutCandidate ltc;
        if (auto prepared = PrepareLtcLut(config, ltc); !prepared) return prepared;
        if (auto committed = ltc_lut.Commit(std::move(ltc), graph_queue_executor.Completed()); !committed) return committed;
        LocalProbeRuntimeCandidate local;
        if (auto prepared = local_probes.Prepare(device.Get(), allocator, root_signature.Get(), config,
                resource_generation_id, local, &pipeline_cache); !prepared) return prepared;
        if (auto committed = local_probes.Commit(std::move(local), graph_queue_executor.Completed()); !committed) return committed;
        auto &pool = owned_views.at(kMainRenderViewId.value)->targets.transient_pools[0];
        return pool.Initialize(allocator, device.Get());
    }
    catch (const std::exception &error) { return Failure(error.what()); }
}
#endif
Result LocalProbeRuntime::Prepare(ID3D12Device *device,D3D12MA::Allocator *allocator,ID3D12RootSignature *root,
    const RendererConfig &config,std::string_view scene_generation,
    LocalProbeRuntimeCandidate &candidate,RendererPipelineCache *pipeline_cache) const
{
    candidate=LocalProbeRuntimeCandidate{};
    if(config.gi_mode==GraphicsGiMode::Off && config.reflection_mode==GraphicsReflectionMode::Off)
    { candidate.prepared_=true; return Result::Success(); }
    try
    {
        Require(device && allocator && root && pipeline_cache &&
            (config.packed_content || (!config.content_generation_id.empty() && !scene_generation.empty())),
            "Active local probes need native owners, pipeline cache and selected/resident scene Generations");
        const auto bytes=ReadProduct(config,kProduct); const auto package=renderer_detail::MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return rd::ReadSceneProbePackage(bytes,MakeAssetId(kProduct)); });
        auto capture=std::make_shared<LocalProbeCaptureState>();
#if defined(HS_OFFLINE_PROBE_BAKER)
        if(package.payload!=rd::SceneProbePayload::Baked)
        {
            capture->native=std::make_unique<LocalProbeNativeCapture>();
            capture->native->device=device; capture->native->allocator=allocator; capture->native->root=root;
        }
#else
        Require(package.payload==rd::SceneProbePayload::Baked,"Runtime requires baked scene lighting; cook the selected generation.");
#endif
        capture->baseline=package.description.revisions; capture->payload=package.payload; capture->probes=package.probes;
        capture->scene_generation=scene_generation;
        Require(package.dependencies[package.description.environment_dependency]==kEnvironment,"Local package references a different global environment");
        const auto environment=ReadProduct(config,kEnvironment);
        (void)renderer_detail::MeasureStartupOperation(RendererStartupOperation::PackageDecodeValidate, [&] { return rd::ReadLightingProbePackage(environment,MakeAssetId(kEnvironment)); });
        capture->environment.emplace(kEnvironment,RendererProductDigest(config,kEnvironment));
        Require(Revision(rd::SceneProbeRevisionDomain::Environment,capture->environment)==capture->baseline.environment,
            "Local package baseline environment revision is stale for the selected Generation");
        Identities dependency_digests;
        for(const auto &id:package.dependencies)
        {
            RendererAssetSource source;
            auto digest=RendererProductDigest(config,id,false);
            Require(config.packed_content ? !digest.empty() :
                static_cast<bool>(ResolveRendererAsset(config,id,source,false)),"Local package dependency is absent: "+id);
            dependency_digests.emplace(id,std::move(digest));
        }
        std::vector<rd::SceneProbeRevisionInput> source_inputs; source_inputs.reserve(package.dependencies.size());
        for(const auto &id:package.dependencies)
        {
            const auto &digest=dependency_digests.at(id);
            Require(digest.starts_with("sha256:") && digest.size()==71,"Invalid local-probe source dependency digest: "+id);
            source_inputs.push_back({id,std::string_view(digest).substr(7)});
        }
        capture->input_identity=rd::DeriveSceneProbeInputIdentity(package.description,package.probes,
            package.dependencies,source_inputs);
        // Keep the four validated package baselines; native revisions below fold
        // real retained scene/lights/parameters/products rather than forging them.
        if(config.packed_content)
        {
            for(const auto &source:config.packed_source_revisions)
                if(rd::IsLocalProbeMaterialRevisionProduct(source.product_id))
                    capture->material_products.emplace(source.product_id, "sha256:"+Hex(source.digest));
            for(const auto &entry:config.packed_revision_bulks)
                if(rd::IsLocalProbeMaterialRevisionProduct(entry.product_id))
                    capture->material_products.emplace(entry.product_id,
                        "sha256:"+Hex(config.packed_bulks[entry.bulk_index].range.expected_digest));
        }
        else for(const auto &[id,digest]:config.product_digests)
            if(rd::IsLocalProbeMaterialRevisionProduct(id))
                capture->material_products.emplace(id,digest);
        capture->material_products["scene-probe/semantic-inputs"]=capture->input_identity;
#if defined(HS_OFFLINE_PROBE_BAKER)
        if(package.payload!=rd::SceneProbePayload::Baked)
            if(auto pipelines=PrepareCapturePipelines(capture,config,pipeline_cache,true);!pipelines) return pipelines;
#endif
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
#if defined(HS_OFFLINE_PROBE_BAKER)
    for(const auto &frame:impl_->retired_frames) if(!frame->completion.IsSatisfiedBy(completed)) return Failure("Capture leases are still in flight");
#endif
    if(candidate.capture_)
    { if(auto committed=impl_->service.Commit(std::move(candidate.service_),completed);!committed) return committed; }
    else if(auto reset=impl_->service.ResetAfterCompletion(completed);!reset) return reset;
    impl_->capture=std::move(candidate.capture_);
#if defined(HS_OFFLINE_PROBE_BAKER)
    impl_->retired_frames.clear();
#endif
     candidate.prepared_=false;
    return Result::Success();
}
Result LocalProbeRuntime::CurrentRevisions(const LocalProbeRuntimeFrameInputs &in,rd::SceneProbeRevisions &out) const
{
    out={}; const auto capture=impl_->capture; if(!capture || !in.scene) return Result::Success();
    try
    {
        Require(in.scene && in.frame_constants && in.scene->content_generation_id==capture->scene_generation && in.static_instance_generation,
            "Native local-probe scene generation does not match adoption");
        std::vector<LocalProbePageRevision> owned_page_facts;
        std::span<const LocalProbePageRevision> page_facts=in.page_revisions;
        if(page_facts.empty()&&!in.pages.empty())
        {
            owned_page_facts.reserve(in.pages.size());
            for(const auto &page:in.pages)
                owned_page_facts.push_back({page.page_slot,page.generation,page.index_count,page.source_digest,page.bounds});
            page_facts=owned_page_facts;
        }
        Require(page_facts.size()==rd::kEnvironmentMaterialNames.size()+1,
            "Native local-probe revision facts do not match the complete page catalog");
        if(!in.pages.empty())
        {
            Require(in.pages.size()==page_facts.size(),"Native local-probe capture pages do not match revision facts");
            for(std::size_t i=0;i<in.pages.size();++i)
            {
                const auto &page=in.pages[i];
                Require(page.complete&&page.generation&&page.index_count,
                    "Capture geometry is not an actual complete page");
                Require(std::ranges::any_of(page.source_digest,[](std::byte value) { return value!=std::byte{}; }),
                    "Capture geometry is missing its decoded source digest");
                ValidateBounds(page.bounds);
                const LocalProbePageRevision actual{page.page_slot,page.generation,page.index_count,page.source_digest,page.bounds};
                Require(SamePageRevision(actual,page_facts[i]),"Capture geometry changed after revision qualification");
            }
        }
        // The retained scene is immutable; revalidate its layout only when its adoption or density changes.
        const bool layout_owner_changed=capture->revision_scene!=in.scene ||
            capture->revision_instance_generation!=in.static_instance_generation ||
            capture->revision_foliage_density!=in.foliage_density;
        if(layout_owner_changed)
        {
            // Native Generation/instance ids validate resource compatibility but
            // are not inputs to the sampled static layout. The baked package is
            // adopted across the layout-only/baked Generation boundary, whose
            // product closure and geometry are already hashed below.
            Identity layout; layout.Text("local-probe.static-scene-compatibility.v1");
            layout.Integer(in.scene->layout_abi,4);
            layout.Integer(in.scene->instances.size());
            for(const auto &instance:in.scene->instances)
            {
                (void)MaterialOrdinal(instance); layout.Integer(instance.stable_id); layout.Integer(static_cast<std::uint32_t>(instance.mesh),4);
                layout.Vector(instance.position); layout.Vector(instance.scale); layout.Float(instance.yaw);
                layout.Integer(instance.color_rgba,4); layout.Integer(instance.environment_seed,4); layout.Integer(instance.environment_variant,4);
                layout.Integer(instance.asset_id.value); layout.Integer(instance.material_id.value);
                layout.Integer(instance.revisions.transform); layout.Integer(instance.revisions.material); layout.Integer(instance.revisions.content);
            }
            auto capture_layout=layout;
            capture_layout.Float(in.foliage_density);
            capture->layout_identity=capture_layout.Finish();
            capture->compatibility_layout_identity=layout.Finish();
            capture->revision_scene=in.scene;
            capture->revision_instance_generation=in.static_instance_generation;
            capture->revision_foliage_density=in.foliage_density;
        }
        const auto &quality=in.frame_constants->material_quality_options;
        const std::array material_quality{std::bit_cast<std::uint32_t>(quality.x),
            std::bit_cast<std::uint32_t>(quality.y),std::bit_cast<std::uint32_t>(quality.z)};
        const bool material_inputs_changed=!capture->material_revision_facts_valid||
            capture->revision_material_parameter!=in.material_parameter_revision||
            capture->revision_pipeline!=in.pipeline_revision||capture->revision_material_quality!=material_quality||
            !SamePageRevisions(capture->revision_pages,page_facts);
        if(material_inputs_changed)
        {
            Identity material; material.Text("local-probe.material-compatibility.v2");
            // pipeline_revision is a renderer-wide refresh trigger. Keep it in
            // material_inputs_changed above, but let the scoped product digests
            // below determine whether probe capture semantics actually changed.
            material.Integer(in.material_parameter_revision);
            for(const auto &page:page_facts)
            {
                Require(page.generation&&page.index_count,"Capture geometry is not an actual complete page");
                Require(std::ranges::any_of(page.source_digest,[](std::byte value) { return value!=std::byte{}; }),
                    "Capture geometry is missing its decoded source digest");
                ValidateBounds(page.bounds);
            }
            // Authored page payloads, including every LOD, are already covered
            // by the scoped immutable Generation product digests. Keep the
            // internal procedural cube in the semantic identity because it has
            // no catalog product and is generated by renderer code.
            const auto &procedural=page_facts.back();
            material.Bytes(procedural.source_digest); material.Integer(procedural.index_count,4);
            for(float value:procedural.bounds.minimum) material.Float(value);
            for(float value:procedural.bounds.maximum) material.Float(value);
            for(float value:procedural.bounds.sphere) material.Float(value);
            // Static capture evaluates surface lighting, never screen-space refraction (w).
            auto capture_material=material;
            capture_material.Float(quality.x); capture_material.Float(quality.y); capture_material.Float(quality.z);
            capture->material_revision_identity=capture_material.Finish();
            capture->compatibility_material_identity=material.Finish();
            capture->revision_pages.assign(page_facts.begin(),page_facts.end());
            capture->revision_material_parameter=in.material_parameter_revision;
            capture->revision_pipeline=in.pipeline_revision;
            capture->revision_material_quality=material_quality;
            capture->material_revision_facts_valid=true;
        }
        const auto material_identity=capture->material_revision_identity;
        if(capture->derived_material_identity!=material_identity)
        {
            auto materials=capture->material_products; materials["native/material-and-complete-page-state"]=material_identity;
            capture->derived_revisions.material=Revision(rd::SceneProbeRevisionDomain::Material,materials);
            materials["native/material-and-complete-page-state"]=capture->compatibility_material_identity;
            capture->compatibility_revisions.material=Revision(rd::SceneProbeRevisionDomain::Material,materials);
            capture->derived_material_identity=material_identity;
        }
        if(capture->layout_revision_identity!=capture->layout_identity)
        {
            const Identities layout_inputs{{"scene-probe/semantic-inputs",capture->input_identity},
                {"native/immutable-static-scene",capture->layout_identity}};
            capture->derived_revisions.static_layout=Revision(rd::SceneProbeRevisionDomain::StaticLayout,layout_inputs);
            const Identities compatibility_inputs{{"scene-probe/semantic-inputs",capture->input_identity},
                {"native/immutable-static-scene",capture->compatibility_layout_identity}};
            capture->compatibility_revisions.static_layout=Revision(rd::SceneProbeRevisionDomain::StaticLayout,compatibility_inputs);
            capture->layout_revision_identity=capture->layout_identity;
        }
        if(capture->payload==rd::SceneProbePayload::Baked)
        {
            out=capture->compatibility_revisions;
            out.environment=capture->baseline.environment;
            out.light=capture->baseline.light;
            return Result::Success();
        }
#if defined(HS_OFFLINE_PROBE_BAKER)
        const auto &light_direction=in.frame_constants->light_direction_intensity;
        const auto &light_color=in.frame_constants->light_color;
        const std::array frame_lighting{std::bit_cast<std::uint32_t>(light_direction.x),
            std::bit_cast<std::uint32_t>(light_direction.y),std::bit_cast<std::uint32_t>(light_direction.z),
            std::bit_cast<std::uint32_t>(light_direction.w),
            std::bit_cast<std::uint32_t>(light_color.x),std::bit_cast<std::uint32_t>(light_color.y),
            std::bit_cast<std::uint32_t>(light_color.z),std::bit_cast<std::uint32_t>(light_color.w)};
        const bool light_inputs_changed=!capture->light_revision_facts_valid||
            capture->revision_frame_lighting!=frame_lighting||!SameLightRevisions(capture->revision_lights,in.lights);
        if(light_inputs_changed)
        {
            Identity input_lights; input_lights.Text("local-probe.actual-light-view.v1"); input_lights.Integer(in.lights.size());
            for(const auto &light:in.lights)
            {
                input_lights.Integer(light.stable_id); input_lights.Integer(light.generation,4); input_lights.Integer(light.revision);
                input_lights.Integer(static_cast<std::uint32_t>(light.type),4); input_lights.Integer(static_cast<std::uint32_t>(light.unit),4);
                input_lights.Vector(light.direction); input_lights.Vector(light.color); input_lights.Vector(light.position); input_lights.Float(light.quantity);
                input_lights.Float(light.range); input_lights.Float(light.shape.x); input_lights.Float(light.shape.y); input_lights.Float(light.emitter_radius);
                input_lights.Float(light.inner_cone_radians); input_lights.Float(light.outer_cone_radians); input_lights.Vector(light.area_tangent);
                input_lights.Integer(light.shadow_flags,4); input_lights.Integer(light.legacy_attenuation,1); input_lights.Integer(light.two_sided,1);
            }
            input_lights.Vector(in.frame_constants->light_direction_intensity); input_lights.Vector(in.frame_constants->light_color);
            capture->light_identity_prefix=input_lights;
            capture->revision_lights.assign(in.lights.begin(),in.lights.end());
            capture->revision_frame_lighting=frame_lighting;
            capture->light_revision_facts_valid=true;
        }
        const bool shadow_inputs_changed=layout_owner_changed||light_inputs_changed||material_inputs_changed||
            capture->shadow_identity.empty();
        if(shadow_inputs_changed)
        {
            auto shadow_key=capture->light_identity_prefix;
            shadow_key.Text("local-probe.static-shadow-view-cache.v2");
            shadow_key.Text(capture->layout_identity); shadow_key.Text(material_identity);
            shadow_key.Integer(page_facts.size());
            for(std::size_t index=0;index<page_facts.size();++index)
            {
                const auto &page=page_facts[index];
                shadow_key.Integer(index,4); shadow_key.Integer(page.page_slot,4);
                shadow_key.Bytes(page.source_digest); shadow_key.Integer(page.index_count,4);
                for(float value:page.bounds.minimum) shadow_key.Float(value);
                for(float value:page.bounds.maximum) shadow_key.Float(value);
                for(float value:page.bounds.sphere) shadow_key.Float(value);
            }
            const auto shadow_identity=shadow_key.Finish();
            if(capture->shadow_identity!=shadow_identity)
            {
                const auto shadow=MakeStaticShadowView(in.scene,
                    std::span<const LocalProbePageRevision>(page_facts),in.lights);
                capture->revision_shadow=shadow;
                capture->shadow_identity=shadow_identity;
            }
            auto lights=capture->light_identity_prefix;
            const auto &shadow=capture->revision_shadow;
            lights.Text("local-probe.pure-static-directional-shadow-policy.v2"); lights.Integer(kShadowEdge,4);
            lights.Integer(shadow.enabled,1); lights.Integer(shadow.light_id); lights.Integer(shadow.light_generation,4);
            lights.Integer(std::bit_cast<std::uint32_t>(std::int32_t(-800)),4); lights.Float(-1.5f);
            capture->light_revision_identity=lights.Finish();
        }
        const auto light_identity=capture->light_revision_identity;
        if(capture->derived_light_identity!=light_identity)
        {
            const Identities light_inputs{{"scene-probe/semantic-inputs",capture->input_identity},
                {"native/authoritative-light-view",light_identity}};
            capture->derived_revisions.light=Revision(rd::SceneProbeRevisionDomain::Light,light_inputs);
            capture->derived_light_identity=light_identity;
        }
        out=capture->derived_revisions;
        out.environment=capture->baseline.environment;
        return Result::Success();
#else
        return Failure("Unexpected non-baked lighting package.");
#endif
    }
    catch(const std::exception &error) { return Failure(error.what()); }
}
Result LocalProbeRuntime::PrepareFrameDecisionData(const LocalProbeRuntimeFrameInputs &in,
    LocalProbeRuntimeFramePlan &plan) const
{
    plan={};plan.source=&in;plan.scene=in.scene;
    plan.static_instance_generation=in.static_instance_generation;
    plan.material_parameter_revision=in.material_parameter_revision;
    plan.pipeline_revision=in.pipeline_revision;
#if defined(HS_OFFLINE_PROBE_BAKER)
    plan.light_count=in.lights.size();plan.page_count=in.pages.size();
    plan.scene_read_count=in.scene_reads.size();plan.max_cube_views=in.max_cube_views;
    plan.capture_identity_valid=in.texture_facts_valid;
    if(plan.capture_identity_valid)
    {
        plan.capture_identity=CaptureBindingIdentity(in.texture_facts,in.page_revisions,in.pages);
        plan.capture_sources_ready=CaptureTexturesReady(in.texture_facts);
    }
#endif
    if(!impl_->capture)return Result::Success();
    plan.active=true;
    if(!in.scene)return Result::Success();
    if(auto qualified=CurrentRevisions(in,plan.revisions);!qualified)return qualified;
#if defined(HS_OFFLINE_PROBE_BAKER)
    if(in.max_cube_views>6)return Failure("Capture cube-view budget exceeds six");
    plan.shadow=impl_->capture->revision_shadow.enabled;
    plan.capture=in.max_cube_views&&impl_->service.NativeCaptureRequired(in.scene,plan.revisions,in.completed)&&
        (!in.texture_facts_valid||plan.capture_sources_ready);
#endif
    return Result::Success();
}
Result LocalProbeRuntime::PrepareFrameRecordData(const LocalProbeRuntimeFrameInputs &in,
    LocalProbeRuntimeFramePlan &plan)
{
    if(impl_->pending_graph)return Failure("Local-probe runtime already has pending graph work");
#if defined(HS_OFFLINE_PROBE_BAKER)
    std::erase_if(impl_->retired_frames,[&](const auto &frame){return frame->completion.IsSatisfiedBy(in.completed);});
    if(auto decided=PrepareFrameDecisionData(in,plan);!decided)return decided;
    if(!plan.capture)return Result::Success();
    try
    {
        Require(in.config&&in.pipeline_cache,"Native probe capture needs the selected pipeline inputs");
        if(auto materials=PrepareCaptureMaterialOrdinals(in.scene,in.foliage_density);!materials)return materials;
        std::array<bool,rd::kAuthoredMaterialNames.size()> selected{};
        for(const auto ordinal:CachedCaptureMaterialOrdinals(in.scene,in.foliage_density))
        {
            assert(ordinal<selected.size());
            selected[ordinal]=true;
        }
        if(auto pipelines=PrepareCapturePipelines(impl_->capture,*in.config,in.pipeline_cache,true,selected);!pipelines) return pipelines;
        const auto reader=[&](std::string_view id,std::vector<std::byte> &output)->Result{
            try{output=ReadProduct(*in.config,id);return Result::Success();}
            catch(const std::exception &error){return Failure(error.what());}
        };
        if(auto prepared=impl_->service.PrepareCapture(reader,in.completed);!prepared)return prepared;
        plan.shadow=impl_->capture->revision_shadow.enabled;
        return Result::Success();
    }
    catch(const std::exception &error){return Failure(error.what());}
#else
    return PrepareFrameDecisionData(in,plan);
#endif
}
Result LocalProbeRuntime::Register(RenderGraphBuilder &graph,const LocalProbeRuntimeFrameInputs &in,LocalProbeOutputs &out)
{
    LocalProbeRuntimeFramePlan plan;
    if(auto prepared=PrepareFrameRecordData(in,plan);!prepared)return prepared;
    return RegisterImpl(graph,in,plan,out,nullptr,nullptr);
}
Result LocalProbeRuntime::Register(RenderGraphBuilder &graph,const LocalProbeRuntimeFrameInputs &in,
    const LocalProbeRuntimeFramePlan &plan,LocalProbeOutputs &out,LocalProbeRuntimeLayout *layout)
{return RegisterImpl(graph,in,plan,out,layout,nullptr);}
Result LocalProbeRuntime::BindRetained(RenderGraphBuilder &graph,const LocalProbeRuntimeFrameInputs &in,
    const LocalProbeRuntimeFramePlan &plan,const LocalProbeRuntimeLayout &layout,LocalProbeOutputs &out)
{return RegisterImpl(graph,in,plan,out,nullptr,&layout);}
bool LocalProbeRuntime::MatchesLayout(const LocalProbeRuntimeFrameInputs &in,
    const LocalProbeRuntimeFramePlan &plan,const LocalProbeRuntimeLayout &layout) const
{
#if defined(HS_OFFLINE_PROBE_BAKER)
    if(plan.source!=&in||plan.scene!=in.scene||plan.active!=layout.active||
        plan.capture!=layout.capture||plan.shadow!=layout.shadow||
        plan.static_instance_generation!=in.static_instance_generation||
        plan.material_parameter_revision!=in.material_parameter_revision||
        plan.pipeline_revision!=in.pipeline_revision||plan.light_count!=in.lights.size()||
        plan.page_count!=in.pages.size()||plan.scene_read_count!=in.scene_reads.size()||
        plan.capture_identity_valid!=in.texture_facts_valid||
        (plan.capture_identity_valid&&(plan.capture_identity!=CaptureBindingIdentity(in.texture_facts,in.page_revisions,in.pages)||
            plan.capture_sources_ready!=CaptureTexturesReady(in.texture_facts)))||
        plan.max_cube_views!=in.max_cube_views||layout.scene_reads.size()!=in.scene_reads.size()||
        layout.shadow_sampling!=(plan.shadow&&bool(in.declare_shadow_sampling)))return false;
    for(std::size_t i=0;i<in.scene_reads.size();++i)
        if(layout.scene_reads[i].version!=in.scene_reads[i].version||
            layout.scene_reads[i].access!=in.scene_reads[i].access)return false;
    if(!plan.active)return true;
    if(!plan.capture)
    {
        const LocalProbeInputs service_input{in.scene,plan.revisions,{},{},in.completed,0,{},plan.capture_identity,plan.capture_identity_valid,plan.capture_sources_ready};
        LocalProbeFramePlan service_plan;
        return bool(impl_->service.PrepareFrameRecordData(service_input,service_plan))&&
            impl_->service.MatchesLayout(service_input,service_plan,layout.service);
    }
    if(!layout.private_constants||layout.private_constants.kind!=ResourceKind::Buffer||
        (plan.shadow&&(!layout.private_shadow||layout.private_shadow.kind!=ResourceKind::Texture)))return false;
    std::vector<LocalProbeSceneRead> reads(in.scene_reads.begin(),in.scene_reads.end());
    reads.push_back({{layout.private_constants,0},Access::AllShaderRead});
    reads.push_back({in.static_version,Access::VertexShaderRead});
    for(const auto &page:in.pages)reads.push_back({page.vertex_version,Access::VertexShaderRead});
    if(reads.size()!=layout.shadow_reads.size())return false;
    for(std::size_t i=0;i<reads.size();++i)
        if(reads[i].version!=layout.shadow_reads[i].version||
            reads[i].access!=layout.shadow_reads[i].access)return false;
    if(plan.shadow)reads.push_back({layout.shadow_output,Access::ShaderRead});
    const LocalProbeInputs service_input{in.scene,plan.revisions,reads,
        [](RenderPassContext &,const LocalProbeCaptureView &){return Result::Success();},
        in.completed,in.max_cube_views,in.declare_material_sampling,plan.capture_identity,plan.capture_identity_valid,plan.capture_sources_ready};
    LocalProbeFramePlan service_plan;
    return bool(impl_->service.PrepareFrameRecordData(service_input,service_plan))&&
        impl_->service.MatchesLayout(service_input,service_plan,layout.service);
#else
    if(plan.source!=&in || plan.scene!=in.scene || plan.active!=layout.active) return false;
    if(!plan.active) return true;
    const LocalProbeInputs input{in.scene,plan.revisions,{},{},in.completed,0};
    LocalProbeFramePlan service_plan;
    return impl_->service.PrepareFrameRecordData(input,service_plan) &&
        impl_->service.MatchesLayout(input,service_plan,layout.service);
#endif
}
Result LocalProbeRuntime::RegisterImpl(RenderGraphBuilder &graph,const LocalProbeRuntimeFrameInputs &in,
    const LocalProbeRuntimeFramePlan &plan,LocalProbeOutputs &out,LocalProbeRuntimeLayout *layout,
    const LocalProbeRuntimeLayout *retained)
{
#if defined(HS_OFFLINE_PROBE_BAKER)
    out={};if(!plan.source||plan.source!=&in||plan.scene!=in.scene||
        plan.static_instance_generation!=in.static_instance_generation||
        plan.material_parameter_revision!=in.material_parameter_revision||
        plan.pipeline_revision!=in.pipeline_revision||plan.light_count!=in.lights.size()||
        plan.page_count!=in.pages.size()||plan.scene_read_count!=in.scene_reads.size()||
        plan.capture_identity_valid!=in.texture_facts_valid||
        (plan.capture_identity_valid&&(plan.capture_identity!=CaptureBindingIdentity(in.texture_facts,in.page_revisions,in.pages)||
            plan.capture_sources_ready!=CaptureTexturesReady(in.texture_facts)))||
        plan.max_cube_views!=in.max_cube_views)
        return Failure("Local-probe frame record input changed before graph registration");
    if(layout){*layout={};layout->active=plan.active;layout->capture=plan.capture;layout->shadow=plan.shadow;
        layout->shadow_sampling=plan.shadow&&bool(in.declare_shadow_sampling);
        layout->scene_reads.assign(in.scene_reads.begin(),in.scene_reads.end());}
    if(retained&&!MatchesLayout(in,plan,*retained))return Failure("Local-probe retained topology changed");
    if(!impl_->capture)return Result::Success();
    if(impl_->pending_graph)return Failure("Local-probe runtime already has pending graph work");
    if(!in.scene)
    {
        // No substitute scene or revisions. The service uploads/exports its
        // real zero-count metadata and rejects prior scene publication.
        const LocalProbeInputs absent{nullptr,{}, {},{},in.completed,0,{},plan.capture_identity,plan.capture_identity_valid,plan.capture_sources_ready};
        LocalProbeFramePlan service_plan;
        if(auto prepared=impl_->service.PrepareFrameRecordData(absent,service_plan);!prepared)return prepared;
        const auto registered=retained?impl_->service.BindRetained(graph,absent,service_plan,retained->service,out):
            impl_->service.Register(graph,absent,service_plan,out,layout?&layout->service:nullptr);
        if(!registered)return registered;
        impl_->pending_graph=&graph; return Result::Success();
    }
    const auto revisions=plan.revisions;
    if(!plan.capture)
    {
        const LocalProbeInputs publication{in.scene,revisions,{}, {},in.completed,0,{},plan.capture_identity,plan.capture_identity_valid,plan.capture_sources_ready};
        LocalProbeFramePlan service_plan;
        if(auto prepared=impl_->service.PrepareFrameRecordData(publication,service_plan);!prepared)return prepared;
        const auto registered=retained?impl_->service.BindRetained(graph,publication,service_plan,retained->service,out):
            impl_->service.Register(graph,publication,service_plan,out,layout?&layout->service:nullptr);
        if(!registered)return registered;
        impl_->pending_graph=&graph; return Result::Success();
    }
    try
    {
        Require(in.config && in.pipeline_cache,"Native probe capture needs the selected pipeline inputs");
        Require(impl_->capture->native&&impl_->capture->native->sky&&impl_->capture->revision_shadow.enabled==plan.shadow,
            "Local probe prepared capture pipelines or shadow branch changed");
        Require(in.frame_constants && in.static_instances && in.static_version.resource && in.frame_srv_heap && in.sampler_heap &&
            in.frame_srv_base.ptr && in.sampler_table.ptr && !in.scene_reads.empty(),
            "Capture requires actual static/shared bindings and graph reads");
        Require(in.scene->instances.size()<=kLocalProbeSourceCapacity && in.max_cube_views<=6,
            "Capture instance/view budget exceeds packed source-base bounds");
        for(const auto &read:in.scene_reads)
            Require(read.version.resource && read.access!=Access::Undefined && read.access!=Access::Common &&
                read.access!=Access::RenderTarget && read.access!=Access::DepthWrite && read.access!=Access::UnorderedWrite && read.access!=Access::CopyDest,
                "Shared capture binding is not a valid actual graph read");
        auto frame=std::make_shared<LocalProbeCaptureFrame>(); frame->capture=impl_->capture; frame->scene=in.scene;
        frame->pages.assign(in.pages.begin(),in.pages.end()); frame->binding_owners=in.binding_owners;
        frame->static_instances=in.static_instances; frame->static_allocation=in.static_allocation;
        frame->generation=in.static_instance_generation; frame->base=*in.frame_constants;
        // CurrentRevisions qualified these exact scene/page/light inputs. Capture time,
        // benders and origin are fixed by FillFaceConstants for every private bank.
        frame->shadow=impl_->capture->revision_shadow;
        for(UINT cascade=0;cascade<3;++cascade)
        {
            const auto matrix=frame->shadow.enabled?DirectX::XMLoadFloat4x4(&frame->shadow.view.view_projection):DirectX::XMMatrixIdentity();
            DirectX::XMStoreFloat4x4(&frame->base.shadow_view_projection[cascade],DirectX::XMMatrixTranspose(matrix));
            frame->base.shadow_atlas_scale_offset[cascade]={1,1,0,0};
        }
        frame->base.shadow_atlas_texel_size=frame->shadow.enabled?
            DirectX::XMFLOAT4{1.0f/kShadowEdge,1.0f/kShadowEdge,0,in.frame_constants->shadow_atlas_texel_size.w}:DirectX::XMFLOAT4{};
        frame->samplers=in.sampler_heap;
        frame->sampler_table=in.sampler_table; frame->source_descriptors=in.frame_srv_heap;
        frame->draws.reserve(in.scene->instances.size());
        for(std::size_t index=0;index<in.scene->instances.size();++index)
        {
            const auto &instance=in.scene->instances[index];
            if(!RetainDecorativeFoliage(instance,index,in.foliage_density)) continue;
            const auto ordinal=MaterialOrdinal(instance);
            const auto page=PageOrdinal(instance);
            frame->draws.push_back({static_cast<std::uint32_t>(index),ordinal,
                LocalProbeWorldSphere(instance,frame->pages[page].bounds.sphere),page});
        }
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
        const auto device=frame->capture->native->device.Get(); frame->stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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
            Native(frame->capture->native->allocator->CreateResource(&allocation,&depth,D3D12_RESOURCE_STATE_COMMON,&clear,
                &frame->shadow_map.allocation,IID_PPV_ARGS(&frame->shadow_map.resource)),"Allocate private pure-static capture shadow map");
            D3D12_DESCRIPTOR_HEAP_DESC dsv{}; dsv.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dsv.NumDescriptors=1;
            Native(device->CreateDescriptorHeap(&dsv,IID_PPV_ARGS(&frame->shadow_dsv)),"Create private static capture shadow DSV heap");
            D3D12_DEPTH_STENCIL_VIEW_DESC view{}; view.Format=DXGI_FORMAT_D32_FLOAT; view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(frame->shadow_map.resource.Get(),&view,frame->shadow_dsv->GetCPUDescriptorHandleForHeapStart());
            HS_RENDERER_SET_DEBUG_NAME(frame->shadow_map.resource.Get(), L"LocalProbe.PureStaticDirectionalCaptureShadow");
        }
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=UINT64(kConstantBankCount)*kConstantsStride;
        buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=D3D12_HEAP_TYPE_UPLOAD;
        Native(frame->capture->native->allocator->CreateResource(&allocation,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,&frame->constants.allocation,
            IID_PPV_ARGS(&frame->constants.resource)),"Allocate immutable capture face constants");
        D3D12_RANGE no_read{}; Native(frame->constants.resource->Map(0,&no_read,reinterpret_cast<void **>(&frame->mapped)),"Map capture face constants");
        for(UINT bank=0;bank<kConstantBankCount;++bank)
        {
            auto constants=frame->base;
            if(bank==kFaceCount && frame->shadow.enabled) FillFaceConstants(constants,frame->shadow.view,frame->generation);
            std::memcpy(frame->mapped+std::size_t(bank)*kConstantsStride,&constants,sizeof(constants));
            auto destination=frame->descriptors->GetCPUDescriptorHandleForHeapStart(); destination.ptr+=SIZE_T(bank)*kCaptureDescriptorCount*frame->stride;
            CopyCaptureDescriptors(device,destination,in.frame_srv_base);
            auto shadow_descriptor=destination; shadow_descriptor.ptr+=3u*frame->stride; // Root5 begins at t2; ShadowMap is t5.
            D3D12_SHADER_RESOURCE_VIEW_DESC shadow_srv{}; shadow_srv.Format=DXGI_FORMAT_R32_FLOAT;
            shadow_srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; shadow_srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            shadow_srv.Texture2D.MipLevels=1;
            // The opacity-only depth shader never reads ShadowMap. Its bank uses
            // a typed null to avoid an SRV/DSV alias while the map is written.
            device->CreateShaderResourceView(bank<kFaceCount?frame->shadow_map.resource.Get():nullptr,&shadow_srv,shadow_descriptor);
        }
        if(auto certified=frame->constants.EnsureNativeContract(device);!certified)return certified;
        if(frame->shadow.enabled)
            if(auto certified=frame->shadow_map.EnsureNativeContract(device);!certified)return certified;
        if(retained)
        {
            if(!retained->private_constants||retained->private_constants.kind!=ResourceKind::Buffer||
                retained->private_constants.value>graph.ResourceCount()||
                (plan.shadow&&(!retained->private_shadow||
                    retained->private_shadow.kind!=ResourceKind::Texture||
                    retained->private_shadow.value>graph.ResourceCount()||
                    !graph.BindPass(retained->shadow_pass_index,177))))
                return Failure("Local probe retained private import or shadow pass changed");
        }
        frame->constants_handle=retained?retained->private_constants:
            graph.ImportBuffer({frame->constants.resource.Get(),{},QueueHint::Direct,{},frame->constants.NativeContract()},
                Access::AllShaderRead,"LocalProbe.PrivateFaceConstants");
        if(retained&&!graph.BindBuffer(frame->constants_handle,
                {frame->constants.resource.Get(),{},QueueHint::Direct,{},frame->constants.NativeContract()},
                Access::AllShaderRead))return Failure("Local probe retained face constants binding changed");
        if(layout)layout->private_constants=frame->constants_handle;
        std::vector<LocalProbeSceneRead> reads(in.scene_reads.begin(),in.scene_reads.end());
        reads.push_back({graph.Version(frame->constants_handle),Access::AllShaderRead}); reads.push_back({in.static_version,Access::VertexShaderRead});
        for(const auto &page:frame->pages) { reads.push_back({page.vertex_version,Access::VertexShaderRead}); }
        if(layout)layout->shadow_reads=reads;
        if(frame->shadow.enabled)
        {
            frame->shadow_handle=retained?retained->private_shadow:
                graph.ImportTexture({frame->shadow_map.resource.Get(),{},QueueHint::Direct,{},frame->shadow_map.NativeContract()},
                    Access::Common,"LocalProbe.PrivateStaticShadow");
            if(retained&&!graph.BindTexture(frame->shadow_handle,
                    {frame->shadow_map.resource.Get(),{},QueueHint::Direct,{},frame->shadow_map.NativeContract()},
                    Access::Common))return Failure("Local probe retained private shadow binding changed");
            if(layout){layout->private_shadow=frame->shadow_handle;layout->shadow_pass_index=graph.PassCount();}
            auto shadow_pass=retained?*graph.BindPass(retained->shadow_pass_index,177):
                graph.AddPass(177,"LocalProbeStaticDirectionalShadow",QueueHint::Direct);
            if(!retained)
            {
                for(const auto &read:reads)shadow_pass.Read(read.version,read.access);
                if(in.declare_shadow_sampling)in.declare_shadow_sampling(shadow_pass);
            }
            const auto shadow_version=retained?retained->shadow_output:
                shadow_pass.Write(graph.Version(frame->shadow_handle),Access::DepthWrite);
            if(layout)layout->shadow_output=shadow_version;
            shadow_pass.SetExecuteChecked([frame](RenderPassContext &context) { return frame->DrawShadow(context); });
            reads.push_back({shadow_version,Access::ShaderRead});
            if(!retained)graph.SetFinalAccess(shadow_version,Access::ShaderRead);
        }
        const LocalProbeInputs input{in.scene,revisions,reads,[frame](RenderPassContext &context,const LocalProbeCaptureView &view) {
            return frame->Draw(context,view);
        },in.completed,in.max_cube_views,in.declare_material_sampling,plan.capture_identity,plan.capture_identity_valid,plan.capture_sources_ready};
        LocalProbeFramePlan service_plan;
        if(auto prepared=impl_->service.PrepareFrameRecordData(input,service_plan);!prepared)return prepared;
        const auto registered=retained?impl_->service.BindRetained(graph,input,service_plan,retained->service,out):
            impl_->service.Register(graph,input,service_plan,out,layout?&layout->service:nullptr);
        if(!registered)return registered;
        impl_->pending_graph=&graph; impl_->pending_frame=std::move(frame); return Result::Success();
    }
    catch(const std::exception &error) { return Failure(error.what()); }
#else
    out={};
    if(plan.source!=&in || plan.scene!=in.scene || plan.static_instance_generation!=in.static_instance_generation ||
        plan.material_parameter_revision!=in.material_parameter_revision || plan.pipeline_revision!=in.pipeline_revision)
        return Failure("Baked lighting publication inputs changed before recording.");
    if(layout){*layout={};layout->active=plan.active;}
    if(!plan.active)return Result::Success();
    if(impl_->pending_graph)return Failure("Baked lighting already has pending graph work.");
    const LocalProbeInputs input{in.scene,plan.revisions,{},{},in.completed,0};
    LocalProbeFramePlan service_plan;
    if(auto ready=impl_->service.PrepareFrameRecordData(input,service_plan);!ready)return ready;
    const auto registered=retained?impl_->service.BindRetained(graph,input,service_plan,retained->service,out):
        impl_->service.Register(graph,input,service_plan,out,layout?&layout->service:nullptr);
    if(registered)impl_->pending_graph=&graph;
    return registered;
#endif
}
Result LocalProbeRuntime::CompleteSubmission(const RenderGraphBuilder &graph,const CompletionSet &completion,bool success,
    const std::shared_ptr<const StaticRenderScene> &scene,rd::SceneProbeRevisions revisions)
{
    if(!impl_->capture) return Result::Success();
    if(impl_->pending_graph!=&graph) return Failure("Local-probe completion does not match its registered graph");
#if defined(HS_OFFLINE_PROBE_BAKER)
    auto frame=std::move(impl_->pending_frame); impl_->pending_graph=nullptr;
    if(frame)
    {
        frame->completion=completion; frame->completion.Merge(graph.ResourceCompletion(frame->constants_handle));
        if(frame->shadow_handle) frame->completion.Merge(graph.ResourceCompletion(frame->shadow_handle));
        impl_->retired_frames.push_back(std::move(frame));
    }
#else
    impl_->pending_graph=nullptr;
#endif
    return impl_->service.CompleteSubmission(graph,completion,success,scene,revisions);
}
Result LocalProbeRuntime::ResetAfterCompletion(const CompletionSet &completed)
{
    if(impl_->pending_graph) return Failure("Cannot reset local-probe runtime with pending graph work");
#if defined(HS_OFFLINE_PROBE_BAKER)
    for(const auto &frame:impl_->retired_frames) if(!frame->completion.IsSatisfiedBy(completed)) return Failure("Cannot reset in-flight capture bindings");
#endif
    if(auto reset=impl_->service.ResetAfterCompletion(completed);!reset) return reset;
    impl_->capture.reset();
#if defined(HS_OFFLINE_PROBE_BAKER)
    impl_->retired_frames.clear();
#endif
    return Result::Success();
}
#if defined(HS_OFFLINE_PROBE_BAKER)
Result LocalProbeRuntime::BeginOfflineIteration(const CompletionSet &completed,bool reflections_only)
{
    if(impl_->pending_graph) return Failure("Cannot restart probe capture with pending graph work");
    return impl_->service.BeginOfflineIteration(completed,reflections_only);
}
Result LocalProbeRuntime::ExportBakedPackage(RenderGraphQueueExecutor &executor,TransientResourcePool &pool,
    std::uint32_t frame_slot,BarrierMode barriers,std::vector<std::byte> &package_bytes)
{
    if(impl_->pending_graph) return Failure("Cannot export a scene-probe package with pending graph work");
    const auto capture=impl_->capture;
    if(!capture || !capture->revision_scene || !capture->material_revision_facts_valid)
        return Failure("Cannot export a scene-probe package without qualified capture revisions");
    auto expected=capture->derived_revisions;
    expected.environment=capture->baseline.environment;
    auto compatibility=capture->compatibility_revisions;
    compatibility.environment=expected.environment;
    compatibility.light=expected.light;
    return impl_->service.ExportBakedPackage(executor,pool,frame_slot,barriers,package_bytes,
        &expected,&compatibility);
}
#endif
std::span<const rd::SceneProbeRecord> LocalProbeRuntime::Probes() const noexcept
{ return impl_->capture?std::span<const rd::SceneProbeRecord>(impl_->capture->probes):std::span<const rd::SceneProbeRecord>{}; }
rd::SceneProbeRevisions LocalProbeRuntime::BaselineRevisions() const noexcept
{ return impl_->capture?impl_->capture->baseline:rd::SceneProbeRevisions{}; }
#if defined(HS_DEVELOPMENT_TOOLS)
LocalProbeRevisionDiagnostics LocalProbeRuntime::RevisionDiagnostics() const
{
    LocalProbeRevisionDiagnostics result;
    if (const auto capture=impl_->capture)
    {
        result.baseline=capture->baseline;
        result.compatibility=capture->compatibility_revisions;
        result.compatibility.environment=capture->baseline.environment;
        result.compatibility.light=capture->baseline.light;
        result.material_parameter_revision=capture->revision_material_parameter;
    }
    return result;
}
#endif
#if defined(HS_OFFLINE_PROBE_BAKER)
LocalProbeBakeStatus LocalProbeRuntime::BakeStatus(const CompletionSet &completed) const noexcept
{ return impl_->service.BakeStatus(completed); }
#endif
std::uint64_t LocalProbeRuntime::AllocatedBytes() const noexcept
{
    auto bytes=impl_->service.AllocatedBytes();
#if defined(HS_OFFLINE_PROBE_BAKER)
    const auto native_bytes=[](const std::shared_ptr<LocalProbeCaptureFrame> &frame) {
        return (frame->constants.allocation?frame->constants.allocation->GetSize():0) +
            (frame->shadow_map.allocation?frame->shadow_map.allocation->GetSize():0);
    };
    if(impl_->pending_frame) bytes+=native_bytes(impl_->pending_frame);
    for(const auto &frame:impl_->retired_frames) bytes+=native_bytes(frame);
#endif
    return bytes;
}

#if defined(HS_OFFLINE_PROBE_BAKER)
Result D3D12Renderer::Impl::PrepareWorldTransportInstances(
    const std::shared_ptr<const StaticRenderScene> &scene, AllocationResource &instances,
    std::uint32_t &generation, bool terrain_sections)
{
    generation = 0;
    if (!scene || scene->instances.empty() || scene->instances.size() > kLocalProbeSourceCapacity)
        return Failure("World transport requires a nonempty scene within the native source capacity");
    GpuSceneTableLayout layout{};
    if (!BuildGpuSceneTableLayout(scene->instances.size(), scene->instances.size(), layout))
        return Failure("World transport static scene table overflows");
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (auto created = CreateAllocation(instances, allocation, BufferDescription(layout.total_bytes),
            D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
    std::byte *mapped{}; D3D12_RANGE no_read{};
    const auto result = instances.resource->Map(0, &no_read, reinterpret_cast<void **>(&mapped));
    if (FAILED(result)) return HResultFailure("Map world transport static scene", result);
    std::memset(mapped, 0, static_cast<std::size_t>(layout.total_bytes));
    std::memcpy(mapped, &layout.header, sizeof(layout.header));
    constexpr std::uint32_t instance_generation = 1;
    for (std::size_t index = 0; index < scene->instances.size(); ++index)
    {
        const auto &source = scene->instances[index];
        const bool environment = rd::IsEnvironmentMesh(source.mesh);
        const auto mesh = static_cast<UINT>(source.mesh) | (environment ? kEnvironmentMeshFlag : 0u) |
            (source.mesh == RenderMesh::GroundDetail ? source.environment_variant << 8u : 0u);
        const auto seed = source.mesh == RenderMesh::Ground || source.mesh == RenderMesh::DirtPatch ||
            source.mesh == RenderMesh::MeadowSurface ? std::bit_cast<float>(terrain_sections
                ? static_cast<UINT>(index) : source.environment_seed) : 0.0f;
        const GpuInstance instance{{source.position.x, source.position.y + (environment ? 0 : source.scale.y * .5f), source.position.z, 0},
            {source.scale.x, source.scale.y, source.scale.z, 1}, source.color_rgba, mesh, source.yaw, seed};
        const auto slot = SerializeGpuSceneSlot(static_cast<UINT>(index), instance,
            {source.asset_id.value, source.material_id.value, UINT32_MAX, 0, instance_generation, 0});
        const auto write = [&](UINT offset, const auto &value) {
            std::memcpy(mapped + offset + index * sizeof(value), &value, sizeof(value));
        };
        write(layout.header.instance_offset, slot.instance); write(layout.header.transform_offset, slot.transform);
        write(layout.header.mesh_offset, slot.mesh); write(layout.header.material_offset, slot.material);
        write(layout.header.bounds_offset, slot.bounds); write(layout.header.skin_offset, slot.skin);
    }
    instances.resource->Unmap(0, nullptr);
    instances.graph_access = Access::AllShaderRead;
    instances.graph_queue = QueueHint::Direct;
    generation = instance_generation;
    return Result::Success();
}

// Retained before submission so a failed fence/event cannot release GPU readers.
struct StaticShadowBakeFrame
{
    std::shared_ptr<LocalProbeCaptureFrame> capture;
    std::vector<AllocationResource> uploads, textures;
    AllocationResource instances, readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
};

Result D3D12Renderer::BakeStaticShadowPages(const std::shared_ptr<const StaticRenderScene> &scene,
    const LightView &light,
    const std::function<Result(const rd::StaticShadowArchiveDescription &)> &begin,
    const std::function<Result(std::uint32_t,std::int32_t,std::int32_t,std::span<const float>)> &page_sink)
{
    if (!impl_->initialized)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Static shadow bake requires an initialized healthy renderer.");
    if (!scene || scene->instances.empty() || !begin || !page_sink)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
            "Static shadow bake requires a nonempty scene and both archive callbacks.");
    return BakeStaticShadowPagesPrepared(scene, light, begin, page_sink);
}

Result D3D12Renderer::BakeStaticShadowPagesPrepared(
    const std::shared_ptr<const StaticRenderScene> &scene, const LightView &light,
    const std::function<Result(const rd::StaticShadowArchiveDescription &)> &begin,
    const std::function<Result(std::uint32_t,std::int32_t,std::int32_t,std::span<const float>)> &page_sink)
{
    std::uint32_t edge=32*rd::kStaticShadowPageInner+2*rd::kStaticShadowPageGuard;
    try
    {
        Require(!impl_->frame_submission_faulted, "Static shadow bake renderer submission state is faulted");
        Require(scene->instances.size()<=kLocalProbeSourceCapacity,"Static shadow source count exceeds native capture ABI");
        if(auto drained=impl_->WaitForGpu();!drained) return drained;
        impl_->pending_static_shadow_bake.reset();
        auto owner=std::make_shared<StaticShadowBakeFrame>();
        auto frame=std::make_shared<LocalProbeCaptureFrame>(); owner->capture=frame;
        frame->capture=std::make_shared<LocalProbeCaptureState>();
        frame->capture->native=std::make_unique<LocalProbeNativeCapture>();
        frame->capture->native->device=impl_->device; frame->capture->native->allocator=impl_->allocator;
        frame->capture->native->root=impl_->root_signature; frame->scene=scene; frame->generation=1;
        frame->stride=impl_->srv_stride; frame->samplers=impl_->sampler_heap;
        frame->sampler_table=impl_->sampler_heap->GetGPUDescriptorHandleForHeapStart();
        auto *device=impl_->device.Get();
        Native(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&owner->allocator)),"Create bake allocator");
        Native(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,owner->allocator.Get(),nullptr,IID_PPV_ARGS(&owner->list)),"Create bake commands");
        auto *list=owner->list.Get();
        const auto allocate=[&](AllocationResource &resource,D3D12_HEAP_TYPE heap,const D3D12_RESOURCE_DESC &description,
            D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE *clear=nullptr) {
            D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType=heap;
            Native(impl_->allocator->CreateResource(&allocation,&description,state,clear,&resource.allocation,
                IID_PPV_ARGS(&resource.resource)),"Allocate static shadow bake resource");
        };
        const auto transition=[&](ID3D12Resource *resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&barrier);
        };
        std::array<bool,rd::kAuthoredMaterialNames.size()> materials{};
        decltype(materials) opacity_materials{};
        bool procedural_two_sided=false;
        for(const auto &source:scene->instances)
        {
            if(!rd::IsStaticShadowCaster(source)) continue;
            const auto material=MaterialOrdinal(source);
            auto canonical=source; canonical.asset_id={}; canonical.material_id={};
            PopulateRenderInstanceIdentity(canonical);
            Require((!source.asset_id.value || source.asset_id.value==canonical.asset_id.value) &&
                (!source.material_id.value || source.material_id.value==canonical.material_id.value),
                "Static shadow bake does not support noncanonical geometry or material overrides");
            materials[material]=true;
            // Match CaptureStaticShadowPS/HsSceneCovered's authored-opacity branch.
            opacity_materials[material] = opacity_materials[material] ||
                source.mesh==RenderMesh::TreeCanopy || source.mesh==RenderMesh::Grass;
            if(!rd::IsEnvironmentMesh(source.mesh)) procedural_two_sided=true;
        }
        Require(std::any_of(materials.begin(),materials.end(),[](bool used) { return used; }),
            "Static shadow bake scene contains no eligible static casters");
        if(auto pipelines=PrepareCapturePipelines(frame->capture,impl_->config,&impl_->pipeline_cache,
            false,materials,0,procedural_two_sided);!pipelines) return pipelines;
        frame->pages.resize(rd::kEnvironmentMaterialNames.size()+1);
        for(std::size_t ordinal=0;ordinal<frame->pages.size();++ordinal)
        {
            // Match the pinned terminal LOD2 page used by virtual shadow world queries.
            const GeometryPageGpu *page=ordinal==rd::kEnvironmentMaterialNames.size()?&impl_->procedural_geometry:
                &impl_->environment_meshes[ordinal*3+2];
            auto &capture=frame->pages[ordinal]; capture.bounds=page->metadata.bounds;
            capture.vertices=page->storage.resource; capture.indices=page->storage.resource;
            capture.vertex_allocation=page->storage.allocation; capture.index_allocation=page->storage.allocation;
            capture.vertex_view=page->vertex_view; capture.index_view=page->index_view;
            capture.index_count=page->index_count; capture.generation=page->generation; capture.page_slot=page->page_slot;
            capture.complete=page->storage.resource && page->index_count;
        }
        StaticShadowBakeBounds bounds;
        frame->shadow=MakeStaticShadowView(scene,
            std::span<const LocalProbeCapturePage>(frame->pages),std::span(&light,1),edge,&bounds);
        Require(frame->shadow.enabled,"Static shadow bake requires a valid shadow-requested directional light");
        rd::StaticShadowArchiveDescription archive;
        {
            const auto &basis=frame->shadow.view.world_to_view;
            archive.light_right={basis._11,basis._21,basis._31};
            archive.light_up={basis._12,basis._22,basis._32};
            archive.light_forward={basis._13,basis._23,basis._33};
            archive.depth_minimum=bounds.minimum.z; archive.depth_maximum=bounds.maximum.z;
            archive.layout_digest=rd::StaticShadowLayoutDigest(*scene);
            // Runtime coverage_extents are cascade half-extents; registration divides
            // their full width by each level's page count.
            constexpr std::array<float,3> page_sizes{0.5625f,6.0f,20.0f};
            std::uint64_t total_pages{};
            for(std::size_t level=0;level<archive.levels.size();++level)
            {
                auto &description=archive.levels[level]; description.page_world_size=page_sizes[level];
                const double side=description.page_world_size;
                const double guard=side*rd::kStaticShadowPageGuard/rd::kStaticShadowPageInner;
                const auto range=[&](float minimum,float maximum,std::int32_t &first,std::uint32_t &count) {
                    const double lo=std::floor((double(minimum)-guard)/side);
                    const double hi=std::floor((double(maximum)+guard)/side);
                    Require(std::isfinite(lo) && std::isfinite(hi) &&
                        lo>=std::numeric_limits<std::int32_t>::min() && hi<=std::numeric_limits<std::int32_t>::max() &&
                        hi>=lo && hi-lo+1<=std::numeric_limits<std::uint32_t>::max(),"Static shadow archive page bounds overflow");
                    first=static_cast<std::int32_t>(lo); count=static_cast<std::uint32_t>(hi-lo+1);
                };
                range(bounds.minimum.x,bounds.maximum.x,description.minimum_x,description.width);
                range(bounds.minimum.y,bounds.maximum.y,description.minimum_y,description.height);
                const auto count=std::uint64_t(description.width)*description.height;
                Require(count<=std::numeric_limits<std::uint64_t>::max()-total_pages,"Static shadow archive page count overflows");
                total_pages+=count;
            }
            if(auto accepted=begin(archive);!accepted) return accepted;
            const auto &coarse=archive.levels.back();
            edge=std::min(32u,std::max(coarse.width,coarse.height))*rd::kStaticShadowPageInner+2*rd::kStaticShadowPageGuard;
        }
        for(std::size_t index=0;index<scene->instances.size();++index)
        {
            const auto &source=scene->instances[index]; if(!rd::IsStaticShadowCaster(source)) continue;
            const auto page=PageOrdinal(source); Require(frame->pages[page].complete,"Bake caster lacks complete source geometry");
            frame->draws.push_back({static_cast<UINT>(index),MaterialOrdinal(source),{},page});
        }
        if (auto prepared=impl_->PrepareWorldTransportInstances(scene,owner->instances,frame->generation,false);!prepared)
            return prepared;
        D3D12_RANGE no_read{};
        frame->static_instances=owner->instances.resource;
        frame->static_allocation=owner->instances.allocation;
        FillFaceConstants(frame->base,frame->shadow.view,1);
        for(std::size_t group=0;group<frame->base.material_parameter_bases.size();++group)
        {
            std::array<UINT,4> bases{},masks{};
            for(std::size_t lane=0;lane<4;++lane) if(const auto ordinal=group*4+lane;ordinal<materials.size())
            { bases[lane]=impl_->material_parameter_bases[ordinal]; masks[lane]=impl_->material_surface_layouts[ordinal].varying_mask; }
            frame->base.material_parameter_bases[group]={bases[0],bases[1],bases[2],bases[3]};
            frame->base.material_varying_masks[group]={masks[0],masks[1],masks[2],masks[3]};
        }
        allocate(frame->constants,D3D12_HEAP_TYPE_UPLOAD,BufferDescription(kConstantsStride),D3D12_RESOURCE_STATE_GENERIC_READ);
        Native(frame->constants.resource->Map(0,&no_read,reinterpret_cast<void **>(&frame->mapped)),"Map bake constants");
        std::memset(frame->mapped,0,kConstantsStride); std::memcpy(frame->mapped,&frame->base,sizeof(frame->base));
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; heap.NumDescriptors=kCaptureDescriptorCount;
        Native(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&frame->descriptors)),"Create bake descriptors");
        const auto descriptor=[&](UINT offset) { auto h=frame->descriptors->GetCPUDescriptorHandleForHeapStart(); h.ptr+=SIZE_T(offset)*frame->stride; return h; };
        CopyCaptureDescriptors(device,descriptor(0),impl_->srv_cpu_heap->GetCPUDescriptorHandleForHeapStart());
        // Authored opacity samples read this metadata even in a depth-only shader.
        // These private textures contain the entire mip chain, so every binding
        // has physical base=resident MinMip=0 and no runtime feedback target.
        // Structured null reads return uint4(0), independently of the last live frame.
        D3D12_SHADER_RESOURCE_VIEW_DESC mip_metadata{};
        mip_metadata.Format=DXGI_FORMAT_UNKNOWN; mip_metadata.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        mip_metadata.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        mip_metadata.Buffer.NumElements=kTextureFeedbackBindingCount;
        mip_metadata.Buffer.StructureByteStride=sizeof(UINT)*4;
        device->CreateShaderResourceView(nullptr,&mip_metadata,descriptor(kTextureFeedbackDescriptorOffset));
        std::map<std::string,ID3D12Resource *> uploaded_textures;
        for(std::size_t material=0;material<materials.size();++material) if(materials[material])
            for(UINT slot=0;slot<rd::kMaterialTextureSlots;++slot)
            {
                const auto &id=impl_->environment_material_bindings[material].product_ids[slot];
                if(id.empty() || !opacity_materials[material])
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC unused{};
                    unused.Format=DXGI_FORMAT_BC7_UNORM_SRGB;
                    if(!id.empty())
                    {
                        const auto texture=FindRendererTexture(impl_->config,MakeAssetId(id));
                        Require(texture && texture.value<impl_->config.packed_textures.size(),
                            "Static shadow material texture lacks its admitted descriptor");
                        unused.Format=static_cast<DXGI_FORMAT>(impl_->config.packed_textures[texture.value].description.format);
                    }
                    unused.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    unused.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    unused.Texture2DArray.MipLevels=unused.Texture2DArray.ArraySize=1;
                    device->CreateShaderResourceView(nullptr,&unused,descriptor(kMaterialTextureDescriptorOffset+
                        static_cast<UINT>(material)*rd::kMaterialTextureSlots+slot));
                    continue;
                }
                auto found=uploaded_textures.find(id);
                ID3D12Resource *texture{};
                if(found!=uploaded_textures.end()) texture=found->second;
                else
                {
                    const auto bytes=ReadProduct(impl_->config,id); const auto package=ReadRendererTexturePackage(impl_->config,id,bytes);
                    const auto &source=package.description;
                    D3D12_RESOURCE_DESC description{}; description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                    description.Width=source.width; description.Height=source.height; description.DepthOrArraySize=static_cast<UINT16>(source.array_size);
                    description.MipLevels=static_cast<UINT16>(source.mip_count); description.Format=static_cast<DXGI_FORMAT>(source.format); description.SampleDesc.Count=1;
                    owner->textures.emplace_back(); allocate(owner->textures.back(),D3D12_HEAP_TYPE_DEFAULT,description,D3D12_RESOURCE_STATE_COPY_DEST);
                    texture=owner->textures.back().resource.Get();
                    const auto count=source.array_size*source.mip_count;
                    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count); std::vector<UINT> rows(count); std::vector<UINT64> row_sizes(count);
                    UINT64 size{}; device->GetCopyableFootprints(&description,0,count,0,footprints.data(),rows.data(),row_sizes.data(),&size);
                    owner->uploads.emplace_back(); auto &upload=owner->uploads.back();
                    allocate(upload,D3D12_HEAP_TYPE_UPLOAD,BufferDescription(size),D3D12_RESOURCE_STATE_GENERIC_READ);
                    std::byte *pixels{}; Native(upload.resource->Map(0,&no_read,reinterpret_cast<void **>(&pixels)),"Map bake full texture");
                    for(UINT subresource=0;subresource<count;++subresource)
                    {
                        const auto &input=package.subresources[subresource];
                        Require(input.row_pitch==row_sizes[subresource] && UINT64(rows[subresource])*input.row_pitch==input.bytes.size(),"Bake texture footprint mismatch");
                        for(UINT row=0;row<rows[subresource];++row)
                            std::memcpy(pixels+footprints[subresource].Offset+SIZE_T(row)*footprints[subresource].Footprint.RowPitch,
                                input.bytes.data()+SIZE_T(row)*input.row_pitch,static_cast<std::size_t>(row_sizes[subresource]));
                        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=texture;
                        destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; destination.SubresourceIndex=subresource;
                        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=upload.resource.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        from.PlacedFootprint=footprints[subresource]; list->CopyTextureRegion(&destination,0,0,0,&from,nullptr);
                    }
                    upload.resource->Unmap(0,nullptr);
                    transition(texture,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    uploaded_textures.emplace(id,texture);
                }
                const auto description=texture->GetDesc(); D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format=description.Format; srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2DArray.MipLevels=description.MipLevels; srv.Texture2DArray.ArraySize=description.DepthOrArraySize;
                device->CreateShaderResourceView(texture,&srv,descriptor(kMaterialTextureDescriptorOffset+static_cast<UINT>(material)*rd::kMaterialTextureSlots+slot));
                for(UINT legacy=0;legacy<impl_->environment_texture_ids.size();++legacy)
                    if(impl_->environment_texture_ids[legacy]==id)
                        device->CreateShaderResourceView(texture,&srv,descriptor(kPostTextureDescriptorCount+
                            kCharacterDescriptorCount+kMonsterPbrDescriptorCount+legacy));
            }
        D3D12_RESOURCE_DESC depth{}; depth.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; depth.Width=depth.Height=edge;
        depth.DepthOrArraySize=depth.MipLevels=1; depth.Format=DXGI_FORMAT_R32_TYPELESS; depth.SampleDesc.Count=1;
        depth.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL; D3D12_CLEAR_VALUE clear{}; clear.Format=DXGI_FORMAT_D32_FLOAT;
        allocate(frame->shadow_map,D3D12_HEAP_TYPE_DEFAULT,depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear);
        D3D12_DESCRIPTOR_HEAP_DESC dsv{}; dsv.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dsv.NumDescriptors=1;
        Native(device->CreateDescriptorHeap(&dsv,IID_PPV_ARGS(&frame->shadow_dsv)),"Create bake DSV");
        D3D12_DEPTH_STENCIL_VIEW_DESC view{}; view.Format=DXGI_FORMAT_D32_FLOAT; view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
        const auto target=frame->shadow_dsv->GetCPUDescriptorHandleForHeapStart();
        device->CreateDepthStencilView(frame->shadow_map.resource.Get(),&view,target);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 readback_bytes{};
        device->GetCopyableFootprints(&depth,0,1,0,&footprint,nullptr,nullptr,&readback_bytes);
        allocate(owner->readback,D3D12_HEAP_TYPE_READBACK,BufferDescription(readback_bytes),D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource=owner->readback.resource.Get();
        destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint=footprint;
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource=frame->shadow_map.resource.Get(); from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        bool first_chunk=true;
        const auto render_chunk=[&](const LocalProbeCaptureView &projection,auto &&consume) -> Result {
            if(!first_chunk)
            {
                Native(owner->allocator->Reset(),"Reset static shadow chunk allocator");
                Native(list->Reset(owner->allocator.Get(),nullptr),"Reset static shadow chunk commands");
                transition(frame->shadow_map.resource.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_DEPTH_WRITE);
            }
            FillFaceConstants(frame->base,projection,1);
            std::memcpy(frame->mapped,&frame->base,sizeof(frame->base));
            frame->BindPrivate(list,0);
            // The source-base shader bypasses dynamic instances/skin/geometry records.
            for(UINT root:{1u,2u,11u,12u,14u,16u,17u,18u,20u,25u,27u,28u,31u,35u})
                list->SetGraphicsRootShaderResourceView(root,0);
            list->ClearDepthStencilView(target,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
            list->OMSetRenderTargets(0,nullptr,FALSE,&target);
            const D3D12_VIEWPORT viewport{0,0,float(projection.edge),float(projection.edge),0,1};
            const D3D12_RECT scissor{0,0,LONG(projection.edge),LONG(projection.edge)};
            list->RSSetViewports(1,&viewport); list->RSSetScissorRects(1,&scissor); frame->DrawInstances(list,true);
            transition(frame->shadow_map.resource.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->CopyTextureRegion(&destination,0,0,0,&from,nullptr); Native(list->Close(),"Close static shadow bake");
            impl_->pending_static_shadow_bake=owner;
            if(auto submitted=impl_->SubmitResidentUtilityUpload(list,owner->allocator.Get(),owner->uploads);!submitted) return submitted;
            impl_->pending_static_shadow_bake.reset(); first_chunk=false;
            D3D12_RANGE read{0,static_cast<SIZE_T>(readback_bytes)}; std::byte *pixels{};
            Native(owner->readback.resource->Map(0,&read,reinterpret_cast<void **>(&pixels)),"Read static shadow depth");
            try
            {
                const auto consumed=consume(pixels+footprint.Offset,footprint.Footprint.RowPitch);
                owner->readback.resource->Unmap(0,&no_read);
                return consumed;
            }
            catch(...) { owner->readback.resource->Unmap(0,&no_read); throw; }
        };
        using namespace DirectX;
        const auto light_view=XMLoadFloat4x4(&frame->shadow.view.world_to_view);
        const auto inverse_view=XMMatrixInverse(nullptr,light_view);
        std::array<float,rd::kStaticShadowPageTexels> tile;
        // Coarse coverage is offline; finer levels retain the existing runtime cache.
        constexpr std::uint32_t level=2;
        {
            const auto &description=archive.levels[level];
            for(std::uint64_t y=0;y<description.height;y+=32)
                for(std::uint64_t x=0;x<description.width;x+=32)
                {
                    const auto columns=static_cast<UINT>(std::min<std::uint64_t>(32,description.width-x));
                    const auto rows=static_cast<UINT>(std::min<std::uint64_t>(32,description.height-y));
                    const auto span=std::max(columns,rows);
                    const auto page_x=std::int64_t(description.minimum_x)+static_cast<std::int64_t>(x);
                    const auto page_y=std::int64_t(description.minimum_y)+static_cast<std::int64_t>(y);
                    const double side=description.page_world_size;
                    const double guard=side*rd::kStaticShadowPageGuard/rd::kStaticShadowPageInner;
                    const float left=static_cast<float>(page_x*side-guard),right=static_cast<float>((page_x+span)*side+guard);
                    const float bottom=static_cast<float>(page_y*side-guard),top=static_cast<float>((page_y+span)*side+guard);
                    Require(std::isfinite(left) && std::isfinite(right) && std::isfinite(bottom) && std::isfinite(top) &&
                        right>left && top>bottom,"Static shadow chunk projection is not representable");
                    auto projection=frame->shadow.view;
                    projection.edge=span*rd::kStaticShadowPageInner+2*rd::kStaticShadowPageGuard;
                    const auto matrix=XMMatrixOrthographicOffCenterLH(left,right,bottom,top,archive.depth_maximum,archive.depth_minimum);
                    XMStoreFloat4x4(&projection.projection,matrix);
                    XMStoreFloat4x4(&projection.view_projection,light_view*matrix);
                    XMFLOAT3 position;
                    XMStoreFloat3(&position,XMVector3TransformCoord(XMVectorSet(
                        float((double(left)+right)*.5),float((double(bottom)+top)*.5),archive.depth_minimum,1),inverse_view));
                    projection.position={position.x,position.y,position.z};
                    if(auto rendered=render_chunk(projection,[&](const std::byte *pixels,UINT pitch) {
                        for(UINT local_y=0;local_y<rows;++local_y)
                            for(UINT local_x=0;local_x<columns;++local_x)
                            {
                                const auto source_y=(span-1-local_y)*rd::kStaticShadowPageInner;
                                const auto source_x=local_x*rd::kStaticShadowPageInner;
                                for(UINT row=0;row<rd::kStaticShadowPageTile;++row)
                                    std::memcpy(tile.data()+std::size_t(row)*rd::kStaticShadowPageTile,
                                        pixels+std::size_t(source_y+row)*pitch+std::size_t(source_x)*sizeof(float),
                                        rd::kStaticShadowPageTile*sizeof(float));
                                if(auto accepted=page_sink(level,static_cast<std::int32_t>(page_x+local_x),
                                    static_cast<std::int32_t>(page_y+local_y),tile);!accepted) return accepted;
                            }
                        return Result::Success();
                    });!rendered) return rendered;
                }
        }
        return Result::Success();
    }
    catch(const std::exception &error) { return Failure(error.what()); }
}
Result D3D12Renderer::BakeSceneProbePackage(const RendererConfig &configuration,
    const std::shared_ptr<const StaticRenderScene> &scene, const LightView &sun,
    std::vector<std::byte> &package_bytes, std::uint32_t maximum_cube_views_per_graph, SceneProbeBakePolicy policy)
{
    if (maximum_cube_views_per_graph < 1 || maximum_cube_views_per_graph > 6)
        return Failure("Scene-probe cube-view budget must be between one and six.");
    if (policy.maximum_iterations < 1 || policy.maximum_iterations > 64 ||
        !std::isfinite(policy.absolute_tolerance) || policy.absolute_tolerance < 0 || policy.absolute_tolerance > 1 ||
        !std::isfinite(policy.relative_tolerance) || policy.relative_tolerance < 0 || policy.relative_tolerance > 1 ||
        (policy.absolute_tolerance == 0 && policy.relative_tolerance == 0))
        return Failure("Invalid scene-probe diffuse convergence policy.");
    D3D12Renderer owner;
    auto sources = std::make_unique<Impl::SceneProbeBakeSources>();
    auto terrain_source = std::make_unique<AllocationResource>();
    struct PreviousGrid { AllocationResource sh,records; UINT sh_elements{}; };
    // Keep the submitted snapshot alive through the outer drain, including an unknown submission result.
    auto retained_grid = std::make_unique<std::shared_ptr<PreviousGrid>>();
    std::vector<std::byte> candidate;
    const auto run = [&]() -> Result {
        if (!scene || sun.type != LightType::Directional)
            return Failure("Scene-probe baking requires the actual static scene and directional sun.");
        if (auto initialized = owner.InitializeWorldTransportBake(configuration, WorldTransportBakeKind::SceneProbe); !initialized)
            return initialized;
        auto &renderer = *owner.impl_;
        if (auto prepared = renderer.PrepareSceneProbeBakeSources(*sources); !prepared) return prepared;
        auto &view = *renderer.owned_views.at(kMainRenderViewId.value);
        auto &pool = view.targets.transient_pools[0];
        std::uint32_t generation{};
        if (auto prepared = renderer.PrepareWorldTransportInstances(scene,
                sources->instances, generation, true); !prepared) return prepared;

        const auto terrain_bytes = scene->instances.size() * sizeof(Impl::GpuTerrainSection);
        D3D12MA::ALLOCATION_DESC terrain_allocation{};
        terrain_allocation.HeapType = D3D12_HEAP_TYPE_UPLOAD;
        if (auto created = renderer.CreateAllocation(*terrain_source, terrain_allocation,
                BufferDescription(terrain_bytes), D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
        Impl::GpuTerrainSection *terrain_records{};
        D3D12_RANGE no_terrain_read{};
        Native(terrain_source->resource->Map(0, &no_terrain_read,
            reinterpret_cast<void **>(&terrain_records)), "Map source-only probe terrain sections");
        for (std::size_t index = 0; index < scene->instances.size(); ++index)
        {
            terrain_records[index] = {};
            const auto &source=scene->instances[index];
            auto &record=terrain_records[index];
            record.seed=source.environment_seed; record.pageSlice=UINT_MAX;
            if(const auto section=Impl::TerrainSectionFrom(source))
            {
                record.center={section->x,section->z}; record.span={section->span_x,section->span_z};
                record.rotation={section->cosine,section->sine}; record.mesh=section->mesh;
                record.heightTransform={source.position.y,source.scale.y};
            }
        }
        terrain_source->resource->Unmap(0, nullptr);
        terrain_source->graph_access = Access::AllShaderRead;
        terrain_source->graph_queue = QueueHint::Direct;
        for (UINT frame = 0; frame < kFrameCount; ++frame)
        {
            renderer.terrain_descriptor_banks[frame].descriptors_initialized = false;
            auto handle = renderer.srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (static_cast<SIZE_T>(frame) * kTextureDescriptorCount + kTerrainPageDescriptorOffset) * renderer.srv_stride;
            const auto terrain_base = handle.ptr;
            handle.ptr = terrain_base + SIZE_T(HS_TERRAIN_SECTION_SRV) * renderer.srv_stride;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Buffer.NumElements = static_cast<UINT>(scene->instances.size());
            srv.Buffer.StructureByteStride = sizeof(Impl::GpuTerrainSection);
            renderer.device->CreateShaderResourceView(terrain_source->resource.Get(), &srv, handle);
            handle.ptr = terrain_base + SIZE_T(HS_TERRAIN_MIP_SRV) * renderer.srv_stride;
            srv.Buffer.NumElements = 1;
            srv.Buffer.StructureByteStride = sizeof(Impl::GpuTerrainPlaneMip);
            renderer.device->CreateShaderResourceView(nullptr, &srv, handle);
            handle.ptr = terrain_base + SIZE_T(HS_TERRAIN_PLANE_SRV) * renderer.srv_stride;
            srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            for (UINT index = 0; index < kTerrainPagePlaneCapacity; ++index)
            {
                renderer.device->CreateShaderResourceView(nullptr, &srv, handle);
                handle.ptr += renderer.srv_stride;
            }
        }

        LightListCandidate lights;
        if (auto prepared = view.light_list.Prepare(renderer.device.Get(), renderer.allocator,
                {1, 1}, 1, 1, [&](std::string_view id, std::vector<std::byte> &bytes) {
                    bytes = ReadProduct(renderer.config, id); return Result::Success();
                }, lights, 1); !prepared) return prepared;
        if (auto committed = view.light_list.Commit(std::move(lights), renderer.graph_queue_executor.Completed()); !committed)
            return committed;

        FrameConstants constants{};
        renderer.FillMaterialFrameConstants(constants);
        constants.shadow_atlas_texel_size.w = float(renderer.QualityProfileFor(renderer.config.shadow_quality).shadow_filter_samples);
        const auto foliage = renderer.QualityProfileFor(renderer.config.foliage_quality).foliage_density;
        if (auto selected = renderer.local_probes.PrepareCaptureMaterialOrdinals(scene, foliage); !selected) return selected;
        std::vector<LocalProbeTextureFact> texture_facts;
        for (const auto material : renderer.local_probes.CachedCaptureMaterialOrdinals(scene, foliage))
            for (const auto &id : renderer.environment_material_bindings[material].product_ids)
                if (!id.empty() && std::ranges::none_of(texture_facts, [&](const auto &fact) { return fact.logical_id == id; }))
                    texture_facts.push_back({id, 0, true});

        std::array<LocalProbeCapturePage, kEnvironmentMaterialNames.size() + 1> pages;
        std::array<LocalProbePageRevision, kEnvironmentMaterialNames.size() + 1> revisions;
        std::vector<std::pair<AllocationResource *, ResourceHandle>> imports;
        std::vector<LocalProbeSceneRead> reads;
        imports.reserve(sources->textures.size() + pages.size() + 8);
        reads.reserve(imports.capacity() + 9);
        const auto barriers = renderer.enhanced ? BarrierMode::Enhanced : BarrierMode::Legacy;
        std::shared_ptr<PreviousGrid> previous_grid;
        std::vector<Float4> previous_coefficients;
        unsigned iterations{};
        bool final_reflections{};
        for (;;)
        {
            auto completed = renderer.graph_queue_executor.Completed();
            if (renderer.local_probes.BakeStatus(completed).capture_complete)
            {
                if (auto exported=renderer.local_probes.ExportBakedPackage(
                        renderer.graph_queue_executor,pool,0,barriers,candidate); !exported) return exported;
                completed=renderer.graph_queue_executor.Completed();
                const auto baked=rd::ReadSceneProbePackage(candidate,MakeAssetId("render/lighting/local_probes.spkg"));
                if (!baked.description.grid.NodeCount() || final_reflections) return Result::Success();
                std::vector<Float4> current(baked.cosine_sh.size()/sizeof(Float4));
                std::memcpy(current.data(),baked.cosine_sh.data(),baked.cosine_sh.size());
                const auto state=rd::CompareSceneProbeIterations(previous_coefficients.empty()?std::span<const Float4>(current):
                    std::span<const Float4>(previous_coefficients),current,policy.absolute_tolerance,policy.relative_tolerance);
                if (state==rd::SceneProbeIterationState::Invalid) return Failure("Probe iteration contains invalid radiance energy or coefficients.");
                const bool converged=!previous_coefficients.empty() && state==rd::SceneProbeIterationState::Converged;
                if (!converged && ++iterations>policy.maximum_iterations)
                    return Failure("Probe diffuse iteration did not converge within its bounded budget.");
                auto next=std::make_shared<PreviousGrid>();
                const auto sh_bytes=baked.cosine_sh.size()+baked.cells.size()*80u;
                next->sh_elements=static_cast<UINT>(sh_bytes/16u);
                D3D12MA::ALLOCATION_DESC upload{}; upload.HeapType=D3D12_HEAP_TYPE_UPLOAD;
                if (auto created=renderer.CreateAllocation(next->sh,upload,BufferDescription(sh_bytes),
                        D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
                if (auto created=renderer.CreateAllocation(next->records,upload,BufferDescription(128),
                        D3D12_RESOURCE_STATE_GENERIC_READ); !created) return created;
                std::byte *mapped{}; D3D12_RANGE no_read{0,0};
                Native(next->sh.resource->Map(0,&no_read,reinterpret_cast<void **>(&mapped)),"Map previous probe grid");
                std::memcpy(mapped,baked.cosine_sh.data(),baked.cosine_sh.size());
                for (std::size_t i=0;i<baked.cells.size();++i)
                {
                    auto *cell=mapped+baked.cosine_sh.size()+i*80u;
                    std::memset(cell,0,80); std::memcpy(cell,baked.cells[i].reachable.data(),64);
                    std::memcpy(cell+64,baked.cells[i].reflections.data(),8);
                }
                next->sh.resource->Unmap(0,nullptr);
                std::array<DirectX::XMFLOAT4,8> record{};
                const auto &grid=baked.description.grid;
                const auto packed=grid.dimensions[0]+256u*grid.dimensions[1]+65536u*grid.dimensions[2];
                record[6]={grid.origin.x,grid.origin.y,grid.origin.z,float(packed)};
                record[7]={grid.spacing.x,grid.spacing.y,grid.spacing.z,float(grid.NodeCount())};
                Native(next->records.resource->Map(0,&no_read,reinterpret_cast<void **>(&mapped)),"Map previous probe grid header");
                std::memcpy(mapped,record.data(),sizeof(record)); next->records.resource->Unmap(0,nullptr);
                previous_grid=std::move(next); previous_coefficients=std::move(current);
                *retained_grid=previous_grid;
                final_reflections=converged;
                if (auto restarted=renderer.local_probes.BeginOfflineIteration(completed,final_reflections); !restarted) return restarted;
            }
            pool.ResetCompletedClaims(completed);
            renderer.graph.Reset();
            imports.clear(); reads.clear();
            const auto import = [&](AllocationResource &resource, bool texture, Access access,
                                    std::string_view name) -> ResourceHandle {
                for (const auto &[existing, handle] : imports) if (existing == &resource) return handle;
                if (auto certified = resource.EnsureNativeContract(renderer.device.Get()); !certified)
                    throw std::runtime_error(std::string(name) + ": " + std::string(certified.Message()));
                const auto initial = resource.graph_access == Access::Undefined ? access : resource.graph_access;
                const ResourceHandle handle = texture
                    ? ResourceHandle(renderer.graph.ImportTexture({resource.resource.Get(), resource.graph_completion,
                        resource.graph_queue, {}, resource.NativeContract()}, initial, name))
                    : ResourceHandle(renderer.graph.ImportBuffer({resource.resource.Get(), resource.graph_completion,
                        resource.graph_queue, {}, resource.NativeContract()}, initial, name));
                imports.emplace_back(&resource, handle);
                return handle;
            };
            const auto read = [&](AllocationResource &resource, bool texture, Access access, std::string_view name) {
                const auto handle = import(resource, texture, access, name);
                reads.push_back({renderer.graph.Version(handle), access});
                return handle;
            };
            const auto instances = import(sources->instances, false, Access::AllShaderRead, "ProbeStaticInstances");
            for (std::size_t index = 0; index < pages.size(); ++index)
            {
                auto &page = index == kEnvironmentMaterialNames.size()
                    ? renderer.procedural_geometry : renderer.environment_meshes[index * 3];
                auto &capture = pages[index];
                capture.vertices = page.storage.resource; capture.indices = page.storage.resource;
                capture.vertex_allocation = page.storage.allocation; capture.index_allocation = page.storage.allocation;
                capture.vertex_version = renderer.graph.Version(import(page.storage, false, Access::VertexShaderRead, "ProbeCompleteGeometry"));
                capture.index_version = capture.vertex_version;
                capture.vertex_view = page.vertex_view; capture.index_view = page.index_view;
                capture.index_count = page.index_count; capture.generation = page.generation; capture.page_slot = page.page_slot;
                capture.source_digest = page.source_digest; capture.bounds = page.metadata.bounds;
                capture.complete = page.storage.resource && page.index_count;
                revisions[index] = {capture.page_slot, capture.generation, capture.index_count, capture.source_digest, capture.bounds};
            }
            read(renderer.material_parameters, false, Access::AllShaderRead, "ProbeMaterialParameters");
            if (previous_grid)
            {
                read(previous_grid->sh,false,Access::AllShaderRead,"ProbePreviousGridSH");
                read(previous_grid->records,false,Access::AllShaderRead,"ProbePreviousGridHeader");
            }
            read(renderer.environment_brdf_lut, true, Access::AllShaderRead, "ProbeBrdfLut");
            read(*terrain_source, false, Access::AllShaderRead, "ProbeSourceTerrainSections");
            for (auto &texture : sources->textures)
                read(texture.resource, true, Access::AllShaderRead, texture.id);

            LocalProbeRuntimeFrameInputs input;
            input.max_cube_views = maximum_cube_views_per_graph;
            input.config = &renderer.config; input.pipeline_cache = &renderer.pipeline_cache;
            input.scene = scene; input.lights = std::span(&sun, 1); input.static_instance_generation = generation;
            input.material_parameter_revision = renderer.MaterialOverrideRevision();
            input.pipeline_revision = renderer.pipeline_revision;
            input.foliage_density = foliage; input.frame_constants = &constants; input.page_revisions = revisions;
            input.texture_facts = texture_facts; input.texture_facts_valid = true; input.pages = pages;
            input.static_instances = sources->instances.resource;
            input.static_allocation = sources->instances.allocation;
            input.static_version = renderer.graph.Version(instances);
            input.frame_srv_heap = renderer.srv_cpu_heap; input.sampler_heap = renderer.sampler_heap;
            input.frame_srv_base = renderer.srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
            constants.render_options.w=previous_grid?1.0f:0.0f;
            if (previous_grid) input.binding_owners.push_back(previous_grid);
            input.sampler_table = renderer.sampler_heap->GetGPUDescriptorHandleForHeapStart(); input.completed = completed;
            LocalProbeRuntimeFramePlan plan;
            struct Pending
            {
                Impl &renderer; LightListSystem &lights; TransientResourcePool &pool;
                const LocalProbeRuntimeFrameInputs &input; LocalProbeRuntimeFramePlan &plan;
                CompletionSet completion;
                bool probe{}, ltc{}, light_list{}, local{}, prepared{};
                ~Pending()
                {
                    if (probe) (void)renderer.lighting_probe.CompleteSubmission(renderer.graph, completion, false);
                    if (ltc) (void)renderer.ltc_lut.CompleteSubmission(renderer.graph, completion, false);
                    if (light_list) (void)lights.CompleteSubmission(renderer.graph, completion, false);
                    if (local) (void)renderer.local_probes.CompleteSubmission(renderer.graph, completion, false, input.scene, plan.revisions);
                    if (prepared) pool.SetClaimCompletion(completion);
                }
            } pending{renderer, view.light_list, pool, input, plan};
            LightingProbeOutputs probe;
            if (auto registered = renderer.lighting_probe.Register(renderer.graph, completed, probe); !registered) return registered;
            pending.probe = true;
            LightListInputs light_input;
            light_input.lights = std::span(&sun, 1); light_input.render_active = {1, 1}; light_input.completed = completed;
            DirectX::XMStoreFloat4x4(&light_input.unjittered_view, DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&light_input.unjittered_projection,
                DirectX::XMMatrixPerspectiveFovLH(DirectX::XM_PIDIV2, 1, 1000, .005f));
            light_input.near_plane = .005f; light_input.far_plane = 1000;
            LightListOutputs light_outputs;
            if (auto registered = view.light_list.Register(renderer.graph, light_input, light_outputs); !registered) return registered;
            pending.light_list = true;
            LtcLutOutputs ltc;
            if (auto registered = renderer.ltc_lut.Register(renderer.graph, completed, ltc); !registered) return registered;
            pending.ltc = true;
            const auto descriptors = [&](UINT offset, auto sources) {
                auto destination = renderer.srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
                destination.ptr += SIZE_T(offset) * renderer.srv_stride;
                for (const auto source : sources) {
                    renderer.device->CopyDescriptorsSimple(1, destination, source, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    destination.ptr += renderer.srv_stride;
                }
            };
            descriptors(kProbeDescriptorOffset, probe.srvs);
            if (previous_grid)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
                srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Buffer.NumElements=previous_grid->sh_elements; srv.Buffer.StructureByteStride=16;
                auto destination=renderer.srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
                destination.ptr+=SIZE_T(kLocalProbeDescriptorOffset+2)*renderer.srv_stride;
                renderer.device->CreateShaderResourceView(previous_grid->sh.resource.Get(),&srv,destination);
                srv.Buffer.NumElements=1; srv.Buffer.StructureByteStride=128;
                destination.ptr+=SIZE_T(2)*renderer.srv_stride;
                renderer.device->CreateShaderResourceView(previous_grid->records.resource.Get(),&srv,destination);
            }
            descriptors(kLightListDescriptorOffset, light_outputs.srvs);
            descriptors(kLightListDescriptorOffset + UINT(light_outputs.srvs.size()), ltc.srvs);
            for (const auto version : {probe.radiance, probe.prefiltered, probe.irradiance,
                    light_outputs.lights, light_outputs.ranges, light_outputs.inline_indices, light_outputs.spill_indices,
                    ltc.matrix, ltc.amplitude}) reads.push_back({version, Access::AllShaderRead});
            input.scene_reads = reads;
            if (auto prepared = renderer.local_probes.PrepareFrameDecisionData(input, plan); !prepared) return prepared;
            if (auto prepared = renderer.local_probes.PrepareFrameRecordData(input, plan); !prepared) return prepared;
            LocalProbeOutputs output;
            if (auto registered = renderer.local_probes.Register(renderer.graph, input, plan, output, nullptr); !registered) return registered;
            pending.local = true;
            if (auto prepared = renderer.graph.Prepare(pool); !prepared) return prepared;
            pending.prepared = true;
            GraphExecutionOptions options; options.task_system = renderer.config.task_system;
            const auto submitted = renderer.graph_queue_executor.Execute(renderer.graph, 0, barriers, pending.completion, &options);
            for (const auto &[resource, handle] : imports)
            {
                resource->graph_completion.Merge(pending.completion);
                if (submitted) {
                    resource->graph_access = renderer.graph.FinalAccess(handle);
                    resource->graph_queue = renderer.graph.FinalQueue(handle);
                    resource->graph_completion.Merge(renderer.graph.ResourceCompletion(handle));
                }
            }
            if (!submitted) { renderer.frame_submission_faulted = true; return submitted; }
            if (auto accepted = renderer.lighting_probe.CompleteSubmission(renderer.graph, pending.completion, true); !accepted) return accepted;
            pending.probe = false;
            if (auto accepted = renderer.ltc_lut.CompleteSubmission(renderer.graph, pending.completion, true); !accepted) return accepted;
            pending.ltc = false;
            if (auto accepted = view.light_list.CompleteSubmission(renderer.graph, pending.completion, true); !accepted) return accepted;
            pending.light_list = false;
            if (auto accepted = renderer.local_probes.CompleteSubmission(renderer.graph, pending.completion, true, scene, plan.revisions); !accepted) return accepted;
            pending.local = false;
            if (auto waited = renderer.graph_queue_executor.Wait(pending.completion); !waited) return waited;
        }
        return Result::Success();
    };
    Result result = Result::Success();
    try { result = run(); }
    catch (const std::exception &error) { result = Failure(error.what()); }
    if (owner.impl_->device)
        if (auto drained = owner.impl_->WaitForGpu(); !drained)
        {
            // The one-shot sources and allocator must survive an unreliable drain together.
            (void)sources.release();
            (void)terrain_source.release();
            (void)retained_grid.release();
            (void)owner.impl_.release();
            return Failure(std::string(result.Message()) + "; drain failed: " + std::string(drained.Message()));
        }
    sources.reset();
    terrain_source.reset();
    // The drain succeeded; release the outer snapshot owner before Shutdown destroys its allocator.
    retained_grid.reset();
    const auto cleanup = owner.Shutdown();
    if (!cleanup) return Failure(std::string(result.Message()) + "; cleanup failed: " + std::string(cleanup.Message()));
    if (!result) return result;
    if (!owner.ValidationStatus().MessageCollectionValid() || owner.ValidationErrorCount())
        return Failure("Scene-probe producer validation failed or message collection was incomplete.");
    package_bytes = std::move(candidate);
    return Result::Success();
}
#endif
} // namespace hs
