#ifndef HS_VISIBILITY_CLASSIFICATION_HLSLI
#define HS_VISIBILITY_CLASSIFICATION_HLSLI

bool HsClassifiedVariant(uint2 visibility, HsRasterRecord record,
                         GeometryDrawRecord geometry, out uint variant)
{
    variant = HsRasterVariant(record);
    if ((geometry.Flags & 16u) != 0u || HsHeroAssetIndex(geometry.MeshId) == HsHeroInvalidOrdinal)
        return variant < HsHeroVisibilityVariantBegin;
    // Archer's FBX slots share one draw. Classify each canonical primitive by
    // its provoking vertex instead of a draw-wide material/variant guess.
    if (geometry.PageSlot >= HsGeometryPageCapacity || geometry.IndexCount == 0u ||
        record.pageGeneration != geometry.GeometryGeneration ||
        visibility.y < geometry.FirstIndex / 3u ||
        visibility.y - geometry.FirstIndex / 3u >= geometry.IndexCount / 3u)
        return false;
    const uint descriptors = 1u + geometry.PageSlot * 4u;
    const uint vertex = GeometryBuffers[NonUniformResourceIndex(descriptors + 1u)].Load(visibility.y * 12u);
    const uint slot = LoadGeometryVertex(descriptors, vertex).Material;
    const uint ordinal = HsHeroBindingOrdinals(geometry.MeshId, slot).x;
    if (!HsHeroOpaqueOrdinalValid(ordinal)) return false;
    variant = HsHeroVisibilityVariantBegin + ordinal;
    return true;
}

#endif
