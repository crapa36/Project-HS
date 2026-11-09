#include <hs/render_data/renderer_catalog.hpp>
#include <hs/render_data/renderer_catalog_identity.hpp>
#include <hs/core/canonical_reader.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

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
    r.Fields(v.metallic,v.roughness_scale,v.flags,v.opacity_texture_mask,v.domain,v.receivers.model_mask,
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
    CanonicalBytes b("project_hs.renderer_catalog",3);
    b.Sequence(v.bulks,[](CanonicalBytes &out,const auto &x){out.Scalar(x.asset_id.value);content_identity::Bulk(out,x.range);});
    b.Sequence(v.textures,content_identity::Texture); b.Sequence(v.geometry,content_identity::Geometry);
    b.Sequence(v.materials,content_identity::Material); b.Sequence(v.characters,content_identity::Character);
    b.Sequence(v.animations,content_identity::Animation);
    b.Scalar(v.atlas != nullptr); if(v.atlas) content_identity::Atlas(b,*v.atlas);
    b.Sequence(v.source_revisions,[](CanonicalBytes &out,const auto &x){out.Scalar(x.asset_id.value);out.String(x.product_id);out.Blob(x.digest);});
    b.Sequence(v.revision_bulks,[](CanonicalBytes &out,const auto &x){out.String(x.product_id);out.Scalar(x.bulk_index);});
    return std::move(b).Take();
}

Result ValidateRendererCatalog(RendererCatalogView view, std::optional<std::uint64_t> pack_size)
{
    try
    {
        Require(view.bulks.size() <= UINT32_MAX && view.textures.size() <= UINT32_MAX &&
            view.geometry.size() <= UINT32_MAX && view.materials.size() <= UINT32_MAX &&
            view.characters.size() <= UINT32_MAX && view.animations.size() <= UINT32_MAX,
            "Renderer table exceeds its handle ABI.");
        const auto rows = [](auto entries, auto validate) {
            std::uint64_t previous{};
            for (const auto &entry : entries)
            {
                Require(entry.asset_id.value > previous, "Renderer table IDs are zero, duplicated or unordered.");
                validate(entry);
                previous = entry.asset_id.value;
            }
        };
        std::vector<AssetId> assets; assets.reserve(view.bulks.size());
        rows(view.bulks, [&](const auto &entry) {
            const auto &range = entry.range;
            Require(range.format <= RenderBulkFormat::SurfaceLayoutAbi && range.byte_count &&
                range.byte_count <= SIZE_MAX, "Invalid renderer bulk size or format.");
            if (pack_size)
                Require(range.offset >= 64 && range.offset % 64 == 0 && range.offset <= *pack_size &&
                    range.byte_count <= *pack_size - range.offset, "Renderer bulk exceeds its aligned pack range.");
            assets.push_back(entry.asset_id);
        });
        const auto product = [&](AssetId asset, std::uint32_t index, RenderBulkFormat format) -> const RendererBulkDescriptor & {
            Require(index < view.bulks.size() && view.bulks[index].asset_id.value == asset.value &&
                view.bulks[index].range.format == format, "Invalid descriptor bulk identity or format.");
            return view.bulks[index];
        };
        rows(view.textures, [&](const auto &v) {
            const auto size = product(v.asset_id,v.bulk_index,RenderBulkFormat::Package).range.byte_count;
            if (v.dimension == TextureDimension::Texture2DArray) ValidateTextureDescriptor(v,size);
            else { Require(v.dimension == TextureDimension::Texture3D,"Invalid texture dimension."); ValidateVolumeTextureDescriptor(v,size); }
        });
        rows(view.geometry, [&](const auto &v) {
            ValidateGeometryDescriptor(v,product(v.asset_id,v.bulk_index,RenderBulkFormat::GeometryPage).range.byte_count);
        });
        rows(view.materials, [&](const auto &v) {
            (void)product(v.asset_id,v.bulk_index,RenderBulkFormat::Package);
            ValidateMaterialDescriptor(v,assets,view.textures);
            for (const auto &program : v.programs)
                Require(program.bulk_index < view.bulks.size() &&
                    view.bulks[program.bulk_index].range.format == RenderBulkFormat::ShaderDxil,
                    "Material program is not a shader bulk.");
        });
        rows(view.characters,[&](const auto &v) { ValidateCharacterDescriptor(v,view.bulks,view.geometry,view.materials); });
        rows(view.animations,[&](const auto &v) { ValidateAnimationDescriptor(v,view.bulks,view.characters); });
        if (view.atlas)
        {
            const auto &atlas = *view.atlas;
            const auto found = std::lower_bound(view.bulks.begin(),view.bulks.end(),atlas.asset_id.value,
                [](const auto &entry,std::uint64_t id) { return entry.asset_id.value < id; });
            Require(found != view.bulks.end() && found->asset_id.value == atlas.asset_id.value &&
                found->range.format == RenderBulkFormat::VfxMeshAtlas,"Invalid atlas asset.");
            std::uint64_t vertices{};
            for (const auto &x : atlas.meshes)
            {
                Require(x.vertex_count && x.vertex_count%3==0 && std::isfinite(x.transverse_radius) && x.transverse_radius>0 &&
                    std::isfinite(x.minimum_z) && std::isfinite(x.widest_z) && std::isfinite(x.maximum_z) &&
                    x.minimum_z<x.maximum_z && x.widest_z>=x.minimum_z && x.widest_z<=x.maximum_z,"Invalid atlas bounds.");
                vertices += x.vertex_count;
            }
            Require(vertices <= (64ull*1024*1024/4-25)/12 && found->range.byte_count == 4*(25+vertices*12),
                "Atlas size differs from its mesh table.");
        }
        rows(view.source_revisions, [&](const auto &source) {
            const auto found = std::lower_bound(view.bulks.begin(),view.bulks.end(),source.asset_id.value,
                [](const auto &entry,std::uint64_t id) { return entry.asset_id.value < id; });
            Require(!source.product_id.empty() && MakeAssetId(source.product_id).value==source.asset_id.value &&
                (found==view.bulks.end() || found->asset_id.value!=source.asset_id.value),"Invalid source revision or bulk overlap.");
        });
        Require(view.revision_bulks.size() <= view.bulks.size(),"Bulk revisions exceed the bulk catalogue.");
        std::vector<bool> seen(view.bulks.size());
        for (const auto &entry : view.revision_bulks)
        {
            Require(!entry.product_id.empty() && IsRendererMaterialRevisionProduct(entry.product_id) &&
                entry.bulk_index < view.bulks.size() && !seen[entry.bulk_index],"Invalid or duplicated revision bulk.");
            const auto id = entry.product_id;
            const auto format = id.ends_with(".rpkg") ? RenderBulkFormat::Package : id.ends_with(".rpage") ? RenderBulkFormat::GeometryPage :
                id.ends_with(".ghbin") ? RenderBulkFormat::GeometryHierarchy : id.ends_with(".shl1") ? RenderBulkFormat::SceneHlod :
                id.ends_with(".dds") ? RenderBulkFormat::DdsTexture : RenderBulkFormat::ShaderDxil;
            const auto &bulk = view.bulks[entry.bulk_index];
            Require(MakeAssetId(id).value==bulk.asset_id.value && bulk.range.format==format &&
                (format!=RenderBulkFormat::ShaderDxil || id.ends_with(".dxil")),"Revision bulk identity or format mismatch.");
            seen[entry.bulk_index]=true;
        }
        return Result::Success();
    }
    catch(const std::exception &e) { return Result::Failure(ErrorCode::InvalidArgument,"hs_render_data",e.what()); }
}

