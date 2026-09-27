struct GpuRibbonSourceUpdate
{
    float4 PositionDuration;
    float4 PreviousInterpolation;
    uint4 Identity;
    float4 RenderedHead;
    float4 ControlAge;
};

struct GpuRibbonOutput
{
    uint4 Metadata;
    float4 Widths;
    float4 Color;
    float4 Material;
    float4 PreviousEye;
    float4 Detail;
    float4 Analytic;
};

StructuredBuffer<GpuRibbonSourceUpdate> RibbonSourceUpdates : register(t2, space3);
StructuredBuffer<GpuRibbonOutput> RibbonOutputs : register(t3, space3);
ByteAddressBuffer RibbonHistoryRead : register(t4, space3);
ByteAddressBuffer RibbonPreviousHistoryRead : register(t5, space3);
Texture2DArray<float> RibbonDetail : register(t6, space3);
RWByteAddressBuffer RibbonHistoryWrite : register(u1, space3);
RWByteAddressBuffer RibbonPreviousHistoryWrite : register(u2, space3);
RWByteAddressBuffer RibbonArgumentsWrite : register(u3, space3);

static const uint RibbonHistoryStride = 2080;
static const uint RibbonHistorySamples = 64;

uint RibbonSlotOffset(uint slot) { return slot * RibbonHistoryStride; }
uint RibbonPointOffset(uint slot, uint index)
{
    return RibbonSlotOffset(slot) + 32 + min(index, RibbonHistorySamples - 1) * 32;
}

uint4 RibbonLoadPoint(ByteAddressBuffer history, uint slot, uint index)
{
    return history.Load4(RibbonPointOffset(slot, index) + 16);
}

void RibbonStorePoint(RWByteAddressBuffer history, uint slot, uint index,
                      float3 position, float cumulative, uint sample_tick)
{
    const uint offset = RibbonPointOffset(slot, index);
    history.Store4(offset, asuint(float4(position, cumulative)));
    history.Store4(offset + 16, uint4(sample_tick, 1, 0, 0));
}

