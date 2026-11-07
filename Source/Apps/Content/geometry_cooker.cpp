#include "geometry_cooker.hpp"
#include <meshoptimizer.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
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

struct WeldKey
{
    std::array<std::uint32_t, 3> position{};
    std::array<std::uint16_t, 4> bones{};
    std::array<std::uint32_t, 4> weights{};
    friend auto operator<=>(const WeldKey &, const WeldKey &) = default;
};

struct EdgeUse { std::uint32_t triangle{}, from{}, to{}; };
using Edge = std::pair<std::uint32_t, std::uint32_t>;

bool ClosedManifold(const rd::GeometryData &geometry,
                    const std::vector<std::array<std::uint32_t, 3>> &triangles,
                    const std::map<Edge, std::vector<EdgeUse>> &edge_uses,
                    std::uint32_t component,
                    std::span<const std::uint32_t> triangle_components)
{
    std::set<std::array<std::uint32_t, 3>> faces;
    double signed_volume6{};
    std::optional<std::array<double, 3>> volume_origin;
    std::array<double, 3> minimum{
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity()};
    std::array<double, 3> maximum{
        -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    for (std::uint32_t triangle = 0; triangle < triangles.size(); ++triangle)
    {
        if (triangle_components[triangle] != component) continue;
        const auto ids = triangles[triangle];
        auto face = ids;
        std::ranges::sort(face);
        if (!faces.insert(face).second) return false;
        const auto &a = geometry.vertices[geometry.indices[std::size_t(triangle) * 3 + 0]].position;
        const auto &b = geometry.vertices[geometry.indices[std::size_t(triangle) * 3 + 1]].position;
        const auto &c = geometry.vertices[geometry.indices[std::size_t(triangle) * 3 + 2]].position;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = std::min({minimum[axis], double(a[axis]), double(b[axis]), double(c[axis])});
            maximum[axis] = std::max({maximum[axis], double(a[axis]), double(b[axis]), double(c[axis])});
        }
        if (!volume_origin)
            volume_origin = std::array<double,3>{a[0],a[1],a[2]};
        const auto &origin = *volume_origin;
        const std::array<double, 3> ar{double(a[0])-origin[0],double(a[1])-origin[1],double(a[2])-origin[2]};
        const std::array<double, 3> br{double(b[0])-origin[0],double(b[1])-origin[1],double(b[2])-origin[2]};
        const std::array<double, 3> cr{double(c[0])-origin[0],double(c[1])-origin[1],double(c[2])-origin[2]};
        const std::array<double, 3> ab{double(b[0])-a[0], double(b[1])-a[1], double(b[2])-a[2]};
        const std::array<double, 3> ac{double(c[0])-a[0], double(c[1])-a[1], double(c[2])-a[2]};
        const std::array<double, 3> normal{ab[1]*ac[2]-ab[2]*ac[1],
            ab[2]*ac[0]-ab[0]*ac[2], ab[0]*ac[1]-ab[1]*ac[0]};
        const double area2 = normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2];
        if (!(area2 > 0.0) || !std::isfinite(area2)) return false;
        const std::array<double, 3> cross{br[1]*cr[2]-br[2]*cr[1],
            br[2]*cr[0]-br[0]*cr[2], br[0]*cr[1]-br[1]*cr[0]};
        signed_volume6 += ar[0]*cross[0] + ar[1]*cross[1] + ar[2]*cross[2];
    }
    const double extent = std::max({maximum[0]-minimum[0], maximum[1]-minimum[1],
        maximum[2]-minimum[2], 1e-20});
    if (!std::isfinite(signed_volume6) || std::abs(signed_volume6) <= 1e-14 * extent * extent * extent)
        return false;

    for (const auto &[edge, uses] : edge_uses)
    {
        (void)edge;
        std::array<EdgeUse, 2> local{};
        std::size_t count{};
        for (const auto &use : uses)
            if (triangle_components[use.triangle] == component)
            {
                if (count == local.size()) return false;
                local[count++] = use;
            }
        if (count && (count != 2 || local[0].from != local[1].to || local[0].to != local[1].from))
            return false;
    }
    return true;
}

