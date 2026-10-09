cbuffer FrameConstants : register(b0)
{
    float4x4 ViewProjection;
    float4x4 PreviousViewProjection;
    float4 TemporalOptions;
    float4 CameraTime;
    float4 LightDirectionIntensity;
    float4 LightColor;
    float4 ScreenSize;
};

cbuffer PyramidConstants : register(b1)
{
    uint PyramidLevel;
    uint PyramidReserved;
};

Texture2D<float> SurfaceDepth : register(t0, space6);
RWStructuredBuffer<float> CurrentHzb : register(u0);

uint LevelOffset(uint level, out uint width, out uint height)
{
    width = max((uint)ScreenSize.x, 1u);
    height = max((uint)ScreenSize.y, 1u);
    uint offset = 0;
    for (uint index = 0; index < level; ++index)
    {
        offset += width * height;
        width = max((width + 1u) / 2u, 1u);
        height = max((height + 1u) / 2u, 1u);
    }
    return offset;
}

[numthreads(8, 8, 1)]
void DepthPyramidInitCS(uint3 dispatch_id : SV_DispatchThreadID)
{
    const uint width = max((uint)ScreenSize.x, 1u);
    const uint height = max((uint)ScreenSize.y, 1u);
    if (dispatch_id.x >= width || dispatch_id.y >= height) return;
    const uint2 pixel = dispatch_id.xy;
    CurrentHzb[pixel.y * width + pixel.x] = SurfaceDepth.Load(int3(pixel, 0));
}

[numthreads(8, 8, 1)]
void DepthPyramidReduceCS(uint3 dispatch_id : SV_DispatchThreadID)
{
    uint source_width, source_height;
    const uint source_offset =
        LevelOffset(PyramidLevel - 1u, source_width, source_height);
    uint destination_width, destination_height;
    const uint destination_offset =
        LevelOffset(PyramidLevel, destination_width, destination_height);
    if (dispatch_id.x >= destination_width || dispatch_id.y >= destination_height) return;

    const uint2 base = dispatch_id.xy * 2u;
    float minimum_depth = 1.0;
    [unroll] for (uint y = 0; y < 2; ++y)
    {
        [unroll] for (uint x = 0; x < 2; ++x)
        {
            const uint2 source = min(base + uint2(x, y),
                                     uint2(source_width - 1u, source_height - 1u));
            minimum_depth = min(minimum_depth,
                CurrentHzb[source_offset + source.y * source_width + source.x]);
        }
    }
    CurrentHzb[destination_offset +
               dispatch_id.y * destination_width + dispatch_id.x] = minimum_depth;
}
