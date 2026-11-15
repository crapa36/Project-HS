#include "render_graph_validation.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{
using namespace hs;
using namespace hs::render_graph;
using Range = decltype(ResourceUse::range);
constexpr ResourceHandle buffer{1, ResourceKind::Buffer};
constexpr ResourceHandle texture{1, ResourceKind::Texture};

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

ResourceUse Read(std::uint32_t ordinal, Range range = {}, ResourceHandle handle = buffer)
{
    return {{handle, ordinal}, {handle, ordinal}, Access::ComputeRead, false, true, std::move(range)};
}
ResourceUse Write(std::uint32_t predecessor, Range range = {}, bool load = false, ResourceHandle handle = buffer)
{
    return {{handle, predecessor}, {handle, predecessor + 1}, Access::UnorderedWrite, true, load, std::move(range)};
}

struct Graph
{
    struct Pass
    {
        std::uint32_t id;
        std::vector<ResourceUse> uses;
        QueueHint queue{QueueHint::Direct};
        bool callback{true};
    };
    std::vector<ResourceInfo> resources{{ResourceKind::Buffer, false, 64}};
    std::vector<Pass> passes;
    CompiledSchedule schedule;
    void Add(std::uint32_t id, std::initializer_list<ResourceUse> uses) { passes.push_back({id, uses}); }
    bool Compile()
    {
        std::vector<PassInfo> inputs;
        for (const auto &pass : passes) inputs.push_back({pass.id, pass.queue, pass.callback, pass.uses});
        return hs::render_graph::Compile(resources, inputs, schedule).Succeeded();
    }
    bool Order(std::initializer_list<std::uint32_t> ids) const
    {
        std::vector<std::uint32_t> actual;
        for (const auto index : schedule.execution_order) actual.push_back(passes[index].id);
        return actual == std::vector<std::uint32_t>(ids);
    }
};

void BufferBounds()
{
    for (const auto range : {BufferRange{0, 0}, BufferRange{64, 1}, BufferRange{63, 2}})
    {
        Graph g; g.Add(0, {Write(0, range)});
        Check(!g.Compile(), "reject empty, out-of-bounds and overflowing buffer range");
    }
    Graph g;
    g.Add(0, {Write(0, BufferRange{63, 1})});
    g.Add(1, {Read(1, BufferRange{63, 1})});
    Check(g.Compile(), "accept last valid byte");
    g.passes[0].uses[0].range = TextureSubresourceRange{0, 1, 0, 1, 0, 1};
    Check(!g.Compile() && g.schedule.execution_order.empty(), "reject texture range for buffer and clear stale schedule");
}

void Initialization()
{
    Graph g;
    g.Add(0, {Write(0, BufferRange{0, 16})});
    g.Add(1, {Read(1, BufferRange{0, 16})});
    Check(g.Compile(), "partial initialization allows covered read");
    g.passes[1].uses[0].range = BufferRange{0, 17};
    Check(!g.Compile(), "partial initialization rejects read extending into hole");
    g.passes[1].uses[0] = Read(1);
    Check(!g.Compile(), "whole-resource read cannot consume partial initialization");
    g.passes.resize(1);
    g.Add(1, {Write(1, BufferRange{32, 32})});
    g.Add(2, {Read(2, BufferRange{0, 16})});
    Check(g.Compile(), "later partial version inherits previously initialized range");
    g.resources[0].final_set = true; g.resources[0].final_version = {buffer, 2};
    Check(!g.Compile(), "export rejects hole in partial initialization");
    g.Add(3, {Write(2, BufferRange{16, 16})});
    g.Add(4, {Read(3)});
    g.resources[0].final_version.ordinal = 3;
    Check(g.Compile(), "union of adjacent writes initializes complete buffer");
    Graph load; load.Add(0, {Write(0, BufferRange{0, 16}, true)});
    Check(!load.Compile(), "ReadWrite requires initialized predecessor range");
    load.resources[0].initialized = true;
    Check(load.Compile(), "ReadWrite may consume initialized import");
    Graph covered_load;
    covered_load.Add(0, {Write(0, BufferRange{0, 16})});
    covered_load.Add(1, {Write(1, BufferRange{0, 16}, true)});
    Check(covered_load.Compile(), "ReadWrite consumes covered partial predecessor");
    Graph imported;
    imported.resources[0].initialized = true;
    imported.Add(20, {Write(0, BufferRange{0, 16}, true)});
    imported.Add(10, {Read(1)});
    imported.resources[0].final_set = true;
    imported.resources[0].final_version = {buffer, 1};
    Check(imported.Compile() && imported.Order({20, 10}),
          "partial ReadWrite preserves the whole initialized import and RAW order");
    imported.resources[0].final_version.ordinal = 0;
    Check(!imported.Compile(), "initialized import still rejects an old final version");
}

