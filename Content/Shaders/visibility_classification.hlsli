#ifndef HS_VISIBILITY_CLASSIFICATION_HLSLI
#define HS_VISIBILITY_CLASSIFICATION_HLSLI

bool HsClassifiedMaterialVariant(HsRasterRecord record,
                                  GeometryDrawRecord geometry, uint slot,
                                  out uint variant, out uint2 material_bindings)
{
    variant = HsRasterVariant(record);
    material_bindings = HsMeshMaterialOrdinals(geometry.MaterialBindingSet, slot);
    if ((geometry.Flags & 16u) != 0u || geometry.MaterialBindingSet == 0u)
        return variant < HsCharacterVisibilityVariantBegin;
    const uint ordinal = material_bindings.x;
    if (!HsCharacterOpaqueOrdinalValid(ordinal)) return false;
    variant = HsCharacterVisibilityVariantBegin + ordinal;
    return true;
}

bool HsClassifiedVariant(uint2 visibility, HsRasterRecord record,
                         GeometryDrawRecord geometry, out uint variant)
{
    variant = HsRasterVariant(record);
    if ((geometry.Flags & 16u) != 0u || geometry.MaterialBindingSet == 0u)
        return variant < HsCharacterVisibilityVariantBegin;
    // Multiple material slots share one draw. Classify each canonical primitive by
    // its provoking vertex instead of a draw-wide material/variant guess.
    if (geometry.PageSlot >= HsGeometryPageCapacity || geometry.IndexCount == 0u ||
        record.pageGeneration != geometry.GeometryGeneration ||
        visibility.y < geometry.FirstIndex / 3u ||
        visibility.y - geometry.FirstIndex / 3u >= geometry.IndexCount / 3u)
        return false;
    const uint descriptors = 1u + geometry.PageSlot * 4u;
    const uint vertex = GeometryBuffers[NonUniformResourceIndex(descriptors + 1u)].Load(visibility.y * 12u);
    const uint slot = LoadGeometryVertex(descriptors, vertex).Material;
    uint2 material_bindings;
    return HsClassifiedMaterialVariant(record, geometry, slot, variant, material_bindings);
}

#endif
