// Output-resolution, pre-exposed scene-linear effects. Display transforms and
// UI encoding remain in the existing post graph. Primary algorithm references:
// GPU Gems 3 chapters 27/28; GPU Gems 2 chapter24 (3D LUT sampling/shaping).
cbuffer HdrPostCB : register(b0, space17)
{
    row_major float4x4 InverseJitteredViewProjection;
    float4 CameraRelativePosition;
    float4 CameraForward;
    float4 RenderSize;
    float4 OutputSize;
    float4 JitterUv;
    float4 FrameParameters;
    float4 Lens;
    float4 GradeLift;
    float4 GradeContrast;
    float4 GradeGain;
    float4 EffectParameters;
    uint4 Options;
    row_major float4x4 PreviousUnjitteredViewProjection;
};
Texture2D<float4> SourceHdr : register(t0, space17);
Texture2D<float> OpaqueDepth : register(t1, space17);
Texture2D<float2> OpaqueMotion : register(t2, space17);
Texture2D<float2> OpaqueHistory : register(t3, space17);
Texture2D<float> TransparentDepth : register(t4, space17);
Texture2D<float2> TransparentMotion : register(t5, space17);
Texture2D<float2> TransparentHistory : register(t6, space17);
Texture2D<float4> TransparentClassification : register(t7, space17);
Texture2D<float4> DisplayGuide : register(t8, space17); // positive view distance, motion UV xy, confidence.
Texture2D<float> SignedCoC : register(t9, space17);
Texture2D<float> ForegroundRadius : register(t10, space17);
Texture3D<float4> GradeLut : register(t11, space17);
Texture2D<float2> OpaqueNormal : register(t12, space17);
Texture2D<float2> DisplayNormal : register(t13, space17);
RWTexture2D<float4> ColorOutput : register(u0, space17);
RWTexture2D<float4> GuideOutput : register(u1, space17);
RWTexture2D<float> CoCOutput : register(u2, space17);
RWTexture2D<float> ForegroundOutput : register(u3, space17);
RWTexture3D<float4> LutOutput : register(u4, space17);
RWTexture2D<float2> NormalOutput : register(u5, space17);
SamplerState LinearClamp : register(s0, space17);
// Prepare limits the radius to32. Cooperative loading keeps foreground dilation
// from issuing one global texture read for every candidate at every pixel.
groupshared float CoCTile[72 * 72];
groupshared uint CoCTileMaximum;

