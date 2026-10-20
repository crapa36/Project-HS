#ifndef HS_VOLUMETRIC_SAMPLING_HLSLI
#define HS_VOLUMETRIC_SAMPLING_HLSLI

// Shared suffix: b0,t0..13,u0..8; hero t14 and actual STBN t15 in space24. Camera matrices
// are camera-relative row-vector matrices, not the transposed FrameCB ABI.
cbuffer VolumetricConstants : register(b0,space24)
{
    row_major float4x4 VmInverseViewProjection,VmWorldToView,VmPreviousViewProjection,VmPreviousWorldToView;
    row_major float4x4 VmShadowViewProjection[3];
    float4 VmShadowAtlasScaleOffset[3];
    float4 VmRenderSize,VmFroxelSize,VmCapacity,VmPreviousRenderSize,VmPreviousFroxelSize;
    float4 VmOriginNear,VmPreviousOriginNear,VmDepthRange,VmFilterParameters,VmShadowParameters;
    uint4 VmShadowIdentity,VmFrameInfo,VmCounts,VmHeroInfo;
};
Texture3D<float4> VmFilteredScattering : register(t9,space24);
Texture3D<float4> VmCumulativeL : register(t10,space24);
Texture3D<float> VmCumulativeT : register(t11,space24);
Texture2D<float> VmColumnDepth : register(t13,space24);

