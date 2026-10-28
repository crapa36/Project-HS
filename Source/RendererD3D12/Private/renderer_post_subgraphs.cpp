#include "renderer_post_subgraphs.hpp"

#include <cstddef>
#include <utility>

namespace hs
{
namespace
{
std::array<ResourceVersion, 34> PostVersions(const PostSubgraphInputs &in,
    const PostSubgraphTargets &target)
{
    return {in.scene_hdr, in.composite_hdr, in.depth, in.normal, in.metadata,
        in.motion, in.surface_history, in.temporal_history, in.ui,
        in.temporal_normal_history, in.temporal_material_history, in.exposure,
        in.external_temporal, in.transparent_guides[0], in.transparent_guides[1],
        in.transparent_guides[2], in.transparent_guides[3], in.transparent_guides[4],
        in.transparent_guides[5], in.transparent_guides[6], in.screen_overlay_alpha,
        in.screen_overlay_upload, in.screen_overlay_gradient,
        in.reactive_composite_hdr, target.temporal, target.bloom_half,
        target.bloom_quarter, target.bloom_half_combined, target.post_a,
        target.post_b, target.back_buffer, target.temporal_normal,
        target.temporal_material, target.display_linear};
}
bool SameVersion(ResourceVersion a, ResourceVersion b)
{
    return a.resource.value == b.resource.value && a.resource.kind == b.resource.kind &&
        a.ordinal == b.ordinal;
}
}
Result BuildPostSubgraphs(RenderGraphBuilder &graph,
                         const PostSubgraphInputs &inputs,
                         const PostSubgraphTargets &targets,
                         PostPassCallbacks callbacks,
                         PostSubgraphOutputs &outputs, PostSubgraphLayout *layout)
{
    PostSubgraphLayout next{};
    next.inputs = PostVersions(inputs, targets);
    next.bloom = inputs.bloom; next.outline = inputs.outline;
    next.fxaa_identity = inputs.fxaa_identity;
    next.initialize_post_targets = inputs.initialize_post_targets;
    next.pre_bloom = static_cast<bool>(inputs.pre_bloom);
    next.pre_tone_map = static_cast<bool>(inputs.pre_tone_map);
    next.devtools_ui = static_cast<bool>(inputs.devtools_ui);
    PostSubgraphPlan plan;
    if (auto compiled = CompilePostSubgraphs(inputs, targets, plan); !compiled) return compiled;
    for (const auto &pass : plan.passes)
        if (!callbacks[pass.id - 9])
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features",
                                   "Post subgraph requires an execution callback.");
    auto downstream_hdr = plan.outputs.temporal;
    bool pre_bloom_invoked = false;
    const auto same = [](ResourceVersion left, ResourceVersion right) {
        return left.resource.value == right.resource.value && left.resource.kind == right.resource.kind &&
            left.ordinal == right.ordinal;
    };
    for (auto pass : plan.passes)
    {
        if (pass.id > 9 && !pre_bloom_invoked)
        {
            pre_bloom_invoked = true;
            if (inputs.pre_bloom)
            {
                ResourceVersion processed;
                if (auto lowered = inputs.pre_bloom(graph, downstream_hdr, processed); !lowered) return lowered;
                if (!processed.resource || processed.resource.kind != ResourceKind::Texture)
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features", "HDR pre-bloom hook returned an invalid texture.");
                downstream_hdr = processed;
            }
        }
        if (pass.id > 9)
            for (auto &read : pass.reads)
                if (same(read.version, plan.outputs.temporal)) read.version = downstream_hdr;
        if (pass.id == 14 && inputs.pre_tone_map)
        {
            const auto source = pass.reads.front().version;
            ResourceVersion graded;
            if (auto lowered = inputs.pre_tone_map(graph, source, graded); !lowered) return lowered;
            if (!graded.resource || graded.resource.kind != ResourceKind::Texture)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features", "HDR pre-tone-map hook returned an invalid texture.");
            for (auto &read : pass.reads)
                if (same(read.version, source)) read.version = graded;
        }
        if (pass.id == 17 && inputs.devtools_ui)
        {
            ResourceVersion encoded;
            if (auto drawn = inputs.devtools_ui(graph, plan.outputs.post_a, encoded); !drawn) return drawn;
            if (!encoded.resource || encoded.resource.kind != ResourceKind::Texture)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features", "DevTools UI returned an invalid texture.");
            pass.reads.push_back({encoded, Access::ShaderRead});
            plan.outputs.post_a = encoded;
        }
        if (next.pass_count == next.passes.size())
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features",
                "Post subgraph pass count exceeds retained layout capacity.");
        next.passes[next.pass_count++] = {graph.PassCount(), pass.id};
        auto builder = graph.AddPass(pass.id, pass.name, QueueHint::Direct);
        if (pass.id == 9 && inputs.screen_overlay_alpha.resource)
            builder.Read(inputs.screen_overlay_alpha, Access::ShaderRead);
        if (pass.id == 16 && inputs.screen_overlay_upload.resource)
        {
            builder.Read(inputs.screen_overlay_upload, Access::ShaderRead);
            builder.Read(inputs.screen_overlay_gradient, Access::ShaderRead);
        }
        for (const auto &read : pass.reads) builder.Read(read.version, read.access);
        const auto written = pass.read_write
            ? builder.ReadWrite(pass.predecessor, Access::RenderTarget)
            : builder.Write(pass.predecessor, Access::RenderTarget);
        if (written.resource.value != pass.output.resource.value ||
            written.resource.kind != pass.output.resource.kind || written.ordinal != pass.output.ordinal)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features",
                                   "Post subgraph resource version disagrees with its plan.");
        for (const auto &[predecessor, expected] : pass.additional_outputs)
        {
            const auto actual = builder.Write(predecessor, Access::RenderTarget);
            if (actual.resource.value != expected.resource.value || actual.ordinal != expected.ordinal)
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features",
                                       "Temporal guide output disagrees with its plan.");
        }
        // Graph owns the callback. No callback captures this helper's local plan,
        // feature registrations, input span or PassBuilder.
        builder.SetExecute(std::move(callbacks[pass.id - 9]));
    }
    outputs = plan.outputs;
    next.outputs = outputs;
    if (layout) *layout = next;
    return Result::Success();
}

