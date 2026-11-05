#ifndef HS_CHARACTER_MATERIAL_HLSLI
#define HS_CHARACTER_MATERIAL_HLSLI

#include "material_surface.hlsli"
#include "render_binding_contract.hlsli"

static const uint HsCharacterMaterialIdBase = 0x20000000u;
static const uint HsHeroInvalidOrdinal = 0xffffffffu;

// Each authored FBX slot has an opaque ordinal and an optional transparent
// ordinal. The native package loader validates the slot/domain identities.
StructuredBuffer<uint2> MeshMaterialBindings : register(t1, space8);

uint HsHeroAssetIndex(uint mesh)
{
    if (mesh == 0u) return 0u;
    if (mesh >= 10u && mesh <= 15u) return mesh - 9u;
    return HsHeroInvalidOrdinal;
}

uint2 HsHeroBindingOrdinals(uint mesh, uint slot)
{
    const uint asset = HsHeroAssetIndex(mesh);
    if (asset == HsHeroInvalidOrdinal) return HsHeroInvalidOrdinal.xx;
    const uint4 range = HeroBindingRanges[asset];
    // Ranges and the SRV are published together from one admitted binding table.
    if (slot >= range.y)
        return HsHeroInvalidOrdinal.xx;
    return MeshMaterialBindings[range.x + slot];
}

bool HsHeroOpaqueOrdinalValid(uint ordinal)
{
    // The CPU-validated binding table selects the opaque domain through .x.
    return ordinal < HsCharacterMaterialCount;
}

bool HsHeroTranslucentOrdinalValid(uint ordinal)
{
    // The CPU-validated binding table selects the translucent domain through .y.
    return ordinal < HsCharacterMaterialCount;
}

uint HsHeroOrdinal(uint material_id)
{
    return material_id - HsCharacterMaterialIdBase;
}

uint HsHeroOpaqueMaterialId(uint mesh, uint slot)
{
    const uint ordinal = HsHeroBindingOrdinals(mesh, slot).x;
    return HsHeroOpaqueOrdinalValid(ordinal)
        ? HsCharacterMaterialIdBase + ordinal : HsHeroInvalidOrdinal;
}

// The existing material producer supplies all seven validated context evaluators.
// Native adapters own texture sampling, expressions/charge, world-space normals
// and tangents, and the material parameter base. The binding table above is the
// only additional resource; authored graphs use the existing native textures.
#ifdef HS_GENERATED_CHARACTER_MATERIAL
#include "character_materials.generated.hlsli"
#endif

#endif
