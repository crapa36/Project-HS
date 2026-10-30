#include "../Source/RendererD3D12/Private/renderer_shadow_guarantee.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace
{
struct Caster
{
    hs::Float4 current_bounds{};
    std::uint32_t draw_group{};
    bool present{true}, immutable_source{}, skin{}, wind{}, animated_alpha{}, baked_source{};
};
struct Receiver { hs::Float4 minimum{}, maximum{}; };

hs::ShadowGuaranteeFacts CompleteFacts()
{
    hs::ShadowGuaranteeFacts facts;
    facts.right = {1,0,0,0}; facts.up = {0,1,0,0}; facts.forward = {0,0,1,0};
    facts.depth = {-10,0.05f,0.0001f,20};
    facts.window_sides = {32,8,8}; facts.minimum_x = facts.minimum_y = -4;
    facts.page_world_size = 4; facts.world_texel = 4.0f/128.0f;
    facts.physical_pages = 64; facts.filter_samples = 16;
    facts.segment_capacity = 64; facts.draw_group_count = 1;
    facts.receiver_frustum={{{1,0,0,2},{-1,0,0,2},{0,1,0,2},{0,-1,0,2},{0,0,1,100},{0,0,-1,100}}};
    facts.required_receiver_domains=facts.closed_receiver_domains=hs::ShadowReceiverCamera;
    facts.compatible_bake = facts.seed_available = facts.camera_valid = true;
    std::array<hs::ShadowViewProjection,3> csm{};
    for(auto &matrix:csm)
    {
        matrix[0]=0.1f;matrix[5]=0.1f;matrix[10]=0.1f;matrix[14]=0.5f;matrix[15]=1.0f;
    }
    facts.csm_query_frusta_valid=hs::BuildShadowCsmQueryFrusta(csm,facts.csm_query_frusta);
    return facts;
}
void Require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}
std::string ReadShaderSource(const char *name)
{
    const auto locate=[&](std::filesystem::path directory) -> std::filesystem::path
    {
        for(unsigned depth=0;depth<10&&!directory.empty();++depth,directory=directory.parent_path())
        {
            const auto file=directory/"Content"/"Shaders"/name;
            if(std::filesystem::exists(file))return file;
        }
        return {};
    };
    auto path=locate(std::filesystem::path(__FILE__).parent_path());
    if(path.empty())path=locate(std::filesystem::current_path());
    if(path.empty())throw std::runtime_error("cannot locate authoritative shader source for the CSM query contract");
    std::ifstream stream(path,std::ios::binary);
    if(!stream)throw std::runtime_error("cannot read authoritative shader source for the CSM query contract");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}
}

