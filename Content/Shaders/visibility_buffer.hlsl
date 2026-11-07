#include "project_hs.hlsl"
#include "geometry.hlsli"
#include "triangle_sample.hlsli"

// Visibility resources use their own table so the raster and resolve passes
// can share the scene root signature without disturbing the existing GBuffer.
ByteAddressBuffer HsRasterRecords : register(t0, space7);
StructuredBuffer<uint4> HsRasterRanges : register(t1, space7);
ByteAddressBuffer HsStaticScene : register(t2, space7);
ByteAddressBuffer HsDynamicScene : register(t3, space7);
StructuredBuffer<StaticDrawRef> HsStaticVisibleRefs : register(t4, space7);
StructuredBuffer<StaticDrawRef> HsDynamicVisibleRefs : register(t5, space7);
StructuredBuffer<StaticDrawRef> HsStaticRetestRefs : register(t6, space7);
StructuredBuffer<StaticDrawRef> HsStaticInputRefs : register(t7, space7);
StructuredBuffer<StaticDrawRef> HsDynamicInputRefs : register(t8, space7);
Texture2D<uint2> HsVisibility : register(t9, space7);
Texture2D<uint> HsFrontFace : register(t10, space7);
ByteAddressBuffer HsClassifiedPixels : register(t11, space7);
ByteAddressBuffer HsVariantCounts : register(t12, space7);
StructuredBuffer<StaticDrawRef> HsClusterRefs : register(t13, space7);

RWByteAddressBuffer HsRasterRecordsWrite : register(u0, space7);
RWTexture2D<float4> HsSurfaceBaseWrite : register(u1, space7);
RWTexture2D<float2> HsSurfaceNormalWrite : register(u2, space7);
RWTexture2D<uint> HsSurfaceMetaWrite : register(u3, space7);
RWTexture2D<float4> HsSurfaceMaterialWrite : register(u4, space7);
RWTexture2D<float2> HsSurfaceMotionWrite : register(u5, space7);
RWTexture2D<float2> HsSurfaceHistoryWrite : register(u6, space7);
RWTexture2D<uint2> HsVisibilityWrite : register(u7, space7);
RWTexture2D<uint> HsFrontFaceWrite : register(u8, space7);
RWByteAddressBuffer HsClassifiedPixelsWrite : register(u9, space7);
RWByteAddressBuffer HsVariantCountsWrite : register(u10, space7);
RWByteAddressBuffer HsDispatchArgsWrite : register(u11, space7);
RWTexture2D<uint4> HsSurfaceSidecar0Write : register(u12, space7);
RWTexture2D<uint4> HsSurfaceSidecar1Write : register(u13, space7);

#include "visibility_records.hlsli"

StaticDrawRef HsLoadRasterRef(uint source, uint index)
{
    StaticDrawRef draw = (StaticDrawRef)0;
    if (source == 1u)
        draw = HsStaticVisibleRefs[index];
    else if (source == 2u)
        draw = HsDynamicVisibleRefs[index];
    else if (source == 3u)
        draw = HsStaticRetestRefs[index];
    else if (source == 4u)
        draw = HsStaticInputRefs[index];
    else if (source == 5u)
        draw = HsDynamicInputRefs[index];
    else if (source == 6u)
        draw = HsClusterRefs[index];
    return draw;
}

bool HsRasterSourceIsStatic(uint source)
{
    return source == 1u || source == 3u || source == 4u || source == 6u;
}

HsRasterRange HsLoadRasterRange(uint rangeIndex)
{
    const uint4 words = HsRasterRanges[rangeIndex];
    HsRasterRange range;
    range.geometryRecordId = words.x;
    range.rasterBase = words.y;
    range.instanceCapacity = words.z;
    range.materialWord = words.w;
    return range;
}

uint HsRasterRefIndex(GeometryDrawRecord geometry, uint local)
{
    // Every ref SRV is rooted at the beginning of its complete source array;
    // Geometry.VisibleBase selects the draw's span within that array.
    return geometry.VisibleBase + local;
}

