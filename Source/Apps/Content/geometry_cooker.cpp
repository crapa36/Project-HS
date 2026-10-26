#include "geometry_cooker.hpp"
#include <meshoptimizer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
namespace hs::geometry_content
{
namespace
{
namespace rd=render_data;
void Require(bool b,const char* why){if(!b)throw std::runtime_error(why);}
std::array<float,4> Sphere(std::span<const std::array<float,3>> p)
{
    auto b=meshopt_computeSphereBounds(p.front().data(),p.size(),sizeof(p.front()),nullptr,0);
    std::array<float,4>s{b.center[0],b.center[1],b.center[2],b.radius};double radius=b.radius;
    for(auto v:p){double d=0;for(std::size_t k=0;k<3;++k){double q=double(v[k])-s[k];d+=q*q;}radius=std::max(radius,std::sqrt(d));}
    s[3]=std::nextafter(static_cast<float>(radius),std::numeric_limits<float>::infinity());return s;
}
}
void IndexVertices(std::span<const rd::ExpandedVertex> source,
                   std::vector<rd::ExpandedVertex> &vertices,
                   std::vector<unsigned> &indices)
{
    Require(!source.empty() && source.size() % 3 == 0 && source.size() <= rd::kMaxGeometryIndices,
            "invalid expanded geometry input");
    rd::ValidateExpandedVertices(source);
    static_assert(std::is_trivially_copyable_v<rd::ExpandedVertex>);
    static_assert(sizeof(rd::ExpandedVertex) <= 256);
    const auto &first = source.front();
    constexpr auto stride = sizeof(rd::ExpandedVertex);
    // Streams compare every field, excluding C++ padding; serialization remains portable.
    const std::array<meshopt_Stream, 8> streams{{
        {first.position.data(), 3 * sizeof(float), stride},
        {first.normal.data(), 3 * sizeof(float), stride},
        {first.bone_indices.data(), 4 * sizeof(std::uint16_t), stride},
        {first.bone_weights.data(), 4 * sizeof(float), stride},
        {first.uv.data(), 2 * sizeof(float), stride},
        {first.tangent.data(), 4 * sizeof(float), stride},
        {&first.material_index, sizeof(first.material_index), stride},
        {&first.reserved, sizeof(first.reserved), stride}}};
    std::vector<unsigned> remap(source.size());
    const auto count = meshopt_generateVertexRemapMulti(remap.data(), nullptr, source.size(),
        source.size(), streams.data(), streams.size());
    vertices.resize(count); indices.resize(source.size());
    meshopt_remapVertexBuffer(vertices.data(), source.data(), source.size(), stride, remap.data());
    meshopt_remapIndexBuffer(indices.data(), nullptr, source.size(), remap.data());
}
render_data::GeometryData CookGeometry(std::span<const rd::ExpandedVertex>source,std::uint32_t flags)
{
    Require(!source.empty() && source.size()%3==0 && source.size()<=rd::kMaxGeometryIndices && !(flags&~7u),"invalid expanded geometry input");
    rd::GeometryData g;g.flags=flags;IndexVertices(source,g.vertices,g.indices);
    std::vector<std::array<float,3>>positions;for(const auto&v:g.vertices)positions.push_back(v.position);
    g.bounds.minimum=g.bounds.maximum=positions.front();for(auto p:positions)for(std::size_t k=0;k<3;++k){g.bounds.minimum[k]=std::min(g.bounds.minimum[k],p[k]);g.bounds.maximum[k]=std::max(g.bounds.maximum[k],p[k]);}g.bounds.sphere=Sphere(positions);
    using Triangle=std::array<std::uint32_t,3>;
    for(std::size_t first=0;first<g.indices.size();)
    {
        const auto material=g.vertices[g.indices[first]].material_index;std::size_t end=first;
        while(end<g.indices.size() && g.vertices[g.indices[end]].material_index==material)
        {
            for(std::size_t k=0;k<3;++k)Require(g.vertices[g.indices[end+k]].material_index==material,"mixed authored triangle material");end+=3;
        }
        rd::GeometrySubmesh run{static_cast<std::uint32_t>(first),static_cast<std::uint32_t>(end-first),static_cast<std::uint32_t>(g.meshlets.size()),0,material};
        const auto occurrence_count = static_cast<std::uint32_t>((end - first) / 3);
        std::vector<std::uint32_t> next_occurrence(occurrence_count);
        std::map<Triangle, std::uint32_t> occurrences;
        // Prepend in reverse authored order to retain FIFO without a deque per triangle.
        for (auto local = occurrence_count; local > 0;)
        {
            --local;
            const auto t = first + static_cast<std::size_t>(local) * 3;
            auto [entry, inserted] = occurrences.try_emplace(
                Triangle{g.indices[t], g.indices[t + 1], g.indices[t + 2]}, occurrence_count);
            (void)inserted;
            next_occurrence[local] = entry->second;
            entry->second = local;
        }
        std::vector<meshopt_Meshlet>native(meshopt_buildMeshletsBound(end-first,64,124));std::vector<unsigned>refs(end-first);std::vector<unsigned char>triangles(end-first);
        native.resize(meshopt_buildMeshlets(native.data(),refs.data(),triangles.data(),g.indices.data()+first,end-first,positions.front().data(),positions.size(),12,64,124,0));
        for(const auto&m:native)
        {
            rd::GeometryMeshlet cl;cl.first_vertex_ref=static_cast<std::uint32_t>(g.meshlet_vertex_refs.size());cl.vertex_ref_count=m.vertex_count;cl.first_triangle=static_cast<std::uint32_t>(g.meshlet_triangles.size());cl.triangle_count=m.triangle_count;cl.submesh_index=static_cast<std::uint32_t>(g.submeshes.size());
            std::vector<std::array<float,3>>referenced;for(unsigned k=0;k<m.vertex_count;++k){auto v=refs[m.vertex_offset+k];g.meshlet_vertex_refs.push_back(v);referenced.push_back(positions[v]);}cl.sphere=Sphere(referenced);bool degenerate=false;
            for(unsigned k=0;k<m.triangle_count;++k)
            {
                const auto o=m.triangle_offset+k*3;const unsigned a=triangles[o],b=triangles[o+1],c=triangles[o+2];g.meshlet_triangles.push_back(a|(b<<8)|(c<<16));Triangle key{refs[m.vertex_offset+a],refs[m.vertex_offset+b],refs[m.vertex_offset+c]};auto it=occurrences.find(key);Require(it!=occurrences.end() && it->second!=occurrence_count,"meshlet lost oriented primitive occurrence");
                g.canonical_primitive_remap.push_back(static_cast<std::uint32_t>(first / 3) + it->second);
                it->second = next_occurrence[it->second];
                const auto p=positions[key[0]],q=positions[key[1]],r=positions[key[2]];std::array<double,3>x{},y{};for(std::size_t z=0;z<3;++z){x[z]=double(q[z])-p[z];y[z]=double(r[z])-p[z];}const std::array cross{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};degenerate|=cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2]==0;
            }
            if(flags==0 && !degenerate)
            {
                const auto b=meshopt_computeMeshletBounds(refs.data()+m.vertex_offset,triangles.data()+m.triangle_offset,m.triangle_count,positions.front().data(),positions.size(),12);
                const auto cutoff=std::nextafter(b.cone_cutoff,std::numeric_limits<float>::infinity());if(cutoff>=0 && cutoff<1){cl.flags=rd::kGeometryMeshletConeValid;cl.cone={b.cone_axis[0],b.cone_axis[1],b.cone_axis[2],cutoff};}
            }
            g.meshlets.push_back(cl);
        }
        for(const auto&[key,head]:occurrences){(void)key;Require(head==occurrence_count,"unconsumed authored primitive");}run.meshlet_count=static_cast<std::uint32_t>(g.meshlets.size())-run.first_meshlet;g.submeshes.push_back(run);first=end;
    }
    rd::ValidateGeometryData(g);return g;
}

