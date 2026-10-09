// Environment colors use sRGB views; the Gaussian terrain source stays linear.
#include "material_surface.hlsli"
#include "bsdf_metallic_roughness.hlsli"
#include "light_list.hlsli"
#include "probe_ibl.hlsli"
#include "local_probe_sampling.hlsli"
Texture2D<float2> EnvironmentBrdfLut : register(t4, space2);
Texture2DArray<float4> RockBase : register(t5, space2);
Texture2DArray<float4> RockSurface : register(t6, space2);
Texture2DArray<float4> RockNormal : register(t7, space2);
Texture2DArray<float4> RockDetail : register(t8, space2);
Texture2DArray<float4> BarkBase : register(t9, space2);
Texture2DArray<float4> BarkSurface : register(t10, space2);
Texture2DArray<float4> BarkNormal : register(t11, space2);
Texture2DArray<float4> BarkDetail : register(t12, space2);
Texture2DArray<float4> GrassBase : register(t13, space2);
Texture2DArray<float4> GrassNormal : register(t14, space2);
Texture2DArray<float> GrassRoughness : register(t15, space2);
Texture2DArray<float4> LeafBase : register(t16, space2);
Texture2DArray<float4> LeafNormal : register(t17, space2);
Texture2DArray<float> LeafRoughness : register(t18, space2);
Texture2D<float4> GBufferMaterial : register(t20, space2);
Texture2D<float2> GBufferMotion : register(t21, space2);
SamplerState EnvironmentWrap : register(s3);
SamplerState FoliageClamp : register(s4);
uint EnvHash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16); }
float EnvRandom(uint x) { return (EnvHash(x) & 0xffffff) / 16777216.0; }
uint EnvCell(float2 p, uint seed) { return EnvHash(asuint((int)p.x) ^ EnvHash(asuint((int)p.y)) ^ seed); }
float EnvNoise(float2 p, uint seed)
{
    float2 i=floor(p), f=frac(p); float2 u=f*f*f*(f*(f*6-15)+10);
    return lerp(lerp(EnvRandom(EnvCell(i,seed)),EnvRandom(EnvCell(i+float2(1,0),seed)),u.x),
      lerp(EnvRandom(EnvCell(i+float2(0,1),seed)),EnvRandom(EnvCell(i+1,seed)),u.x),u.y)*2-1;
}
float2 EnvRotate(float2 p,uint r) { return r==0?p:r==1?float2(-p.y,p.x):r==2?-p:float2(p.y,-p.x); }
float3 EnvDecode(float2 rg) { float2 xy=rg*2-1; return float3(xy,sqrt(saturate(1-dot(xy,xy)))); }
float3 EnvRNM(float3 n,float3 d) { float3 t=n+float3(0,0,1),u=d*float3(-1,-1,1); return normalize(t*dot(t,u)/max(t.z,0.001)-u); }
struct EnvMaterial { float3 color; float3 normal; float3 detail; float roughness; float ao; float height; };
#ifndef HS_GENERATED_ENVIRONMENT_MATERIAL
// The generic PSOs retain the same context contract as authored material PSOs.
HsMaterialSurface HsMaterialEvaluateSurface(HsMaterialContext ctx)
{
    HsMaterialSurface material = (HsMaterialSurface)0;
    material.base_color = ctx.sampled_base_color;
    material.normal = ctx.normal;
    material.roughness = saturate(ctx.sampled_roughness * ctx.roughness_scale);
    material.metallic = ctx.metallic;
    material.ao = ctx.sampled_ao;
    material.opacity = ctx.sampled_alpha;
    material.normal_is_world = false;
    return material;
}
float HsMaterialEvaluateOpacity(HsMaterialContext ctx) { return ctx.sampled_alpha; }
#endif
EnvMaterial EnvOpaque(float2 uv,float2 dx,float2 dy,uint layer,bool rock,float distanceToEye)
{
    EnvMaterial m=(EnvMaterial)0;
    float3 p=float3(uv,layer);
    m.color=rock?RockBase.SampleGrad(EnvironmentWrap,p,dx,dy).rgb:BarkBase.SampleGrad(EnvironmentWrap,p,dx,dy).rgb;
    float2 ra=rock?RockSurface.SampleGrad(EnvironmentWrap,p,dx,dy).rg:BarkSurface.SampleGrad(EnvironmentWrap,p,dx,dy).rg;
    float3 n=EnvDecode(rock?RockNormal.SampleGrad(EnvironmentWrap,p,dx,dy).rg:BarkNormal.SampleGrad(EnvironmentWrap,p,dx,dy).rg);
    float3 d=EnvDecode(rock?RockDetail.SampleGrad(EnvironmentWrap,p,dx,dy).rg:BarkDetail.SampleGrad(EnvironmentWrap,p,dx,dy).rg);
    m.normal=EnvRNM(n,normalize(lerp(float3(0,0,1),d,1-smoothstep(35,55,distanceToEye))));
    m.roughness=ra.x;m.ao=ra.y;
    return m;
}
void EnvironmentLodClip(SceneOutput input)
{
    if(input.Mesh<16 || input.Mesh>19 || (input.EnvironmentFlags&0x10000)==0) return;
    // Both LOD surfaces use the same coverage decision at a raster sample,
    // even when decimation changes their interpolated world positions.
    uint2 cell=(uint2)input.Position.xy;
    uint hash=EnvHash(cell.x^EnvHash(cell.y));
    float sampleValue=(hash&0xffffff)/16777216.0;
    bool below=sampleValue<saturate(input.EnvironmentFade);
    bool complement=(input.EnvironmentFlags&0x20000)!=0;
    clip(below!=complement?1.0:-1.0);
}
static const float4 GrassRects[30] = {
float4(48,48,690,575),
float4(834,48,676,538),
float4(834,682,631,463),
float4(48,719,605,377),
float4(48,1192,540,573),
float4(1606,48,355,564),
float4(1561,708,419,555),
float4(684,1359,428,555),
float4(1208,1359,459,512),
float4(1763,1359,203,500),
float4(48,48,481,325),
float4(48,469,183,470),
float4(48,1035,275,454),
float4(327,469,191,450),
float4(419,1015,403,442),
float4(419,1553,364,441),
float4(48,1585,177,412),
float4(879,1553,226,404),
float4(614,469,171,387),
float4(881,48,215,369),
float4(881,513,218,360),
float4(1192,48,330,342),
float4(625,48,106,320),
float4(918,969,166,318),
float4(1180,969,119,306),
float4(1195,486,259,295),
float4(1618,48,280,290),
float4(1618,434,158,277),
float4(1550,807,168,276),
float4(1872,434,93,273)
};
static const uint GrassPages[30] = {0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
static const float4 LeafRects[18] = {
float4(48,48,737,525),
float4(881,48,688,439),
float4(881,583,609,470),
float4(48,669,591,372),
float4(48,1137,589,498),
float4(733,1149,453,584),
float4(1282,1149,432,570),
float4(48,48,569,458),
float4(713,48,565,491),
float4(48,602,547,523),
float4(1374,48,547,455),
float4(1374,599,533,540),
float4(691,635,484,515),
float4(48,1221,493,515),
float4(1271,1235,502,304),
float4(637,1246,483,443),
float4(48,48,446,453),
float4(1586,583,397,404)
};
static const uint LeafPages[18] = {0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,2,0};
// Group lists keep a single leaf species on every canopy instance.
static const uint MapleEntries[3]={0,9,11};
static const uint OakEntries[4]={2,4,7,12};
static const uint OvalEntries[11]={1,3,5,6,8,10,13,14,15,16,17};
uint FoliageEntry(uint mesh,uint seed,uint card)
{
    uint h=EnvHash(seed+card*137);
    if(mesh==19) return h%30;
    uint species=EnvHash(seed)%3;
    return species==0?MapleEntries[h%3]:species==1?OakEntries[h%4]:OvalEntries[h%11];
}
float3 FoliageUv(uint mesh,uint entry,float2 uv)
{
    float4 rect=mesh==19?GrassRects[entry]:LeafRects[entry];
    return float3((rect.xy+saturate(uv)*rect.zw)/2048.0,mesh==19?GrassPages[entry]:LeafPages[entry]);
}
float4 FoliageColor(uint mesh,uint entry,float2 uv)
{
    float3 p=FoliageUv(mesh,entry,uv);
    return mesh==19?GrassBase.Sample(FoliageClamp,p):LeafBase.Sample(FoliageClamp,p);
}
void EnvironmentVertex(inout SceneInput input,InstanceData instance,uint vertex,uint seed,out uint entry)
{
    uint mesh=instance.Mesh&255;entry=0;
    if((instance.Mesh&0x10000)!=0) {entry=input.Material;return;}
    if(mesh!=16 && mesh!=17 && mesh!=19) return;
    static const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    uint card=vertex/6;float2 uv=corners[vertex%6];
    if(mesh==16)
    {
        float u=(card+uv.x)/6.0,theta=u*6.2831853;float radius=lerp(0.36,0.21,uv.y);
        input.Position=float3(cos(theta)*radius,uv.y-0.5,sin(theta)*radius);
        input.Normal=normalize(float3(cos(theta),0.15,sin(theta)));
        input.Tangent=float4(-sin(theta),0,cos(theta),-1);
        input.Uv=float2(u*max(1,round(3.14159265*0.57*(instance.Scale.x+instance.Scale.z)*0.5)),uv.y*instance.Scale.y);
        return;
    }
    entry=FoliageEntry(mesh,seed,card);float4 rect=mesh==19?GrassRects[entry]:LeafRects[entry];
    float angle=card*2.399963+EnvRandom(seed)*6.2831853;float3 tangent=float3(cos(angle),0,sin(angle));
    float aspect=rect.z/rect.w;
    float height=mesh==19?0.65:0.68;float3 center=mesh==19?float3(0,-0.5,0):float3(cos(angle)*0.28,0.18+(card%3)*0.22,sin(angle)*0.28);
    input.Position=center+tangent*((uv.x-0.5)*height*aspect)+float3(0,uv.y*height,0);
    input.Normal=float3(-sin(angle),0,cos(angle)); input.Tangent=float4(tangent,-1);
    input.Uv=float2(uv.x,1-uv.y);
}
Texture2D<float> SceneAmbientOcclusion : register(t5, space9);
Texture2D<float> SceneContactShadow : register(t6, space9);

float3 EnvironmentSkyRadiance(float3 direction)
{
    return ProbeOptions.w>0.0 ? HsProbeRadiance(direction) :
        lerp(float3(0.055,0.065,0.045),float3(0.24,0.31,0.42),saturate(direction.y*0.5+0.5));
}

float3 HsSceneProbeRadiance(float3 world,float3 direction)
{
#ifdef HS_LOCAL_PROBE_CAPTURE
    return HsProbeRadiance(direction);
#else
    return HsLocalProbeRadiance(world,direction);
#endif
}
float3 HsSceneProbeIrradiance(float3 world,float3 normal)
{
#ifdef HS_LOCAL_PROBE_CAPTURE
    return HsProbeIrradiance(normal);
#else
    return HsLocalProbeIrradiance(world,normal);
#endif
}
float3 HsSceneProbePrefiltered(float3 world,float3 direction,float roughness)
{
#ifdef HS_LOCAL_PROBE_CAPTURE
    return HsProbePrefiltered(direction,roughness);
#else
    return HsLocalProbePrefiltered(world,direction,roughness);
#endif
}
void EnvironmentIndirectLightingComponents(HsSurfaceClosure closure, float3 world, float3 view_direction,
    float base_fresnel_override, bool indirect_diffuse_enabled, bool indirect_specular_enabled,
    out float3 ambient_diffuse, out float3 ambient_specular)
{
    float3 v=normalize(view_direction);
    closure=HsBsdfOrientThinSurface(closure,v);
    float3 n=normalize(closure.normal),base=closure.base_color;
    float nv=saturate(dot(n,v));
    float metallic=saturate(closure.metallic),dielectric=1-metallic;
    float3 F0=HsBsdfF0(closure);
    ambient_diffuse=0.0;
    ambient_specular=0.0;
    if(closure.coat_weight>0.0 || closure.anisotropy!=0.0 ||
       (closure.sheen_weight>0.0 && any(closure.sheen_color>0.0)))
    {
        // Use the existing matched mixture and its exact sample sequence;
        // component separation changes neither PDFs nor null-sample accounting.
        const uint sample_count=32u;
        [loop] for(uint index=0u;index<sample_count;++index)
        {
            float u_lobe=frac((float(index)+0.5)*0.6180339887498948482);
            float2 u_dir=float2((float(index)+0.5)/float(sample_count),
                float(HsBsdfReverseBits(index))*2.3283064365386962890625e-10);
            HsBsdfSampleResult sample=HsBsdfSample(closure,v,u_lobe,u_dir);
            if(!sample.valid || sample.pdf<=0.0) continue;
            float3 sample_diffuse,sample_specular;
            HsBsdfEvaluateComponentsOptical(closure,v,sample.wi,base_fresnel_override,sample_diffuse,sample_specular);
            float3 baseline_sky=EnvironmentSkyRadiance(sample.wi);
            float3 diffuse_sky=ProbeOptions.w>0.0 && indirect_diffuse_enabled ?
                HsSceneProbeRadiance(world,sample.wi) : baseline_sky;
            float3 specular_sky=ProbeOptions.w>0.0 && indirect_specular_enabled ?
                HsSceneProbeRadiance(world,sample.wi) : baseline_sky;
            float3 weight=abs(dot(n,sample.wi))/sample.pdf;
            ambient_diffuse+=sample_diffuse*diffuse_sky*weight;
            ambient_specular+=sample_specular*specular_sky*weight;
        }
        float normalization=saturate(closure.ao)/float(sample_count);
        ambient_diffuse*=normalization;
        ambient_specular*=normalization;
        return;
    }
    float thinWeight=HsBsdfThinTransmissionWeight(closure);
    float3 irradiance=ProbeOptions.w>0.0 ?
        (indirect_diffuse_enabled ? HsSceneProbeIrradiance(world,n) : HsProbeIrradiance(n))*HsBsdfInvPi :
        lerp(float3(0.07,0.08,0.055),float3(0.22,0.28,0.36),n.y*0.5+0.5);
    // Cosine-weighted Schlick transmission averages to (1-F0)*20/21.
    // Match the entry/exit substrate attenuation in HsBsdfEvaluate.
    float3 diffuseAlbedo=base*(1-HsBsdfSchlickFresnel(F0,nv))*(1-F0)*(20.0/21.0)*dielectric;
    ambient_diffuse=diffuseAlbedo*irradiance*saturate(closure.ao);
    if(thinWeight>0.0)
    {
        // A linear hemispherical sky integrates exactly over a cosine sheet
        // lobe: E[wi.y] = -2*n.y/3. Dense vegetation needs no sample loop.
        float3 skyBottom=float3(0.055,0.065,0.045),skyTop=float3(0.24,0.31,0.42);
        float3 backSky=ProbeOptions.w>0.0 ?
            (indirect_diffuse_enabled ? HsSceneProbeIrradiance(world,-n) : HsProbeIrradiance(-n))*HsBsdfInvPi :
            (skyBottom+skyTop)*0.5-(skyTop-skyBottom)*n.y/3.0;
        ambient_diffuse=ambient_diffuse*(1.0-thinWeight)+HsBsdfThinTransmissionColor(closure)*
            thinWeight*backSky*saturate(closure.ao);
    }
    // The basic lobe uses the same sampled DFG as the authored GGX cube mips.
    float2 AB=EnvironmentBrdfLut.SampleLevel(LinearClamp,float2(nv,saturate(closure.roughness)),0);
    float3 reflected=reflect(-v,n);float skyT=lerp(saturate(reflected.y*0.5+0.5),0.5,closure.roughness*closure.roughness);
    float3 sky=ProbeOptions.w>0.0 ?
        (indirect_specular_enabled ? HsSceneProbePrefiltered(world,reflected,saturate(closure.roughness)) :
            HsProbePrefiltered(reflected,saturate(closure.roughness))) :
        lerp(float3(0.055,0.065,0.045),float3(0.24,0.31,0.42),skyT);
    const float3 integrated_fresnel=base_fresnel_override>=0.0 ?
        lerp(F0*AB.x+AB.y,saturate(base_fresnel_override).xxx*(AB.x+AB.y),saturate(closure.solid_transmission_weight)) :
        F0*AB.x+AB.y;
    ambient_specular=sky*integrated_fresnel*saturate(closure.ao)*(1.0-thinWeight);
}

void EnvironmentLightingComponents(HsSurfaceClosure closure, float3 world, float visibility,
    float base_fresnel_override, out float3 diffuse, out float3 specular, out float3 ambient_specular,
    out float3 ambient_diffuse)
{
    const float3 v=normalize(CameraTime.xyz-world);
    closure=HsBsdfOrientThinSurface(closure,v);
    HsLightListEvaluateComponentsOptical(closure,world,CameraTime.xyz,visibility,
        HsLightDirectionalMask,false,base_fresnel_override,diffuse,specular);
    EnvironmentIndirectLightingComponents(closure,world,v,base_fresnel_override,
        LightingOptions.z>0.0,LightingOptions.w>0.0,ambient_diffuse,ambient_specular);
    diffuse+=ambient_diffuse;
    specular+=ambient_specular;
}

void EnvironmentLightingComponents(HsSurfaceClosure closure, float3 world, float visibility,
    float base_fresnel_override, out float3 diffuse, out float3 specular, out float3 ambient_specular)
{
    float3 ambient_diffuse;
    EnvironmentLightingComponents(closure,world,visibility,base_fresnel_override,
        diffuse,specular,ambient_specular,ambient_diffuse);
}

void EnvironmentLightingComponents(HsSurfaceClosure closure,float3 world,float visibility,
    out float3 diffuse,out float3 specular,out float3 ambient_specular)
{
    EnvironmentLightingComponents(closure,world,visibility,-1.0,diffuse,specular,ambient_specular);
}

float3 EnvironmentLighting(HsSurfaceClosure closure,float3 world,float visibility)
{
    float3 diffuse,specular,ambient_specular;
    EnvironmentLightingComponents(closure,world,visibility,diffuse,specular,ambient_specular);
    return diffuse+specular+closure.emission;
}
