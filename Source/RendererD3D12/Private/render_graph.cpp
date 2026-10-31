#include "render_graph.hpp"
#include "render_graph_validation.hpp"
#include "render_graph_membership.hpp"
#include "d3d12_resources.hpp"
#include <hs/renderer/renderer.hpp>
#include <limits>

#include <Windows.h>
#include <d3d12.h>

#if defined(HS_DEVELOPMENT_TOOLS)
#include <WinPixEventRuntime/pix3.h>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <exception>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace hs
{

namespace
{

enum class Preparation
{
    Building,
    Ready,
    Failed,
};

using render_graph::ResourceUse;

[[nodiscard]] constexpr std::size_t QueueIndex(QueueHint queue) noexcept
{
    return static_cast<std::size_t>(queue);
}

[[nodiscard]] constexpr D3D12_COMMAND_LIST_TYPE NativeQueueType(QueueHint queue) noexcept
{
    switch (queue)
    {
    case QueueHint::Direct:
        return D3D12_COMMAND_LIST_TYPE_DIRECT;
    case QueueHint::Compute:
        return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    case QueueHint::Copy:
        return D3D12_COMMAND_LIST_TYPE_COPY;
    }
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

[[nodiscard]] constexpr const char *QueueName(QueueHint queue) noexcept
{
    switch (queue)
    {
    case QueueHint::Direct:
        return "Direct";
    case QueueHint::Compute:
        return "Compute";
    case QueueHint::Copy:
        return "Copy";
    }
    return "Unknown";
}

[[nodiscard]] Result QueueFailure(std::string_view operation, HRESULT result)
{
    return Result::Failure(
        ErrorCode::InvalidState, "hs_renderer_d3d12",
        std::string(operation) + " failed (HRESULT " +
            std::to_string(static_cast<std::int64_t>(result)) + ").");
}

[[nodiscard]] Result RemovedFenceFailure(ID3D12Device *device, std::string_view operation)
{
    const auto reason = device->GetDeviceRemovedReason();
    return QueueFailure(operation, FAILED(reason) ? reason : DXGI_ERROR_DEVICE_REMOVED);
}

[[nodiscard]] Result WaitForFence(ID3D12Device *device, ID3D12Fence *fence,
                                  std::uint64_t required, HANDLE event,
                                  std::string_view operation)
{
    auto completed = fence->GetCompletedValue();
    // Device removal wakes fence events with UINT64_MAX, without retiring work.
    if (completed == UINT64_MAX) return RemovedFenceFailure(device, operation);
    if (completed >= required) return Result::Success();
    const auto watched = fence->SetEventOnCompletion(required, event);
    if (FAILED(watched)) return QueueFailure(operation, watched);
    const auto wake = WaitForSingleObject(event, INFINITE);
    completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return RemovedFenceFailure(device, operation);
    if (wake != WAIT_OBJECT_0)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::string(operation) + " event failed.");
    if (completed < required)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               std::string(operation) + " event did not complete the required fence value.");
    return Result::Success();
}

[[nodiscard]] D3D12_RESOURCE_DESC NativeDesc(ResourceKind kind, const TextureDesc &texture,
                                               const BufferDesc &buffer) noexcept
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = kind == ResourceKind::Texture ? D3D12_RESOURCE_DIMENSION_TEXTURE2D
                                                    : D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kind == ResourceKind::Texture ? texture.width : buffer.size;
    desc.Height = kind == ResourceKind::Texture ? texture.height : 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = kind == ResourceKind::Texture ? static_cast<DXGI_FORMAT>(texture.format)
                                                 : DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = kind == ResourceKind::Texture ? D3D12_TEXTURE_LAYOUT_UNKNOWN
                                                 : D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(kind == ResourceKind::Texture
                                                        ? texture.flags : buffer.flags);
    return desc;
}

[[nodiscard]] D3D12_HEAP_FLAGS HeapFlags(ResourceKind kind, std::uint32_t flags) noexcept
{
    if (kind == ResourceKind::Buffer) return D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    if (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ||
        flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
        return D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    return D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
}

struct LogicalResource
{
    std::string name;
    [[nodiscard]] const std::string &Name() const noexcept { return cached_name ? *cached_name : name; }
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
    bool final_set{};
    ResourceVersion final_version{};
    CompletionSet imported_completion{};
    CompletionSet completion{};
    QueueHint initial_queue{QueueHint::Direct};
    QueueHint final_queue{QueueHint::Direct};
    bool alias_activation{};
    std::size_t pool_view_index{SIZE_MAX};
    ExternalQueueCompletion imported_external_completion;
    std::uintptr_t imported_identity{};
    D3D12_HEAP_TYPE fixed_buffer_heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC native_description{};
    const std::string *cached_name{};
    NativeResourceContract native_contract{};
    bool certified{};
};

[[nodiscard]] D3D12_HEAP_TYPE FixedBufferHeap(const LogicalResource &resource) noexcept
{
    return resource.fixed_buffer_heap;
}

[[nodiscard]] bool BufferDecays(const LogicalResource &resource) noexcept
{
    return resource.kind == ResourceKind::Buffer &&
        FixedBufferHeap(resource) == D3D12_HEAP_TYPE_DEFAULT;
}

[[nodiscard]] bool IsMemoryWrite(Access access) noexcept
{
    return access == Access::UnorderedWrite;
}

struct ExternalOperation
{
    ExternalQueueBinding binding;
    std::vector<ExternalResourceAccess> accesses;
    std::move_only_function<Result(RenderExternalPassContext &, ExternalQueueCompletion &)> execute;
};

struct Pass
{
    std::uint32_t id{};
    std::string name;
    QueueHint queue{};
    std::vector<ResourceUse> uses;
    [[nodiscard]] const std::string &Name() const noexcept { return cached_name ? *cached_name : name; }
    [[nodiscard]] const std::vector<ResourceUse> &Uses() const noexcept { return cached_uses ? *cached_uses : uses; }
    std::move_only_function<void(RenderPassContext &)> execute;
    std::shared_ptr<ExternalOperation> external;
    std::move_only_function<Result(RenderPassContext &)> owner_execute;
    bool execute_on_owner{};
    const std::string *cached_name{};
    const std::vector<ResourceUse> *cached_uses{};
};

bool SameVersion(ResourceVersion a, ResourceVersion b) noexcept
{
    return a.resource.value == b.resource.value && a.resource.kind == b.resource.kind && a.ordinal == b.ordinal;
}

bool SameUse(const ResourceUse &a, const ResourceUse &b) noexcept
{
    if (!SameVersion(a.predecessor, b.predecessor) || !SameVersion(a.successor, b.successor) ||
        a.access != b.access || a.writes != b.writes || a.reads != b.reads || a.range.index() != b.range.index()) return false;
    if (const auto *left = std::get_if<BufferRange>(&a.range))
    {
        const auto &right = std::get<BufferRange>(b.range);
        return left->offset == right.offset && left->size == right.size;
    }
    if (const auto *left = std::get_if<TextureSubresourceRange>(&a.range))
    {
        const auto &right = std::get<TextureSubresourceRange>(b.range);
        return std::tie(left->first_mip, left->mip_count, left->first_array_slice, left->array_slice_count,
                        left->first_plane, left->plane_count) ==
               std::tie(right.first_mip, right.mip_count, right.first_array_slice, right.array_slice_count,
                        right.first_plane, right.plane_count);
    }
    return true;
}

bool SameDescriptor(const D3D12_RESOURCE_DESC &a, const D3D12_RESOURCE_DESC &b) noexcept
{
    return std::tie(a.Dimension, a.Alignment, a.Width, a.Height, a.DepthOrArraySize, a.MipLevels,
                    a.Format, a.SampleDesc.Count, a.SampleDesc.Quality, a.Layout, a.Flags) ==
           std::tie(b.Dimension, b.Alignment, b.Width, b.Height, b.DepthOrArraySize, b.MipLevels,
                    b.Format, b.SampleDesc.Count, b.SampleDesc.Quality, b.Layout, b.Flags);
}

unsigned ExecutionKind(const Pass &pass) noexcept
{
    return pass.external ? 4u : pass.execute_on_owner ? 3u : pass.owner_execute ? 2u : pass.execute ? 1u : 0u;
}

// Declaration values and compiled logical facts survive Reset. Current native
// owners, callbacks and completion are validated on every binding.
struct TopologyValues
{
    struct Resource
    {
        std::string name;
        ResourceKind kind{};
        bool imported{}, final_set{};
        TextureDesc texture{};
        BufferDesc buffer{};
        D3D12_RESOURCE_DESC descriptor{};
        D3D12_HEAP_TYPE fixed_heap{};
        Access final{};
        ResourceVersion final_version{};
        std::uint32_t first_use{UINT32_MAX}, last_use{};
        bool excludes_shared_reads{};
    };
    struct PassValues
    {
        std::uint32_t id{};
        std::string name;
        QueueHint queue{};
        unsigned execution_kind{};
        std::vector<ResourceUse> uses;
        QueueHint external_queue{};
        std::vector<ExternalResourceAccess> boundaries;
    };
    bool valid{};
    std::uint64_t handle{};
    std::vector<Resource> resources;
    std::vector<PassValues> passes;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ordering;
    struct AliasValues
    {
        std::vector<D3D12_RESOURCE_DESC> descriptors;
        bool aliased{};
    };
    std::vector<AliasValues> alias_values;

    bool Matches(std::span<const LogicalResource> current_resources, std::span<const Pass> current_passes,
                 const std::vector<std::pair<std::uint32_t, std::uint32_t>> &current_ordering,
                 bool restored_declarations = false) const
    {
        if (!valid || resources.size() != current_resources.size() || passes.size() != current_passes.size() ||
            ordering != current_ordering) return false;
        for (std::size_t i = 0; i < resources.size(); ++i)
        {
            const auto &a = resources[i]; const auto &b = current_resources[i];
            if (a.kind != b.kind || a.imported != b.imported || a.final_set != b.final_set ||
                a.fixed_heap != b.fixed_buffer_heap || a.final != (b.final_set ? b.final : Access::Undefined) ||
                !SameVersion(a.final_version, b.final_version) ||
                !SameDescriptor(a.descriptor, b.imported ? b.native_description : NativeDesc(b.kind, b.texture, b.buffer)))
                return false;
        }
        for (std::size_t i = 0; i < passes.size(); ++i)
        {
            const auto &a = passes[i]; const auto &b = current_passes[i];
            if (a.id != b.id || a.queue != b.queue || a.execution_kind != ExecutionKind(b) ||
                (!restored_declarations &&
                 !std::equal(a.uses.begin(), a.uses.end(), b.Uses().begin(), b.Uses().end(), SameUse))) return false;
            if (!b.external) continue;
            if (a.external_queue != b.external->binding.queue_type ||
                !std::equal(a.boundaries.begin(), a.boundaries.end(), b.external->accesses.begin(), b.external->accesses.end(),
                    [](const auto &left, const auto &right) {
                        return left.resource.value == right.resource.value && left.resource.kind == right.resource.kind &&
                            left.incoming == right.incoming && left.outgoing == right.outgoing;
                    })) return false;
        }
        return true;
    }

    std::string FirstMismatch(std::span<const LogicalResource> current_resources,
                              std::span<const Pass> current_passes,
                              const std::vector<std::pair<std::uint32_t, std::uint32_t>> &current_ordering,
                              bool restored_declarations = false) const
    {
        if (!valid) return "cached topology is invalid";
        if (resources.size() != current_resources.size()) return "resource count";
        if (passes.size() != current_passes.size()) return "pass count";
        if (ordering != current_ordering) return "ordering dependencies";
        for (std::size_t i = 0; i < resources.size(); ++i)
        {
            const auto &a = resources[i]; const auto &b = current_resources[i];
            const auto name = b.Name();
            if (a.kind != b.kind) return "resource[" + std::to_string(i) + "] " + std::string(name) + " kind";
            if (a.imported != b.imported) return "resource[" + std::to_string(i) + "] " + std::string(name) + " import mode";
            if (a.final_set != b.final_set || a.final != (b.final_set ? b.final : Access::Undefined))
                return "resource[" + std::to_string(i) + "] " + std::string(name) + " final access";
            if (a.fixed_heap != b.fixed_buffer_heap)
                return "resource[" + std::to_string(i) + "] " + std::string(name) + " fixed heap";
            if (!SameVersion(a.final_version, b.final_version))
                return "resource[" + std::to_string(i) + "] " + std::string(name) + " final version";
            if (!SameDescriptor(a.descriptor, b.imported ? b.native_description : NativeDesc(b.kind, b.texture, b.buffer)))
                return "resource[" + std::to_string(i) + "] " + std::string(name) + " descriptor";
        }
        for (std::size_t i = 0; i < passes.size(); ++i)
        {
            const auto &a = passes[i]; const auto &b = current_passes[i];
            const auto name = b.Name();
            if (a.id != b.id) return "pass[" + std::to_string(i) + "] " + std::string(name) + " id";
            if (a.queue != b.queue) return "pass[" + std::to_string(i) + "] " + std::string(name) + " queue";
            if (a.execution_kind != ExecutionKind(b)) return "pass[" + std::to_string(i) + "] " + std::string(name) + " execution binding";
            if (!restored_declarations && !std::equal(a.uses.begin(), a.uses.end(), b.Uses().begin(), b.Uses().end(), SameUse))
                return "pass[" + std::to_string(i) + "] " + std::string(name) + " resource uses";
            if (b.external)
            {
                if (a.external_queue != b.external->binding.queue_type)
                    return "pass[" + std::to_string(i) + "] " + std::string(name) + " external queue";
                if (!std::equal(a.boundaries.begin(), a.boundaries.end(), b.external->accesses.begin(),
                        b.external->accesses.end(), [](const auto &left, const auto &right) {
                            return left.resource.value == right.resource.value && left.resource.kind == right.resource.kind &&
                                left.incoming == right.incoming && left.outgoing == right.outgoing;
                        }))
                    return "pass[" + std::to_string(i) + "] " + std::string(name) + " external boundaries";
            }
        }
        return "external binding or unknown topology value";
    }

    void Capture(std::span<const LogicalResource> current_resources, std::span<const Pass> current_passes,
                 const std::vector<std::pair<std::uint32_t, std::uint32_t>> &current_ordering,
                 const render_graph::CompiledSchedule &schedule, const render_graph::AliasPlan &aliases)
    {
        resources.clear(); resources.reserve(current_resources.size());
        passes.clear(); passes.reserve(current_passes.size());
        for (const auto &r : current_resources)
            resources.push_back({r.Name(), r.kind, r.imported, r.final_set, r.texture, r.buffer,
                r.imported ? r.native_description : NativeDesc(r.kind, r.texture, r.buffer),
                r.fixed_buffer_heap, r.final_set ? r.final : Access::Undefined, r.final_version});
        for (const auto &p : current_passes)
        {
            auto &stored = passes.emplace_back();
            stored.id = p.id; stored.name = p.Name(); stored.queue = p.queue;
            stored.execution_kind = ExecutionKind(p); stored.uses = p.Uses();
            if (p.external) { stored.external_queue = p.external->binding.queue_type; stored.boundaries = p.external->accesses; }
            // Owner/provider callbacks can bridge read-only resources through COMMON.
            if (p.external || p.execute_on_owner)
                for (const auto &use : stored.uses)
                    resources[use.predecessor.resource.value - 1].excludes_shared_reads = true;
        }
        ordering = current_ordering;
        for (std::uint32_t position = 0; position < schedule.execution_order.size(); ++position)
            for (const auto &use : current_passes[schedule.execution_order[position]].Uses())
            {
                auto &resource = resources[use.predecessor.resource.value - 1];
                if (resource.first_use == UINT32_MAX) resource.first_use = position;
                resource.last_use = position;
            }
        alias_values.clear();
        alias_values.reserve(aliases.groups.size());
        for (const auto &group : aliases.groups)
        {
            auto &values = alias_values.emplace_back();
            values.descriptors.reserve(group.resources.size());
            for (const auto resource : group.resources)
            {
                values.descriptors.push_back(resources[resource].descriptor);
                values.aliased |= aliases.resource_groups[resource] != render_graph::AliasPlan::kExcluded;
            }
        }
        valid = true;
    }
};

[[nodiscard]] bool SameDevice(ID3D12DeviceChild *child, ID3D12Device *device)
{
    Microsoft::WRL::ComPtr<IUnknown> owner_identity, device_identity;
    // Only IID_IUnknown has a canonical COM identity; device interface addresses may differ.
    return child && device && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&owner_identity))) && owner_identity &&
        SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device_identity))) && device_identity &&
        owner_identity.Get() == device_identity.Get();
}

[[nodiscard]] bool ValidExternalCompletion(const ExternalQueueCompletion &completion,
                                           ID3D12Device *device)
{
    if (!completion.native_fence_owner && !completion.value) return true;
    return completion.native_fence_owner && completion.value && completion.value != UINT64_MAX &&
        SameDevice(static_cast<ID3D12Fence *>(completion.native_fence_owner.get()), device);
}

[[nodiscard]] const ExternalResourceAccess *ExternalAccess(const Pass &pass, ResourceHandle resource)
{
    if (!pass.external) return nullptr;
    const auto found = std::find_if(pass.external->accesses.begin(), pass.external->accesses.end(),
        [&](const auto &access) { return access.resource.value == resource.value && access.resource.kind == resource.kind; });
    return found == pass.external->accesses.end() ? nullptr : &*found;
}

