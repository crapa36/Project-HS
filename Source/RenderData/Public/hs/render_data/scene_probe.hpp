#pragma once

#include <hs/render_data/texture_package.hpp>

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace hs::render_data
{
// This is a separate SPB1 package, not a new interpretation of environment LPB1.
inline constexpr std::uint32_t kSceneProbeVersion = 1;
inline constexpr std::uint32_t kSceneProbeHeaderStride = 192;
inline constexpr std::uint32_t kSceneProbeRecordStride = 128;
inline constexpr std::uint32_t kSceneProbeMaximumCount = 64;
inline constexpr std::uint32_t kSceneProbeShCount = 9;

struct SceneProbeRevisions
{
    std::uint64_t environment{}, material{}, light{}, static_layout{};
    bool operator==(const SceneProbeRevisions &) const = default;
};

enum class SceneProbeRevisionDomain : std::uint32_t
{ Environment = 1, Material = 2, Light = 3, StaticLayout = 4 };
struct SceneProbeRevisionInput
{
    std::string_view logical_id;
    std::string_view sha256; // Canonical lowercase 64-hex digest of actual input bytes.
};
// Shared producer/Generation validation contract. Inputs are nonempty, sorted
// unique logical IDs and verified SHA256 identities, including their actual
// dependency closure. Hash: ASCII HSSPROBE-REVISION-1, LE uint32 domain, LE
// uint64 count, then LE uint64 ID length, ID bytes and decoded 32-byte digest
// per input. Return the first eight digest bytes as big-endian uint64, OR 1.
// This compact revision is a change identity; full digests remain authoritative.
// Runtime capture additionally qualifies the actual immutable scene/LightView;
// producer source-file hashes cannot be reconstructed from instance data alone.
[[nodiscard]] std::uint64_t DeriveSceneProbeRevision(SceneProbeRevisionDomain domain,
    std::span<const SceneProbeRevisionInput> inputs);

enum class SceneProbePayload : std::uint32_t { Layout = 1, Baked = 2 };
enum class SceneProbeBlend : std::uint32_t { NormalizedInfluenceVisibility = 1 };

struct SceneProbeRecord
{
    std::uint64_t stable_id{};
    Float3 position{};
    float weight{1.0f};
    Float3 influence_min{};
    float influence_fade{1.0f}; // Metres of smooth fade inside the influence boundary.
    Float3 influence_max{};
    float visibility_bias{0.05f}; // Metres beyond the captured first hit before attenuation.
    Float3 projection_min{};
    float visibility_fade{0.25f}; // Metres of smooth attenuation beyond visibility_bias.
    Float3 projection_max{};
    float capture_distance{100.0f}; // Finite miss sentinel and capture far plane in metres.
};

struct SceneProbeDescription
{
    std::uint32_t edge{32}; // 16..128, power of two; D3D +X,-X,+Y,-Y,+Z,-Z.
    std::uint32_t ggx_samples{128}; // 64..2048, power of two.
    SceneProbeBlend blend{SceneProbeBlend::NormalizedInfluenceVisibility};
    SceneProbeRevisions revisions;
    // Indices into the sorted dependency IDs. These must reference the actual
    // environment, material catalog, light definition and static layout inputs.
    std::uint32_t environment_dependency{}, material_dependency{}, light_dependency{}, static_layout_dependency{};
};

// Payload-independent identity of authored bake inputs. It includes only the
// semantic bake description/records and the selected source product identities;
// revision values and payload bytes are excluded so replacing a layout package
// with its baked form cannot feed the output digest back into its own inputs.
[[nodiscard]] std::string DeriveSceneProbeInputIdentity(const SceneProbeDescription &description,
    std::span<const SceneProbeRecord> probes, std::span<const std::string> dependencies,
    std::span<const SceneProbeRevisionInput> source_inputs);

struct SceneProbeBakeData
{
    // probe-major, D3D face-major. GGX is face-major/mip-minor, including mip0.
    // Scene-linear Rec709, alpha=1, no exposure; local cubes include global
    // environment once through sky/miss and static direct/emissive transport.
    std::span<const Float4> radiance, prefiltered, cosine_sh;
    // One nonnegative first-hit world distance per raw texel. Miss equals the
    // corresponding probe's capture_distance. Visibility is not baked into SH.
    std::span<const float> first_hit_distance;
};

struct SceneProbePackageView
{
    SceneProbePayload payload{SceneProbePayload::Layout};
    SceneProbeDescription description;
    std::vector<std::string> dependencies;
    std::vector<SceneProbeRecord> probes;
    TexturePackageView radiance, prefiltered; // RGBA32F cube arrays, 6*probe_count layers.
    std::span<const std::byte> cosine_sh; // Explicit LE float4, probe_count*9 records.
    std::span<const std::byte> first_hit_distance; // Explicit LE float, raw face order.
};

// Empty bake spans produce a strictly layout-only package. A baked package must
// provide all four complete payloads; no replicated environment SH substitute.
// Header/record fields and float payloads are serialized explicitly little-endian.
// Dependencies are canonical sorted unique relative product IDs, all four
// references/revisions are required. Throws std::runtime_error on invalid data.
[[nodiscard]] std::vector<std::byte> BuildSceneProbePackage(AssetId asset_id,
    std::span<const std::string> dependencies, const SceneProbeDescription &description,
    std::span<const SceneProbeRecord> probes, SceneProbeBakeData bake = {});

// Payload byte spans borrow package_bytes. Rejects LPB1, other versions, unsafe
// counts/strides, nonfinite records, invalid bounds, missing dependencies,
// ambiguous layout, nonfinite/negative radiance or invalid distance/SH values.
[[nodiscard]] SceneProbePackageView ReadSceneProbePackage(
    std::span<const std::byte> package_bytes, AssetId expected_asset_id);
} // namespace hs::render_data