bool HsRasterRefIndexValid(GeometryDrawRecord geometry, HsRasterRecord record)
{
    return record.visibleRefIndex >= geometry.VisibleBase &&
        record.visibleRefIndex - geometry.VisibleBase < geometry.InstanceCount;
}

uint HsRasterVariantForGeometry(GeometryDrawRecord geometry)
{
    // Geometry families keep their page ordinals; authored terrain uses variant 15.
    if ((geometry.Flags & 16u) != 0u && geometry.MeshId == 9u) return 15u;
    if ((geometry.Flags & 32u) != 0u) return 1u + (geometry.Lod & 0xffffu);
    return (geometry.PageSlot < HsEnvironmentGeometryPageCount && (geometry.Flags & 16u) == 0u)
        ? 1u + HsEnvironmentMaterialOrdinal(geometry.PageSlot / HsEnvironmentGeometryLodCount) : 0u;
}

[numthreads(64, 1, 1)]
void BuildRasterRecordsCS(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const HsRasterRange range = HsLoadRasterRange(PassValue);
    const GeometryDrawRecord geometry =
        LoadGeometryDrawRecord(range.geometryRecordId);
    const uint local = HsVisibilityGroupOrdinal(groupId) * 64u + lane;
    if (local >= geometry.InstanceCount || local >= range.instanceCapacity)
        return;
    uint rasterRecordBytes;
    HsRasterRecordsWrite.GetDimensions(rasterRecordBytes);
    if (range.rasterBase > rasterRecordBytes / 32u ||
        local >= rasterRecordBytes / 32u - range.rasterBase)
        return;

    const uint source = geometry.Reserved1;
    if (source < 1u || source > 6u)
        return;
    const uint visibleRefIndex = HsRasterRefIndex(geometry, local);
    const StaticDrawRef draw = HsLoadRasterRef(source, visibleRefIndex);
    HsRasterRecord record;
    record.geometryRecordId = range.geometryRecordId;
    record.visibleRefIndex = visibleRefIndex;
    record.refSource = source;
    record.materialWord = range.materialWord;
    record.instanceIndex = draw.InstanceIndex;
    record.instanceGeneration = draw.InstanceGeneration;
    record.pageGeneration = geometry.GeometryGeneration;
    record.flags = HsRasterVariantForGeometry(geometry);
    HsStoreRasterRecord(HsRasterRecordsWrite,
                        range.rasterBase + local, record);
}

[numthreads(8, 8, 1)]
void ClearVisibilityCS(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID,
                      uint groupIndex : SV_GroupIndex)
{
    const uint2 pixel = groupId.xy * 8u + groupThreadId.xy;
    const uint width = (uint)ScreenSize.x;
    const uint height = (uint)ScreenSize.y;
    if (groupId.x == 0u && groupId.y == 0u && groupIndex == 0u)
    {
        [unroll]
        for (uint index = 0u; index < HsVisibilityCursorBase +
                                      HsVisibilityVariantCount; ++index)
            HsVariantCountsWrite.Store(index * 4u, 0u);
    }
    if (pixel.x >= width || pixel.y >= height)
        return;

    HsVisibilityWrite[pixel] = uint2(HsVisibilityInvalid, HsVisibilityInvalid);
    HsFrontFaceWrite[pixel] = 0u;
    HsSurfaceBaseWrite[pixel] = 0.0;
    HsSurfaceNormalWrite[pixel] = 0.0;
    HsSurfaceMetaWrite[pixel] = 0u;
    HsSurfaceMaterialWrite[pixel] = 0.0;
    HsSurfaceMotionWrite[pixel] = 0.0;
    HsSurfaceHistoryWrite[pixel] = 0.0;
    if (MaterialSidecarInfo.z > 0u)
        HsSurfaceSidecar0Write[pixel] = 0u;
    if (MaterialSidecarInfo.z > 1u)
        HsSurfaceSidecar1Write[pixel] = 0u;
}

