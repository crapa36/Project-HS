#pragma once

#include <hs/core/types.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <span>
#include <utility>

namespace hs
{
enum class VirtualShadowCsmReason : std::uint8_t
{
    Guaranteed, NoCache, NoCompatibleBake, SeedUnavailable, ReceiverDomainIncomplete,
    UnsupportedLayout, CoarseSlotBudget, InvalidLightSpace, ReceiverOutsideCoverage,
    CommandGroupLimit, CoarseCommandBudget, CasterOutsideDepth,
};
[[nodiscard]] inline std::string_view VirtualShadowCsmReasonName(VirtualShadowCsmReason reason) noexcept
{
    switch (reason)
    {
    case VirtualShadowCsmReason::Guaranteed: return "guaranteed-coarse";
    case VirtualShadowCsmReason::NoCache: return "no-cache";
    case VirtualShadowCsmReason::NoCompatibleBake: return "no-compatible-bake";
    case VirtualShadowCsmReason::SeedUnavailable: return "seed-unavailable";
    case VirtualShadowCsmReason::ReceiverDomainIncomplete: return "receiver-domain-incomplete";
    case VirtualShadowCsmReason::UnsupportedLayout: return "unsupported-layout";
    case VirtualShadowCsmReason::CoarseSlotBudget: return "coarse-slot-budget";
    case VirtualShadowCsmReason::InvalidLightSpace: return "invalid-light-space";
    case VirtualShadowCsmReason::ReceiverOutsideCoverage: return "receiver-outside-coverage";
    case VirtualShadowCsmReason::CommandGroupLimit: return "command-group-limit";
    case VirtualShadowCsmReason::CoarseCommandBudget: return "coarse-command-budget";
    case VirtualShadowCsmReason::CasterOutsideDepth: return "caster-outside-depth";
    }
    return "unknown";
}
struct VirtualShadowGuarantee
{
    bool csm_required{true};
    VirtualShadowCsmReason reason{VirtualShadowCsmReason::NoCache};
    std::uint32_t required_coarse_pages{}, maximum_coarse_commands{};
    std::uint64_t coarse_command_total{};
    std::uint32_t command_capacity{};
};
enum ShadowReceiverDomain : std::uint32_t
{
    ShadowReceiverCamera = 1u, ShadowReceiverWorld = 2u, ShadowReceiverSoftwareGi = 4u,
};
struct ShadowSurfaceReceiver { Float4 minimum{},maximum{}; };
struct ShadowSurfaceReceiverRange
{
    std::span<const ShadowSurfaceReceiver> stable,dynamic,stable_aggregate;
};
struct ShadowCommandSegment { std::uint32_t first{},capacity{}; };
static_assert(sizeof(ShadowCommandSegment)==8);
[[nodiscard]] constexpr bool IsCompleteBakedStaticCaster(bool present,bool immutable_source,
    bool baked_source,bool skin,bool wind,bool animated_alpha) noexcept
{
    return present&&immutable_source&&baked_source&&!skin&&!wind&&!animated_alpha;
}
struct ShadowCommandDemand
{
    std::array<std::uint64_t,256> coarse{},weights{};
    std::uint32_t maximum{};
    VirtualShadowCsmReason reason{VirtualShadowCsmReason::Guaranteed};
    bool uses_spatial_facts{};
};
// Fixed native command storage, partitioned without overlap. Coarse commands
// reserve first; remaining capacity follows actual segment populations. Empty
// opposite-layer segments do not consume another group's fine-page budget.
[[nodiscard]] inline bool PlanShadowCommandSegments(std::uint32_t capacity,
    std::span<const std::uint64_t> coarse,std::span<const std::uint64_t> weights,
    std::span<ShadowCommandSegment> segments)
{
    const bool sized=coarse.size()==segments.size()&&weights.size()==segments.size();
    std::uint64_t required{},weight_sum{};bool weights_valid=sized,reserve=sized;
    for(std::size_t i=0;i<segments.size()&&sized;++i)
    {
        if(coarse[i]>capacity||required>capacity-coarse[i])reserve=false;
        else required+=coarse[i];
        if(weights[i]>UINT32_MAX||weight_sum>UINT32_MAX-weights[i])weights_valid=false;
        else weight_sum+=weights[i];
    }
    if(!reserve)required=0;
    const bool equal=!weights_valid||!weight_sum;
    if(equal)weight_sum=segments.size();
    const auto remaining=capacity-static_cast<std::uint32_t>(required);
    std::uint64_t allocated{};
    for(std::size_t i=0;i<segments.size();++i)
    {
        const auto weight=equal?1u:weights[i];
        const auto extra=weight_sum?std::uint64_t(remaining)*weight/weight_sum:0;
        segments[i]={0,static_cast<std::uint32_t>((reserve?coarse[i]:0)+extra)};
        allocated+=segments[i].capacity;
    }
    auto left=capacity-static_cast<std::uint32_t>(allocated);
    for(std::size_t i=0;i<segments.size()&&left;++i)
        if(equal||weights[i]) { ++segments[i].capacity;--left; }
    std::uint32_t first{};
    for(auto &segment:segments) { segment.first=first;first+=segment.capacity; }
    return reserve;
}
[[nodiscard]] inline bool ValidateShadowCommandSegments(std::span<const ShadowCommandSegment> segments,std::uint32_t capacity)
{
    std::uint32_t end{};
    for(const auto segment:segments)
    {
        if(segment.first!=end||segment.capacity>capacity-end)return false;
        end+=segment.capacity;
    }
    return true;
}
// Same qualified position enclosure feeds receiver coverage and caster overlap.
// Radius includes the rounded float center and is rounded outward once more.
[[nodiscard]] inline std::optional<Float4> ShadowSurfaceCasterSphere(const ShadowSurfaceReceiver &surface)
{
    const std::array<double,3> lo{surface.minimum.x,surface.minimum.y,surface.minimum.z};
    const std::array<double,3> hi{surface.maximum.x,surface.maximum.y,surface.maximum.z};
    std::array<float,3> center{};double squared{};
    for(unsigned axis=0;axis<3;++axis)
    {
        if(!std::isfinite(lo[axis])||!std::isfinite(hi[axis])||lo[axis]>hi[axis])return {};
        center[axis]=float((lo[axis]+hi[axis])*.5);
        const double extent=std::max(std::abs(lo[axis]-center[axis]),std::abs(hi[axis]-center[axis]));
        squared+=extent*extent;
    }
    const double radius=std::sqrt(squared);
    if(!std::isfinite(radius)||radius>=std::numeric_limits<float>::max())return {};
    return Float4{center[0],center[1],center[2],std::nextafter(float(radius),std::numeric_limits<float>::max())};
}

// Fixed CPU preparation facts, not a second caster/receiver owner. The two
// ranges below borrow the existing concrete records; there is no native API.
struct ShadowGuaranteeFacts
{
    Float4 right{}, up{}, forward{}, depth{};
    std::array<std::uint32_t,3> window_sides{};
    std::int32_t minimum_x{}, minimum_y{};
    float page_world_size{}, world_texel{};
    std::uint32_t physical_pages{}, filter_samples{}, segment_capacity{}, draw_group_count{},command_capacity{};
    std::span<const ShadowCommandSegment> command_segments;
    // Exact current raster clip halfspaces, in the same scene basis as receivers.
    std::array<std::array<double,4>,6> receiver_frustum{};
    std::uint32_t required_receiver_domains{}, closed_receiver_domains{};
    bool compatible_bake{}, seed_available{}, camera_valid{};
    // CPU-only index of the World AABB that encloses the exact camera frustum.
    // It is validated against receivers and never enters the GPU receiver ABI.
    std::uint32_t frustum_receiver_index=UINT32_MAX;
    // Main-camera CSM shader clip halfspaces for the nonzero smoothstep region.
    // This is a CPU proof input only; local probe capture owns its own shadow map.
    std::array<std::array<std::array<double,4>,6>,3> csm_query_frusta{};
    bool csm_query_frusta_valid{};
};

using ShadowViewProjection = std::array<float,16>;
using ShadowCsmFrusta = std::array<std::array<std::array<double,4>,6>,3>;

// Convert row-vector orthographic matrices as consumed by HLSL mul(position,M).
// The domain expands past the fade start and depth endpoints to cover float
// projection error. Unexpected perspective/nonfinite matrices fail closed.
[[nodiscard]] inline bool BuildShadowCsmQueryFrusta(
    const std::array<ShadowViewProjection,3> &matrices, ShadowCsmFrusta &frusta) noexcept
{
    constexpr double edge_start=0.02;
    constexpr double clip_rounding=1.0e-4;
    constexpr double depth_rounding=1.0e-5;
    for(std::size_t cascade=0;cascade<matrices.size();++cascade)
    {
        const auto &m=matrices[cascade];
        for(const float value:m) if(!std::isfinite(value)) return false;
        if(m[3]!=0.0f || m[7]!=0.0f || m[11]!=0.0f || m[15]!=1.0f) return false;
        const double a=m[0],b=m[1],c=m[2],d=m[4],e=m[5],f=m[6],g=m[8],h=m[9],i=m[10];
        const double determinant=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
        const double scale=std::sqrt(a*a+b*b+c*c)*std::sqrt(d*d+e*e+f*f)*std::sqrt(g*g+h*h+i*i);
        if(!std::isfinite(determinant)||!std::isfinite(scale)||scale<=0||std::abs(determinant)<=scale*1.e-8) return false;
        const double bound=1.0-2.0*edge_start+clip_rounding;
        const auto make=[&](std::size_t axis,double scale,double w_scale)
        {
            std::array<double,4> p{};
            for(std::size_t row=0;row<4;++row)
                p[row]=scale*double(m[row*4+axis])+w_scale*double(m[row*4+3]);
            return p;
        };
        frusta[cascade][0]=make(0,1.0,bound);       // x + bound*w >= 0
        frusta[cascade][1]=make(0,-1.0,bound);      // bound*w - x >= 0
        frusta[cascade][2]=make(1,1.0,bound);       // y + bound*w >= 0
        frusta[cascade][3]=make(1,-1.0,bound);      // bound*w - y >= 0
        frusta[cascade][4]=make(2,1.0,depth_rounding); // z + epsilon*w >= 0
        frusta[cascade][5]=make(2,-1.0,1.0+depth_rounding); // (1+epsilon)*w-z >= 0
        for(auto &p:frusta[cascade])
        {
            const double length=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
            if(!std::isfinite(length)||length<=0||!std::isfinite(p[3])) return false;
            for(auto &value:p)value/=length;
        }
    }
    return true;
}

[[nodiscard]] inline std::pair<double,double> ShadowProjectCoordinate(const auto &p,Float4 axis)
{
    const double value=double(p.x)*axis.x+double(p.y)*axis.y+double(p.z)*axis.z;
    const double error=(std::abs(double(p.x)*axis.x)+std::abs(double(p.y)*axis.y)+std::abs(double(p.z)*axis.z))*0x1p-20+1.e-5;
    return {value,error};
}
struct ShadowRootCoverageDomain
{
    Float4 right{},up{},forward{};
    double minimum_x{},minimum_y{},maximum_x{},maximum_y{},minimum_z{},maximum_z{};
};
[[nodiscard]] inline bool ShadowRootReceiverBoundsValid(Float4 minimum,Float4 maximum) noexcept
{
    return std::isfinite(minimum.x)&&std::isfinite(minimum.y)&&std::isfinite(minimum.z)&&std::isfinite(minimum.w)&&
        std::isfinite(maximum.x)&&std::isfinite(maximum.y)&&std::isfinite(maximum.z)&&std::isfinite(maximum.w)&&
        minimum.x<=maximum.x&&minimum.y<=maximum.y&&minimum.z<=maximum.z&&
        (minimum.w==0.0f||minimum.w==1.0f)&&(maximum.w==0.0f||maximum.w==1.0f);
}
[[nodiscard]] inline bool ShadowRootContainsXY(Float3 point,const ShadowRootCoverageDomain &domain) noexcept
{
    if(!std::isfinite(point.x)||!std::isfinite(point.y)||!std::isfinite(point.z))return false;
    const auto [x,ex]=ShadowProjectCoordinate(point,domain.right);
    const auto [y,ey]=ShadowProjectCoordinate(point,domain.up);
    return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(ex)&&std::isfinite(ey)&&
        x-ex>=domain.minimum_x&&y-ey>=domain.minimum_y&&
        x+ex<domain.maximum_x&&y+ey<domain.maximum_y;
}
[[nodiscard]] inline bool ShadowRootContainsCaster(Float4 bounds,const ShadowRootCoverageDomain &domain) noexcept
{
    if(!std::isfinite(bounds.x)||!std::isfinite(bounds.y)||!std::isfinite(bounds.z)||
       !std::isfinite(bounds.w)||bounds.w<0)return false;
    const auto [x,ex]=ShadowProjectCoordinate(bounds,domain.right);
    const auto [y,ey]=ShadowProjectCoordinate(bounds,domain.up);
    const auto [z,ez]=ShadowProjectCoordinate(bounds,domain.forward);
    const double radius=double(bounds.w)*(1.0+0x1p-20)+1.e-5;
    return std::isfinite(radius)&&x-ex-radius>=domain.minimum_x&&y-ey-radius>=domain.minimum_y&&
        x+ex+radius<domain.maximum_x&&y+ey+radius<domain.maximum_y&&
        z-ez-radius>=domain.minimum_z&&z+ez+radius<=domain.maximum_z;
}
template<typename Casters>
[[nodiscard]] ShadowCommandDemand ComputeShadowCommandDemand(const ShadowGuaranteeFacts &f,const Casters &casters,
    ShadowCommandDemand result={})
{
    // A retained prefix must have been evaluated with the same relevant demand facts.
    // Preserve the original first-failure result before visiting the suffix.
    if(result.reason!=VirtualShadowCsmReason::Guaranteed)return result;
    const auto groups=f.draw_group_count;
    if(groups>result.coarse.size()/2) { result.reason=VirtualShadowCsmReason::CommandGroupLimit;return result; }
    const double side=f.page_world_size,guard=double(f.world_texel)*2;
    for(const auto &caster:casters)
    {
        if(!caster.present)continue;
        const auto &bounds=caster.current_bounds;
        if(!std::isfinite(bounds.x)||!std::isfinite(bounds.y)||!std::isfinite(bounds.z)||!std::isfinite(bounds.w)||bounds.w<0)
        { result.reason=VirtualShadowCsmReason::InvalidLightSpace;return result; }
        const bool is_static=caster.immutable_source&&!caster.skin&&!caster.wind&&!caster.animated_alpha;
        const bool seeded=is_static&&caster.baked_source&&f.compatible_bake;
        if(caster.draw_group>=groups)
        { if(seeded)continue;result.reason=VirtualShadowCsmReason::CommandGroupLimit;return result; }
        const auto segment=(is_static?0:groups)+caster.draw_group;
        ++result.weights[segment]; // Seeded static geometry still renders requested fine pages.
        if(seeded)continue;
        result.uses_spatial_facts=true;
        const auto [x,ex]=ShadowProjectCoordinate(bounds,f.right);
        const auto [y,ey]=ShadowProjectCoordinate(bounds,f.up);
        const auto [z,ez]=ShadowProjectCoordinate(bounds,f.forward);
        const double radius=double(bounds.w)*(1.0+0x1p-20)+1.e-5;
        if(z-ez-radius<f.depth.x||z+ez+radius>double(f.depth.x)+f.depth.w)
        { result.reason=VirtualShadowCsmReason::CasterOutsideDepth;return result; }
        // The guarded tile predicate is separable in XY. This is the same
        // conservative64-page bound with eight paired axis checks, not64 products.
        std::uint32_t columns{},rows{};
        for(int cell=0;cell<8;++cell)
        {
            const double left=(double(f.minimum_x)+cell)*side-guard,bottom=(double(f.minimum_y)+cell)*side-guard;
            columns+=!(x+ex+radius<left||x-ex-radius>left+side+2*guard);
            rows+=!(y+ey+radius<bottom||y-ey-radius>bottom+side+2*guard);
        }
        const auto count=(result.coarse[segment]+=columns*rows);
        result.maximum=std::max(result.maximum,static_cast<std::uint32_t>(std::min<std::uint64_t>(count,UINT32_MAX)));
    }
    return result;
}

template<typename Covered>
[[nodiscard]] inline bool ShadowCsmFrustumFullyCovered(
    const std::array<std::array<double,4>,6> &source,Covered &&covered)
{
    using Plane=std::array<double,4>;
    auto planes=source;
    const auto cross=[](const Plane &a,const Plane &b) { return std::array<double,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; };
    std::array<std::array<double,3>,16> points{};
    std::size_t count{};
    const auto enumerate=[&](const auto &active,auto &&visit)
    {
        std::size_t found{};
        for(unsigned a=0;a<4;++a)for(unsigned b=a+1;b<5;++b)for(unsigned c=b+1;c<6;++c)
        {
            const auto bc=cross(active[b],active[c]),ca=cross(active[c],active[a]),ab=cross(active[a],active[b]);
            const double determinant=active[a][0]*bc[0]+active[a][1]*bc[1]+active[a][2]*bc[2];
            if(!std::isfinite(determinant)||std::abs(determinant)<1.e-12)continue;
            std::array<double,3> point{};
            for(unsigned axis=0;axis<3;++axis)
                point[axis]=(-active[a][3]*bc[axis]-active[b][3]*ca[axis]-active[c][3]*ab[axis])/determinant;
            bool inside=std::isfinite(point[0])&&std::isfinite(point[1])&&std::isfinite(point[2]);
            for(const auto &p:active)
            {
                const double sum=p[0]*point[0]+p[1]*point[1]+p[2]*point[2]+p[3];
                const double error=(std::abs(p[0]*point[0])+std::abs(p[1]*point[1])+std::abs(p[2]*point[2])+std::abs(p[3]))*1.e-9+1.e-7;
                if(sum < -error){inside=false;break;}
            }
            if(inside){++found;if(!visit(point))return std::size_t{};}
        }
        return found;
    };
    count=enumerate(planes,[&](const auto &point)
    {
        for(unsigned axis=0;axis<3;++axis)points[count][axis]=point[axis];
        ++count;return count<points.size();
    });
    if(count!=8)return false;
    std::array<double,3> max_abs{};
    for(std::size_t i=0;i<count;++i)for(unsigned axis=0;axis<3;++axis)
        max_abs[axis]=std::max(max_abs[axis],std::abs(points[i][axis]));
    for(auto &p:planes)
        p[3]+=(std::abs(p[0])*max_abs[0]+std::abs(p[1])*max_abs[1]+std::abs(p[2])*max_abs[2]+std::abs(p[3]))*0x1p-18+1.e-5;
    bool all_covered=true;
    const auto expanded_count=enumerate(planes,[&](const auto &point)
    {
        all_covered&=covered(Float3{float(point[0]),float(point[1]),float(point[2])});
        return all_covered;
    });
    return all_covered&&expanded_count==8;
}

// Vertices of receiver AABB intersect camera plus optional CSM query frusta.
// The maximum18 halfspaces have816 plane triples. All-corner coverage and
// separating-plane tests keep normal instances on bounded fast paths.
template<typename Receiver,typename Covered>
[[nodiscard]] int CheckShadowCameraReceiver(const Receiver &r,Covered &&covered,
    const std::array<std::array<double,4>,6> &camera_frustum,
    const std::array<std::array<double,4>,6> *csm_frustum=nullptr)
{
    using Plane=std::array<double,4>;
    std::array<Plane,18> planes{};
    const std::array<double,3> lo{r.minimum.x,r.minimum.y,r.minimum.z},hi{r.maximum.x,r.maximum.y,r.maximum.z};
    const unsigned frustum_count=csm_frustum?12u:6u;
    for(unsigned i=0;i<6;++i) planes[i]=camera_frustum[i];
    if(csm_frustum) for(unsigned i=0;i<6;++i) planes[6+i]=(*csm_frustum)[i];
    for(unsigned i=0;i<frustum_count;++i)
    {
        auto &p=planes[i];
        if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2])||!std::isfinite(p[3])) return -1;
        // Outward allowance for the actual float clip dot product, never an
        // inward receiver/camera shrink. Shader positions have outward bounds.
        p[3]+=(std::abs(p[0])*std::max(std::abs(lo[0]),std::abs(hi[0]))+
               std::abs(p[1])*std::max(std::abs(lo[1]),std::abs(hi[1]))+
               std::abs(p[2])*std::max(std::abs(lo[2]),std::abs(hi[2]))+std::abs(p[3]))*0x1p-18+1.e-5;
        double maximum=p[3];
        for(unsigned axis=0;axis<3;++axis) maximum+=p[axis]*(p[axis]>=0?hi[axis]:lo[axis]);
        if(maximum<0) return 0; // A known surface wholly outside actual raster.
    }
    for(unsigned axis=0;axis<3;++axis)
    { planes[frustum_count+axis*2][axis]=1;planes[frustum_count+axis*2][3]=-lo[axis];planes[frustum_count+axis*2+1][axis]=-1;planes[frustum_count+axis*2+1][3]=hi[axis]; }
    bool all_covered=true;
    for(unsigned corner=0;corner<8;++corner)
        all_covered&=covered(Float3{float(corner&1?hi[0]:lo[0]),float(corner&2?hi[1]:lo[1]),float(corner&4?hi[2]:lo[2])});
    if(all_covered) return 1;
    const auto cross=[](const Plane &a,const Plane &b) { return std::array<double,3>{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; };
    bool found_vertex{};
    const unsigned plane_count=frustum_count+6;
    for(unsigned a=0;a+2<plane_count;++a) for(unsigned b=a+1;b+1<plane_count;++b) for(unsigned c=b+1;c<plane_count;++c)
    {
        const auto bc=cross(planes[b],planes[c]),ca=cross(planes[c],planes[a]),ab=cross(planes[a],planes[b]);
        const double determinant=planes[a][0]*bc[0]+planes[a][1]*bc[1]+planes[a][2]*bc[2];
        if(determinant==0) continue;
        std::array<double,3> point{};
        for(unsigned axis=0;axis<3;++axis)
            point[axis]=(-planes[a][3]*bc[axis]-planes[b][3]*ca[axis]-planes[c][3]*ab[axis])/determinant;
        if(!std::isfinite(point[0])||!std::isfinite(point[1])||!std::isfinite(point[2])) return -1;
        bool inside=true;
        for(unsigned plane=0;plane<plane_count;++plane)
        {
            const auto &p=planes[plane];
            const double sum=p[0]*point[0]+p[1]*point[1]+p[2]*point[2]+p[3];
            const double error=(std::abs(p[0]*point[0])+std::abs(p[1]*point[1])+std::abs(p[2]*point[2])+std::abs(p[3]))*1.e-9+1.e-7;
            if(sum < -error) { inside=false;break; }
        }
        if(inside)
        {
            found_vertex=true;
            if(!covered(Float3{float(point[0]),float(point[1]),float(point[2])})) return 2;
        }
    }
    // No feasible vertex is not promoted to an empty-domain proof: degenerate
    // or numerically ambiguous intersections conservatively retain actual CSM.
    return found_vertex?1:2;
}

