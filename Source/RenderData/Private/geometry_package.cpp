#include <hs/render_data/geometry_package.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace hs::render_data
{
namespace
{
void Require(bool ok, std::string_view why)
{ if (!ok) throw std::runtime_error("geometry package: " + std::string(why)); }
void Put(std::span<std::byte> b, std::size_t o, std::uint32_t v, std::size_t n = 4)
{ Require(o <= b.size() && n <= b.size()-o, "write range"); for (std::size_t i=0;i<n;++i) b[o+i]=std::byte((v>>(i*8))&255); }
std::uint32_t Get(std::span<const std::byte> b, std::size_t o, std::size_t n=4)
{ Require(o<=b.size() && n<=b.size()-o,"read range"); std::uint32_t v=0; for(std::size_t i=0;i<n;++i)v|=std::uint32_t(std::to_integer<unsigned>(b[o+i]))<<(i*8); return v; }
void PF(std::span<std::byte> b,std::size_t o,float v){Put(b,o,std::bit_cast<std::uint32_t>(v));}
float GF(std::span<const std::byte>b,std::size_t o){return std::bit_cast<float>(Get(b,o));}
bool Range(std::uint32_t first,std::uint32_t count,std::size_t size){return first<=size && count<=size-first;}
void Bounds(const GeometryBounds& b)
{
    for(std::size_t i=0;i<3;++i)Require(std::isfinite(b.minimum[i]) && std::isfinite(b.maximum[i]) && b.minimum[i]<=b.maximum[i],"invalid AABB");
    for(float v:b.sphere)Require(std::isfinite(v),"nonfinite sphere");
    Require(b.sphere[3]>=0,"negative sphere radius");
}
void Encloses(const std::array<float,4>& s,const ExpandedVertex& v)
{ double d=0;for(std::size_t k=0;k<3;++k){double q=double(v.position[k])-s[k];d+=q*q;}Require(std::sqrt(d)<=double(s[3]),"sphere excludes referenced vertex"); }
GeometryBounds PayloadBounds(std::span<const ExpandedVertex> vertices)
{
    Require(!vertices.empty(), "empty page bounds input");
    GeometryBounds bounds;bounds.minimum=bounds.maximum=vertices.front().position;
    for(const auto&vertex:vertices)for(std::size_t lane=0;lane<3;++lane)
    {bounds.minimum[lane]=std::min(bounds.minimum[lane],vertex.position[lane]);bounds.maximum[lane]=std::max(bounds.maximum[lane],vertex.position[lane]);}
    for(std::size_t lane=0;lane<3;++lane)bounds.sphere[lane]=static_cast<float>((double(bounds.minimum[lane])+bounds.maximum[lane])*0.5);
    double radius=0;
    for(const auto&vertex:vertices){double squared=0;for(std::size_t lane=0;lane<3;++lane){const double delta=double(vertex.position[lane])-bounds.sphere[lane];squared+=delta*delta;}radius=std::max(radius,std::sqrt(squared));}
    Require(std::isfinite(radius) && radius<=std::numeric_limits<float>::max(),"unrepresentable page sphere");
    bounds.sphere[3]=static_cast<float>(radius);
    if(radius>0)bounds.sphere[3]=std::nextafter(bounds.sphere[3],std::numeric_limits<float>::infinity());
    Bounds(bounds);return bounds;
}
void Shape(const GeometryMetadata& m)
{
    Require(m.abi==kGeometryAbi && m.error_method_abi==kGeometryErrorMethodAbi && !(m.flags&~7u),"unsupported geometry ABI/flags");
    Require(m.vertex_count>0 && m.vertex_count<=kMaxGeometryVertices && m.index_count>0 && m.index_count<=kMaxGeometryIndices && m.index_count%3==0 && m.vertex_count<=m.index_count && m.submesh_count>0 && m.submesh_count<=m.index_count/3 && m.meshlet_count>0 && m.meshlet_count<=m.index_count/3 && m.vertex_ref_count>0 && m.vertex_ref_count<=m.index_count && m.max_material_index<=65535,"invalid geometry counts");
    Require(std::isfinite(m.geometric_error) && m.geometric_error>=0 && m.lod<=2 && (m.lod!=0 || m.geometric_error==0),"invalid LOD error");
    Require((m.flags&kGeometryFlagSkinned)==0 || (m.lod==0 && m.fallback_dependency_index==kNoGeometryDependency && m.terminal_fallback_dependency_index==kNoGeometryDependency),"skinned fallback unsupported");
    Bounds(m.bounds);
}
void Dependencies(const GeometryMetadata& m,std::span<const std::string>d,std::string_view id)
{
    Require(m.page_dependency_index<d.size(),"missing geometry page dependency");
    const auto& page=d[m.page_dependency_index];
    Require(page.starts_with("render/geometry/") && page.ends_with(".rpage"),"invalid geometry page product");
    if(!id.empty())
    {
        Require(id.starts_with("render/") && id.ends_with(".rpkg"),"invalid metadata product");
        const auto stem=std::string(id.substr(7,id.size()-7-5));
        Require(page=="render/geometry/"+stem+".rpage","geometry page does not match metadata");
        const auto base=m.lod==0?stem:stem.substr(0,stem.size()-5);
        Require(m.lod==0 || stem.ends_with("_lod"+std::to_string(m.lod)),"LOD product suffix mismatch");
        if(!(m.flags&kGeometryFlagSkinned))
        {
            const auto check=[&](std::uint16_t index,unsigned lod){Require(index<d.size() && d[index]=="render/"+base+"_lod"+std::to_string(lod)+".rpkg","invalid fallback product");};
            if(m.lod<2){check(m.fallback_dependency_index,m.lod+1);check(m.terminal_fallback_dependency_index,2);}
            else Require(m.fallback_dependency_index==kNoGeometryDependency && m.terminal_fallback_dependency_index==kNoGeometryDependency,"terminal LOD has fallback");
        }
    }
    for(auto index:{m.fallback_dependency_index,m.terminal_fallback_dependency_index})
        Require(index==kNoGeometryDependency || (index<d.size() && index!=m.page_dependency_index && d[index].starts_with("render/") && d[index].ends_with(".rpkg")),"invalid fallback dependency");
}
std::vector<std::byte> Words(std::span<const std::uint32_t> values)
{std::vector<std::byte>b(values.size()*4);for(std::size_t i=0;i<values.size();++i)Put(b,i*4,values[i]);return b;}
std::vector<std::uint32_t> ReadWords(std::span<const std::byte>b)
{std::vector<std::uint32_t>v(b.size()/4);for(std::size_t i=0;i<v.size();++i)v[i]=Get(b,i*4);return v;}
}
std::vector<std::byte> EncodeGeometryMetadata(const GeometryMetadata&m,std::span<const std::string>d,std::string_view id)
{
    Shape(m);Dependencies(m,d,id);std::vector<std::byte>b(96);
    const std::array words{m.abi,m.flags,m.vertex_count,m.index_count,m.submesh_count,m.meshlet_count,m.vertex_ref_count,m.max_material_index};
    for(std::size_t i=0;i<words.size();++i)Put(b,i*4,words[i]);
    Put(b,32,m.page_dependency_index,2);Put(b,34,m.fallback_dependency_index,2);Put(b,36,m.lod,2);Put(b,38,m.terminal_fallback_dependency_index,2);
    for(std::size_t i=0;i<3;++i){PF(b,40+i*4,m.bounds.minimum[i]);PF(b,52+i*4,m.bounds.maximum[i]);}
    for(std::size_t i=0;i<4;++i)PF(b,64+i*4,m.bounds.sphere[i]);PF(b,80,m.geometric_error);Put(b,84,m.error_method_abi);return b;
}
GeometryMetadata DecodeGeometryMetadata(std::span<const std::byte>b,std::span<const std::string>d,std::string_view id)
{
    Require(b.size()==96 && Get(b,88)==0 && Get(b,92)==0,"invalid metadata size/reserved");GeometryMetadata m;
    m.abi=Get(b,0);m.flags=Get(b,4);m.vertex_count=Get(b,8);m.index_count=Get(b,12);m.submesh_count=Get(b,16);m.meshlet_count=Get(b,20);m.vertex_ref_count=Get(b,24);m.max_material_index=Get(b,28);
    m.page_dependency_index=std::uint16_t(Get(b,32,2));m.fallback_dependency_index=std::uint16_t(Get(b,34,2));m.lod=std::uint16_t(Get(b,36,2));m.terminal_fallback_dependency_index=std::uint16_t(Get(b,38,2));
    for(std::size_t i=0;i<3;++i){m.bounds.minimum[i]=GF(b,40+i*4);m.bounds.maximum[i]=GF(b,52+i*4);}for(std::size_t i=0;i<4;++i)m.bounds.sphere[i]=GF(b,64+i*4);m.geometric_error=GF(b,80);m.error_method_abi=Get(b,84);Shape(m);Dependencies(m,d,id);return m;
}
void ValidateGeometryData(const GeometryData&g)
{
    Require(g.vertices.size()<=kMaxGeometryVertices && g.indices.size()<=kMaxGeometryIndices &&
            g.submeshes.size()<=kMaxGeometryIndices/3 && g.meshlets.size()<=kMaxGeometryIndices/3 &&
            g.meshlet_vertex_refs.size()<=kMaxGeometryIndices && g.meshlet_triangles.size()<=kMaxGeometryIndices/3 &&
            g.canonical_primitive_remap.size()<=kMaxGeometryIndices/3,"geometry allocation cap");
    GeometryMetadata m;m.flags=g.flags;m.vertex_count=static_cast<std::uint32_t>(g.vertices.size());m.index_count=static_cast<std::uint32_t>(g.indices.size());m.submesh_count=static_cast<std::uint32_t>(g.submeshes.size());m.meshlet_count=static_cast<std::uint32_t>(g.meshlets.size());m.vertex_ref_count=static_cast<std::uint32_t>(g.meshlet_vertex_refs.size());m.bounds=g.bounds;Shape(m);
    Require(g.meshlet_triangles.size()==g.indices.size()/3 && g.canonical_primitive_remap.size()==g.meshlet_triangles.size(),"primitive component counts disagree");
    (void)EncodeExpandedVertices(g.vertices); // finite scalar and complete record validation
    std::vector<bool>used(g.vertices.size()),seen(g.indices.size()/3);
    for(auto index:g.indices){Require(index<g.vertices.size(),"canonical index range");used[index]=true;}
    Require(std::ranges::all_of(used,[](bool v){return v;}),"unreferenced unique vertex");
    for(const auto&v:g.vertices){for(std::size_t k=0;k<3;++k)Require(v.position[k]>=g.bounds.minimum[k] && v.position[k]<=g.bounds.maximum[k],"AABB excludes vertex");Encloses(g.bounds.sphere,v);}
    std::uint32_t next_index=0,next_meshlet=0,next_ref=0,next_tri=0;
    for(std::size_t s=0;s<g.submeshes.size();++s)
    {
        const auto&run=g.submeshes[s];Require(run.first_index==next_index && run.index_count>0 && run.index_count%3==0 && Range(run.first_index,run.index_count,g.indices.size()) && run.first_meshlet==next_meshlet && run.meshlet_count>0 && Range(run.first_meshlet,run.meshlet_count,g.meshlets.size()) && run.material_index<=65535,"invalid material run");
        if(s)Require(g.submeshes[s-1].material_index!=run.material_index,"adjacent material runs must be merged");
        for(std::uint32_t i=run.first_index;i<run.first_index+run.index_count;++i)Require(g.vertices[g.indices[i]].material_index==run.material_index,"mixed canonical material run");
        for(std::uint32_t j=run.first_meshlet;j<run.first_meshlet+run.meshlet_count;++j)
        {
            const auto&cl=g.meshlets[j];Require(cl.submesh_index==s && cl.first_vertex_ref==next_ref && cl.first_triangle==next_tri && cl.vertex_ref_count>0 && cl.vertex_ref_count<=64 && cl.triangle_count>0 && cl.triangle_count<=124 && Range(cl.first_vertex_ref,cl.vertex_ref_count,g.meshlet_vertex_refs.size()) && Range(cl.first_triangle,cl.triangle_count,g.meshlet_triangles.size()) && !(cl.flags&~kGeometryMeshletConeValid),"invalid meshlet range");
            for(float v:cl.sphere)Require(std::isfinite(v),"nonfinite meshlet sphere");Require(cl.sphere[3]>=0,"negative meshlet radius");for(float v:cl.cone)Require(std::isfinite(v),"nonfinite cone");
            if(cl.flags&kGeometryMeshletConeValid){double n=0;for(std::size_t k=0;k<3;++k)n+=double(cl.cone[k])*cl.cone[k];Require(g.flags==0 && std::abs(n-1)<=0.001 && cl.cone[3]>=0 && cl.cone[3]<1,"invalid culling cone");}
            else Require(cl.cone==std::array<float,4>{0,0,0,1},"disabled cone payload must be canonical");
            for(std::uint32_t k=0;k<cl.vertex_ref_count;++k){auto v=g.meshlet_vertex_refs[cl.first_vertex_ref+k];Require(v<g.vertices.size() && g.vertices[v].material_index==run.material_index,"meshlet vertex/material range");Encloses(cl.sphere,g.vertices[v]);}
            for(std::uint32_t k=0;k<cl.triangle_count;++k)
            {
                const auto t=cl.first_triangle+k,p=g.canonical_primitive_remap[t],packed=g.meshlet_triangles[t];Require((packed>>24)==0 && p<seen.size() && !seen[p] && p*3>=run.first_index && p*3<run.first_index+run.index_count,"invalid primitive remap");seen[p]=true;
                for(std::size_t c=0;c<3;++c){auto local=(packed>>(c*8))&255;Require(local<cl.vertex_ref_count && g.meshlet_vertex_refs[cl.first_vertex_ref+local]==g.indices[p*3+c],"oriented primitive remap mismatch");}
                if(cl.flags&kGeometryMeshletConeValid)
                {
                    const auto&a=g.vertices[g.indices[p*3]].position;const auto&b=g.vertices[g.indices[p*3+1]].position;const auto&c=g.vertices[g.indices[p*3+2]].position;
                    const std::array<double,3>x{double(b[0])-a[0],double(b[1])-a[1],double(b[2])-a[2]},y{double(c[0])-a[0],double(c[1])-a[1],double(c[2])-a[2]};
                    const std::array n{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};
                    Require(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]>0,"degenerate primitive has culling cone");
                }
            }
            next_ref+=cl.vertex_ref_count;next_tri+=cl.triangle_count;
        }
        next_index+=run.index_count;next_meshlet+=run.meshlet_count;
    }
    Require(next_index==g.indices.size() && next_meshlet==g.meshlets.size() && next_ref==g.meshlet_vertex_refs.size() && next_tri==g.meshlet_triangles.size() && std::ranges::all_of(seen,[](bool v){return v;}),"geometry ranges do not cover components");
}
void ValidateGeometryPage(const GeometryMetadata&m,const GeometryData&g)
{
    Shape(m);ValidateGeometryData(g);std::uint32_t max_material=0;for(const auto&v:g.vertices)max_material=std::max(max_material,std::uint32_t(v.material_index));
    Require(m.flags==g.flags && m.vertex_count==g.vertices.size() && m.index_count==g.indices.size() && m.submesh_count==g.submeshes.size() && m.meshlet_count==g.meshlets.size() && m.vertex_ref_count==g.meshlet_vertex_refs.size() && m.max_material_index==max_material && m.bounds.minimum==g.bounds.minimum && m.bounds.maximum==g.bounds.maximum && m.bounds.sphere==g.bounds.sphere,"page disagrees with metadata");
}
std::vector<std::byte> BuildGeometryPage(AssetId id,std::span<const std::string>d,const GeometryData&g)
{
    ValidateGeometryData(g);const auto vertices=EncodeExpandedVertices(g.vertices),indices=Words(g.indices),refs=Words(g.meshlet_vertex_refs),triangles=Words(g.meshlet_triangles),remap=Words(g.canonical_primitive_remap);
    std::vector<std::byte>submeshes(g.submeshes.size()*20),meshlets(g.meshlets.size()*64);
    for(std::size_t i=0;i<g.submeshes.size();++i){const auto&s=g.submeshes[i];const std::array w{s.first_index,s.index_count,s.first_meshlet,s.meshlet_count,s.material_index};for(std::size_t k=0;k<5;++k)Put(submeshes,i*20+k*4,w[k]);}
    for(std::size_t i=0;i<g.meshlets.size();++i){const auto&m=g.meshlets[i];const std::array w{m.first_vertex_ref,m.vertex_ref_count,m.first_triangle,m.triangle_count,m.submesh_index};for(std::size_t k=0;k<5;++k)Put(meshlets,i*64+k*4,w[k]);for(std::size_t k=0;k<4;++k){PF(meshlets,i*64+20+k*4,m.sphere[k]);PF(meshlets,i*64+36+k*4,m.cone[k]);}Put(meshlets,i*64+52,m.flags);}
    const std::array chunks{ChunkSource{14,1,true,vertices,g.vertices.size(),76},ChunkSource{15,1,true,indices,g.indices.size(),4},ChunkSource{16,1,true,submeshes,g.submeshes.size(),20},ChunkSource{17,1,true,meshlets,g.meshlets.size(),64},ChunkSource{18,1,true,refs,g.meshlet_vertex_refs.size(),4},ChunkSource{19,1,true,triangles,g.meshlet_triangles.size(),4},ChunkSource{20,1,true,remap,g.canonical_primitive_remap.size(),4}};return BuildPackage(id,d,chunks);
}
GeometryData DecodeGeometryPagePayload(std::span<const std::byte>b,AssetId id,std::uint32_t flags)
{
    Require(!(flags&~7u),"unsupported geometry page flags");auto p=ReadPackage(b,id);Require(p.chunks.size()==7,"unexpected geometry page chunks");std::array<const ChunkView*,21>c{};for(const auto&v:p.chunks){auto type=static_cast<std::uint32_t>(v.type);Require(type>=14 && type<=20 && v.required && v.version==1,"unsupported geometry page component");c[type]=&v;}for(unsigned t=14;t<=20;++t)Require(c[t]!=nullptr,"missing geometry component");
    Require(c[19]->count==c[15]->count/3 && c[20]->count==c[15]->count/3,"page component count mismatch");GeometryData g;g.flags=flags;g.vertices=DecodeExpandedVertices(c[14]->bytes);g.bounds=PayloadBounds(g.vertices);g.indices=ReadWords(c[15]->bytes);g.meshlet_vertex_refs=ReadWords(c[18]->bytes);g.meshlet_triangles=ReadWords(c[19]->bytes);g.canonical_primitive_remap=ReadWords(c[20]->bytes);
    for(std::size_t i=0;i<c[16]->count;++i){auto s=c[16]->bytes.subspan(i*20,20);g.submeshes.push_back({Get(s,0),Get(s,4),Get(s,8),Get(s,12),Get(s,16)});}
    for(std::size_t i=0;i<c[17]->count;++i){auto s=c[17]->bytes.subspan(i*64,64);Require(Get(s,56)==0 && Get(s,60)==0,"meshlet reserved fields");GeometryMeshlet cl;cl.first_vertex_ref=Get(s,0);cl.vertex_ref_count=Get(s,4);cl.first_triangle=Get(s,8);cl.triangle_count=Get(s,12);cl.submesh_index=Get(s,16);for(std::size_t k=0;k<4;++k){cl.sphere[k]=GF(s,20+k*4);cl.cone[k]=GF(s,36+k*4);}cl.flags=Get(s,52);g.meshlets.push_back(cl);}ValidateGeometryData(g);return g;
}
GeometryData DecodeGeometryPage(std::span<const std::byte>b,AssetId id,const GeometryMetadata&m)
{ Shape(m);auto g=DecodeGeometryPagePayload(b,id,m.flags);g.bounds=m.bounds;ValidateGeometryPage(m,g);return g; }
} // namespace hs::render_data
