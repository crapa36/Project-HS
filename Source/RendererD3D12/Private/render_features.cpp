#include "renderer_post_subgraphs.hpp"

#include <initializer_list>
#include <utility>

namespace hs
{
namespace
{
Result PostFailure(const char *message)
{
    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_features", message);
}

bool IsTexture(ResourceVersion version)
{
    return version.resource && version.resource.kind == ResourceKind::Texture;
}
}

// The post chain is fixed; the FrameGraph owns resource dependencies and lifetimes.
Result CompilePostSubgraphs(const PostSubgraphInputs &inputs,
                            const PostSubgraphTargets &targets,
                            PostSubgraphPlan &plan)
{
    plan = {};
    if (inputs.composite_hdr.resource.value == targets.post_a.resource.value &&
        inputs.composite_hdr.resource.kind == targets.post_a.resource.kind &&
        inputs.composite_hdr.ordinal != targets.post_a.ordinal)
        return PostFailure("Post target must continue the current physical composite version.");
    PostSubgraphPlan candidate;
    candidate.outputs = {targets.temporal, targets.bloom_half, targets.bloom_quarter,
                         targets.bloom_half_combined, targets.post_a, targets.post_b,
                         targets.back_buffer, targets.temporal_normal,
                         targets.temporal_material, targets.display_linear};
    candidate.passes.reserve(4 + (inputs.bloom ? 4 : 0) + (inputs.outline ? 1 : 0));
    const auto next_pass = [&](std::uint32_t id, std::string_view name,
                               std::initializer_list<ResourceVersion> reads,
                               ResourceVersion &destination, bool read_write = false) -> Result {
        if (!IsTexture(destination) || destination.ordinal == UINT32_MAX)
            return PostFailure("Post target is invalid or has no next resource version.");
        PostPassPlan pass;
        pass.id = id;
        pass.name = name;
        for (const auto read : reads)
        {
            if (!IsTexture(read)) return PostFailure("Post input requires a texture resource.");
            pass.reads.push_back({read, Access::ShaderRead});
        }
        pass.predecessor = destination;
        ++destination.ordinal;
        pass.output = destination;
        pass.read_write = read_write;
        candidate.passes.push_back(std::move(pass));
        return Result::Success();
    };
    if (inputs.external_temporal.resource)
    {
        if (!IsTexture(inputs.external_temporal))
            return PostFailure("Reconstruction output requires a texture resource.");
        candidate.outputs.temporal = inputs.external_temporal;
    }
    else
    {
        if (auto added = next_pass(9, "Temporal Resolve",
                {inputs.composite_hdr, inputs.scene_hdr, inputs.metadata, inputs.normal,
                 inputs.motion, inputs.surface_history, inputs.depth, inputs.temporal_history},
                candidate.outputs.temporal); !added) return added;
        auto &pass = candidate.passes.back();
        for (const auto version : {inputs.reactive_composite_hdr, inputs.temporal_normal_history,
                                  inputs.temporal_material_history})
            if (version.resource) pass.reads.push_back({version, Access::ShaderRead});
        if (inputs.exposure.resource) pass.reads.push_back({inputs.exposure, Access::ShaderRead});
        for (const auto guide : inputs.transparent_guides)
            if (guide.resource) pass.reads.push_back({guide, Access::ShaderRead});
        for (auto *version : {&candidate.outputs.temporal_normal, &candidate.outputs.temporal_material})
            if (version->resource)
            {
                if (!IsTexture(*version) || version->ordinal == UINT32_MAX)
                    return PostFailure("Temporal guide target is invalid or has no next resource version.");
                const auto predecessor = *version;
                ++version->ordinal;
                pass.additional_outputs.emplace_back(predecessor, *version);
            }
    }
    auto color = candidate.outputs.temporal;
    if (inputs.bloom)
    {
        if (auto added = next_pass(10, "Bloom Half Extract", {inputs.composite_hdr, color},
                                  candidate.outputs.bloom_half); !added) return added;
        if (auto added = next_pass(11, "Bloom Quarter Downsample", {candidate.outputs.bloom_half},
                                  candidate.outputs.bloom_quarter); !added) return added;
        if (auto added = next_pass(12, "Bloom Half Upsample",
                {candidate.outputs.bloom_half, candidate.outputs.bloom_quarter},
                candidate.outputs.bloom_half_combined); !added) return added;
        if (auto added = next_pass(13, "Bloom", {inputs.composite_hdr, color, candidate.outputs.bloom_half_combined},
                                  candidate.outputs.post_b); !added) return added;
        color = candidate.outputs.post_b;
    }
    const bool linear_ui = static_cast<bool>(targets.display_linear.resource);
    const bool identity_fxaa = inputs.fxaa_identity && IsTexture(targets.display_linear);
    auto &tone_map_target = identity_fxaa && !inputs.outline
        ? candidate.outputs.display_linear : candidate.outputs.post_a;
    if (auto added = next_pass(14, "ToneMap", {color}, tone_map_target); !added) return added;
    if (identity_fxaa && !inputs.outline && inputs.initialize_post_targets)
    {
        if (!IsTexture(candidate.outputs.post_a) || candidate.outputs.post_a.ordinal == UINT32_MAX)
            return PostFailure("Post target is invalid or has no next resource version.");
        const auto predecessor = candidate.outputs.post_a;
        ++candidate.outputs.post_a.ordinal;
        candidate.passes.back().additional_outputs.emplace_back(predecessor, candidate.outputs.post_a);
    }
    color = tone_map_target;
    if (inputs.outline)
    {
        auto &outline_target = identity_fxaa ? candidate.outputs.display_linear : candidate.outputs.post_b;
        if (auto added = next_pass(15, "Screen-space Outline", {color, inputs.metadata, inputs.depth},
                                  outline_target); !added) return added;
        color = outline_target;
    }
    if (identity_fxaa)
    {
        if (inputs.screen_overlay_upload.resource)
        {
            if (auto added = next_pass(16, "Screen Overlay", {}, candidate.outputs.display_linear, true); !added)
                return added;
        }
    }
    else
    {
        auto &aa_target = linear_ui ? candidate.outputs.display_linear : candidate.outputs.back_buffer;
        if (auto added = next_pass(16, "FXAA", {color}, aa_target); !added) return added;
    }
    if (auto added = next_pass(17, "Game UI", {inputs.ui}, candidate.outputs.back_buffer, !linear_ui); !added)
        return added;
    if (linear_ui) candidate.passes.back().reads.push_back({candidate.outputs.display_linear, Access::ShaderRead});
    plan = std::move(candidate);
    return Result::Success();
}
} // namespace hs