[[nodiscard]] D3D12_RESOURCE_STATES LegacyState(ResourceKind kind, Access access) noexcept
{
    switch (access)
    {
    case Access::Common:
        return D3D12_RESOURCE_STATE_COMMON;
    case Access::ShaderRead:
        return kind == ResourceKind::Texture ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                             : (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    case Access::ComputeRead:
        return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case Access::AllShaderRead:
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case Access::RenderTarget:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case Access::ShadingRateSource:
        return D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
    case Access::DepthRead:
        return D3D12_RESOURCE_STATE_DEPTH_READ;
    case Access::DepthShaderRead:
        return D3D12_RESOURCE_STATE_DEPTH_READ |
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case Access::DepthWrite:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case Access::CopySource:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case Access::CopyDest:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case Access::VertexBuffer:
        return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    case Access::IndexBuffer:
        return D3D12_RESOURCE_STATE_INDEX_BUFFER;
    case Access::VertexShaderRead:
        return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
               D3D12_RESOURCE_STATE_INDEX_BUFFER |
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
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
    case Access::AllShaderRead:
        return D3D12_BARRIER_SYNC_ALL_SHADING;
    case Access::ComputeRead:
        return D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    case Access::RenderTarget:
        return D3D12_BARRIER_SYNC_RENDER_TARGET;
    case Access::ShadingRateSource:
        return D3D12_BARRIER_SYNC_PIXEL_SHADING;
    case Access::DepthRead:
    case Access::DepthWrite:
        return D3D12_BARRIER_SYNC_DEPTH_STENCIL;
    case Access::DepthShaderRead:
        return D3D12_BARRIER_SYNC_DEPTH_STENCIL | D3D12_BARRIER_SYNC_ALL_SHADING;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        // UAV access includes API clears, whose scope is outside ALL_SHADING.
        return D3D12_BARRIER_SYNC_ALL_SHADING | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
    case Access::CopySource:
    case Access::CopyDest:
        return D3D12_BARRIER_SYNC_COPY;
    case Access::VertexBuffer:
        return D3D12_BARRIER_SYNC_VERTEX_SHADING;
    case Access::IndexBuffer:
        return D3D12_BARRIER_SYNC_INDEX_INPUT;
    case Access::VertexShaderRead:
        return D3D12_BARRIER_SYNC_VERTEX_SHADING | D3D12_BARRIER_SYNC_COMPUTE_SHADING |
               D3D12_BARRIER_SYNC_INDEX_INPUT;
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
    case Access::AllShaderRead:
        return D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    case Access::RenderTarget:
        return D3D12_BARRIER_ACCESS_RENDER_TARGET;
    case Access::ShadingRateSource:
        return D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE;
    case Access::DepthRead:
        return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
    case Access::DepthShaderRead:
        return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ |
               D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    case Access::DepthWrite:
        return D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        return D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    case Access::CopySource:
        return D3D12_BARRIER_ACCESS_COPY_SOURCE;
    case Access::CopyDest:
        return D3D12_BARRIER_ACCESS_COPY_DEST;
    case Access::VertexBuffer:
        return D3D12_BARRIER_ACCESS_VERTEX_BUFFER;
    case Access::IndexBuffer:
        return D3D12_BARRIER_ACCESS_INDEX_BUFFER;
    case Access::VertexShaderRead:
        return D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_INDEX_BUFFER |
               D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
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
    case Access::AllShaderRead:
        return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    case Access::RenderTarget:
        return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    case Access::ShadingRateSource:
        return D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE;
    case Access::DepthRead:
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
    case Access::DepthShaderRead:
        // Depth-only layout cannot be sampled. This Direct-only read layout
        // permits both DEPTH_STENCIL_READ and SHADER_RESOURCE access.
        return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ;
    case Access::DepthWrite:
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    case Access::UnorderedRead:
    case Access::UnorderedWrite:
        return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    case Access::CopySource:
        return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    case Access::CopyDest:
        return D3D12_BARRIER_LAYOUT_COPY_DEST;
    case Access::VertexBuffer:
    case Access::IndexBuffer:
    case Access::VertexShaderRead:
        return D3D12_BARRIER_LAYOUT_COMMON;
    case Access::Present:
        return D3D12_BARRIER_LAYOUT_PRESENT;
    default:
        return D3D12_BARRIER_LAYOUT_COMMON;
    }
}

[[nodiscard]] bool IsUavAccess(Access access) noexcept
{
    return access == Access::UnorderedRead || access == Access::UnorderedWrite;
}

[[nodiscard]] bool NeedsMemoryBarrier(Access before, Access after) noexcept
{
    return (IsUavAccess(before) && IsUavAccess(after)) &&
        (IsMemoryWrite(before) || IsMemoryWrite(after));
}

[[nodiscard]] D3D12_BARRIER_SYNC QueueBarrierSync(Access access, QueueHint queue) noexcept
{
    if (queue == QueueHint::Compute &&
        (access == Access::UnorderedRead || access == Access::UnorderedWrite))
        return D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
    return BarrierSync(access);
}

struct BufferBarrierBatch
{
    ID3D12GraphicsCommandList7 *commands{};
    std::array<D3D12_BUFFER_BARRIER, 64> barriers;
    UINT count{};

    void Flush()
    {
        if (!count) return;
        D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, count};
        group.pBufferBarriers = barriers.data();
        commands->Barrier(1, &group);
        count = 0;
    }
    void Append(const D3D12_BUFFER_BARRIER &barrier)
    {
        if (count == barriers.size()) Flush();
        barriers[count++] = barrier;
    }
};

void EmitBufferBarrier(ID3D12GraphicsCommandList7 *commands,
                       const D3D12_BUFFER_BARRIER &barrier, BufferBarrierBatch *batch)
{
    if (batch) { batch->Append(barrier); return; }
    D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
    group.pBufferBarriers = &barrier;
    commands->Barrier(1, &group);
}

void EmitUavBarrier(ID3D12GraphicsCommandList *commands, ID3D12GraphicsCommandList7 *enhanced,
                    BarrierMode mode, const LogicalResource &resource,
                    QueueHint queue = QueueHint::Direct,
                    Access before = Access::UnorderedWrite, Access after = Access::UnorderedWrite,
                    BufferBarrierBatch *batch = nullptr)
{
    if (!resource.native)
    {
        return;
    }
    if (mode == BarrierMode::Enhanced)
    {
        const auto sync_before = QueueBarrierSync(before, queue);
        const auto sync_after = QueueBarrierSync(after, queue);
        const auto access_before = BarrierAccess(before);
        const auto access_after = BarrierAccess(after);
        if (resource.kind == ResourceKind::Texture)
        {
            D3D12_GLOBAL_BARRIER barrier{};
            barrier.SyncBefore = sync_before;
            barrier.SyncAfter = sync_after;
            barrier.AccessBefore = access_before;
            barrier.AccessAfter = access_after;
            D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_GLOBAL, 1};
            group.pGlobalBarriers = &barrier;
            enhanced->Barrier(1, &group);
            return;
        }
        D3D12_BUFFER_BARRIER barrier{};
        barrier.SyncBefore = sync_before;
        barrier.SyncAfter = sync_after;
        barrier.AccessBefore = access_before;
        barrier.AccessAfter = access_after;
        barrier.pResource = static_cast<ID3D12Resource *>(resource.native);
        barrier.Offset = 0;
        barrier.Size = UINT64_MAX;
        EmitBufferBarrier(enhanced, barrier, batch);
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = static_cast<ID3D12Resource *>(resource.native);
    commands->ResourceBarrier(1, &barrier);
}

void Transition(ID3D12GraphicsCommandList *commands, ID3D12GraphicsCommandList7 *enhanced,
                BarrierMode mode, const LogicalResource &resource, Access before, Access after,
                QueueHint queue = QueueHint::Direct, bool release = false, bool acquire = false,
                BufferBarrierBatch *batch = nullptr)
{
    if (!resource.native || FixedBufferHeap(resource) != D3D12_HEAP_TYPE_DEFAULT)
    {
        return;
    }
    if (before == after) return;
    // UnorderedRead and UnorderedWrite share the same D3D12 state/layout. Their
    // ordering is expressed with a UAV barrier below, never as a state transition.
    if (IsUavAccess(before) && IsUavAccess(after))
    {
        return;
    }
    if (mode == BarrierMode::Legacy && LegacyState(resource.kind, before) ==
            LegacyState(resource.kind, after))
    {
        return;
    }

    auto *native = static_cast<ID3D12Resource *>(resource.native);
    if (mode == BarrierMode::Enhanced)
    {
        if (resource.kind == ResourceKind::Texture)
        {
            const auto texture_layout = [&](Access access) {
                // Enhanced texture copies recorded on a COPY command list use
                // COMMON layout; COPY_SOURCE/COPY_DEST are access scopes, not
                // legal copy-queue texture layouts. Direct/Compute queues keep
                // the ordinary explicit layouts.
                if (queue == QueueHint::Copy &&
                    (access == Access::Common || access == Access::CopySource ||
                     access == Access::CopyDest))
                    return D3D12_BARRIER_LAYOUT_COMMON;
                return BarrierLayout(access);
            };
            D3D12_TEXTURE_BARRIER barrier{};
            barrier.SyncBefore = acquire ? D3D12_BARRIER_SYNC_NONE : QueueBarrierSync(before, queue);
            barrier.SyncAfter = release ? D3D12_BARRIER_SYNC_NONE : QueueBarrierSync(after, queue);
            barrier.AccessBefore = acquire ? D3D12_BARRIER_ACCESS_NO_ACCESS : BarrierAccess(before);
            barrier.AccessAfter = release ? D3D12_BARRIER_ACCESS_NO_ACCESS : BarrierAccess(after);
            barrier.LayoutBefore = texture_layout(before);
            barrier.LayoutAfter = texture_layout(after);
            barrier.pResource = native;
            // Conservative whole-resource state: the sentinel covers every
            // mip, array slice and format plane, including 3D textures.
            barrier.Subresources = {D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, 0, 0, 0, 0, 0};
            D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1};
            group.pTextureBarriers = &barrier;
            enhanced->Barrier(1, &group);
        }
        else
        {
            D3D12_BUFFER_BARRIER barrier{};
            barrier.SyncBefore = acquire ? D3D12_BARRIER_SYNC_NONE : QueueBarrierSync(before, queue);
            barrier.SyncAfter = release ? D3D12_BARRIER_SYNC_NONE : QueueBarrierSync(after, queue);
            barrier.AccessBefore = acquire ? D3D12_BARRIER_ACCESS_NO_ACCESS : BarrierAccess(before);
            barrier.AccessAfter = release ? D3D12_BARRIER_ACCESS_NO_ACCESS : BarrierAccess(after);
            barrier.pResource = native;
            barrier.Offset = 0;
            barrier.Size = UINT64_MAX;
            EmitBufferBarrier(enhanced, barrier, batch);
        }
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {native, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          LegacyState(resource.kind, before), LegacyState(resource.kind, after)};
    commands->ResourceBarrier(1, &barrier);
}

void EmitAliasBarrier(ID3D12GraphicsCommandList *commands, ID3D12GraphicsCommandList7 *enhanced,
                      BarrierMode mode, const LogicalResource &resource, Access previous, Access first,
                      QueueHint queue)
{
    if (!resource.native) return;
    auto *native = static_cast<ID3D12Resource *>(resource.native);
    if (mode == BarrierMode::Enhanced)
    {
        D3D12_GLOBAL_BARRIER global{};
        global.SyncBefore = global.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        global.AccessBefore = global.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
        D3D12_BARRIER_GROUP global_group{D3D12_BARRIER_TYPE_GLOBAL, 1};
        global_group.pGlobalBarriers = &global;
        enhanced->Barrier(1, &global_group);
        if (resource.kind == ResourceKind::Texture)
        {
            const bool copy_activation = queue == QueueHint::Copy;
            D3D12_TEXTURE_BARRIER barrier{};
            barrier.SyncBefore = copy_activation
                ? (first == Access::Common ? D3D12_BARRIER_SYNC_ALL : D3D12_BARRIER_SYNC_COPY)
                : D3D12_BARRIER_SYNC_NONE;
            barrier.SyncAfter = QueueBarrierSync(first, queue);
            barrier.AccessBefore = copy_activation ? D3D12_BARRIER_ACCESS_COMMON
                                                    : D3D12_BARRIER_ACCESS_NO_ACCESS;
            barrier.AccessAfter = copy_activation ? D3D12_BARRIER_ACCESS_COMMON
                                                   : BarrierAccess(first);
            barrier.LayoutBefore = copy_activation ? D3D12_BARRIER_LAYOUT_COMMON
                                                    : D3D12_BARRIER_LAYOUT_UNDEFINED;
            barrier.LayoutAfter = copy_activation ? D3D12_BARRIER_LAYOUT_COMMON
                                                  : BarrierLayout(first);
            barrier.pResource = native;
            barrier.Subresources = {D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, 0, 0, 0, 0, 0};
            barrier.Flags = copy_activation ? D3D12_TEXTURE_BARRIER_FLAG_NONE
                                            : D3D12_TEXTURE_BARRIER_FLAG_DISCARD;
            D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_TEXTURE, 1};
            group.pTextureBarriers = &barrier;
            enhanced->Barrier(1, &group);
        }
        else
        {
            const bool copy_activation = queue == QueueHint::Copy;
            D3D12_BUFFER_BARRIER barrier{};
            barrier.SyncBefore = copy_activation
                ? (first == Access::Common ? D3D12_BARRIER_SYNC_ALL : D3D12_BARRIER_SYNC_COPY)
                : D3D12_BARRIER_SYNC_NONE;
            barrier.SyncAfter = QueueBarrierSync(first, queue);
            barrier.AccessBefore = copy_activation ? D3D12_BARRIER_ACCESS_COMMON
                                                    : D3D12_BARRIER_ACCESS_NO_ACCESS;
            barrier.AccessAfter = copy_activation ? D3D12_BARRIER_ACCESS_COMMON
                                                   : BarrierAccess(first);
            barrier.pResource = native;
            barrier.Offset = 0;
            barrier.Size = UINT64_MAX;
            D3D12_BARRIER_GROUP group{D3D12_BARRIER_TYPE_BUFFER, 1};
            group.pBufferBarriers = &barrier;
            enhanced->Barrier(1, &group);
        }
        return;
    }
    D3D12_RESOURCE_BARRIER alias{};
    alias.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    alias.Aliasing.pResourceBefore = nullptr;
    alias.Aliasing.pResourceAfter = native;
    commands->ResourceBarrier(1, &alias);
    if (!(mode == BarrierMode::Legacy && queue == QueueHint::Copy))
        Transition(commands, enhanced, mode, resource,
                   previous == Access::Undefined ? Access::Common : previous, first, queue);
    if (resource.kind == ResourceKind::Texture &&
        (first == Access::RenderTarget || first == Access::DepthRead ||
         first == Access::DepthWrite || first == Access::DepthShaderRead))
        commands->DiscardResource(native, nullptr);
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
        QueueHint queue{QueueHint::Direct};
        bool claimed{};
        std::uint32_t active_members{};
        std::uint64_t size{};
        std::uint64_t alignment{};
        D3D12_HEAP_FLAGS heap_flags{D3D12_HEAP_FLAG_NONE};
        CompletionSet completion;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> active_views;
        std::vector<D3D12_RESOURCE_DESC> active_view_descs;
        std::vector<Access> active_view_states;
        std::vector<QueueHint> active_view_queues;
    };
    D3D12MA::Allocator *allocator{};
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IUnknown> device_identity;
    std::vector<std::pair<DXGI_FORMAT, std::uint32_t>> format_planes;
    std::vector<Entry> entries;
};
TransientResourcePool::TransientResourcePool() : impl_(std::make_unique<Impl>()) {}
TransientResourcePool::~TransientResourcePool() = default;
std::uint64_t TransientResourcePool::AllocatedBytes() const noexcept
{
    std::uint64_t bytes{};
    for (const auto &entry : impl_->entries)
        if (entry.allocation.allocation) bytes += entry.allocation.allocation->GetSize();
    return bytes;
}
Result TransientResourcePool::Initialize(D3D12MA::Allocator *allocator, ID3D12Device *device)
{
    if (!allocator || !device)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Transient pool requires an allocator and its D3D12 device.");
    Clear(); impl_->allocator = allocator; impl_->device = device;
    impl_->device_identity.Reset();
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&impl_->device_identity))))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Transient pool cannot identify its D3D12 device.");
    impl_->format_planes.clear();
    return Result::Success();
}
Result TransientResourcePool::ReserveTexture(const TextureDesc &description)
{
    if (!impl_->allocator || !description.width || !description.height || !description.format)
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "Transient texture reservation requires a valid pool and descriptor.");
    const auto native = NativeDesc(ResourceKind::Texture, description, {});
    const auto info = impl_->device->GetResourceAllocationInfo(0, 1, &native);
    if (!info.SizeInBytes || !info.Alignment)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Transient texture reservation returned invalid allocation information.");
    D3D12MA::ALLOCATION_DESC allocation{};
    allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
    allocation.ExtraHeapFlags = HeapFlags(ResourceKind::Texture, description.flags);
    Impl::Entry entry;
    entry.kind = ResourceKind::Texture;
    entry.texture = description;
    entry.size = info.SizeInBytes;
    entry.alignment = info.Alignment;
    entry.heap_flags = allocation.ExtraHeapFlags;
    const auto hr = impl_->allocator->AllocateMemory(&allocation, &info, &entry.allocation.allocation);
    if (FAILED(hr))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "Transient texture reservation failed (HRESULT " +
                                   std::to_string(hr) + ").");
    impl_->entries.push_back(std::move(entry));
    return Result::Success();
}
void TransientResourcePool::ResetClaims() noexcept
{
    for (auto &entry : impl_->entries)
    {
        if (entry.completion.direct || entry.completion.compute || entry.completion.copy) continue;
        entry.claimed = false;
        entry.active_members = 0;
    }
}
void TransientResourcePool::SetClaimCompletion(const CompletionSet &completion) noexcept
{
    for (auto &entry : impl_->entries)
        if (entry.claimed) entry.completion.Merge(completion);
}
void TransientResourcePool::ResetCompletedClaims(const CompletionSet &completed) noexcept
{
    for (auto &entry : impl_->entries)
    {
        if (!entry.claimed || !entry.completion.IsSatisfiedBy(completed)) continue;
        entry.claimed = false;
        entry.active_members = 0;
        entry.completion = {};
    }
}
void TransientResourcePool::Clear() noexcept { impl_->entries.clear(); }
void TransientResourcePool::Swap(TransientResourcePool &other) noexcept
{
    impl_.swap(other.impl_);
}

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

    Preparation state{Preparation::Building};
    TransientResourcePool::Impl *pool{};
    std::vector<LogicalResource> resources;
    std::vector<Pass> passes;
    std::vector<std::vector<ResourceUse>> reusable_pass_uses;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> ordering_dependencies;
    std::vector<std::uintptr_t> imported_identities;
    struct PlacedImport { void *heap; std::uint64_t begin, end; };
    std::vector<PlacedImport> placed_imports;
    render_graph::CompiledSchedule schedule;
    render_graph::AliasPlan alias_plan;
    TopologyValues topology;
    Microsoft::WRL::ComPtr<ID3D12Device> plan_device;
    struct PreviousPlan
    {
        TopologyValues topology;
        render_graph::CompiledSchedule schedule;
        render_graph::AliasPlan aliases;
    };
    std::array<PreviousPlan, 7> previous_plans;
    std::size_t next_previous_plan{};
    std::uint64_t plan_input_builds{};
    std::uint64_t plan_compilations{};
    std::uint64_t plan_reuses{};
    std::uint64_t next_plan_handle{1};
    bool restored_plan{};
    bool binding_failed{};
    bool invalid_final{};
};

