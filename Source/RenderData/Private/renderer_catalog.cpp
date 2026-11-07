#include <hs/render_data/renderer_catalog.hpp>
#include <hs/render_data/renderer_catalog_identity.hpp>
#include <hs/core/canonical_reader.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace hs::render_data
{
namespace
{
using R = CanonicalReader;
void Require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class T, std::size_t N> void Numbers(R &r, std::array<T,N> &a)
{
    Require(r.Count(4) == N, "Invalid fixed array length.");
    for (auto &x : a) x = r.Scalar<T>();
}
template<std::size_t N> void Blob(R &r, std::array<std::byte,N> &a)
{
    const auto b = r.Blob(); Require(b.size() == N, "Invalid digest length.");
    std::ranges::copy(b, a.begin());
}
template<class T, class F> void Rows(R &r, std::vector<T> &v, std::size_t minimum, F f)
{
    const auto count = r.Count(minimum); v.resize(count);
    for (auto &item : v) f(r, item);
}
void Bounds(R &r, GeometryBounds &v)
{ Numbers(r,v.minimum); Numbers(r,v.maximum); Numbers(r,v.sphere); }
void Bulk(R &r, BulkRange &v)
{ r.Fields(v.offset,v.byte_count,v.format); Blob(r,v.expected_digest); }
void Metadata(R &r, GeometryMetadata &v)
{
    r.Fields(v.abi,v.flags,v.vertex_count,v.index_count,v.submesh_count,v.meshlet_count,
        v.vertex_ref_count,v.max_material_index,v.page_dependency_index,v.fallback_dependency_index,
        v.lod,v.terminal_fallback_dependency_index);
    Bounds(r,v.bounds); r.Fields(v.geometric_error,v.error_method_abi);
}
void Transform(R &r, CharacterLocalTransform &v)
{
    Numbers(r,v.translation); r.Fields(v.translation_padding); Numbers(r,v.rotation);
    Numbers(r,v.scale); r.Fields(v.scale_padding);
}
void Texture(R &r, TextureDescriptorStorage &v)
{
    r.Fields(v.asset_id.value,v.bulk_index,v.description.width,v.description.height,
        v.description.array_size,v.description.mip_count,v.description.format);
    Rows(r,v.dependencies,8,[](R &in, AssetId &x){in.Fields(x.value);});
    Rows(r,v.subresources,36,[](R &in, TextureSubresourceLayout &x){
        in.Fields(x.offset,x.row_pitch,x.slice_pitch,x.byte_count); Blob(in,x.digest); in.Fields(x.has_digest);
    });
    r.Fields(v.dimension,v.depth);
}
void Geometry(R &r, GeometryDescriptorStorage &v)
{
    r.Fields(v.asset_id.value,v.bulk_index,v.flags,v.vertex_count,v.index_count,v.meshlet_count,
        v.vertex_ref_count,v.max_material_index,v.max_bone_index);
    Bounds(r,v.bounds);
    Require(r.Count(16) == v.chunks.size(), "Invalid geometry chunk count.");
    for (auto &x:v.chunks) r.Fields(x.offset,x.byte_count);
    Rows(r,v.dependencies,8,[](R &in,AssetId &x){in.Fields(x.value);});
    Rows(r,v.submeshes,20,[](R &in,GeometrySubmesh &x){
        in.Fields(x.first_index,x.index_count,x.first_meshlet,x.meshlet_count,x.material_index);
    });
    Rows(r,v.ground_support,56,[](R &in,ExpandedVertex &x){
        Numbers(in,x.position); Numbers(in,x.normal); Numbers(in,x.bone_indices);
        Numbers(in,x.bone_weights); Numbers(in,x.uv); Numbers(in,x.tangent); in.Fields(x.material_index,x.optical_component_id);
    });
    r.Fields(v.payload_ready);
}
void Material(R &r, MaterialDescriptorStorage &s)
{
    auto &v=s.value;
    r.Fields(v.asset_id.value,v.bulk_index,v.kind,v.surface.capability_mask,v.surface.varying_mask,
        v.surface.has_static_solid_transmission_profile);
    Numbers(r,v.surface.solid_transmission_profile);
    Blob(r,v.parameter_layout);
    Rows(r,s.defaults,8,[](R &in,MaterialParameterSlot &x){Numbers(in,x.words);});
    Rows(r,s.programs,20,[&](R &in,MaterialProgramDescriptor &x){
        in.Fields(x.kind,x.bulk_index,x.asset_id.value); s.product_ids.emplace_back(in.String()); x.product_id=s.product_ids.back();
    });
    Rows(r,s.textures,28,[&](R &in,MaterialTextureDescriptor &x){
        in.Fields(x.slot,x.texture.value,x.dimension,x.semantic,x.shader_binding_abi);
        s.product_ids.emplace_back(in.String()); x.product_id=s.product_ids.back();
    });
    r.Fields(v.metallic,v.roughness_scale,v.flags,v.domain,v.receivers.model_mask,
        v.receivers.required_surface_flags,v.receivers.forbidden_surface_flags,v.receivers.normal_filter);
}
void Character(R &r, CharacterDescriptorStorage &s)
{
    auto &v=s.value; auto &d=s.data;
    r.Fields(v.asset_id.value,v.bulk_index,v.geometry_page.value); Metadata(r,d.geometry);
    Rows(r,s.dependencies,8,[](R &in,AssetId &x){in.Fields(x.value);});
    Rows(r,d.material_bindings,16,[](R &in,CharacterMaterialBinding &x){in.Fields(x.slot,x.dependency_index,x.hero_ordinal,x.domain);});
    Rows(r,s.materials,4,[](R &in,MaterialHandle &x){in.Fields(x.value);});
    r.Fields(d.bone_count,d.material_count,d.ground_offset);
    Rows(r,d.clips,24,[](R &in,CharacterClipHeader &x){in.Fields(x.clip,x.looping,x.reserved,x.first_transform,x.frame_count,x.duration_seconds);});
    Rows(r,d.parents,4,[](R &in,std::uint16_t &x){in.Fields(x);});
    Rows(r,d.inverse_bind_matrices,72,[](R &in,auto &x){Numbers(in,x);});
    Rows(r,d.upper_body_weights,4,[](R &in,float &x){in.Fields(x);});
    Bulk(r,v.transforms);
    std::uint32_t abi{},flags{}; std::array<std::byte,32> binding{}; float margin{},bubble{};
    r.Fields(abi,flags); Blob(r,binding); r.Fields(margin,bubble);
    std::vector<GeometryMetadata> lods; std::vector<CharacterIntervalBound> intervals;
    Rows(r,lods,96,[](R &in,GeometryMetadata &x){Metadata(in,x);});
    Rows(r,intervals,36,[](R &in,CharacterIntervalBound &x){in.Fields(x.lod,x.clip,x.frame);Numbers(in,x.minimum);Numbers(in,x.maximum);});
    Bulk(r,v.final_matrices);
    if (abi || flags || !lods.empty() || !intervals.empty())
    {
        d.monster.emplace(); auto &m=*d.monster; m.abi=abi;m.flags=flags;m.bone_count=d.bone_count;
        m.skeleton_binding=binding;m.numerical_margin=margin;m.bubble_amplitude=bubble;
        m.lods=std::move(lods);m.interval_bounds=std::move(intervals);
    }
    else Require(binding == std::array<std::byte,32>{} && margin == 0 && bubble == 0,
                 "Unexpected deformation fields.");
}
void Animation(R &r, AnimationDescriptorStorage &s)
{
    auto &v=s.value; auto &d=s.data;
    r.Fields(v.asset_id.value,v.bulk_index,v.character.value,d.character_asset_id.value,d.sample_rate_hz);
    Rows(r,d.bone_names,8,[](R &in,std::string &x){x=in.String();});
    Rows(r,d.parents,4,[](R &in,std::uint16_t &x){in.Fields(x);});
    Rows(r,d.inverse_bind_matrices,72,[](R &in,auto &x){Numbers(in,x);});
    Rows(r,d.clips,28,[](R &in,NamedCharacterClip &x){
        in.Fields(x.id.value); x.name=in.String(); in.Fields(x.first_transform,x.frame_count,x.duration_seconds,x.looping);
    });
    Rows(r,d.sockets,20,[](R &in,CharacterAnimationSocket &x){x.name=in.String();in.Fields(x.bone_index);Transform(in,x.local_transform);});
    Bulk(r,v.transforms);
    for (const auto &x:d.bone_names) s.bone_names.push_back(x);
    for (const auto &x:d.clips) s.clips.push_back({x.id,x.name,x.first_transform,x.frame_count,x.duration_seconds,x.looping});
    for (const auto &x:d.sockets) s.sockets.push_back({x.name,x.bone_index,x.local_transform});
}
void Atlas(R &r,VfxMeshAtlasDescriptor &v)
{
    r.Fields(v.asset_id.value); Require(r.Count(20)==v.meshes.size(),"Invalid atlas mesh count.");
    for(auto &x:v.meshes) r.Fields(x.vertex_count,x.transverse_radius,x.minimum_z,x.widest_z,x.maximum_z);
}
}

std::vector<std::byte> EncodeRendererCatalog(RendererCatalogView v)
{
    CanonicalBytes b("project_hs.renderer_catalog",2);
    b.Sequence(v.bulks,[](CanonicalBytes &out,const auto &x){out.Scalar(x.asset_id.value);content_identity::Bulk(out,x.range);});
    b.Sequence(v.textures,content_identity::Texture); b.Sequence(v.geometry,content_identity::Geometry);
    b.Sequence(v.materials,content_identity::Material); b.Sequence(v.characters,content_identity::Character);
    b.Sequence(v.animations,content_identity::Animation);
    b.Scalar(v.atlas != nullptr); if(v.atlas) content_identity::Atlas(b,*v.atlas);
    b.Sequence(v.source_revisions,[](CanonicalBytes &out,const auto &x){out.Scalar(x.asset_id.value);out.String(x.product_id);out.Blob(x.digest);});
    b.Sequence(v.revision_bulks,[](CanonicalBytes &out,const auto &x){out.String(x.product_id);out.Scalar(x.bulk_index);});
    return std::move(b).Take();
}

Result DecodeRendererCatalog(std::span<const std::byte> bytes,std::uint64_t pack_size,RendererCatalogData &out)
{
    try
    {
        R r(bytes,"project_hs.renderer_catalog",2); RendererCatalogData next;
        Rows(r,next.bulks,60,[](R &in,RendererBulkDescriptor &x){in.Fields(x.asset_id.value);Bulk(in,x.range);});
        Rows(r,next.texture_storage,48,Texture); Rows(r,next.geometry_storage,100,Geometry);
        Rows(r,next.material_storage,84,Material); Rows(r,next.character_storage,120,Character);
        Rows(r,next.animation_storage,64,Animation);
        r.Fields(next.has_atlas); if(next.has_atlas) Atlas(r,next.atlas);
        Rows(r,next.source_revisions,24,[&](R &in,RendererSourceRevision &x){in.Fields(x.asset_id.value);next.revision_strings.emplace_back(in.String());x.product_id=next.revision_strings.back();Blob(in,x.digest);});
        Rows(r,next.revision_bulks,12,[&](R &in,RendererRevisionBulk &x){next.revision_strings.emplace_back(in.String());x.product_id=next.revision_strings.back();in.Fields(x.bulk_index);});
        r.Finish();
        std::vector<AssetId> assets; assets.reserve(next.bulks.size());
        std::unordered_set<std::uint64_t> ids;
        for(const auto &x:next.bulks)
        {
            Require(x.asset_id.value && ids.insert(x.asset_id.value).second && x.range.format <= RenderBulkFormat::SurfaceLayoutAbi &&
                x.range.offset <= pack_size && x.range.byte_count <= pack_size-x.range.offset,"Invalid renderer bulk.");
            assets.push_back(x.asset_id);
        }
        const auto product = [&](AssetId asset,std::uint32_t index, RenderBulkFormat format) -> std::uint64_t {
            Require(index < next.bulks.size() && next.bulks[index].asset_id.value == asset.value &&
                    next.bulks[index].range.format == format && next.bulks[index].range.byte_count,
                    "Invalid descriptor bulk identity or format.");
            return next.bulks[index].range.byte_count;
        };
        for(const auto &x:next.texture_storage) { auto v=x.View();const auto size=product(v.asset_id,v.bulk_index,RenderBulkFormat::Package);
            if(v.dimension==TextureDimension::Texture2DArray) ValidateTextureDescriptor(v,size);
            else { Require(v.dimension==TextureDimension::Texture3D,"Invalid texture dimension.");ValidateVolumeTextureDescriptor(v,size); }
            next.textures.push_back(v); }
        for(const auto &x:next.geometry_storage) { auto v=x.View();ValidateGeometryDescriptor(v,product(v.asset_id,v.bulk_index,RenderBulkFormat::GeometryPage));next.geometry.push_back(v); }
        for(const auto &x:next.material_storage) { auto v=x.View();(void)product(v.asset_id,v.bulk_index,RenderBulkFormat::Package);ValidateMaterialDescriptor(v,assets,next.textures);next.materials.push_back(v); }
        for(const auto &x:next.character_storage) { auto v=x.View();ValidateCharacterDescriptor(v,next.bulks,next.geometry,next.materials);next.characters.push_back(v); }
        for(const auto &x:next.animation_storage) { auto v=x.View();ValidateAnimationDescriptor(v,next.bulks,next.characters);next.animations.push_back(v); }
        if(next.has_atlas) { Require(std::ranges::any_of(next.bulks,[&](const auto &x){return x.asset_id.value==next.atlas.asset_id.value && x.range.format==RenderBulkFormat::VfxMeshAtlas && x.range.byte_count != 0;}),"Invalid atlas asset."); for(const auto &x:next.atlas.meshes)
            Require(x.vertex_count && x.vertex_count%3==0 && std::isfinite(x.transverse_radius) && x.transverse_radius>0 &&
                std::isfinite(x.minimum_z) && std::isfinite(x.widest_z) && std::isfinite(x.maximum_z) &&
                x.minimum_z<x.maximum_z && x.widest_z>=x.minimum_z && x.widest_z<=x.maximum_z,"Invalid atlas bounds."); }
        for(const auto &x:next.source_revisions) Require(x.asset_id.value && MakeAssetId(x.product_id).value==x.asset_id.value,"Invalid source revision.");
        for(const auto &x:next.revision_bulks) Require(x.bulk_index<next.bulks.size() && MakeAssetId(x.product_id).value==next.bulks[x.bulk_index].asset_id.value,"Invalid revision bulk.");
        out=std::move(next); return Result::Success();
    }
    catch(const std::exception &e) { return Result::Failure(ErrorCode::InvalidArgument,"hs_render_data",e.what()); }
}
}
