cbuffer ScreenOverlayConstants : register(b0, space34)
{
    float4 OverlayRenderSize;
    float4 OverlayOutputSize;
    uint4 OverlayInfo; // command count, intensity float bits, gradient rows, reserved
};
struct ScreenCommand
{
    float4 color;
    float4 appearance; // alpha, HDR, gradient age, envelope
    float4 coverage; // clear radius, edge exponent, screen source direction xy
    uint4 identity; // shape, gradient row, reserved
};
StructuredBuffer<ScreenCommand> OverlayCommands : register(t0, space34);
Texture2D<float4> OverlayGradient : register(t1, space34);
RWTexture2D<unorm float> OverlayReactiveOutput : register(u0, space34);
SamplerState OverlayLinearClamp : register(s0, space34);

float4 EvaluateOverlay(ScreenCommand command,float2 uv)
{
    const float2 p=uv*2.0-1.0;
    const float edge=pow(saturate((length(p)-command.coverage.x)/(1.0-command.coverage.x)),command.coverage.y);
    float shape=edge;
    if(command.identity.x==1u)
    {
        const float d=length(command.coverage.zw);
        shape*=d>0.0&&length(p)>0.0?pow(saturate(dot(normalize(p),command.coverage.zw/d)),32.0):0.0;
    }
    const float4 gradient=OverlayGradient.SampleLevel(OverlayLinearClamp,
        float2(saturate(command.appearance.z),(command.identity.y+0.5)/OverlayInfo.z),0);
    const float alpha=saturate(shape*command.appearance.x*command.appearance.w*gradient.a*asfloat(OverlayInfo.y));
    return float4(command.color.rgb*gradient.rgb*command.appearance.y*alpha,alpha);
}
[numthreads(8,8,1)]
void ScreenOverlayMaskCS(uint3 dispatch:SV_DispatchThreadID)
{
    if(any(dispatch.xy>=uint2(OverlayRenderSize.xy)))return;
    const float2 uv=(dispatch.xy+0.5)*OverlayRenderSize.zw;
    float transmittance=1.0;
    for(uint i=0;i<OverlayInfo.x;++i)transmittance*=1.0-EvaluateOverlay(OverlayCommands[i],uv).a;
    OverlayReactiveOutput[dispatch.xy]=saturate(1.0-transmittance);
}
struct ScreenVertex { float4 position:SV_Position; float2 uv:TEXCOORD0; nointerpolation uint command:TEXCOORD1; };
ScreenVertex ScreenOverlayVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID)
{
    ScreenVertex result;
    result.uv=float2((vertex<<1)&2,vertex&2);
    result.position=float4(result.uv*float2(2,-2)+float2(-1,1),0,1);
    result.command=instance;
    return result;
}
float4 ScreenOverlayPS(ScreenVertex input):SV_Target0
{ return EvaluateOverlay(OverlayCommands[input.command],input.uv); }
