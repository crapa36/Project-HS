#ifndef HS_MATERIAL_TERRAIN_HLSLI
#define HS_MATERIAL_TERRAIN_HLSLI

#include "texture_streaming.hlsli"
#include "meadow_surface.hlsli"

#ifndef HS_MATERIAL_POM_STEPS
#define HS_MATERIAL_POM_STEPS 24u
#endif
#ifndef HS_MATERIAL_POM_REFINEMENT_STEPS
#define HS_MATERIAL_POM_REFINEMENT_STEPS 3u
#endif
#ifndef HS_MATERIAL_DETAIL_FADE_RANGE
// Fixed standalone material compatibility policy; native raster uploads its
// authored package range through the packed binary16 frame lane.
#define HS_MATERIAL_DETAIL_FADE_RANGE float2(35.0,55.0)
#endif

// The terrain material owns the stochastic terrain algorithm.  All texture
// resources, gradients and view data are arguments so generated material code
// never reaches through an implicit camera or derivative global.
struct HsMaterialTerrainResult
{
    float3 base_color;
    float3 normal;
    float3 detail;
    float roughness;
    float ao;
    float height;
};

uint TerrainHash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16); }
float TerrainRandom(uint x) { return (TerrainHash(x) & 0xffffff) / 16777216.0; }
uint TerrainCell(float2 p, uint seed) { return TerrainHash(asuint((int)p.x) ^ TerrainHash(asuint((int)p.y)) ^ seed); }
float TerrainNoise(float2 p, uint seed)
{
    float2 i=floor(p), f=frac(p); float2 u=f*f*f*(f*(f*6-15)+10);
    return lerp(lerp(TerrainRandom(TerrainCell(i,seed)),TerrainRandom(TerrainCell(i+float2(1,0),seed)),u.x),
      lerp(TerrainRandom(TerrainCell(i+float2(0,1),seed)),TerrainRandom(TerrainCell(i+1,seed)),u.x),u.y)*2-1;
}
float2 TerrainRotate(float2 p,uint r) { return r==0?p:r==1?float2(-p.y,p.x):r==2?-p:float2(p.y,-p.x); }
float3 TerrainDecode(float2 rg) { float2 xy=rg*2-1; return float3(xy,sqrt(saturate(1-dot(xy,xy)))); }
float TerrainTileSize(uint layer) { return layer==3 ? 9.0 : 12.0; }
float2 TerrainDetailFadeRange()
{
    // Native frame Z is two IEEE binary16 distances, not an enum or arithmetic
    // float value. The package validates positivity/order after half rounding.
    return HS_MATERIAL_DETAIL_FADE_RANGE;
}

float2 TerrainParallaxUv(Texture2DArray<float4> height, SamplerState environment_wrap,
    float2 uv, float2 dx, float2 dy, uint layer, uint rotation,
    float3 viewTs, float distanceToEye, bool pom, uint steps, uint refinementSteps, uint height_slot)
{
    // POM-off performs no height lookup. The separate authored height output
    // remains available to the material's height-based layer blending.
    if (!pom || steps == 0u || distanceToEye >= 55.0) return uv;
    float tile=TerrainTileSize(layer); float heightScale=layer==3?0.05:layer==2?0.035:0.025;
    float2 ray=TerrainRotate(viewTs.xy,rotation)/max(abs(viewTs.z),0.05)*heightScale/tile*(1-smoothstep(35,55,distanceToEye));
    float pomWeight=1;
    const float gradientEnergy=dot(dx,dx)+dot(dy,dy);
    const float gradientArea=abs(dx.x*dy.y-dx.y*dy.x);
    // The inverse-Jacobian norm bounds displacement even across anisotropic footprints.
    if(gradientEnergy>1e-20 && gradientArea>gradientEnergy*1e-6)
    {
        const float parallaxPixels=length(ray)*(sqrt(gradientEnergy)/gradientArea);
        if(isfinite(parallaxPixels)) pomWeight=smoothstep(0.25,0.5,parallaxPixels);
        [branch] if(pomWeight<=0) return uv;
    }
    const HsTextureGradState height_state = HsTexturePrepareGrad(height_slot);
    float depth=0,previous=0; float2 start=uv;
    [loop] for(uint j=0;j<steps;++j)
    {
        if(depth>=1-HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(uv,layer),dx,dy,height_state).r) break;
        previous=depth; depth+=1.0/steps; uv=start-ray*depth;
    }
    [loop] for(uint k=0;k<refinementSteps;++k)
    {
        float mid=(previous+depth)*0.5; float2 trial=start-ray*mid;
        if(mid>=1-HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(trial,layer),dx,dy,height_state).r) depth=mid;
        else previous=mid;
    }
    return start-ray*((previous+depth)*0.5*pomWeight);
}