struct RenderGraphQueueExecutor::Impl
{
    struct PassRecorder
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList7> enhanced;
    };

    struct QueueState
    {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::uint64_t next_fence{1};
        std::uint64_t timestamp_frequency{};
        CompletionSet joined;
    };

    struct FrameSlot
    {
        CompletionSet completion;
        std::array<std::vector<PassRecorder>, 3> recorders;
    };

    struct ExternalRetention
    {
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Fence> retirement_fence;
        std::uint64_t next_value{1};
        ExternalQueueCompletion provider;
        std::shared_ptr<ExternalOperation> operation;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
        CompletionSet bridge;
        bool bridged{};
    };

    enum class BarrierOperation { Transition, Uav, Release, Acquire, Alias };
    struct PlannedBarrier
    {
        std::uint32_t resource{};
        Access before{}, after{};
        BarrierOperation operation{};
    };
    struct Batch
    {
        QueueHint queue{};
        std::uint32_t pass{UINT32_MAX};
        std::uint32_t position{};
        std::vector<PlannedBarrier> before, after;
        std::vector<std::uint32_t> dependencies, resources;
        CompletionSet imported;
        std::vector<std::uint32_t> external_imports;
        std::size_t recorder{};
        bool resolve_timestamps{};
        std::uint32_t external_finish{UINT32_MAX};

        void Reset()
        {
            queue = {}; pass = external_finish = UINT32_MAX; position = 0;
            before.clear(); after.clear(); dependencies.clear();
            resources.clear();
            imported = {}; external_imports.clear(); recorder = 0; resolve_timestamps = false;
        }
    };
    struct ResourceState
    {
        Access logical{}, physical{};
        QueueHint queue{};
        std::uint32_t batch{UINT32_MAX};
        std::uint32_t read_entry{UINT32_MAX};
        std::array<std::uint32_t, 3> readers{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    };

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Device4> device4;
    bool copy_timestamp_queries_supported{};
    std::array<QueueState, 3> queues;
    std::vector<FrameSlot> frames;
    HANDLE completion_event{};
    bool reliable{true};
    bool fault_drained{};
    CompletionSet submitted{};
    GraphExecutionStats stats{};
    std::vector<ExternalRetention> external_retained;
    // Only CPU values survive Execute; recording tasks join before these are reused.
    std::vector<Batch> batches;
    // Indexed by batch slot so scratch for synthesized join/export batches
    // survives when the active Batch vector shrinks back to pass count.
    std::vector<std::vector<std::uint64_t>> batch_resource_memberships;
    std::vector<ResourceState> resource_states;
    std::vector<std::uint32_t> submission_order;
    std::vector<std::size_t> counted_blocks, ordering_predecessor;
    std::vector<bool> shareable, waits_before, signal_after;
    std::vector<Result> record_results;
    std::vector<std::uint64_t> signals;
    std::vector<std::pair<std::uint32_t, std::size_t>> pass_positions;
    std::vector<ID3D12CommandList *> lists;

    void ReleaseCompletedExternal(const CompletionSet &completed)
    {
        std::erase_if(external_retained, [&](const auto &retained) {
            return retained.bridged && retained.bridge.IsSatisfiedBy(completed);
        });
    }
};

RenderGraphQueueExecutor::RenderGraphQueueExecutor() : impl_(std::make_unique<Impl>()) {}

RenderGraphQueueExecutor::~RenderGraphQueueExecutor()
{
    Clear();
    // A live device with undrainable submissions cannot safely release native
    // pointers. Preserve that exceptional quarantine through process teardown.
    if (impl_ && impl_->device) static_cast<void>(impl_.release());
}

Result RenderGraphQueueExecutor::Initialize(ID3D12Device *device,
                                            ID3D12CommandQueue *direct_queue,
                                            std::uint32_t frame_slot_count,
                                            bool copy_timestamp_queries_supported)
{
    Clear();
    if (impl_->device)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "RenderGraph previous submissions could not be drained.");
    if (!device || !frame_slot_count)
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "RenderGraph queue executor requires a device and frame slots.");
    }

    impl_->device = device;
    impl_->copy_timestamp_queries_supported = copy_timestamp_queries_supported;
    // Device4 creates command lists closed, which matches the recorder's
    // first operation (allocator/list Reset) and avoids the legacy open/Close
    // bootstrap. Keep the older path for devices without this interface.
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&impl_->device4))))
        impl_->device4.Reset();
    for (const auto queue_hint : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
    {
        auto &state = impl_->queues[QueueIndex(queue_hint)];
        if (queue_hint == QueueHint::Direct && direct_queue)
        {
            if (direct_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            {
                Clear();
                return Result::Failure(
                    ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                    "RenderGraph queue executor requires a Direct queue for its Direct timeline.");
            }
            state.queue = direct_queue;
        }
        else
        {
            D3D12_COMMAND_QUEUE_DESC description{};
            description.Type = NativeQueueType(queue_hint);
            const auto result =
                device->CreateCommandQueue(&description, IID_PPV_ARGS(&state.queue));
            if (FAILED(result))
            {
                Clear();
                return QueueFailure(
                    std::string("Create ") + QueueName(queue_hint) + " command queue", result);
            }
        }

        const auto fence_result =
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence));
        if (FAILED(fence_result))
        {
            Clear();
            return QueueFailure(std::string("Create ") + QueueName(queue_hint) + " fence",
                                fence_result);
        }
        state.next_fence = 1;
        state.timestamp_frequency = 0;
        state.joined = {};
        if (queue_hint != QueueHint::Copy || impl_->copy_timestamp_queries_supported)
            static_cast<void>(state.queue->GetTimestampFrequency(&state.timestamp_frequency));
    }

    impl_->completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->completion_event)
    {
        Clear();
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "RenderGraph queue executor could not create a fence event.");
    }
    impl_->frames.resize(frame_slot_count);
    return Result::Success();
}

CompletionSet RenderGraphQueueExecutor::Completed() const noexcept
{
    CompletionSet completed;
    if (!impl_) return completed;
    if (!impl_->reliable)
    {
        // UINT64_MAX means unproven work on a terminally faulted timeline.
        // A successful full drain proves all of it retired, but the timeline
        // stays faulted: it cannot submit new work against these completions.
        if (impl_->fault_drained)
            for (const auto queue : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
                completed.Include(queue, UINT64_MAX);
        return completed;
    }
    for (const auto queue_hint : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
    {
        const auto &state = impl_->queues[QueueIndex(queue_hint)];
        if (state.fence)
        {
            const auto value = state.fence->GetCompletedValue();
            if (value != UINT64_MAX) completed.Include(queue_hint, value);
        }
    }
    return completed;
}

Result RenderGraphQueueExecutor::Wait(const CompletionSet &completion)
{
    if (!impl_ || !impl_->completion_event)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "RenderGraph queue executor is not initialized.");
    }
    for (const auto queue_hint : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
    {
        const auto required = completion.Required(queue_hint);
        if (!required) continue;
        if (required == UINT64_MAX)
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "RenderGraph completion includes an unsignaled submission; drain is required.");
        auto &state = impl_->queues[QueueIndex(queue_hint)];
        if (!state.fence)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "RenderGraph queue executor is missing a queue fence.");
        }
        if (auto waited = WaitForFence(impl_->device.Get(), state.fence.Get(), required,
                impl_->completion_event, std::string("Wait for ") + QueueName(queue_hint) + " fence");
            !waited) return waited;
    }
    impl_->ReleaseCompletedExternal(Completed());
    return Result::Success();
}

Result RenderGraphQueueExecutor::Join(const CompletionSet &completion, QueueHint destination)
{
    if (!impl_->device || !impl_->reliable || QueueIndex(destination) >= impl_->queues.size())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "RenderGraph queue executor has no reliable destination timeline.");
    auto &consumer = impl_->queues[QueueIndex(destination)];
    for (const auto producer : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
    {
        const auto value = completion.Required(producer);
        if (!value || producer == destination) continue;
        const auto &state = impl_->queues[QueueIndex(producer)];
        if (value >= state.next_fence)
            return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                                   "RenderGraph completion does not belong to this executor timeline.");
        if (value <= consumer.joined.Required(producer)) continue;
        const auto result = consumer.queue->Wait(state.fence.Get(), value);
        if (FAILED(result)) return QueueFailure("RenderGraph queue join", result);
        consumer.joined.Include(producer, value);
        ++impl_->stats.cross_queue_waits;
    }
    return Result::Success();
}

Result RenderGraphQueueExecutor::Drain()
{
    if (!impl_->device || !impl_->completion_event)
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "RenderGraph queue executor is not initialized.");
    // Drain provider queues before owned timelines. This also covers a callback
    // that submitted work but returned no usable completion or failed to bridge.
    for (auto &retained : impl_->external_retained)
    {
        if (!retained.queue)
        {
            if (!retained.provider.native_fence_owner) continue;
            auto *fence = static_cast<ID3D12Fence *>(retained.provider.native_fence_owner.get());
            if (auto waited = WaitForFence(impl_->device.Get(), fence, retained.provider.value,
                    impl_->completion_event, "RenderGraph imported provider drain");
                !waited) return waited;
            continue;
        }
        if (retained.retirement_fence->GetCompletedValue() == UINT64_MAX)
            return RemovedFenceFailure(impl_->device.Get(), "RenderGraph provider drain");
        const auto value = retained.next_value++;
        const auto signaled = retained.queue->Signal(retained.retirement_fence.Get(), value);
        if (FAILED(signaled)) return QueueFailure("RenderGraph provider drain Signal", signaled);
        if (auto waited = WaitForFence(impl_->device.Get(), retained.retirement_fence.Get(), value,
                impl_->completion_event, "RenderGraph provider drain");
            !waited) return waited;
    }
    // Fresh signals also cover work on a supplied Direct queue and work whose
    // original Signal failed. Only this complete drain restores safe teardown.
    CompletionSet drained;
    for (const auto queue : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
    {
        auto &state = impl_->queues[QueueIndex(queue)];
        if (state.fence->GetCompletedValue() == UINT64_MAX)
            return RemovedFenceFailure(impl_->device.Get(), "RenderGraph drain");
        const auto value = state.next_fence++;
        const auto result = state.queue->Signal(state.fence.Get(), value);
        if (FAILED(result)) return QueueFailure("RenderGraph drain Signal", result);
        drained.Include(queue, value);
    }
    const auto result = Wait(drained);
    if (result)
    {
        impl_->external_retained.clear();
        impl_->fault_drained = !impl_->reliable;
    }
    return result;
}

std::uint64_t RenderGraphQueueExecutor::TimestampFrequency(QueueHint queue) const noexcept
{
    return QueueIndex(queue) < impl_->queues.size()
        ? impl_->queues[QueueIndex(queue)].timestamp_frequency : 0;
}

GraphExecutionStats RenderGraphQueueExecutor::LastExecutionStats() const noexcept
{
    return impl_->stats;
}

void RenderGraphQueueExecutor::Clear() noexcept
{
    if (!impl_) return;
    if (impl_->completion_event)
    {
        const auto drained = Drain();
        if (!drained && impl_->device && SUCCEEDED(impl_->device->GetDeviceRemovedReason())) return;
    }
    impl_->external_retained.clear();
    impl_->lists.clear();
    impl_->frames.clear();
    for (auto &state : impl_->queues)
    {
        state.queue.Reset();
        state.fence.Reset();
        state.next_fence = 1;
        state.timestamp_frequency = 0;
        state.joined = {};
    }
    impl_->device.Reset();
    impl_->device4.Reset();
    impl_->copy_timestamp_queries_supported = false;
    impl_->reliable = true;
    impl_->fault_drained = false;
    impl_->submitted = {};
    impl_->stats = {};
    if (impl_->completion_event)
    {
        CloseHandle(impl_->completion_event);
        impl_->completion_event = nullptr;
    }
}

