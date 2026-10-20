#include "hzb_region.hlsli"
#include "environment_wind.hlsli"
#include "scene_hlod_sampling.hlsli"
cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProjection;
    float4x4 PreviousViewProjection;
    float4 TemporalOptions;
    float4 CameraTime;
    float4 LightDirectionIntensity;
    float4 LightColor;
    float4 ScreenSize;
    float4 CameraForwardSoftness;
    float4x4 ShadowViewProjection[3];
    float4 ShadowAtlasScaleOffset[3];
    float4 ShadowAtlasTexelSize;
    row_major float4x4 ArcherBones[128];
    uint4 MonsterAssetMeta[6];
    uint4 MonsterClipMeta[6][5];
    float4 RenderOptions;
    uint4 ParticleOptions;
    float4 GrassBenders[32];
    uint4 GrassBenderCount;
    uint4 AuthoredParticleClock;
    float4 VfxLightPositionRadius[32];
    float4 VfxLightColorIntensity[32];
    uint4 VfxLightCount;
    uint4 VfxDecalCount;
    uint4 SkinCacheOptions;
    row_major float4x4 InverseViewProjection;
    row_major float4x4 CurrentUnjitteredViewProjection;
    row_major float4x4 PreviousUnjitteredViewProjection;
    float4 SurfaceViewOrigin;
    float4 SurfaceJitterUv;
    float4 LodViewRight;
    float4 LodViewUp;
    float4 LodViewForward;
    float4 LodProjection;
    float4 LodOptions;
    uint4 LodClock;
};

cbuffer VisibilityConstants : register(b1)
{
    uint VisibleRefStride;
    float GrassDeformationRadius;
};

struct InstanceData
{
    float4 Position; // xyz: center; w: conservative world-space radius
    float4 Scale;
    uint Color;
    uint Mesh;
    float Yaw;
    float Padding;
};
#include "gpu_scene_tables.hlsli"

struct StaticDrawRef
{
    uint InstanceIndex;
    uint MeshFlags;
    float LodThreshold;
    uint InstanceGeneration;
};

struct VisibilityRange
{
    uint Begin;
    uint Count;
    uint IndexCount;
    uint ArgumentStride;
    uint DrawRecordBase;
    uint MeshletCount;
    uint Flags;
    uint Reserved;
};

StructuredBuffer<VisibilityRange> Ranges : register(t0, space3);
StructuredBuffer<float> PreviousHzb : register(t2, space3);
StructuredBuffer<float4> SkinBounds : register(t3, space3);
StructuredBuffer<StaticDrawRef> StaticSceneDraws : register(t29, space3);
RWStructuredBuffer<StaticDrawRef> VisibleDraws : register(u0);
RWByteAddressBuffer Draws : register(u1);
RWStructuredBuffer<StaticDrawRef> RejectedDraws : register(u2);
RWByteAddressBuffer RejectedArgs : register(u3);
RWByteAddressBuffer GeometryDrawRecords : register(u1, space3);
RWByteAddressBuffer LodStates : register(u2, space3);

// Every bucket/view owns a 24-byte slot. Indexed commands are constant+20B;
// mesh commands are constant+12B and leave the final eight bytes unused.
void StoreGeometryArguments(VisibilityRange range, uint bucket, uint view, uint count)
{
    const uint record_id = range.DrawRecordBase + view;
    const uint record_offset = record_id * 64u;
    const uint argument_offset = (view * range.ArgumentStride + bucket) * 24u;
    const uint pass_value = (record_id << 2u) | view;
    GeometryDrawRecords.Store(record_offset + 24u, count);
    if ((range.Flags & 2u) != 0u)
    {
        const uint groups = range.MeshletCount * count;
        const uint x = min(groups, 65535u);
        const uint y = groups == 0u ? 1u : (groups + 65534u) / 65535u;
        Draws.Store4(argument_offset, uint4(pass_value, x, y, 1u));
        Draws.Store2(argument_offset + 16u, uint2(0u, 0u));
    }
    else
    {
        const uint first_index = GeometryDrawRecords.Load(record_offset + 4u);
        Draws.Store4(argument_offset, uint4(pass_value, range.IndexCount, count, first_index));
        Draws.Store2(argument_offset + 16u, uint2(0u, 0u));
    }
}

