cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProjection;
    float4x4 PreviousViewProjection;
    float4 TemporalOptions;
    float4 CameraTime;
    float4 LightDirectionIntensity;
    float4 LightColor;
    float4 ScreenSize;
    float4 CameraForwardSoftness;
    float4x4 ShadowViewProjection[3];
    float4 ShadowAtlasScaleOffset[3];
    float4 ShadowAtlasTexelSize;
    row_major float4x4 ArcherBones[128];
    uint4 MonsterAssetMeta[6];
    uint4 MonsterClipMeta[6][5];
    float4 RenderOptions;
    uint4 ParticleOptions;
    float4 GrassBenders[32];
    uint4 GrassBenderCount;
    uint4 AuthoredParticleClock;
    float4 VfxLightPositionRadius[32];
    float4 VfxLightColorIntensity[32];
    uint4 VfxLightCount;
    uint4 VfxDecalCount;
    uint4 SkinCacheOptions;
};

cbuffer PassConstants : register(b1)
{
    // bits 0-7 mesh, 8-11 clip, 12-31 vertex count.
    uint PassValue;
    // Bitwise float normalized animation time.
    uint SceneDrawMaterial;
};

ByteAddressBuffer SkinSource : register(t0, space3);
struct SkinMatrixElement
{
    row_major float4x4 value;
};
StructuredBuffer<SkinMatrixElement> SkinMatrices : register(t2, space3);
RWByteAddressBuffer SkinOutput : register(u0);
RWByteAddressBuffer SkinBounds : register(u1, space3);

static const uint kVertexStride = 76;

uint4 LoadBoneIndices(uint base)
{
    const uint2 packed = SkinSource.Load2(base + 24);
    return uint4(packed.x & 0xffffu, packed.x >> 16u,
                 packed.y & 0xffffu, packed.y >> 16u);
}