struct HsVisibilityOutput
{
    uint2 visibility : SV_Target0;
    uint frontFace : SV_Target1;
};

HsVisibilityOutput HsVisibilityPSImpl(SceneOutput input, bool front,
                                      bool environmentOpaque,
                                      uint primitiveId)
{
    HsVisibilityOutput output = (HsVisibilityOutput)0;
    const uint rasterId = input.RasterDrawRecordId;
    if (!HsRasterIdValid(rasterId) || rasterId == 0u || PassValue < 4u)
        clip(-1.0);
    const HsSceneGradients gradients = RasterSceneGradients(input);
#ifdef HS_GENERATED_CHARACTER_MATERIAL
    if (!HsCharacterSceneCovered(input, front, gradients))
#else
    if (!HsSceneCovered(input, front, environmentOpaque, gradients, SceneDrawMaterial))
#endif
        clip(-1.0);

    const GeometryDrawRecord geometry =
        LoadGeometryDrawRecord(PassValue >> 2u);
    if (geometry.PageSlot >= HsGeometryPageCapacity ||
        geometry.IndexCount == 0u)
        clip(-1.0);
    const bool meshPrimitive = (geometry.Flags & 2u) != 0u;
    uint canonicalPrimitive = meshPrimitive
        ? primitiveId : geometry.FirstIndex / 3u + primitiveId;
    if (canonicalPrimitive < geometry.FirstIndex / 3u ||
        canonicalPrimitive - geometry.FirstIndex / 3u >= geometry.IndexCount / 3u)
        clip(-1.0);
    if (!meshPrimitive && (geometry.Flags & 64u) != 0u)
    {
        canonicalPrimitive = GeometryBuffers[NonUniformResourceIndex(1u + geometry.PageSlot * 4u + 3u)].Load(
            geometry.RemapOffset + canonicalPrimitive * 4u);
        if (canonicalPrimitive < geometry.FirstIndex / 3u ||
            canonicalPrimitive - geometry.FirstIndex / 3u >= geometry.IndexCount / 3u)
            clip(-1.0);
    }
    output.visibility = uint2(rasterId, canonicalPrimitive);
    output.frontFace = front ? 1u : 0u;
    return output;
}

HsVisibilityOutput VisibilityPS(SceneOutput input, bool front : SV_IsFrontFace,
                                uint primitiveId : SV_PrimitiveID)
{
    return HsVisibilityPSImpl(input, front, false, primitiveId);
}

HsVisibilityOutput EnvironmentOpaqueVisibilityPS(
    SceneOutput input, bool front : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID)
{
    return HsVisibilityPSImpl(input, front, true, primitiveId);
}

HsVisibilityOutput EnvironmentMaskedVisibilityPS(
    SceneOutput input, bool front : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID)
{
    return HsVisibilityPSImpl(input, front, false, primitiveId);
}

#ifdef HS_GENERATED_CHARACTER_MATERIAL
HsVisibilityOutput CharacterVisibilityPS(SceneOutput input, bool front : SV_IsFrontFace,
    uint primitiveId : SV_PrimitiveID)
{
    return HsVisibilityPSImpl(input, front, false, primitiveId);
}
#endif

#include "visibility_classification.hlsli"

