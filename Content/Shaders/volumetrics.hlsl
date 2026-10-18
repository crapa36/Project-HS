#include "volumetric_sampling.hlsli"
#include "light_list.hlsli"
#include "shadow_data.hlsli"

struct VmVolume
{
    row_major float4x4 world_to_local;
    float4 density_extinction,scattering;
    uint4 identity;
};
// Separate 176-byte hero metadata; the cooked VmVolume ABI remains 112 bytes.
struct VmHeroVolume
{
    row_major float4x4 world_to_local;
    float4 optical,albedo,emission;
    uint4 sampling,effect_source,owner_event,episode_output;
};
Texture2D<float> VmDepth : register(t0,space24);
Texture2D<float4> VmHdr : register(t1,space24);
StructuredBuffer<VmVolume> VmVolumes : register(t2,space24);
Texture3D<float4> VmMedium : register(t3,space24);
Texture3D<float> VmPhase : register(t4,space24);
Texture3D<float4> VmRawScattering : register(t5,space24);
Texture3D<float4> VmPreviousScattering : register(t6,space24);
Texture2D<float> VmPreviousDepth : register(t7,space24);
Texture3D<float4> VmTemporalScattering : register(t8,space24);
Texture2D<float> VmCompactShadow : register(t12,space24);
StructuredBuffer<VmHeroVolume> VmHeroes : register(t14,space24);
Texture2DArray<float> VmHeroBlueNoise : register(t15,space24);
Texture3D<float4> VmHeroNoiseVolume : register(t16,space24);
Texture2D<float4> VmHeroGradient : register(t1,space3);
SamplerState VmWrapSampler : register(s2);
RWTexture3D<float4> VmMediumOutput : register(u0,space24);
RWTexture3D<float> VmPhaseOutput : register(u1,space24);
RWTexture3D<float4> VmRawOutput : register(u2,space24);
RWTexture3D<float4> VmTemporalOutput : register(u3,space24);
RWTexture2D<float> VmDepthOutput : register(u4,space24);
RWTexture3D<float4> VmFilteredOutput : register(u5,space24);
RWTexture3D<float4> VmLuminanceOutput : register(u6,space24);
RWTexture3D<float> VmTransmittanceOutput : register(u7,space24);
RWTexture2D<float4> VmResolvedOutput : register(u8,space24);

