#include "ui_draw_abi.hlsli"

struct UiVertex
{
    float4 position : SV_Position;
    nointerpolation float4 bounds : TEXCOORD0;
    nointerpolation float4 clipRect : TEXCOORD1;
    nointerpolation float4 color : COLOR0;
    nointerpolation uint2 glyph : TEXCOORD2;
    nointerpolation float radius : TEXCOORD3;
};

UiVertex UiDrawVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    UiQuad quad = quads[instance];
    float2 corner = float2(vertex & 1, vertex >> 1);
    float fringe = quad.glyphStride == 0 ? 0.5 : 0;
    float2 p = lerp(quad.bounds.xy - fringe, quad.bounds.zw + fringe, corner);
    UiVertex result;
    result.position = float4(p / outputExtent * float2(2, -2) + float2(-1, 1), 0, 1);
    result.bounds = quad.bounds;
    result.clipRect = quad.clipRect;
    result.color = ((quad.color >> uint4(0, 8, 16, 24)) & 255) / 255.0;
    result.glyph = uint2(quad.glyphOffset, quad.glyphStride);
    result.radius = quad.radius;
    return result;
}

float4 UiDrawPS(UiVertex input) : SV_Target
{
    float2 p = input.position.xy;
    float2 clipCoverage = saturate(min(p + 0.5, input.clipRect.zw) - max(p - 0.5, input.clipRect.xy));
    float alpha = input.color.a * clipCoverage.x * clipCoverage.y;
    if (input.glyph.y != 0)
    {
        uint2 pixel = uint2(p - input.bounds.xy);
        uint address = input.glyph.x + pixel.y * input.glyph.y + pixel.x;
        uint packed = coverageAtlas.Load(address & ~3u);
        alpha *= ((packed >> ((address & 3u) * 8)) & 255) / 255.0;
    }
    else if (input.radius > 0)
    {
        float2 halfSize = 0.5 * (input.bounds.zw - input.bounds.xy);
        float radius = min(input.radius, min(halfSize.x, halfSize.y));
        float2 q = abs(p - input.bounds.xy - halfSize) - halfSize + radius;
        float distance = length(max(q, 0)) + min(max(q.x, q.y), 0) - radius;
        alpha *= saturate(0.5 - distance);
    }
    else
    {
        float2 coverage = saturate(min(p + 0.5, input.bounds.zw) - max(p - 0.5, input.bounds.xy));
        alpha *= coverage.x * coverage.y;
    }
    return float4(input.color.rgb * alpha, alpha);
}
