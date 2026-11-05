#include <hs/render_data/scene_probe_grid_bake.hpp>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <stdexcept>

namespace hs::render_data
{
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
        // Voxelize the shadow behind each captured hit through adjacent cells.
        // No depth or visibility textures are consulted during diffuse sampling.
        for(unsigned f=0;f<6;++f) for(unsigned v=0;v<edge;++v) for(unsigned u=0;u<edge;++u)
        {
            const float hit=std::abs(hits[f*face_size+v*edge+u]);
            const float a=2*(float(u)+.5f)/edge-1,b=2*(float(v)+.5f)/edge-1;
            const std::array<std::array<float,3>,6> faces{{{1,-b,-a},{-1,-b,a},{a,1,b},{a,-1,-b},{a,-b,1},{-a,-b,-1}}};
            const auto &d=faces[f];
            const float norm=std::sqrt(1+a*a+b*b),start=std::max(0.f,hit-.02f);
            std::array<int,3> voxel{},step{};
            std::array<float,3> next{},delta{};
            for(unsigned axis=0;axis<3;++axis)
            {
                const float rate=d[axis]/(norm*substep[axis]);
                const float position=start*rate;
                voxel[axis]=static_cast<int>(std::floor(std::clamp(position,-5.f,5.f)));
                step[axis]=rate>0?1:-1;
                delta[axis]=rate!=0?1/std::abs(rate):FLT_MAX;
                next[axis]=rate!=0?(float(voxel[axis]+(rate>0))-position)/rate:FLT_MAX;
            }
            while(voxel[0]>=-4 && voxel[0]<4 && voxel[1]>=-4 && voxel[1]<4 && voxel[2]>=-4 && voxel[2]<4)
            {
                occlude(voxel[0],voxel[1],voxel[2]);
                const auto axis=std::min_element(next.begin(),next.end())-next.begin();
                voxel[axis]+=step[axis]; next[axis]+=delta[axis];
            }
        }
    }
    return cells;
}
}
