#include "renderer_impl.hpp"
#include "renderer_reconstruction.hpp"

namespace hs
{
namespace
{
Result ViewFailure(std::string_view message)
{
    return Result::Failure(ErrorCode::InvalidArgument, "hs_render_view", std::string(message));
}
bool ExtentValid(ViewExtent e)
{
    return e.width && e.height && e.width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
        e.height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}
}

Result D3D12Renderer::ConfigureView(const RenderViewConfiguration &configuration)
{
    auto &state = *impl_;
    if (!state.initialized || state.frame_submission_faulted)
        return ViewFailure("Renderer is not available for view configuration.");
    if (!configuration.id.value || configuration.role > RenderViewRole::Capture ||
        ((configuration.id == kMainRenderViewId) != (configuration.role == RenderViewRole::Main)) ||
        !ExtentValid(configuration.output_extent) || !ExtentValid(configuration.render_capacity))
        return ViewFailure("View configuration has an invalid identity, role or native extent.");
    auto found = state.owned_views.find(configuration.id.value);
    if (configuration.id == kMainRenderViewId)
    {
        const auto &main = *found->second;
        return configuration.output_extent == main.state.output_extent &&
            configuration.render_capacity == main.state.render_capacity ? Result::Success()
            : ViewFailure("Configure Main through Resize or ApplyOptions.");
    }
    if (found != state.owned_views.end() && found->second->role != configuration.role)
        return ViewFailure("A view role is fixed for the renderer lifetime.");
    if (found != state.owned_views.end())
    {
        const auto &owner = *found->second;
        if (owner.state.output_extent == configuration.output_extent &&
            owner.state.render_capacity == configuration.render_capacity &&
            owner.configuration_revision == state.view_configuration_revision)
            return Result::Success();
    }
    if (state.config.dynamic_resolution)
    {
        ViewExtent minimum;
        if (!ResolveDynamicResolutionExtent(configuration.output_extent, state.config.dynamic_min_percent, minimum) ||
            configuration.render_capacity.width < minimum.width || configuration.render_capacity.height < minimum.height)
            return ViewFailure("View capacity cannot cover the configured DRS minimum extent.");
    }
    std::unique_ptr<Impl::OwnedView> candidate;
    if (auto r = state.PrepareOwnedView(configuration, candidate, false); !r) return r;
    state.unresolved_view_candidates.reserve(state.unresolved_view_candidates.size() + 1);
    if (found != state.owned_views.end()) candidate->history_generation = found->second->history_generation + 1;
    // Allocate the new registry node before any native configuration is accepted.
    std::map<std::uint32_t, std::unique_ptr<Impl::OwnedView>> prepared;
    prepared.emplace(configuration.id.value, std::move(candidate));
    auto node = prepared.extract(configuration.id.value);
    auto &next = *node.mapped();
    if (auto r = state.WaitForGpu(); !r) return r;
    if (auto r = state.ActivateView(kMainRenderViewId); !r) return r;
    const auto restore = [&]() -> Result {
        if (state.streamline_session)
            if (auto r = state.streamline_session->SelectReconstructionViewAfterCompletion(state.active_native_viewport_id); !r) {
                state.frame_submission_faulted = true;
                return r;
            }
        if (state.fsr_session)
            if (auto r = state.fsr_session->SelectReconstructionViewAfterCompletion(state.active_native_viewport_id); !r) {
                state.frame_submission_faulted = true;
                return r;
            }
        return Result::Success();
    };
    const auto reject = [&](Result failure) -> Result {
        if (auto r = state.ResetOwnedViewAfterCompletion(next); !r) {
            state.initialized = false;
            state.unresolved_view_candidates.push_back(std::move(node.mapped()));
            return r;
        }
        if (auto r = restore(); !r) return r;
        return failure;
    };
    if (state.streamline_session)
        if (auto r = state.streamline_session->SelectReconstructionViewAfterCompletion(next.native_viewport_id); !r) return reject(r);
    if (state.fsr_session)
        if (auto r = state.fsr_session->SelectReconstructionViewAfterCompletion(next.native_viewport_id); !r) return reject(r);
    if (next.reconstruction_provider)
        if (auto r = next.reconstruction_provider->ConfigureAfterCompletion(next.state.render_capacity); !r) return reject(r);
    if (auto r = restore(); !r) return reject(r);
    if (found != state.owned_views.end())
    {
        if (auto r = state.RetireHlodView(configuration.id); !r) return reject(r);
        if (auto r = state.ResetOwnedViewAfterCompletion(*found->second); !r) {
            state.frame_submission_faulted = true;
            return reject(r);
        }
        found->second = std::move(node.mapped());
    }
    else state.owned_views.insert(std::move(node));
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareToolViews(std::span<const RenderViewSample> samples)
{
    if (samples.empty()) return ViewFailure("A tool frame requires at least one prepared view.");
    if (native_input_phase && std::ranges::none_of(samples, [](const auto &s) { return s.id == kMainRenderViewId; }))
        return ViewFailure("An outstanding Main input token requires its Main view.");
    for (auto current = samples.begin(); current != samples.end(); ++current)
    {
        const auto &sample = *current;
        if (std::find_if(samples.begin(), current, [&](const auto &prior) { return prior.id == sample.id; }) != current)
            return ViewFailure("Tool view IDs must be unique within a frame.");
        const auto found = owned_views.find(sample.id.value);
        if (found == owned_views.end() || !found->second->ready)
            return ViewFailure("Configure the tool view before submitting its sample.");
        const auto &owner = *found->second;
        if (owner.configuration_revision != view_configuration_revision)
            return ViewFailure("Reconfigure the tool view after renderer configuration changes.");
        const auto capacity = owner.state.render_capacity;
        if (!ExtentValid(sample.render_active) || sample.render_active.width > capacity.width ||
            sample.render_active.height > capacity.height)
            return ViewFailure("Tool active extent exceeds its configured native capacity.");
        if (owner.reconstruction_provider && owner.reconstruction_provider->Initialized())
        {
            const auto &plan = owner.reconstruction_plan;
            if (sample.render_active.width < plan.input_minimum.width || sample.render_active.height < plan.input_minimum.height ||
                sample.render_active.width > plan.input_maximum.width || sample.render_active.height > plan.input_maximum.height)
                return ViewFailure("Tool active extent is outside the selected reconstruction domain.");
        }
        for (const auto position : {sample.camera.position, sample.camera.target})
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
                return ViewFailure("Tool camera coordinates must be finite.");
    }
    return Result::Success();
}

Result D3D12Renderer::Impl::PrepareOwnedView(const RenderViewConfiguration &input, std::unique_ptr<OwnedView> &output, bool decal_work)
{
    const bool dynamic_resolution = config.dynamic_resolution;
    auto candidate = std::make_unique<OwnedView>();
    candidate->view_id = input.id;
    candidate->role = input.role;
    candidate->native_viewport_id = next_native_viewport_id++;
    candidate->width = input.output_extent.width; candidate->height = input.output_extent.height;
    candidate->display_state = owned_views.at(kMainRenderViewId.value)->display_state;
    candidate->configuration_revision = view_configuration_revision;
    auto &v = *candidate;
    ViewExtent native_capacity;
    if (auto r = ResolveNativeReconstruction(config, dynamic_resolution, input.output_extent, v.reconstruction_plan, native_capacity); !r) return r;
    if (auto r = PrepareReconstructionProvider(config.reconstruction_request ? &*config.reconstruction_request : nullptr,
            dynamic_resolution, config.dynamic_min_percent, config.dynamic_max_percent, input.output_extent,
            v.reconstruction_plan, native_capacity, v.reconstruction_provider, &config); !r) return r;
    const auto capacity = input.render_capacity;
    if (v.reconstruction_provider && (native_capacity.width > capacity.width || native_capacity.height > capacity.height))
        return ViewFailure("Requested view capacity does not cover the selected native reconstruction domain.");
    const auto active = v.reconstruction_provider ? v.reconstruction_plan.render_active : capacity;
    (void)UpdateViewState(v.state, input.output_extent, capacity, active);
    v.render_width = active.width; v.render_height = active.height;
    if (auto r = PrepareOptionTargets(v.targets, input.render_capacity.width, input.render_capacity.height,
            config.bloom, input.output_extent.width, input.output_extent.height,
            UsesInternalResolveTargets(v.reconstruction_plan),
            RequiredLightingAuxiliaries(config)); !r) return r;
    if (auto r = PrepareOptionPools(v.targets.transient_pools, capacity.width, capacity.height,
            config.bloom, v.width, v.height); !r) return r;
    if (auto r = CreateViewDepthHeap(v); !r) return r;
    WriteDepthViews(v);
    candidate->transient_textures_common = enhanced;
    if (auto r = PrepareUiResources(v.ui, v.width, v.height); !r) return r;
    D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = v.width; desc.Height = v.height;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = v.display_state.format; desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (std::size_t i = 0; i < kFrameCount; ++i)
    {
        if (auto r = CreateAllocation(v.offscreen_allocations[i], allocation, desc, D3D12_RESOURCE_STATE_COMMON); !r) return r;
        v.outputs[i] = v.offscreen_allocations[i].resource;
    }
    if (auto r = PrepareExposureResources(v); !r) return r;
    const auto completed = graph_queue_executor.Completed();
    ViewFeatureCandidates features;
    if (auto r = PrepareViewFeatures(v, config, capacity, input.output_extent, decal_work, features); !r)
        return r;
    LocalShadowCandidate local_shadows;
    if (auto r = PrepareLocalShadows(v, config, local_shadows); !r) return r;
    if (auto r = CommitViewFeaturePrefix(v, features, completed); !r) return r;
    if (auto r = v.local_shadows.Commit(std::move(local_shadows), completed); !r) return r;
    if (auto r = CommitVolumetrics(v, std::move(features.volume)); !r) return r;
    ReconstructionGuidesCandidate guides;
    if (v.reconstruction_provider)
    {
        v.reconstruction_provider->SetViewportId(v.native_viewport_id);
        if (auto r = PrepareReconstructionGuides(v, config, capacity, input.output_extent, guides,
                v.reconstruction_provider->NeedsCompositionGuide()); !r) return r;
        if (auto r = v.reconstruction_guides.Commit(std::move(guides), completed); !r) return r;
    }
    v.reconstruction_plan.render_active = active;
    v.ready = true;
    output = std::move(candidate);
    return Result::Success();
}


} // namespace hs