template <typename Casters, typename Receivers>
[[nodiscard]] VirtualShadowGuarantee CheckVirtualShadowGuaranteeFacts(
    const ShadowGuaranteeFacts &f, const Casters &casters, const Receivers &receivers,ShadowSurfaceReceiverRange surfaces,
    const ShadowCommandDemand *prepared_commands=nullptr)
{
    VirtualShadowGuarantee result;
    if(prepared_commands)result.maximum_coarse_commands=prepared_commands->maximum;
    const auto deny = [&](VirtualShadowCsmReason reason) { result.reason = reason; return result; };
    if (!f.compatible_bake) return deny(VirtualShadowCsmReason::NoCompatibleBake);
    if (!f.seed_available) return deny(VirtualShadowCsmReason::SeedUnavailable);
    constexpr auto known=ShadowReceiverCamera|ShadowReceiverWorld|ShadowReceiverSoftwareGi;
    if(!(f.required_receiver_domains&ShadowReceiverCamera) || (f.required_receiver_domains&~known) ||
       (f.closed_receiver_domains&f.required_receiver_domains)!=f.required_receiver_domains)
        return deny(VirtualShadowCsmReason::ReceiverDomainIncomplete);
    if (f.window_sides != std::array<std::uint32_t,3>{32,8,8})
        return deny(VirtualShadowCsmReason::UnsupportedLayout);
    result.required_coarse_pages = 64;
    if (f.physical_pages < result.required_coarse_pages) return deny(VirtualShadowCsmReason::CoarseSlotBudget);
    const auto finite = [](const auto &v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
    };
    const auto dot = [](Float4 a, Float4 b) { return double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z; };
    const double side = f.page_world_size, texel = f.world_texel;
    if (!f.camera_valid || !finite(f.right) || !finite(f.up) || !finite(f.forward) || !finite(f.depth) ||
        !(f.depth.w > 0) || !(f.depth.y > 0) || f.depth.z < 0 ||
        std::abs(dot(f.right,f.right)-1) > 1.e-4 || std::abs(dot(f.up,f.up)-1) > 1.e-4 ||
        std::abs(dot(f.forward,f.forward)-1) > 1.e-4 || std::abs(dot(f.right,f.up)) > 1.e-4 ||
        std::abs(dot(f.right,f.forward)) > 1.e-4 || std::abs(dot(f.up,f.forward)) > 1.e-4 ||
        !std::isfinite(side) || side <= 0 || !std::isfinite(texel) || texel <= 0 ||
        !f.filter_samples || f.filter_samples > 16)
        return deny(VirtualShadowCsmReason::InvalidLightSpace);
    const double lower_x = double(f.minimum_x)*side, lower_y = double(f.minimum_y)*side;
    const double upper_x = (double(f.minimum_x)+8)*side, upper_y = (double(f.minimum_y)+8)*side;
    // Outward dot-product error covers GPU float rounding at key boundaries.
    const auto covered = [&](Float3 p) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        const auto [x,ex] = ShadowProjectCoordinate(p,f.right);
        const auto [y,ey] = ShadowProjectCoordinate(p,f.up);
        const auto [z,ez] = ShadowProjectCoordinate(p,f.forward);
        return x-ex >= lower_x && x+ex < upper_x && y-ey >= lower_y && y+ey < upper_y &&
            z-ez >= f.depth.x && z+ez <= double(f.depth.x)+f.depth.w;
    };
    const auto normalize_frustum=[](auto &frustum)
    {
        for(auto &p:frustum)
        {
            for(const auto value:p)if(!std::isfinite(value))return false;
            const double length=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
            if(!std::isfinite(length)||length<=0)return false;
            for(auto &value:p)value/=length;
        }
        return true;
    };
    auto camera_frustum=f.receiver_frustum;
    const bool camera_frustum_valid=normalize_frustum(camera_frustum);
    auto csm_query_frusta=f.csm_query_frusta;
    bool csm_query_frusta_valid=f.csm_query_frusta_valid;
    if(csm_query_frusta_valid)
        for(auto &frustum:csm_query_frusta)csm_query_frusta_valid&=normalize_frustum(frustum);
    std::array<bool,3> csm_frusta_covered{};
    if(csm_query_frusta_valid)
        for(std::size_t cascade=0;cascade<csm_query_frusta.size();++cascade)
            csm_frusta_covered[cascade]=ShadowCsmFrustumFullyCovered(csm_query_frusta[cascade],covered);
    const auto camera_csm_receiver_check=[&](const auto &receiver)
    {
        if(!camera_frustum_valid||!csm_query_frusta_valid)return -1;
        bool all_corners_covered=true;
        for(unsigned corner=0;corner<8;++corner)
            all_corners_covered&=covered({corner&1u?receiver.maximum.x:receiver.minimum.x,
                corner&2u?receiver.maximum.y:receiver.minimum.y,corner&4u?receiver.maximum.z:receiver.minimum.z});
        if(all_corners_covered)return 1;
        for(std::size_t cascade=0;cascade<csm_query_frusta.size();++cascade)
        {
            if(csm_frusta_covered[cascade])continue;
            const auto status=CheckShadowCameraReceiver(receiver,covered,camera_frustum,&csm_query_frusta[cascade]);
            if(status<0||status==2)return status;
        }
        return 1;
    };
    if(f.frustum_receiver_index!=UINT32_MAX && f.frustum_receiver_index>=receivers.size())
        return deny(VirtualShadowCsmReason::InvalidLightSpace);
    const auto receiver_check=[&](const auto &receiver,bool camera,bool frustum_enclosure=false)
    {
        if (!finite(receiver.minimum) || !finite(receiver.maximum) ||
            (receiver.minimum.w != 0 && receiver.minimum.w != 1) ||
            receiver.minimum.x > receiver.maximum.x || receiver.minimum.y > receiver.maximum.y ||
            receiver.minimum.z > receiver.maximum.z || (receiver.maximum.w!=0&&receiver.maximum.w!=1)) return -1;
        if(frustum_enclosure && (receiver.minimum.w!=0 || receiver.maximum.w!=0)) return -1;
        if(camera&&(receiver.minimum.w!=0||receiver.maximum.w!=1)) return -1;
        if (receiver.minimum.w == 1) return 1; // Private capture retains its own actual CSM.
        // Camera-only GPU feedback envelopes (water/gel/monster unions) are not
        // the surface proof. The independent complete actual-draw range below
        // supplies that proof with tighter bounds, including shadow-off draws.
        if(!camera&&receiver.maximum.w==1) return 1;
        if(camera||frustum_enclosure) return camera_csm_receiver_check(receiver);
        for (unsigned corner = 0; corner < 8; ++corner)
            if (!covered({corner&1u ? receiver.maximum.x : receiver.minimum.x,
                          corner&2u ? receiver.maximum.y : receiver.minimum.y,
                          corner&4u ? receiver.maximum.z : receiver.minimum.z}))
                return 2;
        return 1;
    };
    for(std::size_t index=0;index<receivers.size();++index)
    {
        const auto status=receiver_check(receivers[index],false,index==f.frustum_receiver_index);
        if(status<0)return deny(VirtualShadowCsmReason::InvalidLightSpace);
        if(status==2)return deny(VirtualShadowCsmReason::ReceiverOutsideCoverage);
    }
    bool stable_covered{};
    if(surfaces.stable_aggregate.size()>1)return deny(VirtualShadowCsmReason::InvalidLightSpace);
    if(surfaces.stable_aggregate.size()==1)
    {
        const auto status=receiver_check(surfaces.stable_aggregate.front(),true);
        if(status<0)return deny(VirtualShadowCsmReason::InvalidLightSpace);
        stable_covered=status!=2;
    }
    if(!stable_covered) for(const auto &receiver:surfaces.stable)
    { const auto status=receiver_check(receiver,true);if(status<0)return deny(VirtualShadowCsmReason::InvalidLightSpace);if(status==2)return deny(VirtualShadowCsmReason::ReceiverOutsideCoverage); }
    for(const auto &receiver:surfaces.dynamic)
    { const auto status=receiver_check(receiver,true);if(status<0)return deny(VirtualShadowCsmReason::InvalidLightSpace);if(status==2)return deny(VirtualShadowCsmReason::ReceiverOutsideCoverage); }
    const auto groups = f.draw_group_count;
    const auto calculated=prepared_commands?ShadowCommandDemand{}:ComputeShadowCommandDemand(f,casters);
    const auto &commands=prepared_commands?*prepared_commands:calculated;
    result.maximum_coarse_commands=commands.maximum;
    result.command_capacity=f.command_capacity;
    for(const auto count:commands.coarse)result.coarse_command_total+=count;
    if(commands.reason!=VirtualShadowCsmReason::Guaranteed)return deny(commands.reason);
    if(groups>commands.coarse.size()/2)return deny(VirtualShadowCsmReason::CommandGroupLimit);
    if((f.command_capacity||!f.command_segments.empty())&&f.command_segments.size()!=std::size_t(groups)*2)
        return deny(VirtualShadowCsmReason::CommandGroupLimit);
    if(!f.command_segments.empty()&&!ValidateShadowCommandSegments(f.command_segments,f.command_capacity))
        return deny(VirtualShadowCsmReason::CoarseCommandBudget);
    for(std::size_t segment=0;segment<std::size_t(groups)*2;++segment)
        if(commands.coarse[segment]>(f.command_segments.empty()?f.segment_capacity:f.command_segments[segment].capacity))
            return deny(VirtualShadowCsmReason::CoarseCommandBudget);
    result.csm_required = false;
    result.reason = VirtualShadowCsmReason::Guaranteed;
    return result;
}
template<typename Casters,typename Receivers>
[[nodiscard]] VirtualShadowGuarantee CheckVirtualShadowGuaranteeFacts(const ShadowGuaranteeFacts &f,
    const Casters &casters,const Receivers &receivers)
{
    return CheckVirtualShadowGuaranteeFacts(f,casters,receivers,ShadowSurfaceReceiverRange{});
}
} // namespace hs
