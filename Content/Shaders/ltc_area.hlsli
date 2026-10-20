#ifndef HS_LTC_AREA_HLSLI
#define HS_LTC_AREA_HLSLI

// Heitz, Dupuy, Hill, Neubelt (SIGGRAPH 2016). The independently produced
// LTC1 package fits this project's isotropic GGX/uncorrelated Smith model.
// These are fit data, not an environment DFG table. x=roughness,
// y=sqrt(1-NoV); endpoint samples lie at the centers of a 64x64 texture.
Texture2D<float4> HsLtcMatrix : register(t4, space19);
Texture2D<float4> HsLtcAmplitude : register(t5, space19);
static const float HsAreaPi=3.14159265358979323846;
static const uint HsAreaSampleCount=32u;

float HsAreaRadicalInverse(uint bits)
{
    bits=reversebits(bits); return float(bits)*2.3283064365386963e-10;
}
bool HsAreaLightVisible(HsGpuLightRecord light,float3 world,out float range_weight)
{
    range_weight=0.0;
    const float3 delta=world-light.position_range.xyz;
    const float distance_squared=dot(delta,delta),range_squared=light.position_range.w*light.position_range.w;
    if(!isfinite(distance_squared) || distance_squared>=range_squared || light.color_intensity.w<=0.0) return false;
    const float side=dot(light.direction_cos_outer.xyz,delta);
    // A point on the emitting plane has zero projected emitter area. Neither
    // the area sampler nor its solid-angle density is defined at the origin.
    if(side==0.0 || ((light.metadata.w&4u)==0u && side<0.0)) return false;
    const float edge=saturate(1.0-distance_squared/range_squared);
    range_weight=edge*edge; return true;
}
float3 HsAreaPoint(HsGpuLightRecord light,float2 sample)
{
    float2 coordinate;
    if(light.identity.w==3u) coordinate=sample*2.0-1.0;
    else
    {
        const float radius=sqrt(sample.x),phi=2.0*HsAreaPi*sample.y;
        coordinate=radius*float2(cos(phi),sin(phi));
    }
    return light.position_range.xyz+light.area_tangent_half_width.xyz*(coordinate.x*light.area_tangent_half_width.w)+
        light.area_bitangent_half_height.xyz*(coordinate.y*light.area_bitangent_half_height.w);
}
bool HsAreaSampleDirection(HsGpuLightRecord light,float3 world,float2 sample,
    out float3 direction,out float solid_angle_pdf)
{
    direction=0.0; solid_angle_pdf=0.0;
    const float3 delta=HsAreaPoint(light,sample)-world; const float square=dot(delta,delta);
    if(square<=0.0 || !isfinite(square)) return false;
    direction=delta*rsqrt(square);
    const float cosine=abs(dot(light.direction_cos_outer.xyz,-direction));
    if(cosine<=0.0) return false;
    solid_angle_pdf=square/(light.shape_cone.x*cosine);
    return solid_angle_pdf>0.0 && isfinite(solid_angle_pdf);
}
bool HsAreaIntersect(HsGpuLightRecord light,float3 world,float3 direction,out float solid_angle_pdf)
{
    solid_angle_pdf=0.0;
    const float denominator=dot(light.direction_cos_outer.xyz,direction);
    if(denominator==0.0) return false;
    const float distance=dot(light.direction_cos_outer.xyz,light.position_range.xyz-world)/denominator;
    if(distance<=0.0 || !isfinite(distance)) return false;
    const float3 relative=world+distance*direction-light.position_range.xyz;
    const float2 coordinate=float2(dot(relative,light.area_tangent_half_width.xyz)/light.area_tangent_half_width.w,
        dot(relative,light.area_bitangent_half_height.xyz)/light.area_bitangent_half_height.w);
    if(light.identity.w==3u ? any(abs(coordinate)>1.0) : dot(coordinate,coordinate)>1.0) return false;
    solid_angle_pdf=distance*distance/(light.shape_cone.x*abs(denominator));
    return solid_angle_pdf>0.0 && isfinite(solid_angle_pdf);
}
float3 HsLtcTransform(float3 direction,float3 tangent,float3 bitangent,float3 normal,float4 matrix)
{
    const float3 local=float3(dot(direction,tangent),dot(direction,bitangent),dot(direction,normal));
    return float3(matrix.x*local.x+matrix.y*local.z,local.y,matrix.z*local.x+matrix.w*local.z);
}
float3 HsLtcEdge(float3 from,float3 to)
{
    const float from_square=dot(from,from),to_square=dot(to,to);
    if(from_square<=0.0 || to_square<=0.0) return 0.0;
    from*=rsqrt(from_square); to*=rsqrt(to_square);
    const float3 axis=cross(from,to); const float sine=length(axis);
    return sine>0.0 ? axis*(atan2(sine,clamp(dot(from,to),-1.0,1.0))/sine):0.0;
}
float HsLtcRectangle(float3 center,float3 width,float3 height)
{
    float3 corners[4]={center-width-height,center+width-height,center+width+height,center-width+height};
    float3 clipped[8]; uint count=0u;
    // Exact receiver-horizon clipping in the inverse-transformed space.
    [unroll] for(uint i=0u;i<4u;++i)
    {
        const float3 from=corners[i],to=corners[(i+1u)&3u];
        const bool from_inside=from.z>0.0,to_inside=to.z>0.0;
        if(from_inside) clipped[count++]=from;
        if(from_inside!=to_inside) clipped[count++]=lerp(from,to,from.z/(from.z-to.z));
    }
    if(count<3u) return 0.0;
    float3 integral=0.0;
    [loop] for(uint i=0u;i<count;++i) integral+=HsLtcEdge(clipped[i],clipped[(i+1u)%count]);
    return saturate(abs(integral.z)/(2.0*HsAreaPi));
}
float HsLtcEllipse(float3 center,float3 width,float3 height)
{
    // True ellipse boundary r(phi)=c+a*cos(phi)+b*sin(phi). Its cosine/pi
    // form factor is integral cross(r,r').z/|r|^2 /(2*pi), plus a closing
    // horizon arc when clipped. Fixed 8-panel GL8 quadrature is 64 boundary
    // samples, not a polygon approximation of the authored emitter.
    const float horizon_amplitude=length(float2(width.z,height.z));
    float middle=0.0,half_arc=HsAreaPi; bool clipped=false;
    if(horizon_amplitude>0.0)
    {
        if(center.z<=-horizon_amplitude) return 0.0;
        if(center.z<horizon_amplitude)
        {
            middle=atan2(height.z,width.z); half_arc=acos(clamp(-center.z/horizon_amplitude,-1.0,1.0));
            clipped=true;
        }
    }
    else if(center.z<=0.0) return 0.0;
    // Positive nodes/weights of Gauss-Legendre order8 (exact through degree15).
    static const float nodes[4]={0.1834346425,0.5255324099,0.7966664774,0.9602898565};
    static const float weights[4]={0.3626837834,0.3137066459,0.2223810345,0.1012285363};
    float integral=0.0; const float panel_half=half_arc/8.0;
    [unroll] for(uint panel=0u;panel<8u;++panel)
    {
        const float panel_center=middle-half_arc+(2.0*float(panel)+1.0)*panel_half;
        [unroll] for(uint node=0u;node<4u;++node)
            [unroll] for(uint side=0u;side<2u;++side)
            {
                const float phi=panel_center+(side==0u?-1.0:1.0)*nodes[node]*panel_half;
                float sine,cosine; sincos(phi,sine,cosine);
                const float3 boundary_position=center+width*cosine+height*sine,derivative=height*cosine-width*sine;
                integral+=weights[node]*panel_half*cross(boundary_position,derivative).z/dot(boundary_position,boundary_position);
            }
    }
    if(clipped)
    {
        const float start=middle-half_arc,end=middle+half_arc;
        const float3 first=center+width*cos(start)+height*sin(start),last=center+width*cos(end)+height*sin(end);
        integral+=HsLtcEdge(float3(last.xy,0.0),float3(first.xy,0.0)).z;
    }
    return saturate(abs(integral)/(2.0*HsAreaPi));
}
float HsLtcIntegral(HsGpuLightRecord light,float3 world,float3 tangent,float3 bitangent,float3 normal,float4 matrix)
{
    const float3 center=HsLtcTransform(light.position_range.xyz-world,tangent,bitangent,normal,matrix);
    const float3 width=HsLtcTransform(light.area_tangent_half_width.xyz*light.area_tangent_half_width.w,
        tangent,bitangent,normal,matrix);
    const float3 height=HsLtcTransform(light.area_bitangent_half_height.xyz*light.area_bitangent_half_height.w,
        tangent,bitangent,normal,matrix);
    return light.identity.w==3u?HsLtcRectangle(center,width,height):HsLtcEllipse(center,width,height);
}

