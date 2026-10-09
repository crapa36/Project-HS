#ifndef HS_SCENE_HLOD_SAMPLING_HLSLI
#define HS_SCENE_HLOD_SAMPLING_HLSLI

// Generation-qualified static-slot export. A null descriptor, an absent slot
// or a recycled static slot retains the complete original representation.
StructuredBuffer<uint4> HsHlodLeaves : register(t0, space25);
uint4 HsHlodLeaf(uint slot, uint generation)
{
    uint count, stride;
    HsHlodLeaves.GetDimensions(count, stride);
    if (slot >= count || stride != 16u || generation == 0u) return uint4(0u, 15u, 0u, 0u);
    const uint4 value = HsHlodLeaves[slot];
    return value.x == generation && value.w == 0u && (value.y & ~15u) == 0u
        ? value : uint4(0u, 15u, 0u, 0u);
}
uint HsHlodLeafViewMask(uint slot, uint generation) { return HsHlodLeaf(slot, generation).y; }
uint HsHlodHistoryFlags(uint slot, uint generation) { return HsHlodLeaf(slot, generation).z; }

#endif
