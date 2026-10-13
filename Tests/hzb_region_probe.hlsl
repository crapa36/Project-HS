#include "hzb_region.hlsli"

struct ProbeInput
{
    uint width, height, levels, capacity;
    uint min_x, min_y, max_x, max_y;
};
struct ProbeOutput { uint valid; HsHzbRegion region; };
StructuredBuffer<ProbeInput> Inputs : register(t0);
RWStructuredBuffer<ProbeOutput> Outputs : register(u0);
cbuffer Parameters : register(b0) { uint Count; }

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Count) return;
    ProbeInput input = Inputs[id.x];
    ProbeOutput output = (ProbeOutput)0;
    output.valid = HsFindHzbRegion(input.width, input.height,
        input.levels, input.capacity, input.min_x, input.min_y,
        input.max_x, input.max_y, output.region) ? 1u : 0u;
    if (output.valid == 0u) output.region = (HsHzbRegion)0;
    Outputs[id.x] = output;
}