bool MatchesPostSubgraphLayout(const PostSubgraphInputs &inputs,
    const PostSubgraphTargets &targets, const PostSubgraphLayout &layout) noexcept
{
    if (!MatchesPostSubgraphShape(inputs.bloom, inputs.outline, inputs.fxaa_identity,
            inputs.initialize_post_targets, static_cast<bool>(inputs.pre_bloom),
            static_cast<bool>(inputs.pre_tone_map), static_cast<bool>(inputs.devtools_ui), layout))
        return false;
    const auto current = PostVersions(inputs, targets);
    for (std::size_t index = 0; index < current.size(); ++index)
        if (!SameVersion(current[index], layout.inputs[index])) return false;
    return true;
}

bool MatchesPostSubgraphShape(bool bloom, bool outline, bool fxaa_identity,
    bool initialize_post_targets, bool pre_bloom, bool pre_tone_map, bool devtools_ui,
    const PostSubgraphLayout &layout) noexcept
{
    return layout.pass_count && layout.pass_count <= layout.passes.size() &&
        layout.bloom == bloom && layout.outline == outline &&
        layout.fxaa_identity == fxaa_identity &&
        layout.initialize_post_targets == initialize_post_targets &&
        layout.pre_bloom == pre_bloom && layout.pre_tone_map == pre_tone_map &&
        layout.devtools_ui == devtools_ui;
}

Result BindRetainedPostSubgraphs(RenderGraphBuilder &graph,
    const PostSubgraphInputs &inputs, const PostSubgraphTargets &targets,
    const PostSubgraphLayout &layout, PostPassCallbacks callbacks,
    PostSubgraphOutputs &outputs)
{
    if (!MatchesPostSubgraphLayout(inputs, targets, layout))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features",
            "Post retained layout variant changed.");
    for (std::uint32_t index = 0; index < layout.pass_count; ++index)
    {
        const auto [ordinal, id] = layout.passes[index];
        if (id < 9 || id > 17 || ordinal >= graph.PassCount() ||
            graph.DeclaredPassId(ordinal) != id || !callbacks[id - 9] ||
            !graph.BindPass(ordinal, id))
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features",
                "Post retained pass or callback changed.");
    }
    for (std::uint32_t index = 0; index < layout.pass_count; ++index)
    {
        const auto [ordinal, id] = layout.passes[index];
        auto pass = graph.BindPass(ordinal, id);
        if (!pass)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features",
                "Post retained pass was already bound.");
        pass->SetExecute(std::move(callbacks[id - 9]));
    }
    outputs = layout.outputs;
    return Result::Success();
}
} // namespace hs
