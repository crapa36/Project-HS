#include "render_binding_contract.hlsli"
#include "hzb_region.hlsli"
// Runtime hierarchy cut transaction. ABI: renderer_cluster_catalog.hpp.
// Inputs and page residency stay immutable across Count/Prefix/Emit. One cut is
// shared by camera and three cascades, using the largest output-pixel error.
cbuffer ClusterRefinementConstants : register(b0, space6)
{
    uint CandidateCount, GroupCount, ClusterCount, ChildCount;
    uint PageCount, OutputCapacity, GeometryDrawBase, ContentGeneration;
    uint PrefixStage, PrefixBlockCount, MaxAncestorSteps, GeometryFrontendFlags;
    uint ClusterRefBase, RasterRecordBase, MaterialWord, ActiveViewMask;
    float4 CameraOriginNear;
    float4 ViewRight, ViewUp, ViewForward;
    float4 ProjectionScale;
    float4 ErrorTolerance;
    float4 FrustumPlanes[4][6];
    float4 FrustumPlaneLengths[6];
    uint HistoryInstanceCapacity, HistoryWordsPerInstance, PreviousHistoryInstanceCapacity, PreviousHistoryValid;
    uint FrameIdLow, FrameIdHigh, CatalogClusterBase, CatalogClusterCount;
    uint RepresentationPageCount;
};

cbuffer ClusterOcclusionConstants : register(b1, space6)
{
    float4x4 OcclusionCurrentViewProjection, OcclusionPreviousViewProjection;
    float4 OcclusionExtentNearFar;
    uint OcclusionLevelCount, OcclusionElementCount, OcclusionHistoryValid, OcclusionPhase;
};

struct ClusterGroup
{
    uint Parent, FirstChild, ChildCount, FirstCluster;
    uint ClusterCount, MaterialPartition, Flags, MaterialSelector;
    float4 Sphere;
    float4 MinimumError;
    uint4 RepresentationPages; // first page-list entry, unique page count
    uint CatalogIndex, Generation, AncestorSteps, NextSubtree;
};
struct ClusterRecord
{
    uint PageSlot, FirstIndex, IndexCount, FirstMeshlet;
    uint MeshletCount, MaterialSelector, GroupIndex, MaterialPartition;
};
struct ClusterPage
{
    uint Generation, Resident, PhysicalPageSlot, PageVersion;
    uint TriangleOffset, RemapOffset, DrawFlags, Reserved;
};
struct ClusterCandidate
{
    uint InstanceIndex, InstanceGeneration, CatalogIndex, ViewMask;
    uint FirstGroup, GroupCount, MeshId, PolicyFlags;
    float DeformationRadius;
    uint MeshFlags, FirstRoot, Reserved;
};
struct ClusterInstanceRef
{
    uint InstanceIndex, MeshFlags;
    float LodThreshold;
    uint InstanceGeneration;
};
struct InstanceData
{
    float4 Position, Scale;
    uint Color, Mesh;
    float Yaw, Padding;
};
#include "gpu_scene_tables.hlsli"
#include "scene_hlod_sampling.hlsli"

StructuredBuffer<ClusterGroup> Groups : register(t0, space6);
StructuredBuffer<uint> Children : register(t1, space6);
StructuredBuffer<ClusterRecord> Clusters : register(t2, space6);
StructuredBuffer<ClusterPage> Pages : register(t3, space6);
StructuredBuffer<ClusterCandidate> Candidates : register(t4, space6);
StructuredBuffer<uint> RepresentationPages : register(t10, space6);
StructuredBuffer<uint> PreviousHistoryBits : register(t5, space6);
StructuredBuffer<uint> PreviousHistoryGenerations : register(t6, space6);
StructuredBuffer<ClusterPage> PreviousPages : register(t7, space6);
ByteAddressBuffer PreviousCutStatus : register(t8, space6);
StructuredBuffer<float> OcclusionHzb : register(t9, space6);
RWStructuredBuffer<uint2> CutCounts : register(u0, space6);
RWStructuredBuffer<uint2> CutOffsets : register(u1, space6);
RWStructuredBuffer<uint2> BlockSums : register(u2, space6);
RWStructuredBuffer<uint2> BlockOffsets : register(u3, space6);
RWStructuredBuffer<uint> PageDemand : register(u4, space6);
RWByteAddressBuffer CutStatus : register(u5, space6);
RWByteAddressBuffer GeometryDrawRecords : register(u6, space6);
RWStructuredBuffer<ClusterInstanceRef> CutRefs : register(u7, space6);
RWByteAddressBuffer IndexedCommands : register(u8, space6);
RWByteAddressBuffer MeshCommands : register(u9, space6);
RWByteAddressBuffer CommandCounts : register(u10, space6);
RWByteAddressBuffer RasterRecords : register(u11, space6);
RWStructuredBuffer<uint> HistoryBits : register(u12, space6);
RWStructuredBuffer<uint> HistoryGenerations : register(u13, space6);
RWStructuredBuffer<float4> SelectedBounds : register(u14, space6);
RWStructuredBuffer<uint> PreviousRejected : register(u15, space6);
RWByteAddressBuffer RetestIndexedCommands : register(u16, space6);
RWByteAddressBuffer RetestMeshCommands : register(u17, space6);

