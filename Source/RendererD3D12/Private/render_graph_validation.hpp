#pragma once

#include "render_graph.hpp"

#include <array>
#include <span>
#include <variant>
#include <vector>

namespace hs::render_graph
{
// D3D-free inputs to the production graph compiler. Physical state/barriers stay
// in render_graph.cpp; logical byte ranges never imply ranged D3D12 buffers.
struct ResourceUse
{
    ResourceVersion predecessor;
    ResourceVersion successor;
    Access access{};
    bool writes{};
    bool reads{};
    std::variant<std::monostate, BufferRange, TextureSubresourceRange> range;
};

struct ResourceInfo
{
    ResourceKind kind{};
    bool initialized{};
    std::uint64_t buffer_size{};
    std::uint32_t mip_count{1};
    std::uint32_t array_count{1};
    std::uint32_t plane_count{1};
    std::uintptr_t imported_identity{};
    bool final_set{};
    ResourceVersion final_version{};
    // D3D12 allocation properties supplied by the backend. These are kept out
    // of the compiler's descriptor logic so the alias planner remains D3D-free.
    std::uint64_t allocation_size{};
    std::uint64_t allocation_alignment{};
    std::uint32_t heap_class{};
    std::string_view name;
};

struct PassInfo
{
    std::uint32_t id{};
    QueueHint queue{};
    bool has_execute{};
    std::span<const ResourceUse> uses;
    std::span<const std::uint32_t> predecessor_ids;
};

// Pass indices refer to the input PassInfo span. A cross-queue dependency means
// the consumer queue must wait for completion of the producer pass; same-queue
// ordering is represented by queue_orders and needs no fence hop.
struct QueueDependency
{
    std::uint32_t producer{};
    std::uint32_t consumer{};
    QueueHint producer_queue{};
    QueueHint consumer_queue{};
};

struct CompiledSchedule
{
    std::vector<std::uint32_t> execution_order;
    std::array<std::vector<std::uint32_t>, 3> queue_orders;
    std::vector<QueueDependency> cross_queue_dependencies;
    std::vector<std::vector<std::uint32_t>> predecessors;

    void Clear() noexcept
    {
        execution_order.clear();
        for (auto &order : queue_orders) order.clear();
        cross_queue_dependencies.clear();
        predecessors.clear();
    }
};

struct AliasGroup
{
    std::vector<std::uint32_t> resources;
    std::uint64_t size{};
    std::uint64_t alignment{};
    std::uint32_t heap_class{};
};

struct AliasPlan
{
    static constexpr std::uint32_t kExcluded = UINT32_MAX;

    std::vector<AliasGroup> groups;
    std::vector<std::uint32_t> resource_groups;
    std::uint64_t transient_bytes_without_aliasing{};
    std::uint64_t transient_backing_bytes{};
    std::uint32_t alias_activation_count{};

    void Clear() noexcept
    {
        groups.clear();
        resource_groups.clear();
        transient_bytes_without_aliasing = 0;
        transient_backing_bytes = 0;
        alias_activation_count = 0;
    }
};

[[nodiscard]] Result Compile(std::span<const ResourceInfo> resources,
                             std::span<const PassInfo> passes,
                             CompiledSchedule &schedule);

[[nodiscard]] Result Compile(std::span<const ResourceInfo> resources,
                             std::span<const PassInfo> passes,
                             std::vector<std::uint32_t> &execution_order);

// Plans physical aliasing only where the compiled schedule proves that every
// use of the first resource completes before every use of the next resource.
// Imports, final exports, same-pass uses, and unrelated queue work are left out.
[[nodiscard]] Result PlanTransientAliases(std::span<const ResourceInfo> resources,
                                           std::span<const PassInfo> passes,
                                           const CompiledSchedule &schedule,
                                           AliasPlan &plan);
} // namespace hs::render_graph
