// Scene-object SHL1 selection. Geometry storage and native records belong to
// the existing renderer page stream; this service owns only cut metadata.
struct HlodView
{
    float4 OriginNear, Forward;
    float4 Planes[6];
    float4 ErrorPolicy;
};
cbuffer HlodConstants : register(b0, space25)
{
    uint4 Counts; // groups, members, proxies, static-slot capacity
    uint4 Limits; // pages, buckets, command capacity, enabled
    uint4 Identity; // content generation, rendered-frame lo/hi, history valid
    uint4 Options; // active view bits, maximum ancestor steps,0,0
    HlodView Views[4];
};
struct HlodGroup
{
    uint Parent, FirstMember, MemberCount, FirstProxy;
    uint ProxyCount, FirstChild, ChildCount, AncestorSteps;
    float4 Sphere, Error;
};
struct HlodMember
{
    uint StableLow, StableHigh, LeafGroup, InstanceSlot;
    uint InstanceGeneration, ContentGeneration, ObjectLow, ObjectHigh;
};
struct HlodProxy
{
    uint GroupIndex, LogicalPage, FirstIndex, IndexCount;
    uint FirstMeshlet, MeshletCount, MeshSemantic, AtlasSelector;
    uint Tint, Variant, GeometryFlags, Reserved;
};
struct ProxyBinding
{
    uint LogicalPage, PhysicalPage, PageVersion, ContentGeneration;
    uint RecordId, CommandOrdinal, Bucket, Flags;
    uint InstanceSlot, InstanceGeneration, VisibleRef, Reserved;
};
struct HlodBucket { uint FirstCommand, CommandCount, MaterialWord, Reserved; };
struct StaticRef { uint InstanceSlot, MeshFlags; float LodThreshold; uint InstanceGeneration; };
StructuredBuffer<HlodGroup> Groups : register(t0, space25);
StructuredBuffer<HlodMember> Members : register(t1, space25);
StructuredBuffer<HlodProxy> Proxies : register(t2, space25);
StructuredBuffer<ProxyBinding> Bindings : register(t3, space25);
StructuredBuffer<HlodBucket> Buckets : register(t4, space25);
StructuredBuffer<uint4> PreviousGroups : register(t5, space25);
StructuredBuffer<uint4> PreviousLeaves : register(t6, space25);
StructuredBuffer<ProxyBinding> PreviousBindings : register(t7, space25);
ByteAddressBuffer StaticScene : register(t8, space25);
ByteAddressBuffer GeometryRecords : register(t9, space25);
StructuredBuffer<StaticRef> ProxyRefs : register(t10, space25);
RWStructuredBuffer<uint4> GroupChoice : register(u0, space25);
RWStructuredBuffer<uint4> LeafMasks : register(u1, space25);
RWStructuredBuffer<uint4> ProxySelection : register(u2, space25);
RWByteAddressBuffer IndexedCommands : register(u3, space25);
RWByteAddressBuffer MeshCommands : register(u4, space25);
RWByteAddressBuffer CommandCounts : register(u5, space25);
RWStructuredBuffer<uint> PageDemand : register(u6, space25);
RWByteAddressBuffer Status : register(u7, space25);
static const uint Invalid = 0xffffffffu;
static const uint FaultIdentity = 1u, FaultTables = 2u, FaultRecord = 4u, FaultCapacity = 8u;
static const uint GeometrySceneRefs = 1u, GeometryMesh = 2u, GeometryStatic = 4u;