static const uint Invalid = 0xffffffffu;
static const uint ReferencePose = 1u;
static const uint FineOnlyDeformationBound = 1u;
static const uint FailureCapacity = 1u;
static const uint FailureCatalog = 2u;
static const uint FailureMissingPage = 4u;
static const uint FailureErrorDomain = 8u;
static const uint FailureAncestorRange = 16u;
static const float MaximumPriority = 3.402823466e+38f;

void Fault(uint flags)
{
    uint ignored;
    CutStatus.InterlockedOr(16u, flags, ignored);
    CutStatus.Store(20u, 0u);
    // Emit never republishes live counts. Cull may trim cascade counts only
    // after every Emit completes and the transaction is known to be fault-free.
    CommandCounts.Store4(0u, uint4(0u, 0u, 0u, 0u));
    CommandCounts.Store(16u, 0u);
}
bool Range(uint first, uint count, uint size)
{
    return first <= size && count <= size - first;
}
uint LinearThread(uint3 dispatch)
{
    const uint row_stride = 65535u * 64u;
    if (dispatch.y > Invalid / row_stride) return Invalid;
    const uint first = dispatch.y * row_stride;
    return dispatch.x > Invalid - first ? Invalid : first + dispatch.x;
}
uint2 Add64(uint2 a, uint2 b)
{
    uint low = a.x + b.x;
    return uint2(low, a.y + b.y + (low < a.x ? 1u : 0u));
}
uint2 Sub64(uint2 a, uint2 b)
{
    return uint2(a.x - b.x, a.y - b.y - (a.x < b.x ? 1u : 0u));
}
bool CandidateValid(ClusterCandidate candidate, out InstanceData instance)
{
    instance = (InstanceData)0;
    uint4 h0, h1, h2, h3;
    if (!Range(candidate.FirstGroup, candidate.GroupCount, GroupCount) || candidate.GroupCount == 0u ||
        (candidate.ViewMask & ~15u) != 0u || (candidate.PolicyFlags & ~FineOnlyDeformationBound) != 0u ||
        candidate.FirstRoot < candidate.FirstGroup || candidate.FirstRoot - candidate.FirstGroup >= candidate.GroupCount ||
        candidate.Reserved != 0u || candidate.InstanceIndex >= HistoryInstanceCapacity ||
        !isfinite(candidate.DeformationRadius) || candidate.DeformationRadius < 0.0 ||
        !HsGpuSceneSlotValid(candidate.InstanceIndex, h0, h1, h2, h3))
    {
        Fault(FailureCatalog); return false;
    }
    if (Groups[candidate.FirstRoot].Parent != Invalid) { Fault(FailureCatalog); return false; }
    const uint address = h1.x + candidate.InstanceIndex * h2.z;
    if (HsGpuSceneTables.Load(address + 20u) != candidate.InstanceGeneration)
    {
        Fault(FailureCatalog); return false;
    }
    instance = HsLoadGpuSceneInstance(candidate.InstanceIndex);
    if (!all(isfinite(instance.Position)) || !all(isfinite(instance.Scale)) || !isfinite(instance.Yaw))
    {
        Fault(FailureCatalog); return false;
    }
    return true;
}
bool GroupValid(uint id, ClusterCandidate candidate, out ClusterGroup group)
{
    group = (ClusterGroup)0;
    if (id < candidate.FirstGroup || id - candidate.FirstGroup >= candidate.GroupCount)
    {
        Fault(FailureAncestorRange); return false;
    }
    group = Groups[id];
    if (group.CatalogIndex != candidate.CatalogIndex || group.Generation != ContentGeneration ||
        (group.NextSubtree != Invalid && (group.NextSubtree < candidate.FirstGroup ||
            group.NextSubtree - candidate.FirstGroup >= candidate.GroupCount)) ||
        group.ClusterCount == 0u || !Range(group.FirstCluster, group.ClusterCount, ClusterCount) ||
        group.RepresentationPages.y == 0u ||
        !Range(group.RepresentationPages.x, group.RepresentationPages.y, RepresentationPageCount) ||
        !Range(group.FirstChild, group.ChildCount, ChildCount))
    {
        Fault(FailureCatalog); return false;
    }
    if ((group.Flags & ReferencePose) != 0u && (candidate.PolicyFlags & FineOnlyDeformationBound) == 0u)
    {
        Fault(FailureErrorDomain); return false;
    }
    return true;
}
bool ClusterValid(uint id, uint group_id, ClusterGroup group, out ClusterRecord cluster)
{
    if (id < CatalogClusterBase || id - CatalogClusterBase >= CatalogClusterCount)
    {
        cluster = (ClusterRecord)0; Fault(FailureCatalog); return false;
    }
    cluster = Clusters[id];
    if (cluster.GroupIndex != group_id || cluster.MaterialPartition != group.MaterialPartition ||
        cluster.MaterialSelector != group.MaterialSelector || cluster.PageSlot >= PageCount ||
        cluster.IndexCount == 0u || cluster.IndexCount % 3u != 0u || cluster.FirstIndex % 3u != 0u ||
        cluster.MeshletCount == 0u)
    {
        Fault(FailureCatalog); return false;
    }
    return true;
}
bool Resident(uint page_slot)
{
    const ClusterPage page = Pages[page_slot];
    if (page.Resident != 0u && (page.Resident != 1u || page.PhysicalPageSlot >= HsGeometryPageCapacity ||
        page.PageVersion == 0u || page.Reserved != 0u || (page.DrawFlags & ~31u) != 0u))
    {
        Fault(FailureCatalog); return false;
    }
    return page.Generation == ContentGeneration && page.Resident == 1u;
}
bool CompleteRepresentation(ClusterGroup group)
{
    bool complete = true;
    for (uint offset = 0u; offset < group.RepresentationPages.y; ++offset)
        complete = Resident(RepresentationPages[group.RepresentationPages.x + offset]) && complete;
    return complete;
}
bool DemandRepresentation(ClusterGroup group, float priority, bool check_residency)
{
    const uint encoded = asuint(max(priority, 1.0));
    bool complete = true;
    for (uint offset = 0u; offset < group.RepresentationPages.y; ++offset)
    {
        const uint page_slot = RepresentationPages[group.RepresentationPages.x + offset];
        uint ignored;
        InterlockedMax(PageDemand[page_slot], encoded, ignored);
        // Missing pages still need demand; inspect the complete representation.
        if (check_residency) complete = Resident(page_slot) && complete;
    }
    return complete;
}
float4 WorldSphere(ClusterGroup group, ClusterCandidate candidate, InstanceData instance)
{
    const float scale = max(abs(instance.Scale.x), max(abs(instance.Scale.y), abs(instance.Scale.z)));
    const float3 local = group.Sphere.xyz * instance.Scale.xyz;
    float sine_yaw, cosine_yaw;
    sincos(instance.Yaw, sine_yaw, cosine_yaw);
    const float3 center = instance.Position.xyz + float3(local.x * cosine_yaw + local.z * sine_yaw,
        local.y, -local.x * sine_yaw + local.z * cosine_yaw);
    // The group encloses every cluster in its selected representation. Retain
    // that conservative enclosure rather than treating undeformed mesh bounds
    // as a qualification for deformed geometry.
    const float radius = group.Sphere.w * scale + candidate.DeformationRadius;
    return float4(center, radius);
}
uint Projection(ClusterGroup group, ClusterCandidate candidate, InstanceData instance, out float priority)
{
    const float4 sphere = WorldSphere(group, candidate, instance);
    const float3 center = sphere.xyz;
    const float radius = sphere.w;
    const float scale = max(abs(instance.Scale.x), max(abs(instance.Scale.y), abs(instance.Scale.z)));
    if (!all(isfinite(center)) || !isfinite(radius)) { Fault(FailureCatalog); priority = 0.0; return 0u; }
    uint mask = candidate.ViewMask;
    [unroll] for (uint view = 0u; view < 4u; ++view)
    {
        const uint view_bit = 1u << view;
        if ((mask & view_bit) == 0u) continue;
        [unroll] for (uint plane = 0u; plane < 6u; ++plane)
            if (dot(float4(center, 1.0), FrustumPlanes[view][plane]) <
                -radius * FrustumPlaneLengths[plane][view])
            {
                mask &= ~view_bit;
                break;
            }
    }
    priority = 0.0;
    if (mask == 0u) return mask;
    if ((group.Flags & ReferencePose) != 0u)
    {
        // No reference-pose geometric error participates in a qualified cut.
        // An explicit bounded-deformation policy requires complete original leaves.
        priority = MaximumPriority; return mask;
    }
    const float error = group.MinimumError.w * scale;
    const float3 relative = center - CameraOriginNear.xyz;
    const float3 view_center = float3(dot(relative, ViewRight.xyz), dot(relative, ViewUp.xyz), dot(relative, ViewForward.xyz));
    [unroll] for (uint view = 0u; view < 4u; ++view)
    {
        if ((mask & (1u << view)) == 0u) continue;
        float projected;
        if (view == 0u)
        {
            const float distance = view_center.z - radius - error;
            if (distance <= CameraOriginNear.w) { priority = MaximumPriority; continue; }
            const float rho = length(abs(view_center.xy) + radius + error) / distance;
            projected = error * ProjectionScale.x * sqrt(1.0 + rho * rho) / distance;
        }
        else projected = error * ProjectionScale[view];
        const float ratio = projected / ErrorTolerance[view];
        if (!isfinite(ratio)) priority = MaximumPriority;
        else priority = max(priority, ratio);
    }
    return mask;
}
bool Refinement(ClusterGroup group, ClusterCandidate candidate, InstanceData instance,
                out uint mask, out float priority, out bool desired)
{
    mask = Projection(group, candidate, instance, priority);
    desired = mask != 0u && group.ChildCount != 0u && priority > 1.0;
    if (!desired) return false;
    bool complete = true;
    for (uint offset = 0u; offset < group.ChildCount; ++offset)
    {
        const uint child_id = Children[group.FirstChild + offset];
        ClusterGroup child;
        if (!GroupValid(child_id, candidate, child))
        {
            Fault(FailureCatalog); return false;
        }
        complete = CompleteRepresentation(child) && complete;
    }
    return complete;
}
[numthreads(64, 1, 1)]
void CountCuts(uint3 dispatch : SV_DispatchThreadID)
{
    const uint candidate_index = LinearThread(dispatch);
    if (candidate_index >= CandidateCount) return;
    CutCounts[candidate_index] = uint2(0u, 0u);
    ClusterCandidate candidate = Candidates[candidate_index];
    InstanceData instance;
    if (!CandidateValid(candidate, instance)) return;
    candidate.ViewMask &= ActiveViewMask & HsHlodLeafViewMask(candidate.InstanceIndex, candidate.InstanceGeneration);
    const uint cache_stride = OutputCapacity / CandidateCount;
    const uint cache_base = CandidateCount * 2u + candidate_index * cache_stride;
    uint selected_groups = 0u;
    uint count = 0u;
    uint node = candidate.FirstRoot;
    for (uint visited = 0u; node != Invalid && visited < candidate.GroupCount; ++visited)
    {
        const uint id = node;
        ClusterGroup group;
        if (!GroupValid(id, candidate, group)) return;
        uint mask; float priority; bool desired;
        const bool refined = Refinement(group, candidate, instance, mask, priority, desired);
        node = refined ? Children[group.FirstChild] : group.NextSubtree;
        if (mask == 0u) continue;
        if (desired)
            for (uint child = 0u; child < group.ChildCount; ++child)
            {
                const uint child_id = Children[group.FirstChild + child];
                ClusterGroup child_group;
                if (GroupValid(child_id, candidate, child_group)) DemandRepresentation(child_group, priority, false);
            }
        if (refined) continue;
        if (!DemandRepresentation(group, priority, true)) { Fault(FailureMissingPage); continue; }
        if ((group.Flags & ReferencePose) != 0u && group.ChildCount != 0u)
        {
            Fault(FailureErrorDomain); continue;
        }
        if (group.ClusterCount > Invalid - count) { Fault(FailureCapacity); return; }
        if (selected_groups >= cache_stride) { Fault(FailureCapacity); return; }
        CutCounts[cache_base + selected_groups++] = uint2(id, mask);
        count += group.ClusterCount;
    }
    if (node != Invalid) { Fault(FailureAncestorRange); return; }
    // Publish directory only after the immutable traversal completes.
    CutCounts[CandidateCount + candidate_index] = uint2(count, selected_groups);
    CutCounts[candidate_index] = uint2(count, 0u);
}

