#include "ray_radiance.hlsli"
#if HS_WITH_NRD
#include "NRDConfig.hlsli"
#include "NRD.hlsli"
#endif
void RtTraceGiPixel(uint2 pixel)
{
    if(any(pixel>=uint2(RtGiSize.xy)))return;
    RtGuide g=RtSignalGuide(pixel,RtGiSize);float3 result=0.0;
    float distanceSum=0.0,secondMoment=0.0,confidence=1.0;
    float rawMetricSum=0.0;uint acceptedPaths=0u;
    uint samples=max(RtGiBudget.x,1u);
#if HS_WITH_NRD
    bool reblur=(RtFrameInfo.w&4u)!=0u&&RtNrdParameters.w==1.0;
    uint diffusePathCount=0u;
#endif
    if(g.valid)
    {
        HsSurfaceClosure closure=RtPrimaryClosure(g);float3 view=normalize(-g.relative);
        closure=HsBsdfOrientThinSurface(closure,view);
        if(closure.unlit)confidence=0.0;
        [loop]for(uint i=0u;i<samples;++i)
        {
            float uLobe=RtRandom(pixel,i,0u);float2 uDirection=float2(RtRandom(pixel,i,1u),RtRandom(pixel,i,2u));
            HsBsdfSampleResult proposal;
            if((RtFrameInfo.w&4u)!=0u)proposal=HsBsdfNrdSampleComponent(closure,view,uLobe,uDirection,false);
            else proposal=HsBsdfSample(closure,view,uLobe,uDirection);
            if(!proposal.valid||proposal.pdf<=0.0)continue;
            float3 diffuse,specular;HsBsdfEvaluateComponents(closure,view,proposal.wi,diffuse,specular);
            if(!any(diffuse>0.0))continue;
            float distance,pathConfidence;bool secondaryHit;
            float3 incoming=RtTraceRadiance(g.relative+RtWorldOrigin.xyz,closure.normal,proposal.wi,
                RtGiParameters.x,RtGiParameters.y,RtGiParameters.z,pixel,i,RtGiBudget.w,distance,pathConfidence,secondaryHit);
            float3 contribution=incoming*diffuse*(abs(dot(closure.normal,proposal.wi))*saturate(closure.ao)/proposal.pdf);
            if(!all(isfinite(contribution))){confidence=0.0;continue;}
            result+=contribution;
            if((RtFrameInfo.w&16u)!=0u)
            {
                rawMetricSum+=secondaryHit?distance:65504.0;
                ++acceptedPaths;
            }
            float pathHitDistance=(RtFrameInfo.w&4u)!=0u&&!secondaryHit?65504.0:distance;
#if HS_WITH_NRD
            if(reblur)
            {
                // Normalize each actual diffuse path before averaging: a miss
                // contributes one, rather than saturating other nearby hits.
                pathHitDistance=REBLUR_FrontEnd_GetNormHitDist(pathHitDistance,g.viewDepth,RtNrdParameters.xyz,1.0);
                ++diffusePathCount;
            }
#endif
            distanceSum+=pathHitDistance;
            secondMoment+=pow(HsBsdfLuminance(contribution),2.0);
            confidence=min(confidence,pathConfidence);
        }
        result/=float(samples);secondMoment/=float(samples);
#if HS_WITH_NRD
        // Radiance retains the fixed proposal denominator. Hit distance is an
        // average over accepted diffuse paths, with zero meaning no path data.
        if(reblur)distanceSum=diffusePathCount?distanceSum/float(diffusePathCount):0.0;
        else
#endif
            distanceSum/=float(samples);
    }
    else confidence=0.0;
    // Null and opposite-sheet proposals retain the fixed denominator. GI owns
    // only the diffuse component, including authored thin diffuse transmission.
    float luminance=HsBsdfLuminance(result);
    RtRawOutput[pixel]=float4(max(result,0.0),confidence);
    if((RtFrameInfo.w&32u)!=0u)
    {
        // Fused RR keeps this actual unfiltered diffuse estimator. Full-size
        // trace and raster guides share the same footprint; no clean lobe or
        // second material factor enters the existing SSS correction.
        float3 fallback=RtFallbackDiffuse.Load(int3(g.pixel,0)).rgb;
        float3 delta=lerp(fallback,max(result,0.0),saturate(confidence))-fallback;
        RtResolvedAuxiliary[pixel]=float4(delta,g.valid?1.0:0.0);
        float4 original=RtOriginalSubsurface.Load(int3(g.pixel,0));
        RtAdjustedSubsurface[pixel]=float4(original.rgb+delta,original.a);
    }
    RtRawStatsOutput[pixel]=float4(distanceSum,luminance,secondMoment,confidence);
    if((RtFrameInfo.w&16u)!=0u)
        RtRawMetricCountOutput[pixel]=float2(acceptedPaths?rawMetricSum/float(acceptedPaths):0.0,float(acceptedPaths));
}
#if !HS_RAY_SER
[numthreads(8,8,1)]void GITraceCS(uint3 dispatch:SV_DispatchThreadID){RtTraceGiPixel(dispatch.xy);}
[numthreads(8,8,1)]void GITemporalCS(uint3 dispatch:SV_DispatchThreadID){RtFilterTemporal(dispatch.xy,false);}
[numthreads(8,8,1)]void GISpatialCS(uint3 dispatch:SV_DispatchThreadID){RtFilterSpatial(dispatch.xy,false);}
#endif
