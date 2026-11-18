#include "renderer_transparency_targets.hpp"

#include <algorithm>
#include <utility>

namespace hs
{
using namespace translucency_detail;

namespace
{
void ReadMotion(PassBuilder &pass, const LayeredTranslucencyInputs &inputs)
{
    if (inputs.previous_instances.resource) pass.Read(inputs.previous_instances, Access::VertexShaderRead);
    if (inputs.previous_skin.resource) pass.Read(inputs.previous_skin, Access::VertexShaderRead);
}
bool Fits(const LayeredTranslucencyGuide &guide, ViewExtent active, DXGI_FORMAT format)
{
    if (!guide.resource || !guide.version.resource || guide.version.resource.kind != ResourceKind::Texture)
        return false;
    const auto desc = guide.resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.Width >= active.width && desc.Height >= active.height && desc.Format == format;
}
bool FitsDepth(const LayeredTranslucencyGuide &guide, ViewExtent active)
{
    if (!guide.resource || !guide.version.resource || guide.version.resource.kind != ResourceKind::Texture)
        return false;
    const auto desc = guide.resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.DepthOrArraySize == 1 &&
        desc.SampleDesc.Count == 1 && desc.Width >= active.width && desc.Height >= active.height &&
        (desc.Format == DXGI_FORMAT_R32_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT ||
         desc.Format == DXGI_FORMAT_R32_FLOAT);
}
} // namespace

Result TranslucencySystem::RegisterLayers(RenderGraphBuilder &graph,
    const LayeredTranslucencyInputs &inputs, LayeredTranslucencyOutputs &outputs,
    LayeredTranslucencyLayout *layout)
{
    return RegisterLayersImpl(graph, inputs, outputs, layout, nullptr);
}
Result TranslucencySystem::BindRetainedLayers(RenderGraphBuilder &graph,
    const LayeredTranslucencyInputs &inputs, const LayeredTranslucencyLayout &layout,
    LayeredTranslucencyOutputs &outputs)
{
    return RegisterLayersImpl(graph, inputs, outputs, nullptr, &layout);
}
Result TranslucencySystem::RegisterLayersImpl(RenderGraphBuilder &graph,
    const LayeredTranslucencyInputs &inputs, LayeredTranslucencyOutputs &outputs,
    LayeredTranslucencyLayout *layout, const LayeredTranslucencyLayout *retained)
{
    outputs = {};
    auto storage = impl_->targets;
    if (!storage) return Failure(ErrorCode::InvalidState, "Prepare and commit translucency transparency before registration.");
    if (!storage->enabled) return Result::Success();
    if (impl_->pending.graph || !storage->usable)
        return Failure(ErrorCode::InvalidState, "Translucency transparency is pending or its previous graph failed.");
    const auto active = inputs.render_active;
    const auto rect = inputs.composition_rect;
    const bool rect_empty = rect.left == rect.right && rect.top == rect.bottom;
    if (!rect_empty && (rect.left < 0 || rect.top < 0 || rect.right <= rect.left ||
            rect.bottom <= rect.top || static_cast<std::uint32_t>(rect.right) > active.width ||
            static_cast<std::uint32_t>(rect.bottom) > active.height))
        return Failure(ErrorCode::InvalidArgument, "Translucency composition rectangle exceeds its active extent.");
    const bool use_occupancy = inputs.use_occupancy_queries && storage->occupancy_queries.Get();
#if defined(HS_DEVELOPMENT_TOOLS)
    const bool capture_work = static_cast<bool>(storage->work);
#else
    constexpr bool capture_work = false;
#endif
    if (retained)
    {
        const std::array current{inputs.opaque_depth.version, inputs.hdr.version,
            inputs.accumulation.version, inputs.revealage.version};
        if (retained->target_generation != storage->generation ||
            retained->layer_count != storage->layer_count ||
            retained->capture_work != capture_work ||
            retained->occupancy != use_occupancy ||
            inputs.previous_instances != retained->previous_instances ||
            inputs.previous_skin != retained->previous_skin ||
            !std::ranges::equal(inputs.peel_reads, retained->peel_reads) ||
            !std::ranges::equal(inputs.scene_reads, retained->scene_reads))
            return Failure(ErrorCode::InvalidState, "Translucency retained topology variant changed.");
        if (current != retained->inputs)
            return Failure(ErrorCode::InvalidState, "Translucency retained input version changed.");
    }
    if (!ValidExtent(active) || active.width > storage->capacity.width || active.height > storage->capacity.height ||
        inputs.frame_slot >= storage->frame_slot_count || !storage->slot_completion[inputs.frame_slot].IsSatisfiedBy(inputs.completed) ||
        !FitsDepth(inputs.opaque_depth, active) || !Fits(inputs.hdr, active, kColor) ||
        !Fits(inputs.accumulation, active, DXGI_FORMAT_R32G32B32A32_FLOAT) ||
        !Fits(inputs.revealage, active, DXGI_FORMAT_R16G16_FLOAT) ||
        !inputs.accumulation_rtv.ptr || !inputs.revealage_rtv.ptr || !inputs.opaque_readonly_dsv.ptr || !inputs.shader_heap ||
        !inputs.table_gpu.ptr || inputs.descriptor_stride != storage->srv_stride || !inputs.draw)
        return Failure(ErrorCode::InvalidArgument, "Translucency transparency frame, guides, native table or draw callback is invalid.");
    const auto heap = inputs.shader_heap->GetDesc();
    const auto heapGpu = inputs.shader_heap->GetGPUDescriptorHandleForHeapStart().ptr;
    const auto heapBytes = UINT64(heap.NumDescriptors) * inputs.descriptor_stride;
    if (heap.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV || !(heap.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ||
        inputs.table_gpu.ptr < heapGpu || (inputs.table_gpu.ptr - heapGpu) % inputs.descriptor_stride != 0 ||
        heapBytes < UINT64(kLayeredTranslucencyDescriptorsPerFrame) * inputs.descriptor_stride ||
        inputs.table_gpu.ptr - heapGpu > heapBytes - UINT64(kLayeredTranslucencyDescriptorsPerFrame) * inputs.descriptor_stride)
        return Failure(ErrorCode::InvalidArgument, "Translucency transparency descriptor suffix does not fit its native shader heap.");
    Impl::Pending pending;
    pending.graph = &graph; pending.targets = storage; pending.inputs = inputs; pending.shader_heap = inputs.shader_heap;
    if (layout)
    {
        *layout = {};
        layout->target_generation = storage->generation;
        layout->layer_count = storage->layer_count;
        layout->capture_work = capture_work;
        layout->inputs = {inputs.opaque_depth.version, inputs.hdr.version,
            inputs.accumulation.version, inputs.revealage.version};
        layout->peel_reads.assign(inputs.peel_reads.begin(), inputs.peel_reads.end());
        layout->scene_reads.assign(inputs.scene_reads.begin(), inputs.scene_reads.end());
        layout->previous_instances = inputs.previous_instances;
        layout->previous_skin = inputs.previous_skin;
    }
    if (layout) layout->occupancy = use_occupancy;
    if (!retained) pending.scene_reads.assign(inputs.scene_reads.begin(), inputs.scene_reads.end());
    const auto certify = [&](AllocationResource &resource) {
        return resource.EnsureNativeContract(storage->device.Get());
    };
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
        for (auto *resource : {&storage->front[layer], &storage->identity[layer], &storage->optical_component[layer]})
            if (auto certified = certify(*resource); !certified) return certified;
    if (auto certified = certify(storage->exit); !certified) return certified;
    for (std::uint32_t index = 0; index < storage->guides.size(); ++index)
    {
        if (index == 0 || index == 4) continue;
        if (auto certified = certify(storage->guides[index]); !certified) return certified;
    }
    for (auto &resource : storage->color)
        if (auto certified = certify(resource); !certified) return certified;
    if (use_occupancy)
        for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
            if (auto certified = certify(storage->occupancy_results[inputs.frame_slot][layer]);
                !certified) return certified;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (storage->work)
    {
        if (auto collected = storage->work->Collect(inputs.completed, impl_->completed_work); !collected) return collected;
        const auto begun = retained
            ? storage->work->BindRetainedBegin(graph, inputs, retained->work)
            : storage->work->Begin(graph, inputs, layout ? &layout->work : nullptr);
        if (!begun) return begun;
    }
#endif
    bool imports_valid = true;
    auto import = [&](AllocationResource &resource, std::string_view name, TextureHandle saved = {}) {
        const auto external = ExternalTexture{resource.resource.Get(), resource.graph_completion,
            resource.graph_queue, {}, resource.NativeContract()};
        const auto handle = retained ? saved : graph.ImportTexture(external, resource.graph_access, name);
        if (retained && !graph.BindTexture(handle, external, resource.graph_access))
            imports_valid = false;
        pending.imports.push_back({&resource, handle}); return graph.Version(handle);
    };
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
    {
        pending.front[layer] = import(storage->front[layer], "Translucency.FrontDepth",
            retained ? retained->front[layer] : TextureHandle{});
        pending.identity[layer] = import(storage->identity[layer], "Translucency.InstanceIdentity",
            retained ? retained->identity[layer] : TextureHandle{});
        pending.optical_component[layer] = import(storage->optical_component[layer], "Translucency.OpticalComponent",
            retained ? retained->optical_component[layer] : TextureHandle{});
        if (layout)
        {
            layout->front[layer] = pending.front[layer].resource;
            layout->identity[layer] = pending.identity[layer].resource;
            layout->optical_component[layer] = pending.optical_component[layer].resource;
        }
    }
    pending.exit = import(storage->exit, "Translucency.ExitDepth", retained ? retained->exit : TextureHandle{});
    if (layout) layout->exit = pending.exit.resource;
    for (std::uint32_t index = 0; index < storage->color.size(); ++index)
    {
        pending.color[index] = import(storage->color[index], "Translucency.CompositeHDR",
            retained ? retained->color[index] : TextureHandle{});
        if (layout) layout->color[index] = pending.color[index].resource;
    }
    auto &guides = pending.guides;
    for (std::uint32_t index = 0; index < guides.size(); ++index)
    {
        if (index == 0) guides[index] = pending.front[0];
        else if (index == 4) guides[index] = pending.identity[0];
        else guides[index] = import(storage->guides[index], "Translucency.NearestFrontGuide",
            retained ? retained->guides[index] : TextureHandle{});
        if (layout) layout->guides[index] = guides[index].resource;
    }
    std::array<ResourceVersion, kLayeredTranslucencyMaxLayers> occupancy{};
    if (use_occupancy)
        for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
        {
            auto &result = storage->occupancy_results[inputs.frame_slot][layer];
            const auto external = ExternalBuffer{result.resource.Get(), result.graph_completion,
                result.graph_queue, {}, result.NativeContract()};
            const auto handle = retained ? retained->occupancy_results[layer]
                : graph.ImportBuffer(external, result.graph_access, "Translucency.LayerOccupancy");
            if (retained && !graph.BindBuffer(handle, external, result.graph_access))
                imports_valid = false;
            pending.imports.push_back({&result, handle});
            occupancy[layer] = graph.Version(handle);
            if (layout) layout->occupancy_results[layer] = handle;
        }
    if (!imports_valid)
        return Failure(ErrorCode::InvalidState, "Translucency retained import bindings changed.");
    auto readScene = [&](PassBuilder &pass, bool shading, Access depthAccess = Access::ShaderRead) {
        pass.Read(inputs.opaque_depth.version, depthAccess);
        for (const auto &read : shading ? std::span<const LayeredTranslucencySceneRead>(pending.scene_reads) : inputs.peel_reads)
            pass.Read(read.version, read.access);
    };
    {
    for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
        {
            if (layout) layout->front_pass[layer] = graph.PassCount();
            auto front_pass = retained
                ? graph.BindPass(retained->front_pass[layer], 90 + layer * 2)
                : graph.AddPass(90 + layer * 2, "Translucency.PeelFront", QueueHint::Direct);
            if (!front_pass) return Failure(ErrorCode::InvalidState, "Translucency retained front pass changed.");
            auto &front = *front_pass;
            if (!retained)
            {
                readScene(front, false);
                if (layer) front.Read(pending.front[layer - 1], Access::ShaderRead);
            }
            if (use_occupancy)
            {
                if (!retained)
                {
                    if (layer) front.Read(occupancy[layer - 1], Access::IndirectArgs);
                    occupancy[layer] = front.Write(occupancy[layer], Access::CopyDest);
                }
                else occupancy[layer] = {occupancy[layer].resource, 1};
                pending.occupancy[layer] = occupancy[layer];
            }
            pending.front[layer] = retained ? ResourceVersion{pending.front[layer].resource, 1}
                : front.Write(pending.front[layer], Access::DepthWrite);
            pending.identity[layer] = retained ? ResourceVersion{pending.identity[layer].resource, 1}
                : front.Write(pending.identity[layer], Access::RenderTarget);
            pending.optical_component[layer] = retained ? ResourceVersion{pending.optical_component[layer].resource, 1}
                : front.Write(pending.optical_component[layer], Access::RenderTarget);
    #if defined(HS_DEVELOPMENT_TOOLS)
            if (storage->work)
            {
                const auto bound = retained
                    ? storage->work->BindRetainedPhase(graph, inputs.frame_slot, layer, retained->work)
                    : Result::Success();
                if (!bound) return bound;
                if (!retained) storage->work->DeclarePhase(graph, front, inputs.frame_slot, layer,
                    layout ? &layout->work : nullptr);
            }
    #endif
            front.SetExecute([storage, inputs, layer, use_occupancy](RenderPassContext &context) {
                auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
                constexpr float clear[]{0, 0, 0, 0};
                const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{storage->Rtv(layer), storage->Rtv(4 + layer)};
                const auto depth = storage->Dsv(layer);
                commands->ClearDepthStencilView(depth, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                commands->ClearRenderTargetView(targets[0], clear, 0, nullptr);
                commands->ClearRenderTargetView(targets[1], clear, 0, nullptr);
                commands->OMSetRenderTargets(2, targets, FALSE, &depth);
                if (use_occupancy)
                {
                    commands->BeginQuery(storage->occupancy_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                        inputs.frame_slot * kLayeredTranslucencyMaxLayers + layer);
                    if (layer)
                        commands->SetPredication(storage->occupancy_results[inputs.frame_slot][layer - 1].resource.Get(),
                            0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                }
    #if defined(HS_DEVELOPMENT_TOOLS)
                if (storage->work) storage->work->Record(commands, storage->pipelines[0].Get(), inputs, LayeredTranslucencyPhase::Front, layer, layer, storage->Gpu(inputs, layer));
                else
    #endif
                    inputs.draw(commands, storage->pipelines[0].Get(), LayeredTranslucencyPhase::Front, layer, storage->Gpu(inputs, layer));
                if (use_occupancy)
                {
                    if (layer) commands->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                    const auto query = inputs.frame_slot * kLayeredTranslucencyMaxLayers + layer;
                    commands->EndQuery(storage->occupancy_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
                    commands->ResolveQueryData(storage->occupancy_queries.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                        query, 1, storage->occupancy_results[inputs.frame_slot][layer].resource.Get(), 0);
                }
            });
        }
        if (layout) layout->exit_pass = graph.PassCount();
        auto exit_pass = retained ? graph.BindPass(retained->exit_pass, 97)
            : graph.AddPass(97, "Translucency.GatherExitDepth", QueueHint::Direct);
        if (!exit_pass) return Failure(ErrorCode::InvalidState, "Translucency retained exit pass changed.");
        auto &exit = *exit_pass;
        if (!retained)
        {
            readScene(exit, false);
            for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
            {
                exit.Read(pending.front[layer], Access::ShaderRead);
                exit.Read(pending.identity[layer], Access::ShaderRead);
                exit.Read(pending.optical_component[layer], Access::ShaderRead);
            }
            if (use_occupancy) exit.Read(occupancy[0], Access::IndirectArgs);
        }
        pending.exit = retained ? ResourceVersion{pending.exit.resource, 1}
            : exit.Write(pending.exit, Access::RenderTarget);
    #if defined(HS_DEVELOPMENT_TOOLS)
        if (storage->work)
        {
            const auto bound = retained
                ? storage->work->BindRetainedPhase(graph, inputs.frame_slot, 4, retained->work)
                : Result::Success();
            if (!bound) return bound;
            if (!retained) storage->work->DeclarePhase(graph, exit, inputs.frame_slot, 4,
                layout ? &layout->work : nullptr);
        }
    #endif
        exit.SetExecute([storage, inputs, use_occupancy](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            constexpr float zero[]{0, 0, 0, 0};
            const auto target = storage->Rtv(8);
            commands->ClearRenderTargetView(target, zero, 0, nullptr);
            commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
            if (use_occupancy)
                commands->SetPredication(storage->occupancy_results[inputs.frame_slot][0].resource.Get(),
                    0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    #if defined(HS_DEVELOPMENT_TOOLS)
            if (storage->work) storage->work->Record(commands, storage->pipelines[1].Get(), inputs, LayeredTranslucencyPhase::Exit, 0, 4, storage->Gpu(inputs, 4));
            else
    #endif
                inputs.draw(commands, storage->pipelines[1].Get(), LayeredTranslucencyPhase::Exit, 0, storage->Gpu(inputs, 4));
            if (use_occupancy) commands->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        });
    }
    outputs.accumulation = inputs.accumulation.version;
    outputs.revealage = inputs.revealage.version;
    {
    if (layout) layout->tail_pass = graph.PassCount();
        auto tail_pass = retained ? graph.BindPass(retained->tail_pass, 99)
            : graph.AddPass(99, "Translucency.AccumulateUnpeeled", QueueHint::Direct);
        if (!tail_pass) return Failure(ErrorCode::InvalidState, "Translucency retained tail pass changed.");
        auto &tail = *tail_pass;
        if (!retained)
        {
            readScene(tail, true, Access::DepthShaderRead);
            tail.Read(inputs.hdr.version, Access::ShaderRead);
        }
        // Layer 0 and the tail use the same front-face, reserved-key, opaque-depth
        // and transport-opacity admission. If layer 0 has no accepted sample, no
        // tail sample can exist. Deeper emptiness alone is insufficient because
        // equal-depth, different-identity fragments may still reach the tail.
        if (use_occupancy && !retained) tail.Read(occupancy[0], Access::IndirectArgs);
        if (!retained) for (std::uint32_t layer = 0; layer < storage->layer_count; ++layer)
        {
            tail.Read(pending.front[layer], Access::ShaderRead); tail.Read(pending.identity[layer], Access::ShaderRead);
            tail.Read(pending.optical_component[layer], Access::ShaderRead);
        }
        outputs.accumulation = retained ? retained->accumulation_version
            : tail.ReadWrite(inputs.accumulation.version, Access::RenderTarget);
        outputs.revealage = retained ? retained->revealage_version
            : tail.ReadWrite(inputs.revealage.version, Access::RenderTarget);
        pending.revealage = outputs.revealage;
    #if defined(HS_DEVELOPMENT_TOOLS)
        if (storage->work)
        {
            const auto bound = retained
                ? storage->work->BindRetainedPhase(graph, inputs.frame_slot, kTailPhase, retained->work)
                : Result::Success();
            if (!bound) return bound;
            if (!retained) storage->work->DeclarePhase(graph, tail, inputs.frame_slot,
                kTailPhase, layout ? &layout->work : nullptr);
        }
    #endif
        tail.SetExecute([storage, inputs, use_occupancy](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{inputs.accumulation_rtv, inputs.revealage_rtv};
            commands->OMSetRenderTargets(2, targets, FALSE, &inputs.opaque_readonly_dsv);
            if (use_occupancy)
                commands->SetPredication(storage->occupancy_results[inputs.frame_slot][0].resource.Get(),
                    0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    #if defined(HS_DEVELOPMENT_TOOLS)
            if (storage->work) storage->work->Record(commands, storage->pipelines[2].Get(), inputs, LayeredTranslucencyPhase::Tail, 0, kTailPhase, storage->Gpu(inputs, kTailPhase));
            else
    #endif
                inputs.draw(commands, storage->pipelines[2].Get(), LayeredTranslucencyPhase::Tail, 0, storage->Gpu(inputs, kTailPhase));
            if (use_occupancy)
                commands->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        });
    }
    outputs.depth = pending.front[0]; outputs.motion = guides[1]; outputs.history = guides[2];
    outputs.classification = guides[3]; outputs.metadata = pending.identity[0]; outputs.transport = guides[5];
    outputs.depth_resource = storage->front[0].resource.Get(); outputs.motion_resource = storage->guides[1].resource.Get();
    outputs.history_resource = storage->guides[2].resource.Get(); outputs.classification_resource = storage->guides[3].resource.Get();
    outputs.metadata_resource = storage->identity[0].resource.Get();
    if (layout)
    {
        layout->accumulation_version = outputs.accumulation;
        layout->revealage_version = outputs.revealage;
    }
    impl_->pending = std::move(pending);
    return Result::Success();
}

Result TranslucencySystem::RegisterComposition(RenderGraphBuilder &graph,
    LayeredTranslucencyGuide background, LayeredTranslucencyOutputs &outputs,
    LayeredTranslucencyLayout *layout)
{
    return RegisterCompositionImpl(graph, background, outputs, layout, nullptr);
}
Result TranslucencySystem::BindRetainedComposition(RenderGraphBuilder &graph,
    LayeredTranslucencyGuide background, const LayeredTranslucencyLayout &layout,
    LayeredTranslucencyOutputs &outputs)
{
    return RegisterCompositionImpl(graph, background, outputs, nullptr, &layout);
}
bool TranslucencySystem::MatchesShape(const LayeredTranslucencyLayout &layout,
    bool use_occupancy_queries, std::size_t diagnostic_contributor_count) const noexcept
{
    const auto storage = impl_->targets;
#if defined(HS_DEVELOPMENT_TOOLS)
    const bool capture_work = storage && static_cast<bool>(storage->work);
#else
    constexpr bool capture_work = false;
#endif
    if (!storage || !storage->enabled || !storage->usable ||
        !layout.target_generation || layout.target_generation != storage->generation ||
        layout.layer_count != storage->layer_count ||
        layout.capture_work != capture_work ||
        layout.occupancy != (use_occupancy_queries && storage->occupancy_queries.Get()))
        return false;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (storage->work)
    {
        if (diagnostic_contributor_count != layout.work.contributor_count ||
            diagnostic_contributor_count > layout.work.argument_versions.size()) return false;
    }
#else
    (void)diagnostic_contributor_count;
#endif
    return true;
}
Result TranslucencySystem::RegisterCompositionImpl(RenderGraphBuilder &graph,
    LayeredTranslucencyGuide background, LayeredTranslucencyOutputs &outputs,
    LayeredTranslucencyLayout *layout, const LayeredTranslucencyLayout *retained)
{
    if (impl_->targets && !impl_->targets->enabled) return Result::Success();
    auto &pending = impl_->pending;
    if (pending.graph != &graph || pending.composed)
        return Failure(ErrorCode::InvalidState, "Register translucency layers once before their composition on the same graph.");
    auto storage = pending.targets;
    const auto &inputs = pending.inputs;
    const bool use_occupancy = inputs.use_occupancy_queries && storage->occupancy_queries.Get();
#if defined(HS_DEVELOPMENT_TOOLS)
    const bool capture_work = static_cast<bool>(storage->work);
#else
    constexpr bool capture_work = false;
#endif
    if (layout && (layout->target_generation != storage->generation ||
            layout->layer_count != storage->layer_count || layout->occupancy != use_occupancy ||
            layout->capture_work != capture_work))
        return Failure(ErrorCode::InvalidState, "Translucency composition layout variant changed.");
    if (retained)
    {
        const auto expected = retained->background;
        if (retained->target_generation != storage->generation ||
            retained->layer_count != storage->layer_count ||
            retained->occupancy != use_occupancy ||
            retained->capture_work != capture_work ||
            expected != background.version)
            return Failure(ErrorCode::InvalidState, "Translucency retained background or variant changed.");
    }
    if (!Fits(background, inputs.render_active, kColor))
        return Failure(ErrorCode::InvalidArgument, "Translucency composition requires an initialized RGBA16F background at the active extent.");
    if (layout) layout->background = background.version;
    auto source = background.version;
    auto *sourceResource = background.resource;
    WriteTextureSrv(storage->device.Get(), sourceResource, kColor,
        storage->Cpu(kCompositePhase + storage->layer_count - 1, 1 + HS_TRANSLUCENCY_SRV_BACKGROUND));
    outputs.descriptors = storage->Cpu(0, 0);
    // Occupancy is available from this frame's front peel only. A missing
    // front cannot write its corresponding composite, while the tail retains
    // its independent front-face/opacity path.
    for (std::uint32_t step = 0; step < storage->layer_count; ++step)
    {
        const auto layer = storage->layer_count - 1 - step, target = step % 2;
        const auto phase = kCompositePhase + layer;
        if (layout) layout->copy_pass[step] = graph.PassCount();
        auto copy_pass = retained ? graph.BindPass(retained->copy_pass[step], 101 + step * 2)
            : graph.AddPass(101 + step * 2, "Translucency.CopyBackground", QueueHint::Direct);
        if (!copy_pass) return Failure(ErrorCode::InvalidState, "Translucency retained copy pass changed.");
        auto &copy = *copy_pass;
        if (!retained) copy.Read(source, Access::CopySource);
        pending.color[target] = retained
            ? ResourceVersion{pending.color[target].resource, (step / 2) * 2 + 1}
            : copy.Write(pending.color[target], Access::CopyDest);
        copy.SetExecute([storage, sourceResource, target, step, active = inputs.render_active,
                            rect = inputs.composition_rect](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
            D3D12_TEXTURE_COPY_LOCATION destination{}, sourceLocation{};
            destination.pResource = storage->color[target].resource.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            sourceLocation.pResource = sourceResource; sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            // The first two full copies initialize both ping-pong targets this
            // frame. Thereafter their pixels outside the proven write rect are
            // the same background, including refracted source samples.
            const bool partial = step >= 2 && rect.right > rect.left && rect.bottom > rect.top;
            const D3D12_BOX box{partial ? static_cast<UINT>(rect.left) : 0u,
                partial ? static_cast<UINT>(rect.top) : 0u, 0u,
                partial ? static_cast<UINT>(rect.right) : active.width,
                partial ? static_cast<UINT>(rect.bottom) : active.height, 1u};
            commands->CopyTextureRegion(&destination, box.left, box.top, 0, &sourceLocation, &box);
        });
        if (layout) layout->composite_pass[step] = graph.PassCount();
        auto compose_pass = retained ? graph.BindPass(retained->composite_pass[step], 102 + step * 2)
            : graph.AddPass(102 + step * 2, layer == 0 ? "Translucency.CompositeFront" : "Translucency.CompositeLayer", QueueHint::Direct);
        if (!compose_pass) return Failure(ErrorCode::InvalidState, "Translucency retained compose pass changed.");
        auto &compose = *compose_pass;
        if (!retained)
        {
            compose.Read(source, Access::ShaderRead);
            compose.Read(inputs.opaque_depth.version, Access::ShaderRead);
            compose.Read(pending.front[layer], Access::DepthShaderRead);
            compose.Read(pending.identity[layer], Access::ShaderRead);
            compose.Read(pending.optical_component[layer], Access::ShaderRead);
            compose.Read(pending.exit, Access::ShaderRead);
            if (use_occupancy) compose.Read(pending.occupancy[layer], Access::IndirectArgs);
            if (layer + 1 < storage->layer_count)
                compose.Read(pending.front[layer + 1], Access::ShaderRead);
            for (const auto &read : pending.scene_reads) compose.Read(read.version, read.access);
            if (layer == 0) ReadMotion(compose, inputs);
        }
        pending.color[target] = retained
            ? ResourceVersion{pending.color[target].resource, (step / 2) * 2 + 2}
            : compose.ReadWrite(pending.color[target], Access::RenderTarget);
        if (layer == 0)
        {
            if (!retained) compose.Read(pending.revealage, Access::ShaderRead);
            for (const auto index : kCompositeGuideIndices)
                pending.guides[index] = retained ? ResourceVersion{pending.guides[index].resource, 1}
                    : compose.Write(pending.guides[index], Access::RenderTarget);
            outputs.depth = pending.front[0]; outputs.motion = pending.guides[1]; outputs.history = pending.guides[2];
            outputs.classification = pending.guides[3]; outputs.metadata = pending.identity[0]; outputs.transport = pending.guides[5];
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        if (storage->work)
        {
            const auto bound = retained
                ? storage->work->BindRetainedPhase(graph, inputs.frame_slot, phase, retained->work)
                : Result::Success();
            if (!bound) return bound;
            if (!retained) storage->work->DeclarePhase(graph, compose, inputs.frame_slot,
                phase, layout ? &layout->work : nullptr);
        }
#endif
        compose.SetExecute([storage, inputs, layer, phase, target, use_occupancy](RenderPassContext &context) {
            auto *commands = static_cast<ID3D12GraphicsCommandList *>(context.NativeCommandList());
        const auto rtv = storage->Rtv(13 + target);
            const auto depth = storage->Dsv(8 + layer);
            if (layer == 0)
            {
                std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1 + kCompositeGuideIndices.size()> targets{rtv};
                constexpr float zero[]{0, 0, 0, 0};
                for (std::uint32_t index = 0; index < kCompositeGuideIndices.size(); ++index)
                {
                    const auto guideIndex = kCompositeGuideIndices[index];
                    targets[1 + index] = storage->GuideRtv(guideIndex);
                    commands->ClearRenderTargetView(targets[1 + index], zero, 0, nullptr);
                }
                commands->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), FALSE, &depth);
            }
            else commands->OMSetRenderTargets(1, &rtv, FALSE, &depth);
            auto *pipeline = storage->pipelines[layer == 0 ? 3 : 4].Get();
            if (use_occupancy)
                commands->SetPredication(storage->occupancy_results[inputs.frame_slot][layer].resource.Get(),
                    0, D3D12_PREDICATION_OP_EQUAL_ZERO);
#if defined(HS_DEVELOPMENT_TOOLS)
            if (storage->work) storage->work->Record(commands, pipeline, inputs, LayeredTranslucencyPhase::Composite, layer, phase, storage->Gpu(inputs, phase));
            else
#endif
                inputs.draw(commands, pipeline, LayeredTranslucencyPhase::Composite, layer, storage->Gpu(inputs, phase));
            if (use_occupancy)
                commands->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        });
        source = pending.color[target]; sourceResource = storage->color[target].resource.Get();
        outputs.corrected = source; outputs.corrected_resource = sourceResource; outputs.corrected_srv = storage->Export(target);
    }
    pending.composed = true;
    return Result::Success();
}

Result TranslucencySystem::CompleteSubmission(const RenderGraphBuilder &graph, const CompletionSet &completion, bool success)
{
    auto &pending = impl_->pending;
    if (!pending.graph)
    {
        if (impl_->targets && !impl_->targets->enabled) return Result::Success();
        return Failure(ErrorCode::InvalidState, "Translucency transparency has no registered submission.");
    }
    if (pending.graph != &graph) return Failure(ErrorCode::InvalidArgument, "Translucency transparency completion refers to another graph.");
    auto storage = pending.targets;
    storage->completion.Merge(completion); storage->slot_completion[pending.inputs.frame_slot].Merge(completion);
    for (const auto &imported : pending.imports)
    {
        const auto resourceCompletion = graph.ResourceCompletion(imported.handle);
        storage->completion.Merge(resourceCompletion); storage->slot_completion[pending.inputs.frame_slot].Merge(resourceCompletion);
        imported.allocation->graph_completion.Merge(completion); imported.allocation->graph_completion.Merge(resourceCompletion);
        if (success)
        {
            imported.allocation->graph_access = graph.FinalAccess(imported.handle);
            imported.allocation->graph_queue = graph.FinalQueue(imported.handle);
        }
    }
    if (!success) storage->usable = false;
#if defined(HS_DEVELOPMENT_TOOLS)
    if (storage->work) storage->work->Submitted(graph, pending.inputs.frame_slot, completion, success);
    impl_->diagnostic_submission_valid = success && pending.composed;
    if (impl_->diagnostic_submission_valid)
    {
        impl_->diagnostic_color_index = (storage->layer_count - 1u) % 2u;
    }
#endif
    pending = {};
    return Result::Success();
}
} // namespace hs
