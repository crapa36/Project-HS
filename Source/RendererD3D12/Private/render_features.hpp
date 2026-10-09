#pragma once

#include "render_graph.hpp"

#include <functional>
#include <span>
#include <vector>

namespace hs
{
enum class FeatureId : std::uint32_t
{
    Imported = 0,
    Temporal = 9,
    Bloom = 10,
    ToneMap = 14,
    Outline = 15,
    Fxaa = 16,
    Ui = 17,
    AmbientOcclusion = 19,
    RayAmbientOcclusion = 20,
    RayShadows = 21,
    SoftwareGi = 22,
    DiffuseTransport = 23,
    DiffuseDenoising = 24,
    SoftwareGiComposite = 25,
    SpecularTransport = 26,
    SpecularDenoising = 27,
    RayTransportComposite = 28,
    Reconstruction = 29,
    RayReconstructionGuides = 30,
    FusedRayReconstruction = 31,
};

enum class SignalSemantic : std::uint8_t
{
    SceneHdr,
    CompositeHdr,
    Depth,
    Normal,
    SurfaceMetadata,
    Motion,
    SurfaceHistory,
    TemporalHistory,
    TemporalNormalHistory,
    TemporalMaterialHistory,
    UiColor,
    TemporalHdr,
    BloomHdr,
    ToneMappedColor,
    OutlinedColor,
    BackBufferColor,
    AntialiasedColor,
    BaseColor,
    Material,
    MaterialSidecar,
    ClosureBacking,
    RayScene,
    RaySceneMetadata,
    RayGeometryPositions,
    RayGeometryRecords,
    RayGeometryInstances,
    SoftwareGiVertices,
    SoftwareGiIndices,
    SoftwareGiLightingBacking,
    EnvironmentCube,
    AmbientVisibility,
    ContactVisibility,
    RayShadowVisibility,
    RayShadowConfidence,
    RayShadowDepth,
    RayShadowNormal,
    RayShadowMetadata,
    RayShadowRecords,
    FallbackDiffuse,
    EnvironmentSpecular,
    SubsurfaceDiffuse,
    DiffuseRadiance,
    DiffuseDelta,
    AdjustedSubsurfaceDiffuse,
    SpecularRadiance,
    NoisyDiffuseRadiance,
    NoisySpecularRadiance,
    DiffuseStatistics,
    SpecularStatistics,
    DiffuseHitDistance,
    SpecularHitDistance,
    OpaqueHdr,
    Exposure,
    OitRevealage,
    TransparentDepth,
    TransparentMotion,
    TransparentHistory,
    TransparentClassification,
    TransparentMetadata,
    TransparentOverflow,
    TransparentTransport,
    ScreenOverlayAlpha,
    DiffuseAlbedo,
    SpecularAlbedo,
    NormalRoughness,
    ReconstructionDepth,
    ReconstructionMotion,
    ReconstructionExposure,
    ResponsiveMask,
    ReconstructedSceneHdr,
    AdaptiveDensity,
    BlueNoise,
};

enum class SignalEncoding : std::uint8_t
{
    RgbaFloat,
    Depth,
    Normal,
    Metadata,
    Motion,
    SurfaceHistory,
    RgbaUnorm,
    Float,
    Float2,
    RawBytes,
    AccelerationStructure,
};

enum class RadiometricMeaning : std::uint8_t
{
    None,
    SceneLinear,
    PreExposedSceneLinear,
    DisplayLinear,
    DisplayEncoded,
};

struct SignalExtent
{
    std::uint32_t width{};
    std::uint32_t height{};
    bool operator==(const SignalExtent &) const = default;
};

struct SignalKey
{
    SignalSemantic semantic{};
    std::uint32_t view{};
    std::uint32_t version{};
    bool operator==(const SignalKey &) const = default;
};

enum class SignalFiltering : std::uint8_t { Unspecified, Noisy, Resolved };

struct SignalMetadata
{
    ResourceKind kind{ResourceKind::Texture};
    SignalExtent extent;
    SignalEncoding encoding{};
    RadiometricMeaning radiometric{};
    std::uint32_t layers{1};
    SignalFiltering filtering{SignalFiltering::Unspecified};
    bool operator==(const SignalMetadata &) const = default;
};

enum class SignalUsage : std::uint8_t { Input, Guide, History };

struct SignalDeclaration
{
    SignalKey key;
    SignalMetadata metadata;
    // Required guides/history are ordinary, versioned dependencies. Build
    // receives this usage with the resolved producer handle; native services
    // retain their own completed-frame history/reset ownership.
    SignalUsage usage{SignalUsage::Input};
};

struct SignalHandle
{
    SignalDeclaration declaration;
    FeatureId producer{FeatureId::Imported};
    ResourceVersion resource;
};

// Registration is concrete data plus a build function. Metadata and dependencies
// are checked for all enabled features before any build function is invoked.
struct FeatureRegistration
{
    FeatureId id{};
    std::function<bool()> predicate;
    std::vector<SignalDeclaration> inputs;
    std::vector<SignalDeclaration> outputs;
    std::function<Result(std::span<const SignalHandle>, std::vector<SignalHandle> &)> build;
};

struct FeatureComposition
{
    std::vector<FeatureId> order;
    std::vector<SignalHandle> signals;
};

[[nodiscard]] Result ComposeRenderFeatures(std::span<const SignalHandle> imported,
                                          std::span<const FeatureRegistration> registrations,
                                          FeatureComposition &composition);
} // namespace hs