groupshared uint2 scan_values[64];

[numthreads(64, 1, 1)]
void PrefixCuts(uint3 dispatch : SV_DispatchThreadID, uint3 group_id : SV_GroupID,
                uint lane : SV_GroupIndex)
{
    const uint thread_index = LinearThread(dispatch);
    const uint block_index = group_id.x + group_id.y * 65535u;
    if (PrefixStage == 0u)
    {
        // All lanes participate in every barrier, including the final partial block.
        const uint2 count = thread_index < CandidateCount ? CutCounts[thread_index] : uint2(0u, 0u);
        scan_values[lane] = count;
        GroupMemoryBarrierWithGroupSync();
        for (uint distance = 1u; distance < 64u; distance <<= 1u)
        {
            const uint2 previous = lane >= distance ? scan_values[lane - distance] : uint2(0u, 0u);
            GroupMemoryBarrierWithGroupSync();
            scan_values[lane] = Add64(scan_values[lane], previous);
            GroupMemoryBarrierWithGroupSync();
        }
        if (thread_index < CandidateCount) CutOffsets[thread_index] = Sub64(scan_values[lane], count);
        if (lane == 63u && block_index < PrefixBlockCount) BlockSums[block_index] = scan_values[lane];
        return;
    }
    if (PrefixStage == 1u)
    {
        if (thread_index != 0u) return;
        CommandCounts.Store4(0u, uint4(0u, 0u, 0u, 0u));
        uint2 total = uint2(0u, 0u);
        for (uint block = 0u; block < PrefixBlockCount; ++block)
        {
            BlockOffsets[block] = total;
            total = Add64(total, BlockSums[block]);
        }
        const uint2 commands = uint2(total.x << 2u, (total.y << 2u) | (total.x >> 30u));
        CutStatus.Store4(0u, uint4(total, commands));
        CutStatus.Store(24u, CandidateCount);
        CutStatus.Store4(32u, uint4(FrameIdLow, FrameIdHigh, ContentGeneration, Candidates[0].CatalogIndex));
        if (total.y != 0u || total.x > OutputCapacity) Fault(FailureCapacity);
        const uint committed = CutStatus.Load(16u) == 0u ? total.x : 0u;
        CutStatus.Store(20u, committed);
        // Main is published by fault-immutable Cull from its final live commands.
        // Keep the full selected transaction for Emit/history/current-depth Retest.
        CommandCounts.Store4(0u, uint4(0u, committed, committed, committed));
        return;
    }
    if (PrefixStage == 2u)
    {
        if (thread_index < CandidateCount)
            CutOffsets[thread_index] = Add64(CutOffsets[thread_index], BlockOffsets[thread_index / 64u]);
        if (CutStatus.Load(16u) != 0u && thread_index < OutputCapacity)
        {
            [unroll] for (uint view = 0u; view < 4u; ++view)
            {
                IndexedCommands.Store4((view * OutputCapacity + thread_index) * 24u, uint4(0u, 0u, 0u, 0u));
                IndexedCommands.Store2((view * OutputCapacity + thread_index) * 24u + 16u, uint2(0u, 0u));
                MeshCommands.Store4((view * OutputCapacity + thread_index) * 16u, uint4(0u, 0u, 0u, 0u));
            }
        }
        return;
    }
    if (thread_index == 0u) Fault(FailureCatalog);
}