HsSceneVertexContext HsResolveVertexContext(HsRasterRecord record,
                                            GeometryDrawRecord geometry,
                                            StaticDrawRef draw,
                                            uint rasterRecordId)
{
    HsSceneVertexContext context = (HsSceneVertexContext)0;
    context.draw = draw;
    context.instanceKey = uint2(draw.InstanceIndex + 1u, draw.InstanceGeneration);
    context.skinSlot = 0xffffffffu;
    context.gpuSceneRefs = true;
#ifdef HS_TERRAIN_VISIBILITY_RESOLVE
    context.cachedSkin = false;
    context.cachedSkinHistory = false;
#else
    context.cachedSkin = (draw.MeshFlags & 0x40000u) != 0u;
    context.cachedSkinHistory = (draw.MeshFlags & 0x80000u) != 0u;
#endif
    const bool staticSource = HsRasterSourceIsStatic(record.refSource);
    if (staticSource)
        context.instance = HsLoadGpuSceneInstance(HsStaticScene, draw.InstanceIndex);
    else
        context.instance = HsLoadGpuSceneInstance(HsDynamicScene, draw.InstanceIndex);
#ifndef HS_TERRAIN_VISIBILITY_RESOLVE
    if (context.cachedSkin)
    {
        uint4 skin;
        if (staticSource)
            skin = HsLoadGpuSceneSkin(HsStaticScene, draw.InstanceIndex);
        else
            skin = HsLoadGpuSceneSkin(draw.InstanceIndex);
        context.skinSlot = skin.x;
        context.skinVertexOffsets = skin.zw;
        context.skinVertexStrides = staticSource
            ? HsLoadGpuSceneSkinStrides(HsStaticScene, draw.InstanceIndex)
            : HsLoadGpuSceneSkinStrides(draw.InstanceIndex);
    }
#endif
    context.instance.Mesh |= draw.MeshFlags;
    const uint resolvedMesh = context.instance.Mesh & 0xffu;
    if (resolvedMesh >= 10u && resolvedMesh <= 15u)
        context.instance.Padding = draw.LodThreshold;
    else
        context.instance.Scale.w = draw.LodThreshold;
    context.geometry = geometry;
    context.geometryValid = true;
    context.rasterRecordId = rasterRecordId;
    return context;
}

bool HsLoadResolveTriangle(uint2 pixel, uint2 visibility,
                           HsRasterRecord record, GeometryDrawRecord geometry,
                           StaticDrawRef draw, uint expectedVariant,
                           out SceneOutput vertex0,
                           out SceneOutput vertex1, out SceneOutput vertex2,
                           out HsTriangleSample sample)
{
    vertex0 = (SceneOutput)0;
    vertex1 = (SceneOutput)0;
    vertex2 = (SceneOutput)0;
    sample = (HsTriangleSample)0;
    uint4 sceneHeader0, sceneHeader1, sceneHeader2, sceneHeader3;
    bool sceneSlotValid;
    if (HsRasterSourceIsStatic(record.refSource))
        sceneSlotValid = HsGpuSceneSlotValid(HsStaticScene, draw.InstanceIndex,
                                             sceneHeader0, sceneHeader1,
                                             sceneHeader2, sceneHeader3);
    else
        sceneSlotValid = HsGpuSceneSlotValid(HsDynamicScene, draw.InstanceIndex,
                                             sceneHeader0, sceneHeader1,
                                             sceneHeader2, sceneHeader3);
    if (!sceneSlotValid)
        return false;

    const uint descriptors = 1u + geometry.PageSlot * 4u;
    const uint indexByteOffset = visibility.y * 12u;
    const uint3 indices = GeometryBuffers[NonUniformResourceIndex(descriptors + 1u)]
        .Load3(indexByteOffset);
    const SceneInput input0 = LoadGeometryVertex(descriptors, indices.x);
    uint classifiedVariant;
    if (!HsClassifiedMaterialVariant(record, geometry, input0.Material, classifiedVariant) ||
        classifiedVariant != expectedVariant)
        return false;
    const SceneInput input1 = LoadGeometryVertex(descriptors, indices.y);
    const SceneInput input2 = LoadGeometryVertex(descriptors, indices.z);
    const HsSceneVertexContext context =
        HsResolveVertexContext(record, geometry, draw, visibility.x);
    vertex0 = EvaluateSceneVertex(input0, context, indices.x);
    vertex1 = EvaluateSceneVertex(input1, context, indices.y);
    vertex2 = EvaluateSceneVertex(input2, context, indices.z);
    const float2 extent = ScreenSize.xy;
    const float2 pixelCenter = (float2)pixel + 0.5;
    const float2 sampleNdc = float2(
        pixelCenter.x * (2.0 / extent.x) - 1.0,
        1.0 - pixelCenter.y * (2.0 / extent.y));
    sample = HsSampleTriangle(vertex0.Position, vertex1.Position,
                              vertex2.Position, sampleNdc, extent);
    return sample.valid;
}

