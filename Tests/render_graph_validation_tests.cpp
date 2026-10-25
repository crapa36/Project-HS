#include "render_graph_validation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string_view>
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

void QueueAccessPolicy()
{
    constexpr std::array compute{Access::Common, Access::ComputeRead, Access::UnorderedRead,
        Access::UnorderedWrite, Access::CopySource, Access::CopyDest, Access::IndirectArgs};
    constexpr std::array copy{Access::Common, Access::CopySource, Access::CopyDest};
    for (unsigned value = 0; value <= static_cast<unsigned>(Access::ShadingRateSource); ++value)
    {
        const auto access = static_cast<Access>(value);
        Check(QueueAccessCompatible(QueueHint::Direct, access), "Direct queue access policy changed.");
        Check(QueueAccessCompatible(QueueHint::Compute, access) ==
            (std::find(compute.begin(), compute.end(), access) != compute.end()),
            "Compute queue access policy changed.");
        Check(QueueAccessCompatible(QueueHint::Copy, access) ==
            (std::find(copy.begin(), copy.end(), access) != copy.end()),
            "Copy queue access policy changed.");
        Check(!QueueAccessCompatible(static_cast<QueueHint>(255), access),
            "Unknown queue accepted an access.");
    }
}

void BufferBounds()
{
    for (const auto range : {BufferRange{0, 0}, BufferRange{64, 1}, BufferRange{63, 2},
                             BufferRange{UINT64_MAX, 2}, BufferRange{1, UINT64_MAX}})
    {
        Graph g; g.Add(0, {Write(0, range)});
        Check(!g.Compile(), "reject empty, out-of-bounds and overflowing buffer range");
    }
    Graph g; g.resources[0].buffer_size = UINT64_MAX;
    g.Add(0, {Write(0, BufferRange{UINT64_MAX - 1, 1})});
    g.Add(1, {Read(1, BufferRange{UINT64_MAX - 1, 1})});
    Check(g.Compile(), "accept last valid byte without unsigned overflow");
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

void VersionAndIdentityValidation()
{
    Graph g; g.Add(0, {Write(0, BufferRange{0, 16})});
    g.Add(1, {Write(0, BufferRange{32, 16})});
    Check(!g.Compile(), "disjoint writes cannot branch one logical successor");
    g.passes[1].uses[0] = Write(2);
    Check(!g.Compile(), "missing predecessor rejected");
    g.passes[1].uses[0] = Read(2);
    Check(!g.Compile(), "nonexistent read version rejected");
    g.passes[1].uses[0] = Write(UINT32_MAX);
    Check(!g.Compile(), "wrapped successor rejected");
    g.passes.resize(1); g.passes[0].uses[0] = Write(0);
    g.resources[0].final_set = true; g.resources[0].final_version = {buffer, 0};
    Check(!g.Compile(), "stale final version rejected");
    g.resources[0].final_set = false;
    g.passes[0].uses.push_back(Read(0, BufferRange{32, 16}));
    Check(!g.Compile(), "one physical resource cannot have two uses in a pass");
    g.passes[0].uses.resize(1);
    g.passes[0].uses[0].predecessor.resource.kind = ResourceKind::Texture;
    Check(!g.Compile(), "wrong handle kind rejected");
    Graph aliases; aliases.resources[0].initialized = true;
    aliases.resources[0].imported_identity = 0x1234;
    aliases.resources.push_back(aliases.resources[0]);
    Check(!aliases.Compile(), "duplicate imports of same physical identity rejected");
    aliases.resources[1].imported_identity = 0x5678;
    Check(aliases.Compile(), "distinct physical imports accepted");
    Graph imported; imported.resources[0].initialized = true;
    imported.Add(2, {Read(0, BufferRange{0, 16})});
    imported.Add(0, {Write(0, BufferRange{32, 16})});
    imported.Add(1, {Write(1, BufferRange{0, 16})});
    Check(imported.Compile() && imported.Order({0, 2, 1}), "imported version zero has distant WAR protection");
}

void LargeImportAndPassUseValidation()
{
    constexpr std::uint32_t count = 2048;
    Graph graph;
    graph.resources.assign(count, {ResourceKind::Buffer, true, 64});
    graph.resources[0].name = "first imported buffer";
    for (std::uint32_t index = 2; index < count; ++index)
        graph.resources[index].imported_identity = std::uintptr_t{0x10000} + std::uintptr_t{index} * 4096;
    graph.Add(100, {});
    graph.Add(200, {});
    for (std::uint32_t index = 0; index < count; ++index)
    {
        graph.passes[0].uses.push_back(Read(0, {}, {index + 1, ResourceKind::Buffer}));
        graph.passes[1].uses.push_back(Read(0, {}, {count - index, ResourceKind::Buffer}));
    }
    const auto compile = [&] {
        std::vector<PassInfo> passes;
        for (const auto &pass : graph.passes) passes.push_back({pass.id, pass.queue, pass.callback, pass.uses});
        return hs::render_graph::Compile(graph.resources, passes, graph.schedule);
    };
    Check(compile().Succeeded(), "large unique imports and read sets compile; zero identities remain unspecified");
    graph.resources.back().imported_identity = graph.resources[512].imported_identity;
    auto result = compile();
    Check(!result && result.Message() == "RenderGraph imports one physical resource more than once; reuse its handle." &&
              graph.schedule.execution_order.empty(), "distant duplicate import preserves the diagnostic and clears the schedule");
    graph.resources.back().imported_identity += 1;
    Check(compile().Succeeded(), "changed frame import identities are revalidated after a prior failure");
    auto duplicate = Read(0, BufferRange{64, 1});
    duplicate.access = Access::CopySource;
    graph.passes[1].uses.push_back(duplicate);
    result = compile();
    Check(!result && result.Message() == "RenderGraph pass 200 declares resource 1 (first imported buffer) twice; accesses " +
              std::to_string(static_cast<unsigned>(Access::ComputeRead)) + " and " +
              std::to_string(static_cast<unsigned>(Access::CopySource)) + ".",
          "distant duplicate retains first-use access and takes precedence over invalid range");
    for (const auto handle : {ResourceHandle{}, ResourceHandle{UINT32_MAX, ResourceKind::Buffer}})
    {
        graph.passes[1].uses.back() = Read(0, {}, handle);
        result = compile();
        Check(!result && result.Message() == "RenderGraph names an invalid or nonexistent resource version.",
            "invalid handles fail before indexing the duplicate-use table");
    }
    graph.passes[1].uses.pop_back();
    Check(compile().Succeeded(), "per-pass use tracking resets between passes and compile invocations");
}

void ReadOnlyImports()
{
    Graph graph;
    graph.resources[0].initialized = true;
    graph.Add(20, {Read(0)});
    graph.Add(10, {Read(0, BufferRange{63, 1})});
    graph.passes[1].queue = QueueHint::Compute;
    graph.resources[0].final_set = true;
    graph.resources[0].final_version = {buffer, 0};
    Check(graph.Compile() && graph.Order({10, 20}) && graph.schedule.cross_queue_dependencies.empty(),
          "read-only imports need no version hazards and may export version zero");
    graph.passes[1].uses[0] = Read(1);
    Check(!graph.Compile(), "read-only import cannot reference an unwritten version");
    graph.passes[1].uses[0] = Read(0, BufferRange{63, 2});
    Check(!graph.Compile(), "read-only import still validates bounds");
    graph.passes[1].uses[0] = Read(0);
    graph.resources[0].initialized = false;
    Check(!graph.Compile(), "read-only resource must be initialized");
    graph.resources[0].initialized = true;
    graph.resources[0].final_version.ordinal = 1;
    Check(!graph.Compile(), "read-only export cannot reference an unwritten version");
    graph.Add(1, {Write(0)});
    Check(graph.Compile() && graph.Order({10, 20, 1}),
          "a later declared writer retains both version-zero read-before-write hazards");
}

void SparseVersionExtents()
{
    Graph sparse;
    sparse.resources = {{ResourceKind::Buffer, true, 64}, {ResourceKind::Buffer, true, 64},
                        {ResourceKind::Buffer, false, 64}};
    sparse.resources[1].final_set = true;
    sparse.resources[1].final_version = {{2, ResourceKind::Buffer}, 0};
    for (std::uint32_t index = 0; index < 128; ++index) sparse.Add(index, {});
    sparse.passes[0].uses.push_back(Read(0));
    Check(sparse.Compile(), "unused imported version zero may export while unused transient needs no version storage");
    sparse.resources[2].final_set = true;
    sparse.resources[2].final_version = {{3, ResourceKind::Buffer}, 0};
    Check(!sparse.Compile(), "unused transient export still requires fully initialized version zero");
    sparse.resources[2].final_set = false;
    sparse.passes.back().uses.push_back(Read(128));
    Check(!sparse.Compile(), "high read ordinal inside pass bound rejects missing writer across sparse holes");
    sparse.passes.back().uses[0] = Write(127);
    Check(!sparse.Compile(), "high write ordinal rejects nonexistent predecessor across sparse holes");
    sparse.passes.back().uses[0] = Read(129);
    Check(!sparse.Compile(), "ordinal beyond pass count is rejected before sparse chain growth");
    sparse.passes.back().uses.clear();
    Check(sparse.Compile(), "sparse chains are rebuilt after invalid high ordinals");

    Graph high;
    for (std::uint32_t ordinal = 0; ordinal < 64; ++ordinal)
        high.Add(ordinal, {Write(ordinal, BufferRange{0, 32})});
    high.resources[0].final_set = true;
    high.resources[0].final_version = {buffer, 64};
    Check(!high.Compile(), "high latest version cannot export an inherited initialization hole");
    high.passes.back().uses[0] = Write(63);
    Check(high.Compile(), "high contiguous version chain exports once the full range is initialized");
    high.resources[0].final_version.ordinal = 63;
    Check(!high.Compile(), "high stale final version remains rejected");
    high.resources[0].final_version.ordinal = 64;
    high.passes[31].uses.clear();
    Check(!high.Compile(), "interior missing writer remains invalid even with a fully initialized final range");
}

void TextureBounds()
{
    Graph g; g.resources[0] = {ResourceKind::Texture, true, 0, 4, 3, 2};
    g.Add(0, {Read(0, TextureSubresourceRange{3, 1, 2, 1, 1, 1}, texture)});
    Check(g.Compile(), "last mip array and plane accepted");
    for (const auto range : {TextureSubresourceRange{0, 0, 0, 1, 0, 1},
                             TextureSubresourceRange{3, 2, 0, 1, 0, 1},
                             TextureSubresourceRange{0, 1, 2, 2, 0, 1},
                             TextureSubresourceRange{0, 1, 0, 1, 1, 2},
                             TextureSubresourceRange{UINT32_MAX, 2, 0, 1, 0, 1},
                             TextureSubresourceRange{0, 1, UINT32_MAX, 2, 0, 1},
                             TextureSubresourceRange{0, 1, 0, 1, UINT32_MAX, 2}})
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
}

void RandomizedVersionReplay()
{
    // Compare the actual compiled schedule to byte snapshots of each logical
    // version, including undefined bytes. This checks distant hazards after
    // randomized pass IDs and registration order, independently of edge logic.
    std::mt19937 random(0x68734772);
    for (unsigned trial = 0; trial < 1000; ++trial)
    {
        Graph g;
        g.resources[0].buffer_size = 8;
        g.resources[0].initialized = (random() & 1) != 0;
        const auto writer_count = 1 + random() % 6;
        std::vector<std::array<int, 8>> snapshots(writer_count + 1);
        snapshots[0].fill(g.resources[0].initialized ? 0 : -1);
        std::vector<std::uint32_t> ids(writer_count + 8);
        for (std::uint32_t i = 0; i < ids.size(); ++i) ids[i] = i;
        std::shuffle(ids.begin(), ids.end(), random);
        bool valid = true;
        for (std::uint32_t ordinal = 1; ordinal <= writer_count; ++ordinal)
        {
            const auto first = random() % 8, count = 1 + random() % (8 - first);
            snapshots[ordinal] = snapshots[ordinal - 1];
            const bool load = (random() % 4) == 0;
            for (auto byte = first; byte < first + count; ++byte)
            {
                if (load && snapshots[ordinal - 1][byte] < 0) valid = false;
                snapshots[ordinal][byte] = static_cast<int>(ordinal);
            }
            g.Add(ids[ordinal - 1], {Write(ordinal - 1, BufferRange{first, count}, load)});
        }
        for (std::uint32_t reader = 0; reader < 8; ++reader)
        {
            const auto ordinal = random() % (writer_count + 1);
            const auto first = random() % 8, count = 1 + random() % (8 - first);
            for (auto byte = first; byte < first + count; ++byte)
                if (snapshots[ordinal][byte] < 0) valid = false;
            g.Add(ids[writer_count + reader], {Read(ordinal, BufferRange{first, count})});
        }
        std::shuffle(g.passes.begin(), g.passes.end(), random);
        for (auto &pass : g.passes)
        {
            auto &use = pass.uses[0];
            pass.queue = static_cast<QueueHint>(random() % 3);
            if (pass.queue == QueueHint::Copy)
            {
                if (use.writes && use.reads) pass.queue = QueueHint::Compute;
                else use.access = use.writes ? Access::CopyDest : Access::CopySource;
            }
        }
        Check(g.Compile() == valid, "randomized compile agrees with initialized-version snapshots");
        if (!valid) continue;
        const auto count = g.passes.size();
        std::vector<std::vector<bool>> reachable(count, std::vector<bool>(count));
        for (const auto &queue : g.schedule.queue_orders)
            for (std::size_t i = 1; i < queue.size(); ++i) reachable[queue[i - 1]][queue[i]] = true;
        for (const auto &dependency : g.schedule.cross_queue_dependencies)
            reachable[dependency.producer][dependency.consumer] = true;
        for (std::size_t via = 0; via < count; ++via)
            for (std::size_t from = 0; from < count; ++from)
                for (std::size_t to = 0; to < count; ++to)
                    reachable[from][to] = reachable[from][to] || (reachable[from][via] && reachable[via][to]);
        // Independently require every old RAW/WAR edge, including distant
        // partial overlaps, to remain ordered by actual queue execution/waits.
        for (std::size_t reader = 0; reader < count; ++reader)
            for (std::size_t writer = 0; writer < count; ++writer)
            {
                if (reader == writer) continue;
                const auto &a = g.passes[reader].uses[0], &b = g.passes[writer].uses[0];
                if (!b.writes) continue;
                if (a.writes && a.successor.ordinal < b.successor.ordinal)
                    Check(reachable[reader][writer], "queue schedule preserves physical writer chain");
                const auto ar = std::get<BufferRange>(a.range), br = std::get<BufferRange>(b.range);
                if (!a.reads || ar.offset >= br.offset + br.size || br.offset >= ar.offset + ar.size) continue;
                Check(b.successor.ordinal <= a.predecessor.ordinal ? reachable[writer][reader] : reachable[reader][writer],
                      "queue schedule preserves every overlapping RAW/WAR hazard");
            }
        auto physical = snapshots[0];
        for (const auto index : g.schedule.execution_order)
        {
            const auto &use = g.passes[index].uses[0];
            const auto range = std::get<BufferRange>(use.range);
            for (auto byte = range.offset; byte < range.offset + range.size; ++byte)
            {
                if (use.reads)
                    Check(physical[byte] == snapshots[use.predecessor.ordinal][byte],
                          "compiled order preserves exact old-version bytes");
                if (use.writes) physical[byte] = static_cast<int>(use.successor.ordinal);
            }
        }
        Check(physical == snapshots.back(), "compiled physical writes preserve final version");
    }
}

void PassValidation()
{
    Graph g; g.Add(0, {Write(0)});
    g.passes[0].queue = QueueHint::Compute;
    Check(g.Compile(), "compute queue accepted by logical compiler");
    g.passes[0].queue = QueueHint::Copy;
    g.passes[0].uses[0].access = Access::CopyDest;
    Check(g.Compile(), "copy queue accepted by logical compiler");
    g.passes[0].queue = static_cast<QueueHint>(255);
    Check(!g.Compile(), "invalid queue enum rejected");
    g.passes[0].queue = QueueHint::Direct; g.passes[0].callback = false;
    Check(!g.Compile(), "missing callback rejected");
    g.passes[0].callback = true; g.Add(0, {});
    Check(!g.Compile(), "duplicate pass ID rejected");
    g.passes.resize(1); g.passes[0].uses[0].access = Access::ComputeRead;
    Check(!g.Compile(), "write access mismatch rejected");

    Graph queue_access; queue_access.resources[0].initialized = true;
    auto shader_read = Read(0); shader_read.access = Access::ShaderRead;
    queue_access.passes.push_back({0, {shader_read}, QueueHint::Compute, true});
    Check(!queue_access.Compile(), "compute queue rejects graphics shader-read access");
    queue_access.passes[0].uses[0].access = Access::ComputeRead;
    Check(queue_access.Compile(), "compute queue accepts compute read access");
    queue_access.passes[0].queue = QueueHint::Copy;
    Check(!queue_access.Compile(), "copy queue rejects compute read access");
    queue_access.passes[0].uses[0].access = Access::CopySource;
    Check(queue_access.Compile(), "copy queue accepts copy source access");

    Graph depth_texture;
    depth_texture.resources[0] = {ResourceKind::Texture, true, 0, 1, 1, 1};
    auto depth_read = Read(0, {}, texture);
    depth_read.access = Access::DepthShaderRead;
    depth_texture.passes.push_back({0, {depth_read}, QueueHint::Direct, true});
    Check(depth_texture.Compile(), "direct queue accepts combined depth shader read on a texture");
    depth_texture.passes[0].queue = QueueHint::Compute;
    Check(!depth_texture.Compile(), "compute queue rejects combined depth shader read");

    Graph depth_buffer;
    auto depth_buffer_read = Read(0);
    depth_buffer_read.access = Access::DepthShaderRead;
    depth_buffer.passes.push_back({0, {depth_buffer_read}, QueueHint::Direct, true});
    Check(!depth_buffer.Compile(), "combined depth shader read rejects a buffer");
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
        std::vector<ResourceInfo> resources{make_resource(128), make_resource(64)};
        std::vector<ResourceUse> first_uses{write(1)}, second_uses{write(2)};
        std::vector<PassInfo> passes{{0, QueueHint::Direct, true, first_uses},
                                     {1, QueueHint::Direct, true, second_uses}};
        CompiledSchedule schedule;
        Check(Compile(resources, passes, schedule).Succeeded(), "serial alias graph compiles");
        AliasPlan plan;
        Check(PlanTransientAliases(resources, passes, schedule, plan).Succeeded(),
              "serial alias graph plans");
        Check(plan.groups.size() == 1 && plan.alias_activation_count == 1 &&
                  plan.transient_backing_bytes < plan.transient_bytes_without_aliasing,
              "serial resources share one best-fit backing block");
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

void GeometryBufferContract()
{
    for (const auto geometry_access : {Access::IndexBuffer, Access::VertexShaderRead})
    {
        Graph graph;
        auto copy = Write(0); copy.access = Access::CopyDest;
        auto indices = Read(1); indices.access = geometry_access;
        graph.passes.push_back({10, {copy}, QueueHint::Copy, true});
        graph.passes.push_back({20, {indices}, QueueHint::Direct, true});
        Check(graph.Compile() && graph.Order({10, 20}) &&
                  graph.schedule.cross_queue_dependencies.size() == 1,
              "geometry input waits for its copied buffer on the graphics queue");
        for (const auto queue : {QueueHint::Compute, QueueHint::Copy})
        {
            graph.passes[1].queue = queue;
            Check(!graph.Compile(), "geometry input is rejected on non-graphics queues");
        }
        Graph texture_graph;
        texture_graph.resources[0] = {ResourceKind::Texture, true, 0, 1, 1, 1};
        auto texture_indices = Read(0, {}, texture); texture_indices.access = geometry_access;
        texture_graph.Add(0, {texture_indices});
        Check(!texture_graph.Compile(), "geometry input rejects a texture resource");
    }
}

void MaterialTextureReadContract()
{
    Graph graph;
    graph.resources[0] = {ResourceKind::Texture, true, 0, 1, 1, 1};
    auto read = Read(0, {}, texture);
    read.access = Access::AllShaderRead;
    graph.Add(10, {read});
    Check(graph.Compile(), "combined material shader read is valid on Direct");
    for (const auto queue : {QueueHint::Compute, QueueHint::Copy})
    {
        graph.passes[0].queue = queue;
        Check(!graph.Compile(), "combined shader read transition requires Direct");
    }
}

struct ExplicitDag
{
    std::vector<std::vector<std::uint32_t>> dependencies;
    std::vector<PassInfo> passes;
    ExplicitDag(std::uint32_t count, bool dense, std::uint32_t seed) : dependencies(count)
    {
        std::mt19937 random(seed);
        std::vector<std::uint32_t> ids(count), registration(count);
        std::iota(ids.begin(), ids.end(), 1);
        std::iota(registration.begin(), registration.end(), 0);
        std::shuffle(ids.begin(), ids.end(), random);
        std::shuffle(registration.begin(), registration.end(), random);
        passes.resize(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const auto rank = registration[index];
            auto &parents = dependencies[index];
            if (dense)
            {
                for (std::uint32_t before = 0; before < rank; ++before)
                    if (random() % 4 == 0) parents.push_back(ids[before]);
            }
            else if (rank)
                for (unsigned edge = 0; edge < 4; ++edge) parents.push_back(ids[random() % rank]);
            if (!parents.empty()) parents.push_back(parents.front());
            passes[index] = {ids[rank], static_cast<QueueHint>(rank % 3), true, {}, parents};
        }
    }
};

void ExplicitDagOrdering()
{
    {
        CompiledSchedule schedule;
        std::array<std::uint32_t, 1> parent{UINT32_MAX};
        std::array<PassInfo, 2> passes{{
            {0, QueueHint::Direct, true, {}, parent},
            {UINT32_MAX, QueueHint::Copy, true, {}, {}}}};
        Check(Compile({}, passes, schedule).Succeeded() && schedule.execution_order.front() == 1,
              "full-width IDs and forward registration dependencies are supported");
        parent[0] = 1;
        Check(!Compile({}, passes, schedule), "absent explicit predecessor rejected");
        parent[0] = 0;
        Check(!Compile({}, passes, schedule), "self explicit predecessor rejected");
        parent[0] = UINT32_MAX;
        passes[1].id = 0;
        Check(!Compile({}, passes, schedule), "duplicate explicit pass ID rejected");
    }
    for (bool dense : {false, true})
        for (std::uint32_t seed = 0; seed < 24; ++seed)
        {
            ExplicitDag graph(1 + seed * 7, dense, seed);
            CompiledSchedule schedule;
            Check(Compile({}, graph.passes, schedule).Succeeded(), "explicit DAG must compile");
            std::vector<bool> visited(graph.passes.size());
            for (const auto actual : schedule.execution_order)
            {
                std::uint32_t expected = UINT32_MAX;
                for (std::uint32_t candidate = 0; candidate < graph.passes.size(); ++candidate)
                {
                    if (visited[candidate]) continue;
                    const bool ready = std::all_of(graph.dependencies[candidate].begin(), graph.dependencies[candidate].end(),
                        [&](auto id) {
                            const auto parent = std::find_if(graph.passes.begin(), graph.passes.end(),
                                [&](const PassInfo &pass) { return pass.id == id; });
                            return visited[static_cast<std::size_t>(parent - graph.passes.begin())];
                        });
                    if (ready && (expected == UINT32_MAX || graph.passes[candidate].id < graph.passes[expected].id))
                        expected = candidate;
                }
                Check(actual == expected, "DAG order must select the smallest ready pass ID");
                visited[actual] = true;
            }
            Check(std::all_of(visited.begin(), visited.end(), [](bool value) { return value; }), "every DAG pass executes once");
        }
}

void BenchmarkExplicitDags()
{
    for (bool dense : {false, true})
        for (const std::uint32_t count : {256u, 1024u})
        {
            ExplicitDag graph(count, dense, 4099);
            CompiledSchedule schedule;
            const auto begin = std::chrono::steady_clock::now();
            std::uint64_t digest = 14695981039346656037ull;
            const auto add = [&](std::uint32_t value) { digest = (digest ^ value) * 1099511628211ull; };
            for (unsigned repeat = 0; repeat < 20; ++repeat)
            {
                Check(Compile({}, graph.passes, schedule).Succeeded(), "benchmark DAG must compile");
                for (auto index : schedule.execution_order) add(index);
                for (const auto &queue : schedule.queue_orders)
                {
                    add(static_cast<std::uint32_t>(queue.size()));
                    for (auto index : queue) add(index);
                }
                for (const auto &edge : schedule.cross_queue_dependencies)
                {
                    add(edge.producer); add(edge.consumer);
                    add(static_cast<std::uint32_t>(edge.producer_queue));
                    add(static_cast<std::uint32_t>(edge.consumer_queue));
                }
            }
            const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            std::printf("graph_bench passes=%u dense=%u iterations=20 ms=%.3f schedule_digest=%llu\n",
                count, unsigned(dense), milliseconds, static_cast<unsigned long long>(digest));
        }
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark-explicit-dags")
        {
            BenchmarkExplicitDags();
            return 0;
        }
        ExplicitDagOrdering();
        BufferBounds(); Initialization(); Dependencies(); VersionAndIdentityValidation(); LargeImportAndPassUseValidation(); ReadOnlyImports(); SparseVersionExtents(); TextureBounds();
        QueueAccessPolicy(); PassValidation(); MultiQueueSchedule(); CompletionSetContract(); PhysicalQueueOwnership(); TransientAliasPlanner(); GeometryBufferContract(); MaterialTextureReadContract(); RandomizedVersionReplay();
        std::puts("Render graph CPU range/version validation passed.");
        return 0;
    }
    catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