void CertifyOpticalComponents(rd::GeometryData &geometry,
                              std::span<const OpticalMaterialProfile> material_profiles)
{
    for (auto &vertex : geometry.vertices) vertex.optical_component_id = 0;
    if (material_profiles.empty() || geometry.indices.empty() ||
        std::ranges::none_of(material_profiles, [](const OpticalMaterialProfile &profile) {
            return profile.solid_transmission && profile.compatibility_group != 0;
        })) return;

    std::map<WeldKey, std::uint32_t> welded;
    std::vector<std::array<std::uint32_t, 3>> triangles(geometry.indices.size() / 3);
    std::vector<std::vector<std::uint32_t>> vertex_triangles;
    for (std::size_t triangle = 0; triangle < triangles.size(); ++triangle)
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
            const auto source_index = geometry.indices[triangle * 3 + corner];
            const auto &vertex = geometry.vertices[source_index];
            WeldKey key;
            for (std::size_t axis = 0; axis < 3; ++axis)
                key.position[axis] = vertex.position[axis] == 0.0f ? 0u :
                    std::bit_cast<std::uint32_t>(vertex.position[axis]);
            key.bones = vertex.bone_indices;
            for (std::size_t lane = 0; lane < 4; ++lane)
                key.weights[lane] = vertex.bone_weights[lane] == 0.0f ? 0u :
                    std::bit_cast<std::uint32_t>(vertex.bone_weights[lane]);
            const auto [entry, inserted] = welded.try_emplace(key,
                static_cast<std::uint32_t>(welded.size()));
            if (inserted) vertex_triangles.emplace_back();
            const auto id = entry->second;
            triangles[triangle][corner] = id;
            vertex_triangles[id].push_back(static_cast<std::uint32_t>(triangle));
        }

    std::map<Edge, std::vector<EdgeUse>> edge_map;
    std::set<std::array<std::uint32_t, 3>> unique_faces;
    std::vector<bool> duplicate_face(triangles.size());
    for (std::uint32_t triangle = 0; triangle < triangles.size(); ++triangle)
    {
        const auto &t = triangles[triangle];
        auto sorted = t;
        std::ranges::sort(sorted);
        if (sorted[0] == sorted[1] || sorted[1] == sorted[2] || !unique_faces.insert(sorted).second)
            duplicate_face[triangle] = true;
        for (std::size_t edge = 0; edge < 3; ++edge)
        {
            const auto from = t[edge], to = t[(edge + 1) % 3];
            edge_map[{std::min(from, to), std::max(from, to)}].push_back({triangle, from, to});
        }
    }

    std::vector<std::vector<std::uint32_t>> adjacency(triangles.size());
    for (const auto &[edge, uses] : edge_map)
    {
        (void)edge;
        if (uses.size() > 1)
            for (std::size_t index = 1; index < uses.size(); ++index)
            {
                adjacency[uses.front().triangle].push_back(uses[index].triangle);
                adjacency[uses[index].triangle].push_back(uses.front().triangle);
            }
    }
    std::vector<std::uint32_t> components(triangles.size(), 0xffffffffu);
    std::uint32_t component_count{};
    for (std::uint32_t seed = 0; seed < triangles.size(); ++seed)
    {
        if (components[seed] != 0xffffffffu) continue;
        std::vector<std::uint32_t> pending{seed};
        components[seed] = component_count;
        while (!pending.empty())
        {
            const auto current = pending.back(); pending.pop_back();
            for (const auto adjacent : adjacency[current])
                if (components[adjacent] == 0xffffffffu)
                { components[adjacent] = component_count; pending.push_back(adjacent); }
        }
        ++component_count;
    }
    std::vector<bool> closed(component_count, true);
    for (std::uint32_t triangle = 0; triangle < triangles.size(); ++triangle)
        if (duplicate_face[triangle]) closed[components[triangle]] = false;
    for (std::uint32_t component = 0; component < component_count; ++component)
        closed[component] = closed[component] && ClosedManifold(
            geometry, triangles, edge_map, component, components);

    // A vertex fan shared by separate shells is non-manifold even when each
    // edge-connected shell has its own closed edge count.
    for (std::uint32_t vertex = 0; vertex < vertex_triangles.size(); ++vertex)
    {
        std::map<std::uint32_t, std::vector<std::uint32_t>> link;
        for (const auto triangle : vertex_triangles[vertex])
        {
            const auto &t = triangles[triangle];
            std::array<std::uint32_t, 2> other{};
            std::size_t found{};
            for (const auto item : t) if (item != vertex && found < other.size()) other[found++] = item;
            if (found != 2 || other[0] == other[1])
            {
                closed[components[triangle]] = false;
                continue;
            }
            link[other[0]].push_back(other[1]);
            link[other[1]].push_back(other[0]);
        }
        bool manifold = !link.empty();
        for (const auto &[neighbor, adjacent] : link)
        { (void)neighbor; manifold = manifold && adjacent.size() == 2; }
        if (manifold)
        {
            std::set<std::uint32_t> reached;
            std::vector<std::uint32_t> pending{link.begin()->first};
            reached.insert(pending.front());
            while (!pending.empty())
            {
                const auto current = pending.back(); pending.pop_back();
                for (const auto next : link[current])
                    if (reached.insert(next).second) pending.push_back(next);
            }
            manifold = reached.size() == link.size();
        }
        if (!manifold)
            for (const auto triangle : vertex_triangles[vertex])
                closed[components[triangle]] = false;
    }

    using Pair = std::pair<std::uint32_t, std::uint32_t>; // component, compatible profile
    std::vector<std::uint32_t> component_profiles(component_count);
    std::vector<bool> profile_valid(component_count, true);
    for (std::uint32_t triangle = 0; triangle < triangles.size(); ++triangle)
    {
        if (!closed[components[triangle]]) continue;
        const auto vertex_index = geometry.indices[std::size_t(triangle) * 3];
        const auto material = geometry.vertices[vertex_index].material_index;
        if (material >= material_profiles.size() ||
            !material_profiles[material].solid_transmission ||
            !material_profiles[material].compatibility_group)
        {
            profile_valid[components[triangle]] = false;
            continue;
        }
        auto &group = component_profiles[components[triangle]];
        if (group && group != material_profiles[material].compatibility_group)
            profile_valid[components[triangle]] = false;
        else
            group = material_profiles[material].compatibility_group;
    }
    std::vector<std::optional<Pair>> triangle_pairs(triangles.size());
    std::set<Pair> unique_pairs;
    for (std::uint32_t triangle = 0; triangle < triangles.size(); ++triangle)
    {
        const auto component = components[triangle];
        if (!closed[component] || !profile_valid[component] || !component_profiles[component]) continue;
        triangle_pairs[triangle] = Pair{component, component_profiles[component]};
        unique_pairs.insert(*triangle_pairs[triangle]);
    }
    if (unique_pairs.size() > std::numeric_limits<std::uint16_t>::max()) return;
    std::map<Pair, std::uint16_t> ids;
    std::uint32_t next_id = 1;
    for (const auto &pair : unique_pairs) ids.emplace(pair, static_cast<std::uint16_t>(next_id++));
    std::vector<std::uint16_t> vertex_ids(geometry.vertices.size());
    for (std::uint32_t triangle = 0; triangle < triangles.size(); ++triangle)
    {
        if (!triangle_pairs[triangle]) continue;
        const auto id = ids.at(*triangle_pairs[triangle]);
        for (std::size_t corner = 0; corner < 3; ++corner)
        {
            const auto vertex = geometry.indices[std::size_t(triangle) * 3 + corner];
            if (vertex_ids[vertex] && vertex_ids[vertex] != id)
            { std::fill(vertex_ids.begin(), vertex_ids.end(), 0); return; }
            vertex_ids[vertex] = id;
        }
    }
    for (std::size_t vertex = 0; vertex < geometry.vertices.size(); ++vertex)
        geometry.vertices[vertex].optical_component_id = vertex_ids[vertex];
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
        {&first.optical_component_id, sizeof(first.optical_component_id), stride}}};
    std::vector<unsigned> remap(source.size());
    const auto count = meshopt_generateVertexRemapMulti(remap.data(), nullptr, source.size(),
        source.size(), streams.data(), streams.size());
    vertices.resize(count); indices.resize(source.size());
    meshopt_remapVertexBuffer(vertices.data(), source.data(), source.size(), stride, remap.data());
    meshopt_remapIndexBuffer(indices.data(), nullptr, source.size(), remap.data());
}
render_data::GeometryData CookGeometry(std::span<const rd::ExpandedVertex>source,std::uint32_t flags,
    std::span<const OpticalMaterialProfile> optical_profiles)
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
    CertifyOpticalComponents(g, optical_profiles);
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