uint Thread(uint3 id) { return id.x + id.y * (65535u * 64u); }
bool Range(uint first, uint count, uint size) { return first <= size && count <= size - first; }
void Fault(uint bits) { uint ignored; Status.InterlockedOr(0u, bits, ignored); }
bool SceneGeneration(uint slot, out uint generation)
{
    generation = 0u;
    uint size; StaticScene.GetDimensions(size);
    if (size < 64u) return false;
    const uint4 h0 = StaticScene.Load4(0u), h1 = StaticScene.Load4(16u), h2 = StaticScene.Load4(32u);
    if (h0.x != 0x37475348u || h0.y != 1u || slot >= h0.w || h2.z != 32u ||
        h1.x > size || slot >= (size - h1.x) / 32u) return false;
    generation = StaticScene.Load(h1.x + slot * h2.z + 20u);
    return generation != 0u;
}
bool Dimensions()
{
    uint n, stride; bool valid = true;
    Groups.GetDimensions(n, stride); valid = valid && n >= Counts.x && stride == 64u;
    Members.GetDimensions(n, stride); valid = valid && n >= Counts.y && stride == 32u;
    Proxies.GetDimensions(n, stride); valid = valid && n >= Counts.z && stride == 48u;
    Bindings.GetDimensions(n, stride); valid = valid && n >= Counts.z && stride == 48u;
    Buckets.GetDimensions(n, stride); valid = valid && n >= Limits.y && stride == 16u;
    GroupChoice.GetDimensions(n, stride); valid = valid && n >= Counts.x && stride == 16u;
    LeafMasks.GetDimensions(n, stride); valid = valid && n >= Counts.w && stride == 16u;
    ProxySelection.GetDimensions(n, stride); valid = valid && n >= Counts.z && stride == 16u;
    PageDemand.GetDimensions(n, stride); valid = valid && n >= Limits.x && stride == 4u;
    IndexedCommands.GetDimensions(n); valid = valid && Limits.z <= n / 96u;
    MeshCommands.GetDimensions(n); valid = valid && Limits.z <= n / 64u;
    CommandCounts.GetDimensions(n); valid = valid && Limits.y <= n / 16u;
    Status.GetDimensions(n); valid = valid && n >= 64u;
    if (Identity.w != 0u)
    {
        PreviousGroups.GetDimensions(n, stride); valid = valid && n >= Counts.x;
        PreviousLeaves.GetDimensions(n, stride); valid = valid && n >= Counts.w;
        PreviousBindings.GetDimensions(n, stride); valid = valid && n >= Counts.z;
    }
    valid = valid && Identity.x != 0u && (Identity.y != 0u || Identity.z != 0u) &&
        (Options.x & ~15u) == 0u && Options.y != 0u && Options.z == 0u && Options.w == 0u &&
        Limits.z == Counts.z;
    if (!valid) Fault(FaultTables);
    return valid;
}
bool SameBinding(uint proxy)
{
    if (Identity.w == 0u) return false;
    const ProxyBinding a = Bindings[proxy], b = PreviousBindings[proxy];
    return a.LogicalPage == b.LogicalPage && a.PhysicalPage == b.PhysicalPage &&
        a.PageVersion == b.PageVersion && a.ContentGeneration == b.ContentGeneration &&
        a.RecordId == b.RecordId && a.InstanceSlot == b.InstanceSlot &&
        a.InstanceGeneration == b.InstanceGeneration && a.VisibleRef == b.VisibleRef &&
        a.Flags == b.Flags && (a.Flags & 1u) != 0u;
}
bool NativeRange(uint proxy, out bool resident)
{
    resident = false;
    const HlodProxy p = Proxies[proxy]; const ProxyBinding b = Bindings[proxy];
    if (p.GroupIndex >= Counts.x || p.LogicalPage >= Limits.x || b.LogicalPage != p.LogicalPage ||
        b.CommandOrdinal >= Limits.z || b.Bucket >= Limits.y || b.Reserved != 0u ||
        (b.Flags & ~1u) != 0u || p.Reserved != 0u)
    { Fault(FaultRecord); return false; }
    if ((b.Flags & 1u) == 0u) return true;
    uint recordBytes, refs, stride;
    GeometryRecords.GetDimensions(recordBytes); ProxyRefs.GetDimensions(refs, stride);
    uint generation;
    if (b.ContentGeneration != Identity.x || b.PageVersion == 0u || b.RecordId == 0u ||
        b.RecordId > 0x3fffffffu || b.RecordId >= recordBytes / 64u || b.VisibleRef >= refs ||
        !SceneGeneration(b.InstanceSlot, generation) || generation != b.InstanceGeneration)
    { Fault(FaultIdentity | FaultRecord); return false; }
    const uint address = b.RecordId * 64u;
    const uint4 r0 = GeometryRecords.Load4(address), r1 = GeometryRecords.Load4(address + 16u);
    const uint4 r2 = GeometryRecords.Load4(address + 32u), r3 = GeometryRecords.Load4(address + 48u);
    const StaticRef ref = ProxyRefs[b.VisibleRef];
    const HlodBucket bucket = Buckets[b.Bucket];
    if (r0.x != b.PhysicalPage || r0.y != p.FirstIndex || r0.z != p.IndexCount ||
        r0.w != p.FirstMeshlet || r1.x != p.MeshletCount || r1.y != b.VisibleRef || r1.z != 1u ||
        (r1.w & (GeometrySceneRefs | GeometryStatic)) != (GeometrySceneRefs | GeometryStatic) ||
        r2.z != b.PageVersion || (r3.x & 255u) != p.MeshSemantic ||
        ref.InstanceSlot != b.InstanceSlot || ref.InstanceGeneration != b.InstanceGeneration ||
        (ref.MeshFlags & 255u) != p.MeshSemantic || !isfinite(ref.LodThreshold) ||
        !Range(bucket.FirstCommand, bucket.CommandCount, Limits.z) ||
        b.CommandOrdinal < bucket.FirstCommand || b.CommandOrdinal - bucket.FirstCommand >= bucket.CommandCount)
    { Fault(FaultRecord); return false; }
    resident = true; return true;
}
bool GroupValid(uint id, out HlodGroup group)
{
    group = (HlodGroup)0;
    if (id >= Counts.x) { Fault(FaultTables); return false; }
    group = Groups[id];
    if ((group.Parent != Invalid && group.Parent >= Counts.x) ||
        group.AncestorSteps == 0u || group.AncestorSteps > Options.y ||
        !Range(group.FirstMember, group.MemberCount, Counts.y) || group.MemberCount == 0u ||
        !Range(group.FirstProxy, group.ProxyCount, Counts.z) ||
        !all(isfinite(group.Sphere)) || group.Sphere.w < 0.0 ||
        !isfinite(group.Error.x) || group.Error.x < 0.0)
    { Fault(FaultTables); return false; }
    return true;
}
bool Intersects(float4 sphere, uint view)
{
    [unroll] for (uint plane = 0u; plane < 6u; ++plane)
    {
        const float4 p = Views[view].Planes[plane];
        if (dot(p.xyz, sphere.xyz) + p.w < -sphere.w * length(p.xyz)) return false;
    }
    return true;
}
float PixelError(HlodGroup group, uint view)
{
    const HlodView v = Views[view];
    if (v.ErrorPolicy.w == 1.0) return group.Error.x * v.ErrorPolicy.x;
    const float closest = dot(group.Sphere.xyz - v.OriginNear.xyz, v.Forward.xyz) - group.Sphere.w;
    // Near-plane intersections must refine to exact original leaves.
    if (!isfinite(closest) || closest <= v.OriginNear.w) return 3.402823466e+38f;
    return group.Error.x * v.ErrorPolicy.x / closest;
}
uint Eligible(uint id)
{
    HlodGroup group; if (!GroupValid(id, group) || group.ChildCount == 0u || group.ProxyCount == 0u) return 0u;
    bool complete = true, same = Identity.w != 0u;
    for (uint p = group.FirstProxy; p < group.FirstProxy + group.ProxyCount; ++p)
    {
        bool resident;
        if (!NativeRange(p, resident)) return 0u;
        complete = complete && resident; same = same && SameBinding(p);
    }
    uint mask = 0u;
    [unroll] for (uint view = 0u; view < 4u; ++view)
    {
        const uint bit = 1u << view;
        if ((Options.x & bit) == 0u || !Intersects(group.Sphere, view)) continue;
        const float error = PixelError(group, view);
        const float hysteresis = Views[view].ErrorPolicy.z;
        const bool previous = same && (PreviousGroups[id].y & bit) != 0u;
        const float threshold = Views[view].ErrorPolicy.y * (previous ? 1.0 + hysteresis : 1.0 - hysteresis);
        if (error <= threshold)
        {
            if (complete) mask |= bit;
            // Complete groups refresh IO admission too. Residency and native
            // record completeness still control the all-or-nothing draw cut.
            for (uint p = group.FirstProxy; p < group.FirstProxy + group.ProxyCount; ++p)
            {
                uint ignored;
                const float priority = min(3.402823466e+38f, max(1.0, group.Sphere.w * Views[view].ErrorPolicy.x));
                InterlockedMax(PageDemand[Proxies[p].LogicalPage], asuint(priority), ignored);
            }
        }
    }
    return mask;
}

