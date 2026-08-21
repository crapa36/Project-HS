#include "render_graph.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <WinPixEventRuntime/pix3.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace hs
{
namespace
{

struct ResourceUse
{
    ResourceHandle resource;
    Access access{};
    bool writes{};
};

struct LogicalResource
{
    std::string name;
    ResourceKind kind{};
    void *native{};
    Access initial{};
    Access current{};
    Access final{};
    bool imported{};
    bool written{};
};

struct Pass
{
    std::string name;
    QueueHint queue{};
    std::vector<ResourceUse> uses;
    std::move_only_function<void(RenderPassContext &)> execute;
};

[[nodiscard]] D3D12_RESOURCE_STATES LegacyState(ResourceKind kind, Access access) noexcept
{
    switch (access)
    {
    case Access::Common:
        return D3D12_RESOURCE_STATE_COMMON;
    case Access::ShaderRead:
        return kind == ResourceKind::Texture ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                             : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case Access::RenderTarget:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case Access::DepthRead:
        return D3D12_RESOURCE_STATE_DEPTH_READ;
    case Access::DepthWrite:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case Access::CopySource:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case Access::CopyDest:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case Access::IndirectArgs:
        return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    case Access::Present:
        return D3D12_RESOURCE_STATE_PRESENT;
    default:
        return D3D12_RESOURCE_STATE_COMMON;
    }
}

[[nodiscard]] D3D12_BARRIER_SYNC BarrierSync(Access access) noexcept
{
    switch (access)
    {
    case Access::Common:
        return D3D12_BARRIER_SYNC_ALL;
    case Access::ShaderRead:
        return D3D12_BARRIER_SYNC_ALL_SHADING;
    case Access::RenderTarget:
        return D3D12_BARRIER_SYNC_RENDER_TARGET;
    case Access::DepthRead:
    case Access::DepthWrite:
        return D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        return D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    case Access::CopySource:
    case Access::CopyDest:
        return D3D12_BARRIER_SYNC_COPY;
    case Access::IndirectArgs:
        return D3D12_BARRIER_SYNC_EXECUTE_INDIRECT;
    case Access::Present:
    case Access::Undefined:
        return D3D12_BARRIER_SYNC_NONE;
    }
    return D3D12_BARRIER_SYNC_ALL;
}

[[nodiscard]] D3D12_BARRIER_ACCESS BarrierAccess(Access access) noexcept
{
    switch (access)
    {
    case Access::Common:
        return D3D12_BARRIER_ACCESS_COMMON;
    case Access::ShaderRead:
        return D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    case Access::RenderTarget:
        return D3D12_BARRIER_ACCESS_RENDER_TARGET;
    case Access::DepthRead:
        return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    case Access::DepthWrite:
        return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        return D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    case Access::CopySource:
        return D3D12_BARRIER_ACCESS_COPY_SOURCE;
    case Access::CopyDest:
        return D3D12_BARRIER_ACCESS_COPY_DEST;
    case Access::IndirectArgs:
        return D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT;
    case Access::Present:
    case Access::Undefined:
        return D3D12_BARRIER_ACCESS_NO_ACCESS;
    }
    return D3D12_BARRIER_ACCESS_COMMON;
}

[[nodiscard]] D3D12_BARRIER_LAYOUT BarrierLayout(Access access) noexcept
{
    switch (access)
    {
    case Access::Common:
        return D3D12_BARRIER_LAYOUT_COMMON;
    case Access::ShaderRead:
        return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    case Access::RenderTarget:
        return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    case Access::DepthRead:
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
    case Access::DepthWrite:
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    case Access::CopySource:
        return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    case Access::CopyDest:
        return D3D12_BARRIER_LAYOUT_COPY_DEST;
    case Access::Present:
        return D3D12_BARRIER_LAYOUT_PRESENT;
    default:
        return D3D12_BARRIER_LAYOUT_COMMON;
    }
}

void Transition(ID3D12GraphicsCommandList *commands, ID3D12GraphicsCommandList7 *enhanced,
                BarrierMode mode, const LogicalResource &resource, Access before, Access after)
{
    if (!resource.native || before == after)
    {
        return;
    }

    auto *native = static_cast<ID3D12Resource *>(resource.native);
    if (mode == BarrierMode::Enhanced)
    {
        if (resource.kind == ResourceKind::Texture)
        {
            D3D12_TEXTURE_BARRIER barrier{};
            barrier.SyncBefore = BarrierSync(before);
            barrier.SyncAfter = BarrierSync(after);
            barrier.AccessBefore = BarrierAccess(before);
            barrier.AccessAfter = BarrierAccess(after);
            barrier.LayoutBefore = BarrierLayout(before);
            barrier.LayoutAfter = BarrierLayout(after);
            barrier.pResource = native;
            const auto description = native->GetDesc();
            barrier.Subresources = {0, description.MipLevels, 0, description.DepthOrArraySize, 0,
                                    1};
            D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1};
            group.pTextureBarriers = &barrier;
            enhanced->Barrier(1, &group);
        }
        else
        {
            D3D12_BUFFER_BARRIER barrier{};
            barrier.SyncBefore = BarrierSync(before);
            barrier.SyncAfter = BarrierSync(after);
            barrier.AccessBefore = BarrierAccess(before);
            barrier.AccessAfter = BarrierAccess(after);
            barrier.pResource = native;
            barrier.Offset = 0;
            barrier.Size = UINT64_MAX;
            D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
            group.pBufferBarriers = &barrier;
            enhanced->Barrier(1, &group);
        }
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {native, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          LegacyState(resource.kind, before), LegacyState(resource.kind, after)};
    commands->ResourceBarrier(1, &barrier);
}

} // namespace

struct RenderGraphBuilder::Impl
{
    [[nodiscard]] LogicalResource *Find(ResourceHandle handle) noexcept
    {
        if (!handle || handle.value > resources.size())
        {
            return nullptr;
        }
        auto &resource = resources[handle.value - 1];
        return resource.kind == handle.kind ? &resource : nullptr;
    }

    [[nodiscard]] const LogicalResource *Find(ResourceHandle handle) const noexcept
    {
        return const_cast<Impl *>(this)->Find(handle);
    }

    std::vector<LogicalResource> resources;
    std::vector<Pass> passes;
};

RenderPassContext::RenderPassContext(void *native_command_list, void *enhanced_command_list,
                                     BarrierMode barrier_mode, const void *graph) noexcept
    : native_command_list_(native_command_list), enhanced_command_list_(enhanced_command_list),
      barrier_mode_(barrier_mode), graph_(graph)
{
}

void *RenderPassContext::NativeCommandList() const noexcept
{
    return native_command_list_;
}

void *RenderPassContext::Resolve(ResourceHandle handle) const noexcept
{
    const auto *graph = static_cast<const RenderGraphBuilder::Impl *>(graph_);
    const auto *resource = graph ? graph->Find(handle) : nullptr;
    return resource ? resource->native : nullptr;
}

void RenderPassContext::UavBarrier(ResourceHandle handle) const noexcept
{
    const auto *graph = static_cast<const RenderGraphBuilder::Impl *>(graph_);
    const auto *resource = graph ? graph->Find(handle) : nullptr;
    if (!resource || !resource->native)
    {
        return;
    }
    if (barrier_mode_ == BarrierMode::Enhanced)
    {
        auto *enhanced = static_cast<ID3D12GraphicsCommandList7 *>(enhanced_command_list_);
        D3D12_BUFFER_BARRIER barrier{};
        barrier.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        barrier.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        barrier.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        barrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        barrier.pResource = static_cast<ID3D12Resource *>(resource->native);
        barrier.Offset = 0;
        barrier.Size = UINT64_MAX;
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
        group.pBufferBarriers = &barrier;
        enhanced->Barrier(1, &group);
        return;
    }

    auto *commands = static_cast<ID3D12GraphicsCommandList *>(native_command_list_);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = static_cast<ID3D12Resource *>(resource->native);
    commands->ResourceBarrier(1, &barrier);
}

PassBuilder::PassBuilder(void *graph, std::uint32_t pass) noexcept : graph_(graph), pass_(pass)
{
}

void PassBuilder::Read(ResourceHandle resource, Access access)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {resource, access, false});
}

