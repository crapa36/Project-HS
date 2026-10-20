#include "render_graph_validation.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace hs::render_graph
{
namespace
{
struct Interval
{
    std::uint64_t begin{}, end{};
};
using Intervals = std::vector<Interval>;

bool Contains(std::uint64_t first, std::uint64_t count, std::uint64_t capacity)
{
    return count && first < capacity && count <= capacity - first;
}

bool Normalize(const ResourceInfo &resource, const ResourceUse &use, Intervals &ranges, bool collect = true)
{
    if (resource.kind == ResourceKind::Buffer)
    {
        if (std::holds_alternative<TextureSubresourceRange>(use.range)) return false;
        const auto range = std::holds_alternative<BufferRange>(use.range)
            ? std::get<BufferRange>(use.range) : BufferRange{0, resource.buffer_size};
        if (!Contains(range.offset, range.size, resource.buffer_size)) return false;
        if (collect) ranges.push_back({range.offset, range.offset + range.size});
        return true;
    }
    if (std::holds_alternative<BufferRange>(use.range)) return false;
    const auto range = std::holds_alternative<TextureSubresourceRange>(use.range)
        ? std::get<TextureSubresourceRange>(use.range)
        : TextureSubresourceRange{0, resource.mip_count, 0, resource.array_count, 0, resource.plane_count};
    if (!Contains(range.first_mip, range.mip_count, resource.mip_count) ||
        !Contains(range.first_array_slice, range.array_slice_count, resource.array_count) ||
        !Contains(range.first_plane, range.plane_count, resource.plane_count)) return false;
    if (!collect) return true;
    const auto plane_size = std::uint64_t(resource.array_count) * resource.mip_count;
    if (range.mip_count == resource.mip_count && range.array_slice_count == resource.array_count)
    {
        const auto first = std::uint64_t(range.first_plane) * plane_size;
        ranges.push_back({first, first + std::uint64_t(range.plane_count) * plane_size});
        return true;
    }
    for (std::uint64_t plane = range.first_plane; plane < std::uint64_t(range.first_plane) + range.plane_count; ++plane)
    {
        if (range.mip_count == resource.mip_count)
        {
            const auto first = plane * plane_size + std::uint64_t(range.first_array_slice) * resource.mip_count;
            ranges.push_back({first, first + std::uint64_t(range.array_slice_count) * resource.mip_count});
        }
        else for (std::uint64_t array = range.first_array_slice; array < std::uint64_t(range.first_array_slice) + range.array_slice_count; ++array)
        {
            const auto first = (plane * resource.array_count + array) * resource.mip_count + range.first_mip;
            ranges.push_back({first, first + range.mip_count});
        }
    }
    return true;
}

bool Overlaps(const Intervals &a, const Intervals &b)
{
    std::size_t left{}, right{};
    while (left < a.size() && right < b.size())
    {
        if (a[left].begin < b[right].end && b[right].begin < a[left].end) return true;
        if (a[left].end <= b[right].begin) ++left;
        else ++right;
    }
    return false;
}

void Include(Intervals &initialized, const Intervals &ranges)
{
    initialized.insert(initialized.end(), ranges.begin(), ranges.end());
    std::sort(initialized.begin(), initialized.end(), [](auto a, auto b) { return a.begin < b.begin; });
    std::size_t count{};
    for (const auto range : initialized)
    {
        if (count && range.begin <= initialized[count - 1].end)
            initialized[count - 1].end = std::max(initialized[count - 1].end, range.end);
        else initialized[count++] = range;
    }
    initialized.resize(count);
}

bool Covered(const Intervals &initialized, const Intervals &ranges)
{
    return std::all_of(ranges.begin(), ranges.end(), [&](const auto range) {
        return std::any_of(initialized.begin(), initialized.end(), [&](const auto known) {
            return known.begin <= range.begin && known.end >= range.end;
        });
    });
}

bool QueueAccessCompatible(QueueHint queue, Access access)
{
    switch (queue)
    {
    case QueueHint::Direct:
        return true;
    case QueueHint::Compute:
        return access == Access::Common || access == Access::ComputeRead ||
               access == Access::UnorderedRead || access == Access::UnorderedWrite ||
               access == Access::CopySource || access == Access::CopyDest ||
               access == Access::IndirectArgs;
    case QueueHint::Copy:
        return access == Access::Common || access == Access::CopySource ||
               access == Access::CopyDest;
    }
    return false;
}
} // namespace

Result Compile(std::span<const ResourceInfo> resources, std::span<const PassInfo> passes,
               CompiledSchedule &schedule)
{
    schedule.Clear();
    const auto fail = [](std::string message) {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
    };
    if (passes.size() >= UINT32_MAX) return fail("RenderGraph has too many passes.");
    const auto pass_count = static_cast<std::uint32_t>(passes.size());
    struct UseInfo
    {
        std::uint32_t pass{};
        Intervals ranges;
    };
    struct VersionInfo
    {
        std::int64_t producer{-1};
        Intervals written;
        std::vector<UseInfo> readers;
    };
    std::vector<std::vector<VersionInfo>> versions(resources.size());
    std::unordered_set<std::uintptr_t> imported_identities;
    imported_identities.reserve(resources.size());
    for (std::size_t i = 0; i < resources.size(); ++i)
    {
        const auto &r = resources[i];
        if (r.kind != ResourceKind::Buffer && r.kind != ResourceKind::Texture)
            return fail("RenderGraph resource kind is invalid.");
        if (r.kind == ResourceKind::Buffer ? !r.buffer_size :
            (!r.mip_count || !r.array_count || !r.plane_count ||
             std::uint64_t(r.mip_count) * r.array_count > UINT32_MAX / r.plane_count))
            return fail("RenderGraph resource range extent is invalid.");
        if (r.imported_identity && !imported_identities.insert(r.imported_identity).second)
            return fail("RenderGraph imports one physical resource more than once; reuse its handle.");
    }
    std::vector<bool> has_writes(resources.size());
    for (const auto &pass : passes)
        for (const auto &use : pass.uses)
            if (use.writes && use.predecessor.resource && use.predecessor.resource.value <= resources.size())
                has_writes[use.predecessor.resource.value - 1] = true;
    Intervals ranges;
    std::vector<std::uint32_t> seen_pass(resources.size(), UINT32_MAX);
    std::vector<std::size_t> first_use(resources.size());
    for (std::uint32_t index = 0; index < pass_count; ++index)
    {
        const auto &pass = passes[index];
        if (static_cast<unsigned>(pass.queue) > static_cast<unsigned>(QueueHint::Copy))
            return fail("RenderGraph pass queue is invalid.");
        if (!pass.has_execute)
            return fail("RenderGraph pass requires an execute callback.");
        for (std::uint32_t previous = 0; previous < index; ++previous)
            if (passes[previous].id == pass.id) return fail("RenderGraph pass IDs must be unique.");
        for (std::size_t i = 0; i < pass.uses.size(); ++i)
        {
            const auto &use = pass.uses[i];
            const auto handle = use.predecessor.resource;
            if (!handle || handle.value > resources.size() || resources[handle.value - 1].kind != handle.kind ||
                use.predecessor.ordinal > pass_count ||
                use.successor.resource.value != handle.value || use.successor.resource.kind != handle.kind ||
                use.successor.ordinal > pass_count ||
                std::uint64_t(use.successor.ordinal) != std::uint64_t(use.predecessor.ordinal) + (use.writes ? 1u : 0u))
                return fail("RenderGraph names an invalid or nonexistent resource version.");
            const bool write_access = use.access == Access::RenderTarget || use.access == Access::DepthWrite ||
                use.access == Access::UnorderedWrite || use.access == Access::CopyDest;
            if (use.access == Access::Undefined || static_cast<unsigned>(use.access) > static_cast<unsigned>(Access::ShadingRateSource) ||
                use.writes != write_access || (!use.writes && !use.reads))
                return fail("RenderGraph read/write declaration disagrees with its access.");
            if (!QueueAccessCompatible(pass.queue, use.access))
                return fail("RenderGraph access is incompatible with its pass queue.");
            if (use.access == Access::DepthShaderRead && handle.kind != ResourceKind::Texture)
                return fail("RenderGraph depth shader read requires a texture.");
            if (use.access == Access::ShadingRateSource && handle.kind != ResourceKind::Texture)
                return fail("RenderGraph shading rate source requires a texture.");
            if ((use.access == Access::IndexBuffer || use.access == Access::VertexShaderRead) &&
                handle.kind != ResourceKind::Buffer)
                return fail("RenderGraph geometry input requires a buffer.");
            // One callback has one access/state for each physical resource. A
            // disjoint range does not authorize simultaneous physical writers.
            const auto resource_index = handle.value - 1;
            if (seen_pass[resource_index] == index)
                return fail("RenderGraph pass " + std::to_string(pass.id) + " declares resource " +
                    std::to_string(handle.value) + " (" + std::string(resources[resource_index].name) +
                    ") twice; accesses " + std::to_string(static_cast<unsigned>(pass.uses[first_use[resource_index]].access)) +
                    " and " + std::to_string(static_cast<unsigned>(use.access)) + ".");
            seen_pass[resource_index] = index;
            first_use[resource_index] = i;
            const bool immutable = resources[resource_index].initialized && !has_writes[resource_index] &&
                use.predecessor.ordinal == 0;
            ranges.clear();
            if (!Normalize(resources[resource_index], use, ranges, !immutable))
                return fail("RenderGraph resource range is empty, out of bounds, or the wrong kind.");
            // Initialized resources with no writers have no version hazards; range/access validation still applies.
            if (immutable) continue;
            auto &chain = versions[handle.value - 1];
            if (chain.size() <= use.successor.ordinal)
                chain.resize(std::size_t(use.successor.ordinal) + 1);
            if (use.reads) chain[use.predecessor.ordinal].readers.push_back({index, ranges});
            if (use.writes)
            {
                auto &version = chain[use.successor.ordinal];
                if (version.producer >= 0) return fail("RenderGraph branches or duplicates a resource version write.");
                version.producer = index;
                version.written = std::move(ranges);
            }
        }
    }
    std::vector<std::vector<std::uint8_t>> edges(pass_count, std::vector<std::uint8_t>(pass_count));
    std::vector<std::uint32_t> indegree(pass_count);
    const auto edge = [&](std::int64_t before, std::int64_t after) {
        if (before < 0 || after < 0 || before == after) return;
        if (!edges[before][after]) { edges[before][after] = 1; ++indegree[after]; }
    };
    for (std::uint32_t consumer = 0; consumer < pass_count; ++consumer)
        for (const auto producer_id : passes[consumer].predecessor_ids)
        {
            const auto producer = std::find_if(passes.begin(), passes.end(),
                [&](const PassInfo &pass) { return pass.id == producer_id; });
            if (producer == passes.end() || producer == passes.begin() + consumer)
                return fail("RenderGraph ordering dependency names an absent or identical pass.");
            edge(producer - passes.begin(), consumer);
        }
    for (std::size_t resource_index = 0; resource_index < resources.size(); ++resource_index)
    {
        const auto &r = resources[resource_index];
        const auto &chain = versions[resource_index];
        const auto extent = r.kind == ResourceKind::Buffer ? r.buffer_size :
            std::uint64_t(r.mip_count) * r.array_count * r.plane_count;
        // Initially complete resources retain full coverage across later writes.
        Intervals initialized;
        std::uint32_t latest{};
        for (std::uint32_t ordinal = 0; ordinal < chain.size(); ++ordinal)
        {
            const auto &version = chain[ordinal];
            if (ordinal && version.producer >= 0)
            {
                if (ordinal > 1 && chain[ordinal - 1].producer < 0)
                    return fail("RenderGraph writes from a nonexistent predecessor version.");
                latest = ordinal;
                if (!r.initialized) Include(initialized, version.written);
                // Physical resource writes remain ordered even when their
                // logical ranges do not overlap (both barrier backends).
                edge(chain[ordinal - 1].producer, version.producer);
            }
            else if (ordinal && !version.readers.empty())
                return fail("RenderGraph reads a nonexistent resource version.");
            for (const auto &reader : version.readers)
            {
                if (!r.initialized && !Covered(initialized, reader.ranges))
                    return fail("RenderGraph reads an undefined resource range.");
                // Physical writes already form a chain; the nearest overlapping
                // predecessor/successor orders every more distant writer too.
                for (std::uint32_t writer = ordinal; writer > 0; --writer)
                {
                    if (chain[writer].producer < 0 || !Overlaps(reader.ranges, chain[writer].written)) continue;
                    edge(chain[writer].producer, reader.pass);
                    break;
                }
                for (std::uint32_t writer = ordinal + 1; writer < chain.size(); ++writer)
                {
                    if (chain[writer].producer < 0 || !Overlaps(reader.ranges, chain[writer].written)) continue;
                    edge(reader.pass, chain[writer].producer);
                    break;
                }
            }
        }
        if (r.final_set && (r.final_version.resource.value != resource_index + 1 ||
                            r.final_version.resource.kind != r.kind || r.final_version.ordinal != latest ||
                            (!r.initialized && !Covered(initialized, Intervals{{0, extent}}))))
            return fail("RenderGraph final access requires the latest fully initialized version.");
    }
    std::vector<std::uint32_t> order;
    order.reserve(pass_count);
    for (std::uint32_t position = 0; position < pass_count; ++position)
    {
        std::uint32_t ready = pass_count;
        for (std::uint32_t index = 0; index < pass_count; ++index)
            if (!indegree[index] && (ready == pass_count || passes[index].id < passes[ready].id)) ready = index;
        if (ready == pass_count)
        {
            // Follow remaining predecessor edges until a pass repeats. This
            // reports an actual cycle instead of every blocked downstream pass.
            std::vector<std::uint32_t> chain;
            auto current = std::uint32_t{};
            while (indegree[current] == UINT32_MAX) ++current;
            for (;;)
            {
                const auto repeated = std::find(chain.begin(), chain.end(), current);
                if (repeated != chain.end())
                {
                    std::string message = "RenderGraph resource versions contain a dependency cycle: " +
                        std::to_string(passes[current].id);
                    for (auto entry = chain.end(); entry != repeated;)
                        message += " -> " + std::to_string(passes[*--entry].id);
                    return fail(message.c_str());
                }
                chain.push_back(current);
                for (std::uint32_t predecessor = 0; predecessor < pass_count; ++predecessor)
                    if (indegree[predecessor] != UINT32_MAX && edges[predecessor][current])
                    {
                        current = predecessor;
                        break;
                    }
            }
        }
        order.push_back(ready);
        indegree[ready] = UINT32_MAX;
        for (std::uint32_t next = 0; next < pass_count; ++next)
            if (edges[ready][next]) --indegree[next];
    }
    schedule.execution_order = order;
    for (const auto pass : order)
        schedule.queue_orders[static_cast<std::size_t>(passes[pass].queue)].push_back(pass);
    std::vector<std::uint32_t> queue_positions(pass_count);
    for (const auto &queue_order : schedule.queue_orders)
        for (std::uint32_t position = 0; position < queue_order.size(); ++position)
            queue_positions[queue_order[position]] = position;
    constexpr std::array<std::uint32_t, 3> absent{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::vector<std::array<std::uint32_t, 3>> earliest_consumer(pass_count, absent);
    std::vector<std::array<std::uint32_t, 3>> later_producer_consumer(pass_count, absent);
    for (std::uint32_t before = 0; before < pass_count; ++before)
        for (std::uint32_t after = 0; after < pass_count; ++after)
            if (edges[before][after] && passes[before].queue != passes[after].queue)
            {
                auto &position = earliest_consumer[before][static_cast<std::size_t>(passes[after].queue)];
                position = std::min(position, queue_positions[after]);
            }
    for (const auto &queue_order : schedule.queue_orders)
    {
        auto earliest = absent;
        for (auto position = queue_order.size(); position != 0; --position)
        {
            const auto pass = queue_order[position - 1];
            later_producer_consumer[pass] = earliest;
            for (std::size_t queue = 0; queue < earliest.size(); ++queue)
                earliest[queue] = std::min(earliest[queue], earliest_consumer[pass][queue]);
        }
    }
    for (const auto before : order)
        for (const auto after : order)
            if (edges[before][after] && passes[before].queue != passes[after].queue)
            {
                // A later signal on the same producer queue, waited by an
                // earlier consumer on its queue, already orders this edge.
                // Keep boundary markers from splitting every async pass.
                const auto queue = static_cast<std::size_t>(passes[after].queue);
                const bool dominated = earliest_consumer[before][queue] < queue_positions[after] ||
                    later_producer_consumer[before][queue] <= queue_positions[after];
                if (dominated) continue;
                schedule.cross_queue_dependencies.push_back(
                    {before, after, passes[before].queue, passes[after].queue});
            }
    return Result::Success();
}

Result PlanTransientAliases(std::span<const ResourceInfo> resources,
                            std::span<const PassInfo> passes,
                            const CompiledSchedule &schedule,
                            AliasPlan &plan)
{
    plan.Clear();
    plan.resource_groups.assign(resources.size(), AliasPlan::kExcluded);
    const auto fail = [](std::string message) {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
    };
    if (schedule.execution_order.size() != passes.size())
        return fail("RenderGraph alias planner requires a complete compiled schedule.");

    std::vector<std::uint32_t> position(passes.size(), UINT32_MAX);
    for (std::uint32_t i = 0; i < schedule.execution_order.size(); ++i)
    {
        const auto pass = schedule.execution_order[i];
        if (pass >= passes.size() || position[pass] != UINT32_MAX)
            return fail("RenderGraph alias planner received an invalid execution order.");
        position[pass] = i;
    }

    std::vector<std::vector<std::uint32_t>> successors(passes.size());
    const auto add_edge = [&](std::uint32_t before, std::uint32_t after) {
        if (before >= passes.size() || after >= passes.size() || before == after) return;
        auto &out = successors[before];
        if (std::find(out.begin(), out.end(), after) == out.end()) out.push_back(after);
    };
    for (const auto &order : schedule.queue_orders)
        for (std::size_t i = 1; i < order.size(); ++i) add_edge(order[i - 1], order[i]);
    for (const auto &dependency : schedule.cross_queue_dependencies)
        add_edge(dependency.producer, dependency.consumer);

    // Alias pairs query the same producer repeatedly; traverse each producer at most once.
    std::vector<std::vector<std::uint8_t>> reachable_from(passes.size());
    std::vector<std::uint32_t> stack;
    const auto reachable = [&](std::uint32_t from, std::uint32_t to) {
        if (from == to || from >= successors.size() || to >= successors.size()) return false;
        auto &seen = reachable_from[from];
        if (seen.empty())
        {
            seen.resize(passes.size());
            stack.clear(); stack.push_back(from); seen[from] = 1;
            while (!stack.empty())
            {
                const auto current = stack.back(); stack.pop_back();
                for (const auto next : successors[current])
                    if (!seen[next]) { seen[next] = 1; stack.push_back(next); }
            }
        }
        return seen[to] != 0;
    };

    std::vector<std::vector<std::uint32_t>> users(resources.size());
    for (std::uint32_t pass = 0; pass < passes.size(); ++pass)
        for (const auto &use : passes[pass].uses)
        {
            if (!use.predecessor.resource || use.predecessor.resource.value > resources.size())
                return fail("RenderGraph alias planner received an invalid resource use.");
            auto &resource_users = users[use.predecessor.resource.value - 1];
            if (resource_users.empty() || resource_users.back() != pass)
                resource_users.push_back(pass);
        }

    std::vector<std::uint32_t> candidates;
    for (std::uint32_t resource = 0; resource < resources.size(); ++resource)
    {
        const auto &info = resources[resource];
        if (info.initialized || users[resource].empty()) continue;
        if (!info.allocation_size || !info.allocation_alignment) continue;
        candidates.push_back(resource);
        if (std::numeric_limits<std::uint64_t>::max() - plan.transient_bytes_without_aliasing <
            info.allocation_size)
            return fail("RenderGraph alias planner transient size overflow.");
        plan.transient_bytes_without_aliasing += info.allocation_size;
    }
    std::sort(candidates.begin(), candidates.end(), [&](auto left, auto right) {
        const auto first = [&](auto resource) {
            return *std::min_element(users[resource].begin(), users[resource].end(),
                [&](auto a, auto b) { return position[a] < position[b]; });
        };
        const auto left_pass = first(left), right_pass = first(right);
        return position[left_pass] == position[right_pass] ? left < right
                                                            : position[left_pass] < position[right_pass];
    });

    const auto can_follow = [&](std::uint32_t before, std::uint32_t after) {
        for (const auto first : users[before])
            for (const auto second : users[after])
            {
                if (first == second || position[first] >= position[second] || !reachable(first, second))
                    return false;
            }
        return true;
    };
    const auto aligned_size = [](std::uint64_t size, std::uint64_t alignment) {
        if (!alignment || size > std::numeric_limits<std::uint64_t>::max() - (alignment - 1))
            return std::uint64_t{0};
        return (size + alignment - 1) / alignment * alignment;
    };

    for (const auto resource : candidates)
    {
        const auto &info = resources[resource];
        // A final export must retain its own backing block. It participates in
        // memory accounting, but never becomes an alias successor.
        if (info.final_set)
        {
            AliasGroup group;
            group.resources.push_back(resource);
            group.size = aligned_size(info.allocation_size, info.allocation_alignment);
            group.alignment = info.allocation_alignment;
            group.heap_class = info.heap_class;
            if (!group.size) return fail("RenderGraph alias planner alignment overflow.");
            plan.groups.push_back(std::move(group));
            continue;
        }
        std::size_t best = SIZE_MAX;
        std::uint64_t best_size = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t group = 0; group < plan.groups.size(); ++group)
        {
            auto &candidate = plan.groups[group];
            if (candidate.heap_class != info.heap_class) continue;
            if (std::any_of(candidate.resources.begin(), candidate.resources.end(),
                            [&](auto member) { return resources[member].final_set; }))
                continue;
            bool ordered = true;
            for (const auto member : candidate.resources)
                if (!can_follow(member, resource)) { ordered = false; break; }
            if (!ordered) continue;
            const auto alignment = std::max(candidate.alignment, info.allocation_alignment);
            const auto size = aligned_size(std::max(candidate.size, info.allocation_size), alignment);
            if (!size) return fail("RenderGraph alias planner alignment overflow.");
            if (size < best_size) { best = group; best_size = size; }
        }
        if (best == SIZE_MAX)
        {
            AliasGroup group;
            group.resources.push_back(resource);
            group.size = aligned_size(info.allocation_size, info.allocation_alignment);
            group.alignment = info.allocation_alignment;
            group.heap_class = info.heap_class;
            if (!group.size) return fail("RenderGraph alias planner alignment overflow.");
            plan.groups.push_back(std::move(group));
            best = plan.groups.size() - 1;
        }
        else
        {
            auto &group = plan.groups[best];
            group.alignment = std::max(group.alignment, info.allocation_alignment);
            group.size = aligned_size(std::max(group.size, info.allocation_size), group.alignment);
            group.resources.push_back(resource);
            ++plan.alias_activation_count;
        }
        plan.resource_groups[resource] = static_cast<std::uint32_t>(best);
    }
    for (const auto &group : plan.groups)
    {
        if (std::numeric_limits<std::uint64_t>::max() - plan.transient_backing_bytes < group.size)
            return fail("RenderGraph alias planner backing size overflow.");
        plan.transient_backing_bytes += group.size;
    }
    return Result::Success();
}

Result Compile(std::span<const ResourceInfo> resources, std::span<const PassInfo> passes,
               std::vector<std::uint32_t> &execution_order)
{
    CompiledSchedule schedule;
    const auto result = Compile(resources, passes, schedule);
    execution_order = std::move(schedule.execution_order);
    return result;
}
} // namespace hs::render_graph
