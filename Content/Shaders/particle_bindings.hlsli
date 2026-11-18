struct ParticleData
{
    float4 PositionLife;
    float4 InitialPositionSpawnTime;
    float4 InitialVelocityMaxLife;
    float4 StartColor;
    float4 EndColor;
    float4 SizeRotation;
    float4 PhysicsMetadata;
    float4 CurrentVelocityDrag;
    float4 CollisionMaterial;
    uint4 Authored;
};

struct ParticleSpawnCommandGpu
{
    float4 PositionLifetimeMin;
    float4 DirectionLifetimeMax;
    float4 ShapeExtentSpeedMin;
    float4 SpeedConeGravityStretch;
    float4 StartColor;
    float4 EndColor;
    float4 SizeRange;
    float4 RotationRange;
    uint4 Modes;
    uint4 Metadata;
    float4 CurrentVelocityDrag;
    float4 CollisionMaterial;
    uint4 Authored;
};

StructuredBuffer<ParticleData> Particles : register(t1);
ByteAddressBuffer VfxMeshAtlas : register(t0, space3);
StructuredBuffer<ParticleSpawnCommandGpu> ParticleSpawnCommands : register(t12);