float3 HsInterpolate3(float3 a, float3 b, float3 c, float3 barycentric)
{
    return a * barycentric.x + b * barycentric.y + c * barycentric.z;
}

float2 HsInterpolate2(float2 a, float2 b, float2 c, float3 barycentric)
{
    return a * barycentric.x + b * barycentric.y + c * barycentric.z;
}

float3 HsGradient3(float3 a, float3 b, float3 c, float3 derivative)
{
    return a * derivative.x + b * derivative.y + c * derivative.z;
}

float2 HsGradient2(float2 a, float2 b, float2 c, float3 derivative)
{
    return a * derivative.x + b * derivative.y + c * derivative.z;
}

static const uint HsResolveUv = 1u;
static const uint HsResolveColor = 2u;
static const uint HsResolveWorld = 4u;
static const uint HsResolveStable = 8u;
static const uint HsResolveWorldGradients = 16u;
static const uint HsResolveStableGradients = 32u;

uint HsResolveVaryingDemand(uint variant, uint mesh)
{
#if defined(HS_GENERATED_CHARACTER_MATERIAL) || defined(HS_GENERATED_ENVIRONMENT_MATERIAL)
#ifdef HS_GENERATED_CHARACTER_MATERIAL
    const uint mask = HsCharacterSurfaceContextDemand(variant - HsCharacterVisibilityVariantBegin) |
        HsContextShadingNormal | HsContextTangent;
    const uint sampled = HsContextSampledBaseColor | HsContextSampledRoughness |
        HsContextSampledAlpha | HsContextSampledAo | HsContextSampledMetallic |
        HsContextSampledEmission | HsContextShadingNormal;
#else
    const uint mask = HsEnvironmentSurfaceContextDemand();
    const uint sampled = 0u;
#endif
    uint demand = 0u;
    if ((mask & (HsContextUv | HsContextDUvDx | HsContextDUvDy | sampled)) != 0u)
        demand |= HsResolveUv;
    if ((mask & (HsContextActualWorldPosition | HsContextEyeDistance | HsContextViewDirection)) != 0u)
        demand |= HsResolveWorld;
    if ((mask & (HsContextDActualWorldDx | HsContextDActualWorldDy)) != 0u)
        demand |= HsResolveWorldGradients;
#ifdef HS_GENERATED_CHARACTER_MATERIAL
    // Character stable coordinates use the same deformed world basis.
    if ((mask & HsContextStableWorldPosition) != 0u)
        demand |= HsResolveWorld;
    if ((mask & (HsContextDStableWorldDx | HsContextDStableWorldDy)) != 0u)
        demand |= HsResolveWorldGradients;
#else
    if ((mask & HsContextSampledBaseColor) != 0u)
        demand |= HsResolveColor;
    if ((mask & HsContextStableWorldPosition) != 0u || mesh == 19u)
        demand |= HsResolveStable;
    if ((mask & (HsContextDStableWorldDx | HsContextDStableWorldDy)) != 0u)
        demand |= HsResolveStableGradients;
    if ((LodClock.w & 1u) != 0u && (mesh == 9u || mesh == 20u))
        demand |= HsResolveStable | HsResolveStableGradients;
#endif
    return demand;
#else
    return 63u;
#endif
}