void Dependencies()
{
    Graph g;
    // Registered backwards; IDs deliberately prefer the disjoint writer before
    // the old reader, and the overlapping overwrite before the old reader.
    g.Add(1, {Write(2, BufferRange{0, 16})});
    g.Add(30, {Read(1, BufferRange{0, 16})});
    g.Add(0, {Write(1, BufferRange{32, 16})});
    g.Add(10, {Write(0, BufferRange{0, 16})});
    Check(g.Compile() && g.Order({10, 0, 30, 1}), "distant WAR protects old range without false disjoint WAR");
    Graph inherited;
    inherited.Add(0, {Read(2, BufferRange{0, 16})});
    inherited.Add(20, {Write(1, BufferRange{32, 16})});
    inherited.Add(10, {Write(0, BufferRange{0, 16})});
    Check(inherited.Compile() && inherited.Order({10, 0, 20}), "inherited-range RAW finds earlier producer");
    Graph writers;
    writers.Add(0, {Write(1, BufferRange{32, 16})});
    writers.Add(10, {Write(0, BufferRange{0, 16})});
    Check(writers.Compile() && writers.Order({10, 0}), "disjoint physical writers still serialize");
    Graph whole;
    whole.Add(2, {Read(2)}); whole.Add(1, {Write(1)});
    whole.Add(0, {Write(0)}); whole.Add(3, {Read(1)});
    Check(whole.Compile() && whole.Order({0, 3, 1, 2}), "existing full-resource RAW WAR WAW behavior preserved");
    Graph cycle;
    cycle.resources.push_back(cycle.resources[0]);
    const ResourceHandle other{2, ResourceKind::Buffer};
    cycle.Add(0, {Write(0), Read(1, {}, other)});
    cycle.Add(1, {Write(0, {}, false, other), Read(1)});
    Check(!cycle.Compile(), "resource dependency cycle rejected");
}

