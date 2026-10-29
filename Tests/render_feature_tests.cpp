#include "renderer_post_subgraph_plan.hpp"
#include "renderer_feature_resolve.hpp"
#include "renderer_vfx_candidates.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
using namespace hs;

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

void BoundedDecalCandidates()
{
    std::array<VfxDecalInput, 513> inputs{};
    std::vector<VfxDecalCandidate> expected;
    for (std::size_t i = 0; i < inputs.size(); ++i)
    {
        inputs[i].importance = i < 128 ? 3u : static_cast<std::uint32_t>((i * 17) % 5);
        inputs[i].stable_id = i < 128 ? 7u : (i * 23) % 41;
        inputs[i].stable_seed = i < 128 ? 1u : static_cast<std::uint32_t>(i % 5);
        expected.push_back({&inputs[i], i < 128 ? .5f : static_cast<float>(i % 9) / 8.0f});
    }
    auto stable_reference = expected;
    std::stable_sort(stable_reference.begin(), stable_reference.end(), [](const auto &a, const auto &b) {
        if (a.source->importance != b.source->importance) return a.source->importance > b.source->importance;
        if (a.area != b.area) return a.area > b.area;
        if (a.source->stable_id != b.source->stable_id) return a.source->stable_id < b.source->stable_id;
        return a.source->stable_seed < b.source->stable_seed;
    });
    for (const std::size_t limit : {0u, 1u, 2u, 7u, 31u, 64u})
    {
        VfxDecalCandidates candidates(limit);
        for (const auto candidate : expected) candidates.Add(candidate);
        const auto selected = candidates.Sorted();
        Check(selected.size() == limit, "decal candidate storage remains bounded by the actual output limit");
        for (std::size_t i = 0; i < selected.size(); ++i)
            Check(selected[i].source == stable_reference[i].source && selected[i].area == stable_reference[i].area,
                "bounded decal selection must match the original stable-sort prefix, including ties");
    }
    VfxDecalCandidates sparse(64);
    for (const auto candidate : std::span(expected).first(3))
        sparse.Add(candidate);
    Check(sparse.Sorted().size() == 3, "sparse decal input never exposes uninitialized candidate slots");
    VfxDecalCandidates ties(64);
    for (auto &input : inputs)
    {
        input.importance = 5; input.stable_id = 1; input.stable_seed = 7;
        ties.Add({&input, .5f});
    }
    const auto tied = ties.Sorted();
    for (std::size_t i = 0; i < tied.size(); ++i)
        Check(tied[i].source == &inputs[i], "equal-ranked decals keep the original first64 input order");
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

void IdentityFxaaTopology()
{
    for (const bool outline : {false, true})
        for (const bool overlay : {false, true})
        {
            PostFixture fixture;
            fixture.inputs.outline = outline;
            fixture.inputs.fxaa_identity = true;
            fixture.targets.display_linear = Texture(21, 3);
            if (overlay)
            {
                fixture.inputs.screen_overlay_upload = Texture(22);
                fixture.inputs.screen_overlay_gradient = Texture(23);
            }
            PostSubgraphPlan plan;
            Check(CompilePostSubgraphs(fixture.inputs, fixture.targets, plan).Succeeded(),
                  "compile identity FXAA plan");
            const auto &tone_map = Pass(plan, 14);
            const auto &color_output = outline ? Pass(plan, 15).output : tone_map.output;
            Check(SameVersion(tone_map.output, outline ? Texture(2, 1) : Texture(21, 4)),
                  "identity FXAA routes tone map to display linear only without outline");
            if (outline)
                Check(SameVersion(Pass(plan, 15).output, Texture(21, 4)) &&
                          SameVersion(Pass(plan, 15).predecessor, Texture(21, 3)) &&
                          SameVersion(Pass(plan, 15).reads[0].version, tone_map.output),
                      "identity FXAA routes outline directly to display linear");
            Check(SameVersion(plan.outputs.display_linear,
                              Texture(21, overlay ? 5 : 4)) &&
                      SameVersion(Pass(plan, 17).reads.back().version, plan.outputs.display_linear),
                  "Game UI reads the actual final display-linear version");
            Check(SameVersion(plan.outputs.post_a, Texture(2, outline ? 1 : 0)) &&
                      SameVersion(plan.outputs.post_b, Texture(14, 0)),
                  "identity FXAA preserves unused ping-pong target versions");
            Check(tone_map.additional_outputs.empty(), "steady identity FXAA does not rewrite post A");
            const auto overlay_pass = std::ranges::find(plan.passes, 16u, &PostPassPlan::id);
            if (overlay)
            {
                Check(overlay_pass != plan.passes.end() && overlay_pass->name == "Screen Overlay" &&
                          overlay_pass->read_write && overlay_pass->reads.empty() &&
                          SameVersion(overlay_pass->predecessor, color_output) &&
                          SameVersion(overlay_pass->output, plan.outputs.display_linear),
                      "identity FXAA retains an overlay-only read-write without color input");
            }
            else
                Check(overlay_pass == plan.passes.end(), "identity FXAA omits the unused pass 16 copy");

        }

    PostFixture no_linear_target;
    no_linear_target.inputs.fxaa_identity = true;
    PostSubgraphPlan legacy_plan;
    Check(CompilePostSubgraphs(no_linear_target.inputs, no_linear_target.targets, legacy_plan).Succeeded() &&
              Pass(legacy_plan, 16).name == "FXAA" &&
              SameVersion(Pass(legacy_plan, 16).output, Texture(15, 1)),
          "identity FXAA leaves the legacy path unchanged without a linear target");

    PostFixture initialize_post;
    initialize_post.inputs.fxaa_identity = true;
    initialize_post.inputs.initialize_post_targets = true;
    initialize_post.targets.display_linear = Texture(21, 3);
    PostSubgraphPlan initialization_plan;
    Check(CompilePostSubgraphs(initialize_post.inputs, initialize_post.targets, initialization_plan).Succeeded(),
          "compile first-use identity FXAA plan");
    const auto &initialization_tone_map = Pass(initialization_plan, 14);
    Check(initialization_tone_map.additional_outputs.size() == 1 &&
              SameVersion(initialization_tone_map.additional_outputs[0].first, Texture(2, 0)) &&
              SameVersion(initialization_tone_map.additional_outputs[0].second, Texture(2, 1)) &&
              SameVersion(initialization_plan.outputs.post_a, Texture(2, 1)),
          "first-use identity tone map initializes post A exactly once");
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
        Check(!EffectiveDynamicResolution(true, plan),
            "requested DRS is ineffective while software GI owns a fixed extent");
        Check(!EffectiveDynamicResolution(false, plan),
            "requested DRS off remains ineffective with software GI");
        plan.software_gi_enabled = false;
        Check(!EffectiveDynamicResolution(false, plan),
            "requested DRS off stays off when software GI is inactive");
        Check(EffectiveDynamicResolution(true, plan),
            "qualified DRS request is effective when software GI is inactive");
        ConfiguredPostTopology();
        ExternalPostTopology();
        IdentityFxaaTopology();
        BoundedDecalCandidates();
        std::puts("Configured post topology checks passed.");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