SceneOutput HsInterpolateSceneOutput(SceneOutput a, SceneOutput b, SceneOutput c,
                                     HsTriangleSample sample, uint2 pixel, uint demand)
{
    SceneOutput output = (SceneOutput)0;
    output.Position = float4((float2)pixel + 0.5,
        SurfaceDepth.Load(int3(pixel, 0)), rcp(sample.clipW));
    output.WorldNormal = normalize(HsInterpolate3(a.WorldNormal, b.WorldNormal,
                                                   c.WorldNormal, sample.barycentric));
    if ((demand & HsResolveWorld) != 0u)
        output.WorldPosition = HsInterpolate3(a.WorldPosition, b.WorldPosition,
                                               c.WorldPosition, sample.barycentric);
    output.PreviousUnjitteredClipPosition = a.PreviousUnjitteredClipPosition * sample.barycentric.x +
        b.PreviousUnjitteredClipPosition * sample.barycentric.y + c.PreviousUnjitteredClipPosition * sample.barycentric.z;
    if ((demand & HsResolveColor) != 0u)
        output.Color = a.Color * sample.barycentric.x + b.Color * sample.barycentric.y +
                       c.Color * sample.barycentric.z;
    if ((demand & HsResolveUv) != 0u)
        output.Uv = HsInterpolate2(a.Uv, b.Uv, c.Uv, sample.barycentric);
    output.WorldTangent = a.WorldTangent * sample.barycentric.x +
                          b.WorldTangent * sample.barycentric.y +
                          c.WorldTangent * sample.barycentric.z;
    if ((demand & HsResolveStable) != 0u)
        output.EnvironmentStableWorld = HsInterpolate3(a.EnvironmentStableWorld,
            b.EnvironmentStableWorld, c.EnvironmentStableWorld, sample.barycentric);
    output.Mesh = a.Mesh;
    output.Material = a.Material;
    output.Charge = a.Charge;
    output.EnvironmentSeed = a.EnvironmentSeed;
    output.EnvironmentFade = a.EnvironmentFade;
    output.EnvironmentFlags = a.EnvironmentFlags;
    output.AnimationClip = a.AnimationClip;
    output.AnimationProgress = a.AnimationProgress;
    output.MaterialId = a.MaterialId;
    output.SurfaceFlags = a.SurfaceFlags;
    output.HistoryConfidence = a.HistoryConfidence;
    output.RasterDrawRecordId = a.RasterDrawRecordId;
    output.InstanceKey = a.InstanceKey;
    output.OpticalComponentId = a.OpticalComponentId;
    return output;
}

HsSceneGradients HsInterpolateSceneGradients(SceneOutput a, SceneOutput b,
                                             SceneOutput c,
                                             HsTriangleSample sample, uint demand)
{
    HsSceneGradients gradients = (HsSceneGradients)0;
    if ((demand & HsResolveUv) != 0u)
    {
        gradients.uvDx = HsGradient2(a.Uv, b.Uv, c.Uv, sample.dx);
        gradients.uvDy = HsGradient2(a.Uv, b.Uv, c.Uv, sample.dy);
    }
    if ((demand & HsResolveWorldGradients) != 0u)
    {
        gradients.worldDx = HsGradient3(a.WorldPosition, b.WorldPosition,
                                         c.WorldPosition, sample.dx);
        gradients.worldDy = HsGradient3(a.WorldPosition, b.WorldPosition,
                                         c.WorldPosition, sample.dy);
    }
    if ((demand & HsResolveStableGradients) != 0u)
    {
        gradients.stableDx = HsGradient3(a.EnvironmentStableWorld,
                                         b.EnvironmentStableWorld,
                                         c.EnvironmentStableWorld, sample.dx);
        gradients.stableDy = HsGradient3(a.EnvironmentStableWorld,
                                         b.EnvironmentStableWorld,
                                         c.EnvironmentStableWorld, sample.dy);
    }
    return gradients;
}

