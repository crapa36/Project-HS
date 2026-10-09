#include "ray_radiance.hlsli"
#if HS_WITH_NRD
#include "NRDConfig.hlsli"
#include "NRD.hlsli"
#endif

uint2 RtLevelActive(uint level)
{uint scale=1u<<level;return (uint2(RtRenderSize.xy)+scale-1u)/scale;}
uint RtDepthIndex(uint2 pixel,uint level)
{return RtHierarchyLevels[level].z+pixel.y*RtHierarchyLevels[level].x+pixel.x;}
#if !HS_RAY_SER
[numthreads(8,8,1)]
void HybridDepthInitCS(uint3 dispatch:SV_DispatchThreadID)
{
    if(any(dispatch.xy>=uint2(RtRenderSize.xy)))return;
    RtClosestDepthOutput[RtDepthIndex(dispatch.xy,0u)]=RtDepth.Load(int3(dispatch.xy,0));
}
[numthreads(8,8,1)]
void HybridDepthReduceCS(uint3 dispatch:SV_DispatchThreadID)
{
    uint level=PassValue;
    if(level==0u||level>=RtCounts.z||any(dispatch.xy>=RtLevelActive(level)))return;
    uint2 previous=RtLevelActive(level-1u);float closest=0.0;
    [unroll]for(uint y=0u;y<2u;++y)[unroll]for(uint x=0u;x<2u;++x)
    {
        uint2 p=dispatch.xy*2u+uint2(x,y);
        if(all(p<previous))closest=max(closest,RtClosestDepthOutput[RtDepthIndex(p,level-1u)]);
    }
    RtClosestDepthOutput[RtDepthIndex(dispatch.xy,level)]=closest;
}
#endif
float RtCurrentViewDepth(float depth)
{
    float denominator=depth*RtProjection[2][3]-RtProjection[2][2];
    return abs(denominator)>1e-8?(RtProjection[3][2]-depth*RtProjection[3][3])/denominator:-1.0;
}
float RtBoundExit(float start,float delta)
{
    if(delta>1e-8)return (1.0-start)/delta;
    if(delta< -1e-8)return -start/delta;
    return 1.0;
}
// Projected line/cell intersections preserve perspective depth and cannot jump
// over thin foreground surfaces as a constant world-space stride can.
bool RtScreenTrace(RtGuide source,float3 directionWorld,out int2 hitPixel,
    out float distance,out float confidence)
{
    hitPixel=0;distance=RtSpecularParameters.x;confidence=0.0;
    float3 start=mul(float4(source.relative+source.normal*RtSpecularParameters.y,0.0),RtWorldToView).xyz;
    float3 direction=normalize(mul(float4(directionWorld,0.0),RtWorldToView).xyz);
    float lengthLimit=RtSpecularParameters.x;
    float nearDepth=max(RtCurrentViewDepth(1.0),1e-4);
    if(direction.z<0.0)lengthLimit=min(lengthLimit,(start.z-nearDepth*1.001)/-direction.z);
    if(lengthLimit<=0.0)return false;
    float4 ac=mul(float4(start,1.0),RtProjection);
    float4 bc=mul(float4(start+direction*lengthLimit,1.0),RtProjection);
    if(ac.w<=0.0||bc.w<=0.0)return false;
    float3 a=ac.xyz/ac.w,b=bc.xyz/bc.w;
    a.xy=a.xy*float2(0.5,-0.5)+0.5;b.xy=b.xy*float2(0.5,-0.5)+0.5;
    if(any(a.xy<0.0)||any(a.xy>=1.0))return false;
    float3 delta=b-a;
    float exit=min(1.0,min(RtBoundExit(a.x,delta.x),RtBoundExit(a.y,delta.y)));
    exit=min(exit,RtBoundExit(a.z,delta.z));
    float pixelSpan=max(abs(delta.x)*RtRenderSize.x,abs(delta.y)*RtRenderSize.y);
    float epsilon=1e-3/max(pixelSpan,1.0),cursor=epsilon;
    uint maximumLevel=RtCounts.z-1u,level=0u;
    [loop]for(uint iteration=0u;iteration<RtSpecularBudget.y&&cursor<exit;++iteration)
    {
        float3 sample_position=a+delta*cursor;
        if(any(sample_position.xy<0.0)||any(sample_position.xy>=1.0))break;
        float scale=float(1u<<level);uint2 cell=uint2(floor(sample_position.xy*RtRenderSize.xy/scale));
        if(any(cell>=RtLevelActive(level)))break;
        float2 boundary=(float2(cell)+float2(delta.x>=0.0?1.0:0.0,delta.y>=0.0?1.0:0.0))*scale*RtRenderSize.zw;
        float nextX=abs(delta.x)>1e-8?(boundary.x-a.x)/delta.x:exit;
        float nextY=abs(delta.y)>1e-8?(boundary.y-a.y)/delta.y:exit;
        float next=max(min(exit,min(nextX,nextY)),cursor+epsilon);
        float nearest=RtClosestDepth[RtDepthIndex(cell,level)];
        float endDepth=a.z+delta.z*min(next,exit);
        bool inFront=nearest==0.0||min(sample_position.z,endDepth)>nearest;
        if(!inFront&&level>0u){--level;continue;}
        if(!inFront)
        {
            float location=abs(delta.z)>1e-8?clamp((nearest-a.z)/delta.z,cursor,min(next,exit)):cursor;
            float rayDepth=RtCurrentViewDepth(a.z+delta.z*location),sceneDepth=RtCurrentViewDepth(nearest);
            float gap=rayDepth-sceneDepth;int2 pixel=int2(cell);
            if(any(pixel!=source.pixel)&&gap>=-1e-4&&gap<=RtSpecularParameters.z)
            {
                float2 uv=(float2(pixel)+0.5)*RtRenderSize.zw;
                uint2 metadata=RtMetadata.Load(int3(pixel,0));
                float3 hitNormal=HsDecodeSurfaceNormal(RtNormal.Load(int3(pixel,0)));
                if(metadata.x!=0u&&dot(hitNormal,-directionWorld)>0.0)
                {
                    float edge=min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y));
                    confidence=saturate(edge/0.05)*(1.0-smoothstep(RtSpecularParameters.z*0.5,RtSpecularParameters.z,max(gap,0.0)));
                    hitPixel=pixel;distance=length(RtRelative(pixel,nearest)-source.relative);
                    return confidence>0.0;
                }
            }
        }
        cursor=next+epsilon;level=min(level+1u,maximumLevel);
    }
    return false;
}
float RtSpecularRoughness(HsSurfaceClosure closure)
{
    float r=closure.roughness;
    if(closure.coat_weight>0.0)r=min(r,closure.coat_roughness);
    if(closure.sheen_weight>0.0)r=min(r,closure.sheen_roughness);
    return saturate(r);
}
void RtTraceSpecularPixel(uint2 pixel)
{
    if(any(pixel>=uint2(RtSpecularSize.xy)))return;
    RtGuide g=RtSignalGuide(pixel,RtSpecularSize);
    float3 result=0.0;float distanceSum=0.0,secondMoment=0.0,validity=0.0;
    float rawMetricMinimum=65504.0;uint acceptedPaths=0u;
#if HS_WITH_NRD
    float nativeHitDistance=NRD_FrontEnd_SpecHitDistAveraging_Begin();
#endif
    uint samples=max(RtSpecularBudget.x,1u);
    if(g.valid)
    {
        HsSurfaceClosure closure=RtPrimaryClosure(g);float3 view=normalize(-g.relative);
        closure=HsBsdfOrientThinSurface(closure,view);g.normal=normalize(closure.normal);
        uint environment=g.identity.x-0x10000000u,hero=g.identity.x-HsHeroMaterialIdBase;
        // Only domains that exported environment specular have a replaceable
        // term. Above the authored roughness budget the existing probe remains.
        bool supported=!closure.unlit&&(environment<15u||HsHeroOpaqueOrdinalValid(hero));
        if(supported&&RtSpecularRoughness(closure)<RtSpecularParameters.w)
        {
            validity=1.0;
            [loop]for(uint i=0u;i<samples;++i)
            {
                float uLobe=RtRandom(pixel,i,0u);float2 uDirection=float2(RtRandom(pixel,i,1u),RtRandom(pixel,i,2u));
                HsBsdfSampleResult proposal;
                if((RtFrameInfo.w&8u)!=0u)proposal=HsBsdfNrdSampleComponent(closure,view,uLobe,uDirection,true);
                else proposal=HsBsdfSample(closure,view,uLobe,uDirection);
                float cosine=dot(closure.normal,proposal.wi);
                if(!proposal.valid||proposal.pdf<=0.0||cosine<=0.0)continue;
                float3 diffuse,specular;HsBsdfEvaluateComponents(closure,view,proposal.wi,diffuse,specular);
                if(!any(specular>0.0))continue;
                int2 hitPixel;float screenDistance,screenConfidence;
                bool screenHit=RtScreenTrace(g,proposal.wi,hitPixel,screenDistance,screenConfidence);
                float3 screenRadiance=0.0;
                if(screenHit)
                {
                    int3 h=int3(hitPixel,0);
                    screenRadiance=RtHdr.Load(h).rgb-RtEnvironmentSpecular.Load(h).rgb;
                    if((RtFrameInfo.w&1u)!=0u&&(RtFrameInfo.w&32u)==0u)
                        screenRadiance+=RtGiClean.Load(h).rgb-RtFallbackDiffuse.Load(h).rgb;
                    screenRadiance=max(screenRadiance,0.0);
                }
                float3 completedRadiance=screenRadiance;float distance=screenDistance;
                if(screenConfidence<1.0)
                {
                    float rayDistance,rayConfidence;bool secondaryHit;
                    float3 rayRadiance=RtTraceRadiance(g.relative+RtWorldOrigin.xyz,closure.normal,proposal.wi,
                        RtSpecularParameters.x,RtSpecularParameters.y,RtGiParameters.z,pixel,i,RtSpecularBudget.w,rayDistance,rayConfidence,secondaryHit);
                    // Complete this exact sampled direction before resolving.
                    // Screen confidence weights its incident-radiance estimate;
                    // it never multiplies unrelated filtered reflection RGB.
                    completedRadiance=lerp(rayRadiance,screenRadiance,screenConfidence);
                    if((RtFrameInfo.w&8u)!=0u&&!secondaryHit)rayDistance=65504.0;
                    if((RtFrameInfo.w&8u)!=0u)
                        distance=screenHit&&screenConfidence>0.0?min(rayDistance,screenDistance):rayDistance;
                    else distance=lerp(rayDistance,screenDistance,screenConfidence);
                    if(rayConfidence<=0.0)validity=0.0;
                }
                float3 contribution=completedRadiance*specular*(cosine*saturate(closure.ao)/proposal.pdf);
                if(!all(isfinite(contribution))){validity=0.0;continue;}
                result+=contribution;distanceSum+=distance;secondMoment+=pow(HsBsdfLuminance(contribution),2.0);
                if((RtFrameInfo.w&16u)!=0u)
                {
                    if(distance>0.0)rawMetricMinimum=min(rawMetricMinimum,distance);
                    ++acceptedPaths;
                }
#if HS_WITH_NRD
                // Null and opposite-sheet proposals contribute zero rays. The
                // official helper excludes zero distances from the minimum.
                if((RtFrameInfo.w&8u)!=0u)NRD_FrontEnd_SpecHitDistAveraging_Add(nativeHitDistance,distance);
#endif
            }
            result/=float(samples);distanceSum/=float(samples);secondMoment/=float(samples);
        }
    }
    // Null VNDF and opposite-sheet proposals retain the fixed denominator.
    // Alpha records estimator validity; SSR coverage is already completed above.
    float luminance=HsBsdfLuminance(result);
#if HS_WITH_NRD
    NRD_FrontEnd_SpecHitDistAveraging_End(nativeHitDistance);
    if((RtFrameInfo.w&8u)!=0u)distanceSum=nativeHitDistance;
#endif
    RtRawOutput[pixel]=float4(max(result,0.0),validity);
    RtRawStatsOutput[pixel]=float4(distanceSum,luminance,secondMoment,validity);
    if((RtFrameInfo.w&16u)!=0u)
        RtRawMetricCountOutput[pixel]=float2(acceptedPaths?rawMetricMinimum:0.0,float(acceptedPaths));
}
#if !HS_RAY_SER
[numthreads(8,8,1)]void HybridReflectionTraceCS(uint3 dispatch:SV_DispatchThreadID){RtTraceSpecularPixel(dispatch.xy);}
[numthreads(8,8,1)]void HybridReflectionTemporalCS(uint3 dispatch:SV_DispatchThreadID){RtFilterTemporal(dispatch.xy,true);}
[numthreads(8,8,1)]void HybridReflectionSpatialCS(uint3 dispatch:SV_DispatchThreadID){RtFilterSpatial(dispatch.xy,true);}
#endif
