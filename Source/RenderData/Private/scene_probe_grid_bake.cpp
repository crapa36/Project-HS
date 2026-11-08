#include <hs/render_data/scene_probe_grid_bake.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace hs::render_data
{
SceneProbeIterationState CompareSceneProbeIterations(std::span<const Float4> previous,
    std::span<const Float4> next,double absolute_tolerance,double relative_tolerance) noexcept
{
    if (previous.empty() || previous.size()!=next.size() || previous.size()%9!=0 ||
        !std::isfinite(absolute_tolerance) || absolute_tolerance<0 ||
        !std::isfinite(relative_tolerance) || relative_tolerance<0)
        return SceneProbeIterationState::Invalid;
    bool converged=true;
    for (std::size_t i=0;i<previous.size();++i)
    {
        const float a[]{previous[i].x,previous[i].y,previous[i].z};
        const float b[]{next[i].x,next[i].y,next[i].z};
        if (!std::isfinite(previous[i].w) || !std::isfinite(next[i].w) ||
            previous[i].w!=0 || next[i].w!=0) return SceneProbeIterationState::Invalid;
        for (std::size_t channel=0;channel<3;++channel)
        {
            if (!std::isfinite(a[channel]) || !std::isfinite(b[channel]) ||
                (i%9==0 && (a[channel]<0 || b[channel]<0))) return SceneProbeIterationState::Invalid;
            const double scale=std::max(std::abs(double(a[channel])),std::abs(double(b[channel])));
            if (std::abs(double(a[channel])-double(b[channel]))>absolute_tolerance+relative_tolerance*scale)
                converged=false;
        }
    }
    return converged ? SceneProbeIterationState::Converged : SceneProbeIterationState::Unconverged;
}
std::vector<SceneProbeCell> BakeSceneProbeCells(const SceneProbeDescription &description,
    std::span<const SceneProbeRecord> reflections,std::span<const float> signed_hits)
{
    const auto &g=description.grid;
    const auto [nx,ny,nz]=g.dimensions;
    const auto edge=description.edge;
    if(edge<16 || edge>128 || !std::has_single_bit(edge) ||
        nx<2 || ny<2 || nz<2 || nx>64 || ny>64 || nz>64 || g.NodeCount()>256 ||
        !(g.spacing.x>=.001f && g.spacing.y>=.001f && g.spacing.z>=.001f) ||
        !std::isfinite(g.spacing.x) || !std::isfinite(g.spacing.y) || !std::isfinite(g.spacing.z) ||
        reflections.empty() || reflections.size()>kSceneProbeMaximumCount)
        throw std::runtime_error("Invalid irradiance cell bake inputs");
    const auto face_size=edge*edge;
    if(signed_hits.size()!=std::size_t(g.NodeCount())*6*face_size)
        throw std::runtime_error("Incomplete irradiance node visibility capture");
    const std::array<float,3> substep{g.spacing.x/4,g.spacing.y/4,g.spacing.z/4};
    std::vector<SceneProbeCell> cells(g.CellCount());
    for(std::uint32_t z=0;z<nz-1;++z) for(std::uint32_t y=0;y<ny-1;++y) for(std::uint32_t x=0;x<nx-1;++x)
    {
        auto &cell=cells[x+(nx-1)*(y+(ny-1)*z)];
        cell.reachable.fill(255);
        const auto low=g.Position(x+nx*(y+ny*z));
        const auto intersects=[](float low,float high,std::uint32_t i,std::uint32_t n,float pmin,float pmax) {
            return (i==0 || pmax>=low) && (i==n-2 || pmin<=high);
        };
        for(std::size_t i=0;i<reflections.size();++i)
        {
            const auto &p=reflections[i];
            if(intersects(low.x,low.x+g.spacing.x,x,nx,p.influence_min.x,p.influence_max.x) &&
                intersects(low.y,low.y+g.spacing.y,y,ny,p.influence_min.y,p.influence_max.y) &&
                intersects(low.z,low.z+g.spacing.z,z,nz,p.influence_min.z,p.influence_max.z))
                cell.reflections[i/32]|=1u<<(i%32);
        }
    }
    for(std::uint32_t node=0;node<g.NodeCount();++node)
    {
        const auto hits=signed_hits.subspan(std::size_t(node)*6*face_size,6*face_size);
        const int px=int(node%nx),py=int((node/nx)%ny),pz=int(node/(nx*ny));
        const auto occlude=[&](int x,int y,int z) {
            const int cx=px-(x<0),cy=py-(y<0),cz=pz-(z<0);
            if(cx<0 || cy<0 || cz<0 || cx>=int(nx)-1 || cy>=int(ny)-1 || cz>=int(nz)-1) return;
            const unsigned corner=(x<0)+2*(y<0)+4*(z<0);
            auto &cell=cells[cx+(nx-1)*(cy+(ny-1)*cz)];
            cell.reachable[(x+4)%4+4*((y+4)%4)+16*((z+4)%4)]&=static_cast<std::uint8_t>(~(1u<<corner));
        };
        // A closed shell observed from within presents opaque back faces.
        // Near-contact samples are unreliable at the capture near clip.
        bool near_surface=false;
        unsigned exits=0;
        for(float hit:hits)
        {
            if(!std::isfinite(hit)) throw std::runtime_error("Nonfinite irradiance node distance");
            exits+=hit<0; near_surface|=std::abs(hit)<.08f;
        }
        if(near_surface || exits>hits.size()/4)
        {
            for(int z=-4;z<4;++z) for(int y=-4;y<4;++y) for(int x=-4;x<4;++x) occlude(x,y,z);
            continue;
        }
        // Test the center of each receiver subcell against the capture direction.
        for(int z=-4;z<4;++z) for(int y=-4;y<4;++y) for(int x=-4;x<4;++x)
        {
            const std::array<float,3> d{(x+.5f)*substep[0],(y+.5f)*substep[1],(z+.5f)*substep[2]};
            const float ax=std::abs(d[0]),ay=std::abs(d[1]),az=std::abs(d[2]);
            unsigned face; float a,b,major;
            if(ax>=ay && ax>=az)
            {
                major=ax; face=d[0]>0?0:1;
                a=(face==0?-d[2]:d[2])/major; b=-d[1]/major;
            }
            else if(ay>=az)
            {
                major=ay; face=d[1]>0?2:3;
                a=d[0]/major; b=(face==2?d[2]:-d[2])/major;
            }
            else
            {
                major=az; face=d[2]>0?4:5;
                a=(face==4?d[0]:-d[0])/major; b=-d[1]/major;
            }
            const auto u=std::min(static_cast<unsigned>((a+1)*.5f*edge),edge-1);
            const auto v=std::min(static_cast<unsigned>((b+1)*.5f*edge),edge-1);
            const float sample_a=2*(float(u)+.5f)/edge-1, sample_b=2*(float(v)+.5f)/edge-1;
            const float hit=std::abs(hits[face*face_size+v*edge+u]);
            const float hit_major=hit/std::sqrt(1+sample_a*sample_a+sample_b*sample_b);
            if(hit_major+.02f<major) occlude(x,y,z);
        }
    }
    return cells;
}
}
