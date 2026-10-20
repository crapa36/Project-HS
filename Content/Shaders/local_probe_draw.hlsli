#ifndef HS_LOCAL_PROBE_DRAW_HLSLI
#define HS_LOCAL_PROBE_DRAW_HLSLI

// Root b1 keeps its two-word ABI. Bits30/31 must stay clear for shadow clipping.
uint HsLocalProbeSourceSlot(uint pass_value, uint instance_id)
{ return (pass_value >> 4u) + instance_id; }
uint HsLocalProbeMaterialOrdinal(uint pass_value)
{ return pass_value & 15u; }

#endif