bool SameSelectedHistory(uint cluster_id, ClusterRecord cluster, ClusterCandidate candidate, ClusterPage page)
{
    if (PreviousHistoryValid == 0u || candidate.InstanceIndex >= PreviousHistoryInstanceCapacity) return false;
    const uint4 stamp = PreviousCutStatus.Load4(32u);
    const uint previous_low = FrameIdLow - 1u;
    const uint previous_high = FrameIdHigh - (FrameIdLow == 0u ? 1u : 0u);
    if (PreviousCutStatus.Load(16u) != 0u || stamp.x != previous_low || stamp.y != previous_high ||
        stamp.z != ContentGeneration || stamp.w != candidate.CatalogIndex ||
        PreviousHistoryGenerations[candidate.InstanceIndex] != candidate.InstanceGeneration) return false;
    const uint local_cluster = cluster_id - CatalogClusterBase;
    const uint word = candidate.InstanceIndex * HistoryWordsPerInstance + local_cluster / 32u;
    if ((PreviousHistoryBits[word] & (1u << (local_cluster % 32u))) == 0u) return false;
    const ClusterPage previous_page = PreviousPages[cluster.PageSlot];
    return previous_page.Generation == ContentGeneration && previous_page.Resident == 1u &&
        previous_page.PhysicalPageSlot == page.PhysicalPageSlot && previous_page.PageVersion == page.PageVersion;
}

