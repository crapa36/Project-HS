#ifndef HS_MATERIAL_TERRAIN_HLSLI
#define HS_MATERIAL_TERRAIN_HLSLI

#ifndef HS_TERRAIN_PAGE_BAKE
#include "texture_streaming.hlsli"
#include "terrain_page_abi.hlsli"
#endif
#include "meadow_surface.hlsli"
#include "terrain_histogram_abi.hlsli"

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
#ifdef HS_TERRAIN_PAGE_BAKE
    float residual_depth;
    float4 layer_weights;
#endif
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
float TerrainTileSize(uint layer) { return TerrainSourceMetersPerTile[layer]; }
float TerrainHeightScale(uint layer) { return TerrainSourceHeightScale[layer]; }
float2 TerrainDetailFadeRange()
{
    // Native frame Z is two IEEE binary16 distances, not an enum or arithmetic
    // float value. The package validates positivity/order after half rounding.
    return HS_MATERIAL_DETAIL_FADE_RANGE;
}

float3 TerrainParallaxFootprint(float2 dx, float2 dy, uint layer,
    float3 viewTs, float distanceToEye, uint steps)
{
    if (steps == 0u || distanceToEye >= 55.0) return 0;
    float tile=TerrainTileSize(layer); float heightScale=TerrainHeightScale(layer);
    // Preserve the component-wise divide/multiply order. Quarter turns only
    // permute/sign-flip components; ray length and the Jacobian norms agree.
    float2 ray=viewTs.xy/max(abs(viewTs.z),0.05)*heightScale/tile*(1-smoothstep(35,55,distanceToEye));
    float pomWeight=1;
    const float gradientEnergy=dot(dx,dx)+dot(dy,dy);
    const float gradientArea=abs(dx.x*dy.y-dx.y*dy.x);
    // The inverse-Jacobian norm bounds displacement even across anisotropic footprints.
    if(gradientEnergy>1e-20 && gradientArea>gradientEnergy*1e-6)
    {
        const float parallaxPixels=length(ray)*(sqrt(gradientEnergy)/gradientArea);
        if(isfinite(parallaxPixels)) pomWeight=smoothstep(0.25,0.5,parallaxPixels);
    }
    return float3(ray,pomWeight);
}

struct TerrainCellSample
{
    uint rotation;
    float2 start, uv, dx, dy, ray;
    float depth, previous;
    bool active;
};

struct TerrainCellBasis
{
    uint rotation;
    float2 start;
};

TerrainCellBasis TerrainPrepareCellBasis(float2 baseUv,uint seed)
{
    TerrainCellBasis basis;
    basis.rotation=TerrainHash(seed)&3;
    const float2 offset=float2(TerrainRandom(seed+1),TerrainRandom(seed+2));
    basis.start=TerrainRotate(baseUv,basis.rotation)+offset;
    return basis;
}

TerrainCellSample TerrainPrepareCell(TerrainCellBasis basis,float2 baseDx,float2 baseDy,float3 parallax)
{
    TerrainCellSample cell;
    cell.rotation=basis.rotation;
    cell.start=basis.start;
    cell.uv=cell.start;
    cell.dx=TerrainRotate(baseDx,cell.rotation);
    cell.dy=TerrainRotate(baseDy,cell.rotation);
    cell.ray=TerrainRotate(parallax.xy,cell.rotation);
    cell.depth=0;cell.previous=0;cell.active=true;
    return cell;
}

void TerrainStochasticCoordinates(float2 world,uint layer,float2 warp,
    out float2 a,out float2 b,out float2 c,out float3 weights)
{
    const float tile=TerrainTileSize(layer);
    const float2 p=(world+warp*1.3)/(tile*0.9);
    const float2 skew=float2(p.x-0.5773502691896258*p.y,1.1547005383792515*p.y);
    const float2 i=floor(skew),f=frac(skew);
    // Sharpened weights can reveal the underlying cell pattern.
    a=i;b=i+float2(1,0);c=i+float2(0,1);
    weights=float3(1-f.x-f.y,f.x,f.y);
    if(f.x+f.y>=1) {a=i+1;b=i+float2(0,1);c=i+float2(1,0);weights=float3(f.x+f.y-1,1-f.x,1-f.y);}
}

