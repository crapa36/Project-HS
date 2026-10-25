#include "geometry_cooker.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <vector>
namespace hs::geometry_content {
GeometryErrorQualificationKey GeometryErrorImplementationKey() noexcept
{
    GeometryErrorQualificationKey key{1, 1, render_data::kGeometryAbi, 1};
#if defined(__clang__)
    key.compiler=2; key.compiler_version=(std::uint64_t(__clang_major__)<<32u)|(__clang_minor__<<16u)|__clang_patchlevel__;
#elif defined(_MSC_FULL_VER)
    key.compiler=1; key.compiler_version=_MSC_FULL_VER;
#if defined(_MSC_BUILD)
    key.compiler_version=(key.compiler_version<<16u)|_MSC_BUILD;
#endif
#elif defined(__GNUC__)
    key.compiler=3; key.compiler_version=(std::uint64_t(__GNUC__)<<32u)|(__GNUC_MINOR__<<16u)|__GNUC_PATCHLEVEL__;
#endif
#if defined(_M_X64) || defined(__x86_64__)
    key.target=1;
#elif defined(_M_ARM64) || defined(__aarch64__)
    key.target=2;
#endif
#if defined(HS_GEOMETRY_ERROR_FP_STRICT) && HS_GEOMETRY_ERROR_FP_STRICT && !defined(_M_FP_FAST) && !defined(__FAST_MATH__)
    key.floating_point_contract=1;
#endif
    key.rounding_mode=std::fegetround();
    return key;
}
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
    struct Nearest { double distance{};const T* triangle{}; };
    explicit Bvh(std::vector<T> source):triangles(std::move(source)){Require(!triangles.empty(),"empty target material surface");nodes.reserve(triangles.size()/2+1);root=Build(0,static_cast<unsigned>(triangles.size()));}
    unsigned Build(unsigned begin,unsigned end){
        unsigned id=static_cast<unsigned>(nodes.size());nodes.emplace_back();Node n;n.begin=begin;n.end=end;
        Box centers;for(unsigned i=begin;i<end;++i){for(V p:triangles[i].p)n.box.Add(p);centers.Add((triangles[i].p[0]+triangles[i].p[1]+triangles[i].p[2])*(1.0/3));}
        if(end-begin<=8)n.leaf=true;
        else {V extent=centers.hi-centers.lo;int axis=extent.y>extent.x?1:0;if(extent.z>Coord(extent,axis))axis=2;unsigned middle=(begin+end)/2;
            std::nth_element(triangles.begin()+begin,triangles.begin()+middle,triangles.begin()+end,[axis](const T& a,const T& b){return Coord(a.p[0]+a.p[1]+a.p[2],axis)<Coord(b.p[0]+b.p[1]+b.p[2],axis);});
            n.left=Build(begin,middle);n.right=Build(middle,end);}
        nodes[id]=n;return id;
    }
    Nearest Closest(V p)const{
        double best=INFINITY;const T* nearest=nullptr;
        // Build() always bisects the triangle range. An unsigned triangle count
        // yields depth <= 32; at most one sibling per level is pending.
        // An inline stack avoids one heap allocation per distance query.
        std::array<unsigned,64> stack{};std::size_t pending=0;stack[pending++]=root;
        const auto push=[&](unsigned node){Require(pending<stack.size(),"BVH traversal stack overflow");stack[pending++]=node;};
        while(pending){unsigned id=stack[--pending];const auto& n=nodes[id];if(n.box.Distance2(p)>best)continue;
            if(n.leaf){for(unsigned i=n.begin;i<n.end;++i){double d=TriangleDistance2(p,triangles[i]);if(d<best){best=d;nearest=&triangles[i];}}}
            else {double l=nodes[n.left].box.Distance2(p),r=nodes[n.right].box.Distance2(p);if(l<r){if(r<=best)push(n.right);if(l<=best)push(n.left);}else{if(l<=best)push(n.left);if(r<=best)push(n.right);}}
        }
        Require(nearest!=nullptr,"BVH target has no nearest triangle");
        return {std::sqrt(best),nearest};
    }
};
struct Patch {T t;double upper{};std::uint64_t serial{};};
struct Highest { bool operator()(const Patch& a,const Patch& b)const{return a.upper==b.upper?a.serial>b.serial:a.upper<b.upper;} };
using SurfaceKey=std::array<double,10>;
SurfaceKey Key(const T& t){auto p=t.p;std::sort(p.begin(),p.end(),[](V a,V b){return std::array<double,3>{a.x,a.y,a.z}<std::array<double,3>{b.x,b.y,b.z};});return {double(t.material),p[0].x,p[0].y,p[0].z,p[1].x,p[1].y,p[1].z,p[2].x,p[2].y,p[2].z};}
bool Qualified(const GeometryErrorQualification* qualification,const std::vector<T>& a,const std::vector<T>& b)
{
    if(!qualification) return false;
    const auto key=GeometryErrorImplementationKey();
    const auto &q=*qualification;
    if(!key.compiler || !key.target || key.floating_point_contract!=1 || key.rounding_mode!=FE_TONEAREST ||
       q.key!=key || !std::numeric_limits<double>::is_iec559 || sizeof(double)!=8 ||
       !std::isfinite(q.maximum_absolute_coordinate) || q.maximum_absolute_coordinate<0 ||
       !std::isfinite(q.minimum_positive_edge_squared) || q.minimum_positive_edge_squared<0 ||
       !std::isfinite(q.minimum_positive_area_squared) || q.minimum_positive_area_squared<0 ||
       !q.maximum_triangles_per_surface || a.size()>q.maximum_triangles_per_surface || b.size()>q.maximum_triangles_per_surface) return false;
    for(const auto* surface:{&a,&b}) for(const auto &triangle:*surface)
    {
        for(const auto p:triangle.p)
            if(std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z)})>q.maximum_absolute_coordinate) return false;
        for(unsigned i=0;i<3;++i)
        {
            const auto edge=Norm2(triangle.p[i]-triangle.p[(i+1)%3]);
            if(edge>0 && edge<q.minimum_positive_edge_squared) return false;
        }
        const auto ab=triangle.p[1]-triangle.p[0],ac=triangle.p[2]-triangle.p[0];
        const V cross{ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
        const auto area=Norm2(cross);
        if((area==0 && !q.includes_degenerate_triangles) || (area>0 && area<q.minimum_positive_area_squared)) return false;
    }
    return true;
}
GeometryErrorBounds Directed(const std::vector<T>& source,const std::vector<T>& target,double tolerance,std::uint64_t maxQueries,
                             std::uint64_t profileId,const char* label,bool qualified,GeometryErrorCheckMode mode){
    const bool profile=profileId!=0;
    const auto started=std::chrono::steady_clock::now();
    std::map<unsigned,std::vector<T>> grouped;std::vector<SurfaceKey> identical;identical.reserve(target.size());double coordinateScale=1;
    for(auto t:target){grouped[t.material].push_back(t);identical.push_back(Key(t));for(V p:t.p)coordinateScale=std::max({coordinateScale,std::abs(p.x),std::abs(p.y),std::abs(p.z)});}
    std::sort(identical.begin(),identical.end());
    for(auto t:source)for(V p:t.p)coordinateScale=std::max({coordinateScale,std::abs(p.x),std::abs(p.y),std::abs(p.z)});
    // Practical double-precision allowance, separate from the geometric proof.
    // This is deliberately much larger than one double ULP for these fixtures.
    const double numericMargin=coordinateScale*1e-9;
    std::map<unsigned,Bvh> surfaces;for(auto& [material,triangles]:grouped)surfaces.emplace(material,Bvh(std::move(triangles)));
    std::uint64_t oracle_queries=0;
    const auto check_query=[&](V point,unsigned material) {
        const auto found=surfaces.find(material);Require(found!=surfaces.end(),"authored LOD removes a complete material surface");
        double exhaustive=INFINITY;
        for(const auto &triangle:found->second.triangles) exhaustive=std::min(exhaustive,TriangleDistance2(point,triangle));
        const auto accelerated=found->second.Closest(point).distance;
        Require(std::isfinite(exhaustive) && std::isfinite(accelerated) &&
            std::abs(accelerated-std::sqrt(exhaustive))<=numericMargin,"BVH/exhaustive mismatch");
        ++oracle_queries;
    };
    if(mode==GeometryErrorCheckMode::Exhaustive)
    {
        for(const auto &triangle:source)
        {
            check_query((triangle.p[0]+triangle.p[1]+triangle.p[2])*(1.0/3),triangle.material);
            // The explicit adversarial corpus includes vertices and edge ties,
            // including repeated points and zero-area/collinear triangles.
            for(unsigned i=0;i<3;++i) {
                check_query(triangle.p[i],triangle.material);
                check_query((triangle.p[i]+triangle.p[(i+1)%3])*0.5,triangle.material);
            }
        }
    }
    else if(!qualified)
    {
        const auto step=std::max<std::size_t>(1,source.size()/12);
        for(std::size_t i=0,count=0;i<source.size() && count<12;i+=step,++count)
            check_query((source[i].p[0]+source[i].p[1]+source[i].p[2])*(1.0/3),source[i].material);
    }
    if(profile)std::fprintf(stderr,"hs.hlod.profile call=%llu direction=%s phase=oracle queries=%llu qualified=%u mode=%s\n",
        static_cast<unsigned long long>(profileId),label,static_cast<unsigned long long>(oracle_queries),
        qualified?1u:0u,mode==GeometryErrorCheckMode::Exhaustive?"exhaustive":"normal");
    if(profile)std::fprintf(stderr,"hs.hlod.profile call=%llu direction=%s phase=bvh source=%zu target=%zu materials=%zu elapsed_ms=%lld\n",
        static_cast<unsigned long long>(profileId),label,source.size(),target.size(),surfaces.size(),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()));
    std::vector<Patch> storage;storage.reserve(source.size());
    std::priority_queue<Patch,std::vector<Patch>,Highest> patches(Highest{},std::move(storage));
    std::uint64_t serial=0,queries=0;double lower=0;
    auto insert=[&](T t,double parentUpper){auto found=surfaces.find(t.material);Require(found!=surfaces.end(),"authored LOD removes a complete material surface");V center=(t.p[0]+t.p[1]+t.p[2])*(1.0/3);const auto closest=found->second.Closest(center);double distance=closest.distance;++queries;lower=std::max(lower,std::max(0.0,distance-numericMargin));double radius=std::sqrt(std::max({Norm2(t.p[0]-center),Norm2(t.p[1]-center),Norm2(t.p[2]-center)}));
        // The distance to any closed target set is 1-Lipschitz. Every point of
        // this source triangle lies inside the centroid sphere, so d(q)+r is
        // an upper bound for the complete triangle, not just sampled vertices.
        // A tighter certified bound is available from one target triangle:
        // distance to a closed convex triangle is convex, hence at any point
        // of this source triangle it cannot exceed its maximum at the three
        // source vertices. The selected target triangle is contained in the
        // complete target surface, so this bounds surface distance as well.
        // This is especially effective on coplanar, differently tessellated
        // patches where the centroid Lipschitz radius was grossly pessimistic.
        const T& triangle=*closest.triangle;
        const double convexBound=std::sqrt(std::max({
            TriangleDistance2(t.p[0],triangle),
            TriangleDistance2(t.p[1],triangle),
            TriangleDistance2(t.p[2],triangle)}));
        const double certifiedBound=std::min(distance+radius,convexBound);
        double upper=std::min(parentUpper,std::nextafter(certifiedBound+numericMargin,INFINITY));patches.push({t,upper,serial++});};
    for(auto t:source)if(!std::binary_search(identical.data(),identical.data()+identical.size(),Key(t)))insert(t,INFINITY);
    // Every source face needs initial coverage; a refinement budget cannot
    // invalidate large but otherwise valid surfaces before that coverage exists.
    const auto initialQueries=queries;
    if(profile)std::fprintf(stderr,"hs.hlod.profile call=%llu direction=%s phase=initial queries=%llu patches=%zu elapsed_ms=%lld\n",
        static_cast<unsigned long long>(profileId),label,static_cast<unsigned long long>(queries),patches.size(),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()));
    while(!patches.empty()&&patches.top().upper-lower>tolerance&&queries-initialQueries+4<=maxQueries){auto p=patches.top();patches.pop();V a=p.t.p[0],b=p.t.p[1],c=p.t.p[2],ab=(a+b)*0.5,bc=(b+c)*0.5,ca=(c+a)*0.5;
        insert(T{{a,ab,ca},p.t.material},p.upper);insert(T{{ab,b,bc},p.t.material},p.upper);insert(T{{ca,bc,c},p.t.material},p.upper);insert(T{{ab,bc,ca},p.t.material},p.upper);}
    double upper=patches.empty()?0.0:std::max(lower,patches.top().upper);
    if(profile)std::fprintf(stderr,"hs.hlod.profile call=%llu direction=%s phase=done initial_queries=%llu refinement_queries=%llu patches=%zu lower=%.7g upper=%.7g tolerance=%.7g elapsed_ms=%lld\n",
        static_cast<unsigned long long>(profileId),label,static_cast<unsigned long long>(initialQueries),
        static_cast<unsigned long long>(queries-initialQueries),patches.size(),lower,upper,tolerance,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()));
    return {lower,upper,oracle_queries,qualified && mode!=GeometryErrorCheckMode::Exhaustive};
}
std::vector<T> Surface(std::span<const render_data::ExpandedVertex> vertices,
                       std::span<const std::uint16_t> material_mapping) {
    Require(!vertices.empty() && vertices.size()%3==0,"invalid error surface");
    Require(vertices.size()/3<=std::numeric_limits<unsigned>::max(),"error surface exceeds BVH triangle addressing");
    std::vector<T> out;out.reserve(vertices.size()/3);
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
GeometryErrorBounds MeasureGeometryError(std::span<const render_data::ExpandedVertex> fine,std::span<const render_data::ExpandedVertex> coarse,std::span<const std::uint16_t> material_mapping,std::uint64_t refinement_budget,GeometryErrorChecks checks) {
    const auto a=Surface(fine,material_mapping),b=Surface(coarse,material_mapping);Box box;for(const auto&t:a)for(auto p:t.p)box.Add(p);const double tolerance=std::sqrt(Norm2(box.hi-box.lo))*0.005;
    const bool qualified=checks.mode==GeometryErrorCheckMode::Normal && Qualified(checks.qualification,a,b);
    static std::atomic<std::uint64_t> profileCounter{0};
    const bool profile=std::getenv("HS_HLOD_GEOMETRY_PROFILE")!=nullptr;
    const auto id=profile?profileCounter.fetch_add(1,std::memory_order_relaxed)+1:0;
    if(profile)std::fprintf(stderr,"hs.hlod.profile call=%llu phase=begin fine=%zu coarse=%zu tolerance=%.7g\n",
        static_cast<unsigned long long>(id),a.size(),b.size(),tolerance);
    const auto forward=Directed(a,b,tolerance,refinement_budget,id,"forward",qualified,checks.mode),reverse=Directed(b,a,tolerance,refinement_budget,id,"reverse",qualified,checks.mode);
    return {std::max(forward.lower,reverse.lower),std::max(forward.upper,reverse.upper),
        forward.oracle_queries+reverse.oracle_queries,qualified};
}
} // namespace hs::geometry_content
