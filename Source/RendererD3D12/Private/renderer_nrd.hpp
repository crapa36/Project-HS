#pragma once

#include "renderer_sdk_frame.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace D3D12MA { class Allocator; }

namespace hs
{
enum class NrdRadianceMethod : std::uint8_t { Off, Reblur, Relax };
enum class NrdSignal : std::uint8_t { Diffuse, Specular, DiffuseSpecular, AmbientOcclusion, Shadow };
// One selected signal per native instance. Independent traces at different
// resolutions must never share a dummy second lobe or native history.
struct NrdSelection
{
    NrdSignal signal{NrdSignal::Diffuse};
    NrdRadianceMethod method{NrdRadianceMethod::Reblur};
};
struct RayDenoiserRequest
{
    GraphicsProviderId provider{"auto"};
    GraphicsAutoToggle enabled{GraphicsAutoToggle::Auto};
};
[[nodiscard]] NrdRadianceMethod ResolveRayDenoiser(const RayDenoiserRequest &) noexcept;
struct RayDenoiserStatus
{
    GraphicsProviderId requested_provider;
    NrdRadianceMethod active_method{NrdRadianceMethod::Off};
    ReconstructionAvailability availability{ReconstructionAvailability::Unavailable};
    std::string reason;
};
[[nodiscard]] RayDenoiserStatus DescribeRayDenoiser(const RayDenoiserRequest &);
// Global library/device admission only; no native instance, pools or history.
[[nodiscard]] RayDenoiserStatus QueryNrdCapability(ID3D12Device *, NrdSelection);
enum class NrdRadiancePacking : std::uint8_t { None, ReblurRadianceAndNormHitDistance, RelaxRadianceAndHitDistance };
struct NrdNativeResource { ResourceVersion version; ID3D12Resource *resource{}; };
struct NrdNativeInputs
{
    // Dedicated NRD producers, never existing GBuffer channels under new names:
    // motion: RG16F, unjittered PreviousUV-CurrentUV, including camera/object motion.
    // normal_roughness: RGB10A2_UNORM from NRD_FrontEnd_PackNormalAndRoughness,
    //   world-space normals, LINEAR roughness, matching library encodings 2/1.
    // view_z: R32F LINEAR view Z in metres, sky beyond denoising_range_m.
    NrdNativeResource motion, normal_roughness, view_z;
    // RGBA16F, demodulated scene-linear radiance WITHOUT exposure. Pack through
    // REBLUR_FrontEnd_PackRadianceAndNormHitDist or RELAX_FrontEnd_PackRadianceAndHitDist.
    // Hit distance excludes the primary hit; skipped paths follow NRD's rules.
    NrdNativeResource diffuse_radiance_hit_distance, specular_radiance_hit_distance;
    // R16F, REBLUR_FrontEnd_GetNormHitDist using this frame's A/B/C and roughness=1.
    // This is normalized ray hit distance, not an already evaluated visibility.
    NrdNativeResource diffuse_normalized_hit_distance;
    // R16F, SIGMA_FrontEnd_PackPenumbra. A scalar hard-shadow visibility cannot substitute.
    NrdNativeResource shadow_penumbra;
    NrdRadiancePacking radiance_packing{NrdRadiancePacking::None};
    bool native_geometry_packed{}, normalized_hit_distance_packed{}, sigma_penumbra_packed{};
    // view/projection and their previous values are UNJITTERED, absolute-world
    // DirectX row-vector matrices. Jitter is supplied separately in pixels.
    SdkCamera camera;
    SdkFrameIdentity identity;
    ViewExtent active;
    std::uint32_t frame_slot{}, first_pass_id{3000};
    CompletionSet completed;
    // Resource COM references alone cannot preserve placed allocation backing.
    // Retain the actual producer allocation/heap owners through this slot's
    // complete Direct/Compute/Copy requirement (bounded by frame_slot_count).
    std::vector<std::shared_ptr<const void>> owners;
};
struct NrdSettings
{
    NrdRadianceMethod radiance{NrdRadianceMethod::Off};
    bool ambient_occlusion{}, shadow{};
    bool probabilistic_lobe_selection{};
    float denoising_range_m{1000.0f}, disocclusion_threshold{0.01f};
    float hit_distance_A{3.0f}, hit_distance_B{0.1f}, hit_distance_C{20.0f};
    std::uint32_t radiance_history_frames{30}, fast_history_frames{6}, shadow_history_frames{5};
    // Direction TO a directional light; zero is appropriate for local lights.
    DirectX::XMFLOAT3 shadow_light_direction{};
};
struct NrdNativeOutput
{
    ResourceVersion version;
    ID3D12Resource *resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
};
struct NrdNativeOutputs
{
    // Decode with REBLUR_BackEnd_UnpackRadianceAndNormHitDist or
    // RELAX_BackEnd_UnpackRadiance. Remodulate materials and exposure afterwards.
    // REBLUR alpha is normalized hit distance; RELAX alpha is native history
    // length, not the producer's input hit distance.
    NrdNativeOutput diffuse, specular;
    // REBLUR normalized diffuse hit distance (AO), and SIGMA_BackEnd_UnpackShadow.
    NrdNativeOutput ambient_occlusion, shadow;
    NrdRadiancePacking radiance_packing{NrdRadiancePacking::None};
    // Internal transport handoff: radiance denoisers may write the borrowed
    // native motion through CLEAR_AND_RESTART/temporal passes. This is the
    // final Binding.version, or the initial input version when read-only.
    ResourceVersion final_motion;
    // NRD does not write pixels beyond denoising_range_m. Composition must use
    // original signals there; these outputs are only the native denoised region.
};
struct NrdStorage;
class NrdCandidate
{
  public:
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Exact DXIL logical IDs requested from the pinned instance's PipelineDesc.
    [[nodiscard]] std::span<const std::string> ShaderProducts() const noexcept;
  private:
    std::shared_ptr<NrdStorage> storage_;
    friend class NrdSystem;
};

// Direct D3D12 integration of NRD v4.17.3, with no NRI dependency. A default Off
// object owns no native DLL, GPU resources, descriptors or PSOs. Prepare is the
// explicit opt-in. Pool descriptions and dispatches come from the actual SDK.
// One view/one registration at a time. Register and native SDK calls run on the
// Prepare owner; dispatch callbacks own copied constants/resources immediately.
class NrdSystem
{
  public:
    using ShaderReader = std::function<Result(std::string_view, std::vector<std::byte> &)>;
    NrdSystem();
    ~NrdSystem();
    NrdSystem(const NrdSystem &) = delete;
    NrdSystem &operator=(const NrdSystem &) = delete;
    [[nodiscard]] Result Prepare(ID3D12Device *, D3D12MA::Allocator *, ViewExtent maximum_extent,
        std::uint32_t frame_slot_count, const ShaderReader &, NrdCandidate &, NrdSelection) const;
    [[nodiscard]] Result Commit(NrdCandidate &&, const CompletionSet &completed);
    [[nodiscard]] Result Register(RenderGraphBuilder &, const NrdNativeInputs &, const NrdSettings &,
                                   NrdNativeOutputs &);
    // Mandatory for every registered graph, including failed/partial submissions.
    // Actual graph resource completion AND all submitted queue requirements own
    // history/pools/descriptors. Failed rendering invalidates the native instance;
    // prepare a fresh candidate after draining, rather than continuing stale state.
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &, const CompletionSet &, bool success);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &completed);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] ReconstructionAvailability Availability() const noexcept;
    [[nodiscard]] std::string_view FailureReason() const noexcept;
    [[nodiscard]] ID3D12Resource *PreparedOutput(NrdSignal) const noexcept;
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