void TerrainStochasticBasis(float2 world,uint layer,uint seed,float2 warp,
    out TerrainCellBasis x,out TerrainCellBasis y,out TerrainCellBasis z,out float3 weights)
{
    const float tile=TerrainTileSize(layer);
    const float2 uv=world/tile;
    float2 a,b,c;
    TerrainStochasticCoordinates(world,layer,warp,a,b,c,weights);
    x=TerrainPrepareCellBasis(uv,TerrainCell(a,seed));
    y=TerrainPrepareCellBasis(uv,TerrainCell(b,seed));
    z=TerrainPrepareCellBasis(uv,TerrainCell(c,seed));
}

void TerrainParallaxCells(Texture2DArray<float4> height, SamplerState environment_wrap,
    uint layer, float pomWeight, uint height_slot, HsTextureGradState height_state,
    inout TerrainCellSample x,inout TerrainCellSample y,inout TerrainCellSample z)
{
    // Disabled/subpixel POM performs no height lookup.
    [branch] if(pomWeight<=0) return;
    [loop] for(uint j=0;j<HS_MATERIAL_POM_STEPS;++j)
    {
        float hx=0,hy=0,hz=0;
        [branch] if(x.active) hx=HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(x.uv,layer),x.dx,x.dy,height_state).r;
        [branch] if(y.active) hy=HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(y.uv,layer),y.dx,y.dy,height_state).r;
        [branch] if(z.active) hz=HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(z.uv,layer),z.dx,z.dy,height_state).r;
        if(x.active) { if(x.depth>=1-hx) x.active=false; else { x.previous=x.depth;x.depth+=1.0/HS_MATERIAL_POM_STEPS;x.uv=x.start-x.ray*x.depth; } }
        if(y.active) { if(y.depth>=1-hy) y.active=false; else { y.previous=y.depth;y.depth+=1.0/HS_MATERIAL_POM_STEPS;y.uv=y.start-y.ray*y.depth; } }
        if(z.active) { if(z.depth>=1-hz) z.active=false; else { z.previous=z.depth;z.depth+=1.0/HS_MATERIAL_POM_STEPS;z.uv=z.start-z.ray*z.depth; } }
        if(!x.active && !y.active && !z.active) break;
    }
    [loop] for(uint k=0;k<HS_MATERIAL_POM_REFINEMENT_STEPS;++k)
    {
        float mx=(x.previous+x.depth)*0.5,my=(y.previous+y.depth)*0.5,mz=(z.previous+z.depth)*0.5;
        float2 tx=x.start-x.ray*mx,ty=y.start-y.ray*my,tz=z.start-z.ray*mz;
        float hx=HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(tx,layer),x.dx,x.dy,height_state).r;
        float hy=HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(ty,layer),y.dx,y.dy,height_state).r;
        float hz=HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(tz,layer),z.dx,z.dy,height_state).r;
        if(mx>=1-hx) x.depth=mx;else x.previous=mx;
        if(my>=1-hy) y.depth=my;else y.previous=my;
        if(mz>=1-hz) z.depth=mz;else z.previous=mz;
    }
    x.uv=x.start-x.ray*((x.previous+x.depth)*0.5*pomWeight);
    y.uv=y.start-y.ray*((y.previous+y.depth)*0.5*pomWeight);
    z.uv=z.start-z.ray*((z.previous+z.depth)*0.5*pomWeight);
}