struct HsVolumetricTransport { float3 radiance; float transmittance; };
HsVolumetricTransport HsVolumetricIdentity()
{
    HsVolumetricTransport result; result.radiance=0.0; result.transmittance=1.0; return result;
}
float VmBoundaryDepth(float boundary)
{
    return VmOriginNear.w*exp2(VmDepthRange.y*boundary/float(VmCounts.y));
}
float3 VmRelativeAtDepth(float2 uv,float view_depth)
{
    const float2 ndc=uv*float2(2.0,-2.0)+float2(-1.0,1.0);
    float4 a=mul(float4(ndc,1.0,1.0),VmInverseViewProjection);
    float4 b=mul(float4(ndc,0.5,1.0),VmInverseViewProjection);
    a.xyz/=a.w; b.xyz/=b.w;
    const float za=mul(float4(a.xyz,0.0),VmWorldToView).z;
    const float zb=mul(float4(b.xyz,0.0),VmWorldToView).z;
    return a.xyz+(b.xyz-a.xyz)*((view_depth-za)/(zb-za));
}
float VmSegmentMeters(float2 uv,float near_depth,float far_depth)
{
    return length(VmRelativeAtDepth(uv,far_depth)-VmRelativeAtDepth(uv,near_depth));
}
// Stable Beer-Lambert integral, including the zero-extinction limit. A short
// series avoids cancellation of 1-exp(-tau) for optically thin intervals.
float VmOneMinusExp(float tau)
{
    return tau<0.001 ? tau*(1.0-tau*(0.5-tau/6.0)) : 1.0-exp(-tau);
}
HsVolumetricTransport VmIntegrateCoefficient(float4 source,float meters)
{
    HsVolumetricTransport result;
    const float tau=max(source.w,0.0)*meters;
    result.transmittance=exp(-tau);
    const float integral=tau<0.001 ? meters*(1.0-tau*(0.5-tau/6.0)) : VmOneMinusExp(tau)/source.w;
    result.radiance=max(source.xyz,0.0)*integral; return result;
}
HsVolumetricTransport VmCompose(HsVolumetricTransport near_segment,HsVolumetricTransport far_segment)
{
    near_segment.radiance+=near_segment.transmittance*far_segment.radiance;
    near_segment.transmittance*=far_segment.transmittance; return near_segment;
}
HsVolumetricTransport VmColumnTransport(int2 column,float depth)
{
    HsVolumetricTransport result=HsVolumetricIdentity();
    if(depth<=VmOriginNear.w) return result;
    const float coordinate=clamp(log2(min(depth,VmDepthRange.x)/VmOriginNear.w)*float(VmCounts.y)/VmDepthRange.y,
        0.0,float(VmCounts.y));
    const uint lower=min((uint)floor(coordinate),VmCounts.y);
    result.radiance=VmCumulativeL.Load(int4(column,lower,0)).xyz;
    result.transmittance=VmCumulativeT.Load(int4(column,lower,0));
    if(lower<VmCounts.y && coordinate>float(lower))
    {
        // Integrate the actual partial physical interval; interpolating L/T
        // linearly in logarithmic texture Z produces incorrect optical depths.
        const float2 column_uv=(float2(column)+0.5)*VmFroxelSize.zw;
        const float4 source=VmFilteredScattering.Load(int4(column,lower,0));
        result=VmCompose(result,VmIntegrateCoefficient(source,
            VmSegmentMeters(column_uv,VmBoundaryDepth(float(lower)),min(depth,VmDepthRange.x))));
    }
    return result;
}
// Depth-aware four-column reconstruction. Depth is positive camera view-Z in
// meters. The caller supplies the actual fragment depth, including OIT layers.
HsVolumetricTransport HsSampleVolumetricTransport(float2 uv,float depth)
{
    HsVolumetricTransport result=HsVolumetricIdentity();
    if(VmCounts.y==0u || depth<=VmOriginNear.w) return result;
    const float2 coordinate=saturate(uv)*VmFroxelSize.xy-0.5;
    const int2 base=int2(floor(coordinate)); const float2 fraction=frac(coordinate);
    const int2 extent=int2(VmFroxelSize.xy);
    float3 sum_l=0.0; float sum_t=0.0,sum_weight=0.0;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x)
    {
        const int2 column=clamp(base+int2(x,y),int2(0,0),extent-1);
        const float2 component=float2(x!=0?fraction.x:1.0-fraction.x,y!=0?fraction.y:1.0-fraction.y);
        const float opaque_depth=VmColumnDepth.Load(int3(column,0));
        // Only an opaque boundary in front of this fragment is incompatible.
        // Clear farther columns remain usable for transparent fragments.
        const float gap=max(depth-opaque_depth,0.0)/max(depth,VmOriginNear.w);
        const float weight=component.x*component.y*exp(-gap/max(VmFilterParameters.y,1e-6));
        const HsVolumetricTransport value=VmColumnTransport(column,depth);
        sum_l+=value.radiance*weight; sum_t+=value.transmittance*weight; sum_weight+=weight;
    }
    if(sum_weight>0.0) { result.radiance=sum_l/sum_weight; result.transmittance=sum_t/sum_weight; }
    else
    {
        const int2 nearest=clamp(int2(round(coordinate)),int2(0,0),extent-1);
        result=VmColumnTransport(nearest,depth);
    }
    return result;
}
HsVolumetricTransport VmColumnInterval(int2 column,float near_depth,float far_depth)
{
    HsVolumetricTransport result=HsVolumetricIdentity();
    const float first=max(near_depth,VmOriginNear.w),last=min(far_depth,VmDepthRange.x);
    if(last<=first) return result;
    const uint first_slice=min((uint)floor(log2(first/VmOriginNear.w)*float(VmCounts.y)/VmDepthRange.y),VmCounts.y-1u);
    const float2 column_uv=(float2(column)+0.5)*VmFroxelSize.zw;
    [loop] for(uint slice=first_slice;slice<VmCounts.y;++slice)
    {
        const float start=max(first,VmBoundaryDepth(float(slice)));
        const float end=min(last,VmBoundaryDepth(float(slice+1u)));
        if(end>start) result=VmCompose(result,VmIntegrateCoefficient(VmFilteredScattering.Load(int4(column,slice,0)),
            VmSegmentMeters(column_uv,start,end)));
        if(end>=last) break;
    }
    return result;
}
HsVolumetricTransport HsSampleVolumetricInterval(float2 uv,float near_depth,float far_depth)
{
    if(VmCounts.y==0u || far_depth<=near_depth) return HsVolumetricIdentity();
    const float2 coordinate=saturate(uv)*VmFroxelSize.xy-0.5;
    const int2 base=int2(floor(coordinate)); const float2 fraction=frac(coordinate);
    HsVolumetricTransport result;
    result.radiance=0.0; result.transmittance=0.0;
    float sum_weight=0.0;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x)
    {
        const int2 column=clamp(base+int2(x,y),int2(0,0),int2(VmFroxelSize.xy)-1);
        const float opaque_depth=VmColumnDepth.Load(int3(column,0));
        const float gap=max(far_depth-opaque_depth,0.0)/max(far_depth,VmOriginNear.w);
        const float weight=(x!=0?fraction.x:1.0-fraction.x)*(y!=0?fraction.y:1.0-fraction.y)*
            exp(-gap/max(VmFilterParameters.y,1e-6));
        const HsVolumetricTransport near_value=VmColumnTransport(column,near_depth);
        HsVolumetricTransport value;
        if(near_value.transmittance>=0.001)
        {
            const HsVolumetricTransport far_value=VmColumnTransport(column,far_depth);
            value.transmittance=saturate(far_value.transmittance/near_value.transmittance);
            value.radiance=max(far_value.radiance-near_value.radiance,0.0)/near_value.transmittance;
        }
        else value=VmColumnInterval(column,near_depth,far_depth);
        result.radiance+=value.radiance*weight; result.transmittance+=value.transmittance*weight;
        sum_weight+=weight;
    }
    if(sum_weight>0.0) { result.radiance/=sum_weight; result.transmittance/=sum_weight; }
    else result=VmColumnInterval(clamp(int2(round(coordinate)),int2(0,0),int2(VmFroxelSize.xy)-1),near_depth,far_depth);
    return result;
}
float3 HsApplyVolumetricTransport(float3 scene_linear_radiance,HsVolumetricTransport transport)
{
    return transport.radiance+transport.transmittance*scene_linear_radiance;
}
#endif
