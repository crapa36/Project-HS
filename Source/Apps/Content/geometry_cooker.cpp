#include "geometry_cooker.hpp"
#include <meshoptimizer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
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
render_data::GeometryData CookGeometry(std::span<const rd::ExpandedVertex>source,std::uint32_t flags)
{
    Require(!source.empty() && source.size()%3==0 && source.size()<=rd::kMaxGeometryIndices && !(flags&~7u),"invalid expanded geometry input");
    const auto encoded=rd::EncodeExpandedVertices(source); // exact complete portable 76-byte record
    std::vector<unsigned>remap(source.size());const auto count=meshopt_generateVertexRemap(remap.data(),nullptr,source.size(),encoded.data(),source.size(),76);
    std::vector<std::byte>unique(count*76);rd::GeometryData g;g.flags=flags;g.indices.resize(source.size());
    meshopt_remapVertexBuffer(unique.data(),encoded.data(),source.size(),76,remap.data());meshopt_remapIndexBuffer(g.indices.data(),nullptr,source.size(),remap.data());g.vertices=rd::DecodeExpandedVertices(unique);
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
        std::map<Triangle,std::deque<std::uint32_t>>occurrences;for(std::size_t t=first;t<end;t+=3)occurrences[{g.indices[t],g.indices[t+1],g.indices[t+2]}].push_back(static_cast<std::uint32_t>(t/3));
        std::vector<meshopt_Meshlet>native(meshopt_buildMeshletsBound(end-first,64,124));std::vector<unsigned>refs(end-first);std::vector<unsigned char>triangles(end-first);
        native.resize(meshopt_buildMeshlets(native.data(),refs.data(),triangles.data(),g.indices.data()+first,end-first,positions.front().data(),positions.size(),12,64,124,0));
        for(const auto&m:native)
        {
            rd::GeometryMeshlet cl;cl.first_vertex_ref=static_cast<std::uint32_t>(g.meshlet_vertex_refs.size());cl.vertex_ref_count=m.vertex_count;cl.first_triangle=static_cast<std::uint32_t>(g.meshlet_triangles.size());cl.triangle_count=m.triangle_count;cl.submesh_index=static_cast<std::uint32_t>(g.submeshes.size());
            std::vector<std::array<float,3>>referenced;for(unsigned k=0;k<m.vertex_count;++k){auto v=refs[m.vertex_offset+k];g.meshlet_vertex_refs.push_back(v);referenced.push_back(positions[v]);}cl.sphere=Sphere(referenced);bool degenerate=false;
            for(unsigned k=0;k<m.triangle_count;++k)
            {
                const auto o=m.triangle_offset+k*3;const unsigned a=triangles[o],b=triangles[o+1],c=triangles[o+2];g.meshlet_triangles.push_back(a|(b<<8)|(c<<16));Triangle key{refs[m.vertex_offset+a],refs[m.vertex_offset+b],refs[m.vertex_offset+c]};auto it=occurrences.find(key);Require(it!=occurrences.end() && !it->second.empty(),"meshlet lost oriented primitive occurrence");g.canonical_primitive_remap.push_back(it->second.front());it->second.pop_front();
                const auto p=positions[key[0]],q=positions[key[1]],r=positions[key[2]];std::array<double,3>x{},y{};for(std::size_t z=0;z<3;++z){x[z]=double(q[z])-p[z];y[z]=double(r[z])-p[z];}const std::array cross{x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]};degenerate|=cross[0]*cross[0]+cross[1]*cross[1]+cross[2]*cross[2]==0;
            }
            if(flags==0 && !degenerate)
            {
                const auto b=meshopt_computeMeshletBounds(refs.data()+m.vertex_offset,triangles.data()+m.triangle_offset,m.triangle_count,positions.front().data(),positions.size(),12);
                const auto cutoff=std::nextafter(b.cone_cutoff,std::numeric_limits<float>::infinity());if(cutoff>=0 && cutoff<1){cl.flags=rd::kGeometryMeshletConeValid;cl.cone={b.cone_axis[0],b.cone_axis[1],b.cone_axis[2],cutoff};}
            }
            g.meshlets.push_back(cl);
        }
        for(const auto&[key,q]:occurrences){(void)key;Require(q.empty(),"unconsumed authored primitive");}run.meshlet_count=static_cast<std::uint32_t>(g.meshlets.size())-run.first_meshlet;g.submeshes.push_back(run);first=end;
    }
    rd::ValidateGeometryData(g);return g;
}
}