Result RenderGraphQueueExecutor::Execute(RenderGraphBuilder &graph, std::uint32_t frame_slot,
                                         BarrierMode barriers, CompletionSet &completion,
                                         const GraphExecutionOptions *options)
{
    completion = {};
    impl_->stats = {};
    auto cpu_stage_start = std::chrono::steady_clock::now();
    const auto measure_stage = [&] {
        const auto end = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - cpu_stage_start).count();
        cpu_stage_start = end;
        return static_cast<std::uint64_t>(elapsed);
    };
    const auto fail = [](std::string message) {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
    };
    if (!impl_->device || !impl_->completion_event || frame_slot >= impl_->frames.size())
        return fail("RenderGraph queued execution requires a valid executor frame slot.");
    if (!impl_->reliable)
        return fail("RenderGraph queue timeline is unreliable after a failed submission Signal.");
    auto &g = *graph.impl_;
    if (g.state != Preparation::Ready || !g.pool)
        return fail("RenderGraph queued execution requires a prepared graph.");
    if (options && options->cpu_pass_timing_count && !options->cpu_pass_timings)
        return fail("CPU pass timing output count requires a non-null buffer.");
    const auto cpu_passes = options
        ? std::span<GpuPassTiming>(options->cpu_pass_timings, options->cpu_pass_timing_count)
        : std::span<GpuPassTiming>{};
    if (options)
    {
        for (std::size_t queue = 0; queue < 3; ++queue)
            if ((options->timestamp_query_heaps[queue] == nullptr) !=
                (options->timestamp_readbacks[queue] == nullptr))
                return fail("Each timestamp queue requires both a query heap and a readback resource.");
        const auto copy = static_cast<std::size_t>(QueueHint::Copy);
        if ((options->timestamp_query_heaps[copy] || options->timestamp_readbacks[copy]) &&
            !impl_->copy_timestamp_queries_supported)
            return fail("Copy queue timestamps were supplied without device support.");
        for (std::size_t first = 0; first < 3; ++first)
            for (std::size_t second = first + 1; second < 3; ++second)
            {
                if (options->timestamp_query_heaps[first] &&
                    options->timestamp_query_heaps[first] == options->timestamp_query_heaps[second])
                    return fail("Each timestamp queue requires a separate query heap.");
                if (options->timestamp_readbacks[first] &&
                    options->timestamp_readbacks[first] == options->timestamp_readbacks[second])
                    return fail("Each timestamp queue requires a separate readback resource.");
            }
    }
    const auto queries_for = [&](QueueHint queue) {
        return options ? static_cast<ID3D12QueryHeap *>(
            options->timestamp_query_heaps[QueueIndex(queue)]) : nullptr;
    };
    const auto readback_for = [&](QueueHint queue) {
        return options ? static_cast<ID3D12Resource *>(
            options->timestamp_readbacks[QueueIndex(queue)]) : nullptr;
    };
    const auto query_slot_for = [&](std::uint32_t position) {
        return options && options->timestamp_query_slots &&
            position < options->timestamp_query_slot_count
            ? options->timestamp_query_slots[position] : position;
    };
    if (!cpu_passes.empty())
    {
        if (cpu_passes.size() != g.schedule.execution_order.size())
            return fail("CPU pass timing output must match compiled execution order.");
        for (std::size_t position = 0; position < cpu_passes.size(); ++position)
        {
            auto &timing = cpu_passes[position];
            const auto &pass = g.passes[g.schedule.execution_order[position]];
            if (timing.pass_id != pass.id || timing.queue != static_cast<RenderQueue>(pass.queue))
                return fail("CPU pass timing identity must match compiled execution order.");
            timing.cpu_record_begin_qpc = timing.cpu_record_end_qpc = 0;
            timing.cpu_callback_begin_qpc = timing.cpu_callback_end_qpc = 0;
            timing.cpu_provider_submit_begin_qpc = timing.cpu_provider_submit_end_qpc = 0;
            timing.cpu_record_valid = timing.cpu_callback_valid = timing.cpu_provider_submit_valid = false;
        }
    }
    const auto cpu_counter = []() -> std::uint64_t {
        LARGE_INTEGER counter{};
        return QueryPerformanceCounter(&counter) && counter.QuadPart > 0
            ? static_cast<std::uint64_t>(counter.QuadPart) : 0;
    };
    impl_->stats.transient_resource_count = 0;
    impl_->stats.transient_backing_bytes = 0;
    auto &counted_blocks = impl_->counted_blocks;
    counted_blocks.clear();
    for (const auto &group : g.alias_plan.groups)
    {
        impl_->stats.transient_resource_count += static_cast<std::uint32_t>(group.resources.size());
        if (group.resources.empty()) continue;
        const auto pool_index = g.resources[group.resources.front()].pool_index;
        if (pool_index == SIZE_MAX ||
            std::find(counted_blocks.begin(), counted_blocks.end(), pool_index) != counted_blocks.end())
            continue;
        counted_blocks.push_back(pool_index);
        if (pool_index < g.pool->entries.size())
            impl_->stats.transient_backing_bytes += g.pool->entries[pool_index].size;
    }
    impl_->stats.transient_block_count = static_cast<std::uint32_t>(counted_blocks.size());
    impl_->stats.alias_activation_count = g.alias_plan.alias_activation_count;
    impl_->stats.transient_bytes_without_aliasing = g.alias_plan.transient_bytes_without_aliasing;
    const auto *tasks = options ? options->task_executor : nullptr;
    if (tasks && (!tasks->submit || !tasks->join))
        return fail("RenderGraph recording executor requires both submit and join callbacks.");
    // Copy queues can own both buffer and texture COPY_SOURCE/COPY_DEST work.
    // Cross-queue texture ownership follows the same Common release/acquire
    // protocol as buffers; Legacy copy lists rely on Common promotion/decay,
    // while Enhanced mode records COPY layout texture barriers explicitly.
    for (const auto &resource : g.resources)
        for (const auto queue : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
            if (resource.imported_completion.Required(queue) >= impl_->queues[QueueIndex(queue)].next_fence)
                return fail("RenderGraph imported completion does not belong to this executor timeline.");
    auto &slot = impl_->frames[frame_slot];
    if (auto result = Wait(slot.completion); !result) return result;
    slot.completion = {};
    impl_->ReleaseCompletedExternal(Completed());

    using BarrierOperation = Impl::BarrierOperation;
    using PlannedBarrier = Impl::PlannedBarrier;
    using Batch = Impl::Batch;
    using ResourceState = Impl::ResourceState;
    // Legacy ownership, promotion and decay retain the single-queue path.
    const bool shared_readers = barriers == BarrierMode::Enhanced;
    auto &shareable = impl_->shareable;
    if (shared_readers)
    {
        shareable.assign(g.resources.size(), false);
        for (std::size_t index = 0; index < g.resources.size(); ++index)
        {
            const auto &resource = g.resources[index];
            shareable[index] = resource.imported && resource.kind == ResourceKind::Texture &&
                !resource.alias_activation && !resource.imported_external_completion.native_fence_owner &&
                !resource.imported_external_completion.resource_lease &&
                !g.topology.resources[index].excludes_shared_reads;
        }
    }
    auto &batches = impl_->batches;
    auto &batch_resource_memberships = impl_->batch_resource_memberships;
    for (std::size_t index = 0; index < batches.size(); ++index)
    {
        auto &batch = batches[index];
        if (index < batch_resource_memberships.size())
            render_graph::ResetBatchResourceMembership(batch.resources, batch_resource_memberships[index]);
        else
        {
            assert(batch.resources.empty());
            batch.resources.clear();
        }
    }
    batches.resize(g.passes.size());
    for (auto &batch : batches) batch.Reset();
    auto &submission_order = impl_->submission_order;
    submission_order.clear();
    auto &states = impl_->resource_states;
    states.clear();
    states.reserve(g.resources.size());
    for (auto &resource : g.resources)
    {
        resource.completion = resource.imported_completion;
        const bool decayed = barriers == BarrierMode::Legacy && BufferDecays(resource) &&
            (resource.imported_completion.direct || resource.imported_completion.compute ||
             resource.imported_completion.copy);
        states.push_back({resource.initial, decayed ? Access::Common : resource.initial,
                          resource.initial_queue});
    }
    const auto include_resource = [&](std::uint32_t batch, std::uint32_t resource) {
        auto &target = batches[batch];
        // Use handles are one-based; every path below converts them to the
        // zero-based index used by g.resources before reaching this helper.
        if (batch_resource_memberships.size() <= batch)
            batch_resource_memberships.resize(static_cast<std::size_t>(batch) + 1);
        auto &membership = batch_resource_memberships[batch];
        render_graph::EnsureBatchResourceMembershipDomain(membership, g.resources.size());
        render_graph::AppendUniqueBatchResource(target.resources, membership, resource, g.resources.size());
    };
    const auto depend = [&](std::uint32_t consumer, std::uint32_t producer) {
        if (producer == UINT32_MAX || consumer == producer) return;
        auto &dependencies = batches[consumer].dependencies;
        if (std::find(dependencies.begin(), dependencies.end(), producer) == dependencies.end())
            dependencies.push_back(producer);
    };
    const auto include_external_import = [&](std::uint32_t batch, std::uint32_t resource) {
        const auto &external = g.resources[resource].imported_external_completion;
        if (!external.native_fence_owner && !external.resource_lease) return;
        auto &imports = batches[batch].external_imports;
        if (std::find(imports.begin(), imports.end(), resource) == imports.end()) imports.push_back(resource);
    };
    const auto join_readers = [&](std::uint32_t resource, ResourceState &state) {
        // A release cannot run in one concurrent reader's list. Join all readers
        // before a barrier-only list changes the shared SHADER_RESOURCE layout.
        Batch joined;
        joined.queue = state.queue;
        const auto index = static_cast<std::uint32_t>(batches.size());
        batches.push_back(std::move(joined));
        for (const auto reader : state.readers) depend(index, reader);
        include_resource(index, resource);
        submission_order.push_back(index);
        state.batch = index;
    };
    std::array<std::uint32_t, 3> initial_releases{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    const auto release = [&](std::uint32_t resource, ResourceState &state) {
        if (FixedBufferHeap(g.resources[resource]) != D3D12_HEAP_TYPE_DEFAULT) return;
        if (state.physical == Access::Common || state.physical == Access::Undefined) return;
        if (state.batch == UINT32_MAX)
        {
            auto &initial = initial_releases[QueueIndex(state.queue)];
            if (initial == UINT32_MAX)
            {
                Batch prefix;
                prefix.queue = state.queue;
                initial = static_cast<std::uint32_t>(batches.size());
                batches.push_back(std::move(prefix));
                submission_order.push_back(initial);
            }
            state.batch = initial;
            batches[initial].imported.Merge(g.resources[resource].imported_completion);
            include_external_import(state.batch, resource);
        }
        const auto released = Access::Common;
        batches[state.batch].after.push_back(
            {resource, state.physical, released, BarrierOperation::Release});
        include_resource(state.batch, resource);
        state.physical = released;
    };
    for (std::uint32_t position = 0; position < g.schedule.execution_order.size(); ++position)
    {
        const auto index = g.schedule.execution_order[position];
        const auto &pass = g.passes[index];
        batches[index].queue = pass.queue;
        batches[index].pass = index;
        batches[index].position = position;
        for (const auto &use : pass.Uses())
        {
            const auto resource = use.predecessor.resource.value - 1;
            const auto *boundary = ExternalAccess(pass, use.predecessor.resource);
            const auto incoming = boundary ? boundary->incoming : use.access;
            const auto outgoing = boundary ? boundary->outgoing : use.access;
            auto &state = states[resource];
            const bool shared_read = shared_readers && shareable[resource] && !pass.external && use.reads && !use.writes &&
                incoming == Access::ComputeRead && outgoing == Access::ComputeRead && pass.queue != QueueHint::Copy;
            if (state.read_entry != UINT32_MAX && shared_read)
            {
                assert(state.logical == Access::ComputeRead && state.physical == Access::ComputeRead);
                // This fence includes the writer/layout transition and imported
                // completions, but does not wait for unrelated later readers.
                depend(index, state.read_entry);
                include_resource(index, resource);
                state.readers[QueueIndex(pass.queue)] = index;
                state.queue = pass.queue;
                state.batch = index;
                continue;
            }
            if (state.read_entry != UINT32_MAX)
            {
                if (state.queue != pass.queue) join_readers(resource, state);
                else for (const auto reader : state.readers) depend(index, reader);
                state.read_entry = UINT32_MAX;
                state.readers.fill(UINT32_MAX);
            }
            const bool alias_activation = g.resources[resource].alias_activation &&
                g.resources[resource].first_use == position;
            Access alias_previous = state.physical;
            if (alias_activation)
            {
                const auto &logical = g.resources[resource];
                if (logical.pool_index != SIZE_MAX && logical.pool_index < g.pool->entries.size())
                {
                    const auto &entry = g.pool->entries[logical.pool_index];
                    if (logical.pool_view_index < entry.active_view_states.size())
                        alias_previous = entry.active_view_states[logical.pool_view_index];
                }
            }
            const bool handoff = state.queue != pass.queue;
            if (handoff && !alias_activation) release(resource, state);
            if (state.batch == UINT32_MAX || g.resources[resource].first_use == position)
            {
                batches[index].imported.Merge(g.resources[resource].imported_completion);
                include_external_import(index, resource);
            }
            if (handoff && !alias_activation) depend(index, state.batch);
            if (!alias_activation && !handoff && NeedsMemoryBarrier(state.physical, incoming))
                batches[index].before.push_back({resource, state.physical, incoming, BarrierOperation::Uav});
            batches[index].before.push_back({resource, state.physical, incoming,
                alias_activation ? BarrierOperation::Alias :
                    (handoff ? BarrierOperation::Acquire : BarrierOperation::Transition)});
            if (alias_activation)
                batches[index].before.back().before = alias_previous;
            include_resource(index, resource);
            state.logical = outgoing;
            state.physical = barriers == BarrierMode::Legacy && BufferDecays(g.resources[resource])
                ? Access::Common : outgoing;
            state.queue = pass.queue;
            state.batch = index;
            if (shared_read)
            {
                state.read_entry = index;
                state.readers[QueueIndex(pass.queue)] = index;
            }
        }
        submission_order.push_back(index);
        if (pass.external)
        {
            // Outgoing barriers run only after the provider fence bridge.
            Batch tail;
            tail.queue = pass.queue;
            tail.position = position;
            tail.external_finish = index;
            const auto tail_index = static_cast<std::uint32_t>(batches.size());
            batches.push_back(std::move(tail));
            depend(tail_index, index);
            for (const auto &use : pass.Uses())
            {
                const auto resource = use.predecessor.resource.value - 1;
                include_resource(tail_index, resource);
                states[resource].batch = tail_index;
            }
            submission_order.push_back(tail_index);
        }
    }
    for (const auto &dependency : g.schedule.cross_queue_dependencies)
        depend(dependency.consumer, dependency.producer);
    // End every alias view in COMMON on its own last-use batch. This records
    // the physical state that the next frame-slot activation will inherit and
    // keeps activation queue-neutral after a cross-queue producer.
    for (std::uint32_t resource = 0; resource < g.resources.size(); ++resource)
    {
        const auto &logical = g.resources[resource];
        if (!logical.alias_activation || logical.last_use == UINT32_MAX) continue;
        auto &state = states[resource];
        const auto pass = state.batch;
        if (pass == UINT32_MAX || state.physical == Access::Common ||
            state.physical == Access::Undefined)
            continue;
        batches[pass].after.push_back(
            {resource, state.physical, Access::Common, BarrierOperation::Release});
        include_resource(pass, resource);
        state.physical = Access::Common;
    }
    std::array<std::uint32_t, 3> final_reader_joins{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::array<std::uint32_t, 3> final_exports{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    for (std::uint32_t resource = 0; resource < g.resources.size(); ++resource)
    {
        const auto &logical = g.resources[resource];
        if (!logical.imported && logical.first_use == UINT32_MAX) continue;
        if (logical.alias_activation) continue;
        auto &state = states[resource];
        const auto target = logical.final == Access::Undefined ? state.logical : logical.final;
        if (state.read_entry != UINT32_MAX && target != Access::ComputeRead)
        {
            const bool cross_queue_readers = std::any_of(state.readers.begin(), state.readers.end(),
                [&](const auto reader) {
                    return reader != UINT32_MAX && batches[reader].queue != state.queue;
                });
            if (cross_queue_readers)
            {
                auto &joined = final_reader_joins[QueueIndex(state.queue)];
                if (joined == UINT32_MAX)
                {
                    Batch tail;
                    tail.queue = state.queue;
                    joined = static_cast<std::uint32_t>(batches.size());
                    batches.push_back(std::move(tail));
                    submission_order.push_back(joined);
                }
                for (const auto reader : state.readers) depend(joined, reader);
                include_resource(joined, resource);
                state.batch = joined;
            }
            state.read_entry = UINT32_MAX;
            state.readers.fill(UINT32_MAX);
        }
        const auto queue = QueueAccessCompatible(state.queue, target) ? state.queue : QueueHint::Direct;
        const bool external_import = logical.imported_external_completion.native_fence_owner ||
            logical.imported_external_completion.resource_lease;
        // An untouched import already in its final physical state needs no
        // command list. Its imported completion and ownership remain intact.
        // External leases still require their explicit fence/retention bridge.
        if (state.batch == UINT32_MAX && !external_import &&
            queue == state.queue && target == state.logical && target == state.physical)
            continue;
        if (queue != state.queue) release(resource, state);
        if (state.batch == UINT32_MAX || queue != state.queue)
        {
            const auto previous = state.batch;
            auto &shared_export = final_exports[QueueIndex(queue)];
            auto export_index = external_import ? UINT32_MAX : shared_export;
            if (export_index == UINT32_MAX)
            {
                Batch exported;
                exported.queue = queue;
                export_index = static_cast<std::uint32_t>(batches.size());
                batches.push_back(std::move(exported));
                if (external_import) submission_order.push_back(export_index);
                else shared_export = export_index;
            }
            batches[export_index].imported.Merge(logical.imported_completion);
            if (previous == UINT32_MAX) include_external_import(export_index, resource);
            depend(export_index, previous);
            batches[export_index].before.push_back({resource, state.physical, target,
                queue != state.queue ? BarrierOperation::Acquire : BarrierOperation::Transition});
            include_resource(export_index, resource);
            state.batch = export_index;
        }
        else if (target != state.logical)
        {
            // The final transition is inside the producer list, before any
            // Legacy buffer decay at ExecuteCommandLists completion.
            batches[state.batch].after.push_back({resource, state.logical, target, BarrierOperation::Transition});
        }
        state.logical = target;
        state.physical = barriers == BarrierMode::Legacy && BufferDecays(logical)
            ? Access::Common : target;
        state.queue = queue;
    }
    // These barriers touch distinct resources after their last use. Append
    // them after all release/join batches, including ones discovered later in
    // the resource loop, so every merged dependency has already been submitted.
    // Ordinary pass boundaries (and Legacy decay) are unchanged.
    for (const auto batch : final_exports)
        if (batch != UINT32_MAX) submission_order.push_back(batch);
    for (const auto queue : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
    {
        if (!queries_for(queue) || !readback_for(queue)) continue;
        auto last_query_producer = UINT32_MAX;
        for (const auto index : submission_order)
        {
            const auto &batch = batches[index];
            if (batch.queue != queue ||
                (batch.pass == UINT32_MAX && batch.external_finish == UINT32_MAX) ||
                query_slot_for(batch.position) == UINT32_MAX) continue;
            last_query_producer = index;
        }
        if (last_query_producer == UINT32_MAX) continue;
        Batch resolve;
        resolve.queue = queue;
        resolve.resolve_timestamps = true;
        const auto resolve_index = static_cast<std::uint32_t>(batches.size());
        batches.push_back(std::move(resolve));
        depend(resolve_index, last_query_producer);
        submission_order.push_back(resolve_index);
    }
    // No vectors or graph resource states change after this point until every
    // recording task joins. Each task owns one allocator and one command list.
    std::array<std::size_t, 3> recorder_counts{};
    for (auto &batch : batches)
    {
        const auto queue_index = QueueIndex(batch.queue);
        batch.recorder = recorder_counts[queue_index]++;
        auto &recorders = slot.recorders[queue_index];
        if (batch.recorder >= recorders.size())
        {
            Impl::PassRecorder recorder;
            const auto type = NativeQueueType(batch.queue);
            auto result = impl_->device->CreateCommandAllocator(type, IID_PPV_ARGS(&recorder.allocator));
            if (FAILED(result)) return QueueFailure("Create graph command allocator", result);
            if (impl_->device4)
            {
                result = impl_->device4->CreateCommandList1(
                    0, type, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&recorder.commands));
                if (FAILED(result)) return QueueFailure("Create closed graph command list", result);
            }
            else
            {
                result = impl_->device->CreateCommandList(0, type, recorder.allocator.Get(), nullptr,
                                                          IID_PPV_ARGS(&recorder.commands));
                if (FAILED(result)) return QueueFailure("Create graph command list", result);
                result = recorder.commands->Close();
                if (FAILED(result)) return QueueFailure("Close new graph command list", result);
            }
            recorders.push_back(std::move(recorder));
        }
        auto &recorder = recorders[batch.recorder];
        if (barriers == BarrierMode::Enhanced && !recorder.enhanced && FAILED(recorder.commands.As(&recorder.enhanced)))
            return fail("Enhanced barriers unavailable on graph command list.");
    }
    auto &record_results = impl_->record_results;
    record_results.assign(batches.size(), Result::Success());
    const auto record = [&](std::uint32_t index) {
        const auto &batch = batches[index];
        auto &recorder = slot.recorders[QueueIndex(batch.queue)][batch.recorder];
        auto *queries = queries_for(batch.queue);
        auto *readback = readback_for(batch.queue);
        auto *cpu_timing = batch.pass != UINT32_MAX && !cpu_passes.empty()
            ? &cpu_passes[batch.position] : nullptr;
        if (cpu_timing) cpu_timing->cpu_record_begin_qpc = cpu_counter();
        try
        {
            auto result = recorder.allocator->Reset();
            if (FAILED(result)) { record_results[index] = QueueFailure("Reset graph allocator", result); return; }
            result = recorder.commands->Reset(recorder.allocator.Get(), nullptr);
            if (FAILED(result)) { record_results[index] = QueueFailure("Reset graph command list", result); return; }
            auto *commands = recorder.commands.Get();
            auto *enhanced = barriers == BarrierMode::Enhanced ? recorder.enhanced.Get() : nullptr;
            BufferBarrierBatch buffer_barriers{enhanced};
            const auto emit = [&](const PlannedBarrier &barrier) {
                const auto &resource = g.resources[barrier.resource];
                const bool batch_buffer = enhanced && resource.kind == ResourceKind::Buffer &&
                    barrier.operation != BarrierOperation::Alias;
                // Do not move buffer barriers across texture/global/alias operations.
                if (!batch_buffer) buffer_barriers.Flush();
                auto *buffer_batch = batch_buffer ? &buffer_barriers : nullptr;
                if (barrier.operation == BarrierOperation::Uav)
                    EmitUavBarrier(commands, enhanced, barriers, resource, batch.queue, barrier.before, barrier.after, buffer_batch);
                else if (barrier.operation == BarrierOperation::Alias)
                    EmitAliasBarrier(commands, enhanced, barriers, resource, barrier.before,
                                     barrier.after, batch.queue);
                else if (!(barriers == BarrierMode::Legacy && batch.queue == QueueHint::Copy))
                    Transition(commands, enhanced, barriers, resource, barrier.before, barrier.after, batch.queue,
                               barrier.operation == BarrierOperation::Release, barrier.operation == BarrierOperation::Acquire, buffer_batch);
            };
            for (const auto &barrier : batch.before) emit(barrier);
            buffer_barriers.Flush();
            const auto query_slot = query_slot_for(batch.position);
            const bool pass_has_queries = query_slot != UINT32_MAX;
            const auto query = options ? options->timestamp_base + query_slot * 2 : 0;
            if (batch.pass != UINT32_MAX && g.passes[batch.pass].external && queries &&
                pass_has_queries)
                commands->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, query);
            if (batch.pass != UINT32_MAX && !g.passes[batch.pass].external)
            {
                auto &pass = g.passes[batch.pass];
                if (queries && pass_has_queries)
                    commands->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, query);
#if defined(HS_DEVELOPMENT_TOOLS)
                // Copy lists are left unmarked because PIX may issue compute-only calls.
                if (batch.queue != QueueHint::Copy)
                    PIXBeginEvent(commands, PIX_COLOR_DEFAULT, "%s", pass.Name().c_str());
#endif
                RenderPassContext context(commands, enhanced, barriers, &g, batch.queue);
                if (cpu_timing) cpu_timing->cpu_callback_begin_qpc = cpu_counter();
                if (pass.owner_execute)
                    record_results[index] = pass.owner_execute(context);
                else
                    pass.execute(context);
                if (cpu_timing)
                {
                    cpu_timing->cpu_callback_end_qpc = cpu_counter();
                    cpu_timing->cpu_callback_valid = cpu_timing->cpu_callback_begin_qpc != 0 &&
                        cpu_timing->cpu_callback_end_qpc >= cpu_timing->cpu_callback_begin_qpc;
                }
#if defined(HS_DEVELOPMENT_TOOLS)
                if (batch.queue != QueueHint::Copy) PIXEndEvent(commands);
#endif
                if (queries && pass_has_queries)
                    commands->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
            }
            for (const auto &barrier : batch.after) emit(barrier);
            buffer_barriers.Flush();
            if (batch.external_finish != UINT32_MAX && queries && pass_has_queries)
                commands->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
            if (batch.resolve_timestamps)
                for (std::uint32_t position = 0; position < g.schedule.execution_order.size(); ++position)
                {
                    const auto slot = options && options->timestamp_query_slots &&
                        position < options->timestamp_query_slot_count
                        ? options->timestamp_query_slots[position] : position;
                    if (slot != UINT32_MAX && g.passes[g.schedule.execution_order[position]].queue == batch.queue)
                    {
                        const auto query = options->timestamp_base + slot * 2;
                        commands->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, query, 2, readback,
                                                  static_cast<UINT64>(query) * sizeof(std::uint64_t));
                    }
                }
            result = commands->Close();
            if (FAILED(result)) record_results[index] = QueueFailure("Close graph command list", result);
        }
        catch (const std::exception &error)
        {
            static_cast<void>(recorder.commands->Close());
            record_results[index] = fail(std::string("RenderGraph recording callback failed: ") + error.what());
        }
        catch (...)
        {
            static_cast<void>(recorder.commands->Close());
            record_results[index] = fail("RenderGraph recording callback failed.");
        }
        if (cpu_timing)
        {
            cpu_timing->cpu_record_end_qpc = cpu_counter();
            cpu_timing->cpu_record_valid = cpu_timing->cpu_record_begin_qpc != 0 &&
                cpu_timing->cpu_record_end_qpc >= cpu_timing->cpu_record_begin_qpc;
        }
    };
    impl_->stats.cpu_plan_us = measure_stage();
    Result dispatch = Result::Success();
    if (tasks)
    {
        try
        {
            for (std::uint32_t index = 0; index < batches.size(); ++index)
            {
                if (batches[index].pass != UINT32_MAX && g.passes[batches[index].pass].execute_on_owner)
                    continue;
                dispatch = tasks->submit([&, index] { record(index); });
                if (!dispatch) break;
                ++impl_->stats.record_jobs;
            }
            // Submit every worker list before owner-only SDK recording can block dispatch.
            if (dispatch)
                for (std::uint32_t index = 0; index < batches.size(); ++index)
                    if (batches[index].pass != UINT32_MAX && g.passes[batches[index].pass].execute_on_owner)
                        record(index);
        }
        catch (...) { dispatch = fail("RenderGraph recording task submission threw."); }
        // join is mandatory even after a rejected/throwing submit. All lambda
        // captures remain alive here, before any command list is submitted.
        Result joined = Result::Success();
        try { joined = tasks->join(); }
        catch (...) { joined = fail("RenderGraph recording task join threw."); }
        if (!dispatch) return dispatch;
        if (!joined) return joined;
    }
    else
        for (std::uint32_t index = 0; index < batches.size(); ++index) record(index);
    impl_->stats.cpu_record_us = measure_stage();
    for (const auto &result : record_results) if (!result) return result;

    // Allocate completion/ownership storage before any submission. Callback
    // failure must not leave retirement dependent on a later host allocation.
    std::vector<Impl::ExternalRetention> pending_operations(batches.size());
    std::vector<Impl::ExternalRetention> pending_imports(g.resources.size());
    std::vector<std::size_t> import_retention(g.resources.size(), SIZE_MAX);
    std::vector<std::size_t> operation_retention(batches.size(), SIZE_MAX);
    impl_->external_retained.reserve(impl_->external_retained.size() + g.resources.size() + g.passes.size());
    for (std::size_t index = 0; index < batches.size(); ++index)
    {
        const auto &batch = batches[index];
        if (batch.pass == UINT32_MAX || !g.passes[batch.pass].external) continue;
        auto &pending = pending_operations[index];
        pending.operation = g.passes[batch.pass].external;
        pending.queue = static_cast<ID3D12CommandQueue *>(pending.operation->binding.native_queue_owner.get());
        for (const auto resource : batch.resources)
            pending.resources.emplace_back(static_cast<ID3D12Resource *>(g.resources[resource].native));
        const auto result = impl_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                                      IID_PPV_ARGS(&pending.retirement_fence));
        if (FAILED(result)) return QueueFailure("Create graph provider retirement fence", result);
    }
    for (std::size_t resource = 0; resource < g.resources.size(); ++resource)
    {
        const auto &logical = g.resources[resource];
        if (!logical.imported_external_completion.native_fence_owner &&
            !logical.imported_external_completion.resource_lease) continue;
        pending_imports[resource].provider = logical.imported_external_completion;
        pending_imports[resource].resources.emplace_back(static_cast<ID3D12Resource *>(logical.native));
    }

    auto &signals = impl_->signals;
    signals.assign(batches.size(), 0);
    auto &waits_before = impl_->waits_before;
    auto &signal_after = impl_->signal_after;
    waits_before.assign(batches.size(), false);
    signal_after.assign(batches.size(), false);
    auto &ordering_predecessor = impl_->ordering_predecessor;
    ordering_predecessor.assign(batches.size(), SIZE_MAX);
    auto &pass_positions = impl_->pass_positions;
    pass_positions.clear();
    pass_positions.reserve(g.passes.size());
    for (std::size_t position = 0; position < submission_order.size(); ++position)
    {
        const auto index = submission_order[position];
        const auto &batch = batches[index];
        if (batch.pass != UINT32_MAX)
            pass_positions.emplace_back(g.passes[batch.pass].id, position);
        for (const auto queue : {QueueHint::Direct, QueueHint::Compute, QueueHint::Copy})
            if (queue != batch.queue && batch.imported.Required(queue)) waits_before[index] = true;
        for (const auto producer : batch.dependencies)
            if (batches[producer].queue != batch.queue)
            {
                waits_before[index] = true;
                signal_after[producer] = true;
            }
    }
    std::sort(pass_positions.begin(), pass_positions.end());
    for (const auto &[producer_id, consumer_id] : g.ordering_dependencies)
    {
        const auto producer = std::lower_bound(pass_positions.begin(), pass_positions.end(),
            std::pair{producer_id, std::size_t{0}})->second;
        const auto consumer = std::lower_bound(pass_positions.begin(), pass_positions.end(),
            std::pair{consumer_id, std::size_t{0}})->second;
        auto &previous = ordering_predecessor[submission_order[consumer]];
        previous = previous == SIZE_MAX ? producer : std::max(previous, producer);
    }
    const auto standalone = [&](std::uint32_t index) {
        const auto &batch = batches[index];
        return !batch.external_imports.empty() || batch.external_finish != UINT32_MAX ||
            batch.resolve_timestamps || (batch.pass != UINT32_MAX && g.passes[batch.pass].external);
    };
    auto &lists = impl_->lists;
    lists.reserve(submission_order.size());
    const auto retain = [&](std::uint32_t index, std::uint64_t value) {
        const auto &batch = batches[index];
        completion.Include(batch.queue, value);
        slot.completion.Include(batch.queue, value);
        impl_->submitted.Include(batch.queue, value);
        if (operation_retention[index] != SIZE_MAX)
        {
            auto &retained = impl_->external_retained[operation_retention[index]];
            retained.bridge.Include(batch.queue, value);
            if (value == UINT64_MAX) retained.bridged = false;
        }
        for (const auto resource_index : batch.resources)
        {
            auto &resource = g.resources[resource_index];
            resource.completion.Include(batch.queue, value);
            if (import_retention[resource_index] != SIZE_MAX)
            {
                auto &retained = impl_->external_retained[import_retention[resource_index]];
                retained.bridge.Include(batch.queue, value);
                retained.bridged = value != UINT64_MAX;
            }
            if (!resource.imported && resource.pool_index != SIZE_MAX)
                g.pool->entries[resource.pool_index].completion.Include(batch.queue, value);
        }
    };
    for (std::size_t first = 0; first < submission_order.size();)
    {
        const auto index = submission_order[first];
        const auto &batch = batches[index];
        auto &state = impl_->queues[QueueIndex(batch.queue)];
        auto end = first + 1;
        // Legacy buffer decay needs each ExecuteCommandLists boundary. Enhanced
        // lists can share a call without moving queue waits or delaying signals.
        if (barriers == BarrierMode::Enhanced && !standalone(index))
            while (end < submission_order.size())
            {
                const auto next = submission_order[end];
                const auto predecessor = ordering_predecessor[next];
                if (batches[next].queue != batch.queue || standalone(next) || waits_before[next] ||
                    signal_after[submission_order[end - 1]] ||
                    (predecessor != SIZE_MAX && predecessor >= first)) break;
                ++end;
            }
        for (const auto resource : batch.external_imports)
        {
            if (import_retention[resource] == SIZE_MAX)
            {
                import_retention[resource] = impl_->external_retained.size();
                impl_->external_retained.push_back(std::move(pending_imports[resource]));
            }
            const auto &external = impl_->external_retained[import_retention[resource]].provider;
            if (!external.native_fence_owner) continue;
            const auto waited = state.queue->Wait(
                static_cast<ID3D12Fence *>(external.native_fence_owner.get()), external.value);
            if (FAILED(waited))
            {
                impl_->reliable = false;
                retain(index, UINT64_MAX);
                return QueueFailure("RenderGraph imported provider fence Wait", waited);
            }
            ++impl_->stats.cross_queue_waits;
        }
        CompletionSet waits = batch.imported;
        for (const auto producer : batch.dependencies)
            waits.Include(batches[producer].queue, signals[producer]);
        if (auto result = Join(waits, batch.queue); !result) return result;
        if (batch.pass != UINT32_MAX && g.passes[batch.pass].external)
        {
            // Prefix barriers already reference provider resources. Install
            // durable ownership before submission, including a failed Signal.
            operation_retention[index] = impl_->external_retained.size();
            impl_->external_retained.push_back(std::move(pending_operations[index]));
        }
        lists.clear();
        for (auto position = first; position < end; ++position)
        {
            const auto &submitted_batch = batches[submission_order[position]];
            lists.push_back(slot.recorders[QueueIndex(batch.queue)][submitted_batch.recorder].commands.Get());
        }
        state.queue->ExecuteCommandLists(static_cast<UINT>(lists.size()), lists.data());
        const auto value = state.next_fence++;
        const auto result = state.queue->Signal(state.fence.Get(), value);
        if (FAILED(result))
        {
            impl_->reliable = false;
            for (auto position = first; position < end; ++position)
                retain(submission_order[position], UINT64_MAX);
            return QueueFailure("RenderGraph queue Signal after ExecuteCommandLists", result);
        }
        for (auto position = first; position < end; ++position)
        {
            signals[submission_order[position]] = value;
            retain(submission_order[position], value);
        }
        Result provider_result = Result::Success();
        if (batch.pass != UINT32_MAX && g.passes[batch.pass].external)
        {
            auto &retained = impl_->external_retained[operation_retention[index]];
            const auto external_failure = [&](std::string_view operation, HRESULT result) {
                impl_->reliable = false;
                retain(index, UINT64_MAX);
                return QueueFailure(operation, result);
            };
            const auto incoming = retained.queue->Wait(state.fence.Get(), value);
            if (FAILED(incoming)) return external_failure("RenderGraph provider incoming Wait", incoming);
            ++impl_->stats.cross_queue_waits;
            RenderExternalPassContext context(retained.queue.Get(), &g);
            auto *cpu_timing = !cpu_passes.empty() ? &cpu_passes[batch.position] : nullptr;
            if (cpu_timing) cpu_timing->cpu_provider_submit_begin_qpc = cpu_counter();
            try { provider_result = retained.operation->execute(context, retained.provider); }
            catch (const std::exception &error)
            {
                provider_result = fail(std::string("RenderGraph external callback failed: ") + error.what());
            }
            catch (...) { provider_result = fail("RenderGraph external callback failed."); }
            if (cpu_timing)
            {
                cpu_timing->cpu_provider_submit_end_qpc = cpu_counter();
                cpu_timing->cpu_provider_submit_valid = cpu_timing->cpu_provider_submit_begin_qpc != 0 &&
                    cpu_timing->cpu_provider_submit_end_qpc >= cpu_timing->cpu_provider_submit_begin_qpc;
            }
            const bool valid_provider = ValidExternalCompletion(retained.provider, impl_->device.Get());
            const auto retirement_value = retained.next_value++;
            const auto retirement = retained.queue->Signal(retained.retirement_fence.Get(), retirement_value);
            if (FAILED(retirement)) return external_failure("RenderGraph provider retirement Signal", retirement);
            if (valid_provider && retained.provider.native_fence_owner)
            {
                const auto waited = state.queue->Wait(
                    static_cast<ID3D12Fence *>(retained.provider.native_fence_owner.get()), retained.provider.value);
                if (FAILED(waited)) return external_failure("RenderGraph provider completion Wait", waited);
                ++impl_->stats.cross_queue_waits;
            }
            const auto retired = state.queue->Wait(retained.retirement_fence.Get(), retirement_value);
            if (FAILED(retired)) return external_failure("RenderGraph provider retirement Wait", retired);
            ++impl_->stats.cross_queue_waits;
            const auto bridge = state.next_fence++;
            const auto bridged = state.queue->Signal(state.fence.Get(), bridge);
            if (FAILED(bridged)) return external_failure("RenderGraph provider bridge Signal", bridged);
            signals[index] = bridge;
            retain(index, bridge);
            retained.bridge.Include(batch.queue, bridge);
            retained.bridged = true;
            if (!valid_provider) provider_result = fail("RenderGraph external callback returned an invalid owned fence completion.");
        }
        // Commit only batches that were actually submitted and signaled. A
        // later queue Wait failure preserves a usable partial completion and
        // the exact resource state left by those successful batches.
        for (auto position = first; position < end; ++position)
        {
            const auto &submitted_batch = batches[submission_order[position]];
            // Planning is complete; reuse its physical-state scratch for linear publication.
            for (const auto resource_index : submitted_batch.resources)
                states[resource_index].physical = g.resources[resource_index].current;
            for (const auto &barrier : submitted_batch.before)
                g.resources[barrier.resource].current = states[barrier.resource].physical = barrier.after;
            if (submitted_batch.pass != UINT32_MAX)
                for (const auto &use : g.passes[submitted_batch.pass].Uses())
                {
                    const auto resource_index = use.predecessor.resource.value - 1;
                    const auto *boundary = ExternalAccess(g.passes[submitted_batch.pass], use.predecessor.resource);
                    g.resources[resource_index].current = states[resource_index].physical =
                        boundary ? boundary->outgoing : use.access;
                }
            for (const auto &barrier : submitted_batch.after)
            {
                states[barrier.resource].physical = barrier.after;
                if (barrier.operation != BarrierOperation::Release)
                    g.resources[barrier.resource].current = barrier.after;
            }
            for (const auto resource_index : submitted_batch.resources)
            {
                auto &resource = g.resources[resource_index];
                const auto physical = states[resource_index].physical;
                resource.final_queue = batch.queue;
                if (!resource.imported && resource.pool_index != SIZE_MAX)
                {
                    auto &entry = g.pool->entries[resource.pool_index];
                    entry.state = barriers == BarrierMode::Legacy && BufferDecays(resource)
                        ? Access::Common : physical;
                    entry.queue = batch.queue;
                    if (resource.pool_view_index < entry.active_view_states.size())
                    {
                        entry.active_view_states[resource.pool_view_index] = entry.state;
                        entry.active_view_queues[resource.pool_view_index] = batch.queue;
                    }
                }
            }
        }
        auto *count = batch.queue == QueueHint::Direct ? &impl_->stats.direct_batches :
            batch.queue == QueueHint::Compute ? &impl_->stats.compute_batches : &impl_->stats.copy_batches;
        ++*count;
        if (!provider_result) return provider_result;
        first = end;
    }
    impl_->stats.cpu_submit_us = measure_stage();
    return Result::Success();
}