Result DecodeRendererCatalog(std::span<const std::byte> bytes,std::uint64_t pack_size,
                            std::shared_ptr<const RendererCatalogData> &out)
{
    try
    {
        R r(bytes,"project_hs.renderer_catalog",3);
        auto next=std::shared_ptr<RendererCatalogData>(new RendererCatalogData);
        Rows(r,next->bulks,60,[](R &in,RendererBulkDescriptor &x){in.Fields(x.asset_id.value);Bulk(in,x.range);});
        Rows(r,next->texture_storage,48,Texture); Rows(r,next->geometry_storage,100,Geometry);
        Rows(r,next->material_storage,84,Material); Rows(r,next->character_storage,120,Character);
        Rows(r,next->animation_storage,64,Animation);
        r.Fields(next->has_atlas); if(next->has_atlas) Atlas(r,next->atlas);
        Rows(r,next->source_revisions,24,[&](R &in,RendererSourceRevision &x){in.Fields(x.asset_id.value);next->revision_strings.emplace_back(in.String());x.product_id=next->revision_strings.back();Blob(in,x.digest);});
        Rows(r,next->revision_bulks,12,[&](R &in,RendererRevisionBulk &x){next->revision_strings.emplace_back(in.String());x.product_id=next->revision_strings.back();in.Fields(x.bulk_index);});
        r.Finish();
        for(const auto &x:next->texture_storage) next->textures.push_back(x.View());
        for(const auto &x:next->geometry_storage) next->geometry.push_back(x.View());
        for(const auto &x:next->material_storage) next->materials.push_back(x.View());
        for(const auto &x:next->character_storage) next->characters.push_back(x.View());
        for(const auto &x:next->animation_storage) next->animations.push_back(x.View());
        if(auto valid=ValidateRendererCatalog(next->View(),pack_size);!valid) return valid;
        next->pack_size_=pack_size;
        out=std::move(next); return Result::Success();
    }
    catch(const std::exception &e) { return Result::Failure(ErrorCode::InvalidArgument,"hs_render_data",e.what()); }
}
}