HsMaterialTerrainResult TerrainSample(
    Texture2DArray<float4> gaussian, Texture2DArray<float4> surface,
    Texture2DArray<float4> normal, Texture2DArray<float4> detail,
    Texture2DArray<float4> height, SamplerState environment_wrap,
    float2 baseUv, float2 baseDx, float2 baseDy, uint layer, uint seed,
    float3 viewTs, float distanceToEye, bool pom,
    uint gaussian_slot, uint surface_slot, uint normal_slot, uint detail_slot, uint height_slot)
{
    uint rotation=TerrainHash(seed)&3; float2 offset=float2(TerrainRandom(seed+1),TerrainRandom(seed+2));
    float2 uv=TerrainRotate(baseUv,rotation)+offset, dx=TerrainRotate(baseDx,rotation),dy=TerrainRotate(baseDy,rotation);
    uv=TerrainParallaxUv(height,environment_wrap,uv,dx,dy,layer,rotation,viewTs,
        distanceToEye,pom,HS_MATERIAL_POM_STEPS,HS_MATERIAL_POM_REFINEMENT_STEPS,height_slot);
    HsMaterialTerrainResult m=(HsMaterialTerrainResult)0;
    m.base_color=HsTextureSampleGrad(gaussian,gaussian_slot,environment_wrap,float3(uv,layer),dx,dy).rgb;
    float2 ra=HsTextureSampleGrad(surface,surface_slot,environment_wrap,float3(uv,layer),dx,dy).rg; m.roughness=ra.x;m.ao=ra.y;
    float3 n=TerrainDecode(HsTextureSampleGrad(normal,normal_slot,environment_wrap,float3(uv,layer),dx,dy).rg);
    const float2 detail_fade=TerrainDetailFadeRange();
    float3 d=float3(0,0,1);
    if(distanceToEye<detail_fade.y)
    {
        d=TerrainDecode(HsTextureSampleGrad(detail,detail_slot,environment_wrap,float3(uv,layer),dx,dy).rg);
        d=normalize(lerp(float3(0,0,1),d,1-smoothstep(detail_fade.x,detail_fade.y,distanceToEye)));
    }
    n.xy=TerrainRotate(n.xy,(4-rotation)&3);d.xy=TerrainRotate(d.xy,(4-rotation)&3);m.normal=n;m.detail=d;
    m.height=HsTextureSampleGrad(height,height_slot,environment_wrap,float3(uv,layer),dx,dy).r;return m;
}

float3 TerrainGrassPalette(float3 color,float coverage)
{
    // Linear mean of both foliage_grass_atlas_[0,1]_basecolor.png atlases, alpha >= 0.5.
    const float3 foliageAlbedo=float3(0.099848532,0.159184398,0.020007386);
    // Per-channel gain keeps soil/leaf color differences instead of replacing them with one hue.
    const float3 groundAlbedo=float3(0.165660347,0.151494940,0.047493307);
    // Match the lit canopy rather than the brighter upward-facing ground under daylight.
    const float3 canopyGain=float3(0.5,0.72,0.08);
    return color*lerp(float3(1,1,1),foliageAlbedo/groundAlbedo*canopyGain,coverage);
}

float3 DecodeTerrainHistogram(Texture2DArray<float4> histogram, SamplerState linear_clamp,
                              float3 gaussian,uint layer,float lod)
{
    float row=(clamp(lod,0,11)+0.5)/16;
    float3 g=clamp(gaussian,0.5/256,1-0.5/256);
    float3 p=float3(
        histogram.SampleLevel(linear_clamp,float3(g.x,row,layer),0).r,
        histogram.SampleLevel(linear_clamp,float3(g.y,row,layer),0).g,
        histogram.SampleLevel(linear_clamp,float3(g.z,row,layer),0).b);
    float3 origin=histogram.Load(int4(0,12,layer,0)).rgb;
    float3 x=histogram.Load(int4(0,13,layer,0)).rgb;
    float3 y=histogram.Load(int4(0,14,layer,0)).rgb;
    float3 z=histogram.Load(int4(0,15,layer,0)).rgb;
    return saturate(origin+x*p.x+y*p.y+z*p.z);
}