ID3D12CommandQueue *RenderGraphQueueExecutor::NativeQueue(QueueHint queue) const noexcept
{
    return impl_ && QueueIndex(queue) < impl_->queues.size() ? impl_->queues[QueueIndex(queue)].queue.Get() : nullptr;
}
ID3D12Fence *RenderGraphQueueExecutor::NativeFence(QueueHint queue) const noexcept
{
    return impl_ && QueueIndex(queue) < impl_->queues.size() ? impl_->queues[QueueIndex(queue)].fence.Get() : nullptr;
}
Result RenderGraphQueueExecutor::ReleaseRecordersAfterCompletion()
{
    if (!impl_) return Result::Success();
    const auto completed = Completed();
    if (!impl_->submitted.IsSatisfiedBy(completed) || !impl_->external_retained.empty())
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
            "Drain owned/provider queues before releasing graph command recorders.");
    for (auto &frame : impl_->frames)
        for (auto &recorders : frame.recorders) recorders.clear();
    return Result::Success();
}

RenderPassContext::RenderPassContext(void *native_command_list, void *enhanced_command_list,
                                     BarrierMode barrier_mode, const void *graph, QueueHint queue) noexcept
    : native_command_list_(native_command_list), enhanced_command_list_(enhanced_command_list),
      barrier_mode_(barrier_mode), graph_(graph), queue_(queue)
{
}