void TextureBounds()
{
    Graph g; g.resources[0] = {ResourceKind::Texture, true, 0, 4, 3, 2};
    g.Add(0, {Read(0, TextureSubresourceRange{3, 1, 2, 1, 1, 1}, texture)});
    Check(g.Compile(), "last mip array and plane accepted");
    for (const auto range : {TextureSubresourceRange{0, 0, 0, 1, 0, 1},
                             TextureSubresourceRange{3, 2, 0, 1, 0, 1},
                             TextureSubresourceRange{0, 1, 2, 2, 0, 1},
                             TextureSubresourceRange{0, 1, 0, 1, 1, 2}})
    {
        g.passes[0].uses[0].range = range;
        Check(!g.Compile(), "invalid texture axis rejected without overflow");
    }
    g.passes[0].uses[0].range = BufferRange{0, 1};
    Check(!g.Compile(), "buffer range for texture rejected");
    g.passes[0].uses[0].range = TextureSubresourceRange{0, 1, 1, 1, 0, 1};
    g.resources[0].array_count = 1;
    Check(!g.Compile(), "3D depth slices do not become array slices");
    Graph partial; partial.resources[0] = {ResourceKind::Texture, false, 0, 2, 2, 2};
    const TextureSubresourceRange plane0{0, 2, 0, 2, 0, 1}, plane1{0, 2, 0, 2, 1, 1};
    partial.Add(0, {Write(0, plane0, false, texture)});
    partial.Add(1, {Read(1, plane1, texture)});
    Check(!partial.Compile(), "one texture plane does not initialize the other");
    partial.passes[1].uses[0] = Write(1, plane1, false, texture);
    partial.Add(2, {Read(2, {}, texture)});
    partial.resources[0].final_set = true; partial.resources[0].final_version = {texture, 2};
    Check(partial.Compile(), "both planes initialize whole texture");
    Graph mips; mips.resources[0] = {ResourceKind::Texture, false, 0, 2, 2, 1};
    mips.Add(0, {Write(0, TextureSubresourceRange{0, 1, 0, 2, 0, 1}, false, texture)});
    mips.Add(1, {Read(1, TextureSubresourceRange{1, 1, 0, 1, 0, 1}, texture)});
    Check(!mips.Compile(), "multi-array mip selection preserves holes between subresources");
    Graph arrays; arrays.resources[0] = {ResourceKind::Texture, false, 0, 8, 256, 2};
    const TextureSubresourceRange selected_arrays{0, 8, 64, 128, 0, 2};
    arrays.Add(0, {Write(0, selected_arrays, false, texture)});
    arrays.Add(1, {Read(1, selected_arrays, texture)});
    Check(arrays.Compile(), "full mip chains coalesce selected arrays within each plane");
    arrays.passes[1].uses[0] = Read(1, TextureSubresourceRange{0, 1, 0, 1, 1, 1}, texture);
    Check(!arrays.Compile(), "coalesced arrays preserve the hole between selected planes");
    arrays.passes[0].uses[0] = Write(0, {}, false, texture);
    arrays.passes[1].uses[0] = Read(1, {}, texture);
    Check(arrays.Compile(), "whole texture coalesces all contiguous mips arrays and planes");
    Graph interval_chain; interval_chain.resources[0] = {ResourceKind::Texture, true, 0, 4, 3, 2};
    const TextureSubresourceRange first_slice{0, 1, 0, 2, 0, 1};
    const TextureSubresourceRange disjoint_slice{3, 1, 2, 1, 1, 1};
    interval_chain.Add(20, {Read(0, first_slice, texture)});
    interval_chain.Add(0, {Write(0, first_slice, true, texture)});
    interval_chain.Add(10, {Read(1, first_slice, texture)});
    interval_chain.Add(5, {Read(0, disjoint_slice, texture)});
    Check(interval_chain.Compile() && interval_chain.Order({5, 20, 0, 10}),
          "partial texture reads stay independent while old/read-write/new versions remain ordered");
}

