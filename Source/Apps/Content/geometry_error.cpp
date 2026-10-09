#include "geometry_cooker.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <vector>
namespace hs::geometry_content {
namespace {
struct V { double x{},y{},z{}; };
V operator+(V a,V b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
V operator-(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
V operator*(V a,double b){return {a.x*b,a.y*b,a.z*b};}
double Dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
double Norm2(V a){return Dot(a,a);}
double Coord(V a,int k){return k==0?a.x:(k==1?a.y:a.z);}
V Min(V a,V b){return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)};}
V Max(V a,V b){return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)};}
void Require(bool condition,const char* why){if(!condition)throw std::runtime_error(why);}
struct T { std::array<V,3> p; unsigned material{}; };
struct Box {
    V lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
    void Add(V v){lo=Min(lo,v);hi=Max(hi,v);}
    double Distance2(V p)const {
        double d=0;for(int k=0;k<3;++k){double q=Coord(p,k);double delta=std::max({Coord(lo,k)-q,0.0,q-Coord(hi,k)});d+=delta*delta;}return d;
    }
};
double SegmentDistance2(V p,V a,V b){V ab=b-a;double n=Norm2(ab);double f=n>0?std::clamp(Dot(p-a,ab)/n,0.0,1.0):0.0;return Norm2(p-(a+ab*f));}
double TriangleDistance2(V p,const T& t){
    // Closed triangle closest point, including its edges and degenerate cases.
    V a=t.p[0],b=t.p[1],c=t.p[2],ab=b-a,ac=c-a,ap=p-a;
    double d1=Dot(ab,ap),d2=Dot(ac,ap);if(d1<=0&&d2<=0)return Norm2(ap);
    V bp=p-b;double d3=Dot(ab,bp),d4=Dot(ac,bp);if(d3>=0&&d4<=d3)return Norm2(bp);
    double vc=d1*d4-d3*d2;if(vc<=0&&d1>=0&&d3<=0&&d1!=d3){double v=d1/(d1-d3);return Norm2(p-(a+ab*v));}
    V cp=p-c;double d5=Dot(ab,cp),d6=Dot(ac,cp);if(d6>=0&&d5<=d6)return Norm2(cp);
    double vb=d5*d2-d1*d6;if(vb<=0&&d2>=0&&d6<=0&&d2!=d6){double w=d2/(d2-d6);return Norm2(p-(a+ac*w));}
    double va=d3*d6-d5*d4;if(va<=0&&(d4-d3)>=0&&(d5-d6)>=0&&d4-d3+d5-d6!=0){double w=(d4-d3)/(d4-d3+d5-d6);return Norm2(p-(b+(c-b)*w));}
    double denom=va+vb+vc;
    if(denom>0){double v=vb/denom,w=vc/denom;return Norm2(p-(a+ab*v+ac*w));}
    return std::min({SegmentDistance2(p,a,b),SegmentDistance2(p,b,c),SegmentDistance2(p,c,a)});
}
struct Node { Box box; unsigned begin{},end{},left{},right{}; bool leaf{}; };
struct Bvh {
    std::vector<T> triangles;std::vector<Node> nodes;unsigned root{};
    explicit Bvh(std::vector<T> source):triangles(std::move(source)){Require(!triangles.empty(),"empty target material surface");root=Build(0,static_cast<unsigned>(triangles.size()));}
    unsigned Build(unsigned begin,unsigned end){
        unsigned id=static_cast<unsigned>(nodes.size());nodes.emplace_back();Node n;n.begin=begin;n.end=end;
        Box centers;for(unsigned i=begin;i<end;++i){for(V p:triangles[i].p)n.box.Add(p);centers.Add((triangles[i].p[0]+triangles[i].p[1]+triangles[i].p[2])*(1.0/3));}
        if(end-begin<=8)n.leaf=true;
        else {V extent=centers.hi-centers.lo;int axis=extent.y>extent.x?1:0;if(extent.z>Coord(extent,axis))axis=2;unsigned middle=(begin+end)/2;
            std::nth_element(triangles.begin()+begin,triangles.begin()+middle,triangles.begin()+end,[axis](const T& a,const T& b){return Coord(a.p[0]+a.p[1]+a.p[2],axis)<Coord(b.p[0]+b.p[1]+b.p[2],axis);});
            n.left=Build(begin,middle);n.right=Build(middle,end);}
        nodes[id]=n;return id;
    }
    double Distance(V p)const{
        double best=INFINITY;std::vector<unsigned> stack{root};
        while(!stack.empty()){unsigned id=stack.back();stack.pop_back();const auto& n=nodes[id];if(n.box.Distance2(p)>best)continue;
            if(n.leaf){for(unsigned i=n.begin;i<n.end;++i)best=std::min(best,TriangleDistance2(p,triangles[i]));}
            else {double l=nodes[n.left].box.Distance2(p),r=nodes[n.right].box.Distance2(p);if(l<r){if(r<=best)stack.push_back(n.right);if(l<=best)stack.push_back(n.left);}else{if(l<=best)stack.push_back(n.left);if(r<=best)stack.push_back(n.right);}}
        }return std::sqrt(best);
    }
};
struct Patch {T t;double upper{};std::uint64_t serial{};};
struct Highest { bool operator()(const Patch& a,const Patch& b)const{return a.upper==b.upper?a.serial>b.serial:a.upper<b.upper;} };
using SurfaceKey=std::array<double,10>;
SurfaceKey Key(const T& t){auto p=t.p;std::sort(p.begin(),p.end(),[](V a,V b){return std::array<double,3>{a.x,a.y,a.z}<std::array<double,3>{b.x,b.y,b.z};});return {double(t.material),p[0].x,p[0].y,p[0].z,p[1].x,p[1].y,p[1].z,p[2].x,p[2].y,p[2].z};}
GeometryErrorBounds Directed(const std::vector<T>& source,const std::vector<T>& target,double tolerance,std::uint64_t maxQueries){
    std::map<unsigned,std::vector<T>> grouped;std::map<SurfaceKey,bool> identical;double coordinateScale=1;
    for(auto t:target){grouped[t.material].push_back(t);identical.emplace(Key(t),true);for(V p:t.p)coordinateScale=std::max({coordinateScale,std::abs(p.x),std::abs(p.y),std::abs(p.z)});}
    for(auto t:source)for(V p:t.p)coordinateScale=std::max({coordinateScale,std::abs(p.x),std::abs(p.y),std::abs(p.z)});
    // Practical double-precision allowance, separate from the geometric proof.
    // This is deliberately much larger than one double ULP for these fixtures.
    const double numericMargin=coordinateScale*1e-9;
    std::map<unsigned,Bvh> surfaces;for(auto& [material,triangles]:grouped)surfaces.emplace(material,Bvh(std::move(triangles)));
    unsigned bvhChecks=0;
    for(std::size_t i=0;i<source.size()&&bvhChecks<12;i+=std::max<std::size_t>(1,source.size()/12)){
        const T& t=source[i];auto found=surfaces.find(t.material);Require(found!=surfaces.end(),"missing target material");V p=(t.p[0]+t.p[1]+t.p[2])*(1.0/3);double exhaustive=INFINITY;
        for(const auto& candidate:found->second.triangles)exhaustive=std::min(exhaustive,TriangleDistance2(p,candidate));
        Require(std::abs(found->second.Distance(p)-std::sqrt(exhaustive))<=numericMargin,"BVH closest distance differs from exhaustive oracle");++bvhChecks;
    }
    std::priority_queue<Patch,std::vector<Patch>,Highest> patches;
    std::uint64_t serial=0,queries=0;double lower=0;
    auto insert=[&](T t,double parentUpper){auto found=surfaces.find(t.material);Require(found!=surfaces.end(),"authored LOD removes a complete material surface");V center=(t.p[0]+t.p[1]+t.p[2])*(1.0/3);double distance=found->second.Distance(center);++queries;lower=std::max(lower,std::max(0.0,distance-numericMargin));double radius=std::sqrt(std::max({Norm2(t.p[0]-center),Norm2(t.p[1]-center),Norm2(t.p[2]-center)}));
        // The distance to any closed target set is 1-Lipschitz. Every point of
        // this source triangle lies inside the centroid sphere, so d(q)+r is
        // an upper bound for the complete triangle, not just sampled vertices.
        double upper=std::min(parentUpper,std::nextafter(distance+radius+numericMargin,INFINITY));patches.push({t,upper,serial++});};
    for(auto t:source)if(!identical.contains(Key(t)))insert(t,INFINITY);
    // Every source face needs initial coverage; a refinement budget cannot
    // invalidate large but otherwise valid surfaces before that coverage exists.
    const auto initialQueries=queries;
    while(!patches.empty()&&patches.top().upper-lower>tolerance&&queries-initialQueries+4<=maxQueries){auto p=patches.top();patches.pop();V a=p.t.p[0],b=p.t.p[1],c=p.t.p[2],ab=(a+b)*0.5,bc=(b+c)*0.5,ca=(c+a)*0.5;
        insert(T{{a,ab,ca},p.t.material},p.upper);insert(T{{ab,b,bc},p.t.material},p.upper);insert(T{{ca,bc,c},p.t.material},p.upper);insert(T{{ab,bc,ca},p.t.material},p.upper);}
    double upper=patches.empty()?0.0:std::max(lower,patches.top().upper);
    return {lower,upper};
}
std::vector<T> Surface(std::span<const render_data::ExpandedVertex> vertices,
                       std::span<const std::uint16_t> material_mapping) {
    Require(!vertices.empty() && vertices.size()%3==0,"invalid error surface");
    std::vector<T> out;
    for(std::size_t i=0;i<vertices.size();i+=3){
        const auto source_material=vertices[i].material_index;
        T t;
        for(std::size_t k=0;k<3;++k){Require(vertices[i+k].material_index==source_material,"mixed error surface material");const auto p=vertices[i+k].position;Require(std::isfinite(p[0])&&std::isfinite(p[1])&&std::isfinite(p[2]),"nonfinite error surface");t.p[k]={p[0],p[1],p[2]};}
        if(material_mapping.empty())t.material=source_material;
        else{Require(source_material<material_mapping.size(),"error surface material mapping index out of range");t.material=material_mapping[source_material];}
        out.push_back(t);
    }return out;
}
} // namespace
GeometryErrorBounds MeasureGeometryError(std::span<const render_data::ExpandedVertex> fine,std::span<const render_data::ExpandedVertex> coarse,std::span<const std::uint16_t> material_mapping) {
    const auto a=Surface(fine,material_mapping),b=Surface(coarse,material_mapping);Box box;for(const auto&t:a)for(auto p:t.p)box.Add(p);const double tolerance=std::sqrt(Norm2(box.hi-box.lo))*0.005;
    const auto forward=Directed(a,b,tolerance,250000),reverse=Directed(b,a,tolerance,250000);return {std::max(forward.lower,reverse.lower),std::max(forward.upper,reverse.upper)};
}
} // namespace hs::geometry_content
