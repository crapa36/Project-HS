#ifndef HS_LIGHT_LIST_ABI_HLSLI
#define HS_LIGHT_LIST_ABI_HLSLI

#define HS_LIGHT_LIST_RECORD_BYTES 96u
#define HS_LIGHT_LIST_HEADER_BYTES 256u
#define HS_LIGHT_LIST_MASK_WORDS 4u
#define HS_LIGHT_LIST_MASK_BYTES 16u
#define HS_LIGHT_LIST_TILE_PIXELS 16u
#define HS_LIGHT_LIST_DEPTH_SLICES 24u
#define HS_LIGHT_LIST_MAX_LIGHTS 128u

// Kind, native name, shader name, byte offset.
#define HS_LIGHT_LIST_RECORD_FIELDS(X) \
 X(Float4,position_range,position_range,0u) \
 X(Float4,direction_cos_outer,direction_cos_outer,16u) \
 X(Float4,color_intensity,color_intensity,32u) \
 X(Float4,shape_cone,shape_cone,48u) \
 X(Uint4,identity,identity,64u) \
 X(Uint4,metadata,metadata,80u)

#define HS_LIGHT_LIST_CONSTANT_FIELDS(X) \
 X(Matrix,world_to_view,WorldToView,0u) \
 X(Matrix,projection,Projection,64u) \
 X(Matrix,inverse_projection,InverseProjection,128u) \
 X(Float4,camera_origin_near,CameraOriginNear,192u) \
 X(Float4,extent_far_log,ExtentFarLog,208u) \
 X(Uint4,grid,Grid,224u) \
 X(Uint4,limits,Limits,240u)

#endif