void MultiQueueSchedule()
{
    Graph g;
    g.resources.push_back({ResourceKind::Buffer, false, 64});
    const ResourceHandle second{2, ResourceKind::Buffer};

    auto final_read = Read(1, BufferRange{0, 64}, second);
    final_read.access = Access::ShaderRead;
    g.passes.push_back({10, {final_read}, QueueHint::Direct, true});

    auto first_read = Read(1, BufferRange{0, 64}, buffer);
    auto middle_write = Write(0, BufferRange{0, 64}, false, second);
    g.passes.push_back({20, {first_read, middle_write}, QueueHint::Compute, true});

    auto copy_write = Write(0, BufferRange{0, 64});
    copy_write.access = Access::CopyDest;
    g.passes.push_back({30, {copy_write}, QueueHint::Copy, true});

    Check(g.Compile() && g.Order({30, 20, 10}),
          "cross-queue RAW chain determines global topological order");
    Check(g.schedule.queue_orders[static_cast<std::size_t>(QueueHint::Direct)] ==
              std::vector<std::uint32_t>{0} &&
          g.schedule.queue_orders[static_cast<std::size_t>(QueueHint::Compute)] ==
              std::vector<std::uint32_t>{1} &&
          g.schedule.queue_orders[static_cast<std::size_t>(QueueHint::Copy)] ==
              std::vector<std::uint32_t>{2},
          "queue-local schedules preserve compiled order");
    Check(g.schedule.cross_queue_dependencies.size() == 2,
          "cross-queue RAW chain emits two queue waits");
    Check(g.schedule.predecessors[0] == std::vector<std::uint32_t>{1} &&
          g.schedule.predecessors[1] == std::vector<std::uint32_t>{2},
          "compiled output closure retains the actual resource predecessors");
    const auto &copy_to_compute = g.schedule.cross_queue_dependencies[0];
    const auto &compute_to_direct = g.schedule.cross_queue_dependencies[1];
    Check(copy_to_compute.producer == 2 && copy_to_compute.consumer == 1 &&
              copy_to_compute.producer_queue == QueueHint::Copy &&
              copy_to_compute.consumer_queue == QueueHint::Compute &&
              compute_to_direct.producer == 1 && compute_to_direct.consumer == 0 &&
              compute_to_direct.producer_queue == QueueHint::Compute &&
              compute_to_direct.consumer_queue == QueueHint::Direct,
          "cross-queue dependency identifies producer and consumer queues");

    Graph war;
    war.resources[0].initialized = true;
    auto overwrite = Write(0, BufferRange{0, 64});
    overwrite.access = Access::CopyDest;
    war.passes.push_back({0, {overwrite}, QueueHint::Copy, true});
    auto old_read = Read(0, BufferRange{0, 64});
    old_read.access = Access::ShaderRead;
    war.passes.push_back({10, {old_read}, QueueHint::Direct, true});
    Check(war.Compile() && war.Order({10, 0}) &&
              war.schedule.cross_queue_dependencies.size() == 1 &&
              war.schedule.cross_queue_dependencies[0].producer == 1 &&
              war.schedule.cross_queue_dependencies[0].consumer == 0,
          "cross-queue WAR protects imported old contents");
}

void CompletionSetContract()
{
    CompletionSet required;
    required.Include(QueueHint::Direct, 4);
    required.Include(QueueHint::Direct, 2);
    required.Include(QueueHint::Compute, 7);
    CompletionSet copy_requirement;
    copy_requirement.Include(QueueHint::Copy, 3);
    required.Merge(copy_requirement);
    Check(required.Required(QueueHint::Direct) == 4 &&
              required.Required(QueueHint::Compute) == 7 &&
              required.Required(QueueHint::Copy) == 3,
          "CompletionSet merges per-queue maxima");
    CompletionSet completed;
    completed.Include(QueueHint::Direct, 4);
    completed.Include(QueueHint::Compute, 6);
    completed.Include(QueueHint::Copy, 9);
    Check(!required.IsSatisfiedBy(completed),
          "CompletionSet waits for every required queue");
    completed.Include(QueueHint::Compute, 7);
    Check(required.IsSatisfiedBy(completed),
          "CompletionSet releases only after all queue requirements complete");
}

void PhysicalQueueOwnership()
{
    Graph reads;
    reads.resources[0].initialized = true;
    auto graphics = Read(0, BufferRange{0, 16}); graphics.access = Access::VertexBuffer;
    reads.passes.push_back({10, {graphics}, QueueHint::Direct, true});
    reads.passes.push_back({20, {Read(0, BufferRange{32, 16})}, QueueHint::Compute, true});
    reads.passes.push_back({30, {graphics}, QueueHint::Direct, true});
    Check(reads.Compile() && reads.Order({10, 20, 30}), "read/read schedule remains deterministic");
    Check(reads.schedule.cross_queue_dependencies.empty(),
          "disjoint reads do not add queue serialization edges");
    Graph disjoint;
    disjoint.resources[0].initialized = true;
    auto read = Read(0, BufferRange{0, 16}); read.access = Access::CopySource;
    disjoint.passes.push_back({10, {read}, QueueHint::Copy, true});
    disjoint.passes.push_back({20, {Write(0, BufferRange{32, 16})}, QueueHint::Compute, true});
    Check(disjoint.Compile() && disjoint.schedule.cross_queue_dependencies.empty(),
          "disjoint read/write queues remain independently schedulable");
}