void EmitCluster(uint selection, uint cluster_id, uint group_id, ClusterGroup group,
    ClusterRecord cluster, ClusterCandidate candidate, InstanceData instance, uint mask)
{
    if (selection >= CutStatus.Load(20u) || selection >= OutputCapacity || !Resident(cluster.PageSlot))
    {
        Fault(FailureCapacity | FailureMissingPage); return;
    }
    const ClusterPage page = Pages[cluster.PageSlot];
    const uint record_id = GeometryDrawBase + selection;
    const uint address = record_id * 64u;
    const uint flags = (page.DrawFlags & 8u) | 1u | 4u | 32u | GeometryFrontendFlags;
    GeometryDrawRecords.Store4(address, uint4(page.PhysicalPageSlot, cluster.FirstIndex, cluster.IndexCount, cluster.FirstMeshlet));
    const uint visible_ref = ClusterRefBase + selection;
    const uint raster_id = RasterRecordBase == Invalid ? Invalid : RasterRecordBase + selection;
    GeometryDrawRecords.Store4(address + 16u, uint4(cluster.MeshletCount, visible_ref, 1u, flags));
    GeometryDrawRecords.Store4(address + 32u, uint4(page.TriangleOffset, page.RemapOffset, page.PageVersion, 0x10000000u + group.MaterialPartition));
    GeometryDrawRecords.Store4(address + 48u, uint4(candidate.MeshId, asuint(group.MinimumError.w), 6u, raster_id));
    ClusterInstanceRef reference;
    reference.InstanceIndex = candidate.InstanceIndex;
    reference.MeshFlags = (candidate.MeshFlags & ~0x100000u) |
        (SameSelectedHistory(cluster_id, cluster, candidate, page) ? 0u : 0x100000u);
    reference.LodThreshold = 1.0; reference.InstanceGeneration = candidate.InstanceGeneration;
    CutRefs[visible_ref] = reference;
    SelectedBounds[selection] = WorldSphere(group, candidate, instance);
    // Membership belongs to the complete transaction. A concurrent Emit fault
    // invalidates its status, so none of these bits can qualify the next frame.
    const uint local_cluster = cluster_id - CatalogClusterBase;
    const uint word = candidate.InstanceIndex * HistoryWordsPerInstance + local_cluster / 32u;
    uint ignored;
    InterlockedOr(HistoryBits[word], 1u << (local_cluster % 32u), ignored);
    InterlockedExchange(HistoryGenerations[candidate.InstanceIndex], candidate.InstanceGeneration, ignored);
    if (raster_id != Invalid)
    {
        RasterRecords.Store4(raster_id * 32u, uint4(record_id, visible_ref, 6u, MaterialWord));
        RasterRecords.Store4(raster_id * 32u + 16u, uint4(candidate.InstanceIndex, candidate.InstanceGeneration, page.PageVersion, 1u + group.MaterialPartition));
    }
    [unroll] for (uint view = 0u; view < 4u; ++view)
    {
        const bool active = (mask & (1u << view)) != 0u;
        const uint pass = (record_id << 2u) | view;
        const uint indexed_address = (view * OutputCapacity + selection) * 24u;
        IndexedCommands.Store4(indexed_address, uint4(pass, active ? cluster.IndexCount : 0u, active ? 1u : 0u, 0u));
        IndexedCommands.Store2(indexed_address + 16u, uint2(0u, 0u));
        const uint mesh_address = (view * OutputCapacity + selection) * 16u;
        const uint x = active ? min(cluster.MeshletCount, 65535u) : 0u;
        const uint y = active ? (cluster.MeshletCount / 65535u + (cluster.MeshletCount % 65535u != 0u ? 1u : 0u)) : 1u;
        MeshCommands.Store4(mesh_address, uint4(pass, x, y, 1u));
    }
}

