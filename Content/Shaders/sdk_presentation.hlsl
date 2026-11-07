#include "display_transform.hlsli"
#include "sdk_presentation_abi.hlsli"

cbuffer PresentationConstants : register(b0, HS_PRESENT_JOIN(space,HS_PRESENT_SPACE))
{
#define HS_PRESENT_HLSL_Uint uint
#define HS_PRESENT_HLSL_Float float
#define HS_PRESENT_HLSL_Float2 float2
#define HS_PRESENT_HLSL_Float4 float4
#define HS_PRESENT_HLSL_Matrix row_major float4x4
#define HS_PRESENT_DECLARE_FIELD(type,native,shader) HS_PRESENT_HLSL_##type shader;
    HS_PRESENT_FIELDS(HS_PRESENT_DECLARE_FIELD)
#undef HS_PRESENT_DECLARE_FIELD
};
#define RenderSize uint2(RenderWidth,RenderHeight)
#define OutputSize uint2(OutputWidth,OutputHeight)
#define HS_PRESENT_DECLARE_SRV(name,slot,type,shader,version,resource,format,pass) \
    Texture2D<type> shader : register(HS_PRESENT_JOIN(t,slot),HS_PRESENT_JOIN(space,HS_PRESENT_SPACE));
HS_PRESENT_SRVS(HS_PRESENT_DECLARE_SRV)
#undef HS_PRESENT_DECLARE_SRV
#define HS_PRESENT_DECLARE_UAV(name,slot,type,shader,version,resource,format,index) \
    RWTexture2D<type> shader : register(HS_PRESENT_JOIN(u,slot),HS_PRESENT_JOIN(space,HS_PRESENT_SPACE));
HS_PRESENT_UAVS(HS_PRESENT_DECLARE_UAV)
#undef HS_PRESENT_DECLARE_UAV

float4 PresentationVS(uint vertex_id : SV_VertexID) : SV_Position
{
    const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 EncodeDisplay(float3 linear_rec709)
{
    return Hdr10 != 0 ? HsDisplayEncodeHdr10(linear_rec709, PaperWhiteNits, PeakNits) :
                        HsDisplayEncodeSdr(linear_rec709);
}
struct PresentationColors
{
#define HS_PRESENT_COLOR_FIELD(name,index) float4 name : HS_PRESENT_JOIN(SV_Target,index);
    HS_PRESENT_COLOR_TARGETS(HS_PRESENT_COLOR_FIELD)
#undef HS_PRESENT_COLOR_FIELD
};
PresentationColors PresentationPS(float4 position : SV_Position)
{
    const uint2 pixel = uint2(position.xy);
    const float3 scene = SceneDisplayLinear.Load(int3(pixel, 0)).rgb;
    const float4 game_ui = HsDisplayDecodePremultipliedUi(EncodedPremultipliedUi.Load(int3(pixel, 0)));
    const float4 devtools_ui = HsDisplayDecodePremultipliedUi(EncodedDevToolsUi.Load(int3(pixel, 0)));
    const float4 ui_linear = game_ui * (1 - devtools_ui.a) + devtools_ui;
    PresentationColors result;
    result.hudless = float4(EncodeDisplay(scene), 1);
    // Encode the unassociated color, then premultiply. Applying a transfer
    // function to already-premultiplied RGB would darken translucent edges.
    result.ui = ui_linear.a > 0 ? float4(EncodeDisplay(ui_linear.rgb / ui_linear.a) * ui_linear.a, ui_linear.a) : 0;
    result.composite = float4(EncodeDisplay(scene * (1 - ui_linear.a) + ui_linear.rgb), 1);
    return result;
}
[numthreads(8, 8, 1)]
void PresentationGuidesCS(uint3 pixel : SV_DispatchThreadID)
{
    if (any(pixel.xy >= RenderSize)) return;
    float depth = DeviceDepth.Load(int3(pixel.xy, 0));
    float2 motion = UndilatedMotionUv.Load(int3(pixel.xy, 0));
    if (TransparentEnabled != 0)
    {
        const float front = TransparentDepth.Load(int3(pixel.xy, 0));
        if (front > depth && TransparentClassification.Load(int3(pixel.xy, 0)).z > 0 &&
            TransparentMetadata.Load(int3(pixel.xy, 0)).x != 0)
        { depth = front; motion = TransparentMotionUv.Load(int3(pixel.xy, 0)); }
    }
    if (ResetHistory != 0) motion = 0;
    else if (depth == 0 && MotionContainsSky == 0)
    {
        const float2 uv = (float2(pixel.xy) + 0.5) / RenderSize - JitterUv;
        const float4 world = mul(float4(uv * float2(2, -2) + float2(-1, 1), 0, 1), InverseCurrentViewProjection);
        const float3 ray = abs(world.w) > 1e-7 ? world.xyz / world.w - CameraPosition.xyz : world.xyz;
        const float4 previous = mul(float4(ray, 0), PreviousViewProjection);
        motion = previous.w > 1e-7 ? previous.xy / previous.w * float2(0.5, -0.5) + 0.5 - uv : 0;
    }
    NativeDepth[pixel.xy] = depth;
    NativeMotionUv[pixel.xy] = all(isfinite(motion)) ? motion : 0;
}
