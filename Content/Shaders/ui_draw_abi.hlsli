#ifndef HS_UI_DRAW_ABI_HLSLI
#define HS_UI_DRAW_ABI_HLSLI

#ifdef HS_UI_DRAW_NATIVE
#define HS_UI_RECT D2D1_RECT_F
#define HS_UI_UINT std::uint32_t
#else
#define HS_UI_RECT float4
#define HS_UI_UINT uint
#endif

struct UiQuad
{
    HS_UI_RECT bounds;
    HS_UI_RECT clipRect;
    HS_UI_UINT color;
    HS_UI_UINT glyphOffset;
    HS_UI_UINT glyphStride;
    float radius;
};
#undef HS_UI_RECT
#undef HS_UI_UINT

// Declaration order is the native root index. Register indices are per shader register class.
#define HS_UI_BINDINGS(SRV, CONSTANTS) \
    SRV(Quads, StructuredBuffer<UiQuad>, quads, 0, VERTEX) \
    SRV(Atlas, ByteAddressBuffer, coverageAtlas, 1, PIXEL) \
    CONSTANTS(Extent, outputExtent, 2, 0, VERTEX)

#ifdef HS_UI_DRAW_NATIVE
enum UiDrawRoot : UINT
{
#define HS_UI_ROOT_INDEX(name, ...) UiRoot##name,
    HS_UI_BINDINGS(HS_UI_ROOT_INDEX, HS_UI_ROOT_INDEX)
#undef HS_UI_ROOT_INDEX
};
inline constexpr D3D12_ROOT_PARAMETER kUiDrawParameters[]{
#define HS_UI_SRV(name, type, var, reg, stage) \
    {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV, .Descriptor = {reg, 0}, .ShaderVisibility = D3D12_SHADER_VISIBILITY_##stage},
#define HS_UI_CONSTANTS(name, var, width, reg, stage) \
    {.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = {reg, 0, width}, .ShaderVisibility = D3D12_SHADER_VISIBILITY_##stage},
    HS_UI_BINDINGS(HS_UI_SRV, HS_UI_CONSTANTS)
};
static_assert(sizeof(UiQuad) == 48);
#else
#define HS_UI_SRV(name, type, var, reg, stage) type var : register(t##reg);
#define HS_UI_CONSTANTS(name, var, width, reg, stage) cbuffer Ui##name : register(b##reg) { float##width var; }
HS_UI_BINDINGS(HS_UI_SRV, HS_UI_CONSTANTS)
#endif
#undef HS_UI_SRV
#undef HS_UI_CONSTANTS
#undef HS_UI_BINDINGS
#endif
