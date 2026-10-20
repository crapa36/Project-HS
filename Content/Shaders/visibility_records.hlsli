#ifndef HS_VISIBILITY_RECORDS_HLSLI
#define HS_VISIBILITY_RECORDS_HLSLI

#include "render_binding_contract.hlsli"

// 0 is generic, then authored environment materials, then hero ordinals.
// Ordinal5 is transparent and never writes opaque visibility.
static const uint HsVisibilityInvalid = 0xffffffffu;
static const uint HsVisibilityOffsetBase = HsVisibilityVariantCount;
static const uint HsVisibilityCursorBase = HsVisibilityVariantCount * 2u;

struct HsRasterRecord
{
    uint geometryRecordId;
    uint visibleRefIndex;
    uint refSource;
    uint materialWord;
    uint instanceIndex;
    uint instanceGeneration;
    uint pageGeneration;
    uint flags;
};

struct HsRasterRange
{
    uint geometryRecordId;
    uint rasterBase;
    uint instanceCapacity;
    uint materialWord;
};

HsRasterRecord HsLoadRasterRecord(ByteAddressBuffer records, uint id)
{
    const uint4 words = records.Load4(id * 32u);
    HsRasterRecord record;
    record.geometryRecordId = words.x;
    record.visibleRefIndex = words.y;
    record.refSource = words.z;
    record.materialWord = words.w;
    const uint4 identity = records.Load4(id * 32u + 16u);
    record.instanceIndex = identity.x;
    record.instanceGeneration = identity.y;
    record.pageGeneration = identity.z;
    record.flags = identity.w;
    return record;
}

void HsStoreRasterRecord(RWByteAddressBuffer records, uint id,
                         HsRasterRecord record)
{
    records.Store4(id * 32u, uint4(record.geometryRecordId,
                                    record.visibleRefIndex,
                                    record.refSource,
                                    record.materialWord));
    records.Store4(id * 32u + 16u, uint4(record.instanceIndex,
                                         record.instanceGeneration,
                                         record.pageGeneration,
                                         record.flags));
}

uint HsRasterVariant(HsRasterRecord record)
{
    return record.flags & 0xffu;
}

bool HsRasterRecordDomainValid(HsRasterRecord record)
{
    return record.geometryRecordId != 0u &&
           record.refSource >= 1u && record.refSource <= 6u &&
           HsRasterVariant(record) < HsVisibilityVariantCount;
}

uint HsVisibilityGroupOrdinal(uint3 groupId)
{
    return groupId.x + groupId.y * 65535u;
}

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

bool HsRasterIdValid(uint id)
{
    return id != HsVisibilityInvalid;
}

#endif