#ifdef HS_BSDF_METALLIC_ROUGHNESS_HLSLI
void HsAreaEvaluateSampled(HsGpuLightRecord light,HsSurfaceClosure closure,float3 world,float3 view,
    float range_weight,float base_fresnel_override,out float3 diffuse,out float3 specular)
{
    diffuse=0.0; specular=0.0;
    const float3 radiance=light.color_intensity.rgb*light.color_intensity.w*range_weight;
    // Balance MIS: uniform actual emitter area + the unchanged shared BSDF
    // proposal. Layered/anisotropic/thin/solid closures keep their actual eval,
    // sample and PDF; failed/null BSDF samples retain the 32-sample denominator.
    [loop] for(uint i=0u;i<HsAreaSampleCount;++i)
    {
        const float2 sample=float2((float(i)+0.5)/float(HsAreaSampleCount),HsAreaRadicalInverse(i));
        float3 direction; float area_pdf;
        if(HsAreaSampleDirection(light,world,sample,direction,area_pdf))
        {
            const float bsdf_pdf=HsBsdfPdf(closure,view,direction);
            float3 d,s; HsBsdfEvaluateComponentsOptical(closure,view,direction,base_fresnel_override,d,s);
            const float3 incident=radiance*abs(dot(closure.normal,direction))/
                ((area_pdf+bsdf_pdf)*float(HsAreaSampleCount));
            diffuse+=d*incident; specular+=s*incident;
        }
        const HsBsdfSampleResult proposal=HsBsdfSample(closure,view,frac((float(i)+0.5)*0.61803398875),sample);
        if(proposal.valid && HsAreaIntersect(light,world,proposal.wi,area_pdf))
        {
            float3 d,s; HsBsdfEvaluateComponentsOptical(closure,view,proposal.wi,base_fresnel_override,d,s);
            const float3 incident=radiance*abs(dot(closure.normal,proposal.wi))/
                ((area_pdf+proposal.pdf)*float(HsAreaSampleCount));
            diffuse+=d*incident; specular+=s*incident;
        }
    }
}
void HsAreaEvaluateComponents(HsGpuLightRecord light,HsSurfaceClosure closure,float3 world,float3 view,
    float base_fresnel_override,out float3 diffuse,out float3 specular)
{
    diffuse=0.0; specular=0.0; float range_weight;
    if(closure.unlit || !HsAreaLightVisible(light,world,range_weight)) return;
    const bool supported=closure.anisotropy==0.0 && closure.coat_weight==0.0 && closure.sheen_weight==0.0 &&
        closure.transmission_weight==0.0 && closure.solid_transmission_weight==0.0;
    if(!supported)
    {
        HsAreaEvaluateSampled(light,closure,world,view,range_weight,base_fresnel_override,diffuse,specular); return;
    }
    const float3 normal=normalize(closure.normal); const float no_v=dot(normal,view);
    if(no_v<=0.0) return;
    const float3 projected=view-normal*no_v; const float length_squared=dot(projected,projected);
    const float3 tangent=length_squared>0.0?projected*rsqrt(length_squared):HsBsdfTangent(normal);
    const float3 bitangent=cross(normal,tangent);
    const float2 uv=(saturate(float2(closure.roughness,sqrt(saturate(1.0-no_v))))*63.0+0.5)/64.0;
    const float4 matrix=HsLtcMatrix.SampleLevel(LinearClamp,uv,0);
    const float2 amplitude=HsLtcAmplitude.SampleLevel(LinearClamp,uv,0).xy;
    // A valid produced fit has a positive determinant. Bilinear interpolation
    // can leave that family for very steep adjacent fits; use the actual BSDF
    // integral for that domain rather than evaluating an invalid distribution.
    if(!all(isfinite(matrix)) || matrix.x*matrix.w-matrix.y*matrix.z<=0.0)
    {
        HsAreaEvaluateSampled(light,closure,world,view,range_weight,base_fresnel_override,diffuse,specular); return;
    }
    const float3 radiance=light.color_intensity.rgb*light.color_intensity.w*range_weight;
    const float3 f0=HsBsdfF0(closure);
    const float cosine_integral=HsLtcIntegral(light,world,tangent,bitangent,normal,float4(1,0,0,1));
    // The shared substrate has entry/exit macro Fresnel, rather than plain
    // Lambert. Retain its (1-NoL)^5 moment using actual emitter-area quadrature.
    float grazing_integral=0.0;
    [loop] for(uint i=0u;i<HsAreaSampleCount;++i)
    {
        float3 direction; float pdf;
        if(HsAreaSampleDirection(light,world,float2((float(i)+0.5)/float(HsAreaSampleCount),HsAreaRadicalInverse(i)),direction,pdf))
        {
            const float no_l=saturate(dot(normal,direction));
            grazing_integral+=no_l*pow(1.0-no_l,5.0)/(HsAreaPi*pdf*float(HsAreaSampleCount));
        }
    }
    grazing_integral=clamp(grazing_integral,0.0,cosine_integral); // Exact moment lies in this interval.
    diffuse=radiance*(1.0-HsBsdfSchlickFresnel(f0,no_v))*(1.0-f0)*
        ((1.0-HsBsdfMetallic(closure))*closure.base_color)*(cosine_integral-grazing_integral);
    const float specular_integral=HsLtcIntegral(light,world,tangent,bitangent,normal,matrix);
    specular=radiance*specular_integral*(f0*amplitude.x+(1.0-f0)*amplitude.y);
}
#endif
#endif