HsMaterialTerrainResult TerrainLayer(
    Texture2DArray<float4> gaussian, Texture2DArray<float4> surface,
    Texture2DArray<float4> normal, Texture2DArray<float4> detail,
    Texture2DArray<float4> height, Texture2DArray<float4> histogram,
    SamplerState environment_wrap, SamplerState linear_clamp,
    float3 world,float3 worldDx,float3 worldDy,uint layer,uint seed,float2 warp,
    float3 viewDirection,float distanceToEye,
    uint gaussian_slot,uint surface_slot,uint normal_slot,uint detail_slot,uint height_slot,uint histogram_slot)
{
    float tile=TerrainTileSize(layer);
    float2 uv=world.xz/tile,dx=worldDx.xz/tile,dy=worldDy.xz/tile;
    float3 ts=normalize(viewDirection).xzy;

    float2 p=(world.xz+warp*1.3)/(tile*0.9),i=floor(p),f=frac(p);
    float2 a=i,b=i+float2(1,0),c=i+float2(0,1);
    float3 w=float3(1-f.x-f.y,f.x,f.y);
    if(f.x+f.y>=1) {a=i+1;b=i+float2(0,1);c=i+float2(1,0);w=float3(f.x+f.y-1,1-f.x,1-f.y);}
    w=w*w*(3-2*w);w/=max(w.x+w.y+w.z,1e-5);
    HsMaterialTerrainResult x=TerrainSample(gaussian,surface,normal,detail,height,environment_wrap,uv,dx,dy,layer,TerrainCell(a,seed),ts,distanceToEye,true,gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot);
    HsMaterialTerrainResult y=TerrainSample(gaussian,surface,normal,detail,height,environment_wrap,uv,dx,dy,layer,TerrainCell(b,seed),ts,distanceToEye,true,gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot);
    HsMaterialTerrainResult z=TerrainSample(gaussian,surface,normal,detail,height,environment_wrap,uv,dx,dy,layer,TerrainCell(c,seed),ts,distanceToEye,true,gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot);
    float3 gaussian_value=0.5+(x.base_color*w.x+y.base_color*w.y+z.base_color*w.z-0.5)*rsqrt(dot(w,w));
    float lod=0.5*log2(max(max(dot(dx,dx),dot(dy,dy))*2048*2048,1e-8));
    // Decode with the mip actually available to the Gaussian sample, including streaming fallback.
    lod=max(lod,float(HsTextureMipMetadata[gaussian_slot].y));
    x.base_color=DecodeTerrainHistogram(histogram,linear_clamp,gaussian_value,layer,lod);
    x.normal=normalize(x.normal*w.x+y.normal*w.y+z.normal*w.z);
    x.detail=normalize(x.detail*w.x+y.detail*w.y+z.detail*w.z);
    x.roughness=x.roughness*w.x+y.roughness*w.y+z.roughness*w.z;
    x.ao=x.ao*w.x+y.ao*w.y+z.ao*w.z;
    x.height=x.height*w.x+y.height*w.y+z.height*w.z;
    return x;
}

float TerrainMeadowBlend(float2 world)
{
    return smoothstep(0.0,1.5,12.0-max(abs(world.x),abs(world.y)));
}

float TerrainMeadowRoad(float2 world,float2 warp,float edgeDetail)
{
    const float edge=warp.x*.12+warp.y*.07+edgeDetail*.09;
    return max(MeadowPressed(world)*TerrainMeadowBlend(world),MeadowRoadCoverage(world,edge)*.82);
}

