#ifndef HS_GPU_SCENE_TABLES_HLSLI
#define HS_GPU_SCENE_TABLES_HLSLI

// Versioned Renderer-v7 current-scene buffer. One root SRV carries independent
// logical tables so frontends can evolve without widening the root signature.
static const uint HsGpuSceneMagic = 0x37475348u; // "HSG7"
// Version 3 stores current/previous deformation strides in the second skin word.
static const uint HsGpuSceneVersion = 3u;

ByteAddressBuffer HsGpuSceneTables : register(t28, space3);

bool HsGpuSceneSlotValid(ByteAddressBuffer scene, uint slot,
                         out uint4 header0, out uint4 header1,
                         out uint4 header2, out uint4 header3)
{
    header0 = scene.Load4(0);
    header1 = scene.Load4(16);
    header2 = scene.Load4(32);
    header3 = scene.Load4(48);
    return header0.x == HsGpuSceneMagic &&
           header0.y == HsGpuSceneVersion &&
           slot < header0.w;
}

bool HsGpuSceneSlotValid(uint slot, out uint4 header0, out uint4 header1,
                         out uint4 header2, out uint4 header3)
{
    return HsGpuSceneSlotValid(HsGpuSceneTables, slot, header0, header1,
                               header2, header3);
}

InstanceData HsLoadGpuSceneInstance(ByteAddressBuffer scene, uint slot)
{
    uint4 header0, header1, header2, header3;
    if (!HsGpuSceneSlotValid(scene, slot, header0, header1, header2, header3))
        return (InstanceData)0;

    // header1: instance/transform/mesh/material offsets
    // header2: bounds/skin offsets, instance/transform strides
    // header3: mesh/material/bounds/skin strides
    const uint instance_address = header1.x + slot * header2.z;
    const uint4 instance0 = scene.Load4(instance_address);
    const uint transform_address = header1.y + instance0.x * header2.w;
    const uint mesh_address = header1.z + instance0.y * header3.x;
    const uint material_address = header1.w + instance0.z * header3.y;
    const uint bounds_address = header2.x + instance0.w * header3.z;

    const float4 position_yaw =
        asfloat(scene.Load4(transform_address));
    const float4 scale_parameter =
        asfloat(scene.Load4(transform_address + 16));
    const float4 local_bounds =
        asfloat(scene.Load4(bounds_address));

    InstanceData result;
    result.Position = float4(position_yaw.xyz, local_bounds.w);
    result.Scale = float4(scale_parameter.xyz, 1.0);
    result.Color = scene.Load(material_address);
    result.Mesh = scene.Load(mesh_address);
    result.Yaw = position_yaw.w;
    result.Padding = scale_parameter.w;
    return result;
}

InstanceData HsLoadGpuSceneInstance(uint slot)
{
    return HsLoadGpuSceneInstance(HsGpuSceneTables, slot);
}

float4 HsLoadGpuSceneLocalBounds(uint slot)
{
    uint4 header0, header1, header2, header3;
    if (!HsGpuSceneSlotValid(slot, header0, header1, header2, header3))
        return 0.0;
    const uint instance_address = header1.x + slot * header2.z;
    const uint bounds_index = HsGpuSceneTables.Load(instance_address + 12);
    return asfloat(HsGpuSceneTables.Load4(
        header2.x + bounds_index * header3.z));
}

uint4 HsLoadGpuSceneSkin(ByteAddressBuffer scene, uint slot)
{
    uint4 header0, header1, header2, header3;
    if (!HsGpuSceneSlotValid(scene, slot, header0, header1, header2, header3))
        return uint4(0xffffffffu, 0u, 0u, 0u);
    const uint instance_address = header1.x + slot * header2.z;
    const uint skin_index = scene.Load(instance_address + 16);
    if (skin_index == 0xffffffffu)
        return uint4(0xffffffffu, 0u, 0u, 0u);
    return scene.Load4(
        header2.y + skin_index * header3.w);
}

uint4 HsLoadGpuSceneSkin(uint slot)
{
    return HsLoadGpuSceneSkin(HsGpuSceneTables, slot);
}

uint2 HsLoadGpuSceneSkinStrides(ByteAddressBuffer scene, uint slot)
{
    uint4 header0, header1, header2, header3;
    if (!HsGpuSceneSlotValid(scene, slot, header0, header1, header2, header3)) return 0u;
    const uint instance_address = header1.x + slot * header2.z;
    const uint skin_index = scene.Load(instance_address + 16);
    if (skin_index == 0xffffffffu || header3.w != 32u) return 0u;
    return scene.Load2(header2.y + skin_index * header3.w + 16u);
}
uint2 HsLoadGpuSceneSkinStrides(uint slot)
{
    return HsLoadGpuSceneSkinStrides(HsGpuSceneTables, slot);
}

#endif
