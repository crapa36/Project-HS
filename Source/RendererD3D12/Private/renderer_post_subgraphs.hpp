#pragma once

#include "render_graph.hpp"
#include "renderer_post_subgraph_plan.hpp"

#include <functional>
#include <utility>

namespace hs
{
using PostPassCallback = std::move_only_function<void(RenderPassContext &)>;
// Index zero corresponds to stable pass ID 9, through ID 17 at index eight.
using PostPassCallbacks = std::array<PostPassCallback, 9>;

[[nodiscard]] Result BuildPostSubgraphs(RenderGraphBuilder &graph,
    const PostSubgraphInputs &inputs, const PostSubgraphTargets &targets,
    PostPassCallbacks callbacks, PostSubgraphOutputs &outputs,
    PostSubgraphLayout *layout = nullptr);
[[nodiscard]] bool MatchesPostSubgraphLayout(const PostSubgraphInputs &inputs,
    const PostSubgraphTargets &targets, const PostSubgraphLayout &layout) noexcept;
[[nodiscard]] Result BindRetainedPostSubgraphs(RenderGraphBuilder &graph,
    const PostSubgraphInputs &inputs, const PostSubgraphTargets &targets,
    const PostSubgraphLayout &layout, PostPassCallbacks callbacks,
    PostSubgraphOutputs &outputs);
} // namespace hs