[numthreads(1,1,1)]
void RibbonUpdateCS(uint3 id : SV_DispatchThreadID)
{
    GpuRibbonSourceUpdate source = RibbonSourceUpdates[id.x];
    uint slot = source.Identity.x, tick = source.Identity.y, base = RibbonSlotOffset(slot);
    bool reset = (source.Identity.w & 2u) != 0u;
    if ((source.Identity.w & 4u) != 0u)
    {
        if (!reset)
            for (uint byte_offset=0; byte_offset<RibbonHistoryStride; byte_offset+=16)
                RibbonPreviousHistoryWrite.Store4(base+byte_offset,RibbonHistoryWrite.Load4(base+byte_offset));
        for (uint byte_offset=0; byte_offset<RibbonHistoryStride; byte_offset+=16)
            RibbonHistoryWrite.Store4(base+byte_offset,0);
        if ((source.Identity.w & 1u) == 0u) return;
        const uint segments = (source.Identity.w >> 8) & 255u;
        float arc = 0;
        float3 previous = source.PreviousInterpolation.xyz;
        for (uint point_index=0; point_index<=segments; ++point_index)
        {
            const float t = float(point_index)/float(segments);
            const float3 position = (1-t)*(1-t)*source.PreviousInterpolation.xyz +
                2*t*(1-t)*source.ControlAge.xyz + t*t*source.PositionDuration.xyz;
            arc += length(position-previous);
            RibbonStorePoint(RibbonHistoryWrite,slot,point_index,position,arc,tick);
            previous = position;
        }
        RibbonHistoryWrite.Store4(base,uint4(segments,segments+1,tick,source.Identity.z));
        RibbonHistoryWrite.Store4(base+16,asuint(source.RenderedHead));
        if (reset)
            for (uint byte_offset=0; byte_offset<RibbonHistoryStride; byte_offset+=16)
                RibbonPreviousHistoryWrite.Store4(base+byte_offset,RibbonHistoryWrite.Load4(base+byte_offset));
        return;
    }
    if (reset)
    {
        // Initialize all bytes before current/previous history is read.
        for (uint b = 0; b < RibbonHistoryStride; b += 16) RibbonHistoryWrite.Store4(base+b,0);
        RibbonStorePoint(RibbonHistoryWrite,slot,0,source.PreviousInterpolation.xyz,0,tick > 0 ? tick-1 : 0);
        RibbonStorePoint(RibbonHistoryWrite,slot,1,source.PositionDuration.xyz,
            length(source.PositionDuration.xyz-source.PreviousInterpolation.xyz),tick);
        RibbonHistoryWrite.Store4(base,uint4(1,2,tick,source.Identity.z));
        RibbonHistoryWrite.Store4(base+16,asuint(source.RenderedHead));
    }
    for (uint b=0; b<RibbonHistoryStride; b+=16)
        RibbonPreviousHistoryWrite.Store4(base+b,RibbonHistoryWrite.Load4(base+b));
    if (reset || (source.Identity.w & 1u)==0u) return;
    uint4 header=RibbonHistoryWrite.Load4(base);
    if (header.z != tick)
    {
        float4 last=asfloat(RibbonHistoryWrite.Load4(RibbonPointOffset(slot,header.x)));
        uint cursor=(header.x+1)%RibbonHistorySamples;
        RibbonStorePoint(RibbonHistoryWrite,slot,cursor,source.PositionDuration.xyz,
            last.w+length(source.PositionDuration.xyz-last.xyz),tick);
        RibbonHistoryWrite.Store4(base,uint4(cursor,min(header.y+1,RibbonHistorySamples),tick,source.Identity.z));
    }
    RibbonHistoryWrite.Store4(base+16,asuint(source.RenderedHead));
}
[numthreads(1,1,1)]
void RibbonArgsCS(uint3 id : SV_DispatchThreadID)
{
    GpuRibbonOutput output=RibbonOutputs[id.x];
    uint slot=output.Metadata.x;
    uint4 header=RibbonHistoryWrite.Load4(RibbonSlotOffset(slot));
    uint now=asuint(output.PreviousEye.w), segments=0;
    float3 lo=asfloat(RibbonHistoryWrite.Load4(RibbonSlotOffset(slot)+16)).xyz;
    float3 hi=lo;
    // Keep one sample beyond lifetime; VS clips it to the tail boundary.
    for(uint i=0;i<header.y;++i)
    {
        uint p=RibbonPointOffset(slot,(header.x+RibbonHistorySamples-i)%RibbonHistorySamples);
        float3 world=asfloat(RibbonHistoryWrite.Load4(p)).xyz;
        lo=min(lo,world); hi=max(hi,world);
        if(i>0) segments=i;
        if(output.Analytic.z == 0 && float(now-RibbonHistoryWrite.Load(p+16)) >= output.Material.y*60.0) break;
    }
    float extent=max(output.Widths.x,output.Widths.y)*output.Material.w+output.Widths.z;
    lo-=extent; hi+=extent;
    uint outside=63;
    for(uint c=0;c<8;++c)
    {
        float3 world=float3((c&1)?hi.x:lo.x,(c&2)?hi.y:lo.y,(c&4)?hi.z:lo.z);
        float4 clip=mul(float4(world,1),ViewProjection);
        uint mask=(clip.x < -clip.w?1u:0u)|(clip.x > clip.w?2u:0u)|
            (clip.y < -clip.w?4u:0u)|(clip.y > clip.w?8u:0u)|
            (clip.z < 0?16u:0u)|(clip.z > clip.w?32u:0u);
        outside &= mask;
    }
    if(outside!=0 || (output.Analytic.z == 0 && float(now-header.z)>=output.Material.y*60.0)) segments=0;
    uint strands=(output.Metadata.z==2 || output.Metadata.z==7)?2:1;
    // SV_InstanceID excludes StartInstanceLocation in Direct3D. Pass the
    // output index as an explicit indirect root constant instead.
    // https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/
    RibbonArgumentsWrite.Store(id.x*20,id.x);
    RibbonArgumentsWrite.Store4(id.x*20+4,uint4(segments*6*strands,1,0,0));
}
float3 RibbonNormalize(float3 v,float3 fallback)
{
    float d=dot(v,v); return d>1e-10?v*rsqrt(d):fallback;
}
float4 RibbonSample(uint slot,uint4 header,uint ordinal)
{
    uint index=(header.x+RibbonHistorySamples-min(ordinal,header.y-1))%RibbonHistorySamples;
    float4 sample=asfloat(RibbonHistoryRead.Load4(RibbonPointOffset(slot,index)));
    if(ordinal==0) sample.xyz=asfloat(RibbonHistoryRead.Load4(RibbonSlotOffset(slot)+16)).xyz;
    return sample;
}
struct RibbonOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
    float Age : TEXCOORD1;
    nointerpolation uint OutputIndex : TEXCOORD2;
};
RibbonOutput RibbonVS(uint vertex : SV_VertexID)
{
    uint output_index = PassValue;
    GpuRibbonOutput material=RibbonOutputs[output_index];
    uint slot=material.Metadata.x;
    uint4 header=RibbonHistoryRead.Load4(RibbonSlotOffset(slot));
    // Interleave two strands per segment so both share history and culling.
    const bool ground_rails=material.Metadata.z==7;
    uint strands=(material.Metadata.z==2 || ground_rails)?2:1;
    uint segment=vertex/(6*strands),strand=(vertex/6)%strands,corner=vertex%6;
    uint ordinal=segment+((corner==2||corner==3||corner==5)?1:0);
    float sign=(corner==0||corner==2||corner==3)?-1.0:1.0;
    float4 sample=RibbonSample(slot,header,ordinal);
    float4 newer=RibbonSample(slot,header,ordinal>0?ordinal-1:0);
    float4 older=RibbonSample(slot,header,ordinal+1);
    uint physical=(header.x+RibbonHistorySamples-ordinal)%RibbonHistorySamples;
    uint tick=RibbonHistoryRead.Load(RibbonPointOffset(slot,physical)+16);
    const bool analytic = material.Analytic.z != 0;
    const float total_arc = RibbonSample(slot,header,0).w;
    float age = analytic ? (total_arc > 0.000001 ? saturate(sample.w/total_arc) : 1.0-float(ordinal)/float(max(header.y-1,1u)))
        : float(asuint(material.PreviousEye.w)-tick)/(material.Material.y*60.0);
    if(!analytic && age>1 && ordinal>0)
    {
        uint newer_tick=RibbonHistoryRead.Load(RibbonPointOffset(slot,(physical+1)%RibbonHistorySamples)+16);
        float newer_age=float(asuint(material.PreviousEye.w)-newer_tick)/(material.Material.y*60.0);
        sample=lerp(newer,sample,saturate((1-newer_age)/max(age-newer_age,1e-5)));
        age=1;
    }
    float3 tangent=RibbonNormalize(newer.xyz-older.xyz,float3(0,0,1));
    float3 view=RibbonNormalize(CameraTime.xyz-sample.xyz,float3(0,1,0));
    float3 fallback=RibbonNormalize(cross(float3(0,1,0),tangent),float3(1,0,0));
    float3 side=RibbonNormalize(cross(view,tangent),fallback);
    if(ground_rails) side=fallback;
    float3 edge=RibbonNormalize(ordinal==0?sample.xyz-older.xyz:newer.xyz-sample.xyz,tangent);
    float3 edge_side=RibbonNormalize(cross(view,edge),side);
    float miter=min(material.Material.w,1.0/max(abs(dot(side,edge_side)),0.001));
    if(ground_rails) miter=1.0;
    float arc=sample.w;
    float endpoint_envelope = (age <= 0 || age >= 1) ? 0.0 : sin(age*3.14159265);
    float width = analytic ? lerp(material.Widths.y,material.Widths.x,endpoint_envelope)
        : lerp(material.Widths.x,material.Widths.y,saturate(age));
    if(ground_rails) width=min(width,material.Analytic.w/3.0);
    float wave=sin(arc*17.0+(ground_rails?1.0:-1.0)*CameraTime.w*9.0+material.Widths.w*6.2831853);
    float offset=wave*material.Widths.z*(analytic ? endpoint_envelope : saturate(age));
    if(strands==2) offset+=(strand==0?-1.0:1.0)*width*(0.75+0.25*wave);
    if(ground_rails)
    {
        // Positive time moves phase toward the older, smaller arc: the rails
        // tear backward while their centerlines stay inside the dash lane.
        const float rail=(strand==0?-1.0:1.0);
        const float separation=max(0.0,material.Analytic.w*0.5-width*0.68);
        offset=rail*separation+wave*width*0.18*saturate(age);
    }
    float3 world=sample.xyz+side*(sign*width*0.5*miter+offset);
    RibbonOutput output;
    output.Position=mul(float4(world,1),ViewProjection);
    output.Uv=float2(arc,sign*0.5+0.5);
    output.Age=saturate(age);
    output.OutputIndex=output_index;
    return output;
}
float4 RibbonEnergy(RibbonOutput input)
{
    GpuRibbonOutput material=RibbonOutputs[input.OutputIndex];
    float4 gradient=SampleVfxGradient(material.Metadata.y,input.Age);
    float cross_section=smoothstep(0.0,0.18,input.Uv.y)*(1-smoothstep(0.82,1.0,input.Uv.y));
    float detail=1;
    if(material.Detail.x>=0)
        detail=lerp(1.0,RibbonDetail.Sample(MaterialSampler,float3(input.Uv*float2(material.Detail.y,1),material.Detail.x)),material.Material.z);
    float dashed=(material.Metadata.w&2u)!=0?smoothstep(0.35,0.45,frac(input.Uv.x*5.0)):1;
    float life_fade = material.Analytic.z != 0 ? 1.0 : 1-input.Age;
    float pulse = 1.0;
    if (material.Analytic.z != 0 && material.Analytic.y != 0)
    {
        const float delta = (input.Age-saturate(material.Analytic.x))/0.08;
        pulse = 0.35 + 0.65*exp(-delta*delta);
    }
    float alpha=saturate(gradient.a*material.Color.a*material.Detail.w*cross_section*detail*dashed*life_fade*pulse);
    return float4(gradient.rgb*material.Color.rgb*material.Material.x*alpha,alpha);
}
float4 RibbonAddPS(RibbonOutput input) : SV_Target0 { return RibbonEnergy(input); }
OitOutput RibbonOitPS(RibbonOutput input)
{
    float4 energy=RibbonEnergy(input);
    float weight=clamp(pow(energy.a+0.01,3.0)*1e8,1e-2,3e3);
    OitOutput output;
    output.Accumulation=float4(energy.rgb*weight,energy.a*weight);
    output.Revealage=energy.a.xxxx;
    return output;
}
