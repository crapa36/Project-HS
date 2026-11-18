#include "render_graph_validation.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>

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

bool Overlaps(std::span<const Interval> a, std::span<const Interval> b)
{
    std::size_t left{}, right{};
    const auto *left_data = a.data(), *right_data = b.data();
    while (left < a.size() && right < b.size())
    {
        if (left_data[left].begin < right_data[right].end && right_data[right].begin < left_data[left].end) return true;
        if (left_data[left].end <= right_data[right].begin) ++left;
        else ++right;
    }
    return false;
}

void Include(Intervals &initialized, std::span<const Interval> ranges)
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

bool Covered(std::span<const Interval> initialized, std::span<const Interval> ranges)
{
    const auto *range_data = ranges.data(), *known_data = initialized.data();
    for (std::size_t i = 0; i < ranges.size(); ++i)
    {
        bool covered{};
        for (std::size_t j = 0; j < initialized.size(); ++j)
            if (known_data[j].begin <= range_data[i].begin && known_data[j].end >= range_data[i].end)
            { covered = true; break; }
        if (!covered) return false;
    }
    return true;
}

} // namespace

Result Compile(std::span<const ResourceInfo> resources, std::span<const PassInfo> passes,
               CompiledSchedule &schedule,
               std::span<const std::pair<std::uint32_t, std::uint32_t>> ordering)
{
    schedule.Clear();
    const auto fail = [](std::string message) {
        return Result::Failure(ErrorCode::InvalidState, "hs_renderer_d3d12", std::move(message));
    };
    if (passes.size() >= UINT32_MAX) return fail("RenderGraph has too many passes.");
    const auto pass_count = static_cast<std::uint32_t>(passes.size());
    struct RangeSlice
    {
        std::size_t first{}, count{};
    };
    struct UseInfo
    {
        std::uint32_t pass{};
        RangeSlice ranges;
        std::size_t next{SIZE_MAX};
    };
    struct VersionInfo
    {
        std::int64_t producer{-1};
        RangeSlice written;
        std::size_t first_reader{SIZE_MAX},last_reader{SIZE_MAX};
    };
    // Immutable imports need no version chain or Debug-container allocation.
    std::vector<std::vector<VersionInfo>> versions;
    Intervals interval_arena;
    std::vector<UseInfo> reader_records;
    for (std::size_t i = 0; i < resources.size(); ++i)
    {
        const auto &r = resources[i];
        if (r.kind != ResourceKind::Buffer && r.kind != ResourceKind::Texture)
            return fail("RenderGraph resource kind is invalid.");
        if (r.kind == ResourceKind::Buffer ? !r.buffer_size :
            (!r.mip_count || !r.array_count || !r.plane_count ||
             std::uint64_t(r.mip_count) * r.array_count > UINT32_MAX / r.plane_count))
            return fail("RenderGraph resource range extent is invalid.");
    }
    struct ResourceState
    {
        bool has_writes{};
        std::uint32_t seen_pass{UINT32_MAX};
        std::size_t first_use{};
        std::size_t chain{SIZE_MAX};
    };
    std::vector<ResourceState> resource_states(resources.size());
    auto *const state_data = resource_states.data();
    const auto *const resource_data = resources.data();
    const auto resource_count = resources.size();
    for (const auto &pass : passes)
    {
        const auto *const uses = pass.uses.data();
        const auto use_count = pass.uses.size();
        for (std::size_t i = 0; i < use_count; ++i)
        {
            const auto &use = uses[i];
            if (use.writes && use.predecessor.resource && use.predecessor.resource.value <= resource_count)
                state_data[use.predecessor.resource.value - 1].has_writes = true;
        }
    }
    std::unordered_map<std::uint32_t, std::uint32_t> pass_indices;
    pass_indices.reserve(pass_count);
    for (std::uint32_t index = 0; index < pass_count; ++index)
    {
        const auto &pass = passes[index];
        if (static_cast<unsigned>(pass.queue) > static_cast<unsigned>(QueueHint::Copy))
            return fail("RenderGraph pass queue is invalid.");
        if (!pass.has_execute)
            return fail("RenderGraph pass requires an execute callback.");
        if (!pass_indices.emplace(pass.id, index).second)
            return fail("RenderGraph pass IDs must be unique.");
        const auto *const uses = pass.uses.data();
        const auto use_count = pass.uses.size();
        for (std::size_t i = 0; i < use_count; ++i)
        {
            const auto &use = uses[i];
            const auto handle = use.predecessor.resource;
            if (!handle || handle.value > resource_count)
                return fail("RenderGraph names an invalid or nonexistent resource version.");
            const auto resource_index = handle.value - 1;
            const auto &resource = resource_data[resource_index];
            auto &state = state_data[resource_index];
            if (resource.kind != handle.kind || use.predecessor.ordinal > pass_count ||
                use.successor.resource.value != handle.value || use.successor.resource.kind != handle.kind ||
                use.successor.ordinal > pass_count ||
                std::uint64_t(use.successor.ordinal) != std::uint64_t(use.predecessor.ordinal) + (use.writes ? 1u : 0u))
                return fail("RenderGraph names an invalid or nonexistent resource version.");
            const bool write_access = use.access == Access::RenderTarget || use.access == Access::DepthWrite ||
                use.access == Access::UnorderedWrite || use.access == Access::CopyDest;
            if (use.access == Access::Undefined || static_cast<unsigned>(use.access) > static_cast<unsigned>(Access::ConstantBuffer) ||
                use.writes != write_access || (!use.writes && !use.reads))
                return fail("RenderGraph read/write declaration disagrees with its access.");
            if (!QueueAccessCompatible(pass.queue, use.access))
                return fail("RenderGraph access is incompatible with its pass queue.");
            if (use.access == Access::DepthShaderRead && handle.kind != ResourceKind::Texture)
                return fail("RenderGraph depth shader read requires a texture.");
            if (use.access == Access::ShadingRateSource && handle.kind != ResourceKind::Texture)
                return fail("RenderGraph shading rate source requires a texture.");
            if ((use.access == Access::IndexBuffer || use.access == Access::VertexShaderRead ||
                 use.access == Access::ConstantBuffer) &&
                handle.kind != ResourceKind::Buffer)
                return fail("RenderGraph buffer input requires a buffer.");
            // One callback has one access/state for each physical resource. A
            // disjoint range does not authorize simultaneous physical writers.
            if (state.seen_pass == index)
                return fail("RenderGraph pass " + std::to_string(pass.id) + " declares resource " +
                    std::to_string(handle.value) + " (" + std::string(resource.name) +
                    ") twice; accesses " + std::to_string(static_cast<unsigned>(uses[state.first_use].access)) +
                    " and " + std::to_string(static_cast<unsigned>(use.access)) + ".");
            state.seen_pass = index;
            state.first_use = i;
            const bool immutable = resource.initialized && !state.has_writes &&
                use.predecessor.ordinal == 0;
            // Positive resource extents already prove an unchanged whole-resource read is in bounds.
            if (immutable && std::holds_alternative<std::monostate>(use.range)) continue;
            const auto range_begin = interval_arena.size();
            if (!Normalize(resource, use, interval_arena, !immutable))
                return fail("RenderGraph resource range is empty, out of bounds, or the wrong kind.");
            // Initialized resources with no writers have no version hazards; range/access validation still applies.
            if (immutable) continue;
            const RangeSlice ranges{range_begin, interval_arena.size() - range_begin};
            if (state.chain == SIZE_MAX)
            {
                state.chain = versions.size();
                versions.emplace_back();
            }
            auto &chain = versions[state.chain];
            if (chain.size() <= use.successor.ordinal)
                chain.resize(std::size_t(use.successor.ordinal) + 1);
            if (use.reads)
            {
                auto &version = chain[use.predecessor.ordinal];
                const auto reader_index = reader_records.size();
                reader_records.push_back({index, ranges});
                if (version.last_reader == SIZE_MAX) version.first_reader = reader_index;
                else reader_records[version.last_reader].next = reader_index;
                version.last_reader = reader_index;
            }
            if (use.writes)
            {
                auto &version = chain[use.successor.ordinal];
                if (version.producer >= 0) return fail("RenderGraph branches or duplicates a resource version write.");
                version.producer = index;
                version.written = ranges;
            }
        }
    }
    std::vector<std::vector<std::uint32_t>> successors(pass_count);
    std::vector<std::uint32_t> indegree(pass_count);
    const auto edge = [&](std::int64_t before, std::int64_t after) {
        if (before < 0 || after < 0 || before == after) return;
        successors[before].push_back(static_cast<std::uint32_t>(after));
    };
    for (const auto &[producer_id, consumer_id] : ordering)
    {
        const auto producer = pass_indices.find(producer_id);
        const auto consumer = pass_indices.find(consumer_id);
        if (producer == pass_indices.end() || consumer == pass_indices.end() ||
            producer->second == consumer->second)
            return fail("RenderGraph ordering dependency names an absent or identical pass.");
        schedule.explicit_ordering.emplace_back(producer->second, consumer->second);
        edge(producer->second, consumer->second);
    }
    std::stable_sort(schedule.explicit_ordering.begin(), schedule.explicit_ordering.end(),
        [](const auto &left, const auto &right) { return left.second < right.second; });
    const auto interval_slice = [&](RangeSlice slice) {
        return std::span<const Interval>{interval_arena.data() + slice.first, slice.count};
    };
    Intervals initialized;
    const auto *const readers = reader_records.data();
    for (std::size_t resource_index = 0; resource_index < resources.size(); ++resource_index)
    {
        const auto &r = resources[resource_index];
        const auto chain_index = resource_states[resource_index].chain;
        const auto chain = chain_index == SIZE_MAX ? std::span<const VersionInfo>{}
            : std::span<const VersionInfo>{versions[chain_index]};
        const auto extent = r.kind == ResourceKind::Buffer ? r.buffer_size :
            std::uint64_t(r.mip_count) * r.array_count * r.plane_count;
        // Initially complete resources retain full coverage across later writes.
        initialized.clear();
        std::uint32_t latest{};
        for (std::uint32_t ordinal = 0; ordinal < chain.size(); ++ordinal)
        {
            const auto &version = chain[ordinal];
            if (ordinal && version.producer >= 0)
            {
                if (ordinal > 1 && chain[ordinal - 1].producer < 0)
                    return fail("RenderGraph writes from a nonexistent predecessor version.");
                latest = ordinal;
                if (!r.initialized) Include(initialized, interval_slice(version.written));
                // Physical resource writes remain ordered even when their
                // logical ranges do not overlap (both barrier backends).
                edge(chain[ordinal - 1].producer, version.producer);
            }
            else if (ordinal && version.first_reader != SIZE_MAX)
                return fail("RenderGraph reads a nonexistent resource version.");
            for (auto reader_index = version.first_reader; reader_index != SIZE_MAX;
                 reader_index = readers[reader_index].next)
            {
                const auto &reader = readers[reader_index];
                const auto reader_ranges = interval_slice(reader.ranges);
                if (!r.initialized && !Covered(initialized, reader_ranges))
                    return fail("RenderGraph reads an undefined resource range.");
                // Physical writes already form a chain; the nearest overlapping
                // predecessor/successor orders every more distant writer too.
                for (std::uint32_t writer = ordinal; writer > 0; --writer)
                {
                    if (chain[writer].producer < 0 || !Overlaps(reader_ranges, interval_slice(chain[writer].written))) continue;
                    edge(chain[writer].producer, reader.pass);
                    break;
                }
                for (std::uint32_t writer = ordinal + 1; writer < chain.size(); ++writer)
                {
                    if (chain[writer].producer < 0 || !Overlaps(reader_ranges, interval_slice(chain[writer].written))) continue;
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
    // Multiple resources/explicit ordering may name the same edge. Finalize
    // once before topology so it contributes exactly one indegree decrement.
    for (std::uint32_t before = 0; before < pass_count; ++before)
    {
        auto &next = successors[before];
        if (next.size() > 1)
        {
            auto *const first = next.data();
            const auto last = first + next.size();
            std::sort(first, last);
            next.resize(static_cast<std::size_t>(std::unique(first, last) - first));
        }
        for (const auto after : next)
            ++indegree[after];
    }
    schedule.predecessors.resize(pass_count);
    for (std::uint32_t before = 0; before < pass_count; ++before)
        for (const auto after : successors[before])
            schedule.predecessors[after].push_back(before);
    const auto later_id = [&](std::uint32_t left, std::uint32_t right) {
        return passes[left].id > passes[right].id;
    };
    std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, decltype(later_id)> ready_passes(later_id);
    for (std::uint32_t index = 0; index < pass_count; ++index)
        if (!indegree[index]) ready_passes.push(index);
    std::vector<std::uint32_t> order;
    order.reserve(pass_count);
    for (std::uint32_t position = 0; position < pass_count; ++position)
    {
        if (ready_passes.empty())
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
                    if (indegree[predecessor] != UINT32_MAX &&
                        std::binary_search(successors[predecessor].begin(), successors[predecessor].end(), current))
                    {
                        current = predecessor;
                        break;
                    }
            }
        }
        const auto ready = ready_passes.top();
        ready_passes.pop();
        order.push_back(ready);
        indegree[ready] = UINT32_MAX;
        for (const auto next : successors[ready])
            if (!--indegree[next]) ready_passes.push(next);
    }
    schedule.execution_order = std::move(order);
    for (const auto pass : schedule.execution_order)
        schedule.queue_orders[static_cast<std::size_t>(passes[pass].queue)].push_back(pass);
    std::vector<std::uint32_t> queue_positions(pass_count);
    for (const auto &queue_order : schedule.queue_orders)
        for (std::uint32_t position = 0; position < queue_order.size(); ++position)
            queue_positions[queue_order[position]] = position;
    constexpr std::array<std::uint32_t, 3> absent{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::vector<std::array<std::uint32_t, 3>> earliest_consumer(pass_count, absent);
    std::vector<std::array<std::uint32_t, 3>> later_producer_consumer(pass_count, absent);
    for (std::uint32_t before = 0; before < pass_count; ++before)
        for (const auto after : successors[before])
            if (passes[before].queue != passes[after].queue)
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
    std::vector<std::uint32_t> execution_positions(pass_count);
    for (std::uint32_t position = 0; position < pass_count; ++position)
        execution_positions[schedule.execution_order[position]] = position;
    for (const auto before : schedule.execution_order)
        for (const auto after : successors[before])
            if (passes[before].queue != passes[after].queue)
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
    // Only surviving handoffs need execution ordering; most edges are dominated.
    std::sort(schedule.cross_queue_dependencies.begin(), schedule.cross_queue_dependencies.end(),
        [&](const auto &left, const auto &right) {
            if (left.producer != right.producer)
                return execution_positions[left.producer] < execution_positions[right.producer];
            return execution_positions[left.consumer] < execution_positions[right.consumer];
        });
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

    constexpr std::array<std::uint32_t, 3> absent{UINT32_MAX, UINT32_MAX, UINT32_MAX};
    std::vector<std::uint32_t> queue_position(passes.size());
    std::vector<std::uint32_t> queue_previous(passes.size(), UINT32_MAX);
    for (const auto &order : schedule.queue_orders)
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            queue_position[order[i]] = static_cast<std::uint32_t>(i + 1);
            if (i) queue_previous[order[i]] = order[i - 1];
        }
    std::vector<std::array<std::uint32_t, 3>> waits(passes.size(), absent);
    for (const auto &dependency : schedule.cross_queue_dependencies)
    {
        const auto before = dependency.producer, after = dependency.consumer;
        if (before >= passes.size() || after >= passes.size() || before == after) continue;
        auto &latest = waits[after][static_cast<std::size_t>(passes[before].queue)];
        if (latest == UINT32_MAX || queue_position[latest] < queue_position[before]) latest = before;
    }
    // FIFO order and accepted cross-queue waits need only three progress counters.
    std::vector<std::array<std::uint32_t, 3>> completed(passes.size());
    for (const auto pass : schedule.execution_order)
    {
        auto &clock = completed[pass];
        const auto merge = [&](std::uint32_t predecessor) {
            if (predecessor == UINT32_MAX) return;
            for (std::size_t queue = 0; queue < clock.size(); ++queue)
                clock[queue] = std::max(clock[queue], completed[predecessor][queue]);
        };
        merge(queue_previous[pass]);
        for (const auto predecessor : waits[pass]) merge(predecessor);
        clock[static_cast<std::size_t>(passes[pass].queue)] = queue_position[pass];
    }
    const auto reachable = [&](std::uint32_t from, std::uint32_t to) {
        return from != to && queue_position[from] != 0 &&
            completed[to][static_cast<std::size_t>(passes[from].queue)] >= queue_position[from];
    };

    struct Lifetime
    {
        std::array<std::uint32_t, 3> first{UINT32_MAX, UINT32_MAX, UINT32_MAX};
        std::array<std::uint32_t, 3> last{UINT32_MAX, UINT32_MAX, UINT32_MAX};
        std::uint32_t first_pass{UINT32_MAX};
    };
    std::vector<Lifetime> users(resources.size());
    for (const auto pass : schedule.execution_order)
        for (const auto &use : passes[pass].uses)
        {
            if (!use.predecessor.resource || use.predecessor.resource.value > resources.size())
                return fail("RenderGraph alias planner received an invalid resource use.");
            const auto resource = use.predecessor.resource.value - 1;
            // Imported/initialized resources never enter transient alias groups.
            if (resources[resource].initialized) continue;
            auto &lifetime = users[resource];
            const auto queue = static_cast<std::size_t>(passes[pass].queue);
            if (lifetime.first_pass == UINT32_MAX) lifetime.first_pass = pass;
            if (lifetime.first[queue] == UINT32_MAX) lifetime.first[queue] = pass;
            lifetime.last[queue] = pass;
        }

    std::vector<std::uint32_t> candidates;
    for (std::uint32_t resource = 0; resource < resources.size(); ++resource)
    {
        const auto &info = resources[resource];
        if (info.initialized || users[resource].first_pass == UINT32_MAX) continue;
        if (!info.allocation_size || !info.allocation_alignment) continue;
        candidates.push_back(resource);
        if (std::numeric_limits<std::uint64_t>::max() - plan.transient_bytes_without_aliasing <
            info.allocation_size)
            return fail("RenderGraph alias planner transient size overflow.");
        plan.transient_bytes_without_aliasing += info.allocation_size;
    }
    std::sort(candidates.begin(), candidates.end(), [&](auto left, auto right) {
        const auto left_pass = users[left].first_pass, right_pass = users[right].first_pass;
        return position[left_pass] == position[right_pass] ? left < right
                                                            : position[left_pass] < position[right_pass];
    });

    const auto can_follow = [&](std::uint32_t before, std::uint32_t after) {
        // Each queue orders its other uses before its last / after its first use.
        for (const auto first : users[before].last)
            for (const auto second : users[after].first)
            {
                if (first == UINT32_MAX || second == UINT32_MAX) continue;
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
            // Earlier owners already precede every use of the last owner.
            const auto last = candidate.resources.back();
            if (resources[last].final_set || !can_follow(last, resource)) continue;
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

} // namespace hs::render_graph