void *RenderExternalPassContext::Resolve(ResourceHandle handle) const noexcept
{
    const auto *graph = static_cast<const RenderGraphBuilder::Impl *>(graph_);
    const auto *resource = graph->Find(handle);
    return resource ? resource->native : nullptr;
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
        EmitUavBarrier(static_cast<ID3D12GraphicsCommandList *>(native_command_list_),
                   static_cast<ID3D12GraphicsCommandList7 *>(enhanced_command_list_),
                    barrier_mode_, *resource, queue_);
}

Result RenderPassContext::RecordLegacyResourceCommands(
    std::span<const ExternalResourceAccess> accesses,
    const std::function<Result()> &execute) const
{
    const auto fail = [](std::string message) {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12", std::move(message));
    };
    if (!native_command_list_ || !graph_ || !execute)
        return fail("Legacy SDK recording requires a command list, graph and callback.");
    if (queue_ == QueueHint::Copy || static_cast<unsigned>(queue_) > static_cast<unsigned>(QueueHint::Copy))
        return fail("Legacy SDK recording requires a direct or compute queue.");
    if (barrier_mode_ == BarrierMode::Enhanced && !enhanced_command_list_)
        return fail("Enhanced legacy SDK recording requires an enhanced command list.");
    if (barrier_mode_ != BarrierMode::Legacy && barrier_mode_ != BarrierMode::Enhanced)
        return fail("Legacy SDK recording requires an explicit barrier mode.");

    struct ResourceRecord
    {
        const LogicalResource *resource{};
        Access access{};
        Microsoft::WRL::ComPtr<IUnknown> identity;
    };
    const auto *graph = static_cast<const RenderGraphBuilder::Impl *>(graph_);
    std::vector<ResourceRecord> textures;
    textures.reserve(accesses.size());
    for (const auto &access : accesses)
    {
        if (!access.resource ||
            access.incoming != access.outgoing ||
            (access.incoming != Access::ComputeRead && access.incoming != Access::UnorderedRead &&
             access.incoming != Access::UnorderedWrite) ||
            !QueueAccessCompatible(queue_, access.incoming))
            return fail("Legacy SDK recording requires matching whole-resource SRV or UAV accesses.");

        const auto *resource = graph->Find(access.resource);
        if (!resource || resource->kind != access.resource.kind || !resource->native)
            return fail("Legacy SDK recording references an invalid graph resource.");
        auto *native = static_cast<ID3D12Resource *>(resource->native);
        if ((native->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) !=
                (resource->kind == ResourceKind::Buffer) ||
            FixedBufferHeap(*resource) != D3D12_HEAP_TYPE_DEFAULT)
            return fail("Legacy SDK recording requires correctly typed default-heap resources.");

        Microsoft::WRL::ComPtr<IUnknown> identity;
        if (FAILED(native->QueryInterface(IID_PPV_ARGS(&identity))))
            return fail("Legacy SDK recording cannot identify a graph resource.");
        const auto found = std::find_if(textures.begin(), textures.end(),
            [&](const auto &record) { return record.identity.Get() == identity.Get(); });
        if (found != textures.end())
        {
            if (found->access != access.incoming)
                return fail("Legacy SDK recording has conflicting accesses for one physical resource.");
            continue;
        }
        textures.push_back(ResourceRecord{resource, access.incoming, std::move(identity)});
    }

    if (barrier_mode_ == BarrierMode::Legacy)
        return execute();

    auto *commands = static_cast<ID3D12GraphicsCommandList *>(native_command_list_);
    auto *enhanced = static_cast<ID3D12GraphicsCommandList7 *>(enhanced_command_list_);
    const auto enter_legacy = [&] {
        for (const auto &texture : textures)
        {
            Transition(commands, enhanced, BarrierMode::Enhanced, *texture.resource,
                        texture.access, Access::Common, queue_);
            Transition(commands, enhanced, BarrierMode::Legacy, *texture.resource,
                        Access::Common, texture.access, queue_);
        }
    };
    const auto restore_enhanced = [&] {
        for (auto texture = textures.rbegin(); texture != textures.rend(); ++texture)
        {
            Transition(commands, enhanced, BarrierMode::Legacy, *texture->resource,
                        texture->access, Access::Common, queue_);
            Transition(commands, enhanced, BarrierMode::Enhanced, *texture->resource,
                        Access::Common, texture->access, queue_);
        }
    };

    enter_legacy();
    try
    {
        auto result = execute();
        restore_enhanced();
        return result;
    }
    catch (...)
    {
        restore_enhanced();
        throw;
    }
}

PassBuilder::PassBuilder(void *graph, std::uint32_t pass) noexcept : graph_(graph), pass_(pass)
{
}

void PassBuilder::Read(ResourceVersion version, Access access)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->state = Preparation::Building;
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {version, version, access, false, true, {}});
}

ResourceVersion PassBuilder::Write(ResourceVersion predecessor, Access access)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->state = Preparation::Building;
    const ResourceVersion successor{predecessor.resource, predecessor.ordinal + 1};
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {predecessor, successor, access, true, false, {}});
    return successor;
}

ResourceVersion PassBuilder::ReadWrite(ResourceVersion predecessor, Access access)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->state = Preparation::Building;
    const ResourceVersion successor{predecessor.resource, predecessor.ordinal + 1};
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.push_back(
        {predecessor, successor, access, true, true, {}});
    return successor;
}

void PassBuilder::Read(ResourceVersion version, Access access, BufferRange range)
{
    Read(version, access);
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.back().range = range;
}

ResourceVersion PassBuilder::Write(ResourceVersion predecessor, Access access, BufferRange range)
{
    const auto successor = Write(predecessor, access);
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.back().range = range;
    return successor;
}

ResourceVersion PassBuilder::ReadWrite(ResourceVersion predecessor, Access access, BufferRange range)
{
    const auto successor = ReadWrite(predecessor, access);
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.back().range = range;
    return successor;
}

void PassBuilder::Read(ResourceVersion version, Access access, TextureSubresourceRange range)
{
    Read(version, access);
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.back().range = range;
}

ResourceVersion PassBuilder::Write(ResourceVersion predecessor, Access access, TextureSubresourceRange range)
{
    const auto successor = Write(predecessor, access);
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.back().range = range;
    return successor;
}

ResourceVersion PassBuilder::ReadWrite(ResourceVersion predecessor, Access access, TextureSubresourceRange range)
{
    const auto successor = ReadWrite(predecessor, access);
    static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_].uses.back().range = range;
    return successor;
}

void PassBuilder::SetExecute(std::move_only_function<void(RenderPassContext &)> execute)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->state = Preparation::Building;
    auto &pass = static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_];
    pass.external.reset();
    pass.owner_execute = {};
    pass.execute_on_owner = false;
    pass.execute = std::move(execute);
}

void PassBuilder::SetExecuteChecked(std::move_only_function<Result(RenderPassContext &)> execute)
{
    auto &graph = *static_cast<RenderGraphBuilder::Impl *>(graph_);
    graph.state = Preparation::Building;
    auto &pass = graph.passes[pass_];
    pass.external.reset();
    pass.execute = {};
    pass.owner_execute = std::move(execute);
    pass.execute_on_owner = false;
}

void PassBuilder::SetExecuteOnOwner(std::move_only_function<Result(RenderPassContext &)> execute)
{
    SetExecuteChecked(std::move(execute));
    auto &pass = static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_];
    pass.execute_on_owner = true;
}

void PassBuilder::SetExternalExecute(ExternalQueueBinding binding,
    std::span<const ExternalResourceAccess> accesses,
    std::move_only_function<Result(RenderExternalPassContext &, ExternalQueueCompletion &)> execute)
{
    static_cast<RenderGraphBuilder::Impl *>(graph_)->state = Preparation::Building;
    auto &pass = static_cast<RenderGraphBuilder::Impl *>(graph_)->passes[pass_];
    pass.execute = {};
    pass.owner_execute = {};
    pass.execute_on_owner = false;
    pass.external = std::make_shared<ExternalOperation>();
    pass.external->binding = std::move(binding);
    pass.external->accesses.assign(accesses.begin(), accesses.end());
    pass.external->execute = std::move(execute);
}

void PassBuilder::BindExternalExecute(ExternalQueueBinding binding,
    std::move_only_function<Result(RenderExternalPassContext &, ExternalQueueCompletion &)> execute)
{
    auto &graph = *static_cast<RenderGraphBuilder::Impl *>(graph_);
    graph.state = Preparation::Building;
    auto &pass = graph.passes[pass_];
    if (!pass.external) return;
    pass.external->binding = std::move(binding);
    pass.external->execute = std::move(execute);
}

RenderGraphBuilder::RenderGraphBuilder() : impl_(std::make_unique<Impl>())
{
    impl_->resources.reserve(32);
    impl_->passes.reserve(16);
}

RenderGraphBuilder::~RenderGraphBuilder() = default;

void RenderGraphBuilder::Reset()
{
    impl_->state = Preparation::Building;
    impl_->restored_plan = false;
    impl_->binding_failed = false;
    impl_->pool = nullptr;
    impl_->resources.clear();
    if (impl_->reusable_pass_uses.size() < impl_->passes.size())
        impl_->reusable_pass_uses.resize(impl_->passes.size());
    for (std::size_t index = 0; index < impl_->passes.size(); ++index)
    {
        auto &reusable = impl_->reusable_pass_uses[index];
        reusable.swap(impl_->passes[index].uses);
        reusable.clear();
    }
    impl_->passes.clear();
    impl_->ordering_dependencies.clear();
    // The last successful value-only plan survives frame construction. Ready
    // guards prevent its use until this frame has validated and claimed storage.
    impl_->invalid_final = false;
}

bool RenderGraphBuilder::RestorePlan(PlanHandle handle)
{
    Reset();
    if (!handle) return false;
    if (impl_->topology.handle != handle)
    {
        const auto found = std::find_if(impl_->previous_plans.begin(), impl_->previous_plans.end(),
            [handle](const auto &entry) { return entry.topology.valid && entry.topology.handle == handle; });
        if (found == impl_->previous_plans.end()) return false;
        std::swap(impl_->topology, found->topology);
        std::swap(impl_->schedule, found->schedule);
        std::swap(impl_->alias_plan, found->aliases);
    }
    const auto &plan = impl_->topology;
    if (!plan.valid) return false;
    impl_->resources.reserve(plan.resources.size());
    for (const auto &saved : plan.resources)
    {
        auto &resource = impl_->resources.emplace_back();
        resource.cached_name = &saved.name;
        resource.kind = saved.kind;
        resource.imported = saved.imported;
        resource.texture = saved.texture;
        resource.buffer = saved.buffer;
        resource.final_set = saved.final_set;
        resource.final = saved.final;
        resource.final_version = saved.final_version;
    }
    impl_->passes.reserve(plan.passes.size());
    for (const auto &saved : plan.passes)
    {
        auto &pass = impl_->passes.emplace_back();
        pass.id = saved.id;
        pass.cached_name = &saved.name;
        pass.queue = saved.queue;
        pass.cached_uses = &saved.uses;
        if (saved.execution_kind == 4)
        {
            pass.external = std::make_shared<ExternalOperation>();
            pass.external->binding.queue_type = saved.external_queue;
            pass.external->accesses = saved.boundaries;
        }
    }
    impl_->ordering_dependencies = plan.ordering;
    impl_->restored_plan = true;
    return true;
}

RenderGraphBuilder::PlanHandle RenderGraphBuilder::PreparedPlan() const noexcept
{
    return impl_->state == Preparation::Ready ? impl_->topology.handle : 0;
}

bool RenderGraphBuilder::AllImportsCertified() const noexcept
{
    return std::all_of(impl_->resources.begin(), impl_->resources.end(),
        [](const LogicalResource &resource) {
            return !resource.imported || (resource.certified &&
                resource.native_contract.backing != NativeBackingKind::Unknown);
        });
}

bool RenderGraphBuilder::BindTexture(TextureHandle slot, const ExternalTexture &texture, Access initial)
{
    auto *resource = impl_->restored_plan && impl_->state == Preparation::Building ? impl_->Find(slot) : nullptr;
    if (!resource || !resource->imported || resource->kind != ResourceKind::Texture || resource->native)
    {
        impl_->binding_failed = true;
        return false;
    }
    resource->native = texture.native_resource;
    resource->initial = resource->current = initial;
    resource->initial_queue = resource->final_queue = texture.initial_queue;
    resource->imported_completion = texture.completion;
    resource->imported_external_completion = texture.external_completion;
    resource->certified = texture.contract != nullptr;
    if (texture.contract) resource->native_contract = *texture.contract;
    return true;
}

bool RenderGraphBuilder::BindBuffer(BufferHandle slot, const ExternalBuffer &buffer, Access initial)
{
    auto *resource = impl_->restored_plan && impl_->state == Preparation::Building ? impl_->Find(slot) : nullptr;
    if (!resource || !resource->imported || resource->kind != ResourceKind::Buffer || resource->native)
    {
        impl_->binding_failed = true;
        return false;
    }
    resource->native = buffer.native_resource;
    resource->initial = resource->current = initial;
    resource->initial_queue = resource->final_queue = buffer.initial_queue;
    resource->imported_completion = buffer.completion;
    resource->imported_external_completion = buffer.external_completion;
    resource->certified = buffer.contract != nullptr;
    if (buffer.contract) resource->native_contract = *buffer.contract;
    return true;
}

std::optional<PassBuilder> RenderGraphBuilder::BindPass(std::uint32_t declaration_index,
                                                       std::uint32_t expected_pass_id)
{
    if (!impl_->restored_plan || impl_->state != Preparation::Building ||
        declaration_index >= impl_->passes.size() ||
        impl_->passes[declaration_index].id != expected_pass_id) return std::nullopt;
    const auto &pass = impl_->passes[declaration_index];
    if (pass.execute || pass.owner_execute || (pass.external && pass.external->execute))
        return std::nullopt;
    return PassBuilder(impl_.get(), declaration_index);
}

std::uint32_t RenderGraphBuilder::ResourceCount() const noexcept
{
    return static_cast<std::uint32_t>(impl_->resources.size());
}

TextureHandle RenderGraphBuilder::CreateTexture(const TextureDesc &description, std::string_view name)
{
    impl_->state = Preparation::Building;
    impl_->resources.push_back(
        {std::string(name), ResourceKind::Texture, nullptr, Access::Undefined, Access::Undefined,
         Access::Undefined, false, false});
    impl_->resources.back().texture = description;
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Texture};
}

BufferHandle RenderGraphBuilder::CreateBuffer(const BufferDesc &description, std::string_view name)
{
    impl_->state = Preparation::Building;
    impl_->resources.push_back(
        {std::string(name), ResourceKind::Buffer, nullptr, Access::Undefined, Access::Undefined,
         Access::Undefined, false, false});
    impl_->resources.back().buffer = description;
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Buffer};
}

TextureHandle RenderGraphBuilder::ImportTexture(const ExternalTexture &texture, Access initial,
                                                std::string_view name)
{
    impl_->state = Preparation::Building;
    impl_->resources.push_back({std::string(name), ResourceKind::Texture, texture.native_resource,
                                initial, initial, initial, true, true});
    impl_->resources.back().imported_completion = texture.completion;
    impl_->resources.back().imported_external_completion = texture.external_completion;
    impl_->resources.back().certified = texture.contract != nullptr;
    if (texture.contract) impl_->resources.back().native_contract = *texture.contract;
    impl_->resources.back().initial_queue = texture.initial_queue;
    impl_->resources.back().final_queue = texture.initial_queue;
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Texture};
}

BufferHandle RenderGraphBuilder::ImportBuffer(const ExternalBuffer &buffer, Access initial,
                                              std::string_view name)
{
    impl_->state = Preparation::Building;
    impl_->resources.push_back({std::string(name), ResourceKind::Buffer, buffer.native_resource,
                                initial, initial, initial, true, true});
    impl_->resources.back().imported_completion = buffer.completion;
    impl_->resources.back().imported_external_completion = buffer.external_completion;
    impl_->resources.back().certified = buffer.contract != nullptr;
    if (buffer.contract) impl_->resources.back().native_contract = *buffer.contract;
    impl_->resources.back().initial_queue = buffer.initial_queue;
    impl_->resources.back().final_queue = buffer.initial_queue;
    return {static_cast<std::uint32_t>(impl_->resources.size()), ResourceKind::Buffer};
}

PassBuilder RenderGraphBuilder::AddPass(std::uint32_t pass_id, std::string_view name, QueueHint queue)
{
    impl_->state = Preparation::Building;
    impl_->passes.push_back({pass_id, std::string(name), queue, {}, {}});
    const auto index = impl_->passes.size() - 1;
    if (index < impl_->reusable_pass_uses.size())
    {
        auto &reusable = impl_->reusable_pass_uses[index];
        impl_->passes.back().uses.swap(reusable);
    }
    return PassBuilder(impl_.get(), static_cast<std::uint32_t>(impl_->passes.size() - 1));
}

void RenderGraphBuilder::AddOrderingDependency(std::uint32_t producer_id, std::uint32_t consumer_id)
{
    impl_->state = Preparation::Building;
    impl_->ordering_dependencies.emplace_back(producer_id, consumer_id);
}

std::uint32_t RenderGraphBuilder::DeclaredPassId(std::uint32_t index) const noexcept
{
    return index < impl_->passes.size() ? impl_->passes[index].id : UINT32_MAX;
}

void RenderGraphBuilder::SetFinalAccess(ResourceVersion version, Access access)
{
    impl_->state = Preparation::Building;
    if (auto *logical = impl_->Find(version.resource))
    {
        logical->final = access;
        logical->final_set = true;
        logical->final_version = version;
    }
    else impl_->invalid_final = true;
}

void *RenderGraphBuilder::Resolve(ResourceHandle handle) const noexcept
{
    const auto *resource = impl_->Find(handle);
    // Imports already have a native identity during graph construction. This
    // lets native subgraphs validate caller handles before adding their passes;
    // transient allocations become resolvable only after successful Prepare.
    if (!resource) return nullptr;
    switch (impl_->state)
    {
    case Preparation::Building:
        return resource->imported ? resource->native : nullptr;
    case Preparation::Ready:
        return resource->native;
    case Preparation::Failed:
        return nullptr;
    }
    return nullptr;
}

