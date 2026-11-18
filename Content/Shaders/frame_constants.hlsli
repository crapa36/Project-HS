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
    uint4 MonsterAssetMeta[6];
    uint4 MonsterClipMeta[6][5];
    float4 RenderOptions;
    uint4 ParticleOptions;
    float4 GrassBenders[32];
    uint4 GrassBenderCount;
    uint4 AuthoredParticleClock;
    uint4 VfxDecalCount;
    uint4 SkinCacheOptions; // w: static instance generation for probe capture.
    row_major float4x4 InverseViewProjection;
    row_major float4x4 CurrentUnjitteredViewProjection;
    row_major float4x4 PreviousUnjitteredViewProjection;
    float4 SurfaceViewOrigin;
    float4 SurfaceJitterUv; // xy: current jitter UV; zw: previous jitter UV.
    float4 LodViewRight;
    float4 LodViewUp;
    float4 LodViewForward;
    float4 LodProjection; // x: perspective pixel scale; yzw: cascade pixels/world unit.
    float4 LodOptions; // pixel tolerance, hysteresis, near distance, fade step.
    uint4 LodClock; // frame, session epoch, valid view history, diagnostic flags.
    uint4 MaterialParameterBases[4]; // Same selected table used by raster and deferred.
    uint4 MaterialVaryingMasks[4];
    uint4 MaterialSidecarOffsets[2];
    uint4 MaterialSidecarInfo; // union mask, packed word count, plane count, reserved.
    float4 OutputSize; // Output extent in xy, reciprocal extent in zw.
    float4 PostOptions; // Bloom strength, reserved, grading, sharpening.
    float4 MaterialQualityOptions; // POM march/refinement, packed binary16 terrain fade range, screen refraction quality.
    float4 DisplayOptions; // Native HDR enabled, paper white nits, peak nits, encoded DevTools in PostA.
    row_major float4x4 InverseUnjitteredViewProjection;
    float4 LightingOptions;
    uint4 CharacterMaterialParameterBases[2];
    uint4 CharacterMaterialVaryingMasks[2];
    float4 ProbeOptions; // cosine/sine yaw, maximum prefiltered LOD, enabled.
    float4 PreviousGrassBenders[32];
    float4 PreviousGrassState; // time, bender count, valid, reserved.
    uint4 SceneTableBounds; // Serialized dynamic scene byte length in x; reserved yzw.
    float4 GrassBenderGrid;
    uint4 GrassBenderMasks[16];
    float4 PreviousGrassBenderGrid;
    uint4 PreviousGrassBenderMasks[16];
    float4 ParticleTerrain; // Upper floor, reserved yz, bit-cast section count.
    row_major float4x4 PreviousInstanceUnjitteredViewProjection;
};

cbuffer PassConstants : register(b1)
{
    uint PassValue;
    // bit 0: static scene; bits 1-31: reflected material parameter slot base.
    uint SceneDrawMaterial;
};