HsMaterialTerrainResult TerrainSample(
    Texture2DArray<float4> gaussian, Texture2DArray<float4> surface,
    Texture2DArray<float4> normal, Texture2DArray<float4> detail,
    Texture2DArray<float4> height, SamplerState environment_wrap,
    TerrainCellSample cell, uint layer, bool detail_enabled, float detail_weight,
    uint gaussian_slot, uint surface_slot, uint normal_slot, uint detail_slot, uint height_slot,
    HsTextureGradState gaussian_state, HsTextureGradState surface_state,
    HsTextureGradState normal_state, HsTextureGradState detail_state,
    HsTextureGradState height_state)
{
    uint rotation=cell.rotation;float2 uv=cell.uv,dx=cell.dx,dy=cell.dy;
    HsMaterialTerrainResult m=(HsMaterialTerrainResult)0;
    m.base_color=HsTextureSampleGradPrepared(gaussian,gaussian_slot,environment_wrap,float3(uv,layer),dx,dy,gaussian_state).rgb;
    float2 ra=HsTextureSampleGradPrepared(surface,surface_slot,environment_wrap,float3(uv,layer),dx,dy,surface_state).rg; m.roughness=ra.x;m.ao=ra.y;
    float3 n=TerrainDecode(HsTextureSampleGradPrepared(normal,normal_slot,environment_wrap,float3(uv,layer),dx,dy,normal_state).rg);
    float3 d=float3(0,0,1);
    if(detail_enabled)
    {
        d=TerrainDecode(HsTextureSampleGradPrepared(detail,detail_slot,environment_wrap,float3(uv,layer),dx,dy,detail_state).rg);
        d=normalize(lerp(float3(0,0,1),d,detail_weight));
    }
    n.xy=TerrainRotate(n.xy,(4-rotation)&3);d.xy=TerrainRotate(d.xy,(4-rotation)&3);m.normal=n;m.detail=d;
    m.height=HsTextureSampleGradPrepared(height,height_slot,environment_wrap,float3(uv,layer),dx,dy,height_state).r;return m;
}

float3 TerrainGrassPalette(float3 color)
{
    // Linear mean of both foliage_grass_atlas_[0,1]_basecolor.png atlases, alpha >= 0.5.
    const float3 foliageAlbedo=float3(0.099848532,0.159184398,0.020007386);
    // Per-channel gain keeps soil/leaf color differences instead of replacing them with one hue.
    const float3 groundAlbedo=float3(0.165660347,0.151494940,0.047493307);
    // Match the lit canopy rather than the brighter upward-facing ground under daylight.
    const float3 canopyGain=float3(0.5,0.72,0.08);
    return color*(foliageAlbedo/groundAlbedo*canopyGain);
}

float3 DecodeTerrainHistogram(Texture2DArray<float4> histogram, SamplerState linear_clamp,
                              float3 gaussian,uint layer,float lod)
{
    const float mip=clamp(lod,0,HS_TERRAIN_HISTOGRAM_MIPS-1);
    const float3 rows=(mip+float3(.5,HS_TERRAIN_HISTOGRAM_MIPS+.5,2*HS_TERRAIN_HISTOGRAM_MIPS+.5))/HS_TERRAIN_HISTOGRAM_HEIGHT;
    const float3 g=clamp(gaussian,.5/HS_TERRAIN_HISTOGRAM_WIDTH,1-.5/HS_TERRAIN_HISTOGRAM_WIDTH);
    return saturate(
        histogram.SampleLevel(linear_clamp,float3(g.x,rows.x,layer),0).rgb+
        histogram.SampleLevel(linear_clamp,float3(g.y,rows.y,layer),0).rgb+
        histogram.SampleLevel(linear_clamp,float3(g.z,rows.z,layer),0).rgb);
}