// Fixed metre-scale environmental marks for terrain materials.
// Albedo gains preserve the sampled 2K detail; these are shallow material depressions, not water.
void TerrainGroundTraces(inout HsMaterialTerrainResult m,float3 world,float path,float grassCoverage,uint mesh,uint seed,float wetness,float dryness)
{
    const float meadow=TerrainMeadowBlend(world.xz);
    if(meadow>0)
    {
        const float wet=wetness*meadow;
        const float rut=MeadowCartWear(world.xz)*meadow;
        m.base_color*=lerp(float3(1,1,1),float3(.72,.68,.62),rut);
        m.base_color*=lerp(float3(1,1,1),float3(.65,.60,.53),wet);
        m.roughness=lerp(m.roughness,.48,wet);
        m.roughness=lerp(m.roughness,.24,smoothstep(.82,1.0,wet)*.65);
        m.roughness=lerp(m.roughness,max(m.roughness,.88),dryness*.7);
        if(meadow>=1) return;
    }
    if(mesh==20u||path<=0.015) return;
    float soil=path*(1.0-grassCoverage)*(1.0-meadow);
    float across=world.x-sin(world.z*0.085)*6.5;
    float fine=TerrainNoise(world.xz/1.3,seed+4013);
    float broad=TerrainNoise(world.xz/4.3,seed+4051);

    // One jittered low spot in only some 22 m path sections, kept clear of section seams.
    float section=floor((world.z+73.0)/22.0),sectionZ=world.z+73.0-section*22.0;
    uint key=TerrainCell(float2(0,section),seed+4093);
    float2 basinCentre=float2((TerrainRandom(key+1)-0.5)*2.8,3.0+16.0*TerrainRandom(key+2));
    float2 basinRadius=float2(0.35+0.65*TerrainRandom(key+3),0.65+1.8*TerrainRandom(key+4));
    float2 basinQ=(float2(across,sectionZ)-basinCentre)/basinRadius;
    float basin=dot(basinQ,basinQ)+fine*0.22+broad*0.12;
    float lowSpot=TerrainRandom(key+5)>0.48?soil:0.0;
    float wet=lowSpot*(1.0-smoothstep(0.25,1.0,basin));
    float dryRim=lowSpot*smoothstep(0.25,0.6,basin)*(1.0-smoothstep(0.8,1.3,basin));

    // Paired 12-28 cm ruts, 1.64 m apart; gaps and wandering edges break the continuous rails.
    float rutSection=floor((world.z+19.0)/14.0),rutZ=world.z+19.0-rutSection*14.0;
    uint rutKey=TerrainCell(float2(1,rutSection),seed+4127);
    float rutStart=1.0+2.0*TerrainRandom(rutKey+1),rutEnd=rutStart+4.0+4.0*TerrainRandom(rutKey+2);
    float rutWindow=smoothstep(rutStart,rutStart+0.3,rutZ)*(1.0-smoothstep(rutEnd-0.4,rutEnd,rutZ));
    float rutOffset=abs(across)-0.82-fine*0.045;
    float ruts=(1.0-smoothstep(0.06,0.14,abs(rutOffset)))*rutWindow*soil;
    ruts*=TerrainRandom(rutKey+3)>0.35?saturate(0.65+broad*0.5):0.0;

    // Short, occasional alternating shoe rows; per-step jitter avoids stamped identical pairs.
    float footStart=3.0+9.0*TerrainRandom(key+7),footLength=2.2+1.7*TerrainRandom(key+8);
    float walkZ=sectionZ-footStart,stepIndex=floor(walkZ/0.62),stepZ=walkZ-stepIndex*0.62;
    uint footKey=TerrainCell(float2(stepIndex,section),seed+4153);
    float footAcross=(TerrainRandom(key+9)-0.5)*0.8+((int(stepIndex)&1)==0?-0.12:0.12);
    footAcross+=(TerrainRandom(footKey+1)-0.5)*0.07;
    float2 shoe=float2((across-footAcross)/(0.055+0.012*TerrainRandom(footKey+2)),
                      (stepZ-0.31)/(0.125+0.02*TerrainRandom(footKey+3)));
    float shoeWindow=smoothstep(0.0,0.2,walkZ)*(1.0-smoothstep(footLength-0.2,footLength,walkZ));
    float footprints=(1.0-smoothstep(0.65,1.0,dot(shoe,shoe)))*shoeWindow*soil;
    footprints*=TerrainRandom(key+10)>0.72?1.0:0.0;

    float edge=smoothstep(0.03,0.16,path)*(1.0-smoothstep(0.45,0.8,path));
    float moss=edge*smoothstep(0.12,0.55,broad+fine*0.25)*(1.0-meadow);
    float pressed=max(ruts,footprints);
    m.base_color*=lerp(float3(1,1,1),float3(0.54,0.50,0.43),wet);
    m.base_color*=lerp(float3(1,1,1),float3(0.88,0.82,0.72),dryRim);
    m.base_color*=lerp(float3(1,1,1),float3(0.72,0.67,0.58),pressed);
    m.base_color*=lerp(float3(1,1,1),float3(0.58,0.92,0.38),moss);
    m.roughness=lerp(m.roughness,0.3,wet);
    m.roughness=saturate(m.roughness+dryRim*0.08);
    m.roughness=lerp(m.roughness,max(m.roughness,0.92),moss);
    m.ao*=1.0-pressed*0.08-moss*0.05;
    float2 slope=-clamp(basinQ,-1.0,1.0)*float2(0.025,0.018)*wet;
    slope.x-=sign(across)*sign(rutOffset)*ruts*0.025;
    slope-=clamp(shoe,-1.0,1.0)*float2(0.025,0.018)*footprints;
    m.normal=normalize(float3(m.normal.xy+slope,m.normal.z));
}

