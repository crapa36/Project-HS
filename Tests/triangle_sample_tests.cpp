#include <hs/render_data/triangle_sample.hpp>
#include <hs/render_data/scene_probe_grid_bake.hpp>
#include <hs/core/cooked_format.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace
{
namespace rd = hs::render_data;

void Require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void Near(double actual, double expected, double tolerance, const char *message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

rd::TriangleSample Sample(const std::array<rd::TriangleClipVertex, 3> &vertices,
                          double x, double y, double width = 640.0,
                          double height = 480.0)
{
    const auto result = rd::SampleTriangleAtNdc(vertices, x, y, width, height);
    Require(result.valid, "expected valid triangle sample");
    return result;
}

double Interpolate(const rd::TriangleSample &sample,
                   const std::array<double, 3> &values)
{
    return sample.barycentric[0] * values[0] +
           sample.barycentric[1] * values[1] +
           sample.barycentric[2] * values[2];
}

void TestEqualW()
{
    const std::array vertices{
        rd::TriangleClipVertex{-1.0, -1.0, 0.2, 1.0},
        rd::TriangleClipVertex{1.0, -1.0, 0.4, 1.0},
        rd::TriangleClipVertex{-1.0, 1.0, 0.8, 1.0}};
    const auto result = Sample(vertices, -0.5, -0.5);
    Near(result.barycentric[0], 0.5, 1e-12, "equal-w b0");
    Near(result.barycentric[1], 0.25, 1e-12, "equal-w b1");
    Near(result.barycentric[2], 0.25, 1e-12, "equal-w b2");
    Near(result.derivative_x[0], -1.0 / 640.0, 1e-14, "equal-w dx0");
    Near(result.derivative_x[1], 1.0 / 640.0, 1e-14, "equal-w dx1");
    Near(result.derivative_x[2], 0.0, 1e-14, "equal-w dx2");
    Near(result.derivative_y[0], 1.0 / 480.0, 1e-14, "equal-w dy0");
    Near(result.derivative_y[1], 0.0, 1e-14, "equal-w dy1");
    Near(result.derivative_y[2], -1.0 / 480.0, 1e-14, "equal-w dy2");
    Near(result.clip_w, 1.0, 1e-12, "equal-w clip w");
    Near(result.ndc_depth, 0.4, 1e-12, "equal-w depth");
}

void TestUnequalWDerivatives()
{
    const std::array vertices{
        rd::TriangleClipVertex{-1.0, -1.0, 0.2, 1.0},
        rd::TriangleClipVertex{2.0, -1.0, 0.6, 2.0},
        rd::TriangleClipVertex{-1.0, 3.0, 0.8, 1.0}};
    constexpr std::array<double, 3> uv_u{0.0, 1.0, 0.0};
    constexpr std::array<double, 3> uv_v{0.0, 0.0, 1.0};
    constexpr double b0 = 0.2;
    constexpr double b1 = 0.3;
    constexpr double b2 = 0.5;
    const double clip_w = b0 * vertices[0].w + b1 * vertices[1].w +
                          b2 * vertices[2].w;
    const double x = (b0 * vertices[0].x + b1 * vertices[1].x +
                      b2 * vertices[2].x) /
                     clip_w;
    const double y = (b0 * vertices[0].y + b1 * vertices[1].y +
                      b2 * vertices[2].y) /
                     clip_w;
    const auto result = Sample(vertices, x, y, 800.0, 600.0);
    Near(result.barycentric[0], b0, 1e-12, "unequal-w b0");
    Near(result.barycentric[1], b1, 1e-12, "unequal-w b1");
    Near(result.barycentric[2], b2, 1e-12, "unequal-w b2");

    constexpr double h = 1e-6;
    const auto left = Sample(vertices, x - h, y, 800.0, 600.0);
    const auto right = Sample(vertices, x + h, y, 800.0, 600.0);
    const auto down = Sample(vertices, x, y - h, 800.0, 600.0);
    const auto up = Sample(vertices, x, y + h, 800.0, 600.0);
    const double finite_u_x =
        (Interpolate(right, uv_u) - Interpolate(left, uv_u)) / (2.0 * h);
    const double finite_u_y =
        (Interpolate(up, uv_u) - Interpolate(down, uv_u)) / (2.0 * h);
    const double finite_v_x =
        (Interpolate(right, uv_v) - Interpolate(left, uv_v)) / (2.0 * h);
    const double finite_v_y =
        (Interpolate(up, uv_v) - Interpolate(down, uv_v)) / (2.0 * h);
    Near(result.derivative_x[1] * 400.0, finite_u_x, 1e-8,
         "unequal-w analytic U dNDCx");
    Near(result.derivative_y[1] * -300.0, finite_u_y, 1e-8,
         "unequal-w analytic U dNDCy");
    Near(result.derivative_x[2] * 400.0, finite_v_x, 1e-8,
         "unequal-w analytic V dNDCx");
    Near(result.derivative_y[2] * -300.0, finite_v_y, 1e-8,
         "unequal-w analytic V dNDCy");
}

void TestCrossingAndZeroW()
{
    const std::array crossing{
        rd::TriangleClipVertex{-1.0, -1.0, 0.1, 1.0},
        rd::TriangleClipVertex{1.0, -1.0, 0.2, -1.0},
        rd::TriangleClipVertex{0.0, 1.0, 0.3, 1.0}};
    constexpr std::array<double, 3> crossing_b{0.6, 0.1, 0.3};
    const double crossing_w = crossing_b[0] * crossing[0].w +
                              crossing_b[1] * crossing[1].w +
                              crossing_b[2] * crossing[2].w;
    const double crossing_x =
        (crossing_b[0] * crossing[0].x + crossing_b[1] * crossing[1].x +
         crossing_b[2] * crossing[2].x) /
        crossing_w;
    const double crossing_y =
        (crossing_b[0] * crossing[0].y + crossing_b[1] * crossing[1].y +
         crossing_b[2] * crossing[2].y) /
        crossing_w;
    const auto crossing_result = Sample(crossing, crossing_x, crossing_y);
    Near(crossing_result.clip_w, crossing_w, 1e-12,
         "negative-w crossing positive sample w");
    for (std::size_t i = 0; i < 3; ++i)
        Near(crossing_result.barycentric[i], crossing_b[i], 1e-12,
             "negative-w crossing barycentric");

    const std::array zero_w{
        rd::TriangleClipVertex{-1.0, -1.0, 0.1, 0.0},
        rd::TriangleClipVertex{1.0, -1.0, 0.3, 1.0},
        rd::TriangleClipVertex{-1.0, 1.0, 0.7, 1.0}};
    const auto zero_w_result = Sample(zero_w, -0.5, 0.0);
    Near(zero_w_result.clip_w, 0.8, 1e-12, "zero-w vertex sample w");
}

void TestDegenerateScaleAndNonfinite()
{
    const std::array degenerate{
        rd::TriangleClipVertex{-1.0, -1.0, 0.0, 1.0},
        rd::TriangleClipVertex{0.0, 0.0, 0.0, 1.0},
        rd::TriangleClipVertex{1.0, 1.0, 0.0, 1.0}};
    Require(!rd::SampleTriangleAtNdc(degenerate, 0.0, 0.0, 640.0, 480.0).valid,
            "exactly degenerate triangle accepted");

    const std::array base{
        rd::TriangleClipVertex{-1.0, -1.0, 0.2, 1.0},
        rd::TriangleClipVertex{2.0, -1.0, 0.6, 2.0},
        rd::TriangleClipVertex{-1.0, 3.0, 0.8, 1.0}};
    std::array scaled = base;
    constexpr double scale = 1e-120;
    for (auto &vertex : scaled)
    {
        vertex.x *= scale;
        vertex.y *= scale;
        vertex.z *= scale;
        vertex.w *= scale;
    }
    const auto original = Sample(base, -1.0 / 13.0, 10.0 / 13.0, 800.0, 600.0);
    const auto tiny = Sample(scaled, -1.0 / 13.0, 10.0 / 13.0, 800.0, 600.0);
    for (std::size_t i = 0; i < 3; ++i)
    {
        Near(tiny.barycentric[i], original.barycentric[i], 1e-12,
             "common tiny scale barycentric");
        Near(tiny.derivative_x[i], original.derivative_x[i], 1e-12,
             "common tiny scale derivative x");
        Near(tiny.derivative_y[i], original.derivative_y[i], 1e-12,
             "common tiny scale derivative y");
    }
    Near(tiny.ndc_depth, original.ndc_depth, 1e-12,
         "common tiny scale depth");
    Near(tiny.clip_w, original.clip_w * scale, 1e-132,
         "common tiny scale clip w");

    auto nonfinite = base;
    nonfinite[0].x = std::numeric_limits<double>::quiet_NaN();
    Require(!rd::SampleTriangleAtNdc(nonfinite, -1.0 / 13.0, 10.0 / 13.0,
                                     800.0, 600.0)
                 .valid,
            "nonfinite vertex accepted");
    Require(!rd::SampleTriangleAtNdc(base, std::numeric_limits<double>::infinity(),
                                     0.0, 800.0, 600.0)
                 .valid,
            "nonfinite target accepted");
    Require(!rd::SampleTriangleAtNdc(base, -1.0 / 13.0, 10.0 / 13.0,
                                     0.0, 600.0)
                 .valid,
            "nonpositive viewport extent accepted");
}

void TestSceneProbeInputIdentity()
{
    const auto asset=hs::MakeAssetId("render/lighting/local_probes.spkg");
    const std::array<std::string,4> dependencies{
        "environment.spkg","layout.spkg","lights.spkg","materials.spkg"};
    rd::SceneProbeDescription description;
    description.edge=16; description.ggx_samples=64; description.revisions={1,2,3,4};
    description.environment_dependency=0; description.static_layout_dependency=1;
    description.light_dependency=2; description.material_dependency=3;
    rd::SceneProbeRecord probe; probe.stable_id=1;
    probe.influence_min=probe.projection_min={-1,-1,-1};
    probe.influence_max=probe.projection_max={1,1,1};
    const std::array probes{probe};
    const std::array<std::string,4> input_digests{
        std::string(64,'1'),std::string(64,'2'),std::string(64,'3'),std::string(64,'4')};
    const std::array inputs{
        rd::SceneProbeRevisionInput{"environment.spkg",input_digests[0]},
        rd::SceneProbeRevisionInput{"layout.spkg",input_digests[1]},
        rd::SceneProbeRevisionInput{"lights.spkg",input_digests[2]},
        rd::SceneProbeRevisionInput{"materials.spkg",input_digests[3]}};
    const auto layout=rd::BuildSceneProbePackage(asset,dependencies,description,probes);
    const auto layout_view=rd::ReadSceneProbePackage(layout,asset);
    const auto identity=rd::DeriveSceneProbeInputIdentity(layout_view.description,layout_view.probes,
        layout_view.dependencies,inputs);

    auto baked_description=description; baked_description.revisions={41,42,43,44};
    const std::vector<hs::Float4> raw(6*16*16,{1,1,1,1});
    const std::vector<hs::Float4> filtered(6*(256+64+16+4+1),{0.5f,0.5f,0.5f,1});
    const std::vector<hs::Float4> sh(9,{0.25f,0.25f,0.25f,0});
    const std::vector<float> distance(raw.size(),probe.capture_distance);
    const auto baked=rd::BuildSceneProbePackage(asset,dependencies,baked_description,probes,
        {raw,filtered,sh,distance});
    const auto baked_view=rd::ReadSceneProbePackage(baked,asset);
    Require(baked_view.payload==rd::SceneProbePayload::Baked,"fixture must contain a baked reflection fixture");
    Require(rd::DeriveSceneProbeInputIdentity(baked_view.description,baked_view.probes,
        baked_view.dependencies,inputs)==identity,
        "baked payload bytes and replacement revisions must not affect probe input identity");

    auto grid_description=description;
    grid_description.grid={{-1,.5f,-1},{2,1,2},{2,2,2}};
    std::vector<hs::Float4> grid_sh(8*9);
    grid_sh[7*9]={2,1,.25f,0};
    auto previous_sh=grid_sh;
    previous_sh[1]={-.1f,.02f,-.03f,0};
    auto next_sh=previous_sh;
    Require(rd::CompareSceneProbeIterations(previous_sh,next_sh,1e-6,1e-4)==rd::SceneProbeIterationState::Converged,
        "unchanged signed SH iteration must converge");
    next_sh[1].x+=.01f;
    Require(rd::CompareSceneProbeIterations(previous_sh,next_sh,1e-6,1e-4)==rd::SceneProbeIterationState::Unconverged,
        "changed reflected radiance must not converge prematurely");
    next_sh[0].x=-1;
    Require(rd::CompareSceneProbeIterations(previous_sh,next_sh,1e-6,1e-4)==rd::SceneProbeIterationState::Invalid,
        "negative integral energy must reject the iteration");
    rd::SceneProbeCell cell;
    cell.reachable.fill(0x0f);
    cell.reachable.back()=0xf0;
    cell.reflections[0]=1;
    const auto grid_bytes=rd::BuildSceneProbePackage(asset,dependencies,grid_description,probes,
        {raw,filtered,grid_sh,{},std::span(&cell,1)});
    const auto grid=rd::ReadSceneProbePackage(grid_bytes,asset);
    Require(grid.cosine_sh.size()==grid_sh.size()*sizeof(hs::Float4) && grid.first_hit_distance.empty() &&
        grid.cells.size()==1 && grid.cells[0].reachable==cell.reachable && grid.cells[0].reflections==cell.reflections,
        "independent grid SH and subcell wall/reflector masks must survive publication");
    Require(rd::DeriveSceneProbeInputIdentity(grid.description,grid.probes,dependencies,inputs)!=identity,
        "grid extent and capture placement must affect semantic identity");
    bool rejected=false;
    try { (void)rd::BuildSceneProbePackage(asset,dependencies,grid_description,probes,{raw,filtered,sh,{},std::span(&cell,1)}); }
    catch(const std::runtime_error &) { rejected=true; }
    Require(rejected,"a reflection SH set cannot substitute for independent grid samples");

    // Independent analytic scene: a vertical wall bisects the two-metre cell.
    // Direct each measured intersection to the production receiver visibility test.
    grid_description.grid={{0,0,0},{2,2,2},{2,2,2}};
    const auto edge=grid_description.edge;
    std::vector<float> wall_hits(8*6*edge*edge,100.f);
    for(unsigned node=0;node<8;++node) for(unsigned face=0;face<6;++face)
        for(unsigned v=0;v<edge;++v) for(unsigned u=0;u<edge;++u)
        {
            const float a=2*(float(u)+.5f)/edge-1,b=2*(float(v)+.5f)/edge-1;
            const float ray_x=(face==0?1.f:face==1?-1.f:face==5?-a:a)/std::sqrt(1+a*a+b*b);
            const float hit=(1-grid_description.grid.Position(node).x)/ray_x;
            if(hit>0) wall_hits[((node*6+face)*edge+v)*edge+u]=hit;
        }
    const auto walls=rd::BakeSceneProbeCells(grid_description,probes,wall_hits);
    Require(walls.size()==1 && walls[0].reflections[0]==1 && walls[0].reachable[20]==0x55 &&
        walls[0].reachable[23]==0xaa,"irradiance corners must stay on the receiver side of a wall");
    // A floor below the grid cannot block any segment between nodes and receivers.
    auto floor_grid=grid_description;
    floor_grid.grid.origin.y=.5f;
    std::vector<float> floor_hits(wall_hits.size(),100.f);
    for(unsigned node=0;node<8;++node) for(unsigned face=0;face<6;++face)
        for(unsigned v=0;v<edge;++v) for(unsigned u=0;u<edge;++u)
        {
            const float a=2*(float(u)+.5f)/edge-1,b=2*(float(v)+.5f)/edge-1;
            const float ray_y=(face==2?1.f:face==3?-1.f:-b)/std::sqrt(1+a*a+b*b);
            const float hit=-floor_grid.grid.Position(node).y/ray_y;
            if(hit>0) floor_hits[((node*6+face)*edge+v)*edge+u]=hit;
        }
    const auto floor=rd::BakeSceneProbeCells(floor_grid,probes,floor_hits);
    for(auto mask:floor[0].reachable)
        Require(mask==0xff,"a floor below receivers must not cast voxel-sized shadows");

    // Only node zero sees a wall: other corner bits must remain reachable.
    auto single_wall=wall_hits;
    std::fill(single_wall.begin()+6*edge*edge,single_wall.end(),100.f);
    const auto one_sided=rd::BakeSceneProbeCells(grid_description,probes,single_wall);
    Require(one_sided[0].reachable[20]==0xff && one_sided[0].reachable[23]==0xfe,
        "a single occluded node must not remove other corners");

    // A small sphere near a receiver subcell corner misses its center ray.
    std::vector<float> grazing_hits(wall_hits.size(),100.f);
    for(unsigned face=0;face<6;++face) for(unsigned v=0;v<edge;++v) for(unsigned u=0;u<edge;++u)
    {
        const float a=2*(float(u)+.5f)/edge-1,b=2*(float(v)+.5f)/edge-1;
        const std::array<std::array<float,3>,6> directions{{{1,-b,-a},{-1,-b,a},{a,1,b},{a,-1,-b},{a,-b,1},{-a,-b,-1}}};
        const auto &d=directions[face];
        const float inv=1/std::sqrt(1+a*a+b*b);
        const float projection=(d[0]*.45f+d[1]*.25f+d[2]*.45f)*inv;
        const float discriminant=projection*projection-(.45f*.45f+.25f*.25f+.45f*.45f-.08f*.08f);
        if(projection>0 && discriminant>=0)
            grazing_hits[(face*edge+v)*edge+u]=projection-std::sqrt(discriminant);
    }
    const auto grazing=rd::BakeSceneProbeCells(grid_description,probes,grazing_hits);
    Require((grazing[0].reachable[0]&1u)!=0,
        "a prop grazing a subcell corner must not hide its center from the probe");
    std::fill_n(wall_hits.begin(),6*edge*edge,-1.f);
    const auto embedded=rd::BakeSceneProbeCells(grid_description,probes,wall_hits);
    for(auto mask:embedded[0].reachable)
        Require(!(mask&1u),"a probe inside an opaque shell cannot illuminate its neighbors");

    auto changed=probe; changed.weight=0.5f;
    const std::array changed_probes{changed};
    Require(rd::DeriveSceneProbeInputIdentity(description,changed_probes,dependencies,inputs)!=identity,
        "semantic probe record changes must affect probe input identity");
    auto changed_description=description; changed_description.edge=32;
    Require(rd::DeriveSceneProbeInputIdentity(changed_description,probes,dependencies,inputs)!=identity,
        "capture description changes must affect probe input identity");
    const std::string changed_digest(64,'a');
    auto changed_inputs=inputs; changed_inputs[1].sha256=changed_digest;
    Require(rd::DeriveSceneProbeInputIdentity(description,probes,dependencies,changed_inputs)!=identity,
        "source dependency changes must affect probe input identity");
}
} // namespace

int main()
{
    try
    {
        TestEqualW();
        TestUnequalWDerivatives();
        TestCrossingAndZeroW();
        TestDegenerateScaleAndNonfinite();
        TestSceneProbeInputIdentity();
        std::puts("Render-data CPU tests passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
