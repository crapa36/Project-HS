#include <cstdint>
using uint = std::uint32_t;
#include "../Content/Shaders/hzb_region.hlsli"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>
#include <cmath>

#ifdef HS_HZB_GPU_PROBE
#include "hzb_region_gpu.hpp"
std::vector<HzbProbeInput> gpu_inputs;
std::vector<HzbProbeOutput> gpu_expected;
#endif

bool FindRegion(uint w, uint h, uint levels, uint capacity,
    uint x0, uint y0, uint x1, uint y1, HsHzbRegion& region)
{
    const bool valid = HsFindHzbRegion(w,h,levels,capacity,x0,y0,x1,y1,region);
#ifdef HS_HZB_GPU_PROBE
    gpu_inputs.push_back({w,h,levels,capacity,x0,y0,x1,y1});
    gpu_expected.push_back({valid ? 1u : 0u, valid ? region : HsHzbRegion{}});
#endif
    return valid;
}

void Check(bool ok) { if (!ok) throw std::runtime_error("HZB region contract"); }
void Case(uint w, uint h, uint x0, uint y0, uint x1, uint y1)
{
    std::array<uint,16> widths{}, heights{}, offsets{};
    uint levels=0, capacity=0;
    for (uint x=w,y=h; levels<16; x=(x+1)/2,y=(y+1)/2)
    {
        widths[levels]=x; heights[levels]=y; offsets[levels++]=capacity;
        capacity+=x*y;
        if (x==1 && y==1) break;
    }
    HsHzbRegion r{};
    Check(FindRegion(w,h,levels,capacity,x0,y0,x1,y1,r));
    Check(r.width==widths[r.level] && r.height==heights[r.level] && r.offset==offsets[r.level]);
    // Independent oracle: each selected cell owns a power-of-two base-pixel
    // interval clipped at the image border, including every odd trailing pixel.
    const std::uint64_t span=std::uint64_t{1}<<r.level;
    Check(r.min_x*span<=x0 && (r.max_x+1)*span>x1);
    Check(r.min_y*span<=y0 && (r.max_y+1)*span>y1);
    Check(r.max_x-r.min_x<=1 && r.max_y-r.min_y<=1);
    Check(r.offset+r.max_y*r.width+r.max_x<capacity);
    Check(r.min_x==x0/span && r.max_x==x1/span && r.min_y==y0/span && r.max_y==y1/span);
    if (r.level) Check((x1/(span/2)-x0/(span/2)>1) || (y1/(span/2)-y0/(span/2)>1));
    // One fewer available level must conservatively refuse a still-wide region.
    if (r.level) Check(!FindRegion(w,h,r.level,capacity,x0,y0,x1,y1,r));
}
void SphereCounterexample()
{
    constexpr uint w=1920,h=1080;
    std::vector<float> pyramid(w*h,0.8f);
    pyramid[1017*w+414]=0.0f; // Pixel-center ray intersects the supplied sphere.
    uint levels=1,offset=0,width=w,height=h,old_offset=0,old_width=0,old_height=0;
    while(width>1 || height>1)
    {
        const uint next_width=(width+1)/2,next_height=(height+1)/2;
        const uint next_offset=static_cast<uint>(pyramid.size());
        pyramid.resize(next_offset+next_width*next_height,1.0f);
        for(uint y=0;y<height;++y) for(uint x=0;x<width;++x)
        {
            auto& depth=pyramid[next_offset+(y/2)*next_width+x/2];
            depth=std::min(depth,pyramid[offset+y*width+x]);
        }
        width=next_width;height=next_height;offset=next_offset;
        if(levels==4) { old_offset=offset;old_width=width;old_height=height; }
        ++levels;
    }
    HsHzbRegion region{};
    Check(FindRegion(w,h,levels,static_cast<uint>(pyramid.size()),409,1016,418,1024,region));
    float actual=1;
    for(uint y=region.min_y;y<=region.max_y;++y)
        for(uint x=region.min_x;x<=region.max_x;++x)
            actual=std::min(actual,pyramid[region.offset+y*region.width+x]);
    // Original UV remap at mip4 missed the hole's reduction tile.
    const uint old_x=static_cast<uint>(409.9217224121094/w*old_width);
    const uint old_y=static_cast<uint>(1016.566162109375/h*old_height);
    Check(old_width==120 && old_height==68 && old_y==64);
    Check(pyramid[old_offset+old_y*old_width+old_x]>0.8f-0.0001f);
    Check(actual==0.0f); // Reversed-Z background forces visible, bias unchanged.
}

int main(int argc, char** argv)
{
    try
    {
        for(uint w=1;w<=17;++w) for(uint h=1;h<=17;++h)
            for(uint x0=0;x0<w;++x0) for(uint x1=x0;x1<w;++x1)
                Case(w,h,x0,0,x1,h-1);
        std::mt19937 random(0x485a42);
        for(auto extent : {std::array<uint,2>{17,17},{1919,1079},{1920,1080},{1,1},{3,5},{960,540}})
            for(int i=0;i<10000;++i)
            {
                auto x0=random()%extent[0],x1=random()%extent[0];
                auto y0=random()%extent[1],y1=random()%extent[1];
                Case(extent[0],extent[1],std::min(x0,x1),std::min(y0,y1),std::max(x0,x1),std::max(y0,y1));
            }
        SphereCounterexample();
        HsHzbRegion r{};
        Check(!FindRegion(0,17,5,500,0,0,0,0,r));
        Check(!FindRegion(17,0,5,500,0,0,0,0,r));
        Check(!FindRegion(17,17,0,500,0,0,1,1,r));
        Check(!FindRegion(17,17,17,500,0,0,1,1,r));
        Check(!FindRegion(17,17,5,288,0,0,1,1,r));
        Check(!FindRegion(17,17,5,500,2,0,1,1,r));
        Check(!FindRegion(17,17,5,500,0,0,17,1,r));
        Check(!FindRegion(UINT32_MAX,2,16,UINT32_MAX,0,0,1,1,r));
        Check(!FindRegion(UINT32_MAX,1,16,UINT32_MAX,0,0,UINT32_MAX-1,0,r));
#ifdef HS_HZB_GPU_PROBE
        Check(argc==2 || (argc==3 && std::string_view(argv[2])=="--warp"));
        VerifyHzbReadback(argv[1], argc==3, gpu_inputs, gpu_expected);
#else
        (void)argc; (void)argv;
#endif
        std::cout << "PASS production HZB address helper and independent ownership oracle\n";
    }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