HsMaterialTerrainResult EnvTerrain(
    Texture2DArray<float4> gaussian, Texture2DArray<float4> surface,
    Texture2DArray<float4> normal, Texture2DArray<float4> detail,
    Texture2DArray<float4> height, Texture2DArray<float4> histogram,
    SamplerState environment_wrap, SamplerState linear_clamp,
    float3 world,float3 worldDx,float3 worldDy,uint mesh,uint seed,
    float3 viewDirection,float distanceToEye,
    uint gaussian_slot,uint surface_slot,uint normal_slot,uint detail_slot,uint height_slot,uint histogram_slot,
    float2 authoredControls)
{
    float4 w=max(float4(0.5-TerrainNoise(world.xz/12,seed),0.7+TerrainNoise(world.xz/16,seed+9),TerrainNoise(world.xz/22,seed+31),TerrainNoise(world.xz/9,seed+71)),0);
    w.x=max(w.x,0.05);
    const float2 warp=float2(TerrainNoise(world.xz/7,seed+413),TerrainNoise(world.xz/7,seed+827));
    const float meso=TerrainNoise(world.xz/.85,seed+571);
    const float path=TerrainMeadowRoad(world.xz,warp,meso);
    // Static controls use meadow UVs; texture mapping remains in world metres.
    float wetness,dryness;
    [branch] if(mesh==22u)
    {
        wetness=saturate(authoredControls.x);dryness=saturate(authoredControls.y);
    }
    else
    {
        wetness=MeadowWetness(world.xz);dryness=MeadowSoilDryness(world.xz,wetness);
    }
    w=lerp(w,float4(1-dryness,0,dryness,0),path);
    if(mesh==20) w=float4(1,0,0,0);
    float total=0;
    HsMaterialTerrainResult m=(HsMaterialTerrainResult)0;
    [loop] for(uint layer=0;layer<4;++layer)
    {
        if(w[layer]<=0) continue;
        HsMaterialTerrainResult n=TerrainLayer(gaussian,surface,normal,detail,height,histogram,environment_wrap,linear_clamp,world,worldDx,worldDy,layer,seed,warp,viewDirection,distanceToEye,gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot,histogram_slot);
        float weight=w[layer]*exp2((n.height-0.5)*(2+3*path*(1-path)));
        m.base_color+=n.base_color*weight;m.normal+=n.normal*weight;m.detail+=n.detail*weight;
        m.roughness+=n.roughness*weight;m.ao+=n.ao*weight;total+=weight;
    }
    m.base_color/=total;m.roughness/=total;m.ao/=total;
    const float grassCoverage=mesh==20u?0.0:(1.0-path)*smoothstep(0.0,0.2,w.y);
    m.base_color=TerrainGrassPalette(m.base_color,grassCoverage);
    // Soil-only reflectance correction keeps sampled grain and foliage/UI exposure intact.
    m.base_color*=lerp(float3(.58,.43,.30),float3(1,1,1),grassCoverage);
    m.base_color*=lerp(float3(1,1,1),MeadowDryGrassGain(world.xz),grassCoverage);
    // Dense turf occludes sky fill and its pale reflection between the individual grass cards.
    m.ao*=lerp(1.0,0.1,grassCoverage);
    float macro=TerrainNoise(world.xz/18,seed+173);
    // Keep terrain reflectance below the washed-out daylight range without darkening UI or actors.
    m.base_color*=0.6*(1+macro*0.08+meso*0.04);m.roughness=saturate(m.roughness+macro*0.04);
    m.roughness=lerp(m.roughness,1.0,0.8*grassCoverage);
    m.normal=normalize(m.normal);m.detail=normalize(m.detail);
    TerrainGroundTraces(m,world,path,grassCoverage,mesh,seed,wetness,dryness);
    return m;
}

HsMaterialTerrainResult HsMaterialTerrainStochastic(
    Texture2DArray<float4> gaussian, Texture2DArray<float4> surface,
    Texture2DArray<float4> normal, Texture2DArray<float4> detail,
    Texture2DArray<float4> height, Texture2DArray<float4> histogram,
    SamplerState environment_wrap, SamplerState linear_clamp,
    float3 world,float3 worldDx,float3 worldDy,uint mesh,uint seed,
    float3 viewDirection,float distanceToEye,
    uint gaussian_slot,uint surface_slot,uint normal_slot,uint detail_slot,uint height_slot,uint histogram_slot,
    float2 authoredControls)
{
    return EnvTerrain(gaussian,surface,normal,detail,height,histogram,environment_wrap,linear_clamp,
                      world,worldDx,worldDy,mesh,seed,viewDirection,distanceToEye,
                      gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot,histogram_slot,authoredControls);
}
#endif