HsMaterialTerrainResult TerrainLayer(
    Texture2DArray<float4> gaussian, Texture2DArray<float4> surface,
    Texture2DArray<float4> normal, Texture2DArray<float4> detail,
    Texture2DArray<float4> height, Texture2DArray<float4> histogram,
    SamplerState environment_wrap, SamplerState linear_clamp,
    float2 dx,float2 dy,uint layer,TerrainCellBasis basisX,TerrainCellBasis basisY,TerrainCellBasis basisZ,
    float3 w,float lod,
    float3 viewTs,float distanceToEye,bool detail_enabled,float detail_weight,
    uint gaussian_slot,uint surface_slot,uint normal_slot,uint detail_slot,uint height_slot,uint histogram_slot,
    HsTextureGradState gaussian_state,HsTextureGradState surface_state,
    HsTextureGradState normal_state,HsTextureGradState detail_state,
    HsTextureGradState height_state)
{
#ifdef HS_TERRAIN_PAGE_BAKE
    const float3 parallax=0;
#else
    const float3 parallax=TerrainParallaxFootprint(dx,dy,layer,viewTs,distanceToEye,HS_MATERIAL_POM_STEPS);
#endif

    // Only the compact mapping basis survives the layer loop; POM state is layer-local.
    TerrainCellSample cellX=TerrainPrepareCell(basisX,dx,dy,parallax);
    TerrainCellSample cellY=TerrainPrepareCell(basisY,dx,dy,parallax);
    TerrainCellSample cellZ=TerrainPrepareCell(basisZ,dx,dy,parallax);
    TerrainParallaxCells(height,environment_wrap,layer,parallax.z,height_slot,height_state,cellX,cellY,cellZ);
    HsMaterialTerrainResult x=TerrainSample(gaussian,surface,normal,detail,height,environment_wrap,cellX,layer,detail_enabled,detail_weight,gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot,gaussian_state,surface_state,normal_state,detail_state,height_state);
    HsMaterialTerrainResult y=TerrainSample(gaussian,surface,normal,detail,height,environment_wrap,cellY,layer,detail_enabled,detail_weight,gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot,gaussian_state,surface_state,normal_state,detail_state,height_state);
    HsMaterialTerrainResult z=TerrainSample(gaussian,surface,normal,detail,height,environment_wrap,cellZ,layer,detail_enabled,detail_weight,gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot,gaussian_state,surface_state,normal_state,detail_state,height_state);
    // The final row holds decoded Gaussian means, distinct from RGB contributions.
    float3 mean=histogram.SampleLevel(linear_clamp,float3(
        (clamp(lod,0,HS_TERRAIN_HISTOGRAM_MIPS-1)+1.5)/HS_TERRAIN_HISTOGRAM_WIDTH,
        (HS_TERRAIN_HISTOGRAM_MEAN_ROW+.5)/HS_TERRAIN_HISTOGRAM_HEIGHT,layer),0).rgb;
    float3 gaussian_value=mean+(x.base_color*w.x+y.base_color*w.y+z.base_color*w.z-mean)*rsqrt(dot(w,w));
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

// Fixed metre-scale environmental marks for terrain materials.
// Albedo gains preserve the sampled 2K detail; these are shallow material depressions, not water.
void TerrainGroundTraces(inout HsMaterialTerrainResult m,float3 world,float2 meadowLocal,float meadow,float path,float grassCoverage,uint mesh,uint seed,float wetness,float dryness)
{
    if(meadow>0)
    {
        const float wet=wetness*meadow;
        const float rut=MeadowCartWear(meadowLocal)*meadow;
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
    float2 authoredControls,float2 meadowLocal)
{
    if(mesh==22u) seed=MeadowSurfaceSeed;
    const float2 warp=float2(TerrainNoise(world.xz/7,seed+413),TerrainNoise(world.xz/7,seed+827));
    const float meso=TerrainNoise(world.xz/.85,seed+571);
    const float meadow=mesh==22u ? TerrainMeadowBlend(meadowLocal) : 0;
    const float edge=warp.x*.12+warp.y*.07+meso*.09;
    float path=0,wetness=0,dryness=0;
    float3 grassGain=1;
    // Authored marks follow their Meadow; the boundary rejoins generic terrain.
    [branch] if(meadow>0)
    {
        wetness=saturate(authoredControls.x);
        const float localRoad=MeadowRoadCoverage(meadowLocal,edge);
        float road=localRoad;
        dryness=MeadowSoilDryness(meadowLocal,wetness);
        [branch] if(meadow<1)
        {
            road=lerp(MeadowRoadCoverage(world.xz,edge),localRoad,meadow);
            dryness=lerp(MeadowSoilDryness(world.xz,wetness),dryness,meadow);
        }
        path=max(MeadowPressed(meadowLocal)*meadow,road*.82);
        grassGain=lerp(1,MeadowDryGrassGain(meadowLocal),meadow);
    }
    else
    {
        path=MeadowRoadCoverage(world.xz,edge)*.82;
        if(mesh!=20u) dryness=MeadowSoilDryness(world.xz,0);
    }
    float4 w=float4(1,0,0,0);
    [branch] if(mesh!=20u)
    {
        const float patch=TerrainNoise((world.xz+warp*2)/16,seed+9);
        const float bare=smoothstep(.35,.70,patch+meso*.10);
        const float soilDry=saturate(.5+.5*TerrainNoise(world.xz/22,seed+31));
        const float stones=.25*smoothstep(.55,.85,TerrainNoise(world.xz/9,seed+71));
        w=float4(bare*(1-stones)*(1-soilDry),1-bare,bare*(1-stones)*soilDry,bare*stones);
        w=lerp(w,float4(1-dryness,0,dryness,0),path);
    }
    float total=0,grassWeight=0;
    const float macro=TerrainNoise(world.xz/18,seed+173);
    HsMaterialTerrainResult m=(HsMaterialTerrainResult)0;
    const float3 viewTs=normalize(viewDirection).xzy;
    const HsTextureGradState gaussian_state=HsTexturePrepareGrad(gaussian_slot);
    const HsTextureGradState surface_state=HsTexturePrepareGrad(surface_slot);
    const HsTextureGradState normal_state=HsTexturePrepareGrad(normal_slot);
    const HsTextureGradState height_state=HsTexturePrepareGrad(height_slot);
    const float2 detail_fade=TerrainDetailFadeRange();
#ifdef HS_TERRAIN_PAGE_BAKE
    const bool detail_enabled=false;
#else
    const bool detail_enabled=distanceToEye<detail_fade.y;
#endif
    const float detail_weight=1-smoothstep(detail_fade.x,detail_fade.y,distanceToEye);
    HsTextureGradState detail_state=(HsTextureGradState)0;
    if(detail_enabled) detail_state=HsTexturePrepareGrad(detail_slot);
    TerrainCellBasis basisX=(TerrainCellBasis)0,basisY=(TerrainCellBasis)0,basisZ=(TerrainCellBasis)0;
    float3 cell_weights=0;
    float2 layer_dx=0,layer_dy=0;
    float prepared_tile=0,layer_lod=0;
    [loop] for(uint layer=0;layer<4;++layer)
    {
        if(w[layer]<=0) continue;
        const float tile=TerrainTileSize(layer);
        if(tile!=prepared_tile)
        {
            TerrainStochasticBasis(world.xz,layer,seed,warp,basisX,basisY,basisZ,cell_weights);
            layer_dx=worldDx.xz/tile;layer_dy=worldDy.xz/tile;
            layer_lod=0.5*log2(max(max(dot(layer_dx,layer_dx),dot(layer_dy,layer_dy))*
                float(TerrainSourceTextureEdge)*float(TerrainSourceTextureEdge),1e-8));
#ifndef HS_TERRAIN_PAGE_BAKE
            // Histogram reconstruction needs logical MinMip, not the physical SampleGrad clamp.
            layer_lod=max(layer_lod,float(HsTextureMipMetadata[gaussian_slot].y));
#endif
            prepared_tile=tile;
        }
        HsMaterialTerrainResult n=TerrainLayer(gaussian,surface,normal,detail,height,histogram,environment_wrap,linear_clamp,layer_dx,layer_dy,layer,basisX,basisY,basisZ,cell_weights,layer_lod,viewTs,distanceToEye,detail_enabled,detail_weight,gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot,histogram_slot,gaussian_state,surface_state,normal_state,detail_state,height_state);
        float weight=w[layer]*exp2((n.height-0.5)*(2+3*path*(1-path)));
        n.roughness=saturate(n.roughness+macro*0.04);
        if(layer==1u)
        {
            n.base_color=TerrainGrassPalette(n.base_color)*grassGain;
            n.ao*=0.1;
            n.roughness=lerp(n.roughness,1.0,0.8);
            grassWeight=weight;
        }
        else n.base_color*=float3(.58,.43,.30);
        m.base_color+=n.base_color*weight;m.normal+=n.normal*weight;m.detail+=n.detail*weight;
        m.roughness+=n.roughness*weight;m.ao+=n.ao*weight;total+=weight;
#ifdef HS_TERRAIN_PAGE_BAKE
        m.residual_depth+=(1-n.height)*TerrainHeightScale(layer)*weight;
        m.layer_weights[layer]=weight;
#endif
    }
    m.base_color/=total;m.roughness/=total;m.ao/=total;
#ifdef HS_TERRAIN_PAGE_BAKE
    m.residual_depth/=total;
    m.layer_weights/=total;
#endif
    const float grassCoverage=grassWeight/total;
    // Keep terrain reflectance below the washed-out daylight range without darkening UI or actors.
    m.base_color*=0.6*(1+macro*0.08+meso*0.04);
    m.normal=normalize(m.normal);m.detail=normalize(m.detail);
    TerrainGroundTraces(m,world,meadowLocal,meadow,path,grassCoverage,mesh,seed,wetness,dryness);
    return m;
}

struct HsTerrainSection
{
    float2 center, span;
    float2 rotation;
    uint mesh, seed;
};

#ifndef HS_TERRAIN_PAGE_BAKE
#ifndef HS_TERRAIN_SECTION_DECLARED
#define HS_TERRAIN_SECTION_DECLARED
struct HsTerrainPageSection
{
#define HS_TERRAIN_F2(name) float2 name;
#define HS_TERRAIN_U1(name) uint name;
#define HS_TERRAIN_U2(name) uint2 name;
    HS_TERRAIN_GPU_SECTION_FIELDS(HS_TERRAIN_F2, HS_TERRAIN_U1, HS_TERRAIN_U2)
#undef HS_TERRAIN_F2
#undef HS_TERRAIN_U1
#undef HS_TERRAIN_U2
};
StructuredBuffer<HsTerrainPageSection> HsTerrainSections : register(HS_TERRAIN_JOIN(t,HS_TERRAIN_SECTION_SRV),HS_TERRAIN_JOIN(space,HS_TERRAIN_PAGE_SPACE));
#endif
struct HsTerrainPlaneMip
{
#define HS_TERRAIN_U1(name) uint name;
    HS_TERRAIN_GPU_PLANE_MIP_FIELDS(HS_TERRAIN_U1)
#undef HS_TERRAIN_U1
};
StructuredBuffer<HsTerrainPlaneMip> HsTerrainPlaneMips : register(HS_TERRAIN_JOIN(t,HS_TERRAIN_MIP_SRV),HS_TERRAIN_JOIN(space,HS_TERRAIN_PAGE_SPACE));
Texture2D<float4> HsTerrainPlanes[HS_TERRAIN_PAGE_PLANE_CAPACITY] : register(HS_TERRAIN_JOIN(t,HS_TERRAIN_PLANE_SRV),HS_TERRAIN_JOIN(space,HS_TERRAIN_PAGE_SPACE));

float4 TerrainPageSample(SamplerState page_sampler,uint plane,float2 uv,float2 gx,float2 gy,float clamp_mip)
{
    return HsTerrainPlanes[NonUniformResourceIndex(plane)].SampleGrad(
        page_sampler,uv,gx,gy,int2(0,0),clamp_mip);
}

float2 TerrainSectionLocal(float2 v,float2 rotation)
{
    return float2(dot(v,float2(rotation.x,-rotation.y)),dot(v,rotation.yx));
}

float2 TerrainPageParallax(SamplerState page_sampler,float2 uv,float2 gx,float2 gy,
    float2 ray,float weight,uint plane,float clamp_mip)
{
    const uint steps=HS_MATERIAL_POM_STEPS;
    [branch] if(steps>0 && weight>0 && any(ray!=0))
    {
        float depth=0,previous=0;
        [loop] for(uint step=0;step<=steps;++step)
        {
            // This is a filtered height sample, not a conservative skip bound.
            const float surface=TerrainPageSample(page_sampler,plane,uv-ray*depth,gx,gy,clamp_mip).a;
            if(depth>=surface) break;
            previous=depth;
            depth=min(1.0,depth+1.0/steps);
        }
        [loop] for(uint refinement=0;refinement<HS_MATERIAL_POM_REFINEMENT_STEPS;++refinement)
        {
            const float middle=(previous+depth)*.5;
            const float surface=TerrainPageSample(page_sampler,plane,uv-ray*middle,gx,gy,clamp_mip).a;
            if(middle>=surface) depth=middle; else previous=middle;
        }
        uv-=ray*((previous+depth)*.5*weight);
    }
    return uv;
}

float3 TerrainPageDetailCell(Texture2DArray<float4> detail,SamplerState samp,
    uint detail_slot,HsTextureGradState detail_state,uint layer,TerrainCellSample cell,float weight)
{
    float3 n=TerrainDecode(HsTextureSampleGradPrepared(detail,detail_slot,samp,float3(cell.uv,layer),cell.dx,cell.dy,detail_state).rg);
    n=normalize(lerp(float3(0,0,1),n,weight));
    n.xy=TerrainRotate(n.xy,(4-cell.rotation)&3);
    return n;
}

float3 TerrainPageDetail(Texture2DArray<float4> detail,SamplerState environment_wrap,
    uint detail_slot,uint seed,float2 world,float2 dx,float2 dy,float4 weights,float weight)
{
    if(weight<=0) return float3(0,0,1);
    const HsTextureGradState state=HsTexturePrepareGrad(detail_slot);
    const float2 warp=float2(TerrainNoise(world/7,seed+413),TerrainNoise(world/7,seed+827));
    float3 combined=0;
    TerrainCellBasis basisX=(TerrainCellBasis)0,basisY=(TerrainCellBasis)0,basisZ=(TerrainCellBasis)0;
    float2 layer_dx=0,layer_dy=0;
    float3 cell_weights=0;
    float prepared_tile=0;
    [unroll] for(uint layer=0;layer<4;++layer)
    {
        if(weights[layer]<=0) continue;
        const float tile=TerrainTileSize(layer);
        if(tile!=prepared_tile)
        {
            TerrainStochasticBasis(world,layer,seed,warp,basisX,basisY,basisZ,cell_weights);
            layer_dx=dx/tile;layer_dy=dy/tile;
            prepared_tile=tile;
        }
        const TerrainCellSample x=TerrainPrepareCell(basisX,layer_dx,layer_dy,0);
        const TerrainCellSample y=TerrainPrepareCell(basisY,layer_dx,layer_dy,0);
        const TerrainCellSample z=TerrainPrepareCell(basisZ,layer_dx,layer_dy,0);
        const float3 nx=TerrainPageDetailCell(detail,environment_wrap,detail_slot,state,layer,x,weight);
        const float3 ny=TerrainPageDetailCell(detail,environment_wrap,detail_slot,state,layer,y,weight);
        const float3 nz=TerrainPageDetailCell(detail,environment_wrap,detail_slot,state,layer,z,weight);
        combined+=normalize(nx*cell_weights.x+ny*cell_weights.y+nz*cell_weights.z)*weights[layer];
    }
    combined.z=max(combined.z,1e-6);
    return normalize(combined);
}

HsMaterialTerrainResult HsMaterialTerrainPage(
    Texture2DArray<float4> gaussian,Texture2DArray<float4> surface,
    Texture2DArray<float4> normal,Texture2DArray<float4> detail,
    Texture2DArray<float4> height,Texture2DArray<float4> histogram,
    SamplerState environment_wrap,SamplerState linear_clamp,
    float3 world,float3 dx,float3 dy,uint mesh,uint section_index,float3 view,float distance_to_eye,
    uint gaussian_slot,uint surface_slot,uint normal_slot,uint detail_slot,uint height_slot,uint histogram_slot,
    float2 authored_controls)
{
    // Every terrain draw owns an admitted record; source-only records have no page.
    const HsTerrainPageSection section=HsTerrainSections[section_index];
    const bool has_page=section.pageSlice!=0xffffffffu;
    const float2 meadowLocal=mesh==22u && all(section.span!=0)
        ? TerrainSectionLocal(world.xz-section.center,section.rotation)*24.0/section.span : world.xz;
    const float2 fade=TerrainDetailFadeRange();
    const float detail_weight=1-smoothstep(fade.x,fade.y,distance_to_eye);
    float2 uv=0,gx=0,gy=0,ray=0;
    float pom_weight=0;
    float3 clamp_mips=0;
    float2 tile_span=section.span;
    uint slice=section.pageSlice;
    bool supported=false;
    [branch] if(has_page)
    {
        const float2 extent=float(section.pageEdge);
        const float2 apron=extent/16.0,interior=extent-2*apron;
        const float2 section_uv=TerrainSectionLocal(world.xz-section.center,section.rotation)/section.span+.5;
        const float2 counts=float2(section.tileCount);
        const uint2 tile=(uint2)clamp(floor(section_uv*counts),0,counts-1);
        const float2 local=section_uv*counts-float2(tile);
        tile_span=section.span/counts;
        slice+=(tile.y*section.tileCount.x+tile.x)*3u;
        const HsTerrainPlaneMip color_mip=HsTerrainPlaneMips[slice];
        const HsTerrainPlaneMip base_mip=HsTerrainPlaneMips[slice+1u];
        const HsTerrainPlaneMip weight_mip=HsTerrainPlaneMips[slice+2u];
        const uint resident=max(max(color_mip.residentMinMip,base_mip.residentMinMip),detail_weight>0 ? weight_mip.residentMinMip : 0u);
        clamp_mips=float(resident)-float3(color_mip.firstMip,base_mip.firstMip,weight_mip.firstMip);
        uv=(apron+local*interior)/extent;
        gx=TerrainSectionLocal(dx.xz,section.rotation)/tile_span*interior/extent;
        gy=TerrainSectionLocal(dy.xz,section.rotation)/tile_span*interior/extent;
        const float2 tx=gx*extent,ty=gy*extent;
        const float xx=dot(tx,tx),xy=dot(tx,ty),yy=dot(ty,ty);
        const float major=sqrt(.5*(xx+yy+length(float2(xx-yy,2*xy))));
        const float minor=abs(tx.x*ty.y-tx.y*ty.x)/max(major,1e-12);
        const float2 edge=min(uv,1-uv)*extent;
        const float parallax_tile=TerrainTileSize(3);
        const float3 parallax=TerrainParallaxFootprint(dx.xz/parallax_tile,dy.xz/parallax_tile,3,
            normalize(view).xzy,distance_to_eye,HS_MATERIAL_POM_STEPS);
        ray=TerrainSectionLocal(parallax.xy*parallax_tile,section.rotation)/tile_span*interior/extent;
        pom_weight=parallax.z;
        // Include the residual ray and trilinear/anisotropic prefilter support.
        const float2 support=3*max(major,exp2(float(resident)))+abs(ray)*extent;
        // Section indices name the admitted draw; apron contains its own
        // material continuation beyond the receiver rectangle.
        // Texture quality selects stored mips without changing the source route.
        const float wanted_mip=max(float(color_mip.qualityMipPlusOne)-1.0,floor(log2(max(minor*.5,1.0))));
        supported=color_mip.qualityMipPlusOne!=0 && base_mip.qualityMipPlusOne!=0 && (detail_weight<=0 || weight_mip.qualityMipPlusOne!=0) &&
            float(resident)<=wanted_mip && minor>=2.0*(extent.x/4096.0) && all(edge>=support);
    }
    [branch] if(!supported)
        return EnvTerrain(gaussian,surface,normal,detail,height,histogram,environment_wrap,linear_clamp,
            world,dx,dy,mesh,section.seed,view,distance_to_eye,
            gaussian_slot,surface_slot,normal_slot,detail_slot,height_slot,histogram_slot,authored_controls,meadowLocal);
    const float2 start_uv=uv;
    uv=TerrainPageParallax(environment_wrap,uv,gx,gy,ray,pom_weight,slice+1u,clamp_mips.y);
    const float4 color=TerrainPageSample(environment_wrap,slice,uv,gx,gy,clamp_mips.x);
    const float4 base=TerrainPageSample(environment_wrap,slice+1u,uv,gx,gy,clamp_mips.y);
    HsMaterialTerrainResult page=(HsMaterialTerrainResult)0;
    page.base_color=color.rgb;
    page.roughness=color.a;
    page.ao=base.b;
    page.normal=normalize(TerrainDecode(base.rg));
    page.detail=float3(0,0,1);
    [branch] if(detail_weight>0)
    {
        float4 weights=max(TerrainPageSample(environment_wrap,slice+2u,uv,gx,gy,clamp_mips.z),0);
        weights/=max(dot(weights,float4(1,1,1,1)),1e-5);
        const float2 local_offset=(uv-start_uv)*tile_span*(16.0/14.0);
        const float2 world_offset=local_offset.x*float2(section.rotation.x,-section.rotation.y)+local_offset.y*section.rotation.yx;
        page.detail=TerrainPageDetail(detail,environment_wrap,detail_slot,mesh==22u?MeadowSurfaceSeed:section.seed,world.xz+world_offset,
            dx.xz,dy.xz,weights,detail_weight);
    }
    return page;
}
#endif
#endif