// Called by the existing render-package CPU host; no device or Renderer library is needed.
void TestVirtualShadowGuarantees()
{
    using Reason = hs::VirtualShadowCsmReason;
    {
        const auto bounds=[](int key,float side,float guard) {
            const float lower=float(key)*side-guard;
            return std::array{lower,lower+side+2.0f*guard};
        };
        const auto range=[&](float center,float radius,int origin,int side,float page_side,float guard) {
            const float minimum=center-radius,maximum=center+radius;
            const auto first_bounds=bounds(origin,page_side,guard);
            const auto last_bounds=bounds(origin+side-1,page_side,guard);
            if(!std::isfinite(minimum)||!std::isfinite(maximum)||!std::isfinite(page_side)||
               !std::isfinite(guard)||page_side<=0||!std::isfinite(first_bounds[0])||
               !std::isfinite(first_bounds[1])||!std::isfinite(last_bounds[0])||
               !std::isfinite(last_bounds[1])) return std::array{0,side-1};
            if(maximum<first_bounds[0]||minimum>last_bounds[1]) return std::array{side,-1};
            int lo=0,hi=side;
            while(lo<hi) { const int mid=(lo+hi)/2;
                if(minimum<=bounds(origin+mid,page_side,guard)[1]) hi=mid; else lo=mid+1; }
            const int first=lo;
            lo=0;hi=side;
            while(lo<hi) { const int mid=(lo+hi)/2;
                if(maximum>=bounds(origin+mid,page_side,guard)[0]) lo=mid+1; else hi=mid; }
            return std::array{first,lo-1};
        };
        struct AxisCase { int origin,side; float page_side,guard,center,radius; };
        const std::array cases{
            AxisCase{-17,32,4.0f,0.0625f,-64.0625f,0.0f},
            AxisCase{-17,32,4.0f,0.0625f,-59.9375f,0.0f},
            AxisCase{-17,32,4.0f,0.0625f,-64.0625f-2.0f,2.0f},
            AxisCase{-17,32,4.0f,0.0625f,10000.0f,1.0f},
            AxisCase{-4,8,16.0f,0.25f,-0.25f,0.0f},
            AxisCase{10000000,8,0.0001f,0.0000015625f,1000.0f,0.0f},
            AxisCase{-4,8,16.0f,0.25f,1.0e38f,1.0e38f}
        };
        for(const auto &test:cases)
        {
            const auto selected=range(test.center,test.radius,test.origin,test.side,test.page_side,test.guard);
            for(int local=0;local<test.side;++local)
            {
                const auto page=bounds(test.origin+local,test.page_side,test.guard);
                const bool overlaps=test.center+test.radius>=page[0]&&test.center-test.radius<=page[1];
                Require(!overlaps||(local>=selected[0]&&local<=selected[1]),
                    "VSM sparse candidate range must retain touching, negative, guard, and precision-edge pages");
            }
        }
    }
    const auto surface_shadow_shader=ReadShaderSource("project_hs.hlsl");
    const auto volume_shadow_shader=ReadShaderSource("volumetrics.hlsl");
    const auto probe_capture_shader=ReadShaderSource("local_probe_capture.hlsl");
    Require(surface_shadow_shader.find("smoothstep(0.02, 0.12, edge)")!=std::string::npos&&
            surface_shadow_shader.find("if (any(uv < 0.0) || any(uv > 1.0) || projected.z < 0.0 || projected.z > 1.0)")!=std::string::npos&&
            surface_shadow_shader.find("#ifndef HS_LOCAL_PROBE_CAPTURE")!=std::string::npos&&
            volume_shadow_shader.find("smoothstep(0.02,0.12,edge)")!=std::string::npos&&
            volume_shadow_shader.find("if(any(uv<0.0)||any(uv>1.0)||projected.z<0.0||projected.z>1.0) continue;")!=std::string::npos&&
            probe_capture_shader.find("#define HS_LOCAL_PROBE_CAPTURE 1")!=std::string::npos&&
            probe_capture_shader.find("float HsCaptureStaticShadowVisibility(float3 world)")!=std::string::npos,
            "CPU CSM query clipping must match both surface and volumetric shader contribution domains");
    const auto fixed_root_shader=ReadShaderSource("shadow_data.hlsli");
    Require(fixed_root_shader.find("caster==0.0 || receiver+receiver_bias>=caster")!=std::string::npos&&
            fixed_root_shader.find("max(static_caster,HsShadowRootDynamic.Load(int3(pixel,0)))")!=std::string::npos,
            "fixed-root reverse-Z taps preserve valid-empty and same-tap static/dynamic composition");
    Require(hs::IsCompleteBakedStaticCaster(true,true,true,false,false,false),
            "only immutable, baked, non-deformed source geometry is removed from current root draws");
    Require(!hs::IsCompleteBakedStaticCaster(true,true,true,true,false,false)&&
            !hs::IsCompleteBakedStaticCaster(true,true,true,false,true,false)&&
            !hs::IsCompleteBakedStaticCaster(true,true,true,false,false,true),
            "skin, wind, or animated alpha keeps a baked-tagged caster in the fresh root draw set");
    Require(!hs::IsCompleteBakedStaticCaster(false,true,true,false,false,false)&&
            !hs::IsCompleteBakedStaticCaster(true,false,true,false,false,false)&&
            !hs::IsCompleteBakedStaticCaster(true,true,false,false,false,false),
            "absent, mutable, or unbaked geometry is not classified as complete baked static");
    hs::ShadowRootCoverageDomain root_domain{{1,0,0,0},{0,1,0,0},{0,0,1,0},-10,-10,10,10,-20,20};
    Require(hs::ShadowRootReceiverBoundsValid({100,100,-500,0},{120,130,800,0})&&
            !hs::ShadowRootReceiverBoundsValid({121,100,0,0},{120,130,1,0})&&
            !hs::ShadowRootReceiverBoundsValid({0,0,0,2},{1,1,1,0}),
            "finite ordered receiver records remain valid outside root XY while malformed bounds or classes fail closed");
    Require(hs::ShadowRootContainsXY({0,0,500},root_domain)&&
            !hs::ShadowRootContainsXY({10,0,0},root_domain),
            "root caster admission domain remains guarded while receiver positions can be outside it");
    Require(hs::ShadowRootContainsCaster({0,0,0,1},root_domain)&&
            !hs::ShadowRootContainsCaster({9.5f,0,0,1},root_domain)&&
            !hs::ShadowRootContainsCaster({0,0,20,0},root_domain)&&
            !hs::ShadowRootContainsCaster({0,0,0,std::numeric_limits<float>::infinity()},root_domain),
            "root dynamic admission encloses full caster radius in XY/Z and rejects depth/domain overflow");
    const hs::Float4 admitted_caster{0,0,0,1};
    const hs::Float3 outside_tap{10.25f,0,0};
    const auto [tap_x,tap_error]=hs::ShadowProjectCoordinate(outside_tap,root_domain.right);
    const auto [caster_x,caster_error]=hs::ShadowProjectCoordinate(
        hs::Float3{admitted_caster.x,admitted_caster.y,admitted_caster.z},root_domain.right);
    Require(hs::ShadowRootContainsCaster(admitted_caster,root_domain)&&
            !hs::ShadowRootContainsXY(outside_tap,root_domain)&&
            tap_x-tap_error>caster_x+caster_error+admitted_caster.w,
            "certified fresh-caster bounds cannot occupy an exterior root filter tap, which is valid empty");
    const std::array casters{Caster{{0,0,0,0.25f},0,true,false,true}};
    const std::array receivers{Receiver{{-1,-1,-1,0},{1,1,1,0}}};
    const auto check = [&](const auto &facts, const auto &sources, const auto &domains) {
        return hs::CheckVirtualShadowGuaranteeFacts(facts,sources,domains);
    };
    const auto expect = [&](const auto &facts, Reason reason) {
        const auto result = check(facts,casters,receivers);
        Require(result.csm_required && result.reason == reason, "shadow proof must retain actual CSM with its precise rejection reason");
    };
    auto facts = CompleteFacts();
    auto result = check(facts,casters,receivers);
    Require(!result.csm_required && result.reason == Reason::Guaranteed &&
                result.required_coarse_pages == 64 && result.maximum_coarse_commands == 4,
            "complete coarse slots and current dynamic commands permit CSM omission");
    facts.segment_capacity = 4;
    Require(!check(facts,casters,receivers).csm_required, "exact conservative segment capacity is sufficient");
    facts.segment_capacity = 3;
    expect(facts,Reason::CoarseCommandBudget);
    // The same fixed storage can satisfy an imbalanced actual segment without
    // lending it another segment's addresses or reserving an unused layer.
    facts=CompleteFacts();facts.command_capacity=8;
    const auto demand=hs::ComputeShadowCommandDemand(facts,casters);
    std::array<hs::ShadowCommandSegment,2> partition{};
    Require(hs::PlanShadowCommandSegments(facts.command_capacity,std::span(demand.coarse).first(2),
                std::span(demand.weights).first(2),partition),"coarse demand is reserved before fine-page capacity");
    facts.command_segments=partition;
    Require(partition[0].capacity==0&&partition[1].capacity==8&&partition[1].first==0&&
                !check(facts,casters,receivers).csm_required,
            "empty static segment does not waste the dynamic segment's fixed storage");
    const std::array<std::uint64_t,4> skewed_coarse{0,0,886,4},skewed_weights{0,0,1000,1};
    std::array<hs::ShadowCommandSegment,4> skewed{};
    Require(hs::PlanShadowCommandSegments(3540,skewed_coarse,skewed_weights,skewed)&&
                hs::ValidateShadowCommandSegments(skewed,3540)&&skewed[0].capacity==0&&skewed[1].capacity==0&&
                skewed[2].capacity>=886&&skewed[3].capacity>=4&&skewed.back().first+skewed.back().capacity==3540,
            "fixed3540 storage admits a real886 coarse demand which equal four-way885 division rejects");
    Require(hs::PlanShadowCommandSegments(890,skewed_coarse,skewed_weights,skewed)&&
                skewed[2].capacity==886&&skewed[3].capacity==4,
            "exact total coarse capacity leaves no fine allocation and still preserves every coarse command");
    Require(!hs::PlanShadowCommandSegments(889,skewed_coarse,skewed_weights,skewed)&&
                hs::ValidateShadowCommandSegments(skewed,889),
            "insufficient total storage stays bounded and does not pretend coarse reservation succeeded");
    facts.command_capacity=3;
    Require(!hs::PlanShadowCommandSegments(3,std::span(demand.coarse).first(2),std::span(demand.weights).first(2),partition),
            "genuine total coarse shortage is not fixed by repartitioning");
    Require(check(facts,casters,receivers).reason==Reason::CoarseCommandBudget,
            "CSM remains real when repartitioned coarse reservation is insufficient");
    facts.command_capacity=8;partition={hs::ShadowCommandSegment{0,4},hs::ShadowCommandSegment{3,4}};
    Require(check(facts,casters,receivers).reason==Reason::CoarseCommandBudget,
            "overlapping segment addresses cannot authorize omission even when individual capacities suffice");
    partition={hs::ShadowCommandSegment{0,0},hs::ShadowCommandSegment{1,8}};
    Require(!hs::ValidateShadowCommandSegments(partition,8),"gapped or one-past-end segment metadata is rejected");
    const std::array<std::uint64_t,2> unbounded_coarse{UINT64_MAX,1},unbounded_weights{UINT64_MAX,1};
    Require(!hs::PlanShadowCommandSegments(8,unbounded_coarse,unbounded_weights,partition)&&
                hs::ValidateShadowCommandSegments(partition,8),
            "planning additions and weights cannot overflow the fixed allocation");
    const std::array<std::uint64_t,2> zeros{};
    Require(hs::PlanShadowCommandSegments(8,zeros,zeros,partition)&&
                hs::ValidateShadowCommandSegments(partition,8),"empty current supply has a bounded deterministic layout");
    Require(!hs::PlanShadowCommandSegments(7,{},{},partition)&&
                hs::ValidateShadowCommandSegments(partition,7)&&
                partition.back().first+partition.back().capacity==7,
            "unavailable demand still produces a complete bounded private frame partition");
    Require(!hs::PlanShadowCommandSegments(0,{},{},partition)&&
                hs::ValidateShadowCommandSegments(partition,0)&&partition.back().capacity==0,
            "zero command storage retains a valid empty fallback partition");
    auto baked=casters;baked[0].immutable_source=true;baked[0].skin=false;baked[0].baked_source=true;
    const auto baked_demand=hs::ComputeShadowCommandDemand(CompleteFacts(),baked);
    Require(baked_demand.coarse[0]==0&&baked_demand.weights[0]==1&&!baked_demand.uses_spatial_facts,
            "seeded static supply reserves no duplicate coarse draw but retains its real fine-page population");
    baked[0].wind=true;
    const auto wind_demand=hs::ComputeShadowCommandDemand(CompleteFacts(),baked);
    Require(wind_demand.coarse[1]==4&&wind_demand.weights[1]==1,
            "wind is dynamic despite immutable source or baked tag and reserves actual coarse work");
    auto no_seed=CompleteFacts();no_seed.compatible_bake=false;baked[0].wind=false;
    Require(hs::ComputeShadowCommandDemand(no_seed,baked).coarse[0]==4,
            "incompatible bake cannot erase fallback static draw demand");
    std::array<Caster,128> varied{};std::uint32_t random=0x534f3143u;
    const auto sample=[&]() { random=random*1664525u+1013904223u;return float(random>>8)/float(1u<<24); };
    std::uint64_t reference{};
    for(auto &caster:varied)
    {
        caster.current_bounds={sample()*50-25,sample()*50-25,0,sample()*3+.01f};caster.skin=true;
        const auto &b=caster.current_bounds;
        const double ex=std::abs(double(b.x))*0x1p-20+1.e-5,ey=std::abs(double(b.y))*0x1p-20+1.e-5;
        const double radius=double(b.w)*(1+0x1p-20)+1.e-5,guard=double(CompleteFacts().world_texel)*2;
        for(int py=0;py<8;++py)for(int px=0;px<8;++px)
        {
            const double left=(-4+px)*4.0-guard,bottom=(-4+py)*4.0-guard;
            reference+=!(b.x+ex+radius<left||b.x-ex-radius>left+4+2*guard||
                b.y+ey+radius<bottom||b.y-ey-radius>bottom+4+2*guard);
        }
    }
    const auto varied_demand=hs::ComputeShadowCommandDemand(CompleteFacts(),varied);
    Require(varied_demand.coarse[1]==reference&&varied_demand.weights[1]==varied.size(),
            "factored XY reservation exactly matches the prior64-page guarded predicate across inside/outside/boundary casters");
    std::array mixed{baked[0],varied[0],varied[1],varied[2]};
    mixed[1].immutable_source=mixed[1].baked_source=mixed[1].wind=true;
    mixed[2].immutable_source=mixed[2].baked_source=mixed[2].animated_alpha=true;
    mixed[1].skin=mixed[2].skin=false;
    mixed[3].skin=true;
    const auto verify_split_demand=[&](const hs::ShadowGuaranteeFacts &input) {
        const auto full=hs::ComputeShadowCommandDemand(input,mixed);
        for(std::size_t split=0;split<=mixed.size();++split)
        {
            const auto prefix=hs::ComputeShadowCommandDemand(input,std::span(mixed).first(split));
            const auto combined=hs::ComputeShadowCommandDemand(input,std::span(mixed).subspan(split),prefix);
            Require(combined.coarse==full.coarse&&combined.weights==full.weights&&
                    combined.maximum==full.maximum&&combined.reason==full.reason&&
                    combined.uses_spatial_facts==full.uses_spatial_facts,
                    "retained stable prefix and current suffix preserve exact demand and first-failure semantics");
        }
    };
    verify_split_demand(CompleteFacts());
    auto moved_demand=CompleteFacts();moved_demand.minimum_x+=2;moved_demand.depth.w=40;
    verify_split_demand(moved_demand);
    moved_demand.compatible_bake=false;verify_split_demand(moved_demand);
    Require(hs::ComputeShadowCommandDemand(CompleteFacts(),std::span(mixed).subspan(1,2)).uses_spatial_facts,
            "baked-tagged wind and animated alpha still require current spatial command admission");
    mixed[1].current_bounds.z=1000;verify_split_demand(CompleteFacts());
    mixed[0].current_bounds.w=std::numeric_limits<float>::infinity();verify_split_demand(CompleteFacts());
    facts = CompleteFacts(); facts.physical_pages = 63;
    expect(facts,Reason::CoarseSlotBudget);
    facts = CompleteFacts(); facts.compatible_bake = false;
    expect(facts,Reason::NoCompatibleBake);
    facts = CompleteFacts(); facts.seed_available = false;
    expect(facts,Reason::SeedUnavailable);
    facts = CompleteFacts(); facts.closed_receiver_domains = 0;
    expect(facts,Reason::ReceiverDomainIncomplete);
    facts = CompleteFacts(); facts.window_sides[2] = 7;
    expect(facts,Reason::UnsupportedLayout);
    facts = CompleteFacts(); facts.camera_valid = false;
    expect(facts,Reason::InvalidLightSpace);
    facts = CompleteFacts(); facts.forward = {};
    expect(facts,Reason::InvalidLightSpace);
    facts = CompleteFacts(); facts.draw_group_count = 129;
    expect(facts,Reason::CommandGroupLimit);

    facts = CompleteFacts();
    std::array far_receiver{Receiver{{100,100,0,0},{101,101,1,0}}};
    Require(check(facts,casters,far_receiver).reason == Reason::ReceiverOutsideCoverage,
            "transparent water volume or auxiliary receivers outside coarse coverage keep CSM");
    const auto prepared_demand=hs::ComputeShadowCommandDemand(facts,casters);
    const auto denied_with_demand=hs::CheckVirtualShadowGuaranteeFacts(facts,casters,far_receiver,
        hs::ShadowSurfaceReceiverRange{},&prepared_demand);
    Require(prepared_demand.maximum>0 && denied_with_demand.reason==Reason::ReceiverOutsideCoverage &&
            denied_with_demand.maximum_coarse_commands==prepared_demand.maximum,
            "coverage rejection retains the already prepared command-demand diagnostic");
    far_receiver[0].minimum.w = 1;
    Require(!check(facts,casters,far_receiver).csm_required,
            "private-capture depth-only envelope is not confused with a main-view receiver");
    far_receiver[0].minimum.w = 2;
    Require(check(facts,casters,far_receiver).reason == Reason::InvalidLightSpace,
            "unknown receiver roles cannot authorize omission");

    facts=CompleteFacts();
    const auto camera_check=[&](const auto &f,hs::ShadowSurfaceReceiverRange surfaces) {
        return hs::CheckVirtualShadowGuaranteeFacts(f,std::span<const Caster>{},std::span<const Receiver>{},surfaces);
    };
    Require(!camera_check(facts,{}).csm_required,
            "explicitly closed empty camera domain admits clear-only pages, unlike an unknown domain");
    facts.closed_receiver_domains=0;
    Require(camera_check(facts,{}).reason==Reason::ReceiverDomainIncomplete,
            "missing actual receiver inventory cannot be inferred from an empty caster list");
    facts.required_receiver_domains=facts.closed_receiver_domains=hs::ShadowReceiverWorld;
    Require(camera_check(facts,{}).reason==Reason::ReceiverDomainIncomplete,
            "world-only proof cannot silently omit the mandatory camera surface domain");
    facts=CompleteFacts();facts.required_receiver_domains|=1u<<31;
    Require(camera_check(facts,{}).reason==Reason::ReceiverDomainIncomplete,
            "unknown receiver domains cannot authorize omission");
    facts=CompleteFacts();
    std::array surfaces{hs::ShadowSurfaceReceiver{{-1,-1,-1,0},{1,1,1,1}}};
    Require(!camera_check(facts,{{},surfaces,{}}).csm_required,
            "empty far-frustum space is not a receiver when all actual draw surfaces are closed");
    // Perspective clip volume: +/-x,y <= z; near1/far100. A ground-like
    // receiver intersects only the near portion, despite its wide world AABB.
    facts.receiver_frustum={{{1,0,1,0},{-1,0,1,0},{0,1,1,0},{0,-1,1,0},{0,0,1,-1},{0,0,-1,100}}};
    surfaces[0]={{-100,0,1,0},{100,.01f,4,1}};
    Require(!camera_check(facts,{{},surfaces,{}}).csm_required,
            "actual wide ground surface is clipped to the complete current raster volume, preserving far plane");
    surfaces[0]={{17,0,20,0},{18,1,21,1}};
    Require(!camera_check(facts,{{},surfaces,{}}).csm_required,
            "visible receiver outside both VSM and legacy CSM query volumes remains fully lit");
    std::array<hs::ShadowViewProjection,3> wide_csm{};
    for(auto &matrix:wide_csm)
    {matrix[0]=matrix[5]=1.0f/60.0f;matrix[10]=1.0f/100.0f;matrix[14]=0.5f;matrix[15]=1.0f;}
    facts.csm_query_frusta_valid=hs::BuildShadowCsmQueryFrusta(wide_csm,facts.csm_query_frusta);
    Require(camera_check(facts,{{},surfaces,{}}).reason==Reason::ReceiverOutsideCoverage,
            "visible shadow-off opaque receiver outside coarse still requires CSM without any caster");
    facts=CompleteFacts();surfaces[0]={{100,100,0,0},{101,101,1,1}};
    Require(!camera_check(facts,{{},surfaces,{}}).csm_required,
            "known wholly offscreen surfaces do not invent camera shadow queries");
    facts.receiver_frustum[0][3]=facts.receiver_frustum[1][3]=100;
    surfaces[0]={{200,100,0,0},{201,101,1,1}};
    std::array stable{hs::ShadowSurfaceReceiver{{-1,-1,-1,0},{1,1,1,1}},surfaces[0]};
    std::array aggregate{hs::ShadowSurfaceReceiver{{-1,-1,-1,0},{201,101,1,1}}};
    Require(!camera_check(facts,{stable,{},aggregate}).csm_required,
            "broad static aggregate falls back to retained precise boxes including offscreen members");
    aggregate[0]={{-1,-1,-1,0},{1,1,1,1}};
    Require(!camera_check(facts,{std::span(stable).first(1),{},aggregate}).csm_required,
            "covered retained aggregate admits its complete immutable surface range");
    const std::array world{Receiver{{100,100,0,0},{101,101,1,0}}};
    facts.required_receiver_domains|=hs::ShadowReceiverWorld;facts.closed_receiver_domains|=hs::ShadowReceiverWorld;
    Require(check(facts,std::span<const Caster>{},world).reason==Reason::ReceiverOutsideCoverage,
            "world fog or volume queries outside raster surfaces keep CSM; they are never camera clipped");

    // The frustum is a diamond rotated 45 degrees relative to the VSM axes.
    // Its actual vertices fit the coarse square, while the corners of its
    // enclosing world AABB project outside it. Only the specifically marked
    // AABB may be proven by the exact convex frustum intersection.
    facts=CompleteFacts();
    constexpr float diagonal=0.7071067811865475f;
    facts.right={diagonal,diagonal,0,0};facts.up={-diagonal,diagonal,0,0};
    facts.receiver_frustum={{{1,1,0,20},{-1,-1,0,20},{1,-1,0,20},{-1,1,0,20},
                             {0,0,1,2},{0,0,-1,2}}};
    std::array frustum_aabb{Receiver{{-20,-20,-1,0},{20,20,1,0}}};
    Require(check(facts,std::span<const Caster>{},frustum_aabb).reason==Reason::ReceiverOutsideCoverage,
            "unmarked world bounds retain the conservative all-corners coverage requirement");
    facts.frustum_receiver_index=0;
    Require(!check(facts,std::span<const Caster>{},frustum_aabb).csm_required,
            "marked frustum enclosure proves the actual rotated convex receiver domain");

    // A nonzero legacy CSM query leaves coarse coverage despite a valid marker.
    facts.receiver_frustum={{{1,1,0,20-40*diagonal},{-1,-1,0,20+40*diagonal},
                             {1,-1,0,20},{-1,1,0,20},{0,0,1,2},{0,0,-1,2}}};
    frustum_aabb[0]={{-6,-6,-1,0},{35,35,1,0}};
    facts.csm_query_frusta_valid=hs::BuildShadowCsmQueryFrusta(wide_csm,facts.csm_query_frusta);
    Require(check(facts,std::span<const Caster>{},frustum_aabb).reason==Reason::ReceiverOutsideCoverage,
            "valid marker cannot hide an actual camera-frustum point beyond coarse coverage");

    facts=CompleteFacts();facts.frustum_receiver_index=1;
    Require(check(facts,std::span<const Caster>{},frustum_aabb).reason==Reason::InvalidLightSpace,
            "out-of-range frustum marker conservatively retains CSM");
    facts=CompleteFacts();facts.frustum_receiver_index=0;
    frustum_aabb[0]={{-20,-20,-1,0},{20,20,1,0}};
    auto wrong_marker_class=frustum_aabb;wrong_marker_class[0].minimum.w=1;
    Require(check(facts,std::span<const Caster>{},wrong_marker_class).reason==Reason::InvalidLightSpace,
            "marked receiver must be the ordinary World AABB class");
    wrong_marker_class[0].minimum.w=0;wrong_marker_class[0].maximum.w=1;
    Require(check(facts,std::span<const Caster>{},wrong_marker_class).reason==Reason::InvalidLightSpace,
            "feedback-envelope receivers cannot impersonate the marked World frustum bound");
    auto malformed_marker=frustum_aabb;malformed_marker[0].minimum.x=malformed_marker[0].maximum.x+1;
    Require(check(facts,std::span<const Caster>{},malformed_marker).reason==Reason::InvalidLightSpace,
            "frustum marker does not bypass malformed receiver bounds");

    facts=CompleteFacts();facts.receiver_frustum[0]={};
    Require(camera_check(facts,{{},std::span(stable).first(1),{}}).reason==Reason::InvalidLightSpace,
            "invalid raster planes cannot authorize omission");
    facts=CompleteFacts();
    std::array froxel_enclosure{Receiver{{-20,-20,-100,0},{20,20,100,0}}};
    facts.frustum_receiver_index=0;
    Require(!check(facts,std::span<const Caster>{},froxel_enclosure).csm_required,
            "far froxel AABB portions outside every nonzero CSM query region do not retain CSM");
    facts.minimum_x=0;
    Require(check(facts,std::span<const Caster>{},froxel_enclosure).reason==Reason::ReceiverOutsideCoverage,
            "marked froxels still require coarse VSM coverage where a CSM cascade contributes");
    facts=CompleteFacts();facts.minimum_x=0;
    const std::array crossing_surface{hs::ShadowSurfaceReceiver{{-2,-1,-2,0},{2,1,2,1}}};
    Require(camera_check(facts,{{},std::span<const hs::ShadowSurfaceReceiver>(crossing_surface),{}}).reason==Reason::ReceiverOutsideCoverage,
            "actual camera surface outside coarse coverage inside the legacy CSM domain retains CSM");
    const std::array inside_surface{hs::ShadowSurfaceReceiver{{1,-1,-2,0},{2,1,2,1}}};
    Require(!camera_check(facts,{{},std::span<const hs::ShadowSurfaceReceiver>(inside_surface),{}}).csm_required,
            "surface AABB is clipped to the actual camera and CSM query intersection");
    facts=CompleteFacts();
    auto moved_csm=std::array<hs::ShadowViewProjection,3>{};
    for(auto &matrix:moved_csm)
    {matrix[0]=0.1f;matrix[5]=0.1f;matrix[10]=0.1f;matrix[14]=0.5f;matrix[15]=1.0f;matrix[12]=-3.0f;}
    Require(hs::BuildShadowCsmQueryFrusta(moved_csm,facts.csm_query_frusta),"finite orthographic shadow matrices produce bounded clip planes");
    facts.minimum_x=0;
    Require(!camera_check(facts,{{},std::span<const hs::ShadowSurfaceReceiver>(crossing_surface),{}}).csm_required,
            "receiver wholly outside camera-visible CSM query region uses the legacy fully-lit result");
    facts=CompleteFacts();facts.minimum_x=0;facts.csm_query_frusta_valid=false;
    Require(camera_check(facts,{{},std::span<const hs::ShadowSurfaceReceiver>(crossing_surface),{}}).reason==Reason::InvalidLightSpace,
            "missing or invalid CSM matrices retain CSM for ambiguous receivers");
    auto non_orthographic=std::array<hs::ShadowViewProjection,3>{};
    for(auto &matrix:non_orthographic)
    {matrix[0]=0.1f;matrix[5]=0.1f;matrix[10]=0.1f;matrix[14]=0.5f;matrix[15]=1.0f;matrix[3]=1.0f;}
    Require(!hs::BuildShadowCsmQueryFrusta(non_orthographic,facts.csm_query_frusta),
            "perspective or malformed CSM transforms cannot authorize omission");
    auto identity_csm=std::array<hs::ShadowViewProjection,3>{};
    for(auto &matrix:identity_csm)matrix[0]=matrix[5]=matrix[10]=matrix[15]=1.0f;
    hs::ShadowCsmFrusta identity_frusta{};
    Require(hs::BuildShadowCsmQueryFrusta(identity_csm,identity_frusta)&&
            identity_frusta[0][0][3]>0.96&&identity_frusta[0][1][3]>0.96,
            "CPU query region expands outward past the shader's zero-fade edge threshold");
    auto invalid_role=receivers;invalid_role[0].maximum.w=2;
    Require(check(CompleteFacts(),casters,invalid_role).reason==Reason::InvalidLightSpace,
            "unknown feedback envelope role cannot authorize omission");
    surfaces[0].minimum.w=1;
    Require(camera_check(CompleteFacts(),{{},surfaces,{}}).reason==Reason::InvalidLightSpace,
            "actual camera surfaces cannot masquerade as private depth-only captures");
    const hs::ShadowSurfaceReceiver skin_enclosure{{3.0000002f,-7,-.25f,0},{6,2,9,1}};
    const auto skin_sphere=hs::ShadowSurfaceCasterSphere(skin_enclosure);
    Require(bool(skin_sphere),"qualified skin enclosure supplies a native caster sphere");
    for(unsigned corner=0;corner<8;++corner)
    {
        const double dx=double(corner&1?skin_enclosure.maximum.x:skin_enclosure.minimum.x)-skin_sphere->x;
        const double dy=double(corner&2?skin_enclosure.maximum.y:skin_enclosure.minimum.y)-skin_sphere->y;
        const double dz=double(corner&4?skin_enclosure.maximum.z:skin_enclosure.minimum.z)-skin_sphere->z;
        Require(dx*dx+dy*dy+dz*dz<=double(skin_sphere->w)*skin_sphere->w,
                "same actual-pose caster sphere encloses all source AABB corners after float center rounding");
    }
    auto invalid_skin=skin_enclosure;invalid_skin.maximum.z=std::numeric_limits<float>::infinity();
    Require(!hs::ShadowSurfaceCasterSphere(invalid_skin),"unbounded skin enclosure is never a qualified caster");

    facts=CompleteFacts();
    facts.draw_group_count = 0;
    facts.segment_capacity = 0;
    Require(!check(facts,std::span<const Caster>{},receivers).csm_required,
            "current empty dynamic set still admits required clear-only coarse pages");
    auto source = casters;
    source[0].immutable_source = true; source[0].skin = false; source[0].baked_source = true;
    Require(!check(facts,source,receivers).csm_required,
            "complete baked static supply does not consume runtime raster command budget");
    source[0].baked_source = false;
    Require(check(facts,source,receivers).reason == Reason::CommandGroupLimit,
            "unbaked static raster still requires an admitted real draw group");
    facts = CompleteFacts();
    source = casters; source[0].current_bounds.z = 30;
    Require(check(facts,source,receivers).reason == Reason::CasterOutsideDepth,
            "off-camera caster Z must fit the same complete light-space depth interval");
    source = casters; source[0].current_bounds.w = std::numeric_limits<float>::infinity();
    Require(check(facts,source,receivers).reason == Reason::InvalidLightSpace,
            "invalid caster bounds never become an empty shadow success");
    Require(hs::VirtualShadowCsmReasonName(Reason::CoarseCommandBudget) == "coarse-command-budget",
            "diagnostic rejection names preserve the admission reason");
}
