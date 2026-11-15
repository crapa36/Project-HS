#ifndef HS_CHARACTER_MATERIAL_HLSLI
#define HS_CHARACTER_MATERIAL_HLSLI

#include "material_surface.hlsli"
#include "render_binding_contract.hlsli"

static const uint HsCharacterMaterialIdBase = 0x20000000u;
static const uint HsCharacterInvalidOrdinal = 0xffffffffu;

// Set headers and slot pairs share the immutable native material table.
StructuredBuffer<uint2> MeshMaterialBindings : register(t1, space8);

uint2 HsMeshMaterialOrdinals(uint binding_set, uint slot)
{
    if (binding_set == 0u) return HsCharacterInvalidOrdinal.xx;
    const uint2 range = MeshMaterialBindings[binding_set];
    if (slot >= range.y) return HsCharacterInvalidOrdinal.xx;
    return MeshMaterialBindings[range.x + slot];
}

bool HsCharacterOpaqueOrdinalValid(uint ordinal)
{
    // The CPU-validated binding table selects the opaque domain through .x.
    return ordinal < HsCharacterMaterialCount;
}

bool HsCharacterTranslucentOrdinalValid(uint ordinal)
{
    // The CPU-validated binding table selects the translucent domain through .y.
    return ordinal < HsCharacterMaterialCount;
}

uint HsCharacterOrdinal(uint material_id)
{
    return material_id - HsCharacterMaterialIdBase;
}

// The existing material producer supplies all seven validated context evaluators.
// Native adapters own texture sampling, expressions/charge, world-space normals
// and tangents, and the material parameter base. The binding table above is the
// only additional resource; authored graphs use the existing native textures.
#ifdef HS_GENERATED_CHARACTER_MATERIAL
#include "character_materials.generated.hlsli"
#endif

#endif
