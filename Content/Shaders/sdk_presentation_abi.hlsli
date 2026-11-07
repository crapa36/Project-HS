#ifndef HS_SDK_PRESENTATION_ABI_HLSLI
#define HS_SDK_PRESENTATION_ABI_HLSLI
#define HS_PRESENT_SPACE 23
#define HS_PRESENT_JOIN_I(a,b) a##b
#define HS_PRESENT_JOIN(a,b) HS_PRESENT_JOIN_I(a,b)

#define HS_PRESENT_FIELDS(X) \
 X(Uint,render_width,RenderWidth) X(Uint,render_height,RenderHeight) \
 X(Uint,output_width,OutputWidth) X(Uint,output_height,OutputHeight) \
 X(Float,paper_white,PaperWhiteNits) X(Float,peak,PeakNits) \
 X(Uint,hdr,Hdr10) X(Uint,motion_contains_sky,MotionContainsSky) \
 X(Matrix,inverse_current_view_projection,InverseCurrentViewProjection) \
 X(Matrix,previous_view_projection,PreviousViewProjection) \
 X(Float4,camera_position,CameraPosition) X(Float2,jitter_uv,JitterUv) \
 X(Uint,reset_history,ResetHistory) X(Uint,transparent_enabled,TransparentEnabled)

// Each entry owns its shader register, descriptor input and graph version.
#define HS_PRESENT_SRVS(X) \
 X(Display,0,float4,SceneDisplayLinear,input.display_linear.version,input.display_linear.resource,DXGI_FORMAT_R16G16B16A16_FLOAT,Color) \
 X(Ui,1,float4,EncodedPremultipliedUi,input.encoded_ui.version,input.encoded_ui.resource,DXGI_FORMAT_B8G8R8A8_UNORM,Color) \
 X(Depth,2,float,DeviceDepth,input.depth.version,input.depth.resource,DXGI_FORMAT_R32_FLOAT,Guide) \
 X(Motion,3,float2,UndilatedMotionUv,input.motion.version,input.motion.resource,DXGI_FORMAT_R16G16_FLOAT,Guide) \
 X(Devtools,4,float4,EncodedDevToolsUi,devtools.version,slot.devtools.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,Color) \
 X(TransparentDepth,5,float,TransparentDepth,input.transparent_depth.version,input.transparent_guides_enabled?input.transparent_depth.resource:nullptr,DXGI_FORMAT_R32_FLOAT,Transparent) \
 X(TransparentMotion,6,float2,TransparentMotionUv,input.transparent_motion.version,input.transparent_guides_enabled?input.transparent_motion.resource:nullptr,DXGI_FORMAT_R16G16_FLOAT,Transparent) \
 X(TransparentClassification,7,float4,TransparentClassification,input.transparent_classification.version,input.transparent_guides_enabled?input.transparent_classification.resource:nullptr,DXGI_FORMAT_R16G16B16A16_FLOAT,Transparent) \
 X(TransparentMetadata,8,uint2,TransparentMetadata,input.transparent_metadata.version,input.transparent_guides_enabled?input.transparent_metadata.resource:nullptr,DXGI_FORMAT_R32G32_UINT,Transparent)
#define HS_PRESENT_UAVS(X) \
 X(Depth,0,float,NativeDepth,depth.version,slot.depth.resource.Get(),DXGI_FORMAT_R32_FLOAT,3) \
 X(Motion,1,float2,NativeMotionUv,motion.version,slot.motion.resource.Get(),DXGI_FORMAT_R16G16_FLOAT,4)
#define HS_PRESENT_COLOR_TARGETS(X) \
 X(hudless,0) X(ui,1) X(composite,2)
#endif