groupshared uint visible_flags[64];
groupshared uint rejected_flags[64];
groupshared uint visible_count;
groupshared uint rejected_count;
groupshared uint family_visible_flags[3][64];
groupshared uint family_rejected_flags[3][64];
groupshared uint family_visible_count[3];
groupshared uint family_rejected_count[3];

struct LodPage
{
    uint PageSlot;
    uint Generation;
    uint PhysicalLod;
    float ErrorLocal;
};

LodPage LoadLodPage(VisibilityRange range, uint view)
{
    const uint offset = (range.DrawRecordBase + view) * 64u;
    LodPage page;
    page.PageSlot = GeometryDrawRecords.Load(offset);
    page.Generation = GeometryDrawRecords.Load(offset + 40u);
    page.PhysicalLod = GeometryDrawRecords.Load(offset + 44u);
    page.ErrorLocal = asfloat(GeometryDrawRecords.Load(offset + 52u));
    return page;
}

uint CanonicalLodBucket(uint bucket, LodPage pages[3])
{
    [unroll] for (uint preceding = 0; preceding < 3u; ++preceding)
        if (preceding < bucket &&
            pages[preceding].PageSlot == pages[bucket].PageSlot &&
            pages[preceding].Generation == pages[bucket].Generation)
            return preceding;
    return bucket;
}

uint FinestLodBucket(LodPage pages[3])
{
    uint finest = 0u;
    [unroll] for (uint bucket = 1; bucket < 3u; ++bucket)
        if (CanonicalLodBucket(bucket, pages) == bucket &&
            pages[bucket].PhysicalLod < pages[finest].PhysicalLod)
            finest = bucket;
    return finest;
}