[numthreads(64, 1, 1)]
void ClearCS(uint3 dispatch : SV_DispatchThreadID)
{
    const uint id = Thread(dispatch);
    if (id < Counts.w)
    {
        uint generation; SceneGeneration(id, generation);
        LeafMasks[id] = uint4(generation, 15u, 0u, 0u);
    }
    if (id < Counts.x) GroupChoice[id] = 0u;
    if (id < Counts.z)
    {
        ProxySelection[id] = 0u;
        [unroll] for (uint view = 0u; view < 4u; ++view)
        {
            IndexedCommands.Store4((view * Limits.z + id) * 24u, 0u);
            IndexedCommands.Store2((view * Limits.z + id) * 24u + 16u, 0u);
            MeshCommands.Store4((view * Limits.z + id) * 16u, 0u);
        }
    }
    if (id < Limits.x) PageDemand[id] = 0u;
    if (id < Limits.y * 4u) CommandCounts.Store(id * 4u, 0u);
    if (id == 0u)
    {
        Status.Store4(0u, uint4(Limits.w != 0u ? 0u : FaultIdentity, 0u, 0u, 0u));
        Status.Store4(16u, 0u); Status.Store4(32u, Identity); Status.Store4(48u, 0u);
    }
}

[numthreads(64, 1, 1)]
void SelectCS(uint3 dispatch : SV_DispatchThreadID)
{
    const uint id = Thread(dispatch);
    if (Limits.w == 0u || id >= Counts.x || !Dimensions()) return;
    uint selected = Eligible(id), covered = 0u;
    HlodGroup group; if (!GroupValid(id, group)) return;
    uint ancestor = group.Parent;
    for (uint step = 1u; ancestor != Invalid && step <= Options.y; ++step)
    {
        covered |= Eligible(ancestor);
        HlodGroup parent; if (!GroupValid(ancestor, parent)) return;
        ancestor = parent.Parent;
    }
    if (ancestor != Invalid) { Fault(FaultTables); return; }
    selected &= ~covered;
    const uint previous = Identity.w != 0u ? PreviousGroups[id].y : 0u;
    GroupChoice[id] = uint4(0u, selected, selected ^ previous, 0u);
    if (selected != 0u) { uint ignored; Status.InterlockedAdd(8u, 1u, ignored); }
}