void HsSurfaceResolveImpl(uint3 groupId, uint lane)
{
    const uint variant = PassValue;
    if (variant >= HsVisibilityVariantCount)
        return;
    const uint count = HsVariantCounts.Load(variant * 4u);
    const uint local = HsVisibilityGroupOrdinal(groupId) * 64u + lane;
    if (local >= count)
        return;
    const uint offset = HsVariantCounts.Load(
        (HsVisibilityOffsetBase + variant) * 4u);
    const uint linearPixel = HsClassifiedPixels.Load((offset + local) * 4u);
    const uint width = (uint)ScreenSize.x;
    const uint height = (uint)ScreenSize.y;
    if (width == 0u || height == 0u || linearPixel >= width * height)
        return;
    const uint2 pixel = uint2(linearPixel % width, linearPixel / width);
    const uint2 visibility = HsVisibility[pixel];
    if (!HsRasterIdValid(visibility.x) || visibility.y == HsVisibilityInvalid)
        return;
    const HsRasterRecord record = HsLoadRasterRecord(HsRasterRecords, visibility.x);
    if (!HsRasterRecordDomainValid(record))
        return;
    const GeometryDrawRecord geometry =
        LoadGeometryDrawRecord(record.geometryRecordId);
    if (geometry.PageSlot >= HsGeometryPageCapacity || geometry.IndexCount == 0u ||
        record.pageGeneration != geometry.GeometryGeneration ||
        visibility.y < geometry.FirstIndex / 3u ||
        visibility.y - geometry.FirstIndex / 3u >= geometry.IndexCount / 3u ||
        !HsRasterRefIndexValid(geometry, record))
        return;
    const StaticDrawRef draw =
        HsLoadRasterRef(record.refSource, record.visibleRefIndex);
    if (draw.InstanceIndex != record.instanceIndex ||
        draw.InstanceGeneration != record.instanceGeneration)
        return;
    SceneOutput vertex0, vertex1, vertex2;
    HsTriangleSample sample;
    if (!HsLoadResolveTriangle(pixel, visibility, record, geometry, draw, variant,
                               vertex0, vertex1, vertex2, sample))
        return;
    const uint demand = HsResolveVaryingDemand(variant, vertex0.Mesh);
    SceneOutput input = HsInterpolateSceneOutput(vertex0, vertex1, vertex2,
                                                 sample, pixel, demand);
    const HsSceneGradients gradients = HsInterpolateSceneGradients(
        vertex0, vertex1, vertex2, sample, demand);
    const bool front = HsFrontFace[pixel] != 0u;
#ifdef HS_GENERATED_CHARACTER_MATERIAL
    if (variant < HsCharacterVisibilityVariantBegin || !HsCharacterOpaqueOrdinalValid(variant - HsCharacterVisibilityVariantBegin) ||
        input.MaterialId != HsCharacterMaterialIdBase + variant - HsCharacterVisibilityVariantBegin) return;
    const GBufferOutput surface = HsCharacterEvaluateSceneSurface(input, front, gradients);
#else
    if (variant >= HsCharacterVisibilityVariantBegin) return;
    const GBufferOutput surface = HsEvaluateSceneSurface(input, front, false, gradients, record.materialWord);
#endif
    HsSurfaceBaseWrite[pixel] = surface.BaseColor;
    HsSurfaceNormalWrite[pixel] = surface.Normal;
    HsSurfaceMetaWrite[pixel] = surface.Metadata;
    HsSurfaceMaterialWrite[pixel] = surface.Material;
    HsSurfaceMotionWrite[pixel] = surface.Motion;
    HsSurfaceHistoryWrite[pixel] = surface.History;
#if HS_SURFACE_SIDECAR_PLANES > 0
    HsSurfaceSidecar0Write[pixel] = surface.Sidecar0;
#endif
#if HS_SURFACE_SIDECAR_PLANES > 1
    HsSurfaceSidecar1Write[pixel] = surface.Sidecar1;
#endif
}

[numthreads(64, 1, 1)]
void SurfaceResolveCS(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    HsSurfaceResolveImpl(groupId, lane);
}

#ifdef HS_GENERATED_CHARACTER_MATERIAL
[numthreads(64, 1, 1)]
void CharacterSurfaceResolveCS(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    HsSurfaceResolveImpl(groupId, lane);
}
#endif
