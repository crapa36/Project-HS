#ifndef HS_MATERIAL_PARAMETERS_HLSLI
#define HS_MATERIAL_PARAMETERS_HLSLI

#include "render_binding_contract.hlsli"

// Reflected declaration ordinal selects one 16-byte slot. RenderData owns the
// CPU word encoding; generated evaluators select the declared type's lanes.
StructuredBuffer<uint4> MaterialParameters : register(t0, space8);

#endif