void PassBuilder::Write(ResourceHandle resource, Access access)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {resource, access, true});
}

void PassBuilder::ReadWrite(ResourceHandle resource, Access access)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {resource, access, true});
}

void PassBuilder::SetExecute(std::move_only_function<void(RenderPassContext &)> execute)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].execute = std::move(execute);
}

RenderGraphBuilder::RenderGraphBuilder() : impl_(std::make_unique<Impl>())
{
    impl_->resources.reserve(32);
    impl_->passes.reserve(16);
}

RenderGraphBuilder::~RenderGraphBuilder() = default;

void RenderGraphBuilder::Reset()
{
    impl_->resources.clear();
    impl_->passes.clear();
}

TextureHandle RenderGraphBuilder::CreateTexture(const TextureDesc &, std::string_view name)
{
    impl_->resources.push_back(
        {std::string(name), ResourceKind::Texture, nullptr, Access::Undefined, Access::Undefined,
         Access::Undefined, false, false});
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Texture};
}

BufferHandle RenderGraphBuilder::CreateBuffer(const BufferDesc &, std::string_view name)
{
    impl_->resources.push_back(
        {std::string(name), ResourceKind::Buffer, nullptr, Access::Undefined, Access::Undefined,
         Access::Undefined, false, false});
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Buffer};
}