int2 OutputPixel(float2 uv)
{
    return clamp(int2(uv * OutputSize.xy), int2(0, 0), int2(OutputSize.xy) - 1);
}
float2 OutputUv(uint2 pixel) { return (float2(pixel) + 0.5) * OutputSize.zw; }
float3 FiniteRadiance(float3 value)
{
    return float3(isfinite(value.x) ? clamp(value.x, 0.0, 65504.0) : 0.0,
                  isfinite(value.y) ? clamp(value.y, 0.0, 65504.0) : 0.0,
                  isfinite(value.z) ? clamp(value.z, 0.0, 65504.0) : 0.0);
}
float4 LoadRenderGuide(int2 pixel)
{
    const int3 location = int3(pixel, 0);
    float depth = OpaqueDepth.Load(location);
    float2 motion = OpaqueMotion.Load(location);
    float confidence = OpaqueHistory.Load(location).y;
    if (Options.y != 0u)
    {
        const float hero_depth = TransparentDepth.Load(location);
        if (hero_depth > depth && TransparentClassification.Load(location).z > 0.0)
        {
            depth = hero_depth;
            motion = TransparentMotion.Load(location);
            confidence = TransparentHistory.Load(location).y;
        }
    }
    const bool valid = FrameParameters.y > 0.0 && confidence > 0.0 && all(isfinite(motion));
    return float4(depth, valid ? motion : 0.0, valid ? saturate(confidence) : 0.0);
}
[numthreads(8, 8, 1)]
void HdrPostGuidesCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(OutputSize.xy))) return;
    // Color/history coordinates are unjittered; current deformation guides are
    // in jittered render coordinates. Select actual nearest front from 2x2.
    const float2 render_position = (OutputUv(dispatch.xy) + JitterUv.xy) * RenderSize.xy - 0.5;
    const int2 first = int2(floor(render_position));
    int2 selected_pixel = clamp(first, int2(0, 0), int2(RenderSize.xy) - 1);
    float4 selected = LoadRenderGuide(selected_pixel);
    [unroll] for (uint index = 1u; index < 4u; ++index)
    {
        const int2 pixel = clamp(first + int2(index & 1u, index >> 1u), int2(0, 0), int2(RenderSize.xy) - 1);
        const float4 candidate = LoadRenderGuide(pixel);
        if (candidate.x > selected.x) { selected = candidate; selected_pixel = pixel; }
    }
    const float2 raster_uv = (float2(selected_pixel) + 0.5) * RenderSize.zw;
    float distance = 1000000.0; // Infinite sky is outside the finite focus plane.
    if (selected.x > 0.0)
    {
        const float4 relative = mul(float4(raster_uv * float2(2, -2) + float2(-1, 1), selected.x, 1),
                                   InverseJitteredViewProjection);
        if (all(isfinite(relative)) && relative.w != 0.0)
            distance = max(dot(relative.xyz / relative.w - CameraRelativePosition.xyz, CameraForward.xyz), 0.0);
    }
    else if (FrameParameters.y > 0.0)
    {
        const float4 near_relative = mul(float4(raster_uv * float2(2,-2) + float2(-1,1),1,1),
            InverseJitteredViewProjection);
        if (all(isfinite(near_relative)) && near_relative.w != 0.0)
        {
            const float3 direction = normalize(near_relative.xyz / near_relative.w - CameraRelativePosition.xyz);
            const float4 previous = mul(float4(direction,0),PreviousUnjitteredViewProjection);
            if (all(isfinite(previous)) && previous.w > 0.0)
            {
                const float2 previous_uv = previous.xy / previous.w * float2(0.5,-0.5) + 0.5;
                if (all(previous_uv >= 0.0) && all(previous_uv < 1.0))
                {
                    selected.yz = previous_uv - (raster_uv - JitterUv.xy);
                    selected.w = 1.0;
                }
            }
        }
    }
    GuideOutput[dispatch.xy] = float4(distance, selected.yzw);
    if (Options.w != 0u)
    {
        // Thin-lens circle diameter = f^2/(N*(focus-f)) * (z-focus)/z.
        // Store signed radius in display pixels: foreground negative, far positive.
        const float radius = 0.5 * Lens.y * Lens.y / (Lens.z * (Lens.x - Lens.y)) *
            (distance - Lens.x) / max(distance, Lens.y) * OutputSize.x / Lens.w;
        CoCOutput[dispatch.xy] = clamp(radius, -EffectParameters.x, EffectParameters.x);
    }
    if (Options.x != 0u)
    {
        float2 normal = OpaqueNormal.Load(int3(selected_pixel,0));
        if (Options.y != 0u && TransparentDepth.Load(int3(selected_pixel,0)) > OpaqueDepth.Load(int3(selected_pixel,0)))
        {
            const float4 classification = TransparentClassification.Load(int3(selected_pixel,0));
            if (classification.z > 0.0) normal = classification.xy;
        }
        NormalOutput[dispatch.xy] = normal;
    }
}
float PixelFootprint(float depth)
{
    // Use the live rendering projection for rejection. The independent
    // physical lens controls CoC and must not stand in for the render FOV.
    const float4 a = mul(float4(0, 0, 1, 1), InverseJitteredViewProjection);
    const float4 b = mul(float4(2.0 * OutputSize.z, 0, 1, 1), InverseJitteredViewProjection);
    const float3 ray_a = a.xyz / a.w - CameraRelativePosition.xyz;
    const float3 ray_b = b.xyz / b.w - CameraRelativePosition.xyz;
    const float forward_a = dot(ray_a, CameraForward.xyz), forward_b = dot(ray_b, CameraForward.xyz);
    return forward_a > 0.0 && forward_b > 0.0 ? depth * length(ray_a / forward_a - ray_b / forward_b) : 0.0;
}
float3 ViewRay(float2 output_uv)
{
    const float2 raster_uv = output_uv + JitterUv.xy;
    const float4 relative = mul(float4(raster_uv * float2(2,-2)+float2(-1,1),1,1),InverseJitteredViewProjection);
    const float3 ray = relative.xyz / relative.w - CameraRelativePosition.xyz;
    return ray / dot(ray,CameraForward.xyz);
}
float3 DecodeNormal(float2 oct)
{
    float3 normal = float3(oct,1.0-abs(oct.x)-abs(oct.y));
    if (normal.z < 0.0)
        normal.xy = (1.0-abs(normal.yx))*float2(normal.x<0.0?-1.0:1.0,normal.y<0.0?-1.0:1.0);
    return normalize(normal);
}
[numthreads(8, 8, 1)]
void HdrPostMotionBlurCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(OutputSize.xy))) return;
    const float2 uv = OutputUv(dispatch.xy);
    const float4 guide = DisplayGuide.Load(int3(dispatch.xy, 0));
    const float3 center = FiniteRadiance(SourceHdr.Load(int3(dispatch.xy, 0)).rgb);
    const float2 velocity = guide.yz * FrameParameters.z;
    float3 color = 0.0; float normalization = 0.0;
    const float3 normal = DecodeNormal(DisplayNormal.Load(int3(dispatch.xy,0)));
    const float plane_distance = guide.x * dot(normal,ViewRay(uv));
    [loop] for (uint index = 0u; index < Options.x; ++index)
    {
        const float time = (float(index) + 0.5) / float(Options.x) - 0.5;
        const float2 sample_uv = uv + velocity * time;
        if (any(sample_uv < 0.0) || any(sample_uv >= 1.0)) continue;
        const float sampled_depth = DisplayGuide.Load(int3(OutputPixel(sample_uv), 0)).x;
        const float denominator = dot(normal,ViewRay(sample_uv));
        const float expected_depth = guide.x == 1000000.0 ? guide.x : plane_distance / denominator;
        // Reject against the actual front's tangent plane. Neighbor depths from
        // different objects cannot enlarge this footprint or defeat rejection.
        if (!isfinite(expected_depth) || expected_depth <= 0.0 ||
            abs(sampled_depth-expected_depth) > PixelFootprint(expected_depth)) continue;
        // Match the tested guide texel. Filtering across an untested depth edge
        // would reintroduce foreground/background radiance after rejection.
        color += FiniteRadiance(SourceHdr.Load(int3(OutputPixel(sample_uv), 0)).rgb);
        normalization += 1.0;
    }
    ColorOutput[dispatch.xy] = float4(guide.w > 0.0 && normalization > 0.0 ? color / normalization : center, 1);
}
[numthreads(8, 8, 1)]
void HdrPostDilateCS(uint3 dispatch : SV_DispatchThreadID, uint3 group : SV_GroupID,
                     uint3 local : SV_GroupThreadID)
{
    const int budget = int(ceil(EffectParameters.x));
    const uint tile_width = 8u + 2u * uint(budget);
    const int2 tile_origin = int2(group.xy * 8u) - budget;
    if (local.x == 0u && local.y == 0u) CoCTileMaximum = 0u;
    GroupMemoryBarrierWithGroupSync();
    float lane_maximum = 0.0;
    [loop] for (uint index = local.y * 8u + local.x; index < tile_width * tile_width; index += 64u)
    {
        const int2 pixel = tile_origin + int2(index % tile_width,index / tile_width);
        CoCTile[index] = any(pixel < 0) || any(pixel >= int2(OutputSize.xy)) ? 0.0 :
            max(-SignedCoC.Load(int3(pixel,0)),0.0);
        lane_maximum = max(lane_maximum, CoCTile[index]);
    }
    InterlockedMax(CoCTileMaximum, asuint(lane_maximum));
    GroupMemoryBarrierWithGroupSync();
    if (any(dispatch.xy >= uint2(OutputSize.xy))) return;
    float radius = 0.0;
    // No source circle in this tile can reach beyond its largest radius.
    const int search_budget = min(budget, int(ceil(asfloat(CoCTileMaximum))));
    [loop] for (int y = -search_budget; y <= search_budget; ++y)
    [loop] for (int x = -search_budget; x <= search_budget; ++x)
    {
        const int2 tile_pixel = int2(local.xy) + budget + int2(x,y);
        const float near_radius = CoCTile[uint(tile_pixel.y) * tile_width + uint(tile_pixel.x)];
        if (float(x*x + y*y) <= near_radius * near_radius) radius = max(radius, near_radius);
    }
    ForegroundOutput[dispatch.xy] = radius;
}
[numthreads(8, 8, 1)]
void HdrPostDoFCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(OutputSize.xy))) return;
    const float2 uv = OutputUv(dispatch.xy);
    const float center_coc = SignedCoC.Load(int3(dispatch.xy, 0));
    const float center_depth = DisplayGuide.Load(int3(dispatch.xy, 0)).x;
    const float3 center = FiniteRadiance(SourceHdr.Load(int3(dispatch.xy, 0)).rgb);
    const float gather_radius = max(max(center_coc, 0.0), ForegroundRadius.Load(int3(dispatch.xy, 0)));
    if (gather_radius == 0.0) { ColorOutput[dispatch.xy] = float4(center, 1); return; }
    float3 near_color = 0.0, far_color = 0.0;
    float near_weight = 0.0, far_weight = 0.0, near_coverage = 0.0;
    const uint taps = 32u;
    [loop] for (uint index = 0u; index < taps; ++index)
    {
        const float angle = float(index) * 2.39996323;
        const float radial = sqrt((float(index) + 0.5) / float(taps));
        const float2 offset = float2(cos(angle), sin(angle)) * radial * gather_radius;
        const float2 sample_uv = uv + offset * OutputSize.zw;
        if (any(sample_uv < 0.0) || any(sample_uv >= 1.0)) continue;
        const int2 pixel = OutputPixel(sample_uv);
        const float coc = SignedCoC.Load(int3(pixel, 0));
        const float depth = DisplayGuide.Load(int3(pixel, 0)).x;
        const float3 sample_color = FiniteRadiance(SourceHdr.Load(int3(pixel, 0)).rgb);
        const float distance_pixels = length(offset);
        const float near_radius = max(-coc, 0.0);
        if (near_radius > 0.0 && distance_pixels < near_radius && depth <= center_depth + PixelFootprint(center_depth))
        {
            const float aperture_weight = 1.0 / max(near_radius * near_radius, 1.0);
            near_color += sample_color * aperture_weight; near_weight += aperture_weight;
            near_coverage += gather_radius * gather_radius * aperture_weight / float(taps);
        }
        if (center_coc > 0.0 && coc > 0.0 && distance_pixels < min(center_coc, coc) &&
            depth >= center_depth - PixelFootprint(center_depth))
        {
            far_color += sample_color; far_weight += 1.0;
        }
    }
    const float3 far_result = far_weight > 0.0 ? far_color / far_weight : center;
    const float3 near_result = near_weight > 0.0 ? near_color / near_weight : far_result;
    ColorOutput[dispatch.xy] = float4(lerp(far_result, near_result, saturate(near_coverage)), 1);
}
float3 ShapeHdr(float3 value) { return log2(1.0 + FiniteRadiance(value)) / EffectParameters.y; }
float3 UnshapeHdr(float3 value) { return exp2(saturate(value) * EffectParameters.y) - 1.0; }
float3 GradeInput(uint2 pixel)
{
    const int2 location = int2(pixel);
    const float3 center = FiniteRadiance(SourceHdr.Load(int3(location, 0)).rgb);
    if (FrameParameters.w <= 0.0) return center;
    // Exactly HsSharpenTemporal's five-tap math at display pixel centers, using
    // active-rectangle Loads so a larger persistent allocation cannot leak in.
    const int2 last = int2(OutputSize.xy) - 1;
    const float3 north = FiniteRadiance(SourceHdr.Load(int3(clamp(location + int2(0,-1),0,last),0)).rgb);
    const float3 south = FiniteRadiance(SourceHdr.Load(int3(clamp(location + int2(0,1),0,last),0)).rgb);
    const float3 east = FiniteRadiance(SourceHdr.Load(int3(clamp(location + int2(1,0),0,last),0)).rgb);
    const float3 west = FiniteRadiance(SourceHdr.Load(int3(clamp(location + int2(-1,0),0,last),0)).rgb);
    const float3 minimum = min(center,min(min(north,south),min(east,west)));
    const float3 maximum = max(center,max(max(north,south),max(east,west)));
    const float3 sharpened = center+saturate(FrameParameters.w)*(center-0.25*(north+south+east+west));
    return max(clamp(sharpened,minimum,maximum),0.0);
}
[numthreads(4, 4, 4)]
void HdrPostLutInitCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch >= Options.z)) return;
    const float3 scene = UnshapeHdr(float3(dispatch) / float(Options.z - 1u));
    // Current ToneMapPS has no artistic grade. Identity defaults preserve it;
    // explicit scene-linear lift/gain and contrast around middle gray are baked.
    const float3 lifted = max(scene + GradeLift.xyz, 0.0);
    const float3 graded = pow(lifted / 0.18, GradeContrast.xyz) * 0.18 * GradeGain.xyz;
    // Store the shaped result, so the identity lattice remains exactly linear
    // under trilinear interpolation and does not bend HDR between LUT nodes.
    LutOutput[dispatch] = float4(ShapeHdr(graded), 1);
}
[numthreads(8, 8, 1)]
void HdrPostGradeCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (any(dispatch.xy >= uint2(OutputSize.xy))) return;
    const float3 coordinate = ShapeHdr(GradeInput(dispatch.xy));
    // Texture samples address texel centers; compensate all three axes.
    const float3 lut_uv = (coordinate * float(Options.z - 1u) + 0.5) / float(Options.z);
    const float3 shaped = GradeLut.SampleLevel(LinearClamp, lut_uv, 0).rgb;
    ColorOutput[dispatch.xy] = float4(FiniteRadiance(UnshapeHdr(shaped)), 1);
}
