#ifndef HS_HERO_MATERIAL_HLSLI
#define HS_HERO_MATERIAL_HLSLI

#include "material_surface.hlsli"
#include "render_binding_contract.hlsli"

static const uint HsHeroMaterialIdBase = 0x20000000u;
static const uint HsHeroInvalidOrdinal = 0xffffffffu;

// Each authored FBX slot has an opaque ordinal and an optional transparent
// ordinal. The native package loader validates the slot/domain identities.
StructuredBuffer<uint2> HeroMaterialBindings : register(t1, space8);

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
    uint count, stride;
    HeroMaterialBindings.GetDimensions(count, stride);
    if (slot >= range.y || range.x >= count || slot >= count - range.x)
        return HsHeroInvalidOrdinal.xx;
    return HeroMaterialBindings[range.x + slot];
}

bool HsHeroOpaqueOrdinalValid(uint ordinal)
{
    return ordinal < HsHeroMaterialCount && ordinal != 5u;
}

uint HsHeroOrdinal(uint material_id)
{
    return material_id - HsHeroMaterialIdBase;
}

uint HsHeroOpaqueMaterialId(uint mesh, uint slot)
{
    const uint ordinal = HsHeroBindingOrdinals(mesh, slot).x;
    return HsHeroOpaqueOrdinalValid(ordinal)
        ? HsHeroMaterialIdBase + ordinal : HsHeroInvalidOrdinal;
}

// The existing material producer supplies all seven validated context evaluators.
// Native adapters own texture sampling, expressions/charge, world-space normals
// and tangents, and the material parameter base. The binding table above is the
// only additional resource; authored graphs use the existing native textures.
#ifdef HS_GENERATED_HERO_MATERIAL
#include "hero_materials.generated.hlsli"
#endif

#endif