[numthreads(64, 1, 1)]
void EmitCuts(uint3 dispatch : SV_DispatchThreadID)
{
    const uint candidate_index = LinearThread(dispatch);
    if (candidate_index >= CandidateCount || CutStatus.Load(16u) != 0u) return;
    ClusterCandidate candidate = Candidates[candidate_index];
    InstanceData instance;
    if (!CandidateValid(candidate, instance)) return;
    candidate.ViewMask &= ActiveViewMask & HsHlodLeafViewMask(candidate.InstanceIndex, candidate.InstanceGeneration);
    const uint2 base = CutOffsets[candidate_index];
    if (base.y != 0u) { Fault(FailureCapacity); return; }
    const uint2 expected = CutCounts[candidate_index];
    // Count validated the immutable cut; an empty cut has no records or history to emit.
    if (all(expected == uint2(0u, 0u))) return;
    uint cache_elements, cache_element_stride;
    CutCounts.GetDimensions(cache_elements, cache_element_stride);
    if (CandidateCount > cache_elements / 2u ||
        !Range(CandidateCount * 2u, OutputCapacity, cache_elements)) { Fault(FailureCapacity); return; }
    const uint cache_stride = OutputCapacity / CandidateCount;
    const uint cache_base = CandidateCount * 2u + candidate_index * cache_stride;
    const uint2 selected = CutCounts[CandidateCount + candidate_index];
    if (expected.y != 0u || selected.x != expected.x || selected.y == 0u || selected.y > cache_stride)
    { Fault(FailureCatalog); return; }
    uint count = 0u;
    uint3 cascade_suffix = uint3(0u, 0u, 0u);
    for (uint selected_index = 0u; selected_index < selected.y; ++selected_index)
    {
        const uint2 selection = CutCounts[cache_base + selected_index];
        const uint id = selection.x;
        const uint mask = selection.y;
        ClusterGroup group;
        if (!GroupValid(id, candidate, group)) return;
        if (mask == 0u || (mask & ~candidate.ViewMask) != 0u || group.ClusterCount > expected.x - count)
        { Fault(FailureCatalog); return; }
        if (!CompleteRepresentation(group) || ((group.Flags & ReferencePose) != 0u && group.ChildCount != 0u))
        {
            Fault(FailureMissingPage | FailureErrorDomain); return;
        }
        for (uint cluster_offset = 0u; cluster_offset < group.ClusterCount; ++cluster_offset)
        {
            ClusterRecord cluster;
            const uint cluster_id = group.FirstCluster + cluster_offset;
            if (!ClusterValid(cluster_id, id, group, cluster)) return;
            EmitCluster(base.x + count, cluster_id, id, group, cluster, candidate, instance, mask);
            ++count;
        }
        const uint end = base.x + count;
        if ((mask & 2u) != 0u) cascade_suffix.x = max(cascade_suffix.x, end);
        if ((mask & 4u) != 0u) cascade_suffix.y = max(cascade_suffix.y, end);
        if ((mask & 8u) != 0u) cascade_suffix.z = max(cascade_suffix.z, end);
    }
    if (expected.y != 0u || count != expected.x)
    {
        Fault(FailureCatalog); return;
    }
    // Scratch only: a concurrent Emit fault may still invalidate all commands.
    // Publish indirect counts later, after the complete transaction is immutable.
    [unroll] for (uint cascade = 0u; cascade < 3u; ++cascade)
    {
        if (cascade_suffix[cascade] == 0u) continue;
        uint ignored;
        CommandCounts.InterlockedMax(20u + cascade * 4u, cascade_suffix[cascade], ignored);
    }
}