std::vector<rd::ExpandedVertex> SimplifySkinnedGeometry(const rd::GeometryData &fine,float absolute_error)
{
    Require((fine.flags&rd::kGeometryFlagSkinned)!=0 && std::isfinite(absolute_error) && absolute_error>0,
        "invalid skinned simplification budget");
    // Bone IDs are categorical. The simplifier returns only indices into the
    // original buffer; survivors keep exact weights, UVs, normals and tangents.
    using Attributes=std::array<float,9>;
    std::vector<Attributes> attributes;attributes.reserve(fine.vertices.size());
    for(const auto &vertex:fine.vertices)
        attributes.push_back({vertex.normal[0],vertex.normal[1],vertex.normal[2],
            vertex.uv[0],vertex.uv[1],vertex.tangent[0],vertex.tangent[1],vertex.tangent[2],vertex.tangent[3]});
    const float radius=std::max(fine.bounds.sphere[3],0.0001f);
    const std::array<float,9> weights{radius*0.25f,radius*0.25f,radius*0.25f,
        radius,radius,radius*0.125f,radius*0.125f,radius*0.125f,radius*0.125f};
    std::vector<rd::ExpandedVertex> output;output.reserve(fine.indices.size());
    for(const auto &submesh:fine.submeshes)
    {
        std::vector<unsigned> simplified(submesh.index_count);
        const auto count=meshopt_simplifyWithAttributes(simplified.data(),
            fine.indices.data()+submesh.first_index,submesh.index_count,
            fine.vertices.front().position.data(),fine.vertices.size(),sizeof(fine.vertices.front()),
            attributes.front().data(),sizeof(attributes.front()),weights.data(),weights.size(),nullptr,
            3,absolute_error,meshopt_SimplifyErrorAbsolute|meshopt_SimplifyRegularize,nullptr);
        Require(count>0 && count<=submesh.index_count && count%3==0,"invalid skinned simplifier output");
        for(std::size_t i=0;i<count;++i) output.push_back(fine.vertices[simplified[i]]);
    }
    return output;
}
}
