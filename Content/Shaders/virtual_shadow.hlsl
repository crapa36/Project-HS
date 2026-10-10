cbuffer VirtualShadowConstants : register(b0, space21)
{
    row_major float4x4 InverseViewProjection;
    float4 LightRight, LightUp, LightForward;
    float4 Bank, Depth, ViewOrigin;
    float4 Clipmaps[3];
    int4 Windows[3];
    uint4 Counts, Limits, Identity, Frame, Options;
};
cbuffer ClearConstants : register(b1, space21) { uint ClearEntry; };
struct PageEntry
{
    int x, y; uint level, physical;
    uint epoch, flags, reserved0, reserved1;
};
struct Caster
{
    float4 current_bounds, previous_bounds;
    uint4 draw; // instance, generation, source ref, canonical geometry record
    uint4 indexed; // group, index count, first index, signed base vertex
    uint4 properties; // flags, meshlet count, frontend, next changed static caster
};
struct Receiver { float4 minimum, maximum; };
struct DrawRecord
{
    uint geometry_record, source_ref, instance_index, instance_generation;
    uint page_entry, layer, draw_group, reserved;
};
Texture2D<float> ReceiverDepth : register(t0, space21);
Texture2D<float2> ReceiverNormal : register(t1, space21);
StructuredBuffer<Caster> Casters : register(t2, space21);
StructuredBuffer<Receiver> Receivers : register(t3, space21);
StructuredBuffer<PageEntry> ClearPageTable : register(t4, space21);
RWStructuredBuffer<PageEntry> PageTable : register(u0, space21);
RWStructuredBuffer<PageEntry> Residents : register(u1, space21);
RWByteAddressBuffer Requests : register(u2, space21);
RWStructuredBuffer<DrawRecord> DrawRecords : register(u3, space21);
RWByteAddressBuffer IndexedCommands : register(u4, space21);
RWByteAddressBuffer MeshCommands : register(u5, space21);
RWByteAddressBuffer CommandCounts : register(u6, space21);
RWByteAddressBuffer ClearCommands : register(u7, space21);
RWByteAddressBuffer PageDrawCounts : register(u8, space21);