bool SphereOccluded(float4 sphere, float4x4 view_projection)
{
    uint hzb_count, hzb_stride;
    OcclusionHzb.GetDimensions(hzb_count, hzb_stride);
    if (OcclusionHistoryValid == 0u || OcclusionPhase > 1u ||
        OcclusionLevelCount == 0u || OcclusionLevelCount > 16u ||
        OcclusionElementCount == 0u || OcclusionElementCount > hzb_count ||
        !all(isfinite(OcclusionExtentNearFar)) ||
        any(OcclusionExtentNearFar.xy < 1.0) ||
        OcclusionExtentNearFar.z <= 0.0 || OcclusionExtentNearFar.w <= OcclusionExtentNearFar.z ||
        !all(isfinite(sphere)) || sphere.w < 0.0) return false;
    const float4 clip = mul(float4(sphere.xyz, 1.0), view_projection);
    const float3 x_axis = float3(view_projection._11, view_projection._21, view_projection._31);
    const float3 y_axis = float3(view_projection._12, view_projection._22, view_projection._32);
    const float3 z_axis = float3(view_projection._13, view_projection._23, view_projection._33);
    const float3 w_axis = float3(view_projection._14, view_projection._24, view_projection._34);
    if (!all(isfinite(clip)) || !all(isfinite(x_axis)) || !all(isfinite(y_axis)) ||
        !all(isfinite(z_axis)) || !all(isfinite(w_axis))) return false;
    const float minimum_w = clip.w - sphere.w * length(w_axis);
    // Perspective clip.w is view depth. Any near-plane intersection, including
    // the actual physical near distance in a reversed projection, stays visible.
    if (!isfinite(minimum_w) || minimum_w <= OcclusionExtentNearFar.z) return false;
    const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
    const float2 uv_radius = 0.5 * sphere.w *
        (float2(length(x_axis), length(y_axis)) + abs(clip.xy / clip.w) * length(w_axis)) / minimum_w;
    const float2 uv_min = uv - uv_radius - 1.0 / OcclusionExtentNearFar.xy;
    const float2 uv_max = uv + uv_radius + 1.0 / OcclusionExtentNearFar.xy;
    if (!all(isfinite(uv_min)) || !all(isfinite(uv_max)) ||
        any(uv_min <= 0.0) || any(uv_max >= 1.0)) return false;

    const uint2 extent = (uint2)OcclusionExtentNearFar.xy;
    const uint2 base_min = (uint2)(uv_min * OcclusionExtentNearFar.xy);
    const uint2 base_max = (uint2)(uv_max * OcclusionExtentNearFar.xy);
    HsHzbRegion region;
    if (!HsFindHzbRegion(extent.x, extent.y, OcclusionLevelCount, OcclusionElementCount,
        base_min.x, base_min.y, base_max.x, base_max.y, region)) return false;
    const uint2 minimum_coordinate = uint2(region.min_x, region.min_y);
    const uint2 maximum_coordinate = uint2(region.max_x, region.max_y);
    const uint offset = region.offset;
    const uint2 coordinates[4] = {minimum_coordinate,
        uint2(maximum_coordinate.x, minimum_coordinate.y),
        uint2(minimum_coordinate.x, maximum_coordinate.y), maximum_coordinate};
    float region_farthest = 1.0;
    [unroll] for (uint sample = 0u; sample < 4u; ++sample)
    {
        const float depth = OcclusionHzb[offset + coordinates[sample].y * region.width + coordinates[sample].x];
        if (!isfinite(depth) || depth <= 0.0 || depth > 1.0) return false;
        region_farthest = min(region_farthest, depth);
    }
    // Bound homogeneous Z/W over the entire world sphere using this HZB's
    // actual projection. This includes rotation and denominator variation and
    // remains conservative without reconstructing a different near/far matrix.
    const float center_depth = clip.z / clip.w;
    const float nearest_depth = center_depth + sphere.w *
        (length(z_axis) + abs(center_depth) * length(w_axis)) / minimum_w;
    return isfinite(nearest_depth) && nearest_depth >= 0.0 && nearest_depth + 0.0005 < region_farthest;
}

