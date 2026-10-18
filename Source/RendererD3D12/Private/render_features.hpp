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
    SoftwareGi = 22,
    SoftwareGiComposite = 25,
    Reconstruction = 29,
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
    SoftwareGiVertices,
    SoftwareGiIndices,
    SoftwareGiLightingBacking,
    EnvironmentCube,
    AmbientVisibility,
    ContactVisibility,
    FallbackDiffuse,
    EnvironmentSpecular,
    SubsurfaceDiffuse,
    DiffuseRadiance,
    DiffuseDelta,
    AdjustedSubsurfaceDiffuse,
    SpecularRadiance,
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
    ReconstructionDepth,
    ReconstructionMotion,
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