uint VmHash(uint value)
{
    value^=value>>16; value*=0x7feb352du; value^=value>>15; value*=0x846ca68bu; return value^(value>>16);
}
float VmRandom(uint3 p,uint sample,uint dimension)
{
    return float(VmHash(p.x^VmHash(p.y)^VmHash(p.z)^VmHash(VmFrameInfo.x)^VmHash(sample*31u+dimension*7919u))&0x00ffffffu)
        *5.9604644775390625e-8;
}
float VmPixelDepth(int2 pixel)
{
    const float depth=VmDepth.Load(int3(pixel,0));
    if(depth<=0.0) return VmDepthRange.x;
    const float2 uv=(float2(pixel)+0.5)*VmRenderSize.zw;
    float4 relative=mul(float4(uv*float2(2.0,-2.0)+float2(-1.0,1.0),depth,1.0),VmInverseViewProjection);
    relative.xyz/=relative.w;
    return clamp(mul(float4(relative.xyz,0.0),VmWorldToView).z,VmOriginNear.w,VmDepthRange.x);
}
float3 VmFroxelWorld(uint3 froxel)
{
    const float2 uv=(float2(froxel.xy)+0.5)*VmFroxelSize.zw;
    const float depth=VmBoundaryDepth(float(froxel.z)+0.5);
    return VmRelativeAtDepth(uv,depth)+VmOriginNear.xyz;
}
float VmPhaseFunction(float cosine,float g)
{
    const float denominator=1.0+g*g-2.0*g*clamp(cosine,-1.0,1.0);
    return (1.0-g*g)/(12.566370614359172*denominator*sqrt(denominator));
}
float VmDirectionalVisibility(HsGpuLightRecord light,float3 world)
{
    if(light.identity.w!=0u || light.metadata.z==0u) return 1.0;
    float visibility;
    if(VmCounts.z!=0u && HsVirtualShadowLookup(world,light.identity.xy,light.identity.z,visibility)) return visibility;
    if(VmShadowIdentity.w==0u || any(VmShadowIdentity.xyz!=light.identity.xyz)) return 1.0;
    visibility=1.0;
    [unroll] for(int cascade=2;cascade>=0;--cascade)
    {
        float4 projected=mul(float4(world,1.0),VmShadowViewProjection[cascade]);
        projected.xyz/=projected.w;
        const float2 uv=projected.xy*float2(0.5,-0.5)+0.5;
        if(any(uv<0.0)||any(uv>1.0)||projected.z<0.0||projected.z>1.0) continue;
        const float4 tile=VmShadowAtlasScaleOffset[cascade];
        const float2 atlas_uv=uv*tile.xy+tile.zw;
        const int2 lower=int2(ceil(tile.zw/VmShadowParameters.xy));
        const int2 upper=int2(floor((tile.zw+tile.xy)/VmShadowParameters.xy))-1;
        const int2 center=int2(floor(atlas_uv/VmShadowParameters.xy));
        float sum=0.0;
        [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x)
        {
            const float caster=VmCompactShadow.Load(int3(clamp(center+int2(x,y),lower,upper),0));
            sum+=projected.z+VmShadowParameters.z>=caster?1.0:0.0;
        }
        const float edge=min(min(uv.x,1.0-uv.x),min(uv.y,1.0-uv.y));
        visibility=lerp(visibility,sum/9.0,smoothstep(0.02,0.12,edge));
    }
    return visibility;
}
float3 VmHeroSeedOffset(uint seed)
{
    return float3(VmHash(seed^0x9e3779b9u)&0xffffu,
                  VmHash(seed^0x243f6a88u)&0xffffu,
                  VmHash(seed^0xb7e15162u)&0xffffu) * (1.0/65536.0);
}
float VmHeroFbm(float3 coordinate,uint seed)
{
    // Five authored octaves sample the production RGBA8 3D basis. R carries
    // low-frequency mass, G carries Worley erosion/void, B carries breakup,
    // and A carries phase/decorrelation noise. The seed only offsets wrapped
    // coordinates, keeping the authored channel meaning intact.
    const float3 seed_offset=VmHeroSeedOffset(seed);
    float sum=0.0,weight=0.5,total=0.0;
    [unroll] for(uint octave=0u;octave<5u;++octave)
    {
        const float scale=0.5*exp2(float(octave));
        const float3 uv=frac(coordinate*scale+seed_offset+
            float(octave)*float3(0.173,0.317,0.541));
        const float4 authored=VmHeroNoiseVolume.SampleLevel(VmWrapSampler,uv,0.0);
        float value=octave==0u?authored.r:octave==1u?(1.0-authored.g):
            octave==2u?authored.b:octave==3u?authored.a:authored.b;
        sum+=weight*value;total+=weight;weight*=0.5;
    }
    return saturate(sum/total);
}
float VmHeroDensity(VmHeroVolume volume,float3 world)
{
    const float3 local=mul(float4(world,1.0),volume.world_to_local).xyz;
    if(any(abs(local)>1.0))return 0.0;
    if(volume.sampling.w==0u)
    {
        const float radius=length(local);if(radius>=1.0)return 0.0;
        // Low-frequency buoyant turbulent flow, in canonical component space.
        const float3 flow=local*2.0+float3(0.12*sin(volume.optical.w*0.4),-volume.optical.w*0.22,0.0);
        const float noise=VmHeroFbm(flow,volume.sampling.z);
        return smoothstep(0.0,0.2,1.0-radius)*smoothstep(0.24,0.72,noise);
    }
    // A bounded annular crown with an inward curling density field. Geometry
    // constants describe this visual canonical shape, never the boss hitbox.
    const float radius=length(local.xz),angle=atan2(local.z,local.x);
    const float curl=angle+volume.optical.w*0.3+local.y*1.5+(1.0-radius)*4.0;
    const float outer=0.92+0.08*cos(6.0*angle);
    const float annulus=smoothstep(0.50,0.68,radius)*(1.0-smoothstep(outer-0.12,outer,radius));
    const float crest=0.35+0.65*pow(0.5+0.5*cos(6.0*angle),2.0);
    const float vertical=smoothstep(-1.0,-0.75,local.y)*(1.0-smoothstep(crest-0.2,crest,local.y));
    return annulus*vertical*(0.35+0.65*pow(0.5+0.5*sin(curl*3.0),2.0));
}
bool VmHeroIntersection(VmHeroVolume volume,float3 near_world,float3 far_world,out float2 interval)
{
    const float3 origin=mul(float4(near_world,1.0),volume.world_to_local).xyz;
    const float3 direction=mul(float4(far_world-near_world,0.0),volume.world_to_local).xyz;
    interval=float2(0.0,1.0);
    if(volume.sampling.w==0u)
    {
        const float a=dot(direction,direction);if(a<=0.0)return false;
        const float closest=-dot(origin,direction)/a;
        const float3 closest_point=origin+direction*closest;
        const float chord_squared=1.0-dot(closest_point,closest_point);
        if(chord_squared<0.0)return false;
        const float half_chord=sqrt(chord_squared/a);
        interval=float2(max(0.0,closest-half_chord),min(1.0,closest+half_chord));
    }
    else
    {
        [unroll] for(uint axis=0u;axis<3u;++axis)
        {
            if(direction[axis]==0.0)
            {if(abs(origin[axis])>1.0)return false;}
            else
            {
                const float2 face=float2(-1.0-origin[axis],1.0-origin[axis])/direction[axis];
                interval.x=max(interval.x,min(face.x,face.y));interval.y=min(interval.y,max(face.x,face.y));
            }
        }
    }
    return interval.y>interval.x;
}
float VmHeroCellAverage(VmHeroVolume volume,uint3 cell)
{
    const float2 uv=(float2(cell.xy)+0.5)*VmFroxelSize.zw;
    const float near_depth=VmOriginNear.w,far_depth=VmDepthRange.x;
    const float3 near_world=VmRelativeAtDepth(uv,near_depth)+VmOriginNear.xyz;
    const float3 far_world=VmRelativeAtDepth(uv,far_depth)+VmOriginNear.xyz;
    float2 interval;if(!VmHeroIntersection(volume,near_world,far_world,interval))return 0.0;
    const float depth_span=far_depth-near_depth;
    const float opaque_fraction=(VmColumnDepth.Load(int3(cell.xy,0))-near_depth)/depth_span;
    interval.y=min(interval.y,opaque_fraction);if(interval.y<=interval.x)return 0.0;
    const float2 cell_interval=(float2(VmBoundaryDepth(float(cell.z)),VmBoundaryDepth(float(cell.z)+1.0))-near_depth)/depth_span;
    if(cell_interval.y<=interval.x||cell_interval.x>=interval.y)return 0.0;
    const uint seed=volume.sampling.z;
    const uint2 noise_pixel=(cell.xy+uint2(seed,seed>>8))&63u;
    // The sample offset is shared by every cell on this ray. Exactly the
    // authored 12/18 bins cover the whole clipped hero interval; no per-froxel
    // multiplication of the authored integration budget.
    const float jitter=min(VmHeroBlueNoise.Load(int4(noise_pixel,VmFrameInfo.x&63u,0)),0.999999);
    const float step_size=(interval.y-interval.x)/float(volume.sampling.x);float integral=0.0;
    [loop] for(uint sample=0u;sample<volume.sampling.x;++sample)
    {
        const float begin=interval.x+float(sample)*step_size,end=begin+step_size;
        const float overlap=max(0.0,min(cell_interval.y,end)-max(cell_interval.x,begin));
        if(overlap==0.0)continue;
        const float t=begin+jitter*step_size;
        integral+=VmHeroDensity(volume,lerp(near_world,far_world,t))*overlap;
    }
    // Depth parameter and physical metres differ by a constant factor on the
    // ray, so this is the conservative physical cell-average coefficient.
    return integral/(cell_interval.y-cell_interval.x);
}
float VmHeroGradientAlpha(VmHeroVolume volume)
{
    uint width,rows;VmHeroGradient.GetDimensions(width,rows);
    const float x=volume.albedo.w*float(width-1u);const uint left=uint(floor(x)),row=uint(volume.emission.w);
    return max(0.0,lerp(VmHeroGradient.Load(int3(left,row,0)).a,
        VmHeroGradient.Load(int3(min(left+1u,width-1u),row,0)).a,frac(x)));
}
[numthreads(4,4,4)]
void VolumetricDensityCS(uint3 p:SV_DispatchThreadID)
{
    if(any(p.xy>=uint2(VmFroxelSize.xy))||p.z>=VmCounts.y) return;
    const float3 world=VmFroxelWorld(p);
    float4 medium=0.0; float phase=0.0,phase_weight=0.0;
    [loop] for(uint i=0u;i<VmCounts.x;++i)
    {
        const VmVolume volume=VmVolumes[i];
        const float3 local=mul(float4(world,1.0),volume.world_to_local).xyz;
        if(volume.identity.w==1u && any(abs(local)>1.0)) continue;
        if(volume.identity.w==2u && dot(local,local)>1.0) continue;
        const float density=volume.density_extinction.x;
        const float3 scattering=volume.scattering.xyz*density;
        medium+=float4(scattering,volume.density_extinction.y*density);
        const float weight=volume.density_extinction.y*density;
        phase+=volume.density_extinction.z*weight; phase_weight+=weight;
    }
    VmMediumOutput[p]=medium; VmPhaseOutput[p]=phase_weight>0.0?phase/phase_weight:0.0;
    if(VmHeroInfo.x!=0u)VmRawOutput[p]=0.0;
    if(p.z==0u)
    {
        const int2 first=int2(floor(float2(p.xy)*VmRenderSize.xy*VmFroxelSize.zw));
        const int2 last=min(int2(ceil(float2(p.xy+1u)*VmRenderSize.xy*VmFroxelSize.zw)),int2(VmRenderSize.xy));
        float nearest=VmDepthRange.x;
        [loop] for(int y=first.y;y<last.y;++y) [loop] for(int x=first.x;x<last.x;++x)
            nearest=min(nearest,VmPixelDepth(int2(x,y)));
        VmDepthOutput[p.xy]=nearest;
    }
}
[numthreads(4,4,4)]
void VolumetricHeroDensityCS(uint3 p:SV_DispatchThreadID)
{
    if(any(p.xy>=uint2(VmFroxelSize.xy))||p.z>=VmCounts.y)return;
    float4 medium=VmMediumOutput[p];float phase=VmPhaseOutput[p]*medium.w;float3 emission=0.0;
    [loop] for(uint index=0u;index<VmHeroInfo.x;++index)
    {
        const VmHeroVolume volume=VmHeroes[index];
        const float density=volume.optical.x*VmHeroGradientAlpha(volume)*VmHeroCellAverage(volume,p);
        const float extinction=density*volume.optical.y;
        medium+=float4(volume.albedo.xyz*extinction,extinction);
        phase+=extinction*volume.optical.z;emission+=density*volume.emission.xyz;
    }
    VmMediumOutput[p]=medium;VmPhaseOutput[p]=medium.w>0.0?phase/medium.w:0.0;
    VmRawOutput[p]+=float4(emission,0.0);
}
[numthreads(4,4,4)]
void VolumetricLightingCS(uint3 p:SV_DispatchThreadID)
{
    if(any(p.xy>=uint2(VmFroxelSize.xy))||p.z>=VmCounts.y) return;
    const float4 medium=VmMedium.Load(int4(p,0));
    const float3 emission=VmHeroInfo.x!=0u?VmRawOutput[p].xyz:0.0;
    // Zero scattering still carries absorption and independent hero emission.
    [branch] if(all(medium.xyz==0.0))
    {
        VmRawOutput[p]=float4(emission,medium.w);
        return;
    }
    const float2 uv=(float2(p.xy)+0.5)*VmFroxelSize.zw;
    const float3 world=VmFroxelWorld(p);
    const float3 view=normalize(VmRelativeAtDepth(uv,VmOriginNear.w)-VmRelativeAtDepth(uv,VmDepthRange.x));
    const float g=VmPhase.Load(int4(p,0)); float3 lighting=0.0;
    HsLightQuery query=HsBeginLightQuery(world,HsLightAllMask,true); uint index;
    [loop] while(HsNextLight(query,index))
    {
        const HsGpuLightRecord light=HsLightRecords[index];
        if(light.identity.w>=3u)
        {
            float range_weight;
            if(!HsAreaLightVisible(light,world,range_weight)) continue;
            [loop] for(uint sample=0u;sample<VmFrameInfo.w;++sample)
            {
                const float2 u=float2((float(sample)+VmRandom(p,sample,0u))/float(VmFrameInfo.w),VmRandom(p,sample,1u));
                float3 direction; float pdf;
                if(!HsAreaSampleDirection(light,world,u,direction,pdf)) continue;
                // Incoming propagation is -direction; outgoing is toward camera.
                const float visibility=VmHeroInfo.z!=0u?HsLocalShadowVisibility(light,world,0.0.xxx):1.0;
                lighting+=light.color_intensity.rgb*light.color_intensity.w*range_weight*
                    VmPhaseFunction(dot(-direction,view),g)*visibility/(pdf*float(VmFrameInfo.w));
            }
        }
        else
        {
            float3 direction,incident;
            if(!HsLightIncident(index,world,direction,incident)) continue;
            const float visibility=light.identity.w==0u?VmDirectionalVisibility(light,world):
                (VmHeroInfo.z!=0u?HsLocalShadowVisibility(light,world,0.0.xxx):1.0);
            lighting+=incident*VmPhaseFunction(dot(-direction,view),g)*visibility;
        }
    }
    VmRawOutput[p]=float4(medium.xyz*lighting+emission,medium.w);
}
float4 VmPreviousSample(float3 coordinate)
{
    const int3 base=int3(floor(coordinate)); const float3 fraction=frac(coordinate); float4 value=0.0;
    const int3 maximum=int3(VmPreviousFroxelSize.xy,VmCounts.y)-1;
    [unroll] for(int z=0;z<2;++z) [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x)
    {
        const float weight=(x!=0?fraction.x:1.0-fraction.x)*(y!=0?fraction.y:1.0-fraction.y)*(z!=0?fraction.z:1.0-fraction.z);
        value+=VmPreviousScattering.Load(int4(clamp(base+int3(x,y,z),int3(0,0,0),maximum),0))*weight;
    }
    return value;
}
[numthreads(4,4,4)]
void VolumetricTemporalCS(uint3 p:SV_DispatchThreadID)
{
    if(any(p.xy>=uint2(VmFroxelSize.xy))||p.z>=VmCounts.y) return;
    const float4 current=VmRawScattering.Load(int4(p,0)); float4 resolved=current;
    if(VmFrameInfo.z!=0u)
    {
        const float3 world=VmFroxelWorld(p),previous_relative=world-VmPreviousOriginNear.xyz;
        const float4 clip=mul(float4(previous_relative,1.0),VmPreviousViewProjection);
        const float previous_z=mul(float4(previous_relative,0.0),VmPreviousWorldToView).z;
        const float2 uv=clip.xy/clip.w*float2(0.5,-0.5)+0.5;
        bool valid=clip.w>0.0 && previous_z>=VmPreviousOriginNear.w && previous_z<=VmDepthRange.z && all(uv>=0.0)&&all(uv<1.0);
        if(valid)
        {
            const int2 previous_column=clamp(int2(uv*VmPreviousFroxelSize.xy),int2(0,0),int2(VmPreviousFroxelSize.xy)-1);
            const float old_depth=VmPreviousDepth.Load(int3(previous_column,0));
            const float depth=VmColumnDepth.Load(int3(p.xy,0));
            const bool sky=depth>=VmDepthRange.x,old_sky=old_depth>=VmDepthRange.z;
            const float2 current_uv=(float2(p.xy)+0.5)*VmFroxelSize.zw;
            const float3 opaque_world=VmRelativeAtDepth(current_uv,depth)+VmOriginNear.xyz;
            const float expected_z=mul(float4(opaque_world-VmPreviousOriginNear.xyz,0.0),VmPreviousWorldToView).z;
            valid=sky==old_sky && (sky || abs(old_depth-expected_z)<=VmFilterParameters.y*max(expected_z,VmPreviousOriginNear.w));
            // Geometry in front of only one endpoint is an occlusion change.
            valid=valid && ((VmBoundaryDepth(float(p.z)+0.5)<depth)==(previous_z<old_depth));
            if(valid)
            {
                const float slice=log2(previous_z/VmPreviousOriginNear.w)*float(VmCounts.y)/VmDepthRange.w-0.5;
                float4 previous=VmPreviousSample(float3(uv*VmPreviousFroxelSize.xy-0.5,slice));
                float4 lower=current,upper=current;
                [unroll] for(int axis=0;axis<3;++axis) [unroll] for(int sign=-1;sign<=1;sign+=2)
                {
                    int3 offset=0; offset[axis]=sign;
                    const int3 neighbor=clamp(int3(p)+offset,int3(0,0,0),int3(VmFroxelSize.xy,VmCounts.y)-1);
                    const float4 value=VmRawScattering.Load(int4(neighbor,0)); lower=min(lower,value); upper=max(upper,value);
                }
                previous=clamp(previous,lower,upper);
                resolved=lerp(current,previous,VmFilterParameters.x);
            }
        }
    }
    VmTemporalOutput[p]=resolved;
}
[numthreads(4,4,4)]
void VolumetricSpatialCS(uint3 p:SV_DispatchThreadID)
{
    if(any(p.xy>=uint2(VmFroxelSize.xy))||p.z>=VmCounts.y) return;
    const float center_depth=VmColumnDepth.Load(int3(p.xy,0));
    const float4 center=VmTemporalScattering.Load(int4(p,0)); float4 sum=0.0; float weights=0.0;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x)
    {
        const int2 column=clamp(int2(p.xy)+int2(x,y),int2(0,0),int2(VmFroxelSize.xy)-1);
        const float neighbor_depth=VmColumnDepth.Load(int3(column,0));
        const float4 value=VmTemporalScattering.Load(int4(column,p.z,0));
        const float edge=abs(neighbor_depth-center_depth)/max(center_depth,VmOriginNear.w);
        const float extinction_edge=abs(value.w-center.w)/max(max(value.w,center.w),1e-6);
        const float weight=exp(-float(x*x+y*y)*0.5-edge/max(VmFilterParameters.y,1e-6)-extinction_edge*4.0);
        sum+=value*weight; weights+=weight;
    }
    VmFilteredOutput[p]=sum/weights;
}
[numthreads(8,8,1)]
void VolumetricIntegrateCS(uint3 p:SV_DispatchThreadID)
{
    if(any(p.xy>=uint2(VmFroxelSize.xy))) return;
    HsVolumetricTransport integrated=HsVolumetricIdentity();
    VmLuminanceOutput[uint3(p.xy,0)]=0.0; VmTransmittanceOutput[uint3(p.xy,0)]=1.0;
    const float2 uv=(float2(p.xy)+0.5)*VmFroxelSize.zw;
    [loop] for(uint slice=0u;slice<VmCounts.y;++slice)
    {
        const float4 source=VmFilteredScattering.Load(int4(p.xy,slice,0));
        integrated=VmCompose(integrated,VmIntegrateCoefficient(source,
            VmSegmentMeters(uv,VmBoundaryDepth(float(slice)),VmBoundaryDepth(float(slice+1u)))));
        VmLuminanceOutput[uint3(p.xy,slice+1u)]=float4(integrated.radiance,0.0);
        VmTransmittanceOutput[uint3(p.xy,slice+1u)]=integrated.transmittance;
    }
}
[numthreads(8,8,1)]
void VolumetricResolveCS(uint3 p:SV_DispatchThreadID)
{
    if(any(p.xy>=uint2(VmRenderSize.xy))) return;
    const float4 hdr=VmHdr.Load(int3(p.xy,0)); const float2 uv=(float2(p.xy)+0.5)*VmRenderSize.zw;
    const HsVolumetricTransport transport=HsSampleVolumetricTransport(uv,VmPixelDepth(int2(p.xy)));
    VmResolvedOutput[p.xy]=float4(HsApplyVolumetricTransport(hdr.xyz,transport),hdr.w);
}
