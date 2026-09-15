// v9 environment materials. All colors are sampled through sRGB resource views.
Texture2DArray<float4> TerrainGaussian : register(t0, space2);
Texture2DArray<float4> TerrainSurface : register(t1, space2);
Texture2DArray<float4> TerrainNormal : register(t2, space2);
Texture2DArray<float4> TerrainDetail : register(t3, space2);
Texture2DArray<float> TerrainHeight : register(t4, space2);
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
Texture2DArray<float4> TerrainHistogram : register(t19, space2);
Texture2D<float4> GBufferMaterial : register(t20, space2);
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
float TerrainTileSize(uint layer) { return layer==3 ? 4.5 : 6.0; }
struct EnvMaterial { float3 color; float3 normal; float3 detail; float roughness; float ao; float height; };
EnvMaterial TerrainSample(float2 baseUv, float2 baseDx, float2 baseDy, uint layer, uint seed,float3 viewTs,float distanceToEye,bool pom)
{
    uint rotation=EnvHash(seed)&3; float2 offset=float2(EnvRandom(seed+1),EnvRandom(seed+2));
    float2 uv=EnvRotate(baseUv,rotation)+offset, dx=EnvRotate(baseDx,rotation),dy=EnvRotate(baseDy,rotation);
    if(pom && distanceToEye<55)
    {
        float tile=TerrainTileSize(layer); float heightScale=layer==3?0.05:layer==2?0.035:0.025;
        uint steps=(uint)lerp(8,24,1-saturate(abs(viewTs.z)));
        float2 ray=EnvRotate(viewTs.xy,rotation)/max(abs(viewTs.z),0.05)*heightScale/tile*(1-smoothstep(35,55,distanceToEye));
        float depth=0,previous=0; float2 start=uv;
        // Trace depth below the highest surface: black=low, white=high.
        [loop] for(uint j=0;j<steps;++j) { if(depth>=1-TerrainHeight.SampleGrad(EnvironmentWrap,float3(uv,layer),dx,dy)) break; previous=depth; depth+=1.0/steps; uv=start-ray*depth; }
        [unroll] for(uint k=0;k<3;++k) { float mid=(previous+depth)*0.5; float2 trial=start-ray*mid; if(mid>=1-TerrainHeight.SampleGrad(EnvironmentWrap,float3(trial,layer),dx,dy)) depth=mid; else previous=mid; }
        uv=start-ray*((previous+depth)*0.5);
    }
    EnvMaterial m=(EnvMaterial)0;
    m.color=TerrainGaussian.SampleGrad(EnvironmentWrap,float3(uv,layer),dx,dy).rgb;
    float2 ra=TerrainSurface.SampleGrad(EnvironmentWrap,float3(uv,layer),dx,dy).rg; m.roughness=ra.x;m.ao=ra.y;
    float3 n=EnvDecode(TerrainNormal.SampleGrad(EnvironmentWrap,float3(uv,layer),dx,dy).rg);
    float3 d=EnvDecode(TerrainDetail.SampleGrad(EnvironmentWrap,float3(uv,layer),dx,dy).rg);
    d=normalize(lerp(float3(0,0,1),d,1-smoothstep(35,55,distanceToEye)));
    n.xy=EnvRotate(n.xy,(4-rotation)&3);d.xy=EnvRotate(d.xy,(4-rotation)&3);m.normal=n;m.detail=d;
    m.height=TerrainHeight.SampleGrad(EnvironmentWrap,float3(uv,layer),dx,dy);return m;
}
float3 DecodeTerrainHistogram(float3 gaussian,uint layer,float lod)
{
    float row=(clamp(lod,0,11)+0.5)/16;
    float3 g=clamp(gaussian,0.5/256,1-0.5/256);
    float3 p=float3(
        TerrainHistogram.SampleLevel(LinearClamp,float3(g.x,row,layer),0).r,
        TerrainHistogram.SampleLevel(LinearClamp,float3(g.y,row,layer),0).g,
        TerrainHistogram.SampleLevel(LinearClamp,float3(g.z,row,layer),0).b);
    float3 origin=TerrainHistogram.Load(int4(0,12,layer,0)).rgb;
    float3 x=TerrainHistogram.Load(int4(0,13,layer,0)).rgb;
    float3 y=TerrainHistogram.Load(int4(0,14,layer,0)).rgb;
    float3 z=TerrainHistogram.Load(int4(0,15,layer,0)).rgb;
    return saturate(origin+x*p.x+y*p.y+z*p.z);
}
EnvMaterial TerrainLayer(float3 world,float2 worldDx,float2 worldDy,uint layer,uint seed,float distanceToEye)
{
    float tile=TerrainTileSize(layer);
    float2 uv=world.xz/tile,dx=worldDx/tile,dy=worldDy/tile;
    float3 ts=normalize(CameraTime.xyz-world).xzy;
    // Random transforms apply at gameplay distances too. Warp only the blend
    // lattice, not texture coordinates, to avoid stretching the reference maps.
    float2 warp=float2(EnvNoise(world.xz/7,seed+413),EnvNoise(world.xz/7,seed+827));
    float2 p=(world.xz+warp*1.3)/(tile*0.9),i=floor(p),f=frac(p);
    float2 a=i,b=i+float2(1,0),c=i+float2(0,1);
    float3 w=float3(1-f.x-f.y,f.x,f.y);
    if(f.x+f.y>=1) {a=i+1;b=i+float2(0,1);c=i+float2(1,0);w=float3(f.x+f.y-1,1-f.x,1-f.y);}
    // Zero derivative at cell edges avoids visible triangular blending lines.
    w=w*w*(3-2*w);w/=max(w.x+w.y+w.z,1e-5);
    EnvMaterial x=TerrainSample(uv,dx,dy,layer,EnvCell(a,seed),ts,distanceToEye,true);
    EnvMaterial y=TerrainSample(uv,dx,dy,layer,EnvCell(b,seed),ts,distanceToEye,true);
    EnvMaterial z=TerrainSample(uv,dx,dy,layer,EnvCell(c,seed),ts,distanceToEye,true);
    // Preserve Gaussian variance before the mip-aware inverse histogram transform.
    float3 gaussian=0.5+(x.color*w.x+y.color*w.y+z.color*w.z-0.5)*rsqrt(dot(w,w));
    // Isotropic derivative LOD follows the reference LUT prefilter approximation;
    // the Gaussian and the original PBR maps retain anisotropic texture filtering.
    float lod=0.5*log2(max(max(dot(dx,dx),dot(dy,dy))*2048*2048,1e-8));
    x.color=DecodeTerrainHistogram(gaussian,layer,lod);
    x.normal=normalize(x.normal*w.x+y.normal*w.y+z.normal*w.z);
    x.detail=normalize(x.detail*w.x+y.detail*w.y+z.detail*w.z);
    x.roughness=x.roughness*w.x+y.roughness*w.y+z.roughness*w.z;
    x.ao=x.ao*w.x+y.ao*w.y+z.ao*w.z;
    x.height=x.height*w.x+y.height*w.y+z.height*w.z;
    return x;
}
EnvMaterial EnvTerrain(float3 world,uint mesh,uint seed)
{
    float2 worldDx=ddx(world.xz),worldDy=ddy(world.xz);
    float4 w=max(float4(0.5-EnvNoise(world.xz/12,seed),0.7+EnvNoise(world.xz/16,seed+9),EnvNoise(world.xz/22,seed+31),EnvNoise(world.xz/9,seed+71)),0);
    w.x=max(w.x,0.05);
    float edgeNoise=EnvNoise(world.xz/1.7,seed+317)*0.35+EnvNoise(world.xz/5,seed+617)*0.4;
    float path=1-smoothstep(2.8,5.8,abs(world.x-sin(world.z*0.085)*6.5)+edgeNoise);
    w=lerp(w,float4(1,0,0,0),path);
    if(mesh==20) w=float4(1,0,0,0);
    float distanceToEye=length(CameraTime.xyz-world),total=0;
    EnvMaterial m=(EnvMaterial)0;
    // Every positive layer contributes continuously, including three-way junctions.
    // Dropping a ranked layer structurally creates a seam when ranks exchange.
    [loop] for(uint layer=0;layer<4;++layer)
    {
        if(w[layer]<=0) continue;
        EnvMaterial n=TerrainLayer(world,worldDx,worldDy,layer,seed,distanceToEye);
        float weight=w[layer]*exp2((n.height-0.5)*2);
        m.color+=n.color*weight;m.normal+=n.normal*weight;m.detail+=n.detail*weight;
        m.roughness+=n.roughness*weight;m.ao+=n.ao*weight;total+=weight;
    }
    m.color/=total;m.roughness/=total;m.ao/=total;
    m.normal=normalize(m.normal);m.detail=normalize(m.detail);
    float macro=EnvNoise(world.xz/18,seed+173),meso=EnvNoise(world.xz/3,seed+571);
    m.color*=1+macro*0.08+meso*0.04;m.roughness=saturate(m.roughness+macro*0.04);
    m.normal=EnvRNM(m.normal,m.detail).xzy;return m;
}
EnvMaterial EnvOpaque(float2 uv,uint layer,bool rock,float distanceToEye)
{
    EnvMaterial m=(EnvMaterial)0;
    float3 p=float3(uv,layer);
    m.color=rock?RockBase.Sample(EnvironmentWrap,p).rgb:BarkBase.Sample(EnvironmentWrap,p).rgb;
    float2 ra=rock?RockSurface.Sample(EnvironmentWrap,p).rg:BarkSurface.Sample(EnvironmentWrap,p).rg;
    float3 n=EnvDecode(rock?RockNormal.Sample(EnvironmentWrap,p).rg:BarkNormal.Sample(EnvironmentWrap,p).rg);
    float3 d=EnvDecode(rock?RockDetail.Sample(EnvironmentWrap,p).rg:BarkDetail.Sample(EnvironmentWrap,p).rg);
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
float3 EnvironmentLighting(float3 base,float3 n,float3 world,float4 material,float visibility)
{
    const float pi=3.14159265;float3 v=normalize(CameraTime.xyz-world),l=normalize(-LightDirectionIntensity.xyz),h=normalize(v+l);
    float nv=max(saturate(dot(n,v)),0.001),nl=saturate(dot(n,l)),nh=saturate(dot(n,h)),vh=saturate(dot(v,h));
    float alpha=max(material.r*material.r,0.002),a2=alpha*alpha;
    float D=a2/(pi*pow(nh*nh*(a2-1)+1,2));
    float Gv=2*nv/(nv+sqrt(a2+(1-a2)*nv*nv));float Gl=2*nl/max(nl+sqrt(a2+(1-a2)*nl*nl),0.0001);
    float3 F=0.04+0.96*pow(1-vh,5);
    float3 direct=((1-F)*base/pi+D*Gv*Gl*F/max(4*nv*nl,0.0001))*nl*LightColor.rgb*LightDirectionIntensity.w*visibility;
    // Analytic split-sum BRDF fit and a roughness-prefiltered hemispherical sky.
    // No HDR environment cubemap is shipped with this texture pack.
    float4 r=material.r*float4(-1,-0.0275,-0.572,0.022)+float4(1,0.0425,1.04,-0.04);
    float a004=min(r.x*r.x,exp2(-9.28*nv))*r.x+r.y;float2 AB=float2(-1.04,1.04)*a004+r.zw;
    float3 reflected=reflect(-v,n);float skyT=lerp(saturate(reflected.y*0.5+0.5),0.5,material.r*material.r);
    float3 sky=lerp(float3(0.055,0.065,0.045),float3(0.24,0.31,0.42),skyT);
    float3 irradiance=lerp(float3(0.07,0.08,0.055),float3(0.22,0.28,0.36),n.y*0.5+0.5);
    float3 indirect=(base*0.96*irradiance+sky*(0.04*AB.x+AB.y))*material.g;
    float transmission=material.a<0.68?0.3:material.a<0.9?0.45:0;
    direct+=base*transmission*pow(saturate(dot(-n,l)),2)*LightColor.rgb*LightDirectionIntensity.w*visibility;
    return direct+indirect;
}