void TransientAliasPlanner()
{
    const auto make_resource = [](std::uint64_t size) {
        ResourceInfo info{ResourceKind::Buffer, false, 64};
        info.allocation_size = size * 1024; // Allocation sizes must respect the 64 KiB alignment.
        info.allocation_alignment = 64 * 1024;
        info.heap_class = 1;
        return info;
    };
    const auto write = [](std::uint32_t value) {
        const ResourceHandle handle{value, ResourceKind::Buffer};
        return ResourceUse{{handle, 0}, {handle, 1}, Access::UnorderedWrite, true, false, {}};
    };
    {
        auto initialized_import = make_resource(32);
        initialized_import.initialized = true;
        std::vector<ResourceInfo> resources{make_resource(128), make_resource(64), initialized_import};
        std::vector<ResourceUse> first_uses{write(1)}, second_uses{write(2)};
        const ResourceHandle imported_handle{3, ResourceKind::Buffer};
        first_uses.push_back({{imported_handle, 0}, {imported_handle, 0},
                              Access::ComputeRead, false, true, {}});
        std::vector<PassInfo> passes{{0, QueueHint::Direct, true, first_uses},
                                     {1, QueueHint::Direct, true, second_uses}};
        CompiledSchedule schedule;
        Check(Compile(resources, passes, schedule).Succeeded(), "serial alias graph compiles");
        AliasPlan plan;
        Check(PlanTransientAliases(resources, passes, schedule, plan).Succeeded(),
              "serial alias graph plans");
        Check(plan.groups.size() == 1 && plan.alias_activation_count == 1 &&
                  plan.transient_backing_bytes < plan.transient_bytes_without_aliasing &&
                  plan.resource_groups.size() == 3 &&
                  plan.resource_groups[2] == AliasPlan::kExcluded &&
                  plan.transient_bytes_without_aliasing == (128 + 64) * 1024,
              "serial transients alias while initialized imported reads stay outside transient groups and byte totals");
        first_uses[1].predecessor.resource = {};
        AliasPlan invalid_plan;
        Check(!PlanTransientAliases(resources, passes, schedule, invalid_plan).Succeeded(),
              "alias planner validates a use handle before skipping an initialized import");
    }
    {
        std::vector<ResourceInfo> resources{make_resource(128), make_resource(64)};
        std::vector<ResourceUse> first_uses{write(1)}, second_uses{write(2)};
        std::vector<PassInfo> passes{{0, QueueHint::Copy, true, first_uses},
                                     {1, QueueHint::Compute, true, second_uses}};
        first_uses[0].access = Access::CopyDest;
        CompiledSchedule schedule;
        Check(Compile(resources, passes, schedule).Succeeded(), "independent queue graph compiles");
        AliasPlan plan;
        Check(PlanTransientAliases(resources, passes, schedule, plan).Succeeded() &&
                  plan.groups.size() == 2,
              "independent queues refuse aliasing without reachability");
    }
    {
        std::vector<ResourceInfo> resources{make_resource(128), make_resource(64)};
        const ResourceHandle first{1, ResourceKind::Buffer};
        std::vector<ResourceUse> producer{ResourceUse{{first, 0}, {first, 1},
                                                       Access::UnorderedWrite, true, false, {}}};
        auto consumer_compute = ResourceUse{{first, 1}, {first, 1}, Access::ComputeRead, false, true, {}};
        auto consumer_copy = ResourceUse{{first, 1}, {first, 1}, Access::CopySource, false, true, {}};
        std::vector<ResourceUse> compute_uses{consumer_compute};
        std::vector<ResourceUse> copy_uses{consumer_copy};
        std::vector<ResourceUse> second_uses{write(2)};
        std::vector<PassInfo> passes{{0, QueueHint::Direct, true, producer},
                                     {1, QueueHint::Compute, true, compute_uses},
                                     {2, QueueHint::Copy, true, copy_uses},
                                     {3, QueueHint::Direct, true, second_uses}};
        CompiledSchedule schedule;
        Check(Compile(resources, passes, schedule).Succeeded(), "fanout alias graph compiles");
        AliasPlan plan;
        Check(PlanTransientAliases(resources, passes, schedule, plan).Succeeded() &&
                  plan.groups.size() == 2,
              "fanout keeps a block until every consumer completes");

        const ResourceHandle second{2, ResourceKind::Buffer};
        std::vector<ResourceUse> empty_uses;
        std::vector<ResourceUse> second_read{Read(1, {}, second)};
        const std::array<std::uint32_t, 1> copy_after_compute{3};
        const std::array<std::uint32_t, 1> second_write_after_copy{5};
        passes = {{0, QueueHint::Direct, true, producer},
                  {1, QueueHint::Compute, true, compute_uses},
                  {2, QueueHint::Copy, true, copy_uses},
                  {3, QueueHint::Compute, true, compute_uses},
                  {4, QueueHint::Direct, true, compute_uses},
                  {5, QueueHint::Copy, true, empty_uses, copy_after_compute},
                  {6, QueueHint::Direct, true, second_uses, second_write_after_copy},
                  {7, QueueHint::Compute, true, second_read}};
        Check(Compile(resources, passes, schedule).Succeeded(),
              "three-queue alias graph with explicit final-reader chain compiles");
        AliasPlan fully_ordered_plan;
        Check(PlanTransientAliases(resources, passes, schedule, fully_ordered_plan).Succeeded() &&
                  fully_ordered_plan.groups.size() == 1 &&
                  fully_ordered_plan.alias_activation_count == 1,
              "all queue readers complete before the second resource begins and permit aliasing");

        passes[5].predecessor_ids = {};
        Check(Compile(resources, passes, schedule).Succeeded(),
              "three-queue graph without the explicit compute-reader edge compiles");
        AliasPlan unordered_plan;
        Check(PlanTransientAliases(resources, passes, schedule, unordered_plan).Succeeded() &&
                  unordered_plan.groups.size() == 2,
              "later same-queue read does not order the second resource's first write after every reader");
    }
    {
        std::vector<ResourceInfo> resources{make_resource(128), make_resource(64)};
        std::vector<ResourceUse> same_pass{write(1), write(2)};
        std::vector<PassInfo> passes{{0, QueueHint::Direct, true, same_pass}};
        CompiledSchedule schedule;
        Check(Compile(resources, passes, schedule).Succeeded(), "same-pass alias graph compiles");
        AliasPlan plan;
        Check(PlanTransientAliases(resources, passes, schedule, plan).Succeeded() &&
                  plan.groups.size() == 2,
              "same-pass resources never alias");
    }
    {
        std::vector<ResourceInfo> resources{make_resource(128), make_resource(64)};
        resources[0].final_set = true;
        resources[0].final_version = {{1, ResourceKind::Buffer}, 1};
        std::vector<ResourceUse> first_uses{write(1)}, second_uses{write(2)};
        std::vector<PassInfo> passes{{0, QueueHint::Direct, true, first_uses},
                                     {1, QueueHint::Direct, true, second_uses}};
        CompiledSchedule schedule;
        Check(Compile(resources, passes, schedule).Succeeded(), "export alias graph compiles");
        AliasPlan plan;
        Check(PlanTransientAliases(resources, passes, schedule, plan).Succeeded() &&
                  plan.resource_groups[0] == AliasPlan::kExcluded && plan.groups.size() == 2 &&
                  plan.alias_activation_count == 0,
              "final exports keep independent backing and are excluded from aliasing");
    }
}

} // namespace

int main()
{
    try
    {
        BufferBounds(); Initialization(); Dependencies(); TextureBounds();
        MultiQueueSchedule(); CompletionSetContract(); PhysicalQueueOwnership(); TransientAliasPlanner();
        std::puts("Render graph CPU range/version validation passed.");
        return 0;
    }
    catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