uint SelectLodBucket(LodPage pages[3], float errors[3], float tolerance,
                     uint finest)
{
    uint selected = finest;
    [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
        if (CanonicalLodBucket(bucket, pages) == bucket &&
            errors[bucket] <= tolerance &&
            pages[bucket].PhysicalLod >= pages[selected].PhysicalLod)
            selected = bucket;
    return selected;
}

// Each persistent scene slot owns one state per view. Only the first bucket in
// its family evaluates the unique input span and writes these states.
uint ResolveFamilyLod(VisibilityRange range, uint view, LodPage pages[3],
                      InstanceData instance, float3 center, float radius,
                      StaticDrawRef source, out StaticDrawRef selected[3])
{
    const uint finest = FinestLodBucket(pages);
    const float world_scale = max(abs(instance.Scale.x),
        max(abs(instance.Scale.y), abs(instance.Scale.z)));
    const float3 camera_relative = center - CameraTime.xyz;
    const float3 view_center = float3(
        dot(camera_relative, LodViewRight.xyz),
        dot(camera_relative, LodViewUp.xyz),
        dot(camera_relative, LodViewForward.xyz));
    float errors[3];
    bool near_bound = false;
    [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
    {
        float world_error = pages[bucket].ErrorLocal * world_scale;
        // Coarse grass does not preserve every deformed fine vertex. The
        // deformation amplitude is already in world units.
        if (HsEnvironmentWind(instance.Mesh) && pages[bucket].PhysicalLod != 0u)
            world_error += 2.0 * abs(GrassDeformationRadius);
        if (view == 0u)
        {
            const float distance = view_center.z - radius - world_error;
            near_bound = near_bound || distance <= LodOptions.z;
            const float rho = length(abs(view_center.xy) + radius + world_error) /
                max(distance, LodOptions.z);
            errors[bucket] = world_error * LodProjection.x *
                sqrt(1.0 + rho * rho) / max(distance, LodOptions.z);
        }
        else
        {
            errors[bucket] = world_error * LodProjection[view];
        }
    }

    const uint address = (source.InstanceIndex * 4u + view) * 32u;
    const uint epoch = range.Reserved ^ LodClock.y;
    const uint4 identity = LodStates.Load4(address);
    const uint4 temporal = LodStates.Load4(address + 16u);
    bool valid = LodClock.z != 0u && identity.x == source.InstanceGeneration &&
        identity.y == epoch && identity.z < 3u && identity.w < 3u;
    if (valid)
        valid = CanonicalLodBucket(identity.z, pages) == identity.z &&
            CanonicalLodBucket(identity.w, pages) == identity.w;

    uint desired = near_bound ? finest :
        SelectLodBucket(pages, errors, LodOptions.x, finest);
    uint current = valid ? identity.z : desired;
    uint target = valid ? identity.w : desired;
    float progress = valid ? saturate(asfloat(temporal.x)) : 1.0;
    bool invalidate_history = !valid;
    if (valid)
    {
        // The target is the cut toward which an active transition is moving.
        // Compare physical LODs because resident fallback may alias requests.
        if (!near_bound)
        {
            if (pages[desired].PhysicalLod > pages[target].PhysicalLod)
            {
                const uint coarse = SelectLodBucket(pages, errors,
                    LodOptions.x * (1.0 - LodOptions.y), finest);
                desired = pages[coarse].PhysicalLod > pages[target].PhysicalLod
                    ? coarse : target;
            }
            else if (pages[desired].PhysicalLod < pages[target].PhysicalLod &&
                     errors[target] <= LodOptions.x * (1.0 + LodOptions.y))
                desired = target;
        }

        invalidate_history = current != target;
        if (current != target && desired == current)
        {
            // Swapping endpoints and complementing the threshold preserves
            // both pages' coverage when the desired cut reverses direction.
            const uint previous_current = current;
            current = target;
            target = previous_current;
            progress = 1.0 - progress;
        }
        else if (desired != target)
        {
            // A third page cannot join the pair. Finish at its nearer endpoint
            // before starting a new two-page transition.
            if (current != target)
                current = progress < 0.5 ? current : target;
            target = desired;
            progress = 0.0;
        }
        if (current != target)
        {
            invalidate_history = true;
            if (temporal.y != LodClock.x)
                progress = saturate(progress + LodOptions.w);
            if (progress >= 1.0)
            {
                current = target;
                progress = 1.0;
            }
        }
    }

    LodStates.Store4(address,
        uint4(source.InstanceGeneration, epoch, current, target));
    LodStates.Store4(address + 16u,
        uint4(asuint(progress), LodClock.x, 0u, 0u));
    [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
    {
        selected[bucket] = source;
        selected[bucket].MeshFlags &= ~0x120000u;
        selected[bucket].LodThreshold = 1.0;
        if (invalidate_history)
            selected[bucket].MeshFlags |= 0x100000u;
    }
    if (current == target)
        return 1u << current;

    // The incoming page keeps samples below progress and the outgoing page
    // keeps its exact complement, so their union always covers one full cut.
    selected[current].MeshFlags |= 0x20000u;
    selected[current].LodThreshold = progress;
    selected[target].LodThreshold = progress;
    return (1u << current) | (1u << target);
}

bool SphereInFrustum(float3 center, float radius, float4x4 view_projection)
{
    // Row-vector clip coordinates: planes are c3 +/- c0, c3 +/- c1, c2, c3 - c2.
    const float4 c0 = float4(view_projection._11, view_projection._21,
                             view_projection._31, view_projection._41);
    const float4 c1 = float4(view_projection._12, view_projection._22,
                             view_projection._32, view_projection._42);
    const float4 c2 = float4(view_projection._13, view_projection._23,
                             view_projection._33, view_projection._43);
    const float4 c3 = float4(view_projection._14, view_projection._24,
                             view_projection._34, view_projection._44);
    const float4 planes[6] = {c3 + c0, c3 - c0, c3 + c1, c3 - c1, c2, c3 - c2};
    [unroll]
    for (uint index = 0; index < 6; ++index)
        if (dot(float4(center, 1.0), planes[index]) <
            -radius * length(planes[index].xyz))
            return false;
    return true;
}

void ResolveVisibilitySphere(InstanceData instance, StaticDrawRef draw,
                             out float3 center, out float radius)
{
    center = instance.Position.xyz;
    radius = instance.Position.w;
    if ((draw.MeshFlags & 0x40000u) != 0u)
    {
        const uint skin_slot = HsLoadGpuSceneSkin(draw.InstanceIndex).x;
        const float4 local_bound = SkinBounds[skin_slot];
        const float3 scaled = local_bound.xyz * instance.Scale.xyz;
        float sine_yaw, cosine_yaw;
        sincos(instance.Yaw, sine_yaw, cosine_yaw);
        center += float3(
            scaled.x * cosine_yaw + scaled.z * sine_yaw,
            scaled.y,
            -scaled.x * sine_yaw + scaled.z * cosine_yaw);
        radius = local_bound.w *
            max(abs(instance.Scale.x),
                max(abs(instance.Scale.y), abs(instance.Scale.z)));
    }
    if (HsEnvironmentWind(instance.Mesh))
        radius += abs(GrassDeformationRadius);
}

bool SphereOccludedByHzb(float3 center, float radius, float4x4 view_projection)
{
    // A negative deformation radius is the explicit conservative-disable mode
    // used when previous HZB history is invalid or for paths that do not yet
    // participate in occlusion history.
    if (!isfinite(GrassDeformationRadius) || GrassDeformationRadius < 0.0 ||
        !all(isfinite(center)) || !isfinite(radius) || radius < 0.0 ||
        !all(isfinite(TemporalOptions.zw))) return false;
    const float4 clip = mul(float4(center, 1.0), view_projection);
    const float3 clip_x_axis = float3(view_projection._11,
        view_projection._21, view_projection._31);
    const float3 clip_y_axis = float3(view_projection._12,
        view_projection._22, view_projection._32);
    const float3 clip_w_axis = float3(view_projection._14,
        view_projection._24, view_projection._34);
    const float w_radius = radius * length(clip_w_axis);
    const float minimum_w = clip.w - w_radius;
    if (!all(isfinite(clip)) || !isfinite(minimum_w) ||
        minimum_w <= max(TemporalOptions.w, 0.0001)) return false;
    const float2 uv = float2(clip.x / clip.w * 0.5 + 0.5,
                             0.5 - clip.y / clip.w * 0.5);
    // Bound (X+dX)/(W+dW)-X/W over the entire world-space sphere.
    // Projection diagonals alone omit camera rotation and the denominator's
    // variation, underestimating the HZB rectangle near/off the view axis.
    const float2 uv_radius = 0.5 * radius *
        (float2(length(clip_x_axis), length(clip_y_axis)) +
         abs(clip.xy / clip.w) * length(clip_w_axis)) / minimum_w;
    const float2 uv_min = uv - uv_radius;
    const float2 uv_max = uv + uv_radius;
    // Near-screen-edge bounds are uncertain and must stay visible.
    if (!all(isfinite(uv_min)) || !all(isfinite(uv_max)) ||
        any(uv_min <= 0.0) || any(uv_max >= 1.0)) return false;

    // The trailing range-sized record carries producer layout metadata without
    // increasing the already-full 64-DWORD root signature or changing record stride.
    const VisibilityRange layout = Ranges[Ranges[0].ArgumentStride];
    const uint2 extent = uint2(layout.Begin, layout.Count);
    const uint2 base_min = (uint2)floor(uv_min * float2(extent));
    const uint2 base_max = (uint2)floor(uv_max * float2(extent));
    HsHzbRegion region;
    if (!HsFindHzbRegion(extent.x, extent.y, layout.IndexCount, layout.ArgumentStride,
        base_min.x, base_min.y, base_max.x, base_max.y, region)) return false;
    const uint width = region.width, offset = region.offset;
    const uint2 minimum_coordinate = uint2(region.min_x, region.min_y);
    const uint2 maximum_coordinate = uint2(region.max_x, region.max_y);
    float region_farthest = 1.0;
    const uint2 coordinates[4] = {
        minimum_coordinate,
        uint2(maximum_coordinate.x, minimum_coordinate.y),
        uint2(minimum_coordinate.x, maximum_coordinate.y),
        maximum_coordinate};
    [unroll] for (uint sample_index = 0; sample_index < 4; ++sample_index)
    {
        const uint2 coordinate = coordinates[sample_index];
        const float depth = PreviousHzb[offset + coordinate.y * width + coordinate.x];
        if (!isfinite(depth) || depth <= 0.0 || depth > 1.0) return false;
        region_farthest = min(region_farthest, depth);
    }
    // Reversed-Z clear is zero. A region touching background cannot be a
    // conservative full occluder.
    if (region_farthest <= 0.0) return false;

    // The renderer currently stores the reverse perspective near/far pair in
    // TemporalOptions.zw. For a perspective projection clip.w is view Z, so the
    // sphere's nearest possible point is viewZ-radius. Projecting that value
    // gives a conservative maximum reversed-Z depth for the entire sphere.
    const float near_z = max(TemporalOptions.z, TemporalOptions.w + 0.0001);
    const float far_z = max(TemporalOptions.w, 0.0001);
    const float projection_a = far_z / (far_z - near_z);
    const float projection_b = -near_z * far_z / (far_z - near_z);
    const float nearest_view_z = max(minimum_w, far_z);
    const float conservative_nearest =
        saturate(projection_a + projection_b / nearest_view_z);
    return isfinite(conservative_nearest) && conservative_nearest + 0.0005 < region_farthest;
}

void StaticLodFamilyVisibility(uint owner_bucket, uint view, uint thread_id)
{
    VisibilityRange family_ranges[3];
    LodPage pages[3];
    uint output_begin[3];
    [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
    {
        family_ranges[bucket] = Ranges[owner_bucket + bucket];
        pages[bucket] = LoadLodPage(family_ranges[bucket], view);
        output_begin[bucket] = GeometryDrawRecords.Load(
            (family_ranges[bucket].DrawRecordBase + view) * 64u + 20u);
        if (thread_id == 0u)
        {
            family_visible_count[bucket] = 0u;
            family_rejected_count[bucket] = 0u;
        }
    }
    float4x4 view_projection = ViewProjection;
    if (view != 0u) view_projection = ShadowViewProjection[view - 1u];
    GroupMemoryBarrierWithGroupSync();

    const VisibilityRange input_range = family_ranges[0];
    for (uint chunk = 0; chunk < input_range.Count; chunk += 64u)
    {
        const uint local = chunk + thread_id;
        bool visible = false;
        bool rejected = false;
        uint selected_mask = 0u;
        StaticDrawRef selected[3];
        [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
            selected[bucket] = (StaticDrawRef)0;
        if (local < input_range.Count)
        {
            const StaticDrawRef draw = StaticSceneDraws[input_range.Begin + local];
            const InstanceData instance = HsLoadGpuSceneInstance(draw.InstanceIndex);
            float3 center;
            float radius;
            ResolveVisibilitySphere(instance, draw, center, radius);
            selected_mask = ResolveFamilyLod(input_range, view, pages,
                instance, center, radius, draw, selected);
            visible = SphereInFrustum(center, radius, view_projection) &&
                ((GeometryDrawRecords.Load(input_range.DrawRecordBase * 64u + 28u) & 4u) == 0u ||
                (HsHlodLeafViewMask(draw.InstanceIndex, draw.InstanceGeneration) & (1u << view)) != 0u);
            if (visible && view == 0u && GrassDeformationRadius >= 0.0)
            {
                rejected = SphereOccludedByHzb(
                    center, radius, PreviousViewProjection);
                visible = !rejected;
            }
        }
        [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
        {
            const bool selected_bucket = (selected_mask & (1u << bucket)) != 0u;
            family_visible_flags[bucket][thread_id] = visible && selected_bucket ? 1u : 0u;
            family_rejected_flags[bucket][thread_id] = rejected && selected_bucket ? 1u : 0u;
        }
        GroupMemoryBarrierWithGroupSync();

        [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
        {
            uint visible_prefix = 0u;
            uint rejected_prefix = 0u;
            for (uint preceding = 0; preceding < thread_id; ++preceding)
            {
                visible_prefix += family_visible_flags[bucket][preceding];
                rejected_prefix += family_rejected_flags[bucket][preceding];
            }
            if (family_visible_flags[bucket][thread_id] != 0u)
                VisibleDraws[output_begin[bucket] +
                    family_visible_count[bucket] + visible_prefix] = selected[bucket];
            if (family_rejected_flags[bucket][thread_id] != 0u)
                RejectedDraws[output_begin[bucket] +
                    family_rejected_count[bucket] + rejected_prefix] = selected[bucket];
        }
        GroupMemoryBarrierWithGroupSync();
        if (thread_id == 0u)
        {
            [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
            {
                uint visible_chunk_count = 0u;
                uint rejected_chunk_count = 0u;
                [unroll] for (uint lane = 0; lane < 64u; ++lane)
                {
                    visible_chunk_count += family_visible_flags[bucket][lane];
                    rejected_chunk_count += family_rejected_flags[bucket][lane];
                }
                family_visible_count[bucket] += visible_chunk_count;
                family_rejected_count[bucket] += rejected_chunk_count;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread_id == 0u)
    {
        // The owner writes all three commands, including unused aliases and
        // empty families. No stale command can survive a changed resident cut.
        [unroll] for (uint bucket = 0; bucket < 3u; ++bucket)
        {
            StoreGeometryArguments(family_ranges[bucket], owner_bucket + bucket,
                view, family_visible_count[bucket]);
            if (view == 0u &&
                (GeometryDrawRecords.Load(family_ranges[bucket].DrawRecordBase * 64u + 28u) & 4u) != 0u)
            {
                RejectedArgs.Store4((owner_bucket + bucket) * 24u,
                    uint4(0u, family_ranges[bucket].IndexCount,
                        family_rejected_count[bucket], 0u));
                RejectedArgs.Store2((owner_bucket + bucket) * 24u + 16u,
                    uint2(0u, 0u));
            }
        }
    }
}

[numthreads(64, 1, 1)]
void StaticVisibilityCS(uint3 group_id : SV_GroupID, uint thread_id : SV_GroupIndex)
{
    const VisibilityRange range = Ranges[group_id.x];
    if ((range.Flags & 4u) != 0u)
    {
        // Family input spans are shared; only the first bucket may consume
        // them. This branch and return are uniform across the entire group.
        if (group_id.x % 3u != 0u) return;
        StaticLodFamilyVisibility(group_id.x, group_id.y, thread_id);
        return;
    }
    // Every light cascade owns its own compaction output. Camera visibility
    // cannot reject off-camera geometry that can cast into a light's view.
    float4x4 view_projection = ViewProjection;
    if (group_id.y != 0) view_projection = ShadowViewProjection[group_id.y - 1];
    const uint output_begin = GeometryDrawRecords.Load(
        (range.DrawRecordBase + group_id.y) * 64u + 20u);
    if (thread_id == 0) visible_count = 0;
    if (thread_id == 0) rejected_count = 0;
    GroupMemoryBarrierWithGroupSync();

    // One group owns one bucket. Sequential chunks keep the input order stable.
    for (uint chunk = 0; chunk < range.Count; chunk += 64)
    {
        const uint local = chunk + thread_id;
        bool visible = false;
        bool rejected = false;
        StaticDrawRef draw = (StaticDrawRef)0;
        if (local < range.Count)
        {
            draw = StaticSceneDraws[range.Begin + local];
            const InstanceData instance = HsLoadGpuSceneInstance(draw.InstanceIndex);
            float3 center;
            float radius;
            ResolveVisibilitySphere(instance, draw, center, radius);
            visible = SphereInFrustum(center, radius, view_projection) &&
                ((GeometryDrawRecords.Load(range.DrawRecordBase * 64u + 28u) & 4u) == 0u ||
                (HsHlodLeafViewMask(draw.InstanceIndex, draw.InstanceGeneration) & (1u << group_id.y)) != 0u);
            if (visible && group_id.y == 0 && GrassDeformationRadius >= 0.0)
            {
                rejected = SphereOccludedByHzb(
                    center, radius, PreviousViewProjection);
                visible = !rejected;
            }
        }
        visible_flags[thread_id] = visible ? 1u : 0u;
        rejected_flags[thread_id] = rejected ? 1u : 0u;
        GroupMemoryBarrierWithGroupSync();
        uint visible_prefix = 0;
        uint rejected_prefix = 0;
        for (uint preceding = 0; preceding < thread_id; ++preceding)
        {
            visible_prefix += visible_flags[preceding];
            rejected_prefix += rejected_flags[preceding];
        }
        if (visible)
            VisibleDraws[output_begin + visible_count + visible_prefix] = draw;
        if (rejected)
            RejectedDraws[output_begin + rejected_count + rejected_prefix] = draw;
        GroupMemoryBarrierWithGroupSync();
        if (thread_id == 0)
        {
            uint chunk_count = 0;
            uint rejected_chunk_count = 0;
            [unroll]
            for (uint index = 0; index < 64; ++index)
            {
                chunk_count += visible_flags[index];
                rejected_chunk_count += rejected_flags[index];
            }
            visible_count += chunk_count;
            rejected_count += rejected_chunk_count;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread_id == 0)
    {
        StoreGeometryArguments(range, group_id.x, group_id.y, visible_count);
        if (group_id.y == 0)
        {
            if ((GeometryDrawRecords.Load(range.DrawRecordBase * 64u + 28u) & 4u) != 0u)
            {
                // Retest candidates keep their count at byte 8, independent
                // of the native command type chosen for the raster output.
                RejectedArgs.Store4(group_id.x * 24u,
                    uint4(0u, range.IndexCount, rejected_count, 0u));
                RejectedArgs.Store2(group_id.x * 24u + 16u, uint2(0u, 0u));
            }
        }
    }
}

[numthreads(64, 1, 1)]
void VisibilityRetestCS(uint3 group_id : SV_GroupID, uint thread_id : SV_GroupIndex)
{
    const VisibilityRange range = Ranges[group_id.x];
    const uint candidate_count = RejectedArgs.Load(group_id.x * 24u + 8u);
    const uint output_begin = GeometryDrawRecords.Load(range.DrawRecordBase * 64u + 20u);
    if (thread_id == 0) visible_count = 0;
    GroupMemoryBarrierWithGroupSync();

    for (uint chunk = 0; chunk < candidate_count; chunk += 64)
    {
        const uint local = chunk + thread_id;
        bool visible = false;
        StaticDrawRef draw = (StaticDrawRef)0;
        if (local < candidate_count)
        {
            draw = StaticSceneDraws[range.Begin + local];
            const InstanceData instance = HsLoadGpuSceneInstance(draw.InstanceIndex);
            float3 center;
            float radius;
            ResolveVisibilitySphere(instance, draw, center, radius);
            visible = ((GeometryDrawRecords.Load(range.DrawRecordBase * 64u + 28u) & 4u) == 0u ||
                (HsHlodLeafViewMask(draw.InstanceIndex, draw.InstanceGeneration) & 1u) != 0u) &&
                SphereInFrustum(center, radius, ViewProjection) &&
                !SphereOccludedByHzb(center, radius, ViewProjection);
        }
        visible_flags[thread_id] = visible ? 1u : 0u;
        GroupMemoryBarrierWithGroupSync();
        uint prefix = 0;
        for (uint preceding = 0; preceding < thread_id; ++preceding)
            prefix += visible_flags[preceding];
        if (visible)
            VisibleDraws[output_begin + visible_count + prefix] = draw;
        GroupMemoryBarrierWithGroupSync();
        if (thread_id == 0)
        {
            uint chunk_count = 0;
            [unroll] for (uint index = 0; index < 64; ++index)
                chunk_count += visible_flags[index];
            visible_count += chunk_count;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread_id == 0)
    {
        StoreGeometryArguments(range, group_id.x, 0u, visible_count);
    }
}