[numthreads(64, 1, 1)]
void EmitCS(uint3 dispatch : SV_DispatchThreadID)
{
    const uint id = Thread(dispatch);
    if (Limits.w == 0u || !Dimensions()) return;
    if (id < Counts.y)
    {
        const HlodMember member = Members[id]; uint generation;
        if (member.InstanceSlot >= Counts.w || member.LeafGroup >= Counts.x ||
            member.ContentGeneration != Identity.x || !SceneGeneration(member.InstanceSlot, generation) ||
            member.InstanceGeneration != generation)
        { Fault(FaultIdentity); return; }
        uint mask = 15u, group = member.LeafGroup;
        for (uint step = 0u; group != Invalid && step < Options.y; ++step)
        {
            if (group >= Counts.x) { Fault(FaultTables); return; }
            mask &= ~GroupChoice[group].y; group = Groups[group].Parent;
        }
        if (group != Invalid) { Fault(FaultTables); return; }
        LeafMasks[member.InstanceSlot] = uint4(generation, mask, 0u, 0u);
    }
    if (id >= Counts.z) return;
    const HlodProxy p = Proxies[id]; const ProxyBinding b = Bindings[id];
    if (p.GroupIndex >= Counts.x) { Fault(FaultTables); return; }
    const uint mask = GroupChoice[p.GroupIndex].y;
    if (mask == 0u) return;
    bool resident; if (!NativeRange(id, resident) || !resident) { Fault(FaultRecord); return; }
    const uint changed = SameBinding(id) ? mask & GroupChoice[p.GroupIndex].z : mask;
    ProxySelection[id] = uint4(mask, changed, b.PageVersion, 0u);
    uint ignored; Status.InterlockedAdd(4u, 1u, ignored);
    if (changed != 0u) Status.InterlockedAdd(12u, 1u, ignored);
    const uint flags = GeometryRecords.Load(b.RecordId * 64u + 28u);
    [unroll] for (uint view = 0u; view < 4u; ++view)
    {
        const bool active = (mask & (1u << view)) != 0u;
        const uint pass = (b.RecordId << 2u) | view;
        const uint indexed = (view * Limits.z + b.CommandOrdinal) * 24u;
        IndexedCommands.Store4(indexed, uint4(pass, active ? p.IndexCount : 0u, active ? 1u : 0u, 0u));
        IndexedCommands.Store2(indexed + 16u, 0u);
        const bool mesh = active && (flags & GeometryMesh) != 0u;
        const uint x = mesh ? min(p.MeshletCount, 65535u) : 0u;
        const uint y = mesh ? (p.MeshletCount + 65534u) / 65535u : 1u;
        if (mesh && x * y > (1u << 22u)) { Fault(FaultCapacity); return; }
        MeshCommands.Store4((view * Limits.z + b.CommandOrdinal) * 16u, uint4(pass, x, y, 1u));
    }
}