[numthreads(64, 1, 1)]
void SkinCacheCS(uint3 dispatch_id : SV_DispatchThreadID)
{
    const uint mesh = PassValue & 0xffu;
    const uint clip = (PassValue >> 8u) & 0xfu;
    const uint vertex_count = PassValue >> 12u;
    const uint vertex = dispatch_id.x;
    if (vertex >= vertex_count) return;

    const uint base = vertex * kVertexStride;
    const float3 source_position = asfloat(SkinSource.Load3(base));
    float3 position = source_position;
    float3 normal = asfloat(SkinSource.Load3(base + 12));
    const uint4 bone_indices = LoadBoneIndices(base);
    const float4 bone_weights = asfloat(SkinSource.Load4(base + 32));
    const uint4 tangent_bits = SkinSource.Load4(base + 56);
    float3 tangent = asfloat(tangent_bits.xyz);

    if (mesh == 0u)
    {
        const float4x4 skin =
            SkinMatrices[bone_indices.x].value * bone_weights.x +
            SkinMatrices[bone_indices.y].value * bone_weights.y +
            SkinMatrices[bone_indices.z].value * bone_weights.z +
            SkinMatrices[bone_indices.w].value * bone_weights.w;
        position = mul(skin, float4(position, 1.0)).xyz;
        // Match the monster deformation path: blended/sheared skin matrices
        // require the cofactor inverse-transpose for correct normals.
        const float3x3 linear_skin = (float3x3)skin;
        const float3x3 cofactor = float3x3(
            cross(linear_skin[1], linear_skin[2]),
            cross(linear_skin[2], linear_skin[0]),
            cross(linear_skin[0], linear_skin[1]));
        const float determinant = dot(linear_skin[0], cofactor[0]);
        normal = normalize(mul(cofactor, normal) * (determinant < 0.0 ? -1.0 : 1.0));
        tangent = normalize(mul((float3x3)skin, tangent));
    }
    else if (mesh >= 10u && mesh <= 15u)
    {
        const uint asset = mesh - 10u;
        const uint4 clip_meta = MonsterClipMeta[asset][min(clip, 4u)];
        const float normalized_time = saturate(asfloat(SceneDrawMaterial));
        const float frame_position =
            normalized_time * max((float)clip_meta.y - 1.0, 0.0);
        const uint frame0 = min((uint)floor(frame_position), clip_meta.y - 1u);
        const uint frame1 = min(frame0 + 1u, clip_meta.y - 1u);
        const float frame_alpha = frac(frame_position);
        const uint matrix_base0 = MonsterAssetMeta[asset].x + clip_meta.x +
                                  frame0 * clip_meta.w;
        const uint matrix_base1 = MonsterAssetMeta[asset].x + clip_meta.x +
                                  frame1 * clip_meta.w;
        const float4x4 skin0 =
            SkinMatrices[matrix_base0 + bone_indices.x].value * bone_weights.x +
            SkinMatrices[matrix_base0 + bone_indices.y].value * bone_weights.y +
            SkinMatrices[matrix_base0 + bone_indices.z].value * bone_weights.z +
            SkinMatrices[matrix_base0 + bone_indices.w].value * bone_weights.w;
        const float4x4 skin1 =
            SkinMatrices[matrix_base1 + bone_indices.x].value * bone_weights.x +
            SkinMatrices[matrix_base1 + bone_indices.y].value * bone_weights.y +
            SkinMatrices[matrix_base1 + bone_indices.z].value * bone_weights.z +
            SkinMatrices[matrix_base1 + bone_indices.w].value * bone_weights.w;
        // Cached deformation is the authoritative raster/shadow/history input.
        // Interpolating the two authored matrix frames removes the stepped
        // clip sampling that the legacy VS fallback still uses.
        const float4x4 skin = lerp(skin0, skin1, frame_alpha);
        position = mul(skin, float4(position, 1.0)).xyz;
        const float3x3 linear_skin = (float3x3)skin;
        const float3x3 cofactor = float3x3(
            cross(linear_skin[1], linear_skin[2]),
            cross(linear_skin[2], linear_skin[0]),
            cross(linear_skin[0], linear_skin[1]));
        const float determinant = dot(linear_skin[0], cofactor[0]);
        normal = normalize(mul(cofactor, normal) * (determinant < 0.0 ? -1.0 : 1.0));
        tangent = normalize(mul(linear_skin, tangent));

        if (mesh == 12u && clip == 2u)
        {
            const float progress = normalized_time;
            const float phase = progress * 48.0;
            const float bubble = sin(source_position.x * 43.0 + phase) *
                sin(source_position.y * 37.0 - phase * 0.73) *
                sin(source_position.z * 41.0 + phase * 0.61);
            position += normal * (0.006 * progress * bubble);
        }
    }

    SkinOutput.Store3(base, asuint(position));
    SkinOutput.Store3(base + 12, asuint(normal));
    SkinOutput.Store2(base + 24, SkinSource.Load2(base + 24));
    SkinOutput.Store4(base + 32, SkinSource.Load4(base + 32));
    SkinOutput.Store2(base + 48, SkinSource.Load2(base + 48));
    SkinOutput.Store4(base + 56, uint4(asuint(tangent), tangent_bits.w));
    SkinOutput.Store(base + 72, SkinSource.Load(base + 72));
}

groupshared float3 BoundMinimum[64];
groupshared float3 BoundMaximum[64];

[numthreads(64, 1, 1)]
void SkinBoundsCS(uint thread_id : SV_GroupIndex)
{
    const uint vertex_count = PassValue >> 12u;
    float3 minimum = float3(3.402823466e+38, 3.402823466e+38, 3.402823466e+38);
    float3 maximum = -minimum;
    for (uint vertex = thread_id; vertex < vertex_count; vertex += 64u)
    {
        const float3 position = asfloat(SkinOutput.Load3(vertex * kVertexStride));
        minimum = min(minimum, position);
        maximum = max(maximum, position);
    }
    BoundMinimum[thread_id] = minimum;
    BoundMaximum[thread_id] = maximum;
    GroupMemoryBarrierWithGroupSync();

    for (uint width = 32u; width != 0u; width >>= 1u)
    {
        if (thread_id < width)
        {
            BoundMinimum[thread_id] =
                min(BoundMinimum[thread_id], BoundMinimum[thread_id + width]);
            BoundMaximum[thread_id] =
                max(BoundMaximum[thread_id], BoundMaximum[thread_id + width]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (thread_id == 0u)
    {
        const float3 center = (BoundMinimum[0] + BoundMaximum[0]) * 0.5;
        const float radius = length(BoundMaximum[0] - center) + 0.0001;
        SkinBounds.Store4(0, asuint(float4(center, max(radius, 0.0001))));
    }
}
