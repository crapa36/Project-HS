#include "renderer_post_subgraphs.hpp"

#include <utility>

namespace hs
{
Result BuildPostSubgraphs(RenderGraphBuilder &graph,
                         const PostSubgraphInputs &inputs,
                         const PostSubgraphTargets &targets,
                         PostPassCallbacks callbacks,
                         PostSubgraphOutputs &outputs)
{
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
    return Result::Success();
}
} // namespace hs
