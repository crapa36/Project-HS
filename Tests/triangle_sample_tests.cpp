#include <hs/render_data/triangle_sample.hpp>
#include <hs/render_data/scene_probe.hpp>
#include <hs/core/cooked_format.hpp>

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
    Require(baked_view.payload==rd::SceneProbePayload::Baked,"fixture must contain real four-payload SPB1");
    Require(rd::DeriveSceneProbeInputIdentity(baked_view.description,baked_view.probes,
        baked_view.dependencies,inputs)==identity,
        "baked payload bytes and replacement revisions must not affect probe input identity");

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