[numthreads(64, 1, 1)]
void CullClusters(uint3 dispatch : SV_DispatchThreadID)
{
    const uint selection = LinearThread(dispatch);
    if (selection == 0u)
    {
        const uint failure = CutStatus.Load(16u);
        const uint committed = CutStatus.Load(20u);
        const uint3 cascade_suffix = CommandCounts.Load3(20u);
        // This pass follows every Emit. Keep holes and command order, trimming
        // only empty cascade suffixes even when camera HZB/history is disabled.
        if (failure != 0u)
            CommandCounts.Store3(4u, uint3(0u, 0u, 0u));
        else if (committed <= OutputCapacity && all(cascade_suffix <= committed))
            CommandCounts.Store3(4u, cascade_suffix);
        // An unexpected scratch bound leaves the original full prefix intact.
    }
    if (selection >= OutputCapacity || selection >= CutStatus.Load(20u) || CutStatus.Load(16u) != 0u) return;
    PreviousRejected[selection] = 0u;
    const uint address = selection * 24u; // Only main-camera commands.
    const uint2 draw = IndexedCommands.Load2(address + 4u);
    const ClusterInstanceRef reference = CutRefs[ClusterRefBase + selection];
    // Newly selected clusters, reused instance slots and new page versions
    // carry history-reset membership and cannot trust previous depth.
    if (draw.x == 0u || draw.y == 0u) return;
    if ((reference.MeshFlags & 0x100000u) == 0u && OcclusionPhase == 0u &&
        SphereOccluded(SelectedBounds[selection], OcclusionPreviousViewProjection))
    {
        PreviousRejected[selection] = 1u;
        IndexedCommands.Store2(address + 4u, uint2(0u, 0u));
        MeshCommands.Store3(selection * 16u + 4u, uint3(0u, 1u, 1u));
        return;
    }
    // Prefix1 cleared this count before Emit/Cull. No Fault writer remains;
    // preserve every surviving command, including history-disabled/new cuts.
    uint ignored;
    CommandCounts.InterlockedMax(0u, selection + 1u, ignored);
}

[numthreads(64, 1, 1)]
void RetestClusters(uint3 dispatch : SV_DispatchThreadID)
{
    const uint selection = LinearThread(dispatch);
    const uint committed = CutStatus.Load(16u) == 0u ? min(CutStatus.Load(20u), OutputCapacity) : 0u;
    if (selection >= committed) return;
    const uint record_id = GeometryDrawBase + selection;
    const uint address = record_id * 64u;
    const bool visible = PreviousRejected[selection] != 0u &&
        (OcclusionPhase != 1u || !SphereOccluded(SelectedBounds[selection], OcclusionCurrentViewProjection));
    const uint index_count = visible ? GeometryDrawRecords.Load(address + 8u) : 0u;
    const uint meshlet_count = visible ? GeometryDrawRecords.Load(address + 16u) : 0u;
    RetestIndexedCommands.Store4(selection * 24u, uint4(record_id << 2u, index_count, visible ? 1u : 0u, 0u));
    RetestIndexedCommands.Store2(selection * 24u + 16u, uint2(0u, 0u));
    const uint x = min(meshlet_count, 65535u);
    const uint y = visible ? meshlet_count / 65535u + (meshlet_count % 65535u != 0u ? 1u : 0u) : 1u;
    RetestMeshCommands.Store4(selection * 16u, uint4(record_id << 2u, x, y, 1u));
    // Count cleared this word; preserve every live draw and omit only an empty suffix.
    if (visible)
    {
        uint ignored;
        CommandCounts.InterlockedMax(16u, selection + 1u, ignored);
    }
}