// flags: requested1, static redraw2, dynamic redraw4, overflow8, published16,
// resident static valid32. A table entry is never published before both draws.
float3 LightPosition(float3 world)
{
    return float3(dot(world, LightRight.xyz), dot(world, LightUp.xyz), dot(world, LightForward.xyz));
}
uint EntryForKey(int2 key, uint level)
{
    const int2 local = key - Windows[level].xy;
    if (any(local < 0) || any(local >= Windows[level].z)) return 0xffffffffu;
    return uint(Windows[level].w + local.y * Windows[level].z + local.x);
}
bool Requested(uint entry)
{
    return entry != 0xffffffffu && (Requests.Load((entry >> 5u) * 4u) & (1u << (entry & 31u))) != 0u;
}
void Request(uint entry)
{
    if (entry == 0xffffffffu) return;
    uint ignored;
    Requests.InterlockedOr((entry >> 5u) * 4u, 1u << (entry & 31u), ignored);
}
bool SphereOverlaps(float4 sphere, PageEntry page)
{
    const float3 light = LightPosition(sphere.xyz);
    const float guard = Clipmaps[page.level].y * 2.0;
    const float2 lower = float2(page.x, page.y) * Clipmaps[page.level].x - guard;
    const float2 upper = lower + Clipmaps[page.level].x + 2.0 * guard;
    return sphere.w >= 0.0 && all(light.xy + sphere.w >= lower) &&
        all(light.xy - sphere.w <= upper) && light.z + sphere.w >= Depth.x &&
        light.z - sphere.w <= Depth.x + Depth.w;
}
uint PageDrawCountOffset(uint entry, uint segment)
{
    return (entry * Frame.w + segment) * 4u;
}
[numthreads(64, 1, 1)]
void InitCS(uint3 tid : SV_DispatchThreadID)
{
    const uint id = tid.x;
    if (id < Frame.z) Requests.Store(id * 4u, 0u);
    if (id < Frame.w + 2u) CommandCounts.Store(id * 4u, 0u);
    if (id < Counts.z)
    {
        // One page thread clears its complete row without enlarging dispatch X.
        for (uint segment = 0u; segment < Frame.w; ++segment)
            PageDrawCounts.Store(PageDrawCountOffset(id, segment), 0u);
        PageEntry page = (PageEntry)0;
        page.physical = 0xffffffffu; page.epoch = Identity.w;
        [unroll] for (uint level = 0; level < 3; ++level)
            if (id >= uint(Windows[level].w) && id < uint(Windows[level].w + Windows[level].z * Windows[level].z))
            {
                const uint local = id - uint(Windows[level].w);
                page.x = Windows[level].x + int(local % uint(Windows[level].z));
                page.y = Windows[level].y + int(local / uint(Windows[level].z));
                page.level = level;
            }
        PageTable[id] = page;
    }
    if (Options.x != 0u && id < Counts.w)
    {
        PageEntry resident = (PageEntry)0;
        resident.physical = id; resident.epoch = 0u;
        Residents[id] = resident;
    }
}
[numthreads(8, 8, 1)]
void FeedbackCS(uint3 tid : SV_DispatchThreadID)
{
    if (Options.y == 0u) return;
    if (tid.x < Limits.z && tid.y < Limits.w)
    {
        const float depth = ReceiverDepth.Load(int3(tid.xy, 0));
        const float2 normal = ReceiverNormal.Load(int3(tid.xy, 0));
        if (depth > 0.0 && isfinite(depth) && all(isfinite(normal)))
        {
            const float2 uv = (float2(tid.xy) + 0.5) / float2(Limits.zw);
            float4 position = mul(float4(uv * float2(2, -2) + float2(-1, 1), depth, 1), InverseViewProjection);
            if (abs(position.w) > 1e-8 && all(isfinite(position)))
            {
                const float3 light = LightPosition(position.xyz / position.w + ViewOrigin.xyz);
                [unroll] for (uint level = 0; level < 3; ++level)
                {
                    const uint entry = EntryForKey(int2(floor(light.xy / Clipmaps[level].x)), level);
                    if (entry != 0xffffffffu)
                    {
                        Request(entry);
                        const float2 cell = light.xy / Clipmaps[level].x - float2(Windows[level].xy);
                        const float edge = min(min(cell.x, cell.y), min(Windows[level].z - cell.x, Windows[level].z - cell.y));
                        if (level < 2u && edge < 1.0)
                            Request(EntryForKey(int2(floor(light.xy / Clipmaps[level + 1u].x)), level + 1u));
                        break;
                    }
                }
            }
        }
    }
    // One GPU thread per transparent/volumetric AABB, independent of opaque HZB.
    const uint receiver = tid.y * ((Limits.z + 7u) & ~7u) + tid.x;
    if (receiver < Counts.y)
    {
        const Receiver box = Receivers[receiver];
        float2 lower = 3.402823e38, upper = -3.402823e38;
        [unroll] for (uint corner = 0; corner < 8; ++corner)
        {
            const float3 p = float3((corner & 1u) ? box.maximum.x : box.minimum.x,
                (corner & 2u) ? box.maximum.y : box.minimum.y,
                (corner & 4u) ? box.maximum.z : box.minimum.z);
            const float2 projected = LightPosition(p).xy;
            lower = min(lower, projected); upper = max(upper, projected);
        }
        // Request every overlapping level; transparent fragments and froxels can
        // later select any valid resident level at their own world position.
        [unroll] for (uint level = 0; level < 3; ++level)
        {
            const int2 first = max(int2(floor(lower / Clipmaps[level].x)), Windows[level].xy);
            const int2 last = min(int2(floor(upper / Clipmaps[level].x)), Windows[level].xy + Windows[level].z - 1);
            for (int y = first.y; y <= last.y; ++y)
                for (int x = first.x; x <= last.x; ++x) Request(EntryForKey(int2(x, y), level));
        }
    }
}
bool KeyEqual(PageEntry a, PageEntry b)
{
    return a.epoch == b.epoch && a.level == b.level && a.x == b.x && a.y == b.y;
}
[numthreads(64, 1, 1)]
void ResidentCS(uint3 tid : SV_DispatchThreadID)
{
    // Phase zero invalidates physical pages in parallel and reconnects their
    // absolute keys to the new window. Unique physical ownership means the
    // frame-table mapping has exactly one writer for each resident key.
    if (ClearEntry == 0u)
    {
        const uint p = tid.x;
        if (p >= Counts.w) return;
        PageEntry resident = Residents[p];
        if (resident.epoch != Identity.w) return;
        if ((resident.flags & 32u) != 0u) for (uint c = Limits.x; c < Counts.x; c = Casters[c].properties.w)
        {
            const uint f = Casters[c].properties.x;
            if ((f & 16u) != 0u && (((f & 12u) != 0u) &&
                (((f & 1u) != 0u && SphereOverlaps(Casters[c].current_bounds, resident)) ||
                 ((f & 2u) != 0u && SphereOverlaps(Casters[c].previous_bounds, resident)))))
            {
                resident.flags &= ~32u; break;
            }
        }
        Residents[p] = resident;
        const uint entry = EntryForKey(int2(resident.x, resident.y), resident.level);
        if (entry != 0xffffffffu) PageTable[entry].physical = p;
        return;
    }
    // The UAV boundary between phases completes invalidation/mapping. A single
    // monotonic free-slot scan allocates without races in O(entries+physical).
    if (tid.x != 0u) return;
    if (Options.y == 0u) return;
    uint free_cursor = 0u;
    for (uint entry = 0; entry < Counts.z; ++entry)
    {
        if (!Requested(entry)) continue;
        PageEntry page = PageTable[entry];
        uint physical = page.physical;
        if (physical == 0xffffffffu)
        {
            for (; free_cursor < Counts.w; ++free_cursor)
            {
                const uint p = free_cursor;
                const PageEntry old = Residents[p];
                // Do not evict a resident requested anywhere in this frame.
                const uint old_entry = old.epoch == Identity.w
                    ? EntryForKey(int2(old.x, old.y), old.level) : 0xffffffffu;
                if (!Requested(old_entry)) { physical = p; ++free_cursor; break; }
            }
            if (physical == 0xffffffffu) continue; // budget: remains invalid
            PageEntry resident = page; resident.physical = physical; resident.flags = 0u;
            Residents[physical] = resident;
        }
        page.physical = physical;
        page.flags = 1u | 4u;
        if ((Residents[physical].flags & 32u) == 0u) page.flags |= 2u;
        PageTable[entry] = page;
        for (uint layer = 0; layer < 2; ++layer)
        {
            if (layer == 0u && (page.flags & 2u) == 0u) continue;
            const uint count_offset = (Frame.w + layer) * 4u;
            const uint slot = CommandCounts.Load(count_offset);
            CommandCounts.Store(count_offset, slot + 1u);
            const uint offset = (layer * Counts.w + slot) * 20u;
            ClearCommands.Store(offset, entry);
            ClearCommands.Store4(offset + 4u, uint4(3u, 1u, 0u, 0u));
        }
    }
}
bool DrawCandidate(PageEntry page, Caster caster, out uint layer, out uint segment)
{
    layer = (caster.properties.x & 4u) != 0u ? 0u : 1u;
    segment = layer * (Frame.w / 2u) + caster.indexed.x;
    return page.physical != 0xffffffffu && (page.flags & 1u) != 0u &&
        (caster.properties.x & 1u) != 0u && SphereOverlaps(caster.current_bounds, page) &&
        (layer != 0u || (page.flags & 2u) != 0u);
}
[numthreads(64, 1, 1)]
void CullCS(uint3 tid : SV_DispatchThreadID)
{
    // The existing b1 root constant selects count0, admission1, scatter2.
    if (ClearEntry == 1u)
    {
        const uint segment = tid.x;
        if (segment >= Frame.w) return;
        uint remaining = Options.z;
        for (uint entry = 0u; entry < Counts.z; ++entry)
        {
            const uint count = PageDrawCounts.Load(PageDrawCountOffset(entry, segment));
            if (count > remaining)
            {
                uint ignored;
                InterlockedOr(PageTable[entry].flags, 8u, ignored);
            }
            else remaining -= count;
        }
        // Never read peer-written flags while reserving: every segment's page
        // decisions depend only on its counts and ascending entry order.
        return;
    }
    if (tid.x >= Counts.x || tid.y >= Counts.z) return;
    const uint entry = tid.y, caster_index = tid.x;
    const PageEntry page = PageTable[entry];
    const Caster caster = Casters[caster_index];
    uint layer, segment;
    if (!DrawCandidate(page, caster, layer, segment)) return;
    if (ClearEntry == 0u)
    {
        uint ignored;
        PageDrawCounts.InterlockedAdd(PageDrawCountOffset(entry, segment), 1u, ignored);
        return;
    }
    // Admission completes globally before scatter. Rejected pages emit no
    // commands, so every remaining page fits completely in every segment.
    if ((page.flags & 8u) != 0u) return;
    uint slot;
    CommandCounts.InterlockedAdd(segment * 4u, 1u, slot);
    if (slot >= Options.z)
    {
        // Unreachable when the shared candidate predicate matches the counts.
        uint ignored;
        InterlockedOr(PageTable[entry].flags, 8u, ignored);
        return;
    }
    const uint record_index = segment * Options.z + slot;
    DrawRecord record;
    record.geometry_record = caster.draw.w; record.source_ref = caster.draw.z;
    record.instance_index = caster.draw.x; record.instance_generation = caster.draw.y;
    record.page_entry = entry; record.layer = layer; record.draw_group = caster.indexed.x; record.reserved = 0;
    DrawRecords[record_index] = record;
    if (caster.properties.z == 0u)
    {
        const uint offset = record_index * 24u;
        IndexedCommands.Store4(offset, uint4(0x80000000u | record_index, caster.indexed.y, 1u, caster.indexed.z));
        IndexedCommands.Store2(offset + 16u, uint2(caster.indexed.w, 0u));
    }
    else MeshCommands.Store4(record_index * 16u,
        uint4(0x80000000u | record_index, caster.properties.y, 1u, 1u));
}
[numthreads(64, 1, 1)]
void PublishCS(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= Counts.z) return;
    PageEntry page = PageTable[tid.x];
    if (page.physical == 0xffffffffu || (page.flags & 9u) != 1u) return;
    // Both bank passes precede publication through graph dependencies. An
    // overflowed static page never upgrades its persistent cache validity.
    page.flags |= 16u;
    PageTable[tid.x] = page;
    PageEntry resident = Residents[page.physical];
    resident.flags |= 32u;
    Residents[page.physical] = resident;
}
struct ClearOutput { float4 position : SV_Position; float4 clip_distance : SV_ClipDistance0; };
ClearOutput ClearVS(uint vertex : SV_VertexID)
{
    const PageEntry page = ClearPageTable[ClearEntry];
    const float2 corner = vertex == 0u ? float2(0, 0) : vertex == 1u ? float2(2, 0) : float2(0, 2);
    const float2 origin = float2(page.physical % uint(Bank.z), page.physical / uint(Bank.z)) * 132.0;
    const float2 pixel = origin + corner * 132.0;
    ClearOutput output;
    output.position = float4(pixel / Bank.xy * float2(2, -2) + float2(-1, 1), 0, 1);
    output.clip_distance = float4(corner, 1.0 - corner);
    return output;
}
float ClearPS() : SV_Depth { return 0.0; }