TextureHandle RenderGraphBuilder::ImportTexture(const ExternalTexture &texture, Access initial,
                                                std::string_view name)
{
    impl_->resources.push_back({std::string(name), ResourceKind::Texture, texture.native_resource,
                                initial, initial, initial, true, true});
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Texture};
}

BufferHandle RenderGraphBuilder::ImportBuffer(const ExternalBuffer &buffer, Access initial,
                                              std::string_view name)
{
    impl_->resources.push_back({std::string(name), ResourceKind::Buffer, buffer.native_resource,
                                initial, initial, initial, true, true});
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Buffer};
}

PassBuilder RenderGraphBuilder::AddPass(std::string_view name, QueueHint queue)
{
    impl_->passes.push_back({std::string(name), queue, {}, {}});
    return PassBuilder(impl_.get(), static_cast<std::uint32_t>(impl_->passes.size() - 1));
}

void RenderGraphBuilder::SetFinalAccess(ResourceHandle resource, Access access)
{
    if (auto *logical = impl_->Find(resource))
    {
        logical->final = access;
    }
}

Result RenderGraphBuilder::Execute(void *native_command_list, void *enhanced_command_list,
                                   BarrierMode barriers, void *timestamp_query_heap,
                                   void *timestamp_readback, std::uint32_t timestamp_base)
{
    auto *commands = static_cast<ID3D12GraphicsCommandList *>(native_command_list);
    auto *enhanced = static_cast<ID3D12GraphicsCommandList7 *>(enhanced_command_list);
    auto *queries = static_cast<ID3D12QueryHeap *>(timestamp_query_heap);
    auto *readback = static_cast<ID3D12Resource *>(timestamp_readback);
    if (!commands || (barriers == BarrierMode::Enhanced && !enhanced))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "RenderGraph requires a compatible command list.");
    }

    for (auto &resource : impl_->resources)
    {
        resource.current = resource.initial;
        resource.written = resource.imported;
    }

    RenderPassContext context(commands, enhanced, barriers, impl_.get());
    std::uint32_t pass_index{};
    for (auto &pass : impl_->passes)
    {
        if (pass.queue != QueueHint::Direct)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "Stage 1 RenderGraph supports Direct queue passes only.");
        }
        if (!pass.execute)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "RenderGraph pass has no execute callback.");
        }

        for (std::size_t left = 0; left < pass.uses.size(); ++left)
        {
            const auto &use = pass.uses[left];
            auto *resource = impl_->Find(use.resource);
            if (!resource || use.access == Access::Undefined)
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "RenderGraph pass has an invalid resource access.");
            }
            for (std::size_t right = left + 1; right < pass.uses.size(); ++right)
            {
                if (pass.uses[right].resource.value == use.resource.value)
                {
                    return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                           "RenderGraph pass declares one resource twice.");
                }
            }
            if (!resource->written && !use.writes)
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "RenderGraph reads a transient resource before writing it.");
            }
            Transition(commands, enhanced, barriers, *resource, resource->current, use.access);
            resource->current = use.access;
            resource->written |= use.writes;
        }

        if (queries)
        {
            commands->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP,
                               timestamp_base + pass_index * 2);
        }
        PIXBeginEvent(commands, PIX_COLOR_DEFAULT, "%s", pass.name.c_str());
        pass.execute(context);
        PIXEndEvent(commands);
        if (queries)
        {
            commands->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP,
                               timestamp_base + pass_index * 2 + 1);
        }
        ++pass_index;
    }

    for (auto &resource : impl_->resources)
    {
        const auto target = resource.final == Access::Undefined ? resource.current : resource.final;
        Transition(commands, enhanced, barriers, resource, resource.current, target);
        resource.current = target;
    }
    if (queries && readback)
    {
        commands->ResolveQueryData(
            queries, D3D12_QUERY_TYPE_TIMESTAMP, timestamp_base,
            static_cast<UINT>(impl_->passes.size() * 2), readback,
            static_cast<UINT64>(timestamp_base) * sizeof(std::uint64_t));
    }
    return Result::Success();
}

std::uint32_t RenderGraphBuilder::PassCount() const noexcept
{
    return static_cast<std::uint32_t>(impl_->passes.size());
}

Access RenderGraphBuilder::FinalAccess(ResourceHandle resource) const noexcept
{
    const auto *logical = impl_->Find(resource);
    return logical ? logical->current : Access::Undefined;
}

} // namespace hs
