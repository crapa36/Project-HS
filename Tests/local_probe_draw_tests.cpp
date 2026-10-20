#include "renderer_local_probe_draws.hpp"
#include "renderer_quality.hpp"

#include <iostream>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
using namespace hs;
using Matrix = std::array<float, 16>;
using Point = std::array<double, 3>;
using Source = std::pair<std::uint32_t, std::uint32_t>;
void Check(bool valid, const char *message)
{
    if (!valid) throw std::runtime_error(message);
}
Matrix Identity()
{
    return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}
Point Cross(Point a, Point b)
{
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
double Dot(Point a, Point b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Matrix CubeMatrix(Point eye, Point forward)
{
    Point up = std::abs(forward[1]) > .95 ? Point{0,0,1} : Point{0,1,0};
    auto right = Cross(up, forward);
    const auto length = std::sqrt(Dot(right, right));
    for (auto &v : right) v /= length;
    up = Cross(forward, right);
    Matrix view{};
    for (std::size_t row=0;row<3;++row)
    {
        view[row*4]=static_cast<float>(right[row]);
        view[row*4+1]=static_cast<float>(up[row]);
        view[row*4+2]=static_cast<float>(forward[row]);
    }
    view[12]=static_cast<float>(-Dot(eye,right));
    view[13]=static_cast<float>(-Dot(eye,up));
    view[14]=static_cast<float>(-Dot(eye,forward));view[15]=1;
    // Native 90-degree LH perspective, near=1/far=0 reversed depth.
    constexpr double near=.05,far=50;
    Matrix projection{};
    projection[0]=projection[5]=1;
    projection[10]=static_cast<float>(near/(near-far));
    projection[11]=1;
    projection[14]=static_cast<float>(near*far/(far-near));
    Matrix result{};
    for (std::size_t row=0;row<4;++row)
        for (std::size_t column=0;column<4;++column)
        {
            double value{};
            for (std::size_t inner=0;inner<4;++inner)
                value+=double(view[row*4+inner])*projection[inner*4+column];
            result[row*4+column]=static_cast<float>(value);
        }
    return result;
}
void TestClipEdgesAndInvalidInputs()
{
    auto matrix=Identity();
    auto frustum=MakeLocalProbeFrustum(matrix);
    Check(frustum.valid,"identity clip planes are invalid");
    constexpr std::array outside{Point{-1.25,0,.5},Point{1.25,0,.5},
        Point{0,-1.25,.5},Point{0,1.25,.5},Point{0,0,-.25},Point{0,0,1.25}};
    for (auto center:outside)
        Check(!LocalProbeIntersects({center,.125},frustum),"outside native clip plane retained unexpectedly");
    constexpr std::array grazing{Point{-1.125,0,.5},Point{1.125,0,.5},
        Point{0,-1.125,.5},Point{0,1.125,.5},Point{0,0,-.125},Point{0,0,1.125}};
    for (auto center:grazing)
        Check(LocalProbeIntersects({center,.125},frustum),"grazing sphere was rejected");
    matrix[10]=-1;matrix[14]=1;
    Check(!LocalProbeIntersects({{0,0,1.25},.125},MakeLocalProbeFrustum(matrix)),
        "reversed depth lost the near/far clip constraints");
    const double infinity=std::numeric_limits<double>::infinity();
    Check(LocalProbeIntersects({{0,0,0},infinity},frustum),"unknown radius must fail open");
    Check(LocalProbeIntersects({{infinity,0,0},1},frustum),"nonfinite center must fail open");
    Check(LocalProbeIntersects({{100,0,0},-1},frustum),"invalid radius must fail open");
    matrix[0]=std::numeric_limits<float>::quiet_NaN();
    Check(LocalProbeIntersects({{100,0,0},1},MakeLocalProbeFrustum(matrix)),
        "nonfinite matrix must fail open");
    Check(LocalProbeIntersects({{100,0,0},1},MakeLocalProbeFrustum({})),
        "degenerate matrix must fail open");
    constexpr std::array directions{Point{1,0,0},Point{-1,0,0},Point{0,1,0},
        Point{0,-1,0},Point{0,0,1},Point{0,0,-1}};
    const Point eye{3,-2,5};
    for (auto direction:directions)
    {
        const auto face=MakeLocalProbeFrustum(CubeMatrix(eye,direction));
        Check(face.valid,"own translated cube face planes are invalid");
        Point inside{},behind{};
        for(std::size_t axis=0;axis<3;++axis)
        {
            inside[axis]=eye[axis]+direction[axis]*5;
            behind[axis]=eye[axis]-direction[axis]*5;
        }
        Check(LocalProbeIntersects({inside,.01},face),"own cube face rejected a visible point");
        Check(!LocalProbeIntersects({behind,.01},face),"own cube face retained a point behind it");
    }
}
void TestWorldBounds()
{
    RenderInstance instance{};
    instance.mesh=RenderMesh::Rock;
    instance.position={3,4,5};instance.scale={-2,3,-4};instance.yaw=2;
    const auto bounds=LocalProbeWorldSphere(instance,{1,2,3,2});
    Check(bounds.center==Point{3,4,5},"environment source origin changed");
    Check(bounds.radius>=std::hypot(2.0,6.0,12.0)+8,"negative/nonuniform scale underestimated");
    instance.mesh=RenderMesh::Ground;
    Check(LocalProbeWorldSphere(instance,{0,0,0,1}).center[1]==5.5,
        "terrain bounds lost the native half-height offset");
    instance.mesh=RenderMesh::Grass;instance.position={1.03f,.5f,.5f};instance.scale={1,1,1};
    const auto grass=LocalProbeWorldSphere(instance,{0,0,0,0});
    Check(LocalProbeIntersects(grass,MakeLocalProbeFrustum(Identity())),
        "fixed capture wind was clipped at the face edge");
    instance.mesh=RenderMesh::GroundDetail;
    for (std::uint32_t variant=0; variant<kGroundDetailVariantCount; ++variant)
    {
        instance.environment_variant=variant;
        const bool expected_wind=variant<=3 || variant==7;
        Check(HasEnvironmentWind(instance.mesh,variant)==expected_wind,
            "only flowers, clover and shrubs deform; props remain rigid");
        Check(LocalProbeIntersects(LocalProbeWorldSphere(instance,{0,0,0,0}),
            MakeLocalProbeFrustum(Identity()))==expected_wind,
            "ground-detail capture bounds must follow its wind envelope");
    }
    instance.mesh=RenderMesh::Rock;
    Check(!LocalProbeIntersects(LocalProbeWorldSphere(instance,{0,0,0,0}),MakeLocalProbeFrustum(Identity())),
        "non-grass zero-radius point gained the grass envelope");
    const auto finite=instance;
    instance.scale.x=std::numeric_limits<float>::infinity();
    Check(!std::isfinite(LocalProbeWorldSphere(instance,{0,0,0,1}).radius),
        "nonfinite transform must retain an unknown sphere");
    instance=finite;instance.scale={1,1,1};
    instance.position.x=std::numeric_limits<float>::max()*.75f;
    Check(!std::isfinite(LocalProbeWorldSphere(instance,{0,0,0,
        std::numeric_limits<float>::max()*.5f}).radius),"derived float vertex overflow must fail open");
    instance=finite;instance.yaw=std::numeric_limits<float>::quiet_NaN();
    Check(!std::isfinite(LocalProbeWorldSphere(instance,{0,0,0,1}).radius),"nonfinite yaw must fail open");
    instance=finite;instance.mesh=RenderMesh::Archer;
    Check(!std::isfinite(LocalProbeWorldSphere(instance,{0,0,0,1}).radius),"unknown capture geometry must fail open");
    instance=finite;
    Check(!std::isfinite(LocalProbeWorldSphere(instance,{0,0,0,-1}).radius),"negative page radius must fail open");
    Check(!std::isfinite(LocalProbeWorldSphere(instance,{0,0,0,
        std::numeric_limits<float>::quiet_NaN()}).radius),"nonfinite page radius must fail open");
}
// Independent, slow transformed-corner reference: a reject is safe only if
// every complete-page/wind-envelope corner lies outside one native clip plane.
bool CornersOutside(const RenderInstance &source, Point lower, Point upper, const Matrix &matrix)
{
    std::array<double,6> maximum;maximum.fill(-std::numeric_limits<double>::infinity());
    const double sine=std::sin(double(source.yaw)),cosine=std::cos(double(source.yaw));
    const bool terrain=source.mesh==RenderMesh::Ground || source.mesh==RenderMesh::DirtPatch;
    const double wind=(source.mesh==RenderMesh::Grass ||
        (source.mesh==RenderMesh::GroundDetail && (source.environment_variant<=3 || source.environment_variant==7)))?.035:0;
    for(unsigned corner=0;corner<8;++corner)
        for(unsigned bend=0;bend<(wind>0?4u:1u);++bend)
        {
            const double x=((corner&1)?upper[0]:lower[0])*source.scale.x;
            const double y=((corner&2)?upper[1]:lower[1])*source.scale.y;
            const double z=((corner&4)?upper[2]:lower[2])*source.scale.z;
            const std::array<double,4> world{source.position.x+x*cosine+z*sine+((bend&1)?wind:-wind),
                source.position.y+y+(terrain?source.scale.y*.5:0),
                source.position.z-x*sine+z*cosine+((bend&2)?wind*.7:-wind*.7),1};
            std::array<double,4> clip{};
            for(std::size_t column=0;column<4;++column)
                for(std::size_t row=0;row<4;++row) clip[column]+=world[row]*matrix[row*4+column];
            const std::array distance{clip[3]+clip[0],clip[3]-clip[0],clip[3]+clip[1],
                clip[3]-clip[1],clip[2],clip[3]-clip[2]};
            for(std::size_t plane=0;plane<6;++plane) maximum[plane]=std::max(maximum[plane],distance[plane]);
        }
    return std::ranges::any_of(maximum,[](double value){return value<0;});
}
void TestRandomConservativeCorners()
{
    std::mt19937 random(0x50524f42);
    const auto unit=[&]{return double(random()%100000)/100000.0;};
    constexpr std::array directions{Point{1,0,0},Point{-1,0,0},Point{0,1,0},
        Point{0,-1,0},Point{0,0,1},Point{0,0,-1}};
    std::size_t rejected{};
    for(std::size_t test=0;test<3000;++test)
    {
        RenderInstance source{};
        source.mesh=test%4==0?RenderMesh::Grass:test%4==1?RenderMesh::Rock:
            test%4==2?RenderMesh::Ground:RenderMesh::GroundDetail;
        source.environment_variant=static_cast<std::uint32_t>(test/4)%kGroundDetailVariantCount;
        source.position={float(unit()*160-80),float(unit()*20-10),float(unit()*160-80)};
        source.scale={float(unit()*8-4),float(unit()*8-4),float(unit()*8-4)};
        source.yaw=float(unit()*20-10);
        Point lower{},upper{},center{},extent{};
        for(std::size_t axis=0;axis<3;++axis)
        {
            center[axis]=float(unit()*2-1);extent[axis]=float(unit()*2+.01);
            lower[axis]=center[axis]-extent[axis];upper[axis]=center[axis]+extent[axis];
        }
        const std::array<float,4> local{float(center[0]),float(center[1]),float(center[2]),
            float(std::hypot(extent[0],extent[1],extent[2]))};
        const auto sphere=LocalProbeWorldSphere(source,local);
        const Point eye{unit()*10-5,unit()*10-5,unit()*10-5};
        for(auto direction:directions)
        {
            const auto matrix=CubeMatrix(eye,direction);
            if(!LocalProbeIntersects(sphere,MakeLocalProbeFrustum(matrix)))
            {
                ++rejected;
                Check(CornersOutside(source,lower,upper,matrix),"conservative cull removed a possible raster hit");
            }
        }
    }
    Check(rejected>1000,"random oracle did not exercise meaningful culling");
}
std::vector<Source> ExpandRuns(std::span<const LocalProbeDraw> draws,const LocalProbeFrustum *frustum,
                              std::vector<LocalProbeDrawRun> *runs=nullptr)
{
    std::vector<Source> result;
    ForEachLocalProbeDrawRun(draws,frustum,[&](const LocalProbeDrawRun &run)
    {
        if(runs) runs->push_back(run);
        for(std::uint32_t instance=0;instance<run.instance_count;++instance)
            result.emplace_back(run.first_instance+instance,run.material_ordinal);
    });
    return result;
}
void TestSharedMaterialDifferentPages()
{
    const LocalProbeDrawSphere inside{{0,0,.5},.01};
    const std::array<LocalProbeDraw,5> draws{{
        {0,15,inside,14},{1,15,inside,14},{2,15,inside,15},
        {3,15,inside,20},{4,15,inside,20}}};
    std::vector<LocalProbeDrawRun> runs;
    const auto expanded=ExpandRuns(draws,nullptr,&runs);
    Check(expanded==std::vector<Source>{{0,15},{1,15},{2,15},{3,15},{4,15}},
        "shared material changed source/material order");
    Check(runs.size()==3 && runs[0].page_ordinal==14 && runs[0].instance_count==2 &&
        runs[1].page_ordinal==15 && runs[1].instance_count==1 &&
        runs[2].page_ordinal==20 && runs[2].instance_count==2,
        "different geometry pages with one material were merged into one native draw");
    for(const auto &run:runs)
        Check(LocalProbeDrawRunValid(run,draws.size()) && (LocalProbePassValue(run)&15u)==15u,
            "geometry ordinal escaped into the four-bit shared material binding");
    RenderInstance detail{};detail.mesh=RenderMesh::GroundDetail;detail.position={2,0,3};
    detail.scale={1,1,1};
    const auto sphere=LocalProbeWorldSphere(detail,{0,.1f,0,.5f});
    Check(std::isfinite(sphere.radius) && sphere.radius>=.5,
        "appended detail semantic did not receive conservative complete-page bounds");
}

void TestPackedSourceBase()
{
    for(std::uint32_t ordinal=0;ordinal<16;++ordinal)
        for(std::uint32_t first:{0u,1u,17u,kLocalProbeSourceCapacity-3u})
        {
            const LocalProbeDrawRun run{first,3u,ordinal};
            Check(LocalProbeDrawRunValid(run,kLocalProbeSourceCapacity),"valid packed run rejected");
            const auto packed=LocalProbePassValue(run);
            Check((packed&0xc0000000u)==0u && (packed&15u)==ordinal && (packed>>4u)==first,
                "packed source/material escaped its bit contract");
            for(std::uint32_t local=0;local<run.instance_count;++local)
                Check((packed>>4u)+local==first+local,"draw-local source base changed an instance slot");
        }
    Check(!LocalProbeDrawRunValid({0,1,0},0),"empty source accepted a run");
    Check(!LocalProbeDrawRunValid({0,0,0},1),"empty run accepted");
    Check(!LocalProbeDrawRunValid({0,1,16},1),"material ordinal overflow accepted");
    Check(!LocalProbeDrawRunValid({kLocalProbeSourceCapacity,1,0},kLocalProbeSourceCapacity),"base overflow accepted");
    Check(!LocalProbeDrawRunValid({kLocalProbeSourceCapacity-1u,2,0},kLocalProbeSourceCapacity),"tail overflow accepted");
    Check(!LocalProbeDrawRunValid({0,1,0},std::uint64_t(kLocalProbeSourceCapacity)+1u),"full source overflow accepted");
    Check(!LocalProbeDrawRunValid({1,UINT32_MAX,0},kLocalProbeSourceCapacity),"run length wrapped its source bound");
}
void TestDrawOrderAndDensity()
{
    const LocalProbeDrawSphere inside{{0,0,.5},.01},outside{{100,0,.5},.01};
    std::vector<LocalProbeDraw> draws{{0,2,inside},{1,2,inside},{3,2,inside},
        {4,2,outside},{5,2,inside},{6,3,inside},{7,3,inside},{8,2,inside}};
    const auto frustum=MakeLocalProbeFrustum(Identity());
    std::vector<LocalProbeDrawRun> runs;
    const auto visible=ExpandRuns(draws,&frustum,&runs);
    Check(visible==std::vector<Source>{{0,2},{1,2},{3,2},{5,2},{6,3},{7,3},{8,2}},
        "instancing changed source/material order or crossed a density/culling hole");
    Check(runs.size()==5 && runs[0].instance_count==2 && runs[3].instance_count==2,
        "contiguous identical ordinal draws were not combined");
    Check(ExpandRuns(draws,nullptr)==std::vector<Source>{{0,2},{1,2},{3,2},{4,2},{5,2},{6,3},{7,3},{8,2}},
        "full-scene shadow omitted a retained source");
    Check(ExpandRuns({},&frustum).empty(),"empty source retained a previous run");
    std::mt19937 random(0x44524157);
    for(std::size_t test=0;test<300;++test)
    {
        std::vector<RenderInstance> sources(random()%512);
        for(std::size_t index=0;index<sources.size();++index)
        {
            sources[index].mesh=index%3?RenderMesh::Grass:RenderMesh::Rock;
            sources[index].stable_id=index%5?index+1:0;
            sources[index].vfx_owner_id=index%17?0:index+1;
        }
        for(float density:{0.0f,.35f,.75f,1.0f})
        {
            draws.clear();std::vector<Source> expected,all;
            for(std::size_t index=0;index<sources.size();++index)
            {
                if(!RetainDecorativeFoliage(sources[index],index,density)) continue;
                const auto ordinal=std::uint32_t(random()%15);
                const bool present=random()%3!=0;
                draws.push_back({std::uint32_t(index),ordinal,present?inside:outside});
                all.emplace_back(std::uint32_t(index),ordinal);
                if(present) expected.emplace_back(std::uint32_t(index),ordinal);
            }
            Check(ExpandRuns(draws,&frustum)==expected,"random source/density/culling order changed");
            Check(ExpandRuns(draws,nullptr)==all,"shadow source order or foliage identity changed");
        }
    }
    draws={{UINT32_MAX,1,inside},{0,1,inside}};
    runs.clear();(void)ExpandRuns(draws,nullptr,&runs);
    Check(runs.size()==2 && runs[0].instance_count==1 && runs[1].instance_count==1,
        "source arithmetic wrapped into an instanced run");
}
} // namespace
int main()
{
    try
    {
        TestClipEdgesAndInvalidInputs();TestWorldBounds();TestRandomConservativeCorners();TestDrawOrderAndDensity();TestSharedMaterialDifferentPages();TestPackedSourceBase();
        std::cout<<"local probe draws: clip edges, invalid inputs, wind/terrain, corner oracle and exact source order passed\n";
        return 0;
    }
    catch(const std::exception &error)
    {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
