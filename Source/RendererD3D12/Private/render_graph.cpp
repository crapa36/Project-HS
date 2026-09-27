#include "render_graph.hpp"
#include "d3d12_resources.hpp"
#include <limits>

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
    bool reads{};
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
    TextureDesc texture{};
    BufferDesc buffer{};
    std::size_t pool_index{SIZE_MAX};
    std::uint32_t first_use{UINT32_MAX}, last_use{};
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
    case Access::ComputeRead:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
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
    case Access::ComputeRead:
        return D3D12_BARRIER_SYNC_COMPUTE_SHADING;
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
    case Access::ComputeRead:
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
    case Access::ComputeRead:
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

struct TransientResourcePool::Impl
{
    struct Entry
    {
        ResourceKind kind{};
        TextureDesc texture{};
        BufferDesc buffer{};
        AllocationResource allocation;
        Access state{Access::Common};
        bool claimed{};
    };
    D3D12MA::Allocator *allocator{};
    std::vector<Entry> entries;
};
TransientResourcePool::TransientResourcePool() : impl_(std::make_unique<Impl>()) {}
TransientResourcePool::~TransientResourcePool() = default;
Result TransientResourcePool::Initialize(D3D12MA::Allocator *allocator)
{
    Clear(); impl_->allocator = allocator;
    return Result::Success();
}
void TransientResourcePool::ResetClaims() noexcept
{
    for (auto &entry : impl_->entries) entry.claimed = false;
}
void TransientResourcePool::Clear() noexcept { impl_->entries.clear(); }

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

    bool prepared{};
    TransientResourcePool::Impl *pool{};
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
        if (resource->kind == ResourceKind::Texture)
        {
            D3D12_GLOBAL_BARRIER barrier{};
            barrier.SyncBefore = barrier.SyncAfter = D3D12_BARRIER_SYNC_ALL_SHADING;
            barrier.AccessBefore = barrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
            D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_GLOBAL, 1};
            group.pGlobalBarriers = &barrier;
            enhanced->Barrier(1, &group);
            return;
        }
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
    static_cast<RenderGraphBuilder::Impl *>(graph_)->prepared = false;
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {resource, access, false, true});
}

void PassBuilder::Write(ResourceHandle resource, Access access)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->prepared = false;
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {resource, access, true, false});
}

void PassBuilder::ReadWrite(ResourceHandle resource, Access access)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->prepared = false;
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {resource, access, true, true});
}

void PassBuilder::SetExecute(std::move_only_function<void(RenderPassContext &)> execute)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->prepared = false;
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
    impl_->prepared = false;
    impl_->pool = nullptr;
    impl_->resources.clear();
    impl_->passes.clear();
}

TextureHandle RenderGraphBuilder::CreateTexture(const TextureDesc &description, std::string_view name)
{
    impl_->prepared = false;
    impl_->resources.push_back(
        {std::string(name), ResourceKind::Texture, nullptr, Access::Undefined, Access::Undefined,
         Access::Undefined, false, false});
    impl_->resources.back().texture = description;
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Texture};
}

BufferHandle RenderGraphBuilder::CreateBuffer(const BufferDesc &description, std::string_view name)
{
    impl_->prepared = false;
    impl_->resources.push_back(
        {std::string(name), ResourceKind::Buffer, nullptr, Access::Undefined, Access::Undefined,
         Access::Undefined, false, false});
    impl_->resources.back().buffer = description;
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Buffer};
}

TextureHandle RenderGraphBuilder::ImportTexture(const ExternalTexture &texture, Access initial,
                                                std::string_view name)
{
    impl_->prepared = false;
    impl_->resources.push_back({std::string(name), ResourceKind::Texture, texture.native_resource,
                                initial, initial, initial, true, true});
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Texture};
}

BufferHandle RenderGraphBuilder::ImportBuffer(const ExternalBuffer &buffer, Access initial,
                                              std::string_view name)
{
    impl_->prepared = false;
    impl_->resources.push_back({std::string(name), ResourceKind::Buffer, buffer.native_resource,
                                initial, initial, initial, true, true});
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Buffer};
}