Result RenderGraphBuilder::Prepare(TransientResourcePool &pool, GraphExecutionStats *capture_cpu)
{
    if (capture_cpu) *capture_cpu = {};
    GraphExecutionStats captured_cpu;
    auto cpu_stage_start = capture_cpu ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    const auto capture_stage = [&](std::uint64_t &duration) {
        if (!capture_cpu) return;
        const auto end = std::chrono::steady_clock::now();
        duration = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - cpu_stage_start).count());
        cpu_stage_start = end;
    };
    impl_->state = Preparation::Failed;
    auto fail = [](std::string message) { return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message)); };
    if (impl_->invalid_final) return fail("RenderGraph final access names a nonexistent resource.");
    if (impl_->binding_failed) return fail("RenderGraph current frame binding does not match the restored plan.");
    if (!pool.impl_->allocator || !pool.impl_->device)
        return fail("RenderGraph transient pool has no allocator or device.");
    if (impl_->plan_device.Get() != pool.impl_->device.Get())
    {
        impl_->topology = {};
        for (auto &previous : impl_->previous_plans) previous = {};
        impl_->plan_device = pool.impl_->device;
    }
    for (auto &resource : impl_->resources)
    {
        resource.first_use = UINT32_MAX; resource.last_use = 0;
        resource.written = resource.imported;
        resource.alias_activation = false;
        if (!resource.imported)
        {
            resource.native = nullptr;
            resource.pool_index = SIZE_MAX;
        }
        if (resource.imported)
        {
            if (impl_->restored_plan && !resource.certified)
                return fail("RenderGraph restored import lacks an owner alias certificate.");
            if (!resource.native || resource.initial == Access::Undefined)
                return fail("RenderGraph import requires a native resource and known initial access.");
            if (resource.certified)
            {
                if (resource.native_contract.native_resource != resource.native ||
                    !resource.native_contract.resource_identity ||
                    resource.native_contract.device_identity != pool.impl_->device_identity.Get() ||
                    !resource.native_contract.generation ||
                    resource.native_contract.backing == NativeBackingKind::Unknown ||
                    ((resource.native_contract.backing == NativeBackingKind::DxgiSwapchain ||
                      resource.native_contract.backing == NativeBackingKind::OwnedReservedTiles) &&
                     (resource.kind != ResourceKind::Texture || resource.native_contract.heap_identity)) ||
                    (resource.native_contract.heap_identity &&
                     (!resource.native_contract.heap_size ||
                      resource.native_contract.heap_offset > UINT64_MAX - resource.native_contract.heap_size)))
                    return fail("RenderGraph imported owner contract is stale or invalid.");
                resource.native_description = resource.native_contract.description;
                resource.fixed_buffer_heap = resource.native_contract.buffer_heap;
            }
            else
            {
                if (!SameDevice(static_cast<ID3D12Resource *>(resource.native), pool.impl_->device.Get()))
                    return fail("RenderGraph imported resource belongs to a different device.");
                resource.native_description = static_cast<ID3D12Resource *>(resource.native)->GetDesc();
            }
            if (QueueIndex(resource.initial_queue) > QueueIndex(QueueHint::Copy) ||
                !QueueAccessCompatible(resource.initial_queue, resource.initial))
                return fail("RenderGraph imported initial access is incompatible with its owning queue.");
            const auto &desc = resource.native_description;
            if ((desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) != (resource.kind == ResourceKind::Buffer))
                return fail("RenderGraph imported resource kind disagrees with its native descriptor.");
            if (resource.kind == ResourceKind::Buffer && !resource.certified)
            {
                D3D12_HEAP_PROPERTIES properties{};
                D3D12_HEAP_FLAGS flags{};
                if (FAILED(static_cast<ID3D12Resource *>(resource.native)->GetHeapProperties(&properties, &flags)))
                    return fail("RenderGraph cannot query an imported buffer heap.");
                resource.fixed_buffer_heap = properties.Type == D3D12_HEAP_TYPE_UPLOAD ||
                    properties.Type == D3D12_HEAP_TYPE_READBACK ? properties.Type : D3D12_HEAP_TYPE_DEFAULT;
            }
            if (!ValidExternalCompletion(resource.imported_external_completion, pool.impl_->device.Get()))
                return fail("RenderGraph import has an invalid owned external fence completion.");
            if (BufferDecays(resource) && resource.imported_external_completion.native_fence_owner &&
                resource.initial != Access::Common)
                return fail("RenderGraph externally completed buffers must be imported in COMMON.");
        }
    }
    capture_stage(captured_cpu.cpu_prepare_validation_us);
    const auto compatible = [](const LogicalResource &r, Access access) {
        const auto fixed_heap = FixedBufferHeap(r);
        if (fixed_heap == D3D12_HEAP_TYPE_READBACK) return access == Access::CopyDest;
        if (fixed_heap == D3D12_HEAP_TYPE_UPLOAD)
            return access == Access::ShaderRead || access == Access::ComputeRead ||
                access == Access::AllShaderRead || access == Access::CopySource ||
                access == Access::VertexBuffer || access == Access::IndexBuffer ||
                access == Access::VertexShaderRead || access == Access::IndirectArgs;
        if (access == Access::Undefined || static_cast<unsigned>(access) > static_cast<unsigned>(Access::ShadingRateSource)) return false;
        const auto &desc = r.native_description;
        const auto flags = r.imported ? static_cast<unsigned>(desc.Flags) : (r.kind == ResourceKind::Texture ? r.texture.flags : r.buffer.flags);
        switch (access)
        {
        case Access::ShadingRateSource:
            return r.kind == ResourceKind::Texture &&
                (r.imported ? desc.Format : static_cast<DXGI_FORMAT>(r.texture.format)) == DXGI_FORMAT_R8_UINT;
        case Access::RenderTarget: return r.kind == ResourceKind::Texture && (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        case Access::DepthRead: case Access::DepthWrite: return r.kind == ResourceKind::Texture && (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        case Access::DepthShaderRead:
            return r.kind == ResourceKind::Texture &&
                   (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
                   !(flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        case Access::UnorderedRead: case Access::UnorderedWrite:
            return (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
        case Access::ShaderRead: case Access::ComputeRead: case Access::AllShaderRead:
            return !(flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        case Access::VertexBuffer: case Access::IndexBuffer: case Access::IndirectArgs:
            return r.kind == ResourceKind::Buffer;
        case Access::VertexShaderRead:
            return r.kind == ResourceKind::Buffer && !(flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
        case Access::Present: return r.imported && r.kind == ResourceKind::Texture;
        default: return true;
        }
    };
    auto &imported_identities = impl_->imported_identities;
    imported_identities.clear();
    imported_identities.reserve(impl_->resources.size());
    auto &placed_imports = impl_->placed_imports;
    placed_imports.clear();
    placed_imports.reserve(impl_->resources.size());
    for (auto &resource : impl_->resources)
    {
        if (!resource.imported) continue;
        if (!compatible(resource, resource.initial))
            return fail("RenderGraph initial access is incompatible with its imported resource.");
        Microsoft::WRL::ComPtr<IUnknown> identity;
        if (!resource.certified &&
            FAILED(static_cast<ID3D12Resource *>(resource.native)->QueryInterface(IID_PPV_ARGS(&identity))))
            return fail("RenderGraph cannot identify an imported resource.");
        resource.imported_identity = reinterpret_cast<std::uintptr_t>(resource.certified
            ? resource.native_contract.resource_identity : identity.Get());
        imported_identities.push_back(resource.imported_identity);
        if (resource.certified && resource.native_contract.heap_identity)
        {
            const auto &contract = resource.native_contract;
            const auto end = contract.heap_offset + contract.heap_size;
            placed_imports.push_back({contract.heap_identity, contract.heap_offset, end});
        }
    }
    std::sort(imported_identities.begin(), imported_identities.end());
    if (std::adjacent_find(imported_identities.begin(), imported_identities.end()) != imported_identities.end())
        return fail("RenderGraph imports one physical resource more than once; reuse its handle.");
    // Nonempty, overflow-checked ranges need only their neighbor on each heap.
    std::sort(placed_imports.begin(), placed_imports.end(), [](const auto &a, const auto &b) {
        return a.heap == b.heap ? a.begin < b.begin : std::less<void *>{}(a.heap, b.heap);
    });
    for (std::size_t index = 1; index < placed_imports.size(); ++index)
        if (placed_imports[index - 1].heap == placed_imports[index].heap &&
            placed_imports[index].begin < placed_imports[index - 1].end)
            return fail("RenderGraph imports overlapping placed resources without an alias contract.");
    for (const auto &pass : impl_->passes)
        if (pass.external)
        {
            const auto &external = *pass.external;
            auto *queue = static_cast<ID3D12CommandQueue *>(external.binding.native_queue_owner.get());
            if (!queue || external.binding.queue_type != pass.queue ||
                queue->GetDesc().Type != NativeQueueType(pass.queue) ||
                !SameDevice(queue, pool.impl_->device.Get()) || !external.execute)
                return fail("RenderGraph external operation requires a matching owned native queue and device.");
        }
    if (impl_->restored_plan && std::any_of(impl_->passes.begin(), impl_->passes.end(),
        [](const Pass &pass) { return !pass.uses.empty(); }))
        return fail("RenderGraph restored passes cannot change their declared resource uses.");
    const auto matches = [&](const TopologyValues &values) {
        return values.Matches(impl_->resources, impl_->passes, impl_->ordering_dependencies,
                              impl_->restored_plan);
    };
    const bool current_matches = matches(impl_->topology);
    if (impl_->restored_plan && !current_matches)
        return fail("RenderGraph restored plan does not match current resource or pass bindings (" +
            impl_->topology.FirstMismatch(impl_->resources, impl_->passes, impl_->ordering_dependencies,
                impl_->restored_plan) + ").");
    bool reused = current_matches;
    if (!reused)
        if (const auto previous = std::find_if(impl_->previous_plans.begin(), impl_->previous_plans.end(),
                [&](const auto &entry) { return matches(entry.topology); }); previous != impl_->previous_plans.end())
        {
            std::swap(impl_->topology, previous->topology);
            std::swap(impl_->schedule, previous->schedule);
            std::swap(impl_->alias_plan, previous->aliases);
            reused = true;
        }
    if (reused) ++impl_->plan_reuses;
    if (!reused)
    {
        ++impl_->plan_input_builds;
        for (const auto &resource : impl_->resources)
        {
            if (resource.imported) continue;
            if (resource.kind == ResourceKind::Texture)
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
        const auto format_planes = [&](DXGI_FORMAT format, std::uint32_t &count) {
            auto &cache = pool.impl_->format_planes;
            const auto found = std::find_if(cache.begin(), cache.end(),
                                           [&](const auto &entry) { return entry.first == format; });
            if (found != cache.end()) { count = found->second; return true; }
            D3D12_FEATURE_DATA_FORMAT_INFO info{format, 0};
            if (!pool.impl_->device ||
                FAILED(pool.impl_->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) ||
                !info.PlaneCount) return false;
            count = info.PlaneCount;
            cache.emplace_back(format, count);
            return true;
        };
        std::vector<render_graph::ResourceInfo> resource_infos;
        resource_infos.reserve(impl_->resources.size());
        for (const auto &r : impl_->resources)
        {
            render_graph::ResourceInfo info;
            info.kind = r.kind;
            info.initialized = r.imported;
            info.buffer_size = r.buffer.size;
            info.final_set = r.final_set;
            info.final_version = r.final_version;
            if (r.final_set && !compatible(r, r.final))
                return fail("RenderGraph final access is incompatible with its resource.");
            if (r.imported)
            {
                const auto &desc = r.native_description;
                info.imported_identity = r.imported_identity;
                if (r.kind == ResourceKind::Buffer) info.buffer_size = desc.Width;
                else
                {
                    info.mip_count = desc.MipLevels;
                    info.array_count = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize;
                    if (!format_planes(desc.Format, info.plane_count))
                        return fail("RenderGraph cannot query imported texture format planes.");
                }
            }
            else if (r.kind == ResourceKind::Texture)
            {
                if (!format_planes(static_cast<DXGI_FORMAT>(r.texture.format), info.plane_count))
                    return fail("RenderGraph cannot query transient texture format planes.");
            }
            resource_infos.push_back(info);
        }
        std::vector<std::vector<std::uint32_t>> predecessor_ids(impl_->passes.size());
        std::vector<std::pair<std::uint32_t, std::size_t>> pass_indices;
        if (!impl_->ordering_dependencies.empty())
        {
            pass_indices.reserve(impl_->passes.size());
            for (std::size_t index = 0; index < impl_->passes.size(); ++index)
                pass_indices.emplace_back(impl_->passes[index].id, index);
            std::sort(pass_indices.begin(), pass_indices.end());
        }
        for (const auto &[producer_id, consumer_id] : impl_->ordering_dependencies)
        {
            const auto consumer = std::lower_bound(pass_indices.begin(), pass_indices.end(),
                std::pair{consumer_id, std::size_t{0}});
            if (consumer == pass_indices.end() || consumer->first != consumer_id)
                return fail("RenderGraph ordering consumer does not exist.");
            predecessor_ids[consumer->second].push_back(producer_id);
        }
        for (std::size_t pass_index = 0; pass_index < impl_->passes.size(); ++pass_index)
        {
            const auto &pass = impl_->passes[pass_index];
            if (pass.external)
            {
                const auto &external = *pass.external;
                if (external.accesses.size() != pass.Uses().size())
                    return fail("RenderGraph external operation requires one boundary for every whole-resource use.");
                for (std::size_t index = 0; index < external.accesses.size(); ++index)
                {
                    const auto &boundary = external.accesses[index];
                    const auto *resource = impl_->Find(boundary.resource);
                    const auto native_flags = resource && resource->imported
                        ? resource->native_description.Flags
                        : static_cast<D3D12_RESOURCE_FLAGS>(resource && resource->kind == ResourceKind::Texture
                            ? resource->texture.flags : resource ? resource->buffer.flags : 0);
                    const bool common_boundary = resource && (resource->kind == ResourceKind::Buffer ||
                        pass.queue == QueueHint::Copy || (native_flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS));
                    if (!resource || !compatible(*resource, boundary.incoming) || !compatible(*resource, boundary.outgoing) ||
                        (resource->imported && !SameDevice(static_cast<ID3D12Resource *>(resource->native), pool.impl_->device.Get())) ||
                        !QueueAccessCompatible(pass.queue, boundary.incoming) ||
                        !QueueAccessCompatible(pass.queue, boundary.outgoing) ||
                        boundary.incoming == Access::Present || boundary.outgoing == Access::Present ||
                        boundary.incoming == Access::DepthRead || boundary.incoming == Access::DepthWrite ||
                        boundary.incoming == Access::DepthShaderRead || boundary.outgoing == Access::DepthRead ||
                        boundary.outgoing == Access::DepthWrite || boundary.outgoing == Access::DepthShaderRead ||
                        (common_boundary &&
                         (boundary.incoming != Access::Common || boundary.outgoing != Access::Common)))
                        return fail("RenderGraph external resource boundary is unsupported or incompatible.");
                    for (std::size_t previous = 0; previous < index; ++previous)
                        if (external.accesses[previous].resource.value == boundary.resource.value)
                            return fail("RenderGraph external resource boundary is duplicated.");
                    const auto use = std::find_if(pass.Uses().begin(), pass.Uses().end(), [&](const auto &item) {
                        return item.predecessor.resource.value == boundary.resource.value &&
                               item.predecessor.resource.kind == boundary.resource.kind;
                    });
                    if (use == pass.Uses().end() || !std::holds_alternative<std::monostate>(use->range))
                        return fail("RenderGraph external boundaries require declared whole-resource uses.");
                    if (!use->writes && (boundary.incoming == Access::RenderTarget ||
                        boundary.incoming == Access::UnorderedWrite || boundary.incoming == Access::CopyDest))
                        return fail("RenderGraph external writable entry requires a declared resource write.");
                }
            }
            for (const auto &use : pass.Uses())
            {
                const auto *resource = impl_->Find(use.predecessor.resource);
                if (!resource || !compatible(*resource, use.access))
                    return fail("RenderGraph access is incompatible with its resource.");
            }
        }
        capture_stage(captured_cpu.cpu_prepare_info_key_us);
        // Only a topology miss builds compiler inputs and validates immutable uses.
        for (std::size_t index = 0; index < resource_infos.size(); ++index)
        {
            const auto &resource = impl_->resources[index];
            auto &info = resource_infos[index];
            info.name = resource.Name();
            if (resource.imported) continue;
            // Allocation requirements derive from the device and retained descriptor contract.
            const auto desc = NativeDesc(resource.kind, resource.texture, resource.buffer);
            const auto allocation = pool.impl_->device->GetResourceAllocationInfo(0, 1, &desc);
            if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX || !allocation.Alignment)
                return fail("RenderGraph transient descriptor returned invalid allocation information.");
            info.allocation_size = allocation.SizeInBytes;
            info.allocation_alignment = allocation.Alignment;
            info.heap_class = static_cast<std::uint32_t>(HeapFlags(resource.kind,
                resource.kind == ResourceKind::Texture ? resource.texture.flags : resource.buffer.flags));
        }
        std::vector<render_graph::PassInfo> pass_infos;
        pass_infos.reserve(impl_->passes.size());
        for (std::size_t index = 0; index < impl_->passes.size(); ++index)
        {
            const auto &pass = impl_->passes[index];
            pass_infos.push_back({pass.id, pass.queue,
                static_cast<bool>(pass.execute) || static_cast<bool>(pass.owner_execute) || static_cast<bool>(pass.external),
                pass.Uses(), std::move(predecessor_ids[index])});
        }
        render_graph::CompiledSchedule schedule;
        render_graph::AliasPlan aliases;
        if (auto result = render_graph::Compile(resource_infos, pass_infos, schedule); !result)
            return result;
        if (auto result = render_graph::PlanTransientAliases(resource_infos, pass_infos,
                                                              schedule, aliases); !result)
            return result;
        // Publish only a fully validated compiler result. A failed candidate
        // leaves the previous successful plan available for a later frame.
        TopologyValues compiled_topology;
        compiled_topology.Capture(impl_->resources, impl_->passes, impl_->ordering_dependencies, schedule, aliases);
        compiled_topology.handle = impl_->next_plan_handle++;
        if (impl_->topology.valid)
        {
            auto &previous = impl_->previous_plans[impl_->next_previous_plan];
            previous.topology = std::move(impl_->topology);
            previous.schedule = std::move(impl_->schedule);
            previous.aliases = std::move(impl_->alias_plan);
            impl_->next_previous_plan = (impl_->next_previous_plan + 1) % impl_->previous_plans.size();
        }
        impl_->schedule = std::move(schedule);
        impl_->alias_plan = std::move(aliases);
        impl_->topology = std::move(compiled_topology);
        ++impl_->plan_compilations;
    }
    if (reused) capture_stage(captured_cpu.cpu_prepare_info_key_us);
    capture_stage(captured_cpu.cpu_prepare_compile_us);
    for (std::size_t i = 0; i < impl_->resources.size(); ++i)
    {
        impl_->resources[i].first_use = impl_->topology.resources[i].first_use;
        impl_->resources[i].last_use = impl_->topology.resources[i].last_use;
    }
    // Validation is complete before any allocation or command callback.
    auto &entries = pool.impl_->entries;
    std::vector<std::size_t> claims;
    const auto allocation_failure = [&](std::string message) {
        for (const auto index : claims)
        {
            entries[index].claimed = false;
            entries[index].active_members = 0;
            entries[index].active_views.clear();
            entries[index].active_view_descs.clear();
            entries[index].active_view_states.clear();
            entries[index].active_view_queues.clear();
        }
        for (auto &logical : impl_->resources)
            if (!logical.imported)
            {
                logical.native = nullptr;
                logical.pool_index = SIZE_MAX;
                logical.alias_activation = false;
            }
        return fail(message);
    };
    for (std::size_t group_index = 0; group_index < impl_->alias_plan.groups.size(); ++group_index)
    {
        const auto &group = impl_->alias_plan.groups[group_index];
        const auto &member_descs = impl_->topology.alias_values[group_index].descriptors;
        if (!pool.impl_->allocator) return allocation_failure("RenderGraph transient pool has no allocator.");
        const auto heap_flags = static_cast<D3D12_HEAP_FLAGS>(group.heap_class);
        auto found = entries.end();
        for (auto candidate = entries.begin(); candidate != entries.end(); ++candidate)
        {
            if (candidate->claimed || candidate->heap_flags != heap_flags ||
                candidate->size < group.size || candidate->alignment < group.alignment)
                continue;
            if (found == entries.end() || candidate->size < found->size ||
                (candidate->size == found->size && candidate->alignment < found->alignment))
                found = candidate;
        }
        if (found == entries.end())
        {
            TransientResourcePool::Impl::Entry entry;
            entry.kind = impl_->resources[group.resources.front()].kind;
            const auto &first = impl_->resources[group.resources.front()];
            entry.texture = first.texture;
            entry.buffer = first.buffer;
            entry.size = group.size;
            entry.alignment = group.alignment;
            entry.heap_flags = heap_flags;
            D3D12_RESOURCE_ALLOCATION_INFO allocation_info{group.size, group.alignment};
            D3D12MA::ALLOCATION_DESC allocation{};
            allocation.HeapType = D3D12_HEAP_TYPE_DEFAULT;
            allocation.ExtraHeapFlags = heap_flags;
            const auto hr = pool.impl_->allocator->AllocateMemory(
                &allocation, &allocation_info, &entry.allocation.allocation);
            if (FAILED(hr)) return allocation_failure("RenderGraph transient backing allocation failed (HRESULT " + std::to_string(hr) + ").");
            entries.push_back(std::move(entry)); found = entries.end() - 1;
        }
        found->claimed = true;
        found->active_members = static_cast<std::uint32_t>(group.resources.size());
        const bool alias_group = impl_->topology.alias_values[group_index].aliased;
        // A previous export or partial submission can leave a view owned by a
        // different queue in a non-COMMON state. The block is completed here;
        // recreate those views so activation starts from a legal neutral state.
        const bool neutral_views = !alias_group ||
            std::all_of(found->active_view_states.begin(), found->active_view_states.end(),
                        [](auto state) { return state == Access::Common; });
        const bool reuse_views = neutral_views &&
            found->active_views.size() == member_descs.size() &&
            found->active_view_descs.size() == member_descs.size() &&
            found->active_view_states.size() == member_descs.size() &&
            found->active_view_queues.size() == member_descs.size() &&
            std::equal(member_descs.begin(), member_descs.end(), found->active_view_descs.begin(), SameDescriptor);
        if (!reuse_views)
        {
            found->active_views.clear();
            found->active_view_descs.clear();
            found->active_view_states.clear();
            found->active_view_queues.clear();
        }
        else
        {
            found->active_view_states.resize(member_descs.size(), Access::Common);
            found->active_view_queues.resize(member_descs.size(), QueueHint::Direct);
        }
        const auto pool_index = static_cast<std::size_t>(found - entries.begin());
        claims.push_back(pool_index);
        for (std::size_t member = 0; member < group.resources.size(); ++member)
        {
            const auto resource_index = group.resources[member];
            auto &r = impl_->resources[resource_index];
            if (!reuse_views)
            {
                found->active_views.emplace_back();
                auto &view = found->active_views.back();
                const auto hr = pool.impl_->allocator->CreateAliasingResource(
                    found->allocation.allocation, 0, &member_descs[member], D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(&view));
                if (FAILED(hr)) return allocation_failure("RenderGraph alias view creation failed for " + r.Name() + " (HRESULT " + std::to_string(hr) + ").");
                found->active_view_states.push_back(Access::Common);
                found->active_view_queues.push_back(QueueHint::Direct);
            }
            r.native = found->active_views[member].Get();
            r.pool_index = pool_index;
            r.pool_view_index = member;
            r.alias_activation = impl_->alias_plan.resource_groups[resource_index] !=
                render_graph::AliasPlan::kExcluded;
            if (r.alias_activation)
            {
                r.initial = Access::Undefined;
                r.current = Access::Undefined;
                r.initial_queue = impl_->passes[impl_->schedule.execution_order[r.first_use]].queue;
            }
            else
            {
                r.initial = found->active_view_states.empty() ? found->state
                                                               : found->active_view_states[member];
                r.current = r.initial;
                r.initial_queue = found->active_view_queues.empty() ? found->queue
                                                                      : found->active_view_queues[member];
            }
            r.final_queue = r.initial_queue;
        }
        if (!reuse_views) found->active_view_descs = member_descs;
    }
    impl_->pool = pool.impl_.get(); impl_->state = Preparation::Ready;
    capture_stage(captured_cpu.cpu_prepare_transient_us);
    if (capture_cpu) *capture_cpu = captured_cpu;
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
    if (impl_->state != Preparation::Ready || !commands ||
        (barriers == BarrierMode::Enhanced && !enhanced))
    {
        return Result::Failure(ErrorCode::InvalidArgument, "hs_renderer_d3d12",
                               "RenderGraph requires a compatible command list.");
    }
    if (impl_->schedule.queue_orders[static_cast<std::size_t>(QueueHint::Compute)].size() != 0 ||
        impl_->schedule.queue_orders[static_cast<std::size_t>(QueueHint::Copy)].size() != 0)
    {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "RenderGraph single-list Execute requires a Direct-only schedule.");
    }
    if (std::any_of(impl_->passes.begin(), impl_->passes.end(), [](const auto &pass) { return bool(pass.external); }) ||
        std::any_of(impl_->resources.begin(), impl_->resources.end(), [](const auto &resource) {
            return bool(resource.imported_external_completion.native_fence_owner) ||
                   bool(resource.imported_external_completion.resource_lease);
        }))
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                               "RenderGraph external work requires the native queue executor.");

    for (auto &resource : impl_->resources)
    {
        resource.current = resource.initial;
        resource.written = resource.imported;
    }

    RenderPassContext context(commands, enhanced, barriers, impl_.get());
    std::uint32_t pass_index{};
    for (const auto scheduled : impl_->schedule.execution_order)
    {
        auto &pass = impl_->passes[scheduled];
        if (!pass.execute && !pass.owner_execute)
        {
            return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                   "RenderGraph pass has no execute callback.");
        }

        for (std::size_t left = 0; left < pass.Uses().size(); ++left)
        {
            const auto &use = pass.Uses()[left];
            auto *resource = impl_->Find(use.predecessor.resource);
            if (!resource || use.access == Access::Undefined)
            {
                return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12",
                                       "RenderGraph pass has an invalid resource access.");
            }
            for (std::size_t right = left + 1; right < pass.Uses().size(); ++right)
            {
                if (pass.Uses()[right].predecessor.resource.value == use.predecessor.resource.value)
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
            // Imported and pooled resources may carry a UAV write from the
            // previous command list. The first use needs the same memory
            // ordering as two UAV passes within this execution.
            const bool alias_activation = resource->alias_activation && resource->first_use == pass_index;
            if (alias_activation)
            {
                Access previous = Access::Common;
                if (resource->pool_index != SIZE_MAX && resource->pool_index < impl_->pool->entries.size())
                {
                    const auto &entry = impl_->pool->entries[resource->pool_index];
                    if (resource->pool_view_index < entry.active_view_states.size())
                        previous = entry.active_view_states[resource->pool_view_index];
                }
                EmitAliasBarrier(commands, enhanced, barriers, *resource, previous, use.access,
                                 pass.queue);
            }
            else if (NeedsMemoryBarrier(resource->current, use.access))
            {
                EmitUavBarrier(commands, enhanced, barriers, *resource, pass.queue, resource->current, use.access);
                Transition(commands, enhanced, barriers, *resource, resource->current, use.access);
            }
            else Transition(commands, enhanced, barriers, *resource, resource->current, use.access);
            resource->current = use.access;
            resource->written |= use.writes;
        }

        if (queries)
        {
            commands->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP,
                               timestamp_base + pass_index * 2);
        }
#if defined(HS_DEVELOPMENT_TOOLS)
        PIXBeginEvent(commands, PIX_COLOR_DEFAULT, "%s", pass.Name().c_str());
#endif
        if (pass.owner_execute)
        {
            if (auto result = pass.owner_execute(context); !result) return result;
        }
        else
            pass.execute(context);
#if defined(HS_DEVELOPMENT_TOOLS)
        PIXEndEvent(commands);
#endif
        if (queries)
        {
            commands->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP,
                               timestamp_base + pass_index * 2 + 1);
        }
        for (const auto &use : pass.Uses())
        {
            auto *resource = impl_->Find(use.predecessor.resource);
            if (!resource || !resource->alias_activation || resource->last_use != pass_index)
                continue;
            Transition(commands, enhanced, barriers, *resource, resource->current, Access::Common,
                       pass.queue);
            if (resource->pool_index != SIZE_MAX && resource->pool_index < impl_->pool->entries.size())
            {
                auto &entry = impl_->pool->entries[resource->pool_index];
                if (resource->pool_view_index < entry.active_view_states.size())
                {
                    entry.active_view_states[resource->pool_view_index] = Access::Common;
                    entry.active_view_queues[resource->pool_view_index] = pass.queue;
                }
                entry.state = Access::Common;
                entry.queue = pass.queue;
            }
        }
        ++pass_index;
    }

    std::vector<std::uint32_t> pool_last(impl_->pool->entries.size(), UINT32_MAX);
    for (auto &resource : impl_->resources)
    {
        if (!resource.imported && resource.first_use == UINT32_MAX) continue;
        if (resource.alias_activation) continue;
        const auto target = resource.final == Access::Undefined ? resource.current : resource.final;
        Transition(commands, enhanced, barriers, resource, resource.current, target);
        resource.current = target;
        if (!resource.imported && resource.pool_index != SIZE_MAX)
        {
            auto &entry = impl_->pool->entries[resource.pool_index];
            const auto physical_target = resource.alias_activation ? Access::Common
                : (barriers == BarrierMode::Legacy && BufferDecays(resource)
                    ? Access::Common : target);
            if (resource.pool_view_index < entry.active_view_states.size())
            {
                entry.active_view_states[resource.pool_view_index] = physical_target;
                entry.active_view_queues[resource.pool_view_index] =
                    resource.final_set ? resource.final_queue : QueueHint::Direct;
            }
            if (pool_last[resource.pool_index] == UINT32_MAX ||
                resource.last_use >= pool_last[resource.pool_index])
            {
                pool_last[resource.pool_index] = resource.last_use;
                entry.state = physical_target;
                entry.queue = resource.final_set ? resource.final_queue : QueueHint::Direct;
            }
        }
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

std::uint32_t RenderGraphBuilder::CompiledPassId(std::uint32_t execution_index) const noexcept
{
    if (impl_->state != Preparation::Ready ||
        execution_index >= impl_->schedule.execution_order.size()) return UINT32_MAX;
    return impl_->passes[impl_->schedule.execution_order[execution_index]].id;
}

std::uint64_t RenderGraphBuilder::PlanCompilationCount() const noexcept
{
    return impl_->plan_compilations;
}

std::uint64_t RenderGraphBuilder::PlanReuseCount() const noexcept
{
    return impl_->plan_reuses;
}

std::uint64_t RenderGraphBuilder::PlanInputBuildCount() const noexcept
{
    return impl_->plan_input_builds;
}

std::string_view RenderGraphBuilder::CompiledPassName(std::uint32_t execution_index) const noexcept
{
    return impl_->state == Preparation::Ready &&
        execution_index < impl_->schedule.execution_order.size()
        ? impl_->passes[impl_->schedule.execution_order[execution_index]].Name() : std::string_view{};
}

QueueHint RenderGraphBuilder::CompiledPassQueue(std::uint32_t execution_index) const noexcept
{
    return impl_->state == Preparation::Ready &&
        execution_index < impl_->schedule.execution_order.size()
        ? impl_->passes[impl_->schedule.execution_order[execution_index]].queue : QueueHint::Direct;
}

std::uint32_t RenderGraphBuilder::QueuePassCount(QueueHint queue) const noexcept
{
    const auto index = static_cast<std::size_t>(queue);
    return impl_->state == Preparation::Ready && index < impl_->schedule.queue_orders.size()
        ? static_cast<std::uint32_t>(impl_->schedule.queue_orders[index].size()) : 0;
}

std::uint32_t RenderGraphBuilder::CompiledQueuePassId(QueueHint queue,
                                                       std::uint32_t queue_index) const noexcept
{
    const auto index = static_cast<std::size_t>(queue);
    if (impl_->state != Preparation::Ready || index >= impl_->schedule.queue_orders.size() ||
        queue_index >= impl_->schedule.queue_orders[index].size()) return UINT32_MAX;
    return impl_->passes[impl_->schedule.queue_orders[index][queue_index]].id;
}

std::uint32_t RenderGraphBuilder::CrossQueueDependencyCount() const noexcept
{
    return impl_->state == Preparation::Ready
        ? static_cast<std::uint32_t>(impl_->schedule.cross_queue_dependencies.size()) : 0;
}

QueueScheduleDependency RenderGraphBuilder::CrossQueueDependency(
    std::uint32_t dependency_index) const noexcept
{
    if (impl_->state != Preparation::Ready ||
        dependency_index >= impl_->schedule.cross_queue_dependencies.size())
        return {};
    const auto &dependency = impl_->schedule.cross_queue_dependencies[dependency_index];
    return {impl_->passes[dependency.producer].id,
            impl_->passes[dependency.consumer].id,
            dependency.producer_queue, dependency.consumer_queue};
}

std::vector<std::uint32_t> RenderGraphBuilder::OutputDependencyClosureSlots(std::uint32_t pass_id) const
{
    const auto &schedule = impl_->schedule;
    if (impl_->state != Preparation::Ready ||
        schedule.predecessors.size() != impl_->passes.size()) return {};
    std::uint32_t output = UINT32_MAX;
    for (std::uint32_t index = 0; index < impl_->passes.size(); ++index)
        if (impl_->passes[index].id == pass_id) { output = index; break; }
    if (output == UINT32_MAX) return {};
    std::vector<std::uint32_t> prior(impl_->passes.size(), UINT32_MAX);
    for (const auto &queue : schedule.queue_orders)
        for (std::uint32_t index = 1; index < queue.size(); ++index)
            prior[queue[index]] = queue[index - 1];
    std::vector<bool> included(impl_->passes.size());
    std::vector<std::uint32_t> pending{output};
    while (!pending.empty())
    {
        const auto pass = pending.back(); pending.pop_back();
        if (included[pass]) continue;
        included[pass] = true;
        for (const auto predecessor : schedule.predecessors[pass]) pending.push_back(predecessor);
        if (prior[pass] != UINT32_MAX) pending.push_back(prior[pass]);
    }
    std::vector<std::uint32_t> slots;
    for (std::uint32_t slot = 0; slot < schedule.execution_order.size(); ++slot)
        if (included[schedule.execution_order[slot]]) slots.push_back(slot);
    return slots;
}

bool RenderGraphBuilder::OutputClosureImportsReady(
    std::span<const std::uint32_t> slots, const CompletionSet &completed) const noexcept
{
    if (impl_->state != Preparation::Ready || slots.empty()) return false;
    for (const auto slot : slots)
    {
        if (slot >= impl_->schedule.execution_order.size()) return false;
        const auto &pass = impl_->passes[impl_->schedule.execution_order[slot]];
        for (const auto &use : pass.Uses())
        {
            const auto *resource = impl_->Find(use.predecessor.resource);
            if (!resource || (resource->imported && !resource->completion.IsSatisfiedBy(completed)))
                return false;
        }
    }
    return true;
}

Access RenderGraphBuilder::FinalAccess(ResourceHandle resource) const noexcept
{
    const auto *logical = impl_->Find(resource);
    return logical ? logical->current : Access::Undefined;
}

CompletionSet RenderGraphBuilder::ResourceCompletion(ResourceHandle resource) const noexcept
{
    const auto *logical = impl_->Find(resource);
    return logical ? logical->completion : CompletionSet{};
}

QueueHint RenderGraphBuilder::FinalQueue(ResourceHandle resource) const noexcept
{
    const auto *logical = impl_->Find(resource);
    return logical ? logical->final_queue : QueueHint::Direct;
}

} // namespace hs