[numthreads(64, 1, 1)]
void PublishCS(uint3 dispatch : SV_DispatchThreadID)
{
    const uint id = Thread(dispatch); const bool valid = Status.Load(0u) == 0u;
    if (id < Counts.w)
    {
        uint4 current = LeafMasks[id];
        if (!valid) current.y = 15u;
        const uint4 previous = Identity.w != 0u ? PreviousLeaves[id] : uint4(0u, 15u, 0u, 0u);
        current.z = Identity.w == 0u || previous.x != current.x || previous.y != current.y ? 1u : 0u;
        LeafMasks[id] = current;
    }
    if (id < Counts.z && !valid) ProxySelection[id] = 0u;
    if (id < Limits.y * 4u)
    {
        const uint bucket = id % Limits.y;
        CommandCounts.Store(id * 4u, valid ? Buckets[bucket].CommandCount : 0u);
    }
}

[numthreads(64, 1, 1)]
void HistoryCS(uint3 dispatch : SV_DispatchThreadID)
{
    const uint id = Thread(dispatch);
    if (id < Counts.x && Status.Load(0u) != 0u) GroupChoice[id] = 0u;
    if (id < Counts.z)
    {
        const uint4 selected = ProxySelection[id];
        if (selected.y != 0u)
        {
            const uint slot = Bindings[id].InstanceSlot;
            if (slot < Counts.w) { uint ignored; InterlockedOr(LeafMasks[slot].z, 1u, ignored); }
        }
    }
}