PassBuilder RenderGraphBuilder::AddPass(std::string_view name, QueueHint queue)
{
    impl_->prepared = false;
    impl_->passes.push_back({std::string(name), queue, {}, {}});
    return PassBuilder(impl_.get(), static_cast<std::uint32_t>(impl_->passes.size() - 1));
}

void RenderGraphBuilder::SetFinalAccess(ResourceHandle resource, Access access)
{
    impl_->prepared = false;
    if (auto *logical = impl_->Find(resource))
    {
        logical->final = access;
    }
}

void *RenderGraphBuilder::Resolve(ResourceHandle handle) const noexcept
{
    const auto *resource = impl_->prepared ? impl_->Find(handle) : nullptr;
    return resource ? resource->native : nullptr;
}

Result RenderGraphBuilder::Prepare(TransientResourcePool &pool)
{
    impl_->prepared = false;
    auto fail = [](std::string message) { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message)); };
    for (auto &resource : impl_->resources)
    {
        resource.first_use = UINT32_MAX; resource.last_use = 0;
        resource.written = resource.imported;
        if (resource.imported)
        {
            if (!resource.native || resource.initial == Access::Undefined)
                return fail("RenderGraph import requires a native resource and known initial access.");
            const auto desc = static_cast<ID3D12Resource *>(resource.native)->GetDesc();
            if ((desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) != (resource.kind == ResourceKind::Buffer))
                return fail("RenderGraph imported resource kind disagrees with its native descriptor.");
        }
        else if (resource.kind == ResourceKind::Texture)
        {
            const auto &d = resource.texture;
            if (!d.width || !d.height || d.width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                d.height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || !d.format ||
                (d.flags & ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) ||
                ((d.flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
                 (d.flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))))
                return fail("RenderGraph transient texture descriptor is invalid.");
        }
        else if (!resource.buffer.size || resource.buffer.flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
            return fail("RenderGraph transient buffer descriptor is invalid.");
    }
    const auto compatible = [](const LogicalResource &r, Access access) {
        if (access == Access::Undefined || static_cast<unsigned>(access) > static_cast<unsigned>(Access::Present)) return false;
        const auto desc = r.imported ? static_cast<ID3D12Resource *>(r.native)->GetDesc() : D3D12_RESOURCE_DESC{};
        const auto flags = r.imported ? static_cast<unsigned>(desc.Flags) : (r.kind == ResourceKind::Texture ? r.texture.flags : r.buffer.flags);
        switch (access)
        {
        case Access::RenderTarget: return r.kind == ResourceKind::Texture && (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        case Access::DepthRead: case Access::DepthWrite: return r.kind == ResourceKind::Texture && (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        case Access::UnorderedRead: case Access::UnorderedWrite: return (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        case Access::ShaderRead: case Access::ComputeRead: return !(flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        case Access::IndirectArgs: return r.kind == ResourceKind::Buffer;
        case Access::Present: return r.imported && r.kind == ResourceKind::Texture;
        default: return true;
        }
    };
    for (std::uint32_t index = 0; index < impl_->passes.size(); ++index)
    {
        const auto &pass = impl_->passes[index];
        if (pass.queue != QueueHint::Direct || !pass.execute) return fail("RenderGraph requires Direct passes with execute callbacks.");
        for (std::size_t i = 0; i < pass.uses.size(); ++i)
        {
            const auto &use = pass.uses[i]; auto *r = impl_->Find(use.resource);
            if (!r || !compatible(*r, use.access)) return fail("RenderGraph resource access is incompatible with its descriptor.");
            const bool write_access = use.access == Access::RenderTarget || use.access == Access::DepthWrite ||
                use.access == Access::UnorderedWrite || use.access == Access::CopyDest;
            if (use.writes != write_access)
                return fail("RenderGraph read/write declaration disagrees with its access.");
            for (std::size_t j = 0; j < i; ++j)
                if (pass.uses[j].resource.value == use.resource.value) return fail("RenderGraph pass declares one resource twice.");
            if (!r->written && use.reads) return fail("RenderGraph reads a transient before writing it.");
            if (r->first_use == UINT32_MAX) r->first_use = index;
            r->last_use = index; r->written |= use.writes;
        }
    }
    for (const auto &r : impl_->resources)
        if (r.final != Access::Undefined && !compatible(r, r.final)) return fail("RenderGraph final access is incompatible with its descriptor.");
    // Validation is complete before any allocation or command callback.
    auto &entries = pool.impl_->entries;
    std::vector<std::size_t> claims;
    const auto allocation_failure = [&](std::string message) {
        for (const auto index : claims) entries[index].claimed = false;
        for (auto &logical : impl_->resources)
            if (!logical.imported) { logical.native = nullptr; logical.pool_index = SIZE_MAX; }
        return fail(message);
    };
    for (auto &r : impl_->resources)
    {
        if (r.imported || r.first_use == UINT32_MAX) continue;
        if (!pool.impl_->allocator) return allocation_failure("RenderGraph transient pool has no allocator.");
        auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &e) {
            return !e.claimed && e.kind == r.kind && (r.kind == ResourceKind::Texture
                ? e.texture.width == r.texture.width && e.texture.height == r.texture.height &&
                  e.texture.format == r.texture.format && e.texture.flags == r.texture.flags
                : e.buffer.size == r.buffer.size && e.buffer.flags == r.buffer.flags);
        });
        if (found == entries.end())
        {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = r.kind == ResourceKind::Texture ? D3D12_RESOURCE_DIMENSION_TEXTURE2D : D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = r.kind == ResourceKind::Texture ? r.texture.width : r.buffer.size;
            desc.Height = r.kind == ResourceKind::Texture ? r.texture.height : 1;
            desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
            desc.Format = r.kind == ResourceKind::Texture ? static_cast<DXGI_FORMAT>(r.texture.format) : DXGI_FORMAT_UNKNOWN;
            desc.Layout = r.kind == ResourceKind::Texture ? D3D12_TEXTURE_LAYOUT_UNKNOWN : D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(r.kind == ResourceKind::Texture ? r.texture.flags : r.buffer.flags);
            TransientResourcePool::Impl::Entry entry;
            entry.kind = r.kind; entry.texture = r.texture; entry.buffer = r.buffer;
            D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            const auto hr = pool.impl_->allocator->CreateResource(&allocation, &desc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, &entry.allocation.allocation, IID_PPV_ARGS(&entry.allocation.resource));
            if (FAILED(hr)) return allocation_failure("RenderGraph transient allocation failed for " + r.name + " (HRESULT " + std::to_string(hr) + ").");
            entries.push_back(std::move(entry)); found = entries.end() - 1;
        }
        found->claimed = true;
        r.pool_index = static_cast<std::size_t>(found - entries.begin());
        claims.push_back(r.pool_index);
        r.native = found->allocation.resource.Get(); r.initial = found->state;
    }
    impl_->pool = pool.impl_.get(); impl_->prepared = true;
    return Result::Success();
}

Result RenderGraphBuilder::Execute(void *native_command_list, void *enhanced_command_list,
                                   BarrierMode barriers, void *timestamp_query_heap,
                                   void *timestamp_readback, std::uint32_t timestamp_base)
{
    auto *commands = static_cast<ID3D12GraphicsCommandList *>(native_command_list);
    auto *enhanced = static_cast<ID3D12GraphicsCommandList7 *>(enhanced_command_list);
    auto *queries = static_cast<ID3D12QueryHeap *>(timestamp_query_heap);
    auto *readback = static_cast<ID3D12Resource *>(timestamp_readback);
    if (!impl_->prepared || !commands || (barriers == BarrierMode::Enhanced && !enhanced))
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
            if (!resource->written && use.reads)
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
        if (!resource.imported && resource.first_use == UINT32_MAX) continue;
        const auto target = resource.final == Access::Undefined ? resource.current : resource.final;
        Transition(commands, enhanced, barriers, resource, resource.current, target);
        resource.current = target;
        if (!resource.imported) impl_->pool->entries[resource.pool_index].state = target;
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
