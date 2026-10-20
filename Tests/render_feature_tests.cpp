#include "renderer_post_subgraphs.hpp"
#include "renderer_feature_resolve.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace
{
using namespace hs;

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

ResourceVersion Texture(std::uint32_t id, std::uint32_t version = 0)
{
    return {{id, ResourceKind::Texture}, version};
}

bool SameVersion(ResourceVersion left, ResourceVersion right)
{
    return left.resource.value == right.resource.value &&
           left.resource.kind == right.resource.kind && left.ordinal == right.ordinal;
}

struct PostFixture
{
    PostSubgraphInputs inputs{false, false,
        Texture(1), Texture(2), Texture(3), Texture(4), Texture(5), Texture(6),
        Texture(7), Texture(8), Texture(9)};
    PostSubgraphTargets targets{Texture(10), Texture(11), Texture(12), Texture(13),
        Texture(2), Texture(14), Texture(15)};
};

const PostPassPlan &Pass(const PostSubgraphPlan &plan, std::uint32_t id)
{
    const auto found = std::ranges::find(plan.passes, id, &PostPassPlan::id);
    Check(found != plan.passes.end(), "expected configured post pass");
    return *found;
}

void ConfiguredPostTopology()
{
    for (const bool enable_bloom : {false, true})
        for (const bool enable_outline : {false, true})
        {
            PostFixture fixture;
            fixture.inputs.bloom = enable_bloom;
            fixture.inputs.outline = enable_outline;
            PostSubgraphPlan plan;
            Check(CompilePostSubgraphs(fixture.inputs, fixture.targets, plan).Succeeded(), "compile post topology");
            Check(plan.passes.size() == 4u + (enable_bloom ? 4u : 0u) + (enable_outline ? 1u : 0u),
                  "disabled features omit their actual GPU passes");
            for (std::uint32_t id = 10; id <= 13; ++id)
                Check((std::ranges::find(plan.passes, id, &PostPassPlan::id) != plan.passes.end()) == enable_bloom,
                      "bloom subgraph is present only when enabled");
            Check((std::ranges::find(plan.passes, 15u, &PostPassPlan::id) != plan.passes.end()) == enable_outline,
                  "outline subgraph is present only when enabled");
            const auto tone_input = enable_bloom ? Pass(plan, 13).output : Pass(plan, 9).output;
            Check(Pass(plan, 14).reads.size() == 1 && SameVersion(Pass(plan, 14).reads[0].version, tone_input),
                  "tone mapping consumes bloom output or bypasses to temporal output");
            const auto aa_input = enable_outline ? Pass(plan, 15).output : Pass(plan, 14).output;
            Check(Pass(plan, 16).reads.size() == 1 && SameVersion(Pass(plan, 16).reads[0].version, aa_input),
                  "FXAA consumes outline output or bypasses to tone mapping");
            Check(Pass(plan, 17).read_write && SameVersion(Pass(plan, 17).predecessor, Pass(plan, 16).output),
                  "UI loads and blends into the produced back buffer version");
            Check(SameVersion(plan.outputs.temporal, Texture(10, 1)) &&
                  SameVersion(plan.outputs.post_a, Texture(2, 1)) &&
                  SameVersion(plan.outputs.post_b, Texture(14, static_cast<std::uint32_t>(enable_bloom) +
                                                             static_cast<std::uint32_t>(enable_outline))) &&
                  SameVersion(plan.outputs.back_buffer, Texture(15, 2)),
                  "post outputs preserve physical ping-pong targets and final versions");
        }

    PostFixture fixture;
    fixture.inputs.bloom = true;
    fixture.targets.temporal.ordinal = 4;
    fixture.targets.post_a.ordinal = 3;
    fixture.inputs.composite_hdr.ordinal = 3;
    fixture.targets.post_b.ordinal = 5;
    fixture.targets.back_buffer.ordinal = 7;
    PostSubgraphPlan plan;
    Check(CompilePostSubgraphs(fixture.inputs, fixture.targets, plan).Succeeded(), "continue existing resource versions");
    Check(plan.outputs.temporal.ordinal == 5 && plan.outputs.post_a.ordinal == 4 &&
          plan.outputs.post_b.ordinal == 6 && plan.outputs.back_buffer.ordinal == 9,
          "post composition continues prior frame graph writes without resetting ordinals");
    fixture.targets.temporal.ordinal = UINT32_MAX;
    Check(!CompilePostSubgraphs(fixture.inputs, fixture.targets, plan) && plan.passes.empty(),
          "invalid version cannot publish a partial post plan");
    fixture.targets.temporal.ordinal = 0;
    fixture.targets.post_a.ordinal = 2;
    Check(!CompilePostSubgraphs(fixture.inputs, fixture.targets, plan),
          "post target cannot disconnect the physical composite version");
    fixture.targets.post_a.ordinal = fixture.inputs.composite_hdr.ordinal;
    fixture.inputs.depth.resource = {};
    Check(!CompilePostSubgraphs(fixture.inputs, fixture.targets, plan), "required guide needs an actual resource");
}

void ExternalPostTopology()
{
    for (const bool bloom : {false, true})
        for (const bool outline : {false, true})
            for (const bool linear_ui : {false, true})
            {
                PostFixture fixture;
                fixture.inputs.bloom = bloom;
                fixture.inputs.outline = outline;
                fixture.inputs.external_temporal = Texture(20, 7);
                fixture.inputs.normal = {};
                fixture.inputs.motion = {};
                fixture.inputs.surface_history = {};
                fixture.inputs.temporal_history = {};
                fixture.targets.temporal = {};
                if (linear_ui) fixture.targets.display_linear = Texture(21, 3);
                PostSubgraphPlan plan;
                Check(CompilePostSubgraphs(fixture.inputs, fixture.targets, plan).Succeeded(),
                      "external reconstruction does not require native temporal guides");
                Check(std::ranges::find(plan.passes, 9u, &PostPassPlan::id) == plan.passes.end() &&
                          SameVersion(plan.outputs.temporal, fixture.inputs.external_temporal),
                      "external reconstruction bypasses temporal pass and preserves the provider version");
                Check(SameVersion(Pass(plan, bloom ? 10 : 14).reads[bloom ? 1 : 0].version,
                                  fixture.inputs.external_temporal),
                      "post consumes the exact external reconstruction output");
                Check(Pass(plan, 17).read_write == !linear_ui &&
                          Pass(plan, 17).reads.size() == (linear_ui ? 2u : 1u),
                      "UI uses display-linear shader read or back-buffer load as configured");
                if (linear_ui)
                    Check(SameVersion(Pass(plan, 17).reads[1].version, Pass(plan, 16).output),
                          "linear UI consumes the actual FXAA output");
            }
    PostFixture fixture;
    fixture.targets.temporal_normal = Texture(30, UINT32_MAX);
    PostSubgraphPlan plan;
    Check(!CompilePostSubgraphs(fixture.inputs, fixture.targets, plan) && plan.passes.empty(),
          "temporal guide write must not wrap its resource ordinal");
    fixture.targets.temporal_normal = Texture(30, 4);
    fixture.targets.temporal_material = Texture(31, 8);
    Check(CompilePostSubgraphs(fixture.inputs, fixture.targets, plan).Succeeded() &&
              Pass(plan, 9).additional_outputs.size() == 2 &&
              plan.outputs.temporal_normal.ordinal == 5 && plan.outputs.temporal_material.ordinal == 9,
          "native temporal retains normal and material history writes");
    fixture.inputs.external_temporal = {{20, ResourceKind::Buffer}, 7};
    Check(!CompilePostSubgraphs(fixture.inputs, fixture.targets, plan) && plan.passes.empty(),
          "external reconstruction output must be a texture");
}

} // namespace

int main()
{
    try
    {
        RendererFeaturePlan plan;
        plan.reflection_mode = GraphicsReflectionMode::Off;
        Check(LightingAuxiliaryTargets(plan, false) == std::array{false, false, false},
            "probe lighting without consumers has no auxiliary attachments");
        Check(LightingAuxiliaryTargets(plan, true) == std::array{true, false, false},
            "authored subsurface keeps its diffuse attachment");
        plan.reflection_mode = GraphicsReflectionMode::ScreenSpace;
        Check(LightingAuxiliaryTargets(plan, false) == std::array{false, true, false},
            "SSR retains only its environment fallback");
        plan.reflection_mode = GraphicsReflectionMode::Off;
        plan.software_gi_enabled = true;
        Check(LightingAuxiliaryTargets(plan, false) == std::array{true, true, true},
            "software GI retains all backing signals");
        plan.software_gi_enabled = false;
        ConfiguredPostTopology();
        ExternalPostTopology();
        std::puts("Configured post topology checks passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
